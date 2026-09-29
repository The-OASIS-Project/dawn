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
 * Schema migration v92: messages.llm_blocks, and a re-render of voice rows
 * saved as raw Claude block arrays.
 *
 * `llm_blocks` holds an assistant turn's blocks (text, tool calls, and the
 * reasoning a vendor issued for itself), read only to rebuild an LLM context.
 * A trigger drops them when the row's text or tool calls change, since they no
 * longer describe the row.
 *
 * Before this version the voice save wrote a Claude turn's content array as a
 * JSON string into `content`: thinking and signatures included, tool calls and
 * results as raw blocks. Those rows are re-rendered as the other writers store
 * a turn: the text, the tool calls in `tool_calls`, a single result as a tool
 * row, and reasoning dropped. A turn whose results can't be paired one-to-one
 * becomes text notes ("[Tool Call: name]", "[Tool Result: text]", the forms the
 * WebUI shows as tool entries, results on the assistant's side), so no call is
 * left without its result.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <json-c/json.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "logging.h"

/* A legacy row is re-rendered only if it parses, whole, as a block array within
 * this depth, so a user who said something bracketed is left alone. */
#define V92_JSON_DEPTH 32

static bool column_exists(sqlite3 *db, const char *table, const char *col) {
   char sql[128];
   snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
      return false;
   }
   bool found = false;
   while (sqlite3_step(st) == SQLITE_ROW) {
      const unsigned char *name = sqlite3_column_text(st, 1);
      if (name && strcmp((const char *)name, col) == 0) {
         found = true;
         break;
      }
   }
   sqlite3_finalize(st);
   return found;
}

static const char *block_type(json_object *block) {
   json_object *t = NULL;
   if (!json_object_is_type(block, json_type_object) ||
       !json_object_object_get_ex(block, "type", &t) || !json_object_is_type(t, json_type_string))
      return NULL;
   return json_object_get_string(t);
}

static bool is_one_of(const char *type, const char *const *types, size_t n) {
   for (size_t k = 0; type && k < n; k++) {
      if (strcmp(type, types[k]) == 0)
         return true;
   }
   return false;
}

static const char *const CLAUDE_TYPES[] = { "text",     "thinking",    "redacted_thinking",
                                            "tool_use", "tool_result", "image" };
/* What a user turn the voice save wrote could hold. */
static const char *const USER_TYPES[] = { "text", "tool_result", "image" };

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

/* The parsed array when @p content is, whole, a Claude content array the
 * voice save wrote, else NULL.  Every element is a block with a string type.
 * A user row holds only the types a user turn could; an assistant row may hold
 * types this doesn't know (they become notes), but at least one it does. */
static json_object *parse_block_array(const char *content, bool assistant) {
   json_tokener *tok = json_tokener_new_ex(V92_JSON_DEPTH);
   if (!tok)
      return NULL;
   const size_t len = strlen(content);
   json_object *arr = json_tokener_parse_ex(tok, content, (int)len);
   bool whole = json_tokener_get_error(tok) == json_tokener_success;
   for (size_t i = whole ? json_tokener_get_parse_end(tok) : len; whole && i < len; i++) {
      whole = content[i] == ' ' || content[i] == '\n' || content[i] == '\r' || content[i] == '\t';
   }
   json_tokener_free(tok);
   if (!whole || !json_object_is_type(arr, json_type_array) || json_object_array_length(arr) == 0) {
      json_object_put(arr);
      return NULL;
   }
   bool known = false;
   for (size_t i = 0; i < json_object_array_length(arr); i++) {
      const char *type = block_type(json_object_array_get_idx(arr, i));
      const bool ok = assistant ? type != NULL : is_one_of(type, USER_TYPES, COUNT_OF(USER_TYPES));
      if (!ok) {
         json_object_put(arr);
         return NULL;
      }
      known = known || is_one_of(type, CLAUDE_TYPES, COUNT_OF(CLAUDE_TYPES));
   }
   if (!known) {
      json_object_put(arr);
      return NULL;
   }
   return arr;
}

/* @p obj's @p key when it's a string, else NULL. */
static json_object *string_value(json_object *obj, const char *key) {
   json_object *v = NULL;
   return json_object_object_get_ex(obj, key, &v) && json_object_is_type(v, json_type_string)
              ? v
              : NULL;
}

