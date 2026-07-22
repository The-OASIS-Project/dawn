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

#include "llm/llm_streaming.h"

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "core/session_manager.h"
#include "llm/llm_context.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "ui/metrics.h"
#include "webui/webui_server.h"

#define DEFAULT_ACCUMULATED_CAPACITY 8192
#define DEFAULT_THINKING_CAPACITY 4096
#define MAX_ACCUMULATED_SIZE (10 * 1024 * 1024)  // 10MB hard limit for LLM responses
#define MAX_THINKING_SIZE (2 * 1024 * 1024)      // 2MB hard limit for thinking content

/**
 * @brief Record TTFT metric if this is the first token
 *
 * Called when first text token is received from LLM stream.
 */
static void record_ttft_if_first_token(llm_stream_context_t *ctx) {
   if (ctx->first_token_received) {
      return;  // Already recorded
   }

   ctx->first_token_received = 1;

   struct timeval now;
   gettimeofday(&now, NULL);

   double ttft_ms = (now.tv_sec - ctx->stream_start_time.tv_sec) * 1000.0 +
                    (now.tv_usec - ctx->stream_start_time.tv_usec) / 1000.0;

   OLOG_INFO("LLM TTFT: %.1f ms", ttft_ms);
   metrics_record_llm_ttft(ttft_ms);
}

/**
 * @brief Append text to accumulated response buffer
 *
 * Forward declaration so the public wrappers can be defined before the static
 * definition without confusing the compiler.
 */
static int append_to_accumulated(llm_stream_context_t *ctx, const char *text);
static int append_to_thinking(llm_stream_context_t *ctx, const char *text);

void llm_stream_append_text(llm_stream_context_t *ctx, const char *text) {
   if (ctx)
      append_to_accumulated(ctx, text);
}

void llm_stream_append_thinking(llm_stream_context_t *ctx, const char *text) {
   if (ctx)
      append_to_thinking(ctx, text);
}

static int append_to_accumulated(llm_stream_context_t *ctx, const char *text) {
   if (!text || !*text) {
      return 1;
   }

   size_t text_len = strlen(text);
   size_t needed = ctx->accumulated_size + text_len + 1;

   // Prevent runaway memory allocation from excessively long LLM responses
   if (needed > MAX_ACCUMULATED_SIZE) {
      OLOG_ERROR("Accumulated response size limit exceeded: %zu bytes, maximum %zu bytes (%.1f MB)",
                 needed, MAX_ACCUMULATED_SIZE, MAX_ACCUMULATED_SIZE / (1024.0 * 1024.0));
      return 0;
   }

   // Reallocate if needed
   if (needed > ctx->accumulated_capacity) {
      size_t new_capacity = ctx->accumulated_capacity * 2;
      while (new_capacity < needed) {
         new_capacity *= 2;
      }

      // Cap at maximum size
      if (new_capacity > MAX_ACCUMULATED_SIZE) {
         new_capacity = MAX_ACCUMULATED_SIZE;
      }

      char *new_buffer = realloc(ctx->accumulated_response, new_capacity);
      if (!new_buffer) {
         OLOG_ERROR("Failed to reallocate accumulated response buffer");
         return 0;
      }

      ctx->accumulated_response = new_buffer;
      ctx->accumulated_capacity = new_capacity;
   }

   // Append text
   memcpy(ctx->accumulated_response + ctx->accumulated_size, text, text_len);
   ctx->accumulated_size += text_len;
   ctx->accumulated_response[ctx->accumulated_size] = '\0';

   return 1;
}

/**
 * @brief Append text to accumulated thinking buffer
 */
static int append_to_thinking(llm_stream_context_t *ctx, const char *text) {
   if (!text || !*text) {
      return 1;
   }

   size_t text_len = strlen(text);

   // Lazy initialization of thinking buffer
   if (!ctx->accumulated_thinking) {
      ctx->accumulated_thinking = malloc(DEFAULT_THINKING_CAPACITY);
      if (!ctx->accumulated_thinking) {
         OLOG_ERROR("Failed to allocate thinking buffer");
         return 0;
      }
      ctx->accumulated_thinking[0] = '\0';
      ctx->thinking_capacity = DEFAULT_THINKING_CAPACITY;
      ctx->thinking_size = 0;
   }

   size_t needed = ctx->thinking_size + text_len + 1;

   // Prevent runaway memory allocation
   if (needed > MAX_THINKING_SIZE) {
      OLOG_WARNING("Thinking content size limit exceeded: %zu bytes", needed);
      return 0;
   }

   // Reallocate if needed
   if (needed > ctx->thinking_capacity) {
      size_t new_capacity = ctx->thinking_capacity * 2;
      while (new_capacity < needed) {
         new_capacity *= 2;
      }
      if (new_capacity > MAX_THINKING_SIZE) {
         new_capacity = MAX_THINKING_SIZE;
      }

      char *new_buffer = realloc(ctx->accumulated_thinking, new_capacity);
      if (!new_buffer) {
         OLOG_ERROR("Failed to reallocate thinking buffer");
         return 0;
      }

      ctx->accumulated_thinking = new_buffer;
      ctx->thinking_capacity = new_capacity;
   }

   // Append text
   memcpy(ctx->accumulated_thinking + ctx->thinking_size, text, text_len);
   ctx->thinking_size += text_len;
   ctx->accumulated_thinking[ctx->thinking_size] = '\0';

   return 1;
}

/**
 * @brief Emit text as thinking content (accumulate + callbacks + WebUI)
 */
static void emit_thinking(llm_stream_context_t *ctx,
                          const char *text,
                          int has_ws_session,
                          session_t *ws_session) {
   append_to_thinking(ctx, text);
   if (ctx->chunk_callback) {
      ctx->chunk_callback(LLM_CHUNK_THINKING, text, ctx->chunk_callback_userdata);
   }
   if (has_ws_session) {
      webui_send_thinking_delta(ws_session, text);
   }
}

/**
 * @brief Emit text as response content (accumulate + callbacks + TTFT)
 */
static void emit_response(llm_stream_context_t *ctx,
                          const char *text,
                          int has_ws_session,
                          session_t *ws_session) {
   (void)has_ws_session;
   (void)ws_session;
   record_ttft_if_first_token(ctx);
   ctx->callback(text, ctx->callback_userdata);
   if (ctx->chunk_callback) {
      ctx->chunk_callback(LLM_CHUNK_TEXT, text, ctx->chunk_callback_userdata);
   }
   append_to_accumulated(ctx, text);
}

/**
 * @brief Emit text routed by current think tag state
 */
static void emit_content(llm_stream_context_t *ctx,
                         const char *text,
                         int has_ws_session,
                         session_t *ws_session) {
   if (ctx->inside_think_tag) {
      emit_thinking(ctx, text, has_ws_session, ws_session);
   } else {
      emit_response(ctx, text, has_ws_session, ws_session);
   }
}

/**
 * @brief Handle a matched <think> open tag
 */
