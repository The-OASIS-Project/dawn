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
 * Unit tests for document_chunker.c
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dawn_error.h"
#include "tools/document_chunker.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* =============================================================================
 * Test Cases
 * ============================================================================= */

static void test_empty_input(void) {
   chunk_result_t result;

   int rc = document_chunk_text("", NULL, &result);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "empty string returns success");
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, result.count, "empty string produces 0 chunks");
   chunk_result_free(&result);

   rc = document_chunk_text(NULL, NULL, &result);
   TEST_ASSERT_NOT_EQUAL_MESSAGE(0, rc, "NULL input returns error");
}

static void test_single_sentence(void) {
   chunk_result_t result;
   const char *text = "This is a simple sentence.";

   int rc = document_chunk_text(text, NULL, &result);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "single sentence returns success");
   TEST_ASSERT_EQUAL_INT_MESSAGE(1, result.count, "single sentence produces 1 chunk");
   TEST_ASSERT_EQUAL_STRING_MESSAGE(text, result.chunks[0], "chunk matches input");
   chunk_result_free(&result);
}

static void test_two_paragraphs_small(void) {
   chunk_result_t result;
   /* Two short paragraphs that should merge into one chunk */
   const char *text = "First paragraph here.\n\nSecond paragraph here.";

   chunk_config_t cfg = { .target_tokens = 500, .max_tokens = 1000, .overlap_tokens = 0 };
   int rc = document_chunk_text(text, &cfg, &result);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "returns success");
   TEST_ASSERT_EQUAL_INT_MESSAGE(1, result.count, "two small paragraphs merge into 1 chunk");
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(result.chunks[0], "First paragraph"),
                                "contains first paragraph");
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(result.chunks[0], "Second paragraph"),
                                "contains second paragraph");
   chunk_result_free(&result);
}

static void test_paragraph_splitting(void) {
   chunk_result_t result;

   /* Create text with multiple paragraphs that exceed target size */
   char text[8192];
   int pos = 0;
   for (int i = 0; i < 10; i++) {
      if (i > 0) {
         pos += sprintf(text + pos, "\n\n");
      }
      /* Each paragraph ~200 chars = ~50 tokens */
      for (int j = 0; j < 5; j++) {
         pos += sprintf(text + pos, "This is sentence number %d in paragraph %d. ", j + 1, i + 1);
      }
   }

   /* target=120 tokens, so ~2 paragraphs per chunk (~53 tokens each) */
   chunk_config_t cfg = { .target_tokens = 120, .max_tokens = 500, .overlap_tokens = 0 };
   int rc = document_chunk_text(text, &cfg, &result);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "returns success");
   TEST_ASSERT_TRUE_MESSAGE(result.count > 1, "multiple paragraphs produce multiple chunks");
   TEST_ASSERT_TRUE_MESSAGE(result.count < 10, "some merging occurred");

   /* Verify no chunk is empty */
   int all_nonempty = 1;
   for (int i = 0; i < result.count; i++) {
      if (strlen(result.chunks[i]) == 0)
         all_nonempty = 0;
   }
   TEST_ASSERT_TRUE_MESSAGE(all_nonempty, "all chunks are non-empty");

   chunk_result_free(&result);
}

static void test_sentence_splitting(void) {
   chunk_result_t result;

   /* One giant paragraph that needs sentence-level splitting */
   char text[8192];
   int pos = 0;
   for (int i = 0; i < 40; i++) {
      pos += sprintf(text + pos, "This is test sentence number %d with some filler words. ", i + 1);
   }

   /* max=100 tokens (~400 chars), so the single paragraph must be split */
   chunk_config_t cfg = { .target_tokens = 50, .max_tokens = 100, .overlap_tokens = 0 };
   int rc = document_chunk_text(text, &cfg, &result);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "returns success");
   TEST_ASSERT_TRUE_MESSAGE(result.count > 1, "large paragraph split into multiple chunks");

   /* Each chunk should be under max_tokens */
   int all_under_max = 1;
   for (int i = 0; i < result.count; i++) {
      int tokens = chunk_estimate_tokens(result.chunks[i], (int)strlen(result.chunks[i]));
      if (tokens > cfg.max_tokens + 10) /* small tolerance */
         all_under_max = 0;
   }
   TEST_ASSERT_TRUE_MESSAGE(all_under_max, "all chunks under max_tokens");

   chunk_result_free(&result);
}

