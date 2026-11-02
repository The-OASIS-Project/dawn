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
 * Unit tests for llm_selection.c: the default selection and whether a config can
 * run.
 */

#include <string.h>

#include "config/dawn_config.h"
#include "llm/llm_selection.h"
#include "unity.h"
#include "utils/string_utils.h"

void setUp(void) {
   config_set_defaults(&g_config);
   config_set_secrets_defaults(&g_secrets);
}

void tearDown(void) {
}

static void set_default(const char *type, const char *provider) {
   safe_strscpy(g_config.llm.type, type);
   safe_strscpy(g_config.llm.cloud.provider, provider);
}

static session_llm_config_t cfg(llm_type_t type, cloud_provider_t provider) {
   session_llm_config_t c;
   memset(&c, 0, sizeof(c));
   c.type = type;
   c.cloud_provider = provider;
   return c;
}

/* ---- llm_default_selection ---- */

static void test_local_type_is_local(void) {
   set_default("local", "claude");
   llm_selection_t sel;
   llm_default_selection(&sel);
   TEST_ASSERT_EQUAL_INT(LLM_LOCAL, sel.type);
   TEST_ASSERT_EQUAL_INT(CLOUD_PROVIDER_NONE, sel.provider);
   TEST_ASSERT_EQUAL_STRING(g_config.llm.local.model, sel.model);
}

static void test_cloud_without_a_provider_is_local(void) {
   /* "" is not selected; "local" isn't a cloud provider; config_validate refuses an
    * unknown name at load, but one in memory still selects nothing. */
   const char *unselected[] = { "", "local", "bogus" };
   for (size_t i = 0; i < 3; i++) {
      set_default("cloud", unselected[i]);
      g_secrets.claude_api_key[0] = 'k'; /* a key alone never selects its provider */
      llm_selection_t sel;
      llm_default_selection(&sel);
      TEST_ASSERT_EQUAL_INT_MESSAGE(LLM_LOCAL, sel.type, unselected[i]);
      TEST_ASSERT_EQUAL_INT(CLOUD_PROVIDER_NONE, sel.provider);
   }
}

static void test_cloud_with_a_provider_is_that_provider(void) {
   set_default("cloud", "gemini");
   llm_selection_t sel;
   llm_default_selection(&sel);
   TEST_ASSERT_EQUAL_INT(LLM_CLOUD, sel.type);
   TEST_ASSERT_EQUAL_INT(CLOUD_PROVIDER_GEMINI, sel.provider);
   TEST_ASSERT_EQUAL_STRING(llm_get_default_gemini_model(), sel.model);
}

static void test_local_selection_carries_the_local_model(void) {
   set_default("local", "");
   safe_strscpy(g_config.llm.local.model, "qwen3-4b");
   llm_selection_t sel;
   llm_default_selection(&sel);
   TEST_ASSERT_EQUAL_STRING("qwen3-4b", sel.model);
}

static void test_apply_sets_only_the_selection(void) {
   session_llm_config_t c;
   memset(&c, 0, sizeof(c));
   c.type = LLM_UNDEFINED;
   safe_strscpy(c.reasoning_effort, "high");
   c.suppress_tools = true;
   safe_strscpy(c.endpoint, "http://example.invalid");
   llm_selection_t sel = { .type = LLM_CLOUD, .provider = CLOUD_PROVIDER_CLAUDE };
   safe_strscpy(sel.model, "claude-x");
   llm_selection_apply(&sel, &c);
   TEST_ASSERT_EQUAL_INT(LLM_CLOUD, c.type);
   TEST_ASSERT_EQUAL_INT(CLOUD_PROVIDER_CLAUDE, c.cloud_provider);
   TEST_ASSERT_EQUAL_STRING("claude-x", c.model);
   TEST_ASSERT_EQUAL_STRING("high", c.reasoning_effort);
   TEST_ASSERT_TRUE(c.suppress_tools);
   TEST_ASSERT_EQUAL_STRING("http://example.invalid", c.endpoint);
}

