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
 * The consolidation record: batches and their operations, read and written
 * here, and removed with what a forget takes.
 */

#define _GNU_SOURCE /* asprintf */
#define AUTH_DB_INTERNAL_ALLOWED
#include "memory/memory_db_consolidation.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_db_internal.h"
#include "memory/memory_similarity.h"
#include "utils/string_utils.h"

/* Bind @p text cut to @p max bytes on a character boundary, or NULL. */
static void bind_capped(sqlite3_stmt *st, int col, const char *text, size_t max) {
   if (!text) {
      sqlite3_bind_null(st, col);
      return;
   }
   if (strlen(text) <= max) {
      sqlite3_bind_text(st, col, text, -1, SQLITE_TRANSIENT);
      return;
   }
   char *cut = malloc(max + 1);
   if (!cut) {
      sqlite3_bind_null(st, col);
      return;
   }
   memcpy(cut, text, max);
   cut[max] = '\0';
   utf8_trim_incomplete(cut); /* a character the cut split goes whole */
   sqlite3_bind_text(st, col, cut, -1, SQLITE_TRANSIENT);
   free(cut);
}

static void bind_id(sqlite3_stmt *st, int col, int64_t id) {
   if (id > 0) {
      sqlite3_bind_int64(st, col, id);
   } else {
      sqlite3_bind_null(st, col);
   }
}

int memory_db_consolidation_batch_add(const memory_consolidation_batch_t *b, int64_t *id_out) {
   if (id_out) {
      *id_out = 0;
   }
   if (!b || b->user_id <= 0 || !b->path || !b->stop) {
      return MEMORY_DB_FAILURE;
   }
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "INSERT INTO memory_consolidation_batches (user_id, "
                               "conversation_id, msg_id_start, msg_id_end, path, stop, outside, "
                               "rounds, input_tokens, created_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, "
                               "?7, ?8, ?9, ?10)",
                               -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(st, 1, b->user_id);
      bind_id(st, 2, b->conversation_id);
      bind_id(st, 3, b->msg_id_start);
      bind_id(st, 4, b->msg_id_end);
      sqlite3_bind_text(st, 5, b->path, -1, SQLITE_STATIC);
      sqlite3_bind_text(st, 6, b->stop, -1, SQLITE_STATIC);
      sqlite3_bind_int(st, 7, b->outside);
      sqlite3_bind_int(st, 8, b->rounds);
      sqlite3_bind_int(st, 9, b->input_tokens);
      sqlite3_bind_int64(st, 10, (int64_t)time(NULL));
      rc = sqlite3_step(st);
   }
   if (rc == SQLITE_DONE && id_out) {
      *id_out = sqlite3_last_insert_rowid(s_db.db);
   } else if (rc != SQLITE_DONE) {
      OLOG_WARNING("memory_db: recording a consolidation batch failed: %s",
                   sqlite3_errmsg(s_db.db));
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return rc == SQLITE_DONE ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

/* One UPDATE bound (?1 int64, ?2 user, ?3 int64 or text). */
static int update_one(const char *sql, int64_t a, int user_id, int64_t b, const char *text) {
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, a);
      sqlite3_bind_int(st, 2, user_id);
      if (text) {
         sqlite3_bind_text(st, 3, text, -1, SQLITE_STATIC);
      } else {
         sqlite3_bind_int64(st, 3, b);
      }
      rc = sqlite3_step(st);
   }
   const int changed = rc == SQLITE_DONE ? sqlite3_changes(s_db.db) : 0;
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   if (rc != SQLITE_DONE) {
      return MEMORY_DB_FAILURE;
   }
   return changed > 0 ? MEMORY_DB_SUCCESS : MEMORY_DB_NOT_FOUND;
}

int memory_db_consolidation_batch_set_summary(int64_t batch_id, int user_id, int64_t summary_id) {
   return update_one("UPDATE memory_consolidation_batches SET summary_id = ?3 WHERE id = ?1 AND "
                     "user_id = ?2",
                     batch_id, user_id, summary_id, NULL);
}

int memory_db_consolidation_op_set_status(int64_t op_id, int user_id, const char *status) {
   return status ? update_one("UPDATE memory_consolidation_ops SET status = ?3 WHERE id = ?1 AND "
                              "user_id = ?2",
                              op_id, user_id, 0, status)
                 : MEMORY_DB_FAILURE;
}

/* A deferred row still open, in a batch whose conversation can still be
 * learned from (?1 the op, ?2 the user). */
#define DEFERRED_OPEN_SQL                                                                 \
   "id = ?1 AND user_id = ?2 AND op = 'deferred' AND status = 'applied' AND batch_id IN " \
   "(SELECT b.id FROM memory_consolidation_batches b WHERE b.user_id = ?2 AND "           \
   "(b.conversation_id IS NULL OR EXISTS (SELECT 1 FROM conversations c WHERE c.id = "    \
   "b.conversation_id AND c.is_private = 0)))"

int memory_db_consolidation_deferred_claim(int64_t op_id, int user_id) {
   return update_one(
       "UPDATE memory_consolidation_ops SET status = 'consumed' WHERE " DEFERRED_OPEN_SQL, op_id,
       user_id, 0, NULL);
}

