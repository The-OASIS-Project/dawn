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
 * Music search relevance matching — pure, punctuation-insensitive token scoring
 * (no DB, no locks), so it can be unit-tested in isolation. music_db_query.c exposes
 * these as SQLite functions so the SQL predicate, the ranking and the result
 * total all come from this one definition of "match".
 */

#ifndef MUSIC_RANK_H
#define MUSIC_RANK_H

#include <stdbool.h>
#include <stddef.h>

#include "audio/music_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Per-field match-quality tiers (before field weighting), compared on FOLDED text
 *  (lowercase; apostrophes dropped; '&' = "and"; other punctuation = space), so
 *  "Rockin the Suburbs" is an exact match for "Rockin' the Suburbs". Tiers set the
 *  result ORDER; a field scores 0 only when some query token is absent from it. */
#define MUSIC_RANK_EXACT 1000      /**< Field equals the needle */
#define MUSIC_RANK_PREFIX 700      /**< Field starts with the needle at a word boundary */
#define MUSIC_RANK_WORD 500        /**< Needle appears as a contiguous whole-word phrase */
#define MUSIC_RANK_ALL_WORDS 400   /**< Every needle token appears as a whole word, any order */
#define MUSIC_RANK_SUBSTRING 200   /**< Needle appears contiguously, but mid-word */
#define MUSIC_RANK_ALL_PRESENT 100 /**< Every token appears, some only mid-word */

/** Row-level floor tiers when no single field holds every token but the row's
 *  fields together do (e.g. "Ben Folds Rockin" = artist + title). Deliberately
 *  below the lowest weighted single-field score, so any single-field match ranks
 *  above any cross-field one. */
#define MUSIC_RANK_CROSS_WORDS 50   /**< Every token a whole word in some field */
#define MUSIC_RANK_CROSS_PRESENT 25 /**< Every token present in some field */

/** Lowest free-text tier: a long query (3+ significant tokens) with all but one
 *  token found somewhere in the row and at least one as a whole word — tolerates
 *  one typo or stray word ("Rockin the Suburbz Ben"). music_db only falls back to
 *  it when no row matches strictly, so partials never pad a real result set.
 *  Free text only; the fielded filters stay strict. */
#define MUSIC_RANK_PARTIAL 10

/** Minimum field quality for the fielded artist:/title:/album: filters: word-
 *  boundary strict (so artist:"Prince" excludes "Princeton"), order-insensitive. */
#define MUSIC_RANK_FIELD_MIN MUSIC_RANK_ALL_WORDS

/** Score bonus for an artist match in music_rank_pick_best(); above the highest
 *  title tier so a matching artist always dominates. */
#define MUSIC_RANK_PICK_ARTIST_BONUS 2000

/** Tokens of this length or shorter count only as whole words — "dc" or "n" as a
 *  mid-word hit would match nearly every row. */
#define MUSIC_RANK_SHORT_TOKEN 2

/** Tokens considered per needle; further tokens are ignored. */
#define MUSIC_RANK_MAX_TOKENS 16

/** Folded-text buffer size. Fields are < AUDIO_METADATA_STRING_MAX bytes; '&'
 *  expands to " and ", so leave room (overlong input is truncated safely). */
#define MUSIC_RANK_FOLD_MAX (AUDIO_METADATA_STRING_MAX * 2)

/** A needle folded and tokenized once, reusable across many rows. */
typedef struct {
   char folded[MUSIC_RANK_FOLD_MAX];
   size_t len;
   int ntok;
   int nsig; /**< tokens that must match (non-filler); all tokens if every one is filler */
   struct {
      unsigned short off;
      unsigned short len;
      unsigned char sig; /**< 1 = required; 0 = filler ("the", "by", "album", ...) */
   } tok[MUSIC_RANK_MAX_TOKENS];
} music_rank_needle_t;

/**
 * @brief Fold text for matching
 *
 * ASCII lowercase; apostrophes (' ` and U+2018/U+2019) removed; '&' → "and";
 * every other ASCII non-alphanumeric, plus U+2010-2015 dashes, U+201C/U+201D
 * quotes, U+2026 ellipsis and U+00A0 no-break space → a single space;
 * leading/trailing space trimmed. Other bytes >= 0x80 (UTF-8) are kept as word
 * characters as-is — accents are NOT folded, so "Beyonce" does not match "Beyoncé".
 *
 * @return folded length (always NUL-terminated when @p cap > 0)
 */
