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
 * Searching every account at once (email_fanout): the merge keeps the newest
 * rows across accounts, the searches overlap, and each runs under the
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

/* A stub account: its rows' dates (newest first), how it fails, how long it
 * takes. */
typedef struct {
   int rows;
   time_t dates[8];
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
   for (int i = 0; i < n; i++) {
      snprintf(slot->rows[i].subject, sizeof(slot->rows[i].subject), "a%d-%d", index, i);
      slot->rows[i].date = a->dates[i];
   }
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

/* Run the fan-out and merge; the merged subjects joined by spaces into @p got. */
static bool fan_merge(stub_acct_t *a, int n, int max, char *got, size_t got_len, fails_t *fails) {
   email_fanout_slot_t slots[16];
   static email_summary_t out[64];
   int total = 0;
   TEST_ASSERT_EQUAL_INT(0, email_fanout_run(n, max, stub_search, a, slots));
   const bool whole = email_fanout_merge(slots, n, max, out, &total, note_fail, fails);
   email_fanout_free(slots, n);
   got[0] = '\0';
   for (int i = 0; i < total; i++) {
      size_t used = strlen(got);
      snprintf(got + used, got_len - used, "%s%s", i ? " " : "", out[i].subject);
   }
   return whole;
}

static void test_merge_newest_across_accounts(void) {
   char got[512];
   /* Interleaved by date; the limit keeps the newest, whatever the account. */
   stub_acct_t a1[] = { { .rows = 3, .dates = { 90, 50, 10 } },
                        { .rows = 3, .dates = { 100, 60, 55 } } };
   fails_t f = { 0 };
   TEST_ASSERT_TRUE(fan_merge(a1, 2, 4, got, sizeof(got), &f));
   TEST_ASSERT_EQUAL_STRING("a1-0 a0-0 a1-1 a1-2", got);
   TEST_ASSERT_EQUAL_INT(0, f.count);

   /* By date alone: an old row at the head of an account (a re-filed message)
    * doesn't hold back its newer ones; a tie goes to the earlier account. */
   stub_acct_t a2[] = { { .rows = 2, .dates = { 50, 70 } }, { .rows = 1, .dates = { 50 } } };
   memset(&f, 0, sizeof(f));
   TEST_ASSERT_TRUE(fan_merge(a2, 2, 10, got, sizeof(got), &f));
   TEST_ASSERT_EQUAL_STRING("a0-1 a0-0 a1-0", got);

   /* Every failed account is reported, wherever it sits. */
   stub_acct_t a3[] = { { .rows = 2, .dates = { 9, 8 } },
                        { .rc = 1, .err = EMAIL_ERR_AUTH_FAILED },
                        { .rows = 1, .dates = { 7 } },
                        { .rc = 1, .err = EMAIL_ERR_TIMEOUT } };
   memset(&f, 0, sizeof(f));
   TEST_ASSERT_TRUE(fan_merge(a3, 4, 2, got, sizeof(got), &f));
   TEST_ASSERT_EQUAL_STRING("a0-0 a0-1", got);
   TEST_ASSERT_EQUAL_INT(2, f.count);
   TEST_ASSERT_EQUAL_INT(1, f.failed[0]);
   TEST_ASSERT_EQUAL_INT(3, f.failed[1]);

   /* A cancelled account makes the result partial; what was found is kept. */
   stub_acct_t a4[] = { { .rows = 1, .dates = { 5 } }, { .rc = 1, .err = EMAIL_ERR_CANCELLED } };
   memset(&f, 0, sizeof(f));
   TEST_ASSERT_FALSE(fan_merge(a4, 2, 10, got, sizeof(got), &f));
   TEST_ASSERT_EQUAL_STRING("a0-0", got);
   TEST_ASSERT_EQUAL_INT(0, f.count);
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
   RUN_TEST(test_merge_newest_across_accounts);
   RUN_TEST(test_searches_overlap_under_callers_cancel);
   return UNITY_END();
}
