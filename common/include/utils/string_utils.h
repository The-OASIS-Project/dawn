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
 * String Utilities - Common string functions shared across tools
 *
 * This module provides portable string utility functions that may not be
 * available on all platforms or that need consistent behavior.
 */

#ifndef DAWN_COMMON_STRING_UTILS_H
#define DAWN_COMMON_STRING_UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Safe bounded string copy with guaranteed null-termination.
 *
 * Unlike strncpy, this always null-terminates the destination and does not
 * pad the tail with zeros. Portable replacement for strlcpy; the canonical
 * explicit-size bounded copy for the project. Prefer safe_strscpy() (below)
 * when the destination is a fixed-size array — it derives the size and rejects
 * a pointer destination at compile time.
 *
 * NULL-safe: a NULL dest (or size 0) is a no-op; a NULL src yields an empty
 * dest. Thread-safe (modifies only the dest buffer).
 *
 * @param dest Destination buffer (may be NULL).
 * @param src Source string, null-terminated (may be NULL).
 * @param size Capacity of the destination buffer (copies at most size-1).
 * @return The length of @p src (strlcpy semantics): a return value >= @p size
 *         means the copy was TRUNCATED. Returns 0 for a NULL/empty src or a
 *         no-op call. Callers that don't care may ignore it.
 */
static inline size_t safe_strncpy(char *dest, const char *src, size_t size) {
   if (dest == NULL || size == 0) {
      return 0;
   }
   if (src == NULL) {
      dest[0] = '\0';
      return 0;
   }
   size_t srclen = strlen(src);
   size_t copylen = srclen < size ? srclen : size - 1;
   memcpy(dest, src, copylen);
   dest[copylen] = '\0';
   return srclen;
}

/*
 * safe_strscpy(dst, src) — the preferred bounded copy for a fixed-size array.
 *
 * Derives the capacity from sizeof(dst) so the size can't be mis-passed, and
 * rejects a POINTER destination at compile time (a pointer would otherwise copy
 * only sizeof(pointer)-1 bytes). Returns strlcpy semantics like safe_strncpy:
 * a return >= sizeof(dst) means truncation.
 *
 * Use safe_strncpy() directly when the destination is a pointer with a known
 * capacity (a size parameter), where sizeof(dst) would be wrong.
 *
 * Two implementations give the same guarantee:
 *  - C: a macro using the GCC/Clang array-detection idiom (typeof +
 *    __builtin_types_compatible_p). DAWN_MUST_BE_ARRAY expands to 0 for an array
 *    and to an ill-formed (negative-width bitfield) type otherwise, failing the
 *    build. DAWN_-prefixed (not reserved __names) to stay collision-safe in this
 *    widely-included header.
 *  - C++: an array-reference template (char (&)[N]) — a pointer won't bind, so it
 *    yields the same compile-time rejection without the GNU builtins, which the
 *    C++ front end does not accept.
 */
#ifdef __cplusplus
} /* extern "C" — a function template cannot have C language linkage */
template<size_t N> static inline size_t safe_strscpy(char (&dst)[N], const char *src) {
   return safe_strncpy(dst, src, N);
}
extern "C" {
#else
#define DAWN_SAME_TYPE(a, b) __builtin_types_compatible_p(__typeof__(a), __typeof__(b))
#define DAWN_MUST_BE_ARRAY(a) \
   (sizeof(struct { int _dummy[1 - 2 * !!(DAWN_SAME_TYPE((a), &(a)[0]))]; }) * 0)
#define safe_strscpy(dst, src) safe_strncpy((dst), (src), sizeof(dst) + DAWN_MUST_BE_ARRAY(dst))
#endif

/**
 * @brief Sanitize string for safe use in JSON and LLM APIs
 *
 * Removes or replaces characters that cause problems with JSON parsing or
 * LLM API calls:
 * - Invalid UTF-8 sequences are replaced with '?'
 * - Control characters (except \n, \r, \t) are removed
 * - High surrogate/private use area codepoints are replaced
 *
 * Modifies the string in-place for efficiency. Safe to call on any string
 * that will be embedded in JSON or sent to an API.
 *
 * Thread Safety: This function is thread-safe (modifies only the input buffer).
 *
 * @param str String to sanitize (modified in-place)
 */
void sanitize_utf8_for_json(char *str);

/**
 * @brief Case-insensitive substring search (portable implementation)
 *
 * Finds the first occurrence of needle in haystack, ignoring case.
 * This is a portable alternative to the non-standard strcasestr().
 *
 * Thread Safety: This function is thread-safe (uses only input parameters).
 *
 * @param haystack String to search in
 * @param needle Substring to find
 * @return Pointer to first occurrence, or NULL if not found
 */
const char *strcasestr_portable(const char *haystack, const char *needle);

/**
 * @brief Extract hostname from a URL
 *
 * Extracts the hostname portion from a URL, stripping protocol, port,
 * and path components.
 *
 * Examples:
 *   "https://www.example.com/path" -> "www.example.com"
 *   "http://example.com:8080/foo"  -> "example.com"
 *   "example.com/bar"              -> "example.com"
 *
 * Thread Safety: This function is thread-safe (writes only to output buffer).
 *
 * @param url Full URL or hostname string
 * @param out Output buffer for hostname
 * @param out_size Size of output buffer (hostname will be truncated if needed)
 */
void extract_url_host(const char *url, char *out, size_t out_size);

/**
 * @brief Check if a period is part of an abbreviation
 *
 * Looks backwards from the period position to find the preceding word
 * and checks against a list of common abbreviations (Mr., Mrs., U.S., etc.)
 *
 * Also handles:
 * - Single capital letters (middle initials like "John F. Kennedy")
 * - Embedded periods (like "U.S." or "e.g.")
 *
 * Thread Safety: This function is thread-safe (uses only input parameters).
 *
 * @param text The full text being processed
 * @param period_pos Pointer to the period character in the text
 * @return true if this period is part of an abbreviation, false otherwise
 */
bool str_is_abbreviation(const char *text, const char *period_pos);

/**
 * @brief Check if a character is a sentence terminator
 *
 * Note: Colon (':') is NOT included — it causes over-segmentation on times
 * (3:00) and lists. Colon-newline boundaries are handled separately by
 * sentence_buffer_feed().
 *
 * Thread Safety: This function is thread-safe.
 *
 * @param c Character to check
 * @return true if c is '.', '!', or '?'
 */
bool str_is_sentence_terminator(char c);

/**
 * @brief Check if this is a valid sentence boundary
 *
 * Combines terminator detection with abbreviation checking.
 * A valid sentence boundary is a terminator (.!?:) followed by
 * whitespace or end of string, where the terminator is NOT part
 * of an abbreviation (for periods only).
 *
 * Thread Safety: This function is thread-safe (uses only input parameters).
 *
 * @param text The full text being processed
 * @param pos Position in the text to check
 * @return true if this is a valid sentence boundary
 */
bool str_is_sentence_boundary(const char *text, const char *pos);

#ifdef __cplusplus
}
#endif

#endif /* DAWN_COMMON_STRING_UTILS_H */
