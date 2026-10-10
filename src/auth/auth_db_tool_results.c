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
 * Stored tool results (auth_db_tool_results.h).
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include "auth/auth_db_tool_results.h"

#include <ctype.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

bool tool_results_id_valid(const char *id) {
   if (!id || strlen(id) != TOOL_RESULTS_ID_LEN - 1 ||
       strncmp(id, TOOL_RESULTS_ID_PREFIX, strlen(TOOL_RESULTS_ID_PREFIX)) != 0) {
      return false;
   }
   for (size_t i = strlen(TOOL_RESULTS_ID_PREFIX); i < TOOL_RESULTS_ID_LEN - 1; i++) {
      if (!isalnum((unsigned char)id[i])) {
         return false;
      }
   }
   return true;
}

/* What a cap is kept over: one conversation, one user, or every result. */
typedef enum {
   SCOPE_CONVERSATION,
   SCOPE_USER,
   SCOPE_ALL,
} scope_t;

static const char *const k_scope_name[] = { "conversation", "user", "total" };

/* The statements a put and a read run every time, prepared on first use and
 * kept (tool_results_db_release_locked finalizes them with the database);
 * each use ends with stmt_done(), so none is left holding a read or a bound
 * pointer. */
typedef enum {
   STMT_USAGE,
   STMT_RECLAIM,
   STMT_INSERT,
   STMT_GET_META,
   STMT_GET_BODY,
   STMT_COUNT,
} stmt_id_t;

static const char *const k_stmt_sql[STMT_COUNT] = {
   [STMT_USAGE] = "SELECT bytes FROM tool_results_usage WHERE scope = ?1 AND key = ?2",
   [STMT_RECLAIM] = "DELETE FROM tool_results WHERE conversation_id IS NULL AND created_at < ?",
   [STMT_INSERT] =
       "INSERT INTO tool_results (id, user_id, conversation_id, session_key, tool_name, "
       "tool_call_id, content_kind, chars, bytes, body, created_at) VALUES (?, ?, ?, "
       "?, ?, ?, ?, ?, ?, ?, ?)",
   [STMT_GET_META] = "SELECT user_id, conversation_id, content_kind, chars, bytes, tool_name FROM "
                     "tool_results WHERE id = ?",
   [STMT_GET_BODY] = "SELECT user_id, conversation_id, content_kind, chars, bytes, tool_name, body "
                     "FROM tool_results WHERE id = ?",
};

static sqlite3_stmt *s_stmts[STMT_COUNT];

/* Statement @p id, prepared if it isn't yet; NULL on failure.  Caller holds
 * the lock. */
static sqlite3_stmt *stmt_locked(stmt_id_t id) {
   if (!s_stmts[id] &&
       sqlite3_prepare_v2(s_db.db, k_stmt_sql[id], -1, &s_stmts[id], NULL) != SQLITE_OK) {
      OLOG_ERROR("tool_results: preparing statement %d failed: %s", (int)id,
                 sqlite3_errmsg(s_db.db));
      s_stmts[id] = NULL;
   }
   return s_stmts[id];
}

static void stmt_done(sqlite3_stmt *st) {
   if (st) {
      sqlite3_reset(st);
      sqlite3_clear_bindings(st);
   }
}

void tool_results_db_release_locked(void) {
   for (int i = 0; i < STMT_COUNT; i++) {
      sqlite3_finalize(s_stmts[i]);
      s_stmts[i] = NULL;
   }
}

/* The oldest results of a scope first, from its age-ordered index: a user's
 * unbound results (no conversation holds them yet) go before the rest. */
static const char *const k_scope_victims[][2] = {
   { "SELECT rowid, bytes FROM tool_results WHERE conversation_id = ?1 ORDER BY created_at", NULL },
   { "SELECT rowid, bytes FROM tool_results WHERE conversation_id IS NULL AND user_id = ?1 "
     "ORDER BY created_at",
     "SELECT rowid, bytes FROM tool_results WHERE user_id = ?1 ORDER BY created_at" },
   /* The total: the heaviest user's first (one user's results don't push out
    * everyone else's), then anyone's oldest. */
   { "SELECT rowid, bytes FROM tool_results WHERE user_id = (SELECT key FROM tool_results_usage "
     "WHERE scope = 1 ORDER BY bytes DESC LIMIT 1) ORDER BY created_at",
     "SELECT rowid, bytes FROM tool_results ORDER BY rowid" },
};

