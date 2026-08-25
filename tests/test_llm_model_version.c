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
 * Unit tests for the provider-neutral model-id version parser
 * (llm_parse_model_version). Pins the parse cases the OpenAI Responses routing
 * gate and the Claude adaptive-thinking gate both depend on.
 */

#include "llm/llm_model_version.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

static void check(const char *model, int want_major, int want_minor) {
   int major = -1, minor = -1;
   llm_parse_model_version(model, &major, &minor);
   TEST_ASSERT_EQUAL_INT_MESSAGE(want_major, major, model);
   TEST_ASSERT_EQUAL_INT_MESSAGE(want_minor, minor, model);
}

/* OpenAI ids — the cases that drive the Responses routing gate. */
void test_openai_minor_versions(void) {
   check("gpt-5.6-luna", 5, 6);
   check("gpt-5.4", 5, 4);
   check("gpt-5.4-mini", 5, 4);
   check("gpt-5.5", 5, 5);
   check("gpt-5.10", 5, 10); /* double-digit minor */
   check("gpt-5.1", 5, 1);
   check("gpt-5.2", 5, 2);
   check("gpt-5.3", 5, 3);
}

/* gpt-5 base family carries no minor → 5.0. */
void test_openai_base_family(void) {
   check("gpt-5", 5, 0);
   check("gpt-5-mini", 5, 0);
   check("gpt-5-nano", 5, 0);
}

void test_openai_older_and_future(void) {
   check("gpt-4o", 4, 0);
   check("gpt-4.1", 4, 1);
   check("gpt-6", 6, 0);
   check("gpt-6.2-turbo", 6, 2);
}

/* Non-versioned gpt-* families: the parser grabs whatever first numeric token it
 * finds (a size, not a version), which is why the Responses routing predicate
 * additionally requires a digit right after "gpt-" before trusting this. */
void test_openai_non_versioned_families(void) {
   check("gpt-oss-20b", 20, 0); /* size token, NOT a major version */
   check("gpt-image-1", 1, 0);
}

/* Claude ids — the parser is shared, so pin them too. */
void test_claude_ids(void) {
   check("claude-opus-4-8", 4, 8);
   check("claude-sonnet-5", 5, 0);
   check("claude-haiku-4-5", 4, 5);
   check("claude-fable-5", 5, 0);
   check("claude-3-5-sonnet-20241022", 3, 5); /* trailing date must not interfere */
}

void test_edge_cases(void) {
   check(NULL, 0, 0);
   check("", 0, 0);
   check("no-digits-here", 0, 0);
   /* Only-out-param NULLs must not crash. */
   llm_parse_model_version("gpt-5.6", NULL, NULL);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_openai_minor_versions);
   RUN_TEST(test_openai_base_family);
   RUN_TEST(test_openai_older_and_future);
   RUN_TEST(test_openai_non_versioned_families);
   RUN_TEST(test_claude_ids);
   RUN_TEST(test_edge_cases);
   return UNITY_END();
}
