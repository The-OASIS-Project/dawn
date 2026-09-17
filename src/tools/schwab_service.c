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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/iso8601.h" /* iso8601_parse_date_utc */
#include "core/strbuf.h"
#include "logging.h"
#include "tools/oauth_client.h"
#include "tools/schwab_client.h"
#include "tools/schwab_txn.h"    /* transaction classifier + aggregates */
#include "tools/tool_registry.h" /* TOOL_RESULT_ERROR_MARK */

#define SCHWAB_MAX_SYMBOLS 25
/* Upper bound on candles we allocate for, guarding against a pathological
 * response — legitimate price history is well under this (5y daily ~1,300). */
#define SCHWAB_MAX_CANDLES 20000

/* Transactions: Schwab serves only a recent window. schwab-py's 60-day figure is
 * TD-Ameritrade heritage; schwabr + a live-account PR report ~365 days — SPIKE
 * pins the exact edge (365/366/400d probe). Default to a short recent window. */
#define SCHWAB_TXN_DEFAULT_DAYS 60
#define SCHWAB_TXN_MAX_LOOKBACK_DAYS 365
#define SCHWAB_TXN_MAX_ACCOUNTS 16
/* Aggregate over every returned row, but bound the parse allocation and the
 * per-account listing (a voice user wants recent activity, not a wall of rows). */
#define SCHWAB_TXN_MAX_ROWS 4000
#define SCHWAB_TXN_LIST_CAP 50
/* The full transaction-type set, comma-joined for the `types=` query. schwab-py
 * always sends all types joined and it works; `startDate`/`endDate`/`types` are
 * effectively required, so never rely on "omit → all". */
#define SCHWAB_TXN_ALL_TYPES                                                                   \
   "TRADE,RECEIVE_AND_DELIVER,DIVIDEND_OR_INTEREST,ACH_RECEIPT,ACH_DISBURSEMENT,CASH_RECEIPT," \
   "CASH_DISBURSEMENT,ELECTRONIC_FUND,WIRE_OUT,WIRE_IN,JOURNAL,MEMORANDUM,MARGIN_CALL,"        \
   "MONEY_MARKET,SMA_ADJUSTMENT"

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
      case SCHWAB_RC_TOO_LARGE:
         return result_err("That request returned too much data to handle — narrow the date range "
                           "and try again.");
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

/* GET with one forced-refresh retry on a mid-window 401. @p http_out (optional)
 * receives the final HTTP status so callers can distinguish 400/404 from a
 * transport error. */
static schwab_rc_t schwab_get_with_retry(oauth_provider_config_t *prov,
                                         int user_id,
                                         const char *url,
                                         char *bearer,
                                         size_t blen,
                                         struct json_object **root,
                                         long *http_out) {
   long http = 0;
   schwab_rc_t rc = schwab_client_get_json(bearer, url, root, &http);
   if (rc == SCHWAB_RC_AUTH) {
      if (schwab_force_refresh(prov, user_id, bearer, blen) == SCHWAB_RC_OK) {
         rc = schwab_client_get_json(bearer, url, root, &http);
      }
   }
   if (http_out) {
      *http_out = http;
   }
   return rc;
}

/* Map a failed request to an error-marked message, using the HTTP status to
 * turn a 400/404 (bad symbol/params) into something more useful than a generic
 * transport error. */
static char *schwab_req_err(schwab_rc_t rc, long http) {
   if (rc == SCHWAB_RC_HTTP && (http == 400 || http == 404)) {
      return result_err(
          "Schwab rejected that request — check the symbol, and any dates or range/interval.");
   }
   return rc_to_err(rc);
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
   schwab_rc_t rc = schwab_get_with_retry(&prov, user_id, url, bearer, sizeof(bearer), &root, NULL);
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
   schwab_rc_t rc = schwab_get_with_retry(&prov, user_id, url, bearer, sizeof(bearer), &root, NULL);
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
   double total_pl = 0.0;
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
            double acct_mv = 0.0, acct_pl = 0.0;
            for (size_t j = 0; j < np; j++) {
               struct json_object *pos = json_object_array_get_idx(positions, j);
               fmt_position(&sb, pos);
               acct_mv += jget_d(pos, "marketValue");
               acct_pl += jget_d(pos, "longOpenProfitLoss");
            }
            /* Account-level unrealized P/L; % is against cost basis (market value
             * minus the open gain), which the position rows already carry. */
            double cost = acct_mv - acct_pl;
            if (cost > 0.0) {
               strbuf_appendf(&sb, "  Unrealized P/L (long positions) %+.2f (%+.2f%%)\n", acct_pl,
                              acct_pl / cost * 100.0);
            } else {
               strbuf_appendf(&sb, "  Unrealized P/L (long positions) %+.2f\n", acct_pl);
            }
            total_pl += acct_pl;
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
   if (!accounts_only) {
      strbuf_appendf(&sb, "Total unrealized P/L (long positions): %+.2f.\n", total_pl);
   }

   char *out = (!strbuf_oom(&sb) && sb.buf) ? strdup(sb.buf) : NULL;
   strbuf_free(&sb);
   return out ? out : result_err("Schwab portfolio formatting failed.");
}

/* ===== price history + analytics ===== */

static int64_t jget_i64(struct json_object *o, const char *k) {
   struct json_object *v = NULL;
   return (o && json_object_object_get_ex(o, k, &v)) ? json_object_get_int64(v) : 0;
}

