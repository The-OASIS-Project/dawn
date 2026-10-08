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
 * Unit tests for schema v94: request-context rows (messages.kind) that only
 * the replay read returns, the frozen prefix and tool set (prompt_blobs), the
 * reasoning floor, and stable memory citation handles.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "auth/auth_db_conv_prefix.h"
#include "auth/auth_db_focus_handles.h"
#include "auth/auth_db_internal.h"
#include "auth/auth_db_messages.h"
#include "auth/auth_db_withdraw.h"
#include "core/message_kind.h"
#include "test_tmp.h"
#include "unity.h"


static char TEST_DB[TEST_TMP_PATH_MAX];
static char OLD_DB[TEST_TMP_PATH_MAX];
static int alice_id = 0;
static int bob_id = 0;

void setUp(void) {
   unlink(TEST_DB);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(TEST_DB));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("alice", "h", true));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("bob", "h", false));
   auth_user_t u;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user("alice", &u));
   alice_id = u.id;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user("bob", &u));
   bob_id = u.id;
}

void tearDown(void) {
   auth_db_shutdown();
   unlink(TEST_DB);
   unlink(OLD_DB);
}

/* ---- helpers ---- */

static sqlite3 *raw_open(const char *path) {
   sqlite3 *db = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(path, &db));
   return db;
}

static int64_t raw_int(sqlite3 *db, const char *sql) {
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(db, sql, -1, &st, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
   int64_t v = sqlite3_column_int64(st, 0);
   sqlite3_finalize(st);
   return v;
}

static int64_t message_count(int64_t conv) {
   char sql[96];
   snprintf(sql, sizeof(sql), "SELECT message_count FROM conversations WHERE id = %lld",
            (long long)conv);
   sqlite3 *db = raw_open(TEST_DB);
   int64_t n = raw_int(db, sql);
   sqlite3_close(db);
   return n;
}

static int64_t add_row(int64_t conv, const char *role, const char *content, const char *kind) {
   const conv_message_row_t row = { .role = role, .content = content, .kind = kind };
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, alice_id, &row, &id));
   TEST_ASSERT_TRUE(id > 0);
   return id;
}

typedef struct {
   int count;
   bool saw_hidden;
} display_t;

static int collect_display(const conversation_message_t *msg, void *ctx) {
   display_t *out = ctx;
   out->count++;
   if (msg->content && strstr(msg->content, "HIDDEN"))
      out->saw_hidden = true;
   return 0;
}

typedef struct {
   int count;
   char kinds[8][24];
   int64_t context_of[8];
} replay_t;

static int collect_replay(const conversation_llm_row_t *row, void *ctx) {
   replay_t *out = ctx;
   if (out->count < 8) {
      snprintf(out->kinds[out->count], sizeof(out->kinds[0]), "%s", row->kind ? row->kind : "-");
      out->context_of[out->count] = row->context_of;
   }
   out->count++;
   return 0;
}

static int count_conv(const conversation_t *conv, void *ctx) {
   (void)conv;
   (*(int *)ctx)++;
   return 0;
}

/* A conversation with: user, turn_context (hidden), memory (hidden),
 * assistant, directive (hidden). */
static int64_t conv_with_kind_rows(int64_t *first, int64_t *last_visible, int64_t *last) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   *first = add_row(conv, "user", "What's up?", NULL);
   add_row(conv, "user", "HIDDEN turn context", "turn_context");
   add_row(conv, "user", "HIDDEN memory", "memory");
   *last_visible = add_row(conv, "assistant", "Not much.", NULL);
   *last = add_row(conv, "system", "HIDDEN directive", "directive");
   return conv;
}

/* ---- request-context rows ---- */

static void test_display_reads_skip_kind_rows(void) {
   int64_t first = 0, last_visible = 0, last = 0;
   const int64_t conv = conv_with_kind_rows(&first, &last_visible, &last);

   display_t shown = { 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages(conv, alice_id, collect_display, &shown));
   TEST_ASSERT_EQUAL_INT(2, shown.count);
   TEST_ASSERT_FALSE(shown.saw_hidden);

   shown = (display_t){ 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_after(conv, alice_id, 0, collect_display, &shown));
   TEST_ASSERT_EQUAL_INT(2, shown.count);
   TEST_ASSERT_FALSE(shown.saw_hidden);

   shown = (display_t){ 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_admin(conv, collect_display, &shown));
   TEST_ASSERT_EQUAL_INT(2, shown.count);
   TEST_ASSERT_FALSE(shown.saw_hidden);

   shown = (display_t){ 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_by_range(conv, alice_id, first, last, 0, true,
                                                       collect_display, &shown));
   TEST_ASSERT_EQUAL_INT(2, shown.count);
   TEST_ASSERT_FALSE(shown.saw_hidden);

   int hits = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_search_content(alice_id, "HIDDEN", NULL, count_conv, &hits));
   TEST_ASSERT_EQUAL_INT(0, hits);

   int64_t max_id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_get_max_msg_id(conv, alice_id, &max_id));
   TEST_ASSERT_EQUAL_INT64(last_visible, max_id);
}

static void test_replay_read_returns_kind_rows_in_order(void) {
   int64_t first = 0, last_visible = 0, last = 0;
   const int64_t conv = conv_with_kind_rows(&first, &last_visible, &last);

   replay_t rows = { 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_for_llm(conv, alice_id, 0, collect_replay, &rows));
   TEST_ASSERT_EQUAL_INT(5, rows.count);
   TEST_ASSERT_EQUAL_STRING("-", rows.kinds[0]);
   TEST_ASSERT_EQUAL_STRING("turn_context", rows.kinds[1]);
   TEST_ASSERT_EQUAL_STRING("memory", rows.kinds[2]);
   TEST_ASSERT_EQUAL_STRING("-", rows.kinds[3]);
   TEST_ASSERT_EQUAL_STRING("directive", rows.kinds[4]);
}

static void test_kind_rows_do_not_count_as_messages(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   add_row(conv, "user", "Hi", NULL);
   TEST_ASSERT_EQUAL_INT64(1, message_count(conv));
   add_row(conv, "user", "HIDDEN", "turn_context");
   add_row(conv, "system", "HIDDEN", "instruction");
   TEST_ASSERT_EQUAL_INT64(1, message_count(conv));
}

/* Rows written together: a context row names its question by its place in the
 * batch; a bad row saves nothing; another user's conversation takes none. */
static void test_rows_are_saved_together(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   const conv_message_row_t rows[] = {
      { .role = "user", .content = "Q" },
      { .role = "user", .content = "ctx", .kind = "turn_context", .context_of_row = 1 },
      { .role = "assistant", .content = "A" },
   };
   int64_t ids[3] = { 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_rows(conv, alice_id, rows, 3, ids));
   TEST_ASSERT_TRUE(ids[0] > 0 && ids[1] > ids[0] && ids[2] > ids[1]);
   TEST_ASSERT_EQUAL_INT64(2, message_count(conv));
   char sql[96];
   snprintf(sql, sizeof(sql), "SELECT context_of FROM messages WHERE id = %lld", (long long)ids[1]);
   sqlite3 *db = raw_open(TEST_DB);
   TEST_ASSERT_EQUAL_INT64(ids[0], raw_int(db, sql));
   sqlite3_close(db);

   /* A kind the role can't carry: the whole batch is refused. */
   const conv_message_row_t bad[] = {
      { .role = "user", .content = "Q2" },
      { .role = "assistant", .content = "x", .kind = "turn_context" },
   };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, conv_db_add_rows(conv, alice_id, bad, 2, NULL));
   /* A question later in the batch than its context. */
   const conv_message_row_t ahead[] = {
      { .role = "user", .content = "ctx", .kind = "turn_context", .context_of_row = 2 },
      { .role = "user", .content = "Q3" },
   };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, conv_db_add_rows(conv, alice_id, ahead, 2, NULL));
   /* Someone else's conversation: nothing is written. */
   int64_t no_ids[1] = { 0 };
   TEST_ASSERT_NOT_EQUAL(AUTH_DB_SUCCESS, conv_db_add_rows(conv, bob_id, rows, 1, no_ids));
   TEST_ASSERT_EQUAL_INT64(0, no_ids[0]);
   TEST_ASSERT_EQUAL_INT64(2, message_count(conv));
}

static void test_kind_must_match_role(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   const struct {
      const char *role;
      const char *kind;
   } bad[] = {
      { "system", "turn_context" },    { "user", "directive" },   { "tool", "memory" },
      { "assistant", "instruction" },  { "user", "nonsense" },    { "user", "prefix" },
      { "assistant", "turn_context" }, { "assistant", "memory" }, { "assistant", "envelope" },
   };
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
      const conv_message_row_t row = { .role = bad[i].role, .content = "x", .kind = bad[i].kind };
      TEST_ASSERT_EQUAL_INT_MESSAGE(AUTH_DB_INVALID, conv_db_add_row(conv, alice_id, &row, NULL),
                                    bad[i].kind);
   }
   /* The schema refuses them too, for any writer that skips the API. */
   char sql[160];
   snprintf(sql, sizeof(sql),
            "INSERT INTO messages (conversation_id, role, content, kind, created_at) "
            "VALUES (%lld, 'system', 'x', 'turn_context', 1)",
            (long long)conv);
   sqlite3 *db = raw_open(TEST_DB);
   TEST_ASSERT_NOT_EQUAL(SQLITE_OK, sqlite3_exec(db, sql, NULL, NULL, NULL));
   sqlite3_close(db);
}

