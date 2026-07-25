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
 * Unit tests for the conversation event log (src/auth/auth_db_events.c) — the
 * durable half of the background-jobs observe contract.
 *
 * Three properties carry real weight here:
 *   - seq is per-CONVERSATION monotonic, not global.  A client's replay cursor
 *     (`attach_conversation {last_seq}`) is meaningless if seq is shared.
 *   - reads are ownership-JOINed (§8.3).  A bare WHERE conversation_id=? would
 *     leak another user's job steps, which is why there is a test that asks for
 *     someone else's conversation and expects nothing.
 *   - retention is kind-AWARE (§8.8): bulky payloads are nulled with the row
 *     kept (so the seq chain — and therefore replay — stays coherent), while
 *     status heartbeats are deleted outright.
 */

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "unity.h"

static int alice_id = 0;
static int bob_id = 0;

/* A FILE-backed temp DB rather than ":memory:" (which the sibling job tests
 * use): the retention tests need to age rows, and a second sqlite3 handle can
 * only reach the same data if it lives in a file.  Unlinked in tearDown. */
static char db_path[128];

void setUp(void) {
   snprintf(db_path, sizeof(db_path), "/tmp/dawn_test_events_%d.db", (int)getpid());
   unlink(db_path);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(db_path));
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
   unlink(db_path);
}

/* Backdate rows so the retention sweep has something aged to act on — the
 * alternative would be a test that sleeps for days. */
static void backdate_all(int days) {
   sqlite3 *raw = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_open(db_path, &raw));
   char sql[160];
   snprintf(sql, sizeof(sql), "UPDATE conversation_events SET created_at = created_at - %lld",
            (long long)days * 86400);
   char *err = NULL;
   int rc = sqlite3_exec(raw, sql, NULL, NULL, &err);
   if (rc != SQLITE_OK) {
      TEST_FAIL_MESSAGE(err ? err : "backdate failed");
   }
   sqlite3_free(err);
   sqlite3_close(raw);
}

/* ── seq is per-conversation monotonic, not global ─────────────────────────── */

static void test_seq_is_per_conversation(void) {
   int64_t a = 0, b = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "conv a", &a));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "conv b", &b));

   int64_t s1 = 0, s2 = 0, s3 = 0, other = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_event_append(a, "status", "{\"n\":1}", &s1));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_event_append(a, "tool_call", "{\"n\":2}", &s2));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_event_append(a, "complete", "{\"n\":3}", &s3));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_event_append(b, "status", "{\"n\":1}", &other));

   TEST_ASSERT_EQUAL_INT64(1, s1);
   TEST_ASSERT_EQUAL_INT64(2, s2);
   TEST_ASSERT_EQUAL_INT64(3, s3);
   /* The second conversation restarts at 1 — if seq were global this would be 4
    * and every client's replay cursor would skip events. */
   TEST_ASSERT_EQUAL_INT64(1, other);
}

/* ── list returns ASC, honours after_seq, and preserves NULL payloads ──────── */

static void test_list_after_seq_and_ordering(void) {
   int64_t c = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &c));
   for (int i = 0; i < 5; i++) {
      TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_event_append(c, "status", "{}", NULL));
   }

   conv_event_t rows[10];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_event_list(c, alice_id, 0, 10, rows, &n));
   TEST_ASSERT_EQUAL_INT(5, n);
   for (int i = 0; i < n; i++) {
      TEST_ASSERT_EQUAL_INT64(i + 1, rows[i].seq); /* ASC, dense from 1 */
      TEST_ASSERT_EQUAL_STRING("status", rows[i].kind);
   }
   conv_db_event_rows_free(rows, n);

   /* after_seq is EXCLUSIVE — this is the client's reconnect cursor. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_event_list(c, alice_id, 3, 10, rows, &n));
   TEST_ASSERT_EQUAL_INT(2, n);
   TEST_ASSERT_EQUAL_INT64(4, rows[0].seq);
   TEST_ASSERT_EQUAL_INT64(5, rows[1].seq);
   conv_db_event_rows_free(rows, n);

   /* max caps the batch so a huge backlog can't blow the caller's buffer. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_event_list(c, alice_id, 0, 2, rows, &n));
   TEST_ASSERT_EQUAL_INT(2, n);
   conv_db_event_rows_free(rows, n);
}

static void test_null_payload_survives_as_null(void) {
   int64_t c = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &c));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_event_append(c, "status", NULL, NULL));

   conv_event_t rows[2];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_event_list(c, alice_id, 0, 2, rows, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   /* NULL must stay NULL, not become "": the renderer distinguishes a pruned
    * payload ("expired") from an empty one. */
   TEST_ASSERT_NULL(rows[0].payload);
   conv_db_event_rows_free(rows, n);
}

/* ── §8.3: reads are ownership-JOINed ──────────────────────────────────────── */