/* Schwab candle datetime is epoch ms; daily candles are stamped US-midnight, so
 * the UTC calendar date equals the trading date — gmtime_r is correct and
 * thread-safe (localtime_r/TZ would be wrong on non-Eastern hosts). */
static void fmt_date(int64_t epoch_ms, char *out, size_t n) {
   time_t s = (time_t)(epoch_ms / 1000);
   struct tm tmv;
   /* gmtime_r can return NULL for an out-of-range time_t, and strftime returns 0
    * (buffer contents unspecified) if the result doesn't fit — guard both so a
    * pathological server datetime can't leave `out` unterminated. */
   if (!gmtime_r(&s, &tmv) || strftime(out, n, "%Y-%m-%d", &tmv) == 0) {
      snprintf(out, n, "?");
   }
}

static const char *freq_name(schwab_freq_t f) {
   return f == SCHWAB_FREQ_WEEKLY ? "weekly" : f == SCHWAB_FREQ_MONTHLY ? "monthly" : "daily";
}
static unsigned freq_bit(schwab_freq_t f) {
   return 1u << (unsigned)f; /* daily=1, weekly=2, monthly=4 */
}

/* Range → Schwab pricehistory params + the set of legal intervals. */
typedef struct {
   const char *range;
   const char *period_type;
   int period;
   unsigned legal;             /* OR of freq_bit() */
   schwab_freq_t def_interval; /* default when none requested */
} hist_range_t;

static const hist_range_t HIST_RANGES[] = {
   { "1mo", "month", 1, 1u | 2u, SCHWAB_FREQ_DAILY },
   { "3mo", "month", 3, 1u | 2u, SCHWAB_FREQ_DAILY },
   { "6mo", "month", 6, 1u | 2u, SCHWAB_FREQ_DAILY },
   { "1y", "year", 1, 1u | 2u | 4u, SCHWAB_FREQ_DAILY },
   { "5y", "year", 5, 1u | 2u | 4u, SCHWAB_FREQ_WEEKLY },
   { "ytd", "ytd", 1, 1u | 2u, SCHWAB_FREQ_DAILY },
};

/* Strict YYYY-MM-DD check. Stricter than strptime (which tolerates trailing junk
 * and single-digit fields) AND rejects semantically-invalid dates like
 * 2025-02-30 / 2025-13-45 that timegm would silently normalize — by requiring an
 * exact parse→reformat round-trip. Local so the stocks tool does not depend on
 * the email tool being compiled in. */
static bool schwab_valid_date(const char *s) {
   if (!s || strlen(s) != 10 || s[4] != '-' || s[7] != '-') {
      return false;
   }
   for (int i = 0; i < 10; i++) {
      if (i == 4 || i == 7) {
         continue;
      }
      if (s[i] < '0' || s[i] > '9') {
         return false;
      }
   }
   time_t t = (time_t)iso8601_parse_date_utc(s);
   struct tm tmv;
   char rt[16];
   if (!gmtime_r(&t, &tmv) || strftime(rt, sizeof(rt), "%Y-%m-%d", &tmv) == 0) {
      return false;
   }
   return strcmp(rt, s) == 0;
}