/* A tool_result's text: a string, or the text parts of a content array. */
static void result_text(json_object *block, json_object *out_parts) {
   json_object *c = NULL;
   if (!json_object_object_get_ex(block, "content", &c))
      return;
   if (json_object_is_type(c, json_type_string)) {
      json_object_array_add(out_parts, json_object_get(c));
      return;
   }
   for (size_t i = 0; json_object_is_type(c, json_type_array) && i < json_object_array_length(c);
        i++) {
      json_object *part = json_object_array_get_idx(c, i);
      json_object *t = string_value(part, "text");
      const char *type = block_type(part);
      if (type && strcmp(type, "text") == 0 && t)
         json_object_array_add(out_parts, json_object_get(t));
   }
}

/* Append @p s to a growing string; false on OOM. */
static bool sb_add(char **buf, size_t *len, const char *s) {
   const size_t n = s ? strlen(s) : 0;
   char *grown = realloc(*buf, *len + n + 1);
   if (!grown)
      return false;
   memcpy(grown + *len, s ? s : "", n + 1);
   *buf = grown;
   *len += n;
   return true;
}

/* Join strings with a blank line between them. */
static char *join_parts(json_object *parts) {
   char *buf = NULL;
   size_t len = 0;
   if (!sb_add(&buf, &len, ""))
      return NULL;
   for (size_t i = 0; i < json_object_array_length(parts); i++) {
      if ((i > 0 && !sb_add(&buf, &len, "\n\n")) ||
          !sb_add(&buf, &len, json_object_get_string(json_object_array_get_idx(parts, i)))) {
         free(buf);
         return NULL;
      }
   }
   return buf;
}

/* The text a row keeps: its text blocks, plus a note for each call and result
 * that isn't carried in the tool columns. Thinking is dropped. */
static char *render_text(json_object *arr, bool calls_as_notes) {
   json_object *parts = json_object_new_array();
   if (!parts)
      return NULL;
   char note[512];
   for (size_t i = 0; i < json_object_array_length(arr); i++) {
      json_object *b = json_object_array_get_idx(arr, i);
      const char *type = block_type(b);
      json_object *v = NULL;
      if (strcmp(type, "text") == 0) {
         if ((v = string_value(b, "text")) != NULL)
            json_object_array_add(parts, json_object_get(v));
      } else if (strcmp(type, "tool_use") == 0) {
         if (calls_as_notes && (v = string_value(b, "name")) != NULL) {
            snprintf(note, sizeof(note), "[Tool Call: %s]", json_object_get_string(v));
            json_object_array_add(parts, json_object_new_string(note));
         }
      } else if (strcmp(type, "tool_result") == 0) {
         json_object *texts = json_object_new_array();
         result_text(b, texts);
         char *joined = texts ? join_parts(texts) : NULL;
         json_object_put(texts);
         if (joined && *joined) {
            char *wrapped = NULL;
            size_t len = 0;
            if (sb_add(&wrapped, &len, "[Tool Result: ") && sb_add(&wrapped, &len, joined) &&
                sb_add(&wrapped, &len, "]"))
               json_object_array_add(parts, json_object_new_string(wrapped));
            free(wrapped);
         }
         free(joined);
      } else if (strcmp(type, "image") == 0) {
         json_object_array_add(parts, json_object_new_string("[Image]"));
      } else if (strcmp(type, "thinking") != 0 && strcmp(type, "redacted_thinking") != 0) {
         /* A block type this doesn't render: a note, never its content. */
         snprintf(note, sizeof(note), "[%s]", type);
         json_object_array_add(parts, json_object_new_string(note));
      }
   }
   char *out = join_parts(parts);
   json_object_put(parts);
   return out;
}

/* The single block of @p type in @p arr, or NULL when there are none or several. */
static json_object *only_block_of(json_object *arr, const char *type) {
   json_object *found = NULL;
   for (size_t i = 0; i < json_object_array_length(arr); i++) {
      json_object *b = json_object_array_get_idx(arr, i);
      if (strcmp(block_type(b), type) == 0) {
         if (found)
            return NULL;
         found = b;
      }
   }
   return found;
}

static bool only_type(json_object *arr, const char *type) {
   for (size_t i = 0; i < json_object_array_length(arr); i++) {
      if (strcmp(block_type(json_object_array_get_idx(arr, i)), type) != 0)
         return false;
   }
   return true;
}

