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
 * Who may make a tool call: the caller's kind of turn against the call's
 * kind of action.
 */

#include <string.h>

#include "core/session_manager.h"
#include "core/tool_call_challenge.h"
#include "core/tool_call_policy.h"
#include "unity.h"

/* The session layer, as the policy sees it. */
static session_t *s_ctx;
static bool s_user_turn;
static bool s_background_turn;
static bool s_code_redeemed;

session_t *session_get_command_context(void) {
   return s_ctx;
}

bool session_turn_user_originated(session_t *session) {
   return session == s_ctx && s_user_turn && !s_background_turn;
}

bool session_turn_is_background(session_t *session) {
   return session == s_ctx && s_background_turn;
}

static uint64_t s_turn_token = 7;
uint64_t session_turn_token(void) {
   return s_turn_token;
}

bool session_call_code_redeemed(void) {
   return s_code_redeemed;
}

void session_set_call_code_redeemed(bool redeemed) {
   s_code_redeemed = redeemed;
}

static session_t s_session;

void setUp(void) {
   memset(&s_session, 0, sizeof(s_session));
   s_session.type = SESSION_TYPE_WEBUI;
   s_ctx = &s_session;
   s_user_turn = true;
   s_background_turn = false;
   s_code_redeemed = false;
   s_turn_token = 7;
}

void tearDown(void) {
}

/* Which caller each kind of turn is. */
static void test_callers(void) {
   TEST_ASSERT_EQUAL_INT(TOOL_CALLER_USER, tool_call_policy_caller());

   s_session.type = SESSION_TYPE_MESSAGING;
   TEST_ASSERT_EQUAL_INT(TOOL_CALLER_USER, tool_call_policy_caller());
   s_session.messaging_identity.sender_unverified = true;
   TEST_ASSERT_EQUAL_INT(TOOL_CALLER_UNVERIFIED, tool_call_policy_caller());

   /* A background turn on that channel is unattended, not the sender. */
   s_background_turn = true;
   TEST_ASSERT_EQUAL_INT(TOOL_CALLER_UNATTENDED, tool_call_policy_caller());
   s_background_turn = false;
   s_user_turn = false; /* the session's turn isn't this thread's */
   TEST_ASSERT_EQUAL_INT(TOOL_CALLER_UNATTENDED, tool_call_policy_caller());

   /* A job's own turn is the job; a job's follow-up on a job's session (no
    * one viewing its conversation) is unattended, as on a viewer's. */
   s_session.type = SESSION_TYPE_JOB;
   TEST_ASSERT_EQUAL_INT(TOOL_CALLER_JOB, tool_call_policy_caller());
   s_background_turn = true;
   TEST_ASSERT_EQUAL_INT(TOOL_CALLER_UNATTENDED, tool_call_policy_caller());

   s_ctx = NULL;
   TEST_ASSERT_EQUAL_INT(TOOL_CALLER_UNATTENDED, tool_call_policy_caller());
}

/* The policy table, row by row. */
static void test_allows(void) {
   static const struct {
      tool_caller_t caller;
      bool read, fetch, state, device, prepare, act;
   } rows[] = {
      { TOOL_CALLER_USER, true, true, true, true, true, true },
      { TOOL_CALLER_UNVERIFIED, true, false, true, false, true, false }, /* the rest wait */
      { TOOL_CALLER_JOB, true, true, true, false, false, false },
      { TOOL_CALLER_UNATTENDED, true, false, true, false, false, false },
   };
   for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
      const tool_caller_t c = rows[i].caller;
      TEST_ASSERT_EQUAL(rows[i].read,
                        tool_call_policy_decide(c, TOOL_KIND_READ) == TOOL_CALL_ALLOW);
      TEST_ASSERT_EQUAL(rows[i].fetch,
                        tool_call_policy_decide(c, TOOL_KIND_FETCH) == TOOL_CALL_ALLOW);
      TEST_ASSERT_EQUAL(rows[i].state,
                        tool_call_policy_decide(c, TOOL_KIND_STATE) == TOOL_CALL_ALLOW);
      TEST_ASSERT_EQUAL(rows[i].device,
                        tool_call_policy_decide(c, TOOL_KIND_DEVICE) == TOOL_CALL_ALLOW);
      TEST_ASSERT_EQUAL(rows[i].prepare,
                        tool_call_policy_decide(c, TOOL_KIND_PREPARE) == TOOL_CALL_ALLOW);
      TEST_ASSERT_EQUAL(rows[i].act, tool_call_policy_decide(c, TOOL_KIND_ACT) == TOOL_CALL_ALLOW);
      TEST_ASSERT_TRUE(strlen(tool_call_policy_caller_name(c)) > 0);
   }
}

