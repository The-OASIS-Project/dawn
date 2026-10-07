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
 * The WebUI's email executor (see webui_email_exec.h).
 *
 * Locks, in order: s_mutex (run queue, sessions' slots, workers' users) → a
 * join's deliver_mutex → the session registry and a session's metrics_mutex
 * (opening a session to reply happens before the deliver_mutex, the send
 * itself only queues).  s_mutex is taken before the lease mutex (a worker asks
 * for a lease, a cancel withdraws a ticket) and never while releasing a lease:
 * a release may call the hook, which takes s_mutex.
 *
 * A join lives while it has references: one for its tasks together (dropped
 * when the last task is done) and one for each canceller, which finds the join
 * in its session's slot under s_mutex and so can't see it freed.  free_ctx takes
 * no locks, so freeing is safe with or without s_mutex held.
 */

#include "webui/webui_email_exec.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"
#include "tools/email_account_lease.h"
#include "tools/email_transfer.h"

/* Sessions that can have email work at once. */
#define EMAIL_EXEC_SESSIONS 64
/* Joins one session holds at most, and one stop may cancel. */
#define EMAIL_EXEC_SESSION_JOINS (EMAIL_EXEC_SLOT_COUNT * EMAIL_EXEC_SLOT_JOINS_MAX)
#define EMAIL_EXEC_CANCEL_MAX (EMAIL_EXEC_SESSIONS * EMAIL_EXEC_SESSION_JOINS)

typedef enum {
   TASK_QUEUED = 0, /* in the run queue */
   TASK_PARKED,     /* its ticket waits in the lease */
   TASK_RUNNING,
} task_state_t;

struct exec_join;

typedef struct exec_task {
   email_lease_ticket_t ticket; /* owner = this task */
   struct exec_join *join;
   struct exec_task *next; /* run queue */
   int64_t account_id;
   int index;
   bool is_imap;
   bool has_lease;
   email_err_t lease_err;
   task_state_t state;
} exec_task_t;

/* A user's cancelled joins still running (they count toward the user's cap). */
typedef struct {
   int user_id;
   atomic_int count;
} exec_drain_t;

typedef struct exec_join {
   atomic_int remaining; /* tasks not done */
   atomic_int refs;      /* the tasks' reference + one per canceller */
   atomic_bool cancel;
   pthread_mutex_t deliver_mutex;
   bool delivered;      /* its result was sent (under deliver_mutex) */
   bool started;        /* its tasks went into the run queue */
   bool ran;            /* a task of it began its work (under s_mutex) */
   exec_drain_t *drain; /* set when cancelled with tasks still running */
   uint32_t session_id;
   int user_id;
   email_exec_slot_t slot;
   char verb[EMAIL_EXEC_VERB_MAX];
   char req[EMAIL_EXEC_REQ_MAX + 1];
   bool has_req;
   email_exec_op_fn op;
   email_exec_finish_fn finish;
   email_exec_free_fn free_ctx;
   void *ctx;
   int task_count;
   exec_task_t tasks[];
} exec_join_t;

typedef struct {
   bool in_use;
   uint32_t session_id;
   int user_id;
   exec_join_t *running[EMAIL_EXEC_SLOT_COUNT];
   /* Waiting, oldest first: one at most, except moves (EMAIL_EXEC_MOVE_QUEUE). */
   exec_join_t *waiting[EMAIL_EXEC_SLOT_COUNT][EMAIL_EXEC_MOVE_QUEUE];
   int nwait[EMAIL_EXEC_SLOT_COUNT];
} exec_session_t;

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static pthread_t s_workers[EMAIL_EXEC_WORKERS];
static int s_worker_user[EMAIL_EXEC_WORKERS]; /* the user a worker runs a task for, or -1 */
static int s_worker_count;
static bool s_started;
static bool s_stopped;
static exec_task_t *s_head, *s_tail;
static exec_session_t s_sessions[EMAIL_EXEC_SESSIONS];
static exec_drain_t s_drain[EMAIL_EXEC_SESSIONS];

/* =============================================================================
 * Replies
 * ============================================================================= */

static const char *refusal_message(email_err_t code) {
   switch (code) {
      case EMAIL_ERR_SUPERSEDED:
         return "Replaced by a newer request";
      case EMAIL_ERR_SHUTTING_DOWN:
         return "Shutting down";
      case EMAIL_ERR_BUSY:
         return "Too many email requests at once";
      default:
         return "Email work couldn't start";
   }
}

