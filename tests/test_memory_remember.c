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
 *
 * Unit tests for the remember tool (memoryCallback) on a real schema: the
 * conversation a remembered fact is recorded as learned in, and the refusal
 * when a turn has more facts waiting for its conversation than it can track.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "memory/memory_db.h"
#include "memory/memory_db_entities.h"
#include "unity.h"

char *memoryCallback(const char *actionName, char *value, int *should_respond);

/* Test hooks in bench_retrieval_stub.c. */
extern session_t *g_stub_command_context;
extern int g_stub_defer_result;
extern int64_t g_stub_defer_conv;

static session_t s_session;
static int s_user;

static char *remember(const char *fact) {
   char value[256];
   snprintf(value, sizeof(value), "%s", fact);
   int respond = 0;
   return memoryCallback("remember", value, &respond);
}

/* The fact row for @p text: its source conversation (0 = none) and whether it
 * counts as learned outside any conversation.  False when there is no row. */
static bool fact_row(const char *text, int64_t *conv_out, int *unsourced_out) {
   sqlite3_stmt *st = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT source_conversation_id, origin_unsourced FROM memory_facts "
                      "WHERE user_id = ? AND fact_text = ?",
                      -1, &st, NULL);
   sqlite3_bind_int(st, 1, s_user);
   sqlite3_bind_text(st, 2, text, -1, SQLITE_STATIC);
   const bool found = sqlite3_step(st) == SQLITE_ROW;
   if (found) {
      *conv_out = sqlite3_column_type(st, 0) == SQLITE_NULL ? 0 : sqlite3_column_int64(st, 0);
      *unsourced_out = sqlite3_column_int(st, 1);
   }
   sqlite3_finalize(st);
   return found;
}

void setUp(void) {
   config_set_defaults(&g_config);
   g_config.memory.enabled = true;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   auth_db_create_user("rememberer", "hash", false);
   auth_user_t user;
   memset(&user, 0, sizeof(user));
   auth_db_get_user("rememberer", &user);
   s_user = user.id;
   memset(&s_session, 0, sizeof(s_session));
   s_session.type = SESSION_TYPE_WEBUI;
   s_session.metrics.user_id = s_user;
   g_stub_command_context = &s_session;
   g_stub_defer_result = FAILURE;
   g_stub_defer_conv = 0;
}

void tearDown(void) {
   g_stub_command_context = NULL;
   auth_db_shutdown();
}

void test_remember_in_a_known_conversation_is_learned_there(void) {
   int64_t conv = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "Chat", &conv));
   g_stub_defer_result = SUCCESS;
   g_stub_defer_conv = conv;
   free(remember("My locker number is 314"));
   int64_t source = 0;
   int unsourced = -1;
   TEST_ASSERT_TRUE(fact_row("My locker number is 314", &source, &unsourced));
   TEST_ASSERT_EQUAL_INT64(conv, source);
   TEST_ASSERT_EQUAL_INT(0, unsourced); /* forgetting that conversation removes it */
}

void test_remember_outside_a_turn_is_stated_outside_any_conversation(void) {
   g_stub_defer_result = FAILURE; /* the scheduler, MQTT */
   free(remember("My bike is green"));
   int64_t source = -1;
   int unsourced = -1;
   TEST_ASSERT_TRUE(fact_row("My bike is green", &source, &unsourced));
   TEST_ASSERT_EQUAL_INT64(0, source);
   TEST_ASSERT_EQUAL_INT(1, unsourced);
}

void test_remember_waiting_for_its_conversation_is_kept(void) {
   g_stub_defer_result = SESSION_FACT_SOURCE_QUEUED; /* a new chat's first message */
   char *r = remember("My hat size is seven");
   TEST_ASSERT_NULL(strstr(r, "Too many facts"));
   free(r);
   int64_t source = -1;
   int unsourced = -1;
   TEST_ASSERT_TRUE(fact_row("My hat size is seven", &source, &unsourced));
   TEST_ASSERT_EQUAL_INT64(0, source); /* recorded once the conversation exists */
}

void test_remember_past_the_waiting_limit_is_refused_and_not_stored(void) {
   /* Stored, it could never be tied to its conversation, and forgetting that
    * conversation would keep it: refused, so the model can say so. */
   g_stub_defer_result = SESSION_FACT_SOURCE_DROPPED;
   char *r = remember("My gate code is 2468");
   TEST_ASSERT_NOT_NULL(strstr(r, "Too many facts"));
   free(r);
   int64_t source = 0;
   int unsourced = 0;
   TEST_ASSERT_FALSE(fact_row("My gate code is 2468", &source, &unsourced));
}

/* The id of the user's fact with @p text, 0 if none. */
static int64_t fact_id_of(const char *text) {
   sqlite3_stmt *st = NULL;
   sqlite3_prepare_v2(s_db.db, "SELECT id FROM memory_facts WHERE user_id = ? AND fact_text = ?",
                      -1, &st, NULL);
   sqlite3_bind_int(st, 1, s_user);
   sqlite3_bind_text(st, 2, text, -1, SQLITE_STATIC);
   const int64_t id = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
   sqlite3_finalize(st);
   return id;
}

