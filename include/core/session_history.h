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
 * A session's conversation history and the turn that runs on it: which
 * history a turn uses, the conversation it belongs to, and the writes other
 * threads make meanwhile (src/core/session_history.c).  Included by
 * core/session_manager.h, which defines session_t.
 */

#ifndef SESSION_HISTORY_H
#define SESSION_HISTORY_H

#include <json-c/json.h>
#include <stdbool.h>
#include <stdint.h>

#include "core/session_manager.h" /* session_t (no-op when included from it) */

#ifdef __cplusplus
extern "C" {
#endif

/** session_defer_fact_source(): kept until its conversation is known. */
#define SESSION_FACT_SOURCE_QUEUED 2
/** session_defer_fact_source(): in a turn, but no room to keep it (logged). */
#define SESSION_FACT_SOURCE_DROPPED 3

#ifdef ENABLE_MULTI_CLIENT

/**
 * @brief The line heading the device events in a turn's volatile block
 *
 * Carries a random tag chosen once per process, so text inside the block (an
 * event's own text, a document chunk) can't pose as it: a refresh finds the
 * events where the renderer put them.
 */
const char *session_notices_header(void);

/**
 * @brief Render the session's device events for a turn's volatile block.
 *
 * Drops events older than SESSION_NOTICE_TTL_SEC, then renders, oldest first
 * with how long ago each happened, the household's and those that are
 * @p viewer_user_id's (the surface's user, read before taking the lock).
 * Another user's are kept but not shown.  Caller holds history_mutex.
 *
 * @return Allocated text ("" when there are none; caller frees), or NULL on
 *         allocation failure.
 */
char *session_render_notices_locked(session_t *session, int viewer_user_id);

/** history_conversation_id value: the history holds more than one conversation's turns */
#define SESSION_HISTORY_CONV_MIXED ((int64_t)-1)

/**
 * @brief Record that the session's history holds @p conv_id
 *
 * For a session that is dedicated to one conversation and whose history already
 * holds it (e.g. a messaging channel's conversation with nothing to restore).
 * To install a conversation's messages use session_replace_history(), which
 * binds in the same step.
 *
 * @param session Session
 * @param conv_id Conversation the history holds (0 = none)
 */
void session_bind_history_conversation(session_t *session, int64_t conv_id);

/**
 * @brief Snapshot the session's live history for memory extraction
 *
 * Returns a deep copy (provider state stripped) taken under history_mutex, with
 * the conversation it holds read in the same critical section, so a turn
 * appending concurrently can neither corrupt the copy nor change its attribution.
 *
 * @param session   Session
 * @param conv_out  Receives history_conversation_id (may be NULL)
 * @param count_out Receives the copy's message count (may be NULL)
 * @return New array (caller owns), or NULL
 */
struct json_object *session_snapshot_history(session_t *session, int64_t *conv_out, int *count_out);

/**
 * @brief Replace the session's history with one conversation's messages
 *
 * Swaps the array and binds @p conv_id in one history_mutex critical section.
 * Takes ownership of @p history.
 *
 * @param session Session
 * @param history New history array (ownership transferred)
 * @param conv_id Conversation the history holds (0 = none)
 */
void session_replace_history(session_t *session, struct json_object *history, int64_t conv_id);

/**
 * @brief Builds a conversation's LLM context, detached from any session
 *
 * Registered by the WebUI layer (which owns the canonical restore: image
 * rehydration, compaction watermark).  Must not touch any session.  Also
 * reports the conversation's stored LLM settings (applied over @p base) in
 * @p cfg_out, setting *@p has_cfg_out when it has any.
 *
 * @return New history array (caller owns), or NULL on failure
 */
typedef struct json_object *(*session_history_loader_fn)(int user_id,
                                                         int64_t conv_id,
                                                         const session_llm_config_t *base,
                                                         session_llm_config_t *cfg_out,
                                                         bool *has_cfg_out);

/** Register the loader session_turn_begin() uses (NULL to unregister). */
void session_set_history_loader(session_history_loader_fn loader);

/**
 * @brief Start a turn: pin the history it runs on
 *
 * For @p conv_id > 0: the live history if it holds that conversation (or it is a
 * fresh chat and the conversation has no messages yet, which binds it);
 * otherwise the conversation's own history, built by the registered loader and
 * kept private to the turn.  If loading fails the turn runs on an empty private
 * history rather than on another conversation's context.  For @p conv_id 0 the
 * live history is pinned until session_turn_set_conversation() resolves it.
 * Call when a turn is dequeued, before anything touches the history; pair with
 * session_turn_end().
 *
 * @param session Session
 * @param conv_id Conversation the turn belongs to (0 = not yet known)
 * @param user_id Owner, for loading the conversation
 */
void session_turn_begin(session_t *session, int64_t conv_id, int user_id);

/**
 * @brief Record the running turn's conversation once it is resolved late
 *
 * For a turn that began with conv 0 (fresh chat before the conversation row
 * existed, or a voice turn that adopts one after ASR).  No-op outside a turn
 * (between session_turn_begin and _end).  If the pinned history holds this
 * conversation or nothing but the turn's own messages, it is bound to it.
 * Otherwise, with @p may_load, the turn moves onto the conversation's own
 * history (loaded; the turn's messages so far are carried over, and a live
 * history they were written into is marked SESSION_HISTORY_CONV_MIXED).
 * Without @p may_load nothing is loaded: the turn keeps its history, while its
 * stream tag and persistence follow @p conv_id.  This is the one writer of the
 * turn's stream_conversation_id after session_turn_begin().
 *
 * @param may_load Loading allowed (the turn's own worker thread)
 */
void session_turn_set_conversation(session_t *session, int64_t conv_id, bool may_load);

/**
 * @brief Release the turn's pinned history (safe when nothing is pinned)
 *
 * If the turn ran on its own copy of the conversation the client is showing
 * (session_t.viewed_conversation_id) while the live history holds something
 * else, the copy becomes the live history.  A prompt refresh another thread
 * made while the turn held the live history is applied here.
 */
void session_turn_end(session_t *session);

/** What a finishing turn has yet to write (session_turn_finish()); caller frees the strings. */
typedef struct {
   int64_t conv;      /* the conversation to write them to */
   char *prior_user;  /* an earlier turn's exchange this turn adopted: written */
   char *prior_reply; /* first, then stamped with session_stamp_claimed() */
   char *user;        /* this turn's own user message (persisted form) */
   char *reply;       /* and reply */
} session_turn_unsaved_t;

/** session_turn_finish(): the turn ended. */
#define SESSION_TURN_ENDED 0
/** session_turn_finish(): write what it handed out, then call it again. */
#define SESSION_TURN_WRITE_UNSAVED 2

/**
 * @brief session_turn_end(), unless the turn has messages to save first
 *
 * When the turn's conversation is known and messages wait for it (its own,
 * session_turn_set_pending(), or an adopted earlier exchange), hands them out
 * WITHOUT ending the turn and returns SESSION_TURN_WRITE_UNSAVED: the caller
 * writes them, in the order of the struct, so their row ids are stamped on the
 * turn's own history, and calls again.  Otherwise ends the turn and returns
 * SESSION_TURN_ENDED; messages still waiting for an unknown conversation then
 * wait for it (session_bind_created_conversation).  Deciding and ending in one
 * critical section means a conversation created at the last moment is seen by
 * one or the other.
 */
int session_turn_finish(session_t *session, session_turn_unsaved_t *out);

/**
 * @brief The conversation the running turn belongs to
 *
 * While a turn is active, its conversation (0 until resolved); otherwise the
 * conversation the session history holds.  Use this, not the client's view,
 * for anything a turn writes or reads on behalf of its conversation.
 */
int64_t session_turn_conversation(session_t *session);

/**
 * @brief The conversation @p history holds
 *
 * @p history is the running turn's pinned history or the session history;
 * returns the conversation it holds, or 0 (none, mixed, or neither array).
 */
int64_t session_history_conversation_of(session_t *session, struct json_object *history);

/**
 * @brief Whether the running turn works on its own history, not the live one
 *
 * Caller holds history_mutex.  Per-session state that describes the live context
 * (e.g. the focus-injection dedup set) does not apply to such a turn.
 */
static inline bool session_turn_on_own_history_locked(const session_t *session) {
   return session->turn_history && session->turn_history != session->conversation_history;
}

/** session_turn_on_own_history_locked(), taking history_mutex. */
bool session_turn_on_own_history(session_t *session);

/**
 * @brief Append @p msg (ownership taken) to a turn's history under its lock
 *
 * The one way the LLM tool loop adds to the history it runs on: other threads
 * (a sidebar switch's snapshot, the back-fill) read the same array under
 * history_mutex, so the writer must take it too.  The lock is the command
 * context session's; without one (single-threaded callers) it appends plainly.
 */
void session_history_append(struct json_object *history, struct json_object *msg);

/**
 * @brief Replace @p history's messages with @p from's, under the same lock
 *
 * Compaction's in-place rewrite of the history the turn runs on.
 */
void session_history_replace_contents(struct json_object *history, struct json_object *from);

/**
 * @brief Mark the running turn as waiting for a conversation created after it
 *
 * For a typed first message of a new chat: it is dispatched before the client
 * creates the conversation row.  Only such a turn is bound by
 * session_bind_created_conversation().  No-op when the turn already has one.
 */
void session_turn_await_conversation(session_t *session);

/** Mark the running turn as background (a reinvoke, not the user's own). */
void session_turn_mark_background(session_t *session);

/**
 * @brief Whether the caller is the session's running turn
 *
 * True on the turn's thread and on the tool threads carrying its token
 * (session_turn_token()); false outside a turn and on any other thread.
 */
bool session_turn_is_caller(session_t *session);

/** Whether a turn is running on @p session (on any thread). */
bool session_turn_active(session_t *session);

/**
 * @brief Whether the caller is a running turn the user started
 *
 * True only for the running turn's own code (session_turn_is_caller()) on a
 * turn not marked background, in a session that isn't a job's.  False outside
 * a turn: there is no user request to attribute the effect to.  For effects
 * that should follow only from what the user asked for, not from untrusted
 * content a background turn read.
 */
bool session_turn_user_originated(session_t *session);

/**
 * @brief Keep a message the turn could not save: its conversation doesn't exist yet
 *
 * @p role "user" (the persisted form, [IMAGE:] markers included) or "assistant"
 * (the reply).  The turn's own worker writes them once the conversation is known
 * (session_turn_take_pending()).  A turn that ends still waiting hands them to
 * session_bind_created_conversation().
 */
void session_turn_set_pending(session_t *session, const char *role, const char *persist_text);

/**
 * @brief Take a pending message once the turn's conversation is known
 *
 * Returns it (caller frees) with the conversation in @p conv_out, or NULL when
 * there is none or the conversation is still unknown.
 */
char *session_turn_take_pending(session_t *session, const char *role, int64_t *conv_out);

/** How long an ended turn's unsaved exchange waits for its conversation. */
#define SESSION_UNCLAIMED_TURN_SEC 120

/**
 * @brief Hand a conversation just created to the turn it was created for
 *
 * For the handler creating a new chat's conversation, in one critical section:
 * - A turn still running that waits for one (session_turn_await_conversation)
 *   is bound to @p conv_id (history pin and stream tag; loads nothing) and
 *   @p adopted_out is set.  Its own worker writes its messages.
 * - A turn that ended before the conversation existed left its exchange: when
 *   a running turn is adopted, that turn writes it ahead of its own
 *   (session_turn_take_prior()); otherwise returns true with the user message
 *   and reply (either may be NULL; caller frees both) for the caller to write
 *   to @p conv_id, then pass the rows' ids to session_stamp_claimed().
 */
bool session_bind_created_conversation(session_t *session,
                                       int64_t conv_id,
                                       char **user_out,
                                       char **reply_out,
                                       bool *adopted_out);

/**
 * @brief Stamp the rows of a claimed exchange
 *
 * Puts each row id (0 = not written) on the history message it was written
 * from, wherever it now sits.  While another thread's turn is running (it reads
 * those messages without the lock), that turn stamps them when it ends.
 */
void session_stamp_claimed(session_t *session, int64_t user_row_id, int64_t reply_row_id);

/**
 * @brief Take the earlier exchange a running turn adopted, to write first
 *
 * For the turn's own code once its conversation is known: returns true with the
 * exchange (either may be NULL; caller frees) and the conversation.  Write the
 * rows before the turn's own, then session_stamp_claimed().
 */
bool session_turn_take_prior(session_t *session,
                             int64_t *conv_out,
                             char **user_out,
                             char **reply_out);


/**
 * @brief Start a new context in the live history (caller holds history_mutex)
 *
 * Replaces it with an empty one (holding just @p system_prompt when given)
 * bound to no conversation, and drops what belonged to the old context: its
 * focus items, visual guidelines, an ended turn's unsaved exchange, and the
 * facts waiting for its conversation.
 */
void session_new_context_locked(session_t *session, const char *system_prompt);

/** Records a conversation as facts' source (memory_db_fact_attach_sources). */
typedef void (*session_fact_source_fn)(const session_fact_source_t *facts,
                                       int count,
                                       int64_t conv_id);

/** Register the recorder session_flush_fact_sources() calls. */
void session_set_fact_source_hook(session_fact_source_fn fn);

/**
 * @brief Where a fact the running turn saved or restated was learned
 *
 * For the running turn's own code:
 * - SUCCESS with @p conv_out: the turn's conversation is known; record it now.
 * - SESSION_FACT_SOURCE_QUEUED: not known yet (a voice turn, a new chat's first
 *   message).  Recorded when the turn learns it; if the turn ends without one,
 *   when the live history it wrote becomes a conversation (the new chat's row,
 *   a voice save), and forgotten if that history is discarded first.
 * - SESSION_FACT_SOURCE_DROPPED: in a turn, but too many facts wait; nothing is
 *   recorded (not "outside a conversation" either).
 * - FAILURE: outside a turn (the scheduler, MQTT): no conversation.
 */
int session_defer_fact_source(session_t *session,
                              int64_t fact_id,
                              int user_id,
                              bool created,
                              int64_t *conv_out);

/**
 * @brief Record @p conv_id as the source of the facts ended turns left waiting
 *
 * For when the live history those turns wrote becomes a conversation.  Only
 * @p owner_user_id's facts are recorded (0 = any: a WebUI session is one
 * user's); they are taken off the list either way.  Recorded through the
 * registered hook, outside history_mutex.
 */
void session_flush_fact_sources(session_t *session, int64_t conv_id, int owner_user_id);

/** Take the facts ended turns left waiting (caller holds history_mutex); returns how many. */
int session_take_fact_sources_locked(session_t *session,
                                     session_fact_source_t out[SESSION_PENDING_FACT_SOURCES_MAX]);

/** Record taken facts' source as session_flush_fact_sources() does (no lock held). */
void session_record_fact_sources(const session_fact_source_t *facts,
                                 int count,
                                 int64_t conv_id,
                                 int owner_user_id);

/**
 * @brief Reference to the history the running turn works on
 *
 * The pinned turn history if a turn is running, else conversation_history.
 * Caller owns the returned reference (json_object_put).
 */
struct json_object *session_get_turn_history(session_t *session);

/**
 * @brief Release a reference from session_get_turn_history() / session_get_history()
 *
 * Under history_mutex: json-c's reference count is not atomic, and other
 * threads take and drop references to the same array under that lock.
 */
void session_put_history(session_t *session, struct json_object *history);

/**
 * @brief Take the running turn's messages back out of its history
 *
 * For a turn abandoned (interrupted): removes its user message (the one it
 * appended through session_add_turn_message) and everything after it — its
 * tool calls and results — from its history.  Nothing when that message is no
 * longer there (compacted away).  For the turn's own code.  Returns how many
 * were removed.
 */
int session_rollback_turn(session_t *session);

/**
 * @brief End a turn the user stopped, keeping their question
 *
 * For a reply stopped at the user's request: keeps the turn's user message,
 * removes what followed it (tool calls and results, which the provider rejects
 * without their pairs) and appends @p note as the assistant's reply, so a later
 * "do that again" still has the request to refer to, and the model sees it was
 * stopped rather than left unanswered.  For the turn's own code.  False when the
 * question is no longer there (the caller then rolls back) or on failure.
 */
bool session_stop_turn(session_t *session, const char *note);

/**
 * @brief Whether a history write from this thread must wait for the running turn
 *
 * True while a turn runs on the live history and the caller is not that turn
 * (it lacks its token).  Caller holds history_mutex.
 */
bool session_turn_defers_writes_locked(const session_t *session);

#else /* !ENABLE_MULTI_CLIENT: no sessions; the history is the caller's alone */

static inline void session_history_append(struct json_object *history, struct json_object *msg) {
   json_object_array_add(history, msg);
}

static inline void session_history_replace_contents(struct json_object *history,
                                                    struct json_object *from) {
   json_object_array_del_idx(history, 0, json_object_array_length(history));
   for (size_t i = 0; i < json_object_array_length(from); i++) {
      json_object_array_add(history, json_object_get(json_object_array_get_idx(from, i)));
   }
}

static inline bool session_turn_is_caller(session_t *session) {
   (void)session;
   return false;
}

static inline bool session_turn_active(session_t *session) {
   (void)session;
   return false;
}

static inline bool session_turn_user_originated(session_t *session) {
   (void)session;
   return false;
}

#endif /* ENABLE_MULTI_CLIENT */

#ifdef __cplusplus
}
#endif

#endif /* SESSION_HISTORY_H */
