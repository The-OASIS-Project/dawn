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
 * Memory Callback Implementation
 *
 * Handles the memory tool actions: search, remember, forget.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_similarity.h"
#include "memory/memory_types.h"
#include "mosquitto_comms.h"
#include "tools/time_utils.h"

/* =============================================================================
 * Guardrails: Blocked Patterns
 *
 * Patterns that should not be stored as facts to prevent prompt injection
 * and system manipulation attempts.
 *
 * Security note: These patterns are checked after unicode normalization and
 * whitespace collapsing to prevent bypass via obfuscation.
 * ============================================================================= */

static const char *MEMORY_BLOCKED_PATTERNS[] = {
   /* Imperative/instruction patterns */
   "whenever", "always", "never", "you should", "you must", "you need to", "you shall",
   "you have to", "you will", "you are to", "make sure", "ensure that", "be sure to",
   "don't forget",
   /* Negation/override patterns */
   "ignore", "forget", "disregard", "pretend", "act as if", "override", "bypass", "skip", "disable",
   /* System manipulation */
   "system prompt", "instructions", "guidelines", "rules", "constraints", "from now on",
   "in future", "going forward", "henceforth",
   /* Credential patterns */
   "password", "api key", "apikey", "token", "secret", "credential", "private key", "auth",
   "bearer",
   /* Role/persona manipulation */
   "you are", "your role", "your purpose", "your job", "your task", "act like", "behave as",
   "respond as", NULL
};

/* Common unicode lookalikes to normalize (Cyrillic, Greek, etc.) */
static const struct {
   const char *lookalike;
   char replacement;
} UNICODE_NORMALIZATIONS[] = { { "\xd0\xb0", 'a' }, /* Cyrillic а -> a */
                               { "\xd0\xb5", 'e' }, /* Cyrillic е -> e */
                               { "\xd0\xbe", 'o' }, /* Cyrillic о -> o */
                               { "\xd1\x80", 'p' }, /* Cyrillic р -> p */
                               { "\xd1\x81", 'c' }, /* Cyrillic с -> c */
                               { "\xd1\x85", 'x' }, /* Cyrillic х -> x */
                               { "\xd1\x83", 'y' }, /* Cyrillic у -> y */
                               { "\xce\xb1", 'a' }, /* Greek α -> a */
                               { "\xce\xb5", 'e' }, /* Greek ε -> e */
                               { "\xce\xbf", 'o' }, /* Greek ο -> o */
                               { NULL, 0 } };

/* Zero-width and invisible characters to strip */
static const char *ZERO_WIDTH_CHARS[] = { "\xe2\x80\x8b", /* Zero-width space U+200B */
                                          "\xe2\x80\x8c", /* Zero-width non-joiner U+200C */
                                          "\xe2\x80\x8d", /* Zero-width joiner U+200D */
                                          "\xef\xbb\xbf", /* BOM U+FEFF */
                                          "\xc2\xad",     /* Soft hyphen U+00AD */
                                          NULL };

/* =============================================================================
 * Helper: Normalize text for pattern matching
 *
 * Removes zero-width characters, normalizes unicode lookalikes to ASCII,
 * collapses multiple whitespace, and converts to lowercase.
 * ============================================================================= */

