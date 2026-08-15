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
 * Authentication Database - Deep-Research Accessors (v75)
 *
 * Read/write surface over the four research_* tables (runs/questions/claims/
 * report_revisions).  A research run's ENTIRE state is SQLite rows (design
 * invariant), so this layer IS the run: restart-durable, and the report is a
 * view over research_claims.
 *
 * Statements are prepared ad-hoc (prepare/step/finalize) rather than cached in
 * s_db, matching auth_db_jobs.c: every path is low-frequency (a handful of
 * writes per round, a few rounds per run).  A batched claim path
 * (research_db_claims_add, one BEGIN/COMMIT for N rows) EXISTS for a caller that
 * accumulates claims in memory — but the live loop does NOT use it: research_record
 * records ONE claim per tool call (via research_db_claim_add → the n=1 path), each
 * its own transaction.  That stays cheap because WAL + synchronous=NORMAL make a
 * COMMIT a lock-held frame append with no fsync, and claims are dispersed across
 * seconds of LLM latency, so they never burst against the global auth_db mutex.
 *
 * SECURITY: All SQL is constant or parameterized.  research_db_run_get() is the
 * only user-facing reader and binds user_id; every other function is a system
 * caller (worker/controller/renderer) operating on a run_id it already
 * ownership-checked — mirroring the job system-setters that take no user_id.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "logging.h"

/* Column projections, kept adjacent to the unpackers that read them. */
#define RESEARCH_RUN_COLS                                                           \
   "id, conversation_id, user_id, brief, mode, status, report_doc_id, rounds_run, " \
   "tool_calls, input_tokens, stop_reason, created_at, finished_at"
#define RESEARCH_QUESTION_COLS \
   "id, run_id, question, status, confidence, parent_qid, created_at, resolution_reason"
#define RESEARCH_CLAIM_COLS \
   "id, run_id, question_id, claim, source_url, source_kind, quote, round, created_at"
/* Report/digest projection: the only claim fields those readers consume
 * (research_render_report uses question_id/claim/source_url; the digest gloss uses
 * claim).  Omitting the two 2 KB text columns (quote, source_kind) skips fetching +
 * copying ~4 KB/row of data that is never rendered — over a 256-claim report
 * snapshot that is ~512 KB of quote alone.  Paired with res_unpack_claim_light. */
#define RESEARCH_CLAIM_LIGHT_COLS "question_id, claim, source_url"

/* =============================================================================
 * Helpers
 * ============================================================================= */

/* Copy a possibly-NULL text column into a fixed buffer, always NUL-terminated. */
static void res_copy_text(sqlite3_stmt *st, int col, char *dst, size_t n) {
   const char *s = (const char *)sqlite3_column_text(st, col);
   if (s && n > 0) {
      strncpy(dst, s, n - 1);
      dst[n - 1] = '\0';
   } else if (n > 0) {
      dst[0] = '\0';
   }
}

/* A nullable INTEGER column as int64, mapping SQL NULL -> 0. */
static int64_t res_col_int64_or_zero(sqlite3_stmt *st, int col) {
   if (sqlite3_column_type(st, col) == SQLITE_NULL) {
      return 0;
   }
   return sqlite3_column_int64(st, col);
}

static void res_unpack_run(sqlite3_stmt *st, research_run_t *r) {
   memset(r, 0, sizeof(*r));
   r->id = sqlite3_column_int64(st, 0);
   r->conversation_id = sqlite3_column_int64(st, 1);
   r->user_id = sqlite3_column_int(st, 2);
   res_copy_text(st, 3, r->brief, sizeof(r->brief));
   res_copy_text(st, 4, r->mode, sizeof(r->mode));
   res_copy_text(st, 5, r->status, sizeof(r->status));
   r->report_doc_id = res_col_int64_or_zero(st, 6);
   r->rounds_run = sqlite3_column_int(st, 7);
   r->tool_calls = sqlite3_column_int(st, 8);
   r->input_tokens = sqlite3_column_int64(st, 9);
   res_copy_text(st, 10, r->stop_reason, sizeof(r->stop_reason));
   r->created_at = (time_t)sqlite3_column_int64(st, 11);
   r->finished_at = (time_t)res_col_int64_or_zero(st, 12);
}