int memory_db_consolidation_deferred_open(int64_t op_id, int user_id) {
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT 1 FROM memory_consolidation_ops WHERE " DEFERRED_OPEN_SQL,
                               -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, op_id);
      sqlite3_bind_int(st, 2, user_id);
      rc = sqlite3_step(st);
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return rc == SQLITE_ROW    ? MEMORY_DB_SUCCESS
          : rc == SQLITE_DONE ? MEMORY_DB_NOT_FOUND
                              : MEMORY_DB_FAILURE;
}

/* Take up deferred candidate @p claim_op_id (DEFERRED_OPEN_SQL).  Lock held.
 * SQLITE_DONE with *taken, or the error. */
static int claim_locked(int64_t claim_op_id, int user_id, bool *taken) {
   *taken = false;
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db, "UPDATE memory_consolidation_ops SET status = 'consumed' WHERE " DEFERRED_OPEN_SQL,
       -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, claim_op_id);
      sqlite3_bind_int(st, 2, user_id);
      rc = sqlite3_step(st);
   }
   *taken = rc == SQLITE_DONE && sqlite3_changes(s_db.db) > 0;
   sqlite3_finalize(st);
   return rc;
}

/* Whether batch @p batch_id holds MEMORY_CONSOLIDATION_OPS_MAX ops (logged
 * once per batch).  Lock held. */
static bool batch_full_locked(int64_t batch_id) {
   static _Atomic int64_t s_logged_batch;
   sqlite3_stmt *st = NULL;
   bool full = false;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT COUNT(*) FROM memory_consolidation_ops WHERE batch_id = ?1", -1,
                          &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, batch_id);
      full = sqlite3_step(st) == SQLITE_ROW &&
             sqlite3_column_int(st, 0) >= MEMORY_CONSOLIDATION_OPS_MAX;
   }
   sqlite3_finalize(st);
   if (full && atomic_exchange(&s_logged_batch, batch_id) != batch_id) {
      OLOG_WARNING("memory_db: consolidation batch %lld holds %d ops; the rest are not recorded",
                   (long long)batch_id, MEMORY_CONSOLIDATION_OPS_MAX);
   }
   return full;
}

/* Insert @p op (claiming its deferred candidate first, in the same
 * savepoint).  A row quoting a conversation that went private is not
 * written: that conversation's record was removed, and must stay so.  Lock
 * held.  MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND (nothing written),
 * MEMORY_CONSOLIDATION_FULL (the batch already holds
 * MEMORY_CONSOLIDATION_OPS_MAX ops; nothing written) or MEMORY_DB_FAILURE. */
static int op_add_locked(const memory_consolidation_op_t *op, int64_t *id_out) {
   if (batch_full_locked(op->batch_id)) {
      return MEMORY_CONSOLIDATION_FULL;
   }
   if (auth_db_savepoint_begin_locked("consolidation_op") != AUTH_DB_SUCCESS) {
      return MEMORY_DB_FAILURE;
   }
   bool written = true;
   int rc = SQLITE_DONE;
   if (op->claim_op_id > 0) {
      rc = claim_locked(op->claim_op_id, op->user_id, &written);
   }
   sqlite3_stmt *st = NULL;
   if (rc == SQLITE_DONE && written) {
      rc = sqlite3_prepare_v2(
          s_db.db,
          "INSERT INTO memory_consolidation_ops (batch_id, user_id, seq, op, fact_id, "
          "other_fact_id, relation_id, entity_id, other_entity_id, link_id, outside, text, "
          "target_text, reason, pre_image, post_image, status, created_at, "
          "source_conversation_id, reason_by) SELECT ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, "
          "?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20 WHERE ?19 IS NULL OR EXISTS (SELECT 1 "
          "FROM conversations c WHERE c.id = ?19 AND c.is_private = 0)",
          -1, &st, NULL);
   }
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, op->batch_id);
      sqlite3_bind_int(st, 2, op->user_id);
      sqlite3_bind_int(st, 3, op->seq);
      sqlite3_bind_text(st, 4, op->op, -1, SQLITE_STATIC);
      bind_id(st, 5, op->fact_id);
      bind_id(st, 6, op->other_fact_id);
      bind_id(st, 7, op->relation_id);
      bind_id(st, 8, op->entity_id);
      bind_id(st, 9, op->other_entity_id);
      bind_id(st, 10, op->link_id);
      sqlite3_bind_int(st, 11, op->outside);
      bind_capped(st, 12, op->text, MEMORY_FACT_TEXT_MAX - 1);
      bind_capped(st, 13, op->target_text, MEMORY_FACT_TEXT_MAX - 1);
      bind_capped(st, 14, op->reason, MEMORY_CONSOLIDATE_REASON_MAX);
      if (op->pre_image) {
         sqlite3_bind_text(st, 15, op->pre_image, -1, SQLITE_STATIC);
      } else {
         sqlite3_bind_null(st, 15);
      }
      if (op->post_image) {
         sqlite3_bind_text(st, 16, op->post_image, -1, SQLITE_STATIC);
      } else {
         sqlite3_bind_null(st, 16);
      }
      sqlite3_bind_text(st, 17, op->status ? op->status : "applied", -1, SQLITE_STATIC);
      sqlite3_bind_int64(st, 18, (int64_t)time(NULL));
      bind_id(st, 19, op->source_conversation_id);
      if (op->reason_by) {
         sqlite3_bind_text(st, 20, op->reason_by, -1, SQLITE_STATIC);
      } else {
         sqlite3_bind_null(st, 20);
      }
      rc = sqlite3_step(st);
      written = rc == SQLITE_DONE && sqlite3_changes(s_db.db) > 0;
   }
   if (rc == SQLITE_DONE && written && id_out) {
      *id_out = sqlite3_last_insert_rowid(s_db.db);
   } else if (rc != SQLITE_DONE) {
      OLOG_WARNING("memory_db: recording a consolidation op failed: %s", sqlite3_errmsg(s_db.db));
   }
   sqlite3_finalize(st);
   /* Not written: the claim (if any) goes back too. */
   const bool keep = rc == SQLITE_DONE && written;
   if (auth_db_savepoint_end_locked(keep ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE, "consolidation_op") !=
           AUTH_DB_SUCCESS &&
       keep) {
      return MEMORY_DB_FAILURE;
   }
   if (rc != SQLITE_DONE) {
      return MEMORY_DB_FAILURE;
   }
   return written ? MEMORY_DB_SUCCESS : MEMORY_DB_NOT_FOUND;
}