static char *normalize_for_matching(const char *text) {
   if (!text) {
      return NULL;
   }

   size_t len = strlen(text);
   /* Allocate enough for worst case (all single-byte after normalization) */
   char *result = malloc(len + 1);
   if (!result) {
      return NULL;
   }

   size_t out_idx = 0;
   size_t in_idx = 0;
   bool last_was_space = false;

   while (in_idx < len) {
      bool handled = false;

      /* Check for zero-width characters to strip */
      for (int i = 0; ZERO_WIDTH_CHARS[i] != NULL; i++) {
         size_t zw_len = strlen(ZERO_WIDTH_CHARS[i]);
         if (in_idx + zw_len <= len && memcmp(text + in_idx, ZERO_WIDTH_CHARS[i], zw_len) == 0) {
            in_idx += zw_len;
            handled = true;
            break;
         }
      }
      if (handled)
         continue;

      /* Check for unicode lookalikes to normalize */
      for (int i = 0; UNICODE_NORMALIZATIONS[i].lookalike != NULL; i++) {
         size_t ul_len = strlen(UNICODE_NORMALIZATIONS[i].lookalike);
         if (in_idx + ul_len <= len &&
             memcmp(text + in_idx, UNICODE_NORMALIZATIONS[i].lookalike, ul_len) == 0) {
            result[out_idx++] = UNICODE_NORMALIZATIONS[i].replacement;
            in_idx += ul_len;
            last_was_space = false;
            handled = true;
            break;
         }
      }
      if (handled)
         continue;

      /* Regular character processing */
      unsigned char c = (unsigned char)text[in_idx];

      /* Skip non-ASCII bytes we don't recognize (multi-byte UTF-8 start) */
      if (c >= 0x80) {
         /* Skip this UTF-8 sequence */
         if ((c & 0xE0) == 0xC0)
            in_idx += 2; /* 2-byte */
         else if ((c & 0xF0) == 0xE0)
            in_idx += 3; /* 3-byte */
         else if ((c & 0xF8) == 0xF0)
            in_idx += 4; /* 4-byte */
         else
            in_idx++; /* Invalid, skip one byte */
         continue;
      }

      /* Collapse whitespace */
      if (isspace(c)) {
         if (!last_was_space && out_idx > 0) {
            result[out_idx++] = ' ';
            last_was_space = true;
         }
         in_idx++;
         continue;
      }

      /* Convert to lowercase */
      result[out_idx++] = tolower(c);
      last_was_space = false;
      in_idx++;
   }

   /* Trim trailing space */
   if (out_idx > 0 && result[out_idx - 1] == ' ') {
      out_idx--;
   }

   result[out_idx] = '\0';
   return result;
}

/* =============================================================================
 * Helper: Check for blocked patterns
 * ============================================================================= */

static bool contains_blocked_pattern(const char *text) {
   if (!text) {
      return false;
   }

   /* Normalize text: remove zero-width chars, normalize lookalikes, collapse whitespace */
   char *normalized = normalize_for_matching(text);
   if (!normalized) {
      /* If normalization fails, be conservative and block */
      LOG_WARNING("memory_callback: normalization failed, blocking for safety");
      return true;
   }

   bool found = false;
   for (int i = 0; MEMORY_BLOCKED_PATTERNS[i] != NULL; i++) {
      if (strstr(normalized, MEMORY_BLOCKED_PATTERNS[i]) != NULL) {
         LOG_WARNING("memory_callback: blocked pattern detected: '%s'", MEMORY_BLOCKED_PATTERNS[i]);
         found = true;
         break;
      }
   }

   free(normalized);
   return found;
}

/* =============================================================================
 * Helper: Get user ID from current session
 * ============================================================================= */

static int get_current_user_id(void) {
#ifdef ENABLE_MULTI_CLIENT
   session_t *session = session_get_command_context();
   if (session) {
      /* For authenticated WebSocket sessions, use their user_id */
      if (session->metrics.user_id > 0) {
         return session->metrics.user_id;
      }
      /* For local voice sessions, use configured default user */
      if (session->type == SESSION_TYPE_LOCAL) {
         int user_id = g_config.memory.default_voice_user_id;
         return (user_id > 0) ? user_id : 1; /* Fallback to admin */
      }
   }
#endif
   /* Fallback for non-multi-client builds: use default voice user */
   int user_id = g_config.memory.default_voice_user_id;
   return (user_id > 0) ? user_id : 1;
}

/* =============================================================================
 * Helper: Format time difference
 * ============================================================================= */

