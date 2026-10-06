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
 * An MCP tools/call result as a model reads it: its payload, not the JSON-RPC
 * envelope around it.  Pure: no client or registry state.
 */

#ifndef MCP_RESULT_H
#define MCP_RESULT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/** Longest one-line reference to a link, resource or media part. */
#define MCP_RESULT_REF_MAX 300

/**
 * @brief The text of a tools/call @p result (its parsed "result" object)
 *
 * - structuredContent, when present, is the result: its JSON, compact and
 *   unescaped ("/" as is).  The content parts (the spec's text copy of it)
 *   are dropped.
 * - Otherwise the content parts in order, a line break between parts that
 *   say something:
 *   - text as it is, less C0 controls other than tab, line feed and carriage
 *     return (JSON sent as one text part stays parseable);
 *   - a resource_link, an embedded resource's binary, an image or audio as a
 *     one-line inert reference: never fetched or decoded, control and
 *     invisible characters removed, brackets made parentheses, invalid UTF-8
 *     replaced, length capped;
 *   - an embedded resource's text as text;
 *   - a part of a kind it doesn't know: its JSON, as sent.
 * - No content (absent, not an array, empty): the result's own JSON.
 *
 * @param is_error Set from isError when it is a boolean (the tool's own
 *        failure, reported as data)
 * @return The text (heap, caller frees), or NULL when out of memory
 */
char *mcp_result_text(struct json_object *result, bool *is_error);

#ifdef __cplusplus
}
#endif

#endif /* MCP_RESULT_H */
