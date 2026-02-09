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
 */

/**
 * @file tts_preprocessing.cpp
 * @brief Implementation of text preprocessing utilities for TTS
 *
 * This file implements text transformations to improve TTS speech quality.
 * It provides both C-compatible functions (via extern "C") for in-place
 * string manipulation and C++ std::string functions for complex transformations.
 *
 * OPTIMIZATION: Uses two-pass architecture:
 * - Pass 1: Calculate exact output size (single allocation)
 * - Pass 2: Generate output in preallocated buffer
 * Reduces 7 passes + 7 allocations to 2 passes + 1 allocation.
 *
 * @see tts_preprocessing.h for the public API
 */

#include "tts/tts_preprocessing.h"

#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstring>

// ============================================================================
// UTF-8 Utilities
// ============================================================================

/**
 * @brief Check if a byte is a valid UTF-8 continuation byte (10xxxxxx)
 */
static inline bool is_utf8_continuation(unsigned char byte) {
   return (byte & 0xC0) == 0x80;
}

/**
 * @brief Get number of bytes in a UTF-8 character from its lead byte
 * @return 1-4 for valid lead bytes, 1 for invalid (treat as single byte)
 */
static inline int utf8_char_bytes(unsigned char lead) {
   if (lead < 0x80)
      return 1;  // ASCII
   if (lead < 0xC0)
      return 1;  // Invalid lead byte (continuation) - treat as 1
   if (lead < 0xE0)
      return 2;  // 110xxxxx
   if (lead < 0xF0)
      return 3;  // 1110xxxx
   if (lead < 0xF8)
      return 4;  // 11110xxx
   return 1;     // Invalid (0xF8-0xFF) - treat as 1
}

/**
 * @brief Decode a UTF-8 character to its Unicode codepoint
 * @param src Pointer to UTF-8 sequence
 * @param bytes Expected number of bytes (from utf8_char_bytes)
 * @return Unicode codepoint, or 0xFFFFFFFF if invalid
 */
static inline unsigned int decode_utf8(const char *src, int bytes) {
   const unsigned char *s = reinterpret_cast<const unsigned char *>(src);

   if (bytes == 1) {
      return s[0];
   } else if (bytes == 2) {
      if (!is_utf8_continuation(s[1]))
         return 0xFFFFFFFF;
      return ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
   } else if (bytes == 3) {
      if (!is_utf8_continuation(s[1]) || !is_utf8_continuation(s[2]))
         return 0xFFFFFFFF;
      return ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
   } else if (bytes == 4) {
      if (!is_utf8_continuation(s[1]) || !is_utf8_continuation(s[2]) || !is_utf8_continuation(s[3]))
         return 0xFFFFFFFF;
      return ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
   }
   return 0xFFFFFFFF;
}

// ============================================================================
// Character and Emoji Functions (C-compatible)
// ============================================================================

