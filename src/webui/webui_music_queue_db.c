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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Music Queue DB Persistence - SQLite save/load for per-user music queues
 */

#include "webui/webui_music_queue_db.h"

#include <sqlite3.h>
#include <string.h>

#include "core/path_utils.h"
#include "logging.h"
#include "webui/webui_music_internal.h"

static sqlite3 *s_db = NULL;

/* =============================================================================
 * Schema
 * ============================================================================= */

static const char *SCHEMA_SQL = "CREATE TABLE IF NOT EXISTS user_queue ("
                                "   user_id INTEGER NOT NULL,"
                                "   position INTEGER NOT NULL,"
                                "   path TEXT NOT NULL,"
                                "   title TEXT,"
                                "   artist TEXT,"
                                "   album TEXT,"
                                "   duration_sec INTEGER,"
                                "   PRIMARY KEY (user_id, position)"
                                ");"
                                "CREATE TABLE IF NOT EXISTS user_queue_state ("
                                "   user_id INTEGER PRIMARY KEY,"
                                "   shuffle INTEGER DEFAULT 0,"
                                "   repeat_mode INTEGER DEFAULT 0"
                                ");";

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

int music_queue_db_init(const char *db_path) {
   if (s_db) {
      return 0; /* Already initialized */
   }

   /* Expand tilde (e.g. ~/.local/share/dawn/music.db) — the caller passes the
    * raw configured data_dir, which may start with '~'. sqlite3_open() does not
    * expand it, so do it here, matching music_db.c (same MUSIC_DB_PATH_MAX). */
   char expanded[MUSIC_DB_PATH_MAX];
   if (!path_expand_tilde(db_path, expanded, sizeof(expanded))) {
      OLOG_ERROR("Music queue DB: Failed to expand path: %s", db_path);
      return 1;
   }

   int rc = sqlite3_open(expanded, &s_db);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("Music queue DB: Failed to open %s: %s", expanded, sqlite3_errmsg(s_db));
      sqlite3_close(s_db);
      s_db = NULL;
      return 1;
   }

   /* WAL mode for concurrency with music_db scans */
   sqlite3_exec(s_db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
   sqlite3_exec(s_db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
   sqlite3_busy_timeout(s_db, 5000);

   /* Create tables */
   char *err_msg = NULL;
   rc = sqlite3_exec(s_db, SCHEMA_SQL, NULL, NULL, &err_msg);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("Music queue DB: Schema creation failed: %s", err_msg);
      sqlite3_free(err_msg);
      sqlite3_close(s_db);
      s_db = NULL;
      return 1;
   }

   OLOG_INFO("Music queue DB: Initialized at %s", expanded);
   return 0;
}

void music_queue_db_cleanup(void) {
   if (s_db) {
      sqlite3_close(s_db);
      s_db = NULL;
      OLOG_INFO("Music queue DB: Closed");
   }
}

/* =============================================================================
 * Save
 * ============================================================================= */