/* ---- the migration ---- */

static void test_v94_migrates_a_v93_database(void) {
   unlink(OLD_DB);
   sqlite3 *db = raw_open(OLD_DB);
   TEST_ASSERT_EQUAL_INT(SQLITE_OK,
                         sqlite3_exec(db,
                                      "CREATE TABLE conversations (id INTEGER PRIMARY KEY, "
                                      "user_id INTEGER NOT NULL);"
                                      "CREATE TABLE messages (id INTEGER PRIMARY KEY, "
                                      "conversation_id INTEGER NOT NULL, role TEXT NOT NULL, "
                                      "content TEXT NOT NULL);"
                                      "INSERT INTO conversations VALUES (1, 1);"
                                      "INSERT INTO messages VALUES (1, 1, 'user', 'kept');"
                                      "CREATE TABLE documents (id INTEGER PRIMARY KEY, user_id "
                                      "INTEGER, is_global INTEGER DEFAULT 0);"
                                      "CREATE TABLE doc_chunk_generation (owner INTEGER PRIMARY "
                                      "KEY, gen INTEGER NOT NULL DEFAULT 0);"
                                      "CREATE TABLE document_chunks (id INTEGER PRIMARY KEY, "
                                      "document_id INTEGER NOT NULL, chunk_index INTEGER NOT "
                                      "NULL, text TEXT NOT NULL, embedding BLOB NOT NULL, "
                                      "embedding_norm REAL NOT NULL, created_at INTEGER NOT NULL "
                                      "DEFAULT 0);"
                                      "INSERT INTO documents VALUES (1, 1, 0);"
                                      "INSERT INTO document_chunks VALUES (7, 1, 0, 'c', x'00', "
                                      "1.0, 5);"
                                      /* a chunk whose document is gone */
                                      "INSERT INTO document_chunks VALUES (9, 99, 0, 'o', x'00', "
                                      "1.0, 5);",
                                      NULL, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_migrations_v94(db));
   /* Idempotent: a re-run after a partial failure finds the columns. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_migrations_v94(db));

   TEST_ASSERT_EQUAL_INT64(0, raw_int(db, "SELECT reasoning_floor_msg_id FROM conversations"));
   TEST_ASSERT_EQUAL_INT64(1, raw_int(db, "SELECT COUNT(*) FROM messages WHERE kind IS NULL "
                                          "AND context_of IS NULL"));
   TEST_ASSERT_EQUAL_INT64(1, raw_int(db, "SELECT COUNT(*) FROM conversations "
                                          "WHERE prefix_hash IS NULL AND tools_hash IS NULL"));
   TEST_ASSERT_EQUAL_INT64(4, raw_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE name IN "
                                          "('idx_messages_display', 'idx_conversations_prefix', "
                                          "'idx_conversations_tools', "
                                          "'idx_conversations_in_force')"));
   /* document_chunks rebuilt: same rows and ids, its triggers back, and a
    * deleted id never handed out again. */
   TEST_ASSERT_EQUAL_INT64(1, raw_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE name = "
                                          "'document_chunks' AND sql LIKE '%AUTOINCREMENT%'"));
   TEST_ASSERT_EQUAL_INT64(7, raw_int(db, "SELECT id FROM document_chunks WHERE text = 'c'"));
   TEST_ASSERT_EQUAL_INT64(1,
                           raw_int(db, "SELECT COUNT(*) FROM document_chunks")); /* orphan left */
   TEST_ASSERT_EQUAL_INT64(3, raw_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type = "
                                          "'trigger' AND tbl_name = 'document_chunks'"));
   TEST_ASSERT_EQUAL_INT(SQLITE_OK,
                         sqlite3_exec(db,
                                      "DELETE FROM document_chunks; INSERT INTO document_chunks "
                                      "(document_id, chunk_index, text, embedding, "
                                      "embedding_norm) VALUES (1, 0, 'd', x'00', 1.0);",
                                      NULL, NULL, NULL));
   /* Past the orphan's id (9) too. */
   TEST_ASSERT_EQUAL_INT64(10, raw_int(db, "SELECT id FROM document_chunks"));
   TEST_ASSERT_EQUAL_INT(SQLITE_OK,
                         sqlite3_exec(db,
                                      "INSERT INTO messages (conversation_id, role, content, "
                                      "kind) VALUES (1, 'user', 'x', 'memory')",
                                      NULL, NULL, NULL));
   TEST_ASSERT_NOT_EQUAL(SQLITE_OK, sqlite3_exec(db,
                                                 "INSERT INTO messages (conversation_id, role, "
                                                 "content, kind) VALUES (1, 'user', 'x', 'bogus')",
                                                 NULL, NULL, NULL));
   sqlite3_close(db);
}

/* ---- a turn's save: rows, prefix, floor ---- */

/* Save a prefix (and tools) alone, as a turn with no rows. */
static int save_prefix(int64_t conv, int user, const char *prefix, const char *tools) {
   const conv_turn_save_t save = { .prefix = prefix, .tools = tools };
   return conv_db_save_turn(conv, user, &save, NULL);
}

static void test_turn_save_stores_prefix_and_anchored_rows(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   conv_prefix_t p;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_prefix_get(conv, alice_id, &p));
   TEST_ASSERT_NULL(p.prefix);
   TEST_ASSERT_NULL(p.tools);
   TEST_ASSERT_EQUAL_INT64(0, p.reasoning_floor_msg_id);
   conv_prefix_free(&p);

   const int64_t q = add_row(conv, "user", "What's up?", NULL);
   /* Another writer's row lands between the question and its context. */
   add_row(conv, "assistant", "Interleaved.", NULL);
   const conv_message_row_t rows[] = {
      { .role = "user", .content = "HIDDEN memory", .kind = "memory", .context_of = q },
      { .role = "user", .content = "HIDDEN context", .kind = "turn_context", .context_of = q },
      { .role = "system", .content = "HIDDEN directive", .kind = "directive", .context_of = q },
   };
   int64_t ids[3] = { 0 };
   const conv_turn_save_t save = { .rows = rows,
                                   .n_rows = 3,
                                   .prefix = "You are helpful.",
                                   .tools = "[\"a\"]",
                                   .in_force = "{\"directives\":\"d\"}" };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_save_turn(conv, alice_id, &save, ids));
   TEST_ASSERT_TRUE(ids[0] > q && ids[1] > ids[0] && ids[2] > ids[1]);
   TEST_ASSERT_EQUAL_INT64(2, message_count(conv)); /* request context counts as nothing */

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_prefix_get(conv, alice_id, &p));
   TEST_ASSERT_EQUAL_STRING("You are helpful.", p.prefix);
   TEST_ASSERT_EQUAL_STRING("[\"a\"]", p.tools);
   TEST_ASSERT_EQUAL_STRING("{\"directives\":\"d\"}", p.in_force);
   TEST_ASSERT_EQUAL_INT(DAWN_SHA256_HEX_LEN - 1, (int)strlen(p.prefix_hash));
   conv_prefix_free(&p);

   replay_t seen = { 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_for_llm(conv, alice_id, 0, collect_replay, &seen));
   TEST_ASSERT_EQUAL_INT(5, seen.count);
   TEST_ASSERT_EQUAL_INT64(q, seen.context_of[2]);
   TEST_ASSERT_EQUAL_INT64(q, seen.context_of[4]);
   TEST_ASSERT_EQUAL_INT64(0, seen.context_of[0]);

   /* Saving the same prefix again stores nothing new. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         save_prefix(conv, alice_id, "You are helpful.", "[\"a\"]"));
   sqlite3 *db = raw_open(TEST_DB);
   TEST_ASSERT_EQUAL_INT64(3, raw_int(db, "SELECT COUNT(*) FROM prompt_blobs"));
   sqlite3_close(db);
}

/* A row naming no question of this conversation fails the whole save. */
static void test_turn_save_is_all_or_nothing(void) {
   int64_t conv = 0, other = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "o", &other));
   const int64_t q = add_row(conv, "user", "Q", NULL);
   const int64_t reply = add_row(conv, "assistant", "A", NULL);
   const int64_t elsewhere = add_row(other, "user", "Q2", NULL);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, save_prefix(conv, alice_id, "kept", NULL));

   const int64_t bad_anchor[] = { elsewhere, reply };
   for (size_t i = 0; i < sizeof(bad_anchor) / sizeof(bad_anchor[0]); i++) {
      const conv_message_row_t rows[] = {
         { .role = "user", .content = "ok", .kind = "turn_context", .context_of = q },
         { .role = "user", .content = "bad", .kind = "memory", .context_of = bad_anchor[i] },
      };
      const conv_turn_save_t save = { .rows = rows, .n_rows = 2, .prefix = "changed" };
      int64_t ids[2] = { 7, 7 };
      TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, conv_db_save_turn(conv, alice_id, &save, ids));
      TEST_ASSERT_EQUAL_INT64(0, ids[0]);
   }
   replay_t seen = { 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_for_llm(conv, alice_id, 0, collect_replay, &seen));
   TEST_ASSERT_EQUAL_INT(2, seen.count);
   conv_prefix_t p;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_prefix_get(conv, alice_id, &p));
   TEST_ASSERT_EQUAL_STRING("kept", p.prefix);
   conv_prefix_free(&p);
}

