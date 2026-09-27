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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Memory Database — core / shared helpers + per-user utility ops.
 *
 * Phase 6 source split: per-noun CRUD lives in sibling files —
 *   memory_db_facts.c       (24 fact functions + FTS5 maintenance)
 *   memory_db_summaries.c   (12 summary functions incl. semantic search)
 *   memory_db_prefs.c       (5 preference functions)
 *   memory_db_entities.c    (11 entity functions + memory_make_canonical_name)
 *   memory_db_relations.c   (9 relation functions + EXCLUSIVE_RELATIONS /
 *                            CONTRADICTORY_PAIRS / memory_db_relation_supersede)
 *   memory_db_alias.c       (Phase 1/2 equivalence-class resolver cascade)
 *   memory_db_provenance.c  (Phase B provenance reads)
 *   memory_db_admin.c       (recategorize / reextract / etc. workers)
 *
 * This TU keeps:
 *   - cross-domain helpers (bind_provenance / build_like_pattern /
 *     fk_row_exists) exposed via memory_db_internal.h
 *   - user-wide cleanup (memory_db_delete_user_memories)
 *   - aggregate stats (memory_db_get_stats / memory_db_get_all_user_ids)
 *   - extraction tracking (last_extracted_* / undo_extraction_attempt)
 *   - decay + maintenance worker entry points
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "memory/memory_db.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "logging.h"
#include "memory/memory_db_internal.h"
#include "memory/memory_embeddings.h"

/* =============================================================================
 * Cross-domain helpers exposed to sibling memory_db_*.c TUs.
 *
 * These were `static` in the pre-split monolithic memory_db.c.  Promoted to
 * non-static + given a `memory_db_internal_` prefix so the new per-noun TUs
 * can share one canonical implementation per helper.  NOT a public API —
 * see memory_db_internal.h.
 * ============================================================================= */

/* Lightweight FK existence check.  Returns 1 if a row with the given id exists
 * in the specified table, 0 otherwise, or -1 if the prepare itself failed.
 * The -1 is intentionally not SUCCESS/FAILURE — this is a diagnostic-only
 * helper whose only consumer is the relation_supersede FK probe block, which
 * passes the result straight into a log format string for operator inspection.
 * Used by error-path diagnostics (relation_supersede) to pinpoint which FK
 * actually fired on xrc=787 SQLITE_CONSTRAINT_FOREIGNKEY — the bare error
 * message names no column.
 *
 * Ad-hoc prepare/finalize because this is invoked only on the error path; the
 * extra parser/planner cost is irrelevant compared to surfacing the cause. */
int memory_db_internal_fk_row_exists(const char *table, int64_t id, int user_id_scope) {
   if (!table || id <= 0) {
      return 0;
   }
   /* When user_id_scope > 0, append " AND user_id = ?" so a foreign-user row
    * doesn't read back as exists (CWE-209 close).  The users table itself
    * has no user_id column — callers pass scope=0 there. */
   char sql[128];
   if (user_id_scope > 0) {
      snprintf(sql, sizeof(sql), "SELECT 1 FROM %s WHERE id = ? AND user_id = ?", table);
   } else {
      snprintf(sql, sizeof(sql), "SELECT 1 FROM %s WHERE id = ?", table);
   }
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      return -1; /* prepare failed — caller logs "-1=check_failed" */
   }
   sqlite3_bind_int64(stmt, 1, id);
   if (user_id_scope > 0) {
      sqlite3_bind_int(stmt, 2, user_id_scope);
   }
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_ROW) ? 1 : 0;
}

/* Bind provenance columns to sequential params at positions (base), (base+1), (base+2).
 * Emits SQLITE_NULL for all three when prov is NULL or prov->conv_id <= 0. */
void memory_db_internal_bind_provenance(sqlite3_stmt *stmt,
                                        int base,
                                        const memory_provenance_t *prov) {
   if (prov && prov->conv_id > 0) {
      sqlite3_bind_int64(stmt, base, prov->conv_id);
      sqlite3_bind_int64(stmt, base + 1, prov->msg_id_start);
      sqlite3_bind_int64(stmt, base + 2, prov->msg_id_end);
   } else {
      sqlite3_bind_null(stmt, base);
      sqlite3_bind_null(stmt, base + 1);
      sqlite3_bind_null(stmt, base + 2);
   }
}

/* Build LIKE pattern with wildcards.  Escapes the LIKE metacharacters %, _, \\
 * with backslash; callers SHOULD pair this with `ESCAPE '\\'` on the SQL side
 * (most existing call sites omit ESCAPE because their queries don't expect a
 * literal %/_ in user input — defense-in-depth either way). */
