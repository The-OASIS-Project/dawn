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
 * Background-job session pool (background-jobs Phase 1).  See job_manager.h.
 *
 * LOCKING: s_pool_mutex is a REGISTRY-tier lock (it guards the slot array +
 * running counters + by-id lookups — the same role session_manager_rwlock plays
 * for the interactive pool, NOT a leaf).  It is released before any ref-cond
 * wait, before session_manager_free_bare(), and before any callout — only quick,
 * lock-free work happens under it.  Acquire order: s_pool_mutex (registry) ->
 * session->ref_mutex (leaf).  The resolver is registered with session_manager so
 * session_get()/_for_reconnect() reach the job pool on interactive-array miss.
 */

#include "core/job_manager.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/conv_event.h"
#include "core/event_payload.h"
#include "core/missed_notifications_db.h"
#include "core/scheduler.h"
#include "core/scheduler_db.h"
#include "dawn_error.h"
#include "logging.h"

/* One pool slot: the job session + the provider class it is accounted against
 * (so job_manager_end() decrements the right counter without the caller having
 * to remember it), plus the runtime-reap bookkeeping.
 *
 * started_at lives HERE rather than being read back from conversations.started_at
 * so the 1-Hz overdue scan is a pure in-memory walk — no DB work on the voice
 * loop's heartbeat, running or idle.
 *
 * Field order puts the 8-byte members first: the reap walks this array every
 * second, and the natural declaration order would waste 8 bytes per slot to
 * padding (32 B/slot vs 24 B), costing extra cache lines across a 256-slot pool. */
typedef struct {
   session_t *session;
   time_t started_at; /* slot reservation time; 0 = free, or reap verdict claimed */
   time_t reaped_at;  /* when reap_overdue flagged it; 0 = not reaped */
   time_t nag_at;     /* last re-cancel/re-log while it refuses to die */
   job_provider_class_t provider;
   bool counters_released; /* provider counter already force-released (zombie) */
   bool user_cancelled;    /* the cancel came from job_manager_cancel(), not shutdown */
} job_slot_t;

static job_slot_t *s_slots = NULL; /* [s_pool_size]; NULL session = free slot */
static int s_pool_size = 0;
static int s_n_running_local = 0;
static int s_n_running_cloud = 0;
static bool s_initialized = false;
static pthread_mutex_t s_pool_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Dirty-gate for the completion monitor: set when a job reaches a terminal
 * state, at boot, and when a client the daemon may owe a notice CONNECTS —
 * cleared when the drain finds nothing left to do.  An idle system does zero
 * per-tick DB work, which is why anything that changes the answer has to open
 * the gate explicitly. */
static atomic_bool s_jobs_dirty = false;

/* Set before shutdown cancels the pool.  A worker sees only "my turn was
 * cancelled" and otherwise treats that as the USER having stopped the job —
 * which suppresses the completion notice ("they just asked for this") and
 * records the disposition as 'cancelled', a state only a human may resume.
 * Neither is true when the daemon pulled the rug, so the worker needs to be able
 * to tell the two apart. */
static atomic_bool s_shutting_down = false;

void job_manager_note_shutdown_requested(void) {
   atomic_store(&s_shutting_down, true);
}

bool job_manager_is_shutting_down(void) {
   return atomic_load(&s_shutting_down);
}

/* Daemon boot time — the monitor's restart-safety input.  A job whose terminal
 * state predates this boot (finished_at < s_boot_time) is DELIVERED as an
 * ordinary notification rather than being allowed to re-engage its parent's LLM,
 * because auto-resuming a conversation across a restart is not something the
 * user asked for.  Note "delivered", not "suppressed": they are still owed the
 * news, and this is deliberately a per-tick decision rather than a rewrite of
 * the row, so an explicit Resume (which clears finished_at) gets the
 * re-engagement back for free.  See jobs_monitor_tick. */
static time_t s_boot_time = 0;

/* JOB_MONITOR_MAX_PER_TICK (per-tick drain bound + delivery-batch cap) is
 * defined in job_manager.h so job_reinvoke sizes its group buffers to match. */

/* Defined with the delivery table in the completion-monitor section below. */
static void delivery_table_reset(void);

/* Reinvoke processor (registered by job_reinvoke; NULL until then).  Read on the
 * monitor thread; set once at init — a plain pointer is adequate (init happens
 * before the heartbeat starts handing it rows). */
static job_reinvoke_processor_fn s_reinvoke_processor = NULL;

void job_manager_register_reinvoke_processor(job_reinvoke_processor_fn fn) {
   s_reinvoke_processor = fn;
}

/* WebUI completion toast — weak default reaches nobody; webui_broadcasts.c
 * provides the strong override (silent in-browser banner, no voice).  Returning
 * 0 rather than "delivered" is what keeps a headless build from marking notices
 * it never sent as fired. */
__attribute__((weak)) int webui_broadcast_job_notification(int user_id,
                                                           const char *text,
                                                           int64_t conv_id,
                                                           int running_count) {
   (void)user_id;
   (void)text;
   (void)conv_id;
   (void)running_count;
   return 0;
}

/* Single-job lifecycle push — weak default is a no-op; webui_jobs.c provides the
 * strong override (feeds the client's active-job set, and through it the pills). */
__attribute__((weak)) void webui_broadcast_job_update(int user_id,
                                                      const job_record_t *rec,
                                                      bool resumed) {
   (void)user_id;
   (void)rec;
   (void)resumed;
}

int job_manager_set_terminal(int64_t conv_id,
                             int user_id,
                             const char *status,
                             const char *error_or_null,
                             time_t finished_at,
                             int64_t final_message_id) {
   int rc = conv_db_job_set_terminal(conv_id, status, error_or_null, finished_at);
   /* Emit even when the DB write failed: the job HAS ended, and a tailer that
    * never sees an ending is worse than one that sees an ending the row didn't
    * record.  The event carries the disposition either way. */
   conv_event_emit(conv_id, user_id, CONV_EVENT_COMPLETE,
                   event_payload_complete(status, error_or_null, final_message_id));
   /* Every terminal disposition funnels through here, so this one call retires
    * the job from every client's active set — including the boot interrupted-scan
    * and the spawn-failure path, which the old per-call-site emits missed. */
   job_update_emit(conv_id, user_id);
   return rc;
}

void job_update_emit(int64_t conv_id, int user_id) {
   job_update_emit_ex(conv_id, user_id, false);
}

