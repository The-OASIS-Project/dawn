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
 * Provider-agnostic text-input dispatch — Layer 2.
 *
 * Common core of "user typed something, run an LLM turn" used by the WebUI
 * text path and (in future) the messaging-channels engine (Telegram,
 * Discord, Slack, SMS).  Handles: add-user-message-to-history, persist to
 * conv_db, per-turn focus injection, LLM call with optional TTS sentence
 * streaming.  Does NOT post-process `<command>` tags — that's a Layer 4
 * concern handled per-provider by the caller.
 *
 * Synchronous from the caller's perspective; intended to be invoked from a
 * worker thread, NEVER from the libwebsockets service thread or any other
 * latency-sensitive event loop.
 */
#ifndef TEXT_INPUT_DISPATCH_H
#define TEXT_INPUT_DISPATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/session_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Optional callback fired after the user message has been added
 *        to session history and (if configured) persisted to conv_db,
 *        but BEFORE per-turn focus injection and the LLM call.
 *
 * Use this to send a transcript echo or otherwise mirror the user's
 * input back to the originating client at the same point in the
 * pipeline as the existing WebUI behavior.
 *
 * @param ctx Caller-supplied context pointer (`opts->user_msg_added_ctx`).
 * @param text The user message text that was just added.
 * @param persisted_to_db True if the message was successfully written
 *                        to conv_db (only when `opts->conversation_id > 0`).
 */
typedef void (*text_input_user_msg_added_fn)(void *ctx, const char *text, bool persisted_to_db);

/**
 * @brief Per-call options for `core_text_input_dispatch()`.
 *
 * Zero-initialize and populate only the fields you need.  All fields are
 * optional — the zero value means "skip the corresponding step."
 */
typedef struct {
   /* Conversation persistence.  Set conversation_id > 0 to persist the
    * user message to conv_db via conv_db_add_message_ex() and stamp the
    * returned row ID into the in-memory history entry.  When 0, no DB
    * write happens. */
   int64_t conversation_id;
   int auth_user_id;

   /* TTS sentence streaming.  Pass NULL to skip TTS (text-only mode).
    * When non-NULL, the LLM call uses sentence buffering and invokes
    * sentence_cb for each complete sentence with sentence_userdata. */
   session_sentence_callback sentence_cb;
   void *sentence_userdata;

   /* Optional user-message-added hook (see typedef above).  Pass NULL
    * to skip. */
   text_input_user_msg_added_fn on_user_msg_added;
   void *user_msg_added_ctx;

   /* Optional per-turn channel hint appended to the system prompt
    * AFTER per-turn focus injection has built it.  Used by the
    * messaging engine to inform the LLM of channel-specific
    * constraints (e.g., "Channel: SMS. Keep reply concise — aim for
    * under 500 chars, plain text, no markdown.").  NULL = no
    * appendage.  The hint applies only to this turn — the next call
    * to session_dispatch_user_turn rebuilds the prompt from scratch. */
   const char *channel_hint;

   /* Observe-side `status` events (background-jobs Phase 2).  Emitted at turn
    * start/end so a jobs panel or TUI reads one durable signal instead of
    * inferring liveness from delta timing.
    *
    * Scoped deliberately (see BACKGROUND_JOBS_DESIGN.md §6.2): only turns worth
    * watching emit.  An interactive conversation is already observable through
    * the sidebar chip, and emitting for every turn would roughly double the
    * event table for no added visibility.
    *
    * `is_background_turn` — reinvoke / child-agent turn (TURN_SOURCE_BACKGROUND).
    * `is_job_conversation` — the conversation itself is a job (job_status IS NOT
    *   NULL).  Needed on top of session type + turn source: a user can type into
    *   a job conversation from their own WebUI session, and a tailer watching
    *   that job must still see the turn.  A SESSION_TYPE_JOB session implies
    *   this, so the worker need not set it. */
   bool is_background_turn;
   bool is_job_conversation;
} text_input_dispatch_opts_t;

/**
 * @brief Dispatch a text-input turn through the LLM pipeline.
 *
 * Pipeline:
 *   1. Adds the user message to session history
 *      (session_add_message / session_add_message_with_images).
 *   2. If opts->conversation_id > 0, persists to conv_db and stamps the
 *      message ID into the in-memory history entry.
 *   3. Fires opts->on_user_msg_added (if set) with the persistence
 *      result — caller's hook for UI transcript echo.
 *   4. Runs per-turn focus injection (session_dispatch_user_turn).
 *   5. Calls session_llm_call_with_tts_vision_no_add with the supplied
 *      TTS sentence callback (or NULL).
 *
 * Returns the LLM response text on success; caller must free().  Returns
 * NULL on LLM error or session cancellation (the caller is expected to
 * have already incremented the session's request_generation if it cares
 * about cancellation semantics).
 *
 * Does NOT:
 *   - Increment / read session->request_generation (caller's lifecycle).
 *   - Post-process `<command>` tags in the response.
 *   - Free vision image buffers (caller owns them).
 *   - Release the session reference (caller acquired, caller releases).
 *
 * Thread safety: must be called from a worker thread.  Adds messages
 * under session->history_mutex internally; do not hold it across the
 * call.
 *
 * @param session              Session context.
 * @param text                 User message text (must be non-NULL/non-empty).
 * @param vision_images        Array of base64-encoded image data, or NULL.
 * @param vision_image_sizes   Array of image sizes, or NULL.
 * @param vision_mimes         Array of MIME type strings, or NULL.
 * @param vision_image_count   Number of images (0 for text-only).
 * @param opts                 Per-call options, or NULL for all defaults.
 *
 * @return Allocated response string (caller frees), or NULL on failure.
 */
char *core_text_input_dispatch(session_t *session,
                               const char *text,
                               const char **vision_images,
                               const size_t *vision_image_sizes,
                               const char (*vision_mimes)[24],
                               int vision_image_count,
                               const text_input_dispatch_opts_t *opts);

#ifdef __cplusplus
}
#endif

#endif /* TEXT_INPUT_DISPATCH_H */
