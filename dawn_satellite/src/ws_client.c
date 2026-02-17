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
 * DAP2 WebSocket Client - Implementation
 */

#include "ws_client.h"

#include <json-c/json.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Shared logging (same format as daemon) */
#include "logging.h"

#ifdef HAVE_OPUS
#include "music_playback.h"
#endif

/* =============================================================================
 * Internal Structures
 * ============================================================================= */

#define WS_RX_BUFFER_SIZE 65536
#define WS_RX_BUFFER_MAX (2 * 1024 * 1024) /* 2MB cap to prevent OOM */
#define WS_TX_BUFFER_SIZE 4096
#define WS_TX_QUEUE_SIZE 8 /* Ring buffer for pending TX messages */

typedef struct {
   char *data;
   size_t len;
} ws_tx_entry_t;

struct ws_client {
   /* Connection info */
   char host[256];
   uint16_t port;
   bool use_ssl;
   bool ssl_verify; /* Verify SSL certificates (default: true) */

   /* State */
   ws_state_t state;
   bool registered;
   uint32_t session_id;

   /* Identity */
   ws_identity_t identity;
   ws_capabilities_t caps;

   /* libwebsockets context */
   struct lws_context *lws_ctx;
   struct lws *wsi;

   /* Receive buffer for fragmented messages */
   char *rx_buffer;
   size_t rx_len;
   size_t rx_capacity;

   /* Pending send queue (ring buffer) */
   ws_tx_entry_t tx_queue[WS_TX_QUEUE_SIZE];
   int tx_head;  /* Next to send */
   int tx_tail;  /* Next to write */
   int tx_count; /* Number of pending entries */

   /* Callbacks */
   ws_stream_callback_t stream_cb;
   void *stream_cb_data;
   ws_state_callback_t state_cb;
   void *state_cb_data;

   /* Music callbacks */
   ws_music_state_cb_t music_state_cb;
   ws_music_position_cb_t music_position_cb;
   ws_music_queue_cb_t music_queue_cb;
   ws_music_library_cb_t music_library_cb;
   void *music_cb_data;

   /* Session token for music WebSocket auth (32 hex chars + null) */
   char session_token[33];

#ifdef HAVE_OPUS
   /* Music playback engine for binary audio fallback on main WS (not owned) */
   music_playback_t *music_pb;
#endif

   /* Error message */
   char error_msg[256];

   /* Status detail from daemon (tool calls, thinking info) */
   char status_detail[128];

   /* Connection timing */
   time_t connected_at; /* Wall-clock time of last successful connection (0 = not connected) */

   /* Thread safety */
   pthread_mutex_t mutex;

   /* Background service thread */
   pthread_t service_thread;
   volatile bool service_running;
};

/* =============================================================================
 * Forward Declarations
 * ============================================================================= */

static int callback_ws(struct lws *wsi,
                       enum lws_callback_reasons reason,
                       void *user,
                       void *in,
                       size_t len);
static void handle_message(ws_client_t *client, const char *msg, size_t len);
static int send_json(ws_client_t *client, struct json_object *obj);
static const char *json_get_string(struct json_object *obj, const char *key);
static int json_get_int(struct json_object *obj, const char *key);
static double json_get_double(struct json_object *obj, const char *key);
static bool json_get_bool(struct json_object *obj, const char *key);
static void parse_track(struct json_object *obj, music_track_t *track);
static void parse_music_state(struct json_object *payload, music_state_update_t *state);
static void parse_music_queue(struct json_object *payload, music_queue_update_t *queue);
static void parse_music_library(struct json_object *payload, music_library_update_t *lib);

/* =============================================================================
 * libwebsockets Protocol
 * ============================================================================= */

static const struct lws_protocols protocols[] = {
   {
       .name = "dawn-1.0", /* Must match server's expected subprotocol */
       .callback = callback_ws,
       .per_session_data_size = sizeof(void *),
       .rx_buffer_size = WS_RX_BUFFER_SIZE,
       .id = 0,
       .user = NULL,
       .tx_packet_size = 0,
   },
   { 0 } /* terminator - zero init all fields */
};

/* =============================================================================
 * WebSocket Callback
 * ============================================================================= */

