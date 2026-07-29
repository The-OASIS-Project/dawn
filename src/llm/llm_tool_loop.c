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
 * Central tool iteration loop for LLM streaming with tool calling.
 *
 * This module extracts the tool call -> execute -> re-call loop from the
 * individual providers (OpenAI, Claude) into a single central loop that:
 * - Runs auto-compaction between iterations (THE KEY FIX)
 * - Provides duplicate tool call detection for all providers
 * - Handles provider switching mid-loop (switch_llm tool)
 * - Enforces uniform iteration limits
 */

#include "llm/llm_tool_loop.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/conv_event.h"
#include "core/event_payload.h"
#include "core/session_manager.h"
#include "llm/llm_claude.h"
#include "llm/llm_context.h"
#include "llm/llm_interface.h"
#include "llm/llm_openai.h"
#include "llm/llm_openai_internal.h"
#include "llm/llm_rate_limit.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "webui/webui_server.h"

/* Transient-network retry policy.  Triggered when the provider returns rc != 0
 * AND llm_last_error() == LLM_ERR_TRANSIENT_NETWORK (pre-flight unreachable,
 * HTTP 429, or HTTP 5xx).  Backoff is exponential from BASE: 1s, 2s, 4s. */
#define LLM_TRANSIENT_RETRY_MAX 3
#define LLM_TRANSIENT_BACKOFF_BASE_MS 1000

/* Thread-local flag: set when the tool loop skips follow-up */
static _Thread_local bool s_did_skip_followup = false;

/* Sleep in 250ms chunks honoring llm_is_interrupt_requested().  Returns 1 if
 * the wait was interrupted (caller should bail), 0 if the full duration
 * elapsed normally.  Not SUCCESS/FAILURE — the 1/0 is a normal/interrupted
 * sentinel.  Mirrors the pattern in llm_rate_limit.c (a future refactor could
 * promote both copies to a shared helper — see docs/TODO.md).
 *
 * 250ms is below the ~400ms wake-word-feels-laggy threshold and gives 2.5×
 * fewer scheduler wakeups than 100ms on the 7s worst-case backoff path. */
static int sleep_with_interrupt_check(int total_ms) {
   struct timespec chunk = { .tv_sec = 0, .tv_nsec = 250000000L }; /* 250ms */
   int elapsed_ms = 0;
   while (elapsed_ms < total_ms) {
      if (llm_is_interrupt_requested()) {
         return 1;
      }
      nanosleep(&chunk, NULL);
      elapsed_ms += 250;
   }
   return 0;
}

bool llm_tool_loop_did_skip_followup(void) {
   return s_did_skip_followup;
}

/* =============================================================================
 * History Format Helpers
 *
 * Format assistant messages with tool calls in the provider's native format.
 * Extracted from llm_openai.c and llm_claude.c recursion blocks.
 * ============================================================================= */

/* Map the active provider to a short label stored as METADATA in the reasoning JSON.
 * The WebUI does NOT display this — the "AI thought" panel is labelled with the
 * assistant's name (DawnFormat.assistantName) — but it's kept for debugging/provenance
 * (which provider produced a stored reasoning blob). */
static const char *reasoning_provider_label(llm_type_t llm_type, cloud_provider_t cloud_provider) {
   if (llm_type == LLM_LOCAL) {
      return "local";
   }
   switch (cloud_provider) {
      case CLOUD_PROVIDER_CLAUDE:
         return "claude";
      case CLOUD_PROVIDER_GEMINI:
         return "gemini";
      case CLOUD_PROVIDER_OPENROUTER:
         return "openrouter";
      case CLOUD_PROVIDER_OPENAI:
      default:
         return "openai";
   }
}

/* Build the display-only reasoning JSON ({provider, content?, tokens?}) for an iteration's
 * assistant message, or NULL if there's nothing to show.  Duration is intentionally omitted
 * (the daemon doesn't track per-iteration thinking wall-clock; the panel degrades to
 * "(N tokens)").  Caller frees with free(). */
static char *build_reasoning_json(const llm_tool_response_t *result, const char *provider_label) {
   bool has_content = result->thinking_content && result->thinking_content[0] != '\0';
   if (!has_content && result->reasoning_tokens <= 0) {
      return NULL;
   }
   struct json_object *obj = json_object_new_object();
   if (!obj) {
      return NULL;
   }
   json_object_object_add(obj, "provider", json_object_new_string(provider_label));
   if (has_content) {
      json_object_object_add(obj, "content", json_object_new_string(result->thinking_content));
   }
   if (result->reasoning_tokens > 0) {
      json_object_object_add(obj, "tokens", json_object_new_int(result->reasoning_tokens));
   }
   /* PLAIN: no inter-token whitespace — trims the persisted blob (reasoning content can
    * be multi-KB). Re-parsed as JSON on reload, so the formatting is immaterial. */
   char *out = strdup(json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN));
   json_object_put(obj);
   if (!out) {
      OLOG_WARNING("build_reasoning_json: strdup OOM — reasoning will not persist for this turn");
   }
   return out;
}