static void test_turn_save_checks_the_owner_and_input(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   conv_prefix_t p;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, conv_db_prefix_get(conv, bob_id, &p));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, save_prefix(conv, bob_id, "x", NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, save_prefix(conv, alice_id, "", NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, save_prefix(conv, alice_id, "x", ""));
   /* A message someone sees is no request context. */
   const conv_message_row_t plain = { .role = "user", .content = "hi" };
   const conv_turn_save_t save = { .rows = &plain, .n_rows = 1 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, conv_db_save_turn(conv, alice_id, &save, NULL));
   /* Nor is an ordinary row allowed to name a question. */
   const conv_message_row_t anchored = { .role = "user", .content = "hi", .context_of = 1 };
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, conv_db_add_row(conv, alice_id, &anchored, &id));

   char *big = malloc(CONV_PROMPT_BLOB_MAX + 2);
   TEST_ASSERT_NOT_NULL(big);
   memset(big, 'a', CONV_PROMPT_BLOB_MAX + 1);
   big[CONV_PROMPT_BLOB_MAX + 1] = '\0';
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, save_prefix(conv, alice_id, big, NULL));
   free(big);
}

static void test_turn_save_moves_the_prefix_and_raises_the_floor(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, save_prefix(conv, alice_id, "old", NULL));
   conv_turn_save_t save = { .prefix = "new", .tools = "[]", .floor_msg_id = 40 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_save_turn(conv, alice_id, &save, NULL));
   /* A lower boundary never lowers the floor. */
   save.floor_msg_id = 20;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_save_turn(conv, alice_id, &save, NULL));

   conv_prefix_t p;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_prefix_get(conv, alice_id, &p));
   TEST_ASSERT_EQUAL_STRING("new", p.prefix);
   TEST_ASSERT_EQUAL_STRING("[]", p.tools);
   TEST_ASSERT_EQUAL_INT64(40, p.reasoning_floor_msg_id);
   conv_prefix_free(&p);
}

typedef struct {
   int count;
   bool has_blocks[4];
} blocks_seen_t;

static int collect_blocks(const conversation_llm_row_t *row, void *ctx) {
   blocks_seen_t *out = ctx;
   if (out->count < 4)
      out->has_blocks[out->count] = row->llm_blocks != NULL;
   out->count++;
   return 0;
}

/* Blocks at or below the floor never load; the rows still do, as text. */
static void test_replay_read_honors_the_reasoning_floor(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   int64_t ids[3] = { 0 };
   for (int i = 0; i < 3; i++) {
      const conv_message_row_t row = { .role = "assistant", .content = "a", .llm_blocks = "{}" };
      TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, alice_id, &row, &ids[i]));
   }
   const conv_turn_save_t save = { .floor_msg_id = ids[1] };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_save_turn(conv, alice_id, &save, NULL));
   blocks_seen_t seen = { 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_for_llm(conv, alice_id, 0, collect_blocks, &seen));
   TEST_ASSERT_EQUAL_INT(3, seen.count);
   TEST_ASSERT_FALSE(seen.has_blocks[0]);
   TEST_ASSERT_FALSE(seen.has_blocks[1]);
   TEST_ASSERT_TRUE(seen.has_blocks[2]);
}

static void test_gc_keeps_what_a_conversation_uses(void) {
   int64_t a = 0, b = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "a", &a));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "b", &b));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, save_prefix(a, alice_id, "shared", NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, save_prefix(b, alice_id, "shared", "tools-b"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, save_prefix(b, alice_id, "moved", NULL));

   int deleted = -1;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_prompt_blobs_gc(&deleted));
   TEST_ASSERT_EQUAL_INT(1, deleted); /* tools-b */

   conv_prefix_t p;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_prefix_get(a, alice_id, &p));
   TEST_ASSERT_EQUAL_STRING("shared", p.prefix);
   conv_prefix_free(&p);

   /* Deleting a conversation takes its frozen prompt with it, then and there. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_delete(a, alice_id));
   sqlite3 *db = raw_open(TEST_DB);
   TEST_ASSERT_EQUAL_INT64(0,
                           raw_int(db, "SELECT COUNT(*) FROM prompt_blobs WHERE bytes = 'shared'"));
   sqlite3_close(db);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_prompt_blobs_gc(&deleted));
   TEST_ASSERT_EQUAL_INT(0, deleted);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_prefix_get(b, alice_id, &p));
   TEST_ASSERT_EQUAL_STRING("moved", p.prefix);
   conv_prefix_free(&p);
}

static int keep_content(const conversation_llm_row_t *row, void *ctx) {
   char **out = ctx;
   if (row->kind && strcmp(row->kind, "tool_change") == 0) {
      free(*out);
      *out = strdup(row->content);
   }
   return 0;
}

/* A large tool change's definitions are stored by hash (the row names them),
 * read back whole, kept by the collector while a row names them, and gone
 * with the conversation. */
static void test_a_large_tool_change_is_stored_by_hash(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   add_row(conv, "user", "Q", NULL);
   char *big = malloc(12000);
   TEST_ASSERT_NOT_NULL(big);
   int n = snprintf(big, 12000,
                    "{\"rendered\":\"inline\",\"tools\":[{\"name\":\"t\","
                    "\"description\":\"");
   memset(big + n, 'x', 10000);
   snprintf(big + n + 10000, 12000 - (size_t)n - 10000, "\",\"parameters\":{}}]}");
   add_row(conv, "system", big, "tool_change");

   sqlite3 *db = raw_open(TEST_DB);
   TEST_ASSERT_EQUAL_INT64(1, raw_int(db, "SELECT COUNT(*) FROM messages WHERE kind = "
                                          "'tool_change' AND content LIKE '{\"blob\":%'"));
   sqlite3_close(db);
   char *read = NULL;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_for_llm(conv, alice_id, 0, keep_content, &read));
   TEST_ASSERT_EQUAL_STRING(big, read);
   free(read);

   int deleted = -1;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_prompt_blobs_gc(&deleted));
   TEST_ASSERT_EQUAL_INT(0, deleted);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_delete(conv, alice_id));
   db = raw_open(TEST_DB);
   char sql[64];
   snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM prompt_blobs WHERE length(bytes) > 9000");
   TEST_ASSERT_EQUAL_INT64(0, raw_int(db, sql));
   sqlite3_close(db);
   free(big);
}

static void test_altered_prefix_bytes_are_refused(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, save_prefix(conv, alice_id, "original", NULL));
   sqlite3 *db = raw_open(TEST_DB);
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(db, "UPDATE prompt_blobs SET bytes = 'altered'",
                                                 NULL, NULL, NULL));
   sqlite3_close(db);
   conv_prefix_t p;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, conv_db_prefix_get(conv, alice_id, &p));
   TEST_ASSERT_NULL(p.prefix);
   /* The next turn's prefix replaces it. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, save_prefix(conv, alice_id, "renewed", NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_prefix_get(conv, alice_id, &p));
   TEST_ASSERT_EQUAL_STRING("renewed", p.prefix);
   conv_prefix_free(&p);
}

/* ---- withdrawing what the user forgot ---- */

