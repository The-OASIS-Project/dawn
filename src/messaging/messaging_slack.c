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
 * Slack Socket Mode driver — DM-only, text-only.
 *
 * Inbound:  Two-step open — POST apps.connections.open returns a
 *           one-time WSS URL; libwebsockets client connects there.  A
 *           single listener thread runs lws_service() + drives the
 *           reconnect state machine.  Slack does not use Discord's
 *           sequence-based resume — each disconnect requires a fresh
 *           apps.connections.open call.
 * Outbound: REST POST chat.postMessage via libcurl on a separate CURL
 *           handle protected by its own mutex.
 *
 * Lifecycle:
 *   apps.connections.open (xapp-) → WSS URL
 *   CONNECT → CLIENT_ESTABLISHED →
 *   HELLO (envelope type=hello) → connection live
 *   EVENT (envelope type=events_api) → ack {"envelope_id":"..."} → dispatch
 *   DISCONNECT (envelope type=disconnect, reason=warning|link_disabled) →
 *     close + fresh apps.connections.open + reconnect.
 *
 * Token redaction: both tokens are sent only in Authorization headers
 * (Bearer xapp-... for apps.connections.open, Bearer xoxb-... for
 * chat.postMessage).  Tokens never appear in URLs or logged content.
 *
 * See docs/MESSAGING_CHANNELS_DESIGN.md §8 (Slack).
 */
#include "messaging/messaging_slack.h"

#include <ctype.h>
#include <curl/curl.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <sodium.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/curl_buffer.h"
#include "core/ws_reconnect.h"
#include "dawn_error.h"
#include "logging.h"
#include "messaging/messaging_driver.h"
#include "messaging/messaging_engine.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

/* xapp-/xoxb- tokens are ~80-90 chars today; 256-byte cap defends
 * against silent mid-token truncation if Slack ever issues longer ones. */
#define SK_TOKEN_MAX 256
#define SK_API_OPEN_URL "https://slack.com/api/apps.connections.open"
#define SK_API_POST_MESSAGE_URL "https://slack.com/api/chat.postMessage"
#define SK_USER_AGENT "DAWN-Slack/0.1 (libcurl, libwebsockets)"
#define SK_LISTENER_STACK (64 * 1024)
#define SK_RX_BUF_INITIAL 4096
#define SK_RX_BUF_MAX (1 * 1024 * 1024)
#define SK_TX_BUF_MAX 4096
/* Service tick — same rationale as Discord: 20 Hz wakes at idle for
 * shutdown responsiveness without burdening the ASR/LLM hot paths. */
#define SK_SERVICE_TIMEOUT_MS 50
/* Max ack JSON payload — `{"envelope_id":"..."}` with a ~64-byte UUID
 * is well under 200 bytes; keep the cap modest. */
#define SK_ACK_BUF_SIZE 256

/* lws callback returns -1 to close the connection — this is the lws API
 * contract, NOT DAWN's SUCCESS/FAILURE convention.  Mirrors the constant
 * in messaging_discord.c. */
#define LWS_CLOSE_CONNECTION (-1)

/* Bounded scratch for the WSS URL parsed out of apps.connections.open. */
#define SK_WSS_URL_MAX 512
#define SK_WSS_HOST_MAX 128
#define SK_WSS_PATH_MAX 384

/* Outbound ack queue.  Slack expects an ack envelope per inbound event;
 * if multiple events arrive faster than the writeable callback can
 * drain them, we queue them in a bounded FIFO. */
#define SK_ACK_QUEUE_DEPTH 16

/* =============================================================================
 * State
 * ============================================================================= */

static char s_app_token[SK_TOKEN_MAX];
static char s_bot_token[SK_TOKEN_MAX];

static atomic_bool s_running = ATOMIC_VAR_INIT(false);
static atomic_bool s_connected = ATOMIC_VAR_INIT(false);

/* Deferred-close signal.  Set when something inside an lws callback (or
 * an off-thread driver->reconnect() caller) decides we should close.
 * The listener loop drains the flag with atomic_exchange and calls
 * close_active_connection() — lws_set_timeout(LWS_TO_KILL_SYNC) — only
 * from OUTSIDE the callback dispatch chain.  See messaging_discord.c
 * s_reconnect_requested for the full use-after-free rationale. */
static atomic_bool s_reconnect_requested = ATOMIC_VAR_INIT(false);

static pthread_t s_listener_thread;
static bool s_listener_started = false;

static messaging_inbound_fn s_inbound_cb = NULL;
static pthread_mutex_t s_inbound_cb_mutex = PTHREAD_MUTEX_INITIALIZER;

/* REST handle — used by worker threads for outbound sends. */
static CURL *s_send_curl = NULL;
static pthread_mutex_t s_send_curl_mutex = PTHREAD_MUTEX_INITIALIZER;

/* lws context + active wsi — owned by listener thread only. */
static struct lws_context *s_lws_context = NULL;
static struct lws *s_wsi = NULL;

/* Reconnect state — backoff timing only (no session resume on Slack). */
static ws_reconnect_state_t s_reconnect;

