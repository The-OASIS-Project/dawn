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
 * IMAP read state: FETCH (FLAGS) and STATUS (UNSEEN) replies, UID sets.
 */

#include <stdio.h>
#include <string.h>

#include "tools/email_imap_state.h"
#include "unity.h"

void setUp(void) {
}

void tearDown(void) {
}

static void test_fetch_seen_reads_each_uid(void) {
   const char *reply = "* 3 FETCH (UID 40 FLAGS (\\Seen \\Answered))\r\n"
                       "* 4 FETCH (FLAGS () UID 41)\r\n"
                       "* 5 FETCH (UID 42 FLAGS ($Junk \\Flagged))\r\n"
                       "A005 OK UID FETCH completed\r\n";
   TEST_ASSERT_EQUAL_INT(EMAIL_IMAP_UID_SEEN, email_imap_fetch_seen(reply, 40));
   TEST_ASSERT_EQUAL_INT(EMAIL_IMAP_UID_UNSEEN, email_imap_fetch_seen(reply, 41));
   TEST_ASSERT_EQUAL_INT(EMAIL_IMAP_UID_UNSEEN, email_imap_fetch_seen(reply, 42));
   /* Not in the reply: no such message */
   TEST_ASSERT_EQUAL_INT(EMAIL_IMAP_UID_ABSENT, email_imap_fetch_seen(reply, 43));
   TEST_ASSERT_EQUAL_INT(EMAIL_IMAP_UID_ABSENT, email_imap_fetch_seen(reply, 4));
}

static void test_fetch_seen_needs_the_uid_and_flags(void) {
   /* A sequence number equal to the UID isn't the UID */
   TEST_ASSERT_EQUAL_INT(EMAIL_IMAP_UID_ABSENT,
                         email_imap_fetch_seen("* 40 FETCH (FLAGS (\\Seen))\r\n", 40));
   /* A UID with no FLAGS says nothing about the state */
   TEST_ASSERT_EQUAL_INT(EMAIL_IMAP_UID_ABSENT,
                         email_imap_fetch_seen("* 1 FETCH (UID 40)\r\n", 40));
   /* \SeenLater is a keyword, not \Seen; UIDNEXT isn't UID */
   TEST_ASSERT_EQUAL_INT(EMAIL_IMAP_UID_UNSEEN,
                         email_imap_fetch_seen("* 1 FETCH (UID 7 FLAGS (\\SeenLater))\r\n", 7));
   TEST_ASSERT_EQUAL_INT(EMAIL_IMAP_UID_ABSENT,
                         email_imap_fetch_seen("* OK [UIDNEXT 7] predicted\r\n", 7));
   TEST_ASSERT_EQUAL_INT(EMAIL_IMAP_UID_ABSENT, email_imap_fetch_seen(NULL, 7));
   TEST_ASSERT_EQUAL_INT(EMAIL_IMAP_UID_ABSENT, email_imap_fetch_seen("", 7));
   /* Case-insensitive keywords, LF-only lines */
   TEST_ASSERT_EQUAL_INT(EMAIL_IMAP_UID_SEEN,
                         email_imap_fetch_seen("* 2 fetch (uid 9 flags (\\seen))\n", 9));
}

static void test_status_unseen(void) {
   int n = -1;
   TEST_ASSERT_TRUE(email_imap_status_unseen("* STATUS INBOX (UNSEEN 12)\r\nA1 OK\r\n", &n));
   TEST_ASSERT_EQUAL_INT(12, n);
   TEST_ASSERT_TRUE(
       email_imap_status_unseen("* STATUS \"INBOX\" (MESSAGES 20 UNSEEN 0 UIDNEXT 9)\r\n", &n));
   TEST_ASSERT_EQUAL_INT(0, n);
   /* A mailbox name holding the word doesn't count */
   n = -1;
   TEST_ASSERT_FALSE(email_imap_status_unseen("* STATUS \"UNSEEN 5\" (MESSAGES 2)\r\n", &n));
   TEST_ASSERT_EQUAL_INT(-1, n);
   TEST_ASSERT_FALSE(email_imap_status_unseen("A1 NO no such mailbox\r\n", &n));
   TEST_ASSERT_FALSE(email_imap_status_unseen("* STATUS INBOX (UNSEEN x)\r\n", &n));
   TEST_ASSERT_FALSE(email_imap_status_unseen(NULL, &n));
}

