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
 * MCP JSON Schema -> DAWN treg_param_t translator, with hardening caps.
 */

#ifndef MCP_BRIDGE_SCHEMA_H
#define MCP_BRIDGE_SCHEMA_H

#include "tools/tool_registry.h"

struct json_object; /* forward decl: keep json-c out of this header */

/**
 * @brief A translated parameter set. Owns its params array and every string
 *        the treg_param_t entries point into. Free with mcp_param_set_free().
 */
typedef struct {
   treg_param_t *params;
   int param_count;
} mcp_param_set_t;

/**
 * @brief Translate an MCP tool inputSchema into DAWN parameters.
 *
 * Maps JSON Schema property types to treg_param_t. Hardening (sec-H1):
 *  - reject the tool if it declares more than TOOL_PARAM_MAX properties;
 *  - reject if any property's `enum` exceeds TOOL_PARAM_ENUM_MAX values;
 *  - reject if a `$ref` appears anywhere in a property schema;
 *  - reject if `oneOf`/`anyOf` nest deeper than 2 levels;
 *  - arrays and objects/oneOf/anyOf collapse to an opaque JSON-passthrough
 *    string parameter (type STRING, unit "json") with a logged warning.
 *
 * A NULL/empty/propertyless schema yields zero params (a no-argument tool).
 *
 * @param input_schema The tool's inputSchema JSON object (may be NULL).
 * @param tool_name     Tool name, for log context.
 * @param out           Filled on SUCCESS (caller frees with mcp_param_set_free).
 * @return SUCCESS, or FAILURE if the tool must be rejected.
 */
int mcp_schema_translate(struct json_object *input_schema,
                         const char *tool_name,
                         mcp_param_set_t *out);

/** @brief Free a translated parameter set (safe on a zero-initialized set). */
void mcp_param_set_free(mcp_param_set_t *out);

/**
 * @brief Wrap an untrusted MCP tool description for safe LLM exposure (sec-H2).
 *
 * Wraps the (sanitized) text in
 *   [BEGIN UNTRUSTED MCP TOOL DESCRIPTION from server '<alias>'] ... [END ...]
 * markers, strips ASCII control chars (except \n/\t) plus zero-width and bidi
 * Unicode sequences, and truncates the whole result to TOOL_DESC_MAX.
 *
 * @return malloc'd string (caller frees), or NULL on allocation failure.
 */
char *mcp_schema_wrap_description(const char *server_alias, const char *raw_description);

#endif /* MCP_BRIDGE_SCHEMA_H */
