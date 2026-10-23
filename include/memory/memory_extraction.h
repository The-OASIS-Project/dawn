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

/** Room for memory_extraction_anchor_line()'s line ("Conversation anchor:
 *  YYYY-MM-DD" and two newlines, 34 bytes) with slack. */
#define MEMORY_EXTRACTION_ANCHOR_LINE_LEN 64

/**
 * @brief The prompt's "Conversation anchor: YYYY-MM-DD" line: the
 *        conversation's logical now (conversations.anchor_date) as a date in
 *        @p user_id's zone, so the model resolves "yesterday" and "last
 *        month" against it.
 *
 * Empty when the conversation has no anchor (or @p conversation_id isn't
 * positive).  @p out_sz must be at least 1.
 * Reads the database: never call it holding the auth_db lock.
 */
void memory_extraction_anchor_line(int user_id, int64_t conversation_id, char *out, size_t out_sz);

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
 * @return 0 when started or nothing to do, 1 on failure, MEMORY_EXTRACTION_BUSY when the
 *         user's extraction is running or held or every slot is busy (a conversation then
 *         waits in the queue and runs, re-read from the database, at the next release;
 *         memory recovery's own requests aren't queued)
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
 * @brief Wait (up to @p timeout_sec) until no extraction is running for a user
 *
 * Claims nothing: an extraction may start again right after.
 *
 * @return true if idle, false on timeout
 */
bool memory_extraction_wait_idle(int user_id, int timeout_sec);

/** memory_trigger_extraction(): refused for now (queued, or recovery retries it). */
#define MEMORY_EXTRACTION_BUSY 2

/** Drop the user's queued extractions of the conversations in @p ids (a forget,
 *  while it holds the user, so none runs again after it). */
void memory_extraction_unqueue_conversations(int user_id, const int64_t *ids, int n);

/** Drop every queued extraction of the user (forgetting all of their memories). */
void memory_extraction_unqueue_user(int user_id);

/** At shutdown: start nothing more from the queue and drop it (memory recovery
 *  picks those conversations up on the next start). */
void memory_extraction_stop_queue(void);

/** Users whose extraction can be held at once (memory_extraction_hold_user). */
#define MEMORY_EXTRACTION_MAX_HOLDS 64

/**
 * @brief Wait for a user's in-flight extraction to finish, then block new ones
 *
 * Marks the user held, so an extraction triggered meanwhile for this user
 * waits in the queue until the hold is released; while the hold waits, the
 * queue starts nothing for the user.  Holds
 * have their own table (MEMORY_EXTRACTION_MAX_HOLDS), apart from the running
 * extractions' slots, so they never take another user's capacity.  For
 * operations that must see every row an extraction writes (e.g. forgetting a
 * conversation).  Pair with memory_extraction_release_user().
 *
 * @param user_id     User
 * @param timeout_sec Longest to wait for an in-flight extraction
 * @return true if held; false on timeout (nothing held)
 */
bool memory_extraction_hold_user(int user_id, int timeout_sec);

/** Release a hold taken by memory_extraction_hold_user(). */
void memory_extraction_release_user(int user_id);

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

/** The text around the extraction template's four values, as a benchmark gives it. */
#define MEMORY_EXTRACTION_TEMPLATE_PIECES 5

/**
 * @brief Benchmarks only: extract with this template instead of the compiled
 *        one, so a prompt variant is measured without a rebuild.  The pieces
 *        are the literal text before, between and after the four values
 *        (anchor, conversation, profile, expiry); they are never used as a
 *        format.  The caller keeps the array and its strings alive until it
 *        passes NULL, which restores the compiled template.  The daemon never
 *        calls this.
 *
 * @param pieces The template's text, or NULL for the compiled template
 */
void memory_extraction_set_template_pieces(
    const char *const pieces[MEMORY_EXTRACTION_TEMPLATE_PIECES]);

/**
 * @brief The prompt an extraction sends: the compiled template (or a
 *        benchmark's pieces) with the four values in place.
 *
 * @param anchor       The conversation's "now" line
 * @param conversation The conversation JSON
 * @param profile      What is already known about the user
 * @param expiry       The fact-expiry instructions, or ""
 * @return The prompt (caller frees), or NULL when out of memory
 */
char *memory_extraction_build_prompt(const char *anchor,
                                     const char *conversation,
                                     const char *profile,
                                     const char *expiry);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_EXTRACTION_H */
