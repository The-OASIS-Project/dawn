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
 * WebUI text-input processing — async worker thread.
 *
 * Owns `webui_process_text_input_with_images` and `webui_process_text_input`,
 * the two public entry points used by the JSON-message dispatcher when the
 * client sends text (with or without attached images), plus the
 * `text_worker_thread` that actually drives the session through the LLM
 * call on a detached thread.  Split out of webui_server.c so that file can
 * stay under the size limits in CLAUDE.md.
 *
 * The moved block uses only the public session_t API, public webui_*
 * helpers (queue_response, webui_sentence_audio_callback, etc., declared in
 * webui_internal.h), and public llm_/conv_db_ interfaces — no file-static
 * state from webui_server.c is touched here.
 */

#include <json-c/json.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_messages.h"
#include "config/dawn_config.h"
#include "core/conv_event.h"
#include "core/image_rehydrate.h"
#include "core/session_manager.h"
#include "core/session_prefix.h"
#include "core/text_input_dispatch.h"
#include "core/turn_queue.h"
#include "core/worker_pool.h"
#include "dawn.h"
#include "image_store.h"
#include "llm/llm_context.h"
#include "llm/llm_context_text.h"
#include "llm/llm_turn_blocks.h"
#include "logging.h"
#include "utils/string_utils.h"
#include "webui/webui_internal.h"
#include "webui/webui_server.h"

/* =============================================================================
 * Text Processing (Async Worker Thread)
 *
 * For Phase 2, we use a simple detached thread for text processing.
 * Phase 4 may integrate with the worker pool for audio + text.
 * ============================================================================= */

typedef struct {
   session_t *session;
   char *text;
   int64_t conv_id;          /* Conversation this turn was sent for, captured at enqueue and
                              * applied to session->stream_conversation_id AT DEQUEUE (a queued
                              * turn must not clobber a running turn's conversation). */
   unsigned int request_gen; /* Captured request_generation to detect superseded requests */
   bool input_was_voice;     /* True = this turn's input was ASR-transcribed (voice); applied
                              * to session->input_was_voice on the worker thread right before
                              * dispatch so the prompt builder gates the ASR hint per turn. */
   /* The images the turn was sent with, by id (the stored files are read on
    * the worker); none for a text turn. */
   char image_ids[WEBUI_MAX_VISION_IMAGES_CAP][IMAGE_ID_LEN];
   int image_id_count;
   char *persist_content; /* Server-authoritative persisted form (text + [IMAGE:<id>] markers)
                           * for an image turn; NULL persists plain text. Owned/freed here. */
   char client_ref[WEBUI_CLIENT_REF_MAX + 1]; /* the frame's client_ref ("" = none): the
                                               * worker's turn ref while the turn runs */
   bool from_visual; /* the text came from a rendered visual's prompt, not the person */
} text_work_t;

/* REQUEST_SUPERSEDED macro now defined in webui_internal.h */

void webui_image_error_describe(int rc, const char **code_out, const char **message_out) {
   switch (rc) {
      case IMAGE_REHYDRATE_ERR_NOT_FOUND:
         *code_out = WEBUI_ERR_IMAGE_UNAVAILABLE;
         *message_out = "An attached image isn't available (deleted, or not yours). "
                        "Attach it again.";
         break;
      case IMAGE_REHYDRATE_ERR_LIMIT:
         *code_out = WEBUI_ERR_IMAGE_LIMIT;
         *message_out = "Too many or too large images for one message.";
         break;
      default:
         *code_out = WEBUI_ERR_IMAGE_ERROR;
         *message_out = "The attached images couldn't be prepared. Try again.";
         break;
   }
}

/* A turn's images refused (image_rehydrate_question's @p rc), to its client. */
static void webui_send_image_error(session_t *session, int rc) {
   const char *code = NULL;
   const char *message = NULL;
   webui_image_error_describe(rc, &code, &message);
   webui_send_error(session, code, message);
}

/* A new chat's first message is dispatched before its conversation exists, so
 * it has no row yet.  Once the conversation is created for it
 * (session_bind_created_conversation), write it here, on the turn's own thread,
 * before anything else of the turn, so the user row precedes the reply. */
/* Write an earlier turn's exchange this turn adopted (it ended before the
 * conversation existed), ahead of this turn's own rows, the reply with the
 * blocks it was captured with. */
static void write_prior_exchange(session_t *session,
                                 int user_id,
                                 int64_t conv_id,
                                 const char *user,
                                 const char *reply,
                                 struct json_object *reply_blocks) {
   int64_t user_row = 0;
   int64_t reply_row = 0;
   if (user_id > 0 && user) {
      (void)conv_db_add_message_ex(conv_id, user_id, "user", user, &user_row);
   }
   if (user_id > 0 && reply) {
      char *stored = llm_turn_blocks_answer_stored(reply_blocks);
      const conv_message_row_t row = { .role = "assistant",
                                       .content = reply,
                                       .llm_blocks = stored };
      (void)conv_db_add_row(conv_id, user_id, &row, &reply_row);
      free(stored);
   }
   session_stamp_claimed(session, user_row, reply_row);
   /* Its request context goes with its own question. */
   if (user_row > 0) {
      session_prefix_question_saved(session, conv_id, user_id, user_row);
   }
}