char *schwab_service_history(int user_id,
                             const char *symbol,
                             const char *range,
                             const char *interval,
                             const char *data,
                             const char *start,
                             const char *end) {
   /* symbol: history is single — take the first valid token. */
   char clean[32];
   if (clean_symbols(symbol ? symbol : "", clean, sizeof(clean), NULL) == 0) {
      return result_err("No valid stock symbol was given (e.g. NVDA).");
   }
   char *comma = strchr(clean, ',');
   if (comma) {
      *comma = '\0';
   }

   /* data mode: 0 summary, 1 series, 2 raw (all include the summary) */
   int mode = 0;
   if (data && data[0]) {
      if (strcmp(data, "series") == 0) {
         mode = 1;
      } else if (strcmp(data, "raw") == 0) {
         mode = 2;
      } else if (strcmp(data, "summary") != 0) {
         return result_err("Unknown data mode. Use summary, series, or raw.");
      }
   }

   /* Build the pricehistory URL via either an explicit start/end date range or
    * a preset range. `label`, `freq`, and (date mode) the client-side trim
    * bounds are set by whichever branch runs, so nothing below dereferences a
    * range-table row that doesn't exist in date mode. */
   char url[512];
   char coerce_note[96] = "";
   char label[48];
   schwab_freq_t freq = SCHWAB_FREQ_DAILY;
   bool date_mode = (start && start[0]);
   int64_t start_ms = 0, end_excl = 0;
   if (!date_mode && end && end[0]) {
      return result_err("An end date needs a start date too (or use range).");
   }

   if (date_mode) {
      if (!schwab_valid_date(start)) {
         return result_err("Dates must be YYYY-MM-DD (e.g. 2025-03-01).");
      }
      int64_t s_sec = iso8601_parse_date_utc(start);
      /* +1 day of slack so "today" in a UTC+N timezone isn't rejected (start is
       * UTC-midnight); a genuinely future date just yields an empty window. */
      if (s_sec > (int64_t)time(NULL) + 86400) {
         return result_err("Start date is in the future.");
      }
      int64_t e_sec;
      if (end && end[0]) {
         if (!schwab_valid_date(end)) {
            return result_err("Dates must be YYYY-MM-DD (e.g. 2025-03-01).");
         }
         e_sec = iso8601_parse_date_utc(end);
         if (e_sec < s_sec) {
            return result_err("End date is before the start date.");
         }
      } else {
         e_sec = (int64_t)time(NULL);
      }
      /* interval: no coercion — periodType=year legally allows all three. */
      if (interval && interval[0]) {
         if (strcmp(interval, "daily") == 0) {
            freq = SCHWAB_FREQ_DAILY;
         } else if (strcmp(interval, "weekly") == 0) {
            freq = SCHWAB_FREQ_WEEKLY;
         } else if (strcmp(interval, "monthly") == 0) {
            freq = SCHWAB_FREQ_MONTHLY;
         } else {
            return result_err("Unknown interval. Use daily, weekly, or monthly.");
         }
      }
      start_ms = s_sec * 1000;
      end_excl = (e_sec + 86400) * 1000; /* 00:00Z of the day after `end` */
      int64_t end_ms = end_excl - 1;     /* end-of-day so Schwab includes the end day */
      snprintf(url, sizeof(url),
               "%s/pricehistory?symbol=%s&periodType=year&frequencyType=%s&frequency=1"
               "&startDate=%lld&endDate=%lld&needExtendedHoursData=false",
               SCHWAB_MARKETDATA_BASE, clean, freq_name(freq), (long long)start_ms,
               (long long)end_ms);
      snprintf(label, sizeof(label), "%s to %s", start, (end && end[0]) ? end : "today");
   } else {
      const char *rq = (range && range[0]) ? range : "1y";
      const hist_range_t *rr = NULL;
      for (size_t i = 0; i < sizeof(HIST_RANGES) / sizeof(HIST_RANGES[0]); i++) {
         if (strcmp(rq, HIST_RANGES[i].range) == 0) {
            rr = &HIST_RANGES[i];
            break;
         }
      }
      if (!rr) {
         return result_err("Unknown range. Use 1mo, 3mo, 6mo, 1y, 5y, or ytd.");
      }
      if (!interval || !interval[0]) {
         freq = rr->def_interval;
      } else if (strcmp(interval, "daily") == 0) {
         freq = SCHWAB_FREQ_DAILY;
      } else if (strcmp(interval, "weekly") == 0) {
         freq = SCHWAB_FREQ_WEEKLY;
      } else if (strcmp(interval, "monthly") == 0) {
         freq = SCHWAB_FREQ_MONTHLY;
      } else {
         return result_err("Unknown interval. Use daily, weekly, or monthly.");
      }
      if (!(rr->legal & freq_bit(freq))) {
         schwab_freq_t nf = (rr->legal & 2u) ? SCHWAB_FREQ_WEEKLY : SCHWAB_FREQ_DAILY;
         snprintf(coerce_note, sizeof(coerce_note), " (%s bars aren't available for %s — using %s)",
                  freq_name(freq), rr->range, freq_name(nf));
         freq = nf;
      }
      snprintf(url, sizeof(url),
               "%s/pricehistory?symbol=%s&periodType=%s&period=%d&frequencyType=%s&frequency=1"
               "&needExtendedHoursData=false",
               SCHWAB_MARKETDATA_BASE, clean, rr->period_type, rr->period, freq_name(freq));
      snprintf(label, sizeof(label), "%s", rr->range);
   }

   oauth_provider_config_t prov;
   char bearer[OAUTH_TOKEN_BUF_SIZE];
   char *err = NULL;
   if (schwab_bearer(user_id, &prov, bearer, sizeof(bearer), &err) != SCHWAB_RC_OK) {
      sodium_memzero(&prov, sizeof(prov));
      return err;
   }

   struct json_object *root = NULL;
   long http = 0;
   schwab_rc_t rc = schwab_get_with_retry(&prov, user_id, url, bearer, sizeof(bearer), &root,
                                          &http);
   sodium_memzero(bearer, sizeof(bearer));
   sodium_memzero(&prov, sizeof(prov));
   if (rc != SCHWAB_RC_OK) {
      return schwab_req_err(rc, http);
   }

   struct json_object *candles = NULL;
   if (!json_object_is_type(root, json_type_object) || jget_b(root, "empty", false) ||
       !json_object_object_get_ex(root, "candles", &candles) ||
       !json_object_is_type(candles, json_type_array) || json_object_array_length(candles) == 0) {
      json_object_put(root);
      char m[96];
      snprintf(m, sizeof(m), "No price history returned for %s.", clean);
      return result_err(m);
   }

   int n = (int)json_object_array_length(candles);
   if (n > SCHWAB_MAX_CANDLES) {
      n = SCHWAB_MAX_CANDLES;
   }
   double *op = malloc(sizeof(double) * n), *hi = malloc(sizeof(double) * n);
   double *lo = malloc(sizeof(double) * n), *cl = malloc(sizeof(double) * n);
   double *vo = malloc(sizeof(double) * n);
   int64_t *dt = malloc(sizeof(int64_t) * n);
   if (!op || !hi || !lo || !cl || !vo || !dt) {
      free(op);
      free(hi);
      free(lo);
      free(cl);
      free(vo);
      free(dt);
      json_object_put(root);
      return result_err("Stocks: out of memory.");
   }
   for (int i = 0; i < n; i++) {
      struct json_object *c = json_object_array_get_idx(candles, i);
      op[i] = jget_d(c, "open");
      hi[i] = jget_d(c, "high");
      lo[i] = jget_d(c, "low");
      cl[i] = jget_d(c, "close");
      vo[i] = jget_d(c, "volume");
      dt[i] = jget_i64(c, "datetime");
   }
   json_object_put(root);

   /* Date mode: trim to the requested window client-side (candles are oldest-
    * first). This makes both boundary days inclusive regardless of how Schwab
    * interprets startDate/endDate — a daily candle stamped ~05:00Z on the day
    * falls inside [start 00:00Z, day-after-end 00:00Z). Base pointers are kept
    * for free(); the window is shifted to the front. */
   if (date_mode) {
      int s0 = 0;
      while (s0 < n && dt[s0] < start_ms) {
         s0++;
      }
      int e = s0;
      while (e < n && dt[e] < end_excl) {
         e++;
      }
      int win = e - s0;
      if (win <= 0) {
         free(op);
         free(hi);
         free(lo);
         free(cl);
         free(vo);
         free(dt);
         char m[96];
         snprintf(m, sizeof(m), "No price history for %s in that date range.", clean);
         return result_err(m);
      }
      if (s0 > 0) {
         memmove(op, op + s0, sizeof(double) * win);
         memmove(hi, hi + s0, sizeof(double) * win);
         memmove(lo, lo + s0, sizeof(double) * win);
         memmove(cl, cl + s0, sizeof(double) * win);
         memmove(vo, vo + s0, sizeof(double) * win);
         memmove(dt, dt + s0, sizeof(int64_t) * win);
      }
      n = win;
   }

   /* Base the period return on the first candle's close (an unambiguous
    * "change across the returned window"). Schwab's previousClose is not
    * reliably the pre-window close, so using it would risk mislabeling a
    * one-bar move as the whole-period change. */
   schwab_hist_stats_t st;
   schwab_history_stats(cl, hi, lo, n, 0.0 /* base=0 → use close[0] */, freq, &st);

   strbuf_t sb;
   strbuf_init(&sb, 512);
   char d0[24], d1[24];
   fmt_date(dt[0], d0, sizeof(d0));
   fmt_date(dt[n - 1], d1, sizeof(d1));
   strbuf_appendf(&sb, "%s — %s price history, %s bars%s\n", clean, label, freq_name(freq),
                  coerce_note);
   strbuf_appendf(&sb, "%s to %s (%d bars). Last close $%.2f (as of %s).\n", d0, d1, n,
                  st.last_close, d1);
   strbuf_appendf(&sb, "Change %+.2f%% over the period; range $%.2f-$%.2f; max drawdown %.2f%%.\n",
                  st.pct_change, st.period_low, st.period_high, st.max_drawdown * 100.0);
   if (st.volatility >= 0.0) {
      strbuf_appendf(&sb, "Annualized volatility %.1f%% (%d %s log returns).\n",
                     st.volatility * 100.0, st.vol_n, freq_name(freq));
   } else {
      strbuf_append(&sb, "Annualized volatility: n/a (not enough data).\n");
   }
   if (st.sma20 >= 0.0) {
      strbuf_appendf(&sb, "20-bar SMA $%.2f (%s)", st.sma20,
                     st.last_close >= st.sma20 ? "above" : "below");
      if (st.sma50 >= 0.0) {
         strbuf_appendf(&sb, "; 50-bar SMA $%.2f (%s)", st.sma50,
                        st.last_close >= st.sma50 ? "above" : "below");
      } else {
         strbuf_append(&sb, "; 50-bar SMA needs \u226550 bars");
      }
      strbuf_append(&sb, ".\n");
   }

   if (mode == 1) {
      /* Compact series for charting — mirror the system_status trend contract. */
      int stride = (n + 58) / 59; /* ≤59 strided points + the forced last = ≤60 */
      if (stride < 1) {
         stride = 1;
      }
      int idx[64];
      int np = 0;
      for (int i = 0; i < n && np < 63; i += stride) {
         idx[np++] = i;
      }
      if (np == 0 || idx[np - 1] != n - 1) {
         idx[np++] = n - 1;
      }
      strbuf_append(&sb, "\nSeries (copy these arrays verbatim into a render_visual Chart.js line "
                         "chart — do not recompute):\n");
      strbuf_append(&sb, "labels=[");
      for (int j = 0; j < np; j++) {
         char d[24];
         fmt_date(dt[idx[j]], d, sizeof(d));
         strbuf_appendf(&sb, "%s\"%s\"", j ? "," : "", d);
      }
      strbuf_append(&sb, "]\nclose=[");
      for (int j = 0; j < np; j++) {
         strbuf_appendf(&sb, "%s%.2f", j ? "," : "", cl[idx[j]]);
      }
      strbuf_append(&sb, "]\nlow=[");
      for (int j = 0; j < np; j++) {
         strbuf_appendf(&sb, "%s%.2f", j ? "," : "", lo[idx[j]]);
      }
      strbuf_append(&sb, "]\nhigh=[");
      for (int j = 0; j < np; j++) {
         strbuf_appendf(&sb, "%s%.2f", j ? "," : "", hi[idx[j]]);
      }
      strbuf_append(&sb, "]\n");
   } else if (mode == 2) {
      int cap = 300;
      int raw_start = (n > cap) ? (n - cap) : 0;
      if (n > cap) {
         strbuf_appendf(&sb, "\nRaw candles (last %d of %d):\n", cap, n);
      } else {
         strbuf_appendf(&sb, "\nRaw candles (%d):\n", n);
      }
      for (int i = raw_start; i < n; i++) {
         char d[24], vb[24];
         fmt_date(dt[i], d, sizeof(d));
         fmt_volume(vo[i], vb, sizeof(vb));
         strbuf_appendf(&sb, "%s  O%.2f H%.2f L%.2f C%.2f  vol %s\n", d, op[i], hi[i], lo[i], cl[i],
                        vb);
      }
   }

   free(op);
   free(hi);
   free(lo);
   free(cl);
   free(vo);
   free(dt);

   char *outstr = (!strbuf_oom(&sb) && sb.buf) ? strdup(sb.buf) : NULL;
   strbuf_free(&sb);
   return outstr ? outstr : result_err("Schwab history formatting failed.");
}

