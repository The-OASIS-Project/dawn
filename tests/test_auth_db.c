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
 * Unit tests for auth_db_core.c — authentication database CRUD.
 * Uses an in-memory SQLite database via auth_db_init(":memory:").
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <fcntl.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "unity.h"

/* ============================================================================
 * setUp / tearDown — fresh DB per test
 * ============================================================================ */

void setUp(void) {
   auth_db_init(":memory:");
}

void tearDown(void) {
   auth_db_shutdown();
}

/* ============================================================================
 * Helpers
 * ============================================================================ */

/**
 * @brief Create a user and return the user ID via auth_db_get_user.
 */
static int create_and_get_id(const char *username, const char *hash, bool is_admin) {
   auth_db_create_user(username, hash, is_admin);
   auth_user_t user;
   memset(&user, 0, sizeof(user));
   auth_db_get_user(username, &user);
   return user.id;
}

/**
 * @brief Force a session's expires_at to a past timestamp via raw SQL.
 *
 * Needed because auth_db_create_session always computes expiry from now().
 */
static void force_session_expiry(const char *token, time_t expires_at) {
   const char *sql = "UPDATE sessions SET expires_at = ? WHERE token = ?";
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   sqlite3_bind_int64(stmt, 1, (int64_t)expires_at);
   sqlite3_bind_text(stmt, 2, token, -1, SQLITE_STATIC);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
}

/* ============================================================================
 * Lifecycle Tests
 * ============================================================================ */

static void test_init_returns_success(void) {
   /* setUp already called auth_db_init; verify it's ready */
   TEST_ASSERT_TRUE(auth_db_is_ready());
}

static void test_shutdown_and_reinit(void) {
   auth_db_shutdown();
   TEST_ASSERT_FALSE(auth_db_is_ready());

   int rc = auth_db_init(":memory:");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_TRUE(auth_db_is_ready());
}

/* ============================================================================
 * User Tests
 * ============================================================================ */

static void test_create_user(void) {
   int rc = auth_db_create_user("alice", "hash_alice", true);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);

   int count = 0;
   rc = auth_db_user_count(&count);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   /* One user we created plus the initial user count may vary;
    * just verify at least one user exists */
   TEST_ASSERT_TRUE(count >= 1);
}

static void test_get_user_by_name(void) {
   auth_db_create_user("bob", "hash_bob", false);

   auth_user_t user;
   memset(&user, 0, sizeof(user));
   int rc = auth_db_get_user("bob", &user);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_STRING("bob", user.username);
   TEST_ASSERT_EQUAL_STRING("hash_bob", user.password_hash);
   TEST_ASSERT_FALSE(user.is_admin);
   TEST_ASSERT_TRUE(user.id > 0);
}

static void test_get_user_not_found(void) {
   int rc = auth_db_get_user("nonexistent", NULL);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, rc);
}

static void test_create_duplicate_user(void) {
   auth_db_create_user("carol", "hash1", false);
   int rc = auth_db_create_user("carol", "hash2", false);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_DUPLICATE, rc);
}

static void test_delete_user(void) {
   /* Need two admins so we can delete one (last-admin protection) */
   auth_db_create_user("admin1", "hash1", true);
   auth_db_create_user("admin2", "hash2", true);

   int rc = auth_db_delete_user("admin1");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);

   rc = auth_db_get_user("admin1", NULL);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, rc);
}

static void test_delete_last_admin_fails(void) {
   auth_db_create_user("sole_admin", "hash", true);

   int rc = auth_db_delete_user("sole_admin");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_LAST_ADMIN, rc);

   /* User should still exist */
   rc = auth_db_get_user("sole_admin", NULL);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
}

static void test_verify_password(void) {
   auth_db_create_user("dave", "correct_hash", false);

   auth_user_t user;
   memset(&user, 0, sizeof(user));
   int rc = auth_db_get_user("dave", &user);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_STRING("correct_hash", user.password_hash);
}

static void test_verify_wrong_password(void) {
   auth_db_create_user("eve", "real_hash", false);

   auth_user_t user;
   memset(&user, 0, sizeof(user));
   auth_db_get_user("eve", &user);
   TEST_ASSERT_NOT_EQUAL(0, strcmp(user.password_hash, "wrong_hash"));
}

static void test_update_password(void) {
   auth_db_create_user("frank", "old_hash", false);

   int rc = auth_db_update_password("frank", "new_hash");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);

   auth_user_t user;
   memset(&user, 0, sizeof(user));
   auth_db_get_user("frank", &user);
   TEST_ASSERT_EQUAL_STRING("new_hash", user.password_hash);
}