void memory_db_internal_source_add_locked(memory_source_kind_t kind,
                                          int64_t row_id,
                                          int64_t conv_id) {
   sqlite3_stmt *stmt = kind == MEMORY_SOURCE_FACT       ? s_db.stmt_memory_fact_source_add
                        : kind == MEMORY_SOURCE_RELATION ? s_db.stmt_memory_relation_source_add
                                                         : s_db.stmt_memory_pref_source_add;
   if (row_id <= 0 || conv_id <= 0) {
      return;
   }
   if (!stmt) {
      /* The v89 tables are missing (a failed migration): a forget would then
       * see only the row's latest source.  Say so once. */
      static bool warned;
      if (!warned) {
         warned = true;
         OLOG_WARNING("memory_db: memory source tables unavailable; forgetting a conversation "
                      "can only see each memory's latest source");
      }
      return;
   }
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, row_id);
   sqlite3_bind_int64(stmt, 2, conv_id);
   if (sqlite3_step(stmt) != SQLITE_DONE) {
      OLOG_WARNING("memory_db: recording source of memory row %lld (kind %d) failed: %s",
                   (long long)row_id, (int)kind, sqlite3_errmsg(s_db.db));
   }
   sqlite3_reset(stmt);
}

bool memory_db_internal_ids_json(const int64_t *ids, int n, char *out, size_t size) {
   if (!out || size < 3 || n < 0 || (n > 0 && !ids)) {
      return false;
   }
   size_t len = 0;
   out[len++] = '[';
   for (int i = 0; i < n; i++) {
      int w = snprintf(out + len, size - len, "%s%lld", i ? "," : "", (long long)ids[i]);
      if (w < 0 || (size_t)w >= size - len) {
         return false;
      }
      len += (size_t)w;
   }
   if (len + 2 > size) {
      return false;
   }
   out[len++] = ']';
   out[len] = '\0';
   return true;
}

void memory_db_internal_build_like_pattern(const char *keywords,
                                           char *out_pattern,
                                           size_t max_len) {
   if (!keywords || !out_pattern || max_len < 4) {
      if (out_pattern && max_len > 0) {
         out_pattern[0] = '\0';
      }
      return;
   }

   /* Build pattern with escaped LIKE metacharacters (%, _, \) */
   size_t out_idx = 0;
   out_pattern[out_idx++] = '%';

   for (size_t i = 0; keywords[i] != '\0' && out_idx < max_len - 3; i++) {
      char c = keywords[i];
      /* Escape LIKE metacharacters with backslash */
      if (c == '%' || c == '_' || c == '\\') {
         if (out_idx < max_len - 4) {
            out_pattern[out_idx++] = '\\';
            out_pattern[out_idx++] = c;
         }
      } else {
         out_pattern[out_idx++] = c;
      }
   }

   out_pattern[out_idx++] = '%';
   out_pattern[out_idx] = '\0';
}

/* =============================================================================
 * Utility Operations
 * ============================================================================= */

int memory_db_delete_user_memories(int user_id) {
   if (user_id <= 0) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   int rc;

   /* Delete in FK-safe order: relations -> entities -> facts -> prefs -> summaries */

   /* Delete relations first (FK references entities) */
   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM memory_relations WHERE user_id = ?", -1, &stmt,
                           NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: delete_user_memories (relations) prepare failed: %s",
                 sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: delete_user_memories (relations) failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   /* Delete entities */
   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM memory_entities WHERE user_id = ?", -1, &stmt,
                           NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: delete_user_memories (entities) prepare failed: %s",
                 sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: delete_user_memories (entities) failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   /* Delete facts */
   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM memory_facts WHERE user_id = ?", -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: delete_user_memories (facts) prepare failed: %s",
                 sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: delete_user_memories (facts) failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   /* Delete preferences */
   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM memory_preferences WHERE user_id = ?", -1, &stmt,
                           NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: delete_user_memories (prefs) prepare failed: %s",
                 sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: delete_user_memories (prefs) failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   /* Delete summaries */
   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM memory_summaries WHERE user_id = ?", -1, &stmt,
                           NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: delete_user_memories (summaries) prepare failed: %s",
                 sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: delete_user_memories (summaries) failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_UNLOCK();

   /* Drop in-memory embedding caches.  Without this, hybrid_search keeps
    * returning the deleted facts/entities until another invalidation fires
    * (e.g., a fresh embed_and_store).  Acquired *after* releasing the auth_db
    * lock — embedding_cache mutex is a leaf lock per ARCHITECTURE.md. */
   memory_embeddings_invalidate_all();

   OLOG_INFO("memory_db: deleted all memories for user %d", user_id);
   return MEMORY_DB_SUCCESS;
}