/* The bytes @p scope (keyed by @p key) holds (tool_results_usage). */
static int usage_locked(scope_t scope, int64_t key, int64_t *bytes) {
   *bytes = 0;
   sqlite3_stmt *st = stmt_locked(STMT_USAGE);
   if (!st) {
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int(st, 1, (int)scope);
   sqlite3_bind_int64(st, 2, scope == SCOPE_ALL ? 0 : key);
   const int rc = sqlite3_step(st);
   if (rc == SQLITE_ROW) {
      *bytes = sqlite3_column_int64(st, 0);
   }
   stmt_done(st);
   return rc == SQLITE_ROW || rc == SQLITE_DONE ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

/* Delete the oldest rows @p sql lists (keyed by @p key) until @p *excess
 * bytes are freed; counts into @p count / @p freed.  One pass: the rows are
 * collected from the index, then deleted. */
static int evict_pass_locked(const char *sql,
                             int64_t key,
                             bool keyed,
                             int64_t *excess,
                             int *count,
                             int64_t *freed) {
   sqlite3_stmt *sel = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &sel, NULL) != SQLITE_OK) {
      return AUTH_DB_FAILURE;
   }
   if (keyed) {
      sqlite3_bind_int64(sel, 1, key);
   }
   int64_t *rows = NULL;
   int n = 0;
   int cap = 0;
   int rc = SQLITE_ROW;
   while (*excess > 0 && (rc = sqlite3_step(sel)) == SQLITE_ROW) {
      if (n == cap) {
         cap = cap ? cap * 2 : 16;
         int64_t *grown = realloc(rows, (size_t)cap * sizeof(*rows));
         if (!grown) {
            rc = SQLITE_NOMEM;
            break;
         }
         rows = grown;
      }
      rows[n++] = sqlite3_column_int64(sel, 0);
      const int64_t bytes = sqlite3_column_int64(sel, 1);
      *excess -= bytes;
      *freed += bytes;
   }
   sqlite3_finalize(sel);
   int result = rc == SQLITE_ROW || rc == SQLITE_DONE ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
   sqlite3_stmt *del = NULL;
   if (result == AUTH_DB_SUCCESS && n > 0 &&
       sqlite3_prepare_v2(s_db.db, "DELETE FROM tool_results WHERE rowid = ?", -1, &del, NULL) !=
           SQLITE_OK) {
      result = AUTH_DB_FAILURE;
   }
   for (int i = 0; result == AUTH_DB_SUCCESS && i < n; i++) {
      sqlite3_reset(del);
      sqlite3_bind_int64(del, 1, rows[i]);
      if (sqlite3_step(del) != SQLITE_DONE) {
         result = AUTH_DB_FAILURE;
      }
   }
   sqlite3_finalize(del);
   free(rows);
   *count += n;
   return result;
}

/* Evict the oldest results in @p scope (keyed by @p key) until @p need more
 * bytes fit under @p cap.  Caller holds the lock, in a transaction. */
static int evict_locked(scope_t scope, int64_t key, int64_t cap, int64_t need, int *evicted) {
   int64_t total = 0;
   if (usage_locked(scope, key, &total) != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }
   int64_t excess = total + need - cap;
   if (excess <= 0) {
      return AUTH_DB_SUCCESS;
   }
   int count = 0;
   int64_t freed = 0;
   int result = AUTH_DB_SUCCESS;
   for (int pass = 0; pass < 2 && excess > 0 && result == AUTH_DB_SUCCESS; pass++) {
      const char *sql = k_scope_victims[scope][pass];
      if (sql) {
         result = evict_pass_locked(sql, key, scope != SCOPE_ALL, &excess, &count, &freed);
      }
   }
   if (count > 0) {
      OLOG_INFO("tool_results: evicted %d result(s), %lld bytes, over the %s cap (key %lld)", count,
                (long long)freed, k_scope_name[scope], (long long)key);
   }
   if (evicted) {
      *evicted += count;
   }
   return result;
}

static int reclaim_unbound_locked(int64_t before, int *deleted_out) {
   sqlite3_stmt *st = stmt_locked(STMT_RECLAIM);
   if (!st) {
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, before);
   const int rc = sqlite3_step(st);
   stmt_done(st);
   if (rc != SQLITE_DONE) {
      return AUTH_DB_FAILURE;
   }
   const int n = sqlite3_changes(s_db.db);
   if (n > 0) {
      OLOG_INFO("tool_results: reclaimed %d result(s) no conversation kept", n);
   }
   if (deleted_out) {
      *deleted_out = n;
   }
   return AUTH_DB_SUCCESS;
}

