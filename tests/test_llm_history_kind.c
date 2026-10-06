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
 * Unit tests for request context on a history: loaded rows fold into their
 * question, the rows a question with context saves as, the predicate every
 * counter uses, and the copies that must never carry context (extraction,
 * summaries) or DAWN's own keys (the wire).
 */

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm_history_kind.h"
#include "llm/llm_history_rows.h"
#include "llm/llm_turn_blocks.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

#define FLAGS (JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE)

static struct json_object *parse(const char *json) {
   struct json_object *obj = json_tokener_parse(json);
   TEST_ASSERT_NOT_NULL_MESSAGE(obj, json);
   return obj;
}

static void assert_json(const char *expected, struct json_object *obj) {
   TEST_ASSERT_EQUAL_STRING(expected, json_object_to_json_string_ext(obj, FLAGS));
}

/* Loaded rows: a turn's context rows (saved after its question) become the
 * question's leading parts, in row order. */
static void test_fold_puts_context_in_front_of_the_question(void) {
   struct json_object *h = parse(
       "[{\"role\":\"system\",\"content\":\"P\"},"
       "{\"role\":\"user\",\"content\":\"Hi\",\"id\":1},"
       "{\"role\":\"user\",\"content\":\"MEM\",\"_kind\":\"memory\",\"id\":2},"
       "{\"role\":\"user\",\"content\":\"CTX\",\"_kind\":\"turn_context\",\"id\":3},"
       "{\"role\":\"system\",\"content\":\"D\",\"_kind\":\"directive\",\"id\":4},"
       "{\"role\":\"assistant\",\"content\":\"Hello\",\"id\":5}]");
   TEST_ASSERT_EQUAL_INT(0, llm_history_fold_context(h, 0));
   assert_json("[{\"role\":\"system\",\"content\":\"P\"},"
               "{\"role\":\"user\",\"id\":1,\"content\":["
               "{\"type\":\"text\",\"text\":\"MEM\",\"_kind\":\"memory\"},"
               "{\"type\":\"text\",\"text\":\"CTX\",\"_kind\":\"turn_context\"},"
               "{\"type\":\"text\",\"text\":\"Hi\"}]},"
               "{\"role\":\"system\",\"content\":\"D\",\"_kind\":\"directive\",\"id\":4},"
               "{\"role\":\"assistant\",\"content\":\"Hello\",\"id\":5}]",
               h);
   TEST_ASSERT_FALSE(llm_history_is_context(json_object_array_get_idx(h, 1)));
   TEST_ASSERT_TRUE(llm_history_is_context(json_object_array_get_idx(h, 2)));
   json_object_put(h);
}

/* An image question keeps its parts after the context. */
static void test_fold_keeps_a_content_array_question(void) {
   struct json_object *h = parse(
       "[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"what?\"},"
       "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:x\"}}]},"
       "{\"role\":\"user\",\"content\":\"CTX\",\"_kind\":\"turn_context\"}]");
   TEST_ASSERT_EQUAL_INT(0, llm_history_fold_context(h, 0));
   assert_json("[{\"role\":\"user\",\"content\":["
               "{\"type\":\"text\",\"text\":\"CTX\",\"_kind\":\"turn_context\"},"
               "{\"type\":\"text\",\"text\":\"what?\"},"
               "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:x\"}}]}]",
               h);
   json_object_put(h);
}

/* Context with no question since the last reply (the question's row wasn't
 * saved) stays a message of its own, made only of context, with its id. */
