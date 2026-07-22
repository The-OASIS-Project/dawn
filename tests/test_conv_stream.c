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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Unit tests for the conversation-keyed live-partial replay ring (conv_stream):
 * begin/append/dup/end, stale-turn rejection, implicit begin, truncation cap,
 * grace-window eviction, LRU reclaim, and clear/reset.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/conv_stream.h"
#include "unity.h"

void setUp(void) {
   conv_stream_reset();
}

void tearDown(void) {
   conv_stream_reset();
}

/* ── begin → append → dup ──────────────────────────────────────────────────── */

static void test_begin_append_dup(void) {
   conv_stream_begin(42, 1);
   conv_stream_append(42, 1, "Hello, ");
   conv_stream_append(42, 1, "world");

   bool active = false, trunc = true;
   uint32_t sid = 0;
   char *p = conv_stream_dup_partial(42, &active, &sid, &trunc);
   TEST_ASSERT_NOT_NULL(p);
   TEST_ASSERT_EQUAL_STRING("Hello, world", p);
   TEST_ASSERT_TRUE(active);
   TEST_ASSERT_EQUAL_UINT32(1, sid);
   TEST_ASSERT_FALSE(trunc);
   free(p);
}

/* ── unknown conversation → NULL ───────────────────────────────────────────── */

static void test_dup_absent_is_null(void) {
   TEST_ASSERT_NULL(conv_stream_dup_partial(999, NULL, NULL, NULL));
}

/* ── stale/superseded turn id is ignored ───────────────────────────────────── */

static void test_stale_stream_id_ignored(void) {
   conv_stream_begin(7, 5);
   conv_stream_append(7, 5, "keep");
   conv_stream_append(7, 4, "DROP"); /* older turn */
   conv_stream_append(7, 5, "-me");

   char *p = conv_stream_dup_partial(7, NULL, NULL, NULL);
   TEST_ASSERT_EQUAL_STRING("keep-me", p);
   free(p);
}

/* ── a new begin resets the partial ────────────────────────────────────────── */

static void test_begin_resets_partial(void) {
   conv_stream_begin(3, 1);
   conv_stream_append(3, 1, "old turn");
   conv_stream_begin(3, 2); /* new turn */
   conv_stream_append(3, 2, "new");

   char *p = conv_stream_dup_partial(3, NULL, NULL, NULL);
   TEST_ASSERT_EQUAL_STRING("new", p);
   free(p);
}

/* ── append with no explicit begin implicitly starts a turn ────────────────── */

static void test_implicit_begin(void) {
   conv_stream_append(11, 9, "implicit");
   bool active = false;
   char *p = conv_stream_dup_partial(11, &active, NULL, NULL);
   TEST_ASSERT_EQUAL_STRING("implicit", p);
   TEST_ASSERT_TRUE(active);
   free(p);
}

/* ── end marks inactive but keeps the partial (grace) ──────────────────────── */

static void test_end_keeps_partial(void) {
   conv_stream_begin(8, 1);
   conv_stream_append(8, 1, "done text");
   conv_stream_end(8, 1);

   bool active = true;
   char *p = conv_stream_dup_partial(8, &active, NULL, NULL);
   TEST_ASSERT_EQUAL_STRING("done text", p);
   TEST_ASSERT_FALSE(active); /* finalized */
   free(p);
}

/* ── truncation at the per-turn cap ────────────────────────────────────────── */

static void test_truncation_cap(void) {
   size_t big = CONV_STREAM_MAX_PARTIAL_BYTES + 5000;
   char *huge = malloc(big + 1);
   TEST_ASSERT_NOT_NULL(huge);
   memset(huge, 'A', big);
   huge[big] = '\0';

   conv_stream_begin(50, 1);
   conv_stream_append(50, 1, huge);
   free(huge);

   bool trunc = false;
   char *p = conv_stream_dup_partial(50, NULL, NULL, &trunc);
   TEST_ASSERT_NOT_NULL(p);
   TEST_ASSERT_TRUE(trunc);
   TEST_ASSERT_EQUAL_size_t(CONV_STREAM_MAX_PARTIAL_BYTES, strlen(p));
   /* Further appends are dropped once truncated. */
   conv_stream_append(50, 1, "more");
   char *p2 = conv_stream_dup_partial(50, NULL, NULL, NULL);
   TEST_ASSERT_EQUAL_size_t(CONV_STREAM_MAX_PARTIAL_BYTES, strlen(p2));
   free(p);
   free(p2);
}

/* ── clear drops a conversation ────────────────────────────────────────────── */

