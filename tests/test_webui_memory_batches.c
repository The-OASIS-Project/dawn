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
 * Unit tests for the memory batches frames on a real schema: the batch
 * object (ops, counts, actions, the counterpart's status, a merge's sources),
 * keyset pages with has_more and next_before_batch_id, one "Not found" for a
 * conversation, batch or op that is missing or another user's, and the undo
 * and keep replies (a stale undo reported as such).
 */

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/outside_kind.h"
#include "dawn_error.h"
#include "memory/memory_consolidate.h"
#include "memory/memory_consolidation_undo.h"
#include "memory/memory_db.h"
#include "memory/memory_db_consolidation.h"
#include "memory/memory_db_provenance.h"
#include "memory/memory_forgotten.h"
#include "unity.h"
#include "webui/webui_internal.h"
#include "webui/webui_memory_batches.h"

/* ---- stubs: the connection's replies ---- */

static json_object *s_sent; /* the last frame sent */

void send_json_response(ws_connection_t *conn, json_object *response) {
   (void)conn;
   json_object_put(s_sent);
   s_sent = json_object_get(response);
}

bool conn_require_auth(ws_connection_t *conn) {
   return conn->auth_user_id > 0;
}

/* The batch frame a notify sends its user's tabs (memory_batch's payload), as
 * webui_broadcasts.c builds it. */
static json_object *s_notified;

void memory_consolidation_batch_notify(int user_id, int64_t conversation_id, int64_t batch_id) {
   (void)conversation_id;
   json_object_put(s_notified);
   s_notified = webui_memory_batch_notice(user_id, batch_id);
}

/* ---- fixtures ---- */

static int s_user;
static int s_other_user;
static int64_t s_conv;
static memory_forgotten_list_t s_forgotten;
static const char *s_reply;

static char *scripted(struct json_object *history,
                      const llm_resolved_config_t *cfg,
                      int *prompt_tokens,
                      int *output_tokens) {
   (void)history;
   (void)cfg;
   *prompt_tokens = 1000;
   *output_tokens = 0;
   const char *r = s_reply;
   s_reply = NULL;
   return r ? strdup(r) : NULL;
}

static int user_named(const char *name) {
   auth_db_create_user(name, "hash", false);
   auth_user_t user;
   memset(&user, 0, sizeof(user));
   auth_db_get_user(name, &user);
   return user.id;
}

void setUp(void) {
   config_set_defaults(&g_config);
   g_config.memory.enabled = true;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   s_user = user_named("jonathan");
   s_other_user = user_named("mallory");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "Chat", &s_conv));
   memset(&s_forgotten, 0, sizeof(s_forgotten));
   s_forgotten.user_id = s_user;
   s_sent = NULL;
   memory_consolidate_set_model_fn(scripted);
}

void tearDown(void) {
   json_object_put(s_sent);
   s_sent = NULL;
   json_object_put(s_notified);
   s_notified = NULL;
   memory_forgotten_free(&s_forgotten);
   memory_consolidate_set_model_fn(NULL);
   auth_db_shutdown();
}

static int64_t run(const char *round0, const char *plan) {
   struct json_object *history = json_object_new_array();
   const llm_resolved_config_t cfg = { .type = LLM_CLOUD,
                                       .cloud_provider = CLOUD_PROVIDER_CLAUDE,
                                       .model = "claude-haiku-5-5" };
   const memory_provenance_t prov = { .conv_id = s_conv, .msg_id_start = 3, .msg_id_end = 9 };
   const memory_consolidate_input_t in = { .user_id = s_user,
                                           .conversation_id = s_conv,
                                           .session_id = "test",
                                           .message_count = 4,
                                           .prov = &prov,
                                           .first_msg_id = 3,
                                           .forgotten = &s_forgotten,
                                           .cfg = &cfg,
                                           .history = history,
                                           .reply = round0 };
   memory_consolidate_result_t res;
   memset(&res, 0, sizeof(res));
   s_reply = plan;
   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_consolidate_batch(&in, &res));
   json_object_put(history);
   return res.batch_id;
}

