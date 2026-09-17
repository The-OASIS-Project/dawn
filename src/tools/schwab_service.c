/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s).
 *
 * Charles Schwab service layer — quotes + portfolio orchestration/formatting.
 * Talks to schwab_client for HTTP and oauth_client for the bearer token; formats
 * compact, LLM-facing result strings. Account numbers are masked to the last 4
 * before they ever leave this layer.
 */

#include "tools/schwab_service.h"

#include <json-c/json.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/strbuf.h"
#include "logging.h"
#include "tools/oauth_client.h"
#include "tools/schwab_client.h"
#include "tools/tool_registry.h" /* TOOL_RESULT_ERROR_MARK */

#define SCHWAB_MAX_SYMBOLS 25

/* ===== small helpers ===== */

static char *result_err(const char *msg) {
   size_t n = strlen(msg) + 2; /* mark (1) + NUL (1) */
   char *s = malloc(n);
   if (s) {
      snprintf(s, n, "%s%s", TOOL_RESULT_ERROR_MARK, msg);
   }
   return s;
}

static char *rc_to_err(schwab_rc_t rc) {
   switch (rc) {
      case SCHWAB_RC_RATE_LIMITED:
         return result_err("Schwab rate limit reached — try again in a minute.");
      case SCHWAB_RC_AUTH:
         return result_err(
             "Schwab authorization failed. Re-link by running 'dawn-admin schwab auth'.");
      case SCHWAB_RC_HTTP:
         return result_err("Couldn't reach Schwab (network or service error).");
      case SCHWAB_RC_NOT_LINKED:
      case SCHWAB_RC_ERROR:
      case SCHWAB_RC_OK:
         break;
   }
   return result_err("Schwab request failed.");
}

static double jget_d(struct json_object *o, const char *k) {
   struct json_object *v = NULL;
   return (o && json_object_object_get_ex(o, k, &v)) ? json_object_get_double(v) : 0.0;
}
static const char *jget_s(struct json_object *o, const char *k) {
   struct json_object *v = NULL;
   return (o && json_object_object_get_ex(o, k, &v)) ? json_object_get_string(v) : "";
}
static bool jget_b(struct json_object *o, const char *k, bool def) {
   struct json_object *v = NULL;
   return (o && json_object_object_get_ex(o, k, &v)) ? json_object_get_boolean(v) : def;
}

static void fmt_volume(double v, char *b, size_t n) {
   if (v >= 1e9) {
      snprintf(b, n, "%.1fB", v / 1e9);
   } else if (v >= 1e6) {
      snprintf(b, n, "%.1fM", v / 1e6);
   } else if (v >= 1e3) {
      snprintf(b, n, "%.0fK", v / 1e3);
   } else {
      snprintf(b, n, "%.0f", v);
   }
}

/* Mask an account number to its last 4 digits. */
static void mask_account(const char *acct, char *out, size_t out_len) {
   size_t len = acct ? strlen(acct) : 0;
   if (len >= 4) {
      snprintf(out, out_len, "\xe2\x80\xa6%s", acct + (len - 4)); /* …1234 */
   } else {
      snprintf(out, out_len, "\xe2\x80\xa6");
   }
}

/* Validate/uppercase symbols into a URL-safe CSV (charset [A-Z0-9.]). Returns the
 * count accepted; *bad_count receives the number rejected. */
static int clean_symbols(const char *in, char *out, size_t out_len, int *bad_count) {
   int n = 0, bad = 0;
   size_t olen = 0;
   out[0] = '\0';
   const char *p = in;
   while (*p && n < SCHWAB_MAX_SYMBOLS) {
      while (*p == ',' || *p == ' ' || *p == '\t') {
         p++;
      }
      if (!*p) {
         break;
      }
      char tok[16];
      size_t t = 0;
      bool ok = true;
      while (*p && *p != ',' && *p != ' ' && *p != '\t') {
         char c = *p++;
         if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
         }
         if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.')) {
            ok = false;
         }
         if (t < sizeof(tok) - 1) {
            tok[t++] = c;
         } else {
            ok = false; /* over-length ticker → reject, don't silently truncate */
         }
      }
      tok[t] = '\0';
      if (t == 0) {
         continue;
      }
      if (ok) {
         int w = snprintf(out + olen, out_len - olen, "%s%s", olen ? "," : "", tok);
         if (w > 0 && (size_t)w < out_len - olen) {
            olen += (size_t)w;
            n++;
         }
      } else {
         bad++;
      }
   }
   if (bad_count) {
      *bad_count = bad;
   }
   return n;
}

