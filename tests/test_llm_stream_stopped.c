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
 * What a Claude stream had produced when a stop cut it: the reply text
 * streamed so far, and only the blocks that finished (signed reasoning stays,
 * reasoning still being written and the open text block go).
 */

#include <json-c/json.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "core/session_manager.h"
#include "llm/llm_context.h"
#include "llm/llm_streaming.h"
#include "llm/llm_tools.h"
#include "llm/llm_turn_blocks.h"
#include "ui/metrics.h"
#include "unity.h"
#include "webui/webui_server.h"

/* ---- what the stream reaches outside the parser: nothing under test ---- */

void llm_context_update_usage(uint32_t session_id, const llm_usage_report_t *usage) {
   (void)session_id;
   (void)usage;
}
void llm_set_last_error(int code) {
   (void)code;
}
void llm_tools_args_append(char *buf,
                           size_t *len,
                           bool *overflow,
                           const char *text,
                           size_t n,
                           bool replace) {
   (void)buf;
   (void)len;
   (void)overflow;
   (void)text;
   (void)n;
   (void)replace;
}
void metrics_record_llm_tokens(llm_type_t type,
                               cloud_provider_t cloud_provider,
                               int input_tokens,
                               int output_tokens,
                               int cached_tokens) {
   (void)type;
   (void)cloud_provider;
   (void)input_tokens;
   (void)output_tokens;
   (void)cached_tokens;
}
void metrics_record_llm_ttft(double ttft_ms) {
   (void)ttft_ms;
}
session_t *session_get_command_context(void) {
   return NULL;
}
void webui_send_error(struct session *session, const char *code, const char *message) {
   (void)session;
   (void)code;
   (void)message;
}
void webui_send_metrics_update(struct session *session,
                               const char *state,
                               int ttft_ms,
                               float token_rate,
                               int context_percent) {
   (void)session;
   (void)state;
   (void)ttft_ms;
   (void)token_rate;
   (void)context_percent;
}
void webui_send_session_json(struct session *session, const char *json_str) {
   (void)session;
   (void)json_str;
}
void webui_send_thinking_start(struct session *session, const char *provider) {
   (void)session;
   (void)provider;
}
void webui_send_thinking_delta(struct session *session, const char *text) {
   (void)session;
   (void)text;
}
void webui_send_thinking_end(struct session *session, bool has_content) {
   (void)session;
   (void)has_content;
}

void setUp(void) {
}
void tearDown(void) {
}

static void on_text(const char *chunk, void *userdata) {
   (void)chunk;
   (void)userdata;
}

static void feed(llm_stream_context_t *ctx, const char *const *events) {
   for (; *events; events++) {
      llm_stream_handle_event(ctx, *events);
   }
}

#define START_THINKING                                                          \
   "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":" \
   "\"thinking\",\"thinking\":\"\"}}"
#define THINK                                                                              \
   "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\"," \
   "\"thinking\":\"Mulling.\"}}"

/* A stop in the answer: its text so far, with the signed reasoning before it
 * as the only block. */
static void test_a_stop_in_the_text_keeps_the_signed_reasoning(void) {
   static const char *const events[] = {
      "{\"type\":\"message_start\",\"message\":{\"id\":\"m\",\"usage\":{\"input_tokens\":5}}}",
      START_THINKING,
      THINK,
      "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\","
      "\"signature\":\"SIG\"}}",
      "{\"type\":\"content_block_stop\",\"index\":0}",
      "{\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\","
      "\"text\":\"\"}}",
      "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\","
      "\"text\":\"Half an \"}}",
      "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\","
      "\"text\":\"answer\"}}",
      NULL,
   };
   llm_stream_context_t *ctx = llm_stream_create_messages(CLOUD_PROVIDER_CLAUDE, on_text, NULL);
   TEST_ASSERT_NOT_NULL(ctx);
   feed(ctx, events);
   llm_tool_response_t result;
   memset(&result, 0, sizeof(result));
   llm_stream_fill_stopped(ctx, &result);
   struct json_object *native = llm_stream_take_claude_content(ctx);
   llm_stream_free(ctx);

   TEST_ASSERT_TRUE(result.stopped);
   TEST_ASSERT_FALSE(result.has_tool_calls);
   TEST_ASSERT_EQUAL_STRING("Half an answer", result.text);
   TEST_ASSERT_EQUAL_STRING("Mulling.", result.thinking_content);
   TEST_ASSERT_EQUAL_INT(1, (int)json_object_array_length(native));
   struct json_object *block = json_object_array_get_idx(native, 0);
   TEST_ASSERT_EQUAL_STRING("thinking",
                            json_object_get_string(json_object_object_get(block, "type")));
   TEST_ASSERT_EQUAL_STRING("SIG",
                            json_object_get_string(json_object_object_get(block, "signature")));
   json_object_put(native);
   free(result.text);
   free(result.thinking_content);
}

/* A stop while it still reasons: nothing to reply with, and no block (the
 * reasoning was never signed). */
static void test_a_stop_in_the_reasoning_keeps_nothing(void) {
   static const char *const events[] = { START_THINKING, THINK, NULL };
   llm_stream_context_t *ctx = llm_stream_create_messages(CLOUD_PROVIDER_CLAUDE, on_text, NULL);
   feed(ctx, events);
   llm_tool_response_t result;
   memset(&result, 0, sizeof(result));
   llm_stream_fill_stopped(ctx, &result);
   struct json_object *native = llm_stream_take_claude_content(ctx);
   llm_stream_free(ctx);

   TEST_ASSERT_TRUE(result.stopped);
   TEST_ASSERT_NULL(result.text); /* never the thinking in the reply's place */
   TEST_ASSERT_EQUAL_INT(0, native ? (int)json_object_array_length(native) : 0);
   json_object_put(native);
   free(result.text);
   free(result.thinking_content);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_a_stop_in_the_text_keeps_the_signed_reasoning);
   RUN_TEST(test_a_stop_in_the_reasoning_keeps_nothing);
   return UNITY_END();
}
