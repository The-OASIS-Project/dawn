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
 * Charles Schwab portfolio — structured snapshot parser + JSON serializer.
 * See schwab_portfolio.h. The /accounts field paths mirror the confirmed shape
 * DAWN already parses in schwab_service.c (fmt_position + account loop).
 */

#include "tools/schwab_portfolio.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dawn_error.h"

/* ----- small json-c accessors (null-safe), mirroring schwab_service.c ----- */

static double jget_d(struct json_object *o, const char *k) {
   struct json_object *v = NULL;
   return (o && json_object_object_get_ex(o, k, &v)) ? json_object_get_double(v) : 0.0;
}

static const char *jget_s(struct json_object *o, const char *k) {
   struct json_object *v = NULL;
   return (o && json_object_object_get_ex(o, k, &v)) ? json_object_get_string(v) : "";
}

/* Mask an account number to "…1234"; never stores more than the last 4 digits. */
static void mask_account(const char *acct, char *out, size_t out_len) {
   size_t len = acct ? strlen(acct) : 0;
   if (len >= 4) {
      snprintf(out, out_len, "\xe2\x80\xa6%s", acct + (len - 4));
   } else {
      snprintf(out, out_len, "\xe2\x80\xa6");
   }
}

/* Percentage of a component against its basis = whole − component (e.g. day
 * change vs prior close, unrealized P/L vs cost). 0 when the basis is ~0. */
static double pct_of_basis(double component, double whole) {
   double basis = whole - component;
   if (basis > 1e-9 || basis < -1e-9) {
      return component / basis * 100.0;
   }
   return 0.0;
}

schwab_asset_type_t schwab_asset_type_from_str(const char *s) {
   if (!s || !s[0]) {
      return SCHWAB_ASSET_OTHER;
   }
   /* Schwab reports upper-case assetType tokens. NOTE: plain "EQUITY" covers both
    * stocks and (today) most ETFs — Schwab does not always distinguish them here;
    * COLLECTIVE_INVESTMENT is its newer ETF/closed-end tag. ETF-vs-equity fidelity
    * is the one open live-spike item (docs §12). */
   if (strcmp(s, "EQUITY") == 0)
      return SCHWAB_ASSET_EQUITY;
   if (strcmp(s, "ETF") == 0 || strcmp(s, "COLLECTIVE_INVESTMENT") == 0)
      return SCHWAB_ASSET_ETF;
   if (strcmp(s, "OPTION") == 0)
      return SCHWAB_ASSET_OPTION;
   if (strcmp(s, "MUTUAL_FUND") == 0)
      return SCHWAB_ASSET_MUTUAL_FUND;
   if (strcmp(s, "CASH_EQUIVALENT") == 0)
      return SCHWAB_ASSET_CASH_EQUIVALENT;
   return SCHWAB_ASSET_OTHER;
}

const char *schwab_asset_type_str(schwab_asset_type_t t) {
   switch (t) {
      case SCHWAB_ASSET_EQUITY:
         return "equity";
      case SCHWAB_ASSET_ETF:
         return "etf";
      case SCHWAB_ASSET_OPTION:
         return "option";
      case SCHWAB_ASSET_MUTUAL_FUND:
         return "mutual_fund";
      case SCHWAB_ASSET_CASH_EQUIVALENT:
         return "cash_equivalent";
      case SCHWAB_ASSET_OTHER:
      default:
         return "other";
   }
}

static void parse_position(struct json_object *pos, schwab_position_t *out) {
   struct json_object *inst = NULL;
   json_object_object_get_ex(pos, "instrument", &inst);

   const char *sym = jget_s(inst, "symbol");
   snprintf(out->symbol, sizeof(out->symbol), "%s", (sym && sym[0]) ? sym : "?");
   const char *desc = jget_s(inst, "description");
   snprintf(out->description, sizeof(out->description), "%s", desc ? desc : "");
   out->asset_type = schwab_asset_type_from_str(jget_s(inst, "assetType"));

   double lq = jget_d(pos, "longQuantity");
   double sq = jget_d(pos, "shortQuantity");
   out->qty = lq - sq; /* net; negative for a net short */
   out->market_value = jget_d(pos, "marketValue");
   out->unrealized_pl = jget_d(pos, "longOpenProfitLoss");
   out->day_change = jget_d(pos, "currentDayProfitLoss");
   out->price = (out->qty > 1e-9 || out->qty < -1e-9) ? out->market_value / out->qty : 0.0;
   out->unrealized_pl_pct = pct_of_basis(out->unrealized_pl, out->market_value);
   out->day_change_pct = pct_of_basis(out->day_change, out->market_value);
}

