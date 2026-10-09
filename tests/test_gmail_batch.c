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
 * The Gmail batch metadata fetch without the network (gmail_batch.c): the
 * request body for some of a listing's messages, the reply matched back by
 * Content-ID (a rate-limited part stays pending, a deleted one is dropped),
 * and search terms cleaned for Gmail's query syntax.
 */

#include <stdio.h>
#include <string.h>

#include "tools/gmail_client_internal.h"
#include "unity.h"

void setUp(void) {
}

void tearDown(void) {
}

#define B "resp_bound"

/* One reply part answering listing index @p i with @p status and @p json. */
static void add_part(char *buf, size_t len, int i, int status, const char *json) {
   size_t used = strlen(buf);
   snprintf(buf + used, len - used,
            "--" B "\r\nContent-Type: application/http\r\nContent-ID: <response-%d>\r\n\r\n"
            "HTTP/1.1 %d OK\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n%s\r\n",
            i, status, json);
}

static void test_body_names_the_asked_messages(void) {
   gmail_msg_id_t ids[4] = { { "aa01" }, { "bb02" }, { "not-hex" }, { "dd04" } };
   const int which[] = { 1, 2, 3 };
   char *body = gmail_batch_body(ids, which, 3);
   TEST_ASSERT_NOT_NULL(body);
   TEST_ASSERT_NOT_NULL(strstr(body, "Content-ID: <1>"));
   TEST_ASSERT_NOT_NULL(strstr(body, "/messages/bb02?format=metadata"));
   TEST_ASSERT_NOT_NULL(strstr(body, "Content-ID: <3>"));
   TEST_ASSERT_NULL(strstr(body, "aa01"));    /* not asked for */
   TEST_ASSERT_NULL(strstr(body, "not-hex")); /* refused */
   TEST_ASSERT_NOT_NULL(strstr(body, "--" GMAIL_BATCH_BOUNDARY "--\r\n"));
   free(body);
}

