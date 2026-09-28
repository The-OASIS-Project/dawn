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
   json_object *blocks = llm_turn_blocks_from_claude(content, "claude-opus-5-5");
   TEST_ASSERT_NOT_NULL(blocks);
   json_object *rendered = llm_turn_blocks_render_claude(blocks);
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
   json_object *rendered = llm_turn_blocks_render_claude(blocks);
   TEST_ASSERT_EQUAL_INT(1, json_object_array_length(rendered));
   TEST_ASSERT_EQUAL_STRING("text", str_at(rendered, 0, "type"));
   json_object_put(rendered);
   json_object_put(blocks);
}

static void test_final_text_replaces_the_answer_keeping_reasoning(void) {
   json_object *blocks = llm_turn_blocks_new();
   llm_turn_blocks_add_reasoning(blocks, LLM_CARRIER_ANTHROPIC, LLM_FORMAT_ANTHROPIC,
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
   llm_turn_blocks_add_reasoning(blocks, LLM_CARRIER_ANTHROPIC, LLM_FORMAT_ANTHROPIC, "m",
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
   json_object *blocks = llm_turn_blocks_from_claude(content, "claude-opus-5-5");
   json_object *rendered = llm_turn_blocks_render_claude(blocks);
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
   llm_turn_blocks_add_reasoning(blocks, LLM_CARRIER_ANTHROPIC, LLM_FORMAT_ANTHROPIC, "m", native);
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
       json_object_object_get(m, LLM_TURN_BLOCKS_KEY));
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
   llm_turn_blocks_add_reasoning(blocks, LLM_CARRIER_ANTHROPIC, LLM_FORMAT_ANTHROPIC, "c", t);
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
   llm_turn_blocks_add_reasoning(blocks, LLM_CARRIER_ANTHROPIC, LLM_FORMAT_ANTHROPIC, "c", t);
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

/* The carrier: the host (lower case, no credentials) and a key fingerprint. */
static void test_carrier(void) {
   char a[LLM_CARRIER_MAX], b[LLM_CARRIER_MAX], c[LLM_CARRIER_MAX];
   llm_turn_blocks_carrier("https://User:Pass@API.Example.com/v1", "key-1", a, sizeof(a));
   llm_turn_blocks_carrier("https://api.example.com/v1", "key-1", b, sizeof(b));
   llm_turn_blocks_carrier("https://api.example.com/v1", "key-2", c, sizeof(c));
   TEST_ASSERT_EQUAL_STRING(a, b);
   TEST_ASSERT_NULL(strstr(a, "Pass"));
   TEST_ASSERT_EQUAL_INT(0, strncmp(a, "api.example.com#", 16));
   TEST_ASSERT_NOT_EQUAL(0, strcmp(b, c));
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
       "{\"type\":\"text\",\"text\":\"Checking.\"},"
       "{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"weather\",\"input\":{}}]}");
   json_object_array_add(history, turn);
   struct json_object *copy = llm_history_strip_internal(history);
   TEST_ASSERT_NOT_NULL(copy);
   const char *text = json_object_to_json_string(copy);
   TEST_ASSERT_NULL(strstr(text, "private plan"));
   TEST_ASSERT_NULL(strstr(text, "SIG"));
   TEST_ASSERT_NULL(strstr(text, "ENC"));
   TEST_ASSERT_NOT_NULL(strstr(text, "Checking."));
   TEST_ASSERT_NOT_NULL(strstr(text, "t1"));
   TEST_ASSERT_NOT_NULL(strstr(json_object_to_json_string(history), "private plan"));
   json_object_put(copy);
   json_object_put(history);
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
   RUN_TEST(test_message_blocks);
   RUN_TEST(test_reasoning_details_merge);
   RUN_TEST(test_reasoning_details_small_deltas);
   RUN_TEST(test_served_as_asked);
   RUN_TEST(test_openrouter_entry_counts_whole);
   RUN_TEST(test_render_chat);
   RUN_TEST(test_carrier);
   return UNITY_END();
}
