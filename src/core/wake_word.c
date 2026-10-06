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
 * Wake word matching utilities shared between local session and WebUI always-on mode.
 * Extracted from dawn.c to enable reuse without depending on file-scope statics.
 */

#include "core/wake_word.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"

/* Wake word prefixes combined with ai_name at runtime */
static const char *s_wake_prefixes[] = { "hello ",    "okay ",         "alright ",
                                         "hey ",      "hi ",           "good evening ",
                                         "good day ", "good morning ", "yeah ",
                                         "k " };
#define NUM_WAKE_PREFIXES (sizeof(s_wake_prefixes) / sizeof(s_wake_prefixes[0]))
#define WAKE_WORD_BUF_SIZE 64

/* Built wake words (prefix + ai_name) */
static char s_wake_bufs[NUM_WAKE_PREFIXES][WAKE_WORD_BUF_SIZE];
static char *s_wake_words[NUM_WAKE_PREFIXES];
static size_t s_num_wake_words = 0;

/* Goodbye phrases */
static const char *s_goodbye_words[] = {
   "good bye", "goodbye", "good night", "bye", "quit", "exit"
};
#define NUM_GOODBYE (sizeof(s_goodbye_words) / sizeof(s_goodbye_words[0]))

/* Ignore phrases */
static const char *s_ignore_words[] = { "", "the", "cancel", "never mind", "nevermind", "ignore" };
#define NUM_IGNORE (sizeof(s_ignore_words) / sizeof(s_ignore_words[0]))

/* Cancel phrases, as wake_word_normalize() leaves them ("that's" → "thats").
 * The one list for every voice surface: the local mic, always-on WebUI and
 * satellites all stop on the same words. */
static const char *s_cancel_words[] = {
   "stop",     "stop it", "cancel",   "hold on",      "wait",           "never mind",
   "abort",    "pause",   "enough",   "disregard",    "no thanks",      "forget it",
   "leave it", "drop it", "stand by", "cease",        "interrupt",      "say no more",
   "shut up",  "silence", "zip it",   "thats enough", "enough already", "stop right there"
};
#define NUM_CANCEL (sizeof(s_cancel_words) / sizeof(s_cancel_words[0]))

void wake_word_init(const char *ai_name) {
   if (!ai_name || ai_name[0] == '\0') {
      OLOG_ERROR("wake_word_init: ai_name is empty");
      return;
   }

   for (size_t i = 0; i < NUM_WAKE_PREFIXES; i++) {
      snprintf(s_wake_bufs[i], WAKE_WORD_BUF_SIZE, "%s%s", s_wake_prefixes[i], ai_name);
      s_wake_words[i] = s_wake_bufs[i];
   }
   s_num_wake_words = NUM_WAKE_PREFIXES;

   OLOG_INFO("Wake words configured for '%s' (e.g., '%s', '%s')", ai_name, s_wake_words[0],
             s_wake_words[1]);
}

char *wake_word_normalize(const char *input) {
   if (!input) {
      return NULL;
   }

   size_t len = strlen(input);
   char *normalized = (char *)malloc(len + 1);
   if (!normalized) {
      return NULL;
   }

   size_t j = 0;
   for (size_t i = 0; i < len; i++) {
      char c = input[i];
      if (c >= 'A' && c <= 'Z') {
         normalized[j++] = c + 32;
      } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ') {
         normalized[j++] = c;
      }
      /* Skip punctuation and other characters */
   }
   normalized[j] = '\0';

   return normalized;
}

/**
 * Skip leading whitespace and punctuation in a string
 */
static const char *skip_whitespace_punct(const char *s) {
   while (*s != '\0' && (*s == ' ' || *s == '.' || *s == ',' || *s == '!' || *s == '?')) {
      s++;
   }
   return s;
}

/**
 * Map a position in normalized text back to the original text,
 * accounting for characters removed during normalization.
 */
