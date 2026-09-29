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
 * Conversation message rows: the single insert, the replay read that returns
 * each assistant turn's stored blocks, and the compaction watermark (which
 * drops blocks the context no longer loads).
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "auth/auth_db_messages.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "core/message_kind.h"
#include "logging.h"
#include "utils/string_utils.h"

/* Rows the watermark GC clears per statement: each is its own short hold of
 * the database lock, since clearing a row frees its blocks' pages. */
#define GC_BATCH_ROWS 32

int auth_db_messages_prepare(void) {
   static const struct {
      const char *name;
      const char *sql;
      sqlite3_stmt **stmt;
   } stmts[] = {
      /* A context row's question (?12) must be a row of the same conversation
       * a turn's context goes in front of: a user message, ordinary or an
       * envelope. */
      { "msg_add",
        "INSERT INTO messages (conversation_id, role, content, tool_calls, tool_call_id, "
        "reasoning, llm_blocks_len, llm_blocks, created_at, is_error, kind, context_of) "
        "SELECT ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12 "
        "WHERE EXISTS (SELECT 1 FROM conversations WHERE id = ?1 AND user_id = ?13) "
        "AND (?12 IS NULL OR EXISTS (SELECT 1 FROM messages q WHERE q.id = ?12 "
        "AND q.conversation_id = ?1 AND q.role = 'user' "
        "AND (q.kind IS NULL OR q.kind = 'envelope')))",
        &s_db.stmt_msg_add },
      /* A row's blocks load only past the budget cutoff (?4) and the
       * conversation's reasoning floor (a declared boundary left the reasoning at
       * or below it behind), and none while a withdrawal's floor is pending
       * (auth_db_withdraw.h); the column is read nowhere else.  llm_blocks_len
       * sits before the blocks, so testing it never reads them. */
      { "msg_get_llm",
        "SELECT m.id, m.role, m.content, m.tool_calls, m.tool_call_id, "
        "CASE WHEN m.id > MAX(?4, c.reasoning_floor_msg_id) AND c.reasoning_floor_pending = 0 "
        "AND m.llm_blocks_len IS NOT NULL "
        "THEN m.llm_blocks END, "
        "m.created_at, m.is_error, m.kind, m.context_of FROM messages m "
        "INNER JOIN conversations c ON m.conversation_id = c.id "
        "WHERE m.conversation_id = ?1 AND c.user_id = ?2 AND m.id > ?3 ORDER BY m.id ASC",
        &s_db.stmt_msg_get_llm },
      /* kind-rows: sizes the replay read's own rows. */
      { "msg_llm_sizes",
        "SELECT m.id, m.llm_blocks_len FROM messages m "
        "INNER JOIN conversations c ON m.conversation_id = c.id "
        "WHERE m.conversation_id = ?1 AND c.user_id = ?2 AND m.id > ?3 "
        "AND m.id > c.reasoning_floor_msg_id AND c.reasoning_floor_pending = 0 "
        "AND m.llm_blocks_len IS NOT NULL "
        "ORDER BY m.id DESC",
        &s_db.stmt_msg_llm_sizes },
      /* The v67 watermark, with a monotonic guard so a stale async compaction
       * can't rewind one a later pass already advanced. */
      { "conv_set_watermark",
        "UPDATE conversations SET compaction_summary = ?, context_watermark_msg_id = ? "
        "WHERE id = ? AND user_id = ? AND ? >= context_watermark_msg_id",
        &s_db.stmt_conv_set_watermark },
      /* kind-rows: clears blocks, whatever the row. */
      { "msg_gc_blocks",
        "UPDATE messages SET llm_blocks = NULL, llm_blocks_len = NULL WHERE id IN ("
        "SELECT id FROM messages WHERE conversation_id = ? AND id <= ? "
        "AND llm_blocks_len IS NOT NULL LIMIT " STRINGIFY(GC_BATCH_ROWS) ")",
        &s_db.stmt_msg_gc_blocks },
      /* kind-rows: clears blocks, whatever the row. */
      { "msg_sweep_blocks",
        "UPDATE messages SET llm_blocks = NULL, llm_blocks_len = NULL WHERE id IN ("
        "SELECT m.id FROM messages m JOIN conversations c ON c.id = m.conversation_id "
        "WHERE m.llm_blocks_len IS NOT NULL AND m.id <= c.context_watermark_msg_id "
        "LIMIT " STRINGIFY(GC_BATCH_ROWS) ")",
        &s_db.stmt_msg_sweep_blocks },
   };

   for (size_t i = 0; i < sizeof(stmts) / sizeof(stmts[0]); i++) {
      if (sqlite3_prepare_v2(s_db.db, stmts[i].sql, -1, stmts[i].stmt, NULL) != SQLITE_OK) {
         OLOG_ERROR("auth_db: prepare %s failed: %s", stmts[i].name, sqlite3_errmsg(s_db.db));
         return AUTH_DB_FAILURE;
      }
   }
   return AUTH_DB_SUCCESS;
}

