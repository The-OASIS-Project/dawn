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
 * Unit tests for the consolidation record's readers on a real schema: undo of
 * each op kind, and the stale check that keeps an undo from removing what a
 * later change made; owner checks (a user only ever acts on their own
 * batches); Keep (a candidate set aside, stored with the outside text it was
 * recorded with, never as the user's own word, and never once forgotten);
 * listing, the counterpart's status, the record's caps, expiry and prune.
 * Batches come from the consolidator with the model's replies scripted, or
 * are recorded directly where that is simpler.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <json-c/json.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "core/outside_kind.h"
#include "core/strbuf.h"
#include "dawn_error.h"
#include "memory/memory_consolidate.h"
#include "memory/memory_consolidation_undo.h"
#include "memory/memory_db.h"
#include "memory/memory_db_aliases.h"
#include "memory/memory_db_consolidation.h"
#include "memory/memory_db_entities.h"
#include "memory/memory_db_provenance.h"
#include "memory/memory_forgotten.h"
#include "unity.h"
#include "utils/string_utils.h"

static int s_user;
static int s_other_user;
static int64_t s_conv;
static memory_forgotten_list_t s_forgotten;

/* The scripted model: reply i to call i (NULL: the call fails). */
#define SCRIPT_MAX 4
static const char *s_replies[SCRIPT_MAX];
static int s_calls;

static char *scripted(struct json_object *history,
                      const llm_resolved_config_t *cfg,
                      int *prompt_tokens,
                      int *output_tokens) {
   (void)history;
   (void)cfg;
   const int call = s_calls++;
   *prompt_tokens = 1000;
   *output_tokens = 0;
   const char *reply = call < SCRIPT_MAX ? s_replies[call] : NULL;
   return reply ? strdup(reply) : NULL;
}

static void script(const char *plan) {
   memset(s_replies, 0, sizeof(s_replies));
   s_replies[0] = plan;
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
   s_calls = 0;
   memory_consolidate_set_model_fn(scripted);
}

void tearDown(void) {
   memory_forgotten_free(&s_forgotten);
   memory_consolidate_set_model_fn(NULL);
   auth_db_shutdown();
}

static int64_t q(const char *sql, int64_t a, const char *text) {
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL));
   sqlite3_bind_int64(st, 1, a);
   if (text) {
      sqlite3_bind_text(st, 2, text, -1, SQLITE_STATIC);
   }
   const int64_t v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
   sqlite3_finalize(st);
   return v;
}

static void exec(const char *sql) {
   char *err = NULL;
   TEST_ASSERT_EQUAL_INT_MESSAGE(SQLITE_OK, sqlite3_exec(s_db.db, sql, NULL, NULL, &err), err);
}

static int64_t stored(const char *text, const char *source) {
   memory_provenance_t prov = { .conv_id = s_conv, .msg_id_start = 1, .msg_id_end = 1 };
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_fact_create(s_user, text, 0.7f, source, &prov, &id));
   return id;
}

/* A batch of this test's conversation, read from @p round0 (tagged @p outside). */
static int64_t run(const char *round0, int outside) {
   struct json_object *history = json_object_new_array();
   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "role", json_object_new_string("user"));
   json_object_object_add(msg, "content", json_object_new_string("Extract what to remember."));
   json_object_array_add(history, msg);
   const llm_resolved_config_t cfg = { .type = LLM_CLOUD,
                                       .cloud_provider = CLOUD_PROVIDER_CLAUDE,
                                       .model = "claude-haiku-5-5" };
   const memory_provenance_t prov = { .conv_id = s_conv,
                                      .msg_id_start = 1,
                                      .msg_id_end = 5,
                                      .outside = outside };
   const memory_consolidate_input_t in = { .user_id = s_user,
                                           .conversation_id = s_conv,
                                           .session_id = "test",
                                           .message_count = 4,
                                           .prov = &prov,
                                           .first_msg_id = 1,
                                           .forgotten = &s_forgotten,
                                           .cfg = &cfg,
                                           .history = history,
                                           .reply = round0 };
   memory_consolidate_result_t res;
   memset(&res, 0, sizeof(res));
   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_consolidate_batch(&in, &res));
   json_object_put(history);
   TEST_ASSERT_TRUE(res.batch_id > 0);
   return res.batch_id;
}

static int64_t current(const char *text) {
   const int64_t id = q("SELECT id FROM memory_facts WHERE user_id = ?1 AND fact_text = ?2 AND "
                        "superseded_by IS NULL",
                        s_user, text);
   return id < 0 ? 0 : id;
}

static int64_t superseded_by(int64_t fact_id) {
   return q("SELECT COALESCE(superseded_by, 0) FROM memory_facts WHERE id = ?1", fact_id, NULL);
}

/* The first op @p op of @p batch (0: none). */
static int64_t op_of(int64_t batch, const char *op) {
   const int64_t id = q("SELECT id FROM memory_consolidation_ops WHERE batch_id = ?1 AND op = ?2 "
                        "AND status != 'refused' ORDER BY seq LIMIT 1",
                        batch, op);
   return id < 0 ? 0 : id;
}

static char s_status[16];
static const char *status_of(int64_t op_id) {
   sqlite3_stmt *st = NULL;
   sqlite3_prepare_v2(s_db.db, "SELECT status FROM memory_consolidation_ops WHERE id = ?1", -1, &st,
                      NULL);
   sqlite3_bind_int64(st, 1, op_id);
   s_status[0] = '\0';
   if (sqlite3_step(st) == SQLITE_ROW) {
      safe_strscpy(s_status, (const char *)sqlite3_column_text(st, 0));
   }
   sqlite3_finalize(st);
   return s_status;
}

static int undo(int64_t batch, int64_t op) {
   return memory_db_consolidation_undo_op(s_user, batch, op, NULL);
}

static void outcome(memory_consolidation_outcome_t *io,
                    memory_consolidation_action_t action,
                    int user_id,
                    int64_t batch,
                    int64_t op) {
   memset(io, 0, sizeof(*io));
   io->action = action;
   io->user_id = user_id;
   io->batch_id = batch;
   io->op_id = op;
   memory_consolidation_run(io);
}

#define ONE_FACT(text) "{\"facts\": [{\"text\": \"" text "\", \"subject\": \"Jonathan Smith\"}]}"
#define TWO_FACTS(a, b)                                                    \
   "{\"facts\": [{\"text\": \"" a "\", \"subject\": \"Jonathan Smith\"}, " \
   "{\"text\": \"" b "\", \"subject\": \"Jonathan Smith\"}]}"
#define CELLO "Jonathan Smith plays the cello"

/* ---- undo, each op kind ---- */

void test_undo_added_deletes_the_fact_and_is_idempotent(void) {
   script(NULL);
   const int64_t batch = run(ONE_FACT(CELLO), 0);
   const int64_t added = op_of(batch, "added");
   TEST_ASSERT_TRUE(current(CELLO) > 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, undo(batch, added));
   TEST_ASSERT_EQUAL_INT(0, current(CELLO));
   TEST_ASSERT_EQUAL_STRING("undone", status_of(added));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, undo(batch, added)); /* again: nothing more */
   TEST_ASSERT_TRUE(q("SELECT undone_at IS NOT NULL FROM memory_consolidation_batches WHERE id = "
                      "?1",
                      batch, NULL) == 1);
}

/* A restatement's pre-image is taken before the store: the expiry, subject
 * and relations the store set on the stored fact are what the undo takes back. */
void test_undo_reinforced_puts_back_what_the_restatement_set(void) {
   g_config.memory.expire_enabled = true;
   const int64_t fact = stored(CELLO, "inferred");
   script(NULL);
   const int64_t batch = run("{\"facts\": [{\"text\": \"" CELLO "\", \"subject\": \"Jonathan "
                             "Smith\", \"source\": \"explicit\", \"expires_at\": \"2099-01-01\", "
                             "\"relations\": [{\"subject\": \"Jonathan Smith\", \"predicate\": "
                             "\"plays\", \"object\": \"cello\"}]}]}",
                             0);
   const int64_t op = op_of(batch, "reinforced");
   TEST_ASSERT_TRUE(op > 0);
   TEST_ASSERT_EQUAL_INT(1, q("SELECT expires_at IS NOT NULL AND subject_entity_id IS NOT NULL AND "
                              "source = 'explicit' FROM memory_facts WHERE id = ?1",
                              fact, NULL));
   TEST_ASSERT_EQUAL_INT(1,
                         q("SELECT COUNT(*) FROM memory_relations WHERE fact_id = ?1", fact, NULL));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, undo(batch, op));
   TEST_ASSERT_EQUAL_INT(1, q("SELECT expires_at IS NULL AND subject_entity_id IS NULL AND "
                              "source = 'inferred' AND ABS(confidence - 0.7) < 0.001 FROM "
                              "memory_facts WHERE id = ?1",
                              fact, NULL));
   TEST_ASSERT_EQUAL_INT(0,
                         q("SELECT COUNT(*) FROM memory_relations WHERE fact_id = ?1", fact, NULL));
   TEST_ASSERT_EQUAL_INT64(fact, current(CELLO)); /* the fact itself stays */
}

void test_undo_merged_restores_the_duplicate(void) {
   const int64_t short_one = stored("Jonathan Smith works at Acme", "explicit");
   script("{\"ops\": [{\"op\": \"merge\", \"from\": \"S1\", \"into\": \"C1\"}], \"finish\": true}");
   const int64_t batch = run(ONE_FACT("Jonathan Smith works at Acme as a senior engineer"), 0);
   const int64_t merged = op_of(batch, "merged");
   TEST_ASSERT_TRUE(superseded_by(short_one) > 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, undo(batch, merged));
   TEST_ASSERT_EQUAL_INT(0, superseded_by(short_one));
}