static void res_unpack_question(sqlite3_stmt *st, research_question_t *q) {
   memset(q, 0, sizeof(*q));
   q->id = sqlite3_column_int64(st, 0);
   q->run_id = sqlite3_column_int64(st, 1);
   res_copy_text(st, 2, q->question, sizeof(q->question));
   res_copy_text(st, 3, q->status, sizeof(q->status));
   q->confidence = sqlite3_column_double(st, 4);
   q->parent_qid = res_col_int64_or_zero(st, 5);
   q->created_at = (time_t)sqlite3_column_int64(st, 6);
   res_copy_text(st, 7, q->resolution_reason, sizeof(q->resolution_reason));
}

static void res_unpack_claim(sqlite3_stmt *st, research_claim_t *c) {
   memset(c, 0, sizeof(*c));
   c->id = sqlite3_column_int64(st, 0);
   c->run_id = sqlite3_column_int64(st, 1);
   c->question_id = res_col_int64_or_zero(st, 2);
   res_copy_text(st, 3, c->claim, sizeof(c->claim));
   res_copy_text(st, 4, c->source_url, sizeof(c->source_url));
   res_copy_text(st, 5, c->source_kind, sizeof(c->source_kind));
   res_copy_text(st, 6, c->quote, sizeof(c->quote));
   c->round = sqlite3_column_int(st, 7);
   c->created_at = (time_t)sqlite3_column_int64(st, 8);
}

/* Unpack the RESEARCH_CLAIM_LIGHT_COLS projection — question_id/claim/source_url
 * only.  memset zeroes the fields the projection omits (quote/source_kind/round/…)
 * so a stack-allocated research_claim_t (the digest gloss) has no uninitialized
 * reads even though the caller never touches those fields. */
static void res_unpack_claim_light(sqlite3_stmt *st, research_claim_t *c) {
   memset(c, 0, sizeof(*c));
   c->question_id = res_col_int64_or_zero(st, 0);
   res_copy_text(st, 1, c->claim, sizeof(c->claim));
   res_copy_text(st, 2, c->source_url, sizeof(c->source_url));
}

/* Run a constant one-column-int UPDATE-by-id, returning SUCCESS on DONE.  @bind
 * binds every parameter (id last, by convention of the caller's SQL). */
