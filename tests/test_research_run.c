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
 * Unit tests for the deep-research controller's deterministic core
 * (src/tools/research_run.c): coverage-driven question promotion, the P0
 * continue/stop decision (budgets + all-closed), and the bounded round-digest
 * renderer (content + cap enforcement).
 */

#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "tools/research_run.h"
#include "unity.h"

static int uid = 0;
static int64_t conv = 0;
static int64_t run = 0;

void setUp(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("alice", "h", true));
   auth_user_t u;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user("alice", &u));
   uid = u.id;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(uid, "research", &conv));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         research_db_run_create(uid, conv, "why is the sky blue?", "web", &run));
}

void tearDown(void) {
   auth_db_shutdown();
}

/* ── defaults ───────────────────────────────────────────────────────────────── */

static void test_budgets_defaults(void) {
   research_budgets_t b;
   research_budgets_defaults(&b);
   TEST_ASSERT_EQUAL_INT(RESEARCH_DEFAULT_MAX_ROUNDS, b.max_rounds);
   TEST_ASSERT_EQUAL_INT(RESEARCH_DEFAULT_MIN_SOURCES, b.min_sources);
   TEST_ASSERT_EQUAL_INT(RESEARCH_DEFAULT_ROUND_DIGEST_MAX_CHARS, b.round_digest_max_chars);
   TEST_ASSERT_EQUAL_INT(RESEARCH_DEFAULT_SATURATION_ROUNDS, b.saturation_rounds);
}

/* ── coverage promotion: open→answered at >= min_sources distinct URLs ───────── */

static void test_refresh_coverage_promotes(void) {
   int64_t q1 = 0, q2 = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_question_add(run, "q1", 0, &q1));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_question_add(run, "q2", 0, &q2));

   /* q1 gets two DISTINCT sources; q2 gets one (plus a dup of the same url). */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         research_db_claim_add(run, q1, "c", "http://a", "web", NULL, 0));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         research_db_claim_add(run, q1, "c", "http://b", "web", NULL, 0));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         research_db_claim_add(run, q2, "c", "http://x", "web", NULL, 0));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         research_db_claim_add(run, q2, "c", "http://x", "web", NULL, 0));

   int closed = -1, total = -1;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_refresh_coverage(run, 2, &closed, &total));
   TEST_ASSERT_EQUAL_INT(2, total);
   TEST_ASSERT_EQUAL_INT(1, closed); /* q1 answered (2 distinct); q2 still open (1) */

   research_question_t qs[8];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_question_list(run, qs, 8, &n));
   TEST_ASSERT_EQUAL_STRING("answered", qs[0].status);
   TEST_ASSERT_EQUAL_STRING("open", qs[1].status);
}

/* ── staleness: auto-retire a question that gains no new source for N rounds ──── */