/* ===== token acquisition ===== */

/* Fetch a bearer for the user, distinguishing not-configured / not-linked /
 * expired. On non-OK returns an error-marked message in *err_out. */
static schwab_rc_t schwab_bearer(int user_id,
                                 oauth_provider_config_t *prov,
                                 char *bearer,
                                 size_t blen,
                                 char **err_out) {
   *err_out = NULL;
   if (oauth_build_schwab_provider(prov) != 0) {
      *err_out = result_err("Stocks isn't configured. Set [secrets.schwab] client_id and "
                            "client_secret in secrets.toml.");
      return SCHWAB_RC_ERROR;
   }
   if (!oauth_has_tokens(user_id, "schwab", SCHWAB_ACCOUNT_KEY)) {
      *err_out = result_err("Your Schwab account isn't linked yet. Link it by running "
                            "'dawn-admin schwab auth' on the DAWN host.");
      return SCHWAB_RC_NOT_LINKED;
   }
   if (oauth_get_access_token(prov, user_id, SCHWAB_ACCOUNT_KEY, bearer, blen) != 0) {
      if (oauth_was_last_refresh_revoked(NULL, 0)) {
         *err_out = result_err("Your Schwab link expired (Schwab requires re-linking about "
                               "weekly). Re-link by running 'dawn-admin schwab auth'.");
      } else {
         *err_out = result_err("Couldn't authorize with Schwab. Try again shortly.");
      }
      return SCHWAB_RC_AUTH;
   }
   return SCHWAB_RC_OK;
}

/* Force a token refresh regardless of the expiry margin (for a mid-window 401). */
static schwab_rc_t schwab_force_refresh(oauth_provider_config_t *prov,
                                        int user_id,
                                        char *bearer,
                                        size_t blen) {
   oauth_token_set_t t;
   if (oauth_load_tokens(user_id, "schwab", SCHWAB_ACCOUNT_KEY, &t) != 0) {
      return SCHWAB_RC_AUTH;
   }
   if (oauth_refresh(prov, &t) != 0) {
      sodium_memzero(&t, sizeof(t));
      return SCHWAB_RC_AUTH;
   }
   oauth_store_tokens(user_id, "schwab", SCHWAB_ACCOUNT_KEY, &t);
   snprintf(bearer, blen, "%s", t.access_token);
   sodium_memzero(&t, sizeof(t));
   return SCHWAB_RC_OK;
}

/* GET with one forced-refresh retry on a mid-window 401. */
static schwab_rc_t schwab_get_with_retry(oauth_provider_config_t *prov,
                                         int user_id,
                                         const char *url,
                                         char *bearer,
                                         size_t blen,
                                         struct json_object **root) {
   long http = 0;
   schwab_rc_t rc = schwab_client_get_json(bearer, url, root, &http);
   if (rc == SCHWAB_RC_AUTH) {
      if (schwab_force_refresh(prov, user_id, bearer, blen) == SCHWAB_RC_OK) {
         rc = schwab_client_get_json(bearer, url, root, &http);
      }
   }
   return rc;
}

/* ===== quotes ===== */

static void fmt_quote_symbol(strbuf_t *sb, const char *sym, struct json_object *symobj) {
   struct json_object *q = NULL;
   if (!json_object_object_get_ex(symobj, "quote", &q)) {
      strbuf_appendf(sb, "%s: no quote data\n", sym);
      return;
   }
   double last = jget_d(q, "lastPrice");
   double close = jget_d(q, "closePrice");
   double netchg = jget_d(q, "netChange");
   double pct = jget_d(q, "netPercentChange");
   if (netchg == 0.0 && close > 0.0) {
      netchg = last - close;
   }
   if (pct == 0.0 && close > 0.0) {
      pct = (last - close) / close * 100.0;
   }
   char volb[24];
   fmt_volume(jget_d(q, "totalVolume"), volb, sizeof(volb));
   bool realtime = jget_b(symobj, "realtime", true);
   strbuf_appendf(sb,
                  "%s: $%.2f  %+.2f (%+.2f%%)  bid %.2f / ask %.2f  vol %s  "
                  "day %.2f-%.2f  52wk %.2f-%.2f%s\n",
                  sym, last, netchg, pct, jget_d(q, "bidPrice"), jget_d(q, "askPrice"), volb,
                  jget_d(q, "lowPrice"), jget_d(q, "highPrice"), jget_d(q, "52WeekLow"),
                  jget_d(q, "52WeekHigh"), realtime ? "" : "  [delayed]");
}

