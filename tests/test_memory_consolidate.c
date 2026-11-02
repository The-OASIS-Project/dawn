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
 * Unit tests for the consolidator on a real schema, with the model's replies
 * scripted: triage, the ids the model may name, the repair round, each stop,
 * the loss cap, the forgotten and wording guards, the assistant-subject rule,
 * outside text (a batch or a counterpart that read it retires nothing that
 * didn't), the re-check at apply, the privacy re-check before storing, the
 * record, its forget scrub and its prune.  Every test also checks that each
 * candidate ended with exactly one disposition.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <json-c/json.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "core/embedding_engine.h"
#include "dawn_error.h"
#include "memory/memory_consolidate.h"
#include "memory/memory_db.h"
#include "memory/memory_db_consolidation.h"
#include "memory/memory_db_entities.h"
#include "memory/memory_extraction.h"
#include "memory/memory_forgotten.h"
#include "unity.h"
#include "utils/string_utils.h"

static int s_user;
static int64_t s_conv;
static memory_forgotten_list_t s_forgotten;

/* The scripted model: reply i to call i (NULL: the call fails). */
#define SCRIPT_MAX 6
static const char *s_replies[SCRIPT_MAX];
static int s_output[SCRIPT_MAX];
static int s_calls;
static char s_last_user[16384];         /* the last call's last user message */
static void (*s_before_call)(int call); /* a change made while the model "thinks" */
static bool (*s_allowed)(int user_id, int64_t conversation_id); /* the privacy re-check */

static char *scripted(struct json_object *history,
                      const llm_resolved_config_t *cfg,
                      int *prompt_tokens,
                      int *output_tokens) {
   (void)cfg;
   const size_t n = json_object_array_length(history);
   s_last_user[0] = '\0';
   for (size_t i = n; i > 0; i--) {
      struct json_object *msg = json_object_array_get_idx(history, i - 1);
      struct json_object *role = NULL;
      struct json_object *content = NULL;
      if (json_object_object_get_ex(msg, "role", &role) &&
          strcmp(json_object_get_string(role), "user") == 0 &&
          json_object_object_get_ex(msg, "content", &content)) {
         safe_strscpy(s_last_user, json_object_get_string(content));
         break;
      }
   }
   const int call = s_calls++;
   if (s_before_call) {
      s_before_call(call);
   }
   *prompt_tokens = 1000;
   *output_tokens = call < SCRIPT_MAX ? s_output[call] : 0;
   const char *reply = call < SCRIPT_MAX ? s_replies[call] : NULL;
   return reply ? strdup(reply) : NULL;
}

/* The plan, then the repair (a batch takes one extraction sample: its own). */
static void script(const char *plan, const char *repair) {
   memset(s_replies, 0, sizeof(s_replies));
   memset(s_output, 0, sizeof(s_output));
   s_replies[0] = plan;
   s_replies[1] = repair;
}

/* With a second extraction sample (the benchmark's ablation): it first. */
static void script2(const char *sample2, const char *plan, const char *repair) {
   memset(s_replies, 0, sizeof(s_replies));
   memset(s_output, 0, sizeof(s_output));
   s_replies[0] = sample2;
   s_replies[1] = plan;
   s_replies[2] = repair;
}

void setUp(void) {
   config_set_defaults(&g_config);
   g_config.memory.enabled = true;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   auth_db_create_user("jonathan", "hash", false);
   auth_user_t user;
   memset(&user, 0, sizeof(user));
   auth_db_get_user("jonathan", &user);
   s_user = user.id;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "Chat", &s_conv));
   memset(&s_forgotten, 0, sizeof(s_forgotten));
   s_forgotten.user_id = s_user;
   s_calls = 0;
   s_before_call = NULL;
   s_allowed = NULL;
   memory_consolidate_set_model_fn(scripted);
}

void tearDown(void) {
   memory_forgotten_free(&s_forgotten);
   memory_consolidate_set_model_fn(NULL);
   memory_consolidate_set_samples(1);
   auth_db_shutdown();
}

static void forgot(const char *text) {
   s_forgotten.items = realloc(s_forgotten.items,
                               (size_t)(s_forgotten.count + 1) * sizeof(*s_forgotten.items));
   memory_forgotten_t *f = &s_forgotten.items[s_forgotten.count++];
   memset(f, 0, sizeof(*f));
   safe_strscpy(f->text, text);
}

/* A stored fact, learned in this test's conversation, tagged @p outside. */
static int64_t stored(const char *text, const char *source, int outside) {
   memory_provenance_t prov = { .conv_id = s_conv, .msg_id_start = 1, .msg_id_end = 1 };
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_fact_create(s_user, text, 0.7f, source, &prov, &id));
   if (outside) {
      char sql[96];
      snprintf(sql, sizeof(sql), "UPDATE memory_facts SET outside = %d WHERE id = ?1", outside);
      sqlite3_stmt *st = NULL;
      sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL);
      sqlite3_bind_int64(st, 1, id);
      sqlite3_step(st);
      sqlite3_finalize(st);
   }
   return id;
}

/* A batch of this test's conversation; its return code checked against @p rc. */
static memory_consolidate_result_t run_rc(const char *round0,
                                          int outside,
                                          int input_tokens,
                                          int rc) {
   struct json_object *history = json_object_new_array();
   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "role", json_object_new_string("user"));
   json_object_object_add(msg, "content", json_object_new_string("Extract what to remember."));
   json_object_array_add(history, msg);
   /* A cloud model: its window comes from the registry, with no local probe. */
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
                                           .forgotten = &s_forgotten,
                                           .cfg = &cfg,
                                           .history = history,
                                           .reply = round0,
                                           .input_tokens = input_tokens,
                                           .allowed = s_allowed };
   memory_consolidate_result_t res;
   memset(&res, 0, sizeof(res));
   TEST_ASSERT_EQUAL_INT(rc, memory_consolidate_batch(&in, &res));
   json_object_put(history);
   return res;
}

static memory_consolidate_result_t run(const char *round0, int outside, int input_tokens) {
   return run_rc(round0, outside, input_tokens, SUCCESS);
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

/* The current fact with this text (0: none). */
static int64_t current(const char *text) {
   const int64_t id = q("SELECT id FROM memory_facts WHERE user_id = ?1 AND fact_text = ?2 AND "
                        "superseded_by IS NULL",
                        s_user, text);
   return id < 0 ? 0 : id;
}

static int64_t superseded_by(int64_t fact_id) {
   return q("SELECT COALESCE(superseded_by, 0) FROM memory_facts WHERE id = ?1", fact_id, NULL);
}

static int64_t outside_of(int64_t fact_id) {
   return q("SELECT outside FROM memory_facts WHERE id = ?1", fact_id, NULL);
}

/* How many of the batch's rows are op @p op with status @p status. */
static int rows(int64_t batch, const char *op, const char *status) {
   memory_consolidation_op_row_t r[64];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_consolidation_ops_list(s_user, batch, r, 64, &n));
   int k = 0;
   for (int i = 0; i < n; i++) {
      k += strcmp(r[i].op, op) == 0 && (!status || strcmp(r[i].status, status) == 0);
   }
   memory_db_consolidation_ops_free(r, n);
   return k;
}

/* Every candidate ended with one disposition: an applied added, reinforced,
 * dropped or deferred row each. */
static void one_disposition_each(int64_t batch, int candidates) {
   const int n = rows(batch, "added", "applied") + rows(batch, "reinforced", "applied") +
                 rows(batch, "dropped", "applied") + rows(batch, "deferred", "applied");
   TEST_ASSERT_EQUAL_INT(candidates, n);
}

#define ONE_FACT(text) "{\"facts\": [{\"text\": \"" text "\", \"subject\": \"Jonathan Smith\"}]}"
#define TWO_FACTS(a, b)                                                    \
   "{\"facts\": [{\"text\": \"" a "\", \"subject\": \"Jonathan Smith\"}, " \
   "{\"text\": \"" b "\", \"subject\": \"Jonathan Smith\"}]}"
