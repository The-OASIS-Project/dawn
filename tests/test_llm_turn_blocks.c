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
 * Unit tests for capturing a streamed Claude response's content blocks in order
 * and replaying them exactly: provider-neutral turn blocks and their Claude
 * rendering.
 */

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/llm_claude_blocks.h"
#include "llm/llm_reasoning_details.h"
#include "llm/llm_turn_blocks.h"
#include "unity.h"

/* A Claude request's carrier: its endpoint and key tag (llm_request_carrier). */
#define TEST_CLAUDE_CARRIER "api.anthropic.com#0123456789abcdef"

void setUp(void) {
}
void tearDown(void) {
}

/* Feed one SSE event's data (content_block_start / _delta / _stop). */
static void feed(llm_claude_capture_t *c, const char *event_json) {
   json_object *ev = json_tokener_parse(event_json);
   TEST_ASSERT_NOT_NULL_MESSAGE(ev, event_json);
   json_object *type = NULL;
   json_object_object_get_ex(ev, "type", &type);
   const char *t = json_object_get_string(type);
   json_object *part = NULL;
   if (strcmp(t, "content_block_start") == 0) {
      json_object_object_get_ex(ev, "content_block", &part);
      llm_claude_capture_start(c, part);
   } else if (strcmp(t, "content_block_delta") == 0) {
      json_object_object_get_ex(ev, "delta", &part);
      llm_claude_capture_delta(c, part);
   } else if (strcmp(t, "content_block_stop") == 0) {
      llm_claude_capture_stop(c);
   }
   json_object_put(ev);
}

static const char *str_at(json_object *arr, size_t i, const char *key) {
   json_object *v = NULL;
   json_object *o = json_object_array_get_idx(arr, i);
   return json_object_object_get_ex(o, key, &v) ? json_object_get_string(v) : NULL;
}

/* A response with two thinking blocks (each its own signature), an empty
 * signed one, a redacted one, text between them and a tool call. */
static json_object *capture_rich_response(void) {
   llm_claude_capture_t c;
   memset(&c, 0, sizeof(c));
   feed(&c, "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":"
            "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"\"}}");
   feed(&c, "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
            "{\"type\":\"thinking_delta\",\"thinking\":\"Let me \"}}");
   feed(&c, "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
            "{\"type\":\"thinking_delta\",\"thinking\":\"check.\"}}");
   feed(&c, "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
            "{\"type\":\"signature_delta\",\"signature\":\"SIG-A1\"}}");
   feed(&c, "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
            "{\"type\":\"signature_delta\",\"signature\":\"SIG-A2\"}}");
   feed(&c, "{\"type\":\"content_block_stop\",\"index\":0}");
   feed(&c, "{\"type\":\"content_block_start\",\"index\":1,\"content_block\":"
            "{\"type\":\"text\",\"text\":\"\"}}");
   feed(&c, "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":"
            "{\"type\":\"text_delta\",\"text\":\"Checking now.\"}}");
   feed(&c, "{\"type\":\"content_block_stop\",\"index\":1}");
   feed(&c, "{\"type\":\"content_block_start\",\"index\":2,\"content_block\":"
            "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"\"}}");
   feed(&c, "{\"type\":\"content_block_delta\",\"index\":2,\"delta\":"
            "{\"type\":\"signature_delta\",\"signature\":\"SIG-B\"}}");
   feed(&c, "{\"type\":\"content_block_stop\",\"index\":2}");
   feed(&c, "{\"type\":\"content_block_start\",\"index\":3,\"content_block\":"
            "{\"type\":\"redacted_thinking\",\"data\":\"ENCRYPTED\"}}");
   feed(&c, "{\"type\":\"content_block_stop\",\"index\":3}");
   feed(&c, "{\"type\":\"content_block_start\",\"index\":4,\"content_block\":"
            "{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"weather\",\"input\":{}}}");
   feed(&c, "{\"type\":\"content_block_delta\",\"index\":4,\"delta\":"
            "{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"city\\\":\"}}");
   feed(&c, "{\"type\":\"content_block_delta\",\"index\":4,\"delta\":"
            "{\"type\":\"input_json_delta\",\"partial_json\":\"\\\"Atlanta\\\"}\"}}");
   feed(&c, "{\"type\":\"content_block_stop\",\"index\":4}");
   json_object *content = llm_claude_capture_take(&c);
   llm_claude_capture_reset(&c);
   return content;
}

static void test_capture_keeps_every_block_in_order(void) {
   json_object *content = capture_rich_response();
   TEST_ASSERT_NOT_NULL(content);
   TEST_ASSERT_EQUAL_INT(5, json_object_array_length(content));
   TEST_ASSERT_EQUAL_STRING("thinking", str_at(content, 0, "type"));
   TEST_ASSERT_EQUAL_STRING("Let me check.", str_at(content, 0, "thinking"));
   /* Each block keeps its own signature (never one concatenation). */
   TEST_ASSERT_EQUAL_STRING("SIG-A1SIG-A2", str_at(content, 0, "signature"));
   TEST_ASSERT_EQUAL_STRING("text", str_at(content, 1, "type"));
   TEST_ASSERT_EQUAL_STRING("Checking now.", str_at(content, 1, "text"));
   /* An empty-text thinking block is kept, with its signature. */
   TEST_ASSERT_EQUAL_STRING("thinking", str_at(content, 2, "type"));
   TEST_ASSERT_EQUAL_STRING("", str_at(content, 2, "thinking"));
   TEST_ASSERT_EQUAL_STRING("SIG-B", str_at(content, 2, "signature"));
   TEST_ASSERT_EQUAL_STRING("redacted_thinking", str_at(content, 3, "type"));
   TEST_ASSERT_EQUAL_STRING("ENCRYPTED", str_at(content, 3, "data"));
   TEST_ASSERT_EQUAL_STRING("tool_use", str_at(content, 4, "type"));
   json_object *input = NULL;
   TEST_ASSERT_TRUE(
       json_object_object_get_ex(json_object_array_get_idx(content, 4), "input", &input));
   TEST_ASSERT_EQUAL_STRING("Atlanta",
                            json_object_get_string(json_object_object_get(input, "city")));
   json_object_put(content);
}

