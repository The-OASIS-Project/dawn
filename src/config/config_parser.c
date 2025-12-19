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
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/worker_pool.h"
#include "logging.h"
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

static void parse_audio(toml_table_t *table, audio_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "backend",     "capture_device",  "playback_device",
                                             "output_rate", "output_channels", "bargein",
                                             NULL };
   warn_unknown_keys(table, "audio", known_keys);

   PARSE_STRING(table, "backend", config->backend);
   PARSE_STRING(table, "capture_device", config->capture_device);
   PARSE_STRING(table, "playback_device", config->playback_device);
   PARSE_INT(table, "output_rate", config->output_rate);
   PARSE_INT(table, "output_channels", config->output_channels);

   /* Parse [audio.bargein] sub-table */
   toml_table_t *bargein = toml_table_in(table, "bargein");
   parse_audio_bargein(bargein, &config->bargein);
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

   static const char *const known_keys[] = { "provider", "openai_model",   "claude_model",
                                             "model", /* legacy */
                                             "endpoint", "vision_enabled", NULL };
   warn_unknown_keys(table, "llm.cloud", known_keys);

   PARSE_STRING(table, "provider", config->provider);
   PARSE_STRING(table, "openai_model", config->openai_model);
   PARSE_STRING(table, "claude_model", config->claude_model);
   PARSE_STRING(table, "endpoint", config->endpoint);
   PARSE_BOOL(table, "vision_enabled", config->vision_enabled);

   /* Backward compatibility: if legacy "model" is set but new fields aren't,
    * copy it to the appropriate field based on provider */
   toml_datum_t legacy = toml_string_in(table, "model");
   if (legacy.ok && legacy.u.s[0] != '\0') {
      if (strcmp(config->provider, "claude") == 0 && config->claude_model[0] == '\0') {
         snprintf(config->claude_model, sizeof(config->claude_model), "%s", legacy.u.s);
      } else if (config->openai_model[0] == '\0') {
         snprintf(config->openai_model, sizeof(config->openai_model), "%s", legacy.u.s);
      }
      free(legacy.u.s);
   }
}

static void parse_llm_local(toml_table_t *table, llm_local_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "endpoint", "model", "vision_enabled", NULL };
   warn_unknown_keys(table, "llm.local", known_keys);

   PARSE_STRING(table, "endpoint", config->endpoint);
   PARSE_STRING(table, "model", config->model);
   PARSE_BOOL(table, "vision_enabled", config->vision_enabled);
}

static void parse_llm(toml_table_t *table, llm_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "type", "max_tokens", "cloud", "local", NULL };
   warn_unknown_keys(table, "llm", known_keys);

   PARSE_STRING(table, "type", config->type);
   PARSE_INT(table, "max_tokens", config->max_tokens);

   /* Parse [llm.cloud] and [llm.local] sub-tables */
   toml_table_t *cloud = toml_table_in(table, "cloud");
   parse_llm_cloud(cloud, &config->cloud);

   toml_table_t *local = toml_table_in(table, "local");
   parse_llm_local(local, &config->local);
}

static void parse_summarizer(toml_table_t *table, summarizer_file_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "backend", "threshold_bytes", "target_words", NULL };
   warn_unknown_keys(table, "search.summarizer", known_keys);

   PARSE_STRING(table, "backend", config->backend);
   PARSE_SIZE_T(table, "threshold_bytes", config->threshold_bytes);
   PARSE_SIZE_T(table, "target_words", config->target_words);
}

static void parse_search(toml_table_t *table, search_config_t *config) {
   if (!table)
      return;

   static const char *const known_keys[] = { "engine", "endpoint", "summarizer", NULL };
   warn_unknown_keys(table, "search", known_keys);

   PARSE_STRING(table, "engine", config->engine);
   PARSE_STRING(table, "endpoint", config->endpoint);

   /* Parse [search.summarizer] sub-table */
   toml_table_t *summarizer = toml_table_in(table, "summarizer");
   parse_summarizer(summarizer, &config->summarizer);
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

   static const char *const known_keys[] = { "music_dir", "commands_config", NULL };
   warn_unknown_keys(table, "paths", known_keys);

   PARSE_STRING(table, "music_dir", config->music_dir);
   PARSE_STRING(table, "commands_config", config->commands_config);
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
   parse_shutdown(toml_table_in(root, "shutdown"), &config->shutdown);
   parse_debug(toml_table_in(root, "debug"), &config->debug);
   parse_paths(toml_table_in(root, "paths"), &config->paths);

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

   /* Legacy: Parse [mqtt] section for credentials (old format) */
   toml_table_t *mqtt = toml_table_in(root, "mqtt");
   if (mqtt) {
      if (secrets->mqtt_username[0] == '\0')
         PARSE_STRING(mqtt, "username", secrets->mqtt_username);
      if (secrets->mqtt_password[0] == '\0')
         PARSE_STRING(mqtt, "password", secrets->mqtt_password);
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