/* WSS URL components — parsed from apps.connections.open between
 * connects.  Owned by the listener thread. */
static char s_wss_host[SK_WSS_HOST_MAX];
static char s_wss_path[SK_WSS_PATH_MAX];
static int s_wss_port = 443;

/* RX accumulator for fragmented WS frames.  Owned by listener thread. */
static char *s_rx_buf = NULL;
static size_t s_rx_buf_len = 0;
static size_t s_rx_buf_cap = 0;

/* Outbound ack FIFO.  Slack acks are tiny JSON blobs (`{"envelope_id":
 * "..."}`).  Producer: handle_event (called from CLIENT_RECEIVE on the
 * listener thread).  Consumer: CLIENT_WRITEABLE on the same thread.
 * Both run on the listener thread synchronously inside lws_service so
 * no locking is required, but we keep the structure as a defensive
 * bounded FIFO to prevent unbounded growth if Slack ever floods us. */
typedef struct {
   char data[SK_ACK_BUF_SIZE];
   size_t len;
} sk_ack_t;

static sk_ack_t s_ack_queue[SK_ACK_QUEUE_DEPTH];
static int s_ack_head = 0;
static int s_ack_tail = 0;
static int s_ack_count = 0;

/* Single LWS_PRE-prefixed scratch for the next outbound frame. */
static unsigned char s_tx_buf[LWS_PRE + SK_TX_BUF_MAX];

/* =============================================================================
 * Internal helpers
 * ============================================================================= */

static void rx_buf_reset(void) {
   s_rx_buf_len = 0;
}

static int rx_buf_append(const void *data, size_t len) {
   if (s_rx_buf_len + len > SK_RX_BUF_MAX) {
      OLOG_WARNING("slack: rx buffer would exceed %d bytes; dropping frame", SK_RX_BUF_MAX);
      rx_buf_reset();
      return FAILURE;
   }
   if (s_rx_buf_len + len + 1 > s_rx_buf_cap) {
      size_t new_cap = s_rx_buf_cap ? s_rx_buf_cap * 2 : SK_RX_BUF_INITIAL;
      while (new_cap < s_rx_buf_len + len + 1) {
         new_cap *= 2;
      }
      if (new_cap > SK_RX_BUF_MAX) {
         new_cap = SK_RX_BUF_MAX;
      }
      char *nb = realloc(s_rx_buf, new_cap);
      if (!nb) {
         OLOG_ERROR("slack: rx buffer realloc failed");
         return FAILURE;
      }
      s_rx_buf = nb;
      s_rx_buf_cap = new_cap;
   }
   memcpy(s_rx_buf + s_rx_buf_len, data, len);
   s_rx_buf_len += len;
   s_rx_buf[s_rx_buf_len] = '\0';
   return SUCCESS;
}

static void ack_queue_reset(void) {
   s_ack_head = 0;
   s_ack_tail = 0;
   s_ack_count = 0;
}

static int ack_queue_push(const char *envelope_id) {
   if (s_ack_count >= SK_ACK_QUEUE_DEPTH) {
      OLOG_WARNING("slack: ack queue full (%d entries); dropping ack — Slack will redeliver",
                   SK_ACK_QUEUE_DEPTH);
      return FAILURE;
   }
   sk_ack_t *slot = &s_ack_queue[s_ack_tail];
   /* Slack envelope_ids are UUIDv4-shaped; defensively bound them. */
   int n = snprintf(slot->data, sizeof(slot->data), "{\"envelope_id\":\"%s\"}", envelope_id);
   if (n <= 0 || (size_t)n >= sizeof(slot->data)) {
      return FAILURE;
   }
   slot->len = (size_t)n;
   s_ack_tail = (s_ack_tail + 1) % SK_ACK_QUEUE_DEPTH;
   s_ack_count++;
   if (s_wsi) {
      lws_callback_on_writable(s_wsi);
   }
   return SUCCESS;
}

static const sk_ack_t *ack_queue_peek(void) {
   if (s_ack_count == 0) {
      return NULL;
   }
   return &s_ack_queue[s_ack_head];
}

static void ack_queue_pop(void) {
   if (s_ack_count == 0) {
      return;
   }
   s_ack_head = (s_ack_head + 1) % SK_ACK_QUEUE_DEPTH;
   s_ack_count--;
}

/* =============================================================================
 * Two-step open: POST apps.connections.open → returns WSS URL
 * ============================================================================= */

