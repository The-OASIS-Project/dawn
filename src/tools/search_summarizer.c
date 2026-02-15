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
 * Search Result Summarizer - LLM-based summarization for large search results
 */

#include "tools/search_summarizer.h"

#include <curl/curl.h>
#include <json-c/json.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/session_manager.h"
#include "llm/llm_interface.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "tools/curl_buffer.h"
#include "tools/tfidf_summarizer.h"
#include "tts/text_to_speech.h"
#include "ui/metrics.h"
#ifdef ENABLE_WEBUI
#include "webui/webui_server.h"
#endif

// =============================================================================
// Module State
// =============================================================================

static summarizer_config_t g_config;
static int g_initialized = 0;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

// =============================================================================
// Truncation Helper
// =============================================================================

/**
 * @brief Truncate text with intelligent boundary detection
 *
 * Attempts to truncate at natural boundaries (paragraph, sentence, word)
 * rather than mid-word. Appends a truncation notice.
 *
 * @param text Original text to truncate
 * @param max_len Maximum length including truncation notice
 * @param original_len Original length (for notice)
 * @return Allocated truncated string (caller frees) or NULL on failure
 */
static char *truncate_with_notice(const char *text, size_t max_len, size_t original_len) {
   if (!text || max_len < 100) {
      return NULL;
   }

   // Reserve space for truncation notice (use sizeof for robustness)
   const char *notice_fmt = "\n\n[Content truncated: showing %zu of %zu bytes]";
   char notice[80];
   size_t content_max = max_len - sizeof(notice) - 1;
   snprintf(notice, sizeof(notice), notice_fmt, content_max, original_len);
   size_t notice_len = strlen(notice);

   if (content_max >= strlen(text)) {
      // No truncation needed
      return strdup(text);
   }

   // Find best truncation point within the last 500 chars before limit
   size_t search_start = (content_max > 500) ? content_max - 500 : 0;
   size_t best_break = content_max;
   const char *boundary_type = "hard limit";

   // Priority 1: paragraph break (double newline)
   for (size_t i = content_max; i > search_start; i--) {
      if (i > 0 && text[i] == '\n' && text[i - 1] == '\n') {
         best_break = i;
         boundary_type = "paragraph";
         break;
      }
   }

   // Priority 2: sentence end (.!? followed by space or newline)
   if (best_break == content_max) {
      for (size_t i = content_max; i > search_start; i--) {
         char c = text[i - 1];
         if ((c == '.' || c == '!' || c == '?') &&
             (text[i] == ' ' || text[i] == '\n' || text[i] == '\0')) {
            best_break = i;
            boundary_type = "sentence";
            break;
         }
      }
   }

   // Priority 3: word boundary (space or newline)
   if (best_break == content_max) {
      for (size_t i = content_max; i > search_start; i--) {
         if (text[i] == ' ' || text[i] == '\n') {
            best_break = i;
            boundary_type = "word";
            break;
         }
      }
   }

   LOG_INFO("search_summarizer: Truncating at %s boundary (pos %zu of %zu)", boundary_type,
            best_break, original_len);

   // Allocate result
   size_t result_len = best_break + notice_len + 1;
   char *result = malloc(result_len);
   if (!result) {
      return NULL;
   }

   memcpy(result, text, best_break);
   memcpy(result + best_break, notice, notice_len);
   result[best_break + notice_len] = '\0';

   return result;
}

// Summarization prompt template
static const char *SUMMARIZER_PROMPT_TEMPLATE =
    "You are a search result summarizer. Given raw search results for the query \"%s\", "
    "write a concise prose summary of the key information found.\n\n"
    "Rules:\n"
    "- Synthesize information across all sources\n"
    "- Focus on facts directly relevant to the query\n"
    "- Note conflicting information if present\n"
    "- Do not include URLs\n"
    "- Keep under %zu words\n\n"
    "Search Results:\n%s\n\n"
    "Summary:";

// =============================================================================
// Lifecycle
// =============================================================================

