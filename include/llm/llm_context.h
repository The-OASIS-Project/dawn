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

#define LLM_CONTEXT_DEFAULT_LOCAL 8192     /* Default local context if query fails */
#define LLM_CONTEXT_DEFAULT_OPENAI 128000  /* GPT-4o default */
#define LLM_CONTEXT_DEFAULT_CLAUDE 200000  /* Claude default */
#define LLM_CONTEXT_DEFAULT_GEMINI 1048576 /* Gemini default (1M) */
#define LLM_CONTEXT_SUMMARY_TARGET_L1 500  /* Target tokens for L1 normal summary */
#define LLM_CONTEXT_SUMMARY_TARGET_L2 250  /* Target tokens for L2 aggressive summary */
#define LLM_CONTEXT_SUMMARY_TARGET_L3 150  /* Hard budget for L3 deterministic truncation */
#define LLM_CONTEXT_KEEP_EXCHANGES 2       /* Keep last N user/assistant pairs */

/**
 * @brief Compaction escalation levels — guaranteed convergence
 */
typedef enum {
   LLM_COMPACT_NORMAL = 0,       /* Detailed summary via LLM (~500 tokens) */
   LLM_COMPACT_AGGRESSIVE = 1,   /* Bullet-point summary via LLM (~250 tokens) */
   LLM_COMPACT_DETERMINISTIC = 2 /* Mechanical truncation, no LLM call */
} llm_compaction_level_t;

#define LLM_COMPACT_MAX_LEVEL LLM_COMPACT_DETERMINISTIC

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
   bool performed;               /* True if compaction was performed */
   llm_compaction_level_t level; /* Escalation level used (L1/L2/L3) */
   int tokens_before;            /* Token count before compaction */
   int tokens_after;             /* Token count after compaction */
   int messages_summarized;      /* Number of messages summarized */
   char log_filename[256];       /* Saved conversation log (if logging enabled) */
   char *summary;                /* Generated summary (heap-allocated) */
} llm_compaction_result_t;

/**
 * @brief Free resources in a compaction result
 *
 * Frees dynamically allocated fields (summary) and zeros the struct.
 * Safe to call on already-freed or zero-initialized results.
 *
 * @param result Result struct to free (can be NULL)
 */
void llm_compaction_result_free(llm_compaction_result_t *result);

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

/* Anthropic's drop report (llm/llm_claude_binding.h); only a pointer here. */
typedef struct llm_claude_drops llm_claude_drops_t;

/**
 * @brief A turn's token usage, reported to the context tracker
 *
 * Passed as a struct (rather than positional args) so the four provider call
 * sites can't transpose the several int fields, and so the provider/type that
 * produced the tokens travels with them for cache-savings accounting.
 */
typedef struct {
   int prompt_tokens;         /**< Full prompt (input) token count */
   int completion_tokens;     /**< Output token count */
   int cached_tokens;         /**< Prompt tokens read from cache (0 if none) */
   int cache_write_tokens;    /**< Prompt tokens newly written to cache (GPT-5.6+/Claude; else 0) */
   llm_type_t type;           /**< LLM_LOCAL or LLM_CLOUD (selects cache economics) */
   cloud_provider_t provider; /**< Cloud provider that produced the tokens */
   const char *message_id;    /**< Anthropic: the response's id (NULL otherwise) */
   const char *cache_miss_reason;   /**< Anthropic cache diagnostics: why the cache missed */
   int cache_missed_tokens;         /**< ...and how many input tokens it missed */
   const llm_claude_drops_t *drops; /**< Anthropic: thinking blocks the API dropped (NULL: none) */
} llm_usage_report_t;

/**
 * @brief Update token/cache counts from an LLM response (per session)
 *
 * Call once per completed LLM sub-call with its usage. Tracks per-session token
 * usage and derives the provider-discounted cache saving for later display.
 *
 * @param session_id Session to update
 * @param usage This sub-call's usage report (must be non-NULL)
 */
void llm_context_update_usage(uint32_t session_id, const llm_usage_report_t *usage);

/**
 * @brief The last turn's cache-facing token snapshot for a session
 *
 * All fields mirror last_prompt_tokens semantics: for a multi-iteration tool
 * turn they reflect the LAST LLM sub-call, not a sum across iterations.
 * saved_input_tokens is the provider-discounted net input-token saving and can
 * be negative on a cache-write-heavy turn.
 */
typedef struct {
   int prompt_tokens;      /**< Full prompt (input) tokens — the cache-rate denominator */
   int cached_tokens;      /**< Cache-read prompt tokens */
   int cache_write_tokens; /**< Cache-write prompt tokens */
   int saved_input_tokens; /**< Net effective input tokens saved (may be negative) */
} llm_cache_snapshot_t;

