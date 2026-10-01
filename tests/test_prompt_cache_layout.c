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
#include "llm/llm_openai_internal.h"
#include "llm/llm_openai_responses_input.h"
#include "llm/llm_tool_defs.h"
#include "llm/llm_tool_images_render.h"
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
/* The conversation's tools by value (llm_tools_request_tools): its frozen set
 * and the changes not sent in place; a name (an older set) as itself. */
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

/* Whether the model takes images (llm_command_parser.c). */
static int s_vision = 1;
int is_vision_enabled_for_current_llm(void) {
   return s_vision;
}


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
   struct json_object *req = convert_to_claude_format(history, NULL, model, CARRIER, 0, false);
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

/* An image question's images are parts of its own message (built from its
 * stored ids): they go right after its text; its context stays in front. */
static void test_images_follow_the_question(void) {
   const char *msgs[] = {
      PREFIX, "{\"role\":\"user\",\"content\":["
              "{\"type\":\"text\",\"text\":\"[system_time] 09:05\",\"_kind\":\"turn_context\"},"
              "{\"type\":\"text\",\"text\":\"What is this?\"},"
              "{\"type\":\"image_url\",\"image_url\":{\"url\":"
              "\"data:image/png;base64,iVBORw0KGgo=\"}}]}"
   };
   struct json_object *h = history(msgs, 2);
   struct json_object *req = convert_to_claude_format(h, NULL, MID_SYSTEM_MODEL, CARRIER, 0, false);
   struct json_object *messages = field(req, "messages");
   TEST_ASSERT_EQUAL_INT(1, (int)json_object_array_length(messages));
   struct json_object *blocks = field(json_object_array_get_idx(messages, 0), "content");
   TEST_ASSERT_EQUAL_INT(3, (int)json_object_array_length(blocks));
   TEST_ASSERT_EQUAL_STRING("{\"type\":\"text\",\"text\":\"[system_time] 09:05\"}",
                            str(json_object_array_get_idx(blocks, 0)));
   TEST_ASSERT_EQUAL_STRING("{\"type\":\"text\",\"text\":\"What is this?\"}",
                            str(json_object_array_get_idx(blocks, 1)));
   struct json_object *image = json_object_array_get_idx(blocks, 2);
   TEST_ASSERT_EQUAL_STRING("image", json_object_get_string(field(image, "type")));
   TEST_ASSERT_EQUAL_STRING("image/png",
                            json_object_get_string(field(field(image, "source"), "media_type")));
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

/* ---- A conversation's tool changes (llm_tool_defs.h), per provider ---- */

#define TOOL_A "{\"name\":\"a\",\"description\":\"A\",\"parameters\":{\"type\":\"object\"}}"
#define TOOL_B "{\"name\":\"b\",\"description\":\"B\",\"parameters\":{\"type\":\"object\"}}"
static const char *TOOLED_PREFIX = "{\"role\":\"system\",\"content\":\"You are Friday.\","
                                   "\"_kind\":\"prefix\",\"_tools\":[" TOOL_A "]}";
static const char *QUESTION = "{\"role\":\"user\",\"content\":\"Hi\"}";
static const char *ADDED_INLINE =
    "{\"role\":\"system\",\"_kind\":\"tool_change\",\"content\":"
    "\"{\\\"rendered\\\":\\\"inline\\\",\\\"tools\\\":[{\\\"name\\\":\\\"b\\\",\\\"description\\\":"
    "\\\"B\\\",\\\"parameters\\\":{\\\"type\\\":\\\"object\\\"}}]}\"}";
static const char *ANSWER = "{\"role\":\"assistant\",\"content\":\"Hello.\"}";
static const char *NEXT = "{\"role\":\"user\",\"content\":\"And now?\"}";

static struct json_object *render_tools(struct json_object *h, bool inline_tools) {
   struct json_object *req = convert_to_claude_format(h, NULL, MID_SYSTEM_MODEL, CARRIER, 0,
                                                      inline_tools);
   TEST_ASSERT_NOT_NULL(req);
   return req;
}

/* Claude, inline: `tools` stays the frozen set, the change is a system
 * message of tool_addition blocks after the question, and it stays put. */
static void test_claude_sends_a_tool_change_in_place(void) {
   s_tools_on = true;
   const char *m1[] = { TOOLED_PREFIX, QUESTION, ADDED_INLINE };
   const char *m2[] = { TOOLED_PREFIX, QUESTION, ADDED_INLINE, ANSWER, NEXT };
   struct json_object *h1 = history(m1, 3), *h2 = history(m2, 5);
   struct json_object *r1 = render_tools(h1, true), *r2 = render_tools(h2, true);
   TEST_ASSERT_EQUAL_STRING("[{\"name\":\"a\",\"description\":\"A\",\"input_schema\":{\"type\":"
                            "\"object\"},\"cache_control\":{\"type\":\"ephemeral\"}}]",
                            str(field(r1, "tools")));
   TEST_ASSERT_EQUAL_STRING(str(field(r1, "tools")), str(field(r2, "tools")));
   struct json_object *msgs1 = field(r1, "messages");
   TEST_ASSERT_EQUAL_INT(2, (int)json_object_array_length(msgs1));
   TEST_ASSERT_EQUAL_STRING("{\"role\":\"system\",\"content\":[{\"type\":\"tool_addition\","
                            "\"tool\":{\"type\":\"tool_definition\",\"definition\":{\"name\":"
                            "\"b\",\"description\":\"B\",\"input_schema\":{\"type\":\"object\"}}"
                            "}}]}",
                            str(json_object_array_get_idx(msgs1, 1)));
   drop_breakpoints(r1);
   drop_breakpoints(r2);
   struct json_object *msgs2 = field(r2, "messages");
   for (size_t i = 0; i < json_object_array_length(msgs1); i++) {
      TEST_ASSERT_EQUAL_STRING(str(json_object_array_get_idx(msgs1, i)),
                               str(json_object_array_get_idx(msgs2, i)));
   }
   TEST_ASSERT_NULL(strstr(str(r2), "_kind"));
   json_object_put(r1);
   json_object_put(r2);
   json_object_put(h1);
   json_object_put(h2);
   s_tools_on = false;
}

/* Claude elsewhere (another model, another host, a rejected beta): the
 * change folds into `tools`, and no message carries it. */
static void test_claude_folds_a_tool_change_elsewhere(void) {
   s_tools_on = true;
   const char *m[] = { TOOLED_PREFIX, QUESTION, ADDED_INLINE };
   struct json_object *h = history(m, 3);
   struct json_object *req = render_tools(h, false);
   TEST_ASSERT_EQUAL_STRING("[{\"name\":\"a\",\"description\":\"A\",\"input_schema\":{\"type\":"
                            "\"object\"}},{\"name\":\"b\",\"description\":\"B\",\"input_schema\":"
                            "{\"type\":\"object\"},\"cache_control\":{\"type\":\"ephemeral\"}}]",
                            str(field(req, "tools")));
   TEST_ASSERT_EQUAL_INT(1, (int)json_object_array_length(field(req, "messages")));
   TEST_ASSERT_NULL(strstr(str(req), "tool_addition"));
   json_object_put(req);
   json_object_put(h);
   s_tools_on = false;
}

/* Chat completions and Responses: a change is in the request's tools (folded),
 * never a message. */
static void test_openai_folds_a_tool_change(void) {
   const char *m[] = { TOOLED_PREFIX, QUESTION, ADDED_INLINE, ANSWER };
   struct json_object *h = history(m, 4);
   struct json_object *chat = llm_openai_prepare_chat_history(h, "api.openai.com#00", "gpt-5.6");
   TEST_ASSERT_NOT_NULL(chat);
   TEST_ASSERT_EQUAL_INT(3, (int)json_object_array_length(chat));
   TEST_ASSERT_NULL(strstr(str(chat), "tool_change"));
   TEST_ASSERT_NULL(strstr(str(chat), "rendered"));
   struct json_object *input = llm_responses_build_input(h, NULL, NULL, 1, false,
                                                         "api.openai.com#00", "gpt-5.6");
   TEST_ASSERT_NOT_NULL(input);
   TEST_ASSERT_NULL(strstr(str(input), "rendered"));
   struct json_object *defs = llm_tool_defs_for_request(h, false);
   TEST_ASSERT_EQUAL_INT(2, (int)json_object_array_length(defs));
   json_object_put(defs);
   json_object_put(input);
   json_object_put(chat);
   json_object_put(h);
}

/* ---- A tool's images (a camera capture), per provider ---- */

#define PNG_DATA "iVBORw0KGgoAAAANSUhEUgAAAAE="
#define PNG_URI "data:image/png;base64," PNG_DATA
#define IMAGE_PART(id) \
   "{\"type\":\"image_url\",\"image_url\":{\"url\":\"" PNG_URI "\"},\"_image_id\":\"" id "\"}"
#define RESULT_PARTS(id) "[{\"type\":\"text\",\"text\":\"Image captured.\"}," IMAGE_PART(id) "]"

static const char *Q1 = "{\"role\":\"user\",\"content\":\"What is on my desk?\"}";
static const char *CALL1 = "{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"id\":"
                           "\"c1\",\"type\":\"function\",\"function\":{\"name\":\"viewing\","
                           "\"arguments\":\"{}\"}}]}";
/* The result as a reload builds it (a tool row and its images) ... */
static const char *RESULT1 = "{\"role\":\"tool\",\"tool_call_id\":\"c1\",\"content\":" RESULT_PARTS(
    "img_0000000000001") "}";
/* ... and as a live Claude turn holds it. */
static const char *RESULT1_LIVE_CLAUDE =
    "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"c1\","
    "\"content\":" RESULT_PARTS("img_0000000000001") "}]}";
static const char *ANSWER1 = "{\"role\":\"assistant\",\"content\":\"A red mug.\"}";
static const char *Q2 = "{\"role\":\"user\",\"content\":\"And its handle?\"}";
static const char *ANSWER2 = "{\"role\":\"assistant\",\"content\":\"Chipped.\"}";
static const char *Q3 = "{\"role\":\"user\",\"content\":\"Look again.\"}";
static const char *CALL2 = "{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"id\":"
                           "\"c2\",\"type\":\"function\",\"function\":{\"name\":\"viewing\","
                           "\"arguments\":\"{}\"}}]}";
static const char *RESULT2 = "{\"role\":\"tool\",\"tool_call_id\":\"c2\",\"content\":" RESULT_PARTS(
    "img_0000000000002") "}";

/* The tool_result block of the user message at @p i of a Claude request. */
static struct json_object *claude_result(struct json_object *req, int i) {
   struct json_object *msg = json_object_array_get_idx(field(req, "messages"), i);
   return json_object_array_get_idx(field(msg, "content"), 0);
}

/* A capture's image goes in its tool_result, as the image it was (PNG), and
 * every later request carries it byte for byte; a reload (a tool row with its
 * images) renders as the live turn did. */
static void test_tool_image_in_its_result_and_stays_put(void) {
   const char *m1[] = { PREFIX, Q1, CALL1, RESULT1 };
   const char *m2[] = { PREFIX, Q1, CALL1, RESULT1, ANSWER1, Q2 };
   const char *m3[] = { PREFIX, Q1, CALL1, RESULT1, ANSWER1, Q2, ANSWER2, Q3 };
   struct json_object *h1 = history(m1, 4), *h2 = history(m2, 6), *h3 = history(m3, 8);
   struct json_object *r1 = render(h1, MID_SYSTEM_MODEL);
   struct json_object *r2 = render(h2, MID_SYSTEM_MODEL);
   struct json_object *r3 = render(h3, MID_SYSTEM_MODEL);

   struct json_object *result = claude_result(r1, 2);
   TEST_ASSERT_EQUAL_STRING("tool_result", json_object_get_string(field(result, "type")));
   struct json_object *image = json_object_array_get_idx(field(result, "content"), 1);
   TEST_ASSERT_EQUAL_STRING("{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":"
                            "\"image/png\",\"data\":\"" PNG_DATA "\"}}",
                            str(image));
   TEST_ASSERT_NULL(strstr(str(r1), "_image_id"));
   assert_prefix_of(r1, r2);
   assert_prefix_of(r2, r3);

   /* Live = reload. */
   const char *live[] = { PREFIX, Q1, CALL1, RESULT1_LIVE_CLAUDE };
   struct json_object *hl = history(live, 4);
   struct json_object *rl = render(hl, MID_SYSTEM_MODEL);
   drop_breakpoints(rl);
   TEST_ASSERT_EQUAL_STRING(str(claude_result(r1, 2)), str(claude_result(rl, 2)));

   json_object_put(rl);
   json_object_put(hl);
   json_object_put(r1);
   json_object_put(r2);
   json_object_put(r3);
   json_object_put(h1);
   json_object_put(h2);
   json_object_put(h3);
}

/* A second capture leaves the first as it was sent. */
static void test_second_capture_leaves_the_first(void) {
   const char *m1[] = { PREFIX, Q1, CALL1, RESULT1, ANSWER1, Q3 };
   const char *m2[] = { PREFIX, Q1, CALL1, RESULT1, ANSWER1, Q3, CALL2, RESULT2 };
   struct json_object *h1 = history(m1, 6), *h2 = history(m2, 8);
   struct json_object *r1 = render(h1, MID_SYSTEM_MODEL);
   struct json_object *r2 = render(h2, MID_SYSTEM_MODEL);
   assert_prefix_of(r1, r2);
   int images = 0;
   for (const char *p = str(field(r2, "messages")); (p = strstr(p, "\"type\":\"image\"")); p++) {
      images++;
   }
   TEST_ASSERT_EQUAL_INT(2, images);
   json_object_put(r1);
   json_object_put(r2);
   json_object_put(h1);
   json_object_put(h2);
}

/* Chat completions: tool messages carry text; one user message after ALL of
 * a turn's tool messages (parallel calls) carries their images, each call's
 * labelled; the same on every request. */
static void test_chat_image_message_follows_all_tool_messages(void) {
   const char *calls = "{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":["
                       "{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"viewing\","
                       "\"arguments\":\"{}\"}},"
                       "{\"id\":\"c9\",\"type\":\"function\",\"function\":{\"name\":\"weather\","
                       "\"arguments\":\"{}\"}}]}";
   const char *weather = "{\"role\":\"tool\",\"tool_call_id\":\"c9\",\"content\":\"Sunny\"}";
   const char *m1[] = { PREFIX, Q1, calls, RESULT1, weather };
   const char *m2[] = { PREFIX, Q1, calls, RESULT1, weather, ANSWER1, Q2 };
   struct json_object *h1 = history(m1, 5), *h2 = history(m2, 7);
   struct json_object *p1 = llm_openai_prepare_chat_history(h1, "api.openai.com#00", "gpt-5.6");
   struct json_object *p2 = llm_openai_prepare_chat_history(h2, "api.openai.com#00", "gpt-5.6");
   TEST_ASSERT_NOT_NULL(p1);
   TEST_ASSERT_NOT_NULL(p2);
   TEST_ASSERT_EQUAL_INT(6, (int)json_object_array_length(p1));
   TEST_ASSERT_EQUAL_STRING("{\"role\":\"tool\",\"tool_call_id\":\"c1\",\"content\":\"Image "
                            "captured.\"}",
                            str(json_object_array_get_idx(p1, 3)));
   TEST_ASSERT_EQUAL_STRING("{\"role\":\"tool\",\"tool_call_id\":\"c9\",\"content\":\"Sunny\"}",
                            str(json_object_array_get_idx(p1, 4)));
   TEST_ASSERT_EQUAL_STRING("{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":"
                            "\"Images returned by tool call c1:\"},{\"type\":\"image_url\","
                            "\"image_url\":{\"url\":\"" PNG_URI "\"}}]}",
                            str(json_object_array_get_idx(p1, 5)));
   for (size_t i = 0; i < json_object_array_length(p1); i++) {
      TEST_ASSERT_EQUAL_STRING(str(json_object_array_get_idx(p1, i)),
                               str(json_object_array_get_idx(p2, i)));
   }
   TEST_ASSERT_NULL(strstr(str(p2), "_image_id"));
   json_object_put(p1);
   json_object_put(p2);
   json_object_put(h1);
   json_object_put(h2);
}

/* The output item of call @p id in a Responses input. */
static struct json_object *output_of(struct json_object *input, const char *id) {
   for (size_t i = 0; i < json_object_array_length(input); i++) {
      struct json_object *item = json_object_array_get_idx(input, i);
      struct json_object *t = NULL, *c = NULL;
      if (json_object_object_get_ex(item, "type", &t) &&
          strcmp(json_object_get_string(t), "function_call_output") == 0 &&
          json_object_object_get_ex(item, "call_id", &c) &&
          strcmp(json_object_get_string(c), id) == 0) {
         return field(item, "output");
      }
   }
   TEST_FAIL_MESSAGE(id);
   return NULL;
}

/* Responses: the function_call_output carries the image (input_image). */
static void test_responses_output_carries_the_image(void) {
   const char *m[] = { PREFIX, Q1, CALL1, RESULT1 };
   struct json_object *h = history(m, 4);
   struct json_object *input = llm_responses_build_input(h, NULL, NULL, 1, false,
                                                         "api.openai.com#00", "gpt-5.6");
   TEST_ASSERT_NOT_NULL(input);
   TEST_ASSERT_EQUAL_STRING("[{\"type\":\"input_text\",\"text\":\"Image captured.\"},"
                            "{\"type\":\"input_image\",\"image_url\":\"" PNG_URI "\"}]",
                            str(output_of(input, "c1")));
   json_object_put(input);
   json_object_put(h);
}

/* A model that takes no images: one fixed text per image, every request. */
static void test_no_vision_gets_a_fixed_placeholder(void) {
   const char *m[] = { PREFIX, Q1, CALL1, RESULT1 };
   struct json_object *h = history(m, 4);
   s_vision = 0;
   struct json_object *r1 = render(h, MID_SYSTEM_MODEL);
   struct json_object *r2 = render(h, MID_SYSTEM_MODEL);
   s_vision = 1;
   TEST_ASSERT_EQUAL_STRING(str(r1), str(r2));
   TEST_ASSERT_NULL(strstr(str(r1), "\"type\":\"image\""));
   TEST_ASSERT_NULL(strstr(str(r1), PNG_DATA));
   struct json_object *parts = field(claude_result(r1, 2), "content");
   TEST_ASSERT_EQUAL_STRING(LLM_TOOL_IMAGES_NO_VISION_TEXT,
                            json_object_get_string(
                                field(json_object_array_get_idx(parts, 1), "text")));
   /* Responses, from the same no-vision history (llm_openai_responses.c). */
   struct json_object *without = llm_tool_images_without(h);
   struct json_object *input = llm_responses_build_input(without, NULL, NULL, 1, false,
                                                         "api.openai.com#00", "gpt-5.6");
   TEST_ASSERT_EQUAL_STRING("Image captured.\n" LLM_TOOL_IMAGES_NO_VISION_TEXT,
                            json_object_get_string(output_of(input, "c1")));
   /* The history itself keeps its image. */
   TEST_ASSERT_NOT_NULL(strstr(str(h), PNG_DATA));
   json_object_put(input);
   json_object_put(without);
   json_object_put(r1);
   json_object_put(r2);
   json_object_put(h);
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
   RUN_TEST(test_claude_sends_a_tool_change_in_place);
   RUN_TEST(test_claude_folds_a_tool_change_elsewhere);
   RUN_TEST(test_openai_folds_a_tool_change);
   RUN_TEST(test_tool_image_in_its_result_and_stays_put);
   RUN_TEST(test_second_capture_leaves_the_first);
   RUN_TEST(test_chat_image_message_follows_all_tool_messages);
   RUN_TEST(test_responses_output_carries_the_image);
   RUN_TEST(test_no_vision_gets_a_fixed_placeholder);
   const int rc = UNITY_END();
   llm_capabilities_free_registry();
   toml_free(s_models);
   return rc;
}
