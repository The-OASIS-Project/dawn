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
 * Request layout for prompt caching, asserted on the serialized Claude body:
 * the top-level system prompt holds only the frozen prefix; a turn's context
 * sits in front of its question; directions go in place (a system message on
 * models that take one, an operator's note otherwise); the conversation
 * breakpoint is on the last user turn; and each request's messages are a
 * prefix of the next one's, so the cache (and preserved thinking) holds.
 */

#include <json-c/json.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "llm/llm_capabilities.h"
#include "llm/llm_claude_format.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_local_provider.h"
#include "llm/llm_tools.h"
#include "tools/toml.h"
#include "unity.h"

#define CARRIER "api.anthropic.com#0123456789abcdef"
#define MID_SYSTEM_MODEL "claude-opus-5-5"
#define NOTE_MODEL "claude-sonnet-5"

/* ---- Stubs for the converter's config / tool-registry deps ---- */
dawn_config_t g_config;
const char *llm_get_default_claude_model(void) {
   return MID_SYSTEM_MODEL;
}
const char *llm_get_current_thinking_mode(void) {
   return "adaptive";
}
const char *llm_get_current_reasoning_effort(void) {
   return "medium";
}
static bool s_one_off; /* the call is a compaction's summary (llm_cache_monitor.c) */
bool llm_cache_monitor_one_off_call(void) {
   return s_one_off;
}
static bool s_tools_on;
bool llm_tools_enabled(const llm_resolved_config_t *c) {
   (void)c;
   return s_tools_on;
}
bool llm_tools_suppressed(void) {
   return false;
}
int llm_budget_tokens_for_effort(const char *effort) {
   (void)effort;
   return 8192;
}
local_provider_t llm_local_get_provider(void) {
   return LOCAL_PROVIDER_LLAMA_CPP;
}
local_provider_t llm_local_detect_provider(const char *endpoint) {
   (void)endpoint;
   return LOCAL_PROVIDER_LLAMA_CPP;
}
struct json_object *llm_tools_get_claude_format_filtered(bool r) {
   (void)r;
   return NULL;
}
/* The conversation's frozen set, as names only (llm_tools_request_tools). */
struct json_object *llm_tools_request_tools(struct json_object *history,
                                            bool r,
                                            bool claude,
                                            const char **source) {
   (void)r;
   (void)claude;
   if (source)
      *source = "stub";
   struct json_object *names = llm_history_frozen_tools(history);
   if (!names)
      return NULL;
   struct json_object *out = json_object_new_array();
   for (size_t i = 0; i < json_object_array_length(names); i++) {
      struct json_object *tool = json_object_new_object();
      json_object_object_add(tool, "name", json_object_get(json_object_array_get_idx(names, i)));
      json_object_array_add(out, tool);
   }
   return out;
}
int llm_tools_get_enabled_count_filtered(bool r) {
   (void)r;
   return 0;
}
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
void webui_send_error_ex(struct session *s, const char *code, const char *message, int severity) {
   (void)s;
   (void)code;
   (void)message;
   (void)severity;
}

static toml_table_t *s_models;

void setUp(void) {
   g_config.llm.max_tokens = 4096;
}
void tearDown(void) {
}

#define FLAGS (JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE)

static struct json_object *parse(const char *json) {
   struct json_object *obj = json_tokener_parse(json);
   TEST_ASSERT_NOT_NULL_MESSAGE(obj, json);
   return obj;
}

static struct json_object *render(struct json_object *history, const char *model) {
   struct json_object *req = convert_to_claude_format(history, NULL, NULL, NULL, 0, model, CARRIER,
                                                      0);
   TEST_ASSERT_NOT_NULL(req);
   return req;
}

static struct json_object *field(struct json_object *obj, const char *key) {
   struct json_object *v = NULL;
   TEST_ASSERT_TRUE_MESSAGE(json_object_object_get_ex(obj, key, &v), key);
   return v;
}

static const char *str(struct json_object *obj) {
   return json_object_to_json_string_ext(obj, FLAGS);
}

/* Remove every cache_control marker (they move as the conversation grows). */
static void drop_breakpoints(struct json_object *obj) {
   if (json_object_is_type(obj, json_type_object)) {
      json_object_object_del(obj, "cache_control");
      json_object_object_foreach(obj, key, val) {
         (void)key;
         drop_breakpoints(val);
      }
   } else if (json_object_is_type(obj, json_type_array)) {
      for (size_t i = 0; i < json_object_array_length(obj); i++) {
         drop_breakpoints(json_object_array_get_idx(obj, i));
      }
   }
}

/* A turn: its context, its question, and (after it) a direction. */
static const char *PREFIX = "{\"role\":\"system\",\"content\":\"You are Friday.\"}";
static const char *TURN1 =
    "{\"role\":\"user\",\"content\":["
    "{\"type\":\"text\",\"text\":\"[memory] Alex likes tea.\",\"_kind\":\"memory\"},"
    "{\"type\":\"text\",\"text\":\"[system_time] 09:00\",\"_kind\":\"turn_context\"},"
    "{\"type\":\"text\",\"text\":\"Good morning\"}]}";
static const char *DIRECTIVE =
    "{\"role\":\"system\",\"content\":\"Answer briefly: voice.\",\"_kind\":\"directive\"}";
static const char *REPLY1 = "{\"role\":\"assistant\",\"content\":\"Morning, Alex.\"}";
static const char *TURN2 =
    "{\"role\":\"user\",\"content\":["
    "{\"type\":\"text\",\"text\":\"[system_time] 09:05\",\"_kind\":\"turn_context\"},"
    "{\"type\":\"text\",\"text\":\"Any tea left?\"}]}";

static struct json_object *history(const char *const *msgs, int n) {
   struct json_object *h = json_object_new_array();
   for (int i = 0; i < n; i++) {
      json_object_array_add(h, parse(msgs[i]));
   }
   return h;
}

static void test_context_goes_in_front_of_the_question(void) {
   const char *msgs[] = { PREFIX, TURN1 };
   struct json_object *h = history(msgs, 2);
   struct json_object *req = render(h, MID_SYSTEM_MODEL);

   TEST_ASSERT_EQUAL_STRING("[{\"type\":\"text\",\"text\":\"You are "
                            "Friday.\",\"cache_control\":{\"type\":\"ephemeral\"}}]",
                            str(field(req, "system")));
   TEST_ASSERT_EQUAL_STRING("[{\"role\":\"user\",\"content\":["
                            "{\"type\":\"text\",\"text\":\"[memory] Alex likes tea.\"},"
                            "{\"type\":\"text\",\"text\":\"[system_time] 09:00\"},"
                            "{\"type\":\"text\",\"text\":\"Good "
                            "morning\",\"cache_control\":{\"type\":\"ephemeral\"}}]}]",
                            str(field(req, "messages")));
   /* The history's own blocks keep their marks and gain no breakpoint. */
   TEST_ASSERT_NULL(strstr(str(h), "cache_control"));
   TEST_ASSERT_NOT_NULL(strstr(str(h), "_kind"));
   json_object_put(req);
   json_object_put(h);
}

static void test_direction_is_a_system_message_where_taken(void) {
   const char *msgs[] = { PREFIX, TURN1, DIRECTIVE, REPLY1 };
   struct json_object *h = history(msgs, 4);
   struct json_object *req = render(h, MID_SYSTEM_MODEL);
   struct json_object *messages = field(req, "messages");
   TEST_ASSERT_EQUAL_INT(3, (int)json_object_array_length(messages));
   TEST_ASSERT_EQUAL_STRING("{\"role\":\"system\",\"content\":\"Answer briefly: voice.\"}",
                            str(json_object_array_get_idx(messages, 1)));
   /* Never in the top-level system prompt. */
   TEST_ASSERT_NULL(strstr(str(field(req, "system")), "Answer briefly"));
   json_object_put(req);
   json_object_put(h);
}

static void test_direction_is_a_note_elsewhere(void) {
   const char *msgs[] = { PREFIX, TURN1, DIRECTIVE, REPLY1 };
   struct json_object *h = history(msgs, 4);
   struct json_object *req = render(h, NOTE_MODEL);
   struct json_object *messages = field(req, "messages");
   TEST_ASSERT_EQUAL_INT(2, (int)json_object_array_length(messages));
   struct json_object *blocks = field(json_object_array_get_idx(messages, 0), "content");
   TEST_ASSERT_EQUAL_INT(4, (int)json_object_array_length(blocks));
   /* The last user turn: it carries the conversation breakpoint too. */
   TEST_ASSERT_EQUAL_STRING("{\"type\":\"text\",\"text\":\"[Operator note] Answer briefly: "
                            "voice.\",\"cache_control\":{\"type\":\"ephemeral\"}}",
                            str(json_object_array_get_idx(blocks, 3)));
   TEST_ASSERT_NULL(strstr(str(field(req, "system")), "Answer briefly"));
   json_object_put(req);
   json_object_put(h);
}

/* Two directions in a row are one system message (each must be followed by
 * an assistant turn). */
static void test_directions_in_a_row_are_one_message(void) {
   const char *instruction =
       "{\"role\":\"system\",\"content\":\"Persona changed.\",\"_kind\":\"instruction\"}";
   const char *msgs[] = { PREFIX, TURN1, DIRECTIVE, instruction, REPLY1 };
   struct json_object *h = history(msgs, 5);
   struct json_object *req = render(h, MID_SYSTEM_MODEL);
   struct json_object *messages = field(req, "messages");
   TEST_ASSERT_EQUAL_INT(3, (int)json_object_array_length(messages));
   TEST_ASSERT_EQUAL_STRING(
       "{\"role\":\"system\",\"content\":\"Answer briefly: voice.\\n\\nPersona changed.\"}",
       str(json_object_array_get_idx(messages, 1)));
   json_object_put(req);
   json_object_put(h);
}

/* Each request's messages, breakpoints aside, begin with the previous one's:
 * nothing already sent is rendered differently later. */
static void assert_prefix_of(struct json_object *earlier, struct json_object *later) {
   drop_breakpoints(earlier);
   drop_breakpoints(later);
   TEST_ASSERT_EQUAL_STRING(str(field(earlier, "system")), str(field(later, "system")));
   struct json_object *a = field(earlier, "messages");
   struct json_object *b = field(later, "messages");
   TEST_ASSERT_TRUE(json_object_array_length(a) <= json_object_array_length(b));
   for (size_t i = 0; i < json_object_array_length(a); i++) {
      TEST_ASSERT_EQUAL_STRING(str(json_object_array_get_idx(a, i)),
                               str(json_object_array_get_idx(b, i)));
   }
}

static void check_turns_stay_put(const char *model) {
   const char *first[] = { PREFIX, TURN1, DIRECTIVE };
   const char *second[] = { PREFIX, TURN1, DIRECTIVE, REPLY1, TURN2 };
   struct json_object *h1 = history(first, 3);
   struct json_object *h2 = history(second, 5);
   struct json_object *r1 = render(h1, model);
   struct json_object *r2 = render(h2, model);
   assert_prefix_of(r1, r2);
   json_object_put(r1);
   json_object_put(r2);
   json_object_put(h1);
   json_object_put(h2);
}

/* Plain questions (no context): the breakpoint never changes how a turn reads. */
static void test_plain_questions_stay_put(void) {
   const char *first[] = { PREFIX, "{\"role\":\"user\",\"content\":\"Hi\"}" };
   const char *second[] = { PREFIX, "{\"role\":\"user\",\"content\":\"Hi\"}", REPLY1,
                            "{\"role\":\"user\",\"content\":\"Bye\"}" };
   struct json_object *h1 = history(first, 2);
   struct json_object *h2 = history(second, 4);
   struct json_object *r1 = render(h1, MID_SYSTEM_MODEL);
   struct json_object *r2 = render(h2, MID_SYSTEM_MODEL);
   assert_prefix_of(r1, r2);
   json_object_put(r1);
   json_object_put(r2);
   json_object_put(h1);
   json_object_put(h2);
}

static void test_turns_stay_put_with_system_messages(void) {
   check_turns_stay_put(MID_SYSTEM_MODEL);
}

static void test_turns_stay_put_with_notes(void) {
   check_turns_stay_put(NOTE_MODEL);
}

/* The breakpoint follows the conversation: on the newest tool results in a
 * tool round. */
static void test_breakpoint_on_the_newest_results(void) {
   const char *msgs[] = {
      PREFIX,
      TURN2,
      "{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"id\":\"c1\",\"type\":"
      "\"function\",\"function\":{\"name\":\"weather\",\"arguments\":\"{}\"}}]}",
      "{\"role\":\"tool\",\"tool_call_id\":\"c1\",\"content\":\"Sunny\"}",
   };
   struct json_object *h = history(msgs, 4);
   struct json_object *req = render(h, MID_SYSTEM_MODEL);
   struct json_object *messages = field(req, "messages");
   const char *wire = str(messages);
   /* One conversation breakpoint, on the result. */
   const char *mark = strstr(wire, "cache_control");
   TEST_ASSERT_NOT_NULL(mark);
   TEST_ASSERT_NULL(strstr(mark + 1, "cache_control"));
   TEST_ASSERT_EQUAL_STRING(
       "{\"type\":\"tool_result\",\"tool_use_id\":\"c1\",\"content\":\"Sunny\","
       "\"cache_control\":{\"type\":\"ephemeral\"}}",
       str(json_object_array_get_idx(field(json_object_array_get_idx(messages, 2), "content"), 0)));
   json_object_put(req);
   json_object_put(h);
}

/* A request sent once and never again (a compaction's summary) has no
 * conversation breakpoint: its cache write would only cost. */
static void test_a_one_off_request_has_no_breakpoint(void) {
   const char *msgs[] = { PREFIX, TURN2 };
   struct json_object *h = history(msgs, 2);
   s_one_off = true;
   struct json_object *req = render(h, MID_SYSTEM_MODEL);
   s_one_off = false;
   TEST_ASSERT_NULL(strstr(str(field(req, "messages")), "cache_control"));
   json_object_put(req);
   json_object_put(h);
}

/* A reply that was never saved leaves a direction between two questions: it
 * becomes a note in the first, which takes in the second. */
static void test_direction_before_a_question_becomes_a_note(void) {
   const char *msgs[] = { PREFIX, TURN1, DIRECTIVE, TURN2 };
   struct json_object *h = history(msgs, 4);
   struct json_object *req = render(h, MID_SYSTEM_MODEL);
   struct json_object *messages = field(req, "messages");
   TEST_ASSERT_EQUAL_INT(1, (int)json_object_array_length(messages));
   const char *wire = str(messages);
   const char *note = strstr(wire, "[Operator note] Answer briefly: voice.");
   TEST_ASSERT_NOT_NULL(note);
   TEST_ASSERT_NOT_NULL(strstr(note, "Any tea left?"));
   json_object_put(req);
   json_object_put(h);
}

/* Images go right after the question; its context stays in front. */
static void test_images_follow_the_question(void) {
   const char *msgs[] = { PREFIX, TURN2 };
   struct json_object *h = history(msgs, 2);
   const char *image = "iVBORw0KGgo=";
   const char *images[] = { image };
   const size_t sizes[] = { strlen(image) };
   struct json_object *req = convert_to_claude_format(h, "What is this?", images, sizes, 1,
                                                      MID_SYSTEM_MODEL, CARRIER, 0);
   struct json_object *messages = field(req, "messages");
   TEST_ASSERT_EQUAL_INT(1, (int)json_object_array_length(messages));
   struct json_object *blocks = field(json_object_array_get_idx(messages, 0), "content");
   TEST_ASSERT_EQUAL_INT(3, (int)json_object_array_length(blocks));
   TEST_ASSERT_EQUAL_STRING("{\"type\":\"text\",\"text\":\"[system_time] 09:05\"}",
                            str(json_object_array_get_idx(blocks, 0)));
   TEST_ASSERT_EQUAL_STRING("{\"type\":\"text\",\"text\":\"What is this?\"}",
                            str(json_object_array_get_idx(blocks, 1)));
   TEST_ASSERT_EQUAL_STRING("image", json_object_get_string(
                                         field(json_object_array_get_idx(blocks, 2), "type")));
   json_object_put(req);
   json_object_put(h);
}

/* A conversation advertises its own tool set, the same on every request, and
 * the set's key never goes on the wire. */
static void test_the_conversations_tool_set(void) {
   s_tools_on = true;
   const char *msgs[] = { "{\"role\":\"system\",\"content\":\"You are Friday.\",\"_kind\":"
                          "\"prefix\",\"_tools\":[\"weather\",\"memory\"]}",
                          "{\"role\":\"user\",\"content\":\"Hi\"}" };
   struct json_object *h = history(msgs, 2);
   struct json_object *req = render(h, MID_SYSTEM_MODEL);
   TEST_ASSERT_EQUAL_STRING("[{\"name\":\"weather\"},{\"name\":\"memory\",\"cache_control\":{"
                            "\"type\":\"ephemeral\"}}]",
                            str(field(req, "tools")));
   TEST_ASSERT_NULL(strstr(str(req), "_tools"));
   TEST_ASSERT_NULL(strstr(str(req), "_kind"));
   json_object_put(req);
   json_object_put(h);
   s_tools_on = false;
}

int main(void) {
   char err[256];
   FILE *f = fopen(MODELS_TOML_PATH, "r");
   s_models = f ? toml_parse_file(f, err, sizeof(err)) : NULL;
   if (f) {
      fclose(f);
   }
   llm_capabilities_load_registry(s_models);
   llm_capabilities_load_mid_system(s_models);
   UNITY_BEGIN();
   RUN_TEST(test_context_goes_in_front_of_the_question);
   RUN_TEST(test_a_one_off_request_has_no_breakpoint);
   RUN_TEST(test_direction_is_a_system_message_where_taken);
   RUN_TEST(test_direction_is_a_note_elsewhere);
   RUN_TEST(test_directions_in_a_row_are_one_message);
   RUN_TEST(test_plain_questions_stay_put);
   RUN_TEST(test_turns_stay_put_with_system_messages);
   RUN_TEST(test_turns_stay_put_with_notes);
   RUN_TEST(test_breakpoint_on_the_newest_results);
   RUN_TEST(test_direction_before_a_question_becomes_a_note);
   RUN_TEST(test_images_follow_the_question);
   RUN_TEST(test_the_conversations_tool_set);
   const int rc = UNITY_END();
   llm_capabilities_free_registry();
   toml_free(s_models);
   return rc;
}
