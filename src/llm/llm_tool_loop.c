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

#include "config/dawn_config.h"
#include "core/conv_event.h"
#include "core/event_payload.h"
#include "core/session_manager.h"
#include "core/tool_result_store.h"
#include "llm/llm_cache_monitor.h"
#include "llm/llm_claude.h"
#include "llm/llm_claude_route.h"
#include "llm/llm_compaction.h"
#include "llm/llm_context.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_history_rows.h"
#include "llm/llm_interface.h"
#include "llm/llm_key_tag.h"
#include "llm/llm_openai.h"
#include "llm/llm_openai_internal.h"
#include "llm/llm_rate_limit.h"
#include "llm/llm_tool_images.h"
#include "llm/llm_tool_views.h"
#include "llm/llm_tool_views_apply.h"
#include "llm/llm_tools.h"
#include "llm/llm_turn_blocks.h"
#include "logging.h"
#include "prompts.h"
#include "utils/string_utils.h"
#include "webui/webui_server.h"

/* Transient-network retry policy.  Triggered when the provider returns rc != 0
 * AND llm_last_error() == LLM_ERR_TRANSIENT_NETWORK (pre-flight unreachable,
 * HTTP 429, or HTTP 5xx).  Backoff is exponential from BASE: 1s, 2s, 4s. */
#define LLM_TRANSIENT_RETRY_MAX 3
#define LLM_TRANSIENT_BACKOFF_BASE_MS 1000

/* Thread-local flag: set when the tool loop skips follow-up */
static _Thread_local bool s_did_skip_followup = false;

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

/* A string field of a row object, or NULL. */
static const char *str_field(struct json_object *obj, const char *key) {
   struct json_object *v = NULL;
   return json_object_object_get_ex(obj, key, &v) ? json_object_get_string(v) : NULL;
}

/* A request of this call's session is being sent: its size estimate, to
 * calibrate its model's density when its usage comes back (llm_compaction.h). */
static void note_request(const llm_tool_loop_params_t *params, const char *input) {
   if (!params->has_session) {
      return;
   }
   const size_t input_len = input ? strlen(input) : 0;
   const int estimate = llm_context_estimate_tokens(params->conversation_history) +
                        (int)(input_len / 4);
   llm_context_note_request(params->session_id, estimate, params->llm_type, params->cloud_provider,
                            params->model);
}

/* The session this call is for (a ref, or NULL): none for a call on no
 * session's behalf.  @p even_detached finds one whose client has gone (its
 * turn still runs and saves). */
static session_t *call_session(const llm_tool_loop_params_t *params, bool even_detached) {
   if (!params->has_session) {
      return NULL;
   }
   return even_detached ? session_get_for_reconnect(params->session_id)
                        : session_get(params->session_id);
}

/* Persist the tool messages just appended to history in [before_len, end) via the
 * session's tool-persist hook (if set), in OpenAI-canonical form.  Runs on the
 * worker thread with NO lock held, so conv_db (auth_db lock) is safe to call here.
 * Every format goes through llm_history_rows_append, so the stored shape is
 * provider-neutral and an assistant row carries its turn's stored blocks.
 *
 * @p reasoning_json is the display-only reasoning JSON for THIS iteration's assistant
 * message (NULL if none).  It attaches to the single assistant row this iteration
 * appended — guaranteed unique because an assistant turn is always one row
 * (llm_history_rows_append) and only role:tool result rows fan out.
 *
 * A result's images (llm_tool_images.h) go with its tool row: the row names
 * them by id (messages.images) and binds them as it is saved, so a reload
 * rebuilds them from the store as the live turn built them. */
