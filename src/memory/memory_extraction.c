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
 * Memory Extraction Implementation
 *
 * Extracts facts, preferences, and summaries from conversation history.
 */

#include "memory/memory_extraction.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "llm/llm_interface.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_types.h"

/* =============================================================================
 * Extraction Prompt Template
 * ============================================================================= */

static const char *EXTRACTION_PROMPT_TEMPLATE =
    "Analyze this conversation and extract user information in JSON format.\n\n"
    "CONVERSATION:\n%s\n\n"
    "EXISTING USER PROFILE:\n%s\n\n"
    "Extract the following and respond ONLY with valid JSON:\n"
    "{\n"
    "  \"facts\": [\n"
    "    {\"text\": \"factual statement about user\", \"source\": \"explicit|inferred\", "
    "\"confidence\": 0.0-1.0}\n"
    "  ],\n"
    "  \"preferences\": [\n"
    "    {\"category\": \"normalized_category\", \"value\": \"preference_value\", "
    "\"confidence\": 0.0-1.0}\n"
    "  ],\n"
    "  \"corrections\": [\n"
    "    {\"old_fact\": \"outdated statement\", \"new_fact\": \"corrected statement\"}\n"
    "  ],\n"
    "  \"summary\": \"1-2 sentence conversation summary\",\n"
    "  \"topics\": [\"topic1\", \"topic2\"]\n"
    "}\n\n"
    "Guidelines:\n"
    "- \"explicit\" source: user directly stated it\n"
    "- \"inferred\" source: reasonably deduced from context\n"
    "- Use normalized categories for preferences (e.g., \"theme\", \"units\", "
    "\"communication_style\")\n"
    "- Only include facts that are specific to this user, not general knowledge\n"
    "- High confidence (0.8-1.0) for explicit statements, lower for inferences\n"
    "- List corrections if new information contradicts existing profile\n"
    "- If nothing notable to extract, return empty arrays\n";

/* =============================================================================
 * Thread Context
 * ============================================================================= */

typedef struct {
   int user_id;
   int64_t conversation_id; /* For incremental extraction tracking */
   char session_id[MEMORY_SESSION_ID_MAX];
   char *conversation_json; /* Serialized conversation history */
   int message_count;       /* Total messages in conversation */
   int new_message_count;   /* Messages being extracted this time */
   int duration_seconds;
} extraction_context_t;

/* =============================================================================
 * Extraction Concurrency Tracking
 *
 * Tracks in-progress extractions using a bounded array instead of a bitmap.
 * This removes the 64-user limit and enables system-wide concurrency limiting
 * tied to max_clients configuration.
 * ============================================================================= */

#define MAX_EXTRACTION_SLOTS 16 /* Compile-time max; runtime limit is min(this, max_clients) */

static struct {
   int user_ids[MAX_EXTRACTION_SLOTS]; /* User IDs with active extractions (0 = empty slot) */
   int count;                          /* Current number of active extractions */
} s_extraction_state = { { 0 }, 0 };
static pthread_mutex_t s_extraction_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Helper: Get runtime concurrency limit */
static int get_max_concurrent_extractions(void) {
   int limit = g_config.webui.max_clients;
   if (limit <= 0 || limit > MAX_EXTRACTION_SLOTS) {
      limit = MAX_EXTRACTION_SLOTS;
   }
   return limit;
}

/* Helper: Check if user has active extraction (must hold mutex) */
static bool extraction_is_active_locked(int user_id) {
   for (int i = 0; i < MAX_EXTRACTION_SLOTS; i++) {
      if (s_extraction_state.user_ids[i] == user_id) {
         return true;
      }
   }
   return false;
}

/* Helper: Try to acquire extraction slot for user (must hold mutex)
 * Returns true if slot acquired, false if user already active or slots full */
static bool extraction_slot_acquire_locked(int user_id) {
   /* Check if user already has active extraction */
   if (extraction_is_active_locked(user_id)) {
      return false;
   }

   /* Check if we've hit the concurrency limit */
   int max_concurrent = get_max_concurrent_extractions();
   if (s_extraction_state.count >= max_concurrent) {
      return false;
   }

   /* Find empty slot and claim it */
   for (int i = 0; i < MAX_EXTRACTION_SLOTS; i++) {
      if (s_extraction_state.user_ids[i] == 0) {
         s_extraction_state.user_ids[i] = user_id;
         s_extraction_state.count++;
         return true;
      }
   }

   return false; /* Should not reach here if count is accurate */
}

