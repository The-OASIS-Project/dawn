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
 * Who may speak for a messaging channel: the sender rule, and Telegram
 * messages read into a sender (none when no single person sent them).
 */

#include <json-c/json.h>
#include <string.h>

#include "messaging/messaging_sender_gate.h"
#include "messaging/messaging_telegram_parse.h"
#include "unity.h"

void setUp(void) {
}

void tearDown(void) {
}

/* --- the sender rule ----------------------------------------------------- */

#define CHAT_APP true /* the provider vouches for senders */
#define SMS false

static void test_gate_owner_matches(void) {
   TEST_ASSERT_EQUAL_INT(MESSAGING_SENDER_ALLOW,
                         messaging_sender_gate("111", "111", MESSAGING_CHAT_SHARED, CHAT_APP));
   TEST_ASSERT_EQUAL_INT(MESSAGING_SENDER_ALLOW,
                         messaging_sender_gate("111", "111", MESSAGING_CHAT_ONE_TO_ONE, CHAT_APP));
}

static void test_gate_other_sender_denied(void) {
   TEST_ASSERT_EQUAL_INT(MESSAGING_SENDER_DENY,
                         messaging_sender_gate("111", "222", MESSAGING_CHAT_SHARED, CHAT_APP));
   TEST_ASSERT_EQUAL_INT(MESSAGING_SENDER_DENY,
                         messaging_sender_gate("111", NULL, MESSAGING_CHAT_ONE_TO_ONE, CHAT_APP));
   TEST_ASSERT_EQUAL_INT(MESSAGING_SENDER_DENY,
                         messaging_sender_gate("111", "", MESSAGING_CHAT_ONE_TO_ONE, CHAT_APP));
}

/* A group linked before owners were recorded answers no one until re-linked. */
static void test_gate_unowned_shared_needs_relink(void) {
   TEST_ASSERT_EQUAL_INT(MESSAGING_SENDER_RELINK,
                         messaging_sender_gate(NULL, "222", MESSAGING_CHAT_SHARED, CHAT_APP));
   TEST_ASSERT_EQUAL_INT(MESSAGING_SENDER_RELINK,
                         messaging_sender_gate("", NULL, MESSAGING_CHAT_SHARED, CHAT_APP));
}

/* A one-to-one chat names its owner with its first message. */
static void test_gate_unowned_one_to_one(void) {
   TEST_ASSERT_EQUAL_INT(MESSAGING_SENDER_BIND,
                         messaging_sender_gate(NULL, "333", MESSAGING_CHAT_ONE_TO_ONE, CHAT_APP));
}

/* A chat app that names no sender is refused, not let in. */
static void test_gate_chat_app_without_sender_denied(void) {
   TEST_ASSERT_EQUAL_INT(MESSAGING_SENDER_DENY,
                         messaging_sender_gate(NULL, NULL, MESSAGING_CHAT_ONE_TO_ONE, CHAT_APP));
   TEST_ASSERT_EQUAL_INT(MESSAGING_SENDER_DENY,
                         messaging_sender_gate(NULL, "", MESSAGING_CHAT_ONE_TO_ONE, CHAT_APP));
}

/* SMS: the chat is the number; a sender id it might carry means nothing and
 * nothing is bound. */
static void test_gate_sms(void) {
   TEST_ASSERT_EQUAL_INT(MESSAGING_SENDER_ALLOW,
                         messaging_sender_gate(NULL, NULL, MESSAGING_CHAT_ONE_TO_ONE, SMS));
   TEST_ASSERT_EQUAL_INT(MESSAGING_SENDER_ALLOW,
                         messaging_sender_gate(NULL, "999", MESSAGING_CHAT_ONE_TO_ONE, SMS));
}

/* --- Telegram messages --------------------------------------------------- */

static bool parse(const char *json, tg_message_t *m, const char **why) {
   struct json_object *o = json_tokener_parse(json);
   TEST_ASSERT_NOT_NULL(o);
   bool ok = tg_parse_message(o, m, why);
   if (ok) {
      /* body is borrowed: copy what the test checks before the parse goes */
      TEST_ASSERT_NOT_NULL(m->body);
   }
   json_object_put(o);
   return ok;
}

static void test_tg_private(void) {
   tg_message_t m;
   const char *why = NULL;
   TEST_ASSERT_TRUE(parse("{\"chat\":{\"id\":42,\"type\":\"private\",\"first_name\":\"A\"},"
                          "\"from\":{\"id\":42,\"is_bot\":false,\"first_name\":\"A\"},"
                          "\"date\":1700000000,\"text\":\"hi\"}",
                          &m, &why));
   TEST_ASSERT_EQUAL_STRING("42", m.chat_id);
   TEST_ASSERT_EQUAL_STRING("42", m.sender_id);
   TEST_ASSERT_EQUAL_STRING("A", m.sender_display);
   TEST_ASSERT_EQUAL_INT64(1700000000, m.timestamp);
   TEST_ASSERT_EQUAL_INT(MESSAGING_CHAT_ONE_TO_ONE, m.chat_kind);
}

/* No single Telegram-known person: passed on with no sender (it speaks for
 * no one, and a link code in it is used up), the reason given. */
