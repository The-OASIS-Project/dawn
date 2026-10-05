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
 * Lightweight fuzzy name matcher — shared by surfaces that resolve a
 * user-typed name (Home Assistant entities, Discord channels, ...) against
 * a list of candidate names.  Layer 1 / Foundation: depends only on libc,
 * no DAWN state.  Extracted from the byte-identical helper that previously
 * lived static in homeassistant_service.c.
 */

#ifndef STR_FUZZY_H
#define STR_FUZZY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Score tiers returned by str_fuzzy_score(). */
#define STR_FUZZY_SCORE_EXACT 100      /* candidate == needle */
#define STR_FUZZY_SCORE_CONTAINS 80    /* candidate contains needle as substring */
#define STR_FUZZY_SCORE_TOKEN_BONUS 20 /* per whitespace-delimited needle token found */

/* Upper bound on string length compared by str_fuzzy_ratio().  Longer inputs
 * are truncated to this many characters before the edit-distance DP so the
 * two-row buffers stay stack-bounded.  Names/entity labels are well under it. */
#define STR_FUZZY_RATIO_MAXLEN 128

/**
 * @brief Lowercase @p src into @p dst (ASCII tolower), bounded by @p max_len.
 *
 * Always NUL-terminates within @p max_len.  Intended to pre-normalize both
 * candidate and needle strings before scoring.
 *
 * @param dst     Output buffer.
 * @param src     Source string.
 * @param max_len Size of @p dst (must be >= 1).
 */
void str_fuzzy_tolower(char *dst, const char *src, size_t max_len);

/**
 * @brief Score how well @p needle_lower matches @p haystack_lower.
 *
 * Both arguments MUST already be lowercased (see str_fuzzy_tolower()).
 * Tiered, allocation-free scoring:
 *   - 100  exact match
 *   -  80  haystack contains needle as a substring
 *   - +20  per whitespace-delimited needle token found in haystack
 *
 * The tiers are not mutually exclusive in spirit but return early: an exact
 * or substring hit short-circuits before token scoring.  Callers compare
 * scores across candidates and apply their own acceptance threshold.
 *
 * @param haystack_lower Candidate name, lowercased.
 * @param needle_lower   User-supplied query, lowercased.
 * @return Match score (0 = no overlap; higher is better).
 */
int str_fuzzy_score(const char *haystack_lower, const char *needle_lower);

/**
 * @brief Normalized edit-distance similarity between two lowercased strings.
 *
 * Complements str_fuzzy_score(): where the token scorer only rewards SHARED
 * whole tokens/substrings, this catches near-misses where no token is shared
 * verbatim — e.g. an ASR/typo garble of a name ("kersey" vs "curzy").  The
 * result is 100*(1 - levenshtein(a,b)/max(len_a,len_b)), clamped to [0,100]:
 *   100 = identical, 0 = maximally different (or either side empty).
 *
 * Both arguments MUST already be lowercased (see str_fuzzy_tolower()).  Inputs
 * longer than STR_FUZZY_RATIO_MAXLEN are truncated before scoring.  Callers
 * typically take max(str_fuzzy_score, str_fuzzy_ratio) and apply a threshold.
 *
 * @param a_lower First string, lowercased.
 * @param b_lower Second string, lowercased.
 * @return Similarity in [0,100]; higher is more similar.
 */
int str_fuzzy_ratio(const char *a_lower, const char *b_lower);

/* Longest word str_phonetic_key() reads (the rest is ignored). */
#define STR_PHONETIC_MAXLEN 64

/**
 * @brief A small English sound key for one word (up to a space): names that
 *        sound alike get the same key ("Cris"/"Chris", "Jon"/"John",
 *        "Sean"/"Shawn", "Stephen"/"Steven", "Katherine"/"Catherine")
 *
 * ph→f, sh→s, ch/ck/c(a,o,u,consonant)/q→k, c(e,i,y)→s, z→s, v→f,
 * g(e,i)→j; h after the first letter, and w/y after a vowel, are silent; a
 * leading kn-/wr- drops its first letter; vowels collapse to a leading 'a'
 * and are otherwise dropped; doubled sounds collapse.  ASCII letters only
 * (case ignored); UTF-8 bytes are kept as they are.  Coarse by design (Dan
 * and Don share a key): it flags names a listener could confuse.
 */
void str_phonetic_key(const char *word, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* STR_FUZZY_H */
