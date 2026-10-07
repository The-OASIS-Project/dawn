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
 * Reading a message: synthetic RFC 822 messages of the shapes real mail
 * takes, the hostile shapes it can take, and the same parts arriving the way
 * the Gmail API hands them over.
 */

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "tools/email_display.h"
#include "tools/email_mime.h"
#include "tools/gmail_client_internal.h"
#include "unity.h"

void setUp(void) {
}

void tearDown(void) {
}

static const email_read_opts_t TOOL = { .fetch_bytes = EMAIL_READ_FETCH_TOOL,
                                        .max_text_chars = 50000 };
static const email_read_opts_t PANEL = { .fetch_bytes = EMAIL_READ_FETCH_PANEL,
                                         .max_text_chars = 50000,
                                         .max_html_bytes = EMAIL_READ_HTML_PANEL,
                                         .want_html = true };

/* Parses @p text (copied: the parse points into its buffer) into @p m. */
static int parse(const char *text, bool cut, const email_read_opts_t *opts, email_message_t *m) {
   memset(m, 0, sizeof(*m));
   const size_t len = strlen(text);
   char *raw = malloc(len + 1);
   memcpy(raw, text, len + 1);
   const int rc = email_mime_parse_raw(raw, len, cut, opts, m);
   free(raw);
   return rc;
}

#define HDR                                                                        \
   "From: Ann Example <ann@example.com>\r\nTo: bob@example.org\r\nSubject: Hi\r\n" \
   "Date: Thu, 1 Oct 2026 10:00:00 +0000\r\nMIME-Version: 1.0\r\n"

static void test_plain(void) {
   email_message_t m;
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: text/plain\r\n\r\nHello there.\r\n", false,
                                  &TOOL, &m));
   TEST_ASSERT_EQUAL_STRING("Hello there.\r\n", m.body);
   TEST_ASSERT_EQUAL_INT((int)strlen(m.body), m.body_len);
   TEST_ASSERT_EQUAL_STRING("Ann Example", m.from_name);
   TEST_ASSERT_EQUAL_STRING("ann@example.com", m.from_addr);
   TEST_ASSERT_EQUAL_STRING("Hi", m.subject);
   TEST_ASSERT_EQUAL_STRING("Thu, 1 Oct 2026 10:00:00 +0000", m.date_str);
   TEST_ASSERT_EQUAL_INT(1, m.to_count);
   TEST_ASSERT_EQUAL_STRING("bob@example.org", m.to_list[0].addr);
   TEST_ASSERT_EQUAL_INT(0, m.attachment_count);
   TEST_ASSERT_FALSE(m.text_truncated);
   TEST_ASSERT_NULL(m.body_html);
   email_message_free(&m);
}

static const char *ALT = HDR
    "Content-Type: multipart/alternative; boundary=b1\r\n\r\n"
    "--b1\r\nContent-Type: text/plain; charset=utf-8\r\n"
    "Content-Transfer-Encoding: base64\r\n\r\nSGVsbG8sIHdvcmxkIOKAlCBkYXNo\r\n"
    "--b1\r\nContent-Type: text/html; charset=utf-8\r\n"
    "Content-Transfer-Encoding: base64\r\n\r\n"
    "PHA+SGVsbG8sIDxiPndvcmxkPC9iPjwvcD4=\r\n--b1--\r\n";

/* The baseline case: a base64 multipart/alternative reads correctly. */
static void test_alternative_base64(void) {
   email_message_t m;
   TEST_ASSERT_EQUAL_INT(0, parse(ALT, false, &PANEL, &m));
   TEST_ASSERT_EQUAL_STRING("Hello, world \xE2\x80\x94 dash", m.body);
   TEST_ASSERT_EQUAL_STRING("<p>Hello, <b>world</b></p>", m.body_html);
   TEST_ASSERT_EQUAL_INT(0, m.attachment_count);
   email_message_free(&m);
   /* The tool never gets HTML. */
   TEST_ASSERT_EQUAL_INT(0, parse(ALT, false, &TOOL, &m));
   TEST_ASSERT_NULL(m.body_html);
   TEST_ASSERT_EQUAL_STRING("Hello, world \xE2\x80\x94 dash", m.body);
   email_message_free(&m);
}

static void test_charsets(void) {
   email_message_t m;
   /* QP latin-1 */
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: text/plain; charset=iso-8859-1\r\n"
                                      "Content-Transfer-Encoding: quoted-printable\r\n\r\n"
                                      "Caf=E9 cr=E8me\r\n",
                                  false, &TOOL, &m));
   TEST_ASSERT_EQUAL_STRING("Caf\xC3\xA9 cr\xC3\xA8me\r\n", m.body);
   email_message_free(&m);
   /* windows-1252 smart quotes */
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: text/plain; charset=windows-1252\r\n\r\n"
                                      "\x93quoted\x94\r\n",
                                  false, &TOOL, &m));
   TEST_ASSERT_EQUAL_STRING("\xE2\x80\x9Cquoted\xE2\x80\x9D\r\n", m.body);
   email_message_free(&m);
   /* An unknown charset is read as windows-1252 */
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: text/plain; charset=x-made-up\r\n\r\n"
                                      "na\xEFve\r\n",
                                  false, &TOOL, &m));
   TEST_ASSERT_EQUAL_STRING("na\xC3\xAFve\r\n", m.body);
   email_message_free(&m);
   /* Undeclared 8-bit that isn't UTF-8: windows-1252 too */
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: text/plain\r\n\r\nna\xEFve\r\n", false, &TOOL,
                                  &m));
   TEST_ASSERT_EQUAL_STRING("na\xC3\xAFve\r\n", m.body);
   email_message_free(&m);
   /* Undeclared but valid UTF-8 stays as it is */
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: text/plain\r\n\r\nna\xC3\xAFve\r\n", false,
                                  &TOOL, &m));
   TEST_ASSERT_EQUAL_STRING("na\xC3\xAFve\r\n", m.body);
   email_message_free(&m);
   /* A NUL in the body doesn't end it early */
   const char nul[] = HDR "Content-Type: text/plain\r\n\r\nab\0cd\r\n";
   char *raw = malloc(sizeof(nul));
   memcpy(raw, nul, sizeof(nul));
   memset(&m, 0, sizeof(m));
   TEST_ASSERT_EQUAL_INT(0, email_mime_parse_raw(raw, sizeof(nul) - 1, false, &TOOL, &m));
   TEST_ASSERT_EQUAL_STRING("abcd\r\n", m.body);
   TEST_ASSERT_EQUAL_INT(6, m.body_len);
   email_message_free(&m);
   free(raw);
}

static void test_html_bodies(void) {
   email_message_t m;
   /* HTML only: read as text */
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: text/html\r\n\r\n"
                                      "<html><body><p>Your order shipped.</p>"
                                      "<script>x()</script></body></html>\r\n",
                                  false, &PANEL, &m));
   TEST_ASSERT_NOT_NULL(strstr(m.body, "Your order shipped."));
   TEST_ASSERT_NULL(strstr(m.body, "<p>"));
   TEST_ASSERT_NOT_NULL(m.body_html);
   email_message_free(&m);
   /* HTML inside a text/plain part */
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: text/plain\r\n\r\n"
                                      "<div>Pledge <a href=\"https://t.example/x\">confirmed</a>"
                                      "</div>\r\n",
                                  false, &TOOL, &m));
   TEST_ASSERT_NOT_NULL(strstr(m.body, "Pledge"));
   TEST_ASSERT_NULL(strstr(m.body, "<div>"));
   email_message_free(&m);
   /* A short HTML-only reply is the whole message, not an empty one */
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: text/html\r\n\r\n<p>Thanks!</p>\r\n", false,
                                  &TOOL, &m));
   TEST_ASSERT_EQUAL_STRING("Thanks!", m.body);
   email_message_free(&m);
   /* No body at all */
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: text/plain\r\n\r\n", false, &TOOL, &m));
   TEST_ASSERT_EQUAL_STRING("", m.body);
   email_message_free(&m);
}

