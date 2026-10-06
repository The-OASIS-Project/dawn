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
#include "tools/schwab_quotes.h"    /* schwab_quote_t + watch payload */
#include "tools/schwab_service.h"   /* schwab_service_portfolio_snapshot / _quotes */
#include "tools/schwab_watchlist.h" /* per-user watchlist CRUD */
#include "utils/string_utils.h"

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

/* Watchlist cache — the last quotes fanned for a user's watched symbols, so a
 * watch_get/subscribe serves immediately and off-hours holds the last values.
 * Guarded by the same s_stocks_mutex leaf lock as the portfolio cache. */
typedef struct {
   int user_id; /* 0 = free slot */
   schwab_quote_t quotes[SCHWAB_WATCHLIST_MAX];
   int n;
   bool have;
   bool dirty; /* the symbol SET changed (edit); forces a re-fetch regardless of
                * market/cadence, and survives an in-flight fetch's apply so a mid-
                * fetch edit isn't clobbered. */
   char status[16];
   int64_t as_of;
   int64_t last_ok;
   int64_t last_attempt;
   int64_t backoff_until;
   int backoff_sec;
   /* Adds awaiting validation (raw client symbols). The refresher validates them
    * against Schwab's invalidSymbols, stores only the valid ones, and sends the
    * deferred stocks_watch_set_response — so an invalid ticker never enters the DB. */
   char pending_add[SCHWAB_WATCHLIST_MAX][SCHWAB_SYMBOL_MAX];
   int n_pending;
   bool has_pending;
} stocks_watch_cache_t;
static stocks_watch_cache_t s_watch_cache[STOCKS_MAX_USERS];

/* ---------------------------------------------------------------- market hours */

/* "regular" | "pre" | "post" | "closed" — the US-equity session now (DST-aware ET
 * clock + NYSE holidays/half-days live in the reusable, unit-tested market_hours
 * module). Wraps it so the call sites below stay unchanged. */
static const char *stocks_market_state(void) {
   return market_session_str(market_hours_us_equity(time(NULL)));
}

/* True in a session where the extended-hours overlay applies (pre/post). The overlay
 * is fetched + emitted only then, so a subscriber's ext-hours opt-in is inert during
 * regular hours / closed and the panel renders exactly as it does today. */