static void test_blocks_replay_to_claude_verbatim(void) {
   json_object *content = capture_rich_response();
   json_object *blocks = llm_turn_blocks_from_claude(content, TEST_CLAUDE_CARRIER,
                                                     "claude-opus-5-5");
   TEST_ASSERT_NOT_NULL(blocks);
   json_object *rendered = llm_turn_blocks_render_claude(blocks, TEST_CLAUDE_CARRIER);
   /* The replay is the response as received, block for block. */
   TEST_ASSERT_EQUAL_STRING(json_object_to_json_string_ext(content, JSON_C_TO_STRING_PLAIN),
                            json_object_to_json_string_ext(rendered, JSON_C_TO_STRING_PLAIN));
   json_object_put(rendered);
   json_object_put(blocks);
   json_object_put(content);
}

static void test_foreign_reasoning_is_left_out(void) {
   json_object *blocks = llm_turn_blocks_new();
   llm_turn_blocks_add_reasoning(blocks, "openai", "openai-responses", "gpt-5.6-luna",
                                 json_tokener_parse("{\"type\":\"reasoning\",\"id\":\"rs_1\"}"));
   llm_turn_blocks_add_text(blocks, "Hello.");
   json_object *rendered = llm_turn_blocks_render_claude(blocks, TEST_CLAUDE_CARRIER);
   TEST_ASSERT_EQUAL_INT(1, json_object_array_length(rendered));
   TEST_ASSERT_EQUAL_STRING("text", str_at(rendered, 0, "type"));
   json_object_put(rendered);
   json_object_put(blocks);
}

static void test_final_text_replaces_the_answer_keeping_reasoning(void) {
   json_object *blocks = llm_turn_blocks_new();
   llm_turn_blocks_add_reasoning(blocks, TEST_CLAUDE_CARRIER, LLM_FORMAT_ANTHROPIC,
                                 "claude-opus-5-5",
                                 json_tokener_parse("{\"type\":\"thinking\",\"thinking\":\"t\","
                                                    "\"signature\":\"S\"}"));
   llm_turn_blocks_add_text(blocks, "Raw answer [M1] <tag>");
   llm_turn_blocks_add_text(blocks, " more raw");
   json_object *final = llm_turn_blocks_with_final_text(blocks, "Clean answer.");
   TEST_ASSERT_EQUAL_INT(2, json_object_array_length(final));
   TEST_ASSERT_EQUAL_STRING("reasoning", str_at(final, 0, "type"));
   TEST_ASSERT_EQUAL_STRING("Clean answer.", str_at(final, 1, "text"));
   json_object_put(final);
   json_object_put(blocks);

   /* No text block: the answer goes at the end. */
   blocks = llm_turn_blocks_new();
   llm_turn_blocks_add_reasoning(blocks, TEST_CLAUDE_CARRIER, LLM_FORMAT_ANTHROPIC, "m",
                                 json_tokener_parse("{\"type\":\"thinking\",\"thinking\":\"\","
                                                    "\"signature\":\"S\"}"));
   final = llm_turn_blocks_with_final_text(blocks, "Answer.");
   TEST_ASSERT_EQUAL_INT(2, json_object_array_length(final));
   TEST_ASSERT_EQUAL_STRING("Answer.", str_at(final, 1, "text"));
   json_object_put(final);
   json_object_put(blocks);
}

/* A stream cut off mid-block (cancel, dropped connection): the open block's
 * signature is incomplete and must never be replayed. */
static void test_an_unfinished_block_is_discarded(void) {
   llm_claude_capture_t c;
   memset(&c, 0, sizeof(c));
   feed(&c, "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":"
            "{\"type\":\"text\",\"text\":\"\"}}");
   feed(&c, "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
            "{\"type\":\"text_delta\",\"text\":\"Hi\"}}");
   feed(&c, "{\"type\":\"content_block_stop\",\"index\":0}");
   feed(&c, "{\"type\":\"content_block_start\",\"index\":1,\"content_block\":"
            "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"\"}}");
   feed(&c, "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":"
            "{\"type\":\"signature_delta\",\"signature\":\"PARTIAL\"}}");
   json_object *content = llm_claude_capture_take(&c);
   TEST_ASSERT_EQUAL_INT(1, json_object_array_length(content));
   TEST_ASSERT_EQUAL_STRING("text", str_at(content, 0, "type"));
   json_object_put(content);
   llm_claude_capture_reset(&c);
}

static void test_unknown_blocks_are_kept_for_their_vendor(void) {
   json_object *content = json_tokener_parse(
       "[{\"type\":\"server_tool_use\",\"id\":\"srv_1\",\"name\":\"web_search\",\"input\":{}},"
       "{\"type\":\"text\",\"text\":\"Found it.\"}]");
   json_object *blocks = llm_turn_blocks_from_claude(content, TEST_CLAUDE_CARRIER,
                                                     "claude-opus-5-5");
   json_object *rendered = llm_turn_blocks_render_claude(blocks, TEST_CLAUDE_CARRIER);
   TEST_ASSERT_EQUAL_STRING(json_object_to_json_string_ext(content, JSON_C_TO_STRING_PLAIN),
                            json_object_to_json_string_ext(rendered, JSON_C_TO_STRING_PLAIN));
   json_object_put(rendered);
   json_object_put(blocks);
   json_object_put(content);
}

/* A thinking block larger than a default buffer (256 KiB) is kept whole: an
 * emptied block with its signature would be rejected as tampered. */
static void test_a_large_block_is_kept_whole(void) {
   const size_t big = 600 * 1024;
   char *chunk = malloc(big + 1);
   TEST_ASSERT_NOT_NULL(chunk);
   memset(chunk, 'x', big);
   chunk[big] = '\0';
   llm_claude_capture_t c;
   memset(&c, 0, sizeof(c));
   feed(&c, "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":"
            "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"\"}}");
   json_object *delta = json_object_new_object();
   json_object_object_add(delta, "type", json_object_new_string("thinking_delta"));
   json_object_object_add(delta, "thinking", json_object_new_string(chunk));
   llm_claude_capture_delta(&c, delta);
   json_object_put(delta);
   feed(&c, "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
            "{\"type\":\"signature_delta\",\"signature\":\"SIG\"}}");
   feed(&c, "{\"type\":\"content_block_stop\",\"index\":0}");
   json_object *content = llm_claude_capture_take(&c);
   TEST_ASSERT_EQUAL_INT(1, json_object_array_length(content));
   TEST_ASSERT_EQUAL_size_t(big, strlen(str_at(content, 0, "thinking")));
   TEST_ASSERT_EQUAL_STRING("SIG", str_at(content, 0, "signature"));
   json_object_put(content);
   llm_claude_capture_reset(&c);
   free(chunk);
}