void job_update_emit_ex(int64_t conv_id, int user_id, bool resumed) {
   if (conv_id <= 0 || user_id <= 0) {
      return;
   }
   job_record_t rec;
   if (conv_db_job_get(conv_id, user_id, &rec) == AUTH_DB_SUCCESS) {
      webui_broadcast_job_update(user_id, &rec, resumed);
   }
}

/* Bounded wait for transient retains to drain in job_manager_end() before free.
 * On timeout we leak the session rather than risk a use-after-free. */
#define JOB_END_DRAIN_TIMEOUT_SEC 10

/* Boot interrupted-scan buffer: a generous margin over the per-tick cap so a
 * restart with many stale running/queued jobs is caught in one pass. */
#define JOB_BOOT_SCAN_MAX (JOB_MONITOR_MAX_PER_TICK * 4)

/* How stale an unannounced completion may be and still be worth announcing.
 * Generous on purpose: it must comfortably cover a daemon restart, an overnight
 * shutdown, or a machine that was off for a few days, because in every one of
 * those the user genuinely has not heard yet.  Its only job is to stop a
 * one-time flood from legacy rows whose fired flag predates the monitor. */
#define JOB_NOTIFY_MAX_STALENESS_SEC (7 * 24 * 60 * 60)

/* =============================================================================
 * Resolver hook (registered with session_manager)
 * ============================================================================= */

/* session_get()/_for_reconnect() fall back here on an interactive-array miss.
 * Scans the pool, retains (ref++) a match.  for_reconnect=false skips a session
 * whose turn has been cancelled (mirrors session_get's disconnected-skip). */
static session_t *job_manager_resolve(uint32_t session_id, bool for_reconnect) {
   pthread_mutex_lock(&s_pool_mutex);
   session_t *found = NULL;
   for (int i = 0; i < s_pool_size; i++) {
      if (s_slots[i].session != NULL && s_slots[i].session->session_id == session_id) {
         found = s_slots[i].session;
         break;
      }
   }
   if (found != NULL) {
      if (!for_reconnect && atomic_load(&found->cancel_requested)) {
         pthread_mutex_unlock(&s_pool_mutex);
         return NULL;
      }
      pthread_mutex_lock(&found->ref_mutex);
      found->ref_count++;
      pthread_mutex_unlock(&found->ref_mutex);
   }
   pthread_mutex_unlock(&s_pool_mutex);
   return found;
}

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

int job_manager_init(void) {
   pthread_mutex_lock(&s_pool_mutex);
   if (s_initialized) {
      pthread_mutex_unlock(&s_pool_mutex);
      return SUCCESS;
   }
   /* Fixed-size pool, NOT sized from max_active_jobs: that setting is
    * runtime-mutable via the WebUI settings panel, so sizing the array from it
    * would let a raised cap pass job_manager_capacity() (a counter comparison)
    * while begin_ex found no free slot in a smaller boot-time array — a phantom
    * "job capacity reached" failure on a freshly created job row.  max_active_jobs
    * is a pure policy counter; nothing is allocated from it.  See JOB_POOL_MAX_SLOTS. */
   s_slots = calloc((size_t)JOB_POOL_MAX_SLOTS, sizeof(job_slot_t));
   if (s_slots == NULL) {
      pthread_mutex_unlock(&s_pool_mutex);
      OLOG_ERROR("job_manager_init: failed to allocate %d job slots", JOB_POOL_MAX_SLOTS);
      return FAILURE;
   }
   s_pool_size = JOB_POOL_MAX_SLOTS;
   s_n_running_local = 0;
   s_n_running_cloud = 0;
   s_boot_time = time(NULL);
   delivery_table_reset(); /* every other module-static here is re-initialised; so is this */
   atomic_store(&s_shutting_down, false); /* ...and so is this: nothing else ever clears it */
   s_initialized = true;
   pthread_mutex_unlock(&s_pool_mutex);

   session_manager_register_job_lookup(job_manager_resolve);

   /* Boot scan: any job left 'running'/'queued' in the DB is stale — its worker
    * died with the previous daemon.  Mark them 'interrupted' (leaving
    * on_complete_fired=0) so the monitor notifies "interrupted"; never
    * auto-reinvoke a parent across a restart.
    *
    * finished_at is stamped just BEFORE this boot, and that is load-bearing, not
    * cosmetic.  It is the more truthful value — the work stopped when the
    * previous daemon died, not when this one started — and the monitor's
    * restart-safety rule is `finished_at < s_boot_time`.  Stamping these `now`
    * would classify precisely the jobs the restart interrupted as "finished
    * during this run" and auto-reinvoke a parent across the restart, which is
    * the one thing that rule exists to prevent. */
   const time_t pre_boot_ts = s_boot_time - 1;
   job_record_t stale[JOB_BOOT_SCAN_MAX];
   int n_stale = 0;
   if (conv_db_job_scan_active(stale, (int)(sizeof(stale) / sizeof(stale[0])), &n_stale) ==
           AUTH_DB_SUCCESS &&
       n_stale > 0) {
      for (int i = 0; i < n_stale; i++) {
         job_manager_set_terminal(stale[i].id, stale[i].user_id, "interrupted", "daemon restarted",
                                  pre_boot_ts, 0);
      }
      OLOG_INFO("job_manager: marked %d interrupted job(s) from a previous run", n_stale);
   }

   /* Open the dirty gate unconditionally so the first tick looks once.
    *
    * Setting it only when the scan above found something was a silent stranding
    * bug: a job that reached a terminal state cleanly just before shutdown is
    * neither stale nor newly-transitioned, so nothing marked the gate, the
    * monitor early-returned every tick, and the completion sat undelivered until
    * some UNRELATED job happened to transition — on an idle daemon, never.  The
    * cleaner the shutdown, the worse the outcome.  The first tick self-closes
    * the gate when there is genuinely nothing pending, so the cost is one query. */
   atomic_store(&s_jobs_dirty, true);

   OLOG_INFO("job_manager: initialized (%d job slots, max_active_jobs=%d)", JOB_POOL_MAX_SLOTS,
             g_config.jobs.max_active_jobs);
   return SUCCESS;
}

