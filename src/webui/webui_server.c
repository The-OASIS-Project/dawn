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
 * WebUI Server Implementation - libwebsockets HTTP + WebSocket handling
 */

#include "webui/webui_server.h"

#include <json-c/json.h>
#include <libwebsockets.h>
#include <mosquitto.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#ifdef ENABLE_WEBUI_AUDIO
#include "webui/webui_audio.h"
#endif

#include "config/dawn_config.h"
#include "core/command_router.h"
#include "core/session_manager.h"
#include "core/worker_pool.h"
#include "dawn.h"
#include "llm/llm_command_parser.h"
#include "logging.h"

/* =============================================================================
 * Module State
 * ============================================================================= */

static struct lws_context *s_lws_context = NULL;
static pthread_t s_webui_thread;
static volatile int s_running = 0;
static volatile int s_client_count = 0;
static int s_port = 0;
static char s_www_path[256] = { 0 };
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;

/* =============================================================================
 * Response Queue (worker -> WebUI thread)
 *
 * Workers cannot call lws_write() directly (not thread-safe).
 * They queue responses here, then call lws_cancel_service() to wake
 * the WebUI thread, which processes the queue in LWS_CALLBACK_EVENT_WAIT_CANCELLED.
 * ============================================================================= */

typedef struct {
   session_t *session;
   ws_response_type_t type;
   union {
      struct {
         char *state;
      } state;
      struct {
         char *role;
         char *text;
      } transcript;
      struct {
         char *code;
         char *message;
      } error;
      struct {
         char *token;
      } session_token;
      struct {
         uint8_t *data;
         size_t len;
      } audio;
   };
} ws_response_t;

static ws_response_t s_response_queue[WEBUI_RESPONSE_QUEUE_SIZE];
static int s_queue_head = 0;
static int s_queue_tail = 0;
static pthread_mutex_t s_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

/* =============================================================================
 * Token-to-Session Mapping (for reconnection)
 * ============================================================================= */

#define MAX_TOKEN_MAPPINGS 16

typedef struct {
   char token[WEBUI_SESSION_TOKEN_LEN];
   uint32_t session_id;
   time_t created;
   bool in_use;
} token_mapping_t;

static token_mapping_t s_token_map[MAX_TOKEN_MAPPINGS];
static pthread_mutex_t s_token_mutex = PTHREAD_MUTEX_INITIALIZER;

static void register_token(const char *token, uint32_t session_id) {
   pthread_mutex_lock(&s_token_mutex);

   /* Find existing or empty slot */
   int empty_slot = -1;
   for (int i = 0; i < MAX_TOKEN_MAPPINGS; i++) {
      if (s_token_map[i].in_use && strcmp(s_token_map[i].token, token) == 0) {
         /* Update existing */
         s_token_map[i].session_id = session_id;
         s_token_map[i].created = time(NULL);
         pthread_mutex_unlock(&s_token_mutex);
         return;
      }
      if (!s_token_map[i].in_use && empty_slot < 0) {
         empty_slot = i;
      }
   }

   if (empty_slot >= 0) {
      strncpy(s_token_map[empty_slot].token, token, WEBUI_SESSION_TOKEN_LEN - 1);
      s_token_map[empty_slot].token[WEBUI_SESSION_TOKEN_LEN - 1] = '\0';
      s_token_map[empty_slot].session_id = session_id;
      s_token_map[empty_slot].created = time(NULL);
      s_token_map[empty_slot].in_use = true;
   } else {
      /* Table full - evict oldest */
      int oldest = 0;
      for (int i = 1; i < MAX_TOKEN_MAPPINGS; i++) {
         if (s_token_map[i].created < s_token_map[oldest].created) {
            oldest = i;
         }
      }
      strncpy(s_token_map[oldest].token, token, WEBUI_SESSION_TOKEN_LEN - 1);
      s_token_map[oldest].token[WEBUI_SESSION_TOKEN_LEN - 1] = '\0';
      s_token_map[oldest].session_id = session_id;
      s_token_map[oldest].created = time(NULL);
   }

   pthread_mutex_unlock(&s_token_mutex);
}

static session_t *lookup_session_by_token(const char *token) {
   pthread_mutex_lock(&s_token_mutex);

   for (int i = 0; i < MAX_TOKEN_MAPPINGS; i++) {
      if (s_token_map[i].in_use && strcmp(s_token_map[i].token, token) == 0) {
         uint32_t session_id = s_token_map[i].session_id;
         pthread_mutex_unlock(&s_token_mutex);

         /* Look up actual session - use reconnect variant to allow disconnected sessions */
         session_t *session = session_get_for_reconnect(session_id);
         if (session) {
            /* Session exists - reconnect handler will clear disconnected flag */
            LOG_INFO("WebUI: Found existing session %u for token %.8s...", session_id, token);
            return session;
         }
         LOG_INFO("WebUI: Token %.8s... mapped to session %u but session destroyed", token,
                  session_id);
         return NULL;
      }
   }

   pthread_mutex_unlock(&s_token_mutex);
   return NULL;
}

/* =============================================================================
 * Per-WebSocket Connection Data
 * ============================================================================= */

typedef struct {
   struct lws *wsi;                             /* libwebsockets handle */
   session_t *session;                          /* Session manager reference */
   char session_token[WEBUI_SESSION_TOKEN_LEN]; /* Reconnection token */
   uint8_t *audio_buffer;                       /* Phase 4: Opus audio accumulation */
   size_t audio_buffer_len;
   size_t audio_buffer_capacity;
   bool in_binary_fragment; /* True if receiving fragmented binary frame */
   uint8_t binary_msg_type; /* Message type from first fragment */
} ws_connection_t;

/* =============================================================================
 * MIME Type Mapping
 * ============================================================================= */

static const struct {
   const char *extension;
   const char *mime_type;
} s_mime_types[] = {
   { ".html", "text/html" },        { ".htm", "text/html" },
   { ".css", "text/css" },          { ".js", "application/javascript" },
   { ".json", "application/json" }, { ".png", "image/png" },
   { ".jpg", "image/jpeg" },        { ".jpeg", "image/jpeg" },
   { ".gif", "image/gif" },         { ".svg", "image/svg+xml" },
   { ".ico", "image/x-icon" },      { ".woff", "font/woff" },
   { ".woff2", "font/woff2" },      { ".ttf", "font/ttf" },
   { ".txt", "text/plain" },        { NULL, NULL },
};

static const char *get_mime_type(const char *path) {
   const char *ext = strrchr(path, '.');
   if (!ext) {
      return "application/octet-stream";
   }

   for (int i = 0; s_mime_types[i].extension != NULL; i++) {
      if (strcasecmp(ext, s_mime_types[i].extension) == 0) {
         return s_mime_types[i].mime_type;
      }
   }

   return "application/octet-stream";
}

/* =============================================================================
 * HTTP Session Data (minimal - just for lws requirements)
 * ============================================================================= */

struct http_session_data {
   int placeholder;
};

/* =============================================================================
 * Session Token Generation
 * ============================================================================= */

static void generate_session_token(char token_out[WEBUI_SESSION_TOKEN_LEN]) {
   uint8_t random_bytes[16];
   if (getrandom(random_bytes, 16, 0) != 16) {
      /* Fallback to less secure random if getrandom fails */
      for (int i = 0; i < 16; i++) {
         random_bytes[i] = (uint8_t)(rand() & 0xFF);
      }
   }
   for (int i = 0; i < 16; i++) {
      snprintf(&token_out[i * 2], 3, "%02x", random_bytes[i]);
   }
   token_out[32] = '\0';
}

/* =============================================================================
 * Response Queue Functions
 * ============================================================================= */

