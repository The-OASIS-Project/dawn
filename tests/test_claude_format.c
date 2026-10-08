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
#include "core/image_rehydrate.h"
#include "llm/llm_capabilities.h"
#include "llm/llm_claude_format.h"
#include "llm/llm_claude_parts.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_local_provider.h"
#include "llm/llm_tool_defs.h"
#include "llm/llm_tools.h"
#include "llm/llm_turn_blocks.h"
#include "tools/toml.h"
#include "unity.h"

/* A Claude request's carrier: its endpoint and key tag (llm_request_carrier). */
#define TEST_CLAUDE_CARRIER "api.anthropic.com#0123456789abcdef"

/* ---- Stubs for the config / tool-registry deps the converter references.
 *      (log_message comes from dawn_common.) ---- */
dawn_config_t g_config;
/* Every call here is a conversation's (llm_cache_monitor.c). */
bool llm_cache_monitor_one_off_call(void) {
   return false;
}
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
static bool s_tools_on;
bool llm_tools_enabled(const llm_resolved_config_t *c) {
   (void)c;
   return s_tools_on;
}
bool llm_tools_suppressed(void) {
   return s_suppressed;
}
static const char *s_utility_effort = "";
const char *llm_get_current_utility_effort(void) {
   return s_utility_effort;
}
int llm_budget_tokens_for_effort(const char *effort) {
   return (effort && strcmp(effort, "low") == 0) ? 1024 : 8192;
}
local_provider_t llm_local_get_provider(void) {
   return LOCAL_PROVIDER_LLAMA_CPP;
}
local_provider_t llm_local_detect_provider(const char *endpoint) {
   (void)endpoint;
   return llm_local_get_provider();
}
/* The conversation's tools by value (llm_tools_request_tools): its frozen set
 * and the changes not sent in place, a name rendered as itself. */
struct json_object *llm_tools_request_tools(struct json_object *history,
                                            bool r,
                                            bool claude,
                                            bool inline_ok,
                                            const char **source) {
   (void)r;
   if (source)
      *source = "the conversation's set";
   struct json_object *defs = llm_tool_defs_for_request(history, inline_ok);
   if (!defs)
      return NULL;
   struct json_object *out = json_object_new_array();
   for (size_t i = 0; i < json_object_array_length(defs); i++) {
      struct json_object *def = json_object_array_get_idx(defs, i);
      struct json_object *tool = NULL;
      if (json_object_is_type(def, json_type_string)) {
         tool = json_object_new_object();
         json_object_object_add(tool, "name", json_object_get(def));
      } else {
         tool = llm_tool_def_render(def, claude);
      }
      json_object_array_add(out, tool);
   }
   json_object_put(defs);
   return out;
}
int llm_tools_get_enabled_count_filtered(bool r) {
   (void)r;
   return 0;
}
/* ENABLE_WEBUI is defined for the test build, so the converter's thinking-disabled
 * notify path is compiled in.  Return no session so that path is skipped. */
struct session *session_get_command_context(void) {
   return NULL;
}
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

/* Whether the model takes images (llm_command_parser.c). */
static int s_vision = 1;
int is_vision_enabled_for_current_llm(void) {
   return s_vision;
}

void setUp(void) {
   g_config.llm.max_tokens = 4096;
   s_mode = "disabled";
   s_effort = "medium";
   s_utility_effort = "";
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

   json_object *req = convert_to_claude_format(conv, NULL, "claude-sonnet-4-6", TEST_CLAUDE_CARRIER,
                                               0, false);
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

   json_object *req = convert_to_claude_format(conv, NULL, "claude-sonnet-4-6", TEST_CLAUDE_CARRIER,
                                               0, false);
   TEST_ASSERT_NOT_NULL(req);
   json_object_put(req);
   json_object_put(conv);
}

/* A tool result's image that doesn't convert (here a URL that is no data URI;
 * out of memory takes the same path) is the fixed stand-in, not dropped. */
