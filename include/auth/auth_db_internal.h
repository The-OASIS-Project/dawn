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
 * Authentication Database Internal Header
 *
 * SECURITY: This header exposes internal database state and MUST NOT be
 * included by code outside the auth_db_*.c modules. Use auth/auth_db.h
 * for the public API.
 *
 * This header provides shared state and helper macros for the modularized
 * auth_db implementation (auth_db_core.c, auth_db_user.c, etc.).
 */

#ifndef AUTH_DB_INTERNAL_H
#define AUTH_DB_INTERNAL_H

/* Security guard - only auth_db modules should include this */
#ifndef AUTH_DB_INTERNAL_ALLOWED
#error "auth_db_internal.h is an internal header - include auth/auth_db.h instead"
#endif

#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "auth/auth_db.h"
#include "auth/auth_db_messages.h"
#include "core/hash_util.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

/* Canonical background-job column projection, in the order job_unpack_row()
 * (auth_db_jobs.c) reads.  Shared so the cached prepared statements in
 * auth_db_statements.c stay column-aligned with the readers. */
#define JOB_SELECT_COLS                                                                        \
   "id, user_id, parent_id, title, spawn_mode, on_complete, on_complete_fired, job_status, "   \
   "job_error, deliver_to, spawn_depth, reinvoke_count, started_at, finished_at, created_at, " \
   "origin, job_kind"

/* Current schema version.
 * NOTE: the schema version is GLOBAL and must advance uniformly across every
 * build config. The v64 (mcp_user_access) and v65 (code_projects) migrations run
 * UNCONDITIONALLY — their helpers are compiled into the unconditional
 * DAWN_SOURCES block and called from auth_db_apply_migrations(), never behind
 * DAWN_ENABLE_MCP_BRIDGE_TOOL / DAWN_ENABLE_CODE_PROJECTS. Gating them on a
 * feature flag would fork the schema timeline across binaries; do not do it.
 * (arch-A2) */
#define AUTH_DB_SCHEMA_VERSION 101

/* v90 llm_usage_log: in the base schema (created on every start) and repeated by
 * the v90 migration step, so the two can't drift.  The binding_* columns (v91)
 * are added to an existing table by the v91 step. */
#define LLM_USAGE_LOG_SCHEMA_SQL                        \
   "CREATE TABLE IF NOT EXISTS llm_usage_log ("         \
   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"            \
   "  created_at INTEGER NOT NULL,"                     \
   "  user_id INTEGER NOT NULL DEFAULT 0,"              \
   "  conversation_id INTEGER NOT NULL DEFAULT 0,"      \
   "  provider TEXT NOT NULL,"                          \
   "  model TEXT NOT NULL DEFAULT '',"                  \
   "  kind TEXT NOT NULL,"                              \
   "  iteration INTEGER NOT NULL DEFAULT -1,"           \
   "  prompt_tokens INTEGER NOT NULL DEFAULT 0,"        \
   "  cache_read_tokens INTEGER NOT NULL DEFAULT 0,"    \
   "  cache_write_tokens INTEGER NOT NULL DEFAULT 0,"   \
   "  uncached_tokens INTEGER NOT NULL DEFAULT 0,"      \
   "  output_tokens INTEGER NOT NULL DEFAULT 0,"        \
   "  expected_read INTEGER,"                           \
   "  cache_state TEXT,"                                \
   "  gap_ms INTEGER NOT NULL DEFAULT 0,"               \
   "  tools_hash INTEGER NOT NULL DEFAULT 0,"           \
   "  system_hash INTEGER NOT NULL DEFAULT 0,"          \
   "  thinking TEXT NOT NULL DEFAULT '',"               \
   "  images INTEGER NOT NULL DEFAULT 0,"               \
   "  cache_miss_reason TEXT NOT NULL DEFAULT '',"      \
   "  cache_missed_tokens INTEGER NOT NULL DEFAULT 0,"  \
   "  binding_reported INTEGER NOT NULL DEFAULT 0,"     \
   "  binding_prefix_drops INTEGER NOT NULL DEFAULT 0," \
   "  binding_model_drops INTEGER NOT NULL DEFAULT 0,"  \
   "  binding_other_drops INTEGER NOT NULL DEFAULT 0"   \
   ");"                                                 \
   "CREATE INDEX IF NOT EXISTS idx_llm_usage_log_time ON llm_usage_log(created_at);"

/* Retention periods */
#define LOGIN_ATTEMPT_RETENTION_SEC (7 * 24 * 60 * 60) /* 7 days */
#define AUTH_LOG_RETENTION_SEC (30 * 24 * 60 * 60)     /* 30 days */

/* Helper macro for stringifying values in SQL */
#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

/* messages.llm_blocks (v92): stored blocks belong to assistant rows only, stay
 * within CONV_LLM_BLOCKS_MAX bytes, and llm_blocks_len holds their byte length
 * (set together, cleared together).  The length column comes BEFORE the blocks
 * so a size or presence check never reads the blob's overflow pages.  Shared by
 * the base schema and the v92 ALTER so both build the same columns. */
/* Each comparison is guarded against NULL: a CHECK that evaluates to NULL
 * passes, so "len = length(blocks)" alone would let one column be set without
 * the other. */
#define CONV_LLM_BLOCKS_CHECK_SQL                                                  \
   "CHECK((llm_blocks IS NULL AND llm_blocks_len IS NULL) OR (role = 'assistant' " \
   "AND llm_blocks IS NOT NULL AND llm_blocks_len IS NOT NULL "                    \
   "AND llm_blocks_len = length(CAST(llm_blocks AS BLOB)) "                        \
   "AND llm_blocks_len <= " STRINGIFY(CONV_LLM_BLOCKS_MAX) "))"

/* messages.kind as v94 added it: a column CHECK.  Only the v94 ALTER uses it
 * (a database older than v94); v98 rebuilt messages without it and checks
 * kinds with triggers (CONV_MESSAGE_KIND_TRIGGERS_SQL), so a new kind needs
 * no table rebuild.  Frozen: never add a kind here. */
#define CONV_MESSAGE_KIND_CHECK_V94_SQL                                          \
   "CHECK(kind IS NULL OR (kind IN ('turn_context','memory','envelope') AND "    \
   "role = 'user') OR (kind = 'loop_note' AND role IN ('user','assistant')) OR " \
   "(kind IN ('directive','instruction') AND role = 'system'))"

/* Which role each kind of request-context row takes (v98), over the row a
 * trigger sees.  The one place the schema lists kinds: kept in step with
 * message_kind_role_ok() in include/core/message_kind.h (a test checks every
 * kind against every role).  Never NULL for a row with a role. */
#define CONV_MESSAGE_KIND_ROLE_OK_SQL                                                        \
   "(NEW.kind IS NULL OR (NEW.kind IN ('turn_context','memory','envelope') AND "             \
   "NEW.role = 'user') OR (NEW.kind = 'loop_note' AND NEW.role IN ('user','assistant')) OR " \
   "(NEW.kind IN ('directive','instruction','tool_change') AND NEW.role = 'system'))"

/* messages.images (v98): the images a tool result carried, as a JSON array of
 * image ids; on role='tool' rows only. */
#define CONV_MESSAGE_IMAGES_CHECK_SQL                                         \
   "CHECK(images IS NULL OR (role = 'tool' AND CASE WHEN json_valid(images) " \
   "THEN json_type(images) = 'array' ELSE 0 END))"

/* The messages table (v98).  The base schema (a new database) and the v98
 * rebuild (an existing one) both run this, so the two can't differ, down to
 * the stored text.  Column order matters to the reads: a turn's stored blocks
 * are the largest value a row holds and spill to overflow pages, so they come
 * last, after every column a filter or index reads (kind, context_of,
 * images); llm_blocks_len sits just before them, so a size or presence check
 * never reads the blob's overflow pages either. */
