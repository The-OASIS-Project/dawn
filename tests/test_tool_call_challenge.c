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
 * Reply codes and the actions that wait for them: a code is recognized only
 * as a text's first word; an action waits one per channel, its code handed
 * out once; the right code hands the call over once; wrong ones void it; a
 * newer request from a later turn replaces an older one; codes per channel
 * are capped.  And the one-line excerpt descriptions are made with.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/reply_code.h"
#include "core/tool_call_challenge.h"
#include "unity.h"
#include "utils/string_utils.h"

void setUp(void) {
   tool_call_challenge_clear_all();
}

void tearDown(void) {
}

static tool_challenge_rc_t hold(int64_t channel, uint64_t turn, const char *what) {
   const tool_challenge_t c = {
      .channel_id = channel,
      .user_id = 1,
      .turn_token = turn,
      .tool = "music",
      .args = "{\"action\":\"play\"}",
      .binding = what, /* as in use: the binding covers the description */
      .description = what,
   };
   return tool_call_challenge_create(&c);
}

static void test_code_in_text(void) {
   char code[REPLY_CODE_LEN];
   TEST_ASSERT_TRUE(reply_code_in_text("482193", code));
   TEST_ASSERT_EQUAL_STRING("482193", code);
   TEST_ASSERT_TRUE(reply_code_in_text("  482193 thanks", code));
   TEST_ASSERT_TRUE(reply_code_in_text("482193.", code));
   TEST_ASSERT_TRUE(reply_code_in_text("482193!", code));
   TEST_ASSERT_FALSE(reply_code_in_text("4821930", NULL));
   TEST_ASSERT_FALSE(reply_code_in_text("48219", NULL));
   TEST_ASSERT_FALSE(reply_code_in_text("code 482193", NULL));
   TEST_ASSERT_FALSE(reply_code_in_text("482193abc", NULL));
   TEST_ASSERT_FALSE(reply_code_in_text("482193.5", NULL));
   TEST_ASSERT_FALSE(reply_code_in_text(NULL, NULL));
}

static void test_digest(void) {
   char a[REPLY_CODE_DIGEST_HEX], b[REPLY_CODE_DIGEST_HEX], c[REPLY_CODE_DIGEST_HEX];
   reply_code_digest("123456", a);
   reply_code_digest("123456", b);
   reply_code_digest("123457", c);
   TEST_ASSERT_TRUE(reply_code_digest_equal(a, b));
   TEST_ASSERT_FALSE(reply_code_digest_equal(a, c));
   TEST_ASSERT_FALSE(reply_code_digest_equal(a, ""));
   char code[REPLY_CODE_LEN];
   reply_code_new(code);
   TEST_ASSERT_TRUE(reply_code_in_text(code, NULL));
}

/* One waits per channel; its code is handed out once; the right code hands
 * the call over (with its binding) and it's gone. */
static void test_create_send_redeem(void) {
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, hold(7, 100, "music play"));
   TEST_ASSERT_TRUE(tool_call_challenge_live(7));
   TEST_ASSERT_FALSE(tool_call_challenge_live(8));
   /* A second action from the same turn waits its turn. */
   const tool_challenge_t other = {
      .channel_id = 7,
      .user_id = 1,
      .turn_token = 100,
      .tool = "music",
      .args = "{\"action\":\"stop\"}",
      .binding = "def",
      .description = "music stop",
   };
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_BUSY, tool_call_challenge_create(&other));
   /* The same call asked again keeps its code: nothing new is sent. */
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_SAME, hold(7, 101, "music play"));

   char code[REPLY_CODE_LEN], desc[TOOL_CALL_CHALLENGE_DESC_MAX];
   int seconds = 0;
   TEST_ASSERT_EQUAL_INT(TOOL_TAKE_OK,
                         tool_call_challenge_take_unsent(7, code, desc, sizeof(desc), &seconds));
   TEST_ASSERT_EQUAL_STRING("music play", desc);
   TEST_ASSERT_TRUE(seconds > TOOL_CALL_CHALLENGE_TTL_SEC - 5 &&
                    seconds <= TOOL_CALL_CHALLENGE_TTL_SEC);
   char again[REPLY_CODE_LEN];
   TEST_ASSERT_EQUAL_INT(TOOL_TAKE_NONE, tool_call_challenge_take_unsent(7, again, NULL, 0, NULL));

   tool_redeemed_t out;
   /* Another channel, or another user, has nothing waiting. */
   TEST_ASSERT_EQUAL_INT(TOOL_REDEEM_NONE, tool_call_challenge_redeem(8, 1, code, &out, NULL));
   TEST_ASSERT_EQUAL_INT(TOOL_REDEEM_NONE, tool_call_challenge_redeem(7, 2, code, &out, NULL));
   TEST_ASSERT_EQUAL_INT(TOOL_REDEEM_OK, tool_call_challenge_redeem(7, 1, code, &out, NULL));
   TEST_ASSERT_EQUAL_STRING("music", out.tool);
   TEST_ASSERT_EQUAL_STRING("{\"action\":\"play\"}", out.args);
   TEST_ASSERT_EQUAL_STRING("music play", out.binding);
   free(out.args);
   TEST_ASSERT_FALSE(tool_call_challenge_live(7));
   TEST_ASSERT_EQUAL_INT(TOOL_REDEEM_NONE, tool_call_challenge_redeem(7, 1, code, &out, NULL));
}

