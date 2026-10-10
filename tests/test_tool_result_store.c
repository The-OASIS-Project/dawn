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
 * Stored tool results (real auth_db): who may read one, binding to a
 * conversation, and that they go with their conversation and user.
 */

#include <json-c/json.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "auth/auth_db_tool_results.h"
#include "core/session_manager.h"
#include "core/tool_result_store.h"
#include "test_tmp.h"
#include "tools/tool_registry.h"
#include "unity.h"

static char TEST_DB[TEST_TMP_PATH_MAX];

/* The calling thread's turn token (session_manager.c): set by a test that
 * plays a turn. */
static uint64_t s_token;
uint64_t session_turn_token(void) {
   return s_token;
}

/* The session's effective user (session_manager.c): its own user here. */
int session_effective_user_id(session_t *session) {
   return session ? session->metrics.user_id : 0;
}

/* The conversation's tag, masked (session_prefix.c): "TAGSECRET" stands in. */
char *session_prefix_mask_secret(struct session *session, char *text) {
   (void)session;
   for (char *p = text; (p = strstr(p, "TAGSECRET")) != NULL;) {
      memcpy(p, "*********", 9);
   }
   return text;
}

static void bind_to(session_t *s, int64_t conv, uint64_t token, bool ended) {
   pthread_mutex_lock(&s->history_mutex);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, tool_result_store_bind_locked(s, conv, token, ended));
   pthread_mutex_unlock(&s->history_mutex);
}

/* The caller's conversation in @p s: its turn's, else the live history's. */
static session_t *at(session_t *s, int64_t conv) {
   if (s && s->turn_active) {
      s->turn_history_conv = conv;
   } else if (s) {
      atomic_store(&s->history_conversation_id, conv);
   }
   return s;
}

/* @p s acts for @p user_id (its effective user, which the store reads). */
static session_t *as_user(session_t *s, int user_id) {
   if (s) {
      s->metrics.user_id = user_id;
   }
   return s;
}

/* @p s runs turn @p token on its own history (0: no turn). */
static void play_turn(session_t *s, uint64_t token) {
   s->turn_active = token != 0;
   s->turn_owner_token = token;
   s_token = token;
}
static int alice = 0;
static int bob = 0;

static session_t *new_session(uint32_t id) {
   session_t *s = calloc(1, sizeof(*s));
   TEST_ASSERT_NOT_NULL(s);
   s->session_id = id;
   pthread_mutex_init(&s->history_mutex, NULL);
   return s;
}

static void free_session(session_t *s) {
   tool_result_store_free(s);
   pthread_mutex_destroy(&s->history_mutex);
   free(s);
}

static int user(const char *name) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user(name, "h", false));
   auth_user_t u;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user(name, &u));
   return u.id;
}

static int64_t conversation(int owner) {
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(owner, "c", &id));
   return id;
}

void setUp(void) {
   unlink(TEST_DB);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(TEST_DB));
   alice = user("alice");
   bob = user("bob");
}

void tearDown(void) {
   auth_db_shutdown();
   unlink(TEST_DB);
}

static void put(session_t *s, int u, int64_t conv, char id[TOOL_RESULTS_ID_LEN]) {
   const char *json = "{\"results\":[1,2,3]}";
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_STORE_OK,
                         tool_result_store_put(as_user(at(s, conv), u), "cbm_search", "call_1",
                                               json, strlen(json), true, id, NULL));
   TEST_ASSERT_EQUAL_INT(TOOL_RESULTS_ID_LEN - 1, strlen(id));
}

static int open_as(session_t *s, int u, int64_t conv, const char *id) {
   tool_result_doc_t doc;
   const int rc = tool_result_store_open(at(s, conv), u, id, &doc);
   if (rc == TOOL_RESULT_OPEN_OK) {
      TEST_ASSERT_NULL(doc.body); /* the row only, until a read needs the body */
      TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_OK, tool_result_store_load_body(&doc));
      TEST_ASSERT_EQUAL_STRING("{\"results\":[1,2,3]}", doc.body);
      TEST_ASSERT_EQUAL_INT(TOOL_RESULTS_JSON, doc.meta.kind);
      TEST_ASSERT_EQUAL_STRING("cbm_search", doc.meta.tool_name);
   }
   tool_result_store_close(&doc);
   return rc;
}

