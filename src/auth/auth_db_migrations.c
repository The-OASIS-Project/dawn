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
 * Authentication Database Migration Ladder
 *
 * Owns the per-version migration steps (v3 .. vN) applied on top of the base
 * SCHEMA_SQL.  Split out from auth_db_schema.c to keep that file under the size
 * limits in CLAUDE.md.  The base schema + orchestrator entry point
 * (auth_db_create_schema) stay in auth_db_schema.c; this file is reached only
 * via auth_db_apply_migrations().
 *
 * FILE SIZE (deliberate): this ladder exceeds the 2,500-line hard limit by
 * design and grows monotonically — it is an append-only historical record (one
 * block per shipped schema version, never edited once shipped), so it reads as a
 * changelog rather than active logic.  When it next needs cutting, group the
 * blocks into era helpers (e.g. apply_v1_v40() / apply_v41_vN()) called from
 * auth_db_apply_migrations(); do NOT reflow or merge the historical blocks.
 *
 * SECURITY: All database operations use prepared statements or constant SQL.
 * NEVER use sqlite3_exec() or sqlite3_mprintf() with user input.
 * See: CWE-89, OWASP SQL Injection Prevention Cheat Sheet
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "auth/auth_db_migrations.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "auth/auth_db_internal.h"
#include "logging.h"
#include "memory/memory_stem.h"

/* Apply the per-version migration ladder on top of SCHEMA_SQL.  Bumps
 * schema_version to AUTH_DB_SCHEMA_VERSION only when every gating step
 * succeeded; otherwise holds the version for retry on next boot.  Called by
 * auth_db_create_schema() after the base schema has been (re)created. */
