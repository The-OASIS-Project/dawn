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
 * Unit tests for the Schwab transaction classifier. The fixture is a SCRUBBED,
 * synthetic array (fake symbols/amounts, no PII) matching the shape pinned from
 * a live /transactions capture: a TRADE with fee legs, a sell, a dividend whose
 * payer lives only in `description`, an internal JOURNAL, and ACH deposit/
 * withdrawal rows. If Schwab's schema drifts, these assertions go red.
 */

#include <json-c/json.h>

#include "tools/schwab_txn.h"
#include "unity.h"

/* Synthetic — mirrors the real field shape, no real account data. */
static const char *FIXTURE =
    "["
    /* 0: buy 2 sh AMD, $1.00 commission (netAmount already nets fees) */
    "{\"type\":\"TRADE\",\"status\":\"VALID\",\"netAmount\":-420.82,"
    "\"time\":\"2026-08-10T14:23:22+0000\",\"description\":null,\"transferItems\":["
    "{\"amount\":-1.00,\"cost\":0.0,\"feeType\":\"COMMISSION\","
    "\"instrument\":{\"assetType\":\"CURRENCY\",\"symbol\":\"CURRENCY_USD\"}},"
    "{\"amount\":0.0,\"cost\":0.0,\"feeType\":\"SEC_FEE\","
    "\"instrument\":{\"assetType\":\"CURRENCY\",\"symbol\":\"CURRENCY_USD\"}},"
    "{\"amount\":2.0,\"cost\":-420.82,\"price\":210.41,\"positionEffect\":\"OPENING\","
    "\"instrument\":{\"assetType\":\"EQUITY\",\"symbol\":\"AMD\"}}]},"
    /* 1: sell 3 sh NVDA */
    "{\"type\":\"TRADE\",\"status\":\"VALID\",\"netAmount\":900.00,"
    "\"time\":\"2026-08-11T15:00:00+0000\",\"transferItems\":["
    "{\"amount\":-3.0,\"cost\":900.00,\"price\":300.00,\"positionEffect\":\"CLOSING\","
    "\"instrument\":{\"assetType\":\"EQUITY\",\"symbol\":\"NVDA\"}}]},"
    /* 2: dividend — payer only in description, no symbol field */
    "{\"type\":\"DIVIDEND_OR_INTEREST\",\"status\":\"VALID\",\"netAmount\":0.18,"
    "\"time\":\"2026-07-30T08:08:07+0000\",\"description\":\"MARVELL TECHNOLOGY INC\","
    "\"qualifiedDividend\":true,\"transferItems\":["
    "{\"amount\":0.18,\"cost\":0.0,\"price\":0.0,"
    "\"instrument\":{\"assetType\":\"CURRENCY\",\"symbol\":\"CURRENCY_USD\"}}]},"
    /* 3: internal journal (ADR fee) */
    "{\"type\":\"JOURNAL\",\"status\":\"VALID\",\"netAmount\":-0.04,"
    "\"time\":\"2026-09-10T09:35:07+0000\",\"description\":\"ADR FEE\",\"transferItems\":["
    "{\"amount\":-0.04,\"cost\":0.0,"
    "\"instrument\":{\"assetType\":\"CURRENCY\",\"symbol\":\"CURRENCY_USD\"}}]},"
    /* 4: ACH deposit */
    "{\"type\":\"ACH_RECEIPT\",\"status\":\"VALID\",\"netAmount\":1000.00,"
    "\"time\":\"2026-09-01T12:00:00+0000\",\"description\":\"ACH DEPOSIT\"},"
    /* 5: ACH withdrawal */
    "{\"type\":\"ACH_DISBURSEMENT\",\"status\":\"VALID\",\"netAmount\":-500.00,"
    "\"time\":\"2026-09-05T12:00:00+0000\",\"description\":\"ACH WITHDRAWAL\"}"
    "]";

static struct json_object *g_rows;

void setUp(void) {
   g_rows = json_tokener_parse(FIXTURE);
}
void tearDown(void) {
   if (g_rows) {
      json_object_put(g_rows);
      g_rows = NULL;
   }
}

static void classify_idx(int i, schwab_txn_t *out) {
   schwab_txn_classify(json_object_array_get_idx(g_rows, i), out);
}

static void test_fixture_parses(void) {
   TEST_ASSERT_NOT_NULL(g_rows);
   TEST_ASSERT_TRUE(json_object_is_type(g_rows, json_type_array));
   TEST_ASSERT_EQUAL_INT(6, (int)json_object_array_length(g_rows));
}