#define NONE "{\"facts\": []}"

/* ---- triage ---- */

void test_nothing_similar_is_added_without_a_consolidation_round(void) {
   script(NULL, NULL);
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith plays the cello"), 0, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_TRIAGE, res.stop);
   TEST_ASSERT_EQUAL_INT(0, s_calls); /* the extraction's own reply only */
   TEST_ASSERT_EQUAL_INT(0, res.rounds);
   TEST_ASSERT_TRUE(current("Jonathan Smith plays the cello") > 0);
   TEST_ASSERT_EQUAL_INT(1, rows(res.batch_id, "added", "applied"));
   one_disposition_each(res.batch_id, 1);
}

/* With a second sample, its candidates join the first's: here a forgotten one. */
void test_same_text_is_reinforced_and_forgotten_text_dropped(void) {
   const int64_t cello = stored("Jonathan Smith plays the cello", "inferred", 0);
   forgot("Jonathan Smith collects stamps");
   memory_consolidate_set_samples(2);
   script2(ONE_FACT("Jonathan Smith collects stamps."), NULL, NULL);
   const memory_consolidate_result_t res = run(ONE_FACT("jonathan smith plays the cello"), 0, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_TRIAGE, res.stop);
   TEST_ASSERT_EQUAL_INT(1, rows(res.batch_id, "reinforced", "applied"));
   TEST_ASSERT_EQUAL_INT(1, rows(res.batch_id, "dropped", "applied"));
   TEST_ASSERT_EQUAL_INT(0, current("Jonathan Smith collects stamps."));
   TEST_ASSERT_EQUAL_INT(0, superseded_by(cello));
   one_disposition_each(res.batch_id, 2);
}

/* ---- ids and the repair round ---- */

void test_an_unshown_id_is_named_in_the_repair_and_the_fix_applies(void) {
   const int64_t old = stored("Jonathan Smith lives in Portland", "explicit", 0);
   script("{\"ops\": [{\"op\": \"replace\", \"old\": \"S9\", \"by\": \"C1\"}], \"finish\": true}",
          "{\"ops\": [{\"op\": \"replace\", \"old\": \"S1\", \"by\": \"C1\"}], \"finish\": true}");
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith lives in Springfield"), 0,
                                               0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_FINISHED, res.stop);
   TEST_ASSERT_FALSE(res.partial);
   TEST_ASSERT_EQUAL_INT(2, s_calls);
   TEST_ASSERT_NOT_NULL(strstr(s_last_user, "op 1 (replace ?)"));
   TEST_ASSERT_NOT_NULL(strstr(s_last_user, "not shown"));
   const int64_t now = current("Jonathan Smith lives in Springfield");
   TEST_ASSERT_TRUE(now > 0);
   TEST_ASSERT_EQUAL_INT64(now, superseded_by(old));
   TEST_ASSERT_EQUAL_INT(1, rows(res.batch_id, "replaced", "applied"));
   one_disposition_each(res.batch_id, 1);
}

void test_a_failed_repair_applies_only_additions(void) {
   const int64_t old = stored("Jonathan Smith lives in Portland", "explicit", 0);
   script("{\"ops\": [{\"op\": \"replace\", \"old\": \"S9\", \"by\": \"C1\"}]}", "not json at all");
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith lives in Springfield"), 0,
                                               0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_REPAIR_FAILED, res.stop);
   TEST_ASSERT_TRUE(res.partial);
   TEST_ASSERT_EQUAL_INT(0, superseded_by(old));
   TEST_ASSERT_TRUE(current("Jonathan Smith lives in Springfield") > 0);
   memory_consolidation_batch_row_t b[1];
   int n = 0;
   memory_db_consolidation_batches_list(s_user, s_conv, 0, b, 1, &n);
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_STRING("agent_partial", b[0].path);
   TEST_ASSERT_EQUAL_STRING("repair_failed", b[0].stop);
   one_disposition_each(res.batch_id, 1);
}

/* ---- stops ---- */

void test_should_stop_order(void) {
   memory_consolidate_progress_t p = { 0 };
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_GO_ON, memory_consolidate_should_stop(&p));
   p.input_tokens = MEMORY_CONSOLIDATE_INPUT_BUDGET;
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_TOKENS, memory_consolidate_should_stop(&p));
   p.input_tokens = 0;
   p.next_request = MEMORY_CONSOLIDATE_REQUEST_MAX + 1;
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_TOKENS, memory_consolidate_should_stop(&p));
   p.next_request = 0;
   p.max_output = MEMORY_CONSOLIDATE_OUTPUT_BUDGET + 1;
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_TOKENS, memory_consolidate_should_stop(&p));
   p.cancelled = true;
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_CANCELLED, memory_consolidate_should_stop(&p));
   p.repaired = true;
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_REPAIR_FAILED, memory_consolidate_should_stop(&p));
   p.complete = true;
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_FINISHED, memory_consolidate_should_stop(&p));
   TEST_ASSERT_EQUAL_STRING("repair_failed",
                            memory_consolidate_stop_name(MEMORY_CONSOLIDATE_REPAIR_FAILED));
}

void test_samples_over_the_old_budget_still_get_their_round(void) {
   const int64_t old = stored("Jonathan Smith lives in Portland", "explicit", 0);
   script("{\"ops\": [{\"op\": \"replace\", \"old\": \"S1\", \"by\": \"C1\"}]}", NULL);
   /* The extraction alone read more than the rounds' budget. */
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith lives in Springfield"), 0,
                                               MEMORY_CONSOLIDATE_INPUT_BUDGET + 10000);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_FINISHED, res.stop);
   TEST_ASSERT_EQUAL_INT(1, s_calls);
   TEST_ASSERT_EQUAL_INT64(current("Jonathan Smith lives in Springfield"), superseded_by(old));
   one_disposition_each(res.batch_id, 1);
}

void test_a_request_over_the_ceiling_gets_no_round(void) {
   const int64_t old = stored("Jonathan Smith lives in Portland", "explicit", 0);
   script("{\"ops\": [{\"op\": \"replace\", \"old\": \"S1\", \"by\": \"C1\"}]}", NULL);
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith lives in Springfield"), 0,
                                               MEMORY_CONSOLIDATE_REQUEST_MAX);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_TOKENS, res.stop);
   TEST_ASSERT_EQUAL_INT(0, s_calls);
   TEST_ASSERT_EQUAL_INT(0, superseded_by(old));
   TEST_ASSERT_TRUE(current("Jonathan Smith lives in Springfield") > 0);
   one_disposition_each(res.batch_id, 1);
}

void test_an_incomplete_plan_over_the_output_budget_stops(void) {
   stored("Jonathan Smith lives in Portland", "explicit", 0);
   script("{\"ops\": [{\"op\": \"drop\", \"c\": \"C7\"}]}", "{\"ops\": []}");
   s_output[0] = MEMORY_CONSOLIDATE_OUTPUT_BUDGET + 1;
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith lives in Springfield"), 0,
                                               0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_TOKENS, res.stop);
   TEST_ASSERT_EQUAL_INT(1, s_calls);
   one_disposition_each(res.batch_id, 1);
}

/* ---- the loss cap ---- */

