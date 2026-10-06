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
 * Per-user stocks watchlist store — a small set of arbitrary (not-held) tickers the
 * WebUI stocks panel tracks, persisted in auth.db (stocks_watchlist). Thin CRUD over
 * the shared auth-db handle; symbols are normalized/validated to [A-Z0-9.].
 */

#ifndef SCHWAB_WATCHLIST_H
#define SCHWAB_WATCHLIST_H

#include "tools/schwab_portfolio.h" /* SCHWAB_SYMBOL_MAX */

/* Cap per user — also the batch /quotes ceiling (one call covers the whole list). */
#define SCHWAB_WATCHLIST_MAX 25

/** Outcome of an add, so a caller can report WHY a symbol was rejected. */
typedef enum {
   SCHWAB_WATCH_ADDED = 0, /**< newly added */
   SCHWAB_WATCH_DUPLICATE, /**< already on the list (benign) */
   SCHWAB_WATCH_INVALID,   /**< failed [A-Z0-9.] validation (also the error sentinel) */
   SCHWAB_WATCH_FULL       /**< list already at SCHWAB_WATCHLIST_MAX */
} schwab_watch_add_t;

/**
 * Add @p symbol to @p user_id's watchlist (idempotent; normalized to upper-case
 * [A-Z0-9.]). NOTE: this validates only the symbol's characters, not that it's a
 * real tradable ticker — an unknown-but-well-formed symbol (a typo) is stored and
 * only surfaces as un-quotable when the panel fetches it.
 */
schwab_watch_add_t schwab_watchlist_add(int user_id, const char *symbol);

/** Remove @p symbol from @p user_id's watchlist. SUCCESS even if it wasn't present. */
int schwab_watchlist_remove(int user_id, const char *symbol);

/**
 * Upper-case + validate a ticker's CHARACTERS into @p out ([A-Z0-9.] only, non-empty,
 * never truncated). Returns SUCCESS if well-formed, FAILURE otherwise. (Character
 * validity only — not whether it's a real tradable ticker.)
 */
int schwab_watchlist_normalize(const char *in, char *out, int out_len);

/**
 * List @p user_id's watchlist symbols (oldest-added first) into @p out (up to
 * @p max rows of SCHWAB_SYMBOL_MAX); @p n_out receives the count. Returns SUCCESS
 * or FAILURE (n_out set to 0 on failure).
 */
int schwab_watchlist_list(int user_id, char (*out)[SCHWAB_SYMBOL_MAX], int max, int *n_out);

/**
 * Invalidate a user's live WebUI watchlist cache after an edit made outside the
 * WebUI (e.g. the voice tool), so an open panel re-fetches the new set. Weak no-op
 * default (Layer 3); the WebUI layer (webui_stocks.c) provides the strong override.
 */
void webui_stocks_watchlist_invalidate(int user_id);

#endif /* SCHWAB_WATCHLIST_H */