#define CONV_MESSAGES_TABLE_SQL                                                      \
   "CREATE TABLE IF NOT EXISTS messages ("                                           \
   "   id INTEGER PRIMARY KEY AUTOINCREMENT,"                                        \
   "   conversation_id INTEGER NOT NULL,"                                            \
   "   role TEXT NOT NULL CHECK(role IN ('system', 'user', 'assistant', 'tool')),"   \
   "   content TEXT NOT NULL,"                                                       \
   "   tool_calls TEXT,"                                                             \
   "   tool_call_id TEXT,"                                                           \
   "   reasoning TEXT,"                                                              \
   "   created_at INTEGER NOT NULL,"                                                 \
   "   is_error INTEGER NOT NULL DEFAULT 0,"                                         \
   "   kind TEXT DEFAULT NULL,"                                                      \
   "   context_of INTEGER DEFAULT NULL,"                                             \
   "   images TEXT DEFAULT NULL " CONV_MESSAGE_IMAGES_CHECK_SQL ","                  \
   "   llm_blocks_len INTEGER,"                                                      \
   "   llm_blocks TEXT " CONV_LLM_BLOCKS_CHECK_SQL ","                               \
   "   FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE" \
   ");"

/* messages' indexes and triggers, each defined once: the migration that made
 * it and the v98 rebuild run the same text, so a new database and a migrated
 * one store the same schema. */
#define CONV_MESSAGES_IDX_CONVERSATION_SQL \
   "CREATE INDEX IF NOT EXISTS idx_messages_conversation ON messages(conversation_id, id ASC);"
/* Rows holding blocks, for the watermark GC and the cleanup sweep (v92). */
#define CONV_MESSAGES_IDX_LLM_BLOCKS_SQL                             \
   "CREATE INDEX IF NOT EXISTS idx_messages_llm_blocks ON messages " \
   "(conversation_id, id) WHERE llm_blocks_len IS NOT NULL;"
/* Every display read (kind IS NULL) without reading `kind` from the row,
 * which sits after a turn's stored blocks (v94). */
#define CONV_MESSAGES_IDX_DISPLAY_SQL                                                   \
   "CREATE INDEX IF NOT EXISTS idx_messages_display ON messages (conversation_id, id) " \
   "WHERE kind IS NULL;"
/* A conversation's (or a user's) context rows of one kind (v94). */
#define CONV_MESSAGES_IDX_KIND_SQL                                                         \
   "CREATE INDEX IF NOT EXISTS idx_messages_kind ON messages (kind, conversation_id, id) " \
   "WHERE kind IS NOT NULL;"
/* Tool rows holding images (v98): a conversation's, for binding and
 * deleting them, and every one, for whether another conversation names an
 * image; few rows hold images, so either reads this index alone. */
#define CONV_MESSAGES_IDX_IMAGES_SQL                             \
   "CREATE INDEX IF NOT EXISTS idx_messages_images ON messages " \
   "(conversation_id, id) WHERE images IS NOT NULL;"
/* A row's stored blocks go when its text changes (v92). */
#define CONV_MESSAGES_LLM_BLOCKS_TRIGGER_SQL                                                \
   "CREATE TRIGGER IF NOT EXISTS messages_llm_blocks_on_edit "                              \
   "AFTER UPDATE OF content, tool_calls ON messages "                                       \
   "WHEN NEW.llm_blocks_len IS NOT NULL AND "                                               \
   "(NEW.content IS NOT OLD.content OR NEW.tool_calls IS NOT OLD.tool_calls) "              \
   "BEGIN UPDATE messages SET llm_blocks = NULL, llm_blocks_len = NULL WHERE id = NEW.id; " \
   "END;"
/* A kind fits its role (v98), on every write that sets either.  Each
 * trigger is dropped and made again, so running this refreshes them: a later
 * migration that adds a kind (to CONV_MESSAGE_KIND_ROLE_OK_SQL) re-runs
 * CONV_MESSAGES_OBJECTS_SQL and nothing else. */
#define CONV_MESSAGE_KIND_TRIGGERS_SQL \
   CONV_MESSAGE_KIND_TRIGGERS_WITH_SQL(CONV_MESSAGE_KIND_ROLE_OK_SQL)
/* The same over a given kind/role check (tests extend the set with it). */
#define CONV_MESSAGE_KIND_TRIGGERS_WITH_SQL(role_ok)                        \
   "DROP TRIGGER IF EXISTS messages_kind_on_insert;"                        \
   "CREATE TRIGGER messages_kind_on_insert BEFORE INSERT ON messages "      \
   "WHEN NOT " role_ok " BEGIN "                                            \
   "SELECT RAISE(ABORT, 'messages: kind does not fit the role'); END;"      \
   "DROP TRIGGER IF EXISTS messages_kind_on_update;"                        \
   "CREATE TRIGGER messages_kind_on_update BEFORE UPDATE OF kind, role ON " \
   "messages WHEN NOT " role_ok " BEGIN "                                   \
   "SELECT RAISE(ABORT, 'messages: kind does not fit the role'); END;"
/* All of them, for the v98 rebuild (and, as no-ops, a new database); re-run
 * whole by a migration that adds a kind. */
#define CONV_MESSAGES_OBJECTS_SQL                                                            \
   CONV_MESSAGES_IDX_CONVERSATION_SQL CONV_MESSAGES_IDX_LLM_BLOCKS_SQL                       \
       CONV_MESSAGES_IDX_DISPLAY_SQL CONV_MESSAGES_IDX_KIND_SQL CONV_MESSAGES_IDX_IMAGES_SQL \
           CONV_MESSAGES_LLM_BLOCKS_TRIGGER_SQL CONV_MESSAGE_KIND_TRIGGERS_SQL

/* The images a conversation names (v98): a tool row's captures, a question's
 * uploads; see auth_db_messages.h.  A superset index: a record is added when a
 * row naming the image is stored (or, for rows from before v98, by its
 * backfill) and is never removed while the conversation lives, even when the
 * row that named it is edited or deleted.  So it only grows until the
 * conversation is deleted: an image it lists may no longer be named by any of
 * the conversation's rows, never the reverse.  Records go with the
 * conversation or the image (FK cascades); a conversation delete takes the
 * images only it names.  Its tables are base-schema tables, so this is in the
 * base schema. */
#define AUTH_DB_CONVERSATION_IMAGES_SQL                                                  \
   "CREATE TABLE IF NOT EXISTS conversation_images ("                                    \
   "   image_id TEXT NOT NULL REFERENCES images(id) ON DELETE CASCADE,"                  \
   "   conversation_id INTEGER NOT NULL REFERENCES conversations(id) ON DELETE CASCADE," \
   "   PRIMARY KEY (image_id, conversation_id)"                                          \
   ") WITHOUT ROWID;"                                                                    \
   "CREATE INDEX IF NOT EXISTS idx_conversation_images_conv ON "                         \
   "conversation_images(conversation_id);"

/* Tool results kept whole while the model is shown a view of them (v97).
 * Readable only in the conversation that stored them (or, before it exists,
 * by the turn that stored them): see core/tool_result_store.h.  They go with
 * their conversation or user (FK cascades); unbound rows (no conversation
 * yet) are reclaimed after a grace.  body is the last column, so reading any
 * other never walks its overflow pages.  The indexes are in age order and
 * carry bytes, so eviction reads them alone.  tool_results_usage holds the
 * bytes per conversation (scope 0), per user (1) and in all (2, key 0), kept
 * by triggers (FK cascades fire them too), so a cap check is one lookup.
 * Shared by the base schema and the v97 migration. */
