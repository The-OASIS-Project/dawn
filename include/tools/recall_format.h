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
 * Recall tool result formatter — renders a focus_compose result into a
 * grouped, source-tagged "here's what we know" block with per-item
 * read-pointers.  Split into its own translation unit so it is unit-testable
 * without the daemon and so the grouping/pointer logic can grow (Phase 2
 * scope filter, entity/relation pointers) without bloating recall_tool.c.
 */

#ifndef TOOLS_RECALL_FORMAT_H
#define TOOLS_RECALL_FORMAT_H

#include "core/focus/focus_source.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Longest text shown for one item (a longer one is cut, with "..."). */
#define RECALL_LINE_TEXT_MAX 240
/** Longest document label a read-pointer carries. */
#define RECALL_FNAME_MAX 300
/**
 * @brief Bytes recall_format_result prints for candidate @p c: its capped
 * line, its read-pointer and the line's bullet and newline.  For
 * focus_limits_t.item_bytes, so recall's budget counts what it shows.  Not
 * counted: the per-call section headers and footer, and the ~30-byte
 * "(already in current context)" note an item already injected this turn
 * gets (not known while the budget is packed).
 */
int recall_format_item_bytes(const focus_candidate_t *c);

/**
 * @brief Render a composed cross-source recall result for the LLM.
 *
 * Groups `result->candidates` (already ranked descending) by source family —
 * memory facts/relationships, conversation summaries, notes & documents,
 * calendar — and renders each as a bulleted line carrying a pointer to where
 * the exact/full text lives (document_read "<label>", memory get <id>, etc.).
 *
 * Dedup-vs-injection (design §4.2a): a candidate whose `item_id` appears in
 * `injected_ids` is marked "already in current context" rather than dropped,
 * so the LLM still sees the linkage but the byte budget favours fresh hits.
 * Pass `injected_ids = NULL` (and `injected_count = 0`) when the caller cannot
 * supply the turn's injected set — the formatter then appends a brief overlap
 * note instead (the v1 fallback).
 *
 * @param query          The user's recall query (echoed in the header).
 * @param result         Composed focus result (candidates + breakdowns).
 * @param injected_ids   Item-ids already injected this turn, or NULL.
 * @param injected_count Length of injected_ids (0 when NULL).
 * @return Newly-allocated string the caller owns and frees, or NULL on OOM.
 */
char *recall_format_result(const char *query,
                           const focus_compose_result_t *result,
                           const char *const *injected_ids,
                           int injected_count);

#ifdef __cplusplus
}
#endif

#endif /* TOOLS_RECALL_FORMAT_H */
