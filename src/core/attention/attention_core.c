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
 * SAGE core — module lifecycle, the in-memory watch cache (+ per-watch gate
 * state), the leaf-mutex event queue, the heartbeat tick (poll -> gate ->
 * policy -> deliver -> log), self-monitoring counters, and the watch CRUD
 * wrappers used by the `attention` tool and WebUI (each mutation reloads the
 * cache).  s_mutex is a leaf lock: the tick evaluates + enqueues under it, then
 * releases before delivery/DB writes (which take other locks).
 */

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_attention.h"
#include "config/dawn_config.h"
#include "core/attention/attention_internal.h"
#include "core/path_utils.h"
#include "core/scheduler.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "logging.h"

/* --- module state --- */
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER; /* leaf: cache + queue + metrics */
static sage_watch_t s_watches[SAGE_MAX_WATCHES_TOTAL];
static sage_rule_state_t s_states[SAGE_MAX_WATCHES_TOTAL];
static int s_watch_count;
static bool s_initialized;
static attention_metrics_t s_metrics;
/* The master switch is read LIVE from g_config.attention.enabled (not cached) so
 * the WebUI Settings toggle takes effect on the next tick without a restart.  A
 * lock-free bool read is a benign race here (worst case: one stale tick). */

/* Event queue (ring).  P0 poll enqueues from the tick; P1 push sources will
 * enqueue from other threads — same leaf mutex, no restructuring. */
static sage_event_t s_queue[SAGE_QUEUE_CAP];
static int s_q_head, s_q_count;

/* --- queue (caller holds s_mutex) --- */
static void queue_push_locked(const sage_event_t *ev) {
   if (s_q_count >= SAGE_QUEUE_CAP) {
      OLOG_WARNING("attention: event queue full (%d), dropping '%s'", SAGE_QUEUE_CAP,
                   ev->event_key);
      return;
   }
   int tail = (s_q_head + s_q_count) % SAGE_QUEUE_CAP;
   s_queue[tail] = *ev;
   s_q_count++;
}

/* --- catalog value resolution ---
 * Resolve the catalog entry AND read the current value in one lookup (the tick
 * needs both; a single linear scan of the small catalog, not two). */
static const attention_catalog_entry_t *resolve_and_read(const attention_sample_ctx_t *ctx,
                                                         const char *metric,
                                                         double *value,
                                                         bool *present) {
   const attention_catalog_entry_t *cat = attention_catalog_lookup(metric);
   *present = (cat && cat->read) ? cat->read(ctx, value) : false;
   return cat;
}

/* Weak default: SAGE's WebUI banner is a no-op when built without the WebUI.
 * The strong override lives in src/webui/webui_broadcasts.c. */
#ifndef ENABLE_WEBUI
void webui_broadcast_attention_alert(int user_id, const char *summary, const char *level) {
   (void)user_id;
   (void)summary;
   (void)level;
}
#endif

/* =============================================================================
 * Delivery
 * -----------------------------------------------------------------------------
 * SAGE has its OWN banner channel (webui_broadcast_attention_alert) so alerts
 * show an "ATTENTION" badge and never inherit the scheduler's browser chime.
 * Spoken delivery (route_tts_announcement -> text_to_speech) BLOCKS for the full
 * utterance, so it must NOT run on the main heartbeat thread (it would stall the
 * voice state machine) — spoken alerts go to a detached thread, matching how the
 * scheduler delivers alarm TTS off its own thread.  (Choosing WHEN to speak vs
 * the user — the P1 BREAKPOINT tier — is a later refinement; P0 relies on the
 * per-hour budget + backoff to keep alerts sparse.)
 * ============================================================================= */
typedef struct {
   int user_id;
   char text[SAGE_SUMMARY_LEN];
} alert_delivery_t;

static void *alert_delivery_thread(void *arg) {
   alert_delivery_t *p = (alert_delivery_t *)arg;
   scheduler_emit_alert(p->user_id, p->text, SCHED_EVENT_REMINDER, "", true);
   free(p);
   return NULL;
}

/* Speak an alert without blocking the caller (the heartbeat thread). */
static void deliver_spoken_async(int user_id, const char *text) {
   alert_delivery_t *p = malloc(sizeof(*p));
   if (!p) {
      OLOG_WARNING("attention: OOM dispatching spoken alert");
      return;
   }
   p->user_id = user_id;
   safe_strncpy(p->text, text, sizeof(p->text));
   pthread_t tid;
   if (pthread_create(&tid, NULL, alert_delivery_thread, p) != 0) {
      OLOG_ERROR("attention: failed to spawn alert delivery thread");
      free(p);
      return;
   }
   pthread_detach(tid);
}

