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

#define _GNU_SOURCE /* For strcasestr */

#include "webui/webui_server.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <json-c/json.h>
#include <libwebsockets.h>
#include <limits.h>
#include <mosquitto.h>
#include <net/if.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#ifdef ENABLE_WEBUI_AUDIO
#include "webui/webui_audio.h"
#endif

#include "config/config_env.h"
#include "config/config_parser.h"
#include "config/dawn_config.h"
#include "core/command_router.h"
#include "core/ocp_helpers.h"
#include "core/session_manager.h"
#include "core/worker_pool.h"
#include "dawn.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_context.h"
#include "llm/llm_interface.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "state_machine.h"
#include "tools/smartthings_service.h"
#include "tools/string_utils.h"
#include "ui/metrics.h"
#include "version.h"

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

/* Config modification mutex - protects against concurrent config reads during writes */
static pthread_rwlock_t s_config_rwlock = PTHREAD_RWLOCK_INITIALIZER;

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
      struct {
         int current_tokens;
         int max_tokens;
         float threshold;
      } context;
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
 * Model/Interface Cache (avoids repeated filesystem/network scans)
 * ============================================================================= */

#define MODEL_CACHE_TTL 60 /* Cache refresh interval in seconds */

typedef struct {
   json_object *models_response;     /* Cached list_models_response */
   json_object *interfaces_response; /* Cached list_interfaces_response */
   time_t models_cache_time;         /* When models were last scanned */
   time_t interfaces_cache_time;     /* When interfaces were last enumerated */
   pthread_mutex_t cache_mutex;      /* Protects cache access */
} discovery_cache_t;

static discovery_cache_t s_discovery_cache = { .models_response = NULL,
                                               .interfaces_response = NULL,
                                               .models_cache_time = 0,
                                               .interfaces_cache_time = 0,
                                               .cache_mutex = PTHREAD_MUTEX_INITIALIZER };

/* =============================================================================
 * Allowed Path Prefixes for Model Directory Scanning
 *
 * Security: Restricts which directories can be scanned for models.
 * The current working directory is always allowed in addition to these.
 * Modify this list to allow/disallow specific paths.
 * ============================================================================= */

static const char *s_allowed_path_prefixes[] = {
   "/home/",      "/var/lib/", "/opt/", "/usr/local/share/",
   "/usr/share/", NULL /* Sentinel - must be last */
};

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

/**
 * @brief Check if a path contains directory traversal patterns
 *
 * Checks for literal ".." as well as URL-encoded variants (%2e, %252e)
 * to prevent path traversal attacks.
 *
 * @param path The URL path to check
 * @return true if traversal pattern detected, false if path is safe
 */
static bool contains_path_traversal(const char *path) {
   if (!path) {
      return false;
   }

   /* Check for literal ".." */
   if (strstr(path, "..") != NULL) {
      return true;
   }

   /* Check for URL-encoded variants (case-insensitive) */
   /* %2e = ".", so %2e%2e = ".." */
   if (strcasestr(path, "%2e%2e") != NULL) {
      return true;
   }

   /* Single encoded dot followed by literal dot or vice versa */
   if (strcasestr(path, "%2e.") != NULL || strcasestr(path, ".%2e") != NULL) {
      return true;
   }

   /* Double-encoded: %252e = "%2e" after first decode */
   if (strcasestr(path, "%252e") != NULL) {
      return true;
   }

   return false;
}

/**
 * @brief Validate that a resolved path is within the allowed directory
 *
 * Uses realpath() to resolve symlinks and relative paths, then verifies
 * the canonical path is within the www directory.
 *
 * @param filepath The filesystem path to validate
 * @param www_path The allowed base directory (www path)
 * @return true if path is safe (within www_path), false otherwise
 */
