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
 * In-loop deep-research tools: research_plan (seed the coverage ledger),
 * research_record (record one evidence claim), and research_conclude (the agent
 * signals the brief is covered).  Reachable ONLY inside a research session
 * (session->research_run_id > 0) via the read-only tool allowlist; hidden
 * everywhere else.  See docs/DEEP_RESEARCH_DESIGN.md §7.
 */

#ifndef RESEARCH_TOOLS_H
#define RESEARCH_TOOLS_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Register research_plan.  @return 0 on success. */
int research_plan_tool_register(void);

/** @brief Register research_record.  @return 0 on success. */
int research_record_tool_register(void);

/** @brief Register research_conclude.  @return 0 on success. */
int research_conclude_tool_register(void);

#ifdef __cplusplus
}
#endif

#endif /* RESEARCH_TOOLS_H */