void auth_db_messages_finalize(void) {
   sqlite3_stmt **stmts[] = { &s_db.stmt_msg_add,       &s_db.stmt_msg_get_llm,
                              &s_db.stmt_msg_llm_sizes, &s_db.stmt_conv_set_watermark,
                              &s_db.stmt_msg_gc_blocks, &s_db.stmt_msg_sweep_blocks };
   for (size_t i = 0; i < sizeof(stmts) / sizeof(stmts[0]); i++) {
      if (*stmts[i]) {
         sqlite3_finalize(*stmts[i]);
         *stmts[i] = NULL;
      }
   }
}

static void bind_text_or_null(sqlite3_stmt *st, int idx, const char *text) {
   if (text)
      sqlite3_bind_text(st, idx, text, -1, SQLITE_STATIC);
   else
      sqlite3_bind_null(st, idx);
}

static bool valid_role(const char *role) {
   return strcmp(role, "system") == 0 || strcmp(role, "user") == 0 ||
          strcmp(role, "assistant") == 0 || strcmp(role, "tool") == 0;
}

/* After an insert changed nothing: the conversation is missing, or someone
 * else's. */
static int missing_or_forbidden_locked(int64_t conv_id) {
   sqlite3_stmt *st = NULL;
   int result = AUTH_DB_NOT_FOUND;
   if (sqlite3_prepare_v2(s_db.db, "SELECT 1 FROM conversations WHERE id = ?", -1, &st, NULL) ==
       SQLITE_OK) {
      sqlite3_bind_int64(st, 1, conv_id);
      if (sqlite3_step(st) == SQLITE_ROW)
         result = AUTH_DB_FORBIDDEN;
      sqlite3_finalize(st);
   }
   return result;
}

/* A row the insert accepts: a known role, a kind that fits it, a question only
 * on a kinded row.  Its blocks come back through @p blocks_out (NULL when the
 * row can't hold them: the text still saves). */
static int row_check(int64_t conv_id, const conv_message_row_t *row, const char **blocks_out) {
   *blocks_out = NULL;
   if (conv_id <= 0 || !row || !row->role || !row->content || !valid_role(row->role) ||
       row->context_of < 0) {
      return AUTH_DB_INVALID;
   }
   const message_kind_t kind = message_kind_parse(row->kind);
   if ((row->kind && *row->kind && kind == MESSAGE_KIND_NONE) ||
       !message_kind_role_ok(kind, row->role) ||
       (row->context_of > 0 && kind == MESSAGE_KIND_NONE)) {
      OLOG_WARNING("conv_db_add_row: kind '%s' on a %s row refused", row->kind ? row->kind : "",
                   row->role);
      return AUTH_DB_INVALID;
   }

   /* Blocks describe an assistant turn; a row that can't hold them loses them
    * rather than failing the write (the text still saves). */
   const char *blocks = row->llm_blocks;
   const size_t blocks_len = blocks ? strlen(blocks) : 0;
   if (blocks && strcmp(row->role, "assistant") != 0) {
      OLOG_WARNING("conv_db_add_row: blocks on a %s row in conv %lld dropped", row->role,
                   (long long)conv_id);
      blocks = NULL;
   } else if (blocks && (blocks_len == 0 || blocks_len > CONV_LLM_BLOCKS_MAX)) {
      OLOG_WARNING("conv_db_add_row: %zu bytes of blocks in conv %lld not stored", blocks_len,
                   (long long)conv_id);
      blocks = NULL;
   }
   *blocks_out = blocks;
   return AUTH_DB_SUCCESS;
}