static bool market_is_ext(const char *market) {
   return strcmp(market, "pre") == 0 || strcmp(market, "post") == 0;
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
   /* Enrich with the pre/post price only during an active extended window; the raw
    * opt-in (ext) still rides the wire as the ext_hours echo via apply_fetch. */
   bool ext_active = ext && market_is_ext(market);
   schwab_portfolio_t fresh;
   schwab_rc_t rc = schwab_service_portfolio_snapshot(user_id, ext_active, &fresh);
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

/* ---------------------------------------------------------------- watchlist */

static stocks_watch_cache_t *watch_cache_find(int user_id) {
   for (int i = 0; i < STOCKS_MAX_USERS; i++) {
      if (s_watch_cache[i].user_id == user_id) {
         return &s_watch_cache[i];
      }
   }
   return NULL;
}

static stocks_watch_cache_t *watch_cache_get(int user_id) {
   stocks_watch_cache_t *e = watch_cache_find(user_id);
   if (e) {
      return e;
   }
   for (int i = 0; i < STOCKS_MAX_USERS; i++) {
      if (s_watch_cache[i].user_id == 0) {
         memset(&s_watch_cache[i], 0, sizeof(s_watch_cache[i]));
         s_watch_cache[i].user_id = user_id;
         return &s_watch_cache[i];
      }
   }
   return NULL;
}

/* Mark a user's watchlist cache dirty (its symbol set changed) and wake the
 * refresher to re-fetch. Clears any active backoff so an edit isn't stalled behind a
 * 429/token-retry timer. No-op if the user has no live cache entry (not subscribed —
 * they get the current set on subscribe). */
static void mark_watch_dirty(int user_id) {
   pthread_mutex_lock(&s_stocks_mutex);
   stocks_watch_cache_t *e = watch_cache_find(user_id);
   if (e) {
      e->dirty = true;
      e->backoff_until = 0;
   }
   pthread_mutex_unlock(&s_stocks_mutex);
   pthread_cond_signal(&s_stocks_cond);
}

/* Strong override of the Layer-3 weak hook: a watchlist edit made outside the WebUI
 * (the voice tool) invalidates the live panel cache so it re-fetches the new set. */
void webui_stocks_watchlist_invalidate(int user_id) {
   mark_watch_dirty(user_id);
}

/* Build the stocks_watch_update payload for a cache entry (s_stocks_mutex held). A
 * token_expired is state-only; otherwise the last-known quotes (possibly empty). */
static struct json_object *watch_payload_locked(const stocks_watch_cache_t *e, const char *market) {
   const char *status = (e && e->status[0]) ? e->status : "not_connected";
   bool have = e && e->have && strcmp(status, "token_expired") != 0;
   return schwab_quotes_watch_payload_jobj(have ? e->quotes : NULL, have ? e->n : 0, status, market,
                                           e ? e->as_of : 0);
}

static struct json_object *wrap_watch_frame(struct json_object *payload) {
   struct json_object *frame = json_object_new_object();
   json_object_object_add(frame, "type", json_object_new_string("stocks_watch_update"));
   json_object_object_add(frame, "payload", payload);
   return frame;
}

/* Append a {symbol, reason} entry to a rejected array. */
static void reject_add(struct json_object *rejected, const char *symbol, const char *reason) {
   struct json_object *o = json_object_new_object();
   json_object_object_add(o, "symbol", json_object_new_string(symbol));
   json_object_object_add(o, "reason", json_object_new_string(reason));
   json_object_array_add(rejected, o);
}

/* Build a stocks_watch_set_response frame (takes ownership of @p added/@p rejected;
 * caller json_object_put()s the returned frame). */
static struct json_object *build_set_response(struct json_object *added,
                                              struct json_object *rejected) {
   struct json_object *rp = json_object_new_object();
   json_object_object_add(rp, "success", json_object_new_boolean(true));
   json_object_object_add(rp, "added", added);
   json_object_object_add(rp, "rejected", rejected);
   struct json_object *frame = json_object_new_object();
   json_object_object_add(frame, "type", json_object_new_string("stocks_watch_set_response"));
   json_object_object_add(frame, "payload", rp);
   return frame;
}

/* Fan a frame to every live, watch-SUBSCRIBED connection of a user. Registry lock
 * held across the scan+send (same visitor discipline as stocks_fan). */
static void stocks_watch_fan(int user_id, struct json_object *frame) {
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (conn && conn->authenticated && !conn->is_satellite && conn->session &&
          conn->stocks_watch_subscribed && conn->auth_user_id == user_id) {
         send_json_response(conn, frame);
      }
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);
}

/* Distinct watch-subscribed user ids (and whether any of a user's watch subscribers
 * wants extended hours) under the registry lock — mirrors collect_subscribers. */
static int collect_watch_subscribers(stocks_sub_t *out, int max) {
   int n = 0;
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (!conn || !conn->authenticated || conn->is_satellite || !conn->session ||
          !conn->stocks_watch_subscribed || conn->auth_user_id <= 0) {
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
      out[k].ext = out[k].ext || conn->stocks_watch_ext_hours;
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);
   return n;
}

/* Apply a watchlist fetch result into the cache entry (s_stocks_mutex held). Self-
 * heals every failure via backoff_until, mirroring the portfolio path. */
static void apply_watch_fetch(stocks_watch_cache_t *e,
                              schwab_rc_t rc,
                              const schwab_quote_t *q,
                              int nq) {
   int64_t now = (int64_t)time(NULL);
   switch (rc) {
      case SCHWAB_RC_OK:
         e->n = nq > SCHWAB_WATCHLIST_MAX ? SCHWAB_WATCHLIST_MAX : nq;
         for (int i = 0; i < e->n; i++) {
            e->quotes[i] = q[i];
         }
         e->have = true;
         e->as_of = now;
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
         e->backoff_until = now + STOCKS_AUTH_RETRY_SEC;
         snprintf(e->status, sizeof(e->status), "token_expired");
         break;
      case SCHWAB_RC_NOT_LINKED:
         e->backoff_until = now + STOCKS_AUTH_RETRY_SEC;
         snprintf(e->status, sizeof(e->status), "not_connected");
         break;
      default:
         e->backoff_until = now + STOCKS_BACKOFF_BASE_SEC;
         snprintf(e->status, sizeof(e->status), "error");
         break;
   }
}