static void persist_appended_tool_turn(llm_tool_loop_params_t *params,
                                       int before_len,
                                       const char *reasoning_json,
                                       int iteration,
                                       const tool_result_list_t *results) {
   /* for_reconnect (not session_get): a turn that survives a client disconnect
    * (background-jobs Phase 1) must STILL persist its tool-iteration rows
    * server-side.  session_get() skips disconnected sessions and would return
    * NULL here, silently dropping the tool_calls + role:tool rows so a reload
    * shows the answer but no tool use.  The session is not torn down mid-turn
    * (turn_in_flight guards the idle sweep; the worker holds a ref). */
   session_t *s = call_session(params, true);
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

   /* The rows each appended message saves as (llm_history_rows_append): the
    * canonical shape the walk below reads, whatever the history's format, and
    * for an assistant turn its stored blocks. */
   for (int i = before_len; i < after; i++) {
      llm_history_rows_append(json_object_array_get_idx(params->conversation_history, i),
                              canonical);
   }

   /* The event log is conversation-scoped; the turn's conversation was captured
    * at dispatch (never the live view — Phase 0's rule).  ev_observe scopes DURABLE
    * step events to the same observe set as the `status` heartbeat (jobs / background /
    * job-conversation turns), so ordinary interactive turns don't pay the durable-log tax.
    *
    * fan_ephemeral is the mirror image (SERVER_AUTHORITATIVE_PERSISTENCE §Phase-3): an
    * ordinary conversation-bound foreground turn (typed or voice) that is NOT on the durable
    * observe path fans its tool steps LIVE to the user's OTHER browsers viewing this conv, with
    * NO conversation_events write — the messages table already persists the step (tool-persist
    * hook), so reload rebuilds it; the fan is pure live-view sugar.  The two are mutually
    * exclusive (ev_observe XOR fan_ephemeral when ev_conv>0), so a step takes exactly one path
    * and jobs never double-emit. */
   const int64_t ev_conv = atomic_load(&s->stream_conversation_id);
   const int ev_user = (int)s->metrics.user_id;
   const bool ev_observe = ev_conv > 0 && atomic_load(&s->events_observable);
   const bool fan_ephemeral = ev_conv > 0 && !ev_observe;
   const bool ev_live = ev_observe || fan_ephemeral; /* either surface consumes tool events */

   /* Invariant guard (SERVER_AUTHORITATIVE): a foreground turn that promised full-turn
    * server persistence (will_persist_turn) and relies on the messages table for its tool
    * rows (fan_ephemeral — NOT the durable observe log) MUST have a tool-persist writer
    * installed.  cb==NULL here means an entry path armed will_persist_turn but never
    * installed the persist hook — the exact gap that silently dropped voice tool-turns.
    * Log-only (the rows are already lost either way); will_persist_turn scopes it off the
    * jobs/messaging/research paths, which don't arm it.  May repeat per tool iteration on a
    * genuinely broken turn — acceptable for a should-never-fire diagnostic. */
   if (fan_ephemeral && cb == NULL && atomic_load(&s->will_persist_turn)) {
      OLOG_WARNING("tool-persist gap: turn on conv %lld armed will_persist_turn but no persist "
                   "hook is installed — tool rows will NOT be saved (entry path missing "
                   "webui_turn_persist_arm?)",
                   (long long)ev_conv);
   }

   /* The canonical role:tool result row carries no tool name — only its
    * tool_call_id — but the redactor needs the name to apply the
    * TOOL_CAP_SECRETS backstop.  The assistant tool_calls row (with both id and
    * name) precedes its results in this same batch, so map id->name as we pass
    * it and look up when the result arrives.  Built when EITHER surface is live. */
   struct json_object *tcid_to_name = ev_live ? json_object_new_object() : NULL;

   /* Parallel tcid -> is_error map for the red-only pill signal.  Built UP FRONT from `results`
    * (whose structs carry the execute-time is_error verdict), not during the row walk — the
    * canonical role:tool rows carry only stripped text, so the failure signal must come from here.
    * Consumed by BOTH the persist hook (cb — so a reloaded conv reds the failed pill) and the live
    * tool_step emit; hence built whenever we persist OR fan, not only when ev_live.  A tcid with no
    * entry / an uncorrelated result degrades to no-error (neutral), the failure-safe direction. */
   struct json_object *tcid_to_error = (cb != NULL || ev_live) ? json_object_new_object() : NULL;
   if (tcid_to_error != NULL && results != NULL) {
      for (int r = 0; r < results->count; r++) {
         const char *rid = results->results[r].tool_call_id;
         if (rid != NULL && rid[0] != '\0') {
            json_object_object_add(tcid_to_error, rid,
                                   json_object_new_boolean(results->results[r].is_error));
         }
      }
   }

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
            /* Assistant tool_calls row — never a failure verdict itself (that rides the
             * role:tool result rows below). */
            const session_tool_row_t row = { .role = role,
                                             .content = content ? content : "",
                                             .tool_calls = tc_json,
                                             .reasoning = reasoning_json,
                                             .llm_blocks = str_field(m,
                                                                     LLM_HISTORY_ROW_STORED_KEY) };
            (void)cb(ud, &row);
         }
         /* One tool_call event per call in the batch: an iteration can invoke
          * several tools, and a tailer wants them individually, not as one blob. */
         if (ev_live && json_object_is_type(tc_obj, json_type_array)) {
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
               /* ev_observe (jobs) → durable conv_event_emit (persist + fan); else
                * fan_ephemeral (interactive/voice) → live-only cross-viewer fan, no DB row.
                * Both TAKE OWNERSHIP of the payload. */
               char *tc_payload = event_payload_tool_call(tool_name, args, call_id, iteration);
               if (ev_observe) {
                  conv_event_emit(ev_conv, ev_user, CONV_EVENT_TOOL_CALL, tc_payload);
               } else {
                  conv_event_tool_step_fanout(ev_conv, ev_user, s->session_id,
                                              atomic_load(&s->current_stream_id),
                                              CONV_EVENT_TOOL_CALL, tc_payload);
               }
            }
         }
      } else if (llm_history_kind_of(m) == MESSAGE_KIND_LOOP_NOTE) {
         /* A note the loop gave the model: saved where it sat, so a reload
          * replays the request the model answered. */
         if (cb) {
            const session_tool_row_t row = { .role = role,
                                             .content = content ? content : "",
                                             .kind = message_kind_name(MESSAGE_KIND_LOOP_NOTE) };
            (void)cb(ud, &row);
         }
      } else if (json_object_object_get_ex(m, "tool_call_id", &tcid_obj)) {
         const char *tcid = json_object_get_string(tcid_obj);
         /* Result's confirmed-failure verdict — needed by BOTH the persist hook (so reload reds
          * it) and the live emit below.  Uncorrelated/absent tcid -> false (neutral). */
         bool r_is_error = false;
         struct json_object *eo = NULL;
         if (tcid != NULL && tcid_to_error != NULL &&
             json_object_object_get_ex(tcid_to_error, tcid, &eo)) {
            r_is_error = json_object_get_boolean(eo);
         }
         if (cb) {
            struct json_object *images = NULL;
            json_object_object_get_ex(m, LLM_HISTORY_ROW_IMAGES_KEY, &images);
            const session_tool_row_t row = {
               .role = role,
               .content = content ? content : "",
               .tool_call_id = tcid,
               .is_error = r_is_error,
               .images = images ? json_object_to_json_string_ext(images, JSON_C_TO_STRING_PLAIN)
                                : NULL
            };
            (void)cb(ud, &row);
         }
         if (ev_live) {
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
            char *tr_payload = event_payload_tool_result(rtool, content, tcid, iteration,
                                                         r_is_error);
            if (ev_observe) {
               conv_event_emit(ev_conv, ev_user, CONV_EVENT_TOOL_RESULT, tr_payload);
            } else {
               conv_event_tool_step_fanout(ev_conv, ev_user, s->session_id,
                                           atomic_load(&s->current_stream_id),
                                           CONV_EVENT_TOOL_RESULT, tr_payload);
            }
         }
      }
   }

   json_object_put(tcid_to_name);
   json_object_put(tcid_to_error);
   json_object_put(canonical);
   session_release(s);
}

