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
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "config/config_parser.h"
#include "logging.h"

/* =============================================================================
 * Helper Macros
 * ============================================================================= */
#define SAFE_COPY(dst, src)                   \
   do {                                       \
      strncpy((dst), (src), sizeof(dst) - 1); \
      (dst)[sizeof(dst) - 1] = '\0';          \
   } while (0)

#define ENV_STRING(env_name, dest)                          \
   do {                                                     \
      const char *val = getenv(env_name);                   \
      if (val) {                                            \
         SAFE_COPY(dest, val);                              \
         LOG_INFO("Config override: %s=%s", env_name, val); \
      }                                                     \
   } while (0)

/* Like ENV_STRING but redacts value in logs - use for API keys and secrets */
#define ENV_SECRET(env_name, dest)                             \
   do {                                                        \
      const char *val = getenv(env_name);                      \
      if (val) {                                               \
         SAFE_COPY(dest, val);                                 \
         LOG_INFO("Config override: %s=[REDACTED]", env_name); \
      }                                                        \
   } while (0)

#define ENV_INT(env_name, dest)                              \
   do {                                                      \
      const char *val = getenv(env_name);                    \
      if (val) {                                             \
         dest = atoi(val);                                   \
         LOG_INFO("Config override: %s=%d", env_name, dest); \
      }                                                      \
   } while (0)

#define ENV_FLOAT(env_name, dest)                              \
   do {                                                        \
      const char *val = getenv(env_name);                      \
      if (val) {                                               \
         dest = (float)atof(val);                              \
         LOG_INFO("Config override: %s=%.2f", env_name, dest); \
      }                                                        \
   } while (0)

#define ENV_BOOL(env_name, dest)                                                \
   do {                                                                         \
      const char *val = getenv(env_name);                                       \
      if (val) {                                                                \
         dest = (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0 ||       \
                 strcasecmp(val, "yes") == 0);                                  \
         LOG_INFO("Config override: %s=%s", env_name, dest ? "true" : "false"); \
      }                                                                         \
   } while (0)