static void test_attachments(void) {
   email_message_t m;
   TEST_ASSERT_EQUAL_INT(
       0, parse(HDR
                "Content-Type: multipart/mixed; boundary=m\r\n\r\n"
                "--m\r\nContent-Type: multipart/related; boundary=r\r\n\r\n"
                "--r\r\nContent-Type: text/html\r\n\r\n<img src=\"cid:logo@x\">Hi\r\n"
                "--r\r\nContent-Type: image/png\r\nContent-ID: <logo@x>\r\n"
                "Content-Transfer-Encoding: base64\r\n\r\niVBORw0KGgo=\r\n--r--\r\n"
                "--m\r\nContent-Type: application/pdf\r\n"
                "Content-Disposition: attachment; filename*=UTF-8''%2E%2E%2Finv%E2%80%AEfdp.exe\r\n"
                "Content-Transfer-Encoding: base64\r\n\r\nJVBERi0xLjQK\r\n"
                "--m\r\nContent-Type: text/plain; name=\"=?UTF-8?B?bm90ZXMudHh0?=\"\r\n"
                "Content-Disposition: attachment\r\n\r\nfile text\r\n"
                "--m\r\nContent-Type: message/rfc822\r\n\r\n"
                "Subject: inner\r\nContent-Type: multipart/mixed; boundary=i\r\n\r\n"
                "--i\r\nContent-Type: text/plain\r\n\r\ninner text\r\n--i--\r\n"
                "--m--\r\n",
                false, &PANEL, &m));
   TEST_ASSERT_EQUAL_INT(4, m.attachment_count);
   /* the inline image of the related HTML */
   TEST_ASSERT_EQUAL_STRING("1.2", m.attachments[0].part_id);
   TEST_ASSERT_EQUAL_STRING("image/png", m.attachments[0].mime);
   TEST_ASSERT_EQUAL_STRING("logo@x", m.attachments[0].content_id);
   TEST_ASSERT_TRUE(m.attachments[0].is_inline);
   /* the PDF: path separators and the right-to-left override are gone */
   TEST_ASSERT_EQUAL_STRING("2", m.attachments[1].part_id);
   TEST_ASSERT_EQUAL_STRING("application/pdf", m.attachments[1].mime);
   TEST_ASSERT_EQUAL_STRING(".._invfdp.exe", m.attachments[1].filename);
   TEST_ASSERT_EQUAL_UINT(9, m.attachments[1].size);
   TEST_ASSERT_FALSE(m.attachments[1].is_inline);
   /* an attached text file is a file, not body text; its RFC 2047 name decoded */
   TEST_ASSERT_EQUAL_STRING("3", m.attachments[2].part_id);
   TEST_ASSERT_EQUAL_STRING("notes.txt", m.attachments[2].filename);
   TEST_ASSERT_NULL(strstr(m.body, "file text"));
   /* a forwarded message is one attachment */
   TEST_ASSERT_EQUAL_STRING("4", m.attachments[3].part_id);
   TEST_ASSERT_EQUAL_STRING("message/rfc822", m.attachments[3].mime);
   TEST_ASSERT_NULL(strstr(m.body, "inner text"));
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(m.body, "Hi"), m.body);
   TEST_ASSERT_FALSE(m.attachments_truncated);
   email_message_free(&m);
}

static void test_mixed_text_parts_join(void) {
   email_message_t m;
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: multipart/mixed; boundary=m\r\n\r\n"
                                      "--m\r\nContent-Type: text/plain\r\n\r\nMain text.\r\n"
                                      "--m\r\nContent-Type: text/plain\r\n\r\nList footer.\r\n"
                                      "--m--\r\n",
                                  false, &TOOL, &m));
   TEST_ASSERT_NOT_NULL(strstr(m.body, "Main text."));
   TEST_ASSERT_NOT_NULL(strstr(m.body, "List footer."));
   TEST_ASSERT_EQUAL_INT(0, m.attachment_count);
   email_message_free(&m);
}

static void test_cut_fetch(void) {
   email_message_t m;
   /* The fetch stopped in the HTML part: both forms are partial, and parts
    * after the cut may be missing. */
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: multipart/alternative; boundary=b\r\n\r\n"
                                      "--b\r\nContent-Type: text/plain\r\n\r\nshort\r\n"
                                      "--b\r\nContent-Type: text/html\r\n\r\n<p>long and cut",
                                  true, &PANEL, &m));
   TEST_ASSERT_EQUAL_STRING("short", m.body); /* the CRLF before a boundary is the boundary's */
   TEST_ASSERT_FALSE(m.text_truncated);
   TEST_ASSERT_TRUE(m.html_truncated);
   TEST_ASSERT_TRUE(m.attachments_truncated);
   email_message_free(&m);
   /* A missing closing boundary on a complete fetch is just sloppy mail. */
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: multipart/mixed; boundary=b\r\n\r\n"
                                      "--b\r\nContent-Type: text/plain\r\n\r\nno end\r\n",
                                  false, &TOOL, &m));
   TEST_ASSERT_NOT_NULL(strstr(m.body, "no end"));
   TEST_ASSERT_FALSE(m.text_truncated);
   email_message_free(&m);
}

static void test_text_cap(void) {
   email_message_t m;
   email_read_opts_t o = TOOL;
   o.max_text_chars = 10;
   /* the cut lands on a whole character */
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: text/plain; charset=utf-8\r\n\r\n"
                                      "abcdefgh\xC3\xA9\xC3\xA9\xC3\xA9\r\n",
                                  false, &o, &m));
   TEST_ASSERT_EQUAL_STRING("abcdefgh\xC3\xA9", m.body);
   TEST_ASSERT_TRUE(m.text_truncated);
   email_message_free(&m);
}

static void test_addresses(void) {
   email_message_t m;
   char hdr[8192] = "From: =?UTF-8?Q?Jos=C3=A9?= <jose@example.com>\r\n"
                    "Reply-To: Desk <desk@example.com>\r\n"
                    "Cc: Team: c1@example.com, c2@example.com;, \"Q, R\" <q@example.com>\r\n"
                    "Subject: =?UTF-8?B?4oCuYWJj?= test\r\nTo: ";
   for (int i = 0; i < 40; i++) {
      char one[64];
      snprintf(one, sizeof(one), "%su%d@example.com", i ? ", " : "", i);
      strcat(hdr, one);
   }
   strcat(hdr, "\r\nContent-Type: text/plain\r\n\r\nx\r\n");
   TEST_ASSERT_EQUAL_INT(0, parse(hdr, false, &TOOL, &m));
   TEST_ASSERT_EQUAL_STRING("Jos\xC3\xA9", m.from_name);
   TEST_ASSERT_EQUAL_STRING("desk@example.com", m.reply_to.addr);
   TEST_ASSERT_EQUAL_STRING("Desk", m.reply_to.name);
   TEST_ASSERT_EQUAL_INT(EMAIL_MAX_ADDRS, m.to_count);
   TEST_ASSERT_EQUAL_INT(40, m.to_total);
   TEST_ASSERT_EQUAL_STRING("u31@example.com", m.to_list[31].addr);
   /* a group's members count; a quoted comma is part of the name */
   TEST_ASSERT_EQUAL_INT(3, m.cc_total);
   TEST_ASSERT_EQUAL_STRING("Q, R", m.cc_list[2].name);
   /* the right-to-left override is gone from the subject */
   TEST_ASSERT_EQUAL_STRING("abc test", m.subject);
   email_message_free(&m);
   /* No Reply-To: empty */
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: text/plain\r\n\r\nx\r\n", false, &TOOL, &m));
   TEST_ASSERT_EQUAL_STRING("", m.reply_to.addr);
   TEST_ASSERT_EQUAL_INT(0, m.cc_total);
   TEST_ASSERT_NULL(m.cc_list);
   email_message_free(&m);
}

static void test_headers_only(void) {
   email_message_t m;
   email_read_opts_t o = TOOL;
   o.headers_only = true;
   TEST_ASSERT_EQUAL_INT(0, parse(ALT, false, &o, &m));
   TEST_ASSERT_EQUAL_STRING("Hi", m.subject);
   TEST_ASSERT_EQUAL_STRING("ann@example.com", m.from_addr);
   TEST_ASSERT_NULL(m.body);
   email_message_free(&m);
}

/* A node of a hand-built part list. */
static void node(email_mime_part_t *p, const char *id, const char *type, bool multipart) {
   memset(p, 0, sizeof(*p));
   strcpy(p->part_id, id);
   strcpy(p->type, type);
   p->multipart = multipart;
}

/* Parts as a backend hands them over (already transfer-decoded, as Gmail's
 * are): mixed[ alternative[plain, html], pdf, latin-1 plain ]. */
