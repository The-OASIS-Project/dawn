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
 * Unit tests for per-model base and prompt-cache prices, against the shipped
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
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.25f, price(CLOUD_PROVIDER_OPENAI, "o3").read);
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

/* Every row of the C table base prices used to come from, and the models that
 * fell through to a shorter one, give the same price from models.toml. */
void test_base_rates_match_the_old_table(void) {
   const struct {
      cloud_provider_t provider;
      const char *model;
      double in, out;
   } rows[] = {
      { CLOUD_PROVIDER_CLAUDE, "claude-fable-5-2", 8.00, 40.00 },
      { CLOUD_PROVIDER_CLAUDE, "claude-fable-5", 10.00, 50.00 },
      { CLOUD_PROVIDER_CLAUDE, "claude-mythos-5-2", 8.00, 40.00 },
      { CLOUD_PROVIDER_CLAUDE, "claude-mythos-5", 10.00, 50.00 },
      { CLOUD_PROVIDER_CLAUDE, "claude-haiku-5-5", 0.10, 0.50 },
      { CLOUD_PROVIDER_CLAUDE, "claude-haiku-4-5", 1.00, 5.00 },
      { CLOUD_PROVIDER_CLAUDE, "claude-3-5-haiku", 0.80, 4.00 },
      { CLOUD_PROVIDER_CLAUDE, "claude-sonnet-5", 2.00, 10.00 },
      { CLOUD_PROVIDER_CLAUDE, "claude-sonnet", 3.00, 15.00 },
      { CLOUD_PROVIDER_CLAUDE, "claude-opus-5-5", 4.00, 20.00 },
      { CLOUD_PROVIDER_CLAUDE, "claude-opus", 5.00, 25.00 },
      { CLOUD_PROVIDER_CLAUDE, "claude", 1.00, 5.00 },
      { CLOUD_PROVIDER_OPENAI, "gpt-6-astra", 10.00, 50.00 },
      { CLOUD_PROVIDER_OPENAI, "gpt-6-cyber", 20.00, 100.00 },
      { CLOUD_PROVIDER_OPENAI, "gpt-6-sol", 2.00, 10.00 },
      { CLOUD_PROVIDER_OPENAI, "gpt-6.1-sol", 2.00, 10.00 },
      { CLOUD_PROVIDER_OPENAI, "gpt-6.1-terra", 1.00, 5.00 },
      { CLOUD_PROVIDER_OPENAI, "gpt-6-luna", 0.10, 0.50 },
      { CLOUD_PROVIDER_OPENAI, "gpt-5.6-cyber", 12.50, 75.00 },
      { CLOUD_PROVIDER_OPENAI, "gpt-4o-mini", 0.15, 0.60 },
      { CLOUD_PROVIDER_OPENAI, "gpt-4o", 2.50, 10.00 },
      { CLOUD_PROVIDER_OPENAI, "gpt-5", 1.25, 10.00 },
      /* Models the old table priced by a shorter row. */
      { CLOUD_PROVIDER_CLAUDE, "claude-fable-5-1", 10.00, 50.00 },
      { CLOUD_PROVIDER_CLAUDE, "claude-mythos-5-1", 10.00, 50.00 },
      { CLOUD_PROVIDER_CLAUDE, "claude-sonnet-5-5", 2.00, 10.00 },
      { CLOUD_PROVIDER_CLAUDE, "claude-sonnet-4-6", 3.00, 15.00 },
      { CLOUD_PROVIDER_CLAUDE, "claude-opus-5", 5.00, 25.00 },
      { CLOUD_PROVIDER_CLAUDE, "claude-opus-4-8", 5.00, 25.00 },
      { CLOUD_PROVIDER_CLAUDE, "claude-haiku-5-5-20261001", 0.10, 0.50 },
      { CLOUD_PROVIDER_CLAUDE, "claude-3-7-sonnet", 1.00, 5.00 },
      { CLOUD_PROVIDER_OPENAI, "gpt-4o-2024-08-06", 2.50, 10.00 },
      /* Models the old table priced wrong by a shorter row, now OpenAI's list price. */
      { CLOUD_PROVIDER_OPENAI, "gpt-5.6-sol", 4.00, 20.00 },
      { CLOUD_PROVIDER_OPENAI, "gpt-5.6-terra", 2.00, 12.00 },
      { CLOUD_PROVIDER_OPENAI, "gpt-5.6-luna", 0.20, 1.20 },
      { CLOUD_PROVIDER_OPENAI, "gpt-5.5", 5.00, 30.00 },
      { CLOUD_PROVIDER_OPENAI, "gpt-4.1", 2.00, 8.00 },
      { CLOUD_PROVIDER_OPENAI, "gpt-6.2-astra", 8.00, 40.00 },
      { CLOUD_PROVIDER_OPENAI, "gpt-6.2-sol", 2.00, 10.00 },
      { CLOUD_PROVIDER_OPENAI, "gpt-5.5-pro", 30.00, 180.00 },
      { CLOUD_PROVIDER_OPENAI, "gpt-5.4-pro", 30.00, 180.00 },
      { CLOUD_PROVIDER_OPENAI, "gpt-5.4-mini", 0.75, 4.50 },
      { CLOUD_PROVIDER_OPENAI, "gpt-5.4-nano", 0.20, 1.25 },
      { CLOUD_PROVIDER_OPENAI, "gpt-5.4", 2.50, 15.00 },
      { CLOUD_PROVIDER_OPENAI, "gpt-5.2-pro", 21.00, 168.00 },
      { CLOUD_PROVIDER_OPENAI, "gpt-5.2", 1.75, 14.00 },
      { CLOUD_PROVIDER_OPENAI, "gpt-5.1", 1.25, 10.00 },
      { CLOUD_PROVIDER_OPENAI, "gpt-5-pro", 15.00, 120.00 },
      { CLOUD_PROVIDER_OPENAI, "gpt-5-mini", 0.25, 2.00 },
      { CLOUD_PROVIDER_OPENAI, "gpt-5-nano", 0.05, 0.40 },
      { CLOUD_PROVIDER_OPENAI, "gpt-4.1-mini", 0.40, 1.60 },
      { CLOUD_PROVIDER_OPENAI, "o3-pro", 20.00, 80.00 },
      { CLOUD_PROVIDER_OPENAI, "o3", 2.00, 8.00 },
   };
   for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
      double in = -1.0, out = -1.0;
      TEST_ASSERT_TRUE_MESSAGE(
          llm_pricing_rate(LLM_CLOUD, rows[i].provider, rows[i].model, &in, &out), rows[i].model);
      TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1e-9, rows[i].in, in, rows[i].model);
      TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1e-9, rows[i].out, out, rows[i].model);
   }
}

