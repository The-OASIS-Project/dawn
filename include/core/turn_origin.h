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
 * Turn origin: where something awaiting the user's confirm was made.
 *
 * A pending action (an email draft, a call preview, a delete) records the
 * turn that made it; its confirm must come from the same session, in the very
 * next turn (the person's reply to the read-back).  So the model can't confirm
 * in the turn it prepared the action, nor in a later turn after the user moved
 * on, and a pending item made in one session (a browser tab, a device, a
 * channel) can't be confirmed from another.  A confirm the user approved by a
 * reply code may come in any later turn of the same session: the code is the
 * proof the next-turn rule stands in for.
 *
 * Capturing an origin needs the session unit (src/core/session_history.c);
 * checking one needs nothing, so a module that only stores and checks them
 * includes this header alone.
 */

#ifndef TURN_ORIGIN_H
#define TURN_ORIGIN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
   uint32_t session_id;
   uint64_t turn_token;  /* 0: no live turn */
   uint32_t turn_number; /* the session's turn count at that turn */
   /* The call runs because the user replied with its code.  Read only from
    * the confirming side (turn_origin_check's @p now); ignored on a stored
    * origin. */
   bool code_redeemed;
} turn_origin_t;

typedef enum {
   TURN_ORIGIN_OK = 0,
   TURN_ORIGIN_OTHER_SESSION, /* another session, or not made in a live turn */
   TURN_ORIGIN_SAME_TURN,     /* the turn that made it, or no live turn now */
   TURN_ORIGIN_NOT_NEXT,      /* later than the turn right after it */
} turn_origin_rc_t;

/**
 * @brief Whether @p now may confirm what was made at @p made: the same
 *        session, and the turn right after it (any later turn when @p now
 *        was code-redeemed)
 */
static inline turn_origin_rc_t turn_origin_check(const turn_origin_t *made,
                                                 const turn_origin_t *now) {
   if (!made || !now || made->turn_token == 0 || made->session_id != now->session_id)
      return TURN_ORIGIN_OTHER_SESSION;
   if (now->turn_token == 0 || now->turn_token == made->turn_token)
      return TURN_ORIGIN_SAME_TURN; /* not a person's later turn */
   if (now->code_redeemed)
      return now->turn_number > made->turn_number ? TURN_ORIGIN_OK : TURN_ORIGIN_SAME_TURN;
   if (now->turn_number != made->turn_number + 1)
      return TURN_ORIGIN_NOT_NEXT; /* the user's reply was another turn */
   return TURN_ORIGIN_OK;
}

/** Why a confirm was refused, for the log. */
static inline const char *turn_origin_refusal(turn_origin_rc_t rc) {
   switch (rc) {
      case TURN_ORIGIN_SAME_TURN:
         return "in the turn that prepared it";
      case TURN_ORIGIN_NOT_NEXT:
         return "later than the turn right after it";
      default:
         return "from another session";
   }
}

/**
 * @brief Where the calling code runs, when it may make or confirm an action:
 *        a running turn the user started (session_turn_user_originated) in
 *        the command context's session
 *
 * code_redeemed is copied from session_call_code_redeemed().
 *
 * @param out Receives the origin (zeroed on false)
 * @return false with no command context, in a job's session, on a background
 *         turn, or with no turn running (an MQTT message naming a session gets
 *         that session as its context, but no turn).  A build without turn
 *         tracking returns false always.
 */
#ifdef ENABLE_MULTI_CLIENT
bool turn_origin_capture(turn_origin_t *out);
#else
/* No turn tracking: the local mic can't be told from an MQTT message, so
 * nothing counts as a live user turn. */
static inline bool turn_origin_capture(turn_origin_t *out) {
   if (out) {
      const turn_origin_t none = { 0 };
      *out = none;
   }
   return false;
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* TURN_ORIGIN_H */
