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
 * plus the SPECIAL_RELATIONS / CONTRADICTORY_PAIRS tables and the
 * memory_db_relation_supersede atomic close-and-create transaction.
 *
 * Contradiction handling lives here: SPECIAL_RELATIONS[] and
 * CONTRADICTORY_PAIRS[] (compile-time data), and
 * memory_db_relation_supersede_ex(), which in one transaction closes what the
 * new value replaces, stores it, and retires the facts the closed relations
 * stated (unless another open relation still backs them).  Extraction only
 * invalidates its caches afterwards; it doesn't supersede facts itself.
 *
 * Adding a one-at-a-time relation or contradictory pair is a table edit.  A
 * subtler kind of contradiction (quantity, negation) would belong in its own
 * memory_contradiction.{c,h} module.
 *
 * One-at-a-time relations have at-most-one open instance per (subject,
 * relation).  When extraction stores a new such relation with a different
 * object, or any relation it marks as replacing the old value ("my new number
 * is ..."), memory_db_relation_supersede_ex() auto-closes the prior open row.
 * Others (likes, knows, has_pet, email_is) skip the close branch.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <stdio.h>
#include <stdlib.h>
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
/* The standard relations with special handling.  one_at_a_time: a person has
 * one value at a time, so a new value closes the open one.  merge_evidence:
 * entity merging scores a value two entities share as strong evidence they
 * are one (memory_db_alias.c; the list it has always used, not all of it
 * identifying).  A person can have several emails, phones, vehicles, jobs,
 * schools and homes, so those don't close each other unless the new one is
 * said to replace an old one (memory_db_relation_supersede_ex's replaces). */
static const struct {
   const char *name;
   bool one_at_a_time;
   bool merge_evidence;
} SPECIAL_RELATIONS[] = {
   { "married_to", true, true },      { "born_in", true, true },
   { "born_on", true, true },         { "favorite_color", true, true },
   { "favorite_food", true, true },   { "primary_language", true, true },
   { "works_at", false, true },       { "lives_in", false, true },
   { "attends_school", false, true }, { "owns_vehicle", false, true },
   { "email_is", false, true },       { "phone_number_is", false, true },
};
static const int SPECIAL_RELATIONS_COUNT = (int)(sizeof(SPECIAL_RELATIONS) /
                                                 sizeof(SPECIAL_RELATIONS[0]));

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

const char *memory_db_relation_contradictory_opposite(const char *relation) {
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

/* The relation's SPECIAL_RELATIONS entry, or -1. */
static int special_relation_index(const char *relation) {
   for (int i = 0; relation && i < SPECIAL_RELATIONS_COUNT; i++) {
      if (strcmp(relation, SPECIAL_RELATIONS[i].name) == 0) {
         return i;
      }
   }
   return -1;
}

bool memory_db_relation_is_one_at_a_time(const char *relation) {
   const int i = special_relation_index(relation);
   return i >= 0 && SPECIAL_RELATIONS[i].one_at_a_time;
}

bool memory_db_relation_is_merge_evidence(const char *relation) {
   const int i = special_relation_index(relation);
   return i >= 0 && SPECIAL_RELATIONS[i].merge_evidence;
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
   return memory_db_relation_create_ex(user_id, subject_entity_id, relation, object_entity_id,
                                       object_value, fact_id, confidence, valid_from, valid_to,
                                       prov, NULL, NULL);
}

int memory_db_relation_create_ex(int user_id,
                                 int64_t subject_entity_id,
                                 const char *relation,
                                 int64_t object_entity_id,
                                 const char *object_value,
                                 int64_t fact_id,
                                 float confidence,
                                 int64_t valid_from,
                                 int64_t valid_to,
                                 const memory_provenance_t *prov,
                                 int64_t *id_out,
                                 bool *created_out) {
   if (id_out) {
      *id_out = 0;
   }
   if (created_out) {
      *created_out = false;
   }
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
   sqlite3_bind_int(s_db.stmt_memory_relation_create, 13, prov ? prov->outside : OUTSIDE_NONE);

   /* The relation and its source commit together (a savepoint nests inside a
    * caller's transaction). */
   const bool sp = sqlite3_exec(s_db.db, "SAVEPOINT relation_create", NULL, NULL, NULL) ==
                   SQLITE_OK;

   /* v49 upsert: RETURNING id, mention_count.  First step yields SQLITE_ROW
    * (whether INSERT or ON CONFLICT path); drain to SQLITE_DONE.  No need to
    * consume the returned values here — observability of mention_count belongs
    * at the supersede call site where extraction can log dedup-hits. */
   int rc = sqlite3_step(s_db.stmt_memory_relation_create);
   bool ok = (rc == SQLITE_ROW || rc == SQLITE_DONE);
   int64_t rel_id = 0;
   bool created = false;
   if (rc == SQLITE_ROW) {
      rel_id = sqlite3_column_int64(s_db.stmt_memory_relation_create, 0);
      /* A new row starts at one mention; an open one said again counts up. */
      created = sqlite3_column_int(s_db.stmt_memory_relation_create, 1) == 1;
      if (prov) {
         memory_db_internal_source_add_locked(MEMORY_SOURCE_RELATION, rel_id, prov->conv_id);
      }
   }
   while (rc == SQLITE_ROW) {
      rc = sqlite3_step(s_db.stmt_memory_relation_create);
   }
   ok = ok && (rc == SQLITE_DONE);
   sqlite3_reset(s_db.stmt_memory_relation_create);
   if (sp) {
      if (!ok) {
         sqlite3_exec(s_db.db, "ROLLBACK TO relation_create", NULL, NULL, NULL);
      }
      sqlite3_exec(s_db.db, "RELEASE relation_create", NULL, NULL, NULL);
   }
   AUTH_DB_UNLOCK();

   if (ok && id_out) {
      *id_out = rel_id;
   }
   if (ok && created_out) {
      *created_out = created;
   }
   return ok ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

/* Transactional close-and-create.  For one-at-a-time relations (married_to,
 * born_in, ...), or a new value said to replace the old, any existing open row
 * with a different object is closed (valid_to = now()) before
 * the new row is inserted.  Both writes happen under one BEGIN IMMEDIATE so other
 * workers can never observe a state with zero open rows for the same (subject, relation).
 *
 * Other relations skip the close branch — multiple open rows are valid
 * (a user can like many things, know many people, have several emails). */
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
   return memory_db_relation_supersede_ex(user_id, subject_entity_id, relation, object_entity_id,
                                          object_value, fact_id, confidence, valid_from, valid_to,
                                          NULL, prov, out_old_fact_id);
}

/* Whether @p fact_id stays current when a relation it backed closes: it still
 * backs an open one, or the closing batch read outside text (@p tainted) and
 * the fact isn't from such a batch (caller holds the lock). */
static bool fact_stays_locked(int user_id, int64_t fact_id, bool tainted) {
   sqlite3_stmt *st = NULL;
   bool open = true; /* on error, keep the fact (never hide one by mistake) */
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT EXISTS (SELECT 1 FROM memory_relations WHERE user_id = ?1 AND "
                          "fact_id = ?2 AND valid_to IS NULL) OR (?3 AND EXISTS (SELECT 1 FROM "
                          "memory_facts WHERE id = ?2 AND user_id = ?1 AND outside IN "
                          "(" OUTSIDE_UNTAINTED_SQL ")))",
                          -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_int64(st, 2, fact_id);
      sqlite3_bind_int(st, 3, tainted);
      if (sqlite3_step(st) == SQLITE_ROW) {
         open = sqlite3_column_int(st, 0) != 0;
      }
   }
   sqlite3_finalize(st);
   return open;
}

/* Facts one supersede can close relations of; past it, the rest stay as they are. */
#define SUPERSEDE_FACTS_MAX 32

/* Bind a close statement's ?1-?6 (MEMORY_RELATION_CLOSE_OPEN_SQL's order; the
 * contradictory close binds the same six). */
static void bind_close_locked(sqlite3_stmt *st,
                              int64_t close_time,
                              int user_id,
                              int64_t subject_entity_id,
                              const char *relation,
                              int64_t object_entity_id,
                              const char *object_value) {
   sqlite3_bind_int64(st, 1, close_time);
   sqlite3_bind_int(st, 2, user_id);
   sqlite3_bind_int64(st, 3, subject_entity_id);
   sqlite3_bind_text(st, 4, relation, -1, SQLITE_TRANSIENT);
   if (object_entity_id > 0) {
      sqlite3_bind_int64(st, 5, object_entity_id);
   } else {
      sqlite3_bind_null(st, 5);
   }
   if (object_value) {
      sqlite3_bind_text(st, 6, object_value, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(st, 6);
   }
}

/* Step a close statement to the end, adding each returned fact_id (once) to
 * @p facts.  The step's final code (SQLITE_DONE when it ran). */
static int collect_closed_facts(sqlite3_stmt *st, int64_t *facts, int *n) {
   int rc;
   while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
      const int64_t id = sqlite3_column_int64(st, 0);
      bool seen = id <= 0;
      for (int i = 0; i < *n && !seen; i++) {
         seen = facts[i] == id;
      }
      if (!seen && *n < SUPERSEDE_FACTS_MAX) {
         facts[(*n)++] = id;
      }
   }
   return rc;
}

/* Open values of (subject, relation) other than the new one, or -1 on error. */
static int other_open_count_locked(int user_id,
                                   int64_t subject_entity_id,
                                   const char *relation,
                                   int64_t object_entity_id,
                                   const char *object_value) {
   sqlite3_stmt *st = NULL;
   int count = -1;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT COUNT(*) FROM memory_relations WHERE user_id = ?2 AND "
                          "subject_entity_id = ?3 AND relation = ?4 AND valid_to IS NULL AND "
                          "(COALESCE(object_entity_id, 0) != COALESCE(?5, 0) OR "
                          "COALESCE(object_value, '') != COALESCE(?6, '')) AND ?1 IS NOT NULL",
                          -1, &st, NULL) == SQLITE_OK) {
      bind_close_locked(st, 0, user_id, subject_entity_id, relation, object_entity_id,
                        object_value);
      if (sqlite3_step(st) == SQLITE_ROW) {
         count = sqlite3_column_int(st, 0);
      }
   }
   sqlite3_finalize(st);
   return count;
}


