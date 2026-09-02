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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Unit tests for text_filter command tag stripping.
 */

#include <string.h>

#include "core/text_filter.h"
#include "unity.h"

static cmd_tag_filter_state_t s_state;
static cited_tag_filter_state_t s_cited;

void setUp(void) {
   text_filter_reset(&s_state);
   text_filter_cited_reset(&s_cited);
}

void tearDown(void) {
}

/* ── helper for buffer tests ────────────────────────────────────────────── */

static void filter_to_buf(const char *input, char *out, size_t out_size) {
   text_filter_command_tags_to_buffer(&s_state, input, out, out_size);
}

/* ── tests ──────────────────────────────────────────────────────────────── */

static void test_no_tags(void) {
   char buf[128];
   filter_to_buf("Hello world", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("Hello world", buf);
}

static void test_simple_tag_removal(void) {
   char buf[128];
   filter_to_buf("Hello <command>do stuff</command> world", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("Hello  world", buf);
}

static void test_tag_at_start(void) {
   char buf[128];
   filter_to_buf("<command>hidden</command>visible", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("visible", buf);
}

static void test_tag_at_end(void) {
   char buf[128];
   filter_to_buf("visible<command>hidden</command>", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("visible", buf);
}

static void test_only_tag_content(void) {
   char buf[128];
   filter_to_buf("<command>all hidden</command>", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_nested_tags(void) {
   char buf[128];
   filter_to_buf("<command>outer<command>inner</command></command>", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_multiple_tags(void) {
   char buf[128];
   filter_to_buf("a<command>x</command>b<command>y</command>c", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("abc", buf);
}

static void test_streaming_across_chunks(void) {
   char buf[128] = "";
   int len = 0;

   len = text_filter_command_tags_to_buffer(&s_state, "<com", buf, sizeof(buf));

   char buf2[128] = "";
   int len2 = text_filter_command_tags_to_buffer(&s_state, "mand>hidden</command>after", buf2,
                                                 sizeof(buf2));

   TEST_ASSERT_EQUAL_STRING("after", buf2);
   TEST_ASSERT_EQUAL_INT(5, len2);
   (void)len;
}

static void test_partial_tag_not_matching(void) {
   char buf[128];
   filter_to_buf("<comm!and>", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("<comm!and>", buf);
}

static void test_empty_input(void) {
   char buf[128];
   filter_to_buf("", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_buffer_overflow_protection(void) {
   char buf[5];
   int len = text_filter_command_tags_to_buffer(&s_state, "Hello world", buf, sizeof(buf));
   buf[sizeof(buf) - 1] = '\0';
   TEST_ASSERT_EQUAL_INT(4, len);
   TEST_ASSERT_EQUAL_STRING("Hell", buf);
}

static void test_reset_mid_tag(void) {
   char buf[128];

   /* Start a tag but don't finish it */
   filter_to_buf("<command>partial", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("", buf);
   TEST_ASSERT_TRUE(s_state.nesting_depth > 0);

   /* Reset state */
   text_filter_reset(&s_state);
   TEST_ASSERT_EQUAL_INT(0, s_state.nesting_depth);
   TEST_ASSERT_EQUAL_INT(0, s_state.len);

   /* After reset, text passes through normally */
   filter_to_buf("clean text", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("clean text", buf);
}

/* ── callback test ──────────────────────────────────────────────────────── */

typedef struct {
   char chunks[8][64];
   int count;
} capture_ctx_t;

static void capture_callback(const char *text, size_t len, void *ctx) {
   capture_ctx_t *cap = (capture_ctx_t *)ctx;
   if (cap->count < 8 && len < 64) {
      memcpy(cap->chunks[cap->count], text, len);
      cap->chunks[cap->count][len] = '\0';
      cap->count++;
   }
}

static void test_callback_receives_chunks(void) {
   capture_ctx_t cap = { .count = 0 };

   text_filter_command_tags(&s_state, "before<command>hidden</command>after", capture_callback,
                            &cap);

   TEST_ASSERT_EQUAL_INT(2, cap.count);
   TEST_ASSERT_EQUAL_STRING("before", cap.chunks[0]);
   TEST_ASSERT_EQUAL_STRING("after", cap.chunks[1]);
}

static void test_deeply_nested(void) {
   char buf[128];
   filter_to_buf("<command><command>deep</command></command>", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("", buf);
}

/* ── <cited> memory-citation stream filter ──────────────────────────────── */

/* Feed one delta; append the stripped output onto `acc`. */
static void cited_feed(char *acc, size_t acc_size, const char *delta) {
   char tmp[256];
   text_filter_cited_tags_to_buffer(&s_cited, delta, tmp, sizeof(tmp));
   strncat(acc, tmp, acc_size - strlen(acc) - 1);
}

/* Flush at stream end; append any released trailing bytes onto `acc`. */
static void cited_flush(char *acc, size_t acc_size) {
   char tmp[CITED_TAG_BUF_SIZE];
   text_filter_cited_flush_to_buffer(&s_cited, tmp, sizeof(tmp));
   strncat(acc, tmp, acc_size - strlen(acc) - 1);
}

static void test_cited_no_tag(void) {
   char out[128] = "";
   cited_feed(out, sizeof(out), "just a normal answer");
   cited_flush(out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("just a normal answer", out);
}

static void test_cited_whole_tag_one_delta(void) {
   char out[128] = "";
   cited_feed(out, sizeof(out), "The answer.<cited>M1,M5,M11</cited>");
   cited_flush(out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("The answer.", out);
}

static void test_cited_split_opener_across_deltas(void) {
   /* The exact leak shape: opener split "<cit" | "ed>..." */
   char out[128] = "";
   cited_feed(out, sizeof(out), "The answer.<cit");
   cited_feed(out, sizeof(out), "ed>M1,M5,M11</cited>");
   cited_flush(out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("The answer.", out);
}

static void test_cited_split_closer_across_deltas(void) {
   char out[128] = "";
   cited_feed(out, sizeof(out), "answer<cited>M1</cit");
   cited_feed(out, sizeof(out), "ed>");
   cited_flush(out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("answer", out);
}

static void test_cited_char_by_char(void) {
   /* Worst-case fragmentation: one byte per delta. Nothing must leak. */
   const char *s = "Hi.<cited>M1,M5,M11</cited>";
   char out[128] = "";
   char one[2] = { 0, 0 };
   for (const char *p = s; *p; p++) {
      one[0] = *p;
      cited_feed(out, sizeof(out), one);
   }
   cited_flush(out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("Hi.", out);
}

static void test_cited_false_opener_passes_through(void) {
   /* A '<' run that is NOT the tag must be emitted intact. */
   char out[128] = "";
   cited_feed(out, sizeof(out), "1 < 2 and <ci is fine");
   cited_flush(out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("1 < 2 and <ci is fine", out);
}

static void test_cited_partial_opener_at_end_flushed(void) {
   /* A reply legitimately ending in a tag-prefix must NOT be lost. */
   char out[128] = "";
   cited_feed(out, sizeof(out), "ends with <cit");
   cited_flush(out, sizeof(out)); /* no more input — held bytes were real text */
   TEST_ASSERT_EQUAL_STRING("ends with <cit", out);
}

static void test_cited_truncated_tag_dropped(void) {
   /* Opener completed but closer never arrived (truncated stream): drop the tail. */
   char out[128] = "";
   cited_feed(out, sizeof(out), "answer<cited>M1,M5");
   cited_flush(out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("answer", out);
}

static void test_cited_reset_between_turns(void) {
   char out[128] = "";
   cited_feed(out, sizeof(out), "turn one<cited>M1</cited>");
   cited_flush(out, sizeof(out));
   text_filter_cited_reset(&s_cited); /* dispatch-entry boundary */
   char out2[128] = "";
   cited_feed(out2, sizeof(out2), "turn two clean");
   cited_flush(out2, sizeof(out2));
   TEST_ASSERT_EQUAL_STRING("turn one", out);
   TEST_ASSERT_EQUAL_STRING("turn two clean", out2);
}

static void test_cited_double_open_bracket(void) {
   /* "<<cited>" — the held '<' must NOT be dropped when the next '<' restarts. */
   char out[128] = "";
   cited_feed(out, sizeof(out), "5 <<cited>M1</cited>");
   cited_flush(out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("5 <", out);
}

static void test_cited_double_open_bracket_split(void) {
   /* Held '<' from one delta, '<cited>' opener in the next — '<' must survive. */
   char out[128] = "";
   cited_feed(out, sizeof(out), "a<");
   cited_feed(out, sizeof(out), "<cited>M1</cited>");
   cited_flush(out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("a<", out);
}

static void test_cited_stray_bracket_in_closer(void) {
   /* Stray '<' inside the tag body must restart the closer match, not leak. */
   char out[128] = "";
   cited_feed(out, sizeof(out), "a<cited>x<y</cited>b");
   cited_flush(out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("ab", out);
}

static void test_cited_near_full_false_opener(void) {
   /* "<cited x>" is not the tag (space breaks the match) — must pass through. */
   char out[128] = "";
   cited_feed(out, sizeof(out), "<cited x> stays");
   cited_flush(out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("<cited x> stays", out);
}

static void test_cited_two_blocks_one_stream(void) {
   char out[128] = "";
   cited_feed(out, sizeof(out), "a<cited>M1</cited>b<cited>M2</cited>c");
   cited_flush(out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("abc", out);
}

static void test_command_double_open_bracket(void) {
   /* Lockstep regression for the same held-'<' fix in the command filter. */
   char buf[128];
   filter_to_buf("5 <<command>x</command>", buf, sizeof(buf));
   TEST_ASSERT_EQUAL_STRING("5 <", buf);
}

/* ── whole-string cited strip (text_filter_cited_strip) ──────────────────── */

static void test_cited_strip_trailing_tag(void) {
   /* The common case: a trailing citation tag on an assembled response/sentence. */
   char s[128] = "The answer is 42. <cited>M1,M4</cited>";
   text_filter_cited_strip(s);
   TEST_ASSERT_EQUAL_STRING("The answer is 42. ", s);
}

static void test_cited_strip_multiple_tags(void) {
   char s[128] = "a<cited>M1</cited>b<cited>M2</cited>c";
   text_filter_cited_strip(s);
   TEST_ASSERT_EQUAL_STRING("abc", s);
}

static void test_cited_strip_orphan_opener_truncates(void) {
   /* Orphan opener (truncated response): drop from the opener to end-of-string. */
   char s[128] = "answer<cited>M1,M5";
   text_filter_cited_strip(s);
   TEST_ASSERT_EQUAL_STRING("answer", s);
}

static void test_cited_strip_no_tag_unchanged(void) {
   char s[128] = "plain reply, no tag";
   text_filter_cited_strip(s);
   TEST_ASSERT_EQUAL_STRING("plain reply, no tag", s);
}

static void test_cited_strip_null_safe(void) {
   text_filter_cited_strip(NULL); /* must not crash */
}

/* ── whitespace-tolerant normalize (text_filter_cited_normalize) ──────────── */

static void test_cited_normalize_space_after_open(void) {
   /* The exact production leak: "< cited>...</cited>" with a space after '<'. */
   char s[128] = "The answer. < cited>M1,M2</cited>";
   text_filter_cited_normalize(s);
   TEST_ASSERT_EQUAL_STRING("The answer. <cited>M1,M2</cited>", s);
   text_filter_cited_strip(s); /* now the exact grammar matches → stripped */
   TEST_ASSERT_EQUAL_STRING("The answer. ", s);
}

static void test_cited_normalize_space_before_close_bracket(void) {
   char s[128] = "x<cited >M1</cited>y";
   text_filter_cited_normalize(s);
   TEST_ASSERT_EQUAL_STRING("x<cited>M1</cited>y", s);
}

static void test_cited_normalize_closing_variants(void) {
   char s[128] = "a< cited >M1< / cited >b";
   text_filter_cited_normalize(s);
   TEST_ASSERT_EQUAL_STRING("a<cited>M1</cited>b", s);
   text_filter_cited_strip(s);
   TEST_ASSERT_EQUAL_STRING("ab", s);
}

static void test_cited_normalize_wellformed_unchanged(void) {
   char s[128] = "already <cited>M1</cited> fine";
   text_filter_cited_normalize(s);
   TEST_ASSERT_EQUAL_STRING("already <cited>M1</cited> fine", s);
}

static void test_cited_normalize_non_tag_untouched(void) {
   /* A bare '<' that is not a citation tag must be left alone. */
   char s[128] = "1 < 2 and x<y, no < cite here";
   text_filter_cited_normalize(s);
   TEST_ASSERT_EQUAL_STRING("1 < 2 and x<y, no < cite here", s);
}

static void test_cited_normalize_null_safe(void) {
   text_filter_cited_normalize(NULL); /* must not crash */
}

/* ── whole-string command strip (text_filter_command_strip) ──────────────── */

static void test_command_strip_pair_and_eot(void) {
   char s[128] = "hi <command>{\"a\":1}</command> there<end_of_turn>trailing";
   text_filter_command_strip(s, true);
   TEST_ASSERT_EQUAL_STRING("hi  there", s);
}

static void test_command_strip_orphan_truncates_when_true(void) {
   /* Per-sentence TTS intent: drop a mid-split opener. */
   char s[128] = "oops <command>{\"a\":1} no close";
   text_filter_command_strip(s, true);
   TEST_ASSERT_EQUAL_STRING("oops ", s);
}

static void test_command_strip_orphan_left_when_false(void) {
   /* Complete-response intent: an unclosed <command> is left as literal text. */
   char s[128] = "oops <command>{\"a\":1} no close";
   text_filter_command_strip(s, false);
   TEST_ASSERT_EQUAL_STRING("oops <command>{\"a\":1} no close", s);
}

static void test_command_strip_null_safe(void) {
   text_filter_command_strip(NULL, true); /* must not crash */
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_no_tags);
   RUN_TEST(test_simple_tag_removal);
   RUN_TEST(test_tag_at_start);
   RUN_TEST(test_tag_at_end);
   RUN_TEST(test_only_tag_content);
   RUN_TEST(test_nested_tags);
   RUN_TEST(test_multiple_tags);
   RUN_TEST(test_streaming_across_chunks);
   RUN_TEST(test_partial_tag_not_matching);
   RUN_TEST(test_empty_input);
   RUN_TEST(test_buffer_overflow_protection);
   RUN_TEST(test_reset_mid_tag);
   RUN_TEST(test_callback_receives_chunks);
   RUN_TEST(test_deeply_nested);
   RUN_TEST(test_cited_no_tag);
   RUN_TEST(test_cited_whole_tag_one_delta);
   RUN_TEST(test_cited_split_opener_across_deltas);
   RUN_TEST(test_cited_split_closer_across_deltas);
   RUN_TEST(test_cited_char_by_char);
   RUN_TEST(test_cited_false_opener_passes_through);
   RUN_TEST(test_cited_partial_opener_at_end_flushed);
   RUN_TEST(test_cited_truncated_tag_dropped);
   RUN_TEST(test_cited_reset_between_turns);
   RUN_TEST(test_cited_double_open_bracket);
   RUN_TEST(test_cited_double_open_bracket_split);
   RUN_TEST(test_cited_stray_bracket_in_closer);
   RUN_TEST(test_cited_near_full_false_opener);
   RUN_TEST(test_cited_two_blocks_one_stream);
   RUN_TEST(test_command_double_open_bracket);
   RUN_TEST(test_cited_strip_trailing_tag);
   RUN_TEST(test_cited_strip_multiple_tags);
   RUN_TEST(test_cited_strip_orphan_opener_truncates);
   RUN_TEST(test_cited_strip_no_tag_unchanged);
   RUN_TEST(test_cited_strip_null_safe);
   RUN_TEST(test_cited_normalize_space_after_open);
   RUN_TEST(test_cited_normalize_space_before_close_bracket);
   RUN_TEST(test_cited_normalize_closing_variants);
   RUN_TEST(test_cited_normalize_wellformed_unchanged);
   RUN_TEST(test_cited_normalize_non_tag_untouched);
   RUN_TEST(test_cited_normalize_null_safe);
   RUN_TEST(test_command_strip_pair_and_eot);
   RUN_TEST(test_command_strip_orphan_truncates_when_true);
   RUN_TEST(test_command_strip_orphan_left_when_false);
   RUN_TEST(test_command_strip_null_safe);
   return UNITY_END();
}
