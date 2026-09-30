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
 * the project author(s).
 *
 * Per-call prompt-cache telemetry: one normalized record per LLM call, from
 * every provider path, logged as a single grep-stable "LLM cache:" line.
 */

#ifndef LLM_CACHE_MONITOR_H
#define LLM_CACHE_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#include "llm/llm_claude_binding.h"
#include "llm/llm_context.h"

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/** What an LLM call is for; tags the usage record. */
typedef enum {
   LLM_CALL_TURN = 0,   /**< A turn's first provider call */
   LLM_CALL_TOOL_ITER,  /**< A later call in a turn's tool loop */
   LLM_CALL_JOB,        /**< A background job's call */
   LLM_CALL_RESEARCH,   /**< A deep-research run's call */
   LLM_CALL_EXTRACTION, /**< Memory extraction / summarization / recategorization */
   LLM_CALL_COMPACTION, /**< Conversation compaction */
   LLM_CALL_BRIEFING,   /**< A scheduled briefing */
   LLM_CALL_OBSERVE,    /**< Silent observation */
   LLM_CALL_SUMMARIZER, /**< A tool's own helper call (search summarizer) */
   LLM_CALL_SYNTHESIS,  /**< A research run's synthesis, critic or completion take */
   LLM_CALL_OTHER,      /**< No session and no tag */
   LLM_CALL_KIND_COUNT
} llm_call_kind_t;

/** Stable lowercase name for @p kind ("turn", "tool_iter", ...). */
const char *llm_call_kind_name(llm_call_kind_t kind);

/**
 * @brief Whether @p kind is a conversation's own call (turn, tool iteration, job
 * or research run), whose usage is the conversation's context size; the others
 * (extraction, compaction, ...) are side calls that only get recorded.
 */
bool llm_call_kind_is_conversation(llm_call_kind_t kind);

/**
 * @brief Whether a call of @p kind is its session's latest context size
 *
 * A conversation call, or an unattributed one (tracked as before).  A tagged
 * side call still counts toward the session's totals but doesn't replace its
 * last-call numbers, the WebUI context gauge or the "Context:" line.
 */
bool llm_call_kind_sets_context(llm_call_kind_t kind);

/** Why a call's expected cache read is 0 (LLM_CACHE_WARM when it isn't). */
typedef enum {
   LLM_CACHE_WARM = 0,     /**< Expected read computed from the previous call */
   LLM_CACHE_COLD_FIRST,   /**< First call on its cache key */
   LLM_CACHE_COLD_TTL,     /**< The cache expired since the previous call */
   LLM_CACHE_COLD_TOOLS,   /**< The tool set changed */
   LLM_CACHE_COLD_SYSTEM,  /**< The stable system text changed */
   LLM_CACHE_COLD_MODEL,   /**< Another model */
   LLM_CACHE_COLD_THINKING /**< The thinking / effort settings changed */
} llm_cache_state_t;

/** Stable lowercase name for @p state ("warm", "first", "ttl", ...). */
const char *llm_cache_state_name(llm_cache_state_t state);

#define LLM_CACHE_MODEL_MAX 64
#define LLM_CACHE_THINKING_MAX 32

/** One LLM call's normalized usage: what the "LLM cache:" line reports. */
typedef struct {
   llm_type_t type;
   cloud_provider_t provider;
   char model[LLM_CACHE_MODEL_MAX]; /**< As sent ("" if the request wasn't noted) */
   llm_call_kind_t kind;
   int64_t conversation_id; /**< 0 when the call has none */
   int iteration;           /**< Tool-loop iteration; -1 outside a loop */
   int prompt;              /**< The whole prompt: read + write + uncached */
   int read;                /**< Read from the provider's cache */
   int write;               /**< Written to the provider's cache */
   int uncached;            /**< Billed at the full input price */
   int output;
   int expected;                          /**< Expected read; -1 = not computed */
   llm_cache_state_t state;               /**< Why expected is 0, when it is */
   uint64_t gap_ms;                       /**< Since the previous call on the key */
   uint32_t tools_hash;                   /**< 0 = no tools */
   uint32_t system_hash;                  /**< The stable system text */
   char thinking[LLM_CACHE_THINKING_MAX]; /**< e.g. "adaptive/low", "" = none sent */
   bool images;                           /**< The newest message carries an image */
   char miss_reason[32];                  /**< Anthropic's own diagnosis ("" = none) */
   int missed_tokens;                     /**< Input tokens that diagnosis says missed */
   llm_claude_drops_t drops; /**< Anthropic: thinking blocks the API dropped (binding controls) */
} llm_cache_record_t;

