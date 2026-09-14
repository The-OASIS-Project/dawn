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
 * Embedding Recomputation Worker
 *
 * Detects model swaps via system_metadata.embedding_model_id and re-indexes
 * stored embeddings (memory_facts, memory_entities, document_chunks) in the
 * background so cosine similarity remains valid across model changes.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include "memory/memory_embed_recompute.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "core/embedding_engine.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_embeddings.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define RECOMPUTE_TEXT_MAX 512 /* max chars per fact/entity text */
#define RECOMPUTE_BATCH 50     /* rows per batch (overridden by config) */

/* =============================================================================
 * State
 * ============================================================================= */

static pthread_t s_thread;
static atomic_bool s_running;
static atomic_bool s_shutdown;
static atomic_int_least64_t s_done;
static atomic_int_least64_t s_total;

/* =============================================================================
 * system_metadata helpers
 * ============================================================================= */

static int meta_get(const char *key, char *out, size_t out_size) {
   if (!key || !out || out_size == 0)
      return FAILURE;
   out[0] = '\0';

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, "SELECT value FROM system_metadata WHERE key = ?", -1,
                               &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return FAILURE;
   }
   sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      const char *val = (const char *)sqlite3_column_text(stmt, 0);
      if (val)
         safe_strncpy(out, val, out_size);
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return SUCCESS;
}

static int meta_set(const char *key, const char *value) {
   if (!key || !value)
      return FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "INSERT INTO system_metadata (key, value) VALUES (?, ?) "
                               "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return FAILURE;
   }
   sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 2, value, -1, SQLITE_STATIC);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return SUCCESS;
}

/* =============================================================================
 * Per-user embeddings_model_id helpers
 * ============================================================================= */

static int user_needs_recompute(int user_id, bool *needs_out) {
   if (!needs_out)
      return FAILURE;
   *needs_out = false;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, "SELECT embeddings_model_id FROM users WHERE id = ?", -1,
                               &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      const char *stored = (const char *)sqlite3_column_text(stmt, 0);
      if (!stored || strcmp(stored, g_config.memory.model_id) != 0)
         *needs_out = true;
   } else {
      /* User not found — treat as needing recompute to be safe */
      *needs_out = true;
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return SUCCESS;
}

static int user_set_model_id(int user_id) {
   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, "UPDATE users SET embeddings_model_id = ? WHERE id = ?", -1,
                               &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return FAILURE;
   }
   sqlite3_bind_text(stmt, 1, g_config.memory.model_id, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 2, user_id);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return SUCCESS;
}

/* =============================================================================
 * Collect user IDs that need recomputation
 * ============================================================================= */

static int collect_stale_users(int *out_ids, int cap, int *count_out) {
   *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id FROM users WHERE embeddings_model_id IS NULL OR embeddings_model_id != ?", -1,
       &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return FAILURE;
   }
   sqlite3_bind_text(stmt, 1, g_config.memory.model_id, -1, SQLITE_STATIC);

   int n = 0;
   while (n < cap && sqlite3_step(stmt) == SQLITE_ROW) {
      out_ids[n++] = sqlite3_column_int(stmt, 0);
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   *count_out = n;
   if (n == cap)
      OLOG_WARNING("memory_embed_recompute: user list capped at %d; extra users skipped", cap);
   return SUCCESS;
}

/* =============================================================================
 * Fact recomputation pass for one user
 * ============================================================================= */

typedef struct {
   int64_t id;
   char text[RECOMPUTE_TEXT_MAX];
} recompute_row_t;