static void handle_think_open(llm_stream_context_t *ctx,
                              int has_ws_session,
                              session_t *ws_session) {
   if (ctx->inside_think_tag) {
      OLOG_WARNING("LLM: Nested <think> tag ignored (already inside think block)");
      return;
   }
   ctx->thinking_active = 1;
   ctx->has_thinking = 1;
   ctx->inside_think_tag = 1;
   OLOG_INFO("LLM: Inline <think> tag detected");
   if (has_ws_session) {
      webui_send_thinking_start(ws_session, "local");
   }
}

/**
 * @brief Handle a matched </think> close tag
 */
static void handle_think_close(llm_stream_context_t *ctx,
                               int has_ws_session,
                               session_t *ws_session) {
   if (!ctx->inside_think_tag) {
      OLOG_WARNING("LLM: Stray </think> tag ignored (not inside think block)");
      return;
   }
   ctx->inside_think_tag = 0;
   ctx->thinking_active = 0;
   OLOG_INFO("LLM: Inline </think> tag closed");
   if (has_ws_session) {
      webui_send_thinking_end(ws_session, ctx->thinking_size > 0);
   }
}

#define THINK_OPEN_LEN 7  /* strlen("<think>") */
#define THINK_CLOSE_LEN 8 /* strlen("</think>") */

/**
 * @brief Try to resolve partial tag buffer against <think> or </think>
 *
 * @return 1 if tag was resolved (matched or rejected), 0 if still partial
 */
static int try_resolve_partial(llm_stream_context_t *ctx,
                               int has_ws_session,
                               session_t *ws_session) {
   int len = ctx->think_tag_partial_len;

   /* Check for complete <think> match */
   if (len >= THINK_OPEN_LEN && strncmp(ctx->think_tag_partial, "<think>", THINK_OPEN_LEN) == 0) {
      handle_think_open(ctx, has_ws_session, ws_session);
      /* Flush any trailing chars after the tag as content */
      if (len > THINK_OPEN_LEN) {
         ctx->think_tag_partial[len] = '\0';
         emit_content(ctx, ctx->think_tag_partial + THINK_OPEN_LEN, has_ws_session, ws_session);
      }
      ctx->think_tag_partial_len = 0;
      return 1;
   }

   /* Check for complete </think> match */
   if (len >= THINK_CLOSE_LEN &&
       strncmp(ctx->think_tag_partial, "</think>", THINK_CLOSE_LEN) == 0) {
      handle_think_close(ctx, has_ws_session, ws_session);
      ctx->think_tag_partial_len = 0;
      return 1;
   }

   /* Check if partial buffer can still match either tag */
   int could_match_open = (len <= THINK_OPEN_LEN &&
                           strncmp(ctx->think_tag_partial, "<think>", len) == 0);
   int could_match_close = (len <= THINK_CLOSE_LEN &&
                            strncmp(ctx->think_tag_partial, "</think>", len) == 0);

   if (!could_match_open && !could_match_close) {
      /* Not a tag — flush partial buffer as content */
      ctx->think_tag_partial[len] = '\0';
      emit_content(ctx, ctx->think_tag_partial, has_ws_session, ws_session);
      ctx->think_tag_partial_len = 0;
      return 1;
   }

   return 0; /* Still partial, need more data */
}

/**
 * @brief Filter inline <think>...</think> tags from streaming content
 *
 * Processes text character-by-character when a '<' is detected, otherwise
 * operates on spans for efficiency. Text inside <think> tags is redirected
 * to the thinking buffer; text outside goes to the normal response path.
 *
 * @param ctx Stream context (carries inside_think_tag state)
 * @param text Input text chunk from the content delta
 * @param has_ws_session Whether a WebUI session is active
 * @param ws_session The WebUI session (may be NULL)
 */
static void filter_think_tags(llm_stream_context_t *ctx,
                              const char *text,
                              int has_ws_session,
                              session_t *ws_session) {
   if (!text || !*text) {
      return;
   }

   const char *p = text;

   /* If we have a partial tag from previous chunk, try to complete it */
   if (ctx->think_tag_partial_len > 0) {
      while (*p && ctx->think_tag_partial_len < (int)sizeof(ctx->think_tag_partial) - 1) {
         ctx->think_tag_partial[ctx->think_tag_partial_len++] = *p++;
         if (try_resolve_partial(ctx, has_ws_session, ws_session)) {
            goto process_remaining;
         }
      }
      /* Exhausted input while still in partial state — wait for more data */
      if (ctx->think_tag_partial_len > 0) {
         return;
      }
   }

process_remaining:
   while (*p) {
      if (*p == '<') {
         /* Start collecting a potential tag */
         ctx->think_tag_partial[0] = '<';
         ctx->think_tag_partial_len = 1;
         p++;

         /* Try to resolve from remaining input */
         while (*p && ctx->think_tag_partial_len < (int)sizeof(ctx->think_tag_partial) - 1) {
            ctx->think_tag_partial[ctx->think_tag_partial_len++] = *p++;
            if (try_resolve_partial(ctx, has_ws_session, ws_session)) {
               goto process_remaining;
            }
         }
         /* Ran out of input while matching — partial stays buffered */
         return;
      }

      /* Fast path: scan span of non-'<' characters */
      const char *span_start = p;
      while (*p && *p != '<') {
         p++;
      }

      /* Emit the span (C1 fix: use strndup instead of const-cast null-termination) */
      size_t span_len = (size_t)(p - span_start);
      if (span_len > 0) {
         char *span = strndup(span_start, span_len);
         if (span) {
            emit_content(ctx, span, has_ws_session, ws_session);
            free(span);
         }
      }
   }
}

/**
 * @brief Emit a chunk of reasoning/thinking text to all sinks.
 *
 * Marks thinking active (sending thinking_start to the WebUI on the first chunk),
 * forwards the text to the chunk callback (LLM_CHUNK_THINKING), streams it to the
 * WebUI for live display, and accumulates it.  Shared by the llama.cpp
 * `reasoning_content` path and the OpenRouter `reasoning_details` path.  No-op on
 * NULL/empty text.
 */
static void stream_emit_thinking(llm_stream_context_t *ctx,
                                 const char *text,
                                 int has_ws_session,
                                 session_t *ws_session,
                                 const char *provider_label) {
   if (!text || text[0] == '\0') {
      return;
   }
   /* On the first reasoning chunk, mark thinking active + announce thinking_start. */
   if (!ctx->thinking_active) {
      ctx->thinking_active = 1;
      ctx->has_thinking = 1;
      if (has_ws_session) {
         webui_send_thinking_start(ws_session, provider_label);
      }
   }
   /* Delegate accumulate + chunk-callback + WebUI delta to the shared helper. */
   emit_thinking(ctx, text, has_ws_session, ws_session);
}

/**
 * @brief Parse OpenAI/llama.cpp streaming chunk
 *
 * Format: {"choices":[{"delta":{"content":"text"}}]}
 * Or for tool calls: {"choices":[{"delta":{"tool_calls":[...]}}]}
 * Or: [DONE]
 */
