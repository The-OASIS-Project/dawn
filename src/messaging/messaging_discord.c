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
 * Discord Gateway driver — DM-only, text-only.
 *
 * Inbound:  Gateway WebSocket via libwebsockets client mode.  Single
 *           listener thread runs lws_service() + handles the heartbeat
 *           cadence + drives the reconnect state machine.
 * Outbound: REST POST /channels/{id}/messages via libcurl on a separate
 *           CURL handle protected by its own mutex (so worker threads
 *           calling send_text don't fight the listener for the lws
 *           context).
 *
 * Lifecycle:
 *   CONNECT  → CLIENT_ESTABLISHED →
 *   HELLO    → IDENTIFY (or RESUME if reconnect state has a session) →
 *   READY    → record session_id + resume_gateway_url →
 *   DISPATCH → MESSAGE_CREATE (DM filter, bot filter) → engine callback
 *   periodic heartbeat (interval from HELLO)
 *
 * Token redaction: the bot token never appears in URLs (REST uses an
 * Authorization header), but the WS connect string is the gateway URL
 * which has no secrets in it.  REST log lines that include headers
 * MUST omit the Authorization line — the driver logs the URL only.
 *
 * See docs/MESSAGING_CHANNELS_DESIGN.md §8 (Discord).
 */
#include "messaging/messaging_discord.h"

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
#include "core/iso8601.h"
#include "core/ws_reconnect.h"
#include "dawn_error.h"
#include "logging.h"
#include "messaging/messaging_discord_internal.h"
#include "messaging/messaging_driver.h"
#include "messaging/messaging_engine.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

/* DC_BOT_TOKEN_MAX, DC_REST_BASE_URL, DC_USER_AGENT are shared with the read
 * path — defined in messaging_discord_internal.h. */
#define DC_GATEWAY_HOST "gateway.discord.gg"
#define DC_GATEWAY_PATH "/?v=10&encoding=json"
#define DC_GATEWAY_PORT 443
#define DC_LISTENER_STACK (64 * 1024)
#define DC_RX_BUF_INITIAL 4096
#define DC_RX_BUF_MAX (1 * 1024 * 1024) /* hard cap — handshake/dispatch payloads */
#define DC_TX_BUF_MAX 4096
#define DC_HEARTBEAT_TIMEOUT_FACTOR 2 /* missing N intervals → zombie */
/* lws_service() poll quantum.  Unlike Telegram (which blocks for ~25 s
 * inside curl's long-poll cycle and has near-zero wakes/sec at idle),
 * libwebsockets client mode requires periodic `lws_service` calls to
 * drive its event loop.  50 ms = 20 Hz wakes at idle, ~5-10 µs each on
 * Jetson — under 0.05 % CPU, negligible vs the ASR/LLM hot paths.  Any
 * higher value delays the heartbeat-due check and shutdown response;
 * any lower bloats wake overhead without practical benefit. */
#define DC_SERVICE_TIMEOUT_MS 50

/* The channel-history READ path (REST discovery + history) lives in
 * messaging_discord_read.c; its constants/state are file-local there. */

/* Discord intents bitfield.  v1 DM-only: DIRECT_MESSAGES gives DM
 * dispatch events; MESSAGE_CONTENT is required (privileged) to see
 * message text content even in DMs as of 2026.
 *
 *   GUILD_MESSAGES   (1<<9)  = 512    — not used (we don't read server channels in v1)
 *   DIRECT_MESSAGES  (1<<12) = 4096   — receive DM events
 *   MESSAGE_CONTENT  (1<<15) = 32768  — see message body
 */
#define DC_INTENT_DIRECT_MESSAGES (1u << 12)
#define DC_INTENT_MESSAGE_CONTENT (1u << 15)
#define DC_INTENTS (DC_INTENT_DIRECT_MESSAGES | DC_INTENT_MESSAGE_CONTENT)

/* Discord Gateway opcodes (subset relevant to v1). */
enum {
   DC_OP_DISPATCH = 0,
   DC_OP_HEARTBEAT = 1,
   DC_OP_IDENTIFY = 2,
   DC_OP_RESUME = 6,
   DC_OP_RECONNECT = 7,
   DC_OP_INVALID_SESSION = 9,
   DC_OP_HELLO = 10,
   DC_OP_HEARTBEAT_ACK = 11,
};

/* What the next CLIENT_WRITEABLE callback(s) should send.  Bitmask so
 * concurrent schedules don't clobber each other (heartbeat firing
 * while an IDENTIFY/RESUME is pending used to OVERWRITE the pending
 * op under the original scalar shape — bug if the writeable callback
 * hadn't fired yet).  Drained in priority order in the writeable
 * callback: heartbeat > identify > resume, one op per callback. */
enum dc_pending_op {
   DC_PENDING_NONE = 0,
   DC_PENDING_IDENTIFY = 1 << 0,
   DC_PENDING_RESUME = 1 << 1,
   DC_PENDING_HEARTBEAT = 1 << 2,
};

/* lws callback returns -1 to close the connection — this is the lws API
 * contract, NOT DAWN's SUCCESS/FAILURE convention.  Named constant mirrors
 * the convention in src/webui/webui_server.c (see include/webui/webui_internal.h:55). */
#define LWS_CLOSE_CONNECTION (-1)

/* =============================================================================
 * State
 * ============================================================================= */

/* Non-static: shared with the read path via messaging_discord_internal.h. */
char s_bot_token[DC_BOT_TOKEN_MAX];

static atomic_bool s_running = ATOMIC_VAR_INIT(false);
static atomic_bool s_connected = ATOMIC_VAR_INIT(false);

/* Deferred-close signal.  Set whenever something inside a libwebsockets
 * callback (CLIENT_RECEIVE → handle_gateway_frame → server-driven
 * RECONNECT / INVALID_SESSION) decides we should close, or when an
 * off-thread driver->reconnect() fires.  The listener loop drains
 * the flag with atomic_exchange and runs close_active_connection()
 * — i.e. lws_set_timeout(LWS_TO_KILL_SYNC) — only from OUTSIDE the
 * callback dispatch chain.
 *
 * Why: calling lws_set_timeout(LWS_TO_KILL_SYNC) from inside a
 * callback for the same wsi is a use-after-free footgun.  The sync-
 * kill runs CLIENT_CLOSED, frees the wsi struct, and returns — but
 * the outer callback dispatcher (the one that drove us here) still
 * holds a wsi pointer on its stack and dereferences it on return.
 * Observed segfault 2026-05-29 immediately after a server-initiated
 * RECONNECT.  lws_set_timeout is ALSO not documented as thread-safe,
 * so the off-thread caller path uses the same flag for symmetry. */