static int parse_wss_url(const char *url) {
   /* Slack returns wss://wss-primary.slack.com/link/?ticket=...&app_id=...
    * Parse host/path ourselves — keeps the dependency surface minimal
    * (lws_parse_uri exists but its API drift across versions is more
    * brittle than this 15-line strchr walk). */
   if (!url) {
      return FAILURE;
   }
   const char *p = url;
   if (strncmp(p, "wss://", 6) == 0) {
      p += 6;
      s_wss_port = 443;
   } else if (strncmp(p, "ws://", 5) == 0) {
      p += 5;
      s_wss_port = 80; /* Slack never returns this in production */
   } else {
      OLOG_WARNING("slack: apps.connections.open returned unexpected scheme");
      return FAILURE;
   }
   /* Host runs to first '/' or '?'.  Reject empty host. */
   size_t hlen = strcspn(p, "/?");
   if (hlen == 0 || hlen >= sizeof(s_wss_host)) {
      OLOG_WARNING("slack: apps.connections.open host too long or empty (%zu chars)", hlen);
      return FAILURE;
   }
   memcpy(s_wss_host, p, hlen);
   s_wss_host[hlen] = '\0';

   /* Defense in depth: only honor *.slack.com WSS hosts.  Slack's
    * production Socket Mode endpoints are wss-primary.slack.com and
    * wss-backup.slack.com.  The TLS cert SAN check (lws verifies the
    * cert chain against the dialed hostname) is the practical gate,
    * but suffix allowlisting means a compromised apps.connections.open
    * response couldn't redirect us to an attacker-controlled host even
    * if the dialed cert somehow validated. */
   static const char allowed_suffix[] = ".slack.com";
   const size_t suffix_len = sizeof(allowed_suffix) - 1;
   if (hlen < suffix_len || strcmp(s_wss_host + hlen - suffix_len, allowed_suffix) != 0) {
      OLOG_WARNING("slack: apps.connections.open host '%s' not under .slack.com; rejecting",
                   s_wss_host);
      return FAILURE;
   }

   /* Path: everything from '/' onward, including the query string.
    * Slack's WSS URL includes a one-time ticket in the query — preserve
    * it verbatim. */
   p += hlen;
   if (*p == '\0') {
      snprintf(s_wss_path, sizeof(s_wss_path), "/");
   } else {
      if (strlen(p) >= sizeof(s_wss_path)) {
         OLOG_WARNING("slack: apps.connections.open path too long");
         return FAILURE;
      }
      snprintf(s_wss_path, sizeof(s_wss_path), "%s", p);
   }
   return SUCCESS;
}

static int open_socket_mode(void) {
   if (s_app_token[0] == '\0') {
      return FAILURE;
   }
   curl_buffer_t resp;
   curl_buffer_init(&resp);
   int rc = FAILURE;

   CURL *curl = curl_easy_init();
   if (!curl) {
      curl_buffer_free(&resp);
      return FAILURE;
   }
   char auth_header[SK_TOKEN_MAX + 32];
   snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", s_app_token);

   struct curl_slist *hdrs = NULL;
   hdrs = curl_slist_append(hdrs, "Content-Type: application/x-www-form-urlencoded; charset=utf-8");
   hdrs = curl_slist_append(hdrs, auth_header);

   curl_easy_setopt(curl, CURLOPT_URL, SK_API_OPEN_URL);
   curl_easy_setopt(curl, CURLOPT_POST, 1L);
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
   curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
   curl_apply_dawn_defaults(curl, SK_USER_AGENT, 30L, &resp);

   CURLcode cc = curl_easy_perform(curl);
   long http_status = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
   curl_slist_free_all(hdrs);
   curl_easy_cleanup(curl);

   if (cc != CURLE_OK || http_status < 200 || http_status >= 300) {
      OLOG_WARNING("slack: apps.connections.open failed (curl=%d http=%ld)", cc, http_status);
      curl_buffer_free(&resp);
      return FAILURE;
   }

   struct json_object *body = json_tokener_parse(resp.data ? resp.data : "");
   curl_buffer_free(&resp);
   if (!body) {
      OLOG_WARNING("slack: apps.connections.open returned unparsable JSON");
      return FAILURE;
   }
   struct json_object *ok_obj = NULL;
   struct json_object *url_obj = NULL;
   bool ok_field = false;
   if (json_object_object_get_ex(body, "ok", &ok_obj) && ok_obj) {
      ok_field = json_object_get_boolean(ok_obj);
   }
   if (ok_field && json_object_object_get_ex(body, "url", &url_obj) && url_obj) {
      const char *url = json_object_get_string(url_obj);
      if (url && url[0]) {
         rc = parse_wss_url(url);
      }
   } else {
      /* Surface Slack's error code (e.g., "invalid_auth", "not_allowed_token_type")
       * so misconfigured tokens give an actionable log line. */
      struct json_object *err_obj = NULL;
      const char *err = "(no error field)";
      if (json_object_object_get_ex(body, "error", &err_obj) && err_obj) {
         err = json_object_get_string(err_obj);
      }
      OLOG_WARNING("slack: apps.connections.open returned ok=false (error=%s)", err);
   }
   json_object_put(body);
   return rc;
}

/* =============================================================================
 * Envelope dispatch
 * ============================================================================= */

static void dispatch_to_engine(const char *channel_id,
                               const char *user_id,
                               const char *body_text,
                               int64_t timestamp) {
   messaging_inbound_fn cb = NULL;
   pthread_mutex_lock(&s_inbound_cb_mutex);
   cb = s_inbound_cb;
   pthread_mutex_unlock(&s_inbound_cb_mutex);
   if (cb) {
      /* DMs only (filtered by the caller): one person per chat. */
      messaging_inbound_t in = {
         .provider = "slack",
         .provider_address = channel_id,
         .sender_id = user_id,
         .sender_display = user_id,
         .body = body_text,
         .timestamp = timestamp,
         .chat_kind = MESSAGING_CHAT_ONE_TO_ONE,
      };
      cb(&in);
   }
}

