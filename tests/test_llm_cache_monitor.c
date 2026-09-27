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
 * Unit tests for the per-call prompt-cache telemetry: the cacheable-prefix hash
 * of a noted request, and the expected-read bookkeeping per cache key.
 */

#include <json-c/json.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "core/session_manager.h"
#include "llm/llm_cache_monitor.h"
#include "llm/llm_cache_monitor_internal.h"
#include "unity.h"

/* ---- stubs ---- */

static session_t *s_context;

session_t *session_get_command_context(void) {
   return s_context;
}
const char *cloud_provider_to_string(cloud_provider_t provider) {
   return provider == CLOUD_PROVIDER_CLAUDE ? "claude" : "openai";
}
int session_effective_user_id(session_t *session) {
   return session ? session->metrics.user_id : 0;
}
int session_default_voice_user_id(void) {
   return 1;
}

/* The database write: captured. */
static llm_usage_row_t s_written[300];
static int s_written_count;
static int s_insert_calls;
static bool s_insert_fails;
int auth_db_llm_usage_insert(const llm_usage_row_t *rows, int count) {
   s_insert_calls++;
   if (s_insert_fails) {
      return AUTH_DB_FAILURE;
   }
   for (int i = 0; i < count && s_written_count < 300; i++) {
      s_written[s_written_count++] = rows[i];
   }
   return AUTH_DB_SUCCESS;
}

/* ---- helpers ---- */

/* An Anthropic-shaped request: tools, system [stable, per-turn]. */
static struct json_object *claude_request(const char *stable,
                                          const char *per_turn,
                                          const char *tool) {
   struct json_object *r = json_object_new_object();
   json_object_object_add(r, "model", json_object_new_string("claude-sonnet-5"));
   struct json_object *tools = json_object_new_array();
   struct json_object *t = json_object_new_object();
   json_object_object_add(t, "name", json_object_new_string(tool));
   json_object_array_add(tools, t);
   json_object_object_add(r, "tools", tools);
   struct json_object *system = json_object_new_array();
   const char *parts[2] = { stable, per_turn };
   for (int i = 0; i < 2; i++) {
      struct json_object *b = json_object_new_object();
      json_object_object_add(b, "type", json_object_new_string("text"));
      json_object_object_add(b, "text", json_object_new_string(parts[i]));
      json_object_array_add(system, b);
   }
   json_object_object_add(r, "system", system);
   return r;
}

static llm_cache_prefix_t prefix_of(struct json_object *request) {
   llm_cache_monitor_note_request(request);
   llm_cache_prefix_t p = { 0 };
   TEST_ASSERT_TRUE(llm_cache_monitor_noted(&p, NULL));
   return p;
}

static const llm_cache_prefix_t P1 = { .tools = 1, .system = 2, .model = 3, .thinking = 4 };

/* expected_read_at on key (session 7, conversation 70) at @p now. */
static int expect(const llm_cache_prefix_t *p,
                  int cached_after,
                  uint64_t now,
                  llm_cache_state_t *state) {
   uint64_t gap = 0;
   return llm_cache_monitor_expected_read_at(7, 70, p, 300000, cached_after, NULL, now, &gap,
                                             state);
}

void setUp(void) {
   s_context = NULL;
   llm_cache_monitor_flush(); /* drain whatever an earlier test queued */
   s_insert_fails = false;
   s_written_count = 0;
   s_insert_calls = 0;
   llm_cache_monitor_reset_keys();
   llm_cache_monitor_set_iteration(-1);
}

void tearDown(void) {
}

/* ---- tests ---- */

