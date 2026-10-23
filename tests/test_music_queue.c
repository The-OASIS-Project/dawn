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
 * Unit tests for the music queue (music_queue.c): shared and private queues,
 * adding with dedup and a full queue, inserting at the top, removing, the
 * nearest copy, a queue kept across a restart, and two users saving at once
 * (temp database).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio/music_queue.h"
#include "audio/music_queue_db.h"
#include "dawn_error.h"
#include "unity.h"

static char s_db_path[64];

static music_search_result_t track(const char *path) {
   music_search_result_t t;
   memset(&t, 0, sizeof(t));
   snprintf(t.path, sizeof(t.path), "%s", path);
   snprintf(t.title, sizeof(t.title), "Title of %s", path);
   t.duration_sec = 180;
   return t;
}

static void remove_db(void) {
   char side[80];
   unlink(s_db_path);
   snprintf(side, sizeof(side), "%s-wal", s_db_path);
   unlink(side);
   snprintf(side, sizeof(side), "%s-shm", s_db_path);
   unlink(side);
}

void setUp(void) {
   snprintf(s_db_path, sizeof(s_db_path), "/tmp/test_music_queue_XXXXXX");
   int fd = mkstemp(s_db_path);
   TEST_ASSERT_TRUE(fd >= 0);
   close(fd);
   TEST_ASSERT_EQUAL_INT(SUCCESS, music_queue_init(s_db_path));
}

void tearDown(void) {
   music_queue_shutdown();
   remove_db();
}

static void test_shared_and_private(void) {
   user_music_queue_t *a = music_queue_acquire(7);
   user_music_queue_t *b = music_queue_acquire(7);
   user_music_queue_t *p = music_queue_acquire(0);
   TEST_ASSERT_NOT_NULL(a);
   TEST_ASSERT_TRUE(a == b); /* every player of a user shares one */
   TEST_ASSERT_NOT_NULL(p);
   TEST_ASSERT_TRUE(p != a);
   TEST_ASSERT_EQUAL_INT(0, p->user_id);
   music_queue_release(a);
   music_queue_release(b);
   music_queue_release(p); /* frees the private one */
}

static void test_add_dedups_and_fills(void) {
   user_music_queue_t *uq = music_queue_acquire(0);
   music_search_result_t t[3] = { track("/m/a.flac"), track("/m/b.flac"), track("/m/a.flac") };
   t[1].path[0] = '\0'; /* no track */
   music_queue_outcome_t oc[3];
   int added = -1;
   uint32_t gen = uq->generation;
   music_queue_add(uq, t, 3, false, oc, &added);
   TEST_ASSERT_EQUAL_INT(1, added);
   TEST_ASSERT_EQUAL_UINT32(gen + 1, uq->generation); /* other players see it */
   TEST_ASSERT_EQUAL_INT(MUSIC_QUEUE_ADDED, oc[0]);
   TEST_ASSERT_EQUAL_INT(MUSIC_QUEUE_NOT_FOUND, oc[1]);
   TEST_ASSERT_EQUAL_INT(MUSIC_QUEUE_DUPLICATE, oc[2]);
   TEST_ASSERT_EQUAL_STRING("Title of /m/a.flac", uq->queue[0].title);

   /* Fill it, then one more is refused */
   for (int i = uq->queue_length; i < MUSIC_QUEUE_MAX; i++) {
      char path[32];
      snprintf(path, sizeof(path), "/m/%d.flac", i);
      music_search_result_t x = track(path);
      music_queue_add(uq, &x, 1, false, NULL, NULL);
   }
   TEST_ASSERT_EQUAL_INT(MUSIC_QUEUE_MAX, uq->queue_length);
   music_search_result_t extra = track("/m/extra.flac");
   music_queue_add(uq, &extra, 1, false, oc, &added);
   TEST_ASSERT_EQUAL_INT(MUSIC_QUEUE_FULL, oc[0]);
   TEST_ASSERT_EQUAL_INT(0, added);

   /* Replace starts over and bumps the epoch */
   uint32_t epoch = uq->replace_epoch;
   music_queue_add(uq, &extra, 1, true, NULL, &added);
   TEST_ASSERT_EQUAL_INT(1, uq->queue_length);
   TEST_ASSERT_EQUAL_UINT32(epoch + 1, uq->replace_epoch);
   music_queue_release(uq);
}

static void test_insert_top_drops_last_when_full(void) {
   user_music_queue_t *uq = music_queue_acquire(0);
   for (int i = 0; i < MUSIC_QUEUE_MAX; i++) {
      char path[32];
      snprintf(path, sizeof(path), "/m/%d.flac", i);
      music_search_result_t x = track(path);
      TEST_ASSERT_EQUAL_INT(SUCCESS, music_queue_insert(uq, uq->queue_length, &x));
   }
   music_search_result_t top = track("/m/top.flac");
   TEST_ASSERT_EQUAL_INT(FAILURE, music_queue_insert(uq, uq->queue_length, &top));
   TEST_ASSERT_EQUAL_INT(SUCCESS, music_queue_insert(uq, 0, &top));
   TEST_ASSERT_EQUAL_INT(MUSIC_QUEUE_MAX, uq->queue_length);
   TEST_ASSERT_EQUAL_STRING("/m/top.flac", uq->queue[0].path);
   TEST_ASSERT_EQUAL_STRING("/m/0.flac", uq->queue[1].path);
   TEST_ASSERT_EQUAL_INT(-1, music_queue_find(uq, "/m/249.flac", 0)); /* dropped */
   TEST_ASSERT_EQUAL_INT(FAILURE, music_queue_insert(uq, -1, &top));
   music_queue_release(uq);
}

