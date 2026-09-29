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
 * * A conversation's frozen prefix message: messages[0] of a history whose
 * system prompt is frozen (MESSAGE_KIND_PREFIX), carrying its tool set
 * (LLM_HISTORY_TOOLS_KEY) and what is in force (LLM_HISTORY_IN_FORCE_KEY).
 * The one place that makes one: a turn freezing a new prompt
 * (session_prefix.c) and a load installing the stored one alike.
 */

#ifndef CORE_PREFIX_MESSAGE_H
#define CORE_PREFIX_MESSAGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/**
 * @brief A prefix message of @p text, with the tool set @p tools_json (a
 *        JSON array of names, or NULL) and the in-force record
 *        @p in_force_json (or NULL) when they parse
 * @return New message (caller owns), or NULL on allocation failure
 */
struct json_object *prefix_message_new(const char *text,
                                       const char *tools_json,
                                       const char *in_force_json);

/**
 * @brief The prefix message conversation @p conv_id stores (what every
 *        request of it was sent), or NULL when it stores none yet or the
 *        stored one is unusable (logged; its next turn freezes a new one, a
 *        declared boundary)
 */
struct json_object *prefix_message_stored(int64_t conv_id, int user_id);

/**
 * @brief Put @p prefix (taken) first in @p history, in place of any system
 *        message of no kind the history leads with
 *
 * @return false on allocation failure (@p prefix released, history unchanged)
 */
bool prefix_message_install(struct json_object *history, struct json_object *prefix);

#ifdef __cplusplus
}
#endif

#endif /* CORE_PREFIX_MESSAGE_H */