void test_loss_cap_over_three_neighbours(void) {
   const int64_t a = stored("Jonathan Smith works at Acme", "explicit", 0);
   const int64_t b = stored("Jonathan Smith works at Acme Corp", "explicit", 0);
   const int64_t c = stored("Jonathan Smith works at Acme in sales", "explicit", 0);
   script("{\"ops\": [{\"op\": \"merge\", \"from\": \"S2\", \"into\": \"S1\"}, "
          "{\"op\": \"merge\", \"from\": \"S3\", \"into\": \"S1\"}, "
          "{\"op\": \"replace\", \"old\": \"S1\", \"by\": \"C1\"}], \"finish\": true}",
          NULL);
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith works at Initech"), 0, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_FINISHED, res.stop);
   TEST_ASSERT_TRUE(res.partial); /* 3 destructive over max(2, 25% of 3) */
   TEST_ASSERT_EQUAL_INT(0, superseded_by(a));
   TEST_ASSERT_EQUAL_INT(0, superseded_by(b));
   TEST_ASSERT_EQUAL_INT(0, superseded_by(c));
   TEST_ASSERT_EQUAL_INT(3, res.refused);
   TEST_ASSERT_TRUE(current("Jonathan Smith works at Initech") > 0);
   one_disposition_each(res.batch_id, 1);
}

/* ---- forgotten facts ---- */

void test_near_a_forgotten_fact_is_deferred_never_added_on_a_partial_run(void) {
   forgot("Jonathan Smith collects stamps");
   script("nope", "still nope");
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith collects coins"), 0, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_REPAIR_FAILED, res.stop);
   TEST_ASSERT_EQUAL_INT(0, current("Jonathan Smith collects coins"));
   TEST_ASSERT_EQUAL_INT(1, rows(res.batch_id, "deferred", "applied"));
   TEST_ASSERT_NOT_NULL(strstr(s_last_user, "C1 has no disposition"));
   one_disposition_each(res.batch_id, 1);
}

void test_a_shortened_text_that_is_forgotten_is_refused(void) {
   forgot("Jonathan Smith collects stamps");
   script("{\"ops\": [{\"op\": \"add\", \"c\": \"C1\", \"text\": \"Jonathan Smith collects "
          "stamps\"}]}",
          "{\"ops\": [{\"op\": \"drop\", \"c\": \"C1\", \"reason\": \"forgotten\"}]}");
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith collects stamps and coins"),
                                               0, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_FINISHED, res.stop);
   TEST_ASSERT_NOT_NULL(strstr(s_last_user, "forget"));
   TEST_ASSERT_EQUAL_INT(0, current("Jonathan Smith collects stamps"));
   one_disposition_each(res.batch_id, 1);
}

/* ---- wording ---- */

void test_a_shortened_text_may_only_delete_words(void) {
   stored("Jonathan Smith likes music", "explicit", 0);
   script("{\"ops\": [{\"op\": \"add\", \"c\": \"C1\", \"text\": \"Jonathan Smith does not like "
          "loud music\"}], \"finish\": true}",
          NULL);
   const memory_consolidate_result_t res = run(
       ONE_FACT("Jonathan Smith does not like loud music at night."), 0, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_FINISHED, res.stop);
   TEST_ASSERT_TRUE(current("Jonathan Smith does not like loud music") > 0);
   one_disposition_each(res.batch_id, 1);
}

void test_a_new_word_or_a_dropped_not_is_refused(void) {
   stored("Jonathan Smith likes music", "explicit", 0);
   script("{\"ops\": [{\"op\": \"add\", \"c\": \"C1\", \"text\": \"Jonathan Smith likes jazz "
          "music\"}]}",
          "{\"ops\": [{\"op\": \"add\", \"c\": \"C1\", \"text\": \"Jonathan Smith does like loud "
          "music\"}]}");
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith does not like loud music"),
                                               0, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_REPAIR_FAILED, res.stop);
   TEST_ASSERT_EQUAL_INT(0, current("Jonathan Smith likes jazz music"));
   TEST_ASSERT_EQUAL_INT(0, current("Jonathan Smith does like loud music"));
   TEST_ASSERT_TRUE(current("Jonathan Smith does not like loud music") > 0);
   one_disposition_each(res.batch_id, 1);
}

/* ---- the assistant as subject ---- */

void test_the_assistant_is_no_subject_but_dawn_is(void) {
   script(NULL, NULL);
   const memory_consolidate_result_t res = run(
       "{\"facts\": [{\"text\": \"Friday owns a red car\", \"subject\": \"Friday\"}, "
       "{\"text\": \"DAWN is written in C\", \"subject\": \"DAWN\", \"relations\": "
       "[{\"subject\": \"friday\", \"predicate\": \"owns\", \"object\": \"DAWN\"}]}]}",
       0, 0);
   TEST_ASSERT_EQUAL_INT(0, current("Friday owns a red car"));
   TEST_ASSERT_TRUE(current("DAWN is written in C") > 0);
   TEST_ASSERT_EQUAL_INT(0, q("SELECT COUNT(*) FROM memory_relations WHERE user_id = ?1", s_user,
                              NULL));
   TEST_ASSERT_EQUAL_INT(1, rows(res.batch_id, "dropped", "applied")); /* recorded, not lost */
   one_disposition_each(res.batch_id, 2);
}

/* ---- outside text ---- */

void test_a_tainted_batch_replaces_neither_a_clean_nor_an_unknown_fact(void) {
   const int64_t clean = stored("Jonathan Smith lives in Portland", "explicit", 0);
   const int64_t unknown = stored("Jonathan Smith lives near Portland", "explicit",
                                  OUTSIDE_UNKNOWN);
   script("{\"ops\": [{\"op\": \"replace\", \"old\": \"S1\", \"by\": \"C1\"}, "
          "{\"op\": \"replace\", \"old\": \"S2\", \"by\": \"C1\"}], \"finish\": true}",
          NULL);
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith lives in Springfield"),
                                               OUTSIDE_EMAIL, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_FINISHED, res.stop);
   TEST_ASSERT_EQUAL_INT(0, superseded_by(clean));
   TEST_ASSERT_EQUAL_INT(0, superseded_by(unknown));
   const int64_t beside = current("Jonathan Smith lives in Springfield");
   TEST_ASSERT_TRUE(beside > 0);
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_EMAIL, outside_of(beside));
   TEST_ASSERT_EQUAL_INT(2, rows(res.batch_id, "replaced", "refused"));
   one_disposition_each(res.batch_id, 1);
}

void test_a_tainted_fact_replaces_no_clean_one_in_a_clean_batch(void) {
   const int64_t clean = stored("Jonathan Smith lives in Portland", "explicit", 0);
   const int64_t tainted = stored("Jonathan Smith lives in Springfield", "inferred", OUTSIDE_WEB);
   script("{\"ops\": [{\"op\": \"replace\", \"old\": \"S1\", \"by\": \"S2\"}, "
          "{\"op\": \"add\", \"c\": \"C1\"}], \"finish\": true}",
          NULL);
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith lives somewhere"), 0, 0);
   TEST_ASSERT_EQUAL_INT(0, superseded_by(clean));
   TEST_ASSERT_EQUAL_INT(0, superseded_by(tainted));
   TEST_ASSERT_EQUAL_INT(1, rows(res.batch_id, "replaced", "refused"));
   one_disposition_each(res.batch_id, 1);
}

void test_a_tainted_batch_links_nothing_it_only_proposes(void) {
   bool created = false;
   int64_t jon = 0;
   int64_t jonathan = 0;
   memory_db_entity_upsert(s_user, "Jon Smith", "person", "jon smith", &created, &jon);
   memory_db_entity_upsert(s_user, "Jonathan Smith", "person", "jonathan smith", &created,
                           &jonathan);
   stored("Jon Smith plays chess", "explicit", 0);
   script("{\"ops\": [{\"op\": \"add\", \"c\": \"C1\"}, {\"op\": \"link\", \"e\": \"E1\", "
          "\"same_as\": \"E2\"}], \"finish\": true}",
          NULL);
   const memory_consolidate_result_t res = run(
       "{\"facts\": [{\"text\": \"Jon Smith plays chess with Jonathan Smith\", \"subject\": "
       "\"Jon Smith\", \"relations\": [{\"subject\": \"Jon Smith\", \"predicate\": "
       "\"plays_chess_with\", \"object\": \"Jonathan Smith\"}]}]}",
       OUTSIDE_EMAIL, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_FINISHED, res.stop);
   TEST_ASSERT_EQUAL_INT(1, rows(res.batch_id, "proposed", "applied"));
   TEST_ASSERT_EQUAL_INT(0, rows(res.batch_id, "linked", NULL));
   TEST_ASSERT_EQUAL_INT(0, q("SELECT COUNT(*) FROM memory_entities WHERE user_id = ?1 AND "
                              "canonical_id IS NOT NULL",
                              s_user, NULL));
   one_disposition_each(res.batch_id, 1);
}