void test_undo_replaced_restores_the_old_value(void) {
   const int64_t old = stored("Jonathan Smith lives in Portland", "explicit");
   script("{\"ops\": [{\"op\": \"replace\", \"old\": \"S1\", \"by\": \"C1\"}], \"finish\": true}");
   const int64_t batch = run(ONE_FACT("Jonathan Smith lives in Springfield"), 0);
   TEST_ASSERT_TRUE(superseded_by(old) > 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, undo(batch, op_of(batch, "replaced")));
   TEST_ASSERT_EQUAL_INT(0, superseded_by(old));
}

#define GREEN_REPLY                                                                        \
   "{\"facts\": [{\"text\": \"Jonathan Smith's favorite color is green\", \"subject\": "   \
   "\"Jonathan Smith\", \"relations\": [{\"subject\": \"Jonathan Smith\", \"predicate\": " \
   "\"favorite_color\", \"object\": \"green\"}]}]}"
#define CLOSE_PLAN                                                                             \
   "{\"ops\": [{\"op\": \"add\", \"c\": \"C1\"}, {\"op\": \"close_relation\", \"r\": \"R1\", " \
   "\"valid_to\": \"2026-10-01\", \"by\": \"C1\"}], \"finish\": true}"

static int64_t blue_relation(void) {
   bool created = false;
   int64_t jon = 0;
   memory_db_entity_upsert(s_user, "Jonathan Smith", "person", "jonathan smith", &created, &jon);
   const int64_t old = stored("Jonathan Smith's favorite color is blue", "explicit");
   memory_db_relation_create(s_user, jon, "favorite_color", 0, "blue", old, 0.9f, 0, 0, NULL);
   return q("SELECT id FROM memory_relations WHERE object_value = 'blue'", 0, NULL);
}

/* The whole batch undone, the last op first: the close is reopened while the
 * value that closed it is still open (as before the close), then that goes. */
void test_undo_whole_batch_reopens_a_closed_relation(void) {
   const int64_t blue = blue_relation();
   script(CLOSE_PLAN);
   const int64_t batch = run(GREEN_REPLY, 0);
   TEST_ASSERT_EQUAL_INT(1, q("SELECT valid_to IS NOT NULL FROM memory_relations WHERE id = ?1",
                              blue, NULL));
   memory_consolidation_outcome_t io;
   outcome(&io, MEMORY_CONSOLIDATION_UNDO_BATCH, s_user, batch, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATION_OK, io.status);
   TEST_ASSERT_EQUAL_INT(2, io.n_undone);
   TEST_ASSERT_EQUAL_INT(1, q("SELECT valid_to IS NULL AND closed_by_fact_id IS NULL FROM "
                              "memory_relations WHERE id = ?1",
                              blue, NULL));
   TEST_ASSERT_EQUAL_INT(0, current("Jonathan Smith's favorite color is green"));
   TEST_ASSERT_EQUAL_INT(0, q("SELECT COUNT(*) FROM memory_relations WHERE object_value = "
                              "'green'",
                              0, NULL));
}

/* A link, recorded as the consolidator records one. */
static int64_t linked_batch(int64_t *link_out, int64_t *source_out, int64_t *target_out) {
   bool created = false;
   memory_db_entity_upsert(s_user, "Jon Smith", "person", "jon smith", &created, source_out);
   memory_db_entity_upsert(s_user, "Jonathan Smith", "person", "jonathan smith", &created,
                           target_out);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_alias_link(s_user, *source_out, *target_out, "soft",
                                                     "test", 0.9f, NULL, link_out));
   const memory_consolidation_batch_t b = { .user_id = s_user,
                                            .conversation_id = s_conv,
                                            .path = "agent",
                                            .stop = "finished" };
   int64_t batch = 0;
   memory_db_consolidation_batch_add(&b, &batch);
   char post[64];
   snprintf(post, sizeof(post), "{\"link_id\": %lld}", (long long)*link_out);
   const memory_consolidation_op_t op = { .batch_id = batch,
                                          .user_id = s_user,
                                          .seq = 1,
                                          .op = "linked",
                                          .entity_id = *source_out,
                                          .other_entity_id = *target_out,
                                          .link_id = *link_out,
                                          .text = "Jon Smith",
                                          .target_text = "Jonathan Smith",
                                          .post_image = post };
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_consolidation_op_add(&op, NULL));
   return batch;
}

void test_undo_linked_splits_the_entities(void) {
   int64_t link = 0;
   int64_t source = 0;
   int64_t target = 0;
   const int64_t batch = linked_batch(&link, &source, &target);
   TEST_ASSERT_EQUAL_INT64(target, q("SELECT canonical_id FROM memory_entities WHERE id = ?1",
                                     source, NULL));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, undo(batch, op_of(batch, "linked")));
   TEST_ASSERT_EQUAL_INT(1, q("SELECT canonical_id IS NULL FROM memory_entities WHERE id = ?1",
                              source, NULL));
   TEST_ASSERT_EQUAL_INT(1, q("SELECT unlinked_at IS NOT NULL FROM memory_entity_aliases WHERE "
                              "id = ?1",
                              link, NULL));
}

#define PREF_REPLY(value)                                                                    \
   "{\"facts\": [], \"preferences\": [{\"category\": \"coffee\", \"value\": \"" value "\", " \
   "\"confidence\": 0.8}]}"

void test_undo_pref_set_removes_a_new_preference(void) {
   script(NULL);
   const int64_t batch = run(PREF_REPLY("oat latte"), 0);
   memory_preference_t p;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_pref_get(s_user, "coffee", &p));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, undo(batch, op_of(batch, "pref_set")));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_NOT_FOUND, memory_db_pref_get(s_user, "coffee", &p));
}

void test_undo_pref_set_puts_the_old_value_back(void) {
   memory_db_pref_upsert(s_user, "coffee", "espresso", 0.6f, "inferred", NULL);
   script(NULL);
   const int64_t batch = run(PREF_REPLY("oat latte"), 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, undo(batch, op_of(batch, "pref_set")));
   memory_preference_t p;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_pref_get(s_user, "coffee", &p));
   TEST_ASSERT_EQUAL_STRING("espresso", p.value);
}

/* Undone, the value goes back with where it was learned: forgetting the
 * undone batch's conversation keeps it; the USER MEMORY blocks are withdrawn. */
void test_undo_pref_set_puts_the_old_source_back(void) {
   int64_t conv_a = 0;
   conv_db_create(s_user, "A", &conv_a);
   const memory_provenance_t prov_a = { .conv_id = conv_a, .msg_id_start = 2, .msg_id_end = 4 };
   memory_db_pref_upsert(s_user, "coffee", "espresso", 0.6f, "inferred", &prov_a);
   script(NULL);
   const int64_t batch = run(PREF_REPLY("oat latte"), 0);
   memory_consolidation_undo_effect_t eff = { 0 };
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_consolidation_undo_op(
                                                s_user, batch, op_of(batch, "pref_set"), &eff));
   TEST_ASSERT_TRUE(eff.memory_bodies);
   TEST_ASSERT_EQUAL_INT64(conv_a, q("SELECT source_conversation_id FROM memory_preferences WHERE "
                                     "user_id = ?1 AND category = ?2",
                                     s_user, "coffee"));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_conversations_forget(s_user, &s_conv, 1, NULL));
   memory_preference_t p;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_pref_get(s_user, "coffee", &p));
   TEST_ASSERT_EQUAL_STRING("espresso", p.value);
   TEST_ASSERT_EQUAL_INT64(conv_a, q("SELECT source_conversation_id FROM memory_preferences WHERE "
                                     "user_id = ?1 AND category = ?2",
                                     s_user, "coffee"));
   TEST_ASSERT_EQUAL_INT(1, q("SELECT COUNT(*) FROM memory_preference_sources WHERE "
                              "conversation_id = ?1",
                              conv_a, NULL));
}

/* The conversation that taught the value before is forgotten: the record
 * drops it, and an undo can't bring the value back. */
void test_undo_pref_after_forgetting_the_old_values_conversation(void) {
   int64_t conv_a = 0;
   conv_db_create(s_user, "A", &conv_a);
   const memory_provenance_t prov_a = { .conv_id = conv_a, .msg_id_start = 2, .msg_id_end = 4 };
   memory_db_pref_upsert(s_user, "coffee", "espresso", 0.6f, "inferred", &prov_a);
   script(NULL);
   const int64_t batch = run(PREF_REPLY("oat latte"), 0);
   const int64_t op = op_of(batch, "pref_set");
   TEST_ASSERT_TRUE(op > 0);
   /* Forget A: "oat latte" (taught by the batch's conversation) stays. */
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_conversations_forget(s_user, &conv_a, 1, NULL));
   memory_preference_t p;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_pref_get(s_user, "coffee", &p));
   TEST_ASSERT_EQUAL_STRING("oat latte", p.value);
   /* The record must not keep the forgotten value, and an undo must not
    * restore it. */
   const int64_t quoted = q("SELECT COUNT(*) FROM memory_consolidation_ops WHERE id = ?1 "
                            "AND pre_image LIKE '%espresso%'",
                            op, NULL);
   memory_consolidation_undo_effect_t eff = { 0 };
   (void)memory_db_consolidation_undo_op(s_user, batch, op, &eff);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_pref_get(s_user, "coffee", &p));
   TEST_ASSERT_NOT_EQUAL_MESSAGE(0, strcmp("espresso", p.value),
                                 "undo brought back a forgotten conversation's preference");
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, quoted,
                                 "record still quotes the forgotten conversation's value");
}

/* A preference the batch created goes with its sources. */
void test_undo_created_pref_drops_its_sources(void) {
   script(NULL);
   const int64_t batch = run(PREF_REPLY("oat latte"), 0);
   TEST_ASSERT_EQUAL_INT(1, q("SELECT COUNT(*) FROM memory_preference_sources WHERE "
                              "conversation_id = ?1",
                              s_conv, NULL));
   memory_consolidation_undo_effect_t eff = { 0 };
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_consolidation_undo_op(
                                                s_user, batch, op_of(batch, "pref_set"), &eff));
   TEST_ASSERT_TRUE(eff.memory_bodies);
   TEST_ASSERT_EQUAL_INT(0, q("SELECT COUNT(*) FROM memory_preference_sources", 0, NULL));
   TEST_ASSERT_EQUAL_INT(0, q("SELECT COUNT(*) FROM memory_preferences WHERE user_id = ?1", s_user,
                              NULL));
}