int msg_insert_locked(int64_t conv_id,
                      int user_id,
                      const conv_message_row_t *row,
                      time_t now,
                      int64_t *id_out) {
   *id_out = 0;
   const char *blocks = NULL;
   const int checked = row_check(conv_id, row, &blocks);
   if (checked != AUTH_DB_SUCCESS) {
      return checked;
   }
   const message_kind_t kind = message_kind_parse(row->kind);

   sqlite3_stmt *st = s_db.stmt_msg_add;
   sqlite3_reset(st);
   sqlite3_bind_int64(st, 1, conv_id);
   sqlite3_bind_text(st, 2, row->role, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 3, row->content, -1, SQLITE_STATIC);
   bind_text_or_null(st, 4, row->tool_calls);
   bind_text_or_null(st, 5, row->tool_call_id);
   bind_text_or_null(st, 6, row->reasoning);
   if (blocks) {
      const size_t blocks_len = strlen(blocks);
      sqlite3_bind_int64(st, 7, (int64_t)blocks_len);
      sqlite3_bind_text(st, 8, blocks, (int)blocks_len, SQLITE_STATIC);
   } else {
      sqlite3_bind_null(st, 7);
      sqlite3_bind_null(st, 8);
   }
   sqlite3_bind_int64(st, 9, (int64_t)now);
   sqlite3_bind_int(st, 10, row->is_error ? 1 : 0);
   bind_text_or_null(st, 11, message_kind_name(kind));
   if (row->context_of > 0) {
      sqlite3_bind_int64(st, 12, row->context_of);
   } else {
      sqlite3_bind_null(st, 12);
   }
   sqlite3_bind_int(st, 13, user_id);

   int rc = sqlite3_step(st);
   sqlite3_reset(st);
   sqlite3_clear_bindings(st);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("conv_db_add_row: insert failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }
   if (sqlite3_changes(s_db.db) == 0) {
      int result = missing_or_forbidden_locked(conv_id);
      if (result == AUTH_DB_FORBIDDEN && row->context_of > 0) {
         /* The conversation may be the user's: the question isn't one of its. */
         result = AUTH_DB_INVALID;
         OLOG_WARNING("conv_db_add_row: row %lld is no question of conv %lld",
                      (long long)row->context_of, (long long)conv_id);
      }
      return result;
   }
   *id_out = sqlite3_last_insert_rowid(s_db.db);
   return AUTH_DB_SUCCESS;
}

int conv_db_add_row(int64_t conv_id, int user_id, const conv_message_row_t *row, int64_t *id_out) {
   if (id_out)
      *id_out = 0;
   const char *blocks = NULL;
   const int checked = row_check(conv_id, row, &blocks);
   if (checked != AUTH_DB_SUCCESS) {
      return checked;
   }

   AUTH_DB_LOCK_OR_FAIL();

   const time_t now = time(NULL);
   int64_t id = 0;
   const int result = msg_insert_locked(conv_id, user_id, row, now, &id);
   if (result != AUTH_DB_SUCCESS) {
      AUTH_DB_UNLOCK();
      return result;
   }
   if (id_out)
      *id_out = id;

   /* Request context is nobody's message: no count, no reordering, no signal. */
   if (message_kind_parse(row->kind) != MESSAGE_KIND_NONE) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_SUCCESS;
   }

   sqlite3_reset(s_db.stmt_conv_update_meta);
   sqlite3_bind_int64(s_db.stmt_conv_update_meta, 1, (int64_t)now);
   sqlite3_bind_int64(s_db.stmt_conv_update_meta, 2, conv_id);
   const int rc = sqlite3_step(s_db.stmt_conv_update_meta);
   sqlite3_reset(s_db.stmt_conv_update_meta);

   AUTH_DB_UNLOCK();

   /* The row is saved: a caller that retried on this would write it twice. */
   if (rc != SQLITE_DONE) {
      OLOG_WARNING("conv_db_add_row: conv %lld metadata not updated (row %lld saved)",
                   (long long)conv_id, (long long)id);
   }

   /* The updated_at bump re-orders this user's sidebar — tell any connected
    * browser (leaf-lock rule: after the unlock). Skip role="tool" rows: the
    * assistant row of the same turn already emitted a "bumped", so a multi-row
    * tool turn fires one signal, not N. */
   if (strcmp(row->role, "tool") != 0)
      conversation_list_changed_notify(user_id, conv_id, CONV_LIST_CHANGE_BUMPED);

   return AUTH_DB_SUCCESS;
}