#define AUTH_DB_TOOL_RESULTS_SCHEMA_SQL                                                            \
   "CREATE TABLE IF NOT EXISTS tool_results ("                                                     \
   "   id TEXT PRIMARY KEY,"                                                                       \
   "   user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"                           \
   "   conversation_id INTEGER REFERENCES conversations(id) ON DELETE CASCADE,"                    \
   "   session_key TEXT,"                                                                          \
   "   tool_name TEXT NOT NULL,"                                                                   \
   "   tool_call_id TEXT,"                                                                         \
   "   content_kind INTEGER NOT NULL,"                                                             \
   "   chars INTEGER NOT NULL,"                                                                    \
   "   bytes INTEGER NOT NULL,"                                                                    \
   "   created_at INTEGER NOT NULL,"                                                               \
   "   body BLOB NOT NULL"                                                                         \
   ");"                                                                                            \
   "CREATE INDEX IF NOT EXISTS idx_tool_results_conv "                                             \
   "   ON tool_results(conversation_id, created_at, bytes);"                                       \
   "CREATE INDEX IF NOT EXISTS idx_tool_results_user ON tool_results(user_id, created_at, bytes);" \
   "CREATE TABLE IF NOT EXISTS tool_results_usage ("                                               \
   "   scope INTEGER NOT NULL,"                                                                    \
   "   key INTEGER NOT NULL,"                                                                      \
   "   bytes INTEGER NOT NULL,"                                                                    \
   "   PRIMARY KEY (scope, key)"                                                                   \
   ") WITHOUT ROWID;"                                                                              \
   "CREATE TRIGGER IF NOT EXISTS tool_results_usage_add AFTER INSERT ON tool_results BEGIN"        \
   "   INSERT OR IGNORE INTO tool_results_usage VALUES (1, NEW.user_id, 0), (2, 0, 0);"            \
   "   UPDATE tool_results_usage SET bytes = bytes + NEW.bytes"                                    \
   "      WHERE (scope = 1 AND key = NEW.user_id) OR (scope = 2 AND key = 0);"                     \
   "   INSERT OR IGNORE INTO tool_results_usage"                                                   \
   "      SELECT 0, NEW.conversation_id, 0 WHERE NEW.conversation_id IS NOT NULL;"                 \
   "   UPDATE tool_results_usage SET bytes = bytes + NEW.bytes"                                    \
   "      WHERE scope = 0 AND key = NEW.conversation_id;"                                          \
   "END;"                                                                                          \
   "CREATE TRIGGER IF NOT EXISTS tool_results_usage_del AFTER DELETE ON tool_results BEGIN"        \
   "   UPDATE tool_results_usage SET bytes = bytes - OLD.bytes"                                    \
   "      WHERE (scope = 1 AND key = OLD.user_id) OR (scope = 2 AND key = 0)"                      \
   "         OR (scope = 0 AND key = OLD.conversation_id);"                                        \
   "   DELETE FROM tool_results_usage WHERE bytes <= 0"                                            \
   "      AND ((scope = 1 AND key = OLD.user_id) OR (scope = 0 AND key = OLD.conversation_id));"   \
   "END;"                                                                                          \
   "CREATE TRIGGER IF NOT EXISTS tool_results_usage_bind"                                          \
   "   AFTER UPDATE OF conversation_id ON tool_results BEGIN"                                      \
   "   UPDATE tool_results_usage SET bytes = bytes - OLD.bytes"                                    \
   "      WHERE scope = 0 AND key = OLD.conversation_id;"                                          \
   "   DELETE FROM tool_results_usage WHERE bytes <= 0 AND scope = 0"                              \
   "      AND key = OLD.conversation_id;"                                                          \
   "   INSERT OR IGNORE INTO tool_results_usage"                                                   \
   "      SELECT 0, NEW.conversation_id, 0 WHERE NEW.conversation_id IS NOT NULL;"                 \
   "   UPDATE tool_results_usage SET bytes = bytes + NEW.bytes"                                    \
   "      WHERE scope = 0 AND key = NEW.conversation_id;"                                          \
   "END;"

/* Deep-research tables (v75), as ONE shared DDL string so the base SCHEMA_SQL
 * (auth_db_schema.c, fresh installs) and the v75 migration
 * (auth_db_migrations_v75.c, existing DBs) can never silently diverge — both
 * build the identical research_* schema from this single source.  Safe in both
 * paths: every statement is CREATE ... IF NOT EXISTS and references no
 * migration-only column.  Deliberately NOT included here: the `job_kind` ALTER
 * (an inline column in the base CREATE, an ALTER in the migration — structurally
 * different) and `idx_conv_jobs_user` (references conversations.job_status,
 * which does not exist when the base schema runs on a pre-v72 DB).
 *
 * Design notes (see docs/DEEP_RESEARCH_DESIGN.md §3):
 *  - research_runs.conversation_id is UNIQUE: a run IS its bare-session
 *    conversation, strictly 1:1 (P0 blocks research resume; P1 ledger-resume
 *    reuses the same run, never mints a second row per conversation).
 *  - research_questions.parent_qid and research_claims.question_id are FK-less
 *    soft links BY DESIGN: questions/claims are only ever deleted via the run
 *    cascade, so nothing dangles, and parent_qid is a self-reference whose FK
 *    would only add ordering friction.  Add a real FK only if standalone
 *    question deletion is ever introduced.
 *  - Coverage is COUNT(DISTINCT source_url); there is deliberately no
 *    sources_count cache column (it would be a denormalized value that lies). */
#define AUTH_DB_RESEARCH_SCHEMA_SQL                                                  \
   "CREATE TABLE IF NOT EXISTS research_runs ("                                      \
   "   id INTEGER PRIMARY KEY AUTOINCREMENT,"                                        \
   "   conversation_id INTEGER NOT NULL,"                                            \
   "   user_id INTEGER NOT NULL,"                                                    \
   "   brief TEXT NOT NULL,"                                                         \
   "   mode TEXT NOT NULL DEFAULT 'web',"                                            \
   "   status TEXT NOT NULL,"                                                        \
   "   report_doc_id INTEGER,"                                                       \
   "   rounds_run INTEGER NOT NULL DEFAULT 0,"                                       \
   "   tool_calls INTEGER NOT NULL DEFAULT 0,"                                       \
   "   input_tokens INTEGER NOT NULL DEFAULT 0,"                                     \
   "   stop_reason TEXT,"                                                            \
   "   created_at INTEGER NOT NULL,"                                                 \
   "   finished_at INTEGER,"                                                         \
   "   UNIQUE (conversation_id),"                                                    \
   "   FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE" \
   ");"                                                                              \
   "CREATE INDEX IF NOT EXISTS idx_research_runs_user "                              \
   "ON research_runs(user_id, created_at DESC);"                                     \
   "CREATE TABLE IF NOT EXISTS research_questions ("                                 \
   "   id INTEGER PRIMARY KEY AUTOINCREMENT,"                                        \
   "   run_id INTEGER NOT NULL,"                                                     \
   "   question TEXT NOT NULL,"                                                      \
   "   status TEXT NOT NULL DEFAULT 'open',"                                         \
   "   confidence REAL NOT NULL DEFAULT 0.0,"                                        \
   "   parent_qid INTEGER,"                                                          \
   "   created_at INTEGER NOT NULL,"                                                 \
   "   resolution_reason TEXT,"                                                      \
   "   FOREIGN KEY (run_id) REFERENCES research_runs(id) ON DELETE CASCADE"          \
   ");"                                                                              \
   "CREATE INDEX IF NOT EXISTS idx_research_questions_run "                          \
   "ON research_questions(run_id, status);"                                          \
   "CREATE TABLE IF NOT EXISTS research_claims ("                                    \
   "   id INTEGER PRIMARY KEY AUTOINCREMENT,"                                        \
   "   run_id INTEGER NOT NULL,"                                                     \
   "   question_id INTEGER,"                                                         \
   "   claim TEXT NOT NULL,"                                                         \
   "   source_url TEXT,"                                                             \
   "   source_kind TEXT NOT NULL DEFAULT 'web',"                                     \
   "   quote TEXT,"                                                                  \
   "   round INTEGER NOT NULL,"                                                      \
   "   created_at INTEGER NOT NULL,"                                                 \
   "   FOREIGN KEY (run_id) REFERENCES research_runs(id) ON DELETE CASCADE"          \
   ");"                                                                              \
   "CREATE INDEX IF NOT EXISTS idx_research_claims_run "                             \
   "ON research_claims(run_id, question_id);"                                        \
   "CREATE TABLE IF NOT EXISTS research_report_revisions ("                          \
   "   id INTEGER PRIMARY KEY AUTOINCREMENT,"                                        \
   "   run_id INTEGER NOT NULL,"                                                     \
   "   round INTEGER NOT NULL,"                                                      \
   "   markdown TEXT NOT NULL,"                                                      \
   "   created_at INTEGER NOT NULL,"                                                 \
   "   FOREIGN KEY (run_id) REFERENCES research_runs(id) ON DELETE CASCADE"          \
   ");"                                                                              \
   "CREATE INDEX IF NOT EXISTS idx_research_revisions_run "                          \
   "ON research_report_revisions(run_id, round);"

