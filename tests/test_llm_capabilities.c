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
 * Unit tests for per-model reasoning capabilities and the mode/effort resolver,
 * against the shipped models.toml parsed by DAWN's own TOML reader.
 */

#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

#include "config/dawn_config.h"
#include "llm/llm_capabilities.h"
#include "llm/llm_local_provider.h"
#include "toml.h"
#include "unity.h"

/* ---- stubs ---- */
dawn_config_t g_config;
static local_provider_t s_local = LOCAL_PROVIDER_LLAMA_CPP;
local_provider_t llm_local_get_provider(void) {
   return s_local;
}
local_provider_t llm_local_detect_provider(const char *endpoint) {
   (void)endpoint;
   return llm_local_get_provider();
}
const char *llm_get_current_thinking_mode(void) {
   return "disabled";
}
const char *llm_get_current_reasoning_effort(void) {
   return "medium";
}
static bool s_suppressed;
static const char *s_utility_effort = "";
bool llm_tools_suppressed(void) {
   return s_suppressed;
}
const char *llm_get_current_utility_effort(void) {
   return s_utility_effort;
}

static toml_table_t *s_root;

void setUp(void) {
   s_suppressed = false;
   s_utility_effort = "";
   s_local = LOCAL_PROVIDER_LLAMA_CPP;
}
void tearDown(void) {
}

static llm_thinking_resolved_t resolve(cloud_provider_t provider,
                                       const char *model,
                                       const char *mode,
                                       const char *effort) {
   llm_thinking_caps_t caps;
   llm_thinking_caps(LLM_CLOUD, provider, model, &caps);
   llm_thinking_resolved_t r;
   llm_thinking_resolve(&caps, mode, effort, false, &r);
   return r;
}

static void assert_resolved(llm_thinking_resolved_t r,
                            llm_think_mode_t mode,
                            const char *effort,
                            bool clamped) {
   TEST_ASSERT_TRUE(r.controllable);
   TEST_ASSERT_EQUAL_STRING(llm_think_mode_name(mode), llm_think_mode_name(r.mode));
   TEST_ASSERT_EQUAL_STRING(effort, r.effort);
   TEST_ASSERT_EQUAL(clamped, r.clamped);
}

static void test_adaptive_only_claude_has_no_off_switch(void) {
   llm_thinking_caps_t caps;
   llm_thinking_caps(LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, "claude-opus-5-5", &caps);
   TEST_ASSERT_EQUAL_INT(LLM_CAPS_ROW, caps.source);
   TEST_ASSERT_EQUAL_INT(1, caps.mode_count);
   TEST_ASSERT_EQUAL_STRING("adaptive", llm_think_mode_name(caps.modes[0].mode));
   TEST_ASSERT_EQUAL_INT(5, caps.modes[0].effort_count);
   TEST_ASSERT_EQUAL_STRING("low", caps.modes[0].efforts[0]);
   TEST_ASSERT_EQUAL_STRING("max", caps.modes[0].efforts[4]);

   /* "disabled" becomes the gentlest reasoning, and says so. */
   assert_resolved(resolve(CLOUD_PROVIDER_CLAUDE, "claude-opus-5-5", "disabled", "high"),
                   LLM_THINK_ADAPTIVE, "low", true);
   /* Fable 5 and 5.1 share the row. */
   assert_resolved(
       resolve(CLOUD_PROVIDER_CLAUDE, "claude-fable-5-1", "disabled", ""), LLM_THINK_ADAPTIVE,
       "low", true); /* Sonnet 5.5 can't turn it off either: its row, not Sonnet 5's, applies. */
   assert_resolved(resolve(CLOUD_PROVIDER_CLAUDE, "claude-sonnet-5-5", "disabled", "high"),
                   LLM_THINK_ADAPTIVE, "low", true);
}

