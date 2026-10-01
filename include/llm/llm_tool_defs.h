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
 * A conversation's tools, by value.
 *
 * A tool definition is neutral: {"name", "description", "parameters"} (a JSON
 * schema object), the registry's projection of a tool (llm_tools_definitions),
 * rendered per provider when a request is built (llm_tools_render_frozen).
 * A conversation freezes the definitions it starts with on its prefix message
 * (LLM_HISTORY_TOOLS_KEY), and a later change is a tool_change message
 * (MESSAGE_KIND_TOOL_CHANGE, role system) holding the full definitions of the
 * tools it adds or changes:
 *
 *   {"rendered": "inline" | "folded", "tools": [definition, ...]}
 *
 * "inline": sent in place as tool_addition blocks (Claude API, beta
 * inline-tools-2026-09-15), the `tools` array untouched.  "folded": merged into
 * the request's `tools` (a same-name definition replaces in place, a new one is
 * appended); the turn that appended it declared a boundary.  Whether a stored
 * inline change is sent in place is the conversation's, never the process's:
 * its row, its record's rejected flag (LLM_TOOL_DEFS_REJECTED_KEY, set with a
 * declared boundary when its target stops taking them) and where it sits.
 * Nothing here is read back to learn what changed: what is in force is the
 * frozen set plus every change still in the history.
 *
 * Pure (JSON only): shared by the session seam and every formatter.
 */

#ifndef LLM_TOOL_DEFS_H
#define LLM_TOOL_DEFS_H

#include <stdbool.h>
#include <stddef.h>

#include "core/hash_util.h"

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/** Longest tool name a definition may have. */
#define LLM_TOOL_DEF_NAME_MAX 64
/** Longest description a definition may have, in bytes. */
#define LLM_TOOL_DEF_DESC_MAX 16384
/** Longest parameters schema a definition may have, serialized, in bytes. */
#define LLM_TOOL_DEF_PARAMS_MAX 65536

/** The in-force record key (prefix_in_force.h) saying the conversation's
 *  inline tool changes were rejected: every one of them folds from then on. */
#define LLM_TOOL_DEFS_REJECTED_KEY "inline_tools_rejected"

/**
 * @brief Whether @p def is a definition a request may carry: a name of
 *        letters, digits, '_' or '-', a description and a parameters object
 *        within their caps, all valid UTF-8
 */
bool llm_tool_def_valid(struct json_object *def);

/** @p def's name (a definition, or a name: an older conversation's set), or NULL. */
const char *llm_tool_def_name(struct json_object *def);

/**
 * @brief The hash of @p def's canonical form (its keys sorted at every level),
 *        so two definitions compare equal whatever order their keys came in
 * @return false (and @p out empty) when out of memory: no hash, not another one
 */
bool llm_tool_def_hash(struct json_object *def, char out[DAWN_SHA256_HEX_LEN]);

/**
 * @brief @p def rendered for a request: Claude's shape ({name, description,
 *        input_schema}) when @p claude, else OpenAI's function shape.  The
 *        parameters are @p def's own (a reference): a request writes nothing
 *        into them, its breakpoint going on the outer object.
 * @return A new object (caller puts), or NULL
 */
struct json_object *llm_tool_def_render(struct json_object *def, bool claude);

/**
 * @brief The MCP server @p def came from (its description's untrusted-text
 *        header names it), or "" for one of DAWN's own tools
 */
void llm_tool_def_server(struct json_object *def, char *out, size_t size);

/**
 * @brief A tool_change message holding @p defs (taken), stored @p rendered_inline
 * @return The message (caller owns), or NULL
 */
struct json_object *llm_tool_change_new(struct json_object *defs, bool rendered_inline);

/**
 * @brief The definitions a tool_change message holds, as stored (validated
 *        when made, or when loaded: llm_tool_change_normalize)
 * @return A new reference (caller puts), or NULL when @p msg holds none
 */
struct json_object *llm_tool_change_defs(struct json_object *msg);

/**
 * @brief Validate a loaded tool_change message's definitions once: one that
 *        fails is left out (logged), the message's text rewritten without it
 * @return Whether the message still holds a definition
 */
bool llm_tool_change_normalize(struct json_object *msg);

/** Whether the tool_change message @p msg was stored to be sent in place. */
bool llm_tool_change_stored_inline(struct json_object *msg);

/** Whether @p history's inline tool changes were rejected (its record says so). */
bool llm_tool_defs_inline_rejected(struct json_object *history);

/**
 * @brief Whether the tool_change message at @p idx of @p history is sent in
 *        place: stored inline, @p inline_ok (the request may carry the beta:
 *        the Claude API itself, the beta not rejected on this turn),
 *        the conversation's inline changes not rejected, and where a
 *        mid-conversation system message may sit (after a user turn or tool
 *        results; followed by an assistant turn or nothing)
 */
bool llm_tool_change_renders_inline(struct json_object *history, size_t idx, bool inline_ok);

/**
 * @brief The definitions a request's `tools` carries: the frozen set, then
 *        every tool_change not sent in place (llm_tool_change_renders_inline),
 *        a same-name definition replacing in place and a new one appended
 *
 * An older conversation's set holds names; they stay names (the formatter
 * renders a name from the registry).
 *
 * @param inline_ok Whether the request can send changes in place
 * @return A new array (caller puts), or NULL when @p history has no frozen set
 */
struct json_object *llm_tool_defs_for_request(struct json_object *history, bool inline_ok);

/** Merge @p defs into @p into: a same-name definition replaces in place, a new
 *  one is appended (each a new reference). */
void llm_tool_defs_merge(struct json_object *into, struct json_object *defs);

/**
 * @brief @p defs (a frozen set: definitions, or an older set's names) with
 *        each definition validated, once, where a set is frozen or loaded; one
 *        that fails is left out (logged)
 * @return A new array (caller puts), or NULL
 */
struct json_object *llm_tool_defs_usable(struct json_object *defs);

/**
 * @brief Each of @p defs' canonical hashes by name ({"name": hash}, in their
 *        order), and in @p fp (may be NULL) the hash of that object: the set's
 *        fingerprint
 * @return A new object (caller puts), or NULL (out of memory, or a name)
 */
struct json_object *llm_tool_defs_hashes(struct json_object *defs, char fp[DAWN_SHA256_HEX_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* LLM_TOOL_DEFS_H */
