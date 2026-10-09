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
 * The WebUI's half of email_changed_notify: what a move or undo changed goes to
 * every browser tab of the user, the one that asked included (a tab applies it
 * idempotently, and one whose request was cancelled learns it only this way).
 */

#include "tools/email_service.h"
#include "webui/email_wire.h"
#include "webui/webui_internal.h"

void email_changed_notify(int user_id,
                          int64_t account_id,
                          bool gmail_api,
                          email_move_kind_t kind,
                          bool undo,
                          const email_summary_t *created,
                          int nc,
                          const char *const *destroyed,
                          int nd,
                          bool refresh) {
   if (user_id <= 0 || (nc <= 0 && nd <= 0 && !refresh))
      return;
   /* Called with no mutex held (the account's lease may be): the broadcast
    * takes the connection registry's lock and only queues. */
   json_object *frame = email_wire_changed(account_id, gmail_api, kind, undo, created, nc,
                                           destroyed, nd, refresh);
   if (frame)
      webui_broadcast_json_to_user(user_id, frame, true);
}
