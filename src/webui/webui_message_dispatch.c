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
#include "core/image_rehydrate.h"
#include "core/missed_notifications_db.h"
#include "core/scheduler.h"
#include "core/scheduler_db.h"
#include "core/session_manager.h"
#include "core/session_prefix.h"
#include "dawn.h"
#include "document_original_store.h"
#include "image_store.h"
#include "llm/llm_claude_format.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_context.h"
#include "llm/llm_local_provider.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "utils/string_utils.h"
#include "webui/webui_always_on.h"
#include "webui/webui_attachments.h"
#include "webui/webui_attention.h"
#include "webui/webui_contacts.h"
#include "webui/webui_doc_library.h"
#ifdef DAWN_ENABLE_CODE_PROJECTS
#include "webui/webui_code_projects.h"
#endif
#ifdef DAWN_ENABLE_SCHWAB_TOOL
#include "webui/webui_stocks.h"
#endif
#include "webui/webui_email.h"
#include "webui/webui_email_panel.h"
#include "webui/webui_internal.h"
#include "webui/webui_oauth.h"
#include "webui/webui_ota.h"
#include "webui/webui_protocol.h"
#include "webui/webui_reasoning.h"
#include "webui/webui_server.h"

/* handle_cancel_message and handle_ping are defined at the bottom of this TU. */
static void handle_cancel_message(ws_connection_t *conn);
static void handle_ping(ws_connection_t *conn, struct json_object *payload);
static void dispatch_message(ws_connection_t *conn, const char *type, struct json_object *payload);

/* handle_always_on_enable / handle_always_on_disable moved to
 * webui_always_on.c (next to always_on_create / always_on_destroy);
 * declarations in webui_always_on.h. */

/* An attachment's blob_id is an original the turn's user may read (or one
 * that's gone, which the document goes without). */
static int attachment_owner(const char *blob_id, int user_id) {
   char name[WEBUI_ATTACHMENT_FILENAME_MAX + 1];
   const int rc = document_original_get_filename(blob_id, user_id, name, sizeof(name));
   if (rc == BLOB_STORE_SUCCESS) {
      return SUCCESS;
   }
   return rc == BLOB_STORE_NOT_FOUND ? WEBUI_ATTACHMENT_BLOB_GONE : FAILURE;
}

/* The text a turn carries: its attached documents (built from the frame's
 * `attachments`, markers inside them quoted here and the bodies defused when
 * the turn runs), then its words.
 * Sets *built to the new text (caller frees), or leaves it NULL when the frame
 * has none.  False when the turn was refused (the error is sent). */
static bool turn_text_with_attachments(ws_connection_t *conn,
                                       struct json_object *payload,
                                       const char *words,
                                       char **built) {
   *built = NULL;
   struct json_object *arr = NULL;
   if (!json_object_object_get_ex(payload, "attachments", &arr)) {
      return true;
   }
   /* Ownership of a blob_id is the user's: refused here like any turn of an
    * unauthenticated connection. */
   if (!conn_require_auth(conn)) {
      return false;
   }
   const int max_docs = g_config.documents.max_documents;
   const size_t max_content = (size_t)g_config.documents.max_extracted_size_kb * 1024;
   char *docs = NULL;
   const int rc = webui_attachments_build(arr, conn->auth_user_id, max_docs, max_content,
                                          attachment_owner, &docs);
   if (rc == WEBUI_ATTACHMENTS_INVALID) {
      send_error_impl(conn->wsi, "ATTACHMENT_INVALID",
                      "An attached document is malformed, too large, too many, or not yours");
      return false;
   }
   if (rc != SUCCESS || !docs) {
      send_error_impl(conn->wsi, "PROCESSING_ERROR", "The attached documents couldn't be read");
      return false;
   }
   /* Documents first, then the words, as clients inlined them. */
   if (words[strspn(words, " \t\r\n")] == '\0') {
      *built = docs;
      return true;
   }
   const size_t need = strlen(docs) + 2 + strlen(words) + 1;
   *built = malloc(need);
   if (!*built) {
      free(docs);
      send_error_impl(conn->wsi, "PROCESSING_ERROR", "The attached documents couldn't be read");
      return false;
   }
   snprintf(*built, need, "%s\n\n%s", docs, words);
   free(docs);
   return true;
}

/* A text turn's frame, @p payload its payload (non-NULL): validated, then
 * handed to handle_text_message.  Every refusal here is one error frame. */