static void deliver_event(const sage_event_t *ev, sage_delivery_mode_t mode, int64_t now_ms) {
   int64_t delivered_at = 0;

   switch (mode) {
      case SAGE_MODE_ALERT:
         /* Voice (off the heartbeat thread, chime-free) + ATTENTION banner. */
         deliver_spoken_async(ev->user_id, ev->summary);
         webui_broadcast_attention_alert(ev->user_id, ev->summary, "alert");
         attention_policy_note_alert(now_ms);
         delivered_at = now_ms;
         break;
      case SAGE_MODE_AMBIENT:
         /* Silent ATTENTION banner only (no voice, no chime). */
         webui_broadcast_attention_alert(ev->user_id, ev->summary, "ambient");
         delivered_at = now_ms;
         break;
      case SAGE_MODE_DIGEST:
         OLOG_INFO("attention[digest] %s: %s", ev->gate_rule, ev->summary);
         delivered_at = 0; /* not surfaced in P0 (P1 folds into briefings) */
         break;
      case SAGE_MODE_DROP:
      default:
         delivered_at = 0;
         break;
   }

   /* Optionally make an active conversation aware of the alert so DAWN can
    * reference it in chat (config-gated, default off).  Scoped to the owner:
    * session_broadcast_system_message fans out to ALL interactive sessions with
    * no per-user filter, so injecting a non-owner's alert would leak it across
    * users.  P0 is single-owner; a per-user broadcast folds in with multi-user
    * routing. */
   if (delivered_at > 0 && g_config.attention.inject_into_sessions &&
       ev->user_id == ATTENTION_OWNER_USER_ID) {
      char note[SAGE_SUMMARY_LEN + 32];
      snprintf(note, sizeof(note), "[proactive alert] %s", ev->summary);
      session_broadcast_system_message(note);
   }

   attention_db_log_event(ev, mode, NULL, delivered_at);

   pthread_mutex_lock(&s_mutex);
   if (delivered_at > 0) {
      s_metrics.delivered++;
   } else {
      s_metrics.dropped++;
   }
   pthread_mutex_unlock(&s_mutex);
}

/* =============================================================================
 * Heartbeat tick
 * ============================================================================= */
void attention_tick(time_t now) {
   if (!s_initialized || !g_config.attention.enabled) {
      return;
   }

   /* Nothing configured — skip the snapshot copy entirely on idle heartbeats.
    * (Racy read of s_watch_count is a safe hint: a watch added concurrently is
    * picked up on the next tick.) */
   if (s_watch_count == 0) {
      return;
   }

   int64_t now_ms = (int64_t)now * 1000;

   attention_sample_ctx_t ctx;
   attention_ingest_sample(&ctx, now_ms);

   /* Evaluate every enabled watch under the lock, enqueue fired events. */
   pthread_mutex_lock(&s_mutex);
   for (int i = 0; i < s_watch_count; i++) {
      sage_watch_t *w = &s_watches[i];
      if (!w->enabled) {
         continue;
      }
      /* P1 timed-snooze: skip while muted. */
      if (w->muted_until > 0 && now_ms < w->muted_until) {
         continue;
      }
      double value = 0.0;
      bool present = false;
      const attention_catalog_entry_t *cat = resolve_and_read(&ctx, w->metric, &value, &present);
      s_metrics.evaluations++;
      sage_event_t ev;
      if (attention_gate_eval(w, &s_states[i], value, present, now_ms, cat, &ev)) {
         s_metrics.gate_passed++;
         queue_push_locked(&ev);
      }
   }

   /* Drain the queue into a batch, release the lock, then process.  The tick is
    * main-thread-only, so a file-scope static batch keeps ~large events off the
    * stack without a race. */
   static sage_event_t batch[SAGE_QUEUE_CAP];
   int n = 0;
   while (s_q_count > 0 && n < SAGE_QUEUE_CAP) {
      batch[n++] = s_queue[s_q_head];
      s_q_head = (s_q_head + 1) % SAGE_QUEUE_CAP;
      s_q_count--;
   }
   pthread_mutex_unlock(&s_mutex);

   for (int i = 0; i < n; i++) {
      /* TTL expiry (defense in depth; poll events are fresh). */
      if (batch[i].ttl_ms > 0 && (batch[i].observed_at + batch[i].ttl_ms) < now_ms) {
         pthread_mutex_lock(&s_mutex);
         s_metrics.expired++;
         pthread_mutex_unlock(&s_mutex);
         attention_db_log_event(&batch[i], SAGE_MODE_DROP, "expired", 0);
         continue;
      }
      sage_delivery_mode_t mode = attention_policy_decide(&batch[i], now_ms);
      deliver_event(&batch[i], mode, now_ms);
   }
}