char *schwab_service_quote(int user_id, const char *symbols_csv) {
   if (!symbols_csv || !symbols_csv[0]) {
      return result_err("No stock symbols were given.");
   }
   char clean[512];
   int bad = 0;
   int nsym = clean_symbols(symbols_csv, clean, sizeof(clean), &bad);
   if (nsym == 0) {
      return result_err("No valid stock symbols were given (use e.g. NVDA, AMD).");
   }

   oauth_provider_config_t prov;
   char bearer[OAUTH_TOKEN_BUF_SIZE];
   char *err = NULL;
   if (schwab_bearer(user_id, &prov, bearer, sizeof(bearer), &err) != SCHWAB_RC_OK) {
      sodium_memzero(&prov, sizeof(prov));
      return err;
   }

   char url[1024];
   snprintf(url, sizeof(url), "%s/quotes?symbols=%s&fields=quote&indicative=false",
            SCHWAB_MARKETDATA_BASE, clean);

   struct json_object *root = NULL;
   schwab_rc_t rc = schwab_get_with_retry(&prov, user_id, url, bearer, sizeof(bearer), &root);
   sodium_memzero(bearer, sizeof(bearer));
   sodium_memzero(&prov, sizeof(prov));
   if (rc != SCHWAB_RC_OK) {
      return rc_to_err(rc);
   }

   /* A quote response is an object keyed by symbol; json_object_object_foreach on
    * a non-object (e.g. an unexpected [] or scalar 200 body) would NULL-deref. */
   if (!json_object_is_type(root, json_type_object)) {
      json_object_put(root);
      return result_err("Unexpected quote response from Schwab.");
   }

   strbuf_t sb;
   strbuf_init(&sb, 512);

   struct json_object *errors = NULL;
   json_object_object_foreach(root, key, val) {
      if (strcmp(key, "errors") == 0) {
         errors = val;
         continue;
      }
      fmt_quote_symbol(&sb, key, val);
   }

   if (errors) {
      struct json_object *invalid = NULL;
      if (json_object_object_get_ex(errors, "invalidSymbols", &invalid) &&
          json_object_is_type(invalid, json_type_array)) {
         size_t ni = json_object_array_length(invalid);
         if (ni > 0) {
            strbuf_append(&sb, "Not found: ");
            for (size_t i = 0; i < ni; i++) {
               strbuf_appendf(&sb, "%s%s", i ? ", " : "",
                              json_object_get_string(json_object_array_get_idx(invalid, i)));
            }
            strbuf_append(&sb, "\n");
         }
      }
   }
   if (bad > 0) {
      strbuf_appendf(&sb, "(%d entr%s were not valid symbols and were skipped.)\n", bad,
                     bad == 1 ? "y" : "ies");
   }
   if (strbuf_len(&sb) == 0) {
      strbuf_append(&sb, "No quote data returned.\n");
   }

   char *out = (!strbuf_oom(&sb) && sb.buf) ? strdup(sb.buf) : NULL;
   strbuf_free(&sb);
   json_object_put(root);
   return out ? out : result_err("Schwab quote formatting failed.");
}

/* ===== portfolio ===== */

