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
 * Which WebUI messages are the user's own use of a login (webui_user_action.h).
 */

#include "webui/webui_user_action.h"

#include <string.h>

/* Messages only the user's own action sends (a click, typing, a control), so
 * each one keeps the login going (conn_login_used).  Nothing the client sends
 * by itself belongs here: init/reconnect, ping, the requests made on every
 * connect, timer polls and refreshes after a server push would keep an
 * unattended tab signed in (so no mail list or unread-count refresh).  Voice
 * counts too: push-to-talk (webui_audio.c) and an always-on command
 * (webui_always_on.c). */
static const char *const s_user_action_types[] = {
   "text",
   "cancel",
   "stop_other_reply",
   "music_control",
   "music_search",
   "set_config",
   "set_secrets",
   "set_tools_config",
   "set_my_settings",
   "set_tts_enabled",
   "change_password",
   "delete_conversation",
   "rename_conversation",
   "set_pinned",
   "search_conversations",
   "export_conversation",
   "phone_action",
   "ha_call_service",
   "watch_add",
   "watch_update",
   "watch_set_enabled",
   "watch_remove",
   "job_action",
   "coding_reply",
   "coding_publish",
   "coding_close",
   "coding_grant_revoke",
   "email_read",
   "email_set_flags",
   "email_archive",
   "email_trash",
   "email_undo",
   "email_add_account",
   "email_update_account",
   "email_remove_account",
   "email_test_connection",
   "email_set_enabled",
   "email_set_read_only",
   "contacts_add",
   "contacts_update",
   "contacts_delete",
   "contacts_search",
   "doc_library_delete",
   "doc_library_doc_update",
   "doc_library_index",
   "doc_library_note_save",
   "doc_library_note_update",
   "doc_library_toggle_global",
   "doc_library_version_restore",
};

bool webui_is_user_action(const char *type) {
   if (!type) {
      return false;
   }
   for (size_t i = 0; i < sizeof(s_user_action_types) / sizeof(s_user_action_types[0]); i++) {
      if (strcmp(type, s_user_action_types[i]) == 0) {
         return true;
      }
   }
   return false;
}
