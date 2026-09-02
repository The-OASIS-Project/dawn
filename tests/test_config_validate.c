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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Unit tests for config_validate (range, enum, dependency checks).
 */

#include <stdbool.h>
#include <string.h>

#include "config/config_validate.h"
#include "config/dawn_config.h"
#include "unity.h"

/* Stub — config_validate.c calls into llm_silent_observe.c for the
 * silent-observe provider enum.  Test exercises range / enum / dep
 * validation only; treat any non-NULL provider as valid so the
 * silent-observe branch in config_validate doesn't drag the LLM
 * subsystem into the test link. */
bool llm_silent_observe_provider_is_valid(const char *provider) {
   return provider != NULL && provider[0] != '\0';
}

#define MAX_ERRORS 32

static dawn_config_t s_config;
static config_error_t s_errors[MAX_ERRORS];

static void set_valid_defaults(void) {
   memset(&s_config, 0, sizeof(s_config));
   s_config.vad.speech_threshold = 0.5f;
   s_config.vad.speech_threshold_tts = 0.8f;
   s_config.vad.silence_threshold = 0.3f;
   s_config.vad.end_of_speech_duration = 1.0f;
   s_config.vad.max_recording_duration = 30.0f;
   s_config.tts.length_scale = 1.0f;
   s_config.llm.compact_soft_threshold = 0.60f;
   s_config.llm.compact_hard_threshold = 0.85f;
   s_config.llm.max_tokens = 4096;
   s_config.memory.temporal_weight = 0.2f;
   s_config.memory.category_threshold = 0.25f;
   /* paraphrase_dedup_threshold validator added in commit 6aa2e32 (May 2026) —
    * test helper updated to provide a valid default in the same range [0.5,1.0]. */
   s_config.memory.paraphrase_dedup_threshold = 0.92f;
   s_config.memory.extraction_timeout_ms = 30000;
   s_config.memory.recompute_batch_size = 50;
   s_config.memory.recompute_batch_sleep_ms = 100;
   s_config.memory.recovery_idle_threshold_seconds = 3600;
   s_config.memory.recovery_max_attempts = 2;
   s_config.memory.recovery_recurring_interval_seconds = 86400;
   /* focus_injection.* added in Phase 1b — needs valid defaults so
    * config_validate doesn't short-circuit on this field before
    * reaching the field the individual test is exercising. */
   s_config.memory.focus_injection.focus_budget_bytes = 10240;
   s_config.memory.focus_injection.top_k = 8;
   s_config.memory.focus_injection.summary_max_scan = 4096;
   s_config.memory.focus_injection.min_score = 0.4f;
   s_config.memory.focus_injection.weight_semantic = 1.0f;
   s_config.memory.focus_injection.weight_recency = 0.3f;
   s_config.memory.focus_injection.weight_importance = 0.2f;
   s_config.memory.focus_injection.weight_source = 1.0f;
   s_config.memory.focus_injection.source_weights.memory_fact = 1.0f;
   s_config.memory.focus_injection.source_weights.memory_entity = 0.9f;
   s_config.memory.focus_injection.source_weights.memory_relation = 0.85f;
   s_config.memory.focus_injection.source_weights.memory_summary = 0.7f;
   s_config.memory.focus_injection.source_weights.document_chunk = 0.7f;
   s_config.memory.focus_injection.source_weights.calendar_event = 0.6f;
   s_config.memory.focus_injection.source_weights.recent_email = 0.5f;
   s_config.memory.focus_injection.source_weights.dawn_background = 0.8f;
   s_config.memory.focus_injection.dedup.recent_window_turns = 8;
   s_config.memory.focus_injection.dedup.score_uplift_factor = 1.5f;
   /* graph_retrieval.* added in Phase 1A — needs valid defaults so
    * config_validate doesn't short-circuit on these fields before
    * reaching the field the individual test is exercising. */
   s_config.memory.graph_retrieval.entity_grounding_bonus = 0.40f;
   s_config.memory.graph_retrieval.max_facts_per_query = 30;
   s_config.memory.graph_retrieval.entity_bonus = 0.0f;
   /* dominant_token_heuristic.* added in May 2026 reranker workstream —
    * lower bound is 0.01 (not 0.0) on both fields per the validator's
    * comment about silent-no-op-at-zero. */
   s_config.memory.focus_injection.dominant_token_heuristic.threshold = 0.6f;
   s_config.memory.focus_injection.dominant_token_heuristic.base_penalty = 0.5f;
   /* memory.recall.* added with the unified recall tool — needs valid defaults
    * so the baseline config validates clean (mirrors focus_injection above). */
   s_config.memory.recall.top_k = 40;
   s_config.memory.recall.budget_bytes = 24576;
   s_config.memory.recall.min_score = 0.25f;
   s_config.memory.recall.per_source_max = 16;
   /* attention.* (SAGE) — judge_threshold has a [1.0,5.0] validator, so a
    * zeroed baseline would fail; provide the shipped defaults. */
   s_config.attention.max_alerts_per_hour = 4;
   s_config.attention.judge_threshold = 3.5;
   s_config.mqtt.port = 1883;
   s_config.network.workers = 4;
   s_config.network.session_timeout_sec = 3600;
   s_config.network.llm_timeout_ms = 30000;
   s_config.network.summarization_timeout_ms = 60000;
   strncpy(s_config.commands.processing_mode, "direct_first",
           sizeof(s_config.commands.processing_mode) - 1);
   strncpy(s_config.llm.type, "cloud", sizeof(s_config.llm.type) - 1);
   strncpy(s_config.llm.cloud.provider, "openai", sizeof(s_config.llm.cloud.provider) - 1);
   strncpy(s_config.search.summarizer.backend, "tfidf",
           sizeof(s_config.search.summarizer.backend) - 1);
}

