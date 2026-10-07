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
#include "utils/string_utils.h"
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

/* Every string the panel sends is well-formed UTF-8: one bad byte from a
 * mis-declared charset or a cut preview would make the browser drop the socket. */
static bool well_formed(const char *s) {
   size_t n = 0;
   char *fixed = utf8_repair_dup(s, strlen(s), &n);
   const bool ok = fixed == NULL;
   free(fixed);
   return ok;
}

static void test_bad_utf8_never_goes_out(void) {
   email_summary_t s;
   memset(&s, 0, sizeof(s));
   snprintf(s.message_id, sizeof(s.message_id), "INBOX:42.7");
   snprintf(s.subject, sizeof(s.subject), "Caf\xe9 r\xe9sum\xe9"); /* Latin-1 */
   snprintf(s.preview, sizeof(s.preview), "ok \xc3");              /* cut mid-sequence */
   snprintf(s.from_name, sizeof(s.from_name), "Zo\xc3\xab");       /* fine as is */
   json_object *r = email_wire_row(1, &s, false);
   const char *subject = json_object_get_string(get(r, "subject"));
   TEST_ASSERT_TRUE(well_formed(subject));
   TEST_ASSERT_EQUAL_STRING("Caf\xef\xbf\xbd r\xef\xbf\xbdsum\xef\xbf\xbd", subject);
   TEST_ASSERT_EQUAL_STRING("ok \xef\xbf\xbd", json_object_get_string(get(r, "preview")));
   TEST_ASSERT_EQUAL_STRING("Zo\xc3\xab", json_object_get_string(get(r, "from_name")));
   TEST_ASSERT_TRUE(well_formed(json_object_to_json_string(r)));
   json_object_put(r);

   email_message_t m;
   char body[] = "line one\n\xff line two\t";
   char html[] = "<p>\xe9t\xe9</p>";
   message(&m, body, html);
   json_object *p = email_wire_read_payload(1, &m, true, 1 << 20);
   json_object *msg = get(p, "message");
   TEST_ASSERT_EQUAL_STRING("line one\n\xef\xbf\xbd line two\t",
                            json_object_get_string(get(msg, "body_text")));
   TEST_ASSERT_EQUAL_STRING("<p>\xef\xbf\xbdt\xef\xbf\xbd</p>",
                            json_object_get_string(get(msg, "body_html")));
   TEST_ASSERT_TRUE(well_formed(json_object_to_json_string(p)));
   json_object_put(p);
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

static void test_email_changed_frame(void) {
   email_summary_t rows[2];
   memset(rows, 0, sizeof(rows));
   snprintf(rows[0].message_id, sizeof(rows[0].message_id), "INBOX:12.7");
   snprintf(rows[0].subject, sizeof(rows[0].subject), "Back \xe9"); /* repaired */
   snprintf(rows[1].message_id, sizeof(rows[1].message_id), "18abc");
   rows[1].starred = true;
   const char *gone[] = { "Trash:4.9" };
   json_object *f = email_wire_changed(5, false, EMAIL_MOVE_TRASH, true, rows, 2, gone, 1, false);
   TEST_ASSERT_EQUAL_STRING("email_changed", json_object_get_string(get(f, "type")));
   json_object *p = get(f, "payload");
   TEST_ASSERT_EQUAL_INT64(5, json_object_get_int64(get(p, "account_id")));
   TEST_ASSERT_TRUE(json_object_object_get_ex(p, "state", NULL));
   TEST_ASSERT_NULL(get(p, "state"));
   TEST_ASSERT_EQUAL_STRING("trash", json_object_get_string(get(p, "kind")));
   TEST_ASSERT_TRUE(json_object_get_boolean(get(p, "undo")));
   json_object *created = get(p, "created");
   TEST_ASSERT_EQUAL_INT(2, (int)json_object_array_length(created));
   json_object *imap_row = json_object_array_get_idx(created, 0);
   TEST_ASSERT_EQUAL_STRING("Back \xef\xbf\xbd", json_object_get_string(get(imap_row, "subject")));
   TEST_ASSERT_NULL(get(imap_row, "starred")); /* IMAP: not known, whatever the row says */
   TEST_ASSERT_NULL(get(json_object_array_get_idx(created, 1), "starred"));
   TEST_ASSERT_EQUAL_INT(0, (int)json_object_array_length(get(p, "updated")));
   TEST_ASSERT_EQUAL_STRING("Trash:4.9", json_object_get_string(
                                             json_object_array_get_idx(get(p, "destroyed"), 0)));
   TEST_ASSERT_NULL(get(p, "refresh")); /* only when the panel should reload */
   json_object_put(f);

   /* A Gmail account's rows carry the flags. */
   f = email_wire_changed(5, true, EMAIL_MOVE_TRASH, true, &rows[1], 1, NULL, 0, false);
   p = get(f, "payload");
   TEST_ASSERT_TRUE(
       json_object_get_boolean(get(json_object_array_get_idx(get(p, "created"), 0), "starred")));
   json_object_put(f);

   f = email_wire_changed(5, true, EMAIL_MOVE_ARCHIVE, false, NULL, 0, NULL, 0, true);
   p = get(f, "payload");
   TEST_ASSERT_EQUAL_STRING("archive", json_object_get_string(get(p, "kind")));
   TEST_ASSERT_TRUE(json_object_get_boolean(get(p, "refresh")));
   json_object_put(f);
}

static void test_undo_tokens_are_checked(void) {
   TEST_ASSERT_TRUE(email_wire_undo_token_ok("0123456789abcdef0123456789abcdef"));
   TEST_ASSERT_FALSE(email_wire_undo_token_ok("0123456789ABCDEF0123456789abcdef"));  /* case */
   TEST_ASSERT_FALSE(email_wire_undo_token_ok("0123456789abcdef0123456789abcde"));   /* short */
   TEST_ASSERT_FALSE(email_wire_undo_token_ok("0123456789abcdef0123456789abcdef0")); /* long */
   TEST_ASSERT_FALSE(email_wire_undo_token_ok("0123456789abcdef0123456789abcdeg"));
   TEST_ASSERT_FALSE(email_wire_undo_token_ok(""));
   TEST_ASSERT_FALSE(email_wire_undo_token_ok(NULL));
}

static void test_move_payload(void) {
   const char *ids[] = { "INBOX:12", "INBOX:13", "INBOX:14", "INBOX:15" };
   email_move_result_t r[4];
   memset(r, 0, sizeof(r));
   r[0].outcome = EMAIL_MOVE_DONE;
   snprintf(r[0].message_id, sizeof(r[0].message_id), "INBOX:12.7"); /* canonical */
   snprintf(r[0].undo, sizeof(r[0].undo), "0123456789abcdef0123456789abcdef");
   r[1].outcome = EMAIL_MOVE_ALREADY_THERE; /* done, nothing to undo */
   snprintf(r[1].message_id, sizeof(r[1].message_id), "INBOX:13.7");
   r[2].outcome = EMAIL_MOVE_LEFT_FLAGGED;
   snprintf(r[2].message_id, sizeof(r[2].message_id), "INBOX:14.7");
   r[3].outcome = EMAIL_MOVE_FAILED;
   r[3].err = EMAIL_ERR_NOT_REMOVED;
   json_object *p = email_wire_move_payload(ids, r, 4);
   json_object *done = get(p, "done"), *failed = get(p, "failed");
   TEST_ASSERT_EQUAL_INT(3, (int)json_object_array_length(done));
   json_object *d0 = json_object_array_get_idx(done, 0);
   TEST_ASSERT_EQUAL_STRING("INBOX:12.7", json_object_get_string(get(d0, "message_id")));
   TEST_ASSERT_EQUAL_STRING("0123456789abcdef0123456789abcdef",
                            json_object_get_string(get(d0, "undo")));
   TEST_ASSERT_NULL(get(d0, "left_flagged"));
   json_object *d1 = json_object_array_get_idx(done, 1);
   TEST_ASSERT_TRUE(json_object_object_get_ex(d1, "undo", NULL)); /* present, null */
   TEST_ASSERT_NULL(get(d1, "undo"));
   TEST_ASSERT_TRUE(
       json_object_get_boolean(get(json_object_array_get_idx(done, 2), "left_flagged")));
   TEST_ASSERT_EQUAL_INT(1, (int)json_object_array_length(failed));
   json_object *f = json_object_array_get_idx(failed, 0);
   TEST_ASSERT_EQUAL_STRING("INBOX:15", json_object_get_string(get(f, "message_id"))); /* as sent */
   TEST_ASSERT_EQUAL_STRING("NOT_REMOVED", json_object_get_string(get(f, "error_code")));
   TEST_ASSERT_NOT_NULL(get(f, "error"));
   json_object_put(p);
}

static void test_undo_payload(void) {
   const char *tokens[] = { "aa", "bb", "cc", "dd" };
   email_undo_result_t r[3];
   memset(r, 0, sizeof(r));
   snprintf(r[0].row.message_id, sizeof(r[0].row.message_id), "18abc");
   r[0].row_ok = true;
   /* r[1]: restored, its row couldn't be read */
   r[2].err = EMAIL_ERR_NOT_FOUND;
   const email_undo_result_t *results[] = { &r[0], &r[1], &r[2], NULL };
   json_object *p = email_wire_undo_payload(9, tokens, results, 4, true);
   json_object *restored = get(p, "restored"), *failed = get(p, "failed");
   TEST_ASSERT_EQUAL_INT(2, (int)json_object_array_length(restored));
   json_object *o0 = json_object_array_get_idx(restored, 0);
   TEST_ASSERT_EQUAL_STRING("aa", json_object_get_string(get(o0, "undo")));
   TEST_ASSERT_EQUAL_STRING("18abc", json_object_get_string(get(get(o0, "row"), "message_id")));
   TEST_ASSERT_EQUAL_INT64(9, json_object_get_int64(get(get(o0, "row"), "account_id")));
   json_object *o1 = json_object_array_get_idx(restored, 1);
   TEST_ASSERT_TRUE(json_object_object_get_ex(o1, "row", NULL));
   TEST_ASSERT_NULL(get(o1, "row"));
   TEST_ASSERT_EQUAL_INT(2, (int)json_object_array_length(failed));
   json_object *f0 = json_object_array_get_idx(failed, 0);
   TEST_ASSERT_EQUAL_STRING("cc", json_object_get_string(get(f0, "undo")));
   TEST_ASSERT_EQUAL_STRING("NOT_FOUND", json_object_get_string(get(f0, "error_code")));
   json_object *f1 = json_object_array_get_idx(failed, 1); /* never claimed */
   TEST_ASSERT_EQUAL_STRING("dd", json_object_get_string(get(f1, "undo")));
   TEST_ASSERT_EQUAL_STRING("UNDO_EXPIRED", json_object_get_string(get(f1, "error_code")));
   json_object_put(p);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_row);
   RUN_TEST(test_read_payload);
   RUN_TEST(test_html_is_cut_to_the_frame);
   RUN_TEST(test_the_escape_count_matches_json_c);
   RUN_TEST(test_words_for_codes);
   RUN_TEST(test_bad_utf8_never_goes_out);
   RUN_TEST(test_email_changed_frame);
   RUN_TEST(test_undo_tokens_are_checked);
   RUN_TEST(test_move_payload);
   RUN_TEST(test_undo_payload);
   return UNITY_END();
}