static void test_part_list(void) {
   const char plain[] = "Hello, world \xE2\x80\x94 dash";
   const char html[] = "<p>Hello, <b>world</b></p>";
   const char latin[] = "Caf\xE9";
   email_mime_part_t parts[6];
   node(&parts[0], "", "multipart/mixed", true);
   node(&parts[1], "1", "multipart/alternative", true);
   node(&parts[2], "1.1", "text/plain", false);
   strcpy(parts[2].charset, "utf-8");
   parts[2].data = plain;
   parts[2].data_len = strlen(plain);
   node(&parts[3], "1.2", "text/html", false);
   parts[3].data = html;
   parts[3].data_len = strlen(html);
   node(&parts[4], "2", "application/pdf", false);
   strcpy(parts[4].filename, "a.pdf");
   parts[4].attachment = true;
   parts[4].size = 12345;
   parts[4].data_absent = true; /* Gmail: behind an attachmentId */
   node(&parts[5], "3", "text/plain", false);
   strcpy(parts[5].charset, "iso-8859-1");
   parts[5].data = latin;
   parts[5].data_len = strlen(latin);

   email_message_t m;
   memset(&m, 0, sizeof(m));
   TEST_ASSERT_EQUAL_INT(0, email_mime_apply(parts, 6, false, &PANEL, NULL, NULL, &m));
   TEST_ASSERT_EQUAL_STRING("Hello, world \xE2\x80\x94 dash\n\nCaf\xC3\xA9", m.body);
   TEST_ASSERT_EQUAL_STRING(html, m.body_html);
   TEST_ASSERT_EQUAL_INT(1, m.attachment_count);
   TEST_ASSERT_EQUAL_STRING("2", m.attachments[0].part_id);
   TEST_ASSERT_EQUAL_UINT(12345, m.attachments[0].size);
   email_message_free(&m);
}

/* A backend's fetch callback (a Gmail attachmentId): called only for parts
 * being read, in reading order. */
typedef struct {
   int calls[8];
   int ncalls;
   const char *bytes;
} fetch_probe_t;

static bool probe_fetch(void *ctx, int index, const char **data, size_t *len, bool *cut) {
   fetch_probe_t *f = ctx;
   if (f->ncalls < 8)
      f->calls[f->ncalls++] = index;
   *data = f->bytes;
   *len = strlen(f->bytes);
   *cut = false;
   return true;
}

static void test_fetch_on_read(void) {
   const char html[] = "<p>From the server</p>";
   email_mime_part_t parts[4];
   node(&parts[0], "", "multipart/alternative", true);
   node(&parts[1], "1", "text/plain", false);
   parts[1].data_absent = true; /* both forms held elsewhere */
   node(&parts[2], "2", "text/html", false);
   parts[2].data_absent = true;
   node(&parts[3], "3", "text/plain", false); /* never read: another branch */
   parts[3].data_absent = true;
   fetch_probe_t f = { .bytes = "   " }; /* the plain form is blank */
   email_message_t m;
   memset(&m, 0, sizeof(m));
   TEST_ASSERT_EQUAL_INT(0, email_mime_apply(parts, 4, false, &TOOL, probe_fetch, &f, &m));
   /* the plain branch, then the HTML one it fell back to; nothing else */
   TEST_ASSERT_EQUAL_INT(2, f.ncalls);
   TEST_ASSERT_EQUAL_INT(1, f.calls[0]);
   TEST_ASSERT_EQUAL_INT(2, f.calls[1]);
   email_message_free(&m);
   (void)html;
}

static long now_ms(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Generous for an ASan build; the uncut parse of these takes seconds. */
#define BOMB_MS 2000

/* A message built to make the parser work: the limits cut it before GMime
 * sees most of it, and it reads as truncated. */
static void test_bombs(void) {
   /* 200,000 tiny parts */
   const size_t n = 200000;
   const char *part = "--z\r\nContent-Type: text/plain\r\n\r\nx\r\n";
   const size_t plen = strlen(part);
   const char *head = HDR "Content-Type: multipart/mixed; boundary=z\r\n\r\n";
   const size_t hlen = strlen(head);
   char *raw = malloc(hlen + n * plen + 1);
   memcpy(raw, head, hlen);
   for (size_t i = 0; i < n; i++)
      memcpy(raw + hlen + i * plen, part, plen);
   const size_t len = hlen + n * plen;
   raw[len] = '\0';
   bool cut = false;
   TEST_ASSERT_LESS_THAN_size_t(len, email_mime_prescan(raw, len, &cut));
   TEST_ASSERT_TRUE(cut);
   email_message_t m;
   memset(&m, 0, sizeof(m));
   long t = now_ms();
   TEST_ASSERT_EQUAL_INT(0, email_mime_parse_raw(raw, len, false, &TOOL, &m));
   TEST_ASSERT_LESS_THAN_INT(BOMB_MS, now_ms() - t);
   TEST_ASSERT_TRUE(m.attachments_truncated);
   email_message_free(&m);
   free(raw);

   /* 1,000,000 header lines */
   const char *h = "X-A: a\r\n";
   const size_t hl = strlen(h);
   const size_t hn = 1000000;
   raw = malloc(hn * hl + 64);
   for (size_t i = 0; i < hn; i++)
      memcpy(raw + i * hl, h, hl);
   strcpy(raw + hn * hl, "\r\nbody\r\n");
   cut = false;
   TEST_ASSERT_LESS_THAN_size_t(hn * hl, email_mime_prescan(raw, strlen(raw), &cut));
   TEST_ASSERT_TRUE(cut);
   memset(&m, 0, sizeof(m));
   t = now_ms();
   TEST_ASSERT_EQUAL_INT(0, email_mime_parse_raw(raw, strlen(raw), false, &TOOL, &m));
   TEST_ASSERT_LESS_THAN_INT(BOMB_MS, now_ms() - t);
   email_message_free(&m);
   free(raw);

   /* Deep nesting: our walk stops at its depth limit */
   char *deep = malloc(256 * 1024);
   size_t o = (size_t)sprintf(deep, HDR "Content-Type: multipart/mixed; boundary=d0\r\n\r\n");
   for (int d = 0; d < 500; d++)
      o += (size_t)sprintf(deep + o, "--d%d\r\nContent-Type: multipart/mixed; boundary=d%d\r\n\r\n",
                           d, d + 1);
   o += (size_t)sprintf(deep + o, "--d500\r\nContent-Type: text/plain\r\n\r\ndeep\r\n");
   memset(&m, 0, sizeof(m));
   t = now_ms();
   TEST_ASSERT_EQUAL_INT(0, email_mime_parse_raw(deep, o, false, &TOOL, &m));
   TEST_ASSERT_LESS_THAN_INT(BOMB_MS, now_ms() - t);
   TEST_ASSERT_TRUE(m.attachments_truncated);
   email_message_free(&m);
   free(deep);
}

static void test_display_sanitize(void) {
   char out[32];
   /* controls, bidi, zero width; line breaks become one space */
   const char in[] = "  a\x01"
                     "b\r\n\tc\xE2\x80\xAE"
                     "d\xE2\x80\x8B"
                     "e\xE2\x81\xA6"
                     "f  ";
   email_display_sanitize(in, strlen(in), out, sizeof(out), 0);
   TEST_ASSERT_EQUAL_STRING("ab cdef", out);
   /* C1 control (U+0085) and an ill-formed byte */
   email_display_sanitize("x\xC2\x85y\xFFz", 6, out, sizeof(out), 0);
   TEST_ASSERT_EQUAL_STRING("xy?z", out);
   /* filenames lose their path separators */
   email_display_sanitize("../a\\b", 6, out, sizeof(out), EMAIL_DISPLAY_FILENAME);
   TEST_ASSERT_EQUAL_STRING(".._a_b", out);
   /* a cut lands between characters */
   char small[6];
   email_display_sanitize("abcd\xC3\xA9", 6, small, sizeof(small), 0);
   TEST_ASSERT_EQUAL_STRING("abcd", small);
   /* src_len bounds the read */
   email_display_sanitize("abcdef", 3, out, sizeof(out), 0);
   TEST_ASSERT_EQUAL_STRING("abc", out);
   email_display_sanitize(NULL, 5, out, sizeof(out), 0);
   TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_content_id(void) {
   char out[64];
   TEST_ASSERT_TRUE(email_display_content_id("<logo.1@example.com>", 20, out, sizeof(out)));
   TEST_ASSERT_EQUAL_STRING("logo.1@example.com", out);
   TEST_ASSERT_TRUE(email_display_content_id(" part1 ", 7, out, sizeof(out)));
   TEST_ASSERT_EQUAL_STRING("part1", out);
   /* anything a selector or URL could be built from is refused */
   TEST_ASSERT_FALSE(email_display_content_id("<a\"b>", 5, out, sizeof(out)));
   TEST_ASSERT_EQUAL_STRING("", out);
   TEST_ASSERT_FALSE(email_display_content_id("a b", 3, out, sizeof(out)));
   TEST_ASSERT_FALSE(email_display_content_id("<a<b>", 5, out, sizeof(out)));
   TEST_ASSERT_FALSE(email_display_content_id("", 0, out, sizeof(out)));
}

static void test_addr_helpers(void) {
   email_addr_t *list = NULL;
   int count = 0;
   int total = 0;
   TEST_ASSERT_EQUAL_INT(0, email_mime_addr_list("=?ISO-8859-1?Q?Ren=E9?= <rene@example.com>, "
                                                 "plain@example.com",
                                                 EMAIL_MAX_ADDRS, &list, &count, &total));
   TEST_ASSERT_EQUAL_INT(2, count);
   TEST_ASSERT_EQUAL_STRING("Ren\xC3\xA9", list[0].name);
   TEST_ASSERT_EQUAL_STRING("plain@example.com", list[1].addr);
   free(list);
   TEST_ASSERT_EQUAL_INT(0, email_mime_addr_list("", EMAIL_MAX_ADDRS, &list, &count, &total));
   TEST_ASSERT_NULL(list);
   TEST_ASSERT_EQUAL_INT(0, total);
   email_addr_t a;
   TEST_ASSERT_TRUE(email_mime_addr_first("Desk <desk@example.com>", &a));
   TEST_ASSERT_EQUAL_STRING("desk@example.com", a.addr);
   TEST_ASSERT_FALSE(email_mime_addr_first("", &a));
   char text[64];
   email_mime_header_text("=?UTF-8?B?SGVsbG8=?= world", text, sizeof(text));
   TEST_ASSERT_EQUAL_STRING("Hello world", text);
}

/* =============================================================================
 * Regression cases from the code review
 * ============================================================================= */

/* Charsets named the way mail names them convert (GMime's canonical spelling
 * differs: "windows-1251" is "windows-cp1251" there). */
static void test_charset_spellings(void) {
   struct {
      const char *charset;
      const char *bytes;
      const char *utf8;
   } cases[] = {
      { "windows-1251", "\xCF\xF0\xE8\xE2\xE5\xF2",
        "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82" },
      { "windows-1250", "\xA3\xF3\x64\xBC", "\xC5\x81\xC3\xB3\x64\xC4\xBD" },
      { "Shift_JIS", "\x93\xFA\x96\x7B\x8C\xEA", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" },
      { "latin1", "caf\xE9", "caf\xC3\xA9" },
      { "ISO-8859-7", "\xE1\xE2\xE3", "\xCE\xB1\xCE\xB2\xCE\xB3" },
   };
   for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
      char raw[512];
      snprintf(raw, sizeof(raw), HDR "Content-Type: text/plain; charset=%s\r\n\r\n%s",
               cases[k].charset, cases[k].bytes);
      email_message_t m;
      TEST_ASSERT_EQUAL_INT(0, parse(raw, false, &TOOL, &m));
      TEST_ASSERT_EQUAL_STRING_MESSAGE(cases[k].utf8, m.body, cases[k].charset);
      email_message_free(&m);
   }
}

/* A cap or a cut fetch that lands inside a character doesn't turn the rest
 * of a valid UTF-8 body into something else. */
static void test_utf8_cut(void) {
   email_message_t m;
   email_read_opts_t o = TOOL;
   o.max_text_chars = 20;
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: text/plain; charset=utf-8\r\n\r\n"
                                      "abcdefghi\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
                                      "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9",
                                  false, &o, &m));
   TEST_ASSERT_EQUAL_STRING("abcdefghi\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", m.body);
   TEST_ASSERT_TRUE(m.text_truncated);
   email_message_free(&m);
   /* undeclared CJK */
   o.max_text_chars = 12;
   TEST_ASSERT_EQUAL_INT(0,
                         parse(HDR "Content-Type: text/plain\r\n\r\n"
                                   "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE6\x97\xA5\xE6\x9C\xAC",
                               false, &o, &m));
   TEST_ASSERT_EQUAL_STRING("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE6\x97\xA5", m.body);
   email_message_free(&m);
   /* the fetch stopped inside a character */
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: text/plain\r\n\r\nhello \xE6\x97\xA5\xE6\x9C",
                                  true, &TOOL, &m));
   TEST_ASSERT_EQUAL_STRING("hello \xE6\x97\xA5", m.body);
   TEST_ASSERT_TRUE(m.text_truncated);
   email_message_free(&m);
   /* declared UTF-8 with one bad byte stays UTF-8 */
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: text/plain; charset=utf-8\r\n\r\n"
                                      "caf\xC3\xA9 \xFF ok",
                                  false, &TOOL, &m));
   TEST_ASSERT_EQUAL_STRING("caf\xC3\xA9 ? ok", m.body);
   email_message_free(&m);
}

