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
 * DAWN Configuration Environment - Environment variable overrides and dump
 */

#include "config/config_env.h"

#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config/config_parser.h"
#include "logging.h"
#include "tools/tool_registry.h"

/* =============================================================================
 * Helper Macros
 * ============================================================================= */
#define SAFE_COPY(dst, src)                   \
   do {                                       \
      strncpy((dst), (src), sizeof(dst) - 1); \
      (dst)[sizeof(dst) - 1] = '\0';          \
   } while (0)

#define ENV_STRING(env_name, dest)                           \
   do {                                                      \
      const char *val = getenv(env_name);                    \
      if (val) {                                             \
         SAFE_COPY(dest, val);                               \
         OLOG_INFO("Config override: %s=%s", env_name, val); \
      }                                                      \
   } while (0)

/* Like ENV_STRING but redacts value in logs - use for API keys and secrets */
#define ENV_SECRET(env_name, dest)                              \
   do {                                                         \
      const char *val = getenv(env_name);                       \
      if (val) {                                                \
         SAFE_COPY(dest, val);                                  \
         OLOG_INFO("Config override: %s=[REDACTED]", env_name); \
      }                                                         \
   } while (0)

#define ENV_INT(env_name, dest)                                                               \
   do {                                                                                       \
      const char *val = getenv(env_name);                                                     \
      if (val) {                                                                              \
         char *end_;                                                                          \
         errno = 0;                                                                           \
         long v_ = strtol(val, &end_, 10);                                                    \
         if (*end_ != '\0' || errno != 0 || v_ < INT_MIN || v_ > INT_MAX) {                   \
            OLOG_WARNING("Config override: %s=%s (invalid integer, ignored)", env_name, val); \
         } else {                                                                             \
            dest = (int)v_;                                                                   \
            OLOG_INFO("Config override: %s=%d", env_name, dest);                              \
         }                                                                                    \
      }                                                                                       \
   } while (0)

#define ENV_FLOAT(env_name, dest)                               \
   do {                                                         \
      const char *val = getenv(env_name);                       \
      if (val) {                                                \
         dest = (float)atof(val);                               \
         OLOG_INFO("Config override: %s=%.2f", env_name, dest); \
      }                                                         \
   } while (0)

#define ENV_BOOL(env_name, dest)                                                 \
   do {                                                                          \
      const char *val = getenv(env_name);                                        \
      if (val) {                                                                 \
         dest = (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0 ||        \
                 strcasecmp(val, "yes") == 0);                                   \
         OLOG_INFO("Config override: %s=%s", env_name, dest ? "true" : "false"); \
      }                                                                          \
   } while (0)

#define ENV_SIZE_T(env_name, dest)                             \
   do {                                                        \
      const char *val = getenv(env_name);                      \
      if (val) {                                               \
         dest = (size_t)atol(val);                             \
         OLOG_INFO("Config override: %s=%zu", env_name, dest); \
      }                                                        \
   } while (0)

/* =============================================================================
 * Environment Variable Application
 * ============================================================================= */

void config_apply_env(dawn_config_t *config, secrets_config_t *secrets) {
   if (!config || !secrets)
      return;

   /* Standard API keys (highest priority) - use ENV_SECRET to redact in logs */
   ENV_SECRET("OPENAI_API_KEY", secrets->openai_api_key);
   ENV_SECRET("ANTHROPIC_API_KEY", secrets->claude_api_key);
   ENV_SECRET("GEMINI_API_KEY", secrets->gemini_api_key);
   ENV_SECRET("OPENROUTER_API_KEY", secrets->openrouter_api_key);
   ENV_SECRET("TAVILY_API_KEY", secrets->tavily_api_key);
   ENV_SECRET("TELEGRAM_BOT_TOKEN", secrets->telegram_bot_token);
   ENV_SECRET("DISCORD_BOT_TOKEN", secrets->discord_bot_token);
   ENV_SECRET("SLACK_APP_TOKEN", secrets->slack_app_token);
   ENV_SECRET("SLACK_BOT_TOKEN", secrets->slack_bot_token);

   /* DAWN_ prefixed environment variables */

   /* [general] */
   ENV_STRING("DAWN_GENERAL_AI_NAME", config->general.ai_name);
   ENV_STRING("DAWN_GENERAL_LOG_FILE", config->general.log_file);

   /* [persona] */
   ENV_STRING("DAWN_PERSONA_DESCRIPTION", config->persona.description);

   /* [localization] */
   ENV_STRING("DAWN_LOCALIZATION_LOCATION", config->localization.location);
   ENV_STRING("DAWN_LOCALIZATION_TIMEZONE", config->localization.timezone);
   ENV_STRING("DAWN_LOCALIZATION_UNITS", config->localization.units);

   /* [audio] */
   ENV_STRING("DAWN_AUDIO_BACKEND", config->audio.backend);
   ENV_STRING("DAWN_AUDIO_CAPTURE_DEVICE", config->audio.capture_device);
   ENV_STRING("DAWN_AUDIO_PLAYBACK_DEVICE", config->audio.playback_device);

   /* [audio.bargein] */
   ENV_BOOL("DAWN_AUDIO_BARGEIN_ENABLED", config->audio.bargein.enabled);
   ENV_INT("DAWN_AUDIO_BARGEIN_COOLDOWN_MS", config->audio.bargein.cooldown_ms);
   ENV_INT("DAWN_AUDIO_BARGEIN_STARTUP_COOLDOWN_MS", config->audio.bargein.startup_cooldown_ms);

   /* [vad] */
   ENV_FLOAT("DAWN_VAD_SPEECH_THRESHOLD", config->vad.speech_threshold);
   ENV_FLOAT("DAWN_VAD_SPEECH_THRESHOLD_TTS", config->vad.speech_threshold_tts);
   ENV_FLOAT("DAWN_VAD_SILENCE_THRESHOLD", config->vad.silence_threshold);
   ENV_FLOAT("DAWN_VAD_END_OF_SPEECH_DURATION", config->vad.end_of_speech_duration);
   ENV_FLOAT("DAWN_VAD_MAX_RECORDING_DURATION", config->vad.max_recording_duration);
   ENV_INT("DAWN_VAD_PREROLL_MS", config->vad.preroll_ms);

   /* [vad.chunking] */
   ENV_BOOL("DAWN_VAD_CHUNKING_ENABLED", config->vad.chunking.enabled);
   ENV_FLOAT("DAWN_VAD_CHUNKING_PAUSE_DURATION", config->vad.chunking.pause_duration);
   ENV_FLOAT("DAWN_VAD_CHUNKING_MIN_DURATION", config->vad.chunking.min_duration);
   ENV_FLOAT("DAWN_VAD_CHUNKING_MAX_DURATION", config->vad.chunking.max_duration);

   /* [asr] */
   ENV_STRING("DAWN_ASR_MODEL", config->asr.model);
   ENV_STRING("DAWN_ASR_MODELS_PATH", config->asr.models_path);
   ENV_INT("DAWN_ASR_DEDUP_WINDOW_SEC", config->asr.dedup_window_sec);

   /* [tts] */
   ENV_STRING("DAWN_TTS_MODELS_PATH", config->tts.models_path);
   ENV_STRING("DAWN_TTS_VOICE_MODEL", config->tts.voice_model);
   ENV_FLOAT("DAWN_TTS_LENGTH_SCALE", config->tts.length_scale);

   /* [commands] */
   ENV_STRING("DAWN_COMMANDS_PROCESSING_MODE", config->commands.processing_mode);

   /* [llm] */
   ENV_STRING("DAWN_LLM_TYPE", config->llm.type);
   ENV_INT("DAWN_LLM_MAX_TOKENS", config->llm.max_tokens);
   ENV_BOOL("DAWN_LLM_RATE_LIMIT_ENABLED", config->llm.rate_limit_enabled);
   ENV_INT("DAWN_LLM_RATE_LIMIT_RPM", config->llm.rate_limit_rpm);

   /* [llm.cloud] */
   ENV_STRING("DAWN_LLM_CLOUD_PROVIDER", config->llm.cloud.provider);
   ENV_STRING("DAWN_LLM_CLOUD_ENDPOINT", config->llm.cloud.endpoint);
   ENV_BOOL("DAWN_LLM_CLOUD_VISION_ENABLED", config->llm.cloud.vision_enabled);
   ENV_BOOL("DAWN_LLM_CLOUD_USE_OPENROUTER", config->llm.cloud.use_openrouter);
   ENV_STRING("DAWN_LLM_CLOUD_OPENAI_USE_RESPONSES_API",
              config->llm.cloud.openai_use_responses_api);
   ENV_INT("DAWN_LLM_CLOUD_OPENAI_DEFAULT_MODEL_IDX", config->llm.cloud.openai_default_model_idx);
   ENV_INT("DAWN_LLM_CLOUD_CLAUDE_DEFAULT_MODEL_IDX", config->llm.cloud.claude_default_model_idx);
   ENV_INT("DAWN_LLM_CLOUD_GEMINI_DEFAULT_MODEL_IDX", config->llm.cloud.gemini_default_model_idx);
   ENV_INT("DAWN_LLM_CLOUD_OPENROUTER_DEFAULT_MODEL_IDX",
           config->llm.cloud.openrouter_default_model_idx);

   /* [llm.local] */
   ENV_STRING("DAWN_LLM_LOCAL_ENDPOINT", config->llm.local.endpoint);
   ENV_STRING("DAWN_LLM_LOCAL_MODEL", config->llm.local.model);
   ENV_BOOL("DAWN_LLM_LOCAL_VISION_ENABLED", config->llm.local.vision_enabled);

   /* [llm.tools] */
   ENV_STRING("DAWN_LLM_TOOLS_MODE", config->llm.tools.mode);

   /* [llm.silent_observe] */
   ENV_STRING("DAWN_LLM_SILENT_OBSERVE_PROVIDER", config->llm.silent_observe.provider);
   ENV_STRING("DAWN_LLM_SILENT_OBSERVE_MODEL", config->llm.silent_observe.model);

   /* [search] */
   ENV_STRING("DAWN_SEARCH_ENGINE", config->search.engine);
   ENV_STRING("DAWN_SEARCH_ENDPOINT", config->search.endpoint);

   /* [search.summarizer] */
   ENV_STRING("DAWN_SEARCH_SUMMARIZER_BACKEND", config->search.summarizer.backend);
   ENV_SIZE_T("DAWN_SEARCH_SUMMARIZER_THRESHOLD_BYTES", config->search.summarizer.threshold_bytes);
   ENV_SIZE_T("DAWN_SEARCH_SUMMARIZER_TARGET_WORDS", config->search.summarizer.target_words);
   ENV_FLOAT("DAWN_SEARCH_SUMMARIZER_TARGET_RATIO", config->search.summarizer.target_ratio);

   /* [url_fetcher] */
   ENV_STRING("DAWN_URL_FETCHER_FALLBACK", config->url_fetcher.fallback);

   /* [url_fetcher.flaresolverr] */
   ENV_BOOL("DAWN_URL_FETCHER_FLARESOLVERR_ENABLED", config->url_fetcher.flaresolverr.enabled);
   ENV_STRING("DAWN_URL_FETCHER_FLARESOLVERR_ENDPOINT", config->url_fetcher.flaresolverr.endpoint);
   ENV_INT("DAWN_URL_FETCHER_FLARESOLVERR_TIMEOUT_SEC",
           config->url_fetcher.flaresolverr.timeout_sec);
   ENV_SIZE_T("DAWN_URL_FETCHER_FLARESOLVERR_MAX_RESPONSE_BYTES",
              config->url_fetcher.flaresolverr.max_response_bytes);

   /* [url_fetcher.tavily] */
   ENV_INT("DAWN_URL_FETCHER_TAVILY_TIMEOUT_SEC", config->url_fetcher.tavily.timeout_sec);
   ENV_SIZE_T("DAWN_URL_FETCHER_TAVILY_MAX_RESPONSE_BYTES",
              config->url_fetcher.tavily.max_response_bytes);
   ENV_STRING("DAWN_URL_FETCHER_TAVILY_EXTRACT_DEPTH", config->url_fetcher.tavily.extract_depth);
   ENV_INT("DAWN_URL_FETCHER_TAVILY_RATE_LIMIT_PER_MINUTE",
           config->url_fetcher.tavily.rate_limit_per_minute);
   ENV_INT("DAWN_URL_FETCHER_TAVILY_RATE_LIMIT_PER_HOUR",
           config->url_fetcher.tavily.rate_limit_per_hour);
   ENV_INT("DAWN_URL_FETCHER_TAVILY_RATE_LIMIT_PER_DAY",
           config->url_fetcher.tavily.rate_limit_per_day);

   /* [mqtt] */
   ENV_BOOL("DAWN_MQTT_ENABLED", config->mqtt.enabled);
   ENV_STRING("DAWN_MQTT_BROKER", config->mqtt.broker);
   ENV_INT("DAWN_MQTT_PORT", config->mqtt.port);
   ENV_BOOL("DAWN_MQTT_TLS", config->mqtt.tls);
   ENV_STRING("DAWN_MQTT_TLS_CA_CERT", config->mqtt.tls_ca_cert);
   ENV_STRING("DAWN_MQTT_TLS_CERT_PATH", config->mqtt.tls_cert_path);
   ENV_STRING("DAWN_MQTT_TLS_KEY_PATH", config->mqtt.tls_key_path);
   ENV_STRING("MQTT_USERNAME", secrets->mqtt_username);
   ENV_STRING("MQTT_PASSWORD", secrets->mqtt_password);

   /* Home Assistant */
   ENV_SECRET("HOME_ASSISTANT_TOKEN", secrets->home_assistant_token);

   /* Google OAuth 2.0 */
   ENV_SECRET("DAWN_GOOGLE_CLIENT_ID", secrets->google_client_id);
   ENV_SECRET("DAWN_GOOGLE_CLIENT_SECRET", secrets->google_client_secret);
   ENV_SECRET("DAWN_GOOGLE_REDIRECT_URL", secrets->google_redirect_url);

   /* Satellite registration key */
   ENV_SECRET("DAWN_SATELLITE_KEY", secrets->satellite_registration_key);

   /* [network] */
   ENV_INT("DAWN_NETWORK_WORKERS", config->network.workers);
   ENV_INT("DAWN_NETWORK_SESSION_TIMEOUT_SEC", config->network.session_timeout_sec);
   ENV_INT("DAWN_NETWORK_LLM_TIMEOUT_MS", config->network.llm_timeout_ms);
   ENV_INT("DAWN_NETWORK_SUMMARIZATION_TIMEOUT_MS", config->network.summarization_timeout_ms);

   /* [tui] */
   ENV_BOOL("DAWN_TUI_ENABLED", config->tui.enabled);

   /* [debug] */
   ENV_BOOL("DAWN_DEBUG_MIC_RECORD", config->debug.mic_record);
   ENV_BOOL("DAWN_DEBUG_ASR_RECORD", config->debug.asr_record);
   ENV_BOOL("DAWN_DEBUG_AEC_RECORD", config->debug.aec_record);
   ENV_STRING("DAWN_DEBUG_RECORD_PATH", config->debug.record_path);

   /* [memory] */
   ENV_INT("DAWN_MEMORY_EXTRACTION_TIMEOUT_MS", config->memory.extraction_timeout_ms);

   /* [paths] */
   ENV_STRING("DAWN_PATHS_MUSIC_DIR", config->paths.music_dir);
}

/* =============================================================================
 * Configuration Dump
 * ============================================================================= */

