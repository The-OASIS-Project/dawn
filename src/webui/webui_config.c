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
 * WebUI Config Handlers - Configuration, discovery, and device management
 *
 * This module handles WebSocket messages for:
 * - get_config, set_config, set_secrets (configuration management)
 * - get_audio_devices (audio device enumeration)
 * - list_models, list_interfaces (discovery)
 */

#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "config/config_env.h"
#include "config/config_parser.h"
#include "config/dawn_config.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_interface.h"
#include "llm/llm_local_provider.h"
#include "logging.h"
#include "webui/webui_internal.h"
#include "webui/webui_server.h" /* For WEBUI_MAX_VISION_* constants */

/* =============================================================================
 * Module State
 * ============================================================================= */

/* Model/Interface Cache - avoids repeated filesystem/network scans */
static discovery_cache_t s_discovery_cache = { .models_response = NULL,
                                               .interfaces_response = NULL,
                                               .models_cache_time = 0,
                                               .interfaces_cache_time = 0,
                                               .cache_mutex = PTHREAD_MUTEX_INITIALIZER };

/* Allowed path prefixes for model directory scanning.
 * Security: Restricts which directories can be scanned for models.
 * The current working directory is always allowed in addition to these. */
static const char *s_allowed_path_prefixes[] = {
   "/home/",      "/var/lib/", "/opt/", "/usr/local/share/",
   "/usr/share/", NULL /* Sentinel - must be last */
};

/* =============================================================================
 * Constants
 * ============================================================================= */

/* Settings that require restart when changed */
static const char *s_restart_required_fields[] = { "audio.backend",
                                                   "audio.capture_device",
                                                   "audio.playback_device",
                                                   "asr.model",
                                                   "asr.models_path",
                                                   "tts.models_path",
                                                   "tts.voice_model",
                                                   "network.enabled",
                                                   "network.host",
                                                   "network.port",
                                                   "network.workers",
                                                   "webui.port",
                                                   "webui.max_clients",
                                                   "webui.workers",
                                                   "webui.https",
                                                   "webui.ssl_cert_path",
                                                   "webui.ssl_key_path",
                                                   "webui.bind_address",
                                                   NULL };

void handle_get_config(ws_connection_t *conn) {
   /* Build response with config, secrets status, and metadata */
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("get_config_response"));

   json_object *payload = json_object_new_object();

   /* Check if user is admin (re-validate from DB to prevent stale cache) */
   bool is_admin = false;
   if (conn->authenticated) {
      auth_session_t session;
      if (auth_db_get_session(conn->auth_session_token, &session) == AUTH_DB_SUCCESS) {
         is_admin = session.is_admin;
      }
   }

   /* Add config path (redacted for non-admins) */
   const char *config_path = config_get_loaded_path();
   json_object_object_add(payload, "config_path",
                          json_object_new_string(is_admin ? config_path : "(configured)"));

   /* Add secrets path (redacted for non-admins) */
   const char *secrets_path = config_get_secrets_path();
   json_object_object_add(payload, "secrets_path",
                          json_object_new_string(is_admin ? secrets_path : "(configured)"));

   /* Add full config as JSON */
   json_object *config_json = config_to_json(config_get());
   if (config_json) {
      json_object_object_add(payload, "config", config_json);
   }

   /* Add secrets status (only is_set flags, never actual values) */
   json_object *secrets_status = secrets_to_json_status(config_get_secrets());
   if (secrets_status) {
      json_object_object_add(payload, "secrets", secrets_status);
   }

   /* Add list of fields that require restart */
   json_object *restart_fields = json_object_new_array();
   for (int i = 0; s_restart_required_fields[i]; i++) {
      json_object_array_add(restart_fields, json_object_new_string(s_restart_required_fields[i]));
   }
   json_object_object_add(payload, "requires_restart", restart_fields);

   /* Add session LLM status (resolved config for this session) */
   json_object *llm_runtime = json_object_new_object();

   /* Get session's resolved LLM config (or global default if no session yet) */
   session_llm_config_t session_config;
   llm_resolved_config_t resolved = { 0 };
   if (conn->session) {
      session_get_llm_config(conn->session, &session_config);
   } else {
      /* No session yet - use global defaults */
      llm_get_default_config(&session_config);
   }
   llm_resolve_config(&session_config, &resolved);

   json_object_object_add(llm_runtime, "type",
                          json_object_new_string(resolved.type == LLM_LOCAL ? "local" : "cloud"));

   const char *provider_name = resolved.cloud_provider == CLOUD_PROVIDER_OPENAI   ? "OpenAI"
                               : resolved.cloud_provider == CLOUD_PROVIDER_CLAUDE ? "Claude"
                               : resolved.cloud_provider == CLOUD_PROVIDER_GEMINI ? "Gemini"
                                                                                  : "None";
   json_object_object_add(llm_runtime, "provider", json_object_new_string(provider_name));
   /* Get actual model name from config based on type/provider */
   const char *model_name = NULL;
   if (resolved.model && resolved.model[0] != '\0') {
      model_name = resolved.model;
   } else if (resolved.type == LLM_LOCAL) {
      model_name = g_config.llm.local.model[0] ? g_config.llm.local.model : "local";
   } else if (resolved.cloud_provider == CLOUD_PROVIDER_OPENAI) {
      model_name = llm_get_default_openai_model();
   } else if (resolved.cloud_provider == CLOUD_PROVIDER_CLAUDE) {
      model_name = llm_get_default_claude_model();
   } else if (resolved.cloud_provider == CLOUD_PROVIDER_GEMINI) {
      model_name = llm_get_default_gemini_model();
   }
   json_object_object_add(llm_runtime, "model",
                          json_object_new_string(model_name ? model_name : ""));
   json_object_object_add(llm_runtime, "openai_available",
                          json_object_new_boolean(llm_has_openai_key()));
   json_object_object_add(llm_runtime, "claude_available",
                          json_object_new_boolean(llm_has_claude_key()));
   json_object_object_add(llm_runtime, "gemini_available",
                          json_object_new_boolean(llm_has_gemini_key()));
   json_object_object_add(payload, "llm_runtime", llm_runtime);

   /* Add auth state for frontend UI visibility control */
   json_object_object_add(payload, "authenticated", json_object_new_boolean(conn->authenticated));
   json_object_object_add(payload, "is_admin", json_object_new_boolean(is_admin));
   if (conn->authenticated) {
      json_object_object_add(payload, "username", json_object_new_string(conn->username));
   }

   /* Add vision limits (server-authoritative values for client) */
   json_object *vision_limits = json_object_new_object();
   json_object_object_add(vision_limits, "max_images",
                          json_object_new_int(WEBUI_MAX_VISION_IMAGES));
   json_object_object_add(vision_limits, "max_image_size",
                          json_object_new_int(WEBUI_MAX_IMAGE_SIZE));
   json_object_object_add(vision_limits, "max_dimension",
                          json_object_new_int(WEBUI_MAX_IMAGE_DIMENSION));
   json_object_object_add(vision_limits, "max_thumbnail_size",
                          json_object_new_int(WEBUI_MAX_THUMBNAIL_SIZE));
   json_object_object_add(payload, "vision_limits", vision_limits);

   json_object_object_add(response, "payload", payload);

   /* Send response */
   const char *json_str = json_object_to_json_string(response);
   size_t json_len = strlen(json_str);
   unsigned char *buf = malloc(LWS_PRE + json_len);
   if (buf) {
      memcpy(buf + LWS_PRE, json_str, json_len);
      lws_write(conn->wsi, buf + LWS_PRE, json_len, LWS_WRITE_TEXT);
      free(buf);
   }

   json_object_put(response);
   LOG_INFO("WebUI: Sent configuration to client");
}

