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
 * WebUI stocks panel — see webui_stocks.h. A dedicated refresher thread owns the
 * (blocking) Schwab fetch; the lws handlers only flip subscription flags, emit the
 * cached snapshot, and wake the refresher. Fan-out re-resolves live connections
 * AFTER the fetch (never carries a conn* across the ~20s HTTP call).
 */

#include "webui/webui_stocks.h"

#include <json-c/json.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "core/market_hours.h" /* US-equity session (holiday/half-day aware) */
#include "dawn_error.h"
#include "logging.h"
#include "tools/schwab_client.h"    /* schwab_rc_t */
#include "tools/schwab_portfolio.h" /* schwab_portfolio_t + payload_jobj */
#include "tools/schwab_service.h"   /* schwab_service_portfolio_snapshot */

#define STOCKS_MAX_USERS 4            /* owner-only today; keyed per-user for generality */
#define STOCKS_CADENCE_REGULAR_SEC 30 /* market-hours push cadence */
#define STOCKS_BACKOFF_BASE_SEC 60    /* 429 backoff starts here */
#define STOCKS_BACKOFF_MAX_SEC 600    /* …and caps here */
/* Slow auto-retry for the "human must act" states (token expired / not linked):
 * long enough not to hammer a dead credential, short enough to self-heal within a
 * few minutes of a `dawn-admin schwab auth` relink without a daemon restart. */
#define STOCKS_AUTH_RETRY_SEC 300

typedef struct {
   int user_id; /* 0 = free slot */
   schwab_portfolio_t snap;
   bool have_snap; /* a successful fetch has populated snap at least once */
   char status[16];
   bool ext_hours_last;
   int64_t last_ok;         /* time() of the last OK fetch */
   int64_t last_attempt;    /* time() of the last fetch attempt (success OR failure) */
   int64_t backoff_until;   /* don't fetch before this (429 / auth / error retry) */
   int backoff_sec;         /* current 429 backoff step */
   int64_t link_expires_at; /* Schwab refresh-token expiry (0 = unknown); set off-lock
                               by the refresher so the lws thread never decrypts */
} stocks_cache_t;

static stocks_cache_t s_cache[STOCKS_MAX_USERS];
static pthread_mutex_t s_stocks_mutex = PTHREAD_MUTEX_INITIALIZER; /* leaf lock */
static pthread_cond_t s_stocks_cond = PTHREAD_COND_INITIALIZER;
static pthread_t s_refresher;
static bool s_refresher_running = false;
static bool s_stocks_shutdown = false;

/* ---------------------------------------------------------------- market hours */

/* "regular" | "pre" | "post" | "closed" — the US-equity session now (DST-aware ET
 * clock + NYSE holidays/half-days live in the reusable, unit-tested market_hours
 * module). Wraps it so the call sites below stay unchanged. */
static const char *stocks_market_state(void) {
   return market_session_str(market_hours_us_equity(time(NULL)));
}

/* ---------------------------------------------------------------- cache (locked) */

/* Find an existing cache entry for a user (s_stocks_mutex held). NULL if none. */
static stocks_cache_t *cache_find(int user_id) {
   for (int i = 0; i < STOCKS_MAX_USERS; i++) {
      if (s_cache[i].user_id == user_id) {
         return &s_cache[i];
      }
   }
   return NULL;
}

/* Find or allocate a cache entry for a user (s_stocks_mutex held). NULL if the
 * table is full. */
static stocks_cache_t *cache_get(int user_id) {
   stocks_cache_t *e = cache_find(user_id);
   if (e) {
      return e;
   }
   for (int i = 0; i < STOCKS_MAX_USERS; i++) {
      if (s_cache[i].user_id == 0) {
         memset(&s_cache[i], 0, sizeof(s_cache[i]));
         s_cache[i].user_id = user_id;
         return &s_cache[i];
      }
   }
   return NULL;
}

/* Build the wire payload for the cache entry's current state (s_stocks_mutex
 * held). Decides last-known-vs-state-only per the Aurora contract: token_expired
 * is always state-only; a transient error/not_connected shows the last-known
 * snapshot dimmed if we have one. Returns an owned json_object (payload only). */
