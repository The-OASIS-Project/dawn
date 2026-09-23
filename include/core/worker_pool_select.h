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
 * Pure idle-worker selection for the reserve-aware non-blocking ASR borrow.
 * Header-only + dependency-free so the reserve arithmetic (the off-by-one that
 * decides whether the LAST idle context may be lent to speculative work) is
 * unit-testable without initializing the real worker pool (GPU + models).
 * worker_pool_try_borrow_asr() drives this exact function, so the tested logic
 * is the shipped logic.
 */

#ifndef WORKER_POOL_SELECT_H
#define WORKER_POOL_SELECT_H

#include <stdbool.h>

/**
 * @brief Pick an idle worker to lend while leaving a reserve of idle workers.
 * @param idle       array of length @p n; idle[i] != 0 means worker i is idle.
 * @param n          number of workers.
 * @param keep_idle  minimum idle workers to leave un-lent (negative treated as 0).
 * @return index of the first idle worker IF strictly more than @p keep_idle are
 *         idle (so a reserve remains after lending one), otherwise -1.
 */
static inline int worker_pool_select_idle(const unsigned char *idle, int n, int keep_idle) {
   if (!idle || n <= 0) {
      return -1;
   }
   if (keep_idle < 0) {
      keep_idle = 0;
   }
   int idle_count = 0;
   int first_idle = -1;
   for (int i = 0; i < n; i++) {
      if (idle[i]) {
         idle_count++;
         if (first_idle < 0) {
            first_idle = i;
         }
      }
   }
   if (first_idle < 0 || idle_count <= keep_idle) {
      return -1;
   }
   return first_idle;
}

#endif /* WORKER_POOL_SELECT_H */