static int callback_ws(struct lws *wsi,
                       enum lws_callback_reasons reason,
                       void *user,
                       void *in,
                       size_t len) {
   (void)user; /* Per-session data not used, we use context user instead */

   /* Get client from context user data */
   struct lws_context *ctx = lws_get_context(wsi);
   ws_client_t *client = ctx ? (ws_client_t *)lws_context_user(ctx) : NULL;

   switch (reason) {
      case LWS_CALLBACK_CLIENT_ESTABLISHED:
         LOG_INFO("Connected to daemon");
         if (client) {
            pthread_mutex_lock(&client->mutex);
            client->state = WS_STATE_CONNECTED;
            client->connected_at = time(NULL);
            pthread_mutex_unlock(&client->mutex);
         }
         break;

      case LWS_CALLBACK_CLIENT_RECEIVE:
         if (client && in && len > 0) {
            /* Handle binary frames (music audio fallback from main WS) */
            if (lws_frame_is_binary(wsi)) {
#ifdef HAVE_OPUS
               if (client->music_pb && len >= 1 &&
                   !music_playback_has_dedicated_producer(client->music_pb)) {
                  const uint8_t *data = (const uint8_t *)in;
                  if (data[0] == 0x20) { /* WS_BIN_MUSIC_DATA */
                     /* Parse opus frames: [0x20][2-byte LE len][opus]... */
                     size_t offset = 1;
                     while (offset + 2 <= len) {
                        uint16_t frame_len = (uint16_t)data[offset] |
                                             ((uint16_t)data[offset + 1] << 8);
                        offset += 2;
                        if (frame_len == 0 || frame_len > 1500 || offset + frame_len > len)
                           break;
                        music_playback_push_opus(client->music_pb, data + offset, (int)frame_len);
                        offset += frame_len;
                     }
                  }
               }
#endif
               break;
            }

            pthread_mutex_lock(&client->mutex);

            /* Expand buffer if needed (capped to prevent OOM from rogue messages).
             * Overflow-safe check: use subtraction to avoid size_t wraparound. */
            if (client->rx_len > WS_RX_BUFFER_MAX - len - 1) {
               LOG_ERROR("Message too large (%zu + %zu bytes > %d max), dropping", client->rx_len,
                         len, WS_RX_BUFFER_MAX);
               client->rx_len = 0;
               pthread_mutex_unlock(&client->mutex);
               return -1;
            }
            size_t needed = client->rx_len + len + 1;
            if (needed > client->rx_capacity) {
               size_t new_cap = client->rx_capacity * 2;
               if (new_cap < needed)
                  new_cap = needed;
               char *new_buf = realloc(client->rx_buffer, new_cap);
               if (!new_buf) {
                  LOG_ERROR("Failed to expand receive buffer");
                  pthread_mutex_unlock(&client->mutex);
                  return -1;
               }
               client->rx_buffer = new_buf;
               client->rx_capacity = new_cap;
            }

            /* Append data */
            memcpy(client->rx_buffer + client->rx_len, in, len);
            client->rx_len += len;
            client->rx_buffer[client->rx_len] = '\0';

            /* Check if message is complete (not fragmented) */
            if (lws_is_final_fragment(wsi)) {
               handle_message(client, client->rx_buffer, client->rx_len);
               client->rx_len = 0;
            }

            pthread_mutex_unlock(&client->mutex);
         }
         break;

      case LWS_CALLBACK_CLIENT_WRITEABLE:
         if (client) {
            pthread_mutex_lock(&client->mutex);
            if (client->tx_count == 0) {
               pthread_mutex_unlock(&client->mutex);
               break;
            }

            /* Dequeue next entry */
            ws_tx_entry_t entry = client->tx_queue[client->tx_head];
            client->tx_queue[client->tx_head].data = NULL;
            client->tx_queue[client->tx_head].len = 0;
            client->tx_head = (client->tx_head + 1) % WS_TX_QUEUE_SIZE;
            client->tx_count--;
            bool more_pending = (client->tx_count > 0);
            pthread_mutex_unlock(&client->mutex);

            /* Allocate buffer with LWS_PRE padding */
            unsigned char *buf = malloc(LWS_PRE + entry.len);
            if (buf) {
               memcpy(buf + LWS_PRE, entry.data, entry.len);
               int n = lws_write(wsi, buf + LWS_PRE, entry.len, LWS_WRITE_TEXT);
               free(buf);

               if (n < 0) {
                  LOG_ERROR("Write failed");
               } else {
                  LOG_DEBUG("Sent %d bytes", n);
               }
            }

            free(entry.data);

            /* If more messages queued, request another writable callback */
            if (more_pending) {
               lws_callback_on_writable(wsi);
            }
         }
         break;

      case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
         LOG_ERROR("Connection error: %s", in ? (char *)in : "unknown");
         if (client) {
            pthread_mutex_lock(&client->mutex);
            client->state = WS_STATE_ERROR;
            snprintf(client->error_msg, sizeof(client->error_msg), "Connection error: %s",
                     in ? (char *)in : "unknown");
            pthread_mutex_unlock(&client->mutex);
         }
         break;

      case LWS_CALLBACK_CLIENT_CLOSED:
      case LWS_CALLBACK_CLOSED:
         LOG_INFO("Connection closed");
         if (client) {
            pthread_mutex_lock(&client->mutex);
            client->state = WS_STATE_DISCONNECTED;
            client->registered = false;
            client->wsi = NULL;
            client->connected_at = 0;
            pthread_mutex_unlock(&client->mutex);
         }
         break;

      default:
         break;
   }

   return 0;
}

/* =============================================================================
 * Message Handling
 * ============================================================================= */