/**
 * @brief Tag the calling thread's LLM calls as @p kind until the matching pop
 *
 * For callers whose kind can't be told from their session (extraction,
 * compaction, briefings, ...).  Returns the previous tag for
 * llm_cache_monitor_pop_kind().
 */
int llm_cache_monitor_push_kind(llm_call_kind_t kind);

/** Restore the tag llm_cache_monitor_push_kind() returned. */
void llm_cache_monitor_pop_kind(int previous);

/** Whether the calling thread's calls are tagged as side calls (not a conversation's). */
bool llm_cache_monitor_in_side_call(void);

/**
 * @brief Whether the calling thread's call is sent once and never again (a
 *        compaction's summary): a cache write for it would only cost (a write
 *        is billed above an uncached read, and nothing reads it back)
 */
bool llm_cache_monitor_one_off_call(void);

/**
 * @brief The tool-loop iteration the calling thread's next call belongs to
 *
 * 0 for a turn's first call; -1 outside a tool loop.
 */
void llm_cache_monitor_set_iteration(int iteration);

/** The calling thread's tool-loop iteration (-1 outside a loop). */
int llm_cache_monitor_get_iteration(void);

/**
 * @brief Mark the calling thread as running the local microphone's turn
 *
 * That turn has no session: its calls are the turn's (and the default voice
 * user's) only while this is set.  Set by the local LLM worker for its turn.
 */
void llm_cache_monitor_set_local_mic(bool local_mic);

/**
 * @brief Note the request about to be sent on the calling thread
 *
 * Called at each provider path's send site with the finished request.  Records
 * its model and a hash of its cacheable prefix (tools + stable system text) for
 * the usage record that follows on the same thread.
 *
 * @param request The request JSON (not modified)
 */
void llm_cache_monitor_note_request(struct json_object *request);

/**
 * @brief Record one call's usage: build its record and log the "LLM cache:" line
 *
 * Called from llm_context_update_usage() for every provider call, with the
 * request noted on this thread (llm_cache_monitor_note_request).  @p usage
 * carries the normalized counts: prompt_tokens is the whole prompt, cached and
 * cache-write are parts of it.
 *
 * @param out Optional: the record, for the caller's own use of the call's kind
 *            and model
 */
void llm_cache_monitor_record(uint32_t session_id,
                              const llm_usage_report_t *usage,
                              llm_cache_record_t *out);

/**
 * @brief The last Anthropic response id on a conversation's cache key
 *
 * For the next request's diagnostics.previous_message_id (Anthropic cache
 * diagnostics compare the two requests and name where they diverged).
 *
 * @return true and fills @p out when there is one
 */
bool llm_cache_monitor_previous_message_id(uint32_t session_id,
                                           int64_t conversation_id,
                                           char *out,
                                           size_t out_len);

/**
 * @brief The prompt-cache TTL the expected-read math assumes, per provider
 *
 * A call more than this after the previous one on the same cache key is cold.
 */
int llm_cache_monitor_ttl_ms(llm_type_t type, cloud_provider_t provider);

/**
 * @brief Write the queued call records to the llm_usage_log table
 *
 * Records are queued by llm_cache_monitor_record() (which runs inside provider
 * callbacks, so it never touches the database) and written here in one
 * transaction.  Called from the main loop's once-a-second heartbeat.
 */
void llm_cache_monitor_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* LLM_CACHE_MONITOR_H */