static void queue_response(ws_response_t *resp) {
   pthread_mutex_lock(&s_queue_mutex);

   int next_tail = (s_queue_tail + 1) % WEBUI_RESPONSE_QUEUE_SIZE;
   if (next_tail == s_queue_head) {
      /* Queue full - drop oldest */
      LOG_WARNING("WebUI: Response queue full, dropping oldest entry");
      s_queue_head = (s_queue_head + 1) % WEBUI_RESPONSE_QUEUE_SIZE;
   }

   s_response_queue[s_queue_tail] = *resp;
   s_queue_tail = next_tail;

   LOG_INFO("WebUI: Queued response type=%d for session %u", resp->type,
            resp->session ? resp->session->session_id : 0);

   pthread_mutex_unlock(&s_queue_mutex);

   /* Wake up lws_service() to process queue */
   if (s_lws_context) {
      lws_cancel_service(s_lws_context);
   }
}

static void free_response(ws_response_t *resp) {
   switch (resp->type) {
      case WS_RESP_STATE:
         free(resp->state.state);
         break;
      case WS_RESP_TRANSCRIPT:
         free(resp->transcript.role);
         free(resp->transcript.text);
         break;
      case WS_RESP_ERROR:
         free(resp->error.code);
         free(resp->error.message);
         break;
      case WS_RESP_SESSION:
         free(resp->session_token.token);
         break;
      case WS_RESP_AUDIO:
         free(resp->audio.data);
         break;
      case WS_RESP_AUDIO_END:
         /* No data to free */
         break;
   }
}

/* =============================================================================
 * WebSocket Send Helpers (called from WebUI thread only)
 * ============================================================================= */

/* LWS requires LWS_PRE bytes before the buffer for protocol framing */
#define WS_SEND_BUFFER_SIZE 4096

static int send_json_message(struct lws *wsi, const char *json) {
   size_t len = strlen(json);
   if (len >= WS_SEND_BUFFER_SIZE - LWS_PRE) {
      LOG_ERROR("WebUI: JSON message too large (%zu bytes)", len);
      return -1;
   }

   unsigned char buf[LWS_PRE + WS_SEND_BUFFER_SIZE];
   memcpy(&buf[LWS_PRE], json, len);

   int written = lws_write(wsi, &buf[LWS_PRE], len, LWS_WRITE_TEXT);
   if (written < (int)len) {
      LOG_ERROR("WebUI: lws_write failed (wrote %d of %zu)", written, len);
      return -1;
   }

   return 0;
}

static int send_binary_message(struct lws *wsi, uint8_t msg_type, const uint8_t *data, size_t len) {
   if (!wsi) {
      LOG_ERROR("WebUI: send_binary_message called with NULL wsi");
      return -1;
   }

   /* Allocate buffer with LWS_PRE padding + 1 byte for type + data */
   size_t total_len = 1 + len;
   unsigned char *buf = malloc(LWS_PRE + total_len);
   if (!buf) {
      LOG_ERROR("WebUI: Failed to allocate binary send buffer (%zu bytes)", LWS_PRE + total_len);
      return -1;
   }

   buf[LWS_PRE] = msg_type;
   if (data && len > 0) {
      memcpy(&buf[LWS_PRE + 1], data, len);
   }

   LOG_INFO("WebUI: Sending binary frame type=0x%02x len=%zu", msg_type, total_len);

   int written = lws_write(wsi, &buf[LWS_PRE], total_len, LWS_WRITE_BINARY);
   free(buf);

   if (written < 0) {
      LOG_ERROR("WebUI: lws_write binary failed with error %d", written);
      return -1;
   }

   if (written < (int)total_len) {
      LOG_ERROR("WebUI: lws_write binary partial write (%d of %zu)", written, total_len);
      return -1;
   }

   LOG_INFO("WebUI: Binary frame sent successfully (%d bytes)", written);
   return 0;
}

static void send_audio_impl(struct lws *wsi, const uint8_t *data, size_t len) {
   int ret = send_binary_message(wsi, WS_BIN_AUDIO_OUT, data, len);
   if (ret != 0) {
      LOG_ERROR("WebUI: Failed to send audio chunk (%zu bytes)", len);
   }
}

static void send_audio_end_impl(struct lws *wsi) {
   send_binary_message(wsi, WS_BIN_AUDIO_OUT_END, NULL, 0);
}

static void send_state_impl(struct lws *wsi, const char *state) {
   char json[256];
   snprintf(json, sizeof(json), "{\"type\":\"state\",\"payload\":{\"state\":\"%s\"}}", state);
   send_json_message(wsi, json);
}

static void send_transcript_impl(struct lws *wsi, const char *role, const char *text) {
   /* Escape JSON special characters in text */
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "role", json_object_new_string(role));
   json_object_object_add(payload, "text", json_object_new_string(text));
   json_object_object_add(obj, "type", json_object_new_string("transcript"));
   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

static void send_error_impl(struct lws *wsi, const char *code, const char *message) {
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "code", json_object_new_string(code));
   json_object_object_add(payload, "message", json_object_new_string(message));
   json_object_object_add(payload, "recoverable", json_object_new_boolean(1));
   json_object_object_add(obj, "type", json_object_new_string("error"));
   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

static void send_session_token_impl(struct lws *wsi, const char *token) {
   char json[256];
   snprintf(json, sizeof(json), "{\"type\":\"session\",\"payload\":{\"token\":\"%s\"}}", token);
   send_json_message(wsi, json);
}

static void send_config_impl(struct lws *wsi) {
   char json[256];
   snprintf(json, sizeof(json), "{\"type\":\"config\",\"payload\":{\"audio_chunk_ms\":%d}}",
            g_config.webui.audio_chunk_ms);
   send_json_message(wsi, json);
}

/**
 * @brief Send conversation history to client on reconnect
 *
 * Iterates through the session's conversation history and sends each
 * user/assistant message as a transcript. Skips system messages.
 *
 * @param wsi WebSocket instance
 * @param session Session with conversation history
 */
static void send_history_impl(struct lws *wsi, session_t *session) {
   if (!wsi || !session) {
      return;
   }

   struct json_object *history = session_get_history(session);
   if (!history) {
      LOG_WARNING("WebUI: Failed to get history for session %u", session->session_id);
      return;
   }

   int len = json_object_array_length(history);
   int sent_count = 0;

   LOG_INFO("WebUI: Sending %d history entries to reconnected client", len);

   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(history, i);
      if (!msg)
         continue;

      struct json_object *role_obj = NULL;
      struct json_object *content_obj = NULL;

      if (!json_object_object_get_ex(msg, "role", &role_obj) ||
          !json_object_object_get_ex(msg, "content", &content_obj)) {
         continue;
      }

      const char *role = json_object_get_string(role_obj);
      const char *content = json_object_get_string(content_obj);

      /* Skip system messages (prompts) - only send user/assistant */
      if (!role || !content || strcmp(role, "system") == 0) {
         continue;
      }

      send_transcript_impl(wsi, role, content);
      sent_count++;
   }

   /* Release the reference we got from session_get_history */
   json_object_put(history);

   LOG_INFO("WebUI: Sent %d transcript entries to reconnected client", sent_count);
}

/* =============================================================================
 * Response Queue Processing (called from WebUI thread)
 *
 * IMPORTANT: libwebsockets only allows ONE lws_write() per writeable callback.
 * This function processes only ONE response per call. If more responses are
 * pending, it requests a writeable callback via lws_callback_on_writable().
 * ============================================================================= */