static void test_abbreviations(void) {
   chunk_result_t result;

   /* Text with abbreviations that shouldn't be treated as sentence boundaries */
   const char *text = "Dr. Smith met with Mrs. Jones at 3.14 Main St. "
                      "They discussed the results. The meeting was productive.";

   chunk_config_t cfg = { .target_tokens = 500, .max_tokens = 1000, .overlap_tokens = 0 };
   int rc = document_chunk_text(text, &cfg, &result);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "returns success");
   TEST_ASSERT_EQUAL_INT_MESSAGE(1, result.count, "abbreviations don't cause extra splits");
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(result.chunks[0], "Dr. Smith"), "Dr. preserved");
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(result.chunks[0], "Mrs. Jones"), "Mrs. preserved");
   chunk_result_free(&result);
}

static void test_overlap(void) {
   chunk_result_t result;

   /* Create two paragraphs that will be separate chunks */
   char text[4096];
   int pos = 0;
   /* Paragraph 1: ~150 tokens */
   for (int i = 0; i < 12; i++)
      pos += sprintf(text + pos, "Alpha sentence number %d with content. ", i + 1);
   pos += sprintf(text + pos, "\n\n");
   /* Paragraph 2: ~150 tokens */
   for (int i = 0; i < 12; i++)
      pos += sprintf(text + pos, "Beta sentence number %d with content. ", i + 1);

   /* Target 100 tokens so each paragraph becomes its own chunk, overlap 25 */
   chunk_config_t cfg = { .target_tokens = 100, .max_tokens = 500, .overlap_tokens = 25 };
   int rc = document_chunk_text(text, &cfg, &result);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "returns success");
   TEST_ASSERT_EQUAL_INT_MESSAGE(2, result.count, "two separate chunks");

   if (result.count >= 2) {
      /* Second chunk should contain some text from end of first chunk */
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(result.chunks[1], "Alpha"),
                                   "overlap: chunk 2 contains text from chunk 1");
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(result.chunks[1], "Beta"),
                                   "overlap: chunk 2 contains its own text");
   }

   chunk_result_free(&result);
}

static void test_token_estimation(void) {
   TEST_ASSERT_EQUAL_INT_MESSAGE(2, chunk_estimate_tokens("hello", 5), "5 chars = 2 tokens");
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, chunk_estimate_tokens("", 0), "0 chars = 0 tokens");
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, chunk_estimate_tokens(NULL, 0), "NULL = 0 tokens");
   TEST_ASSERT_EQUAL_INT_MESSAGE(4, chunk_estimate_tokens("abcdefghijklmnop", 16),
                                 "16 chars = 4 tokens");
}

static void test_whitespace_only(void) {
   chunk_result_t result;

   int rc = document_chunk_text("   \n\n   \n\n   ", NULL, &result);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "whitespace-only returns success");
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, result.count, "whitespace-only produces 0 chunks");
   chunk_result_free(&result);
}

static void test_single_long_word(void) {
   chunk_result_t result;

   /* A single "word" longer than max_tokens — must force-split */
   char text[2048];
   memset(text, 'x', 2000);
   text[2000] = '\0';

   chunk_config_t cfg = { .target_tokens = 50, .max_tokens = 100, .overlap_tokens = 0 };
   int rc = document_chunk_text(text, &cfg, &result);
   TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "long word returns success");
   TEST_ASSERT_TRUE_MESSAGE(result.count >= 1, "long word produces at least 1 chunk");

   /* Verify all text is captured */
   int total_len = 0;
   for (int i = 0; i < result.count; i++)
      total_len += (int)strlen(result.chunks[i]);
   TEST_ASSERT_TRUE_MESSAGE(total_len >= 1900, "most text preserved after force-split");

   chunk_result_free(&result);
}

/* =============================================================================
 * Structure-aware chunking
 * ============================================================================= */

static bool chunk_contains(const chunk_result_t *r, int i, const char *needle) {
   return i < r->count && strstr(r->chunks[i], needle) != NULL;
}

