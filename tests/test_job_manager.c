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

int conv_db_job_scan_active(job_record_t *out, int max, int *count_out) {
   (void)out;
   (void)max;
   if (count_out) {
      *count_out = 0;
   }
   return AUTH_DB_SUCCESS;
}

int conv_db_job_set_terminal(int64_t conv_id, const char *status, const char *error, time_t at) {
   (void)conv_id;
   (void)status;
   (void)error;
   (void)at;
   return AUTH_DB_SUCCESS;
}

int conv_db_job_fire_boot_reinvokes(int *count_out) {
   if (count_out) {
      *count_out = 0;
   }
   return AUTH_DB_SUCCESS;
}

int conv_db_job_list_pending_followups(int max, job_record_t *out, int *count_out) {
   (void)max;
   (void)out;
   if (count_out) {
      *count_out = 0;
   }
   return AUTH_DB_SUCCESS;
}

int conv_db_job_mark_fired(int64_t conv_id) {
   (void)conv_id;
   return AUTH_DB_SUCCESS;
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
   return 0;
}

/* --- fixtures -------------------------------------------------------------- */

void setUp(void) {
   /* Generous defaults; individual tests tighten the cap under test. */
   g_config.jobs.max_active_jobs = 16;
   g_config.jobs.max_concurrent_local = 16;
   g_config.jobs.max_concurrent_cloud = 16;
   g_config.jobs.max_jobs_per_user = 16;
   g_config.jobs.max_runtime_sec = 1800;
   TEST_ASSERT_EQUAL_INT(SUCCESS, job_manager_init());
}

void tearDown(void) {
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
   TEST_ASSERT_TRUE(job_manager_claim_reaped(a));

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
   TEST_ASSERT_FALSE(job_manager_claim_reaped(a));

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
   TEST_ASSERT_TRUE(job_manager_claim_reaped(stuck));

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
   TEST_ASSERT_FALSE(job_manager_claim_reaped(a));
   TEST_ASSERT_FALSE(atomic_load(&a->cancel_requested));

   job_manager_end(a);
}

static void test_reap_disabled_when_zero(void) {
   g_config.jobs.max_runtime_sec = 0; /* the documented "disabled" value */
   session_t *a = NULL;
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(1, 702, JOB_PROVIDER_CLOUD, &a));

   TEST_ASSERT_EQUAL_INT(0, job_manager_reap_overdue(time(NULL) + 999999));
   TEST_ASSERT_FALSE(job_manager_claim_reaped(a));
   TEST_ASSERT_FALSE(atomic_load(&a->cancel_requested));

   job_manager_end(a);
}

static void test_reap_ignores_backwards_clock(void) {
   session_t *a = NULL;
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(1, 703, JOB_PROVIDER_CLOUD, &a));

   /* An NTP step backwards must not mass-reap the pool (negative elapsed). */
   TEST_ASSERT_EQUAL_INT(0, job_manager_reap_overdue(time(NULL) - 999999));
   TEST_ASSERT_FALSE(job_manager_claim_reaped(a));

   job_manager_end(a);
}

static void test_reap_distinct_from_user_cancel(void) {
   session_t *a = NULL;
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_begin(5, 704, JOB_PROVIDER_CLOUD, &a));

   /* A user cancel raises the same cancel_requested flag, but must NOT read as
    * a reap — otherwise the worker would fire a follow-up for a cancelled job. */
   TEST_ASSERT_EQUAL_INT(JOB_MGR_OK, job_manager_cancel(704, 5));
   TEST_ASSERT_TRUE(atomic_load(&a->cancel_requested));
   TEST_ASSERT_FALSE(job_manager_claim_reaped(a));

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
   TEST_ASSERT_FALSE(job_manager_claim_reaped(NULL));
   session_t orphan;
   memset(&orphan, 0, sizeof(orphan)); /* never in the pool */
   TEST_ASSERT_FALSE(job_manager_claim_reaped(&orphan));
}

int main(void) {
   UNITY_BEGIN();
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
   return UNITY_END();
}