void config_dump(const dawn_config_t *config) {
   if (!config)
      return;

   printf("=== DAWN Configuration ===\n\n");

   printf("[general]\n");
   printf("  ai_name = \"%s\"\n", config->general.ai_name);
   printf("  log_file = \"%s\"\n", config->general.log_file);

   printf("\n[persona]\n");
   printf("  description = \"%.*s%s\"\n", config->persona.description[0] ? 50 : 0,
          config->persona.description, strlen(config->persona.description) > 50 ? "..." : "");

   printf("\n[localization]\n");
   printf("  location = \"%s\"\n", config->localization.location);
   printf("  timezone = \"%s\"\n", config->localization.timezone);
   printf("  units = \"%s\"\n", config->localization.units);

   printf("\n[audio]\n");
   printf("  backend = \"%s\"\n", config->audio.backend);
   printf("  capture_device = \"%s\"\n", config->audio.capture_device);
   printf("  playback_device = \"%s\"\n", config->audio.playback_device);

   printf("\n[audio.bargein]\n");
   printf("  enabled = %s\n", config->audio.bargein.enabled ? "true" : "false");
   printf("  cooldown_ms = %d\n", config->audio.bargein.cooldown_ms);
   printf("  startup_cooldown_ms = %d\n", config->audio.bargein.startup_cooldown_ms);

   printf("\n[vad]\n");
   printf("  speech_threshold = %.2f\n", config->vad.speech_threshold);
   printf("  speech_threshold_tts = %.2f\n", config->vad.speech_threshold_tts);
   printf("  silence_threshold = %.2f\n", config->vad.silence_threshold);
   printf("  end_of_speech_duration = %.1f\n", config->vad.end_of_speech_duration);
   printf("  max_recording_duration = %.1f\n", config->vad.max_recording_duration);
   printf("  preroll_ms = %d\n", config->vad.preroll_ms);

   printf("\n[vad.chunking]\n");
   printf("  enabled = %s\n", config->vad.chunking.enabled ? "true" : "false");
   printf("  pause_duration = %.2f\n", config->vad.chunking.pause_duration);
   printf("  min_duration = %.1f\n", config->vad.chunking.min_duration);
   printf("  max_duration = %.1f\n", config->vad.chunking.max_duration);

   printf("\n[asr]\n");
   printf("  model = \"%s\"\n", config->asr.model);
   printf("  models_path = \"%s\"\n", config->asr.models_path);
   printf("  dedup_window_sec = %d\n", config->asr.dedup_window_sec);

   printf("\n[tts]\n");
   printf("  models_path = \"%s\"\n", config->tts.models_path);
   printf("  voice_model = \"%s\"\n", config->tts.voice_model);
   printf("  length_scale = %.2f\n", config->tts.length_scale);

   printf("\n[commands]\n");
   printf("  processing_mode = \"%s\"\n", config->commands.processing_mode);

   printf("\n[llm]\n");
   printf("  type = \"%s\"\n", config->llm.type);
   printf("  max_tokens = %d\n", config->llm.max_tokens);

   printf("\n[llm.cloud]\n");
   printf("  provider = \"%s\"\n", config->llm.cloud.provider);
   printf("  endpoint = \"%s\"\n", config->llm.cloud.endpoint);
   printf("  use_openrouter = %s\n", config->llm.cloud.use_openrouter ? "true" : "false");
   printf("  openai_use_responses_api = \"%s\"\n", config->llm.cloud.openai_use_responses_api);
   printf("  openai_default_model_idx = %d\n", config->llm.cloud.openai_default_model_idx);
   printf("  claude_default_model_idx = %d\n", config->llm.cloud.claude_default_model_idx);
   printf("  gemini_default_model_idx = %d\n", config->llm.cloud.gemini_default_model_idx);

   printf("\n[llm.local]\n");
   printf("  endpoint = \"%s\"\n", config->llm.local.endpoint);
   printf("  model = \"%s\"\n", config->llm.local.model);

   printf("\n[search]\n");
   printf("  engine = \"%s\"\n", config->search.engine);
   printf("  endpoint = \"%s\"\n", config->search.endpoint);

   printf("\n[search.summarizer]\n");
   printf("  backend = \"%s\"\n", config->search.summarizer.backend);
   printf("  threshold_bytes = %zu\n", config->search.summarizer.threshold_bytes);
   printf("  target_words = %zu\n", config->search.summarizer.target_words);
   printf("  target_ratio = %.2f\n", config->search.summarizer.target_ratio);

   printf("\n[url_fetcher]\n");
   printf("  whitelist_count = %d\n", config->url_fetcher.whitelist_count);
   printf("  fallback = \"%s\"\n", config->url_fetcher.fallback);

   printf("\n[url_fetcher.flaresolverr]\n");
   printf("  enabled = %s\n", config->url_fetcher.flaresolverr.enabled ? "true" : "false");
   printf("  endpoint = \"%s\"\n", config->url_fetcher.flaresolverr.endpoint);
   printf("  timeout_sec = %d\n", config->url_fetcher.flaresolverr.timeout_sec);
   printf("  max_response_bytes = %zu\n", config->url_fetcher.flaresolverr.max_response_bytes);

   printf("\n[url_fetcher.tavily]\n");
   printf("  timeout_sec = %d\n", config->url_fetcher.tavily.timeout_sec);
   printf("  max_response_bytes = %zu\n", config->url_fetcher.tavily.max_response_bytes);
   printf("  extract_depth = \"%s\"\n", config->url_fetcher.tavily.extract_depth);
   printf("  rate_limit_per_minute = %d\n", config->url_fetcher.tavily.rate_limit_per_minute);
   printf("  rate_limit_per_hour = %d\n", config->url_fetcher.tavily.rate_limit_per_hour);
   printf("  rate_limit_per_day = %d\n", config->url_fetcher.tavily.rate_limit_per_day);

   printf("\n[mqtt]\n");
   printf("  enabled = %s\n", config->mqtt.enabled ? "true" : "false");
   printf("  broker = \"%s\"\n", config->mqtt.broker);
   printf("  port = %d\n", config->mqtt.port);
   printf("  tls = %s\n", config->mqtt.tls ? "true" : "false");
   if (config->mqtt.tls_ca_cert[0])
      printf("  tls_ca_cert = \"%s\"\n", config->mqtt.tls_ca_cert);
   if (config->mqtt.tls_cert_path[0])
      printf("  tls_cert_path = \"%s\"\n", config->mqtt.tls_cert_path);
   if (config->mqtt.tls_key_path[0])
      printf("  tls_key_path = \"%s\"\n", config->mqtt.tls_key_path);

   printf("\n[network]\n");
   printf("  workers = %d\n", config->network.workers);
   printf("  session_timeout_sec = %d\n", config->network.session_timeout_sec);
   printf("  llm_timeout_ms = %d\n", config->network.llm_timeout_ms);
   printf("  summarization_timeout_ms = %d\n", config->network.summarization_timeout_ms);

   printf("\n[tui]\n");
   printf("  enabled = %s\n", config->tui.enabled ? "true" : "false");

   printf("\n[debug]\n");
   printf("  mic_record = %s\n", config->debug.mic_record ? "true" : "false");
   printf("  asr_record = %s\n", config->debug.asr_record ? "true" : "false");
   printf("  aec_record = %s\n", config->debug.aec_record ? "true" : "false");
   printf("  record_path = \"%s\"\n", config->debug.record_path);

   printf("\n[paths]\n");
   printf("  music_dir = \"%s\"\n", config->paths.music_dir);
}

/* =============================================================================
 * Settings Dump with Sources
 * ============================================================================= */

/* Source detection: compare against defaults and check env vars */
typedef enum {
   SOURCE_DEFAULT,
   SOURCE_FILE,
   SOURCE_ENV
} setting_source_t;

static const char *source_name(setting_source_t src) {
   switch (src) {
      case SOURCE_DEFAULT:
         return "default";
      case SOURCE_FILE:
         return "file";
      case SOURCE_ENV:
         return "env";
      default:
         return "unknown";
   }
}

/* Helper to detect source of a string setting */
static setting_source_t detect_source_str(const char *current,
                                          const char *default_val,
                                          const char *env_name) {
   if (getenv(env_name))
      return SOURCE_ENV;
   if (strcmp(current, default_val) != 0)
      return SOURCE_FILE;
   return SOURCE_DEFAULT;
}

/* Helper to detect source of an int setting */
static setting_source_t detect_source_int(int current, int default_val, const char *env_name) {
   if (getenv(env_name))
      return SOURCE_ENV;
   if (current != default_val)
      return SOURCE_FILE;
   return SOURCE_DEFAULT;
}

/* Helper to detect source of a float setting */
static setting_source_t detect_source_float(float current,
                                            float default_val,
                                            const char *env_name) {
   if (getenv(env_name))
      return SOURCE_ENV;
   /* Use epsilon for float comparison */
   if (current < default_val - 0.001f || current > default_val + 0.001f)
      return SOURCE_FILE;
   return SOURCE_DEFAULT;
}

/* Helper to detect source of a size_t setting */
static setting_source_t detect_source_size(size_t current,
                                           size_t default_val,
                                           const char *env_name) {
   if (getenv(env_name))
      return SOURCE_ENV;
   if (current != default_val)
      return SOURCE_FILE;
   return SOURCE_DEFAULT;
}

/* Helper to detect source of a bool setting */
static setting_source_t detect_source_bool(bool current, bool default_val, const char *env_name) {
   if (getenv(env_name))
      return SOURCE_ENV;
   if (current != default_val)
      return SOURCE_FILE;
   return SOURCE_DEFAULT;
}

/* Print macros for consistent formatting */
#define PRINT_SETTING_STR(name, value, env_name, source) \
   printf("  %-40s = \"%s\"\n", name, value);            \
   printf("    %-38s   (%s)\n\n", env_name, source_name(source))

#define PRINT_SETTING_INT(name, value, env_name, source) \
   printf("  %-40s = %d\n", name, value);                \
   printf("    %-38s   (%s)\n\n", env_name, source_name(source))

#define PRINT_SETTING_FLOAT(name, value, env_name, source) \
   printf("  %-40s = %.2f\n", name, (double)(value));      \
   printf("    %-38s   (%s)\n\n", env_name, source_name(source))

#define PRINT_SETTING_SIZE(name, value, env_name, source) \
   printf("  %-40s = %zu\n", name, value);                \
   printf("    %-38s   (%s)\n\n", env_name, source_name(source))

#define PRINT_SETTING_BOOL(name, value, env_name, source)      \
   printf("  %-40s = %s\n", name, (value) ? "true" : "false"); \
   printf("    %-38s   (%s)\n\n", env_name, source_name(source))