/* Helper to safely copy string from JSON to config field */
#define JSON_TO_CONFIG_STR(obj, key, dest)                \
   do {                                                   \
      struct json_object *_val;                           \
      if (json_object_object_get_ex(obj, key, &_val)) {   \
         const char *_str = json_object_get_string(_val); \
         if (_str) {                                      \
            strncpy(dest, _str, sizeof(dest) - 1);        \
            dest[sizeof(dest) - 1] = '\0';                \
         }                                                \
      }                                                   \
   } while (0)

#define JSON_TO_CONFIG_INT(obj, key, dest)              \
   do {                                                 \
      struct json_object *_val;                         \
      if (json_object_object_get_ex(obj, key, &_val)) { \
         dest = json_object_get_int(_val);              \
      }                                                 \
   } while (0)

#define JSON_TO_CONFIG_BOOL(obj, key, dest)             \
   do {                                                 \
      struct json_object *_val;                         \
      if (json_object_object_get_ex(obj, key, &_val)) { \
         dest = json_object_get_boolean(_val);          \
      }                                                 \
   } while (0)

#define JSON_TO_CONFIG_DOUBLE(obj, key, dest)           \
   do {                                                 \
      struct json_object *_val;                         \
      if (json_object_object_get_ex(obj, key, &_val)) { \
         dest = (float)json_object_get_double(_val);    \
      }                                                 \
   } while (0)

#define JSON_TO_CONFIG_SIZE_T(obj, key, dest)           \
   do {                                                 \
      struct json_object *_val;                         \
      if (json_object_object_get_ex(obj, key, &_val)) { \
         dest = (size_t)json_object_get_int64(_val);    \
      }                                                 \
   } while (0)