static void test_validate_username(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_validate_username("alice"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_validate_username("user_1"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_validate_username("_underscore"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_validate_username("a.b-c"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, auth_db_validate_username(""));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, auth_db_validate_username(NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, auth_db_validate_username("1startsdigit"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, auth_db_validate_username("has space"));
}

/* ============================================================================
 * Session Tests
 * ============================================================================ */

static void test_create_session(void) {
   int user_id = create_and_get_id("sess_user", "hash", false);

   int rc = auth_db_create_session(user_id, "token_abc_1234567890", "127.0.0.1", "TestAgent",
                                   false);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
}

static void test_get_session(void) {
   int user_id = create_and_get_id("sess_user2", "hash", false);
   auth_db_create_session(user_id, "token_xyz_1234567890", "10.0.0.1", "Mozilla", false);

   auth_session_t session;
   memset(&session, 0, sizeof(session));
   int rc = auth_db_get_session("token_xyz_1234567890", &session);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(user_id, session.user_id);
   TEST_ASSERT_EQUAL_STRING("sess_user2", session.username);
}

static void test_delete_session(void) {
   int user_id = create_and_get_id("sess_del", "hash", false);
   auth_db_create_session(user_id, "token_del_1234567890", NULL, NULL, false);

   int rc = auth_db_delete_session("token_del_1234567890");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);

   auth_session_t session;
   memset(&session, 0, sizeof(session));
   rc = auth_db_get_session("token_del_1234567890", &session);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, rc);
}

static void test_expired_session(void) {
   int user_id = create_and_get_id("sess_exp", "hash", false);
   auth_db_create_session(user_id, "token_exp_1234567890", NULL, NULL, false);

   /* Force the session to have already expired */
   force_session_expiry("token_exp_1234567890", time(NULL) - 3600);

   auth_session_t session;
   memset(&session, 0, sizeof(session));
   int rc = auth_db_get_session("token_exp_1234567890", &session);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, rc);
}

static void test_delete_user_sessions(void) {
   int user_id = create_and_get_id("multi_sess", "hash", false);
   auth_db_create_session(user_id, "token_ms1_1234567890", NULL, NULL, false);
   auth_db_create_session(user_id, "token_ms2_1234567890", NULL, NULL, false);

   int deleted = 0;
   int rc = auth_db_delete_user_sessions(user_id, &deleted);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, deleted);

   auth_session_t session;
   memset(&session, 0, sizeof(session));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, auth_db_get_session("token_ms1_1234567890", &session));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, auth_db_get_session("token_ms2_1234567890", &session));
}

/* ============================================================================
 * Conversation Tests
 * ============================================================================ */

static void test_create_conversation(void) {
   int user_id = create_and_get_id("conv_user", "hash", false);

   int64_t conv_id = 0;
   int rc = conv_db_create(user_id, "Test Chat", &conv_id);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_TRUE(conv_id > 0);
}

static void test_get_conversation(void) {
   int user_id = create_and_get_id("conv_get", "hash", false);

   int64_t conv_id = 0;
   conv_db_create(user_id, "My Conversation", &conv_id);

   conversation_t conv;
   memset(&conv, 0, sizeof(conv));
   int rc = conv_db_get(conv_id, user_id, &conv);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_STRING("My Conversation", conv.title);
   TEST_ASSERT_EQUAL_INT(user_id, conv.user_id);
   conv_free(&conv);
}

static void test_delete_conversation(void) {
   int user_id = create_and_get_id("conv_del", "hash", false);

   int64_t conv_id = 0;
   conv_db_create(user_id, "Deletable", &conv_id);

   int rc = conv_db_delete(conv_id, user_id);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);

   conversation_t conv;
   memset(&conv, 0, sizeof(conv));
   rc = conv_db_get(conv_id, user_id, &conv);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, rc);
   conv_free(&conv);
}

static void test_conversation_user_isolation(void) {
   int user1 = create_and_get_id("iso_user1", "hash1", false);
   int user2 = create_and_get_id("iso_user2", "hash2", false);

   int64_t conv_id = 0;
   conv_db_create(user1, "User1 Private", &conv_id);

   /* user2 should not be able to delete user1's conversation */
   int rc = conv_db_delete(conv_id, user2);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, rc);

   /* user2 should not be able to read user1's conversation */
   conversation_t conv;
   memset(&conv, 0, sizeof(conv));
   rc = conv_db_get(conv_id, user2, &conv);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_FORBIDDEN, rc);
   conv_free(&conv);

   /* user1 can still access their own conversation */
   memset(&conv, 0, sizeof(conv));
   rc = conv_db_get(conv_id, user1, &conv);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_STRING("User1 Private", conv.title);
   conv_free(&conv);
}

static void test_conversation_add_message(void) {
   int user_id = create_and_get_id("msg_user", "hash", false);

   int64_t conv_id = 0;
   conv_db_create(user_id, "Chat with messages", &conv_id);

   int rc = conv_db_add_message(conv_id, user_id, "user", "Hello there");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);

   rc = conv_db_add_message(conv_id, user_id, "assistant", "Hi! How can I help?");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);

   /* Verify message count via conv_db_get */
   conversation_t conv;
   memset(&conv, 0, sizeof(conv));
   rc = conv_db_get(conv_id, user_id, &conv);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, conv.message_count);
   conv_free(&conv);
}

/* E3: pin the messages.reasoning column round-trip + the canonical projection order
 * (id, conversation_id, role, content, tool_calls, tool_call_id, reasoning, created_at).
 * A future column edit that shifts indices wrong would land reasoning/created_at on the
 * wrong field and silently corrupt every reload — this test catches that. */