static void handle_message(ws_client_t *client, const char *msg, size_t len) {
   (void)len;

   struct json_object *root = json_tokener_parse(msg);
   if (!root) {
      LOG_ERROR("Failed to parse JSON: %.100s", msg);
      return;
   }

   struct json_object *type_obj;
   if (!json_object_object_get_ex(root, "type", &type_obj)) {
      LOG_ERROR("Message missing 'type' field");
      json_object_put(root);
      return;
   }

   const char *type = json_object_get_string(type_obj);
   struct json_object *payload = NULL;
   json_object_object_get_ex(root, "payload", &payload);

   LOG_DEBUG("Received message type: %s", type);

   if (strcmp(type, "satellite_register_ack") == 0) {
      if (payload) {
         struct json_object *success_obj, *session_obj;
         bool success = false;
         uint32_t session_id = 0;

         if (json_object_object_get_ex(payload, "success", &success_obj)) {
            success = json_object_get_boolean(success_obj);
         }
         if (json_object_object_get_ex(payload, "session_id", &session_obj)) {
            session_id = json_object_get_int(session_obj);
         }

         if (success) {
            client->registered = true;
            client->session_id = session_id;
            client->state = WS_STATE_REGISTERED;

            /* Extract and store reconnect secret for future reconnections
             * SECURITY: Client MUST save this secret persistently */
            struct json_object *secret_obj;
            if (json_object_object_get_ex(payload, "reconnect_secret", &secret_obj)) {
               const char *secret = json_object_get_string(secret_obj);
               if (secret && secret[0]) {
                  strncpy(client->identity.reconnect_secret, secret,
                          sizeof(client->identity.reconnect_secret) - 1);
                  client->identity.reconnect_secret[sizeof(client->identity.reconnect_secret) - 1] =
                      '\0';
                  LOG_INFO("Received reconnect secret (stored)");
               }
            }

            /* Extract session token for music WebSocket auth */
            struct json_object *token_obj;
            if (json_object_object_get_ex(payload, "session_token", &token_obj)) {
               const char *token = json_object_get_string(token_obj);
               if (token && token[0]) {
                  strncpy(client->session_token, token, sizeof(client->session_token) - 1);
                  client->session_token[sizeof(client->session_token) - 1] = '\0';
                  LOG_INFO("Received session token: %.8s...", token);
               }
            }

            LOG_INFO("Registered successfully, session_id=%u", session_id);
         } else {
            struct json_object *msg_obj;
            const char *err_msg = "Unknown error";
            if (json_object_object_get_ex(payload, "message", &msg_obj)) {
               err_msg = json_object_get_string(msg_obj);
            }
            LOG_ERROR("Registration failed: %s", err_msg);
         }
      }
   } else if (strcmp(type, "satellite_pong") == 0) {
      LOG_DEBUG("Pong received");
   } else if (strcmp(type, "state") == 0) {
      if (payload) {
         struct json_object *state_obj, *detail_obj;
         if (json_object_object_get_ex(payload, "state", &state_obj)) {
            const char *state = json_object_get_string(state_obj);
            LOG_DEBUG("State: %s", state);

            /* Store detail if present; clear only on idle (not on every state) */
            if (json_object_object_get_ex(payload, "detail", &detail_obj)) {
               const char *detail = json_object_get_string(detail_obj);
               if (detail) {
                  strncpy(client->status_detail, detail, sizeof(client->status_detail) - 1);
                  client->status_detail[sizeof(client->status_detail) - 1] = '\0';
               }
            }
            if (strcmp(state, "idle") == 0) {
               client->status_detail[0] = '\0';
            }

            /* Copy callback pointers before unlock to prevent UAF */
            ws_state_callback_t state_cb = client->state_cb;
            void *state_cb_data = client->state_cb_data;
            if (state_cb) {
               pthread_mutex_unlock(&client->mutex);
               state_cb(state, state_cb_data);
               pthread_mutex_lock(&client->mutex);
            }
         }
      }
   } else if (strcmp(type, "stream_start") == 0) {
      LOG_DEBUG("Stream start");
   } else if (strcmp(type, "stream_delta") == 0) {
      if (payload) {
         struct json_object *delta_obj;
         if (json_object_object_get_ex(payload, "delta", &delta_obj)) {
            const char *text = json_object_get_string(delta_obj);

            ws_stream_callback_t stream_cb = client->stream_cb;
            void *stream_cb_data = client->stream_cb_data;
            if (stream_cb && text) {
               pthread_mutex_unlock(&client->mutex);
               stream_cb(text, false, stream_cb_data);
               pthread_mutex_lock(&client->mutex);
            }
         }
      }
   } else if (strcmp(type, "stream_end") == 0) {
      LOG_DEBUG("Stream end");
      ws_stream_callback_t stream_cb = client->stream_cb;
      void *stream_cb_data = client->stream_cb_data;
      if (stream_cb) {
         pthread_mutex_unlock(&client->mutex);
         stream_cb("", true, stream_cb_data);
         pthread_mutex_lock(&client->mutex);
      }
   } else if (strcmp(type, "error") == 0) {
      if (payload) {
         struct json_object *code_obj, *msg_obj;
         const char *code = "UNKNOWN";
         const char *message = "Unknown error";

         if (json_object_object_get_ex(payload, "code", &code_obj)) {
            code = json_object_get_string(code_obj);
         }
         if (json_object_object_get_ex(payload, "message", &msg_obj)) {
            message = json_object_get_string(msg_obj);
         }

         LOG_ERROR("Error [%s]: %s", code, message);
         snprintf(client->error_msg, sizeof(client->error_msg), "[%s] %s", code, message);
      }
   } else if (strcmp(type, "transcript") == 0) {
      if (payload) {
         struct json_object *role_obj, *text_obj;
         if (json_object_object_get_ex(payload, "role", &role_obj) &&
             json_object_object_get_ex(payload, "text", &text_obj)) {
            const char *role = json_object_get_string(role_obj);
            const char *text = json_object_get_string(text_obj);

            ws_stream_callback_t scb = client->stream_cb;
            void *scb_data = client->stream_cb_data;
            if (strcmp(role, "satellite_response") == 0 && scb && text) {
               pthread_mutex_unlock(&client->mutex);
               scb(text, true, scb_data);
               pthread_mutex_lock(&client->mutex);
            }
         }
      }
   } else if (strcmp(type, "music_state") == 0) {
      if (payload && client->music_state_cb) {
         music_state_update_t update;
         parse_music_state(payload, &update);
         ws_music_state_cb_t cb = client->music_state_cb;
         void *ud = client->music_cb_data;
         pthread_mutex_unlock(&client->mutex);
         cb(&update, ud);
         pthread_mutex_lock(&client->mutex);
      }
   } else if (strcmp(type, "music_position") == 0) {
      if (payload && client->music_position_cb) {
         float pos = (float)json_get_double(payload, "position_sec");
         ws_music_position_cb_t cb = client->music_position_cb;
         void *ud = client->music_cb_data;
         pthread_mutex_unlock(&client->mutex);
         cb(pos, ud);
         pthread_mutex_lock(&client->mutex);
      }
   } else if (strcmp(type, "music_queue_response") == 0) {
      if (payload && client->music_queue_cb) {
         /* Static to avoid ~175KB on thread stack (100 tracks × 1.8KB each).
          * Safe: handle_message() is single-threaded under client->mutex. */
         static music_queue_update_t queue_update;
         parse_music_queue(payload, &queue_update);
         ws_music_queue_cb_t cb = client->music_queue_cb;
         void *ud = client->music_cb_data;
         pthread_mutex_unlock(&client->mutex);
         cb(&queue_update, ud);
         pthread_mutex_lock(&client->mutex);
      }
   } else if (strcmp(type, "music_library_response") == 0) {
      if (payload && client->music_library_cb) {
         /* Static to avoid ~100KB on thread stack (50 tracks + 50 items).
          * Safe: handle_message() is single-threaded under client->mutex. */
         static music_library_update_t lib_update;
         parse_music_library(payload, &lib_update);
         ws_music_library_cb_t cb = client->music_library_cb;
         void *ud = client->music_cb_data;
         pthread_mutex_unlock(&client->mutex);
         cb(&lib_update, ud);
         pthread_mutex_lock(&client->mutex);
      }
   } else if (strcmp(type, "music_error") == 0) {
      if (payload) {
         LOG_WARNING("Music error: %s", json_get_string(payload, "message"));
      }
   } else {
      LOG_DEBUG("Unknown message type: %s", type);
   }

   json_object_put(root);
}

