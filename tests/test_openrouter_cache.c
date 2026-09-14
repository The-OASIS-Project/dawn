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
 * Unit tests for llm_openai_add_anthropic_cache(),
 * llm_openai_merge_leading_system_messages(), and the
 * llm_openai_apply_system_prompt_caching() dispatcher.
 *
 * The headline guard is the copy-on-write invariant: request.messages may ALIAS
 * the session's canonical conversation_history, so wrapping the first system
 * message in a cache_control block must NOT mutate the shared history.  A
 * regression here re-surfaces the bug where the system-prompt inspector showed
 * the raw content-block JSON and array-typed content leaked to readers that
 * expect a plain string.  Also pins the openrouter+anthropic gate, the
 * first-system-only rule, idempotency, and the tools breakpoint.
 *
 * The merge tests pin the fix for the strict-Jinja HTTP 500: DAWN's two-segment
 * [system, system, user] prompt is collapsed to one system message for every
 * non-Anthropic provider, again under the same copy-on-write contract.  The
 * dispatcher tests pin the mutual exclusion (Anthropic → keep split; else merge).
 */

#include <json-c/json.h>
#include <string.h>

#include "llm/llm_openai_cache.h"
#include "unity.h"

#define OPENROUTER_URL "https://openrouter.ai/api/v1/chat/completions"
#define ANTHROPIC_MODEL "anthropic/claude-opus-4"

void setUp(void) {
}
void tearDown(void) {
}

/* ── Builders / accessors ───────────────────────────────────────────────── */

static json_object *make_msg(const char *role, const char *content) {
   json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string(role));
   json_object_object_add(m, "content", json_object_new_string(content));
   return m;
}

/* A two-segment history: [system(stable), system(volatile), user]. */
static json_object *make_history(void) {
   json_object *h = json_object_new_array();
   json_object_array_add(h, make_msg("system", "Your name is Friday."));
   json_object_array_add(h, make_msg("system", "--- TURN CONTEXT ---"));
   json_object_array_add(h, make_msg("user", "hi"));
   return h;
}

static json_object *msg_at(json_object *arr, int idx) {
   return json_object_array_get_idx(arr, idx);
}

static json_object *content_at(json_object *arr, int idx) {
   json_object *c = NULL;
   json_object_object_get_ex(msg_at(arr, idx), "content", &c);
   return c;
}

static json_object *root_messages(json_object *root) {
   json_object *m = NULL;
   json_object_object_get_ex(root, "messages", &m);
   return m;
}

/* Build a request whose "messages" ALIASES the given history (shared ref), the
 * way the real request builder does (root.messages = prepare_chat_history(...)
 * returns the history by reference in the common path). */
static json_object *make_root_aliasing(json_object *history, int with_tools) {
   json_object *root = json_object_new_object();
   json_object_object_add(root, "messages", json_object_get(history));
   if (with_tools) {
      json_object *tools = json_object_new_array();
      json_object *t0 = json_object_new_object();
      json_object_object_add(t0, "name", json_object_new_string("weather"));
      json_object_array_add(tools, t0);
      json_object *t1 = json_object_new_object();
      json_object_object_add(t1, "name", json_object_new_string("search"));
      json_object_array_add(tools, t1);
      json_object_object_add(root, "tools", tools);
   }
   return root;
}

static int has_cache_control(json_object *obj) {
   json_object *cc = NULL;
   return json_object_object_get_ex(obj, "cache_control", &cc) ? 1 : 0;
}

/* ── The regression guard: copy-on-write, history untouched ─────────────── */