/* A new chat's first message is dispatched before its conversation exists, so
 * it has no row yet.  Once the conversation is created for it
 * (session_bind_created_conversation), write it here, on the turn's own thread,
 * before anything else of the turn, so the user row precedes the reply: after
 * any earlier exchange the turn adopted with the conversation. */
static void persist_pending_user_row(session_t *session, int user_id) {
   int64_t conv_id = 0;
   char *prior_user = NULL;
   char *prior_reply = NULL;
   struct json_object *prior_blocks = NULL;
   if (session_turn_take_prior(session, &conv_id, &prior_user, &prior_reply, &prior_blocks)) {
      write_prior_exchange(session, user_id, conv_id, prior_user, prior_reply, prior_blocks);
   }
   free(prior_user);
   free(prior_reply);
   json_object_put(prior_blocks);
   char *pending = session_turn_take_pending(session, "user", &conv_id);
   if (!pending) {
      return;
   }
   int64_t msg_id = 0;
   if (user_id > 0 &&
       conv_db_add_message_ex(conv_id, user_id, "user", pending, &msg_id) == AUTH_DB_SUCCESS) {
      session_stamp_last_message_id(session, "user", msg_id);
      session_prefix_question_saved(session, conv_id, user_id, msg_id);
   } else {
      OLOG_WARNING("WebUI: could not save the first message of conversation %lld",
                   (long long)conv_id);
   }
   free(pending);
}

/* Context + callback for the LLM tool loop's structured tool-turn persistence (E2).
 * The daemon owns the structured tool data (the browser only saves the final visible
 * assistant text); this writes the assistant tool_calls + role:tool rows to conv_db.
 *
 * Holds the retained `session` (NOT a raw `conn`): the hook fires seconds into the turn
 * when tools resolve, and a client disconnect on the lws thread frees `conn` after
 * LWS_CALLBACK_CLOSED — so caching `conn` here would be a use-after-free.  The worker
 * already holds a session reference for the whole turn, so the session is lifetime-safe.
 * The conv_id is read live via webui_get_active_conversation_id() (which goes through
 * session->client_data, NULLed on disconnect) because a brand-new chat's conversation is
 * created a moment AFTER the turn starts (the browser's new_conversation message), still
 * well before the LLM emits tool calls; a setup-time snapshot would be 0.  auth_user_id is
 * stable for the connection, so it is snapshotted at setup.  (webui_tool_persist_ctx_t
 * is declared in webui_internal.h so the shared arm/disarm helper + the voice path can
 * reference it.) */
static int64_t webui_tool_persist_cb(void *userdata, const session_tool_row_t *row) {
   webui_tool_persist_ctx_t *ctx = (webui_tool_persist_ctx_t *)userdata;
   if (!ctx || !ctx->session || !row || !row->role) {
      return 0;
   }
   /* Persist to the conversation THIS TURN belongs to (captured at dispatch /
    * back-filled at conversation creation), NOT the live view — otherwise
    * switching conversations mid-tool-loop writes this turn's iteration rows into
    * whatever conversation is on screen.  No live-view fallback: an untagged turn
    * (conv_id 0) is skipped rather than mis-attributed. */
   int64_t conv_id = ctx->session->stream_conversation_id;
   if (conv_id <= 0) {
      return 0; /* turn not tagged with a conversation — skip rather than guess */
   }
   persist_pending_user_row(ctx->session, ctx->auth_user_id);
   const conv_message_row_t db_row = { .role = row->role,
                                       .content = row->content ? row->content : "",
                                       .tool_calls = row->tool_calls,
                                       .tool_call_id = row->tool_call_id,
                                       .reasoning = row->reasoning,
                                       .llm_blocks = row->llm_blocks,
                                       .kind = row->kind,
                                       .is_error = row->is_error,
                                       .images = row->images };
   int64_t id = 0;
   if (conv_db_add_row(conv_id, ctx->auth_user_id, &db_row, &id) != AUTH_DB_SUCCESS) {
      OLOG_WARNING("WebUI: failed to persist tool-turn %s row to conv %lld (may orphan on reload)",
                   row->role, (long long)conv_id);
      return 0;
   }
   return id;
}

/* Iteration-boundary hook: close the current streaming bubble (if one is open) so this
 * iteration's tool entries render after its text and the next iteration opens a fresh
 * bubble.  reason="tool_iteration" tells the browser to seal the bubble WITHOUT going
 * idle or saving it (only the final stream_end does that).  Must stay in sync with the
 * matching reason string in www/js/ui/streaming.js.
 *
 * Not static: a background job's worker installs this same hook so a viewed job's live
 * stream seals its bubble at each tool boundary (else the final answer renders in the
 * still-open bubble above the last tool).  For a job session webui_send_stream_end fans
 * out to the viewers (webui_fanout_job_stream_response) instead of one connection. */
void webui_tool_iteration_cb(session_t *session, void *userdata) {
   (void)userdata;
   if (session && atomic_load(&session->llm_streaming_active)) {
      webui_send_stream_end(session, "tool_iteration");
   }
}