static void text_turn_from_payload(ws_connection_t *conn, struct json_object *payload) {
   /* A turn is its words, its images, or both; absent text is no words. */
   struct json_object *text_obj = NULL;
   const char *text = NULL;
   if (json_object_object_get_ex(payload, "text", &text_obj)) {
      text = json_object_get_string(text_obj);
   }
   if (!text) {
      text = "";
   }

   /* Explicit target conversation (background-jobs delta routing): the
    * client sends the conversation this message belongs to, so the
    * server never has to infer it from the live view — robust across
    * reconnect and multi-tab, where server-side active_conversation_id
    * can be stale or 0.  conn_reanchor_conversation validates ownership
    * (a client must not tag/persist into another user's conversation)
    * then heals both the active id and its privacy flag together; a
    * bad/foreign id is a no-op heal (the turn keeps the prior anchor). */
   struct json_object *conv_id_obj;
   if (json_object_object_get_ex(payload, "conversation_id", &conv_id_obj)) {
      conn_reanchor_conversation(conn, json_object_get_int64(conv_id_obj));
   }

   /* The turn's images come only from image_ids[] (/api/images ids;
    * the worker reads the stored files).  Base64 images[] from an
    * older client are neither read nor validated: ignored, not
    * rejected, so its turn still arrives as text. */
   if (json_object_object_get_ex(payload, "images", NULL)) {
      static atomic_bool s_images_ignored_logged = false;
      if (!atomic_exchange(&s_images_ignored_logged, true)) {
         OLOG_DEBUG("WebUI: ignoring a text frame's base64 images[] (only "
                    "image_ids are read); logged once");
      }
   }

   char image_ids[WEBUI_MAX_VISION_IMAGES_CAP][IMAGE_ID_LEN];
   int image_id_count = 0;
   int max_images = g_config.vision.max_images;
   if (max_images > WEBUI_MAX_VISION_IMAGES_CAP) {
      max_images = WEBUI_MAX_VISION_IMAGES_CAP;
   }
   const int rc = image_turn_ids_parse(payload, max_images, image_ids, &image_id_count);
   if (rc != SUCCESS) {
      const char *code = NULL;
      const char *message = NULL;
      webui_image_error_describe(rc, &code, &message);
      send_error_impl(conn->wsi, code, message);
      return;
   }

   /* Attached documents sent as their own field are framed into the text
    * here (their bodies are defused when the turn runs). */
   char *built = NULL;
   if (!turn_text_with_attachments(conn, payload, text, &built)) {
      return;
   }
   if (built) {
      text = built;
   }

   /* Nothing to say and nothing to show is refused, never dropped
    * silently (the client is waiting for its echo).  Words that are
    * only whitespace count as none. */
   if (text[strspn(text, " \t\r\n")] == '\0') {
      if (image_id_count == 0) {
         send_error_impl(conn->wsi, "EMPTY_MESSAGE", "A message needs text or at least one image");
         free(built);
         return;
      }
      text = "";
   }

   /* Server-authoritative persisted form for an image turn: clean text
    * + [IMAGE:<id>] markers.  NULL for text-only turns (persist plain
    * text).  Freed after the call — the worker strdup's what it needs.
    * Retention promotion happens post-persist in the worker, so images
    * are pinned only for turns that actually persisted. */
   char *persist_content = NULL;
   if (image_id_count > 0) {
      persist_content = image_marker_build_content(text, (const char(*)[IMAGE_ID_LEN])image_ids,
                                                   image_id_count);
      if (!persist_content) {
         const char *code = NULL;
         const char *message = NULL;
         webui_image_error_describe(IMAGE_REHYDRATE_ERR_NOMEM, &code, &message);
         send_error_impl(conn->wsi, code, message);
         free(built);
         return;
      }
   }

   handle_text_message(conn, text, strlen(text), (const char(*)[IMAGE_ID_LEN])image_ids,
                       image_id_count, persist_content);
   free(persist_content);
   free(built);
}

/* A `text` frame.  Its client_ref, when it has one, is this thread's turn ref
 * while the frame is handled, so every error the handling raises (here, the
 * auth gate, a full queue) and the user echo name the turn. */
static void dispatch_text_frame(ws_connection_t *conn, struct json_object *payload) {
   if (!payload) {
      /* No payload: no words and no images, refused like any empty turn. */
      send_error_impl(conn->wsi, "EMPTY_MESSAGE", "A message needs text or at least one image");
      return;
   }
   struct json_object *ref_obj = NULL;
   if (json_object_object_get_ex(payload, "client_ref", &ref_obj)) {
      /* Anything but a string of the allowed shape is refused, null included;
       * a string with an embedded NUL too (it couldn't be echoed unchanged). */
      const bool is_string = json_object_is_type(ref_obj, json_type_string);
      const char *ref = is_string ? json_object_get_string(ref_obj) : NULL;
      if (!webui_client_ref_valid(ref) ||
          /* webui_client_ref_valid(NULL) is false, so strlen never sees NULL */
          // NOLINTNEXTLINE(clang-analyzer-core.NonNullParamChecker)
          (size_t)json_object_get_string_len(ref_obj) != strlen(ref)) {
         send_error_impl(conn->wsi, "INVALID_CLIENT_REF",
                         "client_ref must be 1 to 64 printable ASCII characters");
         return;
      }
      webui_turn_ref_set(ref);
   }
   /* A prompt a rendered visual sent through its bridge: the turn it starts
    * can't confirm anything (only a strict true counts; anything else is the
    * person's own message). */
   struct json_object *visual_obj = NULL;
   webui_turn_from_visual_set(json_object_object_get_ex(payload, "from_visual", &visual_obj) &&
                              json_object_is_type(visual_obj, json_type_boolean) &&
                              json_object_get_boolean(visual_obj));
   text_turn_from_payload(conn, payload);
   webui_turn_from_visual_set(false);
   webui_turn_ref_set(NULL);
}

