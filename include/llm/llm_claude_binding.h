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
 * Anthropic thinking binding controls: what the API reports it dropped from a
 * request's history.  A replayed thinking block is bound to the exact prefix it
 * was produced under; with the thinking-binding-controls beta, a mismatched
 * block is dropped rather than failing the request, and each drop is named in
 * the response's input_transformations array.
 */

#ifndef LLM_CLAUDE_BINDING_H
#define LLM_CLAUDE_BINDING_H

#include <json-c/json.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The drops one response reported (its input_transformations)
 *
 * A prefix drop means DAWN changed history the block was bound to: a harness
 * bug.  A model drop means the conversation switched to a model that can't read
 * the block: expected, and not billed.
 */
typedef struct llm_claude_drops {
   bool reported;       /**< The response carried the list (the beta was on) */
   int prefix_drops;    /**< reason prefix_binding_mismatch */
   int model_drops;     /**< reason model_binding_mismatch */
   int other_drops;     /**< An entry this build doesn't classify */
   char first_path[48]; /**< Path of the first prefix drop ("" if none) */
} llm_claude_drops_t;

/**
 * @brief Read an input_transformations array into a drop report
 *
 * Replaces *out.  A NULL or non-array value leaves it unreported and empty.
 * Entries of an unknown type or reason are counted as other_drops, since the
 * API adds values over time.
 *
 * @param transformations The response's input_transformations value (may be NULL)
 * @param out Report to fill
 */
void llm_claude_drops_parse(struct json_object *transformations, llm_claude_drops_t *out);

/**
 * @brief Read the input_transformations of a response message object
 *
 * The list sits on the message: the non-streaming body, or message_start's
 * message (and, after a mid-stream server-side fallback, the final
 * message_delta, which then replaces the earlier report).  Leaves *out alone
 * when the object carries no list.
 *
 * @param message A message object or event (may be NULL)
 * @param out Report to fill
 */
void llm_claude_drops_from_message(struct json_object *message, llm_claude_drops_t *out);

#ifdef __cplusplus
}
#endif

#endif /* LLM_CLAUDE_BINDING_H */
