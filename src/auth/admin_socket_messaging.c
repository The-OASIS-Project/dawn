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
 * Messaging-channels admin opcode handlers.  Sibling of
 * admin_socket_memory.c.  Currently hosts the operator path to issue
 * link codes; Phase 6 will add list-channels / unbind / link-attempts
 * here too.
 */

#define ADMIN_SOCKET_INTERNAL_ALLOWED
#define AUTH_DB_INTERNAL_ALLOWED

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/admin_socket_internal.h"
#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "dawn_error.h"
#include "logging.h"
#include "messaging/messaging_engine.h"

/* =============================================================================
 * Messaging: generate-link-code
 *
 * Payload wire format (matches the design-doc-style admin layouts):
 *   Byte 0:        provider_hint_len  (0..ADMIN_MESSAGING_PROVIDER_HINT_MAX)
 *   Byte 1..1+H:   provider_hint      ("slack" / "telegram" / etc., no NUL)
 *   Byte 1+H..:    username           (no NUL, length = payload_len - 1 - H)
 *
 * Response: text-style.  On success, the body is the issued code on the
 * first line plus a "Expires in N seconds" hint on the second.
 * ============================================================================= */

int handle_messaging_generate_link_code(int client_fd, const char *payload, uint16_t payload_len) {
   /* Top-bound check: hint (≤16) + username (<32) + 1 prefix byte = 48 max.
    * Below the wire-format minimum (1 + 1 = 2) is also a hard reject.
    * Bounds-checking the total up front makes the parse chain trivially
    * sound without depending on per-field bounds executing in order. */
   if (payload_len < 2 ||
       payload_len > 1 + ADMIN_MESSAGING_PROVIDER_HINT_MAX + ADMIN_MESSAGING_USERNAME_MAX) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                "Invalid generate-link-code payload");
   }
   uint8_t hint_len = (uint8_t)payload[0];
   if (hint_len > ADMIN_MESSAGING_PROVIDER_HINT_MAX) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "provider_hint too long");
   }
   if ((size_t)1 + hint_len >= payload_len) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Missing username");
   }

   char hint[ADMIN_MESSAGING_PROVIDER_HINT_MAX + 1] = { 0 };
   if (hint_len > 0) {
      memcpy(hint, payload + 1, hint_len);
      /* Sanity-check the hint against the engine's known driver names so
       * a fat-fingered subcommand doesn't pollute the DB hint column.
       * The engine accepts any string; this is operator-friendliness. */
      if (!messaging_engine_provider_known(hint)) {
         return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                   "Unknown provider hint (use telegram|discord|slack|sms)");
      }
   }

   uint16_t uname_len = (uint16_t)(payload_len - 1 - hint_len);
   if (uname_len == 0 || uname_len >= ADMIN_MESSAGING_USERNAME_MAX) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid username");
   }
   char username[ADMIN_MESSAGING_USERNAME_MAX];
   memcpy(username, payload + 1 + hint_len, uname_len);
   username[uname_len] = '\0';

   auth_user_t user;
   if (auth_db_get_user(username, &user) != AUTH_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "User not found");
   }

   char code[MESSAGING_LINK_CODE_BUF_SIZE];
   const char *hint_arg = (hint_len > 0) ? hint : NULL;
   int rc = messaging_engine_generate_link_code(user.id, hint_arg, code, sizeof(code));
   if (rc == MESSAGING_RATE_LIMITED) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                "The user holds several unused link codes; wait 10 minutes");
   }
   if (rc != MESSAGING_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR,
                                "messaging_engine_generate_link_code failed");
   }

   /* Slack intercepts any '/' as a slash command, so Slack users must
    * send the slashless form ("link CODE").  All other providers accept
    * either form; the slashed form stays the documented default for
    * Telegram/Discord/SMS discoverability. */
   const bool slack_hint = (hint_arg && strcmp(hint_arg, "slack") == 0);
   const char *prefix = slack_hint ? "link" : "/link";
   char msg[256];
   if (hint_arg) {
      snprintf(msg, sizeof(msg),
               "Link code for user '%s' (%s): %s\n"
               "Expires in %d seconds.  Send '%s %s' from the linked %s client to claim.",
               username, hint_arg, code, MESSAGING_LINK_TTL_SECONDS, prefix, code, hint_arg);
   } else {
      snprintf(msg, sizeof(msg),
               "Link code for user '%s': %s\n"
               "Expires in %d seconds.  Send '/link %s' (or 'link %s' on Slack) from the desired "
               "chat client to claim.",
               username, code, MESSAGING_LINK_TTL_SECONDS, code, code);
   }
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
}