static void test_cow_leaves_shared_history_a_plain_string(void) {
   json_object *history = make_history();
   json_object *root = make_root_aliasing(history, 1);

   llm_openai_add_anthropic_cache(root, ANTHROPIC_MODEL, OPENROUTER_URL);

   /* The session's canonical history must be byte-for-byte unchanged: the first
    * system message stays a plain string (NOT wrapped into a content array). */
   TEST_ASSERT_EQUAL_INT(json_type_string, json_object_get_type(content_at(history, 0)));
   TEST_ASSERT_EQUAL_STRING("Your name is Friday.", json_object_get_string(content_at(history, 0)));

   /* The request payload, however, DOES carry the wrapped cache_control block. */
   json_object *rmsgs = root_messages(root);
   TEST_ASSERT_EQUAL_INT(json_type_array, json_object_get_type(content_at(rmsgs, 0)));
   const char *req = json_object_to_json_string(content_at(rmsgs, 0));
   TEST_ASSERT_NOT_NULL(strstr(req, "cache_control"));
   TEST_ASSERT_NOT_NULL(strstr(req, "ephemeral"));
   /* And the wrapped text is preserved. */
   TEST_ASSERT_NOT_NULL(strstr(req, "Your name is Friday."));

   /* Proof the COW path ran: root.messages was swapped to a private array. */
   TEST_ASSERT_TRUE(rmsgs != history);

   json_object_put(root);
   json_object_put(history);
}

/* The wrap targets only the FIRST system message; the volatile second one stays
 * a plain string in both the request and the (untouched) history. */
static void test_only_first_system_message_wrapped(void) {
   json_object *history = make_history();
   json_object *root = make_root_aliasing(history, 0);

   llm_openai_add_anthropic_cache(root, ANTHROPIC_MODEL, OPENROUTER_URL);

   json_object *rmsgs = root_messages(root);
   TEST_ASSERT_EQUAL_INT(json_type_array, json_object_get_type(content_at(rmsgs, 0)));
   TEST_ASSERT_EQUAL_INT(json_type_string, json_object_get_type(content_at(rmsgs, 1)));
   /* History untouched on both system messages. */
   TEST_ASSERT_EQUAL_INT(json_type_string, json_object_get_type(content_at(history, 0)));
   TEST_ASSERT_EQUAL_INT(json_type_string, json_object_get_type(content_at(history, 1)));

   json_object_put(root);
   json_object_put(history);
}

/* ── The tools breakpoint ───────────────────────────────────────────────── */

static void test_cache_control_on_last_tool_only(void) {
   json_object *history = make_history();
   json_object *root = make_root_aliasing(history, 1);

   llm_openai_add_anthropic_cache(root, ANTHROPIC_MODEL, OPENROUTER_URL);

   json_object *tools = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(root, "tools", &tools));
   int n = json_object_array_length(tools);
   TEST_ASSERT_EQUAL_INT(2, n);
   TEST_ASSERT_FALSE(has_cache_control(json_object_array_get_idx(tools, 0)));
   TEST_ASSERT_TRUE(has_cache_control(json_object_array_get_idx(tools, 1)));

   json_object_put(root);
   json_object_put(history);
}

/* ── Idempotency: already-wrapped content is left alone ─────────────────── */

static void test_already_array_content_skipped(void) {
   json_object *history = json_object_new_array();
   /* First system message whose content is ALREADY an array (a prior request
    * already wrapped it, with no intervening per-turn rebuild). */
   json_object *sys = json_object_new_object();
   json_object_object_add(sys, "role", json_object_new_string("system"));
   json_object *arr = json_object_new_array();
   json_object_array_add(arr, json_object_new_string("already-wrapped"));
   json_object_object_add(sys, "content", arr);
   json_object_array_add(history, sys);

   json_object *root = json_object_new_object();
   json_object_object_add(root, "messages", json_object_get(history));

   llm_openai_add_anthropic_cache(root, ANTHROPIC_MODEL, OPENROUTER_URL);

   /* No string content to wrap → messages array left untouched (not re-cloned). */
   TEST_ASSERT_TRUE(root_messages(root) == history);
   TEST_ASSERT_EQUAL_INT(json_type_array, json_object_get_type(content_at(history, 0)));

   json_object_put(root);
   json_object_put(history);
}

/* ── The gate: only openrouter + anthropic models ───────────────────────── */

static void expect_noop(const char *model, const char *url) {
   json_object *history = make_history();
   json_object *root = make_root_aliasing(history, 1);

   llm_openai_add_anthropic_cache(root, model, url);

   /* Untouched: history string, request still aliases history, no cache_control. */
   TEST_ASSERT_EQUAL_INT(json_type_string, json_object_get_type(content_at(history, 0)));
   TEST_ASSERT_TRUE(root_messages(root) == history);
   json_object *tools = NULL;
   json_object_object_get_ex(root, "tools", &tools);
   TEST_ASSERT_FALSE(has_cache_control(json_object_array_get_idx(tools, 1)));

   json_object_put(root);
   json_object_put(history);
}