int memory_db_consolidation_op_add(const memory_consolidation_op_t *op, int64_t *id_out) {
   if (id_out) {
      *id_out = 0;
   }
   if (!op || op->batch_id <= 0 || op->user_id <= 0 || !op->op) {
      return MEMORY_DB_FAILURE;
   }
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   const int rc = op_add_locked(op, id_out);
   AUTH_DB_UNLOCK();
   return rc;
}

static char *dup_column(sqlite3_stmt *st, int col) {
   const unsigned char *t = sqlite3_column_text(st, col);
   return t ? strdup((const char *)t) : NULL;
}

static void copy_column(sqlite3_stmt *st, int col, char *out, size_t size) {
   const unsigned char *t = sqlite3_column_text(st, col);
   safe_strncpy(out, t ? (const char *)t : "", size);
}

int memory_db_consolidation_deferred_list(int user_id,
                                          int64_t since,
                                          memory_consolidation_deferred_t *out,
                                          int max,
                                          int *count_out) {
   *count_out = 0;
   if (user_id <= 0 || !out || max <= 0) {
      return MEMORY_DB_FAILURE;
   }
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT o.id, o.batch_id, o.outside, o.pre_image, o.created_at, "
                               "COALESCE(b.conversation_id, 0), COALESCE(b.msg_id_start, 0), "
                               "COALESCE(b.msg_id_end, 0) FROM memory_consolidation_ops o JOIN "
                               "memory_consolidation_batches b ON b.id = o.batch_id WHERE "
                               "o.user_id = ?1 AND o.op = 'deferred' AND o.status = 'applied' AND "
                               "o.created_at >= ?2 AND o.pre_image IS NOT NULL ORDER BY "
                               "o.created_at DESC, o.id DESC LIMIT ?3",
                               -1, &st, NULL);
   int n = 0;
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_int64(st, 2, since);
      sqlite3_bind_int(st, 3, max);
      while ((rc = sqlite3_step(st)) == SQLITE_ROW && n < max) {
         memory_consolidation_deferred_t *d = &out[n];
         d->op_id = sqlite3_column_int64(st, 0);
         d->batch_id = sqlite3_column_int64(st, 1);
         d->outside = sqlite3_column_int(st, 2);
         d->candidate = dup_column(st, 3);
         d->created_at = sqlite3_column_int64(st, 4);
         d->conversation_id = sqlite3_column_int64(st, 5);
         d->msg_id_start = sqlite3_column_int64(st, 6);
         d->msg_id_end = sqlite3_column_int64(st, 7);
         if (d->candidate) {
            n++;
         }
      }
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   if (rc != SQLITE_DONE) {
      memory_db_consolidation_deferred_free(out, n);
      return MEMORY_DB_FAILURE;
   }
   *count_out = n;
   return MEMORY_DB_SUCCESS;
}

void memory_db_consolidation_deferred_free(memory_consolidation_deferred_t *rows, int n) {
   for (int i = 0; rows && i < n; i++) {
      free(rows[i].candidate);
      rows[i].candidate = NULL;
   }
}

/* A batch whose own conversation is private shows nowhere (its record goes
 * when it is made private; this covers a batch recorded as it was). */
#define BATCH_NOT_PRIVATE                                                                \
   "(conversation_id IS NULL OR NOT EXISTS (SELECT 1 FROM conversations c WHERE c.id = " \
   "conversation_id AND c.is_private = 1))"

/* The batch columns, as batch_from_row reads them. */
#define BATCH_COLS                                                                      \
   "id, COALESCE(conversation_id, 0), COALESCE(msg_id_start, 0), COALESCE(msg_id_end, " \
   "0), COALESCE(summary_id, 0), path, stop, outside, rounds, input_tokens, created_at"

static void batch_from_row(sqlite3_stmt *st, memory_consolidation_batch_row_t *b) {
   memset(b, 0, sizeof(*b));
   b->id = sqlite3_column_int64(st, 0);
   b->conversation_id = sqlite3_column_int64(st, 1);
   b->msg_id_start = sqlite3_column_int64(st, 2);
   b->msg_id_end = sqlite3_column_int64(st, 3);
   b->summary_id = sqlite3_column_int64(st, 4);
   copy_column(st, 5, b->path, sizeof(b->path));
   copy_column(st, 6, b->stop, sizeof(b->stop));
   b->outside = sqlite3_column_int(st, 7);
   b->rounds = sqlite3_column_int(st, 8);
   b->input_tokens = sqlite3_column_int(st, 9);
   b->created_at = sqlite3_column_int64(st, 10);
}