extern "C" {

void remove_chars(char *str, const char *chars_to_remove) {
   if (!str || !chars_to_remove)
      return;

   char *src, *dst;
   bool should_remove;
   for (src = dst = str; *src != '\0'; src++) {
      should_remove = false;
      for (const char *rc = chars_to_remove; *rc != '\0'; rc++) {
         if (*src == *rc) {
            should_remove = true;
            break;
         }
      }
      if (!should_remove) {
         *dst++ = *src;
      }
   }
   *dst = '\0';
}

bool is_emoji(unsigned int codepoint) {
   // Emoji and symbol ranges that TTS engines cannot pronounce
   return (codepoint >= 0x1F600 && codepoint <= 0x1F64F) ||  // Emoticons
          (codepoint >= 0x1F300 &&
           codepoint <= 0x1F5FF) ||  // Miscellaneous Symbols and Pictographs
          (codepoint >= 0x1F680 && codepoint <= 0x1F6FF) ||  // Transport and Map Symbols
          (codepoint >= 0x1F900 && codepoint <= 0x1F9FF) ||  // Supplemental Symbols and Pictographs
          (codepoint >= 0x1F1E0 && codepoint <= 0x1F1FF) ||  // Regional Indicator Symbols (flags)
          (codepoint >= 0x1FA00 && codepoint <= 0x1FA6F) ||  // Chess, Extended-A symbols
          (codepoint >= 0x1FA70 && codepoint <= 0x1FAFF) ||  // Symbols and Pictographs Extended-A
          (codepoint >= 0x2300 && codepoint <= 0x23FF) ||    // Miscellaneous Technical
          (codepoint >= 0x2460 && codepoint <= 0x24FF) ||    // Enclosed Alphanumerics
          (codepoint >= 0x2500 && codepoint <= 0x257F) ||    // Box Drawing
          (codepoint >= 0x2580 && codepoint <= 0x259F) ||    // Block Elements
          (codepoint >= 0x25A0 && codepoint <= 0x25FF) ||    // Geometric Shapes
          (codepoint >= 0x2600 && codepoint <= 0x26FF) ||    // Miscellaneous Symbols
          (codepoint >= 0x2700 && codepoint <= 0x27BF) ||    // Dingbats
          (codepoint >= 0xFE00 && codepoint <= 0xFE0F) ||    // Variation Selectors
          (codepoint >= 0x200D && codepoint <= 0x200D) ||    // Zero Width Joiner
          (codepoint >= 0x20E3 && codepoint <= 0x20E3) ||    // Combining Enclosing Keycap
          (codepoint >= 0xE0000 && codepoint <= 0xE007F);    // Tags block (flag sequences)
}

void remove_emojis(char *str) {
   if (!str)
      return;

   char *src, *dst;
   src = dst = str;

   while (*src) {
      unsigned char byte = *src;
      unsigned int codepoint = 0;
      int bytes_in_char = 1;
      bool valid_sequence = true;

      if (byte < 0x80) {
         // 1-byte ASCII character
         codepoint = byte;
      } else if (byte < 0xE0) {
         // 2-byte sequence: 110xxxxx 10xxxxxx
         if (*(src + 1) == '\0' || !is_utf8_continuation(*(src + 1))) {
            valid_sequence = false;
         } else {
            codepoint = (byte & 0x1F) << 6;
            codepoint |= (*(src + 1) & 0x3F);
            bytes_in_char = 2;
         }
      } else if (byte < 0xF0) {
         // 3-byte sequence: 1110xxxx 10xxxxxx 10xxxxxx
         if (*(src + 1) == '\0' || *(src + 2) == '\0' || !is_utf8_continuation(*(src + 1)) ||
             !is_utf8_continuation(*(src + 2))) {
            valid_sequence = false;
         } else {
            codepoint = (byte & 0x0F) << 12;
            codepoint |= (*(src + 1) & 0x3F) << 6;
            codepoint |= (*(src + 2) & 0x3F);
            bytes_in_char = 3;
         }
      } else if (byte < 0xF8) {
         // 4-byte sequence: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
         if (*(src + 1) == '\0' || *(src + 2) == '\0' || *(src + 3) == '\0' ||
             !is_utf8_continuation(*(src + 1)) || !is_utf8_continuation(*(src + 2)) ||
             !is_utf8_continuation(*(src + 3))) {
            valid_sequence = false;
         } else {
            codepoint = (byte & 0x07) << 18;
            codepoint |= (*(src + 1) & 0x3F) << 12;
            codepoint |= (*(src + 2) & 0x3F) << 6;
            codepoint |= (*(src + 3) & 0x3F);
            bytes_in_char = 4;
         }
      } else {
         // Invalid UTF-8 start byte (0xF8-0xFF are not valid)
         valid_sequence = false;
      }

      if (!valid_sequence) {
         // Skip invalid byte
         src++;
         continue;
      }

      if (!is_emoji(codepoint)) {
         // Copy valid non-emoji character
         for (int i = 0; i < bytes_in_char; i++) {
            *dst++ = *src++;
         }
      } else {
         // Skip emoji
         src += bytes_in_char;
      }
   }
   *dst = '\0';
}

int preprocess_text_for_tts_c(const char *input, char *output, size_t output_size) {
   if (!input || !output || output_size == 0)
      return -1;

   // Use the C++ implementation and copy result
   std::string result = preprocess_text_for_tts(std::string(input));

   if (result.length() >= output_size) {
      // Output buffer too small - truncate
      std::memcpy(output, result.c_str(), output_size - 1);
      output[output_size - 1] = '\0';
      return static_cast<int>(output_size - 1);
   }

   std::memcpy(output, result.c_str(), result.length() + 1);
   return static_cast<int>(result.length());
}

}  // extern "C"

