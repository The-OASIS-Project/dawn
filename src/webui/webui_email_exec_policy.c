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
 * Admission for the email executor: a list replaces the one before it (the
 * client only wants the newest), a read waits behind the open one, the rest
 * run one at a time.  A user's requests across all sessions are capped.
 */

#include "webui/webui_email_exec_policy.h"

email_exec_admit_t email_exec_admit(email_exec_slot_t slot,
                                    bool has_running,
                                    bool has_waiting,
                                    int user_live) {
   email_exec_admit_t a = { .action = EMAIL_EXEC_REFUSE };
   int live_after = user_live;

   switch (slot) {
      case EMAIL_EXEC_SLOT_LIST:
         /* What the client asked for before is no longer wanted.  A replaced
          * waiting list never ran and frees its place; a cancelled running one
          * keeps it until its tasks drain (the caller counts those). */
         a.cancel_running = has_running;
         a.replace_waiting = has_waiting;
         live_after -= has_waiting ? 1 : 0;
         a.action = EMAIL_EXEC_RUN;
         break;
      case EMAIL_EXEC_SLOT_READ:
         /* The open read finishes (it may already have marked the message
          * read); a waiting one hasn't touched anything, so the newer wins. */
         a.replace_waiting = has_waiting;
         live_after -= has_waiting ? 1 : 0;
         a.action = has_running ? EMAIL_EXEC_WAIT : EMAIL_EXEC_RUN;
         break;
      default:
         if (has_running)
            return a;
         a.action = EMAIL_EXEC_RUN;
         break;
   }

   if (live_after >= EMAIL_EXEC_USER_LIVE_MAX) {
      email_exec_admit_t refuse = { .action = EMAIL_EXEC_REFUSE };
      return refuse;
   }
   return a;
}
