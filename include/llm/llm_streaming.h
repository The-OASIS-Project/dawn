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
 */

#ifndef LLM_STREAMING_H
#define LLM_STREAMING_H

#include <stddef.h>
#include <sys/time.h>

#include "llm/llm_claude_binding.h"
#include "llm/llm_interface.h"
#include "llm/llm_tools.h"

/**
 * @brief Initial size for Claude thinking signature buffer
 *
 * Claude's extended thinking returns a cryptographic signature that must be
 * preserved and sent back in conversation history. The buffer grows dynamically
 * since signature sizes vary across model versions.
 */
#define LLM_THINKING_SIGNATURE_INITIAL 8192

/**
 * @brief Callback function type for text chunks from LLM stream
 *
 * Called for each incremental text chunk received from the LLM.
 * The text should be processed immediately (e.g., sent to TTS).
 *
 * @param text Incremental text chunk
 * @param userdata User-provided context pointer
 */
typedef void (*text_chunk_callback)(const char *text, void *userdata);

/**
 * @brief Chunk types for extended thinking/reasoning support
 *
 * Used to distinguish between regular response text and thinking/reasoning
 * content from models that support extended thinking (Claude, DeepSeek-R1).
 */
typedef enum {
   LLM_CHUNK_TEXT,    /**< Regular response text content */
   LLM_CHUNK_THINKING /**< Thinking/reasoning content */
} llm_chunk_type_t;

/**
 * @brief Extended callback for chunks with type information
 *
 * Called for each chunk with its type, allowing callers to handle
 * thinking content separately from regular text (e.g., different UI display).
 *
 * @param type Chunk type (text or thinking)
 * @param text Chunk content
 * @param userdata User-provided context pointer
 */
typedef void (*llm_chunk_callback)(llm_chunk_type_t type, const char *text, void *userdata);

/* =============================================================================
 * Provider-Specific Stream State
 * =============================================================================
 * Each LLM provider has different streaming formats requiring different state:
 *
 * - Claude: Event-based state machine with explicit transitions
 *   (message_start → content_block_start → content_block_delta → ... → message_stop)
 *
 * - OpenAI: Self-contained chunks with incremental tool argument deltas
 *
 * - Local (llama.cpp): Uses OpenAI-compatible format
 * ============================================================================= */

/**
 * @brief Claude-specific streaming state
 *
 * Claude's SSE format is a state machine with explicit event types.
 * This struct tracks the current position in that state machine.
 */
typedef struct {
   int message_started;             /**< message_start event received */
   int content_block_active;        /**< Currently inside a content block */
   int input_tokens;                /**< Input tokens from message_start usage */
   int cache_creation_input_tokens; /**< Tokens written to the prompt cache this turn (non-zero
                                         when cache_control: ephemeral is honored and a fresh
                                         entry was created).  Populated from message_start.usage. */
   int cache_read_input_tokens;     /**< Tokens served from cache (billed at the model's cache-read
                                         price).  Populated from message_start.usage. */
   char message_id[64];             /**< message_start message.id (cache diagnostics' next key) */
   char cache_miss_reason[32]; /**< message_start diagnostics.cache_miss_reason.type ("" = none) */
   int cache_missed_tokens;    /**< ...cache_miss_reason.cache_missed_input_tokens */
   llm_claude_drops_t drops;   /**< input_transformations (message_start / final message_delta) */

   /* Tool use block tracking */
   int tool_block_active;              /**< Currently in a tool_use block */
   int tool_index;                     /**< Current tool block index */
   char tool_id[LLM_TOOLS_ID_LEN];     /**< Tool call ID from content_block_start */
   char tool_name[LLM_TOOLS_NAME_LEN]; /**< Tool name from content_block_start */
   char tool_args[LLM_TOOLS_ARGS_LEN]; /**< Accumulated partial_json */
   size_t tool_args_len;               /**< Length of accumulated args */
   bool tool_args_overflow;            /**< args exceeded the buffer and were clipped */

   /* Visual progress tracking (render_visual tool) */
   int visual_progress_active; /**< Notify frontend when render_visual generation starts */

   /* Thinking block tracking (extended thinking) */
   int thinking_block_active;     /**< Currently in a thinking block */
   char *thinking_signature;      /**< Accumulated signature (heap, grows as needed) */
   size_t thinking_signature_len; /**< Length of accumulated signature */
   size_t thinking_signature_cap; /**< Capacity of signature buffer */
} claude_stream_state_t;

