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
 * WebUI WebSocket JSON message dispatcher.
 *
 * Owns `handle_json_message` — the central switch over every
 * client-to-server JSON message type — plus the always-on voice mode
 * enable/disable handlers and the cancel-message handler that ride
 * with it (they're tiny and conceptually part of the dispatch surface).
 * Every other case in the switch routes to a `handle_*` helper defined
 * in a sibling webui_*.c file via webui_internal.h.
 *
 * Split out of webui_server.c so that file can stay under the size
 * limits in CLAUDE.md.  All cross-module entry points are declared in
 * webui_internal.h or webui_server.h; the connection-registry state
 * (s_active_connections + s_conn_registry_mutex) is reached via the
 * externs in webui_internal.h.
 */

#include <ctype.h>
#include <json-c/json.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/missed_notifications_db.h"
#include "core/scheduler.h"
#include "core/scheduler_db.h"
#include "core/session_manager.h"
#include "dawn.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_context.h"
#include "llm/llm_local_provider.h"
#include "logging.h"
#include "webui/webui_always_on.h"
#include "webui/webui_contacts.h"
#include "webui/webui_doc_library.h"
#ifdef DAWN_ENABLE_CODE_PROJECTS
#include "webui/webui_code_projects.h"
#endif
#include "webui/webui_email.h"
#include "webui/webui_internal.h"
#include "webui/webui_oauth.h"
#include "webui/webui_ota.h"
#include "webui/webui_server.h"

/* handle_cancel_message is defined at the bottom of this TU. */
static void handle_cancel_message(ws_connection_t *conn);

/* handle_always_on_enable / handle_always_on_disable moved to
 * webui_always_on.c (next to always_on_create / always_on_destroy);
 * declarations in webui_always_on.h. */

void handle_json_message(ws_connection_t *conn, const char *data, size_t len) {
   /* Null-terminate for JSON parsing */
   char *json_str = strndup(data, len);
   if (!json_str) {
      OLOG_ERROR("WebUI: Failed to allocate JSON string");
      return;
   }

   struct json_object *root = json_tokener_parse(json_str);
   if (!root) {
      OLOG_WARNING("WebUI: Invalid JSON received: %.*s", (int)len, data);
      free(json_str);
      return;
   }

   /* Get message type */
   struct json_object *type_obj;
   if (!json_object_object_get_ex(root, "type", &type_obj)) {
      OLOG_WARNING("WebUI: JSON missing 'type' field");
      json_object_put(root);
      free(json_str);
      return;
   }

   const char *type = json_object_get_string(type_obj);
   struct json_object *payload;
   json_object_object_get_ex(root, "payload", &payload);

   if (strcmp(type, "text") == 0) {
      /* Text input from user (with optional vision images - supports multiple) */
      if (payload) {
         struct json_object *text_obj;
         if (json_object_object_get_ex(payload, "text", &text_obj)) {
            const char *text = json_object_get_string(text_obj);
            if (text && strlen(text) > 0) {
               /* Extract optional images for vision (array format) */
               const char *vision_images[WEBUI_MAX_VISION_IMAGES_CAP] = { 0 };
               size_t vision_image_sizes[WEBUI_MAX_VISION_IMAGES_CAP] = { 0 };
               const char *vision_mimes[WEBUI_MAX_VISION_IMAGES_CAP] = { 0 };
               int vision_image_count = 0;
               const int max_vision_images = g_config.vision.max_images;

               struct json_object *images_obj;
               if (json_object_object_get_ex(payload, "images", &images_obj) &&
                   json_object_is_type(images_obj, json_type_array)) {
                  int array_len = json_object_array_length(images_obj);
                  if (array_len > max_vision_images) {
                     OLOG_WARNING("WebUI: Too many images (%d), limiting to %d", array_len,
                                  max_vision_images);
                     array_len = max_vision_images;
                  }

                  for (int i = 0; i < array_len; i++) {
                     struct json_object *image_obj = json_object_array_get_idx(images_obj, i);
                     struct json_object *data_obj, *mime_obj;
                     if (json_object_object_get_ex(image_obj, "data", &data_obj) &&
                         json_object_object_get_ex(image_obj, "mime_type", &mime_obj)) {
                        const char *img_data = json_object_get_string(data_obj);
                        const char *img_mime = json_object_get_string(mime_obj);
                        if (img_data) {
                           size_t img_size = strlen(img_data);

                           /* Validate each image BEFORE passing to handler */
                           int val_result = validate_image_data(img_data, img_size, img_mime);
                           if (val_result != 0) {
                              const char *err_msg = "Image validation failed";
                              switch (val_result) {
                                 case 1:
                                    err_msg = "Unsupported image type";
                                    break;
                                 case 2:
                                    err_msg = "Image too large (max 4MB)";
                                    break;
                                 case 3:
                                    err_msg = "Invalid image data encoding";
                                    break;
                                 case 4:
                                    err_msg = "Image format doesn't match declared type";
                                    break;
                              }
                              send_error_impl(conn->wsi, "INVALID_IMAGE", err_msg);
                              json_object_put(root);
                              free(json_str);
                              return;
                           }

                           vision_images[vision_image_count] = img_data;
                           vision_image_sizes[vision_image_count] = img_size;
                           vision_mimes[vision_image_count] = img_mime;
                           vision_image_count++;
                        }
                     }
                  }

                  if (vision_image_count > 0) {
                     OLOG_INFO("WebUI: %d vision image(s) attached", vision_image_count);
                  }
               }

               handle_text_message(conn, text, strlen(text), vision_images, vision_image_sizes,
                                   vision_mimes, vision_image_count);
            }
         }
      }
   } else if (strcmp(type, "cancel") == 0) {
      /* Cancel current operation */
      handle_cancel_message(conn);
   } else if (strcmp(type, "get_config") == 0) {
      /* Request current configuration */
      handle_get_config(conn);
   } else if (strcmp(type, "get_system_prompt") == 0) {
      /* Request current system prompt for debugging.  Uses the FULL
       * variant so the inspector renders both segments of the two-
       * message shape (stable prefix + volatile block joined by
       * "\n\n").  session_get_system_prompt would show only the
       * cached stable prefix — useful elsewhere but misleading for
       * "what does the LLM actually see this turn?". */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type", json_object_new_string("system_prompt_response"));
      struct json_object *resp_payload = json_object_new_object();

      if (conn->session) {
         char *prompt = session_get_full_system_prompt(conn->session);
         if (prompt) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
            json_object_object_add(resp_payload, "prompt", json_object_new_string(prompt));
            json_object_object_add(resp_payload, "length",
                                   json_object_new_int((int)strlen(prompt)));
            free(prompt);
         } else {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string("No system prompt found"));
         }
      } else {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error", json_object_new_string("No active session"));
      }

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
   } else if (strcmp(type, "set_config") == 0) {
      /* Update configuration settings */
      if (payload) {
         handle_set_config(conn, payload);
      }
   } else if (strcmp(type, "set_secrets") == 0) {
      /* Update secrets (API keys, credentials) */
      if (payload) {
         handle_set_secrets(conn, payload);
      }
   } else if (strcmp(type, "get_audio_devices") == 0) {
      /* Request available audio devices for given backend */
      handle_get_audio_devices(conn, payload);
   } else if (strcmp(type, "list_models") == 0) {
      /* Request available ASR and TTS models */
      handle_list_models(conn);
   } else if (strcmp(type, "list_interfaces") == 0) {
      /* Request available network interfaces */
      handle_list_interfaces(conn);
   } else if (strcmp(type, "list_llm_models") == 0) {
      /* Request available local LLM models (Ollama/llama.cpp) */
      handle_list_llm_models(conn);
   } else if (strcmp(type, "restart") == 0) {
      /* Admin-only operation */
      if (!conn_require_admin(conn)) {
         return;
      }

      /* Request application restart */
      OLOG_INFO("WebUI: Restart requested by client '%s'", conn->username);

      /* Send confirmation response before initiating restart */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type", json_object_new_string("restart_response"));
      struct json_object *resp_payload = json_object_new_object();
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message",
                             json_object_new_string("DAWN is restarting..."));
      json_object_object_add(response, "payload", resp_payload);

      send_json_message(conn->wsi, json_object_to_json_string(response));
      json_object_put(response);

      /* Request restart - this will trigger clean shutdown and re-exec */
      dawn_request_restart();
   } else if (strcmp(type, "set_llm_runtime") == 0) {
      /* Admin-only: affects all clients */
      if (!conn_require_admin(conn)) {
         return;
      }
      /* Switch LLM type or provider at runtime (immediate effect, no restart) */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type", json_object_new_string("set_llm_runtime_response"));
      struct json_object *resp_payload = json_object_new_object();

      int success = 1;
      const char *error_msg = NULL;

      if (payload) {
         struct json_object *type_obj, *provider_obj;

         /* Handle LLM type change (local/cloud) */
         if (json_object_object_get_ex(payload, "type", &type_obj)) {
            const char *new_type = json_object_get_string(type_obj);
            if (new_type) {
               if (strcmp(new_type, "local") == 0) {
                  llm_set_type(LLM_LOCAL);
                  OLOG_INFO("WebUI: Switched to local LLM");
               } else if (strcmp(new_type, "cloud") == 0) {
                  /* When switching to cloud, ensure we have a valid provider selected.
                   * Under the OpenRouter gateway, the provider is always OpenRouter;
                   * otherwise prefer OpenAI, then Claude, then Gemini. */
                  if (llm_openrouter_gateway_enabled() && llm_has_openrouter_key()) {
                     llm_set_cloud_provider(CLOUD_PROVIDER_OPENROUTER);
                  } else if (llm_has_openai_key()) {
                     llm_set_cloud_provider(CLOUD_PROVIDER_OPENAI);
                  } else if (llm_has_claude_key()) {
                     llm_set_cloud_provider(CLOUD_PROVIDER_CLAUDE);
                  } else if (llm_has_gemini_key()) {
                     llm_set_cloud_provider(CLOUD_PROVIDER_GEMINI);
                  }
                  int rc = llm_set_type(LLM_CLOUD);
                  if (rc != 0) {
                     success = 0;
                     error_msg = "No cloud API key configured in secrets.toml";
                  } else {
                     OLOG_INFO("WebUI: Switched to cloud LLM");
                  }
               }
            }
         }

         /* Handle cloud provider change (openai/claude/gemini) */
         if (success && json_object_object_get_ex(payload, "provider", &provider_obj)) {
            const char *new_provider = json_object_get_string(provider_obj);
            if (new_provider) {
               int rc = 0;
               if (strcmp(new_provider, "openai") == 0) {
                  rc = llm_set_cloud_provider(CLOUD_PROVIDER_OPENAI);
               } else if (strcmp(new_provider, "claude") == 0) {
                  rc = llm_set_cloud_provider(CLOUD_PROVIDER_CLAUDE);
               } else if (strcmp(new_provider, "gemini") == 0) {
                  rc = llm_set_cloud_provider(CLOUD_PROVIDER_GEMINI);
               } else if (strcmp(new_provider, "openrouter") == 0) {
                  rc = llm_set_cloud_provider(CLOUD_PROVIDER_OPENROUTER);
               }
               if (rc != 0) {
                  success = 0;
                  error_msg = "API key not configured for this provider";
               } else {
                  OLOG_INFO("WebUI: Switched cloud provider to %s", new_provider);
               }
            }
         }
      }

      json_object_object_add(resp_payload, "success", json_object_new_boolean(success));
      if (error_msg) {
         json_object_object_add(resp_payload, "error", json_object_new_string(error_msg));
      }

      /* Return updated runtime state */
      llm_type_t current_type = llm_get_type();
      json_object_object_add(resp_payload, "type",
                             json_object_new_string(current_type == LLM_LOCAL ? "local" : "cloud"));
      json_object_object_add(resp_payload, "provider",
                             json_object_new_string(llm_get_cloud_provider_name()));
      json_object_object_add(resp_payload, "model", json_object_new_string(llm_get_model_name()));

      /* Include API key availability so client can populate provider dropdown */
      json_object_object_add(resp_payload, "openai_available",
                             json_object_new_boolean(llm_has_openai_key()));
      json_object_object_add(resp_payload, "claude_available",
                             json_object_new_boolean(llm_has_claude_key()));
      json_object_object_add(resp_payload, "gemini_available",
                             json_object_new_boolean(llm_has_gemini_key()));
      json_object_object_add(resp_payload, "openrouter_available",
                             json_object_new_boolean(llm_has_openrouter_key()));

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
   } else if (strcmp(type, "set_session_llm") == 0) {
      /* Per-session LLM configuration (does NOT affect other clients) */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type", json_object_new_string("set_session_llm_response"));
      struct json_object *resp_payload = json_object_new_object();

      int success = 1;
      const char *error_msg = NULL;

      if (!conn->session) {
         success = 0;
         error_msg = "No active session";
      } else if (payload) {
         struct json_object *type_obj, *provider_obj;

         /* Get current session config as starting point */
         session_llm_config_t config;
         session_get_llm_config(conn->session, &config);
         bool has_changes = false;

         /* Track old tool_mode for prompt rebuild */
         char old_tool_mode[LLM_TOOL_MODE_MAX];
         strncpy(old_tool_mode, config.tool_mode, sizeof(old_tool_mode) - 1);
         old_tool_mode[sizeof(old_tool_mode) - 1] = '\0';

         /* Track old model + type for context cache invalidation */
         char old_model[LLM_MODEL_NAME_MAX];
         strncpy(old_model, config.model, sizeof(old_model) - 1);
         old_model[sizeof(old_model) - 1] = '\0';
         int old_type = config.type;

         /* Parse type (local/cloud) */
         if (json_object_object_get_ex(payload, "type", &type_obj)) {
            const char *new_type = json_object_get_string(type_obj);
            if (new_type) {
               has_changes = true;
               if (strcmp(new_type, "local") == 0) {
                  config.type = LLM_LOCAL;
               } else if (strcmp(new_type, "cloud") == 0) {
                  config.type = LLM_CLOUD;
                  /* If no provider is set, pick the first one with an API key */
                  if (config.cloud_provider == CLOUD_PROVIDER_NONE) {
                     config.cloud_provider = llm_detect_available_provider();
                     if (config.cloud_provider != CLOUD_PROVIDER_NONE) {
                        OLOG_INFO("WebUI: Auto-selected %s (API key available)",
                                  cloud_provider_to_string(config.cloud_provider));
                     } else {
                        OLOG_WARNING("WebUI: No cloud API keys found — configure in "
                                     "Settings or secrets.toml");
                     }
                  }
               } else if (strcmp(new_type, "reset") == 0) {
                  /* Reset to defaults from dawn.toml */
                  session_clear_llm_config(conn->session);
                  OLOG_INFO("WebUI: Session %u LLM config reset to defaults",
                            conn->session->session_id);
                  has_changes = false; /* Already handled */
               }
            }
         }

         /* Parse provider (openai/claude/gemini) */
         bool provider_explicitly_set = false;
         if (json_object_object_get_ex(payload, "provider", &provider_obj)) {
            const char *new_provider = json_object_get_string(provider_obj);
            if (new_provider && new_provider[0] != '\0') {
               has_changes = true;
               provider_explicitly_set = true;
               if (strcmp(new_provider, "openai") == 0) {
                  config.cloud_provider = CLOUD_PROVIDER_OPENAI;
               } else if (strcmp(new_provider, "claude") == 0) {
                  config.cloud_provider = CLOUD_PROVIDER_CLAUDE;
               } else if (strcmp(new_provider, "gemini") == 0) {
                  config.cloud_provider = CLOUD_PROVIDER_GEMINI;
               } else if (strcmp(new_provider, "openrouter") == 0) {
                  config.cloud_provider = CLOUD_PROVIDER_OPENROUTER;
               }
            }
         }

         /* Parse model */
         struct json_object *model_obj;
         if (json_object_object_get_ex(payload, "model", &model_obj)) {
            const char *new_model = json_object_get_string(model_obj);
            if (new_model && new_model[0] != '\0') {
               /* Validate model name to prevent injection attacks */
               if (llm_local_is_valid_model_name(new_model)) {
                  has_changes = true;
                  strncpy(config.model, new_model, sizeof(config.model) - 1);
                  config.model[sizeof(config.model) - 1] = '\0';
                  /* "requested", not "set": under the OpenRouter gateway a bare
                   * (non vendor/model) id is dropped below and the resolver
                   * substitutes the gateway default — so this value is not
                   * necessarily what runs.  Logging it as "set" previously
                   * implied a model switch that the gateway then discarded. */
                  OLOG_INFO("WebUI: Session model requested '%s'", config.model);

                  /* Infer provider from model name if not explicitly set
                   * (handles old conversations and frontend bugs).
                   * Only infer if the inferred provider has an API key available.
                   * Skipped under the OpenRouter gateway — IDs are "vendor/model"
                   * and the gateway forces OPENROUTER below. */
                  if (!provider_explicitly_set && !llm_openrouter_gateway_enabled()) {
                     if ((strncmp(new_model, "gpt-", 4) == 0 || strncmp(new_model, "o1-", 3) == 0 ||
                          strncmp(new_model, "o3-", 3) == 0) &&
                         llm_has_openai_key()) {
                        config.cloud_provider = CLOUD_PROVIDER_OPENAI;
                        OLOG_INFO("WebUI: Inferred OpenAI provider from model '%s'", new_model);
                     } else if (strncmp(new_model, "claude-", 7) == 0 && llm_has_claude_key()) {
                        config.cloud_provider = CLOUD_PROVIDER_CLAUDE;
                        OLOG_INFO("WebUI: Inferred Claude provider from model '%s'", new_model);
                     } else if (strncmp(new_model, "gemini-", 7) == 0 && llm_has_gemini_key()) {
                        config.cloud_provider = CLOUD_PROVIDER_GEMINI;
                        OLOG_INFO("WebUI: Inferred Gemini provider from model '%s'", new_model);
                     }
                  }
               } else {
                  OLOG_WARNING("WebUI: Rejected invalid model name from client");
               }
            }
         }

         /* OpenRouter gateway is the single authority: any cloud session config
          * change is pinned to OpenRouter regardless of the requested provider.
          * OpenRouter model IDs are "vendor/model"; if a bare model name slipped in
          * (e.g. from a not-yet-gateway-aware control), drop it so the resolver uses
          * the OpenRouter default instead of sending an unrecognized ID. */
         if (config.type == LLM_CLOUD && llm_openrouter_gateway_enabled()) {
            config.cloud_provider = CLOUD_PROVIDER_OPENROUTER;
            if (config.model[0] != '\0' && strchr(config.model, '/') == NULL) {
               OLOG_INFO("WebUI: OpenRouter gateway dropped bare model '%s' (not a vendor/model "
                         "id) — resolver will use the OpenRouter default",
                         config.model);
               config.model[0] = '\0';
            }
         }

         /* Parse tool_mode (native/command_tags/disabled) */
         struct json_object *tool_mode_obj;
         if (json_object_object_get_ex(payload, "tool_mode", &tool_mode_obj)) {
            const char *new_tool_mode = json_object_get_string(tool_mode_obj);
            if (new_tool_mode) {
               /* Validate tool mode value */
               if (strcmp(new_tool_mode, "native") == 0 ||
                   strcmp(new_tool_mode, "command_tags") == 0 ||
                   strcmp(new_tool_mode, "disabled") == 0) {
                  has_changes = true;
                  strncpy(config.tool_mode, new_tool_mode, sizeof(config.tool_mode) - 1);
                  config.tool_mode[sizeof(config.tool_mode) - 1] = '\0';
                  OLOG_INFO("WebUI: Session tool_mode set to '%s'", config.tool_mode);
               } else {
                  OLOG_WARNING("WebUI: Rejected invalid tool_mode '%s' from client", new_tool_mode);
               }
            }
         }

         /* Parse thinking_mode (disabled/auto/enabled) */
         struct json_object *thinking_mode_obj;
         if (json_object_object_get_ex(payload, "thinking_mode", &thinking_mode_obj)) {
            const char *new_thinking_mode = json_object_get_string(thinking_mode_obj);
            if (new_thinking_mode) {
               /* Validate thinking mode value */
               if (strcmp(new_thinking_mode, "disabled") == 0 ||
                   strcmp(new_thinking_mode, "auto") == 0 ||
                   strcmp(new_thinking_mode, "enabled") == 0) {
                  has_changes = true;
                  strncpy(config.thinking_mode, new_thinking_mode,
                          sizeof(config.thinking_mode) - 1);
                  config.thinking_mode[sizeof(config.thinking_mode) - 1] = '\0';
                  OLOG_INFO("WebUI: Session thinking_mode set to '%s'", config.thinking_mode);
               } else {
                  OLOG_WARNING("WebUI: Rejected invalid thinking_mode '%s' from client",
                               new_thinking_mode);
               }
            }
         }

         /* Parse reasoning_effort. The allowlist mirrors the gpt-5.4 Responses API
          * (none/low/medium/high/xhigh); Claude maps low/medium/high to budget
          * tokens via llm_get_effective_budget_tokens. */
         struct json_object *reasoning_effort_obj;
         if (json_object_object_get_ex(payload, "reasoning_effort", &reasoning_effort_obj)) {
            const char *new_effort = json_object_get_string(reasoning_effort_obj);
            if (new_effort) {
               if (strcmp(new_effort, "none") == 0 || strcmp(new_effort, "low") == 0 ||
                   strcmp(new_effort, "medium") == 0 || strcmp(new_effort, "high") == 0 ||
                   strcmp(new_effort, "xhigh") == 0) {
                  has_changes = true;
                  strncpy(config.reasoning_effort, new_effort, sizeof(config.reasoning_effort) - 1);
                  config.reasoning_effort[sizeof(config.reasoning_effort) - 1] = '\0';
                  OLOG_INFO("WebUI: Session reasoning_effort set to '%s'", config.reasoning_effort);
               } else {
                  OLOG_WARNING("WebUI: Rejected invalid reasoning_effort '%s' from client",
                               new_effort);
               }
            }
         }

         /* Apply config if changes were made */
         if (has_changes) {
            int rc = session_set_llm_config(conn->session, &config);
            if (rc != 0) {
               success = 0;
               error_msg = "API key not configured for requested provider";
            } else {
               OLOG_INFO("WebUI: Session %u LLM config updated (type=%d, provider=%d)",
                         conn->session->session_id, config.type, config.cloud_provider);

               /* If local model or LLM type changed, context size may differ */
               if (config.type == LLM_LOCAL &&
                   (strcmp(old_model, config.model) != 0 || old_type != config.type)) {
                  llm_context_refresh_local();
               }

               /* If tool_mode changed, rebuild and update the session's system prompt */
               if (strcmp(old_tool_mode, config.tool_mode) != 0) {
                  invalidate_system_instructions(); /* Clear cached prompt first */
                  char *new_prompt = build_remote_prompt_for_mode(config.tool_mode);
                  if (new_prompt) {
                     session_update_system_prompt(conn->session, new_prompt);
                     OLOG_INFO("WebUI: Updated session prompt for tool_mode change to '%s'",
                               config.tool_mode);
                     free(new_prompt);
                  }
               }

               /* Persist LLM settings to the active conversation DB so that
                * session recreation (after timeout) restores the latest config.
                *
                * Skip when the client tagged this as a restore-push: those carry
                * the loaded conversation's own settings, but rapid clicks can race
                * such that `active_conversation_id` has already advanced to the
                * NEXT conversation by the time we process this message — the
                * cascade would then silently overwrite that conv with the
                * previous conv's values. */
               bool from_restore = false;
               struct json_object *restore_obj;
               if (json_object_object_get_ex(payload, "from_restore", &restore_obj)) {
                  from_restore = json_object_get_boolean(restore_obj);
               }
               if (!from_restore && conn->active_conversation_id > 0) {
                  const char *type_str = config.type == LLM_LOCAL ? "local" : "cloud";
                  conv_db_update_llm_settings(conn->active_conversation_id, conn->auth_user_id,
                                              type_str,
                                              cloud_provider_to_string(config.cloud_provider),
                                              config.model, config.tool_mode, config.thinking_mode,
                                              config.reasoning_effort);
               }
            }
         }
      }

      json_object_object_add(resp_payload, "success", json_object_new_boolean(success));
      if (error_msg) {
         json_object_object_add(resp_payload, "error", json_object_new_string(error_msg));
      }

      /* Return current session config for confirmation */
      if (conn->session) {
         session_llm_config_t current;
         session_get_llm_config(conn->session, &current);

         const char *type_str = current.type == LLM_LOCAL ? "local" : "cloud";
         json_object_object_add(resp_payload, "type", json_object_new_string(type_str));
         json_object_object_add(resp_payload, "provider",
                                json_object_new_string(
                                    cloud_provider_to_string(current.cloud_provider)));

         /* Get model name - prefer session model, fall back to config */
         const char *model_name = NULL;
         if (current.model[0] != '\0') {
            /* Session has explicit model set */
            model_name = current.model;
         } else {
            /* Fall back to config default based on type/provider */
            const dawn_config_t *cfg = config_get();
            if (current.type == LLM_LOCAL) {
               model_name = cfg->llm.local.model[0] ? cfg->llm.local.model : "";
            } else if (current.cloud_provider == CLOUD_PROVIDER_OPENAI) {
               model_name = llm_get_default_openai_model();
            } else if (current.cloud_provider == CLOUD_PROVIDER_CLAUDE) {
               model_name = llm_get_default_claude_model();
            } else if (current.cloud_provider == CLOUD_PROVIDER_GEMINI) {
               model_name = llm_get_default_gemini_model();
            } else if (current.cloud_provider == CLOUD_PROVIDER_OPENROUTER) {
               model_name = llm_get_default_openrouter_model();
            }
         }
         json_object_object_add(resp_payload, "model",
                                json_object_new_string(model_name ? model_name : ""));
      }

      /* Include API key availability */
      json_object_object_add(resp_payload, "openai_available",
                             json_object_new_boolean(llm_has_openai_key()));
      json_object_object_add(resp_payload, "claude_available",
                             json_object_new_boolean(llm_has_claude_key()));
      json_object_object_add(resp_payload, "gemini_available",
                             json_object_new_boolean(llm_has_gemini_key()));
      json_object_object_add(resp_payload, "openrouter_available",
                             json_object_new_boolean(llm_has_openrouter_key()));

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);

      /* Send context info via queue (after response) so gauge reflects new model's
       * limit. Uses queued send to avoid multiple lws_write calls per callback.
       * Actual session token count preserves saved context on conversation load. */
      if (success && conn->session) {
         session_llm_config_t cfg;
         session_get_llm_config(conn->session, &cfg);
         int ctx_max = llm_context_get_size(cfg.type, cfg.cloud_provider, cfg.model);
         if (ctx_max > 0) {
            int ctx_current = 0;
            llm_context_usage_t ctx_usage;
            if (llm_context_get_usage(conn->session->session_id, cfg.type, cfg.cloud_provider,
                                      cfg.model, &ctx_usage) == 0) {
               ctx_current = ctx_usage.current_tokens;
            }
            webui_send_context(conn->session, ctx_current, ctx_max,
                               g_config.llm.compact_hard_threshold);
         }
      }
   } else if (strcmp(type, "reconnect") == 0) {
      /* Session reconnection with stored token */
      if (payload) {
         struct json_object *token_obj;
         if (json_object_object_get_ex(payload, "token", &token_obj)) {
            const char *token = json_object_get_string(token_obj);
            if (token && strlen(token) > 0) {
               session_t *existing = lookup_session_by_token(token);
               if (existing) {
                  /* Found existing session - switch to it */
                  if (conn->session && conn->session != existing) {
                     /* Destroy the abandoned session (just created on connect) */
                     uint32_t abandoned_id = conn->session->session_id;
                     conn->session->client_data = NULL;
                     /* Release ref_count (was 1 from creation) before destroy */
                     session_release(conn->session);
                     session_destroy(abandoned_id);
                     unregister_tokens_for_session(abandoned_id);
                     OLOG_INFO("WebUI: Destroyed abandoned session %u", abandoned_id);
                  }
                  conn->session = existing;
                  existing->client_data = conn;
                  existing->disconnected = false;
                  strncpy(conn->session_token, token, WEBUI_SESSION_TOKEN_LEN - 1);
                  conn->session_token[WEBUI_SESSION_TOKEN_LEN - 1] = '\0';

                  /* Restore connection capabilities from init payload */
                  conn->use_opus = check_opus_capability(payload);
                  struct json_object *tts_obj;
                  if (json_object_object_get_ex(payload, "tts_enabled", &tts_obj)) {
                     conn->tts_enabled = json_object_get_boolean(tts_obj);
                  }

                  OLOG_INFO("WebUI: Reconnected to session %u with token %.4s... "
                            "(opus: %s, tts: %s)",
                            existing->session_id, token, conn->use_opus ? "yes" : "no",
                            conn->tts_enabled ? "yes" : "no");

                  /* Queue init messages (one lws_write per callback) */
                  queue_init_messages(conn, token);

                  /* Re-deliver missed notifications to the reconnected session.
                   * On page refresh, LWS_CALLBACK_RECEIVE's early hook queued
                   * them to the abandoned session (now destroyed), so the
                   * client never saw them. Fire delivery against the real
                   * session and mark the connection as delivered. */
                  conn->missed_notif_delivered = false;
                  if (conn->authenticated && conn->auth_user_id > 0) {
                     deliver_missed_notifications(conn);
                     conn->missed_notif_delivered = true;
                  }
               } else {
                  /* Token not found or session expired - create new session */
                  OLOG_INFO("WebUI: Token %.4s... not found, creating new session", token);
                  if (!conn->session) {
                     conn->session = session_create(SESSION_TYPE_WEBUI, -1);
                     if (conn->session) {
                        /* Set user_id for metrics and memory extraction */
                        session_set_metrics_user(conn->session, conn->auth_user_id);
                        /* Build personalized prompt with user settings + memory context */
                        char *prompt = session_manager_build_system_prompt_string(
                            conn->auth_user_id);
                        session_init_system_prompt(conn->session,
                                                   prompt ? prompt : get_remote_command_prompt());
                        free(prompt);
                        conn->session->client_data = conn;
                        if (generate_session_token(conn->session_token) != 0) {
                           OLOG_ERROR("WebUI: Failed to generate session token");
                           session_destroy(conn->session->session_id);
                           conn->session = NULL;
                           return;
                        }
                        register_token(conn->session_token, conn->session->session_id);
                        queue_init_messages(conn, conn->session_token);
                     }
                  }

                  /* Sync capabilities from reconnect payload regardless of whether
                   * session was just created or already existed (e.g. auto-created
                   * from cookie auth before this message was processed). */
                  if (conn->session && payload) {
                     conn->use_opus = check_opus_capability(payload);
                     struct json_object *tts_obj;
                     if (json_object_object_get_ex(payload, "tts_enabled", &tts_obj)) {
                        conn->tts_enabled = json_object_get_boolean(tts_obj);
                     }
                     OLOG_INFO("WebUI: Session %u capabilities synced (opus: %s, tts: %s)",
                               conn->session->session_id, conn->use_opus ? "yes" : "no",
                               conn->tts_enabled ? "yes" : "no");
                  }
               }
            }
         }
      }
   } else if (strcmp(type, "init") == 0) {
      /* Init message arrived on an already-authenticated connection (cookie auth
       * auto-created the session before this message was processed). Sync capabilities. */
      if (payload) {
         conn->use_opus = check_opus_capability(payload);
         struct json_object *tts_obj;
         if (json_object_object_get_ex(payload, "tts_enabled", &tts_obj)) {
            conn->tts_enabled = json_object_get_boolean(tts_obj);
         }
         OLOG_INFO("WebUI: Session %u init capabilities synced (opus: %s, tts: %s)",
                   conn->session ? conn->session->session_id : 0, conn->use_opus ? "yes" : "no",
                   conn->tts_enabled ? "yes" : "no");
      }
   } else if (strcmp(type, "capabilities_update") == 0) {
      /* Client capability update (e.g., Opus codec became available after connect) */
      if (payload && conn->session) {
         conn->use_opus = check_opus_capability(payload);
         OLOG_INFO("WebUI: Session %u capabilities updated (opus: %s)", conn->session->session_id,
                   conn->use_opus ? "yes" : "no");
      } else if (payload) {
         /* Session not yet created - just store capability, session will read it later */
         conn->use_opus = check_opus_capability(payload);
         OLOG_INFO("WebUI: Connection capabilities updated before session (opus: %s)",
                   conn->use_opus ? "yes" : "no");
      }
   } else if (handle_smart_home_message(conn, type, payload)) {
      /* Handled by smart home dispatch (Home Assistant) */
   } else if (strcmp(type, "get_tools_config") == 0) {
      handle_get_tools_config(conn);
   } else if (strcmp(type, "set_tools_config") == 0) {
      if (payload) {
         handle_set_tools_config(conn, payload);
      }
   } else if (strcmp(type, "get_metrics") == 0) {
      handle_get_metrics(conn);
   }
   /* User management (admin only) */
   else if (strcmp(type, "list_users") == 0) {
      handle_list_users(conn);
   } else if (strcmp(type, "create_user") == 0) {
      if (payload) {
         handle_create_user(conn, payload);
      }
   } else if (strcmp(type, "delete_user") == 0) {
      if (payload) {
         handle_delete_user(conn, payload);
      }
   } else if (strcmp(type, "change_password") == 0) {
      if (payload) {
         handle_change_password(conn, payload);
      }
   } else if (strcmp(type, "unlock_user") == 0) {
      if (payload) {
         handle_unlock_user(conn, payload);
      }
   }
   /* Satellite management (admin only) */
   else if (strcmp(type, "list_satellites") == 0) {
      handle_list_satellites(conn);
   } else if (strcmp(type, "update_satellite") == 0) {
      if (payload) {
         handle_update_satellite(conn, payload);
      }
   } else if (strcmp(type, "delete_satellite") == 0) {
      if (payload) {
         handle_delete_satellite(conn, payload);
      }
   } else if (strcmp(type, "get_satellite_registration_key") == 0) {
      handle_get_satellite_registration_key(conn);
   } else if (strcmp(type, "ota_list") == 0) {
      handle_ota_list(conn);
   } else if (strcmp(type, "ota_push") == 0) {
      handle_ota_push(conn, payload);
   } else if (strcmp(type, "ota_push_all") == 0) {
      handle_ota_push_all(conn, payload);
   } else if (strcmp(type, "ota_rollout_status") == 0) {
      handle_ota_rollout_status(conn);
   } else if (strcmp(type, "ota_rollout_abort") == 0) {
      handle_ota_rollout_abort(conn);
   }
   /* Messaging channel management (user-scoped) */
   else if (strcmp(type, "list_channels") == 0) {
      handle_list_channels(conn);
   } else if (strcmp(type, "create_link_code") == 0) {
      handle_create_link_code(conn, payload);
   } else if (strcmp(type, "unlink_channel") == 0) {
      handle_unlink_channel(conn, payload);
   } else if (strcmp(type, "rename_channel") == 0) {
      handle_rename_channel(conn, payload);
   } else if (strcmp(type, "reenable_channel") == 0) {
      handle_reenable_channel(conn, payload);
   } else if (strcmp(type, "set_channel_llm") == 0) {
      handle_set_channel_llm(conn, payload);
   }
   /* Personal settings (authenticated users) */
   else if (strcmp(type, "get_my_settings") == 0) {
      handle_get_my_settings(conn);
   } else if (strcmp(type, "set_my_settings") == 0) {
      if (payload) {
         handle_set_my_settings(conn, payload);
      }
   }
   /* Dev-only: fires a fake silent_observation event so the WebUI can be
    * exercised before Phase 1/2 producers exist.  Gated by the
    * [debug] silent_observe_test_endpoint config flag (off by default) AND
    * admin-only — non-admin users never reach the broadcast even when the
    * flag is on. */
   else if (strcmp(type, "test_silent_observation") == 0) {
      if (g_config.debug.silent_observe_test_endpoint && conn_require_admin(conn) && payload) {
         json_object *jcat = NULL, *jnote = NULL, *jfilter = NULL;
         const char *cat = "system";
         const char *note = "test event";
         bool filter = false;
         if (json_object_object_get_ex(payload, "category", &jcat))
            cat = json_object_get_string(jcat);
         if (json_object_object_get_ex(payload, "note", &jnote))
            note = json_object_get_string(jnote);
         if (json_object_object_get_ex(payload, "filter_match", &jfilter))
            filter = json_object_get_boolean(jfilter);
         OLOG_INFO(
             "WebUI: test_silent_observation dispatched (user=%d, category=%s, filter_match=%d)",
             conn->auth_user_id, cat, filter ? 1 : 0);
         webui_broadcast_silent_observation(cat, note, conn->auth_user_id, filter);
      } else if (!g_config.debug.silent_observe_test_endpoint) {
         OLOG_WARNING("WebUI: test_silent_observation rejected — "
                      "[debug] silent_observe_test_endpoint is disabled");
      }
   }
   /* Session management (authenticated users) */
   else if (strcmp(type, "list_my_sessions") == 0) {
      handle_list_my_sessions(conn);
   } else if (strcmp(type, "revoke_session") == 0) {
      if (payload) {
         handle_revoke_session(conn, payload);
      }
   }
   /* Conversation history (authenticated users) */
   else if (strcmp(type, "list_conversations") == 0) {
      handle_list_conversations(conn, payload);
   } else if (strcmp(type, "new_conversation") == 0) {
      handle_new_conversation(conn, payload);
   } else if (strcmp(type, "load_conversation") == 0) {
      if (payload) {
         handle_load_conversation(conn, payload);
      }
   } else if (strcmp(type, "delete_conversation") == 0) {
      if (payload) {
         handle_delete_conversation(conn, payload);
      }
   } else if (strcmp(type, "rename_conversation") == 0) {
      if (payload) {
         handle_rename_conversation(conn, payload);
      }
   } else if (strcmp(type, "set_private") == 0) {
      if (payload) {
         handle_set_private(conn, payload);
      }
   } else if (strcmp(type, "reassign_conversation") == 0) {
      if (payload) {
         handle_reassign_conversation(conn, payload);
      }
   } else if (strcmp(type, "export_conversation") == 0) {
      if (payload) {
         handle_export_conversation(conn, payload);
      }
   } else if (strcmp(type, "search_conversations") == 0) {
      if (payload) {
         handle_search_conversations(conn, payload);
      }
   } else if (strcmp(type, "save_message") == 0) {
      if (payload) {
         handle_save_message(conn, payload);
      }
   } else if (strcmp(type, "update_context") == 0) {
      if (payload) {
         handle_update_context(conn, payload);
      }
   } else if (strcmp(type, "lock_conversation_llm") == 0) {
      if (payload) {
         handle_lock_conversation_llm(conn, payload);
      }
   } else if (strcmp(type, "continue_conversation") == 0) {
      if (payload) {
         handle_continue_conversation(conn, payload);
      }
   } else if (strcmp(type, "clear_session") == 0) {
      handle_clear_session(conn);
   }
   /* Memory management (authenticated users) */
   else if (strcmp(type, "get_memory_stats") == 0) {
      handle_get_memory_stats(conn);
   } else if (strcmp(type, "list_memory_facts") == 0) {
      handle_list_memory_facts(conn, payload);
   } else if (strcmp(type, "list_memory_preferences") == 0) {
      handle_list_memory_preferences(conn, payload);
   } else if (strcmp(type, "list_memory_summaries") == 0) {
      handle_list_memory_summaries(conn, payload);
   } else if (strcmp(type, "search_memory") == 0) {
      if (payload) {
         handle_search_memory(conn, payload);
      }
   } else if (strcmp(type, "delete_memory_fact") == 0) {
      if (payload) {
         handle_delete_memory_fact(conn, payload);
      }
   } else if (strcmp(type, "delete_memory_preference") == 0) {
      if (payload) {
         handle_delete_memory_preference(conn, payload);
      }
   } else if (strcmp(type, "delete_memory_summary") == 0) {
      if (payload) {
         handle_delete_memory_summary(conn, payload);
      }
   } else if (strcmp(type, "list_memory_entities") == 0) {
      handle_list_memory_entities(conn, payload);
   } else if (strcmp(type, "delete_memory_entity") == 0) {
      if (payload) {
         handle_delete_memory_entity(conn, payload);
      }
   } else if (strcmp(type, "merge_memory_entities") == 0) {
      if (payload) {
         handle_merge_memory_entities(conn, payload);
      }
   } else if (strcmp(type, "entity_aliases_request") == 0) {
      handle_entity_aliases_request(conn, payload);
   } else if (strcmp(type, "entity_merge_proposal_list_request") == 0) {
      handle_entity_merge_proposal_list_request(conn, payload);
   } else if (strcmp(type, "entity_link_request") == 0) {
      if (payload) {
         handle_entity_link_request(conn, payload);
      }
   } else if (strcmp(type, "entity_unlink_request") == 0) {
      if (payload) {
         handle_entity_unlink_request(conn, payload);
      }
   } else if (strcmp(type, "entity_proposal_resolve_request") == 0) {
      if (payload) {
         handle_entity_proposal_resolve_request(conn, payload);
      }
   } else if (strcmp(type, "delete_all_memories") == 0) {
      if (payload) {
         handle_delete_all_memories(conn, payload);
      }
   } else if (strcmp(type, "export_memories") == 0) {
      handle_export_memories(conn, payload);
   } else if (strcmp(type, "import_memories") == 0) {
      if (payload) {
         handle_import_memories(conn, payload);
      }
   } else if (strcmp(type, "get_memory_fact_source") == 0) {
      if (payload) {
         handle_get_memory_fact_source(conn, payload);
      }
   }
   /* Contacts management */
   else if (strcmp(type, "contacts_list") == 0) {
      handle_contacts_list(conn, payload);
   } else if (strcmp(type, "contacts_search") == 0) {
      if (payload) {
         handle_contacts_search(conn, payload);
      }
   } else if (strcmp(type, "contacts_add") == 0) {
      if (payload) {
         handle_contacts_add(conn, payload);
      }
   } else if (strcmp(type, "contacts_update") == 0) {
      if (payload) {
         handle_contacts_update(conn, payload);
      }
   } else if (strcmp(type, "contacts_delete") == 0) {
      if (payload) {
         handle_contacts_delete(conn, payload);
      }
   } else if (strcmp(type, "contacts_search_entities") == 0) {
      if (payload) {
         handle_contacts_search_entities(conn, payload);
      }
   } else if (strcmp(type, "entity_set_photo") == 0) {
      if (payload) {
         handle_entity_set_photo(conn, payload);
      }
   } else if (strcmp(type, "entity_ensure") == 0) {
      if (payload) {
         handle_entity_ensure(conn, payload);
      }
   }
   /* Document library (RAG) */
   else if (strcmp(type, "doc_library_list") == 0) {
      handle_doc_library_list(conn, payload);
   } else if (strcmp(type, "doc_library_delete") == 0) {
      if (payload) {
         handle_doc_library_delete(conn, payload);
      }
   } else if (strcmp(type, "doc_library_index") == 0) {
      if (payload) {
         handle_doc_library_index(conn, payload);
      }
   } else if (strcmp(type, "doc_library_toggle_global") == 0) {
      if (payload) {
         handle_doc_library_toggle_global(conn, payload);
      }
   } else if (strcmp(type, "doc_library_note_save") == 0) {
      if (payload) {
         handle_doc_library_note_save(conn, payload);
      }
   } else if (strcmp(type, "doc_library_note_update") == 0) {
      if (payload) {
         handle_doc_library_note_update(conn, payload);
      }
   } else if (strcmp(type, "doc_library_version_list") == 0) {
      if (payload) {
         handle_doc_library_version_list(conn, payload);
      }
   } else if (strcmp(type, "doc_library_deleted_list") == 0) {
      handle_doc_library_deleted_list(conn, payload);
   } else if (strcmp(type, "doc_library_version_restore") == 0) {
      if (payload) {
         handle_doc_library_version_restore(conn, payload);
      }
   }
