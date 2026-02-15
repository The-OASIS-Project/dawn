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

   LOG_INFO("LLM TTFT: %.1f ms", ttft_ms);
   metrics_record_llm_ttft(ttft_ms);
}

/**
 * @brief Append text to accumulated response buffer
 */
static int append_to_accumulated(llm_stream_context_t *ctx, const char *text) {
   if (!text || !*text) {
      return 1;
   }

   size_t text_len = strlen(text);
   size_t needed = ctx->accumulated_size + text_len + 1;

   // Prevent runaway memory allocation from excessively long LLM responses
   if (needed > MAX_ACCUMULATED_SIZE) {
      LOG_ERROR("Accumulated response size limit exceeded: %zu bytes, maximum %zu bytes (%.1f MB)",
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
         LOG_ERROR("Failed to reallocate accumulated response buffer");
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
         LOG_ERROR("Failed to allocate thinking buffer");
         return 0;
      }
      ctx->accumulated_thinking[0] = '\0';
      ctx->thinking_capacity = DEFAULT_THINKING_CAPACITY;
      ctx->thinking_size = 0;
   }

   size_t needed = ctx->thinking_size + text_len + 1;

   // Prevent runaway memory allocation
   if (needed > MAX_THINKING_SIZE) {
      LOG_WARNING("Thinking content size limit exceeded: %zu bytes", needed);
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
         LOG_ERROR("Failed to reallocate thinking buffer");
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
      LOG_WARNING("Failed to parse OpenAI chunk JSON");
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
               // Mark thinking as active and send thinking_start on first chunk
               if (!ctx->thinking_active) {
                  ctx->thinking_active = 1;
                  ctx->has_thinking = 1;
                  LOG_INFO("LLM: Reasoning content detected (llama.cpp)");

                  // Send thinking_start to WebUI
                  if (has_ws_session) {
                     webui_send_thinking_start(ws_session, "local");
                  }
               }

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

         // Check for text content
         if (json_object_object_get_ex(delta, "content", &content)) {
            const char *text = json_object_get_string(content);
            if (text && text[0] != '\0') {
               // If we were in thinking mode, we've transitioned to text
               if (ctx->thinking_active) {
                  ctx->thinking_active = 0;
                  LOG_INFO("LLM: Transitioned from reasoning to response");

                  // Send thinking_end to WebUI
                  if (has_ws_session) {
                     webui_send_thinking_end(ws_session, ctx->thinking_size > 0);
                  }
               }

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
                           }
                        } else if (cur_len + add_len < LLM_TOOLS_ARGS_LEN - 1) {
                           // OpenAI-style: append incremental delta
                           memcpy(ctx->provider.openai.tool_args_buffer[tc_index] + cur_len,
                                  args_chunk, add_len);
                           ctx->provider.openai.tool_args_buffer[tc_index][cur_len + add_len] =
                               '\0';
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
                           LOG_WARNING("Gemini thought_signature truncated: %zu -> %d bytes",
                                       sig_len, LLM_TOOLS_THOUGHT_SIG_LEN - 1);
                        } else {
                           LOG_INFO("Captured Gemini thought_signature (%zu bytes)", sig_len);
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
                           LOG_WARNING("Gemini thought_signature truncated: %zu -> %d bytes",
                                       sig_len, LLM_TOOLS_THOUGHT_SIG_LEN - 1);
                        } else {
                           LOG_INFO("Captured Gemini thought_signature (%zu bytes)", sig_len);
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
               LOG_INFO("Stream finish_reason: %s", reason);
            }
            ctx->stream_complete = 1;

            // Finalize tool call arguments
            if (ctx->has_tool_calls) {
               for (int i = 0; i < ctx->tool_calls.count; i++) {
                  strncpy(ctx->tool_calls.calls[i].arguments,
                          ctx->provider.openai.tool_args_buffer[i], LLM_TOOLS_ARGS_LEN - 1);
               }
               LOG_INFO("Stream completed with %d tool call(s)", ctx->tool_calls.count);
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

      // Check for cached tokens in prompt_tokens_details
      json_object *prompt_details;
      if (json_object_object_get_ex(usage_obj, "prompt_tokens_details", &prompt_details)) {
         json_object *cached_obj;
         if (json_object_object_get_ex(prompt_details, "cached_tokens", &cached_obj)) {
            cached_tokens = json_object_get_int(cached_obj);
         }
      }

      // Check for reasoning tokens in completion_tokens_details (OpenAI o-series)
      json_object *completion_details;
      if (json_object_object_get_ex(usage_obj, "completion_tokens_details", &completion_details)) {
         json_object *reasoning_obj;
         if (json_object_object_get_ex(completion_details, "reasoning_tokens", &reasoning_obj)) {
            ctx->reasoning_tokens = json_object_get_int(reasoning_obj);
            if (ctx->reasoning_tokens > 0) {
               LOG_INFO("OpenAI reasoning tokens: %d", ctx->reasoning_tokens);
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
               LOG_INFO("Stream rate: %.1f tok/s (%d tokens in %.0fms)", accurate_rate,
                        output_tokens, duration_ms);
               // Send accurate final metrics to WebUI
               // TTFT was already sent during streaming, just update token rate
               webui_send_metrics_update(ws_session, "thinking", 0, accurate_rate, -1);
            }
         }

         LOG_INFO("Stream usage: %d input, %d output, %d cached tokens", input_tokens,
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
      LOG_WARNING("Failed to parse Claude event JSON");
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

      // Extract input_tokens from message.usage
      json_object *message_obj, *usage_obj, *input_tokens_obj;
      if (json_object_object_get_ex(event, "message", &message_obj)) {
         if (json_object_object_get_ex(message_obj, "usage", &usage_obj)) {
            if (json_object_object_get_ex(usage_obj, "input_tokens", &input_tokens_obj)) {
               ctx->provider.claude.input_tokens = json_object_get_int(input_tokens_obj);
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
               LOG_INFO("Claude: Starting thinking block (extended thinking)");

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

               LOG_INFO("Claude: Starting tool_use block: %s (id=%s)",
                        ctx->provider.claude.tool_name, ctx->provider.claude.tool_id);
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
                     }
                  }
               }
            } else if (strcmp(delta_type, "signature_delta") == 0 &&
                       ctx->provider.claude.thinking_block_active) {
               // Accumulate signature for thinking block (required when sending back to Claude)
               json_object *signature_obj;
               if (json_object_object_get_ex(delta, "signature", &signature_obj)) {
                  const char *sig_chunk = json_object_get_string(signature_obj);
                  if (sig_chunk) {
                     size_t sig_len = strlen(sig_chunk);
                     if (ctx->provider.claude.thinking_signature_len + sig_len <
                         sizeof(ctx->provider.claude.thinking_signature) - 1) {
                        memcpy(ctx->provider.claude.thinking_signature +
                                   ctx->provider.claude.thinking_signature_len,
                               sig_chunk, sig_len);
                        ctx->provider.claude.thinking_signature_len += sig_len;
                        ctx->provider.claude
                            .thinking_signature[ctx->provider.claude.thinking_signature_len] = '\0';
                     } else {
                        LOG_WARNING(
                            "Claude: Thinking signature truncated (buffer full at %zu bytes)",
                            ctx->provider.claude.thinking_signature_len);
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
         LOG_INFO("Claude: Thinking block completed (%zu bytes, signature %zu bytes)",
                  ctx->thinking_size, ctx->provider.claude.thinking_signature_len);

         // Send thinking_end to WebUI
         if (has_ws_session) {
            webui_send_thinking_end(ws_session, ctx->thinking_size > 0);
         }
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
            ctx->tool_calls.count++;
            ctx->has_tool_calls = 1;

            LOG_INFO("Claude: Completed tool_use: %s with args: %.100s%s",
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
               LOG_INFO("Claude stream stop_reason: %s", stop_reason);
            }
         }
      }

      // Extract output_tokens from usage
      json_object *usage_obj, *output_tokens_obj;
      if (json_object_object_get_ex(event, "usage", &usage_obj)) {
         if (json_object_object_get_ex(usage_obj, "output_tokens", &output_tokens_obj)) {
            int output_tokens = json_object_get_int(output_tokens_obj);
            // Record token metrics (input was captured in message_start)
            metrics_record_llm_tokens(LLM_CLOUD, CLOUD_PROVIDER_CLAUDE,
                                      ctx->provider.claude.input_tokens, output_tokens, 0);

            // Update context usage tracking with actual session ID
            uint32_t session_id = ws_session ? ws_session->session_id : 0;
            llm_context_update_usage(session_id, ctx->provider.claude.input_tokens, output_tokens,
                                     0);

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
                  LOG_INFO("Claude rate: %.1f tok/s (%d tokens in %.0fms)", accurate_rate,
                           output_tokens, duration_ms);
                  webui_send_metrics_update(ws_session, "thinking", 0, accurate_rate, -1);
               }
            }

            LOG_INFO("Claude usage: %d input, %d output tokens", ctx->provider.claude.input_tokens,
                     output_tokens);
         }
      }
   } else if (strcmp(type, "message_stop") == 0) {
      ctx->stream_complete = 1;

      // Log completion with tool calls if any
      if (ctx->has_tool_calls) {
         LOG_INFO("Claude stream completed with %d tool call(s)", ctx->tool_calls.count);
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
      LOG_ERROR("LLM stream callback cannot be NULL");
      return NULL;
   }

   llm_stream_context_t *ctx = calloc(1, sizeof(llm_stream_context_t));
   if (!ctx) {
      LOG_ERROR("Failed to allocate LLM stream context");
      return NULL;
   }

   ctx->accumulated_response = malloc(DEFAULT_ACCUMULATED_CAPACITY);
   if (!ctx->accumulated_response) {
      LOG_ERROR("Failed to allocate accumulated response buffer");
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
   free(ctx);
}

void llm_stream_handle_event(llm_stream_context_t *ctx, const char *event_data) {
   if (!ctx || !event_data) {
      return;
   }

   // Route to provider-specific parser
   // Local LLM (llama.cpp) uses OpenAI-compatible format
   // Gemini also uses OpenAI-compatible format
   // Cloud can be OpenAI, Gemini, or Claude
   if (ctx->llm_type == LLM_LOCAL || ctx->cloud_provider == CLOUD_PROVIDER_OPENAI ||
       ctx->cloud_provider == CLOUD_PROVIDER_GEMINI) {
      parse_openai_chunk(ctx, event_data);
   } else if (ctx->cloud_provider == CLOUD_PROVIDER_CLAUDE) {
      parse_claude_event(ctx, event_data);
   }
}

char *llm_stream_get_response(llm_stream_context_t *ctx) {
   if (!ctx || !ctx->accumulated_response) {
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
