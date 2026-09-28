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
 * Unit tests for the OpenAI Responses prompt-cache layout: the stable/volatile
 * split and the "volatile as a user item immediately before the current question"
 * repositioning that makes [instructions][tools][history] a cacheable prefix.
 * See docs/RESPONSES_CACHE_REORDER_PLAN.md.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm_openai_responses_input.h"
#include "llm/llm_turn_blocks.h"
#include "unity.h"

/* The endpoint the requests go to (the carrier of its reasoning). */
#define HOST "api.openai.com"

void setUp(void) {
}
void tearDown(void) {
}

/* ---- helpers -------------------------------------------------------------- */

static struct json_object *msg(const char *role, const char *content) {
   struct json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string(role));
   json_object_object_add(m, "content", json_object_new_string(content));
   return m;
}

/* Role of input item i. */
static const char *item_role(struct json_object *input, int i) {
   struct json_object *it = json_object_array_get_idx(input, i);
   struct json_object *r;
   return (it && json_object_object_get_ex(it, "role", &r)) ? json_object_get_string(r) : NULL;
}

/* First input_text part text of input item i (NULL if none). */
static const char *item_text(struct json_object *input, int i) {
   struct json_object *it = json_object_array_get_idx(input, i);
   struct json_object *content, *part, *type, *text;
   if (!it || !json_object_object_get_ex(it, "content", &content))
      return NULL;
   if (json_object_get_type(content) != json_type_array || json_object_array_length(content) < 1)
      return NULL;
   part = json_object_array_get_idx(content, 0);
   if (json_object_object_get_ex(part, "type", &type) &&
       strcmp(json_object_get_string(type), "input_text") == 0 &&
       json_object_object_get_ex(part, "text", &text))
      return json_object_get_string(text);
   return NULL;
}

/* Index of the last user-role item, or -1. */
static int last_user_index(struct json_object *input) {
   int n = json_object_array_length(input);
   for (int i = n - 1; i >= 0; i--) {
      const char *r = item_role(input, i);
      if (r && strcmp(r, "user") == 0)
         return i;
   }
   return -1;
}

/* True if input item i has an input_image content part. */
static bool item_has_image(struct json_object *input, int i) {
   struct json_object *it = json_object_array_get_idx(input, i);
   struct json_object *content;
   if (!it || !json_object_object_get_ex(it, "content", &content) ||
       json_object_get_type(content) != json_type_array)
      return false;
   int n = json_object_array_length(content);
   for (int k = 0; k < n; k++) {
      struct json_object *part = json_object_array_get_idx(content, k);
      struct json_object *type;
      if (json_object_object_get_ex(part, "type", &type) &&
          strcmp(json_object_get_string(type), "input_image") == 0)
         return true;
   }
   return false;
}

/* ---- extraction (stable vs volatile, leading-run keyed) -------------------- */

void test_extract_stable_and_volatile(void) {
   struct json_object *h = json_object_new_array();
   json_object_array_add(h, msg("system", "STABLE"));
   json_object_array_add(h, msg("system", "VOLATILE"));
   json_object_array_add(h, msg("user", "Q1"));

   TEST_ASSERT_EQUAL_INT(2, llm_responses_count_leading_system_run(h));
   char *stable = llm_responses_extract_stable_instructions(h);
   char *vol = llm_responses_extract_volatile_context(h);
   TEST_ASSERT_EQUAL_STRING("STABLE", stable);
   TEST_ASSERT_EQUAL_STRING("VOLATILE", vol);
   free(stable);
   free(vol);
   json_object_put(h);
}

void test_single_system_has_no_volatile(void) {
   struct json_object *h = json_object_new_array();
   json_object_array_add(h, msg("system", "STABLE"));
   json_object_array_add(h, msg("user", "Q1"));

   TEST_ASSERT_EQUAL_INT(1, llm_responses_count_leading_system_run(h));
   char *stable = llm_responses_extract_stable_instructions(h);
   TEST_ASSERT_EQUAL_STRING("STABLE", stable);
   TEST_ASSERT_NULL(llm_responses_extract_volatile_context(h));
   free(stable);
   json_object_put(h);
}