/* The body follows the message's structure. */
static void test_structure(void) {
   email_message_t m;
   /* A mailing list wraps an HTML-only post with a plain footer: the post is
    * the body, not lost behind the footer. */
   TEST_ASSERT_EQUAL_INT(0,
                         parse(HDR "Content-Type: multipart/mixed; boundary=m\r\n\r\n"
                                   "--m\r\nContent-Type: text/html\r\n\r\n<p>The real post.</p>\r\n"
                                   "--m\r\nContent-Type: text/plain\r\n\r\n___ list footer\r\n"
                                   "--m--\r\n",
                               false, &PANEL, &m));
   TEST_ASSERT_NOT_NULL(strstr(m.body, "The real post."));
   TEST_ASSERT_NOT_NULL(strstr(m.body, "list footer"));
   TEST_ASSERT_TRUE(strstr(m.body, "The real post.") < strstr(m.body, "list footer"));
   TEST_ASSERT_EQUAL_INT(0, m.attachment_count);
   TEST_ASSERT_NOT_NULL(m.body_html);
   email_message_free(&m);
   /* A forward with a cover note: the note, then the forwarded text */
   TEST_ASSERT_EQUAL_INT(0,
                         parse(HDR "Content-Type: multipart/mixed; boundary=m\r\n\r\n"
                                   "--m\r\nContent-Type: text/plain\r\n\r\nNote\r\n"
                                   "--m\r\nContent-Type: multipart/alternative; boundary=a\r\n\r\n"
                                   "--a\r\nContent-Type: text/plain\r\n\r\nForwarded text\r\n"
                                   "--a\r\nContent-Type: text/html\r\n\r\n<p>Forwarded</p>\r\n"
                                   "--a--\r\n--m--\r\n",
                               false, &TOOL, &m));
   TEST_ASSERT_EQUAL_STRING("Note\n\nForwarded text", m.body);
   TEST_ASSERT_EQUAL_INT(0, m.attachment_count);
   email_message_free(&m);
   /* Apple Mail inline images: alternative[plain, mixed[html, img, html]].
    * The text is the plain branch; the HTML fragments are its other form. */
   TEST_ASSERT_EQUAL_INT(0,
                         parse(HDR "Content-Type: multipart/alternative; boundary=a\r\n\r\n"
                                   "--a\r\nContent-Type: text/plain\r\n\r\nPlain version\r\n"
                                   "--a\r\nContent-Type: multipart/mixed; boundary=m\r\n\r\n"
                                   "--m\r\nContent-Type: text/html\r\n\r\n<p>One</p>\r\n"
                                   "--m\r\nContent-Type: image/png\r\nContent-Disposition: inline;"
                                   " filename=a.png\r\nContent-Transfer-Encoding: base64\r\n\r\n"
                                   "iVBORw0KGgo=\r\n"
                                   "--m\r\nContent-Type: text/html\r\n\r\n<p>Two</p>\r\n"
                                   "--m--\r\n--a--\r\n",
                               false, &TOOL, &m));
   TEST_ASSERT_EQUAL_STRING("Plain version", m.body);
   TEST_ASSERT_EQUAL_INT(1, m.attachment_count);
   TEST_ASSERT_EQUAL_STRING("image/png", m.attachments[0].mime);
   TEST_ASSERT_TRUE(m.attachments[0].is_inline);
   email_message_free(&m);
   /* An attached text file sent inline (Apple Mail) is still a file */
   TEST_ASSERT_EQUAL_INT(0,
                         parse(HDR "Content-Type: multipart/mixed; boundary=m\r\n\r\n"
                                   "--m\r\nContent-Type: text/plain\r\n\r\nHello\r\n"
                                   "--m\r\nContent-Type: text/plain\r\nContent-Disposition: inline;"
                                   " filename=log.txt\r\n\r\nLOG LINE\r\n--m--\r\n",
                               false, &TOOL, &m));
   TEST_ASSERT_EQUAL_STRING("Hello", m.body);
   TEST_ASSERT_EQUAL_INT(1, m.attachment_count);
   TEST_ASSERT_EQUAL_STRING("log.txt", m.attachments[0].filename);
   TEST_ASSERT_TRUE(m.attachments[0].is_inline);
   email_message_free(&m);
   /* A message cut before any of its text: truncated, not "no body" */
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: multipart/mixed; boundary=m\r\n\r\n"
                                      "--m\r\nContent-Type: image/png\r\n"
                                      "Content-Transfer-Encoding: base64\r\n\r\niVBORw0K",
                                  true, &TOOL, &m));
   TEST_ASSERT_EQUAL_STRING("", m.body);
   TEST_ASSERT_TRUE(m.text_truncated);
   email_message_free(&m);
}