/* =============================================================================
 * Cache load / reload
 * ============================================================================= */

/* True if two watches have the same FIRING config, i.e. carrying the gate state
 * (armed/backoff/slope ring) from one to the other is valid.  Fields that don't
 * affect when a watch fires (name, notify, ttl, enabled, muted) are ignored — so
 * an enable/disable or rename preserves state, but a threshold/direction/type
 * edit resets it (the old hysteresis/backoff history is meaningless for the new
 * condition). */
static bool same_gate_config(const sage_watch_t *a, const sage_watch_t *b) {
   return a->rule_type == b->rule_type && a->direction == b->direction &&
          a->threshold == b->threshold && a->hysteresis == b->hysteresis &&
          a->slope_per_min == b->slope_per_min && a->slope_window_sec == b->slope_window_sec &&
          a->absence_after_sec == b->absence_after_sec;
}

int attention_reload(void) {
   /* Heap the scratch arrays: they are large (sage_rule_state_t carries a 32-deep
    * slope ring) and this runs on the LLM tool-execution thread (small stack) as
    * well as init, so keep them off the stack — and off file scope, which would
    * race between two concurrent CRUD mutations. */
   sage_watch_t *loaded = calloc(SAGE_MAX_WATCHES_TOTAL, sizeof(*loaded));
   sage_rule_state_t *new_states = calloc(SAGE_MAX_WATCHES_TOTAL, sizeof(*new_states));
   if (!loaded || !new_states) {
      OLOG_ERROR("attention: OOM loading watches");
      free(loaded);
      free(new_states);
      return FAILURE;
   }

   /* Load out-of-lock (auth_db is its own leaf lock — never nest). */
   int count = 0;
   if (auth_db_attention_rule_list(0, loaded, SAGE_MAX_WATCHES_TOTAL, &count) != AUTH_DB_SUCCESS) {
      OLOG_ERROR("attention: failed to load watches from DB");
      free(loaded);
      free(new_states);
      return FAILURE;
   }
   if (count >= SAGE_MAX_WATCHES_TOTAL) {
      /* Cache saturated — watches beyond the cap are silently not evaluated. */
      OLOG_WARNING("attention: watch cache full (%d) — some watches will not be evaluated",
                   SAGE_MAX_WATCHES_TOTAL);
   }

   pthread_mutex_lock(&s_mutex);
   /* Preserve per-watch gate state across reloads by matching rule id — but ONLY
    * when the firing config is unchanged.  A threshold/direction/type edit gets a
    * fresh (zeroed) state so stale hysteresis/backoff doesn't leak into the new
    * condition. */
   for (int i = 0; i < count; i++) {
      for (int j = 0; j < s_watch_count; j++) {
         if (s_states[j].rule_id == loaded[i].id) {
            if (same_gate_config(&s_watches[j], &loaded[i])) {
               new_states[i] = s_states[j];
            }
            break;
         }
      }
   }
   memcpy(s_watches, loaded, sizeof(sage_watch_t) * count);
   memcpy(s_states, new_states, sizeof(sage_rule_state_t) * count);
   s_watch_count = count;
   pthread_mutex_unlock(&s_mutex);

   free(loaded);
   free(new_states);
   return SUCCESS;
}

/* =============================================================================
 * Watch CRUD (tool + WebUI); each mutation reloads the cache
 * ============================================================================= */

/* Fill unset fields (NAN doubles, 0 "use-default" ints, empty name) from the
 * metric catalog + module defaults, so "watch CO2" yields a complete watch. */
static int resolve_watch_defaults(sage_watch_t *w) {
   const attention_catalog_entry_t *cat = attention_catalog_lookup(w->metric);
   if (!cat) {
      OLOG_WARNING("attention: unknown metric '%s'", w->metric);
      return FAILURE;
   }
   if (isnan(w->threshold)) {
      w->threshold = cat->default_threshold;
   }
   if (isnan(w->hysteresis)) {
      /* Use the catalog value directly — 0.0 is a DELIBERATE choice for
       * zero-threshold counter metrics (crit/warn faults): a nonzero margin
       * there makes recovered() require a negative count (never happens), so the
       * watch would fire once and never re-arm. */
      w->hysteresis = cat->default_hysteresis;
   }
   if (w->absence_after_sec <= 0) {
      w->absence_after_sec = cat->default_absence_after_sec;
   }
   if (w->slope_window_sec <= 0) {
      w->slope_window_sec = SAGE_SLOPE_WINDOW_SEC_DEFAULT;
   }
   if (w->ttl_min <= 0) {
      w->ttl_min = SAGE_TTL_DEFAULT_MIN;
   }
   /* NOTE: name is deliberately NOT filled here.  Name assignment is owned by the
    * create/update paths (auto-namer for system-owned names, kept as-is for
    * user-named watches) so a blank name never resurrects a stale/colliding
    * catalog-label name on the update path. */
   return SUCCESS;
}