void test_prefix_hash_covers_tools_and_stable_system_only(void) {
   struct json_object *a = claude_request("persona", "turn 1 context", "search");
   struct json_object *b = claude_request("persona", "turn 2 context", "search");
   struct json_object *c = claude_request("persona edited", "turn 1 context", "search");
   struct json_object *d = claude_request("persona", "turn 1 context", "weather");
   const llm_cache_prefix_t pa = prefix_of(a);
   const llm_cache_prefix_t pb = prefix_of(b);
   TEST_ASSERT_EQUAL_UINT32(pa.system, pb.system); /* the per-turn block isn't prefix */
   TEST_ASSERT_EQUAL_UINT32(pa.tools, pb.tools);
   TEST_ASSERT_NOT_EQUAL(pa.system, prefix_of(c).system); /* the stable system is */
   TEST_ASSERT_NOT_EQUAL(pa.tools, prefix_of(d).tools);   /* and so are the tools */
   const char *model = NULL;
   llm_cache_monitor_noted(NULL, &model);
   TEST_ASSERT_EQUAL_STRING("claude-sonnet-5", model);
   /* Model and thinking settings are part of the prefix too. */
   json_object_object_add(a, "model", json_object_new_string("claude-opus-5-5"));
   TEST_ASSERT_NOT_EQUAL(pa.model, prefix_of(a).model);
   struct json_object *thinking = json_object_new_object();
   json_object_object_add(thinking, "type", json_object_new_string("adaptive"));
   json_object_object_add(b, "thinking", thinking);
   TEST_ASSERT_NOT_EQUAL(pb.thinking, prefix_of(b).thinking);
   json_object_put(a);
   json_object_put(b);
   json_object_put(c);
   json_object_put(d);
}

void test_prefix_hash_of_chat_completions_and_responses(void) {
   /* Chat Completions: the leading system message, merged volatile and all. */
   struct json_object *cc = json_object_new_object();
   struct json_object *messages = json_object_new_array();
   struct json_object *sys = json_object_new_object();
   json_object_object_add(sys, "role", json_object_new_string("system"));
   json_object_object_add(sys, "content", json_object_new_string("persona + turn 1"));
   json_object_array_add(messages, sys);
   json_object_object_add(cc, "messages", messages);
   const uint32_t h1 = prefix_of(cc).system;
   json_object_object_add(sys, "content", json_object_new_string("persona + turn 2"));
   TEST_ASSERT_NOT_EQUAL(h1, prefix_of(cc).system); /* the front-merge moves the prefix */

   /* Responses: the instructions. */
   struct json_object *resp = json_object_new_object();
   json_object_object_add(resp, "instructions", json_object_new_string("persona"));
   const uint32_t h2 = prefix_of(resp).system;
   json_object_object_add(resp, "instructions", json_object_new_string("persona v2"));
   TEST_ASSERT_NOT_EQUAL(h2, prefix_of(resp).system);
   json_object_put(cc);
   json_object_put(resp);
}

void test_images_flag_is_the_newest_message(void) {
   struct json_object *r = claude_request("persona", "ctx", "search");
   struct json_object *messages = json_object_new_array();
   struct json_object *msg = json_object_new_object();
   struct json_object *content = json_object_new_array();
   struct json_object *img = json_object_new_object();
   json_object_object_add(img, "type", json_object_new_string("image"));
   json_object_array_add(content, img);
   json_object_object_add(msg, "content", content);
   json_object_array_add(messages, msg);
   json_object_object_add(r, "messages", messages);
   llm_cache_monitor_note_request(r);
   llm_usage_report_t usage = { .prompt_tokens = 10,
                                .type = LLM_CLOUD,
                                .provider = CLOUD_PROVIDER_CLAUDE };
   llm_cache_record_t rec;
   llm_cache_monitor_record(0, &usage, &rec);
   TEST_ASSERT_TRUE(rec.images);
   json_object_put(r);
}

void test_expected_read_follows_the_previous_call_on_its_key(void) {
   llm_cache_state_t state;
   uint64_t gap = 0;
   /* First call on the key: cold. */
   TEST_ASSERT_EQUAL_INT(0, expect(&P1, 5000, 1000, &state));
   TEST_ASSERT_EQUAL_INT(LLM_CACHE_COLD_FIRST, state);
   /* Next call 4 s later, same prefix: it should read what the first left. */
   TEST_ASSERT_EQUAL_INT(5000, llm_cache_monitor_expected_read_at(7, 70, &P1, 300000, 6200, NULL,
                                                                  5000, &gap, &state));
   TEST_ASSERT_EQUAL_UINT64(4000, gap);
   TEST_ASSERT_EQUAL_INT(LLM_CACHE_WARM, state);
   /* Another conversation on the same session has its own key. */
   TEST_ASSERT_EQUAL_INT(0, llm_cache_monitor_expected_read_at(7, 71, &P1, 300000, 100, NULL, 6000,
                                                               &gap, &state));
   TEST_ASSERT_EQUAL_INT(LLM_CACHE_COLD_FIRST, state);
   /* Past the TTL: cold, whatever was cached. */
   TEST_ASSERT_EQUAL_INT(0, expect(&P1, 6400, 400000, &state));
   TEST_ASSERT_EQUAL_INT(LLM_CACHE_COLD_TTL, state);
   /* No key (no session): never an expectation. */
   TEST_ASSERT_EQUAL_INT(0, llm_cache_monitor_expected_read_at(0, 0, &P1, 300000, 9, NULL, 1, &gap,
                                                               &state));
}

