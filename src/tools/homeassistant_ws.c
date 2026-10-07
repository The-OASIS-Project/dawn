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
 * Home Assistant realtime WebSocket listener.
 *
 * Real implementation is compiled only when DAWN_ENABLE_HA_REALTIME is defined
 * (i.e. libwebsockets is available via ENABLE_WEBUI); otherwise the no-op stub
 * below keeps every build preset linking. See homeassistant_ws.h.
 *
 * Structure mirrors the Discord Gateway driver (src/messaging/messaging_discord.c):
 * a single listener thread owns the lws context + all WS state (lock-free,
 * single-owner); reconnect/backoff via ws_reconnect.c. HA's handshake
 * (auth_required -> auth -> auth_ok) maps onto Discord's HELLO -> IDENTIFY ->
 * READY. Liveness is a self-clocked app-level ping/pong (HA sends no server
 * heartbeat, unlike Discord).
 *
 * This is the READ/EVENT plane only. Commands stay on the REST path
 * (homeassistant_service.c) and the REST-owned s_ha.connected flag is NEVER
 * touched here — the WS plane owns a SEPARATE s_ws_connected so a WS reconnect
 * backoff can't disable command execution.
 */

#include "tools/homeassistant_ws.h"

#include "dawn_error.h"
#include "logging.h"

#ifndef DAWN_ENABLE_HA_REALTIME

/* ---- Build-disabled stub (no libwebsockets) --------------------------------
 * Realtime is unavailable; callers still link and the HA integration works in
 * its REST/poll mode. */

int homeassistant_ws_start(bool insecure_tls) {
   (void)insecure_tls;
   OLOG_INFO(
       "Home Assistant realtime: not compiled in (needs libwebsockets/WebUI); using REST poll");
   return SUCCESS;
}

void homeassistant_ws_stop(void) {
}

bool homeassistant_ws_is_connected(void) {
   return false;
}

#else /* DAWN_ENABLE_HA_REALTIME */

#include <inttypes.h>
#include <json-c/json.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "core/ws_reconnect.h"
#include "tools/homeassistant_service.h"

/* =============================================================================
 * Layer-3 -> Layer-4 hooks (weak no-op defaults; strong overrides in the WebUI
 * layer for the browser delta, and later in a SAGE adapter for observe).
 * ============================================================================= */
void __attribute__((weak)) ha_broadcast_state_changed(struct json_object *dirty_map) {
   (void)dirty_map;
}
void __attribute__((weak))
ha_observe_state_changed(const char *entity_id, const char *old_state, const char *new_state) {
   (void)entity_id;
   (void)old_state;
   (void)new_state;
}

/* =============================================================================
 * Constants
 * ============================================================================= */

#define HA_WS_PATH "/api/websocket"
#define HA_WS_LISTENER_STACK (64 * 1024)
#define HA_WS_RX_BUF_INITIAL 8192
/* Hard cap on a single reassembled frame. Must comfortably exceed the seed:
 * HA delivers get_states (the whole-home snapshot) as ONE WS message, and the
 * REST /api/states ceiling is 2 MB (HA_MAX_RESPONSE_SIZE). Size >= 2x so a
 * large instance's seed is never droppable (a dropped seed would livelock
 * reconnect+reseed). Registry lists are the #2 largest frame class. */
#define HA_WS_RX_BUF_MAX (4 * 1024 * 1024)
/* After a large seed/registry frame, release the buffer back to this so the
 * steady state (tiny pong/event frames) doesn't hold multiple MB resident. */
#define HA_WS_RX_BUF_SHRINK_THRESHOLD (64 * 1024)
#define HA_WS_TX_BUF_MAX 4096
#define HA_WS_SERVICE_TIMEOUT_MS 50
/* Self-clocked liveness: HA's WS heartbeat is a client-sent ping. */
#define HA_WS_PING_INTERVAL_MS 25000
#define HA_WS_PONG_TIMEOUT_MS 10000
/* Coalesce live state_changed events: flush the dirty set after this much
 * quiescence, but never hold a change longer than the max-latency cap (bounds a
 * scene flip of many entities to one broadcast). */
#define HA_WS_COALESCE_MS 200
#define HA_WS_COALESCE_MAX_MS 500
/* Cap the dirty set so a hostile/misbehaving HA event flood can't grow it
 * unbounded: at LIVE this forces a flush; pre-LIVE (during the handshake, when
 * we can't flush yet) new entities past the cap are dropped. */
#define HA_WS_DIRTY_MAX 2048
/* If the handshake (auth -> subscribe -> registries -> seed -> LIVE) doesn't
 * complete within this window, a stalling/hostile HA is holding us pre-LIVE —
 * reconnect rather than buffer forever. */
#define HA_WS_HANDSHAKE_TIMEOUT_MS 30000

/* lws callback returns -1 to close — the lws API contract, NOT DAWN's
 * SUCCESS/FAILURE convention (mirrors messaging_discord.c:132). */
