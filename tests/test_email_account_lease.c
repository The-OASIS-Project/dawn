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
 * The per-account email lease: FIFO handover between blocking threads and
 * asynchronous tickets, cancel, timeout, the release hook, and refusing a
 * re-take by the holder.  Built with NDEBUG so the re-take returns instead of
 * asserting.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tools/email_account_lease.h"
#include "unity.h"

/* Each test uses its own account ids, so a failure can't leave a lease held
 * under the next test. */

static double now_s(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_ms(int ms) {
   usleep((useconds_t)ms * 1000);
}

/* ---- the hook: records grants in order ---- */

static pthread_mutex_t s_rec_mutex = PTHREAD_MUTEX_INITIALIZER;
static email_lease_ticket_t *s_granted[8];
static int s_granted_n;
static atomic_int s_hook_sleep_ms;
static atomic_bool s_hook_done;

static void record_hook(email_lease_ticket_t *t) {
   int ms = atomic_load(&s_hook_sleep_ms);
   if (ms > 0)
      sleep_ms(ms);
   pthread_mutex_lock(&s_rec_mutex);
   if (s_granted_n < 8)
      s_granted[s_granted_n++] = t;
   pthread_mutex_unlock(&s_rec_mutex);
   atomic_store(&s_hook_done, true);
}

static int granted_count(void) {
   pthread_mutex_lock(&s_rec_mutex);
   int n = s_granted_n;
   pthread_mutex_unlock(&s_rec_mutex);
   return n;
}

void setUp(void) {
   s_granted_n = 0;
   atomic_store(&s_hook_sleep_ms, 0);
   atomic_store(&s_hook_done, false);
   email_lease_set_hook(record_hook);
}

void tearDown(void) {
   email_lease_clear_hook();
}

/* ---- a blocking acquirer on its own thread ---- */

typedef struct {
   int64_t id;
   int timeout_s;
   atomic_bool cancel;
   int hold_ms;
   int rc;
   double got_at;
   atomic_bool done;
} acquirer_t;

static void *acquire_thread(void *arg) {
   acquirer_t *a = arg;
   a->rc = email_lease_acquire(a->id, &a->cancel, a->timeout_s);
   a->got_at = now_s();
   if (a->rc == EMAIL_LEASE_OK) {
      if (a->hold_ms > 0)
         sleep_ms(a->hold_ms);
      email_lease_release(a->id);
   }
   atomic_store(&a->done, true);
   return NULL;
}

static void start_acquirer(pthread_t *th, acquirer_t *a, int64_t id, int timeout_s, int hold_ms) {
   memset(a, 0, sizeof(*a));
   a->id = id;
   a->timeout_s = timeout_s;
   a->hold_ms = hold_ms;
   pthread_create(th, NULL, acquire_thread, a);
}

/* ---- tests ---- */

static void test_free_lease_is_taken_and_released(void) {
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_acquire(101, NULL, 1));
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_release(101));
   /* Released: free again, and releasing twice is refused. */
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_FAILURE, email_lease_release(101));
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_acquire(101, NULL, 1));
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_release(101));
}

static void test_a_waiter_gets_the_lease_on_release(void) {
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_acquire(102, NULL, 1));
   pthread_t th;
   acquirer_t a;
   start_acquirer(&th, &a, 102, 5, 0);
   sleep_ms(100);
   TEST_ASSERT_FALSE(atomic_load(&a.done)); /* still waiting */
   const double released_at = now_s();
   email_lease_release(102);
   pthread_join(th, NULL);
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, a.rc);
   TEST_ASSERT_TRUE(a.got_at >= released_at);
}

static void test_waiters_are_served_in_order(void) {
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_acquire(103, NULL, 1));
   pthread_t th;
   acquirer_t a;
   start_acquirer(&th, &a, 103, 5, 0);
   sleep_ms(50); /* the thread queues first */
   email_lease_ticket_t ticket = { 0 };
   bool got = true;
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_acquire_async(103, &ticket, &got));
   TEST_ASSERT_FALSE(got);
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_TICKET_WAITING, ticket.state);

   email_lease_release(103);
   pthread_join(th, NULL); /* the thread had it first, then released it to the ticket */
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, a.rc);
   TEST_ASSERT_EQUAL_INT(1, granted_count());
   TEST_ASSERT_EQUAL_PTR(&ticket, s_granted[0]);
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_TICKET_GRANTED, ticket.state);
   /* The ticket holds it now: a ticket's lease may be released from any thread. */
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_release(103));
}