void test_a_changed_prefix_names_what_changed(void) {
   llm_cache_state_t state;
   const struct {
      llm_cache_prefix_t p;
      llm_cache_state_t want;
   } cases[] = {
      { { .tools = 9, .system = 2, .model = 3, .thinking = 4 }, LLM_CACHE_COLD_TOOLS },
      { { .tools = 1, .system = 9, .model = 3, .thinking = 4 }, LLM_CACHE_COLD_SYSTEM },
      { { .tools = 1, .system = 2, .model = 9, .thinking = 4 }, LLM_CACHE_COLD_MODEL },
      { { .tools = 1, .system = 2, .model = 3, .thinking = 9 }, LLM_CACHE_COLD_THINKING },
   };
   uint64_t now = 1000;
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      llm_cache_monitor_reset_keys();
      expect(&P1, 500, now, &state);
      TEST_ASSERT_EQUAL_INT(0, expect(&cases[i].p, 500, now + 1000, &state));
      TEST_ASSERT_EQUAL_INT(cases[i].want, state);
   }
}

void test_least_recently_used_key_is_replaced(void) {
   uint64_t gap = 0;
   llm_cache_state_t state;
   for (uint32_t k = 1; k <= 320; k++) {
      llm_cache_monitor_expected_read_at(k, 0, &P1, 300000, 10, NULL, k, &gap, &state);
   }
   llm_cache_monitor_expected_read_at(1, 0, &P1, 300000, 10, NULL, 1000, &gap, &state); /* reused */
   llm_cache_monitor_expected_read_at(999, 0, &P1, 300000, 10, NULL, 1001, &gap,
                                      &state); /* evicts 2 */
   TEST_ASSERT_EQUAL_INT(10, llm_cache_monitor_expected_read_at(1, 0, &P1, 300000, 10, NULL, 1002,
                                                                &gap, &state));
   TEST_ASSERT_EQUAL_INT(0, llm_cache_monitor_expected_read_at(2, 0, &P1, 300000, 10, NULL, 1003,
                                                               &gap, &state));
}

void test_side_calls_leave_the_conversation_key_alone(void) {
   session_t session = { .session_id = 7, .type = SESSION_TYPE_WEBUI };
   atomic_store(&session.stream_conversation_id, 70);
   s_context = &session;
   struct json_object *turn = claude_request("persona", "ctx", "search");
   struct json_object *side = claude_request("summarize this", "", "none");
   llm_usage_report_t usage = { .prompt_tokens = 1000,
                                .cached_tokens = 0,
                                .cache_write_tokens = 900,
                                .type = LLM_CLOUD,
                                .provider = CLOUD_PROVIDER_CLAUDE };
   llm_cache_record_t rec;
   llm_cache_monitor_set_iteration(0);
   llm_cache_monitor_note_request(turn);
   llm_cache_monitor_record(7, &usage, &rec); /* the turn writes 900 */
   /* A compaction on the same session in between. */
   const int prev = llm_cache_monitor_push_kind(LLM_CALL_COMPACTION);
   llm_cache_monitor_note_request(side);
   llm_cache_monitor_record(7, &usage, &rec);
   llm_cache_monitor_pop_kind(prev);
   TEST_ASSERT_EQUAL_INT(LLM_CALL_COMPACTION, rec.kind);
   TEST_ASSERT_EQUAL_INT(-1, rec.expected); /* no expectation for a side call */
   /* The next tool iteration still expects the turn's 900. */
   llm_cache_monitor_set_iteration(1);
   llm_cache_monitor_note_request(turn);
   usage.cached_tokens = 900;
   usage.cache_write_tokens = 50;
   llm_cache_monitor_record(7, &usage, &rec);
   TEST_ASSERT_EQUAL_INT(LLM_CALL_TOOL_ITER, rec.kind);
   TEST_ASSERT_EQUAL_INT(900, rec.expected);
   TEST_ASSERT_EQUAL_INT(70, rec.conversation_id);
   json_object_put(turn);
   json_object_put(side);
}

