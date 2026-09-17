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

#include "tools/schwab_stats.h"

#include <math.h>
#include <string.h>

static double sma_last(const double *close, int n, int w) {
   double s = 0.0;
   for (int i = n - w; i < n; i++) {
      s += close[i];
   }
   return s / w;
}

int schwab_history_stats(const double *close,
                         const double *high,
                         const double *low,
                         int n,
                         double base,
                         schwab_freq_t freq,
                         schwab_hist_stats_t *out) {
   if (!out) {
      return 1;
   }
   memset(out, 0, sizeof(*out)); /* always zero the out-struct, even on bad input */
   if (!close || !high || !low || n < 1) {
      return 1;
   }
   out->last_close = close[n - 1];
   out->period_high = high[0];
   out->period_low = low[0];
   for (int i = 0; i < n; i++) {
      if (high[i] > out->period_high) {
         out->period_high = high[i];
      }
      if (low[i] < out->period_low) {
         out->period_low = low[i];
      }
   }

   double base_price = (base > 0.0) ? base : close[0];
   out->pct_change = (base_price > 0.0) ? (out->last_close - base_price) / base_price * 100.0 : 0.0;

   /* max drawdown: most negative peak-to-trough */
   double peak = close[0];
   out->max_drawdown = 0.0;
   for (int i = 0; i < n; i++) {
      if (close[i] > peak) {
         peak = close[i];
      }
      if (peak > 0.0) {
         double dd = (close[i] - peak) / peak;
         if (dd < out->max_drawdown) {
            out->max_drawdown = dd;
         }
      }
   }

   /* annualized volatility from log returns (Welford, sample variance) */
   out->volatility = -1.0;
   out->vol_n = 0;
   {
      int m = 0;
      double mean = 0.0, m2 = 0.0;
      for (int i = 1; i < n; i++) {
         if (close[i] > 0.0 && close[i - 1] > 0.0) {
            double r = log(close[i] / close[i - 1]);
            m++;
            double delta = r - mean;
            mean += delta / m;
            m2 += delta * (r - mean);
         }
      }
      out->vol_n = m;
      if (m >= 3) {
         double var = m2 / (m - 1);
         double ppy = (freq == SCHWAB_FREQ_DAILY)    ? 252.0
                      : (freq == SCHWAB_FREQ_WEEKLY) ? 52.0
                                                     : 12.0;
         out->volatility = sqrt(var) * sqrt(ppy); /* fraction, e.g. 0.35 */
      }
   }

   out->sma20 = (n >= 20) ? sma_last(close, n, 20) : -1.0;
   out->sma50 = (n >= 50) ? sma_last(close, n, 50) : -1.0;
   return 0;
}
