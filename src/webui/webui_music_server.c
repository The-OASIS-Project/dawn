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
 * Dedicated Music Streaming WebSocket Server - Implementation
 *
 * A minimal WebSocket server that handles only binary audio streaming.
 * Control messages stay on the main WebSocket; this server just streams
 * Opus-encoded audio frames with minimal latency.
 */

#include "webui/webui_music_server.h"

#include <json-c/json.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config/dawn_config.h"
#include "logging.h"
#include "webui/webui_internal.h"
#include "webui/webui_music.h"
#include "webui/webui_server.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define MUSIC_SERVER_SUBPROTOCOL "dawn-music"
#define MUSIC_WS_RX_BUFFER_SIZE 1024 /* Small - only auth messages expected */

/* =============================================================================
 * Per-Connection State
 * ============================================================================= */

typedef struct {
   struct lws *wsi;
   bool authenticated;
   char session_token[WEBUI_SESSION_TOKEN_LEN];
   session_t *session; /* Link to main session */
} music_ws_connection_t;

/* =============================================================================
 * Module State
 * ============================================================================= */

static struct lws_context *s_music_lws_context = NULL;
static pthread_t s_music_server_thread;
static atomic_bool s_music_server_running = false;
static int s_music_port = 0;
static pthread_mutex_t s_music_mutex = PTHREAD_MUTEX_INITIALIZER;

/* =============================================================================
 * WebSocket Callback
 * ============================================================================= */

static int callback_music_websocket(struct lws *wsi,
                                    enum lws_callback_reasons reason,
                                    void *user,
                                    void *in,
                                    size_t len) {
   music_ws_connection_t *conn = (music_ws_connection_t *)user;

   switch (reason) {
      case LWS_CALLBACK_ESTABLISHED:
         LOG_INFO("Music server: New connection");
         memset(conn, 0, sizeof(*conn));
         conn->wsi = wsi;
         conn->authenticated = false;
         break;

      case LWS_CALLBACK_RECEIVE:
         /* Only expect JSON auth message before authentication */
         if (!conn->authenticated) {
            /* Parse auth message */
            struct json_object *msg = json_tokener_parse((const char *)in);
            if (!msg) {
               LOG_WARNING("Music server: Invalid JSON in auth message");
               return -1;
            }

            struct json_object *type_obj;
            if (!json_object_object_get_ex(msg, "type", &type_obj)) {
               json_object_put(msg);
               return -1;
            }

            const char *type = json_object_get_string(type_obj);
            if (strcmp(type, "auth") == 0) {
               struct json_object *token_obj;
               if (json_object_object_get_ex(msg, "token", &token_obj)) {
                  const char *token = json_object_get_string(token_obj);

                  /* Look up session by token */
                  session_t *session = lookup_session_by_token(token);
                  if (session) {
                     conn->authenticated = true;
                     conn->session = session;
                     strncpy(conn->session_token, token, WEBUI_SESSION_TOKEN_LEN - 1);
                     conn->session_token[WEBUI_SESSION_TOKEN_LEN - 1] = '\0';

                     /* Register this wsi with the session's music state */
                     webui_music_set_stream_wsi(session, wsi);

                     LOG_INFO("Music server: Authenticated session %u", session->session_id);

                     /* Send auth success */
                     const char *response = "{\"type\":\"auth_ok\"}";
                     unsigned char buf[LWS_PRE + 64];
                     size_t response_len = strlen(response);
                     memcpy(&buf[LWS_PRE], response, response_len);
                     lws_write(wsi, &buf[LWS_PRE], response_len, LWS_WRITE_TEXT);
                  } else {
                     LOG_WARNING("Music server: Invalid token");
                     const char *response =
                         "{\"type\":\"auth_failed\",\"reason\":\"invalid_token\"}";
                     unsigned char buf[LWS_PRE + 64];
                     size_t response_len = strlen(response);
                     memcpy(&buf[LWS_PRE], response, response_len);
                     lws_write(wsi, &buf[LWS_PRE], response_len, LWS_WRITE_TEXT);
                     json_object_put(msg);
                     return -1; /* Close connection */
                  }
               }
            }
            json_object_put(msg);
         }
         /* After auth, we don't expect any messages - audio flows server->client only */
         break;

      case LWS_CALLBACK_SERVER_WRITEABLE:
         /* Streaming thread requests writeable via lws_callback_on_writable().
          * The actual write is handled by webui_music_write_pending(). */
         if (conn->authenticated && conn->session) {
            webui_music_write_pending(conn->session, wsi);
         }
         break;

      case LWS_CALLBACK_CLOSED:
         LOG_INFO("Music server: Connection closed");
         if (conn->authenticated && conn->session) {
            /* Clear the stream wsi from the session */
            webui_music_set_stream_wsi(conn->session, NULL);
         }
         break;

      default:
         break;
   }

   return 0;
}

