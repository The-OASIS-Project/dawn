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
 * Unit tests for the shared WordPiece tokenizer.  Covers single-segment
 * encoding, two-segment ([CLS] q [SEP] p [SEP]) cross-encoder encoding,
 * truncation, padding, and refcount semantics.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memory/memory_embed_tokenizer.h"
#include "unity.h"

/* Resolve vocab.txt regardless of the test's cwd (ctest invokes from build
 * dir; manual runs may use repo root).  CI doesn't ship the production
 * models/embeddings/ tree (gitignored), so tests/fixtures/embeddings/vocab.txt
 * is the tracked test-fixture copy and is the first candidate. */
static const char *vocab_path(void) {
   const char *candidates[] = { "tests/fixtures/embeddings/vocab.txt",
                                "../tests/fixtures/embeddings/vocab.txt",
                                "../../tests/fixtures/embeddings/vocab.txt",
                                "models/embeddings/vocab.txt",
                                "../models/embeddings/vocab.txt",
                                "../../models/embeddings/vocab.txt",
                                NULL };
   for (int i = 0; candidates[i]; i++) {
      FILE *fp = fopen(candidates[i], "r");
      if (fp) {
         fclose(fp);
         return candidates[i];
      }
   }
   return NULL;
}

void setUp(void) {
}

void tearDown(void) {
   /* Drain any leftover refs so the next test starts clean. */
   while (memory_embed_tokenizer_available()) {
      memory_embed_tokenizer_release();
   }
}

/* ── refcount ──────────────────────────────────────────────────────────── */

/* Reference parity: token ids produced by the reference uncased BERT WordPiece
 * tokenizer (HuggingFace `tokenizers`, BertWordPieceTokenizer(lowercase=True))
 * over the same vocab.  Covers accents, curly quotes, German/Turkish/Greek
 * /Cyrillic, CJK, Hangul, emoji, Unicode whitespace and format
 * characters, an over-long word, and a multi-piece word.  An ASCII-only
 * tokenizer turned most of these into [UNK]s and degraded their embeddings. */
typedef struct {
   const char *text;
   int n;
   int64_t ids[32];
} parity_case_t;

static const parity_case_t k_parity[] = {
   { "Hello, World!", 6, { 101, 7592, 1010, 2088, 999, 102 } },
   { "Caf\xc3\xa9 au lait costs \xe2\x82\xac"
     "3.50 \xe2\x80\x94 na\xc3\xafve r\xc3\xa9sum\xc3\xa9.",
     15,
     { 101, 7668, 8740, 21110, 2102, 5366, 1574, 2509, 1012, 2753, 1517, 15743, 13746, 1012,
       102 } },
   { "The user\xe2\x80\x99s dog \xe2\x80\x9c"
     "Biscuit\xe2\x80\x9d is a beagle.",
     16,
     { 101, 1996, 5310, 1521, 1055, 3899, 1523, 20377, 28168, 1524, 2003, 1037, 26892, 9354, 1012,
       102 } },
   { "Stra\xc3\x9f"
     "e, Gr\xc3\xb6\xc3\x9f"
     "e, \xc3\x9c"
     "berm\xc3\xa4\xc3\x9fig",
     13,
     { 101, 2358, 27807, 1010, 24665, 2080, 17499, 1010, 19169, 2863, 19310, 8004, 102 } },
   { "\xc4\xb0stanbul ve \xc3\x87"
     "a\xc4\x9flayan",
     7,
     { 101, 9960, 2310, 6187, 23296, 25868, 102 } },
   { "\xce\x9f\xce\x94\xce\x9f\xce\xa3 \xce\xba\xce\xb1\xce\xb9 "
     "\xce\xa3\xce\x9f\xce\xa6\xce\x99\xce\x91.",
     14,
     { 101, 1169, 29722, 29730, 29733, 1164, 14608, 18199, 1173, 29730, 29736, 27432, 1012, 102 } },
   { "\xd0\x9c\xd0\xbe\xd1\x81\xd0\xba\xd0\xb2\xd0\xb0 \xe2\x80\x94 "
     "\xd1\x81\xd1\x82\xd0\xbe\xd0\xbb\xd0\xb8\xd1\x86\xd0\xb0 "
     "\xd0\xa0\xd0\xbe\xd1\x81\xd1\x81\xd0\xb8\xd0\xb8.",
     22,
     { 101,   1191,  14150, 29747, 23925, 25529, 10260, 1517,  1196,  22919, 14150,
       29436, 10325, 29751, 10260, 1195,  14150, 29747, 29747, 15414, 1012,  102 } },
   { "\xe6\x88\x91\xe7\x88\xb1\xe5\x8c\x97\xe4\xba\xac\xe5\xa4\xa9\xe5\xae\x89\xe9\x97\xa8",
     9,
     { 101, 1855, 100, 1781, 1755, 1811, 1820, 100, 102 } },
   { "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4 \xed\x85\x8d\xec\x8a\xa4\xed\x8a\xb8",
     17,
     { 101, 1469, 30006, 30021, 29991, 30014, 30020, 29999, 30008, 1467, 30009, 30020, 29997, 30017,
       30003, 30017, 102 } },
   { "emoji \xf0\x9f\x8e\x89 test", 7, { 101, 7861, 29147, 2072, 100, 3231, 102 } },
   { "tab\x09new\x0aline\xc2\xa0nbsp", 8, { 101, 21628, 2047, 2240, 1050, 5910, 2361, 102 } },
   { "control\xe2\x80\x8bzero-width\xc2\xadsoft",
     9,
     { 101, 2491, 6290, 2080, 1011, 9381, 6499, 6199, 102 } },
   { "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
     "xxxxxxxx end",
     4,
     { 101, 100, 2203, 102 } },
   { "pneumonoultramicroscopicsilicovolcanoconiosis",
     19,
     { 101, 1052, 2638, 2819, 17175, 11314, 6444, 2594, 7352, 26461, 27572, 11261, 6767, 15472,
       6761, 8663, 10735, 2483, 102 } },
};

