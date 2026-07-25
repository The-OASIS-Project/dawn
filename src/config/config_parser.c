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

#include "dawn_error.h"
#include "logging.h"
#include "tools/toml.h"
#include "utils/string_utils.h"

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
      OLOG_WARNING("========================================");
      OLOG_WARNING("SECURITY WARNING: %s is world-readable!", file_description);
      OLOG_WARNING("File: %s", path);
      OLOG_WARNING("This exposes sensitive data to all users on the system.");
      OLOG_WARNING("Fix with: chmod 600 %s", path);
      OLOG_WARNING("========================================");
   }

   if (st.st_mode & S_IWOTH) {
      OLOG_WARNING("========================================");
      OLOG_WARNING("SECURITY WARNING: %s is world-writable!", file_description);
      OLOG_WARNING("File: %s", path);
      OLOG_WARNING("Any user on the system can modify this file!");
      OLOG_WARNING("Fix with: chmod 600 %s", path);
      OLOG_WARNING("========================================");
   }

   /* Check for group-readable/writable (less critical but still a concern) */
   if (st.st_mode & S_IRGRP) {
      OLOG_WARNING("Security notice: %s is group-readable (%s)", file_description, path);
      OLOG_WARNING("Consider: chmod 600 %s", path);
   }

   if (st.st_mode & S_IWGRP) {
      OLOG_WARNING("Security notice: %s is group-writable (%s)", file_description, path);
      OLOG_WARNING("Consider: chmod 600 %s", path);
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
         OLOG_WARNING("Unknown config key [%s].%s (typo?)", section, key);
      }
   }
}

/* =============================================================================
 * Section Parsers
 * ============================================================================= */

static void parse_general(toml_table_t *table, general_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "ai_name", "log_file", "room", "mode", NULL };
   warn_unknown_keys(table, "general", known_keys);

   PARSE_STRING(table, "ai_name", config->ai_name);
   PARSE_STRING(table, "log_file", config->log_file);
   PARSE_STRING(table, "room", config->room);
   PARSE_STRING(table, "mode", config->mode);
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
         OLOG_WARNING("Skipping audio.named_devices[%d]: missing name or device", i);
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
            OLOG_WARNING("audio.named_devices[%d].type invalid '%s', defaulting to playback", i,
                         type_val.u.s);
            nd->type = AUDIO_DEV_TYPE_PLAYBACK;
         }
         free(type_val.u.s);
      } else {
         OLOG_WARNING("audio.named_devices[%d] missing type, defaulting to playback", i);
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
      OLOG_INFO("Parsed %d named audio devices from config", config->named_device_count);
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

   static const char *const known_keys[] = { "model", "models_path", "dedup_window_sec",
                                             "disambiguation_hint", NULL };
   warn_unknown_keys(table, "asr", known_keys);

   PARSE_STRING(table, "model", config->model);
   PARSE_STRING(table, "models_path", config->models_path);
   PARSE_INT(table, "dedup_window_sec", config->dedup_window_sec);
   PARSE_STRING(table, "disambiguation_hint", config->disambiguation_hint);
}

