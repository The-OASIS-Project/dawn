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
                                            "telegram-main", 1, "go research X thoroughly", &job));
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
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create_job(alice_id, "j", 0, "detached", "notify",
                                                             NULL, 1, "goal text", &job));

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
   conv_db_create_job(alice_id, "j1", 0, "detached", "notify", NULL, 1, "goal text", &j1);
   conv_db_create_job(alice_id, "j2", 0, "detached", "notify", NULL, 1, "goal text", &j2);
   conv_db_create_job(alice_id, "j3", 0, "detached", "notify", NULL, 1, "goal text", &j3);

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
   conv_db_create_job(alice_id, "q", 0, "detached", "notify", NULL, 1, "goal text",
                      &j1); /* queued */
   conv_db_create_job(alice_id, "r", 0, "detached", "notify", NULL, 1, "goal text", &j2);
   conv_db_create_job(alice_id, "d", 0, "detached", "notify", NULL, 1, "goal text", &j3);
   conv_db_job_set_running(j2, 100);
   conv_db_job_set_terminal(j3, "done", NULL, 100);

   job_record_t out[8];
   int n = -1;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_scan_active(out, 8, &n));
   TEST_ASSERT_EQUAL_INT(2, n); /* queued + running, not done */
}

/* ── the lifetime split: active is a SET, history is a PAGE ───────────────────
 *
 * These two readers partition a user's jobs by lifecycle state, and the WebUI
 * frames depend on that partition being exact: the client derives its "N jobs
 * running" counts from the active set, so a row appearing in both lists (or in
 * neither) shows up as a wrong count rather than as an obvious failure. */

static void test_active_and_history_partition(void) {
   int64_t q = 0, r = 0, d = 0, f = 0;
   conv_db_create_job(alice_id, "queued", 0, "detached", "notify", NULL, 1, "goal text", &q);
   conv_db_create_job(alice_id, "running", 0, "detached", "notify", NULL, 1, "goal text", &r);
   conv_db_create_job(alice_id, "done", 0, "detached", "notify", NULL, 1, "goal text", &d);
   conv_db_create_job(alice_id, "failed", 0, "detached", "notify", NULL, 1, "goal text", &f);
   conv_db_job_set_running(r, 100);
   conv_db_job_set_terminal(d, "done", NULL, 200);
   conv_db_job_set_terminal(f, "failed", "boom", 200);

   job_record_t out[8];
   int n = -1;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_list_active_by_user(alice_id, out, 8, &n));
   TEST_ASSERT_EQUAL_INT(2, n); /* queued + running */

   int m = -1;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_job_list_history_by_user(alice_id, 0, 0, out, 8, &m));
   TEST_ASSERT_EQUAL_INT(2, m); /* done + failed */

   /* Every job lands in exactly one list. */
   TEST_ASSERT_EQUAL_INT(4, n + m);

   /* Ownership: bob sees neither list. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_list_active_by_user(bob_id, out, 8, &n));
   TEST_ASSERT_EQUAL_INT(0, n);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_job_list_history_by_user(bob_id, 0, 0, out, 8, &m));
   TEST_ASSERT_EQUAL_INT(0, m);
}

/* A non-job conversation is in neither list — job_status IS NULL for ~every row
 * in the table, so a missing predicate would flood both. */
static void test_active_and_history_ignore_plain_conversations(void) {
   int64_t chat = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "just a chat", &chat));

   job_record_t out[8];
   int n = -1;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_list_active_by_user(alice_id, out, 8, &n));
   TEST_ASSERT_EQUAL_INT(0, n);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_job_list_history_by_user(alice_id, 0, 0, out, 8, &n));
   TEST_ASSERT_EQUAL_INT(0, n);
}

/* Keyset pagination walks the whole history exactly once.  Every job here is
 * created in the same second, so this pins the (created_at, id) tiebreak: with
 * created_at alone the cursor would either loop forever or skip the ties. */
static void test_history_pagination_no_gaps_or_dupes(void) {
   int64_t ids[5] = { 0 };
   for (int i = 0; i < 5; i++) {
      char t[16];
      snprintf(t, sizeof(t), "j%d", i);
      conv_db_create_job(alice_id, t, 0, "detached", "notify", NULL, 1, "goal text", &ids[i]);
      conv_db_job_set_terminal(ids[i], "done", NULL, 200);
   }

   bool seen[5] = { false };
   int total = 0;
   int64_t cursor_at = 0, cursor_id = 0;
   for (int page = 0; page < 10; page++) { /* bounded: a stuck cursor fails loudly */
      job_record_t out[2];
      int n = -1;
      TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_list_history_by_user(
                                                 alice_id, cursor_at, cursor_id, out, 2, &n));
      if (n == 0) {
         break;
      }
      for (int i = 0; i < n; i++) {
         for (int k = 0; k < 5; k++) {
            if (ids[k] == out[i].id) {
               TEST_ASSERT_FALSE(seen[k]); /* no row served twice */
               seen[k] = true;
            }
         }
         total++;
      }
      cursor_at = (int64_t)out[n - 1].created_at;
      cursor_id = out[n - 1].id;
   }
   TEST_ASSERT_EQUAL_INT(5, total); /* no row skipped either */
   for (int k = 0; k < 5; k++) {
      TEST_ASSERT_TRUE(seen[k]);
   }
}