/* =============================================================================
 * Messaging: list-channels (Phase 6)
 *
 * Payload: raw username (1..ADMIN_MESSAGING_USERNAME_MAX bytes, no NUL).
 * Response: aligned text table (operator surface; the WebUI panel uses the
 * JSON form, messaging_engine_list_channels_json, directly).
 * ============================================================================= */

int handle_messaging_list_channels(int client_fd, const char *payload, uint16_t payload_len) {
   if (payload_len == 0 || payload_len >= ADMIN_MESSAGING_USERNAME_MAX) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid username");
   }
   char username[ADMIN_MESSAGING_USERNAME_MAX];
   memcpy(username, payload, payload_len);
   username[payload_len] = '\0';

   auth_user_t user;
   if (auth_db_get_user(username, &user) != AUTH_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_NOT_FOUND, "User not found");
   }

   /* Operator surface gets the aligned text table; the WebUI panel uses
    * the JSON form (messaging_engine_list_channels_json) instead. */
   char buf[ADMIN_MSG_CONTENT_MAX + 1];
   if (messaging_engine_list_channels_text(user.id, buf, sizeof(buf)) != MESSAGING_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "list_channels failed");
   }
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, buf);
}

/* =============================================================================
 * Messaging: unlink-channel (Phase 6)
 *
 * Wire format:
 *   Byte 0:        username_len (1..ADMIN_MESSAGING_USERNAME_MAX)
 *   Byte 1..1+U:   username
 *   Byte 1+U..:    display_name (the channel to soft-delete)
 * ============================================================================= */

int handle_messaging_unlink_channel(int client_fd, const char *payload, uint16_t payload_len) {
   if (payload_len < 3) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid unlink payload");
   }
   uint8_t uname_len = (uint8_t)payload[0];
   if (uname_len == 0 || uname_len >= ADMIN_MESSAGING_USERNAME_MAX ||
       (size_t)1 + uname_len >= payload_len) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid username");
   }
   char username[ADMIN_MESSAGING_USERNAME_MAX];
   memcpy(username, payload + 1, uname_len);
   username[uname_len] = '\0';

   uint16_t name_len = (uint16_t)(payload_len - 1 - uname_len);
   if (name_len == 0 || name_len >= ADMIN_MESSAGING_DISPLAY_NAME_MAX) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid channel name");
   }
   char display_name[ADMIN_MESSAGING_DISPLAY_NAME_MAX];
   memcpy(display_name, payload + 1 + uname_len, name_len);
   display_name[name_len] = '\0';

   auth_user_t user;
   if (auth_db_get_user(username, &user) != AUTH_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_NOT_FOUND, "User not found");
   }

   int rc = messaging_engine_unlink_channel(user.id, display_name);
   if (rc == MESSAGING_UNKNOWN_CHANNEL) {
      return send_text_response(client_fd, ADMIN_RESP_NOT_FOUND,
                                "No linked channel by that name for the user");
   }
   if (rc != MESSAGING_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "unlink failed");
   }
   char msg[128];
   snprintf(msg, sizeof(msg), "Unlinked channel '%s' for user '%s'.", display_name, username);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
}

