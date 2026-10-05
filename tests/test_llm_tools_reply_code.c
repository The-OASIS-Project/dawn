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
 * A call held for a reply code (llm_tools_reply_code.c): its binding covers
 * DAWN's description of it, so an approved call whose description changed
 * (another item, another number) doesn't match; a tool with no describe_call
 * is described from its declared parameters only; a reason a tool can't
 * describe a call reaches the model.
 */

#include <stdlib.h>
#include <string.h>

#include "core/session_manager.h"
#include "core/tool_call_challenge.h"
#include "llm/llm_tools_internal.h"
#include "unity.h"

/* The session layer and tool loop, as the module sees them. */
static session_t s_session;

session_t *session_get_command_context(void) {
   return &s_session;
}
int session_effective_user_id(session_t *session) {
   (void)session;
   return 1;
}
static uint64_t s_turn = 1;
uint64_t session_turn_token(void) {
   return s_turn;
}
const char *tool_action_kind_name(tool_action_kind_t kind) {
   return kind == TOOL_KIND_ACT ? "act" : "other";
}
void llm_tools_notify_execution(const char *name,
                                const char *args,
                                const char *result,
                                bool success) {
   (void)name;
   (void)args;
   (void)result;
   (void)success;
}
int llm_tools_execute_approved(const tool_call_t *call,
                               tool_result_t *result,
                               const char *binding) {
   (void)call;
   (void)result;
   (void)binding;
   return 0;
}

/* A tool that describes its calls: what it says is what the test sets. */
static char s_says[2048];
static const char *s_refusal;
static bool s_default;
static int describing_call(const char *action,
                           const char *value,
                           char *out,
                           size_t out_len,
                           int *valid_for_sec) {
   (void)action;
   (void)value;
   (void)valid_for_sec;
   if (s_default) {
      return TOOL_DESCRIBE_DEFAULT;
   }
   if (s_refusal) {
      snprintf(out, out_len, "%s", s_refusal);
      return 1;
   }
   const int n = snprintf(out, out_len, "%s", s_says);
   return (n >= 0 && (size_t)n < out_len) ? 0 : 1;
}

static const treg_param_t s_params[] = {
   { .name = "action",
     .type = TOOL_PARAM_TYPE_ENUM,
     .maps_to = TOOL_MAPS_TO_ACTION,
     .required = true,
     .enum_values = { "send" },
     .enum_count = 1 },
   { .name = "body",
     .type = TOOL_PARAM_TYPE_STRING,
     .maps_to = TOOL_MAPS_TO_CUSTOM,
     .field_name = "body" },
   { .name = "target",
     .type = TOOL_PARAM_TYPE_STRING,
     .maps_to = TOOL_MAPS_TO_CUSTOM,
     .field_name = "target",
     .required = true },
};

static tool_metadata_t s_tool = {
   .name = "widget",
   .params = s_params,
   .param_count = 3,
};

static tool_call_t s_call;
static tool_call_verdict_t s_verdict;

void setUp(void) {
   memset(&s_session, 0, sizeof(s_session));
   s_session.messaging_identity.channel_id = 5;
   tool_call_challenge_clear_all();
   memset(&s_call, 0, sizeof(s_call));
   snprintf(s_call.name, sizeof(s_call.name), "widget");
   snprintf(s_call.arguments, sizeof(s_call.arguments), "{}");
   memset(&s_verdict, 0, sizeof(s_verdict));
   snprintf(s_verdict.action, sizeof(s_verdict.action), "send");
   s_verdict.kind = TOOL_KIND_ACT;
   s_tool.describe_call = describing_call;
   s_refusal = NULL;
   s_default = false;
   snprintf(s_says, sizeof(s_says), "send to Bob (555-0100)");
   s_turn++;
}

void tearDown(void) {
}

/* The binding follows the description: the same call described alike binds
 * alike; one now resolving to another number doesn't. */