static bool is_path_within_www(const char *filepath, const char *www_path) {
   char resolved_path[PATH_MAX];
   char resolved_www[PATH_MAX];

   /* Resolve the www base path */
   if (realpath(www_path, resolved_www) == NULL) {
      LOG_ERROR("WebUI: Cannot resolve www path: %s", www_path);
      return false;
   }

   /* Resolve the requested file path */
   if (realpath(filepath, resolved_path) == NULL) {
      /* File doesn't exist - check parent directory instead */
      /* This allows serving files that don't exist yet (404 handled elsewhere) */
      char *filepath_copy = strdup(filepath);
      if (!filepath_copy) {
         return false;
      }

      /* Find last slash to get parent directory */
      char *last_slash = strrchr(filepath_copy, '/');
      if (last_slash && last_slash != filepath_copy) {
         *last_slash = '\0';
         if (realpath(filepath_copy, resolved_path) == NULL) {
            free(filepath_copy);
            return false;
         }
      } else {
         free(filepath_copy);
         return false;
      }
      free(filepath_copy);
   }

   /* Ensure resolved path starts with resolved www path */
   size_t www_len = strlen(resolved_www);
   if (strncmp(resolved_path, resolved_www, www_len) != 0) {
      return false;
   }

   /* Ensure it's either exact match or followed by '/' */
   if (resolved_path[www_len] != '\0' && resolved_path[www_len] != '/') {
      return false;
   }

   return true;
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

/**
 * @brief Generate a cryptographically secure session token
 *
 * @param token_out Buffer to store the hex-encoded token (must be WEBUI_SESSION_TOKEN_LEN)
 * @return 0 on success, 1 on failure (token_out will be empty string)
 */
static int generate_session_token(char token_out[WEBUI_SESSION_TOKEN_LEN]) {
   uint8_t random_bytes[16];
   if (getrandom(random_bytes, 16, 0) != 16) {
      /* Security: fail instead of using weak random - getrandom should never fail on modern Linux
       */
      LOG_ERROR("getrandom() failed - cannot generate secure session token");
      token_out[0] = '\0';
      return 1;
   }
   for (int i = 0; i < 16; i++) {
      snprintf(&token_out[i * 2], 3, "%02x", random_bytes[i]);
   }
   token_out[32] = '\0';
   return 0;
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
      case WS_RESP_CONTEXT:
         /* No data to free - all inline values */
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

static void send_context_impl(struct lws *wsi,
                              int current_tokens,
                              int max_tokens,
                              float threshold) {
   char json[256];
   float usage_pct = (max_tokens > 0) ? (float)current_tokens / (float)max_tokens * 100.0f : 0;
   snprintf(json, sizeof(json),
            "{\"type\":\"context\",\"payload\":{\"current\":%d,\"max\":%d,\"usage\":%.1f,"
            "\"threshold\":%.0f}}",
            current_tokens, max_tokens, usage_pct, threshold * 100.0f);
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
      case WS_RESP_CONTEXT:
         send_context_impl(conn->wsi, resp.context.current_tokens, resp.context.max_tokens,
                           resp.context.threshold);
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

         /* SmartThings OAuth callback - extract code and state from URL */
         if (strncmp(path, "/smartthings/callback", 21) == 0) {
            /* Generate inline HTML that extracts params and posts to opener */
            static const char oauth_callback_html[] =
                "<!DOCTYPE html><html><head><title>SmartThings Auth</title></head>"
                "<body><script>"
                "const params = new URLSearchParams(window.location.search);"
                "const code = params.get('code');"
                "const state = params.get('state');"
                "const error = params.get('error');"
                "if (window.opener) {"
                "  window.opener.postMessage({"
                "    type: 'smartthings_oauth_callback',"
                "    code: code,"
                "    state: state,"
                "    error: error"
                "  }, window.location.origin);"
                "  setTimeout(function() { window.close(); }, 500);"
                "} else {"
                "  document.body.innerHTML = '<p>Authorization ' + "
                "    (code ? 'successful' : 'failed') + '. You can close this window.</p>';"
                "}"
                "</script><p>Processing authorization...</p></body></html>";

            unsigned char buffer[LWS_PRE + sizeof(oauth_callback_html)];
            unsigned char *start = &buffer[LWS_PRE];
            unsigned char *p = start;
            unsigned char *end = &buffer[sizeof(buffer) - 1];

            if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end))
               return -1;
            if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                             (unsigned char *)"text/html", 9, &p, end))
               return -1;
            if (lws_add_http_header_content_length(wsi, sizeof(oauth_callback_html) - 1, &p, end))
               return -1;
            if (lws_finalize_http_header(wsi, &p, end))
               return -1;

            n = lws_write(wsi, start, p - start, LWS_WRITE_HTTP_HEADERS);
            if (n < 0)
               return -1;

            n = lws_write(wsi, (unsigned char *)oauth_callback_html,
                          sizeof(oauth_callback_html) - 1, LWS_WRITE_HTTP);
            if (n < 0)
               return -1;

            return -1; /* Close connection after response */
         }

         /* Health check endpoint - returns JSON status */
         if (strcmp(path, "/health") == 0) {
            dawn_metrics_t snapshot;
            metrics_get_snapshot(&snapshot);

            /* Build JSON response */
            char json_body[512];
            snprintf(json_body, sizeof(json_body),
                     "{\"status\":\"ok\",\"version\":\"%s\",\"git_sha\":\"%s\","
                     "\"uptime_seconds\":%ld,\"state\":\"%s\",\"queries\":%u,"
                     "\"active_sessions\":%d}",
                     VERSION_NUMBER, GIT_SHA, (long)metrics_get_uptime(),
                     dawn_state_name(snapshot.current_state), snapshot.queries_total,
                     s_client_count);

            size_t body_len = strlen(json_body);
            unsigned char buffer[LWS_PRE + 512];
            unsigned char *start = &buffer[LWS_PRE];
            unsigned char *p = start;
            unsigned char *end = &buffer[sizeof(buffer) - 1];

            if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end))
               return -1;
            if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                             (unsigned char *)"application/json", 16, &p, end))
               return -1;
            if (lws_add_http_header_content_length(wsi, body_len, &p, end))
               return -1;
            if (lws_finalize_http_header(wsi, &p, end))
               return -1;

            n = lws_write(wsi, start, p - start, LWS_WRITE_HTTP_HEADERS);
            if (n < 0)
               return -1;

            n = lws_write(wsi, (unsigned char *)json_body, body_len, LWS_WRITE_HTTP);
            if (n < 0)
               return -1;

            return -1; /* Close connection after response */
         }

         /* Default to index.html for root */
         if (strcmp(path, "/") == 0) {
            strncpy(path, "/index.html", sizeof(path) - 1);
         }

         /* Prevent directory traversal - check for patterns including URL-encoded */
         if (contains_path_traversal(path)) {
            LOG_WARNING("WebUI: Directory traversal attempt blocked: %s", path);
            lws_return_http_status(wsi, HTTP_STATUS_FORBIDDEN, NULL);
            return -1;
         }

         /* Build full filesystem path */
         snprintf(filepath, sizeof(filepath), "%s%s", s_www_path, path);

         /* Second layer: verify resolved path is within www directory */
         if (!is_path_within_www(filepath, s_www_path)) {
            LOG_WARNING("WebUI: Path escape attempt blocked: %s", filepath);
            lws_return_http_status(wsi, HTTP_STATUS_FORBIDDEN, NULL);
            return -1;
         }

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
static void handle_get_config(ws_connection_t *conn);
static void handle_set_config(ws_connection_t *conn, struct json_object *payload);
static void handle_set_secrets(ws_connection_t *conn, struct json_object *payload);
static void handle_get_audio_devices(ws_connection_t *conn, struct json_object *payload);
static void handle_list_models(ws_connection_t *conn);
static void send_json_response(struct lws *wsi, json_object *response);
static void handle_list_interfaces(ws_connection_t *conn);
static void handle_get_tools_config(ws_connection_t *conn);
static void handle_set_tools_config(ws_connection_t *conn, struct json_object *payload);
static void handle_get_metrics(ws_connection_t *conn);
#ifdef ENABLE_WEBUI_AUDIO
static void handle_binary_message(ws_connection_t *conn, const uint8_t *data, size_t len);
#endif

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