void job_manager_shutdown(void) {
   if (!s_initialized) {
      return;
   }
   /* Flag BEFORE cancelling, so a worker that observes the cancel can tell it
    * came from shutdown rather than from the user (see s_shutting_down). */
   atomic_store(&s_shutting_down, true);

   /* Stop new resolves first, then request cancellation of every running job so
    * their workers observe it and tear down via job_manager_end(). */
   session_manager_register_job_lookup(NULL);

   pthread_mutex_lock(&s_pool_mutex);
   int remaining = 0;
   for (int i = 0; i < s_pool_size; i++) {
      if (s_slots[i].session != NULL) {
         session_cancel_turn(s_slots[i].session);
         remaining++;
      }
   }
   if (remaining > 0) {
      /* Detached workers free their own sessions; at process shutdown some may
       * still be in flight.  Leak the slot array rather than free it out from
       * under a worker's job_manager_end() (the caller is responsible for
       * stopping workers before this point). */
      OLOG_WARNING("job_manager_shutdown: %d job(s) still running; leaking pool array", remaining);
   } else {
      free(s_slots);
      s_slots = NULL;
      s_pool_size = 0;
   }
   s_initialized = false;
   pthread_mutex_unlock(&s_pool_mutex);
}

/* =============================================================================
 * Reserve / create
 * ============================================================================= */

int job_manager_begin(int user_id,
                      int64_t conv_id,
                      job_provider_class_t provider,
                      session_t **out) {
   return job_manager_begin_ex(user_id, conv_id, provider, /*count_against_user_cap=*/true, out);
}

int job_manager_begin_ex(int user_id,
                         int64_t conv_id,
                         job_provider_class_t provider,
                         bool count_against_user_cap,
                         session_t **out) {
   if (out == NULL || user_id <= 0) {
      return JOB_MGR_FAIL;
   }
   *out = NULL;

   pthread_mutex_lock(&s_pool_mutex);
   if (!s_initialized) {
      pthread_mutex_unlock(&s_pool_mutex);
      return JOB_MGR_FAIL;
   }

   /* Running caps (all enforced atomically under the pool lock). */
   int running = s_n_running_local + s_n_running_cloud;
   if (running >= g_config.jobs.max_active_jobs) {
      pthread_mutex_unlock(&s_pool_mutex);
      return JOB_MGR_CAP_GLOBAL;
   }
   int prov_cap = (provider == JOB_PROVIDER_LOCAL) ? g_config.jobs.max_concurrent_local
                                                   : g_config.jobs.max_concurrent_cloud;
   int prov_count = (provider == JOB_PROVIDER_LOCAL) ? s_n_running_local : s_n_running_cloud;
   if (prov_count >= prov_cap) {
      pthread_mutex_unlock(&s_pool_mutex);
      return JOB_MGR_CAP_PROVIDER;
   }
   int user_count = 0;
   int slot = -1;
   for (int i = 0; i < s_pool_size; i++) {
      if (s_slots[i].session == NULL) {
         if (slot < 0) {
            slot = i;
         }
         continue;
      }
      /* One live session per job conversation.  A cancelled worker holds its
       * slot until job_manager_end() completes its cancel-then-wait teardown,
       * and a resume of that same job is now one click away — so without this
       * two sessions could share a stream_conversation_id, after which
       * job_manager_cancel()'s first-match scan can signal the dying one and
       * tell the user a still-running job has stopped. */
      if (atomic_load(&s_slots[i].session->stream_conversation_id) == conv_id) {
         pthread_mutex_unlock(&s_pool_mutex);
         return JOB_MGR_CONV_BUSY;
      }
      if ((int)s_slots[i].session->metrics.user_id == user_id) {
         user_count++;
      }
   }
   if (count_against_user_cap && user_count >= g_config.jobs.max_jobs_per_user) {
      pthread_mutex_unlock(&s_pool_mutex);
      return JOB_MGR_CAP_USER;
   }
   if (slot < 0) {
      /* The pool is JOB_POOL_MAX_SLOTS wide and max_active_jobs is clamped to
       * that ceiling, so the counter check above should already have refused —
       * except when a zombie (force-released) slot is still held by a wedged
       * worker.  Report the global cap either way. */
      pthread_mutex_unlock(&s_pool_mutex);
      return JOB_MGR_CAP_GLOBAL;
   }

   session_t *s = session_manager_alloc_bare();
   if (s == NULL) {
      pthread_mutex_unlock(&s_pool_mutex);
      return JOB_MGR_FAIL;
   }
   /* Session is thread-private until published into the slot below, so these
    * fields need no per-session lock here. */
   s->type = SESSION_TYPE_JOB;
   atomic_store(&s->stream_conversation_id, conv_id);
   s->metrics.user_id = user_id;

   s_slots[slot].session = s;
   s_slots[slot].provider = provider;
   s_slots[slot].started_at = time(NULL); /* runtime-reap clock starts here */
   s_slots[slot].reaped_at = 0;
   s_slots[slot].nag_at = 0;
   s_slots[slot].counters_released = false;
   s_slots[slot].user_cancelled = false; /* per-JOB, not per-slot: see job_manager_end */
   if (provider == JOB_PROVIDER_LOCAL) {
      s_n_running_local++;
   } else {
      s_n_running_cloud++;
   }
   pthread_mutex_unlock(&s_pool_mutex);

   *out = s;
   OLOG_INFO("job_manager: started job session %u for conv %lld (user %d, %s)", s->session_id,
             (long long)conv_id, user_id, provider == JOB_PROVIDER_LOCAL ? "local" : "cloud");
   return JOB_MGR_OK;
}

/* =============================================================================
 * Teardown
 * ============================================================================= */

void job_manager_end(session_t *session) {
   if (session == NULL) {
      return;
   }

   /* Remove from the pool + release the provider counter under the registry
    * lock, so no resolver / cancel can reach this session afterward. */
   pthread_mutex_lock(&s_pool_mutex);
   for (int i = 0; i < s_pool_size; i++) {
      if (s_slots[i].session == session) {
         /* Skip the decrement if the reap already force-released this slot's
          * provider counter (the worker was presumed wedged and came back
          * anyway) — decrementing twice would under-count and let the pool
          * over-admit. */
         if (!s_slots[i].counters_released) {
            if (s_slots[i].provider == JOB_PROVIDER_LOCAL) {
               if (s_n_running_local > 0) {
                  s_n_running_local--;
               }
            } else if (s_n_running_cloud > 0) {
               s_n_running_cloud--;
            }
         } else {
            OLOG_INFO("job_manager: presumed-wedged job session %u returned after all",
                      session->session_id);
         }
         s_slots[i].session = NULL;
         s_slots[i].started_at = 0;
         s_slots[i].reaped_at = 0;
         s_slots[i].nag_at = 0;
         s_slots[i].counters_released = false;
         /* Slots are recycled, so a cancel verdict left behind would be read by
          * whichever job lands here next — and a stale `user_cancelled` files a
          * shutdown-interrupted job as user-cancelled, which fires it and tells
          * nobody.  Cleared at both ends for the same reason as the fields above. */
         s_slots[i].user_cancelled = false;
         break;
      }
   }
   pthread_mutex_unlock(&s_pool_mutex);

   /* Cancel-then-wait, holding NO pool lock (arch-HIGH-1).  Ensure any in-flight
    * resolver user aborts, drop the base ref, then wait (bounded) for transient
    * retains to drain before freeing.  On timeout, leak — never a UAF. */
   session_cancel_turn(session);
   session_release(session); /* drop the base ref established by alloc_bare */

   pthread_mutex_lock(&session->ref_mutex);
   bool drained = true;
   while (session->ref_count > 0) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += JOB_END_DRAIN_TIMEOUT_SEC;
      int rc = pthread_cond_timedwait(&session->ref_zero_cond, &session->ref_mutex, &ts);
      if (rc == ETIMEDOUT) {
         drained = false;
         break;
      }
   }
   int leaked_refs = session->ref_count;
   pthread_mutex_unlock(&session->ref_mutex);

   if (!drained) {
      OLOG_WARNING("job_manager_end: session %u ref_count=%d after %ds; leaking (no free)",
                   session->session_id, leaked_refs, JOB_END_DRAIN_TIMEOUT_SEC);
      return;
   }
   session_manager_free_bare(session);
}

