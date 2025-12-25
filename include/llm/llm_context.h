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
 *
 * This module manages LLM context windows across providers:
 * - Queries local LLM context size via /props endpoint
 * - Maintains lookup table for cloud LLM context sizes
 * - Tracks token usage from responses
 * - Auto-summarizes conversations when approaching context limits
 * - Handles pre-switch compaction when moving to smaller context LLMs
 */

#ifndef LLM_CONTEXT_H
#define LLM_CONTEXT_H

#include <json-c/json.h>
#include <stdbool.h>
#include <stdint.h>

#include "llm/llm_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define LLM_CONTEXT_DEFAULT_LOCAL 8192    /* Default local context if query fails */
#define LLM_CONTEXT_DEFAULT_OPENAI 128000 /* GPT-4o default */
#define LLM_CONTEXT_DEFAULT_CLAUDE 200000 /* Claude 3/4 default */
#define LLM_CONTEXT_SUMMARY_TARGET 500    /* Target tokens for summary */
#define LLM_CONTEXT_KEEP_EXCHANGES 2      /* Keep last N user/assistant pairs */

/* =============================================================================
 * Types
 * ============================================================================= */

/**
 * @brief Context usage information for a session
 */
typedef struct {
   int current_tokens;    /* Tokens used in current conversation */
   int max_tokens;        /* Context limit for current provider */
   float usage_percent;   /* current_tokens / max_tokens */
   bool needs_compaction; /* True if approaching threshold */
} llm_context_usage_t;

/**
 * @brief Result of a compaction operation
 */
typedef struct {
   bool performed;          /* True if compaction was performed */
   int tokens_before;       /* Token count before compaction */
   int tokens_after;        /* Token count after compaction */
   int messages_summarized; /* Number of messages summarized */
   char log_filename[256];  /* Saved conversation log (if logging enabled) */
} llm_compaction_result_t;

/* =============================================================================
 * Lifecycle Functions
 * ============================================================================= */

/**
 * @brief Initialize the context management module
 *
 * Queries local LLM for context size if available.
 *
 * @return 0 on success, non-zero on failure
 */
int llm_context_init(void);

/**
 * @brief Clean up context management resources
 */
void llm_context_cleanup(void);

/* =============================================================================
 * Context Size Functions
 * ============================================================================= */

/**
 * @brief Get context size for a specific provider/model combination
 *
 * For local LLM, queries /props endpoint (cached after first call).
 * For cloud LLMs, uses lookup table based on model name.
 *
 * @param type LLM type (local or cloud)
 * @param provider Cloud provider (ignored for local)
 * @param model Model name (for cloud lookup)
 * @return Context size in tokens
 */
int llm_context_get_size(llm_type_t type, cloud_provider_t provider, const char *model);

/**
 * @brief Query local LLM server for context size
 *
 * Makes HTTP request to /props endpoint and extracts n_ctx.
 * Result is cached for subsequent calls.
 *
 * @param endpoint Local LLM endpoint URL (e.g., "http://127.0.0.1:8080")
 * @return Context size, or LLM_CONTEXT_DEFAULT_LOCAL on failure
 */
int llm_context_query_local(const char *endpoint);

/**
 * @brief Refresh cached local context size
 *
 * Forces re-query of /props endpoint. Use after server restart
 * or model change.
 */
void llm_context_refresh_local(void);

/* =============================================================================
 * Token Tracking Functions
 * ============================================================================= */

/**
 * @brief Update token count from LLM response
 *
 * Call this after each LLM response with the usage information.
 * Tracks per-session token usage.
 *
 * @param session_id Session to update
 * @param prompt_tokens Tokens used for prompt (from response)
 * @param completion_tokens Tokens used for completion
 * @param cached_tokens Cached tokens (if applicable)
 */
void llm_context_update_usage(uint32_t session_id,
                              int prompt_tokens,
                              int completion_tokens,
                              int cached_tokens);

/**
 * @brief Get current context usage for a session
 *
 * @param session_id Session to query
 * @param type Current LLM type
 * @param provider Current cloud provider
 * @param model Current model name
 * @param usage Output: usage information
 * @return 0 on success, non-zero on failure
 */
int llm_context_get_usage(uint32_t session_id,
                          llm_type_t type,
                          cloud_provider_t provider,
                          const char *model,
                          llm_context_usage_t *usage);

/**
 * @brief Get the most recent token counts (for WebUI display)
 *
 * Returns the last known prompt tokens, context size, and threshold.
 * Call after LLM requests to get display values.
 *
 * @param current_tokens Output: last prompt token count
 * @param max_tokens Output: context size for current provider
 * @param threshold Output: compaction threshold (0.0-1.0)
 */
void llm_context_get_last_usage(int *current_tokens, int *max_tokens, float *threshold);

/**
 * @brief Estimate token count for a conversation history
 *
 * Uses rough estimate of ~4 characters per token.
 * More accurate than nothing, but not exact.
 *
 * @param history JSON array of conversation messages
 * @return Estimated token count
 */