static void test_an_image_that_doesnt_convert_is_a_stand_in(void) {
   json_object *conv = json_object_new_array();
   json_object_array_add(conv, assistant_with_tool_calls(1));
   json_object_array_add(conv, json_tokener_parse(
                                   "{\"role\":\"tool\",\"tool_call_id\":\"call_00\",\"content\":"
                                   "[{\"type\":\"text\",\"text\":\"Captured.\"},{\"type\":"
                                   "\"image_url\",\"image_url\":{\"url\":\"https://x/y.png\"}}]}"));
   json_object *req = convert_to_claude_format(conv, NULL, "claude-sonnet-4-6", TEST_CLAUDE_CARRIER,
                                               0, false);
   TEST_ASSERT_NOT_NULL(req);
   const char *wire = json_object_to_json_string(req);
   TEST_ASSERT_NOT_NULL(strstr(wire, IMAGE_REHYDRATE_MISSING_TEXT));
   TEST_ASSERT_NOT_NULL(strstr(wire, "Captured."));
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
   json_object *req = convert_to_claude_format(conv, NULL, model, TEST_CLAUDE_CARRIER, 0, false);
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

/* An OpenRouter slug gets its model's rules (models.toml knows Anthropic's id):
 * "disabled" is sent where the model takes it, and on a model that can't turn
 * thinking off is its lowest adaptive.  It goes out as the slug. */
static void test_openrouter_slug_gets_its_models_rules(void) {
   json_object *req = request_for("anthropic/claude-sonnet-5");
   TEST_ASSERT_EQUAL_STRING("disabled", thinking_type(req));
   json_object_put(req);
   req = request_for("anthropic/claude-opus-5.5");
   TEST_ASSERT_EQUAL_STRING("adaptive", thinking_type(req));
   TEST_ASSERT_EQUAL_STRING("low", effort_of(req));
   json_object *model = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(req, "model", &model));
   TEST_ASSERT_EQUAL_STRING("anthropic/claude-opus-5.5", json_object_get_string(model));
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

/* Haiku 5.5 takes no budget mode: a budget pick goes out as adaptive, and a
 * utility call (extraction, compaction) turns thinking off rather than
 * running at the model's default effort.  No sampling parameters: 400 there. */
static void test_haiku_5_5_never_gets_a_budget(void) {
   s_mode = "enabled";
   s_effort = "low";
   json_object *req = request_for("claude-haiku-5-5");
   TEST_ASSERT_EQUAL_STRING("adaptive", thinking_type(req));
   TEST_ASSERT_EQUAL_STRING("low", effort_of(req));
   json_object *t = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(req, "thinking", &t));
   TEST_ASSERT_FALSE(json_object_object_get_ex(t, "budget_tokens", NULL));
   TEST_ASSERT_FALSE(json_object_object_get_ex(req, "temperature", NULL));
   json_object_put(req);

   s_suppressed = true;
   req = request_for("claude-haiku-5-5");
   TEST_ASSERT_EQUAL_STRING("disabled", thinking_type(req));
   TEST_ASSERT_NULL(effort_of(req));
   json_object_put(req);

   /* Extraction with [memory] extraction_effort: adaptive at that effort. */
   s_utility_effort = "medium";
   req = request_for("claude-haiku-5-5");
   TEST_ASSERT_EQUAL_STRING("adaptive", thinking_type(req));
   TEST_ASSERT_EQUAL_STRING("medium", effort_of(req));
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
   json_object *req = convert_to_claude_format(conv, NULL, "claude-opus-5-5", TEST_CLAUDE_CARRIER,
                                               1, false);
   json_object_put(conv);
   TEST_ASSERT_EQUAL_STRING("adaptive", thinking_type(req));
   TEST_ASSERT_EQUAL_STRING("medium", effort_of(req));
   json_object_put(req);
}

/* A history final answer that carries blocks is replayed with its thinking,
 * signature intact, not as bare text. */
static void test_final_answer_replays_its_blocks(void) {
   json_object *conv = one_user_turn();
   json_object *answer = json_object_new_object();
   json_object_object_add(answer, "role", json_object_new_string("assistant"));
   json_object_object_add(answer, "content", json_object_new_string("Clean answer."));
   json_object *blocks = llm_turn_blocks_new();
   llm_turn_blocks_add_reasoning(blocks, TEST_CLAUDE_CARRIER, LLM_FORMAT_ANTHROPIC,
                                 "claude-opus-5-5",
                                 json_tokener_parse("{\"type\":\"thinking\",\"thinking\":\"\","
                                                    "\"signature\":\"SIG\"}"));
   llm_turn_blocks_add_text(blocks, "Clean answer.");
   json_object_object_add(answer, LLM_TURN_BLOCKS_KEY, blocks);
   json_object_array_add(conv, answer);
   json_object *next = json_object_new_object();
   json_object_object_add(next, "role", json_object_new_string("user"));
   json_object_object_add(next, "content", json_object_new_string("And tomorrow?"));
   json_object_array_add(conv, next);

   json_object *req = convert_to_claude_format(conv, NULL, "claude-opus-5-5", TEST_CLAUDE_CARRIER,
                                               0, false);
   json_object *messages = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(req, "messages", &messages));
   json_object *assistant = json_object_array_get_idx(messages, 1);
   json_object *content = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(assistant, "content", &content));
   TEST_ASSERT_TRUE(json_object_is_type(content, json_type_array));
   TEST_ASSERT_EQUAL_INT(2, json_object_array_length(content));
   json_object *first = json_object_array_get_idx(content, 0);
   TEST_ASSERT_EQUAL_STRING("thinking",
                            json_object_get_string(json_object_object_get(first, "type")));
   TEST_ASSERT_EQUAL_STRING("SIG",
                            json_object_get_string(json_object_object_get(first, "signature")));
   /* DAWN's key never reaches the wire. */
   TEST_ASSERT_FALSE(json_object_object_get_ex(assistant, LLM_TURN_BLOCKS_KEY, NULL));
   json_object_put(req);
   json_object_put(conv);
}

