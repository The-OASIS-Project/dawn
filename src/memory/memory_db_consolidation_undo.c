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
 * Undoing what a consolidation batch did, one op at a time: each op's
 * check against what it left (its post-image) and its write back to what was
 * there before (its pre-image) in one transaction, so nothing a later change
 * made is ever undone.  The caller holds the user's extraction and marks the
 * removal (memory_consolidation_undo.c).
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_db_consolidation.h"
#include "memory/memory_db_internal.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_embeddings_internal.h"
#include "utils/string_utils.h"

/* Ops whose undo puts something back, as a SQL list. */
#define UNDOABLE_SQL "'added', 'reinforced', 'merged', 'replaced', 'closed', 'linked', 'pref_set'"

/* =============================================================================
 * Images
 * ============================================================================= */

static struct json_object *parse_object(const char *s) {
   struct json_object *o = s ? json_tokener_parse(s) : NULL;
   if (o && !json_object_is_type(o, json_type_object)) {
      json_object_put(o);
      o = NULL;
   }
   return o;
}

static bool has(struct json_object *o, const char *key) {
   return o && json_object_object_get_ex(o, key, NULL);
}

static int64_t get_i64(struct json_object *o, const char *key) {
   struct json_object *v = NULL;
   return o && json_object_object_get_ex(o, key, &v) && v ? json_object_get_int64(v) : 0;
}

static double get_double(struct json_object *o, const char *key) {
   struct json_object *v = NULL;
   return o && json_object_object_get_ex(o, key, &v) && v ? json_object_get_double(v) : 0.0;
}

static const char *get_str(struct json_object *o, const char *key) {
   struct json_object *v = NULL;
   return o && json_object_object_get_ex(o, key, &v) && json_object_is_type(v, json_type_string)
              ? json_object_get_string(v)
              : NULL;
}

static bool is_op(const char *op, const char *name) {
   return strcmp(op, name) == 0;
}

/* Whether @p op is recorded with what undoing it needs (rows recorded before
 * an image held it have no undo). */
static bool images_suffice(const char *op,
                           struct json_object *pre,
                           struct json_object *post,
                           const memory_consolidation_op_row_t *o) {
   if (!post) {
      return false;
   }
   if (is_op(op, "added") || is_op(op, "merged") || is_op(op, "replaced")) {
      return o->fact_id > 0;
   }
   if (is_op(op, "reinforced")) {
      struct json_object *rels = NULL;
      return o->fact_id > 0 && has(pre, "expires_at") &&
             json_object_object_get_ex(pre, "relation_ids", &rels) &&
             json_object_is_type(rels, json_type_array);
   }
   if (is_op(op, "closed")) {
      return o->relation_id > 0;
   }
   if (is_op(op, "linked")) {
      return o->link_id > 0;
   }
   if (is_op(op, "pref_set")) {
      return get_str(pre, "category") && (has(pre, "absent") || has(pre, "outside"));
   }
   return false;
}

bool memory_consolidation_op_undoable(const memory_consolidation_op_row_t *o) {
   if (!o || strcmp(o->status, "applied") != 0) {
      return false;
   }
   struct json_object *pre = parse_object(o->pre_image);
   struct json_object *post = parse_object(o->post_image);
   const bool ok = images_suffice(o->op, pre, post, o);
   json_object_put(pre);
   json_object_put(post);
   return ok;
}

bool memory_consolidation_op_keepable(const memory_consolidation_op_row_t *o) {
   if (!o || !o->pre_image) {
      return false;
   }
   return (is_op(o->op, "dropped") && is_op(o->status, "applied")) ||
          (is_op(o->op, "deferred") &&
           (is_op(o->status, "applied") || is_op(o->status, "expired")));
}

/* =============================================================================
 * One op
 * ============================================================================= */

/* An op as undo reads it. */
typedef struct {
   memory_consolidation_op_row_t row;
   int64_t conversation_id; /* the conversation it learned from: its own, else the batch's */
   struct json_object *pre;
   struct json_object *post;
} undo_row_t;

static void undo_row_free(undo_row_t *r) {
   memory_db_consolidation_ops_free(&r->row, 1);
   json_object_put(r->pre);
   json_object_put(r->post);
   r->pre = NULL;
   r->post = NULL;
}

/* Op @p op_id of @p batch_id and @p user_id.  Lock held. */
static int load_locked(int user_id, int64_t batch_id, int64_t op_id, undo_row_t *r) {
   memset(r, 0, sizeof(*r));
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT o.op, o.status, COALESCE(o.fact_id, 0), COALESCE(o.other_fact_id, 0), "
       "COALESCE(o.relation_id, 0), COALESCE(o.link_id, 0), o.pre_image, o.post_image, "
       "COALESCE(o.source_conversation_id, b.conversation_id, 0), o.seq FROM "
       "memory_consolidation_ops o "
       "JOIN memory_consolidation_batches b ON b.id = o.batch_id WHERE o.id = ?1 AND o.batch_id = "
       "?2 AND o.user_id = ?3 AND b.user_id = ?3",
       -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, op_id);
      sqlite3_bind_int64(st, 2, batch_id);
      sqlite3_bind_int(st, 3, user_id);
      rc = sqlite3_step(st);
   }
   if (rc == SQLITE_ROW) {
      const char *op = (const char *)sqlite3_column_text(st, 0);
      const char *status = (const char *)sqlite3_column_text(st, 1);
      r->row.id = op_id;
      r->row.batch_id = batch_id;
      safe_strncpy(r->row.op, op ? op : "", sizeof(r->row.op));
      safe_strncpy(r->row.status, status ? status : "", sizeof(r->row.status));
      r->row.fact_id = sqlite3_column_int64(st, 2);
      r->row.other_fact_id = sqlite3_column_int64(st, 3);
      r->row.relation_id = sqlite3_column_int64(st, 4);
      r->row.link_id = sqlite3_column_int64(st, 5);
      const unsigned char *pre = sqlite3_column_text(st, 6);
      const unsigned char *post = sqlite3_column_text(st, 7);
      r->row.pre_image = pre ? strdup((const char *)pre) : NULL;
      r->row.post_image = post ? strdup((const char *)post) : NULL;
      r->conversation_id = sqlite3_column_int64(st, 8);
      r->row.seq = sqlite3_column_int(st, 9);
      r->pre = parse_object(r->row.pre_image);
      r->post = parse_object(r->row.post_image);
   }
   sqlite3_finalize(st);
   return rc == SQLITE_ROW    ? MEMORY_DB_SUCCESS
          : rc == SQLITE_DONE ? MEMORY_DB_NOT_FOUND
                              : MEMORY_DB_FAILURE;
}