/* Decide + (maybe) fetch + fan the watchlist for one subscribed user. */
static void refresh_watch_user(int user_id, bool ext, const char *market) {
   int64_t now = (int64_t)time(NULL);
   bool market_regular = strcmp(market, "regular") == 0;
   bool ext_active = ext && market_is_ext(market); /* fetch pre/post prices only when live */

   pthread_mutex_lock(&s_stocks_mutex);
   stocks_watch_cache_t *e = watch_cache_get(user_id);
   if (!e) {
      pthread_mutex_unlock(&s_stocks_mutex);
      return;
   }
   bool suppress = now < e->backoff_until;
   /* A pending edit (dirty) bypasses both the cadence floor and the market/staleness
    * gate so a just-added symbol is fetched now, not on the next 30s tick. */
   bool cadence_ok = e->dirty || now - e->last_attempt >= STOCKS_CADENCE_REGULAR_SEC;
   bool want = !e->have || e->dirty ||
               (market_regular && now - e->last_ok >= STOCKS_CADENCE_REGULAR_SEC);
   bool due = !suppress && cadence_ok && want;
   bool was_dirty = false; /* was THIS fetch edit-driven? (re-arm dirty if it fails) */
   if (due) {
      e->last_attempt = now;
      was_dirty = e->dirty;
      e->dirty = false; /* consume; a re-edit during the fetch re-sets it → re-fetch */
   }
   pthread_mutex_unlock(&s_stocks_mutex);
   if (!due) {
      return;
   }

   /* Read the symbols, then fetch quotes — OUTSIDE the stocks lock (the fetch
    * blocks). An empty watchlist is a valid OK state (symbols:[]). */
   char syms[SCHWAB_WATCHLIST_MAX][SCHWAB_SYMBOL_MAX];
   int nsym = 0;
   schwab_watchlist_list(user_id, syms, SCHWAB_WATCHLIST_MAX, &nsym);

   schwab_quote_t quotes[SCHWAB_WATCHLIST_MAX];
   int nq = 0;
   schwab_rc_t rc = SCHWAB_RC_OK;
   if (nsym > 0) {
      const char *ptrs[SCHWAB_WATCHLIST_MAX];
      for (int i = 0; i < nsym; i++) {
         ptrs[i] = syms[i];
      }
      rc = schwab_service_quotes(user_id, ptrs, nsym, ext_active, quotes, SCHWAB_WATCHLIST_MAX,
                                 &nq);
   }

   /* On a SUCCESSFUL fetch, project the FULL stored watchlist: a symbol Schwab
    * returned keeps its quote; a stored symbol it did NOT return becomes a
    * quotable=false row (a bad/unknown ticker the user can then see + prune). An
    * off-hours-valid symbol still comes back quoted (stale price), so only genuine
    * misses get flagged — the healthy-feed guard falls out of gating on rc==OK. */
   schwab_quote_t full[SCHWAB_WATCHLIST_MAX];
   int nfull = 0;
   if (rc == SCHWAB_RC_OK) {
      for (int i = 0; i < nsym && nfull < SCHWAB_WATCHLIST_MAX; i++) {
         int f = -1;
         for (int j = 0; j < nq; j++) {
            if (strcmp(quotes[j].symbol, syms[i]) == 0) {
               f = j;
               break;
            }
         }
         if (f >= 0) {
            full[nfull] = quotes[f]; /* quotable=true (set by the parser) */
         } else {
            memset(&full[nfull], 0, sizeof(full[nfull]));
            safe_strscpy(full[nfull].symbol, syms[i]);
            full[nfull].quotable = false;
         }
         nfull++;
      }
   }

   pthread_mutex_lock(&s_stocks_mutex);
   e = watch_cache_get(user_id);
   if (!e) {
      pthread_mutex_unlock(&s_stocks_mutex);
      return;
   }
   apply_watch_fetch(e, rc, full, nfull);
   /* If an EDIT-driven fetch failed, re-arm dirty so the new set is retried once the
    * backoff clears (rather than waiting for the next market-open staleness tick). */
   if (rc != SCHWAB_RC_OK && was_dirty) {
      e->dirty = true;
   }
   struct json_object *payload = watch_payload_locked(e, market);
   pthread_mutex_unlock(&s_stocks_mutex);

   struct json_object *frame = wrap_watch_frame(payload);
   stocks_watch_fan(user_id, frame);
   json_object_put(frame);
}