static bool has_block_of(json_object *arr, const char *type) {
   for (size_t i = 0; i < json_object_array_length(arr); i++) {
      if (strcmp(block_type(json_object_array_get_idx(arr, i)), type) == 0)
         return true;
   }
   return false;
}

static const char *str_field(json_object *obj, const char *key) {
   json_object *v = NULL;
   return json_object_object_get_ex(obj, key, &v) && json_object_is_type(v, json_type_string)
              ? json_object_get_string(v)
              : NULL;
}

/* The OpenAI tool_calls array for one tool_use block. */
static char *tool_calls_for(json_object *use) {
   json_object *input = NULL;
   json_object_object_get_ex(use, "input", &input);
   json_object *fn = json_object_new_object();
   json_object_object_add(fn, "name", json_object_new_string(str_field(use, "name")));
   json_object_object_add(fn, "arguments",
                          json_object_new_string(
                              input ? json_object_to_json_string_ext(input, JSON_C_TO_STRING_PLAIN)
                                    : "{}"));
   json_object *call = json_object_new_object();
   json_object_object_add(call, "id", json_object_new_string(str_field(use, "id")));
   json_object_object_add(call, "type", json_object_new_string("function"));
   json_object_object_add(call, "function", fn);
   json_object *calls = json_object_new_array();
   json_object_array_add(calls, call);
   char *out = strdup(json_object_to_json_string_ext(calls, JSON_C_TO_STRING_PLAIN));
   json_object_put(calls);
   return out;
}

static bool result_is_error(json_object *result) {
   json_object *v = NULL;
   return json_object_object_get_ex(result, "is_error", &v) && json_object_get_boolean(v);
}

/* A call and its only result, both well formed and naming the same id. */
static bool pairs_one_to_one(json_object *asst, json_object *next) {
   json_object *use = only_block_of(asst, "tool_use");
   if (!use || !next)
      return false;
   for (size_t i = 0; i < json_object_array_length(next); i++) {
      if (strcmp(block_type(json_object_array_get_idx(next, i)), "tool_result") != 0)
         return false;
   }
   json_object *result = only_block_of(next, "tool_result");
   const char *id = str_field(use, "id");
   const char *answers = result ? str_field(result, "tool_use_id") : NULL;
   return id && id[0] && str_field(use, "name") && answers && strcmp(id, answers) == 0;
}

static int apply_update(sqlite3_stmt *up,
                        int64_t id,
                        const char *role,
                        const char *content,
                        const char *tool_calls,
                        const char *tool_call_id,
                        bool is_error) {
   sqlite3_reset(up);
   sqlite3_bind_text(up, 1, role, -1, SQLITE_STATIC);
   sqlite3_bind_text(up, 2, content, -1, SQLITE_STATIC);
   if (tool_calls)
      sqlite3_bind_text(up, 3, tool_calls, -1, SQLITE_STATIC);
   else
      sqlite3_bind_null(up, 3);
   if (tool_call_id)
      sqlite3_bind_text(up, 4, tool_call_id, -1, SQLITE_STATIC);
   else
      sqlite3_bind_null(up, 4);
   sqlite3_bind_int(up, 5, is_error ? 1 : 0);
   sqlite3_bind_int64(up, 6, id);
   const int rc = sqlite3_step(up);
   sqlite3_reset(up);
   sqlite3_clear_bindings(up);
   return rc == SQLITE_DONE ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE;
}

/* One row and the row after it in the same conversation. Values copied, since
 * rows are rewritten after the scan. */
typedef struct {
   int64_t id;
   char role[CONV_ROLE_MAX];
   char *content;
   int64_t next_id;
   char *next_content; /* NULL unless the next row is a user row */
} v92_row_t;

static void rows_free(v92_row_t *rows, size_t n) {
   for (size_t i = 0; i < n; i++) {
      free(rows[i].content);
      free(rows[i].next_content);
   }
   free(rows);
}

/* Voice rows that may hold a raw block array, with the row after each, in
 * conversation order: a call's result row, when it is a candidate too, is the
 * next entry. */