static void test_a_forgotten_item_is_withdrawn(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   const int64_t q = add_row(conv, "user", "Q", NULL);
   const conv_message_row_t ctx = { .role = "user",
                                    .content = "--- TURN CONTEXT (t) ---\n"
                                               "[M1 memory_fact] a secret\n"
                                               "--- END TURN CONTEXT (t) ---\n",
                                    .kind = "turn_context",
                                    .context_of = q };
   int64_t ctx_id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, alice_id, &ctx, &ctx_id));
   const int64_t reply = add_row(conv, "assistant", "A", NULL);
   /* The item was injected as [M1], and the user has since removed it: a
    * delete on DAWN's connection while the removal is marked records it. */
   char fact_sql[160];
   snprintf(fact_sql, sizeof(fact_sql),
            "INSERT INTO memory_facts (user_id, fact_text) VALUES (%d, 'a secret')", alice_id);
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(s_db.db, fact_sql, NULL, NULL, NULL));
   const int64_t fact_id = sqlite3_last_insert_rowid(s_db.db);
   char item_id[32];
   snprintf(item_id, sizeof(item_id), "fact:%lld", (long long)fact_id);
   conv_focus_handle_t item = { .source = "memory_fact", .item_id = item_id };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_focus_handles_assign(conv, alice_id, &item, 1));
   TEST_ASSERT_EQUAL_INT(1, item.handle);
   snprintf(fact_sql, sizeof(fact_sql), "DELETE FROM memory_facts WHERE id = %lld",
            (long long)fact_id);
   conv_db_withdraw_intent_begin(alice_id);
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(s_db.db, fact_sql, NULL, NULL, NULL));
   conv_db_withdraw_intent_end();

   conv_withdrawn_t w;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_withdraw(alice_id, false, &w));
   TEST_ASSERT_EQUAL_INT(1, w.n_items);
   TEST_ASSERT_EQUAL_INT(1, w.n_convs);
   conv_withdrawn_free(&w);

   sqlite3 *db = raw_open(TEST_DB);
   char sql[160];
   snprintf(sql, sizeof(sql), "SELECT instr(content, 'a secret') FROM messages WHERE id = %lld",
            (long long)ctx_id);
   TEST_ASSERT_EQUAL_INT64(0, raw_int(db, sql));
   snprintf(sql, sizeof(sql), "SELECT reasoning_floor_msg_id FROM conversations WHERE id = %lld",
            (long long)conv);
   TEST_ASSERT_EQUAL_INT64(reply, raw_int(db, sql)); /* a boundary at its newest row */
   snprintf(sql, sizeof(sql),
            "SELECT reasoning_floor_pending > 0 FROM conversations "
            "WHERE id = %lld",
            (long long)conv);
   TEST_ASSERT_EQUAL_INT64(1, raw_int(db, sql)); /* and past it, until a later turn */
   sqlite3_close(db);

   /* A turn built after the withdrawal settles the floor at its question. */
   const int64_t q2 = add_row(conv, "user", "Q2", NULL);
   int64_t seq = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_withdraw_seq(&seq));
   const conv_turn_save_t save = { .question_id = q2,
                                   .built_at = (int64_t)time(NULL),
                                   .built_seq = seq };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_save_turn(conv, alice_id, &save, NULL));
   db = raw_open(TEST_DB);
   snprintf(sql, sizeof(sql), "SELECT reasoning_floor_pending FROM conversations WHERE id = %lld",
            (long long)conv);
   TEST_ASSERT_EQUAL_INT64(0, raw_int(db, sql));
   snprintf(sql, sizeof(sql), "SELECT reasoning_floor_msg_id FROM conversations WHERE id = %lld",
            (long long)conv);
   TEST_ASSERT_EQUAL_INT64(q2, raw_int(db, sql));
   sqlite3_close(db);

   /* Once only; and another user's sweep touches nothing of Alice's. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_withdraw(alice_id, false, &w));
   TEST_ASSERT_EQUAL_INT(0, w.n_items);
   conv_withdrawn_free(&w);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_withdraw(bob_id, true, &w));
   TEST_ASSERT_EQUAL_INT(0, w.n_convs);
   conv_withdrawn_free(&w);
}

/* A delete nobody marked as the user's removal (decay, a merge, re-indexing,
 * the sqlite3 shell) records nothing to withdraw. */
static void test_an_unmarked_delete_records_nothing(void) {
   TEST_ASSERT_EQUAL_INT(SQLITE_OK,
                         sqlite3_exec(s_db.db,
                                      "INSERT INTO memory_facts (user_id, fact_text) VALUES "
                                      "(1, 'x'); DELETE FROM memory_facts;",
                                      NULL, NULL, NULL));
   sqlite3 *db = raw_open(TEST_DB);
   TEST_ASSERT_EQUAL_INT64(0, raw_int(db, "SELECT COUNT(*) FROM withdrawn_items"));
   sqlite3_close(db);
}

/* A chunk's id is never reused (v94: AUTOINCREMENT), so a removed chunk's
 * row can't name a new one. */
static void test_chunk_ids_are_never_reused(void) {
   sqlite3 *db = raw_open(TEST_DB);
   TEST_ASSERT_EQUAL_INT64(1, raw_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE "
                                          "name = 'document_chunks' AND sql LIKE "
                                          "'%AUTOINCREMENT%'"));
   sqlite3_close(db);
}

static void test_memory_blocks_are_withdrawn(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   const int64_t q = add_row(conv, "user", "Q", NULL);
   const conv_message_row_t mem = { .role = "user",
                                    .content = "--- USER MEMORY (t) ---\n- food: tacos\n"
                                               "--- END USER MEMORY (t) ---\n",
                                    .kind = "memory",
                                    .context_of = q };
   int64_t mem_id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, alice_id, &mem, &mem_id));
   conv_withdrawn_t w;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_withdraw(alice_id, true, &w));
   TEST_ASSERT_EQUAL_INT(1, w.n_convs);
   conv_withdrawn_free(&w);
   sqlite3 *db = raw_open(TEST_DB);
   char sql[160];
   snprintf(sql, sizeof(sql), "SELECT instr(content, 'tacos') FROM messages WHERE id = %lld",
            (long long)mem_id);
   TEST_ASSERT_EQUAL_INT64(0, raw_int(db, sql));
   sqlite3_close(db);
}

/* A floor pending on a withdrawal still settles after the purge has removed
 * that withdrawal's rows: the sequence never goes backwards. */
static void test_a_purged_withdrawal_still_settles(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   const int64_t q = add_row(conv, "user", "Q", NULL);
   const conv_message_row_t mem = { .role = "user",
                                    .content = "--- USER MEMORY (t) ---\n- pet: cat\n"
                                               "--- END USER MEMORY (t) ---\n",
                                    .kind = "memory",
                                    .context_of = q };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, alice_id, &mem, NULL));
   add_row(conv, "assistant", "A", NULL);
   conv_withdrawn_t w;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_withdraw(alice_id, true, &w));
   conv_withdrawn_free(&w);
   TEST_ASSERT_EQUAL_INT(
       SQLITE_OK, sqlite3_exec(s_db.db, "UPDATE withdrawn_items SET created_at = 0, delivered = 1",
                               NULL, NULL, NULL));
   int deleted = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_withdrawn_items_purge(&deleted));
   TEST_ASSERT_TRUE(deleted > 0);

   const int64_t q2 = add_row(conv, "user", "Q2", NULL);
   int64_t seq = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_withdraw_seq(&seq));
   const conv_turn_save_t save = { .question_id = q2,
                                   .built_at = (int64_t)time(NULL),
                                   .built_seq = seq };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_save_turn(conv, alice_id, &save, NULL));
   sqlite3 *db = raw_open(TEST_DB);
   char sql[160];
   snprintf(sql, sizeof(sql), "SELECT reasoning_floor_pending FROM conversations WHERE id = %lld",
            (long long)conv);
   TEST_ASSERT_EQUAL_INT64(0, raw_int(db, sql));
   sqlite3_close(db);
}

/* Two sessions in one conversation: a turn built after a withdrawal settles
 * the floor first, then one built before it saves.  Its reasoning, bound to
 * the history before the scrub, goes behind the floor again. */