static atomic_bool s_reconnect_requested = ATOMIC_VAR_INIT(false);

static pthread_t s_listener_thread;
static bool s_listener_started = false;

static messaging_inbound_fn s_inbound_cb = NULL;
static pthread_mutex_t s_inbound_cb_mutex = PTHREAD_MUTEX_INITIALIZER;

/* REST handle for outbound sends (worker threads).  The read path uses its
 * own handle in messaging_discord_read.c, so a server sweep never blocks a
 * send. */
static CURL *s_send_curl = NULL;
static pthread_mutex_t s_send_curl_mutex = PTHREAD_MUTEX_INITIALIZER;

/* lws context + active wsi — owned by listener thread only. */
static struct lws_context *s_lws_context = NULL;
static struct lws *s_wsi = NULL;

/* Reconnect state — owned by listener thread only. */
static ws_reconnect_state_t s_reconnect;

/* RX accumulator for fragmented gateway frames.  Owned by listener
 * thread only. */
static char *s_rx_buf = NULL;
static size_t s_rx_buf_len = 0;
static size_t s_rx_buf_cap = 0;

/* TX state — what to send on the next CLIENT_WRITEABLE.  Owned by
 * listener thread (lws callbacks all run on the listener thread).
 * Bitmask of DC_PENDING_* flags. */
static unsigned int s_pending_op = DC_PENDING_NONE;
static unsigned char s_tx_buf[LWS_PRE + DC_TX_BUF_MAX];
static size_t s_tx_payload_len = 0;

/* Heartbeat timing — owned by listener thread only.  CLOCK_MONOTONIC
 * milliseconds. */
static int64_t s_heartbeat_interval_ms = 0;
static int64_t s_heartbeat_next_due_ms = 0;
static int64_t s_last_heartbeat_ack_ms = 0;
static bool s_awaiting_heartbeat_ack = false;

/* =============================================================================
 * Internal helpers
 * ============================================================================= */

