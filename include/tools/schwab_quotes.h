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

/** One symbol's data extracted from a Schwab /quotes response. */
typedef struct {
   char symbol[SCHWAB_SYMBOL_MAX];
   char description[SCHWAB_DESC_MAX]; /* reference.description (company/fund name); "" if absent */
} schwab_quote_t;

/**
 * Parse a Schwab /quotes response (an object keyed by symbol) into @p out (up to
 * @p max entries); @p n_out receives the count written. The top-level "errors"
 * block (invalid-symbol reports) and any non-object member are skipped. Pure — no
 * HTTP/OAuth. Returns SUCCESS, or FAILURE only on a NULL/non-object @p root or
 * invalid args (in which case @p n_out is set to 0).
 */
int schwab_quotes_parse(struct json_object *root, schwab_quote_t *out, int max, int *n_out);

#endif /* SCHWAB_QUOTES_H */
