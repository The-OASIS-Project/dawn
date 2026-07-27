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
 * Unit tests for the background-job session pool (src/core/job_manager.c):
 * running caps (global / per-provider / per-user), the resolver hook, the
 * ownership-checked cancel path, and counter accounting across begin/end.
 *
 * Links against a lightweight session-manager stub (below) so the test doesn't
 * pull in the whole daemon — job_manager only touches a handful of session_*
 * entry points, all stubbed here.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/job_manager.h"
#include "core/scheduler.h"
#include "dawn_error.h"
#include "unity.h"

/* Config global that job_manager reads (defined here for the test). */
dawn_config_t g_config;

/* --- session-manager stub -------------------------------------------------- */

static session_job_lookup_fn g_registered_lookup = NULL;
static _Atomic uint32_t g_stub_next_id = 100;

void session_manager_register_job_lookup(session_job_lookup_fn fn) {
   g_registered_lookup = fn;
}

session_t *session_manager_alloc_bare(void) {
   session_t *s = calloc(1, sizeof(session_t));
   if (!s) {
      return NULL;
   }
   pthread_mutex_init(&s->ref_mutex, NULL);
   pthread_cond_init(&s->ref_zero_cond, NULL);
   s->session_id = atomic_fetch_add(&g_stub_next_id, 1);
   s->ref_count = 1;
   return s;
}

void session_manager_free_bare(session_t *s) {
   if (!s) {
      return;
   }
   pthread_mutex_destroy(&s->ref_mutex);
   pthread_cond_destroy(&s->ref_zero_cond);
   free(s);
}

void session_release(session_t *s) {
   if (!s) {
      return;
   }
   pthread_mutex_lock(&s->ref_mutex);
   s->ref_count--;
   if (s->ref_count <= 0) {
      pthread_cond_broadcast(&s->ref_zero_cond);
   }
   pthread_mutex_unlock(&s->ref_mutex);
}

/* --- DB + scheduler stubs -------------------------------------------------- */
/* job_manager.c's boot scan + completion monitor touch these; the pool-logic
 * tests below don't depend on their behavior, so they are minimal no-ops.  (The
 * DB accessors themselves are exercised by test_conv_db_jobs against the real
 * auth-DB chain.) */

/* Shared bound for every programmable stub table below. */
#define STUB_MAX_ROWS 8

/* Opt-in stale row, so one test can drive the boot interrupted-scan. */
static bool g_stale_enabled = false;
#define STUB_STALE_ID 999

int conv_db_job_scan_active(job_record_t *out, int max, int *count_out) {
   int n = 0;
   if (g_stale_enabled && out != NULL && max > 0) {
      memset(&out[0], 0, sizeof(out[0]));
      out[0].id = STUB_STALE_ID;
      out[0].user_id = 1;
      snprintf(out[0].title, sizeof(out[0].title), "stale");
      snprintf(out[0].on_complete, sizeof(out[0].on_complete), "reinvoke_parent");
      snprintf(out[0].job_status, sizeof(out[0].job_status), "running");
      out[0].parent_id = 42;
      n = 1;
   }
   if (count_out) {
      *count_out = n;
   }
   return AUTH_DB_SUCCESS;
}

/* Records what the boot scan wrote, so a test can assert the stamp it chose. */
static struct {
   int64_t conv_id;
   char status[JOB_STATUS_MAX];
   time_t at;
} g_terminal[STUB_MAX_ROWS];
static int g_terminal_n = 0;

int conv_db_job_set_terminal(int64_t conv_id, const char *status, const char *error, time_t at) {
   (void)error;
   if (g_terminal_n < STUB_MAX_ROWS) {
      g_terminal[g_terminal_n].conv_id = conv_id;
      snprintf(g_terminal[g_terminal_n].status, sizeof(g_terminal[g_terminal_n].status), "%s",
               status ? status : "");
      g_terminal[g_terminal_n].at = at;
      g_terminal_n++;
   }
   return AUTH_DB_SUCCESS;
}

/* Guards the stub tables the DETACHED delivery thread also writes (the fired log
 * and the missed-notification counter).  Without it the harness itself races,
 * which buries any genuine report from job_manager.c under its own noise. */
static pthread_mutex_t g_stub_mutex = PTHREAD_MUTEX_INITIALIZER;

/* The durable fallback the toast path hands off to when no browser is present. */
static int g_missed_inserts = 0;

int missed_notif_insert(int user_id,
                        int64_t event_id,
                        const char *event_type,
                        const char *status,
                        const char *name,
                        const char *message,
                        time_t fire_at,
                        int64_t conversation_id) {
   (void)user_id;
   (void)event_id;
   (void)event_type;
   (void)status;
   (void)name;
   (void)message;
   (void)fire_at;
   (void)conversation_id;
   pthread_mutex_lock(&g_stub_mutex);
   g_missed_inserts++;
   pthread_mutex_unlock(&g_stub_mutex);
   return SUCCESS;
}

static int stub_missed_inserts(void) {
   pthread_mutex_lock(&g_stub_mutex);
   int n = g_missed_inserts;
   pthread_mutex_unlock(&g_stub_mutex);
   return n;
}