static int64_t stored(const char *text) {
   memory_provenance_t prov = { .conv_id = s_conv, .msg_id_start = 1, .msg_id_end = 1 };
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_fact_create(s_user, text, 0.7f, "explicit", &prov, &id));
   return id;
}

static json_object *get(json_object *o, const char *key) {
   json_object *v = NULL;
   json_object_object_get_ex(o, key, &v);
   return v;
}

static const char *str(json_object *o, const char *key) {
   json_object *v = get(o, key);
   return v ? json_object_get_string(v) : NULL;
}

static json_object *op_named(json_object *batch, const char *op) {
   json_object *ops = get(batch, "ops");
   for (size_t i = 0; ops && i < json_object_array_length(ops); i++) {
      json_object *o = json_object_array_get_idx(ops, i);
      if (strcmp(str(o, "op"), op) == 0) {
         return o;
      }
   }
   return NULL;
}

static json_object *request(const char *json) {
   return json_tokener_parse(json);
}

#define ONE_FACT(text) "{\"facts\": [{\"text\": \"" text "\", \"subject\": \"Jonathan Smith\"}]}"

/* ---- the batch object ---- */

void test_the_batch_object_carries_ops_counts_and_actions(void) {
   stored("Jonathan Smith works at Acme");
   const int64_t batch = run(ONE_FACT("Jonathan Smith works at Acme as a senior engineer"),
                             "{\"ops\": [{\"op\": \"merge\", \"from\": \"S1\", \"into\": "
                             "\"C1\", \"reason\": \"<b>same job</b>\"}], \"finish\": true}");
   json_object *b = webui_memory_batch_json(s_user, batch);
   TEST_ASSERT_NOT_NULL(b);
   TEST_ASSERT_EQUAL_INT64(s_conv, json_object_get_int64(get(b, "conversation_id")));
   TEST_ASSERT_EQUAL_INT64(3, json_object_get_int64(get(b, "from_message_id")));
   TEST_ASSERT_EQUAL_INT64(9, json_object_get_int64(get(b, "after_message_id")));
   TEST_ASSERT_EQUAL_STRING("applied", str(b, "status"));
   TEST_ASSERT_EQUAL_INT(1, json_object_get_int(get(get(b, "counts"), "merged")));
   TEST_ASSERT_EQUAL_INT(1, json_object_get_int(get(get(b, "counts"), "added")));
   json_object *merged = op_named(b, "merged");
   TEST_ASSERT_EQUAL_STRING("undo", str(merged, "action"));
   TEST_ASSERT_EQUAL_STRING("current", str(merged, "target_status"));
   TEST_ASSERT_EQUAL_STRING("<b>same job</b>", str(merged, "reason")); /* as stored */
   /* The fact kept lists what was merged into it. */
   json_object *sources = get(op_named(b, "added"), "sources");
   TEST_ASSERT_NOT_NULL(sources);
   TEST_ASSERT_EQUAL_STRING("Jonathan Smith works at Acme",
                            str(json_object_array_get_idx(sources, 0), "text"));
   json_object_put(b);
   TEST_ASSERT_NULL(webui_memory_batch_json(s_other_user, batch)); /* not theirs */
}

void test_a_dropped_candidate_offers_keep(void) {
   const int64_t batch = run("{\"facts\": [{\"text\": \"Jonathan Smith plays the cello\", "
                             "\"subject\": \"Jonathan Smith\"}, {\"text\": \"Jonathan Smith "
                             "visits Lisbon\", \"subject\": \"Jonathan Smith\"}]}",
                             "{\"ops\": [{\"op\": \"add\", \"c\": \"C1\"}, {\"op\": \"drop\", "
                             "\"c\": \"C2\"}], \"finish\": true}");
   json_object *b = webui_memory_batch_json(s_user, batch);
   TEST_ASSERT_EQUAL_STRING("keep", str(op_named(b, "dropped"), "action"));
   json_object_put(b);
}