static void handle_event_envelope(struct json_object *root) {
   /* Pluck envelope_id for ack — required even if we drop the event. */
   struct json_object *eid_obj = NULL;
   const char *envelope_id = NULL;
   if (json_object_object_get_ex(root, "envelope_id", &eid_obj) && eid_obj) {
      envelope_id = json_object_get_string(eid_obj);
   }

   /* Payload contains the Events API event body. */
   struct json_object *payload = NULL;
   struct json_object *event = NULL;
   if (!json_object_object_get_ex(root, "payload", &payload) || !payload) {
      goto ack;
   }
   if (!json_object_object_get_ex(payload, "event", &event) || !event) {
      goto ack;
   }

   /* Only handle message events.  app_mention is delivered separately
    * but we'd want the same dispatch path; defer to v2 along with guild
    * mode. */
   struct json_object *type_obj = NULL;
   const char *evt_type = NULL;
   if (json_object_object_get_ex(event, "type", &type_obj) && type_obj) {
      evt_type = json_object_get_string(type_obj);
   }
   if (!evt_type || strcmp(evt_type, "message") != 0) {
      goto ack;
   }

   /* Filter: skip if subtype is set (bot_message, channel_join, etc. —
    * v1 only handles plain user-typed messages). */
   struct json_object *subtype_obj = NULL;
   if (json_object_object_get_ex(event, "subtype", &subtype_obj) && subtype_obj) {
      goto ack;
   }

   /* DM filter: Slack DM channel IDs start with 'D'.  Skip non-DM v1. */
   struct json_object *channel_obj = NULL;
   const char *channel_id = NULL;
   if (json_object_object_get_ex(event, "channel", &channel_obj) && channel_obj) {
      channel_id = json_object_get_string(channel_obj);
   }
   if (!channel_id || channel_id[0] != 'D') {
      goto ack;
   }
   /* Slack names the channel's kind; only a direct message ("im") holds
    * one person.  Older payloads without the field fall back on the 'D'. */
   struct json_object *ctype_obj = NULL;
   if (json_object_object_get_ex(event, "channel_type", &ctype_obj) && ctype_obj &&
       strcmp(json_object_get_string(ctype_obj), "im") != 0) {
      goto ack;
   }

   /* Bot filter: skip messages from bot_id (don't echo ourselves). */
   struct json_object *bot_id_obj = NULL;
   if (json_object_object_get_ex(event, "bot_id", &bot_id_obj) && bot_id_obj) {
      goto ack;
   }

   struct json_object *text_obj = NULL;
   const char *body_text = NULL;
   if (json_object_object_get_ex(event, "text", &text_obj) && text_obj) {
      body_text = json_object_get_string(text_obj);
   }
   if (!body_text) {
      goto ack;
   }

   /* User who sent the message (Slack user ID like "U03ABCD"). */
   struct json_object *user_obj = NULL;
   const char *user_id_str = NULL;
   if (json_object_object_get_ex(event, "user", &user_obj) && user_obj) {
      user_id_str = json_object_get_string(user_obj);
   }

   /* ts is a Slack-format timestamp string "1234567890.123456".  Take
    * the integer part. */
   int64_t timestamp = 0;
   struct json_object *ts_obj = NULL;
   if (json_object_object_get_ex(event, "ts", &ts_obj) && ts_obj) {
      const char *ts_str = json_object_get_string(ts_obj);
      if (ts_str) {
         timestamp = (int64_t)strtoll(ts_str, NULL, 10);
      }
   }

   /* The engine answers only the person who linked the channel, by id. */
   if (!user_id_str || !user_id_str[0]) {
      goto ack;
   }
   dispatch_to_engine(channel_id, user_id_str, body_text, timestamp);

ack:
   if (envelope_id && envelope_id[0]) {
      ack_queue_push(envelope_id);
   }
}

static void handle_slack_frame(const char *json_text, size_t len) {
   (void)len;
   struct json_object *root = json_tokener_parse(json_text);
   if (!root) {
      OLOG_WARNING("slack: failed to parse envelope");
      return;
   }
   struct json_object *type_obj = NULL;
   if (!json_object_object_get_ex(root, "type", &type_obj) || !type_obj) {
      json_object_put(root);
      return;
   }
   const char *type = json_object_get_string(type_obj);
   if (!type) {
      json_object_put(root);
      return;
   }
   if (strcmp(type, "hello") == 0) {
      OLOG_INFO("slack: Socket Mode HELLO received");
      atomic_store(&s_connected, true);
      ws_reconnect_record_success(&s_reconnect, NULL, NULL);
   } else if (strcmp(type, "events_api") == 0) {
      handle_event_envelope(root);
   } else if (strcmp(type, "disconnect") == 0) {
      struct json_object *reason_obj = NULL;
      const char *reason = "(unspecified)";
      if (json_object_object_get_ex(root, "reason", &reason_obj) && reason_obj) {
         reason = json_object_get_string(reason_obj);
      }
      OLOG_INFO("slack: server requested DISCONNECT (reason=%s)", reason ? reason : "(null)");
      /* Defer close to listener thread to avoid use-after-free on the
       * outer CLIENT_RECEIVE dispatcher (same pattern as Discord). */
      atomic_store(&s_reconnect_requested, true);
   }
   /* Other envelope types (slash_commands, interactive) ignored in v1. */
   json_object_put(root);
}