/* A base64 part whose first stretch decodes to nothing still decodes. */
static void test_base64_leading_whitespace(void) {
   char *raw = malloc(16 * 1024);
   int o = sprintf(raw,
                   HDR "Content-Type: text/plain\r\nContent-Transfer-Encoding: base64\r\n\r\n");
   memset(raw + o, ' ', 9000);
   o += 9000;
   strcpy(raw + o, "aGVsbG8gd29ybGQ=\r\n");
   email_message_t m;
   TEST_ASSERT_EQUAL_INT(0, parse(raw, false, &TOOL, &m));
   TEST_ASSERT_EQUAL_STRING("hello world", m.body);
   TEST_ASSERT_FALSE(m.text_truncated);
   email_message_free(&m);
   free(raw);
}

/* Text a person can't see but a model reads is removed; a line separator in
 * a one-line field can't forge another line. */
static void test_invisible_text(void) {
   email_message_t m;
   TEST_ASSERT_EQUAL_INT(0, parse("From: a@example.com\r\nSubject: =?UTF-8?B?QQ==?="
                                  "\xE2\x80\xA8"
                                  "Reply-To: x\r\n"
                                  "Content-Type: text/plain; charset=utf-8\r\n\r\n"
                                  "visible\xF3\xA0\x81\x89\xF3\xA0\x81\x87\xF3\xA0\x81\x8E"
                                  "\xE2\x80\xA8"
                                  "next\r\n",
                                  false, &TOOL, &m));
   TEST_ASSERT_EQUAL_STRING("visible\nnext\r\n", m.body);
   TEST_ASSERT_NULL(strstr(m.subject, "\xE2\x80\xA8"));
   email_message_free(&m);
   char out[64];
   email_display_sanitize("a\xE2\x80\xA9"
                          "b\xF3\xA0\x80\x81"
                          "c",
                          11, out, sizeof(out), 0);
   TEST_ASSERT_EQUAL_STRING("a bc", out);
}

/* Part ids that wouldn't fit stop the walk instead of naming another part. */
static void test_part_id_fits(void) {
   char id[32];
   TEST_ASSERT_TRUE(email_mime_child_id("", 3, id, sizeof(id)));
   TEST_ASSERT_EQUAL_STRING("3", id);
   TEST_ASSERT_TRUE(email_mime_child_id("2.1", 10, id, sizeof(id)));
   TEST_ASSERT_EQUAL_STRING("2.1.10", id);
   TEST_ASSERT_FALSE(email_mime_child_id("10.10.10.10.10.10.10.10.10.10", 10, id, sizeof(id)));
}

/* An address header too long to be worth parsing is cut at an address
 * boundary, and the total still counts what was cut off. */
static void test_long_address_header(void) {
   const int n = 9000;
   char *v = malloc((size_t)n * 20 + 1);
   size_t o = 0;
   for (int i = 0; i < n; i++)
      o += (size_t)sprintf(v + o, "%su%d@example.com", i ? ", " : "", i);
   email_addr_t *list = NULL;
   int count = 0;
   int total = 0;
   TEST_ASSERT_EQUAL_INT(0, email_mime_addr_list(v, EMAIL_MAX_ADDRS, &list, &count, &total));
   TEST_ASSERT_EQUAL_INT(EMAIL_MAX_ADDRS, count);
   TEST_ASSERT_EQUAL_INT(n, total);
   free(list);
   free(v);
}

static long peak_rss_kb(void) {
   struct rusage ru;
   getrusage(RUSAGE_SELF, &ru);
   return ru.ru_maxrss;
}

/* Headers GMime parses that a narrower count missed: 8-bit names, a space
 * before the colon, one header folded over many lines.  Each is cut before
 * GMime builds it, so a read stays small and fast. */
static void test_header_bombs(void) {
   const char *units[] = { "\xff:\r\n", "a :\r\n", " b,\r\n" };
   for (size_t k = 0; k < 3; k++) {
      const size_t un = strlen(units[k]);
      const size_t reps = (2 * 1024 * 1024) / un;
      const char *head = k == 2 ? "From: a@example.com\r\nTo: x,\r\n" : "";
      const size_t hl = strlen(head);
      char *raw = malloc(hl + reps * un + 32);
      memcpy(raw, head, hl);
      for (size_t i = 0; i < reps; i++)
         memcpy(raw + hl + i * un, units[k], un);
      strcpy(raw + hl + reps * un, "\r\nbody\r\n");
      const size_t len = strlen(raw);
      bool cut = false;
      TEST_ASSERT_LESS_THAN_size_t(len, email_mime_prescan(raw, len, &cut));
      TEST_ASSERT_TRUE(cut);
      const long rss = peak_rss_kb();
      email_message_t m;
      memset(&m, 0, sizeof(m));
      const long t = now_ms();
      TEST_ASSERT_EQUAL_INT(0, email_mime_parse_raw(raw, len, false, &PANEL, &m));
      TEST_ASSERT_LESS_THAN_INT(BOMB_MS, now_ms() - t);
      /* Peak RSS only grows: a bomb that got through would add hundreds of MB. */
      TEST_ASSERT_LESS_THAN_INT(64 * 1024, peak_rss_kb() - rss);
      email_message_free(&m);
      free(raw);
   }
}

/* =============================================================================
 * The Gmail walk: format=full JSON gives the same part ids and body as the
 * raw message
 * ============================================================================= */

static void b64url(const char *in, char *out) {
   static const char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
   const size_t n = strlen(in);
   size_t o = 0;
   for (size_t i = 0; i < n; i += 3) {
      const unsigned a = (unsigned char)in[i];
      const unsigned b = i + 1 < n ? (unsigned char)in[i + 1] : 0;
      const unsigned c = i + 2 < n ? (unsigned char)in[i + 2] : 0;
      const unsigned t = (a << 16) | (b << 8) | c;
      out[o++] = k[(t >> 18) & 63];
      out[o++] = k[(t >> 12) & 63];
      if (i + 1 < n)
         out[o++] = k[(t >> 6) & 63];
      if (i + 2 < n)
         out[o++] = k[t & 63];
   }
   out[o] = '\0';
}

/* Reads a Gmail payload (with %s for base64url text parts) through the walk
 * and the policy. */
static void gmail_read(const char *payload_json,
                       const email_read_opts_t *opts,
                       email_message_t *m) {
   struct json_object *payload = json_tokener_parse(payload_json);
   TEST_ASSERT_NOT_NULL_MESSAGE(payload, payload_json);
   gmail_parts_t w;
   TEST_ASSERT_EQUAL_INT(0, gmail_parts_from_payload(payload, &w));
   memset(m, 0, sizeof(*m));
   TEST_ASSERT_EQUAL_INT(0, email_mime_apply(w.parts, w.count, w.cut, opts, NULL, NULL, m));
   gmail_parts_free(&w);
   json_object_put(payload);
}

/* A listing and a read take From/Subject/Date the same way: Date sanitized
 * (a sender writes it), a From with no address shown as its text. */
static void test_gmail_header_fields(void) {
   struct json_object *h = json_tokener_parse(
       "[{\"name\":\"From\",\"value\":\"Some Name\"},"
       "{\"name\":\"Subject\",\"value\":\"=?UTF-8?B?SGVsbG8=?=\"},"
       "{\"name\":\"Date\",\"value\":\"Mon, 5 Oct 2026 \u202eevil\"}]");
   TEST_ASSERT_NOT_NULL(h);
   char name[64] = "", addr[64] = "", subject[64] = "", date[32] = "";
   gmail_header_fields(h, name, sizeof(name), addr, sizeof(addr), subject, sizeof(subject), date,
                       sizeof(date));
   TEST_ASSERT_EQUAL_STRING("Some Name", name);
   TEST_ASSERT_EQUAL_STRING("", addr);
   TEST_ASSERT_EQUAL_STRING("Hello", subject);
   TEST_ASSERT_NULL(strstr(date, "\xe2\x80\xae")); /* direction override dropped */
   TEST_ASSERT_NOT_NULL(strstr(date, "Mon, 5 Oct 2026"));
   json_object_put(h);
}