#define ENV_SIZE_T(env_name, dest)                            \
   do {                                                       \
      const char *val = getenv(env_name);                     \
      if (val) {                                              \
         dest = (size_t)atol(val);                            \
         LOG_INFO("Config override: %s=%zu", env_name, dest); \
      }                                                       \
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

   /* [tts] */
   ENV_STRING("DAWN_TTS_MODELS_PATH", config->tts.models_path);
   ENV_STRING("DAWN_TTS_VOICE_MODEL", config->tts.voice_model);
   ENV_FLOAT("DAWN_TTS_LENGTH_SCALE", config->tts.length_scale);

   /* [commands] */
   ENV_STRING("DAWN_COMMANDS_PROCESSING_MODE", config->commands.processing_mode);

   /* [llm] */
   ENV_STRING("DAWN_LLM_TYPE", config->llm.type);
   ENV_INT("DAWN_LLM_MAX_TOKENS", config->llm.max_tokens);

   /* [llm.cloud] */
   ENV_STRING("DAWN_LLM_CLOUD_PROVIDER", config->llm.cloud.provider);
   ENV_STRING("DAWN_LLM_CLOUD_ENDPOINT", config->llm.cloud.endpoint);
   ENV_BOOL("DAWN_LLM_CLOUD_VISION_ENABLED", config->llm.cloud.vision_enabled);
   ENV_INT("DAWN_LLM_CLOUD_OPENAI_DEFAULT_MODEL_IDX", config->llm.cloud.openai_default_model_idx);
   ENV_INT("DAWN_LLM_CLOUD_CLAUDE_DEFAULT_MODEL_IDX", config->llm.cloud.claude_default_model_idx);
   ENV_INT("DAWN_LLM_CLOUD_GEMINI_DEFAULT_MODEL_IDX", config->llm.cloud.gemini_default_model_idx);

   /* [llm.local] */
   ENV_STRING("DAWN_LLM_LOCAL_ENDPOINT", config->llm.local.endpoint);
   ENV_STRING("DAWN_LLM_LOCAL_MODEL", config->llm.local.model);
   ENV_BOOL("DAWN_LLM_LOCAL_VISION_ENABLED", config->llm.local.vision_enabled);

   /* [llm.tools] */
   ENV_STRING("DAWN_LLM_TOOLS_MODE", config->llm.tools.mode);

   /* [search] */
   ENV_STRING("DAWN_SEARCH_ENGINE", config->search.engine);
   ENV_STRING("DAWN_SEARCH_ENDPOINT", config->search.endpoint);

   /* [search.summarizer] */
   ENV_STRING("DAWN_SEARCH_SUMMARIZER_BACKEND", config->search.summarizer.backend);
   ENV_SIZE_T("DAWN_SEARCH_SUMMARIZER_THRESHOLD_BYTES", config->search.summarizer.threshold_bytes);
   ENV_SIZE_T("DAWN_SEARCH_SUMMARIZER_TARGET_WORDS", config->search.summarizer.target_words);
   ENV_FLOAT("DAWN_SEARCH_SUMMARIZER_TARGET_RATIO", config->search.summarizer.target_ratio);

   /* [url_fetcher.flaresolverr] */
   ENV_BOOL("DAWN_URL_FETCHER_FLARESOLVERR_ENABLED", config->url_fetcher.flaresolverr.enabled);
   ENV_STRING("DAWN_URL_FETCHER_FLARESOLVERR_ENDPOINT", config->url_fetcher.flaresolverr.endpoint);
   ENV_INT("DAWN_URL_FETCHER_FLARESOLVERR_TIMEOUT_SEC",
           config->url_fetcher.flaresolverr.timeout_sec);
   ENV_SIZE_T("DAWN_URL_FETCHER_FLARESOLVERR_MAX_RESPONSE_BYTES",
              config->url_fetcher.flaresolverr.max_response_bytes);

   /* [mqtt] */
   ENV_BOOL("DAWN_MQTT_ENABLED", config->mqtt.enabled);
   ENV_STRING("DAWN_MQTT_BROKER", config->mqtt.broker);
   ENV_INT("DAWN_MQTT_PORT", config->mqtt.port);
   ENV_STRING("MQTT_USERNAME", secrets->mqtt_username);
   ENV_STRING("MQTT_PASSWORD", secrets->mqtt_password);

   /* SmartThings authentication (PAT or OAuth2) */
   ENV_STRING("SMARTTHINGS_ACCESS_TOKEN", secrets->smartthings_access_token);
   ENV_STRING("SMARTTHINGS_CLIENT_ID", secrets->smartthings_client_id);
   ENV_STRING("SMARTTHINGS_CLIENT_SECRET", secrets->smartthings_client_secret);

   /* [network] */
   ENV_BOOL("DAWN_NETWORK_ENABLED", config->network.enabled);
   ENV_STRING("DAWN_NETWORK_HOST", config->network.host);
   ENV_INT("DAWN_NETWORK_PORT", config->network.port);
   ENV_INT("DAWN_NETWORK_WORKERS", config->network.workers);
   ENV_INT("DAWN_NETWORK_SOCKET_TIMEOUT_SEC", config->network.socket_timeout_sec);
   ENV_INT("DAWN_NETWORK_SESSION_TIMEOUT_SEC", config->network.session_timeout_sec);
   ENV_INT("DAWN_NETWORK_LLM_TIMEOUT_MS", config->network.llm_timeout_ms);

   /* [tui] */
   ENV_BOOL("DAWN_TUI_ENABLED", config->tui.enabled);

   /* [debug] */
   ENV_BOOL("DAWN_DEBUG_MIC_RECORD", config->debug.mic_record);
   ENV_BOOL("DAWN_DEBUG_ASR_RECORD", config->debug.asr_record);
   ENV_BOOL("DAWN_DEBUG_AEC_RECORD", config->debug.aec_record);
   ENV_STRING("DAWN_DEBUG_RECORD_PATH", config->debug.record_path);

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

   printf("\n[url_fetcher.flaresolverr]\n");
   printf("  enabled = %s\n", config->url_fetcher.flaresolverr.enabled ? "true" : "false");
   printf("  endpoint = \"%s\"\n", config->url_fetcher.flaresolverr.endpoint);
   printf("  timeout_sec = %d\n", config->url_fetcher.flaresolverr.timeout_sec);
   printf("  max_response_bytes = %zu\n", config->url_fetcher.flaresolverr.max_response_bytes);

   printf("\n[mqtt]\n");
   printf("  enabled = %s\n", config->mqtt.enabled ? "true" : "false");
   printf("  broker = \"%s\"\n", config->mqtt.broker);
   printf("  port = %d\n", config->mqtt.port);

   printf("\n[network]\n");
   printf("  enabled = %s\n", config->network.enabled ? "true" : "false");
   printf("  host = \"%s\"\n", config->network.host);
   printf("  port = %d\n", config->network.port);
   printf("  workers = %d\n", config->network.workers);
   printf("  socket_timeout_sec = %d\n", config->network.socket_timeout_sec);
   printf("  session_timeout_sec = %d\n", config->network.session_timeout_sec);
   printf("  llm_timeout_ms = %d\n", config->network.llm_timeout_ms);

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

   /* [network] */
   printf("[network]\n");
   PRINT_SETTING_BOOL("enabled", config->network.enabled, "DAWN_NETWORK_ENABLED",
                      detect_source_bool(config->network.enabled, defaults.network.enabled,
                                         "DAWN_NETWORK_ENABLED"));
   PRINT_SETTING_STR("host", config->network.host, "DAWN_NETWORK_HOST",
                     detect_source_str(config->network.host, defaults.network.host,
                                       "DAWN_NETWORK_HOST"));
   PRINT_SETTING_INT("port", config->network.port, "DAWN_NETWORK_PORT",
                     detect_source_int(config->network.port, defaults.network.port,
                                       "DAWN_NETWORK_PORT"));
   PRINT_SETTING_INT("workers", config->network.workers, "DAWN_NETWORK_WORKERS",
                     detect_source_int(config->network.workers, defaults.network.workers,
                                       "DAWN_NETWORK_WORKERS"));
   PRINT_SETTING_INT("socket_timeout_sec", config->network.socket_timeout_sec,
                     "DAWN_NETWORK_SOCKET_TIMEOUT_SEC",
                     detect_source_int(config->network.socket_timeout_sec,
                                       defaults.network.socket_timeout_sec,
                                       "DAWN_NETWORK_SOCKET_TIMEOUT_SEC"));
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
   printf("  SMARTTHINGS_ACCESS_TOKEN                 %s\n",
          (secrets && secrets->smartthings_access_token[0]) ? "[set]" : "[not set]");
   printf("  SMARTTHINGS_CLIENT_ID                    %s\n",
          (secrets && secrets->smartthings_client_id[0]) ? "[set]" : "[not set]");
   printf("  SMARTTHINGS_CLIENT_SECRET                %s\n\n",
          (secrets && secrets->smartthings_client_secret[0]) ? "[set]" : "[not set]");
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

   printf("\n[network]\n");
   printf("enabled = %s\n", config->network.enabled ? "true" : "false");
   printf("host = \"%s\"\n", config->network.host);
   printf("port = %d\n", config->network.port);
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
   json_object_object_add(llm, "thinking", thinking);

   /* Context management settings */
   json_object_object_add(llm, "summarize_threshold",
                          json_object_new_double(config->llm.summarize_threshold));
   json_object_object_add(llm, "conversation_logging",
                          json_object_new_boolean(config->llm.conversation_logging));
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

   /* [url_fetcher] */
   json_object *url_fetcher = json_object_new_object();
   json_object_object_add(url_fetcher, "whitelist_count",
                          json_object_new_int(config->url_fetcher.whitelist_count));

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
   json_object_object_add(root, "url_fetcher", url_fetcher);

   /* [mqtt] */
   json_object *mqtt = json_object_new_object();
   json_object_object_add(mqtt, "enabled", json_object_new_boolean(config->mqtt.enabled));
   json_object_object_add(mqtt, "broker", json_object_new_string(config->mqtt.broker));
   json_object_object_add(mqtt, "port", json_object_new_int(config->mqtt.port));
   json_object_object_add(root, "mqtt", mqtt);

   /* [network] */
   json_object *network = json_object_new_object();
   json_object_object_add(network, "enabled", json_object_new_boolean(config->network.enabled));
   json_object_object_add(network, "host", json_object_new_string(config->network.host));
   json_object_object_add(network, "port", json_object_new_int(config->network.port));
   json_object_object_add(network, "workers", json_object_new_int(config->network.workers));
   json_object_object_add(network, "socket_timeout_sec",
                          json_object_new_int(config->network.socket_timeout_sec));
   json_object_object_add(network, "session_timeout_sec",
                          json_object_new_int(config->network.session_timeout_sec));
   json_object_object_add(network, "llm_timeout_ms",
                          json_object_new_int(config->network.llm_timeout_ms));
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
   json_object_object_add(webui, "workers", json_object_new_int(config->webui.workers));
   json_object_object_add(webui, "www_path", json_object_new_string(config->webui.www_path));
   json_object_object_add(webui, "bind_address",
                          json_object_new_string(config->webui.bind_address));
   json_object_object_add(webui, "https", json_object_new_boolean(config->webui.https));
   json_object_object_add(webui, "ssl_cert_path",
                          json_object_new_string(config->webui.ssl_cert_path));
   json_object_object_add(webui, "ssl_key_path",
                          json_object_new_string(config->webui.ssl_key_path));
   json_object_object_add(root, "webui", webui);

   /* [memory] */
   json_object *memory = json_object_new_object();
   json_object_object_add(memory, "enabled", json_object_new_boolean(config->memory.enabled));
   json_object_object_add(memory, "context_budget_tokens",
                          json_object_new_int(config->memory.context_budget_tokens));
   json_object_object_add(memory, "extraction_provider",
                          json_object_new_string(config->memory.extraction_provider));
   json_object_object_add(memory, "extraction_model",
                          json_object_new_string(config->memory.extraction_model));
   json_object_object_add(memory, "pruning_enabled",
                          json_object_new_boolean(config->memory.pruning_enabled));
   json_object_object_add(memory, "prune_superseded_days",
                          json_object_new_int(config->memory.prune_superseded_days));
   json_object_object_add(memory, "prune_stale_days",
                          json_object_new_int(config->memory.prune_stale_days));
   json_object_object_add(memory, "prune_stale_min_confidence",
                          json_object_new_double(config->memory.prune_stale_min_confidence));
   json_object_object_add(memory, "conversation_idle_timeout_min",
                          json_object_new_int(config->memory.conversation_idle_timeout_min));
   json_object_object_add(memory, "default_voice_user_id",
                          json_object_new_int(config->memory.default_voice_user_id));
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
   json_object_object_add(root, "images", images);

   /* Music configuration (music.streaming) */
   json_object *music = json_object_new_object();
   json_object_object_add(music, "scan_interval_minutes",
                          json_object_new_int(config->music.scan_interval_minutes));
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
   json_object_object_add(obj, "mqtt_username",
                          json_object_new_boolean(secrets && secrets->mqtt_username[0]));
   json_object_object_add(obj, "mqtt_password",
                          json_object_new_boolean(secrets && secrets->mqtt_password[0]));
   json_object_object_add(obj, "smartthings_client_id",
                          json_object_new_boolean(secrets && secrets->smartthings_client_id[0]));
   json_object_object_add(obj, "smartthings_client_secret",
                          json_object_new_boolean(secrets &&
                                                  secrets->smartthings_client_secret[0]));

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
      LOG_ERROR("Failed to open config file for writing: %s (%s)", path, strerror(errno));
      return 1;
   }

   fprintf(fp, "# DAWN Configuration\n");
   fprintf(fp, "# Auto-generated by WebUI settings panel\n\n");

   fprintf(fp, "[general]\n");
   write_toml_string(fp, "ai_name", config->general.ai_name);
   if (config->general.log_file[0])
      write_toml_string(fp, "log_file", config->general.log_file);

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

   fprintf(fp, "\n[tts]\n");
   fprintf(fp, "models_path = \"%s\"\n", config->tts.models_path);
   fprintf(fp, "voice_model = \"%s\"\n", config->tts.voice_model);
   fprintf(fp, "length_scale = %.2f\n", config->tts.length_scale);

   fprintf(fp, "\n[commands]\n");
   fprintf(fp, "processing_mode = \"%s\"\n", config->commands.processing_mode);

   fprintf(fp, "\n[llm]\n");
   fprintf(fp, "type = \"%s\"\n", config->llm.type);
   fprintf(fp, "max_tokens = %d\n", config->llm.max_tokens);
   fprintf(fp, "summarize_threshold = %.2f\n", config->llm.summarize_threshold);
   fprintf(fp, "conversation_logging = %s\n", config->llm.conversation_logging ? "true" : "false");

   fprintf(fp, "\n[llm.cloud]\n");
   fprintf(fp, "provider = \"%s\"\n", config->llm.cloud.provider);
   if (config->llm.cloud.endpoint[0])
      fprintf(fp, "endpoint = \"%s\"\n", config->llm.cloud.endpoint);
   fprintf(fp, "vision_enabled = %s\n", config->llm.cloud.vision_enabled ? "true" : "false");

   /* Helper macro for writing model arrays with proper escaping.
    * Model names are expected to be ASCII alphanumeric (e.g., "gpt-4o", "gemini-2.5-flash"),
    * so escape failures are unlikely. We log a warning but continue with unescaped value
    * to avoid breaking config save for the entire file. */
