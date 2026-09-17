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
 * Charles Schwab transaction classifier (pure). The /transactions schema is
 * undocumented; the shape below was pinned from a live capture:
 *
 *   row = { type, status, subAccount, netAmount, time ("YYYY-MM-DDThh:mm:ss+0000"),
 *           tradeDate, settlementDate, description, activityId, orderId,
 *           accountNumber (PII — masked/withheld by the caller),
 *           transferItems: [ { amount, cost, price, feeType?, positionEffect?,
 *                              instrument: { symbol, assetType, description, ... } } ] }
 *
 *   - TRADE: one non-CURRENCY leg holds the security (symbol, amount=shares,
 *            positionEffect); the CURRENCY legs with feeType are the fees.
 *            netAmount is the cash impact (negative = buy).
 *   - DIVIDEND_OR_INTEREST: a single CURRENCY leg (the cash); the paying security
 *            appears ONLY in the row-level `description` prose — no symbol field.
 *   - JOURNAL / MONEY_MARKET / etc.: internal cash movement (one CURRENCY leg).
 *   - ACH/WIRE/CASH receipts+disbursements, ELECTRONIC_FUND: external cash flow,
 *     direction by the sign of netAmount.
 *
 * The outer `type` covers economically opposite events, so category is decided
 * from the type plus the sign of netAmount, and the symbol from the legs.
 */

#include "tools/schwab_txn.h"

#include <string.h>

/* ---- local json helpers (mirror schwab_service.c; kept local so this module
 * stays free of service-layer coupling and unit-testable on its own) ---- */
static double txn_d(struct json_object *o, const char *k) {
   struct json_object *v = NULL;
   return (o && json_object_object_get_ex(o, k, &v)) ? json_object_get_double(v) : 0.0;
}
static const char *txn_s(struct json_object *o, const char *k) {
   struct json_object *v = NULL;
   return (o && json_object_object_get_ex(o, k, &v)) ? json_object_get_string(v) : "";
}

static void copy_str(char *dst, size_t n, const char *src) {
   if (!src) {
      src = "";
   }
   size_t i = 0;
   for (; src[i] && i + 1 < n; i++) {
      dst[i] = src[i];
   }
   dst[i] = '\0';
}

const char *schwab_txn_cat_name(schwab_txn_cat_t c) {
   switch (c) {
      case SCHWAB_TXN_TRADE:
         return "Trade";
      case SCHWAB_TXN_DIVIDEND:
         return "Dividend/Interest";
      case SCHWAB_TXN_DEPOSIT:
         return "Deposit";
      case SCHWAB_TXN_WITHDRAWAL:
         return "Withdrawal";
      case SCHWAB_TXN_INTERNAL:
         return "Internal";
      case SCHWAB_TXN_OTHER:
         break;
   }
   return "Other";
}

bool schwab_txn_is_internal(const schwab_txn_t *t) {
   return t && t->cat == SCHWAB_TXN_INTERNAL;
}

void schwab_txn_mask_numbers(const char *in, char *out, size_t out_len) {
   if (!out || out_len == 0) {
      return;
   }
   size_t o = 0;
   const char *p = in ? in : "";
   while (*p) {
      if (*p >= '0' && *p <= '9') {
         /* A digit token = digits, with single interior ' '/'-' separators
          * between two digit groups. Mask to '#' at >= 5 digits total. */
         const char *start = p;
         int digits = 0;
         for (;;) {
            if (*p >= '0' && *p <= '9') {
               digits++;
               p++;
            } else if ((*p == ' ' || *p == '-') && p[1] >= '0' && p[1] <= '9') {
               p++;
            } else {
               break;
            }
         }
         if (digits >= 5) {
            if (o + 1 < out_len) {
               out[o++] = '#';
            }
         } else {
            for (const char *q = start; q < p && o + 1 < out_len; q++) {
               out[o++] = *q;
            }
         }
      } else {
         if (o + 1 < out_len) {
            out[o++] = *p;
         }
         p++;
      }
   }
   out[o] = '\0';
}