static void format_time_ago(time_t timestamp, char *buf, size_t buf_size) {
   if (timestamp == 0) {
      snprintf(buf, buf_size, "unknown");
      return;
   }

   time_t now = time(NULL);
   time_t diff = now - timestamp;

   if (diff < 60) {
      snprintf(buf, buf_size, "just now");
   } else if (diff < 3600) {
      snprintf(buf, buf_size, "%ld min ago", diff / 60);
   } else if (diff < 86400) {
      snprintf(buf, buf_size, "%ld hours ago", diff / 3600);
   } else if (diff < 604800) {
      snprintf(buf, buf_size, "%ld days ago", diff / 86400);
   } else {
      snprintf(buf, buf_size, "%ld weeks ago", diff / 604800);
   }
}

/* =============================================================================
 * Helper: Tokenize search query into individual words
 *
 * Splits keywords on whitespace/punctuation, lowercases each token,
 * and skips single-character tokens (noise). Returns token count.
 * ============================================================================= */

#define MAX_SEARCH_TOKENS 8

static int tokenize_query(const char *keywords, char tokens[][64], int max_tokens) {
   if (!keywords || max_tokens <= 0) {
      return 0;
   }

   char buf[512];
   strncpy(buf, keywords, sizeof(buf) - 1);
   buf[sizeof(buf) - 1] = '\0';

   for (size_t i = 0; buf[i]; i++) {
      buf[i] = tolower((unsigned char)buf[i]);
   }

   int count = 0;
   char *saveptr = NULL;
   char *tok = strtok_r(buf, " \t\n\r,.;:!?\"'()[]{}/-", &saveptr);
   while (tok && count < max_tokens) {
      if (strlen(tok) > 1) {
         strncpy(tokens[count], tok, 63);
         tokens[count][63] = '\0';
         count++;
      }
      tok = strtok_r(NULL, " \t\n\r,.;:!?\"'()[]{}/-", &saveptr);
   }
   return count;
}

/* =============================================================================
 * Action: Search
 * ============================================================================= */