static void test_retire_stale_questions(void) {
   int64_t q_stale = 0, q_progress = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_question_add(run, "stuck", 0, &q_stale));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_question_add(run, "moving", 0, &q_progress));

   research_stale_entry_t tracker[8];
   int tracker_n = 0;
   int64_t retired[8];
   int retired_n = -1;
   const int threshold = 2;

   /* Round 1 — first observation seeds baselines, never a dry round. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         research_retire_stale_questions(run, 2, threshold, tracker, &tracker_n, 8,
                                                         retired, 8, &retired_n));
   TEST_ASSERT_EQUAL_INT(0, retired_n);
   TEST_ASSERT_EQUAL_INT(2, tracker_n);

   /* q_progress gains a source between rounds; q_stale gains nothing. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         research_db_claim_add(run, q_progress, "c", "http://p1", "web", NULL, 1));

   /* Round 2 — q_stale dry (streak 1, below threshold); q_progress made progress. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         research_retire_stale_questions(run, 2, threshold, tracker, &tracker_n, 8,
                                                         retired, 8, &retired_n));
   TEST_ASSERT_EQUAL_INT(0, retired_n);

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         research_db_claim_add(run, q_progress, "c", "http://p2", "web", NULL, 2));

   /* Round 3 — q_stale hits the threshold (2 dry rounds) → retired; q_progress spared. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         research_retire_stale_questions(run, 2, threshold, tracker, &tracker_n, 8,
                                                         retired, 8, &retired_n));
   TEST_ASSERT_EQUAL_INT(1, retired_n);
   TEST_ASSERT_EQUAL_INT(q_stale, retired[0]);

   research_question_t qs[8];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_question_list(run, qs, 8, &n));
   TEST_ASSERT_EQUAL_STRING("unanswerable", qs[0].status); /* q_stale retired */
   TEST_ASSERT_EQUAL_STRING("open", qs[1].status);         /* q_progress untouched */

   /* Round 4 — the retired question is no longer 'open', so it is not re-counted. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         research_retire_stale_questions(run, 2, threshold, tracker, &tracker_n, 8,
                                                         retired, 8, &retired_n));
   TEST_ASSERT_EQUAL_INT(0, retired_n);

   /* threshold 0 disables the feature entirely (no-op even on a dry question). */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         research_retire_stale_questions(run, 2, 0, tracker, &tracker_n, 8, retired,
                                                         8, &retired_n));
   TEST_ASSERT_EQUAL_INT(0, retired_n);
}

/* ── stop decision: budgets + all-closed + saturation + conclude ────────────── */

static void test_should_stop_decision(void) {
   research_budgets_t b;
   research_budgets_defaults(&b); /* saturation_rounds = 1 by default */

   research_run_t r;
   memset(&r, 0, sizeof(r));

   /* fresh, nothing closed, making progress → continue */
   TEST_ASSERT_NULL(research_should_stop(&r, &b, false, 0, false));

   /* all closed → coverage */
   TEST_ASSERT_EQUAL_STRING("coverage", research_should_stop(&r, &b, true, 0, false));

   /* rounds budget hit → budget */
   r.rounds_run = b.max_rounds;
   TEST_ASSERT_EQUAL_STRING("budget", research_should_stop(&r, &b, false, 0, false));

   /* token ceiling hit → token_budget (max_tool_calls was retired — no tool-call fuse) */
   memset(&r, 0, sizeof(r));
   r.input_tokens = b.max_input_tokens + 1;
   TEST_ASSERT_EQUAL_STRING("token_budget", research_should_stop(&r, &b, false, 0, false));

   /* saturation: one dry round (>= saturation_rounds=1) → saturation */
   memset(&r, 0, sizeof(r));
   TEST_ASSERT_EQUAL_STRING("saturation", research_should_stop(&r, &b, false, 1, false));
   /* below the threshold still continues */
   TEST_ASSERT_NULL(research_should_stop(&r, &b, false, 0, false));
   /* saturation_rounds=0 disables the stop entirely */
   b.saturation_rounds = 0;
   TEST_ASSERT_NULL(research_should_stop(&r, &b, false, 5, false));
   b.saturation_rounds = 1;

   /* conclude signal → concluded */
   TEST_ASSERT_EQUAL_STRING("concluded", research_should_stop(&r, &b, false, 0, true));

   /* precedence: concluded > coverage > saturation > budget when they coincide.
    * This is the run-2 case — a dry round that also blew the token ceiling reports
    * "saturation" (the real reason), not "token_budget". */
   r.input_tokens = b.max_input_tokens + 1;
   TEST_ASSERT_EQUAL_STRING("saturation", research_should_stop(&r, &b, false, 1, false));
   TEST_ASSERT_EQUAL_STRING("coverage", research_should_stop(&r, &b, true, 1, false));
   TEST_ASSERT_EQUAL_STRING("concluded", research_should_stop(&r, &b, true, 1, true));
}

/* ── digest content: brief + open questions + coverage roll-up + revision ────── */