/* A one-row read: SQLITE_ROW with @p st left to read, else finalized. */
static int read_row(sqlite3_stmt **st, const char *sql, int user_id, int64_t id) {
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(*st, 1, user_id);
      sqlite3_bind_int64(*st, 2, id);
      rc = sqlite3_step(*st);
   }
   if (rc != SQLITE_ROW) {
      sqlite3_finalize(*st);
      *st = NULL;
   }
   return rc;
}

static int64_t col_i64(sqlite3_stmt *st, int col) {
   return sqlite3_column_type(st, col) == SQLITE_NULL ? 0 : sqlite3_column_int64(st, col);
}

/* An undo's write: SQLITE_DONE, or MEMORY_DB_FAILURE. */
static int exec_ok(const char *sql, int user_id, int64_t a, int64_t b) {
   return memory_db_internal_exec_locked(sql, user_id, a, b) == SQLITE_DONE ? MEMORY_DB_SUCCESS
                                                                            : MEMORY_DB_FAILURE;
}

/* added: the fact deleted, unless anything but this batch has a hand in it
 * since (a merge or replace landed on it, another conversation taught it, the
 * user remembered it on request). */
static int undo_added(int user_id,
                      const undo_row_t *r,
                      const char *stems,
                      memory_consolidation_undo_effect_t *eff) {
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT fact_text, superseded_by IS NOT NULL, origin_unsourced, "
       "COALESCE(source_conversation_id, 0), EXISTS (SELECT 1 FROM "
       "memory_facts m WHERE m.superseded_by = f.id), EXISTS (SELECT 1 FROM "
       "memory_fact_sources s WHERE s.fact_id = f.id AND s.conversation_id "
       "!= ?3) FROM memory_facts f WHERE user_id = ?1 AND id = ?2",
       -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_int64(st, 2, r->row.fact_id);
      sqlite3_bind_int64(st, 3, r->conversation_id);
      rc = sqlite3_step(st);
   }
   bool stale = rc != SQLITE_ROW;
   if (rc == SQLITE_ROW) {
      const char *text = (const char *)sqlite3_column_text(st, 0);
      const char *was = get_str(r->post, "text");
      const int64_t own = sqlite3_column_int64(st, 3);
      stale = (was && (!text || strcmp(text, was) != 0)) || sqlite3_column_int(st, 1) ||
              sqlite3_column_int(st, 2) || (own != 0 && own != r->conversation_id) ||
              sqlite3_column_int(st, 4) || sqlite3_column_int(st, 5);
   }
   sqlite3_finalize(st);
   if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
      return MEMORY_DB_FAILURE;
   }
   if (stale) {
      return MEMORY_CONSOLIDATION_STALE;
   }
   int entities_gone = 0;
   bool deleted = false;
   if (memory_db_internal_fact_delete_locked(user_id, r->row.fact_id, stems, &entities_gone,
                                             &deleted) != MEMORY_DB_SUCCESS) {
      return MEMORY_DB_FAILURE;
   }
   eff->facts = eff->facts || deleted;
   eff->entities = eff->entities || entities_gone > 0;
   return deleted ? MEMORY_DB_SUCCESS : MEMORY_CONSOLIDATION_STALE;
}

/* The relations a restatement created, still as it left them: none closed
 * (or its end changed) since, or said again.  @p ends: each one's valid_to as
 * created (0: open; rows recorded without them were all created open). */
static int restatement_relations_unchanged(int user_id,
                                           struct json_object *ids,
                                           struct json_object *ends,
                                           bool *same) {
   *same = true;
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT mention_count > 1 OR COALESCE(valid_to, 0) != ?3 FROM "
                               "memory_relations WHERE user_id = ?1 AND id = ?2",
                               -1, &st, NULL);
   const size_t n = json_object_array_length(ids);
   const bool have_ends = json_object_is_type(ends, json_type_array) &&
                          json_object_array_length(ends) == n;
   for (size_t i = 0; rc == SQLITE_OK && i < n && *same; i++) {
      sqlite3_reset(st);
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_int64(st, 2, json_object_get_int64(json_object_array_get_idx(ids, i)));
      sqlite3_bind_int64(st, 3,
                         have_ends ? json_object_get_int64(json_object_array_get_idx(ends, i)) : 0);
      const int step = sqlite3_step(st);
      if (step == SQLITE_ROW) {
         *same = sqlite3_column_int(st, 0) == 0;
      } else if (step != SQLITE_DONE) {
         rc = step; /* gone (a forget) is fine: nothing to take back */
      }
   }
   sqlite3_finalize(st);
   return rc == SQLITE_OK ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

/* The fact's own source columns, back to @p pre's when they name the
 * reinforcing conversation (a later conversation's reinforce owns them then).
 * A source conversation deleted since, or one a forget cleared (0 marked
 * source_forgotten), goes as auth_db_conv.c's delete takes it: another of its
 * sources, no range, and learned outside any conversation when none is left.
 * An unmarked 0 is a fact that never had one. */
