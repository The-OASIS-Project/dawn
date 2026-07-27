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
 * Messaging engine implementation.  See
 * include/messaging/messaging_engine.h for the public API and
 * docs/MESSAGING_CHANNELS_DESIGN.md for the design rationale.
 */
#define MESSAGING_ENGINE_INTERNAL_ALLOWED

#include "messaging/messaging_engine.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/rate_limiter.h"
#include "core/scheduler.h" /* prototype for the scheduler_send_to_messaging_channel override */
#include "core/session_manager.h"
#include "dawn_error.h"
#include "logging.h"
#include "messaging/messaging_engine_internal.h"
#include "messaging/messaging_format.h"
#include "messaging/messaging_split.h"

/* =============================================================================
 * Internal constants and types
 * ============================================================================= */

/* Shared constants (MESSAGING_INBOUND_QUEUE_DEPTH, MESSAGING_MAX_SESSIONS,
 * MESSAGING_ADDRESS_JSON_BUF_SIZE) and the inbound_item_t / session_slot_t
 * types live in messaging_engine_internal.h.  File-local sizing constants live
 * in the .c that owns them: the inbound body/key/hint caps in
 * messaging_engine_inbound.c, the async-send cap + Crockford alphabet in
 * messaging_engine_link.c. */
#define MESSAGING_MAX_DRIVERS 4

/* Rate limiter slot counts — these MUST match the static array
 * declarations below (s_inbound_link_entries[N] etc.).  If you change
 * one, change both. */
#define MESSAGING_RL_LINK_SLOTS 64
#define MESSAGING_RL_GENERAL_SLOTS 128
#define MESSAGING_RL_OUTBOUND_SLOTS 64
#define MESSAGING_RL_OUTBOUND_MAX_COUNT 10 /* outbound sends per window, per channel */
#define MESSAGING_RL_OUTBOUND_WINDOW_SEC 60
#define MESSAGING_RL_READ_SLOTS 32
#define MESSAGING_RL_READ_MAX_COUNT 10 /* single-channel reads per window, per user */
#define MESSAGING_RL_READ_WINDOW_SEC 60
/* Whole-server sweeps fan out to ~30 fetches each, so they get a stricter
 * per-user budget than single-channel reads. */
#define MESSAGING_RL_READ_SERVER_SLOTS 32
#define MESSAGING_RL_READ_SERVER_MAX_COUNT 3
#define MESSAGING_RL_READ_SERVER_WINDOW_SEC 60

/* =============================================================================
 * Module state
 *
 * The cross-file state (init/shutdown flags, inbound queue, session-slot map,
 * rate limiters) is DEFINED here and declared `extern` in
 * messaging_engine_internal.h.  The driver registry, worker-thread handle, and
 * rate-limiter entry-storage arrays stay file-local — only this file touches
 * them by name.
 * ============================================================================= */

atomic_bool s_initialized = ATOMIC_VAR_INIT(false);
atomic_bool s_shutdown_requested = ATOMIC_VAR_INIT(false);

static const messaging_driver_t *s_drivers[MESSAGING_MAX_DRIVERS];
static size_t s_num_drivers = 0;
static pthread_mutex_t s_drivers_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Inbound queue (bounded ring buffer). */
inbound_item_t *s_inbound_queue[MESSAGING_INBOUND_QUEUE_DEPTH];
size_t s_inbound_head = 0;
size_t s_inbound_tail = 0;
size_t s_inbound_count = 0;
pthread_mutex_t s_inbound_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t s_inbound_cond = PTHREAD_COND_INITIALIZER;

static pthread_t s_worker_thread;
static bool s_worker_started = false;

/* In-memory session map: (provider, provider_address) → session_t*.
 * Linear scan; v1 scale is small (one slot per active conversation per
 * user, typically < 10). */
session_slot_t s_session_slots[MESSAGING_MAX_SESSIONS];
pthread_mutex_t s_session_slots_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Rate limiters.  Slot counts live in MESSAGING_RL_*_SLOTS so the
 * array decls below and the slot_count fields in the rate_limiter
 * configs (messaging_engine_init) can't drift apart. */
static rate_limit_entry_t s_inbound_link_entries[MESSAGING_RL_LINK_SLOTS];
static rate_limit_entry_t s_inbound_general_entries[MESSAGING_RL_GENERAL_SLOTS];
static rate_limit_entry_t s_outbound_per_user_entries[MESSAGING_RL_OUTBOUND_SLOTS];
static rate_limit_entry_t s_read_per_user_entries[MESSAGING_RL_READ_SLOTS];
static rate_limit_entry_t s_read_server_entries[MESSAGING_RL_READ_SERVER_SLOTS];

