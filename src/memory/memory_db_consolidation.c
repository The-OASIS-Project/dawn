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
   utf8_truncate(cut, max);
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

int memory_db_consolidation_op_add(const memory_consolidation_op_t *op, int64_t *id_out) {
   if (id_out) {
      *id_out = 0;
   }
   if (!op || op->batch_id <= 0 || op->user_id <= 0 || !op->op) {
      return MEMORY_DB_FAILURE;
   }
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO memory_consolidation_ops (batch_id, user_id, seq, op, fact_id, "
       "other_fact_id, relation_id, entity_id, other_entity_id, link_id, outside, text, "
       "target_text, reason, pre_image, post_image, status, created_at) VALUES (?1, ?2, ?3, ?4, "
       "?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18)",
       -1, &st, NULL);
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
      rc = sqlite3_step(st);
   }
   if (rc == SQLITE_DONE && id_out) {
      *id_out = sqlite3_last_insert_rowid(s_db.db);
   } else if (rc != SQLITE_DONE) {
      OLOG_WARNING("memory_db: recording a consolidation op failed: %s", sqlite3_errmsg(s_db.db));
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return rc == SQLITE_DONE ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
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
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, COALESCE(conversation_id, 0), COALESCE(msg_id_start, 0), "
       "COALESCE(msg_id_end, 0), COALESCE(summary_id, 0), path, stop, outside, rounds, "
       "input_tokens, created_at FROM memory_consolidation_batches WHERE user_id = ?1 AND (?2 = 0 "
       "OR conversation_id = ?2) AND (?3 = 0 OR id < ?3) ORDER BY id DESC LIMIT ?4",
       -1, &st, NULL);
   int n = 0;
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_int64(st, 2, conversation_id);
      sqlite3_bind_int64(st, 3, before_batch_id);
      sqlite3_bind_int(st, 4, max);
      while ((rc = sqlite3_step(st)) == SQLITE_ROW && n < max) {
         memory_consolidation_batch_row_t *b = &out[n++];
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
       "COALESCE(link_id, 0), outside, text, target_text, reason, status, pre_image, post_image "
       "FROM memory_consolidation_ops WHERE user_id = ?1 AND batch_id = ?2 ORDER BY seq, id "
       "LIMIT ?3",
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
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "DELETE FROM memory_consolidation_batches WHERE user_id = ?1 AND "
                               "conversation_id IN (SELECT value FROM json_each(?2))",
                               -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_text(st, 2, ids_json, -1, SQLITE_STATIC);
      rc = sqlite3_step(st);
   }
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: removing a conversation's consolidation record failed: %s",
                 sqlite3_errmsg(s_db.db));
   }
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_consolidation_scrub_entities_locked(int user_id) {
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "DELETE FROM memory_consolidation_ops WHERE user_id = ?1 AND op IN "
                               "('linked', 'proposed') AND (entity_id NOT IN (SELECT id FROM "
                               "memory_entities WHERE user_id = ?1) OR other_entity_id NOT IN "
                               "(SELECT id FROM memory_entities WHERE user_id = ?1))",
                               -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      rc = sqlite3_step(st);
   }
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: removing a forgotten entity's consolidation record failed: %s",
                 sqlite3_errmsg(s_db.db));
   }
   sqlite3_finalize(st);
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

int memory_db_consolidation_fact_source(int user_id,
                                        int64_t fact_id,
                                        int64_t conversation_id,
                                        memory_consolidation_fact_source_t *out) {
   memset(out, 0, sizeof(*out));
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT COALESCE(source_msg_id_start, 0), COALESCE(source_msg_id_end, "
       "0), EXISTS (SELECT 1 FROM memory_fact_sources s WHERE s.fact_id = "
       "?2 AND s.conversation_id = ?3), COALESCE(source_conversation_id, 0) FROM memory_facts "
       "WHERE id = ?2 AND user_id = ?1",
       -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_int64(st, 2, fact_id);
      sqlite3_bind_int64(st, 3, conversation_id);
      rc = sqlite3_step(st);
   }
   const bool row = rc == SQLITE_ROW;
   if (row) {
      out->msg_id_start = sqlite3_column_int64(st, 0);
      out->msg_id_end = sqlite3_column_int64(st, 1);
      out->had_conversation = sqlite3_column_int(st, 2) != 0;
      out->source_conversation_id = sqlite3_column_int64(st, 3);
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return row ? MEMORY_DB_SUCCESS : MEMORY_DB_NOT_FOUND;
}
