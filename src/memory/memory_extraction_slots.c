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
 * Who may extract now: the per-user extraction slots, the holds a forget
 * takes, and the queue of conversations waiting for a slot (started when one
 * is released).  The extraction itself is memory_extraction.c.
 */

#define MEMORY_EXTRACTION_INTERNAL_ALLOWED

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "logging.h"
#include "memory/memory_extraction.h"
#include "memory/memory_extraction_internal.h"
#include "memory/memory_extraction_queue.h"
#include "memory/memory_history_loader.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Extraction Concurrency Tracking
 *
 * Tracks in-progress extractions using a bounded array instead of a bitmap.
 * This removes the 64-user limit and enables system-wide concurrency limiting
 * tied to max_clients configuration.
 * ============================================================================= */

#define MAX_EXTRACTION_SLOTS 16 /* Compile-time max; runtime limit is min(this, max_clients) */

static struct {
   int user_ids[MAX_EXTRACTION_SLOTS]; /* User IDs with active extractions (0 = empty slot) */
   int count;                          /* Current number of running extractions */
   /* Users held by memory_extraction_hold_user (no LLM call; 0 = empty). */
   int held_user_ids[MEMORY_EXTRACTION_MAX_HOLDS];
   /* Users a hold is waiting for, one entry per waiting call: a queued
    * extraction for them waits until the hold has had its turn. */
   int hold_waiter_ids[MEMORY_EXTRACTION_MAX_HOLDS];
} s_extraction_state;
static pthread_mutex_t s_extraction_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Set at shutdown: nothing more is queued or started from the queue. */
static atomic_bool s_queue_stopped;
/* Signalled whenever a slot is released (see memory_extraction_hold_user).  Uses
 * CLOCK_MONOTONIC so a wall-clock change can't stretch or cut a timed wait. */
static pthread_cond_t s_extraction_cond;
static pthread_once_t s_extraction_cond_once = PTHREAD_ONCE_INIT;

static void extraction_cond_init(void) {
   pthread_condattr_t attr;
   pthread_condattr_init(&attr);
   pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
   pthread_cond_init(&s_extraction_cond, &attr);
   pthread_condattr_destroy(&attr);
}

/* Last-extraction-outcome flag for the recovery/reextract orchestrator.
 * Set by the extraction thread when an LLM call signals transient failure
 * (LLM_ERR_TRANSIENT_NETWORK), read+cleared by memory_recovery's
 * post-wait branch via memory_extraction_consume_last_transient().  Lives
 * outside the slot table because the slot is released BEFORE the recovery
 * worker observes "not in progress" — the flag must survive that
 * transition.  Single-pair design: recovery serializes within a pass and
 * only waits on one user at a time, so race with a concurrent webui
 * session-end extraction is rare and best-effort.  Guarded by the
 * existing s_extraction_mutex to avoid adding another lock. */
static struct {
   int last_user_id;
   bool was_transient;
} s_last_outcome = { 0, false };

/* Stamp the most-recent outcome under the extraction mutex.  Called only
 * from the extraction thread before it releases the slot. */
static void set_last_outcome_locked(int user_id, bool was_transient) {
   s_last_outcome.last_user_id = user_id;
   s_last_outcome.was_transient = was_transient;
}

/* Helper: Get runtime concurrency limit */
static int get_max_concurrent_extractions(void) {
   int limit = g_config.webui.max_clients;
   if (limit <= 0 || limit > MAX_EXTRACTION_SLOTS) {
      limit = MAX_EXTRACTION_SLOTS;
   }
   return limit;
}

/* Helper: Check if user has active extraction (must hold mutex) */
static bool extraction_running_locked(int user_id) {
   for (int i = 0; i < MAX_EXTRACTION_SLOTS; i++) {
      if (s_extraction_state.user_ids[i] == user_id) {
         return true;
      }
   }
   return false;
}

/* Helper: the user's hold entry, or -1 (must hold mutex) */
static int extraction_hold_index_locked(int user_id) {
   for (int i = 0; i < MEMORY_EXTRACTION_MAX_HOLDS; i++) {
      if (s_extraction_state.held_user_ids[i] == user_id) {
         return i;
      }
   }
   return -1;
}