/* A pre-image recorded without its provenance undoes the value only. */
void test_undo_pref_old_pre_image(void) {
   int64_t conv_a = 0;
   conv_db_create(s_user, "A", &conv_a);
   const memory_provenance_t prov_a = { .conv_id = conv_a, .msg_id_start = 2, .msg_id_end = 4 };
   memory_db_pref_upsert(s_user, "coffee", "espresso", 0.6f, "inferred", &prov_a);
   script(NULL);
   const int64_t batch = run(PREF_REPLY("oat latte"), 0);
   const int64_t op = op_of(batch, "pref_set");
   exec("UPDATE memory_consolidation_ops SET pre_image = json_remove(pre_image, "
        "'$.sources', '$.source_conversation_id', '$.msg_id_start', '$.msg_id_end', "
        "'$.origin_unsourced') WHERE op = 'pref_set'");
   memory_consolidation_undo_effect_t eff = { 0 };
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_consolidation_undo_op(s_user, batch, op, &eff));
   memory_preference_t p;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_pref_get(s_user, "coffee", &p));
   TEST_ASSERT_EQUAL_STRING("espresso", p.value);
   TEST_ASSERT_EQUAL_INT64(s_conv, q("SELECT source_conversation_id FROM memory_preferences WHERE "
                                     "user_id = ?1 AND category = ?2",
                                     s_user, "coffee"));
}

/* Same value, new confidence (a reinforcement): the batch's conversation
 * leaves the sources and the range comes back. */
void test_undo_pref_reinforcement(void) {
   int64_t conv_a = 0;
   conv_db_create(s_user, "A", &conv_a);
   const memory_provenance_t prov_a = { .conv_id = conv_a, .msg_id_start = 2, .msg_id_end = 4 };
   memory_db_pref_upsert(s_user, "coffee", "oat latte", 0.6f, "inferred", &prov_a);
   script(NULL);
   const int64_t batch = run(PREF_REPLY("oat latte"), 0);
   const int64_t op = op_of(batch, "pref_set");
   TEST_ASSERT_TRUE(op > 0);
   TEST_ASSERT_EQUAL_INT(2, q("SELECT COUNT(*) FROM memory_preference_sources", 0, NULL));
   memory_consolidation_undo_effect_t eff = { 0 };
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_consolidation_undo_op(s_user, batch, op, &eff));
   TEST_ASSERT_EQUAL_INT(1, q("SELECT COUNT(*) FROM memory_preference_sources WHERE "
                              "conversation_id = ?1",
                              conv_a, NULL));
   TEST_ASSERT_EQUAL_INT(0, q("SELECT COUNT(*) FROM memory_preference_sources WHERE "
                              "conversation_id = ?1",
                              s_conv, NULL));
   TEST_ASSERT_EQUAL_INT64(2, q("SELECT source_msg_id_start FROM memory_preferences WHERE "
                                "user_id = ?1 AND category = ?2",
                                s_user, "coffee"));
}

/* A reinforcement of a value two conversations taught, one forgotten: the
 * undo puts back the other alone, as the row's own source. */
void test_undo_pref_reinforcement_after_forgetting_one_source(void) {
   int64_t conv_a = 0;
   int64_t conv_b = 0;
   conv_db_create(s_user, "A", &conv_a);
   conv_db_create(s_user, "B", &conv_b);
   const memory_provenance_t prov_a = { .conv_id = conv_a, .msg_id_start = 2, .msg_id_end = 4 };
   const memory_provenance_t prov_b = { .conv_id = conv_b, .msg_id_start = 5, .msg_id_end = 6 };
   memory_db_pref_upsert(s_user, "coffee", "oat latte", 0.5f, "inferred", &prov_b);
   memory_db_pref_upsert(s_user, "coffee", "oat latte", 0.6f, "inferred", &prov_a);
   script(NULL);
   const int64_t batch = run(PREF_REPLY("oat latte"), 0);
   const int64_t op = op_of(batch, "pref_set");
   TEST_ASSERT_TRUE(op > 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_conversations_forget(s_user, &conv_a, 1, NULL));
   /* The record keeps B alone. */
   TEST_ASSERT_EQUAL_INT64(conv_b, q("SELECT group_concat(j.value) FROM memory_consolidation_ops "
                                     "o, json_each(o.pre_image, '$.sources') j WHERE o.id = ?1",
                                     op, NULL));
   TEST_ASSERT_EQUAL_INT64(0, q("SELECT json_extract(pre_image, '$.source_conversation_id') FROM "
                                "memory_consolidation_ops WHERE id = ?1",
                                op, NULL));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, undo(batch, op));
   TEST_ASSERT_EQUAL_INT(0, q("SELECT COUNT(*) FROM memory_preference_sources WHERE "
                              "conversation_id = ?1",
                              conv_a, NULL));
   TEST_ASSERT_EQUAL_INT(1, q("SELECT COUNT(*) FROM memory_preference_sources WHERE "
                              "conversation_id = ?1",
                              conv_b, NULL));
   TEST_ASSERT_EQUAL_INT64(conv_b, q("SELECT source_conversation_id FROM memory_preferences WHERE "
                                     "user_id = ?1 AND category = ?2",
                                     s_user, "coffee"));
   TEST_ASSERT_EQUAL_INT64(0, q("SELECT COALESCE(source_msg_id_start, 0) FROM memory_preferences "
                                "WHERE user_id = ?1 AND category = ?2",
                                s_user, "coffee"));
}

/* Undone, the value's reinforcement count and update time are the ones
 * before the batch. */
void test_undo_pref_restores_count_and_updated_at(void) {
   memory_db_pref_upsert(s_user, "coffee", "espresso", 0.6f, "inferred", NULL);
   exec("UPDATE memory_preferences SET reinforcement_count = 5, updated_at = 1000");
   script(NULL);
   const int64_t batch = run(PREF_REPLY("oat latte"), 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, undo(batch, op_of(batch, "pref_set")));
   TEST_ASSERT_EQUAL_INT64(5, q("SELECT reinforcement_count FROM memory_preferences WHERE "
                                "user_id = ?1 AND category = ?2",
                                s_user, "coffee"));
   TEST_ASSERT_EQUAL_INT64(1000, q("SELECT updated_at FROM memory_preferences WHERE "
                                   "user_id = ?1 AND category = ?2",
                                   s_user, "coffee"));
}

/* The conversation the value before was learned from was deleted since: the
 * value comes back learned outside any conversation, with no range. */
void test_undo_pref_after_deleting_the_old_values_conversation(void) {
   int64_t conv_a = 0;
   conv_db_create(s_user, "A", &conv_a);
   const memory_provenance_t prov_a = { .conv_id = conv_a, .msg_id_start = 2, .msg_id_end = 4 };
   memory_db_pref_upsert(s_user, "coffee", "espresso", 0.6f, "inferred", &prov_a);
   script(NULL);
   const int64_t batch = run(PREF_REPLY("oat latte"), 0);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_delete(conv_a, s_user));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, undo(batch, op_of(batch, "pref_set")));
   memory_preference_t p;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_pref_get(s_user, "coffee", &p));
   TEST_ASSERT_EQUAL_STRING("espresso", p.value);
   TEST_ASSERT_EQUAL_INT64(1, q("SELECT origin_unsourced FROM memory_preferences WHERE "
                                "user_id = ?1 AND category = ?2",
                                s_user, "coffee"));
   TEST_ASSERT_EQUAL_INT64(0, q("SELECT COALESCE(source_conversation_id, 0) + "
                                "COALESCE(source_msg_id_start, 0) + COALESCE(source_msg_id_end, 0) "
                                "FROM memory_preferences WHERE user_id = ?1 AND category = ?2",
                                s_user, "coffee"));
}

/* ---- undo never removes what a later batch changed (stale) ---- */

static void assert_stale(int64_t batch, int64_t op) {
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATION_STALE, undo(batch, op));
   TEST_ASSERT_EQUAL_STRING("stale", status_of(op));
}

/* Forget A clears the pre-image's own source to 0; B, the last remaining
 * source, is then deleted; the undo counts the value as learned outside any
 * conversation, as a conversation delete would. */
void test_undo_pref_after_forget_then_delete_last_source(void) {
   int64_t conv_a = 0, conv_b = 0;
   conv_db_create(s_user, "A", &conv_a);
   conv_db_create(s_user, "B", &conv_b);
   const memory_provenance_t pb = { .conv_id = conv_b, .msg_id_start = 5, .msg_id_end = 6 };
   const memory_provenance_t pa = { .conv_id = conv_a, .msg_id_start = 2, .msg_id_end = 4 };
   memory_db_pref_upsert(s_user, "coffee", "espresso", 0.5f, "inferred", &pb);
   memory_db_pref_upsert(s_user, "coffee", "espresso", 0.6f, "inferred", &pa);
   script(NULL);
   const int64_t batch = run(PREF_REPLY("oat latte"), 0);
   const int64_t op = op_of(batch, "pref_set");
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_conversations_forget(s_user, &conv_a, 1, NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_delete(conv_b, s_user));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, undo(batch, op));
   TEST_ASSERT_EQUAL_INT(0, q("SELECT COUNT(*) FROM memory_preference_sources", 0, NULL));
   TEST_ASSERT_EQUAL_INT64(0,
                           q("SELECT COALESCE(source_conversation_id, 0) FROM memory_preferences "
                             "WHERE user_id = ?1 AND category = ?2",
                             s_user, "coffee"));
   TEST_ASSERT_EQUAL_INT64_MESSAGE(1,
                                   q("SELECT origin_unsourced FROM memory_preferences WHERE "
                                     "user_id = ?1 AND category = ?2",
                                     s_user, "coffee"),
                                   "orphan: no source and origin_unsourced 0");
}

/* A conversation that taught the value before teaches the batch's new value
 * since: the undo would put the old value back over it, so it is stale. */
