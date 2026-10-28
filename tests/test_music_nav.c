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
 * queued, a failed track, the queue replace epoch and the size cap; and the
 * rule for what plays next (music_nav_pick), one case per rule.
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

/* ---- music_nav_pick: one case per rule ---------------------------------- */

static const char *path_at(void *ctx, int index) {
   (void)ctx;
   return s_queue[index];
}

static music_nav_queue_t q;
static unsigned int seed = 7;

static void queue_modes(bool shuffle, bool repeat_all, bool repeat_one) {
   q = (music_nav_queue_t){ .find = find,
                            .path_at = path_at,
                            .length = s_queue_len,
                            .shuffle = shuffle,
                            .repeat_all = repeat_all,
                            .repeat_one = repeat_one };
}

static music_nav_pick_t pick(music_nav_reason_t reason, int pos) {
   music_nav_request_t rq = { .reason = reason, .dir = MUSIC_NAV_NEXT, .commit = true, .pos = pos };
   music_nav_pick_t out;
   music_nav_pick(&h, &q, &rq, &seed, &out);
   return out;
}

static music_nav_pick_t pick_rq(const music_nav_request_t *rq) {
   music_nav_pick_t out;
   music_nav_pick(&h, &q, rq, &seed, &out);
   return out;
}

static void test_auto_in_order(void) {
   queue_modes(false, false, false);
   music_nav_pick_t p = pick(MUSIC_NAV_AUTO, 1);
   TEST_ASSERT_EQUAL_INT(MUSIC_NAV_PLAY, p.result);
   TEST_ASSERT_EQUAL_INT(2, p.index);
   TEST_ASSERT_TRUE(p.fresh);
   TEST_ASSERT_EQUAL_STRING("c", music_history_current(&h));
   TEST_ASSERT_EQUAL_INT(MUSIC_NAV_STOP_END, pick(MUSIC_NAV_AUTO, 3).result);
   queue_modes(false, true, false);
   p = pick(MUSIC_NAV_AUTO, 3); /* repeat-all wraps */
   TEST_ASSERT_EQUAL_INT(MUSIC_NAV_PLAY, p.result);
   TEST_ASSERT_EQUAL_INT(0, p.index);
   p = pick(MUSIC_NAV_AUTO, -1); /* the queue doesn't hold the track: its top */
   TEST_ASSERT_EQUAL_INT(0, p.index);
   p = pick(MUSIC_NAV_AUTO, 9); /* a stale position past the end */
   TEST_ASSERT_EQUAL_INT(0, p.index);
}

static void test_auto_stops(void) {
   queue_modes(false, false, true);
   music_nav_request_t rq = { .reason = MUSIC_NAV_AUTO, .pos = 1, .track_failed = true };
   TEST_ASSERT_EQUAL_INT(MUSIC_NAV_STOP_REPEAT_FAILED, pick_rq(&rq).result);
   queue_modes(false, false, false);
   rq = (music_nav_request_t){ .reason = MUSIC_NAV_AUTO, .pos = 1, .failures = 4 };
   TEST_ASSERT_EQUAL_INT(MUSIC_NAV_STOP_ALL_FAILED, pick_rq(&rq).result);
   static const char *const many[] = { "1", "2", "3", "4", "5", "6", "7", "8" };
   queue_set(many, 8);
   queue_modes(false, false, false);
   rq.failures = MUSIC_NAV_MAX_FAILED;
   TEST_ASSERT_EQUAL_INT(MUSIC_NAV_STOP_TOO_MANY, pick_rq(&rq).result);
   queue_set(many, 0);
   queue_modes(false, true, false);
   TEST_ASSERT_EQUAL_INT(MUSIC_NAV_STOP_END, pick(MUSIC_NAV_AUTO, -1).result);
}