/* Programmable follow-up queue + fired log, so the completion-monitor tests
 * below can pin WHEN a row is recorded as announced.  Empty by default, which
 * leaves every pool-logic test above unaffected. */
static job_record_t g_pending[STUB_MAX_ROWS];
static int g_pending_n = 0;
static int64_t g_fired[STUB_MAX_ROWS];
static int g_fired_n = 0;

static bool stub_was_fired(int64_t id) {
   bool found = false;
   pthread_mutex_lock(&g_stub_mutex);
   for (int i = 0; i < g_fired_n && !found; i++) {
      found = (g_fired[i] == id);
   }
   pthread_mutex_unlock(&g_stub_mutex);
   return found;
}

int conv_db_job_list_pending_followups(int max, job_record_t *out, int *count_out) {
   int n = (g_pending_n < max) ? g_pending_n : max;
   for (int i = 0; i < n; i++) {
      out[i] = g_pending[i];
   }
   if (count_out) {
      *count_out = n;
   }
   return AUTH_DB_SUCCESS;
}

int conv_db_job_mark_fired(int64_t conv_id) {
   pthread_mutex_lock(&g_stub_mutex);
   if (g_fired_n < STUB_MAX_ROWS) {
      g_fired[g_fired_n++] = conv_id;
   }
   pthread_mutex_unlock(&g_stub_mutex);
   return AUTH_DB_SUCCESS;
}

/* Strong override of job_manager.c's weak toast seam: reports how many clients
 * it reached, which is the whole question the monitor now asks it. */
static int g_toast_clients = 0;
static int g_toast_calls = 0;

int webui_broadcast_job_notification(int user_id, const char *text, int64_t conv_id, int running) {
   (void)user_id;
   (void)text;
   (void)conv_id;
   (void)running;
   g_toast_calls++;
   return g_toast_clients;
}

/* job_update_emit() reads the row back before broadcasting it; these tests
 * exercise pool logic, not the WS frames, so the read reports "not a job" and
 * the emit is a no-op. */
int conv_db_job_get(int64_t conv_id, int user_id, job_record_t *out) {
   (void)conv_id;
   (void)user_id;
   (void)out;
   return AUTH_DB_NOT_FOUND;
}

/* job_manager_cancel_or_retire()'s not-running branch probes the row's status to
 * decide between "retire a queued row" and "already terminal".  These tests only
 * exercise the running-job path (the pool), so report no such job. */
int conv_db_job_get_status(int64_t conv_id, int user_id, char *status_out, size_t n) {
   (void)conv_id;
   (void)user_id;
   if (status_out && n > 0) {
      status_out[0] = '\0';
   }
   return AUTH_DB_NOT_FOUND;
}

/* Phase-2 observe seam: job_manager_set_terminal pairs the DB write with a
 * `complete` event.  These tests exercise pool logic, not the event log, so the
 * pairing is stubbed out here — same treatment as the conv_db_job_* setters
 * above.  (This file bare-links job_manager.c, so every new cross-module call it
 * gains needs a stub or the target silently stops building — the failure shows
 * up only under `make tests-ci`, never `make dawn`.) */
void conv_event_emit(int64_t conv_id, int user_id, const char *kind, char *payload_owned) {
   (void)conv_id;
   (void)user_id;
   (void)kind;
   free(payload_owned);
}

char *event_payload_complete(const char *disposition,
                             const char *error_or_null,
                             int64_t final_message_id) {
   (void)disposition;
   (void)error_or_null;
   (void)final_message_id;
   return NULL;
}

/* The messaging send, as seen from the detached delivery thread.  Counted and
 * programmable so the claim/ceiling tests can drive a channel that is down. */
static _Atomic int g_emit_alert_calls = 0;
static _Atomic int g_emit_alert_rc = SUCCESS;
/* Holds the delivery thread inside the "send" so a test can observe the row
 * while it is genuinely in flight — the state the in_flight guard exists for.
 * Sleeping instead would make the test a race with itself. */
static _Atomic bool g_emit_alert_block = false;

int scheduler_emit_alert(int user_id,
                         const char *text,
                         sched_event_type_t type,
                         const char *deliver_to,
                         bool speak) {
   (void)user_id;
   (void)text;
   (void)type;
   (void)deliver_to;
   (void)speak;
   atomic_fetch_add(&g_emit_alert_calls, 1);
   while (atomic_load(&g_emit_alert_block)) {
      struct timespec ts = { 0, 2 * 1000 * 1000 };
      nanosleep(&ts, NULL);
   }
   return atomic_load(&g_emit_alert_rc);
}

/* Wait until the delivery thread has entered the send at least @n times. */
static void stub_wait_for_emit_calls(int n) {
   for (int i = 0; i < 500 && atomic_load(&g_emit_alert_calls) < n; i++) {
      struct timespec ts = { 0, 2 * 1000 * 1000 };
      nanosleep(&ts, NULL);
   }
}

/* The delivery thread is detached, so a test that needs its side effects has to
 * wait for them.  Poll the call counter rather than sleeping a fixed amount. */