void setUp(void) {
   set_valid_defaults();
   memset(s_errors, 0, sizeof(s_errors));
}

void tearDown(void) {
}

/* ── Baseline ───────────────────────────────────────────────────────────── */

static void test_valid_config_no_errors(void) {
   int n = config_validate(&s_config, NULL, s_errors, MAX_ERRORS);
   TEST_ASSERT_EQUAL_INT(0, n);
}

/* ── Float Range Checks ─────────────────────────────────────────────────── */

static void test_vad_speech_threshold_out_of_range(void) {
   s_config.vad.speech_threshold = 1.5f;
   int n = config_validate(&s_config, NULL, s_errors, MAX_ERRORS);
   TEST_ASSERT_GREATER_OR_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_STRING("vad.speech_threshold", s_errors[0].field);
}

static void test_tts_length_scale_out_of_range(void) {
   s_config.tts.length_scale = 3.0f;
   int n = config_validate(&s_config, NULL, s_errors, MAX_ERRORS);
   TEST_ASSERT_GREATER_OR_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_STRING("tts.length_scale", s_errors[0].field);
}

static void test_temporal_weight_negative(void) {
   s_config.memory.temporal_weight = -0.1f;
   int n = config_validate(&s_config, NULL, s_errors, MAX_ERRORS);
   TEST_ASSERT_GREATER_OR_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_STRING("memory.temporal_weight", s_errors[0].field);
}

/* ── Integer Range Checks ────────────────────────────────────────────────── */

static void test_mqtt_port_zero(void) {
   s_config.mqtt.port = 0;
   int n = config_validate(&s_config, NULL, s_errors, MAX_ERRORS);
   TEST_ASSERT_GREATER_OR_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_STRING("mqtt.port", s_errors[0].field);
}

static void test_mqtt_port_too_high(void) {
   s_config.mqtt.port = 70000;
   int n = config_validate(&s_config, NULL, s_errors, MAX_ERRORS);
   TEST_ASSERT_GREATER_OR_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_STRING("mqtt.port", s_errors[0].field);
}

static void test_network_workers_too_many(void) {
   s_config.network.workers = 10;
   int n = config_validate(&s_config, NULL, s_errors, MAX_ERRORS);
   TEST_ASSERT_GREATER_OR_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_STRING("network.workers", s_errors[0].field);
}

/* ── Positive Value Checks ───────────────────────────────────────────────── */

