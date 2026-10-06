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
 * The WebUI's entry points the rest of DAWN calls, for a build without the
 * WebUI: nothing to send to, whichever session calls.
 *
 * What goes here: an entry point declared in a WebUI header and called from
 * several modules.  A hook one module owns stays a weak default in that
 * module.
 */

#include <stdbool.h>
#include <stdint.h>

#include "core/session_manager.h"
#include "webui/webui_server.h"

void webui_tool_iteration_cb(struct session *session, void *userdata) {
   (void)session;
   (void)userdata;
}

void webui_send_thinking_start(struct session *session, const char *provider) {
   (void)session;
   (void)provider;
}

void webui_send_thinking_delta(struct session *session, const char *text) {
   (void)session;
   (void)text;
}

void webui_send_thinking_end(struct session *session, bool has_content) {
   (void)session;
   (void)has_content;
}

void webui_send_metrics_update(struct session *session,
                               const char *state,
                               int ttft_ms,
                               float token_rate,
                               int context_percent) {
   (void)session;
   (void)state;
   (void)ttft_ms;
   (void)token_rate;
   (void)context_percent;
}

void webui_send_state_with_detail(struct session *session, const char *state, const char *detail) {
   (void)session;
   (void)state;
   (void)detail;
}

void webui_send_session_json(struct session *session, const char *json_str) {
   (void)session;
   (void)json_str;
}

void webui_broadcast_conversation_messages_appended(int user_id, int64_t conv_id) {
   (void)user_id;
   (void)conv_id;
}

void satellite_send_response(struct session *session, const char *text) {
   (void)session;
   (void)text;
}

bool webui_session_tts_enabled(struct session *session) {
   (void)session;
   return false;
}

void webui_broadcast_context_injection(int user_id,
                                       int64_t conv_id,
                                       int64_t turn_id,
                                       const focus_compose_result_t *result,
                                       const char *const *states) {
   (void)user_id;
   (void)conv_id;
   (void)turn_id;
   (void)result;
   (void)states;
}