void test_a_tainted_restatement_reinforces_without_making_it_told(void) {
   const int64_t cello = stored("Jonathan Smith plays the cello", "inferred", 0);
   script("{\"ops\": [{\"op\": \"merge\", \"c\": \"C1\", \"into\": \"S1\"}]}", NULL);
   const memory_consolidate_result_t res = run(
       "{\"facts\": [{\"text\": \"Jonathan Smith plays cello well\", \"subject\": \"Jonathan "
       "Smith\", \"source\": \"explicit\"}], \"preferences\": [{\"category\": \"music\", "
       "\"value\": \"rock\"}]}",
       OUTSIDE_EMAIL, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_FINISHED, res.stop);
   memory_fact_t f;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_get(cello, s_user, &f));
   TEST_ASSERT_EQUAL_STRING("inferred", f.source);
   TEST_ASSERT_TRUE(f.confidence > 0.75f);
   TEST_ASSERT_EQUAL_INT(0, current("Jonathan Smith plays cello well"));
   one_disposition_each(res.batch_id, 1);
}

void test_a_tainted_preference_does_not_replace_a_clean_one(void) {
   memory_db_pref_upsert(s_user, "music", "jazz", 0.8f, "explicit", NULL);
   script(NULL, NULL);
   const memory_consolidate_result_t res = run(
       "{\"facts\": [], \"preferences\": [{\"category\": \"music\", \"value\": \"rock\"}]}",
       OUTSIDE_WEB, 0);
   memory_preference_t p;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_pref_get(s_user, "music", &p));
   TEST_ASSERT_EQUAL_STRING("jazz", p.value);
   TEST_ASSERT_EQUAL_INT(0, rows(res.batch_id, "pref_set", NULL));
}

/* The S number @p text was shown as in the last message (0: not shown). */
static int shown_as(const char *text) {
   for (int i = 1; i < 10; i++) {
      char line[600];
      snprintf(line, sizeof(line), "\nS%d [", i);
      const char *at = strstr(s_last_user, line);
      const char *end = at ? strchr(at + 1, '\n') : NULL;
      if (at && end) {
         const char *t = strstr(at, text);
         if (t && t < end && t[strlen(text)] == '\n') {
            return i;
         }
      }
   }
   return 0;
}

static char s_plan[512];
static void merge_copy_into_keeper(int call) {
   if (call == 0) {
      const int copy = shown_as("Jonathan Smith lives in Springfield");
      const int keeper = shown_as("Jonathan Smith lives in Springfield, Oregon");
      snprintf(s_plan, sizeof(s_plan),
               "{\"ops\": [{\"op\": \"merge\", \"from\": \"S%d\", \"into\": \"S%d\"}, "
               "{\"op\": \"merge\", \"c\": \"C1\", \"into\": \"S%d\"}], \"finish\": true}",
               copy, keeper, keeper);
      s_replies[0] = s_plan;
   }
}

/* A stored copy merged into its keeper, the two tagged as given, in a batch
 * tagged @p batch: applied, or refused (and both kept). */
static void merge_case(int keeper_outside, int copy_outside, int batch, bool applies) {
   const int64_t keeper = stored("Jonathan Smith lives in Springfield, Oregon", "explicit",
                                 keeper_outside);
   const int64_t copy = stored("Jonathan Smith lives in Springfield", "inferred", copy_outside);
   script(NULL, NULL);
   s_before_call = merge_copy_into_keeper;
   const memory_consolidate_result_t res = run(
       ONE_FACT("Jonathan Smith lives in Springfield Oregon"), batch, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_FINISHED, res.stop);
   TEST_ASSERT_EQUAL_INT64(applies ? keeper : 0, superseded_by(copy));
   TEST_ASSERT_EQUAL_INT(0, superseded_by(keeper));
   TEST_ASSERT_EQUAL_INT64(keeper_outside, outside_of(keeper)); /* a merge retags nothing */
   TEST_ASSERT_EQUAL_INT(applies ? 1 : 0, rows(res.batch_id, "merged", "applied"));
   TEST_ASSERT_EQUAL_INT(applies ? 0 : 1, rows(res.batch_id, "merged", "refused"));
   one_disposition_each(res.batch_id, 1);
}

void test_a_tainted_batch_merges_two_clean_facts(void) {
   merge_case(0, OUTSIDE_UNKNOWN, OUTSIDE_EMAIL, true);
}

void test_two_tainted_facts_merge(void) {
   merge_case(OUTSIDE_WEB, OUTSIDE_EMAIL, OUTSIDE_EMAIL, true);
}

void test_a_clean_and_a_tainted_fact_dont_merge(void) {
   merge_case(0, OUTSIDE_WEB, 0, false);
}

/* ---- what of the forgotten facts the model sees ---- */

void test_only_forgotten_facts_near_a_candidate_are_shown(void) {
   TEST_ASSERT_EQUAL_INT(0, embedding_engine_init());
   forgot("Jonathan Smith collects stamps");
   forgot("The garage door code is blue seven");
   script("{\"ops\": [{\"op\": \"add\", \"c\": \"C1\"}]}", NULL);
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith collects coins"), 0, 0);
   embedding_engine_cleanup();
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_FINISHED, res.stop);
   TEST_ASSERT_NOT_NULL(strstr(s_last_user, "F1 Jonathan Smith collects stamps"));
   TEST_ASSERT_NULL(strstr(s_last_user, "garage door"));
   one_disposition_each(res.batch_id, 1);
}

void test_a_re_added_deferred_candidate_keeps_its_taint(void) {
   const memory_consolidation_batch_t prior = { .user_id = s_user,
                                                .conversation_id = s_conv,
                                                .path = "agent",
                                                .stop = "finished",
                                                .outside = OUTSIDE_EMAIL };
   int64_t prior_id = 0;
   memory_db_consolidation_batch_add(&prior, &prior_id);
   const memory_consolidation_op_t d = {
      .batch_id = prior_id,
      .user_id = s_user,
      .seq = 1,
      .op = "deferred",
      .outside = OUTSIDE_EMAIL,
      .text = "Jonathan Smith visits Lisbon in May",
      .pre_image = "{\"text\": \"Jonathan Smith visits Lisbon in May\", \"subject\": "
                   "\"Jonathan Smith\"}"
   };
   int64_t d_id = 0;
   memory_db_consolidation_op_add(&d, &d_id);
   script("{\"ops\": [{\"op\": \"add\", \"c\": \"D1\"}], \"finish\": true}", NULL);
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith plays the cello"), 0, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_FINISHED, res.stop);
   const int64_t lisbon = current("Jonathan Smith visits Lisbon in May");
   TEST_ASSERT_TRUE(lisbon > 0);
   TEST_ASSERT_EQUAL_INT64(OUTSIDE_EMAIL, outside_of(lisbon));
   TEST_ASSERT_EQUAL_INT(1, rows(prior_id, "deferred", "consumed"));
   one_disposition_each(res.batch_id, 2); /* the C (triaged) and the D */
}

/* ---- apply ---- */

static int64_t s_moved_old;
static int64_t s_moved_to;
static void move_old_during_repair(int call) {
   if (call == 1) {
      memory_db_fact_merge(s_moved_old, s_moved_to, s_user);
   }
}