int memory_db_consolidation_batches_list(int user_id,
                                         int64_t conversation_id,
                                         int64_t before_batch_id,
                                         memory_consolidation_batch_row_t *out,
                                         int max,
                                         int *count_out) {
   *count_out = 0;
   if (user_id <= 0 || !out || max <= 0) {
      return MEMORY_DB_FAILURE;
   }
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   sqlite3_stmt *st = NULL;
   /* Keyset on the id, newest first: one conversation's through idx_mcb_conv,
    * all of the user's through idx_mcb_user (each with a sort by id). */
   int rc = sqlite3_prepare_v2(
       s_db.db,
       conversation_id > 0
           ? "SELECT " BATCH_COLS " FROM memory_consolidation_batches WHERE conversation_id = ?2 "
             "AND +user_id = ?1 AND (?3 = 0 OR id < ?3) AND " BATCH_NOT_PRIVATE " ORDER BY id "
             "DESC LIMIT ?4"
           : "SELECT " BATCH_COLS " FROM memory_consolidation_batches WHERE user_id = ?1 AND (?3 "
             "= 0 OR id < ?3) AND ?2 = 0 AND " BATCH_NOT_PRIVATE " ORDER BY id DESC LIMIT ?4",
       -1, &st, NULL);
   int n = 0;
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_int64(st, 2, conversation_id);
      sqlite3_bind_int64(st, 3, before_batch_id);
      sqlite3_bind_int(st, 4, max);
      while ((rc = sqlite3_step(st)) == SQLITE_ROW && n < max) {
         batch_from_row(st, &out[n++]);
      }
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   if (rc != SQLITE_DONE) {
      return MEMORY_DB_FAILURE;
   }
   *count_out = n;
   return MEMORY_DB_SUCCESS;
}

int memory_db_consolidation_ops_list(int user_id,
                                     int64_t batch_id,
                                     memory_consolidation_op_row_t *out,
                                     int max,
                                     int *count_out) {
   *count_out = 0;
   if (user_id <= 0 || !out || max <= 0) {
      return MEMORY_DB_FAILURE;
   }
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, batch_id, seq, op, COALESCE(fact_id, 0), COALESCE(other_fact_id, 0), "
       "COALESCE(relation_id, 0), COALESCE(entity_id, 0), COALESCE(other_entity_id, 0), "
       "COALESCE(link_id, 0), outside, text, target_text, reason, status, pre_image, post_image, "
       "COALESCE(source_conversation_id, 0), COALESCE(reason_by, ''), CASE WHEN o.other_fact_id "
       "IS NOT NULL THEN "
       "COALESCE((SELECT CASE WHEN f.superseded_by IS NULL THEN 'current' ELSE 'superseded' END "
       "FROM memory_facts f WHERE f.id = o.other_fact_id AND f.user_id = o.user_id), 'gone') WHEN "
       "o.other_entity_id IS NOT NULL THEN COALESCE((SELECT 'current' FROM memory_entities e "
       "WHERE e.id = o.other_entity_id AND e.user_id = o.user_id), 'gone') ELSE '' END FROM "
       "memory_consolidation_ops o WHERE user_id = ?1 AND batch_id = ?2 ORDER BY seq, id LIMIT ?3",
       -1, &st, NULL);
   int n = 0;
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_int64(st, 2, batch_id);
      sqlite3_bind_int(st, 3, max);
      while ((rc = sqlite3_step(st)) == SQLITE_ROW && n < max) {
         memory_consolidation_op_row_t *o = &out[n++];
         memset(o, 0, sizeof(*o));
         o->id = sqlite3_column_int64(st, 0);
         o->batch_id = sqlite3_column_int64(st, 1);
         o->seq = sqlite3_column_int(st, 2);
         copy_column(st, 3, o->op, sizeof(o->op));
         o->fact_id = sqlite3_column_int64(st, 4);
         o->other_fact_id = sqlite3_column_int64(st, 5);
         o->relation_id = sqlite3_column_int64(st, 6);
         o->entity_id = sqlite3_column_int64(st, 7);
         o->other_entity_id = sqlite3_column_int64(st, 8);
         o->link_id = sqlite3_column_int64(st, 9);
         o->outside = sqlite3_column_int(st, 10);
         copy_column(st, 11, o->text, sizeof(o->text));
         copy_column(st, 12, o->target_text, sizeof(o->target_text));
         copy_column(st, 13, o->reason, sizeof(o->reason));
         copy_column(st, 14, o->status, sizeof(o->status));
         o->pre_image = dup_column(st, 15);
         o->post_image = dup_column(st, 16);
         o->source_conversation_id = sqlite3_column_int64(st, 17);
         copy_column(st, 18, o->reason_by, sizeof(o->reason_by));
         copy_column(st, 19, o->target_status, sizeof(o->target_status));
      }
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   if (rc != SQLITE_DONE) {
      memory_db_consolidation_ops_free(out, n);
      return MEMORY_DB_FAILURE;
   }
   *count_out = n;
   return MEMORY_DB_SUCCESS;
}

void memory_db_consolidation_ops_free(memory_consolidation_op_row_t *rows, int n) {
   for (int i = 0; rows && i < n; i++) {
      free(rows[i].pre_image);
      free(rows[i].post_image);
      rows[i].pre_image = NULL;
      rows[i].post_image = NULL;
   }
}

/* The dropped and deferred candidate rows whose text folds to a going fact's
 * (memory_text_fold), removed.  Lock held. */
