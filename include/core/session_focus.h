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
 * A turn's retrieved items at its seam: which the history the turn runs on
 * already shows (core/focus/focus_incremental.h), the items part of the
 * turn's context, the per-turn citation map, and what the client's context
 * panel is told.  Part of the session unit with session_prefix.c, which calls
 * it while it builds a turn's context.
 */

#ifndef SESSION_FOCUS_H
#define SESSION_FOCUS_H

#include <stdbool.h>
#include <stdint.h>

#include "core/focus/focus_incremental.h"
#include "core/prompt_parts.h"

#ifdef __cplusplus
extern "C" {
#endif

struct session;
struct json_object;

/** What one turn's seam decided about its items, for the commit once its
 *  context is attached and for the client once the history lock is released. */
typedef struct {
   prompt_focus_item_t *items; /**< the turn's items as this seam sends them: the
                                    prompt's, less any withdrawn since retrieval, and
                                    unnumbered when the handles aren't this history's
                                    (texts borrowed from the prompt) */
   int *from;                  /**< per item: its index in the prompt's focus_items */
   int n;
   focus_selection_t sel; /**< each item's place, in @p items order */
   int *visible;          /**< handles the history shows (sorted), for citing */
   int n_visible;
   bool numbered; /**< the handles are this history's conversation's */
} session_focus_turn_t;

/**
 * @brief The items part of the turn's context in @p hist (caller holds the
 *        session's history_mutex, after the turn's compaction and withdrawals
 *        are applied, before its context goes in)
 *
 * Reads what @p hist's earlier turn contexts show (passing over @p question's
 * own, and an envelope's earlier context, which this apply replaces), decides
 * which of @p cp's items to send, and renders them masked with @p tag.  An
 * item this seam's withdrawal names (@p withdrawn_ids: forgotten since it was
 * retrieved) is left out; one whose handle's line an earlier withdrawal
 * replaced is sent as new, if retrieval found it again.  Nothing is recorded
 * until session_focus_commit_locked.
 *
 * @param hist_conv     The conversation @p hist holds (0: none yet)
 * @param question      The turn's question in @p hist, or NULL
 * @param withdrawn_ids Item ids this seam withdrew (a JSON array of strings), or NULL
 * @param out           Zeroed by the call; pass to session_focus_commit_locked
 *                      and then session_focus_notify
 * @return Heap text (caller frees), or NULL when nothing is sent
 */
char *session_focus_items_locked(struct session *session,
                                 struct json_object *hist,
                                 int64_t hist_conv,
                                 struct json_object *question,
                                 const char *tag,
                                 const composed_prompt_t *cp,
                                 struct json_object *withdrawn_ids,
                                 session_focus_turn_t *out);

/**
 * @brief Record what the turn's context showed, once it is attached (or not:
 *        @p attached false makes every item due to be sent left out)
 *
 * Caller holds history_mutex.  Sets the session's citation map (this turn's
 * sent and named items) and its list of earlier items the history still
 * shows (citable this turn).
 */
void session_focus_commit_locked(struct session *session,
                                 session_focus_turn_t *turn,
                                 bool attached);

/**
 * @brief Tell the client what the turn's retrieval found, each item with its
 *        place (session_focus_client_notice), then free @p turn
 *
 * Called after the history lock is released (the client's registry lock is
 * never taken under it).  Nothing is told when retrieval didn't run.  An item
 * the seam didn't send (a NULL @p turn: there was no history to apply to; or
 * it couldn't choose) is told as left out.
 */
void session_focus_notify(struct session *session,
                          const composed_prompt_t *cp,
                          session_focus_turn_t *turn);

/**
 * @brief Clear the per-turn citation state (memory citation signal): the
 *        stash, the tool-sourced set, and the earlier items citable.
 *
 * SELF-LOCKING — acquires `session->history_mutex` internally.  Called at
 * dispatch entry so a turn whose context has no items cannot inherit the
 * previous turn's [M#]→item_id map and false-validate a stale `<cited>`.
 */
void session_citation_stash_clear(struct session *session);

/**
 * @brief The client's context panel: what the turn's retrieval found
 *
 * A no-op here; the WebUI replaces it (a surface without a panel has nothing
 * to show it on).  @p states is in @p cp->focus_items order (@p n_states of
 * them), or NULL when they couldn't be told (every item is then shown as
 * new).
 */
void session_focus_client_notice(struct session *session,
                                 const composed_prompt_t *cp,
                                 const focus_item_state_t *states,
                                 int n_states);

#ifdef __cplusplus
}
#endif

#endif /* SESSION_FOCUS_H */
