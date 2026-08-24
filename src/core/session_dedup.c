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
 * Per-session focus-injection dedup state — Phase 1f of Dynamic Context
 * Injection.  Pure helpers operating on session_t::injected_set; extracted
 * from session_manager.c specifically so test_prompt_builder.c can link
 * the dedup APIs without dragging the full session-manager runtime
 * (auth_db, conv_db, satellite_db, json-c) into the test link — same
 * pattern as src/core/prompt_compose.c.
 *
 * Threading: the `_locked` suffix on three of the four public functions
 * makes the contract loud — caller owns history_mutex.  `_clear` is
 * self-locking and must NOT be called while history_mutex is held.  The
 * thread-local dispatch-session getter/setter is lock-free TLS: each
 * worker thread owns its own pointer.
 */

#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/session_manager.h"
#include "dawn_error.h"
#include "logging.h"

#ifdef ENABLE_MULTI_CLIENT

/* =============================================================================
 * Thread-local dispatch session — read by build_focus_block to find the
 * session whose dedup set to update.  Set by session_dispatch_user_turn
 * before invoking the prompt builder and cleared after.  NULL on every
 * non-PER_TURN path so build_focus_block can detect "no session
 * available" and skip dedup safely.
 * ============================================================================= */
static __thread session_t *t_dispatch_session = NULL;

void session_set_dispatch_session(session_t *session) {
   t_dispatch_session = session;
}

session_t *session_get_dispatch_session(void) {
   return t_dispatch_session;
}

/* =============================================================================
 * Dedup-set primitives.  All operate on the fixed-size injected_set_t
 * embedded in session_t — zero heap state, zero teardown cost on session
 * destroy.
 * ============================================================================= */

int session_injected_set_lookup_locked(const session_t *session,
                                       const char *source_id,
                                       const char *item_id,
                                       injected_set_entry_t *out) {
   if (session == NULL || source_id == NULL || item_id == NULL || out == NULL)
      return FAILURE;

   const injected_set_t *set = &session->injected_set;
   for (int i = 0; i < set->count; i++) {
      const injected_set_entry_t *e = &set->entries[i];
      if (strcmp(e->source_id, source_id) == 0 && strcmp(e->item_id, item_id) == 0) {
         *out = *e;
         return SUCCESS;
      }
   }
   return FAILURE;
}

int session_injected_set_advance_turn_locked(session_t *session) {
   /* Returns 0 on NULL session — distinct from sibling APIs that return
    * SUCCESS (0) / FAILURE (1).  This function returns the new turn
    * counter as a positive int, so the SUCCESS/FAILURE convention
    * doesn't apply here.  The NULL guard is defensive only — production
    * callers in build_focus_block::apply_dedup_locked check session
    * before this call. */
   if (session == NULL)
      return 0;
   /* Pre-increment-and-return.  INT_MAX wrap is cosmetic — would take
    * ~68 years at one turn per second. */
   if (session->injected_set.turn_counter == INT_MAX)
      session->injected_set.turn_counter = 0;
   return ++session->injected_set.turn_counter;
}

/* Find the slot to evict — oldest by last_injected_turn.  Linear scan
 * over the fixed 256-entry array; ~1 µs even on Jetson's slowest core.
 * Tied turns broken by lowest array index (deterministic, doesn't matter
 * which one wins).
 *
 * Defensive guard for set->count == 0: today only called from the
 * eviction branch where count == MAX_INJECTED_SET_SIZE > 0, but a
 * future caller that violates that precondition would otherwise read
 * entries[0] uninitialized.  Return 0 as a safe sentinel. */
static int find_lru_slot(const injected_set_t *set) {
   if (set->count <= 0)
      return 0;
   int oldest_idx = 0;
   int oldest_turn = set->entries[0].last_injected_turn;
   for (int i = 1; i < set->count; i++) {
      if (set->entries[i].last_injected_turn < oldest_turn) {
         oldest_turn = set->entries[i].last_injected_turn;
         oldest_idx = i;
      }
   }
   return oldest_idx;
}

int session_injected_set_record_locked(session_t *session,
                                       const char *source_id,
                                       const char *item_id,
                                       float score) {
   if (session == NULL || source_id == NULL || item_id == NULL)
      return FAILURE;

   injected_set_t *set = &session->injected_set;
   const int current_turn = set->turn_counter;

   /* Existing entry?  Update in place. */
   for (int i = 0; i < set->count; i++) {
      injected_set_entry_t *e = &set->entries[i];
      if (strcmp(e->source_id, source_id) == 0 && strcmp(e->item_id, item_id) == 0) {
         e->last_injected_turn = current_turn;
         e->last_score = score;
         return SUCCESS;
      }
   }

   /* New entry — evict oldest if at capacity. */
   int slot;
   if (set->count < MAX_INJECTED_SET_SIZE) {
      slot = set->count++;
   } else {
      slot = find_lru_slot(set);
      if (!set->eviction_logged_once) {
         OLOG_INFO("focus dedup: first LRU eviction in session (set full at %d entries) — "
                   "long-conversation behavior; subsequent evictions silent",
                   MAX_INJECTED_SET_SIZE);
         set->eviction_logged_once = true;
      }
   }

   injected_set_entry_t *e = &set->entries[slot];
   /* strncpy + explicit NUL is safe even when source_id/item_id exceed
    * the field size — we silently truncate and the dedup key becomes
    * effectively the truncated form (cannot collide with anything else
    * because adapters use stable static source_ids and bounded item_id
    * formats). */
   strncpy(e->source_id, source_id, sizeof(e->source_id) - 1);
   e->source_id[sizeof(e->source_id) - 1] = '\0';
   strncpy(e->item_id, item_id, sizeof(e->item_id) - 1);
   e->item_id[sizeof(e->item_id) - 1] = '\0';
   e->first_injected_turn = current_turn;
   e->last_injected_turn = current_turn;
   e->last_score = score;
   return SUCCESS;
}

void session_injected_set_clear(session_t *session) {
   if (session == NULL)
      return;
   pthread_mutex_lock(&session->history_mutex);
   /* memset zeros entries[], count, turn_counter, both log-once gates,
    * and any padding — single statement, no missed field risk. */
   memset(&session->injected_set, 0, sizeof(session->injected_set));
   pthread_mutex_unlock(&session->history_mutex);
}

void session_citation_stash_clear(session_t *session) {
   if (session == NULL)
      return;
   pthread_mutex_lock(&session->history_mutex);
   memset(&session->citation_stash, 0, sizeof(session->citation_stash));
   /* Clear the tool-sourced citation set on the SAME seam — one per-turn clear for
    * both citation structures (Option B). */
   memset(&session->tool_cited_set, 0, sizeof(session->tool_cited_set));
   pthread_mutex_unlock(&session->history_mutex);
}

#endif /* ENABLE_MULTI_CLIENT */