static int scrub_same_words_locked(int user_id, const char *going_sql) {
   char *sql = NULL;
   if (asprintf(&sql, "SELECT fact_text FROM memory_facts WHERE user_id = ?1 AND id IN (%s)",
                going_sql) < 0 ||
       !sql) {
      return MEMORY_DB_FAILURE;
   }
   char **going = NULL;
   int n_going = 0;
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL);
   free(sql);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
         char *f = memory_text_fold((const char *)sqlite3_column_text(st, 0));
         char **grown = f ? realloc(going, (size_t)(n_going + 1) * sizeof(*going)) : NULL;
         if (!grown) {
            free(f);
            rc = SQLITE_NOMEM;
            break;
         }
         going = grown;
         going[n_going++] = f;
      }
   }
   sqlite3_finalize(st);
   st = NULL;
   if (rc == SQLITE_DONE && n_going > 0) {
      rc = sqlite3_prepare_v2(s_db.db,
                              "SELECT id, text FROM memory_consolidation_ops WHERE user_id = ?1 "
                              "AND op IN ('dropped', 'deferred') AND text IS NOT NULL",
                              -1, &st, NULL);
      sqlite3_stmt *del = NULL;
      if (rc == SQLITE_OK) {
         rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM memory_consolidation_ops WHERE id = ?1", -1,
                                 &del, NULL);
      }
      if (rc == SQLITE_OK) {
         sqlite3_bind_int(st, 1, user_id);
         while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            char *f = memory_text_fold((const char *)sqlite3_column_text(st, 1));
            bool same = false;
            for (int i = 0; f && i < n_going && !same; i++) {
               same = strcmp(f, going[i]) == 0;
            }
            free(f);
            if (same) {
               sqlite3_reset(del);
               sqlite3_bind_int64(del, 1, sqlite3_column_int64(st, 0));
               if (sqlite3_step(del) != SQLITE_DONE) {
                  rc = SQLITE_ERROR;
                  break;
               }
            }
         }
      }
      sqlite3_finalize(del);
      sqlite3_finalize(st);
   }
   for (int i = 0; i < n_going; i++) {
      free(going[i]);
   }
   free(going);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: removing a forgotten fact's candidate rows failed: %s",
                 sqlite3_errmsg(s_db.db));
      return MEMORY_DB_FAILURE;
   }
   return MEMORY_DB_SUCCESS;
}

int memory_db_consolidation_scrub_facts_locked(int user_id, const char *going_sql) {
   if (user_id <= 0 || !going_sql) {
      return MEMORY_DB_FAILURE;
   }
   /* Any candidate row with its words (as the forgotten checks fold them),
    * first: the texts are read before the facts go. */
   if (scrub_same_words_locked(user_id, going_sql) != MEMORY_DB_SUCCESS) {
      return MEMORY_DB_FAILURE;
   }
   /* The rows naming a going fact or one of its relations, and the candidates
    * every batch of a conversation that taught one set aside (those may say it
    * too, in other words). */
   char *sql = NULL;
   const int len = asprintf(
       &sql,
       "DELETE FROM memory_consolidation_ops WHERE user_id = ?1 AND (fact_id IN (%s) OR "
       "other_fact_id IN (%s) OR relation_id IN (SELECT id FROM memory_relations WHERE "
       "user_id = ?1 AND fact_id IN (%s)) OR (op IN ('dropped', 'deferred') AND batch_id IN "
       "(SELECT b.id FROM memory_consolidation_batches b WHERE b.user_id = ?1 AND "
       "b.conversation_id IN (SELECT s.conversation_id FROM memory_fact_sources s WHERE "
       "s.fact_id IN (%s) UNION SELECT f.source_conversation_id FROM memory_facts f WHERE "
       "f.user_id = ?1 AND f.id IN (%s)))))",
       going_sql, going_sql, going_sql, going_sql, going_sql);
   if (len < 0 || !sql) {
      return MEMORY_DB_FAILURE;
   }
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      rc = sqlite3_step(st);
   }
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: removing a forgotten fact's consolidation record failed: %s",
                 sqlite3_errmsg(s_db.db));
   }
   sqlite3_finalize(st);
   free(sql);
   return rc == SQLITE_DONE ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_consolidation_drop_conversations_locked(int user_id, const char *ids_json) {
   if (user_id <= 0 || !ids_json) {
      return MEMORY_DB_FAILURE;
   }
   /* Its batches, and the rows other batches made quoting what it set aside. */
   static const char *const k_sql[] = {
      "DELETE FROM memory_consolidation_batches WHERE user_id = ?1 AND conversation_id IN "
      "(SELECT value FROM json_each(?2))",
      "DELETE FROM memory_consolidation_ops WHERE user_id = ?1 AND source_conversation_id IN "
      "(SELECT value FROM json_each(?2))",
   };
   sqlite3_stmt *st = NULL;
   int rc = SQLITE_DONE;
   for (size_t i = 0; i < sizeof(k_sql) / sizeof(k_sql[0]) && rc == SQLITE_DONE; i++) {
      rc = sqlite3_prepare_v2(s_db.db, k_sql[i], -1, &st, NULL);
      if (rc == SQLITE_OK) {
         sqlite3_bind_int(st, 1, user_id);
         sqlite3_bind_text(st, 2, ids_json, -1, SQLITE_STATIC);
         rc = sqlite3_step(st);
      }
      sqlite3_finalize(st);
      st = NULL;
   }
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: removing a conversation's consolidation record failed: %s",
                 sqlite3_errmsg(s_db.db));
   }
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_consolidation_scrub_entities_locked(int user_id) {
   /* Two statements: the relation half then uses idx_mco_rel; the entity half
    * scans the user's ops (bounded by the record's retention). */
   static const char *const k_sql[] = {
      "DELETE FROM memory_consolidation_ops WHERE user_id = ?1 AND op IN ('linked', 'proposed') "
      "AND (entity_id NOT IN (SELECT id FROM memory_entities WHERE user_id = ?1) OR "
      "other_entity_id NOT IN (SELECT id FROM memory_entities WHERE user_id = ?1))",
      "DELETE FROM memory_consolidation_ops WHERE user_id = ?1 AND relation_id IS NOT NULL AND "
      "relation_id NOT IN (SELECT id FROM memory_relations WHERE user_id = ?1)",
   };
   int rc = SQLITE_DONE;
   for (size_t i = 0; i < sizeof(k_sql) / sizeof(k_sql[0]) && rc == SQLITE_DONE; i++) {
      sqlite3_stmt *st = NULL;
      rc = sqlite3_prepare_v2(s_db.db, k_sql[i], -1, &st, NULL);
      if (rc == SQLITE_OK) {
         sqlite3_bind_int(st, 1, user_id);
         rc = sqlite3_step(st);
      }
      sqlite3_finalize(st);
   }
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: removing a forgotten entity's consolidation record failed: %s",
                 sqlite3_errmsg(s_db.db));
   }
   return rc == SQLITE_DONE ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

