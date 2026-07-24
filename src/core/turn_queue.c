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
 * Serialized per-session turn queue.  See turn_queue.h.
 */

#include "core/turn_queue.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>

#include "logging.h"

/* Per-source queue bounds.  User turns reject-newest at the cap (never silently
 * dropped — the caller surfaces an error); background turns get a smaller cap and
 * are expected to coalesce upstream. */
#define TURN_QUEUE_USER_CAP 16
#define TURN_QUEUE_BG_CAP 8

/* Concurrent sessions with a queue are few (MAX_SESSIONS interactive + a handful
 * of job sessions); a fixed table with linear scan matches the codebase style
 * (cf. job_reinvoke s_inflight). */
#define TURN_QUEUE_MAX_SESSIONS 32

typedef struct pending_turn {
   void *work;
   void (*spawn)(void *work);
   void (*free_work)(void *work);
   struct pending_turn *next;
} pending_turn_t;

typedef struct {
   uint32_t session_id; /* 0 = free slot */
   pending_turn_t *head;
   pending_turn_t *tail;
   int count;
   bool in_flight; /* a turn worker is currently running for this session */
   bool closing;   /* session teardown began — refuse new turns, don't chain */
} session_turn_q_t;

static session_turn_q_t s_queues[TURN_QUEUE_MAX_SESSIONS];
static pthread_mutex_t s_turn_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Find the queue for @session_id, or allocate a free slot (caller holds lock).
 * @allocate=false returns NULL when absent. */
static session_turn_q_t *find_queue(uint32_t session_id, bool allocate) {
   session_turn_q_t *freeslot = NULL;
   for (int i = 0; i < TURN_QUEUE_MAX_SESSIONS; i++) {
      if (s_queues[i].session_id == session_id && session_id != 0) {
         return &s_queues[i];
      }
      if (freeslot == NULL && s_queues[i].session_id == 0) {
         freeslot = &s_queues[i];
      }
   }
   if (allocate && freeslot != NULL) {
      freeslot->session_id = session_id;
      freeslot->head = NULL;
      freeslot->tail = NULL;
      freeslot->count = 0;
      freeslot->in_flight = false;
      freeslot->closing = false;
      return freeslot;
   }
   return NULL;
}

/* Detach the head pending turn (caller holds lock).  Returns NULL if empty. */
static pending_turn_t *pop_head(session_turn_q_t *q) {
   pending_turn_t *p = q->head;
   if (p == NULL) {
      return NULL;
   }
   q->head = p->next;
   if (q->head == NULL) {
      q->tail = NULL;
   }
   q->count--;
   p->next = NULL;
   return p;
}

int turn_queue_enqueue(uint32_t session_id,
                       turn_source_t source,
                       void *work,
                       void (*spawn)(void *work),
                       void (*free_work)(void *work)) {
   if (session_id == 0 || work == NULL || spawn == NULL) {
      return TURN_QUEUE_FAIL;
   }

   pending_turn_t *node = calloc(1, sizeof(*node));
   if (node == NULL) {
      return TURN_QUEUE_FAIL;
   }
   node->work = work;
   node->spawn = spawn;
   node->free_work = free_work;

   pthread_mutex_lock(&s_turn_queue_mutex);
   session_turn_q_t *q = find_queue(session_id, /*allocate=*/true);
   if (q == NULL) {
      pthread_mutex_unlock(&s_turn_queue_mutex);
      free(node);
      OLOG_ERROR("turn_queue: no free slot for session %u", session_id);
      return TURN_QUEUE_FAIL;
   }

   /* Session is tearing down — refuse (the caller frees @work).  Prevents a new
    * turn from racing in after purge dropped the queue but before the session is
    * freed. */
   if (q->closing) {
      pthread_mutex_unlock(&s_turn_queue_mutex);
      free(node);
      OLOG_INFO("turn_queue: session %u is closing — rejecting turn", session_id);
      return TURN_QUEUE_FAIL;
   }

   int cap = (source == TURN_SOURCE_USER) ? TURN_QUEUE_USER_CAP : TURN_QUEUE_BG_CAP;
   if (q->count >= cap) {
      pthread_mutex_unlock(&s_turn_queue_mutex);
      free(node); /* caller still owns @work (see contract) */
      OLOG_WARNING("turn_queue: session %u at %s cap (%d) — rejecting turn", session_id,
                   source == TURN_SOURCE_USER ? "user" : "background", cap);
      return TURN_QUEUE_FULL;
   }

   /* Append. */
   if (q->tail != NULL) {
      q->tail->next = node;
   } else {
      q->head = node;
   }
   q->tail = node;
   q->count++;

   /* If no worker is running for this session, claim the gate and take the head
    * to spawn OUTSIDE the lock. */
   pending_turn_t *to_spawn = NULL;
   if (!q->in_flight) {
      q->in_flight = true;
      to_spawn = pop_head(q);
   }
   pthread_mutex_unlock(&s_turn_queue_mutex);

   if (to_spawn != NULL) {
      to_spawn->spawn(to_spawn->work);
      free(to_spawn);
   }
   return TURN_QUEUE_OK;
}

