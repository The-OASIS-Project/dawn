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

#include "auth/auth_db_internal.h"
#include "auth/auth_db_tool_results.h"
#include "logging.h"
#include "memory/contacts_db.h"

/* =============================================================================
 * Prepared Statement Management
 *
 * Every cached statement is one entry in a table, which both prepares and
 * finalizes it, so a statement can't be prepared and never finalized.  The
 * count check below makes a statement field without an entry a build error.
 * ============================================================================= */

int auth_db_stmts_prepare(const auth_db_stmt_def_t *defs, size_t n) {
   for (size_t i = 0; i < n; i++) {
      if (sqlite3_prepare_v2(s_db.db, defs[i].sql, -1, defs[i].stmt, NULL) == SQLITE_OK)
         continue;
      if (!defs[i].optional) {
         OLOG_ERROR("auth_db: prepare %s failed: %s", defs[i].name, sqlite3_errmsg(s_db.db));
         return AUTH_DB_FAILURE;
      }
      OLOG_WARNING("auth_db: prepare %s failed (what uses it stays off): %s", defs[i].name,
                   sqlite3_errmsg(s_db.db));
      *defs[i].stmt = NULL;
   }
   return AUTH_DB_SUCCESS;
}

void auth_db_stmts_finalize(const auth_db_stmt_def_t *defs, size_t n) {
   for (size_t i = 0; i < n; i++) {
      sqlite3_finalize(*defs[i].stmt); /* a no-op on NULL */
      *defs[i].stmt = NULL;
   }
}