static int64_t now_monotonic_ms(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void rx_buf_reset(void) {
   s_rx_buf_len = 0;
}

static int rx_buf_append(const void *data, size_t len) {
   if (s_rx_buf_len + len > DC_RX_BUF_MAX) {
      OLOG_WARNING("discord: rx buffer would exceed %d bytes; dropping frame", DC_RX_BUF_MAX);
      rx_buf_reset();
      return FAILURE;
   }
   if (s_rx_buf_len + len + 1 > s_rx_buf_cap) {
      size_t new_cap = s_rx_buf_cap ? s_rx_buf_cap * 2 : DC_RX_BUF_INITIAL;
      while (new_cap < s_rx_buf_len + len + 1) {
         new_cap *= 2;
      }
      if (new_cap > DC_RX_BUF_MAX) {
         new_cap = DC_RX_BUF_MAX;
      }
      char *nb = realloc(s_rx_buf, new_cap);
      if (!nb) {
         OLOG_ERROR("discord: rx buffer realloc failed");
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

/* =============================================================================
 * Outbound payload builders
 * ============================================================================= */

static void prepare_heartbeat_payload(void) {
   /* {"op":1,"d":<seq or null>} */
   struct json_object *obj = json_object_new_object();
   if (!obj) {
      s_tx_payload_len = 0;
      return;
   }
   json_object_object_add(obj, "op", json_object_new_int(DC_OP_HEARTBEAT));
   if (s_reconnect.last_sequence >= 0) {
      json_object_object_add(obj, "d", json_object_new_int64(s_reconnect.last_sequence));
   } else {
      /* Discord requires the field present as JSON null on the first
       * heartbeat (before any DISPATCH has set a sequence).  Use the
       * explicit constructor — passing NULL to json_object_object_add
       * is legal in upstream json-c but ambiguous across forks. */
      json_object_object_add(obj, "d", json_object_new_null());
   }
   const char *s = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
   size_t len = s ? strlen(s) : 0;
   if (len == 0 || len > DC_TX_BUF_MAX) {
      OLOG_ERROR("discord: heartbeat payload too large (%zu)", len);
      s_tx_payload_len = 0;
      json_object_put(obj);
      return;
   }
   /* WebSocket frame sent by length (lws_write), never read as a C string */
   // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
   memcpy(s_tx_buf + LWS_PRE, s, len);
   s_tx_payload_len = len;
   json_object_put(obj);
}

static void prepare_identify_payload(void) {
   /* {"op":2,"d":{"token":"<TOKEN>","intents":<bits>,"properties":{...}}} */
   struct json_object *root = json_object_new_object();
   struct json_object *d = json_object_new_object();
   struct json_object *props = json_object_new_object();
   if (!root || !d || !props) {
      if (root)
         json_object_put(root);
      if (d)
         json_object_put(d);
      if (props)
         json_object_put(props);
      s_tx_payload_len = 0;
      return;
   }
   json_object_object_add(props, "os", json_object_new_string("linux"));
   json_object_object_add(props, "browser", json_object_new_string("DAWN"));
   json_object_object_add(props, "device", json_object_new_string("DAWN"));

   json_object_object_add(d, "token", json_object_new_string(s_bot_token));
   json_object_object_add(d, "intents", json_object_new_int64(DC_INTENTS));
   json_object_object_add(d, "properties", props);

   json_object_object_add(root, "op", json_object_new_int(DC_OP_IDENTIFY));
   json_object_object_add(root, "d", d);

   const char *s = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
   size_t len = s ? strlen(s) : 0;
   if (len == 0 || len > DC_TX_BUF_MAX) {
      OLOG_ERROR("discord: identify payload too large (%zu)", len);
      s_tx_payload_len = 0;
      json_object_put(root);
      return;
   }
   /* WebSocket frame sent by length (lws_write), never read as a C string */
   // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
   memcpy(s_tx_buf + LWS_PRE, s, len);
   s_tx_payload_len = len;
   json_object_put(root);
}

static void prepare_resume_payload(void) {
   /* {"op":6,"d":{"token":"<TOKEN>","session_id":"...","seq":<seq>}} */
   struct json_object *root = json_object_new_object();
   struct json_object *d = json_object_new_object();
   if (!root || !d) {
      if (root)
         json_object_put(root);
      if (d)
         json_object_put(d);
      s_tx_payload_len = 0;
      return;
   }
   json_object_object_add(d, "token", json_object_new_string(s_bot_token));
   json_object_object_add(d, "session_id", json_object_new_string(s_reconnect.session_id));
   json_object_object_add(d, "seq", json_object_new_int64(s_reconnect.last_sequence));

   json_object_object_add(root, "op", json_object_new_int(DC_OP_RESUME));
   json_object_object_add(root, "d", d);

   const char *s = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
   size_t len = s ? strlen(s) : 0;
   if (len == 0 || len > DC_TX_BUF_MAX) {
      OLOG_ERROR("discord: resume payload too large (%zu)", len);
      s_tx_payload_len = 0;
      json_object_put(root);
      return;
   }
   /* WebSocket frame sent by length (lws_write), never read as a C string */
   // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
   memcpy(s_tx_buf + LWS_PRE, s, len);
   s_tx_payload_len = len;
   json_object_put(root);
}

/* Listener-thread only.  All current callers are lws callbacks (which
 * run on the listener thread synchronously from lws_service) or
 * check_heartbeat (which runs in the listener loop between lws_service
 * calls).  If a future call site adds an off-thread caller,
 * s_pending_op must become atomic and s_wsi reads need a guard.
 *
 * Bitmask semantics: OR-in the op flag so a heartbeat scheduled while
 * an IDENTIFY/RESUME is already pending doesn't clobber it.  The
 * writeable callback drains one bit per dispatch in priority order
 * (heartbeat > identify > resume), re-arming the callback if more
 * bits remain. */
static void schedule_writable(enum dc_pending_op op) {
   s_pending_op |= (unsigned int)op;
   if (s_wsi) {
      lws_callback_on_writable(s_wsi);
   }
}

/* =============================================================================
 * Dispatch handlers
 * ============================================================================= */

static void handle_hello(struct json_object *d) {
   struct json_object *hb = NULL;
   if (!d || !json_object_object_get_ex(d, "heartbeat_interval", &hb)) {
      OLOG_WARNING("discord: HELLO missing heartbeat_interval");
      return;
   }
   s_heartbeat_interval_ms = json_object_get_int64(hb);
   if (s_heartbeat_interval_ms < 1000 || s_heartbeat_interval_ms > 600000) {
      OLOG_WARNING("discord: HELLO heartbeat_interval out of expected range (%" PRId64
                   " ms); clamping",
                   s_heartbeat_interval_ms);
      if (s_heartbeat_interval_ms < 1000)
         s_heartbeat_interval_ms = 1000;
      if (s_heartbeat_interval_ms > 600000)
         s_heartbeat_interval_ms = 600000;
   }
   /* Per Discord docs: first heartbeat fires at random(0, interval) to
    * avoid thundering-herd on the gateway after restart waves.  Simple
    * approximation: half the interval. */
   s_heartbeat_next_due_ms = now_monotonic_ms() + s_heartbeat_interval_ms / 2;
   s_last_heartbeat_ack_ms = now_monotonic_ms();
   s_awaiting_heartbeat_ack = false;

   if (ws_reconnect_has_resumable_session(&s_reconnect)) {
      OLOG_INFO("discord: HELLO received; sending RESUME (session=%.8s..., seq=%" PRId64 ")",
                s_reconnect.session_id, s_reconnect.last_sequence);
      schedule_writable(DC_PENDING_RESUME);
   } else {
      OLOG_INFO("discord: HELLO received; sending IDENTIFY (intents=0x%x)", DC_INTENTS);
      schedule_writable(DC_PENDING_IDENTIFY);
   }
}

static void handle_message_create(struct json_object *d) {
   if (!d) {
      return;
   }
   /* DM filter: skip if guild_id is present and non-null. */
   struct json_object *guild_id_obj = NULL;
   if (json_object_object_get_ex(d, "guild_id", &guild_id_obj) && guild_id_obj &&
       !json_object_is_type(guild_id_obj, json_type_null)) {
      return; /* server channel — not DM-only v1 */
   }

   /* Bot filter: skip if author.bot == true (don't echo own messages). */
   struct json_object *author_obj = NULL;
   if (!json_object_object_get_ex(d, "author", &author_obj) || !author_obj) {
      return;
   }
   struct json_object *bot_obj = NULL;
   if (json_object_object_get_ex(author_obj, "bot", &bot_obj) && json_object_get_boolean(bot_obj)) {
      return;
   }
   /* The engine answers only the person who linked the channel, by id. */
   struct json_object *author_id_obj = NULL;
   if (!json_object_object_get_ex(author_obj, "id", &author_id_obj) || !author_id_obj) {
      return;
   }
   const char *author_id = json_object_get_string(author_id_obj);
   if (!author_id || !author_id[0]) {
      return;
   }

   /* Required fields: channel_id, content. */
   struct json_object *channel_id_obj = NULL;
   struct json_object *content_obj = NULL;
   if (!json_object_object_get_ex(d, "channel_id", &channel_id_obj) || !channel_id_obj) {
      return;
   }
   if (!json_object_object_get_ex(d, "content", &content_obj) || !content_obj) {
      return;
   }
   const char *channel_id = json_object_get_string(channel_id_obj);
   const char *body = json_object_get_string(content_obj);
   if (!channel_id || !body) {
      return;
   }

   /* Sender display: prefer global_name, then username. */
   const char *sender_display = NULL;
   struct json_object *gn_obj = NULL;
   struct json_object *un_obj = NULL;
   if (json_object_object_get_ex(author_obj, "global_name", &gn_obj) && gn_obj &&
       !json_object_is_type(gn_obj, json_type_null)) {
      sender_display = json_object_get_string(gn_obj);
   }
   if (!sender_display && json_object_object_get_ex(author_obj, "username", &un_obj) && un_obj) {
      sender_display = json_object_get_string(un_obj);
   }
   if (!sender_display) {
      sender_display = "discord_user";
   }

   /* Timestamp: ISO-8601 string → epoch via shared parser. */
   int64_t timestamp = 0;
   struct json_object *ts_obj = NULL;
   if (json_object_object_get_ex(d, "timestamp", &ts_obj) && ts_obj) {
      const char *ts_str = json_object_get_string(ts_obj);
      if (ts_str) {
         time_t t = iso8601_parse(ts_str);
         if (t != (time_t)-1) {
            timestamp = (int64_t)t;
         }
      }
   }

   /* Dispatch to engine. */
   messaging_inbound_fn cb = NULL;
   pthread_mutex_lock(&s_inbound_cb_mutex);
   cb = s_inbound_cb;
   pthread_mutex_unlock(&s_inbound_cb_mutex);
   if (cb) {
      /* DMs only (guild messages dropped above): one person per chat. */
      messaging_inbound_t in = {
         .provider = "discord",
         .provider_address = channel_id,
         .sender_id = author_id,
         .sender_display = sender_display,
         .body = body,
         .timestamp = timestamp,
         .chat_kind = MESSAGING_CHAT_ONE_TO_ONE,
      };
      cb(&in);
   }
}

static void handle_dispatch(struct json_object *root) {
   /* Update sequence from the top-level "s" field. */
   struct json_object *s_obj = NULL;
   if (json_object_object_get_ex(root, "s", &s_obj) && s_obj &&
       !json_object_is_type(s_obj, json_type_null)) {
      int64_t seq = json_object_get_int64(s_obj);
      if (seq > s_reconnect.last_sequence) {
         s_reconnect.last_sequence = seq;
      }
   }

   struct json_object *t_obj = NULL;
   if (!json_object_object_get_ex(root, "t", &t_obj) || !t_obj) {
      return;
   }
   const char *t = json_object_get_string(t_obj);
   if (!t) {
      return;
   }

   struct json_object *d_obj = NULL;
   json_object_object_get_ex(root, "d", &d_obj);

   if (strcmp(t, "READY") == 0) {
      /* Record session_id + resume_gateway_url.  These are needed for
       * subsequent RESUME on disconnect. */
      const char *session_id = NULL;
      const char *resume_url = NULL;
      if (d_obj) {
         struct json_object *sid_obj = NULL;
         struct json_object *ru_obj = NULL;
         if (json_object_object_get_ex(d_obj, "session_id", &sid_obj) && sid_obj) {
            session_id = json_object_get_string(sid_obj);
         }
         if (json_object_object_get_ex(d_obj, "resume_gateway_url", &ru_obj) && ru_obj) {
            resume_url = json_object_get_string(ru_obj);
         }
      }
      ws_reconnect_record_success(&s_reconnect, session_id, resume_url);
      atomic_store(&s_connected, true);
      OLOG_INFO("discord: READY (session=%.8s..., gateway=%s)", session_id ? session_id : "(none)",
                resume_url ? resume_url : "(none)");
   } else if (strcmp(t, "RESUMED") == 0) {
      ws_reconnect_record_success(&s_reconnect, NULL, NULL);
      atomic_store(&s_connected, true);
      OLOG_INFO("discord: RESUMED (session=%.8s..., seq=%" PRId64 ")", s_reconnect.session_id,
                s_reconnect.last_sequence);
   } else if (strcmp(t, "MESSAGE_CREATE") == 0) {
      handle_message_create(d_obj);
   }
   /* Other dispatch types ignored in v1. */
}

static void handle_invalid_session(struct json_object *root) {
   /* d=true → session can be resumed; d=false → not resumable. */
   struct json_object *d_obj = NULL;
   bool resumable = false;
   if (json_object_object_get_ex(root, "d", &d_obj) && d_obj) {
      resumable = json_object_get_boolean(d_obj);
   }
   if (!resumable) {
      OLOG_WARNING("discord: INVALID_SESSION (not resumable); will re-IDENTIFY");
      ws_reconnect_invalidate_session(&s_reconnect);
   } else {
      OLOG_INFO("discord: INVALID_SESSION (resumable); will reconnect");
   }
   /* Close socket to drive reconnect via main loop.  Defer to the
    * listener thread via s_reconnect_requested — sync-killing from
    * inside this callback chain would free the wsi while the outer
    * CLIENT_RECEIVE dispatcher still holds a reference on its
    * stack (see s_reconnect_requested comment). */
   atomic_store(&s_reconnect_requested, true);
}

static void handle_gateway_frame(const char *json_text, size_t len) {
   (void)len;
   struct json_object *root = json_tokener_parse(json_text);
   if (!root) {
      OLOG_WARNING("discord: failed to parse gateway frame");
      return;
   }
   struct json_object *op_obj = NULL;
   if (!json_object_object_get_ex(root, "op", &op_obj) || !op_obj) {
      json_object_put(root);
      return;
   }
   int op = json_object_get_int(op_obj);
   struct json_object *d_obj = NULL;
   json_object_object_get_ex(root, "d", &d_obj);

   switch (op) {
      case DC_OP_HELLO:
         handle_hello(d_obj);
         break;
      case DC_OP_DISPATCH:
         handle_dispatch(root);
         break;
      case DC_OP_HEARTBEAT:
         /* Server is asking us to send a heartbeat NOW. */
         schedule_writable(DC_PENDING_HEARTBEAT);
         break;
      case DC_OP_HEARTBEAT_ACK:
         s_last_heartbeat_ack_ms = now_monotonic_ms();
         s_awaiting_heartbeat_ack = false;
         break;
      case DC_OP_RECONNECT:
         OLOG_INFO("discord: server requested RECONNECT");
         /* Defer the close to the listener thread.  Sync-killing
          * here frees the wsi while the outer CLIENT_RECEIVE
          * dispatcher still holds a stack reference → use-after-
          * free segfault (observed 2026-05-29).  See the
          * s_reconnect_requested comment for the full rationale. */
         atomic_store(&s_reconnect_requested, true);
         break;
      case DC_OP_INVALID_SESSION:
         handle_invalid_session(root);
         break;
      default:
         break; /* unknown opcode — ignore in v1 */
   }
   json_object_put(root);
}

/* =============================================================================
 * libwebsockets callback
 * ============================================================================= */

static int dc_lws_callback(struct lws *wsi,
                           enum lws_callback_reasons reason,
                           void *user,
                           void *in,
                           size_t len) {
   (void)user;

   switch (reason) {
      case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
         OLOG_WARNING("discord: gateway connection error: %s",
                      in ? (const char *)in : "(no detail)");
         atomic_store(&s_connected, false);
         s_wsi = NULL;
         return LWS_CLOSE_CONNECTION;

      case LWS_CALLBACK_CLIENT_ESTABLISHED:
         OLOG_INFO("discord: gateway WebSocket established");
         s_wsi = wsi;
         /* Don't send anything yet — HELLO drives the next step. */
         break;

      case LWS_CALLBACK_CLIENT_RECEIVE: {
         if (in && len > 0) {
            if (rx_buf_append(in, len) != SUCCESS) {
               /* Buffer overflow already logged; drop connection to recover. */
               return LWS_CLOSE_CONNECTION;
            }
         }
         if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
            /* Full frame assembled. */
            if (s_rx_buf && s_rx_buf_len > 0) {
               handle_gateway_frame(s_rx_buf, s_rx_buf_len);
            }
            rx_buf_reset();
         }
         break;
      }

      case LWS_CALLBACK_CLIENT_WRITEABLE: {
         if (s_pending_op == DC_PENDING_NONE) {
            break;
         }
         /* Drain one bit per dispatch in priority order.  Heartbeat
          * first because missing one kills the connection; identify/
          * resume happen once and tolerate a few ms of additional
          * latency from being deferred to the next writeable callback. */
         enum dc_pending_op sent = DC_PENDING_NONE;
         if (s_pending_op & DC_PENDING_HEARTBEAT) {
            prepare_heartbeat_payload();
            s_awaiting_heartbeat_ack = true;
            sent = DC_PENDING_HEARTBEAT;
         } else if (s_pending_op & DC_PENDING_IDENTIFY) {
            prepare_identify_payload();
            sent = DC_PENDING_IDENTIFY;
         } else if (s_pending_op & DC_PENDING_RESUME) {
            prepare_resume_payload();
            sent = DC_PENDING_RESUME;
         } else {
            /* Unknown bit — clear and bail to keep the bitmask sane. */
            s_pending_op = DC_PENDING_NONE;
            return 0;
         }
         s_pending_op &= ~(unsigned int)sent;
         if (s_tx_payload_len == 0) {
            /* Builder bailed (OOM, oversized).  Force reconnect. */
            return LWS_CLOSE_CONNECTION;
         }
         int n = lws_write(wsi, s_tx_buf + LWS_PRE, s_tx_payload_len, LWS_WRITE_TEXT);
         if (n < 0 || (size_t)n < s_tx_payload_len) {
            OLOG_WARNING("discord: lws_write failed (op=%d, wrote=%d of %zu)", (int)sent, n,
                         s_tx_payload_len);
            return LWS_CLOSE_CONNECTION;
         }
         /* If more bits remain, re-arm — lws will dispatch us again. */
         if (s_pending_op != DC_PENDING_NONE) {
            lws_callback_on_writable(wsi);
         }
         break;
      }

      case LWS_CALLBACK_CLIENT_CLOSED:
         OLOG_INFO("discord: gateway WebSocket closed");
         atomic_store(&s_connected, false);
         s_wsi = NULL;
         rx_buf_reset();
         break;

      default:
         break;
   }
   return 0;
}

static const struct lws_protocols s_dc_protocols[] = {
   { "discord-gateway", dc_lws_callback, 0, DC_RX_BUF_INITIAL, 0, NULL, 0 },
   { NULL, NULL, 0, 0, 0, NULL, 0 },
};

/* =============================================================================
 * Listener thread
 * ============================================================================= */

static void interruptible_sleep_seconds(int seconds) {
   /* Break the sleep into 250ms chunks so shutdown signals don't have
    * to wait the full backoff window. */
   const int chunk_ms = 250;
   int total_ms = seconds * 1000;
   while (total_ms > 0 && atomic_load(&s_running)) {
      int s = (total_ms > chunk_ms) ? chunk_ms : total_ms;
      struct timespec ts = { .tv_sec = s / 1000, .tv_nsec = (s % 1000) * 1000000L };
      nanosleep(&ts, NULL);
      total_ms -= s;
   }
}

static int attempt_connect(const char *host_override, const char *path_override) {
   struct lws_client_connect_info i;
   memset(&i, 0, sizeof(i));
   i.context = s_lws_context;
   i.address = host_override ? host_override : DC_GATEWAY_HOST;
   i.host = i.address;
   i.origin = i.address;
   i.port = DC_GATEWAY_PORT;
   i.path = path_override ? path_override : DC_GATEWAY_PATH;
   i.ssl_connection = LCCSCF_USE_SSL; /* TLS required; no self-signed certs allowed */
   i.protocol = s_dc_protocols[0].name;
   i.pwsi = &s_wsi;

   OLOG_INFO("discord: connecting to wss://%s%s", i.address, i.path);
   struct lws *w = lws_client_connect_via_info(&i);
   if (!w) {
      OLOG_WARNING("discord: lws_client_connect_via_info failed");
      return FAILURE;
   }
   /* s_wsi is set by lws when the connect completes (pwsi). */
   return SUCCESS;
}

static void close_active_connection(void) {
   if (s_wsi) {
      lws_set_timeout(s_wsi, PENDING_TIMEOUT_USER_OK, LWS_TO_KILL_SYNC);
   }
}

static void check_heartbeat(void) {
   if (s_heartbeat_interval_ms <= 0) {
      return; /* HELLO not received yet */
   }
   int64_t now = now_monotonic_ms();

   /* Zombie check: if we sent a heartbeat and didn't get an ACK
    * within HEARTBEAT_TIMEOUT_FACTOR intervals, the connection is
    * dead — Discord docs say "treat as zombie, close and reconnect
    * with RESUME". */
   if (s_awaiting_heartbeat_ack &&
       now - s_last_heartbeat_ack_ms >
           (int64_t)DC_HEARTBEAT_TIMEOUT_FACTOR * s_heartbeat_interval_ms) {
      OLOG_WARNING("discord: heartbeat ACK timeout — closing for resume");
      s_awaiting_heartbeat_ack = false;
      s_heartbeat_interval_ms = 0;
      close_active_connection();
      return;
   }

   if (now >= s_heartbeat_next_due_ms && atomic_load(&s_connected)) {
      s_heartbeat_next_due_ms = now + s_heartbeat_interval_ms;
      schedule_writable(DC_PENDING_HEARTBEAT);
   }
}

static void *dc_listener_thread(void *arg) {
   (void)arg;
   OLOG_INFO("discord: listener thread started");

   while (atomic_load(&s_running)) {
      /* Reconnect loop: while disconnected, wait the backoff and try
       * to connect.  Once a wsi is in flight (s_wsi non-NULL) we just
       * service lws and let the callbacks drive state. */
      if (!s_wsi) {
         atomic_store(&s_connected, false);
         /* Reset transient lifecycle state — keep reconnect state
          * (session_id / last_sequence / backoff) for RESUME. */
         s_heartbeat_interval_ms = 0;
         s_awaiting_heartbeat_ack = false;
         s_pending_op = DC_PENDING_NONE;
         rx_buf_reset();

         int delay = ws_reconnect_next_delay_seconds(&s_reconnect);
         if (delay > 0) {
            interruptible_sleep_seconds(delay);
         }
         if (!atomic_load(&s_running)) {
            break;
         }

         const char *host = NULL;
         /* If we have a resume_url from a prior READY, parse "wss://host/"
          * out of it — Discord pins specific endpoints for sessions. */
         char host_buf[WS_RECONNECT_RESUME_URL_MAX];
         if (ws_reconnect_has_resumable_session(&s_reconnect) && s_reconnect.resume_url[0]) {
            const char *p = s_reconnect.resume_url;
            if (strncmp(p, "wss://", 6) == 0) {
               p += 6;
            } else if (strncmp(p, "ws://", 5) == 0) {
               p += 5;
            }
            size_t hlen = strcspn(p, "/?");
            if (hlen > 0 && hlen < sizeof(host_buf)) {
               memcpy(host_buf, p, hlen);
               host_buf[hlen] = '\0';
               /* Defense in depth: only honor resume_url hostnames that
                * end with `.discord.gg`.  The TLS cert SAN check is the
                * practical gate today (lws verifies the cert chain
                * against the dialed hostname), but allowlisting the
                * suffix means a compromised READY dispatch couldn't
                * redirect us to an attacker-controlled host even if
                * the dialed cert somehow validated. */
               static const char allowed_suffix[] = ".discord.gg";
               const size_t suffix_len = sizeof(allowed_suffix) - 1;
               bool host_allowed = (hlen >= suffix_len &&
                                    strcmp(host_buf + hlen - suffix_len, allowed_suffix) == 0);
               if (host_allowed) {
                  host = host_buf;
               } else {
                  OLOG_WARNING(
                      "discord: resume_url host '%s' not under .discord.gg; falling back to "
                      "default gateway",
                      host_buf);
                  /* Invalidate the session so the next connect uses a
                   * fresh IDENTIFY against the default gateway instead
                   * of re-trying the same bad host with RESUME. */
                  ws_reconnect_invalidate_session(&s_reconnect);
               }
            }
         }
         if (attempt_connect(host, NULL) != SUCCESS) {
            /* Backoff already advanced; loop and retry. */
            continue;
         }
      }

      /* Honor any deferred external-reconnect request (dc_reconnect
       * from off-thread sets the flag; we do the actual close here on
       * the listener thread).  Test-and-clear via atomic_exchange so
       * a re-trigger during reconnect doesn't get lost. */
      if (atomic_exchange(&s_reconnect_requested, false)) {
         OLOG_INFO("discord: external reconnect requested");
         close_active_connection();
      }

      /* Service the lws context.  Callbacks run synchronously from
       * here.  Returns 0 normally, negative on context error. */
      int n = lws_service(s_lws_context, DC_SERVICE_TIMEOUT_MS);
      if (n < 0) {
         OLOG_WARNING("discord: lws_service returned %d; closing", n);
         close_active_connection();
      }
      check_heartbeat();
   }

   /* Shutting down — close any active connection and let lws clean up. */
   close_active_connection();
   while (s_wsi) {
      lws_service(s_lws_context, 50);
   }

   atomic_store(&s_connected, false);
   OLOG_INFO("discord: listener thread exiting");
   return NULL;
}

/* =============================================================================
 * Outbound REST send
 * ============================================================================= */


static int dc_extract_channel_id(const char *address_json, char *out, size_t out_size) {
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

static int dc_send_text(int user_id,
                        const char *provider_address,
                        const char *address_json,
                        const char *text) {
   /* Bot tokens are bot-wide; user_id is plumbed for contract uniformity
    * (SMS uses it for per-user phone-service audit log scoping). */
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
      if (dc_extract_channel_id(address_json, channel_id, sizeof(channel_id)) != SUCCESS) {
         OLOG_WARNING("discord: send failed — no channel_id in provider_address or address_json");
         return FAILURE;
      }
   } else {
      OLOG_WARNING("discord: send failed — no provider_address or address_json");
      return FAILURE;
   }

   /* Defense in depth: validate snowflake shape (digits-only) BEFORE
    * the channel_id lands in the URL path.  dc_validate_address checks
    * this at /link time, but if a malformed row ever reached us (manual
    * DB edit, future channel-resolution bug), an attacker-controlled
    * string with `../` or `?` could otherwise land in the REST URL. */
   if (!dc_is_valid_snowflake(channel_id)) {
      OLOG_WARNING("discord: send failed — channel_id not a valid snowflake");
      return FAILURE;
   }

   /* Build JSON body: {"content": "<text>"}.  Discord caps content at
    * 2000 chars per message; the engine's outbound shaping is expected
    * to keep us under that, but we don't truncate here — let the
    * provider reject with an HTTP error if oversized, so the failure
    * is visible. */
   struct json_object *body = json_object_new_object();
   if (!body) {
      return FAILURE;
   }
   json_object_object_add(body, "content", json_object_new_string(text));
   const char *body_str = json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN);

   char url[256];
   snprintf(url, sizeof(url), "%s/channels/%s/messages", DC_REST_BASE_URL, channel_id);
   char auth_header[DC_BOT_TOKEN_MAX + 32];
   snprintf(auth_header, sizeof(auth_header), "Authorization: Bot %s", s_bot_token);

   curl_buffer_t resp;
   curl_buffer_init(&resp);
   int rc = FAILURE;

   pthread_mutex_lock(&s_send_curl_mutex);
   if (!s_send_curl) {
      s_send_curl = curl_easy_init();
   }
   if (s_send_curl) {
      curl_easy_reset(s_send_curl);
      curl_easy_setopt(s_send_curl, CURLOPT_URL, url);
      curl_easy_setopt(s_send_curl, CURLOPT_POST, 1L);
      curl_easy_setopt(s_send_curl, CURLOPT_POSTFIELDS, body_str);
      curl_apply_dawn_defaults(s_send_curl, DC_USER_AGENT, 30L, &resp);

      struct curl_slist *hdrs = NULL;
      hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
      hdrs = curl_slist_append(hdrs, auth_header);
      curl_easy_setopt(s_send_curl, CURLOPT_HTTPHEADER, hdrs);

      CURLcode cc = curl_easy_perform(s_send_curl);
      long http_status = 0;
      curl_easy_getinfo(s_send_curl, CURLINFO_RESPONSE_CODE, &http_status);
      curl_slist_free_all(hdrs);

      if (cc == CURLE_OK && http_status >= 200 && http_status < 300) {
         rc = SUCCESS;
      } else {
         /* URL does not contain the bot token (Authorization header
          * does); logging the URL is safe. */
         OLOG_WARNING("discord: POST %s failed (curl=%d http=%ld)", url, cc, http_status);
      }
   }
   pthread_mutex_unlock(&s_send_curl_mutex);

   curl_buffer_free(&resp);
   json_object_put(body);
   return rc;
}

/* Typing-indicator POST.  Fire-and-forget; errors logged at DEBUG and
 * swallowed — typing failures must never block real delivery.  Reuses
 * the same send_curl handle + mutex as dc_send_text (only one of
 * typing-or-message is in flight at a time per driver). */
static void dc_send_typing(int user_id, const char *provider_address, const char *address_json) {
   (void)user_id;
   if (s_bot_token[0] == '\0') {
      return;
   }
   char channel_id[64];
   channel_id[0] = '\0';
   if (provider_address && provider_address[0] != '\0') {
      snprintf(channel_id, sizeof(channel_id), "%s", provider_address);
   } else if (address_json) {
      if (dc_extract_channel_id(address_json, channel_id, sizeof(channel_id)) != SUCCESS) {
         return;
      }
   } else {
      return;
   }
   /* Snowflake validation — same defense as dc_send_text. */
   if (!dc_is_valid_snowflake(channel_id)) {
      return;
   }

   char url[256];
   snprintf(url, sizeof(url), "%s/channels/%s/typing", DC_REST_BASE_URL, channel_id);
   char auth_header[DC_BOT_TOKEN_MAX + 32];
   snprintf(auth_header, sizeof(auth_header), "Authorization: Bot %s", s_bot_token);

   curl_buffer_t resp;
   curl_buffer_init(&resp);

   pthread_mutex_lock(&s_send_curl_mutex);
   if (!s_send_curl) {
      s_send_curl = curl_easy_init();
   }
   if (s_send_curl) {
      curl_easy_reset(s_send_curl);
      curl_easy_setopt(s_send_curl, CURLOPT_URL, url);
      curl_easy_setopt(s_send_curl, CURLOPT_POST, 1L);
      curl_easy_setopt(s_send_curl, CURLOPT_POSTFIELDS, "");
      curl_easy_setopt(s_send_curl, CURLOPT_POSTFIELDSIZE, 0L);
      /* Tighter timeout than send_text: typing is best-effort + repeats
       * every ~4s, so a slow link shouldn't pin the keepalive thread. */
      curl_apply_dawn_defaults(s_send_curl, DC_USER_AGENT, 5L, &resp);

      struct curl_slist *hdrs = NULL;
      hdrs = curl_slist_append(hdrs, auth_header);
      curl_easy_setopt(s_send_curl, CURLOPT_HTTPHEADER, hdrs);

      CURLcode cc = curl_easy_perform(s_send_curl);
      long http_status = 0;
      curl_easy_getinfo(s_send_curl, CURLINFO_RESPONSE_CODE, &http_status);
      curl_slist_free_all(hdrs);

      if (cc != CURLE_OK || http_status < 200 || http_status >= 300) {
         OLOG_DEBUG("discord: typing POST failed (curl=%d http=%ld)", cc, http_status);
      }
   }
   pthread_mutex_unlock(&s_send_curl_mutex);

   curl_buffer_free(&resp);
}

/* =============================================================================
 * Driver hooks
 * ============================================================================= */

static int dc_init(const char *credentials_json) {
   char token[DC_BOT_TOKEN_MAX] = { 0 };
   if (credentials_json && credentials_json[0]) {
      struct json_object *obj = json_tokener_parse(credentials_json);
      if (obj) {
         struct json_object *tk = NULL;
         if (json_object_object_get_ex(obj, "bot_token", &tk) && tk) {
            snprintf(token, sizeof(token), "%s", json_object_get_string(tk));
         }
         json_object_put(obj);
      }
   }
   if (token[0] == '\0') {
      OLOG_ERROR("discord: no bot_token supplied");
      return FAILURE;
   }
   snprintf(s_bot_token, sizeof(s_bot_token), "%s", token);

   /* Initialize lws context (client mode).  CONTEXT_PORT_NO_LISTEN
    * disables the server side; we only initiate outbound connections. */
   struct lws_context_creation_info info;
   memset(&info, 0, sizeof(info));
   info.port = CONTEXT_PORT_NO_LISTEN;
   info.protocols = s_dc_protocols;
   info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
   info.gid = (gid_t)-1;
   info.uid = (uid_t)-1;
   info.user = NULL;

   s_lws_context = lws_create_context(&info);
   if (!s_lws_context) {
      OLOG_ERROR("discord: lws_create_context failed");
      s_bot_token[0] = '\0';
      return FAILURE;
   }

   ws_reconnect_init(&s_reconnect);

   atomic_store(&s_running, true);
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setstacksize(&attr, DC_LISTENER_STACK);
   int prc = pthread_create(&s_listener_thread, &attr, dc_listener_thread, NULL);
   pthread_attr_destroy(&attr);
   if (prc != 0) {
      atomic_store(&s_running, false);
      lws_context_destroy(s_lws_context);
      s_lws_context = NULL;
      s_bot_token[0] = '\0';
      OLOG_ERROR("discord: failed to spawn listener thread (rc=%d)", prc);
      return FAILURE;
   }
   s_listener_started = true;
   OLOG_INFO("discord: driver initialized (Gateway client mode)");
   return SUCCESS;
}

static void dc_shutdown(void) {
   atomic_store(&s_running, false);
   if (s_lws_context) {
      /* Wake the lws_service() poll so the listener can observe
       * s_running=false promptly instead of waiting for the next
       * timeout. */
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
   dc_read_shutdown(); /* tears down the read handle + discovery cache */
   free(s_rx_buf);
   s_rx_buf = NULL;
   s_rx_buf_cap = 0;
   s_rx_buf_len = 0;
   /* Wipe the token from process memory so a post-shutdown core dump
    * or /proc/$PID/maps inspection can't recover it.  reconnect() does
    * not need it (dc_init re-reads from credentials on next invoke). */
   sodium_memzero(s_bot_token, sizeof(s_bot_token));
   OLOG_INFO("discord: driver shut down");
}

static int dc_register_inbound_cb(messaging_inbound_fn cb) {
   pthread_mutex_lock(&s_inbound_cb_mutex);
   s_inbound_cb = cb;
   pthread_mutex_unlock(&s_inbound_cb_mutex);
   return SUCCESS;
}

static int dc_validate_address(const char *address_json) {
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
      if (dc_is_valid_snowflake(s)) {
         rc = SUCCESS;
      }
   }
   json_object_put(obj);
   return rc;
}

static int dc_is_connected(void) {
   return atomic_load(&s_connected) ? 1 : 0;
}

static void dc_build_address_json(const char *provider_address, char *buf, size_t buf_size) {
   if (!buf || buf_size == 0) {
      return;
   }
   /* Re-validate even though all current callers pre-validate.  Defense
    * in depth on the driver's public surface — a future caller bypassing
    * validate_address would otherwise emit malformed JSON via snprintf
    * below.  is_valid_snowflake rejects '"' and '\\' (digits-only). */
   if (!provider_address || !dc_is_valid_snowflake(provider_address)) {
      snprintf(buf, buf_size, "{}");
      return;
   }
   snprintf(buf, buf_size, "{\"channel_id\":\"%s\"}", provider_address);
}

static int dc_reconnect(void) {
   /* Listener auto-reconnects on disconnect via the backoff loop.
    * Explicit reconnect: set a flag the listener picks up on its next
    * iteration — close_active_connection (lws_set_timeout) is NOT
    * documented thread-safe, and the engine could theoretically call
    * driver->reconnect() from any thread.  Flag-based deferral keeps
    * the dangerous call confined to the listener thread regardless of
    * the caller's thread. */
   atomic_store(&s_reconnect_requested, true);
   return SUCCESS;
}

/* =============================================================================
 * Driver descriptor + registration entry point
 * ============================================================================= */

static const messaging_driver_t s_discord_driver = {
   .name = "discord",
   .authenticates_sender = true,
   .out_format = MSG_FMT_DISCORD,
   .init = dc_init,
   .shutdown = dc_shutdown,
   .send_text = dc_send_text,
   .build_address_json = dc_build_address_json,
   .register_inbound_cb = dc_register_inbound_cb,
   .validate_address = dc_validate_address,
   .is_connected = dc_is_connected,
   .reconnect = dc_reconnect,
   .send_typing = dc_send_typing,
   .list_readable_channels = dc_list_readable_channels,
   .read_history = dc_read_history,
   .invalidate_readable_channels_cache = dc_invalidate_channel_cache,
};

int messaging_discord_register(const char *bot_token) {
   if (!bot_token || bot_token[0] == '\0') {
      OLOG_INFO("discord: no bot_token configured; driver not registered");
      return FAILURE;
   }
   /* Init BEFORE engine registration so that an init failure (bad token,
    * lws context creation fail, listener spawn fail) doesn't leave a
    * dead driver in the engine's registry.  dc_init rolls back its own
    * state internally on failure.  The race window where the listener
    * could fire a MESSAGE_CREATE before register_driver wires up the
    * engine's inbound_cb is theoretical — the Gateway handshake (TLS +
    * HELLO + IDENTIFY + READY) takes seconds, register_driver completes
    * in microseconds. */
   /* Build the credentials envelope through json-c so a token containing
    * '"' or '\\' is escaped correctly.  Naive snprintf would substitute
    * one token for another on malformed input — defense-in-depth at the
    * auth boundary. */
   int rc = FAILURE;
   struct json_object *obj = json_object_new_object();
   if (!obj) {
      return FAILURE;
   }
   json_object_object_add(obj, "bot_token", json_object_new_string(bot_token));
   const char *creds_str = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
   if (creds_str && dc_init(creds_str) == SUCCESS) {
      if (messaging_engine_register_driver(&s_discord_driver) == MESSAGING_SUCCESS) {
         rc = SUCCESS;
      } else {
         OLOG_ERROR("discord: driver registration with engine failed");
         dc_shutdown(); /* roll back the listener we just spawned */
      }
   }
   json_object_put(obj);
   return rc;
}

void messaging_discord_shutdown(void) {
   dc_shutdown();
}
