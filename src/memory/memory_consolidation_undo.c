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
 * The user's undo and Keep of a consolidation batch's ops: the extraction
 * hold, the removal marked (an undo is the user taking something back, so it
 * leaves the conversations it was sent into), the detached worker that keeps
 * this off the WebSocket thread, and the push that tells the user's tabs.
 */

#include "memory/memory_consolidation_undo.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db_withdraw.h"
#include "core/session_prefix.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_consolidate.h"
#include "memory/memory_db_consolidation.h"
#include "memory/memory_extraction.h"
#include "memory/memory_forget.h"

/* Users with an undo or Keep running.  One each: an action waits up to
 * MEMORY_FORGET_WAIT_SEC for the user's extraction, so an unbounded number
 * would pin a thread apiece. */
#define UNDO_MAX_INFLIGHT 8

static pthread_mutex_t s_inflight_mutex = PTHREAD_MUTEX_INITIALIZER;
static int s_inflight[UNDO_MAX_INFLIGHT];

#define ERR_NOT_FOUND "Not found"
#define ERR_STALE "This memory changed since; nothing was undone."
#define ERR_BUSY "Couldn't undo this right now. Try again in a moment."
#define ERR_NO_UNDO "This one can't be undone."
#define ERR_KEEP_BUSY "Couldn't keep this right now. Try again in a moment."
#define ERR_KEEP_FORGOTTEN "You asked me to forget this; it stays forgotten."
#define ERR_NO_KEEP "This one can't be kept."
#define ERR_KEEP_FULL "This batch is full; nothing more can be kept from it."

const char *memory_consolidation_status_name(memory_consolidation_status_t status) {
   switch (status) {
      case MEMORY_CONSOLIDATION_OK:
         return "ok";
      case MEMORY_CONSOLIDATION_PARTIAL:
         return "partial";
      case MEMORY_CONSOLIDATION_ALL_STALE:
         return "stale";
      case MEMORY_CONSOLIDATION_ERROR:
         break;
   }
   return "error";
}

static bool inflight_claim(int user_id) {
   int free_slot = -1;
   pthread_mutex_lock(&s_inflight_mutex);
   for (int i = 0; i < UNDO_MAX_INFLIGHT; i++) {
      if (s_inflight[i] == user_id) {
         free_slot = -1;
         break;
      }
      if (s_inflight[i] == 0 && free_slot < 0) {
         free_slot = i;
      }
   }
   if (free_slot >= 0) {
      s_inflight[free_slot] = user_id;
   }
   pthread_mutex_unlock(&s_inflight_mutex);
   return free_slot >= 0;
}

static void inflight_release(int user_id) {
   pthread_mutex_lock(&s_inflight_mutex);
   for (int i = 0; i < UNDO_MAX_INFLIGHT; i++) {
      if (s_inflight[i] == user_id) {
         s_inflight[i] = 0;
         break;
      }
   }
   pthread_mutex_unlock(&s_inflight_mutex);
}

/* Undo each of @p ids (in that order) into @p io's lists.  The removal is
 * the user's: marked, then withdrawn from the conversations it reached.
 * FAILURE when the database refused one (the rest are still tried). */
static int undo_ids(memory_consolidation_outcome_t *io, const int64_t *ids, int n, bool *missing) {
   int rc = SUCCESS;
   *missing = false;
   memory_consolidation_undo_effect_t effect = { 0 };
   conv_db_withdraw_intent_begin(io->user_id); /* the user taking it back */
   for (int i = 0; i < n; i++) {
      const int one = memory_db_consolidation_undo_op(io->user_id, io->batch_id, ids[i], &effect);
      if (one == MEMORY_DB_SUCCESS && io->n_undone < MEMORY_CONSOLIDATION_OPS_MAX) {
         io->undone[io->n_undone++] = ids[i];
      } else if (one == MEMORY_CONSOLIDATION_STALE && io->n_stale < MEMORY_CONSOLIDATION_OPS_MAX) {
         io->stale[io->n_stale++] = ids[i];
      } else if (one == MEMORY_DB_NOT_FOUND) {
         *missing = true;
      } else if (one == MEMORY_DB_FAILURE) {
         rc = FAILURE;
      } else if (one == MEMORY_CONSOLIDATION_NOTHING && n == 1) {
         io->error = ERR_NO_UNDO;
      }
   }
   conv_db_withdraw_intent_end();
   if (effect.facts || effect.entities) {
      /* What was undone leaves the conversations it was sent into. */
      (void)session_withdraw_forgotten(io->user_id, false);
   }
   return rc;
}