static void stub_wait_for_delivery_threads(void) {
   int before = atomic_load(&g_emit_alert_calls);
   for (int i = 0; i < 200; i++) { /* ≤1 s */
      if (atomic_load(&g_emit_alert_calls) > before) {
         break;
      }
      struct timespec ts = { 0, 5 * 1000 * 1000 };
      nanosleep(&ts, NULL);
   }
   /* Let the thread finish mark_fired + delivery_release after its send. */
   struct timespec settle = { 0, 20 * 1000 * 1000 };
   nanosleep(&settle, NULL);
}

/* --- fixtures -------------------------------------------------------------- */

void setUp(void) {
   /* Generous defaults; individual tests tighten the cap under test. */
   g_config.jobs.max_active_jobs = 16;
   g_config.jobs.max_concurrent_local = 16;
   g_config.jobs.max_concurrent_cloud = 16;
   g_config.jobs.max_jobs_per_user = 16;
   g_config.jobs.max_runtime_sec = 1800;
   g_config.jobs.monitor_followups_per_tick = 4;
   g_pending_n = 0;
   g_fired_n = 0;
   g_toast_clients = 0;
   g_toast_calls = 0;
   g_missed_inserts = 0;
   g_terminal_n = 0;
   g_stale_enabled = false;
   atomic_store(&g_emit_alert_calls, 0);
   atomic_store(&g_emit_alert_rc, SUCCESS);
   atomic_store(&g_emit_alert_block, false);
   TEST_ASSERT_EQUAL_INT(SUCCESS, job_manager_init());
}

void tearDown(void) {
   /* Release any held delivery thread and let it finish touching the stub
    * tables before the next setUp resets them. */
   atomic_store(&g_emit_alert_block, false);
   struct timespec settle = { 0, 30 * 1000 * 1000 };
   nanosleep(&settle, NULL);
   job_manager_shutdown();
}

/* --- tests ----------------------------------------------------------------- */

static void test_begin_and_count(void) {
   session_t *a = NULL, *b = NULL;
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(1, 1001, JOB_PROVIDER_CLOUD, &a));
   TEST_ASSERT_NOT_NULL(a);
   TEST_ASSERT_EQUAL_INT(SESSION_TYPE_JOB, a->type);
   TEST_ASSERT_EQUAL_INT64(1001, atomic_load(&a->stream_conversation_id));
   TEST_ASSERT_EQUAL_INT(1, a->metrics.user_id);
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(1, 1002, JOB_PROVIDER_CLOUD, &b));
   TEST_ASSERT_EQUAL_INT(2, job_manager_running_count());

   job_manager_end(a);
   TEST_ASSERT_EQUAL_INT(1, job_manager_running_count());
   job_manager_end(b);
   TEST_ASSERT_EQUAL_INT(0, job_manager_running_count());
}

static void test_global_cap(void) {
   g_config.jobs.max_active_jobs = 2;
   session_t *s[3] = { 0 };
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(1, 1, JOB_PROVIDER_CLOUD, &s[0]));
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(2, 2, JOB_PROVIDER_CLOUD, &s[1]));
   TEST_ASSERT_EQUAL_INT(JOB_MGR_CAP_GLOBAL, job_manager_begin(3, 3, JOB_PROVIDER_CLOUD, &s[2]));
   TEST_ASSERT_NULL(s[2]);
   job_manager_end(s[0]);
   job_manager_end(s[1]);
}

static void test_provider_cap(void) {
   g_config.jobs.max_concurrent_local = 1;
   session_t *a = NULL, *b = NULL, *c = NULL;
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(1, 1, JOB_PROVIDER_LOCAL, &a));
   TEST_ASSERT_EQUAL_INT(JOB_MGR_CAP_PROVIDER, job_manager_begin(1, 2, JOB_PROVIDER_LOCAL, &b));
   /* Cloud is independent — still allowed. */
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(1, 3, JOB_PROVIDER_CLOUD, &c));
   job_manager_end(a);
   job_manager_end(c);
}

static void test_user_cap(void) {
   g_config.jobs.max_jobs_per_user = 2;
   session_t *a = NULL, *b = NULL, *cc = NULL, *d = NULL;
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(7, 1, JOB_PROVIDER_CLOUD, &a));
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(7, 2, JOB_PROVIDER_CLOUD, &b));
   TEST_ASSERT_EQUAL_INT(JOB_MGR_CAP_USER, job_manager_begin(7, 3, JOB_PROVIDER_CLOUD, &cc));
   /* A different user is unaffected. */
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(8, 4, JOB_PROVIDER_CLOUD, &d));
   job_manager_end(a);
   job_manager_end(b);
   job_manager_end(d);
}

