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
 * code_projects CRUD + visibility. Ad-hoc prepared statements under s_db.mutex,
 * consistent with auth_db_mcp.c.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include "tools/code_project_db.h"

#include <pthread.h>
#include <sqlite3.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

#define CP_COLS                                                         \
   "id,name,source_url,local_path,user_id,is_global,status,status_msg," \
   "created_at,updated_at,indexed_at,imported_by"

static void copy_col_text(char *dst, size_t dst_sz, sqlite3_stmt *st, int col) {
   const unsigned char *s = sqlite3_column_text(st, col);
   snprintf(dst, dst_sz, "%s", s != NULL ? (const char *)s : "");
}

/* Populate @p out from a row selecting CP_COLS in order. */
static void read_row(sqlite3_stmt *st, code_project_t *out) {
   memset(out, 0, sizeof(*out));
   out->id = sqlite3_column_int64(st, 0);
   copy_col_text(out->name, sizeof(out->name), st, 1);
   copy_col_text(out->source_url, sizeof(out->source_url), st, 2);
   copy_col_text(out->local_path, sizeof(out->local_path), st, 3);
   out->user_id = sqlite3_column_int64(st, 4);
   out->is_global = sqlite3_column_int(st, 5) != 0;
   copy_col_text(out->status, sizeof(out->status), st, 6);
   copy_col_text(out->status_msg, sizeof(out->status_msg), st, 7);
   out->created_at = sqlite3_column_int64(st, 8);
   out->updated_at = sqlite3_column_int64(st, 9);
   out->indexed_at = sqlite3_column_int64(st, 10);
   out->imported_by = sqlite3_column_int64(st, 11);
}

int code_project_db_create(const code_project_t *p, int64_t *id_out) {
   if (p == NULL) {
      return AUTH_DB_FAILURE;
   }
   const char *sql =
       "INSERT INTO code_projects (name, source_url, local_path, user_id, is_global, status, "
       "status_msg, created_at, updated_at, imported_by) VALUES (?,?,?,?,?,?,?,?,?,?)";
   time_t now = time(NULL);

   pthread_mutex_lock(&s_db.mutex);
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("code_project_db: create prepare failed: %s", sqlite3_errmsg(s_db.db));
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_text(st, 1, p->name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, p->source_url, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 3, p->local_path, -1, SQLITE_TRANSIENT);
   if (p->user_id > 0) {
      sqlite3_bind_int64(st, 4, p->user_id);
   } else {
      sqlite3_bind_null(st, 4);
   }
   sqlite3_bind_int(st, 5, p->is_global ? 1 : 0);
   sqlite3_bind_text(st, 6, p->status[0] ? p->status : "pending", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 7, p->status_msg, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(st, 8, (sqlite3_int64)now);
   sqlite3_bind_int64(st, 9, (sqlite3_int64)now);
   if (p->imported_by > 0) {
      sqlite3_bind_int64(st, 10, p->imported_by);
   } else {
      sqlite3_bind_null(st, 10);
   }

   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }
   if (id_out != NULL) {
      *id_out = sqlite3_last_insert_rowid(s_db.db);
   }
   pthread_mutex_unlock(&s_db.mutex);
   return AUTH_DB_SUCCESS;
}

int code_project_db_update_status(int64_t id, const char *status, const char *msg) {
   if (status == NULL) {
      return AUTH_DB_FAILURE;
   }
   const char *sql = "UPDATE code_projects SET status=?, status_msg=?, updated_at=? WHERE id=?";
   pthread_mutex_lock(&s_db.mutex);
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL) != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_text(st, 1, status, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, msg != NULL ? msg : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(st, 3, (sqlite3_int64)time(NULL));
   sqlite3_bind_int64(st, 4, id);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   pthread_mutex_unlock(&s_db.mutex);
   return rc == SQLITE_DONE ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int code_project_db_set_indexed_at(int64_t id, time_t when) {
   const char *sql = "UPDATE code_projects SET indexed_at=?, updated_at=? WHERE id=?";
   pthread_mutex_lock(&s_db.mutex);
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL) != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, (sqlite3_int64)when);
   sqlite3_bind_int64(st, 2, (sqlite3_int64)time(NULL));
   sqlite3_bind_int64(st, 3, id);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   pthread_mutex_unlock(&s_db.mutex);
   return rc == SQLITE_DONE ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

