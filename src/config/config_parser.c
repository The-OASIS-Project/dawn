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
 * DAWN Configuration Parser - TOML file parsing implementation
 */

#include "config/config_parser.h"

#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/worker_pool.h"
#include "logging.h"
#include "tools/string_utils.h"
#include "tools/toml.h"

#define SUCCESS 0
#define FAILURE 1

/* =============================================================================
 * Static Path Storage for Loaded Config Files
 * ============================================================================= */
static char s_loaded_config_path[CONFIG_PATH_MAX] = { 0 };
static char s_loaded_secrets_path[CONFIG_PATH_MAX] = { 0 };

/* =============================================================================
 * Helper Macros for Safe String Copying
 * ============================================================================= */
#define PARSE_STRING(table, key, dest)             \
   do {                                            \
      toml_datum_t d = toml_string_in(table, key); \
      if (d.ok) {                                  \
         strncpy(dest, d.u.s, sizeof(dest) - 1);   \
         dest[sizeof(dest) - 1] = '\0';            \
         free(d.u.s);                              \
      }                                            \
   } while (0)

#define PARSE_INT(table, key, dest)             \
   do {                                         \
      toml_datum_t d = toml_int_in(table, key); \
      if (d.ok) {                               \
         dest = (int)d.u.i;                     \
      }                                         \
   } while (0)

#define PARSE_DOUBLE(table, key, dest)             \
   do {                                            \
      toml_datum_t d = toml_double_in(table, key); \
      if (d.ok) {                                  \
         dest = (float)d.u.d;                      \
      }                                            \
   } while (0)

#define PARSE_BOOL(table, key, dest)             \
   do {                                          \
      toml_datum_t d = toml_bool_in(table, key); \
      if (d.ok) {                                \
         dest = d.u.b ? true : false;            \
      }                                          \
   } while (0)

#define PARSE_SIZE_T(table, key, dest)          \
   do {                                         \
      toml_datum_t d = toml_int_in(table, key); \
      if (d.ok && d.u.i >= 0) {                 \
         dest = (size_t)d.u.i;                  \
      }                                         \
   } while (0)

/* =============================================================================
 * File Permission Security Check
 * ============================================================================= */

/**
 * @brief Check if a sensitive file has overly permissive permissions
 *
 * Warns if the file is readable by group or others, which could expose
 * sensitive data like API keys or password hashes.
 *
 * @param path Path to the file to check
 * @param file_description Human-readable description for warning messages
 */
static void check_sensitive_file_permissions(const char *path, const char *file_description) {
   if (!path)
      return;

   struct stat st;
   if (stat(path, &st) != 0)
      return; /* File doesn't exist or can't be read - other code handles this */

   /* Check for world-readable/writable (most critical) */
   if (st.st_mode & S_IROTH) {
      LOG_WARNING("========================================");
      LOG_WARNING("SECURITY WARNING: %s is world-readable!", file_description);
      LOG_WARNING("File: %s", path);
      LOG_WARNING("This exposes sensitive data to all users on the system.");
      LOG_WARNING("Fix with: chmod 600 %s", path);
      LOG_WARNING("========================================");
   }

   if (st.st_mode & S_IWOTH) {
      LOG_WARNING("========================================");
      LOG_WARNING("SECURITY WARNING: %s is world-writable!", file_description);
      LOG_WARNING("File: %s", path);
      LOG_WARNING("Any user on the system can modify this file!");
      LOG_WARNING("Fix with: chmod 600 %s", path);
      LOG_WARNING("========================================");
   }

   /* Check for group-readable/writable (less critical but still a concern) */
   if (st.st_mode & S_IRGRP) {
      LOG_WARNING("Security notice: %s is group-readable (%s)", file_description, path);
      LOG_WARNING("Consider: chmod 600 %s", path);
   }

   if (st.st_mode & S_IWGRP) {
      LOG_WARNING("Security notice: %s is group-writable (%s)", file_description, path);
      LOG_WARNING("Consider: chmod 600 %s", path);
   }
}

/* =============================================================================
 * Unknown Key Warning Helper
 * ============================================================================= */

/**
 * @brief Check a table for unknown keys and warn about them
 *
 * @param table TOML table to check
 * @param section Section name for error messages (e.g., "vad", "llm.cloud")
 * @param known_keys NULL-terminated array of known key names
 */
static void warn_unknown_keys(toml_table_t *table,
                              const char *section,
                              const char *const *known_keys) {
   if (!table)
      return;

   int i = 0;
   const char *key;
   while ((key = toml_key_in(table, i++)) != NULL) {
      /* Check if key is in known_keys list */
      int found = 0;
      for (int j = 0; known_keys[j] != NULL; j++) {
         if (strcmp(key, known_keys[j]) == 0) {
            found = 1;
            break;
         }
      }
      if (!found) {
         LOG_WARNING("Unknown config key [%s].%s (typo?)", section, key);
      }
   }
}

/* =============================================================================
 * Section Parsers
 * ============================================================================= */

static void parse_general(toml_table_t *table, general_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "ai_name", "log_file", NULL };
   warn_unknown_keys(table, "general", known_keys);

   PARSE_STRING(table, "ai_name", config->ai_name);
   PARSE_STRING(table, "log_file", config->log_file);
}

static void parse_persona(toml_table_t *table, persona_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "description", NULL };
   warn_unknown_keys(table, "persona", known_keys);

   PARSE_STRING(table, "description", config->description);
}

static void parse_localization(toml_table_t *table, localization_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "location", "timezone", "units", NULL };
   warn_unknown_keys(table, "localization", known_keys);

   PARSE_STRING(table, "location", config->location);
   PARSE_STRING(table, "timezone", config->timezone);
   PARSE_STRING(table, "units", config->units);
}

static void parse_audio_bargein(toml_table_t *table, bargein_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "enabled", "cooldown_ms", "startup_cooldown_ms",
                                             NULL };
   warn_unknown_keys(table, "audio.bargein", known_keys);

   PARSE_BOOL(table, "enabled", config->enabled);
   PARSE_INT(table, "cooldown_ms", config->cooldown_ms);
   PARSE_INT(table, "startup_cooldown_ms", config->startup_cooldown_ms);
}

