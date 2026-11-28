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
 * json_drop_nulls: every null member goes, at any depth and inside arrays'
 * objects; null array elements stay; anything that isn't an object or array is
 * left alone; the count is what was removed.
 */

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "core/json_nulls.h"
#include "unity.h"

void setUp(void) {
}

void tearDown(void) {
}

/* @p json after json_drop_nulls, as compact text; *dropped gets the count. */
static char *dropped_text(const char *json, int *dropped) {
   struct json_object *o = json_tokener_parse(json);
   TEST_ASSERT_NOT_NULL(o);
   *dropped = json_drop_nulls(o);
   char *out = strdup(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN));
   json_object_put(o);
   return out;
}

static void expect(const char *json, const char *want, int want_dropped) {
   int dropped = -1;
   char *got = dropped_text(json, &dropped);
   TEST_ASSERT_EQUAL_STRING_MESSAGE(want, got, json);
   TEST_ASSERT_EQUAL_INT_MESSAGE(want_dropped, dropped, json);
   free(got);
}

void test_nested_null_members_are_removed(void) {
   expect("{\"a\":null,\"b\":{\"c\":null,\"d\":1,\"e\":{\"f\":null}},\"g\":\"x\"}",
          "{\"b\":{\"d\":1,\"e\":{}},\"g\":\"x\"}", 3);
}

void test_objects_inside_arrays_are_cleaned(void) {
   expect("{\"list\":[{\"k\":null,\"v\":2},[{\"deep\":null}],3]}", "{\"list\":[{\"v\":2},[{}],3]}",
          2);
}

void test_null_array_elements_stay(void) {
   expect("{\"list\":[null,1,null]}", "{\"list\":[null,1,null]}", 0);
   expect("[null,{\"a\":null}]", "[null,{}]", 1);
}

void test_an_object_of_nulls_empties(void) {
   expect("{\"a\":null,\"b\":null,\"c\":null,\"d\":null}", "{}", 4);
}

void test_a_second_pass_finds_nothing(void) {
   struct json_object *o = json_tokener_parse(
       "{\"a\":null,\"b\":[{\"c\":null}],\"d\":{\"e\":null}}");
   TEST_ASSERT_EQUAL_INT(3, json_drop_nulls(o));
   TEST_ASSERT_EQUAL_INT(0, json_drop_nulls(o));
   json_object_put(o);
}

void test_roots_that_are_not_objects_are_left(void) {
   TEST_ASSERT_EQUAL_INT(0, json_drop_nulls(NULL));
   static const char *const roots[] = { "\"text\"", "5", "1.5", "true", "[]", "[1,null]" };
   for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
      int dropped = -1;
      char *got = dropped_text(roots[i], &dropped);
      TEST_ASSERT_EQUAL_STRING(roots[i], got);
      TEST_ASSERT_EQUAL_INT(0, dropped);
      free(got);
   }
}

/* What handle_undo_memory_batch relies on: an `"op_id": null` reads as absent
 * after the door, and the door's count is the only sign it was there. */
void test_a_null_op_id_reads_as_absent_and_is_counted(void) {
   struct json_object *frame = json_tokener_parse(
       "{\"type\":\"undo_memory_batch\",\"payload\":{\"batch_id\":4,\"op_id\":null}}");
   TEST_ASSERT_EQUAL_INT(1, json_drop_nulls(frame));
   struct json_object *payload = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(frame, "payload", &payload));
   TEST_ASSERT_FALSE(json_object_object_get_ex(payload, "op_id", NULL));
   json_object_put(frame);

   frame = json_tokener_parse("{\"type\":\"undo_memory_batch\",\"payload\":{\"batch_id\":4}}");
   TEST_ASSERT_EQUAL_INT(0, json_drop_nulls(frame));
   json_object_put(frame);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_nested_null_members_are_removed);
   RUN_TEST(test_objects_inside_arrays_are_cleaned);
   RUN_TEST(test_null_array_elements_stay);
   RUN_TEST(test_an_object_of_nulls_empties);
   RUN_TEST(test_a_second_pass_finds_nothing);
   RUN_TEST(test_roots_that_are_not_objects_are_left);
   RUN_TEST(test_a_null_op_id_reads_as_absent_and_is_counted);
   return UNITY_END();
}