static int get_by(const char *where_col,
                  int64_t id_val,
                  const char *name_val,
                  code_project_t *out) {
   if (out == NULL) {
      return AUTH_DB_FAILURE;
   }
   char sql[256];
   snprintf(sql, sizeof(sql), "SELECT " CP_COLS " FROM code_projects WHERE %s=? LIMIT 1",
            where_col);
   pthread_mutex_lock(&s_db.mutex);
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL) != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }
   if (name_val != NULL) {
      sqlite3_bind_text(st, 1, name_val, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_int64(st, 1, id_val);
   }
   int rc = sqlite3_step(st);
   int result;
   if (rc == SQLITE_ROW) {
      read_row(st, out);
      result = AUTH_DB_SUCCESS;
   } else if (rc == SQLITE_DONE) {
      result = AUTH_DB_NOT_FOUND;
   } else {
      result = AUTH_DB_FAILURE;
   }
   sqlite3_finalize(st);
   pthread_mutex_unlock(&s_db.mutex);
   return result;
}

int code_project_db_get(int64_t id, code_project_t *out) {
   return get_by("id", id, NULL, out);
}

int code_project_db_get_by_name(const char *name, code_project_t *out) {
   if (name == NULL) {
      return AUTH_DB_FAILURE;
   }
   return get_by("name", 0, name, out);
}

int code_project_db_list_visible(int64_t user_id, code_project_t *out, int max, int *count_out) {
   if (out == NULL || count_out == NULL || max <= 0) {
      return AUTH_DB_FAILURE;
   }
   *count_out = 0;
   const char *sql = "SELECT " CP_COLS
                     " FROM code_projects WHERE is_global=1 OR user_id=? ORDER BY name LIMIT ?";
   pthread_mutex_lock(&s_db.mutex);
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL) != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, user_id);
   sqlite3_bind_int(st, 2, max);
   int n = 0;
   while (n < max && sqlite3_step(st) == SQLITE_ROW) {
      read_row(st, &out[n]);
      n++;
   }
   sqlite3_finalize(st);
   pthread_mutex_unlock(&s_db.mutex);
   *count_out = n;
   return AUTH_DB_SUCCESS;
}

int code_project_db_list_all(code_project_t *out, int max, int *count_out) {
   if (out == NULL || count_out == NULL || max <= 0) {
      return AUTH_DB_FAILURE;
   }
   *count_out = 0;
   pthread_mutex_lock(&s_db.mutex);
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, "SELECT " CP_COLS " FROM code_projects ORDER BY name LIMIT ?",
                          -1, &st, NULL) != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int(st, 1, max);
   int n = 0;
   while (n < max && sqlite3_step(st) == SQLITE_ROW) {
      read_row(st, &out[n]);
      n++;
   }
   sqlite3_finalize(st);
   pthread_mutex_unlock(&s_db.mutex);
   *count_out = n;
   return AUTH_DB_SUCCESS;
}

int code_project_db_check_visible(int64_t user_id, const char *name, bool *out) {
   if (name == NULL || out == NULL) {
      return AUTH_DB_FAILURE;
   }
   *out = false;
   const char *sql =
       "SELECT 1 FROM code_projects WHERE name=? AND (is_global=1 OR user_id=?) LIMIT 1";
   pthread_mutex_lock(&s_db.mutex);
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL) != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(st, 2, user_id);
   int rc = sqlite3_step(st);
   int result = AUTH_DB_SUCCESS;
   if (rc == SQLITE_ROW) {
      *out = true;
   } else if (rc != SQLITE_DONE) {
      result = AUTH_DB_FAILURE;
   }
   sqlite3_finalize(st);
   pthread_mutex_unlock(&s_db.mutex);
   return result;
}

int code_project_db_set_global(int64_t id, bool is_global) {
   const char *sql = "UPDATE code_projects SET is_global=?, updated_at=? WHERE id=?";
   pthread_mutex_lock(&s_db.mutex);
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL) != SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int(st, 1, is_global ? 1 : 0);
   sqlite3_bind_int64(st, 2, (sqlite3_int64)time(NULL));
   sqlite3_bind_int64(st, 3, id);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   pthread_mutex_unlock(&s_db.mutex);
   return rc == SQLITE_DONE ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int code_project_db_delete(int64_t id) {
   pthread_mutex_lock(&s_db.mutex);
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, "DELETE FROM code_projects WHERE id=?", -1, &st, NULL) !=
       SQLITE_OK) {
      pthread_mutex_unlock(&s_db.mutex);
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, id);
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   pthread_mutex_unlock(&s_db.mutex);
   return rc == SQLITE_DONE ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}
