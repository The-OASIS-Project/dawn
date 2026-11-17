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
 * A tool exchange as the tool loop appends it to a conversation's history:
 * the assistant's call message in the history's format (OpenAI or Claude),
 * with the turn's blocks, then the tool results.
 */

#include "llm/llm_tool_history.h"

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "core/conv_event.h"
#include "core/event_payload.h"
#include "core/session_history.h"
#include "core/session_manager.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_history_rows.h"
#include "llm/llm_tools.h"
#include "llm/llm_turn_blocks.h"
#include "logging.h"

_Static_assert(LLM_TURN_CALLS_MAX >= LLM_TOOLS_MAX_PARALLEL_CALLS,
               "a turn's blocks must be able to hold every call that runs");

/* The response's blocks with their tool calls replaced by the calls that ran
 * (llm_turn_blocks_with_calls): the calls the stream carried and the calls
 * admitted and answered can differ (a cap, an over-long id, a stream cut
 * short), and a replay must hold each call with its result.  NULL without
 * blocks. */
json_object *llm_tool_history_blocks_as_run(const llm_tool_response_t *response, bool calls_ran) {
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

/**
 * @brief Add assistant message with tool calls in OpenAI format
 *
 * Appends the assistant message containing tool_calls array, then adds
 * tool result messages.  The turn's blocks (reasoning, a call's signature)
 * go with it; each request renders what belongs to its endpoint.
 */
void llm_tool_history_append_openai(struct json_object *history,
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
   json_object *blocks = llm_tool_history_blocks_as_run(response, true);
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
void llm_tool_history_append_claude(struct json_object *history,
                                    const llm_tool_response_t *response,
                                    const tool_result_list_t *results,
                                    const char *carrier) {
   json_object *assistant_msg = json_object_new_object();
   json_object_object_add(assistant_msg, "role", json_object_new_string("assistant"));

   /* The turn exactly as the model produced it: every block in order, each
    * thinking block with its own signature (empty text or not).  The blocks
    * travel with the message; each formatter renders them for its provider. */
   json_object *blocks = llm_tool_history_blocks_as_run(response, true);
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

/* A string field of a row object, or NULL. */
static const char *str_field(struct json_object *obj, const char *key) {
   struct json_object *v = NULL;
   return json_object_object_get_ex(obj, key, &v) ? json_object_get_string(v) : NULL;
}

/* A Claude message of tool results is a row per result: the result's part
 * takes its row's id (LLM_HISTORY_PART_ROW_ID_KEY), so a request rebuilt from
 * the live history names each row as a reload does. */
static void stamp_result_part(session_t *s,
                              struct json_object *msg,
                              struct json_object *row,
                              int64_t row_id) {
   const char *call_id = str_field(row, "tool_call_id");
   const char *role = str_field(msg, "role");
   struct json_object *content = NULL;
   if (row_id <= 0 || !call_id || (role && strcmp(role, "tool") == 0) ||
       !json_object_object_get_ex(msg, "content", &content) ||
       !json_object_is_type(content, json_type_array)) {
      return;
   }
   const size_t n = json_object_array_length(content);
   for (size_t i = 0; i < n; i++) {
      struct json_object *part = json_object_array_get_idx(content, i);
      const char *type = str_field(part, "type");
      const char *use_id = str_field(part, "tool_use_id");
      if (type && strcmp(type, "tool_result") == 0 && use_id && strcmp(use_id, call_id) == 0) {
         session_stamp_row_id(s, part, LLM_HISTORY_PART_ROW_ID_KEY, row_id);
         return;
      }
   }
}

/* Persist the tool messages just appended to @p history in [before_len, end) via
 * the session's tool-persist hook (if set), in OpenAI-canonical form.  Runs on the
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
void llm_tool_history_persist(session_t *s,
                              struct json_object *history,
                              int before_len,
                              const char *reasoning_json,
                              int iteration,
                              const tool_result_list_t *results) {
   if (!s || !history) {
      return;
   }
   session_tool_persist_fn cb = s->tool_persist_cb;
   void *ud = s->tool_persist_userdata;
   /* NOTE: deliberately no early-return when cb is NULL.  The observe-side
    * event log is emitted from the same walk below, and it must NOT be coupled
    * to whether a conversation-persist hook happens to be installed — a
    * surface with no persist hook still has tool steps worth watching. */

   int after = json_object_array_length(history);
   struct json_object *canonical = json_object_new_array();
   if (!canonical) {
      return;
   }

   /* The rows each appended message saves as (llm_history_rows_append): the
    * canonical shape the walk below reads, whatever the history's format, and
    * for an assistant turn its stored blocks.  owner[r] is row r's message,
    * stamped with the row's id once saved (NULL: out of memory, none are). */
   struct json_object **owner = NULL;
   bool owned = cb != NULL;
   for (int i = before_len; i < after; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      const size_t from = json_object_array_length(canonical);
      const int added = llm_history_rows_append(msg, canonical);
      if (owned && added > 0) {
         struct json_object **grown = realloc(owner, (from + (size_t)added) * sizeof(*owner));
         owned = grown != NULL;
         owner = grown ? grown : owner;
         for (int r = 0; grown && r < added; r++) {
            owner[from + (size_t)r] = msg;
         }
      }
   }
   int64_t *row_ids = owned && owner ? calloc(json_object_array_length(canonical), sizeof(*row_ids))
                                     : NULL;
   if (!row_ids) {
      free(owner);
      owner = NULL;
   }

   /* The event log is conversation-scoped; the turn's conversation was captured
    * at dispatch (never the live view).  ev_observe scopes DURABLE step events
    * to the same observe set as the `status` heartbeat (jobs / background /
    * job-conversation turns), so ordinary interactive turns don't pay the durable-log tax.
    *
    * fan_ephemeral is the mirror image: an ordinary conversation-bound foreground turn (typed or
    * voice) that is NOT on the durable observe path fans its tool steps LIVE to the user's OTHER
    * browsers viewing this conv, with NO conversation_events write — the messages table already
    * persists the step (tool-persist hook), so reload rebuilds it; the fan is pure live-view sugar.
    * The two are mutually exclusive (ev_observe XOR fan_ephemeral when ev_conv>0), so a step takes
    * exactly one path and jobs never double-emit. */
   const int64_t ev_conv = atomic_load(&s->stream_conversation_id);
   const int ev_user = (int)s->metrics.user_id;
   const bool ev_observe = ev_conv > 0 && atomic_load(&s->events_observable);
   const bool fan_ephemeral = ev_conv > 0 && !ev_observe;
   const bool ev_live = ev_observe || fan_ephemeral; /* either surface consumes tool events */

   /* Invariant guard: a foreground turn that promised full-turn
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
            const int64_t row_id = cb(ud, &row);
            if (row_ids) {
               row_ids[i] = row_id;
            }
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
            const int64_t row_id = cb(ud, &row);
            if (row_ids) {
               row_ids[i] = row_id;
            }
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
            const int64_t row_id = cb(ud, &row);
            if (row_ids) {
               row_ids[i] = row_id;
            }
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

   for (int i = 0; row_ids && i < n; i++) {
      stamp_result_part(s, owner[i], json_object_array_get_idx(canonical, i), row_ids[i]);
   }
   /* Last row first: a message holding several rows takes its last one's id
    * (a cursor or a compaction past it is past all of them). */
   for (int i = n - 1; row_ids && i >= 0; i--) {
      session_stamp_message_id(s, owner[i], row_ids[i]);
   }
   free(row_ids);
   free(owner);
   json_object_put(tcid_to_name);
   json_object_put(tcid_to_error);
   json_object_put(canonical);
}