/* =============================================================================
 * Music JSON Parsing Helpers
 * ============================================================================= */

static const char *json_get_string(struct json_object *obj, const char *key) {
   struct json_object *val;
   if (json_object_object_get_ex(obj, key, &val)) {
      return json_object_get_string(val);
   }
   return "";
}

static int json_get_int(struct json_object *obj, const char *key) {
   struct json_object *val;
   if (json_object_object_get_ex(obj, key, &val)) {
      return json_object_get_int(val);
   }
   return 0;
}

static double json_get_double(struct json_object *obj, const char *key) {
   struct json_object *val;
   if (json_object_object_get_ex(obj, key, &val)) {
      return json_object_get_double(val);
   }
   return 0.0;
}

static bool json_get_bool(struct json_object *obj, const char *key) {
   struct json_object *val;
   if (json_object_object_get_ex(obj, key, &val)) {
      return json_object_get_boolean(val);
   }
   return false;
}

static void parse_track(struct json_object *obj, music_track_t *track) {
   snprintf(track->path, MUSIC_MAX_PATH, "%s", json_get_string(obj, "path"));
   snprintf(track->title, MUSIC_MAX_TITLE, "%s", json_get_string(obj, "title"));
   snprintf(track->artist, MUSIC_MAX_ARTIST, "%s", json_get_string(obj, "artist"));
   snprintf(track->album, MUSIC_MAX_ALBUM, "%s", json_get_string(obj, "album"));
   track->duration_sec = (uint32_t)json_get_int(obj, "duration_sec");
}

static void parse_music_state(struct json_object *payload, music_state_update_t *state) {
   memset(state, 0, sizeof(*state));
   state->playing = json_get_bool(payload, "playing");
   state->paused = json_get_bool(payload, "paused");
   state->duration_sec = (float)json_get_double(payload, "duration_sec");

   struct json_object *track_obj;
   if (json_object_object_get_ex(payload, "track", &track_obj) && track_obj &&
       !json_object_is_type(track_obj, json_type_null)) {
      parse_track(track_obj, &state->track);
      /* Prefer track's own duration if available */
      if (state->track.duration_sec > 0)
         state->duration_sec = (float)state->track.duration_sec;
   }

   snprintf(state->source_format, sizeof(state->source_format), "%s",
            json_get_string(payload, "source_format"));
   state->source_rate = json_get_int(payload, "source_rate");
   state->bitrate = json_get_int(payload, "bitrate");
   snprintf(state->bitrate_mode, sizeof(state->bitrate_mode), "%s",
            json_get_string(payload, "bitrate_mode"));
   state->queue_length = json_get_int(payload, "queue_length");
   state->queue_index = json_get_int(payload, "queue_index");
   state->shuffle = json_get_bool(payload, "shuffle");
   state->repeat_mode = json_get_int(payload, "repeat_mode");
}

static void parse_music_queue(struct json_object *payload, music_queue_update_t *queue) {
   memset(queue, 0, sizeof(*queue));
   queue->current_index = json_get_int(payload, "current_index");

   struct json_object *queue_arr;
   if (json_object_object_get_ex(payload, "queue", &queue_arr) &&
       json_object_is_type(queue_arr, json_type_array)) {
      int len = json_object_array_length(queue_arr);
      if (len > MUSIC_MAX_QUEUE)
         len = MUSIC_MAX_QUEUE;
      queue->count = len;
      for (int i = 0; i < len; i++) {
         struct json_object *item = json_object_array_get_idx(queue_arr, i);
         if (item) {
            parse_track(item, &queue->tracks[i]);
         }
      }
   }
}