/**
 * @brief Parse [[audio.named_devices]] array-of-tables
 *
 * Named devices allow voice command switching between audio devices,
 * e.g., "switch to microphone" or "use headphones".
 */
static void parse_audio_named_devices(toml_table_t *audio_table, audio_config_t *config) {
   toml_array_t *devices = toml_array_in(audio_table, "named_devices");
   if (!devices) {
      return; /* Optional section */
   }

   config->named_device_count = 0;
   int n = toml_array_nelem(devices);

   for (int i = 0; i < n && i < AUDIO_NAMED_DEVICE_MAX; i++) {
      toml_table_t *dev = toml_table_at(devices, i);
      if (!dev)
         continue;

      audio_named_device_t *nd = &config->named_devices[config->named_device_count];
      memset(nd, 0, sizeof(*nd));

      /* Parse required fields */
      PARSE_STRING(dev, "name", nd->name);
      PARSE_STRING(dev, "device", nd->device);

      /* Validate required fields */
      if (nd->name[0] == '\0' || nd->device[0] == '\0') {
         LOG_WARNING("Skipping audio.named_devices[%d]: missing name or device", i);
         continue;
      }

      /* Parse type (capture/playback) */
      toml_datum_t type_val = toml_string_in(dev, "type");
      if (type_val.ok) {
         if (strcmp(type_val.u.s, "capture") == 0) {
            nd->type = AUDIO_DEV_TYPE_CAPTURE;
         } else if (strcmp(type_val.u.s, "playback") == 0) {
            nd->type = AUDIO_DEV_TYPE_PLAYBACK;
         } else {
            LOG_WARNING("audio.named_devices[%d].type invalid '%s', defaulting to playback", i,
                        type_val.u.s);
            nd->type = AUDIO_DEV_TYPE_PLAYBACK;
         }
         free(type_val.u.s);
      } else {
         LOG_WARNING("audio.named_devices[%d] missing type, defaulting to playback", i);
         nd->type = AUDIO_DEV_TYPE_PLAYBACK;
      }

      /* Parse aliases array */
      toml_array_t *aliases = toml_array_in(dev, "aliases");
      if (aliases) {
         nd->alias_count = 0;
         int alias_n = toml_array_nelem(aliases);
         for (int j = 0; j < alias_n && j < AUDIO_DEVICE_ALIAS_MAX; j++) {
            toml_datum_t alias = toml_string_at(aliases, j);
            if (alias.ok) {
               safe_strncpy(nd->aliases[nd->alias_count++], alias.u.s, AUDIO_ALIAS_LEN);
               free(alias.u.s);
            }
         }
      }

      config->named_device_count++;
   }

   if (config->named_device_count > 0) {
      LOG_INFO("Parsed %d named audio devices from config", config->named_device_count);
   }
}

static void parse_audio(toml_table_t *table, audio_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "backend",         "capture_device",
                                             "playback_device", "output_rate",
                                             "output_channels", "bargein",
                                             "named_devices",   NULL };
   warn_unknown_keys(table, "audio", known_keys);

   PARSE_STRING(table, "backend", config->backend);
   PARSE_STRING(table, "capture_device", config->capture_device);
   PARSE_STRING(table, "playback_device", config->playback_device);
   PARSE_INT(table, "output_rate", config->output_rate);
   PARSE_INT(table, "output_channels", config->output_channels);

   /* Parse [audio.bargein] sub-table */
   toml_table_t *bargein = toml_table_in(table, "bargein");
   parse_audio_bargein(bargein, &config->bargein);

   /* Parse [[audio.named_devices]] array-of-tables */
   parse_audio_named_devices(table, config);
}

static void parse_vad_chunking(toml_table_t *table, vad_chunking_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "enabled", "pause_duration", "min_chunk_duration",
                                             "max_chunk_duration", NULL };
   warn_unknown_keys(table, "vad.chunking", known_keys);

   PARSE_BOOL(table, "enabled", config->enabled);
   PARSE_DOUBLE(table, "pause_duration", config->pause_duration);
   PARSE_DOUBLE(table, "min_chunk_duration", config->min_duration);
   PARSE_DOUBLE(table, "max_chunk_duration", config->max_duration);
}

static void parse_vad(toml_table_t *table, vad_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "speech_threshold",
                                             "speech_threshold_tts",
                                             "silence_threshold",
                                             "end_of_speech_duration",
                                             "max_recording_duration",
                                             "preroll_ms",
                                             "chunking",
                                             NULL };
   warn_unknown_keys(table, "vad", known_keys);

   PARSE_DOUBLE(table, "speech_threshold", config->speech_threshold);
   PARSE_DOUBLE(table, "speech_threshold_tts", config->speech_threshold_tts);
   PARSE_DOUBLE(table, "silence_threshold", config->silence_threshold);
   PARSE_DOUBLE(table, "end_of_speech_duration", config->end_of_speech_duration);
   PARSE_DOUBLE(table, "max_recording_duration", config->max_recording_duration);
   PARSE_INT(table, "preroll_ms", config->preroll_ms);

   /* Parse [vad.chunking] sub-table */
   toml_table_t *chunking = toml_table_in(table, "chunking");
   parse_vad_chunking(chunking, &config->chunking);
}

static void parse_asr(toml_table_t *table, asr_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "model", "models_path", NULL };
   warn_unknown_keys(table, "asr", known_keys);

   PARSE_STRING(table, "model", config->model);
   PARSE_STRING(table, "models_path", config->models_path);
}

static void parse_tts(toml_table_t *table, tts_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "models_path", "voice_model", "length_scale", NULL };
   warn_unknown_keys(table, "tts", known_keys);

   PARSE_STRING(table, "models_path", config->models_path);
   PARSE_STRING(table, "voice_model", config->voice_model);
   PARSE_DOUBLE(table, "length_scale", config->length_scale);
}

