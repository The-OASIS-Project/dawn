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
 * Provider-agnostic text-input dispatch — Layer 2.  See
 * include/core/text_input_dispatch.h for the public contract.
 *
 * Extracted from src/webui/webui_text_processing.c to let non-WebUI
 * sources (the planned messaging-channels engine for Telegram / Discord /
 * Slack, and any future text input path) share the same add-history /
 * persist / focus-injection / LLM-call core without reaching upward into
 * Layer 4 WebUI code.
 */
#include "core/text_input_dispatch.h"

#include <json-c/json.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_messages.h"
#include "core/conv_event.h"
#include "core/event_payload.h"
#include "core/session_focus.h"
#include "core/session_manager.h"
#include "core/session_prefix.h"
#include "llm/llm_history_kind.h"
#include "logging.h"

char *core_text_input_dispatch(session_t *session,
                               const char *text,
                               const text_input_dispatch_opts_t *opts) {
   /* A turn with no words is one only when its question carries images (an
    * image-only turn: the caller builds the question message). */
   if (!session || !text || (text[0] == '\0' && !(opts && opts->question_message))) {
      return NULL;
   }

   /* Observe-side `status` (background-jobs Phase 2, §6.2).  Three terms, because
    * none alone covers the set: the session type catches a job worker, the
    * turn-source bit catches a reinvoke running on a viewer's own session, and
    * the conversation flag catches a user typing directly into a job
    * conversation from the sidebar — which is neither of the first two, yet is
    * exactly the turn someone tailing that job wants to see.
    *
    * The conversation is the one captured at DISPATCH, never the live view
    * (Phase 0's rule), so a background turn tags the right conversation even
    * while the user is looking at another one. */
   const bool emit_status = (session->type == SESSION_TYPE_JOB) ||
                            (opts && (opts->is_background_turn || opts->is_job_conversation));
   const int64_t status_conv = atomic_load(&session->stream_conversation_id);
   const int status_user = opts && opts->auth_user_id > 0 ? opts->auth_user_id
                                                          : (int)session->metrics.user_id;
   /* The tool loop emits tool_call/tool_result on this same turn; scope those to
    * the same observe set as the status heartbeat so ordinary interactive turns
    * don't pay the durable-log + fan-out tax (write-amplification under the global
    * auth_db lock).  Set every dispatch so it reflects THIS turn; the tool loop
    * runs synchronously within this turn on the same session. */
   atomic_store(&session->events_observable, emit_status);
   if (emit_status && status_conv > 0) {
      conv_event_emit(status_conv, status_user, CONV_EVENT_STATUS, event_payload_status(true));
   }

   /* Step 1: add user message to session history: the caller's prebuilt
    * message when it has one (an image question, built as a reload rebuilds
    * it), else the text. */
   /* The turn's conversation may only be resolved here (fallback to the active
    * one) rather than at dequeue; record it before the append attributes it. */
   if (opts && opts->is_background_turn) {
      session_turn_mark_background(session);
   }
   if (opts && opts->conversation_id > 0) {
      session_turn_set_conversation(session, opts->conversation_id, true);
   }
   const message_kind_t question_kind = opts ? opts->question_kind : MESSAGE_KIND_NONE;
   struct json_object *kinded_question = NULL; /* ref kept to stamp its row's id */
   if (question_kind != MESSAGE_KIND_NONE) {
      struct json_object *msg = json_object_new_object();
      if (msg) {
         json_object_object_add(msg, "role", json_object_new_string("user"));
         json_object_object_add(msg, "content", json_object_new_string(text));
         llm_history_set_kind(msg, question_kind);
         kinded_question = json_object_get(msg);
         if (!session_add_turn_message_object(session, msg)) {
            session_release_ref(session, kinded_question);
            kinded_question = NULL;
         }
      }
   } else if (opts && opts->question_message) {
      session_add_turn_message_object(session, json_object_get(opts->question_message));
   } else {
      session_add_turn_message(session, "user", text);
   }

   /* Step 2: persist to conv_db if requested.  The conv_db row ID is
    * stamped back into the in-memory history entry so subsequent
    * memory-extraction / context-injection code paths can reference it.
    *
    * Server-authoritative: EVERY user turn is persisted here (there is no
    * vision exception — the daemon owns user-turn persistence for all payload
    * shapes).  For an image turn the WebUI hands us `persist_content_override`
    * = `text` + `[IMAGE:<id>]` markers so the images re-render on reload; the
    * in-memory history (Step 1) and the transcript echo keep the clean `text`.
    * The persisted-vs-echoed split is deliberate: `persisted == true` makes the
    * echo carry server_saved=true, which is what tells the browser NOT to
    * double-save.  Non-WebUI callers (messaging) leave the override NULL and
    * persist plain text. */
   int64_t user_msg_id = 0;
   if (opts && opts->conversation_id <= 0 && opts->await_conversation) {
      /* A new chat's first message, its conversation created after it: the
       * turn's worker writes it once one is (session_turn_take_pending). */
      session_turn_set_pending(
          session, "user", opts->persist_content_override ? opts->persist_content_override : text);
   }
   if (opts && opts->conversation_id > 0 && kinded_question) {
      /* Request context: saved with its kind, its id on the message itself. */
      const conv_message_row_t row = { .role = "user",
                                       .content = text,
                                       .kind = message_kind_name(question_kind) };
      int64_t row_id = 0;
      if (conv_db_add_row(opts->conversation_id, opts->auth_user_id, &row, &row_id) ==
          AUTH_DB_SUCCESS) {
         session_stamp_message_id(session, kinded_question, row_id);
         session_prefix_question_saved(session, opts->conversation_id, opts->auth_user_id, row_id);
      } else {
         OLOG_WARNING("text_input_dispatch: a turn's %s wasn't saved to conv %lld",
                      message_kind_name(question_kind), (long long)opts->conversation_id);
      }
   } else if (opts && opts->conversation_id > 0 && question_kind == MESSAGE_KIND_NONE) {
      const char *persist_text = opts->persist_content_override ? opts->persist_content_override
                                                                : text;
      if (conv_db_add_message_ex(opts->conversation_id, opts->auth_user_id, "user", persist_text,
                                 &user_msg_id) == AUTH_DB_SUCCESS) {
         session_stamp_last_message_id(session, "user", user_msg_id);
         session_prefix_question_saved(session, opts->conversation_id, opts->auth_user_id,
                                       user_msg_id);
      } else {
         user_msg_id = 0;
      }
   }
   session_release_ref(session, kinded_question); /* a message of the history now */
   kinded_question = NULL;

   /* Step 3: fire the caller's user-msg-added hook BEFORE focus
    * injection + LLM call.  This preserves the existing WebUI
    * transcript-echo-before-LLM timing — the user sees their own
    * message render immediately while the server is still preparing
    * the response. */
   if (opts && opts->on_user_msg_added) {
      /* Hand the hook BOTH forms: clean `text` for the origin echo, and the persisted
       * marker-bearing form for the cross-viewer fan-out (so an image turn rehydrates on a
       * non-origin viewer live, not only on reload).  persist_text mirrors the Step-2 write. */
      const char *persist_text = opts->persist_content_override ? opts->persist_content_override
                                                                : text;
      opts->on_user_msg_added(opts->user_msg_added_ctx, text, persist_text, user_msg_id);
   }

   /* Step 4: per-turn focus injection (memory + entity + relation +
    * summary + document + calendar candidates ranked into the system
    * prompt for this turn only).  Returns SUCCESS even on focus
    * failure; LLM dispatch is never blocked by this.
    *
    * skip_prompt_rebuild bypasses the WHOLE rebuild (persona + memory + focus +
    * tools): the deep-research controller drives a bare session whose research
    * system prompt it set once, and skipping here is what structurally keeps
    * private memory out of the fetch loop (DEEP_RESEARCH_DESIGN.md §4a). */
   /* The per-turn citation stash MUST be clear before this turn's finalizer runs
    * memory_citation_capture — otherwise a stale [M#]->item_id map from a prior turn
    * false-validates this turn's <cited> tags and (with reinforcement on) credits the
    * bump to the wrong facts.  session_dispatch_user_turn clears it, but skip_prompt_rebuild
    * bypasses that call, so clear here UNCONDITIONALLY.  No live trigger today (the only
    * skip_prompt_rebuild caller is deep research, on a bare memory-free session), but this
    * disarms the trap for any future memory-surfacing skip_prompt_rebuild caller.  The clear
    * is idempotent, so the double-clear when the rebuild does run is harmless. */
   session_citation_stash_clear(session);

   /* An optional channel hint (e.g. the messaging engine for SMS: "user is on
    * SMS, outbound truncates at 670 chars") is a note for this turn only: it
    * varies turn to turn, so it goes in the turn's context, never the
    * conversation's system prompt. */
   if (!(opts && opts->skip_prompt_rebuild)) {
      /* What the prompt adds is saved with the question's row
       * (session_prefix_question_saved: above, or once a new chat's
       * conversation exists). */
      session_dispatch_user_turn_ex(session, text, opts ? opts->channel_hint : NULL);
   }

   /* Step 5: LLM call.  Uses the no_add variant because step 1
    * already added the user message; this also ensures the message
    * survives in history if the LLM call is interrupted or cancelled. */
   session_sentence_callback sentence_cb = opts ? opts->sentence_cb : NULL;
   void *sentence_userdata = opts ? opts->sentence_userdata : NULL;

   /* The images are in the question's own history message (Step 1): every
    * request of the turn, and every later one, sends them from there. */
   char *response = session_llm_call_with_tts_no_add(session, text, sentence_cb, sentence_userdata);

   /* Turn end.  This function has a single return, and the LLM call above is
    * synchronous, so one emit here covers success, cancellation and failure
    * alike — an `idle` placed in a worker's exit path instead would be missed
    * on whichever error branch someone later adds. */
   if (emit_status && status_conv > 0) {
      conv_event_emit(status_conv, status_user, CONV_EVENT_STATUS, event_payload_status(false));
   }

   return response;
}
