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
 * Speculative-decode result slot — pure state machine. See spec_slot.h for the
 * locking contract and generation model.
 */

#include "core/spec_slot.h"

#include <stdatomic.h>
#include <stdlib.h>

void spec_slot_init(spec_slot_t *s) {
   if (!s) {
      return;
   }
   atomic_store(&s->gen, 0);
   s->armed = false;
   s->inflight = false;
   s->fires = 0;
   s->ready = false;
   s->text = NULL;
   s->ready_gen = 0;
   s->launch_ms = 0;
   s->ready_ms = 0;
}

void spec_slot_free(spec_slot_t *s) {
   if (!s) {
      return;
   }
   free(s->text);
   s->text = NULL;
   s->ready = false;
}

void spec_slot_invalidate(spec_slot_t *s, bool reset_fires) {
   if (!s) {
      return;
   }
   /* Drop any stored/tentative result and advance the generation so an in-flight
    * decode's store is rejected. inflight is intentionally left alone — the worker
    * clears it when it observes the bump. */
   free(s->text);
   s->text = NULL;
   s->ready = false;
   s->armed = false;
   atomic_fetch_add(&s->gen, 1);
   if (reset_fires) {
      s->fires = 0;
   }
}

uint64_t spec_slot_gen(const spec_slot_t *s) {
   if (!s) {
      return 0;
   }
   return atomic_load(&s->gen);
}

bool spec_slot_can_arm(const spec_slot_t *s, int max_fires) {
   if (!s) {
      return false;
   }
   return !s->armed && !s->inflight && s->fires < max_fires;
}

void spec_slot_mark_launched(spec_slot_t *s, int64_t now_ms) {
   if (!s) {
      return;
   }
   s->armed = true;
   s->inflight = true;
   s->fires++;
   s->launch_ms = now_ms;
}

bool spec_slot_store(spec_slot_t *s,
                     uint64_t gen,
                     char *text,
                     int64_t now_ms,
                     bool state_is_recording) {
   if (!s) {
      free(text);
      return false;
   }
   /* The decode is finished regardless of whether we keep it. */
   s->inflight = false;

   if (gen != atomic_load(&s->gen) || !state_is_recording) {
      /* Superseded by a cancel/new-utterance, or the connection left RECORDING. */
      free(text);
      return false;
   }

   free(s->text);
   s->text = text;
   s->ready_gen = gen;
   s->ready = true;
   s->ready_ms = now_ms;
   return true;
}

char *spec_slot_take_if_current(spec_slot_t *s) {
   if (!s || !s->ready || s->ready_gen != atomic_load(&s->gen)) {
      return NULL;
   }
   char *t = s->text;
   s->text = NULL;
   s->ready = false;
   return t;
}
