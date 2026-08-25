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
 * OpenAI /v1/responses API implementation. Used for the gpt-5.4 family where
 * /v1/chat/completions rejects reasoning_effort + function tools. Operates in
 * stateless mode (Mode B) — echoes prior reasoning items in the input array
 * rather than relying on server state via previous_response_id, so resumed
 * conversations and operator-driven history mutation continue to work.
 *
 * Cross-turn reasoning items live in-memory only on the assistant message JSON
 * under _provider_state.openai_responses.reasoning_items; they are not persisted
 * to the auth_db (consistent with how tool_calls are not persisted today).
 */

#include "llm/llm_openai_responses.h"

#include <curl/curl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config/dawn_config.h"
#include "core/curl_buffer.h"
#include "core/session_manager.h"
#include "llm/llm_context.h"
#include "llm/llm_interface.h"
#include "llm/llm_openai_internal.h"
#include "llm/llm_openai_responses_input.h"
#include "llm/llm_streaming.h"
#include "llm/llm_tools.h"
#include "llm/sse_parser.h"
#include "logging.h"
#include "ui/metrics.h"
#ifdef ENABLE_WEBUI
#include "webui/webui_server.h"
#endif

/* External CURL progress callback for interrupt support (defined in dawn.c) */
extern int llm_curl_progress_callback(void *clientp,
                                      curl_off_t dltotal,
                                      curl_off_t dlnow,
                                      curl_off_t ultotal,
                                      curl_off_t ulnow);

/* External cancellation check */
extern int llm_is_interrupt_requested(void);
extern int llm_get_effective_timeout_ms(void);

/* Cap on raw HTTP body capture (used only for error message extraction on
 * non-200). 200 responses don't actually need this — the SSE parser consumes
 * the stream directly — so the cap is sized for the much smaller error envelope.
 * Most OpenAI error JSON is well under 1 KB; 8 KB leaves headroom for verbose
 * validation arrays without burning memory on every healthy stream. */
#define RESPONSES_RAW_BUFFER_MAX (8 * 1024)

/* Endpoint path. Internal — not part of the public header. */
#define OPENAI_RESPONSES_ENDPOINT "/v1/responses"

/* =============================================================================
 * Streaming context (wraps llm_stream_context_t with Responses-specific state)
 * ============================================================================= */

/**
 * @brief Per-stream state for the Responses event handler.
 *
 * Function call assembly: each function_call item gets a slot in
 * stream_ctx->tool_calls[] and a parallel argument buffer here. Items are
 * matched by their `item_id` (fc_...) since deltas reference item_id, not
 * call_id. We also track an active_index for the most-recent in-progress
 * function_call as a fast path for the common single-tool-at-a-time case.
 */
typedef struct {
   sse_parser_t *sse_parser;
   llm_stream_context_t *stream_ctx;

   /* Raw response capture for error message extraction on non-200.  Cap
    * at RESPONSES_RAW_BUFFER_MAX (8 KB) — matches Claude's streaming
    * context pattern (see llm_claude.c). */
   curl_buffer_t raw_response;

   /* Per-function-call state. item_ids parallel stream_ctx->tool_calls[].
    * fc_args_len tracks the running argument-buffer length so per-delta append
    * is O(1) instead of O(n) (avoids strlen on each delta). */
   char fc_item_ids[LLM_TOOLS_MAX_PARALLEL_CALLS][64];
   size_t fc_args_len[LLM_TOOLS_MAX_PARALLEL_CALLS];
   int fc_count;
   int active_fc_index; /* -1 when no function_call item open */

   /* Reasoning items collected for cross-turn round-trip. JSON array, growable.
    * `reasoning_items_bytes` tracks the aggregate size of stored encrypted_content
    * payloads so we can cap and prevent payload-bloat DoS from a misbehaving upstream. */
   struct json_object *reasoning_items;
   size_t reasoning_items_bytes;

   /* Final response.id captured at response.completed */
   char response_id[64];

   /* Captured usage from response.completed */
   int reasoning_tokens;
} responses_stream_ctx_t;

/* =============================================================================
 * History conversion: DAWN's chat-completions-shaped history → Responses input
 * ============================================================================= */

/**
 * @brief Convert chat-completions tool definitions to the Responses flat schema.
 *
 * Chat completions: [{type:"function", function:{name,description,parameters}}]
 * Responses:        [{type:"function", name, description, parameters, strict?}]
 *
 * Returns a new array (caller json_object_put). Returns NULL if input is NULL.
 */
static struct json_object *flatten_tools_for_responses(struct json_object *cc_tools) {
   if (!cc_tools || json_object_get_type(cc_tools) != json_type_array)
      return NULL;