rate_limiter_t s_inbound_link_limiter;
rate_limiter_t s_inbound_general_limiter;
rate_limiter_t s_outbound_per_user_limiter;
rate_limiter_t s_read_per_user_limiter;
rate_limiter_t s_read_server_limiter;

/* =============================================================================
 * Weak symbol — WebUI broadcast on conversation append.  Defined here
 * as a no-op so the messaging engine has no hard dependency on the
 * WebUI layer (Layer 2 → Layer 4 violation otherwise).
 * src/webui/webui_broadcasts.c provides the strong override that
 * actually pushes a JSON message to the user's open WebUI sessions;
 * when WebUI isn't linked, this no-op stub is the binding instead.
 * ============================================================================= */
void webui_broadcast_conversation_messages_appended(int user_id, int64_t conv_id)
    __attribute__((weak));
void webui_broadcast_conversation_messages_appended(int user_id, int64_t conv_id) {
   (void)user_id;
   (void)conv_id;
}

/* =============================================================================
 * Strong override: scheduler → messaging channel fan-out (Phase 5, schema v54)
 *
 * Sixth scheduler weak symbol per docs/MESSAGING_CHANNELS_DESIGN.md §9.
 * The scheduler-side weak no-op lives in src/core/scheduler.c; this
 * strong definition wins when messaging_engine is linked.  Delegates to
 * messaging_engine_send, which:
 *
 *   1. Verifies the channel belongs to user_id (prevents cross-user
 *      leak via a guessed channel name on a stale event row),
 *   2. Applies per-channel + provider-global rate limits, and
 *   3. Dispatches through the registered driver's send_text.
 *
 * Failures are logged but NOT propagated — the briefing's TTS + WebUI
 * banner have already fired by the time we get here, so a messaging
 * dispatch failure shouldn't poison the announcement.  Operators see
 * the failure in the engine log.
 * ============================================================================= */

/* Symbolic name for an engine return code — used in operator-facing log
 * lines so a Phase 6 panel reviewer doesn't need to cross-reference
 * messaging_engine.h to interpret rc=4. */
static const char *engine_rc_name(int rc) {
   switch (rc) {
      case MESSAGING_SUCCESS:
         return "SUCCESS";
      case MESSAGING_FAILURE:
         return "FAILURE";
      case MESSAGING_UNKNOWN_CHANNEL:
         return "UNKNOWN_CHANNEL";
      case MESSAGING_UNKNOWN_USER:
         return "UNKNOWN_USER";
      case MESSAGING_RATE_LIMITED:
         return "RATE_LIMITED";
      case MESSAGING_PROVIDER_RATE_LIMITED:
         return "PROVIDER_RATE_LIMITED";
      case MESSAGING_DRIVER_NOT_REGISTERED:
         return "DRIVER_NOT_REGISTERED";
      case MESSAGING_INVALID_ADDRESS:
         return "INVALID_ADDRESS";
      default:
         return "UNKNOWN";
   }
}

int scheduler_send_to_messaging_channel(int user_id, const char *channel_name, const char *text) {
   if (user_id <= 0 || !channel_name || channel_name[0] == '\0' || !text) {
      return FAILURE;
   }
   int rc = messaging_engine_send(user_id, channel_name, text);
   if (rc == MESSAGING_SUCCESS) {
      OLOG_INFO("messaging: scheduler fan-out to '%s' for user %d delivered", channel_name,
                user_id);
      return SUCCESS;
   }
   OLOG_WARNING("messaging: scheduler fan-out to '%s' for user %d failed (rc=%d %s)", channel_name,
                user_id, rc, engine_rc_name(rc));
   return FAILURE;
}

/* =============================================================================
 * Init / shutdown
 * ============================================================================= */