int auth_db_apply_migrations(int current_version, const char *db_path) {
   char *errmsg = NULL;
   int rc = 0;
   /* v3 migration: add persona_mode column to user_settings if missing
    * This handles upgrades from v1 or v2 where the table may exist without this column */
   if (current_version >= 1 && current_version < 3) {
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE user_settings ADD COLUMN persona_mode TEXT DEFAULT 'append'",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         /* Column might already exist or table might not exist yet - not fatal */
         OLOG_INFO("auth_db: v3 migration note: %s (may be normal)", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added persona_mode column to user_settings");
      }
   }

   /* v5 migration: add context_tokens and context_max columns to conversations
    * Only runs if conversations table already exists (v4+) without these columns */
   if (current_version >= 1 && current_version < 5) {
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE conversations ADD COLUMN context_tokens INTEGER DEFAULT 0",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_INFO("auth_db: v5 migration note (context_tokens): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      }
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE conversations ADD COLUMN context_max INTEGER DEFAULT 0", NULL,
                        NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_INFO("auth_db: v5 migration note (context_max): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added context columns to conversations");
      }
   }

   /* v6 migration: update messages table CHECK constraint to include 'tool' role
    * SQLite doesn't support ALTER TABLE to modify constraints, so we recreate the table */
   if (current_version >= 4 && current_version < 6) {
      OLOG_INFO("auth_db: migrating messages table to support 'tool' role");
      const char *migration_sql =
          "BEGIN TRANSACTION;"
          "CREATE TABLE messages_new ("
          "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "   conversation_id INTEGER NOT NULL,"
          "   role TEXT NOT NULL CHECK(role IN ('system', 'user', 'assistant', 'tool')),"
          "   content TEXT NOT NULL,"
          "   created_at INTEGER NOT NULL,"
          "   FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE"
          ");"
          "INSERT INTO messages_new SELECT * FROM messages;"
          "DROP TABLE messages;"
          "ALTER TABLE messages_new RENAME TO messages;"
          "CREATE INDEX IF NOT EXISTS idx_messages_conversation ON messages(conversation_id, id "
          "ASC);"
          "COMMIT;";

      rc = sqlite3_exec(s_db.db, migration_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v6 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
         /* Rollback on failure */
         sqlite3_exec(s_db.db, "ROLLBACK;", NULL, NULL, NULL);
      } else {
         OLOG_INFO("auth_db: migrated messages table to v6 (added 'tool' role)");
      }
   }

   /* v7 migration: add continued_from and compaction_summary columns to conversations
    * These support conversation continuation when context compaction occurs */
   if (current_version >= 4 && current_version < 7) {
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE conversations ADD COLUMN continued_from INTEGER DEFAULT NULL "
                        "REFERENCES conversations(id) ON DELETE SET NULL",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_INFO("auth_db: v7 migration note (continued_from): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      }
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE conversations ADD COLUMN compaction_summary TEXT DEFAULT NULL",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_INFO("auth_db: v7 migration note (compaction_summary): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      }
      /* Add index for finding child conversations */
      rc = sqlite3_exec(
          s_db.db,
          "CREATE INDEX IF NOT EXISTS idx_conversations_continued ON conversations(continued_from)",
          NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_INFO("auth_db: v7 migration note (index): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added continuation columns to conversations (v7)");
      }
   }

   /* v8 migration: session_metrics table
    * The table is created by SCHEMA_SQL with IF NOT EXISTS, so no explicit
    * migration is needed. Just log the upgrade for existing databases. */
   if (current_version >= 1 && current_version < 8) {
      OLOG_INFO("auth_db: added session_metrics table (v8)");
   }

   /* v9 migration: add theme column to user_settings */
   if (current_version >= 1 && current_version < 9) {
      rc = sqlite3_exec(s_db.db, "ALTER TABLE user_settings ADD COLUMN theme TEXT DEFAULT 'cyan'",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_INFO("auth_db: v9 migration note (theme): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added theme column to user_settings");
      }
   }

   /* v10 migration: add expires_at column to sessions for "Remember Me" feature
    * Existing sessions get expires_at = last_activity + 24 hours */
   if (current_version >= 1 && current_version < 10) {
      rc = sqlite3_exec(s_db.db, "ALTER TABLE sessions ADD COLUMN expires_at INTEGER", NULL, NULL,
                        &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_INFO("auth_db: v10 migration note (expires_at): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         /* Set default expires_at for existing sessions (last_activity + 24h) */
         char update_sql[128];
         snprintf(update_sql, sizeof(update_sql),
                  "UPDATE sessions SET expires_at = last_activity + %d WHERE expires_at IS NULL",
                  AUTH_SESSION_TIMEOUT_SEC);
         rc = sqlite3_exec(s_db.db, update_sql, NULL, NULL, &errmsg);
         if (rc != SQLITE_OK) {
            OLOG_WARNING("auth_db: v10 migration (set defaults): %s", errmsg ? errmsg : "ok");
            sqlite3_free(errmsg);
            errmsg = NULL;
         }
         OLOG_INFO("auth_db: added expires_at column to sessions (v10)");
      }
      /* Create index for efficient cleanup queries */
      rc = sqlite3_exec(s_db.db,
                        "CREATE INDEX IF NOT EXISTS idx_sessions_expires ON sessions(expires_at)",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_INFO("auth_db: v10 migration (index): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      }
   }

   /* v11 migration: add per-conversation LLM settings columns */
   if (current_version >= 4 && current_version < 11) {
      const char *cols[] = {
         "ALTER TABLE conversations ADD COLUMN llm_type TEXT DEFAULT NULL",
         "ALTER TABLE conversations ADD COLUMN cloud_provider TEXT DEFAULT NULL",
         "ALTER TABLE conversations ADD COLUMN model TEXT DEFAULT NULL",
         "ALTER TABLE conversations ADD COLUMN tools_mode TEXT DEFAULT NULL",
         "ALTER TABLE conversations ADD COLUMN thinking_mode TEXT DEFAULT NULL"
      };
      for (int i = 0; i < 5; i++) {
         rc = sqlite3_exec(s_db.db, cols[i], NULL, NULL, &errmsg);
         if (rc != SQLITE_OK) {
            OLOG_INFO("auth_db: v11 migration note: %s", errmsg ? errmsg : "ok");
            sqlite3_free(errmsg);
            errmsg = NULL;
         }
      }
      OLOG_INFO("auth_db: added LLM settings columns to conversations (v11)");
   }

   /* v12 migration: images table for vision uploads (now superseded by v13) */
   if (current_version >= 1 && current_version < 12) {
      OLOG_INFO("auth_db: added images table for vision uploads (v12)");
   }

   /* v13 migration: add data BLOB column to images table
    * Since v12 images table didn't have the data column, we need to recreate it.
    * Drop existing table (likely empty) and let SCHEMA_SQL recreate with data column. */
   if (current_version == 12) {
      rc = sqlite3_exec(s_db.db, "DROP TABLE IF EXISTS images", NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_WARNING("auth_db: v13 migration - failed to drop images: %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      }
      /* Recreate with data column (from SCHEMA_SQL) */
      const char *images_sql =
          "CREATE TABLE IF NOT EXISTS images ("
          "   id TEXT PRIMARY KEY,"
          "   user_id INTEGER NOT NULL,"
          "   mime_type TEXT NOT NULL,"
          "   size INTEGER NOT NULL,"
          "   data BLOB NOT NULL,"
          "   created_at INTEGER NOT NULL,"
          "   last_accessed INTEGER,"
          "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
          ");"
          "CREATE INDEX IF NOT EXISTS idx_images_user ON images(user_id);"
          "CREATE INDEX IF NOT EXISTS idx_images_created ON images(created_at);";
      rc = sqlite3_exec(s_db.db, images_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v13 migration - failed to create images: %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: migrated images table to include BLOB storage (v13)");
   }

   /* v14 migration: add memory system tables
    * Creates memory_facts, memory_preferences, and memory_summaries tables */
   if (current_version >= 1 && current_version < 14) {
      const char *memory_sql =
          /* memory_facts table */
          "CREATE TABLE IF NOT EXISTS memory_facts ("
          "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "   user_id INTEGER NOT NULL,"
          "   fact_text TEXT NOT NULL,"
          "   confidence REAL DEFAULT 1.0,"
          "   source TEXT DEFAULT 'inferred',"
          "   created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
          "   last_accessed INTEGER,"
          "   access_count INTEGER DEFAULT 0,"
          "   superseded_by INTEGER,"
          "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
          "   FOREIGN KEY (superseded_by) REFERENCES memory_facts(id) ON DELETE SET NULL"
          ");"
          "CREATE INDEX IF NOT EXISTS idx_memory_facts_user ON memory_facts(user_id);"
          "CREATE INDEX IF NOT EXISTS idx_memory_facts_confidence ON "
          "memory_facts(user_id, confidence DESC);"

          /* memory_preferences table */
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
          "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
          "   UNIQUE(user_id, category)"
          ");"

          /* memory_summaries table */
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
          "   FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
          ");"
          "CREATE INDEX IF NOT EXISTS idx_memory_summaries_user ON "
          "memory_summaries(user_id, created_at DESC);";

      rc = sqlite3_exec(s_db.db, memory_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v14 migration - failed to create memory tables: %s",
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: added memory system tables (v14)");
   }

   /* v15 migration: add deduplication and extraction tracking
    * - normalized_hash for fast duplicate detection in memory_facts
    * - last_extracted_msg_count for incremental extraction in conversations */
   if (current_version >= 1 && current_version < 15) {
      const char *v15_sql =
          "ALTER TABLE memory_facts ADD COLUMN normalized_hash INTEGER DEFAULT 0;"
          "CREATE INDEX IF NOT EXISTS idx_memory_facts_hash ON memory_facts(user_id, "
          "normalized_hash);"
          "ALTER TABLE conversations ADD COLUMN last_extracted_msg_count INTEGER DEFAULT 0;";

      rc = sqlite3_exec(s_db.db, v15_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v15 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: added deduplication and extraction tracking (v15)");
   }

   /* v16 migration: add is_private flag to conversations for privacy mode */
   if (current_version >= 1 && current_version < 16) {
      const char *v16_sql = "ALTER TABLE conversations ADD COLUMN is_private INTEGER DEFAULT 0;";

      rc = sqlite3_exec(s_db.db, v16_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v16 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: added conversation privacy flag (v16)");
   }

   /* v17 migration: add origin column to conversations for voice/webui distinction */
   if (current_version >= 1 && current_version < 17) {
      const char *v17_sql = "ALTER TABLE conversations ADD COLUMN origin TEXT DEFAULT 'webui';";

      rc = sqlite3_exec(s_db.db, v17_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v17 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: added conversation origin column (v17)");
   }

   /* v18 migration: scheduler events table */
   if (current_version >= 1 && current_version < 18) {
      const char *v18_sql = "CREATE TABLE IF NOT EXISTS scheduled_events ("
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
                            "  announce_all INTEGER DEFAULT 0,"
                            "  tool_name TEXT,"
                            "  tool_action TEXT,"
                            "  tool_value TEXT,"
                            "  fired_at INTEGER DEFAULT 0,"
                            "  snooze_count INTEGER DEFAULT 0,"
                            "  FOREIGN KEY (user_id) REFERENCES users(id)"
                            ");"
                            "CREATE INDEX IF NOT EXISTS idx_sched_status_fire "
                            "  ON scheduled_events(status, fire_at);"
                            "CREATE INDEX IF NOT EXISTS idx_sched_user "
                            "  ON scheduled_events(user_id, status);"
                            "CREATE INDEX IF NOT EXISTS idx_sched_user_name "
                            "  ON scheduled_events(user_id, status, name);"
                            "CREATE INDEX IF NOT EXISTS idx_sched_source "
                            "  ON scheduled_events(source_uuid);";

      rc = sqlite3_exec(s_db.db, v18_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v18 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: added scheduled_events table (v18)");
   }

   /* v19 migration: semantic memory embeddings + entity/relation tables */
   if (current_version >= 1 && current_version < 19) {
      const char *v19_sql =
          /* Add embedding columns to existing memory_facts table */
          "ALTER TABLE memory_facts ADD COLUMN embedding BLOB DEFAULT NULL;"
          "ALTER TABLE memory_facts ADD COLUMN embedding_norm REAL DEFAULT NULL;"

          /* Entity table (populated in Phase S4, created now for schema stability) */
          "CREATE TABLE IF NOT EXISTS memory_entities ("
          "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "  user_id INTEGER NOT NULL,"
          "  name TEXT NOT NULL,"
          "  entity_type TEXT NOT NULL,"
          "  canonical_name TEXT NOT NULL,"
          "  embedding BLOB DEFAULT NULL,"
          "  embedding_norm REAL DEFAULT NULL,"
          "  first_seen INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
          "  last_seen INTEGER,"
          "  mention_count INTEGER DEFAULT 1,"
          "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
          "  UNIQUE(user_id, canonical_name)"
          ");"
          "CREATE INDEX IF NOT EXISTS idx_memory_entities_user "
          "  ON memory_entities(user_id);"

          /* Relation triples (populated in Phase S4, created now) */
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
          "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
          "  FOREIGN KEY (subject_entity_id) REFERENCES memory_entities(id) ON DELETE CASCADE,"
          "  FOREIGN KEY (object_entity_id) REFERENCES memory_entities(id) ON DELETE SET NULL,"
          "  FOREIGN KEY (fact_id) REFERENCES memory_facts(id) ON DELETE SET NULL"
          ");"
          "CREATE INDEX IF NOT EXISTS idx_memory_relations_subject "
          "  ON memory_relations(subject_entity_id);"
          "CREATE INDEX IF NOT EXISTS idx_memory_relations_object "
          "  ON memory_relations(object_entity_id);"
          "CREATE INDEX IF NOT EXISTS idx_memory_relations_user "
          "  ON memory_relations(user_id);";

      rc = sqlite3_exec(s_db.db, v19_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v19 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: added embedding columns and entity/relation tables (v19)");
   }

   /* v20 migration: satellite_mappings table for persistent satellite-to-user mappings */
   if (current_version >= 1 && current_version < 20) {
      const char *v20_sql =
          "CREATE TABLE IF NOT EXISTS satellite_mappings ("
          "  uuid TEXT PRIMARY KEY,"
          "  name TEXT NOT NULL DEFAULT '',"
          "  location TEXT NOT NULL DEFAULT '',"
          "  ha_area TEXT DEFAULT '',"
          "  user_id INTEGER DEFAULT NULL,"
          "  tier INTEGER DEFAULT 1,"
          "  last_seen INTEGER DEFAULT 0,"
          "  created_at INTEGER NOT NULL,"
          "  enabled INTEGER DEFAULT 1,"
          "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE SET NULL"
          ");"
          "CREATE INDEX IF NOT EXISTS idx_satellite_user ON satellite_mappings(user_id);";

      rc = sqlite3_exec(s_db.db, v20_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v20 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: added satellite_mappings table (v20)");
   }

   /* v21 migration: fix satellite_mappings FK (DEFAULT 0 -> DEFAULT NULL, SET NULL) */
   if (current_version >= 20 && current_version < 21) {
      const char *v21_sql =
          "BEGIN TRANSACTION;"
          "CREATE TABLE IF NOT EXISTS satellite_mappings_new ("
          "  uuid TEXT PRIMARY KEY,"
          "  name TEXT NOT NULL DEFAULT '',"
          "  location TEXT NOT NULL DEFAULT '',"
          "  ha_area TEXT DEFAULT '',"
          "  user_id INTEGER DEFAULT NULL,"
          "  tier INTEGER DEFAULT 1,"
          "  last_seen INTEGER DEFAULT 0,"
          "  created_at INTEGER NOT NULL,"
          "  enabled INTEGER DEFAULT 1,"
          "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE SET NULL"
          ");"
          "INSERT INTO satellite_mappings_new SELECT uuid, name, location, ha_area,"
          "  CASE WHEN user_id = 0 THEN NULL ELSE user_id END,"
          "  tier, last_seen, created_at, enabled FROM satellite_mappings;"
          "DROP TABLE satellite_mappings;"
          "ALTER TABLE satellite_mappings_new RENAME TO satellite_mappings;"
          "CREATE INDEX IF NOT EXISTS idx_satellite_user ON satellite_mappings(user_id);"
          "COMMIT;";

      rc = sqlite3_exec(s_db.db, v21_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v21 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: fixed satellite_mappings FK constraints (v21)");
   }

   /* v22 migration: documents and document_chunks tables for RAG search */
   if (current_version >= 1 && current_version < 22) {
      const char *v22_sql =
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
          "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE"
          ");"
          "CREATE TABLE IF NOT EXISTS document_chunks ("
          "  id INTEGER PRIMARY KEY,"
          "  document_id INTEGER NOT NULL,"
          "  chunk_index INTEGER NOT NULL,"
          "  text TEXT NOT NULL,"
          "  embedding BLOB NOT NULL,"
          "  embedding_norm REAL NOT NULL,"
          "  FOREIGN KEY(document_id) REFERENCES documents(id) ON DELETE CASCADE"
          ");"
          "CREATE INDEX IF NOT EXISTS idx_doc_chunks_doc ON document_chunks(document_id);"
          "CREATE INDEX IF NOT EXISTS idx_documents_user ON documents(user_id);"
          "CREATE INDEX IF NOT EXISTS idx_documents_hash ON documents(file_hash);";

      rc = sqlite3_exec(s_db.db, v22_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v22 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: added documents and document_chunks tables (v22)");
   }

   /* v23 migration: calendar tables for CalDAV integration */
   if (current_version >= 1 && current_version < 23) {
      const char *v23_sql =
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
          "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE"
          ");"
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
          "CREATE UNIQUE INDEX IF NOT EXISTS idx_cal_events_uid "
          "  ON calendar_events(calendar_id, uid);"
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
          "CREATE INDEX IF NOT EXISTS idx_cal_acct_user ON calendar_accounts(user_id);";

      rc = sqlite3_exec(s_db.db, v23_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v23 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: added calendar tables (v23)");
   }

   /* v24 migration: add read_only flag to calendar_accounts */
   if (current_version >= 23 && current_version < 24) {
      const char *v24_sql = "ALTER TABLE calendar_accounts ADD COLUMN read_only INTEGER DEFAULT 0;";
      rc = sqlite3_exec(s_db.db, v24_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v24 migration failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: added calendar read_only column (v24)");
   }

   /* v25 migration: OAuth token storage + calendar account OAuth support */
   if (current_version >= 1 && current_version < 25) {
      const char *v25_sql = "CREATE TABLE IF NOT EXISTS oauth_tokens ("
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
                            "CREATE INDEX IF NOT EXISTS idx_oauth_user_provider "
                            "  ON oauth_tokens(user_id, provider);";

      rc = sqlite3_exec(s_db.db, v25_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v25 migration (oauth_tokens) failed: %s",
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }

      /* Add oauth_account_key column to calendar_accounts */
      rc = sqlite3_exec(
          s_db.db, "ALTER TABLE calendar_accounts ADD COLUMN oauth_account_key TEXT DEFAULT ''",
          NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_INFO("auth_db: v25 migration note (oauth_account_key): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      }

      OLOG_INFO("auth_db: added oauth_tokens table and calendar OAuth support (v25)");
   }

   /* v26 migration: contacts table + email_accounts table */
   if (current_version >= 1 && current_version < 26) {
      const char *v26_sql =
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
          "  max_body_chars INTEGER DEFAULT 4000,"
          "  created_at INTEGER NOT NULL,"
          "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE"
          ");"
          "CREATE INDEX IF NOT EXISTS idx_email_acct_user ON email_accounts(user_id);";

      rc = sqlite3_exec(s_db.db, v26_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v26 migration (contacts + email_accounts) failed: %s",
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }

      OLOG_INFO("auth_db: added contacts and email_accounts tables (v26)");
   }

   /* v27 migration: add title_locked column to conversations for auto-title feature */
   if (current_version >= 4 && current_version < 27) {
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE conversations ADD COLUMN title_locked INTEGER DEFAULT 0", NULL,
                        NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_INFO("auth_db: v27 migration note (title_locked): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added title_locked column to conversations (v27)");
      }
   }

   /* v28 migration: add source_client_type to scheduled_events for notification routing */
   if (current_version >= 18 && current_version < 28) {
      rc = sqlite3_exec(
          s_db.db, "ALTER TABLE scheduled_events ADD COLUMN source_client_type INTEGER DEFAULT 0",
          NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_INFO("auth_db: v28 migration note (source_client_type): %s", errmsg ? errmsg : "ok");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added source_client_type to scheduled_events (v28)");
      }
   }

   /* v29 migration: phone call and SMS log tables */
   if (current_version >= 1 && current_version < 29) {
      const char *v29_sql = "CREATE TABLE IF NOT EXISTS phone_call_log ("
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
                            "  read INTEGER DEFAULT 0"
                            ");"
                            "CREATE INDEX IF NOT EXISTS idx_phone_sms_user_ts "
                            "  ON phone_sms_log(user_id, timestamp DESC);"
                            "CREATE INDEX IF NOT EXISTS idx_phone_sms_unread "
                            "  ON phone_sms_log(user_id, read) WHERE read = 0;";

      rc = sqlite3_exec(s_db.db, v29_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v29 migration (phone tables) failed: %s",
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added phone_call_log and phone_sms_log tables (v29)");
      }
   }

   /* v30 migration: image store BLOB → filesystem + phone_sms_log image_id column
    * Export image BLOBs to <data_dir>/images/ files, rebuild table without BLOB column.
    * Also add image_id column to phone_sms_log for MMS attachment references. */
   if (current_version >= 12 && current_version < 30) {
      /* Derive images directory from db_path parent */
      char images_dir[PATH_MAX];
      char db_path_copy[PATH_MAX];
      strncpy(db_path_copy, db_path, sizeof(db_path_copy) - 1);
      db_path_copy[sizeof(db_path_copy) - 1] = '\0';
      char *parent = dirname(db_path_copy);
      snprintf(images_dir, sizeof(images_dir), "%s/images", parent);

      /* Create images directory */
      if (mkdir(images_dir, 0750) != 0 && errno != EEXIST) {
         OLOG_ERROR("auth_db: v30 migration - failed to create %s: %s", images_dir,
                    strerror(errno));
         return AUTH_DB_FAILURE;
      }

      /* Export BLOBs to files */
      sqlite3_stmt *export_stmt = NULL;
      rc = sqlite3_prepare_v2(s_db.db,
                              "SELECT id, mime_type, data FROM images WHERE data IS NOT NULL", -1,
                              &export_stmt, NULL);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v30 migration - prepare export failed: %s", sqlite3_errmsg(s_db.db));
         return AUTH_DB_FAILURE;
      }

      int exported = 0;
      int export_failed = 0;
      while (sqlite3_step(export_stmt) == SQLITE_ROW) {
         const char *id = (const char *)sqlite3_column_text(export_stmt, 0);
         const char *mime = (const char *)sqlite3_column_text(export_stmt, 1);
         const void *blob = sqlite3_column_blob(export_stmt, 2);
         int blob_size = sqlite3_column_bytes(export_stmt, 2);

         if (!id || !blob || blob_size <= 0)
            continue;

         /* Determine file extension from MIME */
         const char *ext = "bin";
         if (mime) {
            if (strcmp(mime, "image/jpeg") == 0)
               ext = "jpg";
            else if (strcmp(mime, "image/png") == 0)
               ext = "png";
            else if (strcmp(mime, "image/gif") == 0)
               ext = "gif";
            else if (strcmp(mime, "image/webp") == 0)
               ext = "webp";
         }

         /* Write to tmp file, fsync, rename for atomicity */
         char filepath[PATH_MAX + 32];
         char tmppath[PATH_MAX + 32];
         snprintf(filepath, sizeof(filepath), "%s/%s.%s", images_dir, id, ext);
         snprintf(tmppath, sizeof(tmppath), "%s/.%s.%s.tmp", images_dir, id, ext);

         int fd = open(tmppath, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0640);
         if (fd < 0) {
            OLOG_WARNING("auth_db: v30 migration - failed to create %s: %s", tmppath,
                         strerror(errno));
            export_failed++;
            continue;
         }

         const unsigned char *wp = (const unsigned char *)blob;
         size_t remaining = (size_t)blob_size;
         bool write_ok = true;
         while (remaining > 0) {
            ssize_t written = write(fd, wp, remaining);
            if (written < 0) {
               if (errno == EINTR)
                  continue;
               OLOG_WARNING("auth_db: v30 migration - write failed for %s: %s", id,
                            strerror(errno));
               write_ok = false;
               break;
            }
            wp += written;
            remaining -= (size_t)written;
         }
         if (!write_ok) {
            close(fd);
            unlink(tmppath);
            export_failed++;
            continue;
         }

         fsync(fd);
         close(fd);

         if (rename(tmppath, filepath) != 0) {
            OLOG_WARNING("auth_db: v30 migration - rename failed for %s: %s", id, strerror(errno));
            unlink(tmppath);
            export_failed++;
            continue;
         }

         exported++;
      }
      sqlite3_finalize(export_stmt);

      if (export_failed > 0) {
         OLOG_ERROR("auth_db: v30 migration - %d/%d images failed to export", export_failed,
                    exported + export_failed);
         return AUTH_DB_FAILURE;
      }

      /* Rebuild images table without BLOB column (transactional) */
      const char *v30_images_sql =
          "BEGIN TRANSACTION;"
          "DROP TABLE IF EXISTS images_new;"
          "CREATE TABLE images_new ("
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
          "INSERT INTO images_new (id, user_id, source, retention_policy, mime_type, size, "
          "filename, created_at, last_accessed) "
          "SELECT id, user_id, 0, 0, mime_type, size, "
          "id || '.' || CASE mime_type "
          "  WHEN 'image/jpeg' THEN 'jpg' "
          "  WHEN 'image/png' THEN 'png' "
          "  WHEN 'image/gif' THEN 'gif' "
          "  WHEN 'image/webp' THEN 'webp' "
          "  ELSE 'bin' END, "
          "created_at, last_accessed FROM images;"
          "DROP TABLE images;"
          "ALTER TABLE images_new RENAME TO images;"
          "CREATE INDEX IF NOT EXISTS idx_images_user ON images(user_id);"
          "CREATE INDEX IF NOT EXISTS idx_images_created ON images(created_at);"
          "CREATE INDEX IF NOT EXISTS idx_images_retention ON images(retention_policy);"
          "COMMIT;";

      rc = sqlite3_exec(s_db.db, v30_images_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v30 migration (images table rebuild) failed: %s",
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         sqlite3_exec(s_db.db, "ROLLBACK;", NULL, NULL, NULL);
         return AUTH_DB_FAILURE;
      }

      OLOG_INFO("auth_db: migrated %d images from BLOB to filesystem (v30)", exported);
   }

   /* v30 migration: add image_id to phone_sms_log (for MMS attachments) */
   if (current_version >= 29 && current_version < 30) {
      rc = sqlite3_exec(s_db.db, "ALTER TABLE phone_sms_log ADD COLUMN image_id TEXT DEFAULT NULL",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_WARNING("auth_db: v30 migration (sms image_id) failed: %s",
                      errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added image_id column to phone_sms_log (v30)");
      }
   }

   /* v31 migration: add photo_id to memory_entities for contact photos */
   if (current_version >= 19 && current_version < 31) {
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE memory_entities ADD COLUMN photo_id TEXT DEFAULT NULL", NULL,
                        NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_WARNING("auth_db: v31 migration (entity photo_id) failed: %s",
                      errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added photo_id column to memory_entities (v31)");
      }
   }

   /* v32 migration: missed_notifications table for offline-user notification queue */
   if (current_version >= 1 && current_version < 32) {
      const char *v32_sql = "CREATE TABLE IF NOT EXISTS missed_notifications ("
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
                            "  ON missed_notifications(user_id, created_at);";
      rc = sqlite3_exec(s_db.db, v32_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v32 migration (missed_notifications) failed: %s",
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: added missed_notifications table (v32)");
   }

   /* v33 migration: temporal validity columns on memory_relations.
    * NULL = open-ended (no bound).  "currently true" predicate:
    *   valid_to IS NULL OR valid_to > strftime('%s','now')
    * Future relation-decay implementations should skip rows where valid_to is set
    * and in the past — those are historical facts, not stale beliefs. */
   if (current_version >= 19 && current_version < 33) {
      const char *v33_sql = "ALTER TABLE memory_relations ADD COLUMN valid_from INTEGER "
                            "  DEFAULT NULL;"
                            "ALTER TABLE memory_relations ADD COLUMN valid_to INTEGER "
                            "  DEFAULT NULL;"
                            "CREATE INDEX IF NOT EXISTS idx_memory_relations_user_validity "
                            "  ON memory_relations(user_id, valid_to);"
                            "CREATE INDEX IF NOT EXISTS idx_memory_relations_subject_open "
                            "  ON memory_relations(subject_entity_id, relation) "
                            "  WHERE valid_to IS NULL;";
      rc = sqlite3_exec(s_db.db, v33_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         /* Column may already exist if a previous migration partially ran — log and continue. */
         OLOG_WARNING("auth_db: v33 migration (memory_relations validity) returned: %s",
                      errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added valid_from/valid_to to memory_relations (v33)");
      }
   }

   /* v34 migration: fact category column + per-user backfill gate.
    * categories_backfilled_at = 0 means embedding-centroid classification has not yet run
    * for that user; memory_embeddings_start_backfill() picks it up on next session. */
   if (current_version >= 1 && current_version < 34) {
      const char *v34_sql = "ALTER TABLE memory_facts ADD COLUMN category TEXT NOT NULL "
                            "  DEFAULT 'general';"
                            "ALTER TABLE users ADD COLUMN categories_backfilled_at INTEGER "
                            "  DEFAULT 0;"
                            "CREATE INDEX IF NOT EXISTS idx_memory_facts_user_category "
                            "  ON memory_facts(user_id, category);";
      rc = sqlite3_exec(s_db.db, v34_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_WARNING("auth_db: v34 migration (fact category) returned: %s",
                      errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added category column + backfill gate (v34)");
      }
   }

   /* v35 migration: per-chunk created_at for temporal-query scoring.  0 = unknown
    * (chunk gets no proximity boost).  Backfill from documents.created_at would
    * be a follow-up — for v1, only chunks ingested after this migration get a
    * timestamp; older chunks default to 0 and behave as before. */
   if (current_version >= 22 && current_version < 35) {
      const char *v35_sql = "ALTER TABLE document_chunks ADD COLUMN created_at INTEGER "
                            "  NOT NULL DEFAULT 0;";
      rc = sqlite3_exec(s_db.db, v35_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_WARNING("auth_db: v35 migration (chunk created_at) returned: %s",
                      errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added created_at to document_chunks (v35)");
      }
   }

   /* v36 migration: per-conversation reasoning_effort lock.  Without this column
    * the locked-settings restore on page refresh forgets the user's chosen
    * effort and the dropdown snaps back to the global default ("low").
    *
    * No lower bound on current_version: a DB at v10 will run the v11 block
    * above (which adds the conversations LLM-lock columns) AND this v36 block
    * in the same startup. `current_version` is captured once and not bumped
    * between migration blocks, so a `>= 11` guard here would incorrectly skip
    * the column add on v10-or-earlier DBs. ALTER TABLE errors (e.g. if the
    * column already exists on a concurrent path) are logged and swallowed, so
    * the migration is idempotent. */
   if (current_version < 36) {
      const char *v36_sql =
          "ALTER TABLE conversations ADD COLUMN reasoning_effort TEXT DEFAULT NULL;";
      rc = sqlite3_exec(s_db.db, v36_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_WARNING("auth_db: v36 migration (reasoning_effort) returned: %s",
                      errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: added reasoning_effort to conversations (v36)");
      }
   }

   /* v37 migration: backfill document_chunks.created_at from parent document.
    * Legacy chunks (ingested before v35 added created_at) have created_at = 0,
    * which forfeits temporal-query scoring.  Inherit the parent document's
    * created_at as a reasonable proxy.  Idempotent (WHERE created_at = 0).
    * Lower bound >= 35: the created_at column only exists from v35 onward. */
   if (current_version >= 35 && current_version < 37) {
      const char *v37_sql = "UPDATE document_chunks SET created_at = "
                            "(SELECT d.created_at FROM documents d "
                            "WHERE d.id = document_chunks.document_id) "
                            "WHERE created_at = 0;";
      rc = sqlite3_exec(s_db.db, v37_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_WARNING("auth_db: v37 migration (chunk created_at backfill): %s",
                      errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         int affected = sqlite3_changes(s_db.db);
         OLOG_INFO("auth_db: backfilled created_at on %d document chunks (v37)", affected);
      }
   }

   /* v38 migration: summary_nodes table for LCM Phase 4 (hierarchical summaries).
    * Each compaction creates a node linking to its predecessor, enabling multi-resolution
    * drill-down via the context_expand tool. */
   if (current_version < 38) {
      const char *v38_sql =
          "CREATE TABLE IF NOT EXISTS summary_nodes ("
          "id INTEGER PRIMARY KEY AUTOINCREMENT, "
          "conversation_id INTEGER NOT NULL, "
          "prior_node_id INTEGER, "
          "depth INTEGER NOT NULL DEFAULT 0, "
          "msg_id_start INTEGER NOT NULL, "
          "msg_id_end INTEGER NOT NULL, "
          "level INTEGER NOT NULL DEFAULT 0, "
          "summary_text TEXT NOT NULL, "
          "token_count INTEGER, "
          "created_at INTEGER, "
          "FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE, "
          "FOREIGN KEY (prior_node_id) REFERENCES summary_nodes(id) ON DELETE SET NULL)";
      rc = sqlite3_exec(s_db.db, v38_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v38 migration (summary_nodes) failed: %s",
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: created summary_nodes table (v38)");
   }

   /* v39 migration: extraction recovery tracking on conversations.
    * extraction_attempts: incremented before each recovery trigger, reset on success.
    *   Capped by config max_attempts to prevent poison-pill loops.
    * extraction_last_attempt_at: unix timestamp of last recovery attempt (0 = never).
    * The `>= 1` guard mirrors v3-v17 — fresh installs (current_version == 0) get
    * the columns from SCHEMA_SQL and don't need the ALTER. */
   if (current_version >= 1 && current_version < 39) {
      const char *v39_attempts =
          "ALTER TABLE conversations ADD COLUMN extraction_attempts INTEGER DEFAULT 0";
      rc = sqlite3_exec(s_db.db, v39_attempts, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK && !(errmsg && strstr(errmsg, "duplicate column"))) {
         OLOG_WARNING("auth_db: v39 migration (extraction_attempts) returned: %s",
                      errmsg ? errmsg : "unknown");
      }
      sqlite3_free(errmsg);
      errmsg = NULL;

      const char *v39_last =
          "ALTER TABLE conversations ADD COLUMN extraction_last_attempt_at INTEGER DEFAULT 0";
      rc = sqlite3_exec(s_db.db, v39_last, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK && !(errmsg && strstr(errmsg, "duplicate column"))) {
         OLOG_WARNING("auth_db: v39 migration (extraction_last_attempt_at) returned: %s",
                      errmsg ? errmsg : "unknown");
      }
      sqlite3_free(errmsg);
      errmsg = NULL;

      OLOG_INFO("auth_db: added extraction recovery columns (v39)");
   }

   /* v40 migration: memory provenance + ID-based extraction cursor.
    * source_conversation_id / source_msg_id_start / source_msg_id_end link
    * each extracted item back to the message range that produced it.
    * last_extracted_msg_id is the role-agnostic extraction cursor (replaces
    * the count-based cursor on fresh paths going forward; count retained for
    * back-compat one release cycle).
    * Note: migrated DBs omit the FK clause — SQLite cannot add FKs retroactively.
    * Fresh installs get the FK via SCHEMA_SQL. */
   if (current_version >= 1 && current_version < 40) {
      /* Wrap in a transaction so a crash mid-migration leaves the schema clean.
       * Each ALTER is idempotent via duplicate-column check on re-run. */
      rc = sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_WARNING("auth_db: v40 migration BEGIN failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         static const char *const v40_alters[] = {
            "ALTER TABLE memory_facts       ADD COLUMN source_conversation_id INTEGER DEFAULT NULL",
            "ALTER TABLE memory_facts       ADD COLUMN source_msg_id_start    INTEGER DEFAULT NULL",
            "ALTER TABLE memory_facts       ADD COLUMN source_msg_id_end      INTEGER DEFAULT NULL",
            "ALTER TABLE memory_relations   ADD COLUMN source_conversation_id INTEGER DEFAULT NULL",
            "ALTER TABLE memory_relations   ADD COLUMN source_msg_id_start    INTEGER DEFAULT NULL",
            "ALTER TABLE memory_relations   ADD COLUMN source_msg_id_end      INTEGER DEFAULT NULL",
            "ALTER TABLE memory_summaries   ADD COLUMN source_conversation_id INTEGER DEFAULT NULL",
            "ALTER TABLE memory_summaries   ADD COLUMN source_msg_id_start    INTEGER DEFAULT NULL",
            "ALTER TABLE memory_summaries   ADD COLUMN source_msg_id_end      INTEGER DEFAULT NULL",
            "ALTER TABLE memory_preferences ADD COLUMN source_conversation_id INTEGER DEFAULT NULL",
            "ALTER TABLE memory_preferences ADD COLUMN source_msg_id_start    INTEGER DEFAULT NULL",
            "ALTER TABLE memory_preferences ADD COLUMN source_msg_id_end      INTEGER DEFAULT NULL",
            "ALTER TABLE conversations      ADD COLUMN last_extracted_msg_id  INTEGER NOT NULL "
            "DEFAULT 0",
            NULL,
         };
         for (int ai = 0; v40_alters[ai]; ai++) {
            char *v40_err = NULL;
            int v40_rc = sqlite3_exec(s_db.db, v40_alters[ai], NULL, NULL, &v40_err);
            if (v40_rc != SQLITE_OK && !(v40_err && strstr(v40_err, "duplicate column"))) {
               OLOG_WARNING("auth_db: v40 migration ALTER failed: %s",
                            v40_err ? v40_err : "unknown");
            }
            sqlite3_free(v40_err);
         }
         rc = sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, &errmsg);
         if (rc != SQLITE_OK) {
            OLOG_WARNING("auth_db: v40 migration COMMIT failed: %s", errmsg ? errmsg : "unknown");
            sqlite3_free(errmsg);
            errmsg = NULL;
         }
         OLOG_INFO("auth_db: added memory provenance columns + last_extracted_msg_id (v40)");
      }
   }

   /* v41 migration: system_metadata table + users.embeddings_model_id.
    * system_metadata is a generic key/value store for daemon-level state.
    * embeddings_model_id mirrors the categories_backfilled_at pattern — NULL
    * means the user's embeddings pre-date the current model and need recomputing. */
   if (current_version >= 1 && current_version < 41) {
      rc = sqlite3_exec(s_db.db,
                        "BEGIN IMMEDIATE;"
                        "CREATE TABLE IF NOT EXISTS system_metadata ("
                        "   key   TEXT PRIMARY KEY,"
                        "   value TEXT NOT NULL"
                        ");"
                        "COMMIT;",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_WARNING("auth_db: v41 migration (system_metadata) returned: %s",
                      errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
         sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      } else {
         OLOG_INFO("auth_db: created system_metadata table (v41)");
      }

      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE users ADD COLUMN embeddings_model_id TEXT DEFAULT NULL", NULL,
                        NULL, &errmsg);
      if (rc != SQLITE_OK && !(errmsg && strstr(errmsg, "duplicate column"))) {
         OLOG_WARNING("auth_db: v41 migration (embeddings_model_id) returned: %s",
                      errmsg ? errmsg : "unknown");
      }
      sqlite3_free(errmsg);
      errmsg = NULL;
      OLOG_INFO("auth_db: added embeddings_model_id to users (v41)");
   }

   /* v42 migration: conversations.anchor_date for cat-2 temporal extraction.
    * Holds the conversation's logical anchor timestamp (epoch seconds).  Production
    * conv_db_create_* writers populate at insert with time(NULL); the bench harness
    * passes session_X_date_time so LoCoMo's synthetic anchors flow through.  Read by
    * memory_extraction.c when building the prompt so the LLM can resolve relative
    * temporal phrases ("yesterday", "last month", "five years ago") against it.
    *
    * Default 0 (= ANCHOR_DATE_NONE) for legacy rows; the extraction prompt omits
    * the anchor line when this is 0.  The literal-constant default is required for
    * SQLite's O(1) metadata-only ALTER path — switching to strftime() or
    * CURRENT_TIMESTAMP would silently regress this to a full-table rewrite under
    * the auth_db lock at startup.  See atlas/dawn/memory/CAT2_TEMPORAL.md L1+L5. */
   if (current_version >= 1 && current_version < 42) {
      rc = sqlite3_exec(
          s_db.db, "ALTER TABLE conversations ADD COLUMN anchor_date INTEGER NOT NULL DEFAULT 0",
          NULL, NULL, &errmsg);
      if (rc != SQLITE_OK && !(errmsg && strstr(errmsg, "duplicate column"))) {
         OLOG_WARNING("auth_db: v42 migration (anchor_date) returned: %s",
                      errmsg ? errmsg : "unknown");
      } else {
         OLOG_INFO("auth_db: added anchor_date to conversations (v42)");
      }
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   /* v43 migration: entity-merge / user-identity-dedup workstream.
    *   memory_entities.canonical_id  — soft alias self-FK (NULL = self is canonical).
    *                                   ON DELETE SET NULL so dropping a canonical
    *                                   demotes its aliases to canonical rather than
    *                                   cascade-deleting them.
    *   memory_entities.is_user_self  — exactly-one-per-user flag, enforced by the
    *                                   partial UNIQUE index in the post-migration block.
    *   memory_entity_aliases         — append-only audit log for soft/hard merges.
    *   memory_entity_merge_proposals — review band staging for the Phase 2 auto-merge
    *                                   gate.
    * Both ALTER ADD COLUMNs use literal-constant defaults (NULL and 0) so SQLite
    * takes the O(1) metadata-only path — no full-table rewrite under the auth_db
    * lock at startup.  See docs/ENTITY_MERGE_DESIGN.md §3. */
   if (current_version >= 1 && current_version < 43) {
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE memory_entities ADD COLUMN canonical_id INTEGER DEFAULT NULL "
                        "REFERENCES memory_entities(id) ON DELETE SET NULL",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK && !(errmsg && strstr(errmsg, "duplicate column"))) {
         OLOG_WARNING("auth_db: v43 migration (canonical_id) returned: %s",
                      errmsg ? errmsg : "unknown");
      } else {
         OLOG_INFO("auth_db: added canonical_id to memory_entities (v43)");
      }
      sqlite3_free(errmsg);
      errmsg = NULL;

      rc = sqlite3_exec(
          s_db.db, "ALTER TABLE memory_entities ADD COLUMN is_user_self INTEGER NOT NULL DEFAULT 0",
          NULL, NULL, &errmsg);
      if (rc != SQLITE_OK && !(errmsg && strstr(errmsg, "duplicate column"))) {
         OLOG_WARNING("auth_db: v43 migration (is_user_self) returned: %s",
                      errmsg ? errmsg : "unknown");
      } else {
         OLOG_INFO("auth_db: added is_user_self to memory_entities (v43)");
      }
      sqlite3_free(errmsg);
      errmsg = NULL;

      const char *v43_tables_sql =
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
          "  FOREIGN KEY (user_id)          REFERENCES users(id)           ON DELETE CASCADE,"
          "  FOREIGN KEY (source_entity_id) REFERENCES memory_entities(id) ON DELETE SET NULL,"
          "  FOREIGN KEY (target_entity_id) REFERENCES memory_entities(id) ON DELETE SET NULL"
          ");"
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
          "  FOREIGN KEY (user_id)          REFERENCES users(id)           ON DELETE CASCADE,"
          "  FOREIGN KEY (source_entity_id) REFERENCES memory_entities(id) ON DELETE CASCADE,"
          "  FOREIGN KEY (target_entity_id) REFERENCES memory_entities(id) ON DELETE CASCADE"
          ");";

      rc = sqlite3_exec(s_db.db, v43_tables_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v43 migration (alias tables) failed: %s",
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
      OLOG_INFO("auth_db: created memory_entity_aliases + memory_entity_merge_proposals (v43)");
   }

   /* v44 migration: user-identity fields on the users table.
    *   real_name         — required by the link-user-self synthetic-seed path
    *                       (gate enforced in memory_alias_link_user_self_run).
    *                       Surfaced in WebUI Settings → User → Real name.
    *   preferred_address — optional; injected into the LLM system prompt as
    *                       "They prefer to be addressed as ..." for personas.
    *   identity_aliases  — optional newline-separated list of alternate names
    *                       (nicknames, formal names, email handles).  Parsed
    *                       at use-site (split on \n, strip whitespace, drop
    *                       empties, dedupe case-insensitive).  Feeds both the
    *                       system prompt and the synthetic-self resolver token
    *                       set in Phase 1.5 Ckpt B.
    * All three are nullable TEXT with DEFAULT NULL — literal-constant default
    * → SQLite's O(1) metadata-only ALTER TABLE path so startup doesn't take a
    * full-table rewrite under the auth_db lock. */
   if (current_version >= 1 && current_version < 44) {
      rc = sqlite3_exec(s_db.db, "ALTER TABLE users ADD COLUMN real_name TEXT DEFAULT NULL", NULL,
                        NULL, &errmsg);
      if (rc != SQLITE_OK && !(errmsg && strstr(errmsg, "duplicate column"))) {
         OLOG_WARNING("auth_db: v44 migration (real_name) returned: %s",
                      errmsg ? errmsg : "unknown");
      } else {
         OLOG_INFO("auth_db: added real_name to users (v44)");
      }
      sqlite3_free(errmsg);
      errmsg = NULL;

      rc = sqlite3_exec(s_db.db, "ALTER TABLE users ADD COLUMN preferred_address TEXT DEFAULT NULL",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK && !(errmsg && strstr(errmsg, "duplicate column"))) {
         OLOG_WARNING("auth_db: v44 migration (preferred_address) returned: %s",
                      errmsg ? errmsg : "unknown");
      } else {
         OLOG_INFO("auth_db: added preferred_address to users (v44)");
      }
      sqlite3_free(errmsg);
      errmsg = NULL;

      rc = sqlite3_exec(s_db.db, "ALTER TABLE users ADD COLUMN identity_aliases TEXT DEFAULT NULL",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK && !(errmsg && strstr(errmsg, "duplicate column"))) {
         OLOG_WARNING("auth_db: v44 migration (identity_aliases) returned: %s",
                      errmsg ? errmsg : "unknown");
      } else {
         OLOG_INFO("auth_db: added identity_aliases to users (v44)");
      }
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   /* v45: semantic search on memory_summaries.  Adds an `embedding BLOB`
    * column on memory_summaries (NULL default — existing rows stay
    * unembedded; the recompute worker backfills on next boot the same way
    * it handles facts and entities after a model swap).
    *
    * Literal-constant default → SQLite's O(1) ALTER fast path, no row
    * rewrite. */
   if (current_version >= 1 && current_version < 45) {
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE memory_summaries ADD COLUMN embedding BLOB DEFAULT NULL", NULL,
                        NULL, &errmsg);
      if (rc != SQLITE_OK && !(errmsg && strstr(errmsg, "duplicate column"))) {
         OLOG_WARNING("auth_db: v45 migration (memory_summaries.embedding) returned: %s",
                      errmsg ? errmsg : "unknown");
      } else {
         OLOG_INFO("auth_db: added embedding column to memory_summaries (v45)");
      }
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   /* v46: force a recompute pass for every user so the v45-added summary
    * embedding column gets backfilled.  The recompute worker's model_id
    * gate only fires when the embedding *model* changes — adding a new
    * embedding column doesn't change that, so without this nudge the
    * historical summaries stay unembedded indefinitely and the semantic
    * summary adapter only sees rows created after v45 ships.
    *
    * Side effect: every user's facts + entities also get re-embedded.
    * Wasted work, but bounded (the dev's ~2k facts + 300 entities + 270
    * summaries take ~10 seconds total against the local ONNX engine).
    * Justified by avoiding the per-pass-metadata refactor that the
    * recompute "all-three-or-redo-all" trade-off comment in
    * src/memory/memory_embed_recompute.c references. */
   if (current_version >= 1 && current_version < 46) {
      rc = sqlite3_exec(s_db.db, "UPDATE users SET embeddings_model_id = NULL", NULL, NULL,
                        &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_WARNING("auth_db: v46 migration (reset embeddings_model_id) returned: %s",
                      errmsg ? errmsg : "unknown");
      } else {
         OLOG_INFO("auth_db: reset users.embeddings_model_id to trigger v45 summary "
                   "embedding backfill (v46)");
      }
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   /* v47 migration: add memory_facts.subject_entity_id as a hard FK from each
    * fact to the entity it's about.  Backfill from existing memory_relations
    * rows where fact_id is set — that captures the ~17% of facts that already
    * have a linked relation.  The remaining ~83% stay NULL until a re-extract
    * under the new Phase 0 prompt populates them at insert time.  A follow-up
    * migration tightens NOT NULL once backfill + re-extract complete.
    *
    * Phase 0 design: facts MUST carry an entity FK so graph traversal can
    * go fact → entity directly without the relations table hop, AND so the
    * Phase 2 entity-merge resolver's cascading effects automatically apply
    * to facts the same way they apply to relations.  See
    * docs/PHASE_0_EXTRACTION_PROMPT_DRAFT.md §"C-side changes required" #3.
    *
    * NULLABLE during the migration window — re-extract uses the new prompt
    * which fills subject_entity_id at insert time via the existing entity
    * resolver, so the column populates organically as users use the system.
    *
    * Literal-NULL default → SQLite O(1) ALTER fast path. */
   if (current_version >= 1 && current_version < 47) {
      rc = sqlite3_exec(
          s_db.db, "ALTER TABLE memory_facts ADD COLUMN subject_entity_id INTEGER DEFAULT NULL",
          NULL, NULL, &errmsg);
      if (rc != SQLITE_OK && !(errmsg && strstr(errmsg, "duplicate column"))) {
         OLOG_WARNING("auth_db: v47 migration (memory_facts.subject_entity_id) returned: %s",
                      errmsg ? errmsg : "unknown");
      } else {
         OLOG_INFO("auth_db: added subject_entity_id column to memory_facts (v47)");
      }
      sqlite3_free(errmsg);
      errmsg = NULL;

      /* Backfill from memory_relations.  For each fact that has at least one
       * linked relation, copy the subject_entity_id of that relation onto the
       * fact.  When a fact has multiple linked relations (typical), MIN() is
       * a stable deterministic pick — the subject of the lowest-id linked
       * relation, which is usually the primary relation emitted first by the
       * extraction LLM.  Facts with no linked relations stay NULL and rely
       * on re-extraction to populate. */
      rc = sqlite3_exec(s_db.db,
                        "UPDATE memory_facts "
                        "   SET subject_entity_id = ("
                        "     SELECT MIN(r.subject_entity_id) "
                        "       FROM memory_relations r "
                        "      WHERE r.fact_id = memory_facts.id "
                        "        AND r.subject_entity_id IS NOT NULL"
                        "   ) "
                        " WHERE subject_entity_id IS NULL",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_WARNING("auth_db: v47 migration (backfill subject_entity_id) returned: %s",
                      errmsg ? errmsg : "unknown");
      } else {
         OLOG_INFO(
             "auth_db: backfilled memory_facts.subject_entity_id from linked relations (v47)");
      }
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   /* v48 migration: BM25 keyword index via FTS5 virtual table.
    * docs/MEM0_ARCHITECTURAL_PARITY.md Phase 1.  Algorithm + sigmoid
    * normalization adapted from mem0ai/mem0 (Apache-2.0).  See NOTICE.
    *
    * Tracks v48_ok across both CREATE and backfill — only allows the
    * schema_version bump below if BOTH succeed.  Without this, a
    * transient CREATE failure would leave the DB advertised as v48 with
    * no FTS5 table; subsequent boots would skip the migration block,
    * prepare_statements would fail on stmt_memory_fact_search_bm25 prep,
    * and the daemon would refuse to start with no operator-visible
    * recovery path. */
   /* v48 is OK without running the migration block in two cases:
    * (a) DB is already at v48 or newer (nothing to do).
    * (b) Fresh install (current_version == 0) — SCHEMA_SQL creates the
    *     memory_facts_fts virtual table directly, so the migration block
    *     (which only fires on >= 1 && < 48) is correctly skipped and
    *     the schema_version bump still needs to proceed. */
   bool v48_ok = (current_version >= 48) || (current_version == 0);
   if (current_version >= 1 && current_version < 48) {
      rc = sqlite3_exec(s_db.db,
                        "CREATE VIRTUAL TABLE IF NOT EXISTS memory_facts_fts USING fts5("
                        "   fact_stems,"
                        "   tokenize='unicode61 remove_diacritics 2',"
                        "   content=''"
                        ")",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v48 migration (memory_facts_fts CREATE) failed: %s — "
                    "leaving schema_version at %d so next boot retries",
                    errmsg ? errmsg : "unknown", current_version);
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: created memory_facts_fts virtual table (v48)");

         /* Backfill from existing memory_facts.  Stemming runs in C —
          * SQL triggers can't call libstemmer.  Wrap the inserts in a
          * single transaction so the backfill is atomic across thousands
          * of rows. */
         (void)memory_stem_init();
         int backfill_count = 0;
         int backfill_errors = 0;
         sqlite3_stmt *select_stmt = NULL;
         sqlite3_stmt *insert_stmt = NULL;
         int prep_rc = sqlite3_prepare_v2(s_db.db, "SELECT id, fact_text FROM memory_facts", -1,
                                          &select_stmt, NULL);
         if (prep_rc == SQLITE_OK) {
            prep_rc = sqlite3_prepare_v2(
                s_db.db, "INSERT INTO memory_facts_fts(rowid, fact_stems) VALUES (?, ?)", -1,
                &insert_stmt, NULL);
         }
         int commit_rc = SQLITE_DONE;
         if (prep_rc == SQLITE_OK) {
            (void)sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
            while (sqlite3_step(select_stmt) == SQLITE_ROW) {
               int64_t fid = sqlite3_column_int64(select_stmt, 0);
               const unsigned char *txt = sqlite3_column_text(select_stmt, 1);
               char stems[MEMORY_FACT_STEMS_MAX];
               int n_stems = memory_stem_string((const char *)txt, stems, sizeof(stems));
               sqlite3_reset(insert_stmt);
               sqlite3_bind_int64(insert_stmt, 1, fid);
               sqlite3_bind_text(insert_stmt, 2, stems, -1, SQLITE_TRANSIENT);
               if (sqlite3_step(insert_stmt) != SQLITE_DONE) {
                  backfill_errors++;
               } else if (n_stems > 0) {
                  backfill_count++;
               }
            }
            commit_rc = sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL);
            if (commit_rc != SQLITE_OK) {
               /* COMMIT failed (e.g. SQLITE_BUSY / IO) — roll back so the
                * connection returns to autocommit instead of leaving an open
                * transaction that would hold locks and taint later startup SQL.
                * schema_version stays unbumped below, so the next boot retries. */
               (void)sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
            }
         }
         if (select_stmt)
            sqlite3_finalize(select_stmt);
         if (insert_stmt)
            sqlite3_finalize(insert_stmt);
         /* Promote log level when partial-failure occurred so operators
          * see it. */
         if (backfill_errors > 0) {
            OLOG_WARNING("auth_db: v48 BM25 backfill PARTIAL: %d indexed, %d failed — "
                         "some facts will not surface via BM25 keyword search until next "
                         "fact_create writes a new memory_facts_fts row or a manual rebuild",
                         backfill_count, backfill_errors);
         } else {
            OLOG_INFO("auth_db: v48 BM25 backfill complete: %d facts indexed", backfill_count);
         }
         /* Mark v48 successful only when prep, commit, and statement
          * preparation all came through cleanly.  Partial backfill (some
          * errors but commit succeeded) still advances the version —
          * those rows just won't surface until a manual rebuild; not
          * worth blocking the entire migration. */
         if (prep_rc == SQLITE_OK && commit_rc == SQLITE_OK) {
            v48_ok = true;
         } else {
            OLOG_ERROR("auth_db: v48 migration COMMIT or statement prep failed — "
                       "leaving schema_version at %d so next boot retries",
                       current_version);
         }
      }
   }

   /* v49 migration: deduplicate memory_relations + enforce partial UNIQUE on
    * open edges.  Background: extraction's production write path
    * (memory_db_relation_supersede Phase 3) was a plain INSERT, so the same
    * (subject, predicate, object) edge accreted a new row on every re-witness.
    * Live measurement on the dev's DB: 1335 rows in the Jon equivalence class,
    * 1008 distinct tuples — 24% duplication, top offender (Jon, working_on, DAWN)
    * × 49.  Facts dedup via paraphrase-merge, entities via UNIQUE+upsert;
    * relations were the outlier.  This migration closes the gap.
    *
    * Strategy: ALTER outside transaction (SQLite ALTER-in-transaction limitation
    * prior to 3.35), then in one BEGIN IMMEDIATE —
    *   Step 1 — stamp winners: UPDATE the MIN(id) row of each open
    *       duplicate group with mention_count = group size + confidence = MAX.
    *   Step 2 — refresh winner provenance: row-valued UPDATE pulling
    *       source_* / fact_id from the row with MAX(id) inside each group.
    *       One SELECT subquery so all four columns come from the SAME source
    *       row (architecture-review H1 fix).
    *   Step 3 — DELETE non-winner duplicates via correlated EXISTS
    *       (w.id < d.id).  EXISTS avoids the temp B-tree GROUP BY that
    *       NOT IN (SELECT MIN(id) GROUP BY ...) would materialize; both forms
    *       are mechanically equivalent, EXISTS scales linearly at 50k+ rows
    *       (embedded-efficiency-reviewer MED-1 fix).
    *   Step 4 — CREATE UNIQUE INDEX idx_memory_relations_unique_open
    *       partial-scoped to valid_to IS NULL.  Future inserts upsert against
    *       it via the stmt_memory_relation_create ON CONFLICT clause.
    *
    * Scope: WHERE valid_to IS NULL only.  Closed historical rows may
    * legitimately repeat (married_to(A) → divorced → married_to(B) →
    * divorced → married_to(A) is a real lifecycle).  Matches the v33
    * idx_memory_relations_subject_open partial-index precedent.
    *
    * Ordering invariant: Steps 1/2/3/4 MUST stay in the same transaction.
    * If an extraction worker fires between Step 3 (DELETE losers) and Step 4
    * (CREATE UNIQUE INDEX), it could re-insert a duplicate before the index
    * can reject it.  auth_db_init runs migrations BEFORE prepare_statements
    * (and before any worker thread is spawned), so this is already safe,
    * but the comment is load-bearing for any future refactor that splits
    * migration phases.
    *
    * Index-dependency invariant: Steps 1, 2, and 3 all rely on the v33
    * idx_memory_relations_subject_open partial index for performance.
    * EXPLAIN QUERY PLAN against the schema picks it for the outer scan AND
    * every correlated subquery.  Do NOT drop the v33 index inside this
    * migration block — the drop is a separate version-bump follow-up once
    * v49's unique-open index has soaked.
    *
    * Failure-mode cascade: if v49_sql fails and ROLLBACK fires, duplicates
    * remain in the table.  v49_ok stays false → schema_version stays at 48.
    * The fresh-install index pass below ALSO fails to create
    * idx_memory_relations_unique_open (duplicates still violate the UNIQUE),
    * logging OLOG_WARNING.  stmt_memory_relation_create prepare then fails
    * (ON CONFLICT requires the index), so auth_db_prepare_statements returns
    * AUTH_DB_FAILURE and the daemon aborts startup.  This is intentional —
    * the daemon cannot run v49 logic without a deduplicated table.  Fix the
    * underlying error (look earlier in the log for the OLOG_ERROR), restart,
    * and the migration re-runs cleanly on next boot.
    *
    * SQLite version requirements: ON CONFLICT(cols) WHERE expr (upsert
    * against partial UNIQUE) needs >= 3.24; the row-valued UPDATE in Step 2
    * needs >= 3.15.  Jetson and CI Docker base ship 3.37+, well above. */
   bool v49_ok = (current_version >= 49) || (current_version == 0);
   if (current_version >= 1 && current_version < 49) {
      /* ALTER outside transaction (SQLite limitation prior to 3.35; safe at
       * any version since DEFAULT is a literal constant → O(1) fast path).
       * Tolerate "duplicate column" if a previous boot ran the ALTER but
       * crashed before the transaction below committed. */
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE memory_relations "
                        "ADD COLUMN mention_count INTEGER NOT NULL DEFAULT 1",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         bool benign = (errmsg && strstr(errmsg, "duplicate column"));
         if (!benign) {
            OLOG_ERROR("auth_db: v49 ALTER (mention_count column) failed: %s",
                       errmsg ? errmsg : "unknown");
            sqlite3_free(errmsg);
            return AUTH_DB_FAILURE;
         }
         sqlite3_free(errmsg);
         errmsg = NULL;
      }

      rc = sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v49 BEGIN failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }

      /* Snapshot pre-state for the boot-log delta line. */
      int pre_open_count = 0;
      sqlite3_stmt *count_stmt = NULL;
      if (sqlite3_prepare_v2(s_db.db,
                             "SELECT COUNT(*) FROM memory_relations WHERE valid_to IS NULL", -1,
                             &count_stmt, NULL) == SQLITE_OK) {
         if (sqlite3_step(count_stmt) == SQLITE_ROW) {
            pre_open_count = sqlite3_column_int(count_stmt, 0);
         }
         sqlite3_finalize(count_stmt);
      }

      /* Group key throughout: (user_id, subject_entity_id, relation,
       * COALESCE(object_entity_id, 0), COALESCE(object_value, '')) WHERE valid_to IS NULL.
       * COALESCE wrappers mirror the partial UNIQUE index expression so
       * NULL-object-entity-id and NULL-object-value resolve consistently
       * across grouping and conflict-target. */
      const char *v49_sql =
          /* Step 1: stamp winners.  MIN(id) row per group gets the count +
           * MAX(confidence).  Groups of size 1 fall through harmlessly
           * (mention_count stays at the ALTER default of 1). */
          "UPDATE memory_relations AS w SET "
          "  mention_count = ("
          "    SELECT COUNT(*) FROM memory_relations d "
          "     WHERE d.user_id = w.user_id "
          "       AND d.subject_entity_id = w.subject_entity_id "
          "       AND d.relation = w.relation "
          "       AND COALESCE(d.object_entity_id, 0) = COALESCE(w.object_entity_id, 0) "
          "       AND COALESCE(d.object_value, '') = COALESCE(w.object_value, '') "
          "       AND d.valid_to IS NULL), "
          "  confidence = ("
          "    SELECT MAX(d.confidence) FROM memory_relations d "
          "     WHERE d.user_id = w.user_id "
          "       AND d.subject_entity_id = w.subject_entity_id "
          "       AND d.relation = w.relation "
          "       AND COALESCE(d.object_entity_id, 0) = COALESCE(w.object_entity_id, 0) "
          "       AND COALESCE(d.object_value, '') = COALESCE(w.object_value, '') "
          "       AND d.valid_to IS NULL) "
          "WHERE w.valid_to IS NULL "
          "  AND w.id = (SELECT MIN(d.id) FROM memory_relations d "
          "               WHERE d.user_id = w.user_id "
          "                 AND d.subject_entity_id = w.subject_entity_id "
          "                 AND d.relation = w.relation "
          "                 AND COALESCE(d.object_entity_id, 0) = COALESCE(w.object_entity_id, 0) "
          "                 AND COALESCE(d.object_value, '') = COALESCE(w.object_value, '') "
          "                 AND d.valid_to IS NULL);"
          /* Step 2: refresh winner provenance to the latest witness as ONE
           * row-valued subquery.  Four columns from the same source row — a
           * tuple of separate scalar subqueries each pinned to MAX(id) would
           * not guarantee row-level consistency.  Skip groups of size 1
           * (mention_count = 1 means no dedup happened). */
          "UPDATE memory_relations AS w "
          "SET (source_conversation_id, source_msg_id_start, source_msg_id_end, fact_id) = ("
          "    SELECT d.source_conversation_id, d.source_msg_id_start, d.source_msg_id_end, "
          "           COALESCE(w.fact_id, d.fact_id) "
          "    FROM memory_relations d "
          "    WHERE d.user_id = w.user_id "
          "      AND d.subject_entity_id = w.subject_entity_id "
          "      AND d.relation = w.relation "
          "      AND COALESCE(d.object_entity_id, 0) = COALESCE(w.object_entity_id, 0) "
          "      AND COALESCE(d.object_value, '') = COALESCE(w.object_value, '') "
          "      AND d.valid_to IS NULL "
          "    ORDER BY d.id DESC LIMIT 1) "
          "WHERE w.valid_to IS NULL AND w.mention_count > 1 "
          "  AND w.id = (SELECT MIN(d.id) FROM memory_relations d "
          "               WHERE d.user_id = w.user_id "
          "                 AND d.subject_entity_id = w.subject_entity_id "
          "                 AND d.relation = w.relation "
          "                 AND COALESCE(d.object_entity_id, 0) = COALESCE(w.object_entity_id, 0) "
          "                 AND COALESCE(d.object_value, '') = COALESCE(w.object_value, '') "
          "                 AND d.valid_to IS NULL);"
          /* Step 3: DELETE non-winner duplicates from open rows only.
           * Closed historical rows untouched.  EXISTS with w.id < d.id finds
           * any earlier-id row in the same group → current row is a loser
           * iff such a row exists.  Reuses idx_memory_relations_subject_open
           * for both outer scan and EXISTS probe; no temp B-tree GROUP BY.
           * Equivalent to the simpler "id NOT IN (SELECT MIN(id) GROUP BY ...)"
           * shape but scales linearly with open-row count instead of holding
           * 50k+ tuples in scratch (embedded-efficiency-reviewer MED-1 fix). */
          "DELETE FROM memory_relations AS d "
          "WHERE d.valid_to IS NULL "
          "  AND EXISTS ("
          "    SELECT 1 FROM memory_relations w "
          "     WHERE w.valid_to IS NULL "
          "       AND w.user_id = d.user_id "
          "       AND w.subject_entity_id = d.subject_entity_id "
          "       AND w.relation = d.relation "
          "       AND COALESCE(w.object_entity_id, 0) = COALESCE(d.object_entity_id, 0) "
          "       AND COALESCE(w.object_value, '') = COALESCE(d.object_value, '') "
          "       AND w.id < d.id);"
          /* Step 4: enforce the invariant going forward.  Partial UNIQUE on
           * open relations.  COALESCE wrappers must match the ON CONFLICT
           * conflict-target in stmt_memory_relation_create exactly — SQLite
           * matches partial-index upserts on expression equality. */
          "CREATE UNIQUE INDEX IF NOT EXISTS idx_memory_relations_unique_open "
          "ON memory_relations(user_id, subject_entity_id, relation, "
          "                    COALESCE(object_entity_id, 0), "
          "                    COALESCE(object_value, '')) "
          "WHERE valid_to IS NULL;";

      rc = sqlite3_exec(s_db.db, v49_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v49 migration (dedup + unique index) failed: %s",
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
         sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      } else {
         int post_open_count = 0;
         if (sqlite3_prepare_v2(s_db.db,
                                "SELECT COUNT(*) FROM memory_relations WHERE valid_to IS NULL", -1,
                                &count_stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(count_stmt) == SQLITE_ROW) {
               post_open_count = sqlite3_column_int(count_stmt, 0);
            }
            sqlite3_finalize(count_stmt);
         }
         rc = sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, &errmsg);
         if (rc != SQLITE_OK) {
            OLOG_ERROR("auth_db: v49 COMMIT failed: %s", errmsg ? errmsg : "unknown");
            sqlite3_free(errmsg);
            errmsg = NULL;
            sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
         } else {
            OLOG_INFO("auth_db: v49 dedup: %d open relations -> %d winners "
                      "(%d duplicates collapsed); added idx_memory_relations_unique_open",
                      pre_open_count, post_open_count, pre_open_count - post_open_count);
            v49_ok = true;
         }
      }
   }

   /* v50 — briefing_steps table for multi-step briefings.  Single
    * BEGIN IMMEDIATE wrapping CREATE TABLE + CREATE INDEX + backfill of
    * existing single-tool briefings.  Backfills regardless of status
    * (terminal rows may still surface in history queries; steps
    * CASCADE-delete with the parent on purge anyway).
    *
    * Gated `>= 18` because scheduled_events itself only exists from v18.
    * On a pre-v18 DB the table doesn't exist; fresh installs at 0 create
    * briefing_steps via SCHEMA_SQL above and skip this block. */
   bool v50_ok = (current_version >= 50) || (current_version == 0);
   if (current_version >= 18 && current_version < 50) {
      rc = sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v50 BEGIN failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         const char *v50_sql =
             "CREATE TABLE IF NOT EXISTS briefing_steps ("
             "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "  event_id INTEGER NOT NULL,"
             "  seq INTEGER NOT NULL,"
             "  tool_name TEXT NOT NULL,"
             "  tool_action TEXT NOT NULL DEFAULT '',"
             "  tool_value TEXT NOT NULL DEFAULT '',"
             "  FOREIGN KEY (event_id) REFERENCES scheduled_events(id) ON DELETE CASCADE"
             ");"
             "CREATE INDEX IF NOT EXISTS idx_briefing_steps_event "
             "  ON briefing_steps(event_id, seq);"
             /* Backfill existing single-tool briefings into the new table. */
             "INSERT INTO briefing_steps(event_id, seq, tool_name, tool_action, tool_value) "
             "SELECT id, 0, tool_name, COALESCE(tool_action,''), COALESCE(tool_value,'') "
             "  FROM scheduled_events "
             " WHERE event_type='briefing' AND tool_name IS NOT NULL AND tool_name != '';";
         rc = sqlite3_exec(s_db.db, v50_sql, NULL, NULL, &errmsg);
         if (rc != SQLITE_OK) {
            OLOG_ERROR("auth_db: v50 migration failed: %s", errmsg ? errmsg : "unknown");
            sqlite3_free(errmsg);
            errmsg = NULL;
            sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
         } else {
            int backfill = 0;
            sqlite3_stmt *count_stmt = NULL;
            if (sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM briefing_steps", -1, &count_stmt,
                                   NULL) == SQLITE_OK) {
               if (sqlite3_step(count_stmt) == SQLITE_ROW)
                  backfill = sqlite3_column_int(count_stmt, 0);
               sqlite3_finalize(count_stmt);
            }
            sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL);
            OLOG_INFO("auth_db: v50 created briefing_steps table; backfilled %d existing "
                      "single-tool briefing(s)",
                      backfill);
            v50_ok = true;
         }
      }
   }

   /* v51 — messaging channels (Telegram / Discord / Slack / SMS).
    * Three new tables: messaging_channels (per-user channel bindings),
    * messaging_link_codes (10-min TTL one-time codes for the linking
    * flow), messaging_link_attempts (audit log for /link attempts).
    * Single BEGIN IMMEDIATE wrapping CREATE TABLE + CREATE INDEX.  No
    * data backfill (all new tables — empty on first boot).  See
    * docs/MESSAGING_CHANNELS_DESIGN.md §5. */
   bool v51_ok = (current_version >= 51) || (current_version == 0);
   if (current_version > 0 && current_version < 51) {
      rc = sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v51 BEGIN failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         const char *v51_sql =
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
             "  UNIQUE(user_id, provider, provider_address),"
             "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
             ");"
             "CREATE INDEX IF NOT EXISTS idx_messaging_channels_user "
             "  ON messaging_channels(user_id);"
             "CREATE INDEX IF NOT EXISTS idx_messaging_channels_provider_addr "
             "  ON messaging_channels(provider, provider_address);"
             "CREATE INDEX IF NOT EXISTS idx_messaging_channels_active "
             "  ON messaging_channels(provider, provider_address) WHERE is_enabled = 1;"
             "CREATE TABLE IF NOT EXISTS messaging_link_codes ("
             "  code TEXT PRIMARY KEY,"
             "  user_id INTEGER NOT NULL,"
             "  provider_hint TEXT "
             "    CHECK(provider_hint IN ('telegram','discord','slack','sms') "
             "          OR provider_hint IS NULL),"
             "  created_at INTEGER NOT NULL,"
             "  expires_at INTEGER NOT NULL,"
             "  claimed_at INTEGER,"
             "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE"
             ");"
             "CREATE INDEX IF NOT EXISTS idx_messaging_link_codes_expires "
             "  ON messaging_link_codes(expires_at);"
             "CREATE TABLE IF NOT EXISTS messaging_link_attempts ("
             "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "  provider TEXT NOT NULL,"
             "  sender_address TEXT NOT NULL,"
             "  code_tried TEXT,"
             "  result TEXT NOT NULL,"
             "  created_at INTEGER NOT NULL"
             ");"
             "CREATE INDEX IF NOT EXISTS idx_messaging_link_attempts_recent "
             "  ON messaging_link_attempts(provider, sender_address, created_at);";
         rc = sqlite3_exec(s_db.db, v51_sql, NULL, NULL, &errmsg);
         if (rc != SQLITE_OK) {
            OLOG_ERROR("auth_db: v51 migration failed: %s", errmsg ? errmsg : "unknown");
            sqlite3_free(errmsg);
            errmsg = NULL;
            sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
         } else {
            sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL);
            OLOG_INFO("auth_db: v51 created messaging_channels, messaging_link_codes, "
                      "messaging_link_attempts tables");
            v51_ok = true;
         }
      }
   }

   /* v52 — messaging_channels.conversation_id (forever-binding).
    * Adds a NULL-default column with FK to conversations(id) ON DELETE
    * SET NULL so a channel mapping persistently references one
    * conversation row that grows indefinitely.  LCM handles context
    * compaction in-place; recovery worker extracts memory
    * incrementally.  Literal-NULL default → SQLite O(1) ALTER fast
    * path.  Schema v51 just shipped (2026-05-26); no /link binding
    * predating that needs backfill — every existing row has
    * conversation_id = NULL on first read, which means "create a fresh
    * conv on next inbound."  See docs/MESSAGING_CHANNELS_DESIGN.md
    * §13 Phase 2.5 — forever-conversation model.
    *
    * Duplicate-column handling mirrors the v47 pattern: SCHEMA_SQL
    * (executed before this block on a multi-step migration from
    * pre-v51) may have already created the table at the v52 shape via
    * CREATE TABLE IF NOT EXISTS, leaving no work for the ALTER.  Treat
    * "duplicate column" as success in that case. */
   bool v52_ok = (current_version >= 52) || (current_version == 0);
   if (current_version > 0 && current_version < 52) {
      const char *v52_sql = "ALTER TABLE messaging_channels ADD COLUMN conversation_id INTEGER "
                            "REFERENCES conversations(id) ON DELETE SET NULL";
      rc = sqlite3_exec(s_db.db, v52_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK && !(errmsg && strstr(errmsg, "duplicate column"))) {
         OLOG_ERROR("auth_db: v52 migration failed: %s", errmsg ? errmsg : "unknown");
      } else {
         if (rc == SQLITE_OK) {
            OLOG_INFO("auth_db: v52 added messaging_channels.conversation_id (forever-binding)");
         } else {
            /* Duplicate-column path: SCHEMA_SQL already created the
             * column at v52 shape during multi-step migration via
             * CREATE TABLE IF NOT EXISTS.  Mark v52 ok without
             * claiming we did the ALTER ourselves. */
            OLOG_INFO("auth_db: v52 messaging_channels.conversation_id already present "
                      "(applied by SCHEMA_SQL during multi-step migration)");
         }
         v52_ok = true;
      }
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   /* v53 — scheduled_events.say_aloud (per-briefing TTS override).  Tri-state
    * INTEGER column with literal-zero default → SQLite O(1) ALTER fast path.
    * Backfill is implicit: every existing row reads 0 = SCHED_SAY_ALOUD_DEFAULT
    * which delegates to the source heuristic, so behavior is byte-identical
    * pre/post migration.  Only NEW briefings created via the LLM tool (or
    * future WebUI surface) opt into ALWAYS=1 / NEVER=2.
    *
    * Duplicate-column handling mirrors the v47/v52 pattern: SCHEMA_SQL
    * (executed before this block on a multi-step migration from pre-v18) may
    * have already created scheduled_events at the v53 shape via CREATE TABLE
    * IF NOT EXISTS, leaving no work for the ALTER.  Treat "duplicate column"
    * as success in that case. */
   bool v53_ok = (current_version >= 53) || (current_version == 0);
   if (current_version >= 18 && current_version < 53) {
      const char *v53_sql = "ALTER TABLE scheduled_events ADD COLUMN "
                            "say_aloud INTEGER NOT NULL DEFAULT 0";
      rc = sqlite3_exec(s_db.db, v53_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK && !(errmsg && strstr(errmsg, "duplicate column"))) {
         OLOG_ERROR("auth_db: v53 migration failed: %s", errmsg ? errmsg : "unknown");
      } else {
         if (rc == SQLITE_OK) {
            OLOG_INFO("auth_db: v53 added scheduled_events.say_aloud "
                      "(per-briefing TTS override)");
         } else {
            OLOG_INFO("auth_db: v53 scheduled_events.say_aloud already present "
                      "(applied by SCHEMA_SQL during multi-step migration)");
         }
         v53_ok = true;
      }
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   /* v54 — scheduled_events.deliver_to (messaging channel for fan-out).
    * Optional TEXT column holding a messaging_channels.display_name; when
    * present and non-empty at fire time, the scheduler fans the briefing
    * or task announcement out to that channel in addition to the existing
    * TTS + WebUI banner path.  NULL/empty = legacy behavior (no messaging
    * delivery).  Behavior is byte-identical pre/post migration since
    * existing rows read NULL.
    *
    * Duplicate-column handling mirrors v53. */
   bool v54_ok = (current_version >= 54) || (current_version == 0);
   if (current_version >= 18 && current_version < 54) {
      const char *v54_sql = "ALTER TABLE scheduled_events ADD COLUMN deliver_to TEXT";
      rc = sqlite3_exec(s_db.db, v54_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK && !(errmsg && strstr(errmsg, "duplicate column"))) {
         OLOG_ERROR("auth_db: v54 migration failed: %s", errmsg ? errmsg : "unknown");
      } else {
         if (rc == SQLITE_OK) {
            OLOG_INFO("auth_db: v54 added scheduled_events.deliver_to "
                      "(messaging channel for fan-out)");
         } else {
            OLOG_INFO("auth_db: v54 scheduled_events.deliver_to already present "
                      "(applied by SCHEMA_SQL during multi-step migration)");
         }
         v54_ok = true;
      }
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   /* v55 — raise email read body cap default to EMAIL_DEFAULT_BODY_CHARS.
    * max_body_chars caps the per-email body length returned to the LLM on
    * read.  The old 4000 default truncated ordinary newsletters mid-message;
    * the new default (mirrors EMAIL_MAX_READ_BODY_LEN in email_types.h) reads
    * virtually all real emails in full.  Per the maintainer's decision the cap
    * is treated as a ceiling, not a per-account preference, so ALL existing
    * accounts are bumped.  email_accounts is created (or recreated at current
    * shape) by SCHEMA_SQL earlier in this pass, so the table always exists by
    * the time this UPDATE runs. */
   bool v55_ok = (current_version >= 55) || (current_version == 0);
   if (current_version >= 18 && current_version < 55) {
      const char *v55_sql = "UPDATE email_accounts SET max_body_chars = " STRINGIFY(
          EMAIL_DEFAULT_BODY_CHARS);
      rc = sqlite3_exec(s_db.db, v55_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v55 migration failed: %s", errmsg ? errmsg : "unknown");
      } else {
         OLOG_INFO("auth_db: v55 raised email_accounts.max_body_chars to %d",
                   EMAIL_DEFAULT_BODY_CHARS);
         v55_ok = true;
      }
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   /* v56 — messages.tool_calls + messages.tool_call_id (structured tool-turn
    * persistence).  Two nullable TEXT columns: assistant rows store the OpenAI
    * tool_calls JSON array, role='tool' rows store the matching tool_call id.
    * Both NULL for the vast majority of rows (zero payload bytes), so behavior is
    * byte-identical pre/post migration.  Nullable ADD COLUMN is the O(1) fast path
    * (no table rebuild — the 'tool' role already exists in the CHECK constraint).
    * Duplicate-column handling mirrors v52/v53. */
   bool v56_ok = (current_version >= 56) || (current_version == 0);
   if (current_version >= 18 && current_version < 56) {
      bool v56_calls_ok = false;
      bool v56_id_ok = false;
      bool v56_fresh = false; /* true if either ALTER actually added a column */
      rc = sqlite3_exec(s_db.db, "ALTER TABLE messages ADD COLUMN tool_calls TEXT", NULL, NULL,
                        &errmsg);
      if (rc == SQLITE_OK) {
         v56_calls_ok = true;
         v56_fresh = true;
      } else if (errmsg && strstr(errmsg, "duplicate column")) {
         v56_calls_ok = true;
      } else {
         OLOG_ERROR("auth_db: v56 messages.tool_calls migration failed: %s",
                    errmsg ? errmsg : "unknown");
      }
      sqlite3_free(errmsg);
      errmsg = NULL;

      rc = sqlite3_exec(s_db.db, "ALTER TABLE messages ADD COLUMN tool_call_id TEXT", NULL, NULL,
                        &errmsg);
      if (rc == SQLITE_OK) {
         v56_id_ok = true;
         v56_fresh = true;
      } else if (errmsg && strstr(errmsg, "duplicate column")) {
         v56_id_ok = true;
      } else {
         OLOG_ERROR("auth_db: v56 messages.tool_call_id migration failed: %s",
                    errmsg ? errmsg : "unknown");
      }
      sqlite3_free(errmsg);
      errmsg = NULL;

      if (v56_calls_ok && v56_id_ok) {
         if (v56_fresh) {
            OLOG_INFO("auth_db: v56 added messages.tool_calls + tool_call_id "
                      "(structured tool-turn persistence)");
         } else {
            OLOG_INFO("auth_db: v56 messages.tool_call columns already present "
                      "(applied by SCHEMA_SQL during multi-step migration)");
         }
         v56_ok = true;
      }
   }

   /* v57 — messages.reasoning (display-only LLM reasoning/thinking JSON, persisted server-side
    * so the "AI thought" panel reconstructs at the correct position on reload).  Nullable for
    * nearly all rows; never read into the LLM context (only load_msg_callback delivers it to the
    * browser).  Single nullable ADD COLUMN — O(1) fast path.  Mirrors v56. */
   bool v57_ok = (current_version >= 57) || (current_version == 0);
   if (current_version >= 18 && current_version < 57) {
      bool v57_fresh = false;
      rc = sqlite3_exec(s_db.db, "ALTER TABLE messages ADD COLUMN reasoning TEXT", NULL, NULL,
                        &errmsg);
      if (rc == SQLITE_OK) {
         v57_ok = true;
         v57_fresh = true;
      } else if (errmsg && strstr(errmsg, "duplicate column")) {
         v57_ok = true;
      } else {
         OLOG_ERROR("auth_db: v57 messages.reasoning migration failed: %s",
                    errmsg ? errmsg : "unknown");
      }
      sqlite3_free(errmsg);
      errmsg = NULL;

      if (v57_ok) {
         if (v57_fresh) {
            OLOG_INFO("auth_db: v57 added messages.reasoning (server-side reasoning persistence)");
         } else {
            OLOG_INFO("auth_db: v57 messages.reasoning already present "
                      "(applied by SCHEMA_SQL during multi-step migration)");
         }
      }
   }

   /* v58 — memory_facts.expires_at (fact-lifecycle expiry / ephemerality, C3).
    * Nullable (NULL = durable, the default for every existing row), so no
    * back-fill and nothing breaks — Phase 1 is preventive: only new extractions
    * set expires_at.  Single nullable ADD COLUMN — O(1) fast path.  Mirrors v57. */
   bool v58_ok = (current_version >= 58) || (current_version == 0);
   if (current_version >= 18 && current_version < 58) {
      bool v58_fresh = false;
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE memory_facts ADD COLUMN expires_at INTEGER DEFAULT NULL", NULL,
                        NULL, &errmsg);
      if (rc == SQLITE_OK) {
         v58_ok = true;
         v58_fresh = true;
      } else if (errmsg && strstr(errmsg, "duplicate column")) {
         v58_ok = true;
      } else {
         OLOG_ERROR("auth_db: v58 memory_facts.expires_at migration failed: %s",
                    errmsg ? errmsg : "unknown");
      }
      sqlite3_free(errmsg);
      errmsg = NULL;

      if (v58_ok) {
         if (v58_fresh) {
            OLOG_INFO("auth_db: v58 added memory_facts.expires_at (fact-lifecycle expiry)");
         } else {
            OLOG_INFO("auth_db: v58 memory_facts.expires_at already present "
                      "(applied by SCHEMA_SQL during multi-step migration)");
         }
      }
   }

   /* v59 — ota_device_state (server→satellite OTA per-device state).
    * Single new table, created on existing DBs via CREATE TABLE IF NOT EXISTS.
    * No backfill (empty on first boot; rows created lazily when a device first
    * reports a firmware_version).  See docs/OTA_DESIGN.md §5. */
   bool v59_ok = (current_version >= 59) || (current_version == 0);
   if (current_version > 0 && current_version < 59) {
      const char *v59_sql = "CREATE TABLE IF NOT EXISTS ota_device_state ("
                            "   uuid TEXT PRIMARY KEY,"
                            "   current_version TEXT NOT NULL DEFAULT '',"
                            "   target_version TEXT,"
                            "   state TEXT NOT NULL DEFAULT 'idle',"
                            "   last_error TEXT,"
                            "   token TEXT,"
                            "   token_expires INTEGER,"
                            "   created_at INTEGER NOT NULL,"
                            "   updated_at INTEGER NOT NULL"
                            ");";
      rc = sqlite3_exec(s_db.db, v59_sql, NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v59 ota_device_state migration failed: %s",
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: v59 created ota_device_state table");
         v59_ok = true;
      }
   }

   /* v60 — ota_device_state.target_platform.  Binds the one-time download token to
    * the platform the offer was for, so a device offered rpi/X cannot consume that
    * token to fetch esp32/X.  Nullable (NULL = no in-flight offer), no backfill;
    * cleared alongside target_version.  Single nullable ADD COLUMN — O(1).  Runs on
    * any DB that already has the v59 table (which v59's migration just created for
    * pre-59 DBs, so this ALTER follows it). */
   bool v60_ok = (current_version >= 60) || (current_version == 0);
   if (current_version > 0 && current_version < 60) {
      rc = sqlite3_exec(s_db.db, "ALTER TABLE ota_device_state ADD COLUMN target_platform TEXT",
                        NULL, NULL, &errmsg);
      if (rc == SQLITE_OK) {
         v60_ok = true;
         OLOG_INFO("auth_db: v60 added ota_device_state.target_platform");
      } else if (errmsg && strstr(errmsg, "duplicate column")) {
         v60_ok = true;
      } else {
         OLOG_ERROR("auth_db: v60 ota_device_state.target_platform migration failed: %s",
                    errmsg ? errmsg : "unknown");
      }
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   /* v61 migration: two parts, both gated into v61_ok.
    *   (1) memory_facts.note_doc_id — the memory→note bridge pointer.  Plain
    *       ADD COLUMN with no inline FK, matching the v47 subject_entity_id
    *       precedent: the FK (ON DELETE SET NULL) lives in the fresh-install
    *       CREATE, and on upgraded DBs the note-delete path nulls glosses
    *       explicitly, so the FK is only a fresh-install backstop.
    *   (2) document_chunks_fts — the BM25 lexical channel for document search +
    *       exact-label note retrieval.  CREATE (idempotent) + backfill every
    *       existing chunk: stem the parent document's filename into label_stems
    *       and the chunk text into body_stems.  Mirrors the v48 memory_facts_fts
    *       backfill, including the partial-backfill-still-advances policy and the
    *       global-IDF caveat (per-user safety is the JOIN+filter in the search
    *       statement, not the index).
    * Fresh installs (current_version == 0) get both directly from SCHEMA_SQL. */
   bool v61_ok = (current_version >= 61) || (current_version == 0);
   if (current_version >= 1 && current_version < 61) {
      bool alter_ok = false;
      rc = sqlite3_exec(s_db.db,
                        "ALTER TABLE memory_facts ADD COLUMN note_doc_id INTEGER DEFAULT NULL",
                        NULL, NULL, &errmsg);
      if (rc == SQLITE_OK) {
         alter_ok = true;
         OLOG_INFO("auth_db: v61 added memory_facts.note_doc_id");
      } else if (errmsg && strstr(errmsg, "duplicate column")) {
         alter_ok = true;
      } else {
         OLOG_ERROR("auth_db: v61 memory_facts.note_doc_id migration failed: %s",
                    errmsg ? errmsg : "unknown");
      }
      sqlite3_free(errmsg);
      errmsg = NULL;

      bool fts_ok = false;
      rc = sqlite3_exec(s_db.db,
                        "CREATE VIRTUAL TABLE IF NOT EXISTS document_chunks_fts USING fts5("
                        "   label_stems,"
                        "   body_stems,"
                        "   tokenize='unicode61 remove_diacritics 2',"
                        "   content=''"
                        ")",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: v61 migration (document_chunks_fts CREATE) failed: %s — "
                    "leaving schema_version at %d so next boot retries",
                    errmsg ? errmsg : "unknown", current_version);
         sqlite3_free(errmsg);
         errmsg = NULL;
      } else {
         OLOG_INFO("auth_db: created document_chunks_fts virtual table (v61)");

         /* Backfill from existing document_chunks.  Stemming runs in C (SQL
          * triggers can't call libstemmer); a single transaction keeps the
          * backfill atomic across all chunks. */
         (void)memory_stem_init();
         int backfill_count = 0;
         int backfill_errors = 0;
         sqlite3_stmt *select_stmt = NULL;
         sqlite3_stmt *insert_stmt = NULL;
         int prep_rc = sqlite3_prepare_v2(s_db.db,
                                          "SELECT c.id, c.text, d.filename FROM document_chunks c "
                                          "JOIN documents d ON d.id = c.document_id",
                                          -1, &select_stmt, NULL);
         if (prep_rc == SQLITE_OK) {
            prep_rc = sqlite3_prepare_v2(s_db.db,
                                         "INSERT INTO document_chunks_fts(rowid, label_stems, "
                                         "body_stems) VALUES (?, ?, ?)",
                                         -1, &insert_stmt, NULL);
         }
         int commit_rc = SQLITE_DONE;
         if (prep_rc == SQLITE_OK) {
            (void)sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
            while (sqlite3_step(select_stmt) == SQLITE_ROW) {
               int64_t cid = sqlite3_column_int64(select_stmt, 0);
               const unsigned char *body = sqlite3_column_text(select_stmt, 1);
               const unsigned char *label = sqlite3_column_text(select_stmt, 2);
               char label_stems[MEMORY_FACT_STEMS_MAX];
               char body_stems[MEMORY_FACT_STEMS_MAX];
               (void)memory_stem_string((const char *)(label ? label : (const unsigned char *)""),
                                        label_stems, sizeof(label_stems));
               (void)memory_stem_string((const char *)(body ? body : (const unsigned char *)""),
                                        body_stems, sizeof(body_stems));
               sqlite3_reset(insert_stmt);
               sqlite3_bind_int64(insert_stmt, 1, cid);
               sqlite3_bind_text(insert_stmt, 2, label_stems, -1, SQLITE_TRANSIENT);
               sqlite3_bind_text(insert_stmt, 3, body_stems, -1, SQLITE_TRANSIENT);
               if (sqlite3_step(insert_stmt) != SQLITE_DONE) {
                  backfill_errors++;
               } else {
                  backfill_count++;
               }
            }
            commit_rc = sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL);
            if (commit_rc != SQLITE_OK) {
               /* COMMIT failed (e.g. SQLITE_BUSY / IO) — roll back so the
                * connection returns to autocommit instead of leaving an open
                * transaction that would hold locks and taint later startup SQL.
                * schema_version stays unbumped below, so the next boot retries. */
               (void)sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
            }
         }
         if (select_stmt)
            sqlite3_finalize(select_stmt);
         if (insert_stmt)
            sqlite3_finalize(insert_stmt);
         if (backfill_errors > 0) {
            OLOG_WARNING("auth_db: v61 document FTS backfill PARTIAL: %d indexed, %d failed — "
                         "some chunks won't surface via lexical search until a manual rebuild",
                         backfill_count, backfill_errors);
         } else {
            OLOG_INFO("auth_db: v61 document FTS backfill complete: %d chunks indexed",
                      backfill_count);
         }
         if (prep_rc == SQLITE_OK && commit_rc == SQLITE_OK) {
            fts_ok = true;
         } else {
            OLOG_ERROR("auth_db: v61 migration COMMIT or statement prep failed — "
                       "leaving schema_version at %d so next boot retries",
                       current_version);
         }
      }

      v61_ok = alter_ok && fts_ok;
   }

   /* v62 migration: document_versions — soft-archive of pre-mutation content for
    * undo/restore.  Plain CREATE (idempotent), no backfill (snapshots accrue
    * going forward).  Fresh installs get it from SCHEMA_SQL.  No FK to documents
    * by design (a version must outlive a deleted document). */
   bool v62_ok = (current_version >= 62) || (current_version == 0);
   if (current_version >= 1 && current_version < 62) {
      rc = sqlite3_exec(s_db.db,
                        "CREATE TABLE IF NOT EXISTS document_versions ("
                        "  id INTEGER PRIMARY KEY,"
                        "  document_id INTEGER NOT NULL,"
                        "  user_id INTEGER NOT NULL,"
                        "  filename TEXT NOT NULL,"
                        "  text TEXT NOT NULL,"
                        "  archived_at INTEGER NOT NULL,"
                        "  FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE"
                        ")",
                        NULL, NULL, &errmsg);
      if (rc == SQLITE_OK) {
         v62_ok = true;
         OLOG_INFO("auth_db: created document_versions table (v62)");
      } else {
         OLOG_ERROR("auth_db: v62 document_versions migration failed: %s — "
                    "leaving schema_version at %d so next boot retries",
                    errmsg ? errmsg : "unknown", current_version);
      }
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   /* v63 migration: document_full_text — canonical un-chunked text for multi-chunk
    * document editing.  Plain CREATE (idempotent), NO backfill: existing
    * multi-chunk docs have no stored full text and can't be cleanly reconstructed
    * from overlapping chunks, so editing them prompts a re-save (which populates
    * full_text going forward).  Fresh installs get it from SCHEMA_SQL. */
   bool v63_ok = (current_version >= 63) || (current_version == 0);
   if (current_version >= 1 && current_version < 63) {
      rc = sqlite3_exec(s_db.db,
                        "CREATE TABLE IF NOT EXISTS document_full_text ("
                        "  document_id INTEGER PRIMARY KEY,"
                        "  text TEXT NOT NULL,"
                        "  FOREIGN KEY(document_id) REFERENCES documents(id) ON DELETE CASCADE"
                        ")",
                        NULL, NULL, &errmsg);
      if (rc == SQLITE_OK) {
         v63_ok = true;
         OLOG_INFO("auth_db: created document_full_text table (v63)");
      } else {
         OLOG_ERROR("auth_db: v63 document_full_text migration failed: %s — "
                    "leaving schema_version at %d so next boot retries",
                    errmsg ? errmsg : "unknown", current_version);
      }
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   /* Create indexes that depend on migration-added columns.
    * Runs for both fresh installs and migrations — must come after all migrations. */
   rc = sqlite3_exec(s_db.db,
                     "CREATE INDEX IF NOT EXISTS idx_conversations_continued "
                     "ON conversations(continued_from)",
                     NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("auth_db: could not create continuation index: %s", errmsg ? errmsg : "ok");
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   rc = sqlite3_exec(s_db.db,
                     "CREATE INDEX IF NOT EXISTS idx_summary_nodes_conv "
                     "ON summary_nodes(conversation_id)",
                     NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("auth_db: could not create summary_nodes index: %s", errmsg ? errmsg : "ok");
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   /* v47 post-migration index: idx_memory_facts_subject supports the
    * fact-to-entity reverse lookup that graph traversal will use ("give me
    * every fact whose subject is entity X").  Created here because on an
    * existing pre-v47 DB the column doesn't exist until the ALTER TABLE
    * above runs — same pattern as v34's category index. */
   rc = sqlite3_exec(s_db.db,
                     "CREATE INDEX IF NOT EXISTS idx_memory_facts_subject "
                     "ON memory_facts(user_id, subject_entity_id) "
                     "WHERE subject_entity_id IS NOT NULL",
                     NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("auth_db: could not create memory_facts subject index: %s",
                   errmsg ? errmsg : "ok");
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   /* v58 post-migration index: idx_memory_facts_expires keeps both the nightly
    * prune_expired scan and the retrieval guard's expiry predicate cheap.
    * Partial (WHERE expires_at IS NOT NULL) because the transient set is a
    * minority — the index stays tiny and durable (NULL) rows never enter it. */
   rc = sqlite3_exec(s_db.db,
                     "CREATE INDEX IF NOT EXISTS idx_memory_facts_expires "
                     "ON memory_facts(user_id, expires_at) "
                     "WHERE expires_at IS NOT NULL",
                     NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("auth_db: could not create memory_facts expires index: %s",
                   errmsg ? errmsg : "ok");
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   rc = sqlite3_exec(s_db.db,
                     "CREATE INDEX IF NOT EXISTS idx_images_retention "
                     "ON images(retention_policy)",
                     NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("auth_db: could not create retention index: %s", errmsg ? errmsg : "ok");
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   /* v62 post-migration index: document_versions lookups are always by
    * document_id, newest-first (list a doc's history, enforce the per-doc cap,
    * sweep by age).  Created here so upgraded DBs get it (the v62 migration
    * CREATE TABLE doesn't include it; SCHEMA_SQL does for fresh installs). */
   rc = sqlite3_exec(s_db.db,
                     "CREATE INDEX IF NOT EXISTS idx_doc_versions_doc "
                     "ON document_versions(document_id, archived_at DESC)",
                     NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("auth_db: could not create document_versions index: %s", errmsg ? errmsg : "ok");
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   /* Indexes on migration-added columns (v33/v34/v43).  Must run here rather
    * than in SCHEMA_SQL because on an existing pre-migration DB, CREATE TABLE
    * IF NOT EXISTS is a no-op, so the new columns don't exist until migrations
    * run.  Migrations also create these indexes but only fire on DBs with
    * current_version >= 1 — fresh installs (version 0) skip all migrations
    * and reach this block instead. */
   rc = sqlite3_exec(s_db.db,
                     "CREATE INDEX IF NOT EXISTS idx_memory_facts_user_category "
                     "ON memory_facts(user_id, category);"
                     "CREATE INDEX IF NOT EXISTS idx_memory_relations_user_validity "
                     "ON memory_relations(user_id, valid_to);"
                     "CREATE INDEX IF NOT EXISTS idx_memory_relations_subject_open "
                     "ON memory_relations(subject_entity_id, relation) "
                     "WHERE valid_to IS NULL;"
                     /* v49: partial UNIQUE backing the upsert in
                      * stmt_memory_relation_create.  CREATE IF NOT EXISTS so the
                      * migration block above (which also creates it) and this
                      * fresh-install pass don't fight.  Required for the
                      * stmt_memory_relation_create prepare to succeed — without
                      * the index, ON CONFLICT(... COALESCE(...)) WHERE valid_to
                      * IS NULL has no matching uniqueness constraint to
                      * resolve against. */
                     "CREATE UNIQUE INDEX IF NOT EXISTS idx_memory_relations_unique_open "
                     "ON memory_relations(user_id, subject_entity_id, relation, "
                     "                    COALESCE(object_entity_id, 0), "
                     "                    COALESCE(object_value, '')) "
                     "WHERE valid_to IS NULL",
                     NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("auth_db: could not create memory v33/v34/v49 indexes: %s",
                   errmsg ? errmsg : "ok");
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   /* v43 indexes.  idx_memory_entities_user_self is a partial UNIQUE index
    * enforcing the one-self-per-user invariant — a second is_user_self=1 row
    * for the same user_id will fail the constraint.  The two partial indexes
    * on memory_entities both depend on v43 columns; the alias / proposal
    * indexes depend on the v43 tables.  All five are CREATE IF NOT EXISTS so
    * they're idempotent across fresh-install + migration paths.
    *
    * idx_contacts_field_lvalue (functional index on lower(value)) backs the
    * contacts self-join inside compute_contact_field_overlap.  Without it,
    * the alias scorer's inner contact-pair JOIN runs O(N²) over a user's
    * contacts on every score_pair call (Phase 2 will fire that on every
    * extraction).  With the index it's O(log N) per JOIN.  No schema
    * version bump needed — index creation is purely an optimization that
    * back-fills cleanly on the dev's existing DB at next boot. */
   rc = sqlite3_exec(s_db.db,
                     "CREATE INDEX IF NOT EXISTS idx_memory_entities_canonical "
                     "ON memory_entities(canonical_id) WHERE canonical_id IS NOT NULL;"
                     "CREATE UNIQUE INDEX IF NOT EXISTS idx_memory_entities_user_self "
                     "ON memory_entities(user_id) WHERE is_user_self = 1;"
                     "CREATE INDEX IF NOT EXISTS idx_memory_entity_aliases_user_target "
                     "ON memory_entity_aliases(user_id, target_entity_id) "
                     "WHERE unlinked_at IS NULL;"
                     "CREATE INDEX IF NOT EXISTS idx_merge_proposals_pending "
                     "ON memory_entity_merge_proposals(user_id, proposed_at) "
                     "WHERE resolved_at IS NULL;"
                     "CREATE INDEX IF NOT EXISTS idx_contacts_field_lvalue "
                     "ON contacts(user_id, field_type, lower(value))",
                     NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("auth_db: could not create memory v43 indexes: %s", errmsg ? errmsg : "ok");
      sqlite3_free(errmsg);
      errmsg = NULL;
   }

   /* v64 — mcp_user_access (coding-harness MCP bridge per-user allowlist).
    * Idempotent table creation via a split helper (keeps the migration ladder
    * DDL-free).  NOT gated on DAWN_ENABLE_MCP_BRIDGE_TOOL: schema versioning is
    * global and must advance uniformly across build configs. */
   bool v64_ok = (current_version >= 64);
   if (current_version < 64) {
      if (auth_db_migrations_v64(s_db.db) == AUTH_DB_SUCCESS) {
         v64_ok = true;
      } else {
         OLOG_ERROR("auth_db: v64 migration (mcp_user_access) failed");
      }
   }

   /* v65 — code_projects (coding-harness imported repositories).  Idempotent
    * table creation via a split helper; NOT gated on DAWN_ENABLE_CODE_PROJECTS
    * (schema versioning is global). */
   bool v65_ok = (current_version >= 65);
   if (current_version < 65) {
      if (auth_db_migrations_v65(s_db.db) == AUTH_DB_SUCCESS) {
         v65_ok = true;
      } else {
         OLOG_ERROR("auth_db: v65 migration (code_projects) failed");
      }
   }

   /* v66 — code_projects branch/kind/graph_name columns (branch tracking,
    * link-local repos, persisted cbm graph slug).  Idempotent ALTERs (probe
    * PRAGMA table_info); NOT gated on DAWN_ENABLE_CODE_PROJECTS. */
   bool v66_ok = (current_version >= 66);
   if (current_version < 66) {
      if (auth_db_migrations_v66(s_db.db) == AUTH_DB_SUCCESS) {
         v66_ok = true;
      } else {
         OLOG_ERROR("auth_db: v66 migration (code_projects columns) failed");
      }
   }

   /* Log migration if upgrading from an older version */
   if (current_version > 0 && current_version < AUTH_DB_SCHEMA_VERSION) {
      OLOG_INFO("auth_db: migrated schema from v%d to v%d", current_version,
                AUTH_DB_SCHEMA_VERSION);
   } else if (current_version == 0) {
      OLOG_INFO("auth_db: created schema v%d", AUTH_DB_SCHEMA_VERSION);
   }

   /* Only update schema_version if we actually migrated or created fresh,
    * AND every per-version migration step that gates a bump succeeded.
    * `v48_ok` is the tracking flag for the v48 (FTS5 / BM25) migration —
    * extend the conjunction below as future migrations add their own
    * success gates.  Without this, a transient CREATE / backfill failure
    * would leave the DB advertised as v48 with no FTS5 table, and the
    * next boot's prepare_statements pass would fail (hard) on the BM25
    * statement prep, with no operator-visible recovery path.
    *
    * Never downgrade — prevents old code from corrupting a newer DB. */
   const bool ready_to_bump = v48_ok && v49_ok && v50_ok && v51_ok && v52_ok && v53_ok && v54_ok &&
                              v55_ok && v56_ok && v57_ok && v58_ok && v59_ok && v60_ok && v61_ok &&
                              v62_ok && v63_ok && v64_ok && v65_ok && v66_ok;
   if (current_version < AUTH_DB_SCHEMA_VERSION && ready_to_bump) {
      rc = sqlite3_exec(s_db.db, "DELETE FROM schema_version", NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_WARNING("auth_db: failed to clear schema_version: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         errmsg = NULL;
      }
      rc = sqlite3_exec(s_db.db,
                        "INSERT INTO schema_version (version) VALUES (" STRINGIFY(
                            AUTH_DB_SCHEMA_VERSION) ")",
                        NULL, NULL, &errmsg);
      if (rc != SQLITE_OK) {
         OLOG_ERROR("auth_db: failed to set schema version: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
   } else if (current_version > AUTH_DB_SCHEMA_VERSION) {
      OLOG_WARNING("auth_db: database is newer (v%d) than code (v%d) — not downgrading",
                   current_version, AUTH_DB_SCHEMA_VERSION);
   } else if (current_version < AUTH_DB_SCHEMA_VERSION && !ready_to_bump) {
      OLOG_WARNING("auth_db: schema_version held at v%d (target v%d) — one or more migration "
                   "steps failed; daemon will retry on next boot",
                   current_version, AUTH_DB_SCHEMA_VERSION);
   }

   return AUTH_DB_SUCCESS;
}