static void parse_commands(toml_table_t *table, commands_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "processing_mode", NULL };
   warn_unknown_keys(table, "commands", known_keys);

   PARSE_STRING(table, "processing_mode", config->processing_mode);
}

static void parse_llm_cloud(toml_table_t *table, llm_cloud_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "provider",
                                             "endpoint",
                                             "vision_enabled",
                                             "openai_models",
                                             "openai_default_model_idx",
                                             "claude_models",
                                             "claude_default_model_idx",
                                             "gemini_models",
                                             "gemini_default_model_idx",
                                             NULL };
   warn_unknown_keys(table, "llm.cloud", known_keys);

   PARSE_STRING(table, "provider", config->provider);
   PARSE_STRING(table, "endpoint", config->endpoint);
   PARSE_BOOL(table, "vision_enabled", config->vision_enabled);

   /* Parse openai_models array */
   toml_array_t *openai_arr = toml_array_in(table, "openai_models");
   if (openai_arr) {
      config->openai_models_count = 0;
      for (int i = 0; i < toml_array_nelem(openai_arr) && i < LLM_CLOUD_MAX_MODELS; i++) {
         toml_datum_t val = toml_string_at(openai_arr, i);
         if (val.ok) {
            safe_strncpy(config->openai_models[config->openai_models_count++], val.u.s,
                         LLM_CLOUD_MODEL_NAME_MAX);
            free(val.u.s);
         }
      }
   }

   /* Parse openai_default_model_idx with bounds check */
   toml_datum_t openai_idx = toml_int_in(table, "openai_default_model_idx");
   if (openai_idx.ok) {
      /* Check range before cast to avoid integer overflow */
      if (openai_idx.u.i < 0 || openai_idx.u.i > INT_MAX ||
          (int)openai_idx.u.i >= config->openai_models_count) {
         LOG_WARNING("llm.cloud.openai_default_model_idx out of range, defaulting to 0");
         config->openai_default_model_idx = 0;
      } else {
         config->openai_default_model_idx = (int)openai_idx.u.i;
      }
   }

   /* Parse claude_models array */
   toml_array_t *claude_arr = toml_array_in(table, "claude_models");
   if (claude_arr) {
      config->claude_models_count = 0;
      for (int i = 0; i < toml_array_nelem(claude_arr) && i < LLM_CLOUD_MAX_MODELS; i++) {
         toml_datum_t val = toml_string_at(claude_arr, i);
         if (val.ok) {
            safe_strncpy(config->claude_models[config->claude_models_count++], val.u.s,
                         LLM_CLOUD_MODEL_NAME_MAX);
            free(val.u.s);
         }
      }
   }

   /* Parse claude_default_model_idx with bounds check */
   toml_datum_t claude_idx = toml_int_in(table, "claude_default_model_idx");
   if (claude_idx.ok) {
      /* Check range before cast to avoid integer overflow */
      if (claude_idx.u.i < 0 || claude_idx.u.i > INT_MAX ||
          (int)claude_idx.u.i >= config->claude_models_count) {
         LOG_WARNING("llm.cloud.claude_default_model_idx out of range, defaulting to 0");
         config->claude_default_model_idx = 0;
      } else {
         config->claude_default_model_idx = (int)claude_idx.u.i;
      }
   }

   /* Parse gemini_models array */
   toml_array_t *gemini_arr = toml_array_in(table, "gemini_models");
   if (gemini_arr) {
      config->gemini_models_count = 0;
      for (int i = 0; i < toml_array_nelem(gemini_arr) && i < LLM_CLOUD_MAX_MODELS; i++) {
         toml_datum_t val = toml_string_at(gemini_arr, i);
         if (val.ok) {
            safe_strncpy(config->gemini_models[config->gemini_models_count++], val.u.s,
                         LLM_CLOUD_MODEL_NAME_MAX);
            free(val.u.s);
         }
      }
   }

   /* Parse gemini_default_model_idx with bounds check */
   toml_datum_t gemini_idx = toml_int_in(table, "gemini_default_model_idx");
   if (gemini_idx.ok) {
      /* Check range before cast to avoid integer overflow */
      if (gemini_idx.u.i < 0 || gemini_idx.u.i > INT_MAX ||
          (int)gemini_idx.u.i >= config->gemini_models_count) {
         LOG_WARNING("llm.cloud.gemini_default_model_idx out of range, defaulting to 0");
         config->gemini_default_model_idx = 0;
      } else {
         config->gemini_default_model_idx = (int)gemini_idx.u.i;
      }
   }
}

static void parse_llm_local(toml_table_t *table, llm_local_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "endpoint", "model", "vision_enabled", "provider",
                                             NULL };
   warn_unknown_keys(table, "llm.local", known_keys);

   PARSE_STRING(table, "endpoint", config->endpoint);
   PARSE_STRING(table, "model", config->model);
   PARSE_BOOL(table, "vision_enabled", config->vision_enabled);
   PARSE_STRING(table, "provider", config->provider);
}

