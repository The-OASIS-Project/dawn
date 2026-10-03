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
 * Whether one inbound message may speak for a linked channel (see
 * messaging_sender_gate.h).
 */
#include "messaging/messaging_sender_gate.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

messaging_sender_gate_result_t messaging_sender_gate(const char *owner_sender,
                                                     const char *sender_id,
                                                     messaging_chat_kind_t kind,
                                                     bool authenticates_sender) {
   const bool has_sender = authenticates_sender && sender_id && sender_id[0];
   if (owner_sender && owner_sender[0]) {
      return (has_sender && strcmp(owner_sender, sender_id) == 0) ? MESSAGING_SENDER_ALLOW
                                                                  : MESSAGING_SENDER_DENY;
   }
   if (kind == MESSAGING_CHAT_SHARED) {
      return MESSAGING_SENDER_RELINK;
   }
   if (!authenticates_sender) {
      return MESSAGING_SENDER_ALLOW; /* the chat is the number; no one to bind */
   }
   /* A provider that names senders but didn't name this one: refuse rather
    * than let an unknown sender in. */
   return has_sender ? MESSAGING_SENDER_BIND : MESSAGING_SENDER_DENY;
}
