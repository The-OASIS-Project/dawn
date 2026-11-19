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
 * The messaging worker's list of messages waiting for a held conversation:
 * a held conversation never holds up another channel, a channel's messages run
 * in arrival order once it is free, a STOP doesn't queue, the caps, and
 * shutdown.  The real lease; the run function stands in for process_inbound
 * (take the conversation with a try, as it does, and record the message).
 */
#define MESSAGING_ENGINE_INTERNAL_ALLOWED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/conv_turn_lease.h"
#include "messaging/messaging_engine_internal.h"
#include "unity.h"

/* Each channel's conversation, in this test. */
#define CONV_OF(channel) ((channel) + 1000)

static int s_ran[256];
static int s_ran_n;
static int s_runs; /* calls of the run function, run or not */

static inbound_outcome_t fake_run(inbound_item_t *item) {
   s_runs++;
   if (item->kind != INBOUND_STOP) {
      const int64_t conv = CONV_OF(item->ref.channel_id);
      const uint64_t token = conv_turn_lease_new_token();
      if (conv_turn_lease_try_acquire(conv, 1, token) == CONV_TURN_LEASE_BUSY) {
         item->busy_conv = conv;
         return INBOUND_PARKED;
      }
      conv_turn_lease_release(conv, token);
   }
   if (s_ran_n < 256) {
      s_ran[s_ran_n++] = atoi(item->body);
   }
   return INBOUND_RAN;
}

static inbound_item_t *make_item(int64_t channel, int id, inbound_kind_t kind) {
   inbound_item_t *item = calloc(1, sizeof(*item));
   TEST_ASSERT_NOT_NULL(item);
   char body[16];
   snprintf(body, sizeof(body), "%d", id);
   item->body = strdup(body);
   TEST_ASSERT_NOT_NULL(item->body);
   snprintf(item->provider, sizeof(item->provider), "%s", "telegram");
   item->ref.channel_id = channel;
   item->kind = kind;
   return item;
}

static void submit(int64_t channel, int id) {
   inbound_parked_submit(make_item(channel, id, INBOUND_MESSAGE), fake_run);
}

/* Another session's turn holding a channel's conversation. */
static uint64_t hold(int64_t channel) {
   const uint64_t token = conv_turn_lease_new_token();
   TEST_ASSERT_EQUAL_INT(CONV_TURN_LEASE_GRANTED,
                         conv_turn_lease_try_acquire(CONV_OF(channel), 99, token));
   return token;
}

static void let_go(int64_t channel, uint64_t token) {
   TEST_ASSERT_EQUAL_INT(SUCCESS, conv_turn_lease_release(CONV_OF(channel), token));
}

void setUp(void) {
   s_ran_n = 0;
   s_runs = 0;
}

void tearDown(void) {
   inbound_parked_drop_all();
}

static void test_a_held_conversation_does_not_hold_up_another_channel(void) {
   const uint64_t a = hold(1);
   submit(1, 11);
   submit(2, 21);
   /* Channel 2 was answered while channel 1's conversation is still held. */
   TEST_ASSERT_EQUAL_INT(1, s_ran_n);
   TEST_ASSERT_EQUAL_INT(21, s_ran[0]);
   TEST_ASSERT_EQUAL_size_t(1, inbound_parked_count());
   /* Looking again while it is held runs nothing. */
   inbound_parked_retry(fake_run);
   TEST_ASSERT_EQUAL_INT(1, s_ran_n);
   let_go(1, a);
   inbound_parked_retry(fake_run);
   TEST_ASSERT_EQUAL_INT(2, s_ran_n);
   TEST_ASSERT_EQUAL_INT(11, s_ran[1]);
   TEST_ASSERT_EQUAL_size_t(0, inbound_parked_count());
}

static void test_waiting_messages_run_in_arrival_order(void) {
   const uint64_t a = hold(3);
   submit(3, 31);
   submit(3, 32);
   const int runs = s_runs;
   /* A third, arriving while two wait, goes behind them without being tried. */
   submit(3, 33);
   TEST_ASSERT_EQUAL_INT(runs, s_runs);
   TEST_ASSERT_EQUAL_size_t(3, inbound_parked_count());
   let_go(3, a);
   /* Free now, but two wait: a fourth arriving before the worker looks again
    * still goes behind them. */
   submit(3, 34);
   TEST_ASSERT_EQUAL_INT(0, s_ran_n);
   inbound_parked_retry(fake_run);
   TEST_ASSERT_EQUAL_INT(4, s_ran_n);
   TEST_ASSERT_EQUAL_INT(31, s_ran[0]);
   TEST_ASSERT_EQUAL_INT(32, s_ran[1]);
   TEST_ASSERT_EQUAL_INT(33, s_ran[2]);
   TEST_ASSERT_EQUAL_INT(34, s_ran[3]);
   TEST_ASSERT_EQUAL_size_t(0, inbound_parked_count());
}

