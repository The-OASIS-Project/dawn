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
 * Music WebSocket Stream Client
 *
 * Connects to daemon's music streaming port (main_port + 1) using
 * subprotocol "dawn-music". Receives binary Opus frames and pushes
 * them into the music_playback engine for decoding and ALSA output.
 *
 * Binary frame format (from daemon):
 *   [0x20 type byte][2-byte LE len][opus_data][2-byte LE len][opus_data]...
 */

#include "music_stream.h"

#include <json-c/json.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "logging.h"

#define MUSIC_STREAM_TYPE_OPUS 0x20
#define MUSIC_FRAME_MAX_LEN 1500

struct music_stream {
   char host[256];
   uint16_t port; /* main_port + 1 */
   bool use_ssl;
   bool ssl_verify;
   char session_token[33];

   struct lws_context *lws_ctx;
   struct lws *wsi;
   atomic_bool connected;
   atomic_bool authenticated;

   music_playback_t *playback; /* Not owned */

   pthread_mutex_t mutex;
   pthread_t service_thread;
   atomic_bool service_running;

   /* Auto-reconnect state */
   time_t last_disconnect;
   uint32_t reconnect_delay_ms; /* Current delay (exponential backoff) */
   atomic_bool reconnect_enabled;

   /* Fragment reassembly buffer for multi-fragment binary messages */
   uint8_t rx_buf[65536];
   size_t rx_len;

   char error[256];
};

/* =============================================================================
 * LWS Protocol
 * ============================================================================= */

static int callback_music_ws(struct lws *wsi,
                             enum lws_callback_reasons reason,
                             void *user,
                             void *in,
                             size_t len);

static const struct lws_protocols music_protocols[] = {
   {
       .name = "dawn-music",
       .callback = callback_music_ws,
       .per_session_data_size = sizeof(void *),
       .rx_buffer_size = 65536,
   },
   { 0 },
};

/* =============================================================================
 * Auth Message
 * ============================================================================= */

static int send_auth(music_stream_t *stream) {
   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "type", json_object_new_string("auth"));
   json_object_object_add(msg, "token", json_object_new_string(stream->session_token));

   const char *str = json_object_to_json_string_ext(msg, JSON_C_TO_STRING_PLAIN);
   size_t slen = strlen(str);

   unsigned char *buf = malloc(LWS_PRE + slen);
   if (!buf) {
      json_object_put(msg);
      return -1;
   }

   memcpy(buf + LWS_PRE, str, slen);
   int n = lws_write(stream->wsi, buf + LWS_PRE, slen, LWS_WRITE_TEXT);
   free(buf);
   json_object_put(msg);

   return (n < 0) ? -1 : 0;
}

/* =============================================================================
 * Parse Binary Opus Frames (inline — no rx_buf copy)
 * ============================================================================= */

static void parse_opus_frames(music_stream_t *stream, const uint8_t *data, size_t len) {
   if (len < 1 || data[0] != MUSIC_STREAM_TYPE_OPUS) {
      return;
   }

   size_t offset = 1;
   while (offset + 2 <= len) {
      uint16_t frame_len = (uint16_t)data[offset] | ((uint16_t)data[offset + 1] << 8);
      offset += 2;

      if (frame_len == 0 || frame_len > MUSIC_FRAME_MAX_LEN || offset + frame_len > len) {
         break;
      }

      int ret = music_playback_push_opus(stream->playback, data + offset, (int)frame_len);
      if (ret < 0) {
         LOG_WARNING("Music stream: Opus decode error, skipping frame (%u bytes)", frame_len);
      }

      offset += frame_len;
   }
}

/* =============================================================================
 * LWS Callback
 * ============================================================================= */