static void test_a_turn_built_before_a_withdrawal_saves_behind_it(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   const int64_t q = add_row(conv, "user", "Q", NULL);
   const conv_message_row_t mem = { .role = "user",
                                    .content = "--- USER MEMORY (t) ---\n- car: red\n"
                                               "--- END USER MEMORY (t) ---\n",
                                    .kind = "memory",
                                    .context_of = q };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, alice_id, &mem, NULL));
   add_row(conv, "assistant", "A", NULL);

   int64_t other = 0; /* a conversation the withdrawal doesn't touch */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "o", &other));
   add_row(other, "user", "hello", NULL);
   add_row(other, "assistant", "hi", NULL);

   int64_t before = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_withdraw_seq(&before)); /* turn A built */
   conv_withdrawn_t w;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_withdraw(alice_id, true, &w));
   conv_withdrawn_free(&w);
   int64_t after = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_withdraw_seq(&after)); /* turn B built */
   TEST_ASSERT_TRUE(after > before);

   const int64_t qb = add_row(conv, "user", "QB", NULL);
   const conv_turn_save_t b = { .question_id = qb,
                                .built_at = (int64_t)time(NULL),
                                .built_seq = after };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_save_turn(conv, alice_id, &b, NULL));
   sqlite3 *db = raw_open(TEST_DB);
   char sql[160];
   snprintf(sql, sizeof(sql), "SELECT reasoning_floor_pending FROM conversations WHERE id = %lld",
            (long long)conv);
   TEST_ASSERT_EQUAL_INT64(0, raw_int(db, sql)); /* B settled it */
   sqlite3_close(db);

   const int64_t qa = add_row(conv, "user", "QA", NULL);
   const conv_message_row_t ctx = { .role = "user",
                                    .content = "--- TURN CONTEXT (t) ---\nnothing\n"
                                               "--- END TURN CONTEXT (t) ---\n",
                                    .kind = "turn_context",
                                    .context_of = qa };
   const conv_turn_save_t a = { .rows = &ctx,
                                .n_rows = 1,
                                .question_id = qa,
                                .built_at = (int64_t)time(NULL),
                                .built_seq = before };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_save_turn(conv, alice_id, &a, NULL));
   db = raw_open(TEST_DB);
   TEST_ASSERT_TRUE(raw_int(db, sql) > 0); /* pending again: A's reply won't replay */
   snprintf(sql, sizeof(sql),
            "SELECT reasoning_floor_msg_id >= %lld FROM conversations "
            "WHERE id = %lld",
            (long long)qa, (long long)conv);
   TEST_ASSERT_EQUAL_INT64(1, raw_int(db, sql));
   sqlite3_close(db);

   /* Another conversation's turn, built as early, isn't held back. */
   const int64_t qo = add_row(other, "user", "QO", NULL);
   const conv_message_row_t octx = { .role = "user",
                                     .content = "--- TURN CONTEXT (t) ---\nnothing\n"
                                                "--- END TURN CONTEXT (t) ---\n",
                                     .kind = "turn_context",
                                     .context_of = qo };
   const conv_turn_save_t o = { .rows = &octx,
                                .n_rows = 1,
                                .question_id = qo,
                                .built_at = (int64_t)time(NULL),
                                .built_seq = before };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_save_turn(other, alice_id, &o, NULL));
   db = raw_open(TEST_DB);
   char osql[160];
   snprintf(osql, sizeof(osql),
            "SELECT reasoning_floor_pending + reasoning_floor_msg_id FROM conversations "
            "WHERE id = %lld",
            (long long)other);
   TEST_ASSERT_EQUAL_INT64(0, raw_int(db, osql));
   sqlite3_close(db);

   /* A turn built after it settles again. */
   const int64_t qc = add_row(conv, "user", "QC", NULL);
   int64_t now_seq = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_withdraw_seq(&now_seq));
   const conv_turn_save_t c = { .question_id = qc,
                                .built_at = (int64_t)time(NULL),
                                .built_seq = now_seq };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_save_turn(conv, alice_id, &c, NULL));
   db = raw_open(TEST_DB);
   snprintf(sql, sizeof(sql), "SELECT reasoning_floor_pending FROM conversations WHERE id = %lld",
            (long long)conv);
   TEST_ASSERT_EQUAL_INT64(0, raw_int(db, sql));
   sqlite3_close(db);
}

/* v96: a compacted conversation's reasoning floor rises to its newest row
 * (its summary's shape changed); one never compacted is untouched. */
static void test_v96_leaves_compacted_reasoning_behind(void) {
   int64_t compacted = 0, plain = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &compacted));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "p", &plain));
   const int64_t q = add_row(compacted, "user", "Q", NULL);
   const int64_t last = add_row(compacted, "assistant", "A", NULL);
   add_row(plain, "user", "Q", NULL);
   add_row(plain, "assistant", "A", NULL);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_set_compaction_watermark(compacted, alice_id, "earlier", q));

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_migrations_v96(s_db.db));
   sqlite3 *db = raw_open(TEST_DB);
   char sql[160];
   snprintf(sql, sizeof(sql), "SELECT reasoning_floor_msg_id FROM conversations WHERE id = %lld",
            (long long)compacted);
   TEST_ASSERT_EQUAL_INT64(last, raw_int(db, sql));
   snprintf(sql, sizeof(sql), "SELECT reasoning_floor_msg_id FROM conversations WHERE id = %lld",
            (long long)plain);
   TEST_ASSERT_EQUAL_INT64(0, raw_int(db, sql));
   sqlite3_close(db);
}

/* ---- v98: messages rebuilt (images, kind triggers) ---- */

/* The text a database stores for a messages object (NULL-safe copy). */
static void object_sql(sqlite3 *db, const char *name, char *out, size_t out_size) {
   sqlite3_stmt *st = NULL;
   out[0] = '\0';
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(db,
                                                       "SELECT sql FROM sqlite_master "
                                                       "WHERE name = ? AND tbl_name = 'messages'",
                                                       -1, &st, NULL));
   sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
   if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0)) {
      snprintf(out, out_size, "%s", (const char *)sqlite3_column_text(st, 0));
   }
   sqlite3_finalize(st);
}

/* A v97 database as one migrated from v94 has it: columns in the order the
 * ALTERs added them, the kind CHECK, its indexes and trigger, a gap in the
 * ids and a sequence past the newest row (an id handed out, then deleted). */