/* =============================================================================
 * Shared rule-kind resolution + trigger validation
 *
 * WS-agnostic so BOTH the WebUI panel and the `attention` LLM tool drive one
 * validator — reimplementing these rules per surface would let the two drift
 * (the "two surfaces can't drift" invariant).  On failure they return FAILURE and
 * set *err_out to a caller-owned static message; the caller renders it (WS reply
 * or tool error string).
 * ============================================================================= */

int attention_resolve_rule_kind(sage_watch_t *w, const char *rule_type_str, const char **err_out) {
   if (err_out) {
      *err_out = NULL;
   }
   if (!w) {
      return FAILURE;
   }
   if (!rule_type_str || !rule_type_str[0]) {
      return SUCCESS; /* keep the current / template kind */
   }
   /* Only the two numeric kinds are user-selectable; any other value (incl. a
    * re-sent "absence"/"match") is a no-op that leaves the current kind alone. */
   sage_rule_type_t requested;
   if (strcmp(rule_type_str, "slope") == 0) {
      requested = SAGE_RULE_SLOPE;
   } else if (strcmp(rule_type_str, "threshold") == 0) {
      requested = SAGE_RULE_THRESHOLD;
   } else {
      return SUCCESS;
   }
   if (w->rule_type == requested) {
      return SUCCESS; /* same kind — nothing to switch */
   }
   /* An actual switch: only a numeric level/rate metric can change kind.  Absence
    * (feed-silence) and match (P1 discrete-event push) have no value to compare. */
   if (w->rule_type == SAGE_RULE_ABSENCE || w->rule_type == SAGE_RULE_MATCH) {
      if (err_out) {
         *err_out = "This metric's rule kind can't be changed";
      }
      return FAILURE;
   }
   w->rule_type = requested;
   if (requested == SAGE_RULE_SLOPE) {
      if (w->direction != SAGE_DIR_RISING && w->direction != SAGE_DIR_FALLING) {
         w->direction = SAGE_DIR_RISING; /* default; an explicit direction may set falling */
      }
   } else { /* SAGE_RULE_THRESHOLD */
      if (w->direction != SAGE_DIR_ABOVE && w->direction != SAGE_DIR_BELOW) {
         w->direction = SAGE_DIR_ABOVE; /* default; an explicit direction may set below */
      }
   }
   return SUCCESS;
}

int attention_validate_watch_trigger(const sage_watch_t *w, const char **err_out) {
   if (err_out) {
      *err_out = NULL;
   }
   if (!w) {
      return FAILURE;
   }
   if (w->rule_type == SAGE_RULE_SLOPE) {
      /* A slope watch fires on rate — a non-positive rate would trip `slope >= 0`
       * every tick. */
      if (!(w->slope_per_min > 0.0)) {
         if (err_out) {
            *err_out = "A rising/falling watch needs a rate (slope_per_min > 0)";
         }
         return FAILURE;
      }
      /* Reject cross-vocabulary direction rather than silently defaulting it. */
      if (w->direction != SAGE_DIR_RISING && w->direction != SAGE_DIR_FALLING) {
         if (err_out) {
            *err_out = "A rising/falling watch needs direction 'rising' or 'falling'";
         }
         return FAILURE;
      }
   } else if (w->rule_type == SAGE_RULE_THRESHOLD) {
      if (w->direction != SAGE_DIR_ABOVE && w->direction != SAGE_DIR_BELOW) {
         if (err_out) {
            *err_out = "An above/below watch needs direction 'above' or 'below'";
         }
         return FAILURE;
      }
   }
   return SUCCESS;
}

int attention_watch_template(const char *metric, int user_id, sage_watch_t *out) {
   if (!metric || !out) {
      return FAILURE;
   }
   const attention_catalog_entry_t *cat = attention_catalog_lookup(metric);
   if (!cat) {
      return FAILURE;
   }
   memset(out, 0, sizeof(*out));
   out->user_id = user_id;
   safe_strncpy(out->metric, metric, sizeof(out->metric));
   safe_strncpy(out->name, cat->label, sizeof(out->name));
   out->rule_type = cat->default_rule_type;
   out->direction = cat->default_direction;
   out->threshold = cat->default_threshold;
   out->hysteresis = cat->default_hysteresis; /* catalog value is authoritative, incl. 0.0 */
   out->absence_after_sec = cat->default_absence_after_sec;
   out->slope_window_sec = SAGE_SLOPE_WINDOW_SEC_DEFAULT;
   out->notify = cat->default_notify;
   out->ttl_min = SAGE_TTL_DEFAULT_MIN;
   out->enabled = true;
   return SUCCESS;
}