/* ---- volatile is a user item immediately before the current question ------ */

void test_volatile_before_question_in_history(void) {
   /* Question already in history (tool-loop iter >0 / question pre-added). */
   struct json_object *h = json_object_new_array();
   json_object_array_add(h, msg("system", "STABLE"));
   json_object_array_add(h, msg("system", "VOLATILE"));
   json_object_array_add(h, msg("user", "Q1"));
   json_object_array_add(h, msg("assistant", "A1"));
   json_object_array_add(h, msg("user", "Q2"));

   struct json_object *in = llm_responses_build_input(h, "", NULL, NULL, 0, "VOLATILE", 2, true,
                                                      HOST, "m");
   TEST_ASSERT_NOT_NULL(in);

   /* No system message from the leading run leaks into input. */
   int n = json_object_array_length(in);
   for (int i = 0; i < n; i++) {
      const char *r = item_role(in, i);
      TEST_ASSERT_TRUE(r == NULL || strcmp(r, "system") != 0);
   }
   /* Volatile sits immediately before the last user item (Q2). */
   int lu = last_user_index(in);
   TEST_ASSERT_TRUE(lu >= 1);
   TEST_ASSERT_EQUAL_STRING("Q2", item_text(in, lu));
   TEST_ASSERT_EQUAL_STRING("user", item_role(in, lu - 1));
   TEST_ASSERT_EQUAL_STRING("VOLATILE", item_text(in, lu - 1));

   json_object_put(in);
   json_object_put(h);
}

void test_volatile_before_question_via_input_text(void) {
   /* Question arrives via input_text (tool-loop iter 0). */
   struct json_object *h = json_object_new_array();
   json_object_array_add(h, msg("system", "STABLE"));
   json_object_array_add(h, msg("system", "VOLATILE"));
   json_object_array_add(h, msg("user", "Q1"));
   json_object_array_add(h, msg("assistant", "A1"));

   struct json_object *in = llm_responses_build_input(h, "Q2", NULL, NULL, 0, "VOLATILE", 2, true,
                                                      HOST, "m");
   int lu = last_user_index(in);
   TEST_ASSERT_EQUAL_STRING("Q2", item_text(in, lu));
   TEST_ASSERT_EQUAL_STRING("VOLATILE", item_text(in, lu - 1));
   TEST_ASSERT_EQUAL_STRING("user", item_role(in, lu - 1));

   json_object_put(in);
   json_object_put(h);
}

/* ---- mid-history broadcast (3 system messages): emitted inline, not swept -- */

void test_mid_history_broadcast_emitted_inline(void) {
   struct json_object *h = json_object_new_array();
   json_object_array_add(h, msg("system", "STABLE"));
   json_object_array_add(h, msg("system", "VOLATILE"));
   json_object_array_add(h, msg("user", "Q1"));
   json_object_array_add(h, msg("system", "BROADCAST")); /* incoming-call notice */
   json_object_array_add(h, msg("assistant", "A1"));
   json_object_array_add(h, msg("user", "Q2"));

   /* Leading run is only the first two; broadcast is NOT swept into volatile. */
   TEST_ASSERT_EQUAL_INT(2, llm_responses_count_leading_system_run(h));
   char *vol = llm_responses_extract_volatile_context(h);
   TEST_ASSERT_EQUAL_STRING("VOLATILE", vol);
   free(vol);

   struct json_object *in = llm_responses_build_input(h, "", NULL, NULL, 0, "VOLATILE", 2, true,
                                                      HOST, "m");

   /* The broadcast survives inline as a system item (not dropped). */
   int n = json_object_array_length(in);
   bool found_broadcast = false;
   for (int i = 0; i < n; i++) {
      const char *r = item_role(in, i);
      if (r && strcmp(r, "system") == 0 && item_text(in, i) &&
          strcmp(item_text(in, i), "BROADCAST") == 0)
         found_broadcast = true;
   }
   TEST_ASSERT_TRUE(found_broadcast);

   /* Volatile still lands immediately before the current question. */
   int lu = last_user_index(in);
   TEST_ASSERT_EQUAL_STRING("Q2", item_text(in, lu));
   TEST_ASSERT_EQUAL_STRING("VOLATILE", item_text(in, lu - 1));

   json_object_put(in);
   json_object_put(h);
}

