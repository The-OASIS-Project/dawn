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
 * Stable memory citation handles per conversation (conversation_focus_handles).
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include "auth/auth_db_focus_handles.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db_internal.h"
#include "logging.h"

static bool name_ok(const char *s, size_t max) {
   return s && s[0] && strlen(s) <= max;
}

int conv_db_focus_handles_assign(int64_t conv_id,
                                 int user_id,
                                 conv_focus_handle_t *items,
                                 int count) {
   if (!items || count < 0) {
      return AUTH_DB_INVALID;
   }
   for (int i = 0; i < count; i++) {
      items[i].handle = 0;
      items[i].is_new = false;
   }
   if (conv_id <= 0) {
      return AUTH_DB_NOT_FOUND;
   }
   for (int i = 0; i < count; i++) {
      if (!name_ok(items[i].source, CONV_FOCUS_SOURCE_MAX) ||
          !name_ok(items[i].item_id, CONV_FOCUS_ITEM_ID_MAX)) {
         return AUTH_DB_INVALID;
      }
   }
   if (count == 0) {
      return AUTH_DB_SUCCESS;
   }

   AUTH_DB_LOCK_OR_FAIL();
   int result = conv_db_owned_locked(conv_id, user_id);
   if (result != AUTH_DB_SUCCESS) {
      AUTH_DB_UNLOCK();
      return result;
   }
   if (auth_db_txn_begin_locked("focus_handles") != AUTH_DB_SUCCESS) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }

   sqlite3_stmt *find = NULL;
   sqlite3_stmt *add = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT handle FROM conversation_focus_handles "
                          "WHERE conversation_id = ? AND source = ? AND item_id = ?",
                          -1, &find, NULL) != SQLITE_OK ||
       sqlite3_prepare_v2(s_db.db,
                          "INSERT INTO conversation_focus_handles "
                          "(conversation_id, handle, source, item_id) "
                          "SELECT ?1, COALESCE(MAX(handle), 0) + 1, ?2, ?3 "
                          "FROM conversation_focus_handles WHERE conversation_id = ?1 "
                          "RETURNING handle",
                          -1, &add, NULL) != SQLITE_OK) {
      OLOG_ERROR("focus_handles: prepare failed: %s", sqlite3_errmsg(s_db.db));
      result = AUTH_DB_FAILURE;
   }

   for (int i = 0; result == AUTH_DB_SUCCESS && i < count; i++) {
      sqlite3_reset(find);
      sqlite3_bind_int64(find, 1, conv_id);
      sqlite3_bind_text(find, 2, items[i].source, -1, SQLITE_STATIC);
      sqlite3_bind_text(find, 3, items[i].item_id, -1, SQLITE_STATIC);
      int rc = sqlite3_step(find);
      if (rc == SQLITE_ROW) {
         items[i].handle = sqlite3_column_int(find, 0);
         continue;
      }
      if (rc != SQLITE_DONE) {
         result = AUTH_DB_FAILURE;
         break;
      }
      sqlite3_reset(add);
      sqlite3_bind_int64(add, 1, conv_id);
      sqlite3_bind_text(add, 2, items[i].source, -1, SQLITE_STATIC);
      sqlite3_bind_text(add, 3, items[i].item_id, -1, SQLITE_STATIC);
      if (sqlite3_step(add) == SQLITE_ROW) {
         items[i].handle = sqlite3_column_int(add, 0);
         items[i].is_new = true;
         /* Finish the statement so the insert completes before the next read. */
         while (sqlite3_step(add) == SQLITE_ROW) {
         }
      } else {
         result = AUTH_DB_FAILURE;
      }
   }
   if (result == AUTH_DB_FAILURE) {
      OLOG_ERROR("focus_handles: assign failed: %s", sqlite3_errmsg(s_db.db));
   }
   sqlite3_finalize(find);
   sqlite3_finalize(add);

   result = auth_db_txn_end_locked(result, "focus_handles");
   AUTH_DB_UNLOCK();
   if (result != AUTH_DB_SUCCESS) {
      for (int i = 0; i < count; i++) {
         items[i].handle = 0;
         items[i].is_new = false;
      }
   }
   return result;
}