/* True if @name (case-insensitive, post-truncation) is already used by one of
 * @user_id's watches OTHER than @exclude_id.  The single seam for user-name
 * uniqueness across create + rename. */
static bool name_taken_by_other(int user_id, const char *name, int64_t exclude_id) {
   if (!name || !name[0]) {
      return false;
   }
   sage_watch_t buf[SAGE_MAX_WATCHES_PER_USER];
   int count = 0;
   if (auth_db_attention_rule_list(user_id, buf, SAGE_MAX_WATCHES_PER_USER, &count) !=
       AUTH_DB_SUCCESS) {
      return false;
   }
   for (int i = 0; i < count; i++) {
      if (buf[i].id != exclude_id && strcasecmp(buf[i].name, name) == 0) {
         return true;
      }
   }
   return false;
}

void attention_sanitize_name(char *name) {
   if (!name) {
      return;
   }
   /* Drop C0 controls + DEL (the name is spoken and broadcast verbatim); the field
    * is already length-clamped by the fixed buffer at copy time. */
   size_t o = 0;
   for (size_t i = 0; name[i]; i++) {
      unsigned char c = (unsigned char)name[i];
      if (c >= 0x20 && c != 0x7f) {
         name[o++] = (char)c;
      }
   }
   name[o] = '\0';
   while (o > 0 && name[o - 1] == ' ') {
      name[--o] = '\0'; /* trim trailing spaces */
   }
}

int attention_watch_add(const sage_watch_t *watch, int64_t *out_id) {
   if (!watch) {
      return FAILURE;
   }
   sage_watch_t w = *watch;
   if (resolve_watch_defaults(&w) != SUCCESS) {
      return FAILURE;
   }
   /* Name assignment (single seam).  A blank OR system-owned name (named=false)
    * gets a unique auto-name — note attention_watch_template pre-fills name with the
    * catalog label, so the `named` flag (not emptiness) is what distinguishes a
    * user name here.  A user-supplied name (named=true) must be unique across the
    * user's watches. */
   if (!w.named || w.name[0] == '\0') {
      w.named = false;
      attention_watch_auto_name(&w, w.name, sizeof(w.name));
   } else if (name_taken_by_other(w.user_id, w.name, 0)) {
      return ATTENTION_NAME_TAKEN;
   }

   int existing = 0;
   if (auth_db_attention_rule_count(w.user_id, &existing) == AUTH_DB_SUCCESS &&
       existing >= SAGE_MAX_WATCHES_PER_USER) {
      OLOG_WARNING("attention: user %d at watch cap (%d)", w.user_id, SAGE_MAX_WATCHES_PER_USER);
      return FAILURE;
   }

   if (auth_db_attention_rule_insert(&w, out_id) != AUTH_DB_SUCCESS) {
      return FAILURE;
   }
   return attention_reload();
}

int attention_watch_update(int user_id, int64_t id, const sage_watch_t *watch) {
   if (!watch) {
      return FAILURE;
   }
   sage_watch_t w = *watch;
   if (resolve_watch_defaults(&w) != SUCCESS) {
      return FAILURE;
   }
   /* H-A auto-name staleness: while the name is system-owned (named=false),
    * regenerate it from the (possibly edited) condition so a name like
    * "CO2 above 1200" never lies after a threshold change.  A user-named watch
    * (named=true) keeps its name; a blank name is auto-filled. */
   if (!w.named || w.name[0] == '\0') {
      w.named = false;
      attention_watch_auto_name(&w, w.name, sizeof(w.name));
   } else if (name_taken_by_other(user_id, w.name, id)) {
      /* User-named: unique across the user's watches, EXCLUDING this one (renaming
       * a watch to its own name is fine). */
      return ATTENTION_NAME_TAKEN;
   }
   /* Normalize auth_db codes to SUCCESS/FAILURE at this boundary — the public
    * attention API contract is SUCCESS/FAILURE (callers must not see auth_db
    * enums).  "Not found" collapses to FAILURE; callers resolve the target via a
    * user-scoped finder first, so this is a plain failure. */
   if (auth_db_attention_rule_update(user_id, id, &w) != AUTH_DB_SUCCESS) {
      return FAILURE;
   }
   return attention_reload();
}

