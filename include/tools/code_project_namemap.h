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
 * Code-project name-translation boundary for the cbm MCP bridge.
 *
 * cbm names every project by slugifying its absolute repo path
 * (/var/lib/dawn/source/echo -> "var-lib-dawn-source-echo") and prefixes every
 * qualified_name with that slug. To keep the filesystem layout out of the LLM's
 * view (security) and to keep stored conversations portable across a directory
 * move, the bridge translates the cbm identifier namespace at the boundary: the
 * LLM only ever sees clean DAWN project names ("echo") and project-relative paths.
 *
 * The mapping is per-project (DAWN clean name <-> cbm graph slug + repo path),
 * captured from cbm's project list intersected with DAWN's own rows — so it
 * supports multiple projects at arbitrary roots (clones AND linked local repos),
 * not just one shared source_root. Captured from cbm (not computed — robust to
 * whatever slug rule cbm uses) and cached in the daemon.
 *
 * Lock/dependency invariants: only capture() touches code_project_db (to snapshot
 * rows; the DB lock is released before the cbm call). to_graph()/scrub() are
 * DB-free and run on the LLM hot path. Never hold a lock across a cbm call.
 */

#ifndef CODE_PROJECT_NAMEMAP_H
#define CODE_PROJECT_NAMEMAP_H

#include <stddef.h>

/**
 * @brief (Re)capture the per-project name map by asking cbm for its project list
 *        and intersecting it with DAWN's rows (matched by root_path == local_path).
 *
 * Performs a cbm tools/call, so call it from the bridge-init and post-index/
 * mutation paths — never while holding the bridge slot mutex. Idempotent; clears
 * the map when DAWN has no projects.
 */
void code_project_namemap_capture(void);

/**
 * @brief Outbound: translate a clean project name or qualified_name the LLM
 *        supplied into cbm's graph namespace.
 *
 * The leading token (before the first '.') is mapped clean->slug via the captured
 * map; the remainder is passed through. If the project is unknown, @p out receives
 * an unchanged copy (and a warning is logged) so cbm 404s visibly.
 */
void code_project_namemap_to_graph(const char *clean_value, char *out, size_t out_sz);

/**
 * @brief Inbound: return a malloc'd copy of a cbm result with every known graph
 *        slug mapped back to its clean name and every absolute repo path stripped,
 *        so no slug or filesystem layout reaches the LLM.
 *
 * @return malloc'd scrubbed string (caller frees), or NULL on allocation failure
 *         (caller should fall back to the original result).
 */
char *code_project_namemap_scrub(const char *cbm_result);

#endif /* CODE_PROJECT_NAMEMAP_H */