static int restore_provenance(int user_id, const undo_row_t *r, int64_t own_now) {
   if (own_now != r->conversation_id || get_i64(r->pre, "source_conversation_id") == own_now) {
      return MEMORY_DB_SUCCESS;
   }
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE memory_facts SET source_conversation_id = COALESCE((SELECT id FROM conversations "
       "WHERE id = ?3), (SELECT MAX(conversation_id) FROM memory_fact_sources WHERE fact_id = "
       "?2)), source_msg_id_start = (SELECT NULLIF(?4, 0) FROM conversations WHERE id = ?3), "
       "source_msg_id_end = (SELECT NULLIF(?5, 0) FROM conversations WHERE id = ?3), "
       "origin_unsourced = CASE WHEN (?3 = 0 AND ?6 = 0) OR EXISTS (SELECT 1 FROM conversations "
       "WHERE id = ?3) OR EXISTS (SELECT 1 FROM memory_fact_sources WHERE fact_id = ?2) THEN "
       "origin_unsourced ELSE 1 END WHERE user_id = ?1 AND id = ?2",
       -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_int64(st, 2, r->row.fact_id);
      sqlite3_bind_int64(st, 3, get_i64(r->pre, "source_conversation_id"));
      sqlite3_bind_int64(st, 4, get_i64(r->pre, "msg_id_start"));
      sqlite3_bind_int64(st, 5, get_i64(r->pre, "msg_id_end"));
      sqlite3_bind_int(st, 6, has(r->pre, "source_forgotten") ? 1 : 0);
      rc = sqlite3_step(st);
   }
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

/* The relations a restatement created deleted, and the entities nothing names
 * once they (and the subject it set) are gone. */
static int take_back_relations(int user_id,
                               const undo_row_t *r,
                               struct json_object *rels,
                               memory_consolidation_undo_effect_t *eff) {
   int ok = exec_ok("CREATE TEMP TABLE IF NOT EXISTS undo_entities(id INTEGER PRIMARY KEY)", 0, 0,
                    0);
   if (ok == MEMORY_DB_SUCCESS) {
      ok = exec_ok("DELETE FROM undo_entities", 0, 0, 0);
   }
   const size_t n = json_object_array_length(rels);
   for (size_t i = 0; ok == MEMORY_DB_SUCCESS && i < n; i++) {
      const int64_t rel = json_object_get_int64(json_object_array_get_idx(rels, i));
      ok = exec_ok("INSERT OR IGNORE INTO undo_entities SELECT subject_entity_id FROM "
                   "memory_relations WHERE user_id = ?1 AND id = ?2 UNION SELECT object_entity_id "
                   "FROM memory_relations WHERE user_id = ?1 AND id = ?2 AND object_entity_id IS "
                   "NOT NULL",
                   user_id, rel, 0);
      if (ok == MEMORY_DB_SUCCESS) {
         ok = exec_ok("DELETE FROM memory_relations WHERE user_id = ?1 AND id = ?2", user_id, rel,
                      0);
      }
   }
   /* A subject the restatement set where there was none. */
   const int64_t subject = get_i64(r->post, "subject_entity_id");
   if (ok == MEMORY_DB_SUCCESS && subject > 0 && get_i64(r->pre, "subject_entity_id") == 0) {
      ok = exec_ok("INSERT OR IGNORE INTO undo_entities SELECT ?2 WHERE ?1 > 0", user_id, subject,
                   0);
   }
   if (ok == MEMORY_DB_SUCCESS) {
      ok = memory_db_internal_exec_locked(
               "DELETE FROM memory_entities WHERE user_id = ?1 AND id IN "
               "(SELECT id FROM undo_entities) AND " MEMORY_ENTITY_UNREFERENCED,
               user_id, 0, 0) == SQLITE_DONE
               ? MEMORY_DB_SUCCESS
               : MEMORY_DB_FAILURE;
      eff->entities = eff->entities || (ok == MEMORY_DB_SUCCESS && sqlite3_changes(s_db.db) > 0);
   }
   return ok;
}

/* reinforced: what the restatement raised or set (confidence, told, expiry,
 * subject, its source, the relations it created) put back.  Confidence goes
 * down by what the reinforce added: the nightly decay moves it meanwhile, so
 * it is not part of the check. */
static int undo_reinforced(int user_id,
                           const undo_row_t *r,
                           memory_consolidation_undo_effect_t *eff) {
   /* Another writer confirmed it since when it is learned from a
    * conversation the reinforce left it without (other than this one), or
    * outside any conversation now and not then (rows recorded without them:
    * not checked). */
   struct json_object *post_sources = NULL;
   const bool checks_sources = json_object_object_get_ex(r->post, "sources", &post_sources) &&
                               json_object_is_type(post_sources, json_type_array);
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT confidence, source, COALESCE(expires_at, 0), COALESCE(subject_entity_id, 0), "
       "superseded_by IS NOT NULL, COALESCE(source_conversation_id, 0), origin_unsourced, "
       "EXISTS (SELECT 1 FROM memory_fact_sources s WHERE s.fact_id = f.id AND s.conversation_id "
       "!= ?3 AND s.conversation_id NOT IN (SELECT value FROM json_each(?4))) FROM memory_facts "
       "f WHERE user_id = ?1 AND id = ?2",
       -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_int64(st, 2, r->row.fact_id);
      sqlite3_bind_int64(st, 3, r->conversation_id);
      sqlite3_bind_text(st, 4,
                        checks_sources
                            ? json_object_to_json_string_ext(post_sources, JSON_C_TO_STRING_PLAIN)
                            : "[]",
                        -1, SQLITE_TRANSIENT);
      rc = sqlite3_step(st);
   }
   if (rc != SQLITE_ROW) {
      sqlite3_finalize(st);
      return rc == SQLITE_DONE ? MEMORY_CONSOLIDATION_STALE : MEMORY_DB_FAILURE;
   }
   const double confidence = sqlite3_column_double(st, 0);
   const char *source = (const char *)sqlite3_column_text(st, 1);
   const char *post_source = get_str(r->post, "source");
   const bool stale = sqlite3_column_int(st, 4) ||
                      (post_source && (!source || strcmp(source, post_source) != 0)) ||
                      col_i64(st, 2) != get_i64(r->post, "expires_at") ||
                      col_i64(st, 3) != get_i64(r->post, "subject_entity_id") ||
                      (checks_sources && sqlite3_column_int(st, 7) != 0) ||
                      (has(r->post, "origin_unsourced") &&
                       sqlite3_column_int(st, 6) > get_i64(r->post, "origin_unsourced"));
   const int64_t own_now = col_i64(st, 5);
   sqlite3_finalize(st);
   struct json_object *rels = NULL;
   struct json_object *rel_ends = NULL;
   json_object_object_get_ex(r->pre, "relation_ids", &rels);
   json_object_object_get_ex(r->pre, "relation_valid_to", &rel_ends);
   bool rels_same = true;
   if (!stale &&
       restatement_relations_unchanged(user_id, rels, rel_ends, &rels_same) != MEMORY_DB_SUCCESS) {
      return MEMORY_DB_FAILURE;
   }
   if (stale || !rels_same) {
      return MEMORY_CONSOLIDATION_STALE;
   }

   double restored = confidence -
                     (get_double(r->post, "confidence") - get_double(r->pre, "confidence"));
   restored = restored < 0.0 ? 0.0 : restored > 1.0 ? 1.0 : restored;
   const char *pre_source = get_str(r->pre, "source");
   int ok = MEMORY_DB_SUCCESS;
   st = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE memory_facts SET confidence = ?3, source = COALESCE(?4, source), "
                          "expires_at = NULLIF(?5, 0), subject_entity_id = NULLIF(?6, 0) WHERE "
                          "user_id = ?1 AND id = ?2",
                          -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_int64(st, 2, r->row.fact_id);
      sqlite3_bind_double(st, 3, restored);
      if (pre_source) {
         sqlite3_bind_text(st, 4, pre_source, -1, SQLITE_TRANSIENT);
      } else {
         sqlite3_bind_null(st, 4);
      }
      sqlite3_bind_int64(st, 5, get_i64(r->pre, "expires_at"));
      sqlite3_bind_int64(st, 6, get_i64(r->pre, "subject_entity_id"));
      ok = sqlite3_step(st) == SQLITE_DONE ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
   } else {
      ok = MEMORY_DB_FAILURE;
   }
   sqlite3_finalize(st);
   /* The relations it created, the entities only they (or the subject it
    * set) named, and the source it added. */
   if (ok == MEMORY_DB_SUCCESS) {
      ok = take_back_relations(user_id, r, rels, eff);
   }
   if (ok == MEMORY_DB_SUCCESS && r->conversation_id > 0 && has(r->pre, "had_source") &&
       !json_object_get_boolean(json_object_object_get(r->pre, "had_source"))) {
      bool removed = false;
      ok = memory_db_internal_fact_source_remove_locked(r->row.fact_id, user_id, r->conversation_id,
                                                        &removed) == SQLITE_DONE
               ? MEMORY_DB_SUCCESS
               : MEMORY_DB_FAILURE;
   }
   if (ok == MEMORY_DB_SUCCESS) {
      ok = restore_provenance(user_id, r, own_now);
   }
   eff->facts = eff->facts || ok == MEMORY_DB_SUCCESS;
   return ok;
}