int attention_watch_set_enabled(int user_id, int64_t id, bool enabled) {
   if (auth_db_attention_rule_set_enabled(user_id, id, enabled) != AUTH_DB_SUCCESS) {
      return FAILURE;
   }
   return attention_reload();
}

int attention_watch_remove(int user_id, int64_t id) {
   if (auth_db_attention_rule_delete(user_id, id) != AUTH_DB_SUCCESS) {
      return FAILURE;
   }
   return attention_reload();
}

int attention_watch_list(int user_id, sage_watch_t *out, int max, int *out_count) {
   if (auth_db_attention_rule_list(user_id, out, max, out_count) != AUTH_DB_SUCCESS) {
      return FAILURE;
   }
   return SUCCESS;
}

int attention_watch_find_identical(int user_id, const sage_watch_t *w, sage_watch_t *out) {
   if (!w || !out) {
      return FAILURE;
   }
   /* Compare POST-default-resolution: a fresh template carries NAN threshold until
    * defaults run, so an un-resolved compare would never match. */
   sage_watch_t probe = *w;
   if (resolve_watch_defaults(&probe) != SUCCESS) {
      return FAILURE;
   }
   sage_watch_t buf[SAGE_MAX_WATCHES_PER_USER];
   int count = 0;
   if (auth_db_attention_rule_list(user_id, buf, SAGE_MAX_WATCHES_PER_USER, &count) !=
       AUTH_DB_SUCCESS) {
      return FAILURE;
   }
   for (int i = 0; i < count; i++) {
      if (strcmp(buf[i].metric, probe.metric) == 0 && same_gate_config(&buf[i], &probe)) {
         *out = buf[i];
         return SUCCESS; /* same metric + identical firing config => the same watch */
      }
   }
   return FAILURE;
}

int attention_watch_find_by_name(int user_id, const char *name, sage_watch_t *out) {
   if (!name || !name[0] || !out) {
      return FAILURE;
   }
   sage_watch_t buf[SAGE_MAX_WATCHES_PER_USER];
   int count = 0;
   if (auth_db_attention_rule_list(user_id, buf, SAGE_MAX_WATCHES_PER_USER, &count) !=
       AUTH_DB_SUCCESS) {
      return FAILURE;
   }
   for (int i = 0; i < count; i++) {
      if (strcasecmp(buf[i].name, name) == 0) { /* case-insensitive, unique per user */
         *out = buf[i];
         return SUCCESS;
      }
   }
   return FAILURE;
}

int attention_watch_find_by_id(int user_id, int64_t id, sage_watch_t *out) {
   if (id <= 0 || !out) {
      return FAILURE;
   }
   sage_watch_t buf[SAGE_MAX_WATCHES_PER_USER];
   int count = 0;
   if (auth_db_attention_rule_list(user_id, buf, SAGE_MAX_WATCHES_PER_USER, &count) !=
       AUTH_DB_SUCCESS) {
      return FAILURE;
   }
   for (int i = 0; i < count; i++) {
      if (buf[i].id == id) {
         *out = buf[i];
         return SUCCESS;
      }
   }
   return FAILURE;
}

/* True if @metric is @prefix exactly, or lives under it as a component
 * (prefix "stat.cpu" matches "stat.cpu.temp"/"stat.cpu.load"; "stat" matches all
 * "stat.*").  The '.' guard stops "stat.cpu" from matching "stat.cpuidle". */
static bool metric_under_prefix(const char *metric, const char *prefix) {
   size_t plen = strlen(prefix);
   if (strncmp(metric, prefix, plen) != 0) {
      return false;
   }
   return metric[plen] == '\0' || metric[plen] == '.';
}

int attention_watch_list_by_metric_prefix(int user_id,
                                          const char *prefix,
                                          sage_watch_t *out,
                                          int max,
                                          int *out_count) {
   if (!prefix || !prefix[0] || !out || max <= 0 || !out_count) {
      return FAILURE;
   }
   *out_count = 0;
   sage_watch_t buf[SAGE_MAX_WATCHES_PER_USER];
   int count = 0;
   if (auth_db_attention_rule_list(user_id, buf, SAGE_MAX_WATCHES_PER_USER, &count) !=
       AUTH_DB_SUCCESS) {
      return FAILURE;
   }
   int n = 0;
   for (int i = 0; i < count && n < max; i++) {
      if (metric_under_prefix(buf[i].metric, prefix)) {
         out[n++] = buf[i];
      }
   }
   *out_count = n;
   return SUCCESS;
}

/* Build the base (pre-uniqueness) auto-name for a watch from its condition, e.g.
 * "CO2 above 1200", "temp rising", "helmet link silent". */