#define WRITE_MODEL_ARRAY(array_name, idx_key, array, count, idx_var)                              \
   do {                                                                                            \
      if ((count) > 0) {                                                                           \
         fprintf(fp, "%s = [\n", array_name);                                                      \
         for (int i = 0; i < (count); i++) {                                                       \
            char *escaped = toml_escape_string((array)[i]);                                        \
            if (!escaped) {                                                                        \
               LOG_WARNING("Failed to escape model name '%s', using unescaped value", (array)[i]); \
            }                                                                                      \
            fprintf(fp, "    \"%s\"%s\n", escaped ? escaped : (array)[i],                          \
                    i < (count)-1 ? "," : "");                                                     \
            free(escaped);                                                                         \
         }                                                                                         \
         fprintf(fp, "]\n");                                                                       \
      }                                                                                            \
      fprintf(fp, "%s = %d\n", idx_key, idx_var);                                                  \
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

   fprintf(fp, "\n[llm.thinking]\n");
   fprintf(fp, "mode = \"%s\"\n", config->llm.thinking.mode);
   fprintf(fp, "reasoning_effort = \"%s\"\n", config->llm.thinking.reasoning_effort);
   fprintf(fp, "budget_low = %d\n", config->llm.thinking.budget_low);
   fprintf(fp, "budget_medium = %d\n", config->llm.thinking.budget_medium);
   fprintf(fp, "budget_high = %d\n", config->llm.thinking.budget_high);

   fprintf(fp, "\n[search]\n");
   fprintf(fp, "engine = \"%s\"\n", config->search.engine);
   fprintf(fp, "endpoint = \"%s\"\n", config->search.endpoint);

   fprintf(fp, "\n[search.summarizer]\n");
   fprintf(fp, "backend = \"%s\"\n", config->search.summarizer.backend);
   fprintf(fp, "threshold_bytes = %zu\n", config->search.summarizer.threshold_bytes);
   fprintf(fp, "target_words = %zu\n", config->search.summarizer.target_words);
   fprintf(fp, "target_ratio = %.2f\n", config->search.summarizer.target_ratio);

   /* Write title_filters if any are configured */
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

   if (config->url_fetcher.whitelist_count > 0 || config->url_fetcher.flaresolverr.enabled) {
      fprintf(fp, "\n[url_fetcher]\n");
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
   }

   fprintf(fp, "\n[mqtt]\n");
   fprintf(fp, "enabled = %s\n", config->mqtt.enabled ? "true" : "false");
   fprintf(fp, "broker = \"%s\"\n", config->mqtt.broker);
   fprintf(fp, "port = %d\n", config->mqtt.port);

   fprintf(fp, "\n[network]\n");
   fprintf(fp, "enabled = %s\n", config->network.enabled ? "true" : "false");
   fprintf(fp, "host = \"%s\"\n", config->network.host);
   fprintf(fp, "port = %d\n", config->network.port);
   fprintf(fp, "workers = %d\n", config->network.workers);
   fprintf(fp, "socket_timeout_sec = %d\n", config->network.socket_timeout_sec);
   fprintf(fp, "session_timeout_sec = %d\n", config->network.session_timeout_sec);
   fprintf(fp, "llm_timeout_ms = %d\n", config->network.llm_timeout_ms);

   fprintf(fp, "\n[tui]\n");
   fprintf(fp, "enabled = %s\n", config->tui.enabled ? "true" : "false");

   fprintf(fp, "\n[webui]\n");
   fprintf(fp, "enabled = %s\n", config->webui.enabled ? "true" : "false");
   fprintf(fp, "port = %d\n", config->webui.port);
   fprintf(fp, "max_clients = %d\n", config->webui.max_clients);
   fprintf(fp, "audio_chunk_ms = %d\n", config->webui.audio_chunk_ms);
   fprintf(fp, "workers = %d\n", config->webui.workers);
   fprintf(fp, "www_path = \"%s\"\n", config->webui.www_path);
   fprintf(fp, "bind_address = \"%s\"\n", config->webui.bind_address);
   fprintf(fp, "https = %s\n", config->webui.https ? "true" : "false");
   if (config->webui.ssl_cert_path[0])
      fprintf(fp, "ssl_cert_path = \"%s\"\n", config->webui.ssl_cert_path);
   if (config->webui.ssl_key_path[0])
      fprintf(fp, "ssl_key_path = \"%s\"\n", config->webui.ssl_key_path);

   fprintf(fp, "\n[memory]\n");
   fprintf(fp, "enabled = %s\n", config->memory.enabled ? "true" : "false");
   fprintf(fp, "context_budget_tokens = %d\n", config->memory.context_budget_tokens);
   fprintf(fp, "extraction_provider = \"%s\"\n", config->memory.extraction_provider);
   fprintf(fp, "extraction_model = \"%s\"\n", config->memory.extraction_model);
   fprintf(fp, "pruning_enabled = %s\n", config->memory.pruning_enabled ? "true" : "false");
   fprintf(fp, "prune_superseded_days = %d\n", config->memory.prune_superseded_days);
   fprintf(fp, "prune_stale_days = %d\n", config->memory.prune_stale_days);
   fprintf(fp, "prune_stale_min_confidence = %.2f\n", config->memory.prune_stale_min_confidence);
   fprintf(fp, "conversation_idle_timeout_min = %d\n",
           config->memory.conversation_idle_timeout_min);
   fprintf(fp, "default_voice_user_id = %d\n", config->memory.default_voice_user_id);

   fprintf(fp, "\n[debug]\n");
   fprintf(fp, "mic_record = %s\n", config->debug.mic_record ? "true" : "false");
   fprintf(fp, "asr_record = %s\n", config->debug.asr_record ? "true" : "false");
   fprintf(fp, "aec_record = %s\n", config->debug.aec_record ? "true" : "false");
   fprintf(fp, "record_path = \"%s\"\n", config->debug.record_path);

   fprintf(fp, "\n[paths]\n");
   if (config->paths.data_dir[0] != '\0') {
      fprintf(fp, "data_dir = \"%s\"\n", config->paths.data_dir);
   }
   fprintf(fp, "music_dir = \"%s\"\n", config->paths.music_dir);

   fprintf(fp, "\n[images]\n");
   fprintf(fp, "retention_days = %d\n", config->images.retention_days);
   fprintf(fp, "max_size_mb = %d\n", config->images.max_size_mb);
   fprintf(fp, "max_per_user = %d\n", config->images.max_per_user);

   fprintf(fp, "\n[music]\n");
   fprintf(fp, "scan_interval_minutes = %d\n", config->music.scan_interval_minutes);

   fprintf(fp, "\n[music.streaming]\n");
   fprintf(fp, "enabled = %s\n", config->music.streaming_enabled ? "true" : "false");
   fprintf(fp, "default_quality = \"%s\"\n", config->music.streaming_quality);
   fprintf(fp, "bitrate_mode = \"%s\"\n", config->music.streaming_bitrate_mode);

   fclose(fp);
   LOG_INFO("Configuration written to %s", path);
   return 0;
}