/* A newer request from a later turn replaces the older one: its code stops
 * working, and the user is texted only the new one. */
static void test_newer_replaces(void) {
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, hold(7, 100, "first"));
   char first[REPLY_CODE_LEN];
   TEST_ASSERT_EQUAL_INT(TOOL_TAKE_OK, tool_call_challenge_take_unsent(7, first, NULL, 0, NULL));
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, hold(7, 101, "second"));
   char second[REPLY_CODE_LEN], desc[TOOL_CALL_CHALLENGE_DESC_MAX];
   TEST_ASSERT_EQUAL_INT(TOOL_TAKE_OK,
                         tool_call_challenge_take_unsent(7, second, desc, sizeof(desc), NULL));
   TEST_ASSERT_EQUAL_STRING("second", desc);
   tool_redeemed_t out;
   int tries = 0;
   if (strcmp(first, second) != 0) {
      TEST_ASSERT_EQUAL_INT(TOOL_REDEEM_WRONG,
                            tool_call_challenge_redeem(7, 1, first, &out, &tries));
      TEST_ASSERT_EQUAL_INT(1, tries);
   }
   TEST_ASSERT_EQUAL_INT(TOOL_REDEEM_OK, tool_call_challenge_redeem(7, 1, second, &out, NULL));
   free(out.args);
}

/* A code before the text went out never matches (it counts as a wrong try). */
static void test_code_before_sent(void) {
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, hold(7, 100, "music play"));
   tool_redeemed_t out;
   int tries = 0;
   TEST_ASSERT_EQUAL_INT(TOOL_REDEEM_WRONG,
                         tool_call_challenge_redeem(7, 1, "123456", &out, &tries));
   TEST_ASSERT_EQUAL_INT(1, tries);
   TEST_ASSERT_NULL(out.args);
}

/* One that runs out before its code was texted is reported once, so the user
 * is told; one that ran out after is not. */
static void test_expired_before_sent(void) {
   const tool_challenge_t brief = {
      .channel_id = 12,
      .user_id = 1,
      .turn_token = 1,
      .valid_for_sec = 1,
      .tool = "phone",
      .args = "{}",
      .binding = "x",
      .description = "call",
   };
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, tool_call_challenge_create(&brief));
   const tool_challenge_t sent = {
      .channel_id = 13,
      .user_id = 1,
      .turn_token = 1,
      .valid_for_sec = 1,
      .tool = "phone",
      .args = "{}",
      .binding = "x",
      .description = "call",
   };
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, tool_call_challenge_create(&sent));
   char code[REPLY_CODE_LEN];
   TEST_ASSERT_EQUAL_INT(TOOL_TAKE_OK, tool_call_challenge_take_unsent(13, code, NULL, 0, NULL));
   sleep(2);
   TEST_ASSERT_FALSE(tool_call_challenge_live(12));
   TEST_ASSERT_EQUAL_INT(TOOL_TAKE_EXPIRED,
                         tool_call_challenge_take_unsent(12, code, NULL, 0, NULL));
   TEST_ASSERT_EQUAL_INT(TOOL_TAKE_NONE, tool_call_challenge_take_unsent(12, code, NULL, 0, NULL));
   TEST_ASSERT_FALSE(tool_call_challenge_live(13));
   TEST_ASSERT_EQUAL_INT(TOOL_TAKE_NONE, tool_call_challenge_take_unsent(13, code, NULL, 0, NULL));
   /* A late reply with the texted code is answered: it expired (said once). */
   TEST_ASSERT_TRUE(tool_call_challenge_ended_recently(13));
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_END_EXPIRED, tool_call_challenge_take_ended(13));
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_END_NONE, tool_call_challenge_take_ended(13));
   TEST_ASSERT_FALSE(tool_call_challenge_ended_recently(13));
   /* One never texted leaves nothing to answer. */
   TEST_ASSERT_FALSE(tool_call_challenge_ended_recently(12));
}