static void test_remove_and_nearest_copy(void) {
   user_music_queue_t *uq = music_queue_acquire(0);
   const char *paths[] = { "/m/x", "/m/a", "/m/y", "/m/z", "/m/a" };
   for (int i = 0; i < 5; i++) {
      music_search_result_t x = track(paths[i]);
      music_queue_insert(uq, uq->queue_length, &x);
   }
   TEST_ASSERT_EQUAL_INT(1, music_queue_find(uq, "/m/a", 1)); /* still there */
   TEST_ASSERT_EQUAL_INT(1, music_queue_find(uq, "/m/a", 2));
   TEST_ASSERT_EQUAL_INT(4, music_queue_find(uq, "/m/a", 3));
   TEST_ASSERT_EQUAL_INT(-1, music_queue_find(uq, "/m/none", 0));

   music_queue_remove_at(uq, 1);
   TEST_ASSERT_EQUAL_INT(4, uq->queue_length);
   TEST_ASSERT_EQUAL_STRING("/m/y", uq->queue[1].path);
   TEST_ASSERT_EQUAL_INT(3, music_queue_find(uq, "/m/a", 1));
   music_queue_remove_at(uq, 9); /* out of range: nothing */
   TEST_ASSERT_EQUAL_INT(4, uq->queue_length);
   music_queue_release(uq);
}

static void test_kept_across_restart(void) {
   user_music_queue_t *uq = music_queue_acquire(7);
   music_search_result_t t[2] = { track("/m/one.flac"), track("/m/two.flac") };
   pthread_mutex_lock(&uq->queue_mutex);
   uq->shuffle = true;
   uq->repeat_mode = MUSIC_REPEAT_ALL;
   pthread_mutex_unlock(&uq->queue_mutex);
   music_queue_add(uq, t, 2, false, NULL, NULL); /* saves */
   music_queue_release(uq);

   music_queue_shutdown();
   TEST_ASSERT_EQUAL_INT(SUCCESS, music_queue_init(s_db_path));

   uq = music_queue_acquire(7);
   TEST_ASSERT_EQUAL_INT(2, uq->queue_length);
   TEST_ASSERT_EQUAL_STRING("/m/one.flac", uq->queue[0].path);
   TEST_ASSERT_EQUAL_STRING("Title of /m/two.flac", uq->queue[1].title);
   TEST_ASSERT_EQUAL_UINT32(180, uq->queue[1].duration_sec);
   TEST_ASSERT_TRUE(uq->shuffle);
   TEST_ASSERT_EQUAL_INT(MUSIC_REPEAT_ALL, uq->repeat_mode);
   music_queue_release(uq);

   /* Another user's queue starts empty */
   user_music_queue_t *other = music_queue_acquire(8);
   TEST_ASSERT_EQUAL_INT(0, other->queue_length);
   music_queue_release(other);
}

/* Two users save at once: each holds only its own queue_mutex, and both go
 * through the one connection */
#define CONCURRENT_SAVES 200

static void *save_loop(void *arg) {
   user_music_queue_t *uq = arg;
   intptr_t failures = 0;
   for (int i = 0; i < CONCURRENT_SAVES; i++) {
      pthread_mutex_lock(&uq->queue_mutex);
      if (music_queue_db_save(uq->user_id, uq) != 0) {
         failures++;
      }
      pthread_mutex_unlock(&uq->queue_mutex);
   }
   return (void *)failures;
}

static void test_two_users_save_at_once(void) {
   user_music_queue_t *a = music_queue_acquire(7);
   user_music_queue_t *b = music_queue_acquire(8);
   for (int i = 0; i < 40; i++) {
      char path[32];
      snprintf(path, sizeof(path), "/m/%d.flac", i);
      music_search_result_t x = track(path);
      music_queue_insert(a, a->queue_length, &x);
      music_queue_insert(b, b->queue_length, &x);
   }
   pthread_t ta, tb;
   TEST_ASSERT_EQUAL_INT(0, pthread_create(&ta, NULL, save_loop, a));
   TEST_ASSERT_EQUAL_INT(0, pthread_create(&tb, NULL, save_loop, b));
   void *fa, *fb;
   pthread_join(ta, &fa);
   pthread_join(tb, &fb);
   TEST_ASSERT_EQUAL_INT(0, (int)(intptr_t)fa);
   TEST_ASSERT_EQUAL_INT(0, (int)(intptr_t)fb);
   music_queue_release(a);
   music_queue_release(b);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_shared_and_private);
   RUN_TEST(test_add_dedups_and_fills);
   RUN_TEST(test_insert_top_drops_last_when_full);
   RUN_TEST(test_remove_and_nearest_copy);
   RUN_TEST(test_kept_across_restart);
   RUN_TEST(test_two_users_save_at_once);
   return UNITY_END();
}