int search_summarizer_init(const summarizer_config_t *config) {
   pthread_mutex_lock(&g_mutex);

   if (g_initialized) {
      pthread_mutex_unlock(&g_mutex);
      LOG_WARNING("search_summarizer: Already initialized");
      return SUMMARIZER_SUCCESS;
   }

   // Apply configuration or defaults
   if (config) {
      g_config = *config;
   } else {
      g_config.backend = SUMMARIZER_BACKEND_DISABLED;
      g_config.failure_policy = SUMMARIZER_ON_FAILURE_PASSTHROUGH;
      g_config.threshold_bytes = SUMMARIZER_DEFAULT_THRESHOLD;
      g_config.target_summary_words = SUMMARIZER_DEFAULT_TARGET_WORDS;
      g_config.target_ratio = TFIDF_DEFAULT_RATIO;
   }

   g_initialized = 1;
   pthread_mutex_unlock(&g_mutex);

   // Update metrics with config
   metrics_set_summarizer_config(search_summarizer_backend_name(g_config.backend),
                                 g_config.threshold_bytes);

   if (g_config.backend == SUMMARIZER_BACKEND_TFIDF) {
      LOG_INFO("search_summarizer: Initialized (backend=%s, threshold=%zu, target_ratio=%.2f)",
               search_summarizer_backend_name(g_config.backend), g_config.threshold_bytes,
               g_config.target_ratio);
   } else {
      LOG_INFO("search_summarizer: Initialized (backend=%s, threshold=%zu, target_words=%zu)",
               search_summarizer_backend_name(g_config.backend), g_config.threshold_bytes,
               g_config.target_summary_words);
   }

   return SUMMARIZER_SUCCESS;
}

void search_summarizer_cleanup(void) {
   pthread_mutex_lock(&g_mutex);
   g_initialized = 0;
   pthread_mutex_unlock(&g_mutex);
   LOG_INFO("search_summarizer: Cleanup complete");
}

int search_summarizer_is_initialized(void) {
   return __atomic_load_n(&g_initialized, __ATOMIC_ACQUIRE);
}

const summarizer_config_t *search_summarizer_get_config(void) {
   if (!g_initialized) {
      return NULL;
   }
   return &g_config;
}

const char *search_summarizer_backend_name(summarizer_backend_t backend) {
   switch (backend) {
      case SUMMARIZER_BACKEND_LOCAL:
         return "local";
      case SUMMARIZER_BACKEND_DEFAULT:
         return "default";
      case SUMMARIZER_BACKEND_TFIDF:
         return "tfidf";
      case SUMMARIZER_BACKEND_DISABLED:
      default:
         return "disabled";
   }
}

summarizer_backend_t search_summarizer_parse_backend(const char *str) {
   if (!str) {
      return SUMMARIZER_BACKEND_DISABLED;
   }
   if (strcmp(str, "local") == 0) {
      return SUMMARIZER_BACKEND_LOCAL;
   }
   if (strcmp(str, "default") == 0) {
      return SUMMARIZER_BACKEND_DEFAULT;
   }
   if (strcmp(str, "tfidf") == 0) {
      return SUMMARIZER_BACKEND_TFIDF;
   }
   return SUMMARIZER_BACKEND_DISABLED;
}

// =============================================================================
// Local LLM Summarization
// =============================================================================

/**
 * @brief Call local llama-server to summarize search results
 *
 * @param prompt Full prompt including search results
 * @param out_summary Receives allocated summary string
 * @return SUMMARIZER_SUCCESS or error code
 */
