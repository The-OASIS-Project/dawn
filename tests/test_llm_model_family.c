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
 * The provider a bare model name belongs to.
 */

#include "llm/llm_model_family.h"
#include "unity.h"

void setUp(void) {
}

void tearDown(void) {
}

static void test_provider_from_name(void) {
   TEST_ASSERT_EQUAL(CLOUD_PROVIDER_OPENAI, llm_model_provider_from_name("gpt-5.5"));
   TEST_ASSERT_EQUAL(CLOUD_PROVIDER_CLAUDE, llm_model_provider_from_name("claude-haiku-5-5"));
   TEST_ASSERT_EQUAL(CLOUD_PROVIDER_GEMINI, llm_model_provider_from_name("gemini-3-flash"));
   TEST_ASSERT_EQUAL(CLOUD_PROVIDER_NONE,
                     llm_model_provider_from_name("anthropic/claude-opus-5.5"));
   TEST_ASSERT_EQUAL(CLOUD_PROVIDER_NONE, llm_model_provider_from_name("qwen3-4b"));
   TEST_ASSERT_EQUAL(CLOUD_PROVIDER_NONE, llm_model_provider_from_name(""));
   TEST_ASSERT_EQUAL(CLOUD_PROVIDER_NONE, llm_model_provider_from_name(NULL));
}

static void test_openai_o_series(void) {
   TEST_ASSERT_EQUAL(CLOUD_PROVIDER_OPENAI, llm_model_provider_from_name("o3"));
   TEST_ASSERT_EQUAL(CLOUD_PROVIDER_OPENAI, llm_model_provider_from_name("o1-pro"));
   TEST_ASSERT_EQUAL(CLOUD_PROVIDER_OPENAI, llm_model_provider_from_name("o4-mini"));
   TEST_ASSERT_EQUAL(CLOUD_PROVIDER_OPENAI, llm_model_provider_from_name("o12-mini"));
   TEST_ASSERT_EQUAL(CLOUD_PROVIDER_NONE, llm_model_provider_from_name("o"));
   TEST_ASSERT_EQUAL(CLOUD_PROVIDER_NONE, llm_model_provider_from_name("o-mini"));
   TEST_ASSERT_EQUAL(CLOUD_PROVIDER_NONE, llm_model_provider_from_name("o3mini"));
   TEST_ASSERT_EQUAL(CLOUD_PROVIDER_NONE, llm_model_provider_from_name("olmo-2"));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_provider_from_name);
   RUN_TEST(test_openai_o_series);
   return UNITY_END();
}