   struct json_object *out = json_object_new_array();
   int n = json_object_array_length(cc_tools);
   for (int i = 0; i < n; i++) {
      struct json_object *tool = json_object_array_get_idx(cc_tools, i);
      struct json_object *type_obj, *fn_obj;
      if (!json_object_object_get_ex(tool, "type", &type_obj))
         continue;
      if (strcmp(json_object_get_string(type_obj), "function") != 0)
         continue;
      if (!json_object_object_get_ex(tool, "function", &fn_obj))
         continue;

      struct json_object *flat = json_object_new_object();
      json_object_object_add(flat, "type", json_object_new_string("function"));

      struct json_object *name_obj, *desc_obj, *params_obj;
      if (json_object_object_get_ex(fn_obj, "name", &name_obj)) {
         json_object_object_add(flat, "name",
                                json_object_new_string(json_object_get_string(name_obj)));
      }
      if (json_object_object_get_ex(fn_obj, "description", &desc_obj)) {
         json_object_object_add(flat, "description",
                                json_object_new_string(json_object_get_string(desc_obj)));
      }
      if (json_object_object_get_ex(fn_obj, "parameters", &params_obj)) {
         json_object_object_add(flat, "parameters", json_object_get(params_obj));
      }
      json_object_array_add(out, flat);
   }
   return out;
}

/* =============================================================================
 * Reasoning effort selection
 * ============================================================================= */

/**
 * @brief Validate that an effort string is one of the values gpt-5.4+ accepts.
 *
 * Per OpenAI: gpt-5.4 family accepts {none, low, medium, high, xhigh}. "minimal"
 * is gpt-5-base only; mapped to "low" before reaching this gate. Anything else
 * (including operator-supplied junk in g_config.llm.thinking.reasoning_effort)
 * is normalized to "medium" rather than passed verbatim into the JSON.
 */
static bool effort_is_allowed_for_responses(const char *effort) {
   if (!effort)
      return false;
   return strcmp(effort, "none") == 0 || strcmp(effort, "low") == 0 ||
          strcmp(effort, "medium") == 0 || strcmp(effort, "high") == 0 ||
          strcmp(effort, "xhigh") == 0;
}

static const char *select_reasoning_effort(const char *model_name) {
   (void)model_name; /* Reserved for future per-model effort floors. */
   const char *thinking_mode = llm_get_current_thinking_mode();

   if (strcmp(thinking_mode, "disabled") == 0) {
      /* gpt-5.4 (and other Responses-routed models) accept "none" as the lowest. */
      return "none";
   }

   const char *candidate = NULL;
   if (strcmp(thinking_mode, "enabled") == 0 || strcmp(thinking_mode, "auto") == 0) {
      candidate = g_config.llm.thinking.reasoning_effort;
   } else if (strcmp(thinking_mode, "minimal") == 0) {
      candidate = "low";
   } else {
      candidate = thinking_mode;
   }

   if (candidate && strcmp(candidate, "minimal") == 0) {
      candidate = "low";
   }
   if (!effort_is_allowed_for_responses(candidate)) {
      OLOG_WARNING("Responses: rejecting unknown reasoning effort '%s', using 'medium'",
                   candidate ? candidate : "(null)");
      return "medium";
   }
   return candidate;
}

/* =============================================================================
 * Request building
 * ============================================================================= */

static bool is_current_session_remote_local(void) {
   session_t *s = session_get_command_context();
   if (!s)
      return false;
   return s->type != SESSION_TYPE_LOCAL;
}

/**
 * @brief Build the full Responses request JSON payload.
 *
 * @param prior_response_id Optional previous_response_id for Mode A (NULL → Mode B).
 *                          Reserved for future session-scoped optimization; current
 *                          callers always pass NULL.
 */