static void test_claude_that_accepts_disabled(void) {
   assert_resolved(resolve(CLOUD_PROVIDER_CLAUDE, "claude-sonnet-5", "disabled", "high"),
                   LLM_THINK_DISABLED, "", false);
   /* The WebUI's "enabled" and the legacy "auto" mean reasoning on. */
   assert_resolved(resolve(CLOUD_PROVIDER_CLAUDE, "claude-sonnet-5", "enabled", "high"),
                   LLM_THINK_ADAPTIVE, "high", false);
   assert_resolved(resolve(CLOUD_PROVIDER_CLAUDE, "claude-opus-5", "auto", "max"),
                   LLM_THINK_ADAPTIVE, "max", false);
   /* The longer row wins: opus-5-5 is not opus-5. */
   assert_resolved(resolve(CLOUD_PROVIDER_CLAUDE, "claude-opus-5-5", "enabled", "max"),
                   LLM_THINK_ADAPTIVE, "max", false);
}

static void test_budget_claude_uses_budget_levels(void) {
   llm_thinking_caps_t caps;
   llm_thinking_caps(LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, "claude-haiku-4-5-20251001", &caps);
   TEST_ASSERT_EQUAL_INT(2, caps.mode_count);
   TEST_ASSERT_TRUE(caps.modes[1].budget);
   TEST_ASSERT_EQUAL_INT(4, caps.modes[1].effort_count); /* low..xhigh */

   llm_thinking_resolved_t r = resolve(CLOUD_PROVIDER_CLAUDE, "claude-haiku-4-5", "enabled", "max");
   assert_resolved(r, LLM_THINK_ENABLED, "xhigh", true); /* nearest budget level */
   TEST_ASSERT_TRUE(r.budget);
}

/* Haiku 5.5 is adaptive-only plus an off switch: no budget mode, unlike 4.5. */
static void test_haiku_5_5_is_adaptive_with_an_off_switch(void) {
   llm_thinking_caps_t caps;
   llm_thinking_caps(LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, "claude-haiku-5-5", &caps);
   TEST_ASSERT_EQUAL_INT(2, caps.mode_count);
   TEST_ASSERT_EQUAL_INT(LLM_THINK_DISABLED, caps.modes[0].mode);
   TEST_ASSERT_EQUAL_INT(LLM_THINK_ADAPTIVE, caps.modes[1].mode);
   TEST_ASSERT_FALSE(caps.modes[1].budget);
   TEST_ASSERT_EQUAL_INT(5, caps.modes[1].effort_count); /* low..max */

   assert_resolved(resolve(CLOUD_PROVIDER_CLAUDE, "claude-haiku-5-5", "disabled", "high"),
                   LLM_THINK_DISABLED, "", false);
   /* A budget pick (what 4.5 took) becomes adaptive at the same level. */
   llm_thinking_resolved_t r = resolve(CLOUD_PROVIDER_CLAUDE, "claude-haiku-5-5", "enabled",
                                       "xhigh");
   assert_resolved(r, LLM_THINK_ADAPTIVE, "xhigh", false);
   TEST_ASSERT_FALSE(r.budget);
   assert_resolved(resolve(CLOUD_PROVIDER_OPENROUTER, "anthropic/claude-haiku-5.5", "disabled",
                           "low"),
                   LLM_THINK_DISABLED, "", false);
}

static llm_thinking_resolved_t resolve_utility(const char *model, const char *effort) {
   s_suppressed = true;
   s_utility_effort = effort;
   llm_thinking_resolved_t r;
   llm_thinking_resolve_current(LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, model, &r);
   return r;
}

/* A tools-off call (extraction) gets the cheapest setting unless it asked for
 * reasoning: then the model's first reasoning mode at that effort. */
