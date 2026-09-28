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
 * Unit tests for the Anthropic betas on first-party Claude requests: which
 * request fields are added, the matching header, and which 400s turn a beta
 * off.  A rejection lasts for the process (per model), so the tests run in
 * order and the ones that turn a beta off for the unnamed model come last.
 */

#include <curl/curl.h>
#include <json-c/json.h>
#include <string.h>

#include "core/session_manager.h"
#include "llm/llm_cache_monitor.h"
#include "llm/llm_claude_betas.h"
#include "unity.h"

/* ---- stubs ---- */
session_t *session_get_command_context(void) {
   return NULL;
}
bool llm_cache_monitor_in_side_call(void) {
   return false;
}
bool llm_cache_monitor_previous_message_id(uint32_t session_id,
                                           int64_t conversation_id,
                                           char *out,
                                           size_t out_len) {
   (void)session_id;
   (void)conversation_id;
   (void)out;
   (void)out_len;
   return false;
}

#define FIRST_PARTY "https://api.anthropic.com"

void setUp(void) {
   claude_betas_take_retry();
}
void tearDown(void) {
}

static json_object *request_with_thinking(const char *type) {
   json_object *req = json_object_new_object();
   if (type) {
      json_object *thinking = json_object_new_object();
      json_object_object_add(thinking, "type", json_object_new_string(type));
      json_object_object_add(req, "thinking", thinking);
   }
   return req;
}

static bool has_binding(json_object *req) {
   json_object *thinking = NULL;
   json_object *binding = NULL;
   json_object *behavior = NULL;
   return json_object_object_get_ex(req, "thinking", &thinking) &&
          json_object_object_get_ex(thinking, "block_binding", &binding) &&
          json_object_object_get_ex(binding, "prefix_mismatch_behavior", &behavior) &&
          strcmp(json_object_get_string(behavior), "drop_block") == 0;
}

static char *header_of(const claude_betas_t *b) {
   static char text[256];
   text[0] = '\0';
   struct curl_slist *h = claude_betas_header(NULL, b);
   if (h) {
      snprintf(text, sizeof(text), "%s", h->data);
      curl_slist_free_all(h);
   }
   return text;
}

static const char *error_body(const char *type, const char *message) {
   static char body[512];
   snprintf(body, sizeof(body),
            "{\"type\":\"error\",\"error\":{\"type\":\"%s\",\"message\":\"%s\"}}", type, message);
   return body;
}

static void test_only_first_party_gets_betas(void) {
   const char *hosts[] = { "https://openrouter.ai/api", "https://api.anthropic.com.evil.test",
                           "http://localhost:8080", NULL };
   for (int i = 0; hosts[i]; i++) {
      json_object *req = request_with_thinking("adaptive");
      claude_betas_t b;
      claude_betas_add(req, hosts[i], &b);
      TEST_ASSERT_FALSE(b.diagnostics);
      TEST_ASSERT_FALSE(b.binding);
      TEST_ASSERT_FALSE(json_object_object_get_ex(req, "diagnostics", NULL));
      TEST_ASSERT_FALSE(has_binding(req));
      TEST_ASSERT_EQUAL_STRING("", header_of(&b));
      json_object_put(req);
   }
}

static void test_binding_field_only_on_types_that_accept_it(void) {
   const struct {
      const char *type;
      bool field;
   } cases[] = { { "adaptive", true },
                 { "enabled", true },
                 { "disabled", false },
                 { NULL, false } };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      json_object *req = request_with_thinking(cases[i].type);
      claude_betas_t b;
      claude_betas_add(req, FIRST_PARTY, &b);
      TEST_ASSERT_TRUE(b.diagnostics);
      TEST_ASSERT_TRUE(b.binding); /* the header selects drop_block even without the field */
      TEST_ASSERT_EQUAL(cases[i].field, has_binding(req));
      TEST_ASSERT_TRUE(json_object_object_get_ex(req, "diagnostics", NULL));
      json_object_put(req);
   }
}

static void test_header_names_what_the_body_carries(void) {
   claude_betas_t both = { true, true };
   claude_betas_t diag = { true, false };
   claude_betas_t bind = { false, true };
   TEST_ASSERT_EQUAL_STRING(
       "anthropic-beta: cache-diagnosis-2026-04-07,thinking-binding-controls-2026-08-01",
       header_of(&both));
   TEST_ASSERT_EQUAL_STRING("anthropic-beta: cache-diagnosis-2026-04-07", header_of(&diag));
   TEST_ASSERT_EQUAL_STRING("anthropic-beta: thinking-binding-controls-2026-08-01",
                            header_of(&bind));
}