/* =============================================================================
 * libwebsockets callback
 * ============================================================================= */

static int sk_lws_callback(struct lws *wsi,
                           enum lws_callback_reasons reason,
                           void *user,
                           void *in,
                           size_t len) {
   (void)user;

   switch (reason) {
      case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
         OLOG_WARNING("slack: socket-mode connection error: %s",
                      in ? (const char *)in : "(no detail)");
         atomic_store(&s_connected, false);
         s_wsi = NULL;
         return LWS_CLOSE_CONNECTION;

      case LWS_CALLBACK_CLIENT_ESTABLISHED:
         OLOG_INFO("slack: Socket Mode WebSocket established");
         s_wsi = wsi;
         /* Wait for HELLO envelope before declaring ready. */
         break;

      case LWS_CALLBACK_CLIENT_RECEIVE: {
         if (in && len > 0) {
            if (rx_buf_append(in, len) != SUCCESS) {
               return LWS_CLOSE_CONNECTION;
            }
         }
         if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
            if (s_rx_buf && s_rx_buf_len > 0) {
               handle_slack_frame(s_rx_buf, s_rx_buf_len);
            }
            rx_buf_reset();
         }
         break;
      }

      case LWS_CALLBACK_CLIENT_WRITEABLE: {
         const sk_ack_t *ack = ack_queue_peek();
         if (!ack) {
            break;
         }
         if (ack->len > SK_TX_BUF_MAX) {
            /* Shouldn't happen — ack_queue_push uses snprintf with
             * SK_ACK_BUF_SIZE which is smaller — but guard anyway. */
            ack_queue_pop();
            break;
         }
         memcpy(s_tx_buf + LWS_PRE, ack->data, ack->len);
         int n = lws_write(wsi, s_tx_buf + LWS_PRE, ack->len, LWS_WRITE_TEXT);
         if (n < 0 || (size_t)n < ack->len) {
            OLOG_WARNING("slack: lws_write ack failed (wrote=%d of %zu)", n, ack->len);
            return LWS_CLOSE_CONNECTION;
         }
         ack_queue_pop();
         if (ack_queue_peek()) {
            lws_callback_on_writable(wsi);
         }
         break;
      }

      case LWS_CALLBACK_CLIENT_CLOSED:
         OLOG_INFO("slack: Socket Mode WebSocket closed");
         atomic_store(&s_connected, false);
         s_wsi = NULL;
         rx_buf_reset();
         ack_queue_reset();
         break;

      default:
         break;
   }
   return 0;
}

static const struct lws_protocols s_sk_protocols[] = {
   { "slack-socket-mode", sk_lws_callback, 0, SK_RX_BUF_INITIAL, 0, NULL, 0 },
   { NULL, NULL, 0, 0, 0, NULL, 0 },
};

/* =============================================================================
 * Listener thread
 * ============================================================================= */

static void interruptible_sleep_seconds(int seconds) {
   const int chunk_ms = 250;
   int total_ms = seconds * 1000;
   while (total_ms > 0 && atomic_load(&s_running)) {
      int s = (total_ms > chunk_ms) ? chunk_ms : total_ms;
      struct timespec ts = { .tv_sec = s / 1000, .tv_nsec = (s % 1000) * 1000000L };
      nanosleep(&ts, NULL);
      total_ms -= s;
   }
}

static int attempt_connect(void) {
   struct lws_client_connect_info i;
   memset(&i, 0, sizeof(i));
   i.context = s_lws_context;
   i.address = s_wss_host;
   i.host = i.address;
   i.origin = i.address;
   i.port = s_wss_port;
   i.path = s_wss_path;
   i.ssl_connection = LCCSCF_USE_SSL;
   i.protocol = s_sk_protocols[0].name;
   i.pwsi = &s_wsi;

   /* Path contains a one-time ticket — log host + port only. */
   OLOG_INFO("slack: connecting to wss://%s:%d", i.address, i.port);
   struct lws *w = lws_client_connect_via_info(&i);
   if (!w) {
      OLOG_WARNING("slack: lws_client_connect_via_info failed");
      return FAILURE;
   }
   return SUCCESS;
}

static void close_active_connection(void) {
   if (s_wsi) {
      lws_set_timeout(s_wsi, PENDING_TIMEOUT_USER_OK, LWS_TO_KILL_SYNC);
   }
}

