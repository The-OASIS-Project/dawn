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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Memory Similarity API
 *
 * Provides text normalization, hashing, and similarity scoring for
 * fact deduplication. Uses a multi-stage algorithm:
 *
 * 1. Normalization: Lowercase, remove stopwords, sort words alphabetically
 * 2. Hash check: FNV-1a hash for O(1) exact duplicate detection
 * 3. Jaccard similarity: For fuzzy matching when hashes differ
 *
 * The normalized form is used for comparison but original text is preserved.
 */

#ifndef MEMORY_SIMILARITY_H
#define MEMORY_SIMILARITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Jaccard similarity threshold for considering facts as duplicates */
#define MEMORY_SIMILARITY_THRESHOLD 0.7f

/* Maximum words to consider for Jaccard similarity */
#define MEMORY_MAX_WORDS 64

/* =============================================================================
 * Text Normalization
 * ============================================================================= */

/**
 * @brief Normalize text for comparison
 *
 * Transforms text into a canonical form for duplicate detection:
 * - Convert to lowercase
 * - Remove common English stopwords
 * - Sort remaining words alphabetically
 * - Collapse whitespace
 *
 * @param text Input text to normalize
 * @param out_normalized Output: normalized text (caller allocates)
 * @param max_len Maximum output buffer size
 * @param len_out Output: number of characters written (may be NULL)
 * @return 0 on success, 1 on error
 */
int memory_normalize_text(const char *text, char *out_normalized, size_t max_len, int *len_out);

/* =============================================================================
 * Hashing
 * ============================================================================= */

/**
 * @brief Compute FNV-1a hash of normalized text
 *
 * Uses 32-bit FNV-1a for fast exact duplicate detection.
 * The text should be pre-normalized for consistent results.
 *
 * @param normalized_text Pre-normalized text to hash
 * @return 32-bit hash value
 */
uint32_t memory_hash_text(const char *normalized_text);

/**
 * @brief @p text as the exact-text checks compare it: ASCII case and runs of
 *        spacing folded, leading and trailing spacing and trailing full stops
 *        trimmed (the same words in the same order, so "not" and word order
 *        still count).  Heap; NULL for NULL or on allocation failure.
 */
char *memory_text_fold(const char *text);

/**
 * @brief Normalize text and compute hash in one step
 *
 * Convenience function that normalizes internally and returns hash.
 *
 * @param text Raw text to process
 * @return Hash of normalized text, or 0 on error
 */
uint32_t memory_normalize_and_hash(const char *text);

/* =============================================================================
 * Similarity Scoring
 * ============================================================================= */

/**
 * @brief Compute Jaccard similarity between two texts
 *
 * Calculates |intersection| / |union| of word sets.
 * Both texts are normalized before comparison.
 *
 * @param text_a First text
 * @param text_b Second text
 * @return Similarity score from 0.0 (no overlap) to 1.0 (identical)
 */
float memory_jaccard_similarity(const char *text_a, const char *text_b);

/**
 * @brief Check if two facts are duplicates
 *
 * First checks hash equality (fast path), then falls back to
 * Jaccard similarity for fuzzy matching.
 *
 * @param text_a First fact text
 * @param text_b Second fact text
 * @param threshold Similarity threshold (use MEMORY_SIMILARITY_THRESHOLD)
 * @return true if facts are considered duplicates
 */
bool memory_is_duplicate(const char *text_a, const char *text_b, float threshold);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_SIMILARITY_H */