static void test_clear(void) {
   conv_stream_begin(21, 1);
   conv_stream_append(21, 1, "x");
   conv_stream_clear(21);
   TEST_ASSERT_NULL(conv_stream_dup_partial(21, NULL, NULL, NULL));
}

/* ── evict_stale drops finalized+aged, keeps active/recent ─────────────────── */

static void test_evict_stale(void) {
   time_t now = time(NULL);

   conv_stream_begin(100, 1);
   conv_stream_append(100, 1, "finalized");
   conv_stream_end(100, 1); /* updated_at ~= now */

   conv_stream_begin(101, 1);
   conv_stream_append(101, 1, "still active"); /* active — never evicted */

   /* Far in the future: the finalized one ages out, the active one stays. */
   conv_stream_evict_stale(now + CONV_STREAM_EVICT_GRACE_SEC + 10);

   TEST_ASSERT_NULL(conv_stream_dup_partial(100, NULL, NULL, NULL));
   char *p = conv_stream_dup_partial(101, NULL, NULL, NULL);
   TEST_ASSERT_EQUAL_STRING("still active", p);
   free(p);
}

/* ── finalized-but-within-grace is retained ────────────────────────────────── */

static void test_evict_respects_grace(void) {
   time_t now = time(NULL);
   conv_stream_begin(120, 1);
   conv_stream_append(120, 1, "recent");
   conv_stream_end(120, 1);

   conv_stream_evict_stale(now + 1); /* within grace */
   char *p = conv_stream_dup_partial(120, NULL, NULL, NULL);
   TEST_ASSERT_EQUAL_STRING("recent", p);
   free(p);
}

/* ── a newer stream_id supersedes even while the prior turn is still active ─── */

static void test_newer_stream_supersedes_active(void) {
   conv_stream_begin(300, 1);
   conv_stream_append(300, 1, "old"); /* no end -> still active */
   conv_stream_append(300, 2, "new"); /* newer turn supersedes */

   bool active = false;
   uint32_t sid = 0;
   char *p = conv_stream_dup_partial(300, &active, &sid, NULL);
   TEST_ASSERT_EQUAL_STRING("new", p);
   TEST_ASSERT_TRUE(active);
   TEST_ASSERT_EQUAL_UINT32(2, sid);
   free(p);
}

/* ── an abandoned (never-ended) active turn is reclaimed by the backstop ────── */

static void test_abandoned_active_evicted(void) {
   time_t now = time(NULL);
   conv_stream_begin(310, 1);
   conv_stream_append(310, 1, "stuck"); /* active, no end */

   /* Within the active max-age it survives; past it, the backstop reclaims it. */
   conv_stream_evict_stale(now + CONV_STREAM_ACTIVE_MAX_AGE_SEC - 1);
   TEST_ASSERT_NOT_NULL(conv_stream_dup_partial(310, NULL, NULL, NULL));
   conv_stream_evict_stale(now + CONV_STREAM_ACTIVE_MAX_AGE_SEC + 1);
   TEST_ASSERT_NULL(conv_stream_dup_partial(310, NULL, NULL, NULL));
}

/* ── LRU reclaim past the slot table capacity ──────────────────────────────── */

static void test_lru_reclaim(void) {
   /* Fill one more than the table holds. */
   for (int i = 1; i <= CONV_STREAM_MAX_SLOTS + 1; i++) {
      conv_stream_begin(i, 1);
      conv_stream_append(i, 1, "x");
   }

   int present = 0;
   for (int i = 1; i <= CONV_STREAM_MAX_SLOTS + 1; i++) {
      char *p = conv_stream_dup_partial(i, NULL, NULL, NULL);
      if (p != NULL) {
         present++;
         free(p);
      }
   }
   /* Exactly the table's worth survive; the newest must be one of them. */
   TEST_ASSERT_EQUAL_INT(CONV_STREAM_MAX_SLOTS, present);
   char *newest = conv_stream_dup_partial(CONV_STREAM_MAX_SLOTS + 1, NULL, NULL, NULL);
   TEST_ASSERT_NOT_NULL(newest);
   free(newest);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_begin_append_dup);
   RUN_TEST(test_dup_absent_is_null);
   RUN_TEST(test_stale_stream_id_ignored);
   RUN_TEST(test_begin_resets_partial);
   RUN_TEST(test_implicit_begin);
   RUN_TEST(test_end_keeps_partial);
   RUN_TEST(test_truncation_cap);
   RUN_TEST(test_clear);
   RUN_TEST(test_evict_stale);
   RUN_TEST(test_evict_respects_grace);
   RUN_TEST(test_newer_stream_supersedes_active);
   RUN_TEST(test_abandoned_active_evicted);
   RUN_TEST(test_lru_reclaim);
   return UNITY_END();
}