void config_dump_settings(const dawn_config_t *config,
                          const secrets_config_t *secrets,
                          const char *config_file_loaded) {
   if (!config)
      return;

   /* Create defaults for comparison */
   dawn_config_t defaults;
   config_set_defaults(&defaults);

   printf("================================================================================\n");
   printf("DAWN Settings - Current Values, Environment Variables, and Sources\n");
   printf("================================================================================\n\n");
   printf("Source priority: defaults -> config file -> environment variables -> CLI\n\n");

   if (config_file_loaded) {
      printf("Config file loaded: %s\n\n", config_file_loaded);
   } else {
      printf("Config file loaded: (none - using defaults)\n\n");
   }

   printf("Legend:\n");
   printf("  default = compile-time default value\n");
   printf("  file    = loaded from config file\n");
   printf("  env     = overridden by environment variable\n\n");

   /* [general] */
   printf("[general]\n");
   PRINT_SETTING_STR("ai_name", config->general.ai_name, "DAWN_GENERAL_AI_NAME",
                     detect_source_str(config->general.ai_name, defaults.general.ai_name,
                                       "DAWN_GENERAL_AI_NAME"));
   PRINT_SETTING_STR("log_file", config->general.log_file, "DAWN_GENERAL_LOG_FILE",
                     detect_source_str(config->general.log_file, defaults.general.log_file,
                                       "DAWN_GENERAL_LOG_FILE"));

   /* [persona] */
   printf("[persona]\n");
   printf("  %-40s = \"%.*s%s\"\n", "description", config->persona.description[0] ? 50 : 0,
          config->persona.description, strlen(config->persona.description) > 50 ? "..." : "");
   printf("    %-38s   (%s)\n\n", "DAWN_PERSONA_DESCRIPTION",
          source_name(detect_source_str(config->persona.description, defaults.persona.description,
                                        "DAWN_PERSONA_DESCRIPTION")));

   /* [localization] */
   printf("[localization]\n");
   PRINT_SETTING_STR("location", config->localization.location, "DAWN_LOCALIZATION_LOCATION",
                     detect_source_str(config->localization.location,
                                       defaults.localization.location,
                                       "DAWN_LOCALIZATION_LOCATION"));
   PRINT_SETTING_STR("timezone", config->localization.timezone, "DAWN_LOCALIZATION_TIMEZONE",
                     detect_source_str(config->localization.timezone,
                                       defaults.localization.timezone,
                                       "DAWN_LOCALIZATION_TIMEZONE"));
   PRINT_SETTING_STR("units", config->localization.units, "DAWN_LOCALIZATION_UNITS",
                     detect_source_str(config->localization.units, defaults.localization.units,
                                       "DAWN_LOCALIZATION_UNITS"));

   /* [audio] */
   printf("[audio]\n");
   PRINT_SETTING_STR("backend", config->audio.backend, "DAWN_AUDIO_BACKEND",
                     detect_source_str(config->audio.backend, defaults.audio.backend,
                                       "DAWN_AUDIO_BACKEND"));
   PRINT_SETTING_STR("capture_device", config->audio.capture_device, "DAWN_AUDIO_CAPTURE_DEVICE",
                     detect_source_str(config->audio.capture_device, defaults.audio.capture_device,
                                       "DAWN_AUDIO_CAPTURE_DEVICE"));
   PRINT_SETTING_STR("playback_device", config->audio.playback_device, "DAWN_AUDIO_PLAYBACK_DEVICE",
                     detect_source_str(config->audio.playback_device,
                                       defaults.audio.playback_device,
                                       "DAWN_AUDIO_PLAYBACK_DEVICE"));

   /* [audio.bargein] */
   printf("[audio.bargein]\n");
   PRINT_SETTING_BOOL("enabled", config->audio.bargein.enabled, "DAWN_AUDIO_BARGEIN_ENABLED",
                      detect_source_bool(config->audio.bargein.enabled,
                                         defaults.audio.bargein.enabled,
                                         "DAWN_AUDIO_BARGEIN_ENABLED"));
   PRINT_SETTING_INT("cooldown_ms", config->audio.bargein.cooldown_ms,
                     "DAWN_AUDIO_BARGEIN_COOLDOWN_MS",
                     detect_source_int(config->audio.bargein.cooldown_ms,
                                       defaults.audio.bargein.cooldown_ms,
                                       "DAWN_AUDIO_BARGEIN_COOLDOWN_MS"));
   PRINT_SETTING_INT("startup_cooldown_ms", config->audio.bargein.startup_cooldown_ms,
                     "DAWN_AUDIO_BARGEIN_STARTUP_COOLDOWN_MS",
                     detect_source_int(config->audio.bargein.startup_cooldown_ms,
                                       defaults.audio.bargein.startup_cooldown_ms,
                                       "DAWN_AUDIO_BARGEIN_STARTUP_COOLDOWN_MS"));

   /* [vad] */
   printf("[vad]\n");
   PRINT_SETTING_FLOAT("speech_threshold", config->vad.speech_threshold,
                       "DAWN_VAD_SPEECH_THRESHOLD",
                       detect_source_float(config->vad.speech_threshold,
                                           defaults.vad.speech_threshold,
                                           "DAWN_VAD_SPEECH_THRESHOLD"));
   PRINT_SETTING_FLOAT("speech_threshold_tts", config->vad.speech_threshold_tts,
                       "DAWN_VAD_SPEECH_THRESHOLD_TTS",
                       detect_source_float(config->vad.speech_threshold_tts,
                                           defaults.vad.speech_threshold_tts,
                                           "DAWN_VAD_SPEECH_THRESHOLD_TTS"));
   PRINT_SETTING_FLOAT("silence_threshold", config->vad.silence_threshold,
                       "DAWN_VAD_SILENCE_THRESHOLD",
                       detect_source_float(config->vad.silence_threshold,
                                           defaults.vad.silence_threshold,
                                           "DAWN_VAD_SILENCE_THRESHOLD"));
   PRINT_SETTING_FLOAT("end_of_speech_duration", config->vad.end_of_speech_duration,
                       "DAWN_VAD_END_OF_SPEECH_DURATION",
                       detect_source_float(config->vad.end_of_speech_duration,
                                           defaults.vad.end_of_speech_duration,
                                           "DAWN_VAD_END_OF_SPEECH_DURATION"));
   PRINT_SETTING_FLOAT("max_recording_duration", config->vad.max_recording_duration,
                       "DAWN_VAD_MAX_RECORDING_DURATION",
                       detect_source_float(config->vad.max_recording_duration,
                                           defaults.vad.max_recording_duration,
                                           "DAWN_VAD_MAX_RECORDING_DURATION"));
   PRINT_SETTING_INT("preroll_ms", config->vad.preroll_ms, "DAWN_VAD_PREROLL_MS",
                     detect_source_int(config->vad.preroll_ms, defaults.vad.preroll_ms,
                                       "DAWN_VAD_PREROLL_MS"));

   /* [vad.chunking] */
   printf("[vad.chunking]\n");
   PRINT_SETTING_BOOL("enabled", config->vad.chunking.enabled, "DAWN_VAD_CHUNKING_ENABLED",
                      detect_source_bool(config->vad.chunking.enabled,
                                         defaults.vad.chunking.enabled,
                                         "DAWN_VAD_CHUNKING_ENABLED"));
   PRINT_SETTING_FLOAT("pause_duration", config->vad.chunking.pause_duration,
                       "DAWN_VAD_CHUNKING_PAUSE_DURATION",
                       detect_source_float(config->vad.chunking.pause_duration,
                                           defaults.vad.chunking.pause_duration,
                                           "DAWN_VAD_CHUNKING_PAUSE_DURATION"));
   PRINT_SETTING_FLOAT("min_duration", config->vad.chunking.min_duration,
                       "DAWN_VAD_CHUNKING_MIN_DURATION",
                       detect_source_float(config->vad.chunking.min_duration,
                                           defaults.vad.chunking.min_duration,
                                           "DAWN_VAD_CHUNKING_MIN_DURATION"));
   PRINT_SETTING_FLOAT("max_duration", config->vad.chunking.max_duration,
                       "DAWN_VAD_CHUNKING_MAX_DURATION",
                       detect_source_float(config->vad.chunking.max_duration,
                                           defaults.vad.chunking.max_duration,
                                           "DAWN_VAD_CHUNKING_MAX_DURATION"));

   /* [asr] */
   printf("[asr]\n");
   PRINT_SETTING_STR("model", config->asr.model, "DAWN_ASR_MODEL",
                     detect_source_str(config->asr.model, defaults.asr.model, "DAWN_ASR_MODEL"));
   PRINT_SETTING_STR("models_path", config->asr.models_path, "DAWN_ASR_MODELS_PATH",
                     detect_source_str(config->asr.models_path, defaults.asr.models_path,
                                       "DAWN_ASR_MODELS_PATH"));
   PRINT_SETTING_INT("dedup_window_sec", config->asr.dedup_window_sec, "DAWN_ASR_DEDUP_WINDOW_SEC",
                     detect_source_int(config->asr.dedup_window_sec, defaults.asr.dedup_window_sec,
                                       "DAWN_ASR_DEDUP_WINDOW_SEC"));

   /* [tts] */
   printf("[tts]\n");
   PRINT_SETTING_STR("models_path", config->tts.models_path, "DAWN_TTS_MODELS_PATH",
                     detect_source_str(config->tts.models_path, defaults.tts.models_path,
                                       "DAWN_TTS_MODELS_PATH"));
   PRINT_SETTING_STR("voice_model", config->tts.voice_model, "DAWN_TTS_VOICE_MODEL",
                     detect_source_str(config->tts.voice_model, defaults.tts.voice_model,
                                       "DAWN_TTS_VOICE_MODEL"));
   PRINT_SETTING_FLOAT("length_scale", config->tts.length_scale, "DAWN_TTS_LENGTH_SCALE",
                       detect_source_float(config->tts.length_scale, defaults.tts.length_scale,
                                           "DAWN_TTS_LENGTH_SCALE"));

   /* [commands] */
   printf("[commands]\n");
   PRINT_SETTING_STR("processing_mode", config->commands.processing_mode,
                     "DAWN_COMMANDS_PROCESSING_MODE",
                     detect_source_str(config->commands.processing_mode,
                                       defaults.commands.processing_mode,
                                       "DAWN_COMMANDS_PROCESSING_MODE"));

   /* [llm] */
   printf("[llm]\n");
   PRINT_SETTING_STR("type", config->llm.type, "DAWN_LLM_TYPE",
                     detect_source_str(config->llm.type, defaults.llm.type, "DAWN_LLM_TYPE"));
   PRINT_SETTING_INT("max_tokens", config->llm.max_tokens, "DAWN_LLM_MAX_TOKENS",
                     detect_source_int(config->llm.max_tokens, defaults.llm.max_tokens,
                                       "DAWN_LLM_MAX_TOKENS"));

   /* [llm.cloud] */
   printf("[llm.cloud]\n");
   PRINT_SETTING_STR("provider", config->llm.cloud.provider, "DAWN_LLM_CLOUD_PROVIDER",
                     detect_source_str(config->llm.cloud.provider, defaults.llm.cloud.provider,
                                       "DAWN_LLM_CLOUD_PROVIDER"));
   PRINT_SETTING_STR("endpoint", config->llm.cloud.endpoint, "DAWN_LLM_CLOUD_ENDPOINT",
                     detect_source_str(config->llm.cloud.endpoint, defaults.llm.cloud.endpoint,
                                       "DAWN_LLM_CLOUD_ENDPOINT"));
   PRINT_SETTING_STR("openai_use_responses_api", config->llm.cloud.openai_use_responses_api,
                     "DAWN_LLM_CLOUD_OPENAI_USE_RESPONSES_API",
                     detect_source_str(config->llm.cloud.openai_use_responses_api,
                                       defaults.llm.cloud.openai_use_responses_api,
                                       "DAWN_LLM_CLOUD_OPENAI_USE_RESPONSES_API"));
   PRINT_SETTING_INT("openai_default_model_idx", config->llm.cloud.openai_default_model_idx,
                     "DAWN_LLM_CLOUD_OPENAI_DEFAULT_MODEL_IDX",
                     detect_source_int(config->llm.cloud.openai_default_model_idx,
                                       defaults.llm.cloud.openai_default_model_idx,
                                       "DAWN_LLM_CLOUD_OPENAI_DEFAULT_MODEL_IDX"));
   PRINT_SETTING_INT("claude_default_model_idx", config->llm.cloud.claude_default_model_idx,
                     "DAWN_LLM_CLOUD_CLAUDE_DEFAULT_MODEL_IDX",
                     detect_source_int(config->llm.cloud.claude_default_model_idx,
                                       defaults.llm.cloud.claude_default_model_idx,
                                       "DAWN_LLM_CLOUD_CLAUDE_DEFAULT_MODEL_IDX"));
   PRINT_SETTING_INT("gemini_default_model_idx", config->llm.cloud.gemini_default_model_idx,
                     "DAWN_LLM_CLOUD_GEMINI_DEFAULT_MODEL_IDX",
                     detect_source_int(config->llm.cloud.gemini_default_model_idx,
                                       defaults.llm.cloud.gemini_default_model_idx,
                                       "DAWN_LLM_CLOUD_GEMINI_DEFAULT_MODEL_IDX"));
   PRINT_SETTING_BOOL("vision_enabled", config->llm.cloud.vision_enabled,
                      "DAWN_LLM_CLOUD_VISION_ENABLED",
                      detect_source_bool(config->llm.cloud.vision_enabled,
                                         defaults.llm.cloud.vision_enabled,
                                         "DAWN_LLM_CLOUD_VISION_ENABLED"));
   PRINT_SETTING_BOOL("use_openrouter", config->llm.cloud.use_openrouter,
                      "DAWN_LLM_CLOUD_USE_OPENROUTER",
                      detect_source_bool(config->llm.cloud.use_openrouter,
                                         defaults.llm.cloud.use_openrouter,
                                         "DAWN_LLM_CLOUD_USE_OPENROUTER"));

   /* [llm.local] */
   printf("[llm.local]\n");
   PRINT_SETTING_STR("endpoint", config->llm.local.endpoint, "DAWN_LLM_LOCAL_ENDPOINT",
                     detect_source_str(config->llm.local.endpoint, defaults.llm.local.endpoint,
                                       "DAWN_LLM_LOCAL_ENDPOINT"));
   PRINT_SETTING_STR("model", config->llm.local.model, "DAWN_LLM_LOCAL_MODEL",
                     detect_source_str(config->llm.local.model, defaults.llm.local.model,
                                       "DAWN_LLM_LOCAL_MODEL"));
   PRINT_SETTING_BOOL("vision_enabled", config->llm.local.vision_enabled,
                      "DAWN_LLM_LOCAL_VISION_ENABLED",
                      detect_source_bool(config->llm.local.vision_enabled,
                                         defaults.llm.local.vision_enabled,
                                         "DAWN_LLM_LOCAL_VISION_ENABLED"));

   /* [llm.tools] */
   printf("[llm.tools]\n");
   PRINT_SETTING_STR("mode", config->llm.tools.mode, "DAWN_LLM_TOOLS_MODE",
                     detect_source_str(config->llm.tools.mode, defaults.llm.tools.mode,
                                       "DAWN_LLM_TOOLS_MODE"));

   /* [search] */
   printf("[search]\n");
   PRINT_SETTING_STR("engine", config->search.engine, "DAWN_SEARCH_ENGINE",
                     detect_source_str(config->search.engine, defaults.search.engine,
                                       "DAWN_SEARCH_ENGINE"));
   PRINT_SETTING_STR("endpoint", config->search.endpoint, "DAWN_SEARCH_ENDPOINT",
                     detect_source_str(config->search.endpoint, defaults.search.endpoint,
                                       "DAWN_SEARCH_ENDPOINT"));

   /* [search.summarizer] */
   printf("[search.summarizer]\n");
   PRINT_SETTING_STR("backend", config->search.summarizer.backend, "DAWN_SEARCH_SUMMARIZER_BACKEND",
                     detect_source_str(config->search.summarizer.backend,
                                       defaults.search.summarizer.backend,
                                       "DAWN_SEARCH_SUMMARIZER_BACKEND"));
   PRINT_SETTING_SIZE("threshold_bytes", config->search.summarizer.threshold_bytes,
                      "DAWN_SEARCH_SUMMARIZER_THRESHOLD_BYTES",
                      detect_source_size(config->search.summarizer.threshold_bytes,
                                         defaults.search.summarizer.threshold_bytes,
                                         "DAWN_SEARCH_SUMMARIZER_THRESHOLD_BYTES"));
   PRINT_SETTING_SIZE("target_words", config->search.summarizer.target_words,
                      "DAWN_SEARCH_SUMMARIZER_TARGET_WORDS",
                      detect_source_size(config->search.summarizer.target_words,
                                         defaults.search.summarizer.target_words,
                                         "DAWN_SEARCH_SUMMARIZER_TARGET_WORDS"));
   PRINT_SETTING_FLOAT("target_ratio", config->search.summarizer.target_ratio,
                       "DAWN_SEARCH_SUMMARIZER_TARGET_RATIO",
                       detect_source_float(config->search.summarizer.target_ratio,
                                           defaults.search.summarizer.target_ratio,
                                           "DAWN_SEARCH_SUMMARIZER_TARGET_RATIO"));

   /* [url_fetcher.flaresolverr] */
   printf("[url_fetcher.flaresolverr]\n");
   PRINT_SETTING_BOOL("enabled", config->url_fetcher.flaresolverr.enabled,
                      "DAWN_URL_FETCHER_FLARESOLVERR_ENABLED",
                      detect_source_bool(config->url_fetcher.flaresolverr.enabled,
                                         defaults.url_fetcher.flaresolverr.enabled,
                                         "DAWN_URL_FETCHER_FLARESOLVERR_ENABLED"));
   PRINT_SETTING_STR("endpoint", config->url_fetcher.flaresolverr.endpoint,
                     "DAWN_URL_FETCHER_FLARESOLVERR_ENDPOINT",
                     detect_source_str(config->url_fetcher.flaresolverr.endpoint,
                                       defaults.url_fetcher.flaresolverr.endpoint,
                                       "DAWN_URL_FETCHER_FLARESOLVERR_ENDPOINT"));
   PRINT_SETTING_INT("timeout_sec", config->url_fetcher.flaresolverr.timeout_sec,
                     "DAWN_URL_FETCHER_FLARESOLVERR_TIMEOUT_SEC",
                     detect_source_int(config->url_fetcher.flaresolverr.timeout_sec,
                                       defaults.url_fetcher.flaresolverr.timeout_sec,
                                       "DAWN_URL_FETCHER_FLARESOLVERR_TIMEOUT_SEC"));
   PRINT_SETTING_SIZE("max_response_bytes", config->url_fetcher.flaresolverr.max_response_bytes,
                      "DAWN_URL_FETCHER_FLARESOLVERR_MAX_RESPONSE_BYTES",
                      detect_source_size(config->url_fetcher.flaresolverr.max_response_bytes,
                                         defaults.url_fetcher.flaresolverr.max_response_bytes,
                                         "DAWN_URL_FETCHER_FLARESOLVERR_MAX_RESPONSE_BYTES"));

   /* [mqtt] */
   printf("[mqtt]\n");
   PRINT_SETTING_BOOL("enabled", config->mqtt.enabled, "DAWN_MQTT_ENABLED",
                      detect_source_bool(config->mqtt.enabled, defaults.mqtt.enabled,
                                         "DAWN_MQTT_ENABLED"));
   PRINT_SETTING_STR("broker", config->mqtt.broker, "DAWN_MQTT_BROKER",
                     detect_source_str(config->mqtt.broker, defaults.mqtt.broker,
                                       "DAWN_MQTT_BROKER"));
   PRINT_SETTING_INT("port", config->mqtt.port, "DAWN_MQTT_PORT",
                     detect_source_int(config->mqtt.port, defaults.mqtt.port, "DAWN_MQTT_PORT"));
   PRINT_SETTING_BOOL("tls", config->mqtt.tls, "DAWN_MQTT_TLS",
                      detect_source_bool(config->mqtt.tls, defaults.mqtt.tls, "DAWN_MQTT_TLS"));
   PRINT_SETTING_STR("tls_ca_cert", config->mqtt.tls_ca_cert, "DAWN_MQTT_TLS_CA_CERT",
                     detect_source_str(config->mqtt.tls_ca_cert, defaults.mqtt.tls_ca_cert,
                                       "DAWN_MQTT_TLS_CA_CERT"));
   PRINT_SETTING_STR("tls_cert_path", config->mqtt.tls_cert_path, "DAWN_MQTT_TLS_CERT_PATH",
                     detect_source_str(config->mqtt.tls_cert_path, defaults.mqtt.tls_cert_path,
                                       "DAWN_MQTT_TLS_CERT_PATH"));
   PRINT_SETTING_STR("tls_key_path", config->mqtt.tls_key_path, "DAWN_MQTT_TLS_KEY_PATH",
                     detect_source_str(config->mqtt.tls_key_path, defaults.mqtt.tls_key_path,
                                       "DAWN_MQTT_TLS_KEY_PATH"));

   /* [network] */
   printf("[network]\n");
   PRINT_SETTING_INT("workers", config->network.workers, "DAWN_NETWORK_WORKERS",
                     detect_source_int(config->network.workers, defaults.network.workers,
                                       "DAWN_NETWORK_WORKERS"));
   PRINT_SETTING_INT("session_timeout_sec", config->network.session_timeout_sec,
                     "DAWN_NETWORK_SESSION_TIMEOUT_SEC",
                     detect_source_int(config->network.session_timeout_sec,
                                       defaults.network.session_timeout_sec,
                                       "DAWN_NETWORK_SESSION_TIMEOUT_SEC"));
   PRINT_SETTING_INT("llm_timeout_ms", config->network.llm_timeout_ms,
                     "DAWN_NETWORK_LLM_TIMEOUT_MS",
                     detect_source_int(config->network.llm_timeout_ms,
                                       defaults.network.llm_timeout_ms,
                                       "DAWN_NETWORK_LLM_TIMEOUT_MS"));
   PRINT_SETTING_INT("summarization_timeout_ms", config->network.summarization_timeout_ms,
                     "DAWN_NETWORK_SUMMARIZATION_TIMEOUT_MS",
                     detect_source_int(config->network.summarization_timeout_ms,
                                       defaults.network.summarization_timeout_ms,
                                       "DAWN_NETWORK_SUMMARIZATION_TIMEOUT_MS"));

   /* [tui] */
   printf("[tui]\n");
   PRINT_SETTING_BOOL("enabled", config->tui.enabled, "DAWN_TUI_ENABLED",
                      detect_source_bool(config->tui.enabled, defaults.tui.enabled,
                                         "DAWN_TUI_ENABLED"));

   /* [debug] */
   printf("[debug]\n");
   PRINT_SETTING_BOOL("mic_record", config->debug.mic_record, "DAWN_DEBUG_MIC_RECORD",
                      detect_source_bool(config->debug.mic_record, defaults.debug.mic_record,
                                         "DAWN_DEBUG_MIC_RECORD"));
   PRINT_SETTING_BOOL("asr_record", config->debug.asr_record, "DAWN_DEBUG_ASR_RECORD",
                      detect_source_bool(config->debug.asr_record, defaults.debug.asr_record,
                                         "DAWN_DEBUG_ASR_RECORD"));
   PRINT_SETTING_BOOL("aec_record", config->debug.aec_record, "DAWN_DEBUG_AEC_RECORD",
                      detect_source_bool(config->debug.aec_record, defaults.debug.aec_record,
                                         "DAWN_DEBUG_AEC_RECORD"));
   PRINT_SETTING_STR("record_path", config->debug.record_path, "DAWN_DEBUG_RECORD_PATH",
                     detect_source_str(config->debug.record_path, defaults.debug.record_path,
                                       "DAWN_DEBUG_RECORD_PATH"));

   /* [paths] */
   printf("[paths]\n");
   PRINT_SETTING_STR("music_dir", config->paths.music_dir, "DAWN_PATHS_MUSIC_DIR",
                     detect_source_str(config->paths.music_dir, defaults.paths.music_dir,
                                       "DAWN_PATHS_MUSIC_DIR"));

   /* Secrets (only show env var names, not values) */
   printf("================================================================================\n");
   printf("Secrets (values hidden)\n");
   printf("================================================================================\n\n");
   printf("  OPENAI_API_KEY                           %s\n",
          (secrets && secrets->openai_api_key[0]) ? "[set]" : "[not set]");
   printf("  ANTHROPIC_API_KEY                        %s\n",
          (secrets && secrets->claude_api_key[0]) ? "[set]" : "[not set]");
   printf("  GEMINI_API_KEY                           %s\n",
          (secrets && secrets->gemini_api_key[0]) ? "[set]" : "[not set]");
   printf("  MQTT_USERNAME                            %s\n",
          (secrets && secrets->mqtt_username[0]) ? "[set]" : "[not set]");
   printf("  MQTT_PASSWORD                            %s\n",
          (secrets && secrets->mqtt_password[0]) ? "[set]" : "[not set]");
   printf("  HOME_ASSISTANT_TOKEN                     %s\n",
          (secrets && secrets->home_assistant_token[0]) ? "[set]" : "[not set]");
   printf("  DAWN_GOOGLE_CLIENT_ID                    %s\n",
          (secrets && secrets->google_client_id[0]) ? "[set]" : "[not set]");
   printf("  DAWN_GOOGLE_CLIENT_SECRET                %s\n",
          (secrets && secrets->google_client_secret[0]) ? "[set]" : "[not set]");
   printf("  DAWN_SATELLITE_KEY                       %s\n\n",
          (secrets && secrets->satellite_registration_key[0]) ? "[set]" : "[not set]");
}

