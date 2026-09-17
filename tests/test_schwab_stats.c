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
 * Unit tests for schwab_history_stats — pure analytics, no I/O.
 */

#include "tools/schwab_stats.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* NULL / empty inputs are rejected. */
static void test_bad_inputs(void) {
   schwab_hist_stats_t s;
   double x[1] = { 1.0 };
   TEST_ASSERT_EQUAL_INT(1, schwab_history_stats(NULL, x, x, 1, 0, SCHWAB_FREQ_DAILY, &s));
   TEST_ASSERT_EQUAL_INT(1, schwab_history_stats(x, x, x, 0, 0, SCHWAB_FREQ_DAILY, &s));
}

/* Single candle: no returns, no SMA, zero drawdown/change. */
static void test_single(void) {
   double c[1] = { 42.0 };
   schwab_hist_stats_t s;
   TEST_ASSERT_EQUAL_INT(0, schwab_history_stats(c, c, c, 1, 0, SCHWAB_FREQ_DAILY, &s));
   TEST_ASSERT_EQUAL_DOUBLE(42.0, s.last_close);
   TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, s.pct_change);
   TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, s.max_drawdown);
   TEST_ASSERT_EQUAL_INT(0, s.vol_n);
   TEST_ASSERT_TRUE(s.volatility < 0.0); /* n/a */
   TEST_ASSERT_TRUE(s.sma20 < 0.0);
   TEST_ASSERT_TRUE(s.sma50 < 0.0);
}

/* Monotone rise: 40% gain, zero drawdown, 4 returns, positive vol. */
static void test_monotone_up(void) {
   double c[5] = { 10, 11, 12, 13, 14 };
   schwab_hist_stats_t s;
   TEST_ASSERT_EQUAL_INT(0, schwab_history_stats(c, c, c, 5, 0, SCHWAB_FREQ_DAILY, &s));
   TEST_ASSERT_DOUBLE_WITHIN(1e-9, 40.0, s.pct_change);
   TEST_ASSERT_DOUBLE_WITHIN(1e-9, 14.0, s.period_high);
   TEST_ASSERT_DOUBLE_WITHIN(1e-9, 10.0, s.period_low);
   TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, s.max_drawdown);
   TEST_ASSERT_EQUAL_INT(4, s.vol_n);
   TEST_ASSERT_TRUE(s.volatility > 0.0);
}

/* Known drawdown + volatility. closes 100,120,90,108 (highs=lows=closes).
 * max drawdown = (90-120)/120 = -0.25.
 * log-return vol annualized (daily): stdev(n-1) of {ln1.2, ln0.75, ln1.2}
 *   × √252 ≈ 4.3082 (fraction). */
static void test_drawdown_vol(void) {
   double c[4] = { 100, 120, 90, 108 };
   schwab_hist_stats_t s;
   TEST_ASSERT_EQUAL_INT(0, schwab_history_stats(c, c, c, 4, 100.0, SCHWAB_FREQ_DAILY, &s));
   TEST_ASSERT_DOUBLE_WITHIN(1e-9, 8.0, s.pct_change); /* (108-100)/100 */
   TEST_ASSERT_DOUBLE_WITHIN(1e-9, 120.0, s.period_high);
   TEST_ASSERT_DOUBLE_WITHIN(1e-9, 90.0, s.period_low);
   TEST_ASSERT_DOUBLE_WITHIN(1e-9, -0.25, s.max_drawdown);
   TEST_ASSERT_EQUAL_INT(3, s.vol_n);
   TEST_ASSERT_DOUBLE_WITHIN(0.01, 4.3082, s.volatility);
}

/* previousClose base changes the % change. Same closes, base=110:
 * (108-110)/110 * 100 = -1.81818%. */
static void test_prev_close_base(void) {
   double c[4] = { 100, 120, 90, 108 };
   schwab_hist_stats_t s;
   TEST_ASSERT_EQUAL_INT(0, schwab_history_stats(c, c, c, 4, 110.0, SCHWAB_FREQ_DAILY, &s));
   TEST_ASSERT_DOUBLE_WITHIN(1e-4, -1.818182, s.pct_change);
}

/* Flat 50-candle series: SMAs computable and equal to price; zero vol/drawdown. */
static void test_flat_smas(void) {
   double c[50];
   for (int i = 0; i < 50; i++) {
      c[i] = 100.0;
   }
   schwab_hist_stats_t s;
   TEST_ASSERT_EQUAL_INT(0, schwab_history_stats(c, c, c, 50, 0, SCHWAB_FREQ_DAILY, &s));
   TEST_ASSERT_DOUBLE_WITHIN(1e-9, 100.0, s.sma20);
   TEST_ASSERT_DOUBLE_WITHIN(1e-9, 100.0, s.sma50);
   TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, s.volatility); /* constant → zero */
   TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, s.max_drawdown);
   TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, s.pct_change);
}

/* Annualization factor differs by frequency (same returns, larger factor daily). */
static void test_annualization(void) {
   double c[4] = { 100, 120, 90, 108 };
   schwab_hist_stats_t d, w, m;
   schwab_history_stats(c, c, c, 4, 0, SCHWAB_FREQ_DAILY, &d);
   schwab_history_stats(c, c, c, 4, 0, SCHWAB_FREQ_WEEKLY, &w);
   schwab_history_stats(c, c, c, 4, 0, SCHWAB_FREQ_MONTHLY, &m);
   TEST_ASSERT_TRUE(d.volatility > w.volatility);
   TEST_ASSERT_TRUE(w.volatility > m.volatility);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_bad_inputs);
   RUN_TEST(test_single);
   RUN_TEST(test_monotone_up);
   RUN_TEST(test_drawdown_vol);
   RUN_TEST(test_prev_close_base);
   RUN_TEST(test_flat_smas);
   RUN_TEST(test_annualization);
   return UNITY_END();
}
