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
 */

#ifndef SCHWAB_SERVICE_H
#define SCHWAB_SERVICE_H

#include <stdbool.h>

#include "tools/schwab_client.h"    /* schwab_rc_t */
#include "tools/schwab_portfolio.h" /* schwab_portfolio_t */
#include "tools/schwab_quotes.h"    /* schwab_quote_t */
#include "tools/schwab_stats.h"     /* schwab_history_stats + types */

/**
 * Build an LLM-facing quote summary for one or more comma/space-separated
 * symbols. Returns an owned string (caller frees); failure paths are prefixed
 * with TOOL_RESULT_ERROR_MARK. @p user_id owns the Schwab OAuth link.
 */
char *schwab_service_quote(int user_id, const char *symbols_csv);

/**
 * Build an LLM-facing portfolio summary across all of the user's Schwab
 * accounts (positions + balances + combined total). When @p accounts_only is
 * true, balances only (no per-position detail). Returns an owned string;
 * failures prefixed with TOOL_RESULT_ERROR_MARK.
 */
char *schwab_service_portfolio(int user_id, bool accounts_only);

/**
 * Structured portfolio snapshot for the WebUI stocks panel (one /accounts call,
 * parsed into @p out instead of prose). Returns the schwab_rc_t so the caller can
 * map it to a wire status; on non-OK, @p out is zeroed. Free @p out with
 * schwab_portfolio_free(). The ext_hours /quotes overlay is not yet applied
 * (recorded only). @p user_id owns the Schwab OAuth link.
 */
schwab_rc_t schwab_service_portfolio_snapshot(int user_id, bool ext_hours, schwab_portfolio_t *out);

/**
 * Epoch seconds at which @p user_id's Schwab refresh token lapses (link time +
 * SCHWAB_REFRESH_LIFETIME_DAYS), so the panel can warn before it expires. Returns 0
 * when not linked or the link time is unknown (caller omits the hint).
 */
int64_t schwab_service_link_expires_at(int user_id);

/**
 * Batch-quote up to SCHWAB_MAX_SYMBOLS @p syms into @p out (up to @p max; @p n_out
 * set). URL-safety is enforced internally. Requests quote+reference; when @p ext_hours
 * is set (an active pre/post window) it also requests regular+extended so the parser
 * fills each quote's ext overlay. Used by the watchlist.
 */
schwab_rc_t schwab_service_quotes(int user_id,
                                  const char *const *syms,
                                  int n_syms,
                                  bool ext_hours,
                                  schwab_quote_t *out,
                                  int max,
                                  int *n_out);

/**
 * Validate @p syms against Schwab: fills @p invalid (up to @p inv_max) with the
 * symbols Schwab explicitly flagged (errors.invalidSymbols) and sets @p n_invalid.
 * A symbol NOT flagged is treated as valid (returned quote, or ambiguous-but-not-
 * rejected). Returns the schwab_rc_t; on non-OK, @p n_invalid is 0 (validation
 * couldn't run — the caller decides its fallback).
 */
schwab_rc_t schwab_service_validate_symbols(int user_id,
                                            const char *const *syms,
                                            int n_syms,
                                            char (*invalid)[SCHWAB_SYMBOL_MAX],
                                            int inv_max,
                                            int *n_invalid);

/**
 * Build an LLM-facing price-history answer for ONE symbol. @p range is one of
 * 1mo/3mo/6mo/1y/5y/ytd (default 1y); @p interval is daily/weekly/monthly
 * (default per range; an illegal range+interval is coerced); @p data is
 * summary/series/raw (default summary). @p start / @p end are optional
 * YYYY-MM-DD dates: when @p start is non-empty they override @p range with an
 * explicit window (@p end defaults to today). Empty strings take the defaults.
 * Returns an owned string; failures prefixed with TOOL_RESULT_ERROR_MARK.
 */
char *schwab_service_history(int user_id,
                             const char *symbol,
                             const char *range,
                             const char *interval,
                             const char *data,
                             const char *start,
                             const char *end);

/**
 * Build an LLM-facing fundamentals (valuation) snapshot for one or more
 * comma-separated symbols. Returns an owned string; failures prefixed with
 * TOOL_RESULT_ERROR_MARK.
 */
char *schwab_service_fundamentals(int user_id, const char *symbols_csv);

/**
 * Build an LLM-facing list of recent account activity (trades, dividends/
 * interest, deposits/withdrawals, fees) across the user's Schwab accounts, with
 * per-category aggregates. Schwab serves only a recent (~1-year) window and no
 * historical valuations, so this is activity, not lifetime performance.
 *
 * @p symbol optionally filters to one ticker (won't reliably match dividend
 * rows, which carry no symbol field). @p start / @p end are optional YYYY-MM-DD
 * bounds; empty @p start defaults to the last ~60 days, @p end defaults to
 * today. @p type is a user-language category: trades / dividends / deposits /
 * withdrawals / fees / all (default). Returns an owned string; failures prefixed
 * with TOOL_RESULT_ERROR_MARK.
 */
char *schwab_service_transactions(int user_id,
                                  const char *symbol,
                                  const char *start,
                                  const char *end,
                                  const char *type);

#endif /* SCHWAB_SERVICE_H */