/**
 * @brief OpenAI-specific streaming state
 *
 * OpenAI streams tool call arguments as deltas that must be accumulated.
 * Each tool call (up to LLM_TOOLS_MAX_PARALLEL_CALLS) has its own buffer.
 */
typedef struct {
   char tool_args_buffer[LLM_TOOLS_MAX_PARALLEL_CALLS][LLM_TOOLS_ARGS_LEN];
   bool tool_args_overflow[LLM_TOOLS_MAX_PARALLEL_CALLS]; /**< per-call: args clipped at the cap */
} openai_stream_state_t;

/**
 * @brief LLM stream context structure
 *
 * Maintains state for processing streaming LLM responses.
 * Extracts text deltas based on provider-specific format.
 */
typedef struct {
   /* Provider identification */
   llm_type_t llm_type;             /**< LLM type (LOCAL or CLOUD) */
   cloud_provider_t cloud_provider; /**< Cloud provider (if CLOUD) */

   /* Callback for streaming text to caller */
   text_chunk_callback callback; /**< User callback for text chunks */
   void *callback_userdata;      /**< User context passed to callback */

   /* Extended callback for thinking support (optional) */
   llm_chunk_callback chunk_callback; /**< Callback with chunk type (NULL if not used) */
   void *chunk_callback_userdata;     /**< User context for chunk callback */

   /* Provider-specific state (union - only one is active based on cloud_provider)
    * Using a union makes it explicit that Claude and OpenAI parsing are mutually
    * exclusive, and reduces memory from ~40KB to ~32KB per stream context. */
   union {
      claude_stream_state_t claude; /**< Claude state machine tracking */
      openai_stream_state_t openai; /**< OpenAI tool args accumulation */
   } provider;

   /* Accumulated complete response for conversation history */
   char *accumulated_response;
   size_t accumulated_size;
   size_t accumulated_capacity;

   /* Accumulated thinking content (extended thinking) */
   char *accumulated_thinking; /**< Full thinking content */
   size_t thinking_size;       /**< Current thinking content length */
   size_t thinking_capacity;   /**< Allocated thinking buffer size */
   int thinking_active;        /**< 1 if thinking content is being received */
   int has_thinking;           /**< 1 if any thinking content was received */
   int reasoning_tokens;       /**< OpenAI o-series reasoning tokens (from usage) */

   /* Inline <think> tag filtering state (for models that emit think tags in content) */
   int inside_think_tag;       /**< 1 if currently inside inline <think> block */
   char think_tag_partial[16]; /**< Partial tag buffer for cross-chunk splits */
   int think_tag_partial_len;  /**< Length of partial tag data */

   /* Stream completion tracking */
   int stream_complete;    /**< 1 when stream has ended */
   char finish_reason[32]; /**< Final finish/stop reason from stream */

   /* TTFT (Time To First Token) tracking for metrics */
   struct timeval stream_start_time; /**< When stream request was initiated */
   int first_token_received;         /**< Flag: 1 if first token has been received */

   /* Real-time token metrics (llama.cpp with timings_per_token: true) */
   int tokens_generated;       /**< Running count of output tokens from timings */
   float tokens_per_second;    /**< Current generation rate from timings */
   int realtime_prompt_tokens; /**< Prompt tokens from first timing chunk */
   int realtime_cached_tokens; /**< KV cache hit tokens from timings */

   /* Tool calls output (populated by either provider) */
   tool_call_list_t tool_calls; /**< Accumulated tool calls */
   int has_tool_calls;          /**< Flag: 1 if tool_calls detected in response */
} llm_stream_context_t;

/**
 * @brief Create a new LLM stream context
 *
 * @param llm_type LLM type (LLM_LOCAL or LLM_CLOUD)
 * @param cloud_provider Cloud provider (if LLM_CLOUD)
 * @param callback Function to call for each text chunk
 * @param userdata User context pointer passed to callback
 * @return Newly allocated stream context, or NULL on error
 */
llm_stream_context_t *llm_stream_create(llm_type_t llm_type,
                                        cloud_provider_t cloud_provider,
                                        text_chunk_callback callback,
                                        void *userdata);

/**
 * @brief Free an LLM stream context
 *
 * @param ctx Stream context to free
 */
void llm_stream_free(llm_stream_context_t *ctx);

/**
 * @brief Handle an SSE event from the stream
 *
 * Parses the event data (JSON) and extracts text chunks based on
 * provider-specific format. Calls the text callback for each chunk.
 *
 * @param ctx Stream context
 * @param event_data Event data (JSON string)
 */
void llm_stream_handle_event(llm_stream_context_t *ctx, const char *event_data);

