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
 * What is in force in a conversation whose system prompt is frozen.
 *
 * The frozen prefix message (messages[0]) carries a record of it under
 * LLM_HISTORY_IN_FORCE_KEY (never sent): for each section of the system prompt
 * (prompt_parts.h) a hash of the text in force and its title, the hash each
 * held before its last change, and a hash of the standing directions in
 * force.  A turn compares its prompt against the record, and what differs is
 * appended as a change: only the sections that changed, so an edit to the
 * user's persona appends the persona, not the whole prompt.  The record is
 * structural: nothing appended is ever read back to learn what is in force, so
 * the wording of a change can be revised without re-sending anything.
 */

#ifndef CORE_PREFIX_IN_FORCE_H
#define CORE_PREFIX_IN_FORCE_H

#include <stdbool.h>

#include "core/prompt_parts.h"

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/**
 * @brief A new conversation tag ("dawn-ctx-" and 8 hex digits, the secret
 *        part from the system's random source; LLM_CONTEXT_TAG_MAX)
 */
void prefix_in_force_new_tag(char *out, size_t size);

/**
 * @brief Record @p cp's sections as in force on @p prefix_msg (a history
 *        frozen from them, with @p tag filled in), no standing directions
 *        yet, and the conversation's @p tag
 */
void prefix_in_force_init(struct json_object *prefix_msg,
                          const composed_prompt_t *cp,
                          const char *tag);

/**
 * @brief @p hist's tag, made (and recorded) when it has none: a history
 *        frozen before conversations had one.  Its system prompt doesn't
 *        declare it; the change that does is appended with its next turn's
 *        instructions (the section declaring it is new to it).
 * @return The tag (borrowed from the record), or NULL with no prefix
 */
const char *prefix_in_force_ensure_tag(struct json_object *hist);

/**
 * @brief What of @p cp's system prompt differs from what is in force in
 *        @p hist, as the text of an instruction change; the record updated
 *
 * Only the changed sections, each with its title and new text, and any that
 * no longer apply.  A history whose record has no sections (frozen from a
 * prompt with none: an older save, a surface with no builder) takes a section
 * as in force when its text appears in the frozen prefix as is.
 *
 * A section changed back to what it held before its last change is logged:
 * a setting flapping between turns appends a change every time.
 *
 * Section text is sent with the conversation's tag filled in.
 *
 * @return The change's text (caller frees), or NULL when nothing differs (or
 *         on allocation failure, the record left as it was)
 */
char *prefix_in_force_instructions(struct json_object *hist, const composed_prompt_t *cp);

/**
 * @brief Whether @p directives (the surface's standing directions, "" for
 *        none) differ from those in force in @p hist; when they do, the record
 *        now holds them
 */
bool prefix_in_force_directives_changed(struct json_object *hist, const char *directives);

/**
 * @brief The text a directives change appends: @p directives, or a line saying
 *        none apply when it is empty
 */
const char *prefix_in_force_directives_text(const char *directives);

/**
 * @brief Check the schemas of @p hist's frozen tools against @p schemas (each
 *        registered tool's hash, llm_tools_schema_hashes), and record them
 *
 * A frozen tool whose description or parameters changed, or that is no longer
 * registered, changes what every later request sends: logged once per change.
 */
void prefix_in_force_check_tool_schemas(struct json_object *hist, const char *schemas);

/** The record on @p hist's prefix message as JSON (caller frees), or NULL. */
char *prefix_in_force_json(struct json_object *hist);

#ifdef __cplusplus
}
#endif

#endif /* CORE_PREFIX_IN_FORCE_H */
