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

#include <ctype.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "core/message_kind.h"
#include "image_store.h"
#include "llm/llm_context_text.h"
#include "logging.h"
#include "utils/string_utils.h"

/* The image SQL below names these by value. */
_Static_assert(IMAGE_RETAIN_PERMANENT == 1 && IMAGE_RETAIN_UNBOUND == 3 &&
                   IMAGE_SOURCE_CAPTURE == 5 && IMAGE_SOURCE_UPLOAD == 0 && IMAGE_SOURCE_MMS == 3,
               "the image SQL literals must match image_store.h");
_Static_assert(IMAGE_FILENAME_MAX <= CONV_IMAGE_FILENAME_MAX,
               "a delete hands back every image file name whole");

/* A row's images, named by its conversation (conversation_images): the
 * owner's images of @p sources among the ids in ?3 (a JSON array). */
#define IMAGES_REF_SQL(sources)                                                           \
   "INSERT OR IGNORE INTO conversation_images (image_id, conversation_id) SELECT id, ?1 " \
   "FROM images WHERE user_id = ?2 AND " sources " AND id IN (SELECT value FROM json_each(?3))"

/* A row's images bound: the user's unbound captures it names become permanent
 * (they go with the conversation, image_store's delete). */
#define IMAGES_BIND_SQL                                                                \
   "UPDATE images SET retention_policy = 1 WHERE retention_policy = 3 AND source = 5 " \
   "AND user_id = ?2 AND id IN (SELECT value FROM json_each(?1))"

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
        "reasoning, llm_blocks_len, llm_blocks, created_at, is_error, kind, context_of, images) "
        "SELECT ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?14 "
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
        "m.created_at, m.is_error, m.kind, m.context_of, m.images FROM messages m "
        "INNER JOIN conversations c ON m.conversation_id = c.id "
        "WHERE m.conversation_id = ?1 AND c.user_id = ?2 AND m.id > ?3 ORDER BY m.id ASC",
        &s_db.stmt_msg_get_llm },
      { "msg_bind_images", IMAGES_BIND_SQL, &s_db.stmt_msg_bind_images },
      { "msg_ref_captures", IMAGES_REF_SQL("source = 5"), &s_db.stmt_msg_ref_captures },
      { "msg_ref_uploads", IMAGES_REF_SQL("source IN (0, 3)"), &s_db.stmt_msg_ref_uploads },
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
   sqlite3_stmt **stmts[] = { &s_db.stmt_msg_add,         &s_db.stmt_msg_get_llm,
                              &s_db.stmt_msg_llm_sizes,   &s_db.stmt_conv_set_watermark,
                              &s_db.stmt_msg_gc_blocks,   &s_db.stmt_msg_sweep_blocks,
                              &s_db.stmt_msg_bind_images, &s_db.stmt_msg_ref_captures,
                              &s_db.stmt_msg_ref_uploads };
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

/* Whether @p id is an image id: "img_" and 12 letters or digits. */
static bool image_id_ok(const char *id) {
   if (!id || strlen(id) != IMAGE_ID_LEN - 1 || strncmp(id, "img_", 4) != 0) {
      return false;
   }
   for (size_t i = 4; i < IMAGE_ID_LEN - 1; i++) {
      if (!isalnum((unsigned char)id[i])) {
         return false;
      }
   }
   return true;
}

/* Whether @p json is what a row's images hold: a JSON array of 1 to
 * CONV_MESSAGE_IMAGES_MAX image ids. */
static bool images_ok(const char *json) {
   struct json_object *arr = json_tokener_parse(json);
   bool ok = arr && json_object_is_type(arr, json_type_array) &&
             json_object_array_length(arr) > 0 &&
             json_object_array_length(arr) <= CONV_MESSAGE_IMAGES_MAX;
   for (size_t i = 0; ok && i < json_object_array_length(arr); i++) {
      struct json_object *v = json_object_array_get_idx(arr, i);
      ok = json_object_is_type(v, json_type_string) && image_id_ok(json_object_get_string(v));
   }
   json_object_put(arr);
   return ok;
}

/* The image ids an ordinary question's markers ([IMAGE:<id>]) name, as a JSON
 * array into @p out (at most CONV_MESSAGE_IMAGES_MAX).  @return how many. */