static void test_a_cancelled_waiter_gives_up_within_a_slice(void) {
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_acquire(104, NULL, 1));
   pthread_t th;
   acquirer_t a;
   start_acquirer(&th, &a, 104, 30, 0);
   sleep_ms(50);
   const double cancelled_at = now_s();
   atomic_store(&a.cancel, true);
   pthread_join(th, NULL);
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_CANCELLED, a.rc);
   TEST_ASSERT_TRUE(a.got_at - cancelled_at < (EMAIL_LEASE_WAIT_SLICE_MS + 200) / 1000.0);
   /* It left the queue: the next release frees the lease rather than handing it on. */
   email_lease_release(104);
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_acquire(104, NULL, 0));
   email_lease_release(104);
}

static void test_a_waiter_times_out(void) {
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_acquire(105, NULL, 1));
   pthread_t th;
   acquirer_t a;
   const double start = now_s();
   start_acquirer(&th, &a, 105, 1, 0);
   pthread_join(th, NULL);
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_TIMEOUT, a.rc);
   TEST_ASSERT_TRUE(a.got_at - start >= 0.9);
   /* No wait at all when the timeout is zero. */
   const double start0 = now_s();
   start_acquirer(&th, &a, 105, 0, 0);
   pthread_join(th, NULL);
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_TIMEOUT, a.rc);
   TEST_ASSERT_TRUE(a.got_at - start0 < 0.2);
   email_lease_release(105);
}

static void test_a_ticket_takes_a_free_lease_at_once(void) {
   email_lease_ticket_t ticket = { 0 };
   bool got = false;
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_acquire_async(106, &ticket, &got));
   TEST_ASSERT_TRUE(got);
   TEST_ASSERT_EQUAL_INT(0, granted_count()); /* granted now, not through the hook */
   /* A ticket that isn't idle can't be queued again. */
   bool got2 = false;
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_FAILURE, email_lease_acquire_async(106, &ticket, &got2));
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_release(106));
}

static void test_a_released_ticket_can_be_used_again(void) {
   email_lease_ticket_t ticket = { 0 };
   bool got = false;
   TEST_ASSERT_FALSE(email_lease_is_held(120));
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_acquire_async(120, &ticket, &got));
   TEST_ASSERT_TRUE(got);
   TEST_ASSERT_TRUE(email_lease_is_held(120));
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_release(120));
   TEST_ASSERT_FALSE(email_lease_is_held(120));
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_TICKET_IDLE, ticket.state);

   /* Again, this time granted through the hook behind a thread holder. */
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_acquire(120, NULL, 0));
   got = false;
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_acquire_async(120, &ticket, &got));
   TEST_ASSERT_FALSE(got);
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_release(120));
   TEST_ASSERT_EQUAL_INT(1, granted_count());
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_TICKET_GRANTED, ticket.state);
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_release(120));
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_TICKET_IDLE, ticket.state);
   TEST_ASSERT_FALSE(email_lease_is_held(120));
}

static void test_cancel_ticket_removes_only_a_waiting_one(void) {
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_acquire(107, NULL, 1));
   email_lease_ticket_t waiting = { 0 };
   email_lease_ticket_t handed = { 0 };
   bool got = false;
   email_lease_acquire_async(107, &waiting, &got);
   email_lease_acquire_async(107, &handed, &got);
   TEST_ASSERT_TRUE(email_lease_cancel_ticket(&waiting));
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_TICKET_IDLE, waiting.state);

   email_lease_release(107); /* hands it to the remaining ticket */
   TEST_ASSERT_EQUAL_INT(1, granted_count());
   TEST_ASSERT_EQUAL_PTR(&handed, s_granted[0]);
   /* A ticket already handed the lease is the hook's to run and release. */
   TEST_ASSERT_FALSE(email_lease_cancel_ticket(&handed));
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_release(107));
}