static void test_resolver(void) {
   TEST_ASSERT_NOT_NULL(g_registered_lookup); /* job_manager_init registered it */
   session_t *a = NULL;
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(1, 500, JOB_PROVIDER_CLOUD, &a));

   /* Resolve by session id → retained (ref++). */
   session_t *r = g_registered_lookup(a->session_id, false);
   TEST_ASSERT_EQUAL_PTR(a, r);
   TEST_ASSERT_EQUAL_INT(2, a->ref_count); /* base 1 + resolve 1 */
   session_release(r);
   TEST_ASSERT_EQUAL_INT(1, a->ref_count);

   /* Unknown id → NULL. */
   TEST_ASSERT_NULL(g_registered_lookup(999999, false));

   /* Cancelled turn: for_reconnect=false skips, for_reconnect=true still resolves. */
   session_cancel_turn(a);
   TEST_ASSERT_NULL(g_registered_lookup(a->session_id, false));
   session_t *r2 = g_registered_lookup(a->session_id, true);
   TEST_ASSERT_EQUAL_PTR(a, r2);
   session_release(r2);

   job_manager_end(a);
}

static void test_cancel_ownership(void) {
   session_t *a = NULL;
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(5, 42, JOB_PROVIDER_CLOUD, &a));

   TEST_ASSERT_EQUAL_INT(JOB_MGR_NOT_FOUND, job_manager_cancel(9999, 5)); /* unknown conv */
   TEST_ASSERT_EQUAL_INT(JOB_MGR_FORBIDDEN, job_manager_cancel(42, 6));   /* wrong user */
   TEST_ASSERT_FALSE(atomic_load(&a->cancel_requested));
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_cancel(42, 5)); /* owner */
   TEST_ASSERT_TRUE(atomic_load(&a->cancel_requested));

   job_manager_end(a);
}

/* --- runtime reap ----------------------------------------------------------
 * jobs_monitor_tick takes `now` as a parameter precisely so the overdue clock
 * can be simulated: these pass a future `now` rather than sleeping. */

static void test_reap_overdue(void) {
   session_t *a = NULL;
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(1, 700, JOB_PROVIDER_LOCAL, &a));
   TEST_ASSERT_FALSE(atomic_load(&a->cancel_requested));

   time_t overdue = time(NULL) + g_config.jobs.max_runtime_sec + 5;
   TEST_ASSERT_EQUAL_INT(1, job_manager_reap_overdue(overdue));

   /* Reaped jobs are cancel-requested so the worker unwinds and frees the slot. */
   TEST_ASSERT_TRUE(atomic_load(&a->cancel_requested));

   /* Idempotent: an already-reaped slot is not counted again on a later tick
    * (it moves to the nag/force-release track, not a second first-strike). */
   TEST_ASSERT_EQUAL_INT(0, job_manager_reap_overdue(overdue + 1));

   /* The worker claims the verdict — true, so it records `failed`/timed-out
    * rather than `cancelled` (which would suppress the completion follow-up). */
   TEST_ASSERT_TRUE(job_manager_claim_reaped(a, NULL));

   /* The slot is only released by the worker's job_manager_end(). */
   TEST_ASSERT_EQUAL_INT(1, job_manager_running_count());
   job_manager_end(a);
   TEST_ASSERT_EQUAL_INT(0, job_manager_running_count());
}

/* Claiming stops the reap clock, so a job that reaches its terminal-write block
 * can no longer be flagged (or nagged, or force-released) while it finishes —
 * this is what stops the reap stealing an answer that already landed. */
static void test_claim_stops_the_reap_clock(void) {
   session_t *a = NULL;
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(1, 708, JOB_PROVIDER_CLOUD, &a));

   /* Worker finishes just under the wire and claims: not reaped. */
   TEST_ASSERT_FALSE(job_manager_claim_reaped(a, NULL));

   /* A tick well past the deadline must now find nothing to reap. */
   TEST_ASSERT_EQUAL_INT(0, job_manager_reap_overdue(time(NULL) + 999999));
   TEST_ASSERT_FALSE(atomic_load(&a->cancel_requested));

   job_manager_end(a);
}

/* A reaped job that never returns must not hold its provider counter forever:
 * after the grace period the counter is reclaimed so new jobs can start, while
 * the slot stays owned (the zombie's session pointer must remain valid). */
static void test_reap_force_releases_provider_counter(void) {
   g_config.jobs.max_runtime_sec = 10;
   g_config.jobs.max_concurrent_local = 1; /* the default, and the painful case */
   session_t *stuck = NULL, *next = NULL;
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(1, 709, JOB_PROVIDER_LOCAL, &stuck));

   time_t t0 = time(NULL);
   TEST_ASSERT_EQUAL_INT(1, job_manager_reap_overdue(t0 + 11)); /* first strike */

   /* Still inside the grace period: the counter is held, so local is full. */
   TEST_ASSERT_EQUAL_INT(JOB_MGR_CAP_PROVIDER,
                         job_manager_begin(1, 710, JOB_PROVIDER_LOCAL, &next));

   /* Past the grace period: counter reclaimed, a new local job can start. */
   job_manager_reap_overdue(t0 + 11 + JOB_REAP_FORCE_RELEASE_SEC + 1);
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(1, 711, JOB_PROVIDER_LOCAL, &next));

   /* The zombie's slot is still occupied, so its session is still resolvable. */
   TEST_ASSERT_TRUE(job_manager_claim_reaped(stuck, NULL));

   /* If the zombie ever returns, job_manager_end must NOT double-decrement. */
   job_manager_end(stuck);
   TEST_ASSERT_EQUAL_INT(1, job_manager_running_count()); /* only `next` remains */
   job_manager_end(next);
   TEST_ASSERT_EQUAL_INT(0, job_manager_running_count());
}