static void process_one_response(void) {
   pthread_mutex_lock(&s_queue_mutex);

   if (s_queue_head == s_queue_tail) {
      pthread_mutex_unlock(&s_queue_mutex);
      return;
   }

   ws_response_t resp = s_response_queue[s_queue_head];
   s_queue_head = (s_queue_head + 1) % WEBUI_RESPONSE_QUEUE_SIZE;
   bool more_pending = (s_queue_head != s_queue_tail);
   pthread_mutex_unlock(&s_queue_mutex);

   /* Find connection for this session */
   if (!resp.session || resp.session->disconnected) {
      free_response(&resp);
      /* If more responses, schedule another callback */
      if (more_pending && s_lws_context) {
         lws_cancel_service(s_lws_context);
      }
      return;
   }

   ws_connection_t *conn = (ws_connection_t *)resp.session->client_data;
   if (!conn || !conn->wsi) {
      free_response(&resp);
      if (more_pending && s_lws_context) {
         lws_cancel_service(s_lws_context);
      }
      return;
   }

   /* Send via lws_write (one write per callback!) */
   LOG_INFO("WebUI: Sending response type=%d to session %u", resp.type, resp.session->session_id);

   switch (resp.type) {
      case WS_RESP_STATE:
         send_state_impl(conn->wsi, resp.state.state);
         free(resp.state.state);
         break;
      case WS_RESP_TRANSCRIPT:
         send_transcript_impl(conn->wsi, resp.transcript.role, resp.transcript.text);
         free(resp.transcript.role);
         free(resp.transcript.text);
         break;
      case WS_RESP_ERROR:
         send_error_impl(conn->wsi, resp.error.code, resp.error.message);
         free(resp.error.code);
         free(resp.error.message);
         break;
      case WS_RESP_SESSION:
         send_session_token_impl(conn->wsi, resp.session_token.token);
         free(resp.session_token.token);
         break;
      case WS_RESP_AUDIO:
         send_audio_impl(conn->wsi, resp.audio.data, resp.audio.len);
         free(resp.audio.data);
         break;
      case WS_RESP_AUDIO_END:
         send_audio_end_impl(conn->wsi);
         break;
   }

   /* If more responses pending, request writeable callback for this connection */
   if (more_pending) {
      lws_callback_on_writable(conn->wsi);
   }
}

/* Legacy name for backward compatibility */
static void process_response_queue(void) {
   process_one_response();
}

/* =============================================================================
 * HTTP Protocol Callbacks
 * ============================================================================= */

static int callback_http(struct lws *wsi,
                         enum lws_callback_reasons reason,
                         void *user,
                         void *in,
                         size_t len) {
   (void)user; /* Unused in Phase 1 */

   switch (reason) {
      case LWS_CALLBACK_FILTER_NETWORK_CONNECTION:
      case LWS_CALLBACK_FILTER_HTTP_CONNECTION:
         /* Allow all connections */
         return 0;

      case LWS_CALLBACK_HTTP: {
         /* Serve static files - allocate buffers only when needed */
         char path[512];
         char filepath[768];
         const char *mime_type;
         int n;

         if (len < 1) {
            lws_return_http_status(wsi, HTTP_STATUS_BAD_REQUEST, NULL);
            return -1;
         }

         /* Get requested path */
         strncpy(path, (const char *)in, sizeof(path) - 1);
         path[sizeof(path) - 1] = '\0';

         /* Default to index.html for root */
         if (strcmp(path, "/") == 0) {
            strncpy(path, "/index.html", sizeof(path) - 1);
         }

         /* Prevent directory traversal */
         if (strstr(path, "..") != NULL) {
            LOG_WARNING("WebUI: Directory traversal attempt blocked: %s", path);
            lws_return_http_status(wsi, HTTP_STATUS_FORBIDDEN, NULL);
            return -1;
         }

         /* Build full filesystem path */
         snprintf(filepath, sizeof(filepath), "%s%s", s_www_path, path);

         /* Get MIME type */
         mime_type = get_mime_type(filepath);

         /* Serve the file */
         n = lws_serve_http_file(wsi, filepath, mime_type, NULL, 0);
         if (n < 0) {
            /* File not found or error */
            LOG_WARNING("WebUI: File not found: %s", filepath);
            lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL);
            return -1;
         }
         if (n > 0) {
            /* File is being sent, connection will close after */
            return 0;
         }
         break;
      }

      case LWS_CALLBACK_HTTP_FILE_COMPLETION:
         /* File transfer complete */
         return -1; /* Close connection */

      default:
         break;
   }

   return 0;
}

/* =============================================================================
 * JSON Message Handling
 * ============================================================================= */

static void handle_text_message(ws_connection_t *conn, const char *json_str, size_t len);
static void handle_cancel_message(ws_connection_t *conn);
#ifdef ENABLE_WEBUI_AUDIO
static void handle_binary_message(ws_connection_t *conn, const uint8_t *data, size_t len);
#endif

static void handle_json_message(ws_connection_t *conn, const char *data, size_t len) {
   /* Null-terminate for JSON parsing */
   char *json_str = strndup(data, len);
   if (!json_str) {
      LOG_ERROR("WebUI: Failed to allocate JSON string");
      return;
   }

   struct json_object *root = json_tokener_parse(json_str);
   if (!root) {
      LOG_WARNING("WebUI: Invalid JSON received: %.*s", (int)len, data);
      free(json_str);
      return;
   }

   /* Get message type */
   struct json_object *type_obj;
   if (!json_object_object_get_ex(root, "type", &type_obj)) {
      LOG_WARNING("WebUI: JSON missing 'type' field");
      json_object_put(root);
      free(json_str);
      return;
   }

   const char *type = json_object_get_string(type_obj);
   struct json_object *payload;
   json_object_object_get_ex(root, "payload", &payload);

   if (strcmp(type, "text") == 0) {
      /* Text input from user */
      if (payload) {
         struct json_object *text_obj;
         if (json_object_object_get_ex(payload, "text", &text_obj)) {
            const char *text = json_object_get_string(text_obj);
            if (text && strlen(text) > 0) {
               handle_text_message(conn, text, strlen(text));
            }
         }
      }
   } else if (strcmp(type, "cancel") == 0) {
      /* Cancel current operation */
      handle_cancel_message(conn);
   } else if (strcmp(type, "config") == 0) {
      /* Configuration update - Phase 5 */
      LOG_INFO("WebUI: Config message received (not yet implemented)");
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
                     LOG_INFO("WebUI: Destroyed abandoned session %u", abandoned_id);
                  }
                  conn->session = existing;
                  existing->client_data = conn;
                  existing->disconnected = false;
                  strncpy(conn->session_token, token, WEBUI_SESSION_TOKEN_LEN - 1);
                  conn->session_token[WEBUI_SESSION_TOKEN_LEN - 1] = '\0';

                  LOG_INFO("WebUI: Reconnected to session %u with token %.8s...",
                           existing->session_id, token);

                  /* Send confirmation, config, history replay, and current state */
                  send_session_token_impl(conn->wsi, token);
                  send_config_impl(conn->wsi);
                  send_history_impl(conn->wsi, existing);
                  send_state_impl(conn->wsi, "idle");
               } else {
                  /* Token not found or session expired - create new session */
                  LOG_INFO("WebUI: Token %.8s... not found, creating new session", token);
                  if (!conn->session) {
                     conn->session = session_create(SESSION_TYPE_WEBSOCKET, -1);
                     if (conn->session) {
                        session_init_system_prompt(conn->session, get_remote_command_prompt());
                        conn->session->client_data = conn;
                        generate_session_token(conn->session_token);
                        register_token(conn->session_token, conn->session->session_id);
                        send_session_token_impl(conn->wsi, conn->session_token);
                        send_config_impl(conn->wsi);
                        send_state_impl(conn->wsi, "idle");
                     }
                  }
               }
            }
         }
      }
   } else {
      LOG_WARNING("WebUI: Unknown message type: %s", type);
   }

   json_object_put(root);
   free(json_str);
}

