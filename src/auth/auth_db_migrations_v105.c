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
 * Schema migration v105: message_unprompted, the assistant rows DAWN wrote into a
 * conversation on its own (a briefing or message delivered to a channel, a
 * completion note), so memory extraction never reads them as part of the
 * conversation.  A side table, so messages keeps its stored blocks as the last
 * columns (v98); a mark goes with its message.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

int auth_db_migrations_v105(sqlite3 *db) {
   char *errmsg = NULL;
   if (sqlite3_exec(db,
                    "CREATE TABLE IF NOT EXISTS message_unprompted ("
                    "  message_id INTEGER PRIMARY KEY REFERENCES messages(id) ON DELETE CASCADE)",
                    NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v105 message_unprompted failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   /* Posts saved before they were marked: a briefing delivered to a channel
    * was stored word for word in its own briefing conversation too, and a
    * research completion note has a fixed opening.  A post is never a tool
    * call and never empty (a briefing's tool steps are empty assistant rows),
    * so neither arm can take a model turn for one.  Near-exact: a model reply
    * imitating the research note's opening would be marked too, and it is the
    * assistant's words either way.  Channel reminders and alerts have no fixed
    * text to match and stay unmarked. */
   if (sqlite3_exec(db,
                    "INSERT OR IGNORE INTO message_unprompted (message_id) "
                    "SELECT m.id FROM messages m JOIN conversations c ON c.id = m.conversation_id "
                    "WHERE m.role = 'assistant' AND m.kind IS NULL AND m.tool_calls IS NULL "
                    "AND m.content <> '' AND ("
                    "  (c.origin LIKE 'messaging:%' AND EXISTS (SELECT 1 FROM messages b "
                    "   JOIN conversations bc ON bc.id = b.conversation_id "
                    "   WHERE bc.origin = 'briefing' AND bc.user_id = c.user_id "
                    "   AND b.role = 'assistant' AND b.tool_calls IS NULL "
                    "   AND b.content = m.content))"
                    "  OR m.content LIKE '🔍 Deep research %')",
                    NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v105 marking earlier posts failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}