static void parse_tts(toml_table_t *table, tts_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "models_path",           "voice_model",
                                             "length_scale",          "voice_directive",
                                             "voice_directive_webui", NULL };
   warn_unknown_keys(table, "tts", known_keys);

   PARSE_STRING(table, "models_path", config->models_path);
   PARSE_STRING(table, "voice_model", config->voice_model);
   PARSE_DOUBLE(table, "length_scale", config->length_scale);
   PARSE_STRING(table, "voice_directive", config->voice_directive);
   PARSE_STRING(table, "voice_directive_webui", config->voice_directive_webui);
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
                                             "use_openrouter",
                                             "openai_use_responses_api",
                                             "openai_models",
                                             "openai_default_model_idx",
                                             "claude_models",
                                             "claude_default_model_idx",
                                             "gemini_models",
                                             "gemini_default_model_idx",
                                             "openrouter_models",
                                             "openrouter_default_model_idx",
                                             NULL };
   warn_unknown_keys(table, "llm.cloud", known_keys);

   PARSE_STRING(table, "provider", config->provider);
   PARSE_STRING(table, "endpoint", config->endpoint);
   PARSE_BOOL(table, "vision_enabled", config->vision_enabled);
   PARSE_BOOL(table, "use_openrouter", config->use_openrouter);
   PARSE_STRING(table, "openai_use_responses_api", config->openai_use_responses_api);

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
         OLOG_WARNING("llm.cloud.openai_default_model_idx out of range, defaulting to 0");
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
         OLOG_WARNING("llm.cloud.claude_default_model_idx out of range, defaulting to 0");
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
         OLOG_WARNING("llm.cloud.gemini_default_model_idx out of range, defaulting to 0");
         config->gemini_default_model_idx = 0;
      } else {
         config->gemini_default_model_idx = (int)gemini_idx.u.i;
      }
   }

   /* Parse openrouter_models array */
   toml_array_t *openrouter_arr = toml_array_in(table, "openrouter_models");
   if (openrouter_arr) {
      config->openrouter_models_count = 0;
      for (int i = 0; i < toml_array_nelem(openrouter_arr) && i < LLM_CLOUD_MAX_MODELS; i++) {
         toml_datum_t val = toml_string_at(openrouter_arr, i);
         if (val.ok) {
            safe_strncpy(config->openrouter_models[config->openrouter_models_count++], val.u.s,
                         LLM_CLOUD_MODEL_NAME_MAX);
            free(val.u.s);
         }
      }
   }

   /* Parse openrouter_default_model_idx with bounds check */
   toml_datum_t openrouter_idx = toml_int_in(table, "openrouter_default_model_idx");
   if (openrouter_idx.ok) {
      /* Check range before cast to avoid integer overflow */
      if (openrouter_idx.u.i < 0 || openrouter_idx.u.i > INT_MAX ||
          (int)openrouter_idx.u.i >= config->openrouter_models_count) {
         OLOG_WARNING("llm.cloud.openrouter_default_model_idx out of range, defaulting to 0");
         config->openrouter_default_model_idx = 0;
      } else {
         config->openrouter_default_model_idx = (int)openrouter_idx.u.i;
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

   static const char *const known_keys[] = { "mode",
                                             "native_enabled",
                                             "local_enabled",
                                             "remote_enabled",
                                             "local_disabled",
                                             "remote_disabled",
                                             NULL };
   warn_unknown_keys(table, "llm.tools", known_keys);

   /* Parse mode (preferred) or fall back to native_enabled for backwards compatibility */
   PARSE_STRING(table, "mode", config->mode);

   /* Validate mode if set */
   if (config->mode[0] != '\0') {
      if (strcmp(config->mode, "native") != 0 && strcmp(config->mode, "command_tags") != 0 &&
          strcmp(config->mode, "disabled") != 0) {
         OLOG_WARNING("Invalid llm.tools.mode '%s', using 'native'", config->mode);
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
      OLOG_INFO("Parsed %d tools in llm.tools.local_enabled", config->local_enabled_count);
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
      OLOG_INFO("Parsed %d tools in llm.tools.remote_enabled", config->remote_enabled_count);
   }

   /* Parse local_disabled array (blocklist) */
   toml_array_t *local_dis = toml_array_in(table, "local_disabled");
   if (local_dis) {
      config->local_disabled_configured = true;
      config->local_disabled_count = 0;
      if (toml_array_nelem(local_dis) > LLM_TOOLS_MAX_CONFIGURED)
         OLOG_WARNING("llm.tools.local_disabled has %d entries; only %d honored (blocklist fails "
                      "OPEN — dropped tools stay ENABLED)",
                      toml_array_nelem(local_dis), LLM_TOOLS_MAX_CONFIGURED);
      for (int i = 0; i < toml_array_nelem(local_dis) && i < LLM_TOOLS_MAX_CONFIGURED; i++) {
         toml_datum_t val = toml_string_at(local_dis, i);
         if (val.ok) {
            safe_strncpy(config->local_disabled[config->local_disabled_count++], val.u.s,
                         LLM_TOOL_NAME_MAX);
            free(val.u.s);
         }
      }
      OLOG_INFO("Parsed %d tools in llm.tools.local_disabled", config->local_disabled_count);
   }

   /* Parse remote_disabled array (blocklist) */
   toml_array_t *remote_dis = toml_array_in(table, "remote_disabled");
   if (remote_dis) {
      config->remote_disabled_configured = true;
      config->remote_disabled_count = 0;
      if (toml_array_nelem(remote_dis) > LLM_TOOLS_MAX_CONFIGURED)
         OLOG_WARNING("llm.tools.remote_disabled has %d entries; only %d honored (blocklist fails "
                      "OPEN — dropped tools stay ENABLED)",
                      toml_array_nelem(remote_dis), LLM_TOOLS_MAX_CONFIGURED);
      for (int i = 0; i < toml_array_nelem(remote_dis) && i < LLM_TOOLS_MAX_CONFIGURED; i++) {
         toml_datum_t val = toml_string_at(remote_dis, i);
         if (val.ok) {
            safe_strncpy(config->remote_disabled[config->remote_disabled_count++], val.u.s,
                         LLM_TOOL_NAME_MAX);
            free(val.u.s);
         }
      }
      OLOG_INFO("Parsed %d tools in llm.tools.remote_disabled", config->remote_disabled_count);
   }
}

static void parse_llm_thinking(toml_table_t *table, llm_thinking_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = {
      "mode", "reasoning_effort", "budget_low", "budget_medium", "budget_high", "budget_xhigh", NULL
   };
   warn_unknown_keys(table, "llm.thinking", known_keys);

   PARSE_STRING(table, "mode", config->mode);
   PARSE_STRING(table, "reasoning_effort", config->reasoning_effort);
   PARSE_INT(table, "budget_low", config->budget_low);
   PARSE_INT(table, "budget_medium", config->budget_medium);
   PARSE_INT(table, "budget_high", config->budget_high);
   PARSE_INT(table, "budget_xhigh", config->budget_xhigh);

   /* Validate mode. "auto" is accepted for back-compat with legacy DB rows
    * and pre-cleanup configs; treated identically to "enabled" by all providers. */
   if (config->mode[0] != '\0' && strcmp(config->mode, "disabled") != 0 &&
       strcmp(config->mode, "auto") != 0 && strcmp(config->mode, "enabled") != 0) {
      OLOG_WARNING("llm.thinking.mode invalid '%s', defaulting to 'disabled'", config->mode);
      strncpy(config->mode, "disabled", sizeof(config->mode) - 1);
      config->mode[sizeof(config->mode) - 1] = '\0';
   }

   /* Validate reasoning_effort. Full set: none/low/medium/high/xhigh.
    * none and xhigh are gpt-5.4-only at the API level — older OpenAI models
    * and Gemini OpenAI-compat get clamped at request build time. Claude
    * maps any of these to a budget via budget_low/medium/high/xhigh. */
   if (config->reasoning_effort[0] != '\0' && strcmp(config->reasoning_effort, "none") != 0 &&
       strcmp(config->reasoning_effort, "low") != 0 &&
       strcmp(config->reasoning_effort, "medium") != 0 &&
       strcmp(config->reasoning_effort, "high") != 0 &&
       strcmp(config->reasoning_effort, "xhigh") != 0) {
      OLOG_WARNING("llm.thinking.reasoning_effort invalid '%s', defaulting to 'medium'",
                   config->reasoning_effort);
      strncpy(config->reasoning_effort, "medium", sizeof(config->reasoning_effort) - 1);
      config->reasoning_effort[sizeof(config->reasoning_effort) - 1] = '\0';
   }

   /* Validate budget values (minimum 1024 tokens for Claude compatibility) */
   if (config->budget_low > 0 && config->budget_low < 1024) {
      OLOG_WARNING("llm.thinking.budget_low too low (%d), clamping to 1024", config->budget_low);
      config->budget_low = 1024;
   }
   if (config->budget_medium > 0 && config->budget_medium < 1024) {
      OLOG_WARNING("llm.thinking.budget_medium too low (%d), clamping to 1024",
                   config->budget_medium);
      config->budget_medium = 1024;
   }
   if (config->budget_high > 0 && config->budget_high < 1024) {
      OLOG_WARNING("llm.thinking.budget_high too low (%d), clamping to 1024", config->budget_high);
      config->budget_high = 1024;
   }
   if (config->budget_xhigh > 0 && config->budget_xhigh < 1024) {
      OLOG_WARNING("llm.thinking.budget_xhigh too low (%d), clamping to 1024",
                   config->budget_xhigh);
      config->budget_xhigh = 1024;
   }
}

static void parse_llm(toml_table_t *table, llm_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "type",
                                             "max_tokens",
                                             "summarize_threshold",
                                             "compact_soft_threshold",
                                             "compact_hard_threshold",
                                             "compact_use_session",
                                             "compact_provider",
                                             "compact_model",
                                             "compact_openrouter_model",
                                             "conversation_logging",
                                             "rate_limit_enabled",
                                             "rate_limit_rpm",
                                             "cloud",
                                             "local",
                                             "tools",
                                             "thinking",
                                             "silent_observe",
                                             NULL };
   warn_unknown_keys(table, "llm", known_keys);

   PARSE_STRING(table, "type", config->type);
   PARSE_INT(table, "max_tokens", config->max_tokens);
   bool has_summarize = false;
   {
      toml_datum_t st = toml_double_in(table, "summarize_threshold");
      if (st.ok) {
         config->summarize_threshold = (float)st.u.d;
         has_summarize = true;
      }
   }

   /* Parse dual compaction thresholds */
   bool has_soft = false, has_hard = false;
   toml_datum_t soft_d = toml_double_in(table, "compact_soft_threshold");
   if (soft_d.ok) {
      config->compact_soft_threshold = (float)soft_d.u.d;
      has_soft = true;
   }
   toml_datum_t hard_d = toml_double_in(table, "compact_hard_threshold");
   if (hard_d.ok) {
      config->compact_hard_threshold = (float)hard_d.u.d;
      has_hard = true;
   }

   /* Backward compat: derive dual thresholds from legacy summarize_threshold.
    * For low test values (e.g. 0.15), set soft just below hard to keep both active. */
   if (!has_soft && !has_hard && has_summarize) {
      float legacy = config->summarize_threshold;
      config->compact_hard_threshold = legacy;
      float soft = legacy - 0.25f;
      if (soft < 0.30f)
         soft = 0.30f;
      if (soft >= legacy)
         soft = legacy - 0.05f;
      config->compact_soft_threshold = soft;
   }

   PARSE_BOOL(table, "compact_use_session", config->compact_use_session);
   PARSE_STRING(table, "compact_provider", config->compact_provider);
   PARSE_STRING(table, "compact_model", config->compact_model);
   PARSE_STRING(table, "compact_openrouter_model", config->compact_openrouter_model);

   PARSE_BOOL(table, "conversation_logging", config->conversation_logging);
   PARSE_BOOL(table, "rate_limit_enabled", config->rate_limit_enabled);
   PARSE_INT(table, "rate_limit_rpm", config->rate_limit_rpm);

   /* Parse [llm.cloud], [llm.local], [llm.tools], and [llm.thinking] sub-tables */
   toml_table_t *cloud = toml_table_in(table, "cloud");
   parse_llm_cloud(cloud, &config->cloud);

   toml_table_t *local = toml_table_in(table, "local");
   parse_llm_local(local, &config->local);

   toml_table_t *tools = toml_table_in(table, "tools");
   parse_llm_tools(tools, &config->tools);

   toml_table_t *thinking = toml_table_in(table, "thinking");
   parse_llm_thinking(thinking, &config->thinking);

   /* [llm.silent_observe] — Phase 0 of Dynamic Context Injection */
   toml_table_t *silent = toml_table_in(table, "silent_observe");
   if (silent) {
      static const char *const silent_keys[] = { "provider", "model", "openrouter_model", NULL };
      warn_unknown_keys(silent, "llm.silent_observe", silent_keys);
      PARSE_STRING(silent, "provider", config->silent_observe.provider);
      PARSE_STRING(silent, "model", config->silent_observe.model);
      PARSE_STRING(silent, "openrouter_model", config->silent_observe.openrouter_model);
   }
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
         OLOG_WARNING("search.title_filters has %d entries, max is %d - truncating", count,
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
      OLOG_INFO("Parsed %d title filters in search.title_filters", config->title_filters_count);
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

static void parse_url_fetcher_tavily(toml_table_t *table, tavily_fetch_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "timeout_sec",
                                             "max_response_bytes",
                                             "extract_depth",
                                             "rate_limit_per_minute",
                                             "rate_limit_per_hour",
                                             "rate_limit_per_day",
                                             NULL };
   warn_unknown_keys(table, "url_fetcher.tavily", known_keys);

   PARSE_INT(table, "timeout_sec", config->timeout_sec);
   PARSE_SIZE_T(table, "max_response_bytes", config->max_response_bytes);
   PARSE_STRING(table, "extract_depth", config->extract_depth);
   PARSE_INT(table, "rate_limit_per_minute", config->rate_limit_per_minute);
   PARSE_INT(table, "rate_limit_per_hour", config->rate_limit_per_hour);
   PARSE_INT(table, "rate_limit_per_day", config->rate_limit_per_day);
}

static void parse_url_fetcher(toml_table_t *table, url_fetcher_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "whitelist", "fallback", "flaresolverr", "tavily",
                                             NULL };
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

   /* Fallback engine selection: "flaresolverr" | "tavily" | "none" */
   PARSE_STRING(table, "fallback", config->fallback);

   /* Parse [url_fetcher.flaresolverr] sub-table */
   toml_table_t *flaresolverr = toml_table_in(table, "flaresolverr");
   parse_flaresolverr(flaresolverr, &config->flaresolverr);

   /* Parse [url_fetcher.tavily] sub-table (tunables; API key in secrets.toml) */
   toml_table_t *tavily = toml_table_in(table, "tavily");
   parse_url_fetcher_tavily(tavily, &config->tavily);
}

static void parse_mqtt(toml_table_t *table, mqtt_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "enabled",     "broker",        "port",         "tls",
                                             "tls_ca_cert", "tls_cert_path", "tls_key_path", NULL };
   warn_unknown_keys(table, "mqtt", known_keys);

   PARSE_BOOL(table, "enabled", config->enabled);
   PARSE_STRING(table, "broker", config->broker);
   PARSE_INT(table, "port", config->port);
   PARSE_BOOL(table, "tls", config->tls);
   PARSE_STRING(table, "tls_ca_cert", config->tls_ca_cert);
   PARSE_STRING(table, "tls_cert_path", config->tls_cert_path);
   PARSE_STRING(table, "tls_key_path", config->tls_key_path);
}