/* A result stored as someone else's text keeps its frame, whichever it is
 * (and its kind); one stored without reads as none. */
static void test_a_stored_frame_comes_back(void) {
   session_t *s = new_session(1);
   const int64_t conv = conversation(alice);
   char id[TOOL_RESULTS_ID_LEN];
   const char *json = "{\"results\":[1,2,3]}";
   static const char *const frames[] = TOOL_FRAMES_ALL;
   tool_result_doc_t doc;
   for (size_t i = 0; i < sizeof(frames) / sizeof(frames[0]); i++) {
      char call_id[16];
      snprintf(call_id, sizeof(call_id), "call_%zu", i + 1);
      TEST_ASSERT_EQUAL_INT(TOOL_RESULT_STORE_OK,
                            tool_result_store_put_framed(as_user(at(s, conv), alice),
                                                         "execute_plan", call_id, json,
                                                         strlen(json), true, frames[i], id, NULL));
      TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_OK,
                            tool_result_store_open(at(s, conv), alice, id, &doc));
      TEST_ASSERT_EQUAL_STRING(frames[i], tool_result_store_frame(&doc));
      TEST_ASSERT_EQUAL_INT(TOOL_RESULTS_JSON, doc.meta.kind);
      tool_result_store_close(&doc);
   }
   put(s, alice, conv, id);
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_OK, tool_result_store_open(at(s, conv), alice, id, &doc));
   TEST_ASSERT_NULL(tool_result_store_frame(&doc));
   tool_result_store_close(&doc);
   free_session(s);
}

/* Readable in its own conversation, by its user, from any of their sessions;
 * refused in another conversation (a continuation included), to another
 * user, and for an unknown or malformed handle. */
static void test_scope_of_a_bound_result(void) {
   session_t *s1 = new_session(1);
   session_t *s2 = new_session(2);
   const int64_t a = conversation(alice);
   const int64_t b = conversation(alice); /* another, or a continuation of a */
   char id[TOOL_RESULTS_ID_LEN];
   put(s1, alice, a, id);

   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_OK, open_as(s1, alice, a, id));
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_OK, open_as(s2, alice, a, id));
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_REFUSED, open_as(s1, alice, b, id));
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_REFUSED, open_as(s1, alice, 0, id));
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_REFUSED, open_as(s1, bob, a, id));
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_REFUSED, open_as(s1, 0, a, id)); /* a guest */
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_REFUSED, open_as(NULL, alice, a, id));
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_REFUSED, open_as(s1, alice, a, "trs_000000000000"));
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_REFUSED, open_as(s1, alice, a, "blb_abcdefabcdef"));
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_REFUSED, open_as(s1, alice, a, "trs_x' OR 1=1--"));
   free_session(s1);
   free_session(s2);
}

/* A guest's result, or one with no session, is never stored. */
static void test_a_guest_stores_nothing(void) {
   session_t *s = new_session(1);
   char id[TOOL_RESULTS_ID_LEN];
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_STORE_NO_USER,
                         tool_result_store_put(as_user(at(s, 0), 0), "t", NULL, "x", 1, false, id,
                                               NULL));
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_STORE_NO_USER,
                         tool_result_store_put(as_user(at(NULL, 0), alice), "t", NULL, "x", 1,
                                               false, id, NULL));
   free_session(s);
}

/* Unbound (no conversation yet): only the session that stored it reads it,
 * until its history starts over; once bound, the conversation's. */