static int recompute_facts_for_user(int user_id) {
   int batch_size = g_config.memory.recompute_batch_size;
   if (batch_size <= 0)
      batch_size = RECOMPUTE_BATCH;

   recompute_row_t *rows = malloc((size_t)batch_size * sizeof(recompute_row_t));
   if (!rows)
      return FAILURE;

   int64_t cursor_id = 0;
   int total_done = 0;
   int loops = 0;

   while (!atomic_load(&s_shutdown)) {
      int batch_count = 0;
      int64_t batch_max_id = cursor_id;

      pthread_mutex_lock(&s_db.mutex);
      if (!s_db.initialized) {
         pthread_mutex_unlock(&s_db.mutex);
         free(rows);
         return FAILURE;
      }

      sqlite3_stmt *stmt = NULL;
      int rc = sqlite3_prepare_v2(s_db.db,
                                  "SELECT id, fact_text FROM memory_facts "
                                  "WHERE user_id = ? AND superseded_by IS NULL AND id > ? "
                                  "ORDER BY id ASC LIMIT ?",
                                  -1, &stmt, NULL);
      if (rc != SQLITE_OK) {
         pthread_mutex_unlock(&s_db.mutex);
         free(rows);
         return FAILURE;
      }
      sqlite3_bind_int(stmt, 1, user_id);
      sqlite3_bind_int64(stmt, 2, cursor_id);
      sqlite3_bind_int(stmt, 3, batch_size);

      while (batch_count < batch_size && sqlite3_step(stmt) == SQLITE_ROW) {
         int64_t row_id = sqlite3_column_int64(stmt, 0);
         const char *text = (const char *)sqlite3_column_text(stmt, 1);
         if (row_id > batch_max_id)
            batch_max_id = row_id;
         rows[batch_count].id = row_id;
         if (text)
            safe_strscpy(rows[batch_count].text, text);
         else
            rows[batch_count].text[0] = '\0';
         batch_count++;
      }
      cursor_id = batch_max_id;
      sqlite3_finalize(stmt);
      pthread_mutex_unlock(&s_db.mutex);

      if (batch_count == 0)
         break;

      for (int i = 0; i < batch_count && !atomic_load(&s_shutdown); i++) {
         if (rows[i].text[0] == '\0')
            continue;
         if (memory_embeddings_embed_and_store(user_id, rows[i].id, rows[i].text) == SUCCESS) {
            total_done++;
            atomic_fetch_add(&s_done, 1);
         }
         int sleep_ms = g_config.memory.recompute_batch_sleep_ms;
         if (sleep_ms > 0)
            usleep((useconds_t)sleep_ms * 1000u);
      }

      if (batch_count < batch_size)
         break;

      if (++loops > 100000) {
         OLOG_WARNING("memory_embed_recompute: fact loop guard tripped (user %d)", user_id);
         break;
      }
   }

   free(rows);
   OLOG_INFO("memory_embed_recompute: facts done user=%d recomputed=%d", user_id, total_done);
   return SUCCESS;
}

/* =============================================================================
 * Entity recomputation pass for one user
 * ============================================================================= */