static void test_fold_leaves_orphan_context_alone(void) {
   struct json_object *h = parse(
       "[{\"role\":\"user\",\"content\":\"Q\"},"
       "{\"role\":\"assistant\",\"content\":\"A\"},"
       "{\"role\":\"user\",\"content\":\"MEM\",\"_kind\":\"memory\",\"id\":9},"
       "{\"role\":\"assistant\",\"content\":\"B\"}]");
   TEST_ASSERT_EQUAL_INT(0, llm_history_fold_context(h, 0));
   TEST_ASSERT_EQUAL_INT(4, (int)json_object_array_length(h));
   TEST_ASSERT_FALSE(llm_history_is_context(json_object_array_get_idx(h, 0)));
   TEST_ASSERT_TRUE(llm_history_is_context(json_object_array_get_idx(h, 2)));
   assert_json("{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"MEM\","
               "\"_kind\":\"memory\"}],\"id\":9}",
               json_object_array_get_idx(h, 2));
   json_object_put(h);
}

/* An envelope (a turn DAWN started) takes its context; a message of tool
 * results doesn't, and ends the turn's. */
static void test_fold_into_an_envelope_not_a_result(void) {
   struct json_object *h = parse(
       "[{\"role\":\"user\",\"content\":\"ENV\",\"_kind\":\"envelope\"},"
       "{\"role\":\"user\",\"content\":\"CTX\",\"_kind\":\"turn_context\"},"
       "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t\","
       "\"content\":\"r\"}]},"
       "{\"role\":\"user\",\"content\":\"CTX2\",\"_kind\":\"turn_context\"}]");
   TEST_ASSERT_EQUAL_INT(0, llm_history_fold_context(h, 0));
   TEST_ASSERT_EQUAL_INT(3, (int)json_object_array_length(h));
   TEST_ASSERT_EQUAL_INT(MESSAGE_KIND_ENVELOPE,
                         llm_history_kind_of(json_object_array_get_idx(h, 0)));
   TEST_ASSERT_TRUE(llm_history_has_context_parts(json_object_array_get_idx(h, 0)));
   TEST_ASSERT_TRUE(llm_history_is_context(json_object_array_get_idx(h, 2)));
   json_object_put(h);
}

/* A directive among a turn's context rows stays where it is; the context
 * still reaches the question. */
static void test_fold_passes_a_directive_through(void) {
   struct json_object *h = parse(
       "[{\"role\":\"user\",\"content\":\"Hi\"},"
       "{\"role\":\"system\",\"content\":\"D\",\"_kind\":\"directive\"},"
       "{\"role\":\"user\",\"content\":\"CTX\",\"_kind\":\"turn_context\"}]");
   TEST_ASSERT_EQUAL_INT(0, llm_history_fold_context(h, 0));
   TEST_ASSERT_EQUAL_INT(2, (int)json_object_array_length(h));
   TEST_ASSERT_TRUE(llm_history_has_context_parts(json_object_array_get_idx(h, 0)));
   TEST_ASSERT_EQUAL_INT(MESSAGE_KIND_DIRECTIVE,
                         llm_history_kind_of(json_object_array_get_idx(h, 1)));
   json_object_put(h);
}

/* An empty question adds no empty part. */
static void test_fold_empty_question(void) {
   struct json_object *h = parse(
       "[{\"role\":\"user\",\"content\":\"\",\"id\":6},"
       "{\"role\":\"user\",\"content\":\"CTX\",\"_kind\":\"turn_context\",\"id\":7}]");
   TEST_ASSERT_EQUAL_INT(0, llm_history_fold_context(h, 0));
   assert_json("[{\"role\":\"user\",\"id\":6,\"content\":["
               "{\"type\":\"text\",\"text\":\"CTX\",\"_kind\":\"turn_context\"}]}]",
               h);
   json_object_put(h);
}

