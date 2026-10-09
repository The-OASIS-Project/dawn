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
 * What the email executor does with a new request, given what its session
 * already has in that request's slot (pure, so it can be tested alone).
 */

#ifndef WEBUI_EMAIL_EXEC_POLICY_H
#define WEBUI_EMAIL_EXEC_POLICY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A session's kinds of email work; each has its own slot. */
typedef enum {
   EMAIL_EXEC_SLOT_LIST = 0, /* list and search: a newer one replaces the older */
   EMAIL_EXEC_SLOT_READ,     /* one running, one waiting; a newer read replaces the waiting one */
   EMAIL_EXEC_SLOT_COUNTS,   /* one at a time */
   EMAIL_EXEC_SLOT_FLAGS,    /* one at a time */
   EMAIL_EXEC_SLOT_ADMIN,    /* account settings work (test connection): one at a time */
   EMAIL_EXEC_SLOT_MOVE, /* archive, trash, undo: a FIFO, never replaced (each is the user's act) */
   EMAIL_EXEC_SLOT_COUNT
} email_exec_slot_t;

/* Most email requests one user may have running, waiting or still draining, across sessions
 * (queued moves aren't counted: only the one running). */
#define EMAIL_EXEC_USER_LIVE_MAX 6

/* Moves one session may have queued behind its running one. */
#define EMAIL_EXEC_MOVE_QUEUE 3

/* Most requests one slot may hold: one running plus its waiting ones. */
#define EMAIL_EXEC_SLOT_JOINS_MAX (1 + EMAIL_EXEC_MOVE_QUEUE)

typedef enum {
   EMAIL_EXEC_RUN = 0, /* start it now */
   EMAIL_EXEC_WAIT,    /* keep it waiting in the slot (behind the others, for a move) */
   EMAIL_EXEC_REFUSE,  /* answer BUSY */
} email_exec_action_t;

typedef struct {
   email_exec_action_t action;
   bool cancel_running;  /* the running request is superseded (answer it SUPERSEDED) */
   bool replace_waiting; /* the waiting request is superseded (answer it SUPERSEDED) */
} email_exec_admit_t;

/**
 * @brief Decide a new request
 * @param slot        Its slot
 * @param has_running The slot has a request running
 * @param waiting     Requests waiting in the slot
 * @param user_live   The user's live requests now: running or waiting in any session
 *                    (moves: running only), plus cancelled ones whose tasks still run
 */
email_exec_admit_t email_exec_admit(email_exec_slot_t slot,
                                    bool has_running,
                                    int waiting,
                                    int user_live);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_EMAIL_EXEC_POLICY_H */
