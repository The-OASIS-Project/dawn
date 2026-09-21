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

#include "tts/number_to_words.h"

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
   return (codepoint >= 0x1F000 && codepoint <= 0x1FFFF) ||  // All SMP emoji/symbol blocks
          (codepoint >= 0x2300 && codepoint <= 0x23FF) ||    // Miscellaneous Technical
          (codepoint >= 0x2460 && codepoint <= 0x24FF) ||    // Enclosed Alphanumerics
          (codepoint >= 0x2500 && codepoint <= 0x257F) ||    // Box Drawing
          (codepoint >= 0x2580 && codepoint <= 0x259F) ||    // Block Elements
          (codepoint >= 0x25A0 && codepoint <= 0x25FF) ||    // Geometric Shapes
          (codepoint >= 0x2600 && codepoint <= 0x26FF) ||    // Miscellaneous Symbols
          (codepoint >= 0x2700 && codepoint <= 0x27BF) ||    // Dingbats
          (codepoint >= 0x2B00 && codepoint <= 0x2BFF) ||    // Misc Symbols and Arrows (⭐⬆⬇)
          (codepoint >= 0x3030 && codepoint <= 0x3030) ||    // Wavy Dash
          (codepoint >= 0x303D && codepoint <= 0x303D) ||    // Part Alternation Mark
          (codepoint >= 0x3297 && codepoint <= 0x3299) ||    // CJK enclosed ideographs
          (codepoint >= 0xFE00 && codepoint <= 0xFE0F) ||    // Variation Selectors
          (codepoint >= 0x200D && codepoint <= 0x200D) ||    // Zero Width Joiner
          (codepoint >= 0x200B && codepoint <= 0x200B) ||    // Zero Width Space
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