/* Saving a folded question gives back the rows it was loaded from. */
static void test_rows_round_trip_a_question_with_context(void) {
   struct json_object *msg = parse(
       "{\"role\":\"user\",\"content\":["
       "{\"type\":\"text\",\"text\":\"MEM\",\"_kind\":\"memory\"},"
       "{\"type\":\"text\",\"text\":\"CTX\",\"_kind\":\"turn_context\"},"
       "{\"type\":\"text\",\"text\":\"Hi\"}]}");
   struct json_object *rows = json_object_new_array();
   TEST_ASSERT_EQUAL_INT(3, llm_history_rows_append(msg, rows));
   assert_json("[{\"role\":\"user\",\"content\":\"Hi\"},"
               "{\"role\":\"user\",\"content\":\"MEM\",\"_kind\":\"memory\"},"
               "{\"role\":\"user\",\"content\":\"CTX\",\"_kind\":\"turn_context\"}]",
               rows);
   /* And loading those rows folds them back. */
   TEST_ASSERT_EQUAL_INT(0, llm_history_fold_context(rows, 0));
   TEST_ASSERT_EQUAL_INT(1, (int)json_object_array_length(rows));
   assert_json(json_object_to_json_string_ext(msg, FLAGS), json_object_array_get_idx(rows, 0));
   json_object_put(rows);
   json_object_put(msg);
}

static void test_rows_carry_a_message_kind_and_skip_the_prefix(void) {
   struct json_object *directive = parse(
       "{\"role\":\"system\",\"content\":\"D\",\"_kind\":\"directive\"}");
   struct json_object *prefix = parse(
       "{\"role\":\"system\",\"content\":\"P\",\"_kind\":\"prefix\"}");
   struct json_object *rows = json_object_new_array();
   TEST_ASSERT_EQUAL_INT(1, llm_history_rows_append(directive, rows));
   TEST_ASSERT_EQUAL_INT(0, llm_history_rows_append(prefix, rows));
   assert_json("[{\"role\":\"system\",\"content\":\"D\",\"_kind\":\"directive\"}]", rows);
   json_object_put(rows);
   json_object_put(directive);
   json_object_put(prefix);
}

/* Extraction and summaries never see request context. */
static void test_strip_internal_drops_context(void) {
   struct json_object *h = parse(
       "[{\"role\":\"system\",\"content\":\"D\",\"_kind\":\"directive\"},"
       "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"MEM\",\"_kind\":\"memory\"},"
       "{\"type\":\"text\",\"text\":\"Hi\"}]},"
       "{\"role\":\"user\",\"content\":\"ENV\",\"_kind\":\"envelope\"},"
       "{\"role\":\"assistant\",\"content\":\"Hello\"}]");
   struct json_object *clean = llm_history_strip_internal(h);
   TEST_ASSERT_NOT_NULL(clean);
   assert_json("[{\"role\":\"user\",\"content\":\"Hi\"},"
               "{\"role\":\"assistant\",\"content\":\"Hello\"}]",
               clean);
   /* The history itself is untouched. */
   TEST_ASSERT_EQUAL_INT(4, (int)json_object_array_length(h));

   json_object_put(clean);
   json_object_put(h);
}

/* The wire copy removes DAWN's keys from content parts too, without touching
 * the history's parts. */
static void test_wire_copy_strips_part_keys(void) {
   struct json_object *h = parse(
       "[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"CTX\",\"_kind\":"
       "\"turn_context\"},{\"type\":\"text\",\"text\":\"Hi\"}]}]");
   struct json_object *wire = llm_history_wire_copy(h);
   assert_json("[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"CTX\"},"
               "{\"type\":\"text\",\"text\":\"Hi\"}]}]",
               wire);
   TEST_ASSERT_TRUE(llm_history_has_context_parts(json_object_array_get_idx(h, 0)));
   json_object_put(wire);
   json_object_put(h);
}

/* Rows saved naming their question go with it, whatever landed between:
 * another writer's exchange, even another turn's question. */