/* Wrong codes: two leave it, the third voids it. */
static void test_wrong_codes_void(void) {
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, hold(7, 100, "music play"));
   tool_redeemed_t out;
   char code[REPLY_CODE_LEN];
   TEST_ASSERT_EQUAL_INT(TOOL_TAKE_OK, tool_call_challenge_take_unsent(7, code, NULL, 0, NULL));
   const char *wrong = strcmp(code, "000000") == 0 ? "111111" : "000000";
   int tries = 0;
   TEST_ASSERT_EQUAL_INT(TOOL_REDEEM_WRONG, tool_call_challenge_redeem(7, 1, wrong, &out, &tries));
   TEST_ASSERT_EQUAL_INT(1, tries);
   TEST_ASSERT_EQUAL_INT(TOOL_REDEEM_WRONG, tool_call_challenge_redeem(7, 1, wrong, &out, &tries));
   TEST_ASSERT_EQUAL_INT(2, tries);
   TEST_ASSERT_EQUAL_INT(TOOL_REDEEM_VOIDED, tool_call_challenge_redeem(7, 1, wrong, &out, NULL));
   TEST_ASSERT_FALSE(tool_call_challenge_live(7));
   TEST_ASSERT_EQUAL_INT(TOOL_REDEEM_NONE, tool_call_challenge_redeem(7, 1, code, &out, NULL));
   TEST_ASSERT_NULL(out.args);
}

/* STOP drops it; codes per channel are capped per hour, whatever the
 * session (the channel is the key); a short validity window is kept. */
static void test_cancel_limit_and_window(void) {
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, hold(9, 1, "music play"));
   TEST_ASSERT_TRUE(tool_call_challenge_cancel(9));
   TEST_ASSERT_FALSE(tool_call_challenge_cancel(9));
   for (int i = 1; i < TOOL_CALL_CHALLENGE_PER_HOUR; i++) {
      char what[32];
      snprintf(what, sizeof(what), "music play %d", i);
      TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, hold(9, 1 + (uint64_t)i, what));
   }
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_LIMIT, hold(9, 99, "music stop"));
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, hold(10, 1, "music play"));

   const tool_challenge_t short_lived = {
      .channel_id = 11,
      .user_id = 1,
      .turn_token = 1,
      .valid_for_sec = 125,
      .tool = "phone",
      .args = "{}",
      .binding = "x",
      .description = "call",
   };
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, tool_call_challenge_create(&short_lived));
   char code[REPLY_CODE_LEN];
   int seconds = 0;
   TEST_ASSERT_EQUAL_INT(TOOL_TAKE_OK,
                         tool_call_challenge_take_unsent(11, code, NULL, 0, &seconds));
   TEST_ASSERT_TRUE(seconds > 120 && seconds <= 125);
}

/* One plain line: controls and invisible formatting become spaces, runs of
 * them one, and a cut says how much was left out. */
