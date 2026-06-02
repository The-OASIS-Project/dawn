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
 * Admin socket internal API — shared between admin_socket.c and its
 * per-feature sibling files (admin_socket_memory_entity.c, etc.).  Not part
 * of the public admin_socket.h surface; do not include from outside the
 * src/auth/admin_socket*.c family.
 */

#ifndef DAWN_AUTH_ADMIN_SOCKET_INTERNAL_H
#define DAWN_AUTH_ADMIN_SOCKET_INTERNAL_H

/* Security guard — only admin_socket modules should include this. */
#ifndef ADMIN_SOCKET_INTERNAL_ALLOWED
#error "admin_socket_internal.h is an internal header - include auth/admin_socket.h instead"
#endif

#include <stdint.h>

#include "auth/admin_socket.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Wire-level reply helpers shared across admin handler modules.  Defined in
 * admin_socket.c. */
int send_response(int client_fd, admin_resp_code_t code);
int send_text_response(int client_fd, admin_resp_code_t code, const char *text);

/* Phase 6.5 entity-merge handlers (admin_socket_memory_entity.c).  Dispatched
 * from handle_client() in admin_socket.c against ADMIN_MSG_MEMORY_ENTITY_*
 * opcodes 0x83-0x88. */
int handle_memory_entity_merge(int client_fd, const char *payload, uint16_t payload_len);
int handle_memory_entity_split(int client_fd, const char *payload, uint16_t payload_len);
int handle_memory_entity_aliases(int client_fd, const char *payload, uint16_t payload_len);
int handle_memory_entity_history(int client_fd, const char *payload, uint16_t payload_len);
int handle_memory_entity_list(int client_fd, const char *payload, uint16_t payload_len);
int handle_memory_entity_link_user_self(int client_fd, const char *payload, uint16_t payload_len);

/* Memory-maintenance handlers (admin_socket_memory.c).  Dispatched from
 * handle_client() in admin_socket.c against ADMIN_MSG_MEMORY_* opcodes. */
int handle_memory_recategorize(int client_fd, const char *payload, uint16_t payload_len);
int handle_memory_cleanup_meta_facts(int client_fd, const char *payload, uint16_t payload_len);
int handle_memory_summarize_missing(int client_fd, const char *payload, uint16_t payload_len);
int handle_memory_backfill_note_glosses(int client_fd, const char *payload, uint16_t payload_len);
int handle_memory_rebuild_document_fts(int client_fd, const char *payload, uint16_t payload_len);
int handle_memory_reextract(int client_fd, const char *payload, uint16_t payload_len);
int handle_memory_reextract_status(int client_fd, const char *payload, uint16_t payload_len);

/* MCP bridge handlers (admin_socket_mcp.c).  Dispatched from handle_client()
 * against ADMIN_MSG_MCP_* opcodes 0xB0-0xB4.  Compiled only when the MCP bridge
 * tool is enabled. */
#ifdef DAWN_ENABLE_MCP_BRIDGE_TOOL
int handle_mcp_list(int client_fd, const char *payload, uint16_t payload_len);
int handle_mcp_status(int client_fd, const char *payload, uint16_t payload_len);
int handle_mcp_grant(int client_fd, const char *payload, uint16_t payload_len);
int handle_mcp_revoke(int client_fd, const char *payload, uint16_t payload_len);
int handle_mcp_reset(int client_fd, const char *payload, uint16_t payload_len);
#endif

/* Code-projects handlers (admin_socket_code_project.c), opcodes 0xB5-0xB8. */
#ifdef DAWN_ENABLE_CODE_PROJECTS
int handle_code_proj_list(int client_fd, const char *payload, uint16_t payload_len);
int handle_code_proj_import(int client_fd, const char *payload, uint16_t payload_len);
int handle_code_proj_refresh(int client_fd, const char *payload, uint16_t payload_len);
int handle_code_proj_delete(int client_fd, const char *payload, uint16_t payload_len);
#endif

/* Messaging-channels handlers (admin_socket_messaging.c).  Dispatched from
 * handle_client() in admin_socket.c against ADMIN_MSG_MESSAGING_* opcodes. */
int handle_messaging_generate_link_code(int client_fd, const char *payload, uint16_t payload_len);
int handle_messaging_list_channels(int client_fd, const char *payload, uint16_t payload_len);
int handle_messaging_unlink_channel(int client_fd, const char *payload, uint16_t payload_len);
int handle_messaging_link_attempts(int client_fd, const char *payload, uint16_t payload_len);
int handle_messaging_reenable_channel(int client_fd, const char *payload, uint16_t payload_len);

/* OTA operator handlers (admin_socket_ota.c).  Dispatched from handle_client()
 * in admin_socket.c against ADMIN_MSG_OTA_* opcodes (0xC0-0xC5). */
int handle_ota_list_cmd(int client_fd);
int handle_ota_push_cmd(int client_fd, const char *payload, uint16_t payload_len);
int handle_ota_rescan_cmd(int client_fd);
int handle_ota_push_all_cmd(int client_fd, const char *payload, uint16_t payload_len);
int handle_ota_rollout_status_cmd(int client_fd);
int handle_ota_rollout_abort_cmd(int client_fd);

#ifdef __cplusplus
}
#endif

#endif /* DAWN_AUTH_ADMIN_SOCKET_INTERNAL_H */
