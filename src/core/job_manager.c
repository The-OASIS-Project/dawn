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
} job_slot_t;

static job_slot_t *s_slots = NULL; /* [s_pool_size]; NULL session = free slot */
static int s_pool_size = 0;
static int s_n_running_local = 0;
static int s_n_running_cloud = 0;
static bool s_initialized = false;
static pthread_mutex_t s_pool_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Dirty-gate for the completion monitor: set when a job reaches a terminal
 * state (or boot finds interrupted rows), cleared when the drain finds nothing.
 * An idle system does zero per-tick DB work. */
static atomic_bool s_jobs_dirty = false;

/* Set before shutdown cancels the pool.  A worker sees only "my turn was
 * cancelled" and otherwise treats that as the USER having stopped the job —
 * which suppresses the completion notice ("they just asked for this") and
 * records the disposition as 'cancelled', a state only a human may resume.
 * Neither is true when the daemon pulled the rug, so the worker needs to be able
 * to tell the two apart. */
static atomic_bool s_shutting_down = false;

bool job_manager_is_shutting_down(void) {
   return atomic_load(&s_shutting_down);
}

/* Daemon boot time.  The monitor only DELIVERS a notification for a job that
 * reached a terminal state during THIS run (finished_at >= s_boot_time) — a job
 * that finished before this boot (e.g. an old completed job whose fired flag
 * predates the monitor) is marked fired but NOT re-announced, so a restart never
 * replays stale completions. */
static time_t s_boot_time = 0;

/* JOB_MONITOR_MAX_PER_TICK (per-tick drain bound + delivery-batch cap) is
 * defined in job_manager.h so job_reinvoke sizes its group buffers to match. */

/* Reinvoke processor (registered by job_reinvoke; NULL until then).  Read on the
 * monitor thread; set once at init — a plain pointer is adequate (init happens
 * before the heartbeat starts handing it rows). */
static job_reinvoke_processor_fn s_reinvoke_processor = NULL;

void job_manager_register_reinvoke_processor(job_reinvoke_processor_fn fn) {
   s_reinvoke_processor = fn;
}

/* WebUI completion toast — weak default is a no-op; webui_broadcasts.c provides
 * the strong override (silent in-browser banner, no voice). */