/* Default email read-body cap baked into the email_accounts schema column and
 * applied by the v55 migration.  The auth layer cannot include tools/ headers,
 * so this MIRRORS EMAIL_MAX_READ_BODY_LEN in include/tools/email_types.h (the
 * runtime read-body fallback) — keep the two values in sync. */
#define EMAIL_DEFAULT_BODY_CHARS 50000

/* Default per-account digest depth baked into the email_accounts schema column
 * and applied by the v87 migration.  MIRRORS EMAIL_DIGEST_DEPTH_DEFAULT in
 * include/tools/email_types.h — keep the two values in sync. */
#define EMAIL_DEFAULT_DIGEST_DEPTH 50
/* digest_depth's base-schema column definition (the v87 ALTER spells the same
 * type + default itself).  A single token keeps the base schema's long string
 * concatenation formatter-stable. */
#define EMAIL_DIGEST_DEPTH_COLUMN_SQL \
   "  digest_depth INTEGER NOT NULL DEFAULT " STRINGIFY(EMAIL_DEFAULT_DIGEST_DEPTH) ","

/* =============================================================================
 * Database State Structure (~408 bytes in BSS)
 *
 * Contains the SQLite database handle, mutex, and all 43 prepared statements.
 * Allocated statically in auth_db_core.c, not on heap.
 * ============================================================================= */