static void evict_watch_unsubscribed(const stocks_sub_t *subs, int n) {
   pthread_mutex_lock(&s_stocks_mutex);
   for (int i = 0; i < STOCKS_MAX_USERS; i++) {
      if (s_watch_cache[i].user_id == 0) {
         continue;
      }
      bool still = false;
      for (int k = 0; k < n; k++) {
         if (subs[k].user_id == s_watch_cache[i].user_id) {
            still = true;
            break;
         }
      }
      if (!still && !s_watch_cache[i].has_pending) { /* keep an entry mid-validation */
         memset(&s_watch_cache[i], 0, sizeof(s_watch_cache[i]));
      }
   }
   pthread_mutex_unlock(&s_stocks_mutex);
}

/* Validate + commit queued watch adds for every user with pending adds (runs on the
 * refresher thread — the Schwab validation blocks). A candidate Schwab explicitly
 * flags (errors.invalidSymbols) is rejected and NEVER stored; a well-formed
 * candidate Schwab does NOT flag is stored — so a valid symbol, or any candidate
 * during a Schwab outage (validation unavailable), is never lost. The deferred
 * set-response is fanned to the user's watch connections, then the set is marked
 * dirty so the stocks_watch_update follows. */
static void process_pending_validations(void) {
   for (int u = 0; u < STOCKS_MAX_USERS; u++) {
      char cand[SCHWAB_WATCHLIST_MAX][SCHWAB_SYMBOL_MAX];
      int ncand = 0;
      int uid = 0;
      pthread_mutex_lock(&s_stocks_mutex);
      if (s_watch_cache[u].user_id != 0 && s_watch_cache[u].has_pending) {
         uid = s_watch_cache[u].user_id;
         ncand = s_watch_cache[u].n_pending;
         for (int i = 0; i < ncand; i++) {
            snprintf(cand[i], SCHWAB_SYMBOL_MAX, "%s", s_watch_cache[u].pending_add[i]);
         }
         s_watch_cache[u].n_pending = 0;
         s_watch_cache[u].has_pending = false;
      }
      pthread_mutex_unlock(&s_stocks_mutex);
      if (uid == 0 || ncand == 0) {
         continue;
      }

      struct json_object *added = json_object_new_array();
      struct json_object *rejected = json_object_new_array();

      /* Reject bad-character candidates immediately; collect the well-formed ones. */
      char norm[SCHWAB_WATCHLIST_MAX][SCHWAB_SYMBOL_MAX];
      int nn = 0;
      for (int i = 0; i < ncand; i++) {
         char n[SCHWAB_SYMBOL_MAX];
         if (schwab_watchlist_normalize(cand[i], n, sizeof(n)) != SUCCESS) {
            reject_add(rejected, cand[i], "invalid");
         } else {
            snprintf(norm[nn++], SCHWAB_SYMBOL_MAX, "%s", n);
         }
      }

      /* Validate the well-formed candidates against Schwab's invalidSymbols. */
      char invalid[SCHWAB_WATCHLIST_MAX][SCHWAB_SYMBOL_MAX];
      int ninv = 0;
      schwab_rc_t rc = SCHWAB_RC_OK;
      if (nn > 0) {
         const char *ptrs[SCHWAB_WATCHLIST_MAX];
         for (int i = 0; i < nn; i++) {
            ptrs[i] = norm[i];
         }
         rc = schwab_service_validate_symbols(uid, ptrs, nn, invalid, SCHWAB_WATCHLIST_MAX, &ninv);
      }

      for (int i = 0; i < nn; i++) {
         bool bad = false;
         if (rc == SCHWAB_RC_OK) { /* only trust invalidSymbols from a successful fetch */
            for (int j = 0; j < ninv; j++) {
               if (strcmp(norm[i], invalid[j]) == 0) {
                  bad = true;
                  break;
               }
            }
         }
         if (bad) {
            reject_add(rejected, norm[i], "unknown"); /* Schwab says not a real ticker */
            continue;
         }
         schwab_watch_add_t r = schwab_watchlist_add(uid, norm[i]); /* valid → store */
         if (r == SCHWAB_WATCH_ADDED || r == SCHWAB_WATCH_DUPLICATE) {
            json_object_array_add(added, json_object_new_string(norm[i]));
         } else if (r == SCHWAB_WATCH_FULL) {
            reject_add(rejected, norm[i], "limit");
         } else {
            reject_add(rejected, norm[i], "invalid");
         }
      }

      struct json_object *frame = build_set_response(added, rejected);
      stocks_watch_fan(uid, frame);
      json_object_put(frame);
      mark_watch_dirty(uid); /* the stocks_watch_update with the new set follows */
   }
}

