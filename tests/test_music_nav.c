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
 * Unit tests for the music play history (music_nav.c): walking back and
 * forward, cutting forward entries, the planned next track, entries no longer
 * queued, a failed track, the queue replace epoch and the size cap.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/music_nav.h"
#include "unity.h"

/* A stand-in queue: paths by position */
#define QUEUE_MAX 64
static const char *s_queue[QUEUE_MAX];
static int s_queue_len;

static void queue_set(const char *const *paths, int n) {
   for (int i = 0; i < n; i++) {
      s_queue[i] = paths[i];
   }
   s_queue_len = n;
}

/* The copy of path nearest hint, as the player's lookup does */
static int find(void *ctx, const char *path, int hint) {
   (void)ctx;
   int best = -1;
   for (int i = 0; i < s_queue_len; i++) {
      if (strcmp(s_queue[i], path) == 0 && (best < 0 || abs(i - hint) < abs(best - hint))) {
         best = i;
      }
   }
   return best;
}

static music_history_t h;

void setUp(void) {
   static const char *const abcd[] = { "a", "b", "c", "d" };
   queue_set(abcd, 4);
   music_history_init(&h);
}

void tearDown(void) {
   music_history_clear(&h);
}

static int back(void) {
   return music_history_step(&h, false, true, find, NULL);
}

static int fwd(void) {
   return music_history_step(&h, true, true, find, NULL);
}

static void test_empty(void) {
   TEST_ASSERT_NULL(music_history_current(&h));
   TEST_ASSERT_EQUAL_INT(-1, back());
   TEST_ASSERT_EQUAL_INT(-1, fwd());
}

static void test_back_and_forward(void) {
   music_history_push(&h, "a", 0);
   music_history_push(&h, "c", 2);
   music_history_push(&h, "b", 1);
   TEST_ASSERT_EQUAL_STRING("b", music_history_current(&h));

   TEST_ASSERT_EQUAL_INT(2, back());  /* c */
   TEST_ASSERT_EQUAL_INT(0, back());  /* a */
   TEST_ASSERT_EQUAL_INT(-1, back()); /* nothing earlier: stays on a */
   TEST_ASSERT_EQUAL_STRING("a", music_history_current(&h));

   TEST_ASSERT_EQUAL_INT(2, fwd());
   TEST_ASSERT_EQUAL_INT(1, fwd());
   TEST_ASSERT_EQUAL_INT(-1, fwd());
   TEST_ASSERT_EQUAL_STRING("b", music_history_current(&h));
}

static void test_push_after_back_cuts_forward(void) {
   music_history_push(&h, "a", 0);
   music_history_push(&h, "b", 1);
   music_history_push(&h, "c", 2);
   back(); /* b */
   music_history_push(&h, "d", 3);
   TEST_ASSERT_EQUAL_INT(-1, fwd()); /* c is gone */
   TEST_ASSERT_EQUAL_INT(1, back());
   TEST_ASSERT_EQUAL_INT(0, back());
}

static void test_plan_is_the_next_entry(void) {
   music_history_push(&h, "a", 0);
   music_history_plan(&h, "c", 2);
   TEST_ASSERT_EQUAL_STRING("a", music_history_current(&h));

   /* A look ahead doesn't move */
   TEST_ASSERT_EQUAL_INT(2, music_history_step(&h, true, false, find, NULL));
   TEST_ASSERT_EQUAL_STRING("a", music_history_current(&h));

   /* A new plan replaces the old one */
   music_history_plan(&h, "d", 3);
   TEST_ASSERT_EQUAL_INT(3, fwd());
   TEST_ASSERT_EQUAL_INT(-1, fwd());
}

static void test_unqueued_entries_are_skipped_and_dropped(void) {
   music_history_push(&h, "a", 0);
   music_history_push(&h, "b", 1);
   music_history_push(&h, "c", 2);

   static const char *const without_b[] = { "a", "c", "d" };
   queue_set(without_b, 3);
   TEST_ASSERT_EQUAL_INT(0, back()); /* b skipped */
   TEST_ASSERT_EQUAL_INT(2, h.len);  /* and dropped */
   TEST_ASSERT_EQUAL_INT(1, fwd());  /* c, now at position 1 */

   static const char *const only_d[] = { "d" };
   queue_set(only_d, 1);
   TEST_ASSERT_EQUAL_INT(-1, back());
}

static void test_nearest_copy(void) {
   static const char *const dup[] = { "x", "y", "x", "z", "x" };
   queue_set(dup, 5);
   music_history_push(&h, "x", 3);
   music_history_push(&h, "z", 3);
   TEST_ASSERT_EQUAL_INT(2, back()); /* the x nearest position 3 (2 before 4) */
}

static void test_failed_track_is_dropped(void) {
   music_history_push(&h, "a", 0);
   music_history_push(&h, "b", 1);
   music_history_drop_current(&h);
   TEST_ASSERT_EQUAL_STRING("a", music_history_current(&h));
   TEST_ASSERT_EQUAL_INT(-1, fwd());
   music_history_drop_current(&h);
   TEST_ASSERT_NULL(music_history_current(&h));
}

static void test_epoch_clears(void) {
   music_history_sync(&h, 0);
   music_history_push(&h, "a", 0);
   music_history_sync(&h, 0);
   TEST_ASSERT_EQUAL_STRING("a", music_history_current(&h));
   music_history_sync(&h, 1); /* the queue was replaced */
   TEST_ASSERT_NULL(music_history_current(&h));
   TEST_ASSERT_EQUAL_INT(0, h.len);
}

static void test_cap_drops_oldest(void) {
   static char names[MUSIC_HISTORY_MAX + 5][8];
   static const char *paths[MUSIC_HISTORY_MAX + 5];
   for (int i = 0; i < MUSIC_HISTORY_MAX + 5; i++) {
      snprintf(names[i], sizeof(names[i]), "t%d", i);
      paths[i] = names[i];
   }
   queue_set(paths, MUSIC_HISTORY_MAX + 5);
   for (int i = 0; i < MUSIC_HISTORY_MAX + 5; i++) {
      music_history_push(&h, paths[i], i);
   }
   TEST_ASSERT_EQUAL_INT(MUSIC_HISTORY_MAX, h.len);
   int steps = 0;
   int last = -1;
   for (int index; (index = back()) >= 0; steps++) {
      last = index;
   }
   TEST_ASSERT_EQUAL_INT(MUSIC_HISTORY_MAX - 1, steps);
   TEST_ASSERT_EQUAL_INT(5, last); /* t0..t4 were dropped */
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_empty);
   RUN_TEST(test_back_and_forward);
   RUN_TEST(test_push_after_back_cuts_forward);
   RUN_TEST(test_plan_is_the_next_entry);
   RUN_TEST(test_unqueued_entries_are_skipped_and_dropped);
   RUN_TEST(test_nearest_copy);
   RUN_TEST(test_failed_track_is_dropped);
   RUN_TEST(test_epoch_clears);
   RUN_TEST(test_cap_drops_oldest);
   return UNITY_END();
}
