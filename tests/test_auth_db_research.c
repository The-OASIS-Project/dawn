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
 * Unit tests for the deep-research accessor layer (src/auth/auth_db_research.c):
 * run create/get round-trip + ownership isolation, status/progress/terminal
 * setters, the coverage ledger (questions), COUNT(DISTINCT source_url) coverage
 * semantics (dedup + NULL exclusion), batched claim ingest, and report-revision
 * latest/prune.
 */

#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "unity.h"

static int alice_id = 0;
static int bob_id = 0;
static int64_t conv = 0;

void setUp(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("alice", "h", true));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_create_user("bob", "h", false));
   auth_user_t u;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user("alice", &u));
   alice_id = u.id;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_get_user("bob", &u));
   bob_id = u.id;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "research conv", &conv));
}

void tearDown(void) {
   auth_db_shutdown();
}

/* ── run create → get round-trips; ownership isolates; by-conversation works ── */

static void test_run_create_get_ownership(void) {
   int64_t run = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_run_create(
                                              alice_id, conv, "why is the sky blue?", "web", &run));
   TEST_ASSERT_TRUE(run > 0);

   research_run_t r;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_run_get(run, alice_id, &r));
   TEST_ASSERT_EQUAL_INT64(run, r.id);
   TEST_ASSERT_EQUAL_INT64(conv, r.conversation_id);
   TEST_ASSERT_EQUAL_INT(alice_id, r.user_id);
   TEST_ASSERT_EQUAL_STRING("why is the sky blue?", r.brief);
   TEST_ASSERT_EQUAL_STRING("web", r.mode);
   TEST_ASSERT_EQUAL_STRING("planning", r.status);
   TEST_ASSERT_EQUAL_INT64(0, r.report_doc_id);
   TEST_ASSERT_EQUAL_INT64(0, r.finished_at);

   /* bob cannot read alice's run. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, research_db_run_get(run, bob_id, &r));

   /* system-caller by-conversation lookup. */
   research_run_t r2;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_run_get_by_conversation(conv, &r2));
   TEST_ASSERT_EQUAL_INT64(run, r2.id);
}

/* ── mode default; UNIQUE(conversation_id) rejects a second run per conv ─────── */

static void test_run_mode_default_and_unique(void) {
   int64_t run = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_run_create(alice_id, conv, "q", NULL, &run));
   research_run_t r;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_run_get(run, alice_id, &r));
   TEST_ASSERT_EQUAL_STRING("web", r.mode); /* NULL mode -> 'web' */

   int64_t dup = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_FAILURE,
                         research_db_run_create(alice_id, conv, "q2", "web", &dup));
}

/* ── status / progress / report_doc / terminal setters ──────────────────────── */

static void test_run_setters(void) {
   int64_t run = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_run_create(alice_id, conv, "q", "web", &run));

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_run_set_status(run, "researching"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_run_update_progress(run, 3, 12, 45000));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_run_set_report_doc(run, 777));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         research_db_run_set_terminal(run, "done", "coverage", 1234567));

   research_run_t r;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_run_get(run, alice_id, &r));
   TEST_ASSERT_EQUAL_STRING("done", r.status);
   TEST_ASSERT_EQUAL_STRING("coverage", r.stop_reason);
   TEST_ASSERT_EQUAL_INT(3, r.rounds_run);
   TEST_ASSERT_EQUAL_INT(12, r.tool_calls);
   TEST_ASSERT_EQUAL_INT64(45000, r.input_tokens);
   TEST_ASSERT_EQUAL_INT64(777, r.report_doc_id);
   TEST_ASSERT_EQUAL_INT64(1234567, r.finished_at);
}

/* ── questions: add / list / set_status / parent_qid ────────────────────────── */

static void test_questions(void) {
   int64_t run = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_run_create(alice_id, conv, "q", "web", &run));

   int64_t q1 = 0, q2 = 0, sub = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_question_add(run, "top A", 0, &q1));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_question_add(run, "top B", 0, &q2));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_question_add(run, "sub of A", q1, &sub));

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_question_set_status(q1, "answered", 0.9));

   research_question_t out[8];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_question_list(run, out, 8, &n));
   TEST_ASSERT_EQUAL_INT(3, n);
   TEST_ASSERT_EQUAL_STRING("top A", out[0].question);
   TEST_ASSERT_EQUAL_STRING("answered", out[0].status);
   TEST_ASSERT_EQUAL_FLOAT(0.9, out[0].confidence);
   TEST_ASSERT_EQUAL_INT64(0, out[0].parent_qid);
   TEST_ASSERT_EQUAL_STRING("open", out[1].status);
   TEST_ASSERT_EQUAL_INT64(q1, out[2].parent_qid); /* sub links to A */
}

