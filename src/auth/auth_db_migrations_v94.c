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
 * Schema migration v94: request context that stays put.
 *
 * - messages.kind marks rows that are request context rather than messages
 *   anyone sees (turn context, memory, directives, operator instructions, loop
 *   notes, envelopes).  Display, search and export reads skip them; the replay
 *   read returns them, so a reload rebuilds the request exactly.
 * - messages.context_of names the question a turn's context row goes in front
 *   of, so it attaches there even when other rows landed between them.
 * - conversations.prefix_hash / tools_hash name the conversation's frozen system
 *   prompt and tool set in prompt_blobs, in_force_hash which of its sections
 *   and standing directions are in force now; reasoning_floor_msg_id marks where a
 *   declared boundary left earlier reasoning behind, and reasoning_floor_pending
 *   (a time) that a withdrawal left every row's reasoning behind until a turn
 *   built after it is saved.
 * - conversation_focus_handles gives each injected memory item a citation handle
 *   that stays the same for the conversation's life.
 *
 * - document_chunks is rebuilt with AUTOINCREMENT (same rows, same ids), so a
 *   deleted chunk's id never names a new chunk.
 * - withdrawn_items records a user's removals (memory items, document chunks)
 *   for withdrawal from stored context; the TEMP triggers that write it are
 *   made at init on DAWN's connection (auth_db_withdraw_install).
 *
 * Existing conversations take their frozen prefix the first time they are
 * continued.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

/* Whether document_chunks never reuses an id (AUTOINCREMENT), or doesn't
 * exist (nothing to rebuild). */
static bool chunks_autoincrement(sqlite3 *db) {
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db,
                          "SELECT sql FROM sqlite_master WHERE type = 'table' AND "
                          "name = 'document_chunks'",
                          -1, &st, NULL) != SQLITE_OK) {
      return false;
   }
   bool yes = true;
   if (sqlite3_step(st) == SQLITE_ROW) {
      const char *sql = (const char *)sqlite3_column_text(st, 0);
      yes = sql && strstr(sql, "AUTOINCREMENT") != NULL;
   }
   sqlite3_finalize(st);
   return yes;
}

/* The one number @p sql counts, or -1. */
static int count_rows(sqlite3 *db, const char *sql) {
   sqlite3_stmt *st = NULL;
   int n = -1;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
      n = sqlite3_column_int(st, 0);
   }
   sqlite3_finalize(st);
   return n;
}

/* A deleted chunk's id must never name a new one: its withdrawn_items row and
 * the conversations it was injected into still name it.  SQLite can't add
 * AUTOINCREMENT to a table, so it is rebuilt with the same rows and ids (the
 * FTS index keys on them), then its index and triggers are made again. */
static int chunks_rebuild(sqlite3 *db) {
   if (chunks_autoincrement(db)) {
      return AUTH_DB_SUCCESS;
   }
   /* A chunk whose document is gone (deleted where foreign keys were off)
    * can't move to a table that checks them, and was never readable: it is
    * left behind (logged).  A savepoint, so this nests in any transaction. */
   static const char sql[] =
       "SAVEPOINT v94_chunks;"
       "CREATE TABLE document_chunks_v94 ("
       "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
       "  document_id INTEGER NOT NULL,"
       "  chunk_index INTEGER NOT NULL,"
       "  text TEXT NOT NULL,"
       "  embedding BLOB NOT NULL,"
       "  embedding_norm REAL NOT NULL,"
       "  created_at INTEGER NOT NULL DEFAULT 0,"
       "  FOREIGN KEY(document_id) REFERENCES documents(id) ON DELETE CASCADE);"
       "INSERT INTO document_chunks_v94 (id, document_id, chunk_index, text, embedding, "
       "embedding_norm, created_at) SELECT id, document_id, chunk_index, text, embedding, "
       "embedding_norm, created_at FROM document_chunks "
       "WHERE document_id IN (SELECT id FROM documents);"
       /* Its sequence starts past every id the old table ever held (a left
        * orphan's too), so none is handed out again. */
       "INSERT INTO sqlite_sequence (name, seq) SELECT 'document_chunks_v94', 0 "
       "WHERE NOT EXISTS (SELECT 1 FROM sqlite_sequence WHERE name = 'document_chunks_v94');"
       "UPDATE sqlite_sequence SET seq = MAX(seq, (SELECT COALESCE(MAX(id), 0) FROM "
       "document_chunks)) WHERE name = 'document_chunks_v94';"
       "DROP TABLE document_chunks;"
       "ALTER TABLE document_chunks_v94 RENAME TO document_chunks;"
       "CREATE INDEX IF NOT EXISTS idx_doc_chunks_doc ON "
       "document_chunks(document_id);" DOC_CHUNK_GENERATION_TRIGGERS_SQL "RELEASE v94_chunks;";
   const int before = count_rows(db, "SELECT COUNT(*) FROM document_chunks");
   char *errmsg = NULL;
   if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v94 document_chunks rebuild failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      sqlite3_exec(db, "ROLLBACK TO v94_chunks; RELEASE v94_chunks;", NULL, NULL, NULL);
      return AUTH_DB_FAILURE;
   }
   const int after = count_rows(db, "SELECT COUNT(*) FROM document_chunks");
   if (before > after) {
      OLOG_WARNING("auth_db: v94 left %d chunk(s) of deleted documents behind", before - after);
   }
   OLOG_INFO("auth_db: v94 rebuilt document_chunks so chunk ids are never reused");
   return AUTH_DB_SUCCESS;
}