#ifdef DAWN_ENABLE_CODE_PROJECTS
   /* Code projects (coding harness) */
   else if (strcmp(type, "code_projects_list") == 0) {
      handle_code_projects_list(conn, payload);
   } else if (strcmp(type, "code_projects_import") == 0) {
      handle_code_projects_import(conn, payload);
   } else if (strcmp(type, "code_projects_refresh") == 0) {
      handle_code_projects_refresh(conn, payload);
   } else if (strcmp(type, "code_projects_delete") == 0) {
      handle_code_projects_delete(conn, payload);
   }
#endif
   /* OAuth flow (shared by calendar and email) */
#if defined(DAWN_ENABLE_CALENDAR_TOOL) || defined(DAWN_ENABLE_EMAIL_TOOL)
   else if (strcmp(type, "oauth_get_auth_url") == 0) {
      if (payload)
         handle_oauth_get_auth_url(conn, payload);
   } else if (strcmp(type, "oauth_exchange_code") == 0) {
      if (payload)
         handle_oauth_exchange_code(conn, payload);
   } else if (strcmp(type, "oauth_disconnect") == 0) {
      if (payload)
         handle_oauth_disconnect(conn, payload);
   } else if (strcmp(type, "oauth_check_scopes") == 0) {
      if (payload)
         handle_oauth_check_scopes(conn, payload);
   }