/* ===== fundamentals ===== */

/* Format one instrument's fundamental block into sb. `io` is the instrument
 * object; `fund` is its fundamental sub-object (may equal io on a flat shape). */
static void fmt_fundamental(strbuf_t *sb,
                            const char *sym,
                            struct json_object *io,
                            struct json_object *fund) {
   const char *desc = jget_s(io, "description");
   strbuf_appendf(sb, "%s%s%s%s:\n", sym, (desc && desc[0]) ? " (" : "", desc ? desc : "",
                  (desc && desc[0]) ? ")" : "");
   double pe = jget_d(fund, "peRatio");
   double eps = jget_d(fund, "eps");
   if (eps == 0.0) {
      eps = jget_d(fund, "epsTTM");
   }
   double mcap = jget_d(fund, "marketCap");
   double beta = jget_d(fund, "beta");
   double dy = jget_d(fund, "dividendYield");
   double da = jget_d(fund, "dividendAmount");
   double h52 = jget_d(fund, "high52");
   double l52 = jget_d(fund, "low52");
   if (pe != 0.0) {
      strbuf_appendf(sb, "  P/E %.2f", pe);
   }
   if (eps != 0.0) {
      strbuf_appendf(sb, "  EPS $%.2f", eps);
   }
   if (mcap != 0.0) {
      char mb[24];
      fmt_volume(mcap, mb, sizeof(mb));
      strbuf_appendf(sb, "  Market cap $%s", mb);
   }
   if (beta != 0.0) {
      strbuf_appendf(sb, "  Beta %.2f", beta);
   }
   strbuf_append(sb, "\n");
   if (dy != 0.0 || da != 0.0) {
      strbuf_appendf(sb, "  Dividend yield %.2f%%", dy);
      if (da != 0.0) {
         strbuf_appendf(sb, " ($%.2f/yr)", da);
      }
      strbuf_append(sb, "\n");
   }
   if (h52 != 0.0 || l52 != 0.0) {
      strbuf_appendf(sb, "  52-week range $%.2f-$%.2f\n", l52, h52);
   }
}