/* A merge also gave the fact kept the duplicate's sources (and, from those,
 * its having been learned outside any conversation): what the merge's
 * pre-image says it added is taken back off @p keeper. */
static int merge_sources_back(int user_id, const undo_row_t *r, int64_t keeper) {
   struct json_object *added = NULL;
   if (!is_op(r->row.op, "merged") || !r->pre ||
       !json_object_object_get_ex(r->pre, "added_sources", &added) ||
       !json_object_is_type(added, json_type_array)) {
      return MEMORY_DB_SUCCESS; /* recorded without them: the sources stay */
   }
   int ok = MEMORY_DB_SUCCESS;
   for (size_t i = 0; ok == MEMORY_DB_SUCCESS && i < json_object_array_length(added); i++) {
      bool removed = false;
      ok = memory_db_internal_fact_source_remove_locked(
               keeper, user_id, json_object_get_int64(json_object_array_get_idx(added, i)),
               &removed) == SQLITE_DONE
               ? MEMORY_DB_SUCCESS
               : MEMORY_DB_FAILURE;
   }
   /* Learned outside any conversation: back only when this merge made it so
    * (before and after differ, and it still reads as the merge left it).  The
    * keeper's outside kind is left alone: a batch's merge never changes it
    * (it joins only items of one class), and putting it back could un-taint
    * what a later merge outside the record made tainted. */
   const int64_t before = get_i64(r->pre, "keeper_origin_unsourced");
   if (ok == MEMORY_DB_SUCCESS && has(r->post, "keeper_origin_unsourced") &&
       get_i64(r->post, "keeper_origin_unsourced") != before) {
      sqlite3_stmt *st = NULL;
      ok = sqlite3_prepare_v2(s_db.db,
                              "UPDATE memory_facts SET origin_unsourced = ?3 WHERE user_id = ?1 "
                              "AND id = ?2 AND origin_unsourced = ?4",
                              -1, &st, NULL) == SQLITE_OK
               ? MEMORY_DB_SUCCESS
               : MEMORY_DB_FAILURE;
      if (ok == MEMORY_DB_SUCCESS) {
         sqlite3_bind_int(st, 1, user_id);
         sqlite3_bind_int64(st, 2, keeper);
         sqlite3_bind_int(st, 3, (int)before);
         sqlite3_bind_int(st, 4, (int)get_i64(r->post, "keeper_origin_unsourced"));
         ok = sqlite3_step(st) == SQLITE_DONE ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
      }
      sqlite3_finalize(st);
   }
   return ok;
}

/* merged / replaced: the fact current again (memory_db_fact_restore). */
static int undo_retired(int user_id, const undo_row_t *r, memory_consolidation_undo_effect_t *eff) {
   sqlite3_stmt *st = NULL;
   const int rc = read_row(&st,
                           "SELECT COALESCE(superseded_by, 0), superseded_kind, "
                           "COALESCE(superseded_at, 0) FROM memory_facts WHERE user_id = ?1 AND "
                           "id = ?2",
                           user_id, r->row.fact_id);
   if (rc != SQLITE_ROW) {
      return rc == SQLITE_DONE ? MEMORY_CONSOLIDATION_STALE : MEMORY_DB_FAILURE;
   }
   const int want_kind = is_op(r->row.op, "merged") ? MEMORY_SUPERSEDED_MERGED
                                                    : MEMORY_SUPERSEDED_REPLACED;
   /* Restored and retired again since, even to the same fact, is another
    * retirement (its own time). */
   const bool stale = sqlite3_column_int64(st, 0) != get_i64(r->post, "superseded_by") ||
                      sqlite3_column_int(st, 1) != want_kind ||
                      (has(r->post, "superseded_at") &&
                       col_i64(st, 2) != get_i64(r->post, "superseded_at"));
   sqlite3_finalize(st);
   if (stale) {
      return MEMORY_CONSOLIDATION_STALE;
   }
   int64_t keeper = 0;
   memory_restore_result_t rr;
   int restored = memory_db_internal_fact_restore_locked(user_id, r->row.fact_id, &keeper, &rr);
   if (restored == MEMORY_DB_NOT_FOUND) {
      return MEMORY_CONSOLIDATION_STALE;
   }
   if (restored == MEMORY_DB_SUCCESS) {
      restored = merge_sources_back(user_id, r, keeper);
   }
   eff->facts = eff->facts || restored == MEMORY_DB_SUCCESS;
   return restored;
}