int secrets_write_toml(const secrets_config_t *secrets, const char *path) {
   if (!secrets || !path)
      return 1;

   FILE *fp = fopen(path, "w");
   if (!fp) {
      LOG_ERROR("Failed to open secrets file for writing: %s (%s)", path, strerror(errno));
      return 1;
   }

   fprintf(fp, "# DAWN Secrets Configuration\n");
   fprintf(fp, "# Auto-generated by WebUI settings panel\n");
   fprintf(fp, "# WARNING: This file contains sensitive information!\n\n");

   fprintf(fp, "[secrets]\n");

   /* Helper macro to write escaped string, with error handling for allocation failure */
#define WRITE_SECRET(key, value)                                                 \
   do {                                                                          \
      if ((value)[0]) {                                                          \
         char *escaped = toml_escape_string(value);                              \
         if (!escaped) {                                                         \
            LOG_ERROR("Failed to allocate memory for escaping secret: %s", key); \
            fclose(fp);                                                          \
            return 1;                                                            \
         }                                                                       \
         fprintf(fp, "%s = \"%s\"\n", key, escaped);                             \
         free(escaped);                                                          \
      }                                                                          \
   } while (0)

   WRITE_SECRET("openai_api_key", secrets->openai_api_key);
   WRITE_SECRET("claude_api_key", secrets->claude_api_key);
   WRITE_SECRET("gemini_api_key", secrets->gemini_api_key);
   WRITE_SECRET("mqtt_username", secrets->mqtt_username);
   WRITE_SECRET("mqtt_password", secrets->mqtt_password);

   /* SmartThings OAuth client credentials */
   if (secrets->smartthings_client_id[0] || secrets->smartthings_client_secret[0]) {
      fprintf(fp, "\n[secrets.smartthings]\n");
      WRITE_SECRET("client_id", secrets->smartthings_client_id);
      WRITE_SECRET("client_secret", secrets->smartthings_client_secret);
   }

#undef WRITE_SECRET

   fclose(fp);

   /* Set restrictive permissions on secrets file */
   if (chmod(path, 0600) != 0) {
      LOG_WARNING("Failed to set permissions on secrets file: %s", strerror(errno));
   }

   LOG_INFO("Secrets written to %s", path);
   return 0;
}
