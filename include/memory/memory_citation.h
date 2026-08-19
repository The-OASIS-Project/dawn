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
 * Memory citation signal — capture side (Phase 1, log/audit-only).
 *
 * Parses the model's `<cited>M1,M7</cited>` tag out of a completed response,
 * validates the ordinals against the per-turn [M#]->item_id stash on the
 * session, and records one audit row (injected vs cited item_ids + dropped
 * count) for measuring injection precision.  Does NOT change fact confidence —
 * that reinforcement is Phase 2.
 */
#ifndef MEMORY_CITATION_H
#define MEMORY_CITATION_H

/* Forward declaration — the .c includes core/session_manager.h for the stash. */
typedef struct session session_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Audit which surfaced [M#] memories the model cited this turn.
 *
 * Self-gating and side-effect-only: a no-op unless memory citation is enabled
 * (g_config.memory.citation_enabled), the session is non-NULL, and this turn
 * actually stashed [M#] items.  Reads the tag from @p response_text (does not
 * modify it — the response finalizer strips `<cited>` separately).
 *
 * @param session       The turn's session (carries the per-turn citation stash).
 * @param response_text The completed response, still containing `<cited>` if any.
 */
void memory_citation_capture(session_t *session, const char *response_text);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_CITATION_H */