static void test_other_errors_are_not_rejections(void) {
   claude_betas_t both = { true, true };
   /* A binding mismatch on an enforced account names block_binding and the beta:
    * it must never turn the controls off. */
   const char *mismatch = error_body(
       "invalid_request_error",
       "messages.5.content.0: Invalid `signature` in `thinking` block. The block is bound to a "
       "different conversation. Remove the block, or set "
       "`thinking.block_binding.prefix_mismatch_behavior` to \\\"drop_block\\\". That setting "
       "requires the `thinking-binding-controls-2026-08-01` value in the `anthropic-beta` "
       "header.");
   TEST_ASSERT_FALSE(claude_betas_rejected(400, mismatch, &both));
   TEST_ASSERT_FALSE(claude_betas_rejected(
       400, error_body("invalid_request_error", "max_tokens: Field required"), &both));
   TEST_ASSERT_FALSE(
       claude_betas_rejected(429, error_body("rate_limit_error", "diagnostics"), &both));
   TEST_ASSERT_FALSE(claude_betas_rejected(400, "not json", &both));
   TEST_ASSERT_FALSE(claude_betas_take_retry());

   /* A rejection of a beta the request didn't carry isn't its rejection. */
   claude_betas_t none = { false, false };
   TEST_ASSERT_FALSE(claude_betas_rejected(
       400, error_body("invalid_request_error", "diagnostics: Extra inputs are not permitted"),
       &none));

   /* Still on. */
   json_object *req = request_with_thinking("adaptive");
   claude_betas_t b;
   claude_betas_add(req, FIRST_PARTY, &b);
   TEST_ASSERT_TRUE(b.diagnostics && b.binding);
   json_object_put(req);
}

static void test_rejected_diagnostics_turn_off_alone(void) {
   claude_betas_t both = { true, true };
   TEST_ASSERT_TRUE(claude_betas_rejected(
       400,
       error_body("invalid_request_error",
                  "Unexpected value(s) `cache-diagnosis-2026-04-07` for the `anthropic-beta` "
                  "header. Please consult our documentation."),
       &both));
   TEST_ASSERT_TRUE(claude_betas_take_retry());
   TEST_ASSERT_FALSE(claude_betas_take_retry()); /* taking clears it */

   json_object *req = request_with_thinking("adaptive");
   claude_betas_t b;
   claude_betas_add(req, FIRST_PARTY, &b);
   TEST_ASSERT_FALSE(b.diagnostics);
   TEST_ASSERT_TRUE(b.binding);
   TEST_ASSERT_FALSE(json_object_object_get_ex(req, "diagnostics", NULL));
   TEST_ASSERT_TRUE(has_binding(req));
   TEST_ASSERT_EQUAL_STRING("anthropic-beta: thinking-binding-controls-2026-08-01", header_of(&b));
   json_object_put(req);
}

static void test_rejected_binding_field_turns_them_off(void) {
   claude_betas_t bind = { false, true };
   TEST_ASSERT_TRUE(claude_betas_rejected(
       400,
       error_body("invalid_request_error",
                  "thinking.adaptive.block_binding: Extra inputs are not permitted"),
       &bind));
   TEST_ASSERT_TRUE(claude_betas_take_retry());

   json_object *req = request_with_thinking("adaptive");
   claude_betas_t b;
   claude_betas_add(req, FIRST_PARTY, &b);
   TEST_ASSERT_FALSE(b.diagnostics);
   TEST_ASSERT_FALSE(b.binding);
   TEST_ASSERT_FALSE(has_binding(req));
   TEST_ASSERT_EQUAL_STRING("", header_of(&b));
   json_object_put(req);
}

static json_object *request_for(const char *model) {
   json_object *req = request_with_thinking("enabled");
   json_object_object_add(req, "model", json_object_new_string(model));
   return req;
}

static void test_a_rejection_is_per_model(void) {
   /* An old model that doesn't know block_binding must not cost an enforced
    * model its controls. */
   json_object *old_req = request_for("claude-old-model");
   claude_betas_t old_sent;
   claude_betas_add(old_req, FIRST_PARTY, &old_sent);
   TEST_ASSERT_EQUAL_STRING("claude-old-model", old_sent.model);
   TEST_ASSERT_TRUE(claude_betas_rejected(
       400,
       error_body("invalid_request_error",
                  "thinking.enabled.block_binding: Extra inputs are not permitted"),
       &old_sent));
   TEST_ASSERT_TRUE(claude_betas_take_retry());
   json_object_put(old_req);

   old_req = request_for("claude-old-model");
   claude_betas_add(old_req, FIRST_PARTY, &old_sent);
   TEST_ASSERT_FALSE(old_sent.binding);
   TEST_ASSERT_TRUE(old_sent.diagnostics);
   TEST_ASSERT_FALSE(has_binding(old_req));
   json_object_put(old_req);

   json_object *opus_req = request_for("claude-opus-5-5");
   claude_betas_t opus_sent;
   claude_betas_add(opus_req, FIRST_PARTY, &opus_sent);
   TEST_ASSERT_TRUE(opus_sent.binding);
   TEST_ASSERT_TRUE(opus_sent.diagnostics);
   TEST_ASSERT_TRUE(has_binding(opus_req));
   json_object_put(opus_req);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_only_first_party_gets_betas);
   RUN_TEST(test_binding_field_only_on_types_that_accept_it);
   RUN_TEST(test_header_names_what_the_body_carries);
   RUN_TEST(test_other_errors_are_not_rejections);
   RUN_TEST(test_a_rejection_is_per_model);
   RUN_TEST(test_rejected_diagnostics_turn_off_alone);
   RUN_TEST(test_rejected_binding_field_turns_them_off);
   return UNITY_END();
}
