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
 * Unit tests for convert_to_claude_format() — OpenAI->Claude history conversion.
 *
 * Regression for the consecutive-tool-result double-free: when an assistant
 * issues several parallel tool calls, the history carries several role="tool"
 * messages in a row.  The conversion appended each tool_result block into the
 * shared user-message content array AND into a throwaway wrapper array, then
 * freed the wrapper — taking the still-referenced block with it (use-after-free
 * / json-c _ref_count underflow → SIGSEGV with a corrupt stack in production).
 * These tests build that exact shape and free BOTH the input and the output,
 * which aborts under the bug and passes once the second reference is taken.
 */

#include <json-c/json.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "config/dawn_config.h"
#include "llm/llm_capabilities.h"
#include "llm/llm_claude_format.h"
#include "llm/llm_local_provider.h"
#include "llm/llm_tools.h"
#include "tools/toml.h"
#include "unity.h"

/* ---- Stubs for the config / tool-registry deps the converter references.
 *      (log_message comes from dawn_common.) ---- */
dawn_config_t g_config;
const char *llm_get_default_claude_model(void) {
   return "claude-sonnet-4-6";
}
static const char *s_mode = "disabled";
static const char *s_effort = "medium";
static bool s_suppressed;
const char *llm_get_current_thinking_mode(void) {
   return s_mode;
}
const char *llm_get_current_reasoning_effort(void) {
   return s_effort;
}
bool llm_tools_enabled(const llm_resolved_config_t *c) {
   (void)c;
   return false;
}
bool llm_tools_suppressed(void) {
   return s_suppressed;
}
int llm_budget_tokens_for_effort(const char *effort) {
   return (effort && strcmp(effort, "low") == 0) ? 1024 : 8192;
}
local_provider_t llm_local_get_provider(void) {
   return LOCAL_PROVIDER_LLAMA_CPP;
}
struct json_object *llm_tools_get_claude_format_filtered(bool r) {
   (void)r;
   return NULL;
}
int llm_tools_get_enabled_count_filtered(bool r) {
   (void)r;
   return 0;
}
/* ENABLE_WEBUI is defined for the test build, so the converter's thinking-disabled
 * notify path is compiled in.  Return no session so that path is skipped.
 * In local-only mode (ENABLE_MULTI_CLIENT off) session_manager.h already supplies a
 * static-inline stub, so only define our own when the header expects an extern —
 * otherwise the two collide (redefinition). */
#ifdef ENABLE_MULTI_CLIENT
struct session *session_get_command_context(void) {
   return NULL;
}
#endif
void webui_send_error(struct session *s, const char *code, const char *message) {
   (void)s;
   (void)code;
   (void)message;
}
/* severity typed as int in the stub — the linker matches on symbol name only,
 * and this file doesn't pull in the ws_error_severity_t enum. */
void webui_send_error_ex(struct session *s, const char *code, const char *message, int severity) {
   (void)s;
   (void)code;
   (void)message;
   (void)severity;
}

void setUp(void) {
   g_config.llm.max_tokens = 4096;
   s_mode = "disabled";
   s_effort = "medium";
   s_suppressed = false;
}
void tearDown(void) {
}

/* Build an OpenAI-format assistant message with @p n tool_calls. */
static json_object *assistant_with_tool_calls(int n) {
   json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string("assistant"));
   json_object_object_add(m, "content", json_object_new_string(""));
   json_object *tcs = json_object_new_array();
   for (int i = 0; i < n; i++) {
      char id[32];
      snprintf(id, sizeof(id), "call_%02d", i);
      json_object *call = json_object_new_object();
      json_object_object_add(call, "id", json_object_new_string(id));
      json_object_object_add(call, "type", json_object_new_string("function"));
      json_object *fn = json_object_new_object();
      json_object_object_add(fn, "name", json_object_new_string("memory"));
      json_object_object_add(fn, "arguments", json_object_new_string("{\"action\":\"forget\"}"));
      json_object_object_add(call, "function", fn);
      json_object_array_add(tcs, call);
   }
   json_object_object_add(m, "tool_calls", tcs);
   return m;
}