static int summarize_with_local_llm(const char *prompt, char **out_summary) {
   CURL *curl = curl_easy_init();
   if (!curl) {
      LOG_ERROR("search_summarizer: Failed to create CURL handle");
      return SUMMARIZER_ERROR_BACKEND;
   }

   // Build request JSON
   struct json_object *root = json_object_new_object();
   struct json_object *messages = json_object_new_array();
   struct json_object *user_msg = json_object_new_object();

   json_object_object_add(user_msg, "role", json_object_new_string("user"));
   json_object_object_add(user_msg, "content", json_object_new_string(prompt));
   json_object_array_add(messages, user_msg);

   json_object_object_add(root, "messages", messages);
   json_object_object_add(root, "max_tokens", json_object_new_int(1024));
   json_object_object_add(root, "temperature", json_object_new_double(0.3));

   const char *payload = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN |
                                                                  JSON_C_TO_STRING_NOSLASHESCAPE);

   // Set up response buffer
   curl_buffer_t buffer;
   curl_buffer_init_with_max(&buffer, 32 * 1024);  // 32KB max for summary response

   // Configure CURL
   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, "Content-Type: application/json");

   curl_easy_setopt(curl, CURLOPT_URL, SUMMARIZER_LOCAL_ENDPOINT);
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, SUMMARIZER_LOCAL_TIMEOUT_SEC);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5);

   LOG_INFO("search_summarizer: Sending %zu byte prompt to local LLM", strlen(prompt));

   CURLcode res = curl_easy_perform(curl);

   curl_slist_free_all(headers);
   json_object_put(root);

   if (res != CURLE_OK) {
      LOG_ERROR("search_summarizer: Local LLM request failed: %s", curl_easy_strerror(res));
      curl_easy_cleanup(curl);
      curl_buffer_free(&buffer);
      return SUMMARIZER_ERROR_BACKEND;
   }

   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
   curl_easy_cleanup(curl);

   if (http_code != 200) {
      LOG_ERROR("search_summarizer: Local LLM HTTP error %ld", http_code);
      curl_buffer_free(&buffer);
      return SUMMARIZER_ERROR_BACKEND;
   }

   // Parse response
   struct json_object *resp_root = json_tokener_parse(buffer.data);
   curl_buffer_free(&buffer);

   if (!resp_root) {
      LOG_ERROR("search_summarizer: Failed to parse local LLM response");
      return SUMMARIZER_ERROR_BACKEND;
   }

   // Extract content from OpenAI-compatible response
   struct json_object *choices = NULL;
   struct json_object *first_choice = NULL;
   struct json_object *message = NULL;
   struct json_object *content = NULL;

   if (!json_object_object_get_ex(resp_root, "choices", &choices) ||
       json_object_array_length(choices) == 0) {
      LOG_ERROR("search_summarizer: No choices in local LLM response");
      json_object_put(resp_root);
      return SUMMARIZER_ERROR_BACKEND;
   }

   first_choice = json_object_array_get_idx(choices, 0);
   if (!json_object_object_get_ex(first_choice, "message", &message) ||
       !json_object_object_get_ex(message, "content", &content)) {
      LOG_ERROR("search_summarizer: Missing content in local LLM response");
      json_object_put(resp_root);
      return SUMMARIZER_ERROR_BACKEND;
   }

   const char *summary_text = json_object_get_string(content);
   if (!summary_text || strlen(summary_text) == 0) {
      LOG_ERROR("search_summarizer: Empty summary from local LLM");
      json_object_put(resp_root);
      return SUMMARIZER_ERROR_BACKEND;
   }

   *out_summary = strdup(summary_text);
   json_object_put(resp_root);

   if (!*out_summary) {
      return SUMMARIZER_ERROR_ALLOC;
   }

   LOG_INFO("search_summarizer: Local LLM produced %zu byte summary", strlen(*out_summary));
   return SUMMARIZER_SUCCESS;
}

// =============================================================================
// Default LLM Summarization (uses main LLM interface)
// =============================================================================

/**
 * @brief Call main LLM (via llm_interface) to summarize search results
 *
 * Uses whatever LLM backend is configured for the main conversation
 * (could be cloud API or local llama-server depending on configuration).
 *
 * @param prompt Full prompt including search results
 * @param out_summary Receives allocated summary string
 * @return SUMMARIZER_SUCCESS or error code
 */