static void parse_openai_chunk(llm_stream_context_t *ctx, const char *event_data) {
   // Check for [DONE] signal
   if (strcmp(event_data, "[DONE]") == 0) {
      ctx->stream_complete = 1;
      return;
   }

   // Parse JSON
   json_object *chunk = json_tokener_parse(event_data);
   if (!chunk) {
      OLOG_WARNING("Failed to parse OpenAI chunk JSON");
      return;
   }

   // Cache session lookup for WebUI notifications (avoids repeated lookups)
   session_t *ws_session = session_get_command_context();
   int has_ws_session = (ws_session && ws_session->type == SESSION_TYPE_WEBUI);

   // Extract choices[0].delta.content or tool_calls
   json_object *choices, *first_choice, *delta, *content;

   if (json_object_object_get_ex(chunk, "choices", &choices) &&
       json_object_get_type(choices) == json_type_array && json_object_array_length(choices) > 0) {
      first_choice = json_object_array_get_idx(choices, 0);

      if (json_object_object_get_ex(first_choice, "delta", &delta)) {
         // Check for reasoning_content (llama.cpp with DeepSeek-R1 or reasoning models)
         json_object *reasoning_content;
         if (json_object_object_get_ex(delta, "reasoning_content", &reasoning_content)) {
            const char *thinking_text = json_object_get_string(reasoning_content);
            if (thinking_text && thinking_text[0] != '\0') {
               if (!ctx->thinking_active) {
                  OLOG_INFO("LLM: Reasoning content detected (llama.cpp)");
               }
               stream_emit_thinking(ctx, thinking_text, has_ws_session, ws_session, "local");
            }
         }

         // OpenRouter unified reasoning: streaming chunks carry reasoning in
         // delta.reasoning_details[] (reasoning.text -> text, reasoning.summary ->
         // summary; reasoning.encrypted holds opaque base64 in .data -> skip).  Some
         // providers instead stream a flat delta.reasoning string; use it only when no
         // details array is present so the same text isn't surfaced twice.
         json_object *reasoning_details;
         if (json_object_object_get_ex(delta, "reasoning_details", &reasoning_details) &&
             json_object_get_type(reasoning_details) == json_type_array) {
            int rd_count = json_object_array_length(reasoning_details);
            for (int ri = 0; ri < rd_count; ri++) {
               json_object *entry = json_object_array_get_idx(reasoning_details, ri);
               if (!entry || json_object_get_type(entry) != json_type_object) {
                  continue;
               }
               json_object *type_obj, *val_obj = NULL;
               const char *rtype = json_object_object_get_ex(entry, "type", &type_obj)
                                       ? json_object_get_string(type_obj)
                                       : NULL;
               const char *think = NULL;
               if (rtype && strcmp(rtype, "reasoning.text") == 0 &&
                   json_object_object_get_ex(entry, "text", &val_obj)) {
                  think = json_object_get_string(val_obj);
               } else if (rtype && strcmp(rtype, "reasoning.summary") == 0 &&
                          json_object_object_get_ex(entry, "summary", &val_obj)) {
                  think = json_object_get_string(val_obj);
               }
               stream_emit_thinking(ctx, think, has_ws_session, ws_session, "openrouter");
            }
         } else {
            json_object *reasoning_str;
            if (json_object_object_get_ex(delta, "reasoning", &reasoning_str) &&
                json_object_get_type(reasoning_str) == json_type_string) {
               stream_emit_thinking(ctx, json_object_get_string(reasoning_str), has_ws_session,
                                    ws_session, "openrouter");
            }
         }

         // Check for text content
         if (json_object_object_get_ex(delta, "content", &content)) {
            const char *text = json_object_get_string(content);
            if (text && text[0] != '\0') {
               // If we were in thinking mode (from reasoning_content), transition
               if (ctx->thinking_active && !ctx->inside_think_tag) {
                  ctx->thinking_active = 0;
                  OLOG_INFO("LLM: Transitioned from reasoning to response");

                  // Send thinking_end to WebUI
                  if (has_ws_session) {
                     webui_send_thinking_end(ws_session, ctx->thinking_size > 0);
                  }
               }

               // Filter inline <think>...</think> tags from content.
               // Local models (Qwen3) use these for reasoning; cloud models
               // occasionally leak stray </think> tags in responses.
               filter_think_tags(ctx, text, has_ws_session, ws_session);
            }
         }

         // Check for tool_calls (streaming tool calls)
         json_object *tool_calls;
         if (json_object_object_get_ex(delta, "tool_calls", &tool_calls) &&
             json_object_get_type(tool_calls) == json_type_array) {
            ctx->has_tool_calls = 1;

            int tc_len = json_object_array_length(tool_calls);
            for (int i = 0; i < tc_len; i++) {
               json_object *tc = json_object_array_get_idx(tool_calls, i);
               json_object *index_obj, *id_obj, *function_obj;

               // Get index (which tool call this is part of)
               int tc_index = 0;
               if (json_object_object_get_ex(tc, "index", &index_obj)) {
                  tc_index = json_object_get_int(index_obj);
               }

               if (tc_index >= LLM_TOOLS_MAX_PARALLEL_CALLS) {
                  continue;
               }

               // First chunk for this tool call has id and function.name
               if (json_object_object_get_ex(tc, "id", &id_obj)) {
                  const char *id = json_object_get_string(id_obj);
                  if (id) {
                     strncpy(ctx->tool_calls.calls[tc_index].id, id, LLM_TOOLS_ID_LEN - 1);
                     if (tc_index >= ctx->tool_calls.count) {
                        ctx->tool_calls.count = tc_index + 1;
                     }
                  }
               }

               if (json_object_object_get_ex(tc, "function", &function_obj)) {
                  json_object *name_obj, *args_obj;

                  if (json_object_object_get_ex(function_obj, "name", &name_obj)) {
                     const char *name = json_object_get_string(name_obj);
                     if (name) {
                        strncpy(ctx->tool_calls.calls[tc_index].name, name, LLM_TOOLS_NAME_LEN - 1);
                     }
                  }

                  // Arguments come as deltas, accumulate them
                  // OpenAI sends incremental string fragments, but Gemini sends
                  // complete JSON objects on each chunk.
                  if (json_object_object_get_ex(function_obj, "arguments", &args_obj)) {
                     const char *args_chunk = json_object_get_string(args_obj);
                     if (args_chunk) {
                        size_t cur_len = strlen(ctx->provider.openai.tool_args_buffer[tc_index]);
                        size_t add_len = strlen(args_chunk);

                        // Gemini sends complete JSON on each chunk - replace instead of append
                        // Use explicit provider check rather than heuristic
                        if (ctx->cloud_provider == CLOUD_PROVIDER_GEMINI && cur_len > 0) {
                           // Replace with latest complete JSON
                           if (add_len < LLM_TOOLS_ARGS_LEN - 1) {
                              memcpy(ctx->provider.openai.tool_args_buffer[tc_index], args_chunk,
                                     add_len);
                              ctx->provider.openai.tool_args_buffer[tc_index][add_len] = '\0';
                           } else {
                              ctx->provider.openai.tool_args_overflow[tc_index] = true;
                           }
                        } else if (cur_len + add_len < LLM_TOOLS_ARGS_LEN - 1) {
                           // OpenAI-style: append incremental delta
                           memcpy(ctx->provider.openai.tool_args_buffer[tc_index] + cur_len,
                                  args_chunk, add_len);
                           ctx->provider.openai.tool_args_buffer[tc_index][cur_len + add_len] =
                               '\0';
                        } else {
                           // Buffer full — the rest of this (and later) deltas are dropped.
                           ctx->provider.openai.tool_args_overflow[tc_index] = true;
                        }
                     }
                  }
               }

               // Gemini 3+ models: Capture thought_signature from first tool call
               // Required for follow-up requests when reasoning mode is enabled
               if (ctx->cloud_provider == CLOUD_PROVIDER_GEMINI &&
                   ctx->tool_calls.thought_signature[0] == '\0') {
                  json_object *extra_content, *google_obj, *sig_obj;
                  // Try extra_content.google.thought_signature (OpenAI-compatible format)
                  if (json_object_object_get_ex(tc, "extra_content", &extra_content) &&
                      json_object_object_get_ex(extra_content, "google", &google_obj) &&
                      json_object_object_get_ex(google_obj, "thought_signature", &sig_obj)) {
                     const char *sig = json_object_get_string(sig_obj);
                     if (sig && sig[0] != '\0') {
                        size_t sig_len = strlen(sig);
                        strncpy(ctx->tool_calls.thought_signature, sig,
                                LLM_TOOLS_THOUGHT_SIG_LEN - 1);
                        ctx->tool_calls.thought_signature[LLM_TOOLS_THOUGHT_SIG_LEN - 1] = '\0';
                        if (sig_len >= LLM_TOOLS_THOUGHT_SIG_LEN) {
                           OLOG_WARNING("Gemini thought_signature truncated: %zu -> %d bytes",
                                        sig_len, LLM_TOOLS_THOUGHT_SIG_LEN - 1);
                        } else {
                           OLOG_INFO("Captured Gemini thought_signature (%zu bytes)", sig_len);
                        }
                     }
                  }
                  // Also try direct thought_signature field as fallback
                  else if (json_object_object_get_ex(tc, "thought_signature", &sig_obj)) {
                     const char *sig = json_object_get_string(sig_obj);
                     if (sig && sig[0] != '\0') {
                        size_t sig_len = strlen(sig);
                        strncpy(ctx->tool_calls.thought_signature, sig,
                                LLM_TOOLS_THOUGHT_SIG_LEN - 1);
                        ctx->tool_calls.thought_signature[LLM_TOOLS_THOUGHT_SIG_LEN - 1] = '\0';
                        if (sig_len >= LLM_TOOLS_THOUGHT_SIG_LEN) {
                           OLOG_WARNING("Gemini thought_signature truncated: %zu -> %d bytes",
                                        sig_len, LLM_TOOLS_THOUGHT_SIG_LEN - 1);
                        } else {
                           OLOG_INFO("Captured Gemini thought_signature (%zu bytes)", sig_len);
                        }
                     }
                  }
               }
            }
         }
      }

      // Check for finish_reason
      json_object *finish_reason;
      if (json_object_object_get_ex(first_choice, "finish_reason", &finish_reason)) {
         if (!json_object_is_type(finish_reason, json_type_null)) {
            const char *reason = json_object_get_string(finish_reason);
            if (reason) {
               strncpy(ctx->finish_reason, reason, sizeof(ctx->finish_reason) - 1);
               OLOG_INFO("Stream finish_reason: %s", reason);
            }
            ctx->stream_complete = 1;

            /* If reasoning never transitioned to a text response — e.g. the model
             * reasoned and then went straight to tool_calls (no visible text), so the
             * reasoning→response transition above never ran — close the thinking display
             * now.  Otherwise the WebUI reasoning indicator hangs open ("stuck thinking")
             * until the next iteration. */
            if (ctx->thinking_active && !ctx->inside_think_tag) {
               ctx->thinking_active = 0;
               if (has_ws_session) {
                  webui_send_thinking_end(ws_session, ctx->thinking_size > 0);
               }
            }

            // Finalize tool call arguments
            if (ctx->has_tool_calls) {
               for (int i = 0; i < ctx->tool_calls.count; i++) {
                  strncpy(ctx->tool_calls.calls[i].arguments,
                          ctx->provider.openai.tool_args_buffer[i], LLM_TOOLS_ARGS_LEN - 1);
                  ctx->tool_calls.calls[i].arguments[LLM_TOOLS_ARGS_LEN - 1] = '\0';
                  ctx->tool_calls.calls[i].args_truncated =
                      ctx->provider.openai.tool_args_overflow[i];
               }
               OLOG_INFO("Stream completed with %d tool call(s)", ctx->tool_calls.count);
            }
         }
      }
   }

   // Parse real-time timings from llama.cpp (when timings_per_token: true)
   // This provides per-chunk metrics: predicted_n, predicted_per_second, etc.
   json_object *timings_obj;
   if (json_object_object_get_ex(chunk, "timings", &timings_obj)) {
      json_object *val;

      if (json_object_object_get_ex(timings_obj, "predicted_n", &val)) {
         ctx->tokens_generated = json_object_get_int(val);
      }
      if (json_object_object_get_ex(timings_obj, "predicted_per_second", &val)) {
         float rate = (float)json_object_get_double(val);
         // Sanity check: ignore unrealistic values (> 1000 tok/s)
         // First few chunks often have bogus values due to near-zero elapsed time
         if (rate > 0 && rate < 1000.0f && ctx->tokens_generated >= 3) {
            ctx->tokens_per_second = rate;
         }
      }
      if (json_object_object_get_ex(timings_obj, "prompt_n", &val)) {
         ctx->realtime_prompt_tokens = json_object_get_int(val);
      }
      if (json_object_object_get_ex(timings_obj, "cache_n", &val)) {
         ctx->realtime_cached_tokens = json_object_get_int(val);
      }

      // Send real-time metrics to WebUI if we have meaningful data
      // Only send once we have a valid, stable tokens_per_second value
      if (ctx->tokens_per_second > 0 && ctx->tokens_generated >= 3) {
         if (has_ws_session) {
            // Calculate context usage percentage (rough estimate from prompt tokens)
            int context_pct = 0;  // Will be properly calculated elsewhere
            webui_send_metrics_update(ws_session, "thinking", 0, ctx->tokens_per_second,
                                      context_pct);
         }
      }
   }

   // Check for usage stats (sent in final chunk when stream_options.include_usage is true)
   json_object *usage_obj;
   if (json_object_object_get_ex(chunk, "usage", &usage_obj)) {
      json_object *prompt_tokens_obj, *completion_tokens_obj;
      int input_tokens = 0, output_tokens = 0, cached_tokens = 0;

      if (json_object_object_get_ex(usage_obj, "prompt_tokens", &prompt_tokens_obj)) {
         input_tokens = json_object_get_int(prompt_tokens_obj);
      }
      if (json_object_object_get_ex(usage_obj, "completion_tokens", &completion_tokens_obj)) {
         output_tokens = json_object_get_int(completion_tokens_obj);
      }

      // Check for cached tokens in prompt_tokens_details.  Both OpenAI
      // and Gemini-2.5+ surface implicit caching through this field
      // (Gemini routes through our /v1beta/openai shim).  Provider
      // label can't be derived from the streaming context (cloud_provider
      // is CLOUD_PROVIDER_OPENAI for both), so check the active session's
      // model string — "gemini-*" → Gemini, otherwise OpenAI.  Falls
      // back to "OpenAI" when no session context is available.
      //
      // Gemini caching footnote (investigated 2026-05-28): if cached_tokens
      // stays at 0 across many turns on Gemini, that is upstream behavior,
      // not a parsing bug.  Three documented Google issues compound on the
      // OpenAI-compat shim path DAWN uses:
      //   1. Tools defined → caching blocked.  Server returns
      //      "CachedContent cannot be used with GenerateContent request
      //      setting system_instruction, tools or tool_config" (per
      //      vercel/ai#11513).  DAWN sends 27 tools per request.
      //   2. gemini-3-flash-preview has a 9K-17K-token dead zone where
      //      cached_content_token_count drops to 0 even on cacheable
      //      prefixes (googleapis/python-genai#2064, filed 2026-02-16,
      //      P2, unresolved as of 2026-05-28).  DAWN's typical prompt
      //      lands in this zone.
      //   3. Gemini 3.x does not pay out cost savings even when it
      //      caches — discount is 2.5-family-only per Google's blog.
      // Gemini 2.5 Flash engages caching occasionally (observed 8793
      // cached_tokens on a tool-loop iter 1) but inconsistently.  Full
      // analysis: docs/TODO.md §1.  Claude + OpenAI are the reliable
      // cache surfaces; Gemini caching is "sometimes-bonus."
      json_object *prompt_details;
      if (json_object_object_get_ex(usage_obj, "prompt_tokens_details", &prompt_details)) {
         json_object *cached_obj;
         if (json_object_object_get_ex(prompt_details, "cached_tokens", &cached_obj)) {
            cached_tokens = json_object_get_int(cached_obj);
            if (cached_tokens > 0) {
               /* llm_config.model is protected by llm_config_mutex (per session
                * lock-ordering rules in session_manager.h); copy the
                * provider-prefix bytes under the lock then release before the
                * OLOG_INFO so we never hold a leaf lock across I/O. */
               char model_prefix[8] = { 0 };
               if (ws_session != NULL) {
                  pthread_mutex_lock(&ws_session->llm_config_mutex);
                  strncpy(model_prefix, ws_session->llm_config.model, sizeof(model_prefix) - 1);
                  pthread_mutex_unlock(&ws_session->llm_config_mutex);
               }
               const char *provider_label = (strncmp(model_prefix, "gemini-", 7) == 0) ? "Gemini"
                                                                                       : "OpenAI";
               OLOG_INFO("%s cache hit: %d tokens cached", provider_label, cached_tokens);
            }
         }
      }

      // Check for reasoning tokens in completion_tokens_details (OpenAI o-series)
      json_object *completion_details;
      if (json_object_object_get_ex(usage_obj, "completion_tokens_details", &completion_details)) {
         json_object *reasoning_obj;
         if (json_object_object_get_ex(completion_details, "reasoning_tokens", &reasoning_obj)) {
            ctx->reasoning_tokens = json_object_get_int(reasoning_obj);
            if (ctx->reasoning_tokens > 0) {
               OLOG_INFO("OpenAI reasoning tokens: %d", ctx->reasoning_tokens);
            }
         }
      }

      // Only record if we have actual token counts (final chunk has non-zero values)
      if (input_tokens > 0 || output_tokens > 0) {
         llm_type_t type = (ctx->llm_type == LLM_LOCAL) ? LLM_LOCAL : LLM_CLOUD;
         metrics_record_llm_tokens(type, ctx->cloud_provider, input_tokens, output_tokens,
                                   cached_tokens);

         // Update context usage tracking with actual session ID
         uint32_t session_id = ws_session ? ws_session->session_id : 0;
         llm_context_update_usage(session_id, input_tokens, output_tokens, cached_tokens);

         // Calculate accurate token rate from actual output tokens and streaming duration
         // This is more accurate than counting chunks for providers like Gemini
         if (output_tokens > 0 && has_ws_session) {
            struct timeval now;
            gettimeofday(&now, NULL);
            double duration_ms = (now.tv_sec - ctx->stream_start_time.tv_sec) * 1000.0 +
                                 (now.tv_usec - ctx->stream_start_time.tv_usec) / 1000.0;
            /* Require minimum 100ms to avoid artificially high rates from timing jitter */
            if (duration_ms > 100) {
               float accurate_rate = (float)output_tokens * 1000.0f / (float)duration_ms;
               ctx->tokens_per_second = accurate_rate;
               ctx->tokens_generated = output_tokens;
               OLOG_INFO("Stream rate: %.1f tok/s (%d tokens in %.0fms)", accurate_rate,
                         output_tokens, duration_ms);
               // Send accurate final metrics to WebUI
               // TTFT was already sent during streaming, just update token rate
               webui_send_metrics_update(ws_session, "thinking", 0, accurate_rate, -1);
            }
         }

         OLOG_INFO("Stream usage: %d input, %d output, %d cached tokens", input_tokens,
                   output_tokens, cached_tokens);
      }
   }

   json_object_put(chunk);
}

