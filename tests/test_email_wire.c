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
 * The mail panel's wire shapes: rows, read messages, and a read frame that
 * stays inside its budget.
 */

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "webui/email_wire.h"

void setUp(void) {
}
void tearDown(void) {
}

static json_object *get(json_object *o, const char *key) {
   json_object *v = NULL;
   return json_object_object_get_ex(o, key, &v) ? v : NULL;
}

static void test_row(void) {
   email_summary_t s;
   memset(&s, 0, sizeof(s));
   snprintf(s.message_id, sizeof(s.message_id), "INBOX:42");
   snprintf(s.from_name, sizeof(s.from_name), "Ann");
   snprintf(s.from_addr, sizeof(s.from_addr), "ann@example.com");
   snprintf(s.subject, sizeof(s.subject), "Hi");
   s.date = 1700000000;
   s.unread = true;
   s.starred = true;

   json_object *r = email_wire_row(7, &s, false);
   TEST_ASSERT_EQUAL_INT64(7, json_object_get_int64(get(r, "account_id")));
   TEST_ASSERT_EQUAL_STRING("INBOX:42", json_object_get_string(get(r, "message_id")));
   TEST_ASSERT_EQUAL_INT64(1700000000, json_object_get_int64(get(r, "date")));
   TEST_ASSERT_TRUE(json_object_get_boolean(get(r, "unread")));
   TEST_ASSERT_NULL(get(r, "thread_id"));
   TEST_ASSERT_NULL(get(r, "starred")); /* IMAP doesn't know it */
   TEST_ASSERT_EQUAL_STRING("", json_object_get_string(get(r, "preview")));
   json_object_put(r);

   r = email_wire_row(7, &s, true);
   TEST_ASSERT_TRUE(json_object_get_boolean(get(r, "starred")));
   TEST_ASSERT_FALSE(json_object_get_boolean(get(r, "important")));
   json_object_put(r);
}

static void message(email_message_t *m, char *body, char *html) {
   memset(m, 0, sizeof(*m));
   snprintf(m->message_id, sizeof(m->message_id), "18abc");
   snprintf(m->thread_id, sizeof(m->thread_id), "18abd");
   snprintf(m->subject, sizeof(m->subject), "Report");
   snprintf(m->date_str, sizeof(m->date_str), "Tue, 14 Nov 2023 22:13:20 +0000");
   m->body = body;
   m->body_len = (int)strlen(body);
   m->body_html = html;
   m->body_html_len = html ? strlen(html) : 0;
}

static void test_read_payload(void) {
   email_message_t m;
   message(&m, "hello", "<p>hello</p>");
   email_addr_t to[2] = { { "Bo", "bo@example.com" }, { "", "cy@example.com" } };
   m.to_list = to;
   m.to_count = 2;
   m.to_total = 40;
   snprintf(m.reply_to.addr, sizeof(m.reply_to.addr), "lists@example.com");
   email_attachment_t at = { .part_id = "2",
                             .filename = "a.pdf",
                             .mime = "application/pdf",
                             .size = 1234 };
   m.attachments = &at;
   m.attachment_count = 1;

   json_object *p = email_wire_read_payload(3, &m, true, EMAIL_PANEL_FRAME_MAX);
   json_object *msg = get(p, "message");
   TEST_ASSERT_TRUE(json_object_get_boolean(get(p, "unread")));
   TEST_ASSERT_EQUAL_INT64(3, json_object_get_int64(get(msg, "account_id")));
   TEST_ASSERT_EQUAL_STRING("18abc", json_object_get_string(get(msg, "message_id")));
   TEST_ASSERT_EQUAL_STRING("hello", json_object_get_string(get(msg, "body_text")));
   TEST_ASSERT_EQUAL_STRING("<p>hello</p>", json_object_get_string(get(msg, "body_html")));
   TEST_ASSERT_FALSE(json_object_get_boolean(get(msg, "html_truncated")));
   TEST_ASSERT_EQUAL_INT(2, (int)json_object_array_length(get(msg, "to")));
   TEST_ASSERT_EQUAL_INT(40, json_object_get_int(get(msg, "to_total")));
   TEST_ASSERT_NULL(get(msg, "cc_total"));
   TEST_ASSERT_EQUAL_STRING("lists@example.com",
                            json_object_get_string(get(get(msg, "reply_to"), "addr")));
   /* No internal date: the Date header's. */
   TEST_ASSERT_EQUAL_INT64(1700000000, json_object_get_int64(get(msg, "date")));
   json_object *a0 = json_object_array_get_idx(get(msg, "attachments"), 0);
   TEST_ASSERT_EQUAL_STRING("2", json_object_get_string(get(a0, "part_id")));
   TEST_ASSERT_FALSE(json_object_get_boolean(get(a0, "inline")));
   TEST_ASSERT_NULL(get(a0, "content_id"));
   TEST_ASSERT_NULL(get(msg, "attachments_truncated"));
   json_object_put(p);
}

