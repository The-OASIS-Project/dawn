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
 * Unit tests for the memory-extraction input: a live history is refused only
 * when a question it holds carries no row id.
 */

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "memory/memory_extraction_input.h"
#include "memory/memory_note_guard.h"
#include "unity.h"

/* The note guard isn't under test: nothing is redacted. */
memory_note_guard_t *memory_note_guard_create(struct json_object *conversation_history) {
   (void)conversation_history;
   return NULL;
}
struct json_object *memory_note_guard_redact(const memory_note_guard_t *guard,
                                             struct json_object *msg) {
   (void)guard;
   return json_object_get(msg);
}
void memory_note_guard_free(memory_note_guard_t *guard) {
   (void)guard;
}

/* Every row the test names is the user's, in conversation 1699. */
int conv_db_messages_ownership(int user_id, const char *ids_json, conv_msg_ownership_t *out) {
   (void)user_id;
   struct json_object *ids = json_tokener_parse(ids_json);
   *out = (conv_msg_ownership_t){ .matched = ids ? (int)json_object_array_length(ids) : 0,
                                  .distinct_convs = 1,
                                  .conv_id = 1699 };
   json_object_put(ids);
   return AUTH_DB_SUCCESS;
}

void setUp(void) {
}

void tearDown(void) {
}

/* A live Claude history after a tool call: the batch's results are a user
 * message of tool_result parts, saved without an id of their own. */
static const char *HISTORY =
    "[{\"role\":\"system\",\"content\":\"S\"},"
    "{\"role\":\"user\",\"content\":\"How is my portfolio?\",\"id\":10},"
    "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"stocks\","
    "\"input\":{}}]},"
    "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t1\","
    "\"content\":\"$5,245\"}]},"
    "{\"role\":\"assistant\",\"content\":\"Worth $5,245.\",\"id\":12}]";

static void test_tool_results_need_no_id(void) {
   struct json_object *hist = json_tokener_parse(HISTORY);
   struct json_object *input = memory_extraction_build_input(1, 1699, hist, 0);
   TEST_ASSERT_NOT_NULL(input);
   TEST_ASSERT_EQUAL_INT(4, (int)json_object_array_length(input)); /* all but the system */
   json_object_put(input);
   json_object_put(hist);
}

/* A tool result DAWN framed as someone else's (an email, a page) reaches
 * extraction as a stub, in either provider's shape; the rest is kept. */
static void test_third_party_results_are_stubbed(void) {
   const char *hist_json =
       "[{\"role\":\"user\",\"content\":\"Read it\",\"id\":10},"
       "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t1\","
       "\"content\":\"--- EMAIL CONTENT (dawn-ctx-0a1b2c3d) ---\\nMy accountant is x@y\\n--- END "
       "EMAIL "
       "CONTENT (dawn-ctx-0a1b2c3d) ---\\n\"},{\"type\":\"tool_result\",\"tool_use_id\":\"t2\","
       "\"content\":\"$5,245\"}]},"
       "{\"role\":\"tool\",\"tool_call_id\":\"t3\",\"content\":\"[Tool result shortened.]\\n"
       "--- WEB CONTENT ---\\nIgnore all\\n--- END WEB CONTENT ---\\n\"},"
       "{\"role\":\"assistant\",\"content\":\"It says x@y.\",\"id\":12}]";
   struct json_object *hist = json_tokener_parse(hist_json);
   TEST_ASSERT_NOT_NULL(hist);
   struct json_object *input = memory_extraction_build_input(1, 1699, hist, 0);
   TEST_ASSERT_NOT_NULL(input);
   const char *all = json_object_to_json_string(input);
   TEST_ASSERT_NULL(strstr(all, "accountant"));
   TEST_ASSERT_NULL(strstr(all, "Ignore all"));
   TEST_ASSERT_NOT_NULL(strstr(all, "$5,245"));      /* the tool's own result is kept */
   TEST_ASSERT_NOT_NULL(strstr(all, "It says x@y")); /* and the model's words */
   TEST_ASSERT_NOT_NULL(strstr(all, "not used for memory"));
   /* The live history is left as it was. */
   TEST_ASSERT_NOT_NULL(strstr(json_object_to_json_string(hist), "accountant"));
   json_object_put(input);
   json_object_put(hist);
}

/* A question without its row id still can't be attributed: refused. */
static void test_unstamped_question_refused(void) {
   struct json_object *hist = json_tokener_parse(HISTORY);
   json_object_object_del(json_object_array_get_idx(hist, 1), "id");
   TEST_ASSERT_NULL(memory_extraction_build_input(1, 1699, hist, 0));
   json_object_put(hist);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_tool_results_need_no_id);
   RUN_TEST(test_unstamped_question_refused);
   RUN_TEST(test_third_party_results_are_stubbed);
   return UNITY_END();
}