static struct json_object *build_responses_request(struct json_object *history,
                                                   const char *input_text,
                                                   const char **vision_images,
                                                   const size_t *vision_image_sizes,
                                                   int vision_image_count,
                                                   const char *model_name,
                                                   int iteration,
                                                   const char *prior_response_id) {
   struct json_object *root = json_object_new_object();

   json_object_object_add(root, "model", json_object_new_string(model_name));
   json_object_object_add(root, "stream", json_object_new_boolean(1));
   /* Stateless: don't persist on the server. Mode B echoes reasoning items in input. */
   json_object_object_add(root, "store", json_object_new_boolean(0));

   /* Always request encrypted_content so we can round-trip reasoning items */
   struct json_object *include_arr = json_object_new_array();
   json_object_array_add(include_arr, json_object_new_string("reasoning.encrypted_content"));
   json_object_object_add(root, "include", include_arr);

   /* Reasoning config */
   if (!llm_tools_suppressed()) {
      struct json_object *reasoning = json_object_new_object();
      json_object_object_add(reasoning, "effort",
                             json_object_new_string(select_reasoning_effort(model_name)));
      json_object_object_add(reasoning, "summary", json_object_new_string("auto"));
      json_object_object_add(root, "reasoning", reasoning);
   }

   /* System → instructions: ONLY the stable segment, kept byte-stable across turns so
    * [instructions][tools][history] forms a cacheable prefix. The per-turn volatile
    * block is repositioned into `input` (below). The stable persona is always present
    * on the live path, so the field presence is itself stable. */
   char *instructions = llm_responses_extract_stable_instructions(history);
   if (instructions) {
      json_object_object_add(root, "instructions", json_object_new_string(instructions));
      free(instructions);
   }
   char *volatile_ctx = llm_responses_extract_volatile_context(history);
   int leading_run = llm_responses_count_leading_system_run(history);

   /* Input items (or previous_response_id for Mode A) */
   if (prior_response_id && *prior_response_id) {
      json_object_object_add(root, "previous_response_id",
                             json_object_new_string(prior_response_id));
      /* Only attach the new user message — server has the rest. NOTE: Mode A is
       * currently unused (callers pass NULL). If it is ever revived, the volatile
       * TURN CONTEXT block must be repositioned here too (prepend it before the new
       * user message) or per-turn memory/time context vanishes from these turns. */
      struct json_object *input = json_object_new_array();
      if (input_text && *input_text) {
         struct json_object *item = json_object_new_object();
         json_object_object_add(item, "type", json_object_new_string("message"));
         json_object_object_add(item, "role", json_object_new_string("user"));
         struct json_object *content_array = json_object_new_array();
         struct json_object *part = json_object_new_object();
         json_object_object_add(part, "type", json_object_new_string("input_text"));
         json_object_object_add(part, "text", json_object_new_string(input_text));
         json_object_array_add(content_array, part);
         llm_responses_append_vision_parts(content_array, vision_images, vision_image_sizes,
                                           vision_image_count);
         json_object_object_add(item, "content", content_array);
         json_object_array_add(input, item);
      }
      json_object_object_add(root, "input", input);
      free(volatile_ctx);
   } else {
      struct json_object *input = llm_responses_build_input(history, input_text, vision_images,
                                                            vision_image_sizes, vision_image_count,
                                                            volatile_ctx, leading_run);
      free(volatile_ctx);
      if (!input) {
         json_object_put(root);
         return NULL;
      }
      json_object_object_add(root, "input", input);
   }

   /* max_output_tokens */
   json_object_object_add(root, "max_output_tokens", json_object_new_int(g_config.llm.max_tokens));

   /* Tools */
   if (llm_tools_enabled(NULL) && iteration < LLM_TOOLS_MAX_ITERATIONS) {
      bool is_remote = is_current_session_remote_local();
      struct json_object *cc_tools = llm_tools_get_openai_format_filtered(is_remote);
      if (cc_tools) {
         struct json_object *flat = flatten_tools_for_responses(cc_tools);
         json_object_put(cc_tools);
         if (flat && json_object_array_length(flat) > 0) {
            json_object_object_add(root, "tools", flat);
            json_object_object_add(root, "tool_choice", json_object_new_string("auto"));
            json_object_object_add(root, "parallel_tool_calls", json_object_new_boolean(1));
         } else if (flat) {
            json_object_put(flat);
         }
      }
   }

   return root;
}

/* =============================================================================
 * SSE event handler
 * ============================================================================= */

/**
 * @brief Locate or create a function_call slot keyed by item_id.
 *
 * Returns the index in stream_ctx->tool_calls.calls[], or -1 on overflow.
 */
static int responses_fc_slot(responses_stream_ctx_t *rctx, const char *item_id) {
   if (!item_id)
      return -1;
   /* Reject item_ids that won't fit in the slot — silently truncating could let
    * two distinct ids collide on their first 63 bytes, which would route
    * arguments from one call into another's buffer. OpenAI ids are currently
    * ~40 chars; reject anything that approaches the buffer limit. */
   size_t id_len = strlen(item_id);
   if (id_len >= sizeof(rctx->fc_item_ids[0])) {
      OLOG_WARNING("Responses: rejecting function_call item_id of %zu bytes (>%zu)", id_len,
                   sizeof(rctx->fc_item_ids[0]) - 1);
      return -1;
   }
   for (int i = 0; i < rctx->fc_count; i++) {
      if (strcmp(rctx->fc_item_ids[i], item_id) == 0)
         return i;
   }
   if (rctx->fc_count >= LLM_TOOLS_MAX_PARALLEL_CALLS) {
      OLOG_WARNING("Responses: function_call overflow (max %d)", LLM_TOOLS_MAX_PARALLEL_CALLS);
      return -1;
   }
   int idx = rctx->fc_count++;
   memcpy(rctx->fc_item_ids[idx], item_id, id_len);
   rctx->fc_item_ids[idx][id_len] = '\0';
   if (idx >= rctx->stream_ctx->tool_calls.count) {
      rctx->stream_ctx->tool_calls.count = idx + 1;
   }
   return idx;
}

/**
 * @brief Append a string delta to a function_call's argument buffer.
 *
 * O(1) per call — uses the cached fc_args_len[idx] instead of strlen'ing
 * the accumulated buffer (which is O(n²) over a multi-KB JSON-args stream
 * delivered in many small deltas).
 */
static void responses_fc_append_args(responses_stream_ctx_t *rctx, int idx, const char *delta) {
   if (idx < 0 || !delta)
      return;
   tool_call_t *call = &rctx->stream_ctx->tool_calls.calls[idx];
   size_t cur = rctx->fc_args_len[idx];
   size_t add = strlen(delta);
   if (cur + add + 1 > sizeof(call->arguments)) {
      OLOG_WARNING("Responses: function_call args truncated (overflow %zu+%zu vs %zu)", cur, add,
                   sizeof(call->arguments));
      add = sizeof(call->arguments) - cur - 1;
   }
   memcpy(call->arguments + cur, delta, add);
   call->arguments[cur + add] = '\0';
   rctx->fc_args_len[idx] = cur + add;
}

