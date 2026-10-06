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
 * The prompt builder: a turn's prompt, in parts, on every surface.
 */

#ifndef PROMPT_BUILDER_H
#define PROMPT_BUILDER_H

#include "core/session_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The prompt builder (session_prompt_builder_t): a turn's prompt, in
 *        parts
 *
 * The system prompt a conversation starting now would freeze (in named
 * sections), the surface's standing directions, what DAWN knows about the
 * user, the tool set, and the turn's context.  session_prefix.c applies them
 * to the conversation, append-only.
 *
 * @param session        The session the turn runs on (its surface, its
 *                       conversation, its focus dedup state); may be NULL
 * @param user_id        The turn's user (0: a guest; nothing of any user's)
 * @param user_turn_text The turn's words (for retrieval)
 * @param[out] out       Zero-initialized; the caller frees it
 *                       (composed_prompt_free).  On FAILURE it is empty.
 * @return SUCCESS, or FAILURE on allocation failure
 */
int dawn_build_prompt(session_t *session,
                      int user_id,
                      const char *user_turn_text,
                      composed_prompt_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PROMPT_BUILDER_H */