/* A call's scope: its kind, read back by the tool; a call inside it gets its
 * own and the outer one's comes back; a code approves the one call it was
 * entered for, never one inside it; inside a call, the outer caller limits
 * what an inner call may do. */
static void test_scope(void) {
   tool_action_kind_t kind = TOOL_KIND_READ;
   TEST_ASSERT_FALSE(tool_call_policy_decided(&kind));
   TEST_ASSERT_EQUAL_INT(TOOL_KIND_ACT, kind);

   const tool_call_scope_t top = tool_call_policy_enter(TOOL_KIND_PREPARE, TOOL_CALLER_USER, true);
   TEST_ASSERT_TRUE(s_code_redeemed);
   TEST_ASSERT_TRUE(tool_call_policy_decided(&kind));
   TEST_ASSERT_EQUAL_INT(TOOL_KIND_PREPARE, kind);

   const tool_call_scope_t inner = tool_call_policy_enter(TOOL_KIND_READ, TOOL_CALLER_USER, false);
   TEST_ASSERT_FALSE(s_code_redeemed);
   TEST_ASSERT_TRUE(tool_call_policy_decided(&kind));
   TEST_ASSERT_EQUAL_INT(TOOL_KIND_READ, kind);
   tool_call_policy_leave(inner);

   TEST_ASSERT_TRUE(s_code_redeemed);
   TEST_ASSERT_TRUE(tool_call_policy_decided(&kind));
   TEST_ASSERT_EQUAL_INT(TOOL_KIND_PREPARE, kind);
   tool_call_policy_leave(top);
   TEST_ASSERT_FALSE(s_code_redeemed);
   TEST_ASSERT_FALSE(tool_call_policy_decided(&kind));
}

static char *noop_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   (void)value;
   *should_respond = 0;
   return NULL;
}

static const treg_param_t s_params[] = {
   {
       .name = "action",
       .description = "action",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "list", "send" },
       .enum_count = 2,
   },
};

static const tool_action_kind_entry_t s_kinds[] = {
   { "list", TOOL_KIND_READ, NULL },
};

static const tool_metadata_t s_tool = {
   .name = "widget",
   .device_string = "widget",
   .description = "a test tool",
   .params = s_params,
   .param_count = 1,
   .action_kinds = s_kinds,
   .action_kind_count = TOOL_KIND_COUNT(s_kinds),
   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .callback = noop_callback,
};

/* The check: the named action in the tool's own spelling (case ignored), an
 * unknown one refused with the list, an omitted one taking the default, and
 * the kind against the caller. */
