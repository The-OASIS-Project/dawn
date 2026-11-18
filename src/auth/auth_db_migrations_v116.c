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
 * Schema migration v116.  A conversation whose replay was recorded dropping
 * signed reasoning (llm_usage_log.binding_prefix_drops: two sessions wrote it
 * in turn, so its stored order no longer matches what the reasoning was signed
 * against) leaves that reasoning behind: its floor rises to its newest row,
 * never lowers.  The vendor was already dropping it from the first mismatch;
 * text and tool calls still replay.  Also memory_extract_ranges: the ranges an
 * admin extracted behind a conversation's cursor, so a range is never learned
 * twice.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

int auth_db_migrations_v116(sqlite3 *db) {
   char *errmsg = NULL;
   if (sqlite3_exec(db,
                    "CREATE TABLE IF NOT EXISTS memory_extract_ranges ("
                    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    "  conversation_id INTEGER NOT NULL REFERENCES conversations(id) "
                    "ON DELETE CASCADE,"
                    "  msg_id_start INTEGER NOT NULL,"
                    "  msg_id_end INTEGER NOT NULL,"
                    "  created_at INTEGER NOT NULL);"
                    "CREATE INDEX IF NOT EXISTS idx_memory_extract_ranges_conv "
                    "  ON memory_extract_ranges(conversation_id, msg_id_start);",
                    NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v116 memory_extract_ranges failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   /* The log's drop count came with v91; a database without it has none to
    * act on. */
   if (!auth_db_column_exists(db, "llm_usage_log", "binding_prefix_drops")) {
      return AUTH_DB_SUCCESS;
   }
   /* Found through the usage log, which keeps LLM_USAGE_RETENTION_DAYS of
    * calls: a conversation whose last recorded drop is older than that isn't
    * reached here. */
   if (sqlite3_exec(db,
                    "UPDATE conversations SET reasoning_floor_msg_id = MAX(reasoning_floor_msg_id, "
                    "(SELECT COALESCE(MAX(id), 0) FROM messages WHERE conversation_id = "
                    "conversations.id)) "
                    "WHERE id IN (SELECT DISTINCT conversation_id FROM llm_usage_log "
                    "WHERE conversation_id > 0 AND binding_prefix_drops > 0)",
                    NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v116 reasoning floor for conversations with dropped reasoning "
                 "failed: %s",
                 errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   const int changed = sqlite3_changes(db);
   if (changed > 0) {
      OLOG_INFO("auth_db: v116 left the earlier reasoning of %d conversation(s) behind", changed);
   }
   return AUTH_DB_SUCCESS;
}