static void parse_network(toml_table_t *table, network_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "workers", "session_timeout_sec", "llm_timeout_ms",
                                             "summarization_timeout_ms", NULL };
   warn_unknown_keys(table, "network", known_keys);

   PARSE_INT(table, "workers", config->workers);
   PARSE_INT(table, "session_timeout_sec", config->session_timeout_sec);
   PARSE_INT(table, "llm_timeout_ms", config->llm_timeout_ms);
   PARSE_INT(table, "summarization_timeout_ms", config->summarization_timeout_ms);
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

   static const char *const known_keys[] = {
      "enabled",       "port",  "max_clients",   "audio_chunk_ms", "www_path",
      "bind_address",  "https", "ssl_cert_path", "ssl_key_path",   "export_max_messages",
      "export_format", NULL
   };
   warn_unknown_keys(table, "webui", known_keys);

   PARSE_BOOL(table, "enabled", config->enabled);
   PARSE_INT(table, "port", config->port);
   PARSE_INT(table, "max_clients", config->max_clients);
   PARSE_INT(table, "audio_chunk_ms", config->audio_chunk_ms);
   PARSE_STRING(table, "www_path", config->www_path);
   PARSE_STRING(table, "bind_address", config->bind_address);
   PARSE_BOOL(table, "https", config->https);
   PARSE_STRING(table, "ssl_cert_path", config->ssl_cert_path);
   PARSE_STRING(table, "ssl_key_path", config->ssl_key_path);
   PARSE_INT(table, "export_max_messages", config->export_max_messages);
   if (config->export_max_messages < 0)
      config->export_max_messages = 0;
   PARSE_STRING(table, "export_format", config->export_format);
   /* Validate export format */
   if (config->export_format[0] != '\0' && strcmp(config->export_format, "json") != 0 &&
       strcmp(config->export_format, "html") != 0) {
      OLOG_WARNING("Config: Invalid webui.export_format '%s', defaulting to 'json'",
                   config->export_format);
      snprintf(config->export_format, sizeof(config->export_format), "json");
   }

   /* Clamp audio_chunk_ms to valid range */
   if (config->audio_chunk_ms < 100) {
      config->audio_chunk_ms = 100;
   } else if (config->audio_chunk_ms > 500) {
      config->audio_chunk_ms = 500;
   }
}

static void parse_images(toml_table_t *table, images_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "retention_days", "max_size_mb", "max_per_user",
                                             "cache_size_mb", NULL };
   warn_unknown_keys(table, "images", known_keys);

   PARSE_INT(table, "retention_days", config->retention_days);
   PARSE_INT(table, "max_size_mb", config->max_size_mb);
   PARSE_INT(table, "max_per_user", config->max_per_user);
   PARSE_INT(table, "cache_size_mb", config->cache_size_mb);

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

   /* Clamp cache_size_mb to valid range */
   if (config->cache_size_mb < 10) {
      config->cache_size_mb = 10;
   } else if (config->cache_size_mb > 2048) {
      config->cache_size_mb = 2048;
   }
}

static void parse_documents(toml_table_t *table, documents_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "max_file_size_kb",
                                             "max_documents",
                                             "max_pages",
                                             "max_extracted_size_kb",
                                             "max_index_size_kb",
                                             "max_indexed_documents",
                                             "fts_label_weight",
                                             "fts_body_weight",
                                             "hybrid_keyword_weight",
                                             "hybrid_vector_weight",
                                             "phrase_bonus_weight",
                                             "search_min_score",
                                             "version_retention_days",
                                             "version_keep_per_doc",
                                             "originals_enabled",
                                             "original_retention_days",
                                             "max_original_size_mb",
                                             "max_originals_per_user",
                                             "max_originals_total_mb_per_user",
                                             "original_grace_minutes",
                                             NULL };
   warn_unknown_keys(table, "documents", known_keys);

   PARSE_INT(table, "max_file_size_kb", config->max_file_size_kb);
   CONFIG_CLAMP(config->max_file_size_kb, 64, 10240);

   PARSE_INT(table, "max_documents", config->max_documents);
   CONFIG_CLAMP(config->max_documents, 1, 20);

   PARSE_INT(table, "max_pages", config->max_pages);
   CONFIG_CLAMP(config->max_pages, 1, 500);

   PARSE_INT(table, "max_extracted_size_kb", config->max_extracted_size_kb);
   CONFIG_CLAMP(config->max_extracted_size_kb, 128, 4096);

   PARSE_INT(table, "max_index_size_kb", config->max_index_size_kb);
   CONFIG_CLAMP(config->max_index_size_kb, 128, 10240);

   PARSE_INT(table, "max_indexed_documents", config->max_indexed_documents);
   CONFIG_CLAMP(config->max_indexed_documents, 1, 500);

   /* v61 hybrid document search weights.  Column weights may exceed 1 (label is
    * boosted ~3x over body); fusion/phrase/gate weights stay in [0,1]. */
   PARSE_DOUBLE(table, "fts_label_weight", config->fts_label_weight);
   CONFIG_CLAMP(config->fts_label_weight, 0.0f, 100.0f);
   PARSE_DOUBLE(table, "fts_body_weight", config->fts_body_weight);
   CONFIG_CLAMP(config->fts_body_weight, 0.0f, 100.0f);
   PARSE_DOUBLE(table, "hybrid_keyword_weight", config->hybrid_keyword_weight);
   CONFIG_CLAMP(config->hybrid_keyword_weight, 0.0f, 1.0f);
   PARSE_DOUBLE(table, "hybrid_vector_weight", config->hybrid_vector_weight);
   CONFIG_CLAMP(config->hybrid_vector_weight, 0.0f, 1.0f);
   PARSE_DOUBLE(table, "phrase_bonus_weight", config->phrase_bonus_weight);
   CONFIG_CLAMP(config->phrase_bonus_weight, 0.0f, 1.0f);
   PARSE_DOUBLE(table, "search_min_score", config->search_min_score);
   CONFIG_CLAMP(config->search_min_score, 0.0f, 1.0f);

   PARSE_INT(table, "version_retention_days", config->version_retention_days);
   CONFIG_CLAMP(config->version_retention_days, 0, 3650);

   PARSE_INT(table, "version_keep_per_doc", config->version_keep_per_doc);
   CONFIG_CLAMP(config->version_keep_per_doc, 1, 1000);

   /* v68 original-file storage. */
   PARSE_BOOL(table, "originals_enabled", config->originals_enabled);
   PARSE_INT(table, "original_retention_days", config->original_retention_days);
   CONFIG_CLAMP(config->original_retention_days, 0, 3650);
   PARSE_INT(table, "max_original_size_mb", config->max_original_size_mb);
   CONFIG_CLAMP(config->max_original_size_mb, 1, 512);
   PARSE_INT(table, "max_originals_per_user", config->max_originals_per_user);
   CONFIG_CLAMP(config->max_originals_per_user, 1, 100000);
   PARSE_INT(table, "max_originals_total_mb_per_user", config->max_originals_total_mb_per_user);
   CONFIG_CLAMP(config->max_originals_total_mb_per_user, 0, 1048576);
   PARSE_INT(table, "original_grace_minutes", config->original_grace_minutes);
   CONFIG_CLAMP(config->original_grace_minutes, 0, 1440);
}

static void parse_vision(toml_table_t *table, vision_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "max_image_size_kb", "max_dimension", "max_images",
                                             "capture_history_count", NULL };
   warn_unknown_keys(table, "vision", known_keys);

   PARSE_INT(table, "max_image_size_kb", config->max_image_size_kb);
   CONFIG_CLAMP(config->max_image_size_kb, 512, 16384);

   PARSE_INT(table, "max_dimension", config->max_dimension);
   CONFIG_CLAMP(config->max_dimension, 256, 4096);

   PARSE_INT(table, "max_images", config->max_images);
   CONFIG_CLAMP(config->max_images, 1, 10);

   PARSE_INT(table, "capture_history_count", config->capture_history_count);
   CONFIG_CLAMP(config->capture_history_count, 0, 50);
}

