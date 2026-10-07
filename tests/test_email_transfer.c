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
 * Email failure codes on the wire, curl results as failure codes, and the
 * per-thread transfer cancel flag.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "tools/email_transfer.h"
#include "unity.h"

void setUp(void) {
}

void tearDown(void) {
}

static void test_every_code_has_its_own_wire_name(void) {
   TEST_ASSERT_EQUAL_STRING("", email_error_name(EMAIL_ERR_NONE));
   TEST_ASSERT_EQUAL_STRING("FAILED", email_error_name(EMAIL_ERR_FAILED));
   TEST_ASSERT_EQUAL_STRING("CURSOR_STALE", email_error_name(EMAIL_ERR_CURSOR_STALE));
   TEST_ASSERT_EQUAL_STRING("SUPERSEDED", email_error_name(EMAIL_ERR_SUPERSEDED));
   TEST_ASSERT_EQUAL_STRING("UNSUPPORTED_QUERY", email_error_name(EMAIL_ERR_UNSUPPORTED_QUERY));
   TEST_ASSERT_EQUAL_STRING("BUSY", email_error_name(EMAIL_ERR_BUSY));
   TEST_ASSERT_EQUAL_STRING("SHUTTING_DOWN", email_error_name(EMAIL_ERR_SHUTTING_DOWN));
   TEST_ASSERT_EQUAL_STRING("CANNOT_CALCULATE", email_error_name(EMAIL_ERR_CANNOT_CALCULATE));
   TEST_ASSERT_EQUAL_STRING("INVALID_REQUEST", email_error_name(EMAIL_ERR_INVALID_REQUEST));
   TEST_ASSERT_EQUAL_STRING("UNAVAILABLE", email_error_name(EMAIL_ERR_UNAVAILABLE));
   TEST_ASSERT_EQUAL_STRING("IN_TRASH", email_error_name(EMAIL_ERR_IN_TRASH));
   TEST_ASSERT_EQUAL_STRING("OUTCOME_UNKNOWN", email_error_name(EMAIL_ERR_OUTCOME_UNKNOWN));
   /* Every code past FAILED names itself: none falls back to "FAILED". */
   for (int e = EMAIL_ERR_FAILED + 1; e <= EMAIL_ERR_OUTCOME_UNKNOWN; e++) {
      const char *name = email_error_name((email_err_t)e);
      TEST_ASSERT_TRUE(name[0] != '\0');
      TEST_ASSERT_TRUE(strcmp(name, "FAILED") != 0);
      for (int f = EMAIL_ERR_FAILED; f < e; f++)
         TEST_ASSERT_TRUE(strcmp(name, email_error_name((email_err_t)f)) != 0);
   }
}

static void test_curl_results_map_to_codes(void) {
   TEST_ASSERT_EQUAL_INT(EMAIL_ERR_NONE, email_err_from_curl(CURLE_OK));
   TEST_ASSERT_EQUAL_INT(EMAIL_ERR_AUTH_FAILED, email_err_from_curl(CURLE_LOGIN_DENIED));
   TEST_ASSERT_EQUAL_INT(EMAIL_ERR_UNREACHABLE, email_err_from_curl(CURLE_COULDNT_CONNECT));
   TEST_ASSERT_EQUAL_INT(EMAIL_ERR_TIMEOUT, email_err_from_curl(CURLE_OPERATION_TIMEDOUT));
   TEST_ASSERT_EQUAL_INT(EMAIL_ERR_CANCELLED, email_err_from_curl(CURLE_ABORTED_BY_CALLBACK));
   TEST_ASSERT_EQUAL_INT(EMAIL_ERR_FAILED, email_err_from_curl(CURLE_RECV_ERROR));
}

static atomic_bool s_other_flag;

static void *other_thread(void *arg) {
   const atomic_bool **seen = arg;
   seen[0] = email_transfer_thread_cancel(); /* not the main thread's */
   email_transfer_scope_cancel(&s_other_flag);
   seen[1] = email_transfer_thread_cancel();
   return NULL;
}

static void test_the_cancel_flag_is_per_thread(void) {
   atomic_bool flag = false;
   TEST_ASSERT_NULL(email_transfer_thread_cancel());
   TEST_ASSERT_NULL(email_transfer_scope_cancel(&flag));
   TEST_ASSERT_EQUAL_PTR(&flag, email_transfer_thread_cancel());

   const atomic_bool *seen[2] = { &flag, NULL };
   pthread_t th;
   pthread_create(&th, NULL, other_thread, seen);
   pthread_join(th, NULL);
   TEST_ASSERT_NULL(seen[0]);
   TEST_ASSERT_EQUAL_PTR(&s_other_flag, seen[1]);
   TEST_ASSERT_EQUAL_PTR(&flag, email_transfer_thread_cancel()); /* untouched here */

   /* Ending the scope restores what was there. */
   TEST_ASSERT_EQUAL_PTR(&flag, email_transfer_scope_cancel(NULL));
   TEST_ASSERT_NULL(email_transfer_thread_cancel());
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_every_code_has_its_own_wire_name);
   RUN_TEST(test_curl_results_map_to_codes);
   RUN_TEST(test_the_cancel_flag_is_per_thread);
   return UNITY_END();
}
