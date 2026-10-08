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
 * Searching every account at once (email_fanout): the merge gives what the
 * one-at-a-time search gave, the searches overlap, and each runs under the
 * caller's cancel flag.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tools/email_fanout.h"
#include "tools/email_transfer.h"
#include "unity.h"

void setUp(void) {
}

void tearDown(void) {
}

/* A stub account: how many rows it has, how it fails, how long it takes. */
typedef struct {
   int rows;
   int rc;
   email_err_t err;
   int sleep_ms;
   const atomic_bool *seen_cancel; /* the cancel flag it ran under */
} stub_acct_t;

static void stub_search(void *ctx, int index, email_fanout_slot_t *slot, int max) {
   stub_acct_t *a = &((stub_acct_t *)ctx)[index];
   a->seen_cancel = email_transfer_thread_cancel();
   if (a->sleep_ms)
      usleep((useconds_t)a->sleep_ms * 1000);
   slot->rc = a->rc;
   slot->err = a->err;
   if (a->rc != 0)
      return;
   int n = a->rows < max ? a->rows : max;
   for (int i = 0; i < n; i++)
      snprintf(slot->rows[i].subject, sizeof(slot->rows[i].subject), "a%d-%d", index, i);
   slot->count = n;
}

typedef struct {
   int failed[16];
   int count;
} fails_t;

static void note_fail(void *ctx, int index, const email_fanout_slot_t *slot) {
   (void)slot;
   fails_t *f = ctx;
   f->failed[f->count++] = index;
}

/* The one-at-a-time search: each account asked for what max leaves. */
static bool sequential(stub_acct_t *a,
                       int n,
                       int max,
                       email_summary_t *out,
                       int *total,
                       fails_t *fails) {
   *total = 0;
   for (int i = 0; i < n && *total < max; i++) {
      email_fanout_slot_t slot = { .rows = out + *total };
      stub_search(a, i, &slot, max - *total);
      if (slot.rc == 0)
         *total += slot.count;
      else if (slot.err == EMAIL_ERR_CANCELLED)
         return false;
      else
         note_fail(fails, i, &slot);
   }
   return true;
}

static void check_same(stub_acct_t *a, int n, int max) {
   static email_summary_t seq[64], par[64];
   memset(seq, 0, sizeof(seq));
   memset(par, 0, sizeof(par));
   fails_t sf = { 0 }, pf = { 0 };
   int st = 0, pt = 0;
   const bool sw = sequential(a, n, max, seq, &st, &sf);

   email_fanout_slot_t slots[16];
   TEST_ASSERT_EQUAL_INT(0, email_fanout_run(n, max, stub_search, a, slots));
   const bool pw = email_fanout_merge(slots, n, max, par, &pt, note_fail, &pf);
   email_fanout_free(slots, n);

   TEST_ASSERT_EQUAL(sw, pw);
   TEST_ASSERT_EQUAL_INT(st, pt);
   for (int i = 0; i < st; i++)
      TEST_ASSERT_EQUAL_STRING(seq[i].subject, par[i].subject);
   TEST_ASSERT_EQUAL_INT(sf.count, pf.count);
   for (int i = 0; i < sf.count; i++)
      TEST_ASSERT_EQUAL_INT(sf.failed[i], pf.failed[i]);
}

static void test_merge_matches_sequential(void) {
   /* Fills max partway through: the later accounts' rows are dropped. */
   stub_acct_t a1[] = { { .rows = 3 }, { .rows = 30 }, { .rows = 30 }, { .rows = 5 } };
   check_same(a1, 4, 50);
   /* Nothing fills it: everything, in order. */
   stub_acct_t a2[] = { { .rows = 3 }, { .rows = 0 }, { .rows = 7 } };
   check_same(a2, 3, 50);
   /* A failure within the window is reported; one after max is not. */
   stub_acct_t a3[] = { { .rows = 2 },
                        { .rc = 1, .err = EMAIL_ERR_AUTH_FAILED },
                        { .rows = 60 },
                        { .rc = 1, .err = EMAIL_ERR_TIMEOUT } };
   check_same(a3, 4, 50);
   /* A cancelled account within the window stops the merge. */
   stub_acct_t a4[] = { { .rows = 4 }, { .rc = 1, .err = EMAIL_ERR_CANCELLED }, { .rows = 9 } };
   check_same(a4, 3, 50);
   /* One cancelled after max is reached doesn't. */
   stub_acct_t a5[] = { { .rows = 50 }, { .rc = 1, .err = EMAIL_ERR_CANCELLED } };
   check_same(a5, 2, 50);
}

static void test_searches_overlap_under_callers_cancel(void) {
   atomic_bool stop = false;
   const atomic_bool *prev = email_transfer_scope_cancel(&stop);
   stub_acct_t a[4];
   memset(a, 0, sizeof(a));
   for (int i = 0; i < 4; i++) {
      a[i].rows = 1;
      a[i].sleep_ms = 200;
   }
   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   email_fanout_slot_t slots[4];
   TEST_ASSERT_EQUAL_INT(0, email_fanout_run(4, 50, stub_search, a, slots));
   clock_gettime(CLOCK_MONOTONIC, &t1);
   email_transfer_scope_cancel(prev);
   const long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
   /* Four 200 ms searches one after another would take 800 ms. */
   TEST_ASSERT_LESS_THAN_INT_MESSAGE(600, ms, "the searches ran one after another");
   for (int i = 0; i < 4; i++) {
      TEST_ASSERT_TRUE(a[i].seen_cancel == &stop);
      TEST_ASSERT_TRUE(slots[i].ms >= 150);
   }
   email_fanout_free(slots, 4);
   TEST_ASSERT_NULL(email_transfer_thread_cancel());
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_merge_matches_sequential);
   RUN_TEST(test_searches_overlap_under_callers_cancel);
   return UNITY_END();
}