/* Append an OpenAI-format tool result for call_id @p i to @p conv. */
static void add_tool_result(json_object *conv, int i) {
   char id[32];
   snprintf(id, sizeof(id), "call_%02d", i);
   json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string("tool"));
   json_object_object_add(m, "tool_call_id", json_object_new_string(id));
   json_object_object_add(m, "content", json_object_new_string("Forgotten 1 fact"));
   json_object_array_add(conv, m);
}

/* The load-bearing regression: N parallel tool calls -> N consecutive tool
 * results.  Freeing both input and output must not abort/double-free. */
static void test_parallel_tool_results_no_double_free(void) {
   const int N = 8;
   json_object *conv = json_object_new_array();
   json_object_array_add(conv, assistant_with_tool_calls(N));
   for (int i = 0; i < N; i++) {
      add_tool_result(conv, i);
   }

   json_object *req = convert_to_claude_format(conv, NULL, NULL, NULL, 0, "claude-sonnet-4-6", 0);
   TEST_ASSERT_NOT_NULL(req);

   /* The N tool_results must coalesce into ONE user message holding N blocks. */
   json_object *messages;
   TEST_ASSERT_TRUE(json_object_object_get_ex(req, "messages", &messages));
   json_object *user_msg = json_object_array_get_idx(messages,
                                                     json_object_array_length(messages) - 1);
   json_object *content;
   TEST_ASSERT_TRUE(json_object_object_get_ex(user_msg, "content", &content));
   TEST_ASSERT_EQUAL_INT(N, json_object_array_length(content));

   /* Freeing the output then the input is where the double-free struck. */
   json_object_put(req);
   json_object_put(conv); /* aborts here under the bug; clean with the fix */
}

/* A single tool result (no consecutive append) — exercises the other branch. */
static void test_single_tool_result_ok(void) {
   json_object *conv = json_object_new_array();
   json_object_array_add(conv, assistant_with_tool_calls(1));
   add_tool_result(conv, 0);

   json_object *req = convert_to_claude_format(conv, NULL, NULL, NULL, 0, "claude-sonnet-4-6", 0);
   TEST_ASSERT_NOT_NULL(req);
   json_object_put(req);
   json_object_put(conv);
}

/* ---- Reasoning: the request's thinking config, from models.toml ---- */

static json_object *one_user_turn(void) {
   json_object *conv = json_object_new_array();
   json_object *u = json_object_new_object();
   json_object_object_add(u, "role", json_object_new_string("user"));
   json_object_object_add(u, "content", json_object_new_string("hi"));
   json_object_array_add(conv, u);
   return conv;
}

static const char *thinking_type(json_object *req) {
   json_object *t = NULL;
   json_object *v = NULL;
   if (!json_object_object_get_ex(req, "thinking", &t) ||
       !json_object_object_get_ex(t, "type", &v)) {
      return NULL;
   }
   return json_object_get_string(v);
}

static const char *effort_of(json_object *req) {
   json_object *o = NULL;
   json_object *v = NULL;
   if (!json_object_object_get_ex(req, "output_config", &o) ||
       !json_object_object_get_ex(o, "effort", &v)) {
      return NULL;
   }
   return json_object_get_string(v);
}

static json_object *request_for(const char *model) {
   json_object *conv = one_user_turn();
   json_object *req = convert_to_claude_format(conv, NULL, NULL, NULL, 0, model, 0);
   json_object_put(conv);
   TEST_ASSERT_NOT_NULL(req);
   return req;
}

/* A model that can't turn thinking off: "disabled" runs adaptive at its lowest
 * effort, explicitly, never omitted. */
