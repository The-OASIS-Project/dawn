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
 * Memory Database — Entity CRUD operations.
 *
 * Phase 6 split from memory_db.c — entity-side upsert / lookup / search /
 * photo / delete / merge / embedding storage.  The equivalence-class
 * resolver cascade (Stage 1-6 scoring, alias-link, propose-merge) lives
 * in memory_db_alias.c — this file is the lower-level CRUD that the
 * resolver builds on.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_db_internal.h"
#include "memory/memory_embeddings.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Canonical-name normalisation
 * ============================================================================= */

void memory_make_canonical_name(const char *name, char *out, size_t size) {
   if (!name || !out || size == 0)
      return;

   size_t j = 0;
   for (size_t i = 0; name[i] != '\0' && j < size - 1; i++) {
      unsigned char c = (unsigned char)name[i];
      if (c >= 0x80) {
         /* Preserve multibyte UTF-8 as-is */
         out[j++] = (char)c;
      } else {
         out[j++] = (char)tolower(c);
      }
   }

   /* Trim trailing spaces */
   while (j > 0 && out[j - 1] == ' ') {
      j--;
   }

   out[j] = '\0';
}

/* =============================================================================
 * Helper: Populate entity from statement row
 * ============================================================================= */

static void populate_entity_from_row(sqlite3_stmt *stmt, memory_entity_t *entity) {
   entity->id = sqlite3_column_int64(stmt, 0);
   entity->user_id = sqlite3_column_int(stmt, 1);

   const char *name = (const char *)sqlite3_column_text(stmt, 2);
   if (name) {
      safe_strscpy(entity->name, name);
   }

   const char *type = (const char *)sqlite3_column_text(stmt, 3);
   if (type) {
      safe_strscpy(entity->entity_type, type);
   }

   const char *cname = (const char *)sqlite3_column_text(stmt, 4);
   if (cname) {
      safe_strscpy(entity->canonical_name, cname);
   }

   entity->mention_count = sqlite3_column_int(stmt, 5);
   entity->first_seen = (time_t)sqlite3_column_int64(stmt, 6);
   entity->last_seen = (time_t)sqlite3_column_int64(stmt, 7);
}

/* =============================================================================
 * Entity Operations
 * ============================================================================= */

/* Interactive callers (live extraction, contacts UI, memory tool) use this
 * wrapper — timestamps default to time(NULL) which is correct for "now."
 * Reextract and historical-replay paths must call memory_db_entity_upsert_at
 * directly with conv_created_at so first_seen/last_seen are pinned to the
 * original conversation time, not the reextract window. */
int memory_db_entity_upsert(int user_id,
                            const char *name,
                            const char *entity_type,
                            const char *canonical_name,
                            bool *out_created,
                            int64_t *id_out) {
   return memory_db_entity_upsert_at(user_id, name, entity_type, canonical_name, 0, 0, out_created,
                                     id_out);
}