static struct json_object *frame_payload_locked(const stocks_cache_t *e, const char *market) {
   const char *status = (e && e->status[0]) ? e->status : "not_connected";
   bool state_only;
   if (strcmp(status, "token_expired") == 0) {
      state_only = true;
   } else if (strcmp(status, "ok") == 0) {
      state_only = false;
   } else {
      /* not_connected / rate_limited / error: show last-known if we have it. */
      state_only = !(e && e->have_snap);
   }
   const schwab_portfolio_t *snap = state_only ? NULL : &e->snap;
   return schwab_portfolio_payload_jobj(snap, status, market, e ? e->ext_hours_last : false,
                                        e ? e->link_expires_at : 0);
}

/* Wrap a payload object in the {type, payload} frame envelope (takes ownership of
 * payload). Returns an owned json_object. */
static struct json_object *wrap_frame(struct json_object *payload) {
   struct json_object *frame = json_object_new_object();
   json_object_object_add(frame, "type", json_object_new_string("stocks_portfolio_update"));
   json_object_object_add(frame, "payload", payload);
   return frame;
}

/* ---------------------------------------------------------------- fan-out */

/* Send one frame to every live, stocks-SUBSCRIBED connection of a user.
 *
 * Runs on the REFRESHER (non-lws) thread, so it must NOT deref a conn* after
 * releasing the registry lock: lws frees the per-session conn on the lws thread
 * after CLOSED, and CLOSED's unregister_connection takes s_conn_registry_mutex.
 * We therefore hold that lock across the whole loop — which blocks a concurrent
 * teardown, so each conn stays valid while we touch it. send_json_response only
 * enqueues (queue_response); calling it under the registry lock is the documented
 * visitor pattern (webui_internal.h §"Visitor invoked under s_conn_registry_mutex")
 * — it must not re-acquire the registry lock or block, and it does neither.
 *
 * (webui_collect_conns_by_user is unusable here twice over: it filters on
 * music_state — dropping non-music stock viewers — AND it hands back pointers
 * used off-lock, the very UAF this avoids.) */
static void stocks_fan(int user_id, struct json_object *frame) {
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (conn && conn->authenticated && !conn->is_satellite && conn->session &&
          conn->stocks_subscribed && conn->auth_user_id == user_id) {
         send_json_response(conn, frame);
      }
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);
}

/* ---------------------------------------------------------------- refresh */

typedef struct {
   int user_id;
   bool ext;
} stocks_sub_t;

/* Collect the distinct subscribed user ids (and whether any of a user's
 * subscribers wants extended hours) under the registry lock. */
static int collect_subscribers(stocks_sub_t *out, int max) {
   int n = 0;
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (!conn || !conn->authenticated || conn->is_satellite || !conn->session ||
          !conn->stocks_subscribed || conn->auth_user_id <= 0) {
         continue;
      }
      int k = 0;
      for (; k < n; k++) {
         if (out[k].user_id == conn->auth_user_id) {
            break;
         }
      }
      if (k == n) {
         if (n >= max) {
            continue;
         }
         out[n].user_id = conn->auth_user_id;
         out[n].ext = false;
         n++;
      }
      out[k].ext = out[k].ext || conn->stocks_ext_hours;
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);
   return n;
}

/* Apply a fetch result into the cache entry (s_stocks_mutex held). On OK the fresh
 * snapshot is moved in (ownership transferred); on failure the last-known snapshot
 * is kept and only the status/backoff change. Every failure arms backoff_until so
 * the state self-heals on a timer (no sticky latch that a routine weekly token
 * expiry could wedge until daemon restart). */