/* A batch that only set a preference counts it (the tab and marker say so). */
void test_a_preference_only_batch_counts_it(void) {
   const int64_t batch = run("{\"facts\": [], \"preferences\": [{\"category\": \"coffee\", "
                             "\"value\": \"oat latte\", \"confidence\": 0.8}]}",
                             NULL);
   json_object *b = webui_memory_batch_json(s_user, batch);
   TEST_ASSERT_NOT_NULL(b);
   TEST_ASSERT_EQUAL_INT(1, json_object_get_int(get(get(b, "counts"), "pref_set")));
   TEST_ASSERT_EQUAL_STRING("undo", str(op_named(b, "pref_set"), "action"));
   json_object_put(b);
}

/* What outside text a batch read is named once on the batch; a row carries its
 * own (a candidate from another batch differs). */
void test_the_batch_says_what_outside_text_it_read(void) {
   const memory_consolidation_batch_t b = { .user_id = s_user,
                                            .conversation_id = s_conv,
                                            .path = "agent",
                                            .stop = "finished",
                                            .outside = OUTSIDE_EMAIL };
   int64_t batch = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_consolidation_batch_add(&b, &batch));
   memory_consolidation_op_t op = { .batch_id = batch,
                                    .user_id = s_user,
                                    .seq = 1,
                                    .op = "dropped",
                                    .outside = OUTSIDE_EMAIL,
                                    .text = "A memo" };
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_consolidation_op_add(&op, NULL));
   op.seq = 2;
   op.op = "deferred";
   op.outside = OUTSIDE_WEB;
   op.text = "A page";
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_consolidation_op_add(&op, NULL));
   json_object *j = webui_memory_batch_json(s_user, batch);
   TEST_ASSERT_NOT_NULL(j);
   TEST_ASSERT_TRUE(json_object_get_boolean(get(j, "outside")));
   TEST_ASSERT_EQUAL_STRING("email", str(j, "outside_from"));
   TEST_ASSERT_EQUAL_STRING("email", str(op_named(j, "dropped"), "outside_from"));
   TEST_ASSERT_EQUAL_STRING("web", str(op_named(j, "deferred"), "outside_from"));
   json_object_put(j);

   const int64_t clean = run(ONE_FACT("Jonathan Smith plays the cello"), NULL);
   j = webui_memory_batch_json(s_user, clean);
   TEST_ASSERT_FALSE(json_object_get_boolean(get(j, "outside")));
   TEST_ASSERT_NULL(str(j, "outside_from"));
   json_object_put(j);
}

/* ---- list_memory_batches ---- */

static void add_batches(int n, int64_t *ids) {
   for (int i = 0; i < n; i++) {
      const memory_consolidation_batch_t b = { .user_id = s_user,
                                               .conversation_id = s_conv,
                                               .path = "agent",
                                               .stop = "finished" };
      memory_db_consolidation_batch_add(&b, &ids[i]);
   }
}

void test_list_pages_with_has_more_and_the_next_cursor(void) {
   int64_t ids[3];
   add_batches(3, ids);
   json_object *req = request("{\"limit\": 2}");
   json_object *page = webui_memory_batches_list_payload(s_user, req);
   TEST_ASSERT_EQUAL_INT(2, (int)json_object_array_length(get(page, "batches")));
   TEST_ASSERT_TRUE(json_object_get_boolean(get(page, "has_more")));
   const int64_t next = json_object_get_int64(get(page, "next_before_batch_id"));
   TEST_ASSERT_EQUAL_INT64(ids[1], next);
   TEST_ASSERT_NULL(get(page, "error"));
   json_object_put(req);
   json_object_put(page);

   char json[96];
   snprintf(json, sizeof(json), "{\"before_batch_id\": %lld, \"limit\": 2}", (long long)next);
   req = request(json);
   page = webui_memory_batches_list_payload(s_user, req);
   TEST_ASSERT_EQUAL_INT(1, (int)json_object_array_length(get(page, "batches")));
   TEST_ASSERT_FALSE(json_object_get_boolean(get(page, "has_more")));
   TEST_ASSERT_FALSE(json_object_object_get_ex(page, "next_before_batch_id", NULL));
   TEST_ASSERT_EQUAL_INT64(next, json_object_get_int64(get(page, "before_batch_id")));
   json_object_put(req);
   json_object_put(page);
}