static char *memory_action_search(int user_id, const char *keywords) {
   if (!keywords || strlen(keywords) == 0) {
      return strdup("Please provide search keywords.");
   }

   /* Allocate result buffer */
   size_t buf_size = 4096;
   char *result = malloc(buf_size);
   if (!result) {
      return strdup("Memory search failed: out of memory.");
   }
   result[0] = '\0';
   size_t offset = 0;

   /* Tokenize query for per-word matching */
   char tokens[MAX_SEARCH_TOKENS][64];
   int token_count = tokenize_query(keywords, tokens, MAX_SEARCH_TOKENS);

   /* Search facts */
   memory_fact_t facts[10];
   int fact_count = 0;

   if (token_count <= 1) {
      /* Single word or empty: use original single-call path */
      fact_count = memory_db_fact_search(user_id, keywords, facts, 10);
   } else {
      /* Multi-word: search per token, dedup by ID, rank by match count */
      int64_t seen_ids[50];
      int seen_scores[50];
      memory_fact_t seen_facts[50];
      int seen_count = 0;

      for (int t = 0; t < token_count; t++) {
         memory_fact_t token_results[10];
         int n = memory_db_fact_search(user_id, tokens[t], token_results, 10);
         for (int j = 0; j < n; j++) {
            int found = -1;
            for (int k = 0; k < seen_count; k++) {
               if (seen_ids[k] == token_results[j].id) {
                  found = k;
                  break;
               }
            }
            if (found >= 0) {
               seen_scores[found]++;
            } else if (seen_count < 50) {
               seen_ids[seen_count] = token_results[j].id;
               seen_scores[seen_count] = 1;
               seen_facts[seen_count] = token_results[j];
               seen_count++;
            }
         }
      }

      /* Insertion sort by score desc, then confidence desc */
      for (int i = 1; i < seen_count; i++) {
         int64_t tmp_id = seen_ids[i];
         int tmp_score = seen_scores[i];
         memory_fact_t tmp_fact = seen_facts[i];
         int j = i - 1;
         while (j >= 0 &&
                (seen_scores[j] < tmp_score ||
                 (seen_scores[j] == tmp_score && seen_facts[j].confidence < tmp_fact.confidence))) {
            seen_ids[j + 1] = seen_ids[j];
            seen_scores[j + 1] = seen_scores[j];
            seen_facts[j + 1] = seen_facts[j];
            j--;
         }
         seen_ids[j + 1] = tmp_id;
         seen_scores[j + 1] = tmp_score;
         seen_facts[j + 1] = tmp_fact;
      }

      /* Take top 10 */
      fact_count = (seen_count > 10) ? 10 : seen_count;
      for (int i = 0; i < fact_count; i++) {
         facts[i] = seen_facts[i];
      }
   }

   if (fact_count > 0) {
      offset += snprintf(result + offset, buf_size - offset, "FACTS (%d):\n", fact_count);
      for (int i = 0; i < fact_count && offset < buf_size - 100; i++) {
         char time_str[32];
         format_time_ago(facts[i].created_at, time_str, sizeof(time_str));
         offset += snprintf(result + offset, buf_size - offset, "- %s (confidence: %.0f%%, %s)\n",
                            facts[i].fact_text, facts[i].confidence * 100, time_str);

         /* Update access time */
         memory_db_fact_update_access(facts[i].id);
      }
   }

   /* Search preferences */
   memory_preference_t prefs[10];
   int pref_count = memory_db_pref_list(user_id, prefs, 10);

   if (pref_count > 0) {
      /* Filter prefs that match any query token */
      int matches = 0;

      /* Precompute lowercase keywords for single-token fallback */
      char lower_kw[256];
      strncpy(lower_kw, keywords, sizeof(lower_kw) - 1);
      lower_kw[sizeof(lower_kw) - 1] = '\0';
      for (size_t ci = 0; lower_kw[ci]; ci++) {
         lower_kw[ci] = tolower((unsigned char)lower_kw[ci]);
      }

      if (offset > 0 && offset < buf_size - 20) {
         offset += snprintf(result + offset, buf_size - offset, "\n");
      }

      for (int i = 0; i < pref_count && offset < buf_size - 100; i++) {
         char lower_cat[MEMORY_CATEGORY_MAX];
         strncpy(lower_cat, prefs[i].category, MEMORY_CATEGORY_MAX - 1);
         lower_cat[MEMORY_CATEGORY_MAX - 1] = '\0';
         for (size_t j = 0; j < strlen(lower_cat); j++) {
            lower_cat[j] = tolower((unsigned char)lower_cat[j]);
         }

         char lower_val[MEMORY_PREF_VALUE_MAX];
         strncpy(lower_val, prefs[i].value, MEMORY_PREF_VALUE_MAX - 1);
         lower_val[MEMORY_PREF_VALUE_MAX - 1] = '\0';
         for (size_t j = 0; j < strlen(lower_val); j++) {
            lower_val[j] = tolower((unsigned char)lower_val[j]);
         }

         bool matched = false;
         if (token_count >= 2) {
            /* Multi-word: match if ANY token appears in category or value */
            for (int t = 0; t < token_count && !matched; t++) {
               if (strstr(lower_cat, tokens[t]) || strstr(lower_val, tokens[t])) {
                  matched = true;
               }
            }
         } else {
            /* Single/no tokens: original full-keyword match */
            matched = strstr(lower_cat, lower_kw) || strstr(lower_val, lower_kw);
         }

         if (matched) {
            if (matches == 0) {
               offset += snprintf(result + offset, buf_size - offset, "PREFERENCES:\n");
            }
            offset += snprintf(result + offset, buf_size - offset,
                               "- %s: %s (reinforced %d times)\n", prefs[i].category,
                               prefs[i].value, prefs[i].reinforcement_count);
            matches++;
         }
      }
   }

   /* Search summaries */
   memory_summary_t summaries[5];
   int summary_count = 0;

   if (token_count <= 1) {
      summary_count = memory_db_summary_search(user_id, keywords, summaries, 5);
   } else {
      /* Multi-word: search per token, dedup by ID */
      int64_t seen_sum_ids[20];
      int seen_sum_count = 0;

      for (int t = 0; t < token_count && summary_count < 5; t++) {
         memory_summary_t token_results[5];
         int n = memory_db_summary_search(user_id, tokens[t], token_results, 5);
         for (int j = 0; j < n && summary_count < 5; j++) {
            bool dup = false;
            for (int k = 0; k < seen_sum_count; k++) {
               if (seen_sum_ids[k] == token_results[j].id) {
                  dup = true;
                  break;
               }
            }
            if (!dup) {
               if (seen_sum_count < 20) {
                  seen_sum_ids[seen_sum_count++] = token_results[j].id;
               }
               summaries[summary_count++] = token_results[j];
            }
         }
      }
   }

   if (summary_count > 0 && offset < buf_size - 100) {
      if (offset > 0) {
         offset += snprintf(result + offset, buf_size - offset, "\n");
      }
      offset += snprintf(result + offset, buf_size - offset, "CONVERSATION SUMMARIES (%d):\n",
                         summary_count);
      for (int i = 0; i < summary_count && offset < buf_size - 200; i++) {
         char time_str[32];
         format_time_ago(summaries[i].created_at, time_str, sizeof(time_str));
         offset += snprintf(result + offset, buf_size - offset, "- [%s] %s\n  Topics: %s\n",
                            time_str, summaries[i].summary, summaries[i].topics);
      }
   }

   if (offset == 0) {
      snprintf(result, buf_size, "No memories found matching '%s'.", keywords);
   }

   return result;
}