static void test_auto_repeat_one(void) {
   queue_modes(false, false, true);
   music_history_push(&h, "b", 1);
   music_nav_pick_t p = pick(MUSIC_NAV_AUTO, 1);
   TEST_ASSERT_EQUAL_INT(1, p.index);
   TEST_ASSERT_FALSE(p.fresh); /* a replay adds nothing */
   music_nav_request_t rq = { .reason = MUSIC_NAV_AUTO, .pos = -1, .track_failed = true };
   p = pick_rq(&rq); /* not held (replaced): repeat-one doesn't apply, so no repeat stop */
   TEST_ASSERT_EQUAL_INT(MUSIC_NAV_PLAY, p.result);
   TEST_ASSERT_EQUAL_INT(0, p.index);
   music_history_clear(&h);
   p = pick(MUSIC_NAV_AUTO, 9); /* the queue shrank under it: its top */
   TEST_ASSERT_EQUAL_INT(MUSIC_NAV_PLAY, p.result);
   TEST_ASSERT_EQUAL_INT(0, p.index);
   TEST_ASSERT_TRUE(p.fresh);
}

static void test_auto_shuffle(void) {
   queue_modes(true, false, false);
   music_history_push(&h, "a", 0);
   music_nav_pick_t p = pick(MUSIC_NAV_AUTO, 0);
   TEST_ASSERT_EQUAL_INT(MUSIC_NAV_PLAY, p.result);
   TEST_ASSERT_TRUE(p.index > 0 && p.index < 4); /* never the current track */
   TEST_ASSERT_TRUE(p.fresh);
   int first = p.index;
   TEST_ASSERT_EQUAL_INT(0, back()); /* the forward entry is walked before a new pick */
   p = pick(MUSIC_NAV_AUTO, 0);
   TEST_ASSERT_EQUAL_INT(first, p.index);
   TEST_ASSERT_TRUE(p.fresh);
   /* After a failure, in order (wrapping) */
   music_nav_request_t rq = { .reason = MUSIC_NAV_AUTO, .pos = 3, .failures = 1 };
   TEST_ASSERT_EQUAL_INT(0, pick_rq(&rq).index);
}

static void test_auto_shuffle_one_track(void) {
   static const char *const one[] = { "x" };
   queue_set(one, 1);
   queue_modes(true, false, false);
   music_nav_pick_t p = pick(MUSIC_NAV_AUTO, 0); /* replays it, as today */
   TEST_ASSERT_EQUAL_INT(MUSIC_NAV_PLAY, p.result);
   TEST_ASSERT_EQUAL_INT(0, p.index);
}

static void test_user_in_order(void) {
   queue_modes(false, false, false);
   music_nav_request_t dry = { .reason = MUSIC_NAV_USER, .dir = MUSIC_NAV_NEXT, .pos = 1 };
   TEST_ASSERT_EQUAL_INT(2, pick_rq(&dry).index);
   TEST_ASSERT_EQUAL_INT(0, h.len); /* asked, not committed: nothing recorded */
   TEST_ASSERT_EQUAL_INT(2, pick(MUSIC_NAV_USER, 1).index);
   TEST_ASSERT_EQUAL_INT(MUSIC_NAV_NONE, pick(MUSIC_NAV_USER, 3).result);
   music_nav_request_t rq = { .reason = MUSIC_NAV_USER, .dir = MUSIC_NAV_PREVIOUS, .commit = true };
   TEST_ASSERT_EQUAL_INT(MUSIC_NAV_NONE, pick_rq(&rq).result); /* pos 0 */
   rq.pos = -1; /* not held: nothing is before the top */
   TEST_ASSERT_EQUAL_INT(MUSIC_NAV_NONE, pick_rq(&rq).result);
   TEST_ASSERT_EQUAL_INT(0, pick(MUSIC_NAV_USER, -1).index); /* next is the top */
   queue_modes(false, true, true); /* repeat-all wraps; repeat-one is ignored */
   TEST_ASSERT_EQUAL_INT(0, pick(MUSIC_NAV_USER, 3).index);
   rq.pos = 0;
   TEST_ASSERT_EQUAL_INT(3, pick_rq(&rq).index);
   rq.pos = 9; /* past the end: from the last track */
   TEST_ASSERT_EQUAL_INT(2, pick_rq(&rq).index);
}

