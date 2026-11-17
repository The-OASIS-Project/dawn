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
 * The conversation turn lease: FIFO handover, re-entry, a stopped wait, a
 * try that never waits, posts written at once or queued and written in order at release, the table
 * at capacity, the session safety net, waking a waiter, and shutdown.
 */

#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#include "core/conv_turn_lease.h"
#include "core/job_manager.h"
#include "core/session_manager.h"
/* Only for MESSAGING_MAX_SESSIONS, to check the table size against it. */
#define MESSAGING_ENGINE_INTERNAL_ALLOWED
#include "messaging/messaging_engine_internal.h"
#include "unity.h"

/* Every session that can run a turn needs an entry it can hold or wait in. */
_Static_assert(CONV_TURN_LEASE_SLOTS >= MAX_SESSIONS + JOB_POOL_MAX_SLOTS + MESSAGING_MAX_SESSIONS,
               "the lease table is smaller than the sessions that can take a lease");

/* Bounds every wait so a broken lease fails the test instead of hanging it. */
#define GUARD_MS 5000

/* Each test uses its own conversation ids, so a failure can't leave a lease
 * held under the next test. */

static void pause_1ms(void) {
   const struct timespec ts = { 0, 1000000L };
   nanosleep(&ts, NULL);
}

/* Polls a condition the lease changes on another thread, bounded by GUARD_MS. */
static bool wait_for_waiters(int64_t conv, int n) {
   for (int i = 0; i < GUARD_MS; i++) {
      if (conv_turn_lease_waiters(conv) == n)
         return true;
      pause_1ms();
   }
   return false;
}

/* ---- an event log shared by the writer and the turn threads ---- */

static pthread_mutex_t s_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static int s_log[32];
static int s_log_n;

static void log_event(int id) {
   pthread_mutex_lock(&s_log_mutex);
   if (s_log_n < 32)
      s_log[s_log_n++] = id;
   pthread_mutex_unlock(&s_log_mutex);
}

/* ---- the writer ---- */

typedef struct test_post {
   conv_lease_item_t link;
   int id;
   int64_t conv;
   uint64_t expect_token;         /* the holder the write must run under (0: no check) */
   bool expect_post_hold;         /* the write must run under a post's own hold */
   struct test_post *post_during; /* posted from inside this item's write */
   sem_t *entered;                /* posted when the write starts (may be NULL) */
   sem_t *resume;                 /* the write waits for it (may be NULL) */
   bool fail;                     /* the write reports failure */
} test_post_t;

static atomic_int s_wrong_holder;
static atomic_int s_dropped;

static bool test_write(conv_lease_item_t *item) {
   test_post_t *p = (test_post_t *)item;
   if (p->entered)
      sem_post(p->entered);
   if (p->resume)
      sem_wait(p->resume);
   uint32_t session = 0;
   uint64_t token = 0;
   const bool held = conv_turn_lease_holder(p->conv, &session, &token, NULL);
   if (p->expect_token && (!held || token != p->expect_token))
      atomic_fetch_add(&s_wrong_holder, 1);
   if (p->expect_post_hold && (!held || session != CONV_TURN_LEASE_POST_SESSION))
      atomic_fetch_add(&s_wrong_holder, 1);
   log_event(p->id);
   if (p->post_during)
      TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_DEFERRED,
                            conv_turn_lease_post_or_defer(p->conv, &p->post_during->link));
   return !p->fail;
}

static void test_drop(conv_lease_item_t *item) {
   (void)item;
   atomic_fetch_add(&s_dropped, 1);
}

static void post_init(test_post_t *p, int id, int64_t conv, uint64_t expect_token) {
   memset(p, 0, sizeof(*p));
   p->id = id;
   p->conv = conv;
   p->expect_token = expect_token;
}

void setUp(void) {
   s_log_n = 0;
   atomic_store(&s_wrong_holder, 0);
   atomic_store(&s_dropped, 0);
   conv_turn_lease_set_writer(test_write, test_drop);
}