static void test_yaml_sequence_one_record_per_chunk(void) {
   const char *yaml = "- id:\n"
                      "    bioguide: A000001\n"
                      "  name:\n"
                      "    first: Alice\n"
                      "    last: Adams\n"
                      "  bio:\n"
                      "    birthday: '1975-01-01'\n"
                      "- id:\n"
                      "    bioguide: B000002\n"
                      "  name:\n"
                      "    first: Bob\n"
                      "    last: Brown\n"
                      "  bio:\n"
                      "    birthday: '1980-05-05'\n"
                      "- id:\n"
                      "    bioguide: C000003\n"
                      "  name:\n"
                      "    first: Carol\n"
                      "    last: Clark\n"
                      "  bio:\n"
                      "    birthday: '1965-09-09'\n";
   chunk_result_t r;
   TEST_ASSERT_EQUAL(SUCCESS, document_chunk_text(yaml, NULL, &r));
   TEST_ASSERT_EQUAL_MESSAGE(3, r.count, "one chunk per YAML record");
   /* Each chunk holds exactly one person's name AND birthday together. */
   TEST_ASSERT_TRUE_MESSAGE(chunk_contains(&r, 0, "Adams") && chunk_contains(&r, 0, "1975"),
                            "record 0 has name + birthday in one chunk");
   TEST_ASSERT_TRUE_MESSAGE(chunk_contains(&r, 2, "Clark") && chunk_contains(&r, 2, "1965"),
                            "record 2 has name + birthday in one chunk");
   TEST_ASSERT_FALSE_MESSAGE(chunk_contains(&r, 0, "Brown"), "record 0 does not bleed into Bob");
   chunk_result_free(&r);
}

static void test_csv_header_plus_row_per_chunk(void) {
   const char *csv = "name,year,party\n"
                     "Alice,1975,D\n"
                     "Bob,1980,R\n"
                     "Carol,1965,I\n";
   chunk_result_t r;
   TEST_ASSERT_EQUAL(SUCCESS, document_chunk_text(csv, NULL, &r));
   TEST_ASSERT_EQUAL_MESSAGE(3, r.count, "one chunk per CSV data row (header excluded as a row)");
   /* Each chunk is self-describing: carries the header + its row. */
   TEST_ASSERT_TRUE_MESSAGE(chunk_contains(&r, 0, "name,year,party") &&
                                chunk_contains(&r, 0, "Alice"),
                            "row chunk carries header + values");
   TEST_ASSERT_TRUE_MESSAGE(chunk_contains(&r, 2, "Carol") && chunk_contains(&r, 2, "1965"),
                            "last row present");
   chunk_result_free(&r);
}

static void test_prose_with_bullets_not_mis_split(void) {
   /* Two bullets is below the YAML threshold (>=3) and prose-dominated — must
    * fall through to prose chunking, NOT one-chunk-per-dash. */
   const char *prose = "Here is my short shopping list for the week.\n"
                       "- milk\n"
                       "- eggs\n";
   chunk_result_t r;
   TEST_ASSERT_EQUAL(SUCCESS, document_chunk_text(prose, NULL, &r));
   TEST_ASSERT_EQUAL_MESSAGE(1, r.count, "small prose stays a single chunk, not split per bullet");
   TEST_ASSERT_TRUE_MESSAGE(chunk_contains(&r, 0, "milk") && chunk_contains(&r, 0, "eggs"),
                            "both bullets in the same chunk");
   chunk_result_free(&r);
}

/* =============================================================================
 * Main
 * ============================================================================= */

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_empty_input);
   RUN_TEST(test_single_sentence);
   RUN_TEST(test_two_paragraphs_small);
   RUN_TEST(test_paragraph_splitting);
   RUN_TEST(test_sentence_splitting);
   RUN_TEST(test_abbreviations);
   RUN_TEST(test_overlap);
   RUN_TEST(test_token_estimation);
   RUN_TEST(test_whitespace_only);
   RUN_TEST(test_single_long_word);
   RUN_TEST(test_yaml_sequence_one_record_per_chunk);
   RUN_TEST(test_csv_header_plus_row_per_chunk);
   RUN_TEST(test_prose_with_bullets_not_mis_split);
   return UNITY_END();
}