/* ── resume claim: atomic, state-gated, and re-arms delivery ──────────────────
 *
 * The claim being atomic is what stops two clicks (or two tabs) from both
 * spawning a worker onto the same job.  Re-arming on_complete_fired is what
 * stops a resumed job from finishing and notifying nobody — the interrupted
 * notice already consumed the flag, and the monitor skips a fired row. */

static void test_resume_claim_is_exclusive(void) {
   int64_t job = 0;
   conv_db_create_job(alice_id, "j", 0, "detached", "notify", NULL, 1, "goal text", &job);
   conv_db_job_set_terminal(job, "interrupted", "daemon restarted", 100);
   conv_db_job_mark_fired(job); /* the interrupted-notify consumed the flag */

   /* First caller claims it. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_reset_for_resume(job, alice_id));

   job_record_t r;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_get(job, alice_id, &r));
   TEST_ASSERT_EQUAL_STRING("queued", r.job_status);
   TEST_ASSERT_EQUAL_STRING("", r.job_error);
   TEST_ASSERT_EQUAL_INT64(0, (int64_t)r.finished_at);
   TEST_ASSERT_EQUAL_INT64(0, (int64_t)r.started_at);
   TEST_ASSERT_FALSE(r.on_complete_fired); /* delivery re-armed */

   /* Second caller loses: the row already left the resumable set. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, conv_db_job_reset_for_resume(job, alice_id));
}

/* The goal is durable from CREATE, not from a successful dispatch (v74).
 *
 * This is what makes a job that died before it ever ran resumable at all: its
 * conversation has no messages, so before v74 the instruction existed nowhere
 * and resuming re-engaged the model with no task — producing an empty `done`
 * that, being terminal, could never be retried. */
static void test_goal_is_stored_at_create(void) {
   int64_t job = 0;
   conv_db_create_job(alice_id, "t", 0, "detached", "notify", NULL, 1,
                      "research the 2026 season and summarize", &job);

   char *goal = NULL;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_get_goal(job, alice_id, &goal));
   TEST_ASSERT_EQUAL_STRING("research the 2026 season and summarize", goal);
   free(goal);

   /* Ownership-scoped like every other user-facing read. */
   goal = NULL;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, conv_db_job_get_goal(job, bob_id, &goal));
   TEST_ASSERT_NULL(goal);

   /* A job created without one (pre-v74 shape) reports NOT_FOUND, not an empty
    * string — that distinction is what the resume guard keys on. */
   int64_t bare = 0;
   conv_db_create_job(alice_id, "t", 0, "detached", "notify", NULL, 1, NULL, &bare);
   goal = NULL;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, conv_db_job_get_goal(bare, alice_id, &goal));
   TEST_ASSERT_NULL(goal);

   /* And a non-job conversation never yields one. */
   int64_t chat = 0;
   conv_db_create(alice_id, "chat", &chat);
   goal = NULL;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, conv_db_job_get_goal(chat, alice_id, &goal));
   TEST_ASSERT_NULL(goal);
}

/* The claim is self-authorizing: a foreign owner cannot claim the row even if a
 * caller skipped its own ownership check. */
static void test_resume_claim_is_ownership_scoped(void) {
   int64_t job = 0;
   conv_db_create_job(alice_id, "j", 0, "detached", "notify", NULL, 1, "goal text", &job);
   conv_db_job_set_terminal(job, "interrupted", "daemon restarted", 100);

   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, conv_db_job_reset_for_resume(job, bob_id));
   /* …and alice's claim still works afterwards — bob's attempt changed nothing. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_reset_for_resume(job, alice_id));
}

/* set_running is a claim on 'queued', not a blind write.  A cancel that retires
 * a queued row before its detached worker is scheduled must WIN — otherwise the
 * worker resurrects the job and it runs after the user was told it stopped. */
static void test_set_running_loses_to_a_cancel(void) {
   int64_t job = 0;
   conv_db_create_job(alice_id, "j", 0, "detached", "notify", NULL, 1, "goal text", &job);

   /* Cancel lands first (row leaves 'queued'). */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_set_terminal(job, "cancelled", NULL, 100));
   /* The worker then wakes and tries to claim: it must lose. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, conv_db_job_set_running(job, 200));

   job_record_t r;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_get(job, alice_id, &r));
   TEST_ASSERT_EQUAL_STRING("cancelled", r.job_status); /* not resurrected */

   /* And two workers cannot both claim one queued row. */
   int64_t j2 = 0;
   conv_db_create_job(alice_id, "j2", 0, "detached", "notify", NULL, 1, "goal text", &j2);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_set_running(j2, 100));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, conv_db_job_set_running(j2, 200));
}

