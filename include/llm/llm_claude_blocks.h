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
 * Capture a streamed Claude response's content blocks, in order and exactly as
 * sent: each thinking block with its own signature (empty or not), redacted
 * thinking, text, tool_use and anything else.  What DAWN replays is what the
 * API returned, so a signed block's bound prefix is never edited by DAWN.
 */

#ifndef LLM_CLAUDE_BLOCKS_H
#define LLM_CLAUDE_BLOCKS_H

#include "core/strbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

struct json_object;

/** One response's capture state (zero-initialize; free with _reset). */
typedef struct {
   struct json_object *content; /**< The finished blocks, in order */
   struct json_object *current; /**< The block being streamed (not yet in content) */
   strbuf_t text;               /**< Its text / thinking / partial_json */
   strbuf_t signature;          /**< Its signature (thinking) */
   bool text_init;
   bool signature_init;
} llm_claude_capture_t;

/** content_block_start: begin @p content_block (the event's "content_block"). */
void llm_claude_capture_start(llm_claude_capture_t *c, struct json_object *content_block);

/** content_block_delta: add the event's "delta" to the current block. */
void llm_claude_capture_delta(llm_claude_capture_t *c, struct json_object *delta);

/** content_block_stop: finish the current block. */
void llm_claude_capture_stop(llm_claude_capture_t *c);

/**
 * @brief The finished blocks (caller owns them; NULL if none)
 *
 * A block still open (the stream ended mid-block) is discarded: an incomplete
 * signature can't be replayed.  Resets the capture.
 */
struct json_object *llm_claude_capture_take(llm_claude_capture_t *c);

/** Free everything the capture holds. */
void llm_claude_capture_reset(llm_claude_capture_t *c);

#ifdef __cplusplus
}
#endif

#endif /* LLM_CLAUDE_BLOCKS_H */