static void test_noop_non_openrouter_url(void) {
   expect_noop(ANTHROPIC_MODEL, "https://api.anthropic.com/v1/messages");
}

static void test_noop_non_anthropic_model(void) {
   expect_noop("openai/gpt-4o", OPENROUTER_URL);
}

/* ── NULL guards ────────────────────────────────────────────────────────── */

static void test_null_args_are_safe(void) {
   json_object *root = json_object_new_object();
   llm_openai_add_anthropic_cache(NULL, ANTHROPIC_MODEL, OPENROUTER_URL);
   llm_openai_add_anthropic_cache(root, NULL, OPENROUTER_URL);
   llm_openai_add_anthropic_cache(root, ANTHROPIC_MODEL, NULL);
   TEST_PASS();
   json_object_put(root);
}

/* ── merge_leading_system_messages: the strict-Jinja 500 fix ─────────────── */

/* The two-segment [system, system, user] collapses to [system(merged), user] in
 * the REQUEST, joined by a blank line, while the shared history stays untouched. */
static void test_merge_collapses_two_system_messages(void) {
   json_object *history = make_history();
   json_object *root = make_root_aliasing(history, 0);

   llm_openai_merge_leading_system_messages(root);

   json_object *rmsgs = root_messages(root);
   /* Request now has exactly [system, user]. */
   TEST_ASSERT_EQUAL_INT(2, json_object_array_length(rmsgs));
   json_object *role0 = NULL;
   json_object_object_get_ex(msg_at(rmsgs, 0), "role", &role0);
   TEST_ASSERT_EQUAL_STRING("system", json_object_get_string(role0));
   json_object *role1 = NULL;
   json_object_object_get_ex(msg_at(rmsgs, 1), "role", &role1);
   TEST_ASSERT_EQUAL_STRING("user", json_object_get_string(role1));
   /* Merged content = both bodies joined by "\n\n", still a plain string. */
   TEST_ASSERT_EQUAL_INT(json_type_string, json_object_get_type(content_at(rmsgs, 0)));
   TEST_ASSERT_EQUAL_STRING("Your name is Friday.\n\n--- TURN CONTEXT ---",
                            json_object_get_string(content_at(rmsgs, 0)));

   /* COW: the session history is byte-for-byte unchanged (still 3 messages). */
   TEST_ASSERT_TRUE(rmsgs != history);
   TEST_ASSERT_EQUAL_INT(3, json_object_array_length(history));
   TEST_ASSERT_EQUAL_STRING("Your name is Friday.", json_object_get_string(content_at(history, 0)));
   TEST_ASSERT_EQUAL_STRING("--- TURN CONTEXT ---", json_object_get_string(content_at(history, 1)));

   json_object_put(root);
   json_object_put(history);
}

/* A single leading system message is a no-op — the request keeps aliasing history. */
static void test_merge_single_system_is_noop(void) {
   json_object *history = json_object_new_array();
   json_object_array_add(history, make_msg("system", "solo"));
   json_object_array_add(history, make_msg("user", "hi"));
   json_object *root = json_object_new_object();
   json_object_object_add(root, "messages", json_object_get(history));

   llm_openai_merge_leading_system_messages(root);

   TEST_ASSERT_TRUE(root_messages(root) == history);
   TEST_ASSERT_EQUAL_INT(2, json_object_array_length(history));

   json_object_put(root);
   json_object_put(history);
}

/* Only the CONTIGUOUS leading run merges; a system message after a user turn is
 * left in place (order preserved). */