void tearDown(void) {
}

/* ---- a turn on its own thread ---- */

typedef struct {
   int64_t conv;
   uint32_t session;
   uint64_t token;
   int id; /* logged when granted */
   atomic_bool stop;
   int rc;
   pthread_t th;
} turn_t;

static bool turn_should_stop(void *ctx) {
   return atomic_load(&((turn_t *)ctx)->stop);
}

static void *turn_thread(void *arg) {
   turn_t *t = arg;
   t->rc = conv_turn_lease_acquire(t->conv, t->session, t->token, turn_should_stop, t);
   if (t->rc == CONV_TURN_LEASE_GRANTED) {
      log_event(t->id);
      conv_turn_lease_release(t->conv, t->token);
   }
   return NULL;
}

static void start_turn(turn_t *t, int64_t conv, uint32_t session, int id) {
   memset(t, 0, sizeof(*t));
   t->conv = conv;
   t->session = session;
   t->token = conv_turn_lease_new_token();
   t->id = id;
   pthread_create(&t->th, NULL, turn_thread, t);
}

/* ---- tests ---- */

static void test_waiters_are_served_in_order(void) {
   const int64_t conv = 101;
   const uint64_t mine = conv_turn_lease_new_token();
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_GRANTED,
                         conv_turn_lease_acquire(conv, 1, mine, NULL, NULL));
   turn_t t[3];
   for (int i = 0; i < 3; i++) {
      start_turn(&t[i], conv, (uint32_t)(10 + i), i + 1);
      TEST_ASSERT_TRUE(wait_for_waiters(conv, i + 1));
   }
   TEST_ASSERT_EQUAL_INT(SUCCESS, conv_turn_lease_release(conv, mine));
   for (int i = 0; i < 3; i++) {
      pthread_join(t[i].th, NULL);
      TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_GRANTED, t[i].rc);
   }
   TEST_ASSERT_EQUAL_INT(3, s_log_n);
   TEST_ASSERT_EQUAL_INT(1, s_log[0]);
   TEST_ASSERT_EQUAL_INT(2, s_log[1]);
   TEST_ASSERT_EQUAL_INT(3, s_log[2]);
   TEST_ASSERT_FALSE(conv_turn_lease_holder(conv, NULL, NULL, NULL));
}

