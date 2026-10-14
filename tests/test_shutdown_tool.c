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
 * Unit tests for the shutdown tool's passphrase check, which compares a
 * spoken passphrase folded (case, punctuation, spacing).  Only refusing paths
 * are exercised; an accepted request would run the real shutdown.
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "dawn_error.h"
#include "tools/shutdown_tool.h"
#include "tools/tool_registry.h"
#include "unity.h"

/* The global config the tool reads ([shutdown]); the registry stub defines
 * g_config, and config_defaults.c (not linked) the lock */
pthread_rwlock_t g_config_rwlock = PTHREAD_RWLOCK_INITIALIZER;

static const tool_metadata_t *s_meta;

static void configure(bool enabled, const char *passphrase) {
   g_config.shutdown.enabled = enabled;
   snprintf(g_config.shutdown.passphrase, sizeof(g_config.shutdown.passphrase), "%s", passphrase);
}

/* Run the callback; true when it refused (an error-marked reply) */
static bool refused(const char *value) {
   char buf[128] = { 0 };
   int should_respond = 0;
   char *reply = NULL;
   if (value) {
      snprintf(buf, sizeof(buf), "%s", value);
      reply = s_meta->callback("execute", buf, &should_respond);
   } else {
      reply = s_meta->callback("execute", NULL, &should_respond);
   }
   bool is_refusal = reply &&
                     strncmp(reply, TOOL_RESULT_ERROR_MARK, strlen(TOOL_RESULT_ERROR_MARK)) == 0;
   free(reply);
   return is_refusal;
}

void setUp(void) {
   tool_registry_init();
   TEST_ASSERT_EQUAL(SUCCESS, shutdown_tool_register());
   s_meta = tool_registry_find("shutdown");
   TEST_ASSERT_NOT_NULL(s_meta);
}

void tearDown(void) {
   tool_registry_shutdown();
}

/* ============================================================================
 * Callback refusals
 * ============================================================================ */

static void test_disabled_refuses(void) {
   configure(false, "alpha bravo");
   TEST_ASSERT_TRUE(refused("alpha bravo"));
   TEST_ASSERT_TRUE(refused(NULL));
}

static void test_wrong_passphrase_refuses(void) {
   configure(true, "Alpha Bravo");
   TEST_ASSERT_TRUE(refused("alpha charlie"));
   TEST_ASSERT_TRUE(refused("alpha"));
   TEST_ASSERT_TRUE(refused("alpha bravo charlie"));
   TEST_ASSERT_TRUE(refused(""));
   TEST_ASSERT_TRUE(refused(NULL));
}

static void test_unspeakable_passphrase_refuses(void) {
   /* Folds to nothing: an empty match must not authorize */
   configure(true, "?!");
   TEST_ASSERT_TRUE(refused(NULL));
   TEST_ASSERT_TRUE(refused(""));
   configure(true, "   ");
   TEST_ASSERT_TRUE(refused(NULL));
}

static void test_non_ascii_passphrase_refuses_wrong(void) {
   configure(true, "Пароль семь");
   TEST_ASSERT_TRUE(refused(NULL));
   TEST_ASSERT_TRUE(refused("семь"));
   TEST_ASSERT_TRUE(refused("Пароль"));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_disabled_refuses);
   RUN_TEST(test_wrong_passphrase_refuses);
   RUN_TEST(test_unspeakable_passphrase_refuses);
   RUN_TEST(test_non_ascii_passphrase_refuses_wrong);
   return UNITY_END();
}
