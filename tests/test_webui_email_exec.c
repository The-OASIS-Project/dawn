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
 * The email executor under load, with the real lease and stub account work:
 * every request is answered exactly once, every context is freed exactly
 * once, an IMAP account never has two users at once (an outside holder
 * included), and a stop in the middle of it all neither hangs nor leaks.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tools/email_account_lease.h"
#include "unity.h"
#include "webui/webui_email_exec.h"

#define ACCOUNTS 4 /* 1, 2: IMAP; 3, 4: Gmail */
#define SUBMITTERS 8

static atomic_int s_created, s_freed, s_sent, s_submitted, s_finished;
static atomic_int s_active[ACCOUNTS + 1];
static atomic_int s_overlap; /* an IMAP account used by two at once */
static atomic_int s_bad_target;
static atomic_bool s_holder_stop;

void setUp(void) {
}
void tearDown(void) {
}

/* The executor's sender, replaced: count what's answered. */
void *webui_email_exec_session_open(uint32_t session_id, int user_id) {
   (void)session_id;
   (void)user_id;
   return (void *)(uintptr_t)1;
}

void webui_email_exec_session_send(void *session, char *json) {
   (void)session;
   if (json)
      atomic_fetch_add(&s_sent, 1);
   free(json);
}

void webui_email_exec_session_close(void *session) {
   (void)session;
}

typedef enum {
   MODE_LOAD = 0,
   MODE_FAST,
   MODE_SLOW,
   MODE_PROBE
} stub_mode_t;

typedef struct {
   stub_mode_t mode;
   int results[EMAIL_EXEC_MAX_TASKS];
} stub_ctx_t;

static atomic_int s_slow_done; /* MODE_SLOW tasks finished */
static atomic_int s_probe_saw; /* s_slow_done when the probe ran (-1: not yet) */

static bool is_imap_account(int64_t id) {
   return id == 1 || id == 2;
}

static void use_account(int64_t id) {
   if (is_imap_account(id) && atomic_fetch_add(&s_active[id], 1) != 0)
      atomic_fetch_add(&s_overlap, 1);
}

static void done_with_account(int64_t id) {
   if (is_imap_account(id))
      atomic_fetch_sub(&s_active[id], 1);
}

static void stub_op(const email_exec_task_ctx_t *t) {
   stub_ctx_t *ctx = (stub_ctx_t *)t->ctx;
   if (ctx->mode == MODE_FAST) {
      ctx->results[t->index] = 1;
      return;
   }
   if (ctx->mode == MODE_SLOW) {
      usleep(40000);
      atomic_fetch_add(&s_slow_done, 1);
      return;
   }
   if (ctx->mode == MODE_PROBE) {
      atomic_store(&s_probe_saw, atomic_load(&s_slow_done));
      return;
   }
   if (t->lease_err != EMAIL_ERR_NONE) {
      ctx->results[t->index] = -1;
      return;
   }
   const bool imap = is_imap_account(t->account_id);
   if (t->target->account_id != t->account_id || t->target->lease_held != imap ||
       (imap && !email_lease_is_held(t->account_id)))
      atomic_fetch_add(&s_bad_target, 1);
   use_account(t->account_id);
   for (int i = 0; i < 4 && !atomic_load(t->cancel); i++)
      usleep(500 + rand() % 1500);
   done_with_account(t->account_id);
   ctx->results[t->index] = 1;
}

static json_object *stub_finish(void *ctx, const int64_t *account_ids, int n) {
   (void)ctx;
   (void)account_ids;
   (void)n;
   atomic_fetch_add(&s_finished, 1);
   return json_object_new_object();
}

static void stub_free(void *ctx) {
   atomic_fetch_add(&s_freed, 1);
   free(ctx);
}

static int submit_ids(uint32_t session,
                      int user,
                      email_exec_slot_t slot,
                      stub_mode_t mode,
                      const int64_t *ids,
                      const bool *imap,
                      int n) {
   stub_ctx_t *ctx = calloc(1, sizeof(*ctx));
   ctx->mode = mode;
   atomic_fetch_add(&s_created, 1);
   atomic_fetch_add(&s_submitted, 1);
   const email_exec_request_t r = {
      .session_id = session,
      .user_id = user,
      .slot = slot,
      .verb = "email_list",
      .req = "r",
      .task_count = n,
      .account_ids = ids,
      .is_imap = imap,
      .op = stub_op,
      .finish = stub_finish,
      .free_ctx = stub_free,
      .ctx = ctx,
   };
   return webui_email_exec_submit(&r);
}