/* Wraps @p payload (taken) as the verb's reply frame; heap JSON or NULL. */
static char *frame_json(const char *verb, json_object *payload, const char *req) {
   json_object *frame = json_object_new_object();
   if (!frame || !payload) {
      json_object_put(frame);
      json_object_put(payload);
      return NULL;
   }
   char type[EMAIL_EXEC_VERB_MAX + 16];
   snprintf(type, sizeof(type), "%s_response", verb ? verb : "email");
   json_object *ignored;
   if (!json_object_object_get_ex(payload, "success", &ignored))
      json_object_object_add(payload, "success", json_object_new_boolean(1));
   if (req)
      json_object_object_add(payload, "req", json_object_new_string(req));
   json_object_object_add(frame, "type", json_object_new_string(type));
   json_object_object_add(frame, "payload", payload);
   char *out = strdup(json_object_to_json_string_ext(frame, JSON_C_TO_STRING_PLAIN |
                                                                JSON_C_TO_STRING_NOSLASHESCAPE));
   json_object_put(frame);
   return out;
}

char *webui_email_exec_refusal_json(const char *verb, email_err_t code, const char *req) {
   json_object *payload = json_object_new_object();
   if (!payload)
      return NULL;
   json_object_object_add(payload, "success", json_object_new_boolean(0));
   json_object_object_add(payload, "error_code", json_object_new_string(email_error_name(code)));
   json_object_object_add(payload, "error", json_object_new_string(refusal_message(code)));
   return frame_json(verb, payload, req);
}

bool email_exec_payload_req(json_object *payload, char *out, size_t out_size) {
   json_object *req_obj;
   if (!payload || !out || out_size == 0 || !json_object_object_get_ex(payload, "req", &req_obj) ||
       !json_object_is_type(req_obj, json_type_string))
      return false;
   const char *req = json_object_get_string(req_obj);
   const size_t len = req ? strlen(req) : 0;
   if (!req || len > EMAIL_EXEC_REQ_MAX || len >= out_size)
      return false;
   for (size_t i = 0; i < len; i++) {
      const unsigned char ch = (unsigned char)req[i];
      if (ch < 0x20 || ch == 0x7f)
         return false; /* echoed in every reply: keep it plain */
   }
   memcpy(out, req, len + 1);
   return true;
}

static void send_refusal(uint32_t session_id,
                         int user_id,
                         const char *verb,
                         email_err_t code,
                         const char *req) {
   char *json = webui_email_exec_refusal_json(verb, code, req);
   if (!json)
      return;
   void *session = webui_email_exec_session_open(session_id, user_id);
   if (session) {
      webui_email_exec_session_send(session, json);
      webui_email_exec_session_close(session);
   } else {
      free(json);
   }
}

/* =============================================================================
 * Joins and sessions (callers hold s_mutex unless noted)
 * ============================================================================= */

static exec_session_t *session_find_locked(uint32_t session_id) {
   for (int i = 0; i < EMAIL_EXEC_SESSIONS; i++) {
      if (s_sessions[i].in_use && s_sessions[i].session_id == session_id)
         return &s_sessions[i];
   }
   return NULL;
}

static exec_session_t *session_get_locked(uint32_t session_id, int user_id) {
   exec_session_t *s = session_find_locked(session_id);
   if (s)
      return s;
   for (int i = 0; i < EMAIL_EXEC_SESSIONS; i++) {
      if (!s_sessions[i].in_use) {
         memset(&s_sessions[i], 0, sizeof(s_sessions[i]));
         s_sessions[i].in_use = true;
         s_sessions[i].session_id = session_id;
         s_sessions[i].user_id = user_id;
         return &s_sessions[i];
      }
   }
   return NULL;
}

static void session_free_if_idle_locked(exec_session_t *s) {
   for (int i = 0; i < EMAIL_EXEC_SLOT_COUNT; i++) {
      if (s->running[i] || s->nwait[i] > 0)
         return;
   }
   s->in_use = false;
}

static void waiting_push_locked(exec_session_t *s, email_exec_slot_t k, exec_join_t *j) {
   s->waiting[k][s->nwait[k]++] = j; /* the policy keeps it under EMAIL_EXEC_MOVE_QUEUE */
}

