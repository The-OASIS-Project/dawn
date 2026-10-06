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
 * A session's compaction: the oldest part of a long history summarized, and
 * the summary put in its place, at a turn seam and nowhere else.
 *
 * - When a turn ends and the history nears its window, the range to summarize
 *   is copied under the lock and summarized in the background.  The worker
 *   never touches the history or the database.
 * - When the next turn begins (session_prefix_apply_turn), a ready summary is
 *   applied if its range is still where it was: the range goes, the summary
 *   rides in front of the first kept question, every turn's reasoning is left
 *   behind (a declared boundary), and what is in force is worked out again
 *   from what the history still shows.  The turn's record saves the summary,
 *   its node and the watermark with its rows, in one transaction.
 * - A turn that would reach the hard threshold with none ready summarizes
 *   first (session_compaction_prepare), on the turn's own thread, the lock not
 *   held during the call.
 *
 * What a summary reads is its runner's own copy; the session holds only the
 * range's refs and the result, so a teardown never frees what is being read.
 *
 * Only rows already saved are summarized (a reload must start past exactly
 * them); a voice surface, whose history is saved whole, keeps the messages
 * taken out for its save.
 */

#ifndef SESSION_COMPACTION_H
#define SESSION_COMPACTION_H

#include <stdbool.h>
#include <stdint.h>

#include "core/session_manager.h"
#include "llm/llm_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/** What an applied compaction leaves the turn's record to save. */
typedef struct {
   char *summary;                  /**< heap */
   int level;                      /**< llm_compaction_level_t */
   int64_t first_id;               /**< first summarized row */
   int64_t last_id;                /**< last summarized row (before tail rows) */
   int64_t kept_first_id;          /**< first kept row, 0 when unsaved */
   struct json_object *tail_calls; /**< summarized tool calls' ids, for rows past last_id */
   /** The definitions the summarized tool changes held, merged by name in
    *  order (prefix_tools_apply appends again what is no longer in force);
    *  NULL when it summarized none */
   struct json_object *removed_tools;
   int count;         /**< messages summarized */
   int tokens_before; /**< the history's estimate before and after */
   int tokens_after;
} session_compaction_commit_t;

/** Free what @p c holds and zero it. */
void session_compaction_commit_free(session_compaction_commit_t *c);

/**
 * @brief A turn ended: summarize ahead, in the background, when @p hist (the
 *        history it ran on) nears the soft threshold
 *
 * Not for a background job (it compacts at its seams, synchronously), nor
 * while one is pending or within the cooldown.
 */
void session_compaction_trigger(session_t *session,
                                struct json_object *hist,
                                llm_type_t type,
                                cloud_provider_t provider,
                                const char *model);

/**
 * @brief A turn is about to be applied: when the history it will run on, with
 *        @p extra_tokens the turn adds, reaches the hard threshold and no
 *        summary is ready, make one now (a running one is waited for)
 *
 * The lock is not held during the summarizer's call.  A failure falls to the
 * mechanical summary; only a cancel leaves the history as it is.
 */
void session_compaction_prepare(session_t *session, int extra_tokens);

/**
 * @brief Caller holds history_mutex.  Apply a ready summary to @p hist
 *
 * When its range is still the start of @p hist's messages: the range goes,
 * the summary goes in front of the first kept question, every turn's
 * reasoning is dropped, and what is in force is reset to what @p hist shows.
 * For a voice surface the messages taken out are kept for its save.
 *
 * @param out What the turn's record saves (zeroed when nothing was applied)
 * @return true when applied
 */
bool session_compaction_apply_locked(session_t *session,
                                     struct json_object *hist,
                                     session_compaction_commit_t *out);

/**
 * @brief Caller holds history_mutex.  The history array @p from became @p to
 *        (the same messages, a new array): a pending summary of it follows
 */
void session_compaction_rebind_locked(session_t *session,
                                      struct json_object *from,
                                      struct json_object *to);

/**
 * @brief The client's "context compacted" marker, for a compaction applied in
 *        conversation @p conv_id.  A weak no-op here; the WebUI replaces it.
 */
void session_compaction_client_notice(session_t *session,
                                      int64_t conv_id,
                                      int tokens_before,
                                      int tokens_after,
                                      int count,
                                      const char *summary,
                                      int level);

/**
 * @brief Tell the session's client a compaction happened in conversation
 *        @p conv_id (its "context compacted" marker).  Not under history_mutex.
 */
void session_compaction_notify(session_t *session,
                               int64_t conv_id,
                               const session_compaction_commit_t *c);

/**
 * @brief Caller holds history_mutex.  Drop a pending compaction: its range
 *        changed (a withdrawal, another conversation) or its history is going
 */
void session_compaction_drop_locked(session_t *session);

/**
 * @brief Caller holds history_mutex.  The session's context is replaced (a new
 *        conversation, another owner): a pending compaction goes, and so do the
 *        messages a voice surface kept for its save.
 */
void session_compaction_reset_locked(session_t *session);

/**
 * @brief The watermark of @p c in conversation @p conv_id: its last row,
 *        raised over the rows of the tool exchanges it summarized (matched by
 *        call id, never by position).  Not under history_mutex.
 */
int64_t session_compaction_watermark(int64_t conv_id,
                                     int user_id,
                                     const session_compaction_commit_t *c);

/**
 * @brief A turn switched the session's model from @p from: the next turn's
 *        seam judges the history against the new model's window, and
 *        summarizes (if it must) with @p from, which the history fits
 */
void session_compaction_note_switch(session_t *session, const session_llm_config_t *from);

/** Session teardown: none starts again; cancel, join the worker, free what it
 *  holds.  Safe to call more than once. */
void session_compaction_teardown(session_t *session);

#ifdef __cplusplus
}
#endif

#endif /* SESSION_COMPACTION_H */