static void parse_memory(toml_table_t *table, memory_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "enabled",
                                             "context_budget_tokens",
                                             "source_budget_chars",
                                             "extraction_provider",
                                             "extraction_model",
                                             "extraction_openrouter_model",
                                             "extraction_timeout_ms",
                                             "paraphrase_dedup_enabled",
                                             "paraphrase_dedup_threshold",
                                             "note_extraction_guard",
                                             "pruning_enabled",
                                             "prune_superseded_days",
                                             "prune_stale_days",
                                             "prune_stale_min_confidence",
                                             "expire_enabled",
                                             "expire_grace_days",
                                             "prune_expired_days",
                                             "conversation_idle_timeout_min",
                                             "default_voice_user_id",
                                             "pruning",
                                             "decay",
                                             "embeddings",
                                             "recovery",
                                             "focus_injection", /* Phase 1b sub-table */
                                             "entity_merge",    /* Phase 2 sub-table */
                                             "graph_retrieval", /* Phase 1A sub-table */
                                             NULL };
   warn_unknown_keys(table, "memory", known_keys);

   PARSE_BOOL(table, "enabled", config->enabled);
   PARSE_INT(table, "context_budget_tokens", config->context_budget_tokens);
   PARSE_INT(table, "source_budget_chars", config->source_budget_chars);
   PARSE_STRING(table, "extraction_provider", config->extraction_provider);
   PARSE_STRING(table, "extraction_model", config->extraction_model);
   PARSE_STRING(table, "extraction_openrouter_model", config->extraction_openrouter_model);
   PARSE_INT(table, "extraction_timeout_ms", config->extraction_timeout_ms);
   PARSE_BOOL(table, "paraphrase_dedup_enabled", config->paraphrase_dedup_enabled);
   PARSE_DOUBLE(table, "paraphrase_dedup_threshold", config->paraphrase_dedup_threshold);
   CONFIG_CLAMP(config->paraphrase_dedup_threshold, 0.5f, 1.0f);
   PARSE_BOOL(table, "note_extraction_guard", config->note_extraction_guard);

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

   /* Parse fact-expiry / ephemerality settings (v58, C3) */
   PARSE_BOOL(table, "expire_enabled", config->expire_enabled);
   PARSE_INT(table, "expire_grace_days", config->expire_grace_days);
   PARSE_INT(table, "prune_expired_days", config->prune_expired_days);

   /* Parse voice conversation idle timeout settings */
   PARSE_INT(table, "conversation_idle_timeout_min", config->conversation_idle_timeout_min);
   PARSE_INT(table, "default_voice_user_id", config->default_voice_user_id);

   /* Clamp context_budget_tokens to valid range */
   if (config->context_budget_tokens < 100) {
      config->context_budget_tokens = 100;
   } else if (config->context_budget_tokens > 2000) {
      config->context_budget_tokens = 2000;
   }

   /* Clamp source_budget_chars.  0 in config means "use compile-time default";
    * otherwise clamp to the same MEMORY_SOURCE_BUDGET_MAX (32 KiB) the runtime
    * applies in memory_action_search/_recent.  Lower bound of 0 (disabled —
    * no source excerpts at all) is allowed so deployments can opt out of
    * verbatim-source token cost entirely. */
   if (config->source_budget_chars < 0) {
      config->source_budget_chars = 0;
   } else if (config->source_budget_chars > 32768) {
      config->source_budget_chars = 32768;
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

   /* Clamp expiry windows.  Grace 0-365 days (0 = expire on the reference date).
    * prune_expired 0-365 days (0 = hard-expire on the reference date, no buffer). */
   if (config->expire_grace_days < 0) {
      config->expire_grace_days = 0;
   } else if (config->expire_grace_days > 365) {
      config->expire_grace_days = 365;
   }
   if (config->prune_expired_days < 0) {
      config->prune_expired_days = 0;
   } else if (config->prune_expired_days > 365) {
      config->prune_expired_days = 365;
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

   /* Parse [memory.decay] sub-table */
   toml_table_t *decay = toml_table_in(table, "decay");
   if (decay) {
      static const char *const decay_keys[] = { "enabled",
                                                "hour",
                                                "inferred_weekly",
                                                "explicit_weekly",
                                                "preference_weekly",
                                                "inferred_floor",
                                                "explicit_floor",
                                                "preference_floor",
                                                "prune_threshold",
                                                "summary_retention_days",
                                                "access_reinforcement_boost",
                                                NULL };
      warn_unknown_keys(decay, "memory.decay", decay_keys);

      PARSE_BOOL(decay, "enabled", config->decay_enabled);
      PARSE_INT(decay, "hour", config->decay_hour);
      PARSE_DOUBLE(decay, "inferred_weekly", config->decay_inferred_weekly);
      PARSE_DOUBLE(decay, "explicit_weekly", config->decay_explicit_weekly);
      PARSE_DOUBLE(decay, "preference_weekly", config->decay_preference_weekly);
      PARSE_DOUBLE(decay, "inferred_floor", config->decay_inferred_floor);
      PARSE_DOUBLE(decay, "explicit_floor", config->decay_explicit_floor);
      PARSE_DOUBLE(decay, "preference_floor", config->decay_preference_floor);
      PARSE_DOUBLE(decay, "prune_threshold", config->decay_prune_threshold);
      PARSE_INT(decay, "summary_retention_days", config->summary_retention_days);
      PARSE_DOUBLE(decay, "access_reinforcement_boost", config->access_reinforcement_boost);
   }

   /* Clamp decay values to sane ranges */
   if (config->decay_hour < 0)
      config->decay_hour = 0;
   if (config->decay_hour > 23)
      config->decay_hour = 23;

   /* Weekly multipliers: 0.5-1.0 */
   if (config->decay_inferred_weekly < 0.5f)
      config->decay_inferred_weekly = 0.5f;
   if (config->decay_inferred_weekly > 1.0f)
      config->decay_inferred_weekly = 1.0f;
   if (config->decay_explicit_weekly < 0.5f)
      config->decay_explicit_weekly = 0.5f;
   if (config->decay_explicit_weekly > 1.0f)
      config->decay_explicit_weekly = 1.0f;
   if (config->decay_preference_weekly < 0.5f)
      config->decay_preference_weekly = 0.5f;
   if (config->decay_preference_weekly > 1.0f)
      config->decay_preference_weekly = 1.0f;

   /* Floors: 0.0-1.0 */
   if (config->decay_inferred_floor < 0.0f)
      config->decay_inferred_floor = 0.0f;
   if (config->decay_inferred_floor > 1.0f)
      config->decay_inferred_floor = 1.0f;
   if (config->decay_explicit_floor < 0.0f)
      config->decay_explicit_floor = 0.0f;
   if (config->decay_explicit_floor > 1.0f)
      config->decay_explicit_floor = 1.0f;
   if (config->decay_preference_floor < 0.0f)
      config->decay_preference_floor = 0.0f;
   if (config->decay_preference_floor > 1.0f)
      config->decay_preference_floor = 1.0f;

   /* Prune threshold: 0.0-0.5 */
   if (config->decay_prune_threshold < 0.0f)
      config->decay_prune_threshold = 0.0f;
   if (config->decay_prune_threshold > 0.5f)
      config->decay_prune_threshold = 0.5f;

   /* Summary retention: 7-365 days */
   if (config->summary_retention_days < 7)
      config->summary_retention_days = 7;
   if (config->summary_retention_days > 365)
      config->summary_retention_days = 365;

   /* Reinforcement boost: 0.0-0.5 */
   if (config->access_reinforcement_boost < 0.0f)
      config->access_reinforcement_boost = 0.0f;
   if (config->access_reinforcement_boost > 0.5f)
      config->access_reinforcement_boost = 0.5f;

   /* Parse [memory.embeddings] sub-table */
   toml_table_t *embeddings = toml_table_in(table, "embeddings");
   if (embeddings) {
      static const char *const emb_keys[] = { "provider",
                                              "model",
                                              "endpoint",
                                              "keyword_weight",
                                              "vector_weight",
                                              "temporal_weight",
                                              "category_threshold",
                                              "search_score_floor",
                                              "backfill_on_startup",
                                              "model_id",
                                              "recompute_on_model_change",
                                              "recompute_batch_size",
                                              "recompute_batch_sleep_ms",
                                              "temporal_filter_enabled",
                                              "rrf_enabled",
                                              "memory_text_minimal",
                                              "bm25_enabled",
                                              NULL };
      warn_unknown_keys(embeddings, "memory.embeddings", emb_keys);

      PARSE_STRING(embeddings, "provider", config->embedding_provider);
      PARSE_STRING(embeddings, "model", config->embedding_model);
      PARSE_STRING(embeddings, "endpoint", config->embedding_endpoint);
      PARSE_DOUBLE(embeddings, "keyword_weight", config->embedding_keyword_weight);
      PARSE_DOUBLE(embeddings, "vector_weight", config->embedding_vector_weight);
      PARSE_DOUBLE(embeddings, "temporal_weight", config->temporal_weight);
      PARSE_BOOL(embeddings, "temporal_filter_enabled", config->temporal_filter_enabled);
      PARSE_BOOL(embeddings, "rrf_enabled", config->rrf_enabled);
      PARSE_BOOL(embeddings, "memory_text_minimal", config->memory_text_minimal);
      PARSE_BOOL(embeddings, "bm25_enabled", config->bm25_enabled);
      PARSE_DOUBLE(embeddings, "category_threshold", config->category_threshold);
      PARSE_DOUBLE(embeddings, "search_score_floor", config->search_score_floor);
      PARSE_BOOL(embeddings, "backfill_on_startup", config->embedding_backfill_on_startup);
      PARSE_STRING(embeddings, "model_id", config->model_id);
      PARSE_BOOL(embeddings, "recompute_on_model_change", config->recompute_on_model_change);
      PARSE_INT(embeddings, "recompute_batch_size", config->recompute_batch_size);
      PARSE_INT(embeddings, "recompute_batch_sleep_ms", config->recompute_batch_sleep_ms);
   }

   /* Clamp embedding weights to 0.0-1.0 */
   CONFIG_CLAMP(config->embedding_keyword_weight, 0.0f, 1.0f);
   CONFIG_CLAMP(config->embedding_vector_weight, 0.0f, 1.0f);
   CONFIG_CLAMP(config->temporal_weight, 0.0f, 1.0f);
   CONFIG_CLAMP(config->category_threshold, 0.05f, 0.95f);
   CONFIG_CLAMP(config->search_score_floor, 0.0f, 1.0f);

   /* Parse [memory.graph_retrieval] sub-table */
   toml_table_t *graph = toml_table_in(table, "graph_retrieval");
   if (graph) {
      static const char *const graph_keys[] = {
         "enabled",           "entity_grounding_bonus", "max_facts_per_query",
         "use_query_scoring", "entity_bonus",           NULL
      };
      warn_unknown_keys(graph, "memory.graph_retrieval", graph_keys);
      PARSE_BOOL(graph, "enabled", config->graph_retrieval.enabled);
      PARSE_DOUBLE(graph, "entity_grounding_bonus", config->graph_retrieval.entity_grounding_bonus);
      PARSE_INT(graph, "max_facts_per_query", config->graph_retrieval.max_facts_per_query);
      PARSE_BOOL(graph, "use_query_scoring", config->graph_retrieval.use_query_scoring);
      PARSE_DOUBLE(graph, "entity_bonus", config->graph_retrieval.entity_bonus);
   }
   CONFIG_CLAMP(config->graph_retrieval.entity_grounding_bonus, 0.0f, 1.0f);
   CONFIG_CLAMP(config->graph_retrieval.max_facts_per_query, 1, 200);
   CONFIG_CLAMP(config->graph_retrieval.entity_bonus, 0.0f, 1.0f);

   /* Parse [memory.recovery] sub-table */
   toml_table_t *recovery = toml_table_in(table, "recovery");
   if (recovery) {
      static const char *const rec_keys[] = { "enabled", "idle_threshold_seconds", "max_attempts",
                                              "recurring_interval_seconds", NULL };
      warn_unknown_keys(recovery, "memory.recovery", rec_keys);

      PARSE_BOOL(recovery, "enabled", config->recovery_enabled);
      PARSE_INT(recovery, "idle_threshold_seconds", config->recovery_idle_threshold_seconds);
      PARSE_INT(recovery, "max_attempts", config->recovery_max_attempts);
      PARSE_INT(recovery, "recurring_interval_seconds",
                config->recovery_recurring_interval_seconds);
   }

   /* Parse [memory.focus_injection] sub-table — Phase 1 dynamic context
    * injection.  Three nested layers: top-level scalars, [.source_weights]
    * (fixed-shape per-source weights), [.dedup] (1f bookkeeping). */
   toml_table_t *focus = toml_table_in(table, "focus_injection");
   if (focus) {
      static const char *const focus_keys[] = { "enabled",
                                                "focus_budget_bytes",
                                                "focus_budget_tokens", /* deprecated alias */
                                                "top_k",
                                                "summary_max_scan",
                                                "min_score",
                                                "classifier_enabled",
                                                "weight_semantic",
                                                "weight_recency",
                                                "weight_importance",
                                                "weight_source",
                                                "source_weights",
                                                "dedup",
                                                "dominant_token_heuristic",
                                                NULL };
      warn_unknown_keys(focus, "memory.focus_injection", focus_keys);

      focus_injection_config_t *fi = &config->focus_injection;
      PARSE_BOOL(focus, "enabled", fi->enabled);
      PARSE_INT(focus, "focus_budget_bytes", fi->focus_budget_bytes);
      /* Back-compat: the focus budget was renamed from the (estimated)
       * token unit to exact bytes.  If the new key is absent but the old
       * one is present, convert (1 token ≈ 4 bytes) and warn once. */
      if (!toml_int_in(focus, "focus_budget_bytes").ok) {
         toml_datum_t legacy = toml_int_in(focus, "focus_budget_tokens");
         if (legacy.ok) {
            fi->focus_budget_bytes = (int)(legacy.u.i * 4);
            OLOG_WARNING("config: 'focus_budget_tokens' is deprecated — use "
                         "'focus_budget_bytes'.  Converted %lld tokens → %d bytes.",
                         (long long)legacy.u.i, fi->focus_budget_bytes);
         }
      }
      PARSE_INT(focus, "top_k", fi->top_k);
      PARSE_INT(focus, "summary_max_scan", fi->summary_max_scan);
      PARSE_DOUBLE(focus, "min_score", fi->min_score);
      PARSE_BOOL(focus, "classifier_enabled", fi->classifier_enabled);
      PARSE_DOUBLE(focus, "weight_semantic", fi->weight_semantic);
      PARSE_DOUBLE(focus, "weight_recency", fi->weight_recency);
      PARSE_DOUBLE(focus, "weight_importance", fi->weight_importance);
      PARSE_DOUBLE(focus, "weight_source", fi->weight_source);

      toml_table_t *src = toml_table_in(focus, "source_weights");
      if (src) {
         static const char *const src_keys[] = {
            "memory_fact",    "memory_entity",   "memory_relation",
            "memory_summary", "document_chunk",  "calendar_event",
            "recent_email",   "dawn_background", NULL
         };
         warn_unknown_keys(src, "memory.focus_injection.source_weights", src_keys);

         PARSE_DOUBLE(src, "memory_fact", fi->source_weights.memory_fact);
         PARSE_DOUBLE(src, "memory_entity", fi->source_weights.memory_entity);
         PARSE_DOUBLE(src, "memory_relation", fi->source_weights.memory_relation);
         PARSE_DOUBLE(src, "memory_summary", fi->source_weights.memory_summary);
         PARSE_DOUBLE(src, "document_chunk", fi->source_weights.document_chunk);
         PARSE_DOUBLE(src, "calendar_event", fi->source_weights.calendar_event);
         PARSE_DOUBLE(src, "recent_email", fi->source_weights.recent_email);
         PARSE_DOUBLE(src, "dawn_background", fi->source_weights.dawn_background);
      }

      toml_table_t *dedup = toml_table_in(focus, "dedup");
      if (dedup) {
         static const char *const dedup_keys[] = { "recent_window_turns", "score_uplift_factor",
                                                   NULL };
         warn_unknown_keys(dedup, "memory.focus_injection.dedup", dedup_keys);

         PARSE_INT(dedup, "recent_window_turns", fi->dedup.recent_window_turns);
         PARSE_DOUBLE(dedup, "score_uplift_factor", fi->dedup.score_uplift_factor);
      }

      toml_table_t *dth = toml_table_in(focus, "dominant_token_heuristic");
      if (dth) {
         static const char *const dth_keys[] = { "enabled", "threshold", "base_penalty", NULL };
         warn_unknown_keys(dth, "memory.focus_injection.dominant_token_heuristic", dth_keys);

         PARSE_BOOL(dth, "enabled", fi->dominant_token_heuristic.enabled);
         PARSE_DOUBLE(dth, "threshold", fi->dominant_token_heuristic.threshold);
         PARSE_DOUBLE(dth, "base_penalty", fi->dominant_token_heuristic.base_penalty);
      }
   }

   /* Parse [memory.recall] sub-table — unified cross-source recall tool
    * (docs/CROSS_TOOL_RECALL_DESIGN.md).  Deep-gather trim limits, separate
    * from the per-turn focus_injection block above. */
   toml_table_t *recall = toml_table_in(table, "recall");
   if (recall) {
      static const char *const recall_keys[] = { "top_k", "budget_bytes", "min_score",
                                                 "per_source_max", NULL };
      warn_unknown_keys(recall, "memory.recall", recall_keys);

      recall_config_t *rc = &config->recall;
      PARSE_INT(recall, "top_k", rc->top_k);
      PARSE_INT(recall, "budget_bytes", rc->budget_bytes);
      PARSE_DOUBLE(recall, "min_score", rc->min_score);
      PARSE_INT(recall, "per_source_max", rc->per_source_max);
   }

   /* Parse [memory.entity_merge] sub-table — Phase 2 auto-merge gate. */
   toml_table_t *emerge = toml_table_in(table, "entity_merge");
   if (emerge) {
      static const char *const emerge_keys[] = { "enabled", "auto_threshold", "review_threshold",
                                                 NULL };
      warn_unknown_keys(emerge, "memory.entity_merge", emerge_keys);

      PARSE_BOOL(emerge, "enabled", config->entity_merge_enabled);
      PARSE_DOUBLE(emerge, "auto_threshold", config->entity_merge_auto_threshold);
      PARSE_DOUBLE(emerge, "review_threshold", config->entity_merge_review_threshold);
      /* Clamp to sane bounds.  auto >= review enforced after clamping so
       * operators can't accidentally make the auto band wider than review
       * (which would auto-merge proposals that should require approval). */
      CONFIG_CLAMP(config->entity_merge_auto_threshold, 0.30f, 1.0f);
      CONFIG_CLAMP(config->entity_merge_review_threshold, 0.10f, 1.0f);
      if (config->entity_merge_review_threshold > config->entity_merge_auto_threshold) {
         config->entity_merge_review_threshold = config->entity_merge_auto_threshold;
      }
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

   static const char *const known_keys[] = {
      "mic_record", "asr_record", "aec_record", "record_path", "silent_observe_test_endpoint", NULL
   };
   warn_unknown_keys(table, "debug", known_keys);

   PARSE_BOOL(table, "mic_record", config->mic_record);
   PARSE_BOOL(table, "asr_record", config->asr_record);
   PARSE_BOOL(table, "aec_record", config->aec_record);
   PARSE_STRING(table, "record_path", config->record_path);
   PARSE_BOOL(table, "silent_observe_test_endpoint", config->silent_observe_test_endpoint);
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

   static const char *const known_keys[] = { "source", "scan_interval_minutes", "plex", "streaming",
                                             NULL };
   warn_unknown_keys(table, "music", known_keys);

   PARSE_INT(table, "scan_interval_minutes", config->scan_interval_minutes);

   /* Parse music.plex subtable */
   toml_table_t *plex = toml_table_in(table, "plex");
   if (plex) {
      static const char *const plex_keys[] = { "host", "port",       "music_section_id",
                                               "ssl",  "ssl_verify", "client_identifier",
                                               NULL };
      warn_unknown_keys(plex, "music.plex", plex_keys);

      PARSE_STRING(plex, "host", config->plex.host);
      /* Validate hostname chars — prevent TOML injection on write-back */
      for (char *p = config->plex.host; *p; p++) {
         if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
               *p == '.' || *p == '-' || *p == ':' || *p == '[' || *p == ']')) {
            OLOG_WARNING("Config: invalid character in music.plex.host, clearing");
            config->plex.host[0] = '\0';
            break;
         }
      }
      PARSE_INT(plex, "port", config->plex.port);
      if (config->plex.port < 1)
         config->plex.port = 1;
      if (config->plex.port > 65535)
         config->plex.port = 65535;
      PARSE_INT(plex, "music_section_id", config->plex.music_section_id);
      PARSE_BOOL(plex, "ssl", config->plex.ssl);
      PARSE_BOOL(plex, "ssl_verify", config->plex.ssl_verify);
      PARSE_STRING(plex, "client_identifier", config->plex.client_identifier);

      if (!config->plex.ssl_verify && config->plex.ssl) {
         OLOG_WARNING("Plex: ssl_verify=false with ssl=true — TLS certificate "
                      "verification is disabled. Consider adding the Plex server "
                      "certificate to DAWN's trust store instead.");
      }
   }

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

static void parse_scheduler(toml_table_t *table, scheduler_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "enabled",
                                             "default_snooze_minutes",
                                             "max_snooze_count",
                                             "max_events_per_user",
                                             "max_events_total",
                                             "missed_event_recovery",
                                             "missed_task_policy",
                                             "missed_task_max_age_sec",
                                             "alarm_timeout_sec",
                                             "alarm_volume",
                                             "event_retention_days",
                                             "briefing_speak_aloud_on_webui_source",
                                             NULL };
   warn_unknown_keys(table, "scheduler", known_keys);

   PARSE_BOOL(table, "enabled", config->enabled);
   PARSE_INT(table, "default_snooze_minutes", config->default_snooze_minutes);
   PARSE_INT(table, "max_snooze_count", config->max_snooze_count);
   PARSE_INT(table, "max_events_per_user", config->max_events_per_user);
   PARSE_INT(table, "max_events_total", config->max_events_total);
   PARSE_BOOL(table, "missed_event_recovery", config->missed_event_recovery);
   PARSE_STRING(table, "missed_task_policy", config->missed_task_policy);
   PARSE_INT(table, "missed_task_max_age_sec", config->missed_task_max_age_sec);
   PARSE_INT(table, "alarm_timeout_sec", config->alarm_timeout_sec);
   PARSE_INT(table, "alarm_volume", config->alarm_volume);
   PARSE_INT(table, "event_retention_days", config->event_retention_days);
   PARSE_BOOL(table, "briefing_speak_aloud_on_webui_source",
              config->briefing_speak_aloud_on_webui_source);

   /* Clamp values */
   if (config->alarm_timeout_sec > 300)
      config->alarm_timeout_sec = 300;
   if (config->alarm_volume < 0)
      config->alarm_volume = 0;
   if (config->alarm_volume > 100)
      config->alarm_volume = 100;
}