int schwab_portfolio_parse(struct json_object *root, schwab_portfolio_t *out) {
   if (!out) {
      return FAILURE;
   }
   memset(out, 0, sizeof(*out));
   if (!root || !json_object_is_type(root, json_type_array)) {
      return FAILURE;
   }

   size_t nacc = json_object_array_length(root);
   if (nacc == 0) {
      return SUCCESS; /* linked but no accounts — a valid empty snapshot */
   }
   out->accounts = calloc(nacc, sizeof(schwab_account_t));
   if (!out->accounts) {
      return FAILURE;
   }

   double total_mv = 0.0; /* Σ position market value — basis for the total %s */

   for (size_t i = 0; i < nacc; i++) {
      struct json_object *el = json_object_array_get_idx(root, i);
      struct json_object *sa = NULL;
      if (!json_object_object_get_ex(el, "securitiesAccount", &sa)) {
         continue;
      }
      schwab_account_t *acct = &out->accounts[out->account_count];

      char masked[SCHWAB_ACCT_LABEL_MAX];
      mask_account(jget_s(sa, "accountNumber"), masked, sizeof(masked));
      snprintf(acct->label, sizeof(acct->label), "%s", masked);
      snprintf(acct->acct_type, sizeof(acct->acct_type), "%s", jget_s(sa, "type"));

      struct json_object *bal = NULL;
      json_object_object_get_ex(sa, "currentBalances", &bal);
      acct->value = jget_d(bal, "liquidationValue"); /* authoritative account total */
      acct->cash = jget_d(bal, "cashBalance");
      out->total_value += acct->value;

      struct json_object *positions = NULL;
      if (json_object_object_get_ex(sa, "positions", &positions) &&
          json_object_is_type(positions, json_type_array)) {
         size_t np = json_object_array_length(positions);
         if (np > 0) {
            acct->positions = calloc(np, sizeof(schwab_position_t));
            if (!acct->positions) {
               schwab_portfolio_free(out);
               return FAILURE;
            }
            double acct_mv = 0.0;
            for (size_t j = 0; j < np; j++) {
               schwab_position_t *p = &acct->positions[acct->position_count];
               parse_position(json_object_array_get_idx(positions, j), p);
               acct->day_change += p->day_change;
               acct->unrealized_pl += p->unrealized_pl;
               acct_mv += p->market_value;
               acct->position_count++;
            }
            /* Account unrealized % is against the POSITION cost basis, never
             * liquidationValue (which includes cash and would dilute it). */
            acct->unrealized_pl_pct = pct_of_basis(acct->unrealized_pl, acct_mv);
            total_mv += acct_mv;
         }
      }

      out->total_day_change += acct->day_change;
      out->total_unrealized_pl += acct->unrealized_pl;
      out->account_count++;
   }

   out->total_day_change_pct = pct_of_basis(out->total_day_change, total_mv);
   out->total_unrealized_pl_pct = pct_of_basis(out->total_unrealized_pl, total_mv);
   return SUCCESS;
}