/* =============================================================================
 * Cancel / introspection
 * ============================================================================= */

int job_manager_cancel(int64_t conv_id, int user_id) {
   int rc = JOB_MGR_NOT_FOUND;
   pthread_mutex_lock(&s_pool_mutex);
   for (int i = 0; i < s_pool_size; i++) {
      session_t *s = s_slots[i].session;
      if (s != NULL && atomic_load(&s->stream_conversation_id) == conv_id) {
         if ((int)s->metrics.user_id != user_id) {
            rc = JOB_MGR_FORBIDDEN;
         } else {
            /* Record WHO asked before signalling.  session_cancel_turn is a bare
             * flag shared with the shutdown sweep and the runtime reap, so the
             * worker cannot otherwise tell a human's Cancel from the daemon
             * pulling the rug — and the two want opposite dispositions. */
            s_slots[i].user_cancelled = true;
            session_cancel_turn(s); /* atomic store — safe under the pool lock */
            rc = JOB_MGR_OK;
         }
         break;
      }
   }
   pthread_mutex_unlock(&s_pool_mutex);
   return rc;
}

job_cancel_result_t job_manager_cancel_or_retire(int64_t conv_id,
                                                 int user_id,
                                                 char *status_out,
                                                 size_t status_n) {
   if (status_out && status_n > 0) {
      status_out[0] = '\0';
   }
   int rc = job_manager_cancel(conv_id, user_id);
   if (rc == JOB_MGR_OK) {
      return JOB_CANCEL_SIGNALLED;
   }
   if (rc == JOB_MGR_FORBIDDEN) {
      return JOB_CANCEL_FORBIDDEN;
   }

   /* Not running.  Either it never reached a worker (queued) or it is already
    * terminal — the status read is ownership-checked, so a foreign row answers
    * FORBIDDEN here rather than leaking its state. */
   char status[JOB_STATUS_MAX];
   int grc = conv_db_job_get_status(conv_id, user_id, status, sizeof(status));
   if (grc == AUTH_DB_FORBIDDEN) {
      return JOB_CANCEL_FORBIDDEN;
   }
   if (grc != AUTH_DB_SUCCESS || status[0] == '\0') {
      return JOB_CANCEL_NOT_FOUND;
   }
   if (status_out && status_n > 0) {
      snprintf(status_out, status_n, "%s", status);
   }
   if (strcmp(status, "queued") == 0) {
      /* No session to signal, so retire the row directly.  mark_fired suppresses
       * the completion notice: the user just asked for this. */
      job_manager_set_terminal(conv_id, user_id, "cancelled", NULL, time(NULL), 0);
      conv_db_job_mark_fired(conv_id);
      return JOB_CANCEL_RETIRED;
   }
   return JOB_CANCEL_ALREADY_TERMINAL;
}

bool job_manager_conv_is_live(int64_t conv_id) {
   if (conv_id <= 0) {
      return false;
   }
   pthread_mutex_lock(&s_pool_mutex);
   bool live = false;
   for (int i = 0; i < s_pool_size && !live; i++) {
      live = (s_slots[i].session != NULL &&
              atomic_load(&s_slots[i].session->stream_conversation_id) == conv_id);
   }
   pthread_mutex_unlock(&s_pool_mutex);
   return live;
}

int job_manager_running_count(void) {
   pthread_mutex_lock(&s_pool_mutex);
   int n = s_n_running_local + s_n_running_cloud;
   pthread_mutex_unlock(&s_pool_mutex);
   return n;
}

/* What the reap did to one slot this tick, copied out so all logging happens
 * with no lock held. */
typedef enum {
   REAP_NOTE_FIRST,   /* newly flagged: cancel requested */
   REAP_NOTE_NAG,     /* still alive N seconds later: cancel re-issued */
   REAP_NOTE_RELEASED /* grace expired: provider counter force-reclaimed */
} job_reap_note_kind_t;

typedef struct {
   job_reap_note_kind_t kind;
   int64_t conv_id;
   uint32_t session_id;
   long ran_for;
   bool local; /* provider class, for the force-release message */
} job_reap_note_t;