static int recompute_entities_for_user(int user_id) {
   int batch_size = g_config.memory.recompute_batch_size;
   if (batch_size <= 0)
      batch_size = RECOMPUTE_BATCH;

   typedef struct {
      int64_t id;
      char name[256];
   } entity_row_t;

   entity_row_t *rows = malloc((size_t)batch_size * sizeof(entity_row_t));
   if (!rows)
      return FAILURE;

   int64_t cursor_id = 0;
   int total_done = 0;
   int loops = 0;

   while (!atomic_load(&s_shutdown)) {
      int batch_count = 0;
      int64_t batch_max_id = cursor_id;

      pthread_mutex_lock(&s_db.mutex);
      if (!s_db.initialized) {
         pthread_mutex_unlock(&s_db.mutex);
         free(rows);
         return FAILURE;
      }

      sqlite3_stmt *stmt = NULL;
      int rc = sqlite3_prepare_v2(s_db.db,
                                  "SELECT id, name FROM memory_entities "
                                  "WHERE user_id = ? AND id > ? "
                                  "ORDER BY id ASC LIMIT ?",
                                  -1, &stmt, NULL);
      if (rc != SQLITE_OK) {
         pthread_mutex_unlock(&s_db.mutex);
         free(rows);
         return FAILURE;
      }
      sqlite3_bind_int(stmt, 1, user_id);
      sqlite3_bind_int64(stmt, 2, cursor_id);
      sqlite3_bind_int(stmt, 3, batch_size);

      while (batch_count < batch_size && sqlite3_step(stmt) == SQLITE_ROW) {
         int64_t row_id = sqlite3_column_int64(stmt, 0);
         const char *name = (const char *)sqlite3_column_text(stmt, 1);
         if (row_id > batch_max_id)
            batch_max_id = row_id;
         rows[batch_count].id = row_id;
         if (name)
            safe_strscpy(rows[batch_count].name, name);
         else
            rows[batch_count].name[0] = '\0';
         batch_count++;
      }
      cursor_id = batch_max_id;
      sqlite3_finalize(stmt);
      pthread_mutex_unlock(&s_db.mutex);

      if (batch_count == 0)
         break;

      for (int i = 0; i < batch_count && !atomic_load(&s_shutdown); i++) {
         if (rows[i].name[0] == '\0')
            continue;
         if (memory_embeddings_embed_and_store_entity(rows[i].id, user_id, rows[i].name) ==
             SUCCESS) {
            total_done++;
            atomic_fetch_add(&s_done, 1);
         }
         int sleep_ms = g_config.memory.recompute_batch_sleep_ms;
         if (sleep_ms > 0)
            usleep((useconds_t)sleep_ms * 1000u);
      }

      if (batch_count < batch_size)
         break;

      if (++loops > 100000) {
         OLOG_WARNING("memory_embed_recompute: entity loop guard tripped (user %d)", user_id);
         break;
      }
   }

   free(rows);
   OLOG_INFO("memory_embed_recompute: entities done user=%d recomputed=%d", user_id, total_done);
   return SUCCESS;
}

/* =============================================================================
 * Summary recomputation pass for one user (v45)
 *
 * Same shape as recompute_facts_for_user, but scans memory_summaries and
 * uses memory_embeddings_embed_and_store_summary.  Embedding column is
 * nullable so a freshly-added column on an existing DB starts NULL for
 * every row — this pass writes them all on the first model swap (or first
 * boot after v45 ships).
 * ============================================================================= */

static int recompute_summaries_for_user(int user_id) {
   int batch_size = g_config.memory.recompute_batch_size;
   if (batch_size <= 0)
      batch_size = RECOMPUTE_BATCH;

   recompute_row_t *rows = malloc((size_t)batch_size * sizeof(recompute_row_t));
   if (!rows)
      return FAILURE;

   int64_t cursor_id = 0;
   int total_done = 0;
   int loops = 0;

   while (!atomic_load(&s_shutdown)) {
      int batch_count = 0;
      int64_t batch_max_id = cursor_id;

      pthread_mutex_lock(&s_db.mutex);
      if (!s_db.initialized) {
         pthread_mutex_unlock(&s_db.mutex);
         free(rows);
         return FAILURE;
      }

      sqlite3_stmt *stmt = NULL;
      int rc = sqlite3_prepare_v2(s_db.db,
                                  "SELECT id, summary FROM memory_summaries "
                                  "WHERE user_id = ? AND id > ? "
                                  "ORDER BY id ASC LIMIT ?",
                                  -1, &stmt, NULL);
      if (rc != SQLITE_OK) {
         pthread_mutex_unlock(&s_db.mutex);
         free(rows);
         return FAILURE;
      }
      sqlite3_bind_int(stmt, 1, user_id);
      sqlite3_bind_int64(stmt, 2, cursor_id);
      sqlite3_bind_int(stmt, 3, batch_size);

      while (batch_count < batch_size && sqlite3_step(stmt) == SQLITE_ROW) {
         int64_t row_id = sqlite3_column_int64(stmt, 0);
         const char *text = (const char *)sqlite3_column_text(stmt, 1);
         if (row_id > batch_max_id)
            batch_max_id = row_id;
         rows[batch_count].id = row_id;
         if (text)
            safe_strscpy(rows[batch_count].text, text);
         else
            rows[batch_count].text[0] = '\0';
         batch_count++;
      }
      cursor_id = batch_max_id;
      sqlite3_finalize(stmt);
      pthread_mutex_unlock(&s_db.mutex);

      if (batch_count == 0)
         break;

      for (int i = 0; i < batch_count && !atomic_load(&s_shutdown); i++) {
         if (rows[i].text[0] == '\0')
            continue;
         if (memory_embeddings_embed_and_store_summary(user_id, rows[i].id, rows[i].text) ==
             SUCCESS) {
            total_done++;
            atomic_fetch_add(&s_done, 1);
         }
         int sleep_ms = g_config.memory.recompute_batch_sleep_ms;
         if (sleep_ms > 0)
            usleep((useconds_t)sleep_ms * 1000u);
      }

      if (batch_count < batch_size)
         break;

      if (++loops > 100000) {
         OLOG_WARNING("memory_embed_recompute: summary loop guard tripped (user %d)", user_id);
         break;
      }
   }

   free(rows);
   OLOG_INFO("memory_embed_recompute: summaries done user=%d recomputed=%d", user_id, total_done);
   return SUCCESS;
}