void turn_queue_turn_done(uint32_t session_id) {
   if (session_id == 0) {
      return;
   }
   pthread_mutex_lock(&s_turn_queue_mutex);
   session_turn_q_t *q = find_queue(session_id, /*allocate=*/false);
   pending_turn_t *next = NULL;
   pending_turn_t *drop = NULL; /* turns to free WITHOUT running (closing) */
   if (q != NULL) {
      if (q->closing) {
         /* Session is tearing down: don't chain a successor.  Drop any stragglers
          * (a turn could have raced in before `closing` was observed) and retire
          * the slot so it can be reused. */
         drop = q->head;
         q->head = NULL;
         q->tail = NULL;
         q->count = 0;
         q->in_flight = false;
         q->session_id = 0; /* retire */
         q->closing = false;
      } else {
         next = pop_head(q);
         if (next == NULL) {
            /* Nothing left — release the gate AND retire the slot (H1: otherwise a
             * slot bound to a since-freed session id would leak, since ids never
             * reuse).  The next turn for a live session re-allocates a slot. */
            q->in_flight = false;
            q->session_id = 0;
         }
         /* in_flight stays true across the chain when next != NULL */
      }
   }
   pthread_mutex_unlock(&s_turn_queue_mutex);

   if (next != NULL) {
      next->spawn(next->work);
      free(next);
   }
   while (drop != NULL) {
      pending_turn_t *nx = drop->next;
      if (drop->free_work != NULL) {
         drop->free_work(drop->work);
      }
      free(drop);
      drop = nx;
   }
}

void turn_queue_purge_session(uint32_t session_id) {
   if (session_id == 0) {
      return;
   }
   pending_turn_t *dropped = NULL; /* singly-linked list to free outside the lock */
   pthread_mutex_lock(&s_turn_queue_mutex);
   session_turn_q_t *q = find_queue(session_id, /*allocate=*/false);
   if (q != NULL) {
      pending_turn_t *p = q->head;
      q->head = NULL;
      q->tail = NULL;
      q->count = 0;
      /* Mark closing: refuse any new enqueue and stop the in-flight worker's
       * turn_done from chaining a successor (it retires the slot instead). */
      q->closing = true;
      /* Leave in_flight as-is: the running worker (if any) finishes on its own
       * (teardown's cancel flag aborts it promptly), then its turn_done retires
       * the slot. */
      dropped = p;
      /* If nothing is running, retire the slot immediately so it can be reused. */
      if (!q->in_flight) {
         q->session_id = 0;
         q->closing = false;
      }
   }
   pthread_mutex_unlock(&s_turn_queue_mutex);

   int n = 0;
   while (dropped != NULL) {
      pending_turn_t *nx = dropped->next;
      if (dropped->free_work != NULL) {
         dropped->free_work(dropped->work);
      }
      free(dropped);
      dropped = nx;
      n++;
   }
   if (n > 0) {
      OLOG_INFO("turn_queue: purged %d queued turn(s) for session %u", n, session_id);
   }
}