int job_manager_reap_overdue(time_t now) {
   int limit = g_config.jobs.max_runtime_sec;
   if (limit <= 0) {
      return 0; /* reaping disabled */
   }

   /* Bounds LOG volume only — the reap work itself is unbounded because every
    * overdue slot must be acted on in the tick it expires (holding a provider
    * counter is the scarce resource). Any notice past the cap is dropped, not
    * deferred; the next nag re-surfaces it. */
   job_reap_note_t notes[JOB_REAP_LOG_MAX_PER_TICK];
   int n_notes = 0;
   int n_reaped = 0;

   pthread_mutex_lock(&s_pool_mutex);
   if (!s_initialized || (s_n_running_local + s_n_running_cloud) == 0) {
      pthread_mutex_unlock(&s_pool_mutex); /* idle: no scan at all */
      return 0;
   }
   for (int i = 0; i < s_pool_size; i++) {
      session_t *s = s_slots[i].session;
      if (s == NULL || s_slots[i].started_at <= 0) {
         continue; /* free slot, or the worker already claimed its verdict */
      }
      /* Integer subtraction, not difftime(): matches the rest of the codebase
       * and keeps the per-second scan free of a libm call. time_t is signed, so
       * a backwards clock step yields a negative elapsed and reaps nothing. */
      time_t elapsed = now - s_slots[i].started_at;

      if (s_slots[i].reaped_at == 0) {
         if (elapsed <= (time_t)limit) {
            continue; /* not overdue yet */
         }
         /* First strike: request cancellation. This is only OBSERVED where the
          * session cancel flag is polled (the LLM/CURL transfer), so it is a
          * request, not a kill — the nag/force-release below handle a job that
          * ignores it. */
         s_slots[i].reaped_at = now;
         s_slots[i].nag_at = now;
         session_cancel_turn(s); /* atomic store — safe under the pool lock */
         n_reaped++;
         if (n_notes < JOB_REAP_LOG_MAX_PER_TICK) {
            notes[n_notes++] = (job_reap_note_t){ REAP_NOTE_FIRST,
                                                  atomic_load(&s->stream_conversation_id),
                                                  s->session_id, (long)elapsed,
                                                  s_slots[i].provider == JOB_PROVIDER_LOCAL };
         }
         continue;
      }

      /* Already reaped and still here: the worker has not returned. */
      time_t since_reap = now - s_slots[i].reaped_at;

      if (!s_slots[i].counters_released && since_reap > JOB_REAP_FORCE_RELEASE_SEC) {
         /* Grace expired — presume wedged somewhere that never polls the cancel
          * flag (e.g. inside a tool call) and reclaim the PROVIDER COUNTER, which
          * is the scarce resource (max_concurrent_local defaults to 1). The slot
          * itself stays owned so the zombie's session pointer remains valid and
          * job_manager_end() can still run if it ever returns. */
         if (s_slots[i].provider == JOB_PROVIDER_LOCAL) {
            if (s_n_running_local > 0) {
               s_n_running_local--;
            }
         } else if (s_n_running_cloud > 0) {
            s_n_running_cloud--;
         }
         s_slots[i].counters_released = true;
         s_slots[i].nag_at = now;
         if (n_notes < JOB_REAP_LOG_MAX_PER_TICK) {
            notes[n_notes++] = (job_reap_note_t){ REAP_NOTE_RELEASED,
                                                  atomic_load(&s->stream_conversation_id),
                                                  s->session_id, (long)elapsed,
                                                  s_slots[i].provider == JOB_PROVIDER_LOCAL };
         }
         continue;
      }

      if (now - s_slots[i].nag_at >= JOB_REAP_NAG_INTERVAL_SEC) {
         /* Re-issue the cancel and re-log, so a stuck job stays visible instead
          * of producing exactly one line and then going silent forever. */
         s_slots[i].nag_at = now;
         session_cancel_turn(s);
         if (n_notes < JOB_REAP_LOG_MAX_PER_TICK) {
            notes[n_notes++] = (job_reap_note_t){ REAP_NOTE_NAG,
                                                  atomic_load(&s->stream_conversation_id),
                                                  s->session_id, (long)elapsed,
                                                  s_slots[i].provider == JOB_PROVIDER_LOCAL };
         }
      }
   }
   pthread_mutex_unlock(&s_pool_mutex);

   for (int i = 0; i < n_notes; i++) {
      const job_reap_note_t *n = &notes[i];
      switch (n->kind) {
         case REAP_NOTE_FIRST:
            /* Deliberately says "cancel requested", not "marked failed": this
             * function only requests: the WORKER writes the terminal state and the
             * monitor fires the follow-up, and neither happens if it never returns. */
            OLOG_WARNING("job_manager: job conv %lld (session %u) ran %lds, over the %ds [jobs] "
                         "max_runtime_sec — cancel requested",
                         (long long)n->conv_id, n->session_id, n->ran_for, limit);
            break;
         case REAP_NOTE_NAG:
            OLOG_WARNING("job_manager: job conv %lld (session %u) still running %lds after its "
                         "reap — cancel re-issued",
                         (long long)n->conv_id, n->session_id, n->ran_for);
            break;
         case REAP_NOTE_RELEASED:
            OLOG_ERROR("job_manager: job conv %lld (session %u) ignored cancellation for %ds after "
                       "its reap (total %lds) — presumed wedged; force-releasing its %s provider "
                       "slot so new jobs can start. Its pool slot stays held until it returns.",
                       (long long)n->conv_id, n->session_id, JOB_REAP_FORCE_RELEASE_SEC, n->ran_for,
                       n->local ? "local" : "cloud");
            break;
      }
   }
   return n_reaped;
}

bool job_manager_claim_reaped(const session_t *session, bool *user_cancelled_out) {
   if (user_cancelled_out != NULL) {
      *user_cancelled_out = false;
   }
   if (session == NULL) {
      return false;
   }
   bool reaped = false;
   pthread_mutex_lock(&s_pool_mutex);
   for (int i = 0; i < s_pool_size; i++) {
      if (s_slots[i].session == session) {
         reaped = (s_slots[i].reaped_at != 0);
         if (user_cancelled_out != NULL) {
            *user_cancelled_out = s_slots[i].user_cancelled;
         }
         /* Stop the reap clock: the worker has reached its terminal-write block,
          * so the slot must not be flagged (or nagged, or force-released) while
          * it finishes. Clearing started_at is what the scan's skip test reads. */
         s_slots[i].started_at = 0;
         break;
      }
   }
   pthread_mutex_unlock(&s_pool_mutex);
   return reaped;
}

bool job_record_timed_out(const job_record_t *rec) {
   return rec != NULL && strcmp(rec->job_status, "failed") == 0 &&
          strcmp(rec->job_error, JOB_ERR_TIMED_OUT) == 0;
}

/* =============================================================================
 * Completion monitor (heartbeat tick + off-thread delivery)
 * ============================================================================= */

void job_manager_mark_dirty(void) {
   atomic_store(&s_jobs_dirty, true);
}

/* One queued completion notification (copied out so delivery holds no lock). */
typedef struct {
   int64_t job_id;
   int user_id;
   char deliver_to[JOB_DELIVER_TO_MAX];
   char text[512];
} job_notify_t;

typedef struct {
   job_notify_t *items;
   int count;
} job_notify_batch_t;