void config_dump_toml(const dawn_config_t *config) {
   if (!config)
      return;

   printf("# DAWN Configuration (generated)\n");
   printf("# Save as: ~/.config/dawn/config.toml or ./dawn.toml\n\n");

   printf("[general]\n");
   printf("ai_name = \"%s\"\n", config->general.ai_name);
   if (config->general.log_file[0])
      printf("log_file = \"%s\"\n", config->general.log_file);

   printf("\n[localization]\n");
   if (config->localization.location[0])
      printf("location = \"%s\"\n", config->localization.location);
   if (config->localization.timezone[0])
      printf("timezone = \"%s\"\n", config->localization.timezone);
   printf("units = \"%s\"\n", config->localization.units);

   printf("\n[audio]\n");
   printf("backend = \"%s\"\n", config->audio.backend);
   printf("capture_device = \"%s\"\n", config->audio.capture_device);
   printf("playback_device = \"%s\"\n", config->audio.playback_device);

   printf("\n[audio.bargein]\n");
   printf("enabled = %s\n", config->audio.bargein.enabled ? "true" : "false");
   printf("cooldown_ms = %d\n", config->audio.bargein.cooldown_ms);
   printf("startup_cooldown_ms = %d\n", config->audio.bargein.startup_cooldown_ms);

   printf("\n[vad]\n");
   printf("speech_threshold = %.2f\n", config->vad.speech_threshold);
   printf("speech_threshold_tts = %.2f\n", config->vad.speech_threshold_tts);
   printf("silence_threshold = %.2f\n", config->vad.silence_threshold);
   printf("end_of_speech_duration = %.1f\n", config->vad.end_of_speech_duration);
   printf("max_recording_duration = %.1f\n", config->vad.max_recording_duration);
   printf("preroll_ms = %d\n", config->vad.preroll_ms);

   printf("\n[vad.chunking]\n");
   printf("enabled = %s\n", config->vad.chunking.enabled ? "true" : "false");
   printf("pause_duration = %.2f\n", config->vad.chunking.pause_duration);
   printf("min_chunk_duration = %.1f\n", config->vad.chunking.min_duration);
   printf("max_chunk_duration = %.1f\n", config->vad.chunking.max_duration);

   printf("\n[asr]\n");
   printf("model = \"%s\"\n", config->asr.model);
   printf("models_path = \"%s\"\n", config->asr.models_path);
   printf("dedup_window_sec = %d\n", config->asr.dedup_window_sec);

   printf("\n[tts]\n");
   printf("models_path = \"%s\"\n", config->tts.models_path);
   printf("voice_model = \"%s\"\n", config->tts.voice_model);
   printf("length_scale = %.2f\n", config->tts.length_scale);

   printf("\n[commands]\n");
   printf("processing_mode = \"%s\"\n", config->commands.processing_mode);

   printf("\n[llm]\n");
   printf("type = \"%s\"\n", config->llm.type);
   printf("max_tokens = %d\n", config->llm.max_tokens);

   printf("\n[llm.cloud]\n");
   printf("provider = \"%s\"\n", config->llm.cloud.provider);
   if (config->llm.cloud.endpoint[0])
      printf("endpoint = \"%s\"\n", config->llm.cloud.endpoint);
   printf("openai_default_model_idx = %d\n", config->llm.cloud.openai_default_model_idx);
   printf("claude_default_model_idx = %d\n", config->llm.cloud.claude_default_model_idx);
   printf("gemini_default_model_idx = %d\n", config->llm.cloud.gemini_default_model_idx);

   printf("\n[llm.local]\n");
   printf("endpoint = \"%s\"\n", config->llm.local.endpoint);
   if (config->llm.local.model[0])
      printf("model = \"%s\"\n", config->llm.local.model);

   printf("\n[search]\n");
   printf("engine = \"%s\"\n", config->search.engine);
   printf("endpoint = \"%s\"\n", config->search.endpoint);

   printf("\n[search.summarizer]\n");
   printf("backend = \"%s\"\n", config->search.summarizer.backend);
   printf("threshold_bytes = %zu\n", config->search.summarizer.threshold_bytes);
   printf("target_words = %zu\n", config->search.summarizer.target_words);
   printf("target_ratio = %.2f\n", config->search.summarizer.target_ratio);

   printf("\n[mqtt]\n");
   printf("enabled = %s\n", config->mqtt.enabled ? "true" : "false");
   printf("broker = \"%s\"\n", config->mqtt.broker);
   printf("port = %d\n", config->mqtt.port);
   printf("tls = %s\n", config->mqtt.tls ? "true" : "false");
   if (config->mqtt.tls_ca_cert[0])
      printf("tls_ca_cert = \"%s\"\n", config->mqtt.tls_ca_cert);
   if (config->mqtt.tls_cert_path[0])
      printf("tls_cert_path = \"%s\"\n", config->mqtt.tls_cert_path);
   if (config->mqtt.tls_key_path[0])
      printf("tls_key_path = \"%s\"\n", config->mqtt.tls_key_path);

   printf("\n[network]\n");
   printf("workers = %d\n", config->network.workers);

   printf("\n[tui]\n");
   printf("enabled = %s\n", config->tui.enabled ? "true" : "false");

   printf("\n[debug]\n");
   printf("mic_record = %s\n", config->debug.mic_record ? "true" : "false");
   printf("asr_record = %s\n", config->debug.asr_record ? "true" : "false");
   printf("aec_record = %s\n", config->debug.aec_record ? "true" : "false");
   printf("record_path = \"%s\"\n", config->debug.record_path);

   printf("\n[paths]\n");
   printf("music_dir = \"%s\"\n", config->paths.music_dir);
}

/* =============================================================================
 * JSON Serialization for WebUI
 * ============================================================================= */