/* Helper: Release extraction slot for user (must hold mutex) */
static void extraction_slot_release_locked(int user_id) {
   for (int i = 0; i < MAX_EXTRACTION_SLOTS; i++) {
      if (s_extraction_state.user_ids[i] == user_id) {
         s_extraction_state.user_ids[i] = 0;
         s_extraction_state.count--;
         return;
      }
   }
}

/* =============================================================================
 * Helper: Build existing profile string
 * ============================================================================= */

static char *build_existing_profile(int user_id) {
   char *profile = malloc(4096);
   if (!profile)
      return strdup("(none)");

   int offset = 0;

   /* Load existing preferences */
   memory_preference_t prefs[10];
   int pref_count = memory_db_pref_list(user_id, prefs, 10);

   if (pref_count > 0) {
      offset += snprintf(profile + offset, 4096 - offset, "Preferences:\n");
      for (int i = 0; i < pref_count && offset < 3900; i++) {
         offset += snprintf(profile + offset, 4096 - offset, "- %s: %s\n", prefs[i].category,
                            prefs[i].value);
      }
   }

   /* Load existing facts */
   memory_fact_t facts[10];
   int fact_count = memory_db_fact_list(user_id, facts, 10, 0);

   if (fact_count > 0) {
      if (offset > 0)
         offset += snprintf(profile + offset, 4096 - offset, "\n");
      offset += snprintf(profile + offset, 4096 - offset, "Known facts:\n");
      for (int i = 0; i < fact_count && offset < 3900; i++) {
         offset += snprintf(profile + offset, 4096 - offset, "- %s\n", facts[i].fact_text);
      }
   }

   if (offset == 0) {
      strcpy(profile, "(none)");
   }

   return profile;
}

/* =============================================================================
 * Helper: Extract JSON from LLM response
 *
 * LLMs often wrap JSON in markdown code blocks or include preamble text.
 * This function extracts the JSON object from such responses.
 * ============================================================================= */

static struct json_object *extract_json_from_response(const char *response) {
   if (!response || response[0] == '\0') {
      return NULL;
   }

   struct json_object *root = NULL;

   /* First try direct parse (pure JSON) */
   root = json_tokener_parse(response);
   if (root) {
      return root;
   }

   /* Look for JSON in markdown code block: ```json ... ``` or ``` ... ``` */
   const char *json_block_start = strstr(response, "```json");
   if (json_block_start) {
      json_block_start += 7; /* Skip "```json" */
   } else {
      json_block_start = strstr(response, "```");
      if (json_block_start) {
         json_block_start += 3; /* Skip "```" */
      }
   }

   if (json_block_start) {
      /* Skip any newline after opening ``` */
      while (*json_block_start == '\n' || *json_block_start == '\r') {
         json_block_start++;
      }

      const char *json_block_end = strstr(json_block_start, "```");
      if (json_block_end) {
         size_t len = json_block_end - json_block_start;
         char *json_str = malloc(len + 1);
         if (json_str) {
            memcpy(json_str, json_block_start, len);
            json_str[len] = '\0';
            root = json_tokener_parse(json_str);
            free(json_str);
            if (root) {
               return root;
            }
         }
      }
   }

   /* Last resort: find first '{' and try to parse from there */
   const char *brace = strchr(response, '{');
   if (brace) {
      root = json_tokener_parse(brace);
      if (root) {
         return root;
      }
   }

   return NULL;
}

/* =============================================================================
 * Helper: Parse extraction response
 * ============================================================================= */