static void test_a_head_that_finds_it_held_again_keeps_its_channel_behind(void) {
   const uint64_t a = hold(4);
   const uint64_t b = hold(5);
   submit(4, 41);
   submit(5, 51);
   submit(4, 42);
   let_go(5, b);
   inbound_parked_retry(fake_run);
   TEST_ASSERT_EQUAL_INT(1, s_ran_n);
   TEST_ASSERT_EQUAL_INT(51, s_ran[0]);
   TEST_ASSERT_EQUAL_size_t(2, inbound_parked_count());
   let_go(4, a);
   inbound_parked_retry(fake_run);
   TEST_ASSERT_EQUAL_INT(3, s_ran_n);
   TEST_ASSERT_EQUAL_INT(41, s_ran[1]);
   TEST_ASSERT_EQUAL_INT(42, s_ran[2]);
}

static void test_a_stop_does_not_queue(void) {
   const uint64_t a = hold(6);
   submit(6, 61);
   inbound_parked_submit(make_item(6, 62, INBOUND_STOP), fake_run);
   TEST_ASSERT_EQUAL_INT(1, s_ran_n);
   TEST_ASSERT_EQUAL_INT(62, s_ran[0]);
   TEST_ASSERT_EQUAL_size_t(1, inbound_parked_count());
   let_go(6, a);
   inbound_parked_retry(fake_run);
   TEST_ASSERT_EQUAL_INT(61, s_ran[1]);
}

static void test_a_reply_code_queues_like_a_message(void) {
   const uint64_t a = hold(7);
   submit(7, 71);
   inbound_parked_submit(make_item(7, 72, INBOUND_CODE), fake_run);
   TEST_ASSERT_EQUAL_INT(0, s_ran_n);
   let_go(7, a);
   inbound_parked_retry(fake_run);
   TEST_ASSERT_EQUAL_INT(2, s_ran_n);
   TEST_ASSERT_EQUAL_INT(71, s_ran[0]);
   TEST_ASSERT_EQUAL_INT(72, s_ran[1]);
}

static void test_the_caps(void) {
   const uint64_t a = hold(8);
   for (int i = 0; i < MESSAGING_PARKED_PER_CHANNEL_MAX + 1; i++) {
      submit(8, 800 + i);
   }
   /* One past the channel's cap is dropped. */
   TEST_ASSERT_EQUAL_size_t(MESSAGING_PARKED_PER_CHANNEL_MAX, inbound_parked_count());

   /* Fill the rest from other channels, then one more is dropped. */
   uint64_t held[MESSAGING_PARKED_MAX];
   int nheld = 0;
   int64_t channel = 100;
   while (inbound_parked_count() < MESSAGING_PARKED_MAX) {
      held[nheld++] = hold(channel);
      for (int i = 0;
           i < MESSAGING_PARKED_PER_CHANNEL_MAX && inbound_parked_count() < MESSAGING_PARKED_MAX;
           i++) {
         submit(channel, 1);
      }
      channel++;
   }
   held[nheld++] = hold(channel);
   submit(channel, 1);
   TEST_ASSERT_EQUAL_size_t(MESSAGING_PARKED_MAX, inbound_parked_count());
   TEST_ASSERT_EQUAL_INT(0, s_ran_n);

   /* The channel's first sixteen run in order; the dropped one never does. */
   let_go(8, a);
   inbound_parked_retry(fake_run);
   TEST_ASSERT_EQUAL_INT(MESSAGING_PARKED_PER_CHANNEL_MAX, s_ran_n);
   for (int i = 0; i < MESSAGING_PARKED_PER_CHANNEL_MAX; i++) {
      TEST_ASSERT_EQUAL_INT(800 + i, s_ran[i]);
   }
   for (int i = 0; i < nheld; i++) {
      let_go(100 + i, held[i]);
   }
}

static void test_shutdown_drops_what_waits(void) {
   const uint64_t a = hold(9);
   submit(9, 91);
   submit(9, 92);
   TEST_ASSERT_EQUAL_size_t(2, inbound_parked_count());
   inbound_parked_drop_all();
   TEST_ASSERT_EQUAL_size_t(0, inbound_parked_count());
   let_go(9, a);
   inbound_parked_retry(fake_run);
   TEST_ASSERT_EQUAL_INT(0, s_ran_n);
   /* The list works again afterwards. */
   submit(9, 93);
   TEST_ASSERT_EQUAL_INT(1, s_ran_n);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_a_held_conversation_does_not_hold_up_another_channel);
   RUN_TEST(test_waiting_messages_run_in_arrival_order);
   RUN_TEST(test_a_head_that_finds_it_held_again_keeps_its_channel_behind);
   RUN_TEST(test_a_stop_does_not_queue);
   RUN_TEST(test_a_reply_code_queues_like_a_message);
   RUN_TEST(test_the_caps);
   RUN_TEST(test_shutdown_drops_what_waits);
   return UNITY_END();
}