static int collect_rows(sqlite3 *db, v92_row_t **rows_out, size_t *n_out) {
   *rows_out = NULL;
   *n_out = 0;
   sqlite3_stmt *st = NULL;
   const char *sql = "SELECT id, role, content, next_id, next_role, next_content FROM ("
                     "  SELECT m.conversation_id AS conv, m.id, m.role, m.content, "
                     "  LEAD(m.id) OVER w AS next_id, LEAD(m.role) OVER w AS next_role, "
                     "  LEAD(m.content) OVER w AS next_content "
                     "  FROM messages m JOIN conversations c ON c.id = m.conversation_id "
                     "  WHERE c.origin = 'voice' AND c.job_status IS NULL "
                     "  WINDOW w AS (PARTITION BY m.conversation_id ORDER BY m.id)) "
                     "WHERE role IN ('user', 'assistant') AND ltrim(content) LIKE '[%' "
                     "ORDER BY conv, id";
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v92 scan failed: %s", sqlite3_errmsg(db));
      return AUTH_DB_FAILURE;
   }
   v92_row_t *rows = NULL;
   size_t n = 0, cap = 0;
   int rc;
   while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
      if (n == cap) {
         const size_t grown = cap ? cap * 2 : 64;
         v92_row_t *bigger = realloc(rows, grown * sizeof(*rows));
         if (!bigger)
            break;
         rows = bigger;
         cap = grown;
      }
      v92_row_t *r = &rows[n];
      memset(r, 0, sizeof(*r));
      r->id = sqlite3_column_int64(st, 0);
      snprintf(r->role, sizeof(r->role), "%s", (const char *)sqlite3_column_text(st, 1));
      r->content = strdup((const char *)sqlite3_column_text(st, 2));
      r->next_id = sqlite3_column_int64(st, 3);
      const char *next_role = (const char *)sqlite3_column_text(st, 4);
      const char *next_content = (const char *)sqlite3_column_text(st, 5);
      if (next_role && strcmp(next_role, "user") == 0 && next_content)
         r->next_content = strdup(next_content);
      n++;
      if (!r->content ||
          (next_content && next_role && strcmp(next_role, "user") == 0 && !r->next_content)) {
         rc = SQLITE_NOMEM;
         break;
      }
   }
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("auth_db: v92 scan stopped early (%d)", rc);
      rows_free(rows, n);
      return AUTH_DB_FAILURE;
   }
   *rows_out = rows;
   *n_out = n;
   return AUTH_DB_SUCCESS;
}

/* Re-render one row (and, for a paired call, its result row). Sets
 * *paired_id to the result row it rewrote, else 0. */
static int rerender_row(sqlite3_stmt *up, const v92_row_t *r, int64_t *paired_id, int *count) {
   *paired_id = 0;
   const bool assistant = strcmp(r->role, "assistant") == 0;
   json_object *arr = parse_block_array(r->content, assistant);
   if (!arr)
      return AUTH_DB_SUCCESS; /* not a legacy block array */

   int result = AUTH_DB_SUCCESS;
   json_object *next = NULL;
   if (assistant && has_block_of(arr, "tool_use") && r->next_content)
      next = parse_block_array(r->next_content, false);

   if (assistant && pairs_one_to_one(arr, next)) {
      json_object *use = only_block_of(arr, "tool_use");
      json_object *res = only_block_of(next, "tool_result");
      char *text = render_text(arr, false);
      char *calls = tool_calls_for(use);
      json_object *texts = json_object_new_array();
      if (texts)
         result_text(res, texts);
      char *res_text = texts ? join_parts(texts) : NULL;
      json_object_put(texts);
      if (!text || !calls || !res_text ||
          apply_update(up, r->id, "assistant", text, calls, NULL, false) != AUTH_DB_SUCCESS ||
          apply_update(up, r->next_id, "tool", res_text, NULL, str_field(use, "id"),
                       result_is_error(res)) != AUTH_DB_SUCCESS) {
         result = AUTH_DB_FAILURE;
      } else {
         *paired_id = r->next_id;
         *count += 2;
      }
      free(text);
      free(calls);
      free(res_text);
   } else {
      char *text = render_text(arr, true);
      /* A turn with nothing to show (only thinking, say) still needs text: an
       * empty message is one a provider rejects on replay. */
      const char *shown = text && *text ? text : "[No text]";
      /* A user turn that only carried tool results becomes the assistant's
       * notes: tool output isn't something the user said (memory extraction
       * reads user turns as theirs, and a replay would put it where
       * instructions go). */
      const char *role = !assistant && only_type(arr, "tool_result") ? "assistant" : r->role;
      if (!text || apply_update(up, r->id, role, shown, NULL, NULL, false) != AUTH_DB_SUCCESS)
         result = AUTH_DB_FAILURE;
      else
         *count += 1;
      free(text);
   }
   json_object_put(next);
   json_object_put(arr);
   return result;
}