int music_queue_db_save(int user_id, const user_music_queue_t *uq) {
   if (!s_db || !uq || user_id <= 0) {
      return 0; /* Skip private queues silently */
   }

   /* Begin transaction */
   if (sqlite3_exec(s_db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) {
      OLOG_ERROR("Music queue DB: BEGIN failed: %s", sqlite3_errmsg(s_db));
      return 1;
   }

   /* Delete existing queue entries for this user */
   sqlite3_stmt *del_stmt;
   int rc = sqlite3_prepare_v2(s_db, "DELETE FROM user_queue WHERE user_id = ?", -1, &del_stmt,
                               NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("Music queue DB: Prepare DELETE failed: %s", sqlite3_errmsg(s_db));
      sqlite3_exec(s_db, "ROLLBACK", NULL, NULL, NULL);
      return 1;
   }
   sqlite3_bind_int(del_stmt, 1, user_id);
   sqlite3_step(del_stmt);
   sqlite3_finalize(del_stmt);

   /* Insert all queue entries */
   sqlite3_stmt *ins_stmt;
   rc = sqlite3_prepare_v2(
       s_db,
       "INSERT INTO user_queue (user_id, position, path, title, artist, album, duration_sec) "
       "VALUES (?, ?, ?, ?, ?, ?, ?)",
       -1, &ins_stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("Music queue DB: Prepare INSERT failed: %s", sqlite3_errmsg(s_db));
      sqlite3_exec(s_db, "ROLLBACK", NULL, NULL, NULL);
      return 1;
   }

   for (int i = 0; i < uq->queue_length; i++) {
      const music_queue_entry_t *entry = &uq->queue[i];
      sqlite3_bind_int(ins_stmt, 1, user_id);
      sqlite3_bind_int(ins_stmt, 2, i);
      sqlite3_bind_text(ins_stmt, 3, entry->path, -1, SQLITE_STATIC);
      sqlite3_bind_text(ins_stmt, 4, entry->title, -1, SQLITE_STATIC);
      sqlite3_bind_text(ins_stmt, 5, entry->artist, -1, SQLITE_STATIC);
      sqlite3_bind_text(ins_stmt, 6, entry->album, -1, SQLITE_STATIC);
      sqlite3_bind_int(ins_stmt, 7, (int)entry->duration_sec);

      rc = sqlite3_step(ins_stmt);
      if (rc != SQLITE_DONE) {
         OLOG_ERROR("Music queue DB: INSERT step failed at pos %d: %s", i, sqlite3_errmsg(s_db));
         sqlite3_finalize(ins_stmt);
         sqlite3_exec(s_db, "ROLLBACK", NULL, NULL, NULL);
         return 1;
      }
      sqlite3_reset(ins_stmt);
   }
   sqlite3_finalize(ins_stmt);

   /* Upsert state row */
   sqlite3_stmt *state_stmt;
   rc = sqlite3_prepare_v2(
       s_db,
       "INSERT INTO user_queue_state (user_id, shuffle, repeat_mode) VALUES (?, ?, ?) "
       "ON CONFLICT(user_id) DO UPDATE SET shuffle = excluded.shuffle, "
       "repeat_mode = excluded.repeat_mode",
       -1, &state_stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("Music queue DB: Prepare UPSERT state failed: %s", sqlite3_errmsg(s_db));
      sqlite3_exec(s_db, "ROLLBACK", NULL, NULL, NULL);
      return 1;
   }
   sqlite3_bind_int(state_stmt, 1, user_id);
   sqlite3_bind_int(state_stmt, 2, uq->shuffle ? 1 : 0);
   sqlite3_bind_int(state_stmt, 3, (int)uq->repeat_mode);
   sqlite3_step(state_stmt);
   sqlite3_finalize(state_stmt);

   /* Commit */
   if (sqlite3_exec(s_db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
      OLOG_ERROR("Music queue DB: COMMIT failed: %s", sqlite3_errmsg(s_db));
      sqlite3_exec(s_db, "ROLLBACK", NULL, NULL, NULL);
      return 1;
   }

   return 0;
}

/* =============================================================================
 * Save State Only (shuffle/repeat)
 * ============================================================================= */

int music_queue_db_save_state(int user_id, const user_music_queue_t *uq) {
   if (!s_db || !uq || user_id <= 0) {
      return 0;
   }

   sqlite3_stmt *stmt;
   int rc = sqlite3_prepare_v2(
       s_db,
       "INSERT INTO user_queue_state (user_id, shuffle, repeat_mode) VALUES (?, ?, ?) "
       "ON CONFLICT(user_id) DO UPDATE SET shuffle = excluded.shuffle, "
       "repeat_mode = excluded.repeat_mode",
       -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("Music queue DB: Prepare UPSERT state failed: %s", sqlite3_errmsg(s_db));
      return 1;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, uq->shuffle ? 1 : 0);
   sqlite3_bind_int(stmt, 3, (int)uq->repeat_mode);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);

   return 0;
}

/* =============================================================================
 * Load
 * ============================================================================= */

int music_queue_db_load(int user_id, user_music_queue_t *uq) {
   if (!s_db || !uq || user_id <= 0) {
      return 0; /* Skip private queues silently */
   }

   uq->queue_length = 0;

   /* Load queue entries */
   sqlite3_stmt *sel_stmt;
   int rc = sqlite3_prepare_v2(s_db,
                               "SELECT path, title, artist, album, duration_sec FROM user_queue "
                               "WHERE user_id = ? ORDER BY position",
                               -1, &sel_stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("Music queue DB: Prepare SELECT queue failed: %s", sqlite3_errmsg(s_db));
      return 1;
   }
   sqlite3_bind_int(sel_stmt, 1, user_id);

   while (sqlite3_step(sel_stmt) == SQLITE_ROW && uq->queue_length < WEBUI_MUSIC_MAX_QUEUE) {
      music_queue_entry_t *entry = &uq->queue[uq->queue_length];

      const char *path = (const char *)sqlite3_column_text(sel_stmt, 0);
      const char *title = (const char *)sqlite3_column_text(sel_stmt, 1);
      const char *artist = (const char *)sqlite3_column_text(sel_stmt, 2);
      const char *album = (const char *)sqlite3_column_text(sel_stmt, 3);
      int duration = sqlite3_column_int(sel_stmt, 4);

      snprintf(entry->path, sizeof(entry->path), "%s", path ? path : "");
      snprintf(entry->title, sizeof(entry->title), "%s", title ? title : "");
      snprintf(entry->artist, sizeof(entry->artist), "%s", artist ? artist : "");
      snprintf(entry->album, sizeof(entry->album), "%s", album ? album : "");
      entry->duration_sec = (uint32_t)duration;

      uq->queue_length++;
   }
   sqlite3_finalize(sel_stmt);

   /* Load state */
   sqlite3_stmt *state_stmt;
   rc = sqlite3_prepare_v2(s_db,
                           "SELECT shuffle, repeat_mode FROM user_queue_state WHERE user_id = ?",
                           -1, &state_stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("Music queue DB: Prepare SELECT state failed: %s", sqlite3_errmsg(s_db));
      return 1;
   }
   sqlite3_bind_int(state_stmt, 1, user_id);

   if (sqlite3_step(state_stmt) == SQLITE_ROW) {
      uq->shuffle = sqlite3_column_int(state_stmt, 0) != 0;
      uq->repeat_mode = (music_repeat_mode_t)sqlite3_column_int(state_stmt, 1);
   }
   sqlite3_finalize(state_stmt);

   if (uq->queue_length > 0) {
      OLOG_INFO("Music queue DB: Loaded %d tracks for user %d (shuffle=%d, repeat=%d)",
                uq->queue_length, user_id, uq->shuffle, (int)uq->repeat_mode);
   }

   return 0;
}

/* =============================================================================
 * Delete
 * ============================================================================= */

int music_queue_db_delete_user(int user_id) {
   if (!s_db || user_id <= 0) {
      return 0;
   }

   sqlite3_stmt *stmt;
   int rc;

   rc = sqlite3_prepare_v2(s_db, "DELETE FROM user_queue WHERE user_id = ?", -1, &stmt, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, user_id);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
   }

   rc = sqlite3_prepare_v2(s_db, "DELETE FROM user_queue_state WHERE user_id = ?", -1, &stmt, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int(stmt, 1, user_id);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
   }

   OLOG_INFO("Music queue DB: Deleted queue data for user %d", user_id);
   return 0;
}
