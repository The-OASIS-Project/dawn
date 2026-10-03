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
 * Telegram: one `message` from getUpdates, read into what the engine needs,
 * with no sender when no single, Telegram-known person sent it.  Pure (JSON in,
 * struct out) so every case is testable without the network.
 */
#ifndef MESSAGING_TELEGRAM_PARSE_H
#define MESSAGING_TELEGRAM_PARSE_H

#include <json-c/json.h>
#include <stdbool.h>
#include <stdint.h>

#include "messaging/messaging_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
   char chat_id[32];        /**< chat.id as decimal */
   char sender_id[32];      /**< from.id as decimal; "" when no single person sent it */
   char sender_display[64]; /**< from.first_name, or "telegram_user" */
   const char *body;        /**< text; borrowed from the message object */
   int64_t timestamp;       /**< date (0 if absent) */
   messaging_chat_kind_t chat_kind;
} tg_message_t;

/**
 * @brief Read one Telegram `message` object.
 *
 * Dropped (returns false): no chat id or text, or not a private or group
 * chat (a channel).  Passed on with no sender (`sender_id` "", `why` naming
 * the reason): a message with no single, Telegram-known person behind it —
 * no `from`, a bot, a post in a chat's name (`sender_chat`: anonymous group
 * admins, linked-channel posts), an inline-bot result (`via_bot`), forwarded
 * content (`forward_origin`, `forward_from*`, `forward_date`,
 * `forward_sender_name`, `is_automatic_forward`), or a private chat whose
 * sender isn't the chat's own user.  The engine lets such a message speak
 * for no one; it still sees it so a link code it carries is used up.
 * Forwarded text is the owner's message but someone else's words, so it
 * must never count as the owner speaking.
 *
 * @param msg  The `message` object (not the whole update).
 * @param out  Filled on success.
 * @param why  Set to a short reason when the message is dropped or has no
 *             sender (else NULL).
 * @return true when the message should reach the engine.
 */
bool tg_parse_message(struct json_object *msg, tg_message_t *out, const char **why);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGING_TELEGRAM_PARSE_H */