char *schwab_service_fundamentals(int user_id, const char *symbols_csv) {
   char clean[512];
   if (clean_symbols(symbols_csv ? symbols_csv : "", clean, sizeof(clean), NULL) == 0) {
      return result_err("No valid stock symbols were given (e.g. NVDA).");
   }

   oauth_provider_config_t prov;
   char bearer[OAUTH_TOKEN_BUF_SIZE];
   char *err = NULL;
   if (schwab_bearer(user_id, &prov, bearer, sizeof(bearer), &err) != SCHWAB_RC_OK) {
      sodium_memzero(&prov, sizeof(prov));
      return err;
   }

   char url[1024];
   snprintf(url, sizeof(url), "%s/instruments?symbol=%s&projection=fundamental",
            SCHWAB_MARKETDATA_BASE, clean);

   struct json_object *root = NULL;
   long http = 0;
   schwab_rc_t rc = schwab_get_with_retry(&prov, user_id, url, bearer, sizeof(bearer), &root,
                                          &http);
   sodium_memzero(bearer, sizeof(bearer));
   sodium_memzero(&prov, sizeof(prov));
   if (rc != SCHWAB_RC_OK) {
      return schwab_req_err(rc, http);
   }
   if (!json_object_is_type(root, json_type_object)) {
      json_object_put(root);
      return result_err("Unexpected fundamentals response from Schwab.");
   }

   strbuf_t sb;
   strbuf_init(&sb, 512);
   int shown = 0;

   /* Documented shape is {"instruments":[{symbol,description,exchange,fundamental{…}}]}.
    * Fall back to a symbol-keyed object if that's what comes back. */
   struct json_object *instruments = NULL;
   if (json_object_object_get_ex(root, "instruments", &instruments) &&
       json_object_is_type(instruments, json_type_array)) {
      int ni = (int)json_object_array_length(instruments);
      for (int i = 0; i < ni; i++) {
         struct json_object *io = json_object_array_get_idx(instruments, i);
         if (!json_object_is_type(io, json_type_object)) {
            continue;
         }
         struct json_object *fund = NULL;
         if (!json_object_object_get_ex(io, "fundamental", &fund)) {
            fund = io; /* flat shape */
         }
         fmt_fundamental(&sb, jget_s(io, "symbol"), io, fund);
         shown++;
      }
   } else {
      json_object_object_foreach(root, key, io) {
         if (!json_object_is_type(io, json_type_object)) {
            continue;
         }
         struct json_object *fund = NULL;
         if (!json_object_object_get_ex(io, "fundamental", &fund)) {
            fund = io;
         }
         fmt_fundamental(&sb, key, io, fund);
         shown++;
      }
   }

   json_object_put(root);
   if (shown == 0) {
      strbuf_free(&sb);
      return result_err("No fundamentals returned (check the symbol).");
   }

   char *outstr = (!strbuf_oom(&sb) && sb.buf) ? strdup(sb.buf) : NULL;
   strbuf_free(&sb);
   return outstr ? outstr : result_err("Schwab fundamentals formatting failed.");
}