static json_object *answer_with_blocks(const char *text, const char *thinking, const char *sig) {
   json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string("assistant"));
   json_object_object_add(m, "content", json_object_new_string(text));
   json_object *blocks = llm_turn_blocks_new();
   json_object *native = json_object_new_object();
   json_object_object_add(native, "type", json_object_new_string("thinking"));
   json_object_object_add(native, "thinking", json_object_new_string(thinking));
   json_object_object_add(native, "signature", json_object_new_string(sig));
   llm_turn_blocks_add_reasoning(blocks, TEST_CLAUDE_CARRIER, LLM_FORMAT_ANTHROPIC, "m", native);
   llm_turn_blocks_add_text(blocks, text);
   json_object_object_add(m, LLM_TURN_BLOCKS_KEY, blocks);
   json_object_object_add(m, "_internal_probe", json_object_new_object());
   return m;
}

/* Extraction, summaries and logs never see a vendor's reasoning. */
static void test_strip_internal_removes_every_internal_key(void) {
   json_object *history = json_object_new_array();
   json_object_array_add(history, answer_with_blocks("Hi.", "secret reasoning", "SIG"));
   json_object *clean = llm_history_strip_internal(history);
   json_object *m = json_object_array_get_idx(clean, 0);
   TEST_ASSERT_FALSE(json_object_object_get_ex(m, LLM_TURN_BLOCKS_KEY, NULL));
   TEST_ASSERT_FALSE(json_object_object_get_ex(m, "_internal_probe", NULL));
   TEST_ASSERT_NULL(strstr(json_object_to_json_string(clean), "secret reasoning"));
   /* A deep copy: the original keeps them. */
   TEST_ASSERT_TRUE(
       json_object_object_get_ex(json_object_array_get_idx(history, 0), LLM_TURN_BLOCKS_KEY, NULL));
   json_object_put(clean);
   json_object_put(history);
}

/* Changing a recorded turn's text changes what's replayed too. */
static void test_set_text_keeps_blocks_in_step(void) {
   json_object *m = answer_with_blocks("A long reply that was cut.", "t", "SIG");
   llm_turn_message_set_text(m, "A long reply");
   TEST_ASSERT_EQUAL_STRING("A long reply",
                            json_object_get_string(json_object_object_get(m, "content")));
   json_object *rendered = llm_turn_blocks_render_claude(
       json_object_object_get(m, LLM_TURN_BLOCKS_KEY), TEST_CLAUDE_CARRIER);
   TEST_ASSERT_EQUAL_INT(2, json_object_array_length(rendered));
   TEST_ASSERT_EQUAL_STRING("A long reply", str_at(rendered, 1, "text"));
   TEST_ASSERT_EQUAL_STRING("SIG", str_at(rendered, 0, "signature"));
   json_object_put(rendered);
   json_object_put(m);
}

static void test_reasoning_counts_toward_context(void) {
   json_object *m = answer_with_blocks("Hi.", "twelve chars", "SIG");
   TEST_ASSERT_EQUAL_size_t(12, llm_turn_message_reasoning_chars(m));
   json_object_put(m);
   /* Empty text (display omitted): the signature stands in for it. */
   m = answer_with_blocks("Hi.", "", "0123456789");
   TEST_ASSERT_EQUAL_size_t(10, llm_turn_message_reasoning_chars(m));
   json_object_put(m);
}

/* Any number of internal keys go; the wire copy shares clean messages. */
static void test_copies_drop_every_internal_key(void) {
   json_object *history = json_object_new_array();
   json_object *busy = json_object_new_object();
   json_object_object_add(busy, "role", json_object_new_string("assistant"));
   json_object_object_add(busy, "content", json_object_new_string("x"));
   for (int i = 0; i < 20; i++) {
      char key[16];
      snprintf(key, sizeof(key), "_k%d", i);
      json_object_object_add(busy, key, json_object_new_int(i));
   }
   json_object *plain = json_object_new_object();
   json_object_object_add(plain, "role", json_object_new_string("user"));
   json_object_array_add(history, busy);
   json_object_array_add(history, plain);

   json_object *stripped = llm_history_strip_internal(history);
   json_object *wire = llm_history_wire_copy(history);
   TEST_ASSERT_EQUAL_INT(2, json_object_object_length(json_object_array_get_idx(stripped, 0)));
   TEST_ASSERT_EQUAL_INT(2, json_object_object_length(json_object_array_get_idx(wire, 0)));
   TEST_ASSERT_TRUE(json_object_array_get_idx(wire, 1) == plain);
   TEST_ASSERT_TRUE(json_object_array_get_idx(stripped, 1) != plain);
   TEST_ASSERT_EQUAL_INT(22, json_object_object_length(busy)); /* the original is untouched */
   json_object_put(stripped);
   json_object_put(wire);
   json_object_put(history);
}

/* Opaque blocks are replayed whole, so they count whole. */
static void test_opaque_blocks_count_toward_context(void) {
   json_object *blocks = json_object_new_array();
   json_object *b = json_object_new_object();
   json_object_object_add(b, "type", json_object_new_string("opaque"));
   json_object *native = json_object_new_object();
   json_object_object_add(native, "type", json_object_new_string("x"));
   json_object_object_add(b, "native", native);
   json_object_array_add(blocks, b);
   json_object *m = json_object_new_object();
   json_object_object_add(m, LLM_TURN_BLOCKS_KEY, blocks);
   TEST_ASSERT_EQUAL_size_t(strlen("{\"type\":\"x\"}"), llm_turn_message_reasoning_chars(m));
   json_object_put(m);
}

/* Responses rendering: a run of text is one assistant message; a tool call
 * ends it; OpenAI reasoning goes back verbatim; Anthropic reasoning doesn't. */