/* Valid UTF-8 throughout (no character cut in half). */
static bool utf8_ok(const char *s) {
   for (const unsigned char *p = (const unsigned char *)s; *p;) {
      int n = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : (*p >> 3) == 30 ? 4 : 0;
      if (!n)
         return false;
      for (int i = 1; i < n; i++)
         if ((p[i] & 0xC0) != 0x80)
            return false;
      p += n;
   }
   return true;
}

static void test_html_is_cut_to_the_frame(void) {
   /* 20,000 three-byte characters, plus quotes that escaping doubles. */
   const size_t chars = 20000;
   char *html = malloc(chars * 3 + 64);
   size_t pos = 0;
   for (size_t i = 0; i < chars; i++) {
      if (i % 50 == 0) {
         html[pos++] = '"';
      } else {
         memcpy(html + pos, "\xe2\x82\xac", 3); /* € */
         pos += 3;
      }
   }
   html[pos] = '\0';
   email_message_t m;
   message(&m, "text", html);

   const size_t budget = 20000;
   json_object *p = email_wire_read_payload(1, &m, false, budget);
   size_t len = 0;
   json_object_to_json_string_length(p, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE,
                                     &len);
   TEST_ASSERT_TRUE(len + 200 <= budget); /* room left for the frame around it */
   json_object *msg = get(p, "message");
   TEST_ASSERT_TRUE(json_object_get_boolean(get(msg, "html_truncated")));
   const char *cut = json_object_get_string(get(msg, "body_html"));
   TEST_ASSERT_NOT_NULL(cut);
   TEST_ASSERT_TRUE(strlen(cut) > 1000);
   TEST_ASSERT_TRUE(utf8_ok(cut));
   json_object_put(p);

   /* No room at all: the HTML goes, the rest stays. */
   p = email_wire_read_payload(1, &m, false, 300);
   msg = get(p, "message");
   TEST_ASSERT_NULL(get(msg, "body_html"));
   TEST_ASSERT_TRUE(json_object_get_boolean(get(msg, "html_truncated")));
   TEST_ASSERT_EQUAL_STRING("text", json_object_get_string(get(msg, "body_text")));
   json_object_put(p);
   free(html);
}

static void test_the_escape_count_matches_json_c(void) {
   /* Bytes json-c escapes to two and six characters, and '/' (not escaped here). */
   const size_t n = 9000;
   char *html = malloc(n + 1);
   const char pattern[] = "a/\x01\n\\\"<";
   for (size_t i = 0; i < n; i++)
      html[i] = pattern[i % (sizeof(pattern) - 1)];
   html[n] = '\0';
   email_message_t m;
   message(&m, "", html);
   const size_t budget = 30000;
   json_object *p = email_wire_read_payload(1, &m, false, budget);
   size_t len = 0;
   json_object_to_json_string_length(p, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE,
                                     &len);
   TEST_ASSERT_TRUE(len + 200 <= budget);
   /* It fits whole (9000 bytes escape to about 22,000): nothing was cut. */
   json_object *msg = get(p, "message");
   TEST_ASSERT_FALSE(json_object_get_boolean(get(msg, "html_truncated")));
   TEST_ASSERT_EQUAL_size_t(n, strlen(json_object_get_string(get(msg, "body_html"))));
   json_object_put(p);
   free(html);
}

static void test_words_for_codes(void) {
   TEST_ASSERT_EQUAL_STRING("ok", email_wire_account_status(EMAIL_ERR_NONE));
   TEST_ASSERT_EQUAL_STRING("auth_revoked", email_wire_account_status(EMAIL_ERR_AUTH_REVOKED));
   TEST_ASSERT_EQUAL_STRING("auth_failed", email_wire_account_status(EMAIL_ERR_AUTH_FAILED));
   TEST_ASSERT_EQUAL_STRING("unreachable", email_wire_account_status(EMAIL_ERR_TIMEOUT));
   TEST_ASSERT_EQUAL_STRING("unreachable", email_wire_account_status(EMAIL_ERR_RATE_LIMITED));
   for (int e = EMAIL_ERR_FAILED; e <= EMAIL_ERR_UNAVAILABLE; e++)
      TEST_ASSERT_TRUE(email_wire_error_text((email_err_t)e)[0] != '\0');
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_row);
   RUN_TEST(test_read_payload);
   RUN_TEST(test_html_is_cut_to_the_frame);
   RUN_TEST(test_the_escape_count_matches_json_c);
   RUN_TEST(test_words_for_codes);
   return UNITY_END();
}