static bool fact_exists_locked(int user_id, int64_t fact_id) {
   sqlite3_stmt *st = NULL;
   const int rc = read_row(&st, "SELECT 1 FROM memory_facts WHERE user_id = ?1 AND id = ?2",
                           user_id, fact_id);
   sqlite3_finalize(st);
   return rc == SQLITE_ROW;
}

/* closed: the relation open again, unless another value is open now. */
static int undo_closed(int user_id, const undo_row_t *r) {
   sqlite3_stmt *st = NULL;
   const int rc = read_row(&st,
                           "SELECT COALESCE(valid_to, 0), COALESCE(closed_by_fact_id, 0) FROM "
                           "memory_relations WHERE user_id = ?1 AND id = ?2",
                           user_id, r->row.relation_id);
   if (rc != SQLITE_ROW) {
      return rc == SQLITE_DONE ? MEMORY_CONSOLIDATION_STALE : MEMORY_DB_FAILURE;
   }
   const int64_t valid_to = sqlite3_column_int64(st, 0);
   const int64_t closed_by = sqlite3_column_int64(st, 1);
   /* The fact that closed it deleted since (this batch's own add, undone
    * first) clears closed_by_fact_id: that is still the close it made. */
   const int64_t want = get_i64(r->post, "closed_by");
   const bool closer_gone = closed_by == 0 && want > 0 && !fact_exists_locked(user_id, want);
   const bool stale = valid_to == 0 || valid_to != get_i64(r->post, "valid_to") ||
                      (closed_by != want && !closer_gone);
   sqlite3_finalize(st);
   if (stale) {
      return MEMORY_CONSOLIDATION_STALE;
   }
   bool reopened = false;
   if (memory_db_internal_relation_reopen_locked(user_id, r->row.relation_id, &reopened) !=
       MEMORY_DB_SUCCESS) {
      return MEMORY_DB_FAILURE;
   }
   return reopened ? MEMORY_DB_SUCCESS : MEMORY_CONSOLIDATION_STALE;
}

/* linked: the two entities apart again (memory_db_entity_alias_unlink). */
static int undo_linked(int user_id, const undo_row_t *r, memory_consolidation_undo_effect_t *eff) {
   sqlite3_stmt *st = NULL;
   const int rc = read_row(&st,
                           "SELECT a.unlinked_at IS NULL AND a.link_kind != 'hard' AND "
                           "e.canonical_id = a.target_entity_id FROM memory_entity_aliases a JOIN "
                           "memory_entities e ON e.id = a.source_entity_id WHERE a.user_id = ?1 "
                           "AND a.id = ?2",
                           user_id, r->row.link_id);
   if (rc != SQLITE_ROW) {
      return rc == SQLITE_DONE ? MEMORY_CONSOLIDATION_STALE : MEMORY_DB_FAILURE;
   }
   const bool same = sqlite3_column_int(st, 0) != 0;
   sqlite3_finalize(st);
   if (!same) {
      return MEMORY_CONSOLIDATION_STALE;
   }
   int64_t source = 0;
   const int un = memory_db_internal_alias_unlink_locked(user_id, r->row.link_id,
                                                         "undone by the user", &source);
   if (un == MEMORY_DB_NOT_FOUND) {
      return MEMORY_CONSOLIDATION_STALE;
   }
   eff->entities = eff->entities || un == MEMORY_DB_SUCCESS;
   return un;
}

/* pref_set's pre-image provenance (where the value before was learned) put
 * back: a new value reset the sources to the batch's conversation.  Rows
 * recorded before the pre-image held it keep the batch's.  A source
 * conversation deleted since goes as auth_db_conv.c's delete takes it: its
 * range with it, the row's own source another of its sources, and learned
 * outside any conversation when none is left.  A 0 source id with sources
 * listed is one a forget cleared, not a value that never had one. */
