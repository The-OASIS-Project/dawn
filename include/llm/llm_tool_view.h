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
 * A view of a tool result too big to send whole: what it holds, in a budget,
 * with every omission marked where it is.  Pure: text (or a parsed tree) in,
 * view out.
 *
 * JSON keeps its shape: every object's first keys, every array's first items
 * and its last, strings' heads, each elision a marker naming the path of what
 * it left out (and for arrays the shape of the items).  The view is valid
 * JSON.  Numbers are as sent (only an integer -0 shows as 0): text whose
 * numbers json-c can't hold exactly (an integer past 64 bits, a double past
 * its range, "1.") or that isn't strict JSON (NaN, Infinity, single quotes,
 * comments) gets the text view.  A key longer than LLM_TOOL_VIEW_KEY_MAX
 * shows its head, its length and a hash of it; the "more keys" marker's key
 * never repeats a real one.  The shape starts at a middle size; a budget it
 * overflows tightens it, and one it leaves more than half unused loosens it,
 * a dimension that cut something at a time, while it fits (up to the _MAX
 * limits).  An elision is made only when what it hides outweighs its marker.
 * When even the barest skeleton won't fit, the text view is used, which
 * always fits.
 *
 * Text keeps its head and tail as numbered lines ("cat -n"), about two
 * thirds head (the tail gets what the head leaves), with what was left out
 * between them; a line too long to fit is cut by characters.  A NUL byte is
 * shown as a space (a view is a C string).
 *
 * Deterministic: the same input and budget give the same bytes.
 */

#ifndef LLM_TOOL_VIEW_H
#define LLM_TOOL_VIEW_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/* The largest text parsed as JSON for a view: past it, parsing costs more than
 * a structure view is worth (a text view is used). */
#define LLM_TOOL_VIEW_JSON_MAX_BYTES ((size_t)4 * 1024 * 1024)

/* The smallest budget a view is made in (smaller budgets are raised to it). */
#define LLM_TOOL_VIEW_MIN_BUDGET 256

/* The JSON view's starting and barest shapes (the tightening ladder runs from
 * one to the other: strings, then items, then keys, then depth). */
#define LLM_TOOL_VIEW_ITEMS 5
#define LLM_TOOL_VIEW_ITEMS_MIN 1
#define LLM_TOOL_VIEW_STRING 400
#define LLM_TOOL_VIEW_STRING_MIN 40
#define LLM_TOOL_VIEW_KEYS 20
#define LLM_TOOL_VIEW_KEYS_MIN 8
#define LLM_TOOL_VIEW_DEPTH 16
#define LLM_TOOL_VIEW_DEPTH_MIN 3
#define LLM_TOOL_VIEW_ITEMS_MAX 4096 /* a roomy budget loosens the shape up to these */
#define LLM_TOOL_VIEW_STRING_MAX 16000
#define LLM_TOOL_VIEW_DEPTH_MAX 32 /* json-c's own nesting limit for parsed text */
#define LLM_TOOL_VIEW_KEYS_MAX 1024
#define LLM_TOOL_VIEW_ELIDE_MIN 4 /* fewer items (or keys) than this are shown, not elided */
#define LLM_TOOL_VIEW_CUT_MIN 48  /* a string is cut only when this much past its head */
#define LLM_TOOL_VIEW_KEY_MAX 64  /* a key longer than this is shown cut */
#define LLM_TOOL_VIEW_KEY_SHOWN_MAX (LLM_TOOL_VIEW_KEY_MAX + 48) /* a cut key as shown */
#define LLM_TOOL_VIEW_SHAPE_SAMPLES 32 /* items sampled for an array's shape */

typedef enum {
   LLM_TOOL_VIEW_TEXT = 0,
   LLM_TOOL_VIEW_JSON = 1,
} llm_tool_view_mode_t;

/** What a view was made from, and how. */
typedef struct {
   llm_tool_view_mode_t mode;
   bool shortened; /**< false: the input fit and is returned whole */
   size_t bytes;   /**< the input's size */
   size_t lines;   /**< the input's lines (text) */
   size_t view_bytes;
} llm_tool_view_info_t;

