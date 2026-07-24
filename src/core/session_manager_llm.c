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
 * Session manager — LLM call orchestration.
 *
 * Owns the three public LLM entry points used by every caller that needs to
 * run a turn through the LLM (session_llm_call,
 * session_llm_call_with_tts, session_llm_call_with_tts_vision_no_add),
 * the three static helpers that share their setup/teardown contract
 * (llm_call_prepare / llm_call_cleanup / llm_call_finalize), and the two
 * sentence-buffer streaming callbacks (combined_sentence_callback /
 * combined_chunk_callback).
 *
 * Split out of session_manager.c so that file can stay under the size
 * limits in CLAUDE.md.  Uses only the public session_t API plus the LLM,
 * WebUI, and sentence-buffer interfaces — no file-static state from
 * session_manager.c is referenced here.
 */

#include <json-c/json.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "core/session_manager.h"
#include "core/text_filter.h"
#include "llm/llm_interface.h"
#include "logging.h"
#include "tools/time_utils.h"
#include "utils/sentence_buffer.h"
#ifdef ENABLE_WEBUI
#include "webui/webui_server.h"
#endif

/* =============================================================================
 * Streaming chunk callback
 *
 * Sends text to WebUI for real-time display.  Command-tag filtering is
 * handled by webui_send_stream_delta() internally.  Tracks timing metrics
 * for TTFT and token-rate visualization.  Used by session_llm_call() and
 * session_llm_call_with_tts_vision_no_add() — the paths that don't run
 * through the sentence-buffer (TTS) layer.
 * ============================================================================= */

static void session_text_chunk_callback(const char *chunk, void *userdata) {
   session_t *session = (session_t *)userdata;

   /* Early exit if client disconnected */
   if (!session || session->disconnected) {
      return;
   }

   /* Chunks are accumulated by the streaming layer (llm_streaming.c)
    * and also sent to WebSocket/satellite for real-time display */
#ifdef ENABLE_WEBUI
   if ((session->type == SESSION_TYPE_WEBUI || session->type == SESSION_TYPE_DAP2) && chunk &&
       chunk[0] != '\0') {
      uint64_t now_ms = get_time_ms();

      /* Track timing for first token (TTFT) — based on LLM output, not filtering */
      if (session->first_token_ms == 0) {
         session->first_token_ms = now_ms;
      }

      /* Track token count and timing for rate calculation */
      session->stream_token_count++;
      session->last_token_ms = now_ms;

      /* Calculate metrics (WebSocket only — satellites don't need browser metrics) */
      if (session->type == SESSION_TYPE_WEBUI) {
         int ttft_ms = 0;
         float token_rate = 0.0f;

         if (session->stream_start_ms > 0) {
            ttft_ms = (int)(session->first_token_ms - session->stream_start_ms);

            /* Calculate tokens per second based on elapsed time since first token */
            uint64_t streaming_duration_ms = now_ms - session->first_token_ms;
            if (streaming_duration_ms > 0 && session->stream_token_count > 1) {
               token_rate = (float)(session->stream_token_count - 1) * 1000.0f /
                            (float)streaming_duration_ms;
            }
         }

         /* Send metrics update periodically (every 5 tokens to avoid flooding) */
         if (session->stream_token_count % 5 == 0 || session->stream_token_count == 1) {
            webui_send_metrics_update(session, "thinking", ttft_ms, token_rate, -1);
         }
      }

      /* Send to WebUI/satellite — filtering and stream_start handled internally */
      webui_send_stream_delta(session, chunk);
   }
#endif
}

/* =============================================================================
 * LLM Integration
 * ============================================================================= */

/**
 * @brief Context for LLM call preparation (reduces duplication)
 */
typedef struct {
   struct json_object *history;
   const char *llm_input;
   llm_resolved_config_t resolved_config;
   char model_buf[LLM_MODEL_NAME_MAX]; /* Buffer for model name (outlives stack) */
   char endpoint_buf[128];             /* Buffer for endpoint (outlives stack) */
} llm_call_ctx_t;

/**
 * @brief Prepare for LLM call - common setup for all LLM call variants
 *
 * @param skip_add_message If true, don't add user message to history (caller already did)
 * @return 0 on success, non-zero on failure (session disconnected, config error, etc.)
 */