/* The oldest waiting request of the slot, taken out; NULL when none. */
static exec_join_t *waiting_pop_locked(exec_session_t *s, email_exec_slot_t k) {
   if (s->nwait[k] == 0)
      return NULL;
   exec_join_t *j = s->waiting[k][0];
   for (int i = 1; i < s->nwait[k]; i++)
      s->waiting[k][i - 1] = s->waiting[k][i];
   s->waiting[k][--s->nwait[k]] = NULL;
   return j;
}

/* Every request of the slot (running first, then waiting in order), taken out of
 * it into @p out; returns how many. */
static int slot_take_all_locked(exec_session_t *s, email_exec_slot_t k, exec_join_t **out) {
   int n = 0;
   if (s->running[k])
      out[n++] = s->running[k];
   s->running[k] = NULL;
   exec_join_t *w;
   while ((w = waiting_pop_locked(s, k)))
      out[n++] = w;
   return n;
}

/* A user's entry for cancelled joins still running; NULL when the table is full. */
static exec_drain_t *drain_get_locked(int user_id) {
   exec_drain_t *free_entry = NULL;
   for (int i = 0; i < EMAIL_EXEC_SESSIONS; i++) {
      const int count = atomic_load(&s_drain[i].count);
      if (count > 0 && s_drain[i].user_id == user_id)
         return &s_drain[i];
      if (count == 0 && !free_entry)
         free_entry = &s_drain[i];
   }
   if (free_entry)
      free_entry->user_id = user_id; /* count 0: no join refers to it */
   return free_entry;
}

/* The user's requests in slots, plus their cancelled ones still running. */
static int user_live_locked(int user_id) {
   int n = 0;
   for (int i = 0; i < EMAIL_EXEC_SESSIONS; i++) {
      const exec_session_t *s = &s_sessions[i];
      if (s->in_use && s->user_id == user_id) {
         for (int k = 0; k < EMAIL_EXEC_SLOT_COUNT; k++)
            n += (s->running[k] ? 1 : 0) + (k == EMAIL_EXEC_SLOT_MOVE ? 0 : s->nwait[k]);
      }
      const int draining = atomic_load(&s_drain[i].count);
      if (draining > 0 && s_drain[i].user_id == user_id)
         n += draining;
   }
   return n;
}

static void queue_push_locked(exec_task_t *t) {
   t->state = TASK_QUEUED;
   t->next = NULL;
   if (s_tail)
      s_tail->next = t;
   else
      s_head = t;
   s_tail = t;
   pthread_cond_signal(&s_cond);
}

static void queue_unlink_locked(exec_task_t *prev, exec_task_t *cur) {
   if (prev)
      prev->next = cur->next;
   else
      s_head = cur->next;
   if (s_tail == cur)
      s_tail = prev;
   cur->next = NULL;
}

static bool queue_remove_locked(exec_task_t *t) {
   exec_task_t *prev = NULL;
   for (exec_task_t *cur = s_head; cur; prev = cur, cur = cur->next) {
      if (cur == t) {
         queue_unlink_locked(prev, cur);
         return true;
      }
   }
   return false;
}

static int user_workers_locked(int user_id) {
   int n = 0;
   for (int i = 0; i < EMAIL_EXEC_WORKERS; i++)
      n += s_worker_user[i] == user_id ? 1 : 0;
   return n;
}

/* The next task to run: the oldest whose user isn't already on
 * EMAIL_EXEC_USER_WORKERS workers, so one user's slow servers can't hold every
 * worker while others wait; when only such users have work, the oldest. */
static exec_task_t *queue_take_locked(void) {
   exec_task_t *prev = NULL;
   /* A task already handed its account's lease goes first, past the per-user
    * share: while it waits, the account stays idle for everyone else. */
   for (exec_task_t *cur = s_head; cur; prev = cur, cur = cur->next) {
      if (cur->has_lease) {
         queue_unlink_locked(prev, cur);
         return cur;
      }
   }
   prev = NULL;
   for (exec_task_t *cur = s_head; cur; prev = cur, cur = cur->next) {
      if (user_workers_locked(cur->join->user_id) < EMAIL_EXEC_USER_WORKERS) {
         queue_unlink_locked(prev, cur);
         return cur;
      }
   }
   exec_task_t *t = s_head;
   if (t)
      queue_unlink_locked(NULL, t);
   return t;
}