/* ===== transactions (recent account activity) ===== */

/* Format an epoch-seconds instant as Schwab's ISO-8601 datetime. The date is
 * drawn from the (UTC-midnight) instant; the time is fixed to the start or end of
 * that day. SPIKE: confirm whether the `.000Z` millisecond suffix is required or
 * a bare `Z` is accepted (schwab-py's real formatter omits the .000). */
static void fmt_iso8601_z(int64_t epoch_sec, bool is_end, char *out, size_t n) {
   time_t s = (time_t)epoch_sec;
   struct tm tmv;
   const char *fmt = is_end ? "%Y-%m-%dT23:59:59.000Z" : "%Y-%m-%dT00:00:00.000Z";
   if (!gmtime_r(&s, &tmv) || strftime(out, n, fmt, &tmv) == 0) {
      snprintf(out, n, "?");
   }
}

/* A Schwab account hashValue is interpolated as a URL *path segment*, so validate
 * its charset and length before building the request — a stray '/' or '?' would
 * change the endpoint. Defense in depth even over TLS. */
static bool schwab_valid_hash(const char *h) {
   if (!h || !h[0]) {
      return false;
   }
   size_t len = strlen(h);
   if (len > 128) {
      return false;
   }
   for (size_t i = 0; i < len; i++) {
      char c = h[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
         return false;
      }
   }
   return true;
}

/* Map a user-language category to the Schwab `types=` query value. Fees are legs
 * inside TRADE rows (not a transaction type), so `fees` requests all types and
 * the fee legs are summed client-side (post-spike, in schwab_txn.c). Returns NULL
 * for an unknown category. */
static const char *txn_types_for(const char *cat) {
   static const struct {
      const char *cat;
      const char *raw;
   } M[] = {
      { "all", SCHWAB_TXN_ALL_TYPES },
      { "trades", "TRADE" },
      { "dividends", "DIVIDEND_OR_INTEREST" },
      { "deposits", "ACH_RECEIPT,CASH_RECEIPT,WIRE_IN,ELECTRONIC_FUND,JOURNAL" },
      { "withdrawals", "ACH_DISBURSEMENT,CASH_DISBURSEMENT,WIRE_OUT" },
      { "fees", SCHWAB_TXN_ALL_TYPES },
   };
   for (size_t i = 0; i < sizeof(M) / sizeof(M[0]); i++) {
      if (strcmp(cat, M[i].cat) == 0) {
         return M[i].raw;
      }
   }
   return NULL;
}

/* qsort: newest-first by the ISO `time` string (all rows share one format). */
static int txn_cmp_desc(const void *a, const void *b) {
   const schwab_txn_t *x = a, *y = b;
   return strcmp(y->time_key, x->time_key);
}

/* Render one account's transactions: aggregate over ALL rows, then a
 * newest-first listing (internal sweeps hidden from the "all" view). @p rows is
 * the parsed JSON array; @p cat is the requested category ("all" or a filter). */
