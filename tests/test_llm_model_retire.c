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
 * Retired models: matching, dates, chains, and refused rows.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "llm/llm_model_retire.h"
#include "toml.h"
#include "unity.h"

static toml_table_t *s_root;

static void load(const char *text) {
   char err[200];
   char *copy = strdup(text);
   s_root = toml_parse(copy, err, sizeof(err));
   free(copy);
   TEST_ASSERT_NOT_NULL_MESSAGE(s_root, err);
   llm_model_retire_load(s_root);
}

void setUp(void) {
   s_root = NULL;
}

void tearDown(void) {
   toml_free(s_root);
   llm_model_retire_load(NULL);
}

static const time_t NOV_2026 = 1793577600; /* 2026-11-02 00:00 UTC */
#define SECONDS_PER_DAY 86400

static void test_retired_model_and_its_snapshots_are_replaced(void) {
   load("[retired.anthropic]\n\"claude-haiku-4-5\" = { by = \"claude-haiku-5-5\" }\n");
   TEST_ASSERT_EQUAL_STRING("claude-haiku-5-5",
                            llm_model_replacement(CLOUD_PROVIDER_CLAUDE, "claude-haiku-4-5",
                                                  NOV_2026));
   TEST_ASSERT_EQUAL_STRING("claude-haiku-5-5",
                            llm_model_replacement(CLOUD_PROVIDER_CLAUDE,
                                                  "claude-haiku-4-5-20251001", NOV_2026));
   /* Another model sharing the prefix, another provider, an active model: kept. */
   TEST_ASSERT_NULL(llm_model_replacement(CLOUD_PROVIDER_CLAUDE, "claude-haiku-4-5x", NOV_2026));
   TEST_ASSERT_NULL(llm_model_replacement(CLOUD_PROVIDER_OPENAI, "claude-haiku-4-5", NOV_2026));
   TEST_ASSERT_NULL(llm_model_replacement(CLOUD_PROVIDER_CLAUDE, "claude-haiku-5-5", NOV_2026));
}

static void test_a_family_name_doesnt_catch_its_newer_members(void) {
   load("[retired.openai]\n\"gpt-4\" = { by = \"gpt-5.5\" }\n");
   TEST_ASSERT_EQUAL_STRING("gpt-5.5",
                            llm_model_replacement(CLOUD_PROVIDER_OPENAI, "gpt-4-0613", NOV_2026));
   TEST_ASSERT_NULL(llm_model_replacement(CLOUD_PROVIDER_OPENAI, "gpt-4o", NOV_2026));
   TEST_ASSERT_NULL(llm_model_replacement(CLOUD_PROVIDER_OPENAI, "gpt-4-turbo", NOV_2026));
   TEST_ASSERT_NULL(llm_model_replacement(CLOUD_PROVIDER_OPENAI, "gpt-4.1", NOV_2026));
}

static void test_a_version_isnt_a_snapshot(void) {
   load("[retired.anthropic]\n\"claude-opus-4\" = { by = \"claude-opus-4-8\" }\n");
   TEST_ASSERT_EQUAL_STRING("claude-opus-4-8",
                            llm_model_replacement(CLOUD_PROVIDER_CLAUDE, "claude-opus-4-20250514",
                                                  NOV_2026));
   TEST_ASSERT_NULL(llm_model_replacement(CLOUD_PROVIDER_CLAUDE, "claude-opus-4-8", NOV_2026));
   TEST_ASSERT_NULL(llm_model_replacement(CLOUD_PROVIDER_CLAUDE, "claude-opus-4-5", NOV_2026));
}

static void test_retires_on_its_date(void) {
   load("[retired.anthropic]\n\"claude-x\" = { by = \"claude-y\", on = \"2026-11-15\" }\n");
   TEST_ASSERT_NULL(llm_model_replacement(CLOUD_PROVIDER_CLAUDE, "claude-x", NOV_2026));
   TEST_ASSERT_EQUAL_STRING("claude-y", llm_model_replacement(CLOUD_PROVIDER_CLAUDE, "claude-x",
                                                              NOV_2026 + 13 * SECONDS_PER_DAY));
}

static void test_a_retired_replacement_is_followed(void) {
   load("[retired.anthropic]\n"
        "\"claude-a\" = { by = \"claude-b\" }\n"
        "\"claude-b\" = { by = \"claude-c\" }\n");
   TEST_ASSERT_EQUAL_STRING("claude-c",
                            llm_model_replacement(CLOUD_PROVIDER_CLAUDE, "claude-a", NOV_2026));
}