bool job_notify_user(int user_id, const char *text, int64_t conv_id, int running) {
   if (user_id <= 0 || text == NULL || text[0] == '\0') {
      return false;
   }
   if (webui_broadcast_job_notification(user_id, text, conv_id, running) > 0) {
      return true;
   }
   /* Nobody was listening.  Queue it instead of dropping it: an owner with no
    * tab open is the ordinary case, not an error, and it is guaranteed right
    * after a restart because the daemon always finishes booting before a browser
    * can reconnect.  missed_notif replays on the next authenticated connect and
    * caps itself per user, so this is a durable hand-off rather than a retry —
    * which is what lets the row be retired on the spot instead of sitting at the
    * head of a queue every other user shares. */
   if (missed_notif_insert(user_id, /*event_id=*/0, "reminder", "fired", "Background job", text,
                           time(NULL), conv_id) == SUCCESS) {
      return true;
   }
   OLOG_WARNING("job_manager: could not deliver OR queue the notice for job %lld",
                (long long)conv_id);
   return false;
}

/* Messaging-channel delivery bookkeeping.
 *
 * A notice is fired only once it has been DELIVERED, so an undelivered row keeps
 * coming back from the scan.  That is exactly right for a channel that is
 * momentarily down, and exactly wrong twice over without this table:
 *   - the send is a blocking curl on a detached thread, so the next 1 Hz drain
 *     would queue the same row again and the user would get duplicates;
 *   - an unlinked or misconfigured channel fails every attempt, so with no
 *     ceiling the row would retry every second until the staleness bound dropped
 *     it a week later.
 * `in_flight` answers the first, `attempts` the second. */
#define JOB_NOTIFY_MAX_ATTEMPTS 5
#define JOB_NOTIFY_TRACK_SLOTS (JOB_MONITOR_MAX_PER_TICK * 2)

/* An entry not touched for this long has been retired by some other path (the
 * staleness bound, a cancel, a resume, a deleted parent) and its slot is
 * reclaimed — see the sweep in delivery_claim. */
#define JOB_NOTIFY_TRACK_STALE_SEC 300

typedef struct {
   int64_t job_id;
   time_t last_attempt_at;
   int attempts;
   bool in_flight;
} job_delivery_t;

static job_delivery_t s_delivery[JOB_NOTIFY_TRACK_SLOTS];
static int s_delivery_n = 0;
static pthread_mutex_t s_delivery_mutex = PTHREAD_MUTEX_INITIALIZER; /* leaf */

static void delivery_table_reset(void) {
   pthread_mutex_lock(&s_delivery_mutex);
   s_delivery_n = 0;
   pthread_mutex_unlock(&s_delivery_mutex);
}

/* Reserve @job_id for a delivery attempt.  false means the caller must NOT
 * attempt it; @give_up separates the two reasons, because a spent ceiling has to
 * be retired by this caller (nobody else will) while an in-flight row belongs to
 * the thread that already claimed it. */
static bool delivery_claim(int64_t job_id, time_t now, bool *give_up) {
   *give_up = false;
   bool ok = false;
   pthread_mutex_lock(&s_delivery_mutex);
   /* -1 = not tracked yet.  Must NOT be seeded from s_delivery_n, because the
    * sweep below decrements s_delivery_n as it removes dead rows — a stale index
    * captured up front would then miss the "not found" test and read (or, when
    * the table was full, write one past) a dead slot. */
   int idx = -1;
   for (int i = 0; i < s_delivery_n;) {
      /* Sweep abandoned entries while looking.  An entry leaves cleanly on
       * delivery or at the ceiling, but its row can also be retired out from
       * under it — by the staleness bound, a cancel-retire, a resume, or a
       * deleted parent — and nothing would ever remove it then.  Without the
       * sweep the table fills with dead rows and the full-table branch below
       * blocks messaging delivery outright. */
      if (s_delivery[i].job_id != job_id && !s_delivery[i].in_flight &&
          (now - s_delivery[i].last_attempt_at) > JOB_NOTIFY_TRACK_STALE_SEC) {
         s_delivery[i] = s_delivery[s_delivery_n - 1];
         s_delivery_n--;
         continue; /* re-test the row swapped into i */
      }
      if (s_delivery[i].job_id == job_id) {
         idx = i;
         break;
      }
      i++;
   }
   if (idx < 0) {
      if (s_delivery_n >= JOB_NOTIFY_TRACK_SLOTS) {
         /* Full: refuse, do NOT proceed untracked.  An untracked attempt has
          * neither the in_flight guard nor the attempt ceiling, so it would
          * re-send every tick and spawn a thread per second — exactly the two
          * failures this table exists to prevent.  Refusing is safe because the
          * row stays unfired and is retried once a slot frees. */
         ok = false;
      } else {
         s_delivery[s_delivery_n].job_id = job_id;
         s_delivery[s_delivery_n].attempts = 1;
         s_delivery[s_delivery_n].in_flight = true;
         s_delivery[s_delivery_n].last_attempt_at = now;
         s_delivery_n++;
         ok = true;
      }
   } else if (s_delivery[idx].in_flight) {
      ok = false; /* someone else's to finish */
   } else if (s_delivery[idx].attempts >= JOB_NOTIFY_MAX_ATTEMPTS) {
      *give_up = true;
   } else {
      s_delivery[idx].attempts++;
      s_delivery[idx].in_flight = true;
      s_delivery[idx].last_attempt_at = now;
      ok = true;
   }
   pthread_mutex_unlock(&s_delivery_mutex);
   return ok;
}

/* Release a claim.  A row that is done with (delivered, or retired at the
 * ceiling) leaves the table; a failed attempt keeps its count so the ceiling
 * means something. */
static void delivery_release(int64_t job_id, bool finished) {
   pthread_mutex_lock(&s_delivery_mutex);
   for (int i = 0; i < s_delivery_n; i++) {
      if (s_delivery[i].job_id == job_id) {
         if (finished) {
            s_delivery[i] = s_delivery[s_delivery_n - 1];
            s_delivery_n--;
         } else {
            s_delivery[i].in_flight = false;
         }
         break;
      }
   }
   pthread_mutex_unlock(&s_delivery_mutex);
}

/* Test-only shims over the delivery bookkeeping.  Kept out of the public header
 * (the unit test forward-declares them); the statics above stay static.  Exists
 * so the sweep-then-claim OOB corner — a full table swept just before a
 * not-tracked claim — can be pinned without driving the whole notify path. */
bool job_manager_delivery_claim_for_test(int64_t job_id, time_t now, bool *give_up) {
   return delivery_claim(job_id, now, give_up);
}
void job_manager_delivery_release_for_test(int64_t job_id, bool finished) {
   delivery_release(job_id, finished);
}
void job_manager_delivery_reset_for_test(void) {
   delivery_table_reset();
}
int job_manager_delivery_slots_for_test(void) {
   return JOB_NOTIFY_TRACK_SLOTS;
}
int job_manager_delivery_stale_sec_for_test(void) {
   return JOB_NOTIFY_TRACK_STALE_SEC;
}