static void handle_get_config(ws_connection_t *conn) {
   /* Build response with config, secrets status, and metadata */
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("get_config_response"));

   json_object *payload = json_object_new_object();

   /* Add config path */
   const char *config_path = config_get_loaded_path();
   json_object_object_add(payload, "config_path", json_object_new_string(config_path));

   /* Add secrets path */
   const char *secrets_path = config_get_secrets_path();
   json_object_object_add(payload, "secrets_path", json_object_new_string(secrets_path));

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

   /* Get session's resolved LLM config */
   session_llm_config_t session_config = { 0 };
   llm_resolved_config_t resolved = { 0 };
   if (conn->session) {
      session_get_llm_config(conn->session, &session_config);
   }
   llm_resolve_config(&session_config, &resolved);

   json_object_object_add(llm_runtime, "type",
                          json_object_new_string(resolved.type == LLM_LOCAL ? "local" : "cloud"));

   const char *provider_name = resolved.cloud_provider == CLOUD_PROVIDER_OPENAI   ? "OpenAI"
                               : resolved.cloud_provider == CLOUD_PROVIDER_CLAUDE ? "Claude"
                                                                                  : "None";
   json_object_object_add(llm_runtime, "provider", json_object_new_string(provider_name));
   json_object_object_add(llm_runtime, "model",
                          json_object_new_string(resolved.model ? resolved.model : ""));
   json_object_object_add(llm_runtime, "openai_available",
                          json_object_new_boolean(llm_has_openai_key()));
   json_object_object_add(llm_runtime, "claude_available",
                          json_object_new_boolean(llm_has_claude_key()));
   json_object_object_add(payload, "llm_runtime", llm_runtime);

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
         JSON_TO_CONFIG_STR(cloud, "openai_model", config->llm.cloud.openai_model);
         JSON_TO_CONFIG_STR(cloud, "claude_model", config->llm.cloud.claude_model);
         JSON_TO_CONFIG_STR(cloud, "endpoint", config->llm.cloud.endpoint);
         JSON_TO_CONFIG_BOOL(cloud, "vision_enabled", config->llm.cloud.vision_enabled);
      }

      struct json_object *local;
      if (json_object_object_get_ex(section, "local", &local)) {
         JSON_TO_CONFIG_STR(local, "endpoint", config->llm.local.endpoint);
         JSON_TO_CONFIG_STR(local, "model", config->llm.local.model);
         JSON_TO_CONFIG_BOOL(local, "vision_enabled", config->llm.local.vision_enabled);
      }

      struct json_object *tools;
      if (json_object_object_get_ex(section, "tools", &tools)) {
         JSON_TO_CONFIG_BOOL(tools, "native_enabled", config->llm.tools.native_enabled);
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
      JSON_TO_CONFIG_STR(section, "music_dir", config->paths.music_dir);
      JSON_TO_CONFIG_STR(section, "commands_config", config->paths.commands_config);
   }
}