static int question_image_ids(const conv_message_row_t *row, char *out, size_t size) {
   out[0] = '\0';
   if (strcmp(row->role, "user") != 0 || message_kind_parse(row->kind) != MESSAGE_KIND_NONE) {
      return 0;
   }
   static const char marker[] = "[IMAGE:";
   size_t len = 0;
   int n = 0;
   for (const char *p = strstr(row->content, marker); p && n < CONV_MESSAGE_IMAGES_MAX;
        p = strstr(p + 1, marker)) {
      const char *id = p + sizeof(marker) - 1;
      char one[IMAGE_ID_LEN];
      if (strlen(id) < IMAGE_ID_LEN || id[IMAGE_ID_LEN - 1] != ']') {
         continue;
      }
      memcpy(one, id, IMAGE_ID_LEN - 1);
      one[IMAGE_ID_LEN - 1] = '\0';
      if (!image_id_ok(one) || len + IMAGE_ID_LEN + 4 >= size) {
         continue;
      }
      len += (size_t)snprintf(out + len, size - len, "%s\"%s\"", n ? "," : "[", one);
      n++;
   }
   if (n > 0) {
      snprintf(out + len, size - len, "]");
   }
   return n;
}

/* Room for CONV_MESSAGE_IMAGES_MAX ids as a JSON array. */
#define QUESTION_IDS_JSON_MAX (CONV_MESSAGE_IMAGES_MAX * (IMAGE_ID_LEN + 3) + 4)

/* Record that @p conv_id names the images of ?3 (@p json) through @p st.
 * Caller holds the lock and the row's transaction. */
static bool ref_images_locked(sqlite3_stmt *st, int64_t conv_id, int user_id, const char *json) {
   sqlite3_reset(st);
   sqlite3_bind_int64(st, 1, conv_id);
   sqlite3_bind_int(st, 2, user_id);
   sqlite3_bind_text(st, 3, json, -1, SQLITE_STATIC);
   const int rc = sqlite3_step(st);
   sqlite3_reset(st);
   sqlite3_clear_bindings(st);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("conv_db: recording conv %lld's images failed: %s", (long long)conv_id,
                 sqlite3_errmsg(sqlite3_db_handle(st)));
   }
   return rc == SQLITE_DONE;
}

int auth_db_conv_images_backfill(sqlite3 *db, int64_t *recorded_out) {
   if (recorded_out) {
      *recorded_out = 0;
   }
   /* The rows the insert path records images for: an ordinary question whose
    * text may hold a marker (question_image_ids decides, as it does there),
    * and a tool row holding images. */
   static const char rows_sql[] =
       "SELECT m.conversation_id, c.user_id, m.role, m.content, m.kind, m.images "
       "FROM messages m JOIN conversations c ON c.id = m.conversation_id "
       "WHERE (m.role = 'user' AND instr(m.content, '[IMAGE:') > 0) "
       "OR (m.role = 'tool' AND m.images IS NOT NULL AND json_valid(m.images)) ORDER BY m.id";
   sqlite3_stmt *rows = NULL;
   sqlite3_stmt *captures = NULL;
   sqlite3_stmt *uploads = NULL;
   int result = AUTH_DB_FAILURE;
   int before = 0;
   int rc = SQLITE_DONE;
   if (sqlite3_prepare_v2(db, rows_sql, -1, &rows, NULL) != SQLITE_OK ||
       sqlite3_prepare_v2(db, IMAGES_REF_SQL("source = 5"), -1, &captures, NULL) != SQLITE_OK ||
       sqlite3_prepare_v2(db, IMAGES_REF_SQL("source IN (0, 3)"), -1, &uploads, NULL) !=
           SQLITE_OK) {
      OLOG_ERROR("auth_db: conversation images backfill prepare failed: %s", sqlite3_errmsg(db));
      goto out;
   }
   before = sqlite3_total_changes(db);
   while ((rc = sqlite3_step(rows)) == SQLITE_ROW) {
      const int64_t conv_id = sqlite3_column_int64(rows, 0);
      const int user_id = sqlite3_column_int(rows, 1);
      const char *role = (const char *)sqlite3_column_text(rows, 2);
      const char *content = (const char *)sqlite3_column_text(rows, 3);
      const char *kind = (const char *)sqlite3_column_text(rows, 4);
      const char *images = (const char *)sqlite3_column_text(rows, 5);
      if (!role || !content) {
         continue;
      }
      if (strcmp(role, "tool") == 0) {
         if (images && !ref_images_locked(captures, conv_id, user_id, images)) {
            goto out;
         }
         continue;
      }
      const conv_message_row_t row = { .role = role, .content = content, .kind = kind };
      char question_ids[QUESTION_IDS_JSON_MAX];
      if (question_image_ids(&row, question_ids, sizeof(question_ids)) > 0 &&
          !ref_images_locked(uploads, conv_id, user_id, question_ids)) {
         goto out;
      }
   }
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("auth_db: conversation images backfill read failed: %s", sqlite3_errmsg(db));
      goto out;
   }
   if (recorded_out) {
      *recorded_out = sqlite3_total_changes(db) - before;
   }
   result = AUTH_DB_SUCCESS;