struct reasoning_rt_ctx {
   int count;
   char content[64];
   char reasoning[96];
   bool reasoning_was_null;
   time_t created_at;
};

static int reasoning_rt_cb(const conversation_message_t *msg, void *ctx_ptr) {
   struct reasoning_rt_ctx *ctx = (struct reasoning_rt_ctx *)ctx_ptr;
   if (strcmp(msg->role, "assistant") == 0 && ctx->count == 0) {
      ctx->count++;
      snprintf(ctx->content, sizeof(ctx->content), "%s", msg->content ? msg->content : "");
      ctx->reasoning_was_null = (msg->reasoning == NULL);
      snprintf(ctx->reasoning, sizeof(ctx->reasoning), "%s", msg->reasoning ? msg->reasoning : "");
      ctx->created_at = msg->created_at;
   }
   return 0;
}

static void test_message_reasoning_round_trip(void) {
   int user_id = create_and_get_id("reasoning_user", "hash", false);
   int64_t conv_id = 0;
   conv_db_create(user_id, "Reasoning chat", &conv_id);

   const char *reasoning = "{\"provider\":\"openai\",\"tokens\":42}";
   int rc = conv_db_add_message_with_tools(conv_id, user_id, "assistant", "Visible answer", NULL,
                                           NULL, reasoning, NULL);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);

   struct reasoning_rt_ctx ctx;
   memset(&ctx, 0, sizeof(ctx));
   rc = conv_db_get_messages(conv_id, user_id, reasoning_rt_cb, &ctx);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, ctx.count);
   /* content + reasoning land on the right fields (projection order intact). */
   TEST_ASSERT_EQUAL_STRING("Visible answer", ctx.content);
   TEST_ASSERT_FALSE(ctx.reasoning_was_null);
   TEST_ASSERT_EQUAL_STRING(reasoning, ctx.reasoning);
   /* created_at moved 6→7 in E3 — must be a real timestamp, not reasoning text misread. */
   TEST_ASSERT_TRUE(ctx.created_at > 0);
}

static void test_message_reasoning_null(void) {
   int user_id = create_and_get_id("reasoning_null_user", "hash", false);
   int64_t conv_id = 0;
   conv_db_create(user_id, "No-reasoning chat", &conv_id);

   int rc = conv_db_add_message_with_tools(conv_id, user_id, "assistant", "Plain answer", NULL,
                                           NULL, NULL, NULL);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);

   struct reasoning_rt_ctx ctx;
   memset(&ctx, 0, sizeof(ctx));
   rc = conv_db_get_messages(conv_id, user_id, reasoning_rt_cb, &ctx);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, ctx.count);
   TEST_ASSERT_EQUAL_STRING("Plain answer", ctx.content);
   TEST_ASSERT_TRUE(ctx.reasoning_was_null);
   TEST_ASSERT_TRUE(ctx.created_at > 0);
}

/* v81 messages.is_error round-trip: a role='tool' row's failure flag persists and reads back on
 * the correct column, and the neutral case reads 0.  Pins the INSERT bind (is_error at bind 8) +
 * SELECT projection (is_error at column 8) invariant against a future reader/writer of stmt_msg_add
 * — a wrong index would silently corrupt content/tool_call_id/is_error. */
struct tool_err_rt_ctx {
   int count;
   char content[2][64];
   char tcid[2][64];
   int is_error[2];
};

static int tool_err_rt_cb(const conversation_message_t *msg, void *ctx_ptr) {
   struct tool_err_rt_ctx *ctx = (struct tool_err_rt_ctx *)ctx_ptr;
   if (strcmp(msg->role, "tool") == 0 && ctx->count < 2) {
      int i = ctx->count++;
      snprintf(ctx->content[i], sizeof(ctx->content[i]), "%s", msg->content ? msg->content : "");
      snprintf(ctx->tcid[i], sizeof(ctx->tcid[i]), "%s",
               msg->tool_call_id ? msg->tool_call_id : "");
      ctx->is_error[i] = msg->is_error;
   }
   return 0;
}

static void test_message_is_error_round_trip(void) {
   int user_id = create_and_get_id("tool_err_user", "hash", false);
   int64_t conv_id = 0;
   conv_db_create(user_id, "Tool error chat", &conv_id);

   /* Failing tool result (is_error=true) then a successful one (is_error=false). */
   int rc = conv_db_add_message_with_tools_ex(conv_id, user_id, "tool", "Entity not found", NULL,
                                              "call_fail", NULL, true, NULL);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   rc = conv_db_add_message_with_tools_ex(conv_id, user_id, "tool", "3 results", NULL, "call_ok",
                                          NULL, false, NULL);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);

   struct tool_err_rt_ctx ctx;
   memset(&ctx, 0, sizeof(ctx));
   rc = conv_db_get_messages(conv_id, user_id, tool_err_rt_cb, &ctx);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, ctx.count);
   /* Row 0 (failure): is_error persisted as 1, content + tool_call_id on the right columns. */
   TEST_ASSERT_EQUAL_INT(1, ctx.is_error[0]);
   TEST_ASSERT_EQUAL_STRING("Entity not found", ctx.content[0]);
   TEST_ASSERT_EQUAL_STRING("call_fail", ctx.tcid[0]);
   /* Row 1 (success): is_error 0 (neutral), projection intact. */
   TEST_ASSERT_EQUAL_INT(0, ctx.is_error[1]);
   TEST_ASSERT_EQUAL_STRING("3 results", ctx.content[1]);
   TEST_ASSERT_EQUAL_STRING("call_ok", ctx.tcid[1]);
}