// ============================================================================
// Lookup Tables
// ============================================================================

// US state abbreviation to full name mapping with precomputed lengths
struct state_entry_t {
   char abbrev[3];    // 2 chars + null terminator
   uint8_t full_len;  // Precomputed length of full name
   const char *full_name;
};

static const state_entry_t state_abbreviations[] = {
   { "AL", 7, "Alabama" },
   { "AK", 6, "Alaska" },
   { "AZ", 7, "Arizona" },
   { "AR", 8, "Arkansas" },
   { "CA", 10, "California" },
   { "CO", 8, "Colorado" },
   { "CT", 11, "Connecticut" },
   { "DE", 8, "Delaware" },
   { "FL", 7, "Florida" },
   { "GA", 7, "Georgia" },
   { "HI", 6, "Hawaii" },
   { "ID", 5, "Idaho" },
   { "IL", 8, "Illinois" },
   { "IN", 7, "Indiana" },
   { "IA", 4, "Iowa" },
   { "KS", 6, "Kansas" },
   { "KY", 8, "Kentucky" },
   { "LA", 9, "Louisiana" },
   { "ME", 5, "Maine" },
   { "MD", 8, "Maryland" },
   { "MA", 13, "Massachusetts" },
   { "MI", 8, "Michigan" },
   { "MN", 9, "Minnesota" },
   { "MS", 11, "Mississippi" },
   { "MO", 8, "Missouri" },
   { "MT", 7, "Montana" },
   { "NE", 8, "Nebraska" },
   { "NV", 6, "Nevada" },
   { "NH", 13, "New Hampshire" },
   { "NJ", 10, "New Jersey" },
   { "NM", 10, "New Mexico" },
   { "NY", 8, "New York" },
   { "NC", 14, "North Carolina" },
   { "ND", 12, "North Dakota" },
   { "OH", 4, "Ohio" },
   { "OK", 8, "Oklahoma" },
   { "OR", 6, "Oregon" },
   { "PA", 12, "Pennsylvania" },
   { "RI", 12, "Rhode Island" },
   { "SC", 14, "South Carolina" },
   { "SD", 12, "South Dakota" },
   { "TN", 9, "Tennessee" },
   { "TX", 5, "Texas" },
   { "UT", 4, "Utah" },
   { "VT", 7, "Vermont" },
   { "VA", 8, "Virginia" },
   { "WA", 10, "Washington" },
   { "WV", 13, "West Virginia" },
   { "WI", 9, "Wisconsin" },
   { "WY", 7, "Wyoming" },
   { "DC", 4, "D.C." },
   { "", 0, nullptr }  // Sentinel
};

static constexpr size_t STATE_COUNT = 51;

// Generic abbreviation entry structure
struct abbrev_entry_t {
   const char *abbrev;
   uint8_t abbrev_len;
   uint8_t full_len;
   const char *full_name;
};

// Day of week abbreviations (3-letter) to full names
static const abbrev_entry_t day_abbreviations[] = {
   { "Mon", 3, 6, "Monday" },   { "Tue", 3, 7, "Tuesday" }, { "Wed", 3, 9, "Wednesday" },
   { "Thu", 3, 8, "Thursday" }, { "Fri", 3, 6, "Friday" },  { "Sat", 3, 8, "Saturday" },
   { "Sun", 3, 6, "Sunday" },   { nullptr, 0, 0, nullptr }  // Sentinel
};

// Month abbreviations (3-letter) to full names
static const abbrev_entry_t month_abbreviations[] = {
   { "Jan", 3, 7, "January" }, { "Feb", 3, 8, "February" }, { "Mar", 3, 5, "March" },
   { "Apr", 3, 5, "April" },   { "May", 3, 3, "May" },      { "Jun", 3, 4, "June" },
   { "Jul", 3, 4, "July" },    { "Aug", 3, 6, "August" },   { "Sep", 3, 9, "September" },
   { "Oct", 3, 7, "October" }, { "Nov", 3, 8, "November" }, { "Dec", 3, 8, "December" },
   { nullptr, 0, 0, nullptr }  // Sentinel
};