/* ---------------------------------------------------------------- refresh loop */

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

      /* Validate + commit any queued watch adds first (so the set is current before
       * the watch refresh below fetches + fans the update frame). */
      process_pending_validations();

      stocks_sub_t subs[STOCKS_MAX_USERS];
      int n = collect_subscribers(subs, STOCKS_MAX_USERS);
      evict_unsubscribed(subs, n); /* reclaim slots of users who left */

      stocks_sub_t wsubs[STOCKS_MAX_USERS];
      int wn = collect_watch_subscribers(wsubs, STOCKS_MAX_USERS);
      evict_watch_unsubscribed(wsubs, wn);

      if (n == 0 && wn == 0) {
         continue; /* nobody watching either panel → no API traffic */
      }
      const char *market = stocks_market_state();
      for (int i = 0; i < n; i++) {
         refresh_user(subs[i].user_id, subs[i].ext, market);
      }
      for (int i = 0; i < wn; i++) {
         refresh_watch_user(wsubs[i].user_id, wsubs[i].ext, market);
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
      memset(&s_watch_cache[i], 0, sizeof(s_watch_cache[i]));
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

/* Emit the currently-cached watch frame to one connection and wake the refresher. */
static void send_current_watch_and_wake(ws_connection_t *conn) {
   const char *market = stocks_market_state();
   pthread_mutex_lock(&s_stocks_mutex);
   stocks_watch_cache_t *e = watch_cache_find(conn->auth_user_id);
   struct json_object *payload = e ? watch_payload_locked(e, market) : NULL;
   pthread_mutex_unlock(&s_stocks_mutex);
   if (payload) {
      struct json_object *frame = wrap_watch_frame(payload);
      send_json_response(conn, frame);
      json_object_put(frame);
   }
   pthread_cond_signal(&s_stocks_cond);
}

void handle_stocks_watch_subscribe(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }
   bool ext = false;
   json_object *v = NULL;
   if (payload && json_object_object_get_ex(payload, "ext_hours", &v)) {
      ext = json_object_get_boolean(v);
   }
   conn->stocks_watch_ext_hours = ext;
   conn->stocks_watch_subscribed = true;
   send_current_watch_and_wake(conn);
}

void handle_stocks_watch_unsubscribe(ws_connection_t *conn, json_object *payload) {
   (void)payload;
   if (!conn_require_auth(conn)) {
      return;
   }
   conn->stocks_watch_subscribed = false;
}

