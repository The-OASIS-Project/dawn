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
 * Unit tests for per-model prompt-cache pricing, against the shipped
 * models.toml parsed by DAWN's own TOML reader.
 */

#include <stdio.h>
#include <string.h>

#include "llm/llm_pricing.h"
#include "toml.h"
#include "unity.h"

static toml_table_t *s_root;

void setUp(void) {
}
void tearDown(void) {
}

static llm_cache_pricing_t price(cloud_provider_t provider, const char *model) {
   return llm_cache_pricing(LLM_CLOUD, provider, model);
}

void test_shipped_models_toml_parses(void) {
   FILE *fp = fopen(MODELS_TOML_PATH, "r");
   TEST_ASSERT_NOT_NULL(fp);
   char err[200];
   s_root = toml_parse_file(fp, err, sizeof(err));
   fclose(fp);
   TEST_ASSERT_NOT_NULL_MESSAGE(s_root, err);
   llm_pricing_load_registry(s_root);
   /* Opus 5.5 has the 1M window (it once fell back to the 200K default). */
   toml_datum_t window = toml_int_in(toml_table_in(s_root, "anthropic"), "claude-opus-5");
   TEST_ASSERT_TRUE(window.ok);
   TEST_ASSERT_EQUAL_INT64(1000000, window.u.i);
}

void test_anthropic_prices_per_model(void) {
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.05f, price(CLOUD_PROVIDER_CLAUDE, "claude-opus-5-5").read);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.025f, price(CLOUD_PROVIDER_CLAUDE, "claude-fable-5-1").read);
   /* Everything else falls to the "claude" row. */
   const llm_cache_pricing_t sonnet = price(CLOUD_PROVIDER_CLAUDE, "claude-sonnet-5");
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.10f, sonnet.read);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.25f, sonnet.write_5m);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.00f, sonnet.write_1h);
}

void test_openai_and_gemini_prices_per_model(void) {
   const llm_cache_pricing_t luna = price(CLOUD_PROVIDER_OPENAI, "gpt-5.6-luna");
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.10f, luna.read);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.25f, luna.write_5m); /* GPT-5.6 charges writes */
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.00f, price(CLOUD_PROVIDER_OPENAI, "gpt-5.5").write_5m);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.50f, price(CLOUD_PROVIDER_OPENAI, "gpt-4o-mini").read);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.25f, price(CLOUD_PROVIDER_OPENAI, "o4-mini").read);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.10f, price(CLOUD_PROVIDER_GEMINI, "gemini-3-flash").read);
}

void test_openrouter_prices_by_vendor(void) {
   /* OpenRouter spells Anthropic versions with dots. */
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.05f,
                            price(CLOUD_PROVIDER_OPENROUTER, "anthropic/claude-opus-5.5").read);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.025f,
                            price(CLOUD_PROVIDER_OPENROUTER, "anthropic/claude-fable-5.1").read);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.10f,
                            price(CLOUD_PROVIDER_OPENROUTER, "anthropic/claude-sonnet-4.6").read);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.25f,
                            price(CLOUD_PROVIDER_OPENROUTER, "openai/gpt-5.6-sol").write_5m);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.50f, price(CLOUD_PROVIDER_OPENROUTER, "meta/llama-4").read);
}

void test_saved_tokens(void) {
   /* Opus 5.5: 1000 read at 0.05 saves 950; 100 written at 1.25 costs 25. */
   TEST_ASSERT_EQUAL_INT(925, llm_cache_saved_input_tokens(LLM_CLOUD, CLOUD_PROVIDER_CLAUDE,
                                                           "claude-opus-5-5", 1000, 100));
   /* A write-only call is a net cost. */
   TEST_ASSERT_EQUAL_INT(-250, llm_cache_saved_input_tokens(LLM_CLOUD, CLOUD_PROVIDER_CLAUDE,
                                                            "claude-sonnet-5", 0, 1000));
   /* Local has no billing. */
   TEST_ASSERT_EQUAL_INT(0, llm_cache_saved_input_tokens(LLM_LOCAL, CLOUD_PROVIDER_NONE, "qwen",
                                                         1000, 0));
}

void test_defaults_without_a_registry(void) {
   llm_pricing_free_registry();
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.10f, price(CLOUD_PROVIDER_CLAUDE, "claude-opus-5-5").read);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.10f, price(CLOUD_PROVIDER_OPENAI, "gpt-4o").read);
   llm_pricing_load_registry(s_root);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_shipped_models_toml_parses);
   RUN_TEST(test_anthropic_prices_per_model);
   RUN_TEST(test_openai_and_gemini_prices_per_model);
   RUN_TEST(test_openrouter_prices_by_vendor);
   RUN_TEST(test_saved_tokens);
   RUN_TEST(test_defaults_without_a_registry);
   llm_pricing_free_registry();
   toml_free(s_root);
   return UNITY_END();
}