__attribute__((weak)) void webui_broadcast_job_notification(int user_id,
                                                            const char *text,
                                                            int64_t conv_id,
                                                            int running_count) {
   (void)user_id;
   (void)text;
   (void)conv_id;
   (void)running_count;
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
   s_initialized = true;
   pthread_mutex_unlock(&s_pool_mutex);

   session_manager_register_job_lookup(job_manager_resolve);

   /* Boot scan: any job left 'running'/'queued' in the DB is stale — its worker
    * died with the previous daemon.  Mark them 'interrupted' (leaving
    * on_complete_fired=0) so the monitor notifies "interrupted"; never
    * auto-reinvoke a parent across a restart. */
   job_record_t stale[JOB_BOOT_SCAN_MAX];
   int n_stale = 0;
   if (conv_db_job_scan_active(stale, (int)(sizeof(stale) / sizeof(stale[0])), &n_stale) ==
           AUTH_DB_SUCCESS &&
       n_stale > 0) {
      for (int i = 0; i < n_stale; i++) {
         job_manager_set_terminal(stale[i].id, stale[i].user_id, "interrupted", "daemon restarted",
                                  time(NULL), 0);
      }
      atomic_store(&s_jobs_dirty, true);
      OLOG_INFO("job_manager: marked %d interrupted job(s) from a previous run", n_stale);
   }

   /* Restart-safety (design §143): never auto-reinvoke a parent across a restart.
    * Downgrade any terminal + not-yet-fired reinvoke_parent job to 'notify', so
    * the monitor won't re-engage the LLM for a job that finished before this boot
    * — but still TELLS the user it finished.  (Marking the row fired instead, as
    * this did originally, suppressed the notification too, because one flag
    * serves both dispositions; a job that finished just before a restart then
    * vanished silently.  This also keeps a stale reinvoke row from
    * head-of-line-blocking the finished_at-ordered drain.) */
   int n_downgraded = 0;
   if (conv_db_job_downgrade_boot_reinvokes(&n_downgraded) == AUTH_DB_SUCCESS && n_downgraded > 0) {
      atomic_store(&s_jobs_dirty, true); /* wake the monitor to deliver them */
      OLOG_INFO("job_manager: %d pre-restart job(s) downgraded to notify (no auto-reinvoke)",
                n_downgraded);
   }

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

bool job_manager_claim_reaped(const session_t *session) {
   if (session == NULL) {
      return false;
   }
   bool reaped = false;
   pthread_mutex_lock(&s_pool_mutex);
   for (int i = 0; i < s_pool_size; i++) {
      if (s_slots[i].session == session) {
         reaped = (s_slots[i].reaped_at != 0);
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
   int user_id;
   char deliver_to[JOB_DELIVER_TO_MAX];
   char text[512];
} job_notify_t;

typedef struct {
   job_notify_t *items;
   int count;
} job_notify_batch_t;

/* Detached delivery thread for MESSAGING-channel completions only:
 * scheduler_send_to_messaging_channel (via scheduler_emit_alert) does blocking
 * curl, so it must not run on the main-loop heartbeat (arch-HIGH-3).  Voice is
 * OFF — jobs notify via the browser toast / messaging channel, never the local
 * speaker.  Rows were marked fired on the tick, so no double-deliver. */
static void *job_notify_thread(void *arg) {
   job_notify_batch_t *b = (job_notify_batch_t *)arg;
   for (int i = 0; i < b->count; i++) {
      scheduler_emit_alert(b->items[i].user_id, b->items[i].text, SCHED_EVENT_REMINDER,
                           b->items[i].deliver_to, /*speak=*/false);
   }
   free(b->items);
   free(b);
   return NULL;
}

void jobs_monitor_tick(time_t now) {
   /* Runtime reap runs BEFORE the dirty-gate: a wedged job never reaches a
    * terminal state, so it never marks anything dirty — gating the reap on the
    * flag would mean the one case it exists for is the one case it never sees.
    * It is an in-memory pool walk that early-outs when nothing is running, so
    * the idle tick still does zero DB work. */
   job_manager_reap_overdue(now);

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
   /* Drained a full batch → likely more waiting; keep the gate hot. */
   if (n >= max) {
      atomic_store(&s_jobs_dirty, true);
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

   for (int i = 0; i < n; i++) {
      if (s_reinvoke_processor != NULL && strcmp(rows[i].on_complete, "reinvoke_parent") == 0) {
         reinvoke_rows[n_reinvoke++] = rows[i]; /* processor owns firing */
         continue;
      }

      /* Mark fired now (on the tick) so the next tick can't re-drain the same
       * row while delivery is in flight.  A rare delivery failure loses the
       * notify, but the result stays retrievable via `job status`. */
      conv_db_job_mark_fired(rows[i].id);

      if (strcmp(rows[i].on_complete, "notify") != 0) {
         continue; /* on_complete 'none' → mark fired, no delivery */
      }
      /* Announce anything still OWED that finished recently — including across a
       * restart, which is the case that matters most: the daemon goes down while
       * a job is running, or a job finishes moments before shutdown, and the user
       * has no other way to learn about it.
       *
       * This used to read `finished_at < s_boot_time` — "only completions from
       * THIS run" — which suppressed precisely those.  Together with the boot
       * scan marking rows fired, a job that ended around a restart was silenced
       * twice over.
       *
       * `on_complete_fired` is the real "has this been announced?" answer; the
       * bound below exists only for the narrow case the original rule was
       * reaching for — a one-time flood of legacy rows whose flag predates the
       * monitor.  Anything older than this is left to the jobs panel, which
       * still shows it with its result. */
      if (rows[i].finished_at < now_ts - JOB_NOTIFY_MAX_STALENESS_SEC) {
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

      if (rows[i].deliver_to[0] != '\0') {
         if (msg_items != NULL) {
            job_notify_t *it = &msg_items[msg_count++];
            it->user_id = rows[i].user_id;
            snprintf(it->deliver_to, sizeof(it->deliver_to), "%s", rows[i].deliver_to);
            snprintf(it->text, sizeof(it->text), "%s", text);
         }
      } else {
         /* Silent in-browser toast (weak symbol → no-op without WebUI). */
         webui_broadcast_job_notification(rows[i].user_id, text, rows[i].id, running);
      }
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

   job_notify_batch_t *batch = malloc(sizeof(*batch));
   if (batch == NULL) {
      free(msg_items);
      return;
   }
   batch->items = msg_items;
   batch->count = msg_count;

   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   if (pthread_create(&thread, &attr, job_notify_thread, batch) != 0) {
      OLOG_WARNING("jobs_monitor_tick: failed to spawn delivery thread; %d notice(s) dropped",
                   msg_count);
      free(batch->items);
      free(batch);
   }
   pthread_attr_destroy(&attr);
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
