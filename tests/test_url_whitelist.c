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
   RUN_TEST(test_public_address_allowed);
   RUN_TEST(test_invalid_url_blocked);
   return UNITY_END();
}