/* =============================================================================
 * Action: Remember
 * ============================================================================= */

static char *memory_action_remember(int user_id, const char *fact_text) {
   if (!fact_text || strlen(fact_text) == 0) {
      return strdup("Please provide the fact to remember.");
   }

   if (strlen(fact_text) > MEMORY_FACT_TEXT_MAX - 1) {
      return strdup("The fact is too long. Please keep it under 500 characters.");
   }

   /* Check for blocked patterns */
   if (contains_blocked_pattern(fact_text)) {
      return strdup("I cannot store that as a fact. It contains patterns that could affect my "
                    "behavior in unintended ways.");
   }

   /* Stage 1: Fast hash-based duplicate check */
   uint32_t fact_hash = memory_normalize_and_hash(fact_text);

   if (fact_hash != 0) {
      memory_fact_t hash_matches[5];
      int hash_count = memory_db_fact_find_by_hash(user_id, fact_hash, hash_matches, 5);

      if (hash_count > 0) {
         /* Verify with Jaccard similarity (handles hash collisions) */
         for (int i = 0; i < hash_count; i++) {
            if (memory_is_duplicate(fact_text, hash_matches[i].fact_text,
                                    MEMORY_SIMILARITY_THRESHOLD)) {
               /* Exact or near-exact duplicate - reinforce confidence */
               float new_conf = hash_matches[i].confidence + 0.1f;
               if (new_conf > 1.0f)
                  new_conf = 1.0f;
               memory_db_fact_update_confidence(hash_matches[i].id, new_conf);
               LOG_INFO("memory_callback: duplicate detected (hash match), reinforced fact %ld",
                        (long)hash_matches[i].id);
               return strdup("I already know that. Increased my confidence in this fact.");
            }
         }
      }
   }

   /* Stage 2: SQL LIKE search for potential fuzzy duplicates */
   memory_fact_t similar[5];
   int similar_count = memory_db_fact_find_similar(user_id, fact_text, similar, 5);

   if (similar_count > 0) {
      /* Check Jaccard similarity on candidates */
      for (int i = 0; i < similar_count; i++) {
         float similarity = memory_jaccard_similarity(fact_text, similar[i].fact_text);
         if (similarity >= MEMORY_SIMILARITY_THRESHOLD) {
            /* Similar enough to be considered a duplicate */
            float new_conf = similar[i].confidence + 0.1f;
            if (new_conf > 1.0f)
               new_conf = 1.0f;
            memory_db_fact_update_confidence(similar[i].id, new_conf);
            LOG_INFO("memory_callback: duplicate detected (Jaccard=%.2f), reinforced fact %ld",
                     similarity, (long)similar[i].id);
            return strdup(
                "I already know something similar. Increased my confidence in that fact.");
         }
      }
   }

   /* No duplicates found - store the new fact */
   int64_t fact_id = memory_db_fact_create(user_id, fact_text, 1.0f, "explicit");

   if (fact_id < 0) {
      return strdup("Failed to store the fact. Please try again.");
   }

   char *result = malloc(256);
   if (result) {
      snprintf(result, 256, "Remembered: \"%s\"", fact_text);
   }
   return result ? result : strdup("Fact stored successfully.");
}