static int llm_call_prepare(session_t *session,
                            const char *user_text,
                            llm_call_ctx_t *ctx,
                            bool skip_add_message) {
   memset(ctx, 0, sizeof(*ctx));

   // Add user message to history (unless caller already did)
   if (!skip_add_message) {
      session_add_message(session, "user", user_text);
   }

   // Update activity timestamp
   session_touch(session);

   // Get conversation history
   ctx->history = session_get_history(session);
   if (!ctx->history) {
      OLOG_ERROR("Session %u: Failed to get conversation history", session->session_id);
      return 1;
   }

   ctx->llm_input = user_text;

   // Check for cancellation before LLM call
   if (session->cancel_requested) {
      OLOG_INFO("Session %u cancelled before LLM call", session->session_id);
      return 2;
   }

   // Get and resolve LLM config
   session_llm_config_t session_config = { 0 }; /* Zero-init for safety */
   session_get_llm_config(session, &session_config);

   int resolve_rc = llm_resolve_config(&session_config, &ctx->resolved_config);
   if (resolve_rc != 0) {
      OLOG_ERROR("Session %u: Failed to resolve LLM config (type=%d, provider=%d)",
                 session->session_id, session_config.type, session_config.cloud_provider);
      return 3;
   }

   /* Copy model/endpoint to ctx buffers (resolved pointers may point to stack) */
   if (ctx->resolved_config.model && ctx->resolved_config.model[0] != '\0') {
      strncpy(ctx->model_buf, ctx->resolved_config.model, sizeof(ctx->model_buf) - 1);
      ctx->model_buf[sizeof(ctx->model_buf) - 1] = '\0';
      ctx->resolved_config.model = ctx->model_buf;
   }
   if (ctx->resolved_config.endpoint && ctx->resolved_config.endpoint[0] != '\0') {
      strncpy(ctx->endpoint_buf, ctx->resolved_config.endpoint, sizeof(ctx->endpoint_buf) - 1);
      ctx->endpoint_buf[sizeof(ctx->endpoint_buf) - 1] = '\0';
      ctx->resolved_config.endpoint = ctx->endpoint_buf;
   }

   // Set command context for tool callbacks
   session_set_command_context(session);

   // Reset streaming filter state for WebSocket and satellite sessions
   if (session->type == SESSION_TYPE_WEBUI || session->type == SESSION_TYPE_DAP2) {
      session->cmd_tag_filter.nesting_depth = 0;
      session->cmd_tag_filter.len = 0;
      session->stream_had_content = false;
   }

   return 0;
}

/**
 * @brief Clean up LLM call context resources
 */
static void llm_call_cleanup(llm_call_ctx_t *ctx) {
   session_set_command_context(NULL);
   if (ctx->history) {
      json_object_put(ctx->history);
   }
}

/**
 * @brief Finalize LLM call - handle WebSocket streaming end and add to history
 *
 * @return Response string on success, NULL on failure (takes ownership of response)
 */