void test_record_consumes_the_note(void) {
   struct json_object *r = claude_request("persona", "ctx", "search");
   llm_cache_monitor_note_request(r);
   llm_usage_report_t usage = { .prompt_tokens = 100,
                                .cached_tokens = 80,
                                .cache_write_tokens = 10,
                                .type = LLM_CLOUD,
                                .provider = CLOUD_PROVIDER_CLAUDE };
   llm_cache_monitor_record(3, &usage, NULL);
   /* One record per noted request: the next call without a note isn't
    * attributed to this one's model or prefix. */
   TEST_ASSERT_FALSE(llm_cache_monitor_noted(NULL, NULL));
   json_object_put(r);
}

void test_kind_tags_nest(void) {
   TEST_ASSERT_EQUAL_STRING("tool_iter", llm_call_kind_name(LLM_CALL_TOOL_ITER));
   TEST_ASSERT_EQUAL_STRING("other", llm_call_kind_name(LLM_CALL_KIND_COUNT));
   const int outer = llm_cache_monitor_push_kind(LLM_CALL_COMPACTION);
   const int inner = llm_cache_monitor_push_kind(LLM_CALL_EXTRACTION);
   TEST_ASSERT_EQUAL_INT(LLM_CALL_COMPACTION, inner);
   llm_cache_monitor_pop_kind(inner);
   llm_cache_monitor_pop_kind(outer);
   TEST_ASSERT_EQUAL_INT(-1, outer);
}

void test_local_cache_outlives_the_cloud_ttl(void) {
   TEST_ASSERT_TRUE(llm_cache_monitor_ttl_ms(LLM_LOCAL, CLOUD_PROVIDER_NONE) >
                    llm_cache_monitor_ttl_ms(LLM_CLOUD, CLOUD_PROVIDER_CLAUDE));
}

void test_only_side_calls_leave_the_context_numbers_alone(void) {
   /* The conversation's own calls and unattributed ones set the session's
    * context numbers; tagged side calls don't. */
   TEST_ASSERT_TRUE(llm_call_kind_sets_context(LLM_CALL_TURN));
   TEST_ASSERT_TRUE(llm_call_kind_sets_context(LLM_CALL_TOOL_ITER));
   TEST_ASSERT_TRUE(llm_call_kind_sets_context(LLM_CALL_JOB));
   TEST_ASSERT_TRUE(llm_call_kind_sets_context(LLM_CALL_RESEARCH));
   TEST_ASSERT_TRUE(llm_call_kind_sets_context(LLM_CALL_OTHER));
   TEST_ASSERT_FALSE(llm_call_kind_sets_context(LLM_CALL_EXTRACTION));
   TEST_ASSERT_FALSE(llm_call_kind_sets_context(LLM_CALL_COMPACTION));
   TEST_ASSERT_FALSE(llm_call_kind_sets_context(LLM_CALL_BRIEFING));
   TEST_ASSERT_FALSE(llm_call_kind_sets_context(LLM_CALL_OBSERVE));
   TEST_ASSERT_FALSE(llm_call_kind_sets_context(LLM_CALL_SUMMARIZER));
   TEST_ASSERT_FALSE(llm_call_kind_sets_context(LLM_CALL_SYNTHESIS));
}

static llm_call_kind_t kind_now(void) {
   llm_usage_report_t usage = { .prompt_tokens = 1, .type = LLM_LOCAL };
   llm_cache_record_t rec;
   llm_cache_monitor_record(0, &usage, &rec);
   return rec.kind;
}

