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
 * LLM Context Management - Track context usage and auto-summarize conversations
 */

#include "llm/llm_context.h"

#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "llm/llm_interface.h"
#include "logging.h"
#include "tools/string_utils.h"
#include "tts/text_to_speech.h"
#include "webui/webui_server.h"

/* =============================================================================
 * Configuration Access
 * ============================================================================= */

extern dawn_config_t g_config;

/* =============================================================================
 * Model Context Size Lookup Table
 * ============================================================================= */

typedef struct {
   const char *model_prefix; /* Model name prefix to match */
   int context_size;         /* Context size in tokens */
} model_context_entry_t;

/* OpenAI models (updated December 2025) */
static const model_context_entry_t s_openai_models[] = {
   /* GPT-5.x family (400K context) */
   { "gpt-5.2", 400000 },
   { "gpt-5.1", 400000 },
   { "gpt-5-mini", 400000 },
   { "gpt-5-nano", 400000 },
   { "gpt-5", 400000 },
   /* GPT-4.1 family (1M context) */
   { "gpt-4.1", 1000000 },
   { "gpt-4.1-mini", 1000000 },
   { "gpt-4.1-nano", 1000000 },
   /* O-series reasoning models */
   { "o3-pro", 200000 },
   { "o3-mini", 200000 },
   { "o3", 200000 },
   { "o1-pro", 200000 },
   { "o1-mini", 128000 },
   { "o1-preview", 128000 },
   { "o1", 200000 },
   /* GPT-4o family */
   { "gpt-4o-mini", 128000 },
   { "gpt-4o", 128000 },
   /* GPT-4 Turbo */
   { "gpt-4-turbo", 128000 },
   { "gpt-4-0125", 128000 },
   { "gpt-4-1106", 128000 },
   /* Legacy GPT-4 */
   { "gpt-4-32k", 32768 },
   { "gpt-4", 8192 },
   /* Legacy GPT-3.5 */
   { "gpt-3.5-turbo-16k", 16384 },
   { "gpt-3.5-turbo", 16384 },
   { NULL, 0 } /* Sentinel */
};

/* Claude models (updated December 2025) */
static const model_context_entry_t s_claude_models[] = {
   /* Claude 4.x family */
   { "claude-opus-4.5", 200000 },
   { "claude-sonnet-4.5", 200000 },
   { "claude-opus-4.1", 200000 },
   { "claude-sonnet-4.1", 200000 },
   { "claude-opus-4", 200000 },
   { "claude-sonnet-4", 200000 },
   /* Claude 3.5 family */
   { "claude-3-5-sonnet", 200000 },
   { "claude-3-5-haiku", 200000 },
   { "claude-3.5", 200000 },
   /* Claude 3 family (some deprecated) */
   { "claude-3-opus", 200000 },
   { "claude-3-sonnet", 200000 },
   { "claude-3-haiku", 200000 },
   { NULL, 0 } /* Sentinel */
};

/* =============================================================================
 * Module State
 * ============================================================================= */

static struct {
   bool initialized;
   int local_context_size;     /* Cached from /props query */
   bool local_context_queried; /* True if we've queried local LLM */
   int last_prompt_tokens;     /* Last known prompt tokens (for WebUI) */
   int last_context_size;      /* Last known context size (for WebUI) */
   pthread_mutex_t mutex;      /* Protects state */
} s_state = {
   .initialized = false,
   .local_context_size = LLM_CONTEXT_DEFAULT_LOCAL,
   .local_context_queried = false,
   .last_prompt_tokens = 0,
   .last_context_size = 0,
};

/* Per-session token tracking */
typedef struct {
   uint32_t session_id;
   int last_prompt_tokens;
   int last_completion_tokens;
   int total_prompt_tokens;
   int total_completion_tokens;
} session_token_tracking_t;

#define MAX_TRACKED_SESSIONS 16
static session_token_tracking_t s_session_tokens[MAX_TRACKED_SESSIONS];
static int s_session_token_count = 0;

/* =============================================================================
 * CURL Response Buffer
 * ============================================================================= */

typedef struct {
   char *data;
   size_t size;
} curl_response_t;

static size_t context_curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
   size_t realsize = size * nmemb;
   curl_response_t *resp = (curl_response_t *)userp;

   char *ptr = realloc(resp->data, resp->size + realsize + 1);
   if (!ptr) {
      return 0;
   }

   resp->data = ptr;
   memcpy(&(resp->data[resp->size]), contents, realsize);
   resp->size += realsize;
   resp->data[resp->size] = '\0';

   return realsize;
}

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