static char *llm_call_finalize(session_t *session, char *response, llm_call_ctx_t *ctx) {
   /* Capture the provider's error code before any cleanup path can mutate it.
    * Used both to set a distinguishable WebUI stream-end reason and to refine
    * the failure log so transient retries-exhausted aren't conflated with
    * hard errors (auth, parse, bad request).
    *
    * INVARIANT: this function must execute on the same thread that drove the
    * LLM call.  llm_last_error() reads thread-local storage; a future
    * refactor to dispatch finalize onto a different thread would silently
    * read LLM_ERR_NONE here.  All current callers (session_llm_call,
    * session_llm_call_no_add) satisfy this. */
   int post_call_error = llm_last_error();
#ifdef ENABLE_WEBUI
   // End WebSocket streaming
   if (session->type == SESSION_TYPE_WEBUI) {
      // Send idle state update (rate is calculated accurately by streaming layer from usage stats)
      // Only send if we had streaming activity
      if (session->stream_token_count > 0 && session->first_token_ms > 0) {
         int ttft_ms = (int)(session->first_token_ms - session->stream_start_ms);
         // Send idle state with TTFT but rate=0 (accurate rate already sent by streaming layer)
         webui_send_metrics_update(session, "idle", ttft_ms, 0.0f, -1);
      }

      if (session->llm_streaming_active) {
         const char *reason;
         if (response) {
            reason = "complete";
         } else if (post_call_error == LLM_ERR_TRANSIENT_NETWORK) {
            reason = "transient_error";
         } else {
            reason = "error";
         }
         webui_send_stream_end(session, reason);
      }
      // Fallback for non-streaming LLMs
      if (response && !session->stream_had_content && session->cmd_tag_filter.nesting_depth == 0) {
         if (*response) {
            OLOG_INFO("Session %u: LLM didn't stream, sending full transcript",
                      session->session_id);
            webui_send_transcript(session, "assistant", response);
         } else {
            /* Empty completion AND nothing streamed — the model produced no text and no
             * user-visible tool output (observed with some preview models returning an empty
             * completion after a tool call). The all-tools-silent case takes a different path,
             * so reaching here means the user genuinely got nothing. Surface a brief notice
             * instead of dead air. Display-only: the empty response is not added to history
             * below, so this never re-enters LLM context or a reload. */
            OLOG_WARNING("Session %u: empty completion with no streamed output — surfacing "
                         "fallback notice",
                         session->session_id);
            webui_send_transcript(session, "assistant",
                                  "Hmm, I didn't generate a response that time — mind trying "
                                  "again or rephrasing?");
         }
      }
   }
#endif

   // Clean up context
   llm_call_cleanup(ctx);

   // Check for cancellation after LLM call
   if (session->cancel_requested) {
      OLOG_INFO("Session %u cancelled during LLM call", session->session_id);
      free(response);
      return NULL;
   }

   if (!response) {
      if (post_call_error == LLM_ERR_TRANSIENT_NETWORK) {
         OLOG_ERROR("Session %u: LLM call failed (transient network error, retries exhausted)",
                    session->session_id);
      } else {
         OLOG_ERROR("Session %u: LLM call failed", session->session_id);
      }
      return NULL;
   }

   // Add assistant response to history (only if non-empty to avoid Claude API errors)
   if (*response) {
      session_add_message(session, "assistant", response);
   } else {
      OLOG_WARNING("Session %u: LLM returned empty response, not adding to history",
                   session->session_id);
   }

   OLOG_INFO("Session %u: LLM response received (%.50s%s)", session->session_id, response,
             strlen(response) > 50 ? "..." : "");

   return response;
}

char *session_llm_call(session_t *session, const char *user_text) {
   if (!session || !user_text) {
      return NULL;
   }

   if (session->cancel_requested) {
      OLOG_INFO("Session %u cancelled, aborting LLM call", session->session_id);
      return NULL;
   }

   llm_call_ctx_t ctx;
   if (llm_call_prepare(session, user_text, &ctx, false) != 0) {
      llm_call_cleanup(&ctx);
      return NULL;
   }

   OLOG_INFO("Session %u: Calling LLM with %d messages in history", session->session_id,
             json_object_array_length(ctx.history));

   /* Initialize streaming metrics before LLM call */
   session->stream_start_ms = get_time_ms();
   session->first_token_ms = 0;
   session->last_token_ms = 0;
   session->stream_token_count = 0;

   /* Set per-session cancel flag for multi-user WebUI support */
   llm_set_cancel_flag(&session->cancel_requested);

   char *response = llm_chat_completion_streaming_with_config(ctx.history, ctx.llm_input, NULL,
                                                              NULL, 0, session_text_chunk_callback,
                                                              session, &ctx.resolved_config);

   /* Clear per-session cancel flag */
   llm_set_cancel_flag(NULL);

   return llm_call_finalize(session, response, &ctx);
}

/**
 * @brief Combined streaming context for text display + sentence audio
 *
 * Used by session_llm_call_with_tts() to simultaneously:
 * 1. Stream text chunks to WebUI for real-time display
 * 2. Buffer chunks into sentences for TTS audio generation
 */
typedef struct {
   session_t *session;                     // Session for text streaming
   sentence_buffer_t *sentence_buffer;     // Sentence detection for TTS
   session_sentence_callback sentence_cb;  // User's sentence callback
   void *sentence_userdata;                // User's callback context
} combined_stream_ctx_t;

/**
 * @brief Internal sentence callback - forwards to user's callback
 */
static void combined_sentence_callback(const char *sentence, void *userdata) {
   combined_stream_ctx_t *ctx = (combined_stream_ctx_t *)userdata;
   if (ctx->sentence_cb) {
      ctx->sentence_cb(sentence, ctx->sentence_userdata);
   }
}