/**
 * @brief Parse Claude streaming event
 *
 * Format depends on event type:
 * - message_start: {"type":"message_start",...}
 * - content_block_delta: {"type":"content_block_delta","delta":{"text":"..."}}
 * - message_stop: {"type":"message_stop"}
 */
static void parse_claude_event(llm_stream_context_t *ctx, const char *event_data) {
   json_object *event = json_tokener_parse(event_data);
   if (!event) {
      OLOG_WARNING("Failed to parse Claude event JSON");
      return;
   }

   // Get event type
   json_object *type_obj;
   if (!json_object_object_get_ex(event, "type", &type_obj)) {
      json_object_put(event);
      return;
   }

   const char *type = json_object_get_string(type_obj);

   // Cache session lookup for WebUI notifications (avoids repeated lookups)
   session_t *ws_session = session_get_command_context();
   int has_ws_session = (ws_session && ws_session->type == SESSION_TYPE_WEBUI);

   if (strcmp(type, "message_start") == 0) {
      ctx->provider.claude.message_started = 1;

      // Extract input_tokens + cache stats from message.usage.
      // cache_creation_input_tokens / cache_read_input_tokens are present
      // only when cache_control:ephemeral was honored on this request.
      // We log them with the same wording as the non-streaming path
      // (llm_claude.c) so operators get a consistent grep target.
      json_object *message_obj, *usage_obj, *tok_obj;
      ctx->provider.claude.cache_creation_input_tokens = 0;
      ctx->provider.claude.cache_read_input_tokens = 0;
      if (json_object_object_get_ex(event, "message", &message_obj)) {
         if (json_object_object_get_ex(message_obj, "usage", &usage_obj)) {
            if (json_object_object_get_ex(usage_obj, "input_tokens", &tok_obj)) {
               ctx->provider.claude.input_tokens = json_object_get_int(tok_obj);
            }
            if (json_object_object_get_ex(usage_obj, "cache_creation_input_tokens", &tok_obj)) {
               int v = json_object_get_int(tok_obj);
               ctx->provider.claude.cache_creation_input_tokens = v;
               if (v > 0)
                  OLOG_INFO("Claude cache created: %d tokens", v);
            }
            if (json_object_object_get_ex(usage_obj, "cache_read_input_tokens", &tok_obj)) {
               int v = json_object_get_int(tok_obj);
               ctx->provider.claude.cache_read_input_tokens = v;
               if (v > 0)
                  OLOG_INFO("Claude cache hit: %d tokens (90%% cost savings!)", v);
            }
         }
      }
   } else if (strcmp(type, "content_block_start") == 0) {
      ctx->provider.claude.content_block_active = 1;

      // Check if this is a tool_use block
      json_object *content_block, *block_type_obj;
      if (json_object_object_get_ex(event, "content_block", &content_block)) {
         if (json_object_object_get_ex(content_block, "type", &block_type_obj)) {
            const char *block_type = json_object_get_string(block_type_obj);

            if (strcmp(block_type, "thinking") == 0) {
               // Extended thinking block
               ctx->provider.claude.thinking_block_active = 1;
               ctx->thinking_active = 1;
               ctx->has_thinking = 1;
               OLOG_INFO("Claude: Starting thinking block (extended thinking)");

               // Send thinking_start to WebUI
               if (has_ws_session) {
                  webui_send_thinking_start(ws_session, "claude");
               }
            } else if (strcmp(block_type, "tool_use") == 0) {
               // Extract tool ID and name
               json_object *id_obj, *name_obj, *index_obj;
               ctx->provider.claude.tool_block_active = 1;
               ctx->provider.claude.tool_args[0] = '\0';
               ctx->provider.claude.tool_args_len = 0;
               ctx->provider.claude.tool_args_overflow = false;

               if (json_object_object_get_ex(event, "index", &index_obj)) {
                  ctx->provider.claude.tool_index = json_object_get_int(index_obj);
               }

               if (json_object_object_get_ex(content_block, "id", &id_obj)) {
                  strncpy(ctx->provider.claude.tool_id, json_object_get_string(id_obj),
                          LLM_TOOLS_ID_LEN - 1);
                  ctx->provider.claude.tool_id[LLM_TOOLS_ID_LEN - 1] = '\0';
               }

               if (json_object_object_get_ex(content_block, "name", &name_obj)) {
                  strncpy(ctx->provider.claude.tool_name, json_object_get_string(name_obj),
                          LLM_TOOLS_NAME_LEN - 1);
                  ctx->provider.claude.tool_name[LLM_TOOLS_NAME_LEN - 1] = '\0';
               }

               OLOG_INFO("Claude: Starting tool_use block: %s (id=%s)",
                         ctx->provider.claude.tool_name, ctx->provider.claude.tool_id);

               /* Visual progress: notify frontend when render_visual starts */
               if (strcmp(ctx->provider.claude.tool_name, "render_visual") == 0) {
                  ctx->provider.claude.visual_progress_active = 1;
                  if (has_ws_session) {
                     char json_buf[384];
                     snprintf(json_buf, sizeof(json_buf),
                              "{\"type\":\"visual_progress_start\",\"payload\":"
                              "{\"tool_id\":\"%s\",\"conversation_id\":%lld}}",
                              ctx->provider.claude.tool_id,
                              (long long)ws_session->stream_conversation_id);
                     webui_send_session_json(ws_session, json_buf);
                  }
               }
            }
         }
      }
   } else if (strcmp(type, "content_block_delta") == 0) {
      // Extract delta
      json_object *delta, *delta_type_obj, *text_obj;

      if (json_object_object_get_ex(event, "delta", &delta)) {
         // Check delta type
         if (json_object_object_get_ex(delta, "type", &delta_type_obj)) {
            const char *delta_type = json_object_get_string(delta_type_obj);

            if (strcmp(delta_type, "thinking_delta") == 0 &&
                ctx->provider.claude.thinking_block_active) {
               // Extended thinking content
               json_object *thinking_obj;
               if (json_object_object_get_ex(delta, "thinking", &thinking_obj)) {
                  const char *thinking_text = json_object_get_string(thinking_obj);
                  if (thinking_text && thinking_text[0] != '\0') {
                     // Call chunk callback with thinking type if available
                     if (ctx->chunk_callback) {
                        ctx->chunk_callback(LLM_CHUNK_THINKING, thinking_text,
                                            ctx->chunk_callback_userdata);
                     }

                     // Send to WebUI for real-time display
                     if (has_ws_session) {
                        webui_send_thinking_delta(ws_session, thinking_text);
                     }

                     // Accumulate thinking content
                     append_to_thinking(ctx, thinking_text);
                  }
               }
            } else if (strcmp(delta_type, "text_delta") == 0) {
               // Extract text
               if (json_object_object_get_ex(delta, "text", &text_obj)) {
                  const char *text = json_object_get_string(text_obj);
                  if (text && text[0] != '\0') {
                     // Record TTFT on first token
                     record_ttft_if_first_token(ctx);

                     // Call user callback with chunk
                     ctx->callback(text, ctx->callback_userdata);

                     // Call chunk callback with text type if available
                     if (ctx->chunk_callback) {
                        ctx->chunk_callback(LLM_CHUNK_TEXT, text, ctx->chunk_callback_userdata);
                     }

                     // Append to accumulated response
                     append_to_accumulated(ctx, text);
                  }
               }
            } else if (strcmp(delta_type, "input_json_delta") == 0 &&
                       ctx->provider.claude.tool_block_active) {
               // Accumulate partial_json for tool arguments
               json_object *partial_json_obj;
               if (json_object_object_get_ex(delta, "partial_json", &partial_json_obj)) {
                  const char *partial = json_object_get_string(partial_json_obj);
                  if (partial) {
                     size_t partial_len = strlen(partial);
                     if (ctx->provider.claude.tool_args_len + partial_len <
                         LLM_TOOLS_ARGS_LEN - 1) {
                        memcpy(ctx->provider.claude.tool_args + ctx->provider.claude.tool_args_len,
                               partial, partial_len);
                        ctx->provider.claude.tool_args_len += partial_len;
                        ctx->provider.claude.tool_args[ctx->provider.claude.tool_args_len] = '\0';
                     } else {
                        /* Buffer full — this and later deltas are dropped. */
                        ctx->provider.claude.tool_args_overflow = true;
                     }
                  }
               }

               /* Note: visual_progress_update was removed — Claude streams tool args
                * in bursts (not smoothly), so byte count and title extraction arrive
                * too late to provide meaningful progress. The placeholder with pulsing
                * dot + client-side timer is the best UX for this API constraint. */
            } else if (strcmp(delta_type, "signature_delta") == 0 &&
                       ctx->provider.claude.thinking_block_active) {
               // Accumulate signature for thinking block (required when sending back to Claude)
               json_object *signature_obj;
               if (json_object_object_get_ex(delta, "signature", &signature_obj)) {
                  const char *sig_chunk = json_object_get_string(signature_obj);
                  if (sig_chunk) {
                     size_t sig_len = strlen(sig_chunk);
                     size_t needed = ctx->provider.claude.thinking_signature_len + sig_len + 1;

                     /* Grow buffer if needed */
                     if (needed > ctx->provider.claude.thinking_signature_cap) {
                        size_t new_cap = ctx->provider.claude.thinking_signature_cap;
                        if (new_cap == 0)
                           new_cap = LLM_THINKING_SIGNATURE_INITIAL;
                        while (new_cap < needed)
                           new_cap *= 2;
                        char *new_buf = realloc(ctx->provider.claude.thinking_signature, new_cap);
                        if (!new_buf) {
                           OLOG_ERROR("Claude: Failed to allocate signature buffer (%zu bytes)",
                                      new_cap);
                        } else {
                           ctx->provider.claude.thinking_signature = new_buf;
                           ctx->provider.claude.thinking_signature_cap = new_cap;
                        }
                     }

                     if (ctx->provider.claude.thinking_signature &&
                         needed <= ctx->provider.claude.thinking_signature_cap) {
                        memcpy(ctx->provider.claude.thinking_signature +
                                   ctx->provider.claude.thinking_signature_len,
                               sig_chunk, sig_len);
                        ctx->provider.claude.thinking_signature_len += sig_len;
                        ctx->provider.claude
                            .thinking_signature[ctx->provider.claude.thinking_signature_len] = '\0';
                     }
                  }
               }
            }
         }
      }
   } else if (strcmp(type, "content_block_stop") == 0) {
      // If we were in a thinking block, finalize it
      if (ctx->provider.claude.thinking_block_active) {
         ctx->provider.claude.thinking_block_active = 0;
         ctx->thinking_active = 0;
         OLOG_INFO("Claude: Thinking block completed (%zu bytes, signature %zu bytes)",
                   ctx->thinking_size, ctx->provider.claude.thinking_signature_len);

         // Send thinking_end to WebUI
         if (has_ws_session) {
            webui_send_thinking_end(ws_session, ctx->thinking_size > 0);
         }
      }

      // Reset visual progress tracking
      if (ctx->provider.claude.visual_progress_active) {
         ctx->provider.claude.visual_progress_active = 0;
      }

      // If we were in a tool_use block, finalize it
      if (ctx->provider.claude.tool_block_active) {
         // Add to tool_calls list
         if (ctx->tool_calls.count < LLM_TOOLS_MAX_PARALLEL_CALLS) {
            int idx = ctx->tool_calls.count;
            strncpy(ctx->tool_calls.calls[idx].id, ctx->provider.claude.tool_id,
                    LLM_TOOLS_ID_LEN - 1);
            strncpy(ctx->tool_calls.calls[idx].name, ctx->provider.claude.tool_name,
                    LLM_TOOLS_NAME_LEN - 1);
            strncpy(ctx->tool_calls.calls[idx].arguments, ctx->provider.claude.tool_args,
                    LLM_TOOLS_ARGS_LEN - 1);
            ctx->tool_calls.calls[idx].arguments[LLM_TOOLS_ARGS_LEN - 1] = '\0';
            ctx->tool_calls.calls[idx].args_truncated = ctx->provider.claude.tool_args_overflow;
            ctx->tool_calls.count++;
            ctx->has_tool_calls = 1;

            OLOG_INFO("Claude: Completed tool_use: %s with args: %.100s%s",
                      ctx->provider.claude.tool_name, ctx->provider.claude.tool_args,
                      strlen(ctx->provider.claude.tool_args) > 100 ? "..." : "");
         }

         ctx->provider.claude.tool_block_active = 0;
         ctx->provider.claude.tool_id[0] = '\0';
         ctx->provider.claude.tool_name[0] = '\0';
         ctx->provider.claude.tool_args[0] = '\0';
         ctx->provider.claude.tool_args_len = 0;
      }

      ctx->provider.claude.content_block_active = 0;
   } else if (strcmp(type, "message_delta") == 0) {
      // Extract stop_reason
      json_object *delta_obj, *stop_reason_obj;
      if (json_object_object_get_ex(event, "delta", &delta_obj)) {
         if (json_object_object_get_ex(delta_obj, "stop_reason", &stop_reason_obj)) {
            const char *stop_reason = json_object_get_string(stop_reason_obj);
            if (stop_reason) {
               strncpy(ctx->finish_reason, stop_reason, sizeof(ctx->finish_reason) - 1);
               ctx->finish_reason[sizeof(ctx->finish_reason) - 1] = '\0';
               OLOG_INFO("Claude stream stop_reason: %s", stop_reason);
            }
         }
      }

      // Extract output_tokens from usage
      json_object *usage_obj, *output_tokens_obj;
      if (json_object_object_get_ex(event, "usage", &usage_obj)) {
         if (json_object_object_get_ex(usage_obj, "output_tokens", &output_tokens_obj)) {
            int output_tokens = json_object_get_int(output_tokens_obj);
            // Cached tokens were captured in message_start; pass through
            // so metrics + per-session usage tracking reflect the cache
            // discount (90% off on read tokens).
            int cached = ctx->provider.claude.cache_read_input_tokens;
            metrics_record_llm_tokens(LLM_CLOUD, CLOUD_PROVIDER_CLAUDE,
                                      ctx->provider.claude.input_tokens, output_tokens, cached);

            // Update context usage tracking with actual session ID
            uint32_t session_id = ws_session ? ws_session->session_id : 0;
            llm_context_update_usage(session_id, ctx->provider.claude.input_tokens, output_tokens,
                                     cached);

            // Calculate accurate token rate from actual output tokens
            if (output_tokens > 0 && has_ws_session) {
               struct timeval now;
               gettimeofday(&now, NULL);
               double duration_ms = (now.tv_sec - ctx->stream_start_time.tv_sec) * 1000.0 +
                                    (now.tv_usec - ctx->stream_start_time.tv_usec) / 1000.0;
               /* Require minimum 100ms to avoid artificially high rates from timing jitter */
               if (duration_ms > 100) {
                  float accurate_rate = (float)output_tokens * 1000.0f / (float)duration_ms;
                  ctx->tokens_per_second = accurate_rate;
                  ctx->tokens_generated = output_tokens;
                  OLOG_INFO("Claude rate: %.1f tok/s (%d tokens in %.0fms)", accurate_rate,
                            output_tokens, duration_ms);
                  webui_send_metrics_update(ws_session, "thinking", 0, accurate_rate, -1);
               }
            }

            OLOG_INFO("Claude usage: %d input, %d output tokens", ctx->provider.claude.input_tokens,
                      output_tokens);
         }
      }
   } else if (strcmp(type, "message_stop") == 0) {
      ctx->stream_complete = 1;

      // Log completion with tool calls if any
      if (ctx->has_tool_calls) {
         OLOG_INFO("Claude stream completed with %d tool call(s)", ctx->tool_calls.count);
      }
   }
   // Note: ping and error events are ignored

   json_object_put(event);
}