static size_t map_normalized_to_original(const char *original, size_t norm_pos) {
   size_t orig_i = 0;
   size_t norm_i = 0;

   while (norm_i < norm_pos && original[orig_i] != '\0') {
      char c = original[orig_i];
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ') {
         norm_i++;
      }
      orig_i++;
   }
   return orig_i;
}

/* Whether normalized text is exactly one of a list's phrases.  Spaces around
 * it don't count: ASR text often starts with one (" Stop.", " Goodbye."). */
static bool match_phrase(const char *normalized, const char **list, size_t count) {
   while (*normalized == ' ') {
      normalized++;
   }
   size_t n = strlen(normalized);
   while (n > 0 && normalized[n - 1] == ' ') {
      n--;
   }
   for (size_t i = 0; i < count; i++) {
      if (strlen(list[i]) == n && strncmp(normalized, list[i], n) == 0) {
         return true;
      }
   }
   return false;
}

bool wake_word_is_cancel(const char *text) {
   char *normalized = wake_word_normalize(text);
   if (!normalized) {
      return false;
   }
   const bool cancel = match_phrase(normalized, s_cancel_words, NUM_CANCEL);
   free(normalized);
   return cancel;
}

/**
 * Shared core for wake_word_check() and wake_word_check_prefix().
 *
 * prefix_only=false  → substring search via strstr (voice ASR semantics).
 * prefix_only=true   → match only when a wake-word variant appears at
 *                      byte 0 of the normalized string (text-input
 *                      semantics; mandatory for untrusted input).
 *
 * Goodbye / ignore / cancel phrases are only checked in substring mode
 * — those are voice-state-machine routing decisions and don't apply to
 * a prefix-gated text path.
 */
static wake_word_result_t wake_word_check_internal(const char *text, bool prefix_only) {
   wake_word_result_t result = { 0 };

   if (!text || text[0] == '\0') {
      return result;
   }

   char *normalized = wake_word_normalize(text);
   if (!normalized) {
      return result;
   }

   if (!prefix_only) {
      result.is_goodbye = match_phrase(normalized, s_goodbye_words, NUM_GOODBYE);
      result.is_ignore = match_phrase(normalized, s_ignore_words, NUM_IGNORE);
      result.is_cancel = match_phrase(normalized, s_cancel_words, NUM_CANCEL);
   }

   /* In prefix mode, skip any leading whitespace on the normalized
    * string before the start-anchored check.  Punctuation is already
    * stripped by wake_word_normalize, but a leading space can remain
    * (e.g., "...Hey, friday" → " hey friday").  Treating leading
    * whitespace as not significant matches what the user expects from
    * "starts with the wake word" without weakening the gate — an
    * attacker who can craft " hey friday X" can already craft
    * "hey friday X". */
   const char *prefix_search_start = normalized;
   if (prefix_only) {
      while (*prefix_search_start == ' ') {
         prefix_search_start++;
      }
   }

   /* Search for wake word */
   for (size_t i = 0; i < s_num_wake_words; i++) {
      size_t wake_len = strlen(s_wake_words[i]);
      size_t norm_offset = 0;
      bool matched = false;

      if (prefix_only) {
         if (strncmp(prefix_search_start, s_wake_words[i], wake_len) == 0) {
            matched = true;
            norm_offset = (size_t)(prefix_search_start - normalized);
         }
      } else {
         char *found = strstr(normalized, s_wake_words[i]);
         if (found) {
            matched = true;
            norm_offset = (size_t)(found - normalized);
         }
      }

      if (matched) {
         result.detected = true;

         /* Map back to original text position after the wake word */
         size_t orig_after = map_normalized_to_original(text, norm_offset + wake_len);
         result.command = skip_whitespace_punct(text + orig_after);
         result.has_command = (*result.command != '\0');

         break;
      }
   }

   free(normalized);
   return result;
}

wake_word_result_t wake_word_check(const char *text) {
   return wake_word_check_internal(text, false);
}

wake_word_result_t wake_word_check_prefix(const char *text) {
   return wake_word_check_internal(text, true);
}
