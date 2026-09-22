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
 * Charles Schwab /quotes response parser — structured, per-symbol extraction from a
 * batch /quotes call. Pure (no OAuth/HTTP): fixture-testable. Phase 1 carries only
 * the instrument reference name (company/fund), used to fill in position
 * descriptions the /accounts call omits for equities. The extended-hours price
 * overlay extends schwab_quote_t with quote/regular/extended fields — additively,
 * without reshaping this struct or the parser's control flow.
 */

#ifndef SCHWAB_QUOTES_H
#define SCHWAB_QUOTES_H

#include "tools/schwab_portfolio.h" /* SCHWAB_SYMBOL_MAX, SCHWAB_DESC_MAX */

struct json_object;

/** One symbol's data extracted from a Schwab /quotes response. Which fields are
 * populated depends on the `fields` requested: reference → description/asset_type;
 * quote → last/net_change/net_change_pct. Unrequested/absent fields stay zero/"". */
typedef struct {
   char symbol[SCHWAB_SYMBOL_MAX];
   char description[SCHWAB_DESC_MAX]; /* reference.description (company/fund name); "" if absent */
   schwab_asset_type_t asset_type;    /* from assetMainType (SCHWAB_ASSET_EQUITY if absent) */
   double last;                       /* quote.lastPrice */
   double net_change;                 /* quote.netChange (vs prior close) */
   double net_change_pct;             /* quote.netPercentChange (signed %) */
   bool quotable; /* true = Schwab returned quote data; false = a stored watchlist
                   * symbol Schwab did not return (a bad/unknown ticker) */
   /* Extended-hours overlay — populated only when the caller requested the
    * regular+extended fields (i.e. during an active pre/post-market window). Change
    * is measured against the regular-session close, matching a broker's AH display. */
   bool has_ext;          /* true = an extended block with a usable last price */
   double ext_last;       /* extended.lastPrice (the pre/post-market last trade) */
   double ext_change;     /* ext_last − regular-session close */
   double ext_change_pct; /* ext_change as a signed % of the regular close */
} schwab_quote_t;

/**
 * Parse a Schwab /quotes response (an object keyed by symbol) into @p out (up to
 * @p max entries); @p n_out receives the count written. The top-level "errors"
 * block (invalid-symbol reports) and any non-object member are skipped. Pure — no
 * HTTP/OAuth. Returns SUCCESS, or FAILURE only on a NULL/non-object @p root or
 * invalid args (in which case @p n_out is set to 0).
 */
int schwab_quotes_parse(struct json_object *root, schwab_quote_t *out, int max, int *n_out);

/**
 * Extract the symbols Schwab explicitly flagged invalid (errors.invalidSymbols) from
 * a /quotes response into @p out (up to @p max); @p n_out receives the count. This
 * is the DEFINITIVE bad-ticker signal — a symbol absent from the response but NOT in
 * this list is merely un-quoted (e.g. a transient miss), not invalid. Returns
 * SUCCESS, or FAILURE on a NULL/non-object root or bad args.
 */
int schwab_quotes_parse_invalid(struct json_object *root,
                                char (*out)[SCHWAB_SYMBOL_MAX],
                                int max,
                                int *n_out);

/**
 * Build the stocks_watch_update PAYLOAD (owned json_object; caller json_object_put)
 * from @p n quotes: { status, as_of, market, symbols:[{symbol, description?,
 * asset_type, price, day_change, day_change_pct, ext?:{price,change,change_pct}}] }.
 * @p status/@p market are the wire tokens; @p q may be NULL when @p n == 0 (empty
 * watchlist → symbols:[]). The per-row `ext` object is emitted only for a row whose
 * has_ext is set (the server populates it only during an active pre/post window).
 */
struct json_object *schwab_quotes_watch_payload_jobj(const schwab_quote_t *q,
                                                     int n,
                                                     const char *status,
                                                     const char *market,
                                                     int64_t as_of);

/**
 * Emit the shared per-row extended-hours object {price, change, change_pct} into an
 * existing symbol/position row @p o. Both the watchlist and portfolio payloads use
 * this so the wire shape stays identical. The caller decides whether a row carries
 * ext data (its has_ext flag); this only serializes it.
 */
void schwab_quotes_add_ext_obj(struct json_object *o,
                               double ext_last,
                               double ext_change,
                               double ext_change_pct);

#endif /* SCHWAB_QUOTES_H */
