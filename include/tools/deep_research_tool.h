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
 * Deep-research tool (DEEP_RESEARCH_DESIGN.md §7/§7a/§15 Step 7).
 *
 * The conversational surface for deep research: a confirmation-gated `start`
 * (propose-then-spawn), an ownership-validated + injection-gated `status`, and an
 * ownership-validated `cancel`.  `start` reuses the background-job substrate
 * (conv_db_create_job + job_manager caps) but spawns the research controller via
 * research_worker_spawn(), not job_worker.  Layer 3.
 */

#ifndef DEEP_RESEARCH_TOOL_H
#define DEEP_RESEARCH_TOOL_H

#include <stddef.h> /* size_t */
#include <stdint.h> /* int64_t */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Register the deep_research tool with the tool registry. */
int deep_research_tool_register(void);

/**
 * @brief Spawn a research run — the shared, ordering-sensitive spawn path used by
 *        BOTH the confirmation-gated deep_research tool (after its confirm gate)
 *        and the headless admin benchmark verb (DEEP_RESEARCH_DESIGN.md §16.3).
 *
 * Runs the full sequence whose ORDERING carries the feature's safety invariants:
 * brief safety filter -> web-search availability -> [jobs] capacity -> create job
 * conversation -> fail-closed job_kind='research' stamp -> create the run row
 * BEFORE spawning -> job_update_emit -> spawn edge -> research_worker_spawn.  No
 * orphan job/run is left on any failure.  P0 forces mode='web'; the caller owns
 * mode policy (the tool downgrades private/both with a note, the admin verb
 * refuses them), so mode is not a parameter here.
 *
 * @param user_id      owning user.
 * @param parent_conv  originating conversation for the tree spawn edge (0 = none).
 * @param brief        the research question (non-empty; re-filtered here so both
 *                     spawn surfaces gate it).
 * @param deliver_to   optional delivery target, already sanitized by the caller
 *                     (NULL = notify only).
 * @param run_id_out   [out] created research_runs id on SUCCESS (0 on failure).
 * @param conv_id_out  [out] created job conversation id on SUCCESS (0 on failure).
 * @param err          [out] short failure reason on FAILURE (caller relays it).
 * @param err_len      size of @p err.
 * @return SUCCESS on spawn; FAILURE otherwise (with @p err set).
 */
int research_spawn_run(int user_id,
                       int64_t parent_conv,
                       const char *brief,
                       const char *deliver_to,
                       int64_t *run_id_out,
                       int64_t *conv_id_out,
                       char *err,
                       size_t err_len);

#ifdef __cplusplus
}
#endif

#endif /* DEEP_RESEARCH_TOOL_H */
