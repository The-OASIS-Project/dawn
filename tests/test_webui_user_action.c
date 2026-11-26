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
 * What counts as use of a login (webui_user_action.c): user actions do, the
 * messages WebUI and Aurora clients send by themselves don't.
 */

#include "unity.h"
#include "webui/webui_user_action.h"

void setUp(void) {
}
void tearDown(void) {
}

static void test_user_actions_count(void) {
   const char *used[] = { "text",
                          "cancel",
                          "music_control",
                          "music_search",
                          "set_config",
                          "email_read",
                          "email_set_flags",
                          "email_trash",
                          "contacts_add",
                          "doc_library_note_save",
                          "ha_call_service",
                          "phone_action",
                          "job_action",
                          "coding_reply",
                          "watch_add",
                          "watch_remove" };
   for (size_t i = 0; i < sizeof(used) / sizeof(used[0]); i++) {
      TEST_ASSERT_TRUE_MESSAGE(webui_is_user_action(used[i]), used[i]);
   }
}

/* Each of these goes out with no one at the screen: on connect or reconnect,
 * from a timer, or as a refresh after a server push (WebUI and Aurora) */
static void test_automatic_messages_dont(void) {
   const char *automatic[] = { "ping",
                               "init",
                               "reconnect",
                               "get_config",
                               "list_conversations",
                               "load_conversation",
                               "set_active_conversation",
                               "session_keepalive_enable",
                               "always_on_enable",
                               "always_on_state_request",
                               "jobs_request",
                               "phone_status",
                               "music_subscribe",
                               "music_queue",
                               "music_library",
                               "email_list",
                               "email_list_accounts",
                               "email_unread_counts",
                               "email_search",
                               "doc_library_list",
                               "calendar_upcoming_events",
                               "scheduler_action",
                               "watch_list",
                               "get_metrics",
                               "list_memory_batches",
                               "coding_pending",
                               "save_message",
                               "update_context" };
   for (size_t i = 0; i < sizeof(automatic) / sizeof(automatic[0]); i++) {
      TEST_ASSERT_FALSE_MESSAGE(webui_is_user_action(automatic[i]), automatic[i]);
   }
}

/* Exact names only: a prefix or a longer name isn't a match */
static void test_exact_names(void) {
   TEST_ASSERT_FALSE(webui_is_user_action("email_"));
   TEST_ASSERT_FALSE(webui_is_user_action("text_extra"));
   TEST_ASSERT_FALSE(webui_is_user_action("tex"));
   TEST_ASSERT_FALSE(webui_is_user_action(""));
   TEST_ASSERT_FALSE(webui_is_user_action(NULL));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_user_actions_count);
   RUN_TEST(test_automatic_messages_dont);
   RUN_TEST(test_exact_names);
   return UNITY_END();
}