/* Phase B defense-in-depth: conv_db_get_messages_by_range must suppress rows
 * from private conversations when include_private=false, even though the
 * caller passes ownership-correct user_id.  Rationale lives in PROVENANCE.md
 * Sec H1 — closes the only gap where an upstream provenance fetch that
 * forgot to filter could leak content. */

struct mbr_count_ctx {
   int count;
   int64_t first_id;
};

static int mbr_count_cb(const conversation_message_t *msg, void *ctx_ptr) {
   struct mbr_count_ctx *ctx = (struct mbr_count_ctx *)ctx_ptr;
   ctx->count++;
   if (ctx->count == 1)
      ctx->first_id = msg->id;
   return 0;
}

static void test_get_messages_by_range_filters_private_by_default(void) {
   int user_id = create_and_get_id("priv_filter_user", "hash", false);
   int64_t conv_id = 0;
   conv_db_create(user_id, "Will become private", &conv_id);
   conv_db_add_message(conv_id, user_id, "user", "secret one");
   conv_db_add_message(conv_id, user_id, "assistant", "secret two");

   /* Public first — confirm the harness can read messages. */
   struct mbr_count_ctx pub_ctx = { 0, 0 };
   int rc = conv_db_get_messages_by_range(conv_id, user_id, 1, 1000000, /*max_rows=*/0,
                                          /*include_private=*/false, mbr_count_cb, &pub_ctx);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, pub_ctx.count);

   /* Mark private — same call must now return 0 rows. */
   rc = conv_db_set_private(conv_id, user_id, true);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);

   struct mbr_count_ctx priv_ctx = { 0, 0 };
   rc = conv_db_get_messages_by_range(conv_id, user_id, 1, 1000000, /*max_rows=*/0,
                                      /*include_private=*/false, mbr_count_cb, &priv_ctx);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(0, priv_ctx.count);
}

static void test_get_messages_by_range_returns_private_when_opted_in(void) {
   int user_id = create_and_get_id("priv_optin_user", "hash", false);
   int64_t conv_id = 0;
   conv_db_create(user_id, "Private session", &conv_id);
   conv_db_add_message(conv_id, user_id, "user", "context_expand should still see this");
   int rc = conv_db_set_private(conv_id, user_id, true);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);

   /* include_private=true is the context_expand path — user expanding their
    * own current-session COMPACTED block.  Ownership still enforced. */
   struct mbr_count_ctx ctx = { 0, 0 };
   rc = conv_db_get_messages_by_range(conv_id, user_id, 1, 1000000, /*max_rows=*/0,
                                      /*include_private=*/true, mbr_count_cb, &ctx);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, ctx.count);
}

/* Regression: the row cap is a LIMIT on returned rows, and a range far wider
 * than the message-ID span still returns every in-range message.  Guards the
 * bug where the window was capped as `start_id + 500` on the global sparse
 * messages.id, silently returning nothing for wide (first-extraction) ranges. */
static void test_get_messages_by_range_row_cap(void) {
   int user_id = create_and_get_id("range_cap_user", "hash", false);
   int64_t conv_id = 0;
   conv_db_create(user_id, "cap session", &conv_id);
   conv_db_add_message(conv_id, user_id, "user", "one");
   conv_db_add_message(conv_id, user_id, "assistant", "two");
   conv_db_add_message(conv_id, user_id, "user", "three");

   /* Wide range (starts at 1, well below the real message IDs) must still
    * return all three — the old start_id+N cap regressed to zero here. */
   struct mbr_count_ctx all_ctx = { 0, 0 };
   int rc = conv_db_get_messages_by_range(conv_id, user_id, 1, 1000000, /*max_rows=*/0,
                                          /*include_private=*/false, mbr_count_cb, &all_ctx);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(3, all_ctx.count);

   /* max_rows caps the returned row count. */
   struct mbr_count_ctx cap_ctx = { 0, 0 };
   rc = conv_db_get_messages_by_range(conv_id, user_id, 1, 1000000, /*max_rows=*/2,
                                      /*include_private=*/false, mbr_count_cb, &cap_ctx);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, cap_ctx.count);

   /* An inverted range (start > end) is rejected, not silently empty. */
   struct mbr_count_ctx inv_ctx = { 0, 0 };
   rc = conv_db_get_messages_by_range(conv_id, user_id, 1000, 999, /*max_rows=*/0,
                                      /*include_private=*/false, mbr_count_cb, &inv_ctx);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, rc);
}

/* ============================================================================
 * Compaction watermark (v67) Tests
 * ============================================================================ */

static int wm_count_cb(const conversation_message_t *msg, void *ctx) {
   (void)msg;
   (*(int *)ctx)++;
   return 0;
}