/* =============================================================================
 * Protocol Definition
 * ============================================================================= */

static struct lws_protocols s_music_protocols[] = {
   {
       .name = MUSIC_SERVER_SUBPROTOCOL,
       .callback = callback_music_websocket,
       .per_session_data_size = sizeof(music_ws_connection_t),
       .rx_buffer_size = MUSIC_WS_RX_BUFFER_SIZE,
   },
   { NULL, NULL, 0, 0 } /* Terminator */
};

/* =============================================================================
 * Server Thread
 * ============================================================================= */

static void *music_server_thread_func(void *arg) {
   (void)arg;

   LOG_INFO("Music server: Thread started");

   while (atomic_load(&s_music_server_running)) {
      /* Process events with short timeout for responsiveness */
      lws_service(s_music_lws_context, 10);
   }

   LOG_INFO("Music server: Thread exiting");
   return NULL;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int webui_music_server_init(int port) {
   pthread_mutex_lock(&s_music_mutex);

   if (s_music_lws_context) {
      LOG_WARNING("Music server: Already initialized");
      pthread_mutex_unlock(&s_music_mutex);
      return 0;
   }

   /* Determine port - default to main port + 1 */
   if (port == 0) {
      port = webui_server_get_port() + 1;
   }

   /* Configure libwebsockets context */
   struct lws_context_creation_info info;
   memset(&info, 0, sizeof(info));
   info.port = port;
   info.protocols = s_music_protocols;
   info.gid = -1;
   info.uid = -1;
   info.options = 0;

   /* Share SSL settings with main server if HTTPS is enabled */
   if (g_config.webui.https) {
      if (g_config.webui.ssl_cert_path[0] != '\0' && g_config.webui.ssl_key_path[0] != '\0') {
         info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
         info.ssl_cert_filepath = g_config.webui.ssl_cert_path;
         info.ssl_private_key_filepath = g_config.webui.ssl_key_path;
         info.alpn = "http/1.1";
         LOG_INFO("Music server: HTTPS enabled (sharing cert with main server)");
      }
   }

   LOG_INFO("Music server: Initializing on port %d", port);

   /* Create context */
   s_music_lws_context = lws_create_context(&info);
   if (!s_music_lws_context) {
      LOG_ERROR("Music server: Failed to create libwebsockets context");
      pthread_mutex_unlock(&s_music_mutex);
      return -1;
   }

   s_music_port = port;
   atomic_store(&s_music_server_running, true);

   /* Start server thread */
   if (pthread_create(&s_music_server_thread, NULL, music_server_thread_func, NULL) != 0) {
      LOG_ERROR("Music server: Failed to create server thread");
      lws_context_destroy(s_music_lws_context);
      s_music_lws_context = NULL;
      atomic_store(&s_music_server_running, false);
      pthread_mutex_unlock(&s_music_mutex);
      return -1;
   }

   LOG_INFO("Music server: Started on port %d", port);
   pthread_mutex_unlock(&s_music_mutex);
   return 0;
}

void webui_music_server_shutdown(void) {
   pthread_mutex_lock(&s_music_mutex);

   if (!s_music_lws_context) {
      pthread_mutex_unlock(&s_music_mutex);
      return;
   }

   LOG_INFO("Music server: Shutting down");

   /* Signal thread to stop */
   atomic_store(&s_music_server_running, false);

   /* Wake up lws_service() */
   lws_cancel_service(s_music_lws_context);

   pthread_mutex_unlock(&s_music_mutex);

   /* Wait for thread to exit */
   pthread_join(s_music_server_thread, NULL);

   pthread_mutex_lock(&s_music_mutex);

   /* Destroy context */
   lws_context_destroy(s_music_lws_context);
   s_music_lws_context = NULL;
   s_music_port = 0;

   LOG_INFO("Music server: Shutdown complete");
   pthread_mutex_unlock(&s_music_mutex);
}

bool webui_music_server_is_running(void) {
   return atomic_load(&s_music_server_running);
}

int webui_music_server_get_port(void) {
   return s_music_port;
}

void webui_music_server_wake(void) {
   if (s_music_lws_context) {
      lws_cancel_service(s_music_lws_context);
   }
}