int conv_db_add_message_with_tools_ex(int64_t conv_id,
                                      int user_id,
                                      const char *role,
                                      const char *content,
                                      const char *tool_calls,
                                      const char *tool_call_id,
                                      const char *reasoning,
                                      bool is_error,
                                      int64_t *msg_id_out) {
   const conv_message_row_t row = { .role = role,
                                    .content = content,
                                    .tool_calls = tool_calls,
                                    .tool_call_id = tool_call_id,
                                    .reasoning = reasoning,
                                    .is_error = is_error };
   return conv_db_add_row(conv_id, user_id, &row, msg_id_out);
}

int conv_db_add_message_with_tools(int64_t conv_id,
                                   int user_id,
                                   const char *role,
                                   const char *content,
                                   const char *tool_calls,
                                   const char *tool_call_id,
                                   const char *reasoning,
                                   int64_t *msg_id_out) {
   return conv_db_add_message_with_tools_ex(conv_id, user_id, role, content, tool_calls,
                                            tool_call_id, reasoning, false, msg_id_out);
}

int conv_db_add_message_ex(int64_t conv_id,
                           int user_id,
                           const char *role,
                           const char *content,
                           int64_t *msg_id_out) {
   return conv_db_add_message_with_tools(conv_id, user_id, role, content, NULL, NULL, NULL,
                                         msg_id_out);
}

int conv_db_add_message(int64_t conv_id, int user_id, const char *role, const char *content) {
   return conv_db_add_message_ex(conv_id, user_id, role, content, NULL);
}

/* The id at or below which rows load without blocks: walking back from the
 * newest row, the first whose blocks would pass the budget. 0 = all fit. */
static int64_t budget_cutoff_locked(int64_t conv_id, int user_id, int64_t after_id) {
   sqlite3_stmt *st = s_db.stmt_msg_llm_sizes;
   sqlite3_reset(st);
   sqlite3_bind_int64(st, 1, conv_id);
   sqlite3_bind_int(st, 2, user_id);
   sqlite3_bind_int64(st, 3, after_id);

   size_t total = 0;
   int64_t cutoff = 0;
   while (sqlite3_step(st) == SQLITE_ROW) {
      const size_t len = (size_t)sqlite3_column_int64(st, 1);
      if (total + len > CONV_LLM_BLOCKS_LOAD_BUDGET) {
         cutoff = sqlite3_column_int64(st, 0);
         break;
      }
      total += len;
   }
   sqlite3_reset(st);
   return cutoff;
}