/* Shared by the TOML parse path and the WebUI settings POST handler so a value
 * arriving over the wire can't bypass bounds the file path enforces.  The
 * job-session pool array is sized to max_active_jobs, so a nonsense value must
 * not blow the allocation. */
void config_clamp_jobs(jobs_config_t *config) {
   if (!config) {
      return;
   }
   if (config->max_concurrent_local < 0)
      config->max_concurrent_local = 0;
   if (config->max_concurrent_cloud < 0)
      config->max_concurrent_cloud = 0;
   if (config->max_active_jobs < 1)
      config->max_active_jobs = 1;
   if (config->max_active_jobs > 256)
      config->max_active_jobs = 256;
   if (config->max_jobs_per_user < 1)
      config->max_jobs_per_user = 1;
   if (config->max_queued_per_user < 0)
      config->max_queued_per_user = 0;
   if (config->monitor_followups_per_tick < 1)
      config->monitor_followups_per_tick = 1;
   if (config->max_spawn_depth < 0)
      config->max_spawn_depth = 0;
   if (config->max_children_per_tree < 0)
      config->max_children_per_tree = 0;
   if (config->event_chunk_cap < 0) /* sizes a Phase-2 payload buffer */
      config->event_chunk_cap = 0;
   if (config->max_reinvokes_per_tree < 1) /* need at least one reinvoke attempt */
      config->max_reinvokes_per_tree = 1;
   if (config->max_concurrent_reinvokes < 1)
      config->max_concurrent_reinvokes = 1;
   if (config->max_concurrent_reinvokes > 32) /* bounded by the in-flight set */
      config->max_concurrent_reinvokes = 32;
   if (config->max_runtime_sec < 0)
      config->max_runtime_sec = 0;
}