static int pref_provenance_back(int user_id, const char *category, const undo_row_t *r) {
   struct json_object *sources = NULL;
   if (!json_object_object_get_ex(r->pre, "sources", &sources) ||
       !json_object_is_type(sources, json_type_array)) {
      return MEMORY_DB_SUCCESS;
   }
   static const char *const SQL[] = {
      "UPDATE memory_preferences SET source_conversation_id = COALESCE((SELECT id FROM "
      "conversations WHERE id = ?3), (SELECT MAX(j.value) FROM json_each(?7) j WHERE j.value IN "
      "(SELECT id FROM conversations))), source_msg_id_start = (SELECT NULLIF(?4, 0) FROM "
      "conversations WHERE id = ?3), source_msg_id_end = (SELECT NULLIF(?5, 0) FROM "
      "conversations WHERE id = ?3), origin_unsourced = CASE WHEN (?3 = 0 AND "
      "json_array_length(?7) = 0) OR EXISTS (SELECT 1 FROM conversations WHERE id = ?3) OR "
      "EXISTS (SELECT 1 FROM json_each(?7) j WHERE j.value IN (SELECT id FROM conversations)) "
      "THEN ?6 ELSE 1 END WHERE user_id = ?1 AND category = ?2",
      "DELETE FROM memory_preference_sources WHERE preference_id = (SELECT id FROM "
      "memory_preferences WHERE user_id = ?1 AND category = ?2)",
      "INSERT OR IGNORE INTO memory_preference_sources (preference_id, conversation_id) SELECT "
      "p.id, j.value FROM memory_preferences p, json_each(?7) j WHERE p.user_id = ?1 AND "
      "p.category = ?2 AND j.value IN (SELECT id FROM conversations)",
   };
   const char *list = json_object_to_json_string_ext(sources, JSON_C_TO_STRING_PLAIN);
   int rc = SQLITE_DONE;
   for (size_t i = 0; rc == SQLITE_DONE && i < sizeof(SQL) / sizeof(SQL[0]); i++) {
      sqlite3_stmt *st = NULL;
      rc = sqlite3_prepare_v2(s_db.db, SQL[i], -1, &st, NULL);
      if (rc == SQLITE_OK) {
         sqlite3_bind_int(st, 1, user_id);
         sqlite3_bind_text(st, 2, category, -1, SQLITE_TRANSIENT);
         sqlite3_bind_int64(st, 3, get_i64(r->pre, "source_conversation_id"));
         sqlite3_bind_int64(st, 4, get_i64(r->pre, "msg_id_start"));
         sqlite3_bind_int64(st, 5, get_i64(r->pre, "msg_id_end"));
         sqlite3_bind_int(st, 6, get_i64(r->pre, "origin_unsourced") ? 1 : 0);
         sqlite3_bind_text(st, 7, list, -1, SQLITE_TRANSIENT);
         rc = sqlite3_step(st);
      }
      sqlite3_finalize(st);
   }
   return rc == SQLITE_DONE ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

/* pref_set: the preference's value before, and where it was learned, or none.
 * Sessions that were sent it get their USER MEMORY blocks withdrawn. */
static int undo_pref(int user_id, const undo_row_t *r, memory_consolidation_undo_effect_t *eff) {
   const char *category = get_str(r->pre, "category");
   const char *value = get_str(r->post, "value");
   const char *before = get_str(r->pre, "value");
   /* The pre-image's sources are exempt only for a reinforcement: a new value
    * reset the sources to the batch's conversation, so one of them present
    * now taught the new value since. */
   struct json_object *was = NULL;
   const char *sources = before && value && strcmp(before, value) == 0 &&
                                 json_object_object_get_ex(r->pre, "sources", &was)
                             ? json_object_to_json_string_ext(was, JSON_C_TO_STRING_PLAIN)
                             : "[]";
   /* Changed since when its value differs, a conversation other than where
    * it was learned and this batch's taught it since, or it was learned
    * outside a conversation since (a prompt-path extraction, an import). */
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT value, origin_unsourced, EXISTS (SELECT 1 FROM "
                               "memory_preference_sources s WHERE s.preference_id = p.id AND "
                               "s.conversation_id != ?4 AND s.conversation_id NOT IN (SELECT "
                               "value FROM json_each(?3))) FROM memory_preferences p WHERE "
                               "user_id = ?1 AND category = ?2",
                               -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_text(st, 2, category, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 3, sources, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(st, 4, r->conversation_id);
      rc = sqlite3_step(st);
   }
   const char *now = rc == SQLITE_ROW ? (const char *)sqlite3_column_text(st, 0) : NULL;
   const bool stale = rc != SQLITE_ROW || !now || !value || strcmp(now, value) != 0 ||
                      sqlite3_column_int(st, 2) != 0 ||
                      (has(r->post, "origin_unsourced") &&
                       sqlite3_column_int(st, 1) > get_i64(r->post, "origin_unsourced"));
   sqlite3_finalize(st);
   if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
      return MEMORY_DB_FAILURE;
   }
   if (stale) {
      return MEMORY_CONSOLIDATION_STALE;
   }
   st = NULL;
   const bool absent = has(r->pre, "absent");
   rc = sqlite3_prepare_v2(s_db.db,
                           absent ? "DELETE FROM memory_preferences WHERE user_id = ?1 AND "
                                    "category = ?2"
                                  : "UPDATE memory_preferences SET value = ?3, confidence = ?4, "
                                    "outside = ?5, reinforcement_count = COALESCE(?6, "
                                    "reinforcement_count), updated_at = COALESCE(?7, updated_at) "
                                    "WHERE user_id = ?1 AND category = ?2",
                           -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_text(st, 2, category, -1, SQLITE_TRANSIENT);
      if (!absent) {
         sqlite3_bind_text(st, 3, before ? before : "", -1, SQLITE_TRANSIENT);
         sqlite3_bind_double(st, 4, get_double(r->pre, "confidence"));
         sqlite3_bind_int(st, 5, (int)get_i64(r->pre, "outside"));
         /* Pre-images recorded without them leave the two as they are. */
         if (has(r->pre, "reinforcement_count")) {
            sqlite3_bind_int64(st, 6, get_i64(r->pre, "reinforcement_count"));
         }
         if (has(r->pre, "updated_at")) {
            sqlite3_bind_int64(st, 7, get_i64(r->pre, "updated_at"));
         }
      }
      rc = sqlite3_step(st);
   }
   sqlite3_finalize(st);
   int ok = rc == SQLITE_DONE ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
   if (ok == MEMORY_DB_SUCCESS && !absent) {
      ok = pref_provenance_back(user_id, category, r);
   }
   eff->memory_bodies = eff->memory_bodies || ok == MEMORY_DB_SUCCESS;
   return ok;
}

/* The op's status, and its batch undone once nothing of it stays applied. */
static int mark_locked(int user_id, const undo_row_t *r, const char *status) {
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "UPDATE memory_consolidation_ops SET status = ?4, undone_at = ?5 "
                               "WHERE id = ?1 AND batch_id = ?2 AND user_id = ?3",
                               -1, &st, NULL);
   const int64_t now = (int64_t)time(NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, r->row.id);
      sqlite3_bind_int64(st, 2, r->row.batch_id);
      sqlite3_bind_int(st, 3, user_id);
      sqlite3_bind_text(st, 4, status, -1, SQLITE_STATIC);
      sqlite3_bind_int64(st, 5, now);
      rc = sqlite3_step(st);
   }
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE) {
      return MEMORY_DB_FAILURE;
   }
   /* The batch is undone when all of it is: nothing left applied or found
    * changed since. */
   return exec_ok("UPDATE memory_consolidation_batches SET undone_at = strftime('%s', 'now') "
                  "WHERE id = ?2 AND user_id = ?1 AND undone_at IS NULL AND NOT EXISTS (SELECT 1 "
                  "FROM memory_consolidation_ops o WHERE o.batch_id = ?2 AND o.status IN "
                  "('applied', 'stale') AND o.op IN (" UNDOABLE_SQL ")) AND EXISTS (SELECT 1 FROM "
                  "memory_consolidation_ops o WHERE o.batch_id = ?2 AND o.status = 'undone')",
                  user_id, r->row.batch_id, 0);
}