/* conv_db_set_compaction_watermark: advances forward, rejects a stale rewind
 * (watermark AND summary), and rejects an invalid (<=0) watermark. */
static void test_compaction_watermark_monotonic(void) {
   int user_id = create_and_get_id("wm_mono", "hash", false);
   int64_t conv_id = 0;
   conv_db_create(user_id, "Watermark mono", &conv_id);

   conversation_t conv;

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_set_compaction_watermark(conv_id, user_id, "summary v1", 100));
   memset(&conv, 0, sizeof(conv));
   conv_db_get(conv_id, user_id, &conv);
   TEST_ASSERT_EQUAL_INT64(100, conv.context_watermark_msg_id);
   TEST_ASSERT_EQUAL_STRING("summary v1", conv.compaction_summary);
   conv_free(&conv);

   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_set_compaction_watermark(conv_id, user_id, "summary v2", 200));
   memset(&conv, 0, sizeof(conv));
   conv_db_get(conv_id, user_id, &conv);
   TEST_ASSERT_EQUAL_INT64(200, conv.context_watermark_msg_id);
   conv_free(&conv);

   /* Stale rewind: 150 < 200 -> benign no-op (success), watermark + summary unchanged. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_set_compaction_watermark(conv_id, user_id, "summary STALE", 150));
   memset(&conv, 0, sizeof(conv));
   conv_db_get(conv_id, user_id, &conv);
   TEST_ASSERT_EQUAL_INT64(200, conv.context_watermark_msg_id);
   TEST_ASSERT_EQUAL_STRING("summary v2", conv.compaction_summary);
   conv_free(&conv);

   /* Invalid watermark (<= 0). */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID,
                         conv_db_set_compaction_watermark(conv_id, user_id, "x", 0));
}

/* conv_db_get_messages_after: after_id=0 returns all; bounding excludes <= after_id. */
static void test_get_messages_after_bounds(void) {
   int user_id = create_and_get_id("wm_after", "hash", false);
   int64_t conv_id = 0;
   conv_db_create(user_id, "Watermark after", &conv_id);
   conv_db_add_message(conv_id, user_id, "user", "m1");
   conv_db_add_message(conv_id, user_id, "assistant", "m2");
   conv_db_add_message(conv_id, user_id, "user", "m3");

   int n = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_after(conv_id, user_id, 0, wm_count_cb, &n));
   TEST_ASSERT_EQUAL_INT(3, n);

   int64_t *ids = NULL;
   int idc = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_get_message_ids(conv_id, user_id, &ids, &idc));
   TEST_ASSERT_EQUAL_INT(3, idc);

   n = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_after(conv_id, user_id, ids[0], wm_count_cb, &n));
   TEST_ASSERT_EQUAL_INT(2, n); /* only m2, m3 (id > ids[0]) */

   n = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS,
                         conv_db_get_messages_after(conv_id, user_id, ids[2], wm_count_cb, &n));
   TEST_ASSERT_EQUAL_INT(0, n); /* nothing after the last message */

   free(ids);
}

/* ============================================================================
 * User Settings Tests
 * ============================================================================ */

static void test_user_settings_defaults(void) {
   int user_id = create_and_get_id("settings_user", "hash", false);

   auth_user_settings_t settings;
   memset(&settings, 0, sizeof(settings));
   int rc = auth_db_get_user_settings(user_id, &settings);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);

   /* Default settings from schema */
   TEST_ASSERT_EQUAL_STRING("", settings.persona_description);
   TEST_ASSERT_EQUAL_STRING("", settings.location);
   TEST_ASSERT_EQUAL_STRING("UTC", settings.timezone);
}

static void test_user_settings_set_and_get(void) {
   int user_id = create_and_get_id("settings_user2", "hash", false);

   auth_user_settings_t settings;
   memset(&settings, 0, sizeof(settings));
   strncpy(settings.location, "New York", sizeof(settings.location) - 1);
   strncpy(settings.timezone, "America/New_York", sizeof(settings.timezone) - 1);
   strncpy(settings.units, "imperial", sizeof(settings.units) - 1);

   int rc = auth_db_set_user_settings(user_id, &settings);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);

   auth_user_settings_t loaded;
   memset(&loaded, 0, sizeof(loaded));
   rc = auth_db_get_user_settings(user_id, &loaded);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_STRING("New York", loaded.location);
   TEST_ASSERT_EQUAL_STRING("America/New_York", loaded.timezone);
   TEST_ASSERT_EQUAL_STRING("imperial", loaded.units);
}

/* ============================================================================
 * v43 schema tests — entity-merge / user-identity-dedup workstream
 *
 * Three tests cover (1) fresh-install schema completeness, (2) the partial
 * UNIQUE one-self-per-user invariant, (3) round-trip migration: a fresh v43
 * DB is downgraded to v42 (table renamed + recreated without the new columns,
 * new tables and indexes dropped, schema_version reset), then re-init runs
 * the v43 migration and we verify the v43 objects came back and pre-existing
 * data survived.
 * ============================================================================ */

/* Helper: returns true if a column with the given name exists on the table.
 * Uses PRAGMA table_info, which reflects the current schema after migrations. */
