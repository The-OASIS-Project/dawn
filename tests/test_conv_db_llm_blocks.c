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
 * Unit tests for messages.llm_blocks: only the replay read returns it, only
 * assistant rows hold it, the owner check applies, the compaction watermark
 * drops blocks below it, an edit drops a row's blocks, and the v92 migration
 * re-renders voice rows saved as raw Claude block arrays.
 */

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "auth/auth_db_messages.h"
#include "test_tmp.h"
#include "unity.h"

int auth_db_migrations_v92(sqlite3 *db);
int auth_db_migrations_v93(sqlite3 *db);

static char TEST_DB[TEST_TMP_PATH_MAX];
static int alice_id = 0;
static int bob_id = 0;

static void open_db(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(TEST_DB));
}

void setUp(void) {
   unlink(TEST_DB);
   open_db();
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
}

/* ---- helpers ---- */

static sqlite3 *raw_db(void);

typedef struct {
   int count;
   int64_t ids[16];
   char blocks[16][128];
   bool has_blocks[16];
} llm_rows_t;

static int collect_llm(const conversation_llm_row_t *row, void *ctx) {
   llm_rows_t *out = ctx;
   if (out->count < 16) {
      out->ids[out->count] = row->id;
      out->has_blocks[out->count] = row->llm_blocks != NULL;
      if (row->llm_blocks)
         snprintf(out->blocks[out->count], sizeof(out->blocks[0]), "%s", row->llm_blocks);
      out->count++;
   }
   return 0;
}

typedef struct {
   int count;
   bool leaked;
} display_rows_t;

static int collect_display(const conversation_message_t *msg, void *ctx) {
   display_rows_t *out = ctx;
   out->count++;
   const char *fields[] = { msg->content, msg->tool_calls, msg->tool_call_id, msg->reasoning };
   for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
      if (fields[i] && strstr(fields[i], "SECRET_SIG"))
         out->leaked = true;
   }
   return 0;
}

static int64_t add_assistant(int64_t conv, const char *text, const char *blocks) {
   const conv_message_row_t row = { .role = "assistant", .content = text, .llm_blocks = blocks };
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, alice_id, &row, &id));
   TEST_ASSERT_TRUE(id > 0);
   return id;
}

static const char *BLOCKS =
    "{\"v\":1,\"blocks\":[{\"type\":\"reasoning\",\"sig\":\"SECRET_SIG\"}]}";

/* ---- tests ---- */

static void test_blocks_round_trip_through_replay_read_only(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_message(conv, alice_id, "user", "Hi"));
   const int64_t a = add_assistant(conv, "Hello.", BLOCKS);

   llm_rows_t llm = { 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_for_llm(conv, alice_id, 0, collect_llm, &llm));
   TEST_ASSERT_EQUAL_INT(2, llm.count);
   TEST_ASSERT_FALSE(llm.has_blocks[0]);
   TEST_ASSERT_EQUAL_INT64(a, llm.ids[1]);
   TEST_ASSERT_EQUAL_STRING(BLOCKS, llm.blocks[1]);

   /* Every display read leaves the column out. */
   display_rows_t shown = { 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages(conv, alice_id, collect_display, &shown));
   TEST_ASSERT_EQUAL_INT(2, shown.count);
   TEST_ASSERT_FALSE(shown.leaked);
   shown = (display_rows_t){ 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_after(conv, alice_id, 0, collect_display, &shown));
   TEST_ASSERT_FALSE(shown.leaked);
   shown = (display_rows_t){ 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_admin(conv, collect_display, &shown));
   TEST_ASSERT_FALSE(shown.leaked);
}

static void test_replay_read_checks_the_owner(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   add_assistant(conv, "Hello.", BLOCKS);
   llm_rows_t llm = { 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_for_llm(conv, bob_id, 0, collect_llm, &llm));
   TEST_ASSERT_EQUAL_INT(0, llm.count);

   /* Nor can bob write into alice's conversation. */
   const conv_message_row_t row = { .role = "assistant", .content = "x", .llm_blocks = BLOCKS };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_FORBIDDEN, conv_db_add_row(conv, bob_id, &row, NULL));
}

