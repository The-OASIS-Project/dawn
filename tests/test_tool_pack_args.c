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
 * A tool call's JSON arguments become the callback's (action, value) the same
 * way wherever they come from: a live call, or a scheduled call stored as JSON.
 */

#include <json-c/json.h>
#include <string.h>

#include "tools/tool_registry.h"
#include "unity.h"

/* Shaped like the stocks tool: a value param plus named custom fields. */
static const treg_param_t s_fields_params[] = {
   { .name = "action", .type = TOOL_PARAM_TYPE_STRING, .maps_to = TOOL_MAPS_TO_ACTION },
   { .name = "symbols", .type = TOOL_PARAM_TYPE_STRING, .maps_to = TOOL_MAPS_TO_VALUE },
   { .name = "range",
     .type = TOOL_PARAM_TYPE_STRING,
     .maps_to = TOOL_MAPS_TO_CUSTOM,
     .field_name = "range" },
   { .name = "interval",
     .type = TOOL_PARAM_TYPE_STRING,
     .maps_to = TOOL_MAPS_TO_CUSTOM,
     .field_name = "interval" },
   { .name = "data",
     .type = TOOL_PARAM_TYPE_STRING,
     .maps_to = TOOL_MAPS_TO_CUSTOM,
     .field_name = "data" },
};
static const tool_metadata_t s_fields_tool = {
   .name = "fields_tool",
   .params = s_fields_params,
   .param_count = 5,
};

/* Shaped like the email tool: the value is a JSON "arguments" string. */
static const treg_param_t s_json_params[] = {
   { .name = "action", .type = TOOL_PARAM_TYPE_STRING, .maps_to = TOOL_MAPS_TO_ACTION },
   { .name = "arguments", .type = TOOL_PARAM_TYPE_STRING, .maps_to = TOOL_MAPS_TO_VALUE },
};
static const tool_metadata_t s_json_tool = {
   .name = "json_tool",
   .params = s_json_params,
   .param_count = 2,
};

#define NAME_LEN 64
#define VALUE_LEN 256

static char s_action[NAME_LEN];
static char s_value[VALUE_LEN];
static char s_device[NAME_LEN];
static char s_err[160];

void setUp(void) {
   s_action[0] = s_value[0] = s_device[0] = s_err[0] = '\0';
}

void tearDown(void) {
}

static int pack(const tool_metadata_t *meta, const char *json) {
   struct json_object *args = json_tokener_parse(json);
   TEST_ASSERT_NOT_NULL(args);
   int rc = tool_pack_args(meta, args, s_action, sizeof(s_action), s_value, sizeof(s_value),
                           s_device, sizeof(s_device), s_err, sizeof(s_err));
   json_object_put(args);
   return rc;
}

static void test_named_fields_are_packed_after_the_base(void) {
   TEST_ASSERT_EQUAL_INT(SUCCESS, pack(&s_fields_tool, "{\"symbols\":\"SOXX\",\"range\":\"1mo\","
                                                       "\"data\":\"raw\"}"));
   TEST_ASSERT_EQUAL_STRING("SOXX::range::1mo::data::raw", s_value);
}

static void test_an_empty_object_is_an_empty_value(void) {
   TEST_ASSERT_EQUAL_INT(SUCCESS, pack(&s_fields_tool, "{}"));
   TEST_ASSERT_EQUAL_STRING("", s_value);
}

static void test_the_action_key_fills_the_action(void) {
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         pack(&s_fields_tool, "{\"action\":\"quote\",\"symbols\":\"NVDA\"}"));
   TEST_ASSERT_EQUAL_STRING("quote", s_action);
   TEST_ASSERT_EQUAL_STRING("NVDA", s_value);
}

static void test_flat_args_for_a_json_value_stay_json(void) {
   TEST_ASSERT_EQUAL_INT(SUCCESS, pack(&s_json_tool, "{\"window\":\"24h\",\"max\":100}"));
   struct json_object *v = json_tokener_parse(s_value);
   TEST_ASSERT_NOT_NULL(v);
   struct json_object *f = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(v, "window", &f));
   TEST_ASSERT_EQUAL_STRING("24h", json_object_get_string(f));
   TEST_ASSERT_TRUE(json_object_object_get_ex(v, "max", &f));
   TEST_ASSERT_EQUAL_INT(100, json_object_get_int(f));
   TEST_ASSERT_EQUAL_INT(2, json_object_object_length(v));
   json_object_put(v);
}

static void test_outputs_are_cleared_first(void) {
   strcpy(s_value, "stale");
   strcpy(s_action, "stale");
   TEST_ASSERT_EQUAL_INT(SUCCESS, pack(&s_fields_tool, "{\"symbols\":\"NVDA\"}"));
   TEST_ASSERT_EQUAL_STRING("NVDA", s_value);
   TEST_ASSERT_EQUAL_STRING("", s_action);
}

static void test_action_and_device_may_be_left_out(void) {
   struct json_object *args = json_tokener_parse("{\"action\":\"quote\",\"symbols\":\"NVDA\"}");
   TEST_ASSERT_EQUAL_INT(SUCCESS, tool_pack_args(&s_fields_tool, args, NULL, 0, s_value,
                                                 sizeof(s_value), NULL, 0, s_err, sizeof(s_err)));
   TEST_ASSERT_EQUAL_STRING("NVDA", s_value);
   json_object_put(args);
}

static void test_json_too_long_for_the_value_is_refused(void) {
   char json[VALUE_LEN + 32];
   char filler[VALUE_LEN];
   memset(filler, '/', sizeof(filler) - 1);
   filler[sizeof(filler) - 1] = '\0';
   snprintf(json, sizeof(json), "{\"q\":\"%.*s\"}", VALUE_LEN / 2, filler);
   /* Fits as stored; rebuilt, each '/' is written "\\/" and it doesn't. */
   TEST_ASSERT_LESS_THAN(VALUE_LEN, strlen(json));
   TEST_ASSERT_EQUAL_INT(FAILURE, pack(&s_json_tool, json));
   TEST_ASSERT_NOT_EQUAL(0, strlen(s_err));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_named_fields_are_packed_after_the_base);
   RUN_TEST(test_an_empty_object_is_an_empty_value);
   RUN_TEST(test_the_action_key_fills_the_action);
   RUN_TEST(test_flat_args_for_a_json_value_stay_json);
   RUN_TEST(test_outputs_are_cleared_first);
   RUN_TEST(test_action_and_device_may_be_left_out);
   RUN_TEST(test_json_too_long_for_the_value_is_refused);
   return UNITY_END();
}
