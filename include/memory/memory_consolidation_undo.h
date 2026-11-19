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
 * The user acting on what a consolidation batch did: undoing a batch or one
 * of its ops, or keeping a candidate it set aside.  Each runs with the user's
 * memory extraction held (so no batch changes memory meanwhile), an undo as a
 * removal the user made (withdrawn from the conversations it was sent into),
 * on a detached worker: blocking work never runs on the WebSocket thread.
 */

#ifndef MEMORY_CONSOLIDATION_UNDO_H
#define MEMORY_CONSOLIDATION_UNDO_H

#include <stdbool.h>
#include <stdint.h>

#include "memory/memory_db_consolidation.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   MEMORY_CONSOLIDATION_UNDO_BATCH, /* every undoable op of the batch, the last first */
   MEMORY_CONSOLIDATION_UNDO_OP,    /* one op */
   MEMORY_CONSOLIDATION_KEEP_OP,    /* store a candidate the batch set aside */
} memory_consolidation_action_t;

typedef enum {
   MEMORY_CONSOLIDATION_OK,        /* everything asked for was done */
   MEMORY_CONSOLIDATION_PARTIAL,   /* a whole-batch undo: some ops changed since */
   MEMORY_CONSOLIDATION_ALL_STALE, /* nothing could be undone: all changed since */
   MEMORY_CONSOLIDATION_ERROR,     /* nothing done (error says why) */
} memory_consolidation_status_t;

/** An action and what came of it. */
typedef struct {
   memory_consolidation_action_t action;
   int user_id;
   int64_t batch_id;
   int64_t op_id; /* 0: the whole batch (an undo) */

   memory_consolidation_status_t status;
   int64_t undone[MEMORY_CONSOLIDATION_OPS_MAX]; /* ops undone (now or before) */
   int n_undone;
   int64_t stale[MEMORY_CONSOLIDATION_OPS_MAX]; /* ops that changed since: left as they are */
   int n_stale;
   int64_t fact_id;   /* a Keep: the fact stored */
   const char *error; /* user-ready (static), NULL when nothing went wrong */
} memory_consolidation_outcome_t;

/** The outcome's status as the wire names it: ok, partial, stale or error. */
const char *memory_consolidation_status_name(memory_consolidation_status_t status);

/**
 * @brief Run @p io's action now (blocking: waits up to MEMORY_FORGET_WAIT_SEC
 *        for the user's running extraction); fills its outcome
 *
 * Every write binds the op to its batch and to @p io->user_id: an op of
 * another batch or another user is "Not found", the same as one that doesn't
 * exist.  After any change, memory_consolidation_batch_notify.
 */
void memory_consolidation_run(memory_consolidation_outcome_t *io);

/** Called on the worker when an action started with memory_consolidation_start ends. */
typedef void (*memory_consolidation_done_fn)(const memory_consolidation_outcome_t *outcome,
                                             void *ctx);

/** memory_consolidation_start(): the user has an undo or keep running already. */
#define MEMORY_CONSOLIDATION_BUSY 2

/**
 * @brief Run an action on a detached worker, one per user at a time; @p done
 *        (on that worker) gets the outcome and @p ctx (which it owns then)
 * @return SUCCESS (started), MEMORY_CONSOLIDATION_BUSY or FAILURE (not
 *         started: @p ctx stays the caller's)
 */
int memory_consolidation_start(memory_consolidation_action_t action,
                               int user_id,
                               int64_t batch_id,
                               int64_t op_id,
                               memory_consolidation_done_fn done,
                               void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_CONSOLIDATION_UNDO_H */