static void test_reentry_is_counted(void) {
   const int64_t conv = 102;
   const uint64_t mine = conv_turn_lease_new_token();
   for (int i = 0; i < 3; i++)
      TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_GRANTED,
                            conv_turn_lease_acquire(conv, 1, mine, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(SUCCESS, conv_turn_lease_release(conv, mine));
   TEST_ASSERT_EQUAL_INT(SUCCESS, conv_turn_lease_release(conv, mine));
   TEST_ASSERT_TRUE(conv_turn_lease_holder(conv, NULL, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(SUCCESS, conv_turn_lease_release(conv, mine));
   TEST_ASSERT_FALSE(conv_turn_lease_holder(conv, NULL, NULL, NULL));
   /* Released: a further release is refused. */
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_FAILURE, conv_turn_lease_release(conv, mine));
}

static void test_should_stop_ends_a_wait(void) {
   const int64_t conv = 103;
   const uint64_t mine = conv_turn_lease_new_token();
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_GRANTED,
                         conv_turn_lease_acquire(conv, 1, mine, NULL, NULL));
   turn_t t;
   start_turn(&t, conv, 2, 1);
   TEST_ASSERT_TRUE(wait_for_waiters(conv, 1));
   atomic_store(&t.stop, true);
   pthread_join(t.th, NULL);
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_STOPPED, t.rc);
   TEST_ASSERT_EQUAL_INT(0, conv_turn_lease_waiters(conv));
   /* It left the queue: the release frees the lease rather than handing it on. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, conv_turn_lease_release(conv, mine));
   TEST_ASSERT_FALSE(conv_turn_lease_holder(conv, NULL, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(0, s_log_n);
}

static void test_one_conversation_does_not_block_another(void) {
   const uint64_t a = conv_turn_lease_new_token();
   const uint64_t b = conv_turn_lease_new_token();
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_GRANTED, conv_turn_lease_acquire(104, 1, a, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_GRANTED, conv_turn_lease_acquire(105, 2, b, NULL, NULL));
   conv_turn_lease_release(104, a);
   conv_turn_lease_release(105, b);
}

static void test_a_holder_of_one_never_waits_for_another(void) {
   const uint64_t a = conv_turn_lease_new_token();
   const uint64_t b = conv_turn_lease_new_token();
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_GRANTED, conv_turn_lease_acquire(106, 1, a, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_GRANTED, conv_turn_lease_acquire(107, 2, b, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_BUSY, conv_turn_lease_acquire(107, 1, a, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(0, conv_turn_lease_waiters(107));
   conv_turn_lease_release(106, a);
   conv_turn_lease_release(107, b);
}

static void test_a_post_into_a_free_conversation_is_written_now(void) {
   const int64_t conv = 108;
   test_post_t p;
   post_init(&p, 1, conv, 0);
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_WRITTEN, conv_turn_lease_post_or_defer(conv, &p.link));
   TEST_ASSERT_EQUAL_INT(1, s_log_n);
   TEST_ASSERT_FALSE(conv_turn_lease_holder(conv, NULL, NULL, NULL));
}

static void test_a_failed_immediate_write_is_reported(void) {
   const int64_t conv = 119;
   test_post_t p;
   post_init(&p, 1, conv, 0);
   p.fail = true;
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_WRITE_FAILED,
                         conv_turn_lease_post_or_defer(conv, &p.link));
   TEST_ASSERT_FALSE(conv_turn_lease_holder(conv, NULL, NULL, NULL));
}

static void test_a_free_post_is_written_under_the_lease(void) {
   const int64_t conv = 109;
   test_post_t p;
   post_init(&p, 1, conv, 0);
   p.expect_post_hold = true; /* so a turn can't take it mid-write */
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_WRITTEN, conv_turn_lease_post_or_defer(conv, &p.link));
   TEST_ASSERT_EQUAL_INT(0, atomic_load(&s_wrong_holder));
}

static void test_posts_are_deferred_and_written_before_the_next_holder(void) {
   const int64_t conv = 110;
   const uint64_t mine = conv_turn_lease_new_token();
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_GRANTED,
                         conv_turn_lease_acquire(conv, 1, mine, NULL, NULL));
   /* A post by the holding turn itself is queued too. */
   test_post_t p1, p2, late;
   post_init(&p1, 1, conv, mine);
   post_init(&p2, 2, conv, mine);
   post_init(&late, 3, conv, mine);
   p1.post_during = &late; /* lands while the release is writing */
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_DEFERRED, conv_turn_lease_post_or_defer(conv, &p1.link));
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_DEFERRED, conv_turn_lease_post_or_defer(conv, &p2.link));

   turn_t t;
   start_turn(&t, conv, 2, 99);
   TEST_ASSERT_TRUE(wait_for_waiters(conv, 1));
   TEST_ASSERT_EQUAL_INT(0, s_log_n); /* nothing written while held */
   TEST_ASSERT_EQUAL_INT(SUCCESS, conv_turn_lease_release(conv, mine));
   pthread_join(t.th, NULL);
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_GRANTED, t.rc);
   TEST_ASSERT_EQUAL_INT(4, s_log_n);
   TEST_ASSERT_EQUAL_INT(1, s_log[0]);
   TEST_ASSERT_EQUAL_INT(2, s_log[1]);
   TEST_ASSERT_EQUAL_INT(3, s_log[2]);
   TEST_ASSERT_EQUAL_INT(99, s_log[3]);
   /* Every write ran with the releaser still holding the lease. */
   TEST_ASSERT_EQUAL_INT(0, atomic_load(&s_wrong_holder));
}