int memory_db_entity_upsert_at(int user_id,
                               const char *name,
                               const char *entity_type,
                               const char *canonical_name,
                               int64_t first_seen_override,
                               int64_t last_seen_override,
                               bool *out_created,
                               int64_t *id_out) {
   if (id_out)
      *id_out = 0;
   if (!name || !entity_type || !canonical_name)
      return MEMORY_DB_FAILURE;

   /* Resolve effective timestamps once.  The `> 0` check treats any non-
    * positive override (0, negatives) as "use now" — 0 is the documented
    * sentinel, negatives are caller-bug defense.  Live-extraction callers
    * pass 0/0 via the wrapper and get the historical behavior unchanged;
    * the reextract path passes conv_created_at to preserve original
    * conversation times. */
   int64_t now_ts = (int64_t)time(NULL);
   int64_t fs_ts = first_seen_override > 0 ? first_seen_override : now_ts;
   int64_t ls_ts = last_seen_override > 0 ? last_seen_override : now_ts;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_entity_upsert);
   sqlite3_bind_int(s_db.stmt_memory_entity_upsert, 1, user_id);
   sqlite3_bind_text(s_db.stmt_memory_entity_upsert, 2, name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(s_db.stmt_memory_entity_upsert, 3, entity_type, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(s_db.stmt_memory_entity_upsert, 4, canonical_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(s_db.stmt_memory_entity_upsert, 5, fs_ts); /* INSERT first_seen */
   sqlite3_bind_int64(s_db.stmt_memory_entity_upsert, 6, ls_ts); /* INSERT last_seen */
   sqlite3_bind_int64(s_db.stmt_memory_entity_upsert, 7, ls_ts); /* UPDATE last_seen */

   int64_t entity_id = 0;
   int rc = sqlite3_step(s_db.stmt_memory_entity_upsert);
   if (rc == SQLITE_ROW) {
      entity_id = sqlite3_column_int64(s_db.stmt_memory_entity_upsert, 0);
      int mention_count = sqlite3_column_int(s_db.stmt_memory_entity_upsert, 1);
      if (out_created) {
         *out_created = (mention_count == 1);
      }
   } else {
      OLOG_ERROR("memory_db: entity upsert failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_reset(s_db.stmt_memory_entity_upsert);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_memory_entity_upsert);
   AUTH_DB_UNLOCK();
   if (id_out)
      *id_out = entity_id;
   return MEMORY_DB_SUCCESS;
}

int memory_db_entity_get_by_name(int user_id,
                                 const char *canonical_name,
                                 memory_entity_t *out_entity) {
   if (!canonical_name || !out_entity)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_entity_get_by_name);
   sqlite3_bind_int(s_db.stmt_memory_entity_get_by_name, 1, user_id);
   sqlite3_bind_text(s_db.stmt_memory_entity_get_by_name, 2, canonical_name, -1, SQLITE_TRANSIENT);

   int rc = sqlite3_step(s_db.stmt_memory_entity_get_by_name);
   int result;
   if (rc == SQLITE_ROW) {
      populate_entity_from_row(s_db.stmt_memory_entity_get_by_name, out_entity);
      result = MEMORY_DB_SUCCESS;
   } else if (rc == SQLITE_DONE) {
      result = MEMORY_DB_NOT_FOUND;
   } else {
      OLOG_ERROR("memory_db: entity_get_by_name failed: %s", sqlite3_errmsg(s_db.db));
      result = MEMORY_DB_FAILURE;
   }

   sqlite3_reset(s_db.stmt_memory_entity_get_by_name);
   AUTH_DB_UNLOCK();
   return result;
}

int memory_db_entity_update_embedding(int64_t entity_id,
                                      int user_id,
                                      const float *embedding,
                                      int dims,
                                      float norm) {
   if (!embedding || dims <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_entity_update_embedding);
   sqlite3_bind_blob(s_db.stmt_memory_entity_update_embedding, 1, embedding,
                     dims * (int)sizeof(float), SQLITE_TRANSIENT);
   sqlite3_bind_double(s_db.stmt_memory_entity_update_embedding, 2, (double)norm);
   sqlite3_bind_int64(s_db.stmt_memory_entity_update_embedding, 3, entity_id);
   sqlite3_bind_int(s_db.stmt_memory_entity_update_embedding, 4, user_id);

   int rc = sqlite3_step(s_db.stmt_memory_entity_update_embedding);
   sqlite3_reset(s_db.stmt_memory_entity_update_embedding);
   AUTH_DB_UNLOCK();

   return (rc == SQLITE_DONE) ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_entity_list(int user_id, memory_entity_t *out, int max, int offset, int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || max <= 0)
      return MEMORY_DB_FAILURE;

   /* Reuse entity_search statement with "%" pattern to match all entities */
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_entity_search);
   sqlite3_bind_int(s_db.stmt_memory_entity_search, 1, user_id);
   sqlite3_bind_text(s_db.stmt_memory_entity_search, 2, "%", -1, SQLITE_STATIC);
   sqlite3_bind_int(s_db.stmt_memory_entity_search, 3, max);
   sqlite3_bind_int(s_db.stmt_memory_entity_search, 4, offset);

   int count = 0;
   while (count < max && sqlite3_step(s_db.stmt_memory_entity_search) == SQLITE_ROW) {
      populate_entity_from_row(s_db.stmt_memory_entity_search, &out[count]);
      count++;
   }

   sqlite3_reset(s_db.stmt_memory_entity_search);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_entity_search(int user_id,
                            const char *keywords,
                            memory_entity_t *out,
                            int max,
                            int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!keywords || !out || max <= 0)
      return MEMORY_DB_FAILURE;

   char pattern[256];
   memory_db_internal_build_like_pattern(keywords, pattern, sizeof(pattern));

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_entity_search);
   sqlite3_bind_int(s_db.stmt_memory_entity_search, 1, user_id);
   sqlite3_bind_text(s_db.stmt_memory_entity_search, 2, pattern, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(s_db.stmt_memory_entity_search, 3, max);
   sqlite3_bind_int(s_db.stmt_memory_entity_search, 4, 0); /* OFFSET — shared stmt has 4 params */

   int count = 0;
   while (count < max && sqlite3_step(s_db.stmt_memory_entity_search) == SQLITE_ROW) {
      populate_entity_from_row(s_db.stmt_memory_entity_search, &out[count]);
      count++;
   }

   sqlite3_reset(s_db.stmt_memory_entity_search);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_entity_set_photo(int user_id, int64_t entity_id, const char *photo_id) {
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_memory_entity_set_photo;
   sqlite3_reset(stmt);

   if (photo_id && photo_id[0]) {
      sqlite3_bind_text(stmt, 1, photo_id, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(stmt, 1);
   }
   sqlite3_bind_int64(stmt, 2, entity_id);
   sqlite3_bind_int(stmt, 3, user_id);

   int rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(stmt);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: entity_set_photo failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_UNLOCK();
   return (changes > 0) ? MEMORY_DB_SUCCESS : MEMORY_DB_NOT_FOUND;
}

int memory_db_entity_get_photo(int user_id,
                               int64_t entity_id,
                               char *out_photo_id,
                               size_t photo_id_size) {
   if (!out_photo_id || photo_id_size == 0) {
      return MEMORY_DB_FAILURE;
   }
   out_photo_id[0] = '\0';

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_memory_entity_get_photo;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, entity_id);
   sqlite3_bind_int(stmt, 2, user_id);

   int rc = sqlite3_step(stmt);
   if (rc == SQLITE_ROW) {
      const char *val = (const char *)sqlite3_column_text(stmt, 0);
      if (val) {
         snprintf(out_photo_id, photo_id_size, "%s", val);
      }
      sqlite3_reset(stmt);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_SUCCESS;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();

   return (rc == SQLITE_DONE) ? MEMORY_DB_NOT_FOUND : MEMORY_DB_FAILURE;
}

int memory_db_entity_delete(int64_t entity_id, int user_id) {
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* Delete relations where this entity is subject or object */
   sqlite3_stmt *rel_stmt = s_db.stmt_memory_relation_delete_by_entity;
   sqlite3_reset(rel_stmt);
   sqlite3_bind_int(rel_stmt, 1, user_id);
   sqlite3_bind_int64(rel_stmt, 2, entity_id);
   sqlite3_bind_int64(rel_stmt, 3, entity_id);
   int rel_rc = sqlite3_step(rel_stmt);
   sqlite3_reset(rel_stmt);
   if (rel_rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: relation delete failed for entity %ld: %s", (long)entity_id,
                 sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   /* Delete the entity itself */
   sqlite3_stmt *stmt = s_db.stmt_memory_entity_delete;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, entity_id);
   sqlite3_bind_int(stmt, 2, user_id);

   int rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return MEMORY_DB_FAILURE;
   }
   return (changes > 0) ? MEMORY_DB_SUCCESS : MEMORY_DB_NOT_FOUND;
}

int memory_db_entity_merge(int user_id, int64_t source_id, int64_t target_id) {
   if (source_id == target_id || source_id <= 0 || target_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* Verify both entities exist and belong to user */
   sqlite3_stmt *chk = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, mention_count, first_seen, last_seen FROM memory_entities "
       "WHERE id = ? AND user_id = ?",
       -1, &chk, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   sqlite3_bind_int64(chk, 1, source_id);
   sqlite3_bind_int(chk, 2, user_id);
   if (sqlite3_step(chk) != SQLITE_ROW) {
      sqlite3_finalize(chk);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_NOT_FOUND;
   }
   int src_mentions = sqlite3_column_int(chk, 1);
   int64_t src_first_seen = sqlite3_column_int64(chk, 2);
   int64_t src_last_seen = sqlite3_column_int64(chk, 3);
   sqlite3_reset(chk);

   sqlite3_bind_int64(chk, 1, target_id);
   sqlite3_bind_int(chk, 2, user_id);
   if (sqlite3_step(chk) != SQLITE_ROW) {
      sqlite3_finalize(chk);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_NOT_FOUND;
   }
   sqlite3_finalize(chk);

   /* Begin transaction */
   sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL);

   /* Helper macro: prepare, bind, step, finalize — ROLLBACK on any failure */
#define MERGE_EXEC(sql, bind_block)                         \
   do {                                                     \
      sqlite3_stmt *_s = NULL;                              \
      rc = sqlite3_prepare_v2(s_db.db, sql, -1, &_s, NULL); \
      if (rc != SQLITE_OK)                                  \
         goto merge_fail;                                   \
      bind_block;                                           \
      rc = sqlite3_step(_s);                                \
      sqlite3_finalize(_s);                                 \
      if (rc != SQLITE_DONE)                                \
         goto merge_fail;                                   \
   } while (0)

   /* Reassign relations: subject */
   MERGE_EXEC("UPDATE memory_relations SET subject_entity_id = ? "
              "WHERE subject_entity_id = ? AND user_id = ?",
              {
                 sqlite3_bind_int64(_s, 1, target_id);
                 sqlite3_bind_int64(_s, 2, source_id);
                 sqlite3_bind_int(_s, 3, user_id);
              });

   /* Reassign relations: object */
   MERGE_EXEC("UPDATE memory_relations SET object_entity_id = ? "
              "WHERE object_entity_id = ? AND user_id = ?",
              {
                 sqlite3_bind_int64(_s, 1, target_id);
                 sqlite3_bind_int64(_s, 2, source_id);
                 sqlite3_bind_int(_s, 3, user_id);
              });

   /* Reassign contacts */
   MERGE_EXEC("UPDATE contacts SET entity_id = ? "
              "WHERE entity_id = ? AND user_id = ?",
              {
                 sqlite3_bind_int64(_s, 1, target_id);
                 sqlite3_bind_int64(_s, 2, source_id);
                 sqlite3_bind_int(_s, 3, user_id);
              });

   /* Delete self-referencing relations (subject == object == target) */
   MERGE_EXEC("DELETE FROM memory_relations WHERE user_id = ? "
              "AND subject_entity_id = ? AND object_entity_id = ?",
              {
                 sqlite3_bind_int(_s, 1, user_id);
                 sqlite3_bind_int64(_s, 2, target_id);
                 sqlite3_bind_int64(_s, 3, target_id);
              });

   /* Deduplicate relations: keep highest confidence per unique relation tuple */
   MERGE_EXEC("DELETE FROM memory_relations WHERE id IN ("
              "  SELECT id FROM ("
              "    SELECT id, ROW_NUMBER() OVER ("
              "      PARTITION BY subject_entity_id, relation, object_entity_id, "
              "        COALESCE(object_value, '') "
              "      ORDER BY confidence DESC, id ASC"
              "    ) AS rn FROM memory_relations "
              "    WHERE user_id = ? AND (subject_entity_id = ? OR object_entity_id = ?)"
              "  ) WHERE rn > 1"
              ")",
              {
                 sqlite3_bind_int(_s, 1, user_id);
                 sqlite3_bind_int64(_s, 2, target_id);
                 sqlite3_bind_int64(_s, 3, target_id);
              });

   /* Deduplicate contacts: keep oldest per (entity_id, field_type, value) */
   MERGE_EXEC("DELETE FROM contacts WHERE id IN ("
              "  SELECT id FROM ("
              "    SELECT id, ROW_NUMBER() OVER ("
              "      PARTITION BY entity_id, field_type, value "
              "      ORDER BY id ASC"
              "    ) AS rn FROM contacts "
              "    WHERE user_id = ? AND entity_id = ?"
              "  ) WHERE rn > 1"
              ")",
              {
                 sqlite3_bind_int(_s, 1, user_id);
                 sqlite3_bind_int64(_s, 2, target_id);
              });

   /* Update target: absorb mention count and time range */
   MERGE_EXEC("UPDATE memory_entities SET "
              "mention_count = mention_count + ?, "
              "first_seen = MIN(first_seen, ?), "
              "last_seen = MAX(COALESCE(last_seen, 0), ?) "
              "WHERE id = ? AND user_id = ?",
              {
                 sqlite3_bind_int(_s, 1, src_mentions);
                 sqlite3_bind_int64(_s, 2, src_first_seen);
                 sqlite3_bind_int64(_s, 3, src_last_seen);
                 sqlite3_bind_int64(_s, 4, target_id);
                 sqlite3_bind_int(_s, 5, user_id);
              });

   /* Delete source entity */
   MERGE_EXEC("DELETE FROM memory_entities WHERE id = ? AND user_id = ?", {
      sqlite3_bind_int64(_s, 1, source_id);
      sqlite3_bind_int(_s, 2, user_id);
   });

#undef MERGE_EXEC

   sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL);
   AUTH_DB_UNLOCK();

   /* Invalidate the entity-embedding cache so subsequent reads don't return
    * phantom entries for the deleted source row.  The invalidator is an
    * atomic dirty-bit flip — no auth_db lock involvement, no self-deadlock
    * risk — so it fires safely after AUTH_DB_UNLOCK() per the contract in
    * docs/ENTITY_MERGE_DESIGN.md §12.  Rollback path skips invalidation
    * because ROLLBACK reverses the in-progress UPDATEs and the cache state
    * is consistent with the pre-merge DB. */
   memory_embeddings_invalidate_entity_cache();

   OLOG_INFO("memory_db: merged entity %lld into %lld for user %d", (long long)source_id,
             (long long)target_id, user_id);
   return MEMORY_DB_SUCCESS;

merge_fail:
   OLOG_ERROR("memory_db: entity merge failed at step rc=%d: %s", rc, sqlite3_errmsg(s_db.db));
   sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
   AUTH_DB_UNLOCK();
   return MEMORY_DB_FAILURE;
}

int memory_db_entity_get_embeddings(int user_id,
                                    bool include_aliases,
                                    int expected_dims,
                                    int64_t *out_ids,
                                    char out_names[][MEMORY_ENTITY_NAME_MAX],
                                    char out_types[][MEMORY_ENTITY_TYPE_MAX],
                                    float *out_embeddings,
                                    float *out_norms,
                                    int max,
                                    int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_ids || !out_names || !out_types || !out_embeddings || !out_norms || max <= 0 ||
       expected_dims <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   /* Bindings: 1 = user_id, 2 = include_aliases (0/1 — drives the
    * "(? = 1 OR canonical_id IS NULL)" filter in the prepared SQL),
    * 3 = LIMIT.  See auth_db_core.c prepare site for the v43 SQL shape. */
   sqlite3_reset(s_db.stmt_memory_entity_get_embeddings);
   sqlite3_bind_int(s_db.stmt_memory_entity_get_embeddings, 1, user_id);
   sqlite3_bind_int(s_db.stmt_memory_entity_get_embeddings, 2, include_aliases ? 1 : 0);
   sqlite3_bind_int(s_db.stmt_memory_entity_get_embeddings, 3, max);

   int count = 0;
   int expected_bytes = expected_dims * (int)sizeof(float);

   while (count < max && sqlite3_step(s_db.stmt_memory_entity_get_embeddings) == SQLITE_ROW) {
      int blob_bytes = sqlite3_column_bytes(s_db.stmt_memory_entity_get_embeddings, 3);
      if (blob_bytes != expected_bytes)
         continue;

      out_ids[count] = sqlite3_column_int64(s_db.stmt_memory_entity_get_embeddings, 0);

      const char *cname = (const char *)sqlite3_column_text(s_db.stmt_memory_entity_get_embeddings,
                                                            1);
      if (cname) {
         safe_strscpy(out_names[count], cname);
      } else {
         out_names[count][0] = '\0';
      }

      const char *etype = (const char *)sqlite3_column_text(s_db.stmt_memory_entity_get_embeddings,
                                                            2);
      if (etype) {
         safe_strscpy(out_types[count], etype);
      } else {
         out_types[count][0] = '\0';
      }

      const void *blob = sqlite3_column_blob(s_db.stmt_memory_entity_get_embeddings, 3);
      if (blob) {
         memcpy(out_embeddings + count * expected_dims, blob, (size_t)expected_bytes);
      }
      out_norms[count] = (float)sqlite3_column_double(s_db.stmt_memory_entity_get_embeddings, 4);
      count++;
   }

   sqlite3_reset(s_db.stmt_memory_entity_get_embeddings);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}
