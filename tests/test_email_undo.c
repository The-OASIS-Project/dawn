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
 * Undo tokens for trash and archive: bound to user and account, single use,
 * expiry (in-flight tokens exempt), the per-user and global caps.
 */

#include <sodium.h>
#include <stdio.h>
#include <string.h>

#include "tools/email_undo.h"
#include "unity.h"

#define T0 1000000

void setUp(void) {
   email_undo_reset();
   email_undo_set_clock(T0);
}

void tearDown(void) {
   email_undo_reset();
   email_undo_set_clock(0);
}

static email_undo_rec_t rec_for(int64_t account_id, uint32_t uid) {
   email_undo_rec_t r;
   memset(&r, 0, sizeof(r));
   r.account_id = account_id;
   r.fingerprint = 0x1234;
   r.kind = EMAIL_MOVE_TRASH;
   r.imap = true;
   snprintf(r.src_folder, sizeof(r.src_folder), "INBOX");
   snprintf(r.dest_folder, sizeof(r.dest_folder), "Trash");
   r.dest_uid = uid;
   r.dest_uidvalidity = 7;
   return r;
}

static void test_claim_once_then_finish(void) {
   const email_undo_rec_t r = rec_for(3, 41);
   char tok[EMAIL_UNDO_TOKEN_LEN + 1];
   TEST_ASSERT_TRUE(email_undo_put(1, &r, tok));
   TEST_ASSERT_EQUAL_size_t(EMAIL_UNDO_TOKEN_LEN, strlen(tok));
   email_undo_rec_t out;
   TEST_ASSERT_TRUE(email_undo_claim(1, 3, tok, &out));
   TEST_ASSERT_EQUAL_UINT32(41, out.dest_uid);
   TEST_ASSERT_EQUAL_STRING("Trash", out.dest_folder);
   /* In flight: a second claim is refused. */
   TEST_ASSERT_FALSE(email_undo_claim(1, 3, tok, &out));
   email_undo_finish(1, tok);
   TEST_ASSERT_FALSE(email_undo_claim(1, 3, tok, &out));
}

static void test_release_allows_retry(void) {
   const email_undo_rec_t r = rec_for(3, 41);
   char tok[EMAIL_UNDO_TOKEN_LEN + 1];
   TEST_ASSERT_TRUE(email_undo_put(1, &r, tok));
   email_undo_rec_t out;
   TEST_ASSERT_TRUE(email_undo_claim(1, 3, tok, &out));
   email_undo_release(1, tok);
   TEST_ASSERT_TRUE(email_undo_claim(1, 3, tok, &out));
}

static void test_bound_to_user_and_account(void) {
   const email_undo_rec_t r = rec_for(3, 41);
   char tok[EMAIL_UNDO_TOKEN_LEN + 1];
   TEST_ASSERT_TRUE(email_undo_put(1, &r, tok));
   email_undo_rec_t out;
   TEST_ASSERT_FALSE(email_undo_claim(2, 3, tok, &out));
   TEST_ASSERT_FALSE(email_undo_claim(1, 4, tok, &out));
   /* Another user can't finish or release it either. */
   email_undo_finish(2, tok);
   TEST_ASSERT_TRUE(email_undo_claim(1, 3, tok, &out));
}

static void test_bad_tokens_refused(void) {
   email_undo_rec_t out;
   TEST_ASSERT_FALSE(email_undo_claim(1, 3, NULL, &out));
   TEST_ASSERT_FALSE(email_undo_claim(1, 3, "", &out));
   TEST_ASSERT_FALSE(email_undo_claim(1, 3, "abc", &out));
   TEST_ASSERT_FALSE(email_undo_claim(1, 3, "0123456789ABCDEF0123456789abcdef", &out));
   TEST_ASSERT_FALSE(email_undo_claim(1, 3, "0123456789abcdef0123456789abcdef0", &out));
   TEST_ASSERT_FALSE(email_undo_claim(1, 3, "0123456789abcdef0123456789abcdef", &out));
}

static void test_expiry_and_in_flight_exemption(void) {
   const email_undo_rec_t r = rec_for(3, 41);
   char a[EMAIL_UNDO_TOKEN_LEN + 1], b[EMAIL_UNDO_TOKEN_LEN + 1];
   TEST_ASSERT_TRUE(email_undo_put(1, &r, a));
   TEST_ASSERT_TRUE(email_undo_put(1, &r, b));
   email_undo_rec_t out;
   TEST_ASSERT_TRUE(email_undo_claim(1, 3, b, &out));
   email_undo_set_clock(T0 + EMAIL_UNDO_TTL_SEC - 1);
   char c[EMAIL_UNDO_TOKEN_LEN + 1];
   TEST_ASSERT_TRUE(email_undo_put(1, &r, c));
   email_undo_set_clock(T0 + EMAIL_UNDO_TTL_SEC);
   TEST_ASSERT_FALSE(email_undo_claim(1, 3, a, &out)); /* expired */
   TEST_ASSERT_TRUE(email_undo_claim(1, 3, c, &out));  /* younger */
   /* b was in flight past its time: a put's sweep doesn't drop it. */
   TEST_ASSERT_TRUE(email_undo_put(1, &r, a));
   email_undo_release(1, b);
   TEST_ASSERT_FALSE(email_undo_claim(1, 3, b, &out)); /* released, now past its time */
}