json_object *config_to_json(const dawn_config_t *config) {
   if (!config)
      return NULL;

   json_object *root = json_object_new_object();
   if (!root)
      return NULL;

   /* [general] */
   json_object *general = json_object_new_object();
   json_object_object_add(general, "ai_name", json_object_new_string(config->general.ai_name));
   json_object_object_add(general, "log_file", json_object_new_string(config->general.log_file));
   json_object_object_add(general, "room", json_object_new_string(config->general.room));
   json_object_object_add(root, "general", general);

   /* [persona] */
   json_object *persona = json_object_new_object();
   json_object_object_add(persona, "description",
                          json_object_new_string(config->persona.description));
   json_object_object_add(root, "persona", persona);

   /* [localization] */
   json_object *localization = json_object_new_object();
   json_object_object_add(localization, "location",
                          json_object_new_string(config->localization.location));
   json_object_object_add(localization, "timezone",
                          json_object_new_string(config->localization.timezone));
   json_object_object_add(localization, "units",
                          json_object_new_string(config->localization.units));
   json_object_object_add(root, "localization", localization);

   /* [audio] */
   json_object *audio = json_object_new_object();
   json_object_object_add(audio, "backend", json_object_new_string(config->audio.backend));
   json_object_object_add(audio, "capture_device",
                          json_object_new_string(config->audio.capture_device));
   json_object_object_add(audio, "playback_device",
                          json_object_new_string(config->audio.playback_device));
   json_object_object_add(audio, "output_rate", json_object_new_int(config->audio.output_rate));
   json_object_object_add(audio, "output_channels",
                          json_object_new_int(config->audio.output_channels));

   /* [audio.bargein] */
   json_object *bargein = json_object_new_object();
   json_object_object_add(bargein, "enabled",
                          json_object_new_boolean(config->audio.bargein.enabled));
   json_object_object_add(bargein, "cooldown_ms",
                          json_object_new_int(config->audio.bargein.cooldown_ms));
   json_object_object_add(bargein, "startup_cooldown_ms",
                          json_object_new_int(config->audio.bargein.startup_cooldown_ms));
   json_object_object_add(audio, "bargein", bargein);
   json_object_object_add(root, "audio", audio);

   /* [vad] */
   json_object *vad = json_object_new_object();
   json_object_object_add(vad, "speech_threshold",
                          json_object_new_double(config->vad.speech_threshold));
   json_object_object_add(vad, "speech_threshold_tts",
                          json_object_new_double(config->vad.speech_threshold_tts));
   json_object_object_add(vad, "silence_threshold",
                          json_object_new_double(config->vad.silence_threshold));
   json_object_object_add(vad, "end_of_speech_duration",
                          json_object_new_double(config->vad.end_of_speech_duration));
   json_object_object_add(vad, "max_recording_duration",
                          json_object_new_double(config->vad.max_recording_duration));
   json_object_object_add(vad, "preroll_ms", json_object_new_int(config->vad.preroll_ms));

   /* [vad.chunking] */
   json_object *chunking = json_object_new_object();
   json_object_object_add(chunking, "enabled",
                          json_object_new_boolean(config->vad.chunking.enabled));
   json_object_object_add(chunking, "pause_duration",
                          json_object_new_double(config->vad.chunking.pause_duration));
   json_object_object_add(chunking, "min_duration",
                          json_object_new_double(config->vad.chunking.min_duration));
   json_object_object_add(chunking, "max_duration",
                          json_object_new_double(config->vad.chunking.max_duration));
   json_object_object_add(vad, "chunking", chunking);
   json_object_object_add(root, "vad", vad);

   /* [asr] */
   json_object *asr = json_object_new_object();
   json_object_object_add(asr, "model", json_object_new_string(config->asr.model));
   json_object_object_add(asr, "models_path", json_object_new_string(config->asr.models_path));
   json_object_object_add(asr, "dedup_window_sec",
                          json_object_new_int(config->asr.dedup_window_sec));
   json_object_object_add(root, "asr", asr);

   /* [tts] */
   json_object *tts = json_object_new_object();
   json_object_object_add(tts, "models_path", json_object_new_string(config->tts.models_path));
   json_object_object_add(tts, "voice_model", json_object_new_string(config->tts.voice_model));
   json_object_object_add(tts, "length_scale", json_object_new_double(config->tts.length_scale));
   json_object_object_add(root, "tts", tts);

   /* [commands] */
   json_object *commands = json_object_new_object();
   json_object_object_add(commands, "processing_mode",
                          json_object_new_string(config->commands.processing_mode));
   json_object_object_add(root, "commands", commands);

   /* [llm] */
   json_object *llm = json_object_new_object();
   json_object_object_add(llm, "type", json_object_new_string(config->llm.type));
   json_object_object_add(llm, "max_tokens", json_object_new_int(config->llm.max_tokens));

   /* [llm.cloud] */
   json_object *cloud = json_object_new_object();
   json_object_object_add(cloud, "provider", json_object_new_string(config->llm.cloud.provider));
   json_object_object_add(cloud, "endpoint", json_object_new_string(config->llm.cloud.endpoint));
   json_object_object_add(cloud, "vision_enabled",
                          json_object_new_boolean(config->llm.cloud.vision_enabled));
   json_object_object_add(cloud, "use_openrouter",
                          json_object_new_boolean(config->llm.cloud.use_openrouter));
   /* Key presence only (never the value) so the WebUI can warn when the gateway
    * is on but no key is configured. */
   json_object_object_add(cloud, "openrouter_key_present",
                          json_object_new_boolean(g_secrets.openrouter_api_key[0] != '\0'));

   /* Model lists for quick controls dropdown */
   json_object *openai_models = json_object_new_array();
   for (int i = 0; i < config->llm.cloud.openai_models_count; i++) {
      json_object_array_add(openai_models,
                            json_object_new_string(config->llm.cloud.openai_models[i]));
   }
   json_object_object_add(cloud, "openai_models", openai_models);
   json_object_object_add(cloud, "openai_default_model_idx",
                          json_object_new_int(config->llm.cloud.openai_default_model_idx));

   json_object *claude_models = json_object_new_array();
   for (int i = 0; i < config->llm.cloud.claude_models_count; i++) {
      json_object_array_add(claude_models,
                            json_object_new_string(config->llm.cloud.claude_models[i]));
   }
   json_object_object_add(cloud, "claude_models", claude_models);
   json_object_object_add(cloud, "claude_default_model_idx",
                          json_object_new_int(config->llm.cloud.claude_default_model_idx));

   json_object *gemini_models = json_object_new_array();
   for (int i = 0; i < config->llm.cloud.gemini_models_count; i++) {
      json_object_array_add(gemini_models,
                            json_object_new_string(config->llm.cloud.gemini_models[i]));
   }
   json_object_object_add(cloud, "gemini_models", gemini_models);
   json_object_object_add(cloud, "gemini_default_model_idx",
                          json_object_new_int(config->llm.cloud.gemini_default_model_idx));

   json_object *openrouter_models = json_object_new_array();
   for (int i = 0; i < config->llm.cloud.openrouter_models_count; i++) {
      json_object_array_add(openrouter_models,
                            json_object_new_string(config->llm.cloud.openrouter_models[i]));
   }
   json_object_object_add(cloud, "openrouter_models", openrouter_models);
   json_object_object_add(cloud, "openrouter_default_model_idx",
                          json_object_new_int(config->llm.cloud.openrouter_default_model_idx));

   json_object_object_add(llm, "cloud", cloud);

   /* [llm.local] */
   json_object *local = json_object_new_object();
   json_object_object_add(local, "endpoint", json_object_new_string(config->llm.local.endpoint));
   json_object_object_add(local, "model", json_object_new_string(config->llm.local.model));
   json_object_object_add(local, "vision_enabled",
                          json_object_new_boolean(config->llm.local.vision_enabled));
   json_object_object_add(llm, "local", local);

   /* [llm.tools] */
   json_object *tools = json_object_new_object();
   json_object_object_add(tools, "mode", json_object_new_string(config->llm.tools.mode));
   json_object_object_add(llm, "tools", tools);

   /* [llm.silent_observe] */
   json_object *silent_observe = json_object_new_object();
   json_object_object_add(silent_observe, "provider",
                          json_object_new_string(config->llm.silent_observe.provider));
   json_object_object_add(silent_observe, "model",
                          json_object_new_string(config->llm.silent_observe.model));
   json_object_object_add(silent_observe, "openrouter_model",
                          json_object_new_string(config->llm.silent_observe.openrouter_model));
   json_object_object_add(llm, "silent_observe", silent_observe);

   /* [llm.thinking] */
   json_object *thinking = json_object_new_object();
   json_object_object_add(thinking, "mode", json_object_new_string(config->llm.thinking.mode));
   json_object_object_add(thinking, "reasoning_effort",
                          json_object_new_string(config->llm.thinking.reasoning_effort));
   json_object_object_add(thinking, "budget_low",
                          json_object_new_int(config->llm.thinking.budget_low));
   json_object_object_add(thinking, "budget_medium",
                          json_object_new_int(config->llm.thinking.budget_medium));
   json_object_object_add(thinking, "budget_high",
                          json_object_new_int(config->llm.thinking.budget_high));
   json_object_object_add(thinking, "budget_xhigh",
                          json_object_new_int(config->llm.thinking.budget_xhigh));
   json_object_object_add(llm, "thinking", thinking);

   /* Context management settings */
   json_object_object_add(llm, "compact_soft_threshold",
                          json_object_new_double(config->llm.compact_soft_threshold));
   json_object_object_add(llm, "compact_hard_threshold",
                          json_object_new_double(config->llm.compact_hard_threshold));
   json_object_object_add(llm, "compact_use_session",
                          json_object_new_boolean(config->llm.compact_use_session));
   if (config->llm.compact_provider[0]) {
      json_object_object_add(llm, "compact_provider",
                             json_object_new_string(config->llm.compact_provider));
   }
   if (config->llm.compact_model[0]) {
      json_object_object_add(llm, "compact_model",
                             json_object_new_string(config->llm.compact_model));
   }
   json_object_object_add(llm, "compact_openrouter_model",
                          json_object_new_string(config->llm.compact_openrouter_model));
   json_object_object_add(llm, "conversation_logging",
                          json_object_new_boolean(config->llm.conversation_logging));
   json_object_object_add(llm, "rate_limit_enabled",
                          json_object_new_boolean(config->llm.rate_limit_enabled));
   json_object_object_add(llm, "rate_limit_rpm", json_object_new_int(config->llm.rate_limit_rpm));
   json_object_object_add(root, "llm", llm);

   /* [search] */
   json_object *search = json_object_new_object();
   json_object_object_add(search, "engine", json_object_new_string(config->search.engine));
   json_object_object_add(search, "endpoint", json_object_new_string(config->search.endpoint));

   /* [search.summarizer] */
   json_object *summarizer = json_object_new_object();
   json_object_object_add(summarizer, "backend",
                          json_object_new_string(config->search.summarizer.backend));
   json_object_object_add(summarizer, "threshold_bytes",
                          json_object_new_int64(
                              (int64_t)config->search.summarizer.threshold_bytes));
   json_object_object_add(summarizer, "target_words",
                          json_object_new_int64((int64_t)config->search.summarizer.target_words));
   json_object_object_add(summarizer, "target_ratio",
                          json_object_new_double(config->search.summarizer.target_ratio));
   json_object_object_add(search, "summarizer", summarizer);

   /* title_filters array */
   json_object *title_filters = json_object_new_array();
   for (int i = 0; i < config->search.title_filters_count; i++) {
      json_object_array_add(title_filters, json_object_new_string(config->search.title_filters[i]));
   }
   json_object_object_add(search, "title_filters", title_filters);
   json_object_object_add(root, "search", search);

   /* [mcp] + [[mcp.server]] (coding harness MCP bridge). Servers are serialized
    * read-only for visibility; they are configured via TOML, not the settings
    * panel, so the apply path (webui_config.c) only round-trips the scalars. */
   json_object *mcp = json_object_new_object();
   json_object_object_add(mcp, "enabled", json_object_new_boolean(config->mcp.enabled));
   json_object_object_add(mcp, "dev_mode", json_object_new_boolean(config->mcp.dev_mode));
   json_object *mcp_servers = json_object_new_array();
   for (int i = 0; i < config->mcp.server_count && i < MCP_SERVERS_MAX; i++) {
      const mcp_server_config_t *srv = &config->mcp.servers[i];
      json_object *s = json_object_new_object();
      json_object_object_add(s, "alias", json_object_new_string(srv->alias));
      json_object_object_add(s, "url", json_object_new_string(srv->url));
      json_object_object_add(s, "transport", json_object_new_string(srv->transport));
      json_object_object_add(s, "enabled", json_object_new_boolean(srv->enabled));
      json_object_object_add(s, "capabilities", json_object_new_string(srv->capabilities));
      json_object_object_add(s, "request_timeout_seconds",
                             json_object_new_int(srv->request_timeout_seconds));
      json_object_object_add(s, "idle_close_seconds", json_object_new_int(srv->idle_close_seconds));
      json_object_object_add(s, "tls_verify", json_object_new_boolean(srv->tls_verify));
      json_object_object_add(s, "auth_bearer_env", json_object_new_string(srv->auth_bearer_env));
      json_object_array_add(mcp_servers, s);
   }
   json_object_object_add(mcp, "servers", mcp_servers);
   json_object_object_add(root, "mcp", mcp);

   /* [code_projects] (coding harness imported repositories) */
   json_object *code_projects = json_object_new_object();
   json_object_object_add(code_projects, "enabled",
                          json_object_new_boolean(config->code_projects.enabled));
   json_object_object_add(code_projects, "source_root",
                          json_object_new_string(config->code_projects.source_root));
   json_object_object_add(code_projects, "default_index_mode",
                          json_object_new_string(config->code_projects.default_index_mode));
   json_object_object_add(code_projects, "default_global",
                          json_object_new_boolean(config->code_projects.default_global));
   json_object_object_add(code_projects, "import_user_required",
                          json_object_new_string(config->code_projects.import_user_required));
   json_object_object_add(code_projects, "max_repo_size_mb",
                          json_object_new_int(config->code_projects.max_repo_size_mb));
   json_object_object_add(code_projects, "max_file_count",
                          json_object_new_int(config->code_projects.max_file_count));
   json_object_object_add(code_projects, "max_path_depth",
                          json_object_new_int(config->code_projects.max_path_depth));
   json_object_object_add(code_projects, "clone_depth",
                          json_object_new_int(config->code_projects.clone_depth));
   json_object_object_add(code_projects, "allowed_host_pattern",
                          json_object_new_string(config->code_projects.allowed_host_pattern));
   json_object_object_add(code_projects, "default_active",
                          json_object_new_string(config->code_projects.default_active));
   json_object_object_add(root, "code_projects", code_projects);

   /* [url_fetcher] */
   json_object *url_fetcher = json_object_new_object();
   json_object_object_add(url_fetcher, "whitelist_count",
                          json_object_new_int(config->url_fetcher.whitelist_count));
   json_object_object_add(url_fetcher, "fallback",
                          json_object_new_string(config->url_fetcher.fallback));

   /* URL whitelist array */
   json_object *whitelist = json_object_new_array();
   for (int i = 0; i < config->url_fetcher.whitelist_count; i++) {
      json_object_array_add(whitelist, json_object_new_string(config->url_fetcher.whitelist[i]));
   }
   json_object_object_add(url_fetcher, "whitelist", whitelist);

   /* [url_fetcher.flaresolverr] */
   json_object *flaresolverr = json_object_new_object();
   json_object_object_add(flaresolverr, "enabled",
                          json_object_new_boolean(config->url_fetcher.flaresolverr.enabled));
   json_object_object_add(flaresolverr, "endpoint",
                          json_object_new_string(config->url_fetcher.flaresolverr.endpoint));
   json_object_object_add(flaresolverr, "timeout_sec",
                          json_object_new_int(config->url_fetcher.flaresolverr.timeout_sec));
   json_object_object_add(flaresolverr, "max_response_bytes",
                          json_object_new_int64(
                              (int64_t)config->url_fetcher.flaresolverr.max_response_bytes));
   json_object_object_add(url_fetcher, "flaresolverr", flaresolverr);

   /* [url_fetcher.tavily] */
   json_object *tavily_fetch = json_object_new_object();
   json_object_object_add(tavily_fetch, "timeout_sec",
                          json_object_new_int(config->url_fetcher.tavily.timeout_sec));
   json_object_object_add(tavily_fetch, "max_response_bytes",
                          json_object_new_int64(
                              (int64_t)config->url_fetcher.tavily.max_response_bytes));
   json_object_object_add(tavily_fetch, "extract_depth",
                          json_object_new_string(config->url_fetcher.tavily.extract_depth));
   json_object_object_add(tavily_fetch, "rate_limit_per_minute",
                          json_object_new_int(config->url_fetcher.tavily.rate_limit_per_minute));
   json_object_object_add(tavily_fetch, "rate_limit_per_hour",
                          json_object_new_int(config->url_fetcher.tavily.rate_limit_per_hour));
   json_object_object_add(tavily_fetch, "rate_limit_per_day",
                          json_object_new_int(config->url_fetcher.tavily.rate_limit_per_day));
   json_object_object_add(url_fetcher, "tavily", tavily_fetch);

   json_object_object_add(root, "url_fetcher", url_fetcher);

   /* [mqtt] */
   json_object *mqtt = json_object_new_object();
   json_object_object_add(mqtt, "enabled", json_object_new_boolean(config->mqtt.enabled));
   json_object_object_add(mqtt, "broker", json_object_new_string(config->mqtt.broker));
   json_object_object_add(mqtt, "port", json_object_new_int(config->mqtt.port));
   json_object_object_add(mqtt, "tls", json_object_new_boolean(config->mqtt.tls));
   json_object_object_add(mqtt, "tls_ca_cert", json_object_new_string(config->mqtt.tls_ca_cert));
   json_object_object_add(mqtt, "tls_cert_path",
                          json_object_new_string(config->mqtt.tls_cert_path));
   json_object_object_add(mqtt, "tls_key_path", json_object_new_string(config->mqtt.tls_key_path));
   json_object_object_add(root, "mqtt", mqtt);

   /* [network] */
   json_object *network = json_object_new_object();
   json_object_object_add(network, "workers", json_object_new_int(config->network.workers));
   json_object_object_add(network, "session_timeout_sec",
                          json_object_new_int(config->network.session_timeout_sec));
   json_object_object_add(network, "llm_timeout_ms",
                          json_object_new_int(config->network.llm_timeout_ms));
   json_object_object_add(network, "summarization_timeout_ms",
                          json_object_new_int(config->network.summarization_timeout_ms));
   json_object_object_add(root, "network", network);

   /* [tui] */
   json_object *tui = json_object_new_object();
   json_object_object_add(tui, "enabled", json_object_new_boolean(config->tui.enabled));
   json_object_object_add(root, "tui", tui);

   /* [webui] */
   json_object *webui = json_object_new_object();
   json_object_object_add(webui, "enabled", json_object_new_boolean(config->webui.enabled));
   json_object_object_add(webui, "port", json_object_new_int(config->webui.port));
   json_object_object_add(webui, "max_clients", json_object_new_int(config->webui.max_clients));
   json_object_object_add(webui, "audio_chunk_ms",
                          json_object_new_int(config->webui.audio_chunk_ms));
   json_object_object_add(webui, "www_path", json_object_new_string(config->webui.www_path));
   json_object_object_add(webui, "bind_address",
                          json_object_new_string(config->webui.bind_address));
   json_object_object_add(webui, "https", json_object_new_boolean(config->webui.https));
   json_object_object_add(webui, "ssl_cert_path",
                          json_object_new_string(config->webui.ssl_cert_path));
   json_object_object_add(webui, "ssl_key_path",
                          json_object_new_string(config->webui.ssl_key_path));
   json_object_object_add(webui, "export_max_messages",
                          json_object_new_int(config->webui.export_max_messages));
   json_object_object_add(webui, "export_format",
                          json_object_new_string(config->webui.export_format));
   json_object_object_add(root, "webui", webui);

   /* [memory] */
   json_object *memory = json_object_new_object();
   json_object_object_add(memory, "enabled", json_object_new_boolean(config->memory.enabled));
   json_object_object_add(memory, "context_budget_tokens",
                          json_object_new_int(config->memory.context_budget_tokens));
   json_object_object_add(memory, "source_budget_chars",
                          json_object_new_int(config->memory.source_budget_chars));
   json_object_object_add(memory, "extraction_provider",
                          json_object_new_string(config->memory.extraction_provider));
   json_object_object_add(memory, "extraction_model",
                          json_object_new_string(config->memory.extraction_model));
   json_object_object_add(memory, "extraction_openrouter_model",
                          json_object_new_string(config->memory.extraction_openrouter_model));
   json_object_object_add(memory, "extraction_timeout_ms",
                          json_object_new_int(config->memory.extraction_timeout_ms));
   json_object_object_add(memory, "paraphrase_dedup_enabled",
                          json_object_new_boolean(config->memory.paraphrase_dedup_enabled));
   json_object_object_add(memory, "paraphrase_dedup_threshold",
                          json_object_new_double(config->memory.paraphrase_dedup_threshold));
   json_object_object_add(memory, "note_extraction_guard",
                          json_object_new_boolean(config->memory.note_extraction_guard));
   json_object_object_add(memory, "pruning_enabled",
                          json_object_new_boolean(config->memory.pruning_enabled));
   json_object_object_add(memory, "prune_superseded_days",
                          json_object_new_int(config->memory.prune_superseded_days));
   json_object_object_add(memory, "prune_stale_days",
                          json_object_new_int(config->memory.prune_stale_days));
   json_object_object_add(memory, "prune_stale_min_confidence",
                          json_object_new_double(config->memory.prune_stale_min_confidence));
   json_object_object_add(memory, "expire_enabled",
                          json_object_new_boolean(config->memory.expire_enabled));
   json_object_object_add(memory, "expire_grace_days",
                          json_object_new_int(config->memory.expire_grace_days));
   json_object_object_add(memory, "prune_expired_days",
                          json_object_new_int(config->memory.prune_expired_days));
   json_object_object_add(memory, "conversation_idle_timeout_min",
                          json_object_new_int(config->memory.conversation_idle_timeout_min));
   json_object_object_add(memory, "default_voice_user_id",
                          json_object_new_int(config->memory.default_voice_user_id));
   json_object_object_add(memory, "decay_enabled",
                          json_object_new_boolean(config->memory.decay_enabled));
   json_object_object_add(memory, "decay_hour", json_object_new_int(config->memory.decay_hour));
   json_object_object_add(memory, "decay_inferred_weekly",
                          json_object_new_double(config->memory.decay_inferred_weekly));
   json_object_object_add(memory, "decay_explicit_weekly",
                          json_object_new_double(config->memory.decay_explicit_weekly));
   json_object_object_add(memory, "decay_preference_weekly",
                          json_object_new_double(config->memory.decay_preference_weekly));
   json_object_object_add(memory, "decay_inferred_floor",
                          json_object_new_double(config->memory.decay_inferred_floor));
   json_object_object_add(memory, "decay_explicit_floor",
                          json_object_new_double(config->memory.decay_explicit_floor));
   json_object_object_add(memory, "decay_preference_floor",
                          json_object_new_double(config->memory.decay_preference_floor));
   json_object_object_add(memory, "decay_prune_threshold",
                          json_object_new_double(config->memory.decay_prune_threshold));
   json_object_object_add(memory, "summary_retention_days",
                          json_object_new_int(config->memory.summary_retention_days));
   json_object_object_add(memory, "access_reinforcement_boost",
                          json_object_new_double(config->memory.access_reinforcement_boost));
   json_object_object_add(memory, "embedding_provider",
                          json_object_new_string(config->memory.embedding_provider));
   json_object_object_add(memory, "embedding_model",
                          json_object_new_string(config->memory.embedding_model));
   json_object_object_add(memory, "embedding_endpoint",
                          json_object_new_string(config->memory.embedding_endpoint));
   json_object_object_add(memory, "embedding_keyword_weight",
                          json_object_new_double(config->memory.embedding_keyword_weight));
   json_object_object_add(memory, "embedding_vector_weight",
                          json_object_new_double(config->memory.embedding_vector_weight));
   json_object_object_add(memory, "temporal_weight",
                          json_object_new_double(config->memory.temporal_weight));
   json_object_object_add(memory, "temporal_filter_enabled",
                          json_object_new_boolean(config->memory.temporal_filter_enabled));
   json_object_object_add(memory, "rrf_enabled",
                          json_object_new_boolean(config->memory.rrf_enabled));
   json_object_object_add(memory, "memory_text_minimal",
                          json_object_new_boolean(config->memory.memory_text_minimal));
   json_object_object_add(memory, "bm25_enabled",
                          json_object_new_boolean(config->memory.bm25_enabled));
   json_object_object_add(memory, "category_threshold",
                          json_object_new_double(config->memory.category_threshold));
   json_object_object_add(memory, "search_score_floor",
                          json_object_new_double(config->memory.search_score_floor));
   {
      struct json_object *graph = json_object_new_object();
      json_object_object_add(graph, "enabled",
                             json_object_new_boolean(config->memory.graph_retrieval.enabled));
      json_object_object_add(graph, "entity_grounding_bonus",
                             json_object_new_double(
                                 config->memory.graph_retrieval.entity_grounding_bonus));
      json_object_object_add(graph, "max_facts_per_query",
                             json_object_new_int(
                                 config->memory.graph_retrieval.max_facts_per_query));
      json_object_object_add(graph, "use_query_scoring",
                             json_object_new_boolean(
                                 config->memory.graph_retrieval.use_query_scoring));
      json_object_object_add(graph, "entity_bonus",
                             json_object_new_double(config->memory.graph_retrieval.entity_bonus));
      json_object_object_add(memory, "graph_retrieval", graph);
   }
   json_object_object_add(memory, "embedding_backfill_on_startup",
                          json_object_new_boolean(config->memory.embedding_backfill_on_startup));
   json_object_object_add(memory, "model_id", json_object_new_string(config->memory.model_id));
   json_object_object_add(memory, "recompute_on_model_change",
                          json_object_new_boolean(config->memory.recompute_on_model_change));
   json_object_object_add(memory, "recompute_batch_size",
                          json_object_new_int(config->memory.recompute_batch_size));
   json_object_object_add(memory, "recompute_batch_sleep_ms",
                          json_object_new_int(config->memory.recompute_batch_sleep_ms));
   json_object_object_add(memory, "recovery_enabled",
                          json_object_new_boolean(config->memory.recovery_enabled));
   json_object_object_add(memory, "recovery_idle_threshold_seconds",
                          json_object_new_int(config->memory.recovery_idle_threshold_seconds));
   json_object_object_add(memory, "recovery_max_attempts",
                          json_object_new_int(config->memory.recovery_max_attempts));
   json_object_object_add(memory, "recovery_recurring_interval_seconds",
                          json_object_new_int(config->memory.recovery_recurring_interval_seconds));

   /* [memory.focus_injection] — Phase 1 dynamic context injection */
   {
      const focus_injection_config_t *fi = &config->memory.focus_injection;
      json_object *focus = json_object_new_object();
      json_object_object_add(focus, "enabled", json_object_new_boolean(fi->enabled));
      json_object_object_add(focus, "focus_budget_bytes",
                             json_object_new_int(fi->focus_budget_bytes));
      json_object_object_add(focus, "top_k", json_object_new_int(fi->top_k));
      json_object_object_add(focus, "summary_max_scan", json_object_new_int(fi->summary_max_scan));
      json_object_object_add(focus, "min_score", json_object_new_double(fi->min_score));
      json_object_object_add(focus, "classifier_enabled",
                             json_object_new_boolean(fi->classifier_enabled));
      json_object_object_add(focus, "weight_semantic", json_object_new_double(fi->weight_semantic));
      json_object_object_add(focus, "weight_recency", json_object_new_double(fi->weight_recency));
      json_object_object_add(focus, "weight_importance",
                             json_object_new_double(fi->weight_importance));
      json_object_object_add(focus, "weight_source", json_object_new_double(fi->weight_source));

      json_object *src = json_object_new_object();
      json_object_object_add(src, "memory_fact",
                             json_object_new_double(fi->source_weights.memory_fact));
      json_object_object_add(src, "memory_entity",
                             json_object_new_double(fi->source_weights.memory_entity));
      json_object_object_add(src, "memory_relation",
                             json_object_new_double(fi->source_weights.memory_relation));
      json_object_object_add(src, "memory_summary",
                             json_object_new_double(fi->source_weights.memory_summary));
      json_object_object_add(src, "document_chunk",
                             json_object_new_double(fi->source_weights.document_chunk));
      json_object_object_add(src, "calendar_event",
                             json_object_new_double(fi->source_weights.calendar_event));
      json_object_object_add(src, "recent_email",
                             json_object_new_double(fi->source_weights.recent_email));
      json_object_object_add(src, "dawn_background",
                             json_object_new_double(fi->source_weights.dawn_background));
      json_object_object_add(focus, "source_weights", src);

      json_object *dedup = json_object_new_object();
      json_object_object_add(dedup, "recent_window_turns",
                             json_object_new_int(fi->dedup.recent_window_turns));
      json_object_object_add(dedup, "score_uplift_factor",
                             json_object_new_double(fi->dedup.score_uplift_factor));
      json_object_object_add(focus, "dedup", dedup);

      json_object *dth = json_object_new_object();
      json_object_object_add(dth, "enabled",
                             json_object_new_boolean(fi->dominant_token_heuristic.enabled));
      json_object_object_add(dth, "threshold",
                             json_object_new_double(fi->dominant_token_heuristic.threshold));
      json_object_object_add(dth, "base_penalty",
                             json_object_new_double(fi->dominant_token_heuristic.base_penalty));
      json_object_object_add(focus, "dominant_token_heuristic", dth);

      json_object_object_add(memory, "focus_injection", focus);
   }

   /* [memory.entity_merge] — Phase 2 auto-merge gate */
   {
      json_object *emerge = json_object_new_object();
      json_object_object_add(emerge, "enabled",
                             json_object_new_boolean(config->memory.entity_merge_enabled));
      json_object_object_add(emerge, "auto_threshold",
                             json_object_new_double(config->memory.entity_merge_auto_threshold));
      json_object_object_add(emerge, "review_threshold",
                             json_object_new_double(config->memory.entity_merge_review_threshold));
      json_object_object_add(memory, "entity_merge", emerge);
   }

   json_object_object_add(root, "memory", memory);

   /* [shutdown] */
   json_object *shutdown = json_object_new_object();
   json_object_object_add(shutdown, "enabled", json_object_new_boolean(config->shutdown.enabled));
   json_object_object_add(shutdown, "passphrase",
                          json_object_new_string(config->shutdown.passphrase));
   json_object_object_add(root, "shutdown", shutdown);

   /* [debug] */
   json_object *debug = json_object_new_object();
   json_object_object_add(debug, "mic_record", json_object_new_boolean(config->debug.mic_record));
   json_object_object_add(debug, "asr_record", json_object_new_boolean(config->debug.asr_record));
   json_object_object_add(debug, "aec_record", json_object_new_boolean(config->debug.aec_record));
   json_object_object_add(debug, "record_path", json_object_new_string(config->debug.record_path));
   json_object_object_add(debug, "silent_observe_test_endpoint",
                          json_object_new_boolean(config->debug.silent_observe_test_endpoint));
   json_object_object_add(root, "debug", debug);

   /* [paths] */
   json_object *paths = json_object_new_object();
   json_object_object_add(paths, "data_dir", json_object_new_string(config->paths.data_dir));
   json_object_object_add(paths, "music_dir", json_object_new_string(config->paths.music_dir));
   json_object_object_add(root, "paths", paths);

   /* [images] */
   json_object *images = json_object_new_object();
   json_object_object_add(images, "retention_days",
                          json_object_new_int(config->images.retention_days));
   json_object_object_add(images, "max_size_mb", json_object_new_int(config->images.max_size_mb));
   json_object_object_add(images, "max_per_user", json_object_new_int(config->images.max_per_user));
   json_object_object_add(images, "cache_size_mb",
                          json_object_new_int(config->images.cache_size_mb));
   json_object_object_add(root, "images", images);

   /* [documents] */
   json_object *documents = json_object_new_object();
   json_object_object_add(documents, "max_file_size_kb",
                          json_object_new_int(config->documents.max_file_size_kb));
   json_object_object_add(documents, "max_documents",
                          json_object_new_int(config->documents.max_documents));
   json_object_object_add(documents, "max_pages", json_object_new_int(config->documents.max_pages));
   json_object_object_add(documents, "max_extracted_size_kb",
                          json_object_new_int(config->documents.max_extracted_size_kb));
   json_object_object_add(documents, "max_index_size_kb",
                          json_object_new_int(config->documents.max_index_size_kb));
   json_object_object_add(documents, "max_indexed_documents",
                          json_object_new_int(config->documents.max_indexed_documents));
   /* Hybrid-search tuning weights (v61) */
   json_object_object_add(documents, "fts_label_weight",
                          json_object_new_double(config->documents.fts_label_weight));
   json_object_object_add(documents, "fts_body_weight",
                          json_object_new_double(config->documents.fts_body_weight));
   json_object_object_add(documents, "hybrid_keyword_weight",
                          json_object_new_double(config->documents.hybrid_keyword_weight));
   json_object_object_add(documents, "hybrid_vector_weight",
                          json_object_new_double(config->documents.hybrid_vector_weight));
   json_object_object_add(documents, "phrase_bonus_weight",
                          json_object_new_double(config->documents.phrase_bonus_weight));
   json_object_object_add(documents, "search_min_score",
                          json_object_new_double(config->documents.search_min_score));
   json_object_object_add(documents, "version_retention_days",
                          json_object_new_int(config->documents.version_retention_days));
   json_object_object_add(documents, "version_keep_per_doc",
                          json_object_new_int(config->documents.version_keep_per_doc));
   json_object_object_add(root, "documents", documents);

   /* [vision] - per-upload image size and dimension limits */
   json_object *vision = json_object_new_object();
   json_object_object_add(vision, "max_image_size_kb",
                          json_object_new_int(config->vision.max_image_size_kb));
   json_object_object_add(vision, "max_dimension",
                          json_object_new_int(config->vision.max_dimension));
   json_object_object_add(vision, "max_images", json_object_new_int(config->vision.max_images));
   json_object_object_add(root, "vision", vision);

   /* [scheduler] */
   json_object *scheduler = json_object_new_object();
   json_object_object_add(scheduler, "enabled", json_object_new_boolean(config->scheduler.enabled));
   json_object_object_add(scheduler, "default_snooze_minutes",
                          json_object_new_int(config->scheduler.default_snooze_minutes));
   json_object_object_add(scheduler, "max_snooze_count",
                          json_object_new_int(config->scheduler.max_snooze_count));
   json_object_object_add(scheduler, "max_events_per_user",
                          json_object_new_int(config->scheduler.max_events_per_user));
   json_object_object_add(scheduler, "max_events_total",
                          json_object_new_int(config->scheduler.max_events_total));
   json_object_object_add(scheduler, "missed_event_recovery",
                          json_object_new_boolean(config->scheduler.missed_event_recovery));
   json_object_object_add(scheduler, "missed_task_policy",
                          json_object_new_string(config->scheduler.missed_task_policy));
   json_object_object_add(scheduler, "missed_task_max_age_sec",
                          json_object_new_int(config->scheduler.missed_task_max_age_sec));
   json_object_object_add(scheduler, "alarm_timeout_sec",
                          json_object_new_int(config->scheduler.alarm_timeout_sec));
   json_object_object_add(scheduler, "alarm_volume",
                          json_object_new_int(config->scheduler.alarm_volume));
   json_object_object_add(scheduler, "event_retention_days",
                          json_object_new_int(config->scheduler.event_retention_days));
   json_object_object_add(scheduler, "briefing_speak_aloud_on_webui_source",
                          json_object_new_boolean(
                              config->scheduler.briefing_speak_aloud_on_webui_source));
   json_object_object_add(root, "scheduler", scheduler);

   /* [calendar] */
   json_object *calendar = json_object_new_object();
   json_object_object_add(calendar, "enabled", json_object_new_boolean(config->calendar.enabled));
   json_object_object_add(calendar, "sync_interval_sec",
                          json_object_new_int(config->calendar.sync_interval_sec));
   json_object_object_add(calendar, "cache_past_days",
                          json_object_new_int(config->calendar.cache_past_days));
   json_object_object_add(calendar, "cache_future_days",
                          json_object_new_int(config->calendar.cache_future_days));
   json_object_object_add(calendar, "default_event_duration_min",
                          json_object_new_int(config->calendar.default_event_duration_min));
   json_object_object_add(root, "calendar", calendar);

   /* [messaging] — parent object with nested sms child (fixes the pre-existing gap
    * where sms_active_window_sec was parsed but never re-serialized). */
   json_object *messaging = json_object_new_object();
   json_object *messaging_sms = json_object_new_object();
   json_object_object_add(messaging_sms, "active_window_sec",
                          json_object_new_int(config->messaging.sms_active_window_sec));
   json_object_object_add(messaging, "sms", messaging_sms);
   json_object_object_add(root, "messaging", messaging);

   /* Music configuration */
   json_object *music = json_object_new_object();
   json_object_object_add(music, "scan_interval_minutes",
                          json_object_new_int(config->music.scan_interval_minutes));

   /* music.plex */
   json_object *music_plex = json_object_new_object();
   json_object_object_add(music_plex, "host", json_object_new_string(config->music.plex.host));
   json_object_object_add(music_plex, "port", json_object_new_int(config->music.plex.port));
   json_object_object_add(music_plex, "music_section_id",
                          json_object_new_int(config->music.plex.music_section_id));
   json_object_object_add(music_plex, "ssl", json_object_new_boolean(config->music.plex.ssl));
   json_object_object_add(music_plex, "ssl_verify",
                          json_object_new_boolean(config->music.plex.ssl_verify));
   json_object_object_add(music, "plex", music_plex);

   /* music.streaming */
   json_object *music_streaming = json_object_new_object();
   json_object_object_add(music_streaming, "enabled",
                          json_object_new_boolean(config->music.streaming_enabled));
   json_object_object_add(music_streaming, "default_quality",
                          json_object_new_string(config->music.streaming_quality));
   json_object_object_add(music_streaming, "bitrate_mode",
                          json_object_new_string(config->music.streaming_bitrate_mode));
   json_object_object_add(music, "streaming", music_streaming);
   json_object_object_add(root, "music", music);

   return root;
}