static char *forget(int64_t id, const char *mode, int64_t keeper) {
   char value[96];
   if (mode) {
      snprintf(value, sizeof(value), "%lld::%s::%lld", (long long)id, mode, (long long)keeper);
   } else {
      snprintf(value, sizeof(value), "%lld", (long long)id);
   }
   int respond = 0;
   return memoryCallback("forget", value, &respond);
}

/* 'newer_fact_id' files the old value as replaced, so forgetting the newer one
 * brings it back (and says so); 'duplicate_of' merges, so forgetting the kept
 * fact takes the duplicate too. */
void test_forget_tells_a_replacement_from_a_duplicate(void) {
   free(remember("My phone number is 555-0101"));
   free(remember("My phone number is now 555-0202"));
   const int64_t old = fact_id_of("My phone number is 555-0101");
   const int64_t fresh = fact_id_of("My phone number is now 555-0202");
   TEST_ASSERT_TRUE(old > 0 && fresh > 0);
   free(forget(old, "newer_fact_id", fresh));
   memory_fact_t f;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_get(old, s_user, &f));
   TEST_ASSERT_EQUAL_INT64(fresh, f.superseded_by);
   char *r = forget(fresh, NULL, 0);
   TEST_ASSERT_NOT_NULL(strstr(r, "Current again"));
   TEST_ASSERT_NOT_NULL(strstr(r, "555-0101"));
   free(r);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_get(old, s_user, &f));
   TEST_ASSERT_EQUAL_INT64(0, f.superseded_by);

   free(remember("My bike is a Trek"));
   free(remember("I ride a Trek bike"));
   const int64_t keep = fact_id_of("My bike is a Trek");
   const int64_t dup = fact_id_of("I ride a Trek bike");
   free(forget(dup, "duplicate_of", keep));
   r = forget(keep, NULL, 0);
   TEST_ASSERT_NULL(strstr(r, "Current again"));
   free(r);
   TEST_ASSERT_EQUAL_INT64(0, fact_id_of("I ride a Trek bike"));

   r = forget(dup, "duplicate_of", keep);
   TEST_ASSERT_NOT_NULL(strstr(r, "wasn't found")); /* the kept fact is gone: refused */
   free(r);
}

/* 'replaced_by' is what 'duplicate_of' was called before: still a merge. */
void test_forget_replaced_by_still_merges(void) {
   free(remember("My car is a Volvo"));
   free(remember("I drive a Volvo"));
   const int64_t keep = fact_id_of("My car is a Volvo");
   const int64_t dup = fact_id_of("I drive a Volvo");
   free(forget(dup, "replaced_by", keep));
   char *r = forget(keep, NULL, 0);
   TEST_ASSERT_NULL(strstr(r, "Current again"));
   free(r);
   TEST_ASSERT_EQUAL_INT64(0, fact_id_of("I drive a Volvo"));
}

/* Two kept-fact parameters, or a malformed one, change nothing. */
void test_forget_refuses_conflicting_or_malformed_keepers(void) {
   free(remember("My dog is Rex"));
   free(remember("My dog is called Rex"));
   const int64_t a = fact_id_of("My dog is Rex");
   const int64_t b = fact_id_of("My dog is called Rex");
   char value[128];
   snprintf(value, sizeof(value), "%lld::duplicate_of::%lld::newer_fact_id::%lld", (long long)b,
            (long long)a, (long long)a);
   int respond = 0;
   char *r = memoryCallback("forget", value, &respond);
   TEST_ASSERT_NOT_NULL(strstr(r, "not both"));
   free(r);
   snprintf(value, sizeof(value), "%lld::duplicate_of::%lldx", (long long)b, (long long)a);
   r = memoryCallback("forget", value, &respond);
   TEST_ASSERT_NOT_NULL(strstr(r, "single positive number"));
   free(r);
   memory_fact_t f;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_get(b, s_user, &f));
   TEST_ASSERT_EQUAL_INT64(0, f.superseded_by);
   /* 0 is "none" (a model filling an optional number): a plain forget. */
   snprintf(value, sizeof(value), "%lld::duplicate_of::0", (long long)b);
   r = memoryCallback("forget", value, &respond);
   TEST_ASSERT_NOT_NULL(strstr(r, "Forgotten"));
   free(r);
   TEST_ASSERT_EQUAL_INT64(0, fact_id_of("My dog is called Rex"));
}

/* list_merged names how each one was superseded; restore brings one back and
 * says when the newer value is still current. */
void test_list_merged_and_restore(void) {
   free(remember("My locker code is 1111"));
   free(remember("My locker code is now 2222"));
   const int64_t old = fact_id_of("My locker code is 1111");
   const int64_t fresh = fact_id_of("My locker code is now 2222");
   free(forget(old, "newer_fact_id", fresh));
   int respond = 0;
   char empty[] = "";
   char *r = memoryCallback("list_merged", empty, &respond);
   TEST_ASSERT_NOT_NULL(strstr(r, "replaced by"));
   TEST_ASSERT_NOT_NULL(strstr(r, "1111"));
   free(r);
   char id[32];
   snprintf(id, sizeof(id), "%lld", (long long)old);
   r = memoryCallback("restore", id, &respond);
   TEST_ASSERT_NOT_NULL(strstr(r, "Restored memory"));
   TEST_ASSERT_NOT_NULL(strstr(r, "still current too"));
   free(r);
   r = memoryCallback("restore", id, &respond); /* current now: nothing to restore */
   TEST_ASSERT_NOT_NULL(strstr(r, "No merged or replaced memory"));
   free(r);
}

