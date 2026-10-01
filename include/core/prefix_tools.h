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
 * A conversation's tools at a turn seam (llm_tool_defs.h): frozen by value on
 * its first turn, and what changed since appended as one tool_change message.
 *
 * Compared once per seam, after a compaction applied, against what the
 * history still shows (the frozen definitions and every later change in it),
 * so a change a compaction summarized away is appended again, once.  What is
 * compared is what is registered (llm_tools_definitions), never enable flags:
 * a tool toggled, a component coming online or going offline appends nothing
 * (whether a surface may use a tool is decided where a request is advertised
 * and a call executed).  A tool no longer registered appends nothing either:
 * its definition stays, and a call to it is refused.  Changes are bounded per
 * conversation and per MCP server per hour; past a bound the definitions in
 * force stay (logged once), and what was held back is compared again at a
 * later seam.  What a compaction removed is appended again, unbounded.
 */

#ifndef CORE_PREFIX_TOOLS_H
#define CORE_PREFIX_TOOLS_H

#include <stdbool.h>
#include <stdint.h>

#include "core/prompt_parts.h"

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/** Tool changes a conversation appends in all. */
#define PREFIX_TOOL_CHANGES_MAX 32
/** Changes one MCP server (or DAWN's own tools) may append to a conversation
 *  per hour. */
#define PREFIX_TOOL_CHANGES_PER_SERVER_HOUR 4

/** What a seam did to a conversation's tools. */
typedef struct {
   bool bind;     /**< The frozen set was made or converted: the turn saves it */
   bool boundary; /**< The turn left its earlier reasoning behind */
   /** The tool_change message it appended (borrowed: @p hist holds it), or NULL */
   struct json_object *appended;
} prefix_tools_result_t;

/**
 * @brief Freeze, convert or extend @p hist's tools for this turn
 *
 * Caller holds the history's lock; @p hist is frozen.  With no frozen set,
 * the tools registered now (@p cp->tool_defs) are frozen.  A set an older build froze by name is
 * converted to definitions (whole when the schema hashes it recorded still
 * match; else a declared boundary, a name it never sent left out).  Then one
 * tool_change message is appended holding: what @p removed (the changes a
 * compaction just summarized away) had in force that the history no longer
 * does, from those rows (never the registry: a tool whose server is gone
 * keeps its definition; exempt from the bounds); and what is registered that
 * differs from what is in force (bounded).  It is stored inline when
 * @p cp->inline_tools, the turn has a question, and the conversation's inline
 * changes weren't rejected; else folded, a declared boundary.
 *
 * When this turn's target doesn't take changes in place (!@p cp->inline_tools)
 * and the conversation has inline ones, it is marked (folded from now on) and
 * a boundary declared, once: a stored inline change is sent in place or not
 * by the conversation's record, never by what the process knows now.
 *
 * What is in force is kept on the record by hash, and a seam whose registered
 * set (@p cp->tool_defs_fp) is the one whose changes are all in force, with
 * nothing compacted, compares nothing.
 *
 * @param has_question Whether the turn has a question (the change follows it)
 * @param removed The definitions of the tool changes a compaction applied at
 *                this seam removed, merged by name in order (borrowed); NULL
 *                for none.  Applied with any @p cp, NULL included.
 */
void prefix_tools_apply(struct json_object *hist,
                        const composed_prompt_t *cp,
                        bool has_question,
                        struct json_object *removed,
                        uint32_t session_id,
                        prefix_tools_result_t *out);

/** Seams that compared a conversation's tools, since start (a diagnostic: an
 *  unchanged turn compares nothing). */
uint64_t prefix_tools_diffs(void);

/**
 * @brief Record on @p hist that its inline tool changes were rejected
 *        (llm_tool_defs_inline_rejected).  Caller holds the history's lock.
 * @return true when this changed the record
 */
bool prefix_tools_mark_rejected(struct json_object *hist);

#ifdef __cplusplus
}
#endif

#endif /* CORE_PREFIX_TOOLS_H */
