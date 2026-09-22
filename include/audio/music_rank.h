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
 * Music search relevance ranking — pure scoring over already-fetched result
 * rows (no DB, no locks), so it can be unit-tested in isolation and kept out of
 * the DB layer's translation unit.
 */

#ifndef MUSIC_RANK_H
#define MUSIC_RANK_H

#include "audio/music_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Per-field match-quality tiers (before field weighting). These set the result
 *  ORDER only — exact hits rank first, then prefix, whole-word, and loose
 *  substring. Partial matches are NOT dropped; they sort lower so the caller
 *  (the LLM) still sees breadth and decides. */
#define MUSIC_RANK_EXACT 1000    /**< Field equals the needle (case-insensitive) */
#define MUSIC_RANK_PREFIX 700    /**< Field starts with the needle (at a word boundary) */
#define MUSIC_RANK_WORD 500      /**< Needle appears as a whole word */
#define MUSIC_RANK_SUBSTRING 200 /**< Needle appears anywhere */

/**
 * @brief Score one result row against a free-text needle
 *
 * Returns the best weighted field match: artist/title weighted highest, then
 * album, then genre. The file path is deliberately not considered. 0 means no
 * field matched.
 *
 * @param r    Result row
 * @param text Needle (NULL/empty → 0)
 * @return relevance score (0 = no match; higher = better)
 */
int music_rank_score(const music_search_result_t *r, const char *text);

/**
 * @brief Match quality of @p needle within a single field
 *
 * The tiered building block behind music_rank_score, exposed so other rankers
 * (e.g. the play/resolve picker) share one definition of exact > prefix >
 * whole-word > substring. Prefix requires the needle to end at a word boundary,
 * so "Prince" does not prefix-match "Princess".
 *
 * @return one of MUSIC_RANK_EXACT/_PREFIX/_WORD/_SUBSTRING, or 0 for no match
 */
int music_rank_field_quality(const char *field, const char *needle);

/**
 * @brief Order rows best-first for a query, capped at @p max
 *
 * Scores each of @p n rows against @p text, sorts by descending relevance
 * (stable — input order, i.e. the SQL alpha sort, breaks ties), drops only true
 * non-matches (score 0), and caps at @p max. Partial matches are kept, ranked
 * lower, so the caller sees breadth. When @p text is NULL/empty, no scoring is
 * done: the first @p max input rows are kept in order.
 *
 * @param results   Candidate rows
 * @param n         Number of candidates
 * @param text      Free-text needle (NULL/empty → keep input order)
 * @param max       Maximum indices to write (<= 0 → 0)
 * @param out_order Output index array, capacity >= min(n, max)
 * @return number of indices written to @p out_order
 */
int music_rank_select(const music_search_result_t *results,
                      int n,
                      const char *text,
                      int max,
                      int *out_order);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_RANK_H */
