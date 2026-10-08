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

#include <stdbool.h>
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

/* ── Hidden text in an email (plain mode) ──────────────────────────────────── */

static char *plain(const char *html) {
   char *out = NULL;
   TEST_ASSERT_EQUAL_INT(HTML_PARSE_SUCCESS, html_extract_text_plain(html, strlen(html), &out));
   return out;
}

/* What a reader never sees is left out, and only that.  Each case is a
 * way a sender hides text from the reader, so the model must not read it. */
static void test_plain_drops_hidden_elements(void) {
   static const char *const hidden[] = {
      "<div style=\"display:none\">SECRET</div>",
      "<div style = 'DISPLAY : NONE !important'>SECRET</div>",
      "<span style=\"visibility:hidden\">SECRET</span>",
      "<span style=\"visibility:collapse\">SECRET</span>",
      "<span style=\"opacity:0\">SECRET</span>",
      "<span style=\"opacity:3%\">SECRET</span>",
      "<span style=\"font-size:0px\">SECRET</span>",
      "<span style=\"font-size:1px;color:#fff\">SECRET</span>",
      "<span style=\"font-size:0.01em\">SECRET</span>",
      "<span style=\"font:0/0 a\">SECRET</span>",
      "<span style=\"color:transparent\">SECRET</span>",
      "<span style=\"color:rgba(0,0,0,0)\">SECRET</span>",
      "<span style=\"color:#0000\">SECRET</span>",
      "<div style=\"max-height:0;overflow:hidden\">SECRET</div>",
      "<div style=\"max-height:1px;overflow-y:hidden\">SECRET</div>",
      "<div style=\"width:0;overflow:clip\">SECRET</div>",
      "<div style=\"position:absolute;left:-9999px\">SECRET</div>",
      "<div style=\"text-indent:-10000px\">SECRET</div>",
      "<div style=\"clip:rect(0,0,0,0);position:absolute\">SECRET</div>",
      "<div style=\"clip-path:inset(50%)\">SECRET</div>",
      "<div style=\"transform:scale(0)\">SECRET</div>",
      "<div hidden>SECRET</div>",
      "<noembed>SECRET</noembed>",
      /* the last declaration wins, comments, escapes, references */
      "<div style=\"display:block;display:none\">SECRET</div>",
      "<div style=\"display:/**/none\">SECRET</div>",
      "<div style=\"display:\\6e one\">SECRET</div>",
      "<div style=\"\\64isplay:none\">SECRET</div>",
      "<div style=\"display&colon;none\">SECRET</div>",
      "<div style=\"display:&#110;one\">SECRET</div>",
      /* nesting, quotes, raw text, bogus comments */
      "<div style=\"display:none\"><div>a</div>SECRET</div>",
      "<div style=\"display:none\"><!-- </div> -->SECRET</div>",
      "<div style=\"display:none\"><a title=\"</div>\">SECRET</a></div>",
      "<div hidden><textarea></div></textarea>SECRET</div>",
      "<div hidden><? </div>SECRET</div>",
      "<div hidden><img alt=\"</div>\">SECRET</div>",
      /* <style> rules */
      "<style>.pre, p#x{display:none}</style><span class=\"a pre\">SECRET</span>",
      "<style>p#x{display:none}</style><p id=\"x\">SECRET</p>",
      "<style><!-- .h{display:none} --></style><span class=\"h\">SECRET</span>",
      "<style>@media screen{.h{display:none}}</style><span class=\"h\">SECRET</span>",
      "<style>@supports (display:grid){.h{display:none}}</style><span class=\"h\">SECRET</span>",
      "<style>.H{display:none}</style><span class=\"h\">SECRET</span>",
      "<style>.h{display:none}</style><span class=\"&#104;\">SECRET</span>",
      /* selectors: the subject's tag, classes and id, with specificity */
      "<style>u{display:none}</style><u>SECRET</u>",
      "<style>div .h{display:none}</style><div><span class=\"h\">SECRET</span></div>",
      "<style>.a.b{display:none}</style><span class=\"b a\">SECRET</span>",
      "<style>#x{display:none}.y{display:inline;color:red}</style>"
      "<span id=\"x\" class=\"y\">SECRET</span>",
      "<style>.h/**/{display:none}</style><span class=\"h\">SECRET</span>",
      "<style>.\\68{display:none}</style><span class=\"h\">SECRET</span>",
      "<style>@media (min-width:1px){.h{display:none}}</style><span class=\"h\">SECRET</span>",
      "<style>.h{content:\"}\";display:none}</style><span class=\"h\">SECRET</span>",
      "<style>.h{/* } */display:none}</style><span class=\"h\">SECRET</span>",
      "<div title=\"<!--\"><style>.h{display:none}</style></div><span class=\"h\">SECRET</span>",
      "<!--><style>.h{display:none}</style><span class=\"h\">SECRET</span>",
      /* end tags a browser ignores */
      "<div style=\"display:none\"><table><tr><td></div>SECRET</td></tr></table></div>",
      "<span style=\"display:none\"><div></span>SECRET</div></span>",
      /* declarations */
      "<div style=\"font-family:'(';display:none\">SECRET</div>",
      "<div style=\"font-family:a\\(;display:none\">SECRET</div>",
      "<span style=\"font:700 0px/0 a\">SECRET</span>",
      "<span style=\"font:italic 400 0px a\">SECRET</span>",
      "<div style=\"font-size:0\"><span style=\"font-size:2em\">SECRET</span></div>",
      "<div style=\"font-size:0\"><span style=\"font-size:calc(100%)\">SECRET</span></div>",
      "<span style=\"font-size:3px\">SECRET</span>",
      "<span style=\"opacity:0.04\">SECRET</span>",
      "<span style=\"color:rgba(1,2,3,0.01)\">SECRET</span>",
      "<span style=\"filter:opacity(0)\">SECRET</span>",
      "<span style=\"content-visibility:hidden\">SECRET</span>",
      "<span style=\"scale:0\">SECRET</span>",
      "<span style=\"transform:scale(0.001)\">SECRET</span>",
      "<span style=\"transform:translateX(-9999px)\">SECRET</span>",
      "<div style=\"position:absolute;left:9999px\">SECRET</div>",
      "<span style=\"-webkit-text-fill-color:transparent\">SECRET</span>",
      /* raw text closed with a space */
      "<script>var a = \"SECRET\";</script ><p>x</p>",
      "<style>p{} /* SECRET */</style ><p>x</p>",
      "<svg><text>SECRET</text></svg ><p>x</p>",
      /* comments and tags read as a browser reads them */
      "<!--><div style=\"display:none\"><!-- -->SECRET</div>",
      "<!--->x<div style=\"display:none\"><!-- -->SECRET</div>",
      "<!-- x --!><div style=\"display:none\"><!-- -->SECRET</div>",
      "<i x=a=\" ><div hidden><b y=x\">SECRET</b></div></i>",
      "<style>.h{display:none}</style><i x=a=' ><div class=h><b y=x'>SECRET</b></div></i>",
      /* inherited reset keywords take the parent's */
      "<div style=\"visibility:hidden\"><span style=\"visibility:inherit\">SECRET</span></div>",
      "<div style=\"visibility:hidden\"><span style=\"visibility:unset\">SECRET</span></div>",
      "<div style=\"color:transparent\"><span style=\"color:currentcolor\">SECRET</span></div>",
      "<div style=\"-webkit-text-fill-color:transparent\"><span style=\"color:red\">SECRET</span>"
      "</div>",
      /* the cascade, per property */
      "<style>.h{display:block}.k{display:none}.h{color:red}</style>"
      "<span class=\"h k\">SECRET</span>",
      "<style>.h{display:none}.zzz .h{display:block}</style><span class=\"h\">SECRET</span>",
      "<style>@media (max-width:9999px){.h{display:none}}</style><span class=\"h\">SECRET</span>",
      "<style>@media not print{.h{display:none}}</style><span class=\"h\">SECRET</span>",
      "<style>.a.b.c.d.e{display:none}</style><span class=\"a b c d e\">SECRET</span>",
      "<style>abcdefghijklmnopq{display:none}</style><abcdefghijklmnopq>SECRET</abcdefghijklmnopq>",
      "<span style=\"font-size:calc(0px)\">SECRET</span>",
      "<span style=\"font:calc(1px) a\">SECRET</span>",
      "<span style=\"transform:matrix(0,0,0,0,0,0)\">SECRET</span>",
      "<svg><title><style>.h{display:none}</style></title></svg><span class=\"h\">SECRET</span>",
      "<details><summary>Summary.</summary>SECRET</details>",
      "<video>SECRET</video>",
      /* each property cascades on its own: showing one doesn't undo another */
      "<style>.x{position:absolute;left:-9999px}.y{top:0}</style><div class=\"x y\">SECRET</div>",
      "<style>.x{opacity:0}</style><span class=\"x\" style=\"filter:opacity(1)\">SECRET</span>",
      "<style>.x{width:0;overflow:hidden}</style>"
      "<div class=\"x\" style=\"max-width:100px\">SECRET</div>",
      "<style>.x{transform:scale(0)}</style><span class=\"x\" style=\"scale:1\">SECRET</span>",
      "<style>.x{content-visibility:hidden}</style>"
      "<span class=\"x\" style=\"opacity:1\">SECRET</span>",
      /* inherit and friends win the cascade and take the parent's */
      "<style>.x{visibility:visible}</style><div style=\"visibility:hidden\">"
      "<span class=\"x\" style=\"visibility:inherit\">SECRET</span></div>",
      "<style>span{visibility:visible}.x{visibility:unset}</style>"
      "<div style=\"visibility:hidden\"><span class=\"x\">SECRET</span></div>",
      "<style>.x{color:red}</style><div style=\"color:transparent\">"
      "<span class=\"x\" style=\"color:inherit\">SECRET</span></div>",
      /* a block whose condition isn't evaluated only hides */
      "<style>.x{display:none}@media (min-width:99999px){.x{display:block}}</style>"
      "<span class=\"x\">SECRET</span>",
      "<style>.x{display:none}@supports (bogus:x){.x{display:block}}</style>"
      "<span class=\"x\">SECRET</span>",
      "<style>@layer a{#i.x{display:block}}.x{display:none}</style>"
      "<span id=\"i\" class=\"x\">SECRET</span>",
      "<span style=\"clip-path:polygon(0 0)\">SECRET</span>",
   };
   for (size_t i = 0; i < sizeof(hidden) / sizeof(hidden[0]); i++) {
      char html[2048];
      snprintf(html, sizeof(html), "<body><p>Before.</p>%s<p>After.</p></body>", hidden[i]);
      char *out = plain(html);
      TEST_ASSERT_NULL_MESSAGE(strstr(out, "SECRET"), hidden[i]);
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "Before."), hidden[i]);
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "After."), hidden[i]);
      free(out);
   }
   /* Past any length a sender pads with */
   char big[8192];
   char fill[2200];
   memset(fill, 'a', sizeof(fill) - 1);
   fill[sizeof(fill) - 1] = '\0';
   snprintf(big, sizeof(big),
            "<body><p>Before.</p><div data-x=\"%s\" style=\"font-family:%s;display:none\">"
            "SECRET</div><p>After.</p></body>",
            fill, fill);
   char *out = plain(big);
   TEST_ASSERT_NULL(strstr(out, "SECRET"));
   TEST_ASSERT_NOT_NULL(strstr(out, "After."));
   free(out);
   /* A long value, its ')' and the hiding part past where a fixed buffer ended */
   {
      char zeros[600];
      memset(zeros, '0', sizeof(zeros) - 1);
      zeros[sizeof(zeros) - 1] = '\0';
      char longv[2048];
      snprintf(longv, sizeof(longv),
               "<body><p>Before.</p><span style=\"color:rgba(%s,0,0,0)\">SECRET</span>"
               "<p>After.</p></body>",
               zeros);
      char *o = plain(longv);
      TEST_ASSERT_NULL(strstr(o, "SECRET"));
      free(o);
   }
   /* Same-name rules after the hiding one don't push it out */
   {
      char sheet[4096] = "<style>.h{display:none}";
      for (int i = 0; i < 40; i++)
         strcat(sheet, ".h{color:red}");
      strcat(sheet, "</style><body><p>Shown.</p><b class=\"h\">SECRET</b></body>");
      char *o = plain(sheet);
      TEST_ASSERT_NULL(strstr(o, "SECRET"));
      TEST_ASSERT_NOT_NULL(strstr(o, "Shown."));
      free(o);
   }
   /* Deeper than the stack tracks, an element's own style still hides */
   {
      char deep[8192] = "<body><p>Before.</p>";
      for (int i = 0; i < 300; i++)
         strcat(deep, "<div>");
      strcat(deep, "<span style=\"display:none\">SECRET</span>Deep.");
      for (int i = 0; i < 300; i++)
         strcat(deep, "</div>");
      strcat(deep, "<p>After.</p></body>");
      char *o = plain(deep);
      TEST_ASSERT_NULL(strstr(o, "SECRET"));
      TEST_ASSERT_NOT_NULL(strstr(o, "Deep."));
      TEST_ASSERT_NOT_NULL(strstr(o, "After."));
      free(o);
   }
   /* More rules than a fixed table held, the real one last */
   char sheet[8192] = "<style>";
   for (int i = 0; i < 80; i++) {
      char rule[32];
      snprintf(rule, sizeof(rule), ".a%d{display:none}", i);
      strcat(sheet, rule);
   }
   strcat(sheet,
          ".real{display:none}</style><body><p>Shown.</p><b class=\"real\">SECRET</b></body>");
   out = plain(sheet);
   TEST_ASSERT_NULL(strstr(out, "SECRET"));
   TEST_ASSERT_NOT_NULL(strstr(out, "Shown."));
   free(out);
   /* A rule for every element (`*`, or `div > *` whose ancestor isn't
    * followed) hides everything it says to */
   out = plain("<style>*{font-size:0}</style><body><span>SECRET</span></body>");
   TEST_ASSERT_NULL(strstr(out, "SECRET"));
   free(out);
   out = plain("<style>div>*{display:none}</style><body><div><span>SECRET</span></div></body>");
   TEST_ASSERT_NULL(strstr(out, "SECRET"));
   free(out);
   /* <body> and <html> hide everything */
   out = plain("<body style=\"display:none\"><p>SECRET</p></body>");
   TEST_ASSERT_NULL(strstr(out, "SECRET"));
   free(out);
   out = plain("<html hidden><body><p>SECRET</p></body></html>");
   TEST_ASSERT_NULL(strstr(out, "SECRET"));
   free(out);
   /* Never closed: hidden to the end, as a browser shows it */
   out = plain("<body><p>Before.</p><div style=\"display:none\">SECRET<p>more</body>");
   TEST_ASSERT_NOT_NULL(strstr(out, "Before."));
   TEST_ASSERT_NULL(strstr(out, "SECRET"));
   free(out);
   /* <head> and <title> with no <body> */
   out = plain("<html><head><title>SECRET</title></head><p>Shown text.</p></html>");
   TEST_ASSERT_NULL(strstr(out, "SECRET"));
   TEST_ASSERT_NOT_NULL(strstr(out, "Shown text."));
   free(out);
   /* The reader says when it left something out */
   bool dropped = false;
   const char *h = "<body><p>Shown.</p><div hidden>SECRET</div></body>";
   TEST_ASSERT_EQUAL_INT(HTML_PARSE_SUCCESS,
                         html_extract_text_plain_ex(h, strlen(h), &out, &dropped));
   TEST_ASSERT_TRUE(dropped);
   free(out);
   h = "<body><p>Shown.</p></body>";
   TEST_ASSERT_EQUAL_INT(HTML_PARSE_SUCCESS,
                         html_extract_text_plain_ex(h, strlen(h), &out, &dropped));
   TEST_ASSERT_FALSE(dropped);
   free(out);
}

