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
 * Memory Database — Summary CRUD operations.
 *
 * Phase 6 split from memory_db.c — summary-side CRUD, keyword search,
 * v45 semantic search adapter, and embedding storage.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_db_internal.h"
#include "memory/memory_embeddings.h"

/* =============================================================================
 * Helper: Populate summary from statement row
 * ============================================================================= */

static void populate_summary_from_row(sqlite3_stmt *stmt, memory_summary_t *summary) {
   summary->id = sqlite3_column_int64(stmt, 0);
   summary->user_id = sqlite3_column_int(stmt, 1);

   const char *sid = (const char *)sqlite3_column_text(stmt, 2);
   if (sid) {
      strncpy(summary->session_id, sid, MEMORY_SESSION_ID_MAX - 1);
      summary->session_id[MEMORY_SESSION_ID_MAX - 1] = '\0';
   }

   const char *sum = (const char *)sqlite3_column_text(stmt, 3);
   if (sum) {
      strncpy(summary->summary, sum, MEMORY_SUMMARY_MAX - 1);
      summary->summary[MEMORY_SUMMARY_MAX - 1] = '\0';
   }

   const char *topics = (const char *)sqlite3_column_text(stmt, 4);
   if (topics) {
      strncpy(summary->topics, topics, MEMORY_TOPICS_MAX - 1);
      summary->topics[MEMORY_TOPICS_MAX - 1] = '\0';
   }

   const char *sentiment = (const char *)sqlite3_column_text(stmt, 5);
   if (sentiment) {
      strncpy(summary->sentiment, sentiment, MEMORY_SENTIMENT_MAX - 1);
      summary->sentiment[MEMORY_SENTIMENT_MAX - 1] = '\0';
   }

   summary->created_at = (time_t)sqlite3_column_int64(stmt, 6);
   summary->message_count = sqlite3_column_int(stmt, 7);
   summary->duration_seconds = sqlite3_column_int(stmt, 8);
   summary->consolidated = sqlite3_column_int(stmt, 9) != 0;
}

/* =============================================================================
 * Summary Operations
 * ============================================================================= */

int memory_db_summary_create_at(int user_id,
                                const char *session_id,
                                const char *summary,
                                const char *topics,
                                const char *sentiment,
                                int message_count,
                                int duration_seconds,
                                const memory_provenance_t *prov,
                                int64_t created_at_override,
                                int64_t *id_out) {
   if (id_out)
      *id_out = 0;
   if (!session_id || !summary) {
      return MEMORY_DB_FAILURE;
   }

   /* 0 sentinel = "use NOW()".  See memory_db_fact_create_at comment. */
   const int64_t created_at = (created_at_override > 0) ? created_at_override : (int64_t)time(NULL);

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_summary_create;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, session_id, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, summary, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 4, topics ? topics : "", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 5, sentiment ? sentiment : "neutral", -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 6, created_at);
   sqlite3_bind_int(stmt, 7, message_count);
   sqlite3_bind_int(stmt, 8, duration_seconds);
   memory_db_internal_bind_provenance(stmt, 9, prov);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: summary_create failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   int64_t id = sqlite3_last_insert_rowid(s_db.db);
   AUTH_DB_UNLOCK();

   if (id_out)
      *id_out = id;

   OLOG_INFO("memory_db: created summary %ld for user %d", (long)id, user_id);
   return MEMORY_DB_SUCCESS;
}

int memory_db_summary_create(int user_id,
                             const char *session_id,
                             const char *summary,
                             const char *topics,
                             const char *sentiment,
                             int message_count,
                             int duration_seconds,
                             const memory_provenance_t *prov,
                             int64_t *id_out) {
   return memory_db_summary_create_at(user_id, session_id, summary, topics, sentiment,
                                      message_count, duration_seconds, prov,
                                      /*created_at_override*/ 0, id_out);
}

int memory_db_summary_list(int user_id,
                           memory_summary_t *out_summaries,
                           int max_summaries,
                           int offset,
                           int *count_out) {
   return memory_db_summary_list_sorted(user_id, MEMORY_SORT_DEFAULT, out_summaries, max_summaries,
                                        offset, count_out);
}