static void test_render_responses(void) {
   struct json_object *blocks = llm_turn_blocks_new();
   llm_turn_blocks_add_text(blocks, "one");
   llm_turn_blocks_add_text(blocks, "two");
   llm_turn_blocks_add_tool_call(blocks, "call_1", "search", "");
   struct json_object *r = json_object_new_object();
   json_object_object_add(r, "type", json_object_new_string("reasoning"));
   json_object_object_add(r, "encrypted_content", json_object_new_string("ENC"));
   llm_turn_blocks_add_reasoning(blocks, "api.openai.com", LLM_FORMAT_OPENAI, "m", r);
   struct json_object *t = json_object_new_object();
   json_object_object_add(t, "type", json_object_new_string("thinking"));
   json_object_object_add(t, "thinking", json_object_new_string("hidden"));
   llm_turn_blocks_add_reasoning(blocks, TEST_CLAUDE_CARRIER, LLM_FORMAT_ANTHROPIC, "c", t);
   llm_turn_blocks_add_text(blocks, "three");

   struct json_object *input = json_object_new_array();
   llm_turn_blocks_render_responses(blocks, input, "api.openai.com", "m");
   TEST_ASSERT_EQUAL_INT(4, json_object_array_length(input));
   struct json_object *first = json_object_array_get_idx(input, 0), *content, *v;
   TEST_ASSERT_TRUE(json_object_object_get_ex(first, "content", &content));
   TEST_ASSERT_EQUAL_INT(2, json_object_array_length(content)); /* "one" + "two" */
   struct json_object *call = json_object_array_get_idx(input, 1);
   TEST_ASSERT_TRUE(json_object_object_get_ex(call, "arguments", &v));
   TEST_ASSERT_EQUAL_STRING("{}", json_object_get_string(v));
   struct json_object *reasoning = json_object_array_get_idx(input, 2);
   TEST_ASSERT_TRUE(json_object_object_get_ex(reasoning, "encrypted_content", &v));
   TEST_ASSERT_NULL(strstr(json_object_to_json_string(input), "hidden"));

   /* The encrypted content stands in for the reasoning in size estimates. */
   struct json_object *m = json_object_new_object();
   json_object_object_add(m, LLM_TURN_BLOCKS_KEY, json_object_get(blocks));
   TEST_ASSERT_EQUAL_INT(3 + 6, (int)llm_turn_message_reasoning_chars(m)); /* ENC + hidden */
   json_object_put(m);
   json_object_put(input);
   json_object_put(blocks);
}

/* A turn's calls are the calls that ran: one that didn't run goes, one that
 * ran without a block is added after the last call kept, text and reasoning
 * keep their places; a final answer keeps no calls. */
static void test_blocks_with_calls(void) {
   struct json_object *blocks = llm_turn_blocks_new();
   llm_turn_blocks_add_text(blocks, "before");
   llm_turn_blocks_add_tool_call(blocks, "c1", "a", "{\"full\":true}");
   llm_turn_blocks_add_tool_call(blocks, "c9", "never", "{}"); /* didn't run */
   llm_turn_blocks_add_text(blocks, "after");
   const llm_turn_call_t ran[] = { { "c1", "a", "{\"cut\":1}" }, { "c2", "b", "{}" } };

   struct json_object *out = llm_turn_blocks_with_calls(blocks, ran, 2);
   TEST_ASSERT_EQUAL_INT(4, json_object_array_length(out));
   const char *order[] = { "before", "c1", "c2", "after" };
   for (int i = 0; i < 4; i++) {
      struct json_object *b = json_object_array_get_idx(out, i), *v;
      const char *key = (i == 1 || i == 2) ? "id" : "text";
      TEST_ASSERT_TRUE(json_object_object_get_ex(b, key, &v));
      TEST_ASSERT_EQUAL_STRING(order[i], json_object_get_string(v));
   }
   struct json_object *args;
   TEST_ASSERT_TRUE(
       json_object_object_get_ex(json_object_array_get_idx(out, 1), "arguments", &args));
   TEST_ASSERT_EQUAL_STRING("{\"full\":true}", json_object_get_string(args)); /* as sent */
   json_object_put(out);

   out = llm_turn_blocks_with_calls(blocks, NULL, 0);
   TEST_ASSERT_EQUAL_INT(2, json_object_array_length(out)); /* the text only */
   json_object_put(out);
   json_object_put(blocks);
}

/* A message recorded without blocks reads the same as one with them. */
static void test_message_blocks(void) {
   struct json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string("assistant"));
   json_object_object_add(m, "content", json_object_new_string("hi"));
   struct json_object *tcs = json_object_new_array(), *tc = json_object_new_object(),
                      *fn = json_object_new_object();
   json_object_object_add(tc, "id", json_object_new_string("c1"));
   json_object_object_add(fn, "name", json_object_new_string("a"));
   json_object_object_add(fn, "arguments", json_object_new_string("{}"));
   json_object_object_add(tc, "function", fn);
   json_object_array_add(tcs, tc);
   json_object_object_add(m, "tool_calls", tcs);
   struct json_object *b = llm_turn_message_blocks(m);
   TEST_ASSERT_EQUAL_INT(2, json_object_array_length(b));
   json_object_put(b);
   json_object_put(m);
}

/* The pieces OpenRouter streamed for a Gemini turn (probed live): text in
 * parts at index 0, then the signature as a separate encrypted entry at the
 * same index.  Same index and type merge; a new type starts an entry. */