static void test_tg_private_mismatch_no_sender(void) {
   tg_message_t m;
   const char *why = NULL;
   TEST_ASSERT_TRUE(parse("{\"chat\":{\"id\":42,\"type\":\"private\"},"
                          "\"from\":{\"id\":43},\"text\":\"hi\"}",
                          &m, &why));
   TEST_ASSERT_EQUAL_STRING("", m.sender_id);
   TEST_ASSERT_NOT_NULL(why);
}

static void test_tg_group(void) {
   tg_message_t m;
   const char *why = NULL;
   TEST_ASSERT_TRUE(parse("{\"chat\":{\"id\":-1001234,\"type\":\"supergroup\"},"
                          "\"from\":{\"id\":42,\"first_name\":\"A\"},\"text\":\"hi\"}",
                          &m, &why));
   TEST_ASSERT_EQUAL_STRING("-1001234", m.chat_id);
   TEST_ASSERT_EQUAL_STRING("42", m.sender_id);
   TEST_ASSERT_EQUAL_INT(MESSAGING_CHAT_SHARED, m.chat_kind);

   TEST_ASSERT_TRUE(parse("{\"chat\":{\"id\":-55,\"type\":\"group\"},"
                          "\"from\":{\"id\":7},\"text\":\"hi\"}",
                          &m, &why));
   TEST_ASSERT_EQUAL_INT(MESSAGING_CHAT_SHARED, m.chat_kind);
   TEST_ASSERT_EQUAL_STRING("telegram_user", m.sender_display);
}

/* Each of these is not one Telegram-known person speaking: passed on with no
 * sender. */
static void test_tg_no_person(void) {
   static const char *const cases[] = {
      /* anonymous group admin: from = GroupAnonymousBot, sender_chat = the group */
      "{\"chat\":{\"id\":-5,\"type\":\"supergroup\"},\"from\":{\"id\":1087968824,"
      "\"is_bot\":true},\"sender_chat\":{\"id\":-5},\"text\":\"hi\"}",
      /* automatic forward from a linked channel */
      "{\"chat\":{\"id\":-5,\"type\":\"supergroup\"},\"from\":{\"id\":777000},"
      "\"sender_chat\":{\"id\":-9},\"is_automatic_forward\":true,\"text\":\"hi\"}",
      /* forwarded (current and older field names) */
      "{\"chat\":{\"id\":42,\"type\":\"private\"},\"from\":{\"id\":42},"
      "\"forward_origin\":{\"type\":\"user\"},\"text\":\"yes\"}",
      "{\"chat\":{\"id\":42,\"type\":\"private\"},\"from\":{\"id\":42},"
      "\"forward_from\":{\"id\":9},\"forward_date\":1,\"text\":\"yes\"}",
      "{\"chat\":{\"id\":42,\"type\":\"private\"},\"from\":{\"id\":42},"
      "\"forward_sender_name\":\"X\",\"forward_date\":1,\"text\":\"yes\"}",
      /* inline-bot result */
      "{\"chat\":{\"id\":42,\"type\":\"private\"},\"from\":{\"id\":42},"
      "\"via_bot\":{\"id\":5,\"is_bot\":true},\"text\":\"yes\"}",
      /* a bot sender */
      "{\"chat\":{\"id\":-5,\"type\":\"group\"},\"from\":{\"id\":8,\"is_bot\":true},"
      "\"text\":\"hi\"}",
      /* no sender */
      "{\"chat\":{\"id\":-5,\"type\":\"group\"},\"text\":\"hi\"}",
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      tg_message_t m;
      const char *why = NULL;
      TEST_ASSERT_TRUE_MESSAGE(parse(cases[i], &m, &why), cases[i]);
      TEST_ASSERT_EQUAL_STRING_MESSAGE("", m.sender_id, cases[i]);
      TEST_ASSERT_NOT_NULL_MESSAGE(why, cases[i]);
   }
}

/* A channel post is dropped outright. */
static void test_tg_channel_dropped(void) {
   tg_message_t m;
   const char *why = NULL;
   TEST_ASSERT_FALSE(
       parse("{\"chat\":{\"id\":-100,\"type\":\"channel\"},\"text\":\"hi\"}", &m, &why));
   TEST_ASSERT_NOT_NULL(why);
}

/* Not text: nothing to answer, and nothing worth logging. */
static void test_tg_not_text(void) {
   tg_message_t m;
   const char *why = "unset";
   TEST_ASSERT_FALSE(parse("{\"chat\":{\"id\":42,\"type\":\"private\"},\"from\":{\"id\":42},"
                           "\"photo\":[]}",
                           &m, &why));
   TEST_ASSERT_NULL(why);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_gate_owner_matches);
   RUN_TEST(test_gate_other_sender_denied);
   RUN_TEST(test_gate_unowned_shared_needs_relink);
   RUN_TEST(test_gate_unowned_one_to_one);
   RUN_TEST(test_gate_chat_app_without_sender_denied);
   RUN_TEST(test_gate_sms);
   RUN_TEST(test_tg_private);
   RUN_TEST(test_tg_private_mismatch_no_sender);
   RUN_TEST(test_tg_group);
   RUN_TEST(test_tg_no_person);
   RUN_TEST(test_tg_channel_dropped);
   RUN_TEST(test_tg_not_text);
   return UNITY_END();
}