void test_unpriced_models_get_no_rate(void) {
   double in = -1.0, out = -1.0;
   /* No row covers them. */
   TEST_ASSERT_FALSE(
       llm_pricing_rate(LLM_CLOUD, CLOUD_PROVIDER_OPENAI, "gpt-image-2.6-flare", &in, &out));
   TEST_ASSERT_FALSE(
       llm_pricing_rate(LLM_CLOUD, CLOUD_PROVIDER_GEMINI, "gemini-3-flash", &in, &out));
   TEST_ASSERT_FALSE(llm_pricing_rate(LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, "", &in, &out));
   TEST_ASSERT_FALSE(llm_pricing_rate(LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, NULL, &in, &out));
   TEST_ASSERT_FALSE(llm_pricing_rate(LLM_LOCAL, CLOUD_PROVIDER_NONE, "qwen3", &in, &out));
   TEST_ASSERT_FALSE(
       llm_pricing_rate(LLM_CLOUD, CLOUD_PROVIDER_OPENROUTER, "meta/llama-4", &in, &out));
   /* Outputs untouched on false. */
   TEST_ASSERT_EQUAL_DOUBLE(-1.0, in);
   TEST_ASSERT_EQUAL_DOUBLE(-1.0, out);
   /* An OpenRouter model of a priced vendor is priced as its vendor prices it. */
   TEST_ASSERT_TRUE(llm_pricing_rate(LLM_CLOUD, CLOUD_PROVIDER_OPENROUTER,
                                     "anthropic/claude-haiku-4.5", &in, &out));
   TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.00, in);
}

/* Every row of the old [cache_pricing] table, and the models that took a
 * shorter row's multiples, read the same from [pricing]. */