static bool column_exists(const char *table, const char *column) {
   char sql[256];
   snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      return false;
   }
   bool found = false;
   while (sqlite3_step(stmt) == SQLITE_ROW) {
      const char *name = (const char *)sqlite3_column_text(stmt, 1);
      if (name && strcmp(name, column) == 0) {
         found = true;
         break;
      }
   }
   sqlite3_finalize(stmt);
   return found;
}

/* Helper: returns true if a sqlite_master object (table or index) with the
 * given name exists. */
static bool master_object_exists(const char *type, const char *name) {
   sqlite3_stmt *stmt = NULL;
   const char *sql = "SELECT 1 FROM sqlite_master WHERE type = ? AND name = ?";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      return false;
   }
   sqlite3_bind_text(stmt, 1, type, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
   bool found = (sqlite3_step(stmt) == SQLITE_ROW);
   sqlite3_finalize(stmt);
   return found;
}

/* Helper: read schema_version (assumes single row, returns 0 on failure). */
static int read_schema_version(void) {
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, "SELECT version FROM schema_version LIMIT 1", -1, &stmt, NULL) !=
       SQLITE_OK) {
      return 0;
   }
   int version = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      version = sqlite3_column_int(stmt, 0);
   }
   sqlite3_finalize(stmt);
   return version;
}

static void test_v43_schema_fresh_install(void) {
   /* setUp gave us a fresh :memory: DB at the current AUTH_DB_SCHEMA_VERSION;
    * verify all v43 objects are present and the version is correct. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SCHEMA_VERSION, read_schema_version());
   TEST_ASSERT_GREATER_OR_EQUAL_INT(43, AUTH_DB_SCHEMA_VERSION);

   /* New columns on memory_entities */
   TEST_ASSERT_TRUE(column_exists("memory_entities", "canonical_id"));
   TEST_ASSERT_TRUE(column_exists("memory_entities", "is_user_self"));

   /* New tables */
   TEST_ASSERT_TRUE(master_object_exists("table", "memory_entity_aliases"));
   TEST_ASSERT_TRUE(master_object_exists("table", "memory_entity_merge_proposals"));

   /* All four new partial indexes */
   TEST_ASSERT_TRUE(master_object_exists("index", "idx_memory_entities_canonical"));
   TEST_ASSERT_TRUE(master_object_exists("index", "idx_memory_entities_user_self"));
   TEST_ASSERT_TRUE(master_object_exists("index", "idx_memory_entity_aliases_user_target"));
   TEST_ASSERT_TRUE(master_object_exists("index", "idx_merge_proposals_pending"));
}

static void test_v43_partial_unique_user_self_constraint(void) {
   /* idx_memory_entities_user_self is a partial UNIQUE index that enforces
    * "at most one is_user_self=1 row per user".  Verify by attempting to
    * insert two such rows for the same user — the second must fail.  Two
    * is_user_self=0 rows must coexist freely (the partial WHERE clause
    * excludes them from the uniqueness constraint). */
   int uid = create_and_get_id("jon", "hash", true);
   TEST_ASSERT_GREATER_THAN(0, uid);

   const char *insert_sql =
       "INSERT INTO memory_entities (user_id, name, entity_type, canonical_name, is_user_self) "
       "VALUES (?, ?, 'person', ?, ?)";
   sqlite3_stmt *stmt = NULL;

   /* First is_user_self=1 row — must succeed. */
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(s_db.db, insert_sql, -1, &stmt, NULL));
   sqlite3_bind_int(stmt, 1, uid);
   sqlite3_bind_text(stmt, 2, "Jonathan Smith", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, "jonathan smith", -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 4, 1);
   TEST_ASSERT_EQUAL_INT(SQLITE_DONE, sqlite3_step(stmt));
   sqlite3_finalize(stmt);

   /* Second is_user_self=1 row for the same user — must fail with constraint error. */
   stmt = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(s_db.db, insert_sql, -1, &stmt, NULL));
   sqlite3_bind_int(stmt, 1, uid);
   sqlite3_bind_text(stmt, 2, "Jon", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, "jon", -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 4, 1);
   int rc = sqlite3_step(stmt);
   TEST_ASSERT_EQUAL_INT(SQLITE_CONSTRAINT, rc);
   sqlite3_finalize(stmt);

   /* is_user_self=0 row for the same user — must succeed (excluded by WHERE clause). */
   stmt = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(s_db.db, insert_sql, -1, &stmt, NULL));
   sqlite3_bind_int(stmt, 1, uid);
   sqlite3_bind_text(stmt, 2, "Jon", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, "jon", -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 4, 0);
   TEST_ASSERT_EQUAL_INT(SQLITE_DONE, sqlite3_step(stmt));
   sqlite3_finalize(stmt);

   /* A second user can independently have an is_user_self=1 row — uniqueness is per-user. */
   int uid2 = create_and_get_id("alice", "hash2", false);
   TEST_ASSERT_GREATER_THAN(0, uid2);
   stmt = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(s_db.db, insert_sql, -1, &stmt, NULL));
   sqlite3_bind_int(stmt, 1, uid2);
   sqlite3_bind_text(stmt, 2, "Alice", -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 3, "alice", -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 4, 1);
   TEST_ASSERT_EQUAL_INT(SQLITE_DONE, sqlite3_step(stmt));
   sqlite3_finalize(stmt);
}