static void process_extraction_response(int user_id,
                                        const char *session_id,
                                        const char *response_text,
                                        int message_count,
                                        int duration_seconds) {
   if (!response_text) {
      LOG_WARNING("memory_extraction: NULL response from LLM");
      return;
   }

   /* Extract JSON from response (handles markdown blocks, preamble text, etc.) */
   struct json_object *root = extract_json_from_response(response_text);
   if (!root) {
      LOG_WARNING("memory_extraction: Failed to parse LLM response as JSON");
      LOG_WARNING("memory_extraction: Response preview: %.200s...", response_text);
      return;
   }

   /* Process facts */
   struct json_object *facts_arr;
   if (json_object_object_get_ex(root, "facts", &facts_arr)) {
      int count = json_object_array_length(facts_arr);
      for (int i = 0; i < count; i++) {
         struct json_object *fact = json_object_array_get_idx(facts_arr, i);
         struct json_object *text_obj, *source_obj, *conf_obj;

         if (json_object_object_get_ex(fact, "text", &text_obj)) {
            const char *text = json_object_get_string(text_obj);
            const char *source = "inferred";
            float confidence = 0.7f;

            if (json_object_object_get_ex(fact, "source", &source_obj)) {
               source = json_object_get_string(source_obj);
            }
            if (json_object_object_get_ex(fact, "confidence", &conf_obj)) {
               confidence = (float)json_object_get_double(conf_obj);
            }

            /* Check for duplicates before storing */
            memory_fact_t similar[3];
            int similar_count = memory_db_fact_find_similar(user_id, text, similar, 3);

            if (similar_count == 0) {
               memory_db_fact_create(user_id, text, confidence, source);
               LOG_INFO("memory_extraction: stored fact: %s", text);
            } else {
               /* Update confidence of existing similar fact */
               float new_conf = similar[0].confidence + 0.1f;
               if (new_conf > 1.0f)
                  new_conf = 1.0f;
               memory_db_fact_update_confidence(similar[0].id, new_conf);
            }
         }
      }
   }

   /* Process preferences */
   struct json_object *prefs_arr;
   if (json_object_object_get_ex(root, "preferences", &prefs_arr)) {
      int count = json_object_array_length(prefs_arr);
      for (int i = 0; i < count; i++) {
         struct json_object *pref = json_object_array_get_idx(prefs_arr, i);
         struct json_object *cat_obj, *val_obj, *conf_obj;

         if (json_object_object_get_ex(pref, "category", &cat_obj) &&
             json_object_object_get_ex(pref, "value", &val_obj)) {
            const char *category = json_object_get_string(cat_obj);
            const char *value = json_object_get_string(val_obj);
            float confidence = 0.7f;

            if (json_object_object_get_ex(pref, "confidence", &conf_obj)) {
               confidence = (float)json_object_get_double(conf_obj);
            }

            memory_db_pref_upsert(user_id, category, value, confidence, "inferred");
            LOG_INFO("memory_extraction: stored preference: %s=%s", category, value);
         }
      }
   }

   /* Process corrections */
   struct json_object *corr_arr;
   if (json_object_object_get_ex(root, "corrections", &corr_arr)) {
      int count = json_object_array_length(corr_arr);
      for (int i = 0; i < count; i++) {
         struct json_object *corr = json_object_array_get_idx(corr_arr, i);
         struct json_object *old_obj, *new_obj;

         if (json_object_object_get_ex(corr, "old_fact", &old_obj) &&
             json_object_object_get_ex(corr, "new_fact", &new_obj)) {
            const char *old_fact = json_object_get_string(old_obj);
            const char *new_fact = json_object_get_string(new_obj);

            /* Find and supersede old fact */
            memory_fact_t similar[3];
            int similar_count = memory_db_fact_find_similar(user_id, old_fact, similar, 3);

            if (similar_count > 0) {
               /* Create new fact and supersede old one */
               int64_t new_id = memory_db_fact_create(user_id, new_fact, 0.9f, "explicit");
               if (new_id > 0) {
                  memory_db_fact_supersede(similar[0].id, new_id);
                  LOG_INFO("memory_extraction: corrected fact: %s -> %s", old_fact, new_fact);
               }
            }
         }
      }
   }

   /* Process summary */
   struct json_object *summary_obj, *topics_arr;
   if (json_object_object_get_ex(root, "summary", &summary_obj)) {
      const char *summary = json_object_get_string(summary_obj);

      /* Build topics string */
      char topics[MEMORY_TOPICS_MAX] = { 0 };
      if (json_object_object_get_ex(root, "topics", &topics_arr)) {
         int count = json_object_array_length(topics_arr);
         int offset = 0;
         for (int i = 0; i < count && offset < MEMORY_TOPICS_MAX - 32; i++) {
            const char *topic = json_object_get_string(json_object_array_get_idx(topics_arr, i));
            if (topic) {
               if (offset > 0) {
                  offset += snprintf(topics + offset, MEMORY_TOPICS_MAX - offset, ", ");
               }
               offset += snprintf(topics + offset, MEMORY_TOPICS_MAX - offset, "%s", topic);
            }
         }
      }

      memory_db_summary_create(user_id, session_id, summary, topics, "neutral", message_count,
                               duration_seconds);
      LOG_INFO("memory_extraction: stored summary for session %s", session_id);
   }

   json_object_put(root);
}