static struct json_object *position_json(const schwab_position_t *p) {
   struct json_object *o = json_object_new_object();
   json_object_object_add(o, "symbol", json_object_new_string(p->symbol));
   if (p->description[0]) {
      /* Omit when empty so a consumer's presence-check means "has a name". */
      json_object_object_add(o, "description", json_object_new_string(p->description));
   }
   json_object_object_add(o, "asset_type",
                          json_object_new_string(schwab_asset_type_str(p->asset_type)));
   json_object_object_add(o, "qty", json_object_new_double(p->qty));
   json_object_object_add(o, "price", json_object_new_double(p->price));
   json_object_object_add(o, "value", json_object_new_double(p->market_value));
   json_object_object_add(o, "unrealized_pl", json_object_new_double(p->unrealized_pl));
   json_object_object_add(o, "unrealized_pl_pct", json_object_new_double(p->unrealized_pl_pct));
   json_object_object_add(o, "day_change", json_object_new_double(p->day_change));
   json_object_object_add(o, "day_change_pct", json_object_new_double(p->day_change_pct));
   return o;
}

struct json_object *schwab_portfolio_payload_jobj(const schwab_portfolio_t *p,
                                                  const char *status,
                                                  const char *market,
                                                  bool ext_hours,
                                                  int64_t link_expires_at) {
   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "status", json_object_new_string(status ? status : "error"));
   json_object_object_add(payload, "market", json_object_new_string(market ? market : "closed"));
   json_object_object_add(payload, "ext_hours",
                          json_object_new_boolean(p ? p->ext_hours : ext_hours));
   json_object_object_add(payload, "as_of", json_object_new_int64(p ? p->as_of : 0));
   /* Emitted even on a state-only (e.g. token_expired) frame — that's exactly when a
    * panel wants to say "reconnect Schwab". Omitted when the link time is unknown. */
   if (link_expires_at > 0) {
      json_object_object_add(payload, "link_expires_at", json_object_new_int64(link_expires_at));
   }

   if (p) {
      json_object_object_add(payload, "total_value", json_object_new_double(p->total_value));
      json_object_object_add(payload, "total_day_change",
                             json_object_new_double(p->total_day_change));
      json_object_object_add(payload, "total_day_change_pct",
                             json_object_new_double(p->total_day_change_pct));
      json_object_object_add(payload, "total_unrealized_pl",
                             json_object_new_double(p->total_unrealized_pl));
      json_object_object_add(payload, "total_unrealized_pl_pct",
                             json_object_new_double(p->total_unrealized_pl_pct));

      struct json_object *accts = json_object_new_array();
      for (int i = 0; i < p->account_count; i++) {
         const schwab_account_t *a = &p->accounts[i];
         struct json_object *ao = json_object_new_object();
         json_object_object_add(ao, "label", json_object_new_string(a->label));
         json_object_object_add(ao, "acct_type", json_object_new_string(a->acct_type));
         json_object_object_add(ao, "value", json_object_new_double(a->value));
         json_object_object_add(ao, "cash", json_object_new_double(a->cash));
         json_object_object_add(ao, "day_change", json_object_new_double(a->day_change));
         json_object_object_add(ao, "unrealized_pl", json_object_new_double(a->unrealized_pl));
         json_object_object_add(ao, "unrealized_pl_pct",
                                json_object_new_double(a->unrealized_pl_pct));
         struct json_object *pos = json_object_new_array();
         for (int j = 0; j < a->position_count; j++) {
            json_object_array_add(pos, position_json(&a->positions[j]));
         }
         json_object_object_add(ao, "positions", pos);
         json_object_array_add(accts, ao);
      }
      json_object_object_add(payload, "accounts", accts);
   }
   return payload;
}

char *schwab_portfolio_to_json(const schwab_portfolio_t *p,
                               const char *status,
                               const char *market,
                               bool ext_hours,
                               int64_t link_expires_at) {
   struct json_object *payload = schwab_portfolio_payload_jobj(p, status, market, ext_hours,
                                                               link_expires_at);
   const char *s = json_object_to_json_string_ext(payload, JSON_C_TO_STRING_PLAIN);
   char *out = s ? strdup(s) : NULL;
   json_object_put(payload);
   return out;
}

void schwab_portfolio_free(schwab_portfolio_t *p) {
   if (!p || !p->accounts) {
      if (p) {
         memset(p, 0, sizeof(*p));
      }
      return;
   }
   for (int i = 0; i < p->account_count; i++) {
      free(p->accounts[i].positions);
   }
   free(p->accounts);
   memset(p, 0, sizeof(*p));
}
