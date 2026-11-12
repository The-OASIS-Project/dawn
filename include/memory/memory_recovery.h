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
 * Memory Extraction Recovery
 *
 * Recovers memory extraction work for conversations whose
 * last_extracted_msg_count < message_count after the configured idle
 * window.  Catches conversations left unconsolidated by daemon crashes
 * or extraction failures.  Driven by [memory.recovery] config.
 */

#ifndef MEMORY_RECOVERY_H
#define MEMORY_RECOVERY_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the recovery worker thread.
 *
 * Spawns a low-priority thread that runs an immediate startup pass and
 * then re-runs at memory.recovery_recurring_interval_seconds.  No-op when
 * the memory subsystem or recovery feature is disabled.
 *
 * @return SUCCESS on start (or graceful no-op), FAILURE on thread error.
 */
int memory_recovery_start(void);

/**
 * @brief Stop the recovery worker thread and join it.
 */
void memory_recovery_stop(void);

/**
 * @brief Run a single recovery pass synchronously.
 *
 * Public for unit-test and manual-trigger use; the worker thread calls
 * this internally.  Scans for stuck conversations and re-triggers
 * extraction one at a time, waiting for each to finish.  May block for
 * minutes per conversation.
 *
 * @param triggered_out (optional) extraction trigger count
 * @param skipped_out   (optional) over-cap or no-op skips
 */
void memory_recovery_run_pass(int *triggered_out, int *skipped_out);

/**
 * @brief Tests only: run memory_recovery_run_pass() without the worker thread.
 *
 * A pass processes conversations only while the worker runs, and it sleeps
 * after starting an extraction (a settle before checking that it started)
 * and while waiting for it (a poll).  With @p wait set, a pass runs as if
 * the worker were up, and calls @p wait in place of each of those sleeps,
 * in that order: the settle first, then each poll while the extraction
 * still runs.  A test then decides when the extraction it started is seen
 * running and when it ends, with no timing.  NULL restores both.  Never
 * together with memory_recovery_start(): the worker thread reads the hook
 * unlocked.
 */
void memory_recovery_set_test_wait(void (*wait)(void));

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_RECOVERY_H */