#endif

   /* Calendar account management (per-user) */
#ifdef DAWN_ENABLE_CALENDAR_TOOL
   else if (strcmp(type, "calendar_list_accounts") == 0) {
      handle_calendar_list_accounts(conn);
   } else if (strcmp(type, "calendar_add_account") == 0) {
      if (payload) {
         handle_calendar_add_account(conn, payload);
      }
   } else if (strcmp(type, "calendar_edit_account") == 0) {
      if (payload) {
         handle_calendar_edit_account(conn, payload);
      }
   } else if (strcmp(type, "calendar_remove_account") == 0) {
      if (payload) {
         handle_calendar_remove_account(conn, payload);
      }
   } else if (strcmp(type, "calendar_test_account") == 0) {
      if (payload) {
         handle_calendar_test_account(conn, payload);
      }
   } else if (strcmp(type, "calendar_sync_account") == 0) {
      if (payload) {
         handle_calendar_sync_account(conn, payload);
      }
   } else if (strcmp(type, "calendar_list_calendars") == 0) {
      if (payload) {
         handle_calendar_list_calendars(conn, payload);
      }
   } else if (strcmp(type, "calendar_toggle_calendar") == 0) {
      if (payload) {
         handle_calendar_toggle_calendar(conn, payload);
      }
   } else if (strcmp(type, "calendar_toggle_read_only") == 0) {
      if (payload) {
         handle_calendar_toggle_read_only(conn, payload);
      }
   } else if (strcmp(type, "calendar_set_enabled") == 0) {
      if (payload) {
         handle_calendar_set_enabled(conn, payload);
      }
   }