static void test_gmail_walk(void) {
   char plain[64];
   char html[64];
   char foot[64];
   b64url("Hello, world", plain);
   b64url("<p>Hello, <b>world</b></p>", html);
   b64url("___ footer", foot);
   char json[4096];
   email_message_t m;

   /* single part */
   snprintf(json, sizeof(json),
            "{\"partId\":\"\",\"mimeType\":\"text/plain\",\"filename\":\"\",\"headers\":"
            "[{\"name\":\"Content-Type\",\"value\":\"text/plain; charset=utf-8\"}],"
            "\"body\":{\"size\":12,\"data\":\"%s\"}}",
            plain);
   gmail_read(json, &TOOL, &m);
   TEST_ASSERT_EQUAL_STRING("Hello, world", m.body);
   email_message_free(&m);

   /* mixed[ related[ alternative[plain, html], cid image ], pdf, message/rfc822 ],
    * the shape and numbering the raw form gives */
   snprintf(
       json, sizeof(json),
       "{\"partId\":\"\",\"mimeType\":\"multipart/mixed\",\"body\":{\"size\":0},\"parts\":["
       " {\"partId\":\"0\",\"mimeType\":\"multipart/related\",\"body\":{\"size\":0},\"parts\":["
       "  {\"partId\":\"0.0\",\"mimeType\":\"multipart/alternative\",\"body\":{\"size\":0},"
       "   \"parts\":["
       "   {\"partId\":\"0.0.0\",\"mimeType\":\"text/plain\",\"filename\":\"\","
       "    \"body\":{\"size\":12,\"data\":\"%s\"}},"
       "   {\"partId\":\"0.0.1\",\"mimeType\":\"text/html\",\"filename\":\"\","
       "    \"body\":{\"size\":26,\"data\":\"%s\"}}]},"
       "  {\"partId\":\"0.1\",\"mimeType\":\"image/png\",\"filename\":\"logo.png\","
       "   \"headers\":[{\"name\":\"Content-ID\",\"value\":\"<logo@x>\"},"
       "   {\"name\":\"Content-Disposition\",\"value\":\"inline; filename=logo.png\"}],"
       "   \"body\":{\"size\":300,\"attachmentId\":\"ANGjdJ_logo\"}}]},"
       " {\"partId\":\"1\",\"mimeType\":\"application/pdf\",\"filename\":\"a.pdf\","
       "  \"headers\":[{\"name\":\"Content-Disposition\",\"value\":\"attachment; "
       "filename=a.pdf\"}],"
       "  \"body\":{\"size\":12345,\"attachmentId\":\"ANGjdJ_pdf\"}},"
       " {\"partId\":\"2\",\"mimeType\":\"message/rfc822\",\"filename\":\"\","
       "  \"body\":{\"size\":-5},\"parts\":[{\"partId\":\"2.0\",\"mimeType\":\"text/plain\","
       "   \"body\":{\"size\":1,\"data\":\"eA\"}}]},"
       " {\"partId\":\"3\",\"mimeType\":\"text/plain\",\"filename\":\"\","
       "  \"body\":{\"size\":10,\"data\":\"%s\"}}]}",
       plain, html, foot);
   gmail_read(json, &PANEL, &m);
   TEST_ASSERT_EQUAL_STRING("Hello, world\n\n___ footer", m.body);
   TEST_ASSERT_EQUAL_STRING("<p>Hello, <b>world</b></p>", m.body_html);
   TEST_ASSERT_EQUAL_INT(3, m.attachment_count);
   TEST_ASSERT_EQUAL_STRING("1.2", m.attachments[0].part_id);
   TEST_ASSERT_EQUAL_STRING("logo@x", m.attachments[0].content_id);
   TEST_ASSERT_TRUE(m.attachments[0].is_inline);
   TEST_ASSERT_EQUAL_STRING("2", m.attachments[1].part_id);
   TEST_ASSERT_EQUAL_STRING("a.pdf", m.attachments[1].filename);
   TEST_ASSERT_EQUAL_UINT(12345, m.attachments[1].size);
   TEST_ASSERT_EQUAL_STRING("3", m.attachments[2].part_id);
   TEST_ASSERT_EQUAL_STRING("message/rfc822", m.attachments[2].mime);
   TEST_ASSERT_EQUAL_UINT(0, m.attachments[2].size); /* a negative size is no size */
   email_message_free(&m);

   /* The same message raw: the same ids */
   const char *raw = HDR
       "Content-Type: multipart/mixed; boundary=m\r\n\r\n"
       "--m\r\nContent-Type: multipart/related; boundary=r\r\n\r\n"
       "--r\r\nContent-Type: multipart/alternative; boundary=a\r\n\r\n"
       "--a\r\nContent-Type: text/plain\r\n\r\nHello, world\r\n"
       "--a\r\nContent-Type: text/html\r\n\r\n<p>Hello, <b>world</b></p>\r\n--a--\r\n"
       "--r\r\nContent-Type: image/png\r\nContent-ID: <logo@x>\r\n"
       "Content-Disposition: inline; filename=logo.png\r\n"
       "Content-Transfer-Encoding: base64\r\n\r\niVBORw0KGgo=\r\n--r--\r\n"
       "--m\r\nContent-Type: application/pdf\r\nContent-Disposition: attachment; filename=a.pdf\r\n"
       "Content-Transfer-Encoding: base64\r\n\r\nJVBERi0xLjQK\r\n"
       "--m\r\nContent-Type: message/rfc822\r\n\r\nSubject: inner\r\n\r\nx\r\n"
       "--m\r\nContent-Type: text/plain\r\n\r\n___ footer\r\n--m--\r\n";
   TEST_ASSERT_EQUAL_INT(0, parse(raw, false, &PANEL, &m));
   TEST_ASSERT_EQUAL_STRING("Hello, world\n\n___ footer", m.body);
   TEST_ASSERT_EQUAL_INT(3, m.attachment_count);
   TEST_ASSERT_EQUAL_STRING("1.2", m.attachments[0].part_id);
   TEST_ASSERT_EQUAL_STRING("2", m.attachments[1].part_id);
   TEST_ASSERT_EQUAL_STRING("3", m.attachments[2].part_id);
   email_message_free(&m);

   /* A large body part behind an attachmentId: its id kept for the fetch,
    * read as cut when nothing fetches it */
   snprintf(json, sizeof(json),
            "{\"partId\":\"\",\"mimeType\":\"multipart/alternative\",\"body\":{\"size\":0},"
            "\"parts\":["
            "{\"partId\":\"0\",\"mimeType\":\"text/plain\",\"body\":{\"size\":900000,"
            "\"attachmentId\":\"ANGjdJ_big\"}},"
            "{\"partId\":\"1\",\"mimeType\":\"text/html\",\"body\":{\"size\":5,\"data\":\"%s\"}}]}",
            html);
   struct json_object *payload = json_tokener_parse(json);
   gmail_parts_t w;
   TEST_ASSERT_EQUAL_INT(0, gmail_parts_from_payload(payload, &w));
   TEST_ASSERT_TRUE(w.parts[1].data_absent);
   TEST_ASSERT_EQUAL_STRING("ANGjdJ_big", w.attachment_ids[1]);
   memset(&m, 0, sizeof(m));
   TEST_ASSERT_EQUAL_INT(0, email_mime_apply(w.parts, w.count, w.cut, &TOOL, NULL, NULL, &m));
   TEST_ASSERT_TRUE(m.text_truncated);
   email_message_free(&m);
   gmail_parts_free(&w);
   json_object_put(payload);
}

/* base64url decoding keeps its promise of a NUL after the bytes */
static void test_base64url(void) {
   size_t len = 0;
   unsigned char *b = gmail_base64url_decode("aGVsbG8", 0, &len);
   TEST_ASSERT_EQUAL_UINT(5, len);
   TEST_ASSERT_EQUAL_STRING("hello", (char *)b);
   free(b);
   b = gmail_base64url_decode("aGVsbG8gd29ybGQ", 4, &len);
   TEST_ASSERT_EQUAL_UINT(4, len);
   TEST_ASSERT_EQUAL_STRING("hell", (char *)b);
   free(b);
   TEST_ASSERT_NULL(gmail_base64url_decode("", 0, &len));
}

/* =============================================================================
 * Second review round
 * ============================================================================= */

/* A plain reply quoting an address or code is plain text, all of it. */
static void test_plain_with_angle_brackets(void) {
   email_message_t m;
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: text/plain\r\n\r\n"
                                      "Sounds good.\r\n\r\nOn Tue, A Person <a.person@example.com> "
                                      "wrote:\r\n> Lunch?\r\n> x < y and if (a<b) ok\r\n",
                                  false, &TOOL, &m));
   TEST_ASSERT_NOT_NULL(strstr(m.body, "<a.person@example.com>"));
   TEST_ASSERT_NOT_NULL(strstr(m.body, "if (a<b) ok"));
   TEST_ASSERT_FALSE(m.text_truncated);
   email_message_free(&m);
}

/* Lines that start with "--" but aren't a declared boundary are body text: a
 * long Outlook or patch message isn't cut, and its attachment survives. */