/* ── coverage = COUNT(DISTINCT source_url): dedup + NULL exclusion ───────────── */

static void test_coverage_distinct_and_null(void) {
   int64_t run = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_run_create(alice_id, conv, "q", "web", &run));
   int64_t qid = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_question_add(run, "the question", 0, &qid));

   /* two claims from the SAME url (counts once), one from a distinct url, one
    * with a NULL source (private/memory — excluded from the distinct count). */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         research_db_claim_add(run, qid, "c1", "http://a", "web", "qa", 0));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         research_db_claim_add(run, qid, "c2", "http://a", "web", "qb", 0));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         research_db_claim_add(run, qid, "c3", "http://b", "web", "qc", 1));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         research_db_claim_add(run, qid, "c4", NULL, "memory", NULL, 1));

   int cov = -1;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_question_coverage(run, qid, &cov));
   TEST_ASSERT_EQUAL_INT(2, cov); /* http://a (dedup) + http://b; NULL excluded */
}

/* ── batched claim ingest + count + list ordering ───────────────────────────── */

static void test_claims_batch_count_list(void) {
   int64_t run = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_run_create(alice_id, conv, "q", "web", &run));
   int64_t qa = 0, qb = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_question_add(run, "A", 0, &qa));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_question_add(run, "B", 0, &qb));

   research_claim_t batch[3];
   memset(batch, 0, sizeof(batch));
   batch[0].question_id = qb;
   strcpy(batch[0].claim, "b-claim");
   strcpy(batch[0].source_url, "http://b");
   batch[0].round = 2;
   batch[1].question_id = qa;
   strcpy(batch[1].claim, "a-claim");
   strcpy(batch[1].source_url, "http://a");
   batch[1].round = 2;
   batch[2].question_id = 0; /* general */
   strcpy(batch[2].claim, "gen-claim");
   batch[2].round = 2;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_claims_add(run, batch, 3));

   /* an all-empty batch (no claim text anywhere) is rejected, not a silent no-op. */
   research_claim_t empties[2];
   memset(empties, 0, sizeof(empties));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, research_db_claims_add(run, empties, 2));

   int count = -1;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_claim_count(run, &count));
   TEST_ASSERT_EQUAL_INT(3, count); /* still 3 — the empty batch inserted nothing */

   research_claim_t out[8];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_claim_list(run, out, 8, &n));
   TEST_ASSERT_EQUAL_INT(3, n);
   /* ordered by question_id ASC (0/general first), then id ASC. */
   TEST_ASSERT_EQUAL_INT64(0, out[0].question_id);
   TEST_ASSERT_EQUAL_STRING("gen-claim", out[0].claim);
   TEST_ASSERT_EQUAL_INT64(qa, out[1].question_id);
   TEST_ASSERT_EQUAL_STRING("a-claim", out[1].claim);
   TEST_ASSERT_EQUAL_INT64(qb, out[2].question_id);
}

/* ── report revisions: latest returns highest round; prune keeps only it ────── */

static void test_revisions_latest_and_prune(void) {
   int64_t run = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_run_create(alice_id, conv, "q", "web", &run));

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_revision_add(run, 0, "round0"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_revision_add(run, 1, "round1"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_revision_add(run, 2, "round2 FINAL"));

   int rc = -1;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_revision_count(run, &rc));
   TEST_ASSERT_EQUAL_INT(3, rc);

   char *md = NULL;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_revision_get_latest(run, &md));
   TEST_ASSERT_NOT_NULL(md);
   TEST_ASSERT_EQUAL_STRING("round2 FINAL", md);
   free(md);

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_revisions_prune_to_latest(run));
   /* the two earlier rounds are actually deleted — only the final remains. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_revision_count(run, &rc));
   TEST_ASSERT_EQUAL_INT(1, rc);
   md = NULL;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_revision_get_latest(run, &md));
   TEST_ASSERT_EQUAL_STRING("round2 FINAL", md);
   free(md);
}

/* ── get on a missing run / revision returns NOT_FOUND ──────────────────────── */

static void test_not_found(void) {
   research_run_t r;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, research_db_run_get(99999, alice_id, &r));
   int64_t run = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, research_db_run_create(alice_id, conv, "q", "web", &run));
   char *md = NULL;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, research_db_revision_get_latest(run, &md));
   TEST_ASSERT_NULL(md);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_run_create_get_ownership);
   RUN_TEST(test_run_mode_default_and_unique);
   RUN_TEST(test_run_setters);
   RUN_TEST(test_questions);
   RUN_TEST(test_coverage_distinct_and_null);
   RUN_TEST(test_claims_batch_count_list);
   RUN_TEST(test_revisions_latest_and_prune);
   RUN_TEST(test_not_found);
   return UNITY_END();
}