void test_stale_pref_old_source_reteaches_new_value(void) {
   int64_t conv_a = 0;
   conv_db_create(s_user, "A", &conv_a);
   const memory_provenance_t pa = { .conv_id = conv_a, .msg_id_start = 2, .msg_id_end = 4 };
   memory_db_pref_upsert(s_user, "coffee", "espresso", 0.6f, "inferred", &pa);
   script(NULL);
   const int64_t batch = run(PREF_REPLY("oat latte"), 0);
   const memory_provenance_t pa2 = { .conv_id = conv_a, .msg_id_start = 20, .msg_id_end = 24 };
   memory_db_pref_upsert(s_user, "coffee", "oat latte", 0.8f, "inferred", &pa2);
   assert_stale(batch, op_of(batch, "pref_set"));
}

/* Another conversation taught the batch's value since, outside the record:
 * the undo would take its teaching; it stays, with its source. */
void test_stale_pref_another_conversation_taught_it(void) {
   script(NULL);
   const int64_t batch = run(PREF_REPLY("oat latte"), 0);
   int64_t conv_c = 0;
   conv_db_create(s_user, "C", &conv_c);
   const memory_provenance_t prov_c = { .conv_id = conv_c, .msg_id_start = 7, .msg_id_end = 9 };
   memory_db_pref_upsert(s_user, "coffee", "oat latte", 0.8f, "inferred", &prov_c);
   assert_stale(batch, op_of(batch, "pref_set"));
   TEST_ASSERT_EQUAL_INT(1, q("SELECT COUNT(*) FROM memory_preference_sources WHERE "
                              "conversation_id = ?1",
                              conv_c, NULL));
}

/* Learned outside any conversation since (an import): stale too. */
void test_stale_pref_learned_unsourced_since(void) {
   script(NULL);
   const int64_t batch = run(PREF_REPLY("oat latte"), 0);
   memory_db_pref_upsert(s_user, "coffee", "oat latte", 0.8f, "imported", NULL);
   assert_stale(batch, op_of(batch, "pref_set"));
   memory_preference_t p;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_pref_get(s_user, "coffee", &p));
}

void test_stale_added_another_conversation_taught_it(void) {
   script(NULL);
   const int64_t batch = run(ONE_FACT(CELLO), 0);
   int64_t conv2 = 0;
   conv_db_create(s_user, "Later", &conv2);
   memory_db_fact_provenance_extend(current(CELLO), s_user, conv2, 7, 9);
   assert_stale(batch, op_of(batch, "added"));
   TEST_ASSERT_TRUE(current(CELLO) > 0);
}

void test_stale_added_something_since_merged_into_it(void) {
   script(NULL);
   const int64_t batch = run(ONE_FACT(CELLO), 0);
   const int64_t other = stored("Jonathan Smith plays cello", "inferred");
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_merge(other, current(CELLO), s_user));
   assert_stale(batch, op_of(batch, "added"));
   TEST_ASSERT_TRUE(current(CELLO) > 0);
}

void test_stale_reinforced_made_told_since(void) {
   const int64_t fact = stored(CELLO, "inferred");
   script(NULL);
   const int64_t batch = run(ONE_FACT(CELLO), 0);
   memory_db_fact_mark_explicit(fact, s_user);
   assert_stale(batch, op_of(batch, "reinforced"));
}

void test_stale_merged_restored_since(void) {
   const int64_t short_one = stored("Jonathan Smith works at Acme", "explicit");
   script("{\"ops\": [{\"op\": \"merge\", \"from\": \"S1\", \"into\": \"C1\"}], \"finish\": true}");
   const int64_t batch = run(ONE_FACT("Jonathan Smith works at Acme as a senior engineer"), 0);
   memory_db_fact_restore(s_user, short_one, NULL);
   assert_stale(batch, op_of(batch, "merged"));
}

void test_stale_replaced_replaced_again_since(void) {
   const int64_t old = stored("Jonathan Smith lives in Portland", "explicit");
   script("{\"ops\": [{\"op\": \"replace\", \"old\": \"S1\", \"by\": \"C1\"}], \"finish\": true}");
   const int64_t batch = run(ONE_FACT("Jonathan Smith lives in Springfield"), 0);
   memory_db_fact_restore(s_user, old, NULL);
   const int64_t third = stored("Jonathan Smith lives in Shelbyville", "explicit");
   memory_db_fact_replace(old, third, s_user);
   assert_stale(batch, op_of(batch, "replaced"));
   TEST_ASSERT_EQUAL_INT64(third, superseded_by(old));
}

void test_stale_closed_reopened_since(void) {
   const int64_t blue = blue_relation();
   script(CLOSE_PLAN);
   const int64_t batch = run(GREEN_REPLY, 0);
   memory_db_relation_reopen(s_user, blue);
   assert_stale(batch, op_of(batch, "closed"));
}

void test_stale_linked_unlinked_since(void) {
   int64_t link = 0;
   int64_t source = 0;
   int64_t target = 0;
   const int64_t batch = linked_batch(&link, &source, &target);
   memory_db_entity_alias_unlink(s_user, link, "test");
   assert_stale(batch, op_of(batch, "linked"));
}

void test_stale_pref_set_changed_since(void) {
   script(NULL);
   const int64_t batch = run(PREF_REPLY("oat latte"), 0);
   memory_db_pref_upsert(s_user, "coffee", "cortado", 0.9f, "explicit", NULL);
   assert_stale(batch, op_of(batch, "pref_set"));
   memory_preference_t p;
   memory_db_pref_get(s_user, "coffee", &p);
   TEST_ASSERT_EQUAL_STRING("cortado", p.value);
}

/* A whole-batch undo with one op changed since: the rest undone, partial. */
void test_whole_batch_undo_partial_lists_the_stale_ops(void) {
   const int64_t old = stored("Jonathan Smith lives in Portland", "explicit");
   script("{\"ops\": [{\"op\": \"replace\", \"old\": \"S1\", \"by\": \"C1\"}, {\"op\": \"add\", "
          "\"c\": \"C2\"}], \"finish\": true}");
   const int64_t batch = run(TWO_FACTS("Jonathan Smith lives in Springfield", CELLO), 0);
   int64_t conv2 = 0;
   conv_db_create(s_user, "Later", &conv2);
   memory_db_fact_provenance_extend(current(CELLO), s_user, conv2, 7, 9);
   memory_consolidation_outcome_t io;
   outcome(&io, MEMORY_CONSOLIDATION_UNDO_BATCH, s_user, batch, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATION_PARTIAL, io.status);
   TEST_ASSERT_EQUAL_STRING("partial", memory_consolidation_status_name(io.status));
   TEST_ASSERT_EQUAL_INT(2, io.n_undone); /* the replace, and Springfield's add */
   TEST_ASSERT_EQUAL_INT(1, io.n_stale);
   TEST_ASSERT_EQUAL_INT64(q("SELECT id FROM memory_consolidation_ops WHERE fact_id = ?1 AND op = "
                             "'added'",
                             current(CELLO), NULL),
                           io.stale[0]);
   TEST_ASSERT_EQUAL_INT(0, superseded_by(old));
   TEST_ASSERT_TRUE(current(CELLO) > 0);
   /* Again: nothing left to undo, nothing changed. */
   outcome(&io, MEMORY_CONSOLIDATION_UNDO_BATCH, s_user, batch, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATION_OK, io.status);
   TEST_ASSERT_EQUAL_INT(0, io.n_undone);
}

void test_an_undo_with_everything_changed_since_is_stale(void) {
   script(NULL);
   const int64_t batch = run(ONE_FACT(CELLO), 0);
   int64_t conv2 = 0;
   conv_db_create(s_user, "Later", &conv2);
   memory_db_fact_provenance_extend(current(CELLO), s_user, conv2, 7, 9);
   memory_consolidation_outcome_t io;
   outcome(&io, MEMORY_CONSOLIDATION_UNDO_OP, s_user, batch, op_of(batch, "added"));
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATION_ALL_STALE, io.status);
   TEST_ASSERT_EQUAL_STRING("This memory changed since; nothing was undone.", io.error);
}

/* ---- a user can only act on their own batches ---- */

void test_a_user_can_only_act_on_their_own_batches(void) {
   script(NULL);
   const int64_t batch = run(ONE_FACT(CELLO), 0);
   const int64_t added = op_of(batch, "added");
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_NOT_FOUND,
                         memory_db_consolidation_undo_op(s_other_user, batch, added, NULL));
   memory_consolidation_outcome_t io;
   outcome(&io, MEMORY_CONSOLIDATION_UNDO_BATCH, s_other_user, batch, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATION_ERROR, io.status);
   TEST_ASSERT_EQUAL_STRING("Not found", io.error);
   outcome(&io, MEMORY_CONSOLIDATION_UNDO_BATCH, s_user, batch + 1000, 0);
   TEST_ASSERT_EQUAL_STRING("Not found", io.error); /* missing and foreign alike */
   outcome(&io, MEMORY_CONSOLIDATION_KEEP_OP, s_other_user, batch, added);
   TEST_ASSERT_EQUAL_STRING("Not found", io.error);
   TEST_ASSERT_TRUE(current(CELLO) > 0);
}

void test_an_op_named_with_another_batch_is_refused(void) {
   script(NULL);
   const int64_t first = run(ONE_FACT(CELLO), 0);
   const int64_t second = run(ONE_FACT("Jonathan Smith collects stamps"), 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_NOT_FOUND, undo(second, op_of(first, "added")));
   TEST_ASSERT_TRUE(current(CELLO) > 0);
}

/* ---- Keep: outside text never acts as the user's own word ---- */

static int64_t dropped_batch(int outside) {
   script("{\"ops\": [{\"op\": \"add\", \"c\": \"C1\"}, {\"op\": \"drop\", \"c\": \"C2\"}], "
          "\"finish\": true}");
   return run(TWO_FACTS(CELLO, "Jonathan Smith is visiting Lisbon"), outside);
}