static toml_table_t *s_models;

/* Results whose call isn't in the message before them (a compaction point
 * inside a tool exchange): sent as notes, never as tool_result blocks. */
static void test_results_without_their_call_become_notes(void) {
   json_object *conv = json_object_new_array();
   json_object *summary = json_object_new_object();
   json_object_object_add(summary, "role", json_object_new_string("assistant"));
   json_object_object_add(summary, "content", json_object_new_string("Earlier: summarized."));
   json_object_array_add(conv, summary);
   add_tool_result(conv, 5); /* its call is in the summary */
   json_object_array_add(conv, assistant_with_tool_calls(1));
   add_tool_result(conv, 0); /* answered properly */
   json_object *req = convert_to_claude_format(conv, NULL, "claude-sonnet-4-6", TEST_CLAUDE_CARRIER,
                                               0, false);
   TEST_ASSERT_NOT_NULL(req);
   const char *wire = json_object_to_json_string(req);
   TEST_ASSERT_NULL(strstr(wire, "\"call_05\""));
   TEST_ASSERT_NOT_NULL(strstr(wire, "Earlier tool result: Forgotten 1 fact"));
   TEST_ASSERT_TRUE(strstr(wire, "\"tool_use_id\": \"call_00\"") ||
                    strstr(wire, "\"tool_use_id\":\"call_00\""));
   json_object_put(req);
   json_object_put(conv);
}

/* Pairs Claude accepts, whatever the history: a call whose result isn't in
 * the next message is dropped (not left unanswered); results come before any
 * other content in their message. */
static void test_calls_and_results_are_paired(void) {
   json_object *conv = json_object_new_array();
   json_object_array_add(conv, assistant_with_tool_calls(2)); /* call_00, call_01 */
   add_tool_result(conv, 0);
   /* A tool message without an id: the formatter turns it into an assistant
    * note, which separates call_01 from its result. */
   json_object *lost = json_object_new_object();
   json_object_object_add(lost, "role", json_object_new_string("tool"));
   json_object_object_add(lost, "content", json_object_new_string("stray"));
   json_object_array_add(conv, lost);
   add_tool_result(conv, 1);
   json_object *req = convert_to_claude_format(conv, NULL, "claude-sonnet-4-6", TEST_CLAUDE_CARRIER,
                                               0, false);
   TEST_ASSERT_NOT_NULL(req);
   json_object *messages = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(req, "messages", &messages));
   /* Every tool_use is answered in the next message, every tool_result answers
    * one in the message before, and results lead their message. */
   const size_t n = json_object_array_length(messages);
   for (size_t i = 0; i < n; i++) {
      json_object *m = json_object_array_get_idx(messages, i), *parts = NULL;
      if (!json_object_object_get_ex(m, "content", &parts) ||
          !json_object_is_type(parts, json_type_array)) {
         continue;
      }
      bool other_seen = false;
      const size_t k = json_object_array_length(parts);
      for (size_t j = 0; j < k; j++) {
         json_object *p = json_object_array_get_idx(parts, j), *t = NULL, *id = NULL;
         json_object_object_get_ex(p, "type", &t);
         const char *type = json_object_get_string(t);
         if (strcmp(type, "tool_use") == 0) {
            TEST_ASSERT_TRUE(json_object_object_get_ex(p, "id", &id));
            TEST_ASSERT_TRUE(i + 1 < n);
            TEST_ASSERT_NOT_NULL(
                strstr(json_object_to_json_string(json_object_array_get_idx(messages, i + 1)),
                       json_object_get_string(id)));
         } else if (strcmp(type, "tool_result") == 0) {
            TEST_ASSERT_FALSE(other_seen);
            TEST_ASSERT_TRUE(json_object_object_get_ex(p, "tool_use_id", &id));
            TEST_ASSERT_TRUE(i > 0);
            TEST_ASSERT_NOT_NULL(
                strstr(json_object_to_json_string(json_object_array_get_idx(messages, i - 1)),
                       json_object_get_string(id)));
         } else {
            other_seen = true;
         }
      }
   }
   json_object_put(req);
   json_object_put(conv);
}