int llm_context_init(void) {
   if (s_state.initialized) {
      return 0;
   }

   if (pthread_mutex_init(&s_state.mutex, NULL) != 0) {
      LOG_ERROR("llm_context: Failed to initialize mutex");
      return 1;
   }

   s_state.local_context_size = LLM_CONTEXT_DEFAULT_LOCAL;
   s_state.local_context_queried = false;
   s_session_token_count = 0;
   memset(s_session_tokens, 0, sizeof(s_session_tokens));

   s_state.initialized = true;
   LOG_INFO("llm_context: Initialized (default local context: %d)", s_state.local_context_size);

   return 0;
}

void llm_context_cleanup(void) {
   if (!s_state.initialized) {
      return;
   }

   pthread_mutex_destroy(&s_state.mutex);
   s_state.initialized = false;
   LOG_INFO("llm_context: Cleaned up");
}

/* =============================================================================
 * Context Size Functions
 * ============================================================================= */

/**
 * @brief Look up context size from model table
 */
static int lookup_model_context(const model_context_entry_t *table, const char *model) {
   if (!model || !table) {
      return 0;
   }

   for (int i = 0; table[i].model_prefix != NULL; i++) {
      if (strncasecmp(model, table[i].model_prefix, strlen(table[i].model_prefix)) == 0) {
         return table[i].context_size;
      }
   }

   return 0; /* Not found */
}

int llm_context_query_local(const char *endpoint) {
   if (!endpoint || endpoint[0] == '\0') {
      return LLM_CONTEXT_DEFAULT_LOCAL;
   }

   /* Build /props URL */
   char url[512];
   snprintf(url, sizeof(url), "%s/props", endpoint);

   /* Remove any trailing /v1/chat/completions if present */
   char *v1_pos = strstr(url, "/v1/");
   if (v1_pos) {
      strcpy(v1_pos, "/props");
   }

   CURL *curl = curl_easy_init();
   if (!curl) {
      LOG_WARNING("llm_context: Failed to init CURL for /props query");
      return LLM_CONTEXT_DEFAULT_LOCAL;
   }

   curl_response_t response = { .data = NULL, .size = 0 };

   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, context_curl_write_cb);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);

   CURLcode res = curl_easy_perform(curl);
   curl_easy_cleanup(curl);

   if (res != CURLE_OK) {
      LOG_WARNING("llm_context: Failed to query %s: %s", url, curl_easy_strerror(res));
      free(response.data);
      return LLM_CONTEXT_DEFAULT_LOCAL;
   }

   if (!response.data) {
      return LLM_CONTEXT_DEFAULT_LOCAL;
   }

   /* Parse JSON response */
   struct json_object *root = json_tokener_parse(response.data);
   free(response.data);

   if (!root) {
      LOG_WARNING("llm_context: Failed to parse /props response");
      return LLM_CONTEXT_DEFAULT_LOCAL;
   }

   int context_size = LLM_CONTEXT_DEFAULT_LOCAL;

   /* Try to get n_ctx from default_generation_settings */
   struct json_object *settings = NULL;
   struct json_object *n_ctx_obj = NULL;

   if (json_object_object_get_ex(root, "default_generation_settings", &settings)) {
      if (json_object_object_get_ex(settings, "n_ctx", &n_ctx_obj)) {
         context_size = json_object_get_int(n_ctx_obj);
      }
   }

   /* Also try top-level n_ctx (some versions) */
   if (context_size == LLM_CONTEXT_DEFAULT_LOCAL) {
      if (json_object_object_get_ex(root, "n_ctx", &n_ctx_obj)) {
         context_size = json_object_get_int(n_ctx_obj);
      }
   }

   json_object_put(root);

   LOG_INFO("llm_context: Local LLM context size: %d tokens", context_size);
   return context_size;
}

void llm_context_refresh_local(void) {
   pthread_mutex_lock(&s_state.mutex);
   s_state.local_context_queried = false;
   pthread_mutex_unlock(&s_state.mutex);
   LOG_INFO("llm_context: Local context cache invalidated");
}