int preprocess_text_for_tts_c(const char *input,
                              char *output,
                              size_t output_size,
                              int *bytes_written) {
   if (bytes_written)
      *bytes_written = 0;
   if (!input || !output || output_size == 0)
      return 1;

   std::string result = preprocess_text_for_tts(std::string(input));

   if (result.length() >= output_size) {
      std::memcpy(output, result.c_str(), output_size - 1);
      output[output_size - 1] = '\0';
      if (bytes_written)
         *bytes_written = static_cast<int>(output_size - 1);
      return 0;
   }

   std::memcpy(output, result.c_str(), result.length() + 1);
   if (bytes_written)
      *bytes_written = static_cast<int>(result.length());
   return 0;
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

// State codes that collide with common English words / acronyms. These expand to
// the state name ONLY with positive context (a city + comma, or a comma-gated
// ZIP); bare, they fall through so espeak speaks the letters or word — e.g.
// "your ID" -> "eye dee" (not "Idaho"), "OK, here's the plan" -> "okay" (not
// "Oklahoma"). Non-colliders (CA, TX, FL, ...) keep expanding unconditionally.
static const char STATE_COLLIDERS[][3] = {
   "AL", "HI", "ID", "IN", "LA", "MA", "MD", "ME", "MO", "MS",
   "OH", "OK", "OR", "PA", "CT", "SD", "AR", "NE", "VA", "DE",
};

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

// Capitalized words that precede a comma in prose but are NOT cities — so a
// following state code must not expand ("Yes, OK" is not "Yes, Oklahoma").
static const char *const state_interjections[] = { "Yes",  "No", "Oh",    "OK",     "Okay",  "Sure",
                                                   "Well", "Hi", "Hello", "Thanks", "Right", "So" };

static bool word_is_interjection(const char *w, size_t wlen) {
   for (const char *s : state_interjections) {
      if (std::strlen(s) == wlen && std::strncmp(s, w, wlen) == 0) {
         return true;
      }
   }
   return false;
}

// Context signal A: the code at @pos is preceded by "<Capitalized city>, " — a
// capitalized word (>= 2 letters, not a prose interjection) then a comma and an
// optional space. Matches "Boise, ID", "St. Louis, MO", "New York, NY".
static bool state_city_comma_before(const char *src, size_t pos) {
   size_t p = pos;
   if (p == 0)
      return false;
   if (src[p - 1] == ' ')
      p--; /* optional single space between the comma and the code */
   if (p == 0 || src[p - 1] != ',')
      return false;
   p--; /* p now indexes the comma; the city word ends just before it */
   size_t wend = p;
   if (wend == 0 || !std::isalpha((unsigned char)src[wend - 1]))
      return false;
   size_t ws = wend;
   while (ws > 0 && std::isalpha((unsigned char)src[ws - 1]))
      ws--;
   size_t wlen = wend - ws;
   if (wlen < 2 || !std::isupper((unsigned char)src[ws]))
      return false;
   return !word_is_interjection(src + ws, wlen);
}

// Context signal B (comma-gated ZIP): the code at @pos is preceded by a comma and
// followed by exactly a 5-digit ZIP ("Apt 4, ID 83702"). The comma gate keeps
// "order ID 83702" from reading "order Idaho 83702".
static bool state_comma_zip(const char *src, size_t pos, size_t len) {
   size_t p = pos;
   if (p == 0)
      return false;
   if (src[p - 1] == ' ')
      p--;
   if (p == 0 || src[p - 1] != ',')
      return false;
   size_t q = pos + 2; /* char after the 2-letter code */
   if (q < len && src[q] == ' ')
      q++;
   size_t digits = 0;
   while (q + digits < len && std::isdigit((unsigned char)src[q + digits]))
      digits++;
   /* exactly 5 digits, and not run into a letter ("12345x" is not a ZIP; a '-'
    * for ZIP+4 is a fine boundary). */
   return digits == 5 && (q + digits >= len || !std::isalnum((unsigned char)src[q + digits]));
}

// ============================================================================
// Lookup Functions
// ============================================================================

/**
 * @brief O(1) lookup table indexed by (first_letter - 'A', second_letter - 'A').
 * Built once at startup via build_state_lookup_table().
 */
static const state_entry_t *state_lookup[26][26] = {};
static bool state_needs_context[26][26] = {};
static bool state_lookup_built = false;

static void build_state_lookup_table() {
   if (state_lookup_built)
      return;
   for (size_t i = 0; i < STATE_COUNT; i++) {
      unsigned char c1 = (unsigned char)state_abbreviations[i].abbrev[0];
      unsigned char c2 = (unsigned char)state_abbreviations[i].abbrev[1];
      if (c1 >= 'A' && c1 <= 'Z' && c2 >= 'A' && c2 <= 'Z') {
         state_lookup[c1 - 'A'][c2 - 'A'] = &state_abbreviations[i];
      }
   }
   for (const auto &code : STATE_COLLIDERS) {
      state_needs_context[(unsigned char)(code[0] - 'A')][(unsigned char)(code[1] - 'A')] = true;
   }
   state_lookup_built = true;
}

/** @brief True if this 2-letter state code needs positive context to expand. */
static inline bool state_code_needs_context(char c1, char c2) {
   if (c1 < 'A' || c1 > 'Z' || c2 < 'A' || c2 > 'Z')
      return false;
   return state_needs_context[(unsigned char)(c1 - 'A')][(unsigned char)(c2 - 'A')];
}

/**
 * @brief Look up state full name by 2-letter abbreviation (O(1) via direct index)
 * @return Pointer to state entry, or nullptr if not found
 */
static inline const state_entry_t *lookup_state(char c1, char c2) {
   if (c1 < 'A' || c1 > 'Z' || c2 < 'A' || c2 > 'Z')
      return nullptr;
   return state_lookup[(unsigned char)(c1 - 'A')][(unsigned char)(c2 - 'A')];
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
// Currency Expansion Tables
// ============================================================================

struct currency_info_t {
   const char *singular;
   const char *plural;
   uint8_t singular_len;
   uint8_t plural_len;
};

// Indexed by a simple switch on the symbol/codepoint
static const currency_info_t CURRENCY_DOLLAR = { "dollar", "dollars", 6, 7 };
static const currency_info_t CURRENCY_POUND = { "pound", "pounds", 5, 6 };
static const currency_info_t CURRENCY_EURO = { "euro", "euros", 4, 5 };
static const currency_info_t CURRENCY_YEN = { "yen", "yen", 3, 3 };

struct magnitude_info_t {
   const char *word;
   uint8_t len;
};

static const magnitude_info_t MAGNITUDE_K = { " thousand", 9 };
static const magnitude_info_t MAGNITUDE_M = { " million", 8 };
static const magnitude_info_t MAGNITUDE_B = { " billion", 8 };
static const magnitude_info_t MAGNITUDE_T = { " trillion", 9 };

/**
 * @brief Result of scanning digits after a currency symbol
 */
struct currency_scan_t {
   size_t num_len;    ///< Length of digit portion (including commas/decimal point)
   char magnitude;    ///< 'K', 'M', 'B', 'T', or '\0' if none
   bool is_singular;  ///< true if number is exactly "1" (no decimal, no commas)
};

/**
 * @brief Scan a number following a currency symbol
 *
 * Accepts: digits, commas within digits, optional decimal point + digits,
 * optional trailing K/M/B/T magnitude suffix.
 *
 * @param src Source string
 * @param start Position of first character after currency symbol
 * @param len Total length of source string
 * @return Scan result; num_len == 0 means no digits found
 */
static currency_scan_t scan_currency_number(const char *src, size_t start, size_t len) {
   currency_scan_t result = { 0, '\0', false };
   size_t pos = start;

   // Scan integer part: digits and commas
   bool has_digits = false;
   while (pos < len) {
      char c = src[pos];
      if (c >= '0' && c <= '9') {
         has_digits = true;
         pos++;
      } else if (c == ',' && has_digits && pos + 1 < len && src[pos + 1] >= '0' &&
                 src[pos + 1] <= '9') {
         // Comma within number (e.g., 1,000)
         pos++;
      } else {
         break;
      }
   }

   if (!has_digits)
      return result;

   // Optional decimal part
   if (pos < len && src[pos] == '.' && pos + 1 < len && src[pos + 1] >= '0' &&
       src[pos + 1] <= '9') {
      pos++;  // skip '.'
      while (pos < len && src[pos] >= '0' && src[pos] <= '9') {
         pos++;
      }
   }

   result.num_len = pos - start;

   // Check for magnitude suffix (K/M/B/T)
   if (pos < len) {
      char c = src[pos];
      if (c == 'K' || c == 'k' || c == 'M' || c == 'm' || c == 'B' || c == 'b' || c == 'T' ||
          c == 't') {
         // Only treat as magnitude if followed by word boundary
         bool at_boundary = (pos + 1 >= len) || src[pos + 1] == ' ' || src[pos + 1] == ',' ||
                            src[pos + 1] == '.' || src[pos + 1] == '\n' || src[pos + 1] == '\t' ||
                            src[pos + 1] == ')' || src[pos + 1] == '"' || src[pos + 1] == '\'' ||
                            src[pos + 1] == '!' || src[pos + 1] == '?' || src[pos + 1] == ':' ||
                            src[pos + 1] == ';';
         if (at_boundary) {
            result.magnitude = (char)std::toupper((unsigned char)c);
         }
      }
   }

   // Singular: number portion is exactly "1" with no magnitude suffix
   result.is_singular = (result.num_len == 1 && src[start] == '1' && result.magnitude == '\0');

   return result;
}

/**
 * @brief Get magnitude expansion for a suffix character
 * @return Pointer to magnitude info, or nullptr if not a valid suffix
 */
static inline const magnitude_info_t *get_magnitude(char suffix) {
   switch (suffix) {
      case 'K':
         return &MAGNITUDE_K;
      case 'M':
         return &MAGNITUDE_M;
      case 'B':
         return &MAGNITUDE_B;
      case 'T':
         return &MAGNITUDE_T;
      default:
         return nullptr;
   }
}

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
/* If [start,end) begins with an IPv4 literal (4 dot-separated octets 0-255,
 * optionally followed by ':' + port), returns the index just past the dotted-quad
 * so the octets can be spelled; otherwise returns start (not an IP host). */
static inline size_t domain_ipv4_end(const char *src, size_t start, size_t end) {
   size_t i = start;
   for (int octet = 0; octet < 4; octet++) {
      if (octet > 0) {
         if (i >= end || src[i] != '.')
            return start;
         i++;
      }
      int val = 0, nd = 0;
      while (i < end && nd < 3 && std::isdigit((unsigned char)src[i])) {
         val = val * 10 + (src[i] - '0');
         i++;
         nd++;
      }
      if (nd == 0 || val > 255)
         return start;
   }
   if (i != end && src[i] != ':') /* clean host: nothing, or a ':port' follows */
      return start;
   return i;
}

/* Render a URL domain to spoken form. When @out is non-null it writes and returns
 * the byte count; when null it only computes the size — so extract_domain_info
 * (size) and write_spoken_domain (write) share ONE implementation and cannot
 * diverge under the two-pass invariant. Dots become " dot "; an IPv4 host's
 * octets are spelled digit-by-digit ("1 9 2 dot 1 6 8 dot 1 dot 1"), matching how
 * a bare IP is read; any ':port' tail and normal-domain letters are copied. */
static inline size_t render_spoken_domain(const char *src, size_t start, size_t end, char *out) {
   const size_t ip_end = domain_ipv4_end(src, start, end); /* == start if not an IP */
   size_t pos = 0;
   bool first_digit = true;
   for (size_t i = start; i < end; i++) {
      char c = src[i];
      if (c == '.') {
         if (out)
            std::memcpy(out + pos, " dot ", 5);
         pos += 5;
         first_digit = true;
      } else if (i < ip_end && std::isdigit((unsigned char)c)) {
         if (!first_digit) {
            if (out)
               out[pos] = ' ';
            pos++;
         }
         if (out)
            out[pos] = c;
         pos++;
         first_digit = false;
      } else {
         if (out)
            out[pos] = c;
         pos++;
         first_digit = false;
      }
   }
   return pos;
}

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

   return render_spoken_domain(src, start, end, nullptr); /* size only */
}

/**
 * @brief Write domain in spoken form (dots -> " dot "; IPv4 host spelled).
 */
static inline size_t write_spoken_domain(const char *src,
                                         size_t domain_start,
                                         size_t domain_end,
                                         char *out) {
   return render_spoken_domain(src, domain_start, domain_end, out);
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
 * @brief Emit currency expansion: digits + magnitude + space + currency name
 *
 * Shared helper used by both the ASCII ($) and UTF-8 (£/€/¥) currency handlers
 * to avoid duplicating the emit logic in the two-pass template.
 */
template<PassMode mode> static inline void emit_currency(char *out,
                                                         size_t &out_pos,
                                                         const char *src,
                                                         size_t digit_start,
                                                         const currency_scan_t &scan,
                                                         const currency_info_t *cur) {
   if constexpr (mode == PassMode::GenerateOutput) {
      std::memcpy(out + out_pos, src + digit_start, scan.num_len);
   }
   out_pos += scan.num_len;

   if (scan.magnitude != '\0') {
      const magnitude_info_t *mag = get_magnitude(scan.magnitude);
      if (mag) {
         if constexpr (mode == PassMode::GenerateOutput) {
            std::memcpy(out + out_pos, mag->word, mag->len);
         }
         out_pos += mag->len;
      }
   }

   if constexpr (mode == PassMode::GenerateOutput) {
      out[out_pos] = ' ';
   }
   out_pos++;

   const char *name = scan.is_singular ? cur->singular : cur->plural;
   uint8_t name_len = scan.is_singular ? cur->singular_len : cur->plural_len;
   if constexpr (mode == PassMode::GenerateOutput) {
      std::memcpy(out + out_pos, name, name_len);
   }
   out_pos += name_len;
}

/* Integer runs with at least this many digits are spoken as words (e.g. a
 * factorial result), since espeak reads larger numbers awkwardly.  Below it,
 * small numbers — years, counts, prices, times — are left to espeak untouched.
 * 13 digits == trillions, safely clear of any year/time/ordinal. */
#define NUM2WORDS_TTS_MIN_DIGITS 13

/* Largest numeric token (digits + commas + optional decimal) we will buffer.
 * number_to_words caps at ~93 integer digits; this clears that with separators. */
#define NUM2WORDS_TTS_MAX_TOKEN 256

/**
 * @brief If a long numeric token starts at src[start], emit it as spoken words.
 *
 * Scans a number (digits, thousands commas, optional decimal part); when it has
 * >= NUM2WORDS_TTS_MIN_DIGITS integer digits it is replaced with words via
 * number_to_words().  Two-pass safe: number_to_words() is deterministic, so both
 * passes compute the same length.  Returns the number of INPUT bytes consumed,
 * or 0 when the token is too short / too large / unconvertible — in which case
 * the caller copies it verbatim (espeak handles it).
 */
template<PassMode mode> static size_t emit_large_number(char *out,
                                                        size_t &out_pos,
                                                        const char *src,
                                                        size_t start,
                                                        size_t len) {
   size_t p = start;
   if ((src[p] == '-' || src[p] == '+') && p + 1 < len && src[p + 1] >= '0' && src[p + 1] <= '9') {
      p++; /* leading sign — number_to_words renders '-' as "negative" */
   }
   size_t int_digits = 0;
   while (p < len) {
      char c = src[p];
      if (c >= '0' && c <= '9') {
         int_digits++;
         p++;
      } else if (c == ',' && p + 1 < len && src[p + 1] >= '0' && src[p + 1] <= '9') {
         p++; /* thousands separator between digits */
      } else {
         break;
      }
   }
   if (int_digits < NUM2WORDS_TTS_MIN_DIGITS) {
      return 0;
   }

   /* Optional decimal part — only when '.' is followed by a digit (not the
    * period that ends a sentence). */
   size_t tok_end = p;
   if (p < len && src[p] == '.' && p + 1 < len && src[p + 1] >= '0' && src[p + 1] <= '9') {
      tok_end = p + 1;
      while (tok_end < len && src[tok_end] >= '0' && src[tok_end] <= '9') {
         tok_end++;
      }
   }

   size_t tok_len = tok_end - start;
   if (tok_len >= NUM2WORDS_TTS_MAX_TOKEN) {
      return 0;
   }

   char token[NUM2WORDS_TTS_MAX_TOKEN];
   std::memcpy(token, src + start, tok_len);
   token[tok_len] = '\0';

   char words[NUM2WORDS_MIN_BUFFER];
   if (number_to_words(token, words, sizeof(words)) != NUM2WORDS_OK) {
      return 0; /* beyond the scale table or unconvertible — leave as-is */
   }

   size_t wlen = std::strlen(words);
   if constexpr (mode == PassMode::GenerateOutput) {
      std::memcpy(out + out_pos, words, wlen);
   }
   out_pos += wlen;
   return tok_len;
}

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

   // Skip leading whitespace
   while (i < len && (src[i] == ' ' || src[i] == '\t' || src[i] == '\n' || src[i] == '\r')) {
      i++;
   }

   // Skip leading markdown markers:
   // - Bullet markers: "- ", "* ", "1. ", "2. ", etc.
   // - Header markers: "# ", "## ", "### ", etc. (1-6 hashes)
   if (i < len) {
      if (src[i] == '#') {
         // Markdown header: skip 1-6 '#' characters followed by a space
         size_t j = i;
         while (j < len && j - i < 6 && src[j] == '#') {
            j++;
         }
         if (j < len && src[j] == ' ') {
            i = j + 1;
         }
      } else if (src[i] == '-' && i + 1 < len && src[i + 1] == ' ') {
         i += 2;
      } else if (src[i] == '*' && i + 1 < len && src[i + 1] == ' ') {
         i += 2;
      } else if (src[i] >= '0' && src[i] <= '9') {
         // Numbered list: "1. ", "12. ", etc.
         size_t j = i;
         while (j < len && src[j] >= '0' && src[j] <= '9') {
            j++;
         }
         if (j < len && src[j] == '.' && j + 1 < len && src[j + 1] == ' ') {
            i = j + 2;
         }
      }
   }

   while (i < len) {
      unsigned char byte = src[i];

      // === Handle ASCII (fast path) ===
      if (byte < 0x80) {
         // Skip asterisk (markdown bold markers)
         if (byte == '*') {
            i++;
            continue;
         }

         // Collapse runs of 2+ '#' to nothing.  The leading-markdown scrubber
         // above this loop already handles `# heading` at line start, but
         // inline runs like `### heading mid-sentence` or `##` without a
         // trailing space slip through and Piper reads each as "hash"
         // ("hash hash hash heading").  Single '#' is preserved so C#,
         // hashtags, and "#1" still read naturally.
         if (byte == '#' && i + 1 < len && src[i + 1] == '#') {
            while (i < len && src[i] == '#') {
               i++;
            }
            continue;
         }

         // Check for state abbreviation (2 uppercase letters at boundary)
         if (std::isupper(byte) && i + 1 < len &&
             std::isupper(static_cast<unsigned char>(src[i + 1]))) {
            bool valid_before = is_valid_state_before(src, i);
            bool valid_after = (i + 2 >= len) || is_state_boundary(src[i + 2]);

            if (valid_before && valid_after) {
               const state_entry_t *state = lookup_state(src[i], src[i + 1]);
               // Codes that collide with common words/acronyms (ID, OK, OR, ...)
               // expand only with positive context; otherwise fall through so
               // espeak speaks the letters/word. Pure functions of src => both
               // size-calc and generate passes decide identically.
               bool ctx_ok = state &&
                             (!state_code_needs_context(src[i], src[i + 1]) ||
                              state_city_comma_before(src, i) || state_comma_zip(src, i, len));
               if (ctx_ok) {
                  if constexpr (mode == PassMode::GenerateOutput) {
                     std::memcpy(out + out_pos, state->full_name, state->full_len);
                  }
                  out_pos += state->full_len;
                  i += 2;
                  // A 5-digit ZIP right after a state is an address ZIP: spell it
                  // digit-by-digit ("Idaho 8 3 7 0 2"), not a cardinal. Pure src
                  // decision, so both passes advance out_pos identically.
                  size_t sp = (i < len && src[i] == ' ') ? 1 : 0;
                  size_t zd = i + sp;
                  int nd = 0;
                  while (zd + nd < len && nd < 6 && std::isdigit((unsigned char)src[zd + nd]))
                     nd++;
                  if (nd == 5 && (zd + 5 >= len || !std::isalnum((unsigned char)src[zd + 5]))) {
                     if (sp) {
                        if constexpr (mode == PassMode::GenerateOutput)
                           out[out_pos] = ' ';
                        out_pos++;
                     }
                     for (int k = 0; k < 5; k++) {
                        if (k) {
                           if constexpr (mode == PassMode::GenerateOutput)
                              out[out_pos] = ' ';
                           out_pos++;
                        }
                        if constexpr (mode == PassMode::GenerateOutput)
                           out[out_pos] = src[zd + k];
                        out_pos++;
                     }
                     i = zd + 5;
                  }
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

         // Double-hyphen "--" -> comma (ASCII em-dash equivalent)
         if (byte == '-' && i + 1 < len && src[i + 1] == '-') {
            if constexpr (mode == PassMode::GenerateOutput) {
               out[out_pos] = ',';
            }
            out_pos++;
            i += 2;
            continue;
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

         // Currency: $ followed by digits
         if (byte == '$' && i + 1 < len && src[i + 1] >= '0' && src[i + 1] <= '9') {
            currency_scan_t scan = scan_currency_number(src, i + 1, len);
            if (scan.num_len > 0) {
               emit_currency<mode>(out, out_pos, src, i + 1, scan, &CURRENCY_DOLLAR);
               i += 1 + scan.num_len + (scan.magnitude != '\0' ? 1 : 0);
               continue;
            }
         }

         // Tilde before number: ~100 -> "approximately 100"
         if (byte == '~' && i + 1 < len && src[i + 1] >= '0' && src[i + 1] <= '9') {
            static const char APPROX[] = "approximately ";
            static const uint8_t APPROX_LEN = 14;
            if constexpr (mode == PassMode::GenerateOutput) {
               std::memcpy(out + out_pos, APPROX, APPROX_LEN);
            }
            out_pos += APPROX_LEN;
            i++;  // skip '~', digits will be copied normally
            continue;
         }

         // Large number -> spoken words (e.g. a 68-digit factorial result).
         // Fire on a digit, or a sign directly before digits, at a clean token
         // start (not mid-identifier or after a '.') and only for big numbers;
         // small numbers stay with espeak.
         bool starts_number = (byte >= '0' && byte <= '9') ||
                              ((byte == '-' || byte == '+') && i + 1 < len && src[i + 1] >= '0' &&
                               src[i + 1] <= '9');
         if (starts_number) {
            bool boundary = (i == 0);
            if (!boundary) {
               char prev = src[i - 1];
               boundary = !((prev >= '0' && prev <= '9') || (prev >= 'a' && prev <= 'z') ||
                            (prev >= 'A' && prev <= 'Z') || prev == '.');
            }
            if (boundary) {
               size_t consumed = emit_large_number<mode>(out, out_pos, src, i, len);
               if (consumed > 0) {
                  i += consumed;
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

      // Currency symbols: £ (U+00A3), ¥ (U+00A5), € (U+20AC)
      if (codepoint == 0x00A3 || codepoint == 0x00A5 || codepoint == 0x20AC) {
         size_t after_symbol = i + char_bytes;
         if (after_symbol < len && src[after_symbol] >= '0' && src[after_symbol] <= '9') {
            currency_scan_t scan = scan_currency_number(src, after_symbol, len);
            if (scan.num_len > 0) {
               const currency_info_t *cur = (codepoint == 0x00A3)   ? &CURRENCY_POUND
                                            : (codepoint == 0x20AC) ? &CURRENCY_EURO
                                                                    : &CURRENCY_YEN;
               emit_currency<mode>(out, out_pos, src, after_symbol, scan, cur);
               i = after_symbol + scan.num_len + (scan.magnitude != '\0' ? 1 : 0);
               continue;
            }
         }
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
// Phone-number expansion (pre-pass)
// ============================================================================

/* Phone numbers must be read digit-by-digit ("+16786432695" -> "1 6 7 8 ...")
 * rather than as one cardinal ("sixteen billion ...").  A dedicated pre-pass
 * (rather than a branch in the two-pass core) keeps that delicate size-calc /
 * generate symmetry untouched: it rewrites the input string first, and the
 * rest of the pipeline runs on the result.
 *
 * Detection policy (deliberately conservative — a bare undelimited digit run is
 * NOT a phone number, so IDs/order numbers are never mis-spoken):
 *   - a '+'-prefixed run of PHONE_E164_MIN_DIGITS..PHONE_E164_MAX_DIGITS digits
 *     (E.164, e.g. "+16786432695", "+1 (678) 643-2695"), OR
 *   - a punctuated US number whose digit groups match exactly {3,3,4} (10-digit)
 *     or {1,3,3,4} with a leading "1" (11-digit), separated by "-.() " or a
 *     mid-number space.  The exact-shape rule (not just "every group <= 4")
 *     rejects IPs ({3,3,1,3}), ISO dates ({4,2,2}), and score lists ({2,2,...}).
 * 7-digit local numbers, bare runs, and everything else pass through unchanged. */
#define PHONE_E164_MIN_DIGITS 8
#define PHONE_E164_MAX_DIGITS 15
#define PHONE_US_DIGITS 10
#define PHONE_US_CC_DIGITS 11
#define PHONE_MAX_GROUPS 8
/* Longest scan window: nothing beyond 15 digits + a few separators can ever
 * match, so bounding the scan keeps the whole pre-pass O(n) even on adversarial
 * separator-dense input (e.g. "1.1.1.1...") instead of O(n^2). */
#define PHONE_MAX_SCAN 40

/* Result of scanning a candidate phone token. */
struct phone_scan_t {
   char digits[PHONE_E164_MAX_DIGITS + 1]; /* NUL-terminated, capped at max */
   int ndig;
   bool has_plus;
   bool has_sep;
   int groups[PHONE_MAX_GROUPS]; /* length of each digit group */
   int num_groups;
   size_t digit_end; /* index one past the LAST digit (emit/skip end) */
   bool valid;       /* false if over the scan/digit/group cap -> not a phone */
};

/* Scan a candidate phone token at s[start].  @p allow_space controls whether a
 * mid-number space counts as a group separator: the caller tries space=true
 * first (catches "678 643 2695" and "+1 (678) ...") and retries space=false on
 * reject (so "678-643-2695 2 times" doesn't merge the trailing " 2"). */
static void scan_phone_token(const std::string &s,
                             size_t start,
                             bool allow_space,
                             phone_scan_t &r) {
   r.ndig = 0;
   r.has_plus = false;
   r.has_sep = false;
   r.num_groups = 0;
   r.digit_end = start;
   r.valid = true;
   r.digits[0] = '\0';

   size_t i = start;
   const size_t limit = start + PHONE_MAX_SCAN;
   if (i < s.size() && s[i] == '+') {
      r.has_plus = true;
      i++;
   }

   int cur = 0; /* current group length */
   while (i < s.size() && i < limit) {
      unsigned char c = (unsigned char)s[i];
      if (std::isdigit(c)) {
         if (r.ndig >= PHONE_E164_MAX_DIGITS) {
            r.valid = false; /* longer than any phone number */
            break;
         }
         r.digits[r.ndig++] = (char)c;
         cur++;
         i++;
         r.digit_end = i;
      } else if (c == '-' || c == '.' || c == '(' || c == ')' ||
                 (allow_space && c == ' ' && i + 1 < s.size() &&
                  (std::isdigit((unsigned char)s[i + 1]) || s[i + 1] == '('))) {
         /* Separator: close the current group (a space counts only mid-number,
          * i.e. followed by a digit or a '(' area-code group). */
         if (cur > 0) {
            if (r.num_groups >= PHONE_MAX_GROUPS) {
               r.valid = false;
               break;
            }
            r.groups[r.num_groups++] = cur;
            cur = 0;
         }
         r.has_sep = true;
         i++;
      } else {
         break;
      }
   }
   if (cur > 0) {
      if (r.num_groups < PHONE_MAX_GROUPS)
         r.groups[r.num_groups++] = cur;
      else
         r.valid = false;
   }
   if (i >= limit)
      r.valid = false; /* overran the window -> not a phone */
   r.digits[r.ndig] = '\0';
}

/* Decide whether a scanned token is a phone number per the detection policy. */
static bool phone_accept(const phone_scan_t &r) {
   if (!r.valid || r.ndig == 0)
      return false;
   if (r.has_plus)
      return r.ndig >= PHONE_E164_MIN_DIGITS && r.ndig <= PHONE_E164_MAX_DIGITS;
   if (!r.has_sep)
      return false; /* bare undelimited run -> ordinary number */
   /* Exact US shapes: {3,3,4} (10 digits) or {1,3,3,4} with a literal leading 1. */
   if (r.num_groups == 3)
      return r.groups[0] == 3 && r.groups[1] == 3 && r.groups[2] == 4;
   if (r.num_groups == 4)
      return r.groups[0] == 1 && r.groups[1] == 3 && r.groups[2] == 3 && r.groups[3] == 4 &&
             r.digits[0] == '1';
   return false;
}

/* Rewrite phone numbers in @p input to space-separated digits, writing the
 * result to @p out.  Returns true iff at least one number was rewritten; on
 * false @p out is left untouched so the caller uses @p input directly (zero
 * allocation on the common no-phone path). */
static bool expand_phone_numbers_for_tts(const std::string &input, std::string &out) {
   const size_t n = input.size();
   bool started = false;

   size_t i = 0;
   while (i < n) {
      unsigned char c = (unsigned char)input[i];
      /* Attempt a match only at a token boundary: start-of-string, or after a
       * char that is neither alphanumeric nor a phone separator/currency sign.
       * Excluding a preceding '-'/'.'/'$' stops us from re-matching the tail of
       * a rejected number ("2-678-643-2695") or a currency-prefixed run. */
      char prev = (i == 0) ? '\0' : input[i - 1];
      bool boundary = (i == 0) || (!std::isalnum((unsigned char)prev) && prev != '-' &&
                                   prev != '.' && prev != '$');
      if (boundary && (c == '+' || c == '(' || std::isdigit(c))) {
         phone_scan_t r;
         bool ok = false;
         /* Try space-as-separator first, then retry without it. */
         for (int pass = 0; pass < 2 && !ok; pass++) {
            scan_phone_token(input, i, pass == 0, r);
            bool end_boundary = (r.digit_end >= n) ||
                                !std::isalnum((unsigned char)input[r.digit_end]);
            if (end_boundary && phone_accept(r))
               ok = true;
         }
         if (ok) {
            if (!started) {
               out.clear();
               out.reserve(n + 8);
               out.append(input, 0, i); /* copy the prefix scanned so far */
               started = true;
            }
            for (int k = 0; k < r.ndig; k++) {
               if (k)
                  out.push_back(' ');
               out.push_back(r.digits[k]);
            }
            i = r.digit_end;
            continue;
         }
      }
      if (started)
         out.push_back((char)c);
      i++;
   }

   return started;
}

// ============================================================================
// IPv4 verbalization pre-pass
// ============================================================================

/* Match a strict dotted-quad at @start: exactly 4 dot-separated groups of 1-3
 * digits, each 0-255. Rejected if it runs into more digits/letters or a 5th
 * ".<digit>" group (so "1.2.3.4.5", "999.1.1.1", "1.2.3", "3.14", "1.2.3.4x" are
 * NOT IPs). On success sets *end_out just past the last octet. */
static bool scan_ipv4(const std::string &s, size_t start, size_t *end_out) {
   const size_t n = s.size();
   size_t i = start;
   for (int octet = 0; octet < 4; octet++) {
      if (octet > 0) {
         if (i >= n || s[i] != '.')
            return false;
         i++;
      }
      int val = 0, ndig = 0;
      while (i < n && ndig < 3 && std::isdigit((unsigned char)s[i])) {
         val = val * 10 + (s[i] - '0');
         i++;
         ndig++;
      }
      if (ndig == 0 || val > 255)
         return false;
   }
   if (i < n) {
      if (std::isalnum((unsigned char)s[i]))
         return false; /* octet ran into a longer number/word */
      if (s[i] == '.' && i + 1 < n && std::isdigit((unsigned char)s[i + 1]))
         return false; /* a 5th group -> not an IP */
   }
   *end_out = i;
   return true;
}

/* Rewrite IPv4 literals so espeak reads them the way people say IPs: each octet's
 * digits spelled out individually and each '.' spoken as "dot" ("192.168.1.100"
 * -> "1 9 2 dot 1 6 8 dot 1 dot 1 0 0"). Runs before the phone pass and the main
 * loop. Does NOT fire when the digits are preceded by '/', ':' or '@' (an IP
 * inside a URL/host is left for the URL pass, which speaks the domain).
 * Zero-alloc when no IP is present. */
static bool expand_ips_for_tts(const std::string &input, std::string &out) {
   const size_t n = input.size();
   bool started = false;
   size_t i = 0;
   while (i < n) {
      unsigned char c = (unsigned char)input[i];
      char prev = (i == 0) ? '\0' : input[i - 1];
      bool boundary = (i == 0) || (!std::isalnum((unsigned char)prev) && prev != '.' &&
                                   prev != '-' && prev != '/' && prev != ':' && prev != '@');
      if (boundary && std::isdigit(c)) {
         size_t end = 0;
         if (scan_ipv4(input, i, &end)) {
            if (!started) {
               out.clear();
               out.reserve(n + 24);
               out.append(input, 0, i);
               started = true;
            }
            /* Spell each octet's digits ("1 9 2"), "dot" between octets. */
            bool first_digit = true;
            for (size_t p = i; p < end; p++) {
               if (input[p] == '.') {
                  out.append(" dot ");
                  first_digit = true;
               } else {
                  if (!first_digit)
                     out.push_back(' ');
                  out.push_back(input[p]);
                  first_digit = false;
               }
            }
            i = end;
            continue;
         }
      }
      if (started)
         out.push_back((char)c);
      i++;
   }
   return started;
}

// ============================================================================
// Acronym / term override pre-pass
// ============================================================================

/* Exact-case, whole-token substitutions for tokens Piper's espeak MIS-reads.
 * espeak already spells unknown all-caps as letter names (FBI -> "eff bee eye")
 * and reads dictionary acronyms as words (NASA, JSON, RAM), so this is NOT a
 * general speller — only the specific misreads. Spoken values use dotted
 * uppercase to force letter names (espeak reads a bare spaced "A" as the article
 * schwa, so never space letters), a plain lowercase word to force a word reading
 * (mac/sim), or a full expansion for units. Verify any new row with the phoneme
 * probe against Piper's espeak before adding it. */
struct tts_override_t {
   const char *term;
   uint8_t term_len;
   const char *spoken;
};

static const tts_override_t tts_overrides[] = {
   { "IPv4", 4, "I.P.v. four" },   /* dot-before-digit says "dot"; spell the number */
   { "IPv6", 4, "I.P.v. six" },    /* likewise                                      */
   { "GPIO", 4, "G.P.I.O." },      /* espeak: "jee pee oh"    */
   { "AWS", 3, "A.W.S." },         /* espeak: "oz"            */
   { "ETA", 3, "E.T.A." },         /* espeak: "eeta"          */
   { "ASR", 3, "A.S.R." },         /* espeak: "assa"          */
   { "OTA", 3, "O.T.A." },         /* espeak: "ota"           */
   { "FAQ", 3, "F.A.Q." },         /* espeak: "fack"          */
   { "MPH", 3, "miles per hour" }, /* unit expansion          */
   { "kHz", 3, "kilohertz" },      /* espeak: "kay aitch zed" */
   { "mAh", 3, "milliamp hours" }, /* espeak: "em ar"         */
   { "MAC", 3, "mac" },            /* force word, not letters */
   { "SIM", 3, "sim" },            /* force word              */
   { "UX", 2, "U.X." },            /* espeak: "ucks"          */
   { "CI", 2, "C.I." },            /* espeak: "sigh"          */
};

/* Replace whole-token override terms. A term matches only at a token boundary
 * (prev + next char non-alphanumeric), so "IPv4" matches in "(IPv4)"/"IPv4-only"
 * but "CI" never matches inside "CIRCUS". Case-sensitive. Zero-alloc on no match. */
static bool expand_tts_overrides(const std::string &input, std::string &out) {
   const size_t n = input.size();
   bool started = false;
   size_t i = 0;
   while (i < n) {
      char prev = (i == 0) ? '\0' : input[i - 1];
      bool boundary = (i == 0) || !std::isalnum((unsigned char)prev);
      if (boundary) {
         const tts_override_t *match = nullptr;
         for (const auto &o : tts_overrides) {
            if (i + o.term_len <= n && input.compare(i, o.term_len, o.term) == 0 &&
                (i + o.term_len == n || !std::isalnum((unsigned char)input[i + o.term_len]))) {
               match = &o;
               break;
            }
         }
         if (match) {
            if (!started) {
               out.clear();
               out.reserve(n + 32);
               out.append(input, 0, i);
               started = true;
            }
            out.append(match->spoken);
            i += match->term_len;
            continue;
         }
      }
      if (started)
         out.push_back(input[i]);
      i++;
   }
   return started;
}

// ============================================================================
// Main Preprocessing Function (Optimized Two-Pass)
// ============================================================================

std::string preprocess_text_for_tts(const std::string &input) {
   build_state_lookup_table();

   if (input.empty())
      return input;

   /* Pre-passes (each zero-alloc when it doesn't match): IPv4 literals -> spoken
    * octets + "dot", then phone numbers -> spaced digits, before the number/
    * currency passes. IP runs first so it owns the dotted-quad; phone rejects IP
    * shapes anyway. */
   std::string ip_buf;
   const std::string &ip_expanded = expand_ips_for_tts(input, ip_buf) ? ip_buf : input;

   std::string ovr_buf;
   const std::string &ovr_expanded = expand_tts_overrides(ip_expanded, ovr_buf) ? ovr_buf
                                                                                : ip_expanded;

   std::string phone_buf;
   const std::string &phone_expanded = expand_phone_numbers_for_tts(ovr_expanded, phone_buf)
                                           ? phone_buf
                                           : ovr_expanded;

   const char *src = phone_expanded.data();
   const size_t len = phone_expanded.length();

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
