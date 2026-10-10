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
 * Schema migration v98: messages rebuilt once.
 *
 * - messages.images: the images a tool result carried, as a JSON array of
 *   image ids, on role='tool' rows only (CONV_MESSAGE_IMAGES_CHECK_SQL).
 * - Kinds are checked by triggers rather than a column CHECK, so a new kind
 *   (tool_change here, and the ones after it) needs no rebuild: a migration
 *   that adds one re-runs CONV_MESSAGES_OBJECTS_SQL, which makes them again.
 * - The stored blocks (llm_blocks_len, llm_blocks) move to the end of the
 *   row, so the columns filters read (kind, context_of, images) never sit
 *   behind a blob's overflow pages; idx_messages_images finds the rows that
 *   hold images.
 *
 * - conversation_images is filled for the rows already stored (the images
 *   each conversation's questions and tool rows name, recorded exactly as a
 *   new row's are), in the rebuild's transaction, so deleting a conversation
 *   from before this version takes the images only it names.
 *
 * It needs room for a copy of the table.  Without it DAWN can't start (every
 * message read and write needs this table): the error says how much to free.
 *
 * SQLite can't drop a column CHECK, so the table is made again: same rows,
 * same ids, its AUTOINCREMENT sequence kept at its high-water mark (ids are
 * referenced without foreign keys: context_of, the compaction watermark, the
 * reasoning floor, tool results, extraction cursors), its indexes and triggers
 * made again.  It takes the base schema's text and column order
 * (CONV_MESSAGES_TABLE_SQL), so a migrated database and a new one store the
 * same schema.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "auth/auth_db_storage.h"
#include "logging.h"

/* The columns carried over, in the new table's order (and images, from a
 * table that already has them: one made before the stored blocks moved
 * last). */
#define V98_COLUMNS_HEAD                                                       \
   "id, conversation_id, role, content, tool_calls, tool_call_id, reasoning, " \
   "created_at, is_error, kind, context_of, "
#define V98_COLUMNS V98_COLUMNS_HEAD "llm_blocks_len, llm_blocks"
#define V98_COLUMNS_IMAGES V98_COLUMNS_HEAD "images, llm_blocks_len, llm_blocks"

/* Room asked for past twice the table's size (its copy in the WAL, then in the
 * file before the old pages are freed). */
#define V98_MARGIN_BYTES ((int64_t)64 * 1024 * 1024)

