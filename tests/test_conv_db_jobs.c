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
 * Unit tests for the background-job accessor layer (src/auth/auth_db_jobs.c):
 * create_job → get/list round-trip, status transitions, follow-up + active
 * scans, ownership isolation, and the delete-guard status probe.
 */

#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "unity.h"

static int alice_id = 0;
static int bob_id = 0;

void setUp(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("alice", "h", true));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("bob", "h", false));
   auth_user_t u;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user("alice", &u));
   alice_id = u.id;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user("bob", &u));
   bob_id = u.id;
}

void tearDown(void) {
   auth_db_shutdown();
}

/* ── create_job → get round-trips every job field ──────────────────────────── */

static void test_create_and_get(void) {
   int64_t parent = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "parent chat", &parent));

   int64_t job = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_create_job(alice_id, "research X", parent, "detached", "notify",
                                            "telegram-main", 1, &job));
   TEST_ASSERT_TRUE(job > 0);

   job_record_t r;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_get(job, alice_id, &r));
   TEST_ASSERT_EQUAL_INT64(job, r.id);
   TEST_ASSERT_EQUAL_INT(alice_id, r.user_id);
   TEST_ASSERT_EQUAL_INT64(parent, r.parent_id);
   TEST_ASSERT_EQUAL_STRING("research X", r.title);
   TEST_ASSERT_EQUAL_STRING("detached", r.spawn_mode);
   TEST_ASSERT_EQUAL_STRING("notify", r.on_complete);
   TEST_ASSERT_EQUAL_STRING("telegram-main", r.deliver_to);
   TEST_ASSERT_EQUAL_STRING("queued", r.job_status);
   TEST_ASSERT_EQUAL_INT(1, r.spawn_depth);
   TEST_ASSERT_FALSE(r.on_complete_fired);
}

/* ── status transitions: queued → running → terminal + fired ───────────────── */

static void test_status_transitions(void) {
   int64_t job = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_create_job(alice_id, "j", 0, "detached", "notify", NULL, 1, &job));

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_set_running(job, 5000));
   job_record_t r;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_get(job, alice_id, &r));
   TEST_ASSERT_EQUAL_STRING("running", r.job_status);
   TEST_ASSERT_EQUAL_INT64(5000, (int64_t)r.started_at);

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_set_terminal(job, "done", NULL, 6000));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_get(job, alice_id, &r));
   TEST_ASSERT_EQUAL_STRING("done", r.job_status);
   TEST_ASSERT_EQUAL_INT64(6000, (int64_t)r.finished_at);
   TEST_ASSERT_FALSE(r.on_complete_fired);

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_mark_fired(job));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_get(job, alice_id, &r));
   TEST_ASSERT_TRUE(r.on_complete_fired);
}

/* ── follow-up scan: terminal + unfired only, disappears after fired ───────── */

static void test_pending_followups_scan(void) {
   int64_t j1 = 0, j2 = 0, j3 = 0;
   conv_db_create_job(alice_id, "j1", 0, "detached", "notify", NULL, 1, &j1);
   conv_db_create_job(alice_id, "j2", 0, "detached", "notify", NULL, 1, &j2);
   conv_db_create_job(alice_id, "j3", 0, "detached", "notify", NULL, 1, &j3);

   conv_db_job_set_terminal(j1, "done", NULL, 100);     /* pending */
   conv_db_job_set_terminal(j2, "failed", "boom", 100); /* pending */
   conv_db_job_set_running(j3, 100);                    /* NOT terminal */

   job_record_t out[8];
   int n = -1;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_list_pending_followups(8, out, &n));
   TEST_ASSERT_EQUAL_INT(2, n);

   conv_db_job_mark_fired(j1);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_list_pending_followups(8, out, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_INT64(j2, out[0].id);
}

/* ── active scan: running + queued, not terminal ───────────────────────────── */

static void test_active_scan(void) {
   int64_t j1 = 0, j2 = 0, j3 = 0;
   conv_db_create_job(alice_id, "q", 0, "detached", "notify", NULL, 1, &j1); /* queued */
   conv_db_create_job(alice_id, "r", 0, "detached", "notify", NULL, 1, &j2);
   conv_db_create_job(alice_id, "d", 0, "detached", "notify", NULL, 1, &j3);
   conv_db_job_set_running(j2, 100);
   conv_db_job_set_terminal(j3, "done", NULL, 100);

   job_record_t out[8];
   int n = -1;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_scan_active(out, 8, &n));
   TEST_ASSERT_EQUAL_INT(2, n); /* queued + running, not done */
}

/* ── ownership isolation: bob cannot get/see alice's job ───────────────────── */

static void test_ownership_isolation(void) {
   int64_t job = 0;
   conv_db_create_job(alice_id, "secret", 0, "detached", "notify", NULL, 1, &job);

   job_record_t r;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_FORBIDDEN, conv_db_job_get(job, bob_id, &r));

   job_record_t out[8];
   int n = -1;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_list_by_user(bob_id, out, 8, &n));
   TEST_ASSERT_EQUAL_INT(0, n);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_list_by_user(alice_id, out, 8, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
}