json_object *secrets_to_json_status(const secrets_config_t *secrets) {
   json_object *obj = json_object_new_object();
   if (!obj)
      return NULL;

   /* Only report whether secrets are set, never the actual values */
   json_object_object_add(obj, "openai_api_key",
                          json_object_new_boolean(secrets && secrets->openai_api_key[0]));
   json_object_object_add(obj, "claude_api_key",
                          json_object_new_boolean(secrets && secrets->claude_api_key[0]));
   json_object_object_add(obj, "gemini_api_key",
                          json_object_new_boolean(secrets && secrets->gemini_api_key[0]));
   json_object_object_add(obj, "openrouter_api_key",
                          json_object_new_boolean(secrets && secrets->openrouter_api_key[0]));
   json_object_object_add(obj, "mqtt_username",
                          json_object_new_boolean(secrets && secrets->mqtt_username[0]));
   json_object_object_add(obj, "mqtt_password",
                          json_object_new_boolean(secrets && secrets->mqtt_password[0]));
   json_object_object_add(obj, "satellite_registration_key",
                          json_object_new_boolean(secrets &&
                                                  secrets->satellite_registration_key[0]));
   json_object_object_add(obj, "plex_token",
                          json_object_new_boolean(secrets && secrets->plex_token[0]));
   json_object_object_add(obj, "embedding_api_key",
                          json_object_new_boolean(secrets && secrets->embedding_api_key[0]));
   json_object_object_add(obj, "home_assistant_token",
                          json_object_new_boolean(secrets && secrets->home_assistant_token[0]));
   json_object_object_add(obj, "google_client_id",
                          json_object_new_boolean(secrets && secrets->google_client_id[0]));
   json_object_object_add(obj, "google_client_secret",
                          json_object_new_boolean(secrets && secrets->google_client_secret[0]));
   json_object_object_add(obj, "google_redirect_url",
                          json_object_new_boolean(secrets && secrets->google_redirect_url[0]));
   json_object_object_add(obj, "tavily_api_key",
                          json_object_new_boolean(secrets && secrets->tavily_api_key[0]));
   json_object_object_add(obj, "telegram_bot_token",
                          json_object_new_boolean(secrets && secrets->telegram_bot_token[0]));
   json_object_object_add(obj, "discord_bot_token",
                          json_object_new_boolean(secrets && secrets->discord_bot_token[0]));
   json_object_object_add(obj, "slack_app_token",
                          json_object_new_boolean(secrets && secrets->slack_app_token[0]));
   json_object_object_add(obj, "slack_bot_token",
                          json_object_new_boolean(secrets && secrets->slack_bot_token[0]));

   return obj;
}

/* =============================================================================
 * TOML File Writing
 * ============================================================================= */

/**
 * @brief Escape a string for TOML basic string format.
 *
 * Escapes backslashes, quotes, and control characters.
 * The caller must free the returned string.
 *
 * @param str Input string to escape
 * @return Newly allocated escaped string, or NULL on error
 */
static char *toml_escape_string(const char *str) {
   if (!str)
      return strdup("");

   /* Calculate worst-case output size (each char becomes 2 chars + null) */
   size_t len = strlen(str);
   size_t max_size = len * 2 + 1;

   char *out = malloc(max_size);
   if (!out)
      return NULL;

   char *dst = out;
   for (const char *src = str; *src; src++) {
      switch (*src) {
         case '\\':
            *dst++ = '\\';
            *dst++ = '\\';
            break;
         case '"':
            *dst++ = '\\';
            *dst++ = '"';
            break;
         case '\n':
            *dst++ = '\\';
            *dst++ = 'n';
            break;
         case '\r':
            *dst++ = '\\';
            *dst++ = 'r';
            break;
         case '\t':
            *dst++ = '\\';
            *dst++ = 't';
            break;
         default:
            *dst++ = *src;
            break;
      }
   }
   *dst = '\0';
   return out;
}

/**
 * @brief Write an escaped TOML string to file.
 *
 * @param fp File pointer
 * @param key TOML key name
 * @param value String value to escape and write
 */
static void write_toml_string(FILE *fp, const char *key, const char *value) {
   char *escaped = toml_escape_string(value);
   if (escaped) {
      fprintf(fp, "%s = \"%s\"\n", key, escaped);
      free(escaped);
   } else {
      /* Fallback if allocation fails */
      fprintf(fp, "%s = \"%s\"\n", key, value);
   }
}