static void apply_fetch(stocks_cache_t *e, schwab_rc_t rc, schwab_portfolio_t *fresh, bool ext) {
   int64_t now = (int64_t)time(NULL);
   e->ext_hours_last = ext;
   switch (rc) {
      case SCHWAB_RC_OK:
         schwab_portfolio_free(&e->snap);
         e->snap = *fresh; /* move: e now owns fresh's arrays */
         memset(fresh, 0, sizeof(*fresh));
         e->have_snap = true;
         e->last_ok = now;
         e->backoff_until = 0;
         e->backoff_sec = 0;
         snprintf(e->status, sizeof(e->status), "ok");
         break;
      case SCHWAB_RC_RATE_LIMITED:
         e->backoff_sec = e->backoff_sec ? e->backoff_sec * 2 : STOCKS_BACKOFF_BASE_SEC;
         if (e->backoff_sec > STOCKS_BACKOFF_MAX_SEC) {
            e->backoff_sec = STOCKS_BACKOFF_MAX_SEC;
         }
         e->backoff_until = now + e->backoff_sec;
         snprintf(e->status, sizeof(e->status), "rate_limited");
         break;
      case SCHWAB_RC_AUTH:
         /* Token expired (routine ~weekly). Slow auto-retry so a post-relink fetch
          * is attempted within minutes without a restart. */
         e->backoff_until = now + STOCKS_AUTH_RETRY_SEC;
         snprintf(e->status, sizeof(e->status), "token_expired");
         break;
      case SCHWAB_RC_NOT_LINKED:
         /* Human must link. Slow-retry so we don't re-probe every cadence tick. */
         e->backoff_until = now + STOCKS_AUTH_RETRY_SEC;
         snprintf(e->status, sizeof(e->status), "not_connected");
         break;
      default:
         /* Transient network/service/parse error — short backoff, keep last-known. */
         e->backoff_until = now + STOCKS_BACKOFF_BASE_SEC;
         snprintf(e->status, sizeof(e->status), "error");
         break;
   }
}

/* Decide + (maybe) fetch + fan for one subscribed user. */
static void refresh_user(int user_id, bool ext, const char *market) {
   int64_t now = (int64_t)time(NULL);
   bool market_regular = strcmp(market, "regular") == 0;

   pthread_mutex_lock(&s_stocks_mutex);
   stocks_cache_t *e = cache_get(user_id);
   if (!e) {
      pthread_mutex_unlock(&s_stocks_mutex);
      return; /* table full — pathological (owner-only) */
   }
   bool suppress = now < e->backoff_until;
   /* A hard floor between attempts (success OR failure) so a subscribe/get storm
    * against an erroring account can't outrun the cadence. cache_empty forces one
    * seed fetch even off-hours so the panel is never blank; otherwise only refresh
    * on the regular-hours cadence. */
   bool cadence_ok = now - e->last_attempt >= STOCKS_CADENCE_REGULAR_SEC;
   bool want = !e->have_snap || (market_regular && now - e->last_ok >= STOCKS_CADENCE_REGULAR_SEC);
   bool due = !suppress && cadence_ok && want;
   if (due) {
      e->last_attempt = now; /* mark before releasing so the next wake is gated */
   }
   pthread_mutex_unlock(&s_stocks_mutex);
   if (!due) {
      return;
   }

   /* Blocking Schwab fetch + token-expiry read — OUTSIDE every lock (the expiry read
    * decrypts, so it must not run on the lws thread; the refresher owns it). */
   schwab_portfolio_t fresh;
   schwab_rc_t rc = schwab_service_portfolio_snapshot(user_id, ext, &fresh);
   int64_t link_exp = schwab_service_link_expires_at(user_id);

   pthread_mutex_lock(&s_stocks_mutex);
   e = cache_get(user_id);
   if (!e) {
      pthread_mutex_unlock(&s_stocks_mutex);
      schwab_portfolio_free(&fresh);
      return;
   }
   apply_fetch(e, rc, &fresh, ext);
   e->link_expires_at = link_exp;
   struct json_object *payload = frame_payload_locked(e, market);
   pthread_mutex_unlock(&s_stocks_mutex);

   schwab_portfolio_free(&fresh); /* no-op after a move; frees on failure paths */
   struct json_object *frame = wrap_frame(payload);
   stocks_fan(user_id, frame);
   json_object_put(frame);
}

/* Free cache slots whose user no longer has any live subscriber, so a bounded
 * table isn't permanently exhausted by users who subscribed once and left. */
static void evict_unsubscribed(const stocks_sub_t *subs, int n) {
   pthread_mutex_lock(&s_stocks_mutex);
   for (int i = 0; i < STOCKS_MAX_USERS; i++) {
      if (s_cache[i].user_id == 0) {
         continue;
      }
      bool still = false;
      for (int k = 0; k < n; k++) {
         if (subs[k].user_id == s_cache[i].user_id) {
            still = true;
            break;
         }
      }
      if (!still) {
         schwab_portfolio_free(&s_cache[i].snap);
         memset(&s_cache[i], 0, sizeof(s_cache[i]));
      }
   }
   pthread_mutex_unlock(&s_stocks_mutex);
}