void test_keep_stores_a_dropped_candidate_with_the_batch_taint(void) {
   const int64_t batch = dropped_batch(OUTSIDE_EMAIL);
   const int64_t dropped = op_of(batch, "dropped");
   memory_consolidation_outcome_t io;
   outcome(&io, MEMORY_CONSOLIDATION_KEEP_OP, s_user, batch, dropped);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATION_OK, io.status);
   const int64_t kept = current("Jonathan Smith is visiting Lisbon");
   TEST_ASSERT_TRUE(kept > 0);
   TEST_ASSERT_EQUAL_INT64(kept, io.fact_id);
   TEST_ASSERT_EQUAL_INT(OUTSIDE_EMAIL,
                         q("SELECT outside FROM memory_facts WHERE id = ?1", kept, NULL));
   TEST_ASSERT_EQUAL_STRING("kept", status_of(dropped));
   /* The new op joins the batch, and can be undone. */
   const int64_t added = q("SELECT id FROM memory_consolidation_ops WHERE fact_id = ?1 AND op = "
                           "'added'",
                           kept, NULL);
   TEST_ASSERT_TRUE(added > 0);
   TEST_ASSERT_EQUAL_INT(OUTSIDE_EMAIL, q("SELECT outside FROM memory_consolidation_ops WHERE "
                                          "id = ?1",
                                          added, NULL));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, undo(batch, added));
   TEST_ASSERT_EQUAL_INT(0, current("Jonathan Smith is visiting Lisbon"));
   /* Kept once: not again. */
   outcome(&io, MEMORY_CONSOLIDATION_KEEP_OP, s_user, batch, dropped);
   TEST_ASSERT_EQUAL_STRING("This one can't be kept.", io.error);
}

void test_keep_refuses_what_the_user_forgot(void) {
   const int64_t batch = dropped_batch(0);
   const int64_t dropped = op_of(batch, "dropped");
   const int64_t gone = stored("Jonathan Smith is visiting Lisbon", "explicit");
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_forget(gone, s_user, NULL));
   /* The forget scrubs the record's rows with its words: record the drop again. */
   const memory_consolidation_op_t op = {
      .batch_id = batch,
      .user_id = s_user,
      .seq = 9,
      .op = "dropped",
      .text = "Jonathan Smith is visiting Lisbon",
      .pre_image = "{\"text\": \"Jonathan Smith is visiting Lisbon\", \"subject\": \"Jonathan "
                   "Smith\"}"
   };
   int64_t again = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_consolidation_op_add(&op, &again));
   (void)dropped;
   memory_consolidation_outcome_t io;
   outcome(&io, MEMORY_CONSOLIDATION_KEEP_OP, s_user, batch, again);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATION_ERROR, io.status);
   TEST_ASSERT_EQUAL_STRING("You asked me to forget this; it stays forgotten.", io.error);
   TEST_ASSERT_EQUAL_INT(0, current("Jonathan Smith is visiting Lisbon"));
   TEST_ASSERT_EQUAL_STRING("applied", status_of(again));
}

void test_keep_takes_an_expired_deferred_candidate(void) {
   script("{\"ops\": [{\"op\": \"add\", \"c\": \"C1\"}, {\"op\": \"defer\", \"c\": \"C2\"}], "
          "\"finish\": true}");
   const int64_t batch = run(TWO_FACTS(CELLO, "Jonathan Smith may move to Lisbon"), 0);
   const int64_t deferred = op_of(batch, "deferred");
   exec("UPDATE memory_consolidation_ops SET created_at = created_at - 40 * 86400 WHERE op = "
        "'deferred'");
   int expired = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_consolidation_expire_deferred(
                                                time(NULL) - 30 * 86400, &expired));
   TEST_ASSERT_EQUAL_INT(1, expired);
   TEST_ASSERT_EQUAL_STRING("expired", status_of(deferred));
   int64_t fact = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, memory_consolidate_keep(s_user, batch, deferred, &fact));
   TEST_ASSERT_EQUAL_INT64(current("Jonathan Smith may move to Lisbon"), fact);
   TEST_ASSERT_EQUAL_STRING("kept", status_of(deferred));
}

/* ---- the record: claim, list, caps, prune ---- */

/* A deferred candidate claimed and its row written in one step: a claim that
 * fails writes nothing. */
void test_a_row_taking_up_a_deferred_candidate_is_written_with_its_claim(void) {
   const memory_consolidation_batch_t b = { .user_id = s_user,
                                            .conversation_id = s_conv,
                                            .path = "agent",
                                            .stop = "finished" };
   int64_t batch = 0;
   memory_db_consolidation_batch_add(&b, &batch);
   const memory_consolidation_op_t d = { .batch_id = batch,
                                         .user_id = s_user,
                                         .seq = 1,
                                         .op = "deferred",
                                         .text = "x",
                                         .pre_image = "{}" };
   int64_t d_id = 0;
   memory_db_consolidation_op_add(&d, &d_id);
   memory_consolidation_op_t row = { .batch_id = batch,
                                     .user_id = s_user,
                                     .seq = 2,
                                     .op = "dropped",
                                     .claim_op_id = d_id };
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_consolidation_op_add(&row, NULL));
   TEST_ASSERT_EQUAL_STRING("consumed", status_of(d_id));
   row.seq = 3;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_NOT_FOUND, memory_db_consolidation_op_add(&row, NULL));
   TEST_ASSERT_EQUAL_INT(2, q("SELECT COUNT(*) FROM memory_consolidation_ops WHERE batch_id = ?1",
                              batch, NULL));
}

void test_a_row_quoting_a_private_conversation_is_not_written(void) {
   int64_t conv2 = 0;
   conv_db_create(s_user, "B", &conv2);
   script(NULL);
   const int64_t batch = run(ONE_FACT(CELLO), 0);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_set_private(conv2, s_user, true));
   const memory_consolidation_op_t row = { .batch_id = batch,
                                           .user_id = s_user,
                                           .seq = 9,
                                           .op = "dropped",
                                           .text = "quoted",
                                           .source_conversation_id = conv2 };
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_NOT_FOUND, memory_db_consolidation_op_add(&row, NULL));
}

void test_list_pages_by_batch_id_and_conversation(void) {
   int64_t conv2 = 0;
   conv_db_create(s_user, "B", &conv2);
   int64_t ids[5];
   for (int i = 0; i < 5; i++) {
      const memory_consolidation_batch_t b = { .user_id = s_user,
                                               .conversation_id = i == 2 ? conv2 : s_conv,
                                               .path = "agent",
                                               .stop = "finished" };
      memory_db_consolidation_batch_add(&b, &ids[i]);
   }
   memory_consolidation_batch_row_t rows[3];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_consolidation_batches_list(s_user, 0, 0, rows, 3, &n));
   TEST_ASSERT_EQUAL_INT(3, n);
   TEST_ASSERT_EQUAL_INT64(ids[4], rows[0].id);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_consolidation_batches_list(s_user, 0, rows[2].id, rows, 3, &n));
   TEST_ASSERT_EQUAL_INT(2, n);
   TEST_ASSERT_EQUAL_INT64(ids[1], rows[0].id);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_consolidation_batches_list(s_user, s_conv, 0, rows, 3, &n));
   TEST_ASSERT_EQUAL_INT(3, n);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_consolidation_batches_list(s_other_user, 0, 0, rows, 3, &n));
   TEST_ASSERT_EQUAL_INT(0, n);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_NOT_FOUND,
                         memory_db_consolidation_batch_get(s_other_user, ids[0], &rows[0]));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_consolidation_conversation_owned(s_user, conv2));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_NOT_FOUND,
                         memory_db_consolidation_conversation_owned(s_other_user, conv2));
}

static const char *target_status(int64_t batch, const char *op) {
   static char out[12];
   memory_consolidation_op_row_t r[16];
   int n = 0;
   memory_db_consolidation_ops_list(s_user, batch, r, 16, &n);
   out[0] = '\0';
   for (int i = 0; i < n; i++) {
      if (strcmp(r[i].op, op) == 0) {
         safe_strscpy(out, r[i].target_status);
         break;
      }
   }
   memory_db_consolidation_ops_free(r, n);
   return out;
}

void test_target_status_follows_the_counterpart(void) {
   stored("Jonathan Smith lives in Portland", "explicit");
   script("{\"ops\": [{\"op\": \"replace\", \"old\": \"S1\", \"by\": \"C1\"}], \"finish\": true}");
   const int64_t batch = run(ONE_FACT("Jonathan Smith lives in Springfield"), 0);
   TEST_ASSERT_EQUAL_STRING("current", target_status(batch, "replaced"));
   TEST_ASSERT_EQUAL_STRING("", target_status(batch, "added"));
   const int64_t newer = current("Jonathan Smith lives in Springfield");
   const int64_t newest = stored("Jonathan Smith lives in Shelbyville", "explicit");
   memory_db_fact_replace(newer, newest, s_user);
   TEST_ASSERT_EQUAL_STRING("superseded", target_status(batch, "replaced"));
   memory_db_fact_restore(s_user, newer, NULL);
   memory_db_fact_delete(newer, s_user);
   TEST_ASSERT_EQUAL_STRING("gone", target_status(batch, "replaced"));
}

void test_the_record_caps_text_on_a_character_boundary(void) {
   const memory_consolidation_batch_t b = { .user_id = s_user,
                                            .conversation_id = s_conv,
                                            .path = "agent",
                                            .stop = "finished" };
   int64_t batch = 0;
   memory_db_consolidation_batch_add(&b, &batch);
   char text[1201];
   char reason[401];
   text[0] = '\0';
   reason[0] = '\0';
   for (int i = 0; i < 600; i++) {
      strcat(text, "\xc3\xa9"); /* é: two bytes */
   }
   for (int i = 0; i < 200; i++) {
      strcat(reason, "\xc3\xa9");
   }
   const memory_consolidation_op_t op = { .batch_id = batch,
                                          .user_id = s_user,
                                          .seq = 1,
                                          .op = "dropped",
                                          .text = text,
                                          .reason = reason };
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_consolidation_op_add(&op, NULL));
   const int64_t tlen = q("SELECT length(CAST(text AS BLOB)) FROM memory_consolidation_ops WHERE "
                          "batch_id = ?1",
                          batch, NULL);
   const int64_t rlen = q("SELECT length(CAST(reason AS BLOB)) FROM memory_consolidation_ops "
                          "WHERE batch_id = ?1",
                          batch, NULL);
   TEST_ASSERT_TRUE(tlen <= MEMORY_FACT_TEXT_MAX - 1 && tlen % 2 == 0 && tlen > 400);
   TEST_ASSERT_TRUE(rlen <= MEMORY_CONSOLIDATE_REASON_MAX && rlen % 2 == 0 && rlen > 100);
}