static void test_an_unbound_result(void) {
   session_t *s1 = new_session(1);
   session_t *s2 = new_session(2);
   char id[TOOL_RESULTS_ID_LEN];
   put(s1, alice, 0, id);
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_OK, open_as(s1, alice, 0, id));
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_REFUSED, open_as(s2, alice, 0, id));

   const int64_t a = conversation(alice);
   bind_to(s1, a, 0, true);
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_OK, open_as(s2, alice, a, id));
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_REFUSED, open_as(s2, alice, 0, id));

   /* Bound, the minted set no longer reaches it from another conversation. */
   const int64_t b = conversation(alice);
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_REFUSED, open_as(s1, alice, b, id));

   /* An unbound one is the session's until its history starts over. */
   char id2[TOOL_RESULTS_ID_LEN];
   put(s1, alice, 0, id2);
   pthread_mutex_lock(&s1->history_mutex);
   tool_result_store_reset_locked(s1);
   pthread_mutex_unlock(&s1->history_mutex);
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_REFUSED, open_as(s1, alice, 0, id2));
   /* And binding after that doesn't take it (it isn't the session's now). */
   bind_to(s1, b, 0, true);
   tool_results_meta_t meta;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, tool_results_db_get(id2, &meta, NULL, NULL));
   TEST_ASSERT_EQUAL_INT64(0, meta.conversation_id);
   free_session(s1);
   free_session(s2);
}

/* Binding never takes another user's result, or one already bound. */
static void test_bind_is_the_users_unbound_only(void) {
   session_t *s = new_session(1);
   const int64_t a = conversation(alice);
   const int64_t other = conversation(bob);
   char mine[TOOL_RESULTS_ID_LEN];
   char bound[TOOL_RESULTS_ID_LEN];
   put(s, alice, 0, mine);
   put(s, alice, a, bound);
   bind_to(s, other, 0, true); /* bob's conversation: alice's results aren't bound to it */
   tool_results_meta_t meta;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, tool_results_db_get(mine, &meta, NULL, NULL));
   TEST_ASSERT_EQUAL_INT64(0, meta.conversation_id);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, tool_results_db_get(bound, &meta, NULL, NULL));
   TEST_ASSERT_EQUAL_INT64(a, meta.conversation_id);
   free_session(s);
}

/* Results go with their conversation, and with their user. */
static void test_they_go_with_their_conversation_and_user(void) {
   session_t *s = new_session(1);
   const int64_t a = conversation(alice);
   const int64_t kept = conversation(alice);
   const int64_t bobs = conversation(bob);
   char in_a[TOOL_RESULTS_ID_LEN];
   char in_kept[TOOL_RESULTS_ID_LEN];
   char unbound[TOOL_RESULTS_ID_LEN];
   char of_bob[TOOL_RESULTS_ID_LEN];
   put(s, alice, a, in_a);
   put(s, alice, kept, in_kept);
   put(s, alice, 0, unbound);
   put(s, bob, bobs, of_bob);
   tool_results_meta_t meta;

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_delete(a, alice));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, tool_results_db_get(in_a, &meta, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, tool_results_db_get(in_kept, &meta, NULL, NULL));

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_delete_user("alice"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, tool_results_db_get(in_kept, &meta, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, tool_results_db_get(unbound, &meta, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, tool_results_db_get(of_bob, &meta, NULL, NULL));
   free_session(s);
}

/* Unbound results past the grace are reclaimed; bound ones never are. */
static void test_unbound_results_are_reclaimed(void) {
   session_t *s = new_session(1);
   const int64_t a = conversation(alice);
   char unbound[TOOL_RESULTS_ID_LEN];
   char bound[TOOL_RESULTS_ID_LEN];
   put(s, alice, 0, unbound);
   put(s, alice, a, bound);
   int deleted = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         tool_results_db_reclaim_unbound((int64_t)time(NULL) + 10, &deleted));
   TEST_ASSERT_EQUAL_INT(1, deleted);
   tool_results_meta_t meta;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, tool_results_db_get(unbound, &meta, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, tool_results_db_get(bound, &meta, NULL, NULL));
   free_session(s);
}