/* Persist the tool messages just appended to history in [before_len, end) via the
 * session's tool-persist hook (if set), in OpenAI-canonical form.  Runs on the
 * worker thread with NO lock held, so conv_db (auth_db lock) is safe to call here.
 * OpenAI-format history is already canonical; Claude-format is normalized through the
 * shared converter so the stored shape is provider-neutral.
 *
 * @p reasoning_json is the display-only reasoning JSON for THIS iteration's assistant
 * message (NULL if none).  It attaches to the single assistant row this iteration
 * appended — guaranteed unique because convert_claude_tool_to_openai() and
 * append_openai_tool_history() collapse text + all tool_use into exactly one assistant
 * row and fan out only role:tool result rows.
 *
 * A persisted capture-image message (see llm_tools_add_results_openai/claude,
 * llm_tools.c) is DELIBERATELY excluded here: it carries neither tool_calls
 * nor tool_call_id, so it's dropped by both loops below regardless of
 * provider — Claude's convert_claude_tool_to_openai() returns 0 for it
 * (no tool_use/tool_result blocks) and OpenAI's plain passthrough still fails
 * the tool_calls/tool_call_id gate in the second loop.  This is intentional,
 * not a gap: conv_db reload expects images as `[IMAGE:id]` markers pointing
 * at the image store (see webui_image_rehydrate.c), not raw embedded base64
 * in a message row — writing the base64 JSON verbatim here would produce a
 * row the reload path can't parse back into a real image.  Wiring captures
 * through the image store's marker system is a real follow-up, not a
 * same-diff fix.  Net effect: tool-captured images stay visible for the
 * live session (in-memory conversation_history) but don't survive a WebUI
 * reconnect/reload — same as before this feature existed. */
