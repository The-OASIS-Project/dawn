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
 * Memory extraction's outside kind over a conversation's stored rows, on a
 * real schema and the real history loader: what DAWN wrote around outside
 * text (a turn context's outside items, a job's result) counts though the
 * loader leaves those rows out, and rows a compaction took from the live
 * history are read from the database.
 */

#include <json-c/json.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_messages.h"
#include "config/dawn_config.h"
#include "core/outside_kind.h"
#include "memory/memory_extraction_input.h"
#include "memory/memory_history_loader.h"
#include "memory/memory_note_guard.h"
#include "unity.h"

/* The note guard isn't under test: nothing is redacted. */
memory_note_guard_t *memory_note_guard_create(struct json_object *conversation_history) {
   (void)conversation_history;
   return NULL;
}
struct json_object *memory_note_guard_redact(const memory_note_guard_t *guard,
                                             struct json_object *msg) {
   (void)guard;
   return json_object_get(msg);
}
void memory_note_guard_free(memory_note_guard_t *guard) {
   (void)guard;
}

static int s_user;

void setUp(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("jon", "hash", true));
   auth_user_t user;
   memset(&user, 0, sizeof(user));
   auth_db_get_user("jon", &user);
   s_user = user.id;
}

void tearDown(void) {
   auth_db_shutdown();
}

static int64_t add(int64_t conv, const conv_message_row_t *row) {
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_add_row(conv, s_user, row, &id));
   return id;
}

/* The outside kind extraction gives @p hist for @p conv from the start. */
static int outside_of(int64_t conv, struct json_object *hist) {
   int kind = -1;
   json_object_put(memory_extraction_build_input(s_user, conv, hist, 0, &kind));
   json_object_put(hist);
   return kind;
}

/* As recovery and a queued extraction read it: the conversation loaded from
 * the database. */
static int stored_outside(int64_t conv) {
   struct json_object *hist = memory_history_load_from_db(conv, s_user, NULL);
   TEST_ASSERT_NOT_NULL(hist);
   return outside_of(conv, hist);
}

static void test_loaded_history_reads_request_context(void) {
   int64_t ctx = 0, job = 0, clean = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "Context", &ctx));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "Job", &job));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "Clean", &clean));

   const int64_t q = add(ctx, &(conv_message_row_t){ .role = "user", .content = "today?" });
   add(ctx, &(conv_message_row_t){ .role = "user",
                                   .kind = "turn_context",
                                   .context_of = q,
                                   .content = "[M1 calendar_event outside 2026-10-01] Invite" });
   add(ctx, &(conv_message_row_t){ .role = "assistant", .content = "One invite." });
   TEST_ASSERT_EQUAL_INT(OUTSIDE_OTHER, stored_outside(ctx));

   add(job, &(conv_message_row_t){ .role = "user", .content = "start it" });
   add(job, &(conv_message_row_t){ .role = "user",
                                   .kind = "envelope",
                                   .content = "[automated background-job update] Y" });
   add(job, &(conv_message_row_t){ .role = "assistant", .content = "It says Y." });
   TEST_ASSERT_EQUAL_INT(OUTSIDE_RESEARCH, stored_outside(job));

   add(clean, &(conv_message_row_t){ .role = "user", .content = "my dog is Ash" });
   add(clean, &(conv_message_row_t){ .role = "assistant", .content = "Noted." });
   TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, stored_outside(clean));
}

/* A stored turn context naming only recalled memories (learned from outside
 * text or not) and a note's chunk read no outside text; a document's chunk
 * beside them does. */
static void test_recalled_memories_are_not_outside(void) {
   static const char k_recalled[] = "[M1 memory_fact told, outside 2026-10-01] A memo\n"
                                    "[M2 memory_relation outside 2026-10-01] Jon works_at Acme\n"
                                    "[memory_summary outside] A talk\n"
                                    "[M3 memory_entity outside] Acme\n"
                                    "[M4 document_chunk 2026-10-01] [Groceries] Eggs";
   int64_t mem = 0, doc = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "Memories", &mem));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "Document", &doc));
   int64_t q = add(mem, &(conv_message_row_t){ .role = "user", .content = "Acme?" });
   add(mem, &(conv_message_row_t){ .role = "user",
                                   .kind = "turn_context",
                                   .context_of = q,
                                   .content = k_recalled });
   add(mem, &(conv_message_row_t){ .role = "assistant", .content = "Jon works there." });
   TEST_ASSERT_EQUAL_INT(OUTSIDE_NONE, stored_outside(mem));

   char both[512];
   snprintf(both, sizeof(both), "%s\n[M5 document_chunk outside 2026-10-01] [a.pdf] A page",
            k_recalled);
   q = add(doc, &(conv_message_row_t){ .role = "user", .content = "Acme?" });
   add(doc, &(conv_message_row_t){ .role = "user",
                                   .kind = "turn_context",
                                   .context_of = q,
                                   .content = both });
   add(doc, &(conv_message_row_t){ .role = "assistant", .content = "It's in a.pdf." });
   TEST_ASSERT_EQUAL_INT(OUTSIDE_DOCUMENT, stored_outside(doc));
}

/* A compaction's summary in the live history: the rows it replaced are read
 * from the database, tool results and request context alike. */
static void test_compacted_rows_are_read_from_the_database(void) {
   int64_t mail = 0, ctx = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "Mail", &mail));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "Context", &ctx));
   add(mail, &(conv_message_row_t){ .role = "user", .content = "read my mail" });
   add(mail,
       &(conv_message_row_t){ .role = "tool",
                              .tool_call_id = "c1",
                              .content = "--- EMAIL CONTENT ---\nHi\n--- END EMAIL CONTENT ---" });
   const int64_t q2 = add(mail, &(conv_message_row_t){ .role = "user", .content = "And then?" });
   const int64_t q3 = add(ctx, &(conv_message_row_t){ .role = "user", .content = "today?" });
   add(ctx, &(conv_message_row_t){ .role = "user",
                                   .kind = "turn_context",
                                   .context_of = q3,
                                   .content = "[M1 calendar_event outside 2026-10-01] Invite" });
   const int64_t q4 = add(ctx, &(conv_message_row_t){ .role = "user", .content = "And then?" });

   char live[512];
   static const char k_live[] =
       "[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"_kind\":\"summary\",\"text\":"
       "\"Earlier.\"},{\"type\":\"text\",\"text\":\"And then?\"}],\"id\":%lld}]";
   snprintf(live, sizeof(live), k_live, (long long)q2);
   TEST_ASSERT_EQUAL_INT(OUTSIDE_EMAIL, outside_of(mail, json_tokener_parse(live)));
   snprintf(live, sizeof(live), k_live, (long long)q4);
   TEST_ASSERT_EQUAL_INT(OUTSIDE_OTHER, outside_of(ctx, json_tokener_parse(live)));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_loaded_history_reads_request_context);
   RUN_TEST(test_compacted_rows_are_read_from_the_database);
   RUN_TEST(test_recalled_memories_are_not_outside);
   return UNITY_END();
}