static void test_merge_only_leading_run(void) {
   json_object *history = json_object_new_array();
   json_object_array_add(history, make_msg("system", "a"));
   json_object_array_add(history, make_msg("system", "b"));
   json_object_array_add(history, make_msg("user", "hi"));
   json_object_array_add(history, make_msg("system", "mid-stream system"));
   json_object *root = json_object_new_object();
   json_object_object_add(root, "messages", json_object_get(history));

   llm_openai_merge_leading_system_messages(root);

   json_object *rmsgs = root_messages(root);
   TEST_ASSERT_EQUAL_INT(3, json_object_array_length(rmsgs));
   TEST_ASSERT_EQUAL_STRING("a\n\nb", json_object_get_string(content_at(rmsgs, 0)));
   json_object *role2 = NULL;
   json_object_object_get_ex(msg_at(rmsgs, 2), "role", &role2);
   TEST_ASSERT_EQUAL_STRING("system", json_object_get_string(role2));
   TEST_ASSERT_EQUAL_STRING("mid-stream system", json_object_get_string(content_at(rmsgs, 2)));

   json_object_put(root);
   json_object_put(history);
}

/* ── dispatcher: mutual exclusion between merge and Anthropic cache ──────── */

/* Non-Anthropic (local llama.cpp) → merge; no cache_control anywhere. */
static void test_dispatch_local_merges(void) {
   json_object *history = make_history();
   json_object *root = make_root_aliasing(history, 0);

   llm_openai_apply_system_prompt_caching(root, "qwen3-35b", "http://192.168.1.214:8080");

   json_object *rmsgs = root_messages(root);
   TEST_ASSERT_EQUAL_INT(2, json_object_array_length(rmsgs));
   TEST_ASSERT_EQUAL_INT(json_type_string, json_object_get_type(content_at(rmsgs, 0)));

   json_object_put(root);
   json_object_put(history);
}

/* OpenRouter→Anthropic → keep the split (two system messages) and wrap the first
 * with cache_control; the merge must NOT have run. */
static void test_dispatch_anthropic_keeps_split(void) {
   json_object *history = make_history();
   json_object *root = make_root_aliasing(history, 1);

   llm_openai_apply_system_prompt_caching(root, ANTHROPIC_MODEL, OPENROUTER_URL);

   json_object *rmsgs = root_messages(root);
   /* Still three messages — the split survived. */
   TEST_ASSERT_EQUAL_INT(3, json_object_array_length(rmsgs));
   /* First system message is wrapped (array content w/ cache_control). */
   TEST_ASSERT_EQUAL_INT(json_type_array, json_object_get_type(content_at(rmsgs, 0)));
   const char *req = json_object_to_json_string(content_at(rmsgs, 0));
   TEST_ASSERT_NOT_NULL(strstr(req, "cache_control"));
   /* Second system message stays a separate plain string. */
   TEST_ASSERT_EQUAL_INT(json_type_string, json_object_get_type(content_at(rmsgs, 1)));

   json_object_put(root);
   json_object_put(history);
}

/* The gate predicate is a pure function of (model, url). */
static void test_anthropic_cache_applies_predicate(void) {
   TEST_ASSERT_TRUE(llm_openai_anthropic_cache_applies(ANTHROPIC_MODEL, OPENROUTER_URL));
   TEST_ASSERT_FALSE(llm_openai_anthropic_cache_applies("openai/gpt-4o", OPENROUTER_URL));
   TEST_ASSERT_FALSE(
       llm_openai_anthropic_cache_applies(ANTHROPIC_MODEL, "https://api.anthropic.com"));
   TEST_ASSERT_FALSE(llm_openai_anthropic_cache_applies(NULL, OPENROUTER_URL));
   TEST_ASSERT_FALSE(llm_openai_anthropic_cache_applies(ANTHROPIC_MODEL, NULL));
}

int main(void) {
   UNITY_BEGIN();

   RUN_TEST(test_cow_leaves_shared_history_a_plain_string);
   RUN_TEST(test_only_first_system_message_wrapped);
   RUN_TEST(test_cache_control_on_last_tool_only);
   RUN_TEST(test_already_array_content_skipped);
   RUN_TEST(test_noop_non_openrouter_url);
   RUN_TEST(test_noop_non_anthropic_model);
   RUN_TEST(test_null_args_are_safe);

   RUN_TEST(test_merge_collapses_two_system_messages);
   RUN_TEST(test_merge_single_system_is_noop);
   RUN_TEST(test_merge_only_leading_run);
   RUN_TEST(test_dispatch_local_merges);
   RUN_TEST(test_dispatch_anthropic_keeps_split);
   RUN_TEST(test_anthropic_cache_applies_predicate);

   return UNITY_END();
}