int config_write_toml(const dawn_config_t *config, const char *path) {
   if (!config || !path)
      return 1;

   FILE *fp = fopen(path, "w");
   if (!fp) {
      OLOG_ERROR("Failed to open config file for writing: %s (%s)", path, strerror(errno));
      return 1;
   }

   fprintf(fp, "# DAWN Configuration\n");
   fprintf(fp, "# Auto-generated by WebUI settings panel\n\n");

   fprintf(fp, "[general]\n");
   write_toml_string(fp, "ai_name", config->general.ai_name);
   if (config->general.log_file[0])
      write_toml_string(fp, "log_file", config->general.log_file);
   if (config->general.room[0])
      write_toml_string(fp, "room", config->general.room);

   if (config->persona.description[0]) {
      fprintf(fp, "\n[persona]\n");
      /* For multiline strings, use triple quotes in TOML (literal string) */
      if (strchr(config->persona.description, '\n')) {
         fprintf(fp, "description = '''\n%s\n'''\n", config->persona.description);
      } else {
         write_toml_string(fp, "description", config->persona.description);
      }
   }

   fprintf(fp, "\n[localization]\n");
   if (config->localization.location[0])
      write_toml_string(fp, "location", config->localization.location);
   if (config->localization.timezone[0])
      write_toml_string(fp, "timezone", config->localization.timezone);
   fprintf(fp, "units = \"%s\"\n", config->localization.units);

   fprintf(fp, "\n[audio]\n");
   fprintf(fp, "backend = \"%s\"\n", config->audio.backend);
   write_toml_string(fp, "capture_device", config->audio.capture_device);
   write_toml_string(fp, "playback_device", config->audio.playback_device);
   fprintf(fp, "output_rate = %d\n", config->audio.output_rate);
   fprintf(fp, "output_channels = %d\n", config->audio.output_channels);

   fprintf(fp, "\n[audio.bargein]\n");
   fprintf(fp, "enabled = %s\n", config->audio.bargein.enabled ? "true" : "false");
   fprintf(fp, "cooldown_ms = %d\n", config->audio.bargein.cooldown_ms);
   fprintf(fp, "startup_cooldown_ms = %d\n", config->audio.bargein.startup_cooldown_ms);

   fprintf(fp, "\n[vad]\n");
   fprintf(fp, "speech_threshold = %.2f\n", config->vad.speech_threshold);
   fprintf(fp, "speech_threshold_tts = %.2f\n", config->vad.speech_threshold_tts);
   fprintf(fp, "silence_threshold = %.2f\n", config->vad.silence_threshold);
   fprintf(fp, "end_of_speech_duration = %.1f\n", config->vad.end_of_speech_duration);
   fprintf(fp, "max_recording_duration = %.1f\n", config->vad.max_recording_duration);
   fprintf(fp, "preroll_ms = %d\n", config->vad.preroll_ms);

   fprintf(fp, "\n[vad.chunking]\n");
   fprintf(fp, "enabled = %s\n", config->vad.chunking.enabled ? "true" : "false");
   fprintf(fp, "pause_duration = %.2f\n", config->vad.chunking.pause_duration);
   fprintf(fp, "min_chunk_duration = %.1f\n", config->vad.chunking.min_duration);
   fprintf(fp, "max_chunk_duration = %.1f\n", config->vad.chunking.max_duration);

   fprintf(fp, "\n[asr]\n");
   fprintf(fp, "model = \"%s\"\n", config->asr.model);
   fprintf(fp, "models_path = \"%s\"\n", config->asr.models_path);
   fprintf(fp, "dedup_window_sec = %d\n", config->asr.dedup_window_sec);

   fprintf(fp, "\n[tts]\n");
   fprintf(fp, "models_path = \"%s\"\n", config->tts.models_path);
   fprintf(fp, "voice_model = \"%s\"\n", config->tts.voice_model);
   fprintf(fp, "length_scale = %.2f\n", config->tts.length_scale);

   fprintf(fp, "\n[commands]\n");
   fprintf(fp, "processing_mode = \"%s\"\n", config->commands.processing_mode);

   fprintf(fp, "\n[llm]\n");
   fprintf(fp, "type = \"%s\"\n", config->llm.type);
   fprintf(fp, "max_tokens = %d\n", config->llm.max_tokens);
   fprintf(fp, "compact_soft_threshold = %.2f\n", config->llm.compact_soft_threshold);
   fprintf(fp, "compact_hard_threshold = %.2f\n", config->llm.compact_hard_threshold);
   fprintf(fp, "compact_use_session = %s\n", config->llm.compact_use_session ? "true" : "false");
   if (config->llm.compact_provider[0])
      fprintf(fp, "compact_provider = \"%s\"\n", config->llm.compact_provider);
   if (config->llm.compact_model[0])
      fprintf(fp, "compact_model = \"%s\"\n", config->llm.compact_model);
   if (config->llm.compact_openrouter_model[0])
      fprintf(fp, "compact_openrouter_model = \"%s\"\n", config->llm.compact_openrouter_model);
   fprintf(fp, "conversation_logging = %s\n", config->llm.conversation_logging ? "true" : "false");

   fprintf(fp, "\n[llm.cloud]\n");
   fprintf(fp, "provider = \"%s\"\n", config->llm.cloud.provider);
   if (config->llm.cloud.endpoint[0])
      fprintf(fp, "endpoint = \"%s\"\n", config->llm.cloud.endpoint);
   fprintf(fp, "vision_enabled = %s\n", config->llm.cloud.vision_enabled ? "true" : "false");
   fprintf(fp, "use_openrouter = %s\n", config->llm.cloud.use_openrouter ? "true" : "false");

   /* Helper macro for writing model arrays with proper escaping.
    * Model names are expected to be ASCII alphanumeric (e.g., "gpt-4o", "gemini-2.5-flash"),
    * so escape failures are unlikely. We log a warning but continue with unescaped value
    * to avoid breaking config save for the entire file. */
#define WRITE_MODEL_ARRAY(array_name, idx_key, array, count, idx_var)                  \
   do {                                                                                \
      if ((count) > 0) {                                                               \
         fprintf(fp, "%s = [\n", array_name);                                          \
         for (int i = 0; i < (count); i++) {                                           \
            char *escaped = toml_escape_string((array)[i]);                            \
            if (!escaped) {                                                            \
               OLOG_WARNING("Failed to escape model name '%s', using unescaped value", \
                            (array)[i]);                                               \
            }                                                                          \
            fprintf(fp, "    \"%s\"%s\n", escaped ? escaped : (array)[i],              \
                    i < (count)-1 ? "," : "");                                         \
            free(escaped);                                                             \
         }                                                                             \
         fprintf(fp, "]\n");                                                           \
      }                                                                                \
      fprintf(fp, "%s = %d\n", idx_key, idx_var);                                      \
   } while (0)

   WRITE_MODEL_ARRAY("openai_models", "openai_default_model_idx", config->llm.cloud.openai_models,
                     config->llm.cloud.openai_models_count,
                     config->llm.cloud.openai_default_model_idx);
   WRITE_MODEL_ARRAY("claude_models", "claude_default_model_idx", config->llm.cloud.claude_models,
                     config->llm.cloud.claude_models_count,
                     config->llm.cloud.claude_default_model_idx);
   WRITE_MODEL_ARRAY("gemini_models", "gemini_default_model_idx", config->llm.cloud.gemini_models,
                     config->llm.cloud.gemini_models_count,
                     config->llm.cloud.gemini_default_model_idx);
   WRITE_MODEL_ARRAY("openrouter_models", "openrouter_default_model_idx",
                     config->llm.cloud.openrouter_models, config->llm.cloud.openrouter_models_count,
                     config->llm.cloud.openrouter_default_model_idx);

#undef WRITE_MODEL_ARRAY

   fprintf(fp, "\n[llm.local]\n");
   fprintf(fp, "endpoint = \"%s\"\n", config->llm.local.endpoint);
   if (config->llm.local.model[0])
      fprintf(fp, "model = \"%s\"\n", config->llm.local.model);
   fprintf(fp, "vision_enabled = %s\n", config->llm.local.vision_enabled ? "true" : "false");

   fprintf(fp, "\n[llm.tools]\n");
   fprintf(fp, "mode = \"%s\"\n", config->llm.tools.mode);
   /* Write local_enabled array if configured (even if empty - empty means none enabled) */
   if (config->llm.tools.local_enabled_configured || config->llm.tools.local_enabled_count > 0) {
      if (config->llm.tools.local_enabled_count > 0) {
         fprintf(fp, "local_enabled = [\n");
         for (int i = 0; i < config->llm.tools.local_enabled_count; i++) {
            /* Defense-in-depth: escape even though input validation restricts characters */
            char *escaped = toml_escape_string(config->llm.tools.local_enabled[i]);
            fprintf(fp, "    \"%s\"%s\n", escaped ? escaped : config->llm.tools.local_enabled[i],
                    i < config->llm.tools.local_enabled_count - 1 ? "," : "");
            free(escaped);
         }
         fprintf(fp, "]\n");
      } else {
         fprintf(fp, "local_enabled = []\n");
      }
   }
   /* Write remote_enabled array if configured (even if empty - empty means none enabled) */
   if (config->llm.tools.remote_enabled_configured || config->llm.tools.remote_enabled_count > 0) {
      if (config->llm.tools.remote_enabled_count > 0) {
         fprintf(fp, "remote_enabled = [\n");
         for (int i = 0; i < config->llm.tools.remote_enabled_count; i++) {
            /* Defense-in-depth: escape even though input validation restricts characters */
            char *escaped = toml_escape_string(config->llm.tools.remote_enabled[i]);
            fprintf(fp, "    \"%s\"%s\n", escaped ? escaped : config->llm.tools.remote_enabled[i],
                    i < config->llm.tools.remote_enabled_count - 1 ? "," : "");
            free(escaped);
         }
         fprintf(fp, "]\n");
      } else {
         fprintf(fp, "remote_enabled = []\n");
      }
   }
   /* Write local_disabled array (blocklist) if configured */
   if (config->llm.tools.local_disabled_configured) {
      if (config->llm.tools.local_disabled_count > 0) {
         fprintf(fp, "local_disabled = [\n");
         for (int i = 0; i < config->llm.tools.local_disabled_count; i++) {
            char *escaped = toml_escape_string(config->llm.tools.local_disabled[i]);
            fprintf(fp, "    \"%s\"%s\n", escaped ? escaped : config->llm.tools.local_disabled[i],
                    i < config->llm.tools.local_disabled_count - 1 ? "," : "");
            free(escaped);
         }
         fprintf(fp, "]\n");
      } else {
         fprintf(fp, "local_disabled = []\n");
      }
   }
   /* Write remote_disabled array (blocklist) if configured */
   if (config->llm.tools.remote_disabled_configured) {
      if (config->llm.tools.remote_disabled_count > 0) {
         fprintf(fp, "remote_disabled = [\n");
         for (int i = 0; i < config->llm.tools.remote_disabled_count; i++) {
            char *escaped = toml_escape_string(config->llm.tools.remote_disabled[i]);
            fprintf(fp, "    \"%s\"%s\n", escaped ? escaped : config->llm.tools.remote_disabled[i],
                    i < config->llm.tools.remote_disabled_count - 1 ? "," : "");
            free(escaped);
         }
         fprintf(fp, "]\n");
      } else {
         fprintf(fp, "remote_disabled = []\n");
      }
   }

   fprintf(fp, "\n[llm.thinking]\n");
   fprintf(fp, "mode = \"%s\"\n", config->llm.thinking.mode);
   fprintf(fp, "reasoning_effort = \"%s\"\n", config->llm.thinking.reasoning_effort);
   fprintf(fp, "budget_low = %d\n", config->llm.thinking.budget_low);
   fprintf(fp, "budget_medium = %d\n", config->llm.thinking.budget_medium);
   fprintf(fp, "budget_high = %d\n", config->llm.thinking.budget_high);
   fprintf(fp, "budget_xhigh = %d\n", config->llm.thinking.budget_xhigh);

   fprintf(fp, "\n[llm.silent_observe]\n");
   fprintf(fp, "provider = \"%s\"\n", config->llm.silent_observe.provider);
   fprintf(fp, "model = \"%s\"\n", config->llm.silent_observe.model);
   fprintf(fp, "openrouter_model = \"%s\"\n", config->llm.silent_observe.openrouter_model);

   fprintf(fp, "\n[search]\n");
   fprintf(fp, "engine = \"%s\"\n", config->search.engine);
   fprintf(fp, "endpoint = \"%s\"\n", config->search.endpoint);

   /* Write title_filters under [search] BEFORE [search.summarizer] so TOML
    * attaches the array to the right table.  Any key emitted after a sub-
    * table header binds to that sub-table (TOML §3.3); writing this block
    * after [search.summarizer] silently produced [search.summarizer]
    * .title_filters, which the parser doesn't recognize. */
   if (config->search.title_filters_count > 0) {
      fprintf(fp, "\n# Exclude search results with these terms in title (case-insensitive)\n");
      fprintf(fp, "title_filters = [\n");
      for (int i = 0; i < config->search.title_filters_count; i++) {
         char *escaped = toml_escape_string(config->search.title_filters[i]);
         fprintf(fp, "    \"%s\"%s\n", escaped ? escaped : config->search.title_filters[i],
                 i < config->search.title_filters_count - 1 ? "," : "");
         free(escaped);
      }
      fprintf(fp, "]\n");
   }

   fprintf(fp, "\n[search.summarizer]\n");
   fprintf(fp, "backend = \"%s\"\n", config->search.summarizer.backend);
   fprintf(fp, "threshold_bytes = %zu\n", config->search.summarizer.threshold_bytes);
   fprintf(fp, "target_words = %zu\n", config->search.summarizer.target_words);
   fprintf(fp, "target_ratio = %.2f\n", config->search.summarizer.target_ratio);

   /* Always emit the [url_fetcher] header if any sub-section has non-default
    * content. The `fallback` field is operator-controllable so we always
    * write it once we've moved off the default — otherwise a WebUI change
    * to fallback would silently never persist. */
   bool emit_url_fetcher = config->url_fetcher.whitelist_count > 0 ||
                           config->url_fetcher.flaresolverr.enabled ||
                           (config->url_fetcher.fallback[0] != '\0' &&
                            strcmp(config->url_fetcher.fallback, "flaresolverr") != 0);
   if (emit_url_fetcher) {
      fprintf(fp, "\n[url_fetcher]\n");
      if (config->url_fetcher.fallback[0] != '\0') {
         fprintf(fp, "fallback = \"%s\"\n", config->url_fetcher.fallback);
      }
      if (config->url_fetcher.whitelist_count > 0) {
         fprintf(fp, "whitelist = [\n");
         for (int i = 0; i < config->url_fetcher.whitelist_count; i++) {
            char *escaped = toml_escape_string(config->url_fetcher.whitelist[i]);
            fprintf(fp, "    \"%s\"%s\n", escaped ? escaped : config->url_fetcher.whitelist[i],
                    i < config->url_fetcher.whitelist_count - 1 ? "," : "");
            free(escaped);
         }
         fprintf(fp, "]\n");
      }

      fprintf(fp, "\n[url_fetcher.flaresolverr]\n");
      fprintf(fp, "enabled = %s\n", config->url_fetcher.flaresolverr.enabled ? "true" : "false");
      fprintf(fp, "endpoint = \"%s\"\n", config->url_fetcher.flaresolverr.endpoint);
      fprintf(fp, "timeout_sec = %d\n", config->url_fetcher.flaresolverr.timeout_sec);
      fprintf(fp, "max_response_bytes = %zu\n",
              config->url_fetcher.flaresolverr.max_response_bytes);

      /* Emit [url_fetcher.tavily] only when fallback selected — keeps
       * default dawn.toml clean for SearXNG-only users. */
      if (strcmp(config->url_fetcher.fallback, "tavily") == 0) {
         fprintf(fp, "\n[url_fetcher.tavily]\n");
         fprintf(fp, "timeout_sec = %d\n", config->url_fetcher.tavily.timeout_sec);
         fprintf(fp, "max_response_bytes = %zu\n", config->url_fetcher.tavily.max_response_bytes);
         if (config->url_fetcher.tavily.extract_depth[0]) {
            fprintf(fp, "extract_depth = \"%s\"\n", config->url_fetcher.tavily.extract_depth);
         }
         fprintf(fp, "rate_limit_per_minute = %d\n",
                 config->url_fetcher.tavily.rate_limit_per_minute);
         fprintf(fp, "rate_limit_per_hour = %d\n", config->url_fetcher.tavily.rate_limit_per_hour);
         fprintf(fp, "rate_limit_per_day = %d\n", config->url_fetcher.tavily.rate_limit_per_day);
      }
   }

   fprintf(fp, "\n[mqtt]\n");
   fprintf(fp, "enabled = %s\n", config->mqtt.enabled ? "true" : "false");
   fprintf(fp, "broker = \"%s\"\n", config->mqtt.broker);
   fprintf(fp, "port = %d\n", config->mqtt.port);
   fprintf(fp, "tls = %s\n", config->mqtt.tls ? "true" : "false");
   if (config->mqtt.tls_ca_cert[0])
      fprintf(fp, "tls_ca_cert = \"%s\"\n", config->mqtt.tls_ca_cert);
   if (config->mqtt.tls_cert_path[0])
      fprintf(fp, "tls_cert_path = \"%s\"\n", config->mqtt.tls_cert_path);
   if (config->mqtt.tls_key_path[0])
      fprintf(fp, "tls_key_path = \"%s\"\n", config->mqtt.tls_key_path);

   fprintf(fp, "\n[network]\n");
   fprintf(fp, "workers = %d\n", config->network.workers);
   fprintf(fp, "session_timeout_sec = %d\n", config->network.session_timeout_sec);
   fprintf(fp, "llm_timeout_ms = %d\n", config->network.llm_timeout_ms);
   fprintf(fp, "summarization_timeout_ms = %d\n", config->network.summarization_timeout_ms);

   fprintf(fp, "\n[tui]\n");
   fprintf(fp, "enabled = %s\n", config->tui.enabled ? "true" : "false");

   fprintf(fp, "\n[webui]\n");
   fprintf(fp, "enabled = %s\n", config->webui.enabled ? "true" : "false");
   fprintf(fp, "port = %d\n", config->webui.port);
   fprintf(fp, "max_clients = %d\n", config->webui.max_clients);
   fprintf(fp, "audio_chunk_ms = %d\n", config->webui.audio_chunk_ms);
   fprintf(fp, "www_path = \"%s\"\n", config->webui.www_path);
   fprintf(fp, "bind_address = \"%s\"\n", config->webui.bind_address);
   fprintf(fp, "https = %s\n", config->webui.https ? "true" : "false");
   if (config->webui.ssl_cert_path[0])
      fprintf(fp, "ssl_cert_path = \"%s\"\n", config->webui.ssl_cert_path);
   if (config->webui.ssl_key_path[0])
      fprintf(fp, "ssl_key_path = \"%s\"\n", config->webui.ssl_key_path);
   fprintf(fp, "export_max_messages = %d\n", config->webui.export_max_messages);
   /* Validate at write time to prevent TOML injection */
   const char *exp_fmt = (strcmp(config->webui.export_format, "html") == 0) ? "html" : "json";
   fprintf(fp, "export_format = \"%s\"\n", exp_fmt);

   fprintf(fp, "\n[memory]\n");
   fprintf(fp, "enabled = %s\n", config->memory.enabled ? "true" : "false");
   fprintf(fp, "context_budget_tokens = %d\n", config->memory.context_budget_tokens);
   fprintf(fp, "source_budget_chars = %d\n", config->memory.source_budget_chars);
   fprintf(fp, "extraction_provider = \"%s\"\n", config->memory.extraction_provider);
   fprintf(fp, "extraction_model = \"%s\"\n", config->memory.extraction_model);
   fprintf(fp, "extraction_openrouter_model = \"%s\"\n",
           config->memory.extraction_openrouter_model);
   fprintf(fp, "extraction_timeout_ms = %d\n", config->memory.extraction_timeout_ms);
   fprintf(fp, "paraphrase_dedup_enabled = %s\n",
           config->memory.paraphrase_dedup_enabled ? "true" : "false");
   fprintf(fp, "paraphrase_dedup_threshold = %.2f\n", config->memory.paraphrase_dedup_threshold);
   fprintf(fp, "note_extraction_guard = %s\n",
           config->memory.note_extraction_guard ? "true" : "false");
   fprintf(fp, "pruning_enabled = %s\n", config->memory.pruning_enabled ? "true" : "false");
   fprintf(fp, "prune_superseded_days = %d\n", config->memory.prune_superseded_days);
   fprintf(fp, "prune_stale_days = %d\n", config->memory.prune_stale_days);
   fprintf(fp, "prune_stale_min_confidence = %.2f\n", config->memory.prune_stale_min_confidence);
   fprintf(fp, "expire_enabled = %s\n", config->memory.expire_enabled ? "true" : "false");
   fprintf(fp, "expire_grace_days = %d\n", config->memory.expire_grace_days);
   fprintf(fp, "prune_expired_days = %d\n", config->memory.prune_expired_days);
   fprintf(fp, "conversation_idle_timeout_min = %d\n",
           config->memory.conversation_idle_timeout_min);
   fprintf(fp, "default_voice_user_id = %d\n", config->memory.default_voice_user_id);

   fprintf(fp, "\n[memory.decay]\n");
   fprintf(fp, "enabled = %s\n", config->memory.decay_enabled ? "true" : "false");
   fprintf(fp, "hour = %d\n", config->memory.decay_hour);
   fprintf(fp, "inferred_weekly = %.2f\n", config->memory.decay_inferred_weekly);
   fprintf(fp, "explicit_weekly = %.2f\n", config->memory.decay_explicit_weekly);
   fprintf(fp, "preference_weekly = %.2f\n", config->memory.decay_preference_weekly);
   fprintf(fp, "inferred_floor = %.2f\n", config->memory.decay_inferred_floor);
   fprintf(fp, "explicit_floor = %.2f\n", config->memory.decay_explicit_floor);
   fprintf(fp, "preference_floor = %.2f\n", config->memory.decay_preference_floor);
   fprintf(fp, "prune_threshold = %.2f\n", config->memory.decay_prune_threshold);
   fprintf(fp, "summary_retention_days = %d\n", config->memory.summary_retention_days);
   fprintf(fp, "access_reinforcement_boost = %.2f\n", config->memory.access_reinforcement_boost);

   fprintf(fp, "\n[memory.embeddings]\n");
   {
      char *escaped = toml_escape_string(config->memory.embedding_provider);
      fprintf(fp, "provider = \"%s\"\n", escaped ? escaped : config->memory.embedding_provider);
      free(escaped);
   }
   if (config->memory.embedding_model[0]) {
      char *escaped = toml_escape_string(config->memory.embedding_model);
      fprintf(fp, "model = \"%s\"\n", escaped ? escaped : config->memory.embedding_model);
      free(escaped);
   }
   if (config->memory.embedding_endpoint[0]) {
      char *escaped = toml_escape_string(config->memory.embedding_endpoint);
      fprintf(fp, "endpoint = \"%s\"\n", escaped ? escaped : config->memory.embedding_endpoint);
      free(escaped);
   }
   fprintf(fp, "keyword_weight = %.2f\n", config->memory.embedding_keyword_weight);
   fprintf(fp, "vector_weight = %.2f\n", config->memory.embedding_vector_weight);
   fprintf(fp, "temporal_weight = %.2f\n", config->memory.temporal_weight);
   fprintf(fp, "temporal_filter_enabled = %s\n",
           config->memory.temporal_filter_enabled ? "true" : "false");
   fprintf(fp, "rrf_enabled = %s\n", config->memory.rrf_enabled ? "true" : "false");
   fprintf(fp, "memory_text_minimal = %s\n", config->memory.memory_text_minimal ? "true" : "false");
   fprintf(fp, "bm25_enabled = %s\n", config->memory.bm25_enabled ? "true" : "false");
   fprintf(fp, "category_threshold = %.2f\n", config->memory.category_threshold);
   fprintf(fp, "search_score_floor = %.2f\n", config->memory.search_score_floor);

   fprintf(fp, "\n[memory.graph_retrieval]\n");
   fprintf(fp, "enabled = %s\n", config->memory.graph_retrieval.enabled ? "true" : "false");
   fprintf(fp, "entity_grounding_bonus = %.2f\n",
           config->memory.graph_retrieval.entity_grounding_bonus);
   fprintf(fp, "max_facts_per_query = %d\n", config->memory.graph_retrieval.max_facts_per_query);
   fprintf(fp, "use_query_scoring = %s\n",
           config->memory.graph_retrieval.use_query_scoring ? "true" : "false");
   fprintf(fp, "entity_bonus = %.2f\n", config->memory.graph_retrieval.entity_bonus);
   fprintf(fp, "backfill_on_startup = %s\n",
           config->memory.embedding_backfill_on_startup ? "true" : "false");
   if (config->memory.model_id[0]) {
      char *escaped = toml_escape_string(config->memory.model_id);
      fprintf(fp, "model_id = \"%s\"\n", escaped ? escaped : config->memory.model_id);
      free(escaped);
   }
   fprintf(fp, "recompute_on_model_change = %s\n",
           config->memory.recompute_on_model_change ? "true" : "false");
   fprintf(fp, "recompute_batch_size = %d\n", config->memory.recompute_batch_size);
   fprintf(fp, "recompute_batch_sleep_ms = %d\n", config->memory.recompute_batch_sleep_ms);

   fprintf(fp, "\n[memory.recovery]\n");
   fprintf(fp, "enabled = %s\n", config->memory.recovery_enabled ? "true" : "false");
   fprintf(fp, "idle_threshold_seconds = %d\n", config->memory.recovery_idle_threshold_seconds);
   fprintf(fp, "max_attempts = %d\n", config->memory.recovery_max_attempts);
   fprintf(fp, "recurring_interval_seconds = %d\n",
           config->memory.recovery_recurring_interval_seconds);

   {
      const focus_injection_config_t *fi = &config->memory.focus_injection;
      fprintf(fp, "\n[memory.focus_injection]\n");
      fprintf(fp, "enabled = %s\n", fi->enabled ? "true" : "false");
      fprintf(fp, "focus_budget_bytes = %d\n", fi->focus_budget_bytes);
      fprintf(fp, "top_k = %d\n", fi->top_k);
      fprintf(fp, "summary_max_scan = %d\n", fi->summary_max_scan);
      fprintf(fp, "min_score = %.2f\n", fi->min_score);
      fprintf(fp, "classifier_enabled = %s\n", fi->classifier_enabled ? "true" : "false");
      fprintf(fp, "weight_semantic = %.2f\n", fi->weight_semantic);
      fprintf(fp, "weight_recency = %.2f\n", fi->weight_recency);
      fprintf(fp, "weight_importance = %.2f\n", fi->weight_importance);
      fprintf(fp, "weight_source = %.2f\n", fi->weight_source);

      fprintf(fp, "\n[memory.focus_injection.source_weights]\n");
      fprintf(fp, "memory_fact = %.2f\n", fi->source_weights.memory_fact);
      fprintf(fp, "memory_entity = %.2f\n", fi->source_weights.memory_entity);
      fprintf(fp, "memory_relation = %.2f\n", fi->source_weights.memory_relation);
      fprintf(fp, "memory_summary = %.2f\n", fi->source_weights.memory_summary);
      fprintf(fp, "document_chunk = %.2f\n", fi->source_weights.document_chunk);
      fprintf(fp, "calendar_event = %.2f\n", fi->source_weights.calendar_event);
      fprintf(fp, "recent_email = %.2f\n", fi->source_weights.recent_email);
      fprintf(fp, "dawn_background = %.2f\n", fi->source_weights.dawn_background);

      fprintf(fp, "\n[memory.focus_injection.dedup]\n");
      fprintf(fp, "recent_window_turns = %d\n", fi->dedup.recent_window_turns);
      fprintf(fp, "score_uplift_factor = %.2f\n", fi->dedup.score_uplift_factor);

      fprintf(fp, "\n[memory.focus_injection.dominant_token_heuristic]\n");
      fprintf(fp, "enabled = %s\n", fi->dominant_token_heuristic.enabled ? "true" : "false");
      fprintf(fp, "threshold = %.2f\n", fi->dominant_token_heuristic.threshold);
      fprintf(fp, "base_penalty = %.2f\n", fi->dominant_token_heuristic.base_penalty);
   }

   fprintf(fp, "\n[memory.entity_merge]\n");
   fprintf(fp, "enabled = %s\n", config->memory.entity_merge_enabled ? "true" : "false");
   fprintf(fp, "auto_threshold = %.2f\n", config->memory.entity_merge_auto_threshold);
   fprintf(fp, "review_threshold = %.2f\n", config->memory.entity_merge_review_threshold);

   fprintf(fp, "\n[debug]\n");
   fprintf(fp, "mic_record = %s\n", config->debug.mic_record ? "true" : "false");
   fprintf(fp, "asr_record = %s\n", config->debug.asr_record ? "true" : "false");
   fprintf(fp, "aec_record = %s\n", config->debug.aec_record ? "true" : "false");
   fprintf(fp, "record_path = \"%s\"\n", config->debug.record_path);
   fprintf(fp, "silent_observe_test_endpoint = %s\n",
           config->debug.silent_observe_test_endpoint ? "true" : "false");

   fprintf(fp, "\n[paths]\n");
   if (config->paths.data_dir[0] != '\0') {
      fprintf(fp, "data_dir = \"%s\"\n", config->paths.data_dir);
   }
   fprintf(fp, "music_dir = \"%s\"\n", config->paths.music_dir);

   /* [images] controls stored image retention and cleanup */
   fprintf(fp, "\n[images]\n");
   fprintf(fp, "retention_days = %d\n", config->images.retention_days);
   fprintf(fp, "max_size_mb = %d\n", config->images.max_size_mb);
   fprintf(fp, "max_per_user = %d\n", config->images.max_per_user);
   fprintf(fp, "cache_size_mb = %d\n", config->images.cache_size_mb);

   /* [documents] controls document upload and extraction limits */
   fprintf(fp, "\n[documents]\n");
   fprintf(fp, "max_file_size_kb = %d\n", config->documents.max_file_size_kb);
   fprintf(fp, "max_documents = %d\n", config->documents.max_documents);
   fprintf(fp, "max_pages = %d\n", config->documents.max_pages);
   fprintf(fp, "max_extracted_size_kb = %d\n", config->documents.max_extracted_size_kb);
   fprintf(fp, "max_index_size_kb = %d\n", config->documents.max_index_size_kb);
   fprintf(fp, "max_indexed_documents = %d\n", config->documents.max_indexed_documents);
   fprintf(fp, "fts_label_weight = %.2f\n", config->documents.fts_label_weight);
   fprintf(fp, "fts_body_weight = %.2f\n", config->documents.fts_body_weight);
   fprintf(fp, "hybrid_keyword_weight = %.2f\n", config->documents.hybrid_keyword_weight);
   fprintf(fp, "hybrid_vector_weight = %.2f\n", config->documents.hybrid_vector_weight);
   fprintf(fp, "phrase_bonus_weight = %.2f\n", config->documents.phrase_bonus_weight);
   fprintf(fp, "search_min_score = %.2f\n", config->documents.search_min_score);
   fprintf(fp, "version_retention_days = %d\n", config->documents.version_retention_days);
   fprintf(fp, "version_keep_per_doc = %d\n", config->documents.version_keep_per_doc);

   /* [vision] controls per-upload image size and dimension limits */
   fprintf(fp, "\n[vision]\n");
   fprintf(fp, "max_image_size_kb = %d\n", config->vision.max_image_size_kb);
   fprintf(fp, "max_dimension = %d\n", config->vision.max_dimension);
   fprintf(fp, "max_images = %d\n", config->vision.max_images);

   fprintf(fp, "\n[music]\n");
   fprintf(fp, "scan_interval_minutes = %d\n", config->music.scan_interval_minutes);

   /* [music.plex] — Plex Media Server connection settings */
   fprintf(fp, "\n[music.plex]\n");
   fprintf(fp, "host = \"%s\"\n", config->music.plex.host);
   fprintf(fp, "port = %d\n", config->music.plex.port);
   fprintf(fp, "music_section_id = %d\n", config->music.plex.music_section_id);
   fprintf(fp, "ssl = %s\n", config->music.plex.ssl ? "true" : "false");
   fprintf(fp, "ssl_verify = %s\n", config->music.plex.ssl_verify ? "true" : "false");
   if (config->music.plex.client_identifier[0]) {
      fprintf(fp, "client_identifier = \"%s\"\n", config->music.plex.client_identifier);
   }

   fprintf(fp, "\n[music.streaming]\n");
   fprintf(fp, "enabled = %s\n", config->music.streaming_enabled ? "true" : "false");
   fprintf(fp, "default_quality = \"%s\"\n", config->music.streaming_quality);
   fprintf(fp, "bitrate_mode = \"%s\"\n", config->music.streaming_bitrate_mode);

   /* [calendar] CalDAV integration settings */
   fprintf(fp, "\n[calendar]\n");
   fprintf(fp, "enabled = %s\n", config->calendar.enabled ? "true" : "false");
   fprintf(fp, "sync_interval_sec = %d\n", config->calendar.sync_interval_sec);
   fprintf(fp, "cache_past_days = %d\n", config->calendar.cache_past_days);
   fprintf(fp, "cache_future_days = %d\n", config->calendar.cache_future_days);
   fprintf(fp, "default_event_duration_min = %d\n", config->calendar.default_event_duration_min);

   /* [messaging.sms] — explicit sub-table header so sms_active_window_sec
    * round-trips (it was parsed but never re-serialized before). */
   fprintf(fp, "\n[messaging.sms]\n");
   fprintf(fp, "active_window_sec = %d\n", config->messaging.sms_active_window_sec);

   /* [ota] — round-trips so a WebUI settings save can't drop it. */
   fprintf(fp, "\n[ota]\n");
   fprintf(fp, "enabled = %s\n", config->ota.enabled ? "true" : "false");
   fprintf(fp, "release_dir = \"%s\"\n", config->ota.release_dir);
   fprintf(fp, "download_token_ttl_sec = %d\n", config->ota.download_token_ttl_sec);
   fprintf(fp, "require_tls = %s\n", config->ota.require_tls ? "true" : "false");

   /* Write tool-owned config sections (e.g. [home_assistant], [shutdown]) */
   tool_registry_write_configs(fp);

   fclose(fp);
   OLOG_INFO("Configuration written to %s", path);
   return 0;
}