static void parse_music_library(struct json_object *payload, music_library_update_t *lib) {
   memset(lib, 0, sizeof(*lib));

   const char *browse_type = json_get_string(payload, "browse_type");

   if (strcmp(browse_type, "stats") == 0) {
      lib->browse_type = MUSIC_BROWSE_NONE;
      lib->stat_tracks = json_get_int(payload, "track_count");
      lib->stat_artists = json_get_int(payload, "artist_count");
      lib->stat_albums = json_get_int(payload, "album_count");

   } else if (strcmp(browse_type, "tracks") == 0 || strcmp(browse_type, "tracks_by_artist") == 0 ||
              strcmp(browse_type, "tracks_by_album") == 0) {
      /* All three share the same "tracks" array parsing */
      if (strcmp(browse_type, "tracks") == 0)
         lib->browse_type = MUSIC_BROWSE_TRACKS;
      else if (strcmp(browse_type, "tracks_by_artist") == 0)
         lib->browse_type = MUSIC_BROWSE_BY_ARTIST;
      else
         lib->browse_type = MUSIC_BROWSE_BY_ALBUM;

      lib->total_count = json_get_int(payload, "total_count");
      lib->offset = json_get_int(payload, "offset");

      struct json_object *arr;
      if (json_object_object_get_ex(payload, "tracks", &arr) &&
          json_object_is_type(arr, json_type_array)) {
         int len = json_object_array_length(arr);
         if (len > MUSIC_MAX_RESULTS)
            len = MUSIC_MAX_RESULTS;
         lib->track_count = len;
         for (int i = 0; i < len; i++) {
            struct json_object *item = json_object_array_get_idx(arr, i);
            if (item)
               parse_track(item, &lib->tracks[i]);
         }
      }

   } else if (strcmp(browse_type, "artists") == 0) {
      lib->browse_type = MUSIC_BROWSE_ARTISTS;
      lib->total_count = json_get_int(payload, "total_count");
      lib->offset = json_get_int(payload, "offset");
      struct json_object *arr;
      if (json_object_object_get_ex(payload, "artists", &arr) &&
          json_object_is_type(arr, json_type_array)) {
         int len = json_object_array_length(arr);
         if (len > MUSIC_MAX_RESULTS)
            len = MUSIC_MAX_RESULTS;
         lib->item_count = len;
         for (int i = 0; i < len; i++) {
            struct json_object *item = json_object_array_get_idx(arr, i);
            if (item) {
               snprintf(lib->items[i].name, MUSIC_MAX_TITLE, "%s", json_get_string(item, "name"));
               lib->items[i].track_count = json_get_int(item, "track_count");
               lib->items[i].album_count = json_get_int(item, "album_count");
            }
         }
      }

   } else if (strcmp(browse_type, "albums") == 0) {
      lib->browse_type = MUSIC_BROWSE_ALBUMS;
      lib->total_count = json_get_int(payload, "total_count");
      lib->offset = json_get_int(payload, "offset");
      struct json_object *arr;
      if (json_object_object_get_ex(payload, "albums", &arr) &&
          json_object_is_type(arr, json_type_array)) {
         int len = json_object_array_length(arr);
         if (len > MUSIC_MAX_RESULTS)
            len = MUSIC_MAX_RESULTS;
         lib->item_count = len;
         for (int i = 0; i < len; i++) {
            struct json_object *item = json_object_array_get_idx(arr, i);
            if (item) {
               snprintf(lib->items[i].name, MUSIC_MAX_TITLE, "%s", json_get_string(item, "name"));
               lib->items[i].track_count = json_get_int(item, "track_count");
            }
         }
      }
   }
}

/* =============================================================================
 * Send Helper
 * ============================================================================= */

static int send_json(ws_client_t *client, struct json_object *obj) {
   if (!client || !obj || !client->wsi) {
      return -1;
   }

   const char *str = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
   if (!str) {
      return -1;
   }

   size_t len = strlen(str);

   pthread_mutex_lock(&client->mutex);

   if (client->tx_count >= WS_TX_QUEUE_SIZE) {
      /* Queue full — drop oldest message to make room */
      LOG_WARNING("TX queue full, dropping oldest message");
      free(client->tx_queue[client->tx_head].data);
      client->tx_queue[client->tx_head].data = NULL;
      client->tx_head = (client->tx_head + 1) % WS_TX_QUEUE_SIZE;
      client->tx_count--;
   }

   client->tx_queue[client->tx_tail].data = strdup(str);
   client->tx_queue[client->tx_tail].len = len;
   client->tx_tail = (client->tx_tail + 1) % WS_TX_QUEUE_SIZE;
   client->tx_count++;

   pthread_mutex_unlock(&client->mutex);

   /* Request write callback and wake service thread.
    * lws_cancel_service is needed because lws_callback_on_writable may not
    * wake a blocked lws_service() call from a different thread on older lws. */
   lws_callback_on_writable(client->wsi);
   lws_cancel_service(client->lws_ctx);

   return 0;
}

/* =============================================================================
 * Public API - Lifecycle
 * ============================================================================= */

ws_client_t *ws_client_create(const char *host, uint16_t port, bool use_ssl, bool ssl_verify) {
   if (!host) {
      return NULL;
   }

   ws_client_t *client = calloc(1, sizeof(ws_client_t));
   if (!client) {
      return NULL;
   }

   strncpy(client->host, host, sizeof(client->host) - 1);
   client->port = port > 0 ? port : 8080;
   client->use_ssl = use_ssl;
   client->ssl_verify = ssl_verify;
   client->state = WS_STATE_DISCONNECTED;

   /* Allocate receive buffer */
   client->rx_capacity = WS_RX_BUFFER_SIZE;
   client->rx_buffer = malloc(client->rx_capacity);
   if (!client->rx_buffer) {
      free(client);
      return NULL;
   }

   pthread_mutex_init(&client->mutex, NULL);

   LOG_INFO("Client created for %s://%s:%u", use_ssl ? "wss" : "ws", host, client->port);
   return client;
}

void ws_client_destroy(ws_client_t *client) {
   if (!client) {
      return;
   }

   ws_client_disconnect(client);

   pthread_mutex_destroy(&client->mutex);

   free(client->rx_buffer);
   /* Free any queued TX messages */
   for (int i = 0; i < WS_TX_QUEUE_SIZE; i++) {
      free(client->tx_queue[i].data);
   }
   free(client);
}

/* =============================================================================
 * Background Service Thread
 * ============================================================================= */

#define WS_PING_INTERVAL_SEC 10

static void *ws_service_thread(void *arg) {
   ws_client_t *client = (ws_client_t *)arg;
   time_t last_ping = time(NULL);

   LOG_INFO("WebSocket service thread started");

   while (client->service_running && client->lws_ctx) {
      /* Service with 100ms timeout - this blocks but doesn't affect main voice loop */
      lws_service(client->lws_ctx, 100);

      /* Send application-level ping to keep connection alive during TTS playback */
      time_t now = time(NULL);
      if (now - last_ping >= WS_PING_INTERVAL_SEC && client->registered) {
         ws_client_ping(client);
         last_ping = now;
      }
   }

   LOG_INFO("WebSocket service thread stopped");
   return NULL;
}

