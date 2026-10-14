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
 * Unit tests for src/core/device_types.c — pattern matching and lookup.
 */

#include <string.h>

#include "core/device_types.h"
#include "tools/tool_registry.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* ── Lookup tests ────────────────────────────────────────────────────────── */

static void test_get_def_boolean(void) {
   const device_type_def_t *def = device_type_get_def(TOOL_DEVICE_TYPE_BOOLEAN);
   TEST_ASSERT_NOT_NULL(def);
   TEST_ASSERT_EQUAL_STRING("boolean", def->name);
   TEST_ASSERT_EQUAL_INT(2, def->action_count);
}

static void test_get_def_analog(void) {
   const device_type_def_t *def = device_type_get_def(TOOL_DEVICE_TYPE_ANALOG);
   TEST_ASSERT_NOT_NULL(def);
   TEST_ASSERT_EQUAL_STRING("analog", def->name);
   TEST_ASSERT_EQUAL_INT(1, def->action_count);
}

static void test_get_def_all_types(void) {
   TEST_ASSERT_NOT_NULL(device_type_get_def(TOOL_DEVICE_TYPE_BOOLEAN));
   TEST_ASSERT_NOT_NULL(device_type_get_def(TOOL_DEVICE_TYPE_ANALOG));
   TEST_ASSERT_NOT_NULL(device_type_get_def(TOOL_DEVICE_TYPE_GETTER));
   TEST_ASSERT_NOT_NULL(device_type_get_def(TOOL_DEVICE_TYPE_MUSIC));
   TEST_ASSERT_NOT_NULL(device_type_get_def(TOOL_DEVICE_TYPE_TRIGGER));
   TEST_ASSERT_NOT_NULL(device_type_get_def(TOOL_DEVICE_TYPE_PASSPHRASE));
}

static void test_get_def_invalid(void) {
   TEST_ASSERT_NULL(device_type_get_def((tool_device_type_t)999));
}

static void test_get_name(void) {
   TEST_ASSERT_EQUAL_STRING("boolean", device_type_get_name(TOOL_DEVICE_TYPE_BOOLEAN));
   TEST_ASSERT_EQUAL_STRING("analog", device_type_get_name(TOOL_DEVICE_TYPE_ANALOG));
   TEST_ASSERT_EQUAL_STRING("getter", device_type_get_name(TOOL_DEVICE_TYPE_GETTER));
   TEST_ASSERT_EQUAL_STRING("music", device_type_get_name(TOOL_DEVICE_TYPE_MUSIC));
   TEST_ASSERT_EQUAL_STRING("trigger", device_type_get_name(TOOL_DEVICE_TYPE_TRIGGER));
   TEST_ASSERT_EQUAL_STRING("passphrase_trigger",
                            device_type_get_name(TOOL_DEVICE_TYPE_PASSPHRASE));
}

static void test_get_name_invalid(void) {
   TEST_ASSERT_EQUAL_STRING("unknown", device_type_get_name((tool_device_type_t)999));
}

/* ── Pattern matching: BOOLEAN ───────────────────────────────────────────── */

static void test_match_boolean_enable(void) {
   const char *action = NULL;
   char value[64] = "";
   bool ok = device_type_match_pattern(&DEVICE_TYPE_BOOLEAN, "turn on the light", "the light", NULL,
                                       &action, value, sizeof(value));
   TEST_ASSERT_TRUE(ok);
   TEST_ASSERT_EQUAL_STRING("enable", action);
}

static void test_match_boolean_disable(void) {
   const char *action = NULL;
   char value[64] = "";
   bool ok = device_type_match_pattern(&DEVICE_TYPE_BOOLEAN, "turn off the light", "the light",
                                       NULL, &action, value, sizeof(value));
   TEST_ASSERT_TRUE(ok);
   TEST_ASSERT_EQUAL_STRING("disable", action);
}

static void test_match_boolean_case_insensitive(void) {
   const char *action = NULL;
   char value[64] = "";
   bool ok = device_type_match_pattern(&DEVICE_TYPE_BOOLEAN, "TURN ON LAMP", "lamp", NULL, &action,
                                       value, sizeof(value));
   TEST_ASSERT_TRUE(ok);
   TEST_ASSERT_EQUAL_STRING("enable", action);
}

static void test_match_boolean_alias(void) {
   const char *action = NULL;
   char value[64] = "";
   const char *aliases[] = { "bedroom lamp", "lamp", NULL };
   bool ok = device_type_match_pattern(&DEVICE_TYPE_BOOLEAN, "turn off bedroom lamp", "main light",
                                       aliases, &action, value, sizeof(value));
   TEST_ASSERT_TRUE(ok);
   TEST_ASSERT_EQUAL_STRING("disable", action);
}

static void test_match_boolean_no_match(void) {
   const char *action = NULL;
   char value[64] = "";
   bool ok = device_type_match_pattern(&DEVICE_TYPE_BOOLEAN, "what is the weather", "the light",
                                       NULL, &action, value, sizeof(value));
   TEST_ASSERT_FALSE(ok);
}