/* A result larger than the store keeps: its head and tail, said so, as text. */
static void test_a_huge_result_keeps_head_and_tail(void) {
   session_t *s = new_session(1);
   const int64_t a = conversation(alice);
   const size_t len = TOOL_RESULT_STORE_MAX_BYTES + 1000;
   char *text = malloc(len);
   TEST_ASSERT_NOT_NULL(text);
   for (size_t i = 0; i < len; i++) {
      text[i] = (i % 80 == 79) ? '\n' : (char)('a' + i % 26);
   }
   memcpy(text, "HEAD", 4);
   memcpy(text + len - 4, "TAIL", 4);
   char id[TOOL_RESULTS_ID_LEN];
   bool cut = false;
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_STORE_OK,
                         tool_result_store_put(as_user(at(s, a), alice), "big", NULL, text, len,
                                               true, id, &cut));
   TEST_ASSERT_TRUE(cut);
   tool_result_doc_t doc;
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_OK, tool_result_store_open(at(s, a), alice, id, &doc));
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_OK, tool_result_store_load_body(&doc));
   TEST_ASSERT_TRUE(doc.len <= TOOL_RESULT_STORE_MAX_BYTES);
   TEST_ASSERT_EQUAL_INT(0, strncmp(doc.body, "HEAD", 4));
   TEST_ASSERT_EQUAL_INT(0, strcmp(doc.body + doc.len - 4, "TAIL"));
   TEST_ASSERT_NOT_NULL(strstr(doc.body, "bytes omitted: the result was larger than DAWN keeps"));
   TEST_ASSERT_EQUAL_INT(TOOL_RESULTS_TEXT, doc.meta.kind);
   tool_result_store_close(&doc);
   free(text);
   free_session(s);
}

/* A JSON result's tree is parsed once for the turn, then dropped. */
static void test_the_tree_cache(void) {
   session_t *s = new_session(1);
   const int64_t a = conversation(alice);
   char id[TOOL_RESULTS_ID_LEN];
   put(s, alice, a, id);
   tool_result_doc_t doc;
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_OK, tool_result_store_open(at(s, a), alice, id, &doc));
   tool_result_tree_t t1;
   TEST_ASSERT_TRUE(tool_result_store_tree_acquire(s, &doc, &t1));
   TEST_ASSERT_NOT_NULL(t1.slot); /* cached */
   struct json_object *first = t1.tree;
   tool_result_store_tree_release(&t1);
   tool_result_tree_t t2;
   TEST_ASSERT_TRUE(tool_result_store_tree_acquire(s, &doc, &t2));
   TEST_ASSERT_TRUE(t2.tree == first); /* the same tree: not parsed again */
   tool_result_store_tree_release(&t2);
   tool_result_store_drop_trees(s);
   /* Text isn't parsed. */
   doc.meta.kind = TOOL_RESULTS_TEXT;
   tool_result_tree_t t3;
   TEST_ASSERT_FALSE(tool_result_store_tree_acquire(s, &doc, &t3));
   tool_result_store_close(&doc);
   free_session(s);
}

/* Eviction and the reclaim read the age-ordered indexes (no sort), and a
 * cap check is a key lookup. */
static void test_queries_use_the_indexes(void) {
   sqlite3 *db = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(TEST_DB, &db));
   static const struct {
      const char *sql;
      const char *plan;
   } k_plans[] = {
      { "SELECT rowid, bytes FROM tool_results WHERE conversation_id = 1 ORDER BY created_at",
        "COVERING INDEX idx_tool_results_conv" },
      { "SELECT rowid, bytes FROM tool_results WHERE conversation_id IS NULL AND user_id = 1 "
        "ORDER BY created_at",
        "idx_tool_results_" },
      { "SELECT rowid, bytes FROM tool_results WHERE user_id = 1 ORDER BY created_at",
        "COVERING INDEX idx_tool_results_user" },
      { "SELECT rowid, bytes FROM tool_results ORDER BY rowid", "SCAN tool_results" },
      { "SELECT bytes FROM tool_results_usage WHERE scope = 1 AND key = 1", "PRIMARY KEY" },
      { "DELETE FROM tool_results WHERE conversation_id IS NULL AND created_at < 5",
        "COVERING INDEX idx_tool_results_conv" },
   };
   for (size_t i = 0; i < sizeof(k_plans) / sizeof(k_plans[0]); i++) {
      char sql[512];
      snprintf(sql, sizeof(sql), "EXPLAIN QUERY PLAN %s", k_plans[i].sql);
      sqlite3_stmt *st = NULL;
      TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(db, sql, -1, &st, NULL));
      char plan[512] = "";
      while (sqlite3_step(st) == SQLITE_ROW) {
         const size_t used = strlen(plan);
         snprintf(plan + used, sizeof(plan) - used, "%s ",
                  (const char *)sqlite3_column_text(st, 3));
      }
      sqlite3_finalize(st);
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(plan, k_plans[i].plan), plan);
      TEST_ASSERT_NULL_MESSAGE(strstr(plan, "TEMP B-TREE"), plan);
   }
   sqlite3_close(db);
}