void test_prune_removes_old_batches_with_their_ops(void) {
   script(NULL);
   const int64_t batch = run(ONE_FACT(CELLO), 0);
   exec("UPDATE memory_consolidation_batches SET created_at = created_at - 40 * 86400");
   int n = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_consolidation_prune(time(NULL) - 30 * 86400, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_INT(0, q("SELECT COUNT(*) FROM memory_consolidation_ops WHERE batch_id = ?1",
                              batch, NULL));
   TEST_ASSERT_TRUE(current(CELLO) > 0); /* the memory stays; only its record goes */
}

/* The ops table's columns and foreign keys, as text. */
static void ops_shape(char *out, size_t size) {
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(
       SQLITE_OK, sqlite3_prepare_v2(
                      s_db.db,
                      "SELECT (SELECT group_concat(name || ':' || type || ':' || \"notnull\" || "
                      "':' || COALESCE(dflt_value, ''), ',') FROM pragma_table_info("
                      "'memory_consolidation_ops')) || '|' || (SELECT group_concat(\"from\" || "
                      "'>' || \"table\" || ':' || on_delete, ',') FROM pragma_foreign_key_list("
                      "'memory_consolidation_ops'))",
                      -1, &st, NULL));
   out[0] = '\0';
   if (sqlite3_step(st) == SQLITE_ROW) {
      snprintf(out, size, "%s", (const char *)sqlite3_column_text(st, 0));
   }
   sqlite3_finalize(st);
}

/* A database that reached v113 before the ops table had its source column
 * gets it by ALTER: the reference still removes a row with its conversation. */
void test_the_altered_source_column_removes_rows_with_their_conversation(void) {
   char path[] = "/tmp/dawn_mco_XXXXXX";
   const int fd = mkstemp(path);
   TEST_ASSERT_TRUE(fd >= 0);
   close(fd);
   auth_db_shutdown();
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(path));
   char fresh[2048];
   ops_shape(fresh, sizeof(fresh));
   /* The table as v113 made it: today's DDL without the two later columns. */
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(s_db.db,
                                                       "SELECT sql FROM sqlite_master WHERE name = "
                                                       "'memory_consolidation_ops'",
                                                       -1, &st, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(st));
   char ddl[4096];
   snprintf(ddl, sizeof(ddl), "%s", (const char *)sqlite3_column_text(st, 0));
   sqlite3_finalize(st);
   char *cut = strstr(ddl, ",   " MEMORY_CONSOLIDATION_OPS_SOURCE_COL);
   TEST_ASSERT_NOT_NULL(cut);
   snprintf(cut, sizeof(ddl) - (size_t)(cut - ddl), ")");
   exec("PRAGMA foreign_keys = OFF;"
        "DROP INDEX IF EXISTS idx_mco_source;"
        "ALTER TABLE memory_consolidation_ops RENAME TO mco_new;");
   exec(ddl);
   exec("INSERT INTO memory_consolidation_ops SELECT id, batch_id, user_id, seq, op, fact_id, "
        "other_fact_id, relation_id, entity_id, other_entity_id, link_id, outside, text, "
        "target_text, reason, pre_image, post_image, status, created_at, undone_at FROM mco_new;"
        "DROP TABLE mco_new;"
        "PRAGMA foreign_keys = ON;");
   TEST_ASSERT_EQUAL_INT(0, q("SELECT COUNT(*) FROM pragma_table_info('memory_consolidation_ops') "
                              "WHERE name = 'source_conversation_id'",
                              0, NULL));
   auth_db_shutdown();
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(path)); /* the step adds them */
   char migrated[2048];
   ops_shape(migrated, sizeof(migrated));
   TEST_ASSERT_EQUAL_STRING(fresh, migrated); /* as a fresh database has them */
   auth_db_shutdown();
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(path)); /* again: nothing changes */
   ops_shape(migrated, sizeof(migrated));
   TEST_ASSERT_EQUAL_STRING(fresh, migrated);
   TEST_ASSERT_EQUAL_INT(1, q("SELECT COUNT(*) FROM pragma_table_info('memory_consolidation_ops') "
                              "WHERE name = 'reason_by'",
                              0, NULL));
   TEST_ASSERT_EQUAL_INT(1, q("SELECT COUNT(*) FROM pragma_foreign_key_list("
                              "'memory_consolidation_ops') WHERE \"from\" = "
                              "'source_conversation_id' AND \"table\" = 'conversations' AND "
                              "on_delete = 'CASCADE'",
                              0, NULL));
   s_user = user_named("jonathan");
   int64_t conv = 0;
   int64_t quoted = 0;
   conv_db_create(s_user, "A", &conv);
   conv_db_create(s_user, "B", &quoted);
   const memory_consolidation_batch_t b = { .user_id = s_user,
                                            .conversation_id = conv,
                                            .path = "agent",
                                            .stop = "finished" };
   int64_t batch = 0;
   memory_db_consolidation_batch_add(&b, &batch);
   const memory_consolidation_op_t op = { .batch_id = batch,
                                          .user_id = s_user,
                                          .seq = 1,
                                          .op = "dropped",
                                          .text = "quoted",
                                          .source_conversation_id = quoted };
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_consolidation_op_add(&op, NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_delete(quoted, s_user));
   TEST_ASSERT_EQUAL_INT(0, q("SELECT COUNT(*) FROM memory_consolidation_ops WHERE batch_id = ?1",
                              batch, NULL));
   auth_db_shutdown();
   unlink(path);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:")); /* for tearDown */
}

/* ---- review cases: a later change found, whatever made it ---- */

/* An add a later batch of the same conversation said again: that batch's
 * reinforce would go with it, so the add is stale. */
void test_an_add_a_later_batch_restated_is_stale(void) {
   script(NULL);
   const int64_t b1 = run(ONE_FACT(CELLO), 0);
   const int64_t fact = current(CELLO);
   script(NULL);
   const int64_t b2 = run(ONE_FACT(CELLO), 0);
   TEST_ASSERT_TRUE(op_of(b2, "reinforced") > 0);
   assert_stale(b1, op_of(b1, "added"));
   TEST_ASSERT_EQUAL_INT64(fact, current(CELLO));
   /* The later batch undone first, the add can go. */
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, undo(b2, op_of(b2, "reinforced")));
   exec("UPDATE memory_consolidation_ops SET status = 'applied' WHERE op = 'added'");
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, undo(b1, op_of(b1, "added")));
   TEST_ASSERT_EQUAL_INT(0, current(CELLO));
}

/* One closed op undone while the value that closed it is open: a one-at-a-
 * time relation never gets two open values. */
void test_a_close_undone_beside_the_newer_value_is_stale(void) {
   blue_relation();
   script(CLOSE_PLAN);
   const int64_t batch = run(GREEN_REPLY, 0);
   assert_stale(batch, op_of(batch, "closed"));
   TEST_ASSERT_EQUAL_INT(1, q("SELECT COUNT(*) FROM memory_relations WHERE relation = ?2 AND "
                              "valid_to IS NULL AND user_id = ?1",
                              s_user, "favorite_color"));
}

/* A preference set back to what this batch set: a later batch's value. */
void test_a_preference_a_later_batch_set_again_is_stale(void) {
   script(NULL);
   const int64_t b1 = run(PREF_REPLY("oat latte"), 0);
   script(NULL);
   run(PREF_REPLY("cortado"), 0);
   script(NULL);
   run(PREF_REPLY("oat latte"), 0);
   assert_stale(b1, op_of(b1, "pref_set"));
   memory_preference_t p;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_pref_get(s_user, "coffee", &p));
   TEST_ASSERT_EQUAL_STRING("oat latte", p.value);
}

/* Restored and merged into the same fact again: another merge, stale. */
void test_a_merge_restored_and_merged_again_is_stale(void) {
   const int64_t short_one = stored("Jonathan Smith works at Acme", "explicit");
   script("{\"ops\": [{\"op\": \"merge\", \"from\": \"S1\", \"into\": \"C1\"}], \"finish\": true}");
   const int64_t batch = run(ONE_FACT("Jonathan Smith works at Acme as a senior engineer"), 0);
   const int64_t keeper = superseded_by(short_one);
   TEST_ASSERT_TRUE(keeper > 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_restore(s_user, short_one, NULL));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_merge(short_one, keeper, s_user));
   assert_stale(batch, op_of(batch, "merged"));
   TEST_ASSERT_EQUAL_INT64(keeper, superseded_by(short_one));
}

/* Every op changed since: the batch is not undone. */
void test_a_batch_with_every_op_stale_is_not_undone(void) {
   script(NULL);
   const int64_t batch = run(ONE_FACT(CELLO), 0);
   int64_t conv2 = 0;
   conv_db_create(s_user, "Later", &conv2);
   memory_db_fact_provenance_extend(current(CELLO), s_user, conv2, 7, 9);
   memory_consolidation_outcome_t io;
   outcome(&io, MEMORY_CONSOLIDATION_UNDO_BATCH, s_user, batch, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATION_ALL_STALE, io.status);
   TEST_ASSERT_EQUAL_INT(0, q("SELECT undone_at IS NOT NULL FROM memory_consolidation_batches "
                              "WHERE id = ?1",
                              batch, NULL));
}

/* A stored fact from another conversation merged into a new candidate, the
 * whole batch undone: the merge takes back the sources it copied, so the
 * add goes too. */