/* Arm the WebUI per-turn persistence contract as ONE unit: the tool-persist hook
 * (writes assistant tool_calls + role:tool rows), the tool-iteration hook (seals the
 * streaming bubble per iteration), and will_persist_turn (the final-reply stand-down
 * promise).  BOTH WebUI foreground surfaces — typed (this file) and voice
 * (webui_audio.c) — route through this so neither can wire half the contract (the gap
 * that silently dropped voice tool-turns).  `scope` owns the stack-lifetime persist_ctx
 * the hook references and MUST outlive the synchronous LLM dispatch; the hook reads the
 * conversation id LIVE from session->stream_conversation_id, so bind the conversation
 * BEFORE arming.  Captures only session + auth_user_id — never `conn` (freed on a
 * mid-turn disconnect).  Arms nothing when auth_user_id <= 0, so will_persist_turn never
 * promises a persistence the server then can't deliver (rows are user-owned).
 *
 * NOT used by jobs (job_worker/job_reinvoke install their own persist cb with a snapshot
 * conv_id and deliberately do NOT arm will_persist_turn — a job reply persists via its
 * tail + durable conv_event log), nor by messaging/research (they persist inline / to
 * their own ledgers).  Do not "consistency-fix" those onto this helper. */
void webui_turn_persist_arm(session_t *session,
                            int auth_user_id,
                            webui_turn_persist_scope_t *scope) {
   if (!session || !scope) {
      return;
   }
   scope->persist_ctx.session = session;
   scope->persist_ctx.auth_user_id = auth_user_id;
   if (auth_user_id <= 0) {
      return; /* no user → nothing persistable; arm nothing (incl. will_persist_turn) */
   }
   session_set_tool_persist_hook(session, webui_tool_persist_cb, &scope->persist_ctx);
   session_set_tool_iteration_hook(session, webui_tool_iteration_cb, NULL);
   atomic_store(&session->will_persist_turn, true);
}

/* Symmetric teardown; safe/idempotent even if arm installed nothing. */
void webui_turn_persist_disarm(session_t *session, webui_turn_persist_scope_t *scope) {
   (void)scope; /* persist_ctx is cleared implicitly when it goes out of scope in the caller */
   if (!session) {
      return;
   }
   atomic_store(&session->will_persist_turn, false);
   session_set_tool_persist_hook(session, NULL, NULL);
   session_set_tool_iteration_hook(session, NULL, NULL);
}

/* Callback fired by core_text_input_dispatch after the user message is
 * added to history + (optionally) persisted to conv_db, but before
 * focus injection and the LLM call.  Preserves the WebUI's "transcript
 * echoes immediately after the user types" UX while keeping the
 * add/persist/focus/LLM sequence inside the Layer 2 helper. */
static void webui_text_dispatch_on_user_msg(void *ctx,
                                            const char *text,
                                            const char *persist_text,
                                            int64_t message_id) {
   session_t *session = (session_t *)ctx;
   if (!session || !text) {
      return;
   }
   /* Echo to the origin with the CLEAN text — the origin already rendered its own image
    * locally at upload time, so the echo mirrors what the user typed.  Carries the DB row
    * id so the origin stamps data-message-id on its user bubble (and dedups its fan-out copy). */
   webui_send_transcript_ex(session, "user", text, message_id > 0, message_id);
   /* Fan the user message out to every OTHER viewer of this conversation so a
    * second open client sees the question, not just the answer (§12c).  Use the PERSISTED
    * form (persist_text) — for an image turn that carries the `[IMAGE:<id>]` markers, so a
    * non-origin viewer rehydrates the image LIVE (its addNormalEntry parses the marker and
    * fetches /api/images/<id>), not only on reload.  The origin's own copy dedups on
    * message_id via the echo above.  User messages are never streamed → stream_id 0.
    * INVARIANT (mirrored in webui_audio.c): guard on message_id > 0 so a user turn
    * only ever emits TWO frames (echo + fan-out) when they carry the SAME positive
    * id — a save failure (id 0) emits only the echo, never an undedup-able double. */
   if (message_id > 0) {
      ws_connection_t *conn = (ws_connection_t *)session->client_data;
      int64_t conv_id = atomic_load(&session->stream_conversation_id);
      const char *body = persist_text ? persist_text : text;
      if (conn && conn->auth_user_id > 0 && conv_id > 0 && body[0]) {
         /* The sender's copy names its turn too, in case its echo was dropped. */
         webui_broadcast_message_appended_origin(conn->auth_user_id, conv_id, message_id, "user",
                                                 body, NULL, 0, session, webui_turn_ref_get());
      }
   }
}

/* Balanced exit for a worker that has passed its initial supersede check and
 * incremented turn_in_flight: clear the in-flight guard, then release the ref.
 * Decrement BEFORE release so that if this release drops ref_count to 0 and a
 * concurrent session_destroy proceeds, turn_in_flight already reads 0. */
static void text_worker_persist_final(session_t *session,
                                      int64_t turn_conv,
                                      int turn_user_id,
                                      const char *body);

