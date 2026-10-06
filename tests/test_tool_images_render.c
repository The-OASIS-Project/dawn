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
 * A tool's images inside its result, on every history shape: counted (the
 * image budget, the token estimate) and stripped for a model without vision
 * (llm_history_strip_vision_content, the real one in
 * llm_tool_images_render.c).
 */

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm_compaction.h"
#include "llm/llm_tool_images_render.h"
#include "llm/llm_tools.h"
#include "unity.h"

#define PNG_URI "data:image/png;base64,iVBORw0KGgo="

void setUp(void) {
}
void tearDown(void) {
}

static struct json_object *parse(const char *json) {
   struct json_object *obj = json_tokener_parse(json);
   TEST_ASSERT_NOT_NULL_MESSAGE(obj, json);
   return obj;
}

static const char *str(struct json_object *obj) {
   return json_object_to_json_string_ext(obj,
                                         JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
}

/* A Claude result holding an image, a chat-completions one, and a question's own. */
static const char *HISTORY =
    "[{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t1\","
    "\"content\":[{\"type\":\"text\",\"text\":\"Captured.\"},{\"type\":\"image_url\","
    "\"image_url\":{\"url\":\"" PNG_URI "\"},\"_image_id\":\"img_0000000000001\"}]}]},"
    "{\"role\":\"tool\",\"tool_call_id\":\"c2\",\"content\":[{\"type\":\"text\",\"text\":"
    "\"Captured.\"},{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":"
    "\"image/png\",\"data\":\"iVBORw0KGgo=\"}}]},"
    "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"And this?\"},"
    "{\"type\":\"image_url\",\"image_url\":{\"url\":\"" PNG_URI "\"}}]}]";

static void test_totals_count_images_inside_results(void) {
   struct json_object *h = parse(HISTORY);
   int count = 0;
   int64_t bytes = 0;
   llm_history_image_totals(h, 0, -1, &count, &bytes);
   TEST_ASSERT_EQUAL_INT(3, count);
   TEST_ASSERT_EQUAL_INT64(2 * (int64_t)strlen(PNG_URI) + (int64_t)strlen("iVBORw0KGgo="), bytes);
   llm_history_image_totals(h, 0, 1, &count, NULL);
   TEST_ASSERT_EQUAL_INT(1, count); /* the nested one */

   const llm_image_limit_t one = { 3, 1000000 };
   TEST_ASSERT_TRUE(llm_tool_images_history_over(h, &one, 1.0f)); /* no room for one more */
   const llm_image_limit_t four = { 4, 1000000 };
   TEST_ASSERT_FALSE(llm_tool_images_history_over(h, &four, 1.0f));
   const llm_image_limit_t small = { 600, 10 };
   TEST_ASSERT_TRUE(llm_tool_images_history_over(h, &small, 1.0f)); /* bytes */
   json_object_put(h);
}

/* The token estimate counts an image inside a Claude tool_result (it read 0). */
static void test_estimate_counts_nested_images(void) {
   struct json_object *with = parse(
       "[{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t1\","
       "\"content\":[{\"type\":\"text\",\"text\":\"Captured.\"},{\"type\":\"image\","
       "\"source\":{\"type\":\"base64\",\"media_type\":\"image/png\",\"data\":\"x\"}}]}]}]");
   struct json_object *without = parse(
       "[{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t1\","
       "\"content\":[{\"type\":\"text\",\"text\":\"Captured.\"}]}]}]");
   const int a = llm_compaction_estimate_range(with, 0, 1);
   const int b = llm_compaction_estimate_range(without, 0, 1);
   TEST_ASSERT_EQUAL_INT(LLM_COMPACTION_IMAGE_ESTIMATE_CHARS / 4, a - b);
   json_object_put(with);
   json_object_put(without);
}

/* Stripped for a model without vision: a result keeps its shape (and its
 * call id) with the fixed text in place of each image; a question's own image
 * is noted as before; the history is untouched. */
static void test_strip_reaches_images_inside_results(void) {
   struct json_object *h = parse(HISTORY);
   char *before = strdup(str(h));
   struct json_object *out = llm_history_strip_vision_content(h);
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_EQUAL_STRING(before, str(h));
   free(before);
   const char *wire = str(out);
   TEST_ASSERT_NULL(strstr(wire, "\"type\":\"image"));
   TEST_ASSERT_NULL(strstr(wire, "iVBORw0KGgo="));

   struct json_object *claude = json_object_array_get_idx(out, 0);
   TEST_ASSERT_EQUAL_STRING(
       "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t1\","
       "\"content\":[{\"type\":\"text\",\"text\":\"Captured.\"},{\"type\":\"text\",\"text\":"
       "\"" LLM_TOOL_IMAGES_NO_VISION_TEXT "\"}]}]}",
       str(claude));
   TEST_ASSERT_EQUAL_STRING("{\"role\":\"tool\",\"tool_call_id\":\"c2\",\"content\":[{\"type\":"
                            "\"text\",\"text\":\"Captured.\"},{\"type\":\"text\",\"text\":"
                            "\"" LLM_TOOL_IMAGES_NO_VISION_TEXT "\"}]}",
                            str(json_object_array_get_idx(out, 1)));
   TEST_ASSERT_EQUAL_STRING("{\"role\":\"user\",\"content\":\"And this? [An image was shared "
                            "earlier]\"}",
                            str(json_object_array_get_idx(out, 2)));
   /* The same every time. */
   struct json_object *again = llm_history_strip_vision_content(h);
   TEST_ASSERT_EQUAL_STRING(wire, str(again));
   json_object_put(again);
   json_object_put(out);
   json_object_put(h);
}

/* A Responses output: a string unless the result holds an image. */
static void test_responses_output_text_unless_image(void) {
   struct json_object *text = parse("[{\"type\":\"text\",\"text\":\"a\"},{\"type\":\"text\","
                                    "\"text\":\"b\"}]");
   struct json_object *out = llm_tool_images_responses_output(text);
   TEST_ASSERT_EQUAL_STRING("a\nb", json_object_get_string(out));
   json_object_put(out);
   json_object_put(text);
   struct json_object *plain = json_object_new_string("Sunny");
   out = llm_tool_images_responses_output(plain);
   TEST_ASSERT_EQUAL_STRING("Sunny", json_object_get_string(out));
   json_object_put(out);
   json_object_put(plain);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_totals_count_images_inside_results);
   RUN_TEST(test_estimate_counts_nested_images);
   RUN_TEST(test_strip_reaches_images_inside_results);
   RUN_TEST(test_responses_output_text_unless_image);
   return UNITY_END();
}
