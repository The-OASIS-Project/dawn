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
 * The tool loop's view stage: after a batch of tools runs, a result too big
 * for its share of the batch's budget is kept whole (tool_result_store.h) and
 * the model is shown a view of it (llm_tool_view.h) with a handle it can read
 * more through (result_read).  Nothing is thrown away; only the default cost
 * of a big result changes.
 *
 * The budget is in characters, never a tokenizer's guess: the same result
 * gets the same view on any model.  Per batch it follows the window along a
 * curve through two anchors (REF_CHARS at REF_WINDOW tokens, SMALL_CHARS at
 * SMALL_WINDOW), sub-linear so a small window still gets a usable view:
 *
 *    B = min(REF_CHARS x (window / REF_WINDOW)^k,
 *            ROOM x WORST_CHARS_PER_TOKEN), at least FLOOR_CHARS
 *
 * k = ln(REF_CHARS / SMALL_CHARS) / ln(REF_WINDOW / SMALL_WINDOW), the window
 * capped at REF_WINDOW.  ROOM is the tokens the hard threshold leaves after
 * the request as it stands, the assistant's tool-call message and a reply
 * reserve (dense JSON on the densest tokenizer is WORST_CHARS_PER_TOKEN), so
 * a nearly full conversation takes less.  Results under their fair share
 * pass whole; the others share what's left in proportion to their size
 * (water-filling).  A tool that asks to be shown
 * whole up to a size (max_result_chars; an MCP tool's
 * _meta["anthropic/maxResultSizeChars"]) is served first, within the budget.
 *
 * The arithmetic (the budget, the split, the header) is pure
 * (llm_tool_views_plan.c); the stage (llm_tool_views_apply.c) measures the
 * request, views, stores and finishes.
 */

#ifndef LLM_TOOL_VIEWS_H
#define LLM_TOOL_VIEWS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The batch budget's curve (above): its two anchors. */
#define LLM_TOOL_VIEWS_REF_WINDOW 1000000
#define LLM_TOOL_VIEWS_REF_CHARS 48000
#define LLM_TOOL_VIEWS_SMALL_WINDOW 16000
#define LLM_TOOL_VIEWS_SMALL_CHARS 3000
/* The least a batch gets, however full the request: the data stays reachable. */
#define LLM_TOOL_VIEWS_FLOOR_CHARS 1500
/* Characters per token at worst (dense JSON on the densest tokenizer): the
 * room left, in tokens, as characters. */
#define LLM_TOOL_VIEWS_WORST_CHARS_PER_TOKEN 2
/* The reply reserve: the model's max output, at most this. */
#define LLM_TOOL_VIEWS_REPLY_RESERVE_MAX 8192
/* Room kept for a view's header inside its share. */
#define LLM_TOOL_VIEWS_HEADER_MAX 640
/* The most results one batch holds (at least LLM_TOOLS_MAX_PARALLEL_CALLS,
 * asserted where both are in scope). */
#define LLM_TOOL_VIEWS_BATCH_MAX 16
/* The least a result_read answer is built to. */
#define LLM_TOOL_VIEWS_READ_MIN_CHARS 2048

/**
 * @brief The batch budget in characters (above)
 * @param window The model's window in tokens (0 or less: unknown, the
 *        reference window's budget)
 * @param room_tokens The tokens the hard threshold leaves after the request,
 *        the tool-call message and the reply reserve (may be negative)
 * @param room_known Whether @p room_tokens was measured (else no room clamp)
 */
size_t llm_tool_views_budget_chars(int window, int room_tokens, bool room_known);

/**
 * @brief Split @p budget characters across a batch (water-filling)
 *
 * Results marked @p first (their tool asked to be shown whole up to a size
 * they are within: max_result_chars) are served first, smallest first,
 * each whole while it fits in what's left; the rest water-fill what remains.
 *
 * @param demands Each result's size; SIZE_MAX for a result the stage leaves
 *        alone (it takes no share)
 * @param first Which results are served first (NULL: none)
 * @param n Results in the batch (at most LLM_TOOL_VIEWS_BATCH_MAX)
 * @param shares_out Each result's share: its demand when it fits whole,
 *        else what it may use; SIZE_MAX for one left alone.  The shares
 *        that aren't SIZE_MAX sum to at most @p budget, or to the demands
 *        when those do.
 */
void llm_tool_views_split(const size_t *demands,
                          const bool *first,
                          int n,
                          size_t budget,
                          size_t *shares_out);

/**
 * @brief The characters a result_read answer in a batch of @p calls is built
 *        to: its fair share of @p batch_chars, less a header, at least
 *        LLM_TOOL_VIEWS_READ_MIN_CHARS
 */
size_t llm_tool_views_read_chars(size_t batch_chars, int calls);

/** What a view's header says. */
typedef struct {
   const char *tag;     /**< the conversation's tag ("" or NULL: none) */
   size_t chars;        /**< the result's size in characters */
   const char *handle;  /**< its stored handle, or NULL (not stored) */
   bool json;           /**< shown as a JSON view (else lines of text) */
   bool offers_read;    /**< the request offers result_read */
   bool head_tail_only; /**< too big to store whole: its head and tail kept */
   const char *narrow;  /**< a parameter of the tool that narrows a call, or NULL (never
                         *   the tool's own name: an MCP tool's is its server's choice) */
   bool is_read;        /**< result_read's own answer (narrow the read instead) */
} llm_tool_views_header_t;

/**
 * @brief The header framing a view (one bracketed block, ending with a
 *        newline) into @p out
 * @return Its length, or 0 if it doesn't fit in @p size
 */
size_t llm_tool_views_header(const llm_tool_views_header_t *h, char *out, size_t size);

/**
 * @brief Whether parameter @p name narrows a call (a fixed list: limit,
 *        max_results, count, page, offset, query, filter, fields): the header
 *        names only such a parameter, never other schema text
 */
bool llm_tool_views_narrows(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* LLM_TOOL_VIEWS_H */