/* =============================================================================
 * Public API - Connection
 * ============================================================================= */

int ws_client_connect(ws_client_t *client) {
   if (!client) {
      return -1;
   }

   if (client->state == WS_STATE_CONNECTED || client->state == WS_STATE_REGISTERED) {
      return 0; /* Already connected */
   }

   /* Create lws context */
   struct lws_context_creation_info ctx_info = { 0 };
   ctx_info.port = CONTEXT_PORT_NO_LISTEN;
   ctx_info.protocols = protocols;
   ctx_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
   ctx_info.user = client; /* Store client pointer for callback access */

   /* Disable TCP keepalive to prevent 1-second blocking in lws_service */
   ctx_info.ka_time = 0;
   ctx_info.ka_probes = 0;
   ctx_info.ka_interval = 0;
   ctx_info.timeout_secs = 0;

   client->lws_ctx = lws_create_context(&ctx_info);
   if (!client->lws_ctx) {
      LOG_ERROR("Failed to create lws context");
      return -1;
   }

   /* Connect to server */
   struct lws_client_connect_info conn_info = { 0 };
   conn_info.context = client->lws_ctx;
   conn_info.address = client->host;
   conn_info.port = client->port;
   conn_info.path = "/";
   conn_info.host = client->host;
   conn_info.origin = client->host;
   conn_info.protocol = protocols[0].name;

   if (client->use_ssl) {
      if (client->ssl_verify) {
         /* Production mode: enforce certificate validation */
         conn_info.ssl_connection = LCCSCF_USE_SSL;
         LOG_INFO("SSL enabled with certificate verification");
      } else {
         /* Development mode: skip verification (NOT FOR PRODUCTION) */
         conn_info.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED |
                                    LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
         LOG_ERROR("WARNING: SSL certificate verification DISABLED - NOT FOR PRODUCTION");
      }
   }

   client->state = WS_STATE_CONNECTING;

   client->wsi = lws_client_connect_via_info(&conn_info);
   if (!client->wsi) {
      LOG_ERROR("Failed to initiate connection");
      lws_context_destroy(client->lws_ctx);
      client->lws_ctx = NULL;
      client->state = WS_STATE_DISCONNECTED;
      return -1;
   }

   LOG_INFO("Connecting to %s:%u...", client->host, client->port);

   /* Wait for connection with timeout */
   int timeout_ms = 5000;
   int elapsed = 0;
   while (client->state == WS_STATE_CONNECTING && elapsed < timeout_ms) {
      lws_service(client->lws_ctx, 100);
      elapsed += 100;
   }

   if (client->state != WS_STATE_CONNECTED) {
      LOG_ERROR("Connection timeout or failed");
      lws_context_destroy(client->lws_ctx);
      client->lws_ctx = NULL;
      client->wsi = NULL;
      client->state = WS_STATE_DISCONNECTED;
      return -1;
   }

   /* Start background service thread */
   client->service_running = true;
   if (pthread_create(&client->service_thread, NULL, ws_service_thread, client) != 0) {
      LOG_ERROR("Failed to create WebSocket service thread");
      lws_context_destroy(client->lws_ctx);
      client->lws_ctx = NULL;
      client->wsi = NULL;
      client->state = WS_STATE_DISCONNECTED;
      return -1;
   }

   return 0;
}

void ws_client_disconnect(ws_client_t *client) {
   if (!client) {
      return;
   }

   /* Stop service thread first */
   if (client->service_running) {
      client->service_running = false;
      pthread_join(client->service_thread, NULL);
   }

   if (client->wsi) {
      /* This will trigger LWS_CALLBACK_CLIENT_CLOSED */
      client->wsi = NULL;
   }

   if (client->lws_ctx) {
      lws_context_destroy(client->lws_ctx);
      client->lws_ctx = NULL;
   }

   client->state = WS_STATE_DISCONNECTED;
   client->registered = false;
}

bool ws_client_is_connected(ws_client_t *client) {
   return client && (client->state == WS_STATE_CONNECTED || client->state == WS_STATE_REGISTERED);
}

ws_state_t ws_client_get_state(ws_client_t *client) {
   return client ? client->state : WS_STATE_DISCONNECTED;
}

/* =============================================================================
 * Public API - Registration
 * ============================================================================= */

int ws_client_register(ws_client_t *client,
                       const ws_identity_t *identity,
                       const ws_capabilities_t *caps) {
   if (!client || !identity || !caps) {
      return -1;
   }

   if (client->state != WS_STATE_CONNECTED) {
      LOG_ERROR("Not connected");
      return -1;
   }

   /* Store identity and caps */
   memcpy(&client->identity, identity, sizeof(ws_identity_t));
   memcpy(&client->caps, caps, sizeof(ws_capabilities_t));

   /* Build registration message */
   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "type", json_object_new_string("satellite_register"));

   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "uuid", json_object_new_string(identity->uuid));
   json_object_object_add(payload, "name", json_object_new_string(identity->name));
   json_object_object_add(payload, "location", json_object_new_string(identity->location));
   json_object_object_add(payload, "tier", json_object_new_int(1)); /* Tier 1 */

   /* Include reconnect_secret if we have one (for session reclamation)
    * SECURITY: This allows the server to verify we own this session */
   if (identity->reconnect_secret[0]) {
      json_object_object_add(payload, "reconnect_secret",
                             json_object_new_string(identity->reconnect_secret));
      LOG_INFO("Sending reconnect_secret for session reclamation");
   }

   struct json_object *caps_obj = json_object_new_object();
   json_object_object_add(caps_obj, "local_asr", json_object_new_boolean(caps->local_asr));
   json_object_object_add(caps_obj, "local_tts", json_object_new_boolean(caps->local_tts));
   json_object_object_add(caps_obj, "wake_word", json_object_new_boolean(caps->wake_word));
   json_object_object_add(payload, "capabilities", caps_obj);

   json_object_object_add(msg, "payload", payload);

   int ret = send_json(client, msg);
   json_object_put(msg);

   if (ret < 0) {
      LOG_ERROR("Failed to send registration");
      return -1;
   }

   LOG_INFO("Registration sent for '%s'", identity->name);

   /* Wait for registration response
    * NOTE: Don't call lws_service() here - the background service thread
    * is already doing that. Just poll the registered flag with a sleep. */
   int timeout_ms = 5000;
   int elapsed = 0;
   while (!client->registered && elapsed < timeout_ms && client->state == WS_STATE_CONNECTED) {
      usleep(50000); /* 50ms */
      elapsed += 50;
   }

   if (!client->registered) {
      LOG_ERROR("Registration timeout or failed");
      return -1;
   }

   /* Auto-subscribe to music state updates after successful registration */
   if (client->music_state_cb) {
      ws_client_send_music_subscribe(client);
   }

   return 0;
}

