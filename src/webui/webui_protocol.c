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
 * The WebUI protocol's version and feature flags (webui_protocol.h).
 */

#include "webui/webui_protocol.h"

#include <json-c/json.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "logging.h"

/* The feature flags, one line in docs/WEBSOCKET_PROTOCOL.md each.  Names are
 * stable snake_case; a flag is never removed while the protocol version
 * stands. */
static const char *const s_features[] = {
   /* POST /api/auth/logout with the socket open is safe: force_logout, then
    * close 4002 on the login's connections, the music socket closed by the
    * server, the reply at once with the cookie cleared. */
   "logout_closes_sockets",
   /* One login per app: login takes "app", every other request names its app
    * with ?app=, and each app's login lives in its own cookie
    * (dawn_session_<app>), so apps open in one browser don't share a login. */
   "app_logins",
   /* A text turn takes images only by id (image_ids); images[] is ignored.  A
    * bad, missing or foreign id, or too many, refuses the whole turn with
    * IMAGE_UNAVAILABLE / IMAGE_LIMIT / IMAGE_ERROR and saves nothing. */
   "image_turns_by_id",
   /* A text turn with no words but image_ids runs with just the images; one
    * with neither is refused with EMPTY_MESSAGE instead of dropped. */
   "image_only_turns",
   /* A text frame may carry client_ref; that turn's user transcript echo and
    * every error raised for it carry it back unchanged. */
   "turn_refs",
};

size_t webui_protocol_json_members(char *out, size_t size) {
   if (!out || size == 0) {
      return 0;
   }
   size_t len = 0;
   int n = snprintf(out, size, "\"protocol\":%d,\"features\":[", WEBUI_PROTOCOL_VERSION);
   if (n < 0 || (size_t)n >= size) {
      out[0] = '\0';
      return 0;
   }
   len = (size_t)n;
   for (size_t i = 0; i < sizeof(s_features) / sizeof(s_features[0]); i++) {
      n = snprintf(out + len, size - len, "%s\"%s\"", i ? "," : "", s_features[i]);
      if (n < 0 || (size_t)n >= size - len) {
         out[0] = '\0';
         return 0;
      }
      len += (size_t)n;
   }
   if (len + 2 > size) {
      out[0] = '\0';
      return 0;
   }
   out[len++] = ']';
   out[len] = '\0';
   return len;
}

/* A client-supplied string for the log: at most out_size - 1 printable ASCII
 * characters, anything else as '?'. */
static void printable(const char *in, char *out, size_t out_size) {
   size_t i = 0;
   for (; in && in[i] && i + 1 < out_size; i++) {
      const unsigned char c = (unsigned char)in[i];
      out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
   }
   out[i] = '\0';
}

void webui_protocol_note_client(struct json_object *payload, bool *noted) {
   if (!payload || !noted || *noted) {
      return;
   }
   struct json_object *client = NULL;
   if (!json_object_object_get_ex(payload, "client", &client) ||
       !json_object_is_type(client, json_type_object)) {
      return;
   }
   *noted = true;
   struct json_object *field = NULL;
   char name[48] = "?";
   char version[32] = "?";
   int protocol = 0;
   if (json_object_object_get_ex(client, "name", &field)) {
      printable(json_object_get_string(field), name, sizeof(name));
   }
   if (json_object_object_get_ex(client, "version", &field)) {
      printable(json_object_get_string(field), version, sizeof(version));
   }
   if (json_object_object_get_ex(client, "protocol", &field)) {
      protocol = json_object_get_int(field);
   }
   if (protocol != WEBUI_PROTOCOL_VERSION) {
      OLOG_WARNING("WebUI: client %s %s speaks protocol %d; this server speaks %d", name, version,
                   protocol, WEBUI_PROTOCOL_VERSION);
   } else {
      OLOG_INFO("WebUI: client %s %s (protocol %d)", name, version, protocol);
   }
}