_Static_assert(LLM_TURN_CALLS_MAX >= LLM_TOOLS_MAX_PARALLEL_CALLS,
               "a turn's blocks must be able to hold every call that runs");

/* The response's blocks with their tool calls replaced by the calls that ran
 * (llm_turn_blocks_with_calls): the calls the stream carried and the calls
 * admitted and answered can differ (a cap, an over-long id, a stream cut
 * short), and a replay must hold each call with its result.  NULL without
 * blocks. */
static json_object *blocks_as_run(const llm_tool_response_t *response, bool calls_ran) {
   if (!response || !response->blocks) {
      return NULL;
   }
   llm_turn_call_t calls[LLM_TOOLS_MAX_PARALLEL_CALLS];
   int n = 0;
   for (int i = 0; calls_ran && i < response->tool_calls.count && i < LLM_TOOLS_MAX_PARALLEL_CALLS;
        i++) {
      calls[n].id = response->tool_calls.calls[i].id;
      calls[n].name = response->tool_calls.calls[i].name;
      calls[n].arguments = response->tool_calls.calls[i].arguments;
      n++;
   }
   return llm_turn_blocks_with_calls(response->blocks, calls, n);
}

/* Stash BOTH final-turn signals on the session in ONE lookup:
 *   - the finish/stop reason, so a background-job worker can tell a cut-off answer
 *     ("max_tokens"/"length") from a clean finish; and
 *   - the FINAL answer's display-only reasoning (E3 "AI thought" panel), so the
 *     post-dispatch server persist (webui_persist_final_answer) writes it to the
 *     messages.reasoning column + fans it out — the browser client-save carried this
 *     today, and retiring that save drops it server-side otherwise (SERVER_AUTHORITATIVE
 *     §6c-G1).
 * Both are additive + best-effort: other callers never read last_finish_reason, a lookup
 * miss is a no-op (the worker then reads an empty reason = "not truncated"), and a turn
 * with no thinking content leaves final_answer.reasoning_json NULL (build_reasoning_json returns
 * NULL; the turn-start clear in llm_call_prepare guarantees no stale inheritance).  Folded
 * from two helpers into one so a final-answer return does a single session_get_for_reconnect
 * rather than two.  TAKES ownership of the built reasoning JSON into the session (the
 * consuming persist frees it). */