/* =============================================================================
 * Document chunk recomputation pass (deferred lower-priority pass)
 * ============================================================================= */

static int recompute_document_chunks(void) {
   int batch_size = g_config.memory.recompute_batch_size;
   if (batch_size <= 0)
      batch_size = RECOMPUTE_BATCH;

   recompute_row_t *rows = malloc((size_t)batch_size * sizeof(recompute_row_t));
   if (!rows)
      return FAILURE;

   int64_t cursor_id = 0;
   int total_done = 0;
   int loops = 0;

   OLOG_INFO("memory_embed_recompute: starting document_chunks pass");

   while (!atomic_load(&s_shutdown)) {
      int batch_count = 0;
      int64_t batch_max_id = cursor_id;

      pthread_mutex_lock(&s_db.mutex);
      if (!s_db.initialized) {
         pthread_mutex_unlock(&s_db.mutex);
         free(rows);
         return FAILURE;
      }

      sqlite3_stmt *stmt = NULL;
      int rc = sqlite3_prepare_v2(s_db.db,
                                  "SELECT id, text FROM document_chunks "
                                  "WHERE id > ? ORDER BY id ASC LIMIT ?",
                                  -1, &stmt, NULL);
      if (rc != SQLITE_OK) {
         pthread_mutex_unlock(&s_db.mutex);
         free(rows);
         return FAILURE;
      }
      sqlite3_bind_int64(stmt, 1, cursor_id);
      sqlite3_bind_int(stmt, 2, batch_size);

      while (batch_count < batch_size && sqlite3_step(stmt) == SQLITE_ROW) {
         int64_t row_id = sqlite3_column_int64(stmt, 0);
         const char *text = (const char *)sqlite3_column_text(stmt, 1);
         if (row_id > batch_max_id)
            batch_max_id = row_id;
         rows[batch_count].id = row_id;
         if (text)
            safe_strscpy(rows[batch_count].text, text);
         else
            rows[batch_count].text[0] = '\0';
         batch_count++;
      }
      cursor_id = batch_max_id;
      sqlite3_finalize(stmt);
      pthread_mutex_unlock(&s_db.mutex);

      if (batch_count == 0)
         break;

      /* Embed outside the lock, then UPDATE under the lock — one item at a time
       * to keep the lock-hold duration minimal (arch doc: copy data, release, process). */
      for (int i = 0; i < batch_count && !atomic_load(&s_shutdown); i++) {
         if (rows[i].text[0] == '\0')
            continue;

         float embedding[MAX_EMBEDDING_DIMS];
         int dims = 0;
         if (embedding_engine_embed(rows[i].text, embedding, MAX_EMBEDDING_DIMS, &dims) != 0 ||
             dims <= 0)
            continue;

         float norm = embedding_engine_l2_norm(embedding, dims);

         pthread_mutex_lock(&s_db.mutex);
         if (!s_db.initialized) {
            pthread_mutex_unlock(&s_db.mutex);
            free(rows);
            return FAILURE;
         }

         sqlite3_stmt *upd = NULL;
         int urc = sqlite3_prepare_v2(
             s_db.db, "UPDATE document_chunks SET embedding = ?, embedding_norm = ? WHERE id = ?",
             -1, &upd, NULL);
         if (urc == SQLITE_OK) {
            sqlite3_bind_blob(upd, 1, embedding, dims * (int)sizeof(float), SQLITE_TRANSIENT);
            sqlite3_bind_double(upd, 2, (double)norm);
            sqlite3_bind_int64(upd, 3, rows[i].id);
            if (sqlite3_step(upd) == SQLITE_DONE)
               total_done++;
            sqlite3_finalize(upd);
         }
         pthread_mutex_unlock(&s_db.mutex);

         atomic_fetch_add(&s_done, 1);

         int sleep_ms = g_config.memory.recompute_batch_sleep_ms;
         if (sleep_ms > 0)
            usleep((useconds_t)sleep_ms * 1000u);
      }

      if (batch_count < batch_size)
         break;

      if (++loops > 200000) {
         OLOG_WARNING("memory_embed_recompute: chunk loop guard tripped");
         break;
      }
   }

   free(rows);
   OLOG_INFO("memory_embed_recompute: document_chunks done recomputed=%d", total_done);
   return SUCCESS;
}