int memory_db_get_stats(int user_id, memory_stats_t *out_stats) {
   if (!out_stats) {
      return MEMORY_DB_FAILURE;
   }

   memset(out_stats, 0, sizeof(memory_stats_t));

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* Combined stats query — single round-trip for all counts + date range */
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT "
       "(SELECT COUNT(*) FROM memory_facts WHERE user_id = ?1 AND superseded_by IS NULL), "
       "(SELECT MIN(created_at) FROM memory_facts WHERE user_id = ?1 AND superseded_by IS NULL), "
       "(SELECT MAX(created_at) FROM memory_facts WHERE user_id = ?1 AND superseded_by IS NULL), "
       "(SELECT COUNT(*) FROM memory_preferences WHERE user_id = ?1), "
       "(SELECT COUNT(*) FROM memory_summaries WHERE user_id = ?1), "
       "(SELECT COUNT(*) FROM memory_entities WHERE user_id = ?1)",
       -1, &stmt, NULL);

   if (rc == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, user_id);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         out_stats->fact_count = sqlite3_column_int(stmt, 0);
         out_stats->oldest_fact = (time_t)sqlite3_column_int64(stmt, 1);
         out_stats->newest_fact = (time_t)sqlite3_column_int64(stmt, 2);
         out_stats->pref_count = sqlite3_column_int(stmt, 3);
         out_stats->summary_count = sqlite3_column_int(stmt, 4);
         out_stats->entity_count = sqlite3_column_int(stmt, 5);
      }
      sqlite3_finalize(stmt);
   }

   AUTH_DB_UNLOCK();
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * Extraction Tracking
 * ============================================================================= */

int memory_db_get_last_extracted(int64_t conversation_id, int *count_out) {
   if (count_out)
      *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_conv_get_last_extracted;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, conversation_id);

   int result = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      result = sqlite3_column_int(stmt, 0);
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = result;
   return MEMORY_DB_SUCCESS;
}

int memory_db_set_last_extracted(int64_t conversation_id, int message_count, int64_t last_msg_id) {
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_conv_set_last_extracted;
   sqlite3_reset(stmt);
   /* Params: message_count, last_msg_id (x2 for CASE), conversation_id */
   sqlite3_bind_int(stmt, 1, message_count);
   sqlite3_bind_int64(stmt, 2, last_msg_id);
   sqlite3_bind_int64(stmt, 3, last_msg_id);
   sqlite3_bind_int64(stmt, 4, conversation_id);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();
   return (rc == SQLITE_DONE) ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_undo_extraction_attempt(int64_t conversation_id) {
   if (conversation_id <= 0) {
      return MEMORY_DB_FAILURE;
   }
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* Roll back the attempt stamp written by record_attempt in
    * memory_recovery.c.  Decrement to MAX(0, attempts - 1) so a transient
    * failure on the very first attempt drops the counter back to 0; reset
    * last_attempt_at to 0 so the recovery scan's
    * `extraction_last_attempt_at < updated_at` re-eligibility rule treats
    * the conv as never-attempted (it isn't dependent on `updated_at`
    * advancing). */
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE conversations "
                          "SET extraction_attempts = MAX(0, extraction_attempts - 1), "
                          "    extraction_last_attempt_at = 0 "
                          "WHERE id = ?",
                          -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int64(stmt, 1, conversation_id);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);

   AUTH_DB_UNLOCK();
   return (rc == SQLITE_DONE) ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

/* memory_db_facts_get_sources moved to memory_db_provenance.c (Phase B). */

int memory_db_get_last_extracted_msg_id(int64_t conversation_id, int64_t *msg_id_out) {
   if (!msg_id_out)
      return MEMORY_DB_FAILURE;
   *msg_id_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT last_extracted_msg_id FROM conversations WHERE id = ?", -1,
                               &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int64(stmt, 1, conversation_id);
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      *msg_id_out = sqlite3_column_int64(stmt, 0);
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return MEMORY_DB_SUCCESS;
}

/* memory_db_fact_get_source moved to memory_db_provenance.c (Phase B). */

/* =============================================================================
 * Decay and Maintenance Operations (Phase 5)
 *
 * These use ad-hoc prepared statements (acceptable for once-daily execution).
 * ============================================================================= */

int memory_db_apply_fact_decay(int user_id,
                               float inferred_rate,
                               float explicit_rate,
                               float inferred_floor,
                               float explicit_floor,
                               int *count_out) {
   if (count_out)
      *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE memory_facts SET confidence ="
       "  CASE WHEN source = 'explicit'"
       "    THEN MAX(?, confidence * powf(?, "
       "         (CAST(strftime('%s','now') AS REAL) - last_accessed) / 604800.0))"
       "    ELSE MAX(?, confidence * powf(?, "
       "         (CAST(strftime('%s','now') AS REAL) - last_accessed) / 604800.0))"
       "  END "
       /* note_doc_id IS NULL: bridge glosses (v61) must not decay — a rarely-read
        * note pointer would otherwise sink to the floor and rank poorly. */
       "WHERE user_id = ? AND superseded_by IS NULL AND last_accessed IS NOT NULL "
       "AND note_doc_id IS NULL",
       -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: apply_fact_decay prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_double(stmt, 1, (double)explicit_floor);
   sqlite3_bind_double(stmt, 2, (double)explicit_rate);
   sqlite3_bind_double(stmt, 3, (double)inferred_floor);
   sqlite3_bind_double(stmt, 4, (double)inferred_rate);
   sqlite3_bind_int(stmt, 5, user_id);

   rc = sqlite3_step(stmt);
   int affected = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: apply_fact_decay failed: %s", sqlite3_errmsg(s_db.db));
      return MEMORY_DB_FAILURE;
   }

   if (count_out)
      *count_out = affected;
   return MEMORY_DB_SUCCESS;
}