void test_kind_without_a_session_follows_the_tool_loop(void) {
   /* No session and not the local microphone: unattributed. */
   s_context = NULL;
   llm_cache_monitor_set_iteration(0);
   TEST_ASSERT_EQUAL_INT(LLM_CALL_OTHER, kind_now());
   /* The local microphone's turn has no session: its loop names its calls. */
   llm_cache_monitor_set_local_mic(true);
   TEST_ASSERT_EQUAL_INT(LLM_CALL_TURN, kind_now());
   llm_cache_monitor_set_iteration(2);
   TEST_ASSERT_EQUAL_INT(LLM_CALL_TOOL_ITER, kind_now());
   TEST_ASSERT_EQUAL_INT(2, llm_cache_monitor_get_iteration());
   llm_cache_monitor_set_local_mic(false);
   llm_cache_monitor_set_iteration(-1);
   TEST_ASSERT_EQUAL_INT(LLM_CALL_OTHER, kind_now());
   /* A job session is a job whatever the iteration; a research run is research. */
   session_t job = { .session_id = 9, .type = SESSION_TYPE_JOB };
   s_context = &job;
   TEST_ASSERT_EQUAL_INT(LLM_CALL_JOB, kind_now());
   atomic_store(&job.research_run_id, 4);
   TEST_ASSERT_EQUAL_INT(LLM_CALL_RESEARCH, kind_now());
   /* A tag wins over all of it. */
   const int prev = llm_cache_monitor_push_kind(LLM_CALL_SYNTHESIS);
   TEST_ASSERT_EQUAL_INT(LLM_CALL_SYNTHESIS, kind_now());
   llm_cache_monitor_pop_kind(prev);
}

void test_flush_writes_the_queued_records(void) {
   session_t session = { .session_id = 7, .type = SESSION_TYPE_WEBUI };
   session.metrics.user_id = 5;
   atomic_store(&session.stream_conversation_id, 70);
   s_context = &session;
   struct json_object *r = claude_request("persona", "ctx", "search");
   llm_cache_monitor_set_iteration(0);
   llm_cache_monitor_note_request(r);
   llm_usage_report_t usage = { .prompt_tokens = 1000,
                                .cached_tokens = 700,
                                .cache_write_tokens = 200,
                                .completion_tokens = 40,
                                .type = LLM_CLOUD,
                                .provider = CLOUD_PROVIDER_CLAUDE };
   llm_cache_monitor_record(7, &usage, NULL);
   TEST_ASSERT_EQUAL_INT(0, s_written_count); /* nothing written from the call path */
   llm_cache_monitor_flush();
   TEST_ASSERT_EQUAL_INT(1, s_insert_calls);
   TEST_ASSERT_EQUAL_INT(1, s_written_count);
   const llm_usage_row_t *w = &s_written[0];
   TEST_ASSERT_EQUAL_INT(5, w->user_id);
   TEST_ASSERT_EQUAL_INT64(70, w->conversation_id);
   TEST_ASSERT_EQUAL_STRING("claude", w->provider);
   TEST_ASSERT_EQUAL_STRING("claude-sonnet-5", w->model);
   TEST_ASSERT_EQUAL_STRING("turn", w->kind);
   TEST_ASSERT_EQUAL_INT(700, w->cache_read_tokens);
   TEST_ASSERT_EQUAL_INT(200, w->cache_write_tokens);
   TEST_ASSERT_EQUAL_INT(100, w->uncached_tokens);
   TEST_ASSERT_EQUAL_INT(40, w->output_tokens);
   TEST_ASSERT_EQUAL_INT(0, w->expected_read); /* first call on the key */
   TEST_ASSERT_EQUAL_STRING("first", w->cache_state);
   /* An empty queue writes nothing. */
   llm_cache_monitor_flush();
   TEST_ASSERT_EQUAL_INT(1, s_insert_calls);
   json_object_put(r);
}

void test_a_full_queue_keeps_the_newest(void) {
   llm_usage_report_t usage = { .type = LLM_LOCAL };
   for (int i = 1; i <= 260; i++) {
      usage.prompt_tokens = i;
      llm_cache_monitor_record(0, &usage, NULL);
   }
   llm_cache_monitor_flush();
   TEST_ASSERT_EQUAL_INT(256, s_written_count);
   TEST_ASSERT_EQUAL_INT(5, s_written[0].prompt_tokens); /* 1-4 dropped */
   TEST_ASSERT_EQUAL_INT(260, s_written[255].prompt_tokens);
}