static void persist_appended_tool_turn(llm_tool_loop_params_t *params,
                                       int before_len,
                                       const char *reasoning_json) {
   /* for_reconnect (not session_get): a turn that survives a client disconnect
    * (background-jobs Phase 1) must STILL persist its tool-iteration rows
    * server-side.  session_get() skips disconnected sessions and would return
    * NULL here, silently dropping the tool_calls + role:tool rows so a reload
    * shows the answer but no tool use.  The session is not torn down mid-turn
    * (turn_in_flight guards the idle sweep; the worker holds a ref). */
   session_t *s = session_get_for_reconnect(params->session_id);
   if (!s) {
      return;
   }
   session_tool_persist_fn cb = s->tool_persist_cb;
   void *ud = s->tool_persist_userdata;
   /* NOTE: deliberately no early-return when cb is NULL.  The observe-side
    * event log (background-jobs Phase 2) is emitted from the same walk below,
    * and it must NOT be coupled to whether a conversation-persist hook happens
    * to be installed — a surface with no persist hook still has tool steps
    * worth watching. */

   int after = json_object_array_length(params->conversation_history);
   struct json_object *canonical = json_object_new_array();
   if (!canonical) {
      session_release(s);
      return;
   }

   for (int i = before_len; i < after; i++) {
      struct json_object *msg = json_object_array_get_idx(params->conversation_history, i);
      if (params->history_format == LLM_HISTORY_CLAUDE) {
         convert_claude_tool_to_openai(msg, canonical);
      } else {
         json_object_array_add(canonical, json_object_get(msg));
      }
   }

   /* The event log is conversation-scoped; the turn's conversation was captured
    * at dispatch (never the live view — Phase 0's rule).  ev_observe additionally
    * scopes step events to the same observe set as the `status` heartbeat, so
    * ordinary interactive turns don't pay the durable-log + fan-out tax. */
   const int64_t ev_conv = atomic_load(&s->stream_conversation_id);
   const int ev_user = (int)s->metrics.user_id;
   const bool ev_observe = ev_conv > 0 && atomic_load(&s->events_observable);

   /* The canonical role:tool result row carries no tool name — only its
    * tool_call_id — but the redactor needs the name to apply the
    * TOOL_CAP_SECRETS backstop.  The assistant tool_calls row (with both id and
    * name) precedes its results in this same batch, so map id->name as we pass
    * it and look up when the result arrives. */
   struct json_object *tcid_to_name = ev_observe ? json_object_new_object() : NULL;

   int n = json_object_array_length(canonical);
   for (int i = 0; i < n; i++) {
      struct json_object *m = json_object_array_get_idx(canonical, i);
      struct json_object *role_obj, *content_obj, *tc_obj, *tcid_obj;
      if (!json_object_object_get_ex(m, "role", &role_obj)) {
         continue;
      }
      const char *role = json_object_get_string(role_obj);
      const char *content = json_object_object_get_ex(m, "content", &content_obj)
                                ? json_object_get_string(content_obj)
                                : "";
      if (json_object_object_get_ex(m, "tool_calls", &tc_obj)) {
         const char *tc_json = json_object_to_json_string(tc_obj);
         if (cb) {
            cb(ud, role, content ? content : "", tc_json, NULL, reasoning_json);
         }
         /* One tool_call event per call in the batch: an iteration can invoke
          * several tools, and a tailer wants them individually, not as one blob. */
         if (ev_observe && json_object_is_type(tc_obj, json_type_array)) {
            int ncalls = json_object_array_length(tc_obj);
            for (int c = 0; c < ncalls; c++) {
               struct json_object *call = json_object_array_get_idx(tc_obj, c);
               struct json_object *fn = NULL, *nm = NULL, *ar = NULL, *ido = NULL;
               const char *tool_name = NULL, *args = NULL, *call_id = NULL;
               if (json_object_object_get_ex(call, "function", &fn)) {
                  if (json_object_object_get_ex(fn, "name", &nm)) {
                     tool_name = json_object_get_string(nm);
                  }
                  if (json_object_object_get_ex(fn, "arguments", &ar)) {
                     args = json_object_get_string(ar);
                  }
               }
               if (json_object_object_get_ex(call, "id", &ido)) {
                  call_id = json_object_get_string(ido);
               }
               if (call_id && tool_name && tcid_to_name) {
                  json_object_object_add(tcid_to_name, call_id, json_object_new_string(tool_name));
               }
               conv_event_emit(ev_conv, ev_user, CONV_EVENT_TOOL_CALL,
                               event_payload_tool_call(tool_name, args));
            }
         }
      } else if (json_object_object_get_ex(m, "tool_call_id", &tcid_obj)) {
         const char *tcid = json_object_get_string(tcid_obj);
         if (cb) {
            cb(ud, role, content ? content : "", NULL, tcid, NULL);
         }
         if (ev_observe) {
            /* Recover the tool name from the batch's tool_calls (mapped above) so
             * the redactor's TOOL_CAP_SECRETS backstop can fire; without it every
             * result persisted unredacted.  A rare uncorrelated result (NULL name)
             * stays unredacted, as before — a value-shape scan of the whole result
             * body is deliberately NOT applied here, since it would redact any
             * result merely containing a long opaque run (search URLs, base64) and
             * gut the observe surface. */
            struct json_object *nmo = NULL;
            const char *rtool = (tcid && tcid_to_name &&
                                 json_object_object_get_ex(tcid_to_name, tcid, &nmo))
                                    ? json_object_get_string(nmo)
                                    : NULL;
            conv_event_emit(ev_conv, ev_user, CONV_EVENT_TOOL_RESULT,
                            event_payload_tool_result(rtool, content));
         }
      }
   }

   json_object_put(tcid_to_name);
   json_object_put(canonical);
   session_release(s);
}

/* Stash the final turn's finish/stop reason on the session so a background-job
 * worker can tell a cut-off answer ("max_tokens"/"length") from a clean finish.
 * Additive + best-effort: other callers never read it, and a lookup miss is a
 * no-op (the worker then reads an empty reason = "not truncated"). */
static void tool_loop_stash_finish_reason(uint32_t session_id, const char *reason) {
   if (reason == NULL || reason[0] == '\0') {
      return;
   }
   session_t *s = session_get_for_reconnect(session_id);
   if (s == NULL) {
      return;
   }
   snprintf(s->last_finish_reason, sizeof(s->last_finish_reason), "%s", reason);
   session_release(s);
}

/* Fire the per-session iteration-boundary hook (if installed).  Called when an
 * iteration produced tool calls, just before the tools execute, so the WebUI can
 * close the current streaming bubble — the next iteration's text then opens a fresh
 * bubble below the tool entries.  No-op for satellite / local-mic turns (hook NULL). */
static void fire_tool_iteration_boundary(llm_tool_loop_params_t *params) {
   session_t *s = session_get(params->session_id);
   if (!s) {
      return;
   }
   if (s->tool_iteration_cb) {
      s->tool_iteration_cb(s, s->tool_iteration_userdata);
   }
   session_release(s);
}

/**
 * @brief Add assistant message with tool calls in OpenAI format
 *
 * Appends the assistant message containing tool_calls array, then adds
 * tool result messages. Handles Gemini thought_signature if present.
 */