/* ── delete-guard status probe: job status, empty for non-job, FORBIDDEN ────── */

static void test_get_status_probe(void) {
   int64_t plain = 0, job = 0;
   conv_db_create(alice_id, "plain chat", &plain);
   conv_db_create_job(alice_id, "j", 0, "detached", "notify", NULL, 1, &job);
   conv_db_job_set_running(job, 100);

   char status[JOB_STATUS_MAX];
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_job_get_status(job, alice_id, status, sizeof(status)));
   TEST_ASSERT_EQUAL_STRING("running", status);

   /* Non-job conversation → success with empty status (delete allowed). */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_job_get_status(plain, alice_id, status, sizeof(status)));
   TEST_ASSERT_EQUAL_STRING("", status);

   /* Another user → FORBIDDEN. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_FORBIDDEN,
                         conv_db_job_get_status(job, bob_id, status, sizeof(status)));
}

/* ── reinvoke_parent accessors ─────────────────────────────────────────────── */

static void test_mark_fired_many(void) {
   int64_t j1 = 0, j2 = 0, j3 = 0;
   conv_db_create_job(alice_id, "a", 0, "detached", "reinvoke_parent", NULL, 1, &j1);
   conv_db_create_job(alice_id, "b", 0, "detached", "reinvoke_parent", NULL, 1, &j2);
   conv_db_create_job(alice_id, "c", 0, "detached", "reinvoke_parent", NULL, 1, &j3);
   conv_db_job_set_terminal(j1, "done", NULL, 100);
   conv_db_job_set_terminal(j2, "done", NULL, 100);
   conv_db_job_set_terminal(j3, "done", NULL, 100);

   int64_t ids[] = { j1, j2 };
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_mark_fired_many(ids, 2));

   /* j1,j2 fired; j3 not → only j3 remains a pending follow-up. */
   job_record_t rows[8];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_list_pending_followups(8, rows, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_INT64(j3, rows[0].id);

   /* Empty list is a no-op success. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_mark_fired_many(ids, 0));
}

static void test_bump_reinvoke(void) {
   int64_t job = 0;
   conv_db_create_job(alice_id, "j", 0, "detached", "reinvoke_parent", NULL, 1, &job);

   int c = -1;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_bump_reinvoke(job, &c));
   TEST_ASSERT_EQUAL_INT(1, c);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_bump_reinvoke(job, &c));
   TEST_ASSERT_EQUAL_INT(2, c);

   job_record_t r;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_get(job, alice_id, &r));
   TEST_ASSERT_EQUAL_INT(2, r.reinvoke_count);
}

static void test_fire_boot_reinvokes(void) {
   int64_t rp = 0, nf = 0;
   conv_db_create_job(alice_id, "reinvoke", 0, "detached", "reinvoke_parent", NULL, 1, &rp);
   conv_db_create_job(alice_id, "notify", 0, "detached", "notify", NULL, 1, &nf);
   conv_db_job_set_terminal(rp, "done", NULL, 100);
   conv_db_job_set_terminal(nf, "done", NULL, 100);

   int fired = -1;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_fire_boot_reinvokes(&fired));
   TEST_ASSERT_EQUAL_INT(1, fired); /* only the reinvoke_parent row */

   /* The notify row is still a pending follow-up; the reinvoke row is suppressed. */
   job_record_t rows[8];
   int n = 0;
   conv_db_job_list_pending_followups(8, rows, &n);
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_INT64(nf, rows[0].id);
}

static void test_last_assistant_text(void) {
   int64_t job = 0;
   conv_db_create_job(alice_id, "j", 0, "detached", "reinvoke_parent", NULL, 1, &job);

   /* No assistant message yet → success with *out == NULL. */
   char *out = (char *)0x1;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_last_assistant_text(job, alice_id, &out));
   TEST_ASSERT_NULL(out);

   int64_t mid = 0;
   conv_db_add_message_ex(job, alice_id, "user", "the goal", &mid);
   conv_db_add_message_ex(job, alice_id, "assistant", "first answer", &mid);
   conv_db_add_message_ex(job, alice_id, "assistant", "final answer", &mid);

   out = NULL;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_last_assistant_text(job, alice_id, &out));
   TEST_ASSERT_NOT_NULL(out);
   TEST_ASSERT_EQUAL_STRING("final answer", out); /* most recent assistant row */
   free(out);

   /* Ownership: another user cannot read it. */
   out = NULL;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_last_assistant_text(job, bob_id, &out));
   TEST_ASSERT_NULL(out); /* bob's JOIN matches no rows */
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_create_and_get);
   RUN_TEST(test_status_transitions);
   RUN_TEST(test_pending_followups_scan);
   RUN_TEST(test_active_scan);
   RUN_TEST(test_ownership_isolation);
   RUN_TEST(test_get_status_probe);
   RUN_TEST(test_mark_fired_many);
   RUN_TEST(test_bump_reinvoke);
   RUN_TEST(test_fire_boot_reinvokes);
   RUN_TEST(test_last_assistant_text);
   return UNITY_END();
}
