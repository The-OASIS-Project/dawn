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
 * Authentication Database Prepared-Statement Management
 *
 * Owns prepare_statements() and finalize_statements() — the init/teardown
 * of every cached sqlite3_stmt* in auth_db_state_t.  Split out from
 * auth_db_core.c to keep individual files under the size limits in
 * CLAUDE.md.  Cross-module entry points are declared in auth_db_internal.h.
 *
 * SECURITY: All statements use parameter binding.  Never concatenate user
 * input into SQL strings.  See: CWE-89, OWASP SQL Injection Prevention.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <stddef.h>
#include <string.h>

#include "auth/auth_db_internal.h"
#include "logging.h"

/* =============================================================================
 * Prepared Statement Management
 * ============================================================================= */

int auth_db_prepare_statements(void) {
   int rc;

   /* User statements */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO users (username, password_hash, is_admin, created_at) VALUES (?, ?, ?, ?)", -1,
       &s_db.stmt_create_user, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare create_user failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, username, password_hash, is_admin, created_at, "
       "last_login, failed_attempts, lockout_until FROM users WHERE username = ?",
       -1, &s_db.stmt_get_user, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare get_user failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM users", -1, &s_db.stmt_count_users, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare count_users failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db, "UPDATE users SET failed_attempts = failed_attempts + 1 WHERE username = ?", -1,
       &s_db.stmt_inc_failed_attempts, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare inc_failed_attempts failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE users SET failed_attempts = 0 WHERE username = ?", -1,
                           &s_db.stmt_reset_failed_attempts, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare reset_failed_attempts failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE users SET last_login = ? WHERE username = ?", -1,
                           &s_db.stmt_update_last_login, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare update_last_login failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE users SET lockout_until = ? WHERE username = ?", -1,
                           &s_db.stmt_set_lockout, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare set_lockout failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Session statements */
   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO sessions (token, user_id, created_at, last_activity, "
                           "expires_at, ip_address, user_agent) VALUES (?, ?, ?, ?, ?, ?, ?)",
                           -1, &s_db.stmt_create_session, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare create_session failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT s.token, s.user_id, u.username, u.is_admin, s.created_at, "
                           "s.last_activity, s.expires_at, s.ip_address, s.user_agent "
                           "FROM sessions s JOIN users u ON s.user_id = u.id WHERE s.token = ?",
                           -1, &s_db.stmt_get_session, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare get_session failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE sessions SET last_activity = ? WHERE token = ?", -1,
                           &s_db.stmt_update_session_activity, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare update_session_activity failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM sessions WHERE token = ?", -1,
                           &s_db.stmt_delete_session, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare delete_session failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM sessions WHERE user_id = ?", -1,
                           &s_db.stmt_delete_user_sessions, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare delete_user_sessions failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "DELETE FROM sessions WHERE expires_at IS NOT NULL AND expires_at < ?",
                           -1, &s_db.stmt_delete_expired_sessions, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare delete_expired_sessions failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Rate limiting statements */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT COUNT(*) FROM login_attempts WHERE ip_address = ? AND timestamp > ? AND success = 0",
       -1, &s_db.stmt_count_recent_failures, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare count_recent_failures failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO login_attempts (ip_address, username, timestamp, success) VALUES (?, ?, ?, ?)",
       -1, &s_db.stmt_log_attempt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare log_attempt failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM login_attempts WHERE timestamp < ?", -1,
                           &s_db.stmt_delete_old_attempts, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare delete_old_attempts failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Audit log statements */
   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO auth_log (timestamp, event, username, ip_address, details) "
                           "VALUES (?, ?, ?, ?, ?)",
                           -1, &s_db.stmt_log_event, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare log_event failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM auth_log WHERE timestamp < ?", -1,
                           &s_db.stmt_delete_old_logs, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare delete_old_logs failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* User settings statements */
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT persona_description, persona_mode, location, timezone, units, "
                           "theme FROM user_settings WHERE user_id = ?",
                           -1, &s_db.stmt_get_user_settings, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare get_user_settings failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO user_settings (user_id, persona_description, persona_mode, location, timezone, "
       "units, theme, updated_at) "
       "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
       "ON CONFLICT(user_id) DO UPDATE SET "
       "persona_description=excluded.persona_description, persona_mode=excluded.persona_mode, "
       "location=excluded.location, timezone=excluded.timezone, units=excluded.units, "
       "theme=excluded.theme, updated_at=excluded.updated_at",
       -1, &s_db.stmt_set_user_settings, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare set_user_settings failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Conversation statements.  anchor_date (v42) carries the conversation's
    * logical anchor timestamp so memory_extraction.c can resolve relative
    * temporal phrases.  Production passes time(NULL); bench passes session date. */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO conversations (user_id, title, created_at, updated_at, anchor_date) "
       "VALUES (?, ?, ?, ?, ?)",
       -1, &s_db.stmt_conv_create, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, title, created_at, updated_at, message_count, is_archived, "
       "context_tokens, context_max, continued_from, compaction_summary, "
       "llm_type, cloud_provider, model, tools_mode, thinking_mode, is_private, origin, "
       "reasoning_effort "
       "FROM conversations WHERE id = ?",
       -1, &s_db.stmt_conv_get, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, title, created_at, updated_at, message_count, is_archived, "
       "context_tokens, context_max, continued_from, compaction_summary, is_private, origin "
       "FROM conversations WHERE user_id = ? AND (is_archived = 0 OR ? = 1) "
       "ORDER BY updated_at DESC LIMIT ? OFFSET ?",
       -1, &s_db.stmt_conv_list, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Admin-only: list all conversations across all users */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT c.id, c.user_id, c.title, c.created_at, c.updated_at, c.message_count, "
       "c.is_archived, c.context_tokens, c.context_max, c.continued_from, "
       "c.compaction_summary, c.is_private, c.origin, u.username "
       "FROM conversations c LEFT JOIN users u ON c.user_id = u.id "
       "WHERE (c.is_archived = 0 OR ? = 1) "
       "ORDER BY c.updated_at DESC LIMIT ? OFFSET ?",
       -1, &s_db.stmt_conv_list_all, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_list_all failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, title, created_at, updated_at, message_count, is_archived, "
       "context_tokens, context_max, continued_from, compaction_summary, is_private, origin "
       "FROM conversations WHERE user_id = ? AND title LIKE ? "
       "ORDER BY updated_at DESC LIMIT ? OFFSET ?",
       -1, &s_db.stmt_conv_search, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_search failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT DISTINCT c.id, c.user_id, c.title, c.created_at, c.updated_at, "
                           "c.message_count, c.is_archived, c.context_tokens, c.context_max, "
                           "c.continued_from, c.compaction_summary, c.is_private, c.origin "
                           "FROM conversations c "
                           "INNER JOIN messages m ON m.conversation_id = c.id "
                           "WHERE c.user_id = ? AND m.content LIKE ? "
                           "ORDER BY c.updated_at DESC LIMIT ? OFFSET ?",
                           -1, &s_db.stmt_conv_search_content, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_search_content failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE conversations SET title = ? WHERE id = ? AND user_id = ?", -1,
                           &s_db.stmt_conv_rename, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_rename failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM conversations WHERE id = ? AND user_id = ?", -1,
                           &s_db.stmt_conv_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Admin-only: delete any conversation without ownership check */
   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM conversations WHERE id = ?", -1,
                           &s_db.stmt_conv_delete_admin, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_delete_admin failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM conversations WHERE user_id = ?", -1,
                           &s_db.stmt_conv_count, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_count failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO messages (conversation_id, role, content, tool_calls, tool_call_id, "
       "reasoning, created_at) "
       "SELECT ?, ?, ?, ?, ?, ?, ? "
       "WHERE EXISTS (SELECT 1 FROM conversations WHERE id = ? AND user_id = ?)",
       -1, &s_db.stmt_msg_add, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare msg_add failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT m.id, m.conversation_id, m.role, m.content, m.tool_calls, m.tool_call_id, "
       "m.reasoning, m.created_at FROM messages m "
       "INNER JOIN conversations c ON m.conversation_id = c.id "
       "WHERE m.conversation_id = ? AND c.user_id = ? ORDER BY m.id ASC",
       -1, &s_db.stmt_msg_get, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare msg_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Admin-only: get messages without user ownership check */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, conversation_id, role, content, tool_calls, tool_call_id, reasoning, created_at "
       "FROM messages WHERE conversation_id = ? ORDER BY id ASC",
       -1, &s_db.stmt_msg_get_admin, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare msg_get_admin failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE conversations SET updated_at = ?, message_count = message_count + 1 WHERE id = ?",
       -1, &s_db.stmt_conv_update_meta, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_update_meta failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE conversations SET context_tokens = ?, context_max = ? "
                           "WHERE id = ? AND user_id = ?",
                           -1, &s_db.stmt_conv_update_context, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_update_context failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO conversations (user_id, title, created_at, updated_at, origin, anchor_date) "
       "VALUES (?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_conv_create_origin, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_create_origin failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE conversations SET user_id = ? WHERE id = ?", -1,
                           &s_db.stmt_conv_reassign, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_reassign failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Session metrics statements - token usage is in separate provider table */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO session_metrics ("
       "session_id, user_id, session_type, started_at, ended_at, "
       "queries_total, queries_cloud, queries_local, errors_count, fallbacks_count, "
       "avg_asr_ms, avg_llm_ttft_ms, avg_llm_total_ms, avg_tts_ms, avg_pipeline_ms"
       ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_metrics_save, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare metrics_save failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* UPDATE statement for per-query metrics updates (id is param 16) */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE session_metrics SET "
       "ended_at = ?, queries_total = ?, queries_cloud = ?, queries_local = ?, "
       "errors_count = ?, fallbacks_count = ?, avg_asr_ms = ?, avg_llm_ttft_ms = ?, "
       "avg_llm_total_ms = ?, avg_tts_ms = ?, avg_pipeline_ms = ? "
       "WHERE id = ?",
       -1, &s_db.stmt_metrics_update, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare metrics_update failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM session_metrics WHERE started_at < ?", -1,
                           &s_db.stmt_metrics_delete_old, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare metrics_delete_old failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Provider metrics insert (child table) */
   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO session_metrics_providers ("
                           "session_metrics_id, provider, tokens_input, tokens_output, "
                           "tokens_cached, queries"
                           ") VALUES (?, ?, ?, ?, ?, ?)",
                           -1, &s_db.stmt_provider_metrics_save, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare provider_metrics_save failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Delete provider metrics before re-insert (for per-query updates) */
   rc = sqlite3_prepare_v2(s_db.db,
                           "DELETE FROM session_metrics_providers WHERE session_metrics_id = ?", -1,
                           &s_db.stmt_provider_metrics_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare provider_metrics_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Image statements (v30: filesystem-backed, no BLOB) */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO images (id, user_id, source, retention_policy, mime_type, size, filename, "
       "created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_image_create, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, mime_type, size, filename, source, retention_policy, "
       "created_at, last_accessed FROM images WHERE id = ?",
       -1, &s_db.stmt_image_get, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT filename, user_id, source, mime_type, last_accessed FROM images WHERE id = ?", -1,
       &s_db.stmt_image_get_file, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_get_file failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM images WHERE id = ? AND user_id = ?", -1,
                           &s_db.stmt_image_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE images SET last_accessed = ? WHERE id = ?", -1,
                           &s_db.stmt_image_update_access, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_update_access failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE images SET retention_policy = ? "
                           "WHERE id = ? AND (? = 0 OR user_id = ?)",
                           -1, &s_db.stmt_image_update_retention, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_update_retention failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM images WHERE user_id = ?", -1,
                           &s_db.stmt_image_count_user, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_count_user failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "DELETE FROM images WHERE retention_policy = 0 AND created_at < ? "
       "AND id IN (SELECT id FROM images WHERE retention_policy = 0 AND created_at < ? "
       "ORDER BY created_at ASC LIMIT 100)",
       -1, &s_db.stmt_image_delete_old, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_delete_old failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT COALESCE(SUM(size), 0) FROM images WHERE retention_policy = 2",
                           -1, &s_db.stmt_image_cache_total_size, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_cache_total_size failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM images WHERE id = ?", -1,
                           &s_db.stmt_image_delete_cache_lru, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_delete_cache_lru failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, filename FROM images WHERE retention_policy = 0 AND created_at < ? "
       "ORDER BY created_at ASC LIMIT 100",
       -1, &s_db.stmt_image_get_expired_ids, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_get_expired_ids failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, filename, size FROM images WHERE retention_policy = 2 "
                           "ORDER BY COALESCE(last_accessed, created_at) ASC",
                           -1, &s_db.stmt_image_get_cache_lru_ids, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_get_cache_lru_ids failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*), COALESCE(SUM(size), 0) FROM images", -1,
                           &s_db.stmt_image_stats, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare image_stats failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Memory fact statements.  category column appended last in all SELECTs (column 9)
    * to preserve existing column indices in populate_fact_from_row.
    * source_* columns (v40) are always bound last — NULL when no provenance. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO memory_facts (user_id, fact_text, confidence, source, "
                           "category, created_at, normalized_hash, "
                           "source_conversation_id, source_msg_id_start, source_msg_id_end) "
                           "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                           -1, &s_db.stmt_memory_fact_create, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* CWE-639 defense-in-depth: SQL filters on (id, user_id) so a foreign
    * rowid cannot leak a fact owned by another user.  Same shape as
    * memory_db_fact_delete (`stmt_memory_fact_delete`) — wrong-user lookups
    * return zero rows (caller maps to MEMORY_DB_NOT_FOUND, same response a
    * legitimately-missing fact would get; no oracle). */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
       "access_count, superseded_by, category, expires_at FROM memory_facts "
       "WHERE id = ? AND user_id = ?",
       -1, &s_db.stmt_memory_fact_get, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
       "access_count, superseded_by, category FROM memory_facts "
       "WHERE user_id = ?1 AND superseded_by IS NULL "
       "  AND (expires_at IS NULL OR expires_at >= ?4) "
       "ORDER BY confidence DESC LIMIT ?2 OFFSET ?3",
       -1, &s_db.stmt_memory_fact_list, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
       "access_count, superseded_by, category FROM memory_facts "
       "WHERE user_id = ?1 AND superseded_by IS NULL AND fact_text LIKE ?2 ESCAPE '\\' "
       "  AND (expires_at IS NULL OR expires_at >= ?4) "
       "ORDER BY confidence DESC LIMIT ?3",
       -1, &s_db.stmt_memory_fact_search, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_search failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* v48: BM25 keyword search via FTS5.  Bind order: 1=MATCH expression
    * (space-separated pre-stemmed tokens, OR-combined), 2=user_id,
    * 3=max_facts.  Returns rows ordered by BM25 score (raw negative;
    * caller flips sign + sigmoid-normalizes via memory_bm25_normalize).
    * Score column appears as col 10 — populate_fact_from_row reads
    * cols 0-9 by index so the extra col is ignored on the fact-fill
    * side; the caller pulls the score with sqlite3_column_double(.,10). */
   /* v48 BM25 prep statements are SOFT failures: if the FTS5 table is
    * missing (DB held at v47 because the v48 migration failed) these
    * preps fail.  Leave the pointers NULL and let memory_db_fact_search_bm25's
    * NULL-check fall through to the keyword-only legacy path.  Hard-
    * failing the whole daemon on a config-gated experimental feature
    * would be too strong — Phase 1 BM25 is opt-in via bm25_enabled. */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT mf.id, mf.user_id, mf.fact_text, mf.confidence, mf.source, mf.created_at, "
       "mf.last_accessed, mf.access_count, mf.superseded_by, mf.category, "
       "bm25(memory_facts_fts) AS score "
       "FROM memory_facts_fts "
       "JOIN memory_facts mf ON mf.id = memory_facts_fts.rowid "
       "WHERE memory_facts_fts MATCH ?1 "
       "  AND mf.user_id = ?2 "
       "  AND mf.superseded_by IS NULL "
       "  AND (mf.expires_at IS NULL OR mf.expires_at >= ?4) "
       "ORDER BY score ASC LIMIT ?3",
       -1, &s_db.stmt_memory_fact_search_bm25, NULL);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("auth_db: prepare memory_fact_search_bm25 failed (FTS5 table missing?): %s — "
                   "BM25 path will be inactive until v48 migration completes",
                   sqlite3_errmsg(s_db.db));
      s_db.stmt_memory_fact_search_bm25 = NULL;
   }

   /* v48 with `since_ts` filter — same as above but with `AND mf.created_at >= ?`
    * appended.  Used by focus-injection adapters and any time-windowed
    * memory.search query.  Without this, those callers would silently fall
    * back to the legacy multi-LIKE path and the BM25 A/B bench would be
    * dishonest for the windowed-query subset. */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT mf.id, mf.user_id, mf.fact_text, mf.confidence, mf.source, mf.created_at, "
       "mf.last_accessed, mf.access_count, mf.superseded_by, mf.category, "
       "bm25(memory_facts_fts) AS score "
       "FROM memory_facts_fts "
       "JOIN memory_facts mf ON mf.id = memory_facts_fts.rowid "
       "WHERE memory_facts_fts MATCH ?1 "
       "  AND mf.user_id = ?2 "
       "  AND mf.superseded_by IS NULL "
       "  AND mf.created_at >= ?3 "
       "  AND (mf.expires_at IS NULL OR mf.expires_at >= ?5) "
       "ORDER BY score ASC LIMIT ?4",
       -1, &s_db.stmt_memory_fact_search_bm25_since, NULL);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("auth_db: prepare memory_fact_search_bm25_since failed: %s — "
                   "windowed BM25 inactive until v48 migration completes",
                   sqlite3_errmsg(s_db.db));
      s_db.stmt_memory_fact_search_bm25_since = NULL;
   }

   /* v48: FTS5 maintenance — insert/delete are app-driven (no SQL trigger
    * because stemming runs in C).  Update is implemented as delete + insert
    * by the caller; fact_text is immutable post-create in practice (changes
    * go through supersede), but the helper exists for defensive sync.
    * Same soft-failure policy as the search statement above. */
   rc = sqlite3_prepare_v2(s_db.db, "INSERT INTO memory_facts_fts(rowid, fact_stems) VALUES (?, ?)",
                           -1, &s_db.stmt_memory_facts_fts_insert, NULL);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("auth_db: prepare memory_facts_fts_insert failed: %s — "
                   "FTS5 sync inactive until v48 migration completes",
                   sqlite3_errmsg(s_db.db));
      s_db.stmt_memory_facts_fts_insert = NULL;
   }
   /* Contentless FTS5 requires the 'delete' command rather than DELETE FROM
    * (which would leave the index out of sync because there's no content
    * column to read the prior value from). */
   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO memory_facts_fts(memory_facts_fts, rowid, fact_stems) "
                           "VALUES('delete', ?, ?)",
                           -1, &s_db.stmt_memory_facts_fts_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("auth_db: prepare memory_facts_fts_delete failed: %s — "
                   "FTS5 sync inactive until v48 migration completes",
                   sqlite3_errmsg(s_db.db));
      s_db.stmt_memory_facts_fts_delete = NULL;
   }

   /* Per-fact category UPDATE used by the centroid backfill pass (v34).
    * CWE-639 defense-in-depth: SQL filters on (id, user_id) so a foreign
    * rowid cannot overwrite another user's category. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE memory_facts SET category = ? WHERE id = ? AND user_id = ?", -1,
                           &s_db.stmt_memory_fact_update_category, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_update_category failed: %s",
                 sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, fact_text, confidence, source, "
                           "  created_at, last_accessed, access_count, superseded_by, category "
                           "FROM memory_facts "
                           "WHERE user_id = ? AND superseded_by IS NULL "
                           "  AND category = 'general' AND id > ? "
                           "ORDER BY id ASC LIMIT ?",
                           -1, &s_db.stmt_memory_fact_list_general, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_list_general failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT COUNT(*) FROM memory_facts "
                           "WHERE user_id = ? AND superseded_by IS NULL "
                           "  AND category = 'general'",
                           -1, &s_db.stmt_memory_fact_count_general, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_count_general failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE memory_facts SET last_accessed = ?,"
                           "  access_count = access_count + 1,"
                           "  confidence = CASE"
                           "    WHEN (CAST(strftime('%s','now') AS REAL) - last_accessed) > 3600"
                           "    THEN MIN(1.0, confidence + ?)"
                           "    ELSE confidence"
                           "  END "
                           "WHERE id = ? AND user_id = ?",
                           -1, &s_db.stmt_memory_fact_update_access, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_update_access failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* CWE-639 defense-in-depth: SQL filters on (id, user_id) so a foreign
    * rowid cannot bump confidence on another user's fact. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE memory_facts SET confidence = ? WHERE id = ? AND user_id = ?",
                           -1, &s_db.stmt_memory_fact_update_confidence, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_update_confidence failed: %s",
                 sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* CWE-639 defense-in-depth: SQL filters on (id, user_id) for the
    * superseded row AND requires the supersedes-row to be owned by the
    * same user (EXISTS subquery).  A foreign new_fact_id would otherwise
    * let a caller "hide" another user's fact from their own retrieval by
    * pointing superseded_by at a foreign row, AND a foreign old_fact_id
    * would let a caller corrupt another user's fact chain. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE memory_facts SET superseded_by = ? WHERE id = ? AND user_id = ? "
                           "AND EXISTS (SELECT 1 FROM memory_facts WHERE id = ? AND user_id = ?)",
                           -1, &s_db.stmt_memory_fact_supersede, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_supersede failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM memory_facts WHERE id = ? AND user_id = ?", -1,
                           &s_db.stmt_memory_fact_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* note_doc_id IS NULL: exclude memory→note bridge glosses (v61).  This
    * statement backs extraction's like-match dedup AND the relation-supersede
    * old-fact lookup; a LIKE hit on a gloss could otherwise supersede it and
    * silently kill the bridge. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, fact_text, confidence FROM memory_facts "
                           "WHERE user_id = ? AND superseded_by IS NULL "
                           "AND fact_text LIKE ? ESCAPE '\\' AND note_doc_id IS NULL "
                           "ORDER BY confidence DESC LIMIT 5",
                           -1, &s_db.stmt_memory_fact_find_similar, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_find_similar failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, fact_text, confidence FROM memory_facts "
                           "WHERE user_id = ? AND normalized_hash = ? AND superseded_by IS NULL",
                           -1, &s_db.stmt_memory_fact_find_by_hash, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_find_by_hash failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "DELETE FROM memory_facts WHERE user_id = ? AND superseded_by IS NOT NULL "
       "AND created_at < ?",
       -1, &s_db.stmt_memory_fact_prune_superseded, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_prune_superseded failed: %s",
                 sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* note_doc_id IS NULL: bridge glosses live/die with their note (deleted via
    * the note-delete path), never by staleness pruning. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "DELETE FROM memory_facts WHERE user_id = ? AND superseded_by IS NULL "
                           "AND last_accessed < ? AND confidence < ? AND note_doc_id IS NULL",
                           -1, &s_db.stmt_memory_fact_prune_stale, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_prune_stale failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* v58: hard-delete expired facts (the hard phase of fact ephemerality).
    * expires_at < cutoff, where the caller sets cutoff = now - prune_expired_days. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "DELETE FROM memory_facts WHERE user_id = ? "
                           "AND expires_at IS NOT NULL AND expires_at < ?",
                           -1, &s_db.stmt_memory_fact_prune_expired, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_prune_expired failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Memory preference statements.  source_* columns updated on every upsert
    * (latest-source-wins — mirrors the value/confidence overwrite). */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO memory_preferences (user_id, category, value, confidence, source, created_at, "
       "updated_at, source_conversation_id, source_msg_id_start, source_msg_id_end) "
       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
       "ON CONFLICT(user_id, category) DO UPDATE SET "
       "value=excluded.value, confidence=excluded.confidence, updated_at=excluded.updated_at, "
       "source_conversation_id=excluded.source_conversation_id, "
       "source_msg_id_start=excluded.source_msg_id_start, "
       "source_msg_id_end=excluded.source_msg_id_end, "
       "reinforcement_count=reinforcement_count+1",
       -1, &s_db.stmt_memory_pref_upsert, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_pref_upsert failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, category, value, confidence, source, created_at, updated_at, "
       "reinforcement_count FROM memory_preferences WHERE user_id = ? AND category = ?",
       -1, &s_db.stmt_memory_pref_get, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_pref_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, category, value, confidence, source, created_at, updated_at, "
       "reinforcement_count FROM memory_preferences WHERE user_id = ? ORDER BY category "
       "LIMIT ? OFFSET ?",
       -1, &s_db.stmt_memory_pref_list, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_pref_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, category, value, confidence, source, created_at, updated_at, "
       "reinforcement_count FROM memory_preferences "
       "WHERE user_id = ? AND (category LIKE ? ESCAPE '\\' OR value LIKE ? ESCAPE '\\') "
       "ORDER BY confidence DESC LIMIT ?",
       -1, &s_db.stmt_memory_pref_search, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_pref_search failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "DELETE FROM memory_preferences WHERE user_id = ? AND category = ?", -1,
                           &s_db.stmt_memory_pref_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_pref_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Memory summary statements.  source_* appended (v40). */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO memory_summaries (user_id, session_id, summary, topics, sentiment, "
       "created_at, message_count, duration_seconds, "
       "source_conversation_id, source_msg_id_start, source_msg_id_end) "
       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_memory_summary_create, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_summary_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, session_id, summary, topics, sentiment, created_at, "
       "message_count, duration_seconds, consolidated FROM memory_summaries "
       "WHERE user_id = ? AND consolidated = 0 ORDER BY created_at DESC LIMIT ? OFFSET ?",
       -1, &s_db.stmt_memory_summary_list, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_summary_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* CWE-639 defense-in-depth: SQL filters on (id, user_id) so a foreign
    * rowid cannot mark another user's summary consolidated. */
   rc = sqlite3_prepare_v2(
       s_db.db, "UPDATE memory_summaries SET consolidated = 1 WHERE id = ? AND user_id = ?", -1,
       &s_db.stmt_memory_summary_mark_consolidated, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_summary_mark_consolidated failed: %s",
                 sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, session_id, summary, topics, sentiment, created_at, "
       "message_count, duration_seconds, consolidated FROM memory_summaries "
       "WHERE user_id = ? AND (summary LIKE ? ESCAPE '\\' OR topics LIKE ? ESCAPE '\\') "
       "ORDER BY created_at DESC LIMIT ?",
       -1, &s_db.stmt_memory_summary_search, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_summary_search failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Date-filtered memory queries (for time_range search and fixed recent) */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
       "access_count, superseded_by, category FROM memory_facts "
       "WHERE user_id = ?1 AND superseded_by IS NULL AND fact_text LIKE ?2 ESCAPE '\\' "
       "AND created_at >= ?3 AND (expires_at IS NULL OR expires_at >= ?5) "
       "ORDER BY confidence DESC LIMIT ?4",
       -1, &s_db.stmt_memory_fact_search_since, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_search_since failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, session_id, summary, topics, sentiment, created_at, "
       "message_count, duration_seconds, consolidated FROM memory_summaries "
       "WHERE user_id = ? AND (summary LIKE ? ESCAPE '\\' OR topics LIKE ? ESCAPE '\\') "
       "AND created_at >= ? ORDER BY created_at DESC LIMIT ?",
       -1, &s_db.stmt_memory_summary_search_since, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_summary_search_since failed: %s",
                 sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
       "access_count, superseded_by, category FROM memory_facts "
       "WHERE user_id = ?1 AND superseded_by IS NULL AND created_at >= ?2 "
       "  AND (expires_at IS NULL OR expires_at >= ?4) "
       "ORDER BY created_at DESC LIMIT ?3",
       -1, &s_db.stmt_memory_fact_list_since, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_list_since failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, session_id, summary, topics, sentiment, created_at, "
       "message_count, duration_seconds, consolidated FROM memory_summaries "
       "WHERE user_id = ? AND created_at >= ? "
       "ORDER BY created_at DESC LIMIT ?",
       -1, &s_db.stmt_memory_summary_list_since, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_summary_list_since failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Bundle 3 (2026-05-13) — windowed/sorted variants.  Shared WHERE clause
    * (user_id + created_at range) so the DESC variant collapses cleanly to
    * the legacy _list_since semantics when callers pass until=INT64_MAX. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, fact_text, confidence, source, created_at, "
                           "last_accessed, access_count, superseded_by, category FROM memory_facts "
                           "WHERE user_id = ?1 AND superseded_by IS NULL "
                           "  AND created_at >= ?2 AND created_at <= ?3 "
                           "  AND (expires_at IS NULL OR expires_at >= ?5) "
                           "ORDER BY created_at ASC LIMIT ?4",
                           -1, &s_db.stmt_memory_fact_list_window_asc, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_list_window_asc failed: %s",
                 sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, fact_text, confidence, source, created_at, "
                           "last_accessed, access_count, superseded_by, category FROM memory_facts "
                           "WHERE user_id = ?1 AND superseded_by IS NULL "
                           "  AND created_at >= ?2 AND created_at <= ?3 "
                           "  AND (expires_at IS NULL OR expires_at >= ?5) "
                           "ORDER BY created_at DESC LIMIT ?4",
                           -1, &s_db.stmt_memory_fact_list_window_desc, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_fact_list_window_desc failed: %s",
                 sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, session_id, summary, topics, sentiment, created_at, "
       "message_count, duration_seconds, consolidated FROM memory_summaries "
       "WHERE user_id = ? AND created_at >= ? AND created_at <= ? "
       "ORDER BY created_at ASC LIMIT ?",
       -1, &s_db.stmt_memory_summary_list_window_asc, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_summary_list_window_asc failed: %s",
                 sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, session_id, summary, topics, sentiment, created_at, "
       "message_count, duration_seconds, consolidated FROM memory_summaries "
       "WHERE user_id = ? AND created_at >= ? AND created_at <= ? "
       "ORDER BY created_at DESC LIMIT ?",
       -1, &s_db.stmt_memory_summary_list_window_desc, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare memory_summary_list_window_desc failed: %s",
                 sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Conversation extraction tracking statements */
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT last_extracted_msg_count FROM conversations WHERE id = ?", -1,
                           &s_db.stmt_conv_get_last_extracted, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_get_last_extracted failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Advances last_extracted_msg_id from the caller-supplied value rather than
    * re-querying MAX(id) at commit time — avoids TOCTOU when new messages arrive
    * during LLM inference.  CASE guard preserves the existing cursor when the
    * caller passes 0 (early-skip paths with no prov). */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE conversations SET last_extracted_msg_count = ?, "
       "last_extracted_msg_id = CASE WHEN ? > 0 THEN ? ELSE last_extracted_msg_id END, "
       "extraction_attempts = 0, extraction_last_attempt_at = 0 "
       "WHERE id = ?",
       -1, &s_db.stmt_conv_set_last_extracted, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_set_last_extracted failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Conversation privacy statement */
   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE conversations SET is_private = ? "
                           "WHERE id = ? AND user_id = ?",
                           -1, &s_db.stmt_conv_set_private, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_set_private failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Auto-title statements */
   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE conversations SET title = ?, title_locked = 1, updated_at = ? "
                           "WHERE id = ? AND user_id = ? AND title_locked = 0",
                           -1, &s_db.stmt_conv_auto_title, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_auto_title failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE conversations SET title_locked = ? "
                           "WHERE id = ? AND user_id = ?",
                           -1, &s_db.stmt_conv_set_title_locked, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare conv_set_title_locked failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Embedding statements */
   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE memory_facts SET embedding = ?, embedding_norm = ? "
                           "WHERE id = ? AND user_id = ?",
                           -1, &s_db.stmt_memory_fact_update_embedding, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare fact_update_embedding failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* fact_get_embeddings: created_at appended last (col 3) for temporal-query
    * scoring (#3).  Cache loader reads it and stores per-fact for boost computation.
    * note_doc_id (col 4, v61) flags memory→note bridge glosses: they stay in the
    * cache so semantic retrieval can still surface them, but the paraphrase-dedup
    * consumer (nearest_fact) skips them so a gloss never merges with a real fact. */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, embedding, embedding_norm, created_at, note_doc_id FROM memory_facts "
       "WHERE user_id = ? AND superseded_by IS NULL AND embedding IS NOT NULL "
       "ORDER BY confidence DESC LIMIT ?",
       -1, &s_db.stmt_memory_fact_get_embeddings, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare fact_get_embeddings failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, fact_text FROM memory_facts "
                           "WHERE user_id = ? AND superseded_by IS NULL "
                           "AND (embedding IS NULL OR length(embedding)/4 != ?) "
                           "ORDER BY created_at ASC LIMIT ?",
                           -1, &s_db.stmt_memory_fact_list_without_embedding, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare fact_list_without_embedding failed: %s",
                 sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Summary-embedding statements (v45).  No embedding_norm column on
    * memory_summaries — at the per-user summary scale (hundreds) we
    * recompute norms inside the scan instead of paying for the storage. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE memory_summaries SET embedding = ? "
                           "WHERE id = ? AND user_id = ?",
                           -1, &s_db.stmt_memory_summary_update_embedding, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare summary_update_embedding failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Scan-with-embeddings (id + blob only).  Cosine ranking in
    * memory_db_summary_search_semantic happens in two passes: this scan
    * ranks survivors by cosine without materialising the long summary /
    * topics text for every row; a second WHERE id = ? fetch inside the
    * helper pulls full rows for the top-K survivors only.  Same temporal
    * slice as the keyword search_since path (created_at >= ?).
    * ORDER BY created_at DESC means when max_scan eventually trips
    * (at >1k summaries per user), recency wins — the user is more likely
    * to query recent topics than year-old ones. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, embedding FROM memory_summaries "
                           "WHERE user_id = ? AND created_at >= ? AND embedding IS NOT NULL "
                           "ORDER BY created_at DESC LIMIT ?",
                           -1, &s_db.stmt_memory_summary_scan_embeddings, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare summary_scan_embeddings failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, summary FROM memory_summaries "
                           "WHERE user_id = ? "
                           "AND (embedding IS NULL OR length(embedding)/4 != ?) "
                           "ORDER BY created_at ASC LIMIT ?",
                           -1, &s_db.stmt_memory_summary_list_without_embedding, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare summary_list_without_embedding failed: %s",
                 sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Entity graph statements.  Three timestamp slots (?5 ?6 ?7) bound by
    * the caller; see memory_db_entity_upsert_at for the resolution rule. */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO memory_entities (user_id, name, entity_type, canonical_name, "
       "first_seen, last_seen, mention_count) "
       "VALUES (?, ?, ?, ?, ?, ?, 1) "
       "ON CONFLICT(user_id, canonical_name) DO UPDATE SET "
       "last_seen = ?, mention_count = mention_count + 1, "
       "name = CASE WHEN length(excluded.name) > length(name) THEN excluded.name ELSE name END "
       "RETURNING id, mention_count",
       -1, &s_db.stmt_memory_entity_upsert, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare entity_upsert failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Equivalence-class aggregation (v43+): mention_count / first_seen /
    * last_seen are aggregated over {self + soft-aliases pointing at self}
    * via correlated subqueries.  For canonical rows this returns the
    * class total; for alias rows (canonical_id IS NOT NULL) it returns
    * self values, since aliases never have dependents (single-level rule
    * enforced by memory_db_alias_link's no-dependents check).  Idx
    * idx_memory_entities_canonical (partial) + PK back the subqueries
    * at O(log N). */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT e.id, e.user_id, e.name, e.entity_type, e.canonical_name, "
       "  (SELECT COALESCE(SUM(mention_count), 0) FROM memory_entities "
       "     WHERE user_id = e.user_id AND (id = e.id OR canonical_id = e.id)), "
       "  (SELECT COALESCE(MIN(first_seen), 0) FROM memory_entities "
       "     WHERE user_id = e.user_id AND (id = e.id OR canonical_id = e.id)), "
       "  (SELECT COALESCE(MAX(last_seen), 0) FROM memory_entities "
       "     WHERE user_id = e.user_id AND (id = e.id OR canonical_id = e.id)) "
       "FROM memory_entities e "
       "WHERE e.user_id = ? AND e.canonical_name = ?",
       -1, &s_db.stmt_memory_entity_get_by_name, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare entity_get_by_name failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE memory_entities SET embedding = ?, embedding_norm = ? "
                           "WHERE id = ? AND user_id = ?",
                           -1, &s_db.stmt_memory_entity_update_embedding, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare entity_update_embedding failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* canonical_id IS NULL filter (v43): the entity-cache path defaults to
    * canonical-only.  Aliases (rows with canonical_id IS NOT NULL) are
    * excluded from the entity-embedding cache so the resolver / focus
    * adapter pools do not double-count surface-form variants of the same
    * real-world entity.  Bind position 2 = include_aliases (0 = filter
    * aliases out, 1 = include).  See docs/ENTITY_MERGE_DESIGN.md §15. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, name, entity_type, embedding, embedding_norm "
                           "FROM memory_entities "
                           "WHERE user_id = ? AND embedding IS NOT NULL "
                           "  AND (? = 1 OR canonical_id IS NULL) "
                           "ORDER BY mention_count DESC LIMIT ?",
                           -1, &s_db.stmt_memory_entity_get_embeddings, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare entity_get_embeddings failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Memory relation statements.  valid_from/valid_to appended last in all SELECTs
    * (columns 6, 7 in pre-v49 column order) to preserve existing column indices.
    * mention_count appended at column 8 in v49.  source_* columns (v40) appended at
    * bind positions 10, 11, 12.  Bind slots in INSERT unchanged at 12; mention_count
    * uses the column default of 1 on insert and the UPDATE clause bumps it on conflict.
    *
    * On-conflict provenance policy is intentionally simpler than facts.
    *
    * Facts use memory_db_fact_provenance_extend() (four-way decision: same-conv
    * widen / no-prov adopt / newer replace / older no-op) because a fact carries
    * the source text snippets the LLM uses for grounded answering.
    *
    * Relations carry their text trail INDIRECTLY through fact_id (the linked
    * fact's own provenance widens).  Latest-wins on the relation row itself
    * keeps the relation pointing at the most recent witness; widening would
    * duplicate fact-layer machinery on a row that's strictly more constrained
    * (relations have no fact_text).  COALESCE(fact_id, excluded.fact_id) heals
    * orphans without dropping existing links.  valid_from intentionally NOT in
    * the UPDATE-SET — keeps the first observed start-of-validity bound, since
    * latest-wins would silently overwrite a populated valid_from with NULL on
    * re-witness (data loss).
    *
    * confidence = MAX(...): re-witness with a stronger source upgrades; doesn't
    * downgrade.  Lower-confidence re-witness would be data loss in disguise.
    *
    * The conflict-target's COALESCE wrappers must match the partial UNIQUE
    * index expression in auth_db_schema.c exactly — SQLite matches partial-index
    * upserts on expression equality.  WHERE valid_to IS NULL scopes the
    * dedup invariant to currently-open edges (closed historical rows may
    * legitimately repeat: married_to(A) → divorced → re-married is a lifecycle). */
   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO memory_relations (user_id, subject_entity_id, relation, "
                           "object_entity_id, object_value, fact_id, confidence, created_at, "
                           "valid_from, valid_to, "
                           "source_conversation_id, source_msg_id_start, source_msg_id_end) "
                           "VALUES (?, ?, ?, ?, ?, ?, ?, strftime('%s','now'), ?, ?, ?, ?, ?) "
                           "ON CONFLICT(user_id, subject_entity_id, relation, "
                           "            COALESCE(object_entity_id, 0), COALESCE(object_value, '')) "
                           "WHERE valid_to IS NULL DO UPDATE SET "
                           "  mention_count = mention_count + 1, "
                           "  confidence = MAX(confidence, excluded.confidence), "
                           "  source_conversation_id = excluded.source_conversation_id, "
                           "  source_msg_id_start = excluded.source_msg_id_start, "
                           "  source_msg_id_end = excluded.source_msg_id_end, "
                           "  fact_id = COALESCE(fact_id, excluded.fact_id) "
                           "RETURNING id, mention_count",
                           -1, &s_db.stmt_memory_relation_create, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare relation_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Auto-close superseded exclusive relations (v33).  Used inside
    * memory_db_relation_supersede() before the new INSERT.  The (object_entity_id != ?
    * OR object_value != ?) clause skips when the user re-mentions the same target —
    * idempotency check.  COALESCE guards against NULL comparisons. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE memory_relations SET valid_to = ? "
                           "WHERE user_id = ? AND subject_entity_id = ? AND relation = ? "
                           "  AND valid_to IS NULL "
                           "  AND (COALESCE(object_entity_id, 0) != COALESCE(?, 0) "
                           "    OR COALESCE(object_value, '') != COALESCE(?, '')) "
                           "RETURNING fact_id",
                           -1, &s_db.stmt_memory_relation_close_open, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare relation_close_open failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT r.id, r.subject_entity_id, r.relation, r.object_entity_id, "
                           "COALESCE(e.name, r.object_value) AS object_name, r.confidence, "
                           "COALESCE(r.valid_from, 0), COALESCE(r.valid_to, 0), "
                           "r.mention_count "
                           "FROM memory_relations r "
                           "LEFT JOIN memory_entities e ON r.object_entity_id = e.id "
                           "WHERE r.user_id = ? AND r.subject_entity_id = ? LIMIT ?",
                           -1, &s_db.stmt_memory_relation_list_by_subject, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare relation_list_by_subject failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* As-of variant: returns relations valid at the given timestamp.  Bounds:
    *   valid_from IS NULL or valid_from <= as_of
    *   valid_to   IS NULL or valid_to   >  as_of
    * Pass strftime('%s','now') as as_of for the "currently true" common case. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT r.id, r.subject_entity_id, r.relation, r.object_entity_id, "
                           "COALESCE(e.name, r.object_value) AS object_name, r.confidence, "
                           "COALESCE(r.valid_from, 0), COALESCE(r.valid_to, 0), "
                           "r.mention_count "
                           "FROM memory_relations r "
                           "LEFT JOIN memory_entities e ON r.object_entity_id = e.id "
                           "WHERE r.user_id = ? AND r.subject_entity_id = ? "
                           "  AND (r.valid_from IS NULL OR r.valid_from <= ?) "
                           "  AND (r.valid_to IS NULL OR r.valid_to > ?) "
                           "LIMIT ?",
                           -1, &s_db.stmt_memory_relation_list_by_subject_at, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare relation_list_by_subject_at failed: %s",
                 sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT r.id, r.subject_entity_id, r.relation, r.object_entity_id, "
                           "COALESCE(e.name, r.object_value) AS object_name, r.confidence, "
                           "COALESCE(r.valid_from, 0), COALESCE(r.valid_to, 0), "
                           "r.mention_count "
                           "FROM memory_relations r "
                           "LEFT JOIN memory_entities e ON r.subject_entity_id = e.id "
                           "WHERE r.user_id = ? AND r.object_entity_id = ? LIMIT ?",
                           -1, &s_db.stmt_memory_relation_list_by_object, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare relation_list_by_object failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Graph-retrieval Phase 1A: return DISTINCT fact_ids for relations linking
    * @entity_id (as subject OR object).  Only returns relations with non-NULL
    * fact_id — the structured-only relations (NULL fact_id, ~60% of rows) are
    * Phase 1B's territory.  Sorted by confidence DESC so the highest-quality
    * graph-anchored facts surface first when the caller's fan-out cap trips. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT DISTINCT r.fact_id "
                           "FROM memory_relations r "
                           "WHERE r.user_id = ? "
                           "  AND (r.subject_entity_id = ? OR r.object_entity_id = ?) "
                           "  AND r.fact_id IS NOT NULL "
                           "ORDER BY r.confidence DESC, r.created_at DESC "
                           "LIMIT ?",
                           -1, &s_db.stmt_memory_relation_fact_ids_for_entity, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare relation_fact_ids_for_entity failed: %s",
                 sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Equivalence-class aggregation: see entity_get_by_name above for
    * rationale.  ORDER BY uses the row's own mention_count (not the
    * aggregated class total) — keeps the ORDER BY trivially indexable
    * and matches the historical behavior for canonical-row sort order;
    * aggregated counts surface in the row payload, not the sort key.
    *
    * Deliberate asymmetry: the admin canonical-list query in
    * memory_db_alias.c sorts by CLASS total because it's the source-of-
    * truth display for operators making merge decisions, and ~270
    * canonicals is small enough that the per-row subquery sort is
    * sub-millisecond.  Search is on a hotter path and serves both LLM
    * and user queries — index-friendly sort matters more there. */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT e.id, e.user_id, e.name, e.entity_type, e.canonical_name, "
       "  (SELECT COALESCE(SUM(mention_count), 0) FROM memory_entities "
       "     WHERE user_id = e.user_id AND (id = e.id OR canonical_id = e.id)), "
       "  (SELECT COALESCE(MIN(first_seen), 0) FROM memory_entities "
       "     WHERE user_id = e.user_id AND (id = e.id OR canonical_id = e.id)), "
       "  (SELECT COALESCE(MAX(last_seen), 0) FROM memory_entities "
       "     WHERE user_id = e.user_id AND (id = e.id OR canonical_id = e.id)) "
       "FROM memory_entities e "
       "WHERE e.user_id = ? AND e.canonical_name LIKE ? ESCAPE '\\' "
       "ORDER BY e.mention_count DESC LIMIT ? OFFSET ?",
       -1, &s_db.stmt_memory_entity_search, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare entity_search failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM memory_entities WHERE id = ? AND user_id = ?", -1,
                           &s_db.stmt_memory_entity_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare entity_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE memory_entities SET photo_id = ? "
                           "WHERE id = ? AND user_id = ?",
                           -1, &s_db.stmt_memory_entity_set_photo, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare entity_set_photo failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT photo_id FROM memory_entities "
                           "WHERE id = ? AND user_id = ?",
                           -1, &s_db.stmt_memory_entity_get_photo, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare entity_get_photo failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "DELETE FROM memory_relations "
                           "WHERE user_id = ? AND (subject_entity_id = ? OR object_entity_id = ?)",
                           -1, &s_db.stmt_memory_relation_delete_by_entity, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare relation_delete_by_entity failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Satellite mapping statements */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO satellite_mappings (uuid, name, location, ha_area, user_id, tier, "
       "last_seen, created_at, enabled) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
       "ON CONFLICT(uuid) DO UPDATE SET name=excluded.name, location=excluded.location, "
       "tier=excluded.tier, last_seen=excluded.last_seen",
       -1, &s_db.stmt_satellite_upsert, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare satellite_upsert failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT uuid, name, location, ha_area, user_id, tier, last_seen, created_at, enabled "
       "FROM satellite_mappings WHERE uuid = ?",
       -1, &s_db.stmt_satellite_get, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare satellite_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM satellite_mappings WHERE uuid = ?", -1,
                           &s_db.stmt_satellite_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare satellite_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE satellite_mappings SET user_id = ? WHERE uuid = ?", -1,
                           &s_db.stmt_satellite_update_user, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare satellite_update_user failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE satellite_mappings SET location = ?, ha_area = ? WHERE uuid = ?",
                           -1, &s_db.stmt_satellite_update_location, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare satellite_update_location failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE satellite_mappings SET enabled = ? WHERE uuid = ?", -1,
                           &s_db.stmt_satellite_set_enabled, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare satellite_set_enabled failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE satellite_mappings SET last_seen = ? WHERE uuid = ?",
                           -1, &s_db.stmt_satellite_update_last_seen, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare satellite_update_last_seen failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT uuid, name, location, ha_area, user_id, tier, last_seen, created_at, enabled "
       "FROM satellite_mappings ORDER BY name ASC",
       -1, &s_db.stmt_satellite_list, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare satellite_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* Document search statements */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO documents (user_id, filename, filepath, filetype, file_hash, "
       "num_chunks, is_global, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_doc_create, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, filename, filepath, filetype, file_hash, "
                           "num_chunks, is_global, created_at FROM documents WHERE id = ?",
                           -1, &s_db.stmt_doc_get, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id FROM documents WHERE file_hash = ? "
                           "AND (user_id = ? OR is_global = 1)",
                           -1, &s_db.stmt_doc_get_by_hash, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_get_by_hash failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, filename, filepath, filetype, file_hash, "
                           "num_chunks, is_global, created_at FROM documents "
                           "WHERE user_id = ? OR is_global = 1 ORDER BY created_at DESC "
                           "LIMIT ? OFFSET ?",
                           -1, &s_db.stmt_doc_list, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT d.id, d.user_id, d.filename, d.filepath, d.filetype, "
                           "d.file_hash, d.num_chunks, d.is_global, d.created_at, "
                           "COALESCE(u.username, '') FROM documents d "
                           "LEFT JOIN users u ON d.user_id = u.id "
                           "ORDER BY d.created_at DESC LIMIT ? OFFSET ?",
                           -1, &s_db.stmt_doc_list_all, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_list_all failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE documents SET is_global = ? WHERE id = ?", -1,
                           &s_db.stmt_doc_update_global, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_update_global failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM documents WHERE id = ?", -1, &s_db.stmt_doc_delete,
                           NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM documents WHERE user_id = ?", -1,
                           &s_db.stmt_doc_count_user, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_count_user failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* doc_chunk_create: created_at appended last (col 6) — caller passes 0 for
    * unknown timestamps (older docs, manual ingests).  Schema default is 0. */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO document_chunks (document_id, chunk_index, text, embedding, "
       "embedding_norm, created_at) VALUES (?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_doc_chunk_create, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_chunk_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* doc_chunk_search: created_at appended last so existing column indices in
    * downstream populators are preserved. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT c.id, c.chunk_index, c.text, c.embedding, c.embedding_norm, "
                           "d.id, d.filename, d.filetype, c.created_at "
                           "FROM document_chunks c JOIN documents d ON c.document_id = d.id "
                           "WHERE d.user_id = ? OR d.is_global = 1 "
                           "LIMIT ?",
                           -1, &s_db.stmt_doc_chunk_search, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_chunk_search failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, filename, filepath, filetype, file_hash, "
                           "num_chunks, is_global, created_at "
                           "FROM documents "
                           "WHERE (user_id = ? OR is_global = 1) "
                           "AND filename LIKE ? ESCAPE '\\' COLLATE NOCASE "
                           "ORDER BY CASE WHEN LOWER(filename) = LOWER(?) "
                           "THEN 0 ELSE 1 END, created_at DESC LIMIT 1",
                           -1, &s_db.stmt_doc_find_by_name, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_find_by_name failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT chunk_index, text FROM document_chunks "
                           "WHERE document_id = ? ORDER BY chunk_index LIMIT ? OFFSET ?",
                           -1, &s_db.stmt_doc_chunk_read, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_chunk_read failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* doc_chunk_read_range: window a document's chunks by chunk_index value (NOT
    * row offset).  The index pipeline can skip a chunk_index on embed failure, so
    * OFFSET N != chunk at index N — this BETWEEN form fetches the true neighbors
    * for context expansion around a search/grep hit. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT chunk_index, text FROM document_chunks "
                           "WHERE document_id = ? AND chunk_index BETWEEN ? AND ? "
                           "ORDER BY chunk_index",
                           -1, &s_db.stmt_doc_chunk_read_range, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_chunk_read_range failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* doc_chunk_grep (literal substring, permission-scoped).  Two shapes share the
    * same projection + paging: case-insensitive uses LIKE (ASCII-case-insensitive
    * by default; the needle is wildcard-escaped + wrapped in %..% by the caller),
    * case-sensitive uses instr() (literal, no escaping). LIMIT is bound to page+1
    * so the caller can detect "more matches exist" without a COUNT. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT c.chunk_index, c.document_id, d.filename, d.num_chunks "
                           "FROM document_chunks c JOIN documents d ON c.document_id = d.id "
                           "WHERE (d.user_id = ? OR d.is_global = 1) "
                           "AND c.text LIKE ? ESCAPE '\\' "
                           "ORDER BY c.document_id, c.chunk_index LIMIT ? OFFSET ?",
                           -1, &s_db.stmt_doc_chunk_grep_ci, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_chunk_grep_ci failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT c.chunk_index, c.document_id, d.filename, d.num_chunks "
                           "FROM document_chunks c JOIN documents d ON c.document_id = d.id "
                           "WHERE (d.user_id = ? OR d.is_global = 1) "
                           "AND instr(c.text, ?) > 0 "
                           "ORDER BY c.document_id, c.chunk_index LIMIT ? OFFSET ?",
                           -1, &s_db.stmt_doc_chunk_grep_cs, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_chunk_grep_cs failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* === v61: document_chunks_fts (BM25 lexical channel) ===
    * Soft-fail (WARNING + NULL) like the memory_facts_fts statements: these
    * depend on the v61 virtual table, so a DB on which the migration hasn't yet
    * completed must still start — document search falls back to pure-semantic. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO document_chunks_fts(rowid, label_stems, body_stems) "
                           "VALUES (?, ?, ?)",
                           -1, &s_db.stmt_doc_chunk_fts_insert, NULL);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("auth_db: prepare doc_chunk_fts_insert failed: %s — "
                   "document FTS sync inactive until v61 migration completes",
                   sqlite3_errmsg(s_db.db));
      s_db.stmt_doc_chunk_fts_insert = NULL;
   }
   /* Contentless FTS5 requires the 'delete' command (no content column to read
    * the prior value from), so the original stems must be supplied. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO document_chunks_fts(document_chunks_fts, rowid, "
                           "label_stems, body_stems) VALUES('delete', ?, ?, ?)",
                           -1, &s_db.stmt_doc_chunk_fts_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("auth_db: prepare doc_chunk_fts_delete failed: %s — "
                   "document FTS sync inactive until v61 migration completes",
                   sqlite3_errmsg(s_db.db));
      s_db.stmt_doc_chunk_fts_delete = NULL;
   }
   /* Column-weighted BM25 lexical candidate set (its OWN candidates — NOT a
    * boost over the semantic top-K).  ?1=MATCH expr, ?2=label weight,
    * ?3=body weight, ?4=user_id, ?5=limit.  bm25() is negative-for-relevant
    * (ORDER BY score ASC); the caller flips sign before sigmoid-normalizing.
    * Global-IDF caveat: per-user safety is the JOIN + (user_id=? OR is_global=1)
    * filter, not the index. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT c.id, c.chunk_index, c.text, c.document_id, d.filename, "
                           "d.filetype, d.num_chunks, c.created_at, "
                           "bm25(document_chunks_fts, ?2, ?3) AS score "
                           "FROM document_chunks_fts "
                           "JOIN document_chunks c ON c.id = document_chunks_fts.rowid "
                           "JOIN documents d ON d.id = c.document_id "
                           "WHERE document_chunks_fts MATCH ?1 "
                           "AND (d.user_id = ?4 OR d.is_global = 1) "
                           "ORDER BY score ASC LIMIT ?5",
                           -1, &s_db.stmt_doc_chunk_search_bm25, NULL);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("auth_db: prepare doc_chunk_search_bm25 failed: %s — "
                   "lexical document search inactive until v61 migration completes",
                   sqlite3_errmsg(s_db.db));
      s_db.stmt_doc_chunk_search_bm25 = NULL;
   }
   /* Stable-id note edit: replace the single chunk's text + embedding in place
    * (document_chunks always exists, so this is hard-fail).  ?1=text,
    * ?2=embedding, ?3=embedding_norm, ?4=created_at, ?5=chunk id. */
   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE document_chunks SET text = ?, embedding = ?, "
                           "embedding_norm = ?, created_at = ? WHERE id = ?",
                           -1, &s_db.stmt_doc_chunk_update, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare doc_chunk_update failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* === Calendar statements === */

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO calendar_accounts (user_id, name, caldav_url, username, "
       "encrypted_password, auth_type, principal_url, calendar_home_url, enabled, "
       "last_sync, sync_interval_sec, created_at, read_only, oauth_account_key) "
       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_cal_acct_create, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_acct_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, name, caldav_url, username, encrypted_password, "
                           "auth_type, principal_url, calendar_home_url, enabled, last_sync, "
                           "sync_interval_sec, created_at, read_only, oauth_account_key "
                           "FROM calendar_accounts WHERE id = ?",
                           -1, &s_db.stmt_cal_acct_get, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_acct_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, name, caldav_url, username, encrypted_password, "
                           "auth_type, principal_url, calendar_home_url, enabled, last_sync, "
                           "sync_interval_sec, created_at, read_only, oauth_account_key "
                           "FROM calendar_accounts "
                           "WHERE user_id = ? ORDER BY name",
                           -1, &s_db.stmt_cal_acct_list, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_acct_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, user_id, name, caldav_url, username, encrypted_password, "
                           "auth_type, principal_url, calendar_home_url, enabled, last_sync, "
                           "sync_interval_sec, created_at, read_only, oauth_account_key "
                           "FROM calendar_accounts "
                           "WHERE enabled = 1 ORDER BY last_sync ASC",
                           -1, &s_db.stmt_cal_acct_list_enabled, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_acct_list_enabled failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE calendar_accounts SET name=?, caldav_url=?, username=?, "
                           "encrypted_password=?, auth_type=?, enabled=?, sync_interval_sec=? "
                           "WHERE id=?",
                           -1, &s_db.stmt_cal_acct_update, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_acct_update failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM calendar_accounts WHERE id = ?", -1,
                           &s_db.stmt_cal_acct_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_acct_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE calendar_accounts SET last_sync = ? WHERE id = ?", -1,
                           &s_db.stmt_cal_acct_update_sync, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_acct_update_sync failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE calendar_accounts SET principal_url = ?, "
                           "calendar_home_url = ? WHERE id = ?",
                           -1, &s_db.stmt_cal_acct_update_discovery, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_acct_update_discovery failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE calendar_accounts SET read_only = ? WHERE id = ?", -1,
                           &s_db.stmt_cal_acct_set_read_only, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_acct_set_read_only failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE calendar_accounts SET enabled = ? WHERE id = ?", -1,
                           &s_db.stmt_cal_acct_set_enabled, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_acct_set_enabled failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT INTO calendar_calendars (account_id, caldav_path, display_name, "
                           "color, is_active, ctag, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
                           -1, &s_db.stmt_cal_cal_create, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_cal_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, account_id, caldav_path, display_name, color, "
                           "is_active, ctag, created_at FROM calendar_calendars WHERE id = ?",
                           -1, &s_db.stmt_cal_cal_get, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_cal_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT id, account_id, caldav_path, display_name, color, "
                           "is_active, ctag, created_at FROM calendar_calendars "
                           "WHERE account_id = ? ORDER BY display_name",
                           -1, &s_db.stmt_cal_cal_list, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_cal_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE calendar_calendars SET ctag = ? WHERE id = ?", -1,
                           &s_db.stmt_cal_cal_update_ctag, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_cal_update_ctag failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE calendar_calendars SET is_active = ? WHERE id = ?", -1,
                           &s_db.stmt_cal_cal_set_active, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_cal_set_active failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM calendar_calendars WHERE id = ?", -1,
                           &s_db.stmt_cal_cal_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_cal_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT c.id, c.account_id, c.caldav_path, c.display_name, c.color, "
                           "c.is_active, c.ctag, c.created_at, a.read_only "
                           "FROM calendar_calendars c "
                           "JOIN calendar_accounts a ON c.account_id = a.id "
                           "WHERE a.user_id = ? AND a.enabled = 1 AND c.is_active = 1 "
                           "ORDER BY c.display_name",
                           -1, &s_db.stmt_cal_cal_active_for_user, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_cal_active_for_user failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT OR REPLACE INTO calendar_events (calendar_id, uid, etag, summary, "
       "description, location, dtstart, dtend, duration_sec, all_day, "
       "dtstart_date, dtend_date, rrule, raw_ical, last_synced) "
       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_cal_evt_upsert, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_evt_upsert failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT e.id, e.calendar_id, e.uid, e.etag, e.summary, e.description, "
                           "e.location, e.dtstart, e.dtend, e.duration_sec, e.all_day, "
                           "e.dtstart_date, e.dtend_date, e.rrule, e.raw_ical, e.last_synced "
                           "FROM calendar_events e "
                           "JOIN calendar_calendars c ON e.calendar_id = c.id "
                           "JOIN calendar_accounts a ON c.account_id = a.id "
                           "WHERE e.uid = ? AND a.user_id = ? LIMIT 1",
                           -1, &s_db.stmt_cal_evt_get_by_uid, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_evt_get_by_uid failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM calendar_events WHERE id = ?", -1,
                           &s_db.stmt_cal_evt_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_evt_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM calendar_events WHERE calendar_id = ?", -1,
                           &s_db.stmt_cal_evt_delete_by_cal, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_evt_delete_by_cal failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO calendar_occurrences (event_id, dtstart, dtend, all_day, "
       "dtstart_date, dtend_date, summary, location, is_override, is_cancelled, "
       "recurrence_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_cal_occ_insert, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_occ_insert failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM calendar_occurrences WHERE event_id = ?", -1,
                           &s_db.stmt_cal_occ_delete_for_event, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_occ_delete_for_event failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT o.id, o.event_id, o.dtstart, o.dtend, o.all_day, "
                           "o.dtstart_date, o.dtend_date, o.summary, o.location, "
                           "o.is_override, o.is_cancelled, o.recurrence_id, e.uid "
                           "FROM calendar_occurrences o "
                           "JOIN calendar_events e ON o.event_id = e.id "
                           "WHERE e.calendar_id IN (SELECT value FROM json_each(?)) "
                           "AND o.all_day = 0 AND o.is_cancelled = 0 "
                           "AND o.dtstart < ? AND o.dtend > ? "
                           "ORDER BY o.dtstart",
                           -1, &s_db.stmt_cal_occ_in_range, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_occ_in_range failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT o.id, o.event_id, o.dtstart, o.dtend, o.all_day, "
                           "o.dtstart_date, o.dtend_date, o.summary, o.location, "
                           "o.is_override, o.is_cancelled, o.recurrence_id, e.uid "
                           "FROM calendar_occurrences o "
                           "JOIN calendar_events e ON o.event_id = e.id "
                           "WHERE e.calendar_id IN (SELECT value FROM json_each(?)) "
                           "AND o.all_day = 1 AND o.is_cancelled = 0 "
                           "AND o.dtstart_date < ? AND o.dtend_date > ? "
                           "ORDER BY o.dtstart_date",
                           -1, &s_db.stmt_cal_occ_allday_in_range, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_occ_allday_in_range failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT o.id, o.event_id, o.dtstart, o.dtend, o.all_day, "
       "o.dtstart_date, o.dtend_date, o.summary, o.location, "
       "o.is_override, o.is_cancelled, o.recurrence_id, e.uid "
       "FROM calendar_occurrences o "
       "JOIN calendar_events e ON o.event_id = e.id "
       "WHERE e.calendar_id IN (SELECT value FROM json_each(?)) "
       "AND o.is_cancelled = 0 "
       "AND (o.summary LIKE ? COLLATE NOCASE OR o.location LIKE ? COLLATE NOCASE) "
       "ORDER BY o.dtstart LIMIT ?",
       -1, &s_db.stmt_cal_occ_search, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_occ_search failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT o.id, o.event_id, o.dtstart, o.dtend, o.all_day, "
                           "o.dtstart_date, o.dtend_date, o.summary, o.location, "
                           "o.is_override, o.is_cancelled, o.recurrence_id, e.uid "
                           "FROM calendar_occurrences o "
                           "JOIN calendar_events e ON o.event_id = e.id "
                           "WHERE e.calendar_id IN (SELECT value FROM json_each(?)) "
                           "AND o.all_day = 0 AND o.is_cancelled = 0 "
                           "AND o.dtstart >= ? "
                           "ORDER BY o.dtstart LIMIT 1",
                           -1, &s_db.stmt_cal_occ_next, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare cal_occ_next failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* OAuth token statements */
   rc = sqlite3_prepare_v2(s_db.db,
                           "INSERT OR REPLACE INTO oauth_tokens "
                           "(user_id, provider, account_key, encrypted_data, encrypted_data_len, "
                           "scopes, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                           -1, &s_db.stmt_oauth_store, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare oauth_store failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT encrypted_data FROM oauth_tokens "
                           "WHERE user_id = ? AND provider = ? AND account_key = ?",
                           -1, &s_db.stmt_oauth_load, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare oauth_load failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "DELETE FROM oauth_tokens "
                           "WHERE user_id = ? AND provider = ? AND account_key = ?",
                           -1, &s_db.stmt_oauth_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare oauth_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT COUNT(*) FROM oauth_tokens "
                           "WHERE user_id = ? AND provider = ? AND account_key = ?",
                           -1, &s_db.stmt_oauth_exists, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare oauth_exists failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db,
                           "SELECT account_key, scopes FROM oauth_tokens "
                           "WHERE user_id = ? AND provider = ?",
                           -1, &s_db.stmt_oauth_list_accounts, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare oauth_list_accounts failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* === Contacts statements === */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT c.id, c.entity_id, e.name, e.canonical_name, c.field_type, c.value, c.label, "
       "e.photo_id FROM contacts c JOIN memory_entities e ON c.entity_id = e.id "
       "WHERE c.user_id = ? AND e.canonical_name LIKE ? ESCAPE '\\' "
       "AND c.field_type LIKE ? ORDER BY e.name LIMIT ?",
       -1, &s_db.stmt_contacts_find, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare contacts_find failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO contacts (user_id, entity_id, field_type, value, label, created_at) "
       "SELECT ?, ?, ?, ?, ?, ? WHERE EXISTS "
       "(SELECT 1 FROM memory_entities WHERE id = ? AND user_id = ?)",
       -1, &s_db.stmt_contacts_add, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare contacts_add failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM contacts WHERE id = ? AND user_id = ?", -1,
                           &s_db.stmt_contacts_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare contacts_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT c.id, c.entity_id, e.name, e.canonical_name, c.field_type, c.value, c.label, "
       "e.photo_id FROM contacts c JOIN memory_entities e ON c.entity_id = e.id "
       "WHERE c.user_id = ? AND (? IS NULL OR c.field_type = ?) "
       "ORDER BY e.name LIMIT ? OFFSET ?",
       -1, &s_db.stmt_contacts_list, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare contacts_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE contacts SET field_type = ?, value = ?, label = ? WHERE id = ? AND user_id = ?", -1,
       &s_db.stmt_contacts_update, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare contacts_update failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM contacts WHERE user_id = ?", -1,
                           &s_db.stmt_contacts_count, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare contacts_count failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   /* === Email account statements === */
   rc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO email_accounts (user_id, name, imap_server, imap_port, imap_ssl, "
       "smtp_server, smtp_port, smtp_ssl, username, display_name, "
       "encrypted_password, encrypted_password_len, auth_type, oauth_account_key, "
       "enabled, read_only, max_recent, max_body_chars, created_at) "
       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
       -1, &s_db.stmt_email_acct_create, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare email_acct_create failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, name, imap_server, imap_port, imap_ssl, "
       "smtp_server, smtp_port, smtp_ssl, username, display_name, "
       "encrypted_password, encrypted_password_len, auth_type, oauth_account_key, "
       "enabled, read_only, max_recent, max_body_chars, created_at "
       "FROM email_accounts WHERE id = ?",
       -1, &s_db.stmt_email_acct_get, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare email_acct_get failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT id, user_id, name, imap_server, imap_port, imap_ssl, "
       "smtp_server, smtp_port, smtp_ssl, username, display_name, "
       "encrypted_password, encrypted_password_len, auth_type, oauth_account_key, "
       "enabled, read_only, max_recent, max_body_chars, created_at "
       "FROM email_accounts WHERE user_id = ? ORDER BY name",
       -1, &s_db.stmt_email_acct_list, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare email_acct_list failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE email_accounts SET name=?, imap_server=?, imap_port=?, imap_ssl=?, "
       "smtp_server=?, smtp_port=?, smtp_ssl=?, username=?, display_name=?, "
       "encrypted_password=?, encrypted_password_len=?, auth_type=?, oauth_account_key=?, "
       "max_recent=?, max_body_chars=? WHERE id=?",
       -1, &s_db.stmt_email_acct_update, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare email_acct_update failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "DELETE FROM email_accounts WHERE id = ?", -1,
                           &s_db.stmt_email_acct_delete, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare email_acct_delete failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE email_accounts SET read_only = ? WHERE id = ?", -1,
                           &s_db.stmt_email_acct_set_read_only, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare email_acct_set_read_only failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   rc = sqlite3_prepare_v2(s_db.db, "UPDATE email_accounts SET enabled = ? WHERE id = ?", -1,
                           &s_db.stmt_email_acct_set_enabled, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: prepare email_acct_set_enabled failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }

   return AUTH_DB_SUCCESS;
}

void auth_db_finalize_statements(void) {
   if (s_db.stmt_create_user)
      sqlite3_finalize(s_db.stmt_create_user);
   if (s_db.stmt_get_user)
      sqlite3_finalize(s_db.stmt_get_user);
   if (s_db.stmt_count_users)
      sqlite3_finalize(s_db.stmt_count_users);
   if (s_db.stmt_inc_failed_attempts)
      sqlite3_finalize(s_db.stmt_inc_failed_attempts);
   if (s_db.stmt_reset_failed_attempts)
      sqlite3_finalize(s_db.stmt_reset_failed_attempts);
   if (s_db.stmt_update_last_login)
      sqlite3_finalize(s_db.stmt_update_last_login);
   if (s_db.stmt_set_lockout)
      sqlite3_finalize(s_db.stmt_set_lockout);

   if (s_db.stmt_create_session)
      sqlite3_finalize(s_db.stmt_create_session);
   if (s_db.stmt_get_session)
      sqlite3_finalize(s_db.stmt_get_session);
   if (s_db.stmt_update_session_activity)
      sqlite3_finalize(s_db.stmt_update_session_activity);
   if (s_db.stmt_delete_session)
      sqlite3_finalize(s_db.stmt_delete_session);
   if (s_db.stmt_delete_user_sessions)
      sqlite3_finalize(s_db.stmt_delete_user_sessions);
   if (s_db.stmt_delete_expired_sessions)
      sqlite3_finalize(s_db.stmt_delete_expired_sessions);

   if (s_db.stmt_count_recent_failures)
      sqlite3_finalize(s_db.stmt_count_recent_failures);
   if (s_db.stmt_log_attempt)
      sqlite3_finalize(s_db.stmt_log_attempt);
   if (s_db.stmt_delete_old_attempts)
      sqlite3_finalize(s_db.stmt_delete_old_attempts);

   if (s_db.stmt_log_event)
      sqlite3_finalize(s_db.stmt_log_event);
   if (s_db.stmt_delete_old_logs)
      sqlite3_finalize(s_db.stmt_delete_old_logs);

   if (s_db.stmt_get_user_settings)
      sqlite3_finalize(s_db.stmt_get_user_settings);
   if (s_db.stmt_set_user_settings)
      sqlite3_finalize(s_db.stmt_set_user_settings);

   /* Conversation statements */
   if (s_db.stmt_conv_create)
      sqlite3_finalize(s_db.stmt_conv_create);
   if (s_db.stmt_conv_get)
      sqlite3_finalize(s_db.stmt_conv_get);
   if (s_db.stmt_conv_list)
      sqlite3_finalize(s_db.stmt_conv_list);
   if (s_db.stmt_conv_list_all)
      sqlite3_finalize(s_db.stmt_conv_list_all);
   if (s_db.stmt_conv_search)
      sqlite3_finalize(s_db.stmt_conv_search);
   if (s_db.stmt_conv_search_content)
      sqlite3_finalize(s_db.stmt_conv_search_content);
   if (s_db.stmt_conv_rename)
      sqlite3_finalize(s_db.stmt_conv_rename);
   if (s_db.stmt_conv_delete)
      sqlite3_finalize(s_db.stmt_conv_delete);
   if (s_db.stmt_conv_delete_admin)
      sqlite3_finalize(s_db.stmt_conv_delete_admin);
   if (s_db.stmt_conv_count)
      sqlite3_finalize(s_db.stmt_conv_count);
   if (s_db.stmt_msg_add)
      sqlite3_finalize(s_db.stmt_msg_add);
   if (s_db.stmt_msg_get)
      sqlite3_finalize(s_db.stmt_msg_get);
   if (s_db.stmt_msg_get_admin)
      sqlite3_finalize(s_db.stmt_msg_get_admin);
   if (s_db.stmt_conv_update_meta)
      sqlite3_finalize(s_db.stmt_conv_update_meta);
   if (s_db.stmt_conv_update_context)
      sqlite3_finalize(s_db.stmt_conv_update_context);
   if (s_db.stmt_conv_create_origin)
      sqlite3_finalize(s_db.stmt_conv_create_origin);
   if (s_db.stmt_conv_reassign)
      sqlite3_finalize(s_db.stmt_conv_reassign);

   /* Session metrics statements */
   if (s_db.stmt_metrics_save)
      sqlite3_finalize(s_db.stmt_metrics_save);
   if (s_db.stmt_metrics_update)
      sqlite3_finalize(s_db.stmt_metrics_update);
   if (s_db.stmt_metrics_delete_old)
      sqlite3_finalize(s_db.stmt_metrics_delete_old);
   if (s_db.stmt_provider_metrics_save)
      sqlite3_finalize(s_db.stmt_provider_metrics_save);
   if (s_db.stmt_provider_metrics_delete)
      sqlite3_finalize(s_db.stmt_provider_metrics_delete);

   /* Image statements */
   if (s_db.stmt_image_create)
      sqlite3_finalize(s_db.stmt_image_create);
   if (s_db.stmt_image_get)
      sqlite3_finalize(s_db.stmt_image_get);
   if (s_db.stmt_image_get_file)
      sqlite3_finalize(s_db.stmt_image_get_file);
   if (s_db.stmt_image_delete)
      sqlite3_finalize(s_db.stmt_image_delete);
   if (s_db.stmt_image_update_access)
      sqlite3_finalize(s_db.stmt_image_update_access);
   if (s_db.stmt_image_update_retention)
      sqlite3_finalize(s_db.stmt_image_update_retention);
   if (s_db.stmt_image_count_user)
      sqlite3_finalize(s_db.stmt_image_count_user);
   if (s_db.stmt_image_delete_old)
      sqlite3_finalize(s_db.stmt_image_delete_old);
   if (s_db.stmt_image_cache_total_size)
      sqlite3_finalize(s_db.stmt_image_cache_total_size);
   if (s_db.stmt_image_delete_cache_lru)
      sqlite3_finalize(s_db.stmt_image_delete_cache_lru);
   if (s_db.stmt_image_get_expired_ids)
      sqlite3_finalize(s_db.stmt_image_get_expired_ids);
   if (s_db.stmt_image_get_cache_lru_ids)
      sqlite3_finalize(s_db.stmt_image_get_cache_lru_ids);
   if (s_db.stmt_image_stats)
      sqlite3_finalize(s_db.stmt_image_stats);

   /* Memory statements */
   if (s_db.stmt_memory_fact_create)
      sqlite3_finalize(s_db.stmt_memory_fact_create);
   if (s_db.stmt_memory_fact_get)
      sqlite3_finalize(s_db.stmt_memory_fact_get);
   if (s_db.stmt_memory_fact_list)
      sqlite3_finalize(s_db.stmt_memory_fact_list);
   if (s_db.stmt_memory_fact_search)
      sqlite3_finalize(s_db.stmt_memory_fact_search);
   if (s_db.stmt_memory_fact_search_bm25)
      sqlite3_finalize(s_db.stmt_memory_fact_search_bm25);
   if (s_db.stmt_memory_fact_search_bm25_since)
      sqlite3_finalize(s_db.stmt_memory_fact_search_bm25_since);
   if (s_db.stmt_memory_facts_fts_insert)
      sqlite3_finalize(s_db.stmt_memory_facts_fts_insert);
   if (s_db.stmt_memory_facts_fts_delete)
      sqlite3_finalize(s_db.stmt_memory_facts_fts_delete);
   if (s_db.stmt_memory_fact_update_access)
      sqlite3_finalize(s_db.stmt_memory_fact_update_access);
   if (s_db.stmt_memory_fact_update_confidence)
      sqlite3_finalize(s_db.stmt_memory_fact_update_confidence);
   if (s_db.stmt_memory_fact_supersede)
      sqlite3_finalize(s_db.stmt_memory_fact_supersede);
   if (s_db.stmt_memory_fact_delete)
      sqlite3_finalize(s_db.stmt_memory_fact_delete);
   if (s_db.stmt_memory_fact_find_similar)
      sqlite3_finalize(s_db.stmt_memory_fact_find_similar);
   if (s_db.stmt_memory_pref_upsert)
      sqlite3_finalize(s_db.stmt_memory_pref_upsert);
   if (s_db.stmt_memory_pref_get)
      sqlite3_finalize(s_db.stmt_memory_pref_get);
   if (s_db.stmt_memory_pref_list)
      sqlite3_finalize(s_db.stmt_memory_pref_list);
   if (s_db.stmt_memory_pref_search)
      sqlite3_finalize(s_db.stmt_memory_pref_search);
   if (s_db.stmt_memory_pref_delete)
      sqlite3_finalize(s_db.stmt_memory_pref_delete);
   if (s_db.stmt_memory_summary_create)
      sqlite3_finalize(s_db.stmt_memory_summary_create);
   if (s_db.stmt_memory_summary_list)
      sqlite3_finalize(s_db.stmt_memory_summary_list);
   if (s_db.stmt_memory_summary_mark_consolidated)
      sqlite3_finalize(s_db.stmt_memory_summary_mark_consolidated);
   if (s_db.stmt_memory_summary_search)
      sqlite3_finalize(s_db.stmt_memory_summary_search);

   /* Date-filtered memory statements */
   if (s_db.stmt_memory_fact_search_since)
      sqlite3_finalize(s_db.stmt_memory_fact_search_since);
   if (s_db.stmt_memory_summary_search_since)
      sqlite3_finalize(s_db.stmt_memory_summary_search_since);
   if (s_db.stmt_memory_fact_list_since)
      sqlite3_finalize(s_db.stmt_memory_fact_list_since);
   if (s_db.stmt_memory_summary_list_since)
      sqlite3_finalize(s_db.stmt_memory_summary_list_since);
   if (s_db.stmt_memory_fact_list_window_asc)
      sqlite3_finalize(s_db.stmt_memory_fact_list_window_asc);
   if (s_db.stmt_memory_fact_list_window_desc)
      sqlite3_finalize(s_db.stmt_memory_fact_list_window_desc);
   if (s_db.stmt_memory_summary_list_window_asc)
      sqlite3_finalize(s_db.stmt_memory_summary_list_window_asc);
   if (s_db.stmt_memory_summary_list_window_desc)
      sqlite3_finalize(s_db.stmt_memory_summary_list_window_desc);

   /* Category-filtered fact statements (v34) */
   if (s_db.stmt_memory_fact_update_category)
      sqlite3_finalize(s_db.stmt_memory_fact_update_category);
   if (s_db.stmt_memory_fact_list_general)
      sqlite3_finalize(s_db.stmt_memory_fact_list_general);
   if (s_db.stmt_memory_fact_count_general)
      sqlite3_finalize(s_db.stmt_memory_fact_count_general);

   /* Deduplication and pruning statements */
   if (s_db.stmt_memory_fact_find_by_hash)
      sqlite3_finalize(s_db.stmt_memory_fact_find_by_hash);
   if (s_db.stmt_memory_fact_prune_superseded)
      sqlite3_finalize(s_db.stmt_memory_fact_prune_superseded);
   if (s_db.stmt_memory_fact_prune_stale)
      sqlite3_finalize(s_db.stmt_memory_fact_prune_stale);
   if (s_db.stmt_memory_fact_prune_expired)
      sqlite3_finalize(s_db.stmt_memory_fact_prune_expired);

   /* Extraction tracking statements */
   if (s_db.stmt_conv_get_last_extracted)
      sqlite3_finalize(s_db.stmt_conv_get_last_extracted);
   if (s_db.stmt_conv_set_last_extracted)
      sqlite3_finalize(s_db.stmt_conv_set_last_extracted);

   /* Privacy statement */
   if (s_db.stmt_conv_set_private)
      sqlite3_finalize(s_db.stmt_conv_set_private);

   /* Auto-title statements */
   if (s_db.stmt_conv_auto_title)
      sqlite3_finalize(s_db.stmt_conv_auto_title);
   if (s_db.stmt_conv_set_title_locked)
      sqlite3_finalize(s_db.stmt_conv_set_title_locked);

   /* Embedding statements */
   if (s_db.stmt_memory_fact_update_embedding)
      sqlite3_finalize(s_db.stmt_memory_fact_update_embedding);
   if (s_db.stmt_memory_fact_get_embeddings)
      sqlite3_finalize(s_db.stmt_memory_fact_get_embeddings);
   if (s_db.stmt_memory_fact_list_without_embedding)
      sqlite3_finalize(s_db.stmt_memory_fact_list_without_embedding);
   if (s_db.stmt_memory_summary_update_embedding)
      sqlite3_finalize(s_db.stmt_memory_summary_update_embedding);
   if (s_db.stmt_memory_summary_scan_embeddings)
      sqlite3_finalize(s_db.stmt_memory_summary_scan_embeddings);
   if (s_db.stmt_memory_summary_list_without_embedding)
      sqlite3_finalize(s_db.stmt_memory_summary_list_without_embedding);

   /* Entity graph statements */
   if (s_db.stmt_memory_entity_upsert)
      sqlite3_finalize(s_db.stmt_memory_entity_upsert);
   if (s_db.stmt_memory_entity_get_by_name)
      sqlite3_finalize(s_db.stmt_memory_entity_get_by_name);
   if (s_db.stmt_memory_entity_update_embedding)
      sqlite3_finalize(s_db.stmt_memory_entity_update_embedding);
   if (s_db.stmt_memory_entity_get_embeddings)
      sqlite3_finalize(s_db.stmt_memory_entity_get_embeddings);
   if (s_db.stmt_memory_relation_create)
      sqlite3_finalize(s_db.stmt_memory_relation_create);
   if (s_db.stmt_memory_relation_close_open)
      sqlite3_finalize(s_db.stmt_memory_relation_close_open);
   if (s_db.stmt_memory_relation_list_by_subject)
      sqlite3_finalize(s_db.stmt_memory_relation_list_by_subject);
   if (s_db.stmt_memory_relation_list_by_subject_at)
      sqlite3_finalize(s_db.stmt_memory_relation_list_by_subject_at);
   if (s_db.stmt_memory_relation_list_by_object)
      sqlite3_finalize(s_db.stmt_memory_relation_list_by_object);
   if (s_db.stmt_memory_relation_fact_ids_for_entity)
      sqlite3_finalize(s_db.stmt_memory_relation_fact_ids_for_entity);
   if (s_db.stmt_memory_entity_search)
      sqlite3_finalize(s_db.stmt_memory_entity_search);
   if (s_db.stmt_memory_entity_delete)
      sqlite3_finalize(s_db.stmt_memory_entity_delete);
   if (s_db.stmt_memory_relation_delete_by_entity)
      sqlite3_finalize(s_db.stmt_memory_relation_delete_by_entity);

   /* Satellite mapping statements */
   if (s_db.stmt_satellite_upsert)
      sqlite3_finalize(s_db.stmt_satellite_upsert);
   if (s_db.stmt_satellite_get)
      sqlite3_finalize(s_db.stmt_satellite_get);
   if (s_db.stmt_satellite_delete)
      sqlite3_finalize(s_db.stmt_satellite_delete);
   if (s_db.stmt_satellite_update_user)
      sqlite3_finalize(s_db.stmt_satellite_update_user);
   if (s_db.stmt_satellite_update_location)
      sqlite3_finalize(s_db.stmt_satellite_update_location);
   if (s_db.stmt_satellite_set_enabled)
      sqlite3_finalize(s_db.stmt_satellite_set_enabled);
   if (s_db.stmt_satellite_update_last_seen)
      sqlite3_finalize(s_db.stmt_satellite_update_last_seen);
   if (s_db.stmt_satellite_list)
      sqlite3_finalize(s_db.stmt_satellite_list);

   /* Document search statements */
   if (s_db.stmt_doc_create)
      sqlite3_finalize(s_db.stmt_doc_create);
   if (s_db.stmt_doc_get)
      sqlite3_finalize(s_db.stmt_doc_get);
   if (s_db.stmt_doc_get_by_hash)
      sqlite3_finalize(s_db.stmt_doc_get_by_hash);
   if (s_db.stmt_doc_list)
      sqlite3_finalize(s_db.stmt_doc_list);
   if (s_db.stmt_doc_list_all)
      sqlite3_finalize(s_db.stmt_doc_list_all);
   if (s_db.stmt_doc_delete)
      sqlite3_finalize(s_db.stmt_doc_delete);
   if (s_db.stmt_doc_count_user)
      sqlite3_finalize(s_db.stmt_doc_count_user);
   if (s_db.stmt_doc_chunk_create)
      sqlite3_finalize(s_db.stmt_doc_chunk_create);
   if (s_db.stmt_doc_chunk_search)
      sqlite3_finalize(s_db.stmt_doc_chunk_search);
   if (s_db.stmt_doc_find_by_name)
      sqlite3_finalize(s_db.stmt_doc_find_by_name);
   if (s_db.stmt_doc_chunk_read)
      sqlite3_finalize(s_db.stmt_doc_chunk_read);
   if (s_db.stmt_doc_chunk_read_range)
      sqlite3_finalize(s_db.stmt_doc_chunk_read_range);
   if (s_db.stmt_doc_chunk_grep_ci)
      sqlite3_finalize(s_db.stmt_doc_chunk_grep_ci);
   if (s_db.stmt_doc_chunk_grep_cs)
      sqlite3_finalize(s_db.stmt_doc_chunk_grep_cs);
   if (s_db.stmt_doc_update_global)
      sqlite3_finalize(s_db.stmt_doc_update_global);
   if (s_db.stmt_doc_chunk_fts_insert)
      sqlite3_finalize(s_db.stmt_doc_chunk_fts_insert);
   if (s_db.stmt_doc_chunk_fts_delete)
      sqlite3_finalize(s_db.stmt_doc_chunk_fts_delete);
   if (s_db.stmt_doc_chunk_search_bm25)
      sqlite3_finalize(s_db.stmt_doc_chunk_search_bm25);
   if (s_db.stmt_doc_chunk_update)
      sqlite3_finalize(s_db.stmt_doc_chunk_update);

   /* Calendar statements */
   if (s_db.stmt_cal_acct_create)
      sqlite3_finalize(s_db.stmt_cal_acct_create);
   if (s_db.stmt_cal_acct_get)
      sqlite3_finalize(s_db.stmt_cal_acct_get);
   if (s_db.stmt_cal_acct_list)
      sqlite3_finalize(s_db.stmt_cal_acct_list);
   if (s_db.stmt_cal_acct_list_enabled)
      sqlite3_finalize(s_db.stmt_cal_acct_list_enabled);
   if (s_db.stmt_cal_acct_set_read_only)
      sqlite3_finalize(s_db.stmt_cal_acct_set_read_only);
   if (s_db.stmt_cal_acct_set_enabled)
      sqlite3_finalize(s_db.stmt_cal_acct_set_enabled);
   if (s_db.stmt_cal_acct_update)
      sqlite3_finalize(s_db.stmt_cal_acct_update);
   if (s_db.stmt_cal_acct_delete)
      sqlite3_finalize(s_db.stmt_cal_acct_delete);
   if (s_db.stmt_cal_acct_update_sync)
      sqlite3_finalize(s_db.stmt_cal_acct_update_sync);
   if (s_db.stmt_cal_acct_update_discovery)
      sqlite3_finalize(s_db.stmt_cal_acct_update_discovery);
   if (s_db.stmt_cal_cal_create)
      sqlite3_finalize(s_db.stmt_cal_cal_create);
   if (s_db.stmt_cal_cal_get)
      sqlite3_finalize(s_db.stmt_cal_cal_get);
   if (s_db.stmt_cal_cal_list)
      sqlite3_finalize(s_db.stmt_cal_cal_list);
   if (s_db.stmt_cal_cal_update_ctag)
      sqlite3_finalize(s_db.stmt_cal_cal_update_ctag);
   if (s_db.stmt_cal_cal_set_active)
      sqlite3_finalize(s_db.stmt_cal_cal_set_active);
   if (s_db.stmt_cal_cal_delete)
      sqlite3_finalize(s_db.stmt_cal_cal_delete);
   if (s_db.stmt_cal_cal_active_for_user)
      sqlite3_finalize(s_db.stmt_cal_cal_active_for_user);
   if (s_db.stmt_cal_evt_upsert)
      sqlite3_finalize(s_db.stmt_cal_evt_upsert);
   if (s_db.stmt_cal_evt_get_by_uid)
      sqlite3_finalize(s_db.stmt_cal_evt_get_by_uid);
   if (s_db.stmt_cal_evt_delete)
      sqlite3_finalize(s_db.stmt_cal_evt_delete);
   if (s_db.stmt_cal_evt_delete_by_cal)
      sqlite3_finalize(s_db.stmt_cal_evt_delete_by_cal);
   if (s_db.stmt_cal_occ_insert)
      sqlite3_finalize(s_db.stmt_cal_occ_insert);
   if (s_db.stmt_cal_occ_delete_for_event)
      sqlite3_finalize(s_db.stmt_cal_occ_delete_for_event);
   if (s_db.stmt_cal_occ_in_range)
      sqlite3_finalize(s_db.stmt_cal_occ_in_range);
   if (s_db.stmt_cal_occ_allday_in_range)
      sqlite3_finalize(s_db.stmt_cal_occ_allday_in_range);
   if (s_db.stmt_cal_occ_search)
      sqlite3_finalize(s_db.stmt_cal_occ_search);
   if (s_db.stmt_cal_occ_next)
      sqlite3_finalize(s_db.stmt_cal_occ_next);

   /* Contacts statements */
   if (s_db.stmt_contacts_find)
      sqlite3_finalize(s_db.stmt_contacts_find);
   if (s_db.stmt_contacts_add)
      sqlite3_finalize(s_db.stmt_contacts_add);
   if (s_db.stmt_contacts_delete)
      sqlite3_finalize(s_db.stmt_contacts_delete);
   if (s_db.stmt_contacts_list)
      sqlite3_finalize(s_db.stmt_contacts_list);
   if (s_db.stmt_contacts_update)
      sqlite3_finalize(s_db.stmt_contacts_update);
   if (s_db.stmt_contacts_count)
      sqlite3_finalize(s_db.stmt_contacts_count);

   /* Email account statements */
   if (s_db.stmt_email_acct_create)
      sqlite3_finalize(s_db.stmt_email_acct_create);
   if (s_db.stmt_email_acct_get)
      sqlite3_finalize(s_db.stmt_email_acct_get);
   if (s_db.stmt_email_acct_list)
      sqlite3_finalize(s_db.stmt_email_acct_list);
   if (s_db.stmt_email_acct_update)
      sqlite3_finalize(s_db.stmt_email_acct_update);
   if (s_db.stmt_email_acct_delete)
      sqlite3_finalize(s_db.stmt_email_acct_delete);
   if (s_db.stmt_email_acct_set_read_only)
      sqlite3_finalize(s_db.stmt_email_acct_set_read_only);
   if (s_db.stmt_email_acct_set_enabled)
      sqlite3_finalize(s_db.stmt_email_acct_set_enabled);

   /* OAuth statements */
   if (s_db.stmt_oauth_store)
      sqlite3_finalize(s_db.stmt_oauth_store);
   if (s_db.stmt_oauth_load)
      sqlite3_finalize(s_db.stmt_oauth_load);
   if (s_db.stmt_oauth_delete)
      sqlite3_finalize(s_db.stmt_oauth_delete);
   if (s_db.stmt_oauth_exists)
      sqlite3_finalize(s_db.stmt_oauth_exists);
   if (s_db.stmt_oauth_list_accounts)
      sqlite3_finalize(s_db.stmt_oauth_list_accounts);

   /* Clear all statement pointers using offsetof for safety
    * MAINTENANCE: If statements are reordered, update first/last_stmt names */
   size_t first_stmt_offset = offsetof(auth_db_state_t, stmt_create_user);
   size_t last_stmt_end = offsetof(auth_db_state_t, stmt_oauth_list_accounts) +
                          sizeof(sqlite3_stmt *);
   memset((char *)&s_db + first_stmt_offset, 0, last_stmt_end - first_stmt_offset);
}