/* A candidate set aside lives in its batch's ops: the record outlives it. */
_Static_assert(MEMORY_CONSOLIDATE_RECORD_DAYS >= MEMORY_CONSOLIDATE_DEFERRED_DAYS,
               "the record must keep a batch as long as its deferred candidates");

int memory_db_consolidation_prune(int64_t older_than, int *count_out) {
   if (count_out) {
      *count_out = 0;
   }
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "DELETE FROM memory_consolidation_batches WHERE created_at < ?1", -1,
                               &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, older_than);
      rc = sqlite3_step(st);
   }
   if (rc == SQLITE_DONE && count_out) {
      *count_out = sqlite3_changes(s_db.db);
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return rc == SQLITE_DONE ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_consolidation_fact_state(int user_id,
                                       int64_t fact_id,
                                       int64_t conversation_id,
                                       memory_consolidation_fact_state_t *out) {
   memset(out, 0, sizeof(*out));
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT fact_text, confidence, source, outside, COALESCE(superseded_by, 0), "
       "COALESCE(expires_at, 0), COALESCE(subject_entity_id, 0), EXISTS (SELECT 1 FROM "
       "memory_fact_sources s WHERE s.fact_id = ?2 AND s.conversation_id = ?3), "
       "COALESCE(source_conversation_id, 0), COALESCE(source_msg_id_start, 0), "
       "COALESCE(source_msg_id_end, 0), COALESCE(superseded_at, 0), origin_unsourced FROM "
       "memory_facts WHERE id = "
       "?2 AND user_id = ?1",
       -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_int64(st, 2, fact_id);
      sqlite3_bind_int64(st, 3, conversation_id);
      rc = sqlite3_step(st);
   }
   const bool row = rc == SQLITE_ROW;
   if (row) {
      copy_column(st, 0, out->text, sizeof(out->text));
      out->confidence = (float)sqlite3_column_double(st, 1);
      copy_column(st, 2, out->source, sizeof(out->source));
      out->outside = sqlite3_column_int(st, 3);
      out->superseded_by = sqlite3_column_int64(st, 4);
      out->expires_at = sqlite3_column_int64(st, 5);
      out->subject_entity_id = sqlite3_column_int64(st, 6);
      out->had_conversation = sqlite3_column_int(st, 7) != 0;
      out->source_conversation_id = sqlite3_column_int64(st, 8);
      out->msg_id_start = sqlite3_column_int64(st, 9);
      out->msg_id_end = sqlite3_column_int64(st, 10);
      out->superseded_at = sqlite3_column_int64(st, 11);
      out->origin_unsourced = sqlite3_column_int(st, 12);
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return row ? MEMORY_DB_SUCCESS : rc == SQLITE_DONE ? MEMORY_DB_NOT_FOUND : MEMORY_DB_FAILURE;
}

int memory_db_consolidation_batch_get(int user_id,
                                      int64_t batch_id,
                                      memory_consolidation_batch_row_t *row) {
   memset(row, 0, sizeof(*row));
   if (user_id <= 0 || batch_id <= 0) {
      return MEMORY_DB_NOT_FOUND;
   }
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT " BATCH_COLS " FROM memory_consolidation_batches WHERE "
                               "id = ?1 AND user_id = ?2 AND " BATCH_NOT_PRIVATE,
                               -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, batch_id);
      sqlite3_bind_int(st, 2, user_id);
      rc = sqlite3_step(st);
   }
   if (rc == SQLITE_ROW) {
      batch_from_row(st, row);
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return rc == SQLITE_ROW    ? MEMORY_DB_SUCCESS
          : rc == SQLITE_DONE ? MEMORY_DB_NOT_FOUND
                              : MEMORY_DB_FAILURE;
}

