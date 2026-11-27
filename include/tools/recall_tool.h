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
 * Unified cross-source recall tool — one high-level "gather everything known
 * about X" call that fans out across all focus sources (memory facts/entities/
 * relations/summaries, notes, documents, calendar) via the focus engine at a
 * larger budget than the per-turn injection.  See
 * atlas/dawn/memory/DYNAMIC_CONTEXT_INJECTION.md ("Explicit recall tool").
 */

#ifndef TOOLS_RECALL_TOOL_H
#define TOOLS_RECALL_TOOL_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the `recall` tool with the tool registry.
 * @return SUCCESS or FAILURE (per tool_registry_register).
 */
int recall_tool_register(void);

#ifdef __cplusplus
}
#endif

#endif /* TOOLS_RECALL_TOOL_H */