/* ── Pattern matching: ANALOG (with %value%) ─────────────────────────────── */

static void test_match_analog_set_value(void) {
   const char *action = NULL;
   char value[64] = "";
   bool ok = device_type_match_pattern(&DEVICE_TYPE_ANALOG, "set volume to 50", "volume", NULL,
                                       &action, value, sizeof(value));
   TEST_ASSERT_TRUE(ok);
   TEST_ASSERT_EQUAL_STRING("set", action);
   TEST_ASSERT_EQUAL_STRING("50", value);
}

static void test_match_analog_value_with_units(void) {
   const char *action = NULL;
   char value[64] = "";
   bool ok = device_type_match_pattern(&DEVICE_TYPE_ANALOG, "set temperature to 72 degrees",
                                       "temperature", NULL, &action, value, sizeof(value));
   TEST_ASSERT_TRUE(ok);
   TEST_ASSERT_EQUAL_STRING("set", action);
   TEST_ASSERT_EQUAL_STRING("72 degrees", value);
}

static void test_match_analog_trims_trailing_whitespace(void) {
   const char *action = NULL;
   char value[64] = "";
   bool ok = device_type_match_pattern(&DEVICE_TYPE_ANALOG, "set volume to 50   ", "volume", NULL,
                                       &action, value, sizeof(value));
   TEST_ASSERT_TRUE(ok);
   TEST_ASSERT_EQUAL_STRING("50", value);
}

/* ── Pattern matching: GETTER ────────────────────────────────────────────── */

static void test_match_getter(void) {
   const char *action = NULL;
   char value[64] = "";
   bool ok = device_type_match_pattern(&DEVICE_TYPE_GETTER, "what is the weather", "weather", NULL,
                                       &action, value, sizeof(value));
   TEST_ASSERT_TRUE(ok);
   TEST_ASSERT_EQUAL_STRING("get", action);
}

/* ── Pattern matching: MUSIC ─────────────────────────────────────────────── */

static void test_match_music_next(void) {
   const char *action = NULL;
   char value[64] = "";
   bool ok = device_type_match_pattern(&DEVICE_TYPE_MUSIC, "next song", "music", NULL, &action,
                                       value, sizeof(value));
   TEST_ASSERT_TRUE(ok);
   TEST_ASSERT_EQUAL_STRING("next", action);
}

static void test_match_music_play_value(void) {
   const char *action = NULL;
   char value[64] = "";
   /* "play %value%" — captures whole tail */
   bool ok = device_type_match_pattern(&DEVICE_TYPE_MUSIC, "play some jazz", "music", NULL, &action,
                                       value, sizeof(value));
   TEST_ASSERT_TRUE(ok);
   TEST_ASSERT_EQUAL_STRING("play", action);
   TEST_ASSERT_EQUAL_STRING("some jazz", value);

   /* "can you play %value%" — first 3 patterns don't start with "can" */
   memset(value, 0, sizeof(value));
   ok = device_type_match_pattern(&DEVICE_TYPE_MUSIC, "can you play jazz", "music", NULL, &action,
                                  value, sizeof(value));
   TEST_ASSERT_TRUE(ok);
   TEST_ASSERT_EQUAL_STRING("play", action);
   TEST_ASSERT_EQUAL_STRING("jazz", value);
}

static void test_match_music_stop(void) {
   const char *action = NULL;
   char value[64] = "";
   bool ok = device_type_match_pattern(&DEVICE_TYPE_MUSIC, "stop the music", "music", NULL, &action,
                                       value, sizeof(value));
   TEST_ASSERT_TRUE(ok);
   TEST_ASSERT_EQUAL_STRING("stop", action);
}

/* ── Pattern matching: TRIGGER ───────────────────────────────────────────── */

static void test_match_trigger(void) {
   const char *action = NULL;
   char value[64] = "";
   bool ok = device_type_match_pattern(&DEVICE_TYPE_TRIGGER, "self destruct", "self destruct", NULL,
                                       &action, value, sizeof(value));
   TEST_ASSERT_TRUE(ok);
   TEST_ASSERT_EQUAL_STRING("trigger", action);
}

static void test_match_trigger_with_please(void) {
   const char *action = NULL;
   char value[64] = "";
   bool ok = device_type_match_pattern(&DEVICE_TYPE_TRIGGER, "please open the pod bay doors",
                                       "open the pod bay doors", NULL, &action, value,
                                       sizeof(value));
   TEST_ASSERT_TRUE(ok);
   TEST_ASSERT_EQUAL_STRING("trigger", action);
}

/* ── Pattern matching: PASSPHRASE ────────────────────────────────────────── */

static void test_match_passphrase_with_value(void) {
   const char *action = NULL;
   char value[128] = "";
   bool ok = device_type_match_pattern(&DEVICE_TYPE_PASSPHRASE, "shutdown alpha-bravo-1234",
                                       "shutdown", NULL, &action, value, sizeof(value));
   TEST_ASSERT_TRUE(ok);
   TEST_ASSERT_EQUAL_STRING("execute", action);
   TEST_ASSERT_EQUAL_STRING("alpha-bravo-1234", value);
}