/* ---- vision images stay on the question item, not the volatile item -------- */

void test_vision_stays_on_question(void) {
   struct json_object *h = json_object_new_array();
   json_object_array_add(h, msg("system", "STABLE"));
   json_object_array_add(h, msg("system", "VOLATILE"));
   json_object_array_add(h, msg("user", "Q1"));

   const char *imgs[] = { "BASE64IMG" };
   const size_t sizes[] = { 9 };
   struct json_object *in = llm_responses_build_input(h, "Q2", imgs, sizes, 1, "VOLATILE", 2, true,
                                                      HOST, "m");

   int lu = last_user_index(in); /* the question Q2 */
   TEST_ASSERT_EQUAL_STRING("Q2", item_text(in, lu));
   TEST_ASSERT_TRUE(item_has_image(in, lu));      /* image on the question */
   TEST_ASSERT_FALSE(item_has_image(in, lu - 1)); /* not on the volatile item */
   TEST_ASSERT_EQUAL_STRING("VOLATILE", item_text(in, lu - 1));

   json_object_put(in);
   json_object_put(h);
}


/* ---- assistant turns and Claude parts ------------------------------------- */

/* Type of input item i ("message", "reasoning", "function_call", ...). */
static const char *item_type(struct json_object *input, int i) {
   struct json_object *it = json_object_array_get_idx(input, i);
   struct json_object *t;
   return (it && json_object_object_get_ex(it, "type", &t)) ? json_object_get_string(t) : NULL;
}

static const char *item_str(struct json_object *input, int i, const char *key) {
   struct json_object *it = json_object_array_get_idx(input, i);
   struct json_object *v;
   return (it && json_object_object_get_ex(it, key, &v)) ? json_object_get_string(v) : NULL;
}

/* An assistant turn's blocks render in order: OpenAI's own reasoning items
 * verbatim, text as an assistant message, the tool call under its call_id;
 * another vendor's reasoning is left out. */
static void test_assistant_blocks_in_order(void) {
   struct json_object *blocks = llm_turn_blocks_new();
   struct json_object *r = json_object_new_object();
   json_object_object_add(r, "type", json_object_new_string("reasoning"));
   json_object_object_add(r, "id", json_object_new_string("rs_1"));
   json_object_object_add(r, "encrypted_content", json_object_new_string("ENC"));
   llm_turn_blocks_add_reasoning(blocks, HOST, LLM_FORMAT_OPENAI, "m", r);
   struct json_object *t = json_object_new_object();
   json_object_object_add(t, "type", json_object_new_string("thinking"));
   json_object_object_add(t, "signature", json_object_new_string("SIG"));
   llm_turn_blocks_add_reasoning(blocks, LLM_CARRIER_ANTHROPIC, LLM_FORMAT_ANTHROPIC, "c", t);
   llm_turn_blocks_add_text(blocks, "Checking.");
   llm_turn_blocks_add_tool_call(blocks, "call_7", "weather", "{\"city\":\"x\"}");

   struct json_object *history = json_object_new_array();
   json_object_array_add(history, msg("user", "weather?"));
   struct json_object *a = msg("assistant", "Checking.");
   json_object_object_add(a, LLM_TURN_BLOCKS_KEY, blocks);
   json_object_array_add(history, a);
   struct json_object *tool = msg("tool", "sunny");
   json_object_object_add(tool, "tool_call_id", json_object_new_string("call_7"));
   json_object_array_add(history, tool);

   struct json_object *input = llm_responses_build_input(history, "", NULL, NULL, 0, NULL, 0, false,
                                                         HOST, "m");
   TEST_ASSERT_EQUAL_INT(5, json_object_array_length(input));
   TEST_ASSERT_EQUAL_STRING("reasoning", item_type(input, 1));
   TEST_ASSERT_EQUAL_STRING("ENC", item_str(input, 1, "encrypted_content"));
   TEST_ASSERT_EQUAL_STRING("message", item_type(input, 2));
   TEST_ASSERT_EQUAL_STRING("assistant", item_role(input, 2));
   TEST_ASSERT_EQUAL_STRING("function_call", item_type(input, 3));
   TEST_ASSERT_EQUAL_STRING("call_7", item_str(input, 3, "call_id"));
   TEST_ASSERT_EQUAL_STRING("function_call_output", item_type(input, 4));
   TEST_ASSERT_NULL(strstr(json_object_to_json_string(input), "SIG"));
   json_object_put(input);
   json_object_put(history);
}