static void handle_set_config(ws_connection_t *conn, struct json_object *payload) {
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

   /* Apply changes to global config with mutex protection.
    * The write lock ensures no other threads are reading config during modification. */
   pthread_rwlock_wrlock(&s_config_rwlock);
   dawn_config_t *mutable_config = (dawn_config_t *)config_get();
   apply_config_from_json(mutable_config, payload);
   pthread_rwlock_unlock(&s_config_rwlock);

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
            }
            if (rc != 0) {
               json_object_object_add(resp_payload, "warning",
                                      json_object_new_string(
                                          "Config saved but failed to switch cloud provider - "
                                          "API key not configured"));
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

static void handle_set_secrets(ws_connection_t *conn, struct json_object *payload) {
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

/**
 * @brief Send a JSON response efficiently using stack buffer when possible
 *
 * Uses stack allocation for small responses (<2KB) to avoid heap fragmentation.
 * Falls back to heap allocation for larger responses.
 */
#define MAX_STACK_RESPONSE 2048

static void send_json_response(struct lws *wsi, json_object *response) {
   const char *json_str = json_object_to_json_string(response);
   size_t json_len = strlen(json_str);

   if (json_len < MAX_STACK_RESPONSE - LWS_PRE) {
      /* Use stack buffer for small responses */
      unsigned char buf[LWS_PRE + MAX_STACK_RESPONSE];
      memcpy(buf + LWS_PRE, json_str, json_len);
      lws_write(wsi, buf + LWS_PRE, json_len, LWS_WRITE_TEXT);
   } else {
      /* Fall back to heap for large responses */
      unsigned char *buf = malloc(LWS_PRE + json_len);
      if (buf) {
         memcpy(buf + LWS_PRE, json_str, json_len);
         lws_write(wsi, buf + LWS_PRE, json_len, LWS_WRITE_TEXT);
         free(buf);
      } else {
         LOG_ERROR("WebUI: Failed to allocate buffer for JSON response (%zu bytes)", json_len);
      }
   }
}

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

static void handle_get_audio_devices(ws_connection_t *conn, struct json_object *payload) {
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
static void handle_list_models(ws_connection_t *conn) {
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
static void handle_list_interfaces(ws_connection_t *conn) {
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
 * Tool Configuration Handlers
 * ============================================================================= */

static void handle_get_tools_config(ws_connection_t *conn) {
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("get_tools_config_response"));

   json_object *payload = json_object_new_object();
   json_object *tools_array = json_object_new_array();

   /* Get all tools with their enable states */
   tool_info_t tools[LLM_TOOLS_MAX_TOOLS];
   int count = llm_tools_get_all(tools, LLM_TOOLS_MAX_TOOLS);

   for (int i = 0; i < count; i++) {
      json_object *tool_obj = json_object_new_object();
      json_object_object_add(tool_obj, "name", json_object_new_string(tools[i].name));
      json_object_object_add(tool_obj, "description", json_object_new_string(tools[i].description));
      json_object_object_add(tool_obj, "available", json_object_new_boolean(tools[i].enabled));
      json_object_object_add(tool_obj, "local", json_object_new_boolean(tools[i].enabled_local));
      json_object_object_add(tool_obj, "remote", json_object_new_boolean(tools[i].enabled_remote));
      json_object_object_add(tool_obj, "armor_feature",
                             json_object_new_boolean(tools[i].armor_feature));
      json_object_array_add(tools_array, tool_obj);
   }

   json_object_object_add(payload, "tools", tools_array);

   /* Add token estimates */
   json_object *estimates = json_object_new_object();
   json_object_object_add(estimates, "local",
                          json_object_new_int(llm_tools_estimate_tokens(false)));
   json_object_object_add(estimates, "remote",
                          json_object_new_int(llm_tools_estimate_tokens(true)));
   json_object_object_add(payload, "token_estimate", estimates);

   json_object_object_add(response, "payload", payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);

   LOG_INFO("WebUI: Sent tools config (%d tools)", count);
}

/* =============================================================================
 * Metrics Handler
 * ============================================================================= */

static void handle_get_metrics(ws_connection_t *conn) {
   dawn_metrics_t snapshot;
   metrics_get_snapshot(&snapshot);

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("get_metrics_response"));

   json_object *payload = json_object_new_object();

   /* Session statistics */
   json_object *session = json_object_new_object();
   json_object_object_add(session, "uptime_seconds", json_object_new_int64(metrics_get_uptime()));
   json_object_object_add(session, "queries_total", json_object_new_int(snapshot.queries_total));
   json_object_object_add(session, "queries_cloud", json_object_new_int(snapshot.queries_cloud));
   json_object_object_add(session, "queries_local", json_object_new_int(snapshot.queries_local));
   json_object_object_add(session, "errors", json_object_new_int(snapshot.errors_count));
   json_object_object_add(session, "fallbacks", json_object_new_int(snapshot.fallbacks_count));
   json_object_object_add(session, "bargeins", json_object_new_int(snapshot.bargein_count));
   json_object_object_add(payload, "session", session);

   /* Token usage */
   json_object *tokens = json_object_new_object();
   json_object_object_add(tokens, "cloud_input",
                          json_object_new_int64(snapshot.tokens_cloud_input));
   json_object_object_add(tokens, "cloud_output",
                          json_object_new_int64(snapshot.tokens_cloud_output));
   json_object_object_add(tokens, "local_input",
                          json_object_new_int64(snapshot.tokens_local_input));
   json_object_object_add(tokens, "local_output",
                          json_object_new_int64(snapshot.tokens_local_output));
   json_object_object_add(tokens, "cached", json_object_new_int64(snapshot.tokens_cached));
   json_object_object_add(payload, "tokens", tokens);

   /* Last query timing */
   json_object *last = json_object_new_object();
   json_object_object_add(last, "vad_ms", json_object_new_double(snapshot.last_vad_time_ms));
   json_object_object_add(last, "asr_ms", json_object_new_double(snapshot.last_asr_time_ms));
   json_object_object_add(last, "asr_rtf", json_object_new_double(snapshot.last_asr_rtf));
   json_object_object_add(last, "llm_ttft_ms", json_object_new_double(snapshot.last_llm_ttft_ms));
   json_object_object_add(last, "llm_total_ms", json_object_new_double(snapshot.last_llm_total_ms));
   json_object_object_add(last, "tts_ms", json_object_new_double(snapshot.last_tts_time_ms));
   json_object_object_add(last, "pipeline_ms",
                          json_object_new_double(snapshot.last_total_pipeline_ms));
   json_object_object_add(payload, "last", last);

   /* Average timing */
   json_object *avg = json_object_new_object();
   json_object_object_add(avg, "vad_ms", json_object_new_double(snapshot.avg_vad_ms));
   json_object_object_add(avg, "asr_ms", json_object_new_double(snapshot.avg_asr_ms));
   json_object_object_add(avg, "asr_rtf", json_object_new_double(snapshot.avg_asr_rtf));
   json_object_object_add(avg, "llm_ttft_ms", json_object_new_double(snapshot.avg_llm_ttft_ms));
   json_object_object_add(avg, "llm_total_ms", json_object_new_double(snapshot.avg_llm_total_ms));
   json_object_object_add(avg, "tts_ms", json_object_new_double(snapshot.avg_tts_ms));
   json_object_object_add(avg, "pipeline_ms",
                          json_object_new_double(snapshot.avg_total_pipeline_ms));
   json_object_object_add(payload, "averages", avg);

   /* Real-time state */
   json_object *state = json_object_new_object();
   json_object_object_add(state, "current",
                          json_object_new_string(dawn_state_name(snapshot.current_state)));
   json_object_object_add(state, "vad_probability",
                          json_object_new_double(snapshot.current_vad_probability));
   json_object_object_add(state, "audio_buffer_fill",
                          json_object_new_double(snapshot.audio_buffer_fill_pct));
   json_object_object_add(payload, "state", state);

   /* AEC status */
   json_object *aec = json_object_new_object();
   json_object_object_add(aec, "enabled", json_object_new_boolean(snapshot.aec_enabled));
   json_object_object_add(aec, "calibrated", json_object_new_boolean(snapshot.aec_calibrated));
   json_object_object_add(aec, "delay_ms", json_object_new_int(snapshot.aec_delay_ms));
   json_object_object_add(aec, "correlation", json_object_new_double(snapshot.aec_correlation));
   json_object_object_add(payload, "aec", aec);

   /* Summarizer stats */
   json_object *summarizer = json_object_new_object();
   json_object_object_add(summarizer, "backend",
                          json_object_new_string(snapshot.summarizer_backend));
   json_object_object_add(summarizer, "threshold",
                          json_object_new_int64(snapshot.summarizer_threshold));
   json_object_object_add(summarizer, "calls", json_object_new_int(snapshot.summarizer_call_count));
   json_object_object_add(payload, "summarizer", summarizer);

   json_object_object_add(response, "payload", payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Validate a tool name for safe processing.
 *
 * Tool names must be non-empty, under LLM_TOOL_NAME_MAX length,
 * and contain only alphanumeric characters, underscores, and hyphens.
 *
 * @param name The tool name to validate
 * @return true if valid, false otherwise
 */
static bool is_valid_tool_name(const char *name) {
   if (!name || name[0] == '\0') {
      return false;
   }

   size_t len = strlen(name);
   if (len >= LLM_TOOL_NAME_MAX) {
      return false;
   }

   for (size_t i = 0; i < len; i++) {
      char c = name[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-')) {
         return false;
      }
   }

   return true;
}

static void handle_set_tools_config(ws_connection_t *conn, struct json_object *payload) {
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("set_tools_config_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *tools_array;
   if (!json_object_object_get_ex(payload, "tools", &tools_array)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing 'tools' array"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   int updated = 0;
   int skipped = 0;
   int len = json_object_array_length(tools_array);
   for (int i = 0; i < len; i++) {
      json_object *tool_obj = json_object_array_get_idx(tools_array, i);

      json_object *name_obj, *local_obj, *remote_obj;
      if (json_object_object_get_ex(tool_obj, "name", &name_obj) &&
          json_object_object_get_ex(tool_obj, "local", &local_obj) &&
          json_object_object_get_ex(tool_obj, "remote", &remote_obj)) {
         const char *name = json_object_get_string(name_obj);

         /* Validate tool name before processing */
         if (!is_valid_tool_name(name)) {
            LOG_WARNING("WebUI: Skipping invalid tool name: '%s'", name ? name : "(null)");
            skipped++;
            continue;
         }

         bool local = json_object_get_boolean(local_obj);
         bool remote = json_object_get_boolean(remote_obj);

         if (llm_tools_set_enabled(name, local, remote) == 0) {
            updated++;
         }
      }
   }

   /* Update config arrays for persistence - requires write lock on g_config */
   tool_info_t tools[LLM_TOOLS_MAX_TOOLS];
   int count = llm_tools_get_all(tools, LLM_TOOLS_MAX_TOOLS);

   pthread_rwlock_wrlock(&s_config_rwlock);

   g_config.llm.tools.local_enabled_count = 0;
   g_config.llm.tools.remote_enabled_count = 0;

   for (int i = 0; i < count; i++) {
      if (tools[i].enabled_local &&
          g_config.llm.tools.local_enabled_count < LLM_TOOLS_MAX_CONFIGURED) {
         safe_strncpy(g_config.llm.tools.local_enabled[g_config.llm.tools.local_enabled_count++],
                      tools[i].name, LLM_TOOL_NAME_MAX);
      }
      if (tools[i].enabled_remote &&
          g_config.llm.tools.remote_enabled_count < LLM_TOOLS_MAX_CONFIGURED) {
         safe_strncpy(g_config.llm.tools.remote_enabled[g_config.llm.tools.remote_enabled_count++],
                      tools[i].name, LLM_TOOL_NAME_MAX);
      }
   }

   /* Save to TOML */
   const char *config_path = config_get_loaded_path();
   if (!config_path || strcmp(config_path, "(none - using defaults)") == 0) {
      config_path = "./dawn.toml";
   }
   config_write_toml(&g_config, config_path);

   pthread_rwlock_unlock(&s_config_rwlock);

   json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   json_object_object_add(resp_payload, "updated", json_object_new_int(updated));

   /* Include updated token estimates */
   json_object *estimates = json_object_new_object();
   json_object_object_add(estimates, "local",
                          json_object_new_int(llm_tools_estimate_tokens(false)));
   json_object_object_add(estimates, "remote",
                          json_object_new_int(llm_tools_estimate_tokens(true)));
   json_object_object_add(resp_payload, "token_estimate", estimates);

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);

   LOG_INFO("WebUI: Updated %d tool enable states", updated);
}

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
   } else if (strcmp(type, "get_config") == 0) {
      /* Request current configuration */
      handle_get_config(conn);
   } else if (strcmp(type, "get_system_prompt") == 0) {
      /* Request current system prompt for debugging */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type", json_object_new_string("system_prompt_response"));
      struct json_object *resp_payload = json_object_new_object();

      if (conn->session) {
         char *prompt = session_get_system_prompt(conn->session);
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
      send_json_response(conn->wsi, response);
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
   } else if (strcmp(type, "restart") == 0) {
      /* Request application restart */
      LOG_INFO("WebUI: Restart requested by client");

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
                  LOG_INFO("WebUI: Switched to local LLM");
               } else if (strcmp(new_type, "cloud") == 0) {
                  /* When switching to cloud, ensure we have a valid provider selected.
                   * Prefer OpenAI if available, otherwise Claude. */
                  if (llm_has_openai_key()) {
                     llm_set_cloud_provider(CLOUD_PROVIDER_OPENAI);
                  } else if (llm_has_claude_key()) {
                     llm_set_cloud_provider(CLOUD_PROVIDER_CLAUDE);
                  }
                  int rc = llm_set_type(LLM_CLOUD);
                  if (rc != 0) {
                     success = 0;
                     error_msg = "No cloud API key configured in secrets.toml";
                  } else {
                     LOG_INFO("WebUI: Switched to cloud LLM");
                  }
               }
            }
         }

         /* Handle cloud provider change (openai/claude) */
         if (success && json_object_object_get_ex(payload, "provider", &provider_obj)) {
            const char *new_provider = json_object_get_string(provider_obj);
            if (new_provider) {
               int rc = 0;
               if (strcmp(new_provider, "openai") == 0) {
                  rc = llm_set_cloud_provider(CLOUD_PROVIDER_OPENAI);
               } else if (strcmp(new_provider, "claude") == 0) {
                  rc = llm_set_cloud_provider(CLOUD_PROVIDER_CLAUDE);
               }
               if (rc != 0) {
                  success = 0;
                  error_msg = "API key not configured for this provider";
               } else {
                  LOG_INFO("WebUI: Switched cloud provider to %s", new_provider);
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

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
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

         /* Parse type (local/cloud) */
         if (json_object_object_get_ex(payload, "type", &type_obj)) {
            const char *new_type = json_object_get_string(type_obj);
            if (new_type) {
               has_changes = true;
               if (strcmp(new_type, "local") == 0) {
                  config.type = LLM_LOCAL;
               } else if (strcmp(new_type, "cloud") == 0) {
                  config.type = LLM_CLOUD;
                  /* If no provider is set, default to OpenAI (same as cloudLLMCallback) */
                  if (config.cloud_provider == CLOUD_PROVIDER_NONE) {
                     config.cloud_provider = CLOUD_PROVIDER_OPENAI;
                     LOG_INFO("WebUI: No cloud provider set, defaulting to OpenAI");
                  }
               } else if (strcmp(new_type, "reset") == 0) {
                  /* Reset to defaults from dawn.toml */
                  session_clear_llm_config(conn->session);
                  LOG_INFO("WebUI: Session %u LLM config reset to defaults",
                           conn->session->session_id);
                  has_changes = false; /* Already handled */
               }
            }
         }

         /* Parse provider (openai/claude) */
         if (json_object_object_get_ex(payload, "provider", &provider_obj)) {
            const char *new_provider = json_object_get_string(provider_obj);
            if (new_provider) {
               has_changes = true;
               if (strcmp(new_provider, "openai") == 0) {
                  config.cloud_provider = CLOUD_PROVIDER_OPENAI;
               } else if (strcmp(new_provider, "claude") == 0) {
                  config.cloud_provider = CLOUD_PROVIDER_CLAUDE;
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
               LOG_INFO("WebUI: Session %u LLM config updated (type=%d, provider=%d)",
                        conn->session->session_id, config.type, config.cloud_provider);
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
         const char *provider_str = current.cloud_provider == CLOUD_PROVIDER_OPENAI   ? "openai"
                                    : current.cloud_provider == CLOUD_PROVIDER_CLAUDE ? "claude"
                                                                                      : "none";
         json_object_object_add(resp_payload, "type", json_object_new_string(type_str));
         json_object_object_add(resp_payload, "provider", json_object_new_string(provider_str));
      }

      /* Include API key availability */
      json_object_object_add(resp_payload, "openai_available",
                             json_object_new_boolean(llm_has_openai_key()));
      json_object_object_add(resp_payload, "claude_available",
                             json_object_new_boolean(llm_has_claude_key()));

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
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
                        if (generate_session_token(conn->session_token) != 0) {
                           LOG_ERROR("WebUI: Failed to generate session token");
                           session_destroy(conn->session->session_id);
                           conn->session = NULL;
                           return;
                        }
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
   } else if (strcmp(type, "smartthings_status") == 0) {
      /* Get SmartThings connection status */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type",
                             json_object_new_string("smartthings_status_response"));
      struct json_object *resp_payload = json_object_new_object();

      json_object_object_add(resp_payload, "configured",
                             json_object_new_boolean(smartthings_is_configured()));
      json_object_object_add(resp_payload, "authenticated",
                             json_object_new_boolean(smartthings_is_authenticated()));

      if (smartthings_is_configured()) {
         st_status_t status;
         smartthings_get_status(&status);
         json_object_object_add(resp_payload, "has_tokens",
                                json_object_new_boolean(status.has_tokens));
         json_object_object_add(resp_payload, "tokens_valid",
                                json_object_new_boolean(status.tokens_valid));
         json_object_object_add(resp_payload, "token_expiry",
                                json_object_new_int64(status.token_expiry));
         json_object_object_add(resp_payload, "devices_count",
                                json_object_new_int(status.devices_count));
         json_object_object_add(resp_payload, "auth_mode",
                                json_object_new_string(
                                    smartthings_auth_mode_str(status.auth_mode)));
      }

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
   } else if (strcmp(type, "smartthings_get_auth_url") == 0) {
      /* Get OAuth authorization URL */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type",
                             json_object_new_string("smartthings_auth_url_response"));
      struct json_object *resp_payload = json_object_new_object();

      if (!smartthings_is_configured()) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string(
                                    "SmartThings client credentials not configured"));
      } else {
         /* Build redirect URI from current WebUI URL */
         char redirect_uri[256];
         const dawn_config_t *cfg = config_get();
         snprintf(redirect_uri, sizeof(redirect_uri), "%s://localhost:%d/smartthings/callback",
                  cfg->webui.https ? "https" : "http", cfg->webui.port);

         char auth_url[1024];
         st_error_t err = smartthings_get_auth_url(redirect_uri, auth_url, sizeof(auth_url));
         if (err == ST_OK) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
            json_object_object_add(resp_payload, "auth_url", json_object_new_string(auth_url));
            json_object_object_add(resp_payload, "redirect_uri",
                                   json_object_new_string(redirect_uri));
         } else {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string(smartthings_error_str(err)));
         }
      }

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
   } else if (strcmp(type, "smartthings_exchange_code") == 0) {
      /* Exchange OAuth authorization code for tokens */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type",
                             json_object_new_string("smartthings_exchange_response"));
      struct json_object *resp_payload = json_object_new_object();

      struct json_object *code_obj, *redirect_obj, *state_obj;
      if (payload && json_object_object_get_ex(payload, "code", &code_obj) &&
          json_object_object_get_ex(payload, "redirect_uri", &redirect_obj)) {
         const char *code = json_object_get_string(code_obj);
         const char *redirect_uri = json_object_get_string(redirect_obj);
         const char *state = NULL;
         if (json_object_object_get_ex(payload, "state", &state_obj)) {
            state = json_object_get_string(state_obj);
         }

         st_error_t err = smartthings_exchange_code(code, redirect_uri, state);
         if (err == ST_OK) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
            LOG_INFO("WebUI: SmartThings OAuth authorization successful");
         } else {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string(smartthings_error_str(err)));
            LOG_WARNING("WebUI: SmartThings OAuth failed: %s", smartthings_error_str(err));
         }
      } else {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Missing code or redirect_uri"));
      }

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
   } else if (strcmp(type, "smartthings_disconnect") == 0) {
      /* Disconnect (clear tokens) */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type",
                             json_object_new_string("smartthings_disconnect_response"));
      struct json_object *resp_payload = json_object_new_object();

      smartthings_disconnect();
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      LOG_INFO("WebUI: SmartThings disconnected");

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
   } else if (strcmp(type, "smartthings_list_devices") == 0) {
      /* List all SmartThings devices */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type",
                             json_object_new_string("smartthings_devices_response"));
      struct json_object *resp_payload = json_object_new_object();

      if (!smartthings_is_authenticated()) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error", json_object_new_string("Not authenticated"));
      } else {
         const st_device_list_t *devices;
         st_error_t err = smartthings_list_devices(&devices);
         if (err == ST_OK) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
            json_object_object_add(resp_payload, "count", json_object_new_int(devices->count));

            struct json_object *devices_arr = json_object_new_array();
            for (int i = 0; i < devices->count; i++) {
               const st_device_t *dev = &devices->devices[i];
               struct json_object *dev_obj = json_object_new_object();
               json_object_object_add(dev_obj, "id", json_object_new_string(dev->id));
               json_object_object_add(dev_obj, "name", json_object_new_string(dev->name));
               json_object_object_add(dev_obj, "label", json_object_new_string(dev->label));
               json_object_object_add(dev_obj, "room", json_object_new_string(dev->room));

               /* Build capabilities array */
               struct json_object *caps = json_object_new_array();
               for (int j = 0; j < 15; j++) {
                  st_capability_t cap = (st_capability_t)(1 << j);
                  if (dev->capabilities & cap) {
                     json_object_array_add(caps,
                                           json_object_new_string(smartthings_capability_str(cap)));
                  }
               }
               json_object_object_add(dev_obj, "capabilities", caps);

               json_object_array_add(devices_arr, dev_obj);
            }
            json_object_object_add(resp_payload, "devices", devices_arr);
         } else {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string(smartthings_error_str(err)));
         }
      }

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
   } else if (strcmp(type, "smartthings_refresh_devices") == 0) {
      /* Force refresh device list */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type",
                             json_object_new_string("smartthings_devices_response"));
      struct json_object *resp_payload = json_object_new_object();

      if (!smartthings_is_authenticated()) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error", json_object_new_string("Not authenticated"));
      } else {
         const st_device_list_t *devices;
         st_error_t err = smartthings_refresh_devices(&devices);
         if (err == ST_OK) {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
            json_object_object_add(resp_payload, "count", json_object_new_int(devices->count));

            struct json_object *devices_arr = json_object_new_array();
            for (int i = 0; i < devices->count; i++) {
               const st_device_t *dev = &devices->devices[i];
               struct json_object *dev_obj = json_object_new_object();
               json_object_object_add(dev_obj, "id", json_object_new_string(dev->id));
               json_object_object_add(dev_obj, "name", json_object_new_string(dev->name));
               json_object_object_add(dev_obj, "label", json_object_new_string(dev->label));
               json_object_object_add(dev_obj, "room", json_object_new_string(dev->room));

               struct json_object *caps = json_object_new_array();
               for (int j = 0; j < 15; j++) {
                  st_capability_t cap = (st_capability_t)(1 << j);
                  if (dev->capabilities & cap) {
                     json_object_array_add(caps,
                                           json_object_new_string(smartthings_capability_str(cap)));
                  }
               }
               json_object_object_add(dev_obj, "capabilities", caps);

               json_object_array_add(devices_arr, dev_obj);
            }
            json_object_object_add(resp_payload, "devices", devices_arr);
         } else {
            json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
            json_object_object_add(resp_payload, "error",
                                   json_object_new_string(smartthings_error_str(err)));
         }
      }

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
   } else if (strcmp(type, "get_tools_config") == 0) {
      handle_get_tools_config(conn);
   } else if (strcmp(type, "set_tools_config") == 0) {
      if (payload) {
         handle_set_tools_config(conn, payload);
      }
   } else if (strcmp(type, "get_metrics") == 0) {
      handle_get_metrics(conn);
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
         /*
          * New WebSocket connection - defer session creation until init message.
          * This allows reconnecting clients (with valid token) to reuse their
          * existing session without counting against max_clients during the
          * brief overlap when refreshing a browser page.
          */
         memset(conn, 0, sizeof(*conn));
         conn->wsi = wsi;
         conn->session = NULL; /* Will be created/assigned on init message */

         LOG_INFO("WebUI: WebSocket connection established, awaiting init message");
         break;
      }

      case LWS_CALLBACK_CLOSED: {
         /* WebSocket disconnected */
         LOG_INFO("WebUI: WebSocket client disconnecting (session %u)",
                  conn->session ? conn->session->session_id : 0);

         bool had_session = (conn->session != NULL);

         if (conn->session) {
            /* Mark session as disconnected (aborts any pending LLM calls) */
            conn->session->disconnected = true;
            conn->session->client_data = NULL;

            /* Release our reference to the session.
             * Session manager will clean it up when ref_count reaches 0. */
            session_release(conn->session);
            conn->session = NULL;
         }

         /* Clear wsi reference */
         conn->wsi = NULL;

         /* Free any pending audio buffer */
         if (conn->audio_buffer) {
            free(conn->audio_buffer);
            conn->audio_buffer = NULL;
         }

         /* Only decrement client count if this connection had a session
          * (i.e., completed the init handshake) */
         if (had_session) {
            pthread_mutex_lock(&s_mutex);
            if (s_client_count > 0) {
               s_client_count--;
            }
            pthread_mutex_unlock(&s_mutex);
         }

         LOG_INFO("WebUI: WebSocket client disconnected (total: %d)", s_client_count);
         break;
      }

      case LWS_CALLBACK_RECEIVE: {
         /* Message received from client */
         if (!conn->session) {
            /*
             * No session yet - this must be the init/reconnect message.
             * Parse it to determine if we're reconnecting with a token or
             * need a new session. Check client limits here.
             */
            char *json_str = strndup((const char *)in, len);
            if (!json_str) {
               LOG_ERROR("WebUI: Failed to allocate init message buffer");
               return -1;
            }

            struct json_object *root = json_tokener_parse(json_str);
            free(json_str);

            if (!root) {
               LOG_WARNING("WebUI: Invalid JSON in init message");
               return -1;
            }

            struct json_object *type_obj;
            const char *type = NULL;
            if (json_object_object_get_ex(root, "type", &type_obj)) {
               type = json_object_get_string(type_obj);
            }

            struct json_object *payload = NULL;
            json_object_object_get_ex(root, "payload", &payload);

            bool is_reconnect = false;
            session_t *existing_session = NULL;

            /* Check for reconnection token */
            if (type && strcmp(type, "reconnect") == 0 && payload) {
               struct json_object *token_obj;
               if (json_object_object_get_ex(payload, "token", &token_obj)) {
                  const char *token = json_object_get_string(token_obj);
                  if (token && strlen(token) > 0) {
                     existing_session = lookup_session_by_token(token);
                     if (existing_session) {
                        is_reconnect = true;
                        conn->session = existing_session;
                        existing_session->client_data = conn;
                        existing_session->disconnected = false;
                        strncpy(conn->session_token, token, WEBUI_SESSION_TOKEN_LEN - 1);
                        conn->session_token[WEBUI_SESSION_TOKEN_LEN - 1] = '\0';

                        /* Reconnections still count against client limit */
                        pthread_mutex_lock(&s_mutex);
                        s_client_count++;
                        pthread_mutex_unlock(&s_mutex);

                        LOG_INFO("WebUI: Reconnected to session %u with token %.8s... (total: %d)",
                                 existing_session->session_id, token, s_client_count);

                        /* Send confirmation */
                        send_session_token_impl(conn->wsi, token);
                        send_config_impl(conn->wsi);
                        send_history_impl(conn->wsi, existing_session);
                        send_state_impl(conn->wsi, "idle");
                     }
                  }
               }
            }

            /* If not reconnecting, create new session (with client limit check) */
            if (!is_reconnect) {
               pthread_mutex_lock(&s_mutex);
               if (s_client_count >= g_config.webui.max_clients) {
                  pthread_mutex_unlock(&s_mutex);
                  LOG_WARNING("WebUI: Connection rejected - max clients reached (%d)",
                              g_config.webui.max_clients);
                  send_error_impl(wsi, "MAX_CLIENTS",
                                  "Maximum WebUI clients reached. Please try again later.");
                  json_object_put(root);
                  return -1;
               }
               s_client_count++;
               pthread_mutex_unlock(&s_mutex);

               conn->session = session_create(SESSION_TYPE_WEBSOCKET, -1);
               if (!conn->session) {
                  LOG_ERROR("WebUI: Failed to create session");
                  send_error_impl(wsi, "SESSION_LIMIT", "Maximum sessions reached");
                  pthread_mutex_lock(&s_mutex);
                  s_client_count--;
                  pthread_mutex_unlock(&s_mutex);
                  json_object_put(root);
                  return -1;
               }

               session_init_system_prompt(conn->session, get_remote_command_prompt());
               conn->session->client_data = conn;

               if (generate_session_token(conn->session_token) != 0) {
                  LOG_ERROR("WebUI: Failed to generate session token");
                  session_destroy(conn->session->session_id);
                  conn->session = NULL;
                  pthread_mutex_lock(&s_mutex);
                  s_client_count--;
                  pthread_mutex_unlock(&s_mutex);
                  json_object_put(root);
                  return -1;
               }
               register_token(conn->session_token, conn->session->session_id);

               LOG_INFO("WebUI: New session %u created (token %.8s..., total: %d)",
                        conn->session->session_id, conn->session_token, s_client_count);

               send_session_token_impl(conn->wsi, conn->session_token);
               send_config_impl(conn->wsi);
               send_state_impl(conn->wsi, "idle");
            }

            json_object_put(root);
            break; /* Don't process this message further - it was just the init */
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
 * Tool Execution Callback (for debug display)
 * ============================================================================= */

/**
 * @brief Callback for native tool execution notifications
 *
 * Sends tool call/result information to the WebUI for debug display.
 * Called by llm_tools module when tools are executed.
 */
/**
 * @brief Send LLM state update to WebSocket client
 *
 * Pushes the current LLM configuration to the client so it can update
 * the UI controls dynamically (e.g., after switch_llm tool call).
 */
static void webui_send_llm_state_update(session_t *session) {
   if (!session || session->type != SESSION_TYPE_WEBSOCKET) {
      return;
   }

   /* Get session's current LLM config */
   session_llm_config_t config;
   session_get_llm_config(session, &config);

   /* Build JSON response */
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("llm_state_update"));

   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "success", json_object_new_boolean(1));

   const char *type_str = config.type == LLM_LOCAL ? "local" : "cloud";
   const char *provider_str = config.cloud_provider == CLOUD_PROVIDER_OPENAI   ? "openai"
                              : config.cloud_provider == CLOUD_PROVIDER_CLAUDE ? "claude"
                                                                               : "none";

   json_object_object_add(payload, "type", json_object_new_string(type_str));
   json_object_object_add(payload, "provider", json_object_new_string(provider_str));
   json_object_object_add(payload, "model", json_object_new_string(config.model));
   json_object_object_add(payload, "openai_available",
                          json_object_new_boolean(llm_has_openai_key()));
   json_object_object_add(payload, "claude_available",
                          json_object_new_boolean(llm_has_claude_key()));

   json_object_object_add(response, "payload", payload);

   /* Queue response for WebSocket send */
   ws_response_t resp = { .session = session,
                          .type = WS_RESP_TRANSCRIPT,
                          .transcript = { .role = strdup("__llm_state__"),
                                          .text = strdup(json_object_to_json_string(response)) } };

   json_object_put(response);

   if (resp.transcript.role && resp.transcript.text) {
      queue_response(&resp);
   } else {
      free(resp.transcript.role);
      free(resp.transcript.text);
   }
}