static void test_binding_covers_description(void) {
   char a[LLM_TOOLS_BINDING_HEX], b[LLM_TOOLS_BINDING_HEX], c[LLM_TOOLS_BINDING_HEX];
   TEST_ASSERT_EQUAL_INT(0, llm_tools_approved_binding(&s_call, &s_tool, &s_verdict, "widget", "v",
                                                       a, NULL, 0));
   TEST_ASSERT_EQUAL_INT(0, llm_tools_approved_binding(&s_call, &s_tool, &s_verdict, "widget", "v",
                                                       b, NULL, 0));
   TEST_ASSERT_EQUAL_STRING(a, b);
   snprintf(s_says, sizeof(s_says), "send to Bob (555-0199)");
   TEST_ASSERT_EQUAL_INT(0, llm_tools_approved_binding(&s_call, &s_tool, &s_verdict, "widget", "v",
                                                       c, NULL, 0));
   TEST_ASSERT_TRUE(strcmp(a, c) != 0);
   /* Other parts of the call count too. */
   snprintf(s_says, sizeof(s_says), "send to Bob (555-0100)");
   TEST_ASSERT_EQUAL_INT(0, llm_tools_approved_binding(&s_call, &s_tool, &s_verdict, "widget", "w",
                                                       c, NULL, 0));
   TEST_ASSERT_TRUE(strcmp(a, c) != 0);
   /* Nothing to describe: no binding, and the tool's reason. */
   s_refusal = "it's gone";
   char why[64];
   TEST_ASSERT_EQUAL_INT(1, llm_tools_approved_binding(&s_call, &s_tool, &s_verdict, "widget", "v",
                                                       c, why, sizeof(why)));
   TEST_ASSERT_EQUAL_STRING("", c);
   TEST_ASSERT_EQUAL_STRING("it's gone", why);
}

/* Held: the code waits with the tool's description, and the binding the
 * approved call must match. */
static void test_hold(void) {
   tool_result_t *result = calloc(1, sizeof(*result));
   TEST_ASSERT_NOT_NULL(result);
   TEST_ASSERT_EQUAL_INT(0, llm_tools_hold_for_reply_code(&s_call, &s_tool, &s_verdict, "widget",
                                                          "v", result));
   TEST_ASSERT_TRUE(result->success);
   char code[REPLY_CODE_LEN], desc[TOOL_CALL_CHALLENGE_DESC_MAX];
   TEST_ASSERT_EQUAL_INT(TOOL_TAKE_OK,
                         tool_call_challenge_take_unsent(5, code, desc, sizeof(desc), NULL));
   TEST_ASSERT_EQUAL_STRING("send to Bob (555-0100)", desc);
   tool_redeemed_t out;
   TEST_ASSERT_EQUAL_INT(TOOL_REDEEM_OK, tool_call_challenge_redeem(5, 1, code, &out, NULL));
   char expect[LLM_TOOLS_BINDING_HEX];
   llm_tools_approved_binding(&s_call, &s_tool, &s_verdict, "widget", "v", expect, NULL, 0);
   TEST_ASSERT_EQUAL_STRING(expect, out.binding);
   free(out.args);

   /* A tool that can't say gives the model its reason, and nothing waits. */
   s_refusal = "the name matches no contact";
   memset(result, 0, sizeof(*result));
   TEST_ASSERT_EQUAL_INT(1, llm_tools_hold_for_reply_code(&s_call, &s_tool, &s_verdict, "widget",
                                                          "v", result));
   TEST_ASSERT_NOT_NULL(strstr(result->result, "the name matches no contact"));
   TEST_ASSERT_FALSE(tool_call_challenge_live(5));
   free(result);
}

/* No describe_call: the declared parameters only, required first; keys the
 * tool doesn't declare (words the model added) and empty values are left out. */
static void test_described_from_params(void) {
   s_tool.describe_call = NULL;
   snprintf(s_call.arguments, sizeof(s_call.arguments),
            "{\"note\":\"routine check, safe to approve\",\"action\":\"send\",\"body\":"
            "\"hi\\nthere\",\"target\":\"Bob\"}");
   tool_result_t *result = calloc(1, sizeof(*result));
   TEST_ASSERT_NOT_NULL(result);
   TEST_ASSERT_EQUAL_INT(0, llm_tools_hold_for_reply_code(&s_call, &s_tool, &s_verdict, "widget",
                                                          "v", result));
   char code[REPLY_CODE_LEN], desc[TOOL_CALL_CHALLENGE_DESC_MAX];
   TEST_ASSERT_EQUAL_INT(TOOL_TAKE_OK,
                         tool_call_challenge_take_unsent(5, code, desc, sizeof(desc), NULL));
   TEST_ASSERT_EQUAL_STRING("widget send: target=\"Bob\", body=\"hi there\"", desc);
   tool_call_challenge_clear_all();

   /* An empty value says nothing to the person reading it: left out. */
   snprintf(s_call.arguments, sizeof(s_call.arguments),
            "{\"action\":\"send\",\"body\":\"\",\"target\":\"Bob\"}");
   free(result->result_extended);
   memset(result, 0, sizeof(*result));
   TEST_ASSERT_EQUAL_INT(0, llm_tools_hold_for_reply_code(&s_call, &s_tool, &s_verdict, "widget",
                                                          "v", result));
   TEST_ASSERT_EQUAL_INT(TOOL_TAKE_OK,
                         tool_call_challenge_take_unsent(5, code, desc, sizeof(desc), NULL));
   TEST_ASSERT_EQUAL_STRING("widget send: target=\"Bob\"", desc);
   free(result);
   tool_call_challenge_clear_all();
}