static void test_check(void) {
   tool_call_verdict_t v;
   TEST_ASSERT_EQUAL_INT(TOOL_CALL_ALLOW, tool_call_policy_check(&s_tool, NULL, "LIST", NULL,
                                                                 TOOL_CALLER_JOB, false, &v));
   TEST_ASSERT_EQUAL_STRING("list", v.action);
   TEST_ASSERT_EQUAL_INT(TOOL_KIND_READ, v.kind);

   TEST_ASSERT_EQUAL_INT(TOOL_CALL_REFUSE, tool_call_policy_check(&s_tool, NULL, "send", NULL,
                                                                  TOOL_CALLER_JOB, false, &v));
   TEST_ASSERT_EQUAL_INT(TOOL_KIND_ACT, v.kind);
   TEST_ASSERT_TRUE(strlen(v.message) > 0);
   TEST_ASSERT_EQUAL_INT(TOOL_CALL_ALLOW, tool_call_policy_check(&s_tool, NULL, "send", NULL,
                                                                 TOOL_CALLER_USER, false, &v));

   TEST_ASSERT_EQUAL_INT(TOOL_CALL_REFUSE, tool_call_policy_check(&s_tool, NULL, "rewind", NULL,
                                                                  TOOL_CALLER_USER, false, &v));
   TEST_ASSERT_NOT_NULL(strstr(v.message, "list, send"));

   TEST_ASSERT_EQUAL_INT(TOOL_CALL_REFUSE,
                         tool_call_policy_check(&s_tool, NULL, "", NULL, TOOL_CALLER_UNATTENDED,
                                                false, &v));
   TEST_ASSERT_EQUAL_STRING("get", v.action); /* the callback default; not listed, so it acts */

   TEST_ASSERT_EQUAL_INT(TOOL_CALL_REFUSE, tool_call_policy_check(NULL, NULL, "list", NULL,
                                                                  TOOL_CALLER_USER, false, &v));

   /* Inside an unattended call (an MQTT message's plan), a step its own
    * session would let a job fetch is refused. */
   static const tool_action_kind_entry_t fetch_kinds[] = { { "list", TOOL_KIND_FETCH, NULL } };
   tool_metadata_t fetcher = s_tool;
   fetcher.action_kinds = fetch_kinds;
   TEST_ASSERT_EQUAL_INT(TOOL_CALL_ALLOW, tool_call_policy_check(&fetcher, NULL, "list", NULL,
                                                                 TOOL_CALLER_JOB, false, &v));
   const tool_call_scope_t outer = tool_call_policy_enter(TOOL_KIND_READ, TOOL_CALLER_UNATTENDED,
                                                          false);
   TEST_ASSERT_EQUAL_INT(TOOL_CALL_REFUSE, tool_call_policy_check(&fetcher, NULL, "list", NULL,
                                                                  TOOL_CALLER_JOB, false, &v));
   TEST_ASSERT_EQUAL_INT(TOOL_CALLER_UNATTENDED, v.caller);
   tool_call_policy_leave(outer);
}

/* A text's action waits for its code; approved by code it runs; a tool that
 * can't take one, and a call inside another, are refused instead. */
static void test_challenge(void) {
   TEST_ASSERT_EQUAL_INT(TOOL_CALL_CHALLENGE,
                         tool_call_policy_decide(TOOL_CALLER_UNVERIFIED, TOOL_KIND_ACT));
   TEST_ASSERT_EQUAL_INT(TOOL_CALL_CHALLENGE,
                         tool_call_policy_decide(TOOL_CALLER_UNVERIFIED, TOOL_KIND_FETCH));
   TEST_ASSERT_EQUAL_INT(TOOL_CALL_CHALLENGE,
                         tool_call_policy_decide(TOOL_CALLER_UNVERIFIED, TOOL_KIND_DEVICE));

   tool_call_verdict_t v;
   TEST_ASSERT_EQUAL_INT(TOOL_CALL_CHALLENGE,
                         tool_call_policy_check(&s_tool, NULL, "send", NULL, TOOL_CALLER_UNVERIFIED,
                                                false, &v));
   TEST_ASSERT_EQUAL_INT(TOOL_CALL_ALLOW, tool_call_policy_check(&s_tool, NULL, "send", NULL,
                                                                 TOOL_CALLER_UNVERIFIED, true, &v));
   /* Approval by code never reaches a job's or an unattended call. */
   TEST_ASSERT_EQUAL_INT(TOOL_CALL_REFUSE, tool_call_policy_check(&s_tool, NULL, "send", NULL,
                                                                  TOOL_CALLER_JOB, true, &v));

   tool_metadata_t plan = s_tool;
   plan.no_reply_code = true;
   TEST_ASSERT_EQUAL_INT(TOOL_CALL_REFUSE,
                         tool_call_policy_check(&plan, NULL, "send", NULL, TOOL_CALLER_UNVERIFIED,
                                                false, &v));
   TEST_ASSERT_TRUE(strlen(v.message) > 0);

   const tool_call_scope_t outer = tool_call_policy_enter(TOOL_KIND_READ, TOOL_CALLER_UNVERIFIED,
                                                          false);
   TEST_ASSERT_EQUAL_INT(TOOL_CALL_REFUSE,
                         tool_call_policy_check(&s_tool, NULL, "send", NULL, TOOL_CALLER_UNVERIFIED,
                                                true, &v));
   tool_call_policy_leave(outer);
}