static void test_blocks_only_on_assistant_rows(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   const conv_message_row_t row = { .role = "user", .content = "Hi", .llm_blocks = BLOCKS };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, alice_id, &row, NULL));
   llm_rows_t llm = { 0 };
   conv_db_get_messages_for_llm(conv, alice_id, 0, collect_llm, &llm);
   TEST_ASSERT_EQUAL_INT(1, llm.count);
   TEST_ASSERT_FALSE(llm.has_blocks[0]);

   /* The schema refuses them too, whatever writes the row. */
   sqlite3 *db = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(TEST_DB, &db));
   char sql[256];
   snprintf(sql, sizeof(sql),
            "INSERT INTO messages (conversation_id, role, content, created_at, llm_blocks_len, "
            "llm_blocks) VALUES (%lld, 'tool', 'r', 1, 2, '{}')",
            (long long)conv);
   TEST_ASSERT_NOT_EQUAL(SQLITE_OK, sqlite3_exec(db, sql, NULL, NULL, NULL));
   sqlite3_close(db);
}

static void test_after_id_and_watermark_drop_old_blocks(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   const int64_t first = add_assistant(conv, "one", BLOCKS);
   const int64_t second = add_assistant(conv, "two", BLOCKS);

   llm_rows_t llm = { 0 };
   conv_db_get_messages_for_llm(conv, alice_id, first, collect_llm, &llm);
   TEST_ASSERT_EQUAL_INT(1, llm.count);
   TEST_ASSERT_EQUAL_INT64(second, llm.ids[0]);

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_set_compaction_watermark(conv, alice_id, "summary", first));
   llm = (llm_rows_t){ 0 };
   conv_db_get_messages_for_llm(conv, alice_id, 0, collect_llm, &llm);
   TEST_ASSERT_EQUAL_INT(2, llm.count);
   TEST_ASSERT_FALSE(llm.has_blocks[0]); /* below the watermark: dropped */
   TEST_ASSERT_TRUE(llm.has_blocks[1]);

   /* A stale watermark from someone else changes nothing. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_set_compaction_watermark(conv, bob_id, "x", second));
   llm = (llm_rows_t){ 0 };
   conv_db_get_messages_for_llm(conv, alice_id, 0, collect_llm, &llm);
   TEST_ASSERT_TRUE(llm.has_blocks[1]);
}

static void test_editing_a_row_drops_its_blocks(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   const int64_t id = add_assistant(conv, "Hello.", BLOCKS);
   sqlite3 *db = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(TEST_DB, &db));
   char sql[128];
   snprintf(sql, sizeof(sql), "UPDATE messages SET content = 'Edited.' WHERE id = %lld",
            (long long)id);
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(db, sql, NULL, NULL, NULL));
   sqlite3_close(db);

   llm_rows_t llm = { 0 };
   conv_db_get_messages_for_llm(conv, alice_id, 0, collect_llm, &llm);
   TEST_ASSERT_FALSE(llm.has_blocks[0]);
}

static void test_blocks_over_the_cap_are_not_stored(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   char *big = malloc(CONV_LLM_BLOCKS_MAX + 2);
   TEST_ASSERT_NOT_NULL(big);
   memset(big, 'x', CONV_LLM_BLOCKS_MAX + 1);
   big[CONV_LLM_BLOCKS_MAX + 1] = '\0';
   add_assistant(conv, "long", big);
   free(big);
   llm_rows_t llm = { 0 };
   conv_db_get_messages_for_llm(conv, alice_id, 0, collect_llm, &llm);
   TEST_ASSERT_EQUAL_INT(1, llm.count);
   TEST_ASSERT_FALSE(llm.has_blocks[0]);
}

static int count_conv(const conversation_t *conv, void *ctx) {
   (void)conv;
   (*(int *)ctx)++;
   return 0;
}

/* Range reads (context_expand, provenance) and content search don't see the
 * blocks either. */
static void test_range_and_search_leave_blocks_out(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   const int64_t id = add_assistant(conv, "Hello.", BLOCKS);
   display_rows_t shown = { 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_by_range(conv, alice_id, id, id, 0, true,
                                                       collect_display, &shown));
   TEST_ASSERT_EQUAL_INT(1, shown.count);
   TEST_ASSERT_FALSE(shown.leaked);
   int hits = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_search_content(alice_id, "SECRET_SIG", NULL, count_conv, &hits));
   TEST_ASSERT_EQUAL_INT(0, hits);
}