void test_apply_rechecks_a_fact_superseded_since_the_plan(void) {
   s_moved_old = stored("Jonathan Smith lives in Portland", "explicit", 0);
   s_moved_to = stored("Jonathan Smith's home is far away", "explicit", 0);
   s_before_call = move_old_during_repair;
   script("{\"ops\": [{\"op\": \"replace\", \"old\": \"S1\", \"by\": \"C1\"}, {\"op\": \"drop\", "
          "\"c\": \"C9\"}]}",
          "{\"ops\": [{\"op\": \"replace\", \"old\": \"S1\", \"by\": \"C1\"}], \"finish\": true}");
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith lives in Springfield"), 0,
                                               0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_FINISHED, res.stop);
   TEST_ASSERT_EQUAL_INT64(s_moved_to, superseded_by(s_moved_old)); /* not the new value */
   TEST_ASSERT_EQUAL_INT(1, rows(res.batch_id, "replaced", "refused"));
   TEST_ASSERT_TRUE(current("Jonathan Smith lives in Springfield") > 0);
   one_disposition_each(res.batch_id, 1);
}

void test_merging_a_stored_fact_into_a_fuller_candidate_keeps_the_fuller_text(void) {
   const int64_t short_one = stored("Jonathan Smith works at Acme", "explicit", 0);
   script("{\"ops\": [{\"op\": \"merge\", \"from\": \"S1\", \"into\": \"C1\"}]}", NULL);
   const memory_consolidate_result_t res = run(
       ONE_FACT("Jonathan Smith works at Acme as a senior engineer"), 0, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_FINISHED, res.stop);
   const int64_t full = current("Jonathan Smith works at Acme as a senior engineer");
   TEST_ASSERT_TRUE(full > 0);
   TEST_ASSERT_EQUAL_INT64(full, superseded_by(short_one));
   one_disposition_each(res.batch_id, 1);
}

void test_close_relation_ends_an_open_value(void) {
   bool created = false;
   int64_t jon = 0;
   memory_db_entity_upsert(s_user, "Jonathan Smith", "person", "jonathan smith", &created, &jon);
   const int64_t old = stored("Jonathan Smith's favorite color is blue", "explicit", 0);
   memory_db_relation_create(s_user, jon, "favorite_color", 0, "blue", old, 0.9f, 0, 0, NULL);
   script("{\"ops\": [{\"op\": \"add\", \"c\": \"C1\"}, {\"op\": \"close_relation\", \"r\": "
          "\"R1\", \"valid_to\": \"2026-10-01\", \"by\": \"C1\"}], \"finish\": true}",
          NULL);
   const memory_consolidate_result_t res = run(
       "{\"facts\": [{\"text\": \"Jonathan Smith's favorite color is green\", \"subject\": "
       "\"Jonathan Smith\", \"relations\": [{\"subject\": \"Jonathan Smith\", \"predicate\": "
       "\"favorite_color\", \"object\": \"green\"}]}]}",
       0, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_FINISHED, res.stop);
   TEST_ASSERT_EQUAL_INT(1, rows(res.batch_id, "closed", "applied"));
   TEST_ASSERT_EQUAL_INT(1, q("SELECT COUNT(*) FROM memory_relations WHERE user_id = ?1 AND "
                              "object_value = 'blue' AND valid_to IS NOT NULL",
                              s_user, NULL));
   TEST_ASSERT_EQUAL_INT(1, q("SELECT COUNT(*) FROM memory_relations WHERE user_id = ?1 AND "
                              "object_value = 'green' AND valid_to IS NULL",
                              s_user, NULL));
   one_disposition_each(res.batch_id, 1);
}

/* ---- the record and forgetting ---- */

void test_forgetting_a_fact_scrubs_its_batch_record(void) {
   script("{\"ops\": [{\"op\": \"add\", \"c\": \"C1\"}, {\"op\": \"drop\", \"c\": \"C2\"}], "
          "\"finish\": true}",
          NULL);
   const memory_consolidate_result_t res = run(
       TWO_FACTS("Jonathan Smith plays the cello", "Jonathan Smith played the cello today"), 0, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_FINISHED, res.stop);
   one_disposition_each(res.batch_id, 2);
   const int64_t cello = current("Jonathan Smith plays the cello");
   TEST_ASSERT_TRUE(cello > 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_forget(cello, s_user, NULL));
   TEST_ASSERT_EQUAL_INT(0, rows(res.batch_id, "added", NULL));
   TEST_ASSERT_EQUAL_INT(0, rows(res.batch_id, "dropped", NULL));
}

void test_a_conversation_made_private_loses_its_batches(void) {
   script(NULL, NULL);
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith plays the cello"), 0, 0);
   TEST_ASSERT_TRUE(res.batch_id > 0);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_set_private(s_conv, s_user, true));
   TEST_ASSERT_EQUAL_INT(0, q("SELECT COUNT(*) FROM memory_consolidation_batches WHERE user_id = "
                              "?1",
                              s_user, NULL));
   TEST_ASSERT_EQUAL_INT(0, q("SELECT COUNT(*) FROM memory_consolidation_ops WHERE user_id = ?1",
                              s_user, NULL));
}

/* ---- review cases ---- */

/* A candidate an earlier batch set aside, in batch @p conv tagged @p outside;
 * the deferred row's text is @p text. */
static int64_t set_aside(int64_t conv, int outside, const char *text, const char *json) {
   const memory_consolidation_batch_t prior = { .user_id = s_user,
                                                .conversation_id = conv,
                                                .path = "agent",
                                                .stop = "finished",
                                                .outside = outside };
   int64_t prior_id = 0;
   memory_db_consolidation_batch_add(&prior, &prior_id);
   const memory_consolidation_op_t d = { .batch_id = prior_id,
                                         .user_id = s_user,
                                         .seq = 1,
                                         .op = "deferred",
                                         .outside = outside,
                                         .text = text,
                                         .pre_image = json };
   int64_t d_id = 0;
   memory_db_consolidation_op_add(&d, &d_id);
   return prior_id;
}

#define LISBON "Jonathan Smith visits Lisbon in May"

/* A candidate set aside from an email, said again in a clean batch: the stored
 * fact it restates is reinforced but not made told (the D keeps its taint). */
void test_a_tainted_deferred_restatement_makes_nothing_told(void) {
   const int64_t f = stored(LISBON, "inferred", 0);
   set_aside(s_conv, OUTSIDE_EMAIL, LISBON,
             "{\"text\": \"" LISBON "\", \"subject\": \"Jonathan Smith\", \"source\": "
             "\"explicit\"}");
   script("{\"ops\": [{\"op\": \"add\", \"c\": \"C1\"}, {\"op\": \"add\", \"c\": \"D1\"}], "
          "\"finish\": true}",
          NULL);
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith plays the cello"), 0, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_FINISHED, res.stop);
   TEST_ASSERT_EQUAL_INT(0,
                         q("SELECT source = 'explicit' FROM memory_facts WHERE id = ?1", f, NULL));
}

/* A candidate conversation B set aside, kept by a batch of conversation A, is
 * learned from B: forgetting B's memories reaches it. */
void test_a_kept_deferred_candidate_is_learned_from_its_own_conversation(void) {
   int64_t conv_b = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "B", &conv_b));
   set_aside(conv_b, 0, LISBON, "{\"text\": \"" LISBON "\", \"subject\": \"Jonathan Smith\"}");
   script("{\"ops\": [{\"op\": \"add\", \"c\": \"D1\"}], \"finish\": true}", NULL);
   run(ONE_FACT("Jonathan Smith plays the cello"), 0, 0);
   const int64_t lisbon = current(LISBON);
   TEST_ASSERT_TRUE(lisbon > 0);
   TEST_ASSERT_EQUAL_INT64(conv_b, q("SELECT COALESCE(source_conversation_id, 0) FROM "
                                     "memory_facts WHERE id = ?1",
                                     lisbon, NULL));
}

/* C1 into C2 and C2 into C1: the second merge is rejected (a repair is asked
 * for, none comes), and one of the two is stored, never neither. */