static void parse_llm_tools(toml_table_t *table, llm_tools_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "mode", "native_enabled", "local_enabled",
                                             "remote_enabled", NULL };
   warn_unknown_keys(table, "llm.tools", known_keys);

   /* Parse mode (preferred) or fall back to native_enabled for backwards compatibility */
   PARSE_STRING(table, "mode", config->mode);

   /* Validate mode if set */
   if (config->mode[0] != '\0') {
      if (strcmp(config->mode, "native") != 0 && strcmp(config->mode, "command_tags") != 0 &&
          strcmp(config->mode, "disabled") != 0) {
         LOG_WARNING("Invalid llm.tools.mode '%s', using 'native'", config->mode);
         safe_strncpy(config->mode, "native", sizeof(config->mode));
      }
   } else {
      /* Backwards compatibility: convert native_enabled bool to mode */
      toml_datum_t native = toml_bool_in(table, "native_enabled");
      if (native.ok) {
         safe_strncpy(config->mode, native.u.b ? "native" : "command_tags", sizeof(config->mode));
      }
      /* If neither is set, default will be applied from config_defaults.c */
   }

   /* Parse local_enabled array */
   toml_array_t *local_arr = toml_array_in(table, "local_enabled");
   if (local_arr) {
      config->local_enabled_configured = true; /* Explicitly configured (even if empty) */
      config->local_enabled_count = 0;
      for (int i = 0; i < toml_array_nelem(local_arr) && i < LLM_TOOLS_MAX_CONFIGURED; i++) {
         toml_datum_t val = toml_string_at(local_arr, i);
         if (val.ok) {
            safe_strncpy(config->local_enabled[config->local_enabled_count++], val.u.s,
                         LLM_TOOL_NAME_MAX);
            free(val.u.s);
         }
      }
      LOG_INFO("Parsed %d tools in llm.tools.local_enabled", config->local_enabled_count);
   }

   /* Parse remote_enabled array */
   toml_array_t *remote_arr = toml_array_in(table, "remote_enabled");
   if (remote_arr) {
      config->remote_enabled_configured = true; /* Explicitly configured (even if empty) */
      config->remote_enabled_count = 0;
      for (int i = 0; i < toml_array_nelem(remote_arr) && i < LLM_TOOLS_MAX_CONFIGURED; i++) {
         toml_datum_t val = toml_string_at(remote_arr, i);
         if (val.ok) {
            safe_strncpy(config->remote_enabled[config->remote_enabled_count++], val.u.s,
                         LLM_TOOL_NAME_MAX);
            free(val.u.s);
         }
      }
      LOG_INFO("Parsed %d tools in llm.tools.remote_enabled", config->remote_enabled_count);
   }
}

static void parse_llm_thinking(toml_table_t *table, llm_thinking_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "mode",          "reasoning_effort", "budget_low",
                                             "budget_medium", "budget_high",      NULL };
   warn_unknown_keys(table, "llm.thinking", known_keys);

   PARSE_STRING(table, "mode", config->mode);
   PARSE_STRING(table, "reasoning_effort", config->reasoning_effort);
   PARSE_INT(table, "budget_low", config->budget_low);
   PARSE_INT(table, "budget_medium", config->budget_medium);
   PARSE_INT(table, "budget_high", config->budget_high);

   /* Validate mode (disabled, auto, enabled) */
   if (config->mode[0] != '\0' && strcmp(config->mode, "disabled") != 0 &&
       strcmp(config->mode, "auto") != 0 && strcmp(config->mode, "enabled") != 0) {
      LOG_WARNING("llm.thinking.mode invalid '%s', defaulting to 'disabled'", config->mode);
      strncpy(config->mode, "disabled", sizeof(config->mode) - 1);
      config->mode[sizeof(config->mode) - 1] = '\0';
   }

   /* Validate reasoning_effort (low, medium, high) */
   if (config->reasoning_effort[0] != '\0' && strcmp(config->reasoning_effort, "low") != 0 &&
       strcmp(config->reasoning_effort, "medium") != 0 &&
       strcmp(config->reasoning_effort, "high") != 0) {
      LOG_WARNING("llm.thinking.reasoning_effort invalid '%s', defaulting to 'medium'",
                  config->reasoning_effort);
      strncpy(config->reasoning_effort, "medium", sizeof(config->reasoning_effort) - 1);
      config->reasoning_effort[sizeof(config->reasoning_effort) - 1] = '\0';
   }

   /* Validate budget values (minimum 1024 tokens for Claude compatibility) */
   if (config->budget_low > 0 && config->budget_low < 1024) {
      LOG_WARNING("llm.thinking.budget_low too low (%d), clamping to 1024", config->budget_low);
      config->budget_low = 1024;
   }
   if (config->budget_medium > 0 && config->budget_medium < 1024) {
      LOG_WARNING("llm.thinking.budget_medium too low (%d), clamping to 1024",
                  config->budget_medium);
      config->budget_medium = 1024;
   }
   if (config->budget_high > 0 && config->budget_high < 1024) {
      LOG_WARNING("llm.thinking.budget_high too low (%d), clamping to 1024", config->budget_high);
      config->budget_high = 1024;
   }
}

static void parse_llm(toml_table_t *table, llm_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "type",
                                             "max_tokens",
                                             "summarize_threshold",
                                             "conversation_logging",
                                             "cloud",
                                             "local",
                                             "tools",
                                             "thinking",
                                             NULL };
   warn_unknown_keys(table, "llm", known_keys);

   PARSE_STRING(table, "type", config->type);
   PARSE_INT(table, "max_tokens", config->max_tokens);
   PARSE_DOUBLE(table, "summarize_threshold", config->summarize_threshold);
   PARSE_BOOL(table, "conversation_logging", config->conversation_logging);

   /* Parse [llm.cloud], [llm.local], [llm.tools], and [llm.thinking] sub-tables */
   toml_table_t *cloud = toml_table_in(table, "cloud");
   parse_llm_cloud(cloud, &config->cloud);

   toml_table_t *local = toml_table_in(table, "local");
   parse_llm_local(local, &config->local);

   toml_table_t *tools = toml_table_in(table, "tools");
   parse_llm_tools(tools, &config->tools);

   toml_table_t *thinking = toml_table_in(table, "thinking");
   parse_llm_thinking(thinking, &config->thinking);
}

static void parse_summarizer(toml_table_t *table, summarizer_file_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "backend", "threshold_bytes", "target_words",
                                             "target_ratio", NULL };
   warn_unknown_keys(table, "search.summarizer", known_keys);

   PARSE_STRING(table, "backend", config->backend);
   PARSE_SIZE_T(table, "threshold_bytes", config->threshold_bytes);
   PARSE_SIZE_T(table, "target_words", config->target_words);

   /* Parse target_ratio for TF-IDF summarization */
   toml_datum_t ratio = toml_double_in(table, "target_ratio");
   if (ratio.ok) {
      config->target_ratio = (float)ratio.u.d;
   }
}