llm_stream_context_t *llm_stream_create(llm_type_t llm_type,
                                        cloud_provider_t cloud_provider,
                                        text_chunk_callback callback,
                                        void *userdata) {
   if (!callback) {
      OLOG_ERROR("LLM stream callback cannot be NULL");
      return NULL;
   }

   llm_stream_context_t *ctx = calloc(1, sizeof(llm_stream_context_t));
   if (!ctx) {
      OLOG_ERROR("Failed to allocate LLM stream context");
      return NULL;
   }

   ctx->accumulated_response = malloc(DEFAULT_ACCUMULATED_CAPACITY);
   if (!ctx->accumulated_response) {
      OLOG_ERROR("Failed to allocate accumulated response buffer");
      free(ctx);
      return NULL;
   }

   ctx->accumulated_response[0] = '\0';
   ctx->accumulated_capacity = DEFAULT_ACCUMULATED_CAPACITY;
   ctx->accumulated_size = 0;

   ctx->llm_type = llm_type;
   ctx->cloud_provider = cloud_provider;
   ctx->callback = callback;
   ctx->callback_userdata = userdata;
   ctx->stream_complete = 0;

   /* Provider-specific state is zero-initialized by calloc */

   // Initialize TTFT tracking
   gettimeofday(&ctx->stream_start_time, NULL);
   ctx->first_token_received = 0;

   return ctx;
}