static void test_parse_by_content_id(void) {
   static char resp[8192];
   resp[0] = '\0';
   /* Out of order, one rate-limited, one gone, one server error. */
   add_part(resp, sizeof(resp), 2, 200,
            "{\"id\":\"c3\",\"internalDate\":\"3000\",\"payload\":{\"headers\":"
            "[{\"name\":\"Subject\",\"value\":\"third\"}]}}");
   add_part(resp, sizeof(resp), 0, 200,
            "{\"id\":\"a1\",\"internalDate\":\"1000\",\"labelIds\":[\"UNREAD\"]}");
   add_part(resp, sizeof(resp), 1, 429, "{\"error\":{\"code\":429}}");
   add_part(resp, sizeof(resp), 3, 404, "{\"error\":{\"code\":404}}");
   add_part(resp, sizeof(resp), 4, 503, "{\"error\":{\"code\":503}}");
   add_part(resp, sizeof(resp), 9, 200, "{\"id\":\"zz\"}"); /* out of range: ignored */
   strcat(resp, "--" B "--\r\n");

   email_summary_t rows[5];
   memset(rows, 0, sizeof(rows));
   unsigned char state[5] = { 0 };
   TEST_ASSERT_EQUAL_INT(2, gmail_batch_parse(resp, B, 5, rows, state));
   TEST_ASSERT_EQUAL(GMAIL_BATCH_DONE, state[0]);
   TEST_ASSERT_EQUAL_STRING("a1", rows[0].message_id);
   TEST_ASSERT_TRUE(rows[0].unread);
   TEST_ASSERT_EQUAL(GMAIL_BATCH_PENDING, state[1]); /* asked for again */
   TEST_ASSERT_EQUAL(GMAIL_BATCH_DONE, state[2]);
   TEST_ASSERT_EQUAL_STRING("third", rows[2].subject);
   TEST_ASSERT_EQUAL(3, (int)rows[2].date);
   TEST_ASSERT_EQUAL(GMAIL_BATCH_GONE, state[3]);
   TEST_ASSERT_EQUAL(GMAIL_BATCH_PENDING, state[4]);

   /* A second reply for an index already read doesn't overwrite it. */
   resp[0] = '\0';
   add_part(resp, sizeof(resp), 0, 200, "{\"id\":\"other\"}");
   add_part(resp, sizeof(resp), 1, 200, "{\"id\":\"b2\"}");
   strcat(resp, "--" B "--\r\n");
   TEST_ASSERT_EQUAL_INT(1, gmail_batch_parse(resp, B, 5, rows, state));
   TEST_ASSERT_EQUAL_STRING("a1", rows[0].message_id);
   TEST_ASSERT_EQUAL_STRING("b2", rows[1].message_id);

   /* A 401 and a 403 not for rate are given up; a 200 that doesn't parse and
    * a rate-limit 403 stay pending; a lowercase Content-Id still matches; a
    * part without one is ignored. */
   unsigned char st2[5] = { 0 };
   email_summary_t r2[5];
   memset(r2, 0, sizeof(r2));
   resp[0] = '\0';
   add_part(resp, sizeof(resp), 0, 401, "{}");
   add_part(resp, sizeof(resp), 3, 403,
            "{\"error\":{\"errors\":[{\"reason\":\"rateLimitExceeded\"}]}}");
   add_part(resp, sizeof(resp), 4, 403, "{\"error\":{\"errors\":[{\"reason\":\"forbidden\"}]}}");
   add_part(resp, sizeof(resp), 1, 200, "{\"id\":");
   strcat(resp, "--" B "\r\nContent-Type: application/http\r\ncontent-id: <response-2>\r\n\r\n"
                "HTTP/1.1 200 OK\r\n\r\n{\"id\":\"c3\"}\r\n");
   strcat(resp, "--" B "\r\nContent-Type: application/http\r\n\r\nHTTP/1.1 200 OK\r\n\r\n{}\r\n");
   strcat(resp, "--" B "--\r\n");
   TEST_ASSERT_EQUAL_INT(1, gmail_batch_parse(resp, B, 5, r2, st2));
   TEST_ASSERT_EQUAL(GMAIL_BATCH_FAILED, st2[0]); /* a lapsed token: not asked again */
   TEST_ASSERT_EQUAL(GMAIL_BATCH_PENDING, st2[1]);
   TEST_ASSERT_EQUAL(GMAIL_BATCH_DONE, st2[2]);
   TEST_ASSERT_EQUAL(GMAIL_BATCH_PENDING, st2[3]); /* a 403 for rate: asked again */
   TEST_ASSERT_EQUAL(GMAIL_BATCH_FAILED, st2[4]);  /* another 403: given up */

   /* An empty or headerless reply reads nothing. */
   TEST_ASSERT_EQUAL_INT(0, gmail_batch_parse(NULL, B, 5, rows, state));
   TEST_ASSERT_EQUAL_INT(0, gmail_batch_parse("--" B "\r\n--" B "\r\nx", B, 5, rows, state));
}

static void test_query_term_drops_quotes_and_currency(void) {
   char out[64];
   gmail_query_term("Now $850,000", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("Now 850,000", out);
   gmail_query_term("say \"hi\" \xE2\x82\xAC"
                    "5 \xC2\xA3"
                    "6 \xC2\xA5"
                    "7",
                    out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("say hi 5 6 7", out);
   gmail_query_term("caf\xC3\xA9 \xC2\xA9", out, sizeof(out)); /* other UTF-8 kept */
   TEST_ASSERT_EQUAL_STRING("caf\xC3\xA9 \xC2\xA9", out);
   gmail_query_term("\xE2\x82", out, sizeof(out)); /* a cut sequence is kept, not overrun */
   TEST_ASSERT_EQUAL_STRING("\xE2\x82", out);
   gmail_query_term("abcdef", out, 4);
   TEST_ASSERT_EQUAL_STRING("abc", out);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_body_names_the_asked_messages);
   RUN_TEST(test_parse_by_content_id);
   RUN_TEST(test_query_term_drops_quotes_and_currency);
   return UNITY_END();
}