static const auth_db_stmt_def_t s_stmts[] = {

   /* User statements */
   { "create_user",
     "INSERT INTO users (username, password_hash, is_admin, created_at) VALUES (?, ?, ?, ?)",
     &s_db.stmt_create_user },
   { "get_user",
     "SELECT id, username, password_hash, is_admin, created_at, "
     "last_login, failed_attempts, lockout_until FROM users WHERE username = ?",
     &s_db.stmt_get_user },
   { "count_users", "SELECT COUNT(*) FROM users", &s_db.stmt_count_users },
   { "inc_failed_attempts",
     "UPDATE users SET failed_attempts = failed_attempts + 1 WHERE username = ?",
     &s_db.stmt_inc_failed_attempts },
   { "reset_failed_attempts", "UPDATE users SET failed_attempts = 0 WHERE username = ?",
     &s_db.stmt_reset_failed_attempts },
   { "update_last_login", "UPDATE users SET last_login = ? WHERE username = ?",
     &s_db.stmt_update_last_login },
   { "set_lockout", "UPDATE users SET lockout_until = ? WHERE username = ?",
     &s_db.stmt_set_lockout },

   /* Session statements */
   { "create_session",
     "INSERT INTO sessions (token, user_id, created_at, last_activity, "
     "expires_at, ip_address, user_agent) VALUES (?, ?, ?, ?, ?, ?, ?)",
     &s_db.stmt_create_session },
   { "get_session",
     "SELECT s.token, s.user_id, u.username, u.is_admin, s.created_at, "
     "s.last_activity, s.expires_at, s.ip_address, s.user_agent, "
     "s.keepalive_enabled "
     "FROM sessions s JOIN users u ON s.user_id = u.id WHERE s.token = ?",
     &s_db.stmt_get_session },

   /* Slide expires_at forward for a session-keepalive (always-on) session.
    * `MIN(?new_expires, created_at + ?cap)` clamps to the 30-day absolute lifetime
    * cap INSIDE the primitive (defense-in-depth: a future second caller can't write
    * an unbounded expiry even if it forgets to clamp). The WHERE guard
    * (expires_at IS NOT NULL AND expires_at >= ?now) prevents renewing an
    * already-expired or revoked-by-time row. Binds: 1=new_expires, 2=cap_seconds,
    * 3=token, 4=now. */
   { "renew_session",
     "UPDATE sessions SET expires_at = MIN(?, created_at + ?) "
     "WHERE token = ? AND expires_at IS NOT NULL AND expires_at >= ?",
     &s_db.stmt_renew_session },
   { "set_session_keepalive", "UPDATE sessions SET keepalive_enabled = ? WHERE token = ?",
     &s_db.stmt_set_session_keepalive },
   { "update_session_activity", "UPDATE sessions SET last_activity = ? WHERE token = ?",
     &s_db.stmt_update_session_activity },
   { "delete_session", "DELETE FROM sessions WHERE token = ?", &s_db.stmt_delete_session },
   { "delete_user_sessions", "DELETE FROM sessions WHERE user_id = ?",
     &s_db.stmt_delete_user_sessions },
   { "delete_expired_sessions",
     "DELETE FROM sessions WHERE expires_at IS NOT NULL AND expires_at < ?",
     &s_db.stmt_delete_expired_sessions },

   /* Rate limiting statements */
   { "count_recent_failures",
     "SELECT COUNT(*) FROM login_attempts WHERE ip_address = ? AND timestamp > ? AND success = 0",
     &s_db.stmt_count_recent_failures },
   { "log_attempt",
     "INSERT INTO login_attempts (ip_address, username, timestamp, success) VALUES (?, ?, ?, ?)",
     &s_db.stmt_log_attempt },
   { "delete_old_attempts", "DELETE FROM login_attempts WHERE timestamp < ?",
     &s_db.stmt_delete_old_attempts },

   /* Audit log statements */
   { "log_event",
     "INSERT INTO auth_log (timestamp, event, username, ip_address, details) "
     "VALUES (?, ?, ?, ?, ?)",
     &s_db.stmt_log_event },
   { "delete_old_logs", "DELETE FROM auth_log WHERE timestamp < ?", &s_db.stmt_delete_old_logs },

   /* User settings statements */
   { "get_user_settings",
     "SELECT persona_description, persona_mode, location, timezone, units, "
     "theme FROM user_settings WHERE user_id = ?",
     &s_db.stmt_get_user_settings },
   { "set_user_settings",
     "INSERT INTO user_settings (user_id, persona_description, persona_mode, location, timezone, "
     "units, theme, updated_at) "
     "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
     "ON CONFLICT(user_id) DO UPDATE SET "
     "persona_description=excluded.persona_description, persona_mode=excluded.persona_mode, "
     "location=excluded.location, timezone=excluded.timezone, units=excluded.units, "
     "theme=excluded.theme, updated_at=excluded.updated_at",
     &s_db.stmt_set_user_settings },

   /* Conversation statements.  anchor_date (v42) carries the conversation's
    * logical anchor timestamp so memory_extraction.c can resolve relative
    * temporal phrases.  Production passes time(NULL); bench passes session date. */
   { "conv_get",
     "SELECT id, user_id, title, created_at, updated_at, message_count, is_archived, "
     "context_tokens, context_max, continued_from, compaction_summary, "
     "llm_type, cloud_provider, model, tools_mode, thinking_mode, is_private, origin, "
     "reasoning_effort, context_watermark_msg_id, is_pinned "
     "FROM conversations WHERE id = ?",
     &s_db.stmt_conv_get },
   { "conv_list",
     "SELECT id, user_id, title, created_at, updated_at, message_count, is_archived, "
     "context_tokens, context_max, continued_from, compaction_summary, is_private, origin, "
     "is_pinned "
     /* job_status IS NULL: hide background-job conversations from the sidebar
      * (they are surfaced via the `job` tool / the Phase-2 jobs panel). */
     "FROM conversations WHERE user_id = ? AND (is_archived = 0 OR ? = 1) AND job_status IS NULL "
     "ORDER BY is_pinned DESC, updated_at DESC LIMIT ? OFFSET ?",
     &s_db.stmt_conv_list },

   /* Admin-only: list all conversations across all users */
   { "conv_list_all",
     "SELECT c.id, c.user_id, c.title, c.created_at, c.updated_at, c.message_count, "
     "c.is_archived, c.context_tokens, c.context_max, c.continued_from, "
     "c.compaction_summary, c.is_private, c.origin, u.username, c.is_pinned "
     "FROM conversations c LEFT JOIN users u ON c.user_id = u.id "
     "WHERE (c.is_archived = 0 OR ? = 1) "
     "ORDER BY c.updated_at DESC LIMIT ? OFFSET ?",
     &s_db.stmt_conv_list_all },
   { "conv_search",
     "SELECT id, user_id, title, created_at, updated_at, message_count, is_archived, "
     "context_tokens, context_max, continued_from, compaction_summary, is_private, origin "
     "FROM conversations WHERE user_id = ? AND title LIKE ? "
     "ORDER BY updated_at DESC LIMIT ? OFFSET ?",
     &s_db.stmt_conv_search },
   { "conv_search_content",
     "SELECT DISTINCT c.id, c.user_id, c.title, c.created_at, c.updated_at, "
     "c.message_count, c.is_archived, c.context_tokens, c.context_max, "
     "c.continued_from, c.compaction_summary, c.is_private, c.origin "
     "FROM conversations c "
     "INNER JOIN messages m ON m.conversation_id = c.id "
     "WHERE c.user_id = ? AND m.content LIKE ? AND m.kind IS NULL "
     "ORDER BY c.updated_at DESC LIMIT ? OFFSET ?",
     &s_db.stmt_conv_search_content },
   { "conv_rename", "UPDATE conversations SET title = ? WHERE id = ? AND user_id = ?",
     &s_db.stmt_conv_rename },
   { "conv_delete", "DELETE FROM conversations WHERE id = ? AND user_id = ?",
     &s_db.stmt_conv_delete },

   /* Admin-only: delete any conversation without ownership check */
   { "conv_delete_admin", "DELETE FROM conversations WHERE id = ?", &s_db.stmt_conv_delete_admin },
   { "conv_count", "SELECT COUNT(*) FROM conversations WHERE user_id = ?", &s_db.stmt_conv_count },

   /* Background-jobs completion drain — runs under this lock on the 1-Hz
    * heartbeat, so it is cached rather than prepared per tick (see
    * conv_db_job_list_pending_followups). */
   { "job_pending_followups",
     "SELECT " JOB_SELECT_COLS " FROM conversations "
     "WHERE job_status IN ('done','failed','interrupted') "
     "AND on_complete_fired=0 ORDER BY finished_at ASC LIMIT ?",
     &s_db.stmt_job_pending_followups },

   /* conversation_events append — the busiest new write path (fires per persisted
    * step event).  seq is assigned inside the INSERT under this lock; RETURNING
    * hands back the authoritative seq for the live fan-out frame.  See
    * conv_db_event_append for the full rationale. */
   { "event_append",
     "INSERT INTO conversation_events (conversation_id, seq, kind, payload, created_at) "
     "VALUES (?, (SELECT COALESCE(MAX(seq), 0) + 1 FROM conversation_events WHERE "
     "conversation_id = ?), ?, ?, ?) RETURNING seq",
     &s_db.stmt_event_append },
   { "msg_get",
     "SELECT m.id, m.conversation_id, m.role, m.content, m.tool_calls, m.tool_call_id, "
     "m.reasoning, m.created_at, m.is_error FROM messages m "
     "INNER JOIN conversations c ON m.conversation_id = c.id "
     "WHERE m.conversation_id = ? AND c.user_id = ? AND m.kind IS NULL ORDER BY m.id ASC",
     &s_db.stmt_msg_get },

   /* v67: same as stmt_msg_get but bounded to id > ? — the compaction-watermark
    * restore path (load only post-watermark messages). Full column set so tool /
    * reasoning rehydration works identically to the unbounded load. */
   { "msg_get_after",
     "SELECT m.id, m.conversation_id, m.role, m.content, m.tool_calls, m.tool_call_id, "
     "m.reasoning, m.created_at, m.is_error FROM messages m "
     "INNER JOIN conversations c ON m.conversation_id = c.id "
     "WHERE m.conversation_id = ? AND c.user_id = ? AND m.id > ? AND m.kind IS NULL "
     "ORDER BY m.id ASC",
     &s_db.stmt_msg_get_after },

   /* Admin-only: get messages without user ownership check */
   { "msg_get_admin",
     "SELECT id, conversation_id, role, content, tool_calls, tool_call_id, "
     "reasoning, created_at, "
     "is_error FROM messages WHERE conversation_id = ? AND kind IS NULL "
     "ORDER BY id ASC",
     &s_db.stmt_msg_get_admin },
   { "conv_update_meta",
     "UPDATE conversations SET updated_at = ?, message_count = message_count + 1 WHERE id = ?",
     &s_db.stmt_conv_update_meta },
   { "conv_update_context",
     "UPDATE conversations SET context_tokens = ?, context_max = ? "
     "WHERE id = ? AND user_id = ?",
     &s_db.stmt_conv_update_context },
   { "conv_create_origin",
     "INSERT INTO conversations (user_id, title, created_at, updated_at, origin, anchor_date, "
     "is_private) VALUES (?, ?, ?, ?, ?, ?, ?)",
     &s_db.stmt_conv_create_origin },
   { "conv_reassign", "UPDATE conversations SET user_id = ? WHERE id = ?",
     &s_db.stmt_conv_reassign },

   /* Session metrics statements - token usage is in separate provider table */
   { "metrics_save",
     "INSERT INTO session_metrics ("
     "session_id, user_id, session_type, started_at, ended_at, "
     "queries_total, queries_cloud, queries_local, errors_count, fallbacks_count, "
     "avg_asr_ms, avg_llm_ttft_ms, avg_llm_total_ms, avg_tts_ms, avg_pipeline_ms"
     ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
     &s_db.stmt_metrics_save },

   /* UPDATE statement for per-query metrics updates (id is param 16) */
   { "metrics_update",
     "UPDATE session_metrics SET "
     "ended_at = ?, queries_total = ?, queries_cloud = ?, queries_local = ?, "
     "errors_count = ?, fallbacks_count = ?, avg_asr_ms = ?, avg_llm_ttft_ms = ?, "
     "avg_llm_total_ms = ?, avg_tts_ms = ?, avg_pipeline_ms = ? "
     "WHERE id = ?",
     &s_db.stmt_metrics_update },
   { "metrics_delete_old", "DELETE FROM session_metrics WHERE started_at < ?",
     &s_db.stmt_metrics_delete_old },

   /* Provider metrics insert (child table) */
   { "provider_metrics_save",
     "INSERT INTO session_metrics_providers ("
     "session_metrics_id, provider, tokens_input, tokens_output, "
     "tokens_cached, queries"
     ") VALUES (?, ?, ?, ?, ?, ?)",
     &s_db.stmt_provider_metrics_save },

   /* Delete provider metrics before re-insert (for per-query updates) */
   { "provider_metrics_delete",
     "DELETE FROM session_metrics_providers WHERE session_metrics_id = ?",
     &s_db.stmt_provider_metrics_delete },

   /* Image statements (v30: filesystem-backed, no BLOB) */
   { "image_create",
     "INSERT INTO images (id, user_id, source, retention_policy, mime_type, size, filename, "
     "created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
     &s_db.stmt_image_create },
   { "image_get",
     "SELECT id, user_id, mime_type, size, filename, source, retention_policy, "
     "created_at, last_accessed FROM images WHERE id = ?",
     &s_db.stmt_image_get },
   { "image_get_file",
     "SELECT filename, user_id, source, mime_type, last_accessed FROM images WHERE id = ?",
     &s_db.stmt_image_get_file },
   { "image_delete", "DELETE FROM images WHERE id = ? AND user_id = ?", &s_db.stmt_image_delete },
   { "image_update_access", "UPDATE images SET last_accessed = ? WHERE id = ?",
     &s_db.stmt_image_update_access },
   { "image_update_retention",
     "UPDATE images SET retention_policy = ? "
     "WHERE id = ? AND (? = 0 OR user_id = ?)",
     &s_db.stmt_image_update_retention },
   { "image_count_user", "SELECT COUNT(*) FROM images WHERE user_id = ?",
     &s_db.stmt_image_count_user },
   { "image_count_user_source", "SELECT COUNT(*) FROM images WHERE user_id = ? AND source = ?",
     &s_db.stmt_image_count_user_source },
   { "image_delete_old",
     "DELETE FROM images WHERE retention_policy = 0 AND created_at < ? "
     "AND id IN (SELECT id FROM images WHERE retention_policy = 0 AND created_at < ? "
     "ORDER BY created_at ASC LIMIT 100)",
     &s_db.stmt_image_delete_old },
   { "image_cache_total_size",
     "SELECT COALESCE(SUM(size), 0) FROM images WHERE retention_policy = 2",
     &s_db.stmt_image_cache_total_size },
   { "image_delete_cache_lru", "DELETE FROM images WHERE id = ?",
     &s_db.stmt_image_delete_cache_lru },
   { "image_get_expired_ids",
     "SELECT id, filename FROM images WHERE retention_policy = 0 AND created_at < ? "
     "ORDER BY created_at ASC LIMIT 100",
     &s_db.stmt_image_get_expired_ids },
   { "image_get_cache_lru_ids",
     "SELECT id, filename, size FROM images WHERE retention_policy = 2 "
     "ORDER BY COALESCE(last_accessed, created_at) ASC",
     &s_db.stmt_image_get_cache_lru_ids },
   { "image_stats", "SELECT COUNT(*), COALESCE(SUM(size), 0) FROM images", &s_db.stmt_image_stats },

   /* Unbound images (retention 3: IMAGE_RETAIN_UNBOUND) no row named in time,
    * oldest first past the sweep's cursor (blob_store.h, get_orphan_ids). */
   { "image_get_unbound_ids",
     "SELECT id, filename, created_at, user_id FROM images "
     "WHERE retention_policy = 3 AND created_at < ?1 "
     "AND (created_at, id) > (?2, ?3) "
     "ORDER BY created_at ASC, id ASC LIMIT 100",
     &s_db.stmt_image_get_unbound_ids },

   /* One of those, deleted only while still unbound (the sweep asks what
    * holds them with the lock released; a row bound meanwhile stays). */
   { "image_delete_unbound", "DELETE FROM images WHERE id = ? AND retention_policy = 3",
     &s_db.stmt_image_delete_unbound },

   /* Generic blob store statements (v68).  Column/bind orders mirror the image
    * statements so blob_store.c's engine drives both: get->(id,user,mime,size,
    * filename,source,retention,created_at,last_accessed); get_file->(filename,
    * user,source,mime,last_accessed); create binds 1-8 shared + 9 content_hash
    * + 10 filename_original.  `kind` is the blob analogue of images.source. */
   { "blob_create",
     "INSERT INTO blobs (id, user_id, kind, retention_policy, mime_type, size, filename, "
     "created_at, content_hash, filename_original) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
     &s_db.stmt_blob_create },
   { "blob_get",
     "SELECT id, user_id, mime_type, size, filename, kind, retention_policy, "
     "created_at, last_accessed, filename_original FROM blobs WHERE id = ?",
     &s_db.stmt_blob_get },
   { "blob_get_file",
     "SELECT filename, user_id, kind, mime_type, last_accessed FROM blobs WHERE id = ?",
     &s_db.stmt_blob_get_file },
   { "blob_delete", "DELETE FROM blobs WHERE id = ? AND user_id = ?", &s_db.stmt_blob_delete },
   { "blob_update_access", "UPDATE blobs SET last_accessed = ? WHERE id = ?",
     &s_db.stmt_blob_update_access },
   { "blob_update_retention",
     "UPDATE blobs SET retention_policy = ? "
     "WHERE id = ? AND (? = 0 OR user_id = ?)",
     &s_db.stmt_blob_update_retention },
   { "blob_count_user", "SELECT COUNT(*) FROM blobs WHERE user_id = ?",
     &s_db.stmt_blob_count_user },
   { "blob_sum_bytes_user", "SELECT COALESCE(SUM(size), 0) FROM blobs WHERE user_id = ?",
     &s_db.stmt_blob_sum_bytes_user },
   { "blob_find_by_hash", "SELECT id FROM blobs WHERE user_id = ? AND content_hash = ?",
     &s_db.stmt_blob_find_by_hash },
   { "blob_delete_old",
     "DELETE FROM blobs WHERE retention_policy = 0 AND created_at < ? "
     "AND id IN (SELECT id FROM blobs WHERE retention_policy = 0 AND created_at < ? "
     "ORDER BY created_at ASC LIMIT 100)",
     &s_db.stmt_blob_delete_old },
   { "blob_cache_total_size", "SELECT COALESCE(SUM(size), 0) FROM blobs WHERE retention_policy = 2",
     &s_db.stmt_blob_cache_total_size },
   { "blob_delete_by_id", "DELETE FROM blobs WHERE id = ?", &s_db.stmt_blob_delete_by_id },
   { "blob_get_expired_ids",
     "SELECT id, filename FROM blobs WHERE retention_policy = 0 AND created_at < ? "
     "ORDER BY created_at ASC LIMIT 100",
     &s_db.stmt_blob_get_expired_ids },
   { "blob_get_cache_lru_ids",
     "SELECT id, filename, size FROM blobs WHERE retention_policy = 2 "
     "ORDER BY COALESCE(last_accessed, created_at) ASC",
     &s_db.stmt_blob_get_cache_lru_ids },
   { "blob_get_orphan_ids", /* An original is an orphan only if NOTHING references it: neither a
                             * library/indexed document (documents.original_blob_id) NOR a
                             * conversation message that embedded it as a chat attachment
                             * ("blob:<id>" marker in the message text).  Without the messages
                             * check, the sweep would reclaim a still-attached chat document after
                             * the grace window. */
     /* The messages predicate is an unindexable substring scan, but it runs
      * only for candidates (past grace, no documents ref) which is near-always
      * empty under keep-forever; revisit if messages crosses ~100k rows.  The
      * marker is "...bytes) blob:<id>]", so anchor the match with the trailing
      * ']' — blob ids are fixed-length validated tokens, so this can't match a
      * different blob, and the anchor avoids pinning a blob on stray prose.
      * This "blob:<id>]" marker is mirrored by the producer (src/webui/webui_attachments.c) and
      * parser (documents.js); kept in sync by scripts/check_blob_marker_sync.sh
      * — a drift here silently reclaims still-attached files (data loss).
      * Oldest first past the sweep's cursor (blob_store.h, get_orphan_ids). */
     "SELECT b.id, b.filename, b.created_at, b.user_id FROM blobs b "
     "WHERE b.kind = 0 AND b.retention_policy != 1 AND b.created_at < ?1 "
     "AND (b.created_at, b.id) > (?2, ?3) "
     "AND NOT EXISTS (SELECT 1 FROM documents d WHERE d.original_blob_id = b.id) "
     /* kind-rows: every row counts, so a file is kept while anything names it. */
     "AND NOT EXISTS (SELECT 1 FROM messages m WHERE m.content LIKE '%blob:' || b.id || ']%') "
     "ORDER BY b.created_at ASC, b.id ASC LIMIT 100",
     &s_db.stmt_blob_get_orphan_ids },
   { "blob_stats", "SELECT COUNT(*), COALESCE(SUM(size), 0) FROM blobs", &s_db.stmt_blob_stats },

   /* Memory fact statements.  category column appended last in all SELECTs (column 9)
    * to preserve existing column indices in populate_fact_from_row.
    * source_* columns (v40) are always bound last — NULL when no provenance. */
   { "memory_fact_create",
     "INSERT INTO memory_facts (user_id, fact_text, confidence, source, "
     "category, created_at, normalized_hash, "
     "source_conversation_id, source_msg_id_start, source_msg_id_end, "
     "origin_unsourced) "
     "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?8 IS NULL)",
     &s_db.stmt_memory_fact_create },

   /* CWE-639 defense-in-depth: SQL filters on (id, user_id) so a foreign
    * rowid cannot leak a fact owned by another user.  Same shape as
    * memory_db_fact_delete (`stmt_memory_fact_delete`) — wrong-user lookups
    * return zero rows (caller maps to MEMORY_DB_NOT_FOUND, same response a
    * legitimately-missing fact would get; no oracle). */
   { "memory_fact_get",
     "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
     "access_count, superseded_by, category, expires_at FROM memory_facts "
     "WHERE id = ? AND user_id = ?",
     &s_db.stmt_memory_fact_get },
   { "memory_fact_list",
     "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
     "access_count, superseded_by, category FROM memory_facts "
     "WHERE user_id = ?1 AND superseded_by IS NULL "
     "  AND (expires_at IS NULL OR expires_at >= ?4) "
     "ORDER BY confidence DESC LIMIT ?2 OFFSET ?3",
     &s_db.stmt_memory_fact_list },

   /* WebUI sort variants: identical projection + ?1..?4 numbering as
    * stmt_memory_fact_list so memory_db_fact_list_sorted's one bind block serves
    * all three.  The id tiebreak keeps OFFSET paging stable across same-second rows. */
   { "memory_fact_list_created_desc",
     "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
     "access_count, superseded_by, category FROM memory_facts "
     "WHERE user_id = ?1 AND superseded_by IS NULL "
     "  AND (expires_at IS NULL OR expires_at >= ?4) "
     "ORDER BY created_at DESC, id DESC LIMIT ?2 OFFSET ?3",
     &s_db.stmt_memory_fact_list_created_desc },
   { "memory_fact_list_created_asc",
     "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
     "access_count, superseded_by, category FROM memory_facts "
     "WHERE user_id = ?1 AND superseded_by IS NULL "
     "  AND (expires_at IS NULL OR expires_at >= ?4) "
     "ORDER BY created_at ASC, id ASC LIMIT ?2 OFFSET ?3",
     &s_db.stmt_memory_fact_list_created_asc },
   { "memory_fact_search",
     "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
     "access_count, superseded_by, category FROM memory_facts "
     "WHERE user_id = ?1 AND superseded_by IS NULL AND fact_text LIKE ?2 ESCAPE '\\' "
     "  AND (expires_at IS NULL OR expires_at >= ?4) "
     "ORDER BY confidence DESC LIMIT ?3",
     &s_db.stmt_memory_fact_search },

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
   { .name = "memory_fact_search_bm25",
     .sql = "SELECT mf.id, mf.user_id, mf.fact_text, mf.confidence, mf.source, mf.created_at, "
            "mf.last_accessed, mf.access_count, mf.superseded_by, mf.category, "
            "bm25(memory_facts_fts) AS score "
            "FROM memory_facts_fts "
            "JOIN memory_facts mf ON mf.id = memory_facts_fts.rowid "
            "WHERE memory_facts_fts MATCH ?1 "
            "  AND mf.user_id = ?2 "
            "  AND mf.superseded_by IS NULL "
            "  AND (mf.expires_at IS NULL OR mf.expires_at >= ?4) "
            "ORDER BY score ASC LIMIT ?3",
     .stmt = &s_db.stmt_memory_fact_search_bm25,
     .optional = true },

   /* v48 with `since_ts` filter — same as above but with `AND mf.created_at >= ?`
    * appended.  Used by focus-injection adapters and any time-windowed
    * memory.search query.  Without this, those callers would silently fall
    * back to the legacy multi-LIKE path and the BM25 A/B bench would be
    * dishonest for the windowed-query subset. */
   { .name = "memory_fact_search_bm25_since",
     .sql = "SELECT mf.id, mf.user_id, mf.fact_text, mf.confidence, mf.source, mf.created_at, "
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
     .stmt = &s_db.stmt_memory_fact_search_bm25_since,
     .optional = true },

   /* v48: FTS5 maintenance — insert/delete are app-driven (no SQL trigger
    * because stemming runs in C).  Update is implemented as delete + insert
    * by the caller; fact_text is immutable post-create in practice (changes
    * go through supersede), but the helper exists for defensive sync.
    * Same soft-failure policy as the search statement above. */
   { .name = "memory_facts_fts_insert",
     .sql = "INSERT INTO memory_facts_fts(rowid, fact_stems) VALUES (?, ?)",
     .stmt = &s_db.stmt_memory_facts_fts_insert,
     .optional = true },

   /* v89: record a conversation a fact was learned from.  Soft: until the
    * migration has run the table is missing and sources simply aren't recorded. */
   { .name = "memory_fact_source_add",
     .sql = "INSERT OR IGNORE INTO memory_fact_sources (fact_id, conversation_id) "
            "SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM conversations WHERE "
            "id = ?2)",
     .stmt = &s_db.stmt_memory_fact_source_add,
     .optional = true },
   { .name = "memory_relation_source_add",
     .sql = "INSERT OR IGNORE INTO memory_relation_sources (relation_id, "
            "conversation_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM "
            "conversations WHERE id = ?2)",
     .stmt = &s_db.stmt_memory_relation_source_add,
     .optional = true },
   { .name = "memory_pref_source_add",
     .sql = "INSERT OR IGNORE INTO memory_preference_sources (preference_id, "
            "conversation_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM "
            "conversations WHERE id = ?2)",
     .stmt = &s_db.stmt_memory_pref_source_add,
     .optional = true },
   { "memory_pref_current",
     "SELECT id, value FROM memory_preferences WHERE user_id = ? AND "
     "category = ?",
     &s_db.stmt_memory_pref_current },
   { .name = "memory_pref_sources_clear",
     .sql = "DELETE FROM memory_preference_sources WHERE preference_id = ?",
     .stmt = &s_db.stmt_memory_pref_sources_clear,
     .optional = true },
   { .name = "doc_chunk_generation",
     .sql = "SELECT COALESCE((SELECT gen FROM doc_chunk_generation WHERE owner = "
            "?1), 0), COALESCE((SELECT gen FROM doc_chunk_generation WHERE "
            "owner = 0), 0)",
     .stmt = &s_db.stmt_doc_chunk_generation,
     .optional = true },

   /* Contentless FTS5 requires the 'delete' command rather than DELETE FROM
    * (which would leave the index out of sync because there's no content
    * column to read the prior value from). */
   { .name = "memory_facts_fts_delete",
     .sql = "INSERT INTO memory_facts_fts(memory_facts_fts, rowid, fact_stems) "
            "VALUES('delete', ?, ?)",
     .stmt = &s_db.stmt_memory_facts_fts_delete,
     .optional = true },

   /* Per-fact category UPDATE used by the centroid backfill pass (v34).
    * CWE-639 defense-in-depth: SQL filters on (id, user_id) so a foreign
    * rowid cannot overwrite another user's category. */
   { "memory_fact_update_category",
     "UPDATE memory_facts SET category = ? WHERE id = ? AND user_id = ?",
     &s_db.stmt_memory_fact_update_category },
   { "memory_fact_list_general",
     "SELECT id, user_id, fact_text, confidence, source, "
     "  created_at, last_accessed, access_count, superseded_by, category "
     "FROM memory_facts "
     "WHERE user_id = ? AND superseded_by IS NULL "
     "  AND category = 'general' AND id > ? "
     "ORDER BY id ASC LIMIT ?",
     &s_db.stmt_memory_fact_list_general },
   { "memory_fact_count_general",
     "SELECT COUNT(*) FROM memory_facts "
     "WHERE user_id = ? AND superseded_by IS NULL "
     "  AND category = 'general'",
     &s_db.stmt_memory_fact_count_general },
   { "memory_fact_update_access",
     "UPDATE memory_facts SET last_accessed = ?,"
     "  access_count = access_count + 1,"
     "  confidence = CASE"
     "    WHEN (CAST(strftime('%s','now') AS REAL) - last_accessed) > 3600"
     "    THEN MIN(1.0, confidence + ?)"
     "    ELSE confidence"
     "  END "
     "WHERE id = ? AND user_id = ?",
     &s_db.stmt_memory_fact_update_access },

   /* Citation-driven confidence reinforcement (Memory Citation Phase 2).
    * Bumps confidence (ceilinged at 1.0) when the model actually CITED this fact,
    * gated by a 1 h cooldown on last_cited — a SEPARATE column from last_accessed,
    * because the recall render path stamps last_accessed = now on every surfaced
    * fact seconds before the citation, so a shared cooldown would never fire.
    * The cooldown lives in the WHERE (not a CASE) so the row is touched ONLY when
    * eligible: the hour is measured from the last BUMP, not the last attempt, and a
    * NULL last_cited (never cited) always bumps on the first citation.  Binds:
    * 1=boost, 2=id, 3=user_id.  (id, user_id) filter = CWE-639 defense-in-depth. */
   { "memory_fact_reinforce_citation",
     "UPDATE memory_facts SET"
     "  confidence = MIN(1.0, confidence + ?),"
     "  last_cited = CAST(strftime('%s','now') AS INTEGER) "
     "WHERE id = ? AND user_id = ?"
     "  AND (last_cited IS NULL"
     "   OR (CAST(strftime('%s','now') AS REAL) - last_cited) > 3600)",
     &s_db.stmt_memory_fact_reinforce_citation },

   /* CWE-639 defense-in-depth: SQL filters on (id, user_id) so a foreign
    * rowid cannot bump confidence on another user's fact. */
   { "memory_fact_update_confidence",
     "UPDATE memory_facts SET confidence = ? WHERE id = ? AND user_id = ?",
     &s_db.stmt_memory_fact_update_confidence },

   /* CWE-639 defense-in-depth: SQL filters on (id, user_id) for the
    * superseded row AND requires the supersedes-row to be owned by the
    * same user (EXISTS subquery).  A foreign new_fact_id would otherwise
    * let a caller "hide" another user's fact from their own retrieval by
    * pointing superseded_by at a foreign row, AND a foreign old_fact_id
    * would let a caller corrupt another user's fact chain. */
   { "memory_fact_supersede",
     "UPDATE memory_facts SET superseded_by = ?, "
     "superseded_at = CAST(strftime('%s', 'now') AS INTEGER) "
     "WHERE id = ? AND user_id = ? "
     "AND EXISTS (SELECT 1 FROM memory_facts WHERE id = ? AND user_id = ?)",
     &s_db.stmt_memory_fact_supersede },
   { "memory_fact_delete", "DELETE FROM memory_facts WHERE id = ? AND user_id = ?",
     &s_db.stmt_memory_fact_delete },

   /* note_doc_id IS NULL: exclude memory→note bridge glosses (v61).  This
    * statement backs extraction's like-match dedup AND the relation-supersede
    * old-fact lookup; a LIKE hit on a gloss could otherwise supersede it and
    * silently kill the bridge. */
   { "memory_fact_find_similar",
     "SELECT id, fact_text, confidence FROM memory_facts "
     "WHERE user_id = ? AND superseded_by IS NULL "
     "AND fact_text LIKE ? ESCAPE '\\' AND note_doc_id IS NULL "
     "ORDER BY confidence DESC LIMIT 5",
     &s_db.stmt_memory_fact_find_similar },
   { "memory_fact_find_by_hash",
     "SELECT id, fact_text, confidence FROM memory_facts "
     "WHERE user_id = ? AND normalized_hash = ? AND superseded_by IS NULL",
     &s_db.stmt_memory_fact_find_by_hash },
   { "memory_fact_prune_superseded", /* A retention window after the merge (superseded_at), never
                                      * after creation: an old fact merged today stays recoverable
                                      * for the window. Without a merge time it is kept. */
     "DELETE FROM memory_facts WHERE user_id = ? AND superseded_by IS NOT NULL "
     "AND superseded_at IS NOT NULL AND superseded_at < ?",
     &s_db.stmt_memory_fact_prune_superseded },

   /* note_doc_id IS NULL: bridge glosses live/die with their note (deleted via
    * the note-delete path), never by staleness pruning. */
   { "memory_fact_prune_stale",
     "DELETE FROM memory_facts WHERE user_id = ? AND superseded_by IS NULL "
     "AND last_accessed < ? AND confidence < ? AND note_doc_id IS NULL",
     &s_db.stmt_memory_fact_prune_stale },

   /* v58: hard-delete expired facts (the hard phase of fact ephemerality).
    * expires_at < cutoff, where the caller sets cutoff = now - prune_expired_days. */
   { "memory_fact_prune_expired",
     "DELETE FROM memory_facts WHERE user_id = ? "
     "AND expires_at IS NOT NULL AND expires_at < ?",
     &s_db.stmt_memory_fact_prune_expired },

   /* Memory preference statements.  source_* columns updated on every upsert
    * (latest-source-wins — mirrors the value/confidence overwrite). */
   { "memory_pref_upsert",
     "INSERT INTO memory_preferences (user_id, category, value, confidence, source, created_at, "
     "updated_at, source_conversation_id, source_msg_id_start, source_msg_id_end, "
     "origin_unsourced) "
     "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?8 IS NULL) "
     "ON CONFLICT(user_id, category) DO UPDATE SET "
     /* A new value comes only from where it was set; the same value keeps
      * every origin it had. */
     "origin_unsourced = CASE WHEN excluded.value != value THEN excluded.origin_unsourced "
     "ELSE MAX(origin_unsourced, excluded.origin_unsourced) END, "
     "value=excluded.value, confidence=excluded.confidence, updated_at=excluded.updated_at, "
     "source_conversation_id=excluded.source_conversation_id, "
     "source_msg_id_start=excluded.source_msg_id_start, "
     "source_msg_id_end=excluded.source_msg_id_end, "
     "reinforcement_count=reinforcement_count+1",
     &s_db.stmt_memory_pref_upsert },
   { "memory_pref_get",
     "SELECT id, user_id, category, value, confidence, source, created_at, updated_at, "
     "reinforcement_count FROM memory_preferences WHERE user_id = ? AND category = ?",
     &s_db.stmt_memory_pref_get },
   { "memory_pref_list",
     "SELECT id, user_id, category, value, confidence, source, created_at, updated_at, "
     "reinforcement_count FROM memory_preferences WHERE user_id = ? ORDER BY category "
     "LIMIT ? OFFSET ?",
     &s_db.stmt_memory_pref_list },
   { "memory_pref_search",
     "SELECT id, user_id, category, value, confidence, source, created_at, updated_at, "
     "reinforcement_count FROM memory_preferences "
     "WHERE user_id = ? AND (category LIKE ? ESCAPE '\\' OR value LIKE ? ESCAPE '\\') "
     "ORDER BY confidence DESC LIMIT ?",
     &s_db.stmt_memory_pref_search },
   { "memory_pref_delete", "DELETE FROM memory_preferences WHERE user_id = ? AND category = ?",
     &s_db.stmt_memory_pref_delete },

   /* Memory summary statements.  source_* appended (v40). */
   { "memory_summary_create",
     "INSERT INTO memory_summaries (user_id, session_id, summary, topics, sentiment, "
     "created_at, message_count, duration_seconds, "
     "source_conversation_id, source_msg_id_start, source_msg_id_end) "
     "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
     &s_db.stmt_memory_summary_create },
   { "memory_summary_list",
     "SELECT id, user_id, session_id, summary, topics, sentiment, created_at, "
     "message_count, duration_seconds, consolidated FROM memory_summaries "
     "WHERE user_id = ? AND consolidated = 0 ORDER BY created_at DESC LIMIT ? OFFSET ?",
     &s_db.stmt_memory_summary_list },

   /* WebUI sort variant (oldest first): same projection + positional params as
    * stmt_memory_summary_list.  DEFAULT/newest keeps using stmt_memory_summary_list. */
   { "memory_summary_list_created_asc",
     "SELECT id, user_id, session_id, summary, topics, sentiment, created_at, "
     "message_count, duration_seconds, consolidated FROM memory_summaries "
     "WHERE user_id = ? AND consolidated = 0 ORDER BY created_at ASC, id ASC LIMIT ? OFFSET ?",
     &s_db.stmt_memory_summary_list_created_asc },

   /* CWE-639 defense-in-depth: SQL filters on (id, user_id) so a foreign
    * rowid cannot mark another user's summary consolidated. */
   { "memory_summary_mark_consolidated",
     "UPDATE memory_summaries SET consolidated = 1 WHERE id = ? AND user_id = ?",
     &s_db.stmt_memory_summary_mark_consolidated },
   { "memory_summary_search",
     "SELECT id, user_id, session_id, summary, topics, sentiment, created_at, "
     "message_count, duration_seconds, consolidated FROM memory_summaries "
     "WHERE user_id = ? AND (summary LIKE ? ESCAPE '\\' OR topics LIKE ? ESCAPE '\\') "
     "ORDER BY created_at DESC LIMIT ?",
     &s_db.stmt_memory_summary_search },

   /* Date-filtered memory queries (for time_range search and fixed recent) */
   { "memory_fact_search_since",
     "SELECT id, user_id, fact_text, confidence, source, created_at, last_accessed, "
     "access_count, superseded_by, category FROM memory_facts "
     "WHERE user_id = ?1 AND superseded_by IS NULL AND fact_text LIKE ?2 ESCAPE '\\' "
     "AND created_at >= ?3 AND (expires_at IS NULL OR expires_at >= ?5) "
     "ORDER BY confidence DESC LIMIT ?4",
     &s_db.stmt_memory_fact_search_since },
   { "memory_summary_search_since",
     "SELECT id, user_id, session_id, summary, topics, sentiment, created_at, "
     "message_count, duration_seconds, consolidated FROM memory_summaries "
     "WHERE user_id = ? AND (summary LIKE ? ESCAPE '\\' OR topics LIKE ? ESCAPE '\\') "
     "AND created_at >= ? ORDER BY created_at DESC LIMIT ?",
     &s_db.stmt_memory_summary_search_since },

   /* Bundle 3 (2026-05-13) — windowed/sorted variants.  Shared WHERE clause
    * (user_id + created_at range) so the DESC variant collapses cleanly to
    * the legacy _list_since semantics when callers pass until=INT64_MAX. */
   { "memory_fact_list_window_asc",
     "SELECT id, user_id, fact_text, confidence, source, created_at, "
     "last_accessed, access_count, superseded_by, category FROM memory_facts "
     "WHERE user_id = ?1 AND superseded_by IS NULL "
     "  AND created_at >= ?2 AND created_at <= ?3 "
     "  AND (expires_at IS NULL OR expires_at >= ?5) "
     "ORDER BY created_at ASC LIMIT ?4",
     &s_db.stmt_memory_fact_list_window_asc },
   { "memory_fact_list_window_desc",
     "SELECT id, user_id, fact_text, confidence, source, created_at, "
     "last_accessed, access_count, superseded_by, category FROM memory_facts "
     "WHERE user_id = ?1 AND superseded_by IS NULL "
     "  AND created_at >= ?2 AND created_at <= ?3 "
     "  AND (expires_at IS NULL OR expires_at >= ?5) "
     "ORDER BY created_at DESC LIMIT ?4",
     &s_db.stmt_memory_fact_list_window_desc },
   { "memory_summary_list_window_asc",
     "SELECT id, user_id, session_id, summary, topics, sentiment, created_at, "
     "message_count, duration_seconds, consolidated FROM memory_summaries "
     "WHERE user_id = ? AND created_at >= ? AND created_at <= ? "
     "ORDER BY created_at ASC LIMIT ?",
     &s_db.stmt_memory_summary_list_window_asc },
   { "memory_summary_list_window_desc",
     "SELECT id, user_id, session_id, summary, topics, sentiment, created_at, "
     "message_count, duration_seconds, consolidated FROM memory_summaries "
     "WHERE user_id = ? AND created_at >= ? AND created_at <= ? "
     "ORDER BY created_at DESC LIMIT ?",
     &s_db.stmt_memory_summary_list_window_desc },

   /* Conversation extraction tracking statements */
   { "conv_get_last_extracted", "SELECT last_extracted_msg_count FROM conversations WHERE id = ?",
     &s_db.stmt_conv_get_last_extracted },

   /* Advances last_extracted_msg_id from the caller-supplied value rather than
    * re-querying MAX(id) at commit time — avoids TOCTOU when new messages arrive
    * during LLM inference.  CASE guard preserves the existing cursor when the
    * caller passes 0 (early-skip paths with no prov). */
   { "conv_set_last_extracted",
     "UPDATE conversations SET last_extracted_msg_count = ?, "
     "last_extracted_msg_id = CASE WHEN ? > 0 THEN ? ELSE last_extracted_msg_id END, "
     "extraction_attempts = 0, extraction_last_attempt_at = 0 "
     "WHERE id = ?",
     &s_db.stmt_conv_set_last_extracted },

   /* Conversation privacy statement */
   { "conv_set_private",
     "UPDATE conversations SET is_private = ? "
     "WHERE id = ? AND user_id = ?",
     &s_db.stmt_conv_set_private },

   /* Conversation pinned statement */
   { "conv_set_pinned",
     "UPDATE conversations SET is_pinned = ? "
     "WHERE id = ? AND user_id = ?",
     &s_db.stmt_conv_set_pinned },

   /* Auto-title statements */
   { "conv_auto_title",
     "UPDATE conversations SET title = ?, title_locked = 1, updated_at = ? "
     "WHERE id = ? AND user_id = ? AND title_locked = 0",
     &s_db.stmt_conv_auto_title },
   { "conv_set_title_locked",
     "UPDATE conversations SET title_locked = ? "
     "WHERE id = ? AND user_id = ?",
     &s_db.stmt_conv_set_title_locked },

   /* Embedding statements */
   { "memory_fact_update_embedding",
     "UPDATE memory_facts SET embedding = ?, embedding_norm = ? "
     "WHERE id = ? AND user_id = ?",
     &s_db.stmt_memory_fact_update_embedding },
   { "memory_fact_list_without_embedding",
     "SELECT id, fact_text FROM memory_facts "
     "WHERE user_id = ? AND superseded_by IS NULL AND id > ? "
     "AND fact_text != '' "
     "AND (embedding IS NULL OR length(embedding)/4 != ?) "
     "ORDER BY id ASC LIMIT ?",
     &s_db.stmt_memory_fact_list_without_embedding },

   /* Summary-embedding statements (v45).  No embedding_norm column on
    * memory_summaries — at the per-user summary scale (hundreds) we
    * recompute norms inside the scan instead of paying for the storage. */
   { "memory_summary_update_embedding",
     "UPDATE memory_summaries SET embedding = ? "
     "WHERE id = ? AND user_id = ?",
     &s_db.stmt_memory_summary_update_embedding },

   /* Scan-with-embeddings (id + blob only).  Cosine ranking in
    * memory_db_summary_search_semantic happens in two passes: this scan
    * ranks survivors by cosine without materialising the long summary /
    * topics text for every row; a second WHERE id = ? fetch inside the
    * helper pulls full rows for the top-K survivors only.  Same temporal
    * slice as the keyword search_since path (created_at >= ?).
    * ORDER BY created_at DESC means when max_scan eventually trips
    * (at >1k summaries per user), recency wins — the user is more likely
    * to query recent topics than year-old ones. */
   { "memory_summary_scan_embeddings",
     "SELECT id, embedding FROM memory_summaries "
     "WHERE user_id = ? AND created_at >= ? AND embedding IS NOT NULL "
     "ORDER BY created_at DESC LIMIT ?",
     &s_db.stmt_memory_summary_scan_embeddings },
   { "memory_summary_list_without_embedding",
     "SELECT id, summary FROM memory_summaries "
     "WHERE user_id = ? "
     "AND (embedding IS NULL OR length(embedding)/4 != ?) "
     "ORDER BY created_at ASC LIMIT ?",
     &s_db.stmt_memory_summary_list_without_embedding },

   /* Entity graph statements.  Three timestamp slots (?5 ?6 ?7) bound by
    * the caller; see memory_db_entity_upsert_at for the resolution rule. */
   { "memory_entity_upsert",
     "INSERT INTO memory_entities (user_id, name, entity_type, canonical_name, "
     "first_seen, last_seen, mention_count) "
     "VALUES (?, ?, ?, ?, ?, ?, 1) "
     "ON CONFLICT(user_id, canonical_name) DO UPDATE SET "
     "last_seen = ?, mention_count = mention_count + 1, "
     "name = CASE WHEN length(excluded.name) > length(name) THEN excluded.name ELSE name END "
     "RETURNING id, mention_count",
     &s_db.stmt_memory_entity_upsert },

   /* Equivalence-class aggregation (v43+): mention_count / first_seen /
    * last_seen are aggregated over {self + soft-aliases pointing at self}
    * via correlated subqueries.  For canonical rows this returns the
    * class total; for alias rows (canonical_id IS NOT NULL) it returns
    * self values, since aliases never have dependents (single-level rule
    * enforced by memory_db_alias_link's no-dependents check).  Idx
    * idx_memory_entities_canonical (partial) + PK back the subqueries
    * at O(log N). */
   { "memory_entity_get_by_name",
     "SELECT e.id, e.user_id, e.name, e.entity_type, e.canonical_name, "
     "  (SELECT COALESCE(SUM(mention_count), 0) FROM memory_entities "
     "     WHERE user_id = e.user_id AND (id = e.id OR canonical_id = e.id)), "
     "  (SELECT COALESCE(MIN(first_seen), 0) FROM memory_entities "
     "     WHERE user_id = e.user_id AND (id = e.id OR canonical_id = e.id)), "
     "  (SELECT COALESCE(MAX(last_seen), 0) FROM memory_entities "
     "     WHERE user_id = e.user_id AND (id = e.id OR canonical_id = e.id)) "
     "FROM memory_entities e "
     "WHERE e.user_id = ? AND e.canonical_name = ?",
     &s_db.stmt_memory_entity_get_by_name },
   { "memory_entity_update_embedding",
     "UPDATE memory_entities SET embedding = ?, embedding_norm = ? "
     "WHERE id = ? AND user_id = ?",
     &s_db.stmt_memory_entity_update_embedding },

   /* canonical_id IS NULL filter (v43): the entity-cache path defaults to
    * canonical-only.  Aliases (rows with canonical_id IS NOT NULL) are
    * excluded from the entity-embedding cache so the resolver / focus
    * adapter pools do not double-count surface-form variants of the same
    * real-world entity.  Bind position 2 = include_aliases (0 = filter
    * aliases out, 1 = include).  See docs/ENTITY_MERGE_DESIGN.md §15.
    * Position 4 = the embedding's size in bytes: only the current model's
    * embeddings count toward the LIMIT (after a model swap, rows not yet
    * recomputed would otherwise fill it). */
   { "memory_entity_get_embeddings",
     "SELECT id, name, entity_type, embedding, embedding_norm "
     "FROM memory_entities "
     "WHERE user_id = ?1 AND embedding IS NOT NULL "
     "  AND length(embedding) = ?4 "
     "  AND (?2 = 1 OR canonical_id IS NULL) "
     "ORDER BY mention_count DESC LIMIT ?3",
     &s_db.stmt_memory_entity_get_embeddings },

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
   { "memory_relation_create",
     "INSERT INTO memory_relations (user_id, subject_entity_id, relation, "
     "object_entity_id, object_value, fact_id, confidence, created_at, "
     "valid_from, valid_to, "
     "source_conversation_id, source_msg_id_start, source_msg_id_end, "
     "origin_unsourced) "
     "VALUES (?, ?, ?, ?, ?, ?, ?, strftime('%s','now'), ?, ?, ?, ?, ?, "
     "?10 IS NULL) "
     "ON CONFLICT(user_id, subject_entity_id, relation, "
     "            COALESCE(object_entity_id, 0), COALESCE(object_value, '')) "
     "WHERE valid_to IS NULL DO UPDATE SET "
     "  mention_count = mention_count + 1, "
     "  origin_unsourced = MAX(origin_unsourced, excluded.origin_unsourced), "
     "  confidence = MAX(confidence, excluded.confidence), "
     "  source_conversation_id = excluded.source_conversation_id, "
     "  source_msg_id_start = excluded.source_msg_id_start, "
     "  source_msg_id_end = excluded.source_msg_id_end, "
     "  fact_id = COALESCE(fact_id, excluded.fact_id) "
     "RETURNING id, mention_count",
     &s_db.stmt_memory_relation_create },

   /* Auto-close superseded exclusive relations (v33).  Used inside
    * memory_db_relation_supersede() before the new INSERT.  The (object_entity_id != ?
    * OR object_value != ?) clause skips when the user re-mentions the same target —
    * idempotency check.  COALESCE guards against NULL comparisons. */
   { "memory_relation_close_open",
     "UPDATE memory_relations SET valid_to = ? "
     "WHERE user_id = ? AND subject_entity_id = ? AND relation = ? "
     "  AND valid_to IS NULL "
     "  AND (COALESCE(object_entity_id, 0) != COALESCE(?, 0) "
     "    OR COALESCE(object_value, '') != COALESCE(?, '')) "
     "RETURNING fact_id",
     &s_db.stmt_memory_relation_close_open },
   { "memory_relation_list_by_subject",
     "SELECT r.id, r.subject_entity_id, r.relation, r.object_entity_id, "
     "COALESCE(e.name, r.object_value) AS object_name, r.confidence, "
     "COALESCE(r.valid_from, 0), COALESCE(r.valid_to, 0), "
     "r.mention_count "
     "FROM memory_relations r "
     "LEFT JOIN memory_entities e ON r.object_entity_id = e.id "
     "WHERE r.user_id = ? AND r.subject_entity_id = ? LIMIT ?",
     &s_db.stmt_memory_relation_list_by_subject },

   /* As-of variant: returns relations valid at the given timestamp.  Bounds:
    *   valid_from IS NULL or valid_from <= as_of
    *   valid_to   IS NULL or valid_to   >  as_of
    * Pass strftime('%s','now') as as_of for the "currently true" common case. */
   { "memory_relation_list_by_subject_at",
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
     &s_db.stmt_memory_relation_list_by_subject_at },
   { "memory_relation_list_by_object",
     "SELECT r.id, r.subject_entity_id, r.relation, r.object_entity_id, "
     "COALESCE(e.name, r.object_value) AS object_name, r.confidence, "
     "COALESCE(r.valid_from, 0), COALESCE(r.valid_to, 0), "
     "r.mention_count "
     "FROM memory_relations r "
     "LEFT JOIN memory_entities e ON r.subject_entity_id = e.id "
     "WHERE r.user_id = ? AND r.object_entity_id = ? LIMIT ?",
     &s_db.stmt_memory_relation_list_by_object },

   /* Graph-retrieval Phase 1A: return DISTINCT fact_ids for relations linking
    * @entity_id (as subject OR object).  Only returns relations with non-NULL
    * fact_id — the structured-only relations (NULL fact_id, ~60% of rows) are
    * Phase 1B's territory.  Sorted by confidence DESC so the highest-quality
    * graph-anchored facts surface first when the caller's fan-out cap trips. */
   { "memory_relation_fact_ids_for_entity",
     "SELECT DISTINCT r.fact_id "
     "FROM memory_relations r "
     "WHERE r.user_id = ? "
     "  AND (r.subject_entity_id = ? OR r.object_entity_id = ?) "
     "  AND r.fact_id IS NOT NULL "
     "ORDER BY r.confidence DESC, r.created_at DESC "
     "LIMIT ?",
     &s_db.stmt_memory_relation_fact_ids_for_entity },

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
   { "memory_entity_search",
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
     &s_db.stmt_memory_entity_search },
   { "memory_entity_delete", "DELETE FROM memory_entities WHERE id = ? AND user_id = ?",
     &s_db.stmt_memory_entity_delete },
   { "memory_entity_set_photo",
     "UPDATE memory_entities SET photo_id = ? "
     "WHERE id = ? AND user_id = ?",
     &s_db.stmt_memory_entity_set_photo },
   { "memory_entity_get_photo",
     "SELECT photo_id FROM memory_entities "
     "WHERE id = ? AND user_id = ?",
     &s_db.stmt_memory_entity_get_photo },
   { "memory_relation_delete_by_entity",
     "DELETE FROM memory_relations "
     "WHERE user_id = ? AND (subject_entity_id = ? OR object_entity_id = ?)",
     &s_db.stmt_memory_relation_delete_by_entity },

   /* Satellite mapping statements */
   { "satellite_upsert",
     "INSERT INTO satellite_mappings (uuid, name, location, ha_area, user_id, tier, "
     "last_seen, created_at, enabled) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
     "ON CONFLICT(uuid) DO UPDATE SET name=excluded.name, location=excluded.location, "
     "tier=excluded.tier, last_seen=excluded.last_seen",
     &s_db.stmt_satellite_upsert },
   { "satellite_get",
     "SELECT uuid, name, location, ha_area, user_id, tier, last_seen, created_at, enabled "
     "FROM satellite_mappings WHERE uuid = ?",
     &s_db.stmt_satellite_get },
   { "satellite_delete", "DELETE FROM satellite_mappings WHERE uuid = ?",
     &s_db.stmt_satellite_delete },
   { "satellite_update_user", "UPDATE satellite_mappings SET user_id = ? WHERE uuid = ?",
     &s_db.stmt_satellite_update_user },
   { "satellite_update_location",
     "UPDATE satellite_mappings SET location = ?, ha_area = ? WHERE uuid = ?",
     &s_db.stmt_satellite_update_location },
   { "satellite_set_enabled", "UPDATE satellite_mappings SET enabled = ? WHERE uuid = ?",
     &s_db.stmt_satellite_set_enabled },
   { "satellite_update_last_seen", "UPDATE satellite_mappings SET last_seen = ? WHERE uuid = ?",
     &s_db.stmt_satellite_update_last_seen },
   { "satellite_list",
     "SELECT uuid, name, location, ha_area, user_id, tier, last_seen, created_at, enabled "
     "FROM satellite_mappings ORDER BY name ASC",
     &s_db.stmt_satellite_list },

   /* Document search statements */
   { "doc_create",
     "INSERT INTO documents (user_id, filename, filepath, filetype, file_hash, "
     "num_chunks, is_global, created_at, original_blob_id) "
     "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
     &s_db.stmt_doc_create },
   { "doc_get",
     "SELECT id, user_id, filename, filepath, filetype, file_hash, "
     "num_chunks, is_global, created_at FROM documents WHERE id = ?",
     &s_db.stmt_doc_get },
   { "doc_get_by_hash",
     "SELECT id FROM documents WHERE file_hash = ? "
     "AND (user_id = ? OR is_global = 1)",
     &s_db.stmt_doc_get_by_hash },
   { "doc_list",
     "SELECT id, user_id, filename, filepath, filetype, file_hash, "
     "num_chunks, is_global, created_at FROM documents "
     "WHERE user_id = ? OR is_global = 1 ORDER BY created_at DESC "
     "LIMIT ? OFFSET ?",
     &s_db.stmt_doc_list },
   { "doc_list_all",
     "SELECT d.id, d.user_id, d.filename, d.filepath, d.filetype, "
     "d.file_hash, d.num_chunks, d.is_global, d.created_at, "
     "COALESCE(u.username, '') FROM documents d "
     "LEFT JOIN users u ON d.user_id = u.id "
     "ORDER BY d.created_at DESC LIMIT ? OFFSET ?",
     &s_db.stmt_doc_list_all },
   { "doc_update_global", "UPDATE documents SET is_global = ? WHERE id = ?",
     &s_db.stmt_doc_update_global },
   { "doc_delete", "DELETE FROM documents WHERE id = ?", &s_db.stmt_doc_delete },
   { "doc_count_user", "SELECT COUNT(*) FROM documents WHERE user_id = ?",
     &s_db.stmt_doc_count_user },

   /* doc_chunk_create: created_at appended last (col 6) — caller passes 0 for
    * unknown timestamps (older docs, manual ingests).  Schema default is 0. */
   { "doc_chunk_create",
     "INSERT INTO document_chunks (document_id, chunk_index, text, embedding, "
     "embedding_norm, created_at) VALUES (?, ?, ?, ?, ?, ?)",
     &s_db.stmt_doc_chunk_create },
   { "doc_find_by_name",
     "SELECT id, user_id, filename, filepath, filetype, file_hash, "
     "num_chunks, is_global, created_at "
     "FROM documents "
     "WHERE (user_id = ? OR is_global = 1) "
     "AND filename LIKE ? ESCAPE '\\' COLLATE NOCASE "
     "ORDER BY CASE WHEN LOWER(filename) = LOWER(?) "
     "THEN 0 ELSE 1 END, created_at DESC LIMIT 1",
     &s_db.stmt_doc_find_by_name },
   { "doc_chunk_read",
     "SELECT chunk_index, text FROM document_chunks "
     "WHERE document_id = ? ORDER BY chunk_index LIMIT ? OFFSET ?",
     &s_db.stmt_doc_chunk_read },

   /* doc_chunk_read_range: window a document's chunks by chunk_index value (NOT
    * row offset).  The index pipeline can skip a chunk_index on embed failure, so
    * OFFSET N != chunk at index N — this BETWEEN form fetches the true neighbors
    * for context expansion around a search/grep hit. */
   { "doc_chunk_read_range",
     "SELECT chunk_index, text FROM document_chunks "
     "WHERE document_id = ? AND chunk_index BETWEEN ? AND ? "
     "ORDER BY chunk_index",
     &s_db.stmt_doc_chunk_read_range },

   /* doc_chunk_grep (literal substring, permission-scoped).  Two shapes share the
    * same projection + paging: case-insensitive uses LIKE (ASCII-case-insensitive
    * by default; the needle is wildcard-escaped + wrapped in %..% by the caller),
    * case-sensitive uses instr() (literal, no escaping). LIMIT is bound to page+1
    * so the caller can detect "more matches exist" without a COUNT. */
   { "doc_chunk_grep_ci",
     "SELECT c.chunk_index, c.document_id, d.filename, d.num_chunks "
     "FROM document_chunks c JOIN documents d ON c.document_id = d.id "
     "WHERE (d.user_id = ? OR d.is_global = 1) "
     "AND c.text LIKE ? ESCAPE '\\' "
     "ORDER BY c.document_id, c.chunk_index LIMIT ? OFFSET ?",
     &s_db.stmt_doc_chunk_grep_ci },
   { "doc_chunk_grep_cs",
     "SELECT c.chunk_index, c.document_id, d.filename, d.num_chunks "
     "FROM document_chunks c JOIN documents d ON c.document_id = d.id "
     "WHERE (d.user_id = ? OR d.is_global = 1) "
     "AND instr(c.text, ?) > 0 "
     "ORDER BY c.document_id, c.chunk_index LIMIT ? OFFSET ?",
     &s_db.stmt_doc_chunk_grep_cs },

   /* === v61: document_chunks_fts (BM25 lexical channel) ===
    * Soft-fail (WARNING + NULL) like the memory_facts_fts statements: these
    * depend on the v61 virtual table, so a DB on which the migration hasn't yet
    * completed must still start — document search falls back to pure-semantic. */
   { .name = "doc_chunk_fts_insert",
     .sql = "INSERT INTO document_chunks_fts(rowid, label_stems, body_stems) "
            "VALUES (?, ?, ?)",
     .stmt = &s_db.stmt_doc_chunk_fts_insert,
     .optional = true },

   /* Contentless FTS5 requires the 'delete' command (no content column to read
    * the prior value from), so the original stems must be supplied. */
   { .name = "doc_chunk_fts_delete",
     .sql = "INSERT INTO document_chunks_fts(document_chunks_fts, rowid, "
            "label_stems, body_stems) VALUES('delete', ?, ?, ?)",
     .stmt = &s_db.stmt_doc_chunk_fts_delete,
     .optional = true },

   /* Column-weighted BM25 lexical candidate set (its OWN candidates — NOT a
    * boost over the semantic top-K).  ?1=MATCH expr, ?2=label weight,
    * ?3=body weight, ?4=user_id, ?5=limit.  bm25() is negative-for-relevant
    * (ORDER BY score ASC); the caller flips sign before sigmoid-normalizing.
    * Global-IDF caveat: per-user safety is the JOIN + (user_id=? OR is_global=1)
    * filter, not the index. */
   { .name = "doc_chunk_search_bm25",
     .sql = "SELECT c.id, c.chunk_index, c.document_id, d.filename, "
            "d.filetype, d.num_chunks, c.created_at, "
            "bm25(document_chunks_fts, ?2, ?3) AS score "
            "FROM document_chunks_fts "
            "JOIN document_chunks c ON c.id = document_chunks_fts.rowid "
            "JOIN documents d ON d.id = c.document_id "
            "WHERE document_chunks_fts MATCH ?1 "
            "AND (d.user_id = ?4 OR d.is_global = 1) "
            "ORDER BY score ASC LIMIT ?5",
     .stmt = &s_db.stmt_doc_chunk_search_bm25,
     .optional = true },

   /* Stable-id note edit: replace the single chunk's text + embedding in place
    * (document_chunks always exists, so this is hard-fail).  ?1=text,
    * ?2=embedding, ?3=embedding_norm, ?4=created_at, ?5=chunk id. */
   { "doc_chunk_update",
     "UPDATE document_chunks SET text = ?, embedding = ?, "
     "embedding_norm = ?, created_at = ? WHERE id = ?",
     &s_db.stmt_doc_chunk_update },

   /* === Calendar statements === */
   { "cal_acct_create",
     "INSERT INTO calendar_accounts (user_id, name, caldav_url, username, "
     "encrypted_password, auth_type, principal_url, calendar_home_url, enabled, "
     "last_sync, sync_interval_sec, created_at, read_only, oauth_account_key) "
     "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
     &s_db.stmt_cal_acct_create },
   { "cal_acct_get",
     "SELECT id, user_id, name, caldav_url, username, encrypted_password, "
     "auth_type, principal_url, calendar_home_url, enabled, last_sync, "
     "sync_interval_sec, created_at, read_only, oauth_account_key "
     "FROM calendar_accounts WHERE id = ?",
     &s_db.stmt_cal_acct_get },
   { "cal_acct_list",
     "SELECT id, user_id, name, caldav_url, username, encrypted_password, "
     "auth_type, principal_url, calendar_home_url, enabled, last_sync, "
     "sync_interval_sec, created_at, read_only, oauth_account_key "
     "FROM calendar_accounts "
     "WHERE user_id = ? ORDER BY name",
     &s_db.stmt_cal_acct_list },
   { "cal_acct_list_enabled",
     "SELECT id, user_id, name, caldav_url, username, encrypted_password, "
     "auth_type, principal_url, calendar_home_url, enabled, last_sync, "
     "sync_interval_sec, created_at, read_only, oauth_account_key "
     "FROM calendar_accounts "
     "WHERE enabled = 1 ORDER BY last_sync ASC",
     &s_db.stmt_cal_acct_list_enabled },
   { "cal_acct_update",
     "UPDATE calendar_accounts SET name=?, caldav_url=?, username=?, "
     "encrypted_password=?, auth_type=?, enabled=?, sync_interval_sec=? "
     "WHERE id=?",
     &s_db.stmt_cal_acct_update },
   { "cal_acct_delete", "DELETE FROM calendar_accounts WHERE id = ?", &s_db.stmt_cal_acct_delete },
   { "cal_acct_update_sync", "UPDATE calendar_accounts SET last_sync = ? WHERE id = ?",
     &s_db.stmt_cal_acct_update_sync },
   { "cal_acct_update_discovery",
     "UPDATE calendar_accounts SET principal_url = ?, "
     "calendar_home_url = ? WHERE id = ?",
     &s_db.stmt_cal_acct_update_discovery },
   { "cal_acct_set_read_only", "UPDATE calendar_accounts SET read_only = ? WHERE id = ?",
     &s_db.stmt_cal_acct_set_read_only },
   { "cal_acct_set_enabled", "UPDATE calendar_accounts SET enabled = ? WHERE id = ?",
     &s_db.stmt_cal_acct_set_enabled },
   { "cal_cal_create",
     "INSERT INTO calendar_calendars (account_id, caldav_path, display_name, "
     "color, is_active, ctag, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
     &s_db.stmt_cal_cal_create },
   { "cal_cal_get",
     "SELECT id, account_id, caldav_path, display_name, color, "
     "is_active, ctag, sync_token, created_at FROM calendar_calendars "
     "WHERE id = ?",
     &s_db.stmt_cal_cal_get },
   { "cal_cal_list",
     "SELECT id, account_id, caldav_path, display_name, color, "
     "is_active, ctag, sync_token, created_at FROM calendar_calendars "
     "WHERE account_id = ? ORDER BY display_name",
     &s_db.stmt_cal_cal_list },
   { "cal_cal_update_ctag", "UPDATE calendar_calendars SET ctag = ? WHERE id = ?",
     &s_db.stmt_cal_cal_update_ctag },
   { "cal_cal_update_sync_token", "UPDATE calendar_calendars SET sync_token = ? WHERE id = ?",
     &s_db.stmt_cal_cal_update_sync_token },
   { "cal_cal_set_active", "UPDATE calendar_calendars SET is_active = ? WHERE id = ?",
     &s_db.stmt_cal_cal_set_active },
   { "cal_cal_delete", "DELETE FROM calendar_calendars WHERE id = ?", &s_db.stmt_cal_cal_delete },
   { "cal_cal_active_for_user",
     "SELECT c.id, c.account_id, c.caldav_path, c.display_name, c.color, "
     "c.is_active, c.ctag, c.sync_token, c.created_at, a.read_only, "
     "a.name "
     "FROM calendar_calendars c "
     "JOIN calendar_accounts a ON c.account_id = a.id "
     "WHERE a.user_id = ? AND a.enabled = 1 AND c.is_active = 1 "
     "ORDER BY c.display_name",
     &s_db.stmt_cal_cal_active_for_user },
   { "cal_evt_upsert",
     "INSERT OR REPLACE INTO calendar_events (calendar_id, uid, etag, summary, "
     "description, location, dtstart, dtend, duration_sec, all_day, "
     "dtstart_date, dtend_date, rrule, raw_ical, last_synced, href) "
     "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
     &s_db.stmt_cal_evt_upsert },
   { "cal_evt_get_by_uid",
     "SELECT e.id, e.calendar_id, e.uid, e.etag, e.summary, e.description, "
     "e.location, e.dtstart, e.dtend, e.duration_sec, e.all_day, "
     "e.dtstart_date, e.dtend_date, e.rrule, e.raw_ical, e.last_synced, "
     "e.href "
     "FROM calendar_events e "
     "JOIN calendar_calendars c ON e.calendar_id = c.id "
     "JOIN calendar_accounts a ON c.account_id = a.id "
     "WHERE e.uid = ? AND a.user_id = ? LIMIT 1",
     &s_db.stmt_cal_evt_get_by_uid },
   { "cal_evt_delete", "DELETE FROM calendar_events WHERE id = ?", &s_db.stmt_cal_evt_delete },
   { "cal_evt_delete_by_cal", "DELETE FROM calendar_events WHERE calendar_id = ?",
     &s_db.stmt_cal_evt_delete_by_cal },
   { "cal_evt_delete_by_href", "DELETE FROM calendar_events WHERE calendar_id = ? AND href = ?",
     &s_db.stmt_cal_evt_delete_by_href },

   /* In-window deletion reconcile: after a complete time-range fetch re-stamps every
    * still-live in-window event's last_synced to the pass time, any event NOT re-stamped
    * whose OCCURRENCES fall in the window is gone upstream. Keyed on occurrence overlap
    * (not master dtstart), so it also catches a recurring series whose master predates
    * the window but whose occurrences are visible in it — matching exactly what the user
    * sees. The timed/all-day overlap predicates mirror the display range queries
    * (stmt_cal_occ_in_range / _allday_in_range). Keyed on last_synced, so it also prunes
    * pre-v82 empty-href ghosts and never touches out-of-window or just-created rows. */
   { "cal_evt_prune_window_stale",
     "DELETE FROM calendar_events WHERE calendar_id = ?1 AND last_synced < ?2 "
     "AND EXISTS (SELECT 1 FROM calendar_occurrences o "
     "            WHERE o.event_id = calendar_events.id "
     "              AND ((o.all_day = 0 AND o.dtstart < ?4 AND o.dtend > ?3) "
     "                OR (o.all_day = 1 AND o.dtstart_date < ?6 AND o.dtend_date > ?5)))",
     &s_db.stmt_cal_evt_prune_window_stale },

   /* Whole-collection retroactive prune: after a COMPLETE sync-collection baseline
    * enumeration yields the full current server href set, delete local rows for this
    * calendar whose (non-empty) href is not in that set — i.e. deleted upstream while
    * out of the fetch window, which the in-window sentinel above cannot see. Empty-href
    * rows (un-round-tripped / pre-v82) are left to the sentinel. */
   { "cal_evt_prune_not_in_hrefs",
     "DELETE FROM calendar_events WHERE calendar_id = ?1 AND href <> '' "
     "AND href NOT IN (SELECT value FROM json_each(?2))",
     &s_db.stmt_cal_evt_prune_not_in_hrefs },

   /* Blast-radius pre-check for the whole-collection prune: how many local non-empty-href
    * rows actually MATCH the server set. Zero overlap against a non-empty server set means
    * the local vs sync-collection href forms diverged (a future normalization drift) — the
    * NOT IN prune would then wipe the whole calendar, so the caller refuses instead. */
   { "cal_evt_count_href_in_set",
     "SELECT count(*) FROM calendar_events WHERE calendar_id = ?1 "
     "AND href <> '' AND href IN (SELECT value FROM json_each(?2))",
     &s_db.stmt_cal_evt_count_href_in_set },
   { "cal_occ_insert",
     "INSERT INTO calendar_occurrences (event_id, dtstart, dtend, all_day, "
     "dtstart_date, dtend_date, summary, location, is_override, is_cancelled, "
     "recurrence_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
     &s_db.stmt_cal_occ_insert },
   { "cal_occ_delete_for_event", "DELETE FROM calendar_occurrences WHERE event_id = ?",
     &s_db.stmt_cal_occ_delete_for_event },
   { "cal_occ_in_range",
     "SELECT o.id, o.event_id, o.dtstart, o.dtend, o.all_day, "
     "o.dtstart_date, o.dtend_date, o.summary, o.location, "
     "o.is_override, o.is_cancelled, o.recurrence_id, e.uid, "
     "e.calendar_id "
     "FROM calendar_occurrences o "
     "JOIN calendar_events e ON o.event_id = e.id "
     "WHERE e.calendar_id IN (SELECT value FROM json_each(?)) "
     "AND o.all_day = 0 AND o.is_cancelled = 0 "
     "AND o.dtstart < ? AND o.dtend > ? "
     "ORDER BY o.dtstart",
     &s_db.stmt_cal_occ_in_range },

   /* Each event's occurrence nearest to now: SQLite returns the other columns
    * from the row holding MIN() (its bare-column rule for a lone MIN/MAX).
    * All-day occurrences included: they carry the UTC midnight of their date
    * in dtstart/dtend, close enough to rank by distance. */
   { "cal_events_nearest",
     "SELECT o.id, o.event_id, o.dtstart, o.dtend, o.all_day, "
     "o.dtstart_date, o.dtend_date, o.summary, o.location, "
     "o.is_override, o.is_cancelled, o.recurrence_id, e.uid, "
     "e.calendar_id, MIN(ABS(o.dtstart - ?4)) AS dist "
     "FROM calendar_occurrences o "
     "JOIN calendar_events e ON o.event_id = e.id "
     "WHERE e.calendar_id IN (SELECT value FROM json_each(?1)) "
     "AND o.is_cancelled = 0 "
     "AND o.dtstart < ?3 AND o.dtend > ?2 "
     "GROUP BY o.event_id ORDER BY dist, o.id LIMIT ?5",
     &s_db.stmt_cal_events_nearest },
   { "cal_occ_allday_in_range",
     "SELECT o.id, o.event_id, o.dtstart, o.dtend, o.all_day, "
     "o.dtstart_date, o.dtend_date, o.summary, o.location, "
     "o.is_override, o.is_cancelled, o.recurrence_id, e.uid, "
     "e.calendar_id "
     "FROM calendar_occurrences o "
     "JOIN calendar_events e ON o.event_id = e.id "
     "WHERE e.calendar_id IN (SELECT value FROM json_each(?)) "
     "AND o.all_day = 1 AND o.is_cancelled = 0 "
     "AND o.dtstart_date < ? AND o.dtend_date > ? "
     "ORDER BY o.dtstart_date",
     &s_db.stmt_cal_occ_allday_in_range },
   { "cal_occ_search",
     "SELECT o.id, o.event_id, o.dtstart, o.dtend, o.all_day, "
     "o.dtstart_date, o.dtend_date, o.summary, o.location, "
     "o.is_override, o.is_cancelled, o.recurrence_id, e.uid "
     "FROM calendar_occurrences o "
     "JOIN calendar_events e ON o.event_id = e.id "
     "WHERE e.calendar_id IN (SELECT value FROM json_each(?)) "
     "AND o.is_cancelled = 0 "
     "AND (o.summary LIKE ? COLLATE NOCASE OR o.location LIKE ? COLLATE NOCASE) "
     "ORDER BY o.dtstart LIMIT ?",
     &s_db.stmt_cal_occ_search },
   { "cal_occ_next",
     "SELECT o.id, o.event_id, o.dtstart, o.dtend, o.all_day, "
     "o.dtstart_date, o.dtend_date, o.summary, o.location, "
     "o.is_override, o.is_cancelled, o.recurrence_id, e.uid "
     "FROM calendar_occurrences o "
     "JOIN calendar_events e ON o.event_id = e.id "
     "WHERE e.calendar_id IN (SELECT value FROM json_each(?)) "
     "AND o.all_day = 0 AND o.is_cancelled = 0 "
     "AND o.dtstart >= ? "
     "ORDER BY o.dtstart LIMIT 1",
     &s_db.stmt_cal_occ_next },

   /* OAuth token statements */
   { "oauth_store",
     "INSERT OR REPLACE INTO oauth_tokens "
     "(user_id, provider, account_key, encrypted_data, encrypted_data_len, "
     "scopes, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
     &s_db.stmt_oauth_store },
   { "oauth_load",
     "SELECT encrypted_data FROM oauth_tokens "
     "WHERE user_id = ? AND provider = ? AND account_key = ?",
     &s_db.stmt_oauth_load },
   { "oauth_delete",
     "DELETE FROM oauth_tokens "
     "WHERE user_id = ? AND provider = ? AND account_key = ?",
     &s_db.stmt_oauth_delete },
   { "oauth_exists",
     "SELECT COUNT(*) FROM oauth_tokens "
     "WHERE user_id = ? AND provider = ? AND account_key = ?",
     &s_db.stmt_oauth_exists },
   { "oauth_list_accounts",
     "SELECT account_key, scopes FROM oauth_tokens "
     "WHERE user_id = ? AND provider = ?",
     &s_db.stmt_oauth_list_accounts },

   /* === Contacts statements === */
   { "contacts_find", CONTACTS_FIND_SQL, &s_db.stmt_contacts_find },
   { "contacts_add",
     "INSERT INTO contacts (user_id, entity_id, field_type, value, label, created_at) "
     "SELECT ?, ?, ?, ?, ?, ? WHERE EXISTS "
     "(SELECT 1 FROM memory_entities WHERE id = ? AND user_id = ?)",
     &s_db.stmt_contacts_add },
   { "contacts_delete", "DELETE FROM contacts WHERE id = ? AND user_id = ?",
     &s_db.stmt_contacts_delete },
   { "contacts_list",
     "SELECT c.id, c.entity_id, e.name, e.canonical_name, c.field_type, c.value, c.label, "
     "e.photo_id FROM contacts c JOIN memory_entities e ON c.entity_id = e.id "
     "WHERE c.user_id = ? AND (? IS NULL OR c.field_type = ?) "
     "ORDER BY e.name LIMIT ? OFFSET ?",
     &s_db.stmt_contacts_list },
   { "contacts_update",
     "UPDATE contacts SET field_type = ?, value = ?, label = ? WHERE id = ? AND user_id = ?",
     &s_db.stmt_contacts_update },
   { "contacts_count", "SELECT COUNT(*) FROM contacts WHERE user_id = ?",
     &s_db.stmt_contacts_count },

   /* === Email account statements === */
   { "email_acct_create",
     "INSERT INTO email_accounts (user_id, name, imap_server, imap_port, imap_ssl, "
     "smtp_server, smtp_port, smtp_ssl, username, display_name, "
     "encrypted_password, encrypted_password_len, auth_type, oauth_account_key, "
     "enabled, read_only, max_recent, max_body_chars, created_at, digest_depth) "
     "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
     &s_db.stmt_email_acct_create },
   { "email_acct_get",
     "SELECT id, user_id, name, imap_server, imap_port, imap_ssl, "
     "smtp_server, smtp_port, smtp_ssl, username, display_name, "
     "encrypted_password, encrypted_password_len, auth_type, oauth_account_key, "
     "enabled, read_only, max_recent, max_body_chars, created_at, digest_depth "
     "FROM email_accounts WHERE id = ?",
     &s_db.stmt_email_acct_get },
   { "email_acct_list",
     "SELECT id, user_id, name, imap_server, imap_port, imap_ssl, "
     "smtp_server, smtp_port, smtp_ssl, username, display_name, "
     "encrypted_password, encrypted_password_len, auth_type, oauth_account_key, "
     "enabled, read_only, max_recent, max_body_chars, created_at, digest_depth "
     "FROM email_accounts WHERE user_id = ? ORDER BY name",
     &s_db.stmt_email_acct_list },
   { "email_acct_update",
     "UPDATE email_accounts SET name=?, imap_server=?, imap_port=?, imap_ssl=?, "
     "smtp_server=?, smtp_port=?, smtp_ssl=?, username=?, display_name=?, "
     "encrypted_password=?, encrypted_password_len=?, auth_type=?, oauth_account_key=?, "
     "max_recent=?, max_body_chars=?, digest_depth=? WHERE id=?",
     &s_db.stmt_email_acct_update },
   { "email_acct_delete", "DELETE FROM email_accounts WHERE id = ?", &s_db.stmt_email_acct_delete },
   { "email_acct_set_read_only", "UPDATE email_accounts SET read_only = ? WHERE id = ?",
     &s_db.stmt_email_acct_set_read_only },
   { "email_acct_set_enabled", "UPDATE email_accounts SET enabled = ? WHERE id = ?",
     &s_db.stmt_email_acct_set_enabled },
};

#define STMT_COUNT (sizeof(s_stmts) / sizeof(s_stmts[0]))
_Static_assert(STMT_COUNT + AUTH_DB_MESSAGES_STMT_COUNT == AUTH_DB_STMT_FIELDS,
               "every sqlite3_stmt field in auth_db_state_t needs one entry in s_stmts "
               "(or in auth_db_messages.c's table)");

int auth_db_prepare_statements(void) {
   if (auth_db_stmts_prepare(s_stmts, STMT_COUNT) != AUTH_DB_SUCCESS)
      return AUTH_DB_FAILURE;

   /* Message rows: the insert, the replay read and the compaction watermark. */
   if (auth_db_messages_prepare() != AUTH_DB_SUCCESS)
      return AUTH_DB_FAILURE;

   return AUTH_DB_SUCCESS;
}

void auth_db_finalize_statements(void) {
   tool_results_db_release_locked(); /* the store's own, kept on first use */
   auth_db_messages_finalize();
   auth_db_stmts_finalize(s_stmts, STMT_COUNT);
}
