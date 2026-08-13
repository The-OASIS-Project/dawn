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
 * Deep-research read-only tool allowlist — the single source of truth for which
 * tool names a research fetch loop may reach (DEEP_RESEARCH_DESIGN.md §7/§11).
 *
 * The allowlist IS the security boundary of a research run, and it is checked in
 * two structurally different executors — the native tool path
 * (is_tool_enabled_for_session in llm_tools.c, at both schema advertisement and
 * execution) and the legacy <command>/direct/no-callback fallback path
 * (command_execute in command_executor.c, the HIGH-1 defense-in-depth close).
 * Duplicating the name list across the two would let the boundary drift; this
 * header keeps it in one place.  Pure leaf (string.h + stdbool.h only) so a
 * Layer-1 executor can include it without a link or layering dependency.
 */

#ifndef RESEARCH_ALLOWLIST_H
#define RESEARCH_ALLOWLIST_H

#include <stdbool.h>
#include <string.h>

/**
 * @brief Is @p name a tool a deep-research fetch loop is allowed to run?
 *
 * The full read-only set: web `search` + `url_fetch` (reads) plus the three
 * ledger/control research tools (`research_plan` / `research_record` /
 * `research_conclude`).  Nothing side-effecting or outward-facing appears here.
 */
static inline bool research_tool_is_allowlisted(const char *name) {
   if (name == NULL) {
      return false;
   }
   return strcmp(name, "search") == 0 || strcmp(name, "url_fetch") == 0 ||
          strcmp(name, "research_plan") == 0 || strcmp(name, "research_record") == 0 ||
          strcmp(name, "research_conclude") == 0;
}

/**
 * @brief Is @p name one of the research-only tools (never visible outside a
 *        research session)?  A strict subset of research_tool_is_allowlisted().
 */
static inline bool research_tool_is_research_only(const char *name) {
   if (name == NULL) {
      return false;
   }
   return strcmp(name, "research_plan") == 0 || strcmp(name, "research_record") == 0 ||
          strcmp(name, "research_conclude") == 0;
}

#endif /* RESEARCH_ALLOWLIST_H */