static void apply_config_from_json(dawn_config_t *config, struct json_object *payload) {
   struct json_object *section;

   /* [general] */
   if (json_object_object_get_ex(payload, "general", &section)) {
      JSON_TO_CONFIG_STR(section, "ai_name", config->general.ai_name);
      JSON_TO_CONFIG_STR(section, "log_file", config->general.log_file);
      JSON_TO_CONFIG_STR(section, "room", config->general.room);
   }

   /* [persona] */
   if (json_object_object_get_ex(payload, "persona", &section)) {
      JSON_TO_CONFIG_STR(section, "description", config->persona.description);
   }

   /* [localization] */
   if (json_object_object_get_ex(payload, "localization", &section)) {
      JSON_TO_CONFIG_STR(section, "location", config->localization.location);
      JSON_TO_CONFIG_STR(section, "timezone", config->localization.timezone);
      JSON_TO_CONFIG_STR(section, "units", config->localization.units);
   }

   /* [audio] */
   if (json_object_object_get_ex(payload, "audio", &section)) {
      JSON_TO_CONFIG_STR(section, "backend", config->audio.backend);
      JSON_TO_CONFIG_STR(section, "capture_device", config->audio.capture_device);
      JSON_TO_CONFIG_STR(section, "playback_device", config->audio.playback_device);
      JSON_TO_CONFIG_INT(section, "output_rate", config->audio.output_rate);
      JSON_TO_CONFIG_INT(section, "output_channels", config->audio.output_channels);

      struct json_object *bargein;
      if (json_object_object_get_ex(section, "bargein", &bargein)) {
         JSON_TO_CONFIG_BOOL(bargein, "enabled", config->audio.bargein.enabled);
         JSON_TO_CONFIG_INT(bargein, "cooldown_ms", config->audio.bargein.cooldown_ms);
         JSON_TO_CONFIG_INT(bargein, "startup_cooldown_ms",
                            config->audio.bargein.startup_cooldown_ms);
      }
   }

   /* [vad] */
   if (json_object_object_get_ex(payload, "vad", &section)) {
      JSON_TO_CONFIG_DOUBLE(section, "speech_threshold", config->vad.speech_threshold);
      JSON_TO_CONFIG_DOUBLE(section, "speech_threshold_tts", config->vad.speech_threshold_tts);
      JSON_TO_CONFIG_DOUBLE(section, "silence_threshold", config->vad.silence_threshold);
      JSON_TO_CONFIG_DOUBLE(section, "end_of_speech_duration", config->vad.end_of_speech_duration);
      JSON_TO_CONFIG_DOUBLE(section, "max_recording_duration", config->vad.max_recording_duration);
      JSON_TO_CONFIG_INT(section, "preroll_ms", config->vad.preroll_ms);

      struct json_object *chunking;
      if (json_object_object_get_ex(section, "chunking", &chunking)) {
         JSON_TO_CONFIG_BOOL(chunking, "enabled", config->vad.chunking.enabled);
         JSON_TO_CONFIG_DOUBLE(chunking, "pause_duration", config->vad.chunking.pause_duration);
         JSON_TO_CONFIG_DOUBLE(chunking, "min_duration", config->vad.chunking.min_duration);
         JSON_TO_CONFIG_DOUBLE(chunking, "max_duration", config->vad.chunking.max_duration);
      }
   }

   /* [asr] */
   if (json_object_object_get_ex(payload, "asr", &section)) {
      JSON_TO_CONFIG_STR(section, "model", config->asr.model);
      JSON_TO_CONFIG_STR(section, "models_path", config->asr.models_path);
   }

   /* [tts] */
   if (json_object_object_get_ex(payload, "tts", &section)) {
      JSON_TO_CONFIG_STR(section, "models_path", config->tts.models_path);
      JSON_TO_CONFIG_STR(section, "voice_model", config->tts.voice_model);
      JSON_TO_CONFIG_DOUBLE(section, "length_scale", config->tts.length_scale);
   }

   /* [commands] */
   if (json_object_object_get_ex(payload, "commands", &section)) {
      JSON_TO_CONFIG_STR(section, "processing_mode", config->commands.processing_mode);
   }

   /* [llm] */
   if (json_object_object_get_ex(payload, "llm", &section)) {
      JSON_TO_CONFIG_STR(section, "type", config->llm.type);
      JSON_TO_CONFIG_INT(section, "max_tokens", config->llm.max_tokens);

      struct json_object *cloud;
      if (json_object_object_get_ex(section, "cloud", &cloud)) {
         JSON_TO_CONFIG_STR(cloud, "provider", config->llm.cloud.provider);
         /* Validate cloud provider - must be openai, claude, or gemini */
         if (config->llm.cloud.provider[0] != '\0' &&
             strcmp(config->llm.cloud.provider, "openai") != 0 &&
             strcmp(config->llm.cloud.provider, "claude") != 0 &&
             strcmp(config->llm.cloud.provider, "gemini") != 0) {
            LOG_WARNING("WebUI: Invalid cloud.provider '%s', using 'openai'",
                        config->llm.cloud.provider);
            strncpy(config->llm.cloud.provider, "openai", sizeof(config->llm.cloud.provider) - 1);
         }
         JSON_TO_CONFIG_STR(cloud, "endpoint", config->llm.cloud.endpoint);
         JSON_TO_CONFIG_BOOL(cloud, "vision_enabled", config->llm.cloud.vision_enabled);

         /* Parse model lists from settings UI */
         struct json_object *openai_models_arr;
         if (json_object_object_get_ex(cloud, "openai_models", &openai_models_arr) &&
             json_object_is_type(openai_models_arr, json_type_array)) {
            config->llm.cloud.openai_models_count = 0;
            int arr_len = json_object_array_length(openai_models_arr);
            for (int i = 0; i < arr_len && i < LLM_CLOUD_MAX_MODELS; i++) {
               struct json_object *model_obj = json_object_array_get_idx(openai_models_arr, i);
               const char *model = json_object_get_string(model_obj);
               if (model && model[0] != '\0') {
                  strncpy(config->llm.cloud.openai_models[config->llm.cloud.openai_models_count],
                          model, LLM_CLOUD_MODEL_NAME_MAX - 1);
                  config->llm.cloud.openai_models[config->llm.cloud.openai_models_count]
                                                 [LLM_CLOUD_MODEL_NAME_MAX - 1] = '\0';
                  config->llm.cloud.openai_models_count++;
               }
            }
         }

         /* Parse default model index with bounds check */
         struct json_object *openai_idx_obj;
         if (json_object_object_get_ex(cloud, "openai_default_model_idx", &openai_idx_obj)) {
            int idx = json_object_get_int(openai_idx_obj);
            if (idx >= 0 && idx < config->llm.cloud.openai_models_count) {
               config->llm.cloud.openai_default_model_idx = idx;
            } else if (config->llm.cloud.openai_models_count > 0) {
               LOG_WARNING("WebUI: openai_default_model_idx %d out of range, using 0", idx);
               config->llm.cloud.openai_default_model_idx = 0;
            }
         }

         struct json_object *claude_models_arr;
         if (json_object_object_get_ex(cloud, "claude_models", &claude_models_arr) &&
             json_object_is_type(claude_models_arr, json_type_array)) {
            config->llm.cloud.claude_models_count = 0;
            int arr_len = json_object_array_length(claude_models_arr);
            for (int i = 0; i < arr_len && i < LLM_CLOUD_MAX_MODELS; i++) {
               struct json_object *model_obj = json_object_array_get_idx(claude_models_arr, i);
               const char *model = json_object_get_string(model_obj);
               if (model && model[0] != '\0') {
                  strncpy(config->llm.cloud.claude_models[config->llm.cloud.claude_models_count],
                          model, LLM_CLOUD_MODEL_NAME_MAX - 1);
                  config->llm.cloud.claude_models[config->llm.cloud.claude_models_count]
                                                 [LLM_CLOUD_MODEL_NAME_MAX - 1] = '\0';
                  config->llm.cloud.claude_models_count++;
               }
            }
         }

         /* Parse default model index with bounds check */
         struct json_object *claude_idx_obj;
         if (json_object_object_get_ex(cloud, "claude_default_model_idx", &claude_idx_obj)) {
            int idx = json_object_get_int(claude_idx_obj);
            if (idx >= 0 && idx < config->llm.cloud.claude_models_count) {
               config->llm.cloud.claude_default_model_idx = idx;
            } else if (config->llm.cloud.claude_models_count > 0) {
               LOG_WARNING("WebUI: claude_default_model_idx %d out of range, using 0", idx);
               config->llm.cloud.claude_default_model_idx = 0;
            }
         }

         struct json_object *gemini_models_arr;
         if (json_object_object_get_ex(cloud, "gemini_models", &gemini_models_arr) &&
             json_object_is_type(gemini_models_arr, json_type_array)) {
            config->llm.cloud.gemini_models_count = 0;
            int arr_len = json_object_array_length(gemini_models_arr);
            for (int i = 0; i < arr_len && i < LLM_CLOUD_MAX_MODELS; i++) {
               struct json_object *model_obj = json_object_array_get_idx(gemini_models_arr, i);
               const char *model = json_object_get_string(model_obj);
               if (model && model[0] != '\0') {
                  strncpy(config->llm.cloud.gemini_models[config->llm.cloud.gemini_models_count],
                          model, LLM_CLOUD_MODEL_NAME_MAX - 1);
                  config->llm.cloud.gemini_models[config->llm.cloud.gemini_models_count]
                                                 [LLM_CLOUD_MODEL_NAME_MAX - 1] = '\0';
                  config->llm.cloud.gemini_models_count++;
               }
            }
         }

         /* Parse default model index with bounds check */
         struct json_object *gemini_idx_obj;
         if (json_object_object_get_ex(cloud, "gemini_default_model_idx", &gemini_idx_obj)) {
            int idx = json_object_get_int(gemini_idx_obj);
            if (idx >= 0 && idx < config->llm.cloud.gemini_models_count) {
               config->llm.cloud.gemini_default_model_idx = idx;
            } else if (config->llm.cloud.gemini_models_count > 0) {
               LOG_WARNING("WebUI: gemini_default_model_idx %d out of range, using 0", idx);
               config->llm.cloud.gemini_default_model_idx = 0;
            }
         }
      }

      struct json_object *local;
      if (json_object_object_get_ex(section, "local", &local)) {
         JSON_TO_CONFIG_STR(local, "endpoint", config->llm.local.endpoint);
         JSON_TO_CONFIG_STR(local, "model", config->llm.local.model);
         JSON_TO_CONFIG_BOOL(local, "vision_enabled", config->llm.local.vision_enabled);
      }

      struct json_object *tools;
      if (json_object_object_get_ex(section, "tools", &tools)) {
         JSON_TO_CONFIG_STR(tools, "mode", config->llm.tools.mode);
         /* Validate tool mode - must be one of: native, command_tags, disabled */
         if (config->llm.tools.mode[0] != '\0' && strcmp(config->llm.tools.mode, "native") != 0 &&
             strcmp(config->llm.tools.mode, "command_tags") != 0 &&
             strcmp(config->llm.tools.mode, "disabled") != 0) {
            LOG_WARNING("WebUI: Invalid tools.mode '%s', using 'native'", config->llm.tools.mode);
            strncpy(config->llm.tools.mode, "native", sizeof(config->llm.tools.mode) - 1);
         }
      }

      struct json_object *thinking;
      if (json_object_object_get_ex(section, "thinking", &thinking)) {
         JSON_TO_CONFIG_STR(thinking, "mode", config->llm.thinking.mode);
         JSON_TO_CONFIG_STR(thinking, "reasoning_effort", config->llm.thinking.reasoning_effort);
         JSON_TO_CONFIG_INT(thinking, "budget_low", config->llm.thinking.budget_low);
         JSON_TO_CONFIG_INT(thinking, "budget_medium", config->llm.thinking.budget_medium);
         JSON_TO_CONFIG_INT(thinking, "budget_high", config->llm.thinking.budget_high);
      }

      /* Context management settings */
      JSON_TO_CONFIG_DOUBLE(section, "summarize_threshold", config->llm.summarize_threshold);
      JSON_TO_CONFIG_BOOL(section, "conversation_logging", config->llm.conversation_logging);
   }

   /* [search] */
   if (json_object_object_get_ex(payload, "search", &section)) {
      JSON_TO_CONFIG_STR(section, "engine", config->search.engine);
      JSON_TO_CONFIG_STR(section, "endpoint", config->search.endpoint);

      struct json_object *summarizer;
      if (json_object_object_get_ex(section, "summarizer", &summarizer)) {
         JSON_TO_CONFIG_STR(summarizer, "backend", config->search.summarizer.backend);
         JSON_TO_CONFIG_SIZE_T(summarizer, "threshold_bytes",
                               config->search.summarizer.threshold_bytes);
         JSON_TO_CONFIG_SIZE_T(summarizer, "target_words", config->search.summarizer.target_words);
         JSON_TO_CONFIG_DOUBLE(summarizer, "target_ratio", config->search.summarizer.target_ratio);
      }
   }

   /* [url_fetcher] */
   if (json_object_object_get_ex(payload, "url_fetcher", &section)) {
      struct json_object *flaresolverr;
      if (json_object_object_get_ex(section, "flaresolverr", &flaresolverr)) {
         JSON_TO_CONFIG_BOOL(flaresolverr, "enabled", config->url_fetcher.flaresolverr.enabled);
         JSON_TO_CONFIG_STR(flaresolverr, "endpoint", config->url_fetcher.flaresolverr.endpoint);
         JSON_TO_CONFIG_INT(flaresolverr, "timeout_sec",
                            config->url_fetcher.flaresolverr.timeout_sec);
         JSON_TO_CONFIG_SIZE_T(flaresolverr, "max_response_bytes",
                               config->url_fetcher.flaresolverr.max_response_bytes);
      }
   }

   /* [mqtt] */
   if (json_object_object_get_ex(payload, "mqtt", &section)) {
      JSON_TO_CONFIG_BOOL(section, "enabled", config->mqtt.enabled);
      JSON_TO_CONFIG_STR(section, "broker", config->mqtt.broker);
      JSON_TO_CONFIG_INT(section, "port", config->mqtt.port);
   }

   /* [network] */
   if (json_object_object_get_ex(payload, "network", &section)) {
      JSON_TO_CONFIG_BOOL(section, "enabled", config->network.enabled);
      JSON_TO_CONFIG_STR(section, "host", config->network.host);
      JSON_TO_CONFIG_INT(section, "port", config->network.port);
      JSON_TO_CONFIG_INT(section, "workers", config->network.workers);
      JSON_TO_CONFIG_INT(section, "socket_timeout_sec", config->network.socket_timeout_sec);
      JSON_TO_CONFIG_INT(section, "session_timeout_sec", config->network.session_timeout_sec);
      JSON_TO_CONFIG_INT(section, "llm_timeout_ms", config->network.llm_timeout_ms);
   }

   /* [tui] */
   if (json_object_object_get_ex(payload, "tui", &section)) {
      JSON_TO_CONFIG_BOOL(section, "enabled", config->tui.enabled);
   }

   /* [webui] */
   if (json_object_object_get_ex(payload, "webui", &section)) {
      JSON_TO_CONFIG_BOOL(section, "enabled", config->webui.enabled);
      JSON_TO_CONFIG_INT(section, "port", config->webui.port);
      JSON_TO_CONFIG_INT(section, "max_clients", config->webui.max_clients);
      JSON_TO_CONFIG_INT(section, "audio_chunk_ms", config->webui.audio_chunk_ms);
      JSON_TO_CONFIG_INT(section, "workers", config->webui.workers);
      JSON_TO_CONFIG_STR(section, "www_path", config->webui.www_path);
      JSON_TO_CONFIG_STR(section, "bind_address", config->webui.bind_address);
      JSON_TO_CONFIG_BOOL(section, "https", config->webui.https);
      JSON_TO_CONFIG_STR(section, "ssl_cert_path", config->webui.ssl_cert_path);
      JSON_TO_CONFIG_STR(section, "ssl_key_path", config->webui.ssl_key_path);
   }

   /* [memory] */
   if (json_object_object_get_ex(payload, "memory", &section)) {
      JSON_TO_CONFIG_BOOL(section, "enabled", config->memory.enabled);
      JSON_TO_CONFIG_INT(section, "context_budget_tokens", config->memory.context_budget_tokens);
      JSON_TO_CONFIG_STR(section, "extraction_provider", config->memory.extraction_provider);
      JSON_TO_CONFIG_STR(section, "extraction_model", config->memory.extraction_model);
      JSON_TO_CONFIG_BOOL(section, "pruning_enabled", config->memory.pruning_enabled);
      JSON_TO_CONFIG_INT(section, "prune_superseded_days", config->memory.prune_superseded_days);
      JSON_TO_CONFIG_INT(section, "prune_stale_days", config->memory.prune_stale_days);
      JSON_TO_CONFIG_DOUBLE(section, "prune_stale_min_confidence",
                            config->memory.prune_stale_min_confidence);
      JSON_TO_CONFIG_INT(section, "conversation_idle_timeout_min",
                         config->memory.conversation_idle_timeout_min);
      /* Clamp conversation idle timeout (0 = disabled, else 10-60 min) */
      if (config->memory.conversation_idle_timeout_min < 0) {
         config->memory.conversation_idle_timeout_min = 0;
      } else if (config->memory.conversation_idle_timeout_min > 0 &&
                 config->memory.conversation_idle_timeout_min < 10) {
         config->memory.conversation_idle_timeout_min = 10;
      } else if (config->memory.conversation_idle_timeout_min > 60) {
         config->memory.conversation_idle_timeout_min = 60;
      }
      JSON_TO_CONFIG_INT(section, "default_voice_user_id", config->memory.default_voice_user_id);
      /* Default voice user ID must be positive */
      if (config->memory.default_voice_user_id < 1) {
         config->memory.default_voice_user_id = 1;
      }
   }

   /* [shutdown] */
   if (json_object_object_get_ex(payload, "shutdown", &section)) {
      JSON_TO_CONFIG_BOOL(section, "enabled", config->shutdown.enabled);
      JSON_TO_CONFIG_STR(section, "passphrase", config->shutdown.passphrase);
   }

   /* [debug] */
   if (json_object_object_get_ex(payload, "debug", &section)) {
      JSON_TO_CONFIG_BOOL(section, "mic_record", config->debug.mic_record);
      JSON_TO_CONFIG_BOOL(section, "asr_record", config->debug.asr_record);
      JSON_TO_CONFIG_BOOL(section, "aec_record", config->debug.aec_record);
      JSON_TO_CONFIG_STR(section, "record_path", config->debug.record_path);
   }

   /* [paths] */
   if (json_object_object_get_ex(payload, "paths", &section)) {
      JSON_TO_CONFIG_STR(section, "data_dir", config->paths.data_dir);
      JSON_TO_CONFIG_STR(section, "music_dir", config->paths.music_dir);
   }

   /* [images] */
   if (json_object_object_get_ex(payload, "images", &section)) {
      JSON_TO_CONFIG_INT(section, "retention_days", config->images.retention_days);
      JSON_TO_CONFIG_INT(section, "max_size_mb", config->images.max_size_mb);
      JSON_TO_CONFIG_INT(section, "max_per_user", config->images.max_per_user);
   }

   /* [music] */
   if (json_object_object_get_ex(payload, "music", &section)) {
      JSON_TO_CONFIG_INT(section, "scan_interval_minutes", config->music.scan_interval_minutes);

      /* [music.streaming] */
      struct json_object *streaming = NULL;
      if (json_object_object_get_ex(section, "streaming", &streaming)) {
         JSON_TO_CONFIG_BOOL(streaming, "enabled", config->music.streaming_enabled);
         JSON_TO_CONFIG_STR(streaming, "default_quality", config->music.streaming_quality);
         JSON_TO_CONFIG_STR(streaming, "bitrate_mode", config->music.streaming_bitrate_mode);
      }
   }
}