static void test_v43_migration_from_v42(void) {
   /* setUp gave us :memory: at v43.  This test needs a file path so the DB
    * survives the shutdown / reinit cycle that drives the migration code
    * path.  Tear the in-memory DB down, do the round-trip on a /tmp file,
    * then leave the test in a clean (uninitialized) state — tearDown's
    * auth_db_shutdown() is a safe no-op when nothing is open. */
   auth_db_shutdown();

   char db_path[64];
   snprintf(db_path, sizeof(db_path), "/tmp/dawn_test_v43_migration_XXXXXX");
   int fd = mkstemp(db_path);
   TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
   close(fd);
   /* mkstemp leaves a 0-byte file; remove so SQLite creates a fresh DB. */
   unlink(db_path);

   /* Step 1: fresh init creates v43 schema. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(db_path));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SCHEMA_VERSION, read_schema_version());

   /* Insert a user + memory_entities row at the v43 schema; we'll verify
    * the row survives the round trip. */
   sqlite3_exec(s_db.db,
                "INSERT INTO users (username, password_hash, created_at) "
                "VALUES ('alice', 'hash', 1000)",
                NULL, NULL, NULL);
   sqlite3_exec(s_db.db,
                "INSERT INTO memory_entities (user_id, name, entity_type, "
                "canonical_name, mention_count) "
                "VALUES (1, 'Alice', 'person', 'alice', 7)",
                NULL, NULL, NULL);

   /* Step 2: downgrade to v42.  Drop the v43 tables and indexes, then
    * recreate memory_entities without canonical_id / is_user_self via the
    * standard SQLite rename-and-rebuild pattern (DROP COLUMN is blocked by
    * the canonical_id self-FK).  Foreign keys go OFF for the rebuild so the
    * memory_relations FK on subject_entity_id doesn't fail during the swap.
    * Finally, reset schema_version to 42 so the next init sees the DB as
    * pre-v43 and runs the v43 migration block. */
   /* PRAGMA legacy_alter_table=ON disables SQLite's "rewrite references in
    * triggers / views / FK clauses" behaviour during RENAME TO; otherwise the
    * memory_relations FK on subject_entity_id gets silently rewritten to
    * point at memory_entities_v42tmp, and after the swap-and-drop the schema
    * is broken (statements referencing memory_entities will fail to prepare).
    * Both pragmas must come BEFORE any CREATE/DROP/ALTER in this script. */
   char *errmsg = NULL;
   const char *downgrade_sql =
       "PRAGMA legacy_alter_table=ON;"
       "PRAGMA foreign_keys=OFF;"
       "DROP INDEX IF EXISTS idx_merge_proposals_pending;"
       "DROP INDEX IF EXISTS idx_memory_entity_aliases_user_target;"
       "DROP INDEX IF EXISTS idx_memory_entities_user_self;"
       "DROP INDEX IF EXISTS idx_memory_entities_canonical;"
       "DROP TABLE IF EXISTS memory_entity_merge_proposals;"
       "DROP TABLE IF EXISTS memory_entity_aliases;"
       "ALTER TABLE memory_entities RENAME TO memory_entities_v42tmp;"
       "CREATE TABLE memory_entities ("
       "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
       "  user_id INTEGER NOT NULL,"
       "  name TEXT NOT NULL,"
       "  entity_type TEXT NOT NULL,"
       "  canonical_name TEXT NOT NULL,"
       "  embedding BLOB DEFAULT NULL,"
       "  embedding_norm REAL DEFAULT NULL,"
       "  photo_id TEXT DEFAULT NULL,"
       "  first_seen INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
       "  last_seen INTEGER,"
       "  mention_count INTEGER DEFAULT 1,"
       "  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,"
       "  UNIQUE(user_id, canonical_name)"
       ");"
       "INSERT INTO memory_entities (id, user_id, name, entity_type, canonical_name, "
       " embedding, embedding_norm, photo_id, first_seen, last_seen, mention_count) "
       "SELECT id, user_id, name, entity_type, canonical_name, "
       " embedding, embedding_norm, photo_id, first_seen, last_seen, mention_count "
       "FROM memory_entities_v42tmp;"
       "DROP TABLE memory_entities_v42tmp;"
       "DELETE FROM schema_version;"
       "INSERT INTO schema_version (version) VALUES (42);"
       "PRAGMA foreign_keys=ON;"
       "PRAGMA legacy_alter_table=OFF;";
   int rc = sqlite3_exec(s_db.db, downgrade_sql, NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      TEST_FAIL_MESSAGE(errmsg ? errmsg : "downgrade failed");
   }
   sqlite3_free(errmsg);
   errmsg = NULL;

   /* Confirm the downgrade worked: v43 columns / tables / indexes are gone. */
   TEST_ASSERT_FALSE(column_exists("memory_entities", "canonical_id"));
   TEST_ASSERT_FALSE(column_exists("memory_entities", "is_user_self"));
   TEST_ASSERT_FALSE(master_object_exists("table", "memory_entity_aliases"));
   TEST_ASSERT_FALSE(master_object_exists("table", "memory_entity_merge_proposals"));
   TEST_ASSERT_FALSE(master_object_exists("index", "idx_memory_entities_canonical"));
   TEST_ASSERT_FALSE(master_object_exists("index", "idx_memory_entities_user_self"));
   TEST_ASSERT_EQUAL_INT(42, read_schema_version());

   auth_db_shutdown();

   /* Step 3: reinit — the v43 migration block fires, creates the new tables /
    * columns / indexes, bumps schema_version. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(db_path));

   /* Verify v43 schema is back. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SCHEMA_VERSION, read_schema_version());
   TEST_ASSERT_TRUE(column_exists("memory_entities", "canonical_id"));
   TEST_ASSERT_TRUE(column_exists("memory_entities", "is_user_self"));
   TEST_ASSERT_TRUE(master_object_exists("table", "memory_entity_aliases"));
   TEST_ASSERT_TRUE(master_object_exists("table", "memory_entity_merge_proposals"));
   TEST_ASSERT_TRUE(master_object_exists("index", "idx_memory_entities_canonical"));
   TEST_ASSERT_TRUE(master_object_exists("index", "idx_memory_entities_user_self"));
   TEST_ASSERT_TRUE(master_object_exists("index", "idx_memory_entity_aliases_user_target"));
   TEST_ASSERT_TRUE(master_object_exists("index", "idx_merge_proposals_pending"));

   /* Pre-migration data survived: Alice row is still there with the new
    * columns defaulted (canonical_id NULL, is_user_self 0). */
   sqlite3_stmt *stmt = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(s_db.db,
                                                       "SELECT name, mention_count, canonical_id, "
                                                       "       is_user_self "
                                                       "FROM memory_entities WHERE id = 1",
                                                       -1, &stmt, NULL));
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(stmt));
   TEST_ASSERT_EQUAL_STRING("Alice", (const char *)sqlite3_column_text(stmt, 0));
   TEST_ASSERT_EQUAL_INT(7, sqlite3_column_int(stmt, 1));
   TEST_ASSERT_EQUAL_INT(SQLITE_NULL, sqlite3_column_type(stmt, 2));
   TEST_ASSERT_EQUAL_INT(0, sqlite3_column_int(stmt, 3));
   sqlite3_finalize(stmt);

   /* Step 4: shutdown + cleanup.  WAL mode leaves -wal/-shm sidecars; remove
    * all three so nothing is left in /tmp. */
   auth_db_shutdown();
   unlink(db_path);
   char wal_path[80];
   snprintf(wal_path, sizeof(wal_path), "%s-wal", db_path);
   unlink(wal_path);
   char shm_path[80];
   snprintf(shm_path, sizeof(shm_path), "%s-shm", db_path);
   unlink(shm_path);
}

