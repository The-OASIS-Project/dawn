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
 * qualified_name and file_path with that on-disk location. To keep the
 * filesystem layout out of the LLM's view (security) and to keep stored
 * conversations portable across a directory move, the bridge translates the cbm
 * identifier namespace at the boundary: the LLM only ever sees clean project
 * names ("echo") and project-relative paths.
 *
 * Because every project shares a single source_root, the whole mapping reduces
 * to one prefix string, captured from cbm (not computed — robust to whatever
 * slug rule cbm uses) and cached in the daemon.
 */

#ifndef CODE_PROJECT_NAMEMAP_H
#define CODE_PROJECT_NAMEMAP_H

#include <stddef.h>

/**
 * @brief (Re)capture cbm's graph-name prefix by asking cbm for its project list
 *        and comparing a root_path under source_root to its graph name.
 *
 * Performs a cbm tools/call, so call it from the bridge-init and post-index
 * paths — never while holding the bridge slot mutex. Idempotent; a no-op when
 * cbm has no projects yet (nothing to derive the prefix from).
 */
void code_project_namemap_capture(void);

/**
 * @brief Outbound: translate a clean project name or qualified name the LLM
 *        supplied into cbm's graph namespace (prepend the captured prefix).
 *
 * Writes the translated value to @p out. If no prefix is known yet, or the value
 * already carries it, @p out receives an unchanged copy.
 */
void code_project_namemap_to_graph(const char *clean_value, char *out, size_t out_sz);

/**
 * @brief Inbound: return a malloc'd copy of a cbm result string with the
 *        graph-name prefix and the source_root path prefix stripped, so no slug
 *        or absolute path reaches the LLM.
 *
 * @return malloc'd scrubbed string (caller frees), or NULL on allocation failure
 *         (caller should fall back to the original result).
 */
char *code_project_namemap_scrub(const char *cbm_result);

#endif /* CODE_PROJECT_NAMEMAP_H */