static void tool_loop_stash_final(const llm_tool_loop_params_t *params,
                                  const llm_tool_response_t *result,
                                  const char *provider_label) {
   const char *reason = result != NULL ? result->finish_reason : NULL;
   bool have_reason = (reason != NULL && reason[0] != '\0');
   char *json = build_reasoning_json(result, provider_label);
   /* A final answer ran no calls: its blocks keep none. */
   struct json_object *blocks = blocks_as_run(result, false);
   if (!have_reason && json == NULL && blocks == NULL) {
      return; /* nothing to write — skip the lookup entirely */
   }
   session_t *s = call_session(params, true);
   if (s == NULL) {
      free(json);
      json_object_put(blocks);
      return;
   }
   if (have_reason) {
      snprintf(s->last_finish_reason, sizeof(s->last_finish_reason), "%s", reason);
   }
   if (json != NULL) {
      free(s->final_answer.reasoning_json);
      s->final_answer.reasoning_json = json; /* take ownership */
   }
   if (blocks != NULL) {
      json_object_put(s->final_answer.blocks);
      s->final_answer.blocks = blocks; /* the answer's blocks (take ownership) */
   }
   session_release(s);
}

/* Fire the per-session iteration-boundary hook (if installed).  Called when an
 * iteration produced tool calls, just before the tools execute, so the WebUI can
 * close the current streaming bubble — the next iteration's text then opens a fresh
 * bubble below the tool entries.  No-op for satellite / local-mic turns (hook NULL). */
static void fire_tool_iteration_boundary(llm_tool_loop_params_t *params) {
   session_t *s = call_session(params, false);
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
 * tool result messages.  The turn's blocks (reasoning, a call's signature)
 * go with it; each request renders what belongs to its endpoint.
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

      json_object_array_add(tc_array, tc);
   }
   json_object_object_add(assistant_msg, "tool_calls", tc_array);

   /* The turn's blocks, where the path captured them (OpenAI Responses): its
    * reasoning items go back with the next request, its calls as they ran. */
   json_object *blocks = blocks_as_run(response, true);
   if (blocks) {
      json_object_object_add(assistant_msg, LLM_TURN_BLOCKS_KEY, blocks);
   }

   session_history_append(history, assistant_msg);

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
                                       const tool_result_list_t *results,
                                       const char *carrier) {
   json_object *assistant_msg = json_object_new_object();
   json_object_object_add(assistant_msg, "role", json_object_new_string("assistant"));

   /* The turn exactly as the model produced it: every block in order, each
    * thinking block with its own signature (empty text or not).  The blocks
    * travel with the message; each formatter renders them for its provider. */
   json_object *blocks = blocks_as_run(response, true);
   json_object *rendered = llm_turn_blocks_render_claude(blocks, carrier);
   if (rendered) {
      json_object_object_add(assistant_msg, "content", rendered);
      json_object_object_add(assistant_msg, LLM_TURN_BLOCKS_KEY, blocks);
      session_history_append(history, assistant_msg);
      llm_tools_add_results_claude(history, results);
      return;
   }
   json_object_put(blocks);

   /* A path without blocks: the text and tool calls (its reasoning, if any,
    * can't be replayed without the blocks it came in). */
   json_object *content_array = json_object_new_array();

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
   session_history_append(history, assistant_msg);

   /* Add tool results in Claude format */
   llm_tools_add_results_claude(history, results);
}