/* The usage the cap checks read is the tables' own sums, through stores,
 * binds, deletes and their cascades. */
static int64_t scalar(sqlite3 *db, const char *sql) {
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(db, sql, -1, &st, NULL));
   int64_t v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
   sqlite3_finalize(st);
   return v;
}

static void usage_matches(void) {
   sqlite3 *db = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(TEST_DB, &db));
   TEST_ASSERT_EQUAL_INT64(
       scalar(db, "SELECT COALESCE(SUM(bytes), 0) FROM tool_results"),
       scalar(db, "SELECT COALESCE(SUM(bytes), 0) FROM tool_results_usage WHERE scope = 2"));
   TEST_ASSERT_EQUAL_INT64(
       0, scalar(db, "SELECT COUNT(*) FROM (SELECT user_id, SUM(bytes) b FROM tool_results GROUP "
                     "BY user_id) t LEFT JOIN tool_results_usage u ON u.scope = 1 AND u.key = "
                     "t.user_id WHERE u.bytes IS NOT t.b"));
   TEST_ASSERT_EQUAL_INT64(
       0, scalar(db, "SELECT COUNT(*) FROM (SELECT conversation_id c, SUM(bytes) b FROM "
                     "tool_results WHERE conversation_id IS NOT NULL GROUP BY c) t LEFT JOIN "
                     "tool_results_usage u ON u.scope = 0 AND u.key = t.c WHERE u.bytes IS NOT "
                     "t.b"));
   TEST_ASSERT_EQUAL_INT64(
       0, scalar(db, "SELECT COUNT(*) FROM tool_results_usage WHERE bytes < 0 OR (scope < 2 AND "
                     "bytes = 0)"));
   sqlite3_close(db);
}

static void test_usage_stays_exact(void) {
   session_t *s = new_session(1);
   const int64_t a = conversation(alice);
   const int64_t b = conversation(alice);
   const int64_t c = conversation(bob);
   char ids[5][TOOL_RESULTS_ID_LEN];
   put(s, alice, a, ids[0]);
   put(s, alice, 0, ids[1]);
   put(s, alice, b, ids[2]);
   put(s, bob, c, ids[3]);
   put(s, alice, 0, ids[4]);
   usage_matches();
   bind_to(s, b, 0, true);
   usage_matches();
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_delete(a, alice));
   usage_matches();
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, tool_results_db_delete(ids[3]));
   usage_matches();
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_delete_user("alice"));
   usage_matches();
   free_session(s);
}

/* A result is stored with the conversation's tag masked. */
static void test_the_tag_is_masked_at_rest(void) {
   session_t *s = new_session(1);
   const int64_t a = conversation(alice);
   const char *text = "before TAGSECRET after";
   char id[TOOL_RESULTS_ID_LEN];
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_STORE_OK,
                         tool_result_store_put(as_user(at(s, a), alice), "t", NULL, text,
                                               strlen(text), false, id, NULL));
   tool_result_doc_t doc;
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_OK, tool_result_store_open(at(s, a), alice, id, &doc));
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_OK, tool_result_store_load_body(&doc));
   TEST_ASSERT_EQUAL_STRING("before ********* after", doc.body);
   tool_result_store_close(&doc);
   free_session(s);
}

/* A result can't be stored into, or bound to, another user's conversation. */
static void test_only_the_users_conversation(void) {
   session_t *s = new_session(1);
   const int64_t bobs = conversation(bob);
   char id[TOOL_RESULTS_ID_LEN];
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_STORE_FAILED,
                         tool_result_store_put(as_user(at(s, bobs), alice), "t", NULL, "x", 1,
                                               false, id, NULL));
   free_session(s);
}