static void test_max_tokens_zero(void) {
   s_config.llm.max_tokens = 0;
   int n = config_validate(&s_config, NULL, s_errors, MAX_ERRORS);
   TEST_ASSERT_GREATER_OR_EQUAL_INT(1, n);
   bool found = false;
   for (int i = 0; i < n; i++) {
      if (strcmp(s_errors[i].field, "llm.max_tokens") == 0) {
         found = true;
         break;
      }
   }
   TEST_ASSERT_TRUE_MESSAGE(found, "expected error for llm.max_tokens");
}

/* ── Enum Checks ─────────────────────────────────────────────────────────── */

static void test_invalid_processing_mode(void) {
   strncpy(s_config.commands.processing_mode, "bogus",
           sizeof(s_config.commands.processing_mode) - 1);
   int n = config_validate(&s_config, NULL, s_errors, MAX_ERRORS);
   TEST_ASSERT_GREATER_OR_EQUAL_INT(1, n);
   bool found = false;
   for (int i = 0; i < n; i++) {
      if (strcmp(s_errors[i].field, "commands.processing_mode") == 0) {
         found = true;
         break;
      }
   }
   TEST_ASSERT_TRUE_MESSAGE(found, "expected error for commands.processing_mode");
}

static void test_invalid_llm_type(void) {
   strncpy(s_config.llm.type, "quantum", sizeof(s_config.llm.type) - 1);
   int n = config_validate(&s_config, NULL, s_errors, MAX_ERRORS);
   TEST_ASSERT_GREATER_OR_EQUAL_INT(1, n);
   bool found = false;
   for (int i = 0; i < n; i++) {
      if (strcmp(s_errors[i].field, "llm.type") == 0) {
         found = true;
         break;
      }
   }
   TEST_ASSERT_TRUE_MESSAGE(found, "expected error for llm.type");
}

static void test_invalid_cloud_provider(void) {
   strncpy(s_config.llm.cloud.provider, "deepseek", sizeof(s_config.llm.cloud.provider) - 1);
   int n = config_validate(&s_config, NULL, s_errors, MAX_ERRORS);
   TEST_ASSERT_GREATER_OR_EQUAL_INT(1, n);
   bool found = false;
   for (int i = 0; i < n; i++) {
      if (strcmp(s_errors[i].field, "llm.cloud.provider") == 0) {
         found = true;
         break;
      }
   }
   TEST_ASSERT_TRUE_MESSAGE(found, "expected error for llm.cloud.provider");
}

/* "openrouter" is a first-class cloud provider value (2a-0) and must validate. */
static void test_openrouter_is_valid_provider(void) {
   strncpy(s_config.llm.cloud.provider, "openrouter", sizeof(s_config.llm.cloud.provider) - 1);
   int n = config_validate(&s_config, NULL, s_errors, MAX_ERRORS);
   for (int i = 0; i < n; i++) {
      TEST_ASSERT_NOT_EQUAL_MESSAGE(
          0, strcmp(s_errors[i].field, "llm.cloud.provider"),
          "provider=\"openrouter\" must not raise a provider validation error");
   }
}

/* ── OpenRouter gateway ──────────────────────────────────────────────────── */

/* use_openrouter is a separate bool, NOT a provider-string value — turning it on
 * must not break provider validation (provider stays openai/claude/gemini). */
static void test_openrouter_gateway_provider_still_validated(void) {
   s_config.llm.cloud.use_openrouter = true;
   /* provider remains "openai" from defaults — valid */
   int n = config_validate(&s_config, NULL, s_errors, MAX_ERRORS);
   TEST_ASSERT_EQUAL_INT(0, n);
}

/* Gateway on but no openrouter_api_key is warn-only (runtime falls back to local),
 * so it must NOT add a hard validation error. */
static void test_openrouter_gateway_missing_key_is_not_fatal(void) {
   secrets_config_t secrets;
   memset(&secrets, 0, sizeof(secrets));
   s_config.llm.cloud.use_openrouter = true;
   /* secrets.openrouter_api_key left empty */
   int n = config_validate(&s_config, &secrets, s_errors, MAX_ERRORS);
   TEST_ASSERT_EQUAL_INT(0, n);
}