static int submit(uint32_t session, int user, email_exec_slot_t slot, int n_accounts) {
   static const int64_t ids[ACCOUNTS] = { 1, 2, 3, 4 };
   static const bool imap[ACCOUNTS] = { true, true, false, false };
   stub_ctx_t *ctx = calloc(1, sizeof(*ctx));
   atomic_fetch_add(&s_created, 1);
   atomic_fetch_add(&s_submitted, 1);
   const int first = slot == EMAIL_EXEC_SLOT_READ ? rand() % ACCOUNTS : 0;
   const email_exec_request_t r = {
      .session_id = session,
      .user_id = user,
      .slot = slot,
      .verb = slot == EMAIL_EXEC_SLOT_READ ? "email_read" : "email_list",
      .req = "r",
      .task_count = n_accounts,
      .account_ids = &ids[first],
      .is_imap = &imap[first],
      .op = stub_op,
      .finish = stub_finish,
      .free_ctx = stub_free,
      .ctx = ctx,
   };
   return webui_email_exec_submit(&r);
}

/* Someone outside the executor (the tool, the digest) using account 1. */
static void *outside_holder(void *arg) {
   const int hold_ms = *(const int *)arg;
   while (!atomic_load(&s_holder_stop)) {
      if (email_lease_acquire(1, NULL, 5) == EMAIL_LEASE_OK) {
         use_account(1);
         usleep(hold_ms * 1000);
         done_with_account(1);
         email_lease_release(1);
      }
      usleep(3000);
   }
   return NULL;
}

typedef struct {
   uint32_t session;
   int user;
   int rounds;
} submitter_arg_t;

static void *submitter(void *arg) {
   const submitter_arg_t *a = (const submitter_arg_t *)arg;
   for (int i = 0; i < a->rounds; i++) {
      const email_exec_slot_t slot = (i % 3 == 2) ? EMAIL_EXEC_SLOT_READ : EMAIL_EXEC_SLOT_LIST;
      submit(a->session, a->user, slot, slot == EMAIL_EXEC_SLOT_READ ? 1 : ACCOUNTS);
      usleep(rand() % 4000);
   }
   return NULL;
}

/* Wait (bounded) until every context is freed; false on a hang. */
static bool wait_drained(int seconds) {
   for (int i = 0; i < seconds * 100; i++) {
      if (atomic_load(&s_freed) == atomic_load(&s_created))
         return true;
      usleep(10000);
   }
   return false;
}

static void run_load(int rounds, int hold_ms, pthread_t *holder) {
   static int hold;
   hold = hold_ms;
   atomic_store(&s_holder_stop, false);
   pthread_create(holder, NULL, outside_holder, &hold);
   pthread_t th[SUBMITTERS];
   submitter_arg_t args[SUBMITTERS];
   for (int i = 0; i < SUBMITTERS; i++) {
      args[i] = (submitter_arg_t){ .session = 100 + i, .user = 1 + i % 3, .rounds = rounds };
      pthread_create(&th[i], NULL, submitter, &args[i]);
   }
   for (int i = 0; i < SUBMITTERS; i++)
      pthread_join(th[i], NULL);
}

static void test_a_req_is_echoed_only_when_plain(void) {
   char out[EMAIL_EXEC_REQ_MAX + 1];
   json_object *p = json_tokener_parse("{\"req\":\"l-17\"}");
   TEST_ASSERT_TRUE(email_exec_payload_req(p, out, sizeof(out)));
   TEST_ASSERT_EQUAL_STRING("l-17", out);
   json_object_put(p);
   /* A control character, a DEL, too long, not a string: ignored. */
   p = json_tokener_parse("{\"req\":\"a\\nb\"}");
   TEST_ASSERT_FALSE(email_exec_payload_req(p, out, sizeof(out)));
   json_object_put(p);
   p = json_tokener_parse("{\"req\":\"a\\u007fb\"}");
   TEST_ASSERT_FALSE(email_exec_payload_req(p, out, sizeof(out)));
   json_object_put(p);
   char big[128];
   snprintf(big, sizeof(big), "{\"req\":\"%065d\"}", 0);
   p = json_tokener_parse(big);
   TEST_ASSERT_FALSE(email_exec_payload_req(p, out, sizeof(out)));
   json_object_put(p);
   p = json_tokener_parse("{\"req\":7}");
   TEST_ASSERT_FALSE(email_exec_payload_req(p, out, sizeof(out)));
   json_object_put(p);
}

static void test_every_request_is_answered_once_and_freed_once(void) {
   pthread_t holder;
   run_load(30, 20, &holder);
   TEST_ASSERT_TRUE_MESSAGE(wait_drained(20), "requests still pending: a hang");
   atomic_store(&s_holder_stop, true);
   pthread_join(holder, NULL);

   TEST_ASSERT_EQUAL(atomic_load(&s_created), atomic_load(&s_freed));
   TEST_ASSERT_EQUAL(atomic_load(&s_submitted), atomic_load(&s_sent));
   TEST_ASSERT_EQUAL(0, atomic_load(&s_overlap));
   TEST_ASSERT_EQUAL(0, atomic_load(&s_bad_target));
   TEST_ASSERT_TRUE(atomic_load(&s_finished) > 0);
   TEST_ASSERT_FALSE(email_lease_is_held(1));
   TEST_ASSERT_FALSE(email_lease_is_held(2));
}