typedef struct {
   sqlite3 *db;
   pthread_mutex_t mutex;
   bool initialized;
   time_t last_cleanup;
   time_t last_vacuum; /* Rate limiting for vacuum operations */

   /* === User module statements (auth_db_user.c) === */
   sqlite3_stmt *stmt_create_user;
   sqlite3_stmt *stmt_get_user;
   sqlite3_stmt *stmt_count_users;
   sqlite3_stmt *stmt_inc_failed_attempts;
   sqlite3_stmt *stmt_reset_failed_attempts;
   sqlite3_stmt *stmt_update_last_login;
   sqlite3_stmt *stmt_set_lockout;

   /* === Session module statements (auth_db_session.c) === */
   sqlite3_stmt *stmt_create_session;
   sqlite3_stmt *stmt_get_session;
   sqlite3_stmt *stmt_renew_session;
   sqlite3_stmt *stmt_set_session_keepalive;
   sqlite3_stmt *stmt_update_session_activity;
   sqlite3_stmt *stmt_delete_session;
   sqlite3_stmt *stmt_delete_user_sessions;
   sqlite3_stmt *stmt_delete_expired_sessions;

   /* === Rate limit module statements (auth_db_rate_limit.c) === */
   sqlite3_stmt *stmt_count_recent_failures;
   sqlite3_stmt *stmt_log_attempt;
   sqlite3_stmt *stmt_delete_old_attempts;

   /* === Audit module statements (auth_db_audit.c) === */
   sqlite3_stmt *stmt_log_event;
   sqlite3_stmt *stmt_delete_old_logs;

   /* === Settings module statements (auth_db_settings.c) === */
   sqlite3_stmt *stmt_get_user_settings;
   sqlite3_stmt *stmt_set_user_settings;

   /* === Conversation module statements (auth_db_conv.c) === */
   sqlite3_stmt *stmt_conv_get;
   sqlite3_stmt *stmt_conv_list;
   sqlite3_stmt *stmt_conv_list_all;
   sqlite3_stmt *stmt_conv_search;
   sqlite3_stmt *stmt_conv_search_content;
   sqlite3_stmt *stmt_conv_rename;
   sqlite3_stmt *stmt_conv_delete;
   sqlite3_stmt *stmt_conv_delete_admin;
   sqlite3_stmt *stmt_conv_count;
   /* Background-jobs hot paths, cached because they run under the global auth_db
    * lock on the 1-Hz heartbeat (drain) / on every persisted event (append) —
    * a per-call prepare was measured at ~36% of the drain scan.  Prepared with
    * JOB_SELECT_COLS (below) so the projection matches job_unpack_row(). */
   sqlite3_stmt *stmt_job_pending_followups;
   sqlite3_stmt *stmt_event_append;
   sqlite3_stmt *stmt_msg_add;
   sqlite3_stmt *stmt_msg_get;
   sqlite3_stmt *stmt_msg_get_after;
   sqlite3_stmt *stmt_msg_get_llm;      /* replay read with llm_blocks (auth_db_messages.c) */
   sqlite3_stmt *stmt_msg_llm_sizes;    /* block sizes for the per-load budget */
   sqlite3_stmt *stmt_msg_gc_blocks;    /* blocks dropped below the watermark */
   sqlite3_stmt *stmt_msg_sweep_blocks; /* the same, across conversations */
   sqlite3_stmt *stmt_msg_bind_images;  /* a row's images made permanent with it */
   sqlite3_stmt *stmt_msg_ref_captures; /* a tool row's captures, named by its conversation */
   sqlite3_stmt *stmt_msg_ref_uploads;  /* a question's uploads, named by its conversation */
   sqlite3_stmt *stmt_msg_get_admin;
   sqlite3_stmt *stmt_conv_update_meta;
   sqlite3_stmt *stmt_conv_update_context;
   sqlite3_stmt *stmt_conv_set_watermark;
   sqlite3_stmt *stmt_conv_create_origin;
   sqlite3_stmt *stmt_conv_reassign;

   /* === Metrics module statements (auth_db_metrics.c) === */
   sqlite3_stmt *stmt_metrics_save;
   sqlite3_stmt *stmt_metrics_update;
   sqlite3_stmt *stmt_metrics_delete_old;
   sqlite3_stmt *stmt_provider_metrics_save;
   sqlite3_stmt *stmt_provider_metrics_delete;

   /* === Image module statements (image_store.c) === */
   sqlite3_stmt *stmt_image_create;
   sqlite3_stmt *stmt_image_get;
   sqlite3_stmt *stmt_image_get_file; /* filename + user_id + source for path + access check */
   sqlite3_stmt *stmt_image_delete;
   sqlite3_stmt *stmt_image_update_access;
   sqlite3_stmt *stmt_image_update_retention;
   sqlite3_stmt *stmt_image_count_user;
   sqlite3_stmt *stmt_image_count_user_source; /* COUNT by user_id + source */
   sqlite3_stmt *stmt_image_delete_old;        /* DEFAULT retention: created_at < cutoff */
   sqlite3_stmt *stmt_image_cache_total_size;  /* SUM(size) for RETAIN_CACHE images */
   sqlite3_stmt *stmt_image_delete_cache_lru;  /* oldest CACHE image by last_accessed */
   sqlite3_stmt *stmt_image_get_expired_ids;   /* IDs + filenames of expired images */
   sqlite3_stmt *stmt_image_get_cache_lru_ids; /* IDs + filenames of LRU cache overflow */
   sqlite3_stmt *stmt_image_stats;             /* COUNT + SUM(size) for all images */
   sqlite3_stmt *stmt_image_get_unbound_ids;   /* IDs + filenames of unbound images by cutoff */
   sqlite3_stmt *stmt_image_delete_unbound;    /* one of those, if still unbound */

   /* === Memory module statements (memory_db.c) === */
   sqlite3_stmt *stmt_memory_fact_create;
   sqlite3_stmt *stmt_memory_fact_get;
   sqlite3_stmt *stmt_memory_fact_list;
   sqlite3_stmt *stmt_memory_fact_list_created_desc; /* WebUI sort: newest first */
   sqlite3_stmt *stmt_memory_fact_list_created_asc;  /* WebUI sort: oldest first */
   sqlite3_stmt *stmt_memory_fact_search;
   sqlite3_stmt *stmt_memory_fact_update_access;
   sqlite3_stmt *stmt_memory_fact_reinforce_citation;
   sqlite3_stmt *stmt_memory_fact_update_confidence;
   sqlite3_stmt *stmt_memory_fact_supersede;
   sqlite3_stmt *stmt_memory_fact_delete;
   sqlite3_stmt *stmt_memory_fact_find_similar;
   sqlite3_stmt *stmt_memory_fact_find_by_hash;
   sqlite3_stmt *stmt_memory_fact_prune_superseded;
   sqlite3_stmt *stmt_memory_fact_prune_stale;
   sqlite3_stmt *stmt_memory_fact_prune_expired; /* v58: hard-delete expired facts */

   /* v48: FTS5 BM25 keyword search.  See docs/MEM0_ARCHITECTURAL_PARITY.md
    * Phase 1.  External-content rows live in memory_facts_fts; the search
    * statement JOINs back to memory_facts to filter superseded rows and
    * scope by user_id.  `_since` variant adds `AND mf.created_at >= ?`
    * so time-windowed callers (focus-adapter recent windows + memory.recent
    * with since_ts) use BM25 too rather than silently falling back to the
    * legacy LIKE path. */
   sqlite3_stmt *stmt_memory_fact_search_bm25;
   sqlite3_stmt *stmt_memory_fact_search_bm25_since;
   sqlite3_stmt *stmt_memory_facts_fts_insert;
   sqlite3_stmt *stmt_memory_fact_source_add;     /* v89; NULL until migrated */
   sqlite3_stmt *stmt_memory_relation_source_add; /* v89; NULL until migrated */
   sqlite3_stmt *stmt_memory_pref_source_add;     /* v89; NULL until migrated */
   sqlite3_stmt *stmt_memory_pref_current;        /* the row's id and value, before an upsert */
   sqlite3_stmt *stmt_memory_pref_sources_clear;  /* v89; NULL until migrated */
   sqlite3_stmt *stmt_doc_chunk_generation;       /* v89; NULL until migrated */
   sqlite3_stmt *stmt_memory_facts_fts_delete;

   sqlite3_stmt *stmt_memory_pref_upsert;
   sqlite3_stmt *stmt_memory_pref_get;
   sqlite3_stmt *stmt_memory_pref_list;
   sqlite3_stmt *stmt_memory_pref_search;
   sqlite3_stmt *stmt_memory_pref_delete;

   sqlite3_stmt *stmt_memory_summary_create;
   sqlite3_stmt *stmt_memory_summary_list;
   sqlite3_stmt *stmt_memory_summary_list_created_asc; /* WebUI sort: oldest first */
   sqlite3_stmt *stmt_memory_summary_mark_consolidated;
   sqlite3_stmt *stmt_memory_summary_search;

   /* Date-filtered memory queries */
   sqlite3_stmt *stmt_memory_fact_search_since;
   sqlite3_stmt *stmt_memory_summary_search_since;

   /* Bundle 3 (2026-05-13) — windowed/sorted variants for the LLM 'recent'
    * and 'search' tool actions.  Cover (since, until, sort) parameter space
    * with one prepared statement per sort direction.  The DESC variant
    * subsumes the legacy _list_since shape when called with until=INT64_MAX. */
   sqlite3_stmt *stmt_memory_fact_list_window_asc;
   sqlite3_stmt *stmt_memory_fact_list_window_desc;
   sqlite3_stmt *stmt_memory_summary_list_window_asc;
   sqlite3_stmt *stmt_memory_summary_list_window_desc;

   /* Category-filtered fact queries (v34) */
   sqlite3_stmt *stmt_memory_fact_update_category;
   sqlite3_stmt *stmt_memory_fact_list_general;
   sqlite3_stmt *stmt_memory_fact_count_general;

   /* Extraction tracking */
   sqlite3_stmt *stmt_conv_get_last_extracted;
   sqlite3_stmt *stmt_conv_set_last_extracted;

   /* Privacy flag */
   sqlite3_stmt *stmt_conv_set_private;

   /* Pinned flag */
   sqlite3_stmt *stmt_conv_set_pinned;

   /* Auto-title (memory extraction) */
   sqlite3_stmt *stmt_conv_auto_title;
   sqlite3_stmt *stmt_conv_set_title_locked;

   /* === Embedding module statements (memory_db.c) === */
   sqlite3_stmt *stmt_memory_fact_update_embedding;
   sqlite3_stmt *stmt_memory_fact_list_without_embedding;

   /* Summary-embedding statements (v45) — used by the semantic summary
    * adapter and the recompute worker.  None of these need a fact-style
    * cache: the per-user summary count is small (hundreds for the dev),
    * so a single locked scan per query is sub-millisecond. */
   sqlite3_stmt *stmt_memory_summary_update_embedding;
   sqlite3_stmt *stmt_memory_summary_scan_embeddings;
   sqlite3_stmt *stmt_memory_summary_list_without_embedding;

   /* === Satellite mapping statements (auth_db_satellite.c) === */
   sqlite3_stmt *stmt_satellite_upsert;
   sqlite3_stmt *stmt_satellite_get;
   sqlite3_stmt *stmt_satellite_delete;
   sqlite3_stmt *stmt_satellite_update_user;
   sqlite3_stmt *stmt_satellite_update_location;
   sqlite3_stmt *stmt_satellite_set_enabled;
   sqlite3_stmt *stmt_satellite_update_last_seen;
   sqlite3_stmt *stmt_satellite_list;

   /* === Entity graph statements (memory_db.c) === */
   sqlite3_stmt *stmt_memory_entity_upsert;
   sqlite3_stmt *stmt_memory_entity_get_by_name;
   sqlite3_stmt *stmt_memory_entity_update_embedding;
   sqlite3_stmt *stmt_memory_entity_get_embeddings;
   sqlite3_stmt *stmt_memory_relation_create;
   sqlite3_stmt *stmt_memory_relation_close_open; /* v33 — supersede helper */
   sqlite3_stmt *stmt_memory_relation_list_by_subject;
   sqlite3_stmt *stmt_memory_relation_list_by_subject_at; /* v33 — as-of variant */
   sqlite3_stmt *stmt_memory_relation_list_by_object;
   sqlite3_stmt *stmt_memory_relation_fact_ids_for_entity; /* graph-retrieval 1A */
   sqlite3_stmt *stmt_memory_entity_search;
   sqlite3_stmt *stmt_memory_entity_delete;
   sqlite3_stmt *stmt_memory_entity_set_photo;
   sqlite3_stmt *stmt_memory_entity_get_photo;
   sqlite3_stmt *stmt_memory_relation_delete_by_entity;

   /* === Document search statements (document_db.c) === */
   sqlite3_stmt *stmt_doc_create;
   sqlite3_stmt *stmt_doc_get;
   sqlite3_stmt *stmt_doc_get_by_hash;
   sqlite3_stmt *stmt_doc_list;
   sqlite3_stmt *stmt_doc_delete;
   sqlite3_stmt *stmt_doc_count_user;
   sqlite3_stmt *stmt_doc_chunk_create;
   sqlite3_stmt *stmt_doc_find_by_name;
   sqlite3_stmt *stmt_doc_chunk_read;
   sqlite3_stmt *stmt_doc_chunk_read_range; /* window by chunk_index (gap-safe, no OFFSET) */
   sqlite3_stmt *stmt_doc_chunk_grep_ci;    /* literal substring, case-insensitive (LIKE) */
   sqlite3_stmt *stmt_doc_chunk_grep_cs;    /* literal substring, case-sensitive (instr) */
   sqlite3_stmt *stmt_doc_list_all;
   sqlite3_stmt *stmt_doc_update_global;
   /* v61 — document_chunks_fts (BM25 lexical channel) + stable-id note edit. */
   sqlite3_stmt *stmt_doc_chunk_fts_insert;
   sqlite3_stmt *stmt_doc_chunk_fts_delete;
   sqlite3_stmt *stmt_doc_chunk_search_bm25;
   sqlite3_stmt *stmt_doc_chunk_update;

   /* === Calendar module statements (calendar_db.c) === */
   sqlite3_stmt *stmt_cal_acct_create;
   sqlite3_stmt *stmt_cal_acct_get;
   sqlite3_stmt *stmt_cal_acct_list;
   sqlite3_stmt *stmt_cal_acct_update;
   sqlite3_stmt *stmt_cal_acct_delete;
   sqlite3_stmt *stmt_cal_acct_update_sync;
   sqlite3_stmt *stmt_cal_acct_update_discovery;
   sqlite3_stmt *stmt_cal_cal_create;
   sqlite3_stmt *stmt_cal_cal_get;
   sqlite3_stmt *stmt_cal_cal_list;
   sqlite3_stmt *stmt_cal_cal_update_ctag;
   sqlite3_stmt *stmt_cal_cal_update_sync_token;
   sqlite3_stmt *stmt_cal_cal_set_active;
   sqlite3_stmt *stmt_cal_cal_delete;
   sqlite3_stmt *stmt_cal_cal_active_for_user;
   sqlite3_stmt *stmt_cal_evt_upsert;
   sqlite3_stmt *stmt_cal_evt_get_by_uid;
   sqlite3_stmt *stmt_cal_evt_delete;
   sqlite3_stmt *stmt_cal_evt_delete_by_cal;
   sqlite3_stmt *stmt_cal_evt_delete_by_href;
   sqlite3_stmt *stmt_cal_evt_prune_window_stale;
   sqlite3_stmt *stmt_cal_evt_prune_not_in_hrefs;
   sqlite3_stmt *stmt_cal_evt_count_href_in_set;
   sqlite3_stmt *stmt_cal_occ_insert;
   sqlite3_stmt *stmt_cal_occ_delete_for_event;
   sqlite3_stmt *stmt_cal_occ_in_range;
   sqlite3_stmt *stmt_cal_events_nearest;
   sqlite3_stmt *stmt_cal_occ_allday_in_range;
   sqlite3_stmt *stmt_cal_occ_search;
   sqlite3_stmt *stmt_cal_occ_next;
   sqlite3_stmt *stmt_cal_acct_list_enabled;
   sqlite3_stmt *stmt_cal_acct_set_read_only;
   sqlite3_stmt *stmt_cal_acct_set_enabled;

   /* === Contacts module statements (contacts_db.c) === */
   sqlite3_stmt *stmt_contacts_find;
   sqlite3_stmt *stmt_contacts_add;
   sqlite3_stmt *stmt_contacts_delete;
   sqlite3_stmt *stmt_contacts_list;
   sqlite3_stmt *stmt_contacts_update;
   sqlite3_stmt *stmt_contacts_count;

   /* === Email module statements (email_db.c) === */
   sqlite3_stmt *stmt_email_acct_create;
   sqlite3_stmt *stmt_email_acct_get;
   sqlite3_stmt *stmt_email_acct_list;
   sqlite3_stmt *stmt_email_acct_update;
   sqlite3_stmt *stmt_email_acct_delete;
   sqlite3_stmt *stmt_email_acct_set_read_only;
   sqlite3_stmt *stmt_email_acct_set_enabled;

   /* === Generic blob store statements (blob_store.c via document_original_store.c) ===
    * Inserted before the OAuth group so stmt_oauth_list_accounts stays the last
    * field (the _Static_assert + finalize memset bound depend on it). */
   sqlite3_stmt *stmt_blob_create;
   sqlite3_stmt *stmt_blob_get;
   sqlite3_stmt *stmt_blob_get_file;
   sqlite3_stmt *stmt_blob_delete;
   sqlite3_stmt *stmt_blob_update_access;
   sqlite3_stmt *stmt_blob_update_retention;
   sqlite3_stmt *stmt_blob_count_user;
   sqlite3_stmt *stmt_blob_sum_bytes_user;
   sqlite3_stmt *stmt_blob_find_by_hash;
   sqlite3_stmt *stmt_blob_delete_old;
   sqlite3_stmt *stmt_blob_cache_total_size;
   sqlite3_stmt *stmt_blob_delete_by_id;
   sqlite3_stmt *stmt_blob_get_expired_ids;
   sqlite3_stmt *stmt_blob_get_cache_lru_ids;
   sqlite3_stmt *stmt_blob_get_orphan_ids;
   sqlite3_stmt *stmt_blob_stats;

   /* === OAuth module statements (oauth_client.c) === */
   sqlite3_stmt *stmt_oauth_store;
   sqlite3_stmt *stmt_oauth_load;
   sqlite3_stmt *stmt_oauth_delete;
   sqlite3_stmt *stmt_oauth_exists;
   sqlite3_stmt *stmt_oauth_list_accounts;
} auth_db_state_t;