int messaging_engine_init(void) {
   if (atomic_load(&s_initialized)) {
      return MESSAGING_SUCCESS;
   }

   memset(s_drivers, 0, sizeof(s_drivers));
   memset(s_inbound_queue, 0, sizeof(s_inbound_queue));
   memset(s_session_slots, 0, sizeof(s_session_slots));

   /* Rate limiters.  Budgets per docs/MESSAGING_CHANNELS_DESIGN.md §12. */
   rate_limiter_config_t link_cfg = { .max_count = 5,
                                      .window_sec = 600,
                                      .slot_count = MESSAGING_RL_LINK_SLOTS };
   rate_limiter_config_t general_cfg = { .max_count = 60,
                                         .window_sec = 600,
                                         .slot_count = MESSAGING_RL_GENERAL_SLOTS };
   rate_limiter_config_t outbound_cfg = { .max_count = MESSAGING_RL_OUTBOUND_MAX_COUNT,
                                          .window_sec = MESSAGING_RL_OUTBOUND_WINDOW_SEC,
                                          .slot_count = MESSAGING_RL_OUTBOUND_SLOTS };
   /* Channel reads: bounded per user (REST + LLM summarization is heavier
    * than a send, so a tighter budget). */
   rate_limiter_config_t read_cfg = { .max_count = MESSAGING_RL_READ_MAX_COUNT,
                                      .window_sec = MESSAGING_RL_READ_WINDOW_SEC,
                                      .slot_count = MESSAGING_RL_READ_SLOTS };
   rate_limiter_config_t read_server_cfg = { .max_count = MESSAGING_RL_READ_SERVER_MAX_COUNT,
                                             .window_sec = MESSAGING_RL_READ_SERVER_WINDOW_SEC,
                                             .slot_count = MESSAGING_RL_READ_SERVER_SLOTS };

   memset(s_inbound_link_entries, 0, sizeof(s_inbound_link_entries));
   memset(s_inbound_general_entries, 0, sizeof(s_inbound_general_entries));
   memset(s_outbound_per_user_entries, 0, sizeof(s_outbound_per_user_entries));
   memset(s_read_per_user_entries, 0, sizeof(s_read_per_user_entries));
   memset(s_read_server_entries, 0, sizeof(s_read_server_entries));

   rate_limiter_init(&s_inbound_link_limiter, s_inbound_link_entries, &link_cfg);
   rate_limiter_init(&s_inbound_general_limiter, s_inbound_general_entries, &general_cfg);
   rate_limiter_init(&s_outbound_per_user_limiter, s_outbound_per_user_entries, &outbound_cfg);
   rate_limiter_init(&s_read_per_user_limiter, s_read_per_user_entries, &read_cfg);
   rate_limiter_init(&s_read_server_limiter, s_read_server_entries, &read_server_cfg);

   /* Worker thread for the inbound drain. */
   atomic_store(&s_shutdown_requested, false);
   if (pthread_create(&s_worker_thread, NULL, messaging_worker_thread, NULL) != 0) {
      OLOG_ERROR("messaging_engine: failed to spawn worker thread");
      return MESSAGING_FAILURE;
   }
   s_worker_started = true;

   atomic_store(&s_initialized, true);
   OLOG_INFO("messaging_engine: initialized (worker thread running, "
             "drivers will self-register)");
   return MESSAGING_SUCCESS;
}

void messaging_engine_shutdown(void) {
   if (!atomic_load(&s_initialized)) {
      return;
   }

   atomic_store(&s_shutdown_requested, true);

   /* Wake worker so it can exit. */
   pthread_mutex_lock(&s_inbound_mutex);
   pthread_cond_broadcast(&s_inbound_cond);
   pthread_mutex_unlock(&s_inbound_mutex);

   if (s_worker_started) {
      pthread_join(s_worker_thread, NULL);
      s_worker_started = false;
   }

   /* Destroy any retained sessions.  Using session_destroy (vs the
    * old session_release-only path) triggers memory extraction for
    * the closing conversation via session_destroy's existing hook —
    * so in-flight forever-conversations get their facts extracted on
    * graceful shutdown.  Cost: up to 3 sec per slot waiting for
    * ref_count to converge, × MESSAGING_MAX_SESSIONS=64 worst-case.
    * In practice only a handful of slots are active, and ref_count
    * is already 1 (engine's retain) since the worker thread joined
    * above — destroy returns essentially instantly per slot.  We
    * capture the session_ids under the lock, drop it, then call
    * session_destroy outside (avoids the per-module → global lock
    * inversion that the eviction path already documented). */
   pthread_mutex_lock(&s_session_slots_mutex);
   uint32_t shutdown_session_ids[MESSAGING_MAX_SESSIONS];
   size_t shutdown_session_count = 0;
   for (size_t i = 0; i < MESSAGING_MAX_SESSIONS; i++) {
      if (s_session_slots[i].session) {
         shutdown_session_ids[shutdown_session_count++] = s_session_slots[i].session->session_id;
         session_release(s_session_slots[i].session);
         memset(&s_session_slots[i], 0, sizeof(session_slot_t));
      }
   }
   pthread_mutex_unlock(&s_session_slots_mutex);

   for (size_t i = 0; i < shutdown_session_count; i++) {
      session_destroy(shutdown_session_ids[i]);
   }

   /* Drain remaining items in the queue. */
   pthread_mutex_lock(&s_inbound_mutex);
   while (s_inbound_count > 0) {
      inbound_item_t *item = s_inbound_queue[s_inbound_head];
      s_inbound_head = (s_inbound_head + 1) % MESSAGING_INBOUND_QUEUE_DEPTH;
      s_inbound_count--;
      if (item) {
         free(item->body);
         free(item);
      }
   }
   pthread_mutex_unlock(&s_inbound_mutex);

   atomic_store(&s_initialized, false);
   OLOG_INFO("messaging_engine: shutdown complete");
}