static void test_claim_new_never_waits(void) {
   const int64_t conv = 111;
   const uint64_t a = conv_turn_lease_new_token();
   const uint64_t b = conv_turn_lease_new_token();
   TEST_ASSERT_TRUE(conv_turn_lease_claim_new(conv, 1, a));
   TEST_ASSERT_FALSE(conv_turn_lease_claim_new(conv, 2, b));
   TEST_ASSERT_EQUAL_INT(SUCCESS, conv_turn_lease_release(conv, a));
   TEST_ASSERT_TRUE(conv_turn_lease_claim_new(conv, 2, b));
   TEST_ASSERT_EQUAL_INT(SUCCESS, conv_turn_lease_release(conv, b));
   TEST_ASSERT_FALSE(conv_turn_lease_claim_new(0, 1, a));
}

static void test_try_acquire_never_waits(void) {
   const int64_t conv = 119;
   const uint64_t a = conv_turn_lease_new_token();
   const uint64_t b = conv_turn_lease_new_token();
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_GRANTED, conv_turn_lease_try_acquire(conv, 1, a));
   /* Held: another token, and the holder's own (a try never re-enters). */
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_BUSY, conv_turn_lease_try_acquire(conv, 2, b));
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_BUSY, conv_turn_lease_try_acquire(conv, 1, a));
   TEST_ASSERT_EQUAL_INT(0, conv_turn_lease_waiters(conv));

   /* A queued turn gets the lease at the release; a try then still finds it held. */
   turn_t t;
   start_turn(&t, conv, 3, 70);
   TEST_ASSERT_TRUE(wait_for_waiters(conv, 1));
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_BUSY, conv_turn_lease_try_acquire(conv, 2, b));
   TEST_ASSERT_EQUAL_INT(SUCCESS, conv_turn_lease_release(conv, a));
   pthread_join(t.th, NULL);
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_GRANTED, t.rc);
   TEST_ASSERT_EQUAL_INT(1, s_log_n);
   TEST_ASSERT_EQUAL_INT(70, s_log[0]);

   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_GRANTED, conv_turn_lease_try_acquire(conv, 2, b));
   TEST_ASSERT_EQUAL_INT(SUCCESS, conv_turn_lease_release(conv, b));
   TEST_ASSERT_FALSE(conv_turn_lease_holder(conv, NULL, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_FAILURE, conv_turn_lease_try_acquire(0, 1, a));
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_FAILURE, conv_turn_lease_try_acquire(conv, 1, 0));
}

static void test_holder_query(void) {
   const int64_t conv = 112;
   const uint64_t mine = conv_turn_lease_new_token();
   const time_t before = time(NULL);
   TEST_ASSERT_FALSE(conv_turn_lease_holder(conv, NULL, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_GRANTED,
                         conv_turn_lease_acquire(conv, 42, mine, NULL, NULL));
   uint32_t session = 0;
   uint64_t token = 0;
   time_t since = 0;
   TEST_ASSERT_TRUE(conv_turn_lease_holder(conv, &session, &token, &since));
   TEST_ASSERT_EQUAL_UINT32(42, session);
   TEST_ASSERT_TRUE(token == mine);
   TEST_ASSERT_TRUE(since >= before);
   conv_turn_lease_release(conv, mine);
}

static void test_the_table_at_capacity(void) {
   static uint64_t tokens[CONV_TURN_LEASE_SLOTS];
   const int64_t base = 10000;
   for (int i = 0; i < CONV_TURN_LEASE_SLOTS; i++) {
      tokens[i] = conv_turn_lease_new_token();
      TEST_ASSERT_TRUE(conv_turn_lease_claim_new(base + i, 1, tokens[i]));
   }
   const int64_t extra = base + CONV_TURN_LEASE_SLOTS;
   const uint64_t t = conv_turn_lease_new_token();
   TEST_ASSERT_FALSE(conv_turn_lease_claim_new(extra, 1, t));
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_FULL, conv_turn_lease_acquire(extra, 1, t, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_FULL, conv_turn_lease_try_acquire(extra, 1, t));
   /* A post with no entry to hold is still written. */
   test_post_t p;
   post_init(&p, 1, extra, 0);
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_WRITTEN, conv_turn_lease_post_or_defer(extra, &p.link));
   TEST_ASSERT_EQUAL_INT(1, s_log_n);
   for (int i = 0; i < CONV_TURN_LEASE_SLOTS; i++)
      TEST_ASSERT_EQUAL_INT(SUCCESS, conv_turn_lease_release(base + i, tokens[i]));
   /* Freed entries are usable again. */
   TEST_ASSERT_TRUE(conv_turn_lease_claim_new(extra, 1, t));
   conv_turn_lease_release(extra, t);
}

