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
 * Unit tests for memory_history_strip_image_markers().
 *
 * Added when the scan was rewritten from a per-byte strncmp to a memchr
 * fast-skip (7x on a real transcript, and it runs inside the callback that
 * holds the global auth_db mutex).  The function had no coverage, and every
 * case below is one the two implementations could disagree on: a '[' that
 * starts no marker, an unterminated marker, adjacency, and a marker at either
 * boundary.  Six callers share it, so a regression here is wide.
 */

#include <stdlib.h>
#include <string.h>

#include "memory/memory_history_loader.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* Assert the transform and free in one step — every call returns owned memory. */
static void expect(const char *in, const char *want) {
   char *got = memory_history_strip_image_markers(in);
   TEST_ASSERT_NOT_NULL(got);
   TEST_ASSERT_EQUAL_STRING(want, got);
   free(got);
}

static void test_no_markers_is_byte_identical(void) {
   expect("", "");
   expect("plain text with no brackets", "plain text with no brackets");
   /* Bare '[' is the memchr candidate that is NOT a marker — the case the fast
    * skip has to fall through correctly. */
   expect("a [ b", "a [ b");
   expect("[", "[");
   expect("[NOTAMARKER] stays", "[NOTAMARKER] stays");
   expect("[IMAG", "[IMAG"); /* prefix shorter than the needle */
   expect("[[[[", "[[[[");   /* consecutive candidates */
   expect("]]] no opener", "]]] no opener");
}

static void test_markers_are_replaced(void) {
   expect("[IMAGE:abc]", "[image]");
   expect("before [IMAGE:abc] after", "before [image] after");
   expect("[IMAGE:a][IMAGE:b]", "[image][image]"); /* adjacent */
   expect("[IMAGE:a] mid [IMAGE:b]", "[image] mid [image]");
   expect("[IMAGE:]", "[image]"); /* empty payload */
   /* Marker at each boundary. */
   expect("[IMAGE:x] trailing", "[image] trailing");
   expect("leading [IMAGE:x]", "leading [image]");
}

static void test_unterminated_marker_is_copied_verbatim(void) {
   /* No ']' — the rest must survive byte-for-byte, including any later '['. */
   expect("[IMAGE:abc", "[IMAGE:abc");
   expect("text [IMAGE:no close here", "text [IMAGE:no close here");
   expect("[IMAGE:a[IMAGE:b", "[IMAGE:a[IMAGE:b");
   /* A marker runs to the FIRST ']' after its prefix, so a nested-looking
    * "[IMAGE:" inside the payload is just payload — one marker, not two. Safe
    * because the base64 alphabet contains no ']' (see the header), so this shape
    * cannot arise from a real marker. Pinned because it is the case where a
    * scan rewrite would most plausibly diverge from the original. */
   expect("[IMAGE:x[IMAGE:y]", "[image]");
}

static void test_null_input_returns_owned_empty_string(void) {
   char *got = memory_history_strip_image_markers(NULL);
   TEST_ASSERT_NOT_NULL(got);
   TEST_ASSERT_EQUAL_STRING("", got);
   free(got); /* documented as owned — a static "" here would crash */
}

/* The output buffer is sized to the INPUT length, which is only safe because the
 * replacement is never longer than what it replaces ("[image]" is 7, the
 * shortest marker "[IMAGE:]" is 8).  Pin that, since a future marker format
 * would silently overflow. */
static void test_replacement_never_grows_the_buffer(void) {
   TEST_ASSERT_TRUE(strlen("[image]") < strlen("[IMAGE:]"));
   char big[4096];
   memset(big, 'x', sizeof(big) - 1);
   big[sizeof(big) - 1] = '\0';
   memcpy(big, "[IMAGE:0123456789]", 18);
   char *got = memory_history_strip_image_markers(big);
   TEST_ASSERT_NOT_NULL(got);
   TEST_ASSERT_EQUAL_INT(0, strncmp(got, "[image]xxx", 10));
   TEST_ASSERT_EQUAL_size_t(strlen(big) - 18 + 7, strlen(got));
   free(got);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_no_markers_is_byte_identical);
   RUN_TEST(test_markers_are_replaced);
   RUN_TEST(test_unterminated_marker_is_copied_verbatim);
   RUN_TEST(test_null_input_returns_owned_empty_string);
   RUN_TEST(test_replacement_never_grows_the_buffer);
   return UNITY_END();
}
