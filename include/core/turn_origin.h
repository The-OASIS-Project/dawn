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
 * A turn a rendered visual started (its script asked for it through the
 * bridge, not the person typing) confirms nothing: a visual is model-written
 * and can act on load or after any click on the page.
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
   /* The turn was started by a rendered visual's prompt, not the person.  Read
    * only from the confirming side; ignored on a stored origin. */
   bool from_visual;
   /* The turn carries someone else's text the user attached (an email), which
    * could itself say "yes".  Read only from the confirming side. */
   bool third_party;
} turn_origin_t;

typedef enum {
   TURN_ORIGIN_OK = 0,
   TURN_ORIGIN_OTHER_SESSION, /* another session, or not made in a live turn */
   TURN_ORIGIN_SAME_TURN,     /* the turn that made it, or no live turn now */
   TURN_ORIGIN_NOT_NEXT,      /* later than the turn right after it */
   TURN_ORIGIN_FROM_VISUAL,   /* a turn a rendered visual started */
   TURN_ORIGIN_THIRD_PARTY,   /* a turn carrying someone else's text (an email) */
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
   if (now->from_visual)
      return TURN_ORIGIN_FROM_VISUAL; /* not the person, whatever the words */
   if (now->third_party)
      return TURN_ORIGIN_THIRD_PARTY; /* the yes could be the attached text's */
   if (now->turn_token == 0 || now->turn_token == made->turn_token)
      return TURN_ORIGIN_SAME_TURN; /* not a person's later turn */
   if (now->code_redeemed)
      return now->turn_number > made->turn_number ? TURN_ORIGIN_OK : TURN_ORIGIN_SAME_TURN;
   if (now->turn_number != made->turn_number + 1)
      return TURN_ORIGIN_NOT_NEXT; /* the user's reply was another turn */
   return TURN_ORIGIN_OK;
}

/** An origin as a pending item stores it: approval by code belongs to the
 *  confirming call, never to what was staged. */
static inline turn_origin_t turn_origin_stored(const turn_origin_t *origin) {
   turn_origin_t stored = *origin;
   stored.code_redeemed = false;
   stored.from_visual = false;
   stored.third_party = false;
   return stored;
}

/** Why a confirm was refused, for the log. */
static inline const char *turn_origin_refusal(turn_origin_rc_t rc) {
   switch (rc) {
      case TURN_ORIGIN_SAME_TURN:
         return "in the turn that prepared it";
      case TURN_ORIGIN_NOT_NEXT:
         return "later than the turn right after it";
      case TURN_ORIGIN_FROM_VISUAL:
         return "in a turn a rendered visual started";
      case TURN_ORIGIN_THIRD_PARTY:
         return "in a turn carrying an attached email";
      default:
         return "from another session";
   }
}

/** What the model should do after a refused confirm (a sentence for its
 *  tool result). */
static inline const char *turn_origin_retry_hint(turn_origin_rc_t rc) {
   switch (rc) {
      case TURN_ORIGIN_SAME_TURN:
         return "only the user's reply to its preview can confirm it. Ask them, and confirm "
                "when they say yes.";
      case TURN_ORIGIN_NOT_NEXT:
         return "the conversation moved on since its preview. Prepare it again and read it "
                "back to the user.";
      case TURN_ORIGIN_FROM_VISUAL:
         return "this turn came from a rendered visual, not the user, and a visual can't "
                "approve anything. Prepare it again, read it back, and confirm only when the "
                "user replies themselves.";
      case TURN_ORIGIN_THIRD_PARTY:
         return "this turn carries an email the user attached, and text in it can't approve "
                "anything. Prepare it again, read it back, and confirm only when the user "
                "replies in a message of their own.";
      default:
         return "it was prepared in another conversation. Prepare it again here.";
   }
}

/**
 * @brief Where the calling code runs, when it may make or confirm an action:
 *        a running turn the user started (session_turn_user_originated) in
 *        the command context's session
 *
 * code_redeemed is copied from session_call_code_redeemed(); from_visual and
 * third_party from the running turn (session_turn_mark_from_visual,
 * session_turn_attach).
 *
 * @param out Receives the origin (zeroed on false)
 * @return false with no command context, in a job's session, on a background
 *         turn, or with no turn running (an MQTT message naming a session gets
 *         that session as its context, but no turn).
 */
bool turn_origin_capture(turn_origin_t *out);

#ifdef __cplusplus
}
#endif

#endif /* TURN_ORIGIN_H */