static void join_start_locked(exec_join_t *j) {
   j->started = true;
   for (int i = 0; i < j->task_count; i++)
      queue_push_locked(&j->tasks[i]);
}

static void join_free(exec_join_t *j) {
   if (j->free_ctx)
      j->free_ctx(j->ctx);
   if (j->drain)
      atomic_fetch_sub(&j->drain->count, 1);
   pthread_mutex_destroy(&j->deliver_mutex);
   free(j);
}

static void join_put(exec_join_t *j) {
   if (atomic_fetch_sub(&j->refs, 1) == 1)
      join_free(j);
}

/* A task is done (ran, or was withdrawn); the last one finishes the join. */
static void task_done(exec_task_t *t);

/*
 * Called with s_mutex held, after the caller took @p j out of its session's
 * slot.  Takes a reference (the caller drops it with join_put, after unlocking
 * s_mutex), sets the cancel under the join's delivery lock (so its result can't
 * be sent from then on), and withdraws its queued tasks and its tickets still
 * waiting in the lease.  A ticket the lease already handed over is the hook's:
 * its task comes back through the queue, sees the cancel, releases the lease
 * and is done.
 * @return true if its result was already sent (then it isn't answered again)
 */
static bool join_cancel_locked(exec_join_t *j) {
   atomic_fetch_add(&j->refs, 1);
   pthread_mutex_lock(&j->deliver_mutex);
   const bool delivered = j->delivered;
   atomic_store(&j->cancel, true);
   pthread_mutex_unlock(&j->deliver_mutex);

   if (!j->started) {
      /* Never queued: nothing of it runs.  Drop the tasks' reference. */
      atomic_store(&j->remaining, 0);
      atomic_fetch_sub(&j->refs, 1);
      return false;
   }
   exec_task_t *withdrawn[EMAIL_EXEC_MAX_TASKS];
   int n = 0;
   for (int i = 0; i < j->task_count; i++) {
      exec_task_t *t = &j->tasks[i];
      if ((t->state == TASK_QUEUED && !t->has_lease && queue_remove_locked(t)) ||
          (t->state == TASK_PARKED && email_lease_cancel_ticket(&t->ticket)))
         withdrawn[n++] = t;
      /* RUNNING, or handed a lease: it comes back through task_done itself. */
   }
   for (int i = 0; i < n; i++)
      task_done(withdrawn[i]); /* the caller's reference keeps the join */
   if (atomic_load(&j->remaining) > 0) {
      j->drain = drain_get_locked(j->user_id);
      if (j->drain)
         atomic_fetch_add(&j->drain->count, 1);
   }
   return delivered;
}

/* =============================================================================
 * Completion and delivery
 * ============================================================================= */

/* Sends @p json (taken) unless the join was cancelled. */
static void deliver(exec_join_t *j, char *json) {
   if (!json)
      return;
   void *session = webui_email_exec_session_open(j->session_id, j->user_id);
   if (session) {
      pthread_mutex_lock(&j->deliver_mutex);
      if (!atomic_load(&j->cancel)) {
         webui_email_exec_session_send(session, json);
         json = NULL;
         j->delivered = true;
      }
      pthread_mutex_unlock(&j->deliver_mutex);
      webui_email_exec_session_close(session);
   }
   free(json);
}

static void join_tasks_done(exec_join_t *j) {
   if (!atomic_load(&j->cancel)) {
      int64_t ids[EMAIL_EXEC_MAX_TASKS];
      for (int i = 0; i < j->task_count; i++)
         ids[i] = j->tasks[i].account_id;
      json_object *payload = j->finish ? j->finish(j->ctx, ids, j->task_count) : NULL;
      deliver(j, payload ? frame_json(j->verb, payload, j->has_req ? j->req : NULL) : NULL);

      /* Give up the slot; a read waiting behind this one starts.  A canceller
       * that took it out of the slot first holds its own reference. */
      pthread_mutex_lock(&s_mutex);
      exec_session_t *s = session_find_locked(j->session_id);
      if (s && s->running[j->slot] == j) {
         s->running[j->slot] = NULL;
         exec_join_t *next = waiting_pop_locked(s, j->slot);
         if (next) {
            s->running[j->slot] = next;
            if (!s_stopped)
               join_start_locked(next); /* when stopping, the stop cancels it unstarted */
         }
         session_free_if_idle_locked(s);
      }
      pthread_mutex_unlock(&s_mutex);
   }
   join_put(j);
}

