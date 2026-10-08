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
 * libFuzzer harness for the HTML-to-text extractors (html_parser.c): any bytes
 * as a fetched web page (Markdown, with and without a base URL for links) and
 * as an email's HTML part (plain text).  Pages and mail come from anyone.
 * Built by tests/fuzz/run_fuzzers.sh.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tools/html_parser.h"

/* An extractor's output is NUL-terminated text, or nothing. */
static void check(int rc, char *text) {
   if (rc == HTML_PARSE_SUCCESS && !text)
      abort();
   if (text)
      (void)strlen(text);
   free(text);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
   /* Callers hold the page as a C string: the fetch buffer is NUL-terminated. */
   char *html = malloc(size + 1);
   if (!html)
      return 0;
   memcpy(html, data, size);
   html[size] = '\0';

   /* Each call writes text before check() reads it: the order of a call's
    * arguments isn't specified in C. */
   char *text = NULL;
   int rc = html_extract_text(html, size, &text);
   check(rc, text);
   text = NULL;
   rc = html_extract_text_with_base(html, size, &text, "https://example.com/a/b/page.html");
   check(rc, text);
   text = NULL;
   rc = html_extract_text_plain(html, size, &text);
   check(rc, text);

   free(html);
   return 0;
}