int memory_db_consolidation_conversation_owned(int user_id, int64_t conversation_id) {
   if (user_id <= 0 || conversation_id <= 0) {
      return MEMORY_DB_NOT_FOUND;
   }
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT 1 FROM conversations WHERE id = ?1 AND user_id = ?2", -1,
                               &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, conversation_id);
      sqlite3_bind_int(st, 2, user_id);
      rc = sqlite3_step(st);
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return rc == SQLITE_ROW    ? MEMORY_DB_SUCCESS
          : rc == SQLITE_DONE ? MEMORY_DB_NOT_FOUND
                              : MEMORY_DB_FAILURE;
}

/* A candidate that can be kept: a drop, or a deferral still open or expired,
 * with its candidate recorded.  Row aliased o. */
#define KEEPABLE_SQL                                                                     \
   "((o.op = 'dropped' AND o.status = 'applied') OR (o.op = 'deferred' AND o.status IN " \
   "('applied', 'expired'))) AND o.pre_image IS NOT NULL"

int memory_db_consolidation_keep_get(int user_id,
                                     int64_t batch_id,
                                     int64_t op_id,
                                     memory_consolidation_keep_t *out) {
   memset(out, 0, sizeof(*out));
   if (user_id <= 0 || batch_id <= 0 || op_id <= 0) {
      return MEMORY_DB_NOT_FOUND;
   }
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT " KEEPABLE_SQL ", o.pre_image, o.outside, COALESCE(o.source_conversation_id, "
       "b.conversation_id, 0), o.source_conversation_id IS NULL, COALESCE(b.msg_id_start, 0), "
       "COALESCE(b.msg_id_end, 0) FROM memory_consolidation_ops o JOIN "
       "memory_consolidation_batches b ON b.id = o.batch_id WHERE o.id = ?1 AND o.batch_id = ?2 "
       "AND o.user_id = ?3 AND b.user_id = ?3",
       -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, op_id);
      sqlite3_bind_int64(st, 2, batch_id);
      sqlite3_bind_int(st, 3, user_id);
      rc = sqlite3_step(st);
   }
   int result = rc == SQLITE_DONE ? MEMORY_DB_NOT_FOUND : MEMORY_DB_FAILURE;
   if (rc == SQLITE_ROW && !sqlite3_column_int(st, 0)) {
      result = MEMORY_CONSOLIDATION_NOTHING;
   } else if (rc == SQLITE_ROW) {
      out->candidate = dup_column(st, 1);
      out->outside = sqlite3_column_int(st, 2);
      out->conversation_id = sqlite3_column_int64(st, 3);
      /* The batch's rows, when it came from the batch's own conversation. */
      if (sqlite3_column_int(st, 4)) {
         out->msg_id_start = sqlite3_column_int64(st, 5);
         out->msg_id_end = sqlite3_column_int64(st, 6);
      }
      result = out->candidate ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return result;
}

void memory_db_consolidation_keep_free(memory_consolidation_keep_t *k) {
   if (k) {
      free(k->candidate);
      k->candidate = NULL;
   }
}

int memory_db_consolidation_keep_record(int user_id,
                                        int64_t batch_id,
                                        int64_t op_id,
                                        memory_consolidation_op_t *added,
                                        int64_t *added_id_out) {
   if (added_id_out) {
      *added_id_out = 0;
   }
   if (user_id <= 0 || batch_id <= 0 || op_id <= 0 || (added && !added->op)) {
      return MEMORY_DB_FAILURE;
   }
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   if (auth_db_txn_begin_locked("memory_db: keep") != AUTH_DB_SUCCESS) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "UPDATE memory_consolidation_ops AS o SET status = 'kept' WHERE "
                               "o.id = ?1 AND o.batch_id = ?2 AND o.user_id = ?3 AND " KEEPABLE_SQL,
                               -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, op_id);
      sqlite3_bind_int64(st, 2, batch_id);
      sqlite3_bind_int(st, 3, user_id);
      rc = sqlite3_step(st);
   }
   const bool marked = rc == SQLITE_DONE && sqlite3_changes(s_db.db) > 0;
   sqlite3_finalize(st);
   st = NULL;
   int result = rc == SQLITE_DONE ? MEMORY_DB_NOT_FOUND : MEMORY_DB_FAILURE;
   if (marked && !added) {
      result = MEMORY_DB_SUCCESS; /* kept, nothing appended */
   } else if (marked) {
      /* Appended to the batch, after its last op. */
      rc = sqlite3_prepare_v2(s_db.db,
                              "SELECT COALESCE(MAX(seq), 0) + 1 FROM memory_consolidation_ops "
                              "WHERE batch_id = ?1",
                              -1, &st, NULL);
      if (rc == SQLITE_OK) {
         sqlite3_bind_int64(st, 1, batch_id);
         rc = sqlite3_step(st);
      }
      added->seq = rc == SQLITE_ROW ? sqlite3_column_int(st, 0) : 0;
      sqlite3_finalize(st);
      added->batch_id = batch_id;
      added->user_id = user_id;
      added->claim_op_id = 0;
      result = rc == SQLITE_ROW ? op_add_locked(added, added_id_out) : MEMORY_DB_FAILURE;
   }
   if (auth_db_txn_end_locked(result == MEMORY_DB_SUCCESS ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE,
                              "memory_db: keep") != AUTH_DB_SUCCESS &&
       result == MEMORY_DB_SUCCESS) {
      result = MEMORY_DB_FAILURE;
   }
   AUTH_DB_UNLOCK();
   return result;
}