static int callback_music_ws(struct lws *wsi,
                             enum lws_callback_reasons reason,
                             void *user,
                             void *in,
                             size_t len) {
   (void)user;
   struct lws_context *ctx = lws_get_context(wsi);
   music_stream_t *stream = ctx ? (music_stream_t *)lws_context_user(ctx) : NULL;

   switch (reason) {
      case LWS_CALLBACK_CLIENT_ESTABLISHED:
         if (!stream)
            break;
         LOG_INFO("Music stream: connected to %s:%u", stream->host, stream->port);
         atomic_store(&stream->connected, true);
         stream->reconnect_delay_ms = 2000; /* Reset backoff on success */
         send_auth(stream);
         break;

      case LWS_CALLBACK_CLIENT_RECEIVE:
         if (!stream || !in || len == 0)
            break;

         if (!atomic_load(&stream->authenticated)) {
            /* Expecting JSON auth response — null-terminate before parsing */
            char auth_buf[512];
            size_t copy_len = (len < sizeof(auth_buf) - 1) ? len : sizeof(auth_buf) - 1;
            memcpy(auth_buf, in, copy_len);
            auth_buf[copy_len] = '\0';
            struct json_object *root = json_tokener_parse(auth_buf);
            if (root) {
               struct json_object *type_obj;
               if (json_object_object_get_ex(root, "type", &type_obj)) {
                  const char *type = json_object_get_string(type_obj);
                  if (type && strcmp(type, "auth_ok") == 0) {
                     atomic_store(&stream->authenticated, true);
                     LOG_INFO("Music stream: authenticated");
                  } else if (type && strcmp(type, "auth_failed") == 0) {
                     LOG_ERROR("Music stream: authentication failed");
                     snprintf(stream->error, sizeof(stream->error), "Auth failed");
                  }
               }
               json_object_put(root);
            }
         } else if (lws_frame_is_binary(wsi)) {
            if (lws_is_final_fragment(wsi) && lws_is_first_fragment(wsi)) {
               /* Single-fragment message — parse inline (common case) */
               parse_opus_frames(stream, (const uint8_t *)in, len);
            } else {
               /* Fragmented binary frame — accumulate */
               pthread_mutex_lock(&stream->mutex);
               if (lws_is_first_fragment(wsi)) {
                  stream->rx_len = 0;
               }
               if (stream->rx_len + len <= sizeof(stream->rx_buf)) {
                  memcpy(stream->rx_buf + stream->rx_len, in, len);
                  stream->rx_len += len;
               } else {
                  LOG_WARNING("Music stream: binary message too large, dropping");
                  stream->rx_len = 0;
               }
               if (lws_is_final_fragment(wsi) && stream->rx_len > 0) {
                  parse_opus_frames(stream, stream->rx_buf, stream->rx_len);
                  stream->rx_len = 0;
               }
               pthread_mutex_unlock(&stream->mutex);
            }
         }
         break;

      case LWS_CALLBACK_CLIENT_WRITEABLE:
         /* Nothing to send proactively — auth is sent on ESTABLISHED */
         break;

      case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
         LOG_ERROR("Music stream: connection error: %s", in ? (char *)in : "unknown");
         if (stream) {
            atomic_store(&stream->connected, false);
            atomic_store(&stream->authenticated, false);
            stream->last_disconnect = time(NULL);
         }
         break;

      case LWS_CALLBACK_CLIENT_CLOSED:
      case LWS_CALLBACK_CLOSED:
         LOG_INFO("Music stream: connection closed");
         if (stream) {
            atomic_store(&stream->connected, false);
            atomic_store(&stream->authenticated, false);
            stream->wsi = NULL;
            stream->last_disconnect = time(NULL);
         }
         break;

      default:
         break;
   }

   return 0;
}

/* =============================================================================
 * Service Thread
 * ============================================================================= */

static int attempt_reconnect(music_stream_t *stream);

static void *music_service_thread(void *arg) {
   music_stream_t *stream = (music_stream_t *)arg;

   LOG_INFO("Music stream service thread started");

   while (atomic_load(&stream->service_running)) {
      if (stream->lws_ctx) {
         lws_service(stream->lws_ctx, 10);
      }

      /* Auto-reconnect logic */
      if (atomic_load(&stream->service_running) && atomic_load(&stream->reconnect_enabled) &&
          !atomic_load(&stream->connected) && stream->last_disconnect > 0) {
         time_t now = time(NULL);
         time_t elapsed_ms = (now - stream->last_disconnect) * 1000;
         if (elapsed_ms >= (time_t)stream->reconnect_delay_ms) {
            LOG_INFO("Music stream: attempting reconnect (delay=%u ms)",
                     stream->reconnect_delay_ms);
            if (attempt_reconnect(stream) != 0) {
               /* Exponential backoff: 2s -> 4s -> 8s -> ... max 30s */
               stream->reconnect_delay_ms *= 2;
               if (stream->reconnect_delay_ms > 30000)
                  stream->reconnect_delay_ms = 30000;
               stream->last_disconnect = time(NULL);
            }
         }
      }
   }

   LOG_INFO("Music stream service thread stopped");
   return NULL;
}

/* =============================================================================
 * Connect/Reconnect Helper
 * ============================================================================= */

static int do_connect(music_stream_t *stream) {
   /* Create LWS context if needed */
   if (!stream->lws_ctx) {
      struct lws_context_creation_info ctx_info = { 0 };
      ctx_info.port = CONTEXT_PORT_NO_LISTEN;
      ctx_info.protocols = music_protocols;
      ctx_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
      ctx_info.user = stream;
      ctx_info.ka_time = 0;
      ctx_info.ka_probes = 0;
      ctx_info.ka_interval = 0;
      ctx_info.timeout_secs = 0;

      stream->lws_ctx = lws_create_context(&ctx_info);
      if (!stream->lws_ctx) {
         LOG_ERROR("Music stream: failed to create LWS context");
         return -1;
      }
   }

   /* Initiate connection */
   struct lws_client_connect_info conn_info = { 0 };
   conn_info.context = stream->lws_ctx;
   conn_info.address = stream->host;
   conn_info.port = stream->port;
   conn_info.path = "/";
   conn_info.host = stream->host;
   conn_info.origin = stream->host;
   conn_info.protocol = music_protocols[0].name;

   if (stream->use_ssl) {
      if (stream->ssl_verify) {
         conn_info.ssl_connection = LCCSCF_USE_SSL;
      } else {
         conn_info.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED |
                                    LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
      }
   }

   stream->wsi = lws_client_connect_via_info(&conn_info);
   if (!stream->wsi) {
      LOG_ERROR("Music stream: failed to initiate connection to %s:%u", stream->host, stream->port);
      return -1;
   }

   LOG_INFO("Music stream: connecting to %s:%u...", stream->host, stream->port);
   return 0;
}