int memory_db_summary_list_sorted(int user_id,
                                  memory_list_sort_t sort,
                                  memory_summary_t *out_summaries,
                                  int max_summaries,
                                  int offset,
                                  int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_summaries || max_summaries <= 0) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Only CREATED_ASC differs; DEFAULT and CREATED_DESC are both newest-first.
    * Both statements share the positional user / limit / offset binding. */
   sqlite3_stmt *stmt = (sort == MEMORY_SORT_CREATED_ASC)
                            ? s_db.stmt_memory_summary_list_created_asc
                            : s_db.stmt_memory_summary_list;
   if (!stmt) {
      AUTH_DB_UNLOCK();
      OLOG_ERROR("memory_db_summary_list_sorted: statement not prepared (sort=%d)", (int)sort);
      return MEMORY_DB_FAILURE;
   }

   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max_summaries);
   sqlite3_bind_int(stmt, 3, offset);

   int count = 0;
   while (count < max_summaries && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_summary_from_row(stmt, &out_summaries[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_summary_mark_consolidated(int64_t summary_id, int user_id) {
   if (user_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* SQL filters on (id, user_id) — defense-in-depth CWE-639. */
   sqlite3_stmt *stmt = s_db.stmt_memory_summary_mark_consolidated;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, summary_id);
   sqlite3_bind_int(stmt, 2, user_id);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();
   return (rc == SQLITE_DONE) ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_summary_search(int user_id,
                             const char *keywords,
                             memory_summary_t *out_summaries,
                             int max_summaries,
                             int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!keywords || !out_summaries || max_summaries <= 0) {
      return MEMORY_DB_FAILURE;
   }

   char pattern[MEMORY_SUMMARY_MAX];
   memory_db_internal_build_like_pattern(keywords, pattern, sizeof(pattern));

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_summary_search;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 4, max_summaries);

   int count = 0;
   while (count < max_summaries && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_summary_from_row(stmt, &out_summaries[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_summary_delete(int64_t summary_id, int user_id) {
   if (summary_id <= 0 || user_id <= 0) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM memory_summaries WHERE id = ? AND user_id = ?",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: summary_delete prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, summary_id);
   sqlite3_bind_int(stmt, 2, user_id);

   rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return MEMORY_DB_FAILURE;
   }
   return (changes > 0) ? MEMORY_DB_SUCCESS : MEMORY_DB_NOT_FOUND;
}

/* =============================================================================
 * Date-Filtered Summary Queries
 * ============================================================================= */

int memory_db_summary_search_since(int user_id,
                                   const char *keywords,
                                   time_t since_ts,
                                   memory_summary_t *out_summaries,
                                   int max_summaries,
                                   int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!keywords || !out_summaries || max_summaries <= 0) {
      return MEMORY_DB_FAILURE;
   }

   char pattern[MEMORY_SUMMARY_MAX];
   memory_db_internal_build_like_pattern(keywords, pattern, sizeof(pattern));

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_summary_search_since;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 4, (int64_t)since_ts);
   sqlite3_bind_int(stmt, 5, max_summaries);

   int count = 0;
   while (count < max_summaries && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_summary_from_row(stmt, &out_summaries[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_summary_list_window(int user_id,
                                  time_t since_ts,
                                  time_t until_ts,
                                  bool sort_asc,
                                  memory_summary_t *out_summaries,
                                  int max_summaries,
                                  int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_summaries || max_summaries <= 0) {
      return MEMORY_DB_FAILURE;
   }

   int64_t until_resolved = (until_ts > 0) ? (int64_t)until_ts : INT64_MAX;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = sort_asc ? s_db.stmt_memory_summary_list_window_asc
                                 : s_db.stmt_memory_summary_list_window_desc;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, (int64_t)since_ts);
   sqlite3_bind_int64(stmt, 3, until_resolved);
   sqlite3_bind_int(stmt, 4, max_summaries);

   int count = 0;
   while (count < max_summaries && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_summary_from_row(stmt, &out_summaries[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * Summary Embedding Operations (v45 — semantic summary adapter)
 *
 * Norms are recomputed inside the scan rather than persisted alongside the
 * blob.  At the per-user summary scale (hundreds), a one-shot norm pass
 * over each row's float vector is cheaper than the schema churn an extra
 * column would cost, and keeps memory_db_summary_update_embedding
 * symmetric with how the recompute worker writes (vector-only).
 * ============================================================================= */

int memory_db_summary_update_embedding(int user_id,
                                       int64_t summary_id,
                                       const float *embedding,
                                       int dims) {
   if (!embedding || dims <= 0 || user_id <= 0 || summary_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_summary_update_embedding);
   sqlite3_bind_blob(s_db.stmt_memory_summary_update_embedding, 1, embedding,
                     dims * (int)sizeof(float), SQLITE_TRANSIENT);
   sqlite3_bind_int64(s_db.stmt_memory_summary_update_embedding, 2, summary_id);
   sqlite3_bind_int(s_db.stmt_memory_summary_update_embedding, 3, user_id);

   int rc = sqlite3_step(s_db.stmt_memory_summary_update_embedding);
   sqlite3_reset(s_db.stmt_memory_summary_update_embedding);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: summary_update_embedding failed for id %ld: %s", (long)summary_id,
                 sqlite3_errmsg(s_db.db));
      return MEMORY_DB_FAILURE;
   }
   return MEMORY_DB_SUCCESS;
}

/* Compile-time top-K cap.  Heap entries hold only {id, score} (12 bytes),
 * so the entire ranking buffer is ~192 bytes regardless of K — safe on
 * 256 KB pthread stacks.  Full memory_summary_t rows (~3 KB each) are
 * fetched in a second pass for the K survivors only, instead of being
 * copied into a heap slot at every admission.  This both shrinks the
 * stack footprint (~154 KB → 192 B) and skips the strncpy storm under
 * the auth_db lock for non-survivor rows. */
#define MEMORY_SUMMARY_SEMANTIC_TOPK_CAP 16

typedef struct {
   int64_t id;
   float score;
} summary_id_score_t;

int memory_db_summary_search_semantic(int user_id,
                                      const float *query_vec,
                                      int query_dims,
                                      time_t since_ts,
                                      int max_summaries,
                                      int max_scan,
                                      memory_summary_t *out_summaries,
                                      float *out_scores,
                                      int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!query_vec || query_dims <= 0 || max_summaries <= 0 || !out_summaries || !out_scores ||
       user_id <= 0)
      return MEMORY_DB_FAILURE;
   if (max_scan <= 0)
      max_scan = MEMORY_SUMMARY_SEMANTIC_SCAN_CAP_DEFAULT;
   if (max_summaries > MEMORY_SUMMARY_SEMANTIC_TOPK_CAP)
      max_summaries = MEMORY_SUMMARY_SEMANTIC_TOPK_CAP;

   float q_norm = memory_embeddings_l2_norm(query_vec, query_dims);
   if (q_norm <= 0.0f) {
      /* Zero-norm query → nothing to compare against. */
      return MEMORY_DB_SUCCESS;
   }

   const int expected_bytes = query_dims * (int)sizeof(float);

   /* Top-K ranking buffer — id + score only.  Min-heap semantics via
    * linear scan to find evictee (K ≤ 16, sub-microsecond).  Sorted to
    * descending order after the scan completes. */
   summary_id_score_t heap[MEMORY_SUMMARY_SEMANTIC_TOPK_CAP];
   int heap_n = 0;

   AUTH_DB_LOCK_OR_FAIL();

   /* PASS 1 — cosine-rank scan.  The scan SQL was reduced to (id, embedding)
    * only, so SQLite doesn't materialise the long summary / topics text for
    * every row when we're just doing cosine.  Full rows are fetched in
    * pass 2 for survivors only. */
   sqlite3_reset(s_db.stmt_memory_summary_scan_embeddings);
   sqlite3_bind_int(s_db.stmt_memory_summary_scan_embeddings, 1, user_id);
   sqlite3_bind_int64(s_db.stmt_memory_summary_scan_embeddings, 2, (sqlite3_int64)since_ts);
   sqlite3_bind_int(s_db.stmt_memory_summary_scan_embeddings, 3, max_scan);

   int rows_seen = 0;
   while (rows_seen < max_scan &&
          sqlite3_step(s_db.stmt_memory_summary_scan_embeddings) == SQLITE_ROW) {
      rows_seen++;

      int blob_bytes = sqlite3_column_bytes(s_db.stmt_memory_summary_scan_embeddings, 1);
      if (blob_bytes != expected_bytes) {
         /* Dimension mismatch — recompute worker will catch up. */
         continue;
      }
      const void *blob = sqlite3_column_blob(s_db.stmt_memory_summary_scan_embeddings, 1);
      if (!blob)
         continue;

      const float *row_vec = (const float *)blob;
      float row_norm = memory_embeddings_l2_norm(row_vec, query_dims);
      float score = memory_embeddings_cosine_with_norms(query_vec, row_vec, query_dims, q_norm,
                                                        row_norm);

      int min_idx = 0;
      if (heap_n >= max_summaries) {
         for (int i = 1; i < heap_n; i++) {
            if (heap[i].score < heap[min_idx].score)
               min_idx = i;
         }
         if (score <= heap[min_idx].score)
            continue;
      } else {
         min_idx = heap_n;
         heap_n++;
      }

      heap[min_idx].id = sqlite3_column_int64(s_db.stmt_memory_summary_scan_embeddings, 0);
      heap[min_idx].score = score;
   }

   sqlite3_reset(s_db.stmt_memory_summary_scan_embeddings);

   if (heap_n <= 0) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_SUCCESS;
   }

   /* Sort survivors by descending score before the row-fetch pass so the
    * out_summaries[] array lands in the order the caller expects. */
   for (int i = 1; i < heap_n; i++) {
      summary_id_score_t tmp = heap[i];
      int j = i - 1;
      while (j >= 0 && heap[j].score < tmp.score) {
         heap[j + 1] = heap[j];
         j--;
      }
      heap[j + 1] = tmp;
   }

   /* PASS 2 — fetch the full row for each survivor.  Inline-prepared
    * one-shot statement keeps the change scoped to this function; per-call
    * prepare cost (~10 μs) is negligible against the cosine scan that
    * already ran.  The user_id bind is defense-in-depth — the scan already
    * filtered to user_id, but this guards against a re-bound id mismatch.
    *
    * If a row was deleted between pass 1 and pass 2 (we hold the auth_db
    * lock across both, so this can't happen in practice — but the code is
    * tolerant), we simply produce fewer survivors than the heap held. */
   sqlite3_stmt *fetch = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT id, user_id, session_id, summary, topics, sentiment, "
                               "       created_at, message_count, duration_seconds, consolidated "
                               "FROM memory_summaries WHERE id = ? AND user_id = ?",
                               -1, &fetch, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db: summary_search_semantic prepare fetch failed: %s",
                 sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   int produced = 0;
   for (int i = 0; i < heap_n; i++) {
      sqlite3_reset(fetch);
      sqlite3_bind_int64(fetch, 1, heap[i].id);
      sqlite3_bind_int(fetch, 2, user_id);

      if (sqlite3_step(fetch) != SQLITE_ROW) {
         /* Row vanished between passes — skip and continue. */
         continue;
      }

      memory_summary_t *s = &out_summaries[produced];
      memset(s, 0, sizeof(*s));
      s->id = sqlite3_column_int64(fetch, 0);
      s->user_id = sqlite3_column_int(fetch, 1);
      const unsigned char *sid = sqlite3_column_text(fetch, 2);
      if (sid)
         strncpy(s->session_id, (const char *)sid, sizeof(s->session_id) - 1);
      const unsigned char *txt = sqlite3_column_text(fetch, 3);
      if (txt)
         strncpy(s->summary, (const char *)txt, sizeof(s->summary) - 1);
      const unsigned char *topics = sqlite3_column_text(fetch, 4);
      if (topics)
         strncpy(s->topics, (const char *)topics, sizeof(s->topics) - 1);
      const unsigned char *sent = sqlite3_column_text(fetch, 5);
      if (sent)
         strncpy(s->sentiment, (const char *)sent, sizeof(s->sentiment) - 1);
      s->created_at = (time_t)sqlite3_column_int64(fetch, 6);
      s->message_count = sqlite3_column_int(fetch, 7);
      s->duration_seconds = sqlite3_column_int(fetch, 8);
      s->consolidated = (sqlite3_column_int(fetch, 9) != 0);

      out_scores[produced] = heap[i].score;
      produced++;
   }

   sqlite3_finalize(fetch);
   AUTH_DB_UNLOCK();

   if (count_out)
      *count_out = produced;
   return MEMORY_DB_SUCCESS;
}

int memory_db_summary_list_without_embedding(int user_id,
                                             int expected_dims,
                                             int64_t *out_ids,
                                             char (*out_texts)[MEMORY_SUMMARY_MAX],
                                             int max_count,
                                             int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_ids || !out_texts || max_count <= 0 || expected_dims <= 0 || user_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_summary_list_without_embedding);
   sqlite3_bind_int(s_db.stmt_memory_summary_list_without_embedding, 1, user_id);
   sqlite3_bind_int(s_db.stmt_memory_summary_list_without_embedding, 2, expected_dims);
   sqlite3_bind_int(s_db.stmt_memory_summary_list_without_embedding, 3, max_count);

   int n = 0;
   while (n < max_count &&
          sqlite3_step(s_db.stmt_memory_summary_list_without_embedding) == SQLITE_ROW) {
      out_ids[n] = sqlite3_column_int64(s_db.stmt_memory_summary_list_without_embedding, 0);
      const unsigned char *txt = sqlite3_column_text(
          s_db.stmt_memory_summary_list_without_embedding, 1);
      if (txt) {
         strncpy(out_texts[n], (const char *)txt, MEMORY_SUMMARY_MAX - 1);
         out_texts[n][MEMORY_SUMMARY_MAX - 1] = '\0';
      } else {
         out_texts[n][0] = '\0';
      }
      n++;
   }

   sqlite3_reset(s_db.stmt_memory_summary_list_without_embedding);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = n;
   return MEMORY_DB_SUCCESS;
}
