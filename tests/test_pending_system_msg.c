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
 * Unit tests for the deferred system-message queue (circular buffer, FIFO,
 * overflow eviction, truncation, output-buffer clamping).
 */

#include <stdio.h> /* snprintf */
#include <string.h>

#include "core/pending_system_msg.h"
#include "unity.h"

void setUp(void) {
   pending_sysmsg_clear();
}

void tearDown(void) {
}

/* Helper: drain and count remaining items. */
static int drain_count(void) {
   char buf[PENDING_SYSMSG_MAX_TEXT + 1];
   int n = 0;
   while (pending_sysmsg_pop(buf, sizeof(buf))) {
      n++;
   }
   return n;
}

/* ── Empty Queue ────────────────────────────────────────────────────────── */

static void test_empty_queue(void) {
   char buf[PENDING_SYSMSG_MAX_TEXT + 1];
   TEST_ASSERT_EQUAL_INT(0, pending_sysmsg_has_item());
   TEST_ASSERT_EQUAL_INT(0, pending_sysmsg_pop(buf, sizeof(buf)));
}

/* ── Push and Pop ───────────────────────────────────────────────────────── */

static void test_push_and_pop(void) {
   char buf[PENDING_SYSMSG_MAX_TEXT + 1];

   TEST_ASSERT_EQUAL_INT(1, pending_sysmsg_push("Incoming call from Alice"));
   TEST_ASSERT_EQUAL_INT(1, pending_sysmsg_has_item());

   TEST_ASSERT_EQUAL_INT(1, pending_sysmsg_pop(buf, sizeof(buf)));
   TEST_ASSERT_EQUAL_STRING("Incoming call from Alice", buf);

   TEST_ASSERT_EQUAL_INT(0, pending_sysmsg_has_item());
}

/* ── FIFO Order ─────────────────────────────────────────────────────────── */

static void test_fifo_order(void) {
   char buf[PENDING_SYSMSG_MAX_TEXT + 1];

   pending_sysmsg_push("first");
   pending_sysmsg_push("second");
   pending_sysmsg_push("third");

   pending_sysmsg_pop(buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("first", buf);
   pending_sysmsg_pop(buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("second", buf);
   pending_sysmsg_pop(buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("third", buf);
}

/* ── Full Queue Eviction (drop oldest) ──────────────────────────────────── */

static void test_full_queue_eviction(void) {
   char buf[PENDING_SYSMSG_MAX_TEXT + 1];
   char item[32];

   for (int i = 0; i < PENDING_SYSMSG_MAX_ITEMS; i++) {
      snprintf(item, sizeof(item), "item_%d", i);
      TEST_ASSERT_EQUAL_INT(1, pending_sysmsg_push(item));
   }

   /* One more than capacity — oldest (item_0) is evicted. */
   TEST_ASSERT_EQUAL_INT(1, pending_sysmsg_push("overflow"));

   /* Still exactly MAX_ITEMS queued, and the head is now item_1. */
   TEST_ASSERT_EQUAL_INT(1, pending_sysmsg_pop(buf, sizeof(buf)));
   TEST_ASSERT_EQUAL_STRING("item_1", buf);
   TEST_ASSERT_EQUAL_INT(PENDING_SYSMSG_MAX_ITEMS - 1, drain_count());
}

/* ── Clear ──────────────────────────────────────────────────────────────── */

static void test_clear(void) {
   pending_sysmsg_push("a");
   pending_sysmsg_push("b");
   TEST_ASSERT_EQUAL_INT(1, pending_sysmsg_has_item());

   pending_sysmsg_clear();
   TEST_ASSERT_EQUAL_INT(0, pending_sysmsg_has_item());
}

/* ── Null / Empty Push ──────────────────────────────────────────────────── */

static void test_push_null_or_empty(void) {
   TEST_ASSERT_EQUAL_INT(0, pending_sysmsg_push(NULL));
   TEST_ASSERT_EQUAL_INT(0, pending_sysmsg_push(""));
   TEST_ASSERT_EQUAL_INT(0, pending_sysmsg_has_item());
}

/* ── Null / Zero-size Pop ───────────────────────────────────────────────── */

static void test_pop_null_or_zero(void) {
   char buf[PENDING_SYSMSG_MAX_TEXT + 1];
   pending_sysmsg_push("something");

   TEST_ASSERT_EQUAL_INT(0, pending_sysmsg_pop(NULL, sizeof(buf)));
   TEST_ASSERT_EQUAL_INT(0, pending_sysmsg_pop(buf, 0));
   /* The item is untouched by the rejected pops. */
   TEST_ASSERT_EQUAL_INT(1, pending_sysmsg_has_item());
}

/* ── Storage Truncation (message longer than MAX_TEXT) ──────────────────── */

static void test_push_truncation(void) {
   char long_text[PENDING_SYSMSG_MAX_TEXT + 100];
   char buf[PENDING_SYSMSG_MAX_TEXT + 1];

   memset(long_text, 'A', sizeof(long_text) - 1);
   long_text[sizeof(long_text) - 1] = '\0';

   TEST_ASSERT_EQUAL_INT(1, pending_sysmsg_push(long_text));

   /* A full-width drain buffer round-trips exactly MAX_TEXT chars (pins the
    * storage-vs-drain width agreement). */
   TEST_ASSERT_EQUAL_INT(1, pending_sysmsg_pop(buf, sizeof(buf)));
   TEST_ASSERT_EQUAL_size_t(PENDING_SYSMSG_MAX_TEXT, strlen(buf));
   TEST_ASSERT_EQUAL_CHAR('A', buf[0]);
   TEST_ASSERT_EQUAL_CHAR('A', buf[PENDING_SYSMSG_MAX_TEXT - 1]);
   TEST_ASSERT_EQUAL_CHAR('\0', buf[PENDING_SYSMSG_MAX_TEXT]);
}

/* ── Output-buffer Clamping (small consumer buffer) ─────────────────────── */

static void test_pop_clamps_to_out_size(void) {
   char small[3]; /* room for 2 chars + NUL */

   pending_sysmsg_push("hello");
   TEST_ASSERT_EQUAL_INT(1, pending_sysmsg_pop(small, sizeof(small)));
   TEST_ASSERT_EQUAL_STRING("he", small);
   TEST_ASSERT_EQUAL_CHAR('\0', small[2]);
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_empty_queue);
   RUN_TEST(test_push_and_pop);
   RUN_TEST(test_fifo_order);
   RUN_TEST(test_full_queue_eviction);
   RUN_TEST(test_clear);
   RUN_TEST(test_push_null_or_empty);
   RUN_TEST(test_pop_null_or_zero);
   RUN_TEST(test_push_truncation);
   RUN_TEST(test_pop_clamps_to_out_size);
   return UNITY_END();
}