/* Append a note to the model (a user message of the loop's own), marked as
 * request context. */
static void append_loop_note(struct json_object *history, const char *text) {
   json_object *note = json_object_new_object();
   if (!note) {
      return;
   }
   json_object_object_add(note, "role", json_object_new_string("user"));
   json_object_object_add(note, "content", json_object_new_string(text));
   llm_history_set_kind(note, MESSAGE_KIND_LOOP_NOTE);
   session_history_append(history, note);
}

/* The turn's last word: @p note appended (and saved) after the tool results,
 * then one call with tools disabled.  Returns the answer (caller frees), or
 * NULL when the call fails or brings no text. */
static char *final_answer_without_tools(llm_tool_loop_params_t *params,
                                        const char *note,
                                        int iteration) {
   const int note_at = json_object_array_length(params->conversation_history);
   append_loop_note(params->conversation_history, note);
   persist_appended_tool_turn(params, note_at, NULL, iteration, NULL);

   OLOG_INFO("Tool loop: Making final call without tools to present gathered results");
   llm_tool_response_t result;
   memset(&result, 0, sizeof(result));
   note_request(params, "");
   const int rc = params->provider_fn(params->conversation_history, "", params->base_url,
                                      params->api_key, params->model, params->chunk_callback,
                                      params->callback_userdata, LLM_TOOLS_MAX_ITERATIONS, &result);
   char *text = NULL;
   if (rc == 0 && result.text) {
      text = strdup(result.text);
      tool_loop_stash_final(params, &result,
                            reasoning_provider_label(params->llm_type, params->cloud_provider));
   }
   llm_tool_response_free(&result);
   return text;
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
   llm_history_set_kind(closing_msg, MESSAGE_KIND_LOOP_NOTE);

   session_history_append(history, closing_msg);
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
      safe_strscpy(params->model_storage, current_config.model);
      params->model = params->model_storage;
   }

   /* Determine new provider type and format */
   llm_single_shot_fn new_fn = params->provider_fn;
   llm_history_format_t new_format = params->history_format;
   bool switched = false;

   /* The wire format follows the route, not the provider: OpenRouter's
    * anthropic/ models take the Messages format too. */
   const char *endpoint = current_config.endpoint ? current_config.endpoint : params->base_url;
   const bool messages = llm_uses_anthropic_messages(current_config.type,
                                                     current_config.cloud_provider, params->model,
                                                     endpoint);
   if (!messages && params->history_format == LLM_HISTORY_CLAUDE) {
      new_fn = (llm_single_shot_fn)llm_openai_streaming_single_shot;
      new_format = LLM_HISTORY_OPENAI;
      switched = true;
      OLOG_INFO("Tool loop: Provider switched to the OpenAI-compatible API");
   } else if (messages && params->history_format == LLM_HISTORY_OPENAI) {
      new_fn = (llm_single_shot_fn)llm_claude_streaming_single_shot;
      new_format = LLM_HISTORY_CLAUDE;
      switched = true;
      OLOG_INFO("Tool loop: Provider switched to the Anthropic Messages API");
   }

   /* Always update credentials (even if provider didn't change,
    * config may have changed model/endpoint) */
   params->base_url = endpoint;
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

/* A batch's finish: its images held to what the next request may carry (a
 * loop has no seam to compact at), then the view stage. */
static void finish_batch(const tool_call_list_t *calls, tool_result_list_t *results, void *batch) {
   const llm_tool_views_batch_t *b = batch;
   llm_image_limit_t limit;
   (void)llm_tool_images_request_limit(b->params->llm_type, b->params->cloud_provider,
                                       b->params->model, &limit);
   (void)llm_tool_images_cap_batch(b->params->conversation_history, results, &limit);
   llm_tool_views_finish_batch(calls, results, batch);
}