static void auto_name_base(const sage_watch_t *w, char *buf, size_t sz) {
   const char *label = attention_catalog_label(w->metric);
   if (!label || !label[0]) {
      label = w->metric;
   }
   const char *unit = attention_catalog_unit(w->metric);
   if (w->rule_type == SAGE_RULE_SLOPE) {
      snprintf(buf, sz, "%s %s", label, (w->direction == SAGE_DIR_FALLING) ? "falling" : "rising");
   } else if (w->rule_type == SAGE_RULE_ABSENCE) {
      snprintf(buf, sz, "%s silent", label);
   } else {
      snprintf(buf, sz, "%s %s %g%s", label, (w->direction == SAGE_DIR_BELOW) ? "below" : "above",
               w->threshold, (unit && unit[0]) ? unit : "");
   }
}

int attention_watch_auto_name(const sage_watch_t *w, char *out, size_t out_sz) {
   if (!w || !out || out_sz == 0) {
      return FAILURE;
   }
   char base[SAGE_WATCH_NAME_LEN];
   auto_name_base(w, base, sizeof(base));

   /* Load the user's existing names for a unique, case-insensitive, post-truncation
    * choice.  A finder failure is non-fatal — fall back to the base name. */
   sage_watch_t buf[SAGE_MAX_WATCHES_PER_USER];
   int count = 0;
   if (auth_db_attention_rule_list(w->user_id, buf, SAGE_MAX_WATCHES_PER_USER, &count) !=
       AUTH_DB_SUCCESS) {
      safe_strncpy(out, base, out_sz);
      return SUCCESS;
   }

   /* base, then "base 2", "base 3", … (excluding this watch's own row so a
    * regenerated auto-name doesn't collide with itself). */
   for (int suffix = 1; suffix <= SAGE_MAX_WATCHES_PER_USER + 1; suffix++) {
      char cand[SAGE_WATCH_NAME_LEN];
      if (suffix == 1) {
         safe_strncpy(cand, base, sizeof(cand));
      } else {
         /* Wide intermediate so the compiler can prove no truncation in the
          * snprintf; then clip to the name field (post-truncation uniqueness). */
         char wide[SAGE_WATCH_NAME_LEN + 16];
         snprintf(wide, sizeof(wide), "%s %d", base, suffix);
         safe_strncpy(cand, wide, sizeof(cand));
      }
      bool taken = false;
      for (int i = 0; i < count; i++) {
         if (buf[i].id != w->id && strcasecmp(buf[i].name, cand) == 0) {
            taken = true;
            break;
         }
      }
      if (!taken) {
         safe_strncpy(out, cand, out_sz);
         return SUCCESS;
      }
   }
   safe_strncpy(out, base, out_sz); /* pathological fallback */
   return SUCCESS;
}

/* =============================================================================
 * Public helpers
 * ============================================================================= */
bool attention_metric_current(const char *key, double *value) {
   if (!key || !value) {
      return false;
   }
   int64_t now_ms = (int64_t)time(NULL) * 1000;
   attention_sample_ctx_t ctx;
   attention_ingest_sample(&ctx, now_ms);
   bool present = false;
   resolve_and_read(&ctx, key, value, &present);
   return present;
}

/* Authoritative hysteresis-aware breach state, read from the gate's persisted
 * `armed` latch (the same bit that gates a fire).  True only for an enabled watch
 * the tick has actually evaluated (rule_id set) and that is currently latched
 * in-breach (!armed); disabled / never-evaluated / recovered => false.  The single
 * definition shared by the readings snapshot and attention_watch_breaching so the
 * two surfaces can't drift.  Caller holds s_mutex. */
static bool watch_is_breaching_locked(const sage_watch_t *w, const sage_rule_state_t *st) {
   return w->enabled && st->rule_id == w->id && !st->armed;
}

int attention_readings_snapshot(int user_id, sage_reading_t *out, int max, int *out_count) {
   if (!out || max <= 0 || !out_count) {
      return FAILURE;
   }
   *out_count = 0;
   if (!s_initialized || user_id <= 0) {
      return SUCCESS; /* nothing to report — not an error */
   }

   /* Sample every source ONCE (like attention_tick), then read each watch off the
    * shared context — so a user's N watches cost one ingest, not N. */
   int64_t now_ms = (int64_t)time(NULL) * 1000;
   attention_sample_ctx_t ctx;
   attention_ingest_sample(&ctx, now_ms);

   int count = 0;
   pthread_mutex_lock(&s_mutex);
   for (int i = 0; i < s_watch_count && count < max; i++) {
      const sage_watch_t *w = &s_watches[i];
      if (w->user_id != user_id) {
         continue;
      }
      double value = 0.0;
      bool present = false;
      resolve_and_read(&ctx, w->metric, &value, &present);
      out[count].id = w->id;
      out[count].has_current = present;
      out[count].value = value;
      out[count].breaching = watch_is_breaching_locked(w, &s_states[i]);
      count++;
   }
   pthread_mutex_unlock(&s_mutex);

   *out_count = count;
   return SUCCESS;
}