/**
 * @brief Combined chunk callback - does text streaming AND feeds sentence buffer
 *
 * Each chunk is:
 * 1. Filtered to remove <command>...</command> tags
 * 2. Sent to WebUI for real-time text display
 * 3. Fed to sentence buffer for TTS audio generation
 *
 * By filtering once and passing to both consumers, we avoid duplicate
 * command tag tracking in the sentence buffer.
 */
static void combined_chunk_callback(const char *chunk, void *userdata) {
   combined_stream_ctx_t *ctx = (combined_stream_ctx_t *)userdata;
   session_t *session = ctx->session;

   // Early exit if client disconnected (avoid unnecessary TTS/WebSocket work)
   if (!session || session->disconnected || !chunk || !chunk[0]) {
      return;
   }

   uint64_t now_ms = get_time_ms();

   /* Track timing for first token (TTFT) - based on LLM output, not filtering */
   if (session->first_token_ms == 0) {
      session->first_token_ms = now_ms;
   }

   /* Track token count and timing for rate calculation */
   session->stream_token_count++;
   session->last_token_ms = now_ms;

   /* Filter command tags ONCE for both consumers (WebUI and TTS).
    * This updates session state and returns filtered text.
    * Both consumers use the same filtered output for consistency. */
   char filtered[256];
   int len;

   if (session->cmd_tag_filter_bypass) {
      /* Native tools mode: no command tags to filter, pass through */
      size_t chunk_len = strlen(chunk);
      len = chunk_len < sizeof(filtered) - 1 ? (int)chunk_len : (int)(sizeof(filtered) - 1);
      memcpy(filtered, chunk, (size_t)len);
      filtered[len] = '\0';
   } else {
      /* Legacy mode: filter <command>...</command> tags */
      len = text_filter_command_tags_to_buffer(&session->cmd_tag_filter, chunk, filtered,
                                               sizeof(filtered));
   }

   if (len > 0) {
#ifdef ENABLE_WEBUI
      // 1. Send filtered text to WebUI for real-time display
      if (session->type == SESSION_TYPE_WEBUI) {
         /* Calculate metrics */
         int ttft_ms = 0;
         float token_rate = 0.0f;

         if (session->stream_start_ms > 0) {
            ttft_ms = (int)(session->first_token_ms - session->stream_start_ms);

            uint64_t streaming_duration_ms = now_ms - session->first_token_ms;
            if (streaming_duration_ms > 0 && session->stream_token_count > 1) {
               token_rate = (float)(session->stream_token_count - 1) * 1000.0f /
                            (float)streaming_duration_ms;
            }
         }

         /* Send metrics update periodically */
         if (session->stream_token_count % 5 == 0 || session->stream_token_count == 1) {
            webui_send_metrics_update(session, "thinking", ttft_ms, token_rate, -1);
         }

         /* Send pre-filtered text (no command tags to filter) */
         webui_send_stream_delta(session, filtered);
      }
#endif

      // 2. Feed filtered text to sentence buffer for TTS
      if (ctx->sentence_buffer) {
         sentence_buffer_feed(ctx->sentence_buffer, filtered);
      }
   }
}

char *session_llm_call_with_tts(session_t *session,
                                const char *user_text,
                                session_sentence_callback sentence_cb,
                                void *userdata) {
   if (!session || !user_text) {
      return NULL;
   }

   if (session->cancel_requested) {
      OLOG_INFO("Session %u cancelled, aborting LLM call", session->session_id);
      return NULL;
   }

   llm_call_ctx_t ctx;
   if (llm_call_prepare(session, user_text, &ctx, false) != 0) {
      llm_call_cleanup(&ctx);
      return NULL;
   }

   OLOG_INFO("Session %u: Calling LLM (TTS streaming) with %d messages in history",
             session->session_id, json_object_array_length(ctx.history));

   /* Initialize streaming metrics before LLM call */
   session->stream_start_ms = get_time_ms();
   session->first_token_ms = 0;
   session->last_token_ms = 0;
   session->stream_token_count = 0;

   // Create combined streaming context for text + sentence audio
   combined_stream_ctx_t stream_ctx = { .session = session,
                                        .sentence_cb = sentence_cb,
                                        .sentence_userdata = userdata,
                                        .sentence_buffer = NULL };

   // Create sentence buffer for TTS
   stream_ctx.sentence_buffer = sentence_buffer_create(combined_sentence_callback, &stream_ctx);
   if (!stream_ctx.sentence_buffer) {
      OLOG_ERROR("Session %u: Failed to create sentence buffer", session->session_id);
      llm_call_cleanup(&ctx);
      return NULL;
   }

   /* Set per-session cancel flag for multi-user WebUI support */
   llm_set_cancel_flag(&session->cancel_requested);

   // Call LLM with combined callback (text streaming + sentence buffering)
   char *response = llm_chat_completion_streaming_with_config(ctx.history, ctx.llm_input, NULL,
                                                              NULL, 0, combined_chunk_callback,
                                                              &stream_ctx, &ctx.resolved_config);

   /* Clear per-session cancel flag */
   llm_set_cancel_flag(NULL);

   // Flush remaining sentence and free buffer
   sentence_buffer_flush(stream_ctx.sentence_buffer);
   sentence_buffer_free(stream_ctx.sentence_buffer);

   return llm_call_finalize(session, response, &ctx);
}