#endif /* DAWN_ENABLE_CALENDAR_TOOL */
#ifdef DAWN_ENABLE_EMAIL_TOOL
   else if (strcmp(type, "email_list_accounts") == 0) {
      handle_email_list_accounts(conn);
   } else if (strcmp(type, "email_add_account") == 0) {
      if (payload) {
         handle_email_add_account(conn, payload);
      }
   } else if (strcmp(type, "email_update_account") == 0) {
      if (payload) {
         handle_email_update_account(conn, payload);
      }
   } else if (strcmp(type, "email_remove_account") == 0) {
      if (payload) {
         handle_email_remove_account(conn, payload);
      }
   } else if (strcmp(type, "email_test_connection") == 0) {
      if (payload) {
         handle_email_test_connection(conn, payload);
      }
   } else if (strcmp(type, "email_set_read_only") == 0) {
      if (payload) {
         handle_email_set_read_only(conn, payload);
      }
   } else if (strcmp(type, "email_set_enabled") == 0) {
      if (payload) {
         handle_email_set_enabled(conn, payload);
      }
   }
#endif /* DAWN_ENABLE_EMAIL_TOOL */
   /* TTS control (per-connection) */
   else if (strcmp(type, "set_tts_enabled") == 0) {
      if (!conn_require_auth(conn)) {
         return;
      }
      if (payload) {
         struct json_object *enabled_obj;
         if (json_object_object_get_ex(payload, "enabled", &enabled_obj)) {
            conn->tts_enabled = json_object_get_boolean(enabled_obj);
            OLOG_INFO("WebUI: TTS %s for session %u", conn->tts_enabled ? "enabled" : "disabled",
                      conn->session ? conn->session->session_id : 0);
         }
      }
   }
   /* Music streaming — accessible to authenticated users AND registered satellites.
    * Check satellite first: conn_require_auth() has a side-effect of sending
    * an UNAUTHORIZED error, so we must short-circuit before it fires. */
   else if (strcmp(type, "music_subscribe") == 0) {
      if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
         return;
      }
      handle_music_subscribe(conn, payload);
   } else if (strcmp(type, "music_unsubscribe") == 0) {
      if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
         return;
      }
      handle_music_unsubscribe(conn);
   } else if (strcmp(type, "music_control") == 0) {
      if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
         return;
      }
      if (payload) {
         handle_music_control(conn, payload);
      }
   } else if (strcmp(type, "music_search") == 0) {
      if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
         return;
      }
      handle_music_search(conn, payload);
   } else if (strcmp(type, "music_library") == 0) {
      if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
         return;
      }
      handle_music_library(conn, payload);
   } else if (strcmp(type, "music_queue") == 0) {
      if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
         return;
      }
      if (payload) {
         handle_music_queue(conn, payload);
      }
   }
   /* Scheduler dismiss/snooze from WebUI or satellite client */
   else if (strcmp(type, "scheduler_action") == 0) {
      if (!conn_is_satellite_session(conn) && !conn_require_auth(conn))
         return;
      if (!payload)
         return;

      json_object *action_obj, *event_id_obj, *snooze_obj;
      json_object_object_get_ex(payload, "action", &action_obj);
      json_object_object_get_ex(payload, "event_id", &event_id_obj);
      const char *action = action_obj ? json_object_get_string(action_obj) : NULL;
      int64_t event_id = event_id_obj ? json_object_get_int64(event_id_obj) : 0;

      if (!action || !action[0]) {
         send_error_impl(conn->wsi, "INVALID_PARAM", "Missing action");
      } else if (strcmp(action, "list") == 0) {
         handle_scheduler_list_events(conn);
      } else if (strcmp(action, "cancel") == 0) {
         handle_scheduler_cancel_event(conn, event_id);
      } else if (strcmp(action, "cancel_occurrence") == 0) {
         handle_scheduler_cancel_occurrence(conn, event_id);
      } else if (strcmp(action, "clear_missed") == 0) {
         handle_scheduler_clear_missed(conn, event_id);
      } else if (strcmp(action, "dismiss_missed") == 0) {
         /* Delete a queued missed notification. Uses missed_notif_id rather than
          * event_id, so this branch runs before the event_id validation.
          * The DB delete enforces user ownership via AND user_id = ?. */
         json_object *mid_obj = NULL;
         json_object_object_get_ex(payload, "missed_notif_id", &mid_obj);
         int64_t mid = mid_obj ? json_object_get_int64(mid_obj) : 0;
         if (mid <= 0) {
            send_error_impl(conn->wsi, "INVALID_PARAM", "Missing missed_notif_id");
         } else if (conn->auth_user_id <= 0) {
            send_error_impl(conn->wsi, "FORBIDDEN", "Not authenticated");
         } else {
            missed_notif_delete_by_user(mid, conn->auth_user_id);
            /* If the underlying event is still ringing (e.g. alarm looping on
             * the local speaker while user was offline), dismiss it too.
             * event_id comes from the client but ownership is verified before
             * dismissing so it's not an injection vector. */
            if (event_id > 0) {
               sched_event_t ev;
               if (scheduler_db_get(event_id, &ev) == 0 && ev.user_id == conn->auth_user_id &&
                   ev.status == SCHED_STATUS_RINGING) {
                  scheduler_dismiss(event_id);
               }
            }
         }
      } else if (event_id <= 0) {
         send_error_impl(conn->wsi, "INVALID_PARAM", "Missing or invalid event_id");
      } else {
         sched_event_t ev;
         int get_rc = scheduler_db_get(event_id, &ev);
         if (get_rc != 0) {
            send_error_impl(conn->wsi, "NOT_FOUND", "Event not found");
         } else if (!conn->is_satellite && ev.user_id != conn->auth_user_id) {
            /* Satellites can dismiss any announced event; WebUI users can only
             * dismiss their own events. */
            send_error_impl(conn->wsi, "FORBIDDEN", "Not your event");
         } else if (strcmp(action, "dismiss") == 0) {
            int rc = scheduler_dismiss(event_id);
            if (rc != 0) {
               /* Already dismissed (e.g. auto-dismiss for timers) — rebroadcast
                * so other clients (satellites) can sync their UI. */
               scheduler_broadcast_notification(&ev, "Dismissed");
               scheduler_broadcast_events_changed(ev.user_id);
            }
         } else if (strcmp(action, "snooze") == 0) {
            json_object_object_get_ex(payload, "snooze_minutes", &snooze_obj);
            int snooze_min = snooze_obj ? json_object_get_int(snooze_obj) : 0;
            int rc = scheduler_snooze(event_id, snooze_min);
            if (rc != 0)
               send_error_impl(conn->wsi, "NOT_FOUND", "No ringing event to snooze");
            else
               scheduler_broadcast_events_changed(ev.user_id);
         } else {
            send_error_impl(conn->wsi, "INVALID_PARAM", "Unknown scheduler action");
         }
      }
   }
   /* Always-on voice mode */
   else if (strcmp(type, "always_on_enable") == 0) {
      if (!conn_require_auth(conn)) {
         return;
      }
      handle_always_on_enable(conn, payload);
   } else if (strcmp(type, "always_on_disable") == 0) {
      if (!conn_require_auth(conn)) {
         return;
      }
      handle_always_on_disable(conn);
   } else if (strcmp(type, "always_on_state_request") == 0) {
      if (!conn_require_auth(conn)) {
         return;
      }
      /* Client re-sync (e.g., tab returned from background) */
      const char *state_name = conn->always_on
                                   ? always_on_state_name(always_on_get_state(conn->always_on))
                                   : "disabled";
      send_always_on_state(conn->wsi, state_name);
   }
   /* Satellite (DAP2 Tier 1) messages — only accept from existing satellites.
    * Initial registration is handled in the init block above (line ~2924).
    * Re-registration is rejected to prevent identity spoofing. */
   else if (strcmp(type, "satellite_register") == 0) {
      if (conn->is_satellite && conn->session) {
         send_error_impl(conn->wsi, "ALREADY_REGISTERED",
                         "Satellite already registered on this connection");
      } else if (conn->is_satellite && payload) {
         handle_satellite_register(conn, payload);
      }
   } else if (strcmp(type, "satellite_query") == 0) {
      if (conn->is_satellite && payload) {
         handle_satellite_query(conn, payload);
      }
   } else if (strcmp(type, "satellite_ping") == 0) {
      if (conn->is_satellite) {
         handle_satellite_ping(conn);
      }
   } else if (strcmp(type, "volume_state") == 0) {
      if (conn->is_satellite && payload) {
         handle_satellite_volume_state(conn, payload);
      }
   } else if (strcmp(type, "ota_status") == 0) {
      if (conn->is_satellite && payload) {
         handle_ota_status(conn, payload);
      }
   } else if (strcmp(type, "ota_ack") == 0) {
      if (conn->is_satellite) {
         handle_ota_ack(conn, payload);
      }
   } else if (strcmp(type, "ota_reject") == 0) {
      if (conn->is_satellite) {
         handle_ota_reject(conn, payload);
      }
   } else if (strncmp(type, "ha_", sizeof("ha_") - 1) == 0) {
      OLOG_DEBUG("WebUI: Ignoring %s message (feature not compiled in)", type);
   } else {
      OLOG_WARNING("WebUI: Unknown message type: %s", type);
   }

   json_object_put(root);
   free(json_str);
}

static void handle_cancel_message(ws_connection_t *conn) {
   if (conn->session) {
      OLOG_INFO("WebUI: Cancel requested for session %u", conn->session->session_id);
      conn->session->disconnected = true; /* Signal worker to abort */
      /* Note: llm_request_interrupt() is NOT called here because it uses a global flag
       * that would affect all users. Instead, session_manager.c sets the TLS cancel flag
       * to &session->disconnected before each LLM call, so the CURL progress callback
       * will check THIS session's disconnected flag only. */
      send_state_impl(conn->wsi, "idle", NULL);
   }
}