static void test_bad_rows_are_ignored(void) {
   load("[retired.openrouter]\n"
        "\"anthropic/claude-a\" = { by = \"openai/gpt-5.5\" }\n" /* another vendor */
        "\"anthropic/claude-b\" = { by = \"anthropic/claude-c\" }\n"
        "\"x\" = { by = \"y\" }\n"                          /* no vendor */
        "\"openrouter/x\" = { by = \"openrouter/auto\" }\n" /* any vendor */
        "[retired.anthropic]\n"
        "\"claude-d\" = { on = \"2026-01-01\" }\n" /* no replacement */
        "\"claude-e\" = { by = \"claude-f\", on = \"soon\" }\n"
        "\"claude-g\" = { by = \"claude-h\", on = 2027-06-01 }\n" /* unquoted date */
        "\"claude-i\" = { by = \"claude-j\", date = \"2027-06-01\" }\n"
        "\"claude-k\" = { by = \"claude-l\", on = \"2027-02-31\" }\n");
   TEST_ASSERT_NULL(
       llm_model_replacement(CLOUD_PROVIDER_OPENROUTER, "anthropic/claude-a", NOV_2026));
   TEST_ASSERT_EQUAL_STRING("anthropic/claude-c",
                            llm_model_replacement(CLOUD_PROVIDER_OPENROUTER, "anthropic/claude-b",
                                                  NOV_2026));
   TEST_ASSERT_NULL(llm_model_replacement(CLOUD_PROVIDER_OPENROUTER, "x", NOV_2026));
   TEST_ASSERT_NULL(llm_model_replacement(CLOUD_PROVIDER_OPENROUTER, "openrouter/x", NOV_2026));
   TEST_ASSERT_NULL(llm_model_replacement(CLOUD_PROVIDER_CLAUDE, "claude-d", NOV_2026));
   TEST_ASSERT_NULL(llm_model_replacement(CLOUD_PROVIDER_CLAUDE, "claude-e", NOV_2026));
   TEST_ASSERT_NULL(llm_model_replacement(CLOUD_PROVIDER_CLAUDE, "claude-g", NOV_2026));
   TEST_ASSERT_NULL(llm_model_replacement(CLOUD_PROVIDER_CLAUDE, "claude-i", NOV_2026));
   TEST_ASSERT_NULL(llm_model_replacement(CLOUD_PROVIDER_CLAUDE, "claude-k", NOV_2026));
}

/* The shipped table: every row loads, retired models move on, active ones stay. */
static void test_shipped_table(void) {
   FILE *fp = fopen(MODELS_TOML_PATH, "r");
   TEST_ASSERT_NOT_NULL(fp);
   char err[200];
   s_root = toml_parse_file(fp, err, sizeof(err));
   fclose(fp);
   TEST_ASSERT_NOT_NULL_MESSAGE(s_root, err);
   llm_model_retire_load(s_root);
   const time_t in_ten_years = NOV_2026 + 3650 * SECONDS_PER_DAY; /* every dated row passed */
   toml_table_t *retired = toml_table_in(s_root, "retired");
   TEST_ASSERT_NOT_NULL(retired);
   static const struct {
      const char *table;
      cloud_provider_t provider;
   } tables[] = { { "anthropic", CLOUD_PROVIDER_CLAUDE },
                  { "openai", CLOUD_PROVIDER_OPENAI },
                  { "gemini", CLOUD_PROVIDER_GEMINI } };
   for (size_t i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
      toml_table_t *table = toml_table_in(retired, tables[i].table);
      const char *model = NULL;
      for (int k = 0; table && (model = toml_key_in(table, k)) != NULL; k++) {
         const char *by = llm_model_replacement(tables[i].provider, model, in_ten_years);
         TEST_ASSERT_NOT_NULL_MESSAGE(by, model); /* the row loaded */
         TEST_ASSERT_NULL_MESSAGE(llm_model_replacement(tables[i].provider, by, in_ten_years), by);
      }
   }
   TEST_ASSERT_EQUAL_STRING("claude-opus-4-8",
                            llm_model_replacement(CLOUD_PROVIDER_CLAUDE, "claude-3-opus-20240229",
                                                  NOV_2026));
   TEST_ASSERT_NULL(llm_model_replacement(CLOUD_PROVIDER_CLAUDE, "claude-haiku-5-5", in_ten_years));
   TEST_ASSERT_NULL(
       llm_model_replacement(CLOUD_PROVIDER_CLAUDE, "claude-sonnet-4-6", in_ten_years));
   TEST_ASSERT_NULL(llm_model_replacement(CLOUD_PROVIDER_OPENAI, "gpt-5.6-luna", in_ten_years));
   TEST_ASSERT_NULL(llm_model_replacement(CLOUD_PROVIDER_OPENAI, "gpt-5.5", in_ten_years));

   /* DAWN's own default model lists offer nothing retired today. */
   dawn_config_t cfg;
   config_set_defaults(&cfg);
   const llm_cloud_config_t *c = &cfg.llm.cloud;
   for (int i = 0; i < c->openai_models_count; i++) {
      TEST_ASSERT_NULL_MESSAGE(llm_model_replacement(CLOUD_PROVIDER_OPENAI, c->openai_models[i],
                                                     NOV_2026),
                               c->openai_models[i]);
   }
   for (int i = 0; i < c->claude_models_count; i++) {
      TEST_ASSERT_NULL_MESSAGE(llm_model_replacement(CLOUD_PROVIDER_CLAUDE, c->claude_models[i],
                                                     NOV_2026),
                               c->claude_models[i]);
   }
   for (int i = 0; i < c->gemini_models_count; i++) {
      TEST_ASSERT_NULL_MESSAGE(llm_model_replacement(CLOUD_PROVIDER_GEMINI, c->gemini_models[i],
                                                     NOV_2026),
                               c->gemini_models[i]);
   }
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_retired_model_and_its_snapshots_are_replaced);
   RUN_TEST(test_a_family_name_doesnt_catch_its_newer_members);
   RUN_TEST(test_a_version_isnt_a_snapshot);
   RUN_TEST(test_retires_on_its_date);
   RUN_TEST(test_a_retired_replacement_is_followed);
   RUN_TEST(test_bad_rows_are_ignored);
   RUN_TEST(test_shipped_table);
   return UNITY_END();
}