void test_list_clamps_the_limit(void) {
   json_object *req = request("{\"limit\": 500}");
   json_object *page = webui_memory_batches_list_payload(s_user, req);
   TEST_ASSERT_EQUAL_INT(WEBUI_MEMORY_BATCHES_LIMIT_MAX, json_object_get_int(get(page, "limit")));
   json_object_put(req);
   json_object_put(page);
   page = webui_memory_batches_list_payload(s_user, NULL);
   TEST_ASSERT_EQUAL_INT(WEBUI_MEMORY_BATCHES_LIMIT, json_object_get_int(get(page, "limit")));
   json_object_put(page);
}

/* Another user's conversation and one that doesn't exist: the same reply. */
void test_list_answers_not_found_alike_for_missing_and_foreign(void) {
   int64_t ids[1];
   add_batches(1, ids);
   char json[96];
   snprintf(json, sizeof(json), "{\"conversation_id\": %lld}", (long long)s_conv);
   json_object *req = request(json);
   json_object *mine = webui_memory_batches_list_payload(s_user, req);
   TEST_ASSERT_EQUAL_INT(1, (int)json_object_array_length(get(mine, "batches")));
   json_object *theirs = webui_memory_batches_list_payload(s_other_user, req);
   json_object_put(req);
   req = request("{\"conversation_id\": 999999}");
   json_object *missing = webui_memory_batches_list_payload(s_user, req);
   TEST_ASSERT_EQUAL_STRING("Not found", str(theirs, "error"));
   TEST_ASSERT_EQUAL_STRING("Not found", str(missing, "error"));
   TEST_ASSERT_EQUAL_INT(0, (int)json_object_array_length(get(theirs, "batches")));
   TEST_ASSERT_EQUAL_INT(0, (int)json_object_array_length(get(missing, "batches")));
   /* Only the echoed id differs. */
   json_object_object_del(theirs, "conversation_id");
   json_object_object_del(missing, "conversation_id");
   TEST_ASSERT_EQUAL_STRING(json_object_to_json_string(theirs),
                            json_object_to_json_string(missing));
   json_object_put(req);
   json_object_put(mine);
   json_object_put(theirs);
   json_object_put(missing);
}

void test_the_list_handler_replies_for_the_connections_user(void) {
   int64_t ids[2];
   add_batches(2, ids);
   ws_connection_t conn;
   memset(&conn, 0, sizeof(conn));
   conn.auth_user_id = s_other_user;
   handle_list_memory_batches(&conn, NULL);
   TEST_ASSERT_EQUAL_STRING("list_memory_batches_response", str(s_sent, "type"));
   TEST_ASSERT_EQUAL_INT(0, (int)json_object_array_length(get(get(s_sent, "payload"), "batches")));
   conn.auth_user_id = s_user;
   handle_list_memory_batches(&conn, NULL);
   TEST_ASSERT_EQUAL_INT(2, (int)json_object_array_length(get(get(s_sent, "payload"), "batches")));
}

/* ---- undo and keep replies ---- */

void test_an_undo_without_ids_is_not_found_at_once(void) {
   ws_connection_t conn;
   memset(&conn, 0, sizeof(conn));
   conn.auth_user_id = s_user;
   json_object *req = request("{\"op_id\": 4}");
   handle_keep_memory_op(&conn, req);
   json_object *p = get(s_sent, "payload");
   TEST_ASSERT_EQUAL_STRING("keep_memory_op_response", str(s_sent, "type"));
   TEST_ASSERT_EQUAL_STRING("error", str(p, "status"));
   TEST_ASSERT_EQUAL_STRING("Not found", str(p, "error"));
   json_object_put(req);
}