/* =============================================================================
 * Driver registry
 * ============================================================================= */

int messaging_engine_register_driver(const messaging_driver_t *driver) {
   if (!driver || !driver->name || !driver->init || !driver->send_text) {
      return MESSAGING_FAILURE;
   }
   if (!atomic_load(&s_initialized)) {
      OLOG_ERROR("messaging_engine: register_driver called before init");
      return MESSAGING_FAILURE;
   }

   pthread_mutex_lock(&s_drivers_mutex);

   /* Reject duplicates. */
   for (size_t i = 0; i < s_num_drivers; i++) {
      if (strcmp(s_drivers[i]->name, driver->name) == 0) {
         pthread_mutex_unlock(&s_drivers_mutex);
         OLOG_WARNING("messaging_engine: driver '%s' already registered", driver->name);
         return MESSAGING_FAILURE;
      }
   }

   if (s_num_drivers >= MESSAGING_MAX_DRIVERS) {
      pthread_mutex_unlock(&s_drivers_mutex);
      OLOG_ERROR("messaging_engine: registry full (max %d)", MESSAGING_MAX_DRIVERS);
      return MESSAGING_FAILURE;
   }

   s_drivers[s_num_drivers++] = driver;
   pthread_mutex_unlock(&s_drivers_mutex);

   /* Plumb the engine's inbound dispatch into the driver before its
    * init runs (so the listener thread can call back immediately). */
   if (driver->register_inbound_cb) {
      driver->register_inbound_cb(engine_inbound_dispatch);
   }

   OLOG_INFO("messaging_engine: registered driver '%s'", driver->name);
   return MESSAGING_SUCCESS;
}

const messaging_driver_t *find_driver(const char *name) {
   if (!name) {
      return NULL;
   }
   const messaging_driver_t *result = NULL;
   pthread_mutex_lock(&s_drivers_mutex);
   for (size_t i = 0; i < s_num_drivers; i++) {
      if (strcmp(s_drivers[i]->name, name) == 0) {
         result = s_drivers[i];
         break;
      }
   }
   pthread_mutex_unlock(&s_drivers_mutex);
   return result;
}

const messaging_driver_t *find_read_capable_driver(void) {
   const messaging_driver_t *result = NULL;
   pthread_mutex_lock(&s_drivers_mutex);
   for (size_t i = 0; i < s_num_drivers; i++) {
      if (s_drivers[i]->list_readable_channels && s_drivers[i]->read_history) {
         result = s_drivers[i];
         break;
      }
   }
   pthread_mutex_unlock(&s_drivers_mutex);
   return result;
}

/* Engine-cap headroom for the "(NN/NN) " split prefix.  Mirrors the 20-char
 * margin provider_outbound_for leaves below each provider's hard limit. */
#define MESSAGING_PREFIX_HEADROOM 20