/* Ensure last_stmt_end covers all statement fields (catches reorder bugs).
 * Both invariants protect the memset region in auth_db_finalize_statements():
 *   1. stmt_oauth_list_accounts (the named upper bound) must come after
 *      stmt_create_user (the named lower bound) — else last - first
 *      underflows size_t and memset clears far too much.
 *   2. stmt_oauth_list_accounts must be the actual last field in
 *      auth_db_state_t — else a struct append silently leaks the new
 *      statement pointer (memset skips it, and the finalize chain
 *      forgets to call sqlite3_finalize on it).
 * If you append a new sqlite3_stmt* field, also update both the named
 * upper bound here AND the matching last_stmt_end in
 * auth_db_finalize_statements(). */
_Static_assert(offsetof(auth_db_state_t, stmt_oauth_list_accounts) >
                   offsetof(auth_db_state_t, stmt_create_user),
               "stmt_oauth_list_accounts must be after stmt_create_user");
_Static_assert(sizeof(auth_db_state_t) ==
                   offsetof(auth_db_state_t, stmt_oauth_list_accounts) + sizeof(sqlite3_stmt *),
               "stmt_oauth_list_accounts must be the last field — update memset bounds");

/* =============================================================================
 * Shared State (defined in auth_db_core.c)
 * ============================================================================= */

extern auth_db_state_t s_db;

/* =============================================================================
 * Mutex Helper Macros
 *
 * Use these macros to enforce consistent locking patterns across all modules.
 * ============================================================================= */

/**
 * @brief Lock mutex and check initialization, returning specified value if not ready
 *
 * Use this for functions that return a module-specific failure code instead of AUTH_DB_FAILURE.
 */
#define AUTH_DB_LOCK_OR_RETURN(val)         \
   do {                                     \
      pthread_mutex_lock(&s_db.mutex);      \
      if (!s_db.initialized) {              \
         pthread_mutex_unlock(&s_db.mutex); \
         return (val);                      \
      }                                     \
   } while (0)

/**
 * @brief Lock mutex and check initialization, returning AUTH_DB_FAILURE if not ready
 *
 * Usage:
 *   AUTH_DB_LOCK_OR_FAIL();
 *   // ... do work ...
 *   AUTH_DB_UNLOCK();
 *   return result;
 */
#define AUTH_DB_LOCK_OR_FAIL() AUTH_DB_LOCK_OR_RETURN(AUTH_DB_FAILURE)

/**
 * @brief Lock mutex and check initialization for void functions
 *
 * Use this in functions that return void.
 */
#define AUTH_DB_LOCK_OR_RETURN_VOID()       \
   do {                                     \
      pthread_mutex_lock(&s_db.mutex);      \
      if (!s_db.initialized) {              \
         pthread_mutex_unlock(&s_db.mutex); \
         return;                            \
      }                                     \
   } while (0)

/**
 * @brief Unlock the database mutex
 */
#define AUTH_DB_UNLOCK() pthread_mutex_unlock(&s_db.mutex)

/* =============================================================================
 * Prepared Statement Invariant
 *
 * INVARIANT: All s_db.stmt_* pointers are valid after auth_db_init() returns
 * AUTH_DB_SUCCESS and before auth_db_shutdown() is called.
 *
 * Module code MUST NOT check for NULL - if init failed, the system should not
 * be running. This avoids redundant NULL checks throughout the codebase.
 *
 * Statement Usage Pattern:
 *   sqlite3_reset(s_db.stmt_xxx);
 *   sqlite3_bind_*(s_db.stmt_xxx, ...);
 *   int rc = sqlite3_step(s_db.stmt_xxx);
 *   // ... process results ...
 *   sqlite3_reset(s_db.stmt_xxx);  // ALWAYS reset before returning
 * ============================================================================= */