static void fmt_txn_account(strbuf_t *sb,
                            const char *masked,
                            struct json_object *rows,
                            const char *cat) {
   size_t n = json_object_array_length(rows);
   if (n == 0) {
      strbuf_appendf(sb, "Account %s: no transactions in this window.\n", masked);
      return;
   }
   size_t cap = n > SCHWAB_TXN_MAX_ROWS ? SCHWAB_TXN_MAX_ROWS : n;
   schwab_txn_t *tx = malloc(sizeof(schwab_txn_t) * cap);
   if (!tx) {
      strbuf_appendf(sb, "Account %s: out of memory formatting transactions.\n", masked);
      return;
   }

   /* Aggregate over EVERY row; retain up to `cap` for the listing. (Totals stay
    * complete even if a pathological account exceeds the listing bound.) */
   schwab_txn_agg_t agg;
   schwab_txn_agg_init(&agg);
   size_t parsed = 0;
   for (size_t i = 0; i < n; i++) {
      schwab_txn_t t;
      schwab_txn_classify(json_object_array_get_idx(rows, i), &t);
      schwab_txn_agg_add(&agg, &t);
      if (parsed < cap) {
         tx[parsed++] = t;
      }
   }
   qsort(tx, parsed, sizeof(schwab_txn_t), txn_cmp_desc);

   strbuf_appendf(sb, "Account %s — %d transaction%s:\n", masked, agg.count,
                  agg.count == 1 ? "" : "s");
   if (agg.n_trade) {
      strbuf_appendf(sb, "  Trades: %d (net cash %+.2f)\n", agg.n_trade, agg.net_trade);
   }
   if (agg.n_dividend) {
      strbuf_appendf(sb, "  Dividends/interest: %d (%+.2f)\n", agg.n_dividend, agg.income);
   }
   if (agg.n_deposit) {
      strbuf_appendf(sb, "  Deposits: %d (%+.2f)\n", agg.n_deposit, agg.deposits);
   }
   if (agg.n_withdrawal) {
      strbuf_appendf(sb, "  Withdrawals: %d (%+.2f)\n", agg.n_withdrawal, agg.withdrawals);
   }
   if (agg.n_internal) {
      /* Listed as its own line so the per-category counts always sum to the
       * header total, including in a filtered view (e.g. a `deposits` filter
       * that also returns JOURNAL rows). */
      strbuf_appendf(sb, "  Internal moves: %d (net %+.2f)\n", agg.n_internal, agg.net_internal);
   }
   if (agg.fees > 0.0) {
      strbuf_appendf(sb, "  Fees paid: %.2f\n", agg.fees);
   }
   if (agg.n_other) {
      strbuf_appendf(sb, "  Other: %d\n", agg.n_other);
   }

   bool hide_internal = (strcmp(cat, "all") == 0);
   int listed = 0, hidden_internal = 0;
   strbuf_append(sb, "  Activity (newest first):\n");
   for (size_t i = 0; i < parsed; i++) {
      const schwab_txn_t *t = &tx[i];
      if (hide_internal && schwab_txn_is_internal(t)) {
         hidden_internal++;
         continue;
      }
      if (listed >= SCHWAB_TXN_LIST_CAP) {
         continue;
      }
      strbuf_appendf(sb, "    %s  %s", t->date[0] ? t->date : "?", schwab_txn_cat_name(t->cat));
      if (t->symbol[0]) {
         strbuf_appendf(sb, " %s", t->symbol);
         if (t->cat == SCHWAB_TXN_TRADE && t->qty != 0.0) {
            strbuf_appendf(sb, " %s %.4g sh", t->qty > 0.0 ? "buy" : "sell",
                           t->qty > 0.0 ? t->qty : -t->qty);
         }
      } else if (t->memo[0]) {
         char safe[96]; /* memo is <= 80; masking only shrinks it */
         schwab_txn_mask_numbers(t->memo, safe, sizeof(safe));
         strbuf_appendf(sb, " %s", safe);
      }
      strbuf_appendf(sb, "  %+.2f\n", t->net_amount);
      listed++;
   }

   int shown_total = 0;
   for (size_t i = 0; i < parsed; i++) {
      if (!(hide_internal && schwab_txn_is_internal(&tx[i]))) {
         shown_total++;
      }
   }
   if (listed < shown_total) {
      strbuf_appendf(sb, "    (showing %d of %d; ask for a narrower window or a type filter.)\n",
                     listed, shown_total);
   }
   if (hidden_internal > 0) {
      strbuf_appendf(sb,
                     "    (%d internal cash sweep%s not listed; counted in the totals above.)\n",
                     hidden_internal, hidden_internal == 1 ? "" : "s");
   }
   if (n > cap) {
      /* Totals above cover all rows; only the listing array is bounded. */
      strbuf_appendf(sb, "    (%zu transactions total; the activity list covers %zu of them.)\n", n,
                     cap);
   }
   free(tx);
}

