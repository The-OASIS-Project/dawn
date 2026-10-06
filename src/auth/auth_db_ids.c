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
 * Small shared helpers: row-id lists for batch lookups, transactions on the
 * shared handle, one-number queries, conversation ownership.  See
 * auth_db_internal.h.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

bool auth_db_internal_ids_json_into(const int64_t *ids, int n, char *out, size_t size) {
   if (!out || size < 3 || n < 0 || (n > 0 && !ids)) {
      return false;
   }
   size_t len = 0;
   out[len++] = '[';
   for (int i = 0; i < n; i++) {
      const int w = snprintf(out + len, size - len, "%s%lld", i ? "," : "", (long long)ids[i]);
      if (w < 0 || (size_t)w >= size - len) {
         return false;
      }
      len += (size_t)w;
   }
   if (len + 2 > size) {
      return false;
   }
   out[len++] = ']';
   out[len] = '\0';
   return true;
}

char *auth_db_internal_ids_json(const int64_t *ids, int n) {
   if (!ids || n <= 0) {
      return NULL;
   }
   const size_t size = AUTH_DB_IDS_JSON_SIZE(n);
   char *list = malloc(size);
   if (list && !auth_db_internal_ids_json_into(ids, n, list, size)) {
      free(list);
      list = NULL;
   }
   return list;
}

int auth_db_txn_begin_locked(const char *tag) {
   if (sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
      OLOG_ERROR("%s: BEGIN failed: %s", tag, sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}

int auth_db_txn_end_locked(int result, const char *tag) {
   if (result == AUTH_DB_SUCCESS) {
      if (sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK) {
         return AUTH_DB_SUCCESS;
      }
      OLOG_ERROR("%s: COMMIT failed: %s", tag, sqlite3_errmsg(s_db.db));
      result = AUTH_DB_FAILURE;
   }
   sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
   return result;
}

int64_t auth_db_query_int64(sqlite3 *db, const char *sql) {
   sqlite3_stmt *st = NULL;
   int64_t v = -1;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
      v = sqlite3_column_int64(st, 0);
   }
   sqlite3_finalize(st);
   return v;
}

/* Whether @p conv_id belongs to @p user_id: SUCCESS, NOT_FOUND, or FAILURE.
 * Caller holds the lock. */
int conv_db_owned_locked(int64_t conv_id, int user_id) {
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, "SELECT 1 FROM conversations WHERE id = ? AND user_id = ?", -1,
                          &st, NULL) != SQLITE_OK) {
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, conv_id);
   sqlite3_bind_int(st, 2, user_id);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc == SQLITE_ROW) {
      return AUTH_DB_SUCCESS;
   }
   return rc == SQLITE_DONE ? AUTH_DB_NOT_FOUND : AUTH_DB_FAILURE;
}