/* An op_id that isn't a number names no op: never a whole-batch undo. */
void test_an_op_id_that_is_not_a_number_is_not_found(void) {
   const int64_t batch = run(ONE_FACT("Jonathan Smith plays the cello"), NULL);
   ws_connection_t conn;
   memset(&conn, 0, sizeof(conn));
   conn.auth_user_id = s_user;
   char json[96];
   snprintf(json, sizeof(json), "{\"batch_id\": %lld, \"op_id\": \"all\"}", (long long)batch);
   json_object *req = request(json);
   handle_undo_memory_batch(&conn, req);
   json_object *p = get(s_sent, "payload");
   TEST_ASSERT_EQUAL_STRING("undo_memory_batch_response", str(s_sent, "type"));
   TEST_ASSERT_EQUAL_STRING("Not found", str(p, "error"));
   json_object_put(req);
   json_object *b = webui_memory_batch_json(s_user, batch);
   TEST_ASSERT_EQUAL_STRING("applied", str(b, "status")); /* nothing undone */
   json_object_put(b);
}

/* The model's reason and DAWN's say whose they are. */
void test_each_reason_says_who_wrote_it(void) {
   stored("Jonathan Smith lives in Portland");
   const int64_t batch = run(ONE_FACT("Jonathan Smith lives in Springfield"),
                             "{\"ops\": [{\"op\": \"replace\", \"old\": \"S1\", \"by\": "
                             "\"C1\", \"reason\": \"he moved\"}], \"finish\": true}");
   json_object *b = webui_memory_batch_json(s_user, batch);
   TEST_ASSERT_EQUAL_STRING("model", str(op_named(b, "replaced"), "reason_by"));
   json_object_put(b);
   const int64_t triaged = run(ONE_FACT("Jonathan Smith plays the cello"), NULL);
   b = webui_memory_batch_json(s_user, triaged);
   TEST_ASSERT_EQUAL_STRING("dawn", str(op_named(b, "added"), "reason_by"));
   json_object_put(b);
}

void test_a_stale_undo_is_reported_with_its_op(void) {
   const int64_t batch = run(ONE_FACT("Jonathan Smith plays the cello"), NULL);
   json_object *b = webui_memory_batch_json(s_user, batch);
   const int64_t op = json_object_get_int64(get(op_named(b, "added"), "op_id"));
   const int64_t fact = json_object_get_int64(get(op_named(b, "added"), "fact_id"));
   json_object_put(b);
   int64_t conv2 = 0;
   conv_db_create(s_user, "Later", &conv2);
   memory_db_fact_provenance_extend(fact, s_user, conv2, 7, 9);
   memory_consolidation_outcome_t io;
   memset(&io, 0, sizeof(io));
   io.action = MEMORY_CONSOLIDATION_UNDO_OP;
   io.user_id = s_user;
   io.batch_id = batch;
   io.op_id = op;
   memory_consolidation_run(&io);
   json_object *p = webui_memory_batch_outcome_payload(&io);
   TEST_ASSERT_EQUAL_STRING("stale", str(p, "status"));
   TEST_ASSERT_EQUAL_INT64(op,
                           json_object_get_int64(json_object_array_get_idx(get(p, "stale"), 0)));
   TEST_ASSERT_EQUAL_INT(0, (int)json_object_array_length(get(p, "undone")));
   TEST_ASSERT_EQUAL_STRING("This memory changed since; nothing was undone.", str(p, "error"));
   json_object_put(p);
   b = webui_memory_batch_json(s_user, batch);
   TEST_ASSERT_EQUAL_STRING("stale", str(op_named(b, "added"), "status"));
   TEST_ASSERT_NULL(get(op_named(b, "added"), "action"));
   json_object_put(b);
}