static void test_excerpt_line(void) {
   char out[128];
   str_excerpt_line("to:\nbob\t\t@x", 100, out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("to: bob @x", out);
   str_excerpt_line("evil\xE2\x80\xAE"
                    "txt.exe",
                    100, out, sizeof(out)); /* U+202E */
   TEST_ASSERT_EQUAL_STRING("evil txt.exe", out);
   str_excerpt_line("abcdefghij", 4, out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("abcd... (6 more characters)", out);
   str_excerpt_line("\xC3\xA9\xC3\xA9\xC3\xA9", 3, out, sizeof(out)); /* never half a character */
   TEST_ASSERT_EQUAL_STRING("\xC3\xA9... (2 more characters)", out);
   str_excerpt_line(NULL, 10, out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("", out);
   /* Malformed UTF-8 shows nothing: an overlong newline, an overlong RLO, a
    * surrogate. */
   str_excerpt_line("a\xC0\x8A"
                    "b\xF0\x82\x80\xAE"
                    "c\xED\xA0\x80"
                    "d",
                    100, out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("a b c d", out);
   /* Tag characters (U+E0041) and a soft hyphen are invisible. */
   str_excerpt_line("x\xF3\xA0\x81\x81y\xC2\xADz", 100, out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("x y z", out);
   /* A cut that leaves only blanks says nothing was left out. */
   str_excerpt_line("abcd    \xE2\x80\x8B", 4, out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("abcd", out);
}

/* The same call asked again keeps its code: after it was texted too, and it
 * neither counts again nor buys more time; one about to run out is replaced. */
static void test_same_call(void) {
   for (int i = 0; i < TOOL_CALL_CHALLENGE_PER_HOUR; i++) {
      char what[32];
      snprintf(what, sizeof(what), "call %d", i);
      TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, hold(20, 1 + (uint64_t)i, what));
   }
   char code[REPLY_CODE_LEN];
   int before = 0, after = 0;
   TEST_ASSERT_EQUAL_INT(TOOL_TAKE_OK, tool_call_challenge_take_unsent(20, code, NULL, 0, &before));
   /* The cap is reached, yet the same call asked again is SAME, not LIMIT. */
   char newest[32];
   snprintf(newest, sizeof(newest), "call %d", TOOL_CALL_CHALLENGE_PER_HOUR - 1);
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_SAME, hold(20, 50, newest));
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_LIMIT, hold(20, 51, "another call"));
   tool_redeemed_t out;
   TEST_ASSERT_EQUAL_INT(TOOL_REDEEM_OK, tool_call_challenge_redeem(20, 1, code, &out, NULL));
   free(out.args);

   const tool_challenge_t brief = {
      .channel_id = 21,
      .user_id = 1,
      .turn_token = 1,
      .valid_for_sec = TOOL_CALL_CHALLENGE_MIN_SEC - 1,
      .tool = "phone",
      .args = "{}",
      .binding = "b",
      .description = "call",
   };
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, tool_call_challenge_create(&brief));
   tool_challenge_t again = brief;
   again.turn_token = 2;
   again.valid_for_sec = 0;
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, tool_call_challenge_create(&again));
   TEST_ASSERT_EQUAL_INT(TOOL_TAKE_OK, tool_call_challenge_take_unsent(21, code, NULL, 0, &after));
   TEST_ASSERT_TRUE(after > TOOL_CALL_CHALLENGE_MIN_SEC);

   /* In its own turn it's the same however little time is left; asked again
    * in a later turn, it becomes that turn's hold. */
   tool_challenge_t mine = again;
   mine.channel_id = 23;
   mine.turn_token = 5;
   mine.valid_for_sec = TOOL_CALL_CHALLENGE_MIN_SEC - 1;
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, tool_call_challenge_create(&mine));
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_SAME, tool_call_challenge_create(&mine));
   tool_challenge_t later = brief;
   later.channel_id = 24;
   later.valid_for_sec = 0;
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, tool_call_challenge_create(&later));
   later.turn_token = 9;
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_SAME, tool_call_challenge_create(&later));
   TEST_ASSERT_TRUE(tool_call_challenge_live_in_turn(24, 9));
   TEST_ASSERT_FALSE(tool_call_challenge_cancel_earlier(24, 9));
}

/* A code that couldn't be texted is dropped and refunded; a new request in a
 * later turn drops one an earlier turn left waiting, not its own. */
static void test_cancel_unsent_and_earlier(void) {
   for (int i = 0; i < TOOL_CALL_CHALLENGE_PER_HOUR; i++) {
      char what[32];
      snprintf(what, sizeof(what), "text %d", i);
      TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, hold(22, 1 + (uint64_t)i, what));
   }
   tool_call_challenge_cancel_unsent(22);
   TEST_ASSERT_FALSE(tool_call_challenge_live(22));
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, hold(22, 30, "text again"));

   TEST_ASSERT_TRUE(tool_call_challenge_live_in_turn(22, 30));
   TEST_ASSERT_FALSE(tool_call_challenge_live_in_turn(22, 31));
   TEST_ASSERT_FALSE(tool_call_challenge_cancel_earlier(22, 30));
   TEST_ASSERT_TRUE(tool_call_challenge_live(22));
   TEST_ASSERT_TRUE(tool_call_challenge_cancel_earlier(22, 31));
   TEST_ASSERT_FALSE(tool_call_challenge_live(22));
}

/* A code the user replies with doesn't count: working through many requests
 * never reaches the cap, which only unused codes fill. */