static void test_reasoning_details_merge(void) {
   llm_reasoning_details_t acc;
   llm_reasoning_details_init(&acc);
   const char *pieces[] = {
      "{\"type\":\"reasoning.text\",\"text\":\"ab\",\"format\":\"google-gemini-v1\",\"index\":0}",
      "{\"type\":\"reasoning.text\",\"text\":\"cd\",\"format\":\"google-gemini-v1\",\"index\":0}",
      "{\"type\":\"reasoning.encrypted\",\"data\":\"SIG\",\"id\":\"call_1\","
      "\"format\":\"google-gemini-v1\",\"index\":0}",
      "{\"type\":\"reasoning.text\",\"text\":\"ef\",\"index\":1,\"signature\":null}",
      "{\"type\":\"reasoning.text\",\"text\":\"\",\"index\":1,\"signature\":\"S2\"}",
      /* Pieces with no index continue an entry of the same type too. */
      "{\"type\":\"reasoning.summary\",\"summary\":\"s1\"}",
      "{\"type\":\"reasoning.summary\",\"summary\":\"s2\"}",
   };
   for (size_t i = 0; i < sizeof(pieces) / sizeof(pieces[0]); i++) {
      struct json_object *p = json_tokener_parse(pieces[i]);
      llm_reasoning_details_add(&acc, p);
      json_object_put(p);
   }
   struct json_object *details = llm_reasoning_details_finish(&acc);
   TEST_ASSERT_NOT_NULL(details);
   TEST_ASSERT_EQUAL_INT(4, json_object_array_length(details));
   struct json_object *v;
   TEST_ASSERT_TRUE(json_object_object_get_ex(json_object_array_get_idx(details, 0), "text", &v));
   TEST_ASSERT_EQUAL_STRING("abcd", json_object_get_string(v));
   TEST_ASSERT_TRUE(json_object_object_get_ex(json_object_array_get_idx(details, 1), "data", &v));
   TEST_ASSERT_EQUAL_STRING("SIG", json_object_get_string(v));
   TEST_ASSERT_TRUE(
       json_object_object_get_ex(json_object_array_get_idx(details, 2), "signature", &v));
   TEST_ASSERT_EQUAL_STRING("S2", json_object_get_string(v));
   TEST_ASSERT_TRUE(
       json_object_object_get_ex(json_object_array_get_idx(details, 3), "summary", &v));
   TEST_ASSERT_EQUAL_STRING("s1s2", json_object_get_string(v));
   json_object_put(details);
   llm_reasoning_details_free(&acc);

   /* Past the size cap, none are kept: a partial sequence isn't the sequence. */
   llm_reasoning_details_init(&acc);
   char *big = malloc(LLM_REASONING_DETAILS_BYTES_MAX / 2 + 1);
   memset(big, 'x', LLM_REASONING_DETAILS_BYTES_MAX / 2);
   big[LLM_REASONING_DETAILS_BYTES_MAX / 2] = '\0';
   for (int i = 0; i < 3; i++) {
      struct json_object *p = json_object_new_object();
      json_object_object_add(p, "type", json_object_new_string("reasoning.text"));
      json_object_object_add(p, "text", json_object_new_string(big));
      json_object_object_add(p, "index", json_object_new_int(0));
      llm_reasoning_details_add(&acc, p);
      json_object_put(p);
   }
   free(big);
   TEST_ASSERT_NULL(llm_reasoning_details_finish(&acc));
   llm_reasoning_details_free(&acc);
}

/* A long reasoning streamed a token at a time stays under the cap: the fields
 * every piece repeats are counted once, not per piece. */
static void test_reasoning_details_small_deltas(void) {
   llm_reasoning_details_t acc;
   llm_reasoning_details_init(&acc);
   for (int i = 0; i < 20000; i++) {
      struct json_object *p = json_tokener_parse(
          "{\"type\":\"reasoning.text\",\"text\":\"abc\",\"format\":\"anthropic-claude-v1\","
          "\"index\":0}");
      llm_reasoning_details_add(&acc, p);
      json_object_put(p);
   }
   struct json_object *details = llm_reasoning_details_finish(&acc);
   TEST_ASSERT_NOT_NULL(details);
   TEST_ASSERT_EQUAL_INT(1, json_object_array_length(details));
   struct json_object *v;
   TEST_ASSERT_TRUE(json_object_object_get_ex(json_object_array_get_idx(details, 0), "text", &v));
   TEST_ASSERT_EQUAL_INT(60000, json_object_get_string_len(v));
   json_object_put(details);
   llm_reasoning_details_free(&acc);
}

/* Reasoning goes back only to the model that made it: its name, or a dated
 * version of it; not another model sharing a prefix, not a router's pick. */
static void test_served_as_asked(void) {
   TEST_ASSERT_TRUE(
       llm_served_as_asked("anthropic/claude-sonnet-5.5", "anthropic/claude-sonnet-5.5"));
   TEST_ASSERT_TRUE(llm_served_as_asked("gpt-5.5-2026-09-01", "gpt-5.5"));
   TEST_ASSERT_TRUE(llm_served_as_asked("", "anthropic/claude-sonnet-5.5"));
   TEST_ASSERT_TRUE(
       llm_served_as_asked("google/gemini-3.1-pro-preview", "google/gemini-3.1-pro-preview:free"));
   TEST_ASSERT_FALSE(llm_served_as_asked("deepseek/deepseek-v4-flash-0731", "openrouter/auto"));
   TEST_ASSERT_FALSE(llm_served_as_asked("openai/gpt-5", "openai/gpt-5.5"));
   TEST_ASSERT_FALSE(
       llm_served_as_asked("anthropic/claude-sonnet-5.5", "anthropic/claude-sonnet-5"));
}

/* An OpenRouter entry goes back whole, so its text and signature both count. */
static void test_openrouter_entry_counts_whole(void) {
   struct json_object *blocks = llm_turn_blocks_new();
   llm_turn_blocks_add_reasoning(blocks, "openrouter.ai#1", LLM_FORMAT_OPENROUTER, "m",
                                 json_tokener_parse("{\"type\":\"reasoning.text\","
                                                    "\"text\":\"0123456789\","
                                                    "\"signature\":\"SIG\"}"));
   llm_turn_blocks_add_signed_tool_call(blocks, "c1", "w", "{}", "g#1", "m", "GEMSIG");
   struct json_object *m = json_object_new_object();
   json_object_object_add(m, LLM_TURN_BLOCKS_KEY, blocks);
   TEST_ASSERT_EQUAL_INT(10 + 3 + 6, (int)llm_turn_message_reasoning_chars(m));
   json_object_put(m);
}

/* A chat-completions turn renders its text and calls for anyone; its
 * reasoning and a call's signature only for the endpoint and model that
 * issued them. */