void test_undoing_a_merge_takes_back_the_sources_it_copied(void) {
   int64_t old_conv = 0;
   conv_db_create(s_user, "Older", &old_conv);
   memory_provenance_t prov = { .conv_id = old_conv, .msg_id_start = 1, .msg_id_end = 1 };
   int64_t short_one = 0;
   memory_db_fact_create(s_user, "Jonathan Smith works at Acme", 0.7f, "explicit", &prov,
                         &short_one);
   script("{\"ops\": [{\"op\": \"merge\", \"from\": \"S1\", \"into\": \"C1\"}], \"finish\": true}");
   const int64_t batch = run(ONE_FACT("Jonathan Smith works at Acme as a senior engineer"), 0);
   TEST_ASSERT_TRUE(superseded_by(short_one) > 0);
   memory_consolidation_outcome_t io;
   outcome(&io, MEMORY_CONSOLIDATION_UNDO_BATCH, s_user, batch, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATION_OK, io.status);
   TEST_ASSERT_EQUAL_INT(0, current("Jonathan Smith works at Acme as a senior engineer"));
   TEST_ASSERT_EQUAL_INT(0, superseded_by(short_one));
}

/* ---- the cap ---- */

/* A batch recorded to the cap: the next op isn't written, and the whole
 * batch undoes. */
void test_a_batch_at_the_cap_undoes_fully(void) {
   const memory_consolidation_batch_t b = { .user_id = s_user,
                                            .conversation_id = s_conv,
                                            .path = "agent",
                                            .stop = "finished" };
   int64_t batch = 0;
   memory_db_consolidation_batch_add(&b, &batch);
   for (int i = 0; i < MEMORY_CONSOLIDATION_OPS_MAX; i++) {
      char text[64];
      snprintf(text, sizeof(text), "Jonathan Smith owns book %d", i);
      const int64_t fact = stored(text, "inferred");
      char post[160];
      snprintf(post, sizeof(post), "{\"text\": \"%s\", \"superseded_by\": null}", text);
      const memory_consolidation_op_t op = { .batch_id = batch,
                                             .user_id = s_user,
                                             .seq = i + 1,
                                             .op = "added",
                                             .fact_id = fact,
                                             .text = text,
                                             .post_image = post };
      TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_consolidation_op_add(&op, NULL));
   }
   const memory_consolidation_op_t over = { .batch_id = batch,
                                            .user_id = s_user,
                                            .seq = 999,
                                            .op = "dropped",
                                            .text = "over" };
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATION_FULL, memory_db_consolidation_op_add(&over, NULL));
   memory_consolidation_outcome_t io;
   outcome(&io, MEMORY_CONSOLIDATION_UNDO_BATCH, s_user, batch, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATION_OK, io.status);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATION_OPS_MAX, io.n_undone);
   TEST_ASSERT_EQUAL_INT(0,
                         q("SELECT COUNT(*) FROM memory_facts WHERE user_id = ?1", s_user, NULL));
}

/* A Keep whose record is refused (the batch full) leaves no fact behind. */
void test_a_keep_its_record_refused_stores_nothing(void) {
   const int64_t batch = dropped_batch(0);
   const int64_t dropped = op_of(batch, "dropped");
   const int have = (int)q("SELECT COUNT(*) FROM memory_consolidation_ops WHERE batch_id = ?1",
                           batch, NULL);
   for (int i = have; i < MEMORY_CONSOLIDATION_OPS_MAX; i++) {
      const memory_consolidation_op_t pad = { .batch_id = batch,
                                              .user_id = s_user,
                                              .seq = 100 + i,
                                              .op = "dropped",
                                              .text = "pad" };
      memory_db_consolidation_op_add(&pad, NULL);
   }
   memory_consolidation_outcome_t io;
   outcome(&io, MEMORY_CONSOLIDATION_KEEP_OP, s_user, batch, dropped);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATION_ERROR, io.status);
   TEST_ASSERT_EQUAL_STRING("This batch is full; nothing more can be kept from it.", io.error);
   TEST_ASSERT_EQUAL_INT(0, current("Jonathan Smith is visiting Lisbon"));
   TEST_ASSERT_EQUAL_STRING("applied", status_of(dropped));
}

/* ---- who wrote the reason; what a delete or privacy removes ---- */

void test_the_record_says_who_wrote_each_reason(void) {
   const int64_t old = stored("Jonathan Smith lives in Portland", "explicit");
   (void)old;
   script("{\"ops\": [{\"op\": \"replace\", \"old\": \"S1\", \"by\": \"C1\", \"reason\": \"he "
          "moved\"}], \"finish\": true}");
   const int64_t batch = run(ONE_FACT("Jonathan Smith lives in Springfield"), 0);
   TEST_ASSERT_EQUAL_INT(1, q("SELECT COUNT(*) FROM memory_consolidation_ops WHERE batch_id = ?1 "
                              "AND op = 'replaced' AND reason_by = 'model'",
                              batch, NULL));
   script(NULL);
   const int64_t triaged = run(ONE_FACT(CELLO), 0);
   TEST_ASSERT_EQUAL_INT(1, q("SELECT COUNT(*) FROM memory_consolidation_ops WHERE batch_id = ?1 "
                              "AND op = 'added' AND reason_by = 'dawn'",
                              triaged, NULL));
}

void test_deleting_a_preference_removes_its_record_rows(void) {
   script(NULL);
   const int64_t batch = run(PREF_REPLY("oat latte"), 0);
   TEST_ASSERT_TRUE(op_of(batch, "pref_set") > 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_pref_delete(s_user, "coffee"));
   TEST_ASSERT_EQUAL_INT(0, op_of(batch, "pref_set"));
}

void test_a_private_conversations_batch_is_not_listed(void) {
   script(NULL);
   const int64_t batch = run(ONE_FACT(CELLO), 0);
   exec("UPDATE conversations SET is_private = 1");
   memory_consolidation_batch_row_t row;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_NOT_FOUND,
                         memory_db_consolidation_batch_get(s_user, batch, &row));
   int n = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_consolidation_batches_list(s_user, 0, 0, &row, 1, &n));
   TEST_ASSERT_EQUAL_INT(0, n);
}

/* ---- second review cases ---- */

/* A later batch's restatement left in place (its own undo found it stale)
 * still counts: the add stays. */
void test_a_later_restatement_left_stale_still_counts(void) {
   script(NULL);
   const int64_t b1 = run(ONE_FACT(CELLO), 0);
   script(NULL);
   const int64_t b2 = run(ONE_FACT(CELLO), 0);
   script(NULL);
   const int64_t b3 = run(ONE_FACT(CELLO), 0);
   assert_stale(b2, op_of(b2, "reinforced"));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, undo(b3, op_of(b3, "reinforced")));
   assert_stale(b1, op_of(b1, "added"));
   TEST_ASSERT_TRUE(current(CELLO) > 0);
}

/* A merge outside any batch made the keeper tainted: undoing the batch's
 * earlier merge leaves the keeper's kind as it is. */
void test_undoing_a_merge_leaves_the_keepers_taint(void) {
   const int64_t s1 = stored("Jonathan Smith works at Acme", "explicit");
   script("{\"ops\": [{\"op\": \"merge\", \"from\": \"S1\", \"into\": \"C1\"}], \"finish\": true}");
   const int64_t b1 = run(ONE_FACT("Jonathan Smith works at Acme as a senior engineer"), 0);
   const int64_t keeper = superseded_by(s1);
   TEST_ASSERT_TRUE(keeper > 0);
   const int64_t s2 = stored("Jonathan Smith is employed at Acme", "explicit");
   exec("UPDATE memory_facts SET outside = 3 WHERE fact_text = 'Jonathan Smith is employed at "
        "Acme'");
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_merge(s2, keeper, s_user));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, undo(b1, op_of(b1, "merged")));
   TEST_ASSERT_EQUAL_INT(3, q("SELECT outside FROM memory_facts WHERE id = ?1", keeper, NULL));
}

/* A later batch restated the fact a merge kept: the merge is stale (its undo
 * would take that batch's source with it). */
void test_a_merge_whose_keeper_a_later_batch_restated_is_stale(void) {
   int64_t old_conv = 0;
   conv_db_create(s_user, "Older", &old_conv);
   memory_provenance_t prov = { .conv_id = old_conv, .msg_id_start = 1, .msg_id_end = 1 };
   int64_t s1 = 0;
   memory_db_fact_create(s_user, "Jonathan Smith works at Acme", 0.7f, "explicit", &prov, &s1);
   script("{\"ops\": [{\"op\": \"merge\", \"from\": \"S1\", \"into\": \"C1\"}], \"finish\": true}");
   const int64_t b1 = run(ONE_FACT("Jonathan Smith works at Acme as a senior engineer"), 0);
   const int64_t keeper = superseded_by(s1);
   TEST_ASSERT_TRUE(keeper > 0);
   const int64_t main_conv = s_conv;
   s_conv = old_conv;
   script(NULL);
   const int64_t b2 = run(ONE_FACT("Jonathan Smith works at Acme as a senior engineer"), 0);
   s_conv = main_conv;
   TEST_ASSERT_TRUE(op_of(b2, "reinforced") > 0);
   assert_stale(b1, op_of(b1, "merged"));
   TEST_ASSERT_EQUAL_INT(1, (int)q("SELECT COUNT(*) FROM memory_fact_sources WHERE fact_id = ?1 "
                                   "AND conversation_id = (SELECT id FROM conversations WHERE "
                                   "title = 'Older')",
                                   keeper, NULL));
}

/* A batch whose record fills up: nothing past the cap is applied without its
 * row, and it stops as full. */
