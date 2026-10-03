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
 * Pending slots: actions a tool staged for the user's confirm (a call
 * preview, a delete awaiting its yes).
 *
 * A slot belongs to the session and the user that staged it, and holds one
 * item of each kind per session: staging again in that session replaces it,
 * and staging in another session (another device, channel or login; two tabs
 * of one browser share a session) never touches it.  Each staged item gets a
 * new id, which its preview shows and its confirm must name: so an item
 * staged again in the same session (by a forged text in a shared channel, or
 * content the model read) is never the one a confirm for the earlier preview
 * carries out.  A confirm also checks the item against the turn that made it
 * (turn_origin_check).
 *
 * A tool keeps its slots in an array of structs whose first member is a
 * pending_slot_t (PENDING_ITEM_CHECK), declares the table with
 * PENDING_SLOTS_TABLE, and serializes every call with its own mutex: a
 * pending_slot_t pointer from this module is valid only while it's held.
 * Pure apart from the id counter and pending_slots_now(): no locks.
 */

#ifndef PENDING_SLOTS_H
#define PENDING_SLOTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "core/turn_origin.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
   bool active;
   int user_id;
   int kind;             /* the tool's own kind of pending item */
   uint32_t item_id;     /* new for every staged item; never 0 */
   turn_origin_t origin; /* where it was made (turn_origin_stored) */
   time_t made_at;       /* pending_slots_now() when staged */
} pending_slot_t;

/** A tool's slots: @p count structs of @p stride bytes, each starting with a
 *  pending_slot_t. */
typedef struct {
   void *base;
   size_t stride;
   int count;
   int ttl_sec; /* an item older than this has expired */
} pending_slots_t;

/** A pending_slots_t over the array @p arr (whose element type passed
 *  PENDING_ITEM_CHECK, and which passed PENDING_ARRAY_CHECK). */
#define PENDING_SLOTS_TABLE(arr, ttl)                                                            \
   {                                                                                             \
      .base = (arr), .stride = sizeof((arr)[0]), .count = (int)(sizeof(arr) / sizeof((arr)[0])), \
      .ttl_sec = (ttl)                                                                           \
   }

/** A pending item struct must start with its pending_slot_t named hdr. */
#define PENDING_ITEM_CHECK(type)                                                      \
   _Static_assert(offsetof(type, hdr) == 0 && sizeof(type) >= sizeof(pending_slot_t), \
                  #type " must start with its pending_slot_t hdr")

/** A table's storage must be an array, not a pointer (its count would be
 *  wrong). */
#define PENDING_ARRAY_CHECK(arr)                                                         \
   _Static_assert(!__builtin_types_compatible_p(__typeof__(arr), __typeof__(&(arr)[0])), \
                  #arr " must be an array")

/** Seconds on a clock that never steps back (expiry must not stretch when the
 *  wall clock is set back). */
time_t pending_slots_now(void);

typedef enum {
   PENDING_STAGED = 0,
   PENDING_FULL,         /* every slot holds another live item (nothing is evicted) */
   PENDING_TWICE_IN_TURN /* this turn already staged one of this kind (it stays) */
} pending_stage_rc_t;

/**
 * @brief Stage an item: this session's item of this kind for this user is
 *        replaced, else a free slot is used (expired items are cleared first)
 *
 * A second item of one kind in the same turn is refused: the user asked once,
 * so a second request in that turn came from something else (content the
 * model read), and replacing the first would have the user approve one item
 * while the confirm carries out another.
 *
 * The returned slot's header is filled (a new item_id, the stored origin); the
 * caller fills the rest of its struct (cleared first).
 *
 * @param rc Receives why it wasn't staged (may be NULL)
 * @return The slot, or NULL (see @p rc)
 */
pending_slot_t *pending_slots_stage(const pending_slots_t *slots,
                                    const turn_origin_t *current,
                                    int user_id,
                                    int kind,
                                    time_t now,
                                    pending_stage_rc_t *rc);

typedef enum {
   PENDING_FOUND = 0,
   PENDING_NONE,       /* nothing of this kind staged in this session for this user */
   PENDING_EXPIRED,    /* it was, but expired (and is cleared) */
   PENDING_OTHER_ITEM, /* this session's item is another one than the id named (it stays) */
   PENDING_NOT_NOW,    /* not the user's reply to its preview (turn_origin_check; it stays) */
} pending_find_rc_t;

/**
 * @brief This session's item of @p kind for @p user_id
 *
 * Only the session in @p current is searched: an item staged in another
 * session is never found.
 *
 * @param item_id The id the confirm names; 0 matches any (for a tool that
 *                binds its confirm otherwise, such as by a token)
 * @param out     Receives the slot on PENDING_FOUND (else NULL)
 * @return PENDING_FOUND, _NONE, _EXPIRED or _OTHER_ITEM
 */
pending_find_rc_t pending_slots_find(const pending_slots_t *slots,
                                     const turn_origin_t *current,
                                     int user_id,
                                     int kind,
                                     uint32_t item_id,
                                     time_t now,
                                     pending_slot_t **out);

/**
 * @brief Take this session's item for its confirm: found, checked against the
 *        turn that made it, copied to @p out (the whole struct) and cleared
 *
 * @param out      Receives the item (stride bytes) on PENDING_FOUND
 * @param out_size Size of @p out (at least the table's stride)
 * @param orc      Receives the turn check's result on PENDING_NOT_NOW (may be NULL)
 */
pending_find_rc_t pending_slots_take(const pending_slots_t *slots,
                                     const turn_origin_t *current,
                                     int user_id,
                                     int kind,
                                     uint32_t item_id,
                                     time_t now,
                                     void *out,
                                     size_t out_size,
                                     turn_origin_rc_t *orc);

/** Clear this session's item of @p kind for @p user_id, if any, unless the
 *  current turn staged it (it stays, so one turn still stages one item). */
void pending_slots_drop(const pending_slots_t *slots,
                        const turn_origin_t *current,
                        int user_id,
                        int kind);

/** Clear one slot and the struct it heads. */
void pending_slots_clear(const pending_slots_t *slots, pending_slot_t *slot);

#ifdef __cplusplus
}
#endif

#endif /* PENDING_SLOTS_H */
