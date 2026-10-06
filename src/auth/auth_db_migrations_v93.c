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
 * Schema migration v93: voice rows whose tool calls were never saved.
 *
 * Before this version the voice save kept only each message's content.  A
 * tool-call turn from an OpenAI-format model has none, so it was saved as an
 * empty assistant row with its calls dropped, and each result as a tool row
 * with no call id.  The calls can't be recovered.  Those rows become text the
 * WebUI shows as tool entries and a model reads as notes: the turn a note that
 * a tool was called, each result an assistant note carrying its text (never a
 * user message: tool output isn't something the user said).  Nothing is
 * invented, a result is visible again, and a continued conversation sends no
 * empty turn and no result without its call.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

/* Conversations the old voice save wrote: voice origin, and not a background
 * job (a job spawned from voice has that origin too, but its rows come from
 * the tool loop, which always saved calls). */
#define V93_VOICE_CONVERSATIONS                                               \
   "conversation_id IN (SELECT id FROM conversations WHERE origin = 'voice' " \
   "AND job_status IS NULL)"

/* Content with nothing to show: SQLite's trim() strips only spaces unless told. */
#define V93_BLANK(col) "trim(" col ", ' ' || char(9, 10, 13)) = ''"

/* An assistant row with no calls and no stored blocks. */
#define V93_CALLLESS_ASSISTANT                                             \
   "role = 'assistant' AND tool_calls IS NULL AND llm_blocks_len IS NULL " \
   "AND " V93_VOICE_CONVERSATIONS

/* The row after it in its conversation is a result the voice save kept without
 * its call id. */
#define V93_NEXT_IS_LOST_RESULT                                                   \
   "coalesce((SELECT n.role = 'tool' AND n.tool_call_id IS NULL FROM messages n " \
   "WHERE n.conversation_id = messages.conversation_id AND n.id > messages.id "   \
   "ORDER BY n.id LIMIT 1), 0)"

#define V93_CALL_NOTE "'[Tool Call: its name and arguments weren''t saved]'"

int auth_db_migrations_v93(sqlite3 *db) {
   if (!db) {
      return AUTH_DB_FAILURE;
   }
   static const char *const steps[] = {
      /* The calls first: they are recognized by the results after them.  A
       * turn with nothing to show gets the note (or, with no results after
       * it, "[No text]": an empty turn is one a provider rejects on replay);
       * one with text keeps it, the note after it. */
      "UPDATE messages SET content = CASE WHEN " V93_NEXT_IS_LOST_RESULT " THEN " V93_CALL_NOTE
      " ELSE '[No text]' END WHERE " V93_BLANK("content") " AND " V93_CALLLESS_ASSISTANT,
      "UPDATE messages SET content = content || char(10, 10) || " V93_CALL_NOTE " WHERE "
      "NOT " V93_BLANK("content") " AND content != " V93_CALL_NOTE " AND content NOT LIKE "
                                  "'%' || " V93_CALL_NOTE " AND " V93_NEXT_IS_LOST_RESULT
                                  " AND " V93_CALLLESS_ASSISTANT,
      /* Then the results: notes on the assistant's side, as the runtime renders a
       * result without its call.  Never the user's: tool output is not
       * something the user said (memory extraction reads user turns as theirs,
       * and a replay would put it where instructions go). */
      "UPDATE messages SET role = 'assistant', is_error = 0, content = CASE WHEN " V93_BLANK(
          "content") " THEN '[Tool Result: (no text)]' ELSE '[Tool Result: ' || "
                     "content || ']' END WHERE role = 'tool' AND tool_call_id IS NULL "
                     "AND " V93_VOICE_CONVERSATIONS,
   };

   if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v93 BEGIN failed: %s", sqlite3_errmsg(db));
      return AUTH_DB_FAILURE;
   }
   int changed = 0;
   for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
      char *errmsg = NULL;
      if (sqlite3_exec(db, steps[i], NULL, NULL, &errmsg) != SQLITE_OK) {
         OLOG_ERROR("auth_db: v93 step %zu failed: %s", i + 1, errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
         return AUTH_DB_FAILURE;
      }
      changed += sqlite3_changes(db);
   }
   if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v93 COMMIT failed: %s", sqlite3_errmsg(db));
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      return AUTH_DB_FAILURE;
   }
   if (changed > 0) {
      OLOG_INFO("auth_db: v93 rewrote %d voice message row(s) whose tool calls weren't saved",
                changed);
   }
   return AUTH_DB_SUCCESS;
}