static void parse_search(toml_table_t *table, search_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "engine", "endpoint", "summarizer", "title_filters",
                                             NULL };
   warn_unknown_keys(table, "search", known_keys);

   PARSE_STRING(table, "engine", config->engine);
   PARSE_STRING(table, "endpoint", config->endpoint);

   /* Parse [search.summarizer] sub-table */
   toml_table_t *summarizer = toml_table_in(table, "summarizer");
   parse_summarizer(summarizer, &config->summarizer);

   /* Parse title_filters array - exclude results with these terms in title (case-insensitive) */
   toml_array_t *filters_arr = toml_array_in(table, "title_filters");
   if (filters_arr) {
      int count = toml_array_nelem(filters_arr);
      if (count > SEARCH_MAX_TITLE_FILTERS) {
         LOG_WARNING("search.title_filters has %d entries, max is %d - truncating", count,
                     SEARCH_MAX_TITLE_FILTERS);
         count = SEARCH_MAX_TITLE_FILTERS;
      }

      /* Clear default filters when config specifies explicit list */
      config->title_filters_count = 0;

      for (int i = 0; i < count; i++) {
         toml_datum_t d = toml_string_at(filters_arr, i);
         if (d.ok && d.u.s) {
            strncpy(config->title_filters[config->title_filters_count], d.u.s,
                    SEARCH_TITLE_FILTER_MAX - 1);
            config->title_filters[config->title_filters_count][SEARCH_TITLE_FILTER_MAX - 1] = '\0';
            config->title_filters_count++;
            free(d.u.s);
         }
      }
      LOG_INFO("Parsed %d title filters in search.title_filters", config->title_filters_count);
   }
}

static void parse_flaresolverr(toml_table_t *table, flaresolverr_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "enabled", "endpoint", "timeout_sec",
                                             "max_response_bytes", NULL };
   warn_unknown_keys(table, "url_fetcher.flaresolverr", known_keys);

   PARSE_BOOL(table, "enabled", config->enabled);
   PARSE_STRING(table, "endpoint", config->endpoint);
   PARSE_INT(table, "timeout_sec", config->timeout_sec);
   PARSE_SIZE_T(table, "max_response_bytes", config->max_response_bytes);
}

static void parse_url_fetcher(toml_table_t *table, url_fetcher_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "whitelist", "flaresolverr", NULL };
   warn_unknown_keys(table, "url_fetcher", known_keys);

   /* Parse whitelist array into static 2D array */
   toml_array_t *whitelist_arr = toml_array_in(table, "whitelist");
   if (whitelist_arr) {
      int count = toml_array_nelem(whitelist_arr);
      if (count > URL_FETCHER_MAX_WHITELIST)
         count = URL_FETCHER_MAX_WHITELIST;

      for (int i = 0; i < count; i++) {
         toml_datum_t d = toml_string_at(whitelist_arr, i);
         if (d.ok) {
            strncpy(config->whitelist[i], d.u.s, URL_FETCHER_ENTRY_MAX - 1);
            config->whitelist[i][URL_FETCHER_ENTRY_MAX - 1] = '\0';
            config->whitelist_count++;
            free(d.u.s); /* Free tomlc99 allocated string */
         }
      }
   }

   /* Parse [url_fetcher.flaresolverr] sub-table */
   toml_table_t *flaresolverr = toml_table_in(table, "flaresolverr");
   parse_flaresolverr(flaresolverr, &config->flaresolverr);
}

static void parse_mqtt(toml_table_t *table, mqtt_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "enabled", "broker", "port", NULL };
   warn_unknown_keys(table, "mqtt", known_keys);

   PARSE_BOOL(table, "enabled", config->enabled);
   PARSE_STRING(table, "broker", config->broker);
   PARSE_INT(table, "port", config->port);
}

static void parse_network(toml_table_t *table, network_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = {
      "enabled",        "host", "port", "workers", "socket_timeout_sec", "session_timeout_sec",
      "llm_timeout_ms", NULL
   };
   warn_unknown_keys(table, "network", known_keys);

   PARSE_BOOL(table, "enabled", config->enabled);
   PARSE_STRING(table, "host", config->host);
   PARSE_INT(table, "port", config->port);
   PARSE_INT(table, "workers", config->workers);
   PARSE_INT(table, "socket_timeout_sec", config->socket_timeout_sec);
   PARSE_INT(table, "session_timeout_sec", config->session_timeout_sec);
   PARSE_INT(table, "llm_timeout_ms", config->llm_timeout_ms);
}

static void parse_tui(toml_table_t *table, tui_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "enabled", NULL };
   warn_unknown_keys(table, "tui", known_keys);

   PARSE_BOOL(table, "enabled", config->enabled);
}

static void parse_webui(toml_table_t *table, webui_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "enabled",        "port",    "max_clients",
                                             "audio_chunk_ms", "workers", "www_path",
                                             "bind_address",   "https",   "ssl_cert_path",
                                             "ssl_key_path",   NULL };
   warn_unknown_keys(table, "webui", known_keys);

   PARSE_BOOL(table, "enabled", config->enabled);
   PARSE_INT(table, "port", config->port);
   PARSE_INT(table, "max_clients", config->max_clients);
   PARSE_INT(table, "audio_chunk_ms", config->audio_chunk_ms);
   PARSE_INT(table, "workers", config->workers);
   PARSE_STRING(table, "www_path", config->www_path);
   PARSE_STRING(table, "bind_address", config->bind_address);
   PARSE_BOOL(table, "https", config->https);
   PARSE_STRING(table, "ssl_cert_path", config->ssl_cert_path);
   PARSE_STRING(table, "ssl_key_path", config->ssl_key_path);

   /* Clamp audio_chunk_ms to valid range */
   if (config->audio_chunk_ms < 100) {
      config->audio_chunk_ms = 100;
   } else if (config->audio_chunk_ms > 500) {
      config->audio_chunk_ms = 500;
   }

   /* Clamp workers to valid range (1 to WORKER_POOL_MAX_SIZE) */
   if (config->workers < 1) {
      config->workers = 1;
   } else if (config->workers > WORKER_POOL_MAX_SIZE) {
      config->workers = WORKER_POOL_MAX_SIZE;
   }
}