static void test_invalid_summarizer_backend(void) {
   strncpy(s_config.search.summarizer.backend, "gpt",
           sizeof(s_config.search.summarizer.backend) - 1);
   int n = config_validate(&s_config, NULL, s_errors, MAX_ERRORS);
   TEST_ASSERT_GREATER_OR_EQUAL_INT(1, n);
   bool found = false;
   for (int i = 0; i < n; i++) {
      if (strcmp(s_errors[i].field, "search.summarizer.backend") == 0) {
         found = true;
         break;
      }
   }
   TEST_ASSERT_TRUE_MESSAGE(found, "expected error for search.summarizer.backend");
}

/* ── Dependency Checks ───────────────────────────────────────────────────── */

static void test_compact_threshold_ordering(void) {
   s_config.llm.compact_soft_threshold = 0.90f;
   s_config.llm.compact_hard_threshold = 0.85f;
   int n = config_validate(&s_config, NULL, s_errors, MAX_ERRORS);
   TEST_ASSERT_GREATER_OR_EQUAL_INT(1, n);
   bool found = false;
   for (int i = 0; i < n; i++) {
      if (strcmp(s_errors[i].field, "llm.compact_soft_threshold") == 0) {
         found = true;
         break;
      }
   }
   TEST_ASSERT_TRUE_MESSAGE(found, "expected error for compact_soft >= compact_hard");
}

/* ── URL Validation ──────────────────────────────────────────────────────── */

static void test_local_endpoint_no_http(void) {
   strncpy(s_config.llm.local.endpoint, "ftp://host:8080", sizeof(s_config.llm.local.endpoint) - 1);
   int n = config_validate(&s_config, NULL, s_errors, MAX_ERRORS);
   TEST_ASSERT_GREATER_OR_EQUAL_INT(1, n);
   bool found = false;
   for (int i = 0; i < n; i++) {
      if (strcmp(s_errors[i].field, "llm.local.endpoint") == 0) {
         found = true;
         break;
      }
   }
   TEST_ASSERT_TRUE_MESSAGE(found, "expected error for llm.local.endpoint");
}

static void test_local_endpoint_ssrf(void) {
   strncpy(s_config.llm.local.endpoint, "http://169.254.169.254/latest",
           sizeof(s_config.llm.local.endpoint) - 1);
   int n = config_validate(&s_config, NULL, s_errors, MAX_ERRORS);
   TEST_ASSERT_GREATER_OR_EQUAL_INT(1, n);
   bool found = false;
   for (int i = 0; i < n; i++) {
      if (strcmp(s_errors[i].field, "llm.local.endpoint") == 0) {
         found = true;
         break;
      }
   }
   TEST_ASSERT_TRUE_MESSAGE(found, "expected SSRF error for llm.local.endpoint");
}

/* ── NULL Safety ─────────────────────────────────────────────────────────── */

static void test_null_config_returns_zero(void) {
   int n = config_validate(NULL, NULL, s_errors, MAX_ERRORS);
   TEST_ASSERT_EQUAL_INT(0, n);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void) {
   UNITY_BEGIN();

   RUN_TEST(test_valid_config_no_errors);

   RUN_TEST(test_vad_speech_threshold_out_of_range);
   RUN_TEST(test_tts_length_scale_out_of_range);
   RUN_TEST(test_temporal_weight_negative);

   RUN_TEST(test_mqtt_port_zero);
   RUN_TEST(test_mqtt_port_too_high);
   RUN_TEST(test_network_workers_too_many);

   RUN_TEST(test_max_tokens_zero);

   RUN_TEST(test_invalid_processing_mode);
   RUN_TEST(test_invalid_llm_type);
   RUN_TEST(test_invalid_cloud_provider);
   RUN_TEST(test_openrouter_is_valid_provider);
   RUN_TEST(test_openrouter_gateway_provider_still_validated);
   RUN_TEST(test_openrouter_gateway_missing_key_is_not_fatal);
   RUN_TEST(test_invalid_summarizer_backend);

   RUN_TEST(test_compact_threshold_ordering);

   RUN_TEST(test_local_endpoint_no_http);
   RUN_TEST(test_local_endpoint_ssrf);

   RUN_TEST(test_null_config_returns_zero);

   return UNITY_END();
}