/* End the turn, first writing what it couldn't save yet while it is still open
 * (so the rows' ids land on its own history): an earlier exchange it adopted,
 * its question, its reply.  Deciding and ending happen together: a conversation
 * created for it at the last moment is either seen here or handed to
 * session_bind_created_conversation, never lost between the two.  Every exit of
 * a begun text turn goes through here. */
static void finish_turn(session_t *session) {
   const int user_id = (int)session->metrics.user_id;
   session_turn_unsaved_t unsaved;
   while (session_turn_finish(session, &unsaved) == SESSION_TURN_WRITE_UNSAVED) {
      if (unsaved.prior_user || unsaved.prior_reply) {
         write_prior_exchange(session, user_id, unsaved.conv, unsaved.prior_user,
                              unsaved.prior_reply, unsaved.prior_reply_blocks);
      }
      if (user_id > 0) {
         int64_t msg_id = 0;
         if (unsaved.user && conv_db_add_message_ex(unsaved.conv, user_id, "user", unsaved.user,
                                                    &msg_id) == AUTH_DB_SUCCESS) {
            session_stamp_last_message_id(session, "user", msg_id);
            session_prefix_question_saved(session, unsaved.conv, user_id, msg_id);
         }
         if (unsaved.reply) {
            text_worker_persist_final(session, unsaved.conv, user_id, unsaved.reply);
         }
      }
      free(unsaved.prior_user);
      free(unsaved.prior_reply);
      json_object_put(unsaved.prior_reply_blocks);
      free(unsaved.user);
      free(unsaved.reply);
   }
}

static void text_worker_end(session_t *session) {
   if (session) {
      /* Close the multi-target TTS bracket on non-origin listeners (§Phase-4) BEFORE releasing
       * the turn ref — this teardown is the single funnel every text-worker exit passes through,
       * so a fanned bystander always returns to idle regardless of which exit ran. No-op when the
       * turn fanned to no one. */
      webui_fanout_tts_idle(session);
      finish_turn(session);
      atomic_fetch_sub(&session->turn_in_flight, 1);
      session_release(session);
   }
}

static void text_worker_cleanup(text_work_t *work, session_t *session, char *text) {
   if (session) {
      session_release(session);
   }
   if (text) {
      free(text);
   }
   if (work) {
      free(work->persist_content); /* separate alloc from `text` (work->text alias) */
      free(work);
   }
}

/* 2b: the foreground server-authoritative final-answer persist.  Wraps the shared
 * webui_persist_final_answer (which does the splice + row + retention + id-stamp + fan-out
 * with a bounded DB retry) and adds the foreground-only "tell the user it failed" frame on
 * a hard failure — the reply is on screen, but the row didn't save.  turn_conv/turn_user_id
 * are the pre-dispatch, disconnect-safe captures — NEVER conn. */
static void text_worker_persist_final(session_t *session,
                                      int64_t turn_conv,
                                      int turn_user_id,
                                      const char *body) {
   persist_pending_user_row(session, turn_user_id);
   if (body == NULL || body[0] == '\0') {
      return;
   }
   if (turn_conv <= 0) {
      /* The conversation isn't created yet: kept until it is (the turn's end
       * or the handler creating it writes it). */
      session_turn_set_pending(session, "assistant", body);
      return;
   }
   if (turn_user_id <= 0) {
      return;
   }
   if (webui_persist_final_answer(session, turn_conv, turn_user_id, body, NULL) !=
       AUTH_DB_SUCCESS) {
      OLOG_ERROR("WebUI: failed to persist final answer to conv %lld after retries",
                 (long long)turn_conv);
      webui_send_error(session, "PERSIST_ERROR", "Your reply was shown but could not be saved.");
   }
}