static void test_dashes_in_bodies(void) {
   const char *units[] = { "-->\r\n<p>reply line</p>\r\n", "--- a/file.c\r\n context line\r\n",
                           "-- \r\nA Person\r\n" };
   for (size_t k = 0; k < 3; k++) {
      char *raw = malloc(400 * 1024);
      int o = sprintf(raw, HDR "Content-Type: multipart/mixed; boundary=\"b-1\"\r\n\r\n"
                               "--b-1\r\nContent-Type: text/plain\r\n\r\nstart\r\n");
      for (int i = 0; i < 6000; i++)
         o += sprintf(raw + o, "%s", units[k]);
      o += sprintf(raw + o,
                   "end\r\n--b-1\r\nContent-Type: application/pdf\r\n"
                   "Content-Disposition: attachment; filename=a.pdf\r\n\r\nPDF\r\n--b-1--\r\n");
      bool cut = true;
      TEST_ASSERT_EQUAL_size_t((size_t)o, email_mime_prescan(raw, (size_t)o, &cut));
      TEST_ASSERT_FALSE(cut);
      email_message_t m;
      email_read_opts_t big = PANEL;
      big.max_text_chars = 400 * 1024; /* the whole body, to see its end */
      TEST_ASSERT_EQUAL_INT(0, parse(raw, false, &big, &m));
      TEST_ASSERT_EQUAL_INT(1, m.attachment_count);
      TEST_ASSERT_FALSE(m.attachments_truncated);
      TEST_ASSERT_NOT_NULL(strstr(m.body, "end"));
      email_message_free(&m);
      free(raw);
   }
}

/* Headers GMime parses in places a part declares nothing: a digest's parts
 * are messages by default (RFC 2046), and a "--" line inside a forwarded
 * message's headers doesn't end them.  Both are counted. */
static void test_header_blocks_without_types(void) {
   for (int k = 0; k < 2; k++) {
      const size_t reps = 400000;
      char *raw = malloc(reps * 4 + 512);
      int o = k == 0 ? sprintf(raw, HDR "Content-Type: multipart/digest; boundary=d\r\n\r\n"
                                        "--d\r\n\r\nFrom: x@example.com\r\n")
                     : sprintf(raw, HDR "Content-Type: multipart/mixed; boundary=d\r\n\r\n"
                                        "--d\r\nContent-Type: message/rfc822\r\n--fake\r\n\r\n"
                                        "From: x@example.com\r\n");
      for (size_t i = 0; i < reps; i++) {
         memcpy(raw + o, "a:\r\n", 4);
         o += 4;
      }
      strcpy(raw + o, "\r\nbody\r\n--d--\r\n");
      const size_t len = strlen(raw);
      bool cut = false;
      TEST_ASSERT_LESS_THAN_size_t(len, email_mime_prescan(raw, len, &cut));
      TEST_ASSERT_TRUE(cut);
      const long rss = peak_rss_kb();
      email_message_t m;
      memset(&m, 0, sizeof(m));
      TEST_ASSERT_EQUAL_INT(0, email_mime_parse_raw(raw, len, false, &PANEL, &m));
      TEST_ASSERT_LESS_THAN_INT(64 * 1024, peak_rss_kb() - rss);
      email_message_free(&m);
      free(raw);
   }
}

/* Many legal-sized To headers: the address budget cuts them before GMime
 * builds every address. */
static void test_address_budget(void) {
   const int headers = 30;
   const int per = 3500; /* ~60 KB each, under the one-header cap */
   char *raw = malloc((size_t)headers * per * 20 + 1024);
   size_t o = (size_t)sprintf(raw, "From: a@example.com\r\n");
   for (int h = 0; h < headers; h++) {
      o += (size_t)sprintf(raw + o, "To: ");
      for (int i = 0; i < per; i++)
         o += (size_t)sprintf(raw + o, "%su%d@ex.com", i ? "," : "", i);
      o += (size_t)sprintf(raw + o, "\r\n");
   }
   o += (size_t)sprintf(raw + o, "\r\nbody\r\n");
   bool cut = false;
   TEST_ASSERT_LESS_THAN_size_t(o, email_mime_prescan(raw, o, &cut));
   TEST_ASSERT_TRUE(cut);
   email_message_t m;
   memset(&m, 0, sizeof(m));
   const long t = now_ms();
   email_read_opts_t ho = TOOL;
   ho.headers_only = true;
   TEST_ASSERT_EQUAL_INT(0, email_mime_parse_raw(raw, o, false, &ho, &m));
   TEST_ASSERT_LESS_THAN_INT(BOMB_MS, now_ms() - t);
   email_message_free(&m);
   free(raw);
}

static void test_charset_aliases(void) {
   const char *const names[] = { "cp1251", "CP1251", "windows-1251", "x-cp1251" };
   for (size_t k = 0; k < 3; k++) {
      char raw[256];
      snprintf(raw, sizeof(raw), HDR "Content-Type: text/plain; charset=%s\r\n\r\n\xCF\xF0",
               names[k]);
      email_message_t m;
      TEST_ASSERT_EQUAL_INT(0, parse(raw, false, &TOOL, &m));
      TEST_ASSERT_EQUAL_STRING_MESSAGE("\xD0\x9F\xD1\x80", m.body, names[k]);
      email_message_free(&m);
   }
   email_message_t m;
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: text/plain; charset=utf8\r\n\r\n\xC3\xA9",
                                  false, &TOOL, &m));
   TEST_ASSERT_EQUAL_STRING("\xC3\xA9", m.body);
   email_message_free(&m);
   /* A name that can't be a charset is read as windows-1252 */
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: text/plain; charset=\"a b<c>\"\r\n\r\n\xE9",
                                  false, &TOOL, &m));
   TEST_ASSERT_EQUAL_STRING("\xC3\xA9", m.body);
   email_message_free(&m);
}

static void test_body_fallbacks(void) {
   email_message_t m;
   /* An alternative whose plain branch is only markup: the HTML is the message */
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: multipart/alternative; boundary=a\r\n\r\n"
                                      "--a\r\nContent-Type: text/plain\r\n\r\n   \r\n"
                                      "--a\r\nContent-Type: text/html\r\n\r\n<p>real html</p>\r\n"
                                      "--a--\r\n",
                                  false, &TOOL, &m));
   TEST_ASSERT_EQUAL_STRING("real html", m.body);
   email_message_free(&m);
   /* A single text part with a name is still the message's text */
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: text/plain; name=note.txt\r\n\r\nthe text\r\n",
                                  false, &TOOL, &m));
   TEST_ASSERT_EQUAL_STRING("the text\r\n", m.body);
   email_message_free(&m);
   /* An undeclared windows-1252 body cut after a byte that looks like a lead */
   TEST_ASSERT_EQUAL_INT(0, parse(HDR "Content-Type: text/plain\r\n\r\ncaf\xE9", true, &TOOL, &m));
   TEST_ASSERT_EQUAL_STRING("caf\xC3\xA9", m.body);
   email_message_free(&m);
}

/* Headers and parameters in the forms GMime accepts: the scan reads them the
 * same way, so what GMime parses is what the scan counts. */
static void test_prescan_reads_like_gmime(void) {
   /* "Content-Type :" with the boundary folded, spaced and quoted with an
    * escape: only that boundary's lines open parts; "-->" stays body text. */
   const char *msg = HDR "Content-Type : multipart/mixed;\r\n boundary = \"b\\\"1\"\r\n\r\n"
                         "--b\"1\r\nContent-Type: text/plain\r\n\r\nstart\r\n-->\r\nmore\r\n"
                         "--b\"1--\r\n";
   bool cut = true;
   TEST_ASSERT_EQUAL_size_t(strlen(msg), email_mime_prescan(msg, strlen(msg), &cut));
   TEST_ASSERT_FALSE(cut);
   email_message_t m;
   TEST_ASSERT_EQUAL_INT(0, parse(msg, false, &TOOL, &m));
   TEST_ASSERT_NOT_NULL(strstr(m.body, "-->"));
   email_message_free(&m);

   /* A boundary declared in RFC 2231 sections matches by its start */
   const char *sect = HDR
       "Content-Type: multipart/mixed; boundary*0=\"abc\";"
       " boundary*1=\"def\"\r\n\r\n--abcdef\r\nContent-Type: text/plain\r\n\r\nx\r\n";
   char *raw = malloc(64 * 1024);
   strcpy(raw, sect);
   size_t o = strlen(raw);
   for (int k = 0; k < EMAIL_MIME_PRESCAN_BOUNDARIES + 5; k++)
      o += (size_t)sprintf(raw + o, "--abcdef\r\n\r\n");
   cut = false;
   TEST_ASSERT_LESS_THAN_size_t(o, email_mime_prescan(raw, o, &cut));
   TEST_ASSERT_TRUE(cut); /* every one of those lines is a boundary */

   /* An encoded boundary can't be matched as written: every "--" line counts */
   strcpy(raw, HDR "Content-Type: multipart/mixed; boundary*=utf-8''%61\r\n\r\n");
   o = strlen(raw);
   for (int k = 0; k < EMAIL_MIME_PRESCAN_BOUNDARIES + 5; k++)
      o += (size_t)sprintf(raw + o, "--anything\r\n\r\n");
   cut = false;
   TEST_ASSERT_LESS_THAN_size_t(o, email_mime_prescan(raw, o, &cut));
   TEST_ASSERT_TRUE(cut);
   free(raw);

   /* "To :" counts toward the address budget like "To:" */
   const int per = 3500;
   raw = malloc(30 * (size_t)per * 20 + 1024);
   o = (size_t)sprintf(raw, "From: a@example.com\r\n");
   for (int h = 0; h < 30; h++) {
      o += (size_t)sprintf(raw + o, "To : ");
      for (int k = 0; k < per; k++)
         o += (size_t)sprintf(raw + o, "%su%d@ex.com", k ? "," : "", k);
      o += (size_t)sprintf(raw + o, "\r\n");
   }
   o += (size_t)sprintf(raw + o, "\r\nbody\r\n");
   cut = false;
   TEST_ASSERT_LESS_THAN_size_t(o, email_mime_prescan(raw, o, &cut));
   TEST_ASSERT_TRUE(cut);
   free(raw);
}