/**
 * @brief Get the last turn's cache snapshot for a session (WebUI display)
 *
 * Zero-fills @p out for an unknown session. @p out must be non-NULL.
 *
 * @param session_id Session to query
 * @param[out] out Snapshot (zeroed if the session has no tracking slot yet)
 */
void llm_context_get_last_cache(uint32_t session_id, llm_cache_snapshot_t *out);

/**
 * @brief Clear a session's cache-token trackers at the start of a turn
 *
 * Call once per turn before the LLM streams so an interrupted or usage-less turn
 * (Stop / barge-in / a provider that omits the usage chunk) reports 0 cache
 * tokens on its idle metrics frame instead of carrying over the prior turn's
 * figures. A normal turn overwrites these when its usage chunk is parsed.
 *
 * @param session_id Session to reset
 */
void llm_context_reset_turn_cache(uint32_t session_id);

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
 * Uses configured compact_hard_threshold (default 80%).
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
 * @param conv_id Database conversation ID (0 = skip message ID tracking)
 * @param result Output: compaction result details
 * @return 0 on success, non-zero on failure
 */
int llm_context_compact(uint32_t session_id,
                        struct json_object *history,
                        llm_type_t type,
                        cloud_provider_t provider,
                        const char *model,
                        int64_t conv_id,
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
 * history exceeds the compact_hard_threshold and compacts if needed.
 *
 * Uses global LLM configuration. For session-specific config, use
 * llm_context_auto_compact_with_config() instead.
 *
 * @param history Conversation history (modified in place if compacted)
 * @param session_id Session ID for logging
 * @return 1 if compaction was performed, 0 if not needed or failed
 */
int llm_context_auto_compact(struct json_object *history, uint32_t session_id);

/**
 * @brief Check and perform auto-compaction with explicit config
 *
 * Same as llm_context_auto_compact() but uses provided config instead of
 * global LLM settings. Use this for WebUI sessions that have their own
 * LLM configuration.
 *
 * @param history Conversation history (modified in place if compacted)
 * @param session_id Session ID for logging
 * @param type LLM type from session config
 * @param provider Cloud provider from session config
 * @param model Model name from session config
 * @return 1 if compaction was performed, 0 if not needed or failed
 */
int llm_context_auto_compact_with_config(struct json_object *history,
                                         uint32_t session_id,
                                         llm_type_t type,
                                         cloud_provider_t provider,
                                         const char *model);

/* =============================================================================
 * Async Compaction (LCM Phase 2 — background compaction between turns)
 * ============================================================================= */

struct session; /* Forward declaration — avoids circular include */

/**
 * @brief Trigger async compaction after a turn completes
 *
 * Checks soft threshold; if exceeded, deep-copies history and spawns a
 * background thread to compact it. Result is merged by llm_context_async_merge()
 * before the next LLM call. Skips for session 0 (local mic) and non-WebUI.
 *
 * @param session Session (must be retained by caller)
 * @param history Current conversation history
 * @param type LLM type for compaction call
 * @param provider Cloud provider
 * @param model Model name
 * @return 0 on success or skip, 1 on error
 */
int llm_context_async_trigger(struct session *session,
                              struct json_object *history,
                              llm_type_t type,
                              cloud_provider_t provider,
                              const char *model);

/**
 * @brief Merge a completed async compaction result into live history
 *
 * Called before each LLM call. If a background compaction completed (state READY),
 * validates the snapshot and replaces the compacted portion of history while
 * preserving any messages added after the snapshot.
 *
 * @param session Session to check
 * @param history Live conversation history (modified in place if merged)
 * @return 1 if merged, 0 if nothing to merge or result discarded
 */
int llm_context_async_merge(struct session *session, struct json_object *history);

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
 * @return SUCCESS, 1 if logging disabled, or FAILURE on error
 */
int llm_context_save_conversation(uint32_t session_id,
                                  struct json_object *history,
                                  const char *suffix,
                                  char *filename_out,
                                  size_t filename_len);

/* =============================================================================
 * Test-only API (exposed for unit testing)
 * ============================================================================= */

#ifdef DAWN_TESTING
char *llm_context_compact_deterministic(struct json_object *to_summarize, int token_budget);
int llm_context_calculate_compaction_target(int context_size, float threshold);
int llm_context_estimate_tokens_range(struct json_object *history, int start_idx, int end_idx);
#endif

#ifdef __cplusplus
}
#endif

#endif /* LLM_CONTEXT_H */
