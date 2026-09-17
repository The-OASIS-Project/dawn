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
 * Charles Schwab transaction classifier — pure, unit-testable mapping of one
 * (undocumented) /transactions row into a display/aggregation record. Schwab's
 * outer `type` covers economically opposite events, so classification is
 * leg-and-sign based, not type alone (see schwab_txn.c for the pinned schema).
 */

#ifndef SCHWAB_TXN_H
#define SCHWAB_TXN_H

#include <json-c/json.h>
#include <stdbool.h>

typedef enum {
   SCHWAB_TXN_TRADE = 0,  /**< buy/sell of a security */
   SCHWAB_TXN_DIVIDEND,   /**< dividend or interest income */
   SCHWAB_TXN_DEPOSIT,    /**< external cash in (ACH/wire/cash receipt) */
   SCHWAB_TXN_WITHDRAWAL, /**< external cash out */
   SCHWAB_TXN_INTERNAL,   /**< journal / money-market sweep / internal move */
   SCHWAB_TXN_OTHER,      /**< anything unrecognized */
} schwab_txn_cat_t;

typedef struct {
   schwab_txn_cat_t cat;
   bool external;     /**< external cash flow (deposit/withdrawal) — for a
                           future money-weighted-return reconstruction */
   char date[11];     /**< YYYY-MM-DD (from `time`) */
   char time_key[32]; /**< full `time` string, for chronological sort */
   char symbol[16];   /**< security symbol when a non-cash leg carries one */
   char memo[80];     /**< description (dividend payer / journal note); may be empty */
   double qty;        /**< signed share quantity for a trade (0 otherwise) */
   double net_amount; /**< cash impact of the whole transaction */
   double fee_total;  /**< sum of fee legs (>= 0), informational */
   char rawtype[32];  /**< the raw Schwab `type` string */
} schwab_txn_t;

typedef struct {
   int count; /**< rows classified */
   int n_trade, n_dividend, n_deposit, n_withdrawal, n_internal, n_other;
   double net_trade;    /**< sum of trade net cash (buys negative) */
   double income;       /**< dividends + interest received */
   double deposits;     /**< external cash in (>= 0) */
   double withdrawals;  /**< external cash out (<= 0) */
   double fees;         /**< total fees across all rows (>= 0) */
   double net_internal; /**< net of internal journal/sweep moves */
} schwab_txn_agg_t;

/** Classify one transaction row. Defensive: missing fields degrade to blank/0. */
void schwab_txn_classify(struct json_object *row, schwab_txn_t *out);

/** Human category label. */
const char *schwab_txn_cat_name(schwab_txn_cat_t c);

/** Copy @p in to @p out (bounded by @p out_len), masking any digit token that
 * carries >= 5 digits — including groups split by a single ' '/'-' separator
 * ("1234 5678", "123-45-6789") — to a single '#', so a free-text memo can't leak
 * an account/routing/SSN-shaped number. Shorter numbers (dates, share counts)
 * pass through. @p in may be NULL. Output is always NUL-terminated. */
void schwab_txn_mask_numbers(const char *in, char *out, size_t out_len);

/** True for internal journal/sweep rows that are hidden from the "all" listing
 * by default (still counted in aggregates). */
bool schwab_txn_is_internal(const schwab_txn_t *t);

void schwab_txn_agg_init(schwab_txn_agg_t *a);
void schwab_txn_agg_add(schwab_txn_agg_t *a, const schwab_txn_t *t);

#endif /* SCHWAB_TXN_H */