int llm_context_get_size(llm_type_t type, cloud_provider_t provider, const char *model) {
   if (type == LLM_LOCAL) {
      pthread_mutex_lock(&s_state.mutex);

      if (!s_state.local_context_queried) {
         /* Query local LLM for context size */
         const char *endpoint = g_config.llm.local.endpoint[0] != '\0' ? g_config.llm.local.endpoint
                                                                       : "http://127.0.0.1:8080";
         s_state.local_context_size = llm_context_query_local(endpoint);
         s_state.local_context_queried = true;
      }

      int size = s_state.local_context_size;
      pthread_mutex_unlock(&s_state.mutex);
      return size;
   }

   /* Cloud LLM - use lookup table */
   int size = 0;

   if (provider == CLOUD_PROVIDER_OPENAI) {
      size = lookup_model_context(s_openai_models, model);
      if (size == 0) {
         size = LLM_CONTEXT_DEFAULT_OPENAI;
      }
   } else if (provider == CLOUD_PROVIDER_CLAUDE) {
      size = lookup_model_context(s_claude_models, model);
      if (size == 0) {
         size = LLM_CONTEXT_DEFAULT_CLAUDE;
      }
   } else {
      size = LLM_CONTEXT_DEFAULT_OPENAI; /* Fallback */
   }

   return size;
}

/* =============================================================================
 * Token Tracking Functions
 * ============================================================================= */

/**
 * @brief Find or create token tracking entry for session
 */
static session_token_tracking_t *get_session_tracking(uint32_t session_id, bool create) {
   for (int i = 0; i < s_session_token_count; i++) {
      if (s_session_tokens[i].session_id == session_id) {
         return &s_session_tokens[i];
      }
   }

   if (!create || s_session_token_count >= MAX_TRACKED_SESSIONS) {
      return NULL;
   }

   /* Create new entry */
   session_token_tracking_t *entry = &s_session_tokens[s_session_token_count++];
   memset(entry, 0, sizeof(*entry));
   entry->session_id = session_id;
   return entry;
}

void llm_context_update_usage(uint32_t session_id,
                              int prompt_tokens,
                              int completion_tokens,
                              int cached_tokens) {
   pthread_mutex_lock(&s_state.mutex);

   session_token_tracking_t *tracking = get_session_tracking(session_id, true);
   if (tracking) {
      tracking->last_prompt_tokens = prompt_tokens;
      tracking->last_completion_tokens = completion_tokens;
      tracking->total_prompt_tokens += prompt_tokens;
      tracking->total_completion_tokens += completion_tokens;
   }

   pthread_mutex_unlock(&s_state.mutex);

   /* Log context usage with threshold check */
   llm_type_t type = llm_get_type();
   cloud_provider_t provider = llm_get_cloud_provider();
   int context_size = llm_context_get_size(type, provider, llm_get_model_name());
   float usage_pct = (context_size > 0) ? (float)prompt_tokens / (float)context_size * 100.0f : 0;
   float threshold = g_config.llm.summarize_threshold;
   float threshold_pct = threshold * 100.0f;

   /* Store last values for WebUI retrieval */
   s_state.last_prompt_tokens = prompt_tokens;
   s_state.last_context_size = context_size;

   LOG_INFO("Context: %d/%d tokens (%.1f%%), threshold: %.0f%%", prompt_tokens, context_size,
            usage_pct, threshold_pct);

   if (usage_pct >= threshold_pct) {
      LOG_WARNING("Context usage (%.1f%%) exceeds threshold (%.0f%%) - compaction recommended",
                  usage_pct, threshold_pct);
   }
}

void llm_context_get_last_usage(int *current_tokens, int *max_tokens, float *threshold) {
   if (current_tokens) {
      *current_tokens = s_state.last_prompt_tokens;
   }
   if (max_tokens) {
      *max_tokens = s_state.last_context_size;
   }
   if (threshold) {
      *threshold = g_config.llm.summarize_threshold;
   }
}