static void *text_worker_thread(void *arg) {
   text_work_t *work = (text_work_t *)arg;
   session_t *session = work->session;
   char *text = work->text;
   unsigned int expected_gen = work->request_gen;

   /* Check if session is still valid or if this request was superseded */
   if (!session || REQUEST_SUPERSEDED(session, expected_gen)) {
      OLOG_INFO("WebUI: Session disconnected or request superseded, aborting text processing");
      if (session) {
         finish_turn(session); /* begun at dequeue; never ran */
      }
      text_worker_cleanup(work, session, text);
      return NULL;
   }

   /* An attached document's contents came from anywhere: DAWN's markers in
    * them are defused here, on the turn's thread (the user's own words are
    * kept as written).  The text and its persisted form get the same
    * treatment, so the question, the saved row and a reload agree. */
   char *defused = llm_context_neutralize_attachments(text);
   char *defused_persist = work->persist_content
                               ? llm_context_neutralize_attachments(work->persist_content)
                               : NULL;
   if (!defused || (work->persist_content && !defused_persist)) {
      OLOG_ERROR("WebUI: out of memory preparing a turn's attached documents");
      free(defused);
      free(defused_persist);
      webui_send_error(session, "PROCESSING_ERROR", "Your message couldn't be prepared.");
      finish_turn(session);
      text_worker_cleanup(work, session, text);
      return NULL;
   }
   free(text);
   work->text = text = defused;
   if (defused_persist) {
      free(work->persist_content);
      work->persist_content = defused_persist;
   }

   /* Mark a turn in flight so session_cleanup_expired won't reap this session
    * out from under us while it generates (a disconnected client no longer
    * aborts the turn — background-jobs Phase 1).  Cleared by text_worker_end()
    * at every subsequent exit. */
   atomic_fetch_add(&session->turn_in_flight, 1);

   OLOG_INFO("WebUI: Processing text input for session %u: %zu bytes (%d image(s))",
             session->session_id, strlen(text), work->image_id_count);

   /* Clear stale pending_visual from a previous turn that may not have
    * been consumed (e.g., LLM response interrupted or error). Prevents
    * visuals from attaching to the wrong assistant message. */
   pthread_mutex_lock(&session->tools_mutex);
   free(session->pending_visual);
   session->pending_visual = NULL;
   pthread_mutex_unlock(&session->tools_mutex);

   /* Send "thinking" state */
   if (work->image_id_count > 0) {
      if (work->image_id_count == 1) {
         webui_send_state_with_detail(session, "thinking", "Analyzing image...");
      } else {
         webui_send_state_with_detail(session, "thinking", "Analyzing images...");
      }
   } else {
      webui_send_state_with_detail(session, "thinking", "Processing request...");
   }

   ws_connection_t *conn = (ws_connection_t *)session->client_data;
   /* Capture everything we need from conn NOW, while it is valid.  Post
    * background-jobs Phase 1 the turn survives a client disconnect, and on WS
    * close libwebsockets frees `conn` right after the CLOSED callback — so the
    * worker tail (after the long LLM call) must NOT dereference conn again.
    * user_id falls back to the authenticated session user (metrics.user_id),
    * which is stable and disconnect-safe, for server-authoritative persistence. */
   bool tts_enabled = conn && conn->tts_enabled;
   bool use_opus = conn && conn->use_opus;
   int turn_user_id = conn ? conn->auth_user_id : (int)session->metrics.user_id;

   /* Stamp this turn's input modality (voice vs typed) onto the session right
    * before dispatch, on THIS worker thread — same pattern as tts_enabled above.
    * Setting it at the entry point (LWS/always-on thread) instead left a window
    * where a concurrent always-on voice turn could clobber a typed turn's reset;
    * stamping it here, adjacent to the synchronous build, closes that window. The
    * prompt builder reads session->input_was_voice to gate the ASR hint. */
   session->input_was_voice = work->input_was_voice;

   /* An image question goes into the history exactly as a reload rebuilds it,
    * from the stored files its ids name, or not at all: an id that names no
    * image of this user's (or OOM) fails the turn back to the client, with
    * nothing added to the history or saved. */
   struct json_object *question = NULL;
   if (work->image_id_count > 0) {
      const int qrc = image_rehydrate_question(turn_user_id, work->persist_content,
                                               (const char(*)[IMAGE_ID_LEN])work->image_ids,
                                               work->image_id_count, &question);
      if (qrc != SUCCESS) {
         OLOG_WARNING("WebUI: session %u: image question refused (%d), %d image(s)",
                      session->session_id, qrc, work->image_id_count);
         webui_send_image_error(session, qrc);
         webui_send_state(session, "idle");
         text_worker_end(session);
         free(text);
         free(work->persist_content);
         free(work);
         return NULL;
      }
   }

   /* Dispatch the turn through the provider-agnostic Layer 2 helper:
    * add user msg → persist to conv_db → transcript echo (via callback)
    * → focus injection → LLM call. */
   /* Use the turn's CAPTURED conversation (set at dispatch, back-filled at
    * creation) rather than a live re-read of conn->active_conversation_id, which
    * this worker could observe AFTER a mid-turn view switch — that would persist
    * the user message / inject focus for the wrong conversation. */
   /* A new chat's first message may have none yet: the conversation the client
    * creates for it is adopted (session_bind_created_conversation) and its
    * messages are written then, never guessed from whatever is on screen. */
   int64_t turn_conv = session->stream_conversation_id;

   /* Multi-target TTS (SERVER_AUTHORITATIVE_PERSISTENCE §Phase-4): arm synthesis when the origin
    * has TTS on OR any OTHER speaker-capable viewer of this conversation exists, so a silent-origin
    * turn still speaks on a remote listener's device.  Keeping origin.tts_enabled as an independent
    * sufficient condition preserves origin-speaks even before turn_conv is bound (arch HIGH-2). The
    * fanout callback synthesizes once and fans to every speaker viewer (the origin included). */
   bool fanout_tts = tts_enabled ||
                     webui_audio_has_other_speaker(turn_user_id, turn_conv, session->session_id);
   text_input_dispatch_opts_t dispatch_opts = {
      .conversation_id = turn_conv,
      .auth_user_id = conn ? conn->auth_user_id : 0,
      .persist_content_override = work->persist_content, /* text + [IMAGE:<id>] for image turns */
      .question_message = question,                      /* ...as a reload rebuilds it */
      .await_conversation = turn_conv <= 0,
      .sentence_cb = fanout_tts ? webui_sentence_audio_fanout_callback : NULL,
      .sentence_userdata = fanout_tts ? session : NULL,
      .on_user_msg_added = webui_text_dispatch_on_user_msg,
      .user_msg_added_ctx = session,
      .from_visual = work->from_visual,
   };

   /* Clear the per-turn error flag before the call; the provider layer sets it via
    * webui_send_error_ex if it emits a specific error, which we then honor below to
    * skip the redundant generic fallback. */
   atomic_store(&session->turn_error_emitted, false);

   /* Arm the WebUI per-turn persistence contract (tool-persist hook + tool-iteration
    * hook + will_persist_turn) as one unit — same helper the voice worker uses, so a
    * path can't wire half of it.  `persist_scope` must outlive the synchronous dispatch;
    * the hook reads the conversation id live (may still be 0 here for a brand-new chat,
    * but is set before tools fire).  session_begin_turn_flags at dequeue reset
    * will_persist_turn before this worker ran, so reset-then-arm holds. */
   webui_turn_persist_scope_t persist_scope;
   webui_turn_persist_arm(session, conn ? conn->auth_user_id : 0, &persist_scope);
   char *response = core_text_input_dispatch(session, text, &dispatch_opts);
   webui_turn_persist_disarm(session, &persist_scope);
   json_object_put(question); /* the history holds its own reference */
   question = NULL;

   /* Promote the persisted image turn's images to permanent retention — AFTER
    * dispatch persisted the row, so images are pinned only for turns that reached
    * here (queue-full/superseded-before-dequeue rejections never do).  Images have
    * no orphan sweep and PERMANENT is LRU-exempt, so pinning a never-persisted turn
    * would leak forever.  The ids are exactly those the row's markers name (built
    * from them), and the owner is the disconnect-safe turn user, never conn (which
    * libwebsockets may have freed during the call). */
   for (int i = 0; turn_user_id > 0 && i < work->image_id_count; i++) {
      image_store_update_retention(work->image_ids[i], turn_user_id, IMAGE_RETAIN_PERMANENT);
   }

   /* Check if request was superseded during LLM call */
   if (REQUEST_SUPERSEDED(session, expected_gen)) {
      /* Request superseded - don't send the response live.  G4 (SERVER_AUTHORITATIVE §9):
       * but if this turn made a will_persist promise and the reply COMPLETED, the browser
       * already stood down, so persist it before discarding — the in-hand `response`, or the
       * cancel-at-buzzer stash if dispatch already freed it (cancelled-THEN-superseded, the
       * cross case).  current_stream_id is still THIS turn's (the superseding turn is queued,
       * not yet streaming), so the helper's fan-out stamps correctly. */
      OLOG_INFO("WebUI: Session %u request superseded during LLM call", session->session_id);
      char *superseded_reply = response;
      if (superseded_reply == NULL) {
         superseded_reply = session->cancelled_final_response;
         session->cancelled_final_response = NULL;
      }
      if (superseded_reply != NULL && superseded_reply[0] != '\0') {
         text_worker_persist_final(session, session->stream_conversation_id, turn_user_id,
                                   superseded_reply);
      }
      text_worker_end(session);
      free(superseded_reply); /* frees response OR the taken stash (mutually exclusive) */
      free(text);
      free(work->persist_content);
      free(work);
      return NULL;
   }

   if (!response) {
      /* G4 cancel-at-buzzer (SERVER_AUTHORITATIVE §9): a cancel that landed AFTER the reply
       * completed stashed the finalized text and returned NULL — and will_persist was already
       * stamped on the final stream_end, so the browser stood down.  Recover + persist it here,
       * BEFORE any error path, rather than showing a spurious "Failed to get response" for a
       * reply the user just saw/heard. */
      char *stashed = session->cancelled_final_response;
      session->cancelled_final_response = NULL;
      if (stashed != NULL && stashed[0] != '\0') {
         text_worker_persist_final(session, session->stream_conversation_id, turn_user_id, stashed);
         free(stashed);
         webui_send_state(session, "idle");
         text_worker_end(session);
         free(text);
         free(work->persist_content);
         free(work);
         return NULL;
      }
      free(stashed); /* NULL-safe; no completed reply to recover */

      /* Genuine failure.  Emit the generic error ONLY if the provider layer did
       * not already surface a specific one for this turn — otherwise the user
       * sees the same failure twice (the precise message, then this fallback). */
      if (!atomic_load(&session->turn_error_emitted)) {
         webui_send_error(session, "LLM_ERROR", "Failed to get response from AI");
      }
      webui_send_state(session, "idle");
      text_worker_end(session);
      free(text);
      free(work->persist_content);
      free(work);
      return NULL;
   }

   /* Response is already canonical clean text (finalized centrally in
    * llm_call_finalize).  Keep the `final_response` alias for the block below. */
   char *final_response = response;

   /* Send audio end marker if TTS was enabled (use_opus captured at worker
    * start — conn may be freed by now if the client disconnected mid-turn). */
   if (tts_enabled) {
      webui_send_audio_end(session, use_opus);
   }

   /* Note: Don't send transcript here - streaming already delivered the content.
    * The LLM call uses webui_send_stream_start/delta/end for real-time delivery.
    * Assistant response is already added to history inside the LLM call. */

   /* Server-authoritative persistence (SERVER_AUTHORITATIVE Phase 2b-i): the SERVER is now
    * the single writer of every foreground reply — persist UNCONDITIONALLY (was gated on
    * backgrounded/client-gone in Phase 1).  will_persist was stamped on the final stream_end,
    * so the browser stood down; no duplicate row.  Uses turn_user_id + the captured
    * conversation — NEVER conn, which libwebsockets may have freed after a mid-turn
    * disconnect.  Tool-turn rows were already persisted server-side by webui_tool_persist_cb;
    * the helper attaches the final-answer reasoning + accumulated visual, promotes reply-body
    * images, stamps the history id, and fans out message_appended. */
   if (final_response && final_response[0] != '\0') {
      int64_t turn_conv = session->stream_conversation_id;
      text_worker_persist_final(session, turn_conv, turn_user_id, final_response);
   }

   /* Free the final response (either original response or processed copy) */
   free(final_response);

   /* Send context usage update to WebUI */
   {
      int current_tokens, max_tokens;
      float threshold;
      llm_context_get_last_usage(&current_tokens, &max_tokens, &threshold);
      if (max_tokens > 0) {
         webui_send_context(session, current_tokens, max_tokens, threshold);
      }
   }

   /* Return to idle state */
   webui_send_state(session, "idle");

   /* Mark interaction complete for conversation idle timeout tracking */
   session_update_interaction_complete(session);

   /* Release session reference (acquired in webui_process_text_input) + clear
    * the turn-in-flight guard (balanced with the increment after the initial
    * supersede check). */
   text_worker_end(session);

   free(text);
   free(work->persist_content);
   free(work);
   return NULL;
}