static void append_openai_tool_history(struct json_object *history,
                                       const llm_tool_response_t *response,
                                       const tool_result_list_t *results) {
   json_object *assistant_msg = json_object_new_object();
   json_object_object_add(assistant_msg, "role", json_object_new_string("assistant"));
   /* Preserve any text the LLM streamed before its tool calls, so the follow-up
    * iteration can see what was already said and won't repeat itself.
    * Use empty string as fallback for Gemini API compatibility (rejects NULL). */
   const char *pre_tool_text = (response->text && response->text[0] != '\0') ? response->text : "";
   json_object_object_add(assistant_msg, "content", json_object_new_string(pre_tool_text));

   json_object *tc_array = json_object_new_array();
   for (int i = 0; i < response->tool_calls.count; i++) {
      json_object *tc = json_object_new_object();
      json_object_object_add(tc, "id", json_object_new_string(response->tool_calls.calls[i].id));
      json_object_object_add(tc, "type", json_object_new_string("function"));

      json_object *func = json_object_new_object();
      json_object_object_add(func, "name",
                             json_object_new_string(response->tool_calls.calls[i].name));
      json_object_object_add(func, "arguments",
                             json_object_new_string(response->tool_calls.calls[i].arguments));
      json_object_object_add(tc, "function", func);

      /* Gemini 3+ models: Include thought_signature in first tool call */
      if (i == 0 && response->tool_calls.thought_signature[0] != '\0') {
         json_object *extra_content = json_object_new_object();
         json_object *google_obj = json_object_new_object();
         json_object_object_add(google_obj, "thought_signature",
                                json_object_new_string(response->tool_calls.thought_signature));
         json_object_object_add(extra_content, "google", google_obj);
         json_object_object_add(tc, "extra_content", extra_content);
         OLOG_INFO("Tool loop: Including Gemini thought_signature in follow-up request");
      }

      json_object_array_add(tc_array, tc);
   }
   json_object_object_add(assistant_msg, "tool_calls", tc_array);

   /* OpenAI Responses API round-trip: attach opaque per-provider state
    * (reasoning items + response_id) so the next iteration can echo it. */
   if (response->provider_state_json && *response->provider_state_json) {
      json_object *prov = json_tokener_parse(response->provider_state_json);
      if (prov) {
         json_object_object_add(assistant_msg, "_provider_state", prov);
      }
   }

   json_object_array_add(history, assistant_msg);

   /* Add tool results */
   llm_tools_add_results_openai(history, results);
}

/**
 * @brief Add assistant message with tool calls in Claude format
 *
 * Appends the assistant message containing thinking blocks (if any) and
 * tool_use content blocks, then adds tool result messages.
 */
static void append_claude_tool_history(struct json_object *history,
                                       const llm_tool_response_t *response,
                                       const tool_result_list_t *results) {
   json_object *assistant_msg = json_object_new_object();
   json_object_object_add(assistant_msg, "role", json_object_new_string("assistant"));

   json_object *content_array = json_object_new_array();

   /* If thinking was enabled, add the thinking block first (required by Claude API) */
   if (response->thinking_content) {
      json_object *thinking_block = json_object_new_object();
      json_object_object_add(thinking_block, "type", json_object_new_string("thinking"));
      json_object_object_add(thinking_block, "thinking",
                             json_object_new_string(response->thinking_content));

      if (response->thinking_signature) {
         json_object_object_add(thinking_block, "signature",
                                json_object_new_string(response->thinking_signature));
      }

      json_object_array_add(content_array, thinking_block);
   }

   /* Preserve any text the LLM streamed before its tool_use blocks, so the follow-up
    * iteration can see what was already said and won't repeat itself. */
   if (response->text && response->text[0] != '\0') {
      json_object *text_block = json_object_new_object();
      json_object_object_add(text_block, "type", json_object_new_string("text"));
      json_object_object_add(text_block, "text", json_object_new_string(response->text));
      json_object_array_add(content_array, text_block);
   }

   /* Add tool_use blocks */
   for (int i = 0; i < response->tool_calls.count; i++) {
      json_object *tool_use = json_object_new_object();
      json_object_object_add(tool_use, "type", json_object_new_string("tool_use"));
      json_object_object_add(tool_use, "id",
                             json_object_new_string(response->tool_calls.calls[i].id));
      json_object_object_add(tool_use, "name",
                             json_object_new_string(response->tool_calls.calls[i].name));

      json_object *args = json_tokener_parse(response->tool_calls.calls[i].arguments);
      if (args) {
         json_object_object_add(tool_use, "input", args);
      } else {
         json_object_object_add(tool_use, "input", json_object_new_object());
      }

      json_object_array_add(content_array, tool_use);
   }

   json_object_object_add(assistant_msg, "content", content_array);
   json_object_array_add(history, assistant_msg);

   /* Add tool results in Claude format */
   llm_tools_add_results_claude(history, results);
}

/**
 * @brief Add closing assistant message to complete tool call history
 *
 * When skip_followup is set, we need to add a synthetic assistant message
 * to prevent HTTP 400 on subsequent requests due to incomplete history.
 */
