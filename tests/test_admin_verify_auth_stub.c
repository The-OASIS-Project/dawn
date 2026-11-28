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
 * Link stubs for test_admin_verify_auth: the admin socket's command handlers
 * in other files and the subsystems they reach.  The test calls only
 * verify_admin_auth(), so none of these runs.
 */

#define ADMIN_SOCKET_INTERNAL_ALLOWED

#include <stdbool.h>
#include <stdint.h>

#include "audio/music_queue_db.h"
#include "auth/admin_socket_internal.h"
#include "core/conv_images.h"
#include "core/session_prefix.h"
#include "dawn_error.h"
#include "webui/webui_server.h"

#define STUB_HANDLER(name)                                              \
   int name(int client_fd, const char *payload, uint16_t payload_len);  \
   int name(int client_fd, const char *payload, uint16_t payload_len) { \
      (void)client_fd;                                                  \
      (void)payload;                                                    \
      (void)payload_len;                                                \
      return FAILURE;                                                   \
   }

#define STUB_FD_HANDLER(name) \
   int name(int client_fd);   \
   int name(int client_fd) {  \
      (void)client_fd;        \
      return FAILURE;         \
   }

STUB_HANDLER(admin_handle_music_list)
STUB_HANDLER(admin_handle_music_search)
STUB_FD_HANDLER(admin_handle_music_rescan)
STUB_FD_HANDLER(admin_handle_music_stats)
STUB_HANDLER(handle_cache_stats_cmd)
STUB_HANDLER(handle_llm_capture_cmd)
STUB_HANDLER(handle_memory_backfill_note_glosses)
STUB_HANDLER(handle_memory_cleanup_meta_facts)
STUB_HANDLER(handle_memory_entity_aliases)
STUB_HANDLER(handle_memory_entity_history)
STUB_HANDLER(handle_memory_entity_link_user_self)
STUB_HANDLER(handle_memory_entity_list)
STUB_HANDLER(handle_memory_entity_merge)
STUB_HANDLER(handle_memory_entity_split)
STUB_HANDLER(handle_memory_extract_range)
STUB_HANDLER(handle_memory_fact_restore)
STUB_HANDLER(handle_memory_fact_superseded)
STUB_HANDLER(handle_memory_rebuild_document_fts)
STUB_HANDLER(handle_memory_reextract)
STUB_HANDLER(handle_memory_reextract_status)
STUB_HANDLER(handle_memory_summarize_missing)
STUB_HANDLER(handle_messaging_generate_link_code)
STUB_HANDLER(handle_messaging_link_attempts)
STUB_HANDLER(handle_messaging_list_channels)
STUB_HANDLER(handle_messaging_reenable_channel)
STUB_HANDLER(handle_messaging_unlink_channel)
STUB_FD_HANDLER(handle_ota_list_cmd)
STUB_HANDLER(handle_ota_push_all_cmd)
STUB_HANDLER(handle_ota_push_cmd)
STUB_FD_HANDLER(handle_ota_rescan_cmd)
STUB_FD_HANDLER(handle_ota_rollout_abort_cmd)
STUB_FD_HANDLER(handle_ota_rollout_status_cmd)
STUB_HANDLER(handle_research_cancel_cmd)
STUB_HANDLER(handle_research_start_cmd)
STUB_HANDLER(handle_research_status_cmd)
STUB_HANDLER(handle_schwab_auth_complete_cmd)
STUB_HANDLER(handle_schwab_auth_url_cmd)
STUB_HANDLER(handle_schwab_status_cmd)
STUB_HANDLER(handle_mcp_list)
STUB_HANDLER(handle_mcp_status)
STUB_HANDLER(handle_mcp_grant)
STUB_HANDLER(handle_mcp_revoke)
STUB_HANDLER(handle_mcp_reset)
STUB_HANDLER(handle_code_proj_list)
STUB_HANDLER(handle_code_proj_import)
STUB_HANDLER(handle_code_proj_refresh)
STUB_HANDLER(handle_code_proj_delete)
STUB_HANDLER(handle_code_proj_rebuild)
STUB_HANDLER(handle_code_proj_link)
STUB_HANDLER(handle_code_proj_set_branch)

int conv_images_delete_conversation(int64_t conv_id, int user_id);
int conv_images_delete_conversation(int64_t conv_id, int user_id) {
   (void)conv_id;
   (void)user_id;
   return SUCCESS;
}

bool conv_images_purge_user(int user_id);
bool conv_images_purge_user(int user_id) {
   (void)user_id;
   return true;
}

int music_queue_db_delete_user(int user_id);
int music_queue_db_delete_user(int user_id) {
   (void)user_id;
   return SUCCESS;
}

void session_withdraw_forgotten_async(int user_id, bool memory_bodies);
void session_withdraw_forgotten_async(int user_id, bool memory_bodies) {
   (void)user_id;
   (void)memory_bodies;
}

void webui_clear_login_rate_limit(const char *ip_address);
void webui_clear_login_rate_limit(const char *ip_address) {
   (void)ip_address;
}
