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
 * Per-user stocks watchlist store — see schwab_watchlist.h.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include "tools/schwab_watchlist.h"

#include <sqlite3.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "dawn_error.h"
#include "logging.h"

/* Upper-case + validate a ticker into @p out ([A-Z0-9.] only). SUCCESS if non-empty
 * and every character is allowed; FAILURE otherwise (rejects, never truncates). */
static int normalize_symbol(const char *in, char *out, size_t n) {
   if (!in || !in[0]) {
      return FAILURE;
   }
   size_t j = 0;
   for (const char *p = in; *p; p++) {
      if (j >= n - 1) {
         return FAILURE; /* over-length ticker — reject */
      }
      char c = *p;
      if (c >= 'a' && c <= 'z') {
         c = (char)(c - 'a' + 'A');
      }
      if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.')) {
         return FAILURE;
      }
      out[j++] = c;
   }
   out[j] = '\0';
   return j > 0 ? SUCCESS : FAILURE;
}

int schwab_watchlist_normalize(const char *in, char *out, int out_len) {
   return out_len > 0 ? normalize_symbol(in, out, (size_t)out_len) : FAILURE;
}

schwab_watch_add_t schwab_watchlist_add(int user_id, const char *symbol) {
   char sym[SCHWAB_SYMBOL_MAX];
   if (normalize_symbol(symbol, sym, sizeof(sym)) != SUCCESS) {
      return SCHWAB_WATCH_INVALID;
   }

   AUTH_DB_LOCK_OR_RETURN(SCHWAB_WATCH_INVALID); /* lock failure → treat as not-added */
   schwab_watch_add_t result = SCHWAB_WATCH_INVALID;
   bool inserted = false;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(
           s_db.db,
           "INSERT OR IGNORE INTO stocks_watchlist(user_id, symbol, position, added_at) "
           "VALUES(?1, ?2, 0, ?3)",
           -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_text(st, 2, sym, -1, SQLITE_STATIC);
      sqlite3_bind_int64(st, 3, (int64_t)time(NULL));
      if (sqlite3_step(st) == SQLITE_DONE) {
         inserted = (sqlite3_changes(s_db.db) == 1); /* 0 = already present */
         result = inserted ? SCHWAB_WATCH_ADDED : SCHWAB_WATCH_DUPLICATE;
      }
   }
   sqlite3_finalize(st);

   /* Enforce the cap race-safely under the same lock: if THIS call inserted a new
    * row (not an idempotent re-add) and that pushed the list over the limit, undo it. */
   if (inserted) {
      int count = 0;
      sqlite3_stmt *cst = NULL;
      if (sqlite3_prepare_v2(s_db.db, "SELECT COUNT(*) FROM stocks_watchlist WHERE user_id=?1", -1,
                             &cst, NULL) == SQLITE_OK) {
         sqlite3_bind_int(cst, 1, user_id);
         if (sqlite3_step(cst) == SQLITE_ROW) {
            count = sqlite3_column_int(cst, 0);
         }
      }
      sqlite3_finalize(cst);
      if (count > SCHWAB_WATCHLIST_MAX) {
         sqlite3_stmt *dst = NULL;
         if (sqlite3_prepare_v2(s_db.db,
                                "DELETE FROM stocks_watchlist WHERE user_id=?1 AND symbol=?2", -1,
                                &dst, NULL) == SQLITE_OK) {
            sqlite3_bind_int(dst, 1, user_id);
            sqlite3_bind_text(dst, 2, sym, -1, SQLITE_STATIC);
            sqlite3_step(dst);
         }
         sqlite3_finalize(dst);
         result = SCHWAB_WATCH_FULL;
      }
   }
   AUTH_DB_UNLOCK();
   return result;
}

int schwab_watchlist_remove(int user_id, const char *symbol) {
   char sym[SCHWAB_SYMBOL_MAX];
   if (normalize_symbol(symbol, sym, sizeof(sym)) != SUCCESS) {
      return FAILURE;
   }
   AUTH_DB_LOCK_OR_RETURN(FAILURE);
   int result = FAILURE;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, "DELETE FROM stocks_watchlist WHERE user_id=?1 AND symbol=?2",
                          -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_text(st, 2, sym, -1, SQLITE_STATIC);
      result = (sqlite3_step(st) == SQLITE_DONE) ? SUCCESS : FAILURE;
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return result;
}

/* Weak default — the WebUI layer (webui_stocks.c) overrides this. When no WebUI is
 * compiled in, a watchlist edit has no live panel to invalidate. */
__attribute__((weak)) void webui_stocks_watchlist_invalidate(int user_id) {
   (void)user_id;
}

int schwab_watchlist_list(int user_id, char (*out)[SCHWAB_SYMBOL_MAX], int max, int *n_out) {
   if (n_out) {
      *n_out = 0;
   }
   if (!out || max <= 0) {
      return FAILURE;
   }
   AUTH_DB_LOCK_OR_RETURN(FAILURE);
   int n = 0;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(
           s_db.db,
           "SELECT symbol FROM stocks_watchlist WHERE user_id=?1 ORDER BY added_at, symbol", -1,
           &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      while (n < max && sqlite3_step(st) == SQLITE_ROW) {
         const unsigned char *s = sqlite3_column_text(st, 0);
         snprintf(out[n], SCHWAB_SYMBOL_MAX, "%s", s ? (const char *)s : "");
         n++;
      }
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   if (n_out) {
      *n_out = n;
   }
   return SUCCESS;
}