static void task_done(exec_task_t *t) {
   exec_join_t *j = t->join;
   if (atomic_fetch_sub(&j->remaining, 1) == 1)
      join_tasks_done(j);
}

/* =============================================================================
 * Workers
 * ============================================================================= */

/* The lease handed a parked task its account: queue it, holding the lease.
 * Must not touch the ticket after queuing (the task may run and release it). */
static void lease_hook(email_lease_ticket_t *ticket) {
   exec_task_t *t = (exec_task_t *)ticket->owner;
   pthread_mutex_lock(&s_mutex);
   t->has_lease = true;
   queue_push_locked(t);
   pthread_mutex_unlock(&s_mutex);
}

static void run_task(exec_task_t *t) {
   exec_join_t *j = t->join;
   const email_target_t target = { .account_id = t->account_id, .lease_held = t->has_lease };
   const email_exec_task_ctx_t ctx = {
      .user_id = j->user_id,
      .account_id = t->account_id,
      .target = &target,
      .cancel = &j->cancel,
      .lease_err = t->lease_err,
      .ctx = j->ctx,
      .index = t->index,
   };
   const atomic_bool *prev = email_transfer_scope_cancel(&j->cancel);
   j->op(&ctx);
   email_transfer_scope_cancel(prev);
}

static void *worker_main(void *arg) {
   const int me = (int)(intptr_t)arg;
   pthread_mutex_lock(&s_mutex);
   for (;;) {
      while (!s_head && !s_stopped)
         pthread_cond_wait(&s_cond, &s_mutex);
      exec_task_t *t = queue_take_locked();
      if (!t)
         break; /* stopped and drained */
      exec_join_t *j = t->join;

      if (!atomic_load(&j->cancel) && t->is_imap && !t->has_lease) {
         bool got = false;
         const int rc = email_lease_acquire_async(t->account_id, &t->ticket, &got);
         if (rc == EMAIL_LEASE_OK && !got) {
            t->state = TASK_PARKED; /* the hook queues it again, holding the lease */
            continue;
         }
         if (rc == EMAIL_LEASE_OK)
            t->has_lease = true;
         else
            t->lease_err = EMAIL_ERR_BUSY; /* the lease table is full */
      }
      t->state = TASK_RUNNING;
      /* Decided once, under s_mutex: a stop that finds `ran` set leaves this
       * join to the work it will do, so it must then do it. */
      const bool run = !atomic_load(&j->cancel);
      if (run)
         j->ran = true;
      s_worker_user[me] = j->user_id;
      pthread_mutex_unlock(&s_mutex);

      if (run)
         run_task(t);
      if (t->has_lease) {
         t->has_lease = false;
         email_lease_release(t->account_id); /* may call the hook: no s_mutex here */
      }

      pthread_mutex_lock(&s_mutex);
      s_worker_user[me] = -1;
      pthread_mutex_unlock(&s_mutex);
      task_done(t); /* t may be freed */
      pthread_mutex_lock(&s_mutex);
   }
   pthread_mutex_unlock(&s_mutex);
   return NULL;
}

/* With s_mutex held: the workers wait on it until it's released. */
static void start_workers_locked(void) {
   s_started = true;
   email_lease_set_hook(lease_hook);
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setstacksize(&attr, EMAIL_EXEC_STACK_BYTES);
   for (int i = 0; i < EMAIL_EXEC_WORKERS; i++)
      s_worker_user[i] = -1; /* slots of workers that fail to start stay unused */
   for (int i = 0; i < EMAIL_EXEC_WORKERS; i++) {
      if (pthread_create(&s_workers[s_worker_count], &attr, worker_main,
                         (void *)(intptr_t)s_worker_count) == 0)
         s_worker_count++;
      else
         OLOG_ERROR("email exec: failed to start worker %d", i);
   }
   pthread_attr_destroy(&attr);
   OLOG_INFO("email exec: %d workers", s_worker_count);
}

/* =============================================================================
 * Public
 * ============================================================================= */

typedef struct {
   uint32_t session_id;
   int user_id;
   char verb[EMAIL_EXEC_VERB_MAX];
   char req[EMAIL_EXEC_REQ_MAX + 1];
   bool has_req;
   bool answer;
} exec_notice_t;