int memory_db_relation_supersede_ex(int user_id,
                                    int64_t subject_entity_id,
                                    const char *relation,
                                    int64_t object_entity_id,
                                    const char *object_value,
                                    int64_t fact_id,
                                    float confidence,
                                    int64_t valid_from,
                                    int64_t valid_to,
                                    const char *replaces,
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

   /* Auto-close prior open relation (one-at-a-time, or replaced), if any.  Idempotency check
    * (object mismatch) lives in MEMORY_RELATION_CLOSE_OPEN_SQL's WHERE clause.
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
   const int64_t close_time = (valid_from > 0) ? valid_from : now_ts;
   /* The facts of the rows closed here: each is superseded by the new fact
    * once the new row is in, when nothing open rests on it any more. */
   int64_t closed_facts[SUPERSEDE_FACTS_MAX];
   int n_closed_facts = 0;

   const bool one = memory_db_relation_is_one_at_a_time(relation);
   const bool named = !one && replaces && replaces[0];
   bool do_close = new_is_current && (one || replaces);
   if (do_close && !one && !named) {
      /* "Replaces", but not which: only when there is exactly one other value
       * to replace (with a work number and a cell, guessing would lose one). */
      const int others = other_open_count_locked(user_id, subject_entity_id, relation,
                                                 object_entity_id, object_value);
      if (others < 0) {
         OLOG_ERROR("memory_db: relation_supersede count failed: %s", sqlite3_errmsg(s_db.db));
         sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
         AUTH_DB_UNLOCK();
         return MEMORY_DB_FAILURE;
      }
      if (others != 1) {
         OLOG_INFO("memory_db: '%s' replaces one of %d values for subject %ld; none closed",
                   relation, others, (long)subject_entity_id);
         do_close = false;
      }
   }
   if (do_close) {
      sqlite3_stmt *close_stmt = s_db.stmt_memory_relation_close_open;
      sqlite3_reset(close_stmt);
      bind_close_locked(close_stmt, close_time, user_id, subject_entity_id, relation,
                        object_entity_id, object_value);
      if (prov && outside_is_tainted(prov->outside)) {
         sqlite3_bind_int(close_stmt, 9, 1);
      } else {
         sqlite3_bind_null(close_stmt, 9);
      }
      if (named) {
         sqlite3_bind_text(close_stmt, 7, replaces, -1, SQLITE_TRANSIENT);
      } else {
         sqlite3_bind_null(close_stmt, 7);
      }
      if (fact_id > 0) {
         sqlite3_bind_int64(close_stmt, 8, fact_id);
      } else {
         sqlite3_bind_null(close_stmt, 8);
      }
      rc = collect_closed_facts(close_stmt, closed_facts, &n_closed_facts);
      sqlite3_reset(close_stmt);
      if (rc != SQLITE_DONE) {
         OLOG_ERROR("memory_db: relation_supersede close failed: %s", sqlite3_errmsg(s_db.db));
         sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
         AUTH_DB_UNLOCK();
         return MEMORY_DB_FAILURE;
      }
      const int closed = sqlite3_changes(s_db.db);
      if (closed > 0) {
         OLOG_INFO("memory_db: closed %d superseded '%s' relation(s) for subject %ld", closed,
                   relation, (long)subject_entity_id);
      } else if (named) {
         OLOG_INFO("memory_db: '%s' replaces a value matching no other open one for subject "
                   "%ld; none closed",
                   relation, (long)subject_entity_id);
      }
   }

   /* Close contradictory relation (e.g., likes↔dislikes) on the same
    * (subject, object) pair.  Only applies to currently-true new relations. */
   const char *opposite = memory_db_relation_contradictory_opposite(relation);
   if (opposite && new_is_current) {
      sqlite3_stmt *cstmt = NULL;
      int rc2 = sqlite3_prepare_v2(
          s_db.db,
          "UPDATE memory_relations SET valid_to = ?1, "
          "closed_by_fact_id = ?7 "
          "WHERE user_id = ?2 AND subject_entity_id = ?3 AND relation = ?4 "
          "  AND valid_to IS NULL "
          "  AND COALESCE(object_entity_id, 0) = COALESCE(?5, 0) "
          "  AND COALESCE(object_value, '') = COALESCE(?6, '') "
          "  AND (?8 IS NULL OR outside NOT IN (" OUTSIDE_UNTAINTED_SQL ")) "
          "RETURNING fact_id",
          -1, &cstmt, NULL);
      if (rc2 != SQLITE_OK) {
         OLOG_ERROR("memory_db: prepare contradictory close failed: %s", sqlite3_errmsg(s_db.db));
         sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
         AUTH_DB_UNLOCK();
         return MEMORY_DB_FAILURE;
      }
      bind_close_locked(cstmt, close_time, user_id, subject_entity_id, opposite, object_entity_id,
                        object_value);
      if (fact_id > 0) {
         sqlite3_bind_int64(cstmt, 7, fact_id);
      } else {
         sqlite3_bind_null(cstmt, 7);
      }
      if (prov && outside_is_tainted(prov->outside)) {
         sqlite3_bind_int(cstmt, 8, 1);
      } else {
         sqlite3_bind_null(cstmt, 8);
      }
      const int before = n_closed_facts;
      rc2 = collect_closed_facts(cstmt, closed_facts, &n_closed_facts);
      sqlite3_finalize(cstmt);
      if (rc2 != SQLITE_DONE) {
         OLOG_ERROR("memory_db: contradictory close step failed: %s", sqlite3_errmsg(s_db.db));
         sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
         AUTH_DB_UNLOCK();
         return MEMORY_DB_FAILURE;
      }
      if (n_closed_facts > before || sqlite3_changes(s_db.db) > 0) {
         OLOG_INFO("memory_db: closed contradictory '%s' (opposite of '%s') "
                   "for subject %ld",
                   opposite, relation, (long)subject_entity_id);
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
   sqlite3_bind_int(create_stmt, 13, prov ? prov->outside : OUTSIDE_NONE);
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
      if (prov) {
         memory_db_internal_source_add_locked(MEMORY_SOURCE_RELATION, upsert_id, prov->conv_id);
      }
      do {
         rc = sqlite3_step(create_stmt);
      } while (rc == SQLITE_ROW);
   }
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

   /* A closed row's fact goes only when nothing it says is still open (a fact
    * listing two numbers stays when one of them changes), never the new fact
    * itself, never one outside text didn't teach for a batch that read some,
    * and only with a new fact to point at. */
   for (int i = 0; i < n_closed_facts; i++) {
      const int64_t old_id = closed_facts[i];
      if (old_id == fact_id ||
          fact_stays_locked(user_id, old_id, prov && outside_is_tainted(prov->outside))) {
         continue;
      }
      if (fact_id <= 0) {
         continue; /* nothing to point it at */
      }
      bool changed = false;
      if (memory_db_internal_fact_supersede_locked(
              old_id, fact_id, user_id, MEMORY_SUPERSEDED_REPLACED, &changed) != SQLITE_DONE) {
         OLOG_ERROR("memory_db: relation_supersede fact %lld failed: %s", (long long)old_id,
                    sqlite3_errmsg(s_db.db));
         sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
         AUTH_DB_UNLOCK();
         return MEMORY_DB_FAILURE;
      }
      if (changed && out_old_fact_id && *out_old_fact_id == 0) {
         *out_old_fact_id = old_id;
      }
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
 *   4:object_name, 5:confidence, 6:valid_from, 7:valid_to, 8:mention_count,
 *   9:outside
 *
 * valid_* (cols 6, 7) appended in v33; mention_count (col 8) in v49; outside
 * (col 9) in v113.
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
   rel->outside = sqlite3_column_int(stmt, 9);
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

/* Run a by-class id statement (?1 user, ?2 entity, ?3 limit, ?4 the fact
 * expiry cutoff when @p with_cutoff) into @p out_ids. */
static int class_ids(sqlite3_stmt *stmt,
                     int user_id,
                     int64_t entity_id,
                     bool with_cutoff,
                     int64_t *out_ids,
                     int max,
                     int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_ids || max <= 0 || entity_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, entity_id);
   sqlite3_bind_int(stmt, 3, max);
   if (with_cutoff)
      sqlite3_bind_int64(stmt, 4, memory_db_fact_expiry_cutoff());
   int count = 0;
   while (count < max && sqlite3_step(stmt) == SQLITE_ROW) {
      out_ids[count++] = sqlite3_column_int64(stmt, 0);
   }
   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

/* Fact ids of the fact-linked relations touching any name in @p entity_id's
 * class (statement memory_relation_fact_ids_for_entity).  The caller dedups
 * across seeds. */
int memory_db_relation_fact_ids_for_entity(int user_id,
                                           int64_t entity_id,
                                           int64_t *out_fact_ids,
                                           int max,
                                           int *count_out) {
   return class_ids(s_db.stmt_memory_relation_fact_ids_for_entity, user_id, entity_id, false,
                    out_fact_ids, max, count_out);
}

int memory_db_entity_class_members(int user_id,
                                   int64_t entity_id,
                                   int64_t *out_ids,
                                   int max,
                                   int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_ids || max <= 0 || entity_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *stmt = s_db.stmt_memory_entity_class_members;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, entity_id);
   sqlite3_bind_int(stmt, 3, max);
   int count = 0;
   while (count < max && sqlite3_step(stmt) == SQLITE_ROW) {
      out_ids[count++] = sqlite3_column_int64(stmt, 0);
   }
   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_ids_for_subject(int user_id,
                                   int64_t entity_id,
                                   int64_t *out_fact_ids,
                                   int max,
                                   int *count_out) {
   return class_ids(s_db.stmt_memory_fact_ids_for_subject, user_id, entity_id, true, out_fact_ids,
                    max, count_out);
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

/* Relations of @p entity_id (the subject or the object, by @p stmt) valid at
 * @p as_of_ts; 0 = now.  Validity is filtered in SQL, before the limit, so
 * ended relations can't crowd out valid ones. */
static int relation_list_valid_at(sqlite3_stmt *stmt,
                                  int user_id,
                                  int64_t entity_id,
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

   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, entity_id);
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

/* As-of variant.  When as_of_ts == 0, defaults to "currently valid".  Used by
 * the entity-recall block in memory_callback when the LLM passes as_of, and as
 * the default temporal filter for current-state queries. */
int memory_db_relation_list_by_subject_at(int user_id,
                                          int64_t subject_entity_id,
                                          int64_t as_of_ts,
                                          memory_relation_t *out,
                                          int max,
                                          int *count_out) {
   return relation_list_valid_at(s_db.stmt_memory_relation_list_by_subject_at, user_id,
                                 subject_entity_id, as_of_ts, out, max, count_out);
}

int memory_db_relation_list_by_object_at(int user_id,
                                         int64_t object_entity_id,
                                         int64_t as_of_ts,
                                         memory_relation_t *out,
                                         int max,
                                         int *count_out) {
   return relation_list_valid_at(s_db.stmt_memory_relation_list_by_object_at, user_id,
                                 object_entity_id, as_of_ts, out, max, count_out);
}

/* A JSON array of @p ids ("[1,2,3]") for json_each(); NULL on failure.
 * Caller frees. */
static char *ids_json(const int64_t *ids, int n) {
   const size_t cap = (size_t)n * 21 + 3;
   char *buf = malloc(cap);
   if (!buf) {
      return NULL;
   }
   size_t len = 0;
   buf[len++] = '[';
   for (int i = 0; i < n; i++) {
      char num[24];
      int w = snprintf(num, sizeof(num), i ? ",%lld" : "%lld", (long long)ids[i]);
      if (w < 0 || (size_t)w >= sizeof(num) || (size_t)w + 2 > cap - len) {
         free(buf);
         return NULL;
      }
      memcpy(buf + len, num, (size_t)w);
      len += (size_t)w;
   }
   buf[len++] = ']';
   buf[len] = '\0';
   return buf;
}

int memory_db_relation_list_for_entities(int user_id,
                                         const int64_t *entity_ids,
                                         int n_ids,
                                         int per_dir_max,
                                         bool with_incoming,
                                         memory_card_relation_t **out,
                                         int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || !count_out || n_ids < 0 || (n_ids > 0 && !entity_ids))
      return MEMORY_DB_FAILURE;
   *out = NULL;
   if (n_ids == 0)
      return MEMORY_DB_SUCCESS;

   /* cls: every entity whose relations belong on a listed card, with that
    * card: the listed entity itself (an alias id lists nothing of its own)
    * and the aliases linked to it.  cand: each member's strongest rows per
    * direction (outgoing, then incoming from outside its class), at most the
    * limit each, so no member's whole edge list is ranked; ranked then keeps
    * a card's strongest across its members.  tot: a card's full count per
    * direction, from index counts, so the limit can be told.  A negative
    * ?3 is no limit (SQLite reads LIMIT -1 as unlimited).
    *
    * The CROSS JOINs fix the join order: driven from the listed ids through
    * the subject/object indexes, never a scan of the user's whole relation
    * set (+x.user_id keeps the planner off the user_id indexes). */
   static const char sql[] =
       "WITH cls(id, card) AS ( "
       "  SELECT e.id, e.id FROM json_each(?2) j CROSS JOIN memory_entities e ON e.id = j.value "
       "   WHERE e.user_id = ?1 AND e.canonical_id IS NULL "
       "  UNION ALL "
       "  SELECT e.id, e.canonical_id FROM json_each(?2) j "
       "   CROSS JOIN memory_entities e ON e.canonical_id = j.value WHERE e.user_id = ?1), "
       "cand(card, incoming, rid, mc) AS ( "
       "  SELECT c.card, 0, r.id, r.mention_count FROM cls c "
       "   CROSS JOIN memory_relations r ON r.id IN ( "
       "     SELECT x.id FROM memory_relations x WHERE x.subject_entity_id = c.id AND +x.user_id = "
       "?1 "
       "      ORDER BY x.mention_count DESC, x.id DESC LIMIT ?3) "
       "  UNION ALL "
       "  SELECT c.card, 1, r.id, r.mention_count FROM cls c "
       "   CROSS JOIN memory_relations r ON r.id IN ( "
       "     SELECT x.id FROM memory_relations x LEFT JOIN memory_entities s ON s.id = "
       "x.subject_entity_id "
       "      WHERE x.object_entity_id = c.id AND +x.user_id = ?1 "
       "        AND COALESCE(s.canonical_id, s.id, 0) <> c.card "
       "      ORDER BY x.mention_count DESC, x.id DESC LIMIT ?3) "
       "   WHERE ?4), "
       "tot(card, incoming, total) AS ( "
       "  SELECT c.card, 0, SUM((SELECT COUNT(*) FROM memory_relations x "
       "                          WHERE x.subject_entity_id = c.id AND +x.user_id = ?1)) "
       "   FROM cls c GROUP BY c.card "
       "  UNION ALL "
       "  SELECT c.card, 1, SUM((SELECT COUNT(*) FROM memory_relations x "
       "                          LEFT JOIN memory_entities s ON s.id = x.subject_entity_id "
       "                          WHERE x.object_entity_id = c.id AND +x.user_id = ?1 "
       "                            AND COALESCE(s.canonical_id, s.id, 0) <> c.card)) "
       "   FROM cls c WHERE ?4 GROUP BY c.card), "
       "ranked AS (SELECT card, incoming, rid, "
       "  ROW_NUMBER() OVER (PARTITION BY card, incoming ORDER BY mc DESC, rid DESC) AS rn FROM "
       "cand) "
       "SELECT r.id, r.subject_entity_id, r.relation, "
       "       CASE WHEN k.incoming THEN COALESCE(s.canonical_id, s.id, 0) "
       "            ELSE r.object_entity_id END, "
       "       CASE WHEN k.incoming THEN sc.name ELSE COALESCE(e.name, r.object_value) END, "
       "       r.confidence, COALESCE(r.valid_from, 0), COALESCE(r.valid_to, 0), "
       "       r.mention_count, r.outside, k.card, k.incoming, t.total "
       "FROM ranked k JOIN tot t ON t.card = k.card AND t.incoming = k.incoming "
       "CROSS JOIN memory_relations r ON r.id = k.rid "
       "LEFT JOIN memory_entities e ON e.id = r.object_entity_id "
       "LEFT JOIN memory_entities s ON s.id = r.subject_entity_id "
       "LEFT JOIN memory_entities sc ON sc.id = COALESCE(s.canonical_id, s.id) "
       "WHERE ?3 < 0 OR k.rn <= ?3 "
       "ORDER BY k.card, k.incoming, k.rn";

   char *ids = ids_json(entity_ids, n_ids);
   if (!ids) {
      return MEMORY_DB_FAILURE;
   }

   /* Locked by hand: the ids buffer must be freed on the not-ready path. */
   pthread_mutex_lock(&s_db.mutex);
   sqlite3_stmt *stmt = NULL;
   if (!s_db.initialized || sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      free(ids);
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, ids, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 3, per_dir_max > 0 ? per_dir_max : -1);
   sqlite3_bind_int(stmt, 4, with_incoming ? 1 : 0);

   memory_card_relation_t *rows = NULL;
   int count = 0;
   int cap = 0;
   int rc = MEMORY_DB_SUCCESS;
   int step;
   while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
      if (count == cap) {
         const int next = cap ? cap * 2 : 64;
         memory_card_relation_t *grown = realloc(rows, (size_t)next * sizeof(*rows));
         if (!grown) {
            rc = MEMORY_DB_FAILURE;
            break;
         }
         rows = grown;
         cap = next;
      }
      memory_card_relation_t *row = &rows[count++];
      memset(row, 0, sizeof(*row));
      populate_relation_from_row(stmt, &row->rel);
      row->card_id = sqlite3_column_int64(stmt, 10);
      row->incoming = sqlite3_column_int(stmt, 11) != 0;
      row->total = sqlite3_column_int(stmt, 12);
   }
   if (rc == MEMORY_DB_SUCCESS && step != SQLITE_DONE) {
      rc = MEMORY_DB_FAILURE;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   free(ids);

   if (rc != MEMORY_DB_SUCCESS) {
      free(rows);
      return rc;
   }
   *out = rows;
   *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_relation_close(int user_id,
                             int64_t relation_id,
                             int64_t valid_to,
                             int64_t closed_by_fact_id) {
   if (user_id <= 0 || relation_id <= 0 || valid_to <= 0) {
      return MEMORY_DB_FAILURE;
   }
   /* Open only, never before it began, and never more than a day ahead (a
    * date written in another zone): a close records when a value ended. */
   if (valid_to > (int64_t)time(NULL) + 86400) {
      return MEMORY_DB_NOT_FOUND;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "UPDATE memory_relations SET valid_to = ?3, closed_by_fact_id = ?4 "
                               "WHERE id = ?2 AND user_id = ?1 AND valid_to IS NULL AND "
                               "(valid_from IS NULL OR valid_from <= ?3)",
                               -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_int64(st, 2, relation_id);
      sqlite3_bind_int64(st, 3, valid_to);
      if (closed_by_fact_id > 0) {
         sqlite3_bind_int64(st, 4, closed_by_fact_id);
      } else {
         sqlite3_bind_null(st, 4);
      }
      rc = sqlite3_step(st);
   }
   const int changed = rc == SQLITE_DONE ? sqlite3_changes(s_db.db) : 0;
   if (rc != SQLITE_DONE) {
      OLOG_WARNING("memory_db: closing relation %lld failed: %s", (long long)relation_id,
                   sqlite3_errmsg(s_db.db));
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   if (rc != SQLITE_DONE) {
      return MEMORY_DB_FAILURE;
   }
   return changed > 0 ? MEMORY_DB_SUCCESS : MEMORY_DB_NOT_FOUND;
}

int memory_db_relation_list_open(int user_id,
                                 int64_t subject_entity_id,
                                 const char *relation,
                                 memory_relation_t *out,
                                 int max,
                                 int *count_out) {
   *count_out = 0;
   if (!relation || !out || max <= 0) {
      return MEMORY_DB_FAILURE;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   /* idx_memory_relations_subject_open (subject_entity_id, relation) WHERE valid_to IS NULL */
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT r.id, r.subject_entity_id, r.relation, r.object_entity_id, "
                               "COALESCE(e.name, r.object_value), r.confidence, "
                               "COALESCE(r.valid_from, 0), COALESCE(r.valid_to, 0), "
                               "r.mention_count, r.outside FROM memory_relations r LEFT JOIN "
                               "memory_entities e ON r.object_entity_id = e.id WHERE "
                               "r.subject_entity_id = ?2 AND r.relation = ?3 AND r.valid_to IS "
                               "NULL AND r.user_id = ?1 ORDER BY r.id DESC LIMIT ?4",
                               -1, &st, NULL);
   int n = 0;
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_int64(st, 2, subject_entity_id);
      sqlite3_bind_text(st, 3, relation, -1, SQLITE_STATIC);
      sqlite3_bind_int(st, 4, max);
      while ((rc = sqlite3_step(st)) == SQLITE_ROW && n < max) {
         memset(&out[n], 0, sizeof(out[n]));
         populate_relation_from_row(st, &out[n]);
         n++;
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