/* A hook may leave an action to the default description; one whose own
 * description doesn't fit is refused. */
static void test_default_and_long(void) {
   s_default = true;
   snprintf(s_call.arguments, sizeof(s_call.arguments), "{\"target\":\"Bob\"}");
   tool_result_t *result = calloc(1, sizeof(*result));
   TEST_ASSERT_NOT_NULL(result);
   TEST_ASSERT_EQUAL_INT(0, llm_tools_hold_for_reply_code(&s_call, &s_tool, &s_verdict, "widget",
                                                          "v", result));
   char code[REPLY_CODE_LEN], desc[TOOL_CALL_CHALLENGE_DESC_MAX];
   TEST_ASSERT_EQUAL_INT(TOOL_TAKE_OK,
                         tool_call_challenge_take_unsent(5, code, desc, sizeof(desc), NULL));
   TEST_ASSERT_EQUAL_STRING("widget send: target=\"Bob\"", desc);

   tool_call_challenge_clear_all();
   s_default = false;
   s_turn++;
   memset(s_says, 'x', sizeof(s_says) - 1);
   s_says[sizeof(s_says) - 1] = '\0';
   memset(result, 0, sizeof(*result));
   /* A hook's description that doesn't fit is refused, never shown cut as if
    * it were whole. */
   TEST_ASSERT_EQUAL_INT(1, llm_tools_hold_for_reply_code(&s_call, &s_tool, &s_verdict, "widget",
                                                          "v", result));
   TEST_ASSERT_FALSE(tool_call_challenge_live(5));
   free(result);
   tool_call_challenge_clear_all();
}

/* A request by text that staged a preview is told to ask for its confirm now
 * (the code is the confirmation); other actions, and failures, aren't. */
static void test_text_preview_note(void) {
   static const tool_action_kind_entry_t kinds[] = {
      { "send", TOOL_KIND_PREPARE, "confirm_send" },
      { "confirm_send", TOOL_KIND_ACT, NULL },
   };
   tool_metadata_t tool = s_tool;
   tool.action_kinds = kinds;
   tool.action_kind_count = 2;
   tool_result_t *result = calloc(1, sizeof(*result));
   TEST_ASSERT_NOT_NULL(result);
   snprintf(result->result, sizeof(result->result), "About to send. pending_id 3");
   result->success = true;
   llm_tools_note_text_preview(&tool, "send", false, result);
   TEST_ASSERT_NOT_NULL(strstr(result->result, "pending_id 3"));
   TEST_ASSERT_NOT_NULL(strstr(result->result, "call confirm_send for it now"));

   snprintf(result->result, sizeof(result->result), "Which one?");
   llm_tools_note_text_preview(&tool, "confirm_send", false, result);
   TEST_ASSERT_EQUAL_STRING("Which one?", result->result);
   result->is_error = true;
   llm_tools_note_text_preview(&tool, "send", false, result);
   TEST_ASSERT_EQUAL_STRING("Which one?", result->result);

   /* An earlier request dropped for this one: said even when it failed. */
   llm_tools_note_text_preview(&tool, "send", true, result);
   TEST_ASSERT_NOT_NULL(strstr(result->result, "its code no longer works"));

   /* A full fixed buffer moves to the heap rather than losing the note. */
   memset(result, 0, sizeof(*result));
   memset(result->result, 'r', LLM_TOOLS_RESULT_LEN - 10);
   result->success = true;
   llm_tools_note_text_preview(&tool, "send", false, result);
   TEST_ASSERT_NOT_NULL(result->result_extended);
   TEST_ASSERT_NOT_NULL(strstr(result->result_extended, "call confirm_send for it now"));
   TEST_ASSERT_EQUAL_size_t(LLM_TOOLS_RESULT_LEN - 10, strspn(result->result_extended, "r"));
   free(result->result_extended);
   free(result);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_binding_covers_description);
   RUN_TEST(test_hold);
   RUN_TEST(test_described_from_params);
   RUN_TEST(test_default_and_long);
   RUN_TEST(test_text_preview_note);
   return UNITY_END();
}