/* Helper: an extraction is running for the user, or it is held (must hold mutex) */
static bool extraction_is_active_locked(int user_id) {
   return extraction_running_locked(user_id) || extraction_hold_index_locked(user_id) >= 0;
}

/* Helper: Try to acquire extraction slot for user (must hold mutex)
 * Returns true if slot acquired, false if user already active or slots full */
static bool extraction_slot_acquire_locked(int user_id) {
   /* Check if user already has active extraction */
   if (extraction_is_active_locked(user_id)) {
      return false;
   }

   /* Check if we've hit the concurrency limit */
   int max_concurrent = get_max_concurrent_extractions();
   if (s_extraction_state.count >= max_concurrent) {
      return false;
   }

   /* Find empty slot and claim it */
   for (int i = 0; i < MAX_EXTRACTION_SLOTS; i++) {
      if (s_extraction_state.user_ids[i] == 0) {
         s_extraction_state.user_ids[i] = user_id;
         s_extraction_state.count++;
         return true;
      }
   }

   return false; /* Should not reach here if count is accurate */
}

/* Helper: Release extraction slot for user (must hold mutex) */
static void extraction_slot_release_locked(int user_id) {
   pthread_once(&s_extraction_cond_once, extraction_cond_init);
   for (int i = 0; i < MAX_EXTRACTION_SLOTS; i++) {
      if (s_extraction_state.user_ids[i] == user_id) {
         s_extraction_state.user_ids[i] = 0;
         s_extraction_state.count--;
         pthread_cond_broadcast(&s_extraction_cond);
         return;
      }
   }
}

/* Helper: a hold is waiting for the user (must hold mutex) */
static bool hold_waiting_locked(int user_id) {
   for (int i = 0; i < MEMORY_EXTRACTION_MAX_HOLDS; i++) {
      if (s_extraction_state.hold_waiter_ids[i] == user_id) {
         return true;
      }
   }
   return false;
}

/* Whether a queued extraction for the user can start now, claiming its slot
 * when it can (extraction_queue_take; must hold mutex). */
static bool queue_entry_ready_locked(int user_id, void *ctx) {
   (void)ctx;
   return !hold_waiting_locked(user_id) && extraction_slot_acquire_locked(user_id);
}

static void queue_dispatch(void);

/* Release the user's slot, then start what the queue holds that can now run. */
void extraction_slot_release(int user_id) {
   pthread_mutex_lock(&s_extraction_mutex);
   extraction_slot_release_locked(user_id);
   pthread_mutex_unlock(&s_extraction_mutex);
   queue_dispatch();
}

bool extraction_slot_claim_or_queue(int user_id,
                                    int64_t conversation_id,
                                    const char *session_id,
                                    const memory_extraction_fallback_t *fallback) {
   pthread_mutex_lock(&s_extraction_mutex);
   /* A forget waiting for the user goes first: this waits behind it. */
   if (!hold_waiting_locked(user_id) && extraction_slot_acquire_locked(user_id)) {
      pthread_mutex_unlock(&s_extraction_mutex);
      return true;
   }
   const bool user_active = extraction_is_active_locked(user_id) || hold_waiting_locked(user_id);
   const int running = s_extraction_state.count;
   const int max_concurrent = get_max_concurrent_extractions();
   const bool recovery = session_id && strncmp(session_id, "recovery_", 9) == 0;
   bool queued = false;
   if (conversation_id > 0 && !recovery && !atomic_load(&s_queue_stopped)) {
      extraction_queue_entry_t e = { .user_id = user_id,
                                     .conversation_id = conversation_id,
                                     .has_fallback = fallback != NULL };
      if (session_id) {
         safe_strscpy(e.session_id, session_id);
      }
      if (fallback) {
         e.fallback = *fallback;
      }
      queued = extraction_queue_push(&e);
   }
   pthread_mutex_unlock(&s_extraction_mutex);

   if (user_active) {
      OLOG_INFO("memory_extraction: already in progress for user %d%s", user_id,
                queued ? "; conversation queued" : "");
   } else {
      OLOG_INFO("memory_extraction: at capacity (%d/%d) for user %d%s", running, max_concurrent,
                user_id, queued ? "; conversation queued" : "");
   }
   if (!queued && conversation_id > 0 && !recovery) {
      OLOG_WARNING("memory_extraction: queue full; conversation %lld waits for memory recovery",
                   (long long)conversation_id);
   }
   return false;
}