char *schwab_service_transactions(int user_id,
                                  const char *symbol,
                                  const char *start,
                                  const char *end,
                                  const char *type) {
   /* Optional single-symbol filter (URL-safe token). Empty = no filter; a
    * provided-but-invalid symbol is an error. */
   char sym_clean[32] = "";
   if (symbol && symbol[0]) {
      if (clean_symbols(symbol, sym_clean, sizeof(sym_clean), NULL) == 0) {
         return result_err("That doesn't look like a valid stock symbol (e.g. NVDA).");
      }
      char *comma = strchr(sym_clean, ',');
      if (comma) {
         *comma = '\0'; /* single symbol only */
      }
   }

   /* Category → Schwab types= list (default all). ENUM params are not validated
    * at dispatch, so whitelist here. */
   const char *cat = (type && type[0]) ? type : "all";
   const char *types = txn_types_for(cat);
   if (!types) {
      return result_err("Unknown transaction type. Use trades, dividends, deposits, "
                        "withdrawals, fees, or all.");
   }

   /* Date window: reuse the history guards, then add the ~1-year lookback ceiling.
    * Default (no start) = the last SCHWAB_TXN_DEFAULT_DAYS. */
   int64_t now = (int64_t)time(NULL);
   int64_t midnight_today = now - (now % 86400);
   int64_t s_sec, e_sec;
   bool date_mode = (start && start[0]);
   if (!date_mode && end && end[0]) {
      return result_err("An end date needs a start date too.");
   }
   if (date_mode) {
      if (!schwab_valid_date(start)) {
         return result_err("Dates must be YYYY-MM-DD (e.g. 2025-03-01).");
      }
      s_sec = iso8601_parse_date_utc(start);
      if (s_sec > now + 86400) { /* +1 day of slack for UTC+N "today" */
         return result_err("Start date is in the future.");
      }
      /* Compare against UTC-midnight (matching s_sec) so a start exactly ~1 year
       * old isn't rejected by a mid-day `now`. */
      if (s_sec < midnight_today - (int64_t)SCHWAB_TXN_MAX_LOOKBACK_DAYS * 86400) {
         return result_err("Schwab only provides about a year of transaction history — "
                           "try a more recent start date.");
      }
      if (end && end[0]) {
         if (!schwab_valid_date(end)) {
            return result_err("Dates must be YYYY-MM-DD (e.g. 2025-03-01).");
         }
         e_sec = iso8601_parse_date_utc(end);
         if (e_sec < s_sec) {
            return result_err("End date is before the start date.");
         }
         if (e_sec > midnight_today) {
            e_sec = midnight_today; /* clamp a future end to today */
         }
      } else {
         e_sec = midnight_today;
      }
   } else {
      e_sec = midnight_today;
      s_sec = midnight_today - (int64_t)SCHWAB_TXN_DEFAULT_DAYS * 86400;
   }
   char start_iso[32], end_iso[32];
   fmt_iso8601_z(s_sec, false, start_iso, sizeof(start_iso));
   fmt_iso8601_z(e_sec, true, end_iso, sizeof(end_iso));

   oauth_provider_config_t prov;
   char bearer[OAUTH_TOKEN_BUF_SIZE];
   char *err = NULL;
   if (schwab_bearer(user_id, &prov, bearer, sizeof(bearer), &err) != SCHWAB_RC_OK) {
      sodium_memzero(&prov, sizeof(prov));
      return err;
   }

   /* Call 1: resolve account hashes. The response carries plaintext account
    * numbers — copy out the masked number + validated hash, then drop it. */
   char acct_url[128];
   snprintf(acct_url, sizeof(acct_url), "%s/accounts/accountNumbers", SCHWAB_TRADER_BASE);
   struct json_object *aroot = NULL;
   long http = 0;
   schwab_rc_t rc = schwab_get_with_retry(&prov, user_id, acct_url, bearer, sizeof(bearer), &aroot,
                                          &http);
   if (rc != SCHWAB_RC_OK) {
      sodium_memzero(bearer, sizeof(bearer));
      sodium_memzero(&prov, sizeof(prov));
      return schwab_req_err(rc, http);
   }
   if (!json_object_is_type(aroot, json_type_array)) {
      json_object_put(aroot);
      sodium_memzero(bearer, sizeof(bearer));
      sodium_memzero(&prov, sizeof(prov));
      return result_err("Unexpected account response from Schwab.");
   }

   char hashes[SCHWAB_TXN_MAX_ACCOUNTS][129];
   char masked[SCHWAB_TXN_MAX_ACCOUNTS][24];
   int naccts = 0;
   size_t na = json_object_array_length(aroot);
   for (size_t i = 0; i < na && naccts < SCHWAB_TXN_MAX_ACCOUNTS; i++) {
      struct json_object *el = json_object_array_get_idx(aroot, i);
      const char *hv = jget_s(el, "hashValue");
      if (!schwab_valid_hash(hv)) {
         continue;
      }
      snprintf(hashes[naccts], sizeof(hashes[naccts]), "%s", hv);
      mask_account(jget_s(el, "accountNumber"), masked[naccts], sizeof(masked[naccts]));
      naccts++;
   }
   json_object_put(aroot);

   if (naccts == 0) {
      sodium_memzero(bearer, sizeof(bearer));
      sodium_memzero(&prov, sizeof(prov));
      return result_err("No Schwab accounts were found on your link.");
   }

   /* Call 2 (per account): fetch the window. Per-account failure is isolated so
    * one bad account doesn't sink the others. */
   strbuf_t sb;
   strbuf_init(&sb, 1024);
   strbuf_appendf(&sb, "Schwab transactions (%.10s to %.10s%s%s):\n", start_iso, end_iso,
                  strcmp(cat, "all") == 0 ? "" : ", ", strcmp(cat, "all") == 0 ? "" : cat);

   for (int a = 0; a < naccts; a++) {
      char url[1024];
      int un = snprintf(url, sizeof(url),
                        "%s/accounts/%s/transactions?startDate=%s&endDate=%s&types=%s",
                        SCHWAB_TRADER_BASE, hashes[a], start_iso, end_iso, types);
      if (un <= 0 || (size_t)un >= sizeof(url)) {
         /* All components are validated/bounded, so this shouldn't happen; refuse
          * rather than issue a truncated request. */
         strbuf_appendf(&sb, "Account %s: internal error building the request.\n", masked[a]);
         continue;
      }
      if (sym_clean[0]) {
         size_t off = (size_t)un;
         snprintf(url + off, sizeof(url) - off, "&symbol=%s", sym_clean);
      }

      struct json_object *troot = NULL;
      long thttp = 0;
      schwab_rc_t trc = schwab_get_with_retry(&prov, user_id, url, bearer, sizeof(bearer), &troot,
                                              &thttp);
      if (trc != SCHWAB_RC_OK) {
         char *e = schwab_req_err(trc, thttp);
         strbuf_appendf(&sb, "Account %s: %s\n", masked[a],
                        e ? e + 1 /* skip TOOL_RESULT_ERROR_MARK */ : "request failed");
         free(e);
         continue;
      }
      if (!json_object_is_type(troot, json_type_array)) {
         strbuf_appendf(&sb, "Account %s: unexpected transactions response.\n", masked[a]);
         json_object_put(troot);
         continue;
      }
      fmt_txn_account(&sb, masked[a], troot, cat);
      json_object_put(troot);
   }

   sodium_memzero(bearer, sizeof(bearer));
   sodium_memzero(&prov, sizeof(prov));

   char *outstr = (!strbuf_oom(&sb) && sb.buf) ? strdup(sb.buf) : NULL;
   strbuf_free(&sb);
   return outstr ? outstr : result_err("Schwab transactions formatting failed.");
}
