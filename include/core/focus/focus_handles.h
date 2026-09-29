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
 * Stable memory citation handles, per session: the handles the conversation a
 * session's history holds has given its injected memory items ([M1], [M2], ...,
 * each item keeping its handle for the conversation's life), loaded from and
 * saved to conversation_focus_handles.  A history that belongs to no saved
 * conversation yet numbers its items here, and they are written when it is
 * saved (focus_handles_flush).
 */

#ifndef FOCUS_HANDLES_H
#define FOCUS_HANDLES_H

#include <stdbool.h>
#include <stdint.h>

#include "auth/auth_db_focus_handles.h"

#ifdef __cplusplus
extern "C" {
#endif

struct session;

/** Buffer size for an item id a handle names (CONV_FOCUS_ITEM_ID_MAX + 1). */
#define FOCUS_HANDLE_ITEM_ID_LEN (CONV_FOCUS_ITEM_ID_MAX + 1)

/** One handle the session's conversation has given. */
typedef struct {
   char source[CONV_FOCUS_SOURCE_MAX + 1];
   char item_id[FOCUS_HANDLE_ITEM_ID_LEN];
   int handle;
   bool saved; /**< Stored in conversation_focus_handles */
} focus_handle_t;

/** A session's handle table (session_t.focus_handles). */
typedef struct focus_handles {
   int64_t conv_id; /**< The conversation they belong to; 0 = not saved yet */
   int user_id;     /**< Whose memory items they number */
   bool loaded;     /**< conv_id's stored handles have been read */
   int count;
   int cap;
   focus_handle_t *items;
} focus_handles_t;

/**
 * Give each item its handle in conversation @p conv_id (0: the session's
 * history belongs to no saved conversation yet): the one it already has there,
 * or the next.  A saved conversation's handles are read from the database the
 * first time and new ones stored there; the history mutex is never held across
 * the database.
 *
 * @param items In: source and item_id; out: handle (0 when it couldn't be
 *              given) and is_new
 * @return 0 when every item has a handle, 1 otherwise
 */
int focus_handles_assign(struct session *session,
                         int64_t conv_id,
                         int user_id,
                         conv_focus_handle_t *items,
                         int count);

/**
 * The item handle @p handle names in the session's conversation, copied into
 * @p item_id.  Caller holds the session's history mutex.
 *
 * @return true when the conversation has given that handle
 */
bool focus_handles_item_locked(const struct session *session,
                               int handle,
                               char item_id[FOCUS_HANDLE_ITEM_ID_LEN]);

/**
 * Save the handles a history numbered before it belonged to a conversation,
 * once the session's history is @p conv_id (called where a history is bound to
 * its new conversation).  A no-op when there are none, or when the session's
 * history is another conversation.
 *
 * @param user_id The conversation's owner, or 0 for the user the handles were
 *                given for
 * @return 0, or 1 when they couldn't be saved (they stay unsaved)
 */
int focus_handles_flush(struct session *session, int64_t conv_id, int user_id);

/**
 * The handles the session's table gives items named in @p item_ids (sorted,
 * strcmp), when the table is conversation @p conv_id's (0: a history no
 * conversation stored yet).  Caller holds the session's history mutex.
 *
 * @param out Receives up to @p cap handles
 * @return How many
 */
int focus_handles_withdrawn_locked(const struct session *session,
                                   int64_t conv_id,
                                   const char *const *item_ids,
                                   int n_ids,
                                   int *out,
                                   int cap);

/** Forget the session's handles (its history now holds another conversation).
 *  Caller holds the session's history mutex. */
void focus_handles_reset_locked(struct session *session);

/**
 * Save the handles of a history that belonged to no conversation into
 * @p conv_id, the one it was just saved as (a voice session, which saves its
 * whole history at once).  Caller holds the session's history mutex (the
 * database lock is taken inside: it is a leaf).
 *
 * @return 0, or 1 when they couldn't be saved
 */
int focus_handles_save_locked(struct session *session, int64_t conv_id, int user_id);

/** Free a table (session teardown). NULL-safe. */
void focus_handles_free(focus_handles_t *handles);

#ifdef __cplusplus
}
#endif

#endif /* FOCUS_HANDLES_H */