static void append_closing_message(struct json_object *history,
                                   const char *text,
                                   llm_history_format_t format) {
   if (!text) {
      return;
   }

   json_object *closing_msg = json_object_new_object();
   json_object_object_add(closing_msg, "role", json_object_new_string("assistant"));

   if (format == LLM_HISTORY_CLAUDE) {
      json_object *content_array = json_object_new_array();
      json_object *text_block = json_object_new_object();
      json_object_object_add(text_block, "type", json_object_new_string("text"));
      json_object_object_add(text_block, "text", json_object_new_string(text));
      json_object_array_add(content_array, text_block);
      json_object_object_add(closing_msg, "content", content_array);
   } else {
      json_object_object_add(closing_msg, "content", json_object_new_string(text));
   }

   json_object_array_add(history, closing_msg);
   OLOG_INFO("Tool loop: Added closing assistant message to complete history");
}

/**
 * @brief Free heap-allocated data from tool results (vision images, extended results)
 */
static void free_tool_result_resources(tool_result_list_t *results) {
   if (!results) {
      return;
   }
   for (int i = 0; i < results->count; i++) {
      if (results->results[i].vision_image) {
         free(results->results[i].vision_image);
         results->results[i].vision_image = NULL;
      }
      if (results->results[i].result_extended) {
         free(results->results[i].result_extended);
         results->results[i].result_extended = NULL;
      }
   }
}

/**
 * @brief Resolve current provider configuration for follow-up calls
 *
 * Checks if the provider was switched (e.g., switch_llm tool) and updates
 * the loop parameters accordingly.
 *
 * @return true if provider changed, false if same provider
 */
static bool resolve_provider_switch(llm_tool_loop_params_t *params) {
   llm_resolved_config_t current_config;

   if (llm_get_current_resolved_config(&current_config) != 0) {
      return false; /* Can't resolve config, stay on current provider */
   }

   /* Copy model to params-owned buffer (resolved ptr may dangle after return) */
   if (current_config.model && current_config.model[0] != '\0') {
      strncpy(params->model_storage, current_config.model, LLM_MODEL_NAME_MAX - 1);
      params->model_storage[LLM_MODEL_NAME_MAX - 1] = '\0';
      params->model = params->model_storage;
   }

   /* Determine new provider type and format */
   llm_single_shot_fn new_fn = params->provider_fn;
   llm_history_format_t new_format = params->history_format;
   bool switched = false;

   if (current_config.type == LLM_LOCAL || current_config.cloud_provider == CLOUD_PROVIDER_OPENAI ||
       current_config.cloud_provider == CLOUD_PROVIDER_GEMINI ||
       current_config.cloud_provider == CLOUD_PROVIDER_OPENROUTER) {
      if (params->history_format == LLM_HISTORY_CLAUDE) {
         /* Switched from Claude to OpenAI/local/Gemini/OpenRouter */
         new_fn = (llm_single_shot_fn)llm_openai_streaming_single_shot;
         new_format = LLM_HISTORY_OPENAI;
         switched = true;
         OLOG_INFO("Tool loop: Provider switched to OpenAI/local");
      }
   } else if (current_config.cloud_provider == CLOUD_PROVIDER_CLAUDE) {
      if (params->history_format == LLM_HISTORY_OPENAI) {
         /* Switched from OpenAI/local/Gemini to Claude */
         new_fn = (llm_single_shot_fn)llm_claude_streaming_single_shot;
         new_format = LLM_HISTORY_CLAUDE;
         switched = true;
         OLOG_INFO("Tool loop: Provider switched to Claude");
      }
   }

   /* Always update credentials (even if provider didn't change,
    * config may have changed model/endpoint) */
   params->base_url = current_config.endpoint ? current_config.endpoint : params->base_url;
   params->api_key = current_config.api_key;
   params->llm_type = current_config.type;
   params->cloud_provider = current_config.cloud_provider;

   if (switched) {
      params->provider_fn = new_fn;
      params->history_format = new_format;
   }

   return switched;
}

/* =============================================================================
 * Central Tool Iteration Loop
 * ============================================================================= */

