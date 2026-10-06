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
 * What the model is told when a preview can't be staged or a confirm finds no
 * item.  See tool_pending.h.
 */

#include "tools/tool_pending.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tools/tool_registry.h" /* TOOL_RESULT_ERROR_MARK */

#define TOOL_PENDING_MSG_MAX 256

char *tool_pending_stage_refusal(pending_stage_rc_t rc, const char *what) {
   if (rc != PENDING_TWICE_IN_TURN) {
      return strdup(TOOL_RESULT_ERROR_MARK "Error: too many actions are waiting for a confirm. "
                                           "Confirm one, or try again in a couple of minutes.");
   }
   char buf[TOOL_PENDING_MSG_MAX];
   snprintf(buf, sizeof(buf),
            TOOL_RESULT_ERROR_MARK "Error: a %s is already waiting for the user's yes from this "
                                   "turn. Ask about that one first; prepare another after.",
            what);
   return strdup(buf);
}

char *tool_pending_missing_id(const char *what) {
   char buf[TOOL_PENDING_MSG_MAX];
   snprintf(buf, sizeof(buf),
            TOOL_RESULT_ERROR_MARK "Error: name the %s with the pending_id its preview gave.",
            what);
   return strdup(buf);
}

char *tool_pending_take_refusal(pending_find_rc_t rc, turn_origin_rc_t orc, const char *what) {
   char buf[TOOL_PENDING_MSG_MAX];
   switch (rc) {
      case PENDING_EXPIRED:
         snprintf(buf, sizeof(buf),
                  TOOL_RESULT_ERROR_MARK "Error: confirmation expired. Please retry the %s.", what);
         break;
      case PENDING_OTHER_ITEM:
         snprintf(buf, sizeof(buf),
                  TOOL_RESULT_ERROR_MARK "Error: that pending_id isn't the %s waiting for a "
                                         "confirm (it may have been replaced). Use the id from "
                                         "the latest preview, or prepare it again.",
                  what);
         break;
      case PENDING_NOT_NOW:
         snprintf(buf, sizeof(buf), TOOL_RESULT_ERROR_MARK "Error: the %s wasn't confirmed: %s",
                  what, turn_origin_retry_hint(orc));
         break;
      default:
         snprintf(buf, sizeof(buf), TOOL_RESULT_ERROR_MARK "Error: no pending %s to confirm.",
                  what);
         break;
   }
   return strdup(buf);
}
