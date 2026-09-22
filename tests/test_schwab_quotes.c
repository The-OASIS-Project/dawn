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
#include <stdio.h>
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
    "\"ARM\":{\"assetMainType\":\"EQUITY\",\"symbol\":\"ARM\","
    "\"quote\":{\"lastPrice\":150.0,\"netChange\":2.5,\"netPercentChange\":1.6949},"
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
   TEST_ASSERT_EQUAL_INT(SCHWAB_ASSET_EQUITY, arm->asset_type);
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, 150.0, arm->last);
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, 2.5, arm->net_change);
   TEST_ASSERT_DOUBLE_WITHIN(1e-4, 1.6949, arm->net_change_pct);

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

static void test_watch_payload(void) {
   schwab_quote_t q[3];
   memset(q, 0, sizeof(q));
   snprintf(q[0].symbol, sizeof(q[0].symbol), "NVDA");
   snprintf(q[0].description, sizeof(q[0].description), "NVIDIA Corp");
   q[0].asset_type = SCHWAB_ASSET_EQUITY;
   q[0].last = 178.12;
   q[0].net_change = 2.34;
   q[0].net_change_pct = 1.33;
   q[0].quotable = true;
   q[0].has_ext = true; /* carries an ext-hours overlay */
   q[0].ext_last = 179.50;
   q[0].ext_change = 1.38;
   q[0].ext_change_pct = 0.775;
   snprintf(q[1].symbol, sizeof(q[1].symbol), "ARM"); /* no description → key omitted */
   q[1].last = 150.0;
   q[1].quotable = true;                               /* no ext data → ext object omitted */
   snprintf(q[2].symbol, sizeof(q[2].symbol), "TSLQ"); /* unquotable → flagged minimal row */
   q[2].quotable = false;

   /* market "post" → an active ext session, so ext rows are emitted. */
   struct json_object *p = schwab_quotes_watch_payload_jobj(q, 3, "ok", "post", 1750000000);
   TEST_ASSERT_NOT_NULL(p);
   struct json_object *v = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(p, "status", &v));
   TEST_ASSERT_EQUAL_STRING("ok", json_object_get_string(v));
   TEST_ASSERT_TRUE(json_object_object_get_ex(p, "as_of", &v));
   TEST_ASSERT_EQUAL_INT64(1750000000, json_object_get_int64(v));
   struct json_object *syms = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(p, "symbols", &syms));
   TEST_ASSERT_EQUAL_INT(3, json_object_array_length(syms));
   struct json_object *s0 = json_object_array_get_idx(syms, 0);
   json_object_object_get_ex(s0, "symbol", &v);
   TEST_ASSERT_EQUAL_STRING("NVDA", json_object_get_string(v));
   json_object_object_get_ex(s0, "description", &v);
   TEST_ASSERT_EQUAL_STRING("NVIDIA Corp", json_object_get_string(v));
   json_object_object_get_ex(s0, "asset_type", &v);
   TEST_ASSERT_EQUAL_STRING("equity", json_object_get_string(v));
   json_object_object_get_ex(s0, "day_change_pct", &v);
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1.33, json_object_get_double(v));
   /* a quotable row omits the flag */
   TEST_ASSERT_FALSE(json_object_object_get_ex(s0, "quotable", &v));
   /* NVDA carries ext data → an ext object with price/change/change_pct */
   struct json_object *ext = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(s0, "ext", &ext));
   json_object_object_get_ex(ext, "price", &v);
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, 179.50, json_object_get_double(v));
   json_object_object_get_ex(ext, "change", &v);
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1.38, json_object_get_double(v));
   json_object_object_get_ex(ext, "change_pct", &v);
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.775, json_object_get_double(v));
   /* ARM has no description → the key is omitted; no ext data → no ext object */
   struct json_object *s1 = json_object_array_get_idx(syms, 1);
   TEST_ASSERT_FALSE(json_object_object_get_ex(s1, "description", &v));
   TEST_ASSERT_FALSE(json_object_object_get_ex(s1, "ext", &v));
   /* TSLQ is unquotable → quotable:false, no price/asset_type */
   struct json_object *s2 = json_object_array_get_idx(syms, 2);
   TEST_ASSERT_TRUE(json_object_object_get_ex(s2, "quotable", &v));
   TEST_ASSERT_FALSE(json_object_get_boolean(v));
   TEST_ASSERT_FALSE(json_object_object_get_ex(s2, "asset_type", &v));
   TEST_ASSERT_FALSE(json_object_object_get_ex(s2, "price", &v));
   json_object_put(p);

   /* Same quotes, but market "closed" → the ext overlay must be suppressed even though
    * the cached rows still carry has_ext (guards the stale-ext-after-close contract). */
   struct json_object *pc = schwab_quotes_watch_payload_jobj(q, 3, "ok", "closed", 1750000000);
   struct json_object *csyms = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(pc, "symbols", &csyms));
   struct json_object *c0 = json_object_array_get_idx(csyms, 0);
   TEST_ASSERT_FALSE(json_object_object_get_ex(c0, "ext", &v)); /* NVDA ext suppressed */
   json_object_put(pc);

   /* Empty watchlist → symbols:[] */
   struct json_object *pe = schwab_quotes_watch_payload_jobj(NULL, 0, "ok", "closed", 0);
   TEST_ASSERT_TRUE(json_object_object_get_ex(pe, "symbols", &syms));
   TEST_ASSERT_EQUAL_INT(0, json_object_array_length(syms));
   json_object_put(pe);
}