void handle_set_config(ws_connection_t *conn, struct json_object *payload) {
   /* Admin-only operation */
   if (!conn_require_admin(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("set_config_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get the config file path */
   const char *config_path = config_get_loaded_path();
   if (!config_path || strcmp(config_path, "(none - using defaults)") == 0) {
      /* No config file loaded - use default path */
      config_path = "./dawn.toml";
   }

   /* Create backup before modifying */
   if (config_backup_file(config_path) != 0) {
      LOG_WARNING("WebUI: Failed to create config backup");
      /* Continue anyway - backup is optional */
   }

   /* Track tools mode changes for prompt rebuild */
   char old_tools_mode[16];
   strncpy(old_tools_mode, g_config.llm.tools.mode, sizeof(old_tools_mode) - 1);
   old_tools_mode[sizeof(old_tools_mode) - 1] = '\0';

   /* Track local endpoint changes for provider cache invalidation */
   char old_local_endpoint[128];
   strncpy(old_local_endpoint, g_config.llm.local.endpoint, sizeof(old_local_endpoint) - 1);
   old_local_endpoint[sizeof(old_local_endpoint) - 1] = '\0';

   /* Apply changes to global config with mutex protection.
    * The write lock ensures no other threads are reading config during modification. */
   pthread_rwlock_wrlock(&s_config_rwlock);
   dawn_config_t *mutable_config = (dawn_config_t *)config_get();
   apply_config_from_json(mutable_config, payload);
   pthread_rwlock_unlock(&s_config_rwlock);

   /* Check if tool calling mode changed */
   bool tools_mode_changed = (strcmp(old_tools_mode, g_config.llm.tools.mode) != 0);

   /* Write to file (outside lock - file I/O shouldn't block config reads) */
   int result = config_write_toml(mutable_config, config_path);

   if (result == 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message",
                             json_object_new_string("Configuration saved successfully"));
      LOG_INFO("WebUI: Configuration saved to %s", config_path);

      /* Apply runtime changes for LLM type if it was updated */
      struct json_object *llm_section = NULL;
      struct json_object *llm_type_obj = NULL;
      if (json_object_object_get_ex(payload, "llm", &llm_section) &&
          json_object_object_get_ex(llm_section, "type", &llm_type_obj)) {
         const char *new_type = json_object_get_string(llm_type_obj);
         if (new_type) {
            if (strcmp(new_type, "cloud") == 0) {
               int rc = llm_set_type(LLM_CLOUD);
               if (rc != 0) {
                  /* Update response to indicate partial success */
                  json_object_object_add(resp_payload, "warning",
                                         json_object_new_string(
                                             "Config saved but failed to switch to cloud LLM - "
                                             "API key not configured"));
               }
            } else if (strcmp(new_type, "local") == 0) {
               llm_set_type(LLM_LOCAL);
            }
         }
      }

      /* Apply runtime changes for cloud provider if it was updated */
      struct json_object *cloud_section = NULL;
      struct json_object *provider_obj = NULL;
      if (json_object_object_get_ex(payload, "llm", &llm_section) &&
          json_object_object_get_ex(llm_section, "cloud", &cloud_section) &&
          json_object_object_get_ex(cloud_section, "provider", &provider_obj)) {
         const char *new_provider = json_object_get_string(provider_obj);
         if (new_provider) {
            int rc = 0;
            if (strcmp(new_provider, "openai") == 0) {
               rc = llm_set_cloud_provider(CLOUD_PROVIDER_OPENAI);
            } else if (strcmp(new_provider, "claude") == 0) {
               rc = llm_set_cloud_provider(CLOUD_PROVIDER_CLAUDE);
            } else if (strcmp(new_provider, "gemini") == 0) {
               rc = llm_set_cloud_provider(CLOUD_PROVIDER_GEMINI);
            }
            if (rc != 0) {
               json_object_object_add(resp_payload, "warning",
                                      json_object_new_string(
                                          "Config saved but failed to switch cloud provider - "
                                          "API key not configured"));
            }
         }
      }

      /* Invalidate local provider and models cache if endpoint changed */
      if (strcmp(old_local_endpoint, g_config.llm.local.endpoint) != 0) {
         llm_local_invalidate_cache();
         llm_local_invalidate_models_cache();
         LOG_INFO("WebUI: Local LLM endpoint changed, invalidated provider and models cache");
      }

      /* If tool calling mode changed, rebuild system prompt for current session */
      if (tools_mode_changed) {
         invalidate_system_instructions();
         LOG_INFO("Tool calling mode changed (mode=%s), rebuilding prompt",
                  g_config.llm.tools.mode);

         /* Update current session's system prompt so change takes effect immediately */
         if (conn->session) {
            char *new_prompt = build_user_prompt(conn->auth_user_id);
            if (new_prompt) {
               session_update_system_prompt(conn->session, new_prompt);
               LOG_INFO("WebUI: Updated session prompt for tools mode change");
               free(new_prompt);
            }
         }
      }
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to write configuration file"));
      LOG_ERROR("WebUI: Failed to save configuration");
   }

   json_object_object_add(response, "payload", resp_payload);

   send_json_response(conn->wsi, response);
   json_object_put(response);
}

void handle_set_secrets(ws_connection_t *conn, struct json_object *payload) {
   /* Admin-only operation */
   if (!conn_require_admin(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("set_secrets_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get secrets file path */
   const char *secrets_path = config_get_secrets_path();
   if (!secrets_path || strcmp(secrets_path, "(none)") == 0) {
      /* No secrets file loaded - use default path */
      secrets_path = "./secrets.toml";
   }

   /* Create backup before modifying */
   config_backup_file(secrets_path);

   /* Get mutable secrets config */
   secrets_config_t *mutable_secrets = (secrets_config_t *)config_get_secrets();

   /* Apply changes from payload - only update fields that are provided */
   struct json_object *val;
   if (json_object_object_get_ex(payload, "openai_api_key", &val)) {
      const char *str = json_object_get_string(val);
      if (str) {
         strncpy(mutable_secrets->openai_api_key, str, sizeof(mutable_secrets->openai_api_key) - 1);
         mutable_secrets->openai_api_key[sizeof(mutable_secrets->openai_api_key) - 1] = '\0';
      }
   }
   if (json_object_object_get_ex(payload, "claude_api_key", &val)) {
      const char *str = json_object_get_string(val);
      if (str) {
         strncpy(mutable_secrets->claude_api_key, str, sizeof(mutable_secrets->claude_api_key) - 1);
         mutable_secrets->claude_api_key[sizeof(mutable_secrets->claude_api_key) - 1] = '\0';
      }
   }
   if (json_object_object_get_ex(payload, "gemini_api_key", &val)) {
      const char *str = json_object_get_string(val);
      if (str) {
         strncpy(mutable_secrets->gemini_api_key, str, sizeof(mutable_secrets->gemini_api_key) - 1);
         mutable_secrets->gemini_api_key[sizeof(mutable_secrets->gemini_api_key) - 1] = '\0';
      }
   }
   if (json_object_object_get_ex(payload, "mqtt_username", &val)) {
      const char *str = json_object_get_string(val);
      if (str) {
         strncpy(mutable_secrets->mqtt_username, str, sizeof(mutable_secrets->mqtt_username) - 1);
         mutable_secrets->mqtt_username[sizeof(mutable_secrets->mqtt_username) - 1] = '\0';
      }
   }
   if (json_object_object_get_ex(payload, "mqtt_password", &val)) {
      const char *str = json_object_get_string(val);
      if (str) {
         strncpy(mutable_secrets->mqtt_password, str, sizeof(mutable_secrets->mqtt_password) - 1);
         mutable_secrets->mqtt_password[sizeof(mutable_secrets->mqtt_password) - 1] = '\0';
      }
   }

   /* Write to file */
   int result = secrets_write_toml(mutable_secrets, secrets_path);

   if (result == 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message",
                             json_object_new_string("Secrets saved successfully"));

      /* Also update the secrets status */
      json_object *secrets_status = secrets_to_json_status(mutable_secrets);
      if (secrets_status) {
         json_object_object_add(resp_payload, "secrets", secrets_status);
      }

      /* Refresh LLM providers to pick up new API keys immediately */
      llm_refresh_providers();

      LOG_INFO("WebUI: Secrets saved to %s", secrets_path);
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to write secrets file"));
      LOG_ERROR("WebUI: Failed to save secrets");
   }

   json_object_object_add(response, "payload", resp_payload);

   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/* send_json_response is defined in webui_server.c */

/**
 * @brief Whitelisted shell commands for audio device enumeration
 *
 * SECURITY: Only these exact commands can be executed via run_whitelisted_command().
 * This prevents command injection even if a caller mistakenly passes user input.
 */
static const char *const ALLOWED_COMMANDS[] = {
   "arecord -L 2>/dev/null", "aplay -L 2>/dev/null", "pactl list sources short 2>/dev/null",
   "pactl list sinks short 2>/dev/null", NULL /* Sentinel */
};

/**
 * @brief Check if a command is in the whitelist
 */
static bool is_command_whitelisted(const char *cmd) {
   for (int i = 0; ALLOWED_COMMANDS[i] != NULL; i++) {
      if (strcmp(cmd, ALLOWED_COMMANDS[i]) == 0) {
         return true;
      }
   }
   return false;
}

/**
 * @brief Run a whitelisted shell command and capture output
 * @param cmd Command to run (must be in ALLOWED_COMMANDS whitelist)
 * @param output Buffer to store output
 * @param output_size Size of output buffer
 * @return Number of bytes read on success, 0 on error (with empty output)
 *
 * SECURITY: This function only executes commands that match the exact strings
 * in ALLOWED_COMMANDS. Any other command is rejected. This prevents command
 * injection even if caller accidentally passes user-controlled input.
 */
static size_t run_whitelisted_command(const char *cmd, char *output, size_t output_size) {
   if (output_size > 0) {
      output[0] = '\0';
   }

   /* Security check: only run whitelisted commands */
   if (!is_command_whitelisted(cmd)) {
      LOG_ERROR("WebUI: Blocked non-whitelisted command: %.50s...", cmd ? cmd : "(null)");
      return 0;
   }

   FILE *fp = popen(cmd, "r");
   if (!fp) {
      LOG_WARNING("WebUI: popen failed for command");
      return 0;
   }

   size_t total = 0;
   char buf[256];
   while (fgets(buf, sizeof(buf), fp) && total < output_size - 1) {
      size_t len = strlen(buf);
      if (total + len >= output_size) {
         len = output_size - total - 1;
      }
      memcpy(output + total, buf, len);
      total += len;
   }
   output[total] = '\0';

   pclose(fp);
   return total;
}

/**
 * @brief Parse ALSA device list (arecord -L or aplay -L output)
 */
static void parse_alsa_devices(const char *output, json_object *arr) {
   if (!output || !arr)
      return;

   /* ALSA -L output format:
    * devicename
    *     Description line
    *     ...
    * nextdevice
    */
   const char *line = output;
   while (*line) {
      /* Skip whitespace-prefixed description lines */
      if (*line != ' ' && *line != '\t' && *line != '\n') {
         /* This is a device name line */
         const char *end = strchr(line, '\n');
         size_t len = end ? (size_t)(end - line) : strlen(line);

         if (len > 0 && len < 256) {
            char device[256];
            strncpy(device, line, len);
            device[len] = '\0';

            /* Skip null device and some internal devices */
            if (strcmp(device, "null") != 0 && strncmp(device, "hw:", 3) != 0 &&
                strncmp(device, "plughw:", 7) != 0) {
               json_object_array_add(arr, json_object_new_string(device));
            }
         }
      }

      /* Move to next line */
      const char *next = strchr(line, '\n');
      if (!next)
         break;
      line = next + 1;
   }
}

/**
 * @brief Parse PulseAudio source/sink list (pactl list short output)
 * @param output Command output to parse
 * @param arr JSON array to add devices to
 * @param filter_monitors If true, filter out .monitor devices (for sources only)
 */
static void parse_pulse_devices(const char *output, json_object *arr, bool filter_monitors) {
   if (!output || !arr)
      return;

   /* pactl list sources/sinks short format:
    * index\tname\tmodule\tsample_spec\tstate
    */
   const char *line = output;
   while (*line) {
      /* Find the name field (second column, tab-separated) */
      const char *tab1 = strchr(line, '\t');
      if (tab1) {
         tab1++; /* Skip the tab */
         const char *tab2 = strchr(tab1, '\t');
         if (tab2) {
            size_t len = (size_t)(tab2 - tab1);
            if (len > 0 && len < 256) {
               char device[256];
               strncpy(device, tab1, len);
               device[len] = '\0';

               /* Filter out monitor sources if requested (they capture sink output, not mic input)
                */
               if (filter_monitors && strstr(device, ".monitor") != NULL) {
                  /* Skip to next line */
                  const char *next = strchr(line, '\n');
                  if (!next)
                     break;
                  line = next + 1;
                  continue;
               }

               /* Add device (PulseAudio names are usually descriptive) */
               json_object_array_add(arr, json_object_new_string(device));
            }
         }
      }

      /* Move to next line */
      const char *next = strchr(line, '\n');
      if (!next)
         break;
      line = next + 1;
   }
}

/* Audio device cache to avoid repeated popen() calls */
#define AUDIO_DEVICE_CACHE_TTL_SEC 30
#define AUDIO_DEVICE_BUFFER_SIZE 2048

static struct {
   time_t alsa_capture_time;
   time_t alsa_playback_time;
   time_t pulse_capture_time;
   time_t pulse_playback_time;
   char alsa_capture[AUDIO_DEVICE_BUFFER_SIZE];
   char alsa_playback[AUDIO_DEVICE_BUFFER_SIZE];
   char pulse_capture[AUDIO_DEVICE_BUFFER_SIZE];
   char pulse_playback[AUDIO_DEVICE_BUFFER_SIZE];
} s_device_cache = { 0 };
void handle_get_audio_devices(ws_connection_t *conn, struct json_object *payload) {
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("get_audio_devices_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get backend from payload */
   const char *backend = "auto";
   if (payload) {
      struct json_object *backend_obj;
      if (json_object_object_get_ex(payload, "backend", &backend_obj)) {
         backend = json_object_get_string(backend_obj);
      }
   }

   json_object *capture_devices = json_object_new_array();
   json_object *playback_devices = json_object_new_array();

   /* Always add "default" as first option */
   json_object_array_add(capture_devices, json_object_new_string("default"));
   json_object_array_add(playback_devices, json_object_new_string("default"));

   time_t now = time(NULL);

   if (strcmp(backend, "alsa") == 0) {
      /* Get ALSA devices (with caching) */
      if (now - s_device_cache.alsa_capture_time > AUDIO_DEVICE_CACHE_TTL_SEC) {
         if (run_whitelisted_command("arecord -L 2>/dev/null", s_device_cache.alsa_capture,
                                     sizeof(s_device_cache.alsa_capture)) > 0) {
            s_device_cache.alsa_capture_time = now;
         }
      }
      if (s_device_cache.alsa_capture[0]) {
         parse_alsa_devices(s_device_cache.alsa_capture, capture_devices);
      }

      if (now - s_device_cache.alsa_playback_time > AUDIO_DEVICE_CACHE_TTL_SEC) {
         if (run_whitelisted_command("aplay -L 2>/dev/null", s_device_cache.alsa_playback,
                                     sizeof(s_device_cache.alsa_playback)) > 0) {
            s_device_cache.alsa_playback_time = now;
         }
      }
      if (s_device_cache.alsa_playback[0]) {
         parse_alsa_devices(s_device_cache.alsa_playback, playback_devices);
      }
   } else if (strcmp(backend, "pulse") == 0) {
      /* Get PulseAudio devices (with caching) */
      if (now - s_device_cache.pulse_capture_time > AUDIO_DEVICE_CACHE_TTL_SEC) {
         if (run_whitelisted_command("pactl list sources short 2>/dev/null",
                                     s_device_cache.pulse_capture,
                                     sizeof(s_device_cache.pulse_capture)) > 0) {
            s_device_cache.pulse_capture_time = now;
         }
      }
      if (s_device_cache.pulse_capture[0]) {
         parse_pulse_devices(s_device_cache.pulse_capture, capture_devices,
                             true); /* Filter out .monitor sources */
      }

      if (now - s_device_cache.pulse_playback_time > AUDIO_DEVICE_CACHE_TTL_SEC) {
         if (run_whitelisted_command("pactl list sinks short 2>/dev/null",
                                     s_device_cache.pulse_playback,
                                     sizeof(s_device_cache.pulse_playback)) > 0) {
            s_device_cache.pulse_playback_time = now;
         }
      }
      if (s_device_cache.pulse_playback[0]) {
         parse_pulse_devices(s_device_cache.pulse_playback, playback_devices,
                             false); /* Sinks don't need filtering */
      }
   }
   /* For "auto", just return default - actual device selection happens at runtime */

   json_object_object_add(resp_payload, "backend", json_object_new_string(backend));
   json_object_object_add(resp_payload, "capture_devices", capture_devices);
   json_object_object_add(resp_payload, "playback_devices", playback_devices);
   json_object_object_add(response, "payload", resp_payload);

   const char *json_str = json_object_to_json_string(response);
   size_t json_len = strlen(json_str);
   unsigned char *buf = malloc(LWS_PRE + json_len);
   if (buf) {
      memcpy(buf + LWS_PRE, json_str, json_len);
      lws_write(conn->wsi, buf + LWS_PRE, json_len, LWS_WRITE_TEXT);
      free(buf);
   }

   json_object_put(response);
   LOG_INFO("WebUI: Sent audio devices for backend '%s'", backend);
}
/**
 * @brief Validate that a resolved path is within allowed directories
 *
 * Prevents path traversal attacks by ensuring model paths are in expected locations.
 */
static bool is_path_allowed(const char *resolved_path) {
   if (!resolved_path) {
      return false;
   }

   /* Get current working directory as base - always allowed */
   char cwd[CONFIG_PATH_MAX];
   if (getcwd(cwd, sizeof(cwd)) != NULL) {
      if (strncmp(resolved_path, cwd, strlen(cwd)) == 0) {
         return true;
      }
   }

   /* Check against allowed prefixes (defined at top of file) */
   for (int i = 0; s_allowed_path_prefixes[i] != NULL; i++) {
      if (strncmp(resolved_path, s_allowed_path_prefixes[i], strlen(s_allowed_path_prefixes[i])) ==
          0) {
         return true;
      }
   }

   return false;
}

/**
 * @brief Scan a directory for model files and build the response
 *
 * Internal helper that does the actual directory scanning.
 */
static json_object *scan_models_directory(void) {
   const dawn_config_t *config = config_get();
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("list_models_response"));
   json_object *payload = json_object_new_object();

   json_object *asr_models = json_object_new_array();
   json_object *tts_voices = json_object_new_array();

   /* Resolve ASR models path - use dynamic realpath to avoid buffer overflow */
   char asr_path[CONFIG_PATH_MAX];
   char *resolved = realpath(config->asr.models_path, NULL); /* Dynamic allocation */
   bool asr_valid = false;

   if (resolved) {
      asr_valid = is_path_allowed(resolved);
      strncpy(asr_path, resolved, sizeof(asr_path) - 1);
      asr_path[sizeof(asr_path) - 1] = '\0';
      free(resolved);
   } else {
      /* realpath failed - use original path with validation */
      strncpy(asr_path, config->asr.models_path, sizeof(asr_path) - 1);
      asr_path[sizeof(asr_path) - 1] = '\0';
      asr_valid = (asr_path[0] == '.' || is_path_allowed(asr_path));
   }

   if (!asr_valid) {
      LOG_WARNING("WebUI: ASR models path outside allowed directories: %s", asr_path);
   }

   /* Scan ASR models directory for ggml-*.bin files */
   if (asr_valid) {
      DIR *asr_dir = opendir(asr_path);
      if (asr_dir) {
         struct dirent *entry;
         while ((entry = readdir(asr_dir)) != NULL) {
            /* Look for ggml-*.bin files */
            if (entry->d_type == DT_REG || entry->d_type == DT_LNK || entry->d_type == DT_UNKNOWN) {
               const char *name = entry->d_name;
               if (strncmp(name, "ggml-", 5) == 0) {
                  const char *ext = strrchr(name, '.');
                  if (ext && strcmp(ext, ".bin") == 0) {
                     /* Extract model name between "ggml-" and ".bin" */
                     size_t model_len = ext - (name + 5);
                     if (model_len > 0 && model_len < 64) {
                        char model_name[64];
                        strncpy(model_name, name + 5, model_len);
                        model_name[model_len] = '\0';
                        json_object_array_add(asr_models, json_object_new_string(model_name));
                     }
                  }
               }
            }
         }
         closedir(asr_dir);
      } else {
         LOG_WARNING("WebUI: Could not open ASR models path: %s", asr_path);
      }
   }

   /* Resolve TTS models path - use dynamic realpath to avoid buffer overflow */
   char tts_path[CONFIG_PATH_MAX];
   resolved = realpath(config->tts.models_path, NULL); /* Dynamic allocation */
   bool tts_valid = false;

   if (resolved) {
      tts_valid = is_path_allowed(resolved);
      strncpy(tts_path, resolved, sizeof(tts_path) - 1);
      tts_path[sizeof(tts_path) - 1] = '\0';
      free(resolved);
   } else {
      /* realpath failed - use original path with validation */
      strncpy(tts_path, config->tts.models_path, sizeof(tts_path) - 1);
      tts_path[sizeof(tts_path) - 1] = '\0';
      tts_valid = (tts_path[0] == '.' || is_path_allowed(tts_path));
   }

   if (!tts_valid) {
      LOG_WARNING("WebUI: TTS models path outside allowed directories: %s", tts_path);
   }

   /* Scan TTS models directory for *.onnx files (excluding VAD models) */
   if (tts_valid) {
      DIR *tts_dir = opendir(tts_path);
      if (tts_dir) {
         struct dirent *entry;
         while ((entry = readdir(tts_dir)) != NULL) {
            if (entry->d_type == DT_REG || entry->d_type == DT_LNK || entry->d_type == DT_UNKNOWN) {
               const char *name = entry->d_name;
               const char *ext = strrchr(name, '.');
               /* Check extension and skip VAD models in single pass */
               if (ext && strcmp(ext, ".onnx") == 0 && strstr(name, "vad") == NULL &&
                   strstr(name, "VAD") == NULL) {
                  /* Extract voice name (filename without .onnx extension) */
                  size_t voice_len = ext - name;
                  if (voice_len > 0 && voice_len < 128) {
                     char voice_name[128];
                     strncpy(voice_name, name, voice_len);
                     voice_name[voice_len] = '\0';
                     json_object_array_add(tts_voices, json_object_new_string(voice_name));
                  }
               }
            }
         }
         closedir(tts_dir);
      } else {
         LOG_WARNING("WebUI: Could not open TTS models path: %s", tts_path);
      }
   }

   json_object_object_add(payload, "asr_models", asr_models);
   json_object_object_add(payload, "tts_voices", tts_voices);
   json_object_object_add(payload, "asr_path", json_object_new_string(config->asr.models_path));
   json_object_object_add(payload, "tts_path", json_object_new_string(config->tts.models_path));
   json_object_object_add(response, "payload", payload);

   LOG_INFO("WebUI: Scanned models (%zu ASR, %zu TTS)", json_object_array_length(asr_models),
            json_object_array_length(tts_voices));

   return response;
}

/**
 * @brief List available ASR and TTS models from configured paths
 *
 * Scans the configured model directories for:
 * - ASR: ggml-*.bin files (Whisper models) - extracts model name (tiny, base, small, etc.)
 * - TTS: *.onnx files (Piper voices) - returns full filename without extension
 *
 * Results are cached for MODEL_CACHE_TTL seconds to avoid repeated filesystem scans.
 */
void handle_list_models(ws_connection_t *conn) {
   time_t now = time(NULL);

   pthread_mutex_lock(&s_discovery_cache.cache_mutex);

   /* Check if cache is still valid */
   if (s_discovery_cache.models_response &&
       (now - s_discovery_cache.models_cache_time) < MODEL_CACHE_TTL) {
      /* Return cached response */
      send_json_response(conn->wsi, s_discovery_cache.models_response);
      LOG_INFO("WebUI: Sent cached model list");
      pthread_mutex_unlock(&s_discovery_cache.cache_mutex);
      return;
   }

   /* Invalidate old cache */
   if (s_discovery_cache.models_response) {
      json_object_put(s_discovery_cache.models_response);
      s_discovery_cache.models_response = NULL;
   }

   pthread_mutex_unlock(&s_discovery_cache.cache_mutex);

   /* Build new response (outside lock to avoid blocking) */
   json_object *response = scan_models_directory();

   /* Update cache */
   pthread_mutex_lock(&s_discovery_cache.cache_mutex);
   s_discovery_cache.models_response = json_object_get(response); /* Increment refcount */
   s_discovery_cache.models_cache_time = now;
   pthread_mutex_unlock(&s_discovery_cache.cache_mutex);

   /* Send response */
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Scan network interfaces and build the response
 *
 * Internal helper that does the actual interface enumeration.
 */
static json_object *scan_network_interfaces(void) {
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("list_interfaces_response"));
   json_object *payload = json_object_new_object();

   json_object *addresses = json_object_new_array();

   /* Track seen IPs efficiently without JSON library overhead */
   char seen_ips[16][INET_ADDRSTRLEN];
   int seen_count = 0;

   /* Always include common options first */
   json_object_array_add(addresses, json_object_new_string("0.0.0.0"));
   strncpy(seen_ips[seen_count++], "0.0.0.0", INET_ADDRSTRLEN);
   json_object_array_add(addresses, json_object_new_string("127.0.0.1"));
   strncpy(seen_ips[seen_count++], "127.0.0.1", INET_ADDRSTRLEN);

   /* Get actual interface addresses */
   struct ifaddrs *ifaddr, *ifa;
   if (getifaddrs(&ifaddr) == 0) {
      for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
         if (ifa->ifa_addr == NULL)
            continue;

         /* Only IPv4 addresses */
         if (ifa->ifa_addr->sa_family == AF_INET) {
            /* Skip loopback (already added 127.0.0.1) */
            if (ifa->ifa_flags & IFF_LOOPBACK)
               continue;

            struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
            char ip_str[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str))) {
               /* Check for duplicates using simple array (faster than JSON iteration) */
               int duplicate = 0;
               for (int j = 0; j < seen_count; j++) {
                  if (strcmp(seen_ips[j], ip_str) == 0) {
                     duplicate = 1;
                     break;
                  }
               }
               if (!duplicate && seen_count < 16) {
                  strncpy(seen_ips[seen_count++], ip_str, INET_ADDRSTRLEN);
                  json_object_array_add(addresses, json_object_new_string(ip_str));
               }
            }
         }
      }
      freeifaddrs(ifaddr);
   } else {
      LOG_WARNING("WebUI: getifaddrs failed: %s", strerror(errno));
      /* Continue with just 0.0.0.0 and 127.0.0.1 */
   }

   json_object_object_add(payload, "addresses", addresses);
   json_object_object_add(response, "payload", payload);

   LOG_INFO("WebUI: Scanned interfaces (%d addresses)", seen_count);
   return response;
}

/**
 * @brief List available network interfaces and their IP addresses
 *
 * Returns bind address options including:
 * - 0.0.0.0 (all interfaces)
 * - 127.0.0.1 (localhost)
 * - Individual interface IPs (e.g., 192.168.1.100)
 *
 * Results are cached for MODEL_CACHE_TTL seconds to avoid repeated system calls.
 */
void handle_list_interfaces(ws_connection_t *conn) {
   time_t now = time(NULL);

   pthread_mutex_lock(&s_discovery_cache.cache_mutex);

   /* Check if cache is still valid */
   if (s_discovery_cache.interfaces_response &&
       (now - s_discovery_cache.interfaces_cache_time) < MODEL_CACHE_TTL) {
      /* Return cached response */
      send_json_response(conn->wsi, s_discovery_cache.interfaces_response);
      LOG_INFO("WebUI: Sent cached interface list");
      pthread_mutex_unlock(&s_discovery_cache.cache_mutex);
      return;
   }

   /* Invalidate old cache */
   if (s_discovery_cache.interfaces_response) {
      json_object_put(s_discovery_cache.interfaces_response);
      s_discovery_cache.interfaces_response = NULL;
   }

   pthread_mutex_unlock(&s_discovery_cache.cache_mutex);

   /* Build new response (outside lock to avoid blocking) */
   json_object *response = scan_network_interfaces();

   /* Update cache */
   pthread_mutex_lock(&s_discovery_cache.cache_mutex);
   s_discovery_cache.interfaces_response = json_object_get(response); /* Increment refcount */
   s_discovery_cache.interfaces_cache_time = now;
   pthread_mutex_unlock(&s_discovery_cache.cache_mutex);

   /* Send response */
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/* =============================================================================
 * Local LLM Model Discovery
 * ============================================================================= */

void handle_list_llm_models(ws_connection_t *conn) {
   /* Require authentication */
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("list_llm_models_response"));

   json_object *payload = json_object_new_object();

   /* Get endpoint from config */
   const char *endpoint = g_config.llm.local.endpoint[0] != '\0' ? g_config.llm.local.endpoint
                                                                 : "http://127.0.0.1:8080";

   /* Detect provider */
   local_provider_t provider = llm_local_detect_provider(endpoint);

   /* Get models list */
   llm_local_model_t models[LLM_LOCAL_MAX_MODELS];
   size_t count = 0;
   int result = llm_local_list_models(endpoint, models, LLM_LOCAL_MAX_MODELS, &count);

   /* Build response */
   json_object *models_arr = json_object_new_array();

   if (result == 0 && count > 0) {
      for (size_t i = 0; i < count; i++) {
         json_object *model_obj = json_object_new_object();
         json_object_object_add(model_obj, "name", json_object_new_string(models[i].name));
         json_object_object_add(model_obj, "loaded", json_object_new_boolean(models[i].loaded));
         json_object_array_add(models_arr, model_obj);
      }
   }

   json_object_object_add(payload, "models", models_arr);
   json_object_object_add(payload, "provider",
                          json_object_new_string(llm_local_provider_name(provider)));
   json_object_object_add(payload, "endpoint", json_object_new_string(endpoint));

   /* Determine current model - provider-specific logic */
   const char *current_model = NULL;

   /* For llama.cpp: always use actual loaded model (server can only run one model)
    * Config/session settings are not meaningful since llama.cpp can't switch models */
   if (provider == LOCAL_PROVIDER_LLAMA_CPP && count > 0) {
      current_model = models[0].name;
      LOG_INFO("WebUI: llama.cpp actual loaded model: %s", current_model);
   }
   /* For Ollama and others: priority is session > config > first available */
   else {
      /* Check session config first */
      if (conn->session) {
         session_llm_config_t session_config;
         session_get_llm_config(conn->session, &session_config);
         if (session_config.model[0] != '\0') {
            current_model = session_config.model;
         }
      }

      /* Fall back to global config */
      if (!current_model && g_config.llm.local.model[0] != '\0') {
         current_model = g_config.llm.local.model;
      }

      /* Fall back to first available model */
      if (!current_model && count > 0) {
         current_model = models[0].name;
      }
   }

   /* Last resort: unknown */
   if (!current_model || current_model[0] == '\0') {
      current_model = "(unknown)";
   }

   json_object_object_add(payload, "current_model", json_object_new_string(current_model));
   json_object_object_add(payload, "count", json_object_new_int((int)count));

   json_object_object_add(response, "payload", payload);

   send_json_response(conn->wsi, response);
   json_object_put(response);

   LOG_INFO("WebUI: Sent local LLM models list (%zu models from %s)", count,
            llm_local_provider_name(provider));
}