bool attention_watch_breaching(int user_id, int64_t id) {
   if (!s_initialized || user_id <= 0 || id <= 0) {
      return false;
   }
   bool breaching = false;
   pthread_mutex_lock(&s_mutex);
   for (int i = 0; i < s_watch_count; i++) {
      if (s_watches[i].id == id && s_watches[i].user_id == user_id) {
         breaching = watch_is_breaching_locked(&s_watches[i], &s_states[i]);
         break;
      }
   }
   pthread_mutex_unlock(&s_mutex);
   return breaching;
}

void attention_get_metrics(attention_metrics_t *out) {
   if (!out) {
      return;
   }
   pthread_mutex_lock(&s_mutex);
   *out = s_metrics;
   pthread_mutex_unlock(&s_mutex);
}

bool attention_is_enabled(void) {
   return s_initialized && g_config.attention.enabled;
}

/* =============================================================================
 * Seed + lifecycle
 * ============================================================================= */

/* Seed a small safety set on first run so life-safety conditions don't depend on
 * the user remembering to ask.  All are user-removable (source='seed'). */
static void seed_safety_watches(void) {
   int count = 0;
   if (auth_db_attention_rule_count(ATTENTION_OWNER_USER_ID, &count) != AUTH_DB_SUCCESS) {
      return;
   }
   if (count > 0) {
      return; /* already have watches — never re-seed */
   }

   struct {
      const char *metric;
      const char *name;
      sage_rule_type_t type;
      sage_direction_t dir;
      sage_notify_t notify;
   } seeds[] = {
      { "stat.battery.soc", "battery critical", SAGE_RULE_THRESHOLD, SAGE_DIR_BELOW,
        SAGE_NOTIFY_ALERT },
      { "suit.co2_ppm", "CO2 high", SAGE_RULE_THRESHOLD, SAGE_DIR_ABOVE, SAGE_NOTIFY_ALERT },
      { "component.hud", "helmet link", SAGE_RULE_ABSENCE, SAGE_DIR_ABOVE, SAGE_NOTIFY_AMBIENT },
   };
   for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
      sage_watch_t w;
      memset(&w, 0, sizeof(w));
      w.user_id = ATTENTION_OWNER_USER_ID;
      safe_strncpy(w.metric, seeds[i].metric, sizeof(w.metric));
      safe_strncpy(w.name, seeds[i].name, sizeof(w.name));
      w.named = true; /* curated seed names ("battery critical") — keep, don't auto-rename */
      w.rule_type = seeds[i].type;
      w.direction = seeds[i].dir;
      w.notify = seeds[i].notify;
      w.enabled = true;
      w.threshold = NAN; /* resolved from catalog */
      w.hysteresis = NAN;
      safe_strncpy(w.source_tag, "seed", sizeof(w.source_tag));
      if (attention_watch_add(&w, NULL) != SUCCESS) {
         OLOG_WARNING("attention: failed to seed watch '%s'", seeds[i].name);
      }
   }
   OLOG_INFO("attention: seeded %zu safety watches for owner", sizeof(seeds) / sizeof(seeds[0]));
}

int attention_init(void) {
   if (s_initialized) {
      return SUCCESS;
   }
   attention_policy_configure(g_config.attention.max_alerts_per_hour);
   attention_policy_reset_budget();
   memset(&s_metrics, 0, sizeof(s_metrics));
   s_watch_count = 0;
   s_q_head = 0;
   s_q_count = 0;
   s_initialized = true;

   seed_safety_watches();
   if (attention_reload() != SUCCESS) {
      OLOG_WARNING("attention: initial watch load failed");
   }

   OLOG_INFO("attention: initialized (%s, %d watch(es), budget %d/hr)",
             g_config.attention.enabled ? "enabled" : "disabled", s_watch_count,
             g_config.attention.max_alerts_per_hour);
   return SUCCESS;
}

void attention_shutdown(void) {
   pthread_mutex_lock(&s_mutex);
   s_watch_count = 0;
   s_q_head = 0;
   s_q_count = 0;
   pthread_mutex_unlock(&s_mutex);
   s_initialized = false;
}