void test_a_merge_cycle_keeps_one(void) {
   script("{\"ops\": [{\"op\": \"merge\", \"c\": \"C1\", \"into\": \"C2\"}, {\"op\": \"merge\", "
          "\"c\": \"C2\", \"into\": \"C1\"}], \"finish\": true}",
          NULL);
   const memory_consolidate_result_t res = run(
       TWO_FACTS("Jonathan Smith plays the cello", "Jonathan Smith plays cello"), 0, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_REPAIR_FAILED, res.stop);
   TEST_ASSERT_EQUAL_INT(1, res.rejected);
   TEST_ASSERT_TRUE((current("Jonathan Smith plays the cello") > 0) !=
                    (current("Jonathan Smith plays cello") > 0));
   one_disposition_each(res.batch_id, 2);
}

/* A candidate near a forgotten fact that a failed repair said to add: only
 * additions apply on such a run, and this one isn't one (set aside). */
void test_a_failed_repair_adds_nothing_near_a_forgotten_fact(void) {
   forgot("Jonathan Smith collects stamps");
   script("{\"ops\": [{\"op\": \"drop\", \"c\": \"C9\"}]}",
          "{\"ops\": [{\"op\": \"add\", \"c\": \"C1\"}, {\"op\": \"drop\", \"c\": \"C8\"}]}");
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith collects coins"), 0, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_REPAIR_FAILED, res.stop);
   TEST_ASSERT_EQUAL_INT(0, current("Jonathan Smith collects coins"));
   TEST_ASSERT_EQUAL_INT(1, rows(res.batch_id, "deferred", "applied"));
   one_disposition_each(res.batch_id, 1);
}

/* A repair that restates the plan's valid ops beside its fix finishes, and
 * each op applies once. */
void test_a_repair_restating_the_plan_finishes(void) {
   script("{\"ops\": [{\"op\": \"drop\", \"c\": \"C1\"}, {\"op\": \"add\", \"c\": \"C2\"}, "
          "{\"op\": \"drop\", \"c\": \"C9\"}]}",
          "{\"ops\": [{\"op\": \"drop\", \"c\": \"C1\"}, {\"op\": \"add\", \"c\": \"C2\"}], "
          "\"finish\": true}");
   const memory_consolidate_result_t res = run(
       TWO_FACTS("Jonathan Smith plays the cello", "Jonathan Smith played the cello today"), 0, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_FINISHED, res.stop);
   TEST_ASSERT_FALSE(res.partial);
   TEST_ASSERT_EQUAL_INT(1, rows(res.batch_id, "dropped", "applied"));
   TEST_ASSERT_TRUE(current("Jonathan Smith played the cello today") > 0);
   one_disposition_each(res.batch_id, 2);
}

/* A repair with no ops fixes nothing: the run is partial. */
void test_an_empty_repair_fails(void) {
   const int64_t old = stored("Jonathan Smith lives in Portland", "explicit", 0);
   script("{\"ops\": [{\"op\": \"replace\", \"old\": \"S9\", \"by\": \"C1\"}]}",
          "{\"ops\": [], \"finish\": true}");
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith lives in Springfield"), 0,
                                               0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_REPAIR_FAILED, res.stop);
   TEST_ASSERT_TRUE(res.partial);
   TEST_ASSERT_EQUAL_INT(0, superseded_by(old));
   one_disposition_each(res.batch_id, 1);
}

/* The same when the rejected op gave no disposition (every C has one). */
void test_an_empty_repair_fails_when_only_a_non_disposition_was_rejected(void) {
   stored("Jonathan Smith lives in Portland", "explicit", 0);
   script("{\"ops\": [{\"op\": \"add\", \"c\": \"C1\"}, {\"op\": \"close_relation\", \"r\": "
          "\"R9\"}]}",
          "{\"ops\": [], \"finish\": true}");
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith lives in Springfield"), 0,
                                               0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_REPAIR_FAILED, res.stop);
   TEST_ASSERT_TRUE(current("Jonathan Smith lives in Springfield") > 0);
   one_disposition_each(res.batch_id, 1);
}

/* Forgetting a fact removes another conversation's set-aside rows with its
 * exact words; one in other words stays (it is not the fact). */
void test_forget_removes_set_aside_rows_with_its_words(void) {
   const int64_t f = stored("Jonathan Smith collects stamps", "explicit", 0);
   int64_t conv_b = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "B", &conv_b));
   set_aside(conv_b, 0, " jonathan smith collects stamps ",
             "{\"text\": \"jonathan smith collects stamps\", \"subject\": \"Jonathan Smith\"}");
   set_aside(conv_b, 0, "Jonathan Smith has a stamp collection",
             "{\"text\": \"Jonathan Smith has a stamp collection\", \"subject\": \"Jonathan "
             "Smith\"}");
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_forget(f, s_user, NULL));
   TEST_ASSERT_EQUAL_INT(1, q("SELECT COUNT(*) FROM memory_consolidation_ops WHERE user_id = ?1 "
                              "AND op = 'deferred'",
                              s_user, NULL));
   TEST_ASSERT_EQUAL_INT(1, q("SELECT COUNT(*) FROM memory_consolidation_ops WHERE user_id = ?1 "
                              "AND text = ?2",
                              s_user, "Jonathan Smith has a stamp collection"));
}

/* Dropping a word that changes the meaning (used, can't, former) is no
 * shortening. */
void test_a_shortening_may_not_drop_a_meaning_word(void) {
   script("{\"ops\": [{\"op\": \"add\", \"c\": \"C1\", \"text\": \"Jonathan Smith lived in "
          "Portland\"}, {\"op\": \"add\", \"c\": \"C2\", \"text\": \"Jonathan Smith swim\"}]}",
          "{\"ops\": [{\"op\": \"add\", \"c\": \"C1\"}, {\"op\": \"add\", \"c\": \"C2\"}], "
          "\"finish\": true}");
   const memory_consolidate_result_t res = run(
       TWO_FACTS("Jonathan Smith used to have lived in Portland", "Jonathan Smith can't swim"), 0,
       0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_FINISHED, res.stop);
   TEST_ASSERT_EQUAL_INT(0, current("Jonathan Smith lived in Portland"));
   TEST_ASSERT_EQUAL_INT(0, current("Jonathan Smith swim"));
   TEST_ASSERT_TRUE(current("Jonathan Smith can't swim") > 0);
   one_disposition_each(res.batch_id, 2);
}

static bool s_private_now;
static bool not_private(int user_id, int64_t conversation_id) {
   (void)user_id;
   (void)conversation_id;
   return !s_private_now;
}
static void made_private_during_the_plan(int call) {
   if (call == 0) {
      s_private_now = true;
   }
}

/* A conversation made private while the consolidator works: nothing stored,
 * the entities the batch created removed, nothing recorded. */
void test_a_conversation_made_private_mid_batch_stores_nothing(void) {
   stored("Jonathan Smith lives in Portland", "explicit", 0);
   s_private_now = false;
   s_allowed = not_private;
   s_before_call = made_private_during_the_plan;
   script("{\"ops\": [{\"op\": \"replace\", \"old\": \"S1\", \"by\": \"C1\"}]}", NULL);
   run_rc("{\"facts\": [{\"text\": \"Jonathan Smith lives in Springfield\", \"subject\": "
          "\"Jonathan Smith\", \"relations\": [{\"subject\": \"Jonathan Smith\", \"predicate\": "
          "\"lives_in\", \"object\": \"Springfield\"}]}]}",
          0, 0, MEMORY_CONSOLIDATE_REVOKED);
   TEST_ASSERT_EQUAL_INT(0, current("Jonathan Smith lives in Springfield"));
   TEST_ASSERT_EQUAL_INT(0, q("SELECT COUNT(*) FROM memory_entities WHERE user_id = ?1", s_user,
                              NULL));
   TEST_ASSERT_EQUAL_INT(0, q("SELECT COUNT(*) FROM memory_consolidation_batches WHERE user_id = "
                              "?1",
                              s_user, NULL));
}

