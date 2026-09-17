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
 * Charles Schwab price-history analytics — pure, dependency-free (unit-testable).
 */

#ifndef SCHWAB_STATS_H
#define SCHWAB_STATS_H

typedef enum {
   SCHWAB_FREQ_DAILY = 0,
   SCHWAB_FREQ_WEEKLY,
   SCHWAB_FREQ_MONTHLY,
} schwab_freq_t;

typedef struct {
   double pct_change;   /**< % change over the window (vs base) */
   double period_high;  /**< max of candle highs */
   double period_low;   /**< min of candle lows */
   double last_close;   /**< most recent close */
   double volatility;   /**< annualized realized vol (log returns), fraction; <0 = N/A */
   int vol_n;           /**< number of returns used for volatility */
   double max_drawdown; /**< most negative peak-to-trough (fraction, <=0) */
   double sma20;        /**< 20-period SMA; <0 = insufficient candles */
   double sma50;        /**< 50-period SMA; <0 = insufficient candles */
} schwab_hist_stats_t;

/**
 * Compute price-history analytics from close/high/low arrays (length @p n,
 * oldest-first). @p base is previousClose (<=0 → use first close). @p freq sets
 * the volatility annualization factor (√252 daily, √52 weekly, √12 monthly).
 * Pure; safe on degenerate input (volatility=-1 / sma=-1 when insufficient data).
 * Returns 0 on success, 1 if inputs are NULL or @p n < 1.
 */
int schwab_history_stats(const double *close,
                         const double *high,
                         const double *low,
                         int n,
                         double base,
                         schwab_freq_t freq,
                         schwab_hist_stats_t *out);

#endif /* SCHWAB_STATS_H */