static void *sk_listener_thread(void *arg) {
   (void)arg;
   OLOG_INFO("slack: listener thread started");

   while (atomic_load(&s_running)) {
      if (!s_wsi) {
         atomic_store(&s_connected, false);
         rx_buf_reset();
         ack_queue_reset();

         int delay = ws_reconnect_next_delay_seconds(&s_reconnect);
         if (delay > 0) {
            interruptible_sleep_seconds(delay);
         }
         if (!atomic_load(&s_running)) {
            break;
         }

         /* Slack requires a fresh apps.connections.open every time —
          * the WSS URL is one-time, no resume token. */
         if (open_socket_mode() != SUCCESS) {
            OLOG_WARNING("slack: apps.connections.open failed; will retry after backoff");
            continue;
         }
         if (attempt_connect() != SUCCESS) {
            continue;
         }
      }

      if (atomic_exchange(&s_reconnect_requested, false)) {
         OLOG_INFO("slack: external reconnect requested");
         close_active_connection();
      }

      int n = lws_service(s_lws_context, SK_SERVICE_TIMEOUT_MS);
      if (n < 0) {
         OLOG_WARNING("slack: lws_service returned %d; closing", n);
         close_active_connection();
      }
   }

   close_active_connection();
   while (s_wsi) {
      lws_service(s_lws_context, 50);
   }

   atomic_store(&s_connected, false);
   OLOG_INFO("slack: listener thread exiting");
   return NULL;
}

/* =============================================================================
 * Outbound REST send (chat.postMessage)
 * ============================================================================= */

static int sk_extract_channel_id(const char *address_json, char *out, size_t out_size) {
   if (!address_json || !out || out_size == 0) {
      return FAILURE;
   }
   struct json_object *obj = json_tokener_parse(address_json);
   if (!obj) {
      return FAILURE;
   }
   struct json_object *cid = NULL;
   int rc = FAILURE;
   if (json_object_object_get_ex(obj, "channel_id", &cid) && cid) {
      const char *s = json_object_get_string(cid);
      if (s && s[0]) {
         snprintf(out, out_size, "%s", s);
         rc = SUCCESS;
      }
   }
   json_object_put(obj);
   return rc;
}

/* Slack channel IDs are 'D'/'C'/'G' followed by uppercase alphanumerics.
 * v1 is DM-only so we accept 'D...'; the others are allowed at the
 * validate_address layer to keep the door open for v2 guild mode.  This
 * is also a URL-path defense — chat.postMessage uses the channel ID in
 * the JSON body, not the URL path, but the same defensive shape rejects
 * any '/', '?', or whitespace before it can reach the wire. */
static bool is_valid_slack_channel(const char *s) {
   if (!s || !s[0]) {
      return false;
   }
   char prefix = s[0];
   if (prefix != 'D' && prefix != 'C' && prefix != 'G') {
      return false;
   }
   for (size_t i = 1; s[i] != '\0'; i++) {
      char c = s[i];
      if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) {
         return false;
      }
   }
   return true;
}