int conv_db_get_messages_for_llm(int64_t conv_id,
                                 int user_id,
                                 int64_t after_id,
                                 conversation_llm_row_cb callback,
                                 void *ctx) {
   if (conv_id <= 0 || !callback || after_id < 0) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   const int64_t cutoff = budget_cutoff_locked(conv_id, user_id, after_id);
   if (cutoff > 0) {
      OLOG_WARNING("conv_db_get_messages_for_llm: conv %lld over the %zu-byte block budget; "
                   "rows up to %lld load without blocks",
                   (long long)conv_id, CONV_LLM_BLOCKS_LOAD_BUDGET, (long long)cutoff);
   }

   sqlite3_stmt *st = s_db.stmt_msg_get_llm;
   sqlite3_reset(st);
   sqlite3_bind_int64(st, 1, conv_id);
   sqlite3_bind_int(st, 2, user_id);
   sqlite3_bind_int64(st, 3, after_id);
   sqlite3_bind_int64(st, 4, cutoff);

   int rc;
   while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
      conversation_llm_row_t row = { 0 };
      row.id = sqlite3_column_int64(st, 0);
      const char *role = (const char *)sqlite3_column_text(st, 1);
      if (role)
         safe_strscpy(row.role, role);
      row.content = (const char *)sqlite3_column_text(st, 2);
      row.tool_calls = (const char *)sqlite3_column_text(st, 3);
      row.tool_call_id = (const char *)sqlite3_column_text(st, 4);
      row.llm_blocks = (const char *)sqlite3_column_text(st, 5);
      row.llm_blocks_len = row.llm_blocks ? (size_t)sqlite3_column_bytes(st, 5) : 0;
      row.created_at = (time_t)sqlite3_column_int64(st, 6);
      row.is_error = sqlite3_column_int(st, 7);
      row.kind = (const char *)sqlite3_column_text(st, 8);
      row.context_of = sqlite3_column_int64(st, 9);
      if (callback(&row, ctx) != 0)
         break;
   }

   sqlite3_reset(st);
   AUTH_DB_UNLOCK();

   return (rc == SQLITE_ROW || rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

/* Clear stored blocks at or below @p watermark in @p conv_id, a batch per lock
 * hold.  They are already out of every reload's reach (reads take id >
 * watermark); this frees what they hold. */
static void clear_compacted_blocks(int64_t conv_id, int64_t watermark) {
   for (;;) {
      AUTH_DB_LOCK_OR_RETURN_VOID();
      sqlite3_stmt *gc = s_db.stmt_msg_gc_blocks;
      sqlite3_reset(gc);
      sqlite3_bind_int64(gc, 1, conv_id);
      sqlite3_bind_int64(gc, 2, watermark);
      const int rc = sqlite3_step(gc);
      const int changes = sqlite3_changes(s_db.db);
      sqlite3_reset(gc);
      AUTH_DB_UNLOCK();
      if (rc != SQLITE_DONE) {
         OLOG_WARNING("conv_db: clearing compacted blocks in conv %lld failed; the sweep "
                      "retries",
                      (long long)conv_id);
         return;
      }
      if (changes < GC_BATCH_ROWS) {
         return;
      }
   }
}

int conv_db_set_compaction_watermark(int64_t conv_id,
                                     int user_id,
                                     const char *summary,
                                     int64_t watermark_msg_id) {
   if (conv_id <= 0 || watermark_msg_id <= 0) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *st = s_db.stmt_conv_set_watermark;
   sqlite3_reset(st);
   bind_text_or_null(st, 1, summary);
   sqlite3_bind_int64(st, 2, watermark_msg_id);
   sqlite3_bind_int64(st, 3, conv_id);
   sqlite3_bind_int(st, 4, user_id);
   sqlite3_bind_int64(st, 5, watermark_msg_id);
   const int rc = sqlite3_step(st);
   const int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(st);
   sqlite3_clear_bindings(st);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("conv_db_set_compaction_watermark: update failed");
      return AUTH_DB_FAILURE;
   }

   /* No change = missing, someone else's, or a stale pass the guard refused:
    * all benign, and nothing new is out of reach.  Otherwise the blocks the
    * watermark just passed have no reader left: clear them now, in batches
    * (conv_db_sweep_compacted_blocks catches any this misses). */
   if (changes > 0) {
      clear_compacted_blocks(conv_id, watermark_msg_id);
   }
   return AUTH_DB_SUCCESS;
}

int conv_db_sweep_compacted_blocks(int *cleared_out) {
   if (cleared_out) {
      *cleared_out = 0;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = s_db.stmt_msg_sweep_blocks;
   sqlite3_reset(st);
   const int rc = sqlite3_step(st);
   const int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(st);
   AUTH_DB_UNLOCK();
   if (rc != SQLITE_DONE) {
      return AUTH_DB_FAILURE;
   }
   if (cleared_out) {
      *cleared_out = changes;
   }
   return AUTH_DB_SUCCESS;
}