/* =============================================================================
 * Turn-queue integration (P1): the queue serializes turn spawning per session.
 * ============================================================================= */

/* free_work closure: drop a turn WITHOUT running it (bound-reject or purge).
 * Mirrors the run-path cleanup (release the session retain + free the work). */
static void webui_text_turn_free(void *work) {
   text_work_t *w = (text_work_t *)work;
   if (w == NULL) {
      return;
   }
   if (w->session != NULL) {
      session_release(w->session);
   }
   free(w->text);
   free(w->persist_content);
   free(w);
}

/* Worker entry for a queued turn.  Binds this turn's conversation + clears the
 * turn flags AT DEQUEUE (never at enqueue — a queued turn must not disturb the
 * running turn's binding/flags), runs the existing turn body, then chains the
 * next queued turn for this session. */
static void *text_turn_thread_entry(void *arg) {
   text_work_t *work = (text_work_t *)arg;
   uint32_t sid = 0;
   if (work != NULL && work->session != NULL) {
      sid = work->session->session_id;
      if (atomic_load(&work->session->being_destroyed)) {
         /* Session began teardown after this turn was popped-to-spawn (C1): do
          * NOT clear its flags or run — teardown's cancel must stand.  Clean up
          * and let the queue drain. */
         webui_text_turn_free(work);
         turn_queue_turn_done(sid);
         return NULL;
      }
      session_turn_begin(work->session, work->conv_id, (int)work->session->metrics.user_id);
      if (work->conv_id <= 0) {
         /* A new chat's first message: its conversation is created after it. */
         session_turn_await_conversation(work->session);
      }
      session_begin_turn_flags(work->session); /* fresh flags for THIS turn (G2) */
   }
   /* The turn's errors and its user echo, raised on this thread, name it. */
   webui_turn_ref_set(work != NULL ? work->client_ref : NULL);
   text_worker_thread(work); /* existing turn body — frees work, releases session */
   webui_turn_ref_set(NULL);
   turn_queue_turn_done(sid); /* chain the next queued turn for this session */
   return NULL;
}

