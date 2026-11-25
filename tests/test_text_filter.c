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
 * Unit tests for text_filter: the reply-tag strips and text_split_pieces.
 */

#include <stdio.h>
#include <string.h>

#include "core/text_filter.h"
#include "unity.h"
#include "webui/webui_server.h" /* WEBUI_STREAM_TEXT_SIZE */

static cited_tag_filter_state_t s_cited;

void setUp(void) {
   text_filter_cited_reset(&s_cited);
}

void tearDown(void) {
}

/* ── <cited> memory-citation stream filter ──────────────────────────────── */

/* Feed one delta; append the stripped output onto `acc`. */
static void cited_feed(char *acc, size_t acc_size, const char *delta) {
   char tmp[2048];
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

static void test_cited_non_tag_content_is_text(void) {
   /* Content outside the citation grammar ('x', a stray '<') makes it text. */
   char out[128] = "";
   cited_feed(out, sizeof(out), "a<cited>x<y</cited>b");
   cited_flush(out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("a<cited>x<y</cited>b", out);
}

static void test_cited_near_full_false_opener(void) {
   /* "<cited x>" is not the tag (space breaks the match) — must pass through. */
   char out[128] = "";
   cited_feed(out, sizeof(out), "<cited x> stays");
   cited_flush(out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("<cited x> stays", out);
}

static void test_cited_two_trailing_tags_one_stream(void) {
   char out[128] = "";
   cited_feed(out, sizeof(out), "a<cited>M1</cited><cited>M2</cited>");
   cited_flush(out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("a", out);
}

/* Run @p in through the streaming filter in chunks of @p chunk bytes (0 = whole),
 * then flush; the output lands in @p out. */
static void cited_stream(const char *in, size_t chunk, char *out, size_t out_size) {
   text_filter_cited_reset(&s_cited);
   out[0] = '\0';
   size_t n = strlen(in);
   if (chunk == 0) {
      chunk = n ? n : 1;
   }
   char piece[2048];
   for (size_t i = 0; i < n; i += chunk) {
      size_t len = (n - i < chunk) ? n - i : chunk;
      memcpy(piece, in + i, len);
      piece[len] = '\0';
      cited_feed(out, out_size, piece);
   }
   cited_flush(out, out_size);
}

/* Through the whole-string strip and the stream, whole, byte by byte and in
 * every two-chunk split: each gives @p want. */
static void expect_cited(const char *in, const char *want) {
   char buf[2048];
   snprintf(buf, sizeof(buf), "%s", in);
   text_filter_cited_strip(buf);
   TEST_ASSERT_EQUAL_STRING_MESSAGE(want, buf, "whole-string strip");

   char out[2048];
   cited_stream(in, 0, out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING_MESSAGE(want, out, "stream, one chunk");
   cited_stream(in, 1, out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING_MESSAGE(want, out, "stream, 1-byte chunks");
   cited_stream(in, 3, out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING_MESSAGE(want, out, "stream, 3-byte chunks");

   size_t n = strlen(in);
   for (size_t cut = 1; cut < n; cut++) {
      char head[2048];
      memcpy(head, in, cut);
      head[cut] = '\0';
      text_filter_cited_reset(&s_cited);
      out[0] = '\0';
      cited_feed(out, sizeof(out), head);
      cited_feed(out, sizeof(out), in + cut);
      cited_flush(out, sizeof(out));
      char msg[64];
      snprintf(msg, sizeof(msg), "stream, split at %zu", cut);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(want, out, msg);
   }
}

static void test_cited_mention_in_prose_is_kept(void) {
   const char *backticked = "DAWN already logs my `<cited>` tags, and counts them.";
   expect_cited(backticked, backticked);
   const char *bare = "The model ends with a <cited> tag listing what it used.";
   expect_cited(bare, bare);
   const char *near_end = "That is the <cited> tag.";
   expect_cited(near_end, near_end);
   const char *two = "Both <cited> and </cited> appear; <cited>x is text.";
   expect_cited(two, two);
}

static void test_cited_grammar_content_then_prose_is_kept(void) {
   /* Content that looks like citations, then a byte no tag holds: all text. */
   const char *in = "Write <cited>M1, M2 and so on.";
   expect_cited(in, in);
   const char *ids = "Like <cited>ID:12 here</cited>.";
   expect_cited(ids, ids);
}

static void test_cited_wellformed_tags_removed(void) {
   expect_cited("Done.<cited>M1,M4</cited>", "Done.");
   expect_cited("Done.<cited>ID:6432</cited>", "Done.");
   expect_cited("Done.<cited>id:6432,m3</cited>", "Done.");
   expect_cited("Done.<cited>M1, M4</cited>", "Done.");
   expect_cited("Done.<cited>ID : 6432,\tM 2\n</cited>", "Done.");
   expect_cited("Done.<cited></cited>", "Done.");
}

/* A tag counts only at the end of the text. */
static void test_cited_tag_then_more_text_is_kept(void) {
   const char *word = "a<cited>M1</cited>word";
   expect_cited(word, word);
   const char *spaced = "a<cited>M1</cited>  \n more.";
   expect_cited(spaced, spaced); /* the whitespace kept exactly too */
   const char *two = "a<cited>M1</cited>b<cited>ID:7</cited>c";
   expect_cited(two, two);
   expect_cited("See <cited> here.<cited>M2</cited>", "See <cited> here.");
}

static void test_cited_trailing_tags_removed(void) {
   expect_cited("Done.<cited>M1</cited>  \n\n", "Done.");
   expect_cited("Done.<cited>M1</cited><cited>ID:5</cited>", "Done.");
   expect_cited("Done.<cited>M1</cited> \n <cited>ID:5</cited>\n", "Done.");
   expect_cited("Done.<cited>[M1]</cited >", "Done.");
   /* Before something that only starts like an opener: text. */
   const char *near = "Done.<cited>M1</cited> <cit";
   expect_cited(near, near);
   /* A following opener decides the first tag, whatever the second turns out to be. */
   expect_cited("Done.<cited>M1</cited> <cited> is the tag.", "Done.<cited> is the tag.");
}

static void test_cited_trailing_whitespace_bound(void) {
   char in[256] = "Done.<cited>M1</cited>";
   for (int i = 0; i < CITED_TAG_TRAIL_WS_MAX; i++) {
      strcat(in, " ");
   }
   expect_cited(in, "Done.");
   strcat(in, "\n"); /* one more than a trailing tag may carry: text */
   expect_cited(in, in);
}

/* What a model wrote in the stream before its tool calls ends at the flush (the
 * message's end), so a tag ending it is dropped before the next message's text;
 * without it the tag would still be held, and shown, when that text arrives. */
static void test_cited_flush_ends_each_message(void) {
   char out[256] = "";
   cited_feed(out, sizeof(out), "Let me check.<cited>M1</cited>");
   cited_flush(out, sizeof(out));
   cited_feed(out, sizeof(out), "\n\n");
   cited_feed(out, sizeof(out), "Found it.<cited>M2</cited>");
   cited_flush(out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("Let me check.\n\nFound it.", out);

   char unflushed[256] = "";
   cited_feed(unflushed, sizeof(unflushed), "Let me check.<cited>M1</cited>\n\nFound it.");
   cited_flush(unflushed, sizeof(unflushed));
   TEST_ASSERT_EQUAL_STRING("Let me check.<cited>M1</cited>\n\nFound it.", unflushed);
}

/* The reply that quoted example tags mid-text, then went on. */
static const char k_example_reply[] =
    "The tag in backticks: `<cited>M1,M4</cited>` and bare mid-sentence: <cited>M1,M4</cited>"
    "\n\nIf you can read this line, the reply made it past both tags.";

static void test_cited_examples_mid_reply_kept(void) {
   expect_cited(k_example_reply, k_example_reply);
   char with_tag[512];
   snprintf(with_tag, sizeof(with_tag), "%s<cited>M2</cited>", k_example_reply);
   expect_cited(with_tag, k_example_reply);
}

static void test_cited_truncated_partial_tag_dropped(void) {
   expect_cited("The answer.<cited>M1,M", "The answer.");
   expect_cited("The answer.<cited>", "The answer.");
   expect_cited("The answer.<cited>M1</cit", "The answer.");
   expect_cited("The answer.<cited>ID: 64", "The answer.");
}

/* "x<cited>" + @p run digits + @p tail into @p buf. */
static void cited_run(char *buf, size_t size, size_t run, const char *tail) {
   snprintf(buf, size, "x" CITED_TAG_OPEN);
   size_t n = strlen(buf);
   for (size_t i = 0; i < run && n + 1 < size; i++) {
      buf[n++] = '1';
   }
   buf[n] = '\0';
   strncat(buf, tail, size - n - 1);
}

static void test_cited_run_at_and_past_the_hold(void) {
   char in[1024];
   /* At the hold, then prose: text, every byte kept. */
   cited_run(in, sizeof(in), CITED_TAG_HOLD_MAX, "; on.");
   expect_cited(in, in);
   /* One past it, then prose: the opener and the run go, the prose stays. */
   cited_run(in, sizeof(in), CITED_TAG_HOLD_MAX + 1, "; on.");
   expect_cited(in, "x; on.");
   /* Ended by a '<' that isn't the closer: text goes on from that '<'. */
   cited_run(in, sizeof(in), CITED_TAG_HOLD_MAX + 1, "</cix<cited>M1</cited>.");
   expect_cited(in, "x</cix<cited>M1</cited>.");
   cited_run(in, sizeof(in), CITED_TAG_HOLD_MAX + 1, "</cix<cited>M1</cited>");
   expect_cited(in, "x</cix");
   /* Ended by the closer: at the hold a tag mid-text is text, past it a tag. */
   cited_run(in, sizeof(in), CITED_TAG_HOLD_MAX, "</cited>y");
   expect_cited(in, in);
   cited_run(in, sizeof(in), CITED_TAG_HOLD_MAX + 1, "</cited>y");
   expect_cited(in, "xy");
   cited_run(in, sizeof(in), CITED_TAG_HOLD_MAX, "</cited>\n");
   expect_cited(in, "x");
   /* Ended by the end of the text, at and past the hold: cut off, dropped. */
   cited_run(in, sizeof(in), CITED_TAG_HOLD_MAX, "");
   expect_cited(in, "x");
   cited_run(in, sizeof(in), CITED_TAG_HOLD_MAX + 1, "</cit");
   expect_cited(in, "x");
}

/* A tag citing every tool fact a turn can surface is a tag, however long. */
static void test_cited_long_tag_of_ids_removed(void) {
   char in[2048] = "Done." CITED_TAG_OPEN;
   for (int i = 0; i < 96; i++) {
      char tok[16];
      snprintf(tok, sizeof(tok), "%sID:%d", i ? "," : "", 12000 + i);
      strcat(in, tok);
   }
   strcat(in, CITED_TAG_CLOSE);
   TEST_ASSERT_TRUE(strlen(in) > CITED_TAG_HOLD_MAX * 2);
   expect_cited(in, "Done.");

   size_t content = 0;
   size_t tag = 0;
   const char *t = text_filter_cited_find(in, &content, &tag);
   TEST_ASSERT_EQUAL_PTR(in + 5, t);
   TEST_ASSERT_EQUAL_UINT(strlen(in) - 5 - CITED_TAG_OPEN_LEN - CITED_TAG_CLOSE_LEN, content);
   TEST_ASSERT_EQUAL_UINT(strlen(in) - 5, tag);
}

static void test_cited_bracketed_handles_removed(void) {
   expect_cited("Done.<cited>[M1],[M4]</cited>", "Done.");
   expect_cited("Done.<cited>[ID:6432]</cited>", "Done.");
}

static void test_cited_padded_closer_removed(void) {
   expect_cited("Done.<cited>M1</cited >", "Done.");
   expect_cited("Done.<cited>M1</ cited>", "Done.");
   expect_cited("Done.<cited>M1< /cited>", "Done.");
   expect_cited("Done.<cited>M1< / cited\t>", "Done.");
   /* More padding than a slip carries: text. */
   expect_cited("A <cited>M1</cited     > b", "A <cited>M1</cited     > b");
}

/* The whole-string strip and the stream agree on any mix of tag pieces, prose
 * and runs either side of the hold (a fixed-seed generator). */
static void test_cited_strip_and_stream_agree(void) {
   static const char *const parts[] = { CITED_TAG_OPEN,
                                        CITED_TAG_CLOSE,
                                        "<",
                                        "</ci",
                                        "M1",
                                        ",",
                                        " ",
                                        "\n",
                                        "[",
                                        "]",
                                        "ID:7",
                                        "x",
                                        "ab.",
                                        "< /cited >",
                                        "<cited>M1</cited>",
                                        "<cited>M2</cited> ",
                                        "  \n",
                                        "<cit" };
   const size_t n_parts = sizeof(parts) / sizeof(parts[0]);
   unsigned int seed = 12345;
   for (int round = 0; round < 3000; round++) {
      /* Room for every piece being a run: 12 * (CITED_TAG_HOLD_MAX + 1) bytes. */
      char in[4096] = "";
      int pieces = 1 + (int)(seed % 12);
      for (int i = 0; i < pieces; i++) {
         seed = seed * 1103515245u + 12345u;
         if ((seed >> 16) % 9 == 0) {
            size_t run = CITED_TAG_HOLD_MAX - 1 + (seed >> 8) % 3;
            for (size_t k = 0; k < run && strlen(in) + 2 < sizeof(in) - 16; k++) {
               strcat(in, "1");
            }
         } else {
            strcat(in, parts[(seed >> 16) % n_parts]);
         }
      }
      char whole[sizeof(in)];
      snprintf(whole, sizeof(whole), "%s", in);
      text_filter_cited_strip(whole);
      char out[sizeof(in)];
      cited_stream(in, 1, out, sizeof(out));
      TEST_ASSERT_EQUAL_STRING_MESSAGE(whole, out, in);
      cited_stream(in, 7, out, sizeof(out));
      TEST_ASSERT_EQUAL_STRING_MESSAGE(whole, out, in);
   }
}

static void test_cited_flush_held_bytes(void) {
   char out[2048];
   /* A partial opener at the end is text. */
   cited_stream("ends <cite", 0, out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("ends <cite", out);
   /* A whole opener with tag content and no closer: a cut-off tag, dropped. */
   cited_stream("ends <cited>M1", 0, out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("ends ", out);
   /* Held bytes that proved to be text came out before the flush, once. */
   cited_stream("ends <cited>M1.", 0, out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("ends <cited>M1.", out);
   /* A whole tag held, alone or with whitespace after it: the end, dropped. */
   cited_stream("ends <cited>M1</cited>", 0, out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("ends ", out);
   cited_stream("ends <cited>M1</cited> \n\t", 0, out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("ends ", out);
   /* A whole tag held with a partial opener after it: text, emitted. */
   cited_stream("ends <cited>M1</cited> <ci", 0, out, sizeof(out));
   TEST_ASSERT_EQUAL_STRING("ends <cited>M1</cited> <ci", out);
}

static void test_cited_find_reports_spans(void) {
   size_t content = 99;
   size_t tag = 99;
   const char *text = "a <cited> b <cited>M1,M2</cited> ";
   const char *t = text_filter_cited_find(text, &content, &tag);
   TEST_ASSERT_EQUAL_PTR(text + 12, t);
   TEST_ASSERT_EQUAL_UINT(5, content);
   TEST_ASSERT_EQUAL_UINT(CITED_TAG_OPEN_LEN + 5 + CITED_TAG_CLOSE_LEN + 1, tag);
   TEST_ASSERT_NULL(text_filter_cited_find("a <cited>M1</cited> c", &content, &tag));
   t = text_filter_cited_find("a<cited>M1</cited><cited>M2</cited>", &content, &tag);
   TEST_ASSERT_NOT_NULL(t);
   TEST_ASSERT_EQUAL_UINT(CITED_TAG_OPEN_LEN + 2 + CITED_TAG_CLOSE_LEN, tag);
   t = text_filter_cited_find("x<cited>M3", &content, &tag);
   TEST_ASSERT_NOT_NULL(t);
   TEST_ASSERT_EQUAL_UINT(2, content);
   TEST_ASSERT_EQUAL_UINT(0, tag);
   TEST_ASSERT_NULL(text_filter_cited_find("only a `<cited>` mention", &content, &tag));
   TEST_ASSERT_NULL(text_filter_cited_find(NULL, &content, &tag));
}

/* ── whole-string cited strip (text_filter_cited_strip) ──────────────────── */

static void test_cited_strip_trailing_tag(void) {
   /* The common case: a trailing citation tag on an assembled response/sentence. */
   char s[128] = "The answer is 42. <cited>M1,M4</cited>";
   text_filter_cited_strip(s);
   TEST_ASSERT_EQUAL_STRING("The answer is 42. ", s);
}

static void test_cited_strip_tags_mid_text_kept(void) {
   char s[128] = "a<cited>M1</cited>b<cited>M2</cited>c";
   text_filter_cited_strip(s);
   TEST_ASSERT_EQUAL_STRING("a<cited>M1</cited>b<cited>M2</cited>c", s);
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
   char s[128] = "a< cited >M1< / cited >";
   text_filter_cited_normalize(s);
   TEST_ASSERT_EQUAL_STRING("a<cited>M1</cited>", s);
   text_filter_cited_strip(s);
   TEST_ASSERT_EQUAL_STRING("a", s);
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

/* ── end-of-turn cut (text_filter_end_of_turn_strip) ───────────────────── */

static void test_end_of_turn_strip_truncates(void) {
   char s[128] = "hi there<end_of_turn>trailing";
   text_filter_end_of_turn_strip(s);
   TEST_ASSERT_EQUAL_STRING("hi there", s);
}

static void test_end_of_turn_strip_no_marker_unchanged(void) {
   char s[128] = "hi there";
   text_filter_end_of_turn_strip(s);
   TEST_ASSERT_EQUAL_STRING("hi there", s);
}

static void test_end_of_turn_strip_null_safe(void) {
   text_filter_end_of_turn_strip(NULL); /* must not crash */
}

static void test_end_of_turn_strip_at_start_empties(void) {
   char s[128] = "<end_of_turn>trailing";
   text_filter_end_of_turn_strip(s);
   TEST_ASSERT_EQUAL_STRING("", s);
}

static void test_end_of_turn_strip_partial_tag_kept(void) {
   char s[128] = "hi there<end_of";
   text_filter_end_of_turn_strip(s);
   TEST_ASSERT_EQUAL_STRING("hi there<end_of", s);
}

/* ── text_filter_emote_strip ────────────────────────────────────────────── */

static void expect_emote(const char *in, const char *want_text, const char *want_name) {
   char buf[256];
   char name[EMOTE_NAME_MAX];
   snprintf(buf, sizeof(buf), "%s", in);
   bool found = text_filter_emote_strip(buf, name, sizeof(name));
   TEST_ASSERT_EQUAL_STRING(want_text, buf);
   TEST_ASSERT_EQUAL_STRING(want_name, name);
   TEST_ASSERT_EQUAL(want_name[0] != '\0', found);
}

static void test_emote_strip_leading_tag(void) {
   expect_emote("[emote:curious] What's that?", "What's that?", "curious");
}

static void test_emote_strip_spaced_and_cased(void) {
   expect_emote("[emote: Happy ] Great.", "Great.", "happy");
}

static void test_emote_strip_first_known_name_wins(void) {
   expect_emote("[emote:sad] Oh. [emote:calm] Fine.", "Oh. Fine.", "sad");
}

static void test_emote_strip_unknown_name_removed_not_reported(void) {
   expect_emote("[emote:smug] Sure.", "Sure.", "");
   expect_emote("[emote:smug] Sure. [emote:nod] Right.", "Sure. Right.", "nod");
   /* A name from before the vocabulary changed is removed, not reported. */
   expect_emote("[emote:apologetic] Sorry.", "Sorry.", "");
}

static void test_emote_strip_acts(void) {
   expect_emote("[emote:no] I can't do that.", "I can't do that.", "no");
   expect_emote("[emote:done] All set.", "All set.", "done");
   expect_emote("[emote:Unsure] Hard to say.", "Hard to say.", "unsure");
}

static void test_emote_strip_tag_only(void) {
   expect_emote("[emote:excited]", "", "excited");
}

static void test_emote_strip_malformed_removed_unnamed(void) {
   expect_emote("Type [emote:] to see.", "Type to see.", "");
   expect_emote("[emote:two words] x", "x", "");
   expect_emote("[emote: happy, curious] Hi.", "Hi.", "");
   expect_emote("[emote:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa] x", "x", "");
}

static void test_emote_strip_opener_any_case(void) {
   expect_emote("[Emote:Happy] Hi.", "Hi.", "happy");
   expect_emote("[EMOTE:calm] Ok.", "Ok.", "calm");
}

static void test_emote_strip_unclosed_left_as_text(void) {
   expect_emote("[emote:curious", "[emote:curious", "");
   expect_emote("[emote: happy\nmore] x", "[emote: happy\nmore] x", "");
   expect_emote("[emote:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa] x",
                "[emote:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa] x", "");
}

static void test_emote_strip_utf8_neighbours_untouched(void) {
   expect_emote("[emote:surprised] Café — naïve.", "Café — naïve.", "surprised");
}

static void test_emote_strip_mid_text_keeps_words_apart(void) {
   expect_emote("Sure,[emote:happy] glad to.", "Sure, glad to.", "happy");
   expect_emote("Sure, [emote:happy] glad to.", "Sure, glad to.", "happy");
   expect_emote("Done.\n[emote:done] Next.", "Done.\nNext.", "done");
}

static void test_emote_strip_null_and_no_name_out(void) {
   TEST_ASSERT_FALSE(text_filter_emote_strip(NULL, NULL, 0));
   char buf[] = "[emote:calm] Ok.";
   TEST_ASSERT_TRUE(text_filter_emote_strip(buf, NULL, 0));
   TEST_ASSERT_EQUAL_STRING("Ok.", buf);
}

/* ── main ───────────────────────────────────────────────────────────────── */

/* ── text_split_pieces ──────────────────────────────────────────────────── */

/* Append @p add to the NUL-terminated @p dst of @p size bytes, cut to fit. */
static void append(char *dst, size_t size, const char *add) {
   size_t have = strlen(dst);
   size_t n = strlen(add);
   if (n > size - have - 1) {
      n = size - have - 1;
   }
   memcpy(dst + have, add, n);
   dst[have + n] = '\0';
}

typedef struct {
   char out[2048];
   size_t max_seen;
   int calls;
   int broken; /* pieces that start or end inside a character */
   const char *first_ptr;
} pieces_t;

/* True when @p s starts on a character and ends with a complete one. */
static bool whole_characters(const char *s) {
   const unsigned char *p = (const unsigned char *)s;
   while (*p) {
      int len;
      if (*p < 0x80) {
         len = 1;
      } else if ((*p & 0xE0) == 0xC0) {
         len = 2;
      } else if ((*p & 0xF0) == 0xE0) {
         len = 3;
      } else if ((*p & 0xF8) == 0xF0) {
         len = 4;
      } else {
         return false; /* a continuation byte (0x80..0xBF) where a character starts */
      }
      for (int i = 1; i < len; i++) {
         if ((p[i] & 0xC0) != 0x80) {
            return false; /* the character is cut short */
         }
      }
      p += len;
   }
   return true;
}

static void collect_piece(const char *piece, void *ctx) {
   pieces_t *p = (pieces_t *)ctx;
   size_t n = strlen(piece);
   if (!whole_characters(piece)) {
      p->broken++;
   }
   if (n > p->max_seen) {
      p->max_seen = n;
   }
   if (p->calls++ == 0) {
      p->first_ptr = piece;
   }
   append(p->out, sizeof(p->out), piece);
}

static void test_split_pieces_short_passes_through(void) {
   pieces_t p = { 0 };
   const char *text = "Hello world";
   text_split_pieces(text, 64, collect_piece, &p);
   TEST_ASSERT_EQUAL_INT(1, p.calls);
   TEST_ASSERT_EQUAL_PTR(text, p.first_ptr);
   TEST_ASSERT_EQUAL_STRING(text, p.out);

   pieces_t none = { 0 };
   text_split_pieces("", 64, collect_piece, &none);
   text_split_pieces(NULL, 64, collect_piece, &none);
   TEST_ASSERT_EQUAL_INT(0, none.calls);
}

/* Each piece ends on a character: "é" (2 bytes) and "€" (3 bytes) cross every
 * cut point of a 7-byte limit somewhere in the run. */
static void test_split_pieces_never_splits_a_character(void) {
   char text[512] = "";
   for (int i = 0; i < 40; i++) {
      strcat(text, "a\xc3\xa9\xe2\x82\xac"); /* aé€ */
   }
   pieces_t p = { 0 };
   text_split_pieces(text, 7, collect_piece, &p);
   TEST_ASSERT_EQUAL_STRING(text, p.out);
   TEST_ASSERT_TRUE(p.max_seen <= 7);
   TEST_ASSERT_TRUE(p.calls > 1);
   TEST_ASSERT_EQUAL_INT(0, p.broken);
}

/* A stream delta's filter buffer and piece (webui_send_stream_delta): one
 * filtered piece is one frame. */
#define FILTER_BUF WEBUI_STREAM_TEXT_SIZE
#define PIECE_MAX (FILTER_BUF - CITED_TAG_BUF_SIZE)

typedef struct {
   char out[2048];
   int calls;
} filtered_t;

static void filter_piece(const char *piece, void *ctx) {
   filtered_t *f = (filtered_t *)ctx;
   char filtered[FILTER_BUF];
   text_filter_cited_tags_to_buffer(&s_cited, piece, filtered, sizeof(filtered));
   append(f->out, sizeof(f->out), filtered);
   f->calls++;
}

/* A chunk longer than the filter buffer, a tag across the piece boundary:
 * filtered piece by piece (as a stream delta is) nothing is lost and the tag
 * is still removed, where filtering it whole into the buffer cuts it short. */
static void test_split_pieces_chunk_longer_than_filter_buffer(void) {
   static const char tag[] = "<cited>M1,M7</cited>";
   const size_t before = 2 * PIECE_MAX - 4; /* "<cit" ends the second piece */
   char text[2048] = "";
   char expected[2048] = "";
   size_t n = 0;
   while (n < before) {
      /* "é" (2 bytes) along the way, ASCII where only one byte is left */
      const char *add = (n % 10 == 8 && before - n >= 2) ? "\xc3\xa9" : "x";
      strcat(text, add);
      n += strlen(add);
   }
   strcat(expected, text); /* longer than the buffer: filtered whole, it is cut */
   strcat(text, tag);      /* the reply's trailing tag */
   TEST_ASSERT_EQUAL_INT(0, strncmp(text + before, tag, sizeof(tag) - 1));
   TEST_ASSERT_TRUE(before < 2 * PIECE_MAX && 2 * PIECE_MAX < before + sizeof(tag) - 1);

   char whole[FILTER_BUF];
   text_filter_cited_tags_to_buffer(&s_cited, text, whole, sizeof(whole));
   TEST_ASSERT_TRUE(strlen(whole) < strlen(expected));

   text_filter_cited_reset(&s_cited);
   filtered_t f = { 0 };
   text_split_pieces(text, PIECE_MAX, filter_piece, &f);
   cited_flush(f.out, sizeof(f.out)); /* the message's end decides the held tag */
   TEST_ASSERT_EQUAL_INT(3, f.calls);
   TEST_ASSERT_EQUAL_STRING(expected, f.out);
   TEST_ASSERT_NULL(strstr(f.out, "M1"));
}

int main(void) {
   UNITY_BEGIN();
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
   RUN_TEST(test_cited_non_tag_content_is_text);
   RUN_TEST(test_cited_near_full_false_opener);
   RUN_TEST(test_cited_two_trailing_tags_one_stream);
   RUN_TEST(test_cited_mention_in_prose_is_kept);
   RUN_TEST(test_cited_grammar_content_then_prose_is_kept);
   RUN_TEST(test_cited_wellformed_tags_removed);
   RUN_TEST(test_cited_tag_then_more_text_is_kept);
   RUN_TEST(test_cited_trailing_tags_removed);
   RUN_TEST(test_cited_trailing_whitespace_bound);
   RUN_TEST(test_cited_flush_ends_each_message);
   RUN_TEST(test_cited_examples_mid_reply_kept);
   RUN_TEST(test_cited_truncated_partial_tag_dropped);
   RUN_TEST(test_cited_run_at_and_past_the_hold);
   RUN_TEST(test_cited_long_tag_of_ids_removed);
   RUN_TEST(test_cited_bracketed_handles_removed);
   RUN_TEST(test_cited_padded_closer_removed);
   RUN_TEST(test_cited_strip_and_stream_agree);
   RUN_TEST(test_cited_flush_held_bytes);
   RUN_TEST(test_cited_find_reports_spans);
   RUN_TEST(test_cited_strip_trailing_tag);
   RUN_TEST(test_cited_strip_tags_mid_text_kept);
   RUN_TEST(test_cited_strip_orphan_opener_truncates);
   RUN_TEST(test_cited_strip_no_tag_unchanged);
   RUN_TEST(test_cited_strip_null_safe);
   RUN_TEST(test_cited_normalize_space_after_open);
   RUN_TEST(test_cited_normalize_space_before_close_bracket);
   RUN_TEST(test_cited_normalize_closing_variants);
   RUN_TEST(test_cited_normalize_wellformed_unchanged);
   RUN_TEST(test_cited_normalize_non_tag_untouched);
   RUN_TEST(test_cited_normalize_null_safe);
   RUN_TEST(test_end_of_turn_strip_truncates);
   RUN_TEST(test_end_of_turn_strip_no_marker_unchanged);
   RUN_TEST(test_end_of_turn_strip_null_safe);
   RUN_TEST(test_end_of_turn_strip_at_start_empties);
   RUN_TEST(test_end_of_turn_strip_partial_tag_kept);
   RUN_TEST(test_emote_strip_leading_tag);
   RUN_TEST(test_emote_strip_spaced_and_cased);
   RUN_TEST(test_emote_strip_first_known_name_wins);
   RUN_TEST(test_emote_strip_unknown_name_removed_not_reported);
   RUN_TEST(test_emote_strip_acts);
   RUN_TEST(test_emote_strip_tag_only);
   RUN_TEST(test_emote_strip_malformed_removed_unnamed);
   RUN_TEST(test_emote_strip_opener_any_case);
   RUN_TEST(test_emote_strip_unclosed_left_as_text);
   RUN_TEST(test_emote_strip_utf8_neighbours_untouched);
   RUN_TEST(test_emote_strip_mid_text_keeps_words_apart);
   RUN_TEST(test_emote_strip_null_and_no_name_out);
   RUN_TEST(test_split_pieces_short_passes_through);
   RUN_TEST(test_split_pieces_never_splits_a_character);
   RUN_TEST(test_split_pieces_chunk_longer_than_filter_buffer);
   return UNITY_END();
}