static void handle_cancel_message(ws_connection_t *conn) {
   if (conn->session) {
      LOG_INFO("WebUI: Cancel requested for session %u", conn->session->session_id);
      conn->session->disconnected = true; /* Signal worker to abort */
      send_state_impl(conn->wsi, "idle");
   }
}

/* =============================================================================
 * WebSocket Protocol Callbacks
 * ============================================================================= */

static int callback_websocket(struct lws *wsi,
                              enum lws_callback_reasons reason,
                              void *user,
                              void *in,
                              size_t len) {
   ws_connection_t *conn = (ws_connection_t *)user;

   switch (reason) {
      case LWS_CALLBACK_ESTABLISHED: {
         /* New WebSocket connection - check client limit */
         pthread_mutex_lock(&s_mutex);
         if (s_client_count >= g_config.webui.max_clients) {
            pthread_mutex_unlock(&s_mutex);
            LOG_WARNING("WebUI: Connection rejected - max clients reached (%d)",
                        g_config.webui.max_clients);
            return -1; /* Reject connection */
         }
         s_client_count++;
         pthread_mutex_unlock(&s_mutex);

         /* Initialize connection structure */
         memset(conn, 0, sizeof(*conn));
         conn->wsi = wsi;

         /* Create session */
         conn->session = session_create(SESSION_TYPE_WEBSOCKET, -1);
         if (!conn->session) {
            LOG_ERROR("WebUI: Failed to create session (limit reached)");
            send_error_impl(wsi, "SESSION_LIMIT", "Maximum sessions reached");
            pthread_mutex_lock(&s_mutex);
            s_client_count--;
            pthread_mutex_unlock(&s_mutex);
            return -1;
         }

         /* Initialize session with system prompt (Friday persona) */
         session_init_system_prompt(conn->session, get_remote_command_prompt());

         /* Link session to connection for response routing */
         conn->session->client_data = conn;

         /* Generate session token and register mapping */
         generate_session_token(conn->session_token);
         register_token(conn->session_token, conn->session->session_id);

         /* Send token and config to client */
         send_session_token_impl(wsi, conn->session_token);
         send_config_impl(wsi);

         /* Send initial state */
         send_state_impl(wsi, "idle");

         LOG_INFO("WebUI: WebSocket client connected (session %u, token %.8s..., total: %d)",
                  conn->session->session_id, conn->session_token, s_client_count);
         break;
      }

      case LWS_CALLBACK_CLOSED: {
         /* WebSocket disconnected */
         LOG_INFO("WebUI: WebSocket client disconnecting (session %u)",
                  conn->session ? conn->session->session_id : 0);

         if (conn->session) {
            /* Mark session as disconnected (aborts any pending LLM calls) */
            conn->session->disconnected = true;
            conn->session->client_data = NULL;

            /* Don't destroy session immediately - worker thread may still be using it.
             * Session manager will clean it up on timeout or next connection from same client.
             * Just unlink from this connection. */
            conn->session = NULL;
         }

         /* Clear wsi reference */
         conn->wsi = NULL;

         /* Free any pending audio buffer */
         if (conn->audio_buffer) {
            free(conn->audio_buffer);
            conn->audio_buffer = NULL;
         }

         pthread_mutex_lock(&s_mutex);
         if (s_client_count > 0) {
            s_client_count--;
         }
         pthread_mutex_unlock(&s_mutex);

         LOG_INFO("WebUI: WebSocket client disconnected (total: %d)", s_client_count);
         break;
      }

      case LWS_CALLBACK_RECEIVE: {
         /* Message received from client */
         if (!conn->session) {
            LOG_WARNING("WebUI: Received message but no session");
            break;
         }

         session_touch(conn->session);

         int is_final = lws_is_final_fragment(wsi);
         int is_binary = lws_frame_is_binary(wsi);

         if (is_binary) {
#ifdef ENABLE_WEBUI_AUDIO
            /* Handle WebSocket frame fragmentation for binary messages */
            if (conn->in_binary_fragment) {
               /* Continuation of a fragmented message - append ALL bytes as payload */
               const uint8_t *data = (const uint8_t *)in;
               size_t data_len = len;

               if (conn->binary_msg_type == WS_BIN_AUDIO_IN && data_len > 0) {
                  /* Append continuation data to audio buffer */
                  if (conn->audio_buffer &&
                      conn->audio_buffer_len + data_len <= conn->audio_buffer_capacity) {
                     memcpy(conn->audio_buffer + conn->audio_buffer_len, data, data_len);
                     conn->audio_buffer_len += data_len;
                     LOG_INFO("WebUI: Fragment continuation, added %zu bytes (total: %zu)",
                              data_len, conn->audio_buffer_len);
                  }
               }

               /* Check if this is the final fragment */
               if (is_final) {
                  conn->in_binary_fragment = false;
               }
            } else {
               /* New message - parse type byte and handle */
               handle_binary_message(conn, (const uint8_t *)in, len);

               /* If not final, track that we're in a fragmented message */
               if (!is_final && len > 0) {
                  conn->in_binary_fragment = true;
                  conn->binary_msg_type = ((const uint8_t *)in)[0];
               }
            }
#else
            LOG_WARNING("WebUI: Audio not enabled, ignoring binary message (%zu bytes)", len);
#endif
         } else {
            /* Text message (JSON control) */
            handle_json_message(conn, (const char *)in, len);
         }
         break;
      }

      case LWS_CALLBACK_SERVER_WRITEABLE:
         /* Ready to send more data - process next queued response */
         process_one_response();
         break;

      case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
         /* lws_cancel_service() was called - process response queue */
         process_response_queue();
         break;

      default:
         break;
   }

   return 0;
}

/* =============================================================================
 * Protocol Definitions
 * ============================================================================= */

static struct lws_protocols s_protocols[] = {
   /* HTTP protocol (must be first) */
   {
       .name = "http",
       .callback = callback_http,
       .per_session_data_size = sizeof(struct http_session_data),
       .rx_buffer_size = 0,
   },
   /* WebSocket protocol */
   {
       .name = WEBUI_SUBPROTOCOL,
       .callback = callback_websocket,
       .per_session_data_size = sizeof(ws_connection_t),
       .rx_buffer_size = 8192, /* Match DAP packet size */
   },
   /* Terminator */
   { NULL, NULL, 0, 0 },
};

/* =============================================================================
 * Server Thread
 * ============================================================================= */

