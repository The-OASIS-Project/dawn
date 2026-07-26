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
 * Authentication Database Schema and Migration Module
 *
 * Owns the SCHEMA_SQL constant (the base schema for fresh installs) and
 * the per-version migration ladder (create_schema + helpers).  Split out
 * from auth_db_core.c to keep individual files under the size limits in
 * CLAUDE.md.  Cross-module entry points are declared in auth_db_internal.h.
 *
 * SECURITY: All database operations use prepared statements.
 * NEVER use sqlite3_exec() or sqlite3_mprintf() with user input.
 * See: CWE-89, OWASP SQL Injection Prevention Cheat Sheet
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "auth/auth_db_internal.h"
#include "auth/auth_db_migrations.h"
#include "logging.h"

/* =============================================================================
 * Schema SQL
 * ============================================================================= */

/* Base schema for fresh installs.  Must match AUTH_DB_SCHEMA_VERSION.
 *
 * IMPORTANT: When adding a new column or table via migration, also add it here
 * so that fresh installs get the complete schema.  All statements use
 * IF NOT EXISTS / ADD COLUMN guards for idempotency with the migration path. */
static const char *SCHEMA_SQL =
    /* Schema version tracking */
    "CREATE TABLE IF NOT EXISTS schema_version ("
    "   version INTEGER PRIMARY KEY"
    ");"

    /* System-wide key/value metadata (v41).  Used to track daemon-level state
     * that spans all users — e.g., embedding_model_id for recompute detection. */
    "CREATE TABLE IF NOT EXISTS system_metadata ("
    "   key   TEXT PRIMARY KEY,"
    "   value TEXT NOT NULL"
    ");"

    /* Users table (categories_backfilled_at added in v34 — gates lazy fact-category backfill;
     * embeddings_model_id added in v41 — per-user gate for embedding recomputation;
     * v44: real_name / preferred_address / identity_aliases — user-identity fields
     * surfaced in WebUI Settings, injected into the LLM system prompt, and used
     * by the entity-merge link-user-self synthetic-seed scoring path).  All
     * three v44 columns are nullable TEXT — existing rows migrate to NULL. */
    "CREATE TABLE IF NOT EXISTS users ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   username TEXT UNIQUE NOT NULL,"
    "   password_hash TEXT NOT NULL,"
    "   is_admin INTEGER DEFAULT 0,"
    "   created_at INTEGER NOT NULL,"
    "   last_login INTEGER,"
    "   failed_attempts INTEGER DEFAULT 0,"
    "   lockout_until INTEGER DEFAULT 0,"
    "   categories_backfilled_at INTEGER DEFAULT 0,"
    "   embeddings_model_id TEXT DEFAULT NULL,"
    "   real_name TEXT DEFAULT NULL,"
    "   preferred_address TEXT DEFAULT NULL,"
    "   identity_aliases TEXT DEFAULT NULL"
    ");"

    /* Sessions table */
    "CREATE TABLE IF NOT EXISTS sessions ("
    "   token TEXT PRIMARY KEY,"
    "   user_id INTEGER NOT NULL,"
    "   created_at INTEGER NOT NULL,"
    "   last_activity INTEGER NOT NULL,"
    "   expires_at INTEGER,"
    "   ip_address TEXT,"
    "   user_agent TEXT,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id);"
    "CREATE INDEX IF NOT EXISTS idx_sessions_activity ON sessions(last_activity);"
    /* idx_sessions_expires created by v10 migration after expires_at column is added */

    /* Login attempts for rate limiting */
    "CREATE TABLE IF NOT EXISTS login_attempts ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   ip_address TEXT NOT NULL,"
    "   username TEXT,"
    "   timestamp INTEGER NOT NULL,"
    "   success INTEGER DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_attempts_ip ON login_attempts(ip_address, timestamp);"

    /* Audit log */
    "CREATE TABLE IF NOT EXISTS auth_log ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   timestamp INTEGER NOT NULL,"
    "   event TEXT NOT NULL,"
    "   username TEXT,"
    "   ip_address TEXT,"
    "   details TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_log_timestamp ON auth_log(timestamp);"

    /* Per-user settings (added in schema v2, persona_mode added in v3) */
    "CREATE TABLE IF NOT EXISTS user_settings ("
    "   user_id INTEGER PRIMARY KEY,"
    "   persona_description TEXT,"
    "   persona_mode TEXT DEFAULT 'append',"
    "   location TEXT,"
    "   timezone TEXT DEFAULT 'UTC',"
    "   units TEXT DEFAULT 'metric',"
    "   tts_voice_model TEXT,"
    "   tts_length_scale REAL DEFAULT 1.0,"
    "   theme TEXT DEFAULT 'cyan',"
    "   updated_at INTEGER NOT NULL,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
    ");"

    /* Conversations table (added in schema v4, context columns in v5, continuation in v7,
     * LLM settings in v11, extraction tracking in v15, privacy in v16, origin in v17) */
    "CREATE TABLE IF NOT EXISTS conversations ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   user_id INTEGER NOT NULL,"
    "   title TEXT NOT NULL DEFAULT 'New Conversation',"
    "   created_at INTEGER NOT NULL,"
    "   updated_at INTEGER NOT NULL,"
    "   message_count INTEGER DEFAULT 0,"
    "   is_archived INTEGER DEFAULT 0,"
    "   context_tokens INTEGER DEFAULT 0,"
    "   context_max INTEGER DEFAULT 0,"
    "   continued_from INTEGER DEFAULT NULL,"
    "   compaction_summary TEXT DEFAULT NULL,"
    /* context_watermark_msg_id (v67): last compacted message id.  0 = never
     * compacted -> reload loads all messages (pre-watermark behavior).  When > 0,
     * context restore is bounded to messages with id > watermark + the summary. */
    "   context_watermark_msg_id INTEGER NOT NULL DEFAULT 0,"
    "   llm_type TEXT DEFAULT NULL,"
    "   cloud_provider TEXT DEFAULT NULL,"
    "   model TEXT DEFAULT NULL,"
    "   tools_mode TEXT DEFAULT NULL,"
    "   thinking_mode TEXT DEFAULT NULL,"
    "   reasoning_effort TEXT DEFAULT NULL,"
    "   last_extracted_msg_count INTEGER DEFAULT 0,"
    "   last_extracted_msg_id    INTEGER NOT NULL DEFAULT 0,"
    "   extraction_attempts INTEGER DEFAULT 0,"
    "   extraction_last_attempt_at INTEGER DEFAULT 0,"
    "   is_private INTEGER DEFAULT 0,"
    "   title_locked INTEGER DEFAULT 0,"
    "   origin TEXT DEFAULT 'webui',"
    /* is_pinned (v69): user-pinned conversations float to a dedicated section at
     * the top of the WebUI list.  Literal DEFAULT keeps the ALTER fast (see
     * anchor_date note below). */
    "   is_pinned INTEGER NOT NULL DEFAULT 0,"
    /* anchor_date (v42): logical "now" timestamp in epoch seconds.  Production
     * writes time(NULL) at insert; bench overrides per-session.  The 0 default
     * is the ANCHOR_DATE_NONE sentinel — extraction omits the prompt anchor
     * line when this is 0.  KEEP DEFAULT AS A LITERAL CONSTANT (not strftime
     * or CURRENT_TIMESTAMP): SQLite's fast ALTER TABLE ADD COLUMN path requires
     * a literal default, otherwise migration becomes a full table rewrite. */
    "   anchor_date INTEGER NOT NULL DEFAULT 0,"
    /* Background-jobs (v72): a job IS a parented conversation.  parent_id NULL =
     * root/user-initiated; job_status NULL = not a job.  All literal DEFAULTs keep
     * the ALTER fast.  The parent_id index lives in the v72 migration, not here
     * (base schema runs before migrations).  See docs/BACKGROUND_JOBS_DESIGN.md. */
    "   parent_id INTEGER DEFAULT NULL,"
    "   spawn_mode TEXT DEFAULT NULL,"
    "   on_complete TEXT DEFAULT NULL,"
    "   on_complete_fired INTEGER NOT NULL DEFAULT 0,"
    "   job_status TEXT DEFAULT NULL,"
    "   job_error TEXT DEFAULT NULL,"
    "   spawn_depth INTEGER NOT NULL DEFAULT 0,"
    "   reinvoke_count INTEGER NOT NULL DEFAULT 0,"
    "   started_at INTEGER NOT NULL DEFAULT 0,"
    "   finished_at INTEGER NOT NULL DEFAULT 0,"
    "   workspace_ref TEXT DEFAULT NULL,"
    /* deliver_to (v73): job-completion notification target — empty/NULL = local
     * chime+banner+voice; a messaging channel display_name routes the "done"
     * alert there (scheduler deliver_to semantics).  The v73 partial follow-up
     * index (idx_conv_job_followup) lives in the migration, not here. */
    "   deliver_to TEXT DEFAULT NULL,"
    /* job_goal (v74): the instruction the job was created with, stored at CREATE
     * time.  Until v74 the goal existed only as the first `messages` row, which
     * core_text_input_dispatch writes — so a job that died BEFORE dispatch (the
     * "job capacity reached" class, or any pre-dispatch failure) left an empty
     * conversation with its goal nowhere, and resuming it re-engaged the model
     * with no task at all.  `title` is a generated summary, not the goal. */
    "   job_goal TEXT DEFAULT NULL,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
    "   FOREIGN KEY (continued_from) REFERENCES conversations(id) ON DELETE SET NULL,"
    "   FOREIGN KEY (parent_id) REFERENCES conversations(id) ON DELETE SET NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_conversations_user ON conversations(user_id, updated_at DESC);"
    "CREATE INDEX IF NOT EXISTS idx_conversations_search ON conversations(user_id, title);"
    /* Note: idx_conversations_pinned is created in the v69 migration, NOT here.
     * The base SCHEMA_SQL runs before migrations on every boot; on an existing DB
     * the CREATE TABLE above is a no-op (is_pinned not yet added), so referencing
     * is_pinned in a base-schema index would fail with "no such column" until the
     * migration runs.  The migration creates the column then the index. */
    /* Note: idx_conversations_continued is created during migration or post-init
     * to handle both new databases and upgrades from v6 */

    /* Messages table (added in schema v4) */
    "CREATE TABLE IF NOT EXISTS messages ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   conversation_id INTEGER NOT NULL,"
    "   role TEXT NOT NULL CHECK(role IN ('system', 'user', 'assistant', 'tool')),"
    "   content TEXT NOT NULL,"
    "   tool_calls TEXT,"   /* assistant rows: OpenAI tool_calls JSON array (v56) */
    "   tool_call_id TEXT," /* role='tool' rows: matching tool_call id (v56) */
    "   reasoning TEXT,"    /* assistant rows: display-only reasoning JSON (v57) */
    "   created_at INTEGER NOT NULL,"
    "   FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_messages_conversation ON messages(conversation_id, id ASC);"

    /* conversation_events (v72): durable step-granular log for the background-jobs
     * observe/replay contract (status | tool_call | tool_result | terminal_chunk |
     * spawn | complete).  seq is per-conversation monotonic.  Live tokens are NOT
     * persisted here (in-memory replay ring only); final assistant text stays in
     * `messages`.  New table -> table + index are both safe in the base schema. */
    "CREATE TABLE IF NOT EXISTS conversation_events ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   conversation_id INTEGER NOT NULL,"
    "   seq INTEGER NOT NULL,"
    "   kind TEXT NOT NULL,"
    "   payload TEXT,"
    "   created_at INTEGER NOT NULL,"
    "   FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE"
    ");"
    /* UNIQUE enforces the per-conversation monotonic seq invariant at the DB
     * layer (seq is assigned MAX(seq)+1 under the auth_db mutex). */
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_conv_events ON conversation_events(conversation_id, seq "
    "ASC);"

    /* Session metrics table (added in schema v8) */
    "CREATE TABLE IF NOT EXISTS session_metrics ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   session_id INTEGER NOT NULL,"
    "   user_id INTEGER,"
    "   session_type TEXT NOT NULL,"
    "   started_at INTEGER NOT NULL,"
    "   ended_at INTEGER,"
    "   queries_total INTEGER DEFAULT 0,"
    "   queries_cloud INTEGER DEFAULT 0,"
    "   queries_local INTEGER DEFAULT 0,"
    "   errors_count INTEGER DEFAULT 0,"
    "   fallbacks_count INTEGER DEFAULT 0,"
    "   avg_asr_ms REAL,"
    "   avg_llm_ttft_ms REAL,"
    "   avg_llm_total_ms REAL,"
    "   avg_tts_ms REAL,"
    "   avg_pipeline_ms REAL,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE SET NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_session_metrics_user ON session_metrics(user_id, started_at "
    "DESC);"
    "CREATE INDEX IF NOT EXISTS idx_session_metrics_time ON session_metrics(started_at DESC);"

    /* Per-provider token usage breakdown (added in schema v8) */
    "CREATE TABLE IF NOT EXISTS session_metrics_providers ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   session_metrics_id INTEGER NOT NULL,"
    "   provider TEXT NOT NULL,"
    "   tokens_input INTEGER DEFAULT 0,"
    "   tokens_output INTEGER DEFAULT 0,"
    "   tokens_cached INTEGER DEFAULT 0,"
    "   queries INTEGER DEFAULT 0,"
    "   FOREIGN KEY (session_metrics_id) REFERENCES session_metrics(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_metrics_providers_session ON "
    "session_metrics_providers(session_metrics_id);"

    /* Images table — filesystem-backed metadata (v30, migrated from BLOB in v12-v29) */
    "CREATE TABLE IF NOT EXISTS images ("
    "   id TEXT PRIMARY KEY,"
    "   user_id INTEGER NOT NULL,"
    "   source INTEGER NOT NULL DEFAULT 0,"
    "   retention_policy INTEGER NOT NULL DEFAULT 0,"
    "   mime_type TEXT NOT NULL,"
    "   size INTEGER NOT NULL,"
    "   filename TEXT NOT NULL,"
    "   created_at INTEGER NOT NULL,"
    "   last_accessed INTEGER,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_images_user ON images(user_id);"
    "CREATE INDEX IF NOT EXISTS idx_images_created ON images(created_at);"

    /* Generic blob store — original-file storage (v68).  Filesystem-backed:
     * metadata only here.  Per-user content-hash dedup; `kind` tags the consumer
     * (document_original=0; future audio/video).  Referenced by
     * documents.original_blob_id (defined below). */
    "CREATE TABLE IF NOT EXISTS blobs ("
    "   id TEXT PRIMARY KEY,"
    "   user_id INTEGER NOT NULL,"
    "   kind INTEGER NOT NULL DEFAULT 0,"
    "   retention_policy INTEGER NOT NULL DEFAULT 0,"
    "   content_hash TEXT NOT NULL,"
    "   mime_type TEXT NOT NULL,"
    "   size INTEGER NOT NULL,"
    "   filename TEXT NOT NULL,"
    "   filename_original TEXT,"
    "   created_at INTEGER NOT NULL,"
    "   last_accessed INTEGER,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_blobs_user ON blobs(user_id);"
    "CREATE INDEX IF NOT EXISTS idx_blobs_created ON blobs(created_at);"
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_blobs_user_hash ON blobs(user_id, content_hash);"

    /* Satellite mappings table (added in schema v20) */
    "CREATE TABLE IF NOT EXISTS satellite_mappings ("
    "   uuid TEXT PRIMARY KEY,"
    "   name TEXT NOT NULL DEFAULT '',"
    "   location TEXT NOT NULL DEFAULT '',"
    "   ha_area TEXT DEFAULT '',"
    "   user_id INTEGER DEFAULT NULL,"
    "   tier INTEGER DEFAULT 1,"
    "   last_seen INTEGER DEFAULT 0,"
    "   created_at INTEGER NOT NULL,"
    "   enabled INTEGER DEFAULT 1,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE SET NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_satellite_user ON satellite_mappings(user_id);"

    /* OTA per-device update state (v59).  One row per satellite uuid, created
     * lazily on first registration that reports a firmware_version.  Tracks the
     * device's reported running version plus any in-flight update (single source
     * of truth for the OTA state machine — see docs/OTA_DESIGN.md §5).  The
     * token/token_expires columns gate the one-time HTTPS image download (Phase
     * 2); unused in Phase 1.  No FK to satellite_mappings: a device reports its
     * version at register time, which is also when the mapping is upserted, but
     * the two writes are independent rows keyed by the same uuid. */
    "CREATE TABLE IF NOT EXISTS ota_device_state ("
    "   uuid TEXT PRIMARY KEY,"
    "   current_version TEXT NOT NULL DEFAULT '',"
    "   target_version TEXT,"
    "   target_platform TEXT," /* v60: platform the in-flight offer is for (binds the download
                                  token) */
    "   state TEXT NOT NULL DEFAULT 'idle',"
    "   last_error TEXT,"
    "   token TEXT,"
    "   token_expires INTEGER,"
    "   created_at INTEGER NOT NULL,"
    "   updated_at INTEGER NOT NULL"
    ");"

    /* Memory system tables (v14, columns extended in v15/v19) */
    "CREATE TABLE IF NOT EXISTS memory_facts ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   user_id INTEGER NOT NULL,"
    "   fact_text TEXT NOT NULL,"
    "   confidence REAL DEFAULT 1.0,"
    "   source TEXT DEFAULT 'inferred',"
    "   category TEXT NOT NULL DEFAULT 'general',"
    "   created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
    "   last_accessed INTEGER,"
    "   access_count INTEGER DEFAULT 0,"
    "   superseded_by INTEGER,"
    "   normalized_hash INTEGER DEFAULT 0,"
    "   embedding BLOB DEFAULT NULL,"
    "   embedding_norm REAL DEFAULT NULL,"
    "   source_conversation_id INTEGER DEFAULT NULL,"
    "   source_msg_id_start    INTEGER DEFAULT NULL,"
    "   source_msg_id_end      INTEGER DEFAULT NULL,"
    /* v47: subject_entity_id — hard FK from fact to its subject entity.
     * NULLABLE during the migration window (existing rows backfill from
     * linked relations; re-extraction under the new prompt populates the
     * rest).  Tightens to NOT NULL in a follow-up migration once backfill
     * completes.  See PHASE_0_EXTRACTION_PROMPT_DRAFT.md. */
    "   subject_entity_id      INTEGER DEFAULT NULL,"
    /* v58: expires_at — fact-lifecycle expiry (ephemerality / C3).  NULL =
     * durable (the default for every existing and most new rows).  When set
     * (unix seconds), the fact is hidden from retrieval once now >= expires_at
     * (a non-mutating retrieval guard, fully reversible) and hard-pruned by the
     * nightly prune_expired pass after a grace+retention window.  Distinct from
     * relations' valid_to: that bounds a subject→predicate→object triple; this
     * bounds a free-text fact (forecasts, scheduled-not-yet-happened). */
    "   expires_at             INTEGER DEFAULT NULL,"
    /* v61: note_doc_id — memory→note bridge pointer.  NULL for ordinary facts.
     * When set, this fact is a thin "gloss" breadcrumb for a saved reference
     * note (a single-chunk document); the canonical note text lives ONLY in the
     * documents store, never here, so it never enters paraphrase-dedup.  A
     * semantic hit on the gloss resolves to the verbatim note via this id.
     * ON DELETE SET NULL: deleting the note drops the pointer (the gloss text is
     * cleaned up explicitly by the note-delete path). */
    "   note_doc_id            INTEGER DEFAULT NULL,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
    "   FOREIGN KEY (superseded_by) REFERENCES memory_facts(id) ON DELETE SET NULL,"
    "   FOREIGN KEY (source_conversation_id) REFERENCES conversations(id) ON DELETE SET NULL,"
    "   FOREIGN KEY (subject_entity_id) REFERENCES memory_entities(id) ON DELETE SET NULL,"
    "   FOREIGN KEY (note_doc_id) REFERENCES documents(id) ON DELETE SET NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_memory_facts_user ON memory_facts(user_id);"
    "CREATE INDEX IF NOT EXISTS idx_memory_facts_confidence ON "
    "memory_facts(user_id, confidence DESC);"
    "CREATE INDEX IF NOT EXISTS idx_memory_facts_hash ON memory_facts(user_id, normalized_hash);"
    /* idx_memory_facts_subject: created by the v47 post-migration index block
     * on existing DBs (column doesn't exist until ALTER TABLE runs). */
    /* idx_memory_facts_user_category is created by the v34 migration block (runs
     * after ALTER TABLE adds the column).  Keeping it here would fail on an
     * existing pre-v34 DB because CREATE TABLE IF NOT EXISTS is a no-op for
     * the already-existing table, so the column isn't added until migrations run. */

    /* v48: FTS5 BM25 keyword index.  Contentless — application is responsible
     * for keeping rowid in sync with memory_facts.id and writing
     * pre-stemmed text into fact_stems.  See memory_db.c::fts5_insert_fact_stems_locked /
     * fts5_delete_fact_stems_locked and the v48 migration block for the
     * backfill path.  Tokenizer: unicode61 with diacritic folding so
     * "café" matches "cafe".
     *
     * NOTE: this is a SINGLE global FTS5 index across all users.  The
     * BM25 search statement joins to memory_facts and filters by
     * user_id, so users only see their own rows — but bm25() computes
     * its IDF over the GLOBAL token frequency.  At DAWN's threat-model
     * scale (small trusted user set on a Jetson) the residual side-
     * channel is negligible; if user counts grow significantly, consider
     * per-user FTS5 tables to isolate IDF.  See docs/MEM0_ARCHITECTURAL_PARITY.md. */
    "CREATE VIRTUAL TABLE IF NOT EXISTS memory_facts_fts USING fts5("
    "   fact_stems,"
    "   tokenize='unicode61 remove_diacritics 2',"
    "   content=''"
    ");"

    "CREATE TABLE IF NOT EXISTS memory_preferences ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   user_id INTEGER NOT NULL,"
    "   category TEXT NOT NULL,"
    "   value TEXT NOT NULL,"
    "   confidence REAL DEFAULT 0.5,"
    "   source TEXT DEFAULT 'inferred',"
    "   created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
    "   updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
    "   reinforcement_count INTEGER DEFAULT 1,"
    "   source_conversation_id INTEGER DEFAULT NULL,"
    "   source_msg_id_start    INTEGER DEFAULT NULL,"
    "   source_msg_id_end      INTEGER DEFAULT NULL,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
    "   FOREIGN KEY (source_conversation_id) REFERENCES conversations(id) ON DELETE SET NULL,"
    "   UNIQUE(user_id, category)"
    ");"

    "CREATE TABLE IF NOT EXISTS memory_summaries ("
    "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "   user_id INTEGER NOT NULL,"
    "   session_id TEXT NOT NULL,"
    "   summary TEXT NOT NULL,"
    "   topics TEXT,"
    "   sentiment TEXT,"
    "   created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
    "   message_count INTEGER,"
    "   duration_seconds INTEGER,"
    "   consolidated INTEGER DEFAULT 0,"
    "   source_conversation_id INTEGER DEFAULT NULL,"
    "   source_msg_id_start    INTEGER DEFAULT NULL,"
    "   source_msg_id_end      INTEGER DEFAULT NULL,"
    /* embedding (v45): packed float32 vector for semantic summary search.
     * NULL until embed-at-create fires (or recompute worker backfills on
     * model swap).  The summary adapter (memory_focus_adapters.c) hybrids
     * keyword + cosine when this is populated; NULL rows fall back to
     * keyword-only matching. */
    "   embedding BLOB DEFAULT NULL,"
    "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
    "   FOREIGN KEY (source_conversation_id) REFERENCES conversations(id) ON DELETE SET NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_memory_summaries_user ON "
    "memory_summaries(user_id, created_at DESC);"

    /* Entity/relation tables (v19).  canonical_id + is_user_self added in v43
     * for the entity-merge / user-identity-dedup workstream:
     *   canonical_id  — NULL = self is canonical; non-NULL = soft alias of that
     *                   row's id.  Read paths use COALESCE(canonical_id, id) +
     *                   the partial index idx_memory_entities_canonical to
     *                   enumerate aliases without a JOIN per row.
     *   is_user_self  — exactly one row per user_id may have this = 1 (the
     *                   seeded user-identity entity).  Enforced by the partial
     *                   UNIQUE index idx_memory_entities_user_self below.
     * Both DEFAULT clauses are literal constants so SQLite takes the O(1)
     * metadata-only ALTER path on existing DBs. */
    "CREATE TABLE IF NOT EXISTS memory_entities ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  name TEXT NOT NULL,"
    "  entity_type TEXT NOT NULL,"
    "  canonical_name TEXT NOT NULL,"
    "  embedding BLOB DEFAULT NULL,"
    "  embedding_norm REAL DEFAULT NULL,"
    "  photo_id TEXT DEFAULT NULL,"
    "  first_seen INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
    "  last_seen INTEGER,"
    "  mention_count INTEGER DEFAULT 1,"
    "  canonical_id INTEGER DEFAULT NULL REFERENCES memory_entities(id) ON DELETE SET NULL,"
    "  is_user_self INTEGER NOT NULL DEFAULT 0,"
    "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
    "  UNIQUE(user_id, canonical_name)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_memory_entities_user ON memory_entities(user_id);"
    /* idx_memory_entities_canonical (partial, canonical_id IS NOT NULL) and
     * idx_memory_entities_user_self (partial UNIQUE, is_user_self = 1) are
     * created by the v43 post-migration index block — same reason as the v33
     * pattern: on an existing pre-v43 DB the columns don't exist until the
     * ALTER fires, so the indexes can't live in SCHEMA_SQL. */

    /* memory_relations: valid_from/valid_to added in v33.  source_* added in v40.
     * mention_count added in v49 (re-witness counter; upsert via
     * idx_memory_relations_unique_open bumps it).  NULL = open-ended (no bound).
     * "currently true" predicate: valid_to IS NULL OR valid_to > now() */
    "CREATE TABLE IF NOT EXISTS memory_relations ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  subject_entity_id INTEGER NOT NULL,"
    "  relation TEXT NOT NULL,"
    "  object_entity_id INTEGER,"
    "  object_value TEXT,"
    "  fact_id INTEGER,"
    "  confidence REAL DEFAULT 0.8,"
    "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
    "  valid_from INTEGER DEFAULT NULL,"
    "  valid_to INTEGER DEFAULT NULL,"
    "  source_conversation_id INTEGER DEFAULT NULL,"
    "  source_msg_id_start    INTEGER DEFAULT NULL,"
    "  source_msg_id_end      INTEGER DEFAULT NULL,"
    "  mention_count          INTEGER NOT NULL DEFAULT 1,"
    "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
    "  FOREIGN KEY (subject_entity_id) REFERENCES memory_entities(id) ON DELETE CASCADE,"
    "  FOREIGN KEY (object_entity_id) REFERENCES memory_entities(id) ON DELETE SET NULL,"
    "  FOREIGN KEY (fact_id) REFERENCES memory_facts(id) ON DELETE SET NULL,"
    "  FOREIGN KEY (source_conversation_id) REFERENCES conversations(id) ON DELETE SET NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_memory_relations_subject ON "
    "memory_relations(subject_entity_id);"
    "CREATE INDEX IF NOT EXISTS idx_memory_relations_object ON memory_relations(object_entity_id);"
    "CREATE INDEX IF NOT EXISTS idx_memory_relations_user ON memory_relations(user_id);"
    /* idx_memory_relations_user_validity + idx_memory_relations_subject_open are
     * created by the v33 migration block (same reason — runs after the
     * valid_from/valid_to ALTER so the columns exist).
     * idx_memory_relations_unique_open is created by the v49 migration block
     * (and below in the fresh-install index pass). */

    /* Entity-alias audit log (v43) — append-only history of soft/hard merges.
     * Not consulted on hot read paths (those use canonical_id JOIN); this
     * table answers "why was X linked to Y, and when?" for the WebUI Graph tab
     * and dawn-admin memory entity history.  source_entity_id is SET NULL on
     * hard-merge so the row survives the source-row deletion; the preserved
     * source_canonical_name keeps the audit row self-describing. */
    "CREATE TABLE IF NOT EXISTS memory_entity_aliases ("
    "  id                    INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id               INTEGER NOT NULL,"
    "  source_entity_id      INTEGER,"
    "  target_entity_id      INTEGER NOT NULL,"
    "  source_canonical_name TEXT NOT NULL,"
    "  target_canonical_name TEXT NOT NULL,"
    "  link_kind             TEXT NOT NULL,"
    "  reason                TEXT NOT NULL,"
    "  composite_score       REAL,"
    "  evidence_json         TEXT,"
    "  linked_at             INTEGER NOT NULL,"
    "  consolidated_at       INTEGER,"
    "  unlinked_at           INTEGER,"
    "  unlink_reason         TEXT,"
    "  FOREIGN KEY (user_id)          REFERENCES users(id)            ON DELETE CASCADE,"
    "  FOREIGN KEY (source_entity_id) REFERENCES memory_entities(id)  ON DELETE SET NULL,"
    "  FOREIGN KEY (target_entity_id) REFERENCES memory_entities(id)  ON DELETE SET NULL"
    ");"
    /* idx_memory_entity_aliases_user_target is created by the v43 post-migration
     * index block so it is established for both fresh installs and upgrades. */

    /* Mid-confidence merge proposal queue (v43) — review band staging for the
     * auto-merge gate (Phase 2).  Approving a proposal writes the soft link
     * via the regular alias path; rejecting just stamps resolved_at.  Cleared
     * by reextract along with memory_entity_aliases — both are derived state. */
    "CREATE TABLE IF NOT EXISTS memory_entity_merge_proposals ("
    "  id               INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id          INTEGER NOT NULL,"
    "  source_entity_id INTEGER NOT NULL,"
    "  target_entity_id INTEGER NOT NULL,"
    "  composite_score  REAL NOT NULL,"
    "  evidence_json    TEXT NOT NULL,"
    "  proposed_at      INTEGER NOT NULL,"
    "  resolved_at      INTEGER,"
    "  resolution       TEXT,"
    "  FOREIGN KEY (user_id)          REFERENCES users(id)            ON DELETE CASCADE,"
    "  FOREIGN KEY (source_entity_id) REFERENCES memory_entities(id)  ON DELETE CASCADE,"
    "  FOREIGN KEY (target_entity_id) REFERENCES memory_entities(id)  ON DELETE CASCADE"
    ");"
    /* idx_merge_proposals_pending is created by the v43 post-migration index
     * block. */

    /* Scheduler events (v18) */
    "CREATE TABLE IF NOT EXISTS scheduled_events ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  event_type TEXT NOT NULL DEFAULT 'timer',"
    "  status TEXT NOT NULL DEFAULT 'pending',"
    "  name TEXT NOT NULL,"
    "  message TEXT,"
    "  fire_at INTEGER NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  duration_sec INTEGER DEFAULT 0,"
    "  snoozed_until INTEGER DEFAULT 0,"
    "  recurrence TEXT DEFAULT 'once',"
    "  recurrence_days TEXT,"
    "  original_time TEXT,"
    "  source_uuid TEXT,"
    "  source_location TEXT,"
    "  source_client_type INTEGER DEFAULT 0,"
    "  announce_all INTEGER DEFAULT 0,"
    "  tool_name TEXT,"
    "  tool_action TEXT,"
    "  tool_value TEXT,"
    "  fired_at INTEGER DEFAULT 0,"
    "  snooze_count INTEGER DEFAULT 0,"
    "  say_aloud INTEGER NOT NULL DEFAULT 0," /* v53 — tri-state TTS override (briefings) */
    "  deliver_to TEXT,"                      /* v54 — messaging channel for fan-out (optional) */
    "  FOREIGN KEY (user_id) REFERENCES users(id)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_sched_status_fire ON scheduled_events(status, fire_at);"
    "CREATE INDEX IF NOT EXISTS idx_sched_user ON scheduled_events(user_id, status);"
    "CREATE INDEX IF NOT EXISTS idx_sched_user_name ON scheduled_events(user_id, status, name);"
    "CREATE INDEX IF NOT EXISTS idx_sched_source ON scheduled_events(source_uuid);"

    /* Multi-step briefing steps (v50).  One row per step; briefings without
     * a row here are either single-tool legacy rows (read tool_name from
     * scheduled_events) or no-ops.  FK ON DELETE CASCADE so steps go with
     * their parent event when scheduler_db_cleanup_old_events purges. */
    "CREATE TABLE IF NOT EXISTS briefing_steps ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  event_id INTEGER NOT NULL,"
    "  seq INTEGER NOT NULL,"
    "  tool_name TEXT NOT NULL,"
    "  tool_action TEXT NOT NULL DEFAULT '',"
    "  tool_value TEXT NOT NULL DEFAULT '',"
    "  FOREIGN KEY (event_id) REFERENCES scheduled_events(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_briefing_steps_event ON briefing_steps(event_id, seq);"

    /* Missed scheduler notifications (v32) — queued when a ringing event has no
     * connected clients for the target user; replayed on reconnect. */
    "CREATE TABLE IF NOT EXISTS missed_notifications ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  event_id INTEGER NOT NULL,"
    "  event_type TEXT NOT NULL,"
    "  status TEXT NOT NULL,"
    "  name TEXT NOT NULL,"
    "  message TEXT,"
    "  fire_at INTEGER NOT NULL,"
    "  conversation_id INTEGER DEFAULT 0,"
    "  created_at INTEGER NOT NULL,"
    "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_missed_notif_user "
    "  ON missed_notifications(user_id, created_at);"

    /* Documents and chunks for RAG search (v22) */
    "CREATE TABLE IF NOT EXISTS documents ("
    "  id INTEGER PRIMARY KEY,"
    "  user_id INTEGER,"
    "  filename TEXT NOT NULL,"
    "  filepath TEXT NOT NULL,"
    "  filetype TEXT NOT NULL,"
    "  file_hash TEXT NOT NULL,"
    "  num_chunks INTEGER NOT NULL,"
    "  is_global INTEGER DEFAULT 0,"
    "  created_at INTEGER NOT NULL,"
    /* original_blob_id (v68): the stored source file in `blobs`.  ON DELETE SET
     * NULL so deleting a blob clears the link; deleting a document leaves the
     * blob to the orphan sweep.  Migrated DBs get a plain TEXT column (SQLite
     * can't ALTER-add an FK) with the link enforced in C. */
    "  original_blob_id TEXT DEFAULT NULL,"
    "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE,"
    "  FOREIGN KEY(original_blob_id) REFERENCES blobs(id) ON DELETE SET NULL"
    ");"
    /* document_chunks.created_at added in v35 — used by temporal-query scoring to
     * boost chunks whose origin date is near the user's referenced point in time
     * (e.g., "what did we discuss in summer 2021"). 0 = unknown (no boost). */
    "CREATE TABLE IF NOT EXISTS document_chunks ("
    "  id INTEGER PRIMARY KEY,"
    "  document_id INTEGER NOT NULL,"
    "  chunk_index INTEGER NOT NULL,"
    "  text TEXT NOT NULL,"
    "  embedding BLOB NOT NULL,"
    "  embedding_norm REAL NOT NULL,"
    "  created_at INTEGER NOT NULL DEFAULT 0,"
    "  FOREIGN KEY(document_id) REFERENCES documents(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_doc_chunks_doc ON document_chunks(document_id);"
    "CREATE INDEX IF NOT EXISTS idx_documents_user ON documents(user_id);"
    "CREATE INDEX IF NOT EXISTS idx_documents_hash ON documents(file_hash);"
    /* idx_documents_original_blob is created by the v68 migration (auth_db_migrations_v68.c),
     * NOT here: this base SCHEMA_SQL runs before migrations, so on an existing pre-v68 DB the
     * documents.original_blob_id column doesn't exist yet and indexing it would fail.  The v68
     * migration adds the column first, then the index, and also runs on fresh installs. */

    /* v61: FTS5 BM25 keyword index over document chunks — the lexical retrieval
     * channel that runs as its OWN candidate set (fused with semantic in
     * document_search), and the exact-label path that makes "notes" (single-chunk
     * documents whose filename IS the label) reliably retrievable.  TWO weighted
     * columns: label_stems (the filename/label, weighted high at query time) and
     * body_stems (the chunk text).  Contentless — the application keeps rowid in
     * sync with document_chunks.id and writes pre-stemmed text; see
     * document_db.c::fts5_insert/delete_chunk_stems_locked and the v61 migration
     * backfill.  Mirrors memory_facts_fts, including the SINGLE global index /
     * global-IDF caveat: bm25() IDF is computed across all users; per-user safety
     * is the JOIN to documents + (user_id = ? OR is_global = 1) filter in the BM25
     * search statement, not the index.  See docs/MEM0_ARCHITECTURAL_PARITY.md. */
    "CREATE VIRTUAL TABLE IF NOT EXISTS document_chunks_fts USING fts5("
    "   label_stems,"
    "   body_stems,"
    "   tokenize='unicode61 remove_diacritics 2',"
    "   content=''"
    ");"

    /* v62: document_versions — soft-archive of a document/note's content captured
     * BEFORE every destructive mutation (overwrite, edit/append, delete), so a
     * change or deletion can be undone within a retention window (mirrors the
     * email-trash pattern).  Deliberately NO foreign key to documents(id): a
     * version must OUTLIVE its document so a DELETED note can still be restored —
     * document_id is the (possibly-dangling) original id.  user_id keeps it
     * owner-scoped and lets a deleted user's versions cascade away. */
    "CREATE TABLE IF NOT EXISTS document_versions ("
    "  id INTEGER PRIMARY KEY,"
    "  document_id INTEGER NOT NULL,"
    "  user_id INTEGER NOT NULL,"
    "  filename TEXT NOT NULL,"
    "  text TEXT NOT NULL,"
    "  archived_at INTEGER NOT NULL,"
    "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_doc_versions_doc "
    "ON document_versions(document_id, archived_at DESC);"

    /* v63: document_full_text — the canonical, un-chunked text of a multi-chunk
     * document, kept so surgical edits (find/replace) can be applied to the whole
     * text and the document re-chunked + re-embedded.  Single-chunk notes don't
     * need it (the one chunk IS the full text).  Side table (not a column on
     * documents) so the wide documents row stays lean and full_text — which can
     * be tens of KB — is only JOINed when editing.  Cascades with the document. */
    "CREATE TABLE IF NOT EXISTS document_full_text ("
    "  document_id INTEGER PRIMARY KEY,"
    "  text TEXT NOT NULL,"
    "  FOREIGN KEY(document_id) REFERENCES documents(id) ON DELETE CASCADE"
    ");"

    /* Calendar tables (v23, read_only from v24, oauth from v25) */
    "CREATE TABLE IF NOT EXISTS calendar_accounts ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  name TEXT NOT NULL,"
    "  caldav_url TEXT NOT NULL,"
    "  username TEXT NOT NULL,"
    "  encrypted_password BLOB NOT NULL,"
    "  auth_type TEXT DEFAULT 'basic',"
    "  principal_url TEXT DEFAULT '',"
    "  calendar_home_url TEXT DEFAULT '',"
    "  enabled INTEGER DEFAULT 1,"
    "  read_only INTEGER DEFAULT 0,"
    "  last_sync INTEGER DEFAULT 0,"
    "  sync_interval_sec INTEGER DEFAULT 900,"
    "  created_at INTEGER NOT NULL,"
    "  oauth_account_key TEXT DEFAULT '',"
    "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_cal_acct_user ON calendar_accounts(user_id);"

    "CREATE TABLE IF NOT EXISTS calendar_calendars ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  account_id INTEGER NOT NULL,"
    "  caldav_path TEXT NOT NULL,"
    "  display_name TEXT DEFAULT '',"
    "  color TEXT DEFAULT '',"
    "  is_active INTEGER DEFAULT 1,"
    "  ctag TEXT DEFAULT '',"
    "  created_at INTEGER NOT NULL,"
    "  FOREIGN KEY(account_id) REFERENCES calendar_accounts(id) ON DELETE CASCADE"
    ");"

    "CREATE TABLE IF NOT EXISTS calendar_events ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  calendar_id INTEGER NOT NULL,"
    "  uid TEXT NOT NULL,"
    "  etag TEXT DEFAULT '',"
    "  summary TEXT DEFAULT '',"
    "  description TEXT DEFAULT '',"
    "  location TEXT DEFAULT '',"
    "  dtstart INTEGER DEFAULT 0,"
    "  dtend INTEGER DEFAULT 0,"
    "  duration_sec INTEGER DEFAULT 0,"
    "  all_day INTEGER DEFAULT 0,"
    "  dtstart_date TEXT DEFAULT '',"
    "  dtend_date TEXT DEFAULT '',"
    "  rrule TEXT DEFAULT '',"
    "  raw_ical TEXT,"
    "  last_synced INTEGER DEFAULT 0,"
    "  FOREIGN KEY(calendar_id) REFERENCES calendar_calendars(id) ON DELETE CASCADE"
    ");"
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_cal_events_uid ON calendar_events(calendar_id, uid);"

    "CREATE TABLE IF NOT EXISTS calendar_occurrences ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  event_id INTEGER NOT NULL,"
    "  dtstart INTEGER DEFAULT 0,"
    "  dtend INTEGER DEFAULT 0,"
    "  all_day INTEGER DEFAULT 0,"
    "  dtstart_date TEXT DEFAULT '',"
    "  dtend_date TEXT DEFAULT '',"
    "  summary TEXT DEFAULT '',"
    "  location TEXT DEFAULT '',"
    "  is_override INTEGER DEFAULT 0,"
    "  is_cancelled INTEGER DEFAULT 0,"
    "  recurrence_id TEXT DEFAULT '',"
    "  FOREIGN KEY(event_id) REFERENCES calendar_events(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_cal_occ_event ON calendar_occurrences(event_id);"
    "CREATE INDEX IF NOT EXISTS idx_cal_occ_time ON calendar_occurrences(dtstart, dtend);"
    "CREATE INDEX IF NOT EXISTS idx_cal_occ_date ON calendar_occurrences(dtstart_date);"

    /* OAuth token storage (v25) */
    "CREATE TABLE IF NOT EXISTS oauth_tokens ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  provider TEXT NOT NULL,"
    "  account_key TEXT NOT NULL,"
    "  encrypted_data BLOB NOT NULL,"
    "  encrypted_data_len INTEGER NOT NULL,"
    "  scopes TEXT DEFAULT '',"
    "  created_at INTEGER NOT NULL,"
    "  updated_at INTEGER NOT NULL,"
    "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE,"
    "  UNIQUE(user_id, provider, account_key)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_oauth_user_provider ON oauth_tokens(user_id, provider);"

    /* Contacts (v26) */
    "CREATE TABLE IF NOT EXISTS contacts ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  entity_id INTEGER NOT NULL,"
    "  field_type TEXT NOT NULL,"
    "  value TEXT NOT NULL,"
    "  label TEXT DEFAULT '',"
    "  created_at INTEGER NOT NULL,"
    "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE,"
    "  FOREIGN KEY(entity_id) REFERENCES memory_entities(id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_contacts_entity ON contacts(entity_id);"
    "CREATE INDEX IF NOT EXISTS idx_contacts_user_type ON contacts(user_id, field_type);"

    /* Email accounts (v26) */
    "CREATE TABLE IF NOT EXISTS email_accounts ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  name TEXT NOT NULL,"
    "  imap_server TEXT NOT NULL,"
    "  imap_port INTEGER DEFAULT 993,"
    "  imap_ssl INTEGER DEFAULT 1,"
    "  smtp_server TEXT NOT NULL,"
    "  smtp_port INTEGER DEFAULT 465,"
    "  smtp_ssl INTEGER DEFAULT 1,"
    "  username TEXT NOT NULL,"
    "  display_name TEXT DEFAULT '',"
    "  encrypted_password BLOB,"
    "  encrypted_password_len INTEGER DEFAULT 0,"
    "  auth_type TEXT DEFAULT 'app_password',"
    "  oauth_account_key TEXT DEFAULT '',"
    "  enabled INTEGER DEFAULT 1,"
    "  read_only INTEGER DEFAULT 0,"
    "  max_recent INTEGER DEFAULT 10,"
    "  max_body_chars INTEGER DEFAULT " STRINGIFY(
        EMAIL_DEFAULT_BODY_CHARS) ","
                                  "  created_at INTEGER NOT NULL,"
                                  "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE"
                                  ");"
                                  "CREATE INDEX IF NOT EXISTS idx_email_acct_user ON "
                                  "email_accounts(user_id);"

                                  /* Phone call and SMS logs (v29) */
                                  "CREATE TABLE IF NOT EXISTS phone_call_log ("
                                  "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                  "  user_id INTEGER NOT NULL,"
                                  "  direction INTEGER NOT NULL,"
                                  "  number TEXT NOT NULL,"
                                  "  contact_name TEXT DEFAULT '',"
                                  "  duration_sec INTEGER DEFAULT 0,"
                                  "  timestamp INTEGER NOT NULL,"
                                  "  status INTEGER NOT NULL"
                                  ");"
                                  "CREATE INDEX IF NOT EXISTS idx_phone_call_user_ts "
                                  "  ON phone_call_log(user_id, timestamp DESC);"
                                  "CREATE TABLE IF NOT EXISTS phone_sms_log ("
                                  "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                  "  user_id INTEGER NOT NULL,"
                                  "  direction INTEGER NOT NULL,"
                                  "  number TEXT NOT NULL,"
                                  "  contact_name TEXT DEFAULT '',"
                                  "  body TEXT NOT NULL,"
                                  "  timestamp INTEGER NOT NULL,"
                                  "  read INTEGER DEFAULT 0,"
                                  "  image_id TEXT DEFAULT NULL"
                                  ");"
                                  "CREATE INDEX IF NOT EXISTS idx_phone_sms_user_ts "
                                  "  ON phone_sms_log(user_id, timestamp DESC);"
                                  "CREATE INDEX IF NOT EXISTS idx_phone_sms_unread "
                                  "  ON phone_sms_log(user_id, read) WHERE read = 0;"

                                  /* Messaging channels (v51) — per-user binding of an external chat
                                   * platform identity (Telegram chat_id / Discord channel_id /
                                   * Slack channel_id / SMS E.164) to a DAWN user.  See
                                   * docs/MESSAGING_CHANNELS_DESIGN.md §5. */
                                  "CREATE TABLE IF NOT EXISTS messaging_channels ("
                                  "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                  "  user_id INTEGER NOT NULL,"
                                  "  provider TEXT NOT NULL "
                                  "    CHECK(provider IN ('telegram','discord','slack','sms')),"
                                  "  provider_address TEXT NOT NULL,"
                                  "  address_json TEXT NOT NULL,"
                                  "  display_name TEXT,"
                                  "  credentials_ref TEXT,"
                                  "  is_enabled INTEGER NOT NULL DEFAULT 1,"
                                  "  rate_limit_per_min INTEGER NOT NULL DEFAULT 10,"
                                  "  rate_limit_per_day INTEGER NOT NULL DEFAULT 200,"
                                  "  created_at INTEGER NOT NULL,"
                                  "  last_used_at INTEGER,"
                                  /* conversation_id (v52): forever-binding to a conversations row.
                                   * NULL = no conversation yet (next inbound will create one).  Set
                                   * non-NULL after the first inbound and reused for every
                                   * subsequent turn on this channel until the user issues /new
                                   * (which clears it back to NULL).  LCM handles context compaction
                                   * in-place so a single conv row can live indefinitely; the
                                   * recovery worker extracts memory incrementally via
                                   * last_extracted_msg_id. */
                                  "  conversation_id INTEGER DEFAULT NULL,"
                                  "  UNIQUE(user_id, provider, provider_address),"
                                  "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
                                  "  FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON "
                                  "DELETE SET NULL"
                                  ");"
                                  "CREATE INDEX IF NOT EXISTS idx_messaging_channels_user "
                                  "  ON messaging_channels(user_id);"
                                  "CREATE INDEX IF NOT EXISTS idx_messaging_channels_provider_addr "
                                  "  ON messaging_channels(provider, provider_address);"
                                  "CREATE INDEX IF NOT EXISTS idx_messaging_channels_active "
                                  "  ON messaging_channels(provider, provider_address) WHERE "
                                  "is_enabled = 1;"

                                  /* Pending link codes (v51) — short-lived (10-minute TTL) one-time
                                   * codes generated in the WebUI and claimed when the user sends
                                   * `/link CODE` from the chat app. */
                                  "CREATE TABLE IF NOT EXISTS messaging_link_codes ("
                                  "  code TEXT PRIMARY KEY,"
                                  "  user_id INTEGER NOT NULL,"
                                  "  provider_hint TEXT "
                                  "    CHECK(provider_hint IN ('telegram','discord','slack','sms') "
                                  "OR provider_hint IS NULL),"
                                  "  created_at INTEGER NOT NULL,"
                                  "  expires_at INTEGER NOT NULL,"
                                  "  claimed_at INTEGER,"
                                  "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
                                  ");"
                                  "CREATE INDEX IF NOT EXISTS idx_messaging_link_codes_expires "
                                  "  ON messaging_link_codes(expires_at);"

                                  /* Link attempts audit log (v51) — every /link attempt, successful
                                   * or not, for post-hoc abuse review.  7-day TTL via periodic
                                   * sweep. */
                                  "CREATE TABLE IF NOT EXISTS messaging_link_attempts ("
                                  "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                  "  provider TEXT NOT NULL,"
                                  "  sender_address TEXT NOT NULL,"
                                  "  code_tried TEXT,"
                                  "  result TEXT NOT NULL,"
                                  "  created_at INTEGER NOT NULL"
                                  ");"
                                  "CREATE INDEX IF NOT EXISTS idx_messaging_link_attempts_recent "
                                  "  ON messaging_link_attempts(provider, sender_address, "
                                  "created_at);";

/* =============================================================================
 * Schema Version and Migration
 * ============================================================================= */

static int get_current_schema_version(void) {
   sqlite3_stmt *stmt = NULL;
   int version = 0;

   int rc = sqlite3_prepare_v2(s_db.db, "SELECT version FROM schema_version LIMIT 1", -1, &stmt,
                               NULL);
   if (rc == SQLITE_OK) {
      if (sqlite3_step(stmt) == SQLITE_ROW) {
         version = sqlite3_column_int(stmt, 0);
      }
      sqlite3_finalize(stmt);
   }
   return version;
}

int auth_db_create_schema(const char *db_path) {
   char *errmsg = NULL;

   /* Check current schema version (0 if fresh install) */
   int current_version = get_current_schema_version();

   /* Back up the database before any upgrade touches it.  Only for a real
    * version bump on an existing DB (fresh installs have nothing to lose).  A
    * failed backup ABORTS the upgrade — we never migrate unprotected. */
   if (current_version > 0 && current_version < AUTH_DB_SCHEMA_VERSION) {
      if (auth_db_backup_before_upgrade(db_path, current_version) != AUTH_DB_SUCCESS) {
         OLOG_ERROR("auth_db: aborting upgrade v%d->v%d — pre-upgrade backup failed",
                    current_version, AUTH_DB_SCHEMA_VERSION);
         return AUTH_DB_FAILURE;
      }
   }

   /* Execute schema SQL - all tables use IF NOT EXISTS for idempotency */
   int rc = sqlite3_exec(s_db.db, SCHEMA_SQL, NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("auth_db: schema creation failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }

   /* Per-version migration ladder lives in auth_db_migrations.c to keep this
    * file under the size limit (see auth_db_apply_migrations). */
   return auth_db_apply_migrations(current_version, db_path);
}