static int end_transaction_locked(int result) {
   /* A refused request isn't a database failure. */
   if (result != AUTH_DB_SUCCESS && result != AUTH_DB_INVALID) {
      OLOG_ERROR("tool_results: transaction failed: %s", sqlite3_errmsg(s_db.db));
   }
   return auth_db_txn_end_locked(result, "tool_results");
}

int tool_results_db_add(const tool_results_new_t *r, int *evicted_out) {
   if (evicted_out) {
      *evicted_out = 0;
   }
   if (!r || !tool_results_id_valid(r->id) || r->user_id <= 0 || r->conversation_id < 0 ||
       !r->tool_name || !r->tool_name[0] || strlen(r->tool_name) >= TOOL_RESULTS_TOOL_NAME_MAX ||
       !r->body || (r->kind != TOOL_RESULTS_TEXT && r->kind != TOOL_RESULTS_JSON) ||
       (int64_t)r->bytes > TOOL_RESULTS_CONV_MAX_BYTES) {
      return AUTH_DB_INVALID;
   }
   const int64_t need = (int64_t)r->bytes;
   const int64_t now = (int64_t)time(NULL);

   AUTH_DB_LOCK_OR_FAIL();
   if (auth_db_txn_begin_locked("tool_results") != AUTH_DB_SUCCESS) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   int result = r->conversation_id > 0 ? conv_db_owned_locked(r->conversation_id, r->user_id)
                                       : AUTH_DB_SUCCESS;
   if (result == AUTH_DB_NOT_FOUND) {
      result = AUTH_DB_INVALID; /* not the user's conversation */
   }
   if (result == AUTH_DB_SUCCESS) {
      result = reclaim_unbound_locked(now - TOOL_RESULTS_UNBOUND_GRACE_SEC, NULL);
   }
   if (result == AUTH_DB_SUCCESS && r->conversation_id > 0) {
      result = evict_locked(SCOPE_CONVERSATION, r->conversation_id, TOOL_RESULTS_CONV_MAX_BYTES,
                            need, evicted_out);
   }
   if (result == AUTH_DB_SUCCESS) {
      result = evict_locked(SCOPE_USER, r->user_id, TOOL_RESULTS_USER_MAX_BYTES, need, evicted_out);
   }
   if (result == AUTH_DB_SUCCESS) {
      result = evict_locked(SCOPE_ALL, 0, TOOL_RESULTS_TOTAL_MAX_BYTES, need, evicted_out);
   }
   if (result == AUTH_DB_SUCCESS) {
      sqlite3_stmt *st = stmt_locked(STMT_INSERT);
      if (!st) {
         result = AUTH_DB_FAILURE;
      } else {
         sqlite3_bind_text(st, 1, r->id, -1, SQLITE_STATIC);
         sqlite3_bind_int(st, 2, r->user_id);
         if (r->conversation_id > 0) {
            sqlite3_bind_int64(st, 3, r->conversation_id);
         } else {
            sqlite3_bind_null(st, 3);
         }
         if (r->session_key && r->conversation_id <= 0) {
            sqlite3_bind_text(st, 4, r->session_key, -1, SQLITE_STATIC);
         } else {
            sqlite3_bind_null(st, 4);
         }
         sqlite3_bind_text(st, 5, r->tool_name, -1, SQLITE_STATIC);
         if (r->tool_call_id) {
            sqlite3_bind_text(st, 6, r->tool_call_id, -1, SQLITE_STATIC);
         } else {
            sqlite3_bind_null(st, 6);
         }
         const int frame = r->frame >= 0 && r->frame <= TOOL_RESULTS_FRAME_MAX ? r->frame : 0;
         sqlite3_bind_int(st, 7, (int)r->kind | (frame << 4));
         sqlite3_bind_int64(st, 8, r->chars);
         sqlite3_bind_int64(st, 9, need);
         sqlite3_bind_blob64(st, 10, r->body, (sqlite3_uint64)r->bytes, SQLITE_STATIC);
         sqlite3_bind_int64(st, 11, now);
         result = sqlite3_step(st) == SQLITE_DONE ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
         stmt_done(st);
      }
   }
   result = end_transaction_locked(result);
   AUTH_DB_UNLOCK();
   return result;
}

