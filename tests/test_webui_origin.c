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
 * A Referer's origin for the same-origin check (webui_origin.c).
 */

#include <string.h>

#include "unity.h"
#include "webui/webui_origin.h"

void setUp(void) {
}

void tearDown(void) {
}

static const char *origin_of(const char *referer) {
   static char out[128];
   return webui_referer_origin(referer, out, sizeof(out)) ? out : NULL;
}

static void test_the_origin_ends_at_the_path_query_or_fragment(void) {
   TEST_ASSERT_EQUAL_STRING("https://host.example:3000",
                            origin_of("https://host.example:3000/a/b"));
   TEST_ASSERT_EQUAL_STRING("https://host.example", origin_of("https://host.example?x=1"));
   TEST_ASSERT_EQUAL_STRING("https://host.example", origin_of("https://host.example#top"));
   /* No path at all: the whole thing (a prefix check against "https://host/"
    * used to reject this). */
   TEST_ASSERT_EQUAL_STRING("http://host.example", origin_of("http://host.example"));
}

/* The origin is exact, so a look-alike host doesn't pass as a prefix. */
static void test_a_lookalike_host_keeps_its_own_origin(void) {
   TEST_ASSERT_EQUAL_STRING("https://host.example.evil.com",
                            origin_of("https://host.example.evil.com/"));
}

static void test_malformed_referers_are_refused(void) {
   TEST_ASSERT_NULL(origin_of("https://host.example@evil.com/"));
   TEST_ASSERT_NULL(origin_of("https:///path"));
   TEST_ASSERT_NULL(origin_of("://host"));
   TEST_ASSERT_NULL(origin_of("host.example/path"));
   TEST_ASSERT_NULL(origin_of(NULL));
   char tiny[8];
   TEST_ASSERT_FALSE(webui_referer_origin("https://host.example/", tiny, sizeof(tiny)));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_the_origin_ends_at_the_path_query_or_fragment);
   RUN_TEST(test_a_lookalike_host_keeps_its_own_origin);
   RUN_TEST(test_malformed_referers_are_refused);
   return UNITY_END();
}