// ============================================================================
// Boundary Checking Helpers
// ============================================================================

/**
 * @brief Check if character is a word boundary for state abbreviation matching
 */
static inline bool is_state_boundary(char c) {
   return c == '\0' || c == ' ' || c == ',' || c == '.' || c == '\n' || c == '\t' || c == ')' ||
          c == '"' || c == '\'' || c == ':' || c == ';' || c == '!' || c == '?';
}

/**
 * @brief Check if position is at a word boundary (for abbreviation matching)
 */
static inline bool is_abbrev_boundary(char c) {
   return c == '\0' || c == ' ' || c == ',' || c == '.' || c == '\n' || c == '\t' || c == ')' ||
          c == '"' || c == '\'' || c == ':' || c == ';' || c == '!' || c == '?' || c == '-';
}

/**
 * @brief Check if character before position is valid boundary for state abbrev
 */
static inline bool is_valid_state_before(const char *src, size_t pos) {
   if (pos == 0)
      return true;
   char c = src[pos - 1];
   return c == ' ' || c == ',' || c == '(' || c == '\n' || c == '\t' || c == '\r' || c == '.' ||
          c == ':' || c == ';' || c == '"' || c == '\'';
}

/**
 * @brief Check if character before position is valid boundary for day/month abbrev
 */
static inline bool is_valid_abbrev_before(const char *src, size_t pos) {
   return (pos == 0) || is_abbrev_boundary(src[pos - 1]);
}

// ============================================================================
// Lookup Functions
// ============================================================================

/**
 * @brief Look up state full name by 2-letter abbreviation
 * @return Pointer to state entry, or nullptr if not found
 */
static inline const state_entry_t *lookup_state(char c1, char c2) {
   for (size_t i = 0; i < STATE_COUNT; i++) {
      if (state_abbreviations[i].abbrev[0] == c1 && state_abbreviations[i].abbrev[1] == c2) {
         return &state_abbreviations[i];
      }
   }
   return nullptr;
}

/**
 * @brief Look up day or month abbreviation
 * @return Pointer to abbrev entry, or nullptr if not found
 */
static inline const abbrev_entry_t *lookup_day_or_month(const char *src) {
   // Check days first (7 entries)
   for (const abbrev_entry_t *e = day_abbreviations; e->abbrev != nullptr; e++) {
      if (src[0] == e->abbrev[0] &&
          std::tolower(static_cast<unsigned char>(src[1])) ==
              std::tolower(static_cast<unsigned char>(e->abbrev[1])) &&
          std::tolower(static_cast<unsigned char>(src[2])) ==
              std::tolower(static_cast<unsigned char>(e->abbrev[2]))) {
         return e;
      }
   }
   // Check months (12 entries)
   for (const abbrev_entry_t *e = month_abbreviations; e->abbrev != nullptr; e++) {
      if (src[0] == e->abbrev[0] &&
          std::tolower(static_cast<unsigned char>(src[1])) ==
              std::tolower(static_cast<unsigned char>(e->abbrev[1])) &&
          std::tolower(static_cast<unsigned char>(src[2])) ==
              std::tolower(static_cast<unsigned char>(e->abbrev[2]))) {
         return e;
      }
   }
   return nullptr;
}

// ============================================================================
// Temperature Unit Expansion Strings
// ============================================================================

static const char *TEMP_FAHRENHEIT = " degrees Fahrenheit";
static const uint8_t TEMP_FAHRENHEIT_LEN = 19;

static const char *TEMP_CELSIUS = " degrees Celsius";
static const uint8_t TEMP_CELSIUS_LEN = 16;

static const char *TEMP_KELVIN = " Kelvin";
static const uint8_t TEMP_KELVIN_LEN = 7;

static const char *TEMP_DEGREES = " degrees";
static const uint8_t TEMP_DEGREES_LEN = 8;

// ============================================================================
// URL Processing Helpers
// ============================================================================

