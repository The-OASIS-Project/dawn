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
 * Memory injection filter — blocks prompt-injection payloads from being
 * stored as facts or preferences in the persistent memory system.
 */

#ifndef MEMORY_FILTER_H
#define MEMORY_FILTER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Normalize text for injection pattern matching.
 *
 * Produces an ASCII-only normalized form for pattern matching:
 * - Strips zero-width/invisible chars and Unicode tag characters
 * - Replaces line/paragraph separators (U+2028/2029) with space
 * - Maps Cyrillic/Greek homoglyphs to ASCII equivalents
 * - Strips Latin-1 accents (U+00C0-U+00FF) to base letters
 * - Maps fullwidth ASCII (U+FF01-FF5E) to normal ASCII
 * - Collapses whitespace and lowercases
 * - Drops remaining non-ASCII characters
 * Caller must free() the returned string.
 *
 * @param text  Input text (UTF-8).
 * @return Heap-allocated normalized string, or NULL on error/NULL input.
 */
char *memory_filter_normalize(const char *text);

/**
 * @brief Check whether text contains a blocked injection pattern.
 *
 * Normalizes internally then checks against the blocklist and
 * structural attack detectors (ReAct co-occurrence, etc.).
 *
 * @param text  Input text to check.
 * @return true if the text should be REJECTED (contains injection).
 */
bool memory_filter_check(const char *text);

/**
 * @brief Check text against ONLY the high-confidence injection-command subset.
 *
 * A narrowed variant of memory_filter_check() that scans just the AI-directed
 * override / jailbreak / role-marker / memory-poisoning patterns — NOT the
 * credential, vocabulary, markdown-link, base64, or ReAct patterns.  Intended
 * for long LLM/web-derived text (e.g. a background-job research result) that
 * legitimately contains credential/API vocabulary as subject matter, where the
 * full blocklist false-positives heavily.  Still catches the classic
 * "ignore your instructions / act as DAN / <|im_start|>" attack shape.
 *
 * @param text  Input text to check.
 * @return true if the text should be REJECTED (contains an injection command).
 */
bool memory_filter_check_injection_commands(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_FILTER_H */