/* An op recorded after ?2 (batch ?3, seq ?9) and still in effect: of another
 * batch, or of the same one after it in the order the batch's undo takes back
 * (seq, then id, last first), so one undone alone sees its batch's later work
 * too.  A closed relation of the batch doesn't count: the batch's undo takes
 * it back last. */
#define LATER_OP                                                                      \
   "l.status IN ('applied', 'stale') AND l.op != 'suggested_forget' AND ((l.id > ?2 " \
   "AND l.batch_id != ?3) OR (l.batch_id = ?3 AND l.op IN (" UNDOABLE_SQL ") AND "    \
   "l.op != 'closed' AND (l.seq > ?9 OR (l.seq = ?9 AND l.id > ?2))))"

/* Whether later work has a hand in what @p r changed: an op recorded after
 * it (LATER_OP) and still in effect (applied, or found changed since and so
 * left as it is), naming the same fact (as its own or its
 * counterpart; for a merge, the fact kept too), relation, link or preference.
 * Undoing @p r would take that batch's work back with it (a restatement in
 * the same conversation, a value set back to what it was).  One indexed probe
 * per kind (idx_mco_fact, idx_mco_other, idx_mco_rel); a link or preference
 * reads the ops after @p r.  SQLITE_ROW with *later, or the error. */