out:
   sqlite3_finalize(rows);
   sqlite3_finalize(captures);
   sqlite3_finalize(uploads);
   return result;
}

/* A row the insert accepts: a known role, a kind that fits it, a question only
 * on a kinded row.  Its blocks come back through @p blocks_out (NULL when the
 * row can't hold them: the text still saves), its images through
 * @p images_out (likewise). */
static int row_check(int64_t conv_id,
                     const conv_message_row_t *row,
                     const char **blocks_out,
                     const char **images_out) {
   *blocks_out = NULL;
   *images_out = NULL;
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

   /* Images belong to a tool result. */
   if (row->images && (strcmp(row->role, "tool") != 0 || !images_ok(row->images))) {
      OLOG_WARNING("conv_db_add_row: images on a %s row in conv %lld dropped", row->role,
                   (long long)conv_id);
   } else {
      *images_out = row->images;
   }
   return AUTH_DB_SUCCESS;
}

/* A tool_change row whose definitions are in prompt_blobs: those bytes (caller
 * frees), else NULL (the row's own content stands).  Caller holds the lock. */
static char *tool_change_blob_locked(const conversation_llm_row_t *row) {
   static const char head[] = "{\"blob\":\"";
   if (!row->kind || strcmp(row->kind, "tool_change") != 0 || !row->content ||
       strncmp(row->content, head, sizeof(head) - 1) != 0) {
      return NULL;
   }
   char hash[DAWN_SHA256_HEX_LEN];
   const char *h = row->content + sizeof(head) - 1;
   const size_t len = strcspn(h, "\"");
   if (len != DAWN_SHA256_HEX_LEN - 1) {
      return NULL;
   }
   memcpy(hash, h, len);
   hash[len] = '\0';
   char *bytes = NULL;
   if (conv_prompt_blob_get_locked(hash, &bytes) != AUTH_DB_SUCCESS) {
      OLOG_WARNING("conv_db: tool change row %lld's definitions are missing; left out",
                   (long long)row->id);
      return NULL;
   }
   return bytes;
}

int msg_insert_locked(int64_t conv_id,
                      int user_id,
                      const conv_message_row_t *row,
                      time_t now,
                      int64_t *id_out) {
   *id_out = 0;
   const char *blocks = NULL;
   const char *images = NULL;
   const int checked = row_check(conv_id, row, &blocks, &images);
   if (checked != AUTH_DB_SUCCESS) {
      return checked;
   }
   const message_kind_t kind = message_kind_parse(row->kind);

   /* A large tool change's definitions go in prompt_blobs, in this
    * transaction; the row names them. */
   char stub[DAWN_SHA256_HEX_LEN + 16];
   const char *content = row->content;
   if (kind == MESSAGE_KIND_TOOL_CHANGE && strlen(content) > CONV_TOOL_CHANGE_INLINE_MAX) {
      char hash[DAWN_SHA256_HEX_LEN];
      if (conv_prompt_blob_put_locked(content, hash) != AUTH_DB_SUCCESS) {
         return AUTH_DB_FAILURE;
      }
      snprintf(stub, sizeof(stub), "{\"blob\":\"%s\"}", hash);
      content = stub;
   }

   sqlite3_stmt *st = s_db.stmt_msg_add;
   sqlite3_reset(st);
   sqlite3_bind_int64(st, 1, conv_id);
   sqlite3_bind_text(st, 2, row->role, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 3, content, -1, SQLITE_STATIC);
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
   bind_text_or_null(st, 14, images);

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

   /* The conversation names the images the row holds (a tool row's captures,
    * a question's uploads), in the caller's transaction. */
   char question_ids[QUESTION_IDS_JSON_MAX];
   if ((images && !ref_images_locked(s_db.stmt_msg_ref_captures, conv_id, user_id, images)) ||
       (question_image_ids(row, question_ids, sizeof(question_ids)) > 0 &&
        !ref_images_locked(s_db.stmt_msg_ref_uploads, conv_id, user_id, question_ids))) {
      *id_out = 0;
      return AUTH_DB_FAILURE;
   }

   /* The images it names go with the conversation from now on: bound in the
    * caller's transaction, so the row and its images are saved together. */
   if (images && !row->images_bind_later) {
      sqlite3_stmt *bind = s_db.stmt_msg_bind_images;
      sqlite3_reset(bind);
      sqlite3_bind_text(bind, 1, images, -1, SQLITE_STATIC);
      sqlite3_bind_int(bind, 2, user_id);
      rc = sqlite3_step(bind);
      sqlite3_reset(bind);
      sqlite3_clear_bindings(bind);
      if (rc != SQLITE_DONE) {
         OLOG_ERROR("conv_db_add_row: binding row %lld's images failed: %s", (long long)*id_out,
                    sqlite3_errmsg(s_db.db));
         *id_out = 0;
         return AUTH_DB_FAILURE;
      }
   }
   return AUTH_DB_SUCCESS;
}