/* ── Edge cases ──────────────────────────────────────────────────────────── */

static void test_match_punctuation_after_device_name(void) {
   /* Whisper writes "Shutdown, alpha bravo." (trailing punctuation is stripped
    * by normalize_for_matching; this is the comma after the device name) */
   const char *action = NULL;
   char value[64] = { 0 };
   bool ok = device_type_match_pattern(&DEVICE_TYPE_PASSPHRASE, "shutdown, alpha bravo", "shutdown",
                                       NULL, &action, value, sizeof(value));
   TEST_ASSERT_TRUE(ok);
   TEST_ASSERT_EQUAL_STRING("alpha bravo", value);

   /* A word that only starts with the device name is still no match */
   ok = device_type_match_pattern(&DEVICE_TYPE_PASSPHRASE, "shutdowns alpha", "shutdown", NULL,
                                  &action, value, sizeof(value));
   TEST_ASSERT_FALSE(ok);
}

static void test_value_is_secret(void) {
   TEST_ASSERT_TRUE(device_type_value_is_secret(TOOL_DEVICE_TYPE_PASSPHRASE));
   TEST_ASSERT_FALSE(device_type_value_is_secret(TOOL_DEVICE_TYPE_MUSIC));
   TEST_ASSERT_FALSE(device_type_value_is_secret(TOOL_DEVICE_TYPE_ANALOG));
}

static void test_match_null_inputs(void) {
   const char *action = NULL;
   char value[64] = "";

   TEST_ASSERT_FALSE(device_type_match_pattern(NULL, "turn on light", "light", NULL, &action, value,
                                               sizeof(value)));
   TEST_ASSERT_FALSE(device_type_match_pattern(&DEVICE_TYPE_BOOLEAN, NULL, "light", NULL, &action,
                                               value, sizeof(value)));
   TEST_ASSERT_FALSE(device_type_match_pattern(&DEVICE_TYPE_BOOLEAN, "turn on light", NULL, NULL,
                                               &action, value, sizeof(value)));
}

static void test_match_clears_value_on_no_match_attempt(void) {
   const char *action = NULL;
   char value[64] = "preset";
   bool ok = device_type_match_pattern(&DEVICE_TYPE_BOOLEAN, "no match here", "light", NULL,
                                       &action, value, sizeof(value));
   TEST_ASSERT_FALSE(ok);
   /* Value is cleared at start of match (before any attempt) */
   TEST_ASSERT_EQUAL_STRING("", value);
}

static void test_match_empty_input(void) {
   const char *action = NULL;
   char value[64] = "";
   bool ok = device_type_match_pattern(&DEVICE_TYPE_BOOLEAN, "", "light", NULL, &action, value,
                                       sizeof(value));
   TEST_ASSERT_FALSE(ok);
}

static void test_match_whitespace_handling(void) {
   const char *action = NULL;
   char value[64] = "";
   /* Leading whitespace stripped */
   bool ok = device_type_match_pattern(&DEVICE_TYPE_BOOLEAN, "   turn on light", "light", NULL,
                                       &action, value, sizeof(value));
   TEST_ASSERT_TRUE(ok);
   TEST_ASSERT_EQUAL_STRING("enable", action);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
   UNITY_BEGIN();

   /* Lookup */
   RUN_TEST(test_get_def_boolean);
   RUN_TEST(test_get_def_analog);
   RUN_TEST(test_get_def_all_types);
   RUN_TEST(test_get_def_invalid);
   RUN_TEST(test_get_name);
   RUN_TEST(test_get_name_invalid);

   /* Pattern matching */
   RUN_TEST(test_match_boolean_enable);
   RUN_TEST(test_match_boolean_disable);
   RUN_TEST(test_match_boolean_case_insensitive);
   RUN_TEST(test_match_boolean_alias);
   RUN_TEST(test_match_boolean_no_match);
   RUN_TEST(test_match_analog_set_value);
   RUN_TEST(test_match_analog_value_with_units);
   RUN_TEST(test_match_analog_trims_trailing_whitespace);
   RUN_TEST(test_match_getter);
   RUN_TEST(test_match_music_next);
   RUN_TEST(test_match_music_play_value);
   RUN_TEST(test_match_music_stop);
   RUN_TEST(test_match_trigger);
   RUN_TEST(test_match_trigger_with_please);
   RUN_TEST(test_match_passphrase_with_value);
   RUN_TEST(test_match_punctuation_after_device_name);
   RUN_TEST(test_value_is_secret);

   /* Edge cases */
   RUN_TEST(test_match_null_inputs);
   RUN_TEST(test_match_clears_value_on_no_match_attempt);
   RUN_TEST(test_match_empty_input);
   RUN_TEST(test_match_whitespace_handling);

   return UNITY_END();
}
