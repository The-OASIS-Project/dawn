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
 * Schema migration v96: a compacted conversation's summary now rides in front
 * of its first kept question, as its reload renders it, instead of an
 * assistant message of its own.  Reasoning its kept turns recorded was bound
 * to the old shape, so a compacted conversation leaves it behind once: its
 * reasoning floor rises to its newest row.  Text and tool calls stay.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

int auth_db_migrations_v96(sqlite3 *db) {
   char *errmsg = NULL;
   /* kind-rows: every row the conversation has. */
   if (sqlite3_exec(db,
                    "UPDATE conversations SET reasoning_floor_msg_id = MAX(reasoning_floor_msg_id, "
                    "(SELECT COALESCE(MAX(id), 0) FROM messages WHERE conversation_id = "
                    "conversations.id)) "
                    "WHERE context_watermark_msg_id > 0 AND compaction_summary IS NOT NULL "
                    "AND compaction_summary != ''",
                    NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v96 reasoning floor for compacted conversations failed: %s",
                 errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   const int changed = sqlite3_changes(db);
   if (changed > 0) {
      OLOG_INFO("auth_db: v96 left the earlier reasoning of %d compacted conversation(s) behind",
                changed);
   }
   return AUTH_DB_SUCCESS;
}
