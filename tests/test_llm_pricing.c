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
   /* GPT-5.6 Cyber has its own, smaller window than the gpt-5.6 row; each GPT-6
    * model is listed by name. */
   toml_table_t *openai = toml_table_in(s_root, "openai");
   TEST_ASSERT_EQUAL_INT64(400000, toml_int_in(openai, "gpt-5.6-cyber").u.i);
   const char *gpt6[] = { "gpt-6-astra",   "gpt-6-sol",  "gpt-6.1-sol",
                          "gpt-6.1-terra", "gpt-6-luna", "gpt-6-cyber" };
   for (size_t i = 0; i < sizeof(gpt6) / sizeof(gpt6[0]); i++) {
      TEST_ASSERT_EQUAL_INT64_MESSAGE(1050000, toml_int_in(openai, gpt6[i]).u.i, gpt6[i]);
   }
}

void test_anthropic_prices_per_model(void) {
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.05f, price(CLOUD_PROVIDER_CLAUDE, "claude-opus-5-5").read);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.025f, price(CLOUD_PROVIDER_CLAUDE, "claude-fable-5-1").read);
   const llm_cache_pricing_t fable52 = price(CLOUD_PROVIDER_CLAUDE, "claude-fable-5-2");
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.025f, fable52.read);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.25f, fable52.write_5m);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.00f, fable52.write_1h);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.025f, price(CLOUD_PROVIDER_CLAUDE, "claude-mythos-5-2").read);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.025f, price(CLOUD_PROVIDER_CLAUDE, "claude-mythos-5-1").read);
   /* Sonnet 5.5: $0.10 reads, $2.50 / $4 writes, on $2. */
   const llm_cache_pricing_t sonnet55 = price(CLOUD_PROVIDER_CLAUDE, "claude-sonnet-5-5");
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.05f, sonnet55.read);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.25f, sonnet55.write_5m);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.00f, sonnet55.write_1h);
   /* Fable 5 and Mythos 5 read at $1 on $10: the default. */
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.10f, price(CLOUD_PROVIDER_CLAUDE, "claude-fable-5").read);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.10f, price(CLOUD_PROVIDER_CLAUDE, "claude-mythos-5").read);
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

void test_gpt6_astra_and_gemini_4_prices(void) {
   const llm_cache_pricing_t astra = price(CLOUD_PROVIDER_OPENAI, "gpt-6-astra");
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.10f, astra.read);     /* $1 on $10 */
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.25f, astra.write_5m); /* the default 30-minute write */
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.50f, astra.write_1h); /* the 12-hour write */
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.50f, price(CLOUD_PROVIDER_OPENAI, "gpt-6.1-sol").write_1h);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.50f, price(CLOUD_PROVIDER_OPENAI, "gpt-5.6-luna").write_1h);
   /* Before GPT-5.6, writes cost nothing extra. */
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.00f, price(CLOUD_PROVIDER_OPENAI, "gpt-5.5").write_1h);
   /* The rest of GPT-6: 6.1 reads at 0.05x, the others 0.10x; writes 1.25x. */
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.10f, price(CLOUD_PROVIDER_OPENAI, "gpt-6-sol").read);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.05f, price(CLOUD_PROVIDER_OPENAI, "gpt-6.1-sol").read);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.05f, price(CLOUD_PROVIDER_OPENAI, "gpt-6.1-terra").read);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.10f, price(CLOUD_PROVIDER_OPENAI, "gpt-6-luna").read);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.25f, price(CLOUD_PROVIDER_OPENAI, "gpt-6-cyber").write_5m);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.05f,
                            price(CLOUD_PROVIDER_OPENROUTER, "openai/gpt-6.1-terra").read);
   /* Gemini 4: implicit caching only, reads at 0.05x, no write premium. */
   const llm_cache_pricing_t hydrogen = price(CLOUD_PROVIDER_GEMINI, "gemini-4-hydrogen");
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.05f, hydrogen.read);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.00f, hydrogen.write_5m);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.05f,
                            price(CLOUD_PROVIDER_OPENROUTER, "google/gemini-4-carbon").read);
   /* Gemini 3 keeps the family row. */
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.10f, price(CLOUD_PROVIDER_GEMINI, "gemini-3.7-flash").read);
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
   RUN_TEST(test_gpt6_astra_and_gemini_4_prices);
   RUN_TEST(test_openrouter_prices_by_vendor);
   RUN_TEST(test_saved_tokens);
   RUN_TEST(test_defaults_without_a_registry);
   llm_pricing_free_registry();
   toml_free(s_root);
   return UNITY_END();
}