/* A Claude-shaped history (no blocks): the assistant's text and tool_use become
 * a message and a function call (never its array serialized as text, never its
 * thinking); the user's tool_result becomes function_call_output. */
static void test_claude_shaped_history(void) {
   struct json_object *history = json_object_new_array();
   json_object_array_add(history, msg("user", "weather?"));
   struct json_object *a = json_object_new_object();
   json_object_object_add(a, "role", json_object_new_string("assistant"));
   struct json_object *content = json_object_new_array();
   struct json_object *th = json_object_new_object();
   json_object_object_add(th, "type", json_object_new_string("thinking"));
   json_object_object_add(th, "thinking", json_object_new_string("secret plan"));
   json_object_object_add(th, "signature", json_object_new_string("SIG"));
   json_object_array_add(content, th);
   struct json_object *use = json_object_new_object();
   json_object_object_add(use, "type", json_object_new_string("tool_use"));
   json_object_object_add(use, "id", json_object_new_string("toolu_1"));
   json_object_object_add(use, "name", json_object_new_string("weather"));
   json_object_object_add(use, "input", json_object_new_object());
   json_object_array_add(content, use);
   json_object_object_add(a, "content", content);
   json_object_array_add(history, a);
   struct json_object *u = json_object_new_object();
   json_object_object_add(u, "role", json_object_new_string("user"));
   struct json_object *uc = json_object_new_array();
   struct json_object *res = json_object_new_object();
   json_object_object_add(res, "type", json_object_new_string("tool_result"));
   json_object_object_add(res, "tool_use_id", json_object_new_string("toolu_1"));
   json_object_object_add(res, "content", json_object_new_string("sunny"));
   json_object_array_add(uc, res);
   json_object_object_add(u, "content", uc);
   json_object_array_add(history, u);

   struct json_object *input = llm_responses_build_input(history, "and tomorrow?", NULL, NULL, 0,
                                                         NULL, 0, false, HOST, "m");
   TEST_ASSERT_EQUAL_INT(4, json_object_array_length(input));
   TEST_ASSERT_EQUAL_STRING("function_call", item_type(input, 1));
   TEST_ASSERT_EQUAL_STRING("toolu_1", item_str(input, 1, "call_id"));
   TEST_ASSERT_EQUAL_STRING("function_call_output", item_type(input, 2));
   TEST_ASSERT_EQUAL_STRING("sunny", item_str(input, 2, "output"));
   TEST_ASSERT_EQUAL_STRING("and tomorrow?", item_text(input, 3));
   const char *wire = json_object_to_json_string(input);
   TEST_ASSERT_NULL(strstr(wire, "secret plan"));
   TEST_ASSERT_NULL(strstr(wire, "SIG"));
   json_object_put(input);
   json_object_put(history);
}