static void parse_images(toml_table_t *table, images_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "retention_days", "max_size_mb", "max_per_user",
                                             NULL };
   warn_unknown_keys(table, "images", known_keys);

   PARSE_INT(table, "retention_days", config->retention_days);
   PARSE_INT(table, "max_size_mb", config->max_size_mb);
   PARSE_INT(table, "max_per_user", config->max_per_user);

   /* Clamp retention_days to valid range (0 = never delete) */
   if (config->retention_days < 0) {
      config->retention_days = 0;
   }

   /* Clamp max_size_mb to valid range */
   if (config->max_size_mb < 1) {
      config->max_size_mb = 1;
   } else if (config->max_size_mb > 50) {
      config->max_size_mb = 50;
   }

   /* Clamp max_per_user to valid range */
   if (config->max_per_user < 1) {
      config->max_per_user = 1;
   } else if (config->max_per_user > 10000) {
      config->max_per_user = 10000;
   }
}

static void parse_memory(toml_table_t *table, memory_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "enabled",
                                             "context_budget_tokens",
                                             "extraction_provider",
                                             "extraction_model",
                                             "pruning_enabled",
                                             "prune_superseded_days",
                                             "prune_stale_days",
                                             "prune_stale_min_confidence",
                                             "conversation_idle_timeout_min",
                                             "default_voice_user_id",
                                             NULL };
   warn_unknown_keys(table, "memory", known_keys);

   PARSE_BOOL(table, "enabled", config->enabled);
   PARSE_INT(table, "context_budget_tokens", config->context_budget_tokens);
   PARSE_STRING(table, "extraction_provider", config->extraction_provider);
   PARSE_STRING(table, "extraction_model", config->extraction_model);

   /* Parse pruning settings - support both flat keys and [memory.pruning] sub-table */
   PARSE_BOOL(table, "pruning_enabled", config->pruning_enabled);
   PARSE_INT(table, "prune_superseded_days", config->prune_superseded_days);
   PARSE_INT(table, "prune_stale_days", config->prune_stale_days);
   PARSE_DOUBLE(table, "prune_stale_min_confidence", config->prune_stale_min_confidence);

   toml_table_t *pruning = toml_table_in(table, "pruning");
   if (pruning) {
      static const char *const pruning_keys[] = { "enabled", "superseded_days", "stale_days",
                                                  "stale_min_confidence", NULL };
      warn_unknown_keys(pruning, "memory.pruning", pruning_keys);

      PARSE_BOOL(pruning, "enabled", config->pruning_enabled);
      PARSE_INT(pruning, "superseded_days", config->prune_superseded_days);
      PARSE_INT(pruning, "stale_days", config->prune_stale_days);
      PARSE_DOUBLE(pruning, "stale_min_confidence", config->prune_stale_min_confidence);
   }

   /* Parse voice conversation idle timeout settings */
   PARSE_INT(table, "conversation_idle_timeout_min", config->conversation_idle_timeout_min);
   PARSE_INT(table, "default_voice_user_id", config->default_voice_user_id);

   /* Clamp context_budget_tokens to valid range */
   if (config->context_budget_tokens < 100) {
      config->context_budget_tokens = 100;
   } else if (config->context_budget_tokens > 2000) {
      config->context_budget_tokens = 2000;
   }

   /* Clamp pruning days to sensible values */
   if (config->prune_superseded_days < 1) {
      config->prune_superseded_days = 1;
   } else if (config->prune_superseded_days > 365) {
      config->prune_superseded_days = 365;
   }
   if (config->prune_stale_days < 7) {
      config->prune_stale_days = 7;
   } else if (config->prune_stale_days > 730) {
      config->prune_stale_days = 730;
   }
   if (config->prune_stale_min_confidence < 0.0f) {
      config->prune_stale_min_confidence = 0.0f;
   } else if (config->prune_stale_min_confidence > 1.0f) {
      config->prune_stale_min_confidence = 1.0f;
   }

   /* Clamp conversation idle timeout (0 = disabled, otherwise 10-60 min) */
   if (config->conversation_idle_timeout_min < 0) {
      config->conversation_idle_timeout_min = 0;
   } else if (config->conversation_idle_timeout_min > 0 &&
              config->conversation_idle_timeout_min < 10) {
      config->conversation_idle_timeout_min = 10;
   } else if (config->conversation_idle_timeout_min > 60) {
      config->conversation_idle_timeout_min = 60;
   }

   /* Default voice user ID must be positive */
   if (config->default_voice_user_id < 1) {
      config->default_voice_user_id = 1;
   }
}

static void parse_shutdown(toml_table_t *table, shutdown_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "enabled", "passphrase", NULL };
   warn_unknown_keys(table, "shutdown", known_keys);

   PARSE_BOOL(table, "enabled", config->enabled);
   PARSE_STRING(table, "passphrase", config->passphrase);
}

static void parse_debug(toml_table_t *table, debug_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "mic_record", "asr_record", "aec_record",
                                             "record_path", NULL };
   warn_unknown_keys(table, "debug", known_keys);

   PARSE_BOOL(table, "mic_record", config->mic_record);
   PARSE_BOOL(table, "asr_record", config->asr_record);
   PARSE_BOOL(table, "aec_record", config->aec_record);
   PARSE_STRING(table, "record_path", config->record_path);
}

static void parse_paths(toml_table_t *table, paths_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "data_dir", "music_dir", NULL };
   warn_unknown_keys(table, "paths", known_keys);

   PARSE_STRING(table, "data_dir", config->data_dir);
   PARSE_STRING(table, "music_dir", config->music_dir);
}