/* A turn's unbound results are its own: a new context (the user starts
 * another chat mid-turn) doesn't take them, another turn can't read them,
 * and they are bound with the turn's conversation, not the new context's. */
static void test_a_turns_results_are_its_own(void) {
   session_t *s = new_session(1);
   play_turn(s, 7);
   char mine[TOOL_RESULTS_ID_LEN];
   put(s, alice, 0, mine);
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_OK, open_as(s, alice, 0, mine));

   pthread_mutex_lock(&s->history_mutex);
   tool_result_store_reset_locked(s); /* the live history started over */
   pthread_mutex_unlock(&s->history_mutex);
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_OK, open_as(s, alice, 0, mine));

   play_turn(s, 8); /* another turn */
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_REFUSED, open_as(s, alice, 0, mine));
   char theirs[TOOL_RESULTS_ID_LEN];
   put(s, alice, 0, theirs);

   play_turn(s, 7);
   const int64_t a = conversation(alice);
   bind_to(s, a, 7, false); /* turn 7 learned its conversation */
   tool_results_meta_t meta;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, tool_results_db_get(mine, &meta, NULL, NULL));
   TEST_ASSERT_EQUAL_INT64(a, meta.conversation_id);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, tool_results_db_get(theirs, &meta, NULL, NULL));
   TEST_ASSERT_EQUAL_INT64(0, meta.conversation_id);
   play_turn(s, 0);
   free_session(s);
}

/* A turn that ends without a conversation: its results become the live
 * history's when it wrote there (the next turn reads them), else no one's. */
static void test_a_turn_ends_without_a_conversation(void) {
   session_t *s = new_session(1);
   play_turn(s, 3);
   char kept[TOOL_RESULTS_ID_LEN];
   put(s, alice, 0, kept);
   pthread_mutex_lock(&s->history_mutex);
   tool_result_store_turn_ended_locked(s, 3, true);
   pthread_mutex_unlock(&s->history_mutex);
   play_turn(s, 4); /* the next turn, on the live history */
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_OK, open_as(s, alice, 0, kept));

   char lost[TOOL_RESULTS_ID_LEN];
   put(s, alice, 0, lost);
   pthread_mutex_lock(&s->history_mutex);
   tool_result_store_turn_ended_locked(s, 4, false);
   pthread_mutex_unlock(&s->history_mutex);
   play_turn(s, 5);
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_REFUSED, open_as(s, alice, 0, lost));
   play_turn(s, 0);
   free_session(s);
}

/* A cached tree in use is pinned: dropping the session's trees meanwhile
 * frees it only when its reader lets go; another reader parses its own. */
static void test_a_pinned_tree(void) {
   session_t *s = new_session(1);
   const int64_t a = conversation(alice);
   char id[TOOL_RESULTS_ID_LEN];
   put(s, alice, a, id);
   tool_result_doc_t doc;
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_OK, tool_result_store_open(at(s, a), alice, id, &doc));
   tool_result_tree_t t1;
   TEST_ASSERT_TRUE(tool_result_store_tree_acquire(s, &doc, &t1));
   TEST_ASSERT_NOT_NULL(t1.slot);
   tool_result_tree_t t2;
   TEST_ASSERT_TRUE(tool_result_store_tree_acquire(s, &doc, &t2));
   TEST_ASSERT_TRUE(t2.owned && t2.tree != t1.tree); /* its own copy */
   tool_result_store_drop_trees(s);
   TEST_ASSERT_EQUAL_INT(3, json_object_array_length(json_object_object_get(t1.tree, "results")));
   tool_result_store_tree_release(&t1);
   tool_result_store_tree_release(&t2);
   tool_result_store_close(&doc);
   free_session(s);
}

/* A cached tree answers without the body; a large JSON result's tree waits
 * in the one large slot for its turn's follow-ups. */