int messaging_deliver(const messaging_driver_t *drv,
                      int user_id,
                      const char *provider_address,
                      const char *address_json,
                      const char *canonical_markdown) {
   if (!drv || !drv->send_text || !canonical_markdown) {
      return MESSAGING_FAILURE;
   }

   provider_outbound_t cfg = provider_outbound_for(drv->name);
   size_t cap = cfg.max_outbound_chars;
   if (cap == 0) {
      /* No engine cap configured for this provider.  Every registered v1
       * driver sets one; a zero means a driver was added without a
       * provider_outbound_for branch — warn so it's caught, and fall back to
       * a generous single-message cap so formatting still happens. */
      if (drv->out_format != MSG_FMT_PLAIN) {
         OLOG_WARNING("messaging: driver '%s' has no outbound cap — add a provider_outbound_for "
                      "branch",
                      drv->name);
      }
      cap = 1u << 20; /* 1 MiB: effectively single-message */
   }

   /* INVARIANT: the err / reject strings sent on the failure paths below go
    * straight to send_text WITHOUT passing through the formatter, so they MUST
    * stay plain ASCII (no <, >, & or markdown).  A special char would reach
    * the Telegram parse_mode=HTML body unescaped and get the whole message
    * rejected (HTTP 400).  Keep these messages literal English. */
   char **parts = NULL;
   size_t nparts = 0;
   char err[256] = { 0 };
   if (messaging_format_render_split(canonical_markdown, drv->out_format, cap, &parts, &nparts, err,
                                     sizeof(err)) != SUCCESS) {
      OLOG_WARNING("messaging: format/split failed for %s:%s — %s", drv->name,
                   provider_address ? provider_address : "?", err);
      drv->send_text(user_id, provider_address, address_json, err);
      return MESSAGING_FAILURE;
   }

   /* Parts-cap rejection (SMS max_parts=3): deliver the full reply across N
    * messages or an explicit WebUI pointer — never silent partial loss. */
   if (cfg.max_parts > 0 && nparts > (size_t)cfg.max_parts) {
      OLOG_WARNING("messaging: reply exceeds max_parts for %s:%s (%zu > %d), rejecting", drv->name,
                   provider_address ? provider_address : "?", nparts, cfg.max_parts);
      char reject[256];
      snprintf(reject, sizeof(reject),
               "This reply is too long for %s (would take %zu messages, limit %d).  "
               "Open the WebUI to see the full response.",
               drv->name, nparts, cfg.max_parts);
      messaging_format_free_parts(parts, nparts);
      drv->send_text(user_id, provider_address, address_json, reject);
      return MESSAGING_FAILURE;
   }

   /* Deliver each part with a "(N/M) " prefix when split, 100ms pacing.  The
    * prefix is plain ASCII (digits/slash/parens/space) — safe to prepend even
    * to Telegram HTML without escaping or unbalancing tags. */
   for (size_t i = 0; i < nparts; i++) {
      char prefix[16] = { 0 };
      if (nparts > 1) {
         snprintf(prefix, sizeof(prefix), "(%zu/%zu) ", i + 1, nparts);
      }
      size_t prefix_len = strlen(prefix);
      size_t part_len = strlen(parts[i]);
      char *send_buf = NULL;
      const char *to_send = parts[i];
      if (prefix_len > 0 && prefix_len + part_len <= cap + MESSAGING_PREFIX_HEADROOM) {
         send_buf = (char *)malloc(prefix_len + part_len + 1);
         if (send_buf) {
            memcpy(send_buf, prefix, prefix_len);
            memcpy(send_buf + prefix_len, parts[i], part_len);
            send_buf[prefix_len + part_len] = '\0';
            to_send = send_buf;
         }
      }
      drv->send_text(user_id, provider_address, address_json, to_send);
      free(send_buf);
      if (i + 1 < nparts) {
         usleep(MESSAGING_SPLIT_INTER_PART_USEC);
      }
   }

   messaging_format_free_parts(parts, nparts);
   return MESSAGING_SUCCESS;
}

/* =============================================================================
 * Channel lookup
 * ============================================================================= */

/* =============================================================================
 * Outbound send
 * ============================================================================= */

/* =============================================================================
 * List channels JSON
 * ============================================================================= */

/* =============================================================================
 * Provider-name registry (shared with operator-facing surfaces)
 * ============================================================================= */

bool messaging_engine_provider_known(const char *name) {
   if (!name || name[0] == '\0') {
      return false;
   }
   /* Single source of truth for the recognized driver names.  Update
    * here AND in dawn-admin/main.c help text when adding a provider. */
   static const char *const k_known_providers[] = {
      "telegram",
      "discord",
      "slack",
      "sms",
   };
   const size_t n = sizeof(k_known_providers) / sizeof(k_known_providers[0]);
   for (size_t i = 0; i < n; i++) {
      if (strcmp(name, k_known_providers[i]) == 0) {
         return true;
      }
   }
   return false;
}
