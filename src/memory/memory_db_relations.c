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
 * Memory Database — Relation CRUD operations + contradiction handling.
 *
 * Phase 6 split from memory_db.c — relation create / list / supersede,
 * plus the EXCLUSIVE_RELATIONS / CONTRADICTORY_PAIRS tables and the
 * memory_db_relation_supersede atomic close-and-create transaction.
 *
 * Contradiction handling — lives in three coordinated locations:
 *
 *   (1) HERE: EXCLUSIVE_RELATIONS[] + CONTRADICTORY_PAIRS[] tables
 *       (compile-time data) and memory_db_relation_supersede() (atomic
 *       close + create transaction; out_old_fact_id surfaces the linked
 *       fact for the caller to propagate).
 *
 *   (2) src/memory/memory_extraction.c — process_extracted_relation()
 *       (around the fact_map build / find_fact_for_relation() call site):
 *       receives the old fact_id from (1) and calls
 *       memory_db_fact_supersede() to mark the linked fact as stale, so
 *       the structured-relation supersede also retires the natural-
 *       language fact rendered from it.
 *
 * Adding a new exclusive relation or contradictory pair is a (1)-only edit.
 * Changing the supersede-propagation mechanism is a (1)+(2) edit; if a
 * fourth piece lands (e.g. inline LLM-based contradiction judgment for
 * subtle quantity / negation cases — see dawn/docs/TODO.md "LLM-based
 * contradiction judgment"), this whole cluster should move into a
 * dedicated memory_contradiction.{c,h} module.
 *
 * Exclusive relations have at-most-one open instance per (subject, relation).
 * When extraction stores a new such relation with a different object,
 * memory_db_relation_supersede() auto-closes the prior open row.
 * Non-exclusive relations (likes, knows, has_pet) skip the close branch.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_db_internal.h"
#include "utils/string_utils.h"

/* `const char *const` (not just `const char *`) so the pointer array itself
 * lands in .rodata.  Without the inner const, only the C-strings are
 * read-only — the pointer table is in .data and process-image-writable. */
static const char *const EXCLUSIVE_RELATIONS[] = {
   "works_at",      "lives_in",         "married_to", "attends_school",
   "owns_vehicle",  "born_in",          "born_on",    "favorite_color",
   "favorite_food", "primary_language", "email_is",   "phone_number_is",
};
static const int EXCLUSIVE_RELATIONS_COUNT = (int)(sizeof(EXCLUSIVE_RELATIONS) /
                                                   sizeof(EXCLUSIVE_RELATIONS[0]));

static const struct {
   const char *const a;
   const char *const b;
} CONTRADICTORY_PAIRS[] = {
   { "likes", "dislikes" },
   { "enjoys", "hates" },
   { "can", "cannot" },
   { "is", "is_not" },
};
static const int CONTRADICTORY_PAIRS_COUNT = (int)(sizeof(CONTRADICTORY_PAIRS) /
                                                   sizeof(CONTRADICTORY_PAIRS[0]));

static const char *contradictory_opposite(const char *relation) {
   if (!relation)
      return NULL;
   for (int i = 0; i < CONTRADICTORY_PAIRS_COUNT; i++) {
      if (strcmp(relation, CONTRADICTORY_PAIRS[i].a) == 0)
         return CONTRADICTORY_PAIRS[i].b;
      if (strcmp(relation, CONTRADICTORY_PAIRS[i].b) == 0)
         return CONTRADICTORY_PAIRS[i].a;
   }
   return NULL;
}

/* Public-named helper: alias surface (memory_db_alias.c, v43) consults this to
 * identify open exclusive relations during Stage 5 of the resolver cascade.
 * EXCLUSIVE_RELATIONS[] stays static here as the source-of-truth list. */
bool memory_db_relation_is_exclusive(const char *relation) {
   if (!relation)
      return false;
   for (int i = 0; i < EXCLUSIVE_RELATIONS_COUNT; i++) {
      if (strcmp(relation, EXCLUSIVE_RELATIONS[i]) == 0)
         return true;
   }
   return false;
}

/* Internal-name shim — keeps the existing call sites in this file unchanged
 * while the public name becomes the canonical entry point. */
static inline bool relation_is_exclusive(const char *relation) {
   return memory_db_relation_is_exclusive(relation);
}

int memory_db_relation_create(int user_id,
                              int64_t subject_entity_id,
                              const char *relation,
                              int64_t object_entity_id,
                              const char *object_value,
                              int64_t fact_id,
                              float confidence,
                              int64_t valid_from,
                              int64_t valid_to,
                              const memory_provenance_t *prov) {
   if (!relation)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_relation_create);
   sqlite3_bind_int(s_db.stmt_memory_relation_create, 1, user_id);
   sqlite3_bind_int64(s_db.stmt_memory_relation_create, 2, subject_entity_id);
   sqlite3_bind_text(s_db.stmt_memory_relation_create, 3, relation, -1, SQLITE_TRANSIENT);

   if (object_entity_id > 0) {
      sqlite3_bind_int64(s_db.stmt_memory_relation_create, 4, object_entity_id);
   } else {
      sqlite3_bind_null(s_db.stmt_memory_relation_create, 4);
   }

   if (object_value) {
      sqlite3_bind_text(s_db.stmt_memory_relation_create, 5, object_value, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(s_db.stmt_memory_relation_create, 5);
   }

   if (fact_id > 0) {
      sqlite3_bind_int64(s_db.stmt_memory_relation_create, 6, fact_id);
   } else {
      sqlite3_bind_null(s_db.stmt_memory_relation_create, 6);
   }

   sqlite3_bind_double(s_db.stmt_memory_relation_create, 7, (double)confidence);

   if (valid_from > 0) {
      sqlite3_bind_int64(s_db.stmt_memory_relation_create, 8, valid_from);
   } else {
      sqlite3_bind_null(s_db.stmt_memory_relation_create, 8);
   }
   if (valid_to > 0) {
      sqlite3_bind_int64(s_db.stmt_memory_relation_create, 9, valid_to);
   } else {
      sqlite3_bind_null(s_db.stmt_memory_relation_create, 9);
   }
   memory_db_internal_bind_provenance(s_db.stmt_memory_relation_create, 10, prov);

   /* v49 upsert: RETURNING id, mention_count.  First step yields SQLITE_ROW
    * (whether INSERT or ON CONFLICT path); drain to SQLITE_DONE.  No need to
    * consume the returned values here — observability of mention_count belongs
    * at the supersede call site where extraction can log dedup-hits. */
   int rc = sqlite3_step(s_db.stmt_memory_relation_create);
   bool ok = (rc == SQLITE_ROW || rc == SQLITE_DONE);
   while (rc == SQLITE_ROW) {
      rc = sqlite3_step(s_db.stmt_memory_relation_create);
   }
   ok = ok && (rc == SQLITE_DONE);
   sqlite3_reset(s_db.stmt_memory_relation_create);
   AUTH_DB_UNLOCK();

   return ok ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

/* Transactional close-and-create.  For exclusive relations (works_at, lives_in, ...)
 * any existing open row with a different object is closed (valid_to = now()) before
 * the new row is inserted.  Both writes happen under one BEGIN IMMEDIATE so other
 * workers can never observe a state with zero open rows for the same (subject, relation).
 *
 * Non-exclusive relations skip the close branch — multiple open rows are valid
 * (a user can like many things, know many people). */
int memory_db_relation_supersede(int user_id,
                                 int64_t subject_entity_id,
                                 const char *relation,
                                 int64_t object_entity_id,
                                 const char *object_value,
                                 int64_t fact_id,
                                 float confidence,
                                 int64_t valid_from,
                                 int64_t valid_to,
                                 const memory_provenance_t *prov,
                                 int64_t *out_old_fact_id) {
   if (out_old_fact_id)
      *out_old_fact_id = 0;
   if (!relation)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   /* Begin transaction.  Any failure rolls back and leaves the graph unchanged. */
   char *errmsg = NULL;
   int rc = sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: relation_supersede begin failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   /* Auto-close prior open exclusive relation, if any.  Idempotency check (object
    * mismatch) lives in the prepared statement's WHERE clause.
    *
    * Two orthogonal questions govern whether we close and, if so, at what time:
    *
    *   1. Does the new relation represent a currently-true state?
    *        Yes: valid_to == 0 (open-ended) OR valid_to > now().
    *        No:  valid_to is in the past (bounded historical slice).
    *      Only currently-true relations should supersede the existing open row.
    *      Ingesting a bounded historical fact ("Alice worked at Google 2018-2020")
    *      must NOT close the present-day Microsoft row — that would erase reality.
    *
    *   2. If we do close, at what boundary?
    *        - new valid_from when provided (avoids overlapping validity windows)
    *        - now() otherwise
    */
   int64_t now_ts = (int64_t)time(NULL);
   bool new_is_current = (valid_to <= 0) || (valid_to > now_ts);
   if (relation_is_exclusive(relation) && new_is_current) {
      int64_t close_time = (valid_from > 0) ? valid_from : now_ts;
      sqlite3_stmt *close_stmt = s_db.stmt_memory_relation_close_open;
      sqlite3_reset(close_stmt);
      sqlite3_bind_int64(close_stmt, 1, close_time);
      sqlite3_bind_int(close_stmt, 2, user_id);
      sqlite3_bind_int64(close_stmt, 3, subject_entity_id);
      sqlite3_bind_text(close_stmt, 4, relation, -1, SQLITE_TRANSIENT);
      if (object_entity_id > 0) {
         sqlite3_bind_int64(close_stmt, 5, object_entity_id);
      } else {
         sqlite3_bind_null(close_stmt, 5);
      }
      if (object_value) {
         sqlite3_bind_text(close_stmt, 6, object_value, -1, SQLITE_TRANSIENT);
      } else {
         sqlite3_bind_null(close_stmt, 6);
      }
      /* RETURNING fact_id yields SQLITE_ROW for each closed row */
      int64_t old_fact_id_local = 0;
      rc = sqlite3_step(close_stmt);
      if (rc == SQLITE_ROW) {
         old_fact_id_local = sqlite3_column_int64(close_stmt, 0);
         while (sqlite3_step(close_stmt) == SQLITE_ROW) {
         }
         rc = SQLITE_DONE;
      }
      sqlite3_reset(close_stmt);
      if (rc != SQLITE_DONE) {
         OLOG_ERROR("memory_db: relation_supersede close failed: %s", sqlite3_errmsg(s_db.db));
         sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
         AUTH_DB_UNLOCK();
         return MEMORY_DB_FAILURE;
      }
      int closed = sqlite3_changes(s_db.db);
      if (closed > 0) {
         OLOG_INFO("memory_db: closed %d superseded '%s' relation(s) for subject %ld", closed,
                   relation, (long)subject_entity_id);
         if (out_old_fact_id && old_fact_id_local > 0)
            *out_old_fact_id = old_fact_id_local;
      }
   }

   /* Close contradictory relation (e.g., likes↔dislikes) on the same
    * (subject, object) pair.  Only applies to currently-true new relations. */
   const char *opposite = contradictory_opposite(relation);
   if (opposite && new_is_current) {
      sqlite3_stmt *cstmt = NULL;
      int rc2 = sqlite3_prepare_v2(s_db.db,
                                   "UPDATE memory_relations SET valid_to = ? "
                                   "WHERE user_id = ? AND subject_entity_id = ? AND relation = ? "
                                   "  AND valid_to IS NULL "
                                   "  AND COALESCE(object_entity_id, 0) = COALESCE(?, 0) "
                                   "  AND COALESCE(object_value, '') = COALESCE(?, '') "
                                   "RETURNING fact_id",
                                   -1, &cstmt, NULL);
      if (rc2 != SQLITE_OK) {
         OLOG_ERROR("memory_db: prepare contradictory close failed: %s", sqlite3_errmsg(s_db.db));
         sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
         AUTH_DB_UNLOCK();
         return MEMORY_DB_FAILURE;
      } else {
         int64_t close_time = (valid_from > 0) ? valid_from : now_ts;
         sqlite3_bind_int64(cstmt, 1, close_time);
         sqlite3_bind_int(cstmt, 2, user_id);
         sqlite3_bind_int64(cstmt, 3, subject_entity_id);
         sqlite3_bind_text(cstmt, 4, opposite, -1, SQLITE_TRANSIENT);
         if (object_entity_id > 0)
            sqlite3_bind_int64(cstmt, 5, object_entity_id);
         else
            sqlite3_bind_null(cstmt, 5);
         if (object_value)
            sqlite3_bind_text(cstmt, 6, object_value, -1, SQLITE_TRANSIENT);
         else
            sqlite3_bind_null(cstmt, 6);

         rc2 = sqlite3_step(cstmt);
         if (rc2 == SQLITE_ROW) {
            int64_t contra_fact_id = sqlite3_column_int64(cstmt, 0);
            do {
               rc2 = sqlite3_step(cstmt);
            } while (rc2 == SQLITE_ROW);
            if (rc2 != SQLITE_DONE) {
               OLOG_ERROR("memory_db: contradictory close step failed: %s",
                          sqlite3_errmsg(s_db.db));
               sqlite3_finalize(cstmt);
               sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
               AUTH_DB_UNLOCK();
               return MEMORY_DB_FAILURE;
            }
            if (out_old_fact_id && *out_old_fact_id == 0 && contra_fact_id > 0)
               *out_old_fact_id = contra_fact_id;
            OLOG_INFO("memory_db: closed contradictory '%s' (opposite of '%s') "
                      "for subject %ld",
                      opposite, relation, (long)subject_entity_id);
         } else if (rc2 != SQLITE_DONE) {
            OLOG_ERROR("memory_db: contradictory close step failed: %s", sqlite3_errmsg(s_db.db));
            sqlite3_finalize(cstmt);
            sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
            AUTH_DB_UNLOCK();
            return MEMORY_DB_FAILURE;
         }
         sqlite3_finalize(cstmt);
      }
   }

   /* Insert the new row using the existing prepared statement.  Inlined here
    * because we hold the auth_db lock; can't call memory_db_relation_create which
    * would re-acquire it. */
   sqlite3_stmt *create_stmt = s_db.stmt_memory_relation_create;
   sqlite3_reset(create_stmt);
   sqlite3_bind_int(create_stmt, 1, user_id);
   sqlite3_bind_int64(create_stmt, 2, subject_entity_id);
   sqlite3_bind_text(create_stmt, 3, relation, -1, SQLITE_TRANSIENT);
   if (object_entity_id > 0) {
      sqlite3_bind_int64(create_stmt, 4, object_entity_id);
   } else {
      sqlite3_bind_null(create_stmt, 4);
   }
   if (object_value) {
      sqlite3_bind_text(create_stmt, 5, object_value, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(create_stmt, 5);
   }
   if (fact_id > 0) {
      sqlite3_bind_int64(create_stmt, 6, fact_id);
   } else {
      sqlite3_bind_null(create_stmt, 6);
   }
   sqlite3_bind_double(create_stmt, 7, (double)confidence);
   if (valid_from > 0) {
      sqlite3_bind_int64(create_stmt, 8, valid_from);
   } else {
      sqlite3_bind_null(create_stmt, 8);
   }
   if (valid_to > 0) {
      sqlite3_bind_int64(create_stmt, 9, valid_to);
   } else {
      sqlite3_bind_null(create_stmt, 9);
   }
   memory_db_internal_bind_provenance(create_stmt, 10, prov);
   /* v49 upsert: RETURNING id, mention_count.  Consume the first row to learn
    * the dedup outcome (mention_count > 1 means an existing row was bumped
    * rather than a fresh row inserted), then drain to SQLITE_DONE.  Done
    * before sqlite3_reset() so the column read remains valid. */
   rc = sqlite3_step(create_stmt);
   int upsert_mention_count = 0;
   int64_t upsert_id = 0;
   if (rc == SQLITE_ROW) {
      upsert_id = sqlite3_column_int64(create_stmt, 0);
      upsert_mention_count = sqlite3_column_int(create_stmt, 1);
      do {
         rc = sqlite3_step(create_stmt);
      } while (rc == SQLITE_ROW);
   }
   (void)upsert_id; /* read only by the OLOG_DEBUG below (a no-op unless built -DDEBUG) */
   sqlite3_reset(create_stmt);
   if (rc != SQLITE_DONE) {
      int xrc = sqlite3_extended_errcode(s_db.db);
      OLOG_ERROR("memory_db: relation_supersede insert failed: %s "
                 "(rc=%d xrc=%d user=%d subj=%lld rel='%s' obj_id=%lld obj_val='%s' "
                 "fact_id=%lld conv=%lld msg_range=[%lld..%lld] valid=[%lld..%lld])",
                 sqlite3_errmsg(s_db.db), rc, xrc, user_id, (long long)subject_entity_id, relation,
                 (long long)object_entity_id, object_value ? object_value : "(null)",
                 (long long)fact_id, prov ? (long long)prov->conv_id : 0LL,
                 prov ? (long long)prov->msg_id_start : 0LL,
                 prov ? (long long)prov->msg_id_end : 0LL, (long long)valid_from,
                 (long long)valid_to);

      /* xrc=787 = SQLITE_CONSTRAINT_FOREIGNKEY — SQLite's bare error message
       * names no column.  Probe each FK to pinpoint the violator.  Static
       * analysis can't reach it (paraphrase-merged fact_ids appear valid at
       * fact_map build time; no in-extraction delete path touches facts).
       * Live probing is the only way to see which row vanished by the time
       * the INSERT actually fired. */
      if (xrc == SQLITE_CONSTRAINT_FOREIGNKEY) {
         /* users table has no user_id column — scope=0. */
         int user_ok = memory_db_internal_fk_row_exists("users", (int64_t)user_id, 0);
         /* Entity/fact/conv tables all carry user_id — scope to the same user
          * so we don't read back "exists" for a foreign user's row. */
         int subj_ok = memory_db_internal_fk_row_exists("memory_entities", subject_entity_id,
                                                        user_id);
         int obj_ok = (object_entity_id > 0)
                          ? memory_db_internal_fk_row_exists("memory_entities", object_entity_id,
                                                             user_id)
                          : 1;
         int fact_ok = (fact_id > 0)
                           ? memory_db_internal_fk_row_exists("memory_facts", fact_id, user_id)
                           : 1;
         int conv_ok = (prov && prov->conv_id > 0)
                           ? memory_db_internal_fk_row_exists("conversations", prov->conv_id,
                                                              user_id)
                           : 1;
         OLOG_ERROR("memory_db: relation_supersede FK probe: user=%d subj=%d obj=%d "
                    "fact=%d conv=%d (1=exists, 0=missing, -1=check_failed)",
                    user_ok, subj_ok, obj_ok, fact_ok, conv_ok);
      }

      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   rc = sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: relation_supersede commit failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_UNLOCK();

   /* v49 dedup observability — log after the lock is released so the OLOG
    * call doesn't widen the critical section.  mention_count > 1 means the
    * upsert hit the ON CONFLICT path and bumped an existing row rather than
    * inserting a fresh one.  DEBUG-level: at extraction time this can fire
    * tens of times per conversation; aggregate visibility comes from the
    * one-shot v49 migration delta log and ad-hoc `SELECT mention_count ...
    * ORDER BY mention_count DESC` queries.  Also: object_value is user
    * content, so promoting to INFO would surface it in operator logs at
    * every re-witness.  Keep this gated to DEBUG. */
   if (upsert_mention_count > 1) {
      OLOG_DEBUG("memory_db: relation upserted (dedup hit) id=%lld user=%d subj=%lld rel='%s' "
                 "obj_id=%lld obj_val='%s' mention_count=%d",
                 (long long)upsert_id, user_id, (long long)subject_entity_id, relation,
                 (long long)object_entity_id, object_value ? object_value : "(null)",
                 upsert_mention_count);
   }
   return MEMORY_DB_SUCCESS;
}

/* Reads SELECTs ordered:
 *   0:id, 1:subject_entity_id, 2:relation, 3:object_entity_id,
 *   4:object_name, 5:confidence, 6:valid_from, 7:valid_to, 8:mention_count
 *
 * valid_* (cols 6, 7) appended in v33; mention_count (col 8) appended in v49.
 * COALESCE in SQL converts SQL NULL → 0 in C for the validity columns.
 *
 * Must stay column-symmetric with populate_relation_outgoing_row() in
 * memory_db_alias_writes.c — both helpers read the same column layout. */
static void populate_relation_from_row(sqlite3_stmt *stmt, memory_relation_t *rel) {
   rel->id = sqlite3_column_int64(stmt, 0);
   rel->subject_entity_id = sqlite3_column_int64(stmt, 1);

   const char *r = (const char *)sqlite3_column_text(stmt, 2);
   if (r) {
      safe_strscpy(rel->relation, r);
   }

   rel->object_entity_id = sqlite3_column_int64(stmt, 3);

   const char *obj_name = (const char *)sqlite3_column_text(stmt, 4);
   if (obj_name) {
      safe_strscpy(rel->object_name, obj_name);
   }

   rel->confidence = (float)sqlite3_column_double(stmt, 5);

   /* COALESCE in SQL means 0 if NULL — caller treats 0 as "open-ended". */
   rel->valid_from = sqlite3_column_int64(stmt, 6);
   rel->valid_to = sqlite3_column_int64(stmt, 7);

   rel->mention_count = sqlite3_column_int(stmt, 8);
}

int memory_db_relation_list_by_subject(int user_id,
                                       int64_t subject_entity_id,
                                       memory_relation_t *out,
                                       int max,
                                       int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || max <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_relation_list_by_subject);
   sqlite3_bind_int(s_db.stmt_memory_relation_list_by_subject, 1, user_id);
   sqlite3_bind_int64(s_db.stmt_memory_relation_list_by_subject, 2, subject_entity_id);
   sqlite3_bind_int(s_db.stmt_memory_relation_list_by_subject, 3, max);

   int count = 0;
   while (count < max && sqlite3_step(s_db.stmt_memory_relation_list_by_subject) == SQLITE_ROW) {
      populate_relation_from_row(s_db.stmt_memory_relation_list_by_subject, &out[count]);
      count++;
   }

   sqlite3_reset(s_db.stmt_memory_relation_list_by_subject);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_relation_list_by_object(int user_id,
                                      int64_t object_entity_id,
                                      memory_relation_t *out,
                                      int max,
                                      int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || max <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_relation_list_by_object);
   sqlite3_bind_int(s_db.stmt_memory_relation_list_by_object, 1, user_id);
   sqlite3_bind_int64(s_db.stmt_memory_relation_list_by_object, 2, object_entity_id);
   sqlite3_bind_int(s_db.stmt_memory_relation_list_by_object, 3, max);

   int count = 0;
   while (count < max && sqlite3_step(s_db.stmt_memory_relation_list_by_object) == SQLITE_ROW) {
      populate_relation_from_row(s_db.stmt_memory_relation_list_by_object, &out[count]);
      count++;
   }

   sqlite3_reset(s_db.stmt_memory_relation_list_by_object);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

/* Graph-retrieval Phase 1A helper: return DISTINCT fact_ids for fact-linked
 * relations touching @entity_id (as either subject or object).  Skips
 * relations with NULL fact_id — those are structured-only graph edges
 * that Phase 1B will synthesize separately.  Sorted by relation confidence
 * DESC then created_at DESC so the highest-quality graph anchors come
 * first when the fan-out cap (@max) trips.  Caller is responsible for
 * deduplicating across multiple seed entities. */
int memory_db_relation_fact_ids_for_entity(int user_id,
                                           int64_t entity_id,
                                           int64_t *out_fact_ids,
                                           int max,
                                           int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_fact_ids || max <= 0 || entity_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_relation_fact_ids_for_entity;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, entity_id);
   sqlite3_bind_int64(stmt, 3, entity_id);
   sqlite3_bind_int(stmt, 4, max);

   int count = 0;
   while (count < max && sqlite3_step(stmt) == SQLITE_ROW) {
      out_fact_ids[count++] = sqlite3_column_int64(stmt, 0);
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

/* List this user's distinct relation predicates, ordered by frequency.
 * Used by the Phase 0 extraction-prompt builder.  Ad-hoc prepared
 * statement (not cached in s_db.stmt_*) because this runs once per
 * extraction — bounded by call cadence, not by per-relation hot path. */
int memory_db_relation_distinct_predicates(int user_id,
                                           char out[][MEMORY_RELATION_MAX],
                                           int max,
                                           int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || max <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT relation, COUNT(*) AS n "
                               "  FROM memory_relations "
                               " WHERE user_id = ? "
                               " GROUP BY relation "
                               " ORDER BY n DESC "
                               " LIMIT ?",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("memory_db: prepare relation_distinct_predicates failed: %s",
                   sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max);

   int count = 0;
   while (count < max && sqlite3_step(stmt) == SQLITE_ROW) {
      const char *rel = (const char *)sqlite3_column_text(stmt, 0);
      if (!rel || !*rel)
         continue;
      safe_strscpy(out[count], rel);
      count++;
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

/* As-of variant.  When as_of_ts == 0, defaults to "currently valid".  Used by
 * the entity-recall block in memory_callback when the LLM passes as_of, and as
 * the default temporal filter for current-state queries. */
int memory_db_relation_list_by_subject_at(int user_id,
                                          int64_t subject_entity_id,
                                          int64_t as_of_ts,
                                          memory_relation_t *out,
                                          int max,
                                          int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || max <= 0)
      return MEMORY_DB_FAILURE;

   if (as_of_ts <= 0)
      as_of_ts = (int64_t)time(NULL);

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_relation_list_by_subject_at;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, subject_entity_id);
   sqlite3_bind_int64(stmt, 3, as_of_ts);
   sqlite3_bind_int64(stmt, 4, as_of_ts);
   sqlite3_bind_int(stmt, 5, max);

   int count = 0;
   while (count < max && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_relation_from_row(stmt, &out[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_relation_list_with_canonical_roots_by_user(int user_id,
                                                         memory_relation_t *out,
                                                         int64_t *subj_roots,
                                                         int64_t *obj_roots,
                                                         int max,
                                                         int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || !subj_roots || !obj_roots || max <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   /* Single query: all relations for this user with canonical-root id
    * resolved per side via COALESCE(canonical_id, id).  Object side reuses
    * the existing `e` JOIN that resolves object_name; subject side gets
    * its own JOIN aliased `s` (subject_entity_id is NOT NULL per schema,
    * so the s row always exists for valid relations).
    *
    * subj_root / obj_root: 0 means orphaned (LEFT JOIN missed) — callers
    * MUST filter via "root == specific_entity_id" rather than "root != 0"
    * to avoid attributing orphaned-subject relations to any entity card.
    *
    * No ORDER BY — sorting on the computed subj_root materializes a temp
    * B-tree (verified 2026-05-20 via EXPLAIN QUERY PLAN), and no caller
    * depends on iteration order.  valid_from/valid_to appended in v33,
    * mention_count appended in v49 — column order matches
    * populate_relation_from_row.  Roots follow at indices 9/10.
    *
    * EXPLAIN QUERY PLAN (Jetson, 2026-05-20):
    *   SEARCH r USING INDEX idx_memory_relations_user_validity (user_id=?)
    *   SEARCH e USING INTEGER PRIMARY KEY (rowid=?)
    *   SEARCH s USING INTEGER PRIMARY KEY (rowid=?)
    * Three indexed seeks; no scans, no temp tables. */
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT r.id, r.subject_entity_id, r.relation, r.object_entity_id, "
       "       COALESCE(e.name, r.object_value) AS object_name, r.confidence, "
       "       COALESCE(r.valid_from, 0), COALESCE(r.valid_to, 0), "
       "       r.mention_count, "
       "       COALESCE(s.canonical_id, s.id, 0) AS subj_root, "
       "       COALESCE(e.canonical_id, e.id, 0) AS obj_root "
       "FROM memory_relations r "
       "LEFT JOIN memory_entities e ON r.object_entity_id = e.id "
       "LEFT JOIN memory_entities s ON r.subject_entity_id = s.id "
       "WHERE r.user_id = ? "
       "LIMIT ?",
       -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max);

   int count = 0;
   while (count < max && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_relation_from_row(stmt, &out[count]);
      /* Roots are appended after the columns populate_relation_from_row
       * reads (it stops at mention_count / index 8 in v49).  Indices 9/10
       * are ours. */
      subj_roots[count] = sqlite3_column_int64(stmt, 9);
      obj_roots[count] = sqlite3_column_int64(stmt, 10);
      count++;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}