static int attempt_reconnect(music_stream_t *stream) {
   /* Destroy old LWS context before creating new one */
   if (stream->lws_ctx) {
      lws_context_destroy(stream->lws_ctx);
      stream->lws_ctx = NULL;
      stream->wsi = NULL;
   }
   return do_connect(stream);
}

/* =============================================================================
 * Public API
 * ============================================================================= */

music_stream_t *music_stream_create(const char *host,
                                    uint16_t port,
                                    bool use_ssl,
                                    bool ssl_verify,
                                    const char *session_token,
                                    music_playback_t *playback) {
   if (!host || !session_token || !playback)
      return NULL;

   music_stream_t *stream = calloc(1, sizeof(music_stream_t));
   if (!stream)
      return NULL;

   strncpy(stream->host, host, sizeof(stream->host) - 1);
   stream->port = port + 1; /* Music stream is on main_port + 1 */
   stream->use_ssl = use_ssl;
   stream->ssl_verify = ssl_verify;
   strncpy(stream->session_token, session_token, sizeof(stream->session_token) - 1);
   stream->playback = playback;
   stream->reconnect_delay_ms = 2000;
   atomic_store(&stream->reconnect_enabled, true);

   pthread_mutex_init(&stream->mutex, NULL);

   LOG_INFO("Music stream created for %s://%s:%u", use_ssl ? "wss" : "ws", host, stream->port);
   return stream;
}

void music_stream_destroy(music_stream_t *stream) {
   if (!stream)
      return;

   music_stream_disconnect(stream);
   pthread_mutex_destroy(&stream->mutex);
   free(stream);

   LOG_INFO("Music stream destroyed");
}

int music_stream_connect(music_stream_t *stream) {
   if (!stream)
      return -1;

   if (atomic_load(&stream->connected))
      return 0;

   if (do_connect(stream) != 0)
      return -1;

   /* Wait for connection with timeout (service thread not started yet) */
   int timeout_ms = 5000;
   int elapsed = 0;
   while (!atomic_load(&stream->connected) && elapsed < timeout_ms) {
      lws_service(stream->lws_ctx, 100);
      elapsed += 100;
   }

   if (!atomic_load(&stream->connected)) {
      LOG_ERROR("Music stream: connection timeout");
      /* Clean up LWS context on timeout */
      if (stream->lws_ctx) {
         lws_context_destroy(stream->lws_ctx);
         stream->lws_ctx = NULL;
         stream->wsi = NULL;
      }
      return -1;
   }

   /* Wait for auth response */
   elapsed = 0;
   while (!atomic_load(&stream->authenticated) && elapsed < 3000) {
      lws_service(stream->lws_ctx, 100);
      elapsed += 100;
   }

   if (!atomic_load(&stream->authenticated)) {
      LOG_ERROR("Music stream: auth timeout");
      /* Clean up LWS context on auth timeout */
      if (stream->lws_ctx) {
         lws_context_destroy(stream->lws_ctx);
         stream->lws_ctx = NULL;
         stream->wsi = NULL;
      }
      atomic_store(&stream->connected, false);
      return -1;
   }

   /* Start service thread for ongoing message handling */
   atomic_store(&stream->service_running, true);
   if (pthread_create(&stream->service_thread, NULL, music_service_thread, stream) != 0) {
      LOG_ERROR("Music stream: failed to create service thread");
      return -1;
   }

   return 0;
}

void music_stream_disconnect(music_stream_t *stream) {
   if (!stream)
      return;

   atomic_store(&stream->reconnect_enabled, false);

   /* Stop service thread */
   if (atomic_load(&stream->service_running)) {
      atomic_store(&stream->service_running, false);
      if (stream->lws_ctx)
         lws_cancel_service(stream->lws_ctx);
      pthread_join(stream->service_thread, NULL);
   }

   if (stream->lws_ctx) {
      lws_context_destroy(stream->lws_ctx);
      stream->lws_ctx = NULL;
   }

   stream->wsi = NULL;
   atomic_store(&stream->connected, false);
   atomic_store(&stream->authenticated, false);
}

bool music_stream_is_connected(music_stream_t *stream) {
   return stream && atomic_load(&stream->connected) && atomic_load(&stream->authenticated);
}