static sqlite3 *v97_fixture(void) {
   unlink(OLD_DB);
   sqlite3 *db = raw_open(OLD_DB);
   TEST_ASSERT_EQUAL_INT(
       SQLITE_OK,
       sqlite3_exec(db,
                    "PRAGMA foreign_keys=ON;"
                    "CREATE TABLE conversations (id INTEGER PRIMARY KEY, user_id INTEGER NOT "
                    "NULL);"
                    "CREATE TABLE images (id TEXT PRIMARY KEY, user_id INTEGER NOT NULL, "
                    "source INTEGER NOT NULL DEFAULT 0, retention_policy INTEGER NOT NULL "
                    "DEFAULT 0, mime_type TEXT NOT NULL, size INTEGER NOT NULL, filename TEXT "
                    "NOT NULL, created_at INTEGER NOT NULL, last_accessed "
                    "INTEGER);" AUTH_DB_CONVERSATION_IMAGES_SQL
                    "CREATE TABLE \"messages\" (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                    "conversation_id INTEGER NOT NULL, role TEXT NOT NULL CHECK(role IN "
                    "('system', 'user', 'assistant', 'tool')), content TEXT NOT NULL, created_at "
                    "INTEGER NOT NULL, tool_calls TEXT, tool_call_id TEXT, reasoning TEXT, "
                    "is_error INTEGER NOT NULL DEFAULT 0, llm_blocks_len INTEGER, "
                    "llm_blocks TEXT " CONV_LLM_BLOCKS_CHECK_SQL ", kind TEXT DEFAULT "
                    "NULL " CONV_MESSAGE_KIND_CHECK_V94_SQL ", context_of INTEGER DEFAULT NULL, "
                    "FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE "
                    "CASCADE);" CONV_MESSAGES_IDX_CONVERSATION_SQL CONV_MESSAGES_IDX_LLM_BLOCKS_SQL
                        CONV_MESSAGES_IDX_DISPLAY_SQL CONV_MESSAGES_IDX_KIND_SQL
                            CONV_MESSAGES_LLM_BLOCKS_TRIGGER_SQL
                    /* Something else that names messages: it must still name it after. */
                    "CREATE TRIGGER conversations_gone AFTER DELETE ON conversations BEGIN "
                    "DELETE FROM messages WHERE conversation_id = OLD.id; END;"
                    "INSERT INTO conversations VALUES (1, 1), (2, 1);"
                    "INSERT INTO messages (id, conversation_id, role, content, created_at, "
                    "tool_calls, tool_call_id, reasoning, is_error, llm_blocks_len, llm_blocks, "
                    "kind, context_of) VALUES "
                    "(1, 1, 'user', 'Q', 10, NULL, NULL, NULL, 0, NULL, NULL, NULL, NULL),"
                    "(2, 1, 'user', 'ctx', 11, NULL, NULL, NULL, 0, NULL, NULL, 'turn_context', "
                    "1),"
                    "(3, 1, 'assistant', 'A', 12, '[{\"id\":\"t1\"}]', NULL, '{\"r\":1}', 0, 2, "
                    "'[]', NULL, NULL),"
                    "(5, 1, 'tool', 'R', 13, NULL, 't1', NULL, 1, NULL, NULL, NULL, NULL),"
                    "(6, 2, 'system', 'D', 14, NULL, NULL, NULL, 0, NULL, NULL, 'directive', "
                    "NULL);"
                    "UPDATE sqlite_sequence SET seq = 40 WHERE name = 'messages';",
                    NULL, NULL, NULL));
   return db;
}

/* The rebuild keeps every row, id and the sequence; the new table, its
 * indexes and its triggers are stored exactly as a new database stores them;
 * nothing else that names messages is rewritten; it runs once. */
static void test_v98_rebuilds_messages_keeping_rows_ids_and_sequence(void) {
   sqlite3 *db = v97_fixture();
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_migrations_v98(db, OLD_DB));

   TEST_ASSERT_EQUAL_INT64(5, raw_int(db, "SELECT COUNT(*) FROM messages"));
   TEST_ASSERT_EQUAL_INT64(17, raw_int(db, "SELECT SUM(id) FROM messages"));
   TEST_ASSERT_EQUAL_INT64(1, raw_int(db, "SELECT COUNT(*) FROM messages WHERE id = 2 AND "
                                          "kind = 'turn_context' AND context_of = 1 AND "
                                          "created_at = 11"));
   TEST_ASSERT_EQUAL_INT64(1, raw_int(db, "SELECT COUNT(*) FROM messages WHERE id = 3 AND "
                                          "tool_calls = '[{\"id\":\"t1\"}]' AND reasoning = "
                                          "'{\"r\":1}' AND llm_blocks = '[]' AND "
                                          "llm_blocks_len = 2"));
   TEST_ASSERT_EQUAL_INT64(1, raw_int(db, "SELECT COUNT(*) FROM messages WHERE id = 5 AND "
                                          "tool_call_id = 't1' AND is_error = 1 AND images "
                                          "IS NULL"));
   TEST_ASSERT_EQUAL_INT64(40, raw_int(db, "SELECT seq FROM sqlite_sequence WHERE name = "
                                           "'messages'"));
   TEST_ASSERT_EQUAL_INT64(0, raw_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE name = "
                                          "'messages_v97' OR tbl_name = 'messages_v97'"));
   TEST_ASSERT_EQUAL_INT64(0, raw_int(db, "SELECT COUNT(*) FROM sqlite_sequence WHERE name = "
                                          "'messages_v97'"));
   TEST_ASSERT_EQUAL_INT64(1, raw_int(db, "PRAGMA foreign_keys")); /* back on */

   /* The same schema a new database (setUp's, the whole ladder) stores. */
   sqlite3 *fresh = raw_open(TEST_DB);
   TEST_ASSERT_EQUAL_INT64(9, raw_int(fresh, "SELECT COUNT(*) FROM sqlite_master WHERE "
                                             "tbl_name = 'messages'"));
   TEST_ASSERT_EQUAL_INT64(9, raw_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE "
                                          "tbl_name = 'messages'"));
   static const char *const names[] = {
      "messages",
      "idx_messages_conversation",
      "idx_messages_llm_blocks",
      "idx_messages_display",
      "idx_messages_kind",
      "idx_messages_images",
      "messages_llm_blocks_on_edit",
      "messages_kind_on_insert",
      "messages_kind_on_update",
   };
   for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
      char migrated[2048];
      char made[2048];
      object_sql(db, names[i], migrated, sizeof(migrated));
      object_sql(fresh, names[i], made, sizeof(made));
      TEST_ASSERT_TRUE_MESSAGE(made[0] != '\0', names[i]);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(made, migrated, names[i]);
   }
   /* The stored blocks are the last columns (nothing a filter reads sits
    * behind their overflow pages), in both. */
   TEST_ASSERT_EQUAL_INT64(1, raw_int(fresh, "SELECT name = 'llm_blocks' FROM "
                                             "pragma_table_info('messages') ORDER BY cid DESC "
                                             "LIMIT 1"));
   TEST_ASSERT_EQUAL_INT64(1, raw_int(fresh, "SELECT name = 'llm_blocks_len' FROM "
                                             "pragma_table_info('messages') ORDER BY cid DESC "
                                             "LIMIT 1 OFFSET 1"));
   TEST_ASSERT_EQUAL_INT64(1, raw_int(db, "SELECT name = 'llm_blocks' FROM "
                                          "pragma_table_info('messages') ORDER BY cid DESC "
                                          "LIMIT 1"));
   sqlite3_close(fresh);
   TEST_ASSERT_EQUAL_INT64(3, raw_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type = "
                                          "'trigger' AND tbl_name = 'messages'"));
   /* The images index serves the reads that name it. */
   sqlite3_stmt *plan = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(db,
                                                       "EXPLAIN QUERY PLAN SELECT id FROM messages "
                                                       "WHERE conversation_id = 1 AND images IS "
                                                       "NOT NULL",
                                                       -1, &plan, NULL));
   bool indexed = false;
   while (sqlite3_step(plan) == SQLITE_ROW) {
      const char *detail = (const char *)sqlite3_column_text(plan, 3);
      indexed = indexed || (detail && strstr(detail, "idx_messages_images"));
   }
   sqlite3_finalize(plan);
   TEST_ASSERT_TRUE(indexed);

   /* The other table's trigger still names messages, and works. */
   TEST_ASSERT_EQUAL_INT64(1, raw_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE name = "
                                          "'conversations_gone' AND sql LIKE '%FROM messages "
                                          "WHERE%'"));
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(db, "DELETE FROM conversations WHERE id = 2", NULL,
                                                 NULL, NULL));
   TEST_ASSERT_EQUAL_INT64(0, raw_int(db, "SELECT COUNT(*) FROM messages WHERE id = 6"));

   /* The blocks trigger still clears blocks on an edit. */
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(db,
                                                 "UPDATE messages SET content = 'B' WHERE "
                                                 "id = 3",
                                                 NULL, NULL, NULL));
   TEST_ASSERT_EQUAL_INT64(1, raw_int(db, "SELECT COUNT(*) FROM messages WHERE id = 3 AND "
                                          "llm_blocks IS NULL AND llm_blocks_len IS NULL"));

   /* A new row takes an id past the old high-water mark, not past max(id). */
   TEST_ASSERT_EQUAL_INT(SQLITE_OK,
                         sqlite3_exec(db,
                                      "INSERT INTO messages (conversation_id, role, content, "
                                      "created_at, kind) VALUES (1, 'system', 'T', 15, "
                                      "'tool_change')",
                                      NULL, NULL, NULL));
   TEST_ASSERT_EQUAL_INT64(41, raw_int(db, "SELECT MAX(id) FROM messages"));

   /* Once: a re-run finds the v98 table and leaves it be. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_migrations_v98(db, OLD_DB));
   TEST_ASSERT_EQUAL_INT64(5, raw_int(db, "SELECT COUNT(*) FROM messages"));
   sqlite3_close(db);
}

/* A table made by an earlier v98 (images, then the blocks before them) is
 * rebuilt into the final order, its images kept. */
