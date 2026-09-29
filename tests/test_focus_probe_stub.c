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
 * Stubs for test_focus_probe — minimal surface to satisfy build_focus_block
 * and focus_compose link requirements without dragging in the real
 * embedding engine, the WebSocket server, or the rest of the daemon.
 *
 * Differs from test_prompt_builder_stub.c in one critical way: the focus
 * framework (focus_source.c + focus_candidate_helpers.c) and the real
 * filter (memory_filter.c) link into the test binary directly, so this
 * stub only covers the dependencies build_focus_block reaches OUTSIDE
 * those files (embedding engine + broadcast).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "config/dawn_config.h"
#include "core/focus/focus_handles.h"
#include "core/focus/focus_source.h"
#include "dawn_error.h"
#include "memory/memory_embeddings.h"

/* Forward declaration of session — production lives in webui_server.h but
 * this stub avoids pulling in the libwebsockets-laden full header. */
struct session;

/* g_config is defined here as the single owner — the test TU references
 * it via extern.  All ranker tests pre-populate before each compose. */
dawn_config_t g_config;

/* =============================================================================
 * memory_embeddings stubs
 *
 * build_focus_block calls memory_embeddings_available + memory_embeddings_embed
 * before invoking focus_compose.  Tests don't care whether embedding
 * succeeded — they exercise the ranker / dedup / budget paths directly —
 * so the stubs always return "available, succeeded with zero vector."
 *
 * `_dims` is not called by build_focus_block today but is part of the
 * public engine surface; included for forward compatibility / safety.
 * ============================================================================= */

bool memory_embeddings_available(void) {
   return true;
}

int memory_embeddings_dims(void) {
   return 4;
}

int memory_embeddings_embed(const char *text, float *out, int *out_dims) {
   (void)text;
   if (out != NULL) {
      out[0] = 1.0f;
      out[1] = 0.0f;
      out[2] = 0.0f;
      out[3] = 0.0f;
   }
   if (out_dims != NULL)
      *out_dims = 4;
   return SUCCESS;
}

/* =============================================================================
 * WebSocket broadcast stub
 *
 * build_focus_block calls this when conv_id > 0.  Real implementation
 * lives in webui_server.c which pulls in libwebsockets, json-c, the
 * full ws connection registry — none of which the unit test cares about.
 * No-op stub.
 * ============================================================================= */

void webui_broadcast_context_injection(int user_id,
                                       int64_t conv_id,
                                       int64_t turn_id,
                                       const focus_compose_result_t *result) {
   (void)user_id;
   (void)conv_id;
   (void)turn_id;
   (void)result;
}

/* The session's stable citation handles (core/focus/focus_handles.c needs the
 * database): each call numbers its items 1, 2, 3, the old per-turn numbering. */
/* No conversation here: no tag secret to mask. */
char *session_prefix_mask_secret(struct session *session, char *text) {
   (void)session;
   return text;
}

int focus_handles_assign(struct session *session,
                         int64_t conv_id,
                         int user_id,
                         conv_focus_handle_t *items,
                         int count) {
   (void)session;
   (void)conv_id;
   (void)user_id;
   for (int i = 0; i < count; i++) {
      items[i].handle = i + 1;
      items[i].is_new = true;
   }
   return 0;
}

/* =============================================================================
 * session_turn_conversation stub — build_focus_block.c re-reads the dispatching
 * turn's conversation right before broadcast (first-turn race fix).  Tests don't
 * run turns, so returning 0 is the safe no-op (caller falls back to the captured
 * value).
 * ============================================================================= */
int64_t session_turn_conversation(struct session *session) {
   (void)session;
   return 0;
}