/* =============================================================================
 * Extraction Thread
 * ============================================================================= */

static void *extraction_thread(void *arg) {
   extraction_context_t *ctx = (extraction_context_t *)arg;

   LOG_INFO("memory_extraction: starting for user %d, session %s", ctx->user_id, ctx->session_id);

   /* Build existing profile */
   char *existing_profile = build_existing_profile(ctx->user_id);

   /* Build extraction prompt */
   size_t prompt_size = strlen(EXTRACTION_PROMPT_TEMPLATE) + strlen(ctx->conversation_json) +
                        strlen(existing_profile) + 100;
   char *prompt = malloc(prompt_size);
   if (!prompt) {
      LOG_ERROR("memory_extraction: failed to allocate prompt");
      goto cleanup;
   }

   snprintf(prompt, prompt_size, EXTRACTION_PROMPT_TEMPLATE, ctx->conversation_json,
            existing_profile);

   /* Call LLM for extraction using configured provider/model */
   char *response = NULL;

   /* Build a minimal conversation history with just the extraction prompt */
   struct json_object *extraction_history = json_object_new_array();
   struct json_object *user_msg = json_object_new_object();
   json_object_object_add(user_msg, "role", json_object_new_string("user"));
   json_object_object_add(user_msg, "content", json_object_new_string(prompt));
   json_object_array_add(extraction_history, user_msg);

   /* Build resolved config from memory extraction settings */
   llm_resolved_config_t extraction_config = { 0 };
   char model_buf[LLM_MODEL_NAME_MAX];
   char endpoint_buf[128];

   const char *provider = g_config.memory.extraction_provider;
   const char *model = g_config.memory.extraction_model;

   /* Copy model to local buffer */
   if (model && model[0] != '\0') {
      strncpy(model_buf, model, sizeof(model_buf) - 1);
      model_buf[sizeof(model_buf) - 1] = '\0';
      extraction_config.model = model_buf;
   }

   /* Determine provider type and configure accordingly */
   if (strcmp(provider, "local") == 0 || strcmp(provider, "ollama") == 0) {
      extraction_config.type = LLM_LOCAL;
      extraction_config.cloud_provider = CLOUD_PROVIDER_NONE;
      /* Use local endpoint from config */
      strncpy(endpoint_buf, g_config.llm.local.endpoint, sizeof(endpoint_buf) - 1);
      endpoint_buf[sizeof(endpoint_buf) - 1] = '\0';
      extraction_config.endpoint = endpoint_buf;
   } else if (strcmp(provider, "openai") == 0) {
      extraction_config.type = LLM_CLOUD;
      extraction_config.cloud_provider = CLOUD_PROVIDER_OPENAI;
      extraction_config.api_key = g_secrets.openai_api_key;
      extraction_config.endpoint = NULL; /* Use default */
   } else if (strcmp(provider, "claude") == 0) {
      extraction_config.type = LLM_CLOUD;
      extraction_config.cloud_provider = CLOUD_PROVIDER_CLAUDE;
      extraction_config.api_key = g_secrets.claude_api_key;
      extraction_config.endpoint = NULL; /* Use default */
   } else {
      /* Default to local if unknown - warn with valid options */
      LOG_WARNING("memory_extraction: unknown provider '%s' in config "
                  "(valid: local, ollama, openai, claude) - falling back to local",
                  provider);
      extraction_config.type = LLM_LOCAL;
      extraction_config.cloud_provider = CLOUD_PROVIDER_NONE;
      strncpy(endpoint_buf, g_config.llm.local.endpoint, sizeof(endpoint_buf) - 1);
      endpoint_buf[sizeof(endpoint_buf) - 1] = '\0';
      extraction_config.endpoint = endpoint_buf;
   }

   /* Disable tools and thinking for extraction - we just want JSON output */
   strncpy(extraction_config.tool_mode, "disabled", sizeof(extraction_config.tool_mode) - 1);
   strncpy(extraction_config.thinking_mode, "disabled",
           sizeof(extraction_config.thinking_mode) - 1);

   LOG_INFO("memory_extraction: using provider=%s, model=%s", provider,
            model ? model : "(default)");

   /* Use the configured LLM for extraction */
   response = llm_chat_completion_with_config(extraction_history, prompt, NULL, NULL, 0,
                                              &extraction_config);
   json_object_put(extraction_history);

   if (response) {
      process_extraction_response(ctx->user_id, ctx->session_id, response, ctx->new_message_count,
                                  ctx->duration_seconds);
      free(response);

      /* Update extraction high-water mark on success */
      if (ctx->conversation_id > 0) {
         memory_db_set_last_extracted(ctx->conversation_id, ctx->message_count);
         LOG_INFO("memory_extraction: updated high-water mark to %d for conversation %ld",
                  ctx->message_count, (long)ctx->conversation_id);
      }

      /* Run fact pruning if enabled */
      if (g_config.memory.pruning_enabled) {
         int pruned_superseded = memory_db_fact_prune_superseded(
             ctx->user_id, g_config.memory.prune_superseded_days);
         int pruned_stale = memory_db_fact_prune_stale(ctx->user_id,
                                                       g_config.memory.prune_stale_days,
                                                       g_config.memory.prune_stale_min_confidence);
         if (pruned_superseded > 0 || pruned_stale > 0) {
            LOG_INFO("memory_extraction: pruned %d superseded, %d stale facts for user %d",
                     pruned_superseded > 0 ? pruned_superseded : 0,
                     pruned_stale > 0 ? pruned_stale : 0, ctx->user_id);
         }
      }
   } else {
      LOG_WARNING("memory_extraction: LLM returned no response");
   }

   free(prompt);

cleanup:
   free(existing_profile);
   free(ctx->conversation_json);

   /* Clear in-progress flag - save user_id before freeing ctx */
   int saved_user_id = ctx->user_id;
   free(ctx);

   pthread_mutex_lock(&s_extraction_mutex);
   extraction_slot_release_locked(saved_user_id);
   pthread_mutex_unlock(&s_extraction_mutex);

   LOG_INFO("memory_extraction: completed for user %d", saved_user_id);
   return NULL;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int memory_trigger_extraction(int user_id,
                              int64_t conversation_id,
                              const char *session_id_str,
                              struct json_object *conversation_history,
                              int message_count,
                              int duration_seconds) {
   if (user_id <= 0 || !conversation_history) {
      return 1;
   }

   if (!g_config.memory.enabled) {
      return 1;
   }

#ifdef ENABLE_AUTH
   /* Re-check privacy status from database (prevents race condition with set_private) */
   if (conversation_id > 0) {
      int is_private = conv_db_is_private(conversation_id, user_id);
      if (is_private > 0) {
         LOG_INFO("memory_extraction: skipping - conversation %lld is private (DB check)",
                  (long long)conversation_id);
         return 0;
      }
      /* is_private == -1 means error or not found, proceed with extraction */
   }
#endif

   /* Skip if too few messages */
   if (message_count < 2) {
      LOG_INFO("memory_extraction: skipping - too few messages (%d)", message_count);
      return 0;
   }

   /* Check incremental extraction: how many messages were already processed? */
   int last_extracted = 0;
   if (conversation_id > 0) {
      last_extracted = memory_db_get_last_extracted(conversation_id);
      if (last_extracted < 0) {
         last_extracted = 0; /* Treat error as never extracted */
      }

      /* Skip if no new messages since last extraction */
      if (message_count <= last_extracted) {
         LOG_INFO("memory_extraction: skipping - no new messages (count=%d, last=%d)",
                  message_count, last_extracted);
         return 0;
      }
   }

   int new_messages = message_count - last_extracted;

   /* Check concurrency limits and acquire extraction slot */
   pthread_mutex_lock(&s_extraction_mutex);
   if (!extraction_slot_acquire_locked(user_id)) {
      /* Either user already has extraction in progress, or we've hit max concurrent */
      bool user_active = extraction_is_active_locked(user_id);
      int max_concurrent = get_max_concurrent_extractions();
      pthread_mutex_unlock(&s_extraction_mutex);

      if (user_active) {
         LOG_INFO("memory_extraction: already in progress for user %d", user_id);
      } else {
         LOG_INFO("memory_extraction: at capacity (%d/%d), skipping user %d",
                  s_extraction_state.count, max_concurrent, user_id);
      }
      return 0;
   }
   pthread_mutex_unlock(&s_extraction_mutex);

   /* Prepare context */
   extraction_context_t *ctx = calloc(1, sizeof(extraction_context_t));
   if (!ctx) {
      pthread_mutex_lock(&s_extraction_mutex);
      extraction_slot_release_locked(user_id);
      pthread_mutex_unlock(&s_extraction_mutex);
      return 1;
   }

   ctx->user_id = user_id;
   ctx->conversation_id = conversation_id;
   if (session_id_str) {
      strncpy(ctx->session_id, session_id_str, MEMORY_SESSION_ID_MAX - 1);
   } else {
      snprintf(ctx->session_id, MEMORY_SESSION_ID_MAX, "session_%ld", (long)time(NULL));
   }
   ctx->message_count = message_count;
   ctx->new_message_count = new_messages;
   ctx->duration_seconds = duration_seconds;

   /* Serialize conversation history (skip system messages, only include new messages) */
   size_t arr_len = json_object_array_length(conversation_history);
   struct json_object *filtered = json_object_new_array();

   /* Calculate start index: skip already-extracted messages
    * Account for the fact that arr_len includes system messages which we skip */
   int skipped_system = 0;
   int messages_seen = 0;

   for (size_t i = 0; i < arr_len; i++) {
      struct json_object *msg = json_object_array_get_idx(conversation_history, i);
      struct json_object *role_obj;
      if (json_object_object_get_ex(msg, "role", &role_obj)) {
         const char *role = json_object_get_string(role_obj);
         /* Skip system messages */
         if (strcmp(role, "system") == 0) {
            skipped_system++;
            continue;
         }

         messages_seen++;

         /* Only include messages after the last extraction point */
         if (messages_seen > last_extracted) {
            json_object_array_add(filtered, json_object_get(msg));
         }
      }
   }

   ctx->conversation_json = strdup(
       json_object_to_json_string_ext(filtered, JSON_C_TO_STRING_PLAIN));
   json_object_put(filtered);

   if (!ctx->conversation_json) {
      free(ctx);
      pthread_mutex_lock(&s_extraction_mutex);
      extraction_slot_release_locked(user_id);
      pthread_mutex_unlock(&s_extraction_mutex);
      return 1;
   }

   LOG_INFO("memory_extraction: extracting %d new messages (last=%d, total=%d)", new_messages,
            last_extracted, message_count);

   /* Spawn detached extraction thread */
   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

   int rc = pthread_create(&thread, &attr, extraction_thread, ctx);
   pthread_attr_destroy(&attr);

   if (rc != 0) {
      LOG_ERROR("memory_extraction: failed to create thread: %d", rc);
      free(ctx->conversation_json);
      free(ctx);
      pthread_mutex_lock(&s_extraction_mutex);
      extraction_slot_release_locked(user_id);
      pthread_mutex_unlock(&s_extraction_mutex);
      return 1;
   }

   LOG_INFO("memory_extraction: triggered for user %d", user_id);
   return 0;
}

bool memory_extraction_in_progress(int user_id) {
   if (user_id <= 0) {
      return false;
   }

   pthread_mutex_lock(&s_extraction_mutex);
   bool in_progress = extraction_is_active_locked(user_id);
   pthread_mutex_unlock(&s_extraction_mutex);

   return in_progress;
}
