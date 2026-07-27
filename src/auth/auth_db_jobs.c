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
 * Authentication Database - Background Job Accessors
 *
 * Read/write surface over the job lifecycle columns on `conversations` (a job
 * IS a parented conversation).  Kept out of auth_db_conv.c (already > 1500 ln)
 * for cohesion.  The shared conversation_t / conv_db_get read path is left
 * untouched — the job columns are NULL for ~every normal conversation, so the
 * job surface is its own row type (job_record_t) with its own queries.
 *
 * Statements are prepared ad-hoc (prepare/step/finalize) rather than cached in
 * s_db: every path here is low-frequency (spawn, a handful of status
 * transitions per job, the once-per-second-when-dirty monitor scan, the boot
 * scan).  Caching into s_db.stmt_* is a documented follow-up if profiling ever
 * shows it (mirrors the memory_db_alias.c note in TODO.md).
 *
 * SECURITY: All SQL is constant or parameterized — never sqlite3_mprintf with
 * user input.  System-caller setters take no user_id (internal daemon
 * transitions only); every user-facing reader binds and checks user_id.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <stdint.h> /* INT64_MAX */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "logging.h"

/* =============================================================================
 * Helpers
 * ============================================================================= */

/* Copy a possibly-NULL text column into a fixed buffer, always NUL-terminated. */
static void job_copy_text(sqlite3_stmt *st, int col, char *dst, size_t n) {
   const char *s = (const char *)sqlite3_column_text(st, col);
   if (s && n > 0) {
      strncpy(dst, s, n - 1);
      dst[n - 1] = '\0';
   } else if (n > 0) {
      dst[0] = '\0';
   }
}

/* Unpack a full job_record_t from a row selected with JOB_SELECT_COLS order. */
static void job_unpack_row(sqlite3_stmt *st, job_record_t *r) {
   memset(r, 0, sizeof(*r));
   r->id = sqlite3_column_int64(st, 0);
   r->user_id = sqlite3_column_int(st, 1);
   r->parent_id = sqlite3_column_int64(st, 2);
   job_copy_text(st, 3, r->title, sizeof(r->title));
   job_copy_text(st, 4, r->spawn_mode, sizeof(r->spawn_mode));
   job_copy_text(st, 5, r->on_complete, sizeof(r->on_complete));
   r->on_complete_fired = sqlite3_column_int(st, 6) != 0;
   job_copy_text(st, 7, r->job_status, sizeof(r->job_status));
   job_copy_text(st, 8, r->job_error, sizeof(r->job_error));
   job_copy_text(st, 9, r->deliver_to, sizeof(r->deliver_to));
   r->spawn_depth = sqlite3_column_int(st, 10);
   r->reinvoke_count = sqlite3_column_int(st, 11);
   r->started_at = (time_t)sqlite3_column_int64(st, 12);
   r->finished_at = (time_t)sqlite3_column_int64(st, 13);
   r->created_at = (time_t)sqlite3_column_int64(st, 14);
}

#define JOB_SELECT_COLS                                                                      \
   "id, user_id, parent_id, title, spawn_mode, on_complete, on_complete_fired, job_status, " \
   "job_error, deliver_to, spawn_depth, reinvoke_count, started_at, finished_at, created_at"

/* Run a parameterless-after-@nbind constant SELECT into an out[] array.  Caller
 * holds the auth_db lock.  @bind is invoked to bind params (may be NULL). */
static int job_select_into(const char *sql,
                           void (*bind)(sqlite3_stmt *st, void *ctx),
                           void *bind_ctx,
                           job_record_t *out,
                           int max,
                           int *count_out) {
   *count_out = 0;
   if (!out || max <= 0) {
      return AUTH_DB_SUCCESS;
   }
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db_jobs: prepare failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }
   if (bind) {
      bind(st, bind_ctx);
   }
   int n = 0;
   while (n < max && sqlite3_step(st) == SQLITE_ROW) {
      job_unpack_row(st, &out[n]);
      n++;
   }
   sqlite3_finalize(st);
   *count_out = n;
   return AUTH_DB_SUCCESS;
}

/* =============================================================================
 * Create
 * ============================================================================= */

