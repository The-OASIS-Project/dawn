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
 * Voice end-of-speech decision — a pure, testable endpointer.
 *
 * The end-of-speech decision was previously inline in the dawn.c voice state
 * machine (a ~4k-line file) as bare float comparisons, untestable. This module
 * isolates just the DECISION: given a stream of per-frame (is_speech, dt), it
 * emits NONE / TENTATIVE / CANCEL / COMMIT. It owns no audio, no I/O, no config
 * globals — the caller feeds it and reacts to the event — so its behavior is
 * unit-testable via trace replay.
 *
 * Two modes, one code path:
 *   - LEGACY (adaptive=false): COMMIT fires exactly when silence >= t_commit
 *     (or recording >= max_rec, which wins) — byte-for-byte the pre-existing
 *     `silence_duration >= end_of_speech_duration` behavior. TENTATIVE/CANCEL are
 *     still emitted for SHADOW instrumentation (they change no timing) so the
 *     tentative->resume gap distribution can be measured before adaptive is ever
 *     enabled.
 *   - ADAPTIVE (adaptive=true): same COMMIT, but a caller may act on the
 *     TENTATIVE at t_hush (e.g. start a speculative decode) knowing CANCEL will
 *     fire first if speech resumes before t_commit. Commit timing is UNCHANGED
 *     from legacy — adaptive only adds the earlier, cancellable tentative signal.
 *
 * Never act on a TENTATIVE as if it were final: wake-word match, TTS resume, and
 * dispatch belong to COMMIT only.
 */

#ifndef ENDPOINTER_H
#define ENDPOINTER_H

#include <stdbool.h>

typedef enum {
   ENDPOINT_NONE = 0,  /* still recording; nothing to do this frame           */
   ENDPOINT_TENTATIVE, /* silence just reached t_hush; tentative end-of-speech */
   ENDPOINT_CANCEL,    /* speech resumed while a tentative endpoint was armed  */
   ENDPOINT_COMMIT,    /* end of speech: finalize + transition (the real EOS)  */
} endpoint_event_t;

typedef struct {
   /* Config (seconds). Re-synced each frame by the caller (config is runtime-
    * editable), so live settings changes apply exactly as before. */
   float t_hush;   /* silence to a tentative endpoint (0 or >= t_commit disables it) */
   float t_commit; /* silence to a committed end-of-speech (the legacy dwell)        */
   float max_rec;  /* hard utterance cap; 0 = no cap. Wins over t_commit.            */
   bool adaptive;  /* false = legacy timing + shadow signal; true = act on tentative */

   /* Live state (per utterance; cleared by endpointer_reset). */
   float speech_s;
   float silence_s;
   float recording_s;
   bool saw_speech;
   bool tentative;     /* a tentative endpoint is currently outstanding */
   bool commit_forced; /* the last COMMIT was the max_rec cap, not end-of-speech */

   /* Shadow instrumentation (per utterance; cleared by endpointer_reset). */
   int tentative_count; /* tentative endpoints armed this utterance */
   int resume_count;    /* times speech resumed after a tentative (cancels) */
   float max_pause_s;   /* longest mid-utterance pause that still resumed (t_hush..) */
} endpointer_t;

/** @brief Initialize with config and zeroed state. */
void endpointer_init(endpointer_t *ep, float t_hush, float t_commit, float max_rec, bool adaptive);

/** @brief Update config only (per-frame re-sync); leaves live state intact. */
void endpointer_configure(endpointer_t *ep,
                          float t_hush,
                          float t_commit,
                          float max_rec,
                          bool adaptive);

/** @brief Clear live state + shadow stats for a new utterance; keep config. */
void endpointer_reset(endpointer_t *ep);

/**
 * @brief Advance by one frame.
 * @param is_speech VAD verdict for this frame (true = speech).
 * @param dt        frame duration in seconds (e.g. 0.05).
 * @return the event for this frame (see endpoint_event_t).
 *
 * At most one event per frame. Priority: max_rec cap (COMMIT, forced) > committed
 * end-of-speech (COMMIT) > resumed-speech (CANCEL) > tentative arm (TENTATIVE) >
 * NONE. On any speech frame silence is reset to 0 (legacy semantics preserved).
 */
endpoint_event_t endpointer_feed(endpointer_t *ep, bool is_speech, float dt);

/** @brief True if the most recent COMMIT was the max_rec cap (vs end-of-speech). */
static inline bool endpointer_commit_forced(const endpointer_t *ep) {
   return ep->commit_forced;
}

#endif /* ENDPOINTER_H */