int conv_db_focus_handles_put(int64_t conv_id, int user_id, conv_focus_handle_t *items, int count) {
   if (!items || count < 0) {
      return AUTH_DB_INVALID;
   }
   for (int i = 0; i < count; i++) {
      items[i].is_new = false;
   }
   if (conv_id <= 0) {
      return AUTH_DB_NOT_FOUND;
   }
   for (int i = 0; i < count; i++) {
      if (!name_ok(items[i].source, CONV_FOCUS_SOURCE_MAX) ||
          !name_ok(items[i].item_id, CONV_FOCUS_ITEM_ID_MAX) || items[i].handle <= 0) {
         return AUTH_DB_INVALID;
      }
   }
   if (count == 0) {
      return AUTH_DB_SUCCESS;
   }

   AUTH_DB_LOCK_OR_FAIL();
   int result = conv_db_owned_locked(conv_id, user_id);
   if (result != AUTH_DB_SUCCESS) {
      AUTH_DB_UNLOCK();
      return result;
   }
   if (auth_db_txn_begin_locked("focus_handles") != AUTH_DB_SUCCESS) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "INSERT OR IGNORE INTO conversation_focus_handles "
                          "(conversation_id, handle, source, item_id) VALUES (?, ?, ?, ?)",
                          -1, &st, NULL) != SQLITE_OK) {
      result = AUTH_DB_FAILURE;
   }
   for (int i = 0; result == AUTH_DB_SUCCESS && i < count; i++) {
      sqlite3_reset(st);
      sqlite3_bind_int64(st, 1, conv_id);
      sqlite3_bind_int(st, 2, items[i].handle);
      sqlite3_bind_text(st, 3, items[i].source, -1, SQLITE_STATIC);
      sqlite3_bind_text(st, 4, items[i].item_id, -1, SQLITE_STATIC);
      if (sqlite3_step(st) != SQLITE_DONE) {
         result = AUTH_DB_FAILURE;
         break;
      }
      items[i].is_new = sqlite3_changes(s_db.db) > 0;
   }
   if (result == AUTH_DB_FAILURE) {
      OLOG_ERROR("focus_handles: put failed: %s", sqlite3_errmsg(s_db.db));
   }
   sqlite3_finalize(st);
   result = auth_db_txn_end_locked(result, "focus_handles");
   AUTH_DB_UNLOCK();
   if (result != AUTH_DB_SUCCESS) {
      for (int i = 0; i < count; i++) {
         items[i].is_new = false;
      }
   }
   return result;
}

typedef struct {
   char source[CONV_FOCUS_SOURCE_MAX + 1];
   char item_id[CONV_FOCUS_ITEM_ID_MAX + 1];
   int handle;
} handle_row_t;

int conv_db_focus_handles_load(int64_t conv_id, int user_id, conv_focus_handle_cb_t cb, void *ctx) {
   if (!cb) {
      return AUTH_DB_FAILURE;
   }
   if (conv_id <= 0) {
      return AUTH_DB_NOT_FOUND;
   }

   /* Rows are copied out and the callback runs without the lock, so it may use
    * the database itself. */
   AUTH_DB_LOCK_OR_FAIL();
   int result = conv_db_owned_locked(conv_id, user_id);
   handle_row_t *rows = NULL;
   int n = 0;
   int cap = 0;
   sqlite3_stmt *st = NULL;
   if (result == AUTH_DB_SUCCESS &&
       sqlite3_prepare_v2(s_db.db,
                          "SELECT source, item_id, handle FROM conversation_focus_handles "
                          "WHERE conversation_id = ? ORDER BY handle",
                          -1, &st, NULL) != SQLITE_OK) {
      result = AUTH_DB_FAILURE;
   }
   if (result == AUTH_DB_SUCCESS) {
      sqlite3_bind_int64(st, 1, conv_id);
      int rc;
      while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
         if (n == cap) {
            int next = cap ? cap * 2 : 16;
            handle_row_t *grown = realloc(rows, (size_t)next * sizeof(*rows));
            if (!grown) {
               result = AUTH_DB_FAILURE;
               break;
            }
            rows = grown;
            cap = next;
         }
         const char *source = (const char *)sqlite3_column_text(st, 0);
         const char *item_id = (const char *)sqlite3_column_text(st, 1);
         snprintf(rows[n].source, sizeof(rows[n].source), "%s", source ? source : "");
         snprintf(rows[n].item_id, sizeof(rows[n].item_id), "%s", item_id ? item_id : "");
         rows[n].handle = sqlite3_column_int(st, 2);
         n++;
      }
      if (result == AUTH_DB_SUCCESS && rc != SQLITE_DONE) {
         result = AUTH_DB_FAILURE;
      }
   }
   if (result == AUTH_DB_FAILURE) {
      OLOG_ERROR("focus_handles: load failed: %s", sqlite3_errmsg(s_db.db));
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();

   for (int i = 0; result == AUTH_DB_SUCCESS && i < n; i++) {
      if (cb(rows[i].source, rows[i].item_id, rows[i].handle, ctx) != 0) {
         break;
      }
   }
   free(rows);
   rows = NULL;
   return result;
}