int conv_db_create_job(int user_id,
                       const char *title,
                       int64_t parent_id,
                       const char *spawn_mode,
                       const char *on_complete,
                       const char *deliver_to,
                       int spawn_depth,
                       const char *goal,
                       int64_t *conv_id_out) {
   if (user_id <= 0 || !conv_id_out) {
      return AUTH_DB_INVALID;
   }

   char safe_title[CONV_TITLE_MAX];
   if (title && title[0] != '\0') {
      strncpy(safe_title, title, CONV_TITLE_MAX - 1);
      safe_title[CONV_TITLE_MAX - 1] = '\0';
   } else {
      strcpy(safe_title, "Background Job");
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Respect the per-user conversation ceiling (job conversations persist and
    * count like any other).  Job concurrency is bounded separately by the
    * [jobs] caps, but the storage limit still applies. */
   if (CONV_MAX_PER_USER > 0) {
      sqlite3_reset(s_db.stmt_conv_count);
      sqlite3_bind_int(s_db.stmt_conv_count, 1, user_id);
      if (sqlite3_step(s_db.stmt_conv_count) == SQLITE_ROW) {
         int count = sqlite3_column_int(s_db.stmt_conv_count, 0);
         if (count >= CONV_MAX_PER_USER) {
            sqlite3_reset(s_db.stmt_conv_count);
            AUTH_DB_UNLOCK();
            return AUTH_DB_LIMIT_EXCEEDED;
         }
      }
      sqlite3_reset(s_db.stmt_conv_count);
   }

   /* Inherit the parent's privacy.  A job is the private conversation's work
    * carried out elsewhere, so it must not be a public copy of it — without this
    * a private conversation's research was reachable by anything that trusts the
    * flag (source-linked recall filters on `c.is_private = 0`, and extraction
    * gates on it), because the child defaulted to 0.
    *
    * Read inline, under the lock we already hold: conv_db_is_private() takes the
    * same auth_db mutex and would deadlock here.  Ownership is bound so a
    * caller cannot inherit from someone else's conversation, and on any failure
    * the value stays `true` — a job whose parent we cannot read is treated as
    * private, because guessing wrong in that direction is the recoverable one. */
   int inherited_private = 1;
   if (parent_id > 0) {
      sqlite3_stmt *pst = NULL;
      if (sqlite3_prepare_v2(s_db.db,
                             "SELECT is_private FROM conversations WHERE id=? AND user_id=?", -1,
                             &pst, NULL) == SQLITE_OK) {
         sqlite3_bind_int64(pst, 1, parent_id);
         sqlite3_bind_int(pst, 2, user_id);
         if (sqlite3_step(pst) == SQLITE_ROW) {
            inherited_private = sqlite3_column_int(pst, 0) != 0 ? 1 : 0;
         }
         sqlite3_finalize(pst);
      }
   } else {
      inherited_private = 0; /* rootless job — no parent whose privacy to carry */
   }

   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO conversations "
       "(user_id, title, created_at, updated_at, anchor_date, origin, "
       " parent_id, spawn_mode, on_complete, deliver_to, spawn_depth, job_goal, job_status, "
       " is_private) "
       "VALUES (?, ?, ?, ?, ?, 'job', ?, ?, ?, ?, ?, ?, 'queued', ?)",
       -1, &st, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("conv_db_create_job: prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   time_t now = time(NULL);
   sqlite3_bind_int(st, 1, user_id);
   sqlite3_bind_text(st, 2, safe_title, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(st, 3, (int64_t)now);
   sqlite3_bind_int64(st, 4, (int64_t)now);
   sqlite3_bind_int64(st, 5, (int64_t)now); /* anchor_date */
   if (parent_id > 0) {
      sqlite3_bind_int64(st, 6, parent_id);
   } else {
      sqlite3_bind_null(st, 6);
   }
   sqlite3_bind_text(st, 7, spawn_mode ? spawn_mode : "detached", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 8, on_complete ? on_complete : "notify", -1, SQLITE_TRANSIENT);
   if (deliver_to && deliver_to[0] != '\0') {
      sqlite3_bind_text(st, 9, deliver_to, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(st, 9);
   }
   sqlite3_bind_int(st, 10, spawn_depth);
   /* The goal, durable from creation.  Stored here rather than left to emerge as
    * the first `messages` row, because a job that never dispatches (capacity
    * refusal, worker-spawn failure) writes no messages at all and would
    * otherwise lose the instruction entirely — see the v74 migration. */
   if (goal && goal[0] != '\0') {
      sqlite3_bind_text(st, 11, goal, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(st, 11);
   }
   sqlite3_bind_int(st, 12, inherited_private);

   rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("conv_db_create_job: insert failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   *conv_id_out = sqlite3_last_insert_rowid(s_db.db);
   AUTH_DB_UNLOCK();

   if (inherited_private) {
      OLOG_INFO("conv_db_create_job: conv %lld inherits private from parent %lld",
                (long long)*conv_id_out, (long long)parent_id);
   }
   OLOG_INFO("Created job conversation %lld for user %d (parent %lld, depth %d)",
             (long long)*conv_id_out, user_id, (long long)parent_id, spawn_depth);
   return AUTH_DB_SUCCESS;
}

/* =============================================================================
 * System-caller setters (no ownership check — internal daemon transitions)
 * ============================================================================= */

/* Shared one-shot UPDATE helper: prepare, bind via callback, step, finalize. */
static int job_update_exec(const char *sql,
                           void (*bind)(sqlite3_stmt *st, void *ctx),
                           void *bind_ctx,
                           const char *what) {
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db_jobs: prepare %s failed: %s", what, sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   if (bind) {
      bind(st, bind_ctx);
   }
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("auth_db_jobs: %s failed", what);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}

struct running_bind {
   int64_t conv_id;
   time_t started_at;
};
static void bind_running(sqlite3_stmt *st, void *ctx) {
   struct running_bind *b = ctx;
   sqlite3_bind_int64(st, 1, (int64_t)b->started_at);
   sqlite3_bind_int64(st, 2, b->conv_id);
}
int conv_db_job_set_running(int64_t conv_id, time_t started_at) {
   if (conv_id <= 0) {
      return AUTH_DB_INVALID;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   /* A CLAIM on 'queued', not a blind write.  A worker is detached, so between
    * its row being queued and its thread being scheduled the user can cancel:
    * the cancel finds no session, retires the row to 'cancelled' and reports
    * success — and then this write would resurrect it to 'running', so the job
    * runs to completion after the user was told it stopped.  The window is
    * ordinary-sized now that resume creates queued rows on demand and the panel
    * will offer Resume and Cancel on the same row.  Losing the claim is how the
    * worker learns to stand down.
    *
    * ⚠ 'queued' is NOT a value a stale worker can only see once.  Since
    * 'cancelled' became user-resumable, cancel→resume returns the row to
    * 'queued', so this predicate alone no longer proves "nobody cancelled me" —
    * a worker spawned before the cancel could claim the row spawned by the
    * resume.  What actually keeps that from happening is upstream: one live
    * session per conversation (JOB_MGR_CONV_BUSY in job_manager_begin_ex),
    * which refuses the second worker before it ever reaches this statement.  If
    * that guard is ever relaxed, this claim needs a generation/rev term rather
    * than a status term. */
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE conversations SET job_status='running', started_at=? "
                          "WHERE id=? AND job_status='queued'",
                          -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db_jobs: prepare set_running failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, (int64_t)started_at);
   sqlite3_bind_int64(st, 2, conv_id);
   int rc = sqlite3_step(st);
   int changed = sqlite3_changes(s_db.db);
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("auth_db_jobs: set_running failed for conv %lld", (long long)conv_id);
      return AUTH_DB_FAILURE;
   }
   /* NOT_FOUND = the row left 'queued' underneath us (cancelled, or already
    * claimed by another worker).  The caller must abandon the turn. */
   return changed == 1 ? AUTH_DB_SUCCESS : AUTH_DB_NOT_FOUND;
}

struct terminal_bind {
   int64_t conv_id;
   const char *status;
   const char *error;
   time_t finished_at;
};
static void bind_terminal(sqlite3_stmt *st, void *ctx) {
   struct terminal_bind *b = ctx;
   sqlite3_bind_text(st, 1, b->status, -1, SQLITE_TRANSIENT);
   if (b->error && b->error[0] != '\0') {
      /* A POSITIVE length makes sqlite3_bind_text copy exactly that many bytes —
       * it does NOT stop at the NUL — so pass the real string length (capped at
       * the column width) rather than JOB_ERROR_MAX-1, which read past short
       * literals into adjacent .rodata. */
      sqlite3_bind_text(st, 2, b->error, (int)strnlen(b->error, JOB_ERROR_MAX - 1),
                        SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(st, 2);
   }
   sqlite3_bind_int64(st, 3, (int64_t)b->finished_at);
   sqlite3_bind_int64(st, 4, b->conv_id);
}
int conv_db_job_set_terminal(int64_t conv_id,
                             const char *status,
                             const char *error_or_null,
                             time_t finished_at) {
   if (conv_id <= 0 || !status) {
      return AUTH_DB_INVALID;
   }
   struct terminal_bind b = { conv_id, status, error_or_null, finished_at };
   return job_update_exec(
       "UPDATE conversations SET job_status=?, job_error=?, finished_at=? WHERE id=?",
       bind_terminal, &b, "set_terminal");
}

static void bind_conv_id_only(sqlite3_stmt *st, void *ctx) {
   sqlite3_bind_int64(st, 1, *(int64_t *)ctx);
}
/* Firing is only ever correct for a row that is still TERMINAL.  Every caller
 * fires a job it just saw finish, but between that observation and this write a
 * resume can put the row back to 'queued' — and setting on_complete_fired=1 on
 * a freshly-resumed row makes the monitor skip it, so the resumed job completes
 * and notifies nobody.  That is exactly what reset_for_resume clears the flag to
 * prevent.  Reachable since 'cancelled' became user-resumable: the cancelling
 * worker marks fired one statement after set_terminal, and Resume is one click.
 *
 * Silently no-ops rather than erroring — job_update_exec doesn't surface the row
 * count, and "the row moved on" is the correct outcome, not a failure. */
#define JOB_FIRE_ONLY_IF_TERMINAL " AND job_status NOT IN ('queued','running')"

int conv_db_job_mark_fired(int64_t conv_id) {
   if (conv_id <= 0) {
      return AUTH_DB_INVALID;
   }
   return job_update_exec(
       "UPDATE conversations SET on_complete_fired=1 WHERE id=?" JOB_FIRE_ONLY_IF_TERMINAL,
       bind_conv_id_only, &conv_id, "mark_fired");
}

int conv_db_job_get_goal(int64_t conv_id, int user_id, char **out) {
   if (conv_id <= 0 || user_id <= 0 || !out) {
      return AUTH_DB_INVALID;
   }
   *out = NULL;
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   /* Ownership-JOINed like every other user-facing read here.  Deliberately NOT
    * part of job_record_t: a goal is free text, and job_record_t is bulk-copied
    * in arrays of up to JOBS_SNAPSHOT_HARD_MAX for the WebUI snapshot — adding
    * a text field there would multiply that buffer for a value only the resume
    * path reads. */
   if (sqlite3_prepare_v2(s_db.db, "SELECT job_goal FROM conversations WHERE id=? AND user_id=?",
                          -1, &st, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, conv_id);
   sqlite3_bind_int(st, 2, user_id);
   int rc = AUTH_DB_NOT_FOUND;
   if (sqlite3_step(st) == SQLITE_ROW) {
      const char *g = (const char *)sqlite3_column_text(st, 0);
      if (g && g[0] != '\0') {
         *out = strdup(g);
         rc = (*out != NULL) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
      } else {
         /* Row exists but has no goal — a pre-v74 job whose backfill found no
          * first user message, i.e. one that never dispatched. */
         rc = AUTH_DB_NOT_FOUND;
      }
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return rc;
}

int conv_db_job_reset_for_resume(int64_t conv_id, int user_id, bool allow_cancelled) {
   if (conv_id <= 0 || user_id <= 0) {
      return AUTH_DB_INVALID;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   /* The status predicate is the CLAIM, not a precondition checked earlier: two
    * concurrent resume requests both pass a read-then-write check, and the loser
    * would spawn a second worker onto a job the winner is already running.  Here
    * the row leaves the resumable set inside the UPDATE, so exactly one caller
    * sees changes==1.
    *
    * user_id is in the predicate too, so the claim is self-authorizing rather
    * than relying on the caller having checked — every other user-facing
    * statement in this file binds it, and this one should not be the exception.
    *
    * Clearing on_complete_fired is what actually re-arms delivery — the boot
    * interrupted-notify consumed it, and the monitor skips a fired row, so a
    * resumed job would finish and silently notify nobody.  finished_at/started_at
    * reset too, or the panel renders a job that finished before it started.
    *
    * 'cancelled' is admitted only when @p allow_cancelled.  interrupted/failed
    * mean the job stopped against the user's wishes, so restarting restores an
    * intent; cancelled means they asked it to stop, so restarting REVERSES one.
    * That difference is only safe to ignore when a human is asking — hence the
    * flag, set by the WebUI click path and cleared by the `job` tool, so the
    * model cannot restart work a person deliberately stopped.  See the
    * job_worker_resume() contract for why the split lives at the surface. */
   const char *sql =
       allow_cancelled
           ? "UPDATE conversations SET job_status='queued', job_error=NULL, "
             "started_at=0, finished_at=0, on_complete_fired=0 "
             "WHERE id=? AND user_id=? AND job_status IN ('interrupted','failed','cancelled')"
           : "UPDATE conversations SET job_status='queued', job_error=NULL, "
             "started_at=0, finished_at=0, on_complete_fired=0 "
             "WHERE id=? AND user_id=? AND job_status IN ('interrupted','failed')";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db_jobs: prepare reset_for_resume failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, conv_id);
   sqlite3_bind_int(st, 2, user_id);
   int rc = sqlite3_step(st);
   int changed = sqlite3_changes(s_db.db);
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("auth_db_jobs: reset_for_resume failed for conv %lld", (long long)conv_id);
      return AUTH_DB_FAILURE;
   }
   /* No row changed: it does not exist, is not a job, or is not in a resumable
    * state (running/queued/done, plus cancelled unless @p allow_cancelled).  The
    * caller has already ownership-checked, so this is a state answer, not an
    * authorization one. */
   return changed == 1 ? AUTH_DB_SUCCESS : AUTH_DB_NOT_FOUND;
}

/* =============================================================================
 * Ownership-checked readers
 * ============================================================================= */

int conv_db_job_get(int64_t conv_id, int user_id, job_record_t *out) {
   if (conv_id <= 0 || !out) {
      return AUTH_DB_INVALID;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, "SELECT " JOB_SELECT_COLS " FROM conversations WHERE id=?",
                               -1, &st, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, conv_id);
   rc = sqlite3_step(st);
   if (rc != SQLITE_ROW) {
      sqlite3_finalize(st);
      AUTH_DB_UNLOCK();
      return (rc == SQLITE_DONE) ? AUTH_DB_NOT_FOUND : AUTH_DB_FAILURE;
   }
   int owner_id = sqlite3_column_int(st, 1);
   const char *status = (const char *)sqlite3_column_text(st, 7);
   if (owner_id != user_id) {
      sqlite3_finalize(st);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FORBIDDEN;
   }
   if (!status || status[0] == '\0') { /* row exists but is not a job */
      sqlite3_finalize(st);
      AUTH_DB_UNLOCK();
      return AUTH_DB_NOT_FOUND;
   }
   job_unpack_row(st, out);
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return AUTH_DB_SUCCESS;
}

int conv_db_job_get_status(int64_t conv_id, int user_id, char *status_out, size_t n) {
   if (conv_id <= 0 || !status_out || n == 0) {
      return AUTH_DB_INVALID;
   }
   status_out[0] = '\0';
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, "SELECT user_id, job_status FROM conversations WHERE id=?",
                               -1, &st, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, conv_id);
   rc = sqlite3_step(st);
   if (rc != SQLITE_ROW) {
      sqlite3_finalize(st);
      AUTH_DB_UNLOCK();
      return (rc == SQLITE_DONE) ? AUTH_DB_NOT_FOUND : AUTH_DB_FAILURE;
   }
   int owner_id = sqlite3_column_int(st, 0);
   if (owner_id != user_id) {
      sqlite3_finalize(st);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FORBIDDEN;
   }
   const char *status = (const char *)sqlite3_column_text(st, 1);
   if (status) {
      strncpy(status_out, status, n - 1);
      status_out[n - 1] = '\0';
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return AUTH_DB_SUCCESS;
}

int conv_db_job_list_by_user(int user_id, job_record_t *out, int max, int *count_out) {
   if (user_id <= 0 || !count_out) {
      return AUTH_DB_INVALID;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT " JOB_SELECT_COLS " FROM conversations "
                               "WHERE user_id=? AND job_status IS NOT NULL "
                               "ORDER BY created_at DESC LIMIT ?",
                               -1, &st, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int(st, 1, user_id);
   sqlite3_bind_int(st, 2, max);
   int n = 0;
   while (n < max && sqlite3_step(st) == SQLITE_ROW) {
      job_unpack_row(st, &out[n]);
      n++;
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   *count_out = n;
   return AUTH_DB_SUCCESS;
}

/* =============================================================================
 * System-caller scans (monitor + boot) — copy-out under the lock
 * ============================================================================= */

struct limit_bind {
   int limit;
};
static void bind_limit(sqlite3_stmt *st, void *ctx) {
   sqlite3_bind_int(st, 1, ((struct limit_bind *)ctx)->limit);
}

int conv_db_job_list_pending_followups(int max, job_record_t *out, int *count_out) {
   if (!count_out) {
      return AUTH_DB_INVALID;
   }
   struct limit_bind b = { max };
   AUTH_DB_LOCK_OR_FAIL();
   int rc = job_select_into("SELECT " JOB_SELECT_COLS " FROM conversations "
                            "WHERE job_status IN ('done','failed','interrupted') "
                            "AND on_complete_fired=0 "
                            "ORDER BY finished_at ASC LIMIT ?",
                            bind_limit, &b, out, max, count_out);
   AUTH_DB_UNLOCK();
   return rc;
}

int conv_db_job_pending_reinvoke_for_parent(int64_t parent_id,
                                            int user_id,
                                            job_record_t *out,
                                            int max,
                                            int *count_out) {
   if (parent_id <= 0 || user_id <= 0 || !count_out) {
      return AUTH_DB_INVALID;
   }
   *count_out = 0;
   if (!out || max <= 0) {
      return AUTH_DB_SUCCESS;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT " JOB_SELECT_COLS " FROM conversations "
                               "WHERE parent_id=? AND user_id=? "
                               "AND job_status IN ('done','failed','interrupted') "
                               "AND on_complete_fired=0 AND on_complete='reinvoke_parent' "
                               "ORDER BY finished_at ASC LIMIT ?",
                               -1, &st, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("conv_db_job_pending_reinvoke_for_parent: prepare failed: %s",
                 sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, parent_id);
   sqlite3_bind_int(st, 2, user_id);
   sqlite3_bind_int(st, 3, max);
   int n = 0;
   while (n < max && sqlite3_step(st) == SQLITE_ROW) {
      job_unpack_row(st, &out[n]);
      n++;
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   *count_out = n;
   return AUTH_DB_SUCCESS;
}

int conv_db_job_scan_active(job_record_t *out, int max, int *count_out) {
   if (!count_out) {
      return AUTH_DB_INVALID;
   }
   struct limit_bind b = { max };
   AUTH_DB_LOCK_OR_FAIL();
   int rc = job_select_into("SELECT " JOB_SELECT_COLS " FROM conversations "
                            "WHERE job_status IN ('running','queued') "
                            "ORDER BY created_at ASC LIMIT ?",
                            bind_limit, &b, out, max, count_out);
   AUTH_DB_UNLOCK();
   return rc;
}

/* =============================================================================
 * Reinvoke support (Phase 3 — reinvoke_parent)
 * ============================================================================= */

int conv_db_job_mark_fired_many(const int64_t *ids, int n) {
   if (n <= 0) {
      return AUTH_DB_SUCCESS; /* nothing to do */
   }
   if (ids == NULL) {
      return AUTH_DB_INVALID;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE conversations SET on_complete_fired=1 "
                          "WHERE id=?" JOB_FIRE_ONLY_IF_TERMINAL,
                          -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("conv_db_job_mark_fired_many: prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   if (sqlite3_exec(s_db.db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) {
      OLOG_ERROR("conv_db_job_mark_fired_many: BEGIN failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_finalize(st);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   int rc = AUTH_DB_SUCCESS;
   for (int i = 0; i < n; i++) {
      sqlite3_reset(st);
      sqlite3_bind_int64(st, 1, ids[i]);
      if (sqlite3_step(st) != SQLITE_DONE) {
         OLOG_ERROR("conv_db_job_mark_fired_many: update %lld failed", (long long)ids[i]);
         rc = AUTH_DB_FAILURE;
         break;
      }
   }
   sqlite3_exec(s_db.db, (rc == AUTH_DB_SUCCESS) ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return rc;
}

int conv_db_job_bump_reinvoke(int64_t conv_id, int *new_count_out) {
   if (conv_id <= 0) {
      return AUTH_DB_INVALID;
   }
   if (new_count_out) {
      *new_count_out = 0;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   /* UPDATE ... RETURNING: one prepare + step, no read-after-write gap. */
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE conversations SET reinvoke_count = reinvoke_count + 1 "
                          "WHERE id=? RETURNING reinvoke_count",
                          -1, &st, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, conv_id);
   int step = sqlite3_step(st);
   int rc = AUTH_DB_SUCCESS;
   if (step == SQLITE_ROW) {
      if (new_count_out) {
         *new_count_out = sqlite3_column_int(st, 0);
      }
   } else {
      rc = AUTH_DB_FAILURE; /* no such row / error */
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return rc;
}

int conv_db_job_downgrade_boot_reinvokes(int *count_out) {
   if (count_out) {
      *count_out = 0;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   /* DOWNGRADE to 'notify' — do NOT set on_complete_fired.
    *
    * This used to mark the row fired, which did suppress the cross-restart
    * re-engagement it was written for, but `on_complete_fired` answers two
    * different questions with one bit: "has the parent been re-engaged?" and
    * "has the user been told?".  Firing the row answered both, so a job that
    * finished moments before a restart was silently swallowed — no reinvoke
    * (correct) and no notification either (not intended, and the user has no
    * other way to learn it finished).
    *
    * Changing the DISPOSITION instead separates them: the reinvoke can never
    * happen because the row is no longer a reinvoke_parent row, while
    * on_complete_fired stays 0 so the monitor still owes — and delivers — a
    * completion notice through the ordinary notify path.  No schema change, and
    * the row still drains from the finished_at-ordered queue rather than
    * head-of-line-blocking it. */
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE conversations SET on_complete='notify' "
                          "WHERE job_status IN ('done','failed','interrupted') "
                          "AND on_complete_fired=0 AND on_complete='reinvoke_parent'",
                          -1, &st, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   if (count_out) {
      *count_out = sqlite3_changes(s_db.db);
   }
   AUTH_DB_UNLOCK();
   return AUTH_DB_SUCCESS;
}

/* =============================================================================
 * The lifetime split: ACTIVE is a bounded set, HISTORY is an unbounded feed
 *
 * These two readers exist as a pair, and the difference between them is the
 * whole reason the WebUI's job frames are shaped the way they are (§6.4).
 *
 * A user's ACTIVE jobs are structurally bounded: job_manager gates every
 * reservation on the global `max_active_jobs` (clamped to <= 256) before the
 * per-user cap, so `_list_active_by_user` can never truncate in practice and its
 * result is a complete SET.  That is what makes it safe to derive the "N jobs
 * running" pill counts from these rows client-side — the property the counts
 * used to be computed server-side to guarantee.
 *
 * HISTORY grows without bound, so `_list_history_by_user` is keyset-paginated
 * and must never feed a count.  Deriving a total from a LIMITed page would
 * silently under-report, which is exactly the bug the split prevents.
 * ============================================================================= */

/* The active/terminal boundary, defined ONCE.  The two readers below are exact
 * complements of each other — one uses this predicate, the other negates it —
 * and the partition being exact is what the WebUI frames rely on: a status
 * listed in one but not the other puts a job in both lists or in neither, which
 * surfaces as a wrong count rather than as an obvious failure.  A new
 * non-terminal status (e.g. a paused state) is a one-line change here.
 * NOTE: www/js/ui/jobs-activity.js mirrors this — keep isActive() in sync. */
#define JOB_ACTIVE_STATUSES "('queued','running')"

struct job_user_limit_bind {
   int user_id;
   int limit;
};

static void bind_user_limit(sqlite3_stmt *st, void *ctx) {
   struct job_user_limit_bind *b = ctx;
   sqlite3_bind_int(st, 1, b->user_id);
   sqlite3_bind_int(st, 2, b->limit);
}

int conv_db_job_list_active_by_user(int user_id, job_record_t *out, int max, int *count_out) {
   if (user_id <= 0 || !out || max <= 0 || !count_out) {
      return AUTH_DB_INVALID;
   }
   AUTH_DB_LOCK_OR_FAIL();
   struct job_user_limit_bind b = { .user_id = user_id, .limit = max };
   /* Oldest-first: the set is unordered as far as correctness goes, but a
    * longest-running-at-top list is what a watcher expects to read. */
   int rc = job_select_into("SELECT " JOB_SELECT_COLS " FROM conversations "
                            "WHERE user_id=? AND job_status IN " JOB_ACTIVE_STATUSES " "
                            "ORDER BY created_at ASC, id ASC LIMIT ?",
                            bind_user_limit, &b, out, max, count_out);
   AUTH_DB_UNLOCK();
   return rc;
}

struct job_history_bind {
   int user_id;
   int64_t before_created_at;
   int64_t before_id;
   int limit;
};

static void bind_history(sqlite3_stmt *st, void *ctx) {
   struct job_history_bind *b = ctx;
   sqlite3_bind_int(st, 1, b->user_id);
   sqlite3_bind_int64(st, 2, b->before_created_at);
   sqlite3_bind_int64(st, 3, b->before_id);
   sqlite3_bind_int(st, 4, b->limit);
}

int conv_db_job_list_history_by_user(int user_id,
                                     int64_t before_created_at,
                                     int64_t before_id,
                                     job_record_t *out,
                                     int max,
                                     int *count_out) {
   if (user_id <= 0 || !out || max <= 0 || !count_out) {
      return AUTH_DB_INVALID;
   }
   AUTH_DB_LOCK_OR_FAIL();
   /* A first page (cursor 0) starts past the newest possible row rather than
    * needing a second SQL variant. */
   struct job_history_bind b = {
      .user_id = user_id,
      .before_created_at = before_created_at > 0 ? before_created_at : INT64_MAX,
      .before_id = before_created_at > 0 ? before_id : INT64_MAX,
      .limit = max,
   };
   /* Keyset on (created_at, id), not OFFSET: a job reaching a terminal state
    * between two pages would otherwise shift every later row and duplicate or
    * skip one.  The id tiebreak makes the order total — several jobs finishing
    * inside the same second is the common case, not the rare one.  Numbered
    * placeholders because the cursor timestamp appears twice. */
   int rc = job_select_into("SELECT " JOB_SELECT_COLS " FROM conversations "
                            "WHERE user_id=?1 AND job_status IS NOT NULL "
                            "AND job_status NOT IN " JOB_ACTIVE_STATUSES " "
                            "AND (created_at < ?2 OR (created_at = ?2 AND id < ?3)) "
                            "ORDER BY created_at DESC, id DESC LIMIT ?4",
                            bind_history, &b, out, max, count_out);
   AUTH_DB_UNLOCK();
   return rc;
}

int conv_db_job_last_assistant_text(int64_t conv_id, int user_id, char **out) {
   if (conv_id <= 0 || user_id <= 0 || !out) {
      return AUTH_DB_INVALID;
   }
   *out = NULL;
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT m.content FROM messages m "
                          "JOIN conversations c ON c.id = m.conversation_id "
                          "WHERE m.conversation_id=? AND c.user_id=? AND m.role='assistant' "
                          "ORDER BY m.id DESC LIMIT 1",
                          -1, &st, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, conv_id);
   sqlite3_bind_int(st, 2, user_id);
   int step = sqlite3_step(st);
   int rc = AUTH_DB_SUCCESS;
   if (step == SQLITE_ROW) {
      const char *content = (const char *)sqlite3_column_text(st, 0);
      if (content != NULL) {
         char *dup = strdup(content);
         if (dup == NULL) {
            rc = AUTH_DB_FAILURE;
         } else {
            *out = dup;
         }
      }
   } else if (step != SQLITE_DONE) {
      rc = AUTH_DB_FAILURE;
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return rc;
}
