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
 * Pending slots: what a tool staged for the user's confirm, one item of each
 * kind per session (see core/pending_slots.h).
 */

#include "core/pending_slots.h"

#include <stdatomic.h>
#include <string.h>

/* Item ids, shared by every table: a staged item never reuses an id. */
static _Atomic uint32_t s_next_item_id = 1;

time_t pending_slots_now(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (time_t)ts.tv_sec;
}

static pending_slot_t *slot_at(const pending_slots_t *slots, int i) {
   return (pending_slot_t *)((char *)slots->base + (size_t)i * slots->stride);
}

static bool expired(const pending_slots_t *slots, const pending_slot_t *s, time_t now) {
   return now - s->made_at > slots->ttl_sec;
}

/* The item of @p kind this session's @p user_id staged (live or not). */
static pending_slot_t *own_slot(const pending_slots_t *slots,
                                const turn_origin_t *current,
                                int user_id,
                                int kind) {
   for (int i = 0; i < slots->count; i++) {
      pending_slot_t *s = slot_at(slots, i);
      if (s->active && s->user_id == user_id && s->kind == kind &&
          s->origin.session_id == current->session_id) {
         return s;
      }
   }
   return NULL;
}

void pending_slots_clear(const pending_slots_t *slots, pending_slot_t *slot) {
   if (slots && slot) {
      memset(slot, 0, slots->stride);
   }
}

static uint32_t next_item_id(void) {
   uint32_t id;
   do {
      id = atomic_fetch_add(&s_next_item_id, 1);
   } while (id == 0);
   return id;
}

pending_slot_t *pending_slots_stage(const pending_slots_t *slots,
                                    const turn_origin_t *current,
                                    int user_id,
                                    int kind,
                                    time_t now,
                                    pending_stage_rc_t *rc) {
   pending_stage_rc_t scratch;
   if (!rc) {
      rc = &scratch;
   }
   *rc = PENDING_FULL;
   if (!slots || !current || slots->stride < sizeof(pending_slot_t)) {
      return NULL;
   }
   /* Nothing expired lingers (a staged text body or brief). */
   for (int i = 0; i < slots->count; i++) {
      pending_slot_t *s = slot_at(slots, i);
      if (s->active && expired(slots, s, now)) {
         pending_slots_clear(slots, s);
      }
   }
   pending_slot_t *slot = own_slot(slots, current, user_id, kind);
   if (slot && current->turn_token != 0 && slot->origin.turn_token == current->turn_token) {
      *rc = PENDING_TWICE_IN_TURN;
      return NULL;
   }
   for (int i = 0; !slot && i < slots->count; i++) {
      pending_slot_t *s = slot_at(slots, i);
      if (!s->active) {
         slot = s;
      }
   }
   if (!slot) {
      return NULL;
   }
   pending_slots_clear(slots, slot);
   slot->active = true;
   slot->user_id = user_id;
   slot->kind = kind;
   slot->item_id = next_item_id();
   slot->origin = turn_origin_stored(current);
   slot->made_at = now;
   *rc = PENDING_STAGED;
   return slot;
}

pending_find_rc_t pending_slots_find(const pending_slots_t *slots,
                                     const turn_origin_t *current,
                                     int user_id,
                                     int kind,
                                     uint32_t item_id,
                                     time_t now,
                                     pending_slot_t **out) {
   if (out) {
      *out = NULL;
   }
   if (!slots || !current) {
      return PENDING_NONE;
   }
   pending_slot_t *s = own_slot(slots, current, user_id, kind);
   if (!s) {
      return PENDING_NONE;
   }
   if (expired(slots, s, now)) {
      pending_slots_clear(slots, s);
      return PENDING_EXPIRED;
   }
   if (item_id != 0 && s->item_id != item_id) {
      return PENDING_OTHER_ITEM;
   }
   if (out) {
      *out = s;
   }
   return PENDING_FOUND;
}

pending_find_rc_t pending_slots_take(const pending_slots_t *slots,
                                     const turn_origin_t *current,
                                     int user_id,
                                     int kind,
                                     uint32_t item_id,
                                     time_t now,
                                     void *out,
                                     size_t out_size,
                                     turn_origin_rc_t *orc) {
   pending_slot_t *s = NULL;
   pending_find_rc_t rc = pending_slots_find(slots, current, user_id, kind, item_id, now, &s);
   if (rc != PENDING_FOUND) {
      return rc;
   }
   const turn_origin_rc_t check = turn_origin_check(&s->origin, current);
   if (orc) {
      *orc = check;
   }
   if (check != TURN_ORIGIN_OK) {
      return PENDING_NOT_NOW;
   }
   if (!out || out_size < slots->stride) {
      return PENDING_NONE;
   }
   memcpy(out, s, slots->stride);
   pending_slots_clear(slots, s);
   return PENDING_FOUND;
}

void pending_slots_drop(const pending_slots_t *slots,
                        const turn_origin_t *current,
                        int user_id,
                        int kind) {
   if (!slots || !current) {
      return;
   }
   pending_slot_t *s = own_slot(slots, current, user_id, kind);
   /* An item this turn staged stays: dropping it would let a second item of
    * the kind be staged in the same turn (see pending_slots_stage). */
   if (s && !(current->turn_token != 0 && s->origin.turn_token == current->turn_token)) {
      pending_slots_clear(slots, s);
   }
}

int pending_slots_valid_for(const pending_slots_t *slots, const pending_slot_t *slot, time_t now) {
   if (!slots || !slot) {
      return 0;
   }
   return (int)(slots->ttl_sec - (now - slot->made_at));
}