/* Past the per-load budget, the oldest rows load without their blocks. */
static void test_load_budget_drops_the_oldest(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   const size_t each = CONV_LLM_BLOCKS_LOAD_BUDGET / 2 - 16;
   char *big = malloc(each + 1);
   TEST_ASSERT_NOT_NULL(big);
   memset(big, 'x', each);
   big[each] = '\0';
   const int64_t oldest = add_assistant(conv, "one", big);
   add_assistant(conv, "two", big);
   add_assistant(conv, "three", big);
   free(big);
   llm_rows_t llm = { 0 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_for_llm(conv, alice_id, 0, collect_llm, &llm));
   TEST_ASSERT_EQUAL_INT(3, llm.count);
   TEST_ASSERT_EQUAL_INT64(oldest, llm.ids[0]);
   TEST_ASSERT_FALSE(llm.has_blocks[0]);
   TEST_ASSERT_TRUE(llm.has_blocks[1]);
   TEST_ASSERT_TRUE(llm.has_blocks[2]);
}

/* Blocks a watermark passed without clearing (a crash in between, say) are
 * cleared by the maintenance sweep. */
static void test_sweep_clears_what_the_watermark_left(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &conv));
   const int64_t id = add_assistant(conv, "one", BLOCKS);
   add_assistant(conv, "two", BLOCKS);
   sqlite3 *db = raw_db();
   char sql[160];
   snprintf(sql, sizeof(sql),
            "UPDATE conversations SET context_watermark_msg_id = %lld WHERE id = %lld",
            (long long)id, (long long)conv);
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(db, sql, NULL, NULL, NULL));
   sqlite3_close(db);
   int cleared = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_sweep_compacted_blocks(&cleared));
   TEST_ASSERT_EQUAL_INT(1, cleared);
   llm_rows_t llm = { 0 };
   conv_db_get_messages_for_llm(conv, alice_id, 0, collect_llm, &llm);
   TEST_ASSERT_FALSE(llm.has_blocks[0]);
   TEST_ASSERT_TRUE(llm.has_blocks[1]);
}

/* ---- the v92 re-render of legacy voice rows ---- */

static sqlite3 *raw_db(void) {
   sqlite3 *db = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(TEST_DB, &db));
   return db;
}

static int64_t raw_insert(sqlite3 *db, int64_t conv, const char *role, const char *content) {
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK,
                         sqlite3_prepare_v2(db,
                                            "INSERT INTO messages (conversation_id, role, "
                                            "content, created_at) VALUES (?, ?, ?, 1)",
                                            -1, &st, NULL));
   sqlite3_bind_int64(st, 1, conv);
   sqlite3_bind_text(st, 2, role, -1, SQLITE_STATIC);
   sqlite3_bind_text(st, 3, content, -1, SQLITE_STATIC);
   TEST_ASSERT_EQUAL_INT(SQLITE_DONE, sqlite3_step(st));
   sqlite3_finalize(st);
   return sqlite3_last_insert_rowid(db);
}