int auth_db_migrations_v94(sqlite3 *db) {
   if (!db) {
      return AUTH_DB_FAILURE;
   }

   static const struct {
      const char *table;
      const char *column;
      const char *sql;
   } columns[] = {
      { "messages", "kind",
        "ALTER TABLE messages ADD COLUMN kind TEXT DEFAULT NULL " CONV_MESSAGE_KIND_CHECK_SQL },
      { "messages", "context_of",
        "ALTER TABLE messages ADD COLUMN context_of INTEGER DEFAULT NULL" },
      { "conversations", "prefix_hash",
        "ALTER TABLE conversations ADD COLUMN prefix_hash TEXT DEFAULT NULL" },
      { "conversations", "tools_hash",
        "ALTER TABLE conversations ADD COLUMN tools_hash TEXT DEFAULT NULL" },
      { "conversations", "in_force_hash",
        "ALTER TABLE conversations ADD COLUMN in_force_hash TEXT DEFAULT NULL" },
      { "conversations", "reasoning_floor_msg_id",
        "ALTER TABLE conversations ADD COLUMN reasoning_floor_msg_id INTEGER NOT NULL DEFAULT 0" },
      { "conversations", "reasoning_floor_pending",
        "ALTER TABLE conversations ADD COLUMN reasoning_floor_pending INTEGER NOT NULL DEFAULT 0" },
      { "conversations", "reasoning_floor_seq",
        "ALTER TABLE conversations ADD COLUMN reasoning_floor_seq INTEGER NOT NULL DEFAULT 0" },
   };
   for (size_t i = 0; i < sizeof(columns) / sizeof(columns[0]); i++) {
      if (auth_db_column_exists(db, columns[i].table, columns[i].column)) {
         continue;
      }
      char *errmsg = NULL;
      if (sqlite3_exec(db, columns[i].sql, NULL, NULL, &errmsg) != SQLITE_OK) {
         OLOG_ERROR("auth_db: v94 ALTER (%s.%s) failed: %s", columns[i].table, columns[i].column,
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
   }

   /* Here, not in the base schema: both index a column this migration adds.
    * idx_messages_display serves every display read (kind IS NULL) without
    * reading `kind` from the row itself, which sits after a turn's stored
    * blocks; idx_messages_kind finds a conversation's (or a user's) context
    * rows of one kind the same way. */
   static const char *const indexes[] = {
      /* A user's removals (a forgotten memory item, a deleted document's
       * chunks), recorded by the TEMP triggers auth_db_withdraw_install makes
       * on DAWN's connection while a removal is marked, so what was injected
       * into conversations can be withdrawn from their stored context
       * (auth_db_withdraw.c).  'memory' rows mark a USER MEMORY withdrawal.
       * delivered: sent to the user's live sessions; upto_msg_id (a 'memory'
       * row): the newest message its withdrawal covered.  Kept a week. */
      "CREATE TABLE IF NOT EXISTS withdrawn_items ("
      "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "   item_id TEXT NOT NULL,"
      "   user_id INTEGER NOT NULL,"
      "   created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
      "   delivered INTEGER NOT NULL DEFAULT 0,"
      "   upto_msg_id INTEGER NOT NULL DEFAULT 0"
      ")",
      "CREATE INDEX IF NOT EXISTS idx_withdrawn_items_item ON withdrawn_items(item_id, created_at)",
      "CREATE INDEX IF NOT EXISTS idx_withdrawn_items_time ON withdrawn_items(created_at)",
      "CREATE INDEX IF NOT EXISTS idx_withdrawn_items_open ON withdrawn_items(user_id) "
      "WHERE delivered = 0",
      "CREATE INDEX IF NOT EXISTS idx_messages_display ON messages (conversation_id, id) "
      "WHERE kind IS NULL",
      "CREATE INDEX IF NOT EXISTS idx_messages_kind ON messages (kind, conversation_id, id) "
      "WHERE kind IS NOT NULL",
      "CREATE INDEX IF NOT EXISTS idx_conversations_prefix ON conversations (prefix_hash) "
      "WHERE prefix_hash IS NOT NULL",
      "CREATE INDEX IF NOT EXISTS idx_conversations_tools ON conversations (tools_hash) "
      "WHERE tools_hash IS NOT NULL",
      "CREATE INDEX IF NOT EXISTS idx_conversations_in_force ON conversations (in_force_hash) "
      "WHERE in_force_hash IS NOT NULL",
   };
   for (size_t i = 0; i < sizeof(indexes) / sizeof(indexes[0]); i++) {
      char *errmsg = NULL;
      if (sqlite3_exec(db, indexes[i], NULL, NULL, &errmsg) != SQLITE_OK) {
         OLOG_ERROR("auth_db: v94 index failed: %s", errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
   }
   return chunks_rebuild(db);
}