/* Visible text, however its styles look, stays. */
static void test_plain_keeps_visible_elements(void) {
   static const char *const shown[] = {
      "<div style=\"display:block\">KEPT</div>",
      "<div style=\"display:none;display:block\">KEPT</div>",
      "<span style=\"line-height:0\">KEPT</span>",
      "<span style=\"font-size:12px\">KEPT</span>",
      "<span style=\"opacity:0.9\">KEPT</span>",
      "<span style=\"-webkit-display:none\">KEPT</span>",
      "<div style=\"border-width:0;overflow:hidden\">KEPT</div>",
      "<div style=\"max-height:0\">KEPT</div>", /* overflows, still visible */
      "<div style=\"margin-left:-10px\">KEPT</div>",
      "<div style=\"color:rgb(0,0,0)\">KEPT</div>",
      /* inherited properties a child sets back (MJML and hybrid layouts) */
      "<div style=\"font-size:0px\"><div style=\"font-size:14px\">KEPT</div></div>",
      "<td style=\"font-size:0\"><span style=\"font:13px Arial\">KEPT</span></td>",
      "<div style=\"visibility:hidden\"><span style=\"visibility:visible\">KEPT</span></div>",
      "<div style=\"color:transparent\"><p style=\"color:#333\">KEPT</p></div>",
      /* rules that apply only to some screens, or that aren't followed */
      "<div class=\"mobile\">KEPT</div><style>@media "
      "(max-width:600px){.mobile{display:none}}</style>",
      "<span class=\"preheader\">KEPT</span><style>.pre{display:none}</style>",
      "<!--[if mso]><style>.web{display:none}</style><![endif]--><div class=\"web\">KEPT</div>",
      /* end tags left out close implicitly */
      "<p hidden>x<div>KEPT</div>",
      "<table><tr><td style=\"width:1px;overflow:hidden\">KEPT</td></tr></table>",
      "<span style=\"background-clip:text;-webkit-text-fill-color:transparent\">KEPT</span>",
      "<style>[hidden]{display:none!important}</style><div>KEPT</div>",
      "<style>.a:hover{display:none}</style><span class=\"a\">KEPT</span>",
      "<style>@media screen and (max-width:600px){.h{display:none}}</style>"
      "<span class=\"h\">KEPT</span>",
      "<style>@media print{.h{display:none}}</style><span class=\"h\">KEPT</span>",
      "<style>.a.b{display:none}</style><span class=\"a\">KEPT</span>",
      "<div style=\"font-size:0\"><span style=\"font-size:1rem\">KEPT</span></div>",
      "<span style=\"zoom:0\">KEPT</span>",
      "<span style=\"font:700 14px/1.2 Arial\">KEPT</span>",
      "<table><tr><td style=\"display:none\">preheader</table><p>KEPT</p>",
      "<table><tr><td><div style=\"display:none\">x</div></tbody></table><p>KEPT</p>",
      "<details open><summary>s</summary>KEPT</details>",
      "<details><summary>KEPT</summary>x</details>",
      "<style>*{margin:0;padding:0}</style><span>KEPT</span>",
      "<div style=\"visibility:hidden\"><span style=\"visibility:visible\">KEPT</span></div>",
      "<span style=\"font-size:max(1px, 14px)\">KEPT</span>",
      "<style>@media screen{.x{display:none}.x{display:block}}</style><span "
      "class=\"x\">KEPT</span>",
      "<span style=\"clip-path:polygon(0 0, 100% 0, 100% 100%)\">KEPT</span>",
      "<div style=\"color:transparent\"><span style=\"color:#222\">KEPT</span></div>",
      "<table><tr><td style=\"display:none\">x<td>KEPT</td></tr></table>",
      "<ul><li hidden>x<li>KEPT</ul>",
   };
   for (size_t i = 0; i < sizeof(shown) / sizeof(shown[0]); i++) {
      char html[1024];
      snprintf(html, sizeof(html), "<body>%s<p>After.</p></body>", shown[i]);
      char *out = plain(html);
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "KEPT"), shown[i]);
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "After."), shown[i]);
      free(out);
   }
   /* <head> never closed, no <body> */
   char *out = plain(
       "<html><head><meta charset=utf-8><style>p{}</style><div>Hello there</div></html>");
   TEST_ASSERT_NOT_NULL(strstr(out, "Hello there"));
   free(out);
   /* A web page keeps its hidden elements (tabs, accordions): plain mode only */
   const char *page = "<body><p>Some visible article text here.</p>"
                      "<div style=\"display:none\">Tab two text</div></body>";
   TEST_ASSERT_EQUAL_INT(HTML_PARSE_SUCCESS, html_extract_text(page, strlen(page), &out));
   TEST_ASSERT_NOT_NULL(strstr(out, "Tab two text"));
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

