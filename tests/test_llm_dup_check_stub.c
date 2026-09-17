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
 * Link-time stubs for test_llm_dup_check.  llm_tools.c pulls in a wide dep
 * surface (command executor, session, tool registry, webui, worker pool) that
 * the duplicate-call code path never reaches — these provide the symbols so the
 * object links.  Only the two tool-registry functions on the dup-check path
 * have meaningful returns: get_action_param_name() returns NULL so the
 * non-deterministic-action exemption is skipped, leaving the pure history scan
 * under test.  Bare signatures (linker matches by name); none but those two are
 * ever called.
 */

#include <json-c/json.h>
#include <stdbool.h>
#include <stddef.h>

#include "config/dawn_config.h"

dawn_config_t g_config;
/* llm_tools.c's per-tool advertisement gates read g_secrets (e.g. the stocks
 * tool checks schwab_client_id); zero-init leaves those tools "not configured". */
secrets_config_t g_secrets;

/* --- on the dup-check path: real returns --- */
const char *tool_registry_get_action_param_name(const char *tool_name) {
   (void)tool_name;
   return NULL; /* no action param → skip the repeatable-action exemption */
}
bool tool_registry_action_is_repeatable(const char *tool_name, const char *action) {
   (void)tool_name;
   (void)action;
   return false;
}

/* --- symbol providers only (never reached by the dup-check path) --- */
int command_execute(void) {
   return 1;
}
int command_execute_sync(void) {
   return 1;
}
int command_execute_mqtt_direct(void) {
   return 1;
}
void cmd_exec_result_free(void *result) {
   (void)result;
}
bool component_status_is_hud_online(void) {
   return false;
}
const secrets_config_t *config_get_secrets(void) {
   return NULL;
}
int is_vision_enabled_for_current_llm(void) {
   return 0;
}
int llm_context_get_size(void) {
   return 0;
}
int llm_get_type(void) {
   return 0;
}
/* Pulled in by the new capture-image persistence helpers in llm_tools.c
 * (build_openai_capture_image_message/build_claude_capture_image_message) —
 * not on the dup-check path under test. */
const char *llm_claude_detect_image_mime_type(const char *base64_data) {
   (void)base64_data;
   return "image/jpeg";
}
json_object *llm_claude_create_image_block(const char *vision_image) {
   (void)vision_image;
   return NULL;
}
char *ocp_base64_encode(void) {
   return NULL;
}
void *session_get_command_context(void) {
   return NULL;
}
void session_set_command_context(void *session) {
   (void)session;
}
/* strcasestr_portable is NOT stubbed here: llm_tools.c now references
 * sanitize_utf8_for_json (tool-description hardening), which pulls the real
 * string_utils.c from dawn_common into the link — so its strcasestr_portable is
 * available and a stub would multiply-define it. */
const void *tool_registry_find(const char *name) {
   (void)name;
   return NULL;
}
const void *tool_registry_lookup(const char *name) {
   (void)name;
   return NULL;
}
void tool_registry_foreach(void *cb, void *user_data) {
   (void)cb;
   (void)user_data;
}
const void *tool_registry_get_effective_param(const char *tool_name, int idx) {
   (void)tool_name;
   (void)idx;
   return NULL;
}
const char *tool_registry_resolve_device(const void *meta, const char *key) {
   (void)meta;
   (void)key;
   return NULL;
}
void webui_send_state_with_detail(void *session, const char *state, const char *detail) {
   (void)session;
   (void)state;
   (void)detail;
}
void *worker_pool_get_mosq(void) {
   return NULL;
}
