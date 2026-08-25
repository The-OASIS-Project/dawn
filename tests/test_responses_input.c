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
#include <stdlib.h>
#include <string.h>

#include "llm/llm_openai_responses_input.h"
#include "unity.h"

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

   struct json_object *in = llm_responses_build_input(h, "", NULL, NULL, 0, "VOLATILE", 2);
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

   struct json_object *in = llm_responses_build_input(h, "Q2", NULL, NULL, 0, "VOLATILE", 2);
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

   struct json_object *in = llm_responses_build_input(h, "", NULL, NULL, 0, "VOLATILE", 2);

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
   struct json_object *in = llm_responses_build_input(h, "Q2", imgs, sizes, 1, "VOLATILE", 2);

   int lu = last_user_index(in); /* the question Q2 */
   TEST_ASSERT_EQUAL_STRING("Q2", item_text(in, lu));
   TEST_ASSERT_TRUE(item_has_image(in, lu));      /* image on the question */
   TEST_ASSERT_FALSE(item_has_image(in, lu - 1)); /* not on the volatile item */
   TEST_ASSERT_EQUAL_STRING("VOLATILE", item_text(in, lu - 1));

   json_object_put(in);
   json_object_put(h);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_extract_stable_and_volatile);
   RUN_TEST(test_single_system_has_no_volatile);
   RUN_TEST(test_volatile_before_question_in_history);
   RUN_TEST(test_volatile_before_question_via_input_text);
   RUN_TEST(test_mid_history_broadcast_emitted_inline);
   RUN_TEST(test_vision_stays_on_question);
   return UNITY_END();
}