static void test_the_body_waits_and_the_large_slot(void) {
   session_t *s = new_session(1);
   const int64_t a = conversation(alice);
   /* Past the regular slots' limit: [0,1,2,...] a little over 1 MB. */
   const size_t want = TOOL_RESULT_TREE_CACHE_TEXT_MAX + 4096;
   char *json = malloc(want + 32);
   size_t len = 0;
   json[len++] = '[';
   for (int i = 0; len < want; i++) {
      len += (size_t)sprintf(json + len, "%s%d", i ? "," : "", i);
   }
   json[len++] = ']';
   json[len] = '\0';
   char id[TOOL_RESULTS_ID_LEN];
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_STORE_OK,
                         tool_result_store_put(as_user(at(s, a), alice), "big", NULL, json, len,
                                               true, id, NULL));
   tool_result_doc_t doc;
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_OK, tool_result_store_open(at(s, a), alice, id, &doc));
   tool_result_tree_t t1;
   TEST_ASSERT_TRUE(tool_result_store_tree_acquire(s, &doc, &t1));
   TEST_ASSERT_NOT_NULL(t1.slot); /* the large slot */
   struct json_object *first = t1.tree;
   tool_result_store_tree_release(&t1);
   tool_result_store_close(&doc);

   /* The next read opens the row only and reads the cached tree. */
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_OK, tool_result_store_open(at(s, a), alice, id, &doc));
   tool_result_tree_t t2;
   TEST_ASSERT_TRUE(tool_result_store_tree_acquire(s, &doc, &t2));
   TEST_ASSERT_TRUE(t2.tree == first);
   TEST_ASSERT_NULL(doc.body);
   tool_result_store_tree_release(&t2);
   tool_result_store_close(&doc);
   tool_result_store_drop_trees(s);
   free(json);
   free_session(s);
}

/* A thread that isn't the running turn's (a cancelled turn's tool still
 * running) stores and reads nothing; a NUL is stored as a space. */
static void test_a_stale_thread_and_a_nul(void) {
   session_t *s = new_session(1);
   const int64_t a = conversation(alice);
   play_turn(s, 9);
   char id[TOOL_RESULTS_ID_LEN];
   put(s, alice, a, id);
   s_token = 5; /* another thread's token: not the running turn's */
   char none[TOOL_RESULTS_ID_LEN];
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_STORE_FAILED,
                         tool_result_store_put(as_user(at(s, a), alice), "t", NULL, "x", 1, false,
                                               none, NULL));
   tool_result_doc_t doc;
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_REFUSED, tool_result_store_open(s, alice, id, &doc));
   s_token = 9;
   const char text[] = "a\0b";
   char nul[TOOL_RESULTS_ID_LEN];
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_STORE_OK,
                         tool_result_store_put(as_user(at(s, a), alice), "t", NULL, text, 3, false,
                                               nul, NULL));
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_OK, tool_result_store_open(s, alice, nul, &doc));
   TEST_ASSERT_EQUAL_INT(TOOL_RESULT_OPEN_OK, tool_result_store_load_body(&doc));
   TEST_ASSERT_EQUAL_STRING("a b", doc.body);
   tool_result_store_close(&doc);
   play_turn(s, 0);
   free_session(s);
}

int main(void) {
   test_tmp_path(TEST_DB, sizeof(TEST_DB), "dawn_test_tool_result_store.db");
   UNITY_BEGIN();
   RUN_TEST(test_scope_of_a_bound_result);
   RUN_TEST(test_a_stored_frame_comes_back);
   RUN_TEST(test_a_guest_stores_nothing);
   RUN_TEST(test_an_unbound_result);
   RUN_TEST(test_bind_is_the_users_unbound_only);
   RUN_TEST(test_they_go_with_their_conversation_and_user);
   RUN_TEST(test_unbound_results_are_reclaimed);
   RUN_TEST(test_a_huge_result_keeps_head_and_tail);
   RUN_TEST(test_the_tree_cache);
   RUN_TEST(test_queries_use_the_indexes);
   RUN_TEST(test_the_tag_is_masked_at_rest);
   RUN_TEST(test_only_the_users_conversation);
   RUN_TEST(test_a_turns_results_are_its_own);
   RUN_TEST(test_a_turn_ends_without_a_conversation);
   RUN_TEST(test_a_pinned_tree);
   RUN_TEST(test_usage_stays_exact);
   RUN_TEST(test_the_body_waits_and_the_large_slot);
   RUN_TEST(test_a_stale_thread_and_a_nul);
   return UNITY_END();
}