/**
 * @brief @p text (@p len bytes) in at most @p budget bytes (at least
 *        LLM_TOOL_VIEW_MIN_BUDGET): whole when it fits (a NUL shown as a
 *        space), else a view
 *
 * JSON (a text whose first non-space byte is '{' or '[', at most
 * LLM_TOOL_VIEW_JSON_MAX_BYTES, that parses and whose numbers json-c holds
 * exactly) gets the JSON view; anything else, or JSON whose barest skeleton
 * won't fit, the text view.
 *
 * @param path_prefix The path its root has ("$" when NULL): a view of part of
 *        a result names paths from the whole result's root
 * @param info Filled in (may be NULL)
 * @return The view (heap, caller frees), or NULL when out of memory
 */
char *llm_tool_view(const char *text,
                    size_t len,
                    size_t budget,
                    const char *path_prefix,
                    llm_tool_view_info_t *info);

/**
 * @brief llm_tool_view(), handing back the tree it parsed when it made the
 *        JSON view (*@p tree_out; the caller releases it with
 *        json_object_put()), else NULL: so a reader of the stored result
 *        needn't parse it again
 */
char *llm_tool_view_ex(const char *text,
                       size_t len,
                       size_t budget,
                       const char *path_prefix,
                       llm_tool_view_info_t *info,
                       struct json_object **tree_out);

/**
 * @brief A parsed JSON value @p root in at most @p budget bytes: whole (its
 *        compact JSON) when it renders uncut in the budget, else the JSON
 *        view, else (when even the skeleton won't fit) the text view of its
 *        JSON
 *
 * Its numbers are as the tree holds them; a NaN or infinity is shown as null.
 * info->lines is 0; info->bytes is the whole JSON's size when returned whole,
 * else 0 (not measured: the tree is never serialized to size it).
 */
char *llm_tool_view_tree(struct json_object *root,
                         size_t budget,
                         const char *path_prefix,
                         llm_tool_view_info_t *info);

/**
 * @brief Items of a larger array (@p items: a new array whose item i is the
 *        original's item @p first_index + i) viewed as llm_tool_view_tree
 *        does, their paths and markers naming the original indices
 *        (@p path_prefix is the original array's path)
 */
char *llm_tool_view_slice(struct json_object *items,
                          size_t first_index,
                          size_t budget,
                          const char *path_prefix,
                          llm_tool_view_info_t *info);

/**
 * @brief Lines of a text as the text view shows them, numbered from
 *        @p first_line (the slice's first line's number in the whole text):
 *        whole when they fit, else head and tail with what's between counted
 */
char *llm_tool_view_lines(const char *text,
                          size_t len,
                          size_t budget,
                          size_t first_line,
                          llm_tool_view_info_t *info);

/**
 * @brief @p text parsed as the JSON view parses it: one strict JSON value (a
 *        JSON text: '{' or '[' first, at most LLM_TOOL_VIEW_JSON_MAX_BYTES)
 *        whose numbers json-c holds exactly, or NULL
 *
 * A reader of a stored result parses it this way, so it never shows a value
 * json-c changed.  Caller releases with json_object_put().
 */
struct json_object *llm_tool_view_parse(const char *text, size_t len);

/**
 * @brief How a view shows key @p key when it is cut (its head, length and
 *        hash, in @p out), or NULL when it is shown whole
 */
const char *llm_tool_view_key_shown(const char *key, char out[LLM_TOOL_VIEW_KEY_SHOWN_MAX]);

/**
 * @brief Whether a view shows key @p key as @p shown (whole, or, past
 *        LLM_TOOL_VIEW_KEY_MAX, cut with its length and hash): how a reader
 *        resolves a path segment copied from a view
 */
bool llm_tool_view_key_shown_as(const char *key, const char *shown);

#define LLM_TOOL_VIEW_SCALAR_MAX 32

/**
 * @brief A scalar (or null) @p v as JSON text, as its tree holds it: a double
 *        json-c parsed its source text, a NaN or infinity null; the text is
 *        @p buf's or the tree's own (valid while @p v is)
 *
 * Unlike json_object_to_json_string_ext() it leaves no printbuf on @p v, so
 * a cached tree doesn't grow as it's read.
 */
const char *llm_tool_view_scalar(struct json_object *v, char buf[LLM_TOOL_VIEW_SCALAR_MAX]);

#ifdef __cplusplus
}
#endif

#endif /* LLM_TOOL_VIEW_H */