/**
 * @brief Check if position starts a URL (http://, https://, or www.)
 * @return Length of URL prefix (7 for http://, 8 for https://, 4 for www.), or 0 if not a URL
 */
static inline size_t url_prefix_length(const char *src, size_t pos, size_t len) {
   size_t remaining = len - pos;

   // Check for https:// (8 chars)
   if (remaining >= 8 && std::strncmp(src + pos, "https://", 8) == 0) {
      return 8;
   }
   // Check for http:// (7 chars)
   if (remaining >= 7 && std::strncmp(src + pos, "http://", 7) == 0) {
      return 7;
   }
   // Check for www. at word boundary (4 chars)
   if (remaining >= 4 && std::strncmp(src + pos, "www.", 4) == 0) {
      // Verify it's at a word boundary
      if (pos == 0 || src[pos - 1] == ' ' || src[pos - 1] == '\n' || src[pos - 1] == '\t' ||
          src[pos - 1] == '(' || src[pos - 1] == '"' || src[pos - 1] == '\'') {
         return 4;
      }
   }
   return 0;
}

/**
 * @brief Find the end of a URL (first whitespace, newline, or certain punctuation)
 * @return Position of URL end (exclusive)
 */
static inline size_t find_url_end(const char *src, size_t start, size_t len) {
   size_t pos = start;
   while (pos < len) {
      char c = src[pos];
      // URL terminators
      if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '"' || c == '\'' || c == '<' ||
          c == '>' || c == ')' || c == ']') {
         break;
      }
      // Handle trailing punctuation that's likely not part of URL
      if ((c == '.' || c == ',' || c == '!' || c == '?' || c == ';' || c == ':') &&
          (pos + 1 >= len || src[pos + 1] == ' ' || src[pos + 1] == '\n')) {
         break;
      }
      pos++;
   }
   return pos;
}

/**
 * @brief Extract domain from URL and calculate spoken form size
 *
 * For "https://www.example.com/path" -> "example dot com"
 * For "http://github.com/user/repo" -> "github dot com"
 *
 * @param src Source string
 * @param url_start Start of URL (after any prefix already consumed)
 * @param url_end End of URL
 * @param domain_start Output: start of domain within URL
 * @param domain_end Output: end of domain (before path)
 * @return Spoken form size (with dots expanded to " dot ")
 */
static inline size_t extract_domain_info(const char *src,
                                         size_t url_start,
                                         size_t url_end,
                                         size_t *domain_start,
                                         size_t *domain_end) {
   // Skip www. prefix if present
   size_t start = url_start;
   if (url_end - start >= 4 && std::strncmp(src + start, "www.", 4) == 0) {
      start += 4;
   }

   // Find end of domain (first / or end of URL)
   size_t end = start;
   while (end < url_end && src[end] != '/') {
      end++;
   }

   *domain_start = start;
   *domain_end = end;

   // Calculate spoken size: each '.' becomes " dot " (5 chars instead of 1)
   size_t spoken_size = 0;
   for (size_t i = start; i < end; i++) {
      if (src[i] == '.') {
         spoken_size += 5;  // " dot "
      } else {
         spoken_size += 1;
      }
   }
   return spoken_size;
}

/**
 * @brief Write domain in spoken form (dots -> " dot ")
 */
static inline size_t write_spoken_domain(const char *src,
                                         size_t domain_start,
                                         size_t domain_end,
                                         char *out) {
   size_t out_pos = 0;
   for (size_t i = domain_start; i < domain_end; i++) {
      if (src[i] == '.') {
         std::memcpy(out + out_pos, " dot ", 5);
         out_pos += 5;
      } else {
         out[out_pos++] = src[i];
      }
   }
   return out_pos;
}

// ============================================================================
// Two-Pass Optimized Implementation (Unified Template)
// ============================================================================

/**
 * @brief Pass mode for the unified text processing template
 *
 * Using an enum class with if constexpr ensures:
 * - Single source of truth for transformation logic
 * - Zero runtime overhead (compile-time branching)
 * - Impossible to have synchronization bugs between passes
 */