/* =============================================================================
 * Internal Helper Functions (defined in auth_db_core.c)
 * ============================================================================= */

/**
 * @brief Verify database file has secure permissions (0600)
 *
 * @param path Path to database file
 * @return AUTH_DB_SUCCESS if OK, AUTH_DB_FAILURE on error
 */
int auth_db_internal_verify_permissions(const char *path);

/**
 * @brief Create parent directory with secure permissions (0700)
 *
 * @param path Path to file (directory will be extracted)
 * @return AUTH_DB_SUCCESS if OK, AUTH_DB_FAILURE on error
 */
int auth_db_internal_create_parent_dir(const char *path);

/**
 * @brief @p n row ids as a JSON array, for `IN (SELECT value FROM json_each(?))`
 *
 * One bound parameter for any number of ids, so a batch lookup is one
 * statement.  Caller frees.
 *
 * @return The array, or NULL on allocation failure
 */
char *auth_db_internal_ids_json(const int64_t *ids, int n);

/* =============================================================================
 * Schema + Statement Lifecycle (defined in sibling modules)
 *
 * These are called only from auth_db_init() / auth_db_shutdown() in
 * auth_db_core.c.  They live in their own .c files purely to keep
 * individual files under the size limits in CLAUDE.md.
 * ============================================================================= */

/**
 * @brief Create or migrate the schema to AUTH_DB_SCHEMA_VERSION.
 *
 * Runs SCHEMA_SQL for fresh installs and walks the per-version migration
 * ladder for existing databases.  Defined in auth_db_schema.c.
 *
 * @param db_path Database file path (used for diagnostic logging only).
 * @return AUTH_DB_SUCCESS on success, AUTH_DB_FAILURE on any error.
 */
int auth_db_create_schema(const char *db_path);

/**
 * @brief Snapshot the database before a schema upgrade (VACUUM INTO).
 *
 * The backup directory + base filename are derived from @p db_path (no fixed
 * name assumed).  Writes "<dir>/backups/<basename>.v<from>-<timestamp>.bak" and
 * prunes to the newest few.  Defined in auth_db_backup.c.
 *
 * @param db_path      Live database file path.
 * @param from_version Current (pre-upgrade) schema version, for the filename.
 * @return AUTH_DB_SUCCESS, or AUTH_DB_FAILURE if the snapshot could not be made.
 */
int auth_db_backup_before_upgrade(const char *db_path, int from_version);

/**
 * @brief v64 migration: create the mcp_user_access table (coding-harness MCP
 *        bridge per-user allowlist).
 *
 * Split into its own helper (called from auth_db_apply_migrations) to avoid
 * growing the migration ladder with DDL. Runs idempotently (CREATE TABLE IF NOT
 * EXISTS) for both fresh installs and upgrades. Intentionally NOT gated on
 * DAWN_ENABLE_MCP_BRIDGE_TOOL: schema versioning is global and must advance
 * uniformly across build configs.
 *
 * @param db Open database handle (caller holds any needed lock; called during
 *           schema creation where no other thread touches the handle).
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int auth_db_migrations_v64(sqlite3 *db);

/**
 * @brief v65 migration: create the code_projects table (coding-harness imported
 *        repositories). Like v64, idempotent and NOT gated on a feature flag.
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int auth_db_migrations_v65(sqlite3 *db);

/**
 * @brief v66 migration: add branch / kind / graph_name columns to code_projects
 *        (branch tracking, link-local repos, persisted cbm graph slug). Idempotent
 *        (probes PRAGMA table_info before each ALTER); NOT gated on a feature flag.
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int auth_db_migrations_v66(sqlite3 *db);

/**
 * @brief v67 migration: add conversations.context_watermark_msg_id (compaction
 *        watermark, replacing fork-on-compaction) and one-time unlock of legacy
 *        split-archived conversations. Idempotent (probes PRAGMA table_info).
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int auth_db_migrations_v67(sqlite3 *db);

/**
 * @brief v92 migration: messages.llm_blocks (an assistant turn's stored blocks),
 *        the trigger that drops them when a row's text changes, and a one-time
 *        re-render of voice rows saved as raw Claude block arrays. Idempotent.
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int auth_db_migrations_v92(sqlite3 *db);

/**
 * @brief v93 migration: voice rows whose tool calls the old voice save dropped
 *        (an empty assistant turn, results with no call id) become text notes.
 *        Idempotent.
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int auth_db_migrations_v93(sqlite3 *db);

/**
 * @brief v94 migration: messages.kind, the frozen request prefix
 *        (conversations.prefix_hash/tools_hash/reasoning_floor_msg_id +
 *        prompt_blobs) and conversation_focus_handles.  Idempotent.
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int auth_db_migrations_v94(sqlite3 *db);

/**
 * @brief v95 migration: memory_facts.superseded_at (when a fact was merged),
 *        stamped now on facts already superseded.  Idempotent.
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int auth_db_migrations_v95(sqlite3 *db);

/**
 * @brief v96 migration: compacted conversations' reasoning floor to their
 *        newest row (the summary's shape changed).
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int auth_db_migrations_v96(sqlite3 *db);

/**
 * @brief v97 migration: the tool_results table (a tool result kept whole
 *        behind the view the model is shown).
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int auth_db_migrations_v97(sqlite3 *db);

/**
 * @brief v98 migration: messages rebuilt once (same rows, ids and sequence)
 *        with messages.images and with kinds checked by triggers rather than
 *        a column CHECK (kind tool_change added), then the WAL truncated.
 *        Idempotent: a table that already has `images` only gets its
 *        indexes and triggers made.  Not inside a transaction (it turns
 *        foreign keys off around its own).
 * @param db_path The database file, for the free-space check (may be NULL:
 *        no check).
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE (the table as it was).
 */
int auth_db_migrations_v98(sqlite3 *db, const char *db_path);

/**
 * @brief v99: memory_citation_audit.referenced_ids, the items a turn named
 *        again as still relevant rather than sent.  Idempotent (probe-guarded
 *        ALTER; the base schema carries the column).
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int auth_db_migrations_v99(sqlite3 *db);

/**
 * @brief v100: each stored compaction summary brought to the neutralizer's
 *        current rules, once (it is replayed verbatim from now on); a
 *        conversation whose summary that changes gets a declared boundary
 *        (its reasoning floor raised to its newest row).  Idempotent.
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE (nothing changed).
 */
int auth_db_migrations_v100(sqlite3 *db);

/**
 * @brief v101: messaging_channels.owner_sender (who may speak for a channel)
 *        and the SMS verification columns; backfills existing rows (verified,
 *        Telegram private chats owned by their id) and adds the unique
 *        (provider, address, owner) index, disabling rows that would break it.
 *        Idempotent.
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE (rolled back).
 */
int auth_db_migrations_v101(sqlite3 *db);

/**
 * @brief The v98 rebuild's room check: the bytes it needs free next to the
 *        database for a messages table (and its indexes) of @p table_bytes
 *        (*@p need_out), and whether @p free_bytes covers them.  A free size
 *        that can't be read (< 0) passes: the rebuild then fails on its own
 *        if the disk fills, and rolls back.
 */
bool auth_db_v98_has_room(int64_t table_bytes, int64_t free_bytes, int64_t *need_out);

/**
 * @brief Record, for every stored row, the images its conversation names, as
 *        conv_db_add_row() records them for a new row: an ordinary question's
 *        [IMAGE:<id>] uploads and MMS, and a tool row's captures
 *        (messages.images), the conversation owner's images only (v98, for
 *        rows stored before conversation_images existed)
 *
 * Same statements and marker parsing as the insert path, so the two can't
 * record differently.  Idempotent (INSERT OR IGNORE).  Caller holds the
 * transaction; messages must already have its images column.
 * @param recorded_out Records added (may be NULL)
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_conv_images_backfill(sqlite3 *db, int64_t *recorded_out);

/** Whether @p table has column @p col (a migration's probe before an ALTER;
 *  auth_db_migrations.c). */