static void test_v98_moves_the_blocks_last(void) {
   sqlite3 *db = v97_fixture();
   TEST_ASSERT_EQUAL_INT(SQLITE_OK,
                         sqlite3_exec(db,
                                      "ALTER TABLE messages ADD COLUMN images TEXT;"
                                      "UPDATE messages SET images = '[\"img_aaaaaaaaaaaa\"]' "
                                      "WHERE id = 5;",
                                      NULL, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_migrations_v98(db, OLD_DB));
   TEST_ASSERT_EQUAL_INT64(1, raw_int(db, "SELECT name = 'llm_blocks' FROM "
                                          "pragma_table_info('messages') ORDER BY cid DESC "
                                          "LIMIT 1"));
   TEST_ASSERT_EQUAL_INT64(1, raw_int(db, "SELECT COUNT(*) FROM messages WHERE id = 5 AND "
                                          "images = '[\"img_aaaaaaaaaaaa\"]'"));
   TEST_ASSERT_EQUAL_INT64(5, raw_int(db, "SELECT COUNT(*) FROM messages"));
   TEST_ASSERT_EQUAL_INT64(1, raw_int(db, "SELECT COUNT(*) FROM messages WHERE id = 3 AND "
                                          "llm_blocks = '[]' AND llm_blocks_len = 2"));
   sqlite3_close(db);
}

/* The images the stored rows name are recorded as a new row's are: an
 * ordinary question's uploads, a tool row's captures, the conversation
 * owner's only; never a reply's marker, a kinded row's, or another user's
 * image.  A re-run records nothing twice. */
static void test_v98_records_the_images_rows_name(void) {
   sqlite3 *db = v97_fixture();
   TEST_ASSERT_EQUAL_INT(
       SQLITE_OK,
       sqlite3_exec(db,
                    /* A table made by an earlier v98: a tool row's captures. */
                    "ALTER TABLE messages ADD COLUMN images TEXT;"
                    "UPDATE messages SET images = '[\"img_cccccccccccc\"]' WHERE id = 5;"
                    "INSERT INTO images (id, user_id, source, retention_policy, mime_type, size, "
                    "filename, created_at) VALUES "
                    "('img_aaaaaaaaaaaa', 1, 0, 0, 'image/jpeg', 1, 'a.jpg', 1),"
                    "('img_bbbbbbbbbbbb', 1, 3, 0, 'image/jpeg', 1, 'b.jpg', 1),"
                    "('img_cccccccccccc', 1, 5, 1, 'image/png', 1, 'c.png', 1),"
                    "('img_dddddddddddd', 2, 0, 0, 'image/jpeg', 1, 'd.jpg', 1),"
                    "('img_eeeeeeeeeeee', 1, 0, 0, 'image/jpeg', 1, 'e.jpg', 1);"
                    "INSERT INTO messages (id, conversation_id, role, content, created_at, kind, "
                    "context_of) VALUES "
                    "(7, 1, 'user', 'look [IMAGE:img_aaaaaaaaaaaa] [IMAGE:img_bbbbbbbbbbbb] "
                    "[IMAGE:img_dddddddddddd]', 20, NULL, NULL),"
                    "(8, 2, 'user', 'again [IMAGE:img_aaaaaaaaaaaa]', 21, NULL, NULL),"
                    "(9, 2, 'assistant', 'as before [IMAGE:img_eeeeeeeeeeee]', 22, NULL, NULL),"
                    "(10, 2, 'user', '[IMAGE:img_eeeeeeeeeeee]', 23, 'turn_context', 8);",
                    NULL, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_migrations_v98(db, OLD_DB));

   TEST_ASSERT_EQUAL_INT64(4, raw_int(db, "SELECT COUNT(*) FROM conversation_images"));
   TEST_ASSERT_EQUAL_INT64(2, raw_int(db, "SELECT COUNT(*) FROM conversation_images WHERE "
                                          "image_id = 'img_aaaaaaaaaaaa'")); /* both */
   TEST_ASSERT_EQUAL_INT64(1, raw_int(db, "SELECT COUNT(*) FROM conversation_images WHERE "
                                          "image_id = 'img_bbbbbbbbbbbb' AND "
                                          "conversation_id = 1")); /* MMS */
   TEST_ASSERT_EQUAL_INT64(1, raw_int(db, "SELECT COUNT(*) FROM conversation_images WHERE "
                                          "image_id = 'img_cccccccccccc' AND "
                                          "conversation_id = 1")); /* the capture */
   TEST_ASSERT_EQUAL_INT64(0, raw_int(db, "SELECT COUNT(*) FROM conversation_images WHERE "
                                          "image_id IN ('img_dddddddddddd', "
                                          "'img_eeeeeeeeeeee')"));

   /* A re-run (the v98 table, records kept) adds nothing; one lost is made
    * again. */
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(db,
                                                 "DELETE FROM conversation_images WHERE "
                                                 "conversation_id = 2",
                                                 NULL, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_migrations_v98(db, OLD_DB));
   TEST_ASSERT_EQUAL_INT64(4, raw_int(db, "SELECT COUNT(*) FROM conversation_images"));
   sqlite3_close(db);
}

/* The room the rebuild asks for: about twice the table plus a margin; a free
 * size that can't be read doesn't hold it. */
static void test_v98_room_check(void) {
   const int64_t mb = 1024 * 1024;
   int64_t need = 0;
   TEST_ASSERT_TRUE(auth_db_v98_has_room(100 * mb, 1000 * mb, &need));
   TEST_ASSERT_TRUE(need > 200 * mb && need < 400 * mb);
   TEST_ASSERT_FALSE(auth_db_v98_has_room(100 * mb, need - 1, &need));
   TEST_ASSERT_TRUE(auth_db_v98_has_room(100 * mb, need, NULL));
   TEST_ASSERT_TRUE(auth_db_v98_has_room(100 * mb, -1, NULL)); /* unknown */
   TEST_ASSERT_FALSE(auth_db_v98_has_room(0, 0, &need));       /* the margin, at least */
   TEST_ASSERT_TRUE(need > 0);
}

/* The kind triggers are made again each time the objects SQL runs: a
 * migration that extends the kinds re-runs it and the new kind is taken (and
 * a run with the old set takes it back). */
static void test_kind_triggers_are_refreshed(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   sqlite3 *db = raw_open(TEST_DB);
   char sql[256];
   snprintf(sql, sizeof(sql),
            "INSERT INTO messages (conversation_id, role, content, kind, created_at) VALUES "
            "(%lld, 'user', 'x', 'later_kind', 1)",
            (long long)conv);
   TEST_ASSERT_EQUAL_INT(SQLITE_CONSTRAINT, sqlite3_exec(db, sql, NULL, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(
       SQLITE_OK,
       sqlite3_exec(db,
                    CONV_MESSAGE_KIND_TRIGGERS_WITH_SQL(
                        "(NEW.kind = 'later_kind' OR " CONV_MESSAGE_KIND_ROLE_OK_SQL ")"),
                    NULL, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(db, sql, NULL, NULL, NULL));
   TEST_ASSERT_EQUAL_INT64(2, raw_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type = "
                                          "'trigger' AND name LIKE 'messages_kind_on_%'"));
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(db, CONV_MESSAGES_OBJECTS_SQL, NULL, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_CONSTRAINT, sqlite3_exec(db, sql, NULL, NULL, NULL));
   sqlite3_close(db);
}

/* A kind fits its role exactly as message_kind_role_ok() says, for every kind
 * and role, on insert and on update; an unknown kind never fits. */
static void test_kind_triggers_agree_with_message_kind_h(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   static const char *const roles[] = { "system", "user", "assistant", "tool" };
   sqlite3 *db = raw_open(TEST_DB);
   for (int k = MESSAGE_KIND_TURN_CONTEXT; k <= MESSAGE_KIND_SUMMARY; k++) {
      const char *name = message_kind_name((message_kind_t)k);
      for (size_t r = 0; r < sizeof(roles) / sizeof(roles[0]); r++) {
         char sql[256];
         snprintf(sql, sizeof(sql),
                  "INSERT INTO messages (conversation_id, role, content, kind, created_at) "
                  "VALUES (%lld, '%s', 'x', '%s', 1)",
                  (long long)conv, roles[r], name);
         char what[64];
         snprintf(what, sizeof(what), "%s as %s", name, roles[r]);
         const bool ok = message_kind_role_ok((message_kind_t)k, roles[r]);
         TEST_ASSERT_EQUAL_INT_MESSAGE(ok ? SQLITE_OK : SQLITE_CONSTRAINT,
                                       sqlite3_exec(db, sql, NULL, NULL, NULL), what);
      }
   }
   TEST_ASSERT_NOT_EQUAL(SQLITE_OK, sqlite3_exec(db,
                                                 "INSERT INTO messages (conversation_id, role, "
                                                 "content, kind, created_at) VALUES (1, 'user', "
                                                 "'x', 'nonsense', 1)",
                                                 NULL, NULL, NULL));
   /* An update can't move a row out of its kind's role, or give it a kind
    * its role can't take. */
   char sql[160];
   snprintf(sql, sizeof(sql),
            "UPDATE messages SET role = 'user' WHERE conversation_id = %lld AND kind = "
            "'tool_change'",
            (long long)conv);
   TEST_ASSERT_EQUAL_INT(SQLITE_CONSTRAINT, sqlite3_exec(db, sql, NULL, NULL, NULL));
   snprintf(sql, sizeof(sql),
            "UPDATE messages SET kind = 'bogus' WHERE conversation_id = %lld AND kind = "
            "'directive'",
            (long long)conv);
   TEST_ASSERT_EQUAL_INT(SQLITE_CONSTRAINT, sqlite3_exec(db, sql, NULL, NULL, NULL));
   sqlite3_close(db);

   /* And the API takes the new kind. */
   const conv_message_row_t row = { .role = "system", .content = "x", .kind = "tool_change" };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, alice_id, &row, NULL));
}

/* images: a JSON array, on tool rows only. */
static void test_images_only_on_tool_rows_as_a_json_array(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   static const struct {
      const char *role;
      const char *images; /* SQL literal */
      int rc;
   } cases[] = {
      { "tool", "'[\"a1\",\"b2\"]'", SQLITE_OK },
      { "tool", "'[]'", SQLITE_OK },
      { "tool", "NULL", SQLITE_OK },
      { "user", "NULL", SQLITE_OK },
      { "user", "'[\"a1\"]'", SQLITE_CONSTRAINT },
      { "assistant", "'[\"a1\"]'", SQLITE_CONSTRAINT },
      { "tool", "'not json'", SQLITE_CONSTRAINT },
      { "tool", "'{\"a\":1}'", SQLITE_CONSTRAINT },
      { "tool", "'\"a1\"'", SQLITE_CONSTRAINT },
   };
   sqlite3 *db = raw_open(TEST_DB);
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      char sql[256];
      snprintf(sql, sizeof(sql),
               "INSERT INTO messages (conversation_id, role, content, created_at, images) "
               "VALUES (%lld, '%s', 'x', 1, %s)",
               (long long)conv, cases[i].role, cases[i].images);
      TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].rc, sqlite3_exec(db, sql, NULL, NULL, NULL), sql);
   }
   TEST_ASSERT_EQUAL_INT64(1, raw_int(db, "SELECT COUNT(*) FROM messages WHERE images = "
                                          "'[\"a1\",\"b2\"]'"));
   sqlite3_close(db);
}

/* An unanswered envelope goes with its context; one the attempt saved work
 * after stays, and a row that isn't an envelope is never taken. */