static void test_each_provider_selects_its_own_default_model(void) {
   struct {
      const char *name;
      cloud_provider_t provider;
      const char *(*getter)(void);
   } cases[] = {
      { "openai", CLOUD_PROVIDER_OPENAI, llm_get_default_openai_model },
      { "claude", CLOUD_PROVIDER_CLAUDE, llm_get_default_claude_model },
      { "gemini", CLOUD_PROVIDER_GEMINI, llm_get_default_gemini_model },
      { "openrouter", CLOUD_PROVIDER_OPENROUTER, llm_get_default_openrouter_model },
   };
   /* Distinct lists and indices per provider, so a getter reading another
    * provider's fields picks the wrong name. */
   g_config.llm.cloud.openai_models_count = 2;
   safe_strscpy(g_config.llm.cloud.openai_models[1], "openai-b");
   g_config.llm.cloud.openai_default_model_idx = 1;
   g_config.llm.cloud.claude_models_count = 3;
   safe_strscpy(g_config.llm.cloud.claude_models[2], "claude-c");
   g_config.llm.cloud.claude_default_model_idx = 2;
   g_config.llm.cloud.gemini_models_count = 2;
   safe_strscpy(g_config.llm.cloud.gemini_models[0], "gemini-a");
   g_config.llm.cloud.gemini_default_model_idx = 0;
   g_config.llm.cloud.openrouter_models_count = 2;
   safe_strscpy(g_config.llm.cloud.openrouter_models[1], "openrouter-b");
   g_config.llm.cloud.openrouter_default_model_idx = 1;
   const char *expected[] = { "openai-b", "claude-c", "gemini-a", "openrouter-b" };
   for (size_t i = 0; i < 4; i++) {
      set_default("cloud", cases[i].name);
      llm_selection_t sel;
      llm_default_selection(&sel);
      TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].provider, sel.provider, cases[i].name);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(expected[i], sel.model, cases[i].name);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(expected[i], cases[i].getter(), cases[i].name);
   }
}

/* ---- default models (moved from llm_interface.c, unchanged) ---- */

static void test_default_model_follows_the_index(void) {
   g_config.llm.cloud.claude_models_count = 2;
   safe_strscpy(g_config.llm.cloud.claude_models[0], "claude-a");
   safe_strscpy(g_config.llm.cloud.claude_models[1], "claude-b");
   g_config.llm.cloud.claude_default_model_idx = 1;
   TEST_ASSERT_EQUAL_STRING("claude-b", llm_get_default_claude_model());
   g_config.llm.cloud.claude_default_model_idx = 7; /* out of range: the first */
   TEST_ASSERT_EQUAL_STRING("claude-a", llm_get_default_claude_model());
   g_config.llm.cloud.claude_models_count = 0; /* no list: the built-in */
   TEST_ASSERT_EQUAL_STRING(LLM_DEFAULT_CLAUDE_MODEL, llm_get_default_claude_model());
}

/* ---- llm_selection_problem ---- */

static void test_local_can_run(void) {
   session_llm_config_t c = cfg(LLM_LOCAL, CLOUD_PROVIDER_NONE);
   char reason[256];
   TEST_ASSERT_EQUAL_INT(0, llm_selection_problem(&c, reason, sizeof(reason)));
   TEST_ASSERT_EQUAL_STRING("", reason);
}

static void test_selected_provider_with_its_key_can_run(void) {
   g_secrets.claude_api_key[0] = 'k';
   session_llm_config_t c = cfg(LLM_CLOUD, CLOUD_PROVIDER_CLAUDE);
   TEST_ASSERT_EQUAL_INT(0, llm_selection_problem(&c, NULL, 0));
}

