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
 * Unit tests for the URL fetcher's private-address block. Only IP-literal URLs,
 * so nothing leaves the machine.
 */

#include "tools/tavily_rate_limit.h"
#include "tools/url_fetch_tavily.h"
#include "tools/url_fetcher.h"
#include "tools/url_fetcher_internal.h"
#include "unity.h"

/* The Tavily fallback isn't under test: never configured */
int url_fetch_tavily(const char *url, char **out_content, size_t *out_size) {
   (void)url;
   (void)out_content;
   (void)out_size;
   return 1;
}
int url_fetch_tavily_is_configured(void) {
   return 0;
}
bool tavily_rate_limit_check(int user_id) {
   (void)user_id;
   return false;
}
int tavily_rate_limit_resolve_user_id(void) {
   return 0;
}

void setUp(void) {
}

void tearDown(void) {
}

static void test_private_addresses_blocked(void) {
   TEST_ASSERT_TRUE(url_is_blocked("http://169.254.169.254/latest/meta-data/"));
   TEST_ASSERT_TRUE(url_is_blocked("http://10.1.2.3/"));
   TEST_ASSERT_TRUE(url_is_blocked("http://192.168.1.1:8080/"));
   TEST_ASSERT_TRUE(url_is_blocked("http://127.0.0.1/"));
   TEST_ASSERT_TRUE(url_is_blocked("http://localhost/"));
   TEST_ASSERT_TRUE(url_is_blocked("http://[::1]/"));
   TEST_ASSERT_TRUE(url_is_blocked("http://[::1]:8080/"));
   TEST_ASSERT_TRUE(url_is_blocked("http://[fd00::1]/"));
   TEST_ASSERT_TRUE(url_is_blocked("http://[::ffff:127.0.0.1]/"));
}

/* The host is what curl connects to, not the text before the first ':' or '/' */
static void test_userinfo_does_not_hide_the_host(void) {
   TEST_ASSERT_TRUE(url_is_blocked("http://example.com@127.0.0.1/"));
   TEST_ASSERT_TRUE(url_is_blocked("http://user:pass@169.254.169.254/"));
   TEST_ASSERT_TRUE(url_is_blocked("http://user@[::1]/"));
}

/* A whitelist CIDR needs a whole prefix 0-32: a missing or bad one is not
 * read as /0, which would match every address. */
static void test_cidr_prefix(void) {
   unsigned int net = 0, mask = 0;
   TEST_ASSERT_EQUAL_INT(1, url_fetcher_parse_cidr("10.1.2.3/8", &net, &mask));
   TEST_ASSERT_EQUAL_HEX32(0x0A000000, net);
   TEST_ASSERT_EQUAL_HEX32(0xFF000000, mask);
   TEST_ASSERT_EQUAL_INT(1, url_fetcher_parse_cidr("192.168.1.7/32", &net, &mask));
   TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFF, mask);

   const char *bad[] = { "10.0.0.0/",   "10.0.0.0/abc", "10.0.0.0/33", "10.0.0.0/-1",
                         "10.0.0.0/8x", "300.0.0.0/8",  "10.0.0.0",    NULL };
   for (int i = 0; bad[i]; i++) {
      TEST_ASSERT_EQUAL_INT_MESSAGE(0, url_fetcher_parse_cidr(bad[i], &net, &mask), bad[i]);
   }
}

static void test_public_address_allowed(void) {
   TEST_ASSERT_FALSE(url_is_blocked("http://93.184.216.34/"));
}

static void test_invalid_url_blocked(void) {
   TEST_ASSERT_TRUE(url_is_blocked("not a url"));
   TEST_ASSERT_TRUE(url_is_blocked(NULL));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_private_addresses_blocked);
   RUN_TEST(test_userinfo_does_not_hide_the_host);
   RUN_TEST(test_cidr_prefix);
   RUN_TEST(test_public_address_allowed);
   RUN_TEST(test_invalid_url_blocked);
   return UNITY_END();
}