static void test_render_chat(void) {
   const char *C = "openrouter.ai#11111111";
   struct json_object *blocks = llm_turn_blocks_new();
   llm_turn_blocks_add_reasoning(blocks, C, LLM_FORMAT_OPENROUTER, "m",
                                 json_tokener_parse("{\"type\":\"reasoning.encrypted\","
                                                    "\"data\":\"OR\"}"));
   struct json_object *t = json_object_new_object();
   json_object_object_add(t, "type", json_object_new_string("thinking"));
   json_object_object_add(t, "thinking", json_object_new_string("hidden"));
   llm_turn_blocks_add_reasoning(blocks, TEST_CLAUDE_CARRIER, LLM_FORMAT_ANTHROPIC, "c", t);
   llm_turn_blocks_add_text(blocks, "one");
   llm_turn_blocks_add_text(blocks, "two");
   llm_turn_blocks_add_signed_tool_call(blocks, "c1", "weather", "{}", C, "m", "GEMSIG");
   llm_turn_blocks_add_signed_tool_call(blocks, "c2", "weather", "{}", "other#2", "m", "X");

   struct json_object *msg = llm_turn_blocks_render_chat(blocks, C, "m"), *v, *calls;
   TEST_ASSERT_TRUE(json_object_object_get_ex(msg, "content", &v));
   TEST_ASSERT_EQUAL_STRING("one\n\ntwo", json_object_get_string(v));
   TEST_ASSERT_TRUE(json_object_object_get_ex(msg, "reasoning_details", &v));
   TEST_ASSERT_EQUAL_INT(1, json_object_array_length(v));
   TEST_ASSERT_TRUE(json_object_object_get_ex(msg, "tool_calls", &calls));
   TEST_ASSERT_EQUAL_INT(2, json_object_array_length(calls));
   TEST_ASSERT_TRUE(
       json_object_object_get_ex(json_object_array_get_idx(calls, 0), "extra_content", &v));
   TEST_ASSERT_NOT_NULL(strstr(json_object_to_json_string(v), "GEMSIG"));
   TEST_ASSERT_FALSE(
       json_object_object_get_ex(json_object_array_get_idx(calls, 1), "extra_content", NULL));
   TEST_ASSERT_NULL(strstr(json_object_to_json_string(msg), "hidden"));
   json_object_put(msg);

   /* Another model on the same endpoint: text and calls only. */
   msg = llm_turn_blocks_render_chat(blocks, C, "m2");
   const char *wire = json_object_to_json_string(msg);
   TEST_ASSERT_NULL(strstr(wire, "reasoning_details"));
   TEST_ASSERT_NULL(strstr(wire, "GEMSIG"));
   TEST_ASSERT_NOT_NULL(strstr(wire, "c1"));
   json_object_put(msg);
   json_object_put(blocks);
}

/* The carrier: the endpoint (no credentials, query or trailing '/') and the
 * key's tag.  The path counts: two gateways on one host are two carriers. */
static void test_carrier(void) {
   char a[LLM_CARRIER_MAX], b[LLM_CARRIER_MAX], c[LLM_CARRIER_MAX], d[LLM_CARRIER_MAX];
   llm_turn_blocks_carrier("https://User:Pass@API.Example.com/v1/?x=1", "0123456789abcdef", a,
                           sizeof(a));
   llm_turn_blocks_carrier("https://api.example.com/v1", "0123456789abcdef", b, sizeof(b));
   llm_turn_blocks_carrier("https://api.example.com/team-b/v1", "0123456789abcdef", c, sizeof(c));
   llm_turn_blocks_carrier("http://api.example.com/v1", "0123456789abcdef", d, sizeof(d));
   TEST_ASSERT_EQUAL_STRING(a, b);
   TEST_ASSERT_NULL(strstr(a, "Pass"));
   TEST_ASSERT_EQUAL_INT(0, strncmp(a, "api.example.com/", 16));
   TEST_ASSERT_NOT_NULL(strstr(a, "#0123456789abcdef"));
   TEST_ASSERT_NOT_EQUAL(0, strcmp(b, c));
   TEST_ASSERT_NOT_EQUAL(0, strcmp(b, d));
   TEST_ASSERT_TRUE(strlen(a) < LLM_CARRIER_MAX);

   /* A long host is cut, never the key tag; and credentials end at the last '@'. */
   char url[160], e[LLM_CARRIER_MAX];
   snprintf(url, sizeof(url), "https://%.*s.example.com/v1", 95,
            "hhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh"
            "hhhhhhhhhhhhhhhh");
   llm_turn_blocks_carrier(url, "0123456789abcdef", e, sizeof(e));
   TEST_ASSERT_NOT_NULL(strstr(e, "#0123456789abcdef"));
   llm_turn_blocks_carrier("https://u:p@ss@host.example.com/v1", "0123456789abcdef", e, sizeof(e));
   TEST_ASSERT_NULL(strstr(e, "ss@"));
   TEST_ASSERT_EQUAL_INT(0, strncmp(e, "host.example.com/", 17));
}

/* No carrier matches nothing: blocks from a legacy array (carrier "") never
 * replay, even to a request that passes "". */
static void test_empty_carrier_never_matches(void) {
   json_object *content = json_tokener_parse(
       "[{\"type\":\"thinking\",\"thinking\":\"plan\",\"signature\":\"SIG\"}]");
   json_object *blocks = llm_turn_blocks_from_claude(content, "", "m");
   json_object *rendered = llm_turn_blocks_render_claude(blocks, "");
   TEST_ASSERT_NULL(strstr(json_object_to_json_string(rendered), "SIG"));
   json_object_put(rendered);
   json_object_put(blocks);
   json_object_put(content);
}

/* The copy for extraction, summaries and logs has no reasoning: a Claude
 * turn's thinking and redacted thinking go, its text and tool call stay, and
 * the history itself is untouched. */
static void test_strip_internal_drops_thinking_parts(void) {
   struct json_object *history = json_object_new_array();
   struct json_object *turn = json_tokener_parse(
       "{\"role\":\"assistant\",\"content\":["
       "{\"type\":\"thinking\",\"thinking\":\"private plan\",\"signature\":\"SIG\"},"
       "{\"type\":\"redacted_thinking\",\"data\":\"ENC\"},"
       "{\"type\":\"web_search_tool_result\",\"content\":{\"encrypted_content\":\"OPQ\"}},"
       "{\"type\":\"text\",\"text\":\"Checking.\"},"
       "{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"weather\",\"input\":{}}]}");
   json_object_array_add(history, turn);
   struct json_object *copy = llm_history_strip_internal(history);
   TEST_ASSERT_NOT_NULL(copy);
   const char *text = json_object_to_json_string(copy);
   TEST_ASSERT_NULL(strstr(text, "private plan"));
   TEST_ASSERT_NULL(strstr(text, "SIG"));
   TEST_ASSERT_NULL(strstr(text, "ENC"));
   TEST_ASSERT_NULL(strstr(text, "OPQ"));
   TEST_ASSERT_NOT_NULL(strstr(text, "Checking."));
   TEST_ASSERT_NOT_NULL(strstr(text, "t1"));
   TEST_ASSERT_NOT_NULL(strstr(json_object_to_json_string(history), "private plan"));
   json_object_put(copy);
   json_object_put(history);
}