static void test_digest_content(void) {
   int64_t q1 = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         research_db_question_add(run, "what scatters light?", 0, &q1));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         research_db_claim_add(run, q1, "c", "http://a", "web", NULL, 0));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         research_db_revision_add(run, 0, "Rayleigh scattering is the mechanism."));

   research_budgets_t b;
   research_budgets_defaults(&b);
   char digest[8192];
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_render_round_digest(run, "why is the sky blue?",
                                                                       &b, digest, sizeof(digest)));
   TEST_ASSERT_NOT_NULL(strstr(digest, "why is the sky blue?")); /* brief */
   TEST_ASSERT_NOT_NULL(strstr(digest, "what scatters light?")); /* open question */
   TEST_ASSERT_NOT_NULL(strstr(digest, "sources 1/2"));          /* coverage roll-up */
   TEST_ASSERT_NOT_NULL(strstr(digest, "Rayleigh scattering"));  /* last revision prose */
}

/* ── digest is bounded: a small cap truncates and stays NUL-terminated ───────── */

static void test_digest_respects_cap(void) {
   for (int i = 0; i < 40; i++) {
      char q[64];
      snprintf(q, sizeof(q), "a fairly long research sub-question number %d here", i);
      research_db_question_add(run, q, 0, NULL);
   }
   research_budgets_t b;
   research_budgets_defaults(&b);
   b.round_digest_max_chars = 300; /* tight cap */
   b.top_k_questions = 100;        /* would blow past 300 without the cap */

   char digest[8192];
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         research_render_round_digest(run, "brief", &b, digest, sizeof(digest)));
   /* Strict < cap proves the cap actually ENGAGED (40 long questions would blow
    * well past 300 uncapped) — not merely that the content happened to fit. */
   TEST_ASSERT_LESS_THAN_UINT(300u, (unsigned)strlen(digest));
   TEST_ASSERT_NOT_NULL(strstr(digest, "brief")); /* rendered content before truncating */
}

/* ── report render: view over claims, grouped by question, with sources ──────── */

static void test_render_report(void) {
   int64_t q1 = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         research_db_question_add(run, "what scatters blue light?", 0, &q1));
   TEST_ASSERT_EQUAL_INT(
       AUTH_DB_SUCCESS,
       research_db_claim_add(run, q1, "Rayleigh scattering favors short wavelengths", "http://a",
                             "web", "shorter wavelengths scatter more", 0));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, /* a general (question_id 0) claim */
                         research_db_claim_add(run, 0, "The effect is stronger at midday", NULL,
                                               "web", NULL, 1));

   char *md = NULL;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_render_report(run, "why is the sky blue?", &md));
   TEST_ASSERT_NOT_NULL(md);
   TEST_ASSERT_NOT_NULL(strstr(md, "why is the sky blue?"));         /* brief */
   TEST_ASSERT_NOT_NULL(strstr(md, "## what scatters blue light?")); /* question heading */
   TEST_ASSERT_NOT_NULL(strstr(md, "Rayleigh scattering"));          /* claim text */
   TEST_ASSERT_NOT_NULL(strstr(md, "[source](http://a)"));           /* source link */
   TEST_ASSERT_NOT_NULL(strstr(md, "## General findings"));          /* q0 grouping */
   free(md);
}

/* ── report render on an empty run yields a "no findings" note ────────────────── */

static void test_render_report_empty(void) {
   char *md = NULL;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_render_report(run, "empty brief", &md));
   TEST_ASSERT_NOT_NULL(md);
   TEST_ASSERT_NOT_NULL(strstr(md, "No findings"));
   free(md);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_budgets_defaults);
   RUN_TEST(test_refresh_coverage_promotes);
   RUN_TEST(test_retire_stale_questions);
   RUN_TEST(test_should_stop_decision);
   RUN_TEST(test_digest_content);
   RUN_TEST(test_digest_respects_cap);
   RUN_TEST(test_render_report);
   RUN_TEST(test_render_report_empty);
   return UNITY_END();
}