static void parse_jobs(toml_table_t *table, jobs_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "enabled",
                                             "max_concurrent_local",
                                             "max_concurrent_cloud",
                                             "max_active_jobs",
                                             "max_jobs_per_user",
                                             "max_queued_per_user",
                                             "monitor_followups_per_tick",
                                             "max_spawn_depth",
                                             "max_children_per_tree",
                                             "max_reinvokes_per_tree",
                                             "max_concurrent_reinvokes",
                                             "max_runtime_sec",
                                             "event_chunk_cap",
                                             "event_retention_days",
                                             NULL };
   warn_unknown_keys(table, "jobs", known_keys);

   PARSE_BOOL(table, "enabled", config->enabled);
   PARSE_INT(table, "max_concurrent_local", config->max_concurrent_local);
   PARSE_INT(table, "max_concurrent_cloud", config->max_concurrent_cloud);
   PARSE_INT(table, "max_active_jobs", config->max_active_jobs);
   PARSE_INT(table, "max_jobs_per_user", config->max_jobs_per_user);
   PARSE_INT(table, "max_queued_per_user", config->max_queued_per_user);
   PARSE_INT(table, "monitor_followups_per_tick", config->monitor_followups_per_tick);
   PARSE_INT(table, "max_spawn_depth", config->max_spawn_depth);
   PARSE_INT(table, "max_children_per_tree", config->max_children_per_tree);
   PARSE_INT(table, "max_reinvokes_per_tree", config->max_reinvokes_per_tree);
   PARSE_INT(table, "max_concurrent_reinvokes", config->max_concurrent_reinvokes);
   PARSE_INT(table, "max_runtime_sec", config->max_runtime_sec);
   PARSE_INT(table, "event_chunk_cap", config->event_chunk_cap);
   PARSE_INT(table, "event_retention_days", config->event_retention_days);

   config_clamp_jobs(config);
}

