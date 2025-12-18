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
 * Test: Per-Session LLM Command Context
 *
 * Tests that the command context mechanism (thread-local storage) works correctly
 * for passing session pointers to device callbacks.
 *
 * This is a simplified test that validates the TLS mechanism without needing
 * the full session manager or LLM infrastructure.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Test counters */
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(condition, msg)    \
   do {                                \
      if (condition) {                 \
         printf("  [PASS] %s\n", msg); \
         tests_passed++;               \
      } else {                         \
         printf("  [FAIL] %s\n", msg); \
         tests_failed++;               \
      }                                \
   } while (0)

/* =============================================================================
 * Minimal Session Structure for Testing
 * ============================================================================= */

typedef struct test_session {
   uint32_t session_id;
   int type; /* 0=local, 1=websocket */
} test_session_t;

/* Thread-local command context (mirrors session_manager.c implementation) */
static __thread test_session_t *tl_test_context = NULL;

void test_set_context(test_session_t *session) {
   tl_test_context = session;
}

test_session_t *test_get_context(void) {
   return tl_test_context;
}

/* =============================================================================
 * Test: Basic TLS Operations
 * ============================================================================= */
static void test_tls_basics(void) {
   printf("\n=== Test: TLS Basics ===\n");

   /* Initially NULL */
   TEST_ASSERT(test_get_context() == NULL, "Initial context is NULL");

   /* Create test sessions */
   test_session_t session1 = { .session_id = 1, .type = 0 };
   test_session_t session2 = { .session_id = 2, .type = 1 };

   /* Set and get */
   test_set_context(&session1);
   TEST_ASSERT(test_get_context() == &session1, "Context set to session1");
   TEST_ASSERT(test_get_context()->session_id == 1, "Session ID is 1");

   /* Switch context */
   test_set_context(&session2);
   TEST_ASSERT(test_get_context() == &session2, "Context switched to session2");
   TEST_ASSERT(test_get_context()->session_id == 2, "Session ID is 2");

   /* Clear context */
   test_set_context(NULL);
   TEST_ASSERT(test_get_context() == NULL, "Context cleared");
}

/* =============================================================================
 * Test: Thread Isolation
 * ============================================================================= */

typedef struct {
   test_session_t *session;
   test_session_t *observed_context;
   int context_was_isolated;
} thread_test_data_t;

static void *thread_test_func(void *arg) {
   thread_test_data_t *data = (thread_test_data_t *)arg;

   /* Check that context starts NULL in new thread */
   data->observed_context = test_get_context();
   data->context_was_isolated = (data->observed_context == NULL);

   /* Set context in this thread */
   test_set_context(data->session);

   /* Small delay to let main thread verify */
   usleep(50000);

   /* Clear before exit */
   test_set_context(NULL);

   return NULL;
}

static void test_thread_isolation(void) {
   printf("\n=== Test: Thread Isolation ===\n");

   test_session_t main_session = { .session_id = 100, .type = 0 };
   test_session_t thread_session = { .session_id = 200, .type = 1 };

   /* Set context in main thread */
   test_set_context(&main_session);
   TEST_ASSERT(test_get_context() == &main_session, "Main thread context set");

   /* Spawn worker thread */
   thread_test_data_t thread_data = { .session = &thread_session,
                                      .observed_context = NULL,
                                      .context_was_isolated = 0 };

   pthread_t worker;
   int rc = pthread_create(&worker, NULL, thread_test_func, &thread_data);
   TEST_ASSERT(rc == 0, "Worker thread created");

   /* While worker is running, verify main thread context unchanged */
   usleep(25000);
   TEST_ASSERT(test_get_context() == &main_session, "Main thread context unchanged during worker");

   /* Wait for worker */
   pthread_join(worker, NULL);

   /* Verify thread isolation */
   TEST_ASSERT(thread_data.context_was_isolated, "Worker thread started with NULL context");
   TEST_ASSERT(test_get_context() == &main_session,
               "Main thread context still intact after worker");

   /* Cleanup */
   test_set_context(NULL);
}

/* =============================================================================
 * Test: Simulated Callback Flow
 * ============================================================================= */

/* Simulated device callback that reads context */
static uint32_t simulated_callback_session_id = 0;

static char *simulated_llm_callback(void) {
   test_session_t *ctx = test_get_context();
   if (ctx) {
      simulated_callback_session_id = ctx->session_id;
      return "Used session config";
   } else {
      simulated_callback_session_id = 0;
      return "Used global config";
   }
}

static void test_callback_flow(void) {
   printf("\n=== Test: Simulated Callback Flow ===\n");

   test_session_t local = { .session_id = 0, .type = 0 };
   test_session_t webui = { .session_id = 5, .type = 1 };

   /* Simulate local voice command flow */
   test_set_context(&local);
   char *result1 = simulated_llm_callback();
   test_set_context(NULL);

   TEST_ASSERT(simulated_callback_session_id == 0, "Callback saw local session (ID 0)");
   TEST_ASSERT(strcmp(result1, "Used session config") == 0, "Callback used session config");

   /* Simulate WebUI command flow */
   test_set_context(&webui);
   char *result2 = simulated_llm_callback();
   test_set_context(NULL);

   TEST_ASSERT(simulated_callback_session_id == 5, "Callback saw WebUI session (ID 5)");
   TEST_ASSERT(strcmp(result2, "Used session config") == 0, "Callback used session config");

   /* Simulate no context (fallback to global) */
   char *result3 = simulated_llm_callback();
   TEST_ASSERT(simulated_callback_session_id == 0, "Callback got no session");
   TEST_ASSERT(strcmp(result3, "Used global config") == 0, "Callback fell back to global");
}

/* =============================================================================
 * Test: Context Set/Clear Pairs
 * ============================================================================= */
static void test_context_pairs(void) {
   printf("\n=== Test: Context Set/Clear Pairs ===\n");

   test_session_t sessions[3] = {
      { .session_id = 10, .type = 0 },
      { .session_id = 20, .type = 1 },
      { .session_id = 30, .type = 1 },
   };

   /* Simulate nested-like operations (not actually nested, but sequential) */
   for (int i = 0; i < 3; i++) {
      test_set_context(&sessions[i]);
      TEST_ASSERT(test_get_context()->session_id == sessions[i].session_id,
                  "Context correct for iteration");
      test_set_context(NULL);
      TEST_ASSERT(test_get_context() == NULL, "Context cleared after iteration");
   }

   printf("  [INFO] 3 set/clear pairs completed correctly\n");
}

/* =============================================================================
 * Main
 * ============================================================================= */
int main(void) {
   printf("=== Per-Session Command Context Tests ===\n");
   printf("Testing thread-local storage mechanism for command context\n");

   test_tls_basics();
   test_thread_isolation();
   test_callback_flow();
   test_context_pairs();

   /* Summary */
   printf("\n=== Test Summary ===\n");
   printf("Passed: %d\n", tests_passed);
   printf("Failed: %d\n", tests_failed);

   return tests_failed > 0 ? 1 : 0;
}