int llm_context_estimate_tokens(struct json_object *history);

/* =============================================================================
 * Compaction Functions
 * ============================================================================= */

/**
 * @brief Check if compaction is needed before switching providers
 *
 * Compares current token usage against target provider's context size.
 * Should be called BEFORE performing the switch.
 *
 * @param session_id Session to check
 * @param history Current conversation history
 * @param target_type Target LLM type after switch
 * @param target_provider Target cloud provider after switch
 * @param target_model Target model after switch
 * @return true if compaction needed, false otherwise
 */
bool llm_context_needs_compaction_for_switch(uint32_t session_id,
                                             struct json_object *history,
                                             llm_type_t target_type,
                                             cloud_provider_t target_provider,
                                             const char *target_model);

/**
 * @brief Check if compaction is needed based on threshold
 *
 * Uses configured summarize_threshold (default 80%).
 *
 * @param session_id Session to check
 * @param history Current conversation history
 * @param type Current LLM type
 * @param provider Current cloud provider
 * @param model Current model
 * @return true if usage exceeds threshold
 */
bool llm_context_needs_compaction(uint32_t session_id,
                                  struct json_object *history,
                                  llm_type_t type,
                                  cloud_provider_t provider,
                                  const char *model);

/**
 * @brief Perform conversation compaction (summarization)
 *
 * 1. Saves full conversation to log file (if logging enabled)
 * 2. Extracts messages to summarize (all except system + last N exchanges)
 * 3. Calls current LLM to generate summary
 * 4. Replaces history with: system prompt + summary + last N exchanges
 *
 * @param session_id Session to compact
 * @param history Conversation history (modified in place)
 * @param type Current LLM type (used for summarization call)
 * @param provider Current cloud provider
 * @param model Current model
 * @param result Output: compaction result details
 * @return 0 on success, non-zero on failure
 */
int llm_context_compact(uint32_t session_id,
                        struct json_object *history,
                        llm_type_t type,
                        cloud_provider_t provider,
                        const char *model,
                        llm_compaction_result_t *result);

/**
 * @brief Perform compaction before provider switch
 *
 * Wrapper around llm_context_compact that:
 * 1. Checks if compaction is needed for target provider
 * 2. If so, compacts using CURRENT provider (has larger context)
 * 3. Returns result for logging
 *
 * Call this BEFORE switching providers.
 *
 * @param session_id Session to compact
 * @param history Conversation history (modified in place)
 * @param current_type Current LLM type
 * @param current_provider Current cloud provider
 * @param current_model Current model
 * @param target_type Target LLM type after switch
 * @param target_provider Target cloud provider after switch
 * @param target_model Target model after switch
 * @param result Output: compaction result (check result->performed)
 * @return 0 on success (even if no compaction needed), non-zero on failure
 */
int llm_context_compact_for_switch(uint32_t session_id,
                                   struct json_object *history,
                                   llm_type_t current_type,
                                   cloud_provider_t current_provider,
                                   const char *current_model,
                                   llm_type_t target_type,
                                   cloud_provider_t target_provider,
                                   const char *target_model,
                                   llm_compaction_result_t *result);

/* =============================================================================
 * Auto-Compaction Function
 * ============================================================================= */

/**
 * @brief Check and perform auto-compaction before LLM call
 *
 * Should be called before making LLM requests. Checks if the conversation
 * history exceeds the summarize_threshold and compacts if needed.
 *
 * @param history Conversation history (modified in place if compacted)
 * @param session_id Session ID for logging
 * @return 1 if compaction was performed, 0 if not needed or failed
 */
int llm_context_auto_compact(struct json_object *history, uint32_t session_id);

/* =============================================================================
 * Utility Functions
 * ============================================================================= */

/**
 * @brief Get human-readable context usage string
 *
 * Returns string like "6543/8192 (80%)"
 *
 * @param usage Usage information
 * @param buf Output buffer
 * @param buf_len Buffer length
 * @return Pointer to buf
 */
char *llm_context_usage_string(const llm_context_usage_t *usage, char *buf, size_t buf_len);

/**
 * @brief Save conversation history to log file
 *
 * Saves to logs/ directory with timestamped filename.
 * Respects conversation_logging config setting.
 *
 * @param session_id Session ID for filename
 * @param history Conversation history to save
 * @param suffix Filename suffix (e.g., "precompact", "shutdown")
 * @param filename_out Output: saved filename (can be NULL)
 * @param filename_len Length of filename buffer
 * @return 0 on success, 1 if logging disabled, -1 on error
 */
int llm_context_save_conversation(uint32_t session_id,
                                  struct json_object *history,
                                  const char *suffix,
                                  char *filename_out,
                                  size_t filename_len);

#ifdef __cplusplus
}
#endif

#endif /* LLM_CONTEXT_H */