enum class PassMode {
   CalculateSize,  ///< Pass 1: Only calculate output size, don't write
   GenerateOutput  ///< Pass 2: Write output to preallocated buffer
};

/**
 * @brief Unified text processing implementation for both passes
 *
 * This template function handles both size calculation (Pass 1) and output
 * generation (Pass 2) using compile-time branching via if constexpr.
 * This guarantees that both passes use identical transformation logic,
 * eliminating the synchronization risk of maintaining two separate functions.
 *
 * @tparam mode PassMode::CalculateSize or PassMode::GenerateOutput
 * @param src Input string (UTF-8)
 * @param len Input length
 * @param out Output buffer (only used when mode == GenerateOutput, can be nullptr otherwise)
 * @return Number of bytes (calculated size or bytes written)
 */
template<PassMode mode> static size_t process_text_impl(const char *src, size_t len, char *out) {
   size_t i = 0;
   size_t out_pos = 0;

   while (i < len) {
      unsigned char byte = src[i];

      // === Handle ASCII (fast path) ===
      if (byte < 0x80) {
         // Skip asterisk (markdown bold markers)
         if (byte == '*') {
            i++;
            continue;
         }

         // Check for state abbreviation (2 uppercase letters at boundary)
         if (std::isupper(byte) && i + 1 < len &&
             std::isupper(static_cast<unsigned char>(src[i + 1]))) {
            bool valid_before = is_valid_state_before(src, i);
            bool valid_after = (i + 2 >= len) || is_state_boundary(src[i + 2]);

            if (valid_before && valid_after) {
               const state_entry_t *state = lookup_state(src[i], src[i + 1]);
               if (state) {
                  if constexpr (mode == PassMode::GenerateOutput) {
                     std::memcpy(out + out_pos, state->full_name, state->full_len);
                  }
                  out_pos += state->full_len;
                  i += 2;
                  continue;
               }
            }
         }

         // Check for day/month abbreviation (3 letters starting with uppercase)
         if (std::isupper(byte) && i + 2 < len) {
            bool valid_before = is_valid_abbrev_before(src, i);
            bool valid_after = (i + 3 >= len) || is_abbrev_boundary(src[i + 3]);

            if (valid_before && valid_after) {
               const abbrev_entry_t *abbrev = lookup_day_or_month(src + i);
               if (abbrev) {
                  if constexpr (mode == PassMode::GenerateOutput) {
                     std::memcpy(out + out_pos, abbrev->full_name, abbrev->full_len);
                  }
                  out_pos += abbrev->full_len;
                  i += 3;
                  continue;
               }
            }
         }

         // Spaced dash " - " -> comma (creates pause like em-dash)
         // Check: previous char was space, current is dash, next is space
         if (byte == '-' && out_pos > 0 && i + 1 < len && src[i + 1] == ' ') {
            // Check if we just output a space (look at what we wrote, not src)
            bool prev_was_space = false;
            if constexpr (mode == PassMode::GenerateOutput) {
               prev_was_space = (out[out_pos - 1] == ' ');
            } else {
               // In size calculation, check source position
               prev_was_space = (i > 0 && src[i - 1] == ' ');
            }
            if (prev_was_space) {
               // Replace dash with comma, trailing space processed normally
               if constexpr (mode == PassMode::GenerateOutput) {
                  out[out_pos] = ',';
               }
               out_pos++;
               i++;
               continue;
            }
         }

         // URL detection: convert full URLs to spoken domain only
         // e.g., "https://www.example.com/path" -> "example dot com"
         if (byte == 'h' || byte == 'w') {
            size_t prefix_len = url_prefix_length(src, i, len);
            if (prefix_len > 0) {
               // Found a URL - extract domain and convert to spoken form
               size_t url_content_start = i + prefix_len;
               size_t url_end = find_url_end(src, url_content_start, len);

               size_t domain_start, domain_end;
               size_t spoken_size = extract_domain_info(src, url_content_start, url_end,
                                                        &domain_start, &domain_end);

               if (spoken_size > 0) {
                  if constexpr (mode == PassMode::GenerateOutput) {
                     write_spoken_domain(src, domain_start, domain_end, out + out_pos);
                  }
                  out_pos += spoken_size;
                  i = url_end;
                  continue;
               }
            }
         }

         // Regular ASCII character - copy
         if constexpr (mode == PassMode::GenerateOutput) {
            out[out_pos] = byte;
         }
         out_pos++;
         i++;
         continue;
      }

      // === Handle UTF-8 multi-byte ===
      int char_bytes = utf8_char_bytes(byte);

      // Validate we have enough bytes
      if (i + char_bytes > len) {
         // Truncated sequence - skip byte
         i++;
         continue;
      }

      unsigned int codepoint = decode_utf8(src + i, char_bytes);

      // Invalid UTF-8 sequence
      if (codepoint == 0xFFFFFFFF) {
         i++;
         continue;
      }

      // Skip emoji
      if (is_emoji(codepoint)) {
         i += char_bytes;
         continue;
      }

      // Em-dash (U+2014) -> comma
      if (codepoint == 0x2014) {
         if constexpr (mode == PassMode::GenerateOutput) {
            out[out_pos] = ',';
         }
         out_pos++;
         i += 3;
         continue;
      }

      // Degree symbol (U+00B0) -> temperature expansion
      if (codepoint == 0x00B0) {
         // Check what follows the degree symbol
         if (i + 2 < len) {
            char unit = src[i + 2];
            if (unit == 'F' || unit == 'f') {
               if constexpr (mode == PassMode::GenerateOutput) {
                  std::memcpy(out + out_pos, TEMP_FAHRENHEIT, TEMP_FAHRENHEIT_LEN);
               }
               out_pos += TEMP_FAHRENHEIT_LEN;
               i += 3;
               continue;
            } else if (unit == 'C' || unit == 'c') {
               if constexpr (mode == PassMode::GenerateOutput) {
                  std::memcpy(out + out_pos, TEMP_CELSIUS, TEMP_CELSIUS_LEN);
               }
               out_pos += TEMP_CELSIUS_LEN;
               i += 3;
               continue;
            } else if (unit == 'K' || unit == 'k') {
               if constexpr (mode == PassMode::GenerateOutput) {
                  std::memcpy(out + out_pos, TEMP_KELVIN, TEMP_KELVIN_LEN);
               }
               out_pos += TEMP_KELVIN_LEN;
               i += 3;
               continue;
            }
         }
         // Bare degree symbol
         if constexpr (mode == PassMode::GenerateOutput) {
            std::memcpy(out + out_pos, TEMP_DEGREES, TEMP_DEGREES_LEN);
         }
         out_pos += TEMP_DEGREES_LEN;
         i += 2;
         continue;
      }

      // Regular UTF-8 character - copy
      if constexpr (mode == PassMode::GenerateOutput) {
         std::memcpy(out + out_pos, src + i, char_bytes);
      }
      out_pos += char_bytes;
      i += char_bytes;
   }

   return out_pos;
}