bool auth_db_column_exists(sqlite3 *db, const char *table, const char *col);

/* document_chunks' triggers, defined once for the migration that made them
 * and the one that rebuilds the table (a rebuild drops a table's triggers). */

/** Chunk-visibility generation per owner, bumped on every chunk change. */
#define DOC_CHUNK_GENERATION_TRIGGERS_SQL                                  \
   "CREATE TRIGGER IF NOT EXISTS trg_doc_chunks_gen_ins AFTER INSERT ON "  \
   "document_chunks BEGIN INSERT INTO doc_chunk_generation (owner, gen) "  \
   "SELECT COALESCE(CASE WHEN is_global THEN 0 ELSE user_id END, 0), 1 "   \
   "FROM documents WHERE id = NEW.document_id "                            \
   "ON CONFLICT(owner) DO UPDATE SET gen = gen + 1; END;"                  \
   "CREATE TRIGGER IF NOT EXISTS trg_doc_chunks_gen_del AFTER DELETE ON "  \
   "document_chunks BEGIN INSERT INTO doc_chunk_generation (owner, gen) "  \
   "SELECT COALESCE(CASE WHEN is_global THEN 0 ELSE user_id END, 0), 1 "   \
   "FROM documents WHERE id = OLD.document_id "                            \
   "ON CONFLICT(owner) DO UPDATE SET gen = gen + 1; END;"                  \
   "CREATE TRIGGER IF NOT EXISTS trg_doc_chunks_gen_emb AFTER UPDATE OF "  \
   "embedding ON document_chunks BEGIN INSERT INTO doc_chunk_generation "  \
   "(owner, gen) SELECT COALESCE(CASE WHEN is_global THEN 0 ELSE user_id " \
   "END, 0), 1 FROM documents WHERE id = NEW.document_id "                 \
   "ON CONFLICT(owner) DO UPDATE SET gen = gen + 1; END;"

/**
 * @brief Install, on DAWN's own connection, what records a user's removals
 *        for withdrawal (auth_db_withdraw.c): the dawn_withdraw_intent() SQL
 *        function and TEMP delete triggers that write withdrawn_items only
 *        while a removal is marked (conv_db_withdraw_intent_begin).  Another
 *        connection (the sqlite3 shell) has neither, and records nothing.
 *        Called at init, after migrations.
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_withdraw_install(sqlite3 *db);

/** Prepare / finalize the message-row statements (auth_db_messages.c). */
int auth_db_messages_prepare(void);

/**
 * @brief Insert one message row; the caller holds the lock (and any
 *        transaction).  conv_db_add_row() without the conversation's metadata
 *        bump or list signal.
 */
int msg_insert_locked(int64_t conv_id,
                      int user_id,
                      const conv_message_row_t *row,
                      time_t now,
                      int64_t *id_out);
void auth_db_messages_finalize(void);

/**
 * @brief The images @p conv_id owns (auth_db_messages.h), under @p mode:
 *        deleted (their files added to @p files) or unbound.  The caller holds
 *        the lock and a transaction, and deletes the conversation next.
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE (the caller rolls back)
 */
int conv_images_take_locked(int64_t conv_id,
                            int owner,
                            conv_images_mode_t mode,
                            conv_image_files_t *files);

/** A tool_change row's content past this many bytes is stored in prompt_blobs
 *  by hash (deduplicated: a change appended again after a compaction is the
 *  same bytes); the row holds {"blob": "<hash>"}. */
#define CONV_TOOL_CHANGE_INLINE_MAX 8192

/** Store @p bytes in prompt_blobs under their SHA-256 (kept when already
 *  there); the caller holds the lock (auth_db_conv_prefix.c). */
int conv_prompt_blob_put_locked(const char *bytes, char hash_out[DAWN_SHA256_HEX_LEN]);

/** The bytes stored under @p hash (caller frees), checked against it:
 *  AUTH_DB_INVALID when missing or not matching.  Caller holds the lock. */
int conv_prompt_blob_get_locked(const char *hash, char **out);

/** Whether conversation @p conv_id is @p user_id's: AUTH_DB_SUCCESS,
 *  AUTH_DB_NOT_FOUND or AUTH_DB_FAILURE.  Caller holds the lock
 *  (auth_db_focus_handles.c). */
int conv_db_owned_locked(int64_t conv_id, int user_id);

/** Clear the stored blocks of conversation @p conv_id's rows at or below
 *  @p watermark (out of every reload's reach), in batches, each under the lock
 *  (auth_db_messages.c; the sweep retries what this misses). */
void conv_db_clear_compacted_blocks(int64_t conv_id, int64_t watermark);

/**
 * @brief Withdraw, from conversation @p conv_id's context rows saved at or
 *        after row @p from_id, the items removed since @p built_at (a
 *        minute's grace; 0: within the week), and its USER MEMORY blocks when
 *        the user's memory was withdrawn after @p built_seq (the withdrawal
 *        sequence when the turn was built, conv_db_withdraw_seq).  A change,
 *        or a withdrawal after @p built_seq that changed this conversation
 *        (the history the turn's reasoning is bound to), leaves the
 *        conversation's reasoning behind (its floor, pending).  Caller holds
 *        the lock and the transaction (auth_db_withdraw.c).
 * @param changed_out Whether the floor went pending (the turn mustn't settle it)
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int conv_db_withdraw_saved_locked(int64_t conv_id,
                                  int user_id,
                                  int64_t from_id,
                                  int64_t built_at,
                                  int64_t built_seq,
                                  bool *changed_out);

/**
 * @brief v68 migration: generic blobs table + documents.original_blob_id.
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int auth_db_migrations_v68(sqlite3 *db);

/**
 * @brief v69 migration: add conversations.is_pinned + idx_conversations_pinned.
 *        Idempotent (probes PRAGMA table_info; index uses IF NOT EXISTS).
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int auth_db_migrations_v69(sqlite3 *db);

/**
 * @brief v70 migration: authoritative case-insensitive uniqueness for
 *        code_projects.name (UNIQUE INDEX ... COLLATE NOCASE), matching the
 *        NOCASE application-level lookups. Idempotent (IF NOT EXISTS); a
 *        pre-existing case-variant collision is logged and skipped, not fatal.
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int auth_db_migrations_v70(sqlite3 *db);

/**
 * @brief v71: SAGE proactive-attention tables (attention_rules + attention_log).
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int auth_db_migrations_v71(sqlite3 *db);

/**
 * @brief v72: background-jobs foundation — job lifecycle columns on
 *        `conversations` + the `conversation_events` durable step-log table.
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int auth_db_migrations_v72(sqlite3 *db);

/**
 * @brief v73: background-jobs Phase 1 — `deliver_to` job-completion target
 *        column on `conversations` + the completion-monitor partial index.
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int auth_db_migrations_v73(sqlite3 *db);
int auth_db_migrations_v74(sqlite3 *db);

/**
 * @brief v75: deep-research foundation — `job_kind` discriminator on
 *        `conversations`, the four `research_*` tables, and the paginated
 *        job/research list partial index (`idx_conv_jobs_user`).
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int auth_db_migrations_v75(sqlite3 *db);

/**
 * @brief Prepare every cached sqlite3_stmt* in s_db.
 *
 * Defined in auth_db_statements.c.  Called after auth_db_create_schema().
 * On any prepare failure returns AUTH_DB_FAILURE without rolling back
 * partial preparations — auth_db_shutdown() cleans up via
 * auth_db_finalize_statements().
 *
 * @return AUTH_DB_SUCCESS on success, AUTH_DB_FAILURE on first prepare error.
 */
int auth_db_prepare_statements(void);

/**
 * @brief Finalize every cached sqlite3_stmt* in s_db.
 *
 * Defined in auth_db_statements.c.  Tolerates partially-prepared state so
 * it is safe to call from auth_db_shutdown() after a
 * auth_db_prepare_statements() failure.  Zeros the statement pointers via
 * memset on the contiguous statement region of s_db.
 */
void auth_db_finalize_statements(void);

#endif /* AUTH_DB_INTERNAL_H */
