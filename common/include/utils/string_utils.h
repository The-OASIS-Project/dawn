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
 * @brief Safe string copy with guaranteed null-termination
 *
 * Unlike strncpy, this always null-terminates the destination buffer
 * and doesn't waste cycles padding with zeros. This is a portable
 * replacement for strlcpy which isn't available on all platforms.
 *
 * Thread Safety: This function is thread-safe (modifies only dest buffer).
 *
 * @param dest Destination buffer
 * @param src Source string (must be null-terminated)
 * @param size Size of destination buffer
 */
static inline void safe_strncpy(char *dest, const char *src, size_t size) {
   if (size == 0) {
      return;
   }
   size_t len = strlen(src);
   if (len >= size) {
      len = size - 1;
   }
   memcpy(dest, src, len);
   dest[len] = '\0';
}

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
static inline void extract_url_host(const char *url, char *out, size_t out_size) {
   if (!url || !out || out_size == 0) {
      if (out && out_size > 0) {
         out[0] = '\0';
      }
      return;
   }

   /* Skip protocol (http:// or https://) */
   const char *p = strstr(url, "://");
   p = p ? p + 3 : url;

   /* Find end of hostname (stop at /, :, ?, or end) */
   const char *end = p;
   while (*end && *end != '/' && *end != ':' && *end != '?') {
      end++;
   }

   size_t len = (size_t)(end - p);
   if (len >= out_size) {
      len = out_size - 1;
   }
   memcpy(out, p, len);
   out[len] = '\0';
}

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
 * Thread Safety: This function is thread-safe.
 *
 * @param c Character to check
 * @return true if c is '.', '!', '?', or ':'
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