/* A list superseded just as its only task finishes: the cancel and the
 * completion race over the join (it must be freed once, after both). */
static void test_a_supersede_racing_a_finishing_task_frees_once(void) {
   static const int64_t ids[1] = { 3 };
   static const bool imap[1] = { false };
   for (int i = 0; i < 3000; i++) {
      submit_ids(700, 9, EMAIL_EXEC_SLOT_LIST, MODE_FAST, ids, imap, 1);
      if (i % 7 == 0)
         usleep(50);
   }
   TEST_ASSERT_TRUE_MESSAGE(wait_drained(20), "requests still pending: a hang");
   TEST_ASSERT_EQUAL(atomic_load(&s_created), atomic_load(&s_freed));
   TEST_ASSERT_EQUAL(atomic_load(&s_submitted), atomic_load(&s_sent));
}

/* One user's slow request across many accounts doesn't hold every worker: a
 * second user's request starts while it's still running. */
static void test_one_user_cant_hold_every_worker(void) {
   int64_t ids[EMAIL_EXEC_MAX_TASKS];
   bool imap[EMAIL_EXEC_MAX_TASKS];
   for (int i = 0; i < EMAIL_EXEC_MAX_TASKS; i++) {
      ids[i] = 10 + i;
      imap[i] = false;
   }
   atomic_store(&s_slow_done, 0);
   atomic_store(&s_probe_saw, -1);
   TEST_ASSERT_EQUAL(EMAIL_EXEC_OK, submit_ids(800, 50, EMAIL_EXEC_SLOT_LIST, MODE_SLOW, ids, imap,
                                               EMAIL_EXEC_MAX_TASKS));
   usleep(5000);
   static const int64_t probe_id[1] = { 40 };
   static const bool probe_imap[1] = { false };
   TEST_ASSERT_EQUAL(EMAIL_EXEC_OK, submit_ids(801, 51, EMAIL_EXEC_SLOT_READ, MODE_PROBE, probe_id,
                                               probe_imap, 1));
   TEST_ASSERT_TRUE_MESSAGE(wait_drained(20), "requests still pending: a hang");
   const int saw = atomic_load(&s_probe_saw);
   TEST_ASSERT_TRUE(saw >= 0);
   /* Without the cap the probe waits behind the slow user's 16 tasks (it
    * would see at least 12 of them done); with it, it starts at once. */
   TEST_ASSERT_TRUE_MESSAGE(saw <= EMAIL_EXEC_WORKERS, "probe waited behind the slow user");
}

static void *stopper(void *arg) {
   (void)arg;
   usleep(30000);
   webui_email_exec_stop();
   return NULL;
}

static void test_a_stop_under_load_neither_hangs_nor_leaks(void) {
   /* A long outside hold leaves tasks waiting in the lease when the stop comes. */
   pthread_t holder, stop;
   pthread_create(&stop, NULL, stopper, NULL);
   run_load(20, 150, &holder);
   pthread_join(stop, NULL);
   atomic_store(&s_holder_stop, true);
   pthread_join(holder, NULL);

   TEST_ASSERT_TRUE_MESSAGE(wait_drained(20), "contexts not freed after stop");
   TEST_ASSERT_EQUAL(atomic_load(&s_submitted), atomic_load(&s_sent));
   TEST_ASSERT_EQUAL(0, atomic_load(&s_overlap));
   TEST_ASSERT_FALSE(email_lease_is_held(1));

   /* After the stop: refused (and answered), context freed. */
   const int sent = atomic_load(&s_sent);
   TEST_ASSERT_EQUAL(EMAIL_EXEC_SHUTTING_DOWN, submit(999, 1, EMAIL_EXEC_SLOT_LIST, 2));
   TEST_ASSERT_EQUAL(sent + 1, atomic_load(&s_sent));
   TEST_ASSERT_EQUAL(atomic_load(&s_created), atomic_load(&s_freed));
}

/* Unity has no per-test timeout; a hang fails the run instead of blocking CI. */
static void *watchdog(void *arg) {
   (void)arg;
   sleep(90);
   fprintf(stderr, "test_webui_email_exec: watchdog fired (hang)\n");
   _exit(2);
   return NULL;
}

int main(void) {
   srand((unsigned)time(NULL));
   pthread_t dog;
   pthread_create(&dog, NULL, watchdog, NULL);
   pthread_detach(dog);
   UNITY_BEGIN();
   RUN_TEST(test_a_req_is_echoed_only_when_plain);
   RUN_TEST(test_every_request_is_answered_once_and_freed_once);
   RUN_TEST(test_a_supersede_racing_a_finishing_task_frees_once);
   RUN_TEST(test_one_user_cant_hold_every_worker);
   RUN_TEST(test_a_stop_under_load_neither_hangs_nor_leaks); /* last: a stop is final */
   return UNITY_END();
}