static exec_notice_t notice_of(const exec_join_t *j) {
   exec_notice_t n = { .session_id = j->session_id, .user_id = j->user_id, .has_req = j->has_req };
   memcpy(n.verb, j->verb, sizeof(n.verb));
   memcpy(n.req, j->req, sizeof(n.req));
   return n;
}

int webui_email_exec_submit(const email_exec_request_t *r) {
   if (!r || !r->op || r->task_count < 1 || r->task_count > EMAIL_EXEC_MAX_TASKS ||
       !r->account_ids || !r->is_imap || (unsigned)r->slot >= EMAIL_EXEC_SLOT_COUNT) {
      if (r && r->free_ctx)
         r->free_ctx(r->ctx);
      return EMAIL_EXEC_FAILURE;
   }

   exec_join_t *j = calloc(1, sizeof(*j) + (size_t)r->task_count * sizeof(exec_task_t));
   if (!j) {
      if (r->free_ctx)
         r->free_ctx(r->ctx);
      return EMAIL_EXEC_FAILURE;
   }
   atomic_init(&j->remaining, r->task_count);
   atomic_init(&j->refs, 1);
   atomic_init(&j->cancel, false);
   pthread_mutex_init(&j->deliver_mutex, NULL);
   j->session_id = r->session_id;
   j->user_id = r->user_id;
   j->slot = r->slot;
   snprintf(j->verb, sizeof(j->verb), "%s", r->verb ? r->verb : "email");
   if (r->req) {
      snprintf(j->req, sizeof(j->req), "%s", r->req);
      j->has_req = true;
   }
   j->op = r->op;
   j->finish = r->finish;
   j->free_ctx = r->free_ctx;
   j->ctx = r->ctx;
   j->task_count = r->task_count;
   for (int i = 0; i < r->task_count; i++) {
      exec_task_t *t = &j->tasks[i];
      t->join = j;
      t->ticket.owner = t;
      t->account_id = r->account_ids[i];
      t->index = i;
      t->is_imap = r->is_imap[i];
   }
   const char *req = j->has_req ? j->req : NULL;

   exec_join_t *cancelled[EMAIL_EXEC_SESSION_JOINS + 2];
   exec_notice_t notice[EMAIL_EXEC_SESSION_JOINS + 2];
   int n_cancelled = 0;

   pthread_mutex_lock(&s_mutex);
   email_err_t refuse = EMAIL_ERR_NONE;
   exec_session_t *s = NULL;
   if (s_stopped) {
      refuse = EMAIL_ERR_SHUTTING_DOWN;
   } else {
      if (!s_started)
         start_workers_locked();
      if (s_worker_count == 0)
         refuse = EMAIL_ERR_FAILED;
      else if (!(s = session_get_locked(r->session_id, r->user_id)))
         refuse = EMAIL_ERR_BUSY;
   }

   /* A session that's now another user's (log out, log in): its old work goes,
    * unanswered, since the tab is someone else's. */
   if (s && s->user_id != r->user_id) {
      for (int k = 0; k < EMAIL_EXEC_SLOT_COUNT; k++) {
         exec_join_t *old[EMAIL_EXEC_SLOT_JOINS_MAX];
         const int n_old = slot_take_all_locked(s, (email_exec_slot_t)k, old);
         for (int m = 0; m < n_old; m++) {
            notice[n_cancelled] = notice_of(old[m]);
            notice[n_cancelled].answer = false;
            join_cancel_locked(old[m]);
            cancelled[n_cancelled++] = old[m];
         }
      }
      s->user_id = r->user_id;
   }

   if (refuse == EMAIL_ERR_NONE) {
      const email_exec_admit_t a = email_exec_admit(r->slot, s->running[r->slot] != NULL,
                                                    s->nwait[r->slot],
                                                    user_live_locked(r->user_id));
      if (a.action == EMAIL_EXEC_REFUSE) {
         refuse = EMAIL_ERR_BUSY;
      } else {
         exec_join_t *superseded[2] = { NULL, NULL };
         if (a.replace_waiting && s->nwait[r->slot] > 0)
            superseded[0] = waiting_pop_locked(s, r->slot); /* only moves wait more than one */
         if (a.cancel_running && s->running[r->slot]) {
            superseded[1] = s->running[r->slot];
            s->running[r->slot] = NULL;
         }
         for (int m = 0; m < 2; m++) {
            if (!superseded[m])
               continue;
            /* The reply needs its req: copied before the cancel. */
            notice[n_cancelled] = notice_of(superseded[m]);
            notice[n_cancelled].answer = !join_cancel_locked(superseded[m]);
            cancelled[n_cancelled++] = superseded[m];
         }
         if (a.action == EMAIL_EXEC_WAIT) {
            waiting_push_locked(s, r->slot, j);
         } else {
            s->running[r->slot] = j;
            join_start_locked(j);
         }
      }
   }
   if (s && refuse != EMAIL_ERR_NONE)
      session_free_if_idle_locked(s);
   pthread_mutex_unlock(&s_mutex);

   for (int i = 0; i < n_cancelled; i++) {
      if (notice[i].answer)
         send_refusal(notice[i].session_id, notice[i].user_id, notice[i].verb, EMAIL_ERR_SUPERSEDED,
                      notice[i].has_req ? notice[i].req : NULL);
      join_put(cancelled[i]);
   }

   if (refuse != EMAIL_ERR_NONE) {
      send_refusal(r->session_id, r->user_id, j->verb, refuse, req);
      join_put(j);
      return refuse == EMAIL_ERR_SHUTTING_DOWN ? EMAIL_EXEC_SHUTTING_DOWN
             : refuse == EMAIL_ERR_BUSY        ? EMAIL_EXEC_BUSY
                                               : EMAIL_EXEC_NO_WORKERS;
   }
   return EMAIL_EXEC_OK;
}