static void parse_music(toml_table_t *table, music_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "scan_interval_minutes", "streaming", NULL };
   warn_unknown_keys(table, "music", known_keys);

   PARSE_INT(table, "scan_interval_minutes", config->scan_interval_minutes);

   /* Parse music.streaming subtable */
   toml_table_t *streaming = toml_table_in(table, "streaming");
   if (streaming) {
      static const char *const streaming_keys[] = { "enabled", "default_quality", "bitrate_mode",
                                                    NULL };
      warn_unknown_keys(streaming, "music.streaming", streaming_keys);

      PARSE_BOOL(streaming, "enabled", config->streaming_enabled);
      PARSE_STRING(streaming, "default_quality", config->streaming_quality);
      PARSE_STRING(streaming, "bitrate_mode", config->streaming_bitrate_mode);
   }
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int config_file_readable(const char *path) {
   if (!path || path[0] == '\0')
      return 0;

   struct stat st;
   if (stat(path, &st) != 0)
      return 0;

   /* Check it's a regular file and readable */
   if (!S_ISREG(st.st_mode))
      return 0;

   if (access(path, R_OK) != 0)
      return 0;

   return 1;
}

int config_parse_file(const char *path, dawn_config_t *config) {
   if (!path || !config) {
      LOG_ERROR("config_parse_file: NULL argument");
      return FAILURE;
   }

   FILE *fp = fopen(path, "r");
   if (!fp) {
      LOG_ERROR("Failed to open config file: %s", path);
      return FAILURE;
   }

   char errbuf[256];
   toml_table_t *root = toml_parse_file(fp, errbuf, sizeof(errbuf));
   fclose(fp);

   if (!root) {
      LOG_ERROR("Failed to parse config file %s: %s", path, errbuf);
      return FAILURE;
   }

   /* Parse all sections */
   parse_general(toml_table_in(root, "general"), &config->general);
   parse_persona(toml_table_in(root, "persona"), &config->persona);
   parse_localization(toml_table_in(root, "localization"), &config->localization);
   parse_audio(toml_table_in(root, "audio"), &config->audio);
   parse_vad(toml_table_in(root, "vad"), &config->vad);
   parse_asr(toml_table_in(root, "asr"), &config->asr);
   parse_tts(toml_table_in(root, "tts"), &config->tts);
   parse_commands(toml_table_in(root, "commands"), &config->commands);
   parse_llm(toml_table_in(root, "llm"), &config->llm);
   parse_search(toml_table_in(root, "search"), &config->search);
   parse_url_fetcher(toml_table_in(root, "url_fetcher"), &config->url_fetcher);
   parse_mqtt(toml_table_in(root, "mqtt"), &config->mqtt);
   parse_network(toml_table_in(root, "network"), &config->network);
   parse_tui(toml_table_in(root, "tui"), &config->tui);
   parse_webui(toml_table_in(root, "webui"), &config->webui);
   parse_images(toml_table_in(root, "images"), &config->images);
   parse_memory(toml_table_in(root, "memory"), &config->memory);
   parse_shutdown(toml_table_in(root, "shutdown"), &config->shutdown);
   parse_debug(toml_table_in(root, "debug"), &config->debug);
   parse_paths(toml_table_in(root, "paths"), &config->paths);
   parse_music(toml_table_in(root, "music"), &config->music);

   toml_free(root);

   LOG_INFO("Loaded configuration from: %s", path);
   return SUCCESS;
}

int config_parse_secrets(const char *path, secrets_config_t *secrets) {
   if (!path || !secrets) {
      LOG_ERROR("config_parse_secrets: NULL argument");
      return FAILURE;
   }

   /* Check file permissions before loading - warn if too permissive */
   check_sensitive_file_permissions(path, "Secrets file");

   FILE *fp = fopen(path, "r");
   if (!fp) {
      LOG_WARNING("Secrets file not found: %s", path);
      return FAILURE;
   }

   char errbuf[256];
   toml_table_t *root = toml_parse_file(fp, errbuf, sizeof(errbuf));
   fclose(fp);

   if (!root) {
      LOG_ERROR("Failed to parse secrets file %s: %s", path, errbuf);
      return FAILURE;
   }

   /* Parse [secrets] section (WebUI format) */
   toml_table_t *secrets_section = toml_table_in(root, "secrets");
   if (secrets_section) {
      PARSE_STRING(secrets_section, "openai_api_key", secrets->openai_api_key);
      PARSE_STRING(secrets_section, "claude_api_key", secrets->claude_api_key);
      PARSE_STRING(secrets_section, "gemini_api_key", secrets->gemini_api_key);
      PARSE_STRING(secrets_section, "mqtt_username", secrets->mqtt_username);
      PARSE_STRING(secrets_section, "mqtt_password", secrets->mqtt_password);

      /* Parse [secrets.smartthings] sub-section for authentication
       * Supports two modes:
       * 1. PAT mode: access_token only (simpler, recommended)
       * 2. OAuth2 mode: client_id + client_secret */
      toml_table_t *smartthings = toml_table_in(secrets_section, "smartthings");
      if (smartthings) {
         PARSE_STRING(smartthings, "access_token", secrets->smartthings_access_token);
         PARSE_STRING(smartthings, "client_id", secrets->smartthings_client_id);
         PARSE_STRING(smartthings, "client_secret", secrets->smartthings_client_secret);
      }
   }

   /* Legacy: Parse [api_keys] section (old format) */
   toml_table_t *api_keys = toml_table_in(root, "api_keys");
   if (api_keys) {
      /* Only parse if not already set from [secrets] */
      if (secrets->openai_api_key[0] == '\0')
         PARSE_STRING(api_keys, "openai", secrets->openai_api_key);
      if (secrets->claude_api_key[0] == '\0')
         PARSE_STRING(api_keys, "claude", secrets->claude_api_key);
   }

   toml_free(root);

   LOG_INFO("Loaded secrets from: %s", path);
   return SUCCESS;
}

/**
 * @brief Get user's home directory
 */
static const char *get_home_dir(void) {
   const char *home = getenv("HOME");
   if (home)
      return home;

   struct passwd *pw = getpwuid(getuid());
   if (pw)
      return pw->pw_dir;

   return NULL;
}