int secrets_write_toml(const secrets_config_t *secrets, const char *path) {
   if (!secrets || !path)
      return 1;

   /* Open with restrictive permissions from the start (no TOCTOU window) */
   int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0) {
      OLOG_ERROR("Failed to open secrets file for writing: %s (%s)", path, strerror(errno));
      return 1;
   }
   FILE *fp = fdopen(fd, "w");
   if (!fp) {
      OLOG_ERROR("Failed to fdopen secrets file: %s (%s)", path, strerror(errno));
      close(fd);
      return 1;
   }

   fprintf(fp, "# DAWN Secrets Configuration\n");
   fprintf(fp, "# Auto-generated by WebUI settings panel\n");
   fprintf(fp, "# WARNING: This file contains sensitive information!\n\n");

   fprintf(fp, "[secrets]\n");

   /* Helper macro to write escaped string, with error handling for allocation failure */
#define WRITE_SECRET(key, value)                                                  \
   do {                                                                           \
      if ((value)[0]) {                                                           \
         char *escaped = toml_escape_string(value);                               \
         if (!escaped) {                                                          \
            OLOG_ERROR("Failed to allocate memory for escaping secret: %s", key); \
            fclose(fp);                                                           \
            return 1;                                                             \
         }                                                                        \
         fprintf(fp, "%s = \"%s\"\n", key, escaped);                              \
         free(escaped);                                                           \
      }                                                                           \
   } while (0)

   WRITE_SECRET("openai_api_key", secrets->openai_api_key);
   WRITE_SECRET("claude_api_key", secrets->claude_api_key);
   WRITE_SECRET("gemini_api_key", secrets->gemini_api_key);
   WRITE_SECRET("openrouter_api_key", secrets->openrouter_api_key);
   WRITE_SECRET("tavily_api_key", secrets->tavily_api_key);
   WRITE_SECRET("mqtt_username", secrets->mqtt_username);
   WRITE_SECRET("mqtt_password", secrets->mqtt_password);
   WRITE_SECRET("satellite_registration_key", secrets->satellite_registration_key);
   WRITE_SECRET("plex_token", secrets->plex_token);
   WRITE_SECRET("embedding_api_key", secrets->embedding_api_key);

   /* Service-to-service auth token + messaging-channel driver tokens.  These
    * are parsed from [secrets] (config_parser.c) but were previously omitted
    * here, so any WebUI "Save Secrets" silently dropped them (O_TRUNC rewrite).
    * Preserve them on write — even service_token, which isn't WebUI-editable. */
   WRITE_SECRET("service_token", secrets->service_token);
   WRITE_SECRET("telegram_bot_token", secrets->telegram_bot_token);
   WRITE_SECRET("discord_bot_token", secrets->discord_bot_token);
   WRITE_SECRET("slack_app_token", secrets->slack_app_token);
   WRITE_SECRET("slack_bot_token", secrets->slack_bot_token);

   /* Home Assistant */
   if (secrets->home_assistant_token[0]) {
      fprintf(fp, "\n[secrets.home_assistant]\n");
      WRITE_SECRET("token", secrets->home_assistant_token);
   }

   /* Google OAuth 2.0 (Calendar and Email) */
   if (secrets->google_client_id[0] || secrets->google_client_secret[0] ||
       secrets->google_redirect_url[0]) {
      fprintf(fp, "\n[secrets.google]\n");
      WRITE_SECRET("client_id", secrets->google_client_id);
      WRITE_SECRET("client_secret", secrets->google_client_secret);
      WRITE_SECRET("redirect_url", secrets->google_redirect_url);
   }

#undef WRITE_SECRET

   fclose(fp); /* Also closes the underlying fd */

   OLOG_INFO("Secrets written to %s", path);
   return 0;
}