/* =============================================================================
 * Messaging: link-attempts (Phase 6) — operator abuse review
 *
 * Wire format:
 *   Byte 0:           provider_len (0..ADMIN_MESSAGING_PROVIDER_HINT_MAX; 0 = all)
 *   Byte 1..1+P:      provider     (may be empty)
 *   Byte 1+P..1+P+2 (optional): limit, uint16 little-endian (0/absent = default)
 * ============================================================================= */

int handle_messaging_link_attempts(int client_fd, const char *payload, uint16_t payload_len) {
   if (payload_len < 1) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid link-attempts payload");
   }
   uint8_t prov_len = (uint8_t)payload[0];
   if (prov_len > ADMIN_MESSAGING_PROVIDER_HINT_MAX || (size_t)1 + prov_len > payload_len) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid provider filter");
   }
   char provider[ADMIN_MESSAGING_PROVIDER_HINT_MAX + 1] = { 0 };
   if (prov_len > 0) {
      memcpy(provider, payload + 1, prov_len);
   }

   int limit = 0; /* 0 → engine default (50) */
   size_t off = (size_t)1 + prov_len;
   if (payload_len - off >= 2) {
      limit = (uint8_t)payload[off] | ((uint8_t)payload[off + 1] << 8);
   }

   char buf[ADMIN_MSG_CONTENT_MAX + 1];
   int rc = messaging_engine_link_attempts_text((prov_len > 0) ? provider : NULL, 0, limit, buf,
                                                sizeof(buf));
   if (rc != MESSAGING_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "link-attempts query failed");
   }
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, buf);
}

/* =============================================================================
 * Messaging: reenable-channel (Phase 6) — inverse of unlink
 *
 * Wire format identical to unlink:
 *   Byte 0:        username_len (1..ADMIN_MESSAGING_USERNAME_MAX)
 *   Byte 1..1+U:   username
 *   Byte 1+U..:    display_name (the soft-deleted channel to re-enable)
 * ============================================================================= */

int handle_messaging_reenable_channel(int client_fd, const char *payload, uint16_t payload_len) {
   if (payload_len < 3) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid reenable payload");
   }
   uint8_t uname_len = (uint8_t)payload[0];
   if (uname_len == 0 || uname_len >= ADMIN_MESSAGING_USERNAME_MAX ||
       (size_t)1 + uname_len >= payload_len) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid username");
   }
   char username[ADMIN_MESSAGING_USERNAME_MAX];
   memcpy(username, payload + 1, uname_len);
   username[uname_len] = '\0';

   uint16_t name_len = (uint16_t)(payload_len - 1 - uname_len);
   if (name_len == 0 || name_len >= ADMIN_MESSAGING_DISPLAY_NAME_MAX) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid channel name");
   }
   char display_name[ADMIN_MESSAGING_DISPLAY_NAME_MAX];
   memcpy(display_name, payload + 1 + uname_len, name_len);
   display_name[name_len] = '\0';

   auth_user_t user;
   if (auth_db_get_user(username, &user) != AUTH_DB_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_NOT_FOUND, "User not found");
   }

   int rc = messaging_engine_reenable_channel(user.id, display_name);
   if (rc == MESSAGING_UNKNOWN_CHANNEL) {
      return send_text_response(client_fd, ADMIN_RESP_NOT_FOUND,
                                "No unlinked channel by that name for the user");
   }
   if (rc == MESSAGING_NAME_TAKEN) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                "An enabled channel with that name already exists");
   }
   if (rc == MESSAGING_ALREADY_LINKED) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                "That chat is linked to another DAWN account for the same person");
   }
   if (rc == MESSAGING_NOT_VERIFIED) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE,
                                "This number needs linking again (/link with a new code)");
   }
   if (rc != MESSAGING_SUCCESS) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "reenable failed");
   }
   char msg[ADMIN_MESSAGING_DISPLAY_NAME_MAX + ADMIN_MESSAGING_USERNAME_MAX + 48];
   snprintf(msg, sizeof(msg), "Re-enabled channel '%s' for user '%s'.", display_name, username);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, msg);
}