static char *tool_iteration_loop_body(llm_tool_loop_params_t *params) {
   if (!params || !params->provider_fn || !params->conversation_history) {
      OLOG_ERROR("Tool loop: Invalid parameters");
      return NULL;
   }

   s_did_skip_followup = false;
   char *final_response = NULL;

   /* Cancellation policy for this turn (loop-invariant).  A background turn
    * (job / deep research) breaks only on its own session cancel flag; a
    * foreground turn additionally honors the global wake-word / Ctrl+C
    * interrupt.  A zero-valued params (cancel_flag=NULL, is_background=false)
    * yields {NULL, honor_global=true} — the legacy global-only behavior. */
   const llm_interrupt_ctx_t ictx = { .session_flag = params->cancel_flag,
                                      .honor_global = !params->is_background };

   for (int iteration = 0; iteration <= LLM_TOOLS_MAX_ITERATIONS; iteration++) {
      llm_cache_monitor_set_iteration(iteration); /* tags this iteration's provider call */
      /* Step 0: cumulative-session input-token ceiling (opt-in; 0 = unlimited).
       * Input tokens are recorded per provider response (session_record_query), so
       * at the top of this iteration the session total reflects spend through the
       * previous iteration.  Stopping HERE bounds overshoot to a single
       * iteration's tokens rather than a whole multi-tool turn's (the deep-
       * research per-round guard).  This check is only ever reached BETWEEN
       * iterations (a completed iteration with no tool calls already returned its
       * text at step 4), so there is no partial answer to hand back — return an
       * empty string (never NULL barring OOM) so the caller doesn't misread a
       * budget stop as a provider failure. */
      if (params->cumulative_input_token_ceiling > 0) {
         session_t *loop_session = call_session(params, true);
         if (loop_session) {
            uint64_t tok = 0;
            session_metrics_totals(loop_session, &tok, NULL);
            session_release(loop_session);
            if (tok >= (uint64_t)params->cumulative_input_token_ceiling) {
               OLOG_INFO("Tool loop: session input tokens %llu reached ceiling %lld at "
                         "iteration %d — stopping turn",
                         (unsigned long long)tok, (long long)params->cumulative_input_token_ceiling,
                         iteration);
               char *empty = malloc(1);
               if (empty != NULL) {
                  empty[0] = '\0';
               }
               return empty;
            }
         }
      }

      /* Step 1: the request this iteration sends must fit.  A history is
       * compacted only at a turn seam (session_compaction.h), never mid tool round
       * (its reasoning is bound to the request as it stands): past the hard
       * threshold after tools ran, the turn closes here.  A background turn goes
       * on in a continuation turn, compacted at its seam; a foreground one answers
       * with what it has, or, when even that wouldn't fit, says so. */
      {
         /* The request's real size: the estimate, calibrated by what this
          * session's requests measured (llm_compaction.h). */
         const int history_estimate = llm_context_estimate_tokens(params->conversation_history);
         const int est_tokens = params->has_session
                                    ? llm_context_request_tokens(params->session_id,
                                                                 history_estimate, params->llm_type,
                                                                 params->cloud_provider,
                                                                 params->model)
                                    : history_estimate;
         const int window = llm_context_get_size(params->llm_type, params->cloud_provider,
                                                 params->model);
         const float hard = llm_context_hard_threshold();
         if (window > 0 && iteration > 0 && est_tokens >= (int)((float)window * hard)) {
            OLOG_WARNING("Tool loop: ~%d tokens reach the hard threshold of the %d window at "
                         "iteration %d; closing the turn",
                         est_tokens, window, iteration);
            if (params->is_background) {
               session_t *loop_session = call_session(params, true);
               if (loop_session) {
                  atomic_store(&loop_session->turn_overflowed, true);
                  session_release(loop_session);
               }
               char *empty = malloc(1);
               if (empty != NULL) {
                  empty[0] = '\0';
               }
               return empty;
            }
            if (est_tokens < window) {
               return final_answer_without_tools(params, TOOL_LOOP_DIRECTIVE_CONTEXT_FULL,
                                                 iteration);
            }
            return strdup("I've run out of room in this conversation partway through this. Send "
                          "another message and I'll pick it up from a summary of what we have.");
         }
         if (window > 0 && est_tokens >= window) {
            OLOG_ERROR("Tool loop: estimated request ~%d tokens exceeds the model window %d "
                       "(iteration %d): a single message is larger than the window",
                       est_tokens, window, iteration);
         }
      }

      /* Step 2: Gate cloud calls through rate limiter */
      if (params->llm_type != LLM_LOCAL) {
         if (llm_rate_limit_wait_ctx(&ictx)) {
            OLOG_WARNING("Tool loop: rate limit wait interrupted at iteration %d", iteration);
            return NULL;
         }
      }

      /* Step 3: Call provider single-shot */
      llm_tool_response_t result;
      memset(&result, 0, sizeof(result));

      note_request(params, params->input_text);
      int rc = params->provider_fn(params->conversation_history, params->input_text,
                                   params->base_url, params->api_key, params->model,
                                   params->chunk_callback, params->callback_userdata, iteration,
                                   &result);

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
         if (llm_sleep_with_interrupt_check_ctx(backoff_ms, &ictx)) {
            OLOG_INFO("Tool loop: retry backoff interrupted at iteration %d", iteration);
            llm_tool_response_free(&result);
            return NULL;
         }
         /* Re-enter the rate-limit gate on cloud retries (mirrors the
          * top-of-iteration gate at the start of this loop).  Local LLM
          * is exempt — no shared per-minute budget there. */
         if (params->llm_type != LLM_LOCAL) {
            if (llm_rate_limit_wait_ctx(&ictx)) {
               OLOG_WARNING("Tool loop: rate limit wait interrupted during retry at "
                            "iteration %d",
                            iteration);
               llm_tool_response_free(&result);
               return NULL;
            }
         }
         llm_tool_response_free(&result);
         memset(&result, 0, sizeof(result));
         note_request(params, params->input_text);
         rc = params->provider_fn(params->conversation_history, params->input_text,
                                  params->base_url, params->api_key, params->model,
                                  params->chunk_callback, params->callback_userdata, iteration,
                                  &result);
      }

      if (rc != 0) {
         if (llm_last_error() == LLM_ERR_TRANSIENT_NETWORK) {
            OLOG_ERROR("Tool loop: transient network error at iteration %d after %d retries, "
                       "giving up",
                       iteration, LLM_TRANSIENT_RETRY_MAX);
         } else if (llm_interrupt_ctx_triggered(&ictx)) {
            /* The user interrupted (wake word, Stop): not a failure. */
            OLOG_INFO("Tool loop: provider call interrupted at iteration %d", iteration);
         } else {
            OLOG_ERROR("Tool loop: Provider call failed at iteration %d", iteration);
         }
         llm_tool_response_free(&result);
         return NULL;
      }

      /* Step 4: If no tool calls, return text response.
       *
       * A clean provider call (rc == 0) with no tool_calls and NULL text ends
       * the turn without a word.  After tools ran in a turn someone is waiting
       * on, the model is asked once for the outcome (tools off).  Otherwise, or
       * when that brings nothing, an empty malloc'd string is returned rather
       * than NULL so callers (session_manager.c) do not classify it as an LLM
       * call failure.  malloc(1) so the caller owns a heap pointer it can
       * free() identically to the populated path. */
      if (!result.has_tool_calls) {
         if (result.text == NULL && iteration > 0 && !params->is_background &&
             !llm_interrupt_ctx_triggered(&ictx)) {
            /* Tools ran this turn and the model ended without a word: the user
             * would see nothing, even when a tool failed.  Ask once for the
             * outcome, tools off.  (A background turn has no one waiting; its
             * empty ending is judged by its job.) */
            OLOG_INFO("Tool loop: empty content at iteration %d after tool calls; asking for "
                      "the outcome",
                      iteration);
            llm_tool_response_free(&result);
            char *answer = final_answer_without_tools(params, TOOL_LOOP_DIRECTIVE_REPORT_OUTCOME,
                                                      iteration);
            if (answer) {
               return answer;
            }
            memset(&result, 0, sizeof(result)); /* freed above: nothing left to free */
         }
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
         tool_loop_stash_final(params, &result,
                               reasoning_provider_label(params->llm_type, params->cloud_provider));
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
         const int note_at = json_object_array_length(params->conversation_history);
         append_loop_note(params->conversation_history, TOOL_LOOP_NOTE_DUPLICATE_CALL);
         persist_appended_tool_turn(params, note_at, NULL, iteration, NULL);

         llm_tool_response_free(&result);

         /* Make one more call with tools disabled (iteration = MAX forces no tools) */
         OLOG_INFO("Tool loop: Making final call without tools to force text response");
         memset(&result, 0, sizeof(result));
         note_request(params, "");
         rc = params->provider_fn(params->conversation_history, "", params->base_url,
                                  params->api_key, params->model, params->chunk_callback,
                                  params->callback_userdata, LLM_TOOLS_MAX_ITERATIONS, &result);

         if (rc != 0) {
            llm_tool_response_free(&result);
            return NULL;
         }

         tool_loop_stash_final(params, &result,
                               reasoning_provider_label(params->llm_type, params->cloud_provider));
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
      /* The batch's budget, planned before it runs: a result_read answer in
       * it is built to its fair share.  After it runs, the view stage keeps
       * what is over its share whole and shows a view, then finishes every
       * result (neutralized last; the WebUI told what the model sees) before
       * anything reads them. */
      llm_tool_views_budget_t view_budget;
      llm_tool_views_budget_batch(params, &result.tool_calls, result.text, &view_budget);
      session_t *view_session = call_session(params, true);
      tool_result_store_set_read_budget(
          view_session, llm_tool_views_read_chars(view_budget.chars, result.tool_calls.count));
      const llm_tool_views_batch_t view_batch = {
         .params = params,
         .session = view_session,
         .budget = &view_budget,
      };
      llm_tools_execute_all(&result.tool_calls, results, finish_batch, (void *)&view_batch);
      if (view_session) {
         tool_result_store_set_read_budget(view_session, 0);
         session_release(view_session);
      }

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
         /* No final-reasoning stash: direct_response is a tool's own canned text, not a
          * model turn, so it carries no E3 reasoning.  The turn-start clear leaves the
          * stash NULL here — do NOT "fix" this by stashing result's reasoning. */
         return followup.direct_response; /* Caller must free */
      }

      /* Step 8: Append assistant message + tool results to history */
      int hist_before = json_object_array_length(params->conversation_history);
      if (params->history_format == LLM_HISTORY_CLAUDE) {
         char carrier[LLM_CARRIER_MAX];
         llm_request_carrier(params->base_url, params->api_key, carrier, sizeof(carrier));
         append_claude_tool_history(params->conversation_history, &result, results, carrier);
      } else {
         append_openai_tool_history(params->conversation_history, &result, results);
      }
      /* Step 8a: Persist the just-appended structured tool messages (E2) + this iteration's
       * display-only reasoning (E3) attached to its assistant row. */
      char *reasoning_json = build_reasoning_json(
          &result, reasoning_provider_label(params->llm_type, params->cloud_provider));
      persist_appended_tool_turn(params, hist_before, reasoning_json, iteration, results);
      free(reasoning_json);
      reasoning_json = NULL;

      /* Step 8b: All-silent check — tools handled their own output */
      if (followup.all_silent) {
         OLOG_INFO("Tool loop: All tools silent (should_respond=false), skipping follow-up");
         const int note_at = json_object_array_length(params->conversation_history);
         append_closing_message(params->conversation_history,
                                "[Tool execution completed without follow-up response]",
                                params->history_format);
         persist_appended_tool_turn(params, note_at, NULL, iteration, NULL);
         free_tool_result_resources(results);
         free(results);
         llm_tool_response_free(&result);
         return strdup(""); /* Empty = no TTS output */
      }

      /* Step 9: Check iteration limit — force a final text response with what we have */
      if (iteration >= LLM_TOOLS_MAX_ITERATIONS) {
         OLOG_WARNING("Tool loop: Max iterations (%d) reached, forcing text response",
                      LLM_TOOLS_MAX_ITERATIONS);

         free_tool_result_resources(results);
         free(results);
         llm_tool_response_free(&result);
         /* Plain wording, as the duplicate-call note: a "[System:]" prefix reads
          * to a reasoning model as an injected directive. */
         return final_answer_without_tools(params, TOOL_LOOP_DIRECTIVE_ITERATION_CAP, iteration);
      }

      /* Step 11: Check interrupt.  Background turns (job / research) break only
       * on their own session cancel flag; foreground turns also honor the global
       * wake-word / Ctrl+C interrupt.  See llm_interrupt_ctx_t. */
      if (llm_interrupt_ctx_triggered(&ictx)) {
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

char *llm_tool_iteration_loop(llm_tool_loop_params_t *params) {
   /* A side call's own loop (a compaction or summarizer run from inside a turn's
    * iteration) hands the iteration back to the loop it interrupted. */
   const int outer_iteration = llm_cache_monitor_get_iteration();
   char *response = tool_iteration_loop_body(params);
   llm_cache_monitor_set_iteration(outer_iteration);
   return response;
}
