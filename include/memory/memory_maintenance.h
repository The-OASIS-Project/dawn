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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Memory Maintenance API
 *
 * Nightly decay and pruning orchestration for the memory subsystem.
 * Called from auth_maintenance thread.
 */

#ifndef MEMORY_MAINTENANCE_H
#define MEMORY_MAINTENANCE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run nightly memory decay and pruning
 *
 * Checks the current hour (decay_hour) and last-run timestamp before
 * executing. Safe to call every maintenance cycle — will no-op if
 * conditions are not met.  Each night it prunes the consolidation record
 * (batches older than MEMORY_CONSOLIDATE_RECORD_DAYS) whatever memory and
 * decay are set to; the rest runs only with both enabled.
 *
 * For each user with memory data:
 * 1. Applies confidence decay to facts and preferences
 * 2. Prunes low-confidence facts
 * 3. Prunes old superseded facts
 * 4. Prunes old summaries
 */
void memory_run_nightly_decay(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_MAINTENANCE_H */