static void test_release_session_frees_what_a_session_holds(void) {
   const uint32_t session = 77;
   const uint64_t a = conv_turn_lease_new_token();
   const uint64_t b = conv_turn_lease_new_token();
   TEST_ASSERT_TRUE(conv_turn_lease_claim_new(113, session, a));
   TEST_ASSERT_TRUE(conv_turn_lease_claim_new(114, session, b));
   test_post_t p;
   post_init(&p, 1, 113, a);
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_DEFERRED, conv_turn_lease_post_or_defer(113, &p.link));
   turn_t t;
   start_turn(&t, 113, 2, 50);
   TEST_ASSERT_TRUE(wait_for_waiters(113, 1));

   TEST_ASSERT_EQUAL_INT(2, conv_turn_lease_release_session(session));
   pthread_join(t.th, NULL);
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_GRANTED, t.rc);
   TEST_ASSERT_EQUAL_INT(2, s_log_n);
   TEST_ASSERT_EQUAL_INT(1, s_log[0]); /* the queued post before the waiter */
   TEST_ASSERT_EQUAL_INT(50, s_log[1]);
   TEST_ASSERT_FALSE(conv_turn_lease_holder(114, NULL, NULL, NULL));
   TEST_ASSERT_EQUAL_INT(0, conv_turn_lease_release_session(session));
}

typedef struct {
   int64_t conv;
   uint64_t token;
   int rc;
} release_arg_t;

static void *release_thread(void *arg) {
   release_arg_t *a = arg;
   a->rc = conv_turn_lease_release(a->conv, a->token);
   return NULL;
}

/* The safety net leaves a lease alone while its holder's release writes the
 * queued posts: the waiter gets it once, after them. */
static void test_release_session_leaves_a_release_in_progress(void) {
   const int64_t conv = 120;
   const uint32_t session = 78;
   const uint64_t mine = conv_turn_lease_new_token();
   TEST_ASSERT_TRUE(conv_turn_lease_claim_new(conv, session, mine));
   sem_t entered, resume;
   sem_init(&entered, 0, 0);
   sem_init(&resume, 0, 0);
   test_post_t p;
   post_init(&p, 1, conv, mine);
   p.entered = &entered;
   p.resume = &resume;
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_DEFERRED, conv_turn_lease_post_or_defer(conv, &p.link));
   turn_t t;
   start_turn(&t, conv, 3, 51);
   TEST_ASSERT_TRUE(wait_for_waiters(conv, 1));

   release_arg_t ra = { .conv = conv, .token = mine };
   pthread_t rt;
   pthread_create(&rt, NULL, release_thread, &ra);
   sem_wait(&entered); /* the release is writing the post, its mutex dropped */
   TEST_ASSERT_EQUAL_INT(0, conv_turn_lease_release_session(session));
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_FAILURE, conv_turn_lease_release(conv, mine));
   TEST_ASSERT_EQUAL_INT(1, conv_turn_lease_waiters(conv));
   sem_post(&resume);
   pthread_join(rt, NULL);
   pthread_join(t.th, NULL);
   TEST_ASSERT_EQUAL_INT(SUCCESS, ra.rc);
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_GRANTED, t.rc);
   TEST_ASSERT_EQUAL_INT(2, s_log_n);
   TEST_ASSERT_EQUAL_INT(1, s_log[0]);
   TEST_ASSERT_EQUAL_INT(51, s_log[1]);
   TEST_ASSERT_EQUAL_INT(0, atomic_load(&s_wrong_holder));
   TEST_ASSERT_FALSE(conv_turn_lease_holder(conv, NULL, NULL, NULL));
   sem_destroy(&entered);
   sem_destroy(&resume);
}