static void test_utility_call_with_an_effort_reasons(void) {
   llm_thinking_resolved_t r = resolve_utility("claude-haiku-5-5", "");
   assert_resolved(r, LLM_THINK_DISABLED, "", false); /* unchanged: cheapest */

   r = resolve_utility("claude-haiku-5-5", "medium");
   TEST_ASSERT_EQUAL_STRING("adaptive", llm_think_mode_name(r.mode));
   TEST_ASSERT_EQUAL_STRING("medium", r.effort);
   /* A model that also lists a budget mode takes adaptive, its first. */
   r = resolve_utility("claude-sonnet-4-6", "medium");
   TEST_ASSERT_EQUAL_STRING("adaptive", llm_think_mode_name(r.mode));
   /* A budget-only model takes a budget at that level. */
   r = resolve_utility("claude-haiku-4-5", "medium");
   TEST_ASSERT_EQUAL_STRING("enabled", llm_think_mode_name(r.mode));
   TEST_ASSERT_TRUE(r.budget);
   TEST_ASSERT_EQUAL_STRING("medium", r.effort);
   /* An adaptive-only model honors the effort instead of its lowest. */
   r = resolve_utility("claude-opus-5-5", "high");
   TEST_ASSERT_EQUAL_STRING("adaptive", llm_think_mode_name(r.mode));
   TEST_ASSERT_EQUAL_STRING("high", r.effort);
   /* Not a tools-off call: the effort is ignored, the session's mode applies. */
   s_suppressed = false;
   llm_thinking_resolve_current(LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, "claude-haiku-5-5", &r);
   TEST_ASSERT_EQUAL_STRING("disabled", llm_think_mode_name(r.mode));
}

static void test_mid_system_models(void) {
   TEST_ASSERT_TRUE(llm_model_mid_system("claude-haiku-5-5"));
   TEST_ASSERT_TRUE(llm_model_mid_system("claude-sonnet-5-5"));
   TEST_ASSERT_TRUE(llm_model_mid_system("claude-opus-5-5"));
   TEST_ASSERT_FALSE(llm_model_mid_system("claude-sonnet-5"));
   TEST_ASSERT_FALSE(llm_model_mid_system("claude-haiku-4-5"));
}

static void test_claude_with_both_shapes_honours_the_pick(void) {
   assert_resolved(resolve(CLOUD_PROVIDER_CLAUDE, "claude-sonnet-4-6", "adaptive", "medium"),
                   LLM_THINK_ADAPTIVE, "medium", false);
   llm_thinking_resolved_t r = resolve(CLOUD_PROVIDER_CLAUDE, "claude-sonnet-4-6", "enabled",
                                       "high");
   assert_resolved(r, LLM_THINK_ENABLED, "high", false);
   TEST_ASSERT_TRUE(r.budget);
   /* 4.6 adaptive has no xhigh: the nearest level is taken. */
   assert_resolved(resolve(CLOUD_PROVIDER_CLAUDE, "claude-opus-4-6", "adaptive", "xhigh"),
                   LLM_THINK_ADAPTIVE, "high", true);
}

static void test_openai_rows(void) {
   /* gpt-5.1+: "disabled" is effort none; there's no separate "none" effort. */
   assert_resolved(resolve(CLOUD_PROVIDER_OPENAI, "gpt-5.6-luna", "disabled", "high"),
                   LLM_THINK_DISABLED, "", false);
   assert_resolved(resolve(CLOUD_PROVIDER_OPENAI, "gpt-5.6-luna", "enabled", "max"),
                   LLM_THINK_ENABLED, "xhigh", true);
   assert_resolved(resolve(CLOUD_PROVIDER_OPENAI, "gpt-5.1", "enabled", "xhigh"), LLM_THINK_ENABLED,
                   "high", true);
   /* gpt-5 base can't turn reasoning off: minimal is its floor. */
   assert_resolved(resolve(CLOUD_PROVIDER_OPENAI, "gpt-5-mini", "disabled", ""), LLM_THINK_ENABLED,
                   "minimal", true);
   assert_resolved(resolve(CLOUD_PROVIDER_OPENAI, "o3-mini", "enabled", "none"), LLM_THINK_ENABLED,
                   "low", true);
   /* A family row doesn't catch a newer or specialty variant: those get
    * nothing until listed, never a level they reject. */
   TEST_ASSERT_FALSE(resolve(CLOUD_PROVIDER_OPENAI, "gpt-5.7", "disabled", "").controllable);
   TEST_ASSERT_FALSE(
       resolve(CLOUD_PROVIDER_OPENAI, "gpt-5-chat-latest", "enabled", "high").controllable);
   TEST_ASSERT_FALSE(resolve(CLOUD_PROVIDER_OPENAI, "o1-mini", "enabled", "high").controllable);
   assert_resolved(resolve(CLOUD_PROVIDER_OPENAI, "gpt-5-2025-08-07", "disabled", ""),
                   LLM_THINK_ENABLED, "minimal", true);
   TEST_ASSERT_FALSE(
       resolve(CLOUD_PROVIDER_GEMINI, "gemini-2.5-flash-image", "enabled", "high").controllable);
   /* No reasoning control: nothing to send. */
   llm_thinking_resolved_t r = resolve(CLOUD_PROVIDER_OPENAI, "gpt-4o", "enabled", "high");
   TEST_ASSERT_FALSE(r.controllable);
}