static int later_change_locked(int user_id, const undo_row_t *r, bool *later) {
   *later = false;
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT (?4 > 0 AND EXISTS (SELECT 1 FROM memory_consolidation_ops l WHERE l.user_id = ?1 "
       "AND l.fact_id IN (?4, ?8) AND " LATER_OP ")) OR (?4 > 0 AND EXISTS (SELECT 1 FROM "
       "memory_consolidation_ops l WHERE l.user_id = ?1 AND l.other_fact_id IN (?4, ?8) "
       "AND " LATER_OP ")) OR (?5 > 0 AND EXISTS (SELECT 1 FROM memory_consolidation_ops l WHERE "
       "l.user_id = ?1 AND "
       "l.relation_id = ?5 AND " LATER_OP ")) OR (?6 > 0 AND EXISTS (SELECT 1 FROM "
       "memory_consolidation_ops l WHERE " LATER_OP " AND +l.user_id = ?1 AND l.link_id = ?6)) OR "
       "(?7 IS NOT NULL AND EXISTS (SELECT 1 FROM memory_consolidation_ops l WHERE " LATER_OP
       " AND +l.user_id = ?1 AND l.op = 'pref_set' AND json_extract(l.post_image, '$.category') = "
       "?7))",
       -1, &st, NULL);
   if (rc == SQLITE_OK) {
      const bool pref = is_op(r->row.op, "pref_set");
      const int64_t fact = pref ? 0 : r->row.fact_id;
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_int64(st, 2, r->row.id);
      sqlite3_bind_int64(st, 3, r->row.batch_id);
      sqlite3_bind_int64(st, 4, fact);
      sqlite3_bind_int64(st, 5, r->row.relation_id);
      sqlite3_bind_int64(st, 6, r->row.link_id);
      const char *category = pref ? get_str(r->pre, "category") : NULL;
      if (category) {
         sqlite3_bind_text(st, 7, category, -1, SQLITE_TRANSIENT);
      } else {
         sqlite3_bind_null(st, 7);
      }
      /* A merge's undo also takes back what it gave the fact kept. */
      sqlite3_bind_int64(st, 8,
                         is_op(r->row.op, "merged") && r->row.other_fact_id > 0
                             ? r->row.other_fact_id
                             : fact);
      sqlite3_bind_int(st, 9, r->row.seq);
      rc = sqlite3_step(st);
      *later = rc == SQLITE_ROW && sqlite3_column_int(st, 0) != 0;
   }
   sqlite3_finalize(st);
   return rc;
}

static int undo_kind(int user_id,
                     const undo_row_t *r,
                     const char *stems,
                     memory_consolidation_undo_effect_t *eff) {
   const char *op = r->row.op;
   if (is_op(op, "added")) {
      return undo_added(user_id, r, stems, eff);
   }
   if (is_op(op, "reinforced")) {
      return undo_reinforced(user_id, r, eff);
   }
   if (is_op(op, "merged") || is_op(op, "replaced")) {
      return undo_retired(user_id, r, eff);
   }
   if (is_op(op, "closed")) {
      return undo_closed(user_id, r);
   }
   if (is_op(op, "linked")) {
      return undo_linked(user_id, r, eff);
   }
   return undo_pref(user_id, r, eff);
}

/* Lock held: check, write and mark in one savepoint. */
static int undo_locked(int user_id,
                       int64_t batch_id,
                       int64_t op_id,
                       int64_t fact_id,
                       const char *stems,
                       memory_consolidation_undo_effect_t *eff) {
   undo_row_t r;
   int rc = load_locked(user_id, batch_id, op_id, &r);
   if (rc != MEMORY_DB_SUCCESS) {
      undo_row_free(&r);
      return rc;
   }
   if (is_op(r.row.status, "undone")) {
      undo_row_free(&r);
      return MEMORY_DB_SUCCESS; /* undone before: nothing more to do */
   }
   if (is_op(r.row.status, "stale")) {
      undo_row_free(&r);
      return MEMORY_CONSOLIDATION_STALE; /* found changed before: still nothing done */
   }
   if (!is_op(r.row.status, "applied") || !images_suffice(r.row.op, r.pre, r.post, &r.row) ||
       r.row.fact_id != fact_id) {
      undo_row_free(&r);
      return MEMORY_CONSOLIDATION_NOTHING;
   }
   if (auth_db_savepoint_begin_locked("consolidation_undo") != AUTH_DB_SUCCESS) {
      undo_row_free(&r);
      return MEMORY_DB_FAILURE;
   }
   memory_consolidation_undo_effect_t local = { 0 };
   bool later = false;
   if (later_change_locked(user_id, &r, &later) != SQLITE_ROW) {
      rc = MEMORY_DB_FAILURE;
   } else {
      rc = later ? MEMORY_CONSOLIDATION_STALE : undo_kind(user_id, &r, stems, &local);
   }
   if (rc == MEMORY_CONSOLIDATION_STALE) {
      /* Nothing it wrote stays: only the mark. */
      auth_db_savepoint_end_locked(AUTH_DB_FAILURE, "consolidation_undo");
      memset(&local, 0, sizeof(local));
      if (auth_db_savepoint_begin_locked("consolidation_undo") != AUTH_DB_SUCCESS) {
         undo_row_free(&r);
         return MEMORY_DB_FAILURE;
      }
   }
   int marked = MEMORY_DB_SUCCESS;
   if (rc == MEMORY_DB_SUCCESS || rc == MEMORY_CONSOLIDATION_STALE) {
      marked = mark_locked(user_id, &r, rc == MEMORY_DB_SUCCESS ? "undone" : "stale");
   }
   const bool keep = (rc == MEMORY_DB_SUCCESS || rc == MEMORY_CONSOLIDATION_STALE) &&
                     marked == MEMORY_DB_SUCCESS;
   if (auth_db_savepoint_end_locked(keep ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE,
                                    "consolidation_undo") != AUTH_DB_SUCCESS ||
       !keep) {
      undo_row_free(&r);
      return MEMORY_DB_FAILURE;
   }
   if (rc == MEMORY_DB_SUCCESS) {
      eff->facts = eff->facts || local.facts;
      eff->entities = eff->entities || local.entities;
      eff->memory_bodies = eff->memory_bodies || local.memory_bodies;
   }
   undo_row_free(&r);
   return rc;
}

int memory_db_consolidation_undo_op(int user_id,
                                    int64_t batch_id,
                                    int64_t op_id,
                                    memory_consolidation_undo_effect_t *effect) {
   memory_consolidation_undo_effect_t local = { 0 };
   memory_consolidation_undo_effect_t *eff = effect ? effect : &local;
   if (user_id <= 0 || batch_id <= 0 || op_id <= 0) {
      return MEMORY_DB_NOT_FOUND;
   }
   /* An added fact's keyword-index stems are made with no lock held (the
    * stemmer's mutex is a leaf); fact text never changes, so they still
    * match when the delete runs. */
   undo_row_t peek;
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   int rc = load_locked(user_id, batch_id, op_id, &peek);
   AUTH_DB_UNLOCK();
   const int64_t fact_id = peek.row.fact_id;
   const bool added = rc == MEMORY_DB_SUCCESS && is_op(peek.row.op, "added");
   undo_row_free(&peek);
   if (rc != MEMORY_DB_SUCCESS) {
      return rc;
   }
   char stems[MEMORY_FACT_STEMS_MAX];
   stems[0] = '\0';
   if (added) {
      free(memory_db_internal_fact_stems(fact_id, user_id, stems, sizeof(stems)));
   }

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   rc = undo_locked(user_id, batch_id, op_id, fact_id, stems, eff);
   AUTH_DB_UNLOCK();

   /* The caches hold current facts and entities: drop them off the lock. */
   if (eff->facts) {
      memory_embeddings_invalidate_cache_for_user(user_id);
   }
   if (eff->entities) {
      memory_embeddings_invalidate_entity_cache_for_user(user_id);
   }
   if (rc == MEMORY_DB_SUCCESS || rc == MEMORY_CONSOLIDATION_STALE) {
      OLOG_INFO("memory_db: user %d batch %lld op %lld %s", user_id, (long long)batch_id,
                (long long)op_id, rc == MEMORY_DB_SUCCESS ? "undone" : "stale: not undone");
   }
   return rc;
}

int memory_db_consolidation_undo_ids(int user_id,
                                     int64_t batch_id,
                                     int64_t *ids,
                                     int max,
                                     int *count_out) {
   *count_out = 0;
   memory_consolidation_batch_row_t b;
   int rc = memory_db_consolidation_batch_get(user_id, batch_id, &b);
   if (rc != MEMORY_DB_SUCCESS) {
      return rc;
   }
   /* The last applied first; a closed relation after everything else, so the
    * value that closed it (often this batch's own) is gone before it reopens. */
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   sqlite3_stmt *st = NULL;
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, op, COALESCE(fact_id, 0), COALESCE(relation_id, 0), "
                           "COALESCE(link_id, 0), pre_image, post_image FROM "
                           "memory_consolidation_ops WHERE user_id = ?1 AND batch_id = ?2 AND "
                           "status = 'applied' AND op IN (" UNDOABLE_SQL ") ORDER BY op = "
                           "'closed', seq DESC, id DESC LIMIT ?3",
                           -1, &st, NULL);
   int k = 0;
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_int64(st, 2, batch_id);
      sqlite3_bind_int(st, 3, max);
      while ((rc = sqlite3_step(st)) == SQLITE_ROW) { /* at most max: the LIMIT */
         memory_consolidation_op_row_t o;
         memset(&o, 0, sizeof(o));
         const char *op = (const char *)sqlite3_column_text(st, 1);
         safe_strncpy(o.op, op ? op : "", sizeof(o.op));
         safe_strncpy(o.status, "applied", sizeof(o.status));
         o.fact_id = sqlite3_column_int64(st, 2);
         o.relation_id = sqlite3_column_int64(st, 3);
         o.link_id = sqlite3_column_int64(st, 4);
         o.pre_image = (char *)sqlite3_column_text(st, 5);
         o.post_image = (char *)sqlite3_column_text(st, 6);
         if (memory_consolidation_op_undoable(&o)) {
            ids[k++] = sqlite3_column_int64(st, 0);
         }
      }
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   *count_out = k;
   return rc == SQLITE_DONE ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}