static int res_exec_update(const char *sql,
                           void (*bind)(sqlite3_stmt *, void *),
                           void *ctx,
                           const char *what) {
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db_research: %s prepare failed: %s", what, sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   bind(st, ctx);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE) {
      /* Read errmsg while still holding the lock — the shared handle's error
       * state is only stable under the mutex. */
      OLOG_ERROR("auth_db_research: %s failed: %s", what, sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   AUTH_DB_UNLOCK();
   return AUTH_DB_SUCCESS;
}

/* =============================================================================
 * Runs
 * ============================================================================= */

int research_db_run_create(int user_id,
                           int64_t conversation_id,
                           const char *brief,
                           const char *mode,
                           int64_t *run_id_out) {
   if (user_id <= 0 || conversation_id <= 0 || !brief || !run_id_out) {
      return AUTH_DB_INVALID;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "INSERT INTO research_runs "
                               "(conversation_id, user_id, brief, mode, status, created_at) "
                               "VALUES (?, ?, ?, ?, 'planning', ?)",
                               -1, &st, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("research_db_run_create: prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, conversation_id);
   sqlite3_bind_int(st, 2, user_id);
   sqlite3_bind_text(st, 3, brief, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 4, (mode && mode[0]) ? mode : "web", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(st, 5, (int64_t)time(NULL));
   rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("research_db_run_create: insert failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   *run_id_out = sqlite3_last_insert_rowid(s_db.db);
   AUTH_DB_UNLOCK();
   OLOG_INFO("research_db: created run %lld (conv %lld, user %d)", (long long)*run_id_out,
             (long long)conversation_id, user_id);
   return AUTH_DB_SUCCESS;
}

/* Shared body for the two run getters: prepare @sql, bind @key, unpack one row. */
static int res_run_get_by(const char *sql,
                          int64_t key,
                          int key2_present,
                          int key2,
                          research_run_t *out) {
   if (!out) {
      return AUTH_DB_INVALID;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, key);
   if (key2_present) {
      sqlite3_bind_int(st, 2, key2);
   }
   int rc = sqlite3_step(st);
   int result;
   if (rc == SQLITE_ROW) {
      res_unpack_run(st, out);
      result = AUTH_DB_SUCCESS;
   } else if (rc == SQLITE_DONE) {
      result = AUTH_DB_NOT_FOUND;
   } else {
      result = AUTH_DB_FAILURE;
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return result;
}

int research_db_run_get(int64_t run_id, int user_id, research_run_t *out) {
   if (run_id <= 0 || user_id <= 0) {
      return AUTH_DB_INVALID;
   }
   return res_run_get_by("SELECT " RESEARCH_RUN_COLS " FROM research_runs WHERE id=? AND user_id=?",
                         run_id, 1, user_id, out);
}

int research_db_run_get_by_conversation(int64_t conversation_id, research_run_t *out) {
   if (conversation_id <= 0) {
      return AUTH_DB_INVALID;
   }
   return res_run_get_by("SELECT " RESEARCH_RUN_COLS " FROM research_runs WHERE conversation_id=?",
                         conversation_id, 0, 0, out);
}

struct run_status_bind {
   const char *status;
   int64_t run_id;
};
static void bind_run_status(sqlite3_stmt *st, void *ctx) {
   struct run_status_bind *b = ctx;
   sqlite3_bind_text(st, 1, b->status, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(st, 2, b->run_id);
}

int research_db_run_set_status(int64_t run_id, const char *status) {
   if (run_id <= 0 || !status || !status[0]) {
      return AUTH_DB_INVALID;
   }
   struct run_status_bind b = { .status = status, .run_id = run_id };
   return res_exec_update("UPDATE research_runs SET status=? WHERE id=?", bind_run_status, &b,
                          "run_set_status");
}

struct run_doc_bind {
   int64_t report_doc_id;
   int64_t run_id;
};
static void bind_run_doc(sqlite3_stmt *st, void *ctx) {
   struct run_doc_bind *b = ctx;
   if (b->report_doc_id > 0) {
      sqlite3_bind_int64(st, 1, b->report_doc_id);
   } else {
      sqlite3_bind_null(st, 1);
   }
   sqlite3_bind_int64(st, 2, b->run_id);
}

int research_db_run_set_report_doc(int64_t run_id, int64_t report_doc_id) {
   if (run_id <= 0) {
      return AUTH_DB_INVALID;
   }
   struct run_doc_bind b = { .report_doc_id = report_doc_id, .run_id = run_id };
   return res_exec_update("UPDATE research_runs SET report_doc_id=? WHERE id=?", bind_run_doc, &b,
                          "run_set_report_doc");
}

struct run_progress_bind {
   int rounds_run;
   int tool_calls;
   int64_t input_tokens;
   int64_t run_id;
};
static void bind_run_progress(sqlite3_stmt *st, void *ctx) {
   struct run_progress_bind *b = ctx;
   sqlite3_bind_int(st, 1, b->rounds_run);
   sqlite3_bind_int(st, 2, b->tool_calls);
   sqlite3_bind_int64(st, 3, b->input_tokens);
   sqlite3_bind_int64(st, 4, b->run_id);
}

int research_db_run_update_progress(int64_t run_id,
                                    int rounds_run,
                                    int tool_calls,
                                    int64_t input_tokens) {
   if (run_id <= 0) {
      return AUTH_DB_INVALID;
   }
   struct run_progress_bind b = { .rounds_run = rounds_run,
                                  .tool_calls = tool_calls,
                                  .input_tokens = input_tokens,
                                  .run_id = run_id };
   return res_exec_update(
       "UPDATE research_runs SET rounds_run=?, tool_calls=?, input_tokens=? WHERE id=?",
       bind_run_progress, &b, "run_update_progress");
}

struct run_terminal_bind {
   const char *status;
   const char *stop_reason;
   time_t finished_at;
   int64_t run_id;
};
static void bind_run_terminal(sqlite3_stmt *st, void *ctx) {
   struct run_terminal_bind *b = ctx;
   sqlite3_bind_text(st, 1, b->status, -1, SQLITE_TRANSIENT);
   if (b->stop_reason && b->stop_reason[0]) {
      sqlite3_bind_text(st, 2, b->stop_reason, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(st, 2);
   }
   sqlite3_bind_int64(st, 3, (int64_t)b->finished_at);
   sqlite3_bind_int64(st, 4, b->run_id);
}

int research_db_run_set_terminal(int64_t run_id,
                                 const char *status,
                                 const char *stop_reason,
                                 time_t finished_at) {
   if (run_id <= 0 || !status || !status[0]) {
      return AUTH_DB_INVALID;
   }
   struct run_terminal_bind b = { .status = status,
                                  .stop_reason = stop_reason,
                                  .finished_at = finished_at,
                                  .run_id = run_id };
   return res_exec_update(
       "UPDATE research_runs SET status=?, stop_reason=?, finished_at=? WHERE id=?",
       bind_run_terminal, &b, "run_set_terminal");
}

int research_db_reconcile_orphaned(time_t finished_at, int *count_out) {
   if (count_out) {
      *count_out = 0;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   /* Mark every non-terminal run whose job conversation is already terminal as
    * 'interrupted'.  This only fires after a HARD kill (SIGKILL / crash / power
    * loss): a graceful stop runs the worker's disposition, which sets the run row
    * terminal BEFORE the job row (research-first ordering), so during normal
    * operation a terminal job always has a terminal run.  The stranded case is the
    * boot scan having just marked the dead job 'interrupted' while the run row
    * still reads 'planning'/'researching' — without this the run is an
    * un-cancellable zombie the status/cancel surfaces can never clear.  Run AFTER
    * the job boot scan so the job rows are already terminal. */
   /* finished_at is nullable with no default and run_create never sets it, so a
    * live run's row has finished_at = SQL NULL (not 0).  `finished_at = 0` would
    * never match it (NULL = 0 is NULL, not true), so the predicate must test IS
    * NULL as well or the reconcile silently repairs nothing — leaving the exact
    * planning/researching zombies this exists to clear.  The `= 0` arm stays for any
    * legacy row that stored a literal 0. */
   const char *sql = "UPDATE research_runs SET status='interrupted', stop_reason='interrupted', "
                     "finished_at=? "
                     "WHERE (finished_at IS NULL OR finished_at=0) AND conversation_id IN "
                     "(SELECT id FROM conversations WHERE job_status NOT IN ('queued','running'))";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("research_db_reconcile_orphaned: prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, (int64_t)finished_at);
   int rc = sqlite3_step(st);
   int changed = sqlite3_changes(s_db.db);
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("research_db_reconcile_orphaned: update failed");
      return AUTH_DB_FAILURE;
   }
   if (count_out) {
      *count_out = changed;
   }
   return AUTH_DB_SUCCESS;
}

/* =============================================================================
 * Questions (coverage ledger)
 * ============================================================================= */

int research_db_question_add(int64_t run_id,
                             const char *question,
                             int64_t parent_qid,
                             int64_t *qid_out) {
   if (run_id <= 0 || !question || !question[0]) {
      return AUTH_DB_INVALID;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "INSERT INTO research_questions "
                               "(run_id, question, status, confidence, parent_qid, created_at) "
                               "VALUES (?, ?, 'open', 0.0, ?, ?)",
                               -1, &st, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("research_db_question_add: prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, run_id);
   sqlite3_bind_text(st, 2, question, -1, SQLITE_TRANSIENT);
   if (parent_qid > 0) {
      sqlite3_bind_int64(st, 3, parent_qid);
   } else {
      sqlite3_bind_null(st, 3);
   }
   sqlite3_bind_int64(st, 4, (int64_t)time(NULL));
   rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("research_db_question_add: insert failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   if (qid_out) {
      *qid_out = sqlite3_last_insert_rowid(s_db.db);
   }
   AUTH_DB_UNLOCK();
   return AUTH_DB_SUCCESS;
}

struct qstatus_bind {
   const char *status;
   double confidence;
   const char *reason; /**< NULL -> SQL NULL (status carries no reason) */
   int64_t qid;
};
static void bind_qstatus(sqlite3_stmt *st, void *ctx) {
   struct qstatus_bind *b = ctx;
   sqlite3_bind_text(st, 1, b->status, -1, SQLITE_TRANSIENT);
   sqlite3_bind_double(st, 2, b->confidence);
   /* NULL reason -> SQL NULL (resolution_reason stays empty for open/answered). */
   if (b->reason != NULL) {
      sqlite3_bind_text(st, 3, b->reason, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(st, 3);
   }
   sqlite3_bind_int64(st, 4, b->qid);
}

int research_db_question_set_status(int64_t qid,
                                    const char *status,
                                    double confidence,
                                    const char *reason) {
   if (qid <= 0 || !status || !status[0]) {
      return AUTH_DB_INVALID;
   }
   struct qstatus_bind b = { .status = status,
                             .confidence = confidence,
                             .reason = reason,
                             .qid = qid };
   return res_exec_update(
       "UPDATE research_questions SET status=?, confidence=?, resolution_reason=? WHERE id=?",
       bind_qstatus, &b, "question_set_status");
}

int research_db_question_list(int64_t run_id, research_question_t *out, int max, int *count_out) {
   if (run_id <= 0 || !out || max <= 0 || !count_out) {
      return AUTH_DB_INVALID;
   }
   *count_out = 0;
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT " RESEARCH_QUESTION_COLS " FROM research_questions "
                               "WHERE run_id=? ORDER BY id ASC LIMIT ?",
                               -1, &st, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, run_id);
   sqlite3_bind_int(st, 2, max);
   int n = 0;
   while (n < max && sqlite3_step(st) == SQLITE_ROW) {
      res_unpack_question(st, &out[n]);
      n++;
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   *count_out = n;
   return AUTH_DB_SUCCESS;
}

int research_db_question_coverage(int64_t run_id, int64_t qid, int *distinct_sources_out) {
   if (run_id <= 0 || qid <= 0 || !distinct_sources_out) {
      return AUTH_DB_INVALID;
   }
   *distinct_sources_out = 0;
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   /* COUNT(DISTINCT source_url) ignores NULLs, so private/memory claims (NULL
    * source_url) correctly do not count as independent web sources.  Constrain
    * run_id first so this seeks idx_research_claims_run (run_id, question_id)
    * rather than full-scanning the globally-growing research_claims table. */
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT COUNT(DISTINCT source_url) FROM research_claims "
                               "WHERE run_id=? AND question_id=?",
                               -1, &st, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, run_id);
   sqlite3_bind_int64(st, 2, qid);
   /* A COUNT always yields exactly one ROW; a non-ROW step is a genuine step-level
    * failure (IO error / corruption).  Report it as FAILURE rather than a
    * trustworthy 0 — research_retire_stale_questions skips a question on any
    * != SUCCESS, so returning SUCCESS-with-0 here would read as a "dry round" and
    * could advance a still-answerable question's stale streak toward auto-retire. */
   int step = sqlite3_step(st);
   if (step == SQLITE_ROW) {
      *distinct_sources_out = sqlite3_column_int(st, 0);
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return (step == SQLITE_ROW) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int research_db_question_belongs(int64_t run_id, int64_t qid, bool *out) {
   if (!out) {
      return AUTH_DB_INVALID;
   }
   *out = false;
   if (run_id <= 0 || qid <= 0) {
      return AUTH_DB_INVALID;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, "SELECT 1 FROM research_questions WHERE id=? AND run_id=?",
                               -1, &st, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, qid);
   sqlite3_bind_int64(st, 2, run_id);
   if (sqlite3_step(st) == SQLITE_ROW) {
      *out = true;
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return AUTH_DB_SUCCESS;
}

int research_db_question_get(int64_t run_id, int64_t qid, research_question_t *out) {
   if (!out) {
      return AUTH_DB_INVALID;
   }
   memset(out, 0, sizeof(*out));
   if (run_id <= 0 || qid <= 0) {
      return AUTH_DB_INVALID;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT " RESEARCH_QUESTION_COLS
                               " FROM research_questions WHERE id=? AND run_id=?",
                               -1, &st, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, qid);
   sqlite3_bind_int64(st, 2, run_id);
   int result = AUTH_DB_NOT_FOUND;
   if (sqlite3_step(st) == SQLITE_ROW) {
      res_unpack_question(st, out);
      result = AUTH_DB_SUCCESS;
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return result;
}

/* =============================================================================
 * Claims (evidence)
 * ============================================================================= */

/* Bind one claim row into a prepared INSERT with the column order:
 * (run_id, question_id, claim, source_url, source_kind, quote, round, created_at).
 *
 * The struct's char[] fields are bound with an explicit strnlen-bounded length
 * (not -1): a direct batch caller filling a research_claim_t from scraped/LLM
 * content that failed to NUL-terminate would otherwise make sqlite read past the
 * buffer.  strnlen caps the read at the field's own size regardless. */
static void res_bind_claim(sqlite3_stmt *st,
                           int64_t run_id,
                           const research_claim_t *c,
                           time_t now) {
   sqlite3_bind_int64(st, 1, run_id);
   if (c->question_id > 0) {
      sqlite3_bind_int64(st, 2, c->question_id);
   } else {
      sqlite3_bind_null(st, 2);
   }
   sqlite3_bind_text(st, 3, c->claim, (int)strnlen(c->claim, sizeof(c->claim)), SQLITE_TRANSIENT);
   if (c->source_url[0]) {
      sqlite3_bind_text(st, 4, c->source_url, (int)strnlen(c->source_url, sizeof(c->source_url)),
                        SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(st, 4);
   }
   if (c->source_kind[0]) {
      sqlite3_bind_text(st, 5, c->source_kind, (int)strnlen(c->source_kind, sizeof(c->source_kind)),
                        SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_text(st, 5, "web", -1, SQLITE_TRANSIENT);
   }
   if (c->quote[0]) {
      sqlite3_bind_text(st, 6, c->quote, (int)strnlen(c->quote, sizeof(c->quote)),
                        SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(st, 6);
   }
   sqlite3_bind_int(st, 7, c->round);
   sqlite3_bind_int64(st, 8, (int64_t)now);
}

#define RESEARCH_CLAIM_INSERT_SQL                                                     \
   "INSERT INTO research_claims "                                                     \
   "(run_id, question_id, claim, source_url, source_kind, quote, round, created_at) " \
   "VALUES (?, ?, ?, ?, ?, ?, ?, ?)"

int research_db_claim_add(int64_t run_id,
                          int64_t question_id,
                          const char *claim,
                          const char *source_url,
                          const char *source_kind,
                          const char *quote,
                          int round) {
   if (run_id <= 0 || !claim || !claim[0]) {
      return AUTH_DB_INVALID;
   }
   /* Marshal into a research_claim_t so the single + batch paths share res_bind_claim. */
   research_claim_t c;
   memset(&c, 0, sizeof(c));
   c.question_id = question_id;
   strncpy(c.claim, claim, sizeof(c.claim) - 1);
   if (source_url) {
      strncpy(c.source_url, source_url, sizeof(c.source_url) - 1);
   }
   if (source_kind) {
      strncpy(c.source_kind, source_kind, sizeof(c.source_kind) - 1);
   }
   if (quote) {
      strncpy(c.quote, quote, sizeof(c.quote) - 1);
   }
   c.round = round;
   return research_db_claims_add(run_id, &c, 1);
}

int research_db_claims_add(int64_t run_id, const research_claim_t *claims, int n) {
   if (run_id <= 0 || !claims || n <= 0) {
      return AUTH_DB_INVALID;
   }
   AUTH_DB_LOCK_OR_FAIL();
   char *errmsg = NULL;
   if (sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("research_db_claims_add: BEGIN failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, RESEARCH_CLAIM_INSERT_SQL, -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("research_db_claims_add: prepare failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   time_t now = time(NULL);
   int ok = 1;
   int inserted = 0;
   for (int i = 0; i < n; i++) {
      if (!claims[i].claim[0]) {
         continue; /* skip empty claims defensively */
      }
      sqlite3_reset(st);
      sqlite3_clear_bindings(st);
      res_bind_claim(st, run_id, &claims[i], now);
      if (sqlite3_step(st) != SQLITE_DONE) {
         OLOG_ERROR("research_db_claims_add: insert %d failed: %s", i, sqlite3_errmsg(s_db.db));
         ok = 0;
         break;
      }
      inserted++;
   }
   sqlite3_finalize(st);
   if (!ok) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   /* A non-empty batch that produced zero inserts is all-empty garbage — reject
    * it, matching research_db_claim_add's rejection of an empty claim, rather
    * than committing nothing and reporting success. */
   if (inserted == 0) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return AUTH_DB_INVALID;
   }
   if (sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("research_db_claims_add: COMMIT failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   AUTH_DB_UNLOCK();
   return AUTH_DB_SUCCESS;
}

int research_db_claim_count(int64_t run_id, int *count_out) {
   if (run_id <= 0 || !count_out) {
      return AUTH_DB_INVALID;
   }
   *count_out = 0;
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM research_claims WHERE run_id=?", -1, &st,
                          NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, run_id);
   /* A COUNT always yields one ROW; a non-ROW step is a step-level failure, not a
    * real 0 — report FAILURE so callers can tell "no claims" from "read failed". */
   int step = sqlite3_step(st);
   if (step == SQLITE_ROW) {
      *count_out = sqlite3_column_int(st, 0);
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return (step == SQLITE_ROW) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int research_db_claim_list(int64_t run_id, research_claim_t *out, int max, int *count_out) {
   if (run_id <= 0 || !out || max <= 0 || !count_out) {
      return AUTH_DB_INVALID;
   }
   *count_out = 0;
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   /* Light projection: the report render uses only question_id/claim/source_url,
    * so skip fetching + copying the quote/source_kind text columns (§efficiency). */
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT " RESEARCH_CLAIM_LIGHT_COLS " FROM research_claims "
                               "WHERE run_id=? ORDER BY question_id ASC, id ASC LIMIT ?",
                               -1, &st, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, run_id);
   sqlite3_bind_int(st, 2, max);
   int n = 0;
   while (n < max && sqlite3_step(st) == SQLITE_ROW) {
      res_unpack_claim_light(st, &out[n]);
      n++;
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   *count_out = n;
   return AUTH_DB_SUCCESS;
}

int research_db_question_claims(int64_t run_id,
                                int64_t question_id,
                                research_claim_t *out,
                                int max,
                                int *count_out) {
   if (run_id <= 0 || !out || max <= 0 || !count_out) {
      return AUTH_DB_INVALID;
   }
   *count_out = 0;
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   /* Constrains (run_id, question_id) so it seeks idx_research_claims_run.  Light
    * projection: the digest gloss consumes only `claim`, so skip quote/source_kind. */
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT " RESEARCH_CLAIM_LIGHT_COLS " FROM research_claims "
                               "WHERE run_id=? AND question_id=? ORDER BY id ASC LIMIT ?",
                               -1, &st, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, run_id);
   sqlite3_bind_int64(st, 2, question_id);
   sqlite3_bind_int(st, 3, max);
   int n = 0;
   while (n < max && sqlite3_step(st) == SQLITE_ROW) {
      res_unpack_claim_light(st, &out[n]);
      n++;
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   *count_out = n;
   return AUTH_DB_SUCCESS;
}

/* =============================================================================
 * Report revisions (churn)
 * ============================================================================= */

int research_db_revision_add(int64_t run_id, int round, const char *markdown) {
   if (run_id <= 0 || !markdown) {
      return AUTH_DB_INVALID;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "INSERT INTO research_report_revisions "
                               "(run_id, round, markdown, created_at) VALUES (?, ?, ?, ?)",
                               -1, &st, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("research_db_revision_add: prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, run_id);
   sqlite3_bind_int(st, 2, round);
   sqlite3_bind_text(st, 3, markdown, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(st, 4, (int64_t)time(NULL));
   rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("research_db_revision_add: insert failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   AUTH_DB_UNLOCK();
   return AUTH_DB_SUCCESS;
}

int research_db_revision_get_latest(int64_t run_id, char **markdown_out) {
   if (run_id <= 0 || !markdown_out) {
      return AUTH_DB_INVALID;
   }
   *markdown_out = NULL;
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT markdown FROM research_report_revisions "
                               "WHERE run_id=? ORDER BY round DESC, id DESC LIMIT 1",
                               -1, &st, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, run_id);
   int rc2 = sqlite3_step(st);
   int result;
   if (rc2 == SQLITE_ROW) {
      const char *md = (const char *)sqlite3_column_text(st, 0);
      *markdown_out = md ? strdup(md) : strdup("");
      result = (*markdown_out != NULL) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
   } else if (rc2 == SQLITE_DONE) {
      result = AUTH_DB_NOT_FOUND; /* the run genuinely has no revision yet */
   } else {
      result = AUTH_DB_FAILURE; /* a step-time error is NOT "no rows" */
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return result;
}

int research_db_revisions_prune_to_latest(int64_t run_id) {
   if (run_id <= 0) {
      return AUTH_DB_INVALID;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   /* Keep only the highest (round, id) revision; delete the rest. */
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "DELETE FROM research_report_revisions WHERE run_id=? AND id NOT IN "
       "(SELECT id FROM research_report_revisions WHERE run_id=? ORDER BY round DESC, id DESC "
       "LIMIT 1)",
       -1, &st, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("research_db_revisions_prune: prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, run_id);
   sqlite3_bind_int64(st, 2, run_id);
   rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("research_db_revisions_prune: failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   AUTH_DB_UNLOCK();
   return AUTH_DB_SUCCESS;
}

int research_db_revision_count(int64_t run_id, int *count_out) {
   if (run_id <= 0 || !count_out) {
      return AUTH_DB_INVALID;
   }
   *count_out = 0;
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM research_report_revisions WHERE run_id=?",
                          -1, &st, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, run_id);
   /* A COUNT always yields one ROW; a non-ROW step is a step-level failure, not a
    * real 0 — report FAILURE so callers can tell "no revisions" from "read failed". */
   int step = sqlite3_step(st);
   if (step == SQLITE_ROW) {
      *count_out = sqlite3_column_int(st, 0);
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return (step == SQLITE_ROW) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}