/* A waiter whose stop check was already running when it was told to stop. */
typedef struct {
   int64_t conv;
   uint64_t token;
   atomic_bool stop;
   atomic_int calls;
   sem_t in_check;  /* posted when the first check starts */
   sem_t let_check; /* the first check returns once posted */
   atomic_bool done;
   int rc;
} wake_turn_t;

static bool wake_should_stop(void *ctx) {
   wake_turn_t *w = ctx;
   const bool stop = atomic_load(&w->stop);
   if (atomic_fetch_add(&w->calls, 1) == 0) {
      sem_post(&w->in_check);
      sem_wait(&w->let_check);
   }
   return stop;
}

static void *wake_turn_thread(void *arg) {
   wake_turn_t *w = arg;
   w->rc = conv_turn_lease_acquire(w->conv, 79, w->token, wake_should_stop, w);
   atomic_store(&w->done, true);
   return NULL;
}

/* Woken, a waiter asks its stop check again at once instead of sleeping out
 * the wait slice. */
static void test_a_woken_waiter_stops_without_waiting_out_its_slice(void) {
   const int64_t conv = 121;
   const uint64_t mine = conv_turn_lease_new_token();
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_GRANTED,
                         conv_turn_lease_acquire(conv, 1, mine, NULL, NULL));
   wake_turn_t w;
   memset(&w, 0, sizeof(w));
   w.conv = conv;
   w.token = conv_turn_lease_new_token();
   sem_init(&w.in_check, 0, 0);
   sem_init(&w.let_check, 0, 0);
   pthread_t th;
   pthread_create(&th, NULL, wake_turn_thread, &w);
   sem_wait(&w.in_check); /* its first check read "go on" and hasn't returned */
   atomic_store(&w.stop, true);
   conv_turn_lease_wake_session(79);
   sem_post(&w.let_check);
   /* Without the wake it would sleep a whole slice before checking again. */
   int waited = 0;
   while (!atomic_load(&w.done) && waited < CONV_TURN_LEASE_WAIT_SLICE_MS / 2) {
      pause_1ms();
      waited++;
   }
   TEST_ASSERT_TRUE(atomic_load(&w.done));
   pthread_join(th, NULL);
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_STOPPED, w.rc);
   TEST_ASSERT_EQUAL_INT(2, atomic_load(&w.calls));
   TEST_ASSERT_EQUAL_INT(0, conv_turn_lease_waiters(conv));
   TEST_ASSERT_EQUAL_INT(SUCCESS, conv_turn_lease_release(conv, mine));
   sem_destroy(&w.in_check);
   sem_destroy(&w.let_check);
}

static void test_no_writer_refuses_a_post(void) {
   conv_turn_lease_set_writer(NULL, test_drop);
   test_post_t p;
   post_init(&p, 1, 115, 0);
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_FAILURE, conv_turn_lease_post_or_defer(115, &p.link));
   conv_turn_lease_set_writer(test_write, test_drop);
}