/* The two shapes that once cost seconds: many same-name rules against one
 * element's many class tokens, and many tags under a full stack. */
static void test_hostile_visibility_is_bounded(void) {
   const size_t cap = 2 * 1024 * 1024;
   char *html = malloc(cap);
   TEST_ASSERT_NOT_NULL(html);
   size_t n = 0;
   n += (size_t)snprintf(html + n, cap - n, "<style>");
   for (int i = 0; i < 4096; i++)
      n += (size_t)snprintf(html + n, cap - n, ".a%d{color:red}.a{color:red}", i % 7);
   n += (size_t)snprintf(html + n, cap - n, "</style><body><i class=\"");
   while (n + 16 < cap / 2)
      n += (size_t)snprintf(html + n, cap - n, "a a%d ", (int)(n % 7));
   n += (size_t)snprintf(html + n, cap - n, "\">x</i>");
   for (int i = 0; i < 256; i++)
      n += (size_t)snprintf(html + n, cap - n, "<div>");
   while (n + 8 < cap)
      n += (size_t)snprintf(html + n, cap - n, "<li>");
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   char *out = NULL;
   html_extract_text_plain(html, n, &out);
   TEST_ASSERT_LESS_THAN_INT(3000, elapsed_ms(&t));
   free(out);
   free(html);
}