/* =============================================================================
 * Action: Forget
 * ============================================================================= */

static char *memory_action_forget(int user_id, const char *fact_text) {
   if (!fact_text || strlen(fact_text) == 0) {
      return strdup("Please specify what to forget.");
   }

   /* Tokenize query for per-word matching */
   char tokens[MAX_SEARCH_TOKENS][64];
   int token_count = tokenize_query(fact_text, tokens, MAX_SEARCH_TOKENS);

   /* Search for matching facts */
   memory_fact_t facts[5];
   int count = 0;

   if (token_count <= 1) {
      count = memory_db_fact_search(user_id, fact_text, facts, 5);
   } else {
      /* Multi-word: search per token, dedup, pick best match */
      int64_t seen_ids[30];
      int seen_scores[30];
      memory_fact_t seen_facts[30];
      int seen_count = 0;

      for (int t = 0; t < token_count; t++) {
         memory_fact_t token_results[5];
         int n = memory_db_fact_search(user_id, tokens[t], token_results, 5);
         for (int j = 0; j < n; j++) {
            int found = -1;
            for (int k = 0; k < seen_count; k++) {
               if (seen_ids[k] == token_results[j].id) {
                  found = k;
                  break;
               }
            }
            if (found >= 0) {
               seen_scores[found]++;
            } else if (seen_count < 30) {
               seen_ids[seen_count] = token_results[j].id;
               seen_scores[seen_count] = 1;
               seen_facts[seen_count] = token_results[j];
               seen_count++;
            }
         }
      }

      /* Insertion sort by score desc, then confidence desc */
      for (int i = 1; i < seen_count; i++) {
         int64_t tmp_id = seen_ids[i];
         int tmp_score = seen_scores[i];
         memory_fact_t tmp_fact = seen_facts[i];
         int j = i - 1;
         while (j >= 0 &&
                (seen_scores[j] < tmp_score ||
                 (seen_scores[j] == tmp_score && seen_facts[j].confidence < tmp_fact.confidence))) {
            seen_ids[j + 1] = seen_ids[j];
            seen_scores[j + 1] = seen_scores[j];
            seen_facts[j + 1] = seen_facts[j];
            j--;
         }
         seen_ids[j + 1] = tmp_id;
         seen_scores[j + 1] = tmp_score;
         seen_facts[j + 1] = tmp_fact;
      }

      count = (seen_count > 5) ? 5 : seen_count;
      for (int i = 0; i < count; i++) {
         facts[i] = seen_facts[i];
      }
   }

   if (count == 0) {
      return strdup("No matching facts found to forget.");
   }

   /* Delete the most relevant match (highest word-match count) */
   int result = memory_db_fact_delete(facts[0].id, user_id);

   if (result == MEMORY_DB_SUCCESS) {
      char *msg = malloc(256);
      if (msg) {
         /* Truncate fact_text to fit in message buffer */
         snprintf(msg, 256, "Forgotten: \"%.200s%s\"", facts[0].fact_text,
                  strlen(facts[0].fact_text) > 200 ? "..." : "");
         return msg;
      }
      return strdup("Fact forgotten successfully.");
   } else {
      return strdup("Failed to forget the fact. Please try again.");
   }
}

/* =============================================================================
 * Action: Recent
 *
 * Returns facts and summaries created within a specified time period.
 * ============================================================================= */