static void test_gemini_rows(void) {
   assert_resolved(resolve(CLOUD_PROVIDER_GEMINI, "gemini-3-flash-preview", "disabled", ""),
                   LLM_THINK_ENABLED, "low", true);
   TEST_ASSERT_FALSE(
       resolve(CLOUD_PROVIDER_GEMINI, "gemini-1.5-pro", "enabled", "high").controllable);
}

static void test_openrouter_follows_the_vendor(void) {
   assert_resolved(resolve(CLOUD_PROVIDER_OPENROUTER, "anthropic/claude-opus-5.5", "disabled", ""),
                   LLM_THINK_ADAPTIVE, "low", true);
   assert_resolved(resolve(CLOUD_PROVIDER_OPENROUTER, "openai/gpt-5.6-sol", "enabled", "high"),
                   LLM_THINK_ENABLED, "high", false);
   assert_resolved(resolve(CLOUD_PROVIDER_OPENROUTER, "qwen/qwen3-max", "disabled", "high"),
                   LLM_THINK_DISABLED, "", false);
}

static void test_unlisted_models_get_conservative_defaults(void) {
   llm_thinking_caps_t caps;
   llm_thinking_caps(LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, "claude-opus-9", &caps);
   TEST_ASSERT_EQUAL_INT(LLM_CAPS_PROVIDER_DEFAULT, caps.source);
   /* No "disabled" unless known. */
   assert_resolved(resolve(CLOUD_PROVIDER_CLAUDE, "claude-opus-9", "disabled", ""),
                   LLM_THINK_ADAPTIVE, "low", true);
   /* An unlisted OpenAI-compatible model gets no reasoning parameters: an
    * OpenAI-compatible endpoint serves anything, and most reject them. */
   TEST_ASSERT_FALSE(
       resolve(CLOUD_PROVIDER_OPENAI, "gpt-3.5-turbo", "enabled", "high").controllable);
   TEST_ASSERT_FALSE(
       resolve(CLOUD_PROVIDER_OPENAI, "ft:gpt-4o-mini:acme::1", "disabled", "").controllable);
}

static void test_older_claude_models_take_a_budget(void) {
   llm_thinking_resolved_t r = resolve(CLOUD_PROVIDER_CLAUDE, "claude-sonnet-4-20250514", "enabled",
                                       "high");
   assert_resolved(r, LLM_THINK_ENABLED, "high", false);
   TEST_ASSERT_TRUE(r.budget);
   assert_resolved(resolve(CLOUD_PROVIDER_CLAUDE, "claude-opus-4-1-20250805", "disabled", ""),
                   LLM_THINK_DISABLED, "", false);
   assert_resolved(resolve(CLOUD_PROVIDER_CLAUDE, "claude-3-7-sonnet-latest", "enabled", "low"),
                   LLM_THINK_ENABLED, "low", false);
   /* The newer rows still win over their shorter prefixes. */
   assert_resolved(resolve(CLOUD_PROVIDER_CLAUDE, "claude-sonnet-4-6", "adaptive", "high"),
                   LLM_THINK_ADAPTIVE, "high", false);
   TEST_ASSERT_FALSE(
       resolve(CLOUD_PROVIDER_CLAUDE, "claude-3-5-haiku-latest", "enabled", "high").controllable);
}