static void test_reap_not_yet_overdue(void) {
   session_t *a = NULL;
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(1, 701, JOB_PROVIDER_CLOUD, &a));

   /* Exactly at the limit is NOT overdue (strictly-greater comparison). */
   TEST_ASSERT_EQUAL_INT(0, job_manager_reap_overdue(time(NULL) + g_config.jobs.max_runtime_sec));
   TEST_ASSERT_FALSE(job_manager_claim_reaped(a, NULL));
   TEST_ASSERT_FALSE(atomic_load(&a->cancel_requested));

   job_manager_end(a);
}

static void test_reap_disabled_when_zero(void) {
   g_config.jobs.max_runtime_sec = 0; /* the documented "disabled" value */
   session_t *a = NULL;
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(1, 702, JOB_PROVIDER_CLOUD, &a));

   TEST_ASSERT_EQUAL_INT(0, job_manager_reap_overdue(time(NULL) + 999999));
   TEST_ASSERT_FALSE(job_manager_claim_reaped(a, NULL));
   TEST_ASSERT_FALSE(atomic_load(&a->cancel_requested));

   job_manager_end(a);
}

static void test_reap_ignores_backwards_clock(void) {
   session_t *a = NULL;
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(1, 703, JOB_PROVIDER_CLOUD, &a));

   /* An NTP step backwards must not mass-reap the pool (negative elapsed). */
   TEST_ASSERT_EQUAL_INT(0, job_manager_reap_overdue(time(NULL) - 999999));
   TEST_ASSERT_FALSE(job_manager_claim_reaped(a, NULL));

   job_manager_end(a);
}

static void test_reap_distinct_from_user_cancel(void) {
   session_t *a = NULL;
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(5, 704, JOB_PROVIDER_CLOUD, &a));

   /* A user cancel raises the same cancel_requested flag, but must NOT read as
    * a reap — otherwise the worker would fire a follow-up for a cancelled job. */
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_cancel(704, 5));
   TEST_ASSERT_TRUE(atomic_load(&a->cancel_requested));
   TEST_ASSERT_FALSE(job_manager_claim_reaped(a, NULL));

   job_manager_end(a);
}

/* The scan must reap EVERY overdue slot in one pass, not stop at the first —
 * otherwise a second wedged job survives a tick and keeps its provider counter.
 * (Both jobs share a start second here, which is the realistic case: slot
 * clocks are 1-second resolution, so a burst of spawns expires together.) */
static void test_reap_all_overdue_slots(void) {
   g_config.jobs.max_runtime_sec = 100;
   session_t *a = NULL, *b = NULL, *c = NULL;
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(1, 705, JOB_PROVIDER_CLOUD, &a));
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(1, 706, JOB_PROVIDER_CLOUD, &b));
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(2, 707, JOB_PROVIDER_LOCAL, &c));

   TEST_ASSERT_EQUAL_INT(3, job_manager_reap_overdue(time(NULL) + 101));
   TEST_ASSERT_TRUE(atomic_load(&a->cancel_requested));
   TEST_ASSERT_TRUE(atomic_load(&b->cancel_requested));
   TEST_ASSERT_TRUE(atomic_load(&c->cancel_requested)); /* across users and providers */

   job_manager_end(a);
   job_manager_end(b);
   job_manager_end(c);
}

static void test_claim_reaped_null_and_unknown(void) {
   TEST_ASSERT_FALSE(job_manager_claim_reaped(NULL, NULL));
   session_t orphan;
   memset(&orphan, 0, sizeof(orphan)); /* never in the pool */
   TEST_ASSERT_FALSE(job_manager_claim_reaped(&orphan, NULL));
}

/* A human's Cancel and the shutdown sweep raise the SAME cancel_requested flag,
 * and only one of them should suppress the completion notice — so the pool
 * records who asked.  Slots are recycled, which makes "whose verdict is this?"
 * a real question: a stale flag would file the NEXT job's shutdown-interruption
 * as a user cancel, firing it and telling nobody. */
static void test_user_cancel_verdict_is_per_job(void) {
   session_t *a = NULL;
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(1, 1400, JOB_PROVIDER_CLOUD, &a));
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_cancel(1400, 1));

   bool user_cancelled = false;
   TEST_ASSERT_FALSE(job_manager_claim_reaped(a, &user_cancelled));
   TEST_ASSERT_TRUE(user_cancelled);
   job_manager_end(a);

   /* Whichever job lands in that slot next starts with a clean verdict. */
   session_t *b = NULL;
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(1, 1401, JOB_PROVIDER_CLOUD, &b));
   user_cancelled = true; /* poison the out-param so a no-op write would show */
   TEST_ASSERT_FALSE(job_manager_claim_reaped(b, &user_cancelled));
   TEST_ASSERT_FALSE(user_cancelled);
   job_manager_end(b);
}