// Explicit wrapper functions for clarity and backward compatibility
static inline size_t calculate_output_size(const char *src, size_t len) {
   return process_text_impl<PassMode::CalculateSize>(src, len, nullptr);
}

static inline size_t generate_output(const char *src, size_t len, char *out) {
   return process_text_impl<PassMode::GenerateOutput>(src, len, out);
}

// ============================================================================
// Main Preprocessing Function (Optimized Two-Pass)
// ============================================================================

std::string preprocess_text_for_tts(const std::string &input) {
   if (input.empty())
      return input;

   const char *src = input.data();
   const size_t len = input.length();

   // === PASS 1: Calculate exact output size ===
   size_t output_size = calculate_output_size(src, len);

   // If output is same size and no transformations needed, check for early return
   // (This is a heuristic - actual content may differ)
   if (output_size == 0) {
      return std::string();
   }

   // === Allocate output buffer (single allocation) ===
   std::string result;
   result.resize(output_size);

   // === PASS 2: Generate output ===
   size_t bytes_written = generate_output(src, len, &result[0]);

   // Verify size calculation was exact (debug assertion)
   // If this fails, there's a mismatch between calculate_output_size and generate_output
   assert(bytes_written == output_size && "Size calculation mismatch!");
   (void)bytes_written;  // Suppress unused warning in release builds

   return result;
}
