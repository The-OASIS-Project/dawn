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
 * Searching every account of a user at once: each account on its own thread,
 * then the results merged newest first across the accounts.
 */

#ifndef EMAIL_FANOUT_H
#define EMAIL_FANOUT_H

#include <stdbool.h>
#include <stdint.h>

#include "tools/email_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Stack for a thread running account work (a fan-out search, the WebUI's
 * email executor): IMAP, Gmail and GMime calls, a few KB of locals each. */
#define EMAIL_WORKER_STACK_BYTES (1024 * 1024)

/* One account's search. */
typedef struct {
   email_summary_t *rows; /* max rows, newest first; owned (email_fanout_free) */
   int count;
   int cap;         /* rows allocated */
   int missing;     /* rows the search found but couldn't fetch (Gmail refusing) */
   int rc;          /* 0, or the search's failure code */
   email_err_t err; /* why it failed (EMAIL_ERR_CANCELLED: stopped) */
   int64_t ms;      /* how long it took */
} email_fanout_slot_t;

/** Search account @p index into @p slot (rows, count, rc, err), at most @p max rows. */
typedef void (*email_fanout_fn)(void *ctx, int index, email_fanout_slot_t *slot, int max);

/**
 * @brief Run @p fn for accounts 0..@p n-1 at once and wait for all of them
 *
 * Each runs on its own thread under the caller's transfer cancel flag
 * (email_transfer_thread_cancel), so the caller's Stop ends them all.  One
 * whose thread can't start runs on the caller.
 *
 * @return 0, or 1 when out of memory (nothing ran; @p slots are empty)
 */
int email_fanout_run(int n, int max, email_fanout_fn fn, void *ctx, email_fanout_slot_t *slots);

/** Called by email_fanout_merge for an account that failed (not cancelled). */
typedef void (*email_fanout_fail_fn)(void *ctx, int index, const email_fanout_slot_t *slot);

/**
 * @brief Merge @p slots into @p out: the newest @p max rows across all accounts
 *
 * By date, a tie keeping account order.  Every failed account is reported
 * through @p on_fail (any of them may have held newer rows), except a
 * cancelled one, which makes the result false.
 *
 * @param total_out Rows in @p out
 * @return false when an account was cancelled (the rows merged are still in @p out)
 */
bool email_fanout_merge(const email_fanout_slot_t *slots,
                        int n,
                        int max,
                        email_summary_t *out,
                        int *total_out,
                        email_fanout_fail_fn on_fail,
                        void *ctx);

/** Zero and free @p slots' rows. */
void email_fanout_free(email_fanout_slot_t *slots, int n);

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_FANOUT_H */
