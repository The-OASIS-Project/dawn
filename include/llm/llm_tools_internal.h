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
 * State and helpers llm_tools.c shares with llm_tools_filter.c.  Not for
 * anything else: the public API is llm_tools.h.
 */

#ifndef LLM_TOOLS_INTERNAL_H
#define LLM_TOOLS_INTERNAL_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "llm/llm_tools.h"

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/** The registered tools (llm_tools_count of them).  Read under
 *  llm_tools_mutex; its entries' flags are written under it.  Which tools it
 *  holds is written only by llm_tools_init / llm_tools_cleanup, at startup
 *  and shutdown, with no other reader running.  Lock order: llm_tools_mutex,
 *  then the tool registry's own (schemas are built from registry lookups). */
extern tool_definition_t llm_tools_table[LLM_TOOLS_MAX_TOOLS];
extern int llm_tools_count;
/** Set once the table is built (llm_tools_init). */
extern bool llm_tools_ready;
extern pthread_mutex_t llm_tools_mutex;
/** Rises when the table's tools change (llm_tools_init / cleanup): with
 *  tool_registry_generation(), what cached schema work is keyed on. */
extern _Atomic uint64_t llm_tools_generation;

/** Free what llm_tools_filter.c caches (llm_tools_cleanup). */
void llm_tools_filter_release(void);

/** @p tool's parameters as a JSON schema (caller puts). */
struct json_object *llm_tools_parameters_schema(const tool_definition_t *tool);

/** @p t's description as the model is sent it (the registry's, whole). */
const char *llm_tools_effective_description(const tool_definition_t *t);

/**
 * @brief Whether @p t may be used (and is advertised) on the current command
 *        context's surface now: enabled, allowed for a local or remote
 *        session, and under the research allowlist and a turn's tool masks
 */
bool llm_tools_enabled_for_session(const tool_definition_t *t, bool is_remote);

#ifdef __cplusplus
}
#endif

#endif /* LLM_TOOLS_INTERNAL_H */