static void test_numbers_start_right_after_the_keyword(void) {
   /* strtoul/strtol would skip the line break and read the next line's number */
   TEST_ASSERT_EQUAL_INT(EMAIL_IMAP_UID_ABSENT,
                         email_imap_fetch_seen("* 1 FETCH (FLAGS (\\Seen) UID \r\n40)\r\n", 40));
   int n = -1;
   TEST_ASSERT_FALSE(email_imap_status_unseen("* STATUS INBOX (UNSEEN \r\n7)\r\n", &n));
   TEST_ASSERT_EQUAL_INT(-1, n);
}

static void test_seen_found_in_a_long_flags_group(void) {
   /* Many keywords before \Seen: the whole group is read, not its first 511 bytes */
   char reply[2048];
   size_t pos = (size_t)snprintf(reply, sizeof(reply), "* 1 FETCH (UID 5 FLAGS (");
   for (int i = 0; i < 80; i++)
      pos += (size_t)snprintf(reply + pos, sizeof(reply) - pos, "$Keyword%02d ", i);
   snprintf(reply + pos, sizeof(reply) - pos, "\\Seen))\r\n");
   TEST_ASSERT_TRUE(strlen(reply) > 600);
   TEST_ASSERT_EQUAL_INT(EMAIL_IMAP_UID_SEEN, email_imap_fetch_seen(reply, 5));
}

static void test_status_counts_only_inbox(void) {
   int n = -1;
   /* An unsolicited STATUS for another mailbox is not the INBOX's count */
   TEST_ASSERT_FALSE(email_imap_status_unseen("* STATUS Archive (UNSEEN 99)\r\n", &n));
   TEST_ASSERT_FALSE(email_imap_status_unseen("* STATUS \"INBOX.Sub\" (UNSEEN 9)\r\n", &n));
   TEST_ASSERT_EQUAL_INT(-1, n);
   /* ...but it's skipped, not fatal, when the INBOX's line follows */
   TEST_ASSERT_TRUE(email_imap_status_unseen(
       "* STATUS Archive (UNSEEN 99)\r\n* STATUS inbox (UNSEEN 4)\r\n", &n));
   TEST_ASSERT_EQUAL_INT(4, n);
}

static void test_status_ignores_an_unclosed_name(void) {
   int n = -1;
   TEST_ASSERT_FALSE(email_imap_status_unseen("* STATUS \"INBOX\r\n", &n));
   TEST_ASSERT_FALSE(email_imap_status_unseen("* STATUS \"INBOX (UNSEEN 4)", &n));
}

static void test_a_refused_select_isnt_a_refused_login(void) {
   /* Login-denied with no LOGIN sent in that perform: the folder was refused. */
   TEST_ASSERT_TRUE(email_imap_select_failed(true, 1, 1));
   /* A LOGIN went out (a reconnect): it may have been the login. */
   TEST_ASSERT_FALSE(email_imap_select_failed(true, 1, 2));
   /* Not the login-denied code at all. */
   TEST_ASSERT_FALSE(email_imap_select_failed(false, 1, 1));
}

static void test_uid_set(void) {
   char out[64];
   const uint32_t uids[] = { 12, 15, 4000000000u };
   TEST_ASSERT_EQUAL_size_t(16, email_imap_uid_set(uids, 3, out, sizeof(out)));
   TEST_ASSERT_EQUAL_STRING("12,15,4000000000", out);
   TEST_ASSERT_EQUAL_size_t(2, email_imap_uid_set(uids, 1, out, sizeof(out)));
   TEST_ASSERT_EQUAL_STRING("12", out);
   /* Doesn't fit, empty, or a zero UID: nothing */
   char tiny[6];
   TEST_ASSERT_EQUAL_size_t(0, email_imap_uid_set(uids, 3, tiny, sizeof(tiny)));
   TEST_ASSERT_EQUAL_size_t(0, email_imap_uid_set(uids, 0, out, sizeof(out)));
   const uint32_t zero[] = { 5, 0 };
   TEST_ASSERT_EQUAL_size_t(0, email_imap_uid_set(zero, 2, out, sizeof(out)));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_fetch_seen_reads_each_uid);
   RUN_TEST(test_fetch_seen_needs_the_uid_and_flags);
   RUN_TEST(test_status_unseen);
   RUN_TEST(test_numbers_start_right_after_the_keyword);
   RUN_TEST(test_seen_found_in_a_long_flags_group);
   RUN_TEST(test_status_counts_only_inbox);
   RUN_TEST(test_status_ignores_an_unclosed_name);
   RUN_TEST(test_a_refused_select_isnt_a_refused_login);
   RUN_TEST(test_uid_set);
   return UNITY_END();
}