/**
 * @brief Whitelist of SSE event types this handler actually consumes.
 *
 * Cheap pre-parse filter: the Responses stream emits ~20 distinct event types
 * and we only care about 9 of them. Skipping the parse for the rest (especially
 * high-frequency ones like response.in_progress, response.content_part.added/done,
 * response.output_text.done, plus the built-in tool status events) avoids a
 * json_tokener_parse + json_object_put on every heartbeat — material savings on
 * reasoning-heavy turns that emit 50–200 events/sec.
 */
static bool responses_event_is_handled(const char *event_type) {
   return strcmp(event_type, "response.output_text.delta") == 0 ||
          strcmp(event_type, "response.reasoning_summary_text.delta") == 0 ||
          strcmp(event_type, "response.output_item.added") == 0 ||
          strcmp(event_type, "response.function_call_arguments.delta") == 0 ||
          strcmp(event_type, "response.function_call_arguments.done") == 0 ||
          strcmp(event_type, "response.output_item.done") == 0 ||
          strcmp(event_type, "response.completed") == 0 ||
          strcmp(event_type, "response.failed") == 0 || strcmp(event_type, "error") == 0;
}

static void responses_handle_event(const char *event_type, const char *event_data, void *userdata) {
   responses_stream_ctx_t *rctx = (responses_stream_ctx_t *)userdata;
   if (!event_type || !event_data || !rctx)
      return;

   /* Skip ignored event types BEFORE the JSON parse — those are the bulk of
    * the stream during reasoning turns. */
   if (!responses_event_is_handled(event_type))
      return;

   struct json_object *root = json_tokener_parse(event_data);
   if (!root) {
      return;
   }

#ifdef ENABLE_WEBUI
   /* Cache once per event — used by multiple branches below. */
   session_t *ws = session_get_command_context();
   bool has_ws_session = ws && ws->type == SESSION_TYPE_WEBUI;
#endif

   if (strcmp(event_type, "response.output_text.delta") == 0) {
      struct json_object *d;
      if (json_object_object_get_ex(root, "delta", &d)) {
         const char *txt = json_object_get_string(d);
         if (txt && *txt) {
            /* Forward to text chunk callback (TTS sink) and accumulate */
            if (rctx->stream_ctx->callback) {
               rctx->stream_ctx->callback(txt, rctx->stream_ctx->callback_userdata);
            }
            if (rctx->stream_ctx->chunk_callback) {
               rctx->stream_ctx->chunk_callback(LLM_CHUNK_TEXT, txt,
                                                rctx->stream_ctx->chunk_callback_userdata);
            }
            /* Best-effort accumulate via re-feed of generic stream handler is unsafe
             * (different event shape). Instead, the single-shot reader uses
             * llm_stream_get_response_ref which reads accumulated_response. We append
             * directly using the same growth pattern by emitting a synthetic delta. */
            llm_stream_append_text(rctx->stream_ctx, txt);
         }
      }
   } else if (strcmp(event_type, "response.reasoning_summary_text.delta") == 0) {
      struct json_object *d;
      if (json_object_object_get_ex(root, "delta", &d)) {
         const char *txt = json_object_get_string(d);
         if (txt && *txt) {
            llm_stream_append_thinking(rctx->stream_ctx, txt);
            rctx->stream_ctx->has_thinking = 1;
            if (rctx->stream_ctx->chunk_callback) {
               rctx->stream_ctx->chunk_callback(LLM_CHUNK_THINKING, txt,
                                                rctx->stream_ctx->chunk_callback_userdata);
            }
#ifdef ENABLE_WEBUI
            if (has_ws_session) {
               webui_send_thinking_delta(ws, txt);
            }
#endif
         }
      }
   } else if (strcmp(event_type, "response.output_item.added") == 0) {
      struct json_object *item;
      if (json_object_object_get_ex(root, "item", &item)) {
         struct json_object *type_obj;
         if (json_object_object_get_ex(item, "type", &type_obj)) {
            const char *t = json_object_get_string(type_obj);
            if (strcmp(t, "function_call") == 0) {
               struct json_object *id_obj, *call_id_obj, *name_obj;
               const char *item_id = NULL;
               if (json_object_object_get_ex(item, "id", &id_obj)) {
                  item_id = json_object_get_string(id_obj);
               }
               int idx = responses_fc_slot(rctx, item_id);
               if (idx >= 0) {
                  rctx->active_fc_index = idx;
                  tool_call_t *call = &rctx->stream_ctx->tool_calls.calls[idx];
                  if (json_object_object_get_ex(item, "call_id", &call_id_obj)) {
                     strncpy(call->id, json_object_get_string(call_id_obj), sizeof(call->id) - 1);
                     call->id[sizeof(call->id) - 1] = '\0';
                  }
                  if (json_object_object_get_ex(item, "name", &name_obj)) {
                     strncpy(call->name, json_object_get_string(name_obj), sizeof(call->name) - 1);
                     call->name[sizeof(call->name) - 1] = '\0';
                  }
                  call->arguments[0] = '\0';
                  rctx->fc_args_len[idx] = 0;
                  rctx->stream_ctx->has_tool_calls = 1;
               }
            } else if (strcmp(t, "reasoning") == 0) {
               /* Reasoning items are captured on .done (encrypted_content
                * isn't populated until the item closes). No-op here. */
            }
         }
      }
#ifdef ENABLE_WEBUI
      if (has_ws_session) {
         struct json_object *type_obj, *item_inner;
         if (json_object_object_get_ex(root, "item", &item_inner) &&
             json_object_object_get_ex(item_inner, "type", &type_obj) &&
             strcmp(json_object_get_string(type_obj), "reasoning") == 0) {
            webui_send_thinking_start(ws, "openai");
         }
      }
#endif
   } else if (strcmp(event_type, "response.function_call_arguments.delta") == 0) {
      struct json_object *id_obj, *delta_obj;
      const char *item_id = NULL;
      if (json_object_object_get_ex(root, "item_id", &id_obj)) {
         item_id = json_object_get_string(id_obj);
      }
      int idx = (item_id ? responses_fc_slot(rctx, item_id) : rctx->active_fc_index);
      if (json_object_object_get_ex(root, "delta", &delta_obj)) {
         responses_fc_append_args(rctx, idx, json_object_get_string(delta_obj));
      }
   } else if (strcmp(event_type, "response.function_call_arguments.done") == 0) {
      /* Authoritative final arguments — overwrite the accumulated buffer */
      struct json_object *id_obj, *args_obj;
      const char *item_id = NULL;
      if (json_object_object_get_ex(root, "item_id", &id_obj)) {
         item_id = json_object_get_string(id_obj);
      }
      int idx = (item_id ? responses_fc_slot(rctx, item_id) : rctx->active_fc_index);
      if (idx >= 0 && json_object_object_get_ex(root, "arguments", &args_obj)) {
         tool_call_t *call = &rctx->stream_ctx->tool_calls.calls[idx];
         const char *args = json_object_get_string(args_obj);
         if (args) {
            strncpy(call->arguments, args, sizeof(call->arguments) - 1);
            call->arguments[sizeof(call->arguments) - 1] = '\0';
            rctx->fc_args_len[idx] = strlen(call->arguments);
         }
      }
   } else if (strcmp(event_type, "response.output_item.done") == 0) {
      struct json_object *item;
      if (json_object_object_get_ex(root, "item", &item)) {
         struct json_object *type_obj;
         if (json_object_object_get_ex(item, "type", &type_obj)) {
            const char *t = json_object_get_string(type_obj);
            if (strcmp(t, "reasoning") == 0) {
               if (!rctx->reasoning_items) {
                  rctx->reasoning_items = json_object_new_array();
               }
               /* Cap aggregate size of round-trip reasoning items. A misbehaving
                * upstream proxy could feed multi-MB blobs that bloat every
                * subsequent request payload until OpenAI rejects with HTTP 413
                * (DoS-by-bloat). Cap is per-turn; oldest-style not necessary
                * since reasoning_items lives only for this single response. */
               static const size_t REASONING_ITEMS_BYTES_CAP = 256 * 1024;
               struct json_object *enc_obj_check;
               size_t incoming_size = 0;
               if (json_object_object_get_ex(item, "encrypted_content", &enc_obj_check)) {
                  const char *e = json_object_get_string(enc_obj_check);
                  if (e)
                     incoming_size = strlen(e);
               }
               if (rctx->reasoning_items_bytes + incoming_size > REASONING_ITEMS_BYTES_CAP) {
                  OLOG_WARNING("Responses: reasoning items aggregate size %zu+%zu would exceed "
                               "%zu cap; dropping further items this turn",
                               rctx->reasoning_items_bytes, incoming_size,
                               REASONING_ITEMS_BYTES_CAP);
               } else {
                  /* Keep id, type, summary, encrypted_content — strip everything
                   * else to minimize wire size on the round-trip request. */
                  struct json_object *trimmed = json_object_new_object();
                  json_object_object_add(trimmed, "type", json_object_new_string("reasoning"));
                  struct json_object *id_obj, *enc_obj, *sum_obj;
                  if (json_object_object_get_ex(item, "id", &id_obj)) {
                     json_object_object_add(trimmed, "id",
                                            json_object_new_string(json_object_get_string(id_obj)));
                  }
                  if (json_object_object_get_ex(item, "encrypted_content", &enc_obj)) {
                     json_object_object_add(trimmed, "encrypted_content",
                                            json_object_new_string(
                                                json_object_get_string(enc_obj)));
                  }
                  if (json_object_object_get_ex(item, "summary", &sum_obj)) {
                     json_object_object_add(trimmed, "summary", json_object_get(sum_obj));
                  }
                  json_object_array_add(rctx->reasoning_items, trimmed);
                  rctx->reasoning_items_bytes += incoming_size;
               }
            } else if (strcmp(t, "function_call") == 0) {
               rctx->active_fc_index = -1;
            }
         }
      }
#ifdef ENABLE_WEBUI
      if (has_ws_session) {
         struct json_object *item_inner, *type_obj;
         if (json_object_object_get_ex(root, "item", &item_inner) &&
             json_object_object_get_ex(item_inner, "type", &type_obj) &&
             strcmp(json_object_get_string(type_obj), "reasoning") == 0) {
            webui_send_thinking_end(ws, rctx->stream_ctx->thinking_size > 0);
         }
      }
#endif
   } else if (strcmp(event_type, "response.completed") == 0) {
      struct json_object *resp;
      if (json_object_object_get_ex(root, "response", &resp)) {
         struct json_object *id_obj, *usage_obj;
         if (json_object_object_get_ex(resp, "id", &id_obj)) {
            const char *id = json_object_get_string(id_obj);
            if (id) {
               size_t id_len = strlen(id);
               if (id_len >= sizeof(rctx->response_id)) {
                  /* Truncation here would silently break a future Mode A
                   * (previous_response_id) follow-up — log so it's noticed. */
                  OLOG_WARNING("Responses: response.id length %zu exceeds buffer %zu, truncating",
                               id_len, sizeof(rctx->response_id) - 1);
               }
               strncpy(rctx->response_id, id, sizeof(rctx->response_id) - 1);
               rctx->response_id[sizeof(rctx->response_id) - 1] = '\0';
            }
         }
         if (json_object_object_get_ex(resp, "usage", &usage_obj)) {
            /* Responses-API usage shape:
             *   { "input_tokens": N,
             *     "input_tokens_details": { "cached_tokens": M },
             *     "output_tokens": N,
             *     "output_tokens_details": { "reasoning_tokens": M },
             *     "total_tokens": N }
             *
             * Up to this commit only reasoning_tokens was extracted — input/
             * output/cached_tokens were silently dropped, so per-session metrics
             * for gpt-5.4* (the only Responses-routed family today) reported
             * zeros across the board and OpenAI cache activity was invisible.
             * Mirror the chat-completions parser at llm_openai_chat_completions.c
             * so the two paths feed metrics identically. */
            int input_tokens = 0;
            int output_tokens = 0;
            int cached_tokens = 0;

            struct json_object *tok_obj;
            if (json_object_object_get_ex(usage_obj, "input_tokens", &tok_obj))
               input_tokens = json_object_get_int(tok_obj);
            if (json_object_object_get_ex(usage_obj, "output_tokens", &tok_obj))
               output_tokens = json_object_get_int(tok_obj);

            struct json_object *in_details;
            if (json_object_object_get_ex(usage_obj, "input_tokens_details", &in_details)) {
               if (json_object_object_get_ex(in_details, "cached_tokens", &tok_obj)) {
                  cached_tokens = json_object_get_int(tok_obj);
                  if (cached_tokens > 0)
                     OLOG_INFO("OpenAI Responses cache hit: %d tokens cached", cached_tokens);
               }
            }

            struct json_object *out_details;
            if (json_object_object_get_ex(usage_obj, "output_tokens_details", &out_details)) {
               struct json_object *r_tokens;
               if (json_object_object_get_ex(out_details, "reasoning_tokens", &r_tokens)) {
                  rctx->reasoning_tokens = json_object_get_int(r_tokens);
                  rctx->stream_ctx->reasoning_tokens = rctx->reasoning_tokens;
               }
            }

            if (input_tokens > 0 || output_tokens > 0) {
               metrics_record_llm_tokens(LLM_CLOUD, CLOUD_PROVIDER_OPENAI, input_tokens,
                                         output_tokens, cached_tokens);
#ifdef ENABLE_WEBUI
               uint32_t session_id = (ws != NULL) ? ws->session_id : 0;
#else
               uint32_t session_id = 0;
#endif
               llm_context_update_usage(session_id, input_tokens, output_tokens, cached_tokens);
               OLOG_INFO("Responses usage: %d input, %d output, %d cached tokens", input_tokens,
                         output_tokens, cached_tokens);
            }
         }
      }
      /* Set finish_reason consistent with chat-completions semantics */
      if (rctx->stream_ctx->has_tool_calls) {
         strncpy(rctx->stream_ctx->finish_reason, "tool_calls",
                 sizeof(rctx->stream_ctx->finish_reason) - 1);
      } else {
         strncpy(rctx->stream_ctx->finish_reason, "stop",
                 sizeof(rctx->stream_ctx->finish_reason) - 1);
      }
      rctx->stream_ctx->stream_complete = 1;
   } else if (strcmp(event_type, "response.failed") == 0 || strcmp(event_type, "error") == 0) {
      struct json_object *err;
      const char *msg = NULL;
      if (json_object_object_get_ex(root, "error", &err)) {
         struct json_object *m;
         if (json_object_object_get_ex(err, "message", &m)) {
            msg = json_object_get_string(m);
         }
      } else {
         struct json_object *resp, *resp_err, *m;
         if (json_object_object_get_ex(root, "response", &resp) &&
             json_object_object_get_ex(resp, "error", &resp_err) &&
             json_object_object_get_ex(resp_err, "message", &m)) {
            msg = json_object_get_string(m);
         }
      }
      OLOG_ERROR("Responses stream %s: %s", event_type, msg ? msg : "(no message)");
      strncpy(rctx->stream_ctx->finish_reason, "error",
              sizeof(rctx->stream_ctx->finish_reason) - 1);
      rctx->stream_ctx->stream_complete = 1;
   }

   json_object_put(root);
}