/* Categorize from the raw Schwab type + the sign of the net cash. */
static schwab_txn_cat_t categorize(const char *type, double net, bool *external) {
   *external = false;
   if (strcmp(type, "TRADE") == 0) {
      return SCHWAB_TXN_TRADE;
   }
   if (strcmp(type, "DIVIDEND_OR_INTEREST") == 0) {
      return SCHWAB_TXN_DIVIDEND;
   }
   /* External cash flows — direction by sign for the bidirectional types. */
   if (strcmp(type, "ACH_RECEIPT") == 0 || strcmp(type, "CASH_RECEIPT") == 0 ||
       strcmp(type, "WIRE_IN") == 0) {
      *external = true;
      return SCHWAB_TXN_DEPOSIT;
   }
   if (strcmp(type, "ACH_DISBURSEMENT") == 0 || strcmp(type, "CASH_DISBURSEMENT") == 0 ||
       strcmp(type, "WIRE_OUT") == 0) {
      *external = true;
      return SCHWAB_TXN_WITHDRAWAL;
   }
   if (strcmp(type, "ELECTRONIC_FUND") == 0) {
      *external = true;
      return net >= 0.0 ? SCHWAB_TXN_DEPOSIT : SCHWAB_TXN_WITHDRAWAL;
   }
   /* Internal moves: journals, sweeps, position transfers, adjustments. A
    * RECEIVE_AND_DELIVER can be an external ACAT, but also a split/reinvest, so
    * it stays internal for v1 (a future money-weighted-return round refines it). */
   if (strcmp(type, "JOURNAL") == 0 || strcmp(type, "MONEY_MARKET") == 0 ||
       strcmp(type, "SMA_ADJUSTMENT") == 0 || strcmp(type, "MARGIN_CALL") == 0 ||
       strcmp(type, "MEMORANDUM") == 0 || strcmp(type, "RECEIVE_AND_DELIVER") == 0) {
      return SCHWAB_TXN_INTERNAL;
   }
   return SCHWAB_TXN_OTHER;
}

void schwab_txn_classify(struct json_object *row, schwab_txn_t *out) {
   memset(out, 0, sizeof(*out));
   if (!row) {
      out->cat = SCHWAB_TXN_OTHER;
      return;
   }

   const char *type = txn_s(row, "type");
   copy_str(out->rawtype, sizeof(out->rawtype), type);
   out->net_amount = txn_d(row, "netAmount");

   /* Prefer `time`; fall back to tradeDate. Date = first 10 chars (YYYY-MM-DD). */
   const char *tm = txn_s(row, "time");
   if (!tm[0]) {
      tm = txn_s(row, "tradeDate");
   }
   copy_str(out->time_key, sizeof(out->time_key), tm);
   copy_str(out->date, sizeof(out->date), tm); /* copy_str truncates to 10 chars */

   copy_str(out->memo, sizeof(out->memo), txn_s(row, "description"));

   /* Walk the legs: first non-CURRENCY leg gives the security symbol + qty; sum
    * the fee legs (feeType set). */
   struct json_object *legs = NULL;
   if (json_object_object_get_ex(row, "transferItems", &legs) &&
       json_object_is_type(legs, json_type_array)) {
      size_t nl = json_object_array_length(legs);
      for (size_t i = 0; i < nl; i++) {
         struct json_object *leg = json_object_array_get_idx(legs, i);
         if (!json_object_is_type(leg, json_type_object)) {
            continue;
         }
         struct json_object *inst = NULL;
         json_object_object_get_ex(leg, "instrument", &inst);
         const char *at = txn_s(inst, "assetType");
         const char *fee = txn_s(leg, "feeType");
         if (fee[0]) {
            /* Fee legs carry the fee as a (signed) cash amount; accumulate as a
             * positive "fees paid" figure. */
            double a = txn_d(leg, "amount");
            out->fee_total += a < 0.0 ? -a : a;
         }
         if (at[0] && strcmp(at, "CURRENCY") != 0 && !out->symbol[0]) {
            copy_str(out->symbol, sizeof(out->symbol), txn_s(inst, "symbol"));
            out->qty = txn_d(leg, "amount");
         }
      }
   }

   out->cat = categorize(type, out->net_amount, &out->external);
}

void schwab_txn_agg_init(schwab_txn_agg_t *a) {
   memset(a, 0, sizeof(*a));
}

void schwab_txn_agg_add(schwab_txn_agg_t *a, const schwab_txn_t *t) {
   a->count++;
   a->fees += t->fee_total;
   switch (t->cat) {
      case SCHWAB_TXN_TRADE:
         a->n_trade++;
         a->net_trade += t->net_amount;
         break;
      case SCHWAB_TXN_DIVIDEND:
         a->n_dividend++;
         a->income += t->net_amount;
         break;
      case SCHWAB_TXN_DEPOSIT:
         a->n_deposit++;
         a->deposits += t->net_amount;
         break;
      case SCHWAB_TXN_WITHDRAWAL:
         a->n_withdrawal++;
         a->withdrawals += t->net_amount;
         break;
      case SCHWAB_TXN_INTERNAL:
         a->n_internal++;
         a->net_internal += t->net_amount;
         break;
      case SCHWAB_TXN_OTHER:
         a->n_other++;
         break;
   }
}
