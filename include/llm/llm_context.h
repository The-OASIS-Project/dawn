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
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "llm/llm_compaction.h"
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

/** The hard threshold's default, when [llm] compact_hard_threshold is out of range. */
#define LLM_CONTEXT_HARD_THRESHOLD_DEFAULT 0.85f

/**
 * @brief The hard threshold (fraction of a model's window): past it a turn's
 *        request is too big to send as it is
 */
float llm_context_hard_threshold(void);

/**
 * @brief Whether @p history, with @p extra_tokens a turn will add, reaches
 *        @p threshold of the model's window (the larger of the tracked count
 *        and the estimate)
 */
bool llm_context_over_threshold(uint32_t session_id,
                                struct json_object *history,
                                int extra_tokens,
                                llm_type_t type,
                                cloud_provider_t provider,
                                const char *model,
                                float threshold);

/**
 * @brief After a compaction: the session's history now estimates at
 *        @p estimate (the last response counted the history before the swap)
 */
void llm_context_note_compacted(uint32_t session_id, int estimate);

/**
 * @brief A request of session @p session_id is being sent: its history (and
 *        input) estimate at @p estimate, on this model.  Its usage, when it
 *        comes back, calibrates the model's density (llm_compaction.h).
 */
void llm_context_note_request(uint32_t session_id,
                              int estimate,
                              llm_type_t type,
                              cloud_provider_t provider,
                              const char *model);

/** What session @p session_id's requests say about this model's reading. */
void llm_context_calibration(uint32_t session_id,
                             llm_type_t type,
                             cloud_provider_t provider,
                             const char *model,
                             llm_compaction_calibration_t *out);

/**
 * @brief The real size of a request of session @p session_id whose history
 *        estimates at @p estimate, on this model: calibrated when its requests
 *        have been measured, else the larger of the estimate and the last count
 */
int llm_context_request_tokens(uint32_t session_id,
                               int estimate,
                               llm_type_t type,
                               cloud_provider_t provider,
                               const char *model);

/**
 * @brief Summarize @p to_summarize (llm_compaction_summarize): with the
 *        dedicated compaction provider when one is set, else @p config's model;
 *        on no session's behalf (it reads and writes no session's state)
 * @param kept_tokens What the history keeps beside the summary
 * @param window_tokens The window the compacted history must fit (the model the
 *        next turn runs); 0 = the summarizer's own
 * @param cal How that model reads the conversation (NULL: the plain estimate)
 * @param tag The conversation's tag (masked in the summary)
 * @param config The summarizer's model (its session's settings, resolved)
 * @param cancel Set when it is no longer wanted (NULL: never)
 * @param level_out The level used
 * @return The summary (heap), or NULL
 */
char *llm_context_summarize(struct json_object *to_summarize,
                            int kept_tokens,
                            int window_tokens,
                            const llm_compaction_calibration_t *cal,
                            const char *tag,
                            const session_llm_config_t *config,
                            const atomic_bool *cancel,
                            llm_compaction_level_t *level_out);

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

#ifdef __cplusplus
}
#endif

#endif /* LLM_CONTEXT_H */
