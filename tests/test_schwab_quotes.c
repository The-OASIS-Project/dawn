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
 * Unit tests for the Schwab /quotes reference-name parser.
 */

#include <json-c/json.h>
#include <string.h>

#include "dawn_error.h"
#include "tools/schwab_quotes.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* A /quotes response: object keyed by symbol, each with a reference block, plus the
 * top-level "errors" object and one symbol that has no reference block at all. */
static const char *const FIXTURE =
    "{"
    "\"ARM\":{\"assetMainType\":\"EQUITY\",\"symbol\":\"ARM\",\"quote\":{\"lastPrice\":150.0},"
    "\"reference\":{\"description\":\"Arm Holdings plc ADR\",\"exchangeName\":\"NASDAQ\"}},"
    "\"NVDA\":{\"assetMainType\":\"EQUITY\",\"symbol\":\"NVDA\","
    "\"reference\":{\"description\":\"NVIDIA Corp\"}},"
    "\"ZZZZ\":{\"assetMainType\":\"EQUITY\",\"symbol\":\"ZZZZ\"},"
    "\"errors\":{\"invalidSymbols\":[\"ZZZZ\"]}"
    "}";

static schwab_quote_t *find(schwab_quote_t *q, int n, const char *sym) {
   for (int i = 0; i < n; i++) {
      if (strcmp(q[i].symbol, sym) == 0) {
         return &q[i];
      }
   }
   return NULL;
}

static void test_parse_reference_names(void) {
   struct json_object *root = json_tokener_parse(FIXTURE);
   TEST_ASSERT_NOT_NULL(root);
   schwab_quote_t q[8];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, schwab_quotes_parse(root, q, 8, &n));
   TEST_ASSERT_EQUAL_INT(3, n); /* ARM, NVDA, ZZZZ — the "errors" object is skipped */

   schwab_quote_t *arm = find(q, n, "ARM");
   TEST_ASSERT_NOT_NULL(arm);
   TEST_ASSERT_EQUAL_STRING("Arm Holdings plc ADR", arm->description);

   schwab_quote_t *nvda = find(q, n, "NVDA");
   TEST_ASSERT_NOT_NULL(nvda);
   TEST_ASSERT_EQUAL_STRING("NVIDIA Corp", nvda->description);

   schwab_quote_t *zzzz = find(q, n, "ZZZZ");
   TEST_ASSERT_NOT_NULL(zzzz);
   TEST_ASSERT_EQUAL_STRING("", zzzz->description); /* no reference block → empty */

   json_object_put(root);
}

static void test_parse_bad_input(void) {
   int n = 5;
   TEST_ASSERT_EQUAL_INT(FAILURE, schwab_quotes_parse(NULL, NULL, 0, &n));
   TEST_ASSERT_EQUAL_INT(0, n);

   struct json_object *arr = json_tokener_parse("[1,2,3]"); /* non-object root */
   schwab_quote_t q[2];
   TEST_ASSERT_EQUAL_INT(FAILURE, schwab_quotes_parse(arr, q, 2, &n));
   json_object_put(arr);
}

static void test_parse_respects_max(void) {
   struct json_object *root = json_tokener_parse(FIXTURE);
   schwab_quote_t q[1];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, schwab_quotes_parse(root, q, 1, &n));
   TEST_ASSERT_EQUAL_INT(1, n); /* capped at max */
   json_object_put(root);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_parse_reference_names);
   RUN_TEST(test_parse_bad_input);
   RUN_TEST(test_parse_respects_max);
   return UNITY_END();
}