int llm_context_estimate_tokens(struct json_object *history) {
   if (!history || !json_object_is_type(history, json_type_array)) {
      return 0;
   }

   int total_chars = 0;
   int len = json_object_array_length(history);

   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      struct json_object *content_obj = NULL;

      if (json_object_object_get_ex(msg, "content", &content_obj)) {
         if (json_object_is_type(content_obj, json_type_string)) {
            const char *content = json_object_get_string(content_obj);
            if (content) {
               total_chars += strlen(content);
            }
         } else if (json_object_is_type(content_obj, json_type_array)) {
            /* Handle content arrays (vision messages) */
            int content_len = json_object_array_length(content_obj);
            for (int j = 0; j < content_len; j++) {
               struct json_object *part = json_object_array_get_idx(content_obj, j);
               struct json_object *text_obj = NULL;
               if (json_object_object_get_ex(part, "text", &text_obj)) {
                  const char *text = json_object_get_string(text_obj);
                  if (text) {
                     total_chars += strlen(text);
                  }
               }
               /* Images are roughly 85 tokens for low detail, 765+ for high */
               struct json_object *type_obj = NULL;
               if (json_object_object_get_ex(part, "type", &type_obj)) {
                  const char *type = json_object_get_string(type_obj);
                  if (type && (strcmp(type, "image_url") == 0 || strcmp(type, "image") == 0)) {
                     total_chars += 3000; /* Rough estimate for image */
                  }
               }
            }
         }
      }

      /* Add overhead for role, formatting */
      total_chars += 20;
   }

   /* Rough estimate: ~4 characters per token */
   return total_chars / 4;
}

int llm_context_get_usage(uint32_t session_id,
                          llm_type_t type,
                          cloud_provider_t provider,
                          const char *model,
                          llm_context_usage_t *usage) {
   if (!usage) {
      return 1;
   }

   memset(usage, 0, sizeof(*usage));

   /* Get context size for current provider */
   usage->max_tokens = llm_context_get_size(type, provider, model);

   /* Get last known prompt tokens for this session */
   pthread_mutex_lock(&s_state.mutex);
   session_token_tracking_t *tracking = get_session_tracking(session_id, false);
   if (tracking) {
      usage->current_tokens = tracking->last_prompt_tokens;
   }
   pthread_mutex_unlock(&s_state.mutex);

   /* Calculate percentage */
   if (usage->max_tokens > 0) {
      usage->usage_percent = (float)usage->current_tokens / (float)usage->max_tokens;
   }

   /* Check against threshold */
   float threshold = g_config.llm.summarize_threshold;
   if (threshold <= 0 || threshold > 1.0) {
      threshold = 0.80; /* Default 80% */
   }
   usage->needs_compaction = (usage->usage_percent >= threshold);

   return 0;
}

/* =============================================================================
 * Compaction Functions
 * ============================================================================= */

bool llm_context_needs_compaction_for_switch(uint32_t session_id,
                                             struct json_object *history,
                                             llm_type_t target_type,
                                             cloud_provider_t target_provider,
                                             const char *target_model) {
   int target_context = llm_context_get_size(target_type, target_provider, target_model);
   int estimated_tokens = llm_context_estimate_tokens(history);

   float threshold = g_config.llm.summarize_threshold;
   if (threshold <= 0 || threshold > 1.0) {
      threshold = 0.80;
   }

   int threshold_tokens = (int)(target_context * threshold);
   bool needs_compaction = (estimated_tokens > threshold_tokens);

   if (needs_compaction) {
      LOG_INFO("llm_context: Compaction needed for switch - estimated %d tokens, target context %d "
               "(threshold %d)",
               estimated_tokens, target_context, threshold_tokens);
   }

   return needs_compaction;
}

bool llm_context_needs_compaction(uint32_t session_id,
                                  struct json_object *history,
                                  llm_type_t type,
                                  cloud_provider_t provider,
                                  const char *model) {
   llm_context_usage_t usage;
   if (llm_context_get_usage(session_id, type, provider, model, &usage) != 0) {
      return false;
   }

   /*
    * Always estimate from current history if available. The tracked token count
    * (last_prompt_tokens) is from the PREVIOUS call and doesn't account for new
    * messages added since. Use the maximum of tracked vs estimated to catch cases
    * where new messages have pushed us over the threshold.
    */
   if (history) {
      int estimated = llm_context_estimate_tokens(history);
      if (estimated > usage.current_tokens) {
         usage.current_tokens = estimated;
         usage.usage_percent = (float)usage.current_tokens / (float)usage.max_tokens;

         float threshold = g_config.llm.summarize_threshold;
         if (threshold <= 0 || threshold > 1.0) {
            threshold = 0.80;
         }
         usage.needs_compaction = (usage.usage_percent >= threshold);
      }
   }

   return usage.needs_compaction;
}