/* The SIGINT handler has to be able to say "we are going down" BEFORE
 * job_manager_shutdown() runs, because llm_request_interrupt() stops every job's
 * tool loop within milliseconds and each worker classifies itself right then.
 * Without this, every Ctrl+C filed its interrupted jobs as "failed: no response
 * from model" — the branch meant to catch them keyed on a session cancel flag
 * that had not been set yet, so it was unreachable in practice. */
static void test_shutdown_can_be_noted_before_teardown(void) {
   TEST_ASSERT_FALSE(job_manager_is_shutting_down());
   job_manager_note_shutdown_requested();
   TEST_ASSERT_TRUE(job_manager_is_shutting_down());
   /* tearDown's job_manager_shutdown() leaves the flag set; setUp's init is what
    * clears it, which the next test's first assertion above re-checks. */
}

/* --- completion monitor ---------------------------------------------------- */

static void stub_queue_row(int64_t id, const char *on_complete, time_t finished_at) {
   TEST_ASSERT_LESS_THAN_INT(STUB_MAX_ROWS, g_pending_n);
   job_record_t *r = &g_pending[g_pending_n++];
   memset(r, 0, sizeof(*r));
   r->id = id;
   r->user_id = 1;
   snprintf(r->title, sizeof(r->title), "job %lld", (long long)id);
   snprintf(r->on_complete, sizeof(r->on_complete), "%s", on_complete);
   snprintf(r->job_status, sizeof(r->job_status), "%s", "done");
   r->finished_at = finished_at;
}

static job_record_t g_processed[STUB_MAX_ROWS];
static int g_processed_n = 0;

static void stub_reinvoke_processor(const job_record_t *rows, int n) {
   for (int i = 0; i < n && g_processed_n < STUB_MAX_ROWS; i++) {
      g_processed[g_processed_n++] = rows[i];
   }
}

/* Accepts the rows but fires none of them, the way the real processor behaves
 * when every parent is mid-turn — the "nothing moved" case the gate must not
 * re-arm on.  Deliberately does NOT mark dirty, unlike the real one, so the test
 * observes the drain's own decision. */
static void stub_reinvoke_processor_defer_all(const job_record_t *rows, int n) {
   for (int i = 0; i < n && g_processed_n < STUB_MAX_ROWS; i++) {
      g_processed[g_processed_n++] = rows[i];
   }
}

/* The bug that shipped three times: a completion recorded as announced when
 * nothing announced it.  A browser is reliably absent right after a restart —
 * the daemon always finishes booting before one can reconnect — so a bare
 * broadcast loses exactly the completions a restart stranded.  The row is still
 * retired on the spot (it must not sit at the head of a queue every user
 * shares), but only because the notice went somewhere durable first. */
static void test_undeliverable_notice_is_queued_not_dropped(void) {
   stub_queue_row(700, "notify", time(NULL) - 5);

   g_toast_clients = 0; /* nobody connected */
   job_manager_mark_dirty();
   jobs_monitor_tick(time(NULL));
   TEST_ASSERT_EQUAL_INT(1, g_toast_calls);
   TEST_ASSERT_EQUAL_INT(1, stub_missed_inserts()); /* handed to the durable path */
   TEST_ASSERT_TRUE(stub_was_fired(700));           /* and retired, so the queue moves */
}

/* With a browser present the toast is enough; nothing is queued for replay. */
static void test_delivered_notice_is_not_also_queued(void) {
   stub_queue_row(701, "notify", time(NULL) - 5);

   g_toast_clients = 1;
   job_manager_mark_dirty();
   jobs_monitor_tick(time(NULL));
   TEST_ASSERT_EQUAL_INT(1, g_toast_calls);
   TEST_ASSERT_EQUAL_INT(0, stub_missed_inserts());
   TEST_ASSERT_TRUE(stub_was_fired(701));
}

/* The gate exists so an idle daemon does zero DB work.  A pass that moved
 * nothing must not re-arm it, or a stuck batch becomes a permanent 1 Hz scan. */
static void test_gate_closes_when_a_pass_moves_nothing(void) {
   /* on_complete_fired is stubbed, so these rows keep coming back — which is
    * exactly the "nothing moved" shape the flag guards against. */
   for (int i = 0; i < 4; i++) {
      stub_queue_row(800 + i, "reinvoke_parent", time(NULL) + 5);
   }
   g_processed_n = 0;
   job_manager_register_reinvoke_processor(stub_reinvoke_processor_defer_all);
   job_manager_mark_dirty();
   jobs_monitor_tick(time(NULL));
   TEST_ASSERT_EQUAL_INT(4, g_processed_n);

   /* Gate closed → the next tick does no work at all. */
   jobs_monitor_tick(time(NULL));
   TEST_ASSERT_EQUAL_INT(4, g_processed_n);
   job_manager_register_reinvoke_processor(NULL);
}

/* The boot scan stamps finished_at just BEFORE s_boot_time, and that stamp is
 * load-bearing rather than cosmetic: stamping `now` would classify the very jobs
 * the restart interrupted as "finished during this run" and auto-reinvoke their
 * parent conversations across the restart — the one thing restart-safety
 * forbids, and the whole population Resume exists for.
 *
 * Asserted through the behaviour rather than the timestamp, so it neither
 * depends on the wall clock nor passes if the `- 1` is quietly removed. */
