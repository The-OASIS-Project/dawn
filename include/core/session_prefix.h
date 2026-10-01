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
 * A conversation's request, append-only.  Its system prompt is frozen the
 * first time a turn runs on it (messages[0], MESSAGE_KIND_PREFIX), and every
 * later change reaches the model as something appended where it became true:
 * a change to the instructions or to the surface's standing directions as a
 * message after the turn's question, and the turn's own context (the time,
 * retrieved items, what DAWN knows about the user when it changed, new device
 * events) in front of the question.  Nothing already sent is rewritten, so the
 * provider's prompt cache holds and a model's earlier reasoning stays valid.
 */

#ifndef SESSION_PREFIX_H
#define SESSION_PREFIX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/prompt_parts.h"

#ifdef __cplusplus
extern "C" {
#endif

struct session;
struct json_object;
struct session_prefix_turn;

#ifdef ENABLE_MULTI_CLIENT

/**
 * @brief Apply a turn's prompt to the history the turn runs on
 *
 * Called at dispatch, after the turn's question is in the history.  With a
 * NULL @p cp (no prompt builder, a guest surface, a builder failure) the
 * frozen prefix is still ensured from the history's own system prompt and
 * the turn still gets its device events and @p turn_note.
 *
 * A history without a frozen prefix (a new conversation's first turn, or one
 * saved before prefixes were frozen) takes @p cp's stable prefix; one that
 * held reasoning bound to an earlier system prompt is a declared boundary:
 * that reasoning is left out (it would be refused), text and tool calls stay.
 *
 * @param cp        The turn's composed prompt, or NULL
 * @param turn_note A note for this turn only (a channel's constraint), or NULL
 */
void session_prefix_apply_turn(struct session *session,
                               const composed_prompt_t *cp,
                               const char *turn_note);

/**
 * @brief A turn's question was saved to @p conv_id as row @p row_id
 *
 * Every writer of a turn's question row calls it, right after the write (the
 * row id already stamped on the history message).  The turn's record — what
 * its prompt added (session_prefix_apply_turn) — is saved with the
 * conversation once it has both halves, whichever came second: the turn's
 * context rows and appended messages, each naming this question; the prefix
 * and tool set it ran under; a declared boundary's reasoning floor.  All in
 * one transaction (conv_db_save_turn).  Records wait per question, so an
 * earlier turn's question saved after a later turn began (a new chat's first
 * exchange) still gets its own rows.
 *
 * A turn with no question is saved when its prompt is applied.  One whose
 * question is never saved is given up when a later question is: its appended
 * messages are saved with that one, naming no question, and the conversation
 * leaves its earlier reasoning behind (a boundary).  A surface that saves the
 * whole history at once (a voice session) never calls this: its save writes
 * those rows itself.
 */
void session_prefix_question_saved(struct session *session,
                                   int64_t conv_id,
                                   int user_id,
                                   int64_t row_id);

/** Free a session's records (the queue from @p turn).  NULL-safe.  For the
 *  session's teardown, when it owns them outright. */
void session_prefix_turn_free(struct session_prefix_turn *turn);

/**
 * @brief The session's history was replaced (another conversation, a new
 *        context, a voice save): its records of other histories go
 *
 * A running turn's own (its pinned history) stays.  Caller holds the
 * session's history_mutex.
 */
void session_prefix_release_locked(struct session *session);

/**
 * @brief A running turn taking its question back (session_rollback_turn):
 *        the turn's record, if it wasn't saved, keeps only what the turn
 *        announced to the conversation (instruction and direction changes,
 *        which stay in the history), to save at once naming no question
 *
 * Caller holds the session's history_mutex, as the turn.
 * @param boundary The take-back leaves the history's earlier reasoning behind
 *                 (an inline tool change that followed the question now
 *                 folds): the record saves the boundary with its rows
 * @return The record to pass to session_prefix_save_taken() once the lock
 *         is released, or NULL
 */
struct session_prefix_turn *session_prefix_take_back_locked(struct session *session,
                                                            struct json_object *question,
                                                            bool boundary);

/** Save @p turn (from session_prefix_take_back_locked) with its
 *  conversation.  Takes the history lock; NULL-safe. */
void session_prefix_save_taken(struct session *session, struct session_prefix_turn *turn);

/**
 * @brief Before a voice session saves its whole history: a withdrawal kept
 *        for its next turn applies now, so what the user forgot isn't saved
 *
 * Caller holds the session's history_mutex, with no turn running.  Every
 * withdrawal before this reached the live history (at once, or kept for
 * this), so the saved rows are checked only against those during the save
 * (conv_db_withdraw_conversation from the save's start).
 */
void session_prefix_voice_save_locked(struct session *session);

/**
 * @brief Withdraw what @p user_id forgot or deleted from their conversations'
 *        request context: stored (conv_db_withdraw) and in live histories
 *
 * Call after a user forgets or deletes memories or documents.  Each item
 * injected into a conversation since deleted (auth_db_withdraw.h) has its
 * line withdrawn, stored and live, a history no conversation stored yet (a
 * voice session's) included;
 * with @p memory_bodies (a preference or summary went) every USER MEMORY
 * block is too, and the next turn sends the current one.  A conversation
 * that changed replays without its earlier reasoning (a declared boundary).
 * Takes the database lock, then each session's history lock in turn: never
 * call holding either.
 *
 * @return SUCCESS, or FAILURE when the stored context couldn't be changed
 *         (logged)
 */
int session_withdraw_forgotten(int user_id, bool memory_bodies);

/** session_withdraw_forgotten() on the withdraw worker (one thread, however
 *  many are asked for; each user's merged): for a caller that mustn't wait
 *  on it (the WebUI service thread, a tool call). */
void session_withdraw_forgotten_async(int user_id, bool memory_bodies);

/**
 * @brief A copy of the tools the session's conversation defines (its frozen
 *        definitions and their later changes, llm_tool_defs_for_request; caller
 *        puts), or NULL when it has none or a turn is reading the history on
 *        another thread (for a debug inspector)
 */
struct json_object *session_prefix_tool_defs(struct session *session);

/**
 * @brief The Claude API rejected tools defined in a message on the session's
 *        running conversation: record it there (what is in force, saved with
 *        the conversation), so its tool changes fold into its tools from now on,
 *        after a restart too; a declared boundary.  NULL-safe.
 */
void session_prefix_inline_tools_rejected(struct session *session);

/**
 * @brief The tag of the conversation the session's turn runs on (its
 *        running turn's history, else its live one), copied into @p out
 * @return false when it has none (not frozen yet)
 */
bool session_prefix_tag(struct session *session, char *out, size_t size);

/**
 * @brief @p text (taken) with every copy of the session's conversation tag
 *        secret masked (llm_context_mask_secret): for text that reaches the
 *        conversation from outside it, or the model's own reply.  Returns
 *        @p text itself when it holds none; NULL on allocation failure.
 */
char *session_prefix_mask_secret(struct session *session, char *text);

/**
 * @brief Whether @p history has a frozen prefix
 *
 * Writers of the legacy system-message shape leave such a history alone:
 * changes reach it at its next turn instead.  Caller holds the history's lock.
 */
bool session_prefix_is_frozen(struct json_object *history);

#else

static inline void session_prefix_apply_turn(struct session *session,
                                             const composed_prompt_t *cp,
                                             const char *turn_note) {
   (void)session;
   (void)cp;
   (void)turn_note;
}

static inline int session_withdraw_forgotten(int user_id, bool memory_bodies) {
   (void)user_id;
   (void)memory_bodies;
   return 0;
}

static inline void session_withdraw_forgotten_async(int user_id, bool memory_bodies) {
   (void)user_id;
   (void)memory_bodies;
}

static inline struct json_object *session_prefix_tool_defs(struct session *session) {
   (void)session;
   return NULL;
}

static inline void session_prefix_inline_tools_rejected(struct session *session) {
   (void)session;
}

/* No sessions, no frozen prefix, no tag: llm_tools_execute's tag tripwire has
 * nothing to look for in such a build. */
static inline bool session_prefix_tag(struct session *session, char *out, size_t size) {
   (void)session;
   if (out && size) {
      out[0] = '\0';
   }
   return false;
}

static inline char *session_prefix_mask_secret(struct session *session, char *text) {
   (void)session;
   return text;
}

static inline bool session_prefix_is_frozen(struct json_object *history) {
   (void)history;
   return false;
}

static inline void session_prefix_question_saved(struct session *session,
                                                 int64_t conv_id,
                                                 int user_id,
                                                 int64_t row_id) {
   (void)session;
   (void)conv_id;
   (void)user_id;
   (void)row_id;
}

static inline void session_prefix_turn_free(struct session_prefix_turn *turn) {
   (void)turn;
}

static inline void session_prefix_release_locked(struct session *session) {
   (void)session;
}

static inline struct session_prefix_turn *session_prefix_take_back_locked(
    struct session *session,
    struct json_object *question,
    bool boundary) {
   (void)session;
   (void)question;
   (void)boundary;
   return NULL;
}

static inline void session_prefix_save_taken(struct session *session,
                                             struct session_prefix_turn *turn) {
   (void)session;
   (void)turn;
}

static inline void session_prefix_voice_save_locked(struct session *session) {
   (void)session;
}


#endif /* ENABLE_MULTI_CLIENT */

/**
 * @brief Caller holds history_mutex.  Whether a turn record still holds
 *        @p msg (its question, context, or a message it hasn't saved yet)
 */
bool session_prefix_owns_locked(struct session *session, struct json_object *msg);

#ifdef __cplusplus
}
#endif

#endif /* SESSION_PREFIX_H */
