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
 * A conversation's frozen request prefix and tool set (prompt_blobs, keyed by
 * SHA-256) and its reasoning floor.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include "auth/auth_db_conv_prefix.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "core/message_kind.h"
#include "logging.h"

void conv_prefix_free(conv_prefix_t *p) {
   if (!p) {
      return;
   }
   free(p->prefix);
   free(p->tools);
   free(p->in_force);
   memset(p, 0, sizeof(*p));
}

/* Input a caller may bind: a required non-empty prefix, an optional non-empty
 * tool set, both within the stored-size cap. */
static bool blob_input_ok(const char *prefix, const char *tools) {
   if (!prefix || !prefix[0] || strlen(prefix) > CONV_PROMPT_BLOB_MAX) {
      return false;
   }
   if (tools && (!tools[0] || strlen(tools) > CONV_PROMPT_BLOB_MAX)) {
      return false;
   }
   return true;
}

/* Store bytes under their hash (already stored: kept as is). Caller holds the
 * lock. */
int conv_prompt_blob_put_locked(const char *bytes, char hash_out[DAWN_SHA256_HEX_LEN]) {
   size_t len = strlen(bytes);
   dawn_sha256_hex(bytes, len, hash_out);
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "INSERT OR IGNORE INTO prompt_blobs (hash, bytes, created_at) "
                          "VALUES (?, ?, ?)",
                          -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("conv_prefix: prepare blob insert failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_text(st, 1, hash_out, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 2, bytes, (int)len, SQLITE_STATIC);
   sqlite3_bind_int64(st, 3, (int64_t)time(NULL));
   int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("conv_prefix: blob insert failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}

/* The bytes stored under `hash`, checked against it: AUTH_DB_INVALID when they
 * are missing or don't match. Caller holds the lock. */
int conv_prompt_blob_get_locked(const char *hash, char **out) {
   *out = NULL;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, "SELECT bytes FROM prompt_blobs WHERE hash = ?", -1, &st,
                          NULL) != SQLITE_OK) {
      OLOG_ERROR("conv_prefix: prepare blob read failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_text(st, 1, hash, -1, SQLITE_STATIC);
   int result = AUTH_DB_FAILURE;
   if (sqlite3_step(st) == SQLITE_ROW) {
      const char *bytes = (const char *)sqlite3_column_text(st, 0);
      int len = sqlite3_column_bytes(st, 0);
      char check[DAWN_SHA256_HEX_LEN];
      if (bytes) {
         dawn_sha256_hex(bytes, (size_t)len, check);
      }
      if (!bytes || strcmp(check, hash) != 0) {
         OLOG_ERROR("conv_prefix: stored prompt %.12s... does not match its hash", hash);
         result = AUTH_DB_INVALID;
      } else {
         *out = strndup(bytes, (size_t)len);
         result = *out ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
      }
   } else {
      OLOG_ERROR("conv_prefix: stored prompt %.12s... is missing", hash);
      result = AUTH_DB_INVALID;
   }
   sqlite3_finalize(st);
   return result;
}

/* Read the row under the lock. */
static int prefix_read_locked(int64_t conv_id, int user_id, conv_prefix_t *out) {
   memset(out, 0, sizeof(*out));
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT prefix_hash, tools_hash, reasoning_floor_msg_id, in_force_hash "
                          "FROM conversations WHERE id = ? AND user_id = ?",
                          -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("conv_prefix: prepare read failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, conv_id);
   sqlite3_bind_int(st, 2, user_id);
   int rc = sqlite3_step(st);
   if (rc != SQLITE_ROW) {
      sqlite3_finalize(st);
      return rc == SQLITE_DONE ? AUTH_DB_NOT_FOUND : AUTH_DB_FAILURE;
   }
   const char *ph = (const char *)sqlite3_column_text(st, 0);
   const char *th = (const char *)sqlite3_column_text(st, 1);
   if (ph) {
      snprintf(out->prefix_hash, sizeof(out->prefix_hash), "%s", ph);
   }
   if (th) {
      snprintf(out->tools_hash, sizeof(out->tools_hash), "%s", th);
   }
   out->reasoning_floor_msg_id = sqlite3_column_int64(st, 2);
   const char *fh = (const char *)sqlite3_column_text(st, 3);
   if (fh) {
      snprintf(out->in_force_hash, sizeof(out->in_force_hash), "%s", fh);
   }
   sqlite3_finalize(st);

   int result = out->prefix_hash[0] ? conv_prompt_blob_get_locked(out->prefix_hash, &out->prefix)
                                    : AUTH_DB_SUCCESS;
   if (result == AUTH_DB_SUCCESS && out->tools_hash[0]) {
      result = conv_prompt_blob_get_locked(out->tools_hash, &out->tools);
   }
   if (result == AUTH_DB_SUCCESS && out->in_force_hash[0]) {
      result = conv_prompt_blob_get_locked(out->in_force_hash, &out->in_force);
   }
   if (result != AUTH_DB_SUCCESS) {
      conv_prefix_free(out);
   }
   return result;
}

int conv_db_prefix_get(int64_t conv_id, int user_id, conv_prefix_t *out) {
   if (!out) {
      return AUTH_DB_FAILURE;
   }
   memset(out, 0, sizeof(*out));
   if (conv_id <= 0) {
      return AUTH_DB_NOT_FOUND;
   }
   AUTH_DB_LOCK_OR_FAIL();
   int result = prefix_read_locked(conv_id, user_id, out);
   AUTH_DB_UNLOCK();
   return result;
}

/* Make the conversation's stored prefix and tool set these (unchanged when
 * they already are), and raise its floor.  Caller holds the lock and the
 * transaction. */
static int turn_prefix_locked(int64_t conv_id, int user_id, const conv_turn_save_t *save) {
   if (save->prefix) {
      char prefix_hash[DAWN_SHA256_HEX_LEN];
      char tools_hash[DAWN_SHA256_HEX_LEN] = { 0 };
      if (conv_prompt_blob_put_locked(save->prefix, prefix_hash) != AUTH_DB_SUCCESS ||
          (save->tools &&
           conv_prompt_blob_put_locked(save->tools, tools_hash) != AUTH_DB_SUCCESS)) {
         return AUTH_DB_FAILURE;
      }
      sqlite3_stmt *st = NULL;
      if (sqlite3_prepare_v2(s_db.db,
                             "UPDATE conversations SET prefix_hash = ?1, tools_hash = ?2 "
                             "WHERE id = ?3 AND user_id = ?4 AND (prefix_hash IS NOT ?1 "
                             "OR tools_hash IS NOT ?2)",
                             -1, &st, NULL) != SQLITE_OK) {
         OLOG_ERROR("conv_prefix: prepare turn prefix failed: %s", sqlite3_errmsg(s_db.db));
         return AUTH_DB_FAILURE;
      }
      sqlite3_bind_text(st, 1, prefix_hash, -1, SQLITE_STATIC);
      if (save->tools) {
         sqlite3_bind_text(st, 2, tools_hash, -1, SQLITE_STATIC);
      } else {
         sqlite3_bind_null(st, 2);
      }
      sqlite3_bind_int64(st, 3, conv_id);
      sqlite3_bind_int(st, 4, user_id);
      const int rc = sqlite3_step(st);
      sqlite3_finalize(st);
      if (rc != SQLITE_DONE) {
         OLOG_ERROR("conv_prefix: turn prefix failed: %s", sqlite3_errmsg(s_db.db));
         return AUTH_DB_FAILURE;
      }
   }
   if (save->in_force) {
      char hash[DAWN_SHA256_HEX_LEN];
      if (conv_prompt_blob_put_locked(save->in_force, hash) != AUTH_DB_SUCCESS) {
         return AUTH_DB_FAILURE;
      }
      sqlite3_stmt *st = NULL;
      if (sqlite3_prepare_v2(s_db.db,
                             "UPDATE conversations SET in_force_hash = ? WHERE id = ? AND "
                             "user_id = ?",
                             -1, &st, NULL) != SQLITE_OK) {
         OLOG_ERROR("conv_prefix: prepare in-force failed: %s", sqlite3_errmsg(s_db.db));
         return AUTH_DB_FAILURE;
      }
      sqlite3_bind_text(st, 1, hash, -1, SQLITE_STATIC);
      sqlite3_bind_int64(st, 2, conv_id);
      sqlite3_bind_int(st, 3, user_id);
      const int rc = sqlite3_step(st);
      sqlite3_finalize(st);
      if (rc != SQLITE_DONE) {
         OLOG_ERROR("conv_prefix: in-force failed: %s", sqlite3_errmsg(s_db.db));
         return AUTH_DB_FAILURE;
      }
   }
   if (save->floor_msg_id > 0) {
      sqlite3_stmt *st = NULL;
      if (sqlite3_prepare_v2(s_db.db,
                             "UPDATE conversations SET reasoning_floor_msg_id = "
                             "MAX(reasoning_floor_msg_id, ?) WHERE id = ? AND user_id = ?",
                             -1, &st, NULL) != SQLITE_OK) {
         OLOG_ERROR("conv_prefix: prepare turn floor failed: %s", sqlite3_errmsg(s_db.db));
         return AUTH_DB_FAILURE;
      }
      sqlite3_bind_int64(st, 1, save->floor_msg_id);
      sqlite3_bind_int64(st, 2, conv_id);
      sqlite3_bind_int(st, 3, user_id);
      const int rc = sqlite3_step(st);
      sqlite3_finalize(st);
      if (rc != SQLITE_DONE) {
         OLOG_ERROR("conv_prefix: turn floor failed: %s", sqlite3_errmsg(s_db.db));
         return AUTH_DB_FAILURE;
      }
   }
   return AUTH_DB_SUCCESS;
}

/* A boundary with no question: every row before @p first_id (every row, with
 * none) leaves its reasoning behind.  The rows saved with it carry none.
 * Caller holds the lock and the transaction. */
static int floor_at_rows_locked(int64_t conv_id, int64_t first_id) {
   sqlite3_stmt *st = NULL;
   /* kind-rows: the floor is past every row before these. */
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE conversations SET reasoning_floor_msg_id = "
                          "MAX(reasoning_floor_msg_id, CASE WHEN ?2 > 0 THEN ?2 ELSE "
                          "(SELECT COALESCE(MAX(id), 0) FROM messages WHERE conversation_id = ?1) "
                          "END) WHERE id = ?1",
                          -1, &st, NULL) != SQLITE_OK) {
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, conv_id);
   sqlite3_bind_int64(st, 2, first_id);
   const int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

/* A withdrawal left the conversation's reasoning behind until a turn built
 * after it (later in the sequence, conv_db_withdraw_seq): this turn, when it
 * was, settles the floor at its question.  Caller holds the lock and the
 * transaction. */
static int floor_settled_locked(int64_t conv_id, const conv_turn_save_t *save) {
   if (save->question_id <= 0) {
      return AUTH_DB_SUCCESS;
   }
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE conversations SET reasoning_floor_msg_id = "
                          "MAX(reasoning_floor_msg_id, ?1), reasoning_floor_pending = 0 "
                          "WHERE id = ?2 AND reasoning_floor_pending > 0 "
                          "AND reasoning_floor_pending <= ?3",
                          -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("conv_prefix: prepare floor settle failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, save->question_id);
   sqlite3_bind_int64(st, 2, conv_id);
   sqlite3_bind_int64(st, 3, save->built_seq);
   const int rc = sqlite3_step(st);
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

/* A compaction the turn applied: the summary and the watermark reloads start
 * after (never moved back), then a summary node after the conversation's
 * latest (or the latest of the one it continues).  A watermark already past
 * this one (a later compaction saved first) leaves both as they are.  Caller
 * holds the lock and the transaction.  The summary must be the one the turn
 * sent, already neutralized when it was made (llm_compaction): it is stored
 * as given and replayed verbatim. */
static int compaction_locked(int64_t conv_id, int user_id, const conv_turn_save_t *save) {
   sqlite3_stmt *st = s_db.stmt_conv_set_watermark;
   sqlite3_reset(st);
   sqlite3_bind_text(st, 1, save->compaction_summary, -1, SQLITE_STATIC);
   sqlite3_bind_int64(st, 2, save->compaction_last_id);
   sqlite3_bind_int64(st, 3, conv_id);
   sqlite3_bind_int(st, 4, user_id);
   sqlite3_bind_int64(st, 5, save->compaction_last_id);
   int rc = sqlite3_step(st);
   const int moved = sqlite3_changes(s_db.db);
   sqlite3_reset(st);
   sqlite3_clear_bindings(st);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("conv_prefix: compaction watermark failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }
   if (moved == 0) {
      OLOG_WARNING("conv_prefix: conv %lld's watermark is past %lld already; compaction not "
                   "recorded",
                   (long long)conv_id, (long long)save->compaction_last_id);
      return AUTH_DB_SUCCESS;
   }

   int64_t prior = 0;
   int depth = 0;
   st = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT id, depth FROM summary_nodes WHERE conversation_id IN (?1, "
                          "(SELECT continued_from FROM conversations WHERE id = ?1)) "
                          "ORDER BY conversation_id = ?1 DESC, id DESC LIMIT 1",
                          -1, &st, NULL) != SQLITE_OK) {
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, conv_id);
   if (sqlite3_step(st) == SQLITE_ROW) {
      prior = sqlite3_column_int64(st, 0);
      depth = sqlite3_column_int(st, 1) + 1;
   }
   sqlite3_finalize(st);
   st = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "INSERT INTO summary_nodes (conversation_id, prior_node_id, depth, "
                          "msg_id_start, msg_id_end, level, summary_text, token_count, "
                          "created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                          -1, &st, NULL) != SQLITE_OK) {
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, conv_id);
   if (prior > 0) {
      sqlite3_bind_int64(st, 2, prior);
   } else {
      sqlite3_bind_null(st, 2);
   }
   sqlite3_bind_int(st, 3, depth);
   sqlite3_bind_int64(
       st, 4, save->compaction_first_id > 0 ? save->compaction_first_id : save->compaction_last_id);
   sqlite3_bind_int64(st, 5, save->compaction_last_id);
   sqlite3_bind_int(st, 6, save->compaction_level);
   sqlite3_bind_text(st, 7, save->compaction_summary, -1, SQLITE_STATIC);
   sqlite3_bind_int(st, 8, (int)((strlen(save->compaction_summary) + 20) / 4));
   sqlite3_bind_int64(st, 9, (int64_t)time(NULL));
   rc = sqlite3_step(st);
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("conv_prefix: summary node failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}

int conv_db_save_turn(int64_t conv_id,
                      int user_id,
                      const conv_turn_save_t *save,
                      int64_t *ids_out) {
   if (!save || (save->n_rows > 0 && !save->rows) || save->floor_msg_id < 0 ||
       (save->prefix && !blob_input_ok(save->prefix, save->tools)) ||
       (save->in_force && !blob_input_ok(save->in_force, NULL))) {
      return AUTH_DB_INVALID;
   }
   if (conv_id <= 0) {
      return AUTH_DB_NOT_FOUND;
   }
   for (size_t i = 0; i < save->n_rows; i++) {
      if (message_kind_parse(save->rows[i].kind) == MESSAGE_KIND_NONE) {
         return AUTH_DB_INVALID; /* a message someone sees is saved as one */
      }
      if (ids_out) {
         ids_out[i] = 0;
      }
   }

   AUTH_DB_LOCK_OR_FAIL();
   if (sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
      OLOG_ERROR("conv_prefix: BEGIN failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   /* The conversation first: a turn saved to another user's is refused whole. */
   int result = conv_db_owned_locked(conv_id, user_id);
   const time_t now = time(NULL);
   int64_t first_id = 0;
   for (size_t i = 0; result == AUTH_DB_SUCCESS && i < save->n_rows; i++) {
      int64_t id = 0;
      result = msg_insert_locked(conv_id, user_id, &save->rows[i], now, &id);
      if (ids_out) {
         ids_out[i] = id;
      }
      if (first_id == 0) {
         first_id = id;
      }
   }
   /* What the user forgot since the turn was built leaves its rows now. */
   bool withdrawn = false;
   if (result == AUTH_DB_SUCCESS && first_id > 0) {
      result = conv_db_withdraw_saved_locked(conv_id, user_id, first_id, save->built_at,
                                             save->built_seq, &withdrawn);
   }
   if (result == AUTH_DB_SUCCESS) {
      result = turn_prefix_locked(conv_id, user_id, save);
   }
   if (result == AUTH_DB_SUCCESS && save->floor_at_rows) {
      result = floor_at_rows_locked(conv_id, first_id);
   }
   const bool compacted = save->compaction_summary && save->compaction_summary[0] &&
                          save->compaction_last_id > 0;
   if (result == AUTH_DB_SUCCESS && compacted) {
      result = compaction_locked(conv_id, user_id, save);
   }
   if (result == AUTH_DB_SUCCESS && !withdrawn) {
      result = floor_settled_locked(conv_id, save);
   }
   const bool commit = result == AUTH_DB_SUCCESS;
   if (sqlite3_exec(s_db.db, commit ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK) {
      OLOG_ERROR("conv_prefix: turn %s failed: %s", commit ? "COMMIT" : "ROLLBACK",
                 sqlite3_errmsg(s_db.db));
      if (commit) {
         sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
         result = AUTH_DB_FAILURE;
      }
   }
   AUTH_DB_UNLOCK();
   /* What the watermark passed has no reader left: its blocks go, in batches. */
   if (result == AUTH_DB_SUCCESS && compacted) {
      conv_db_clear_compacted_blocks(conv_id, save->compaction_last_id);
   }
   if (result != AUTH_DB_SUCCESS && ids_out) {
      for (size_t i = 0; i < save->n_rows; i++) {
         ids_out[i] = 0;
      }
   }
   return result;
}

int conv_db_retract_envelope(int64_t conv_id, int user_id, int64_t envelope_id) {
   if (conv_id <= 0 || envelope_id <= 0) {
      return AUTH_DB_NOT_FOUND;
   }
   AUTH_DB_LOCK_OR_FAIL();
   if (sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   int result = conv_db_owned_locked(conv_id, user_id);
   sqlite3_stmt *st = NULL;
   if (result == AUTH_DB_SUCCESS) {
      /* kind-rows: the envelope, and whether anything but its context follows. */
      result = sqlite3_prepare_v2(s_db.db,
                                  "SELECT (SELECT COUNT(*) FROM messages WHERE id = ?2 AND "
                                  "conversation_id = ?1 AND kind = 'envelope'), "
                                  "(SELECT COUNT(*) FROM messages WHERE conversation_id = ?1 "
                                  "AND id > ?2 AND context_of IS NOT ?2 AND "
                                  "(kind IS NULL OR kind NOT IN " CONV_SCOPED_KINDS_SQL "))",
                                  -1, &st, NULL) == SQLITE_OK
                   ? AUTH_DB_SUCCESS
                   : AUTH_DB_FAILURE;
   }
   if (result == AUTH_DB_SUCCESS) {
      sqlite3_bind_int64(st, 1, conv_id);
      sqlite3_bind_int64(st, 2, envelope_id);
      if (sqlite3_step(st) != SQLITE_ROW) {
         result = AUTH_DB_FAILURE;
      } else if (sqlite3_column_int(st, 0) == 0) {
         result = AUTH_DB_NOT_FOUND;
      } else if (sqlite3_column_int(st, 1) > 0) {
         result = AUTH_DB_DUPLICATE;
      }
   }
   sqlite3_finalize(st);
   st = NULL;
   if (result == AUTH_DB_SUCCESS) {
      /* kind-rows: the envelope and the context sent in front of it go; what
       * its turn announced to the conversation (an instruction, direction or
       * tool-set change) stays where it is, naming no question. */
      /* A tool change it announced in place now follows no user turn: it
       * folds into the request's tools (live and on reload alike), so the
       * reasoning before it was given other tools: the floor goes past it. */
      static const char *const k_retract[] = {
         "DELETE FROM messages WHERE conversation_id = ?1 AND (id = ?2 OR (context_of = ?2 "
         "AND kind IN ('turn_context', 'memory')))",
         "UPDATE messages SET context_of = NULL WHERE conversation_id = ?1 AND context_of = ?2",
         "UPDATE conversations SET reasoning_floor_msg_id = MAX(reasoning_floor_msg_id, "
         "(SELECT MIN(id) FROM messages WHERE conversation_id = ?1 AND id > ?2 AND "
         "kind = 'tool_change')) WHERE id = ?1 AND EXISTS (SELECT 1 FROM messages WHERE "
         "conversation_id = ?1 AND id > ?2 AND kind = 'tool_change')",
      };
      for (size_t i = 0; result == AUTH_DB_SUCCESS && i < sizeof(k_retract) / sizeof(k_retract[0]);
           i++) {
         result = sqlite3_prepare_v2(s_db.db, k_retract[i], -1, &st, NULL) == SQLITE_OK
                      ? AUTH_DB_SUCCESS
                      : AUTH_DB_FAILURE;
         if (result == AUTH_DB_SUCCESS) {
            sqlite3_bind_int64(st, 1, conv_id);
            sqlite3_bind_int64(st, 2, envelope_id);
            result = sqlite3_step(st) == SQLITE_DONE ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
         }
         sqlite3_finalize(st);
         st = NULL;
      }
   }
   const bool commit = result == AUTH_DB_SUCCESS;
   if (sqlite3_exec(s_db.db, commit ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK &&
       commit) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      result = AUTH_DB_FAILURE;
   }
   AUTH_DB_UNLOCK();
   return result;
}

int conv_db_prompt_blobs_gc(int *deleted_out) {
   if (deleted_out) {
      *deleted_out = 0;
   }
   AUTH_DB_LOCK_OR_FAIL();
   /* Bytes are written in the same transaction as the reference to them, so an
    * unreferenced row is never one a writer is about to use. */
   int rc = sqlite3_exec(s_db.db,
                         "DELETE FROM prompt_blobs WHERE hash NOT IN ("
                         "SELECT prefix_hash FROM conversations WHERE prefix_hash IS NOT NULL "
                         "UNION SELECT tools_hash FROM conversations WHERE tools_hash IS NOT NULL "
                         "UNION SELECT in_force_hash FROM conversations "
                         "WHERE in_force_hash IS NOT NULL "
                         /* kind-rows: a large tool change's definitions. */
                         "UNION SELECT json_extract(content, '$.blob') FROM messages "
                         "WHERE kind = 'tool_change' AND json_valid(content) "
                         "AND json_extract(content, '$.blob') IS NOT NULL)",
                         NULL, NULL, NULL);
   int deleted = sqlite3_changes(s_db.db);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("conv_prefix: prompt GC failed: %s", sqlite3_errmsg(s_db.db));
   }
   AUTH_DB_UNLOCK();
   if (rc != SQLITE_OK) {
      return AUTH_DB_FAILURE;
   }
   if (deleted_out) {
      *deleted_out = deleted;
   }
   return AUTH_DB_SUCCESS;
}