/* The record keeps a batch MEMORY_CONSOLIDATE_RECORD_DAYS: older ones go,
 * their ops with them. */
void test_the_record_prune_removes_old_batches(void) {
   script(NULL, NULL);
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith plays the cello"), 0, 0);
   TEST_ASSERT_TRUE(res.batch_id > 0);
   int n = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_consolidation_prune(0, &n));
   TEST_ASSERT_EQUAL_INT(0, n); /* nothing that old */
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_consolidation_prune(INT64_MAX, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_INT(0, q("SELECT COUNT(*) FROM memory_consolidation_ops WHERE user_id = ?1",
                              s_user, NULL));
   TEST_ASSERT_TRUE(current("Jonathan Smith plays the cello") > 0); /* memory stays */
}


/* ---- targeted review cases ---- */

/* A batch with no round (its request over the ceiling): the extraction gate's
 * entity pairs are decided and recorded; from outside text only proposed. */
static memory_consolidate_result_t no_round_with_a_pair(int outside) {
   g_config.memory.entity_merge_auto_threshold = 0.0f;
   g_config.memory.entity_merge_review_threshold = 0.0f;
   bool created = false;
   int64_t jon = 0;
   memory_db_entity_upsert(s_user, "Jonathan Smith", "person", "jonathan smith", &created, &jon);
   script(NULL, NULL);
   return run("{\"entities\": [{\"name\": \"Jonathan R Smith\", \"type\": \"person\"}], "
              "\"facts\": [{\"text\": \"Jonathan R Smith plays the oboe\", \"subject\": "
              "\"Jonathan R Smith\"}]}",
              outside, MEMORY_CONSOLIDATE_REQUEST_MAX);
}

void test_no_round_from_outside_text_only_proposes_a_link(void) {
   const memory_consolidate_result_t res = no_round_with_a_pair(OUTSIDE_EMAIL);
   TEST_ASSERT_EQUAL_INT(0, res.rounds);
   TEST_ASSERT_EQUAL_INT(0, q("SELECT COUNT(*) FROM memory_entities WHERE user_id = ?1 AND "
                              "canonical_id IS NOT NULL",
                              s_user, NULL));
   TEST_ASSERT_EQUAL_INT(1, rows(res.batch_id, "proposed", "applied"));
   TEST_ASSERT_EQUAL_INT(0, rows(res.batch_id, "linked", NULL));
}

void test_no_round_links_a_pair_and_records_it(void) {
   const memory_consolidate_result_t res = no_round_with_a_pair(0);
   TEST_ASSERT_EQUAL_INT(0, res.rounds);
   TEST_ASSERT_EQUAL_INT(1, rows(res.batch_id, "linked", "applied") +
                                rows(res.batch_id, "proposed", "applied"));
   TEST_ASSERT_EQUAL_INT(rows(res.batch_id, "linked", "applied"),
                         q("SELECT COUNT(*) FROM memory_entities WHERE user_id = ?1 AND "
                           "canonical_id IS NOT NULL",
                           s_user, NULL));
}

/* A stored fact said again keeps what the restatement adds: its relation. */
void test_a_restated_fact_stores_its_new_relation(void) {
   bool created = false;
   int64_t jon = 0;
   memory_db_entity_upsert(s_user, "Jonathan Smith", "person", "jonathan smith", &created, &jon);
   const int64_t f = stored("Jonathan Smith works at Acme", "explicit", 0);
   script(NULL, NULL);
   const memory_consolidate_result_t res = run(
       "{\"facts\": [{\"text\": \"Jonathan Smith works at Acme\", \"subject\": \"Jonathan "
       "Smith\", \"relations\": [{\"subject\": \"Jonathan Smith\", \"predicate\": \"works_at\", "
       "\"object\": \"Acme\", \"valid_from\": \"2026-09-01\"}]}]}",
       0, 0);
   TEST_ASSERT_EQUAL_INT(1, rows(res.batch_id, "reinforced", "applied"));
   TEST_ASSERT_EQUAL_INT(1, q("SELECT COUNT(*) FROM memory_relations WHERE fact_id = ?1", f, NULL));
   one_disposition_each(res.batch_id, 1);
}

/* Forgetting a fact removes a set-aside row with its words up to a final
 * full stop, as the forgotten checks fold them. */
void test_forget_folds_the_set_aside_words(void) {
   const int64_t f = stored("Jonathan Smith collects stamps", "explicit", 0);
   int64_t conv_b = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "B", &conv_b));
   set_aside(conv_b, 0, "Jonathan Smith collects stamps.",
             "{\"text\": \"Jonathan Smith collects stamps.\", \"subject\": \"Jonathan Smith\"}");
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_forget(f, s_user, NULL));
   TEST_ASSERT_EQUAL_INT(0, q("SELECT COUNT(*) FROM memory_consolidation_ops WHERE user_id = ?1 "
                              "AND op = 'deferred'",
                              s_user, NULL));
}

/* A set-aside candidate with forgotten words, dropped by the plan: recorded
 * without them. */
void test_a_dropped_candidate_with_forgotten_words_is_recorded_without_them(void) {
   forgot("Jonathan Smith collects stamps");
   set_aside(s_conv, 0, "Jonathan Smith collects stamps.",
             "{\"text\": \"Jonathan Smith collects stamps.\", \"subject\": \"Jonathan Smith\"}");
   script("{\"ops\": [{\"op\": \"add\", \"c\": \"C1\"}, {\"op\": \"drop\", \"c\": \"D1\"}], "
          "\"finish\": true}",
          NULL);
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith plays the oboe"), 0, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_FINISHED, res.stop);
   TEST_ASSERT_EQUAL_INT(0, q("SELECT COUNT(*) FROM memory_consolidation_ops WHERE batch_id = ?1 "
                              "AND (text LIKE '%stamps%' OR pre_image LIKE '%stamps%')",
                              res.batch_id, NULL));
   TEST_ASSERT_EQUAL_INT(1, rows(res.batch_id, "dropped", "applied"));
}

/* Near a forgotten fact, merged into a stored one by a plan that didn't
 * finish: set aside, not reinforced. */
void test_a_merge_near_a_forgotten_fact_on_a_failed_repair_is_set_aside(void) {
   forgot("Jonathan Smith collects stamps");
   stored("Jonathan Smith collects old coins", "inferred", 0);
   script("{\"ops\": [{\"op\": \"merge\", \"c\": \"C1\", \"into\": \"S1\"}, {\"op\": \"drop\", "
          "\"c\": \"C9\"}]}",
          "nope");
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith collects coins"), 0, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_REPAIR_FAILED, res.stop);
   TEST_ASSERT_EQUAL_INT(1, rows(res.batch_id, "deferred", "applied"));
   TEST_ASSERT_EQUAL_INT(0, rows(res.batch_id, "reinforced", NULL));
}

/* A reinforce from another conversation records the fact's source
 * conversation before it (a reinforce may move it). */
void test_a_reinforce_records_the_source_conversation_before_it(void) {
   stored("Jonathan Smith plays the cello", "explicit", 0);
   const int64_t conv_a = s_conv;
   int64_t conv_b = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "B", &conv_b));
   s_conv = conv_b;
   script(NULL, NULL);
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith plays the cello"), 0, 0);
   memory_consolidation_op_row_t r[8];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_consolidation_ops_list(s_user, res.batch_id, r, 8, &n));
   int64_t before = -1;
   for (int k = 0; k < n; k++) {
      struct json_object *pre = r[k].pre_image ? json_tokener_parse(r[k].pre_image) : NULL;
      struct json_object *v = NULL;
      if (strcmp(r[k].op, "reinforced") == 0 && pre &&
          json_object_object_get_ex(pre, "source_conversation_id", &v)) {
         before = json_object_get_int64(v);
      }
      json_object_put(pre);
   }
   memory_db_consolidation_ops_free(r, n);
   TEST_ASSERT_EQUAL_INT64(conv_a, before);
}