bool ws_client_is_registered(ws_client_t *client) {
   return client && client->registered;
}

/* =============================================================================
 * Public API - Query/Response
 * ============================================================================= */

int ws_client_send_query(ws_client_t *client, const char *text) {
   if (!client || !text) {
      return -1;
   }

   if (!client->registered) {
      LOG_ERROR("Not registered");
      return -1;
   }

   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "type", json_object_new_string("satellite_query"));

   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "text", json_object_new_string(text));
   json_object_object_add(msg, "payload", payload);

   int ret = send_json(client, msg);
   json_object_put(msg);

   if (ret < 0) {
      LOG_ERROR("Failed to send query");
      return -1;
   }

   LOG_INFO("Query sent: %.50s%s", text, strlen(text) > 50 ? "..." : "");
   return 0;
}

void ws_client_set_stream_callback(ws_client_t *client,
                                   ws_stream_callback_t callback,
                                   void *user_data) {
   if (client) {
      pthread_mutex_lock(&client->mutex);
      client->stream_cb = callback;
      client->stream_cb_data = user_data;
      pthread_mutex_unlock(&client->mutex);
   }
}

void ws_client_set_state_callback(ws_client_t *client,
                                  ws_state_callback_t callback,
                                  void *user_data) {
   if (client) {
      pthread_mutex_lock(&client->mutex);
      client->state_cb = callback;
      client->state_cb_data = user_data;
      pthread_mutex_unlock(&client->mutex);
   }
}

int ws_client_service(ws_client_t *client, int timeout_ms) {
   if (!client || !client->lws_ctx) {
      return -1;
   }

   return lws_service(client->lws_ctx, timeout_ms);
}

int ws_client_ping(ws_client_t *client) {
   if (!client || !client->wsi) {
      return -1;
   }

   /* Skip ping if the TX queue is nearly full — leave room for real messages */
   pthread_mutex_lock(&client->mutex);
   bool busy = (client->tx_count >= WS_TX_QUEUE_SIZE - 1);
   pthread_mutex_unlock(&client->mutex);
   if (busy) {
      return 0;
   }

   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "type", json_object_new_string("satellite_ping"));

   int ret = send_json(client, msg);
   json_object_put(msg);

   return ret;
}

/* =============================================================================
 * Public API - Utility
 * ============================================================================= */

void ws_client_generate_uuid(char *uuid) {
   if (!uuid) {
      return;
   }

   /* Simple UUID v4 generation using random numbers */
   srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

   snprintf(uuid, WS_CLIENT_UUID_SIZE, "%08x-%04x-%04x-%04x-%04x%08x", (unsigned int)rand(),
            (unsigned int)(rand() & 0xFFFF),
            (unsigned int)((rand() & 0x0FFF) | 0x4000), /* Version 4 */
            (unsigned int)((rand() & 0x3FFF) | 0x8000), /* Variant 1 */
            (unsigned int)(rand() & 0xFFFF), (unsigned int)rand());
}

const char *ws_client_get_error(ws_client_t *client) {
   return client && client->error_msg[0] ? client->error_msg : NULL;
}

const char *ws_client_get_reconnect_secret(ws_client_t *client) {
   if (!client || client->identity.reconnect_secret[0] == '\0') {
      return NULL;
   }
   return client->identity.reconnect_secret;
}

void ws_client_set_reconnect_secret(ws_client_t *client, const char *secret) {
   if (!client || !secret) {
      return;
   }

   pthread_mutex_lock(&client->mutex);
   strncpy(client->identity.reconnect_secret, secret,
           sizeof(client->identity.reconnect_secret) - 1);
   client->identity.reconnect_secret[sizeof(client->identity.reconnect_secret) - 1] = '\0';
   pthread_mutex_unlock(&client->mutex);
}

size_t ws_client_get_status_detail(ws_client_t *client, char *buf, size_t buf_size) {
   if (!client || !buf || buf_size == 0)
      return 0;

   pthread_mutex_lock(&client->mutex);
   size_t len = strlen(client->status_detail);
   if (len > 0) {
      size_t copy_len = len < buf_size - 1 ? len : buf_size - 1;
      memcpy(buf, client->status_detail, copy_len);
      buf[copy_len] = '\0';
      len = copy_len;
   } else {
      buf[0] = '\0';
   }
   pthread_mutex_unlock(&client->mutex);
   return len;
}

size_t ws_client_get_server_info(ws_client_t *client, char *buf, size_t buf_size) {
   if (!client || !buf || buf_size == 0)
      return 0;
   pthread_mutex_lock(&client->mutex);
   int n = snprintf(buf, buf_size, "%s:%u", client->host, client->port);
   pthread_mutex_unlock(&client->mutex);
   return (n > 0 && (size_t)n < buf_size) ? (size_t)n : 0;
}