static void run_undo(memory_consolidation_outcome_t *io) {
   int64_t *ids = calloc(MEMORY_CONSOLIDATION_OPS_MAX, sizeof(*ids));
   int n = 0;
   int rc = MEMORY_DB_FAILURE;
   if (!ids) {
      io->error = ERR_BUSY;
      return;
   }
   if (io->op_id > 0) {
      ids[0] = io->op_id;
      n = 1;
      rc = MEMORY_DB_SUCCESS;
   } else {
      rc = memory_db_consolidation_undo_ids(io->user_id, io->batch_id, ids,
                                            MEMORY_CONSOLIDATION_OPS_MAX, &n);
   }
   if (rc == MEMORY_DB_NOT_FOUND) {
      io->error = ERR_NOT_FOUND;
   } else if (rc != MEMORY_DB_SUCCESS) {
      io->error = ERR_BUSY;
   } else {
      bool missing = false;
      if (undo_ids(io, ids, n, &missing) != SUCCESS) {
         io->error = ERR_BUSY;
      } else if (missing) {
         io->error = ERR_NOT_FOUND;
      }
   }
   free(ids);
   if (io->n_stale > 0 && io->n_undone > 0) {
      io->status = MEMORY_CONSOLIDATION_PARTIAL;
   } else if (io->n_stale > 0) {
      io->status = MEMORY_CONSOLIDATION_ALL_STALE;
      io->error = ERR_STALE;
   } else if (io->error) {
      io->status = MEMORY_CONSOLIDATION_ERROR;
   } else {
      io->status = MEMORY_CONSOLIDATION_OK; /* a whole batch with nothing left to undo, too */
   }
}

static void run_keep(memory_consolidation_outcome_t *io) {
   const int rc = memory_consolidate_keep(io->user_id, io->batch_id, io->op_id, &io->fact_id);
   io->status = rc == SUCCESS ? MEMORY_CONSOLIDATION_OK : MEMORY_CONSOLIDATION_ERROR;
   io->error = rc == SUCCESS                             ? NULL
               : rc == MEMORY_DB_NOT_FOUND               ? ERR_NOT_FOUND
               : rc == MEMORY_CONSOLIDATE_KEEP_FORGOTTEN ? ERR_KEEP_FORGOTTEN
               : rc == MEMORY_CONSOLIDATION_NOTHING      ? ERR_NO_KEEP
               : rc == MEMORY_CONSOLIDATION_FULL         ? ERR_KEEP_FULL
                                                         : ERR_KEEP_BUSY;
}

void memory_consolidation_run(memory_consolidation_outcome_t *io) {
   io->status = MEMORY_CONSOLIDATION_ERROR;
   io->n_undone = 0;
   io->n_stale = 0;
   io->fact_id = 0;
   io->error = NULL;
   const bool keep = io->action == MEMORY_CONSOLIDATION_KEEP_OP;
   if (io->user_id <= 0 || io->batch_id <= 0 || (keep && io->op_id <= 0)) {
      io->error = ERR_NOT_FOUND;
      return;
   }
   /* No extraction of the user's changes memory while this checks and writes. */
   if (!memory_extraction_hold_user(io->user_id, MEMORY_FORGET_WAIT_SEC)) {
      io->error = keep ? ERR_KEEP_BUSY : ERR_BUSY;
      return;
   }
   if (keep) {
      run_keep(io);
   } else {
      run_undo(io);
   }
   memory_extraction_release_user(io->user_id);

   const bool changed = io->n_undone > 0 || io->n_stale > 0 || (keep && io->fact_id > 0);
   memory_consolidation_batch_row_t b;
   if (changed &&
       memory_db_consolidation_batch_get(io->user_id, io->batch_id, &b) == MEMORY_DB_SUCCESS) {
      memory_consolidation_batch_notify(io->user_id, b.conversation_id, io->batch_id);
   }
   OLOG_INFO("memory_consolidation: user %d %s batch %lld op %lld: %s (%d undone, %d stale)",
             io->user_id, keep ? "keep" : "undo", (long long)io->batch_id, (long long)io->op_id,
             memory_consolidation_status_name(io->status), io->n_undone, io->n_stale);
}

typedef struct {
   memory_consolidation_outcome_t io;
   memory_consolidation_done_fn done;
   void *ctx;
} undo_job_t;

static void *undo_worker(void *arg) {
   undo_job_t *job = (undo_job_t *)arg;
   memory_consolidation_run(&job->io);
   /* Free the user's slot first: a client acting on the reply isn't told
    * it's still busy. */
   inflight_release(job->io.user_id);
   if (job->done) {
      job->done(&job->io, job->ctx);
   }
   free(job);
   return NULL;
}

int memory_consolidation_start(memory_consolidation_action_t action,
                               int user_id,
                               int64_t batch_id,
                               int64_t op_id,
                               memory_consolidation_done_fn done,
                               void *ctx) {
   if (user_id <= 0) {
      return FAILURE;
   }
   if (!inflight_claim(user_id)) {
      return MEMORY_CONSOLIDATION_BUSY;
   }
   undo_job_t *job = calloc(1, sizeof(*job));
   if (!job) {
      inflight_release(user_id);
      return FAILURE;
   }
   job->io.action = action;
   job->io.user_id = user_id;
   job->io.batch_id = batch_id;
   job->io.op_id = op_id;
   job->done = done;
   job->ctx = ctx;

   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   const int rc = pthread_create(&thread, &attr, undo_worker, job);
   pthread_attr_destroy(&attr);
   if (rc != 0) {
      OLOG_ERROR("memory_consolidation: could not start the undo worker (%d)", rc);
      inflight_release(user_id);
      free(job);
      return FAILURE;
   }
   return SUCCESS;
}