static int64_t now_ms(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Bytes messages and its indexes take: their pages (dbstat), or, where SQLite
 * was built without it, every page in use (more than enough). */
static int64_t table_bytes(sqlite3 *db) {
   int64_t bytes = auth_db_query_int64(db,
                                       "SELECT COALESCE(SUM(pgsize), 0) FROM dbstat WHERE name IN "
                                       "('messages', 'idx_messages_conversation', "
                                       "'idx_messages_llm_blocks', 'idx_messages_display', "
                                       "'idx_messages_kind')");
   if (bytes < 0) {
      const int64_t page = auth_db_query_int64(db, "PRAGMA page_size");
      const int64_t pages = auth_db_query_int64(db, "PRAGMA page_count");
      const int64_t free_pages = auth_db_query_int64(db, "PRAGMA freelist_count");
      bytes = page > 0 && pages >= 0 && free_pages >= 0 ? (pages - free_pages) * page : 0;
   }
   return bytes;
}

/* Whether the table being rebuilt has every column it carries over (earlier
 * steps add them; a held one means this waits for it). */
static bool has_columns(sqlite3 *db) {
   static const char *const cols[] = {
      "tool_calls",     "tool_call_id", "reasoning", "is_error",
      "llm_blocks_len", "llm_blocks",   "kind",      "context_of"
   };
   for (size_t i = 0; i < sizeof(cols) / sizeof(cols[0]); i++) {
      if (!auth_db_column_exists(db, "messages", cols[i])) {
         OLOG_ERROR("auth_db: v98 needs messages.%s (an earlier step was held)", cols[i]);
         return false;
      }
   }
   return true;
}

static int exec_logged(sqlite3 *db, const char *sql, const char *what) {
   char *errmsg = NULL;
   if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v98 %s failed: %s", what, errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}

/* Whether messages' last column is the stored blocks (the v98 shape). */
static bool blocks_last(sqlite3 *db) {
   sqlite3_stmt *st = NULL;
   bool last = false;
   if (sqlite3_prepare_v2(db,
                          "SELECT name FROM pragma_table_info('messages') ORDER BY cid DESC "
                          "LIMIT 1",
                          -1, &st, NULL) == SQLITE_OK &&
       sqlite3_step(st) == SQLITE_ROW) {
      const char *name = (const char *)sqlite3_column_text(st, 0);
      last = name && strcmp(name, "llm_blocks") == 0;
   }
   sqlite3_finalize(st);
   return last;
}

bool auth_db_v98_has_room(int64_t table_bytes, int64_t free_bytes, int64_t *need_out) {
   const int64_t bytes = table_bytes > 0 ? table_bytes : 0;
   const int64_t need = 2 * bytes + bytes / 10 + V98_MARGIN_BYTES;
   if (need_out) {
      *need_out = need;
   }
   return free_bytes < 0 || free_bytes >= need;
}

/* The rebuild, in one transaction.  The old table is renamed out of the way
 * with legacy renames on, so nothing else that names messages is rewritten to
 * the old name: it names the new table once it exists. */
/* The rebuild's SQL, carrying @p columns over.  The sequence goes where the
 * old one was (the rename took its row along): an id handed out once, then
 * deleted, is never handed out again. */
#define V98_REBUILD_SQL(columns)                                                          \
   "ALTER TABLE messages RENAME TO messages_v97;" CONV_MESSAGES_TABLE_SQL                 \
   "INSERT INTO messages (" columns ") SELECT " columns " FROM messages_v97 ORDER BY id;" \
   "INSERT INTO sqlite_sequence (name, seq) SELECT 'messages', 0 "                        \
   "WHERE NOT EXISTS (SELECT 1 FROM sqlite_sequence WHERE name = 'messages');"            \
   "UPDATE sqlite_sequence SET seq = MAX(seq, "                                           \
   "COALESCE((SELECT seq FROM sqlite_sequence WHERE name = 'messages_v97'), 0), "         \
   "(SELECT COALESCE(MAX(id), 0) FROM messages_v97)) WHERE name = 'messages';"            \
   "DROP TABLE messages_v97;" CONV_MESSAGES_OBJECTS_SQL

/* The images each conversation's stored rows name, recorded as a new row's
 * are (conversation_images), so a conversation delete takes what only it
 * names.  Inside the caller's transaction. */
static int backfill_images(sqlite3 *db) {
   int64_t recorded = 0;
   if (auth_db_conv_images_backfill(db, &recorded) != AUTH_DB_SUCCESS) {
      OLOG_ERROR("auth_db: v98 recording the conversations' images failed");
      return AUTH_DB_FAILURE;
   }
   if (recorded > 0) {
      OLOG_INFO("auth_db: v98 recorded %lld conversation image reference(s)", (long long)recorded);
   }
   return AUTH_DB_SUCCESS;
}

static int rebuild(sqlite3 *db, bool has_images) {
   const char *sql = has_images ? V98_REBUILD_SQL(V98_COLUMNS_IMAGES)
                                : V98_REBUILD_SQL(V98_COLUMNS);

   if (exec_logged(db, "BEGIN IMMEDIATE", "BEGIN") != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }
   const int64_t before = auth_db_query_int64(db, "SELECT COUNT(*) FROM messages");
   if (exec_logged(db, sql, "messages rebuild") != AUTH_DB_SUCCESS) {
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      return AUTH_DB_FAILURE;
   }
   const int64_t after = auth_db_query_int64(db, "SELECT COUNT(*) FROM messages");
   if (before != after) {
      OLOG_ERROR("auth_db: v98 rebuild copied %lld of %lld messages; rolled back", (long long)after,
                 (long long)before);
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      return AUTH_DB_FAILURE;
   }
   if (backfill_images(db) != AUTH_DB_SUCCESS) {
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      return AUTH_DB_FAILURE;
   }
   if (exec_logged(db, "COMMIT", "COMMIT") != AUTH_DB_SUCCESS) {
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}

int auth_db_migrations_v98(sqlite3 *db, const char *db_path) {
   if (!db) {
      return AUTH_DB_FAILURE;
   }
   /* Already the v98 table (a new database, or a re-run): only its indexes
    * (IF NOT EXISTS) and triggers (made again), and the images its rows name
    * recorded (a table made by an earlier v98 that didn't record them). */
   const bool has_images = auth_db_column_exists(db, "messages", "images");
   /* email_ref after the blocks: v102 added it to this table, so it is
    * already the v98 one (a rebuild would drop the column). */
   if (has_images && (blocks_last(db) || auth_db_column_exists(db, "messages", "email_ref"))) {
      if (exec_logged(db, CONV_MESSAGES_OBJECTS_SQL, "messages indexes and triggers") !=
              AUTH_DB_SUCCESS ||
          exec_logged(db, "BEGIN IMMEDIATE", "BEGIN") != AUTH_DB_SUCCESS) {
         return AUTH_DB_FAILURE;
      }
      if (backfill_images(db) != AUTH_DB_SUCCESS ||
          exec_logged(db, "COMMIT", "COMMIT") != AUTH_DB_SUCCESS) {
         sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
         return AUTH_DB_FAILURE;
      }
      return AUTH_DB_SUCCESS;
   }
   if (!sqlite3_get_autocommit(db)) {
      OLOG_ERROR("auth_db: v98 must run outside a transaction");
      return AUTH_DB_FAILURE;
   }
   if (!has_columns(db)) {
      return AUTH_DB_FAILURE;
   }

   /* Without room the upgrade can't run, and this version can't run on the
    * old table: DAWN stops (the ladder fails init) and says what to free. */
   const int64_t bytes = table_bytes(db);
   const int64_t have = db_path ? auth_db_storage_free_bytes(db_path) : -1;
   int64_t need = 0;
   if (!auth_db_v98_has_room(bytes, have, &need)) {
      OLOG_ERROR("auth_db: CANNOT START: upgrading the database %s (schema v98) rebuilds its "
                 "messages table (%lld MB) and needs %lld bytes (~%lld MB) free on the "
                 "filesystem that holds it; %lld bytes (~%lld MB) are free.  Free at least "
                 "%lld MB there (or move the database to a larger disk and point DAWN at it), "
                 "then start DAWN again.  Nothing was changed.",
                 db_path ? db_path : "(unknown)", (long long)(bytes / (1024 * 1024)),
                 (long long)need, (long long)(need / (1024 * 1024)), (long long)have,
                 (long long)(have / (1024 * 1024)),
                 (long long)((need - have + 1024 * 1024 - 1) / (1024 * 1024)));
      return AUTH_DB_FAILURE;
   }

   OLOG_INFO("auth_db: v98 rebuilding messages (%lld MB, one time)",
             (long long)(bytes / (1024 * 1024)));
   const int64_t t0 = now_ms();
   /* Both are no-ops inside a transaction, so they're set around it. */
   const bool fk_on = auth_db_query_int64(db, "PRAGMA foreign_keys") == 1;
   const bool legacy_on = auth_db_query_int64(db, "PRAGMA legacy_alter_table") == 1;
   sqlite3_exec(db, "PRAGMA foreign_keys=OFF", NULL, NULL, NULL);
   sqlite3_exec(db, "PRAGMA legacy_alter_table=ON", NULL, NULL, NULL);
   const int rc = rebuild(db, has_images);
   if (!legacy_on) {
      sqlite3_exec(db, "PRAGMA legacy_alter_table=OFF", NULL, NULL, NULL);
   }
   if (fk_on) {
      sqlite3_exec(db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
   }
   /* The copy went through the WAL: write it back and shrink it now, while
    * nothing else runs (after a failure too; the main connection doesn't
    * checkpoint by itself once the storage thread runs). */
   sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
   if (rc != AUTH_DB_SUCCESS) {
      return AUTH_DB_FAILURE;
   }
   OLOG_INFO("auth_db: v98 rebuilt messages in %lld ms", (long long)(now_ms() - t0));
   return AUTH_DB_SUCCESS;
}