/* A candidate set aside by conversation B, set aside again by an unfinished
 * run of conversation A: its row stays B's. */
void test_a_deferred_candidate_set_aside_again_keeps_its_row(void) {
   forgot("Jonathan Smith collects stamps");
   int64_t conv_b = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "B", &conv_b));
   const int64_t prior = set_aside(
       conv_b, 0, "Jonathan Smith collects coins",
       "{\"text\": \"Jonathan Smith collects coins\", \"subject\": \"Jonathan Smith\"}");
   script("{\"ops\": [{\"op\": \"add\", \"c\": \"D1\"}, {\"op\": \"drop\", \"c\": \"C9\"}]}",
          "nope");
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith plays the oboe"), 0, 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_REPAIR_FAILED, res.stop);
   TEST_ASSERT_EQUAL_INT(1, rows(prior, "deferred", "applied"));
   TEST_ASSERT_EQUAL_INT(0, q("SELECT COUNT(*) FROM memory_consolidation_ops WHERE batch_id = ?1 "
                              "AND text = ?2",
                              res.batch_id, "Jonathan Smith collects coins"));
   TEST_ASSERT_EQUAL_INT(0, current("Jonathan Smith collects coins"));
}

/* Two candidates with the same text: one stored, the other recorded. */
void test_a_repeated_candidate_text_is_recorded(void) {
   script(NULL, NULL);
   const memory_consolidate_result_t res = run(
       TWO_FACTS("Jonathan Smith plays the cello", "Jonathan Smith plays the cello."), 0, 0);
   TEST_ASSERT_EQUAL_INT(1, rows(res.batch_id, "dropped", "applied"));
   one_disposition_each(res.batch_id, 2);
}

/* A repeated candidate whose words the user forgot: both recorded as drops,
 * neither with the words (the repeat is recorded without a decision). */
void test_a_repeated_candidate_with_forgotten_words_is_recorded_without_them(void) {
   forgot("Jonathan Smith collects stamps");
   script(NULL, NULL);
   const memory_consolidate_result_t res = run(
       TWO_FACTS("Jonathan Smith collects stamps", "jonathan smith collects stamps."), 0, 0);
   TEST_ASSERT_EQUAL_INT(2, rows(res.batch_id, "dropped", "applied"));
   TEST_ASSERT_EQUAL_INT(0, q("SELECT COUNT(*) FROM memory_consolidation_ops WHERE batch_id = ?1 "
                              "AND (text LIKE '%stamps%' OR pre_image LIKE '%stamps%')",
                              res.batch_id, NULL));
   one_disposition_each(res.batch_id, 2);
}

/* Last: the stop can't be undone in the process. */
void test_cancelled_at_shutdown_stores_additions(void) {
   stored("Jonathan Smith lives in Portland", "explicit", 0);
   memory_extraction_stop_queue();
   script("{\"ops\": [{\"op\": \"replace\", \"old\": \"S1\", \"by\": \"C1\"}]}", NULL);
   const memory_consolidate_result_t res = run(ONE_FACT("Jonathan Smith lives in Springfield"), 0,
                                               0);
   TEST_ASSERT_EQUAL_INT(MEMORY_CONSOLIDATE_CANCELLED, res.stop);
   TEST_ASSERT_EQUAL_INT(0, s_calls);
   TEST_ASSERT_TRUE(current("Jonathan Smith lives in Springfield") > 0);
   one_disposition_each(res.batch_id, 1);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_nothing_similar_is_added_without_a_consolidation_round);
   RUN_TEST(test_same_text_is_reinforced_and_forgotten_text_dropped);
   RUN_TEST(test_an_unshown_id_is_named_in_the_repair_and_the_fix_applies);
   RUN_TEST(test_a_failed_repair_applies_only_additions);
   RUN_TEST(test_should_stop_order);
   RUN_TEST(test_samples_over_the_old_budget_still_get_their_round);
   RUN_TEST(test_a_request_over_the_ceiling_gets_no_round);
   RUN_TEST(test_an_incomplete_plan_over_the_output_budget_stops);
   RUN_TEST(test_loss_cap_over_three_neighbours);
   RUN_TEST(test_near_a_forgotten_fact_is_deferred_never_added_on_a_partial_run);
   RUN_TEST(test_a_shortened_text_that_is_forgotten_is_refused);
   RUN_TEST(test_a_shortened_text_may_only_delete_words);
   RUN_TEST(test_a_new_word_or_a_dropped_not_is_refused);
   RUN_TEST(test_the_assistant_is_no_subject_but_dawn_is);
   RUN_TEST(test_a_tainted_batch_replaces_neither_a_clean_nor_an_unknown_fact);
   RUN_TEST(test_a_tainted_fact_replaces_no_clean_one_in_a_clean_batch);
   RUN_TEST(test_a_tainted_batch_links_nothing_it_only_proposes);
   RUN_TEST(test_a_tainted_restatement_reinforces_without_making_it_told);
   RUN_TEST(test_a_tainted_preference_does_not_replace_a_clean_one);
   RUN_TEST(test_a_tainted_batch_merges_two_clean_facts);
   RUN_TEST(test_two_tainted_facts_merge);
   RUN_TEST(test_a_clean_and_a_tainted_fact_dont_merge);
   RUN_TEST(test_only_forgotten_facts_near_a_candidate_are_shown);
   RUN_TEST(test_a_re_added_deferred_candidate_keeps_its_taint);
   RUN_TEST(test_apply_rechecks_a_fact_superseded_since_the_plan);
   RUN_TEST(test_merging_a_stored_fact_into_a_fuller_candidate_keeps_the_fuller_text);
   RUN_TEST(test_close_relation_ends_an_open_value);
   RUN_TEST(test_forgetting_a_fact_scrubs_its_batch_record);
   RUN_TEST(test_a_conversation_made_private_loses_its_batches);
   RUN_TEST(test_a_tainted_deferred_restatement_makes_nothing_told);
   RUN_TEST(test_a_kept_deferred_candidate_is_learned_from_its_own_conversation);
   RUN_TEST(test_a_merge_cycle_keeps_one);
   RUN_TEST(test_a_failed_repair_adds_nothing_near_a_forgotten_fact);
   RUN_TEST(test_a_repair_restating_the_plan_finishes);
   RUN_TEST(test_an_empty_repair_fails);
   RUN_TEST(test_an_empty_repair_fails_when_only_a_non_disposition_was_rejected);
   RUN_TEST(test_forget_removes_set_aside_rows_with_its_words);
   RUN_TEST(test_a_shortening_may_not_drop_a_meaning_word);
   RUN_TEST(test_a_conversation_made_private_mid_batch_stores_nothing);
   RUN_TEST(test_the_record_prune_removes_old_batches);
   RUN_TEST(test_no_round_from_outside_text_only_proposes_a_link);
   RUN_TEST(test_no_round_links_a_pair_and_records_it);
   RUN_TEST(test_a_restated_fact_stores_its_new_relation);
   RUN_TEST(test_forget_folds_the_set_aside_words);
   RUN_TEST(test_a_dropped_candidate_with_forgotten_words_is_recorded_without_them);
   RUN_TEST(test_a_merge_near_a_forgotten_fact_on_a_failed_repair_is_set_aside);
   RUN_TEST(test_a_reinforce_records_the_source_conversation_before_it);
   RUN_TEST(test_a_deferred_candidate_set_aside_again_keeps_its_row);
   RUN_TEST(test_a_repeated_candidate_text_is_recorded);
   RUN_TEST(test_a_repeated_candidate_with_forgotten_words_is_recorded_without_them);
   RUN_TEST(test_cancelled_at_shutdown_stores_additions);
   return UNITY_END();
}