static void test_reference_parity(void) {
   const char *path = vocab_path();
   TEST_ASSERT_NOT_NULL_MESSAGE(path, "vocab.txt not found");
   TEST_ASSERT_EQUAL_INT(0, memory_embed_tokenizer_acquire(path));
   for (size_t c = 0; c < sizeof(k_parity) / sizeof(k_parity[0]); c++) {
      int64_t ids[64], mask[64], types[64];
      int n = memory_embed_tokenizer_encode(k_parity[c].text, ids, mask, types, 64);
      char msg[128];
      snprintf(msg, sizeof(msg), "case %zu: %.60s", c, k_parity[c].text);
      TEST_ASSERT_EQUAL_INT_MESSAGE(k_parity[c].n, n, msg);
      for (int i = 0; i < n; i++) {
         TEST_ASSERT_EQUAL_INT64_MESSAGE(k_parity[c].ids[i], ids[i], msg);
      }
   }
   memory_embed_tokenizer_release();
}

/* Words the vocabulary can't spell (whole-word [UNK]) and punctuation landing
 * right at the length limit must still leave [SEP] last and stay in bounds. */
static void test_unk_at_limit_keeps_sep(void) {
   const char *path = vocab_path();
   TEST_ASSERT_NOT_NULL_MESSAGE(path, "vocab.txt not found");
   TEST_ASSERT_EQUAL_INT(0, memory_embed_tokenizer_acquire(path));
   char text[512] = "";
   for (int i = 0; i < 30; i++) {
      strcat(text, "\xF0\x9F\x80\x80,\xE3\x80\x81 "); /* unknown symbol, ',', CJK punct */
   }
   for (int max_len = 4; max_len <= 16; max_len++) {
      int64_t ids[16 + 4], mask[16 + 4], types[16 + 4];
      for (int i = 0; i < 20; i++) {
         ids[i] = -7; /* sentinel: nothing past max_len may be written */
      }
      int n = memory_embed_tokenizer_encode(text, ids, mask, types, max_len);
      char msg[32];
      snprintf(msg, sizeof(msg), "max_len %d", max_len);
      TEST_ASSERT_TRUE_MESSAGE(n > 0 && n <= max_len, msg);
      TEST_ASSERT_EQUAL_INT64_MESSAGE(MEMORY_TOKENIZER_TOKEN_SEP, ids[n - 1], msg);
      for (int i = max_len; i < 20; i++) {
         TEST_ASSERT_EQUAL_INT64_MESSAGE(-7, ids[i], msg);
      }
   }
   memory_embed_tokenizer_release();
}

