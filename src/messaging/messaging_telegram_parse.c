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
 * Telegram: reading one `message` for the engine (see
 * messaging_telegram_parse.h).
 */
#include "messaging/messaging_telegram_parse.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Telegram markers that make a message someone else's words or no single
 * person's: posted in a chat's name, sent through an inline bot, or
 * forwarded (current and older field names). */
static const char *const NO_PERSON_FIELDS[] = {
   "sender_chat",       "via_bot",      "forward_origin",      "forward_from",
   "forward_from_chat", "forward_date", "forward_sender_name", "is_automatic_forward",
};

static bool has_field(struct json_object *obj, const char *key) {
   struct json_object *v = NULL;
   return json_object_object_get_ex(obj, key, &v) && v && !json_object_is_type(v, json_type_null);
}

bool tg_parse_message(struct json_object *msg, tg_message_t *out, const char **why) {
   if (why) {
      *why = NULL;
   }
   if (!msg || !out) {
      return false;
   }
   memset(out, 0, sizeof(*out));

   struct json_object *chat = NULL;
   struct json_object *text = NULL;
   struct json_object *chat_id = NULL;
   if (!json_object_object_get_ex(msg, "chat", &chat) || !chat ||
       !json_object_object_get_ex(chat, "id", &chat_id) || !chat_id ||
       !json_object_object_get_ex(msg, "text", &text) || !text ||
       !json_object_is_type(text, json_type_string)) {
      return false; /* not a text message: nothing to answer */
   }

   struct json_object *type = NULL;
   const char *chat_type = "";
   if (json_object_object_get_ex(chat, "type", &type) && type) {
      chat_type = json_object_get_string(type);
   }
   if (strcmp(chat_type, "private") == 0) {
      out->chat_kind = MESSAGING_CHAT_ONE_TO_ONE;
   } else if (strcmp(chat_type, "group") == 0 || strcmp(chat_type, "supergroup") == 0) {
      out->chat_kind = MESSAGING_CHAT_SHARED;
   } else {
      if (why) {
         *why = "not a private or group chat";
      }
      return false;
   }

   int64_t chat_num = json_object_get_int64(chat_id);

   /* Find who sent it.  A message with no single, Telegram-known person
    * behind it is still passed on, with no sender: it speaks for no one,
    * and a link code it carries is used up rather than left for someone
    * else in the group to claim. */
   const char *no_person = NULL;
   for (size_t i = 0; i < sizeof(NO_PERSON_FIELDS) / sizeof(NO_PERSON_FIELDS[0]) && !no_person;
        i++) {
      if (has_field(msg, NO_PERSON_FIELDS[i])) {
         no_person = NO_PERSON_FIELDS[i];
      }
   }
   struct json_object *from = NULL;
   struct json_object *from_id = NULL;
   int64_t sender = 0;
   if (!no_person && (!json_object_object_get_ex(msg, "from", &from) || !from ||
                      !json_object_object_get_ex(from, "id", &from_id) || !from_id)) {
      no_person = "no sender";
   }
   struct json_object *is_bot = NULL;
   if (!no_person && json_object_object_get_ex(from, "is_bot", &is_bot) && is_bot &&
       json_object_get_boolean(is_bot)) {
      no_person = "sent by a bot";
   }
   if (!no_person) {
      sender = json_object_get_int64(from_id);
      if (sender <= 0) {
         no_person = "no sender";
      } else if (out->chat_kind == MESSAGING_CHAT_ONE_TO_ONE && sender != chat_num) {
         no_person = "private chat sender isn't the chat's user";
      }
   }
   if (why) {
      *why = no_person;
   }

   snprintf(out->chat_id, sizeof(out->chat_id), "%" PRId64, chat_num);
   if (!no_person) {
      snprintf(out->sender_id, sizeof(out->sender_id), "%" PRId64, sender);
   }

   struct json_object *fn = NULL;
   const char *name = NULL;
   if (!no_person && json_object_object_get_ex(from, "first_name", &fn) && fn) {
      name = json_object_get_string(fn);
   }
   snprintf(out->sender_display, sizeof(out->sender_display), "%s",
            (name && name[0]) ? name : "telegram_user");

   out->body = json_object_get_string(text);

   struct json_object *date = NULL;
   if (json_object_object_get_ex(msg, "date", &date) && date) {
      out->timestamp = json_object_get_int64(date);
   }
   return true;
}