static int summarize_with_default_llm(const char *prompt, char **out_summary) {
   // Build minimal conversation history with just the summarization request
   struct json_object *conversation = json_object_new_array();

   struct json_object *user_msg = json_object_new_object();
   json_object_object_add(user_msg, "role", json_object_new_string("user"));
   json_object_object_add(user_msg, "content", json_object_new_string(prompt));
   json_object_array_add(conversation, user_msg);

   // Suppress tools for this call - we just want a text summary, not tool calls.
   // Without this, the LLM might respond with tool_calls instead of content,
   // causing a "content field is empty" error.
   llm_tools_suppress_push();

   // Call cloud LLM (non-streaming for summarization)
   // Pass allow_fallback=false to prevent summarizer failures from triggering
   // global LLM fallback (which would switch to local LLM and play TTS notification)
   char *response = llm_chat_completion(conversation, prompt, NULL, NULL, 0, false);

   // Restore tools
   llm_tools_suppress_pop();

   json_object_put(conversation);

   if (!response) {
      LOG_ERROR("search_summarizer: Cloud LLM request failed");
      return SUMMARIZER_ERROR_BACKEND;
   }

   *out_summary = response;
   LOG_INFO("search_summarizer: Default LLM produced %zu byte summary", strlen(*out_summary));
   return SUMMARIZER_SUCCESS;
}

// =============================================================================
// User Feedback Helper
// =============================================================================

/**
 * @brief Notify user that summarization is starting
 *
 * For local audio sessions: Plays TTS message
 * For WebUI sessions: Sends status update to display in UI
 */
static void notify_summarization_starting(void) {
#ifdef ENABLE_MULTI_CLIENT
   session_t *session = session_get_command_context();

   if (!session || session->session_id == 0) {
      /* Local audio session - use TTS */
      text_to_speech((char *)"Summarizing the results, please standby.");
   }
#ifdef ENABLE_WEBUI
   else if (session->type == SESSION_TYPE_WEBUI) {
      /* WebUI session - send status update */
      webui_send_state_with_detail(session, "summarizing", "Processing search results...");
   }
#endif
#else
   /* Local-only mode - use TTS */
   text_to_speech((char *)"Summarizing the results, please standby.");
#endif
}

// =============================================================================
// Main Processing Function
// =============================================================================

