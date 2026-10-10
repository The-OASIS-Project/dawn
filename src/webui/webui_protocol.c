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
#include "utils/string_utils.h"
#include "webui/webui_email_client.h"

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
   /* A text frame may carry its documents as `attachments`; the daemon defuses
    * each body and builds the inlined form itself, so a document can't end its
    * own span. */
   "document_attachments",
   /* A text frame may carry from_visual: true for a prompt sent by a rendered
    * visual; no confirm counts in that turn.  A client sends a visual's prompt
    * only to a daemon advertising this, never as an ordinary turn. */
   "visual_prompt_guard",
};

/* The email panel's check (webui_email_panel.c); a build without the panel has
 * none, and the flag isn't advertised. */
#pragma weak webui_email_client_enabled

/* Flags that depend on how the daemon runs, each with its check.  Same rules
 * as s_features. */
static const struct {
   const char *name;
   bool (*on)(void);
} s_runtime_features[] = {
   /* The mail panel's verbs answer: email_list, email_search, email_read,
    * email_set_flags, email_unread_counts (docs/WEBSOCKET_PROTOCOL.md, Email). */
   { "email_client", webui_email_client_enabled },
   /* A text turn may carry email_refs (one email the user attached, by account
    * and message id); the model reads it with the email tool, and the question
    * row comes back with email_ref (docs/WEBSOCKET_PROTOCOL.md, Email). */
   { "email_refs", webui_email_client_enabled },
};

static bool append_feature(char *out, size_t size, size_t *len, bool first, const char *name) {
   const int n = snprintf(out + *len, size - *len, "%s\"%s\"", first ? "" : ",", name);
   if (n < 0 || (size_t)n >= size - *len)
      return false;
   *len += (size_t)n;
   return true;
}

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
   bool first = true;
   for (size_t i = 0; i < sizeof(s_features) / sizeof(s_features[0]); i++) {
      if (!append_feature(out, size, &len, first, s_features[i])) {
         out[0] = '\0';
         return 0;
      }
      first = false;
   }
   for (size_t i = 0; i < sizeof(s_runtime_features) / sizeof(s_runtime_features[0]); i++) {
      if (!s_runtime_features[i].on || !s_runtime_features[i].on())
         continue;
      if (!append_feature(out, size, &len, first, s_runtime_features[i].name)) {
         out[0] = '\0';
         return 0;
      }
      first = false;
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

/* A client string safe to echo back: at most max bytes, no control characters,
 * valid UTF-8 (one bad byte closes the client's socket). */
static bool echo_ok(const char *s, size_t len, size_t max) {
   if (!s || len > max)
      return false;
   for (size_t i = 0; i < len; i++) {
      const unsigned char c = (unsigned char)s[i];
      if (c < 0x20 || c == 0x7f)
         return false;
   }
   return utf8_is_valid(s, len);
}

bool webui_protocol_echo_ok(const char *s) {
   return s && echo_ok(s, strlen(s), WEBUI_REQ_MAX);
}

bool webui_protocol_payload_req(struct json_object *payload,
                                size_t max,
                                char *out,
                                size_t out_size) {
   struct json_object *req_obj;
   if (!payload || !out || out_size == 0 || !json_object_object_get_ex(payload, "req", &req_obj) ||
       !json_object_is_type(req_obj, json_type_string))
      return false;
   const char *req = json_object_get_string(req_obj);
   const size_t len = (size_t)json_object_get_string_len(req_obj);
   /* strlen too: an embedded NUL would cut the echo short of what was checked */
   if (!req || strlen(req) != len || len >= out_size || !echo_ok(req, len, max))
      return false;
   memcpy(out, req, len + 1);
   return true;
}

char *webui_protocol_unknown_type_json(const char *type, const char *req) {
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();
   if (!obj || !payload) {
      json_object_put(obj);
      json_object_put(payload);
      return NULL;
   }
   json_object_object_add(payload, "code", json_object_new_string("UNKNOWN_TYPE"));
   json_object_object_add(payload, "message",
                          json_object_new_string("This server doesn't handle that message type"));
   json_object_object_add(payload, "severity", json_object_new_string("error"));
   json_object_object_add(payload, "recoverable", json_object_new_boolean(1));
   if (webui_protocol_echo_ok(type))
      json_object_object_add(payload, "request_type", json_object_new_string(type));
   if (webui_protocol_echo_ok(req))
      json_object_object_add(payload, "req", json_object_new_string(req));
   json_object_object_add(obj, "type", json_object_new_string("error"));
   json_object_object_add(obj, "payload", payload);
   const char *json = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
   char *out = json ? strdup(json) : NULL;
   json_object_put(obj);
   return out;
}