static void test_acquire_release_refcount(void) {
   const char *path = vocab_path();
   TEST_ASSERT_NOT_NULL_MESSAGE(path, "vocab.txt not found");

   TEST_ASSERT_EQUAL_INT(0, memory_embed_tokenizer_acquire(path));
   TEST_ASSERT_TRUE(memory_embed_tokenizer_available());

   /* Second acquire = bumped refcount; no reload. */
   TEST_ASSERT_EQUAL_INT(0, memory_embed_tokenizer_acquire(path));
   TEST_ASSERT_TRUE(memory_embed_tokenizer_available());

   memory_embed_tokenizer_release();
   TEST_ASSERT_TRUE(memory_embed_tokenizer_available());

   memory_embed_tokenizer_release();
   TEST_ASSERT_FALSE(memory_embed_tokenizer_available());
}

static void test_acquire_with_null_or_empty_path_fails(void) {
   TEST_ASSERT_NOT_EQUAL(0, memory_embed_tokenizer_acquire(NULL));
   TEST_ASSERT_NOT_EQUAL(0, memory_embed_tokenizer_acquire(""));
   TEST_ASSERT_FALSE(memory_embed_tokenizer_available());
}

/* ── single-segment encoding ────────────────────────────────────────────── */

static void test_encode_single_emits_cls_and_sep(void) {
   const char *path = vocab_path();
   TEST_ASSERT_NOT_NULL_MESSAGE(path, "vocab.txt not found");
   TEST_ASSERT_EQUAL_INT(0, memory_embed_tokenizer_acquire(path));

   const int max_len = 32;
   int64_t ids[max_len];
   int64_t mask[max_len];
   int64_t types[max_len];

   int n = memory_embed_tokenizer_encode("hello world", ids, mask, types, max_len);
   TEST_ASSERT_GREATER_THAN_INT(2, n);
   TEST_ASSERT_EQUAL_INT64(MEMORY_TOKENIZER_TOKEN_CLS, ids[0]);
   TEST_ASSERT_EQUAL_INT64(MEMORY_TOKENIZER_TOKEN_SEP, ids[n - 1]);

   /* Single-segment: every token type ID must be 0. */
   for (int i = 0; i < n; i++) {
      TEST_ASSERT_EQUAL_INT64(0, types[i]);
   }
}

/* ── two-segment (cross-encoder) encoding ───────────────────────────────── */

static void test_encode_pair_emits_two_seps_and_segment_ids(void) {
   const char *path = vocab_path();
   TEST_ASSERT_NOT_NULL_MESSAGE(path, "vocab.txt not found");
   TEST_ASSERT_EQUAL_INT(0, memory_embed_tokenizer_acquire(path));

   const int max_len = 32;
   int64_t ids[max_len];
   int64_t mask[max_len];
   int64_t types[max_len];

   int n = memory_embed_tokenizer_encode_pair("hello", "world example", ids, mask, types, max_len);
   TEST_ASSERT_GREATER_THAN_INT(3, n);

   TEST_ASSERT_EQUAL_INT64(MEMORY_TOKENIZER_TOKEN_CLS, ids[0]);
   TEST_ASSERT_EQUAL_INT64(0, types[0]);
   TEST_ASSERT_EQUAL_INT64(1, mask[0]);

   TEST_ASSERT_EQUAL_INT64(MEMORY_TOKENIZER_TOKEN_SEP, ids[n - 1]);
   TEST_ASSERT_EQUAL_INT64(1, types[n - 1]);

   int sep_count = 0;
   for (int i = 0; i < n; i++) {
      if (ids[i] == MEMORY_TOKENIZER_TOKEN_SEP)
         sep_count++;
   }
   TEST_ASSERT_EQUAL_INT(2, sep_count);

   /* Type IDs must transition 0 → 1 monotonically. */
   bool seen_one = false;
   for (int i = 0; i < n; i++) {
      if (types[i] == 1)
         seen_one = true;
      else if (seen_one)
         TEST_FAIL_MESSAGE("token_type_ids reverted from 1 to 0");
   }
   TEST_ASSERT_TRUE_MESSAGE(seen_one, "no passage tokens emitted");
}