char *llm_tool_iteration_loop(llm_tool_loop_params_t *params) {
   if (!params || !params->provider_fn || !params->conversation_history) {
      OLOG_ERROR("Tool loop: Invalid parameters");
      return NULL;
   }

   s_did_skip_followup = false;
   char *final_response = NULL;

   for (int iteration = 0; iteration <= LLM_TOOLS_MAX_ITERATIONS; iteration++) {
      /* Step 0: Merge any completed async compaction (invisible to user).
       * for_reconnect so a turn surviving a client disconnect still merges its
       * compaction across iterations (session_get would skip a disconnected
       * session and let context grow unbounded on a long survivor). */
      session_t *loop_session = session_get_for_reconnect(params->session_id);
      if (loop_session) {
         llm_context_async_merge(loop_session, params->conversation_history);
         session_release(loop_session);
      }

      /* Step 1: Auto-compact if needed (hard threshold — blocking safety net).
       * This runs EVERY iteration, not just once at the top of the entry point.
       * Without this, context can overflow during multi-step tool iterations. */
      llm_context_auto_compact_with_config(params->conversation_history, params->session_id,
                                           params->llm_type, params->cloud_provider, params->model);

      /* Step 1b: Hard pre-flight window guard.  Summary-based compaction keeps the
       * most recent messages verbatim, so if a single recent tool result is itself
       * larger than the model window it can't be shrunk — the request would still
       * overflow and the provider rejects it with HTTP 400.  Surface that loudly
       * (it was previously silent) instead of shipping a doomed request; trimming
       * such oversized results at the source is the follow-up fix.
       *
       * This re-estimates the history *after* the compaction pass above (a distinct
       * value from that pass's pre-compaction decision estimate); the extra walk is
       * sub-millisecond and negligible against the per-iteration network round-trip. */
      {
         int est_tokens = llm_context_estimate_tokens(params->conversation_history);
         int window = llm_context_get_size(params->llm_type, params->cloud_provider, params->model);
         if (window > 0 && est_tokens >= window) {
            OLOG_ERROR("Tool loop: estimated request ~%d tokens still exceeds model window %d "
                       "after compaction (iteration %d) — provider will likely reject with 400; "
                       "an oversized recent tool result cannot be summarized away",
                       est_tokens, window, iteration);
         }
      }

      /* Step 2: Gate cloud calls through rate limiter */
      if (params->llm_type != LLM_LOCAL) {
         if (llm_rate_limit_wait()) {
            OLOG_WARNING("Tool loop: rate limit wait interrupted at iteration %d", iteration);
            return NULL;
         }
      }

      /* Step 3: Call provider single-shot */
      llm_tool_response_t result;
      memset(&result, 0, sizeof(result));

      int rc = params->provider_fn(params->conversation_history, params->input_text,
                                   params->vision_images, params->vision_image_sizes,
                                   params->vision_image_count, params->base_url, params->api_key,
                                   params->model, params->chunk_callback, params->callback_userdata,
                                   iteration, &result);

      /* Retry transient network failures (pre-flight unreachable, HTTP 429,
       * HTTP 5xx) with exponential backoff before bubbling up.  Each retry is
       * a fresh provider call within the same iteration — failed attempts
       * don't burn an iteration slot.  Hard errors (auth, bad request,
       * parse failure) bypass this loop.
       *
       * Retries re-enter llm_rate_limit_wait() on cloud paths: a server-driven
       * 429 means we're locally overcounted, so we must consult the local
       * budget again rather than racing past it.  Log noise stays bounded:
       * intermediate retries log at INFO, final give-up logs at ERROR. */
      for (int retry = 0; rc != 0 && llm_last_error() == LLM_ERR_TRANSIENT_NETWORK &&
                          retry < LLM_TRANSIENT_RETRY_MAX;
           retry++) {
         int backoff_ms = LLM_TRANSIENT_BACKOFF_BASE_MS << retry; /* 1s, 2s, 4s */
         OLOG_INFO("Tool loop: transient network error at iteration %d, "
                   "retry %d/%d in %dms",
                   iteration, retry + 1, LLM_TRANSIENT_RETRY_MAX, backoff_ms);
         if (sleep_with_interrupt_check(backoff_ms)) {
            OLOG_INFO("Tool loop: retry backoff interrupted at iteration %d", iteration);
            llm_tool_response_free(&result);
            return NULL;
         }
         /* Re-enter the rate-limit gate on cloud retries (mirrors the
          * top-of-iteration gate at the start of this loop).  Local LLM
          * is exempt — no shared per-minute budget there. */
         if (params->llm_type != LLM_LOCAL) {
            if (llm_rate_limit_wait()) {
               OLOG_WARNING("Tool loop: rate limit wait interrupted during retry at "
                            "iteration %d",
                            iteration);
               llm_tool_response_free(&result);
               return NULL;
            }
         }
         llm_tool_response_free(&result);
         memset(&result, 0, sizeof(result));
         rc = params->provider_fn(params->conversation_history, params->input_text,
                                  params->vision_images, params->vision_image_sizes,
                                  params->vision_image_count, params->base_url, params->api_key,
                                  params->model, params->chunk_callback, params->callback_userdata,
                                  iteration, &result);
      }

      if (rc != 0) {
         if (llm_last_error() == LLM_ERR_TRANSIENT_NETWORK) {
            OLOG_ERROR("Tool loop: transient network error at iteration %d after %d retries, "
                       "giving up",
                       iteration, LLM_TRANSIENT_RETRY_MAX);
         } else {
            OLOG_ERROR("Tool loop: Provider call failed at iteration %d", iteration);
         }
         llm_tool_response_free(&result);
         return NULL;
      }

      /* Step 4: If no tool calls, return text response.
       *
       * A clean provider call (rc == 0) with no tool_calls and NULL text is a
       * benign "end_turn with empty content" outcome — the LLM signalled the
       * preceding tool result was sufficient and no further response is
       * warranted.  Return an empty malloc'd string rather than NULL so
       * callers (session_manager.c) do not classify it as an LLM call
       * failure.  malloc(1) so the caller owns a heap pointer it can free()
       * identically to the populated path. */
      if (!result.has_tool_calls) {
         if (result.text == NULL) {
            OLOG_INFO("Tool loop: provider returned empty content at iteration %d "
                      "(clean end_turn, no response needed)",
                      iteration);
            llm_tool_response_free(&result);
            char *empty = malloc(1);
            if (!empty) {
               OLOG_ERROR("Tool loop: malloc(1) failed on empty-content path");
               return NULL;
            }
            empty[0] = '\0';
            return empty;
         }
         tool_loop_stash_finish_reason(params->session_id, result.finish_reason);
         final_response = result.text;
         result.text = NULL; /* Transfer ownership to caller */
         llm_tool_response_free(&result);
         return final_response;
      }

      /* Flush any streamed text from this iteration before executing tools.
       * The LLM may have streamed text before its tool_use blocks (e.g. "Let me check
       * that for you."). Without this, the sentence buffer holds "you." waiting for more
       * text, and the next iteration's response concatenates directly ("you.Alright")
       * without a sentence break. The paragraph break triggers the sentence buffer to
       * emit the pending sentence for TTS before the tool execution pause. */
      if (params->chunk_callback) {
         typedef void (*text_chunk_cb)(const char *, void *);
         ((text_chunk_cb)params->chunk_callback)("\n\n", params->callback_userdata);
      }

      /* Close the current streaming bubble at the iteration boundary so this
       * iteration's tool call/result entries render after its text, and the next
       * iteration opens a fresh bubble below them (live order matches reload order). */
      fire_tool_iteration_boundary(params);

      OLOG_INFO("Tool loop: %d tool call(s) at iteration %d/%d", result.tool_calls.count, iteration,
                LLM_TOOLS_MAX_ITERATIONS);

      for (int i = 0; i < result.tool_calls.count; i++) {
         OLOG_INFO("  Tool call [%d]: id=%s name=%s args=%s", i, result.tool_calls.calls[i].id,
                   result.tool_calls.calls[i].name, result.tool_calls.calls[i].arguments);
      }

      /* Step 5: Check for duplicate tool calls (prevents infinite loops) */
      if (result.tool_calls.count > 0 &&
          llm_tools_is_duplicate_call(params->conversation_history, result.tool_calls.calls[0].name,
                                      result.tool_calls.calls[0].arguments,
                                      params->history_format)) {
         OLOG_WARNING("Tool loop: Duplicate tool call detected, forcing text response");

         /* Add a hint to use existing results.  Phrased as a plain instruction
          * (no "[System:]" prefix) so a reasoning model doesn't mistake this
          * daemon control message for an injected directive and flag it; tool-
          * agnostic wording since this fires for any repeated tool, not search. */
         json_object *hint_msg = json_object_new_object();
         json_object_object_add(hint_msg, "role", json_object_new_string("user"));
         json_object_object_add(
             hint_msg, "content",
             json_object_new_string(
                 "You already called that tool with identical arguments and have its result. "
                 "Answer using the information you already have — do not call it again."));
         json_object_array_add(params->conversation_history, hint_msg);

         llm_tool_response_free(&result);

         /* Make one more call with tools disabled (iteration = MAX forces no tools) */
         OLOG_INFO("Tool loop: Making final call without tools to force text response");
         memset(&result, 0, sizeof(result));
         rc = params->provider_fn(params->conversation_history, "", NULL, NULL, 0, params->base_url,
                                  params->api_key, params->model, params->chunk_callback,
                                  params->callback_userdata, LLM_TOOLS_MAX_ITERATIONS, &result);

         if (rc != 0) {
            llm_tool_response_free(&result);
            return NULL;
         }

         tool_loop_stash_finish_reason(params->session_id, result.finish_reason);
         final_response = result.text;
         result.text = NULL;
         llm_tool_response_free(&result);
         return final_response;
      }

      /* Step 6: Execute tools */
      tool_result_list_t *results = calloc(1, sizeof(tool_result_list_t));
      if (!results) {
         OLOG_ERROR("Tool loop: Failed to allocate tool results");
         llm_tool_response_free(&result);
         return NULL;
      }
      llm_tools_execute_all(&result.tool_calls, results);

      /* Log tool results */
      for (int i = 0; i < results->count; i++) {
         const char *content = tool_result_content(&results->results[i]);
         OLOG_INFO("  Tool result [%d] id=%s result=%.200s%s", i, results->results[i].tool_call_id,
                   content, strlen(content) > 200 ? "..." : "");
      }

      /* Step 7: Check follow-up context BEFORE appending to history.
       * Tools like reset_conversation invalidate the conversation history pointer,
       * so we must not append to it after they run. */
      tool_followup_context_t followup;
      llm_tools_prepare_followup(results, &followup);

      if (followup.skip_followup) {
         OLOG_INFO("Tool loop: Skipping follow-up (tool requested no follow-up)");
         s_did_skip_followup = true;

         /* Send through chunk callback so TTS receives it */
         if (followup.direct_response && params->chunk_callback) {
            void (*cb)(const char *, void *) = params->chunk_callback;
            cb(followup.direct_response, params->callback_userdata);
         }

         free_tool_result_resources(results);
         free(results);
         llm_tool_response_free(&result);
         return followup.direct_response; /* Caller must free */
      }

      /* Step 8: Append assistant message + tool results to history */
      int hist_before = json_object_array_length(params->conversation_history);
      if (params->history_format == LLM_HISTORY_CLAUDE) {
         append_claude_tool_history(params->conversation_history, &result, results);
      } else {
         append_openai_tool_history(params->conversation_history, &result, results);
      }
      /* Step 8a: Persist the just-appended structured tool messages (E2) + this iteration's
       * display-only reasoning (E3) attached to its assistant row. */
      char *reasoning_json = build_reasoning_json(
          &result, reasoning_provider_label(params->llm_type, params->cloud_provider));
      persist_appended_tool_turn(params, hist_before, reasoning_json);
      free(reasoning_json);
      reasoning_json = NULL;

      /* Step 8b: All-silent check — tools handled their own output */
      if (followup.all_silent) {
         OLOG_INFO("Tool loop: All tools silent (should_respond=false), skipping follow-up");
         append_closing_message(params->conversation_history,
                                "[Tool execution completed without follow-up response]",
                                params->history_format);
         free_tool_result_resources(results);
         free(results);
         llm_tool_response_free(&result);
         return strdup(""); /* Empty = no TTS output */
      }

      /* Step 9: Check iteration limit — force a final text response with what we have */
      if (iteration >= LLM_TOOLS_MAX_ITERATIONS) {
         OLOG_WARNING("Tool loop: Max iterations (%d) reached, forcing text response",
                      LLM_TOOLS_MAX_ITERATIONS);

         /* Inject a system hint telling the LLM to respond with what it has */
         json_object *hint_msg = json_object_new_object();
         json_object_object_add(hint_msg, "role", json_object_new_string("user"));
         json_object_object_add(
             hint_msg, "content",
             json_object_new_string(
                 "[System: Maximum tool iterations reached. Respond to the user now with "
                 "the information you have gathered so far. Do not call any more tools.]"));
         json_object_array_add(params->conversation_history, hint_msg);

         free_tool_result_resources(results);
         free(results);
         llm_tool_response_free(&result);

         /* Make one final call with tools disabled */
         OLOG_INFO("Tool loop: Making final call without tools to present gathered results");
         memset(&result, 0, sizeof(result));
         int final_rc = params->provider_fn(params->conversation_history, "", NULL, NULL, 0,
                                            params->base_url, params->api_key, params->model,
                                            params->chunk_callback, params->callback_userdata,
                                            LLM_TOOLS_MAX_ITERATIONS, &result);

         char *final_text = NULL;
         if (final_rc == 0 && result.text) {
            final_text = strdup(result.text);
         }
         llm_tool_response_free(&result);
         return final_text;
      }

      /* Step 10: Vision images from a tool result (e.g. the `viewing` camera tool) are
       * now persisted directly into conversation_history by
       * llm_tools_add_results_openai/claude (Step 8), so they flow through normal
       * history serialization on this and every future request instead of a
       * one-shot injection. Clear the ephemeral params so a caller-supplied turn-0
       * image (e.g. a WebUI upload) isn't resent on iteration 2+. */
      params->vision_images = NULL;
      params->vision_image_sizes = NULL;
      params->vision_image_count = 0;

      /* Step 11: Check interrupt */
      if (llm_is_interrupt_requested()) {
         OLOG_INFO("Tool loop: Interrupted by user");
         free_tool_result_resources(results);
         free(results);
         llm_tool_response_free(&result);
         return NULL;
      }

      /* Step 12: Handle provider switching (switch_llm tool) */
      resolve_provider_switch(params);

      /* Clear input text for follow-up calls (history already contains the context) */
      params->input_text = "";

      /* Cleanup for next iteration */
      free_tool_result_resources(results);
      free(results);
      llm_tool_response_free(&result);
   }

   /* Should not reach here (loop exits via returns) */
   OLOG_ERROR("Tool loop: Fell through iteration loop unexpectedly");
   return NULL;
}