void test_a_batch_that_fills_its_record_stops_cleanly(void) {
   strbuf_t reply;
   strbuf_init(&reply, 64 * 1024);
   strbuf_append(&reply, "{\"facts\": [");
   for (int i = 0; i < 300; i++) {
      strbuf_appendf(&reply,
                     "%s{\"text\": \"Jonathan Smith owns rare item number %d\", "
                     "\"subject\": \"Jonathan Smith\"}",
                     i ? ", " : "", i);
   }
   strbuf_append(&reply, "]}");
   TEST_ASSERT_FALSE(strbuf_oom(&reply));
   script(NULL);
   const int64_t batch = run(strbuf_str(&reply), 0);
   strbuf_free(&reply);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATION_OPS_MAX,
                         (int)q("SELECT COUNT(*) FROM memory_consolidation_ops WHERE batch_id = ?1",
                                batch, NULL));
   TEST_ASSERT_EQUAL_INT(1, q("SELECT stop = 'full' AND path = 'agent_partial' FROM "
                              "memory_consolidation_batches WHERE id = ?1",
                              batch, NULL));
   /* Every fact stored has its row. */
   TEST_ASSERT_EQUAL_INT(q("SELECT COUNT(*) FROM memory_consolidation_ops WHERE batch_id = ?1 AND "
                           "op = 'added' AND status = 'applied'",
                           batch, NULL),
                         q("SELECT COUNT(*) FROM memory_facts WHERE user_id = ?1", s_user, NULL));
}

/* Keep of a text a current fact already says: the op kept, that fact named,
 * nothing reinforced. */
void test_keeping_what_is_already_stored_only_marks_it_kept(void) {
   const int64_t batch = dropped_batch(0);
   const int64_t dropped = op_of(batch, "dropped");
   const int64_t same = stored("Jonathan Smith is visiting Lisbon", "inferred");
   const int64_t rows_before = q("SELECT COUNT(*) FROM memory_consolidation_ops WHERE batch_id = "
                                 "?1",
                                 batch, NULL);
   memory_consolidation_outcome_t io;
   outcome(&io, MEMORY_CONSOLIDATION_KEEP_OP, s_user, batch, dropped);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATION_OK, io.status);
   TEST_ASSERT_EQUAL_INT64(same, io.fact_id);
   TEST_ASSERT_EQUAL_STRING("kept", status_of(dropped));
   TEST_ASSERT_EQUAL_INT64(rows_before, q("SELECT COUNT(*) FROM memory_consolidation_ops WHERE "
                                          "batch_id = ?1",
                                          batch, NULL));
   TEST_ASSERT_EQUAL_INT(1, q("SELECT ABS(confidence - 0.7) < 0.001 FROM memory_facts WHERE id = "
                              "?1",
                              same, NULL));
}

/* Undoing an add keeps an entity a later batch linked as another's alias. */
void test_an_add_undo_keeps_an_entity_linked_as_an_alias(void) {
   script(NULL);
   const int64_t batch = run(ONE_FACT(CELLO), 0);
   const int64_t jon = q("SELECT subject_entity_id FROM memory_facts WHERE id = ?1", current(CELLO),
                         NULL);
   TEST_ASSERT_TRUE(jon > 0);
   bool created = false;
   int64_t other = 0;
   memory_db_entity_upsert(s_user, "J. Smith", "person", "j. smith", &created, &other);
   int64_t link = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_entity_alias_link(s_user, jon, other, "soft",
                                                                        "test", 0.9f, NULL, &link));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, undo(batch, op_of(batch, "added")));
   TEST_ASSERT_EQUAL_INT(1, q("SELECT COUNT(*) FROM memory_entities WHERE id = ?1", jon, NULL));
}

/* The plans: one conversation's batches through idx_mcb_conv; a later change
 * on a fact through the fact indexes. */
static void plan_uses(const char *sql, const char *index) {
   char buf[2048];
   snprintf(buf, sizeof(buf), "EXPLAIN QUERY PLAN %s", sql);
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(s_db.db, buf, -1, &st, NULL));
   bool found = false;
   while (sqlite3_step(st) == SQLITE_ROW) {
      found = found || strstr((const char *)sqlite3_column_text(st, 3), index) != NULL;
   }
   sqlite3_finalize(st);
   TEST_ASSERT_TRUE_MESSAGE(found, index);
}

void test_the_record_queries_use_their_indexes(void) {
   /* 400 batches over 40 conversations. */
   int64_t first = 0;
   for (int i = 0; i < 40; i++) {
      int64_t c = 0;
      conv_db_create(s_user, "C", &c);
      first = first ? first : c;
   }
   char sql[512];
   snprintf(sql, sizeof(sql),
            "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 400) INSERT "
            "INTO memory_consolidation_batches (user_id, conversation_id, path, stop, created_at) "
            "SELECT %d, %lld + i %% 40, 'agent', 'finished', i FROM n; ANALYZE;",
            s_user, (long long)first);
   exec(sql);
   plan_uses("SELECT id FROM memory_consolidation_batches WHERE conversation_id = ?2 AND "
             "+user_id = ?1 AND (?3 = 0 OR id < ?3) ORDER BY id DESC LIMIT ?4",
             "idx_mcb_conv");
   plan_uses("SELECT 1 FROM memory_consolidation_ops l WHERE l.user_id = ?1 AND l.fact_id IN (?4, "
             "?8) AND l.id > ?2",
             "idx_mco_fact");
   plan_uses("SELECT 1 FROM memory_consolidation_ops l WHERE l.user_id = ?1 AND l.other_fact_id IN "
             "(?4, ?8) AND l.id > ?2",
             "idx_mco_other");
   plan_uses("SELECT 1 FROM memory_consolidation_ops l WHERE l.user_id = ?1 AND l.relation_id = ?5 "
             "AND l.id > ?2",
             "idx_mco_rel");
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_undo_added_deletes_the_fact_and_is_idempotent);
   RUN_TEST(test_undo_reinforced_puts_back_what_the_restatement_set);
   RUN_TEST(test_undo_merged_restores_the_duplicate);
   RUN_TEST(test_undo_replaced_restores_the_old_value);
   RUN_TEST(test_undo_whole_batch_reopens_a_closed_relation);
   RUN_TEST(test_undo_linked_splits_the_entities);
   RUN_TEST(test_undo_pref_set_removes_a_new_preference);
   RUN_TEST(test_undo_pref_set_puts_the_old_value_back);
   RUN_TEST(test_undo_pref_set_puts_the_old_source_back);
   RUN_TEST(test_undo_pref_after_forgetting_the_old_values_conversation);
   RUN_TEST(test_undo_created_pref_drops_its_sources);
   RUN_TEST(test_undo_pref_old_pre_image);
   RUN_TEST(test_undo_pref_reinforcement);
   RUN_TEST(test_undo_pref_reinforcement_after_forgetting_one_source);
   RUN_TEST(test_undo_pref_restores_count_and_updated_at);
   RUN_TEST(test_undo_pref_after_deleting_the_old_values_conversation);
   RUN_TEST(test_stale_pref_another_conversation_taught_it);
   RUN_TEST(test_stale_pref_learned_unsourced_since);
   RUN_TEST(test_undo_pref_after_forget_then_delete_last_source);
   RUN_TEST(test_stale_pref_old_source_reteaches_new_value);
   RUN_TEST(test_stale_added_another_conversation_taught_it);
   RUN_TEST(test_stale_added_something_since_merged_into_it);
   RUN_TEST(test_stale_reinforced_made_told_since);
   RUN_TEST(test_stale_merged_restored_since);
   RUN_TEST(test_stale_replaced_replaced_again_since);
   RUN_TEST(test_stale_closed_reopened_since);
   RUN_TEST(test_stale_linked_unlinked_since);
   RUN_TEST(test_stale_pref_set_changed_since);
   RUN_TEST(test_whole_batch_undo_partial_lists_the_stale_ops);
   RUN_TEST(test_an_undo_with_everything_changed_since_is_stale);
   RUN_TEST(test_a_user_can_only_act_on_their_own_batches);
   RUN_TEST(test_an_op_named_with_another_batch_is_refused);
   RUN_TEST(test_keep_stores_a_dropped_candidate_with_the_batch_taint);
   RUN_TEST(test_keep_refuses_what_the_user_forgot);
   RUN_TEST(test_keep_takes_an_expired_deferred_candidate);
   RUN_TEST(test_a_row_taking_up_a_deferred_candidate_is_written_with_its_claim);
   RUN_TEST(test_a_row_quoting_a_private_conversation_is_not_written);
   RUN_TEST(test_list_pages_by_batch_id_and_conversation);
   RUN_TEST(test_target_status_follows_the_counterpart);
   RUN_TEST(test_the_record_caps_text_on_a_character_boundary);
   RUN_TEST(test_prune_removes_old_batches_with_their_ops);
   RUN_TEST(test_the_altered_source_column_removes_rows_with_their_conversation);
   RUN_TEST(test_an_add_a_later_batch_restated_is_stale);
   RUN_TEST(test_a_close_undone_beside_the_newer_value_is_stale);
   RUN_TEST(test_a_preference_a_later_batch_set_again_is_stale);
   RUN_TEST(test_a_merge_restored_and_merged_again_is_stale);
   RUN_TEST(test_a_batch_with_every_op_stale_is_not_undone);
   RUN_TEST(test_undoing_a_merge_takes_back_the_sources_it_copied);
   RUN_TEST(test_a_batch_at_the_cap_undoes_fully);
   RUN_TEST(test_a_keep_its_record_refused_stores_nothing);
   RUN_TEST(test_the_record_says_who_wrote_each_reason);
   RUN_TEST(test_deleting_a_preference_removes_its_record_rows);
   RUN_TEST(test_a_private_conversations_batch_is_not_listed);
   RUN_TEST(test_a_later_restatement_left_stale_still_counts);
   RUN_TEST(test_undoing_a_merge_leaves_the_keepers_taint);
   RUN_TEST(test_a_merge_whose_keeper_a_later_batch_restated_is_stale);
   RUN_TEST(test_a_batch_that_fills_its_record_stops_cleanly);
   RUN_TEST(test_keeping_what_is_already_stored_only_marks_it_kept);
   RUN_TEST(test_an_add_undo_keeps_an_entity_linked_as_an_alias);
   RUN_TEST(test_the_record_queries_use_their_indexes);
   return UNITY_END();
}