void handle_json_message(ws_connection_t *conn, const char *data, size_t len) {
   /* Null-terminate for JSON parsing */
   char *json_str = strndup(data, len);
   if (!json_str) {
      OLOG_ERROR("WebUI: Failed to allocate JSON string");
      return;
   }

   struct json_object *root = json_tokener_parse(json_str);
   if (!root) {
      /* The length only: a frame can carry MBs of a user's documents. */
      OLOG_WARNING("WebUI: Invalid JSON received (%zu bytes)", len);
      free(json_str);
      return;
   }

   /* The type must be a string with no NUL inside: `"type": null` is found but
    * NULL, and an embedded NUL would dispatch on a prefix. */
   struct json_object *type_obj = NULL;
   if (!json_object_object_get_ex(root, "type", &type_obj) ||
       !json_object_is_type(type_obj, json_type_string) ||
       strlen(json_object_get_string(type_obj)) != (size_t)json_object_get_string_len(type_obj)) {
      OLOG_WARNING("WebUI: JSON missing a string 'type' field");
   } else {
      struct json_object *payload = NULL;
      json_object_object_get_ex(root, "payload", &payload);
      dispatch_message(conn, json_object_get_string(type_obj), payload);
   }
   json_object_put(root);
   free(json_str);
}

/* Handlers return early freely: handle_json_message owns the parsed frame. */
static void dispatch_message(ws_connection_t *conn, const char *type, struct json_object *payload) {
   if (strcmp(type, "text") == 0) {
      /* Text input from user, with the images attached to it by id */
      dispatch_text_frame(conn, payload);
   } else if (strcmp(type, "cancel") == 0) {
      /* Cancel current operation */
      handle_cancel_message(conn);
   } else if (strcmp(type, "get_config") == 0) {
      /* Request current configuration */
      handle_get_config(conn);
   } else if (strcmp(type, "get_system_prompt") == 0) {
      /* Auth-gate this info-disclosure endpoint like handle_get_config: a WS
       * session is created on `init` BEFORE authentication, and this response
       * exposes the full system prompt AND the native-tools schema. */
      if (!conn_require_auth(conn)) {
         return;
      }
      /* The system prompt for debugging: the conversation's frozen prompt
       * and each instruction change and standing direction since. */
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

         /* Native tools are a separate `tools` array in the API request, NOT part
          * of the system-prompt text — so surface them here, serialized exactly as
          * the LLM receives them (descriptions post-truncation), to let the
          * inspector validate what the model actually sees. */
         /* The conversation's frozen set when it has one, as every request of
          * it is sent; else what this surface would get. */
         bool is_remote = (conn->session->type != SESSION_TYPE_LOCAL);
         struct json_object *frozen = session_prefix_tool_defs(conn->session);
         struct json_object *tools = frozen ? llm_tools_render_frozen(frozen, false)
                                            : llm_tools_get_openai_format_filtered(is_remote);
         json_object_put(frozen);
         if (tools) {
            const char *tools_json = json_object_to_json_string_ext(tools, JSON_C_TO_STRING_PRETTY);
            json_object_object_add(resp_payload, "tools", json_object_new_string(tools_json));
            json_object_object_add(resp_payload, "tools_count",
                                   json_object_new_int((int)json_object_array_length(tools)));
            json_object_put(tools);
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
                  /* When switching to cloud, ensure a provider with a key is selected.
                   * OpenRouter is a normal provider now; it is picked last so it does
                   * not preempt a configured direct provider (decision O1). */
                  if (llm_has_openai_key()) {
                     llm_set_cloud_provider(CLOUD_PROVIDER_OPENAI);
                  } else if (llm_has_claude_key()) {
                     llm_set_cloud_provider(CLOUD_PROVIDER_CLAUDE);
                  } else if (llm_has_gemini_key()) {
                     llm_set_cloud_provider(CLOUD_PROVIDER_GEMINI);
                  } else if (llm_has_openrouter_key()) {
                     llm_set_cloud_provider(CLOUD_PROVIDER_OPENROUTER);
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
      bool reasoning_adjusted = false; /* the user's own reasoning change was adjusted */
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

         /* Track old model + type for context cache invalidation */
         char old_model[LLM_MODEL_NAME_MAX];
         safe_strscpy(old_model, config.model);
         int old_type = config.type;

         /* Parse type (local/cloud) */
         bool type_explicitly_set = false;
         if (json_object_object_get_ex(payload, "type", &type_obj)) {
            const char *new_type = json_object_get_string(type_obj);
            if (new_type) {
               has_changes = true;
               if (strcmp(new_type, "local") == 0) {
                  config.type = LLM_LOCAL;
                  type_explicitly_set = true;
               } else if (strcmp(new_type, "cloud") == 0) {
                  config.type = LLM_CLOUD;
                  type_explicitly_set = true;
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
                  /* Reset to defaults from dawn.toml, in the working copy:
                   * anything else this request sets applies over them, and the
                   * one set below commits all of it or none. */
                  llm_get_default_config(&config);
                  has_changes = true;
                  OLOG_INFO("WebUI: Session %u LLM config reset to defaults requested",
                            conn->session->session_id);
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
                  safe_strscpy(config.model, new_model);
                  /* "requested", not "set": under the OpenRouter gateway a bare
                   * (non vendor/model) id is dropped below and the resolver
                   * substitutes the gateway default — so this value is not
                   * necessarily what runs.  Logging it as "set" previously
                   * implied a model switch that the gateway then discarded. */
                  OLOG_INFO("WebUI: Session model requested '%s'", config.model);

                  /* Infer provider from model name if not explicitly set
                   * (handles old conversations and frontend bugs).
                   * Only infer if the inferred provider has an API key available.
                   * OpenRouter IDs are "vendor/model" slugs and match none of the bare
                   * prefixes below, so an OpenRouter session's model is left untouched. */
                  if (!provider_explicitly_set) {
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

         /* Reconcile type with the selected cloud model/provider (frontend-bug guard).
          * `type` and `model`/`provider` are independent fields; a client that switches to a
          * cloud model/provider but OMITS type='cloud' leaves the session's prior LOCAL type
          * standing, and `type` wins at dispatch — so the turn runs on the local endpoint
          * despite the cloud model name (observed: an Aurora gpt-5.6-luna pick kept a stale
          * local type, and the follow-up ran on llama.cpp).  The existing provider inference
          * above only corrects the PROVIDER and is gated on !provider_explicitly_set, so it
          * misses this.  Force type=cloud on either cloud signal:
          *   (a) the effective model name matches a bare cloud prefix — local models are gguf
          *       paths that match none, so this never misfires on a local pick; or
          *   (b) the client explicitly set a cloud provider this request (covers OpenRouter,
          *       whose "vendor/model" slug matches no prefix) — an active cloud selection.
          * A missing key then fails loudly in session_set_llm_config below rather than
          * silently substituting local.
          *
          * Gated on !type_explicitly_set: this only reconciles the case where the client
          * OMITS type. A client that explicitly sends type='local' this request means it —
          * the carried-over cloud model name is just stale (the client updates the model in
          * a follow-up set_session_llm once the local list arrives). Firing here on an
          * explicit local pick reverts it straight back to cloud, which the WebUI's async
          * flow (send {type:local}, THEN fetch models) can never recover from. */
         bool model_is_cloud = strncmp(config.model, "gpt-", 4) == 0 ||
                               strncmp(config.model, "o1-", 3) == 0 ||
                               strncmp(config.model, "o3-", 3) == 0 ||
                               strncmp(config.model, "claude-", 7) == 0 ||
                               strncmp(config.model, "gemini-", 7) == 0;
         bool provider_is_cloud = provider_explicitly_set &&
                                  config.cloud_provider != CLOUD_PROVIDER_NONE;
         if (!type_explicitly_set && config.type != LLM_CLOUD &&
             (model_is_cloud || provider_is_cloud)) {
            config.type = LLM_CLOUD;
            has_changes = true;
            OLOG_INFO("WebUI: Corrected stale local type to cloud for model '%s'", config.model);
         }

         /* Parse thinking_mode.  Stored as picked; each request resolves it
          * against what the model accepts (llm_thinking_resolve_current). */
         struct json_object *thinking_mode_obj;
         bool sent_mode = false;   /* an accepted thinking_mode came in */
         bool sent_effort = false; /* an accepted reasoning_effort came in */
         if (json_object_object_get_ex(payload, "thinking_mode", &thinking_mode_obj)) {
            const char *new_thinking_mode = json_object_get_string(thinking_mode_obj);
            if (new_thinking_mode) {
               /* Validate thinking mode value */
               if (strcmp(new_thinking_mode, "disabled") == 0 ||
                   strcmp(new_thinking_mode, "adaptive") == 0 ||
                   strcmp(new_thinking_mode, "enabled") == 0 ||
                   strcmp(new_thinking_mode, "auto") == 0) {
                  has_changes = true;
                  sent_mode = true;
                  safe_strscpy(config.thinking_mode, new_thinking_mode);
                  OLOG_INFO("WebUI: Session thinking_mode set to '%s'", config.thinking_mode);
               } else {
                  OLOG_WARNING("WebUI: Rejected invalid thinking_mode '%s' from client",
                               new_thinking_mode);
               }
            }
         }

         /* Parse reasoning_effort: any effort name some model takes; each
          * request snaps it to the nearest one its model offers. */
         struct json_object *reasoning_effort_obj;
         if (json_object_object_get_ex(payload, "reasoning_effort", &reasoning_effort_obj)) {
            const char *new_effort = json_object_get_string(reasoning_effort_obj);
            if (new_effort) {
               if (strcmp(new_effort, "none") == 0 || strcmp(new_effort, "minimal") == 0 ||
                   strcmp(new_effort, "low") == 0 || strcmp(new_effort, "medium") == 0 ||
                   strcmp(new_effort, "high") == 0 || strcmp(new_effort, "xhigh") == 0 ||
                   strcmp(new_effort, "max") == 0) {
                  has_changes = true;
                  sent_effort = true;
                  safe_strscpy(config.reasoning_effort, new_effort);
                  OLOG_INFO("WebUI: Session reasoning_effort set to '%s'", config.reasoning_effort);
               } else {
                  OLOG_WARNING("WebUI: Rejected invalid reasoning_effort '%s' from client",
                               new_effort);
               }
            }
         }

         /* A restore push carries a loaded conversation's own settings. */
         bool from_restore = false;
         struct json_object *restore_obj;
         if (json_object_object_get_ex(payload, "from_restore", &restore_obj)) {
            from_restore = json_object_get_boolean(restore_obj);
         }

         /* Apply config if changes were made.  The reasoning pick is stored as
          * picked (each request resolves it for its model); the reply says
          * whether the user's own change was adjusted, never for a restore. */
         if (has_changes) {
            int rc = session_set_llm_config(conn->session, &config);
            if (rc == 0) {
               /* What the session got: a provider without a key falls back. */
               session_get_llm_config(conn->session, &config);
            }
            if (rc == 0 && !from_restore) {
               reasoning_adjusted = webui_reasoning_adjusted(&config, sent_mode, sent_effort);
            }
            if (rc != 0) {
               success = 0;
               error_msg = "API key not configured for requested provider";
            } else {
               OLOG_INFO("WebUI: Session %u LLM config updated (type=%d, provider=%d)",
                         conn->session->session_id, config.type, config.cloud_provider);

               /* If the local model or the LLM type changed, a reset included,
                * the context size may differ. */
               if (config.type == LLM_LOCAL &&
                   (strcmp(old_model, config.model) != 0 || old_type != config.type)) {
                  llm_context_refresh_local();
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
               if (!from_restore && conn->active_conversation_id > 0) {
                  const char *type_str = config.type == LLM_LOCAL ? "local" : "cloud";
                  /* tools_mode column is retired (dead) — pass empty. */
                  conv_db_update_llm_settings(conn->active_conversation_id, conn->auth_user_id,
                                              type_str,
                                              cloud_provider_to_string(config.cloud_provider),
                                              config.model, "", config.thinking_mode,
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

         /* The model that runs, as get_config reports it (the resolver drops a
          * bare id under the OpenRouter gateway, for one). */
         llm_resolved_config_t resolved;
         const char *model_name = llm_resolve_config(&current, &resolved) == 0
                                      ? webui_effective_model_name(&resolved)
                                      : current.model;
         json_object_object_add(resp_payload, "model",
                                json_object_new_string(model_name ? model_name : ""));

         /* Effective reasoning, with the model's capabilities, so the client
          * control shows what runs and only what the model takes. */
         webui_reasoning_stamp(resp_payload, &current);
         json_object_object_add(resp_payload, "reasoning_adjusted",
                                json_object_new_boolean(reasoning_adjusted));
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
      webui_protocol_note_client(payload, &conn->client_noted);
      /* Session reconnection with stored token.  A login ended since this
       * connection opened reconnects nothing (and the connection closes). */
      if (payload && conn->authenticated && !webui_conn_login_valid(conn)) {
         return;
      }
      if (payload) {
         struct json_object *token_obj;
         if (json_object_object_get_ex(payload, "token", &token_obj)) {
            const char *token = json_object_get_string(token_obj);
            if (token && strlen(token) > 0) {
               session_t *existing = lookup_session_by_token(token);
               if (existing && (!webui_session_owned_by(existing, conn->auth_user_id) ||
                                !webui_conn_may_resume(conn, existing))) {
                  /* Another user's or another login's session: never attach to it. */
                  OLOG_WARNING("WebUI: reconnect token's session isn't this login's; "
                               "not attaching");
                  session_release(existing); /* the lookup's reference */
                  existing = NULL;
               }
               if (existing && existing == conn->session) {
                  /* Already this connection's session: its own reference stays,
                   * the lookup's goes. */
                  session_release(existing);
               } else if (existing) {
                  /* Switching to it: first the abandoned session (just
                   * auto-created on connect).  session_destroy() ->
                   * webui_detach_session() releases the connection's reference to
                   * it (it finds this conn still attached); releasing it here too
                   * would double-decrement and free it early. */
                  if (conn->session) {
                     uint32_t abandoned_id = conn->session->session_id;
                     conn->session->client_data = NULL;
                     session_destroy(abandoned_id);
                     unregister_tokens_for_session(abandoned_id);
                     OLOG_INFO("WebUI: Destroyed abandoned session %u", abandoned_id);
                  }
                  if (!webui_conn_attach_session(conn, existing)) {
                     /* Destroyed meanwhile: a fresh session instead (below). */
                     OLOG_WARNING("WebUI: session %u is ending; not attaching",
                                  existing->session_id);
                     session_release(existing);
                     existing = NULL;
                  }
               }
               if (existing) {
                  /* Evict any other connection still owning this session BEFORE
                   * taking ownership, so the superseded tab backs off (WS 4001)
                   * rather than fighting to re-steal it. */
                  webui_evict_session_owner(existing, conn);
                  conn->session_was_reconnected = true;
                  existing->client_data = conn;
                  webui_conn_publish_view(conn); /* the session shows what this connection does */
                  existing->disconnected = false;
                  safe_strscpy(conn->session_token, token);

                  /* Restore connection capabilities from init payload */
                  conn->use_opus = check_opus_capability(payload);
                  struct json_object *tts_obj;
                  if (json_object_object_get_ex(payload, "tts_enabled", &tts_obj)) {
                     conn->tts_enabled = json_object_get_boolean(tts_obj);
                  }
                  /* Living tool pills: client renders its OWN turn's tool steps from the
                   * tool_step frame, so the server includes it in the fan (default off). */
                  struct json_object *tso_obj;
                  if (json_object_object_get_ex(payload, "tool_step_origin", &tso_obj)) {
                     conn->tool_step_origin = json_object_get_boolean(tso_obj);
                  }
                  /* Session-keepalive intent hint (display only). The authoritative
                   * renewal gate is the persisted keepalive_enabled DB flag, which
                   * survives reconnects — so no re-enable is needed here. */
                  struct json_object *ka_obj;
                  if (json_object_object_get_ex(payload, "session_keepalive", &ka_obj)) {
                     conn->session_keepalive = json_object_get_boolean(ka_obj);
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
                        webui_conn_own_session(conn, conn->session);
                        /* Set user_id for metrics and memory extraction */
                        session_set_metrics_user(conn->session, conn->auth_user_id);
                        /* A new context: its first turn freezes the prompt. */
                        session_clear_history(conn->session);
                        conn->session->client_data = conn;
                        webui_conn_publish_view(
                            conn); /* the session shows what this connection does */
                        /* Fresh session (reconnect token stale) — not a true reconnect,
                         * so the session frame must report reconnected:false.  Enforced
                         * locally at all four fresh-create sites. */
                        conn->session_was_reconnected = false;
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
                     struct json_object *tso_obj;
                     if (json_object_object_get_ex(payload, "tool_step_origin", &tso_obj)) {
                        conn->tool_step_origin = json_object_get_boolean(tso_obj);
                     }
                     struct json_object *ka_obj;
                     if (json_object_object_get_ex(payload, "session_keepalive", &ka_obj)) {
                        conn->session_keepalive = json_object_get_boolean(ka_obj);
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
      webui_protocol_note_client(payload, &conn->client_noted);
      /* Init message arrived on an already-authenticated connection (cookie auth
       * auto-created the session before this message was processed). Sync capabilities. */
      if (payload) {
         conn->use_opus = check_opus_capability(payload);
         struct json_object *tts_obj;
         if (json_object_object_get_ex(payload, "tts_enabled", &tts_obj)) {
            conn->tts_enabled = json_object_get_boolean(tts_obj);
         }
         struct json_object *tso_obj;
         if (json_object_object_get_ex(payload, "tool_step_origin", &tso_obj)) {
            conn->tool_step_origin = json_object_get_boolean(tso_obj);
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
   } else if (strcmp(type, "verify_channel") == 0) {
      handle_verify_channel(conn, payload);
   } else if (strcmp(type, "resend_channel_code") == 0) {
      handle_resend_channel_code(conn, payload);
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
   } else if (strcmp(type, "set_active_conversation") == 0) {
      /* Lightweight reconnect re-anchor: reset conn->active_conversation_id
       * without the full history replay a load_conversation does. */
      handle_set_active_conversation(conn, payload);
   } else if (strcmp(type, "attach_conversation") == 0) {
      /* Same handler as load_conversation — an attach IS a load that also asks
       * for the durable event log.  The distinction is the `last_seq` cursor in
       * the payload, which the handler keys off; keeping them one code path
       * means the ownership check, message batch and ring replay can't drift
       * apart from each other (§6 attach ordering). */
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
   } else if (strcmp(type, "forget_conversation_memories") == 0) {
      handle_forget_conversation_memories(conn, payload);
   } else if (strcmp(type, "conversation_learned_request") == 0) {
      handle_conversation_learned_request(conn, payload);
   } else if (strcmp(type, "set_pinned") == 0) {
      if (payload) {
         handle_set_pinned(conn, payload);
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
   } else if (strcmp(type, "doc_library_get") == 0) {
      /* No payload guard: the handler emits a "Missing document id" response for a
         payload-less frame, so the client's async request always resolves. */
      handle_doc_library_get(conn, payload);
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
   } else if (strcmp(type, "doc_library_doc_update") == 0) {
      if (payload) {
         handle_doc_library_doc_update(conn, payload);
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
   } else if (strcmp(type, "code_projects_link") == 0) {
      handle_code_projects_link(conn, payload);
   } else if (strcmp(type, "code_projects_refresh") == 0) {
      handle_code_projects_refresh(conn, payload);
   } else if (strcmp(type, "code_projects_rebuild") == 0) {
      handle_code_projects_rebuild(conn, payload);
   } else if (strcmp(type, "code_projects_set_branch") == 0) {
      handle_code_projects_set_branch(conn, payload);
   } else if (strcmp(type, "code_projects_delete") == 0) {
      handle_code_projects_delete(conn, payload);
   }
#endif
#ifdef DAWN_ENABLE_SCHWAB_TOOL
   /* Stocks panel (owner's Schwab portfolio) */
   else if (strcmp(type, "stocks_portfolio_subscribe") == 0) {
      handle_stocks_portfolio_subscribe(conn, payload);
   } else if (strcmp(type, "stocks_portfolio_unsubscribe") == 0) {
      handle_stocks_portfolio_unsubscribe(conn, payload);
   } else if (strcmp(type, "stocks_portfolio_get") == 0) {
      handle_stocks_portfolio_get(conn, payload);
   } else if (strcmp(type, "stocks_watch_subscribe") == 0) {
      handle_stocks_watch_subscribe(conn, payload);
   } else if (strcmp(type, "stocks_watch_unsubscribe") == 0) {
      handle_stocks_watch_unsubscribe(conn, payload);
   } else if (strcmp(type, "stocks_watch_get") == 0) {
      handle_stocks_watch_get(conn, payload);
   } else if (strcmp(type, "stocks_watch_set") == 0) {
      handle_stocks_watch_set(conn, payload);
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
   } else if (strcmp(type, "calendar_list_my_calendars") == 0) {
      /* Flat id->{name,color} map across accounts; no payload. */
      handle_calendar_list_my_calendars(conn);
   } else if (strcmp(type, "calendar_upcoming_events") == 0) {
      /* Read-only pull; payload optional (defaults to a 7-day window). */
      handle_calendar_upcoming_events(conn, payload);
   }
#endif /* DAWN_ENABLE_CALENDAR_TOOL */
#ifdef DAWN_ENABLE_EMAIL_TOOL
   else if (strcmp(type, "email_list_accounts") == 0) {
      handle_email_list_accounts(conn, payload);
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
   /* The mail panel: payload optional where every member is. */
   else if (strcmp(type, "email_list") == 0) {
      handle_email_list(conn, payload);
   } else if (strcmp(type, "email_search") == 0) {
      handle_email_search(conn, payload);
   } else if (strcmp(type, "email_read") == 0) {
      handle_email_read(conn, payload);
   } else if (strcmp(type, "email_set_flags") == 0) {
      handle_email_set_flags(conn, payload);
   } else if (strcmp(type, "email_unread_counts") == 0) {
      handle_email_unread_counts(conn, payload);
   } else if (strcmp(type, "email_archive") == 0) {
      handle_email_archive(conn, payload);
   } else if (strcmp(type, "email_trash") == 0) {
      handle_email_trash(conn, payload);
   } else if (strcmp(type, "email_undo") == 0) {
      handle_email_undo(conn, payload);
   }
#endif /* DAWN_ENABLE_EMAIL_TOOL */
   /* Watches (SAGE proactive attention) — per-user attention_rules CRUD */
   else if (strcmp(type, "watch_list") == 0) {
      handle_watch_list(conn);
   } else if (strcmp(type, "watch_add") == 0) {
      if (payload) {
         handle_watch_add(conn, payload);
      }
   } else if (strcmp(type, "watch_update") == 0) {
      if (payload) {
         handle_watch_update(conn, payload);
      }
   } else if (strcmp(type, "watch_set_enabled") == 0) {
      if (payload) {
         handle_watch_set_enabled(conn, payload);
      }
   } else if (strcmp(type, "watch_remove") == 0) {
      if (payload) {
         handle_watch_remove(conn, payload);
      }
   } else if (strcmp(type, "watch_readings_subscribe") == 0) {
      handle_watch_readings_subscribe(conn, payload);
   }
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
   else if (strcmp(type, "phone_action") == 0) {
      /* Answer / reject a ringing call, or hang up an active one, from a
       * browser banner / in-call panel button. */
      if (!conn_require_auth(conn))
         return;
      if (!payload)
         return;

      json_object *action_obj = NULL;
      json_object_object_get_ex(payload, "action", &action_obj);
      const char *action = action_obj ? json_object_get_string(action_obj) : NULL;
      if (!action || (strcmp(action, "answer") != 0 && strcmp(action, "reject") != 0 &&
                      strcmp(action, "hangup") != 0)) {
         send_error_impl(conn->wsi, "INVALID_PARAM",
                         "phone_action requires action=answer|reject|hangup");
      } else {
         webui_phone_handle_action(conn, action);
      }
   } else if (strcmp(type, "phone_status") == 0) {
      /* Client asks for the current active call (reconnect rehydration). */
      if (!conn_require_auth(conn))
         return;
      webui_phone_send_status(conn);
   } else if (strcmp(type, "jobs_request") == 0) {
      /* Client asks for its complete ACTIVE job set (connect/reconnect). */
      if (!conn_require_auth(conn))
         return;
      webui_jobs_send_snapshot(conn);
   } else if (strcmp(type, "list_jobs") == 0) {
      /* Client asks for a page of TERMINAL jobs.  Separate from the snapshot
       * because history is unbounded — see src/webui/webui_jobs.c. */
      if (!conn_require_auth(conn))
         return;
      int64_t before_created_at = 0, before_id = 0;
      int limit = 0;
      if (payload) {
         json_object *o;
         if (json_object_object_get_ex(payload, "before_created_at", &o))
            before_created_at = json_object_get_int64(o);
         if (json_object_object_get_ex(payload, "before_id", &o))
            before_id = json_object_get_int64(o);
         if (json_object_object_get_ex(payload, "limit", &o))
            limit = json_object_get_int(o);
      }
      webui_jobs_send_history(conn, before_created_at, before_id, limit);
   } else if (strcmp(type, "job_action") == 0) {
      /* cancel | resume.  Ownership is checked inside, against conn->auth_user_id. */
      if (!conn_require_auth(conn))
         return;
      webui_jobs_handle_action(conn, payload);
   } else if (strcmp(type, "scheduler_action") == 0) {
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
      } else if (strcmp(action, "update") == 0) {
         /* Edit a briefing's summarization instructions from the panel.  Only a
          * string-typed field is honored (empty string clears); a missing or
          * non-string field is rejected by the handler rather than clearing. */
         json_object *instr_obj = NULL;
         json_object_object_get_ex(payload, "instructions", &instr_obj);
         const char *instructions = (instr_obj && json_object_is_type(instr_obj, json_type_string))
                                        ? json_object_get_string(instr_obj)
                                        : NULL;
         handle_scheduler_update_instructions(conn, event_id, instructions);
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
   } else if (strcmp(type, "session_keepalive_enable") == 0) {
      if (!conn_require_auth(conn)) {
         return;
      }
      /* Informed consent recorded server-side (the client showed a security
       * warning the user accepted). Persist on the session row so renewal in
       * handle_ping is authorized by DB state, not the connect-time payload hint. */
      conn->session_keepalive = true;
      if (auth_db_set_session_keepalive(conn->auth_session_token, true) == AUTH_DB_SUCCESS) {
         auth_db_log_event("SESSION_KEEPALIVE_ENABLE", conn->username, conn->client_ip,
                           "always-on session keepalive enabled");
      }
   } else if (strcmp(type, "session_keepalive_disable") == 0) {
      if (!conn_require_auth(conn)) {
         return;
      }
      conn->session_keepalive = false;
      if (auth_db_set_session_keepalive(conn->auth_session_token, false) == AUTH_DB_SUCCESS) {
         auth_db_log_event("SESSION_KEEPALIVE_DISABLE", conn->username, conn->client_ip,
                           "always-on session keepalive disabled");
      }
   } else if (strcmp(type, "ping") == 0) {
      /* App-level liveness probe from a browser client (client→server).  Gated
       * on auth so only a valid, registered session gets a pong; the missing
       * pong (or the UNAUTHORIZED error) is the client's staleness signal. */
      handle_ping(conn, payload);
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
   } else {
      /* Answered, so a client fails at once instead of waiting out its timeout.
       * The browser sends ha_* whether or not the feature is compiled in. */
      const char *shown = webui_protocol_echo_ok(type) ? type : "(unprintable)";
      if (strncmp(type, "ha_", sizeof("ha_") - 1) == 0)
         OLOG_DEBUG("WebUI: Ignoring %s message (feature not compiled in)", shown);
      else
         OLOG_WARNING("WebUI: Unknown message type: %s", shown);
      char req[WEBUI_REQ_MAX + 1];
      char *reply = webui_protocol_unknown_type_json(
          type, webui_protocol_payload_req(payload, WEBUI_REQ_MAX, req, sizeof(req)) ? req : NULL);
      if (reply) {
         send_json_message(conn->wsi, reply);
         free(reply);
      }
   }
}

static void handle_cancel_message(ws_connection_t *conn) {
   if (conn->session) {
      OLOG_INFO("WebUI: Cancel requested for session %u", conn->session->session_id);
      /* Explicit Stop: abort THIS turn's generation, but the client stays
       * connected — so set cancel_requested (not disconnected).  session
       * manager hands &session->cancel_requested to llm_set_cancel_flag before
       * each LLM call, so the CURL progress callback aborts THIS session only.
       * (llm_request_interrupt() is NOT used — its global flag hits all users.) */
      session_cancel_turn(conn->session);
      send_state_impl(conn->wsi, "idle", NULL);
   }
}

/*
 * Browser (SESSION_TYPE_WEBUI) app-level liveness probe.  The counterpart to
 * handle_satellite_ping, gated on user auth rather than device registration:
 * conn_require_auth() re-validates the session token against the DB on every
 * call, so a revoked or expired session receives an UNAUTHORIZED error and NO
 * pong — precisely the "session no longer alive" signal the client keys off to
 * fall back to re-authentication.  A valid session gets a pong (with any echoed
 * seq + server_time_ms) and has its last_activity refreshed by webui_send_pong.
 */
static void handle_ping(ws_connection_t *conn, struct json_object *payload) {
   auth_session_t session;
   if (!conn_require_auth_ex(conn, &session)) {
      return;
   }

   /* Session-keepalive (always-on): slide the auth token's expiry forward while
    * this browser actively heartbeats — but only if the user enabled keepalive
    * (persisted DB flag, not the connect-time hint), and only when within half a
    * window of expiry so we write at most ~once per 12h rather than every ping.
    * The absolute cap bounds a stolen token's worst-case lifetime.
    * TODO(security): downgrade renewal (fall back to fixed window) on an IP/UA
    * mismatch vs the stored session, and keep step-up re-auth on sensitive ops. */
   if (session.keepalive_enabled) {
      time_t now = time(NULL);
      if (session.expires_at - now < AUTH_SESSION_TIMEOUT_SEC / 2) {
         time_t slid = session.expires_at > now + AUTH_SESSION_TIMEOUT_SEC
                           ? session.expires_at
                           : now + AUTH_SESSION_TIMEOUT_SEC;
         time_t capped = session.created_at + AUTH_SESSION_ABSOLUTE_CAP_SEC;
         time_t new_expires = slid < capped ? slid : capped;
         if (new_expires > session.expires_at) {
            auth_db_renew_session(conn->auth_session_token, new_expires);
         }
      }
   }

   webui_send_pong(conn, "pong", payload);
}