static void test_a_named_mode_the_model_lacks_is_a_clamp(void) {
   /* "adaptive" is Claude's; on OpenAI it becomes reasoning at the effort, and
    * the user is told.  The older clients' "enabled" means "on": no clamp. */
   assert_resolved(resolve(CLOUD_PROVIDER_OPENAI, "gpt-5.6-luna", "adaptive", "high"),
                   LLM_THINK_ENABLED, "high", true);
   assert_resolved(resolve(CLOUD_PROVIDER_CLAUDE, "claude-opus-5-5", "enabled", "high"),
                   LLM_THINK_ADAPTIVE, "high", false);
}

static void test_local_providers(void) {
   llm_thinking_caps_t caps;
   llm_thinking_resolved_t r;
   llm_thinking_caps(LLM_LOCAL, CLOUD_PROVIDER_NONE, "qwen3", &caps);
   TEST_ASSERT_EQUAL_INT(LLM_CAPS_LOCAL, caps.source);
   llm_thinking_resolve(&caps, "enabled", "high", false, &r);
   assert_resolved(r, LLM_THINK_ENABLED, "high", false);
   TEST_ASSERT_TRUE(r.budget);

   s_local = LOCAL_PROVIDER_OLLAMA;
   llm_thinking_caps(LLM_LOCAL, CLOUD_PROVIDER_NONE, "qwen3", &caps);
   llm_thinking_resolve(&caps, "enabled", "high", false, &r);
   TEST_ASSERT_EQUAL_STRING("enabled", llm_think_mode_name(r.mode));
   TEST_ASSERT_EQUAL_STRING("", r.effort); /* think on/off only */
}

static void test_utility_calls_are_cheapest_and_never_clamped(void) {
   llm_thinking_caps_t caps;
   llm_thinking_resolved_t r;
   llm_thinking_caps(LLM_CLOUD, CLOUD_PROVIDER_CLAUDE, "claude-opus-5-5", &caps);
   llm_thinking_resolve(&caps, "enabled", "max", true, &r);
   assert_resolved(r, LLM_THINK_ADAPTIVE, "low", false);
   llm_thinking_caps(LLM_CLOUD, CLOUD_PROVIDER_OPENAI, "gpt-5.6-luna", &caps);
   llm_thinking_resolve(&caps, "enabled", "max", true, &r);
   assert_resolved(r, LLM_THINK_DISABLED, "", false);
}

static json_object *caps_json(cloud_provider_t provider, const char *model) {
   llm_thinking_caps_t caps;
   llm_thinking_caps(LLM_CLOUD, provider, model, &caps);
   json_object *j = llm_thinking_caps_to_json(&caps);
   TEST_ASSERT_NOT_NULL(j);
   return j;
}

static const char *str_at(json_object *obj, const char *key) {
   json_object *v = NULL;
   return json_object_object_get_ex(obj, key, &v) ? json_object_get_string(v) : NULL;
}