int llm_context_save_conversation(uint32_t session_id,
                                  struct json_object *history,
                                  const char *suffix,
                                  char *filename_out,
                                  size_t filename_len) {
   /* Check if logging is enabled */
   if (!g_config.llm.conversation_logging) {
      LOG_INFO("llm_context: Conversation logging disabled, skipping save");
      return 1;
   }

   if (!history) {
      return -1;
   }

   /* Create logs directory if needed */
   if (mkdir("logs", 0755) != 0 && errno != EEXIST) {
      LOG_WARNING("llm_context: Could not create logs directory");
   }

   /* Generate timestamped filename */
   time_t now = time(NULL);
   struct tm *tm_info = localtime(&now);
   char timestamp[32];
   strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);

   char filename[256];
   snprintf(filename, sizeof(filename), "logs/chat_history_session%u_%s_%s.json", session_id,
            suffix, timestamp);

   /* Write JSON to file */
   const char *json_str = json_object_to_json_string_ext(
       history, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE);

   FILE *fp = fopen(filename, "w");
   if (!fp) {
      LOG_ERROR("llm_context: Failed to open %s for writing", filename);
      return -1;
   }

   fprintf(fp, "%s\n", json_str);
   fclose(fp);

   LOG_INFO("llm_context: Conversation saved to %s", filename);

   if (filename_out && filename_len > 0) {
      safe_strncpy(filename_out, filename, filename_len);
   }

   return 0;
}

int llm_context_compact(uint32_t session_id,
                        struct json_object *history,
                        llm_type_t type,
                        cloud_provider_t provider,
                        const char *model,
                        llm_compaction_result_t *result) {
   if (!history || !result) {
      return 1;
   }

   memset(result, 0, sizeof(*result));

   int history_len = json_object_array_length(history);
   if (history_len < 4) {
      /* Too few messages to compact */
      LOG_INFO("llm_context: History too short to compact (%d messages)", history_len);
      return 0;
   }

   result->tokens_before = llm_context_estimate_tokens(history);

   /* Save conversation before compacting */
   llm_context_save_conversation(session_id, history, "precompact", result->log_filename,
                                 sizeof(result->log_filename));

   /* Extract system prompt (usually first message) */
   struct json_object *system_msg = NULL;
   int start_idx = 0;

   struct json_object *first_msg = json_object_array_get_idx(history, 0);
   struct json_object *role_obj = NULL;
   if (json_object_object_get_ex(first_msg, "role", &role_obj)) {
      if (strcmp(json_object_get_string(role_obj), "system") == 0) {
         system_msg = json_object_get(first_msg); /* Increment ref */
         start_idx = 1;
      }
   }

   /* Keep last N exchanges (user + assistant pairs) */
   int keep_messages = LLM_CONTEXT_KEEP_EXCHANGES * 2;
   int end_idx = history_len - keep_messages;

   if (end_idx <= start_idx) {
      /* Not enough messages to summarize */
      if (system_msg) {
         json_object_put(system_msg);
      }
      LOG_INFO("llm_context: Not enough messages to summarize");
      return 0;
   }

   result->messages_summarized = end_idx - start_idx;

   /* Build content to summarize */
   struct json_object *to_summarize = json_object_new_array();
   for (int i = start_idx; i < end_idx; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      json_object_array_add(to_summarize, json_object_get(msg));
   }

   /* Generate summary prompt */
   const char *summary_content = json_object_to_json_string(to_summarize);
   char summary_prompt[8192];
   snprintf(summary_prompt, sizeof(summary_prompt),
            "Summarize this conversation concisely, preserving key facts, decisions, user "
            "preferences, and context needed to continue naturally. Be brief but complete:\n\n%s",
            summary_content);
   json_object_put(to_summarize);

   /* Call LLM to generate summary */
   LOG_INFO("llm_context: Requesting summary of %d messages from %s", result->messages_summarized,
            type == LLM_LOCAL ? "local LLM" : "cloud LLM");

   /* Create minimal conversation for summary request */
   struct json_object *summary_request = json_object_new_array();
   struct json_object *user_msg = json_object_new_object();
   json_object_object_add(user_msg, "role", json_object_new_string("user"));
   json_object_object_add(user_msg, "content", json_object_new_string(summary_prompt));
   json_object_array_add(summary_request, user_msg);

   /* Make LLM call for summary */
   char *summary = llm_chat_completion(summary_request, NULL, NULL, 0);
   json_object_put(summary_request);

   if (!summary) {
      LOG_ERROR("llm_context: Failed to generate summary");
      if (system_msg) {
         json_object_put(system_msg);
      }
      return 1;
   }

   /* Rebuild history: system + summary + last N messages */
   struct json_object *new_history = json_object_new_array();

   /* Add system prompt if present */
   if (system_msg) {
      json_object_array_add(new_history, system_msg); /* Transfers ownership */
   }

   /* Add summary as assistant message */
   struct json_object *summary_msg = json_object_new_object();
   json_object_object_add(summary_msg, "role", json_object_new_string("assistant"));

   char summary_with_note[8192];
   snprintf(summary_with_note, sizeof(summary_with_note), "[Previous conversation summary: %s]",
            summary);
   json_object_object_add(summary_msg, "content", json_object_new_string(summary_with_note));
   json_object_array_add(new_history, summary_msg);
   free(summary);

   /* Add last N messages */
   for (int i = end_idx; i < history_len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      json_object_array_add(new_history, json_object_get(msg));
   }

   /* Replace history contents */
   /* Clear old history */
   while (json_object_array_length(history) > 0) {
      json_object_array_del_idx(history, 0, 1);
   }

   /* Copy new history into original array */
   int new_len = json_object_array_length(new_history);
   for (int i = 0; i < new_len; i++) {
      struct json_object *msg = json_object_array_get_idx(new_history, i);
      json_object_array_add(history, json_object_get(msg));
   }
   json_object_put(new_history);

   result->tokens_after = llm_context_estimate_tokens(history);
   result->performed = true;

   LOG_INFO("llm_context: Compaction complete - %d messages summarized, %d -> %d tokens",
            result->messages_summarized, result->tokens_before, result->tokens_after);

   return 0;
}