/* A turn left with only its reasoning after its unanswered calls are dropped
 * says what happened instead; a user's text before a tool result stays. */
static void test_reasoning_only_turn_and_user_text(void) {
   json_object *conv = json_object_new_array();
   json_object *a = json_object_new_object();
   json_object_object_add(a, "role", json_object_new_string("assistant"));
   json_object *content = json_object_new_array();
   json_object *th = json_object_new_object();
   json_object_object_add(th, "type", json_object_new_string("thinking"));
   json_object_object_add(th, "thinking", json_object_new_string("plan"));
   json_object_object_add(th, "signature", json_object_new_string("SIG"));
   json_object_array_add(content, th);
   json_object *use = json_object_new_object();
   json_object_object_add(use, "type", json_object_new_string("tool_use"));
   json_object_object_add(use, "id", json_object_new_string("toolu_x"));
   json_object_object_add(use, "name", json_object_new_string("memory"));
   json_object_object_add(use, "input", json_object_new_object());
   json_object_array_add(content, use);
   json_object_object_add(a, "content", content);
   json_object_array_add(conv, a);
   json_object *u = json_object_new_object();
   json_object_object_add(u, "role", json_object_new_string("user"));
   json_object_object_add(u, "content", json_object_new_string("my question stays"));
   json_object_array_add(conv, u);
   add_tool_result(conv, 3); /* follows the user's text; its call isn't anywhere */

   json_object *req = convert_to_claude_format(conv, NULL, "claude-sonnet-4-6", TEST_CLAUDE_CARRIER,
                                               0, false);
   TEST_ASSERT_NOT_NULL(req);
   const char *wire = json_object_to_json_string(req);
   TEST_ASSERT_NULL(strstr(wire, "SIG"));
   TEST_ASSERT_NULL(strstr(wire, "toolu_x"));
   TEST_ASSERT_NOT_NULL(strstr(wire, "Tool call not completed"));
   TEST_ASSERT_NOT_NULL(strstr(wire, "my question stays"));
   json_object_put(req);
   json_object_put(conv);
}

/* A reply's text is its text blocks, whatever comes first: a model that can't
 * turn thinking off may open with a thinking block. */
static void test_reply_text_skips_thinking(void) {
   json_object *reply = json_tokener_parse(
       "{\"content\":[{\"type\":\"thinking\",\"thinking\":\"hmm\",\"signature\":\"s\"},"
       "{\"type\":\"redacted_thinking\",\"data\":\"x\"},"
       "{\"type\":\"text\",\"text\":\"Summary one.\"},"
       "{\"type\":\"text\",\"text\":\"Summary two.\"}]}");
   char *text = llm_claude_content_text(reply);
   TEST_ASSERT_EQUAL_STRING("Summary one.\nSummary two.", text);
   free(text);
   json_object_put(reply);

   reply = json_tokener_parse(
       "{\"content\":[{\"type\":\"thinking\",\"thinking\":\"hmm\",\"signature\":\"s\"}]}");
   text = llm_claude_content_text(reply);
   TEST_ASSERT_EQUAL_STRING("", text); /* no text: the caller fails the call */
   free(text);
   json_object_put(reply);
}

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
   RUN_TEST(test_an_image_that_doesnt_convert_is_a_stand_in);
   RUN_TEST(test_disabled_on_adaptive_only_model_is_lowest_adaptive);
   RUN_TEST(test_disabled_is_sent_where_accepted);
   RUN_TEST(test_adaptive_carries_the_session_effort);
   RUN_TEST(test_openrouter_slug_gets_its_models_rules);
   RUN_TEST(test_budget_model_gets_enabled_with_budget);
   RUN_TEST(test_utility_call_gets_the_cheapest_legal_setting);
   RUN_TEST(test_haiku_5_5_never_gets_a_budget);
   RUN_TEST(test_tool_use_without_thinking_keeps_reasoning);
   RUN_TEST(test_final_answer_replays_its_blocks);
   RUN_TEST(test_results_without_their_call_become_notes);
   RUN_TEST(test_calls_and_results_are_paired);
   RUN_TEST(test_reasoning_only_turn_and_user_text);
   RUN_TEST(test_reply_text_skips_thinking);
   return UNITY_END();
}