int memory_db_apply_pref_decay(int user_id, float pref_rate, float pref_floor, int *count_out) {
   if (count_out)
      *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE memory_preferences SET confidence ="
       "  MAX(?, confidence * powf(?, "
       "      (CAST(strftime('%s','now') AS REAL) - updated_at) / 604800.0))"
       " WHERE user_id = ?",
       -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: apply_pref_decay prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   sqlite3_bind_double(stmt, 1, (double)pref_floor);
   sqlite3_bind_double(stmt, 2, (double)pref_rate);
   sqlite3_bind_int(stmt, 3, user_id);

   rc = sqlite3_step(stmt);
   int affected = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: apply_pref_decay failed: %s", sqlite3_errmsg(s_db.db));
      return MEMORY_DB_FAILURE;
   }

   if (count_out)
      *count_out = affected;
   return MEMORY_DB_SUCCESS;
}

int memory_db_prune_low_confidence(int user_id, float threshold, int *count_out) {
   if (count_out)
      *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   /* Wrap audit log + delete in a transaction for consistency */
   sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL);

   /* Log facts that will be pruned (audit trail for irreversible operation) */
   sqlite3_stmt *log_stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT id, fact_text, confidence, source FROM memory_facts "
                               "WHERE user_id = ? AND confidence < ? AND superseded_by IS NULL",
                               -1, &log_stmt, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(log_stmt, 1, user_id);
      sqlite3_bind_double(log_stmt, 2, (double)threshold);

      while (sqlite3_step(log_stmt) == SQLITE_ROW) {
         OLOG_INFO("memory_decay: pruning fact %ld (%.2f, %s): %.60s",
                   (long)sqlite3_column_int64(log_stmt, 0), sqlite3_column_double(log_stmt, 2),
                   (const char *)sqlite3_column_text(log_stmt, 3),
                   (const char *)sqlite3_column_text(log_stmt, 1));
      }
      sqlite3_finalize(log_stmt);
   }

   /* Delete low-confidence facts */
   sqlite3_stmt *stmt = NULL;
   rc = sqlite3_prepare_v2(
       s_db.db,
       /* note_doc_id IS NULL: never prune memory→note bridge glosses (v61) — they
        * are removed with their note, not by confidence. */
       "DELETE FROM memory_facts WHERE user_id = ? AND confidence < ? AND superseded_by IS NULL "
       "AND note_doc_id IS NULL",
       -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: prune_low_confidence prepare failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_double(stmt, 2, (double)threshold);

   rc = sqlite3_step(stmt);
   int deleted = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: prune_low_confidence failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL);
   AUTH_DB_UNLOCK();

   if (count_out)
      *count_out = deleted;
   return MEMORY_DB_SUCCESS;
}

int memory_db_prune_old_summaries(int user_id, int retention_days, int *count_out) {
   if (count_out)
      *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   time_t cutoff = time(NULL) - ((time_t)retention_days * 24 * 60 * 60);

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "DELETE FROM memory_summaries WHERE user_id = ? AND created_at < ?",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: prune_old_summaries prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, (int64_t)cutoff);

   rc = sqlite3_step(stmt);
   int deleted = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: prune_old_summaries failed: %s", sqlite3_errmsg(s_db.db));
      return MEMORY_DB_FAILURE;
   }

   if (count_out)
      *count_out = deleted;
   if (deleted > 0) {
      OLOG_INFO("memory_db: pruned %d old summaries for user %d", deleted, user_id);
   }
   return MEMORY_DB_SUCCESS;
}

int memory_db_get_all_user_ids(int *out_ids, int max_ids, int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_ids || max_ids <= 0) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT user_id FROM memory_facts "
                               "UNION SELECT user_id FROM memory_preferences",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: get_all_user_ids prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   int count = 0;
   while (count < max_ids && sqlite3_step(stmt) == SQLITE_ROW) {
      out_ids[count++] = sqlite3_column_int(stmt, 0);
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}