int llm_context_compact_for_switch(uint32_t session_id,
                                   struct json_object *history,
                                   llm_type_t current_type,
                                   cloud_provider_t current_provider,
                                   const char *current_model,
                                   llm_type_t target_type,
                                   cloud_provider_t target_provider,
                                   const char *target_model,
                                   llm_compaction_result_t *result) {
   if (!result) {
      return 1;
   }

   memset(result, 0, sizeof(*result));

   /* Check if compaction needed for target */
   if (!llm_context_needs_compaction_for_switch(session_id, history, target_type, target_provider,
                                                target_model)) {
      LOG_INFO("llm_context: No compaction needed for switch");
      return 0;
   }

   /* Perform compaction using CURRENT provider (has larger context) */
   LOG_INFO("llm_context: Performing pre-switch compaction using current provider");
   return llm_context_compact(session_id, history, current_type, current_provider, current_model,
                              result);
}

/* =============================================================================
 * Auto-Compaction Function
 * ============================================================================= */

int llm_context_auto_compact(struct json_object *history, uint32_t session_id) {
   if (!history || !s_state.initialized) {
      return 0;
   }

   /* Get current LLM config */
   llm_type_t type = llm_get_type();
   cloud_provider_t provider = llm_get_cloud_provider();
   const char *model = llm_get_model_name();

   /* Check if compaction is needed */
   if (!llm_context_needs_compaction(session_id, history, type, provider, model)) {
      return 0;
   }

   LOG_WARNING("llm_context: Auto-compacting conversation before LLM call");

   /* Notify local user via TTS before compaction (can take a few seconds) */
   if (session_id == 0) {
      text_to_speech((char *)"Compacting my memory. Just a moment.");
   }

   /* Perform compaction */
   llm_compaction_result_t result;
   int rc = llm_context_compact(session_id, history, type, provider, model, &result);

   if (rc == 0 && result.performed) {
      LOG_INFO("llm_context: Auto-compaction complete - %d tokens -> %d tokens",
               result.tokens_before, result.tokens_after);
      return 1; /* Compaction was performed */
   }

   return 0;
}

/* =============================================================================
 * Utility Functions
 * ============================================================================= */

char *llm_context_usage_string(const llm_context_usage_t *usage, char *buf, size_t buf_len) {
   if (!usage || !buf || buf_len == 0) {
      return buf;
   }

   snprintf(buf, buf_len, "%d/%d (%.0f%%)", usage->current_tokens, usage->max_tokens,
            usage->usage_percent * 100.0f);

   return buf;
}
