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
 * Unit tests for src/tools/html_parser.c — HTML to Markdown conversion.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tools/html_parser.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* ── Helper ──────────────────────────────────────────────────────────────── */

static int parse(const char *html, char **out) {
   return html_extract_text(html, strlen(html), out);
}

/* ── Basic extraction ────────────────────────────────────────────────────── */

static void test_extract_paragraph(void) {
   char *out = NULL;
   int rc = parse("<html><body><p>Hello world, this is content.</p></body></html>", &out);
   TEST_ASSERT_EQUAL_INT(HTML_PARSE_SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_NOT_NULL(strstr(out, "Hello world"));
   free(out);
}

static void test_extract_headings(void) {
   char *out = NULL;
   int rc = parse("<h1>Title</h1><h2>Subtitle</h2><p>Body content text here.</p>", &out);
   TEST_ASSERT_EQUAL_INT(HTML_PARSE_SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(out);
   /* H1 should produce # marker */
   TEST_ASSERT_NOT_NULL(strstr(out, "#"));
   TEST_ASSERT_NOT_NULL(strstr(out, "Title"));
   TEST_ASSERT_NOT_NULL(strstr(out, "Subtitle"));
   free(out);
}

static void test_extract_bold_italic(void) {
   char *out = NULL;
   int rc = parse("<p>This is <b>bold</b> and <i>italic</i> text content.</p>", &out);
   TEST_ASSERT_EQUAL_INT(HTML_PARSE_SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_NOT_NULL(strstr(out, "bold"));
   TEST_ASSERT_NOT_NULL(strstr(out, "italic"));
   free(out);
}

static void test_extract_links(void) {
   char *out = NULL;
   int rc = parse("<p>Visit <a href=\"https://example.com\">our website</a> for more info.</p>",
                  &out);
   TEST_ASSERT_EQUAL_INT(HTML_PARSE_SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_NOT_NULL(strstr(out, "our website"));
   /* Markdown link contains the URL */
   TEST_ASSERT_NOT_NULL(strstr(out, "example.com"));
   free(out);
}

static void test_extract_unordered_list(void) {
   char *out = NULL;
   int rc = parse("<ul><li>First item</li><li>Second item</li><li>Third item</li></ul>", &out);
   TEST_ASSERT_EQUAL_INT(HTML_PARSE_SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_NOT_NULL(strstr(out, "First item"));
   TEST_ASSERT_NOT_NULL(strstr(out, "Second item"));
   TEST_ASSERT_NOT_NULL(strstr(out, "Third item"));
   free(out);
}

static void test_extract_ordered_list(void) {
   char *out = NULL;
   int rc = parse("<ol><li>Step one alpha</li><li>Step two beta</li></ol>", &out);
   TEST_ASSERT_EQUAL_INT(HTML_PARSE_SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_NOT_NULL(strstr(out, "Step one"));
   TEST_ASSERT_NOT_NULL(strstr(out, "Step two"));
   free(out);
}

/* ── Strips noise ────────────────────────────────────────────────────────── */

static void test_strips_script(void) {
   char *out = NULL;
   int rc = parse("<html><head><script>alert('xss');</script></head>"
                  "<body><p>Safe paragraph content here.</p></body></html>",
                  &out);
   TEST_ASSERT_EQUAL_INT(HTML_PARSE_SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_NOT_NULL(strstr(out, "Safe paragraph"));
   /* Script content must NOT be in output */
   TEST_ASSERT_NULL(strstr(out, "alert"));
   TEST_ASSERT_NULL(strstr(out, "xss"));
   free(out);
}

static void test_strips_style(void) {
   char *out = NULL;
   int rc = parse("<html><head><style>body{color:red;}</style></head>"
                  "<body><p>Visible content text here.</p></body></html>",
                  &out);
   TEST_ASSERT_EQUAL_INT(HTML_PARSE_SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_NOT_NULL(strstr(out, "Visible content"));
   TEST_ASSERT_NULL(strstr(out, "color:red"));
   free(out);
}

/* ── HTML entities ───────────────────────────────────────────────────────── */

static void test_decodes_html_entities(void) {
   char *out = NULL;
   int rc = parse("<p>Tom &amp; Jerry are &lt;cartoon&gt; characters &quot;classic&quot;.</p>",
                  &out);
   TEST_ASSERT_EQUAL_INT(HTML_PARSE_SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_NOT_NULL(strstr(out, "Tom & Jerry"));
   TEST_ASSERT_NOT_NULL(strstr(out, "<cartoon>"));
   /* Should NOT contain raw entity */
   TEST_ASSERT_NULL(strstr(out, "&amp;"));
   free(out);
}

/* ── Base URL resolution ─────────────────────────────────────────────────── */

static void test_relative_url_with_base(void) {
   char *out = NULL;
   int rc = html_extract_text_with_base(
       "<p>Click <a href=\"/about\">here</a> please please please.</p>",
       strlen("<p>Click <a href=\"/about\">here</a> please please please.</p>"), &out,
       "https://example.com");
   TEST_ASSERT_EQUAL_INT(HTML_PARSE_SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(out);
   /* Relative URL should resolve to absolute */
   TEST_ASSERT_NOT_NULL(strstr(out, "example.com/about"));
   free(out);
}

static void test_relative_url_without_base(void) {
   char *out = NULL;
   int rc = parse("<p>Click <a href=\"/about\">here</a> please please please.</p>", &out);
   TEST_ASSERT_EQUAL_INT(HTML_PARSE_SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(out);
   /* Without base URL, relative path passes through */
   TEST_ASSERT_NOT_NULL(strstr(out, "/about"));
   free(out);
}

/* ── Plain text mode ─────────────────────────────────────────────────────── */

static void test_plain_text_mode_omits_link_url(void) {
   const char *html = "<p>Click <a href=\"https://tracker.example.com/abc\">"
                      "the link to read more articles now</a> here.</p>";
   char *out = NULL;
   int rc = html_extract_text_plain(html, strlen(html), &out);
   TEST_ASSERT_EQUAL_INT(HTML_PARSE_SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_NOT_NULL(strstr(out, "the link to read"));
   /* Plain mode should NOT include URL */
   TEST_ASSERT_NULL(strstr(out, "tracker.example.com"));
   free(out);
}

/* ── Error handling ──────────────────────────────────────────────────────── */

static void test_null_input(void) {
   char *out = NULL;
   int rc = html_extract_text(NULL, 0, &out);
   TEST_ASSERT_EQUAL_INT(HTML_PARSE_ERROR_INVALID_INPUT, rc);
}

static void test_null_output_param(void) {
   int rc = html_extract_text("<p>hello</p>", 12, NULL);
   TEST_ASSERT_EQUAL_INT(HTML_PARSE_ERROR_INVALID_INPUT, rc);
}

static void test_empty_html_returns_empty_error(void) {
   char *out = NULL;
   int rc = parse("", &out);
   /* Empty input may produce HTML_PARSE_ERROR_EMPTY (output too small) */
   TEST_ASSERT_TRUE(rc == HTML_PARSE_ERROR_EMPTY || rc == HTML_PARSE_ERROR_INVALID_INPUT);
   if (out) {
      free(out);
   }
}

/* ── main ────────────────────────────────────────────────────────────────── */

/* Hostile pages: an unclosed skip tag or noise class, or an unclosed comment,
 * repeated.  Each used to rescan the rest of the page per occurrence
 * (quadratic: 500 KB took a minute and more); now each search runs once. */
static long elapsed_ms(const struct timespec *a) {
   struct timespec b;
   clock_gettime(CLOCK_MONOTONIC, &b);
   return (b.tv_sec - a->tv_sec) * 1000L + (b.tv_nsec - a->tv_nsec) / 1000000L;
}

static void check_linear(const char *unit) {
   const size_t n = strlen(unit);
   const size_t reps = 500000 / n;
   char *html = malloc(reps * n + 1);
   TEST_ASSERT_NOT_NULL(html);
   for (size_t i = 0; i < reps; i++)
      memcpy(html + i * n, unit, n);
   html[reps * n] = '\0';
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   char *out = NULL;
   html_extract_text_plain(html, reps * n, &out);
   /* Linear is tens of ms (more under a sanitizer); quadratic was minutes. */
   TEST_ASSERT_LESS_THAN_INT(3000, elapsed_ms(&t));
   free(out);
   free(html);
}

static void test_hostile_pages_are_linear(void) {
   check_linear("<input>");
   check_linear("<meta x>");
   check_linear("<div class=\"modal\">");
   check_linear("<!--x>");
   /* More unclosed names than the parser remembers, cycling */
   char cycle[2048] = "";
   for (int i = 0; i < 40; i++) {
      char one[48];
      snprintf(one, sizeof(one), "<t%d class=modal>", i);
      strcat(cycle, one);
   }
   check_linear(cycle);
   /* CSS at-rules that never close: each used to rescan 64 KB */
   check_linear("x @media {a ");
   check_linear("x @media a ");
   check_linear("<p>x @layer {a</p>");
}

int main(void) {
   UNITY_BEGIN();

   /* Basic extraction */
   RUN_TEST(test_extract_paragraph);
   RUN_TEST(test_extract_headings);
   RUN_TEST(test_extract_bold_italic);
   RUN_TEST(test_extract_links);
   RUN_TEST(test_extract_unordered_list);
   RUN_TEST(test_extract_ordered_list);

   /* Noise stripping */
   RUN_TEST(test_strips_script);
   RUN_TEST(test_strips_style);

   /* Entities */
   RUN_TEST(test_decodes_html_entities);

   /* URL resolution */
   RUN_TEST(test_relative_url_with_base);
   RUN_TEST(test_relative_url_without_base);

   /* Plain text mode */
   RUN_TEST(test_plain_text_mode_omits_link_url);

   /* Error handling */
   RUN_TEST(test_null_input);
   RUN_TEST(test_null_output_param);
   RUN_TEST(test_empty_html_returns_empty_error);

   /* Hostile input */
   RUN_TEST(test_hostile_pages_are_linear);

   return UNITY_END();
}