static void test_an_unanswered_envelope_is_retracted(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   const int64_t q = add_row(conv, "user", "Q", NULL);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, conv_db_retract_envelope(conv, alice_id, q));
   const int64_t env = add_row(conv, "user", "job results", "envelope");
   const conv_message_row_t ctx = { .role = "user",
                                    .content = "--- TURN CONTEXT (t) ---\n",
                                    .kind = "turn_context",
                                    .context_of = env };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, alice_id, &ctx, NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, conv_db_retract_envelope(conv, bob_id, env));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_retract_envelope(conv, alice_id, env));
   sqlite3 *db = raw_open(TEST_DB);
   char sql[128];
   snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM messages WHERE conversation_id = %lld",
            (long long)conv);
   TEST_ASSERT_EQUAL_INT64(1, raw_int(db, sql)); /* only the question */
   sqlite3_close(db);

   /* An announced change saved after it (carried, naming no question)
    * doesn't hold it back, and stays. */
   const int64_t env3 = add_row(conv, "user", "job results", "envelope");
   add_row(conv, "system", "Updated instructions: x", "instruction");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_retract_envelope(conv, alice_id, env3));
   db = raw_open(TEST_DB);
   snprintf(sql, sizeof(sql),
            "SELECT COUNT(*) FROM messages WHERE conversation_id = %lld AND kind = 'instruction'",
            (long long)conv);
   TEST_ASSERT_EQUAL_INT64(1, raw_int(db, sql));
   sqlite3_close(db);

   /* A tool-set change is the conversation's too; it now follows no user
    * turn (it folds), so the reasoning floor goes past it. */
   db = raw_open(TEST_DB);
   snprintf(sql, sizeof(sql), "SELECT reasoning_floor_msg_id FROM conversations WHERE id = %lld",
            (long long)conv);
   TEST_ASSERT_EQUAL_INT64(0, raw_int(db, sql));
   sqlite3_close(db);
   const int64_t env4 = add_row(conv, "user", "job results", "envelope");
   const int64_t change = add_row(conv, "system", "{\"rendered\":\"inline\",\"tools\":[]}",
                                  "tool_change");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_retract_envelope(conv, alice_id, env4));
   db = raw_open(TEST_DB);
   TEST_ASSERT_EQUAL_INT64(change, raw_int(db, sql));
   snprintf(sql, sizeof(sql),
            "SELECT COUNT(*) FROM messages WHERE conversation_id = %lld AND kind = 'tool_change'",
            (long long)conv);
   TEST_ASSERT_EQUAL_INT64(1, raw_int(db, sql));
   sqlite3_close(db);

   const int64_t env2 = add_row(conv, "user", "job results", "envelope");
   add_row(conv, "assistant", "working on it", NULL);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_DUPLICATE, conv_db_retract_envelope(conv, alice_id, env2));
}

/* ---- citation handles ---- */

typedef struct {
   int count;
   int handles[8];
   char items[8][16];
} handles_t;

static int collect_handle(const char *source, const char *item_id, int handle, void *ctx) {
   (void)source;
   handles_t *out = ctx;
   if (out->count < 8) {
      out->handles[out->count] = handle;
      snprintf(out->items[out->count], sizeof(out->items[0]), "%s", item_id);
   }
   out->count++;
   return 0;
}

static void test_handles_are_stable_per_conversation(void) {
   int64_t conv = 0, other = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "d", &other));

   conv_focus_handle_t first[] = {
      { .source = "memory_fact", .item_id = "fact:1" },
      { .source = "memory_fact", .item_id = "fact:2" },
   };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_focus_handles_assign(conv, alice_id, first, 2));
   TEST_ASSERT_EQUAL_INT(1, first[0].handle);
   TEST_ASSERT_EQUAL_INT(2, first[1].handle);
   TEST_ASSERT_TRUE(first[0].is_new && first[1].is_new);

   conv_focus_handle_t second[] = {
      { .source = "memory_fact", .item_id = "fact:3" },
      { .source = "memory_fact", .item_id = "fact:1" },
      { .source = "memory_summary", .item_id = "fact:1" },
   };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_focus_handles_assign(conv, alice_id, second, 3));
   TEST_ASSERT_EQUAL_INT(3, second[0].handle);
   TEST_ASSERT_TRUE(second[0].is_new);
   TEST_ASSERT_EQUAL_INT(1, second[1].handle);
   TEST_ASSERT_FALSE(second[1].is_new);
   TEST_ASSERT_EQUAL_INT(4, second[2].handle);

   /* Another conversation numbers from 1. */
   conv_focus_handle_t elsewhere[] = { { .source = "memory_fact", .item_id = "fact:3" } };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_focus_handles_assign(other, alice_id, elsewhere, 1));
   TEST_ASSERT_EQUAL_INT(1, elsewhere[0].handle);

   handles_t loaded = { 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_focus_handles_load(conv, alice_id, collect_handle, &loaded));
   TEST_ASSERT_EQUAL_INT(4, loaded.count);
   for (int i = 0; i < 4; i++)
      TEST_ASSERT_EQUAL_INT(i + 1, loaded.handles[i]);
   TEST_ASSERT_EQUAL_STRING("fact:3", loaded.items[2]);
}

static void test_handles_check_the_owner_and_input(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   conv_focus_handle_t item[] = { { .source = "memory_fact", .item_id = "fact:1" } };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, conv_db_focus_handles_assign(conv, bob_id, item, 1));
   handles_t loaded = { 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND,
                         conv_db_focus_handles_load(conv, bob_id, collect_handle, &loaded));

   char long_id[CONV_FOCUS_ITEM_ID_MAX + 2];
   memset(long_id, 'x', sizeof(long_id) - 1);
   long_id[sizeof(long_id) - 1] = '\0';
   conv_focus_handle_t bad[] = {
      { .source = "memory_fact", .item_id = "fact:9" },
      { .source = "memory_fact", .item_id = long_id },
   };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, conv_db_focus_handles_assign(conv, alice_id, bad, 2));
   /* Nothing from a refused batch is stored. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_focus_handles_load(conv, alice_id, collect_handle, &loaded));
   TEST_ASSERT_EQUAL_INT(0, loaded.count);
}

static void test_deleting_a_conversation_drops_its_handles(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   conv_focus_handle_t item[] = { { .source = "memory_fact", .item_id = "fact:1" } };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_focus_handles_assign(conv, alice_id, item, 1));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_delete(conv, alice_id));
   sqlite3 *db = raw_open(TEST_DB);
   TEST_ASSERT_EQUAL_INT64(0, raw_int(db, "SELECT COUNT(*) FROM conversation_focus_handles"));
   sqlite3_close(db);
}

int main(void) {
   test_tmp_path(TEST_DB, sizeof(TEST_DB), "dawn_test_kind_prefix.db");
   test_tmp_path(OLD_DB, sizeof(OLD_DB), "dawn_test_kind_prefix_v93.db");
   UNITY_BEGIN();
   RUN_TEST(test_display_reads_skip_kind_rows);
   RUN_TEST(test_replay_read_returns_kind_rows_in_order);
   RUN_TEST(test_kind_rows_do_not_count_as_messages);
   RUN_TEST(test_kind_must_match_role);
   RUN_TEST(test_rows_are_saved_together);
   RUN_TEST(test_v94_migrates_a_v93_database);
   RUN_TEST(test_turn_save_stores_prefix_and_anchored_rows);
   RUN_TEST(test_turn_save_is_all_or_nothing);
   RUN_TEST(test_turn_save_checks_the_owner_and_input);
   RUN_TEST(test_turn_save_moves_the_prefix_and_raises_the_floor);
   RUN_TEST(test_replay_read_honors_the_reasoning_floor);
   RUN_TEST(test_gc_keeps_what_a_conversation_uses);
   RUN_TEST(test_a_large_tool_change_is_stored_by_hash);
   RUN_TEST(test_altered_prefix_bytes_are_refused);
   RUN_TEST(test_a_forgotten_item_is_withdrawn);
   RUN_TEST(test_an_unmarked_delete_records_nothing);
   RUN_TEST(test_chunk_ids_are_never_reused);
   RUN_TEST(test_memory_blocks_are_withdrawn);
   RUN_TEST(test_a_purged_withdrawal_still_settles);
   RUN_TEST(test_a_turn_built_before_a_withdrawal_saves_behind_it);
   RUN_TEST(test_v96_leaves_compacted_reasoning_behind);
   RUN_TEST(test_v98_rebuilds_messages_keeping_rows_ids_and_sequence);
   RUN_TEST(test_v98_moves_the_blocks_last);
   RUN_TEST(test_v98_records_the_images_rows_name);
   RUN_TEST(test_v98_room_check);
   RUN_TEST(test_kind_triggers_are_refreshed);
   RUN_TEST(test_kind_triggers_agree_with_message_kind_h);
   RUN_TEST(test_images_only_on_tool_rows_as_a_json_array);
   RUN_TEST(test_an_unanswered_envelope_is_retracted);
   RUN_TEST(test_handles_are_stable_per_conversation);
   RUN_TEST(test_handles_check_the_owner_and_input);
   RUN_TEST(test_deleting_a_conversation_drops_its_handles);
   return UNITY_END();
}
