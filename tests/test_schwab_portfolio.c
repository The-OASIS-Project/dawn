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
 * Unit tests for the Schwab portfolio parser/serializer. Synthetic /accounts
 * fixture (invented numbers, real shape) — no live account data.
 */

#include <json-c/json.h>
#include <string.h>

#include "dawn_error.h"
#include "tools/schwab_portfolio.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* Two accounts: a MARGIN account (equity + ETF-as-COLLECTIVE_INVESTMENT) and a
 * CASH account (an option + a cash-equivalent MMF). Numbers are invented. */
static const char *FIXTURE =
    "[{\"securitiesAccount\":{\"type\":\"MARGIN\",\"accountNumber\":\"12345678\","
    "\"currentBalances\":{\"liquidationValue\":100000.0,\"cashBalance\":20000.0},"
    "\"positions\":["
    "{\"instrument\":{\"symbol\":\"AAPL\",\"assetType\":\"EQUITY\",\"description\":\"Apple Inc.\"},"
    "\"longQuantity\":100,"
    "\"shortQuantity\":0,\"averagePrice\":150.0,\"marketValue\":19000.0,"
    "\"currentDayProfitLoss\":-100.0,\"longOpenProfitLoss\":4000.0},"
    "{\"instrument\":{\"symbol\":\"SPY\",\"assetType\":\"COLLECTIVE_INVESTMENT\"},\"longQuantity\":"
    "10,"
    "\"shortQuantity\":0,\"marketValue\":6000.0,\"currentDayProfitLoss\":50.0,"
    "\"longOpenProfitLoss\":1000.0}]}},"
    "{\"securitiesAccount\":{\"type\":\"CASH\",\"accountNumber\":\"87654321\","
    "\"currentBalances\":{\"liquidationValue\":30000.0,\"cashBalance\":5000.0},"
    "\"positions\":["
    "{\"instrument\":{\"symbol\":\"TSLA  240119C00250000\",\"assetType\":\"OPTION\"},"
    "\"longQuantity\":2,\"shortQuantity\":0,\"marketValue\":1000.0,\"currentDayProfitLoss\":20.0,"
    "\"longOpenProfitLoss\":-200.0},"
    "{\"instrument\":{\"symbol\":\"SWVXX\",\"assetType\":\"CASH_EQUIVALENT\"},\"longQuantity\":"
    "5000,"
    "\"shortQuantity\":0,\"marketValue\":5000.0,\"currentDayProfitLoss\":0.0,"
    "\"longOpenProfitLoss\":0.0}]}}]";

static void test_asset_type_mapping(void) {
   TEST_ASSERT_EQUAL_INT(SCHWAB_ASSET_EQUITY, schwab_asset_type_from_str("EQUITY"));
   TEST_ASSERT_EQUAL_INT(SCHWAB_ASSET_ETF, schwab_asset_type_from_str("COLLECTIVE_INVESTMENT"));
   TEST_ASSERT_EQUAL_INT(SCHWAB_ASSET_OPTION, schwab_asset_type_from_str("OPTION"));
   TEST_ASSERT_EQUAL_INT(SCHWAB_ASSET_MUTUAL_FUND, schwab_asset_type_from_str("MUTUAL_FUND"));
   TEST_ASSERT_EQUAL_INT(SCHWAB_ASSET_CASH_EQUIVALENT,
                         schwab_asset_type_from_str("CASH_EQUIVALENT"));
   TEST_ASSERT_EQUAL_INT(SCHWAB_ASSET_OTHER, schwab_asset_type_from_str("FIXED_INCOME"));
   TEST_ASSERT_EQUAL_INT(SCHWAB_ASSET_OTHER, schwab_asset_type_from_str(""));
   TEST_ASSERT_EQUAL_INT(SCHWAB_ASSET_OTHER, schwab_asset_type_from_str(NULL));
   TEST_ASSERT_EQUAL_STRING("etf", schwab_asset_type_str(SCHWAB_ASSET_ETF));
   TEST_ASSERT_EQUAL_STRING("cash_equivalent", schwab_asset_type_str(SCHWAB_ASSET_CASH_EQUIVALENT));
}

static void test_parse_bad_input(void) {
   schwab_portfolio_t p;
   TEST_ASSERT_EQUAL_INT(FAILURE, schwab_portfolio_parse(NULL, &p));
   TEST_ASSERT_EQUAL_INT(0, p.account_count);
   TEST_ASSERT_NULL(p.accounts);

   struct json_object *obj = json_tokener_parse("{\"not\":\"an array\"}");
   TEST_ASSERT_EQUAL_INT(FAILURE, schwab_portfolio_parse(obj, &p));
   json_object_put(obj);

   struct json_object *empty = json_tokener_parse("[]");
   TEST_ASSERT_EQUAL_INT(SUCCESS, schwab_portfolio_parse(empty, &p)); /* linked, no accounts */
   TEST_ASSERT_EQUAL_INT(0, p.account_count);
   schwab_portfolio_free(&p);
   json_object_put(empty);
}