size_t music_rank_fold(const char *in, char *out, size_t cap);

/**
 * @brief Fold + tokenize a needle once for repeated scoring
 *
 * A NULL/empty needle (or one that folds to nothing) yields len 0 / ntok 0,
 * which scores 0 against everything. Filler words ("the", "a", "by", "and",
 * "song", "album", "feat", ...) still count toward exact/phrase matching but are
 * not REQUIRED to be present, so "Rockin the Suburbs by Ben Folds" still matches.
 */
void music_rank_needle_prepare(const char *text, music_rank_needle_t *out);

/**
 * @brief Prepare a needle, optionally requiring every token (no filler relaxation)
 *
 * @p all_required = true is the precision mode used by the fielded artist:/
 * title:/album: filters, so artist:"The Band" doesn't match "Dave Matthews Band".
 */
void music_rank_needle_prepare_ex(const char *text, bool all_required, music_rank_needle_t *out);

/**
 * @brief Match quality of a prepared needle within one (raw) field
 * @return one of the MUSIC_RANK_* field tiers, or 0 when a token is missing
 */
int music_rank_field_quality_prepared(const char *field, const music_rank_needle_t *n);

/**
 * @brief Match quality of @p needle within a single field (folds both)
 *
 * The tiered building block behind the row score, exposed so other rankers
 * (the play/resolve picker) and the fielded filters share one definition.
 *
 * @return one of the MUSIC_RANK_* field tiers, or 0 for no match
 */
int music_rank_field_quality(const char *field, const char *needle);

/**
 * @brief Field quality with EVERY needle word required (fielded-filter mode)
 * @return one of the MUSIC_RANK_* field tiers, or 0 for no match
 */
int music_rank_field_match(const char *field, const char *needle);

/**
 * @brief Row relevance from its four text fields against a prepared needle
 *
 * Best weighted single-field quality (artist/title 100%, album 75%, genre 60%);
 * if no single field matches, the cross-field floor tiers apply, then the
 * PARTIAL tier for a long query missing one token; 0 means no match. The file
 * path is never considered.
 * NULL fields are treated as empty.
 */
int music_rank_score_fields(const char *artist,
                            const char *title,
                            const char *album,
                            const char *genre,
                            const music_rank_needle_t *n);

/**
 * @brief Score one result row against a free-text needle (convenience wrapper)
 * @return relevance score (0 = no match; higher = better)
 */
int music_rank_score(const music_search_result_t *r, const char *text);

/**
 * @brief Pick the most relevant result from a candidate set (resolver helper)
 *
 * Ranks by title closeness (the field tiers) with a dominant bonus when @p artist
 * is given and matches the candidate's artist (whole words, punctuation-
 * insensitive), breaking ties toward the shorter title.
 *
 * @param results     Candidate array
 * @param count       Number of candidates
 * @param title_query The track title (or whole query) to match against
 * @param artist      Optional artist to prefer (NULL when the query has none)
 * @return index of the best candidate in [0, count), or 0 if count <= 0
 */
int music_rank_pick_best(const music_search_result_t *results,
                         int count,
                         const char *title_query,
                         const char *artist);

/**
 * @brief Grouping key for an album name: folded, with trailing edition
 *        qualifiers stripped
 *
 * Removes trailing "(…)" / "[…]" groups and " - …" suffixes that name an edition
 * or format (edition, deluxe, expanded, remaster…, explicit, clean, bonus,
 * anniversary, reissue, EP, single, disc/disk/CD N, booklet), plus a " - <artist>" suffix
 * repeating @p artist, so "Rockin' the Suburbs (Expanded Edition)", "Rockin' The
 * Suburbs" and "Speed Graphic (EP)" / "Speed Graphic" share keys. Qualifiers that
 * name different content ("(Live)", "(Taylor's Version)") are kept. If stripping
 * would leave nothing, the whole name is used.
 *
 * @param album  Album name (NULL → empty key)
 * @param artist Row artist, for the " - <artist>" suffix (may be NULL)
 */
void music_rank_album_key(const char *album, const char *artist, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_RANK_H */