void llm_stream_free(llm_stream_context_t *ctx) {
   if (!ctx) {
      return;
   }

   free(ctx->accumulated_response);
   free(ctx->accumulated_thinking);
   /* thinking_signature is a heap pointer that lives ONLY in the Claude arm of
    * the `provider` union; it's allocated solely by the Claude SSE parser.  For
    * OpenAI/OpenRouter/Gemini/local streams the active union member is
    * openai.tool_args_buffer, whose bytes alias this pointer — a response with
    * >=2 parallel tool calls writes arg JSON into tool_args_buffer[1], which
    * overlaps thinking_signature, so freeing it unconditionally hands free() a
    * pointer made of JSON text → SIGSEGV.  Only free it for actual Claude
    * streams (matches the parse-routing condition in llm_stream_handle_event). */
   if (ctx->llm_type != LLM_LOCAL && ctx->cloud_provider == CLOUD_PROVIDER_CLAUDE) {
      free(ctx->provider.claude.thinking_signature);
   }
   free(ctx);
}

void llm_stream_handle_event(llm_stream_context_t *ctx, const char *event_data) {
   if (!ctx || !event_data) {
      return;
   }

   // Route to provider-specific parser
   // Local LLM (llama.cpp) uses OpenAI-compatible format
   // Gemini and OpenRouter also use OpenAI-compatible format
   // Cloud can be OpenAI, Gemini, OpenRouter, or Claude
   if (ctx->llm_type == LLM_LOCAL || ctx->cloud_provider == CLOUD_PROVIDER_OPENAI ||
       ctx->cloud_provider == CLOUD_PROVIDER_GEMINI ||
       ctx->cloud_provider == CLOUD_PROVIDER_OPENROUTER) {
      parse_openai_chunk(ctx, event_data);
   } else if (ctx->cloud_provider == CLOUD_PROVIDER_CLAUDE) {
      parse_claude_event(ctx, event_data);
   }
}

