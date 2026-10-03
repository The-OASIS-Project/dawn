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
 * Whether one inbound message may speak for a linked channel.  A channel
 * belongs to the person who linked it (owner_sender, the provider's id for
 * them); in a one-to-one chat the first message from that person names them.
 * Pure, so the rule is testable on its own.
 */
#ifndef MESSAGING_SENDER_GATE_H
#define MESSAGING_SENDER_GATE_H

#include <stdbool.h>

#include "messaging/messaging_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   MESSAGING_SENDER_ALLOW = 0, /**< The owner, or a provider that names no sender (SMS) */
   MESSAGING_SENDER_BIND,      /**< One-to-one chat, owner not known yet: record this sender */
   MESSAGING_SENDER_DENY,      /**< Someone other than the owner, or no sender where one is due */
   MESSAGING_SENDER_RELINK,    /**< A shared chat whose owner was never recorded */
} messaging_sender_gate_result_t;

/**
 * @param owner_sender          The channel row's owner (NULL or "" = not known).
 * @param sender_id             The provider's id for who sent this message (NULL = none).
 * @param kind                  One-to-one or shared chat.
 * @param authenticates_sender  The provider vouches for who sent each message
 *                              (chat apps).  False for SMS, whose sender number
 *                              is the chat itself and can be forged: its
 *                              sender id is ignored and nothing is bound.
 */
messaging_sender_gate_result_t messaging_sender_gate(const char *owner_sender,
                                                     const char *sender_id,
                                                     messaging_chat_kind_t kind,
                                                     bool authenticates_sender);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGING_SENDER_GATE_H */
