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
 * Speculative-decode result slot — a pure, testable state machine.
 *
 * Part of the adaptive end-of-speech dwell (overlapping a speculative decode with
 * the dwell).
 * On the always-on voice path, when the user pauses mid-command a speculative
 * Whisper decode is started on a snapshot of the audio-so-far WITHOUT committing;
 * resumed speech discards it, and a committed end-of-speech takes it. Today the
 * taken transcript is only compared with the real decode (shadow mode); using it
 * would hide the serial Whisper time. This module owns only the SLOT bookkeeping (generation
 * tagging, arm/cancel/store/take, the in-flight and per-utterance-fire caps). It
 * owns no audio, no threads, no locks.
 *
 * ---- Locking contract (READ THIS) --------------------------------------------
 * The slot is NOT internally synchronized. The owner (the always-on connection)
 * embeds one `spec_slot_t` and MUST hold its own `ctx->mutex` across every call
 * here, with a single exception: `spec_slot_gen()` reads the `_Atomic gen` field
 * lock-free (reserved for a decode that polls it to abort early; nothing reads it that
 * way yet). Every mutating
 * call happens under the mutex, so the compare-and-store in `spec_slot_store()`
 * is atomic against the invalidations driven from the connection's threads.
 *
 * ---- Generation model --------------------------------------------------------
 * `gen` is bumped by every invalidation (a new utterance via set_state, and a
 * resumed-speech cancel). A speculative worker captures `gen` at launch and, when
 * it finishes, stores its transcript ONLY if `gen` is unchanged AND the connection
 * is still recording. Any bump between launch and store drops the result. So a
 * stale decode can never be consumed and the commit path never acts on tentative
 * audio from a superseded utterance.
 */

#ifndef SPEC_SLOT_H
#define SPEC_SLOT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
   /* Lock-free ONLY for a future abort poll; every other access is under the
    * owner's mutex (see the locking contract above). */
   _Atomic uint64_t gen;

   bool armed;         /* a tentative endpoint is armed for the current pause     */
   bool inflight;      /* a speculative decode worker is running (<=1 per owner)  */
   int fires;          /* speculative decodes launched this utterance (cap)       */
   bool ready;         /* `text` holds a current speculative transcript           */
   char *text;         /* owned speculative transcript (NULL unless ready)        */
   uint64_t ready_gen; /* gen under which `text` was produced                  */

   /* Instrumentation (ms wall-clock; 0 = unset). */
   int64_t launch_ms; /* when the current/last speculative decode was launched */
   int64_t ready_ms;  /* when the current stored result finished decoding      */
} spec_slot_t;

/** @brief Zero the slot (no allocation; safe on a fresh struct). */
void spec_slot_init(spec_slot_t *s);

/** @brief Free the owned transcript and NULL it. For owner teardown. */
void spec_slot_free(spec_slot_t *s);

/**
 * @brief Invalidate the slot: drop any stored/tentative result and bump `gen`.
 * @param reset_fires  also zero the per-utterance fire counter.
 *
 * Called from the owner's single-writer transitions: every state change passes
 * `reset_fires = (new_state == RECORDING)` (a new utterance resets the cap); a
 * resumed-speech cancel passes `reset_fires = false` (the cap persists across a
 * stutter). Does NOT touch `inflight` — an in-flight worker clears that itself
 * when it observes the bump and drops its result.
 */
void spec_slot_invalidate(spec_slot_t *s, bool reset_fires);

/** @brief Current generation (lock-free; reserved for a future abort poll). */
uint64_t spec_slot_gen(const spec_slot_t *s);

/**
 * @brief May a new speculative decode be armed right now?
 * @return true iff not already armed, none in flight, and under the fire cap.
 * The caller still owns the try-borrow / spawn; on success it calls
 * spec_slot_mark_launched(), and on a spawn/borrow failure it changes nothing.
 */
bool spec_slot_can_arm(const spec_slot_t *s, int max_fires);

/**
 * @brief Record that a speculative decode was successfully spawned.
 * Sets armed + inflight, increments the fire count, stamps launch_ms.
 * MUST be called only after spec_slot_can_arm() returned true and the worker
 * thread was actually created; pair the captured spec_slot_gen() with the worker.
 */
void spec_slot_mark_launched(spec_slot_t *s, int64_t now_ms);

/**
 * @brief Worker hand-in: store a finished speculative transcript if still current.
 * @param gen               the generation captured at launch.
 * @param text              heap transcript; ownership TRANSFERS UNCONDITIONALLY.
 * @param now_ms            wall-clock now.
 * @param state_is_recording owner still in RECORDING (belt-and-braces vs teardown).
 * @return true  → the result is current and now held in the slot.
 *         false → stale/superseded, or `text` was NULL (a blank/failed decode),
 *                 and DROPPED.
 * The slot takes ownership of `text` either way — it is stored on true and freed
 * on false — so the caller must NEVER free `text` after this call. Passing NULL
 * `text` is valid: it clears `inflight` and returns false without marking ready
 * (the way a worker reports "decode finished, nothing usable"). Clears `inflight`
 * on EVERY outcome (the decode is done regardless).
 */
bool spec_slot_store(spec_slot_t *s,
                     uint64_t gen,
                     char *text,
                     int64_t now_ms,
                     bool state_is_recording);

/**
 * @brief Commit-time take: hand out the stored transcript iff it is current.
 * @return owned transcript (caller frees) when ready and gen matches, else NULL.
 * Clears `ready` and detaches `text` on success; leaves armed/inflight to the
 * following invalidation (the commit's set_state).
 */
char *spec_slot_take_if_current(spec_slot_t *s);

/**
 * @brief Is the current silence inside the tentative-arm ("hush") window?
 *
 * Pure timing predicate for the always-on speculative-decode arm decision, the
 * wall-clock analogue of the endpointer's t_hush arm: the silence since the last
 * speech frame is at least the hush but has not yet reached the commit dwell — a
 * mid-utterance pause long enough to speculate on, short enough that the commit
 * has not fired. Returns false when no speech has been seen yet
 * (@p last_speech_ms <= 0), when hush is disabled (@p hush_ms <= 0), or when hush
 * is not strictly shorter than the commit dwell (so it can never race the commit).
 * The commit clock (end-of-speech) is checked separately and takes priority; this
 * only governs the earlier, cancellable tentative fire.
 */
static inline bool spec_in_hush_window(int64_t last_speech_ms,
                                       int64_t now_ms,
                                       int64_t hush_ms,
                                       int64_t eos_ms) {
   if (last_speech_ms <= 0 || hush_ms <= 0 || hush_ms >= eos_ms) {
      return false;
   }
   const int64_t gap = now_ms - last_speech_ms;
   return gap >= hush_ms && gap < eos_ms;
}

#endif /* SPEC_SLOT_H */