/* A merge of a fact already merged says so, not "not found". */
void test_forget_reports_already_merged(void) {
   free(remember("My bag is blue"));
   free(remember("I carry a blue bag"));
   free(remember("My umbrella is green"));
   const int64_t a = fact_id_of("My bag is blue");
   const int64_t b = fact_id_of("I carry a blue bag");
   const int64_t c = fact_id_of("My umbrella is green");
   TEST_ASSERT_TRUE(a > 0 && b > 0 && c > 0);
   free(forget(b, "duplicate_of", a));
   char *r = forget(b, "duplicate_of", c);
   TEST_ASSERT_NOT_NULL(strstr(r, "already merged or replaced"));
   free(r);
}

static char *search(const char *value) {
   char buf[256];
   snprintf(buf, sizeof(buf), "%s", value);
   int respond = 0;
   return memoryCallback("search", buf, &respond);
}

/* Search shows relations only when asked about a time or for closed ones, and
 * then filters them as asked. */
void test_search_shows_relations_only_when_asked(void) {
   int64_t alice = 0, acme = 0, boston = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_upsert(s_user, "Alice", "person", "alice", NULL, &alice));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_upsert(s_user, "Acme", "org", "acme", NULL, &acme));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_entity_upsert(s_user, "Boston", "place",
                                                                    "boston", NULL, &boston));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_relation_create(s_user, alice, "works_at", acme, NULL, 0, 0.9f,
                                                   0, 0, NULL));
   /* Lived in Boston in 2001-2002, closed since. */
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_relation_create(s_user, alice, "lived_in", boston, NULL, 0, 0.9f,
                                                   978307200, 1041379200, NULL));

   /* Bob mentored Alice in 2001-2002: an incoming relation, filtered too. */
   int64_t bob = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_upsert(s_user, "Bob", "person", "bob", NULL, &bob));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_relation_create(s_user, bob, "mentored", alice, NULL, 0, 0.9f,
                                                   978307200, 1041379200, NULL));

   char *r = search("Alice");
   TEST_ASSERT_NULL(strstr(r, "works_at"));
   free(r);

   r = search("Alice::include_historical::true");
   TEST_ASSERT_NOT_NULL(strstr(r, "works_at: Acme"));
   TEST_ASSERT_NOT_NULL(strstr(r, "lived_in: Boston"));
   TEST_ASSERT_NOT_NULL(strstr(r, "mentored"));
   free(r);

   r = search("Alice::as_of::2001-06-01");
   TEST_ASSERT_NOT_NULL(strstr(r, "lived_in: Boston"));
   TEST_ASSERT_NOT_NULL(strstr(r, "mentored"));
   free(r);

   r = search("Alice::as_of::2030-01-01");
   TEST_ASSERT_NOT_NULL(strstr(r, "works_at: Acme"));
   TEST_ASSERT_NULL(strstr(r, "lived_in"));
   TEST_ASSERT_NULL(strstr(r, "mentored"));
   free(r);

   /* More ended incoming relations than the block's eight slots: the current
    * one still shows (validity is filtered before the limit). */
   for (int i = 0; i < 9; i++) {
      char name[16];
      snprintf(name, sizeof(name), "Old%d", i);
      int64_t old = 0;
      TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                            memory_db_entity_upsert(s_user, name, "person", name, NULL, &old));
      TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                            memory_db_relation_create(s_user, old, "coached", alice, NULL, 0, 0.9f,
                                                      978307200, 1041379200, NULL));
   }
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_relation_create(s_user, bob, "befriended", alice, NULL, 0, 0.9f,
                                                   0, 0, NULL));
   r = search("Alice::as_of::2030-01-01");
   TEST_ASSERT_NOT_NULL(strstr(r, "befriended"));
   TEST_ASSERT_NULL(strstr(r, "coached"));
   free(r);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_remember_in_a_known_conversation_is_learned_there);
   RUN_TEST(test_remember_outside_a_turn_is_stated_outside_any_conversation);
   RUN_TEST(test_remember_waiting_for_its_conversation_is_kept);
   RUN_TEST(test_remember_past_the_waiting_limit_is_refused_and_not_stored);
   RUN_TEST(test_forget_tells_a_replacement_from_a_duplicate);
   RUN_TEST(test_forget_replaced_by_still_merges);
   RUN_TEST(test_forget_refuses_conflicting_or_malformed_keepers);
   RUN_TEST(test_list_merged_and_restore);
   RUN_TEST(test_forget_reports_already_merged);
   RUN_TEST(test_search_shows_relations_only_when_asked);
   return UNITY_END();
}
