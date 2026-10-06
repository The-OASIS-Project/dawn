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
 * What result_read does with a stored result: pure, over its text or its
 * parsed tree.  Every answer fits the budget it's given (a larger part is
 * shown as a view); a bad request is answered with TOOL_RESULT_ERROR_MARK
 * and what went wrong.
 *
 * Paths are those a view names: "$" then ".name", ["key"] (JSON string
 * escapes; a key a view showed cut is matched by its cut form), [i] (a
 * negative i counts from the end) and, last, [i:j] (either end optional).
 */

#ifndef RESULT_READ_OPS_H
#define RESULT_READ_OPS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

#define RESULT_READ_PATH_MAX 512 /* LLM_TOOL_VIEW_PATH_MAX: the longest path taken */
#define RESULT_READ_PATTERN_MAX 200
#define RESULT_READ_CONTEXT_MAX 3
#define RESULT_READ_MATCHES_MAX 50
#define RESULT_READ_DISTINCT_MAX 50
#define RESULT_READ_VALUE_MAX 200 /* a value or match window is shown this many bytes */
#define RESULT_READ_DISTINCT_VALUE_BYTES \
   65536                           /* an object or array value is compared whole up to this */
#define RESULT_READ_LINE_WHOLE 400 /* a matching line up to this long is shown whole */

/**
 * @brief Lines @p from to @p to (1-based, inclusive; clamped, and swapped when
 *        reversed; 0 for either: the start or the end) of @p text, numbered
 */
char *result_read_lines(const char *text, size_t len, long from, long to, size_t budget);

/** @brief The value at @p path in @p root, viewed with its path. */
char *result_read_path(struct json_object *root, const char *path, size_t budget);

/** @brief How many items, keys or characters the value at @p path has. */
char *result_read_count(struct json_object *root, const char *path);

/**
 * @brief The values of @p field in the objects of the array at @p path, with
 *        how many items have each (most first; the first
 *        RESULT_READ_DISTINCT_MAX)
 */
char *result_read_distinct(struct json_object *root,
                           const char *path,
                           const char *field,
                           size_t budget);

/**
 * @brief Where @p pattern (literal, ASCII case-insensitive) is in @p root's
 *        keys and values: one line per match, its path and the value around it
 */
char *result_read_grep_tree(struct json_object *root, const char *pattern, size_t budget);

/**
 * @brief The lines of @p text holding @p pattern (literal, ASCII
 *        case-insensitive), numbered, with @p context lines around each
 */
char *result_read_grep_text(const char *text,
                            size_t len,
                            const char *pattern,
                            int context,
                            size_t budget);

#ifdef __cplusplus
}
#endif

#endif /* RESULT_READ_OPS_H */
