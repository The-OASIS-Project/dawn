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
 * email_render_message: an email as the model reads it.
 */

#include <stdlib.h>
#include <string.h>

#include "tools/email_render.h"
#include "unity.h"

void setUp(void) {
}

void tearDown(void) {
}

static email_message_t message(void) {
   email_message_t m;
   memset(&m, 0, sizeof(m));
   strcpy(m.from_name, "Bob Smith");
   strcpy(m.from_addr, "bob@example.com");
   strcpy(m.subject, "Lunch");
   strcpy(m.date_str, "Mon, 5 Oct 2026 12:00:00 +0000");
   return m;
}

static void test_headers_and_body(void) {
   email_message_t m = message();
   char body[] = "See you at noon.";
   m.body = body;
   m.body_len = (int)strlen(body);
   char *out = email_render_message(&m);
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_NOT_NULL(strstr(out, "From: \"Bob Smith\" <bob@example.com>\n"));
   TEST_ASSERT_NOT_NULL(strstr(out, "Subject: Lunch\nDate: Mon, 5 Oct 2026"));
   TEST_ASSERT_NOT_NULL(strstr(out, "\n\nSee you at noon."));
   TEST_ASSERT_NULL(strstr(out, "Reply-To"));
   TEST_ASSERT_NULL(strstr(out, "[Message truncated]"));
   free(out);
}

/* A name that is just the address is shown once; more recipients than kept
 * are counted; a Reply-To shows only when it differs from From. */
static void test_addresses(void) {
   email_message_t m = message();
   email_addr_t to[2] = { { "ann@example.com", "ann@example.com" }, { "Cy", "cy@example.com" } };
   m.to_list = to;
   m.to_count = 2;
   m.to_total = 5;
   strcpy(m.reply_to.addr, "BOB@example.com");
   char *out = email_render_message(&m);
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_NOT_NULL(strstr(out, "To: ann@example.com, Cy <cy@example.com> (and 3 more)\n"));
   TEST_ASSERT_NULL(strstr(out, "Reply-To"));
   free(out);
   strcpy(m.reply_to.addr, "mallory@example.net");
   out = email_render_message(&m);
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_NOT_NULL(strstr(out, "Reply-To: mallory@example.net\n"));
   free(out);
}

static void test_attachments_and_sizes(void) {
   email_message_t m = message();
   email_attachment_t a[4];
   memset(a, 0, sizeof(a));
   strcpy(a[0].filename, "notes.txt");
   strcpy(a[0].mime, "text/plain");
   a[0].size = 1023;
   strcpy(a[1].mime, "image/png");
   a[1].size = 1024;
   a[1].is_inline = true;
   strcpy(a[2].filename, "big.pdf");
   strcpy(a[2].mime, "application/pdf");
   a[2].size = 1024 * 1024;
   m.attachments = a;
   m.attachment_count = 3;
   m.attachments_truncated = true;
   m.text_truncated = true;
   char *out = email_render_message(&m);
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_NOT_NULL(strstr(out, "  1. notes.txt (text/plain, 1023 B)\n"));
   TEST_ASSERT_NOT_NULL(strstr(out, "  2. (unnamed) (image/png, 1 KB, inline)\n"));
   TEST_ASSERT_NOT_NULL(strstr(out, "  3. big.pdf (application/pdf, 1.0 MB)\n"));
   TEST_ASSERT_NOT_NULL(strstr(out, "  (more attachments not listed)\n"));
   TEST_ASSERT_NOT_NULL(strstr(out, "\n(No body)\n[Message truncated]"));
   free(out);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_headers_and_body);
   RUN_TEST(test_addresses);
   RUN_TEST(test_attachments_and_sizes);
   return UNITY_END();
}