static int sk_send_text(int user_id,
                        const char *provider_address,
                        const char *address_json,
                        const char *text) {
   /* xoxb is bot-scoped, not per-user.  user_id plumbed for contract
    * uniformity (SMS uses it for per-user audit). */
   (void)user_id;
   if (!text) {
      return FAILURE;
   }
   if (s_bot_token[0] == '\0') {
      return FAILURE;
   }

   char channel_id[64];
   channel_id[0] = '\0';
   if (provider_address && provider_address[0] != '\0') {
      snprintf(channel_id, sizeof(channel_id), "%s", provider_address);
   } else if (address_json) {
      if (sk_extract_channel_id(address_json, channel_id, sizeof(channel_id)) != SUCCESS) {
         OLOG_WARNING("slack: send failed — no channel_id in provider_address or address_json");
         return FAILURE;
      }
   } else {
      OLOG_WARNING("slack: send failed — no provider_address or address_json");
      return FAILURE;
   }
   if (!is_valid_slack_channel(channel_id)) {
      OLOG_WARNING("slack: send failed — channel_id not a valid Slack channel shape");
      return FAILURE;
   }

   /* Build JSON body: {"channel":"<id>","text":"<text>"}.  Slack caps
    * text at 40000 chars per message; the engine's outbound shaping is
    * expected to keep us well under that. */
   struct json_object *body = json_object_new_object();
   if (!body) {
      return FAILURE;
   }
   json_object_object_add(body, "channel", json_object_new_string(channel_id));
   json_object_object_add(body, "text", json_object_new_string(text));
   const char *body_str = json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN);

   char auth_header[SK_TOKEN_MAX + 32];
   snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", s_bot_token);

   curl_buffer_t resp;
   curl_buffer_init(&resp);
   int rc = FAILURE;

   pthread_mutex_lock(&s_send_curl_mutex);
   if (!s_send_curl) {
      s_send_curl = curl_easy_init();
   }
   if (s_send_curl) {
      curl_easy_reset(s_send_curl);
      curl_easy_setopt(s_send_curl, CURLOPT_URL, SK_API_POST_MESSAGE_URL);
      curl_easy_setopt(s_send_curl, CURLOPT_POST, 1L);
      curl_easy_setopt(s_send_curl, CURLOPT_POSTFIELDS, body_str);
      curl_apply_dawn_defaults(s_send_curl, SK_USER_AGENT, MESSAGING_SEND_TIMEOUT_SEC, &resp);

      struct curl_slist *hdrs = NULL;
      hdrs = curl_slist_append(hdrs, "Content-Type: application/json; charset=utf-8");
      hdrs = curl_slist_append(hdrs, auth_header);
      curl_easy_setopt(s_send_curl, CURLOPT_HTTPHEADER, hdrs);

      CURLcode cc = curl_easy_perform(s_send_curl);
      long http_status = 0;
      curl_easy_getinfo(s_send_curl, CURLINFO_RESPONSE_CODE, &http_status);
      curl_slist_free_all(hdrs);

      /* Slack returns HTTP 200 even for app-layer errors; check the
       * `ok` field. */
      bool ok = false;
      const char *slack_err = NULL;
      struct json_object *resp_obj = NULL;
      if (cc == CURLE_OK && http_status >= 200 && http_status < 300 && resp.data) {
         resp_obj = json_tokener_parse(resp.data);
         if (resp_obj) {
            struct json_object *ok_obj = NULL;
            if (json_object_object_get_ex(resp_obj, "ok", &ok_obj) && ok_obj) {
               ok = json_object_get_boolean(ok_obj);
            }
            if (!ok) {
               struct json_object *err_obj = NULL;
               if (json_object_object_get_ex(resp_obj, "error", &err_obj) && err_obj) {
                  slack_err = json_object_get_string(err_obj);
               }
            }
         }
      }
      if (ok) {
         rc = SUCCESS;
      } else {
         OLOG_WARNING("slack: chat.postMessage failed (curl=%d http=%ld slack_err=%s)", cc,
                      http_status, slack_err ? slack_err : "(none)");
      }
      if (resp_obj) {
         json_object_put(resp_obj);
      }
   }
   pthread_mutex_unlock(&s_send_curl_mutex);

   curl_buffer_free(&resp);
   json_object_put(body);
   return rc;
}

/* =============================================================================
 * Driver hooks
 * ============================================================================= */

static int sk_init(const char *credentials_json) {
   char app_token[SK_TOKEN_MAX] = { 0 };
   char bot_token[SK_TOKEN_MAX] = { 0 };
   if (credentials_json && credentials_json[0]) {
      struct json_object *obj = json_tokener_parse(credentials_json);
      if (obj) {
         struct json_object *at = NULL;
         struct json_object *bt = NULL;
         if (json_object_object_get_ex(obj, "app_token", &at) && at) {
            snprintf(app_token, sizeof(app_token), "%s", json_object_get_string(at));
         }
         if (json_object_object_get_ex(obj, "bot_token", &bt) && bt) {
            snprintf(bot_token, sizeof(bot_token), "%s", json_object_get_string(bt));
         }
         json_object_put(obj);
      }
   }
   if (app_token[0] == '\0' || bot_token[0] == '\0') {
      OLOG_ERROR("slack: app_token and bot_token both required");
      return FAILURE;
   }
   snprintf(s_app_token, sizeof(s_app_token), "%s", app_token);
   snprintf(s_bot_token, sizeof(s_bot_token), "%s", bot_token);

   struct lws_context_creation_info info;
   memset(&info, 0, sizeof(info));
   info.port = CONTEXT_PORT_NO_LISTEN;
   info.protocols = s_sk_protocols;
   info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
   info.gid = (gid_t)-1;
   info.uid = (uid_t)-1;
   info.user = NULL;

   s_lws_context = lws_create_context(&info);
   if (!s_lws_context) {
      OLOG_ERROR("slack: lws_create_context failed");
      sodium_memzero(s_app_token, sizeof(s_app_token));
      sodium_memzero(s_bot_token, sizeof(s_bot_token));
      return FAILURE;
   }

   ws_reconnect_init(&s_reconnect);

   atomic_store(&s_running, true);
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setstacksize(&attr, SK_LISTENER_STACK);
   int prc = pthread_create(&s_listener_thread, &attr, sk_listener_thread, NULL);
   pthread_attr_destroy(&attr);
   if (prc != 0) {
      atomic_store(&s_running, false);
      lws_context_destroy(s_lws_context);
      s_lws_context = NULL;
      sodium_memzero(s_app_token, sizeof(s_app_token));
      sodium_memzero(s_bot_token, sizeof(s_bot_token));
      OLOG_ERROR("slack: failed to spawn listener thread (rc=%d)", prc);
      return FAILURE;
   }
   s_listener_started = true;
   OLOG_INFO("slack: driver initialized (Socket Mode client)");
   return SUCCESS;
}