/* Once this turn holds an action for its code, the turn can't prepare more
 * (the code's text describes things as they were); a later turn, other
 * channels, and the approved call itself are unaffected. */
static void test_prepare_while_code_waits(void) {
   static const tool_action_kind_entry_t prep_kinds[] = { { "list", TOOL_KIND_PREPARE, NULL } };
   tool_metadata_t preparer = s_tool;
   preparer.action_kinds = prep_kinds;
   s_session.messaging_identity.channel_id = 42;
   tool_call_challenge_clear_all();

   tool_call_verdict_t v;
   TEST_ASSERT_EQUAL_INT(TOOL_CALL_ALLOW,
                         tool_call_policy_check(&preparer, NULL, "list", NULL,
                                                TOOL_CALLER_UNVERIFIED, false, &v));
   const tool_challenge_t c = {
      .channel_id = 42,
      .user_id = 1,
      .turn_token = 7,
      .tool = "widget",
      .args = "{}",
      .binding = "b",
      .description = "widget send",
   };
   TEST_ASSERT_EQUAL_INT(TOOL_CHALLENGE_OK, tool_call_challenge_create(&c));
   TEST_ASSERT_EQUAL_INT(TOOL_CALL_REFUSE,
                         tool_call_policy_check(&preparer, NULL, "list", NULL,
                                                TOOL_CALLER_UNVERIFIED, false, &v));
   TEST_ASSERT_NOT_NULL(strstr(v.message, "reply code"));
   s_turn_token = 8; /* the user's next message */
   TEST_ASSERT_EQUAL_INT(TOOL_CALL_ALLOW,
                         tool_call_policy_check(&preparer, NULL, "list", NULL,
                                                TOOL_CALLER_UNVERIFIED, false, &v));
   s_turn_token = 7;
   TEST_ASSERT_EQUAL_INT(TOOL_CALL_ALLOW, tool_call_policy_check(&preparer, NULL, "list", NULL,
                                                                 TOOL_CALLER_USER, false, &v));
   TEST_ASSERT_EQUAL_INT(TOOL_CALL_ALLOW, tool_call_policy_check(&preparer, NULL, "list", NULL,
                                                                 TOOL_CALLER_UNVERIFIED, true, &v));
   s_session.messaging_identity.channel_id = 43;
   TEST_ASSERT_EQUAL_INT(TOOL_CALL_ALLOW,
                         tool_call_policy_check(&preparer, NULL, "list", NULL,
                                                TOOL_CALLER_UNVERIFIED, false, &v));
   tool_call_challenge_clear_all();
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_callers);
   RUN_TEST(test_allows);
   RUN_TEST(test_scope);
   RUN_TEST(test_check);
   RUN_TEST(test_challenge);
   RUN_TEST(test_prepare_while_code_waits);
   return UNITY_END();
}