static void *webui_thread_func(void *arg) {
   (void)arg;

   LOG_INFO("WebUI: Server thread started");

   while (s_running) {
      /* Process events with 50ms timeout */
      lws_service(s_lws_context, 50);

      /* Process any pending responses from worker threads */
      process_response_queue();
   }

   LOG_INFO("WebUI: Server thread exiting");
   return NULL;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int webui_server_init(int port, const char *www_path) {
   struct lws_context_creation_info info;

   pthread_mutex_lock(&s_mutex);
   if (s_running) {
      pthread_mutex_unlock(&s_mutex);
      LOG_WARNING("WebUI: Server already running");
      return WEBUI_ERROR_ALREADY_RUNNING;
   }
   pthread_mutex_unlock(&s_mutex);

   /* Determine port */
   if (port <= 0) {
      port = g_config.webui.port;
      if (port <= 0) {
         port = WEBUI_DEFAULT_PORT;
      }
   }

   /* Determine www path */
   if (www_path && www_path[0] != '\0') {
      strncpy(s_www_path, www_path, sizeof(s_www_path) - 1);
   } else if (g_config.webui.www_path[0] != '\0') {
      strncpy(s_www_path, g_config.webui.www_path, sizeof(s_www_path) - 1);
   } else {
      strncpy(s_www_path, WEBUI_DEFAULT_WWW_PATH, sizeof(s_www_path) - 1);
   }
   s_www_path[sizeof(s_www_path) - 1] = '\0';

   /* Configure libwebsockets context */
   memset(&info, 0, sizeof(info));
   info.port = port;
   info.protocols = s_protocols;
   info.gid = -1;
   info.uid = -1;
   info.options = LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;

   /* Configure HTTPS if enabled */
   bool use_https = g_config.webui.https;
   if (use_https) {
      if (g_config.webui.ssl_cert_path[0] == '\0' || g_config.webui.ssl_key_path[0] == '\0') {
         LOG_ERROR("WebUI: HTTPS enabled but ssl_cert_path or ssl_key_path not set");
         return WEBUI_ERROR_SOCKET;
      }

      /* Verify certificate files exist */
      if (access(g_config.webui.ssl_cert_path, R_OK) != 0) {
         LOG_ERROR("WebUI: Cannot read SSL certificate: %s", g_config.webui.ssl_cert_path);
         return WEBUI_ERROR_SOCKET;
      }
      if (access(g_config.webui.ssl_key_path, R_OK) != 0) {
         LOG_ERROR("WebUI: Cannot read SSL private key: %s", g_config.webui.ssl_key_path);
         return WEBUI_ERROR_SOCKET;
      }

      info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
      info.ssl_cert_filepath = g_config.webui.ssl_cert_path;
      info.ssl_private_key_filepath = g_config.webui.ssl_key_path;

      LOG_INFO("WebUI: HTTPS enabled with cert: %s", g_config.webui.ssl_cert_path);
   }

   LOG_INFO("WebUI: Initializing %s server on port %d, serving from: %s",
            use_https ? "HTTPS" : "HTTP", port, s_www_path);

   /* Create context */
   s_lws_context = lws_create_context(&info);
   if (!s_lws_context) {
      LOG_ERROR("WebUI: Failed to create libwebsockets context");
      return WEBUI_ERROR_SOCKET;
   }

   s_port = port;
   s_running = 1;
   s_client_count = 0;

   /* Initialize audio subsystem (optional - continues if not available) */
#ifdef ENABLE_WEBUI_AUDIO
   if (webui_audio_init() != WEBUI_AUDIO_SUCCESS) {
      LOG_WARNING("WebUI: Audio subsystem not available, voice input disabled");
   }
#endif

   /* Start server thread */
   if (pthread_create(&s_webui_thread, NULL, webui_thread_func, NULL) != 0) {
      LOG_ERROR("WebUI: Failed to create server thread");
#ifdef ENABLE_WEBUI_AUDIO
      webui_audio_cleanup();
#endif
      lws_context_destroy(s_lws_context);
      s_lws_context = NULL;
      s_running = 0;
      return WEBUI_ERROR_THREAD;
   }

   LOG_INFO("WebUI: Server started successfully on port %d", port);
   return WEBUI_SUCCESS;
}

void webui_server_shutdown(void) {
   pthread_mutex_lock(&s_mutex);
   if (!s_running) {
      pthread_mutex_unlock(&s_mutex);
      return;
   }

   LOG_INFO("WebUI: Shutting down server...");
   s_running = 0;
   pthread_mutex_unlock(&s_mutex);

   /* Wake up lws_service() to process shutdown */
   if (s_lws_context) {
      lws_cancel_service(s_lws_context);
   }

   /* Wait for thread to exit */
   pthread_join(s_webui_thread, NULL);

   /* Destroy context */
   if (s_lws_context) {
      lws_context_destroy(s_lws_context);
      s_lws_context = NULL;
   }

   /* Cleanup audio subsystem */
#ifdef ENABLE_WEBUI_AUDIO
   webui_audio_cleanup();
#endif

   s_port = 0;
   s_client_count = 0;

   LOG_INFO("WebUI: Server shutdown complete");
}

bool webui_server_is_running(void) {
   bool running;
   pthread_mutex_lock(&s_mutex);
   running = s_running != 0;
   pthread_mutex_unlock(&s_mutex);
   return running;
}

int webui_server_client_count(void) {
   int count;
   pthread_mutex_lock(&s_mutex);
   count = s_client_count;
   pthread_mutex_unlock(&s_mutex);
   return count;
}

int webui_server_get_port(void) {
   return s_port;
}

/* =============================================================================
 * Worker-Callable Response Functions (Thread-Safe)
 * ============================================================================= */

void webui_send_transcript(session_t *session, const char *role, const char *text) {
   if (!session || session->type != SESSION_TYPE_WEBSOCKET) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_TRANSCRIPT,
                          .transcript = {
                              .role = strdup(role),
                              .text = strdup(text),
                          } };

   if (!resp.transcript.role || !resp.transcript.text) {
      free(resp.transcript.role);
      free(resp.transcript.text);
      LOG_ERROR("WebUI: Failed to allocate transcript response");
      return;
   }

   queue_response(&resp);
}

void webui_send_state(session_t *session, const char *state) {
   if (!session || session->type != SESSION_TYPE_WEBSOCKET) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_STATE,
                          .state = {
                              .state = strdup(state),
                          } };

   if (!resp.state.state) {
      LOG_ERROR("WebUI: Failed to allocate state response");
      return;
   }

   queue_response(&resp);
}

void webui_send_error(session_t *session, const char *code, const char *message) {
   if (!session || session->type != SESSION_TYPE_WEBSOCKET) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_ERROR,
                          .error = {
                              .code = strdup(code),
                              .message = strdup(message),
                          } };

   if (!resp.error.code || !resp.error.message) {
      free(resp.error.code);
      free(resp.error.message);
      LOG_ERROR("WebUI: Failed to allocate error response");
      return;
   }

   queue_response(&resp);
}

/**
 * @brief Queue audio data for sending to WebSocket client
 *
 * Copies the audio data to the response queue. The WebUI thread will
 * send it as binary WebSocket frames. Large audio is chunked to avoid
 * overwhelming libwebsockets with huge single writes.
 *
 * @param session WebSocket session
 * @param data PCM audio data
 * @param len Length of audio data
 */
#define AUDIO_CHUNK_SIZE \
   8192 /* 8KB chunks for WebSocket frames - keep small for lws compatibility */

static void webui_send_audio(session_t *session, const uint8_t *data, size_t len) {
   if (!session || session->type != SESSION_TYPE_WEBSOCKET || !data || len == 0) {
      return;
   }

   /* Send audio in chunks to avoid overwhelming lws_write with huge buffers */
   size_t offset = 0;
   int chunk_num = 0;

   while (offset < len) {
      size_t chunk_len = len - offset;
      if (chunk_len > AUDIO_CHUNK_SIZE) {
         chunk_len = AUDIO_CHUNK_SIZE;
      }

      uint8_t *chunk_copy = malloc(chunk_len);
      if (!chunk_copy) {
         LOG_ERROR("WebUI: Failed to allocate audio chunk buffer");
         return;
      }
      memcpy(chunk_copy, data + offset, chunk_len);

      ws_response_t resp = { .session = session,
                             .type = WS_RESP_AUDIO,
                             .audio = {
                                 .data = chunk_copy,
                                 .len = chunk_len,
                             } };

      queue_response(&resp);
      offset += chunk_len;
      chunk_num++;
   }

   LOG_INFO("WebUI: Queued %zu bytes audio in %d chunks", len, chunk_num);
}

/**
 * @brief Queue end-of-audio marker for WebSocket client
 *
 * @param session WebSocket session
 */
static void webui_send_audio_end(session_t *session) {
   if (!session || session->type != SESSION_TYPE_WEBSOCKET) {
      return;
   }

   ws_response_t resp = { .session = session, .type = WS_RESP_AUDIO_END };

   queue_response(&resp);
}

/* =============================================================================
 * Text Processing (Async Worker Thread)
 *
 * For Phase 2, we use a simple detached thread for text processing.
 * Phase 4 may integrate with the worker pool for audio + text.
 * ============================================================================= */