int memory_db_consolidation_expire_deferred(int64_t older_than, int *count_out) {
   if (count_out) {
      *count_out = 0;
   }
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "UPDATE memory_consolidation_ops SET status = 'expired' WHERE op = "
                               "'deferred' AND status = 'applied' AND created_at < ?1",
                               -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, older_than);
      rc = sqlite3_step(st);
   }
   if (rc == SQLITE_DONE && count_out) {
      *count_out = sqlite3_changes(s_db.db);
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return rc == SQLITE_DONE ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

char *memory_db_consolidation_merge_image(int user_id, int64_t from, int64_t into) {
   AUTH_DB_LOCK_OR_RETURN(NULL);
   sqlite3_stmt *st = NULL;
   char *out = NULL;
   /* As MERGE_SOURCES_SQL copies them (memory_db_facts.c): the duplicate's
    * source rows and its own conversation, less what the keeper has. */
   if (sqlite3_prepare_v2(
           s_db.db,
           "SELECT json_object('keeper_origin_unsourced', k.origin_unsourced, 'added_sources', "
           "(SELECT json_group_array(c) FROM (SELECT conversation_id c "
           "FROM memory_fact_sources WHERE fact_id = ?2 UNION SELECT source_conversation_id FROM "
           "memory_facts WHERE id = ?2 AND user_id = ?1 AND source_conversation_id IN (SELECT id "
           "FROM conversations)) WHERE c NOT IN (SELECT conversation_id FROM memory_fact_sources "
           "WHERE fact_id = ?3))) FROM memory_facts k WHERE k.id = ?3 AND k.user_id = ?1",
           -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_int64(st, 2, from);
      sqlite3_bind_int64(st, 3, into);
      if (sqlite3_step(st) == SQLITE_ROW) {
         out = dup_column(st, 0);
      }
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return out;
}

char *memory_db_consolidation_pref_image(int user_id, const char *category) {
   AUTH_DB_LOCK_OR_RETURN(NULL);
   sqlite3_stmt *st = NULL;
   char *out = NULL;
   if (sqlite3_prepare_v2(
           s_db.db,
           "SELECT json_object('source_conversation_id', COALESCE(source_conversation_id, 0), "
           "'msg_id_start', COALESCE(source_msg_id_start, 0), 'msg_id_end', "
           "COALESCE(source_msg_id_end, 0), 'origin_unsourced', origin_unsourced, 'sources', "
           "(SELECT json_group_array(conversation_id) FROM memory_preference_sources WHERE "
           "preference_id = p.id)) FROM memory_preferences p WHERE user_id = ?1 AND category = ?2",
           -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_text(st, 2, category, -1, SQLITE_TRANSIENT);
      if (sqlite3_step(st) == SQLITE_ROW) {
         out = dup_column(st, 0);
      }
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return out;
}

int memory_db_consolidation_batches_naming(int user_id,
                                           int64_t fact_id,
                                           int64_t *ids,
                                           int max,
                                           int *count_out) {
   *count_out = 0;
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               /* The fact and the duplicates merged into it (a forget
                                * takes them too), at any depth. */
                               "WITH RECURSIVE " MEMORY_MERGED_UNDER_CTE(
                                   "gone", "?2",
                                   "?1") " SELECT DISTINCT batch_id FROM "
                                         "memory_consolidation_ops WHERE user_id = ?1 "
                                         "AND (fact_id IN (SELECT id FROM gone) OR "
                                         "other_fact_id IN (SELECT id FROM gone)) "
                                         "ORDER BY batch_id DESC LIMIT ?3",
                               -1, &st, NULL);
   int n = 0;
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_int64(st, 2, fact_id);
      sqlite3_bind_int(st, 3, max);
      while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
         ids[n++] = sqlite3_column_int64(st, 0);
      }
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   *count_out = n;
   return rc == SQLITE_DONE ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_consolidation_batch_set_stop(int64_t batch_id,
                                           int user_id,
                                           const char *path,
                                           const char *stop) {
   if (!path || !stop) {
      return MEMORY_DB_FAILURE;
   }
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "UPDATE memory_consolidation_batches SET path = ?3, stop = ?4 WHERE "
                               "id = ?1 AND user_id = ?2",
                               -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, batch_id);
      sqlite3_bind_int(st, 2, user_id);
      sqlite3_bind_text(st, 3, path, -1, SQLITE_STATIC);
      sqlite3_bind_text(st, 4, stop, -1, SQLITE_STATIC);
      rc = sqlite3_step(st);
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return rc == SQLITE_DONE ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_consolidation_batch_room(int user_id, int64_t batch_id, int *room) {
   *room = 0;
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT (SELECT COUNT(*) FROM memory_consolidation_ops o WHERE "
       "o.batch_id = b.id) FROM memory_consolidation_batches b WHERE b.id = "
       "?1 AND b.user_id = ?2",
       -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, batch_id);
      sqlite3_bind_int(st, 2, user_id);
      rc = sqlite3_step(st);
   }
   if (rc == SQLITE_ROW) {
      const int used = sqlite3_column_int(st, 0);
      *room = used < MEMORY_CONSOLIDATION_OPS_MAX ? MEMORY_CONSOLIDATION_OPS_MAX - used : 0;
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return rc == SQLITE_ROW    ? MEMORY_DB_SUCCESS
          : rc == SQLITE_DONE ? MEMORY_DB_NOT_FOUND
                              : MEMORY_DB_FAILURE;
}
