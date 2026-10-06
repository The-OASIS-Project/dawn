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
 * Charles Schwab portfolio — structured snapshot parser + JSON serializer for the
 * WebUI stocks panel. Pure (no OAuth/HTTP): parses a Schwab /accounts?fields=
 * positions response into a struct and serializes it to the stocks_portfolio_update
 * wire shape. Unit-testable against captured/synthetic JSON.
 */

#ifndef SCHWAB_PORTFOLIO_H
#define SCHWAB_PORTFOLIO_H

#include <stdbool.h>
#include <stdint.h>

struct json_object;

#define SCHWAB_SYMBOL_MAX 24     /* OCC option symbols run ~21 chars */
#define SCHWAB_DESC_MAX 96       /* instrument.description ("Apple Inc."); long fund names fit */
#define SCHWAB_ACCT_LABEL_MAX 16 /* masked "…1234" */
#define SCHWAB_ACCT_TYPE_MAX 16  /* "CASH" / "MARGIN" */

/** Asset class, mapped from Schwab instrument.assetType → Aurora's badge enum. */
typedef enum {
   SCHWAB_ASSET_EQUITY = 0,
   SCHWAB_ASSET_ETF,
   SCHWAB_ASSET_OPTION,
   SCHWAB_ASSET_MUTUAL_FUND,
   SCHWAB_ASSET_CASH_EQUIVALENT,
   SCHWAB_ASSET_OTHER
} schwab_asset_type_t;

typedef struct {
   char symbol[SCHWAB_SYMBOL_MAX];
   char description[SCHWAB_DESC_MAX]; /* instrument.description; "" if absent */
   schwab_asset_type_t asset_type;
   double qty;   /* net long − short */
   double price; /* last: marketValue / qty (0 when qty == 0) */
   double market_value;
   double unrealized_pl;     /* longOpenProfitLoss */
   double unrealized_pl_pct; /* vs cost basis (value − unrealized_pl); 0 if undefined */
   double day_change;        /* currentDayProfitLoss */
   double day_change_pct;    /* vs prior close (value − day_change); 0 if undefined */
   /* Extended-hours overlay — /accounts omits it; the snapshot enriches these from a
    * /quotes?fields=quote,regular,extended lookup only during an active pre/post
    * window. Change is per-share, measured against the regular-session close. */
   bool has_ext;          /* true = an extended last price was merged in */
   double ext_last;       /* extended.lastPrice (per-share pre/post-market last) */
   double ext_change;     /* ext_last − regular-session close (per share) */
   double ext_change_pct; /* ext_change as a signed % of the regular close */
} schwab_position_t;

typedef struct {
   char label[SCHWAB_ACCT_LABEL_MAX]; /* masked account number, never the full number */
   char acct_type[SCHWAB_ACCT_TYPE_MAX];
   double value;             /* liquidationValue (authoritative account total) */
   double cash;              /* cashBalance */
   double day_change;        /* Σ position day_change */
   double unrealized_pl;     /* Σ position unrealized_pl */
   double unrealized_pl_pct; /* vs Σ cost basis */
   schwab_position_t *positions;
   int position_count;
} schwab_account_t;

typedef struct {
   schwab_account_t *accounts;
   int account_count;
   double total_value;          /* Σ liquidationValue */
   double total_day_change;     /* Σ account day_change */
   double total_day_change_pct; /* vs Σ prior-close basis */
   double total_unrealized_pl;  /* Σ account unrealized_pl */
   double total_unrealized_pl_pct;
   int64_t as_of;  /* unix seconds when this snapshot was fetched */
   bool ext_hours; /* whether extended-hours pricing was requested */
} schwab_portfolio_t;

/** Map a Schwab assetType string (e.g. "EQUITY", "OPTION") to the badge enum. */
schwab_asset_type_t schwab_asset_type_from_str(const char *schwab_asset_type);

/** Lower-case wire token for an asset type ("equity", "etf", …). Never NULL. */
const char *schwab_asset_type_str(schwab_asset_type_t t);

/**
 * Parse a Schwab /accounts?fields=positions response (a JSON array) into @p out.
 * Allocates @p out->accounts and each account's positions. On success @p out
 * owns that memory; free with schwab_portfolio_free(). @p out->as_of is left 0
 * (the caller stamps it). Returns SUCCESS or FAILURE; on FAILURE @p out is
 * zeroed and owns nothing.
 */
int schwab_portfolio_parse(struct json_object *accounts_root, schwab_portfolio_t *out);

/**
 * Build the stocks_portfolio_update PAYLOAD as an owned json_object (caller
 * json_object_put()s it). @p status is "ok"|"not_connected"|"rate_limited"|
 * "token_expired"|"error"; @p market is "regular"|"pre"|"post"|"closed". A NULL
 * @p p emits a state-only payload (no numeric fields) for token_expired / cold
 * not_connected. The caller wraps it in the {type, payload} frame envelope.
 */
struct json_object *schwab_portfolio_payload_jobj(const schwab_portfolio_t *p,
                                                  const char *status,
                                                  const char *market,
                                                  bool ext_hours,
                                                  int64_t link_expires_at);

/** As above, serialized to an owned JSON string (caller frees). */
char *schwab_portfolio_to_json(const schwab_portfolio_t *p,
                               const char *status,
                               const char *market,
                               bool ext_hours,
                               int64_t link_expires_at);

/** Free allocated arrays and zero the struct. Safe on a zeroed/NULL struct. */
void schwab_portfolio_free(schwab_portfolio_t *p);

#endif /* SCHWAB_PORTFOLIO_H */