/* spawn closure: start the turn worker for a dequeued turn. */
static void webui_text_turn_spawn(void *work) {
   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   int ret = pthread_create(&thread, &attr, text_turn_thread_entry, work);
   pthread_attr_destroy(&attr);
   if (ret != 0) {
      /* Spawn failed: free this turn + advance the queue so it can't wedge. */
      text_work_t *w = (text_work_t *)work;
      uint32_t sid = (w != NULL && w->session != NULL) ? w->session->session_id : 0;
      OLOG_ERROR("WebUI: failed to spawn queued text turn worker; dropping it");
      if (w != NULL && w->session != NULL) {
         /* The client is told which turn didn't run (spawned from any thread:
          * this thread's own ref is put back after). */
         char prev[WEBUI_CLIENT_REF_MAX + 1];
         snprintf(prev, sizeof(prev), "%s", webui_turn_ref_get() ? webui_turn_ref_get() : "");
         webui_turn_ref_set(w->client_ref);
         webui_send_error(w->session, "PROCESSING_ERROR", "Your message couldn't be started.");
         webui_turn_ref_set(prev);
      }
      webui_text_turn_free(work);
      turn_queue_turn_done(sid);
   }
}

int webui_process_text_input_with_images(session_t *session,
                                         const char *text,
                                         const char image_ids[][IMAGE_ID_LEN],
                                         int image_id_count,
                                         const char *persist_content,
                                         bool input_was_voice) {
   /* No words is a turn only when it carries images. */
   if (!session || !text || (text[0] == '\0' && image_id_count <= 0)) {
      return 1;
   }
   /* The caller refuses more (an image is never silently dropped); this guards
    * the array. */
   if (image_id_count < 0 || image_id_count > WEBUI_MAX_VISION_IMAGES_CAP ||
       (image_id_count > 0 && (!image_ids || !persist_content))) {
      return 1;
   }

   /* Turn queue (P1): this turn is SERIALIZED behind any turn already in flight
    * for this session, so we do NOT bump request_generation (that would supersede
    * an in-flight turn and break strict-FIFO ordering) and we do NOT clear the
    * turn flags or bind stream_conversation_id here — both happen AT DEQUEUE (in
    * text_turn_thread_entry), when this turn actually starts, so a queued turn
    * never disturbs the running turn's flags or conversation binding.  Supersede
    * is replaced by the queue; Stop still works via cancel_requested. */
   unsigned int new_gen = atomic_load(&session->request_generation);

   /* Voice turns are server-dispatched, so the browser never ran its conversation
    * pre-create.  Bind (lazily creating) a conversation here — titled from the
    * transcript — or this turn's user row + reply evaporate on the next reload.
    * No-op when a conversation is already selected; leaves the typed-text path
    * (client pre-creates) untouched. */
   if (input_was_voice && webui_voice_transcript_substantive(text)) {
      ws_connection_t *vconn = session ? (ws_connection_t *)session->client_data : NULL;
      if (vconn && vconn->active_conversation_id <= 0) {
         webui_ensure_active_conversation(vconn, text);
      }
   }

   /* Conversation this turn was sent for — captured NOW (the viewed conversation)
    * and applied to session->stream_conversation_id at dequeue. */
   int64_t turn_conv_id = webui_get_active_conversation_id(session);

   /* Create work item */
   text_work_t *work = calloc(1, sizeof(text_work_t));
   if (!work) {
      OLOG_ERROR("WebUI: Failed to allocate text work item");
      return 1;
   }

   work->session = session;
   /* Attached documents are defused when the turn runs, on its own thread
    * (text_worker_thread), not on this one: a turn's documents can be MBs. */
   work->text = strdup(text);
   work->conv_id = turn_conv_id;
   work->request_gen = new_gen; /* current gen; supersede is disabled under the queue */
   work->input_was_voice = input_was_voice;
   if (!work->text) {
      OLOG_ERROR("WebUI: Failed to allocate text copy");
      free(work);
      return 1;
   }
   if (persist_content) {
      work->persist_content = strdup(persist_content);
      if (!work->persist_content) {
         OLOG_ERROR("WebUI: Failed to allocate persist_content copy");
         free(work->text);
         free(work);
         return 1;
      }
   }

   for (int i = 0; i < image_id_count; i++) {
      safe_strscpy(work->image_ids[i], image_ids[i]);
   }
   work->image_id_count = image_id_count;
   /* The frame's ref (held by this thread while it handles the frame) goes with
    * the turn to its worker. */
   if (webui_turn_ref_get()) {
      snprintf(work->client_ref, sizeof(work->client_ref), "%s", webui_turn_ref_get());
   }
   work->from_visual = webui_turn_from_visual_get();

   /* Retain the session for the queued turn (released by the worker when it runs,
    * or by webui_text_turn_free on purge/reject). */
   session_retain(session);

   /* Enqueue onto the per-session turn queue — spawned now iff no turn is in
    * flight for this session, else queued FIFO and chained when the current
    * finishes (strict serialization → no concurrent streams on one session). */
   int qrc = turn_queue_enqueue(session->session_id, TURN_SOURCE_USER, work, webui_text_turn_spawn,
                                webui_text_turn_free);
   if (qrc != TURN_QUEUE_OK) {
      /* Cap hit (FULL) or failure: we still own `work` — clean up + tell the user. */
      if (qrc == TURN_QUEUE_FULL) {
         webui_send_error(session, "TURN_QUEUE_FULL",
                          "You have too many messages queued — wait for the current reply.");
      }
      webui_text_turn_free(work); /* frees work AND releases the session retain */
      return qrc == TURN_QUEUE_FULL ? WEBUI_TEXT_INPUT_REPORTED : 1;
   }

   return 0;
}

/* INVARIANT: the synchronous portion (conv-create + broadcast, before the turn is
 * enqueued to a worker) MUST NOT pump lws (no lws_service / connection-close
 * callbacks). The always-on sweep (webui_thread_func) calls this while iterating a
 * snapshot of raw connection pointers taken without the registry lock; if this
 * path synchronously flushed lws, a CLOSED callback could free a snapshotted
 * connection mid-sweep. */
int webui_process_text_input(session_t *session, const char *text, bool input_was_voice) {
   return webui_process_text_input_with_images(session, text, NULL, 0,
                                               /*persist_content=*/NULL, input_was_voice);
}