static void webui_tool_execution_callback(void *session_ptr,
                                          const char *tool_name,
                                          const char *tool_args,
                                          const char *result,
                                          bool success) {
   session_t *session = (session_t *)session_ptr;
   if (!session || session->type != SESSION_TYPE_WEBSOCKET) {
      return;
   }

   /* Format as debug entry matching <command> tag format for consistent display */
   char debug_msg[LLM_TOOLS_RESULT_LEN + 256];
   snprintf(debug_msg, sizeof(debug_msg), "[Tool Call: %s(%s) -> %s%s]", tool_name,
            tool_args ? tool_args : "", success ? "" : "FAILED: ", result ? result : "no result");

   webui_send_transcript(session, "assistant", debug_msg);

   /* If this was a switch_llm call, send LLM state update to client */
   if (success && strcmp(tool_name, "switch_llm") == 0) {
      webui_send_llm_state_update(session);
   }
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

   /* Register tool execution callback for debug display */
   llm_tools_set_execution_callback(webui_tool_execution_callback);

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

void webui_send_context(session_t *session, int current_tokens, int max_tokens, float threshold) {
   /* If session is NULL, broadcast to all WebSocket sessions */
   if (!session) {
      /* Get local session as default */
      session = session_get_local();
   }

   if (!session || session->type != SESSION_TYPE_WEBSOCKET) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_CONTEXT,
                          .context = {
                              .current_tokens = current_tokens,
                              .max_tokens = max_tokens,
                              .threshold = threshold,
                          } };

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

      /* Add request_id, session_id, and timestamp to command JSON (OCP v1.1) */
      json_object_object_add(parsed_json, "request_id", json_object_new_string(request_id));
      json_object_object_add(parsed_json, "session_id",
                             json_object_new_int((int32_t)session->session_id));
      json_object_object_add(parsed_json, "timestamp",
                             json_object_new_int64(ocp_get_timestamp_ms()));
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

   /* Send context usage update to WebUI */
   {
      int current_tokens, max_tokens;
      float threshold;
      llm_context_get_last_usage(&current_tokens, &max_tokens, &threshold);
      if (max_tokens > 0) {
         webui_send_context(session, current_tokens, max_tokens, threshold);
      }
   }

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

   /* Send context usage update to WebUI */
   {
      int current_tokens, max_tokens;
      float threshold;
      llm_context_get_last_usage(&current_tokens, &max_tokens, &threshold);
      if (max_tokens > 0) {
         webui_send_context(session, current_tokens, max_tokens, threshold);
      }
   }

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