/* The shape the WebUI and Aurora render. */
static void test_wire_shape(void) {
   json_object *j = caps_json(CLOUD_PROVIDER_CLAUDE, "claude-opus-5-5");
   TEST_ASSERT_EQUAL_STRING("row", str_at(j, "source"));
   json_object *modes = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(j, "modes", &modes));
   TEST_ASSERT_EQUAL_INT(1, json_object_array_length(modes));
   json_object *m0 = json_object_array_get_idx(modes, 0);
   TEST_ASSERT_EQUAL_STRING("adaptive", str_at(m0, "mode"));
   json_object *efforts = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(m0, "efforts", &efforts));
   TEST_ASSERT_EQUAL_INT(5, json_object_array_length(efforts));
   TEST_ASSERT_FALSE(json_object_object_get_ex(m0, "budget_tokens", NULL));
   /* The configured default ("disabled") resolved for a model that can't: */
   json_object *def = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(j, "default", &def));
   TEST_ASSERT_EQUAL_STRING("adaptive", str_at(def, "mode"));
   TEST_ASSERT_EQUAL_STRING("low", str_at(def, "effort"));
   json_object_put(j);

   /* A budget mode names its token sizes. */
   g_config.llm.thinking.budget_low = 1024;
   g_config.llm.thinking.budget_high = 16384;
   j = caps_json(CLOUD_PROVIDER_CLAUDE, "claude-haiku-4-5");
   TEST_ASSERT_TRUE(json_object_object_get_ex(j, "modes", &modes));
   json_object *m1 = json_object_array_get_idx(modes, 1);
   TEST_ASSERT_EQUAL_STRING("enabled", str_at(m1, "mode"));
   TEST_ASSERT_TRUE(json_object_get_boolean(json_object_object_get(m1, "budget")));
   json_object *tokens = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(m1, "budget_tokens", &tokens));
   TEST_ASSERT_EQUAL_INT(1024, json_object_get_int(json_object_object_get(tokens, "low")));
   TEST_ASSERT_EQUAL_INT(16384, json_object_get_int(json_object_object_get(tokens, "high")));
   TEST_ASSERT_TRUE(json_object_object_get_ex(j, "default", &def));
   TEST_ASSERT_EQUAL_STRING("disabled", str_at(def, "mode"));
   json_object_put(j);

   /* No reasoning control: an explicit empty list, not a missing entry. */
   j = caps_json(CLOUD_PROVIDER_OPENAI, "gpt-3.5-turbo");
   TEST_ASSERT_EQUAL_STRING("provider_default", str_at(j, "source"));
   TEST_ASSERT_TRUE(json_object_object_get_ex(j, "modes", &modes));
   TEST_ASSERT_EQUAL_INT(0, json_object_array_length(modes));
   json_object_put(j);
}

int main(void) {
   char err[256];
   FILE *f = fopen(MODELS_TOML_PATH, "r");
   TEST_ASSERT_NOT_NULL_MESSAGE(f, MODELS_TOML_PATH);
   s_root = toml_parse_file(f, err, sizeof(err));
   fclose(f);
   if (!s_root) {
      fprintf(stderr, "models.toml parse error: %s\n", err);
      return 1;
   }
   llm_capabilities_load_registry(s_root);
   llm_capabilities_load_mid_system(s_root);

   UNITY_BEGIN();
   RUN_TEST(test_adaptive_only_claude_has_no_off_switch);
   RUN_TEST(test_claude_that_accepts_disabled);
   RUN_TEST(test_budget_claude_uses_budget_levels);
   RUN_TEST(test_haiku_5_5_is_adaptive_with_an_off_switch);
   RUN_TEST(test_mid_system_models);
   RUN_TEST(test_utility_call_with_an_effort_reasons);
   RUN_TEST(test_claude_with_both_shapes_honours_the_pick);
   RUN_TEST(test_openai_rows);
   RUN_TEST(test_gemini_rows);
   RUN_TEST(test_openrouter_follows_the_vendor);
   RUN_TEST(test_unlisted_models_get_conservative_defaults);
   RUN_TEST(test_older_claude_models_take_a_budget);
   RUN_TEST(test_a_named_mode_the_model_lacks_is_a_clamp);
   RUN_TEST(test_local_providers);
   RUN_TEST(test_utility_calls_are_cheapest_and_never_clamped);
   RUN_TEST(test_wire_shape);
   const int rc = UNITY_END();
   llm_capabilities_free_registry();
   toml_free(s_root);
   return rc;
}