static void *release_thread(void *arg) {
   email_lease_release(*(int64_t *)arg);
   return NULL;
}

static void test_clear_hook_waits_for_a_running_hook(void) {
   email_lease_ticket_t holder = { 0 };
   bool got = false;
   email_lease_acquire_async(108, &holder, &got);
   TEST_ASSERT_TRUE(got);
   email_lease_ticket_t ticket = { 0 };
   email_lease_acquire_async(108, &ticket, &got);
   TEST_ASSERT_FALSE(got);

   atomic_store(&s_hook_sleep_ms, 300);
   int64_t id = 108;
   pthread_t th;
   pthread_create(&th, NULL, release_thread, &id); /* the hook runs on this thread */
   sleep_ms(100);                                  /* now inside the hook */
   email_lease_clear_hook();
   TEST_ASSERT_TRUE(atomic_load(&s_hook_done)); /* clear returned only after it */
   pthread_join(th, NULL);
   email_lease_release(108);
}

static void test_tickets_wait_for_a_hook(void) {
   email_lease_clear_hook();
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_acquire(109, NULL, 1));
   email_lease_ticket_t ticket = { 0 };
   bool got = false;
   email_lease_acquire_async(109, &ticket, &got);
   email_lease_release(109); /* nobody to tell: the lease stays free, the ticket waits */
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_TICKET_WAITING, ticket.state);
   TEST_ASSERT_EQUAL_INT(0, granted_count());

   email_lease_set_hook(record_hook); /* the free lease goes to it now */
   TEST_ASSERT_EQUAL_INT(1, granted_count());
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_TICKET_GRANTED, ticket.state);
   email_lease_release(109);
}

static void test_the_holder_cannot_take_it_again(void) {
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_acquire(110, NULL, 1));
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_SELF, email_lease_acquire(110, NULL, 1));
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_release(110));
}

static void test_only_the_holding_thread_releases(void) {
   pthread_t th;
   acquirer_t a;
   start_acquirer(&th, &a, 111, 1, 300);
   sleep_ms(100); /* the other thread holds it */
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_FAILURE, email_lease_release(111));
   pthread_join(th, NULL);
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, a.rc);
}

static void test_too_many_accounts_at_once(void) {
   /* Fill every slot, then one more account is refused. */
   int taken = 0;
   for (int64_t id = 1000; id < 1200; id++) {
      int rc = email_lease_acquire(id, NULL, 0);
      if (rc != EMAIL_LEASE_OK) {
         TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_FULL, rc);
         break;
      }
      taken++;
   }
   TEST_ASSERT_TRUE(taken > 0 && taken < 200);
   for (int64_t id = 1000; id < 1000 + taken; id++)
      email_lease_release(id);
   /* Released slots are reused. */
   TEST_ASSERT_EQUAL_INT(EMAIL_LEASE_OK, email_lease_acquire(1000, NULL, 0));
   email_lease_release(1000);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_free_lease_is_taken_and_released);
   RUN_TEST(test_a_waiter_gets_the_lease_on_release);
   RUN_TEST(test_waiters_are_served_in_order);
   RUN_TEST(test_a_cancelled_waiter_gives_up_within_a_slice);
   RUN_TEST(test_a_waiter_times_out);
   RUN_TEST(test_a_ticket_takes_a_free_lease_at_once);
   RUN_TEST(test_a_released_ticket_can_be_used_again);
   RUN_TEST(test_cancel_ticket_removes_only_a_waiting_one);
   RUN_TEST(test_clear_hook_waits_for_a_running_hook);
   RUN_TEST(test_tickets_wait_for_a_hook);
   RUN_TEST(test_the_holder_cannot_take_it_again);
   RUN_TEST(test_only_the_holding_thread_releases);
   RUN_TEST(test_too_many_accounts_at_once);
   return UNITY_END();
}