static void test_hostile_pages_are_linear(void) {
   check_linear("<input>");
   check_linear("<meta x>");
   check_linear("<div class=\"modal\">");
   check_linear("<!--x>");
   check_linear("<span hidden></span>");
   check_linear("<div hidden><div>");
   check_linear("<style>.a{display:none}</style><b class=\"a a a\">");
   check_linear("<style>.a{");
   check_linear("<style>@a;");
   check_linear("@a;");
   check_linear("<p hidden>x");
   check_linear("<style>.a,.b,.c{display:none}</style><i class=\"a b c\" style=\"color:red\">");
   check_linear("<style>.a{color:red}</style>");
   check_linear("<div>");
   check_linear("<li>");
   check_linear("<i style=\"font:a a a a a a a a a a a a a a a a a a a a a a a a a\">");
   check_linear("<style>.a.b{color:red}.a.c{color:red}.a.d{color:red}</style>");
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
   RUN_TEST(test_plain_drops_hidden_elements);
   RUN_TEST(test_plain_keeps_visible_elements);
   RUN_TEST(test_hostile_visibility_is_bounded);

   /* Error handling */
   RUN_TEST(test_null_input);
   RUN_TEST(test_null_output_param);
   RUN_TEST(test_empty_html_returns_empty_error);

   /* Hostile input */
   RUN_TEST(test_hostile_pages_are_linear);

   return UNITY_END();
}