/**
 * @brief Append text to a stream context's accumulated response buffer.
 *
 * Public wrapper around the internal accumulator. Used by provider-specific
 * SSE handlers (e.g. the OpenAI Responses path) that emit text outside of the
 * built-in chat-completions/Claude dispatchers but still want their output to
 * surface through llm_stream_get_response().
 */
void llm_stream_append_text(llm_stream_context_t *ctx, const char *text);

/**
 * @brief Append text to a stream context's accumulated thinking buffer.
 */
void llm_stream_append_thinking(llm_stream_context_t *ctx, const char *text);

/**
 * @brief Get the complete accumulated response
 *
 * Returns the full text response accumulated from all chunks.
 * This should be called after the stream is complete.
 *
 * @param ctx Stream context
 * @return Complete response string (caller must free), or NULL on error
 */
char *llm_stream_get_response(llm_stream_context_t *ctx);

/**
 * @brief Check if stream is complete
 *
 * @param ctx Stream context
 * @return 1 if stream has completed, 0 otherwise
 */
int llm_stream_is_complete(llm_stream_context_t *ctx);

/**
 * @brief Check if stream contains tool calls instead of text
 *
 * @param ctx Stream context
 * @return 1 if tool calls were detected, 0 otherwise
 */
int llm_stream_has_tool_calls(llm_stream_context_t *ctx);

/**
 * @brief Get the tool calls from the stream
 *
 * @param ctx Stream context
 * @return Pointer to tool_call_list_t (do not free), or NULL if no tool calls
 */
const tool_call_list_t *llm_stream_get_tool_calls(llm_stream_context_t *ctx);

/**
 * @brief Create stream context with extended thinking callback
 *
 * Like llm_stream_create() but adds a chunk callback that receives
 * typed chunks (text vs thinking) for extended thinking support.
 *
 * @param llm_type LLM type (LLM_LOCAL or LLM_CLOUD)
 * @param cloud_provider Cloud provider (if LLM_CLOUD)
 * @param callback Function to call for each text chunk (for TTS)
 * @param chunk_callback Function to call with typed chunks (for UI)
 * @param userdata User context pointer passed to callbacks
 * @return Newly allocated stream context, or NULL on error
 */
llm_stream_context_t *llm_stream_create_extended(llm_type_t llm_type,
                                                 cloud_provider_t cloud_provider,
                                                 text_chunk_callback callback,
                                                 llm_chunk_callback chunk_callback,
                                                 void *userdata);

/**
 * @brief Check if stream contains thinking content
 *
 * @param ctx Stream context
 * @return 1 if thinking content was received, 0 otherwise
 */
int llm_stream_has_thinking(llm_stream_context_t *ctx);

/**
 * @brief Get the accumulated thinking content
 *
 * Returns the full thinking content accumulated from all thinking chunks.
 * This should be called after the stream is complete.
 *
 * @param ctx Stream context
 * @return Complete thinking string (caller must free), or NULL if no thinking
 */
char *llm_stream_get_thinking(llm_stream_context_t *ctx);

/**
 * @brief Get the thinking signature (Claude extended thinking)
 *
 * Returns the signature that was provided with the thinking block.
 * This must be included when sending thinking content back to Claude.
 *
 * @param ctx Stream context
 * @return Signature string (caller must free), or NULL if no signature
 */
char *llm_stream_get_thinking_signature(llm_stream_context_t *ctx);

/**
 * @brief Get response without allocation (reference to internal buffer)
 *
 * Returns a pointer to the internal response buffer. Caller must NOT free.
 * Valid only while stream context exists.
 *
 * @param ctx Stream context
 * @return Response string (do NOT free), or NULL on error
 */
const char *llm_stream_get_response_ref(llm_stream_context_t *ctx);

/**
 * @brief Get thinking content without allocation (reference to internal buffer)
 *
 * Returns a pointer to the internal thinking buffer. Caller must NOT free.
 * Valid only while stream context exists.
 *
 * @param ctx Stream context
 * @return Thinking string (do NOT free), or NULL if no thinking
 */
const char *llm_stream_get_thinking_ref(llm_stream_context_t *ctx);

/**
 * @brief Get thinking signature without allocation (reference to internal buffer)
 *
 * Returns a pointer to the internal signature buffer. Caller must NOT free.
 * Valid only while stream context exists.
 *
 * @param ctx Stream context
 * @return Signature string (do NOT free), or NULL if no signature
 */
const char *llm_stream_get_thinking_signature_ref(llm_stream_context_t *ctx);

#endif  // LLM_STREAMING_H