/* Command processing constants */
#define MAX_TOOL_RESULTS 8
#define TOOL_RESULT_MSG_SIZE 1024
#define WEBUI_WORKER_ID 100 /* Virtual worker ID for command router */

typedef struct {
   session_t *session;
   char *text;
} text_work_t;

/**
 * @brief Process commands in LLM response and make follow-up calls
 *
 * Searches for <command> tags in the response, executes them via MQTT,
 * and makes follow-up LLM calls with the results.
 *
 * @param llm_response The LLM response to process
 * @param session Session for follow-up LLM calls
 * @return Final response text (caller must free), or NULL if no commands
 */
static char *webui_process_commands(const char *llm_response, session_t *session) {
   if (!llm_response || !session) {
      return NULL;
   }

   struct mosquitto *mosq = worker_pool_get_mosq();
   if (!mosq) {
      LOG_WARNING("WebUI: No MQTT connection, cannot process commands");
      return NULL;
   }

   /* Collect all tool results */
   char *tool_results[MAX_TOOL_RESULTS];
   int num_results = 0;

   for (int i = 0; i < MAX_TOOL_RESULTS; i++) {
      tool_results[i] = NULL;
   }

   /* Search for <command> tags and process each one */
   const char *search_ptr = llm_response;
   const char *cmd_start;

   while ((cmd_start = strstr(search_ptr, "<command>")) != NULL && num_results < MAX_TOOL_RESULTS) {
      const char *cmd_end = strstr(cmd_start, "</command>");
      if (!cmd_end) {
         LOG_WARNING("WebUI: Unclosed <command> tag");
         break;
      }

      /* Extract command JSON */
      const char *json_start = cmd_start + strlen("<command>");
      size_t json_len = cmd_end - json_start;

      char *cmd_json = malloc(json_len + 1);
      if (!cmd_json) {
         LOG_ERROR("WebUI: Failed to allocate command JSON");
         break;
      }
      memcpy(cmd_json, json_start, json_len);
      cmd_json[json_len] = '\0';

      LOG_INFO("WebUI: Processing command: %s", cmd_json);

      /* Parse JSON to extract device/action */
      struct json_object *parsed_json = json_tokener_parse(cmd_json);
      if (!parsed_json) {
         LOG_WARNING("WebUI: Invalid command JSON: %s", cmd_json);
         free(cmd_json);
         search_ptr = cmd_end + strlen("</command>");
         continue;
      }

      /* Get device and action for result formatting */
      struct json_object *device_obj = NULL;
      struct json_object *action_obj = NULL;
      const char *device_name = "unknown";
      const char *action_name = "unknown";

      if (json_object_object_get_ex(parsed_json, "device", &device_obj)) {
         device_name = json_object_get_string(device_obj);
      }
      if (json_object_object_get_ex(parsed_json, "action", &action_obj)) {
         action_name = json_object_get_string(action_obj);
      }

      /* Register pending request */
      pending_request_t *req = command_router_register(WEBUI_WORKER_ID);
      if (!req) {
         LOG_ERROR("WebUI: Failed to register pending request");
         json_object_put(parsed_json);
         free(cmd_json);
         search_ptr = cmd_end + strlen("</command>");
         continue;
      }

      const char *request_id = command_router_get_id(req);
      LOG_INFO("WebUI: Registered request %s", request_id);

      /* Add request_id to command JSON */
      json_object_object_add(parsed_json, "request_id", json_object_new_string(request_id));
      const char *cmd_with_id = json_object_to_json_string(parsed_json);

      /* Publish command via MQTT */
      int rc = mosquitto_publish(mosq, NULL, APPLICATION_NAME, strlen(cmd_with_id), cmd_with_id, 0,
                                 false);
      if (rc != MOSQ_ERR_SUCCESS) {
         LOG_ERROR("WebUI: MQTT publish failed: %d", rc);
         command_router_cancel(req);
         json_object_put(parsed_json);
         free(cmd_json);
         search_ptr = cmd_end + strlen("</command>");
         continue;
      }
      LOG_INFO("WebUI: Published command to %s", APPLICATION_NAME);

      /* Wait for result */
      char *callback_result = command_router_wait(req, COMMAND_RESULT_TIMEOUT_MS);

      /* Format result for LLM */
      tool_results[num_results] = malloc(TOOL_RESULT_MSG_SIZE);
      if (tool_results[num_results]) {
         if (callback_result && strlen(callback_result) > 0) {
            LOG_INFO("WebUI: Received callback result: %.50s%s", callback_result,
                     strlen(callback_result) > 50 ? "..." : "");
            snprintf(tool_results[num_results], TOOL_RESULT_MSG_SIZE,
                     "[Tool Result: %s.%s returned: %s]", device_name, action_name,
                     callback_result);
         } else {
            LOG_WARNING("WebUI: No callback result (timeout or empty)");
            snprintf(tool_results[num_results], TOOL_RESULT_MSG_SIZE,
                     "[Tool Result: %s.%s completed successfully]", device_name, action_name);
         }

         /* Send tool result to WebUI for debug display */
         webui_send_transcript(session, "assistant", tool_results[num_results]);

         num_results++;
      }

      if (callback_result) {
         free(callback_result);
      }

      json_object_put(parsed_json);
      free(cmd_json);
      search_ptr = cmd_end + strlen("</command>");
   }

   /* If no results collected, return NULL (no commands processed) */
   if (num_results == 0) {
      return NULL;
   }

   /* Build combined tool results message for LLM */
   size_t total_len = 1; /* For null terminator */
   for (int i = 0; i < num_results; i++) {
      if (tool_results[i]) {
         total_len += strlen(tool_results[i]) + 1; /* +1 for newline */
      }
   }

   char *combined_results = malloc(total_len);
   if (!combined_results) {
      LOG_ERROR("WebUI: Failed to allocate combined results");
      for (int i = 0; i < num_results; i++) {
         free(tool_results[i]);
      }
      return NULL;
   }

   char *ptr = combined_results;
   for (int i = 0; i < num_results; i++) {
      if (tool_results[i]) {
         size_t len = strlen(tool_results[i]);
         memcpy(ptr, tool_results[i], len);
         ptr += len;
         if (i < num_results - 1) {
            *ptr++ = '\n';
         }
         free(tool_results[i]);
      }
   }
   *ptr = '\0';

   LOG_INFO("WebUI: Sending tool results to LLM: %s", combined_results);

   /* Make follow-up LLM call with tool results */
   char *final_response = session_llm_call(session, combined_results);

   free(combined_results);

   if (!final_response) {
      LOG_ERROR("WebUI: Follow-up LLM call failed");
      return NULL;
   }

   LOG_INFO("WebUI: LLM final response: %.50s%s", final_response,
            strlen(final_response) > 50 ? "..." : "");

   return final_response;
}

/**
 * @brief Strip command tags from response text
 *
 * Removes all <command>...</command> blocks from the text.
 *
 * @param text Text to modify in place
 */
static void strip_command_tags(char *text) {
   if (!text)
      return;

   char *cmd_start, *cmd_end;
   while ((cmd_start = strstr(text, "<command>")) != NULL) {
      cmd_end = strstr(cmd_start, "</command>");
      if (cmd_end) {
         cmd_end += strlen("</command>");
         memmove(cmd_start, cmd_end, strlen(cmd_end) + 1);
      } else {
         break;
      }
   }

   /* Also remove <end_of_turn> tags (local AI models) */
   char *match = strstr(text, "<end_of_turn>");
   if (match) {
      *match = '\0';
   }
}