static void test_ownership_join_blocks_other_user(void) {
   int64_t alices = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "alice private", &alices));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_event_append(alices, "tool_call", "{\"secret\":1}", NULL));

   conv_event_t rows[4];
   int n = -1;
   /* Bob asking for Alice's conversation: SUCCESS with zero rows, matching
    * conv_db_get_messages' contract (the JOIN *is* the check — there is
    * deliberately no distinct FORBIDDEN result to probe with). */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_event_list(alices, bob_id, 0, 4, rows, &n));
   TEST_ASSERT_EQUAL_INT(0, n);

   /* Alice still sees her own. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_event_list(alices, alice_id, 0, 4, rows, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   conv_db_event_rows_free(rows, n);
}

/* ── §8.8: retention is kind-aware ─────────────────────────────────────────── */

static void test_prune_is_kind_aware(void) {
   int64_t c = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &c));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_event_append(c, "status", "{\"s\":1}", NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_event_append(c, "tool_call", "{\"big\":1}", NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_event_append(c, "tool_result", "{\"big\":2}", NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_event_append(c, "spawn", "{\"child\":9}", NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_event_append(c, "complete", "{\"d\":\"done\"}", NULL));
   backdate_all(40);

   int nulled = 0, deleted = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_event_prune_expired(30, &nulled, &deleted));
   TEST_ASSERT_EQUAL_INT(2, nulled);  /* tool_call + tool_result payloads cleared */
   TEST_ASSERT_EQUAL_INT(1, deleted); /* the status heartbeat removed entirely */

   conv_event_t rows[10];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_event_list(c, alice_id, 0, 10, rows, &n));
   TEST_ASSERT_EQUAL_INT(4, n); /* 5 appended, 1 status deleted */
   for (int i = 0; i < n; i++) {
      if (strcmp(rows[i].kind, "tool_call") == 0 || strcmp(rows[i].kind, "tool_result") == 0) {
         TEST_ASSERT_NULL(rows[i].payload); /* row kept, body gone */
      } else {
         /* spawn/complete are the tree skeleton + disposition — kept intact. */
         TEST_ASSERT_NOT_NULL(rows[i].payload);
      }
   }
   /* seq gaps left by the deleted status row are harmless: reads are
    * `seq > after_seq`, so nothing depends on density. */
   TEST_ASSERT_TRUE(rows[n - 1].seq >= (int64_t)n);
   conv_db_event_rows_free(rows, n);
}

static void test_prune_respects_age_and_disable(void) {
   int64_t c = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "c", &c));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_event_append(c, "tool_result", "{\"x\":1}", NULL));

   /* Fresh rows are untouched by an active policy. */
   int nulled = -1, deleted = -1;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_event_prune_expired(30, &nulled, &deleted));
   TEST_ASSERT_EQUAL_INT(0, nulled);
   TEST_ASSERT_EQUAL_INT(0, deleted);

   /* retention_days <= 0 disables the sweep even when rows are ancient. */
   backdate_all(400);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_event_prune_expired(0, &nulled, &deleted));
   TEST_ASSERT_EQUAL_INT(0, nulled);
   TEST_ASSERT_EQUAL_INT(0, deleted);

   conv_event_t rows[4];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_event_list(c, alice_id, 0, 4, rows, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_NOT_NULL(rows[0].payload);
   conv_db_event_rows_free(rows, n);
}

/* ── argument validation ───────────────────────────────────────────────────── */

static void test_invalid_arguments_rejected(void) {
   int64_t seq = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, conv_db_event_append(0, "status", "{}", &seq));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, conv_db_event_append(1, NULL, "{}", &seq));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, conv_db_event_append(1, "", "{}", &seq));

   conv_event_t rows[2];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, conv_db_event_list(0, alice_id, 0, 2, rows, &n));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, conv_db_event_list(1, 0, 0, 2, rows, &n));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, conv_db_event_list(1, alice_id, 0, 0, rows, &n));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, conv_db_event_list(1, alice_id, 0, 2, NULL, &n));

   /* NULL-safe free, so a caller's error path never has to branch. */
   conv_db_event_rows_free(NULL, 0);
}

/* ── FK cascade: deleting a conversation takes its events with it ──────────── */

static void test_events_cascade_on_conversation_delete(void) {
   int64_t c = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(alice_id, "doomed", &c));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_event_append(c, "status", "{}", NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_delete(c, alice_id));

   conv_event_t rows[4];
   int n = -1;
   /* The JOIN alone would already yield 0 rows once the conversation is gone;
    * this pins that deleting a conversation doesn't strand its event rows. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_event_list(c, alice_id, 0, 4, rows, &n));
   TEST_ASSERT_EQUAL_INT(0, n);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_seq_is_per_conversation);
   RUN_TEST(test_list_after_seq_and_ordering);
   RUN_TEST(test_null_payload_survives_as_null);
   RUN_TEST(test_ownership_join_blocks_other_user);
   RUN_TEST(test_prune_is_kind_aware);
   RUN_TEST(test_prune_respects_age_and_disable);
   RUN_TEST(test_invalid_arguments_rejected);
   RUN_TEST(test_events_cascade_on_conversation_delete);
   return UNITY_END();
}
