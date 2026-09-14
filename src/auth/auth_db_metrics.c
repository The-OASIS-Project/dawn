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
 * Authentication Database - Session Metrics Module
 *
 * Handles session performance metrics storage and retrieval:
 * - Save and update session metrics (ASR, LLM, TTS timings)
 * - Provider-specific token usage tracking
 * - Metric aggregation and filtering
 * - Retention management
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "logging.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Session Metrics Operations
 * ============================================================================= */

int auth_db_save_session_metrics(session_metrics_t *metrics) {
   if (!metrics) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   int rc;

   if (metrics->id > 0) {
      /* UPDATE existing row (per-query update case) */
      sqlite3_reset(s_db.stmt_metrics_update);

      /* Bind 12 parameters: 11 update values + 1 id for WHERE */
      sqlite3_bind_int64(s_db.stmt_metrics_update, 1, (sqlite3_int64)metrics->ended_at);
      sqlite3_bind_int(s_db.stmt_metrics_update, 2, (int)metrics->queries_total);
      sqlite3_bind_int(s_db.stmt_metrics_update, 3, (int)metrics->queries_cloud);
      sqlite3_bind_int(s_db.stmt_metrics_update, 4, (int)metrics->queries_local);
      sqlite3_bind_int(s_db.stmt_metrics_update, 5, (int)metrics->errors_count);
      sqlite3_bind_int(s_db.stmt_metrics_update, 6, (int)metrics->fallbacks_count);
      sqlite3_bind_double(s_db.stmt_metrics_update, 7, metrics->avg_asr_ms);
      sqlite3_bind_double(s_db.stmt_metrics_update, 8, metrics->avg_llm_ttft_ms);
      sqlite3_bind_double(s_db.stmt_metrics_update, 9, metrics->avg_llm_total_ms);
      sqlite3_bind_double(s_db.stmt_metrics_update, 10, metrics->avg_tts_ms);
      sqlite3_bind_double(s_db.stmt_metrics_update, 11, metrics->avg_pipeline_ms);
      sqlite3_bind_int64(s_db.stmt_metrics_update, 12, metrics->id);

      rc = sqlite3_step(s_db.stmt_metrics_update);
      sqlite3_reset(s_db.stmt_metrics_update);

      if (rc != SQLITE_DONE) {
         OLOG_ERROR("auth_db: failed to update session metrics: %s", sqlite3_errmsg(s_db.db));
         AUTH_DB_UNLOCK();
         return AUTH_DB_FAILURE;
      }
   } else {
      /* INSERT new row (first query in session) */
      sqlite3_reset(s_db.stmt_metrics_save);

      /* Bind all 15 parameters (token usage is in provider table) */
      sqlite3_bind_int(s_db.stmt_metrics_save, 1, (int)metrics->session_id);
      if (metrics->user_id > 0) {
         sqlite3_bind_int(s_db.stmt_metrics_save, 2, metrics->user_id);
      } else {
         sqlite3_bind_null(s_db.stmt_metrics_save, 2);
      }
      sqlite3_bind_text(s_db.stmt_metrics_save, 3, metrics->session_type, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(s_db.stmt_metrics_save, 4, (sqlite3_int64)metrics->started_at);
      sqlite3_bind_int64(s_db.stmt_metrics_save, 5, (sqlite3_int64)metrics->ended_at);

      /* Query counts */
      sqlite3_bind_int(s_db.stmt_metrics_save, 6, (int)metrics->queries_total);
      sqlite3_bind_int(s_db.stmt_metrics_save, 7, (int)metrics->queries_cloud);
      sqlite3_bind_int(s_db.stmt_metrics_save, 8, (int)metrics->queries_local);
      sqlite3_bind_int(s_db.stmt_metrics_save, 9, (int)metrics->errors_count);
      sqlite3_bind_int(s_db.stmt_metrics_save, 10, (int)metrics->fallbacks_count);

      /* Performance averages */
      sqlite3_bind_double(s_db.stmt_metrics_save, 11, metrics->avg_asr_ms);
      sqlite3_bind_double(s_db.stmt_metrics_save, 12, metrics->avg_llm_ttft_ms);
      sqlite3_bind_double(s_db.stmt_metrics_save, 13, metrics->avg_llm_total_ms);
      sqlite3_bind_double(s_db.stmt_metrics_save, 14, metrics->avg_tts_ms);
      sqlite3_bind_double(s_db.stmt_metrics_save, 15, metrics->avg_pipeline_ms);

      rc = sqlite3_step(s_db.stmt_metrics_save);
      sqlite3_reset(s_db.stmt_metrics_save);

      if (rc != SQLITE_DONE) {
         OLOG_ERROR("auth_db: failed to save session metrics: %s", sqlite3_errmsg(s_db.db));
         AUTH_DB_UNLOCK();
         return AUTH_DB_FAILURE;
      }

      /* Get the inserted row ID */
      metrics->id = sqlite3_last_insert_rowid(s_db.db);

      OLOG_INFO("auth_db: created session metrics (id=%lld, session=%u, type=%s)",
                (long long)metrics->id, metrics->session_id, metrics->session_type);
   }

   AUTH_DB_UNLOCK();
   return AUTH_DB_SUCCESS;
}

int auth_db_save_provider_metrics(int64_t session_metrics_id,
                                  const session_provider_metrics_t *providers,
                                  int count) {
   if (!providers || count <= 0 || session_metrics_id <= 0) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Delete existing provider metrics before re-inserting (for per-query updates) */
   sqlite3_reset(s_db.stmt_provider_metrics_delete);
   sqlite3_bind_int64(s_db.stmt_provider_metrics_delete, 1, session_metrics_id);
   int rc = sqlite3_step(s_db.stmt_provider_metrics_delete);
   sqlite3_reset(s_db.stmt_provider_metrics_delete);
   if (rc != SQLITE_DONE) {
      OLOG_WARNING("auth_db: failed to delete old provider metrics: %s", sqlite3_errmsg(s_db.db));
   }

   int saved = 0;
   for (int i = 0; i < count && i < MAX_PROVIDERS_PER_SESSION; i++) {
      const session_provider_metrics_t *p = &providers[i];

      /* Skip entries with no provider name or no data */
      if (!p->provider[0] || (p->tokens_input == 0 && p->tokens_output == 0 && p->queries == 0)) {
         continue;
      }

      sqlite3_reset(s_db.stmt_provider_metrics_save);
      sqlite3_bind_int64(s_db.stmt_provider_metrics_save, 1, session_metrics_id);
      sqlite3_bind_text(s_db.stmt_provider_metrics_save, 2, p->provider, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(s_db.stmt_provider_metrics_save, 3, (sqlite3_int64)p->tokens_input);
      sqlite3_bind_int64(s_db.stmt_provider_metrics_save, 4, (sqlite3_int64)p->tokens_output);
      sqlite3_bind_int64(s_db.stmt_provider_metrics_save, 5, (sqlite3_int64)p->tokens_cached);
      sqlite3_bind_int(s_db.stmt_provider_metrics_save, 6, (int)p->queries);

      rc = sqlite3_step(s_db.stmt_provider_metrics_save);
      sqlite3_reset(s_db.stmt_provider_metrics_save);

      if (rc != SQLITE_DONE) {
         OLOG_WARNING("auth_db: failed to save provider metrics for %s: %s", p->provider,
                      sqlite3_errmsg(s_db.db));
      } else {
         saved++;
      }
   }

   if (saved > 0) {
      OLOG_INFO("auth_db: saved %d provider metrics for session_metrics_id=%lld", saved,
                (long long)session_metrics_id);
   }

   AUTH_DB_UNLOCK();
   return AUTH_DB_SUCCESS;
}

int auth_db_list_session_metrics(const session_metrics_filter_t *filter,
                                 session_metrics_callback_t callback,
                                 void *ctx) {
   if (!callback) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Build dynamic query with filters using parameterized queries to prevent SQL injection */
   char sql[1024];
   int offset = snprintf(
       sql, sizeof(sql),
       "SELECT id, session_id, user_id, session_type, started_at, ended_at, "
       "queries_total, queries_cloud, queries_local, errors_count, fallbacks_count, "
       "avg_asr_ms, avg_llm_ttft_ms, avg_llm_total_ms, avg_tts_ms, avg_pipeline_ms "
       "FROM session_metrics WHERE 1=1");

   /* Track which parameters to bind */
   int param_count = 0;
   bool has_user_id = (filter && filter->user_id > 0);
   bool has_type = (filter && filter->type);
   bool has_since = (filter && filter->since > 0);
   bool has_until = (filter && filter->until > 0);

   if (has_user_id) {
      offset += snprintf(sql + offset, sizeof(sql) - offset, " AND user_id = ?");
      param_count++;
   }
   if (has_type) {
      offset += snprintf(sql + offset, sizeof(sql) - offset, " AND session_type = ?");
      param_count++;
   }
   if (has_since) {
      offset += snprintf(sql + offset, sizeof(sql) - offset, " AND started_at >= ?");
      param_count++;
   }
   if (has_until) {
      offset += snprintf(sql + offset, sizeof(sql) - offset, " AND started_at <= ?");
      param_count++;
   }

   offset += snprintf(sql + offset, sizeof(sql) - offset,
                      " ORDER BY started_at DESC LIMIT ? OFFSET ?");
   param_count += 2; /* limit and offset */

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: failed to prepare metrics query: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   /* Bind parameters in order */
   int param = 1;
   if (has_user_id) {
      sqlite3_bind_int(stmt, param++, filter->user_id);
   }
   if (has_type) {
      sqlite3_bind_text(stmt, param++, filter->type, -1, SQLITE_STATIC);
   }
   if (has_since) {
      sqlite3_bind_int64(stmt, param++, (sqlite3_int64)filter->since);
   }
   if (has_until) {
      sqlite3_bind_int64(stmt, param++, (sqlite3_int64)filter->until);
   }

   int limit = (filter && filter->limit > 0) ? filter->limit : 20;
   int skip = (filter && filter->offset > 0) ? filter->offset : 0;
   sqlite3_bind_int(stmt, param++, limit);
   sqlite3_bind_int(stmt, param++, skip);

   int result = AUTH_DB_SUCCESS;
   while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
      session_metrics_t m = { 0 };

      m.id = sqlite3_column_int64(stmt, 0);
      m.session_id = (uint32_t)sqlite3_column_int(stmt, 1);
      m.user_id = sqlite3_column_int(stmt, 2);

      const char *type = (const char *)sqlite3_column_text(stmt, 3);
      if (type) {
         safe_strscpy(m.session_type, type);
      }

      m.started_at = (time_t)sqlite3_column_int64(stmt, 4);
      m.ended_at = (time_t)sqlite3_column_int64(stmt, 5);

      m.queries_total = (uint32_t)sqlite3_column_int(stmt, 6);
      m.queries_cloud = (uint32_t)sqlite3_column_int(stmt, 7);
      m.queries_local = (uint32_t)sqlite3_column_int(stmt, 8);
      m.errors_count = (uint32_t)sqlite3_column_int(stmt, 9);
      m.fallbacks_count = (uint32_t)sqlite3_column_int(stmt, 10);

      /* Performance averages (token usage is queried separately from provider table) */
      m.avg_asr_ms = sqlite3_column_double(stmt, 11);
      m.avg_llm_ttft_ms = sqlite3_column_double(stmt, 12);
      m.avg_llm_total_ms = sqlite3_column_double(stmt, 13);
      m.avg_tts_ms = sqlite3_column_double(stmt, 14);
      m.avg_pipeline_ms = sqlite3_column_double(stmt, 15);

      if (callback(&m, ctx) != 0) {
         break; /* Callback requested stop */
      }
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int auth_db_get_metrics_aggregate(const session_metrics_filter_t *filter,
                                  session_metrics_t *totals) {
   if (!totals) {
      return AUTH_DB_INVALID;
   }

   memset(totals, 0, sizeof(*totals));

   AUTH_DB_LOCK_OR_FAIL();

   /* Build aggregate query using parameterized queries to prevent SQL injection */
   char sql[1024];
   int offset = snprintf(sql, sizeof(sql),
                         "SELECT "
                         "COUNT(*), "
                         "SUM(queries_total), SUM(queries_cloud), SUM(queries_local), "
                         "SUM(errors_count), SUM(fallbacks_count), "
                         "AVG(avg_asr_ms), AVG(avg_llm_ttft_ms), AVG(avg_llm_total_ms), "
                         "AVG(avg_tts_ms), AVG(avg_pipeline_ms) "
                         "FROM session_metrics WHERE 1=1");

   /* Track which parameters to bind */
   bool has_user_id = (filter && filter->user_id > 0);
   bool has_type = (filter && filter->type);
   bool has_since = (filter && filter->since > 0);
   bool has_until = (filter && filter->until > 0);

   if (has_user_id) {
      offset += snprintf(sql + offset, sizeof(sql) - offset, " AND user_id = ?");
   }
   if (has_type) {
      offset += snprintf(sql + offset, sizeof(sql) - offset, " AND session_type = ?");
   }
   if (has_since) {
      offset += snprintf(sql + offset, sizeof(sql) - offset, " AND started_at >= ?");
   }
   if (has_until) {
      offset += snprintf(sql + offset, sizeof(sql) - offset, " AND started_at <= ?");
   }

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: failed to prepare metrics aggregate query: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   /* Bind parameters in order */
   int param = 1;
   if (has_user_id) {
      sqlite3_bind_int(stmt, param++, filter->user_id);
   }
   if (has_type) {
      sqlite3_bind_text(stmt, param++, filter->type, -1, SQLITE_STATIC);
   }
   if (has_since) {
      sqlite3_bind_int64(stmt, param++, (sqlite3_int64)filter->since);
   }
   if (has_until) {
      sqlite3_bind_int64(stmt, param++, (sqlite3_int64)filter->until);
   }

   if (sqlite3_step(stmt) == SQLITE_ROW) {
      /* session_id stores the count of sessions for aggregate */
      totals->session_id = (uint32_t)sqlite3_column_int(stmt, 0);

      totals->queries_total = (uint32_t)sqlite3_column_int(stmt, 1);
      totals->queries_cloud = (uint32_t)sqlite3_column_int(stmt, 2);
      totals->queries_local = (uint32_t)sqlite3_column_int(stmt, 3);
      totals->errors_count = (uint32_t)sqlite3_column_int(stmt, 4);
      totals->fallbacks_count = (uint32_t)sqlite3_column_int(stmt, 5);

      /* Performance averages */
      totals->avg_asr_ms = sqlite3_column_double(stmt, 6);
      totals->avg_llm_ttft_ms = sqlite3_column_double(stmt, 7);
      totals->avg_llm_total_ms = sqlite3_column_double(stmt, 8);
      totals->avg_tts_ms = sqlite3_column_double(stmt, 9);
      totals->avg_pipeline_ms = sqlite3_column_double(stmt, 10);
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return AUTH_DB_SUCCESS;
}

int auth_db_cleanup_session_metrics(int retention_days, int *deleted_out) {
   if (retention_days <= 0) {
      retention_days = SESSION_METRICS_RETENTION_DAYS;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* Cast to time_t before multiplication to prevent integer overflow */
   time_t cutoff = time(NULL) - ((time_t)retention_days * 24 * 60 * 60);

   sqlite3_reset(s_db.stmt_metrics_delete_old);
   sqlite3_bind_int64(s_db.stmt_metrics_delete_old, 1, (sqlite3_int64)cutoff);

   int rc = sqlite3_step(s_db.stmt_metrics_delete_old);
   sqlite3_reset(s_db.stmt_metrics_delete_old);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("auth_db: failed to cleanup old metrics: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   int deleted = sqlite3_changes(s_db.db);
   if (deleted > 0) {
      OLOG_INFO("auth_db: cleaned up %d old session metrics (older than %d days)", deleted,
                retention_days);
   }

   AUTH_DB_UNLOCK();

   if (deleted_out) {
      *deleted_out = deleted;
   }
   return AUTH_DB_SUCCESS;
}