/* =============================================================================
 * CURL write callback
 * ============================================================================= */

static size_t responses_write_callback(void *contents, size_t size, size_t nmemb, void *userdata) {
   responses_stream_ctx_t *rctx = (responses_stream_ctx_t *)userdata;
   size_t total = size * nmemb;
   const char *data = (const char *)contents;

   /* Capture raw response for error message extraction (cap at
    * RESPONSES_RAW_BUFFER_MAX).  Matches Claude's streaming context
    * pattern (see llm_claude.c). */
   if (rctx->raw_response.size < RESPONSES_RAW_BUFFER_MAX) {
      size_t space = RESPONSES_RAW_BUFFER_MAX - rctx->raw_response.size;
      size_t to_copy = total < space ? total : space;
      curl_buffer_write_callback(contents, 1, to_copy, &rctx->raw_response);
   }

   sse_parser_feed(rctx->sse_parser, data, total);
   return total;
}

/* =============================================================================
 * Public entry point
 * ============================================================================= */

int llm_openai_responses_streaming_single_shot(struct json_object *conversation_history,
                                               const char *input_text,
                                               const char **vision_images,
                                               const size_t *vision_image_sizes,
                                               int vision_image_count,
                                               const char *base_url,
                                               const char *api_key,
                                               const char *model,
                                               llm_openai_text_chunk_callback chunk_callback,
                                               void *callback_userdata,
                                               int iteration,
                                               struct llm_tool_response *result) {
   if (!result) {
      return 1;
   }
   memset(result, 0, sizeof(*result));

   /* Resolve model */
   const char *model_name = model;
   if (!model_name || !*model_name) {
      model_name = llm_get_default_openai_model();
   }
   if (!model_name || !*model_name) {
      OLOG_ERROR("Responses: no model name");
      return 1;
   }

   /* Build request JSON */
   struct json_object *root = build_responses_request(conversation_history, input_text,
                                                      vision_images, vision_image_sizes,
                                                      vision_image_count, model_name, iteration,
                                                      /*prior_response_id=*/NULL);
   if (!root) {
      OLOG_ERROR("Responses: failed to build request");
      return 1;
   }

   const char *payload = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN |
                                                                  JSON_C_TO_STRING_NOSLASHESCAPE);

   OLOG_INFO("OpenAI Responses iter %d: url=%s%s model=%s payload=%zu bytes", iteration, base_url,
             OPENAI_RESPONSES_ENDPOINT, model_name, strlen(payload));

   /* Streaming context */
   responses_stream_ctx_t rctx;
   memset(&rctx, 0, sizeof(rctx));
   rctx.active_fc_index = -1;
   curl_buffer_init_with_max(&rctx.raw_response, RESPONSES_RAW_BUFFER_MAX);

   rctx.stream_ctx = llm_stream_create(LLM_CLOUD, CLOUD_PROVIDER_OPENAI, chunk_callback,
                                       callback_userdata);
   if (!rctx.stream_ctx) {
      OLOG_ERROR("Responses: failed to create stream context");
      json_object_put(root);
      curl_buffer_free(&rctx.raw_response);
      return 1;
   }

   rctx.sse_parser = sse_parser_create(responses_handle_event, &rctx);
   if (!rctx.sse_parser) {
      OLOG_ERROR("Responses: failed to create SSE parser");
      llm_stream_free(rctx.stream_ctx);
      json_object_put(root);
      curl_buffer_free(&rctx.raw_response);
      return 1;
   }

   /* CURL setup with one retry on transient connect failures */
   CURL *curl = NULL;
   struct curl_slist *headers = NULL;
   CURLcode res = CURLE_OK;
   char full_url[2048 + 32];

   const int max_attempts = 2;
   for (int attempt = 0; attempt < max_attempts; attempt++) {
      curl = curl_easy_init();
      if (!curl) {
         OLOG_ERROR("Responses: curl_easy_init failed");
         sse_parser_free(rctx.sse_parser);
         llm_stream_free(rctx.stream_ctx);
         json_object_put(root);
         curl_buffer_free(&rctx.raw_response);
         if (rctx.reasoning_items)
            json_object_put(rctx.reasoning_items);
         return 1;
      }

      headers = llm_openai_build_headers(api_key, base_url);
      headers = curl_slist_append(headers, "Accept: text/event-stream");

      snprintf(full_url, sizeof(full_url), "%s%s", base_url, OPENAI_RESPONSES_ENDPOINT);
      curl_easy_setopt(curl, CURLOPT_URL, full_url);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, responses_write_callback);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rctx);
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, llm_curl_progress_callback);
      curl_easy_setopt(curl, CURLOPT_XFERINFODATA, NULL);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, LLM_CONNECT_TIMEOUT_MS);
      {
         int eff_to = llm_get_effective_timeout_ms();
         long lspt = 60L;
         if (eff_to > 60000) {
            lspt = (long)(eff_to / 1000);
         }
         curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
         curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, lspt);
      }

      res = curl_easy_perform(curl);
      if (res == CURLE_OK)
         break;

      bool retryable = (res == CURLE_COULDNT_CONNECT || res == CURLE_COULDNT_RESOLVE_HOST);
      if (res == CURLE_OPERATION_TIMEDOUT && rctx.raw_response.size == 0) {
         retryable = true;
      }
      if (retryable && attempt < max_attempts - 1) {
         OLOG_WARNING("Responses: CURL connect failed (%s), retrying", curl_easy_strerror(res));
         curl_easy_cleanup(curl);
         curl_slist_free_all(headers);
         curl = NULL;
         headers = NULL;
         curl_buffer_reset(&rctx.raw_response);
         sse_parser_reset(rctx.sse_parser);
         for (int ms = 0; ms < 1000; ms += 100) {
            if (llm_is_interrupt_requested())
               break;
            usleep(100000);
         }
         continue;
      }

      if (res == CURLE_ABORTED_BY_CALLBACK) {
         OLOG_INFO("Responses: transfer interrupted by user");
      } else {
         OLOG_ERROR("Responses: curl_easy_perform failed: %s", curl_easy_strerror(res));
      }