/* A Claude image part becomes an input_image data URL. */
static void test_claude_image_part(void) {
   struct json_object *history = json_object_new_array();
   struct json_object *u = json_object_new_object();
   json_object_object_add(u, "role", json_object_new_string("user"));
   struct json_object *uc = json_object_new_array();
   struct json_object *img = json_object_new_object();
   json_object_object_add(img, "type", json_object_new_string("image"));
   struct json_object *src = json_object_new_object();
   json_object_object_add(src, "type", json_object_new_string("base64"));
   json_object_object_add(src, "media_type", json_object_new_string("image/png"));
   json_object_object_add(src, "data", json_object_new_string("QUJD"));
   json_object_object_add(img, "source", src);
   json_object_array_add(uc, img);
   json_object_object_add(u, "content", uc);
   json_object_array_add(history, u);

   struct json_object *input = llm_responses_build_input(history, "", NULL, NULL, 0, NULL, 0, false,
                                                         HOST, "m");
   TEST_ASSERT_TRUE(item_has_image(input, 0));
   TEST_ASSERT_NOT_NULL(strstr(json_object_to_json_string(input), "data:image\\/png;base64,QUJD"));
   json_object_put(input);
   json_object_put(history);
}


/* Reasoning from another endpoint or model isn't replayed (it's encrypted for
 * the one that produced it); a call without its output, or an output without
 * its call, isn't sent. */
static void test_reasoning_binding_and_pairing(void) {
   struct json_object *blocks = llm_turn_blocks_new();
   const char *sources[][2] = { { HOST, "m" }, { "other.example", "m" }, { HOST, "m2" } };
   for (int i = 0; i < 3; i++) {
      struct json_object *r = json_object_new_object();
      json_object_object_add(r, "type", json_object_new_string("reasoning"));
      char enc[16];
      snprintf(enc, sizeof(enc), "ENC%d", i);
      json_object_object_add(r, "encrypted_content", json_object_new_string(enc));
      llm_turn_blocks_add_reasoning(blocks, sources[i][0], LLM_FORMAT_OPENAI, sources[i][1], r);
   }
   llm_turn_blocks_add_tool_call(blocks, "call_1", "a", "{}");
   llm_turn_blocks_add_tool_call(blocks, "call_2", "b", "{}"); /* never answered */

   struct json_object *history = json_object_new_array();
   json_object_array_add(history, msg("user", "go"));
   struct json_object *a = msg("assistant", "");
   json_object_object_add(a, LLM_TURN_BLOCKS_KEY, blocks);
   json_object_array_add(history, a);
   struct json_object *tool = msg("tool", "done");
   json_object_object_add(tool, "tool_call_id", json_object_new_string("call_1"));
   json_object_array_add(history, tool);
   struct json_object *stray = msg("tool", "lost");
   json_object_object_add(stray, "tool_call_id", json_object_new_string("call_9"));
   json_object_array_add(history, stray);

   struct json_object *input = llm_responses_build_input(history, "", NULL, NULL, 0, NULL, 0, false,
                                                         HOST, "m");
   const char *wire = json_object_to_json_string(input);
   TEST_ASSERT_NOT_NULL(strstr(wire, "ENC0"));
   TEST_ASSERT_NULL(strstr(wire, "ENC1"));
   TEST_ASSERT_NULL(strstr(wire, "ENC2"));
   TEST_ASSERT_NOT_NULL(strstr(wire, "call_1"));
   TEST_ASSERT_NULL(strstr(wire, "call_2"));
   TEST_ASSERT_NULL(strstr(wire, "call_9"));
   json_object_put(input);
   json_object_put(history);
}

/* A large Claude image converts whole; an unknown image type is left out. */
static void test_large_and_unknown_images(void) {
   const size_t big = 300 * 1024;
   char *data = malloc(big + 1);
   TEST_ASSERT_NOT_NULL(data);
   memset(data, 'A', big);
   data[big] = '\0';
   const char *types[] = { "image/png", "image/svg+xml" };
   for (int t = 0; t < 2; t++) {
      struct json_object *history = json_object_new_array();
      struct json_object *u = json_object_new_object();
      json_object_object_add(u, "role", json_object_new_string("user"));
      struct json_object *uc = json_object_new_array();
      struct json_object *img = json_object_new_object();
      json_object_object_add(img, "type", json_object_new_string("image"));
      struct json_object *src = json_object_new_object();
      json_object_object_add(src, "type", json_object_new_string("base64"));
      json_object_object_add(src, "media_type", json_object_new_string(types[t]));
      json_object_object_add(src, "data", json_object_new_string(data));
      json_object_object_add(img, "source", src);
      json_object_array_add(uc, img);
      struct json_object *txt = json_object_new_object();
      json_object_object_add(txt, "type", json_object_new_string("text"));
      json_object_object_add(txt, "text", json_object_new_string("look"));
      json_object_array_add(uc, txt);
      json_object_object_add(u, "content", uc);
      json_object_array_add(history, u);
      struct json_object *input = llm_responses_build_input(history, "", NULL, NULL, 0, NULL, 0,
                                                            false, HOST, "m");
      if (t == 0) {
         TEST_ASSERT_TRUE(item_has_image(input, 0));
         TEST_ASSERT_TRUE(strlen(json_object_to_json_string(input)) > big);
      } else {
         TEST_ASSERT_FALSE(item_has_image(input, 0));
      }
      json_object_put(input);
      json_object_put(history);
   }
   free(data);
}