static void test_encode_pair_pads_remainder_with_zero(void) {
   const char *path = vocab_path();
   TEST_ASSERT_NOT_NULL_MESSAGE(path, "vocab.txt not found");
   TEST_ASSERT_EQUAL_INT(0, memory_embed_tokenizer_acquire(path));

   const int max_len = 64;
   int64_t ids[max_len];
   int64_t mask[max_len];
   int64_t types[max_len];

   int n = memory_embed_tokenizer_encode_pair("a", "b", ids, mask, types, max_len);
   TEST_ASSERT_GREATER_THAN_INT(0, n);
   TEST_ASSERT_LESS_THAN_INT(max_len, n);

   for (int i = n; i < max_len; i++) {
      TEST_ASSERT_EQUAL_INT64(0, ids[i]);
      TEST_ASSERT_EQUAL_INT64(0, mask[i]);
      TEST_ASSERT_EQUAL_INT64(0, types[i]);
   }
}

static void test_encode_pair_truncates_long_passage(void) {
   const char *path = vocab_path();
   TEST_ASSERT_NOT_NULL_MESSAGE(path, "vocab.txt not found");
   TEST_ASSERT_EQUAL_INT(0, memory_embed_tokenizer_acquire(path));

   const int max_len = 16;
   int64_t ids[max_len];
   int64_t mask[max_len];
   int64_t types[max_len];

   const char *long_passage =
       "the quick brown fox jumps over the lazy dog and runs through the forest "
       "looking for a place to rest after a long journey under the bright sun";

   int n = memory_embed_tokenizer_encode_pair("query", long_passage, ids, mask, types, max_len);
   TEST_ASSERT_EQUAL_INT(max_len, n);
   /* Final emitted token must still be [SEP] even after truncation. */
   TEST_ASSERT_EQUAL_INT64(MEMORY_TOKENIZER_TOKEN_SEP, ids[max_len - 1]);
}

static void test_encode_pair_null_passage_is_safe(void) {
   const char *path = vocab_path();
   TEST_ASSERT_NOT_NULL_MESSAGE(path, "vocab.txt not found");
   TEST_ASSERT_EQUAL_INT(0, memory_embed_tokenizer_acquire(path));

   const int max_len = 16;
   int64_t ids[max_len];
   int64_t mask[max_len];
   int64_t types[max_len];

   int n = memory_embed_tokenizer_encode_pair("hello", NULL, ids, mask, types, max_len);
   TEST_ASSERT_GREATER_THAN_INT(2, n);
   TEST_ASSERT_EQUAL_INT64(MEMORY_TOKENIZER_TOKEN_CLS, ids[0]);
   TEST_ASSERT_EQUAL_INT64(MEMORY_TOKENIZER_TOKEN_SEP, ids[n - 1]);
}

static void test_encode_pair_max_len_too_small_returns_zero(void) {
   const char *path = vocab_path();
   TEST_ASSERT_NOT_NULL_MESSAGE(path, "vocab.txt not found");
   TEST_ASSERT_EQUAL_INT(0, memory_embed_tokenizer_acquire(path));

   int64_t ids[8];
   int64_t mask[8];
   int64_t types[8];
   /* max_len < 4 cannot fit [CLS] + 1 query + [SEP] + [SEP]. */
   TEST_ASSERT_EQUAL_INT(0, memory_embed_tokenizer_encode_pair("q", "p", ids, mask, types, 3));
}

int main(void) {
   UNITY_BEGIN();

   RUN_TEST(test_acquire_release_refcount);
   RUN_TEST(test_reference_parity);
   RUN_TEST(test_unk_at_limit_keeps_sep);
   RUN_TEST(test_acquire_with_null_or_empty_path_fails);

   RUN_TEST(test_encode_single_emits_cls_and_sep);

   RUN_TEST(test_encode_pair_emits_two_seps_and_segment_ids);
   RUN_TEST(test_encode_pair_pads_remainder_with_zero);
   RUN_TEST(test_encode_pair_truncates_long_passage);
   RUN_TEST(test_encode_pair_null_passage_is_safe);
   RUN_TEST(test_encode_pair_max_len_too_small_returns_zero);

   return UNITY_END();
}