static void test_boot_scanned_job_cannot_reinvoke_across_the_restart(void) {
   job_manager_shutdown();
   g_stale_enabled = true;
   g_terminal_n = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, job_manager_init());

   /* Boot terminalized the stale row; replay it to the monitor exactly as it
    * was written, stamp and all. */
   TEST_ASSERT_EQUAL_INT(1, g_terminal_n);
   TEST_ASSERT_EQUAL_STRING("interrupted", g_terminal[0].status);
   stub_queue_row(STUB_STALE_ID, "reinvoke_parent", g_terminal[0].at);

   g_processed_n = 0;
   g_toast_clients = 1;
   job_manager_register_reinvoke_processor(stub_reinvoke_processor);
   job_manager_mark_dirty();
   jobs_monitor_tick(time(NULL));

   TEST_ASSERT_EQUAL_INT(0, g_processed_n); /* never re-engages the parent */
   TEST_ASSERT_EQUAL_INT(1, g_toast_calls); /* the user is still told */
   TEST_ASSERT_TRUE(stub_was_fired(STUB_STALE_ID));
   job_manager_register_reinvoke_processor(NULL);
}

/* Restart safety without the destructive rewrite.  Boot used to rewrite
 * on_complete to 'notify' permanently, so a job interrupted by the restart and
 * then explicitly Resumed returned its results to nobody.  The demotion is a
 * per-tick DECISION now, taken from finished_at, so a resumed run (which clears
 * finished_at) regains its disposition with no extra state. */
static void test_cross_restart_reinvoke_is_delivered_not_re_engaged(void) {
   g_processed_n = 0;
   job_manager_register_reinvoke_processor(stub_reinvoke_processor);

   stub_queue_row(900, "reinvoke_parent", time(NULL) - 3600); /* before this boot */
   stub_queue_row(901, "reinvoke_parent", time(NULL) + 5);    /* after it */

   g_toast_clients = 1;
   job_manager_mark_dirty();
   jobs_monitor_tick(time(NULL));

   /* The pre-boot row was delivered as a plain notice... */
   TEST_ASSERT_EQUAL_INT(1, g_toast_calls);
   TEST_ASSERT_TRUE(stub_was_fired(900));
   /* ...and the one that finished during this run still re-engages its parent,
    * unfired, because the processor owns firing it. */
   TEST_ASSERT_EQUAL_INT(1, g_processed_n);
   TEST_ASSERT_EQUAL_INT64(901, g_processed[0].id);
   TEST_ASSERT_FALSE(stub_was_fired(901));

   job_manager_register_reinvoke_processor(NULL);
}

/* The messaging path's claim table is the only concurrency-sensitive code in the
 * completion monitor, and the one place a claim leak already shipped once this
 * cycle.  These drive it through the drain, since the table itself is static.
 *
 * A row queued to the delivery thread must not be queued AGAIN by the next
 * tick — the send is a blocking curl, so the row is still unfired and still at
 * the head of the scan while it is in flight.  Without the in_flight guard the
 * user gets a duplicate message every second until it completes. */
static void test_inflight_messaging_row_is_not_requeued(void) {
   stub_queue_row(970, "notify", time(NULL) - 5);
   snprintf(g_pending[0].deliver_to, sizeof(g_pending[0].deliver_to), "telegram:me");

   atomic_store(&g_emit_alert_block, true); /* hold the thread inside the send */
   job_manager_mark_dirty();
   jobs_monitor_tick(time(NULL));
   stub_wait_for_emit_calls(1);
   TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_emit_alert_calls));

   /* The row is genuinely in flight and still unfired, so the scan returns it
    * again.  It must be skipped — re-queueing sends the user a duplicate. */
   job_manager_mark_dirty();
   jobs_monitor_tick(time(NULL));
   struct timespec settle = { 0, 50 * 1000 * 1000 };
   nanosleep(&settle, NULL);
   TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_emit_alert_calls));

   atomic_store(&g_emit_alert_block, false);
   stub_wait_for_delivery_threads();
   TEST_ASSERT_TRUE(stub_was_fired(970)); /* and the thread fires it exactly once */
}

/* Out of attempts, the row is retired so it stops holding the head of a queue
 * every user shares — but the notice goes to the durable path first.  Giving up
 * on a CHANNEL is not a reason to give up on telling the user. */
static void test_messaging_giveup_retires_but_still_tells_the_user(void) {
   stub_queue_row(971, "notify", time(NULL) - 5);
   snprintf(g_pending[0].deliver_to, sizeof(g_pending[0].deliver_to), "telegram:me");
   atomic_store(&g_emit_alert_rc, FAILURE); /* the channel is down */
   g_toast_clients = 0;                     /* and no browser either */

   /* Each tick makes one attempt; the delivery thread releases the claim on
    * failure, so the ceiling is reached after JOB_NOTIFY_MAX_ATTEMPTS rounds. */
   for (int i = 0; i < 12 && !stub_was_fired(971); i++) {
      job_manager_mark_dirty();
      jobs_monitor_tick(time(NULL));
      stub_wait_for_delivery_threads();
   }
   TEST_ASSERT_TRUE(stub_was_fired(971));
   TEST_ASSERT_GREATER_THAN_INT(0, stub_missed_inserts()); /* not silently dropped */
}

