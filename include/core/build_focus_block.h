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
 * Per-turn focus builder: the items retrieved for a turn, ranked and
 * numbered for the conversation.  Which of them the turn sends is decided at
 * its seam (core/session_focus.h), against what the conversation already
 * shows.
 *
 * Sole consumer: dawn_build_prompt (core/prompt_builder.c), once per turn.
 * Layer 2 (with the prompt builder) — pulls in the L2 focus framework via
 * core/focus/focus_source.h and the L2 embedding engine via
 * memory/memory_embeddings.h.
 */

#ifndef CORE_BUILD_FOCUS_BLOCK_H
#define CORE_BUILD_FOCUS_BLOCK_H

#include <stdint.h>

#include "core/prompt_parts.h"

#ifdef __cplusplus
extern "C" {
#endif

struct session;

/**
 * @brief Retrieve the turn's items into @p out (focus_items, focus_panel).
 *
 * Pipeline (when enabled):
 *   1. Embed `user_turn_text` (with the previous question, for a short
 *      follow-up) via memory_embeddings_embed.
 *   2. focus_compose for the top_k most relevant items (no over-fetch).
 *   3. Give each its handle for the conversation (focus_handles_assign),
 *      defuse DAWN's markers in its text and make it one line.
 *   4. Keep the ranked result as the context panel's (out->focus_panel):
 *      the seam tells the panel each item's place once it has decided it
 *      (session_focus_client_notice; the WebUI shows it).
 *
 * Leaves @p out's focus fields empty, returning SUCCESS, when:
 *   - Feature gate (`config->memory.focus_injection.enabled`) is off
 *   - `user_turn_text` is NULL or empty
 *   - User is unauthenticated (`user_id <= 0`)
 * A retrieval that finds nothing still sets the panel (it shows "looked,
 * found nothing").  Returns FAILURE on hard errors (out-of-memory,
 * focus_compose FAILURE), with @p out's focus fields empty.
 *
 * Logging: one OLOG_INFO per retrieval (candidate and rejection counts,
 * elapsed time; never item text), and an OLOG_WARNING per source with
 * filter rejections.
 *
 * @param session        The session the turn runs on: the conversation's item
 *                       handles ([M7]); NULL for none (items numbered per turn)
 * @param user_id        Authenticated user (must be > 0)
 * @param conv_id        The turn's conversation (0: none yet)
 * @param turn_id        DB id of the user message that triggered this
 *                       prompt (the panel's turn); 0 when not known
 * @param user_turn_text Raw user message text
 * @param[in,out] out    The turn's prompt; its focus fields are set, and
 *                       freed with it (composed_prompt_free)
 * @return SUCCESS or FAILURE.  See contract above.
 */
int build_focus_block(struct session *session,
                      int user_id,
                      int64_t conv_id,
                      int64_t turn_id,
                      const char *user_turn_text,
                      composed_prompt_t *out);

#ifdef __cplusplus
}
#endif

#endif /* CORE_BUILD_FOCUS_BLOCK_H */