static void test_parse_fixture(void) {
   struct json_object *root = json_tokener_parse(FIXTURE);
   TEST_ASSERT_NOT_NULL(root);
   schwab_portfolio_t p;
   TEST_ASSERT_EQUAL_INT(SUCCESS, schwab_portfolio_parse(root, &p));
   json_object_put(root); /* parser copies out; safe to release the tree */

   TEST_ASSERT_EQUAL_INT(2, p.account_count);

   /* Account 0: MARGIN, equity + ETF */
   schwab_account_t *a0 = &p.accounts[0];
   TEST_ASSERT_EQUAL_STRING("MARGIN", a0->acct_type);
   TEST_ASSERT_EQUAL_STRING("\xe2\x80\xa6"
                            "5678",
                            a0->label); /* masked …5678 */
   TEST_ASSERT_EQUAL_INT(2, a0->position_count);
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, 100000.0, a0->value);
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, 20000.0, a0->cash);
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, -50.0, a0->day_change);     /* -100 + 50 */
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, 5000.0, a0->unrealized_pl); /* 4000 + 1000 */
   /* acct position mv = 25000; unrealized% = 5000/(25000-5000) = 25% */
   TEST_ASSERT_DOUBLE_WITHIN(1e-4, 25.0, a0->unrealized_pl_pct);

   schwab_position_t *aapl = &a0->positions[0];
   TEST_ASSERT_EQUAL_STRING("AAPL", aapl->symbol);
   TEST_ASSERT_EQUAL_STRING("Apple Inc.", aapl->description);
   TEST_ASSERT_EQUAL_STRING("", a0->positions[1].description); /* SPY has none → empty */
   TEST_ASSERT_EQUAL_INT(SCHWAB_ASSET_EQUITY, aapl->asset_type);
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, 100.0, aapl->qty);
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, 190.0, aapl->price); /* 19000/100 */
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, 4000.0, aapl->unrealized_pl);
   TEST_ASSERT_DOUBLE_WITHIN(1e-4, 26.6667, aapl->unrealized_pl_pct); /* 4000/15000 */
   TEST_ASSERT_DOUBLE_WITHIN(1e-4, -0.5236, aapl->day_change_pct);    /* -100/19100 */
   TEST_ASSERT_EQUAL_INT(SCHWAB_ASSET_ETF, a0->positions[1].asset_type);

   /* Account 1: CASH, option + cash-equivalent */
   schwab_account_t *a1 = &p.accounts[1];
   TEST_ASSERT_EQUAL_STRING("CASH", a1->acct_type);
   TEST_ASSERT_EQUAL_INT(SCHWAB_ASSET_OPTION, a1->positions[0].asset_type);
   TEST_ASSERT_EQUAL_INT(SCHWAB_ASSET_CASH_EQUIVALENT, a1->positions[1].asset_type);

   /* Totals: value = 130000; day = -30; unrealized = 4800; total mv = 31000 */
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, 130000.0, p.total_value);
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, -30.0, p.total_day_change);
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, 4800.0, p.total_unrealized_pl);
   TEST_ASSERT_DOUBLE_WITHIN(1e-4, 18.3206, p.total_unrealized_pl_pct); /* 4800/26200 */

   schwab_portfolio_free(&p);
   TEST_ASSERT_NULL(p.accounts);
   TEST_ASSERT_EQUAL_INT(0, p.account_count);
}

static void test_to_json_roundtrip(void) {
   struct json_object *root = json_tokener_parse(FIXTURE);
   schwab_portfolio_t p;
   TEST_ASSERT_EQUAL_INT(SUCCESS, schwab_portfolio_parse(root, &p));
   json_object_put(root);
   p.as_of = 1750000000;
   p.ext_hours = false;

   char *js = schwab_portfolio_to_json(&p, "ok", "regular", false);
   TEST_ASSERT_NOT_NULL(js);
   struct json_object *back = json_tokener_parse(js);
   TEST_ASSERT_NOT_NULL(back);

   struct json_object *v = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(back, "status", &v));
   TEST_ASSERT_EQUAL_STRING("ok", json_object_get_string(v));
   TEST_ASSERT_TRUE(json_object_object_get_ex(back, "as_of", &v));
   TEST_ASSERT_EQUAL_INT64(1750000000, json_object_get_int64(v));
   TEST_ASSERT_TRUE(json_object_object_get_ex(back, "total_value", &v));
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, 130000.0, json_object_get_double(v));
   struct json_object *accts = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(back, "accounts", &accts));
   TEST_ASSERT_EQUAL_INT(2, json_object_array_length(accts));
   /* first position of first account carries the asset_type badge */
   struct json_object *a0 = json_object_array_get_idx(accts, 0);
   struct json_object *pos = NULL;
   json_object_object_get_ex(a0, "positions", &pos);
   struct json_object *p0 = json_object_array_get_idx(pos, 0);
   json_object_object_get_ex(p0, "asset_type", &v);
   TEST_ASSERT_EQUAL_STRING("equity", json_object_get_string(v));

   json_object_put(back);
   free(js);
   schwab_portfolio_free(&p);
}

static void test_to_json_state_only(void) {
   /* NULL snapshot → state-only frame: status present, no numeric/accounts. */
   char *js = schwab_portfolio_to_json(NULL, "token_expired", "closed", false);
   TEST_ASSERT_NOT_NULL(js);
   struct json_object *o = json_tokener_parse(js);
   struct json_object *v = NULL;
   TEST_ASSERT_TRUE(json_object_object_get_ex(o, "status", &v));
   TEST_ASSERT_EQUAL_STRING("token_expired", json_object_get_string(v));
   TEST_ASSERT_FALSE(json_object_object_get_ex(o, "accounts", &v));
   TEST_ASSERT_FALSE(json_object_object_get_ex(o, "total_value", &v));
   json_object_put(o);
   free(js);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_asset_type_mapping);
   RUN_TEST(test_parse_bad_input);
   RUN_TEST(test_parse_fixture);
   RUN_TEST(test_to_json_roundtrip);
   RUN_TEST(test_to_json_state_only);
   return UNITY_END();
}