int conv_db_add_row(int64_t conv_id, int user_id, const conv_message_row_t *row, int64_t *id_out) {
   if (id_out)
      *id_out = 0;
   const char *blocks = NULL;
   const char *images = NULL;
   const int checked = row_check(conv_id, row, &blocks, &images);
   if (checked != AUTH_DB_SUCCESS) {
      return checked;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* A row naming images is one transaction with what records them. */
   const bool txn = images || (strcmp(row->role, "user") == 0 && strstr(row->content, "[IMAGE:"));
   if (txn && auth_db_txn_begin_locked("messages") != AUTH_DB_SUCCESS) {
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   const time_t now = time(NULL);
   int64_t id = 0;
   int result = msg_insert_locked(conv_id, user_id, row, now, &id);
   if (txn) {
      result = auth_db_txn_end_locked(result, "messages");
   }
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

int conv_db_add_rows(int64_t conv_id,
                     int user_id,
                     const conv_message_row_t *rows,
                     int n,
                     int64_t *ids_out) {
   if (n <= 0) {
      return AUTH_DB_SUCCESS;
   }
   if (!rows) {
      return AUTH_DB_INVALID;
   }
   int64_t *ids = ids_out ? ids_out : calloc((size_t)n, sizeof(*ids));
   if (!ids) {
      return AUTH_DB_FAILURE;
   }
   memset(ids, 0, (size_t)n * sizeof(*ids));
   for (int i = 0; i < n; i++) {
      const char *blocks = NULL;
      const char *images = NULL;
      const int checked = row_check(conv_id, &rows[i], &blocks, &images);
      if (checked != AUTH_DB_SUCCESS || rows[i].context_of_row < 0 || rows[i].context_of_row > i) {
         if (!ids_out) {
            free(ids);
         }
         return checked != AUTH_DB_SUCCESS ? checked : AUTH_DB_INVALID;
      }
   }

   AUTH_DB_LOCK_OR_FAIL();
   if (auth_db_txn_begin_locked("messages") != AUTH_DB_SUCCESS) {
      AUTH_DB_UNLOCK();
      if (!ids_out) {
         free(ids);
      }
      return AUTH_DB_FAILURE;
   }
   const time_t now = time(NULL);
   int result = AUTH_DB_SUCCESS;
   bool ordinary = false;
   for (int i = 0; i < n && result == AUTH_DB_SUCCESS; i++) {
      conv_message_row_t row = rows[i];
      if (row.context_of_row > 0) {
         row.context_of = ids[row.context_of_row - 1];
      }
      result = msg_insert_locked(conv_id, user_id, &row, now, &ids[i]);
      if (result == AUTH_DB_SUCCESS && message_kind_parse(row.kind) == MESSAGE_KIND_NONE) {
         ordinary = true;
         sqlite3_reset(s_db.stmt_conv_update_meta);
         sqlite3_bind_int64(s_db.stmt_conv_update_meta, 1, (int64_t)now);
         sqlite3_bind_int64(s_db.stmt_conv_update_meta, 2, conv_id);
         if (sqlite3_step(s_db.stmt_conv_update_meta) != SQLITE_DONE) {
            result = AUTH_DB_FAILURE;
         }
         sqlite3_reset(s_db.stmt_conv_update_meta);
      }
   }
   result = auth_db_txn_end_locked(result, "messages");
   if (result != AUTH_DB_SUCCESS) {
      memset(ids, 0, (size_t)n * sizeof(*ids));
   }
   AUTH_DB_UNLOCK();
   if (!ids_out) {
      free(ids);
   }
   if (result == AUTH_DB_SUCCESS && ordinary) {
      conversation_list_changed_notify(user_id, conv_id, CONV_LIST_CHANGE_BUMPED);
   }
   return result;
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
      row.images = (const char *)sqlite3_column_text(st, 10);
      char *stored = tool_change_blob_locked(&row);
      if (stored) {
         row.content = stored;
      }
      const int stop = callback(&row, ctx);
      free(stored);
      if (stop != 0)
         break;
   }

   sqlite3_reset(st);
   AUTH_DB_UNLOCK();

   return (rc == SQLITE_ROW || rc == SQLITE_DONE) ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

/* Clear stored blocks at or below @p watermark in @p conv_id, a batch per lock
 * hold.  They are already out of every reload's reach (reads take id >
 * watermark); this frees what they hold. */
void conv_db_clear_compacted_blocks(int64_t conv_id, int64_t watermark) {
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
   /* Stored as it is sent (a summary is replayed verbatim,
    * llm_history_summary_text): neutralized here, once. */
   char *safe = summary ? llm_context_neutralize(summary) : NULL;
   if (summary && !safe) {
      return AUTH_DB_FAILURE;
   }

   pthread_mutex_lock(&s_db.mutex);
   if (!s_db.initialized) {
      pthread_mutex_unlock(&s_db.mutex);
      free(safe);
      return AUTH_DB_FAILURE;
   }

   sqlite3_stmt *st = s_db.stmt_conv_set_watermark;
   sqlite3_reset(st);
   bind_text_or_null(st, 1, safe);
   sqlite3_bind_int64(st, 2, watermark_msg_id);
   sqlite3_bind_int64(st, 3, conv_id);
   sqlite3_bind_int(st, 4, user_id);
   sqlite3_bind_int64(st, 5, watermark_msg_id);
   const int rc = sqlite3_step(st);
   const int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(st);
   sqlite3_clear_bindings(st);

   AUTH_DB_UNLOCK();
   free(safe);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("conv_db_set_compaction_watermark: update failed");
      return AUTH_DB_FAILURE;
   }

   /* No change = missing, someone else's, or a stale pass the guard refused:
    * all benign, and nothing new is out of reach.  Otherwise the blocks the
    * watermark just passed have no reader left: clear them now, in batches
    * (conv_db_sweep_compacted_blocks catches any this misses). */
   if (changes > 0) {
      conv_db_clear_compacted_blocks(conv_id, watermark_msg_id);
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

/* The number @p sql (binding ?1 conv, ?2 user) gives; -1 on error.  Caller
 * holds the lock. */
static int64_t conv_count_locked(const char *sql, int64_t conv_id, int user_id) {
   sqlite3_stmt *st = NULL;
   int64_t n = -1;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, conv_id);
      sqlite3_bind_int(st, 2, user_id);
      if (sqlite3_step(st) == SQLITE_ROW) {
         n = sqlite3_column_int64(st, 0);
      }
   }
   sqlite3_finalize(st);
   return n;
}

int conv_db_bind_images(int64_t conv_id, int user_id, int *missing_out) {
   if (missing_out) {
      *missing_out = 0;
   }
   if (conv_id <= 0 || user_id <= 0) {
      return AUTH_DB_INVALID;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "UPDATE images SET retention_policy = 1 WHERE retention_policy = 3 "
                               "AND source = 5 AND user_id = ?2 AND id IN (SELECT r.image_id FROM "
                               "conversation_images r JOIN conversations c ON c.id = "
                               "r.conversation_id WHERE r.conversation_id = ?1 AND c.user_id = ?2)",
                               -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, conv_id);
      sqlite3_bind_int(st, 2, user_id);
      rc = sqlite3_step(st);
   }
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("conv_db_bind_images: conv %lld: %s", (long long)conv_id, sqlite3_errmsg(s_db.db));
   }
   sqlite3_finalize(st);
   /* Short: the ids its tool rows name that aren't the user's bound captures
    * (reads the rows holding images through idx_messages_images). */
   int64_t missing = 0;
   if (rc == SQLITE_DONE) {
      /* kind-rows: only tool rows hold images; a kind never goes on one. */
      missing = conv_count_locked(
          "SELECT COUNT(DISTINCT j.value) FROM messages m JOIN conversations c ON c.id = "
          "m.conversation_id, json_each(m.images) j WHERE m.conversation_id = ?1 AND "
          "c.user_id = ?2 AND m.images IS NOT NULL AND NOT EXISTS (SELECT 1 FROM images i "
          "WHERE i.id = j.value AND i.user_id = ?2 AND i.source = 5 AND "
          "i.retention_policy = 1)",
          conv_id, user_id);
   }
   AUTH_DB_UNLOCK();
   if (missing > 0) {
      OLOG_WARNING("conv_db_bind_images: conv %lld names %lld capture(s) no longer stored "
                   "(reclaimed unbound); saved without them",
                   (long long)conv_id, (long long)missing);
      if (missing_out) {
         *missing_out = (int)missing;
      }
   }
   return rc == SQLITE_DONE ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

void conv_image_files_free(conv_image_files_t *files) {
   if (!files) {
      return;
   }
   free(files->names);
   files->names = NULL;
   files->count = 0;
}

/* The images @p conv_id owns: only it names them, they're the owner's, and
 * they're bound (an unbound one is a running turn's or a save's to retry). */
#define OWNED_IMAGES_FROM                                                                  \
   "FROM conversation_images r JOIN images i ON i.id = r.image_id WHERE "                  \
   "r.conversation_id = ?1 AND i.user_id = ?2 AND i.retention_policy != 3 AND NOT EXISTS " \
   "(SELECT 1 FROM conversation_images o WHERE o.image_id = r.image_id AND "               \
   "o.conversation_id != ?1)"

/* Add @p name to @p files.  False on out of memory. */
static bool files_add(conv_image_files_t *files, const char *name) {
   char(*grown)[CONV_IMAGE_FILENAME_MAX] = realloc(files->names, (size_t)(files->count + 1) *
                                                                     sizeof(*files->names));
   if (!grown) {
      return false;
   }
   files->names = grown;
   safe_strncpy(files->names[files->count], name, CONV_IMAGE_FILENAME_MAX);
   files->count++;
   return true;
}

/* Step @p sql (binding ?1 conv, ?2 owner) to its end.  Caller holds the lock. */
static int exec_conv_locked(const char *sql, int64_t conv_id, int owner) {
   sqlite3_stmt *st = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, conv_id);
      sqlite3_bind_int(st, 2, owner);
      rc = sqlite3_step(st);
   }
   sqlite3_finalize(st);
   return rc == SQLITE_DONE ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