static void test_user_shuffle(void) {
   queue_modes(true, false, false);
   music_nav_request_t rq = { .reason = MUSIC_NAV_USER, .dir = MUSIC_NAV_PREVIOUS, .commit = true };
   TEST_ASSERT_EQUAL_INT(MUSIC_NAV_NONE, pick_rq(&rq).result); /* nothing earlier: no wrap */
   music_history_push(&h, "a", 0);
   rq = (music_nav_request_t){ .reason = MUSIC_NAV_USER, .dir = MUSIC_NAV_NEXT, .pos = 0 };
   music_nav_pick_t p = pick_rq(&rq); /* asked, not committed: a move, nothing drawn */
   TEST_ASSERT_EQUAL_INT(MUSIC_NAV_PLAY, p.result);
   TEST_ASSERT_EQUAL_INT(-1, p.index);
   TEST_ASSERT_EQUAL_INT(1, h.len);
   rq.commit = true;
   p = pick_rq(&rq);
   TEST_ASSERT_TRUE(p.index > 0);
   TEST_ASSERT_EQUAL_INT(2, h.len);
   rq.dir = MUSIC_NAV_PREVIOUS;
   TEST_ASSERT_EQUAL_INT(0, pick_rq(&rq).index); /* walks back */
}

static void test_plan(void) {
   queue_modes(false, false, true);
   TEST_ASSERT_EQUAL_INT(MUSIC_NAV_NONE, pick(MUSIC_NAV_PLAN, 1).result); /* repeat-one */
   queue_modes(false, true, false);
   TEST_ASSERT_EQUAL_INT(2, pick(MUSIC_NAV_PLAN, 1).index);
   TEST_ASSERT_EQUAL_INT(0, pick(MUSIC_NAV_PLAN, 3).index); /* wraps */
   queue_modes(false, false, false);
   TEST_ASSERT_EQUAL_INT(MUSIC_NAV_NONE, pick(MUSIC_NAV_PLAN, 3).result);
   /* Shuffle: planned as the forward entry, which the advance then plays */
   queue_modes(true, false, false);
   music_history_push(&h, "a", 0);
   music_nav_pick_t planned = pick(MUSIC_NAV_PLAN, 0);
   TEST_ASSERT_TRUE(planned.index > 0);
   TEST_ASSERT_EQUAL_STRING("a", music_history_current(&h));            /* planning doesn't move */
   TEST_ASSERT_EQUAL_INT(planned.index, pick(MUSIC_NAV_PLAN, 0).index); /* planned once */
   TEST_ASSERT_EQUAL_INT(planned.index, pick(MUSIC_NAV_AUTO, 0).index);
   static const char *const one[] = { "x" };
   queue_set(one, 1);
   queue_modes(true, false, false);
   TEST_ASSERT_EQUAL_INT(MUSIC_NAV_NONE, pick(MUSIC_NAV_PLAN, 0).result);
}

static void test_pick_clears_replaced_history(void) {
   queue_modes(true, false, false);
   music_history_push(&h, "a", 0);
   music_history_push(&h, "b", 1);
   q.epoch = h.epoch + 1; /* another tab replaced the queue */
   music_nav_request_t rq = { .reason = MUSIC_NAV_USER, .dir = MUSIC_NAV_PREVIOUS, .commit = true };
   TEST_ASSERT_EQUAL_INT(MUSIC_NAV_NONE, pick_rq(&rq).result);
   TEST_ASSERT_EQUAL_INT(0, h.len);
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
   RUN_TEST(test_auto_in_order);
   RUN_TEST(test_auto_stops);
   RUN_TEST(test_auto_repeat_one);
   RUN_TEST(test_auto_shuffle);
   RUN_TEST(test_auto_shuffle_one_track);
   RUN_TEST(test_user_in_order);
   RUN_TEST(test_user_shuffle);
   RUN_TEST(test_plan);
   RUN_TEST(test_pick_clears_replaced_history);
   return UNITY_END();
}