char *llm_stream_get_response(llm_stream_context_t *ctx) {
   if (!ctx || !ctx->accumulated_response || ctx->accumulated_size == 0) {
      /* Fallback: if we have thinking content but no response AND no tool calls,
       * use thinking as response. This handles LOCAL models like Qwen3.5 that put
       * all output in reasoning_content — there the thinking IS the answer.
       *
       * Gated to LLM_LOCAL: cloud reasoning models (Claude/OpenAI/Gemini via
       * their separate thinking channel) keep reasoning PRIVATE, so substituting
       * it for an empty response leaks raw chain-of-thought to the user. For
       * those, an empty response returns NULL (same as the no-thinking case).
       *
       * Don't apply when tool calls are present — an empty response with tool_use
       * is normal (the LLM chose to call tools instead of responding with text). */
      if (ctx && ctx->llm_type == LLM_LOCAL && ctx->accumulated_thinking &&
          ctx->thinking_size > 0 && !ctx->has_tool_calls) {
         OLOG_WARNING("LLM: Empty response but has thinking content (%zu bytes), using as response",
                      ctx->thinking_size);
         return strdup(ctx->accumulated_thinking);
      }
      return NULL;
   }

   return strdup(ctx->accumulated_response);
}

int llm_stream_is_complete(llm_stream_context_t *ctx) {
   if (!ctx) {
      return 0;
   }

   return ctx->stream_complete;
}

