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
         OLOG_INFO("Music server: New connection");
         memset(conn, 0, sizeof(*conn));
         conn->wsi = wsi;
         conn->authenticated = false;
         break;

      case LWS_CALLBACK_RECEIVE:
         /* Only expect JSON auth message before authentication */
         if (!conn->authenticated) {
            /* Reject binary frames before auth (prevents reading past buffer) */
            if (lws_frame_is_binary(wsi)) {
               OLOG_WARNING("Music server: Unexpected binary frame before auth");
               return LWS_CLOSE_CONNECTION;
            }

            /* Parse auth message */
            struct json_object *msg = json_tokener_parse((const char *)in);
            if (!msg) {
               OLOG_WARNING("Music server: Invalid JSON in auth message");
               return LWS_CLOSE_CONNECTION;
            }

            struct json_object *type_obj;
            if (!json_object_object_get_ex(msg, "type", &type_obj)) {
               json_object_put(msg);
               return LWS_CLOSE_CONNECTION;
            }

            const char *type = json_object_get_string(type_obj);
            /* json_object_get_string() returns NULL for a JSON null value; guard
             * before strcmp so an unauthenticated {"type":null} can't crash the
             * daemon (CWE-476 DoS). */
            if (type && strcmp(type, "auth") == 0) {
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

                     OLOG_INFO("Music server: Authenticated session %u", session->session_id);

                     /* Direct lws_write() is safe here (and allowlisted in
                      * scripts/check_no_ws_direct_write.sh) — unlike the main
                      * WebUI connection, this is a SEPARATE lws context with no
                      * shared response queue, the frame is a single fixed 18-byte
                      * handshake written exactly once, and it lands BEFORE any
                      * streaming begins, so it can never interleave with or be
                      * choked behind another frame.  Music audio frames use the
                      * proper WRITEABLE-callback path (webui_music_write_pending). */
                     const char *response = "{\"type\":\"auth_ok\"}";
                     unsigned char buf[LWS_PRE + 64];
                     size_t response_len = strlen(response);
                     memcpy(&buf[LWS_PRE], response, response_len);
                     lws_write(wsi, &buf[LWS_PRE], response_len, LWS_WRITE_TEXT);
                  } else {
                     OLOG_WARNING("Music server: Invalid token");
                     const char *response =
                         "{\"type\":\"auth_failed\",\"reason\":\"invalid_token\"}";
                     unsigned char buf[LWS_PRE + 64];
                     size_t response_len = strlen(response);
                     memcpy(&buf[LWS_PRE], response, response_len);
                     lws_write(wsi, &buf[LWS_PRE], response_len, LWS_WRITE_TEXT);
                     json_object_put(msg);
                     return LWS_CLOSE_CONNECTION;
                  }
               }
            }
            json_object_put(msg);
         } else {
            /* Post-auth, the only expected client message is the periodic buffer
             * report for closed-loop flow control. Be lenient: ignore anything
             * malformed / unknown / binary and NEVER close the socket — dropping it
             * would kill playback (and mirrors the client's own back-compat leniency
             * toward an old server). */
            if (!lws_frame_is_binary(wsi)) {
               struct json_tokener *tok = json_tokener_new();
               struct json_object *msg = tok ? json_tokener_parse_ex(tok, (const char *)in,
                                                                     (int)len)
                                             : NULL;
               if (tok) {
                  json_tokener_free(tok);
               }
               if (msg) {
                  struct json_object *type_obj = NULL;
                  struct json_object *ms_obj = NULL;
                  const char *mtype = NULL;
                  /* Guard json_object_get_string() → NULL for a JSON null value
                   * before strcmp (CWE-476): a {"type":null} frame must not crash. */
                  if (conn->session && json_object_object_get_ex(msg, "type", &type_obj) &&
                      (mtype = json_object_get_string(type_obj)) != NULL &&
                      strcmp(mtype, "music_buffer") == 0 &&
                      json_object_object_get_ex(msg, "buffered_ms", &ms_obj)) {
                     int ms = json_object_get_int(ms_obj);
                     if (ms < 0) {
                        ms = 0;
                     } else if (ms > WEBUI_MUSIC_CLIENT_BUFFER_MAX_MS) {
                        ms = WEBUI_MUSIC_CLIENT_BUFFER_MAX_MS;
                     }
                     webui_music_report_buffer(conn->session, (uint32_t)ms);
                  }
                  json_object_put(msg);
               }
            }
         }
         break;

      case LWS_CALLBACK_SERVER_WRITEABLE:
         /* Streaming thread requests writeable via lws_callback_on_writable().
          * The actual write is handled by webui_music_write_pending(). A fatal/short
          * write would leave a truncated frame on the wire and desync the client's
          * WS parser, so close the socket and let the client reconnect cleanly. */
         if (conn->authenticated && conn->session) {
            if (webui_music_write_pending(conn->session, wsi) == WEBUI_MUSIC_WRITE_CLOSE) {
               return LWS_CLOSE_CONNECTION;
            }
         }
         break;

      case LWS_CALLBACK_CLOSED:
         OLOG_INFO("Music server: Connection closed");
         if (conn->authenticated && conn->session) {
            /* Clear the stream wsi from the session */
            webui_music_set_stream_wsi(conn->session, NULL);
            /* Release the session reference acquired during auth */
            session_release(conn->session);
            conn->session = NULL;
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

   OLOG_INFO("Music server: Thread started");

   while (atomic_load(&s_music_server_running)) {
      /* Process events with short timeout for responsiveness */
      lws_service(s_music_lws_context, 10);
   }

   OLOG_INFO("Music server: Thread exiting");
   return NULL;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int webui_music_server_init(int port) {
   pthread_mutex_lock(&s_music_mutex);

   if (s_music_lws_context) {
      OLOG_WARNING("Music server: Already initialized");
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
         OLOG_INFO("Music server: HTTPS enabled (sharing cert with main server)");
      }
   }

   OLOG_INFO("Music server: Initializing on port %d", port);

   /* Match main server log level (errors and warnings only) */
   lws_set_log_level(LLL_ERR | LLL_WARN, NULL);

   /* Create context */
   s_music_lws_context = lws_create_context(&info);
   if (!s_music_lws_context) {
      OLOG_ERROR("Music server: Failed to create libwebsockets context");
      pthread_mutex_unlock(&s_music_mutex);
      return FAILURE;
   }

   s_music_port = port;
   atomic_store(&s_music_server_running, true);

   /* Start server thread */
   if (pthread_create(&s_music_server_thread, NULL, music_server_thread_func, NULL) != 0) {
      OLOG_ERROR("Music server: Failed to create server thread");
      lws_context_destroy(s_music_lws_context);
      s_music_lws_context = NULL;
      atomic_store(&s_music_server_running, false);
      pthread_mutex_unlock(&s_music_mutex);
      return FAILURE;
   }

   OLOG_INFO("Music server: Started on port %d", port);
   pthread_mutex_unlock(&s_music_mutex);
   return 0;
}

void webui_music_server_shutdown(void) {
   pthread_mutex_lock(&s_music_mutex);

   if (!s_music_lws_context) {
      pthread_mutex_unlock(&s_music_mutex);
      return;
   }

   OLOG_INFO("Music server: Shutting down");

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

   OLOG_INFO("Music server: Shutdown complete");
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