/* A turn with every kind of block, and data a careless round trip would
 * change: slashes, non-ASCII text, a precise double, key order. */
static struct json_object *every_kind_of_block(void) {
   struct json_object *b = llm_turn_blocks_new();
   llm_turn_blocks_add_reasoning(
       b, TEST_CLAUDE_CARRIER, LLM_FORMAT_ANTHROPIC, "claude-opus-5-5",
       json_tokener_parse("{\"type\":\"thinking\",\"thinking\":\"a/b \\u00e9t\u00e9 \\\"q\\\"\","
                          "\"signature\":\"S+/=\"}"));
   llm_turn_blocks_add_text(b, "See https://example.com/x \u2014 caf\u00e9");
   llm_turn_blocks_add_signed_tool_call(b, "c1", "weather", "{\"lat\":33.7490000001,\"z\":1}",
                                        "gemini.example#01", "g", "GEMSIG/+=");
   json_object_array_add(b, json_tokener_parse(
                                "{\"type\":\"opaque\",\"carrier\":\"anthropic\",\"format\":"
                                "\"anthropic\",\"native\":{\"type\":\"server_tool_use\","
                                "\"input\":{\"q\":[1,2.50,{\"k\":null}]}}}"));
   return b;
}

/* Stored and reloaded, a turn replays byte for byte what it did before. */
static void test_stored_round_trip_is_exact(void) {
   struct json_object *blocks = every_kind_of_block();
   char *stored = llm_turn_blocks_to_stored(blocks);
   TEST_ASSERT_NOT_NULL(stored);
   TEST_ASSERT_NULL_MESSAGE(strstr(stored, "\\/"), stored);
   TEST_ASSERT_EQUAL_INT(0, strncmp(stored, "{\"v\":1,\"blocks\":[", 17));

   struct json_object *back = llm_turn_blocks_from_stored(stored, strlen(stored), 1);
   TEST_ASSERT_NOT_NULL(back);
   const int flags = JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE;
   TEST_ASSERT_EQUAL_STRING(json_object_to_json_string_ext(blocks, flags),
                            json_object_to_json_string_ext(back, flags));

   struct json_object *before = llm_turn_blocks_render_claude(blocks, TEST_CLAUDE_CARRIER);
   struct json_object *after = llm_turn_blocks_render_claude(back, TEST_CLAUDE_CARRIER);
   TEST_ASSERT_EQUAL_STRING(json_object_to_json_string_ext(before, flags),
                            json_object_to_json_string_ext(after, flags));
   struct json_object *chat_before = llm_turn_blocks_render_chat(blocks, "gemini.example#01", "g");
   struct json_object *chat_after = llm_turn_blocks_render_chat(back, "gemini.example#01", "g");
   TEST_ASSERT_EQUAL_STRING(json_object_to_json_string_ext(chat_before, flags),
                            json_object_to_json_string_ext(chat_after, flags));

   /* Stored again, the same bytes. */
   char *again = llm_turn_blocks_to_stored(back);
   TEST_ASSERT_EQUAL_STRING(stored, again);

   free(again);
   free(stored);
   json_object_put(chat_before);
   json_object_put(chat_after);
   json_object_put(before);
   json_object_put(after);
   json_object_put(back);
   json_object_put(blocks);
   TEST_ASSERT_NULL(llm_turn_blocks_to_stored(NULL));
}

/* Anything but a valid envelope loads without blocks. */
static void test_stored_fails_closed(void) {
   static const char *const bad[] = {
      "",
      "[]",
      "{\"v\":2,\"blocks\":[]}",
      "{\"v\":\"1\",\"blocks\":[]}",
      "{\"v\":1}",
      "{\"v\":1,\"blocks\":{}}",
      "{\"v\":1,\"blocks\":[{\"type\":\"text\"}]}",
      "{\"v\":1,\"blocks\":[{\"type\":\"image\",\"url\":\"x\"}]}",
      "{\"v\":1,\"blocks\":[{\"type\":\"tool_call\",\"id\":\"a\",\"name\":\"b\"}]}",
      "{\"v\":1,\"blocks\":[{\"type\":\"tool_call\",\"id\":\"a\",\"name\":\"b\","
      "\"arguments\":\"{}\",\"sig\":{\"carrier\":\"c\"}}]}",
      "{\"v\":1,\"blocks\":[{\"type\":\"reasoning\",\"carrier\":\"c\",\"format\":\"f\"}]}",
      "{\"v\":1,\"blocks\":[{\"type\":\"reasoning\",\"carrier\":\"c\",\"format\":\"f\","
      "\"native\":\"x\"}]}",
      "{\"v\":1,\"blocks\":[{\"type\":\"opaque\",\"format\":\"f\",\"native\":{}}]}",
      "{\"v\":1,\"blocks\":[]} trailing",
      "{\"v\":1,\"blocks\":[",
   };
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
      TEST_ASSERT_NULL_MESSAGE(llm_turn_blocks_from_stored(bad[i], strlen(bad[i]), (long long)i),
                               bad[i]);
   }

   /* Nested past the depth limit. */
   char deep[512];
   size_t n = (size_t)snprintf(deep, sizeof(deep),
                               "{\"v\":1,\"blocks\":[{\"type\":\"opaque\",\"carrier\":\"c\","
                               "\"format\":\"f\",\"native\":{\"a\":");
   for (int i = 0; i < 70; i++)
      deep[n++] = '[';
   for (int i = 0; i < 70; i++)
      deep[n++] = ']';
   memcpy(deep + n, "}}]}", 5);
   TEST_ASSERT_NULL(llm_turn_blocks_from_stored(deep, strlen(deep), 99));

   /* Too many blocks. */
   struct json_object *many = llm_turn_blocks_new();
   for (int i = 0; i <= LLM_TURN_BLOCKS_STORED_ENTRIES_MAX; i++)
      llm_turn_blocks_add_text(many, "x");
   char *stored = llm_turn_blocks_to_stored(many);
   TEST_ASSERT_NOT_NULL(stored);
   TEST_ASSERT_NULL(llm_turn_blocks_from_stored(stored, strlen(stored), 100));
   free(stored);
   json_object_put(many);

   /* A length that cuts the text short is not the whole value. */
   const char *ok = "{\"v\":1,\"blocks\":[{\"type\":\"text\",\"text\":\"hi\"}]}";
   struct json_object *good = llm_turn_blocks_from_stored(ok, strlen(ok), 101);
   TEST_ASSERT_NOT_NULL(good);
   json_object_put(good);
   TEST_ASSERT_NULL(llm_turn_blocks_from_stored(ok, strlen(ok) - 1, 102));
}