static void test_trade_buy(void) {
   schwab_txn_t t;
   classify_idx(0, &t);
   TEST_ASSERT_EQUAL_INT(SCHWAB_TXN_TRADE, t.cat);
   TEST_ASSERT_FALSE(t.external);
   TEST_ASSERT_EQUAL_STRING("AMD", t.symbol);
   TEST_ASSERT_EQUAL_STRING("2026-08-10", t.date);
   TEST_ASSERT_TRUE(t.qty > 0.0); /* buy */
   TEST_ASSERT_DOUBLE_WITHIN(1e-9, -420.82, t.net_amount);
   TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.00, t.fee_total); /* |commission| */
}

static void test_trade_sell(void) {
   schwab_txn_t t;
   classify_idx(1, &t);
   TEST_ASSERT_EQUAL_INT(SCHWAB_TXN_TRADE, t.cat);
   TEST_ASSERT_EQUAL_STRING("NVDA", t.symbol);
   TEST_ASSERT_TRUE(t.qty < 0.0); /* sell */
}

static void test_dividend_has_no_symbol(void) {
   schwab_txn_t t;
   classify_idx(2, &t);
   TEST_ASSERT_EQUAL_INT(SCHWAB_TXN_DIVIDEND, t.cat);
   TEST_ASSERT_EQUAL_STRING("", t.symbol); /* payer is in memo, not a symbol field */
   TEST_ASSERT_EQUAL_STRING("MARVELL TECHNOLOGY INC", t.memo);
   TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.18, t.net_amount);
}

static void test_journal_is_internal(void) {
   schwab_txn_t t;
   classify_idx(3, &t);
   TEST_ASSERT_EQUAL_INT(SCHWAB_TXN_INTERNAL, t.cat);
   TEST_ASSERT_TRUE(schwab_txn_is_internal(&t));
   TEST_ASSERT_FALSE(t.external);
}

static void test_deposit_withdrawal_external(void) {
   schwab_txn_t d, w;
   classify_idx(4, &d);
   classify_idx(5, &w);
   TEST_ASSERT_EQUAL_INT(SCHWAB_TXN_DEPOSIT, d.cat);
   TEST_ASSERT_TRUE(d.external);
   TEST_ASSERT_EQUAL_INT(SCHWAB_TXN_WITHDRAWAL, w.cat);
   TEST_ASSERT_TRUE(w.external);
}

static void test_aggregate(void) {
   schwab_txn_agg_t a;
   schwab_txn_agg_init(&a);
   for (int i = 0; i < 6; i++) {
      schwab_txn_t t;
      classify_idx(i, &t);
      schwab_txn_agg_add(&a, &t);
   }
   TEST_ASSERT_EQUAL_INT(6, a.count);
   TEST_ASSERT_EQUAL_INT(2, a.n_trade);
   TEST_ASSERT_EQUAL_INT(1, a.n_dividend);
   TEST_ASSERT_EQUAL_INT(1, a.n_deposit);
   TEST_ASSERT_EQUAL_INT(1, a.n_withdrawal);
   TEST_ASSERT_EQUAL_INT(1, a.n_internal);
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, 479.18, a.net_trade); /* -420.82 + 900.00 */
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.18, a.income);
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1000.00, a.deposits);
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, -500.00, a.withdrawals);
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1.00, a.fees);
   TEST_ASSERT_DOUBLE_WITHIN(1e-6, -0.04, a.net_internal);
}

static void test_null_row_safe(void) {
   schwab_txn_t t;
   schwab_txn_classify(NULL, &t);
   TEST_ASSERT_EQUAL_INT(SCHWAB_TXN_OTHER, t.cat);
}

static void test_mask_numbers(void) {
   char out[96];
   /* >= 5 digits, incl. groups split by a single separator → masked to '#'. */
   schwab_txn_mask_numbers("ACCT 12345", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("ACCT #", out);
   schwab_txn_mask_numbers("FROM 1234 5678", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("FROM #", out);
   schwab_txn_mask_numbers("SSN 123-45-6789", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("SSN #", out);
   /* Short numbers (dates, share counts) pass through. */
   schwab_txn_mask_numbers("buy 500 shares", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("buy 500 shares", out);
   schwab_txn_mask_numbers("XFER 12-3", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("XFER 12-3", out);
   /* Security names untouched; NULL and empty safe. */
   schwab_txn_mask_numbers("MARVELL TECHNOLOGY INC", out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("MARVELL TECHNOLOGY INC", out);
   schwab_txn_mask_numbers(NULL, out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("", out);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_fixture_parses);
   RUN_TEST(test_trade_buy);
   RUN_TEST(test_trade_sell);
   RUN_TEST(test_dividend_has_no_symbol);
   RUN_TEST(test_journal_is_internal);
   RUN_TEST(test_deposit_withdrawal_external);
   RUN_TEST(test_aggregate);
   RUN_TEST(test_null_row_safe);
   RUN_TEST(test_mask_numbers);
   return UNITY_END();
}
