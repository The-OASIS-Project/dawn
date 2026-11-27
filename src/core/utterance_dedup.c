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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Cross-device utterance dedup — static singleton suppression gate.
 * See atlas/dawn/archive/CROSS_DEVICE_DEDUP_DESIGN.md and core/utterance_dedup.h.
 */

#include "core/utterance_dedup.h"

#include <pthread.h>
#include <time.h>

#include "logging.h"

/* Number of distinct in-window command sources tracked.  Occupancy equals the
 * number of distinct sessions (same-session events update in place), so 16 is
 * ample for a household.  Beyond 16 concurrent distinct sources LRU eviction
 * means a dropped slot can't suppress its duplicate — a rare extra
 * double-response, never a crash (fail-open). */
#define UTTERANCE_DEDUP_SLOTS 16

typedef struct {
   uint32_t session_id;
   uint64_t ts_ms;
   bool used;
} dedup_slot_t;

static dedup_slot_t s_slots[UTTERANCE_DEDUP_SLOTS];
static int s_window_sec; /* <= 0 disables; static-zero => pre-init safe */
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t (*s_clock)(void); /* NULL => default monotonic-ms clock */

/* Default clock: CLOCK_MONOTONIC milliseconds.  Monotonic (never steps
 * backward) so an NTP correction can't stall slot expiry and silently eat
 * commands the way a wall clock (time(NULL)) would; ms resolution removes the
 * +/-1s edge slop a whole-second window would carry. */
static uint64_t monotonic_ms(void) {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t now_ms(void) {
   return s_clock ? s_clock() : monotonic_ms();
}

void utterance_dedup_init(int window_sec) {
   pthread_mutex_lock(&s_mutex);
   s_window_sec = window_sec;
   for (int i = 0; i < UTTERANCE_DEDUP_SLOTS; i++) {
      s_slots[i].used = false;
   }
   pthread_mutex_unlock(&s_mutex);
}

void utterance_dedup_set_window(int window_sec) {
   pthread_mutex_lock(&s_mutex);
   s_window_sec = window_sec;
   pthread_mutex_unlock(&s_mutex);
}

bool utterance_dedup_check(uint32_t session_id) {
   pthread_mutex_lock(&s_mutex);

   if (s_window_sec <= 0) {
      pthread_mutex_unlock(&s_mutex);
      return false; /* disabled */
   }

   const uint64_t now = now_ms();
   const uint64_t window_ms = (uint64_t)s_window_sec * 1000u;

   /* Purge expired slots; while scanning, look for a live different-session
    * event (=> suppress) and track our own slot + an LRU victim. */
   bool other_active = false;
   dedup_slot_t *own = NULL;
   dedup_slot_t *empty = NULL;
   dedup_slot_t *lru = NULL;
   dedup_slot_t *newest_other = NULL; /* most-recent live peer (for diagnostics) */

   for (int i = 0; i < UTTERANCE_DEDUP_SLOTS; i++) {
      dedup_slot_t *e = &s_slots[i];

      if (!e->used) {
         if (!empty) {
            empty = e;
         }
         continue;
      }

      /* Unsigned subtraction is safe because the clock is monotonic
       * (now >= ts_ms); see the contract on utterance_dedup_set_clock. */
      if ((now - e->ts_ms) >= window_ms) {
         e->used = false; /* expired -> treat as empty */
         if (!empty) {
            empty = e;
         }
         continue;
      }

      if (e->session_id == session_id) {
         own = e;
      } else {
         other_active = true;
         if (!newest_other || e->ts_ms > newest_other->ts_ms) {
            newest_other = e;
         }
      }

      if (!lru || e->ts_ms < lru->ts_ms) {
         lru = e;
      }
   }

   /* A different source already fired in this window: suppress, and do NOT
    * record — anchor the window to the winner so this suppressed device can't
    * later suppress the winner's own deliberate repeat. */
   if (other_active) {
      /* Snapshot peer id + age under the lock; the slot pointer is unstable
       * after unlock (another thread may reclaim it). */
      const uint32_t peer = newest_other ? newest_other->session_id : 0;
      const uint64_t age_ms = newest_other ? (now - newest_other->ts_ms) : 0;
      pthread_mutex_unlock(&s_mutex);
      /* Diagnostic (utterance-dedup skew): logs on BOTH decision paths so a
       * double-response (allow+allow, no suppress) is still visible — compare
       * the t= values across sessions to read the cross-device skew, and this
       * SUPPRESS line shows the exact peer + age when the gate does catch. */
      OLOG_INFO("Dedup CHECK session=%u t=%llums -> SUPPRESS (peer session=%u fired %llums ago, "
                "window=%ds)",
                session_id, (unsigned long long)now, peer, (unsigned long long)age_ms,
                s_window_sec);
      return true;
   }

   /* Allowed.  Record by updating our existing slot if present, else claim an
    * empty/LRU slot.  Update-in-place keeps occupancy = distinct sources so a
    * chatty same-session repeat can't burn through slots. */
   dedup_slot_t *slot = own ? own : (empty ? empty : lru);
   if (slot) { /* lru is non-NULL whenever all slots are full, so this holds */
      slot->session_id = session_id;
      slot->ts_ms = now;
      slot->used = true;
   }

   pthread_mutex_unlock(&s_mutex);
   /* See the SUPPRESS-path note above: this ALLOW line is the one that makes a
    * missed dedup diagnosable (both devices log ALLOW; the t= delta is the skew). */
   OLOG_INFO("Dedup CHECK session=%u t=%llums -> ALLOW (recorded, window=%ds)", session_id,
             (unsigned long long)now, s_window_sec);
   return false;
}

void utterance_dedup_set_clock(uint64_t (*fn)(void)) {
   pthread_mutex_lock(&s_mutex);
   s_clock = fn;
   pthread_mutex_unlock(&s_mutex);
}

void utterance_dedup_reset_all(void) {
   pthread_mutex_lock(&s_mutex);
   for (int i = 0; i < UTTERANCE_DEDUP_SLOTS; i++) {
      s_slots[i].used = false;
   }
   pthread_mutex_unlock(&s_mutex);
}