/* Blocks replay only with the row whose calls they record. */
static bool calls_match_text(struct json_object *blocks, const char *tool_calls) {
   struct json_object *calls = tool_calls ? json_tokener_parse(tool_calls) : NULL;
   const bool match = llm_turn_blocks_calls_match(blocks, calls);
   json_object_put(calls);
   return match;
}

static void test_calls_match(void) {
   struct json_object *blocks = llm_turn_blocks_new();
   llm_turn_blocks_add_text(blocks, "Checking.");
   TEST_ASSERT_TRUE(calls_match_text(blocks, NULL));
   TEST_ASSERT_TRUE(calls_match_text(blocks, "[]"));
   TEST_ASSERT_FALSE(calls_match_text(blocks, "[{\"id\":\"a\"}]"));

   llm_turn_blocks_add_tool_call(blocks, "a", "x", "{}");
   llm_turn_blocks_add_tool_call(blocks, "b", "y", "{}");
   TEST_ASSERT_TRUE(calls_match_text(blocks, "[{\"id\":\"a\"},{\"id\":\"b\"}]"));
   TEST_ASSERT_FALSE(calls_match_text(blocks, "[{\"id\":\"b\"},{\"id\":\"a\"}]"));
   TEST_ASSERT_FALSE(calls_match_text(blocks, "[{\"id\":\"a\"}]"));
   TEST_ASSERT_FALSE(calls_match_text(blocks, NULL));
   TEST_ASSERT_FALSE(calls_match_text(blocks, "{\"id\":\"a\"}"));
   json_object_put(blocks);
}

/* A reply row's blocks record no tool calls. */
static void test_answer_stored(void) {
   TEST_ASSERT_NULL(llm_turn_blocks_answer_stored(NULL));
   struct json_object *blocks = llm_turn_blocks_new();
   llm_turn_blocks_add_text(blocks, "Done.");
   char *stored = llm_turn_blocks_answer_stored(blocks);
   TEST_ASSERT_NOT_NULL(stored);
   free(stored);
   llm_turn_blocks_add_tool_call(blocks, "a", "x", "{}");
   TEST_ASSERT_NULL(llm_turn_blocks_answer_stored(blocks));
   json_object_put(blocks);
}

/* Claude reasoning goes back only to the endpoint and key that issued it: not
 * to a gateway, another account, or a copy of the database elsewhere. */
static void test_claude_reasoning_stays_with_its_carrier(void) {
   json_object *content = json_tokener_parse(
       "[{\"type\":\"thinking\",\"thinking\":\"plan\",\"signature\":\"SIG\"},"
       "{\"type\":\"text\",\"text\":\"Hi.\"}]");
   json_object *blocks = llm_turn_blocks_from_claude(content, TEST_CLAUDE_CARRIER, "m");
   json_object *mine = llm_turn_blocks_render_claude(blocks, TEST_CLAUDE_CARRIER);
   json_object *gateway = llm_turn_blocks_render_claude(blocks,
                                                        "gateway.example.com#0123456789abcdef");
   json_object *other_key = llm_turn_blocks_render_claude(blocks,
                                                          "api.anthropic.com#fedcba9876543210");
   TEST_ASSERT_NOT_NULL(strstr(json_object_to_json_string(mine), "SIG"));
   TEST_ASSERT_NULL(strstr(json_object_to_json_string(gateway), "SIG"));
   TEST_ASSERT_NULL(strstr(json_object_to_json_string(other_key), "SIG"));
   TEST_ASSERT_NOT_NULL(strstr(json_object_to_json_string(gateway), "Hi."));
   json_object_put(mine);
   json_object_put(gateway);
   json_object_put(other_key);
   json_object_put(blocks);
   json_object_put(content);
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_capture_keeps_every_block_in_order);
   RUN_TEST(test_blocks_replay_to_claude_verbatim);
   RUN_TEST(test_foreign_reasoning_is_left_out);
   RUN_TEST(test_final_text_replaces_the_answer_keeping_reasoning);
   RUN_TEST(test_an_unfinished_block_is_discarded);
   RUN_TEST(test_unknown_blocks_are_kept_for_their_vendor);
   RUN_TEST(test_a_large_block_is_kept_whole);
   RUN_TEST(test_strip_internal_removes_every_internal_key);
   RUN_TEST(test_strip_internal_drops_thinking_parts);
   RUN_TEST(test_set_text_keeps_blocks_in_step);
   RUN_TEST(test_reasoning_counts_toward_context);
   RUN_TEST(test_copies_drop_every_internal_key);
   RUN_TEST(test_opaque_blocks_count_toward_context);
   RUN_TEST(test_render_responses);
   RUN_TEST(test_blocks_with_calls);
   RUN_TEST(test_stored_round_trip_is_exact);
   RUN_TEST(test_stored_fails_closed);
   RUN_TEST(test_calls_match);
   RUN_TEST(test_answer_stored);
   RUN_TEST(test_claude_reasoning_stays_with_its_carrier);
   RUN_TEST(test_message_blocks);
   RUN_TEST(test_reasoning_details_merge);
   RUN_TEST(test_reasoning_details_small_deltas);
   RUN_TEST(test_served_as_asked);
   RUN_TEST(test_openrouter_entry_counts_whole);
   RUN_TEST(test_render_chat);
   RUN_TEST(test_carrier);
   RUN_TEST(test_empty_carrier_never_matches);
   return UNITY_END();
}
