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
 * Voice end-of-speech decision — pure endpointer. See endpointer.h.
 */

#include "core/endpointer.h"

#include <string.h>

void endpointer_configure(endpointer_t *ep,
                          float t_hush,
                          float t_commit,
                          float max_rec,
                          bool adaptive) {
   if (!ep) {
      return;
   }
   ep->t_hush = t_hush;
   ep->t_commit = t_commit;
   ep->max_rec = max_rec;
   ep->adaptive = adaptive;
}

void endpointer_reset(endpointer_t *ep) {
   if (!ep) {
      return;
   }
   ep->speech_s = 0.0f;
   ep->silence_s = 0.0f;
   ep->recording_s = 0.0f;
   ep->saw_speech = false;
   ep->tentative = false;
   ep->commit_forced = false;
   ep->tentative_count = 0;
   ep->resume_count = 0;
   ep->max_pause_s = 0.0f;
}

void endpointer_init(endpointer_t *ep, float t_hush, float t_commit, float max_rec, bool adaptive) {
   if (!ep) {
      return;
   }
   memset(ep, 0, sizeof(*ep));
   endpointer_configure(ep, t_hush, t_commit, max_rec, adaptive);
}

endpoint_event_t endpointer_feed(endpointer_t *ep, bool is_speech, float dt) {
   if (!ep) {
      return ENDPOINT_NONE;
   }

   ep->recording_s += dt;
   ep->commit_forced = false;

   if (is_speech) {
      ep->speech_s += dt;
      ep->saw_speech = true;
      /* Resumed speech cancels an outstanding tentative endpoint. Record the pause
       * we just survived (the tentative->resume gap) for shadow instrumentation. */
      if (ep->tentative) {
         ep->tentative = false;
         ep->resume_count++;
         if (ep->silence_s > ep->max_pause_s) {
            ep->max_pause_s = ep->silence_s;
         }
         ep->silence_s = 0.0f;
         return ENDPOINT_CANCEL;
      }
      ep->silence_s = 0.0f;
   } else {
      ep->silence_s += dt;
   }

   /* Priority order mirrors the legacy dawn.c if-else-if: the max-recording cap is
    * checked first and wins over end-of-speech. */
   if (ep->max_rec > 0.0f && ep->recording_s >= ep->max_rec) {
      ep->commit_forced = true;
      return ENDPOINT_COMMIT;
   }

   /* Committed end-of-speech — the legacy `silence >= end_of_speech_duration`. */
   if (ep->silence_s >= ep->t_commit) {
      return ENDPOINT_COMMIT;
   }

   /* Tentative endpoint: arm once when silence first crosses t_hush. A caller in
    * adaptive mode may act on it (speculative decode); in legacy mode it is a
    * pure shadow signal that changes no timing. Disabled when t_hush is 0 or not
    * strictly shorter than the commit dwell. */
   if (!ep->tentative && ep->t_hush > 0.0f && ep->t_hush < ep->t_commit &&
       ep->silence_s >= ep->t_hush) {
      ep->tentative = true;
      ep->tentative_count++;
      return ENDPOINT_TENTATIVE;
   }

   return ENDPOINT_NONE;
}
