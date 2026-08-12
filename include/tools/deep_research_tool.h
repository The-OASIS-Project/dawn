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

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Register the deep_research tool with the tool registry. */
int deep_research_tool_register(void);

#ifdef __cplusplus
}
#endif

#endif /* DEEP_RESEARCH_TOOL_H */