static void parse_calendar(toml_table_t *table, calendar_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = {
      "enabled",           "sync_interval_sec",          "cache_past_days",
      "cache_future_days", "default_event_duration_min", NULL
   };
   warn_unknown_keys(table, "calendar", known_keys);

   PARSE_BOOL(table, "enabled", config->enabled);
   PARSE_INT(table, "sync_interval_sec", config->sync_interval_sec);
   PARSE_INT(table, "cache_past_days", config->cache_past_days);
   PARSE_INT(table, "cache_future_days", config->cache_future_days);
   PARSE_INT(table, "default_event_duration_min", config->default_event_duration_min);

   /* Clamp values */
   if (config->sync_interval_sec < 60)
      config->sync_interval_sec = 60;
   if (config->cache_past_days < 0)
      config->cache_past_days = 0;
   if (config->cache_future_days < 1)
      config->cache_future_days = 1;
   if (config->default_event_duration_min < 1)
      config->default_event_duration_min = 1;
}

static void parse_messaging(toml_table_t *table, messaging_config_t *config) {
   if (!table)
      return;

   /* [messaging.sms] sub-section. */
   toml_table_t *sms = toml_table_in(table, "sms");
   if (sms) {
      static const char *const sms_known_keys[] = { "active_window_sec", NULL };
      warn_unknown_keys(sms, "messaging.sms", sms_known_keys);
      PARSE_INT(sms, "active_window_sec", config->sms_active_window_sec);
   }

   /* Clamp.  Negative is nonsense; 0 disables; cap at 24h to avoid
    * accidental "forever" via stale config. */
   if (config->sms_active_window_sec < 0)
      config->sms_active_window_sec = 0;
   if (config->sms_active_window_sec > 86400)
      config->sms_active_window_sec = 86400;
}

static void parse_ota(toml_table_t *table, ota_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "enabled", "release_dir", "download_token_ttl_sec",
                                             "require_tls", NULL };
   warn_unknown_keys(table, "ota", known_keys);

   PARSE_BOOL(table, "enabled", config->enabled);
   PARSE_STRING(table, "release_dir", config->release_dir);
   PARSE_INT(table, "download_token_ttl_sec", config->download_token_ttl_sec);
   PARSE_BOOL(table, "require_tls", config->require_tls);
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

static void parse_mcp_server(toml_table_t *table, mcp_server_config_t *server) {
   /* Secure-by-default per-server values (the array slot is already zeroed). */
   safe_strncpy(server->transport, "http+sse", sizeof(server->transport));
   safe_strncpy(server->capabilities, "dangerous", sizeof(server->capabilities));
   server->enabled = true;
   server->tls_verify = true;
   server->request_timeout_seconds = 30;
   server->idle_close_seconds = 600;

   static const char *const known_keys[] = { "alias",
                                             "url",
                                             "transport",
                                             "enabled",
                                             "capabilities",
                                             "request_timeout_seconds",
                                             "idle_close_seconds",
                                             "tls_verify",
                                             "auth_bearer_env",
                                             "auth_bearer_token_secret",
                                             NULL };
   warn_unknown_keys(table, "mcp.server", known_keys);

   PARSE_STRING(table, "alias", server->alias);
   PARSE_STRING(table, "url", server->url);
   PARSE_STRING(table, "transport", server->transport);
   PARSE_BOOL(table, "enabled", server->enabled);
   PARSE_STRING(table, "capabilities", server->capabilities);
   PARSE_INT(table, "request_timeout_seconds", server->request_timeout_seconds);
   PARSE_INT(table, "idle_close_seconds", server->idle_close_seconds);
   PARSE_BOOL(table, "tls_verify", server->tls_verify);
   PARSE_STRING(table, "auth_bearer_env", server->auth_bearer_env);
   PARSE_STRING(table, "auth_bearer_token_secret", server->auth_bearer_token_secret);
}

static void parse_mcp(toml_table_t *table, mcp_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "enabled", "dev_mode", "server", NULL };
   warn_unknown_keys(table, "mcp", known_keys);

   PARSE_BOOL(table, "enabled", config->enabled);
   PARSE_BOOL(table, "dev_mode", config->dev_mode);

   toml_array_t *servers = toml_array_in(table, "server");
   if (servers) {
      config->server_count = 0;
      int n = toml_array_nelem(servers);
      for (int i = 0; i < n && i < MCP_SERVERS_MAX; i++) {
         toml_table_t *srv = toml_table_at(servers, i);
         if (!srv)
            continue;
         parse_mcp_server(srv, &config->servers[config->server_count]);
         config->server_count++;
      }
      if (config->server_count > 0) {
         OLOG_INFO("Parsed %d MCP server(s) from config", config->server_count);
      }
   }
}

static void parse_code_projects(toml_table_t *table, code_projects_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "enabled",
                                             "source_root",
                                             "default_index_mode",
                                             "default_global",
                                             "import_user_required",
                                             "max_repo_size_mb",
                                             "max_file_count",
                                             "max_path_depth",
                                             "clone_depth",
                                             "allowed_host_pattern",
                                             "default_active",
                                             "allowed_local_roots",
                                             NULL };
   warn_unknown_keys(table, "code_projects", known_keys);

   PARSE_BOOL(table, "enabled", config->enabled);
   PARSE_STRING(table, "source_root", config->source_root);
   PARSE_STRING(table, "default_index_mode", config->default_index_mode);
   PARSE_BOOL(table, "default_global", config->default_global);
   PARSE_STRING(table, "import_user_required", config->import_user_required);
   PARSE_INT(table, "max_repo_size_mb", config->max_repo_size_mb);
   PARSE_INT(table, "max_file_count", config->max_file_count);
   PARSE_INT(table, "max_path_depth", config->max_path_depth);
   PARSE_INT(table, "clone_depth", config->clone_depth);
   PARSE_STRING(table, "allowed_host_pattern", config->allowed_host_pattern);
   PARSE_STRING(table, "default_active", config->default_active);

   /* allowed_local_roots: absolute dir prefixes a link-local repo may live under
    * (empty = link-local disabled). Content-exposure boundary — see config struct. */
   toml_array_t *roots_arr = toml_array_in(table, "allowed_local_roots");
   if (roots_arr) {
      int count = toml_array_nelem(roots_arr);
      if (count > CODE_PROJECTS_MAX_LOCAL_ROOTS) {
         OLOG_WARNING("code_projects.allowed_local_roots has %d entries, max is %d - truncating",
                      count, CODE_PROJECTS_MAX_LOCAL_ROOTS);
         count = CODE_PROJECTS_MAX_LOCAL_ROOTS;
      }
      config->allowed_local_roots_count = 0;
      for (int i = 0; i < count; i++) {
         toml_datum_t d = toml_string_at(roots_arr, i);
         if (d.ok && d.u.s) {
            strncpy(config->allowed_local_roots[config->allowed_local_roots_count], d.u.s,
                    CONFIG_PATH_MAX - 1);
            config->allowed_local_roots[config->allowed_local_roots_count][CONFIG_PATH_MAX - 1] =
                '\0';
            config->allowed_local_roots_count++;
            free(d.u.s);
         }
      }
      OLOG_INFO("Parsed %d code_projects.allowed_local_roots", config->allowed_local_roots_count);
   }
}

static void parse_attention(toml_table_t *table, attention_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "enabled",
                                             "max_alerts_per_hour",
                                             "inject_into_sessions",
                                             "judge_enabled",
                                             "judge_threshold",
                                             "quiet_hours",
                                             NULL };
   warn_unknown_keys(table, "attention", known_keys);

   PARSE_BOOL(table, "enabled", config->enabled);
   PARSE_INT(table, "max_alerts_per_hour", config->max_alerts_per_hour);
   PARSE_BOOL(table, "inject_into_sessions", config->inject_into_sessions);
   PARSE_BOOL(table, "judge_enabled", config->judge_enabled);
   PARSE_DOUBLE(table, "judge_threshold", config->judge_threshold);
   PARSE_STRING(table, "quiet_hours", config->quiet_hours);
}

/* Parse dawn.toml into `config`.
 *
 * Adding a parse_*() dispatch below is only step 3 of ~9.  A section that is
 * parsed here but never emitted by config_write_toml() gets DELETED from the
 * user's dawn.toml the first time they save any WebUI setting — silently, with
 * a clean build and green tests.  Wire config_to_json(), config_write_toml(),
 * and the webui_config.c POST handler in the same change, and add the section
 * to tests/test_config_roundtrip.c's required[] list.
 *
 * Full checklist: docs/CONFIGURATION_GUIDE.md */