#define LWS_CLOSE_CONNECTION (-1)

/* What the next CLIENT_WRITEABLE should send. Bitmask (OR-in) so a ping
 * scheduled while an auth is still pending doesn't clobber it. Drained one op
 * per callback in priority order: ping > auth > command. */
enum ha_pending_op {
   HA_PENDING_NONE = 0,
   HA_PENDING_AUTH = 1 << 0,
   HA_PENDING_PING = 1 << 1,
   HA_PENDING_COMMAND = 1 << 2, /* a simple {"id":N,"type":<s_pending_command>} request */
};

/* Connect handshake stages. The listener advances through these one result frame
 * at a time (single-pending-id correlation, not a concurrent map): fetch the
 * three registries (for area enrichment), then seed the entity cache, then LIVE.
 * (A later phase inserts a subscribe stage before the registries.) */
enum ha_conn_stage {
   HA_STAGE_PREAUTH = 0,
   HA_STAGE_SUBSCRIBE, /* subscribe FIRST so events buffer during the seed window */
   HA_STAGE_AREA_REG,
   HA_STAGE_ENTITY_REG,
   HA_STAGE_DEVICE_REG,
   HA_STAGE_GET_STATES,
   HA_STAGE_LIVE,
};

/* =============================================================================
 * State (owned by the listener thread unless marked atomic)
 * ============================================================================= */

static atomic_bool s_running = ATOMIC_VAR_INIT(false);
/* READ-plane liveness: authed AND pong-fresh. DELIBERATELY separate from the
 * REST-plane s_ha.connected (see file header / invariant). */
static atomic_bool s_ws_connected = ATOMIC_VAR_INIT(false);
/* Deferred-close signal — set inside a callback or off-thread, drained in the
 * listener loop (never call LWS_TO_KILL_SYNC from inside a callback; see the
 * messaging_discord.c:144 UAF note). */
static atomic_bool s_reconnect_requested = ATOMIC_VAR_INIT(false);

static pthread_t s_listener_thread;
static bool s_listener_started = false;
static bool s_insecure_tls = false;

static struct lws_context *s_lws_context = NULL;
static struct lws *s_wsi = NULL;

static ws_reconnect_state_t s_reconnect;

/* RX accumulator for fragmented frames. */
static char *s_rx_buf = NULL;
static size_t s_rx_buf_len = 0;
static size_t s_rx_buf_cap = 0;

/* TX — what to send on the next CLIENT_WRITEABLE. */
static unsigned int s_pending_op = HA_PENDING_NONE;
static unsigned char s_tx_buf[LWS_PRE + HA_WS_TX_BUF_MAX];
static size_t s_tx_payload_len = 0;
static bool s_tx_is_auth = false; /* the pending TX frame carries the token */

/* Handshake / liveness state. */
static bool s_authed = false;
static int64_t s_handshake_deadline_ms = 0; /* reconnect if not LIVE by this time */
static int64_t s_next_id = 1;               /* HA command id counter, reset per connection */
static int64_t s_expected_result_id = -1;   /* id of the outstanding command result, -1 = none */
static int s_conn_stage = HA_STAGE_PREAUTH;
static const char *s_pending_command = NULL; /* command type to build in WRITEABLE */
/* Registry results held (incref'd) across the sequential fetch until the join. */
static struct json_object *s_area_reg = NULL;
static struct json_object *s_entity_reg = NULL;
static struct json_object *s_device_reg = NULL;
static int64_t s_ping_interval_ms = 0;
static int64_t s_ping_next_due_ms = 0;
static int64_t s_last_pong_ms = 0;
static bool s_awaiting_pong = false;

/* Live-event coalescing: dirty set { entity_id -> new_state|null }, deduped by
 * entity_id (last event wins). Buffered pre-LIVE (during the seed window) and
 * coalesced while LIVE; flushed on quiescence / max-latency. */
static struct json_object *s_dirty = NULL;
static int64_t s_flush_due_ms = 0;      /* quiescence deadline (0 = none pending) */
static int64_t s_flush_deadline_ms = 0; /* hard max-latency deadline */

/* =============================================================================
 * Helpers
 * ============================================================================= */