void extraction_record_outcome(int user_id, bool was_transient) {
   pthread_mutex_lock(&s_extraction_mutex);
   set_last_outcome_locked(user_id, was_transient);
   pthread_mutex_unlock(&s_extraction_mutex);
}

/* A queued conversation, read again from the database (its caller's history
 * may be long gone), on the slot claimed when it was taken.  The trigger
 * checks it again: one made private, deleted or extracted meanwhile does
 * nothing. */
static void *queued_extraction_thread(void *arg) {
   extraction_queue_entry_t *e = arg;
   int message_count = 0;
   struct json_object *history = NULL;
   /* Shutdown may have stopped the queue after this was taken: touch nothing
    * (memory recovery extracts it on the next start). */
   if (atomic_load(&s_queue_stopped)) {
      extraction_slot_release(e->user_id);
      free(e);
      return NULL;
   }
#ifdef ENABLE_AUTH
   conversation_t conv;
   if (conv_db_get(e->conversation_id, e->user_id, &conv) == AUTH_DB_SUCCESS) {
      message_count = conv.message_count;
      conv_free(&conv);
   }
   if (message_count >= 2) {
      history = memory_history_load_from_db(e->conversation_id, e->user_id, NULL);
   }
#endif
   if (!history) {
      OLOG_INFO("memory_extraction: queued conversation %lld has nothing to extract",
                (long long)e->conversation_id);
      extraction_slot_release(e->user_id);
   } else {
      OLOG_INFO("memory_extraction: running queued extraction of conversation %lld for user %d",
                (long long)e->conversation_id, e->user_id);
      extraction_run_queued(e->user_id, e->conversation_id, e->session_id[0] ? e->session_id : NULL,
                            history, message_count, e->has_fallback ? &e->fallback : NULL);
      json_object_put(history);
   }
   free(e);
   return NULL;
}

/* Start every queued extraction that can run now, each on its own thread (a
 * release may come on any thread, the WebUI's included). */
static void queue_dispatch(void) {
   for (;;) {
      extraction_queue_entry_t taken;
      pthread_mutex_lock(&s_extraction_mutex);
      const bool got = !atomic_load(&s_queue_stopped) &&
                       extraction_queue_take(queue_entry_ready_locked, NULL, &taken);
      pthread_mutex_unlock(&s_extraction_mutex);
      if (!got) {
         return;
      }
      extraction_queue_entry_t *e = malloc(sizeof(*e));
      pthread_t thread;
      pthread_attr_t attr;
      pthread_attr_init(&attr);
      pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
      const bool started = e && (*e = taken,
                                 pthread_create(&thread, &attr, queued_extraction_thread, e) == 0);
      pthread_attr_destroy(&attr);
      if (!started) {
         OLOG_WARNING("memory_extraction: couldn't start queued conversation %lld; memory "
                      "recovery will",
                      (long long)taken.conversation_id);
         free(e);
         pthread_mutex_lock(&s_extraction_mutex);
         extraction_slot_release_locked(taken.user_id);
         pthread_mutex_unlock(&s_extraction_mutex);
      }
   }
}

void memory_extraction_unqueue_conversations(int user_id, const int64_t *ids, int n) {
   pthread_mutex_lock(&s_extraction_mutex);
   extraction_queue_drop_conversations(user_id, ids, n);
   pthread_mutex_unlock(&s_extraction_mutex);
}

void memory_extraction_unqueue_user(int user_id) {
   pthread_mutex_lock(&s_extraction_mutex);
   extraction_queue_drop_user(user_id);
   pthread_mutex_unlock(&s_extraction_mutex);
}

void memory_extraction_stop_queue(void) {
   atomic_store(&s_queue_stopped, true);
   pthread_mutex_lock(&s_extraction_mutex);
   const int dropped = extraction_queue_count();
   extraction_queue_clear();
   pthread_mutex_unlock(&s_extraction_mutex);
   if (dropped > 0) {
      OLOG_INFO("memory_extraction: %d queued conversation(s) left for memory recovery", dropped);
   }
}