static char *raw_column(sqlite3 *db, int64_t id, const char *col) {
   char sql[128];
   snprintf(sql, sizeof(sql), "SELECT %s FROM messages WHERE id = %lld", col, (long long)id);
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(db, sql, -1, &st, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
   const unsigned char *v = sqlite3_column_text(st, 0);
   char *out = v ? strdup((const char *)v) : NULL;
   sqlite3_finalize(st);
   return out;
}

static void assert_column(sqlite3 *db, int64_t id, const char *col, const char *expected) {
   char *v = raw_column(db, id, col);
   if (expected)
      TEST_ASSERT_EQUAL_STRING(expected, v);
   else
      TEST_ASSERT_NULL(v);
   free(v);
}

static void test_v92_rerenders_legacy_voice_rows(void) {
   int64_t voice = 0, webui = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_create_with_origin(alice_id, "v", "voice", &voice));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "w", &webui));
   auth_db_shutdown();

   sqlite3 *db = raw_db();
   /* A call and its one result. */
   const int64_t call = raw_insert(
       db, voice, "assistant",
       "[ { \"type\": \"thinking\", \"thinking\": \"hm\", \"signature\": \"SECRET_SIG\" }, "
       "{ \"type\": \"text\", \"text\": \"Checking.\" }, "
       "{ \"type\": \"tool_use\", \"id\": \"toolu_1\", \"name\": \"weather\", "
       "\"input\": { \"city\": \"Paris\" } } ]");
   const int64_t result = raw_insert(db, voice, "user",
                                     "[ { \"type\": \"tool_result\", \"tool_use_id\": "
                                     "\"toolu_1\", \"content\": \"Sunny\" } ]");
   /* Two calls answered in one row: notes, with no call left open. */
   const int64_t two = raw_insert(db, voice, "assistant",
                                  "[ { \"type\": \"tool_use\", \"id\": \"a\", \"name\": \"x\", "
                                  "\"input\": {} }, { \"type\": \"tool_use\", \"id\": \"b\", "
                                  "\"name\": \"y\", \"input\": {} } ]");
   const int64_t both = raw_insert(
       db, voice, "user",
       "[ { \"type\": \"tool_result\", \"tool_use_id\": \"a\", \"content\": \"A\" }, "
       "{ \"type\": \"tool_result\", \"tool_use_id\": \"b\", \"content\": [ { \"type\": "
       "\"text\", \"text\": \"B\" } ] } ]");
   /* Not block arrays, or not a voice conversation: untouched. */
   const int64_t bracket = raw_insert(db, voice, "user", "[laughs] okay");
   const int64_t typed = raw_insert(db, webui, "user",
                                    "[ { \"type\": \"text\", \"text\": \"t\" } ]");
   /* Hostile or odd rows: a null text (must not crash), an array followed by
    * more text (not a block array: left whole), and a block type this doesn't
    * know beside thinking (the thinking still goes). */
   const int64_t null_text = raw_insert(db, voice, "user",
                                        "[ { \"type\": \"text\", \"text\": null } ]");
   const int64_t null_result = raw_insert(db, voice, "assistant",
                                          "[ { \"type\": \"tool_result\", \"content\": [ { "
                                          "\"type\": \"text\", \"text\": null } ] } ]");
   const int64_t trailing = raw_insert(db, voice, "assistant",
                                       "[{\"type\":\"text\",\"text\":\"a\"}] and then more");
   const int64_t unknown = raw_insert(
       db, voice, "assistant",
       "[ { \"type\": \"thinking\", \"thinking\": \"hm\", \"signature\": \"SECRET_SIG\" }, "
       "{ \"type\": \"server_tool_use\", \"id\": \"s1\" }, { \"type\": \"text\", \"text\": "
       "\"Done.\" } ]");

   TEST_ASSERT_EQUAL_INT(0, auth_db_migrations_v92(db));

   assert_column(db, call, "role", "assistant");
   assert_column(db, call, "content", "Checking.");
   assert_column(db, call, "tool_calls",
                 "[{\"id\":\"toolu_1\",\"type\":\"function\",\"function\":{\"name\":"
                 "\"weather\",\"arguments\":\"{\\\"city\\\":\\\"Paris\\\"}\"}}]");
   assert_column(db, result, "role", "tool");
   assert_column(db, result, "content", "Sunny");
   assert_column(db, result, "tool_call_id", "toolu_1");

   assert_column(db, two, "content", "[Tool Call: x]\n\n[Tool Call: y]");
   assert_column(db, two, "tool_calls", NULL);
   assert_column(db, both, "role", "assistant");
   assert_column(db, both, "content", "[Tool Result: A]\n\n[Tool Result: B]");

   assert_column(db, bracket, "content", "[laughs] okay");
   assert_column(db, null_text, "content", "[No text]");
   assert_column(db, null_result, "content", "[No text]");
   assert_column(db, trailing, "content", "[{\"type\":\"text\",\"text\":\"a\"}] and then more");
   assert_column(db, unknown, "content", "[server_tool_use]\n\nDone.");
   assert_column(db, typed, "content", "[ { \"type\": \"text\", \"text\": \"t\" } ]");

   /* Nothing a vendor issued is left, and a second run changes nothing. */
   sqlite3_stmt *st = NULL;
   sqlite3_prepare_v2(db, "SELECT count(*) FROM messages WHERE content LIKE '%SECRET_SIG%'", -1,
                      &st, NULL);
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
   TEST_ASSERT_EQUAL_INT(0, sqlite3_column_int(st, 0));
   sqlite3_finalize(st);
   TEST_ASSERT_EQUAL_INT(0, auth_db_migrations_v92(db));
   assert_column(db, call, "content", "Checking.");
   sqlite3_close(db);

   open_db(); /* for tearDown */
}