/* Detached delivery thread for MESSAGING-channel completions only:
 * scheduler_send_to_messaging_channel (via scheduler_emit_alert) does blocking
 * curl, so it must not run on the main-loop heartbeat (arch-HIGH-3).  Voice is
 * OFF — jobs notify via the browser toast / messaging channel, never the local
 * speaker.
 *
 * The row is fired HERE, on success, rather than by the tick that queued it:
 * marking it up front makes "we tried" indistinguishable from "they heard", and
 * a channel that is down at that moment loses the completion permanently. */
static void *job_notify_thread(void *arg) {
   job_notify_batch_t *b = (job_notify_batch_t *)arg;
   for (int i = 0; i < b->count; i++) {
      const job_notify_t *it = &b->items[i];
      bool delivered = (scheduler_emit_alert(it->user_id, it->text, SCHED_EVENT_REMINDER,
                                             it->deliver_to, /*speak=*/false) == SUCCESS);
      if (delivered) {
         conv_db_job_mark_fired(it->job_id);
      } else {
         OLOG_WARNING("job_manager: completion notice for job %lld to '%s' failed; will retry",
                      (long long)it->job_id, it->deliver_to);
      }
      delivery_release(it->job_id, delivered);
   }
   free(b->items);
   free(b);
   /* Unconditionally, and only once every claim above has been released.
    *
    * Success is progress and may unblock rows behind these — the scan is a
    * GLOBAL `finished_at ASC LIMIT n`, so while these were in flight they
    * occupied the whole window and every tick meanwhile made no progress and
    * closed the gate.  Waking on failure alone therefore stranded the rest of
    * the backlog on the ordinary success path.  Marking after the releases is
    * what stops a tick landing in between, finding the rows still in_flight,
    * and closing the gate on a row that has just become retryable. */
   job_manager_mark_dirty();
   return NULL;
}

