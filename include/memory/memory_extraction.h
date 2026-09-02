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
 * Memory Extraction API
 *
 * Triggers memory extraction at session end to store facts, preferences,
 * and conversation summaries.
 */

#ifndef MEMORY_EXTRACTION_H
#define MEMORY_EXTRACTION_H

#include <json-c/json.h>
#include <stdbool.h>

#include "llm/llm_interface.h"
#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fallback LLM info for extraction retry
 *
 * When the configured extraction model is unavailable (e.g. HTTP 404),
 * the extraction thread retries using the session's active model.
 * Pass NULL to memory_trigger_extraction() to disable fallback.
 */
typedef struct {
   llm_type_t type;
   cloud_provider_t cloud_provider;
   char endpoint[128];
   char model[LLM_MODEL_NAME_MAX];
} memory_extraction_fallback_t;

/**
 * @brief Build fallback LLM info from a session's active model
 *
 * Uses session_get_llm_config() for proper mutex acquisition.
 *
 * @param session Session to read LLM config from (may be NULL)
 * @param fb      Output fallback struct (zeroed if session is NULL)
 */
struct session;
void memory_extraction_build_fallback(struct session *session, memory_extraction_fallback_t *fb);

/**
 * @brief Trigger memory extraction for a session
 *
 * Spawns a detached thread to extract facts, preferences, and a summary
 * from the conversation history. The extraction runs asynchronously so
 * it doesn't block session cleanup.
 *
 * Supports incremental extraction: if conversation_id is provided, only
 * processes messages since the last extraction (tracked via last_extracted_msg_count).
 *
 * @param user_id User ID (must be > 0)
 * @param conversation_id Conversation ID for tracking (0 to skip tracking)
 * @param session_id_str Session identifier string (for summary)
 * @param conversation_history JSON array of messages (copied internally)
 * @param message_count Number of messages in conversation
 * @param duration_seconds Session duration
 * @param fallback Session's active LLM config to retry with on failure (NULL to disable)
 * @return 0 on success, 1 on failure
 */
int memory_trigger_extraction(int user_id,
                              int64_t conversation_id,
                              const char *session_id_str,
                              struct json_object *conversation_history,
                              int message_count,
                              int duration_seconds,
                              const memory_extraction_fallback_t *fallback);

/**
 * @brief Check if extraction is in progress for a user
 *
 * @param user_id User ID to check
 * @return true if extraction is running, false otherwise
 */
bool memory_extraction_in_progress(int user_id);

/**
 * @brief Read and consume the last-extraction-was-transient flag for a user.
 *
 * Returns true exactly once after an extraction completed with a transient
 * failure signal (cloud unreachable / pre-flight check failed).  The
 * recovery / reextract orchestrator reads this after waiting for an
 * extraction to decide whether to roll back the attempt counter instead
 * of treating the failure as a normal "extraction returned no facts"
 * outcome.  Returns false if the last extraction for this user succeeded
 * or failed for a non-transient reason.
 *
 * @param user_id User ID to query
 * @return true if the last extraction for this user signalled transient
 *         failure; false otherwise.  The flag is cleared by this call.
 */
bool memory_extraction_consume_last_transient(int user_id);

/**
 * @brief Parse a JSON response from an LLM, handling common formatting variants.
 *
 * Tries direct parse, markdown code block extraction, and brace/bracket fallback.
 * Caller owns the returned object and must call json_object_put().
 *
 * @param response Raw LLM response text
 * @return Parsed json_object, or NULL on failure
 */
struct json_object *memory_extraction_parse_json(const char *response);

/**
 * @brief Resolve memory.extraction_provider/extraction_model into an llm_resolved_config_t.
 *
 * Converts the string-keyed extraction settings (provider name + model) into
 * a fully-populated config suitable for llm_chat_completion_with_config().
 * Caller-provided buffers back the model and endpoint string fields because
 * the resolved config holds pointers into them — the buffers must outlive
 * every use of the resolved config (typically allocated on the same stack
 * frame that consumes the config, as the in-tree callers do).
 *
 * On unknown provider the helper logs a warning and falls back to LLM_LOCAL.
 * Returns FAILURE when extraction_provider is missing/empty (a config error
 * worth surfacing rather than masking with a silent fallback) or when the
 * caller's buffers are smaller than the required minimums (model_buf_sz <
 * LLM_MODEL_NAME_MAX or endpoint_buf_sz < MEMORY_EXTRACTION_ENDPOINT_BUF_MIN).
 *
 * Sets cfg->suppress_tools = true, cfg->thinking_mode = "disabled", and
 * cfg->timeout_ms from g_config.memory.extraction_timeout_ms — extraction
 * paths universally want JSON output without tools or visible reasoning.
 *
 * @param cfg              Output config (zeroed by the helper)
 * @param model_buf        Caller-owned buffer for cfg->model
 * @param model_buf_sz     Size of model_buf (must be >= LLM_MODEL_NAME_MAX)
 * @param endpoint_buf     Caller-owned buffer for cfg->endpoint
 * @param endpoint_buf_sz  Size of endpoint_buf (must be >= MEMORY_EXTRACTION_ENDPOINT_BUF_MIN)
 * @param log_prefix       Module name for log messages (NULL → "memory_extraction")
 * @return SUCCESS on resolution, FAILURE on missing provider, undersized
 *         buffers, or invalid args
 */
#define MEMORY_EXTRACTION_ENDPOINT_BUF_MIN 128

/**
 * @brief Master extraction prompt template.
 *
 * Single source of truth used by both the live extraction worker and the
 * summarize-missing backfill helper.  Defined in memory_extraction.c.
 * Format args (in order): anchor_line, conversation_json, existing_profile.
 *
 * Exposed (not file-static) so backfill tooling can share the prompt without
 * silently drifting; do NOT duplicate this string elsewhere.
 */
extern const char *MEMORY_EXTRACTION_PROMPT_TEMPLATE;

int memory_extraction_resolve_config(llm_resolved_config_t *cfg,
                                     char *model_buf,
                                     size_t model_buf_sz,
                                     char *endpoint_buf,
                                     size_t endpoint_buf_sz,
                                     const char *log_prefix);

/**
 * @brief Return the character size of the extraction prompt template.
 *
 * The prompt sent to the LLM is the template plus an anchor-date line plus
 * the conversation JSON plus the user's existing-profile snapshot.  This
 * helper exposes the constant template size so cost estimators can avoid
 * hardcoding a guess (the actual template is ~3-5K characters, well above
 * historical 1500-token guesses).  Caller adds per-message + profile
 * estimates on top.
 *
 * @return Size of EXTRACTION_PROMPT_TEMPLATE in characters (excluding NUL).
 */
size_t memory_extraction_get_template_size_chars(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_EXTRACTION_H */