/* =============================================================================
 * Worker thread
 * ============================================================================= */

static void *recompute_thread_fn(void *arg) {
   (void)arg;

#ifdef __linux__
   /* Best-effort priority drop for this background worker.  nice() can return
    * -1 on success, so there's nothing useful to check — consume the result
    * only to satisfy warn_unused_result. */
   int nice_unused = nice(10);
   (void)nice_unused;
#endif

   OLOG_INFO("memory_embed_recompute: worker started (model_id='%s')", g_config.memory.model_id);

   /* Collect users that need recomputation */
   int user_ids[256];
   int user_count = 0;
   if (collect_stale_users(user_ids, 256, &user_count) != SUCCESS || user_count == 0) {
      OLOG_INFO("memory_embed_recompute: all users up-to-date, nothing to do");
      atomic_store(&s_running, false);
      return NULL;
   }

   OLOG_INFO("memory_embed_recompute: re-indexing embeddings for %d user(s)", user_count);

   /* Facts + entities pass per user */
   for (int u = 0; u < user_count && !atomic_load(&s_shutdown); u++) {
      int uid = user_ids[u];

      /* Re-check in case a concurrent update already fixed this user */
      bool still_needs = false;
      if (user_needs_recompute(uid, &still_needs) != SUCCESS || !still_needs)
         continue;

      OLOG_INFO("memory_embed_recompute: processing user=%d", uid);

      bool facts_ok = (recompute_facts_for_user(uid) == SUCCESS);

      if (atomic_load(&s_shutdown))
         break;

      bool entities_ok = (recompute_entities_for_user(uid) == SUCCESS);

      if (atomic_load(&s_shutdown))
         break;

      bool summaries_ok = (recompute_summaries_for_user(uid) == SUCCESS);

      if (atomic_load(&s_shutdown))
         break;

      /* Mark this user current only after ALL THREE passes succeed.
       * If any failed, leave embeddings_model_id NULL so next start retries.
       *
       * Trade-off: a summaries-only failure today re-runs facts + entities
       * on the next boot too.  At per-user scales the dev has measured
       * (~2k facts + 300 entities + 270 summaries), the wasted re-embed
       * cost is small (~tens of seconds) and the simpler single-flag
       * design avoids divergence between which pass succeeded and when
       * the user gets to see semantic results for that pass.  When per-
       * pass independence matters (large multi-user deployments where a
       * single failure across thousands of facts is expensive to redo),
       * split into facts_embeddings_model_id / entities_embeddings_model_id /
       * summaries_embeddings_model_id columns and gate each pass's skip-
       * check on the corresponding column.  Not blocking. */
      if (facts_ok && entities_ok && summaries_ok) {
         user_set_model_id(uid);
         OLOG_INFO("memory_embed_recompute: user=%d fully re-indexed", uid);
      } else {
         OLOG_WARNING("memory_embed_recompute: user=%d incomplete (facts=%s entities=%s "
                      "summaries=%s), will retry on next start",
                      uid, facts_ok ? "ok" : "failed", entities_ok ? "ok" : "failed",
                      summaries_ok ? "ok" : "failed");
      }
   }

   if (atomic_load(&s_shutdown)) {
      OLOG_INFO("memory_embed_recompute: worker interrupted during user pass");
      atomic_store(&s_running, false);
      return NULL;
   }

   /* Deferred lower-priority document_chunks pass.
    * system_metadata.embedding_model_id is updated only AFTER chunks complete so
    * that an interrupted chunk pass triggers a full re-run on next start. */
   recompute_document_chunks();

   meta_set("embedding_model_id", g_config.memory.model_id);
   OLOG_INFO("memory_embed_recompute: system model_id updated to '%s'", g_config.memory.model_id);

   OLOG_INFO("memory_embed_recompute: worker complete (done=%lld)",
             (long long)atomic_load(&s_done));
   atomic_store(&s_running, false);
   return NULL;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int memory_embed_recompute_start(void) {
   if (!g_config.memory.recompute_on_model_change) {
      OLOG_WARNING("memory_embed_recompute: recompute_on_model_change=false — "
                   "stored embeddings will NOT be updated on model swap");
      return SUCCESS;
   }

   if (!embedding_engine_available()) {
      OLOG_INFO("memory_embed_recompute: embedding engine unavailable, skipping");
      return SUCCESS;
   }

   if (g_config.memory.model_id[0] == '\0') {
      OLOG_INFO("memory_embed_recompute: no model_id configured, skipping");
      return SUCCESS;
   }

   /* Check system_metadata for model change */
   char stored_model_id[64] = { 0 };
   meta_get("embedding_model_id", stored_model_id, sizeof(stored_model_id));

   if (strcmp(stored_model_id, g_config.memory.model_id) == 0) {
      /* Model unchanged — still check if any user has a stale or missing model_id */
      int user_ids[256];
      int user_count = 0;
      collect_stale_users(user_ids, 256, &user_count);
      if (user_count == 0) {
         OLOG_INFO("memory_embed_recompute: model_id '%s' unchanged, skipping",
                   g_config.memory.model_id);
         return SUCCESS;
      }
      OLOG_INFO("memory_embed_recompute: %d user(s) have stale embeddings", user_count);
   } else {
      OLOG_INFO("memory_embed_recompute: model changed ('%s' → '%s'), re-indexing",
                stored_model_id[0] ? stored_model_id : "(none)", g_config.memory.model_id);
   }

   atomic_store(&s_shutdown, false);
   atomic_store(&s_running, true);
   atomic_store(&s_done, 0);
   atomic_store(&s_total, 0);

   if (pthread_create(&s_thread, NULL, recompute_thread_fn, NULL) != 0) {
      OLOG_ERROR("memory_embed_recompute: failed to create worker thread");
      atomic_store(&s_running, false);
      return FAILURE;
   }

   return SUCCESS;
}

void memory_embed_recompute_stop(void) {
   if (!atomic_load(&s_running))
      return;

   atomic_store(&s_shutdown, true);
   pthread_join(s_thread, NULL);
   atomic_store(&s_running, false);
   OLOG_INFO("memory_embed_recompute: worker stopped");
}

bool memory_embed_recompute_in_progress(void) {
   return atomic_load(&s_running);
}

int memory_embed_recompute_status(int64_t *done_out, int64_t *total_out) {
   if (!atomic_load(&s_running))
      return FAILURE;
   if (done_out)
      *done_out = (int64_t)atomic_load(&s_done);
   if (total_out)
      *total_out = 0; /* total not populated until admin socket Phase 2 */
   return SUCCESS;
}