int llm_stream_has_tool_calls(llm_stream_context_t *ctx) {
   if (!ctx) {
      return 0;
   }

   return ctx->has_tool_calls && ctx->tool_calls.count > 0;
}

const tool_call_list_t *llm_stream_get_tool_calls(llm_stream_context_t *ctx) {
   if (!ctx || !ctx->has_tool_calls || ctx->tool_calls.count == 0) {
      return NULL;
   }

   return &ctx->tool_calls;
}

llm_stream_context_t *llm_stream_create_extended(llm_type_t llm_type,
                                                 cloud_provider_t cloud_provider,
                                                 text_chunk_callback callback,
                                                 llm_chunk_callback chunk_callback,
                                                 void *userdata) {
   // Create base context
   llm_stream_context_t *ctx = llm_stream_create(llm_type, cloud_provider, callback, userdata);
   if (!ctx) {
      return NULL;
   }

   // Set extended callback for thinking support
   ctx->chunk_callback = chunk_callback;
   ctx->chunk_callback_userdata = userdata;

   return ctx;
}

int llm_stream_has_thinking(llm_stream_context_t *ctx) {
   if (!ctx) {
      return 0;
   }

   return ctx->has_thinking && ctx->thinking_size > 0;
}

char *llm_stream_get_thinking(llm_stream_context_t *ctx) {
   if (!ctx || !ctx->accumulated_thinking || ctx->thinking_size == 0) {
      return NULL;
   }

   return strdup(ctx->accumulated_thinking);
}

char *llm_stream_get_thinking_signature(llm_stream_context_t *ctx) {
   if (!ctx || ctx->provider.claude.thinking_signature_len == 0) {
      return NULL;
   }

   return strdup(ctx->provider.claude.thinking_signature);
}

const char *llm_stream_get_response_ref(llm_stream_context_t *ctx) {
   if (!ctx || !ctx->accumulated_response) {
      return NULL;
   }

   return ctx->accumulated_response;
}

const char *llm_stream_get_thinking_ref(llm_stream_context_t *ctx) {
   if (!ctx || !ctx->accumulated_thinking || ctx->thinking_size == 0) {
      return NULL;
   }

   return ctx->accumulated_thinking;
}

const char *llm_stream_get_thinking_signature_ref(llm_stream_context_t *ctx) {
   if (!ctx || ctx->provider.claude.thinking_signature_len == 0) {
      return NULL;
   }

   return ctx->provider.claude.thinking_signature;
}