void test_an_undo_of_another_users_batch_is_not_found(void) {
   const int64_t batch = run(ONE_FACT("Jonathan Smith plays the cello"), NULL);
   memory_consolidation_outcome_t io;
   memset(&io, 0, sizeof(io));
   io.action = MEMORY_CONSOLIDATION_UNDO_BATCH;
   io.user_id = s_other_user;
   io.batch_id = batch;
   memory_consolidation_run(&io);
   json_object *p = webui_memory_batch_outcome_payload(&io);
   TEST_ASSERT_EQUAL_STRING("error", str(p, "status"));
   TEST_ASSERT_EQUAL_STRING("Not found", str(p, "error"));
   TEST_ASSERT_EQUAL_INT64(batch, json_object_get_int64(get(p, "batch_id")));
   json_object_put(p);
   json_object *b = webui_memory_batch_json(s_user, batch);
   TEST_ASSERT_EQUAL_STRING("applied", str(b, "status"));
   json_object_put(b);
}

/* A forget sends the tabs the batch it changed, without the forgotten fact. */
void test_a_forget_sends_the_batch_it_changed(void) {
   const int64_t batch = run("{\"facts\": [{\"text\": \"Jonathan Smith plays the cello\", "
                             "\"subject\": \"Jonathan Smith\"}, {\"text\": \"Jonathan Smith "
                             "visits Lisbon\", \"subject\": \"Jonathan Smith\"}]}",
                             NULL);
   json_object *b = webui_memory_batch_json(s_user, batch);
   const int64_t fact = json_object_get_int64(get(op_named(b, "added"), "fact_id"));
   json_object_put(b);
   json_object_put(s_notified);
   s_notified = NULL;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_forget(fact, s_user, NULL));
   TEST_ASSERT_NOT_NULL(s_notified);
   TEST_ASSERT_EQUAL_INT64(batch, json_object_get_int64(get(s_notified, "batch_id")));
   json_object *ops = get(s_notified, "ops");
   TEST_ASSERT_EQUAL_INT(1, (int)json_object_array_length(ops));
   TEST_ASSERT_NOT_EQUAL(fact,
                         json_object_get_int64(get(json_object_array_get_idx(ops, 0), "fact_id")));
}

/* A conversation forget that removes a batch tells its tabs by id, marked
 * removed, so a shown batch and its chip go. */
void test_a_conversation_forget_sends_the_batch_it_removed(void) {
   const int64_t batch = run("{\"facts\": [{\"text\": \"Jonathan Smith plays the cello\", "
                             "\"subject\": \"Jonathan Smith\"}]}",
                             NULL);
   json_object_put(s_notified);
   s_notified = NULL;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_conversations_forget(s_user, &s_conv, 1, NULL));
   TEST_ASSERT_NOT_NULL(s_notified);
   TEST_ASSERT_EQUAL_INT64(batch, json_object_get_int64(get(s_notified, "batch_id")));
   TEST_ASSERT_TRUE(json_object_get_boolean(get(s_notified, "removed")));
   TEST_ASSERT_NULL(get(s_notified, "ops"));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_the_batch_object_carries_ops_counts_and_actions);
   RUN_TEST(test_a_preference_only_batch_counts_it);
   RUN_TEST(test_a_dropped_candidate_offers_keep);
   RUN_TEST(test_the_batch_says_what_outside_text_it_read);
   RUN_TEST(test_list_pages_with_has_more_and_the_next_cursor);
   RUN_TEST(test_list_clamps_the_limit);
   RUN_TEST(test_list_answers_not_found_alike_for_missing_and_foreign);
   RUN_TEST(test_the_list_handler_replies_for_the_connections_user);
   RUN_TEST(test_an_undo_without_ids_is_not_found_at_once);
   RUN_TEST(test_an_op_id_that_is_not_a_number_is_not_found);
   RUN_TEST(test_each_reason_says_who_wrote_it);
   RUN_TEST(test_a_stale_undo_is_reported_with_its_op);
   RUN_TEST(test_an_undo_of_another_users_batch_is_not_found);
   RUN_TEST(test_a_forget_sends_the_batch_it_changed);
   RUN_TEST(test_a_conversation_forget_sends_the_batch_it_removed);
   return UNITY_END();
}
