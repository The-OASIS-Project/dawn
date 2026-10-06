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
 * Schema migration v100: a conversation's compaction summary is stored as it
 * is sent.  It was neutralized again at every render; it is now neutralized
 * once, when it is made, and replayed verbatim, so a later change of the
 * neutralizer's rules can't re-render what a conversation already sent.
 *
 * Each stored summary is brought to the current rules here, once.  When that
 * leaves it unchanged, it renders exactly as it did: every rule a released
 * build applied is one the current neutralizer applies, so text the current
 * rules leave alone was left alone before too.  When it changes it, the
 * conversation's requests from here on differ from what its earlier turns
 * were sent under, so that is a declared boundary: the conversation's
 * reasoning floor rises to its newest row (its earlier turns replay without
 * their reasoning, as after any boundary), never a silent re-render.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "llm/llm_context_text.h"
#include "logging.h"

/* Bring conversation @p conv's summary to the current rules, counting it in
 * *@p changed when that changed it, and in *@p boundaries when it was
 * rendered into the conversation's requests. */
static int restore_one(sqlite3 *db,
                       int64_t conv,
                       const char *stored,
                       int64_t watermark,
                       int *changed,
                       int *boundaries) {
   char *now = llm_context_neutralize(stored);
   if (!now) {
      return AUTH_DB_FAILURE;
   }
   int rc = AUTH_DB_SUCCESS;
   if (strcmp(now, stored) != 0) {
      sqlite3_stmt *st = NULL;
      const bool rendered = watermark > 0; /* sent in front of its first kept question */
      const char *sql =
          rendered ? "UPDATE conversations SET compaction_summary = ?1, reasoning_floor_msg_id = "
                     "MAX(reasoning_floor_msg_id, (SELECT COALESCE(MAX(id), 0) FROM messages "
                     "WHERE conversation_id = ?2)) WHERE id = ?2"
                   : "UPDATE conversations SET compaction_summary = ?1 WHERE id = ?2";
      if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
         rc = AUTH_DB_FAILURE;
      } else {
         sqlite3_bind_text(st, 1, now, -1, SQLITE_STATIC);
         sqlite3_bind_int64(st, 2, conv);
         rc = sqlite3_step(st) == SQLITE_DONE ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
         sqlite3_finalize(st);
      }
      if (rc == AUTH_DB_SUCCESS) {
         (*changed)++;
         *boundaries += rendered;
      }
   }
   free(now);
   return rc;
}

int auth_db_migrations_v100(sqlite3 *db) {
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db,
                          "SELECT id, compaction_summary, context_watermark_msg_id FROM "
                          "conversations WHERE compaction_summary IS NOT NULL AND "
                          "compaction_summary != ''",
                          -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v100 read of compaction summaries failed: %s", sqlite3_errmsg(db));
      return AUTH_DB_FAILURE;
   }
   /* Read first, written after: no statement reads the table it updates. */
   typedef struct {
      int64_t id;
      char *summary;
      int64_t watermark;
   } row_t;
   row_t *rows = NULL;
   size_t n = 0;
   size_t cap = 0;
   int rc = AUTH_DB_SUCCESS;
   while (rc == AUTH_DB_SUCCESS && sqlite3_step(st) == SQLITE_ROW) {
      if (n == cap) {
         cap = cap ? cap * 2 : 64;
         row_t *grown = realloc(rows, cap * sizeof(*rows));
         if (!grown) {
            rc = AUTH_DB_FAILURE;
            break;
         }
         rows = grown;
      }
      const char *text = (const char *)sqlite3_column_text(st, 1);
      rows[n].id = sqlite3_column_int64(st, 0);
      rows[n].summary = strdup(text ? text : "");
      rows[n].watermark = sqlite3_column_int64(st, 2);
      rc = rows[n].summary ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
      n += rows[n].summary != NULL;
   }
   sqlite3_finalize(st);
   int changed = 0;
   int boundaries = 0;
   char *errmsg = NULL;
   if (rc == AUTH_DB_SUCCESS &&
       sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v100 begin failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      rc = AUTH_DB_FAILURE;
   } else if (rc == AUTH_DB_SUCCESS) {
      for (size_t i = 0; rc == AUTH_DB_SUCCESS && i < n; i++) {
         rc = restore_one(db, rows[i].id, rows[i].summary, rows[i].watermark, &changed,
                          &boundaries);
      }
      if (rc == AUTH_DB_SUCCESS && sqlite3_exec(db, "COMMIT", NULL, NULL, &errmsg) != SQLITE_OK) {
         OLOG_ERROR("auth_db: v100 commit failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         rc = AUTH_DB_FAILURE;
      }
      if (rc != AUTH_DB_SUCCESS) {
         sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      }
   }
   for (size_t i = 0; i < n; i++) {
      free(rows[i].summary);
   }
   free(rows);
   if (rc != AUTH_DB_SUCCESS) {
      OLOG_ERROR("auth_db: v100 (compaction summaries stored as sent) failed");
      return rc;
   }
   if (changed > 0) {
      OLOG_INFO("auth_db: v100 stored %d compaction summar%s as sent; %d conversation(s) "
                "replay their earlier turns without reasoning (boundary)",
                changed, changed == 1 ? "y" : "ies", boundaries);
   }
   return AUTH_DB_SUCCESS;
}