static void test_selected_provider_without_its_key_is_refused(void) {
   /* Another provider's key doesn't help: no substitution. */
   safe_strscpy(g_secrets.openai_api_key, "sk-secret");
   session_llm_config_t c = cfg(LLM_CLOUD, CLOUD_PROVIDER_GEMINI);
   char reason[256];
   TEST_ASSERT_EQUAL_INT(1, llm_selection_problem(&c, reason, sizeof(reason)));
   TEST_ASSERT_NOT_NULL(strstr(reason, "Gemini is selected"));
   TEST_ASSERT_NULL(strstr(reason, "sk-secret"));
}

static void test_cloud_with_no_provider_is_refused(void) {
   g_secrets.openai_api_key[0] = 'k';
   session_llm_config_t c = cfg(LLM_CLOUD, CLOUD_PROVIDER_NONE);
   char reason[256];
   TEST_ASSERT_EQUAL_INT(1, llm_selection_problem(&c, reason, sizeof(reason)));
   TEST_ASSERT_NOT_NULL(strstr(reason, "No cloud provider is selected"));
}

static void test_unknown_provider_value_is_refused(void) {
   /* A corrupt stored row: refused as no provider, never read as a real one. */
   g_secrets.openai_api_key[0] = 'k';
   session_llm_config_t c = cfg(LLM_CLOUD, (cloud_provider_t)99);
   char reason[256];
   TEST_ASSERT_EQUAL_INT(1, llm_selection_problem(&c, reason, sizeof(reason)));
   TEST_ASSERT_NOT_NULL(strstr(reason, "No cloud provider is selected"));
}

static void test_display_names(void) {
   TEST_ASSERT_EQUAL_STRING("OpenRouter", cloud_provider_display_name(CLOUD_PROVIDER_OPENROUTER));
   TEST_ASSERT_EQUAL_STRING("None", cloud_provider_display_name((cloud_provider_t)99));
}

static void test_undefined_is_the_default_selection(void) {
   session_llm_config_t c = cfg(LLM_UNDEFINED, CLOUD_PROVIDER_NONE);

   set_default("cloud", ""); /* no provider selected: local, whatever keys exist */
   g_secrets.openrouter_api_key[0] = 'k';
   TEST_ASSERT_EQUAL_INT(0, llm_selection_problem(&c, NULL, 0));

   set_default("cloud", "claude"); /* selected, no key: refused, not swapped */
   char reason[256];
   TEST_ASSERT_EQUAL_INT(1, llm_selection_problem(&c, reason, sizeof(reason)));
   TEST_ASSERT_NOT_NULL(strstr(reason, "Claude is selected"));

   g_secrets.claude_api_key[0] = 'k';
   TEST_ASSERT_EQUAL_INT(0, llm_selection_problem(&c, NULL, 0));
}

static void test_no_config_is_refused(void) {
   char reason[64];
   TEST_ASSERT_EQUAL_INT(1, llm_selection_problem(NULL, reason, sizeof(reason)));
   TEST_ASSERT_TRUE(reason[0] != '\0');
}

/* [llm.cloud] endpoint belongs to the configured provider only: another
 * provider's key never goes to a host set up for it. */
static void test_cloud_url_endpoint_belongs_to_its_provider(void) {
   set_default("cloud", "openai");
   safe_strscpy(g_config.llm.cloud.endpoint, "http://proxy.example:8000");
   TEST_ASSERT_EQUAL_STRING("http://proxy.example:8000", llm_cloud_url(CLOUD_PROVIDER_OPENAI));
   TEST_ASSERT_EQUAL_STRING(CLAUDE_URL, llm_cloud_url(CLOUD_PROVIDER_CLAUDE));
   TEST_ASSERT_EQUAL_STRING("", llm_cloud_url(CLOUD_PROVIDER_NONE));

   set_default("cloud", ""); /* no provider: the endpoint belongs to none */
   TEST_ASSERT_EQUAL_STRING(CLOUDAI_URL, llm_cloud_url(CLOUD_PROVIDER_OPENAI));

   g_config.llm.cloud.endpoint[0] = '\0';
   set_default("cloud", "gemini");
   TEST_ASSERT_EQUAL_STRING(GEMINI_URL, llm_cloud_url(CLOUD_PROVIDER_GEMINI));
}