/* Servers that number calls per turn reuse ids: an unanswered "call_0" doesn't
 * borrow a later turn's output.  And dropping it never costs the earlier
 * question. */
static void test_reused_ids_and_questions(void) {
   struct json_object *history = json_object_new_array();
   json_object_array_add(history, msg("user", "Q1"));
   struct json_object *a1 = msg("assistant", "");
   struct json_object *b1 = llm_turn_blocks_new();
   llm_turn_blocks_add_tool_call(b1, "call_0", "a", "{}"); /* never answered */
   json_object_object_add(a1, LLM_TURN_BLOCKS_KEY, b1);
   json_object_array_add(history, a1);
   json_object_array_add(history, msg("user", "Q2"));
   struct json_object *a2 = msg("assistant", "");
   struct json_object *b2 = llm_turn_blocks_new();
   llm_turn_blocks_add_tool_call(b2, "call_0", "b", "{}");
   json_object_object_add(a2, LLM_TURN_BLOCKS_KEY, b2);
   json_object_array_add(history, a2);
   struct json_object *out = msg("tool", "ok");
   json_object_object_add(out, "tool_call_id", json_object_new_string("call_0"));
   json_object_array_add(history, out);

   struct json_object *input = llm_responses_build_input(history, "Q3", NULL, NULL, 0, NULL, 0,
                                                         false, HOST, "m");
   /* Q1, Q2, the second call and its output, Q3: the first call is gone. */
   TEST_ASSERT_EQUAL_INT(5, json_object_array_length(input));
   TEST_ASSERT_EQUAL_STRING("Q1", item_text(input, 0));
   TEST_ASSERT_EQUAL_STRING("Q2", item_text(input, 1));
   TEST_ASSERT_EQUAL_STRING("b", item_str(input, 2, "name"));
   TEST_ASSERT_EQUAL_STRING("Q3", item_text(input, 4));
   json_object_put(input);

   /* An unanswered call at the end: the new question is added, not swapped in. */
   json_object_array_del_idx(history, 2, 3); /* [Q1, unanswered call] */
   input = llm_responses_build_input(history, "Q3", NULL, NULL, 0, NULL, 0, false, HOST, "m");
   TEST_ASSERT_EQUAL_INT(2, json_object_array_length(input));
   TEST_ASSERT_EQUAL_STRING("Q1", item_text(input, 0));
   TEST_ASSERT_EQUAL_STRING("Q3", item_text(input, 1));
   json_object_put(input);
   json_object_put(history);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_extract_stable_and_volatile);
   RUN_TEST(test_single_system_has_no_volatile);
   RUN_TEST(test_volatile_before_question_in_history);
   RUN_TEST(test_volatile_before_question_via_input_text);
   RUN_TEST(test_mid_history_broadcast_emitted_inline);
   RUN_TEST(test_vision_stays_on_question);
   RUN_TEST(test_assistant_blocks_in_order);
   RUN_TEST(test_claude_shaped_history);
   RUN_TEST(test_claude_image_part);
   RUN_TEST(test_reasoning_binding_and_pairing);
   RUN_TEST(test_large_and_unknown_images);
   RUN_TEST(test_reused_ids_and_questions);
   return UNITY_END();
}