void webui_email_exec_stop(void) {
   pthread_mutex_lock(&s_mutex);
   s_stopped = true; /* new work is refused */
   const bool started = s_started;
   pthread_mutex_unlock(&s_mutex);
   if (!started)
      return; /* no worker, no hook, no session ever */

   /* No lease is handed to a ticket from here on, and no hook call is still
    * running (it would need s_mutex, so this can't be held here). */
   email_lease_clear_hook();

   /* What's still running or waiting is answered SHUTTING_DOWN, like a new
    * request would be, so the client isn't left waiting.  A move that started
    * isn't: it stops only between folders, and what it did reaches every tab as
    * email_changed, so "shutting down" would be wrong. */
   exec_notice_t *notices = calloc(EMAIL_EXEC_CANCEL_MAX, sizeof(*notices));
   exec_join_t **cancelled = calloc(EMAIL_EXEC_CANCEL_MAX, sizeof(*cancelled));
   int n = 0;

   pthread_mutex_lock(&s_mutex);
   for (int i = 0; i < EMAIL_EXEC_SESSIONS; i++) {
      exec_session_t *s = &s_sessions[i];
      if (!s->in_use)
         continue;
      for (int k = 0; k < EMAIL_EXEC_SLOT_COUNT; k++) {
         exec_join_t *old[EMAIL_EXEC_SLOT_JOINS_MAX];
         const int n_old = slot_take_all_locked(s, (email_exec_slot_t)k, old);
         for (int m = 0; m < n_old; m++) {
            const exec_notice_t note = notice_of(old[m]);
            /* A move whose work began: what it did reaches the tabs as email_changed. */
            const bool started_move = k == EMAIL_EXEC_SLOT_MOVE && old[m]->ran;
            const bool answer = !join_cancel_locked(old[m]) && !started_move;
            if (notices && cancelled) {
               notices[n] = note;
               notices[n].answer = answer;
               cancelled[n++] = old[m];
            } else {
               join_put(old[m]); /* out of memory at shutdown: unanswered */
            }
         }
      }
      s->in_use = false;
   }
   pthread_cond_broadcast(&s_cond);
   pthread_mutex_unlock(&s_mutex);

   for (int i = 0; i < n; i++) {
      if (notices[i].answer)
         send_refusal(notices[i].session_id, notices[i].user_id, notices[i].verb,
                      EMAIL_ERR_SHUTTING_DOWN, notices[i].has_req ? notices[i].req : NULL);
      join_put(cancelled[i]);
   }
   free(notices);
   free(cancelled);

   for (int i = 0; i < s_worker_count; i++)
      pthread_join(s_workers[i], NULL);
   s_worker_count = 0;
}