static void fmt_position(strbuf_t *sb, struct json_object *pos) {
   struct json_object *inst = NULL;
   json_object_object_get_ex(pos, "instrument", &inst);
   const char *sym = jget_s(inst, "symbol");
   const char *atype = jget_s(inst, "assetType");
   double qty = jget_d(pos, "longQuantity");
   double sqty = jget_d(pos, "shortQuantity");
   double avg = jget_d(pos, "averagePrice");
   double mv = jget_d(pos, "marketValue");
   double dpl = jget_d(pos, "currentDayProfitLoss");
   double tpl = jget_d(pos, "longOpenProfitLoss");

   strbuf_appendf(sb, "  %s: %.4g sh @ $%.2f avg, value $%.2f, day P/L %+.2f, total P/L %+.2f",
                  (sym && sym[0]) ? sym : "?", qty, avg, mv, dpl, tpl);
   if (sqty > 0.0) {
      strbuf_appendf(sb, " (short %.4g)", sqty);
   }
   if (atype && atype[0] && strcmp(atype, "EQUITY") != 0) {
      const char *desc = jget_s(inst, "description");
      strbuf_appendf(sb, " [%s%s%s]", atype, (desc && desc[0]) ? ": " : "", desc ? desc : "");
   }
   strbuf_append(sb, "\n");
}

char *schwab_service_portfolio(int user_id, bool accounts_only) {
   oauth_provider_config_t prov;
   char bearer[OAUTH_TOKEN_BUF_SIZE];
   char *err = NULL;
   if (schwab_bearer(user_id, &prov, bearer, sizeof(bearer), &err) != SCHWAB_RC_OK) {
      sodium_memzero(&prov, sizeof(prov));
      return err;
   }

   char url[512];
   snprintf(url, sizeof(url), "%s/accounts?fields=positions", SCHWAB_TRADER_BASE);

   struct json_object *root = NULL;
   schwab_rc_t rc = schwab_get_with_retry(&prov, user_id, url, bearer, sizeof(bearer), &root);
   sodium_memzero(bearer, sizeof(bearer));
   sodium_memzero(&prov, sizeof(prov));
   if (rc != SCHWAB_RC_OK) {
      return rc_to_err(rc);
   }

   if (!json_object_is_type(root, json_type_array)) {
      json_object_put(root);
      return result_err("Unexpected portfolio response from Schwab.");
   }

   strbuf_t sb;
   strbuf_init(&sb, 1024);
   size_t nacc = json_object_array_length(root);
   double total_liq = 0.0;
   int shown = 0;

   for (size_t i = 0; i < nacc; i++) {
      struct json_object *el = json_object_array_get_idx(root, i);
      struct json_object *sa = NULL;
      if (!json_object_object_get_ex(el, "securitiesAccount", &sa)) {
         continue;
      }
      const char *type = jget_s(sa, "type");
      char masked[24];
      mask_account(jget_s(sa, "accountNumber"), masked, sizeof(masked));

      struct json_object *bal = NULL;
      json_object_object_get_ex(sa, "currentBalances", &bal);
      double liq = jget_d(bal, "liquidationValue");
      double cash = jget_d(bal, "cashBalance");
      total_liq += liq;

      strbuf_appendf(&sb, "Account %s (%s): value $%.2f, cash $%.2f", masked,
                     (type && type[0]) ? type : "?", liq, cash);
      if (type && strcmp(type, "MARGIN") == 0) {
         strbuf_appendf(&sb, ", buying power $%.2f", jget_d(bal, "buyingPower"));
      } else {
         strbuf_appendf(&sb, ", available $%.2f", jget_d(bal, "cashAvailableForTrading"));
      }
      strbuf_append(&sb, "\n");

      if (!accounts_only) {
         struct json_object *positions = NULL;
         if (json_object_object_get_ex(sa, "positions", &positions) &&
             json_object_is_type(positions, json_type_array) &&
             json_object_array_length(positions) > 0) {
            size_t np = json_object_array_length(positions);
            for (size_t j = 0; j < np; j++) {
               fmt_position(&sb, json_object_array_get_idx(positions, j));
            }
         } else {
            strbuf_append(&sb, "  (no open positions)\n");
         }
      }
      shown++;
   }

   json_object_put(root);

   if (shown == 0) {
      strbuf_free(&sb);
      return result_err("No Schwab accounts were found on your link.");
   }
   strbuf_appendf(&sb, "Total value: $%.2f across %d account%s.\n", total_liq, shown,
                  shown == 1 ? "" : "s");

   char *out = (!strbuf_oom(&sb) && sb.buf) ? strdup(sb.buf) : NULL;
   strbuf_free(&sb);
   return out ? out : result_err("Schwab portfolio formatting failed.");
}
