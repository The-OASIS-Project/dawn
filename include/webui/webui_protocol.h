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
 * The WebUI protocol's version and feature flags, advertised to clients on
 * connect (the `config` frame) and before one (/api/auth/status).  See
 * docs/WEBSOCKET_PROTOCOL.md, "Protocol version and feature flags".
 *
 * - A new or changed behaviour a client must know about gets a feature flag
 *   (a new optional field doesn't: clients detect it by its presence).
 * - WEBUI_PROTOCOL_VERSION is bumped only when something is removed or
 *   changes incompatibly, after a deprecation window.
 */

#ifndef WEBUI_PROTOCOL_H
#define WEBUI_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEBUI_PROTOCOL_VERSION 1

/* Room for webui_protocol_json_members()' output: raise it with the flags. */
#define WEBUI_PROTOCOL_JSON_MAX 256

/**
 * @brief The advertised fields as JSON object members (no braces):
 *        "protocol":N,"features":[...].
 * @return Bytes written (excluding the NUL), or 0 if @p size is too small
 *         (then @p out is "").
 */
size_t webui_protocol_json_members(char *out, size_t size);

struct json_object;

/**
 * @brief Log the client a connection says it is (`client` in its init or
 *        reconnect payload: name, version, protocol), once per connection.
 *        Untrusted text: bounded and reduced to printable ASCII.
 * @param noted The connection's once-flag.
 */
void webui_protocol_note_client(struct json_object *payload, bool *noted);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_PROTOCOL_H */