time_t ws_client_get_connect_time(ws_client_t *client) {
   if (!client)
      return 0;
   pthread_mutex_lock(&client->mutex);
   time_t t = client->connected_at;
   pthread_mutex_unlock(&client->mutex);
   return t;
}

/* =============================================================================
 * Public API - Music
 * ============================================================================= */

void ws_client_set_music_callbacks(ws_client_t *client,
                                   ws_music_state_cb_t state_cb,
                                   ws_music_position_cb_t position_cb,
                                   ws_music_queue_cb_t queue_cb,
                                   ws_music_library_cb_t library_cb,
                                   void *user_data) {
   if (!client)
      return;
   pthread_mutex_lock(&client->mutex);
   client->music_state_cb = state_cb;
   client->music_position_cb = position_cb;
   client->music_queue_cb = queue_cb;
   client->music_library_cb = library_cb;
   client->music_cb_data = user_data;
   pthread_mutex_unlock(&client->mutex);
}

int ws_client_send_music_control(ws_client_t *client, const char *action, const char *path) {
   if (!client || !action || !client->registered)
      return -1;

   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "type", json_object_new_string("music_control"));

   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "action", json_object_new_string(action));
   if (path) {
      if (strcmp(action, "play_index") == 0) {
         /* play_index expects "index" as integer */
         json_object_object_add(payload, "index", json_object_new_int(atoi(path)));
      } else {
         json_object_object_add(payload, "path", json_object_new_string(path));
      }
   }
   json_object_object_add(msg, "payload", payload);

   int ret = send_json(client, msg);
   json_object_put(msg);

   LOG_INFO("Music control: %s", action);
   return ret;
}

int ws_client_send_music_seek(ws_client_t *client, float position_sec) {
   if (!client || !client->registered)
      return -1;

   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "type", json_object_new_string("music_control"));

   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "action", json_object_new_string("seek"));
   json_object_object_add(payload, "position_sec", json_object_new_double((double)position_sec));
   json_object_object_add(msg, "payload", payload);

   int ret = send_json(client, msg);
   json_object_put(msg);

   LOG_INFO("Music seek: %.1fs", position_sec);
   return ret;
}

int ws_client_send_music_library(ws_client_t *client, const char *type, const char *filter) {
   if (!client || !type || !client->registered)
      return -1;

   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "type", json_object_new_string("music_library"));

   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "type", json_object_new_string(type));
   if (filter) {
      /* Determine filter key based on browse type */
      if (strcmp(type, "tracks_by_artist") == 0) {
         json_object_object_add(payload, "artist", json_object_new_string(filter));
      } else if (strcmp(type, "tracks_by_album") == 0) {
         json_object_object_add(payload, "album", json_object_new_string(filter));
      }
   }
   json_object_object_add(msg, "payload", payload);

   int ret = send_json(client, msg);
   json_object_put(msg);

   LOG_INFO("Music library browse: %s", type);
   return ret;
}

int ws_client_send_music_library_paged(ws_client_t *client,
                                       const char *type,
                                       const char *filter,
                                       int offset,
                                       int limit) {
   if (!client || !type || !client->registered)
      return -1;

   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "type", json_object_new_string("music_library"));

   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "type", json_object_new_string(type));
   json_object_object_add(payload, "offset", json_object_new_int(offset));
   json_object_object_add(payload, "limit", json_object_new_int(limit));
   if (filter) {
      if (strcmp(type, "tracks_by_artist") == 0) {
         json_object_object_add(payload, "artist", json_object_new_string(filter));
      } else if (strcmp(type, "tracks_by_album") == 0) {
         json_object_object_add(payload, "album", json_object_new_string(filter));
      }
   }
   json_object_object_add(msg, "payload", payload);

   int ret = send_json(client, msg);
   json_object_put(msg);

   LOG_INFO("Music library browse: %s (offset=%d limit=%d)", type, offset, limit);
   return ret;
}

int ws_client_send_music_queue(ws_client_t *client,
                               const char *action,
                               const char *path,
                               int index) {
   if (!client || !action || !client->registered)
      return -1;

   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "type", json_object_new_string("music_queue"));

   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "action", json_object_new_string(action));
   if (path) {
      json_object_object_add(payload, "path", json_object_new_string(path));
   }
   if (index >= 0) {
      json_object_object_add(payload, "index", json_object_new_int(index));
   }
   json_object_object_add(msg, "payload", payload);

   int ret = send_json(client, msg);
   json_object_put(msg);

   LOG_INFO("Music queue: %s", action);
   return ret;
}

int ws_client_send_music_queue_bulk(ws_client_t *client, const char *action, const char *name) {
   if (!client || !action || !name || !client->registered)
      return -1;

   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "type", json_object_new_string("music_control"));

   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "action", json_object_new_string(action));

   /* add_artist expects "artist", add_album expects "album" */
   const char *key = (strcmp(action, "add_artist") == 0) ? "artist" : "album";
   json_object_object_add(payload, key, json_object_new_string(name));
   json_object_object_add(msg, "payload", payload);

   int ret = send_json(client, msg);
   json_object_put(msg);

   LOG_INFO("Music queue: %s '%s'", action, name);
   return ret;
}

const char *ws_client_get_session_token(ws_client_t *client) {
   if (!client || client->session_token[0] == '\0') {
      return NULL;
   }
   return client->session_token;
}

void ws_client_set_music_playback(ws_client_t *client, void *music_pb) {
   if (!client)
      return;
#ifdef HAVE_OPUS
   client->music_pb = (music_playback_t *)music_pb;
#else
   (void)music_pb;
#endif
}

int ws_client_send_music_subscribe(ws_client_t *client) {
   if (!client || !client->registered)
      return -1;

   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "type", json_object_new_string("music_subscribe"));

   int ret = send_json(client, msg);
   json_object_put(msg);

   LOG_INFO("Music subscribe sent");
   return ret;
}
