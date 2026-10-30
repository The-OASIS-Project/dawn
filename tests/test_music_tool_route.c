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
 * Who may play on the local speaker when the caller has no player of its own
 * (music_tool_speaker_allowed): its owner only, from the local mic, a
 * messaging channel or no session; never a WebUI tab, a satellite or a job.
 */

#include <stdlib.h>

#include "tools/music_tool.h"
#include "unity.h"

#define OWNER 3
#define OTHER 5

static session_t *s_session;

void setUp(void) {
   s_session = calloc(1, sizeof(*s_session));
   TEST_ASSERT_NOT_NULL(s_session);
}

void tearDown(void) {
   free(s_session);
   s_session = NULL;
}

static bool allowed(session_type_t type, int caller) {
   s_session->type = type;
   return music_tool_speaker_allowed(s_session, caller, OWNER);
}

static void test_owner_local_mic(void) {
   TEST_ASSERT_TRUE(allowed(SESSION_TYPE_LOCAL, OWNER));
}

static void test_owner_messaging(void) {
   TEST_ASSERT_TRUE(allowed(SESSION_TYPE_MESSAGING, OWNER));
}

/* The scheduler, a briefing, MQTT: no session, acting for the owner */
static void test_owner_no_session(void) {
   TEST_ASSERT_TRUE(music_tool_speaker_allowed(NULL, OWNER, OWNER));
}

static void test_other_user_no_session_refused(void) {
   TEST_ASSERT_FALSE(music_tool_speaker_allowed(NULL, OTHER, OWNER));
}

static void test_other_user_messaging_refused(void) {
   TEST_ASSERT_FALSE(allowed(SESSION_TYPE_MESSAGING, OTHER));
}

static void test_guest_refused(void) {
   TEST_ASSERT_FALSE(allowed(SESSION_TYPE_LOCAL, 0));
}

/* A closed tab or a satellite has a speaker of its own, even the owner's */
static void test_owner_webui_refused(void) {
   TEST_ASSERT_FALSE(allowed(SESSION_TYPE_WEBUI, OWNER));
}

static void test_owner_satellites_refused(void) {
   TEST_ASSERT_FALSE(allowed(SESSION_TYPE_DAP2, OWNER));
   TEST_ASSERT_FALSE(allowed(SESSION_TYPE_DAP, OWNER));
}

static void test_owner_job_refused(void) {
   TEST_ASSERT_FALSE(allowed(SESSION_TYPE_JOB, OWNER));
}

/* No owner known: nobody plays */
static void test_no_owner_refused(void) {
   TEST_ASSERT_FALSE(music_tool_speaker_allowed(NULL, 0, 0));
   s_session->type = SESSION_TYPE_LOCAL;
   TEST_ASSERT_FALSE(music_tool_speaker_allowed(s_session, 0, 0));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_owner_local_mic);
   RUN_TEST(test_owner_messaging);
   RUN_TEST(test_owner_no_session);
   RUN_TEST(test_other_user_no_session_refused);
   RUN_TEST(test_other_user_messaging_refused);
   RUN_TEST(test_guest_refused);
   RUN_TEST(test_owner_webui_refused);
   RUN_TEST(test_owner_satellites_refused);
   RUN_TEST(test_owner_job_refused);
   RUN_TEST(test_no_owner_refused);
   return UNITY_END();
}