static void test_disabled_on_adaptive_only_model_is_lowest_adaptive(void) {
   json_object *req = request_for("claude-opus-5-5");
   TEST_ASSERT_EQUAL_STRING("adaptive", thinking_type(req));
   TEST_ASSERT_EQUAL_STRING("low", effort_of(req));
   json_object_put(req);
}

static void test_disabled_is_sent_where_accepted(void) {
   json_object *req = request_for("claude-sonnet-5");
   TEST_ASSERT_EQUAL_STRING("disabled", thinking_type(req));
   TEST_ASSERT_NULL(effort_of(req));
   json_object_put(req);
}

static void test_adaptive_carries_the_session_effort(void) {
   s_mode = "enabled"; /* the WebUI's "on": adaptive where that's the model's way */
   s_effort = "high";
   json_object *req = request_for("claude-opus-5-5");
   TEST_ASSERT_EQUAL_STRING("adaptive", thinking_type(req));
   TEST_ASSERT_EQUAL_STRING("high", effort_of(req));
   json_object_put(req);
}

static void test_budget_model_gets_enabled_with_budget(void) {
   s_mode = "enabled";
   s_effort = "low";
   json_object *req = request_for("claude-haiku-4-5");
   TEST_ASSERT_EQUAL_STRING("enabled", thinking_type(req));
   json_object *t = NULL;
   json_object *b = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(req, "thinking", &t));
   TEST_ASSERT_TRUE(json_object_object_get_ex(t, "budget_tokens", &b));
   TEST_ASSERT_EQUAL_INT(1024, json_object_get_int(b));
   TEST_ASSERT_NULL(effort_of(req));
   json_object_put(req);
}

static void test_utility_call_gets_the_cheapest_legal_setting(void) {
   s_mode = "enabled";
   s_effort = "high";
   s_suppressed = true;
   json_object *req = request_for("claude-opus-5-5");
   TEST_ASSERT_EQUAL_STRING("adaptive", thinking_type(req));
   TEST_ASSERT_EQUAL_STRING("low", effort_of(req));
   json_object_put(req);
   req = request_for("claude-sonnet-5");
   TEST_ASSERT_EQUAL_STRING("disabled", thinking_type(req));
   json_object_put(req);
}

/* An earlier tool call with no thinking block (adaptive chose not to think)
 * no longer turns reasoning off for the conversation. */
static void test_tool_use_without_thinking_keeps_reasoning(void) {
   s_mode = "enabled";
   s_effort = "medium";
   json_object *conv = one_user_turn();
   json_object_array_add(conv, assistant_with_tool_calls(1));
   add_tool_result(conv, 0);
   json_object *req = convert_to_claude_format(conv, NULL, NULL, NULL, 0, "claude-opus-5-5", 1);
   json_object_put(conv);
   TEST_ASSERT_EQUAL_STRING("adaptive", thinking_type(req));
   TEST_ASSERT_EQUAL_STRING("medium", effort_of(req));
   json_object_put(req);
}

static toml_table_t *s_models;

int main(void) {
   char err[256];
   FILE *f = fopen(MODELS_TOML_PATH, "r");
   s_models = f ? toml_parse_file(f, err, sizeof(err)) : NULL;
   if (f) {
      fclose(f);
   }
   llm_capabilities_load_registry(s_models);
   UNITY_BEGIN();
   RUN_TEST(test_parallel_tool_results_no_double_free);
   RUN_TEST(test_single_tool_result_ok);
   RUN_TEST(test_disabled_on_adaptive_only_model_is_lowest_adaptive);
   RUN_TEST(test_disabled_is_sent_where_accepted);
   RUN_TEST(test_adaptive_carries_the_session_effort);
   RUN_TEST(test_budget_model_gets_enabled_with_budget);
   RUN_TEST(test_utility_call_gets_the_cheapest_legal_setting);
   RUN_TEST(test_tool_use_without_thinking_keeps_reasoning);
   return UNITY_END();
}