static void test_used_codes_refunded(void) {
   for (int i = 0; i < 3 * TOOL_CALL_CHALLENGE_PER_HOUR; i++) {
      char what[32];
      snprintf(what, sizeof(what), "send %d", i);
      TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, hold(25, 1 + (uint64_t)i, what));
      char code[REPLY_CODE_LEN];
      TEST_ASSERT_EQUAL_INT(TOOL_TAKE_OK, tool_call_challenge_take_unsent(25, code, NULL, 0, NULL));
      tool_redeemed_t out;
      TEST_ASSERT_EQUAL_INT(TOOL_REDEEM_OK, tool_call_challenge_redeem(25, 1, code, &out, NULL));
      free(out.args);
   }
   for (int i = 0; i < TOOL_CALL_CHALLENGE_PER_HOUR; i++) {
      char what[32];
      snprintf(what, sizeof(what), "unused %d", i);
      TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, hold(25, 100 + (uint64_t)i, what));
   }
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_LIMIT, hold(25, 200, "one more"));

   /* Replaced codes stay counted when a later one is used. */
   for (int i = 0; i < TOOL_CALL_CHALLENGE_PER_HOUR - 1; i++) {
      char what[32];
      snprintf(what, sizeof(what), "replaced %d", i);
      TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, hold(26, 1 + (uint64_t)i, what));
   }
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, hold(26, 50, "used"));
   char code[REPLY_CODE_LEN];
   TEST_ASSERT_EQUAL_INT(TOOL_TAKE_OK, tool_call_challenge_take_unsent(26, code, NULL, 0, NULL));
   tool_redeemed_t out;
   TEST_ASSERT_EQUAL_INT(TOOL_REDEEM_OK, tool_call_challenge_redeem(26, 1, code, &out, NULL));
   free(out.args);
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, hold(26, 60, "last unused"));
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_LIMIT, hold(26, 61, "over"));
}

/* How a texted code stopped working is remembered for a late reply: used,
 * replaced, cancelled, voided. */
static void test_end_reasons(void) {
   char code[REPLY_CODE_LEN];
   tool_redeemed_t out;
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, hold(30, 1, "a"));
   TEST_ASSERT_EQUAL_INT(TOOL_TAKE_OK, tool_call_challenge_take_unsent(30, code, NULL, 0, NULL));
   TEST_ASSERT_EQUAL_INT(TOOL_REDEEM_OK, tool_call_challenge_redeem(30, 1, code, &out, NULL));
   free(out.args);
   TEST_ASSERT_TRUE(tool_call_challenge_ended_recently(30));
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_END_USED, tool_call_challenge_take_ended(30));

   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, hold(30, 2, "b"));
   TEST_ASSERT_EQUAL_INT(TOOL_TAKE_OK, tool_call_challenge_take_unsent(30, code, NULL, 0, NULL));
   TEST_ASSERT_FALSE(tool_call_challenge_ended_recently(30)); /* one is live */
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, hold(30, 3, "c"));
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_END_REPLACED, tool_call_challenge_take_ended(30));

   TEST_ASSERT_EQUAL_INT(TOOL_TAKE_OK, tool_call_challenge_take_unsent(30, code, NULL, 0, NULL));
   TEST_ASSERT_TRUE(tool_call_challenge_cancel(30));
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_END_CANCELLED, tool_call_challenge_take_ended(30));

   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, hold(30, 4, "d"));
   TEST_ASSERT_EQUAL_INT(TOOL_TAKE_OK, tool_call_challenge_take_unsent(30, code, NULL, 0, NULL));
   const char *wrong = strcmp(code, "000000") == 0 ? "111111" : "000000";
   for (int i = 0; i < TOOL_CALL_CHALLENGE_TRIES; i++) {
      tool_call_challenge_redeem(30, 1, wrong, &out, NULL);
   }
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_END_VOIDED, tool_call_challenge_take_ended(30));
   /* Each is said once. */
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_END_NONE, tool_call_challenge_take_ended(30));
   TEST_ASSERT_FALSE(tool_call_challenge_ended_recently(30));

   /* No channel, no code. */
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_FULL, hold(0, 1, "nowhere"));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_code_in_text);
   RUN_TEST(test_digest);
   RUN_TEST(test_create_send_redeem);
   RUN_TEST(test_newer_replaces);
   RUN_TEST(test_code_before_sent);
   RUN_TEST(test_expired_before_sent);
   RUN_TEST(test_wrong_codes_void);
   RUN_TEST(test_same_call);
   RUN_TEST(test_cancel_unsent_and_earlier);
   RUN_TEST(test_used_codes_refunded);
   RUN_TEST(test_end_reasons);
   RUN_TEST(test_cancel_limit_and_window);
   RUN_TEST(test_excerpt_line);
   return UNITY_END();
}
