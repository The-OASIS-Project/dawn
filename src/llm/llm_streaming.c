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

#include "llm/llm_context.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "ui/metrics.h"

#define DEFAULT_ACCUMULATED_CAPACITY 8192
#define MAX_ACCUMULATED_SIZE (10 * 1024 * 1024)  // 10MB hard limit for LLM responses

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
   if (!text || strlen(text) == 0) {
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

   // Extract choices[0].delta.content or tool_calls
   json_object *choices, *first_choice, *delta, *content;

   if (json_object_object_get_ex(chunk, "choices", &choices) &&
       json_object_get_type(choices) == json_type_array && json_object_array_length(choices) > 0) {
      first_choice = json_object_array_get_idx(choices, 0);

      if (json_object_object_get_ex(first_choice, "delta", &delta)) {
         // Check for text content
         if (json_object_object_get_ex(delta, "content", &content)) {
            const char *text = json_object_get_string(content);
            if (text && strlen(text) > 0) {
               // Record TTFT on first token
               record_ttft_if_first_token(ctx);

               // Call user callback with chunk
               ctx->callback(text, ctx->callback_userdata);

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
                  if (json_object_object_get_ex(function_obj, "arguments", &args_obj)) {
                     const char *args_chunk = json_object_get_string(args_obj);
                     if (args_chunk) {
                        size_t cur_len = strlen(ctx->tool_args_buffer[tc_index]);
                        size_t add_len = strlen(args_chunk);
                        if (cur_len + add_len < LLM_TOOLS_ARGS_LEN - 1) {
                           // Use memcpy instead of strcat to avoid O(n^2) scanning
                           memcpy(ctx->tool_args_buffer[tc_index] + cur_len, args_chunk, add_len);
                           ctx->tool_args_buffer[tc_index][cur_len + add_len] = '\0';
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
                  strncpy(ctx->tool_calls.calls[i].arguments, ctx->tool_args_buffer[i],
                          LLM_TOOLS_ARGS_LEN - 1);
               }
               LOG_INFO("Stream completed with %d tool call(s)", ctx->tool_calls.count);
            }
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

      // Only record if we have actual token counts (final chunk has non-zero values)
      if (input_tokens > 0 || output_tokens > 0) {
         llm_type_t type = (ctx->llm_type == LLM_LOCAL) ? LLM_LOCAL : LLM_CLOUD;
         metrics_record_llm_tokens(type, ctx->cloud_provider, input_tokens, output_tokens,
                                   cached_tokens);

         // Update context usage tracking (session 0 = local session)
         llm_context_update_usage(0, input_tokens, output_tokens, cached_tokens);

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

   if (strcmp(type, "message_start") == 0) {
      ctx->message_started = 1;

      // Extract input_tokens from message.usage
      json_object *message_obj, *usage_obj, *input_tokens_obj;
      if (json_object_object_get_ex(event, "message", &message_obj)) {
         if (json_object_object_get_ex(message_obj, "usage", &usage_obj)) {
            if (json_object_object_get_ex(usage_obj, "input_tokens", &input_tokens_obj)) {
               ctx->claude_input_tokens = json_object_get_int(input_tokens_obj);
            }
         }
      }
   } else if (strcmp(type, "content_block_start") == 0) {
      ctx->content_block_active = 1;

      // Check if this is a tool_use block
      json_object *content_block, *block_type_obj;
      if (json_object_object_get_ex(event, "content_block", &content_block)) {
         if (json_object_object_get_ex(content_block, "type", &block_type_obj)) {
            const char *block_type = json_object_get_string(block_type_obj);

            if (strcmp(block_type, "tool_use") == 0) {
               // Extract tool ID and name
               json_object *id_obj, *name_obj, *index_obj;
               ctx->claude_tool_block_active = 1;
               ctx->claude_tool_args[0] = '\0';
               ctx->claude_tool_args_len = 0;

               if (json_object_object_get_ex(event, "index", &index_obj)) {
                  ctx->claude_tool_index = json_object_get_int(index_obj);
               }

               if (json_object_object_get_ex(content_block, "id", &id_obj)) {
                  strncpy(ctx->claude_tool_id, json_object_get_string(id_obj),
                          LLM_TOOLS_ID_LEN - 1);
                  ctx->claude_tool_id[LLM_TOOLS_ID_LEN - 1] = '\0';
               }

               if (json_object_object_get_ex(content_block, "name", &name_obj)) {
                  strncpy(ctx->claude_tool_name, json_object_get_string(name_obj),
                          LLM_TOOLS_NAME_LEN - 1);
                  ctx->claude_tool_name[LLM_TOOLS_NAME_LEN - 1] = '\0';
               }

               LOG_INFO("Claude: Starting tool_use block: %s (id=%s)", ctx->claude_tool_name,
                        ctx->claude_tool_id);
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

            if (strcmp(delta_type, "text_delta") == 0) {
               // Extract text
               if (json_object_object_get_ex(delta, "text", &text_obj)) {
                  const char *text = json_object_get_string(text_obj);
                  if (text && strlen(text) > 0) {
                     // Record TTFT on first token
                     record_ttft_if_first_token(ctx);

                     // Call user callback with chunk
                     ctx->callback(text, ctx->callback_userdata);

                     // Append to accumulated response
                     append_to_accumulated(ctx, text);
                  }
               }
            } else if (strcmp(delta_type, "input_json_delta") == 0 &&
                       ctx->claude_tool_block_active) {
               // Accumulate partial_json for tool arguments
               json_object *partial_json_obj;
               if (json_object_object_get_ex(delta, "partial_json", &partial_json_obj)) {
                  const char *partial = json_object_get_string(partial_json_obj);
                  if (partial) {
                     size_t partial_len = strlen(partial);
                     if (ctx->claude_tool_args_len + partial_len < LLM_TOOLS_ARGS_LEN - 1) {
                        memcpy(ctx->claude_tool_args + ctx->claude_tool_args_len, partial,
                               partial_len);
                        ctx->claude_tool_args_len += partial_len;
                        ctx->claude_tool_args[ctx->claude_tool_args_len] = '\0';
                     }
                  }
               }
            }
            // Note: thinking_delta is ignored
         }
      }
   } else if (strcmp(type, "content_block_stop") == 0) {
      // If we were in a tool_use block, finalize it
      if (ctx->claude_tool_block_active) {
         // Add to tool_calls list
         if (ctx->tool_calls.count < LLM_TOOLS_MAX_PARALLEL_CALLS) {
            int idx = ctx->tool_calls.count;
            strncpy(ctx->tool_calls.calls[idx].id, ctx->claude_tool_id, LLM_TOOLS_ID_LEN - 1);
            strncpy(ctx->tool_calls.calls[idx].name, ctx->claude_tool_name, LLM_TOOLS_NAME_LEN - 1);
            strncpy(ctx->tool_calls.calls[idx].arguments, ctx->claude_tool_args,
                    LLM_TOOLS_ARGS_LEN - 1);
            ctx->tool_calls.count++;
            ctx->has_tool_calls = 1;

            LOG_INFO("Claude: Completed tool_use: %s with args: %.100s%s", ctx->claude_tool_name,
                     ctx->claude_tool_args, strlen(ctx->claude_tool_args) > 100 ? "..." : "");
         }

         ctx->claude_tool_block_active = 0;
         ctx->claude_tool_id[0] = '\0';
         ctx->claude_tool_name[0] = '\0';
         ctx->claude_tool_args[0] = '\0';
         ctx->claude_tool_args_len = 0;
      }

      ctx->content_block_active = 0;
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
            metrics_record_llm_tokens(LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, ctx->claude_input_tokens,
                                      output_tokens, 0);

            // Update context usage tracking (session 0 = local session)
            llm_context_update_usage(0, ctx->claude_input_tokens, output_tokens, 0);

            LOG_INFO("Claude usage: %d input, %d output tokens", ctx->claude_input_tokens,
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
   ctx->message_started = 0;
   ctx->content_block_active = 0;
   ctx->stream_complete = 0;

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
   free(ctx);
}

void llm_stream_handle_event(llm_stream_context_t *ctx, const char *event_data) {
   if (!ctx || !event_data) {
      return;
   }

   // Route to provider-specific parser
   // Local LLM (llama.cpp) uses OpenAI-compatible format
   // Cloud can be OpenAI or Claude
   if (ctx->llm_type == LLM_LOCAL || ctx->cloud_provider == CLOUD_PROVIDER_OPENAI) {
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