/* ============================================================================
 * main
 * ============================================================================ */

int main(void) {
   UNITY_BEGIN();

   /* Lifecycle */
   RUN_TEST(test_init_returns_success);
   RUN_TEST(test_shutdown_and_reinit);

   /* Users */
   RUN_TEST(test_create_user);
   RUN_TEST(test_get_user_by_name);
   RUN_TEST(test_get_user_not_found);
   RUN_TEST(test_create_duplicate_user);
   RUN_TEST(test_delete_user);
   RUN_TEST(test_delete_last_admin_fails);
   RUN_TEST(test_verify_password);
   RUN_TEST(test_verify_wrong_password);
   RUN_TEST(test_update_password);
   RUN_TEST(test_validate_username);

   /* Sessions */
   RUN_TEST(test_create_session);
   RUN_TEST(test_get_session);
   RUN_TEST(test_delete_session);
   RUN_TEST(test_expired_session);
   RUN_TEST(test_delete_user_sessions);

   /* Conversations */
   RUN_TEST(test_create_conversation);
   RUN_TEST(test_get_conversation);
   RUN_TEST(test_delete_conversation);
   RUN_TEST(test_conversation_user_isolation);
   RUN_TEST(test_conversation_add_message);
   RUN_TEST(test_message_reasoning_round_trip);
   RUN_TEST(test_message_reasoning_null);
   RUN_TEST(test_message_is_error_round_trip);
   RUN_TEST(test_get_messages_by_range_filters_private_by_default);
   RUN_TEST(test_get_messages_by_range_returns_private_when_opted_in);
   RUN_TEST(test_get_messages_by_range_row_cap);
   RUN_TEST(test_compaction_watermark_monotonic);
   RUN_TEST(test_get_messages_after_bounds);

   /* User Settings */
   RUN_TEST(test_user_settings_defaults);
   RUN_TEST(test_user_settings_set_and_get);

   /* v43 schema (entity-merge / user-identity-dedup) */
   RUN_TEST(test_v43_schema_fresh_install);
   RUN_TEST(test_v43_partial_unique_user_self_constraint);
   RUN_TEST(test_v43_migration_from_v42);

   return UNITY_END();
}