static char *memory_action_recent(int user_id, const char *period) {
   if (!period || strlen(period) == 0) {
      return strdup("Please specify a time period (e.g., '24h', '7d', '1w').");
   }

   time_t seconds = parse_time_period(period);
   if (seconds <= 0) {
      return strdup("Invalid time period. Use format like '24h', '7d', '1w', or '30m'.");
   }

   time_t since = time(NULL) - seconds;

   /* Allocate result buffer */
   size_t buf_size = 4096;
   char *result = malloc(buf_size);
   if (!result) {
      return strdup("Memory query failed: out of memory.");
   }
   result[0] = '\0';
   size_t offset = 0;

   /* Get recent facts */
   memory_fact_t facts[20];
   int fact_count = memory_db_fact_list(user_id, facts, 20, 0);
   int recent_facts = 0;

   for (int i = 0; i < fact_count; i++) {
      if (facts[i].created_at >= since) {
         if (recent_facts == 0) {
            offset += snprintf(result + offset, buf_size - offset, "RECENT FACTS:\n");
         }
         char time_str[32];
         format_time_ago(facts[i].created_at, time_str, sizeof(time_str));
         offset += snprintf(result + offset, buf_size - offset, "- %s (%s, %s)\n",
                            facts[i].fact_text, facts[i].source, time_str);
         recent_facts++;
         if (offset >= buf_size - 100)
            break;
      }
   }

   /* Get recent summaries */
   memory_summary_t summaries[10];
   int summary_count = memory_db_summary_list(user_id, summaries, 10);
   int recent_summaries = 0;

   for (int i = 0; i < summary_count && offset < buf_size - 200; i++) {
      if (summaries[i].created_at >= since) {
         if (recent_summaries == 0) {
            if (offset > 0) {
               offset += snprintf(result + offset, buf_size - offset, "\n");
            }
            offset += snprintf(result + offset, buf_size - offset, "RECENT CONVERSATIONS:\n");
         }
         char time_str[32];
         format_time_ago(summaries[i].created_at, time_str, sizeof(time_str));
         offset += snprintf(result + offset, buf_size - offset, "- [%s] %s\n  Topics: %s\n",
                            time_str, summaries[i].summary, summaries[i].topics);
         recent_summaries++;
      }
   }

   if (recent_facts == 0 && recent_summaries == 0) {
      snprintf(result, buf_size, "No memories found in the past %s.", period);
   } else {
      /* Add summary count at the end */
      if (offset < buf_size - 50) {
         offset += snprintf(result + offset, buf_size - offset,
                            "\nTotal: %d facts, %d conversations", recent_facts, recent_summaries);
      }
   }

   return result;
}

/* =============================================================================
 * Main Callback
 * ============================================================================= */

char *memoryCallback(const char *actionName, char *value, int *should_respond) {
   if (should_respond) {
      *should_respond = 1;
   }

   /* Check if memory system is enabled */
   if (!g_config.memory.enabled) {
      return strdup("Memory system is disabled.");
   }

   /* Get current user ID */
   int user_id = get_current_user_id();
   if (user_id <= 0) {
      return strdup("Memory system requires authentication. Please log in.");
   }

   if (!actionName) {
      return strdup("Invalid memory action.");
   }

   LOG_INFO("memory_callback: action='%s', value='%s', user_id=%d", actionName,
            value ? value : "(null)", user_id);

   if (strcmp(actionName, "search") == 0) {
      return memory_action_search(user_id, value);
   } else if (strcmp(actionName, "remember") == 0) {
      return memory_action_remember(user_id, value);
   } else if (strcmp(actionName, "forget") == 0) {
      return memory_action_forget(user_id, value);
   } else if (strcmp(actionName, "recent") == 0) {
      return memory_action_recent(user_id, value);
   } else {
      char *msg = malloc(128);
      if (msg) {
         snprintf(msg, 128, "Unknown memory action: '%s'", actionName);
         return msg;
      }
      return strdup("Unknown memory action.");
   }
}