int search_summarizer_process(const char *search_results,
                              const char *original_query,
                              char **out_result) {
   if (!out_result) {
      return SUMMARIZER_ERROR_ALLOC;
   }
   *out_result = NULL;

   if (!g_initialized) {
      LOG_ERROR("search_summarizer: Not initialized");
      return SUMMARIZER_ERROR_NOT_INIT;
   }

   if (!search_results || !original_query) {
      return SUMMARIZER_ERROR_ALLOC;
   }

   size_t input_size = strlen(search_results);

   // If disabled or under threshold, pass through
   if (g_config.backend == SUMMARIZER_BACKEND_DISABLED) {
      *out_result = strdup(search_results);
      if (!*out_result) {
         return SUMMARIZER_ERROR_ALLOC;
      }
      return SUMMARIZER_SUCCESS;
   }

   if (input_size <= g_config.threshold_bytes) {
      LOG_INFO("search_summarizer: Input %zu bytes under threshold %zu, passing through",
               input_size, g_config.threshold_bytes);
      *out_result = strdup(search_results);
      if (!*out_result) {
         return SUMMARIZER_ERROR_ALLOC;
      }
      return SUMMARIZER_SUCCESS;
   }

   LOG_INFO("search_summarizer: Input %zu bytes exceeds threshold %zu, summarizing with %s backend",
            input_size, g_config.threshold_bytes, search_summarizer_backend_name(g_config.backend));

   // Notify user that summarization is starting (may take a while for local LLM)
   notify_summarization_starting();

   int result;
   char *summary = NULL;

   // Handle TF-IDF backend specially - it doesn't need an LLM prompt
   if (g_config.backend == SUMMARIZER_BACKEND_TFIDF) {
      result = tfidf_summarize(search_results, &summary, g_config.target_ratio,
                               TFIDF_MIN_SENTENCES);

      if (result != TFIDF_SUCCESS) {
         LOG_ERROR("search_summarizer: TF-IDF summarization failed: %s",
                   tfidf_error_string(result));
         result = SUMMARIZER_ERROR_BACKEND;
      } else {
         result = SUMMARIZER_SUCCESS;
         /* TODO: Remove this debug log after quality verification */
         LOG_INFO("search_summarizer: TF-IDF output:\n%s", summary);
      }
   } else {
      // Build the summarization prompt for LLM backends
      size_t prompt_size = strlen(SUMMARIZER_PROMPT_TEMPLATE) + strlen(original_query) +
                           input_size + 32; /* Extra for word count */
      char *prompt = malloc(prompt_size);
      if (!prompt) {
         if (g_config.failure_policy == SUMMARIZER_ON_FAILURE_PASSTHROUGH) {
            if (input_size > SUMMARIZER_MAX_PASSTHROUGH_BYTES) {
               LOG_WARNING("search_summarizer: Allocation failed, truncating for passthrough");
               *out_result = truncate_with_notice(search_results, SUMMARIZER_MAX_PASSTHROUGH_BYTES,
                                                  input_size);
            } else {
               LOG_WARNING("search_summarizer: Allocation failed, passing through raw results");
               *out_result = strdup(search_results);
            }
            return *out_result ? SUMMARIZER_SUCCESS : SUMMARIZER_ERROR_ALLOC;
         }
         return SUMMARIZER_ERROR_ALLOC;
      }

      snprintf(prompt, prompt_size, SUMMARIZER_PROMPT_TEMPLATE, original_query,
               g_config.target_summary_words, search_results);

      // Call appropriate LLM backend
      if (g_config.backend == SUMMARIZER_BACKEND_LOCAL) {
         result = summarize_with_local_llm(prompt, &summary);
      } else {
         result = summarize_with_default_llm(prompt, &summary);
      }

      free(prompt);
   }

   if (result != SUMMARIZER_SUCCESS) {
      if (g_config.failure_policy == SUMMARIZER_ON_FAILURE_PASSTHROUGH) {
         if (input_size > SUMMARIZER_MAX_PASSTHROUGH_BYTES) {
            LOG_WARNING(
                "search_summarizer: Backend failed, truncating %zu bytes to %d for passthrough",
                input_size, SUMMARIZER_MAX_PASSTHROUGH_BYTES);
            *out_result = truncate_with_notice(search_results, SUMMARIZER_MAX_PASSTHROUGH_BYTES,
                                               input_size);
         } else {
            LOG_WARNING("search_summarizer: Backend failed, passing through raw results");
            *out_result = strdup(search_results);
         }
         return *out_result ? SUMMARIZER_SUCCESS : SUMMARIZER_ERROR_ALLOC;
      }
      return result;
   }

   // Prepend a note indicating this is a summary
   size_t summary_len = strlen(summary);
   size_t header_len = 64;
   char *final_result = malloc(summary_len + header_len);
   if (!final_result) {
      free(summary);
      if (g_config.failure_policy == SUMMARIZER_ON_FAILURE_PASSTHROUGH) {
         if (input_size > SUMMARIZER_MAX_PASSTHROUGH_BYTES) {
            *out_result = truncate_with_notice(search_results, SUMMARIZER_MAX_PASSTHROUGH_BYTES,
                                               input_size);
         } else {
            *out_result = strdup(search_results);
         }
         return *out_result ? SUMMARIZER_SUCCESS : SUMMARIZER_ERROR_ALLOC;
      }
      return SUMMARIZER_ERROR_ALLOC;
   }

   snprintf(final_result, summary_len + header_len, "[Summarized from %zu bytes]\n\n%s", input_size,
            summary);
   free(summary);

   *out_result = final_result;
   size_t output_size = strlen(final_result);
   LOG_INFO("search_summarizer: Reduced %zu bytes to %zu bytes (%.1f%% reduction)", input_size,
            output_size, (1.0 - (double)output_size / input_size) * 100.0);
   LOG_INFO("search_summarizer: Summary:\n%s", final_result);

   // Record stats for TUI
   metrics_record_summarization(input_size, output_size);

   return SUMMARIZER_SUCCESS;
}