int tool_results_db_get(const char *id, tool_results_meta_t *meta, char **body, size_t *bytes) {
   if (body) {
      *body = NULL;
   }
   if (bytes) {
      *bytes = 0;
   }
   if (!tool_results_id_valid(id) || !meta) {
      return AUTH_DB_INVALID;
   }
   memset(meta, 0, sizeof(*meta));

   AUTH_DB_LOCK_OR_FAIL();
   int result = AUTH_DB_FAILURE;
   sqlite3_stmt *st = stmt_locked(body ? STMT_GET_BODY : STMT_GET_META);
   if (!st) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
   const int rc = sqlite3_step(st);
   if (rc == SQLITE_ROW) {
      meta->user_id = sqlite3_column_int(st, 0);
      meta->conversation_id = sqlite3_column_int64(st, 1);
      const int kind = sqlite3_column_int(st, 2);
      meta->kind = (kind & 0xF) == TOOL_RESULTS_JSON ? TOOL_RESULTS_JSON : TOOL_RESULTS_TEXT;
      meta->frame = (kind >> 4) & TOOL_RESULTS_FRAME_MAX;
      meta->chars = sqlite3_column_int64(st, 3);
      meta->bytes = sqlite3_column_int64(st, 4);
      const char *name = (const char *)sqlite3_column_text(st, 5);
      snprintf(meta->tool_name, sizeof(meta->tool_name), "%s", name ? name : "");
      result = AUTH_DB_SUCCESS;
      if (body) {
         const void *blob = sqlite3_column_blob(st, 6);
         const size_t n = (size_t)sqlite3_column_bytes(st, 6);
         *body = malloc(n + 1);
         if (!*body) {
            result = AUTH_DB_FAILURE;
         } else {
            if (n > 0) {
               memcpy(*body, blob, n);
            }
            (*body)[n] = '\0';
            if (bytes) {
               *bytes = n;
            }
         }
      }
   } else if (rc == SQLITE_DONE) {
      result = AUTH_DB_NOT_FOUND;
   }
   stmt_done(st);
   AUTH_DB_UNLOCK();
   return result;
}

int tool_results_db_bind(int64_t conv_id, const char (*ids)[TOOL_RESULTS_ID_LEN], int count) {
   if (conv_id <= 0 || count < 0 || (count > 0 && !ids)) {
      return AUTH_DB_INVALID;
   }
   if (count == 0) {
      return AUTH_DB_SUCCESS;
   }
   AUTH_DB_LOCK_OR_FAIL();
   if (auth_db_txn_begin_locked("tool_results") != AUTH_DB_SUCCESS) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   /* Only a row whose user owns the conversation. */
   sqlite3_stmt *st = NULL;
   int result = sqlite3_prepare_v2(
                    s_db.db,
                    "UPDATE tool_results SET conversation_id = ?1, "
                    "session_key = NULL WHERE id = ?2 AND conversation_id IS NULL "
                    "AND user_id = (SELECT user_id FROM conversations WHERE id = ?1)",
                    -1, &st, NULL) == SQLITE_OK
                    ? AUTH_DB_SUCCESS
                    : AUTH_DB_FAILURE;
   for (int i = 0; result == AUTH_DB_SUCCESS && i < count; i++) {
      if (!tool_results_id_valid(ids[i])) {
         continue;
      }
      sqlite3_reset(st);
      sqlite3_bind_int64(st, 1, conv_id);
      sqlite3_bind_text(st, 2, ids[i], -1, SQLITE_STATIC);
      if (sqlite3_step(st) != SQLITE_DONE) {
         result = AUTH_DB_FAILURE;
      }
   }
   sqlite3_finalize(st);
   /* The conversation's cap holds for what it now has. */
   if (result == AUTH_DB_SUCCESS) {
      result = evict_locked(SCOPE_CONVERSATION, conv_id, TOOL_RESULTS_CONV_MAX_BYTES, 0, NULL);
   }
   result = end_transaction_locked(result);
   AUTH_DB_UNLOCK();
   return result;
}

int tool_results_db_delete(const char *id) {
   if (!tool_results_id_valid(id)) {
      return AUTH_DB_INVALID;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int result = AUTH_DB_FAILURE;
   if (sqlite3_prepare_v2(s_db.db, "DELETE FROM tool_results WHERE id = ?", -1, &st, NULL) ==
       SQLITE_OK) {
      sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
      result = sqlite3_step(st) == SQLITE_DONE ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return result;
}

int tool_results_db_reclaim_unbound(int64_t before, int *deleted_out) {
   if (deleted_out) {
      *deleted_out = 0;
   }
   AUTH_DB_LOCK_OR_FAIL();
   const int result = reclaim_unbound_locked(before, deleted_out);
   AUTH_DB_UNLOCK();
   return result;
}