void test_cache_multiples_match_the_old_table(void) {
   const struct {
      cloud_provider_t provider;
      const char *model;
      float read, write_5m, write_1h;
   } rows[] = {
      { CLOUD_PROVIDER_CLAUDE, "claude-opus-5-5", 0.05f, 1.25f, 2.0f },
      { CLOUD_PROVIDER_CLAUDE, "claude-sonnet-5-5", 0.05f, 1.25f, 2.0f },
      { CLOUD_PROVIDER_CLAUDE, "claude-fable-5-2", 0.025f, 1.25f, 2.0f },
      { CLOUD_PROVIDER_CLAUDE, "claude-fable-5-1", 0.025f, 1.25f, 2.0f },
      { CLOUD_PROVIDER_CLAUDE, "claude-mythos-5-2", 0.025f, 1.25f, 2.0f },
      { CLOUD_PROVIDER_CLAUDE, "claude-mythos-5-1", 0.025f, 1.25f, 2.0f },
      { CLOUD_PROVIDER_CLAUDE, "claude", 0.10f, 1.25f, 2.0f },
      { CLOUD_PROVIDER_OPENAI, "gpt-6-astra", 0.10f, 1.25f, 2.5f },
      { CLOUD_PROVIDER_OPENAI, "gpt-6-sol", 0.10f, 1.25f, 2.5f },
      { CLOUD_PROVIDER_OPENAI, "gpt-6.1-sol", 0.05f, 1.25f, 2.5f },
      { CLOUD_PROVIDER_OPENAI, "gpt-6.1-terra", 0.05f, 1.25f, 2.5f },
      { CLOUD_PROVIDER_OPENAI, "gpt-6-luna", 0.10f, 1.25f, 2.5f },
      { CLOUD_PROVIDER_OPENAI, "gpt-6-cyber", 0.10f, 1.25f, 2.5f },
      { CLOUD_PROVIDER_OPENAI, "gpt-6.2-astra", 0.05f, 1.25f, 2.5f },
      { CLOUD_PROVIDER_OPENAI, "gpt-6.2-sol", 0.05f, 1.25f, 2.5f },
      { CLOUD_PROVIDER_OPENAI, "gpt-5.6", 0.10f, 1.25f, 2.5f },
      { CLOUD_PROVIDER_OPENAI, "gpt-5", 0.10f, 1.0f, 1.0f },
      { CLOUD_PROVIDER_OPENAI, "gpt-4.1", 0.25f, 1.0f, 1.0f },
      { CLOUD_PROVIDER_OPENAI, "o3", 0.25f, 1.0f, 1.0f },
      { CLOUD_PROVIDER_OPENAI, "gpt-4o", 0.50f, 1.0f, 1.0f },
      { CLOUD_PROVIDER_GEMINI, "gemini-4", 0.05f, 1.0f, 1.0f },
      { CLOUD_PROVIDER_GEMINI, "gemini", 0.10f, 1.0f, 1.0f },
      /* Rows with base prices only take a shorter row's multiples. */
      { CLOUD_PROVIDER_OPENAI, "gpt-5.6-cyber", 0.10f, 1.25f, 2.5f },
      { CLOUD_PROVIDER_OPENAI, "gpt-5.6-sol", 0.10f, 1.25f, 2.5f },
      { CLOUD_PROVIDER_OPENAI, "gpt-5.5", 0.10f, 1.0f, 1.0f },
      { CLOUD_PROVIDER_OPENAI, "gpt-5.4-mini", 0.10f, 1.0f, 1.0f },
      { CLOUD_PROVIDER_OPENAI, "gpt-5-nano", 0.10f, 1.0f, 1.0f },
      { CLOUD_PROVIDER_OPENAI, "gpt-4.1-mini", 0.25f, 1.0f, 1.0f },
      { CLOUD_PROVIDER_OPENAI, "gpt-4o-mini", 0.50f, 1.0f, 1.0f },
      { CLOUD_PROVIDER_CLAUDE, "claude-haiku-4-5", 0.10f, 1.25f, 2.0f },
      { CLOUD_PROVIDER_CLAUDE, "claude-fable-5", 0.10f, 1.25f, 2.0f },
      /* No row at all: the provider's default. */
      { CLOUD_PROVIDER_OPENAI, "gpt-4", 0.10f, 1.0f, 1.0f },
   };
   for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
      const llm_cache_pricing_t p = price(rows[i].provider, rows[i].model);
      TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, rows[i].read, p.read, rows[i].model);
      TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, rows[i].write_5m, p.write_5m, rows[i].model);
      TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, rows[i].write_1h, p.write_1h, rows[i].model);
   }
}

static toml_table_t *parse_text(const char *text) {
   char buf[1024];
   snprintf(buf, sizeof(buf), "%s", text);
   char err[200];
   toml_table_t *root = toml_parse(buf, err, sizeof(err));
   TEST_ASSERT_NOT_NULL_MESSAGE(root, err);
   return root;
}

void test_a_row_needs_both_base_prices(void) {
   toml_table_t *root = parse_text("[pricing.anthropic]\n"
                                   "\"claude-a\" = { input = 1.0 }\n"
                                   "\"claude-b\" = { input = 2, output = 8, read = 0.2 }\n"
                                   "\"claude-c\" = { input = -1.0, output = 5.0 }\n");
   llm_pricing_load_registry(root);
   double in = -1.0, out = -1.0;
   TEST_ASSERT_FALSE(llm_pricing_rate(LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, "claude-a", &in, &out));
   TEST_ASSERT_FALSE(llm_pricing_rate(LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, "claude-c", &in, &out));
   TEST_ASSERT_TRUE(llm_pricing_rate(LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, "claude-b", &in, &out));
   TEST_ASSERT_DOUBLE_WITHIN(1e-9, 2.0, in); /* an integer price reads too */
   TEST_ASSERT_DOUBLE_WITHIN(1e-9, 8.0, out);
   /* A cache multiple a row leaves out is the provider's default. */
   const llm_cache_pricing_t b = price(CLOUD_PROVIDER_CLAUDE, "claude-b");
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.2f, b.read);
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.25f, b.write_5m);
   /* A row without cache multiples takes the default ones. */
   TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.10f, price(CLOUD_PROVIDER_CLAUDE, "claude-a").read);
   toml_free(root);
   llm_pricing_load_registry(s_root);
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
   RUN_TEST(test_base_rates_match_the_old_table);
   RUN_TEST(test_unpriced_models_get_no_rate);
   RUN_TEST(test_cache_multiples_match_the_old_table);
   RUN_TEST(test_a_row_needs_both_base_prices);
   RUN_TEST(test_defaults_without_a_registry);
   llm_pricing_free_registry();
   toml_free(s_root);
   return UNITY_END();
}