char *session_llm_call_with_tts_vision_no_add(session_t *session,
                                              const char *user_text,
                                              const char **vision_images,
                                              const size_t *vision_image_sizes,
                                              const char (*vision_mimes)[24],
                                              int vision_image_count,
                                              session_sentence_callback sentence_cb,
                                              void *userdata) {
   (void)vision_mimes; /* MIME types passed to LLM layer via provider-specific handling */

   if (!session || !user_text) {
      return NULL;
   }

   if (session->cancel_requested) {
      OLOG_INFO("Session %u cancelled, aborting LLM call", session->session_id);
      return NULL;
   }

   llm_call_ctx_t ctx;
   if (llm_call_prepare(session, user_text, &ctx, true) != 0) {
      llm_call_cleanup(&ctx);
      return NULL;
   }

   /* Log call details */
   const char *mode = sentence_cb ? (vision_image_count > 0 ? "TTS+vision" : "TTS") : "no-add";
   if (vision_image_count > 0) {
      size_t total_bytes = 0;
      for (int i = 0; i < vision_image_count; i++) {
         if (vision_image_sizes) {
            total_bytes += vision_image_sizes[i];
         }
      }
      OLOG_INFO("Session %u: Calling LLM (%s) with %d messages + %d images (%zu bytes)",
                session->session_id, mode, json_object_array_length(ctx.history),
                vision_image_count, total_bytes);
   } else {
      OLOG_INFO("Session %u: Calling LLM (%s) with %d messages in history", session->session_id,
                mode, json_object_array_length(ctx.history));
   }

   /* Initialize streaming metrics before LLM call */
   session->stream_start_ms = get_time_ms();
   session->first_token_ms = 0;
   session->last_token_ms = 0;
   session->stream_token_count = 0;

   /* Set per-session cancel flag for multi-user WebUI support */
   llm_set_cancel_flag(&session->cancel_requested);

   char *response;

   if (sentence_cb) {
      /* TTS enabled: use sentence buffering for per-sentence audio */
      combined_stream_ctx_t stream_ctx = { .session = session,
                                           .sentence_cb = sentence_cb,
                                           .sentence_userdata = userdata,
                                           .sentence_buffer = NULL };

      stream_ctx.sentence_buffer = sentence_buffer_create(combined_sentence_callback, &stream_ctx);
      if (!stream_ctx.sentence_buffer) {
         OLOG_ERROR("Session %u: Failed to create sentence buffer", session->session_id);
         llm_set_cancel_flag(NULL);
         llm_call_cleanup(&ctx);
         return NULL;
      }

      response = llm_chat_completion_streaming_with_config(
          ctx.history, ctx.llm_input, vision_images, vision_image_sizes, vision_image_count,
          combined_chunk_callback, &stream_ctx, &ctx.resolved_config);

      sentence_buffer_flush(stream_ctx.sentence_buffer);
      sentence_buffer_free(stream_ctx.sentence_buffer);
   } else {
      /* No TTS: use simple text streaming callback */
      response = llm_chat_completion_streaming_with_config(
          ctx.history, ctx.llm_input, vision_images, vision_image_sizes, vision_image_count,
          session_text_chunk_callback, session, &ctx.resolved_config);
   }

   /* Clear per-session cancel flag */
   llm_set_cancel_flag(NULL);

   return llm_call_finalize(session, response, &ctx);
}