static void *text_worker_thread(void *arg) {
   text_work_t *work = (text_work_t *)arg;
   session_t *session = work->session;
   char *text = work->text;

   /* Check if session is still valid */
   if (!session || session->disconnected) {
      LOG_INFO("WebUI: Session already disconnected, aborting text processing");
      if (session) {
         session_release(session);
      }
      free(text);
      free(work);
      return NULL;
   }

   LOG_INFO("WebUI: Processing text input for session %u: %s", session->session_id, text);

   /* Send "thinking" state */
   webui_send_state(session, "thinking");

   /* Echo user input as transcript */
   webui_send_transcript(session, "user", text);

   /* Call LLM with session's conversation history */
   char *response = session_llm_call(session, text);

   /* Check disconnection again - session may have been invalidated during LLM call */
   if (session->disconnected) {
      /* Client disconnected during processing - don't try to send response */
      LOG_INFO("WebUI: Session %u disconnected during LLM call", session->session_id);
      session_release(session);
      free(response);
      free(text);
      free(work);
      return NULL;
   }

   if (!response) {
      /* LLM call failed */
      webui_send_error(session, "LLM_ERROR", "Failed to get response from AI");
      webui_send_state(session, "idle");
      session_release(session);
      free(text);
      free(work);
      return NULL;
   }

   /* Check for command tags and process them */
   char *final_response = response;
   if (strstr(response, "<command>")) {
      LOG_INFO("WebUI: Response contains commands, processing...");

      /* Send intermediate response (with commands) to show tool calls */
      webui_send_transcript(session, "assistant", response);

      /* Process commands and get follow-up response */
      char *processed = webui_process_commands(response, session);
      if (processed) {
         /* Check for disconnection after command processing */
         if (session->disconnected) {
            LOG_INFO("WebUI: Session %u disconnected during command processing",
                     session->session_id);
            session_release(session);
            free(response);
            free(processed);
            free(text);
            free(work);
            return NULL;
         }

         /* Recursively process if the follow-up also contains commands */
         while (strstr(processed, "<command>") && !session->disconnected) {
            LOG_INFO("WebUI: Follow-up response contains more commands, processing...");
            webui_send_transcript(session, "assistant", processed);

            char *next_processed = webui_process_commands(processed, session);
            free(processed);
            if (!next_processed) {
               processed = NULL;
               break;
            }
            processed = next_processed;
         }

         if (processed) {
            free(response);
            final_response = processed;
         } else {
            /* Command processing failed, use original response */
            final_response = response;
         }
      }
   }

   /* Strip any remaining command tags before sending final response */
   strip_command_tags(final_response);

   /* Send final response if not empty */
   if (final_response && strlen(final_response) > 0) {
      webui_send_transcript(session, "assistant", final_response);
   }

   /* Free the final response (either original response or processed copy) */
   free(final_response);

   /* Return to idle state */
   webui_send_state(session, "idle");

   /* Release session reference (acquired in webui_process_text_input) */
   session_release(session);

   free(text);
   free(work);
   return NULL;
}

int webui_process_text_input(session_t *session, const char *text) {
   if (!session || !text || strlen(text) == 0) {
      return 1;
   }

   /* Create work item */
   text_work_t *work = malloc(sizeof(text_work_t));
   if (!work) {
      LOG_ERROR("WebUI: Failed to allocate text work item");
      return 1;
   }

   work->session = session;
   work->text = strdup(text);
   if (!work->text) {
      LOG_ERROR("WebUI: Failed to allocate text copy");
      free(work);
      return 1;
   }

   /* Retain session for worker thread (worker will release when done) */
   session_retain(session);

   /* Spawn detached worker thread */
   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

   int ret = pthread_create(&thread, &attr, text_worker_thread, work);
   pthread_attr_destroy(&attr);

   if (ret != 0) {
      LOG_ERROR("WebUI: Failed to create text worker thread");
      session_release(session);
      free(work->text);
      free(work);
      return 1;
   }

   return 0;
}

/* =============================================================================
 * JSON Message Handler Implementation
 * ============================================================================= */

static void handle_text_message(ws_connection_t *conn, const char *text, size_t len) {
   (void)len; /* Length already validated by caller */

   if (!conn->session) {
      LOG_WARNING("WebUI: Text message received but no session");
      return;
   }

   LOG_INFO("WebUI: Text input from session %u: %s", conn->session->session_id, text);

   int ret = webui_process_text_input(conn->session, text);
   if (ret != 0) {
      send_error_impl(conn->wsi, "PROCESSING_ERROR", "Failed to process text input");
   }
}

#ifdef ENABLE_WEBUI_AUDIO
/* =============================================================================
 * Audio Processing (Binary WebSocket Messages)
 *
 * Protocol:
 * - Client sends binary frames with 1-byte type prefix:
 *   - WS_BIN_AUDIO_IN (0x01): Opus audio chunk (length-prefixed frames)
 *   - WS_BIN_AUDIO_IN_END (0x02): End of utterance (triggers ASR + LLM + TTS)
 *
 * - Server responds with binary frames:
 *   - WS_BIN_AUDIO_OUT (0x11): Opus audio chunk for playback
 *   - WS_BIN_AUDIO_OUT_END (0x12): End of response audio
 * ============================================================================= */

typedef struct {
   session_t *session;
   uint8_t *audio_data;
   size_t audio_len;
} audio_work_t;

/**
 * @brief Audio worker thread - process voice input and generate response
 */