void handle_stocks_watch_get(ws_connection_t *conn, json_object *payload) {
   (void)payload;
   if (!conn_require_auth(conn)) {
      return;
   }
   send_current_watch_and_wake(conn);
}

/* Apply the add[]/remove[] deltas to the user's watchlist, force a re-fetch, and
 * wake the refresher (which fans the updated set within a cycle). */
/* Apply the remove[] deltas. Idempotent (remove-absent is a no-op), so nothing is
 * reported. Capped per message to bound the auth_db work on the lws thread. */
static void watch_remove_apply(int user_id, json_object *arr) {
   if (!arr || !json_object_is_type(arr, json_type_array)) {
      return;
   }
   size_t len = json_object_array_length(arr);
   if (len > SCHWAB_WATCHLIST_MAX) {
      len = SCHWAB_WATCHLIST_MAX;
   }
   for (size_t i = 0; i < len; i++) {
      const char *s = json_object_get_string(json_object_array_get_idx(arr, i));
      if (s) {
         schwab_watchlist_remove(user_id, s);
      }
   }
}

void handle_stocks_watch_set(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }
   int uid = conn->auth_user_id;
   /* Editing the watchlist subscribes this connection to its feed, so the DEFERRED
    * set-response (and the update) reach the requester even if it hadn't already
    * subscribed — the response is fanned to the user's watch connections. */
   conn->stocks_watch_subscribed = true;

   /* Removes are idempotent and need no validation — apply immediately (bounded
    * local DB writes, no network/blocking work on this thread). */
   json_object *arr = NULL;
   if (payload && json_object_object_get_ex(payload, "remove", &arr)) {
      watch_remove_apply(uid, arr);
   }

   /* Adds are QUEUED for validation on the refresher thread (a blocking Schwab check
    * against errors.invalidSymbols). It stores only the valid ones and sends the
    * deferred stocks_watch_set_response, so an invalid ticker never enters the DB.
    * No network/blocking work here. */
   int nqueued = 0;
   bool got_slot = true;
   size_t addlen = 0;
   if (payload && json_object_object_get_ex(payload, "add", &arr) &&
       json_object_is_type(arr, json_type_array)) {
      addlen = json_object_array_length(arr);
      if (addlen > SCHWAB_WATCHLIST_MAX) {
         addlen = SCHWAB_WATCHLIST_MAX; /* bound the queued work */
      }
      pthread_mutex_lock(&s_stocks_mutex);
      stocks_watch_cache_t *e = watch_cache_get(uid);
      if (e) {
         for (size_t i = 0; i < addlen; i++) {
            const char *s = json_object_get_string(json_object_array_get_idx(arr, i));
            if (!s || !s[0]) {
               continue;
            }
            bool seen = false;
            for (int k = 0; k < e->n_pending; k++) {
               if (strcmp(e->pending_add[k], s) == 0) {
                  seen = true;
                  break;
               }
            }
            if (!seen && e->n_pending < SCHWAB_WATCHLIST_MAX) {
               snprintf(e->pending_add[e->n_pending++], SCHWAB_SYMBOL_MAX, "%s", s);
               nqueued++;
            }
         }
         if (e->n_pending > 0) {
            e->has_pending = true;
         }
      } else {
         got_slot = false; /* per-user cache full (only with >STOCKS_MAX_USERS active) */
      }
      pthread_mutex_unlock(&s_stocks_mutex);
   }

   if (nqueued > 0) {
      pthread_cond_signal(&s_stocks_cond); /* refresher validates + replies */
      return;
   }

   /* Nothing was queued. Reply now: if the cache was full, report the adds rejected
    * ("busy") rather than a false success; otherwise an empty response. */
   struct json_object *rejected = json_object_new_array();
   if (!got_slot) {
      for (size_t i = 0; i < addlen; i++) {
         const char *s = json_object_get_string(json_object_array_get_idx(arr, i));
         if (s && s[0]) {
            reject_add(rejected, s, "busy");
         }
      }
   }
   struct json_object *frame = build_set_response(json_object_new_array(), rejected);
   send_json_response(conn, frame);
   json_object_put(frame);
   mark_watch_dirty(uid);
}
