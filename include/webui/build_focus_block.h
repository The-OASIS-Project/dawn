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
 * Per-turn focus-block builder — Phase 1e of Dynamic Context Injection.
 *
 * Sole consumer: src/webui/webui_server.c::dawn_build_prompt(), called
 * once per refresh.  Layer 4 (webui) — pulls in the L2 focus framework
 * via core/focus/focus_source.h and the L2 embedding engine via
 * memory/memory_embeddings.h.
 */

#ifndef WEBUI_BUILD_FOCUS_BLOCK_H
#define WEBUI_BUILD_FOCUS_BLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct session;

/**
 * @brief Build the per-turn focus-injection block.
 *
 * Pipeline (when enabled):
 *   1. Embed `user_turn_text` via memory_embeddings_embed.
 *   2. Call focus_compose with config-driven top_k.
 *   3. Render surviving candidates as multi-line "[<source_id>] <text>".
 *   4. (Phase 1g-i) Broadcast `context_injection` WebSocket event to
 *      every WebUI session matching (user_id, conv_id) — even on empty
 *      result, even on filter-only-rejected turns.  Skipped only when
 *      the feature gate is off.
 *
 * Short-circuits at the top with `*out_block = NULL, return SUCCESS`
 * when:
 *   - Feature gate (`config->memory.focus_injection.enabled`) is off
 *   - `user_turn_text` is NULL or empty
 *   - User is unauthenticated (`user_id <= 0`)
 *
 * Returns SUCCESS with `*out_block = NULL` on any benign zero-result
 * (no candidates survive ranking, embedding unavailable, etc.).
 * Returns FAILURE on hard errors (out-of-memory, focus_compose
 * FAILURE).  Caller MUST treat NULL `*out_block` as "omit the focus
 * section entirely" — never as "use last turn's content."
 *
 * The block returned does NOT include framing markers.  The composer
 * in session_manager wraps the block in
 * `--- TURN CONTEXT ---` ... `--- END TURN CONTEXT ---` so the framing
 * lives in one place.
 *
 * Logging:
 *   - LOG_INFO once per non-short-circuited call summarizing
 *     candidate count, rejection count, and elapsed time.  Candidate
 *     text content is NEVER logged (privacy).
 *   - LOG_WARNING per source with non-zero filter rejections.
 *
 * @param session        The session the turn runs on: its focus dedup state and
 *                       the conversation's item handles ([M7]); NULL for none
 * @param user_id        Authenticated user (must be > 0)
 * @param conv_id        Active conversation id (used to scope the
 *                       context_injection broadcast).  0 disables the
 *                       broadcast for this call (SESSION_START path,
 *                       no conversation pinned yet).
 * @param turn_id        DB id of the user message that triggered this
 *                       prompt rebuild (broadcast field).  0 when not
 *                       known — broadcast still fires; clients should
 *                       handle 0 as "turn id unavailable."
 * @param user_turn_text Raw user message text
 * @param[out] out_block Caller-owned heap string, or NULL on
 *                       SUCCESS-with-no-candidates.  Caller frees.
 * @return SUCCESS or FAILURE.  See contract above.
 */
int build_focus_block(struct session *session,
                      int user_id,
                      int64_t conv_id,
                      int64_t turn_id,
                      const char *user_turn_text,
                      char **out_block);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_BUILD_FOCUS_BLOCK_H */