static void *audio_worker_thread(void *arg) {
   audio_work_t *work = (audio_work_t *)arg;
   session_t *session = work->session;
   uint8_t *audio_data = work->audio_data;
   size_t audio_len = work->audio_len;

   if (!session || session->disconnected) {
      LOG_INFO("WebUI: Audio session disconnected, aborting");
      if (session) {
         session_release(session);
      }
      free(audio_data);
      free(work);
      return NULL;
   }

   LOG_INFO("WebUI: Processing audio for session %u (%zu bytes)", session->session_id, audio_len);

   /* Send "listening" state (ASR in progress) */
   webui_send_state(session, "listening");

   /* Transcribe audio (input is raw PCM: 16-bit signed, 16kHz, mono) */
   char *transcript = NULL;
   size_t pcm_samples = audio_len / sizeof(int16_t);
   int ret = webui_audio_transcribe((const int16_t *)audio_data, pcm_samples, &transcript);
   free(audio_data);
   work->audio_data = NULL;

   if (ret != WEBUI_AUDIO_SUCCESS || !transcript || strlen(transcript) == 0) {
      LOG_WARNING("WebUI: Audio transcription failed or empty");
      webui_send_error(session, "ASR_FAILED", "Could not understand audio");
      webui_send_state(session, "idle");
      session_release(session);
      free(transcript);
      free(work);
      return NULL;
   }

   LOG_INFO("WebUI: Transcribed: \"%s\"", transcript);

   /* Check disconnection */
   if (session->disconnected) {
      session_release(session);
      free(transcript);
      free(work);
      return NULL;
   }

   /* Echo transcription as user message */
   webui_send_transcript(session, "user", transcript);

   /* Send "thinking" state (LLM processing) */
   webui_send_state(session, "thinking");

   /* Call LLM with conversation history */
   char *response = session_llm_call(session, transcript);
   free(transcript);

   if (!response || session->disconnected) {
      LOG_WARNING("WebUI: LLM call failed or session disconnected");
      if (!session->disconnected) {
         webui_send_error(session, "LLM_ERROR", "Failed to get response");
      }
      webui_send_state(session, "idle");
      session_release(session);
      free(response);
      free(work);
      return NULL;
   }

   /* Process commands if present (like text processing) */
   char *final_response = response;
   if (strstr(response, "<command>")) {
      LOG_INFO("WebUI: Audio response contains commands");
      webui_send_transcript(session, "assistant", response);

      /* Extract and speak preamble text before first <command> tag */
      char *cmd_start = strstr(response, "<command>");
      if (cmd_start > response) {
         size_t preamble_len = cmd_start - response;
         char *preamble = malloc(preamble_len + 1);
         if (preamble) {
            memcpy(preamble, response, preamble_len);
            preamble[preamble_len] = '\0';

            /* Trim trailing whitespace */
            while (preamble_len > 0 &&
                   (preamble[preamble_len - 1] == ' ' || preamble[preamble_len - 1] == '\n' ||
                    preamble[preamble_len - 1] == '\r' || preamble[preamble_len - 1] == '\t')) {
               preamble[--preamble_len] = '\0';
            }

            /* Speak preamble if it has meaningful content (more than just whitespace) */
            if (preamble_len > 0) {
               LOG_INFO("WebUI: Speaking preamble before command: %s", preamble);
               webui_send_state(session, "speaking");

               int16_t *preamble_pcm = NULL;
               size_t preamble_samples = 0;
               int pret = webui_audio_text_to_pcm16k(preamble, &preamble_pcm, &preamble_samples);
               if (pret == WEBUI_AUDIO_SUCCESS && preamble_pcm && preamble_samples > 0) {
                  size_t preamble_bytes = preamble_samples * sizeof(int16_t);
                  webui_send_audio(session, (const uint8_t *)preamble_pcm, preamble_bytes);
                  webui_send_audio_end(session);
                  free(preamble_pcm);
               }

               webui_send_state(session, "processing");
            }
            free(preamble);
         }
      }

      char *processed = webui_process_commands(response, session);
      if (processed && !session->disconnected) {
         free(response);
         final_response = processed;
      }
   }

   /* Strip command tags */
   strip_command_tags(final_response);

   if (!final_response || strlen(final_response) == 0 || session->disconnected) {
      webui_send_state(session, "idle");
      session_release(session);
      free(final_response);
      free(work);
      return NULL;
   }

   /* Send text response */
   webui_send_transcript(session, "assistant", final_response);

   /* Generate TTS audio as raw PCM (browser can play directly without decoder) */
   webui_send_state(session, "speaking");

   int16_t *tts_pcm = NULL;
   size_t tts_samples = 0;
   ret = webui_audio_text_to_pcm16k(final_response, &tts_pcm, &tts_samples);
   free(final_response);

   if (ret != WEBUI_AUDIO_SUCCESS || !tts_pcm || tts_samples == 0) {
      LOG_WARNING("WebUI: TTS synthesis failed");
      webui_send_state(session, "idle");
      session_release(session);
      free(tts_pcm);
      free(work);
      return NULL;
   }

   /* Send audio response via queue (worker thread can't call lws_write directly) */
   size_t tts_bytes = tts_samples * sizeof(int16_t);
   LOG_INFO("WebUI: Sending %zu bytes PCM audio (%zu samples) to client", tts_bytes, tts_samples);
   webui_send_audio(session, (const uint8_t *)tts_pcm, tts_bytes);
   webui_send_audio_end(session);
   free(tts_pcm);

   webui_send_state(session, "idle");

   /* Release session reference (acquired before thread creation) */
   session_release(session);

   free(work);
   return NULL;
}

/**
 * @brief Handle binary WebSocket message (audio data)
 *
 * Binary message format:
 * - Byte 0: Message type (WS_BIN_AUDIO_IN or WS_BIN_AUDIO_IN_END)
 * - Bytes 1+: Opus audio data (for AUDIO_IN) or empty (for AUDIO_IN_END)
 */
static void handle_binary_message(ws_connection_t *conn, const uint8_t *data, size_t len) {
   if (len < 1) {
      LOG_WARNING("WebUI: Empty binary message");
      return;
   }

   if (!conn->session) {
      LOG_WARNING("WebUI: Binary message but no session");
      return;
   }

   uint8_t msg_type = data[0];
   const uint8_t *payload = data + 1;
   size_t payload_len = len - 1;

   switch (msg_type) {
      case WS_BIN_AUDIO_IN: {
         /* Accumulate audio data in connection buffer */
         if (payload_len == 0) {
            break;
         }

         /* Initialize buffer if needed */
         if (!conn->audio_buffer) {
            conn->audio_buffer_capacity = WEBUI_AUDIO_BUFFER_SIZE;
            conn->audio_buffer = malloc(conn->audio_buffer_capacity);
            if (!conn->audio_buffer) {
               LOG_ERROR("WebUI: Failed to allocate audio buffer");
               send_error_impl(conn->wsi, "BUFFER_ERROR", "Audio buffer allocation failed");
               return;
            }
            conn->audio_buffer_len = 0;
         }

         /* Check if we have room */
         if (conn->audio_buffer_len + payload_len > conn->audio_buffer_capacity) {
            /* Expand buffer */
            size_t new_capacity = conn->audio_buffer_capacity * 2;

            /* Enforce maximum capacity to prevent OOM from malicious/long recordings */
            if (new_capacity > WEBUI_AUDIO_MAX_CAPACITY) {
               LOG_WARNING("WebUI: Audio buffer would exceed max capacity (%d bytes)",
                           WEBUI_AUDIO_MAX_CAPACITY);
               send_error_impl(conn->wsi, "BUFFER_FULL", "Recording too long");
               return;
            }

            uint8_t *new_buffer = realloc(conn->audio_buffer, new_capacity);
            if (!new_buffer) {
               LOG_ERROR("WebUI: Failed to expand audio buffer");
               send_error_impl(conn->wsi, "BUFFER_ERROR", "Audio buffer allocation failed");
               return;
            }
            conn->audio_buffer = new_buffer;
            conn->audio_buffer_capacity = new_capacity;
         }

         /* Append audio data */
         memcpy(conn->audio_buffer + conn->audio_buffer_len, payload, payload_len);
         conn->audio_buffer_len += payload_len;

         LOG_INFO("WebUI: Accumulated %zu bytes audio (total: %zu)", payload_len,
                  conn->audio_buffer_len);
         break;
      }

      case WS_BIN_AUDIO_IN_END: {
         /* End of utterance - process accumulated audio */
         if (!conn->audio_buffer || conn->audio_buffer_len == 0) {
            LOG_WARNING("WebUI: AUDIO_IN_END but no audio accumulated");
            break;
         }

         LOG_INFO("WebUI: Audio end, processing %zu bytes", conn->audio_buffer_len);

         /* Create work item for worker thread */
         audio_work_t *work = malloc(sizeof(audio_work_t));
         if (!work) {
            LOG_ERROR("WebUI: Failed to allocate audio work");
            free(conn->audio_buffer);
            conn->audio_buffer = NULL;
            conn->audio_buffer_len = 0;
            send_error_impl(conn->wsi, "PROCESSING_ERROR", "Audio processing failed");
            return;
         }

         work->session = conn->session;
         work->audio_data = conn->audio_buffer;
         work->audio_len = conn->audio_buffer_len;

         /* Transfer ownership to worker */
         conn->audio_buffer = NULL;
         conn->audio_buffer_len = 0;

         /* Retain session for worker thread (worker will release when done) */
         session_retain(conn->session);

         /* Spawn worker thread */
         pthread_t thread;
         pthread_attr_t attr;
         pthread_attr_init(&attr);
         pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

         int ret = pthread_create(&thread, &attr, audio_worker_thread, work);
         pthread_attr_destroy(&attr);

         if (ret != 0) {
            LOG_ERROR("WebUI: Failed to create audio worker thread");
            session_release(conn->session);
            free(work->audio_data);
            free(work);
            send_error_impl(conn->wsi, "PROCESSING_ERROR", "Audio processing failed");
         }
         break;
      }

      default:
         LOG_WARNING("WebUI: Unknown binary message type: 0x%02x", msg_type);
         break;
   }
}
#endif /* ENABLE_WEBUI_AUDIO */