bool memory_extraction_in_progress(int user_id) {
   if (user_id <= 0) {
      return false;
   }

   pthread_mutex_lock(&s_extraction_mutex);
   bool in_progress = extraction_is_active_locked(user_id);
   pthread_mutex_unlock(&s_extraction_mutex);

   return in_progress;
}

/* Deadline @p timeout_sec from now on the condition variable's clock. */
static struct timespec extraction_deadline(int timeout_sec) {
   struct timespec deadline;
   clock_gettime(CLOCK_MONOTONIC, &deadline);
   deadline.tv_sec += timeout_sec > 0 ? timeout_sec : 0;
   return deadline;
}

bool memory_extraction_wait_idle(int user_id, int timeout_sec) {
   if (user_id <= 0) {
      return true;
   }
   pthread_once(&s_extraction_cond_once, extraction_cond_init);
   const struct timespec deadline = extraction_deadline(timeout_sec);
   pthread_mutex_lock(&s_extraction_mutex);
   bool idle = true;
   while (extraction_running_locked(user_id)) {
      if (pthread_cond_timedwait(&s_extraction_cond, &s_extraction_mutex, &deadline) == ETIMEDOUT) {
         idle = !extraction_running_locked(user_id);
         break;
      }
   }
   pthread_mutex_unlock(&s_extraction_mutex);
   return idle;
}

bool memory_extraction_hold_user(int user_id, int timeout_sec) {
   if (user_id <= 0) {
      return false;
   }
   pthread_once(&s_extraction_cond_once, extraction_cond_init);
   const struct timespec deadline = extraction_deadline(timeout_sec);

   pthread_mutex_lock(&s_extraction_mutex);
   /* While this waits, the queue starts nothing more for the user. */
   int waiter = -1;
   for (int i = 0; i < MEMORY_EXTRACTION_MAX_HOLDS && waiter < 0; i++) {
      if (s_extraction_state.hold_waiter_ids[i] == 0) {
         s_extraction_state.hold_waiter_ids[i] = user_id;
         waiter = i;
      }
   }
   bool held = false;
   for (;;) {
      if (!extraction_is_active_locked(user_id)) {
         /* A hold makes no LLM call; it only keeps one from starting for this
          * user, so it takes an entry of its own, not an extraction slot. */
         const int free_idx = extraction_hold_index_locked(0);
         if (free_idx >= 0) {
            s_extraction_state.held_user_ids[free_idx] = user_id;
            held = true;
            break;
         }
      }
      if (pthread_cond_timedwait(&s_extraction_cond, &s_extraction_mutex, &deadline) == ETIMEDOUT) {
         break;
      }
   }
   if (waiter >= 0) {
      s_extraction_state.hold_waiter_ids[waiter] = 0;
   }
   pthread_mutex_unlock(&s_extraction_mutex);
   if (!held) {
      queue_dispatch(); /* what waited behind this hold */
   }
   return held;
}

void memory_extraction_release_user(int user_id) {
   if (user_id <= 0) {
      return;
   }
   pthread_once(&s_extraction_cond_once, extraction_cond_init);
   pthread_mutex_lock(&s_extraction_mutex);
   const int idx = extraction_hold_index_locked(user_id);
   if (idx >= 0) {
      s_extraction_state.held_user_ids[idx] = 0;
      pthread_cond_broadcast(&s_extraction_cond);
   }
   pthread_mutex_unlock(&s_extraction_mutex);
   queue_dispatch();
}

bool memory_extraction_consume_last_transient(int user_id) {
   if (user_id <= 0) {
      return false;
   }
   pthread_mutex_lock(&s_extraction_mutex);
   bool result = (s_last_outcome.last_user_id == user_id && s_last_outcome.was_transient);
   if (result) {
      /* Clear so a subsequent consume returns false — the recovery
       * worker reads once per extraction it triggered. */
      s_last_outcome.was_transient = false;
   }
   pthread_mutex_unlock(&s_extraction_mutex);
   return result;
}