static void test_resume_rejects_non_resumable_states(void) {
   int64_t done = 0, cancelled = 0, running = 0, failed = 0;
   conv_db_create_job(alice_id, "done", 0, "detached", "notify", NULL, 1, "goal text", &done);
   conv_db_create_job(alice_id, "cancelled", 0, "detached", "notify", NULL, 1, "goal text",
                      &cancelled);
   conv_db_create_job(alice_id, "running", 0, "detached", "notify", NULL, 1, "goal text", &running);
   conv_db_create_job(alice_id, "failed", 0, "detached", "notify", NULL, 1, "goal text", &failed);
   conv_db_job_set_terminal(done, "done", NULL, 100);
   conv_db_job_set_terminal(cancelled, "cancelled", NULL, 100);
   conv_db_job_set_running(running, 100);
   conv_db_job_set_terminal(failed, "failed", "job capacity reached", 100);

   /* A finished job has an answer; a cancelled one was stopped deliberately; a
    * running one is already going.  Only interrupted/failed are resumable. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, conv_db_job_reset_for_resume(done, alice_id));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, conv_db_job_reset_for_resume(cancelled, alice_id));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, conv_db_job_reset_for_resume(running, alice_id));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_reset_for_resume(failed, alice_id));

   /* A plain conversation is not a job and must never be resumable. */
   int64_t chat = 0;
   conv_db_create(alice_id, "just a chat", &chat);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, conv_db_job_reset_for_resume(chat, alice_id));
}

/* A resumed job rejoins the ACTIVE set and leaves history — the partition has to
 * survive a backwards transition, not just forward ones. */
static void test_resume_moves_row_back_to_active(void) {
   int64_t job = 0;
   conv_db_create_job(alice_id, "j", 0, "detached", "notify", NULL, 1, "goal text", &job);
   conv_db_job_set_terminal(job, "interrupted", "daemon restarted", 100);

   job_record_t out[8];
   int n = -1, m = -1;
   conv_db_job_list_active_by_user(alice_id, out, 8, &n);
   conv_db_job_list_history_by_user(alice_id, 0, 0, out, 8, &m);
   TEST_ASSERT_EQUAL_INT(0, n);
   TEST_ASSERT_EQUAL_INT(1, m);

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_job_reset_for_resume(job, alice_id));

   conv_db_job_list_active_by_user(alice_id, out, 8, &n);
   conv_db_job_list_history_by_user(alice_id, 0, 0, out, 8, &m);
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_INT(0, m);
}

/* ── ownership isolation: bob cannot get/see alice's job ───────────────────── */

static void test_ownership_isolation(void) {
   int64_t job = 0;
   conv_db_create_job(alice_id, "secret", 0, "detached", "notify", NULL, 1, "goal text", &job);

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
   conv_db_create_job(alice_id, "j", 0, "detached", "notify", NULL, 1, "goal text", &job);
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
   conv_db_create_job(alice_id, "a", 0, "detached", "reinvoke_parent", NULL, 1, "goal text", &j1);
   conv_db_create_job(alice_id, "b", 0, "detached", "reinvoke_parent", NULL, 1, "goal text", &j2);
   conv_db_create_job(alice_id, "c", 0, "detached", "reinvoke_parent", NULL, 1, "goal text", &j3);
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
   conv_db_create_job(alice_id, "j", 0, "detached", "reinvoke_parent", NULL, 1, "goal text", &job);

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
   conv_db_create_job(alice_id, "reinvoke", 0, "detached", "reinvoke_parent", NULL, 1, "goal text",
                      &rp);
   conv_db_create_job(alice_id, "notify", 0, "detached", "notify", NULL, 1, "goal text", &nf);
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
   conv_db_create_job(alice_id, "j", 0, "detached", "reinvoke_parent", NULL, 1, "goal text", &job);

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
   RUN_TEST(test_active_and_history_partition);
   RUN_TEST(test_active_and_history_ignore_plain_conversations);
   RUN_TEST(test_history_pagination_no_gaps_or_dupes);
   RUN_TEST(test_goal_is_stored_at_create);
   RUN_TEST(test_resume_claim_is_exclusive);
   RUN_TEST(test_resume_claim_is_ownership_scoped);
   RUN_TEST(test_set_running_loses_to_a_cancel);
   RUN_TEST(test_resume_rejects_non_resumable_states);
   RUN_TEST(test_resume_moves_row_back_to_active);
   RUN_TEST(test_ownership_isolation);
   RUN_TEST(test_get_status_probe);
   RUN_TEST(test_mark_fired_many);
   RUN_TEST(test_bump_reinvoke);
   RUN_TEST(test_fire_boot_reinvokes);
   RUN_TEST(test_last_assistant_text);
   return UNITY_END();
}