/* v93: voice rows whose tool calls the old save dropped become notes. */
static void test_v93_notes_for_lost_tool_calls(void) {
   int64_t voice = 0, webui = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_create_with_origin(alice_id, "v", "voice", &voice));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "w", &webui));
   auth_db_shutdown();

   sqlite3 *db = raw_db();
   raw_insert(db, voice, "user", "Search for today's news");
   const int64_t call = raw_insert(db, voice, "assistant", "\n\n");
   const int64_t res_a = raw_insert(db, voice, "tool", "Result A");
   const int64_t res_b = raw_insert(db, voice, "tool", "");
   const int64_t answer = raw_insert(db, voice, "assistant", "Here it is.");
   raw_insert(db, voice, "user", "And the weather?");
   const int64_t spoke = raw_insert(db, voice, "assistant", "Let me check.");
   const int64_t res_c = raw_insert(db, voice, "tool", "Sunny");
   raw_insert(db, voice, "assistant", "Sunny today.");
   raw_insert(db, voice, "user", "Thanks");
   const int64_t lone = raw_insert(db, voice, "assistant", "   ");
   /* A non-voice result right after it by id: "next" is within a conversation. */
   const int64_t web_tool = raw_insert(db, webui, "tool", "web result");
   const int64_t web_empty = raw_insert(db, webui, "assistant", "");
   /* Left alone: a result with its call id, an empty turn that has calls. */
   const int64_t kept = raw_insert(db, voice, "tool", "kept result");
   const int64_t with_calls = raw_insert(db, voice, "assistant", "");
   char sql[200];
   snprintf(sql, sizeof(sql),
            "UPDATE messages SET tool_call_id = 'c1' WHERE id = %lld; UPDATE messages SET "
            "tool_calls = '[]' WHERE id = %lld",
            (long long)kept, (long long)with_calls);
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_exec(db, sql, NULL, NULL, NULL));
   /* A background job spawned from voice: its rows aren't the old save's. */
   TEST_ASSERT_EQUAL_INT(SQLITE_OK,
                         sqlite3_exec(db,
                                      "INSERT INTO conversations (user_id, title, created_at, "
                                      "updated_at, origin, job_status) SELECT user_id, 'j', 1, 1, "
                                      "'voice', 'done' FROM conversations LIMIT 1",
                                      NULL, NULL, NULL));
   const int64_t job = sqlite3_last_insert_rowid(db);
   const int64_t job_empty = raw_insert(db, job, "assistant", "");

   TEST_ASSERT_EQUAL_INT(0, auth_db_migrations_v93(db));

   assert_column(db, call, "content", "[Tool Call: its name and arguments weren't saved]");
   assert_column(db, res_a, "role", "assistant");
   assert_column(db, res_a, "content", "[Tool Result: Result A]");
   assert_column(db, res_b, "content", "[Tool Result: (no text)]");
   assert_column(db, answer, "content", "Here it is.");
   assert_column(db, spoke, "content",
                 "Let me check.\n\n[Tool Call: its name and arguments weren't saved]");
   assert_column(db, res_c, "content", "[Tool Result: Sunny]");
   assert_column(db, lone, "content", "[No text]");
   assert_column(db, web_tool, "role", "tool");
   assert_column(db, web_empty, "content", "");
   assert_column(db, kept, "role", "tool");
   assert_column(db, kept, "content", "kept result");
   assert_column(db, with_calls, "content", "");
   assert_column(db, job_empty, "content", "");

   /* A second run changes nothing. */
   TEST_ASSERT_EQUAL_INT(0, auth_db_migrations_v93(db));
   assert_column(db, spoke, "content",
                 "Let me check.\n\n[Tool Call: its name and arguments weren't saved]");
   assert_column(db, res_a, "content", "[Tool Result: Result A]");
   sqlite3_close(db);

   open_db(); /* for tearDown */
}

int main(void) {
   test_tmp_path(TEST_DB, sizeof(TEST_DB), "dawn_test_llm_blocks.db");
   UNITY_BEGIN();
   RUN_TEST(test_blocks_round_trip_through_replay_read_only);
   RUN_TEST(test_replay_read_checks_the_owner);
   RUN_TEST(test_blocks_only_on_assistant_rows);
   RUN_TEST(test_after_id_and_watermark_drop_old_blocks);
   RUN_TEST(test_editing_a_row_drops_its_blocks);
   RUN_TEST(test_blocks_over_the_cap_are_not_stored);
   RUN_TEST(test_range_and_search_leave_blocks_out);
   RUN_TEST(test_load_budget_drops_the_oldest);
   RUN_TEST(test_sweep_clears_what_the_watermark_left);
   RUN_TEST(test_v92_rerenders_legacy_voice_rows);
   RUN_TEST(test_v93_notes_for_lost_tool_calls);
   return UNITY_END();
}