static void test_has_key_per_provider(void) {
   TEST_ASSERT_FALSE(llm_provider_has_key(CLOUD_PROVIDER_OPENROUTER));
   g_secrets.openrouter_api_key[0] = 'k';
   TEST_ASSERT_TRUE(llm_provider_has_key(CLOUD_PROVIDER_OPENROUTER));
   TEST_ASSERT_FALSE(llm_provider_has_key(CLOUD_PROVIDER_OPENAI));
   TEST_ASSERT_FALSE(llm_provider_has_key(CLOUD_PROVIDER_NONE));
}

/* A conversation row: none of its own follows the default; a stored one is kept. */
static void test_row_reader(void) {
   session_llm_config_t c;
   llm_selection_from_row("", "", "", "", "", &c);
   TEST_ASSERT_EQUAL(LLM_UNDEFINED, c.type);
   TEST_ASSERT_EQUAL_STRING("", c.model);
   TEST_ASSERT_TRUE(c.thinking_mode[0] != '\0'); /* the configured default's */

   llm_selection_from_row(NULL, NULL, NULL, "enabled", "high", &c);
   TEST_ASSERT_EQUAL(LLM_UNDEFINED, c.type);
   TEST_ASSERT_EQUAL_STRING("enabled", c.thinking_mode);
   TEST_ASSERT_EQUAL_STRING("high", c.reasoning_effort);

   llm_selection_from_row("cloud", "claude", "claude-x", "", "", &c);
   TEST_ASSERT_EQUAL(LLM_CLOUD, c.type);
   TEST_ASSERT_EQUAL(CLOUD_PROVIDER_CLAUDE, c.cloud_provider);
   TEST_ASSERT_EQUAL_STRING("claude-x", c.model);

   llm_selection_from_row("local", "none", "qwen", "", "", &c);
   TEST_ASSERT_EQUAL(LLM_LOCAL, c.type);
   TEST_ASSERT_EQUAL_STRING("qwen", c.model);

   /* Legacy cloud rows: the provider from the model's prefix, else none of its own. */
   llm_selection_from_row("cloud", "", "gpt-5", "", "", &c);
   TEST_ASSERT_EQUAL(CLOUD_PROVIDER_OPENAI, c.cloud_provider);
   llm_selection_from_row("cloud", "OpenRouter", "anthropic/claude-x", "", "", &c);
   TEST_ASSERT_EQUAL(CLOUD_PROVIDER_OPENROUTER, c.cloud_provider);
   llm_selection_from_row("cloud", "none", "mystery", "", "", &c);
   TEST_ASSERT_EQUAL(LLM_UNDEFINED, c.type);
   TEST_ASSERT_EQUAL_STRING("", c.model);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_local_type_is_local);
   RUN_TEST(test_cloud_without_a_provider_is_local);
   RUN_TEST(test_cloud_with_a_provider_is_that_provider);
   RUN_TEST(test_local_selection_carries_the_local_model);
   RUN_TEST(test_each_provider_selects_its_own_default_model);
   RUN_TEST(test_apply_sets_only_the_selection);
   RUN_TEST(test_default_model_follows_the_index);
   RUN_TEST(test_local_can_run);
   RUN_TEST(test_selected_provider_with_its_key_can_run);
   RUN_TEST(test_selected_provider_without_its_key_is_refused);
   RUN_TEST(test_cloud_with_no_provider_is_refused);
   RUN_TEST(test_unknown_provider_value_is_refused);
   RUN_TEST(test_display_names);
   RUN_TEST(test_undefined_is_the_default_selection);
   RUN_TEST(test_no_config_is_refused);
   RUN_TEST(test_has_key_per_provider);
   RUN_TEST(test_cloud_url_endpoint_belongs_to_its_provider);
   RUN_TEST(test_row_reader);
   return UNITY_END();
}