void jobs_monitor_tick(time_t now) {
   /* Runtime reap runs BEFORE the dirty-gate: a wedged job never reaches a
    * terminal state, so it never marks anything dirty — gating the reap on the
    * flag would mean the one case it exists for is the one case it never sees.
    * It is an in-memory pool walk that early-outs when nothing is running, so
    * the idle tick still does zero DB work. */
   job_manager_reap_overdue(now);

   if (!s_initialized) {
      /* [jobs] disabled — job_manager_init() never ran, so s_boot_time is 0 and
       * no reinvoke processor is registered.  The heartbeat calls this
       * unconditionally, so without the guard anything that set the dirty flag
       * would drive the drain through an uninitialised subsystem and retire
       * legacy job rows with the restart-safety rule permanently false. */
      return;
   }

   if (!atomic_load(&s_jobs_dirty)) {
      return; /* idle: zero per-tick DB work */
   }
   /* Clear before scanning: a job that goes terminal after this re-sets it. */
   atomic_store(&s_jobs_dirty, false);

   int max = g_config.jobs.monitor_followups_per_tick;
   if (max < 1) {
      max = 1;
   }
   if (max > JOB_MONITOR_MAX_PER_TICK) {
      max = JOB_MONITOR_MAX_PER_TICK;
   }

   job_record_t rows[JOB_MONITOR_MAX_PER_TICK];
   int n = 0;
   if (conv_db_job_list_pending_followups(max, rows, &n) != AUTH_DB_SUCCESS) {
      /* Re-arm the gate: the scan was cleared above, so returning without it
       * would strand every pending follow-up until some UNRELATED job transition
       * happened to mark dirty again — on an otherwise idle system, never.
       * A transient SQLITE_BUSY must cost one tick, not the delivery. */
      atomic_store(&s_jobs_dirty, true);
      OLOG_WARNING("jobs_monitor_tick: follow-up scan failed; retrying next tick");
      return;
   }
   if (n == 0) {
      return; /* genuinely nothing pending — leave the gate closed */
   }

   int running = job_manager_running_count();

   /* Partition: reinvoke_parent rows are handed to the reinvoke processor and
    * are NOT marked fired here (the reinvoke worker fires them only after the
    * re-engagement persists).  Everything else is the notify/none path. */
   job_record_t reinvoke_rows[JOB_MONITOR_MAX_PER_TICK];
   int n_reinvoke = 0;

   /* Messaging-channel completions deliver off-thread (blocking curl); local
    * completions get a silent browser toast, pushed here (non-blocking). */
   job_notify_t *msg_items = calloc((size_t)n, sizeof(job_notify_t));
   int msg_count = 0;

   const time_t now_ts = time(NULL);

   /* Did this pass move ANYTHING forward?  Only then is it worth re-arming the
    * gate on a full batch: a batch of rows that all failed to deliver has
    * changed nothing, and re-arming on it turns the dirty gate — whose entire
    * purpose is that an idle daemon does zero DB work — into a 1 Hz scan that
    * never stops. */
   bool progressed = false;

   for (int i = 0; i < n; i++) {
      const bool is_reinvoke = strcmp(rows[i].on_complete, "reinvoke_parent") == 0;

      /* Restart safety (§143): never auto-reinvoke a parent across a restart.
       * A row whose terminal state predates this boot is DELIVERED as an
       * ordinary notification instead — note *delivered*, not rewritten.  Boot
       * used to rewrite on_complete to 'notify' in the DB, which is permanent:
       * a job interrupted by the restart and then explicitly Resumed completed
       * and only toasted, its results never returning to the parent that asked
       * for them — and since the boot scan terminalizes every in-flight row
       * first, that was the whole population Resume exists for.  Deciding here
       * instead needs no extra state and no schema: Resume clears finished_at,
       * so a resumed run naturally regains its disposition. */
      const bool cross_restart = is_reinvoke && rows[i].finished_at < s_boot_time;

      if (is_reinvoke && !cross_restart && s_reinvoke_processor != NULL) {
         /* Handed off, NOT progressed: the processor owns firing these, and it
          * may well defer every one of them.  Counting the hand-off as progress
          * would re-arm the gate on a batch that moved nothing — a permanent
          * 1 Hz scan whenever the head of the queue is a busy parent.  The
          * processor re-arms on defer, and the worker re-arms on completion, so
          * the wake-up still comes from whoever actually did something. */
         reinvoke_rows[n_reinvoke++] = rows[i];
         continue;
      }

      /* 'none' is the only disposition that owes the user nothing: retire the
       * row silently.  Everything else owes a notice — including a reinvoke row
       * demoted just above, and one in a build with no reinvoke processor linked
       * at all, which can never be re-engaged. */
      if (strcmp(rows[i].on_complete, "none") == 0) {
         conv_db_job_mark_fired(rows[i].id);
         progressed = true;
         continue;
      }

      /* The staleness bound is the one UNCONDITIONAL fire.  Everything below
       * fires on delivery only, so this is what stops an ancient row retrying
       * forever.  Generous on purpose (a week): a daemon restart, an overnight
       * shutdown or a machine off for days are all cases where the user
       * genuinely has not heard yet.  Its real job is to stop a one-time flood
       * from legacy rows whose fired flag predates the monitor; anything older
       * is left to the jobs panel, which still shows it with its result. */
      if (rows[i].finished_at < now_ts - JOB_NOTIFY_MAX_STALENESS_SEC) {
         conv_db_job_mark_fired(rows[i].id);
         progressed = true;
         continue;
      }

      bool timed_out = job_record_timed_out(&rows[i]);
      const char *verb = (strcmp(rows[i].job_status, "done") == 0)          ? "is done"
                         : (strcmp(rows[i].job_status, "interrupted") == 0) ? "was interrupted"
                         : timed_out                                        ? "timed out"
                                                                            : "failed";
      const char *tail = (strcmp(rows[i].job_status, "done") == 0) ? " Ask me for the result." : "";
      char text[512];
      snprintf(text, sizeof(text), "Background job \"%s\" %s.%s", rows[i].title, verb, tail);

      if (rows[i].deliver_to[0] != '\0' && msg_items != NULL) {
         /* Messaging channel: blocking curl, so it is queued to the delivery
          * thread and fired there, on success only.  Claim AFTER the msg_items
          * check — claiming first would burn an attempt on an allocation failure
          * that never reached the wire. */
         bool give_up = false;
         if (!delivery_claim(rows[i].id, now_ts, &give_up)) {
            if (give_up) {
               /* Out of attempts.  Retire the row so it stops holding the head
                * of a queue every user shares, but hand the notice to the
                * durable path first — giving up on a CHANNEL is not a reason to
                * give up on telling the user. */
               OLOG_WARNING("job_manager: giving up on job %lld notice to '%s' after %d attempts",
                            (long long)rows[i].id, rows[i].deliver_to, JOB_NOTIFY_MAX_ATTEMPTS);
               job_notify_user(rows[i].user_id, text, rows[i].id, running);
               conv_db_job_mark_fired(rows[i].id);
               delivery_release(rows[i].id, /*finished=*/true);
               progressed = true;
            }
            continue; /* otherwise a thread already owns it, or the table is full */
         }
         job_notify_t *it = &msg_items[msg_count++];
         it->job_id = rows[i].id;
         it->user_id = rows[i].user_id;
         snprintf(it->deliver_to, sizeof(it->deliver_to), "%s", rows[i].deliver_to);
         snprintf(it->text, sizeof(it->text), "%s", text);
         progressed = true;
      } else {
         /* Browser toast, or a messaging row we could not queue.  job_notify_user
          * either reaches a live browser or queues the text as a missed
          * notification, so the answer is durable and the row can be retired on
          * the spot.  That last part matters more than it looks: the scan is
          * GLOBAL and only a few rows deep, so a row left waiting for its owner
          * to open a tab would sit at the head of it — blocking every other
          * user's completions, for up to the staleness bound. */
         job_notify_user(rows[i].user_id, text, rows[i].id, running);
         conv_db_job_mark_fired(rows[i].id);
         progressed = true;
      }
   }

   /* Drained a full batch AND moved something → likely more waiting behind it. */
   if (n >= max && progressed) {
      atomic_store(&s_jobs_dirty, true);
   }

   /* Hand reinvoke_parent completions to the reinvoke processor (bounded,
    * non-blocking work only — grouping + spawning detached workers).  Rows it
    * defers re-arm the dirty gate internally. */
   if (n_reinvoke > 0 && s_reinvoke_processor != NULL) {
      s_reinvoke_processor(reinvoke_rows, n_reinvoke);
   }

   if (msg_count == 0 || msg_items == NULL) {
      free(msg_items);
      return;
   }

   /* Every queued item holds a delivery claim that only the delivery thread
    * releases.  If the batch never reaches that thread the claims must be handed
    * back HERE, or those rows stay in_flight forever — unfired, so still owed,
    * but permanently ineligible to be re-queued.  That is silent loss, which is
    * the failure mode this whole change exists to remove. */
   job_notify_batch_t *batch = malloc(sizeof(*batch));
   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   bool handed_off = false;
   if (batch != NULL) {
      batch->items = msg_items;
      batch->count = msg_count;
      handed_off = (pthread_create(&thread, &attr, job_notify_thread, batch) == 0);
   }
   pthread_attr_destroy(&attr);

   if (!handed_off) {
      OLOG_WARNING("jobs_monitor_tick: could not spawn delivery thread; %d notice(s) requeued",
                   msg_count);
      for (int i = 0; i < msg_count; i++) {
         delivery_release(msg_items[i].job_id, /*finished=*/false);
      }
      free(msg_items);
      free(batch);
      atomic_store(&s_jobs_dirty, true); /* still owed — try again next tick */
   }
}

int job_manager_capacity(int user_id, job_provider_class_t provider) {
   pthread_mutex_lock(&s_pool_mutex);
   if (!s_initialized) {
      pthread_mutex_unlock(&s_pool_mutex);
      return JOB_MGR_FAIL;
   }
   int rc = JOB_MGR_OK;
   if (s_n_running_local + s_n_running_cloud >= g_config.jobs.max_active_jobs) {
      rc = JOB_MGR_CAP_GLOBAL;
   } else {
      int prov_cap = (provider == JOB_PROVIDER_LOCAL) ? g_config.jobs.max_concurrent_local
                                                      : g_config.jobs.max_concurrent_cloud;
      int prov_count = (provider == JOB_PROVIDER_LOCAL) ? s_n_running_local : s_n_running_cloud;
      if (prov_count >= prov_cap) {
         rc = JOB_MGR_CAP_PROVIDER;
      } else {
         int user_count = 0;
         for (int i = 0; i < s_pool_size; i++) {
            if (s_slots[i].session != NULL && (int)s_slots[i].session->metrics.user_id == user_id) {
               user_count++;
            }
         }
         if (user_count >= g_config.jobs.max_jobs_per_user) {
            rc = JOB_MGR_CAP_USER;
         }
      }
   }
   pthread_mutex_unlock(&s_pool_mutex);
   return rc;
}