/* A claimed token whose caller never finishes or releases it doesn't hold its
 * slot forever: it goes twice the window after the claim. */
static void test_in_flight_ages_out(void) {
   const email_undo_rec_t r = rec_for(3, 51);
   char a[EMAIL_UNDO_TOKEN_LEN + 1];
   TEST_ASSERT_TRUE(email_undo_put(1, &r, a));
   email_undo_rec_t out;
   email_undo_set_clock(T0 + 10);
   TEST_ASSERT_TRUE(email_undo_claim(1, 3, a, &out));
   char b[EMAIL_UNDO_TOKEN_LEN + 1];
   email_undo_set_clock(T0 + 10 + 2 * EMAIL_UNDO_TTL_SEC - 1);
   TEST_ASSERT_TRUE(email_undo_put(1, &r, b));                 /* sweep: a still held */
   email_undo_release(1, a);                                   /* releasing it works... */
   TEST_ASSERT_TRUE(email_undo_claim(1, 3, a, &out) == false); /* ...but its window is gone */
   TEST_ASSERT_TRUE(email_undo_put(1, &r, a));
   TEST_ASSERT_TRUE(email_undo_claim(1, 3, a, &out));
   email_undo_set_clock(T0 + 10 + 2 * EMAIL_UNDO_TTL_SEC - 1 + 2 * EMAIL_UNDO_TTL_SEC);
   TEST_ASSERT_TRUE(email_undo_put(1, &r, b)); /* sweep drops the stale claim */
   email_undo_release(1, a);
   TEST_ASSERT_FALSE(email_undo_claim(1, 3, a, &out));
}

static void test_per_user_cap_evicts_own_oldest(void) {
   const email_undo_rec_t r = rec_for(3, 41);
   char first[EMAIL_UNDO_TOKEN_LEN + 1], tok[EMAIL_UNDO_TOKEN_LEN + 1];
   TEST_ASSERT_TRUE(email_undo_put(1, &r, first));
   for (int i = 1; i < EMAIL_UNDO_PER_USER; i++) {
      email_undo_set_clock(T0 + (i > 30 ? 30 : 0));
      TEST_ASSERT_TRUE(email_undo_put(1, &r, tok));
   }
   /* Another user's token is untouched by user 1's churn. */
   char other[EMAIL_UNDO_TOKEN_LEN + 1];
   TEST_ASSERT_TRUE(email_undo_put(2, &r, other));
   TEST_ASSERT_TRUE(email_undo_put(1, &r, tok)); /* the 129th: drops user 1's oldest */
   email_undo_rec_t out;
   TEST_ASSERT_FALSE(email_undo_claim(1, 3, first, &out));
   TEST_ASSERT_TRUE(email_undo_claim(1, 3, tok, &out));
   TEST_ASSERT_TRUE(email_undo_claim(2, 3, other, &out));
}

static void test_per_user_cap_all_in_flight(void) {
   const email_undo_rec_t r = rec_for(3, 41);
   char tok[EMAIL_UNDO_TOKEN_LEN + 1];
   email_undo_rec_t out;
   for (int i = 0; i < EMAIL_UNDO_PER_USER; i++) {
      TEST_ASSERT_TRUE(email_undo_put(1, &r, tok));
      TEST_ASSERT_TRUE(email_undo_claim(1, 3, tok, &out));
   }
   TEST_ASSERT_FALSE(email_undo_put(1, &r, tok));
   TEST_ASSERT_EQUAL_CHAR('\0', tok[0]);
}

static void test_global_cap_no_cross_user_eviction(void) {
   const email_undo_rec_t r = rec_for(3, 41);
   char tok[EMAIL_UNDO_TOKEN_LEN + 1];
   const int users = EMAIL_UNDO_GLOBAL / EMAIL_UNDO_PER_USER;
   for (int u = 0; u < users; u++) {
      for (int i = 0; i < EMAIL_UNDO_PER_USER; i++)
         TEST_ASSERT_TRUE(email_undo_put(10 + u, &r, tok));
   }
   TEST_ASSERT_FALSE(email_undo_put(99, &r, tok));
   /* A user at the cap still makes room from their own. */
   TEST_ASSERT_TRUE(email_undo_put(10, &r, tok));
   email_undo_set_clock(T0 + EMAIL_UNDO_TTL_SEC);
   TEST_ASSERT_TRUE(email_undo_put(99, &r, tok)); /* everything else expired */
}

int main(void) {
   if (sodium_init() < 0)
      return 1;
   UNITY_BEGIN();
   RUN_TEST(test_claim_once_then_finish);
   RUN_TEST(test_release_allows_retry);
   RUN_TEST(test_bound_to_user_and_account);
   RUN_TEST(test_bad_tokens_refused);
   RUN_TEST(test_expiry_and_in_flight_exemption);
   RUN_TEST(test_in_flight_ages_out);
   RUN_TEST(test_per_user_cap_evicts_own_oldest);
   RUN_TEST(test_per_user_cap_all_in_flight);
   RUN_TEST(test_global_cap_no_cross_user_eviction);
   return UNITY_END();
}