/* 'none' is the only disposition that owes the user nothing. */
static void test_none_disposition_is_retired_silently(void) {
   stub_queue_row(950, "none", time(NULL) - 5);
   g_toast_clients = 1;
   job_manager_mark_dirty();
   jobs_monitor_tick(time(NULL));
   TEST_ASSERT_EQUAL_INT(0, g_toast_calls);
   TEST_ASSERT_TRUE(stub_was_fired(950));
}

/* The staleness bound is the one unconditional fire — otherwise a row nobody
 * can ever receive would be retried until the heat death of the daemon. */
static void test_ancient_row_is_retired_without_delivery(void) {
   stub_queue_row(960, "notify", time(NULL) - (8 * 24 * 60 * 60));
   g_toast_clients = 0;
   job_manager_mark_dirty();
   jobs_monitor_tick(time(NULL));
   TEST_ASSERT_EQUAL_INT(0, g_toast_calls);
   TEST_ASSERT_TRUE(stub_was_fired(960));
}

/* Test-only shims over job_manager.c's static delivery-claim bookkeeping. */
bool job_manager_delivery_claim_for_test(int64_t job_id, time_t now, bool *give_up);
void job_manager_delivery_release_for_test(int64_t job_id, bool finished);
void job_manager_delivery_reset_for_test(void);
int job_manager_delivery_slots_for_test(void);
int job_manager_delivery_stale_sec_for_test(void);

/* A full delivery table swept clean just before a not-tracked claim used to read
 * (or, when full, write) one past the live region: the "not found" sentinel was
 * seeded from s_delivery_n BEFORE the sweep decremented it.  Regression — the
 * claim must cleanly insert, returning a real verdict and leaving a coherent
 * table, not a garbage verdict or corrupt state. */
static void test_delivery_claim_after_full_table_sweep(void) {
   job_manager_delivery_reset_for_test();
   const int slots = job_manager_delivery_slots_for_test();
   const time_t t0 = 1000000; /* fixed clock */
   bool give_up = false;

   /* Fill the table: each id claimed then released-not-finished stays as a
    * not-in-flight entry with last_attempt_at = t0. */
   for (int i = 0; i < slots; i++) {
      TEST_ASSERT_TRUE(job_manager_delivery_claim_for_test(5000 + i, t0, &give_up));
      TEST_ASSERT_FALSE(give_up);
      job_manager_delivery_release_for_test(5000 + i, /*finished=*/false);
   }

   /* Full table; a claim for an untracked id past the staleness bound sweeps
    * every entry, then must INSERT the new one (not fall through to a dead
    * slot's verdict, and not write past the array end). */
   const time_t later = t0 + job_manager_delivery_stale_sec_for_test() + 1;
   give_up = true;
   TEST_ASSERT_TRUE(job_manager_delivery_claim_for_test(9000, later, &give_up));
   TEST_ASSERT_FALSE(give_up);

   /* Table is coherent: the just-inserted id is in-flight (refused), and a
    * second fresh id inserts — proving the sweep really reclaimed the slots. */
   TEST_ASSERT_FALSE(job_manager_delivery_claim_for_test(9000, later, &give_up));
   TEST_ASSERT_TRUE(job_manager_delivery_claim_for_test(9001, later, &give_up));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_delivery_claim_after_full_table_sweep);
   RUN_TEST(test_begin_and_count);
   RUN_TEST(test_global_cap);
   RUN_TEST(test_provider_cap);
   RUN_TEST(test_user_cap);
   RUN_TEST(test_resolver);
   RUN_TEST(test_cancel_ownership);
   RUN_TEST(test_reap_overdue);
   RUN_TEST(test_reap_not_yet_overdue);
   RUN_TEST(test_reap_disabled_when_zero);
   RUN_TEST(test_reap_ignores_backwards_clock);
   RUN_TEST(test_reap_distinct_from_user_cancel);
   RUN_TEST(test_reap_all_overdue_slots);
   RUN_TEST(test_claim_stops_the_reap_clock);
   RUN_TEST(test_reap_force_releases_provider_counter);
   RUN_TEST(test_claim_reaped_null_and_unknown);
   RUN_TEST(test_user_cancel_verdict_is_per_job);
   RUN_TEST(test_shutdown_can_be_noted_before_teardown);
   RUN_TEST(test_undeliverable_notice_is_queued_not_dropped);
   RUN_TEST(test_delivered_notice_is_not_also_queued);
   RUN_TEST(test_gate_closes_when_a_pass_moves_nothing);
   RUN_TEST(test_boot_scanned_job_cannot_reinvoke_across_the_restart);
   RUN_TEST(test_cross_restart_reinvoke_is_delivered_not_re_engaged);
   RUN_TEST(test_inflight_messaging_row_is_not_requeued);
   RUN_TEST(test_messaging_giveup_retires_but_still_tells_the_user);
   RUN_TEST(test_none_disposition_is_retired_silently);
   RUN_TEST(test_ancient_row_is_retired_without_delivery);
   return UNITY_END();
}
