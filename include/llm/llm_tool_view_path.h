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
 * JSON paths as views write them and readers take them back: "$" then
 * ".name" (a key of letters, digits and _, at most LLM_TOOL_VIEW_KEY_MAX),
 * ["key"] (any other key, as a view shows it, as a JSON string: \", \\ and
 * control characters escaped, so it stays one line; a reader decodes JSON
 * string escapes), [i] (a
 * reader takes a negative i from the end) and, last, [i:j] (either end
 * optional).  One module, so a path copied from a view always parses.
 */

#ifndef LLM_TOOL_VIEW_PATH_H
#define LLM_TOOL_VIEW_PATH_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A path longer than this is shown cut ("..." at its end). */
#define LLM_TOOL_VIEW_PATH_MAX 512
#define LLM_TOOL_VIEW_PATH_SEGMENTS 32

/** A path being written: segments are added whole or not at all (a cut path
 *  stays valid UTF-8). */
typedef struct {
   char s[LLM_TOOL_VIEW_PATH_MAX + 4];
   size_t len;
   bool cut;
} llm_tool_view_path_t;

void llm_tool_view_path_start(llm_tool_view_path_t *p, const char *prefix);
void llm_tool_view_path_put(llm_tool_view_path_t *p, const char *s, size_t n);

/** Whether key @p k (@p n bytes) is written ".name". */
bool llm_tool_view_path_identifier(const char *k, size_t n);

/**
 * @brief @p p with key @p key (@p kl bytes) appended: ".key", or ["key"] as
 *        shown (@p shown: its cut form when a view cut it, else NULL)
 */
void llm_tool_view_path_key(llm_tool_view_path_t *p, const char *key, size_t kl, const char *shown);

void llm_tool_view_path_index(llm_tool_view_path_t *p, size_t i);

typedef enum {
   LLM_TOOL_VIEW_PATH_KEY,
   LLM_TOOL_VIEW_PATH_INDEX,
   LLM_TOOL_VIEW_PATH_SLICE,
} llm_tool_view_path_kind_t;

/** One part of a parsed path. */
typedef struct {
   llm_tool_view_path_kind_t kind;
   const char *key; /* KEY: decoded, NUL-terminated (in the expression's key space) */
   bool quoted;     /* written ["key"] */
   long long a;
   long long b;
   bool has_a;
   bool has_b;
} llm_tool_view_path_seg_t;

/** A parsed path. */
typedef struct {
   llm_tool_view_path_seg_t seg[LLM_TOOL_VIEW_PATH_SEGMENTS];
   int n;
   char keys[LLM_TOOL_VIEW_PATH_MAX + LLM_TOOL_VIEW_PATH_SEGMENTS];
   size_t keys_len;
} llm_tool_view_path_expr_t;

/**
 * @brief Parse @p path (NULL: "$") into @p e
 * @return NULL, or why it can't be read (a static message)
 */
const char *llm_tool_view_path_parse(const char *path, llm_tool_view_path_expr_t *e);

#ifdef __cplusplus
}
#endif

#endif /* LLM_TOOL_VIEW_PATH_H */