static int64_t now_monotonic_ms(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void secure_zero(void *ptr, size_t len) {
#if defined(__linux__) || defined(__unix__)
   explicit_bzero(ptr, len);
#else
   volatile unsigned char *p = ptr;
   while (len--) {
      *p++ = 0;
   }
#endif
}

static void rx_buf_reset(void) {
   s_rx_buf_len = 0;
   if (s_rx_buf_cap > HA_WS_RX_BUF_SHRINK_THRESHOLD) {
      free(s_rx_buf);
      s_rx_buf = NULL;
      s_rx_buf_cap = 0;
   }
}

static int rx_buf_append(const void *data, size_t len) {
   /* +1 reserves the trailing NUL: the capacity clamps to HA_WS_RX_BUF_MAX, so
    * admitting exactly MAX bytes would make the terminator write one past the
    * allocation. Cap the usable frame at MAX-1. */
   if (s_rx_buf_len + len + 1 > HA_WS_RX_BUF_MAX) {
      OLOG_WARNING(
          "ha_ws: rx frame would exceed %d bytes; dropping (raise cap if this is the seed)",
          HA_WS_RX_BUF_MAX);
      rx_buf_reset();
      return FAILURE;
   }
   if (s_rx_buf_len + len + 1 > s_rx_buf_cap) {
      size_t new_cap = s_rx_buf_cap ? s_rx_buf_cap * 2 : HA_WS_RX_BUF_INITIAL;
      while (new_cap < s_rx_buf_len + len + 1) {
         new_cap *= 2;
      }
      if (new_cap > HA_WS_RX_BUF_MAX) {
         new_cap = HA_WS_RX_BUF_MAX;
      }
      char *nb = realloc(s_rx_buf, new_cap);
      if (!nb) {
         OLOG_ERROR("ha_ws: rx buffer realloc failed");
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

/* Parse an http(s):// base URL into a ws target: host, port, TLS flag.
 * The path is fixed (HA_WS_PATH) — this assumes HA is served at the domain root
 * (the common case; the REST path preserves a configured subpath, so an HA
 * behind a reverse-proxy subpath would need path-prefix derivation here too).
 * No hostname allowlist — a self-hosted HA is a user-configured LAN host, so the
 * config URL IS the trust anchor. */
static int derive_ws_target(const char *base_url,
                            char *host_out,
                            size_t host_len,
                            int *port_out,
                            bool *ssl_out) {
   if (!base_url || !host_out || !port_out || !ssl_out) {
      return FAILURE;
   }
   const char *p = base_url;
   bool ssl;
   if (strncmp(p, "https://", 8) == 0) {
      ssl = true;
      p += 8;
   } else if (strncmp(p, "http://", 7) == 0) {
      ssl = false;
      p += 7;
   } else {
      return FAILURE;
   }
   size_t hlen = strcspn(p, ":/");
   if (hlen == 0 || hlen >= host_len) {
      return FAILURE;
   }
   memcpy(host_out, p, hlen);
   host_out[hlen] = '\0';

   int port = ssl ? 443 : 80;
   const char *rest = p + hlen;
   if (*rest == ':') {
      long v = strtol(rest + 1, NULL, 10);
      if (v <= 0 || v > 65535) {
         return FAILURE;
      }
      port = (int)v;
   }
   *port_out = port;
   *ssl_out = ssl;
   return SUCCESS;
}

/* =============================================================================
 * Outbound payload builders (build into s_tx_buf + LWS_PRE)
 * ============================================================================= */

/* {"type":"auth","access_token":"<token>"} — the token is copied from the
 * service layer, used, and zeroed immediately; the assembled frame in s_tx_buf
 * is zeroed after send (see the WRITEABLE handler / invariant). */
static void prepare_auth_payload(void) {
   char url[512];
   char token[512];
   s_tx_payload_len = 0;
   if (homeassistant_copy_credentials(url, sizeof(url), token, sizeof(token)) != HA_OK) {
      secure_zero(token, sizeof(token));
      OLOG_WARNING("ha_ws: no credentials for auth frame");
      return;
   }
   secure_zero(url, sizeof(url)); /* not needed here (already connected) */

   struct json_object *obj = json_object_new_object();
   if (!obj) {
      secure_zero(token, sizeof(token));
      return;
   }
   json_object_object_add(obj, "type", json_object_new_string("auth"));
   json_object_object_add(obj, "access_token", json_object_new_string(token));
   secure_zero(token, sizeof(token));

   const char *s = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
   size_t len = s ? strlen(s) : 0;
   if (len == 0 || len > HA_WS_TX_BUF_MAX) {
      OLOG_ERROR("ha_ws: auth payload size invalid (%zu)", len);
      json_object_put(obj);
      return;
   }
   /* WebSocket frame sent by length (lws_write), never read as a C string */
   // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
   memcpy(s_tx_buf + LWS_PRE, s, len);
   s_tx_payload_len = len;
   /* Drop json-c's internal copy of the serialized string (which holds the
    * token) as soon as we've copied it into the TX buffer. */
   json_object_put(obj);
}

/* {"id":N,"type":"ping"} */
static void prepare_ping_payload(void) {
   s_tx_payload_len = 0;
   struct json_object *obj = json_object_new_object();
   if (!obj) {
      return;
   }
   json_object_object_add(obj, "id", json_object_new_int64(s_next_id++));
   json_object_object_add(obj, "type", json_object_new_string("ping"));
   const char *s = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
   size_t len = s ? strlen(s) : 0;
   if (len == 0 || len > HA_WS_TX_BUF_MAX) {
      json_object_put(obj);
      return;
   }
   /* WebSocket frame sent by length (lws_write), never read as a C string */
   // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
   memcpy(s_tx_buf + LWS_PRE, s, len);
   s_tx_payload_len = len;
   json_object_put(obj);
}

/* {"id":N,"type":"<s_pending_command>"} — a simple id-correlated request
 * (registries, get_states). Records the id for the matching result frame. */
static void prepare_command_payload(void) {
   s_tx_payload_len = 0;
   if (!s_pending_command) {
      return;
   }
   int64_t id = s_next_id++;
   struct json_object *obj = json_object_new_object();
   if (!obj) {
      return;
   }
   json_object_object_add(obj, "id", json_object_new_int64(id));
   json_object_object_add(obj, "type", json_object_new_string(s_pending_command));
   if (strcmp(s_pending_command, "subscribe_events") == 0) {
      json_object_object_add(obj, "event_type", json_object_new_string("state_changed"));
   }
   const char *s = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
   size_t len = s ? strlen(s) : 0;
   if (len == 0 || len > HA_WS_TX_BUF_MAX) {
      json_object_put(obj);
      return;
   }
   /* WebSocket frame sent by length (lws_write), never read as a C string */
   // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
   memcpy(s_tx_buf + LWS_PRE, s, len);
   s_tx_payload_len = len;
   s_expected_result_id = id;
   json_object_put(obj);
}

static void schedule_writable(enum ha_pending_op op) {
   s_pending_op |= (unsigned int)op;
   if (s_wsi) {
      lws_callback_on_writable(s_wsi);
   }
}

/* Advance the connect state machine: record the next stage + command type and
 * arm the writeable callback to send it. */
static void send_command(int stage, const char *type) {
   s_conn_stage = stage;
   s_pending_command = type;
   schedule_writable(HA_PENDING_COMMAND);
}

static void release_registries(void) {
   if (s_area_reg) {
      json_object_put(s_area_reg);
      s_area_reg = NULL;
   }
   if (s_entity_reg) {
      json_object_put(s_entity_reg);
      s_entity_reg = NULL;
   }
   if (s_device_reg) {
      json_object_put(s_device_reg);
      s_device_reg = NULL;
   }
}

/* Drop the coalesced dirty set (on reconnect the fresh re-seed supersedes any
 * buffered pre-reconnect events) and clear the flush timers. */
static void drop_dirty(void) {
   if (s_dirty) {
      json_object_put(s_dirty);
      s_dirty = NULL;
   }
   s_flush_due_ms = 0;
   s_flush_deadline_ms = 0;
}

/* =============================================================================
 * Frame dispatch
 * ============================================================================= */

static void on_auth_ok(void) {
   s_authed = true;
   s_next_id = 1;
   /* s_ws_connected + the ping timer + the backoff reset are set only when we
    * reach LIVE (see the get_states result handler), NOT here: is_connected must
    * mean "fully live" so the WebUI reconcile-suppression gate is correct, and
    * resetting backoff at auth would let an unreceivable seed tight-loop. */
   s_handshake_deadline_ms = now_monotonic_ms() + HA_WS_HANDSHAKE_TIMEOUT_MS;
   OLOG_INFO("ha_ws: authenticated; subscribing + fetching registries + seed");
   /* Subscribe FIRST so state_changed events arriving during the registry/seed
    * round-trips are buffered (invariant): they accumulate in s_dirty and are
    * applied on top of the seed when we go LIVE. */
   send_command(HA_STAGE_SUBSCRIBE, "subscribe_events");
}

/* Extract the "state" string from a new_state object (NULL if removed/absent). */
static const char *state_str_of(struct json_object *new_state) {
   if (!new_state || json_object_is_type(new_state, json_type_null)) {
      return NULL;
   }
   struct json_object *st;
   if (json_object_object_get_ex(new_state, "state", &st)) {
      return json_object_get_string(st);
   }
   return NULL;
}

/* Buffer/coalesce a state_changed into the dirty set (deduped by entity_id, so
 * it's naturally bounded by the entity count). Applied + broadcast on the next
 * flush; buffered (no flush) until LIVE so it lands on top of the seed. */
static void handle_state_changed(struct json_object *event_data) {
   struct json_object *eid_obj = NULL;
   if (!json_object_object_get_ex(event_data, "entity_id", &eid_obj)) {
      return;
   }
   const char *eid = json_object_get_string(eid_obj);
   if (!eid || !eid[0] || strlen(eid) >= HA_MAX_ENTITY_ID) {
      return; /* bound the key length (defends against a hostile HA) */
   }
   if (!s_dirty) {
      s_dirty = json_object_new_object();
      if (!s_dirty) {
         return;
      }
   }
   /* Cap growth in EVERY stage. Pre-LIVE we can't flush (would clobber the seed),
    * so a NEW entity past the cap is dropped; an already-present key overwrites
    * (no growth). At LIVE the cap additionally forces a flush below. */
   if (json_object_object_length(s_dirty) >= HA_WS_DIRTY_MAX) {
      struct json_object *existing = NULL;
      if (!json_object_object_get_ex(s_dirty, eid, &existing)) {
         return; /* new entity would exceed the cap — drop it */
      }
   }
   struct json_object *new_state = NULL;
   json_object_object_get_ex(event_data, "new_state",
                             &new_state); /* json null or absent = removal */
   struct json_object *stored = new_state ? json_object_get(new_state) : json_object_new_null();
   json_object_object_add(s_dirty, eid, stored); /* replaces (+ puts) any prior — last wins */

   if (s_conn_stage == HA_STAGE_LIVE) {
      int64_t now = now_monotonic_ms();
      s_flush_due_ms = now + HA_WS_COALESCE_MS;
      if (s_flush_deadline_ms == 0) {
         s_flush_deadline_ms = now + HA_WS_COALESCE_MAX_MS;
      }
      if (json_object_object_length(s_dirty) >= HA_WS_DIRTY_MAX) {
         s_flush_due_ms = now; /* cap reached — flush ASAP */
      }
   }
}

static void handle_ha_frame(const char *data, size_t len) {
   struct json_object *root = json_tokener_parse(data);
   if (!root) {
      OLOG_WARNING("ha_ws: failed to parse frame");
      return;
   }
   struct json_object *type_obj = NULL;
   const char *type = NULL;
   if (json_object_object_get_ex(root, "type", &type_obj) && type_obj) {
      type = json_object_get_string(type_obj);
   }
   if (!type) {
      json_object_put(root);
      return;
   }

   /* Dispatch on type first (auth-phase frames carry no id; events/results are
    * Phase 2+). NEVER log the frame body — the auth handshake is adjacent. */
   if (strcmp(type, "auth_required") == 0) {
      schedule_writable(HA_PENDING_AUTH);
   } else if (strcmp(type, "auth_ok") == 0) {
      on_auth_ok();
   } else if (strcmp(type, "auth_invalid") == 0) {
      OLOG_WARNING("ha_ws: auth_invalid (token rejected — must be a valid ADMIN long-lived token)");
      atomic_store(&s_ws_connected, false);
      atomic_store(&s_reconnect_requested, true); /* back off; not transient */
   } else if (strcmp(type, "pong") == 0) {
      s_awaiting_pong = false;
      s_last_pong_ms = now_monotonic_ms();
   } else if (strcmp(type, "result") == 0) {
      /* Correlate by the single outstanding request id, then advance the stage
       * machine. */
      struct json_object *id_obj = NULL;
      int64_t id = -1;
      if (json_object_object_get_ex(root, "id", &id_obj) && id_obj) {
         id = json_object_get_int64(id_obj);
      }
      if (id < 0 || id != s_expected_result_id) {
         json_object_put(root);
         return;
      }
      s_expected_result_id = -1;

      struct json_object *success_obj = NULL;
      bool ok = true;
      if (json_object_object_get_ex(root, "success", &success_obj)) {
         ok = json_object_get_boolean(success_obj);
      }
      struct json_object *result_obj = NULL;
      bool has_arr = ok && json_object_object_get_ex(root, "result", &result_obj) &&
                     json_object_is_type(result_obj, json_type_array);

      switch (s_conn_stage) {
         case HA_STAGE_SUBSCRIBE:
            /* Subscribed; events now buffer into s_dirty. Fetch registries next.
             * A non-admin token gets success:false here (HA gates subscribe to
             * admins) — log it, since it means no live deltas will ever arrive. */
            if (!ok) {
               OLOG_WARNING("ha_ws: subscribe_events rejected — token likely lacks "
                            "admin; no live updates (board falls back to REST poll)");
            }
            send_command(HA_STAGE_AREA_REG, "config/area_registry/list");
            break;
         case HA_STAGE_AREA_REG:
            if (has_arr) {
               s_area_reg = json_object_get(result_obj);
            }
            send_command(HA_STAGE_ENTITY_REG, "config/entity_registry/list");
            break;
         case HA_STAGE_ENTITY_REG:
            if (has_arr) {
               s_entity_reg = json_object_get(result_obj);
            }
            send_command(HA_STAGE_DEVICE_REG, "config/device_registry/list");
            break;
         case HA_STAGE_DEVICE_REG:
            if (has_arr) {
               s_device_reg = json_object_get(result_obj);
            }
            if (s_area_reg && s_entity_reg && s_device_reg) {
               homeassistant_cache_replace_areas(s_area_reg, s_entity_reg, s_device_reg);
            } else {
               OLOG_WARNING("ha_ws: incomplete registries; areas may be blank until reconnect");
            }
            release_registries();
            send_command(HA_STAGE_GET_STATES, "get_states");
            break;
         case HA_STAGE_GET_STATES:
            if (has_arr) {
               OLOG_INFO("ha_ws: seed frame %zu bytes (rx cap %d)", len, HA_WS_RX_BUF_MAX);
               homeassistant_cache_replace_from_states(result_obj);
               /* Fully connected + seeded: NOW reset the reconnect backoff to
                * floor (see on_auth_ok for why this isn't done at auth time). */
               ws_reconnect_record_success(&s_reconnect, NULL, NULL);
            } else {
               OLOG_WARNING("ha_ws: get_states result unusable (success=%d)", (int)ok);
            }
            s_conn_stage = HA_STAGE_LIVE;
            s_handshake_deadline_ms = 0;
            /* Fully live now: start liveness + mark connected (is_connected means
             * "live", so the WebUI reconcile-suppression gate is correct). */
            {
               int64_t now2 = now_monotonic_ms();
               s_ping_interval_ms = HA_WS_PING_INTERVAL_MS;
               s_ping_next_due_ms = now2 + s_ping_interval_ms;
               s_last_pong_ms = now2;
               s_awaiting_pong = false;
               atomic_store(&s_ws_connected, true);
            }
            OLOG_INFO("ha_ws: live (subscribed to state_changed)");
            /* Apply any events buffered during the seed window on the next flush. */
            if (s_dirty && json_object_object_length(s_dirty) > 0) {
               s_flush_due_ms = now_monotonic_ms(); /* due now */
               if (s_flush_deadline_ms == 0) {
                  s_flush_deadline_ms = now_monotonic_ms();
               }
            }
            break;
         default:
            break;
      }
   } else if (strcmp(type, "event") == 0) {
      struct json_object *event_obj = NULL, *etype_obj = NULL, *data_obj = NULL;
      if (json_object_object_get_ex(root, "event", &event_obj) &&
          json_object_object_get_ex(event_obj, "event_type", &etype_obj)) {
         const char *et = json_object_get_string(etype_obj);
         if (et && strcmp(et, "state_changed") == 0 &&
             json_object_object_get_ex(event_obj, "data", &data_obj)) {
            handle_state_changed(data_obj);
         }
      }
   }
   json_object_put(root);
}

/* =============================================================================
 * libwebsockets callback
 * ============================================================================= */

static int ha_ws_callback(struct lws *wsi,
                          enum lws_callback_reasons reason,
                          void *user,
                          void *in,
                          size_t len) {
   (void)user;

   switch (reason) {
      case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
         OLOG_WARNING("ha_ws: connection error: %s", in ? (const char *)in : "(no detail)");
         atomic_store(&s_ws_connected, false);
         s_wsi = NULL;
         return LWS_CLOSE_CONNECTION;

      case LWS_CALLBACK_CLIENT_ESTABLISHED:
         OLOG_INFO("ha_ws: WebSocket established; awaiting auth_required");
         s_wsi = wsi;
         /* HA sends auth_required first — don't send anything yet. */
         break;

      case LWS_CALLBACK_CLIENT_RECEIVE: {
         if (in && len > 0) {
            if (rx_buf_append(in, len) != SUCCESS) {
               return LWS_CLOSE_CONNECTION;
            }
         }
         if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
            if (s_rx_buf && s_rx_buf_len > 0) {
               handle_ha_frame(s_rx_buf, s_rx_buf_len);
            }
            rx_buf_reset();
         }
         break;
      }

      case LWS_CALLBACK_CLIENT_WRITEABLE: {
         if (s_pending_op == HA_PENDING_NONE) {
            break;
         }
         /* Ping first (missing liveness kills the connection); auth tolerates a
          * few ms of extra latency. One op per callback. */
         enum ha_pending_op sent = HA_PENDING_NONE;
         s_tx_is_auth = false;
         if (s_pending_op & HA_PENDING_PING) {
            prepare_ping_payload();
            s_awaiting_pong = true;
            sent = HA_PENDING_PING;
         } else if (s_pending_op & HA_PENDING_AUTH) {
            prepare_auth_payload();
            s_tx_is_auth = true;
            sent = HA_PENDING_AUTH;
         } else if (s_pending_op & HA_PENDING_COMMAND) {
            prepare_command_payload();
            sent = HA_PENDING_COMMAND;
         } else {
            s_pending_op = HA_PENDING_NONE;
            return 0;
         }
         s_pending_op &= ~(unsigned int)sent;
         if (s_tx_payload_len == 0) {
            return LWS_CLOSE_CONNECTION; /* builder bailed */
         }
         int n = lws_write(wsi, s_tx_buf + LWS_PRE, s_tx_payload_len, LWS_WRITE_TEXT);
         /* Zero the TX region immediately if it held the token (invariant): the
          * buffer is process-lifetime BSS, so leaving the auth frame resident
          * would expose the token to a core dump. */
         if (s_tx_is_auth) {
            secure_zero(s_tx_buf + LWS_PRE, s_tx_payload_len);
         }
         if (n < 0 || (size_t)n < s_tx_payload_len) {
            OLOG_WARNING("ha_ws: lws_write failed (wrote=%d of %zu)", n, s_tx_payload_len);
            return LWS_CLOSE_CONNECTION;
         }
         if (s_pending_op != HA_PENDING_NONE) {
            lws_callback_on_writable(wsi);
         }
         break;
      }

      case LWS_CALLBACK_CLIENT_CLOSED:
         OLOG_INFO("ha_ws: WebSocket closed");
         atomic_store(&s_ws_connected, false);
         s_wsi = NULL;
         rx_buf_reset();
         break;

      default:
         break;
   }
   return 0;
}

static const struct lws_protocols s_ha_protocols[] = {
   { "home-assistant", ha_ws_callback, 0, HA_WS_RX_BUF_INITIAL, 0, NULL, 0 },
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
   char url[512];
   char token[512];
   if (homeassistant_copy_credentials(url, sizeof(url), token, sizeof(token)) != HA_OK) {
      secure_zero(token, sizeof(token));
      OLOG_INFO("ha_ws: not configured; will retry");
      return FAILURE;
   }
   secure_zero(token, sizeof(token)); /* token is re-read when building the auth frame */

   char host[256];
   int port = 0;
   bool ssl = false;
   if (derive_ws_target(url, host, sizeof(host), &port, &ssl) != SUCCESS) {
      OLOG_WARNING("ha_ws: could not derive ws target from configured URL");
      return FAILURE;
   }

   struct lws_client_connect_info i;
   memset(&i, 0, sizeof(i));
   i.context = s_lws_context;
   i.address = host;
   i.host = host;
   i.origin = host;
   i.port = port;
   i.path = HA_WS_PATH;
   if (ssl) {
      i.ssl_connection = LCCSCF_USE_SSL;
      if (s_insecure_tls) {
         i.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK |
                             LCCSCF_ALLOW_INSECURE;
      }
   } else {
      i.ssl_connection = 0; /* plain ws:// — self-hosted HA on the LAN */
   }
   i.protocol = s_ha_protocols[0].name;
   i.pwsi = &s_wsi;

   OLOG_INFO("ha_ws: connecting to %s://%s:%d%s", ssl ? "wss" : "ws", host, port, HA_WS_PATH);
   struct lws *w = lws_client_connect_via_info(&i);
   if (!w) {
      OLOG_WARNING("ha_ws: lws_client_connect_via_info failed");
      return FAILURE;
   }
   return SUCCESS;
}

static void close_active_connection(void) {
   if (s_wsi) {
      lws_set_timeout(s_wsi, PENDING_TIMEOUT_USER_OK, LWS_TO_KILL_SYNC);
   }
}

/* Self-clocked liveness: send a ping every interval; if a pong doesn't arrive
 * within the timeout, the socket is half-open -> close, backoff, reconnect. */
static void check_liveness(void) {
   int64_t now = now_monotonic_ms();
   /* Handshake stall guard: authed but never reached LIVE within the timeout
    * (a stalling/hostile HA that never returns results, keeping us buffering
    * pre-LIVE) — close and reconnect. */
   if (s_authed && s_conn_stage != HA_STAGE_LIVE && s_handshake_deadline_ms != 0 &&
       now > s_handshake_deadline_ms) {
      OLOG_WARNING("ha_ws: handshake stalled (no LIVE within timeout); reconnecting");
      close_active_connection();
      return;
   }
   if (!s_authed || s_ping_interval_ms <= 0) {
      return;
   }
   if (s_awaiting_pong && now - s_last_pong_ms > HA_WS_PONG_TIMEOUT_MS + s_ping_interval_ms) {
      OLOG_WARNING("ha_ws: pong timeout — connection half-open, closing for reconnect");
      s_awaiting_pong = false;
      s_ping_interval_ms = 0;
      atomic_store(&s_ws_connected, false);
      close_active_connection();
      return;
   }
   if (now >= s_ping_next_due_ms && atomic_load(&s_ws_connected)) {
      s_ping_next_due_ms = now + s_ping_interval_ms;
      schedule_writable(HA_PENDING_PING);
   }
}

/* Flush the coalesced dirty set: apply to the cache, feed the SAGE seam, and
 * broadcast a delta to the admin board. Runs on the listener thread between
 * lws_service calls, so s_dirty is single-owner and lock-free here. */
static void check_flush(void) {
   if (!s_dirty || s_conn_stage != HA_STAGE_LIVE) {
      return;
   }
   if (json_object_object_length(s_dirty) == 0) {
      return;
   }
   int64_t now = now_monotonic_ms();
   bool due = (s_flush_due_ms != 0 && now >= s_flush_due_ms) ||
              (s_flush_deadline_ms != 0 && now >= s_flush_deadline_ms);
   if (!due) {
      return;
   }

   struct json_object *batch = s_dirty;
   s_dirty = NULL;
   s_flush_due_ms = 0;
   s_flush_deadline_ms = 0;

   /* Three SEQUENTIAL, non-overlapping critical sections (never nested):
    * 1) apply to the cache (wrlock, internal to the service),
    * 2) SAGE observe (leaf mutex, internal; no-op weak default today),
    * 3) broadcast the delta to admins (registry mutex, internal to WebUI).
    * The broadcast reads the freshly-applied cache, so it must run after 1. */
   homeassistant_cache_apply_batch(batch);
   json_object_object_foreach(batch, key, val) {
      ha_observe_state_changed(key, NULL, state_str_of(val));
   }
   ha_broadcast_state_changed(batch);
   json_object_put(batch);
}

static void *ha_ws_listener_thread(void *arg) {
   (void)arg;
   OLOG_INFO("ha_ws: listener thread started");

   while (atomic_load(&s_running)) {
      if (!s_wsi) {
         atomic_store(&s_ws_connected, false);
         /* Reset per-connection transient state (keep reconnect backoff). */
         s_authed = false;
         s_handshake_deadline_ms = 0;
         s_ping_interval_ms = 0;
         s_awaiting_pong = false;
         s_pending_op = HA_PENDING_NONE;
         s_next_id = 1;
         s_expected_result_id = -1;
         s_conn_stage = HA_STAGE_PREAUTH;
         s_pending_command = NULL;
         release_registries();
         drop_dirty();
         rx_buf_reset();

         int delay = ws_reconnect_next_delay_seconds(&s_reconnect);
         if (delay > 0) {
            interruptible_sleep_seconds(delay);
         }
         if (!atomic_load(&s_running)) {
            break;
         }
         if (attempt_connect() != SUCCESS) {
            continue; /* backoff already advanced */
         }
      }

      if (atomic_exchange(&s_reconnect_requested, false)) {
         close_active_connection();
      }

      int n = lws_service(s_lws_context, HA_WS_SERVICE_TIMEOUT_MS);
      if (n < 0) {
         OLOG_WARNING("ha_ws: lws_service returned %d; closing", n);
         close_active_connection();
      }
      check_liveness();
      check_flush();
   }

   close_active_connection();
   while (s_wsi) {
      lws_service(s_lws_context, 50);
   }
   atomic_store(&s_ws_connected, false);
   OLOG_INFO("ha_ws: listener thread exiting");
   return NULL;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int homeassistant_ws_start(bool insecure_tls) {
   if (s_listener_started) {
      return SUCCESS; /* idempotent */
   }
   s_insecure_tls = insecure_tls;

   struct lws_context_creation_info info;
   memset(&info, 0, sizeof(info));
   info.port = CONTEXT_PORT_NO_LISTEN;
   info.protocols = s_ha_protocols;
   info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
   info.gid = (gid_t)-1;
   info.uid = (uid_t)-1;
   info.user = NULL;

   s_lws_context = lws_create_context(&info);
   if (!s_lws_context) {
      OLOG_ERROR("ha_ws: lws_create_context failed");
      return FAILURE;
   }

   ws_reconnect_init(&s_reconnect);
   atomic_store(&s_running, true);

   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setstacksize(&attr, HA_WS_LISTENER_STACK);
   int prc = pthread_create(&s_listener_thread, &attr, ha_ws_listener_thread, NULL);
   pthread_attr_destroy(&attr);
   if (prc != 0) {
      atomic_store(&s_running, false);
      lws_context_destroy(s_lws_context);
      s_lws_context = NULL;
      OLOG_ERROR("ha_ws: failed to spawn listener thread (rc=%d)", prc);
      return FAILURE;
   }
   s_listener_started = true;
   OLOG_INFO("ha_ws: realtime listener initialized");
   return SUCCESS;
}

void homeassistant_ws_stop(void) {
   if (!s_listener_started && !s_lws_context) {
      return;
   }
   atomic_store(&s_running, false);
   if (s_lws_context) {
      lws_cancel_service(s_lws_context); /* wake the poll to observe s_running */
   }
   if (s_listener_started) {
      pthread_join(s_listener_thread, NULL);
      s_listener_started = false;
   }
   if (s_lws_context) {
      lws_context_destroy(s_lws_context);
      s_lws_context = NULL;
   }
   free(s_rx_buf);
   s_rx_buf = NULL;
   s_rx_buf_cap = 0;
   s_rx_buf_len = 0;
   release_registries();
   drop_dirty();
   secure_zero(s_tx_buf, sizeof(s_tx_buf));
   atomic_store(&s_ws_connected, false);
   OLOG_INFO("ha_ws: realtime listener stopped");
}

bool homeassistant_ws_is_connected(void) {
   return atomic_load(&s_ws_connected);
}

#endif /* DAWN_ENABLE_HA_REALTIME */