static void *stocks_refresher(void *arg) {
   (void)arg;
   while (1) {
      pthread_mutex_lock(&s_stocks_mutex);
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += STOCKS_CADENCE_REGULAR_SEC;
      pthread_cond_timedwait(&s_stocks_cond, &s_stocks_mutex, &ts);
      bool stopping = s_stocks_shutdown;
      pthread_mutex_unlock(&s_stocks_mutex);
      if (stopping) {
         break;
      }

      stocks_sub_t subs[STOCKS_MAX_USERS];
      int n = collect_subscribers(subs, STOCKS_MAX_USERS);
      evict_unsubscribed(subs, n); /* reclaim slots of users who left */
      if (n == 0) {
         continue; /* nobody watching → no API traffic */
      }
      const char *market = stocks_market_state();
      for (int i = 0; i < n; i++) {
         refresh_user(subs[i].user_id, subs[i].ext, market);
      }
   }
   return NULL;
}

/* ---------------------------------------------------------------- lifecycle */

void webui_stocks_start(void) {
   if (s_refresher_running) {
      return;
   }
   s_stocks_shutdown = false;
   if (pthread_create(&s_refresher, NULL, stocks_refresher, NULL) != 0) {
      OLOG_ERROR("stocks: failed to start refresher thread");
      return;
   }
   s_refresher_running = true;
   OLOG_INFO("stocks: refresher thread started");
}

void webui_stocks_stop(void) {
   if (!s_refresher_running) {
      return;
   }
   pthread_mutex_lock(&s_stocks_mutex);
   s_stocks_shutdown = true;
   pthread_cond_signal(&s_stocks_cond);
   pthread_mutex_unlock(&s_stocks_mutex);
   pthread_join(s_refresher, NULL);
   s_refresher_running = false;
   for (int i = 0; i < STOCKS_MAX_USERS; i++) {
      schwab_portfolio_free(&s_cache[i].snap);
      s_cache[i].user_id = 0;
   }
   OLOG_INFO("stocks: refresher thread stopped");
}

/* ---------------------------------------------------------------- WS handlers */

/* Emit the currently-cached frame to one connection (fast, no I/O), and wake the
 * refresher so a first/stale snapshot is fetched promptly. */
static void send_current_and_wake(ws_connection_t *conn, bool ext) {
   const char *market = stocks_market_state();
   pthread_mutex_lock(&s_stocks_mutex);
   stocks_cache_t *e = cache_find(conn->auth_user_id);
   struct json_object *payload = e ? frame_payload_locked(e, market) : NULL;
   pthread_mutex_unlock(&s_stocks_mutex);

   if (payload) {
      struct json_object *frame = wrap_frame(payload);
      send_json_response(conn, frame);
      json_object_put(frame);
   }
   (void)ext;
   pthread_cond_signal(&s_stocks_cond); /* wake the refresher to (re)fetch */
}

void handle_stocks_portfolio_subscribe(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }
   bool ext = false;
   json_object *v = NULL;
   if (payload && json_object_object_get_ex(payload, "ext_hours", &v)) {
      ext = json_object_get_boolean(v);
   }
   conn->stocks_ext_hours = ext;
   conn->stocks_subscribed = true;
   send_current_and_wake(conn, ext);
}

void handle_stocks_portfolio_unsubscribe(ws_connection_t *conn, json_object *payload) {
   (void)payload;
   if (!conn_require_auth(conn)) {
      return;
   }
   conn->stocks_subscribed = false;
}

void handle_stocks_portfolio_get(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }
   bool ext = false;
   json_object *v = NULL;
   if (payload && json_object_object_get_ex(payload, "ext_hours", &v)) {
      ext = json_object_get_boolean(v);
   }
   /* One-shot: returns the cached frame (may be absent on a cold first call — the
    * panel normally uses subscribe, which the refresher then serves). */
   send_current_and_wake(conn, ext);
}