static void test_queued_posts_drop_when_the_writer_is_cleared(void) {
   const int64_t conv = 116;
   const uint64_t mine = conv_turn_lease_new_token();
   TEST_ASSERT_TRUE(conv_turn_lease_claim_new(conv, 1, mine));
   test_post_t p;
   post_init(&p, 1, conv, 0);
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_DEFERRED, conv_turn_lease_post_or_defer(conv, &p.link));
   conv_turn_lease_set_writer(NULL, test_drop);
   conv_turn_lease_release(conv, mine);
   TEST_ASSERT_EQUAL_INT(1, atomic_load(&s_dropped));
   TEST_ASSERT_EQUAL_INT(0, s_log_n);
   conv_turn_lease_set_writer(test_write, test_drop);
}

/* Last: shutdown can't be undone. */
static void test_shutdown_wakes_waiters_and_writes_queued_posts(void) {
   const int64_t conv = 117;
   const uint64_t mine = conv_turn_lease_new_token();
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_GRANTED,
                         conv_turn_lease_acquire(conv, 1, mine, NULL, NULL));
   turn_t t[2];
   for (int i = 0; i < 2; i++) {
      start_turn(&t[i], conv, (uint32_t)(20 + i), 60 + i);
      TEST_ASSERT_TRUE(wait_for_waiters(conv, i + 1));
   }
   test_post_t p;
   post_init(&p, 1, conv, 0);
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_DEFERRED, conv_turn_lease_post_or_defer(conv, &p.link));

   conv_turn_lease_shutdown();
   for (int i = 0; i < 2; i++) {
      pthread_join(t[i].th, NULL);
      TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_STOPPED, t[i].rc);
   }
   TEST_ASSERT_EQUAL_INT(1, s_log_n); /* written although the holder still runs */
   TEST_ASSERT_EQUAL_INT(1, s_log[0]);

   const uint64_t later = conv_turn_lease_new_token();
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_STOPPED,
                         conv_turn_lease_acquire(118, 3, later, NULL, NULL));
   TEST_ASSERT_FALSE(conv_turn_lease_claim_new(118, 3, later));
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_STOPPED, conv_turn_lease_try_acquire(118, 3, later));
   test_post_t after;
   post_init(&after, 2, conv, 0);
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_WRITTEN, conv_turn_lease_post_or_defer(conv, &after.link));
   TEST_ASSERT_EQUAL_INT(2, s_log_n);
   /* The holder still releases normally. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, conv_turn_lease_release(conv, mine));
   TEST_ASSERT_FALSE(conv_turn_lease_holder(conv, NULL, NULL, NULL));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_waiters_are_served_in_order);
   RUN_TEST(test_reentry_is_counted);
   RUN_TEST(test_should_stop_ends_a_wait);
   RUN_TEST(test_one_conversation_does_not_block_another);
   RUN_TEST(test_a_holder_of_one_never_waits_for_another);
   RUN_TEST(test_a_post_into_a_free_conversation_is_written_now);
   RUN_TEST(test_a_failed_immediate_write_is_reported);
   RUN_TEST(test_a_free_post_is_written_under_the_lease);
   RUN_TEST(test_posts_are_deferred_and_written_before_the_next_holder);
   RUN_TEST(test_claim_new_never_waits);
   RUN_TEST(test_try_acquire_never_waits);
   RUN_TEST(test_holder_query);
   RUN_TEST(test_the_table_at_capacity);
   RUN_TEST(test_release_session_frees_what_a_session_holds);
   RUN_TEST(test_release_session_leaves_a_release_in_progress);
   RUN_TEST(test_a_woken_waiter_stops_without_waiting_out_its_slice);
   RUN_TEST(test_no_writer_refuses_a_post);
   RUN_TEST(test_queued_posts_drop_when_the_writer_is_cleared);
   RUN_TEST(test_shutdown_wakes_waiters_and_writes_queued_posts);
   return UNITY_END();
}