/* A /quotes response with the ext-hours fields: MU has regular+extended (a live
 * pre/post overlay), AMD has a regular block but no extended (no overlay). */
static const char *const EXT_FIXTURE =
    "{"
    "\"MU\":{\"assetMainType\":\"EQUITY\",\"symbol\":\"MU\","
    "\"quote\":{\"lastPrice\":1044.26,\"netChange\":28.46,\"netPercentChange\":2.8},"
    "\"regular\":{\"regularMarketLastPrice\":1043.96,\"regularMarketTradeTime\":1790020800078},"
    "\"extended\":{\"lastPrice\":1036.31,\"tradeTime\":1790060255000}},"
    "\"AMD\":{\"assetMainType\":\"EQUITY\",\"symbol\":\"AMD\","
    "\"quote\":{\"lastPrice\":200.0},\"regular\":{\"regularMarketLastPrice\":200.0}}"
    "}";

static void test_parse_ext(void) {
   struct json_object *root = json_tokener_parse(EXT_FIXTURE);
   TEST_ASSERT_NOT_NULL(root);
   schwab_quote_t q[4];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, schwab_quotes_parse(root, q, 4, &n));
   TEST_ASSERT_EQUAL_INT(2, n);

   schwab_quote_t *mu = find(q, n, "MU");
   TEST_ASSERT_NOT_NULL(mu);
   TEST_ASSERT_TRUE(mu->has_ext);
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1036.31, mu->ext_last);
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1036.31 - 1043.96, mu->ext_change); /* vs regular close */
   TEST_ASSERT_DOUBLE_WITHIN(1e-4, (1036.31 - 1043.96) / 1043.96 * 100.0, mu->ext_change_pct);

   /* AMD has no extended block → no overlay. */
   schwab_quote_t *amd = find(q, n, "AMD");
   TEST_ASSERT_NOT_NULL(amd);
   TEST_ASSERT_FALSE(amd->has_ext);
   json_object_put(root);
}

static void test_parse_invalid(void) {
   struct json_object *root = json_tokener_parse(FIXTURE); /* errors.invalidSymbols = ["ZZZZ"] */
   TEST_ASSERT_NOT_NULL(root);
   char invalid[8][SCHWAB_SYMBOL_MAX];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, schwab_quotes_parse_invalid(root, invalid, 8, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_STRING("ZZZZ", invalid[0]);
   json_object_put(root);

   /* No errors block → none flagged (SUCCESS, n=0). */
   struct json_object *clean = json_tokener_parse("{\"NVDA\":{\"symbol\":\"NVDA\"}}");
   n = 5;
   TEST_ASSERT_EQUAL_INT(SUCCESS, schwab_quotes_parse_invalid(clean, invalid, 8, &n));
   TEST_ASSERT_EQUAL_INT(0, n);
   json_object_put(clean);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_parse_reference_names);
   RUN_TEST(test_parse_bad_input);
   RUN_TEST(test_parse_respects_max);
   RUN_TEST(test_parse_invalid);
   RUN_TEST(test_parse_ext);
   RUN_TEST(test_watch_payload);
   return UNITY_END();
}