static void sk_shutdown(void) {
   atomic_store(&s_running, false);
   if (s_lws_context) {
      lws_cancel_service(s_lws_context);
   }
   if (s_listener_started) {
      pthread_join(s_listener_thread, NULL);
      s_listener_started = false;
   }
   if (s_lws_context) {
      lws_context_destroy(s_lws_context);
      s_lws_context = NULL;
   }
   pthread_mutex_lock(&s_send_curl_mutex);
   if (s_send_curl) {
      curl_easy_cleanup(s_send_curl);
      s_send_curl = NULL;
   }
   pthread_mutex_unlock(&s_send_curl_mutex);
   free(s_rx_buf);
   s_rx_buf = NULL;
   s_rx_buf_cap = 0;
   s_rx_buf_len = 0;
   /* Wipe tokens from process memory so a post-shutdown core dump can't
    * recover them.  Matches Discord's sodium_memzero on s_bot_token. */
   sodium_memzero(s_app_token, sizeof(s_app_token));
   sodium_memzero(s_bot_token, sizeof(s_bot_token));
   OLOG_INFO("slack: driver shut down");
}

static int sk_register_inbound_cb(messaging_inbound_fn cb) {
   pthread_mutex_lock(&s_inbound_cb_mutex);
   s_inbound_cb = cb;
   pthread_mutex_unlock(&s_inbound_cb_mutex);
   return SUCCESS;
}

static int sk_validate_address(const char *address_json) {
   if (!address_json || address_json[0] == '\0') {
      return FAILURE;
   }
   struct json_object *obj = json_tokener_parse(address_json);
   if (!obj) {
      return FAILURE;
   }
   struct json_object *cid = NULL;
   int rc = FAILURE;
   if (json_object_object_get_ex(obj, "channel_id", &cid) && cid) {
      const char *s = json_object_get_string(cid);
      if (is_valid_slack_channel(s)) {
         rc = SUCCESS;
      }
   }
   json_object_put(obj);
   return rc;
}

static int sk_is_connected(void) {
   return atomic_load(&s_connected) ? 1 : 0;
}

static void sk_build_address_json(const char *provider_address, char *buf, size_t buf_size) {
   if (!buf || buf_size == 0) {
      return;
   }
   /* Re-validate the channel shape here even though all current callers
    * pre-validate.  This is the driver's public surface; a future caller
    * that bypassed validation would otherwise emit malformed JSON via
    * the snprintf below.  is_valid_slack_channel rejects '"' and '\\',
    * so the snprintf can't be tricked into producing invalid JSON. */
   if (!provider_address || !is_valid_slack_channel(provider_address)) {
      snprintf(buf, buf_size, "{}");
      return;
   }
   snprintf(buf, buf_size, "{\"channel_id\":\"%s\"}", provider_address);
}

static int sk_reconnect(void) {
   atomic_store(&s_reconnect_requested, true);
   return SUCCESS;
}

/* =============================================================================
 * Driver descriptor + registration entry point
 * ============================================================================= */

static const messaging_driver_t s_slack_driver = {
   .name = "slack",
   .authenticates_sender = true,
   .out_format = MSG_FMT_SLACK_MRKDWN,
   .init = sk_init,
   .shutdown = sk_shutdown,
   .send_text = sk_send_text,
   .build_address_json = sk_build_address_json,
   .register_inbound_cb = sk_register_inbound_cb,
   .validate_address = sk_validate_address,
   .is_connected = sk_is_connected,
   .reconnect = sk_reconnect,
   /* Slack has no per-channel typing indicator for bots through Socket
    * Mode (users.setActive is workspace-scoped, not channel-scoped).
    * Leave NULL — engine's keepalive thread handles NULL fine. */
   .send_typing = NULL,
};

int messaging_slack_register(const char *app_token, const char *bot_token) {
   if (!app_token || app_token[0] == '\0' || !bot_token || bot_token[0] == '\0') {
      OLOG_INFO("slack: app_token or bot_token missing; driver not registered");
      return FAILURE;
   }
   /* Build the credentials envelope through json-c so any future token
    * shape that contains '"' or '\\' (Slack tokens today are
    * alphanumeric + dashes, but defense-in-depth matters at the auth
    * boundary) is escaped correctly.  The naive snprintf wrapper would
    * silently substitute one token for another on a malformed input. */
   int rc = FAILURE;
   struct json_object *obj = json_object_new_object();
   if (!obj) {
      return FAILURE;
   }
   json_object_object_add(obj, "app_token", json_object_new_string(app_token));
   json_object_object_add(obj, "bot_token", json_object_new_string(bot_token));
   const char *creds_str = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
   if (creds_str && sk_init(creds_str) == SUCCESS) {
      if (messaging_engine_register_driver(&s_slack_driver) == MESSAGING_SUCCESS) {
         rc = SUCCESS;
      } else {
         OLOG_ERROR("slack: driver registration with engine failed");
         sk_shutdown();
      }
   }
   /* json-c owns creds_str; the json_object_put below frees its internal
    * buffer.  Tokens still live in s_app_token/s_bot_token (wiped on
    * shutdown via sodium_memzero) but the transient JSON copy goes away
    * here. */
   json_object_put(obj);
   return rc;
}

void messaging_slack_shutdown(void) {
   sk_shutdown();
}