/* Content-Type values GMime reads past spacing, comments and folds: the
 * scan takes GMime's own reading, so none of them hides a header flood. */
static void test_prescan_takes_gmime_type(void) {
   const char *const types[] = {
      "multipart/digest; boundary=B",        /* the plain form, as a control */
      "multipart /digest; boundary=B",       /* spaces around '/' */
      "multipart(c)/digest; boundary=B",     /* a comment */
      "multipart\r\n /digest; boundary=B",   /* folded before '/' */
      "multipart/digest; boundary=(c)\"B\"", /* a comment before the value */
      "multipart/digest; boundary= (c) B",
      "multipart/digest; boundary=\"B\r\n X\"", /* folded inside the quotes */
      "multipart/digest; boundary=\"B\r\n\tX\"",
   };
   for (size_t k = 0; k < sizeof(types) / sizeof(types[0]); k++) {
      const size_t reps = 300000;
      char *raw = malloc(reps * 4 + 512);
      int o = sprintf(raw,
                      "From: a@example.com\r\nContent-Type: %s\r\n\r\n--B\r\n\r\n"
                      "From: x@example.com\r\n",
                      types[k]);
      for (size_t r = 0; r < reps; r++) {
         memcpy(raw + o, "a:\r\n", 4);
         o += 4;
      }
      strcpy(raw + o, "\r\nbody\r\n--B--\r\n");
      bool cut = false;
      TEST_ASSERT_LESS_THAN_size_t_MESSAGE(strlen(raw), email_mime_prescan(raw, strlen(raw), &cut),
                                           types[k]);
      TEST_ASSERT_TRUE_MESSAGE(cut, types[k]);
      free(raw);
   }
   /* A forwarded message part spelled with spaces or a comment is a message too */
   const char *const msgs[] = { "message /rfc822", "message(x)/rfc822", "message\r\n /rfc822" };
   for (size_t k = 0; k < 3; k++) {
      const size_t reps = 300000;
      char *raw = malloc(reps * 4 + 512);
      int o = sprintf(raw,
                      HDR "Content-Type: multipart/mixed; boundary=M\r\n\r\n"
                          "--M\r\nContent-Type: %s\r\n\r\nFrom: x@example.com\r\n",
                      msgs[k]);
      for (size_t r = 0; r < reps; r++) {
         memcpy(raw + o, "a:\r\n", 4);
         o += 4;
      }
      strcpy(raw + o, "\r\nbody\r\n--M--\r\n");
      bool cut = false;
      TEST_ASSERT_LESS_THAN_size_t_MESSAGE(strlen(raw), email_mime_prescan(raw, strlen(raw), &cut),
                                           msgs[k]);
      TEST_ASSERT_TRUE_MESSAGE(cut, msgs[k]);
      free(raw);
   }
}

static void test_gmail_undo_labels(void) {
   TEST_ASSERT_TRUE(gmail_label_addable("INBOX"));
   TEST_ASSERT_TRUE(gmail_label_addable("UNREAD"));
   TEST_ASSERT_TRUE(gmail_label_addable("STARRED"));
   TEST_ASSERT_TRUE(gmail_label_addable("IMPORTANT"));
   TEST_ASSERT_TRUE(gmail_label_addable("CATEGORY_UPDATES"));
   TEST_ASSERT_TRUE(gmail_label_addable("Label_123"));
   TEST_ASSERT_FALSE(gmail_label_addable("TRASH"));
   TEST_ASSERT_FALSE(gmail_label_addable("SPAM"));
   TEST_ASSERT_FALSE(gmail_label_addable("SENT"));
   TEST_ASSERT_FALSE(gmail_label_addable("DRAFT"));
   TEST_ASSERT_FALSE(gmail_label_addable("CHAT"));
   TEST_ASSERT_FALSE(gmail_label_addable("YELLOW_STAR"));
   TEST_ASSERT_FALSE(gmail_label_addable("Label_1\",\"TRASH"));
   TEST_ASSERT_FALSE(gmail_label_addable(""));
   TEST_ASSERT_FALSE(gmail_label_addable(NULL));

   const char *labels[] = { "INBOX", "TRASH", "UNREAD", "Label_7", "SENT", "CATEGORY_SOCIAL" };
   char packed[EMAIL_UNDO_LABELS_MAX];
   TEST_ASSERT_EQUAL_INT(0, gmail_labels_pack(labels, 6, packed, sizeof(packed)));
   TEST_ASSERT_EQUAL_STRING("INBOX,UNREAD,Label_7,CATEGORY_SOCIAL", packed);
   char small[14];
   TEST_ASSERT_EQUAL_INT(2, gmail_labels_pack(labels, 6, small, sizeof(small)));
   TEST_ASSERT_EQUAL_STRING("INBOX,UNREAD", small);

   char body[256];
   TEST_ASSERT_TRUE(gmail_labels_add_body(packed, false, body, sizeof(body)));
   TEST_ASSERT_EQUAL_STRING(
       "{\"addLabelIds\":[\"INBOX\",\"UNREAD\",\"Label_7\",\"CATEGORY_SOCIAL\"]}", body);
   TEST_ASSERT_TRUE(gmail_labels_add_body(packed, true, body, sizeof(body)));
   TEST_ASSERT_EQUAL_STRING("{\"addLabelIds\":[\"INBOX\",\"UNREAD\",\"CATEGORY_SOCIAL\"]}", body);
   TEST_ASSERT_FALSE(gmail_labels_add_body("Label_7", true, body, sizeof(body)));
   TEST_ASSERT_FALSE(gmail_labels_add_body("", false, body, sizeof(body)));
   TEST_ASSERT_FALSE(gmail_labels_add_body("TRASH,SPAM", false, body, sizeof(body)));
   TEST_ASSERT_FALSE(gmail_labels_add_body(packed, false, body, 30)); /* doesn't fit */
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_plain);
   RUN_TEST(test_alternative_base64);
   RUN_TEST(test_charsets);
   RUN_TEST(test_html_bodies);
   RUN_TEST(test_attachments);
   RUN_TEST(test_mixed_text_parts_join);
   RUN_TEST(test_cut_fetch);
   RUN_TEST(test_text_cap);
   RUN_TEST(test_addresses);
   RUN_TEST(test_headers_only);
   RUN_TEST(test_part_list);
   RUN_TEST(test_fetch_on_read);
   RUN_TEST(test_bombs);
   RUN_TEST(test_display_sanitize);
   RUN_TEST(test_content_id);
   RUN_TEST(test_addr_helpers);
   RUN_TEST(test_charset_spellings);
   RUN_TEST(test_utf8_cut);
   RUN_TEST(test_structure);
   RUN_TEST(test_base64_leading_whitespace);
   RUN_TEST(test_invisible_text);
   RUN_TEST(test_part_id_fits);
   RUN_TEST(test_long_address_header);
   RUN_TEST(test_header_bombs);
   RUN_TEST(test_gmail_walk);
   RUN_TEST(test_base64url);
   RUN_TEST(test_plain_with_angle_brackets);
   RUN_TEST(test_dashes_in_bodies);
   RUN_TEST(test_header_blocks_without_types);
   RUN_TEST(test_address_budget);
   RUN_TEST(test_charset_aliases);
   RUN_TEST(test_body_fallbacks);
   RUN_TEST(test_prescan_reads_like_gmime);
   RUN_TEST(test_prescan_takes_gmime_type);
   RUN_TEST(test_gmail_header_fields);
   RUN_TEST(test_gmail_undo_labels);
   return UNITY_END();
}