#ifdef ENABLE_WEBUI
      if (res != CURLE_ABORTED_BY_CALLBACK) {
         session_t *s = session_get_command_context();
         if (s && s->type == SESSION_TYPE_WEBUI) {
            const char *err_code = (res == CURLE_OPERATION_TIMEDOUT) ? "LLM_TIMEOUT" : "LLM_ERROR";
            webui_send_error(s, err_code, curl_easy_strerror(res));
         }
      }
#endif
      curl_easy_cleanup(curl);
      curl_slist_free_all(headers);
      sse_parser_free(rctx.sse_parser);
      llm_stream_free(rctx.stream_ctx);
      json_object_put(root);
      curl_buffer_free(&rctx.raw_response);
      if (rctx.reasoning_items)
         json_object_put(rctx.reasoning_items);
      return 1;
   }

   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
   if (http_code != 200) {
      const char *err_msg = llm_openai_parse_error_message(rctx.raw_response.data, http_code);
      OLOG_ERROR("Responses: HTTP %ld: %s", http_code, err_msg);
      if (http_code == 429 || (http_code >= 500 && http_code < 600)) {
         llm_set_last_error(LLM_ERR_TRANSIENT_NETWORK);
      }
#ifdef ENABLE_WEBUI
      session_t *s = session_get_command_context();
      if (s && s->type == SESSION_TYPE_WEBUI) {
         webui_send_error(s, "LLM_ERROR", err_msg);
      }
#endif
      curl_easy_cleanup(curl);
      curl_slist_free_all(headers);
      sse_parser_free(rctx.sse_parser);
      llm_stream_free(rctx.stream_ctx);
      json_object_put(root);
      curl_buffer_free(&rctx.raw_response);
      if (rctx.reasoning_items)
         json_object_put(rctx.reasoning_items);
      return 1;
   }

   curl_easy_cleanup(curl);
   curl_slist_free_all(headers);

   /* Populate result from stream + responses-specific captures */
   if (rctx.stream_ctx->has_tool_calls && rctx.stream_ctx->tool_calls.count > 0) {
      result->has_tool_calls = true;
      memcpy(&result->tool_calls, &rctx.stream_ctx->tool_calls, sizeof(tool_call_list_t));
   }
   result->text = llm_stream_get_response(rctx.stream_ctx);
   result->thinking_content = llm_stream_get_thinking(rctx.stream_ctx);
   result->reasoning_tokens = rctx.reasoning_tokens;
   if (rctx.stream_ctx->finish_reason[0] != '\0') {
      strncpy(result->finish_reason, rctx.stream_ctx->finish_reason,
              sizeof(result->finish_reason) - 1);
   }

   /* Round-trip metadata */
   if (rctx.response_id[0] != '\0') {
      result->response_id = strdup(rctx.response_id);
   }
   if (rctx.reasoning_items && json_object_array_length(rctx.reasoning_items) > 0) {
      /* Wrap as {"openai_responses": {"reasoning_items": [...]}} so the assistant
       * message field _provider_state can hold per-provider state. */
      struct json_object *prov = json_object_new_object();
      struct json_object *openai_resp = json_object_new_object();
      json_object_object_add(openai_resp, "reasoning_items", json_object_get(rctx.reasoning_items));
      json_object_object_add(prov, "openai_responses", openai_resp);
      const char *s = json_object_to_json_string_ext(prov, JSON_C_TO_STRING_PLAIN |
                                                               JSON_C_TO_STRING_NOSLASHESCAPE);
      if (s) {
         result->provider_state_json = strdup(s);
      }
      json_object_put(prov);
   }

#ifdef ENABLE_WEBUI
   if (rctx.reasoning_tokens > 0) {
      session_t *ws = session_get_command_context();
      if (ws && ws->type == SESSION_TYPE_WEBUI) {
         webui_send_reasoning_summary(ws, rctx.reasoning_tokens);
      }
   }
#endif

   sse_parser_free(rctx.sse_parser);
   llm_stream_free(rctx.stream_ctx);
   if (rctx.reasoning_items)
      json_object_put(rctx.reasoning_items);
   json_object_put(root);
   curl_buffer_free(&rctx.raw_response);

   return 0;
}