static void test_fold_attaches_anchored_rows_by_id(void) {
   struct json_object *h = parse(
       "[{\"role\":\"user\",\"content\":\"Q\",\"id\":1},"
       "{\"role\":\"user\",\"content\":\"Other\",\"id\":2},"
       "{\"role\":\"assistant\",\"content\":\"A2\",\"id\":3},"
       "{\"role\":\"user\",\"content\":\"CTX\",\"_kind\":\"turn_context\",\"_context_of\":1,"
       "\"id\":4},"
       "{\"role\":\"system\",\"content\":\"D\",\"_kind\":\"directive\",\"_context_of\":1,"
       "\"id\":5},"
       "{\"role\":\"assistant\",\"content\":\"A\",\"id\":6}]");
   TEST_ASSERT_EQUAL_INT(0, llm_history_fold_context(h, 0));
   assert_json("[{\"role\":\"user\",\"id\":1,\"content\":["
               "{\"type\":\"text\",\"text\":\"CTX\",\"_kind\":\"turn_context\"},"
               "{\"type\":\"text\",\"text\":\"Q\"}]},"
               "{\"role\":\"system\",\"content\":\"D\",\"_kind\":\"directive\",\"_context_of\":1,"
               "\"id\":5},"
               "{\"role\":\"user\",\"content\":\"Other\",\"id\":2},"
               "{\"role\":\"assistant\",\"content\":\"A2\",\"id\":3},"
               "{\"role\":\"assistant\",\"content\":\"A\",\"id\":6}]",
               h);
   json_object_put(h);
}

/* A question left out of the load (past a compaction point) takes its rows
 * with it: they never become an orphan or another question's context. */
static void test_fold_drops_rows_of_an_unloaded_question(void) {
   struct json_object *h = parse(
       "[{\"role\":\"user\",\"content\":\"CTX\",\"_kind\":\"turn_context\",\"_context_of\":7,"
       "\"id\":9},"
       "{\"role\":\"assistant\",\"content\":\"A\",\"id\":10},"
       "{\"role\":\"user\",\"content\":\"Q\",\"id\":11}]");
   TEST_ASSERT_EQUAL_INT(0, llm_history_fold_context(h, 0));
   assert_json("[{\"role\":\"assistant\",\"content\":\"A\",\"id\":10},"
               "{\"role\":\"user\",\"content\":\"Q\",\"id\":11}]",
               h);
   json_object_put(h);
}

/* An envelope's context (untrusted text) comes back as a message of its own
 * just before it, never inside it. */
static void test_fold_keeps_an_envelopes_context_apart(void) {
   struct json_object *h = parse(
       "[{\"role\":\"user\",\"content\":\"JOB OUTPUT\",\"_kind\":\"envelope\",\"id\":5},"
       "{\"role\":\"user\",\"content\":\"CTX\",\"_kind\":\"turn_context\",\"_context_of\":5,"
       "\"id\":6},"
       "{\"role\":\"assistant\",\"content\":\"A\",\"id\":7}]");
   TEST_ASSERT_EQUAL_INT(0, llm_history_fold_context(h, 0));
   assert_json("[{\"role\":\"user\",\"content\":["
               "{\"type\":\"text\",\"text\":\"CTX\",\"_kind\":\"turn_context\"}]},"
               "{\"role\":\"user\",\"content\":\"JOB OUTPUT\",\"_kind\":\"envelope\",\"id\":5},"
               "{\"role\":\"assistant\",\"content\":\"A\",\"id\":7}]",
               h);
   json_object_put(h);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_fold_puts_context_in_front_of_the_question);
   RUN_TEST(test_fold_keeps_a_content_array_question);
   RUN_TEST(test_fold_leaves_orphan_context_alone);
   RUN_TEST(test_fold_into_an_envelope_not_a_result);
   RUN_TEST(test_fold_passes_a_directive_through);
   RUN_TEST(test_fold_empty_question);
   RUN_TEST(test_fold_attaches_anchored_rows_by_id);
   RUN_TEST(test_fold_drops_rows_of_an_unloaded_question);
   RUN_TEST(test_fold_keeps_an_envelopes_context_apart);
   RUN_TEST(test_rows_round_trip_a_question_with_context);
   RUN_TEST(test_rows_carry_a_message_kind_and_skip_the_prefix);
   RUN_TEST(test_strip_internal_drops_context);
   RUN_TEST(test_wire_copy_strips_part_keys);
   return UNITY_END();
}
