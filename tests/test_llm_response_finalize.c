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
 * Unit tests for src/core/llm_response_finalize.c.
 */

#include <string.h>

#include "core/llm_response_finalize.h"
#include "dawn_error.h"
#include "memory/memory_citation.h"
#include "unity.h"

/* The finalizer calls memory_citation_capture(); these strip tests pass a NULL
 * session (capture would no-op anyway) and don't link the memory subsystem, so
 * stub it here. */
void memory_citation_capture(session_t *session, const char *response_text) {
   (void)session;
   (void)response_text;
}

void setUp(void) {
}
void tearDown(void) {
}

/* Helper: finalize `in`, assert SUCCESS + exact clean text, free. */
static void expect_clean(const char *in, const char *expected) {
   response_final_t out;
   int rc = llm_response_finalize(NULL, in, &out);
   TEST_ASSERT_EQUAL_INT(SUCCESS, rc);
   TEST_ASSERT_NOT_NULL(out.text);
   TEST_ASSERT_EQUAL_STRING(expected, out.text);
   TEST_ASSERT_EQUAL_UINT(strlen(expected), out.length);
   response_final_free(&out);
   TEST_ASSERT_NULL(out.text);
   TEST_ASSERT_EQUAL_UINT(0, out.length);
}

static void test_plain_text_unchanged(void) {
   expect_clean("Hello, how can I help?", "Hello, how can I help?");
}

static void test_markdown_preserved(void) {
   /* Markdown/emojis are TTS-path concerns; the canonical text keeps them. */
   expect_clean("Here is **bold** and a list:\n- one\n- two",
                "Here is **bold** and a list:\n- one\n- two");
}

static void test_single_command_tag_removed(void) {
   expect_clean(
       "Turning the lights on.<command>{\"device\":\"lights\",\"action\":\"on\"}</command>",
       "Turning the lights on.");
}

static void test_command_tag_midtext_preserves_surrounding(void) {
   expect_clean("before<command>{\"a\":1}</command>after", "beforeafter");
}

static void test_multiple_command_tags_removed(void) {
   expect_clean("A<command>{\"x\":1}</command>B<command>{\"y\":2}</command>C", "ABC");
}

static void test_end_of_turn_truncates(void) {
   expect_clean("Real answer.<end_of_turn>leftover model chatter", "Real answer.");
}

static void test_command_and_end_of_turn(void) {
   expect_clean("Done.<command>{\"a\":1}</command><end_of_turn>junk", "Done.");
}

static void test_trailing_whitespace_trimmed(void) {
   expect_clean("Answer.   \n\t ", "Answer.");
}

static void test_unclosed_command_tag_left_intact(void) {
   /* No closing tag → the remainder is untouched (matches historical strip). */
   expect_clean("oops <command>{\"a\":1} no close", "oops <command>{\"a\":1} no close");
}

static void test_cited_tag_removed(void) {
   expect_clean("Sasha is a Persian cat.<cited>M1,M7</cited>", "Sasha is a Persian cat.");
}

static void test_cited_tag_midtext_preserves_surrounding(void) {
   expect_clean("a<cited>M1</cited>b", "ab");
}

static void test_orphan_cited_opener_drops_to_end(void) {
   /* A stream that breaks mid-tag leaves an unclosed opener — drop it entirely. */
   expect_clean("The answer is 42.<cited>M1,M", "The answer is 42.");
}

static void test_cited_with_command_and_eot(void) {
   expect_clean("Done.<command>{\"a\":1}</command> <cited>M2</cited><end_of_turn>x", "Done.");
}

static void test_null_input_yields_empty(void) {
   expect_clean(NULL, "");
}

static void test_empty_input_yields_empty(void) {
   expect_clean("", "");
}

static void test_null_out_returns_failure(void) {
   TEST_ASSERT_EQUAL_INT(FAILURE, llm_response_finalize(NULL, "anything", NULL));
}

static void test_free_is_null_safe(void) {
   response_final_free(NULL); /* must not crash */
   response_final_t out = { 0 };
   response_final_free(&out); /* free of NULL text is fine */
   TEST_PASS();
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_plain_text_unchanged);
   RUN_TEST(test_markdown_preserved);
   RUN_TEST(test_single_command_tag_removed);
   RUN_TEST(test_command_tag_midtext_preserves_surrounding);
   RUN_TEST(test_multiple_command_tags_removed);
   RUN_TEST(test_end_of_turn_truncates);
   RUN_TEST(test_command_and_end_of_turn);
   RUN_TEST(test_trailing_whitespace_trimmed);
   RUN_TEST(test_unclosed_command_tag_left_intact);
   RUN_TEST(test_cited_tag_removed);
   RUN_TEST(test_cited_tag_midtext_preserves_surrounding);
   RUN_TEST(test_orphan_cited_opener_drops_to_end);
   RUN_TEST(test_cited_with_command_and_eot);
   RUN_TEST(test_null_input_yields_empty);
   RUN_TEST(test_empty_input_yields_empty);
   RUN_TEST(test_null_out_returns_failure);
   RUN_TEST(test_free_is_null_safe);
   return UNITY_END();
}