void test_diagnostics_chain_follows_the_conversation(void) {
   session_t session = { .session_id = 7, .type = SESSION_TYPE_WEBUI };
   atomic_store(&session.stream_conversation_id, 70);
   s_context = &session;
   struct json_object *r = claude_request("persona", "ctx", "search");
   llm_usage_report_t usage = { .prompt_tokens = 100,
                                .cached_tokens = 0,
                                .cache_write_tokens = 90,
                                .type = LLM_CLOUD,
                                .provider = CLOUD_PROVIDER_CLAUDE,
                                .message_id = "msg_A",
                                .cache_miss_reason = "system_changed",
                                .cache_missed_tokens = 6750 };
   char prev[64];
   TEST_ASSERT_FALSE(llm_cache_monitor_previous_message_id(7, 70, prev, sizeof(prev)));
   llm_cache_monitor_set_iteration(0);
   llm_cache_monitor_note_request(r);
   llm_cache_record_t rec;
   llm_cache_monitor_record(7, &usage, &rec);
   TEST_ASSERT_EQUAL_STRING("system_changed", rec.miss_reason);
   TEST_ASSERT_EQUAL_INT(6750, rec.missed_tokens);
   TEST_ASSERT_TRUE(llm_cache_monitor_previous_message_id(7, 70, prev, sizeof(prev)));
   TEST_ASSERT_EQUAL_STRING("msg_A", prev);
   /* A side call's response isn't the conversation's previous request. */
   const int tag = llm_cache_monitor_push_kind(LLM_CALL_COMPACTION);
   TEST_ASSERT_TRUE(llm_cache_monitor_in_side_call());
   usage.message_id = "msg_side";
   llm_cache_monitor_note_request(r);
   llm_cache_monitor_record(7, &usage, NULL);
   llm_cache_monitor_pop_kind(tag);
   TEST_ASSERT_FALSE(llm_cache_monitor_in_side_call());
   llm_cache_monitor_previous_message_id(7, 70, prev, sizeof(prev));
   TEST_ASSERT_EQUAL_STRING("msg_A", prev);
   /* Other conversations have their own. */
   TEST_ASSERT_FALSE(llm_cache_monitor_previous_message_id(7, 71, prev, sizeof(prev)));
   llm_cache_monitor_flush();
   TEST_ASSERT_EQUAL_STRING("system_changed", s_written[0].cache_miss_reason);
   TEST_ASSERT_EQUAL_INT(6750, s_written[0].cache_missed_tokens);
   json_object_put(r);
}

void test_a_failed_write_keeps_the_rows(void) {
   llm_usage_report_t usage = { .type = LLM_LOCAL };
   for (int i = 1; i <= 3; i++) {
      usage.prompt_tokens = i;
      llm_cache_monitor_record(0, &usage, NULL);
   }
   s_insert_fails = true;
   llm_cache_monitor_flush(); /* the database is unavailable */
   TEST_ASSERT_EQUAL_INT(0, s_written_count);
   usage.prompt_tokens = 4;
   llm_cache_monitor_record(0, &usage, NULL);
   s_insert_fails = false;
   llm_cache_monitor_flush();
   TEST_ASSERT_EQUAL_INT(4, s_written_count); /* each once, in order */
   for (int i = 0; i < 4; i++) {
      TEST_ASSERT_EQUAL_INT(i + 1, s_written[i].prompt_tokens);
   }
   llm_cache_monitor_flush();
   TEST_ASSERT_EQUAL_INT(4, s_written_count); /* nothing written twice */
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_prefix_hash_covers_tools_and_stable_system_only);
   RUN_TEST(test_prefix_hash_of_chat_completions_and_responses);
   RUN_TEST(test_images_flag_is_the_newest_message);
   RUN_TEST(test_expected_read_follows_the_previous_call_on_its_key);
   RUN_TEST(test_a_changed_prefix_names_what_changed);
   RUN_TEST(test_least_recently_used_key_is_replaced);
   RUN_TEST(test_side_calls_leave_the_conversation_key_alone);
   RUN_TEST(test_record_consumes_the_note);
   RUN_TEST(test_kind_tags_nest);
   RUN_TEST(test_local_cache_outlives_the_cloud_ttl);
   RUN_TEST(test_only_side_calls_leave_the_context_numbers_alone);
   RUN_TEST(test_kind_without_a_session_follows_the_tool_loop);
   RUN_TEST(test_flush_writes_the_queued_records);
   RUN_TEST(test_a_full_queue_keeps_the_newest);
   RUN_TEST(test_a_failed_write_keeps_the_rows);
   RUN_TEST(test_diagnostics_chain_follows_the_conversation);
   return UNITY_END();
}