static int scrub_legacy_voice_rows(sqlite3 *db) {
   v92_row_t *rows = NULL;
   size_t n = 0;
   if (collect_rows(db, &rows, &n) != AUTH_DB_SUCCESS)
      return AUTH_DB_FAILURE;
   if (n == 0) {
      free(rows);
      return AUTH_DB_SUCCESS;
   }

   sqlite3_stmt *up = NULL;
   if (sqlite3_prepare_v2(db,
                          "UPDATE messages SET role = ?, content = ?, tool_calls = ?, "
                          "tool_call_id = ?, is_error = ? WHERE id = ?",
                          -1, &up, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v92 prepare failed: %s", sqlite3_errmsg(db));
      rows_free(rows, n);
      return AUTH_DB_FAILURE;
   }

   int result = AUTH_DB_SUCCESS;
   int rewritten = 0;
   int64_t skip_id = 0; /* a result row already rewritten with its call */
   for (size_t i = 0; i < n && result == AUTH_DB_SUCCESS; i++) {
      if (rows[i].id == skip_id)
         continue;
      result = rerender_row(up, &rows[i], &skip_id, &rewritten);
   }
   sqlite3_finalize(up);
   rows_free(rows, n);

   if (result == AUTH_DB_SUCCESS && rewritten > 0)
      OLOG_INFO("auth_db: v92 re-rendered %d voice message row(s) saved as raw blocks", rewritten);
   return result;
}

int auth_db_migrations_v92(sqlite3 *db) {
   if (!db)
      return AUTH_DB_FAILURE;

   /* The length column first: it must sit before the blocks, so reading it
    * never walks their overflow pages (ADD COLUMN appends). */
   static const struct {
      const char *name;
      const char *sql;
   } columns[] = {
      { "llm_blocks_len", "ALTER TABLE messages ADD COLUMN llm_blocks_len INTEGER" },
      { "llm_blocks",
        "ALTER TABLE messages ADD COLUMN llm_blocks TEXT " CONV_LLM_BLOCKS_CHECK_SQL },
   };
   for (size_t i = 0; i < COUNT_OF(columns); i++) {
      if (column_exists(db, "messages", columns[i].name))
         continue;
      char *errmsg = NULL;
      if (sqlite3_exec(db, columns[i].sql, NULL, NULL, &errmsg) != SQLITE_OK) {
         OLOG_ERROR("auth_db: v92 ALTER (%s) failed: %s", columns[i].name,
                    errmsg ? errmsg : "unknown");
         sqlite3_free(errmsg);
         return AUTH_DB_FAILURE;
      }
   }

   char *errmsg = NULL;
   /* Rows holding blocks, for the watermark GC and the cleanup sweep (few:
    * blocks below a watermark are cleared).  Here, not in the base schema: it
    * indexes a column a migration adds. */
   if (sqlite3_exec(db,
                    "CREATE INDEX IF NOT EXISTS idx_messages_llm_blocks ON messages "
                    "(conversation_id, id) WHERE llm_blocks_len IS NOT NULL",
                    NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v92 index failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }
   if (sqlite3_exec(db,
                    "CREATE TRIGGER IF NOT EXISTS messages_llm_blocks_on_edit "
                    "AFTER UPDATE OF content, tool_calls ON messages "
                    "WHEN NEW.llm_blocks_len IS NOT NULL AND "
                    "(NEW.content IS NOT OLD.content OR NEW.tool_calls IS NOT OLD.tool_calls) "
                    "BEGIN UPDATE messages SET llm_blocks = NULL, llm_blocks_len = NULL WHERE id = "
                    "NEW.id; END",
                    NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v92 trigger failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      return AUTH_DB_FAILURE;
   }

   if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v92 BEGIN failed: %s", sqlite3_errmsg(db));
      return AUTH_DB_FAILURE;
   }
   if (scrub_legacy_voice_rows(db) != AUTH_DB_SUCCESS) {
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      return AUTH_DB_FAILURE;
   }
   if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db: v92 COMMIT failed: %s", sqlite3_errmsg(db));
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      return AUTH_DB_FAILURE;
   }
   return AUTH_DB_SUCCESS;
}