int config_load_from_search(const char *explicit_path, dawn_config_t *config) {
   if (!config)
      return FAILURE;

   int result;

   /* Priority 1: Explicit path from --config */
   if (explicit_path && explicit_path[0] != '\0') {
      if (config_file_readable(explicit_path)) {
         result = config_parse_file(explicit_path, config);
         if (result == SUCCESS) {
            strncpy(s_loaded_config_path, explicit_path, sizeof(s_loaded_config_path) - 1);
            s_loaded_config_path[sizeof(s_loaded_config_path) - 1] = '\0';
            LOG_INFO("Config loaded: %s", s_loaded_config_path);
         }
         return result;
      } else {
         LOG_ERROR("Specified config file not found: %s", explicit_path);
         return FAILURE;
      }
   }

   /* Priority 2: ./dawn.toml */
   if (config_file_readable("./dawn.toml")) {
      result = config_parse_file("./dawn.toml", config);
      if (result == SUCCESS) {
         strncpy(s_loaded_config_path, "./dawn.toml", sizeof(s_loaded_config_path) - 1);
         LOG_INFO("Config loaded: %s", s_loaded_config_path);
      }
      return result;
   }

   /* Priority 3: ~/.config/dawn/config.toml */
   const char *home = get_home_dir();
   if (home) {
      char path[CONFIG_PATH_MAX];
      snprintf(path, sizeof(path), "%s/.config/dawn/config.toml", home);
      if (config_file_readable(path)) {
         result = config_parse_file(path, config);
         if (result == SUCCESS) {
            strncpy(s_loaded_config_path, path, sizeof(s_loaded_config_path) - 1);
            s_loaded_config_path[sizeof(s_loaded_config_path) - 1] = '\0';
            LOG_INFO("Config loaded: %s", s_loaded_config_path);
         }
         return result;
      }
   }

   /* Priority 4: /etc/dawn/config.toml */
   if (config_file_readable("/etc/dawn/config.toml")) {
      result = config_parse_file("/etc/dawn/config.toml", config);
      if (result == SUCCESS) {
         strncpy(s_loaded_config_path, "/etc/dawn/config.toml", sizeof(s_loaded_config_path) - 1);
         LOG_INFO("Config loaded: %s", s_loaded_config_path);
      }
      return result;
   }

   /* No config file found - use defaults (not an error) */
   LOG_INFO("No config file found, using defaults");
   return SUCCESS;
}

int config_load_secrets_from_search(secrets_config_t *secrets) {
   if (!secrets)
      return FAILURE;

   int result;

   /* Priority 1: ./secrets.toml (current directory) */
   if (config_file_readable("./secrets.toml")) {
      result = config_parse_secrets("./secrets.toml", secrets);
      if (result == SUCCESS) {
         strncpy(s_loaded_secrets_path, "./secrets.toml", sizeof(s_loaded_secrets_path) - 1);
         LOG_INFO("Secrets loaded: %s", s_loaded_secrets_path);
      }
      return result;
   }

   /* Priority 2: ~/.config/dawn/secrets.toml (user-specific) */
   const char *home = get_home_dir();
   if (home) {
      char path[CONFIG_PATH_MAX];
      snprintf(path, sizeof(path), "%s/.config/dawn/secrets.toml", home);
      if (config_file_readable(path)) {
         result = config_parse_secrets(path, secrets);
         if (result == SUCCESS) {
            strncpy(s_loaded_secrets_path, path, sizeof(s_loaded_secrets_path) - 1);
            s_loaded_secrets_path[sizeof(s_loaded_secrets_path) - 1] = '\0';
            LOG_INFO("Secrets loaded: %s", s_loaded_secrets_path);
         }
         return result;
      }
   }

   /* Priority 3: /etc/dawn/secrets.toml (system-wide) */
   if (config_file_readable("/etc/dawn/secrets.toml")) {
      result = config_parse_secrets("/etc/dawn/secrets.toml", secrets);
      if (result == SUCCESS) {
         strncpy(s_loaded_secrets_path, "/etc/dawn/secrets.toml",
                 sizeof(s_loaded_secrets_path) - 1);
         LOG_INFO("Secrets loaded: %s", s_loaded_secrets_path);
      }
      return result;
   }

   /* Secrets file not found - not an error, secrets are optional */
   LOG_INFO("No secrets file found");
   return SUCCESS;
}

const char *config_get_loaded_path(void) {
   if (s_loaded_config_path[0] != '\0')
      return s_loaded_config_path;
   return "(none - using defaults)";
}

const char *config_get_secrets_path(void) {
   if (s_loaded_secrets_path[0] != '\0')
      return s_loaded_secrets_path;
   return "(none)";
}

int config_backup_file(const char *path) {
   if (!path || path[0] == '\0')
      return 1;

   /* Check if original file exists */
   struct stat st;
   if (stat(path, &st) != 0) {
      /* File doesn't exist, nothing to backup */
      return 0;
   }

   /* Create backup path (.bak extension) */
   char backup_path[CONFIG_PATH_MAX];
   int len = snprintf(backup_path, sizeof(backup_path), "%s.bak", path);
   if (len < 0 || (size_t)len >= sizeof(backup_path)) {
      LOG_ERROR("Backup path too long for: %s", path);
      return 1;
   }

   /* Read original file */
   FILE *src = fopen(path, "rb");
   if (!src) {
      LOG_ERROR("Failed to open file for backup: %s", path);
      return 1;
   }

   /* Create backup file with restrictive permissions from the start.
    * Use open() with explicit mode to avoid race condition where file
    * is briefly world-readable before chmod(). Config/secrets backups
    * should always be owner-only (0600) regardless of original perms. */
   int fd = open(backup_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0) {
      LOG_ERROR("Failed to create backup file: %s", backup_path);
      fclose(src);
      return 1;
   }

   FILE *dst = fdopen(fd, "wb");
   if (!dst) {
      LOG_ERROR("Failed to open backup file stream: %s", backup_path);
      close(fd);
      fclose(src);
      return 1;
   }

   /* Copy contents */
   char buffer[4096];
   size_t bytes;
   while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
      if (fwrite(buffer, 1, bytes, dst) != bytes) {
         LOG_ERROR("Failed to write backup file: %s", backup_path);
         fclose(src);
         fclose(dst);
         return 1;
      }
   }

   fclose(src);
   fclose(dst);

   LOG_INFO("Created backup: %s", backup_path);
   return 0;
}