int conv_images_take_locked(int64_t conv_id,
                            int owner,
                            conv_images_mode_t mode,
                            conv_image_files_t *files) {
   if (mode == CONV_IMAGES_KEEP || owner <= 0) {
      return AUTH_DB_SUCCESS;
   }
   if (mode == CONV_IMAGES_UNBIND) {
      /* Its captures wait for the save's retry, or the grace sweep. */
      return exec_conv_locked("UPDATE images SET retention_policy = 3 WHERE source = 5 AND "
                              "retention_policy = 1 AND id IN (SELECT r.image_id " OWNED_IMAGES_FROM
                              ")",
                              conv_id, owner);
   }
   if (!files) {
      return AUTH_DB_FAILURE;
   }
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, "SELECT i.filename " OWNED_IMAGES_FROM, -1, &st, NULL) !=
       SQLITE_OK) {
      OLOG_ERROR("conv_db_delete: image select failed: %s", sqlite3_errmsg(s_db.db));
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, conv_id);
   sqlite3_bind_int(st, 2, owner);
   int rc;
   bool oom = false;
   while ((rc = sqlite3_step(st)) == SQLITE_ROW && !oom) {
      const char *name = (const char *)sqlite3_column_text(st, 0);
      oom = name && !files_add(files, name);
   }
   sqlite3_finalize(st);
   if (oom || rc != SQLITE_DONE) {
      return AUTH_DB_FAILURE;
   }
   return exec_conv_locked("DELETE FROM images WHERE id IN (SELECT r.image_id " OWNED_IMAGES_FROM
                           ")",
                           conv_id, owner);
}