int config_parse_file(const char *path, dawn_config_t *config) {
   if (!path || !config) {
      OLOG_ERROR("config_parse_file: NULL argument");
      return FAILURE;
   }

   FILE *fp = fopen(path, "r");
   if (!fp) {
      OLOG_ERROR("Failed to open config file: %s", path);
      return FAILURE;
   }

   char errbuf[256];
   toml_table_t *root = toml_parse_file(fp, errbuf, sizeof(errbuf));
   fclose(fp);

   if (!root) {
      OLOG_ERROR("Failed to parse config file %s: %s", path, errbuf);
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
   parse_documents(toml_table_in(root, "documents"), &config->documents);
   parse_vision(toml_table_in(root, "vision"), &config->vision);
   parse_memory(toml_table_in(root, "memory"), &config->memory);
   parse_shutdown(toml_table_in(root, "shutdown"), &config->shutdown);
   parse_debug(toml_table_in(root, "debug"), &config->debug);
   parse_paths(toml_table_in(root, "paths"), &config->paths);
   parse_music(toml_table_in(root, "music"), &config->music);
   parse_scheduler(toml_table_in(root, "scheduler"), &config->scheduler);
   parse_jobs(toml_table_in(root, "jobs"), &config->jobs);
   parse_calendar(toml_table_in(root, "calendar"), &config->calendar);
   parse_messaging(toml_table_in(root, "messaging"), &config->messaging);
   parse_ota(toml_table_in(root, "ota"), &config->ota);
   parse_mcp(toml_table_in(root, "mcp"), &config->mcp);
   parse_code_projects(toml_table_in(root, "code_projects"), &config->code_projects);
   parse_attention(toml_table_in(root, "attention"), &config->attention);

   toml_free(root);

   OLOG_INFO("Loaded configuration from: %s", path);
   return SUCCESS;
}

int config_parse_secrets(const char *path, secrets_config_t *secrets) {
   if (!path || !secrets) {
      OLOG_ERROR("config_parse_secrets: NULL argument");
      return FAILURE;
   }

   /* Check file permissions before loading - warn if too permissive */
   check_sensitive_file_permissions(path, "Secrets file");

   FILE *fp = fopen(path, "r");
   if (!fp) {
      OLOG_WARNING("Secrets file not found: %s", path);
      return FAILURE;
   }

   char errbuf[256];
   toml_table_t *root = toml_parse_file(fp, errbuf, sizeof(errbuf));
   fclose(fp);

   if (!root) {
      OLOG_ERROR("Failed to parse secrets file %s: %s", path, errbuf);
      return FAILURE;
   }

   /* Parse [secrets] section (WebUI format) */
   toml_table_t *secrets_section = toml_table_in(root, "secrets");
   if (secrets_section) {
      PARSE_STRING(secrets_section, "openai_api_key", secrets->openai_api_key);
      PARSE_STRING(secrets_section, "claude_api_key", secrets->claude_api_key);
      PARSE_STRING(secrets_section, "gemini_api_key", secrets->gemini_api_key);
      PARSE_STRING(secrets_section, "openrouter_api_key", secrets->openrouter_api_key);
      PARSE_STRING(secrets_section, "mqtt_username", secrets->mqtt_username);
      PARSE_STRING(secrets_section, "mqtt_password", secrets->mqtt_password);
      PARSE_STRING(secrets_section, "satellite_registration_key",
                   secrets->satellite_registration_key);
      PARSE_STRING(secrets_section, "plex_token", secrets->plex_token);
      PARSE_STRING(secrets_section, "embedding_api_key", secrets->embedding_api_key);

      /* Parse [secrets.home_assistant] sub-section */
      toml_table_t *home_assistant = toml_table_in(secrets_section, "home_assistant");
      if (home_assistant) {
         PARSE_STRING(home_assistant, "token", secrets->home_assistant_token);
      }

      /* Parse [secrets.google] sub-section for OAuth 2.0
       * Used for Google Calendar and Gmail integration */
      toml_table_t *google = toml_table_in(secrets_section, "google");
      if (google) {
         PARSE_STRING(google, "client_id", secrets->google_client_id);
         PARSE_STRING(google, "client_secret", secrets->google_client_secret);
         PARSE_STRING(google, "redirect_url", secrets->google_redirect_url);
      }

      PARSE_STRING(secrets_section, "service_token", secrets->service_token);
      if (secrets->service_token[0] && strlen(secrets->service_token) < 32) {
         OLOG_WARNING("config: service_token is too short (min 32 chars) — ignoring");
         secrets->service_token[0] = '\0';
      }

      /* Tavily API key (used by Tavily search + URL extract adapters) */
      PARSE_STRING(secrets_section, "tavily_api_key", secrets->tavily_api_key);

      /* Telegram Bot API token (messaging channels driver) */
      PARSE_STRING(secrets_section, "telegram_bot_token", secrets->telegram_bot_token);

      /* Discord bot token (messaging channels driver) */
      PARSE_STRING(secrets_section, "discord_bot_token", secrets->discord_bot_token);

      /* Slack Socket Mode tokens (messaging channels driver) */
      PARSE_STRING(secrets_section, "slack_app_token", secrets->slack_app_token);
      PARSE_STRING(secrets_section, "slack_bot_token", secrets->slack_bot_token);
   }

   /* Legacy: Parse [api_keys] section (old format) */
   toml_table_t *api_keys = toml_table_in(root, "api_keys");
   if (api_keys) {
      /* Only parse if not already set from [secrets] */
      if (secrets->openai_api_key[0] == '\0')
         PARSE_STRING(api_keys, "openai", secrets->openai_api_key);
      if (secrets->claude_api_key[0] == '\0')
         PARSE_STRING(api_keys, "claude", secrets->claude_api_key);
      if (secrets->openrouter_api_key[0] == '\0')
         PARSE_STRING(api_keys, "openrouter", secrets->openrouter_api_key);
   }

   toml_free(root);

   OLOG_INFO("Loaded secrets from: %s", path);
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
            OLOG_INFO("Config loaded: %s", s_loaded_config_path);
         }
         return result;
      } else {
         OLOG_ERROR("Specified config file not found: %s", explicit_path);
         return FAILURE;
      }
   }

   /* Priority 2: ./dawn.toml */
   if (config_file_readable("./dawn.toml")) {
      result = config_parse_file("./dawn.toml", config);
      if (result == SUCCESS) {
         strncpy(s_loaded_config_path, "./dawn.toml", sizeof(s_loaded_config_path) - 1);
         OLOG_INFO("Config loaded: %s", s_loaded_config_path);
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
            OLOG_INFO("Config loaded: %s", s_loaded_config_path);
         }
         return result;
      }
   }

   /* Priority 4: /etc/dawn/config.toml */
   if (config_file_readable("/etc/dawn/config.toml")) {
      result = config_parse_file("/etc/dawn/config.toml", config);
      if (result == SUCCESS) {
         strncpy(s_loaded_config_path, "/etc/dawn/config.toml", sizeof(s_loaded_config_path) - 1);
         OLOG_INFO("Config loaded: %s", s_loaded_config_path);
      }
      return result;
   }

   /* No config file found - use defaults (not an error) */
   OLOG_INFO("No config file found, using defaults");
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
         OLOG_INFO("Secrets loaded: %s", s_loaded_secrets_path);
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
            OLOG_INFO("Secrets loaded: %s", s_loaded_secrets_path);
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
         OLOG_INFO("Secrets loaded: %s", s_loaded_secrets_path);
      }
      return result;
   }

   /* Secrets file not found - not an error, secrets are optional */
   OLOG_INFO("No secrets file found");
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
      OLOG_ERROR("Backup path too long for: %s", path);
      return 1;
   }

   /* Read original file */
   FILE *src = fopen(path, "rb");
   if (!src) {
      OLOG_ERROR("Failed to open file for backup: %s", path);
      return 1;
   }

   /* Create backup file with restrictive permissions from the start.
    * Use open() with explicit mode to avoid race condition where file
    * is briefly world-readable before chmod(). Config/secrets backups
    * should always be owner-only (0600) regardless of original perms. */
   int fd = open(backup_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0) {
      OLOG_ERROR("Failed to create backup file: %s", backup_path);
      fclose(src);
      return 1;
   }

   FILE *dst = fdopen(fd, "wb");
   if (!dst) {
      OLOG_ERROR("Failed to open backup file stream: %s", backup_path);
      close(fd);
      fclose(src);
      return 1;
   }

   /* Copy contents */
   char buffer[4096];
   size_t bytes;
   while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
      if (fwrite(buffer, 1, bytes, dst) != bytes) {
         OLOG_ERROR("Failed to write backup file: %s", backup_path);
         fclose(src);
         fclose(dst);
         return 1;
      }
   }

   fclose(src);
   fclose(dst);

   OLOG_INFO("Created backup: %s", backup_path);
   return 0;
}
