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
 * Unit tests for src/tools/document_focus_adapter.c +
 * src/tools/calendar_focus_adapter.c — Phase 1d adapters wired into
 * the focus-source framework.  Cross-user isolation, ranker shape,
 * empty-result behavior, partial-failure cleanup, multi-account cap,
 * NULL-query safety, item_id opacity, calendar event-time recency,
 * and the network-call invariant are all exercised here.
 *
 * Email adapter is DEFERRED to v2 — see docs/DYNAMIC_CONTEXT_INJECTION_DESIGN.md
 * §"API Audit (Phase 1d)" for the missing-cache rationale.  No tests
 * cover an email path because there is no email path to cover.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/focus/focus_candidate_helpers.h" /* FOCUS_TEXT_MAX_BYTES */
#include "core/focus/focus_source.h"
#include "core/focus/focus_source_internal.h"
#include "dawn_error.h"
#include "test_external_focus_adapters_mocks.h"
#include "tools/document_embed_cache.h"
#include "tools/document_rank.h"
#include "tools/external_focus_adapters.h"
#include "unity.h"

/* =====================================================================
 * Fixture / test setup
 * ===================================================================== */

static void config_defaults_for_test(void) {
   /* Open ranker so the assertions on candidate counts aren't masked by
    * min_score / token-budget trimming.  Per-source weights all 1.0. */
   memset(&g_config, 0, sizeof(g_config));
   g_config.memory.focus_injection.enabled = true;
   g_config.memory.focus_injection.focus_budget_bytes = 16384;
   g_config.memory.focus_injection.top_k = 32;
   g_config.memory.focus_injection.min_score = 0.0f;
   g_config.memory.focus_injection.weight_semantic = 1.0f;
   g_config.memory.focus_injection.weight_recency = 0.0f;
   g_config.memory.focus_injection.weight_importance = 0.0f;
   g_config.memory.focus_injection.weight_source = 0.0f;
}

void setUp(void) {
   focus_unregister_all();
   ext_mock_reset();
   config_defaults_for_test();
}

void tearDown(void) {
}

/* Identity-pointed unit-norm embeddings shared across tests. */
static const float embed_v1[EXT_MOCK_DIMS] = { 1.0f, 0.0f, 0.0f, 0.0f };
static const float embed_v2[EXT_MOCK_DIMS] = { 0.0f, 1.0f, 0.0f, 0.0f };
static const float embed_q[EXT_MOCK_DIMS] = { 1.0f, 0.0f, 0.0f, 0.0f };

/* =====================================================================
 * Fixture builders
 * ===================================================================== */

static void seed_chunk(int idx,
                       int64_t id,
                       int user_id,
                       const char *text,
                       const char *filename,
                       const float *embedding,
                       time_t created_at) {
   document_chunk_t *c = &s_ext_mock.chunks[idx];
   memset(c, 0, sizeof(*c));
   c->id = id;
   c->chunk_index = idx;
   strncpy(c->text, text, sizeof(c->text) - 1);
   strncpy(c->doc_filename, filename, sizeof(c->doc_filename) - 1);
   strncpy(c->doc_filetype, "txt", sizeof(c->doc_filetype) - 1);
   c->document_id = id; /* good enough for tests */
   c->embedding_norm = 1.0f;
   c->created_at = created_at;
   s_ext_mock.chunk_user_id[idx] = user_id;
   s_ext_mock.chunk_embeddings[idx] = embedding;
}

static void seed_account(int idx, int64_t id, int user_id, const char *name) {
   calendar_account_t *a = &s_ext_mock.accounts[idx];
   memset(a, 0, sizeof(*a));
   a->id = id;
   a->user_id = user_id;
   strncpy(a->name, name, sizeof(a->name) - 1);
   a->enabled = true;
}

static void seed_calendar(int idx, int64_t id, int64_t account_id, const char *name, bool active) {
   calendar_calendar_t *c = &s_ext_mock.calendars[idx];
   memset(c, 0, sizeof(*c));
   c->id = id;
   c->account_id = account_id;
   strncpy(c->display_name, name, sizeof(c->display_name) - 1);
   c->is_active = active;
}

static void seed_occurrence(int idx,
                            int64_t id,
                            int64_t calendar_id,
                            const char *summary,
                            time_t dtstart,
                            const char *event_uid) {
   calendar_occurrence_t *o = &s_ext_mock.occurrences[idx];
   memset(o, 0, sizeof(*o));
   o->id = id;
   /* Real schema joins occurrence -> event -> calendar; tests skip
    * the event indirection and stash calendar_id in the parallel array. */
   s_ext_mock.occurrence_calendar_id[idx] = calendar_id;
   strncpy(o->summary, summary, sizeof(o->summary) - 1);
   if (event_uid != NULL)
      strncpy(o->event_uid, event_uid, sizeof(o->event_uid) - 1);
   o->dtstart = dtstart;
   o->dtend = dtstart + 3600;
   o->event_id = id; /* one event per occurrence unless a test shares them */
   o->is_cancelled = false;
}

/* Convenience: build a "single user, single account, single
 * always-active calendar" baseline so individual tests focus on what
 * varies.  Returns the calendar_id the seeded occurrences should
 * attach to. */
static int64_t seed_basic_user_calendar(int user_id) {
   seed_account(s_ext_mock.account_count++, 1, user_id, "primary");
   seed_calendar(s_ext_mock.calendar_count++, 100, 1, "personal", true);
   return 100;
}

/* =====================================================================
 * 1-2.  Cross-user isolation — load-bearing security gates
 * ===================================================================== */

static void test_document_cross_user(void) {
   seed_chunk(0, 100, /*user*/ 1, "user1 secret chunk", "u1.txt", embed_v1, 1700000000);
   seed_chunk(1, 101, /*user*/ 2, "user2 chunk text", "u2.txt", embed_v1, 1700000000);
   s_ext_mock.chunk_count = 2;
   s_ext_mock.chunk_dim = EXT_MOCK_DIMS;
   s_ext_mock.embeddings_available = true;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(/*user_id*/ 2, false, "anything", embed_q,
                                                EXT_MOCK_DIMS, 1700000200, 5, &result));
   for (int i = 0; i < result.candidate_count; i++) {
      if (strcmp(result.candidates[i].source_id, "document_chunk") == 0) {
         TEST_ASSERT_TRUE_MESSAGE(strstr(result.candidates[i].text, "user2") != NULL,
                                  "document adapter must NOT surface other users' chunks");
         TEST_ASSERT_NULL_MESSAGE(strstr(result.candidates[i].text, "secret"),
                                  "user1 secret content must not appear in user2 result");
      }
   }
   focus_result_free(&result);
}

static void test_calendar_cross_user(void) {
   seed_account(0, 1, /*user*/ 1, "u1");
   seed_account(1, 2, /*user*/ 2, "u2");
   s_ext_mock.account_count = 2;
   seed_calendar(0, 100, 1, "u1cal", true);
   seed_calendar(1, 200, 2, "u2cal", true);
   s_ext_mock.calendar_count = 2;
   const time_t base = 1700000000;
   seed_occurrence(0, 1000, /*cal*/ 100, "user1 dentist", base + 3600, "uid-u1");
   seed_occurrence(1, 1001, /*cal*/ 200, "user2 standup", base + 3600, "uid-u2");
   s_ext_mock.occurrence_count = 2;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         focus_compose(/*user_id*/ 2, false, NULL, NULL, 0, base, 5, &result));
   for (int i = 0; i < result.candidate_count; i++) {
      if (strcmp(result.candidates[i].source_id, "calendar_event") == 0) {
         TEST_ASSERT_NULL_MESSAGE(strstr(result.candidates[i].text, "user1 dentist"),
                                  "calendar adapter must NOT surface other users' events");
         TEST_ASSERT_TRUE(strstr(result.candidates[i].text, "user2 standup") != NULL);
      }
   }
   focus_result_free(&result);
}

/* =====================================================================
 * 3-5.  Document adapter happy paths
 * ===================================================================== */

static void test_document_adapter_shape(void) {
   seed_chunk(0, 42, 1, "Annual report draft 3", "report.pdf", embed_v1, 1700000000);
   s_ext_mock.chunk_count = 1;
   s_ext_mock.chunk_dim = EXT_MOCK_DIMS;
   s_ext_mock.embeddings_available = true;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "annual", embed_q, EXT_MOCK_DIMS,
                                                1700000200, 5, &result));
   const focus_candidate_t *fc = NULL;
   for (int i = 0; i < result.candidate_count; i++)
      if (strcmp(result.candidates[i].source_id, "document_chunk") == 0)
         fc = &result.candidates[i];
   TEST_ASSERT_NOT_NULL(fc);
   TEST_ASSERT_TRUE_MESSAGE(strstr(fc->text, "[report.pdf]") != NULL,
                            "filename must be rendered as [<filename>] prefix");
   TEST_ASSERT_TRUE_MESSAGE(strstr(fc->text, "Annual report draft 3") != NULL,
                            "chunk text must follow the filename prefix");
   TEST_ASSERT_EQUAL_STRING("document_chunk:42", fc->item_id);
   TEST_ASSERT_TRUE(fc->semantic_score > 0.99f); /* identity vectors */
   TEST_ASSERT_EQUAL_INT64(0, fc->provenance.conv_id);
   focus_result_free(&result);
}

/* Without a query embedding only the keyword channel runs; with no keyword hit
 * nothing is returned. */
static void test_document_no_embedding_without_keyword_hits(void) {
   seed_chunk(0, 1, 1, "anything", "f.txt", embed_v1, 1700000000);
   s_ext_mock.chunk_count = 1;
   s_ext_mock.chunk_dim = EXT_MOCK_DIMS;
   s_ext_mock.embeddings_available = true;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "anything", /*qembed*/ NULL, 0,
                                                1700000000, 5, &result));
   TEST_ASSERT_EQUAL_INT(1, s_ext_mock.call_count_bm25);
   TEST_ASSERT_EQUAL_INT(0, s_ext_mock.call_count_chunk_search_load);
   for (int i = 0; i < result.candidate_count; i++)
      TEST_ASSERT_NOT_EQUAL(0, strcmp(result.candidates[i].source_id, "document_chunk"));
   focus_result_free(&result);
}

static void test_document_cap_honoring(void) {
   for (int i = 0; i < 10; i++) {
      char text[32];
      char fname[32];
      snprintf(text, sizeof(text), "chunk %d", i);
      snprintf(fname, sizeof(fname), "f%d.txt", i);
      seed_chunk(i, 100 + i, 1, text, fname, embed_v1, 1700000000);
   }
   s_ext_mock.chunk_count = 10;
   s_ext_mock.chunk_dim = EXT_MOCK_DIMS;
   s_ext_mock.embeddings_available = true;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "x", embed_q, EXT_MOCK_DIMS, 1700000000,
                                                /*per_source_max=*/3, &result));
   int doc_count = 0;
   for (int i = 0; i < result.candidate_count; i++)
      if (strcmp(result.candidates[i].source_id, "document_chunk") == 0)
         doc_count++;
   TEST_ASSERT_EQUAL_INT(3, doc_count);
   focus_result_free(&result);
}

/* Relevance gate: with a corpus large enough for a baseline, only chunks that
 * stand clearly above the corpus-typical similarity are injected. */
static const float embed_baseline[EXT_MOCK_DIMS] = { 0.5f, 0.8660254f, 0.0f, 0.0f }; /* cos 0.5 */

static void test_document_relevance_gate(void) {
   seed_chunk(0, 500, 1, "the relevant passage", "answer.txt", embed_q, 1700000000);
   for (int i = 1; i < EXT_MOCK_MAX_CHUNKS; i++) {
      char text[32];
      snprintf(text, sizeof(text), "typical chunk %d", i);
      seed_chunk(i, 500 + i, 1, text, "other.txt", embed_baseline, 1700000000);
   }
   s_ext_mock.chunk_count = EXT_MOCK_MAX_CHUNKS;
   s_ext_mock.chunk_dim = EXT_MOCK_DIMS;
   s_ext_mock.embeddings_available = true;
   g_config.memory.focus_injection.document_min_relevance = 0.48f;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "x", embed_q, EXT_MOCK_DIMS, 1700000000,
                                                /*per_source_max=*/5, &result));
   int doc_count = 0;
   const focus_candidate_t *only = NULL;
   for (int i = 0; i < result.candidate_count; i++) {
      if (strcmp(result.candidates[i].source_id, "document_chunk") == 0) {
         doc_count++;
         only = &result.candidates[i];
      }
   }
   /* 31 chunks at the corpus-typical similarity are gated out; the one that
    * stands out is kept. */
   TEST_ASSERT_EQUAL_INT(1, doc_count);
   TEST_ASSERT_TRUE(strstr(only->text, "the relevant passage") != NULL);
   focus_result_free(&result);

   /* With the gate off, the typical chunks come back. */
   g_config.memory.focus_injection.document_min_relevance = 0.0f;
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "x", embed_q, EXT_MOCK_DIMS, 1700000000,
                                                /*per_source_max=*/5, &result));
   doc_count = 0;
   for (int i = 0; i < result.candidate_count; i++)
      if (strcmp(result.candidates[i].source_id, "document_chunk") == 0)
         doc_count++;
   TEST_ASSERT_TRUE(doc_count > 1);
   focus_result_free(&result);
}

/* A corpus large enough for the semantic gate: one chunk that stands out, the
 * rest at the corpus-typical similarity. */
static void seed_gated_corpus(void) {
   seed_chunk(0, 500, 1, "the relevant passage", "answer.txt", embed_q, 1700000000);
   for (int i = 1; i < EXT_MOCK_MAX_CHUNKS; i++) {
      char text[32];
      snprintf(text, sizeof(text), "typical chunk %d", i);
      seed_chunk(i, 500 + i, 1, text, "other.txt", embed_baseline, 1700000000);
   }
   s_ext_mock.chunk_count = EXT_MOCK_MAX_CHUNKS;
   s_ext_mock.chunk_dim = EXT_MOCK_DIMS;
   s_ext_mock.embeddings_available = true;
   g_config.memory.focus_injection.document_min_relevance = 0.48f;
}

static bool result_has_text(const focus_compose_result_t *r, const char *needle) {
   for (int i = 0; i < r->candidate_count; i++) {
      if (strcmp(r->candidates[i].source_id, "document_chunk") == 0 &&
          strstr(r->candidates[i].text, needle) != NULL)
         return true;
   }
   return false;
}

/* A note named in the query is found even when its similarity sits at the
 * corpus-typical level (a short query's usual case), while a chunk that only
 * shares a word in its body is still gated out. */
static void test_document_label_match_passes_the_gate(void) {
   seed_gated_corpus(); /* the stub pages by ascending id: keep 501/502 in place */
   seed_chunk(1, 501, 1, "HARBOR LANE RELOCATION PROGRAM budget plan",
              "Harbor Lane Relocation - Budget Plan", embed_baseline, 1700000000);
   s_ext_mock.chunk_bm25[1] = 1.0f;
   seed_chunk(2, 502, 1, "boats in the harbor during the dock relocation", "boats.txt",
              embed_baseline, 1700000000);
   s_ext_mock.chunk_bm25[2] = 0.9f;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         focus_compose(1, false, "Harbor Lane Relocation", embed_q, EXT_MOCK_DIMS,
                                       1700000000, /*per_source_max=*/5, &result));
   TEST_ASSERT_TRUE(result_has_text(&result, "HARBOR LANE RELOCATION"));
   TEST_ASSERT_TRUE(result_has_text(&result, "the relevant passage"));
   TEST_ASSERT_FALSE(result_has_text(&result, "boats in the harbor"));
   TEST_ASSERT_FALSE(result_has_text(&result, "typical chunk"));
   focus_result_free(&result);
}

/* Without an embedding the label check still finds the named note; its
 * semantic score is unknown rather than a made-up number. */
static void test_document_keyword_only_finds_named_note(void) {
   seed_chunk(0, 700, 1, "moving costs and lease details", "Harbor Lane Relocation", embed_baseline,
              1700000000);
   s_ext_mock.chunk_bm25[0] = 1.0f;
   seed_chunk(1, 701, 1, "unrelated text", "other.txt", embed_baseline, 1700000000);
   s_ext_mock.chunk_count = 2;
   s_ext_mock.chunk_dim = EXT_MOCK_DIMS;
   s_ext_mock.embeddings_available = true;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "harbor relocation", /*qembed*/ NULL, 0,
                                                1700000000, 5, &result));
   int docs = 0;
   for (int i = 0; i < result.candidate_count; i++) {
      if (strcmp(result.candidates[i].source_id, "document_chunk") == 0) {
         docs++;
         TEST_ASSERT_TRUE(strstr(result.candidates[i].text, "moving costs") != NULL);
         TEST_ASSERT_EQUAL_FLOAT(FOCUS_SCORE_NA, result.candidates[i].semantic_score);
      }
   }
   TEST_ASSERT_EQUAL_INT(1, docs);
   TEST_ASSERT_EQUAL_INT(0, s_ext_mock.call_count_chunk_search_load);
   focus_result_free(&result);
}

/* One document can't fill every slot. */
static void test_document_per_document_cap(void) {
   for (int i = 0; i < 5; i++) {
      char text[32];
      snprintf(text, sizeof(text), "big note part %d", i);
      seed_chunk(i, 800 + i, 1, text, "big.txt", embed_v1, 1700000000);
      s_ext_mock.chunks[i].document_id = 80; /* all one document */
   }
   seed_chunk(5, 900, 1, "second document", "second.txt", embed_v1, 1700000000);
   s_ext_mock.chunk_count = 6;
   s_ext_mock.chunk_dim = EXT_MOCK_DIMS;
   s_ext_mock.embeddings_available = true;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "x", embed_q, EXT_MOCK_DIMS, 1700000000,
                                                /*per_source_max=*/6, &result));
   int big = 0;
   for (int i = 0; i < result.candidate_count; i++) {
      if (strstr(result.candidates[i].text, "big note part") != NULL)
         big++;
   }
   TEST_ASSERT_EQUAL_INT(2, big);
   TEST_ASSERT_TRUE(result_has_text(&result, "second document"));
   focus_result_free(&result);
}

/* The shared ranker: fused score, a keyword-only hit scored by its real cosine
 * (not 0), and the phrase bonus reaching a keyword hit outside the top by
 * cosine. */
static const float embed_mid[EXT_MOCK_DIMS] = { 0.6f, 0.8f, 0.0f, 0.0f }; /* cos 0.6 */

static void test_document_rank_fuses_both_channels(void) {
   seed_chunk(0, 10, 1, "alpha text", "a.txt", embed_q, 1700000000);
   seed_chunk(1, 11, 1, "beta text", "b.txt", embed_v2, 1700000000);
   seed_chunk(2, 12, 1, "quarterly budget review notes", "Budget Review", embed_mid, 1700000000);
   s_ext_mock.chunk_bm25[2] = 0.8f;
   s_ext_mock.chunk_count = 3;
   s_ext_mock.chunk_dim = EXT_MOCK_DIMS;
   s_ext_mock.embeddings_available = true;
   g_config.documents.hybrid_vector_weight = 0.7f;
   g_config.documents.hybrid_keyword_weight = 0.3f;
   g_config.documents.phrase_bonus_weight = 0.25f;

   /* semantic_top 1: only chunk 10 comes from the semantic channel, so chunk
    * 12 is a keyword-only hit whose cosine must still be its real 0.6. */
   const document_rank_opts_t opts = { .semantic_top = 1,
                                       .lexical_limit = 8,
                                       .phrase_top = 1,
                                       .body_phrase = false,
                                       .temporal = false };
   document_ranking_t r;
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_rank_hybrid(1, "budget review", embed_q, EXT_MOCK_DIMS,
                                                       &opts, &r));
   TEST_ASSERT_EQUAL_INT(2, r.count);
   TEST_ASSERT_EQUAL_INT(3, r.stats.pool);
   TEST_ASSERT_EQUAL_INT(2, r.query_terms);

   const document_ranked_t *kw = r.items[0].chunk_id == 12 ? &r.items[0] : &r.items[1];
   const document_ranked_t *sem = r.items[0].chunk_id == 10 ? &r.items[0] : &r.items[1];
   TEST_ASSERT_TRUE(kw->has_cosine);
   TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.6f, kw->cosine);
   /* 0.7 * 0.6 + 0.3 * 0.8 + 0.25 * 1.0 (full label phrase) */
   TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.42f + 0.24f + 0.25f, kw->hybrid);
   TEST_ASSERT_EQUAL_INT(2, kw->label_terms);
   TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.7f, sem->hybrid);
   TEST_ASSERT_EQUAL_INT(0, sem->label_terms);
   TEST_ASSERT_EQUAL_INT64(12, r.items[0].chunk_id); /* best fused first */
   /* Text is read only for the chunks a caller keeps. */
   TEST_ASSERT_NULL(r.items[0].text);
   document_ranked_t *keep[] = { &r.items[0] };
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_ranking_load_text(1, &r, keep, 1));
   TEST_ASSERT_EQUAL_STRING("quarterly budget review notes", r.items[0].text);
   TEST_ASSERT_NULL(r.items[1].text);
   document_ranking_free(&r);
   TEST_ASSERT_NULL(r.items);

   /* Keywords only: no cosine, keyword + phrase still rank. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_rank_hybrid(1, "budget review", NULL, 0, &opts, &r));
   TEST_ASSERT_EQUAL_INT(1, r.count);
   TEST_ASSERT_FALSE(r.items[0].has_cosine);
   TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.24f + 0.25f, r.items[0].hybrid);
   document_ranking_free(&r);
}

/* The label rule: content words only, stemmed, at least two or the only one. */
static void test_document_label_rule(void) {
   document_query_terms_t terms;
   document_query_terms("What's the Harbor Lane Relocation?", &terms);
   TEST_ASSERT_EQUAL_INT(3, terms.count); /* what / s / the are dropped */
   TEST_ASSERT_EQUAL_INT(3, document_label_terms(&terms, "Harbor Lane Relocation - Budget"));
   TEST_ASSERT_EQUAL_INT(1, document_label_terms(&terms, "Harbor camera notes"));
   TEST_ASSERT_EQUAL_INT(0, document_label_terms(&terms, "Garden Planting Schedule"));

   document_query_terms("relocations", &terms); /* stems match the label's */
   TEST_ASSERT_EQUAL_INT(1, terms.count);
   TEST_ASSERT_EQUAL_INT(1, document_label_terms(&terms, "Lane Relocation"));

   /* Both sides split words the same way: underscores separate, non-ASCII
    * letters don't. */
   document_query_terms("tax return", &terms);
   TEST_ASSERT_EQUAL_INT(2, document_label_terms(&terms, "Tax_Return_2024.pdf"));
   document_query_terms("r\xc3\xa9sum\xc3\xa9 draft", &terms);
   TEST_ASSERT_EQUAL_INT(2, terms.count);
   TEST_ASSERT_EQUAL_INT(2, document_label_terms(&terms, "r\xc3\xa9sum\xc3\xa9 (draft)"));
   TEST_ASSERT_EQUAL_INT(0, document_label_terms(&terms, "Sum of costs"));

   /* A long message of repeated words still reaches a name at its end. */
   {
      char msg[8192];
      size_t off = 0;
      for (int i = 0; i < 1200; i++) {
         off += (size_t)snprintf(msg + off, sizeof(msg) - off, "spam ");
      }
      snprintf(msg + off, sizeof(msg) - off, "harbor relocation");
      document_query_terms(msg, &terms);
      TEST_ASSERT_EQUAL_INT(3, terms.count);
      TEST_ASSERT_EQUAL_INT(2, document_label_terms(&terms, "Harbor Lane Relocation"));
   }

   /* Curly quotes and dashes separate words; accented capitals fold. */
   document_query_terms("show me \xe2\x80\x9cmarigold project plan\xe2\x80\x9d", &terms);
   TEST_ASSERT_EQUAL_INT(3,
                         document_label_terms(&terms, "Marigold Project Plan \xe2\x80\x94 Primer"));
   /* Letters and digits from the Latin-1 block stay in their word. */
   document_query_terms("m\xc2\xb2 pricing", &terms);
   TEST_ASSERT_EQUAL_INT(2, terms.count);
   TEST_ASSERT_EQUAL_INT(2, document_label_terms(&terms, "Office m\xc2\xb2 pricing"));
   TEST_ASSERT_EQUAL_INT(1, document_label_terms(&terms, "m pricing"));
   document_query_terms("R\xc3\x89SUM\xc3\x89", &terms);
   TEST_ASSERT_EQUAL_INT(1, document_label_terms(&terms, "r\xc3\xa9sum\xc3\xa9"));

   TEST_ASSERT_TRUE(document_label_matches(3, 3));
   TEST_ASSERT_TRUE(document_label_matches(2, 5));
   TEST_ASSERT_FALSE(document_label_matches(1, 3));
   TEST_ASSERT_TRUE(document_label_matches(1, 1));
   TEST_ASSERT_FALSE(document_label_matches(0, 0));
}

/* =====================================================================
 * 6-10.  Calendar adapter happy paths
 * ===================================================================== */

static void test_calendar_range_only_path(void) {
   const time_t now = 1700000000;
   const int64_t cal = seed_basic_user_calendar(/*user*/ 1);
   seed_occurrence(0, 5000, cal, "dentist", now + 3600, "uid-1");
   s_ext_mock.occurrence_count = 1;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   /* A schedule question: the window's pull, plus the named-event lookup. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "what's on my calendar", NULL, 0, now, 5,
                                                &result));
   TEST_ASSERT_EQUAL_INT(1, s_ext_mock.call_count_occurrences_in_range);
   TEST_ASSERT_EQUAL_INT(1, s_ext_mock.call_count_events_nearest);
   TEST_ASSERT_EQUAL_INT(0, s_ext_mock.call_count_occurrences_search);
   const focus_candidate_t *fc = NULL;
   for (int i = 0; i < result.candidate_count; i++)
      if (strcmp(result.candidates[i].source_id, "calendar_event") == 0)
         fc = &result.candidates[i];
   TEST_ASSERT_NOT_NULL(fc);
   TEST_ASSERT_TRUE(strstr(fc->text, "dentist") != NULL);
   /* Range-only path → semantic_score is FOCUS_SCORE_NA (negative sentinel). */
   TEST_ASSERT_TRUE(fc->semantic_score < 0.0f);
   focus_result_free(&result);
}

/* An event the message names is found outside any window it asks about, and
 * scores by how much of its title matched ("Pepper" is one of two distinctive
 * words in "Pepper birthday": 0.5 + 0.2 * 0.5). */
static void test_calendar_named_event_found_beyond_the_window(void) {
   const time_t now = 1700000000;
   const int64_t cal = seed_basic_user_calendar(1);
   seed_occurrence(0, 5000, cal, "Pepper birthday", now + 30 * 86400, "uid-x");
   s_ext_mock.occurrence_count = 1;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "when is Pepper's party?", NULL, 0, now,
                                                5, &result));
   /* No time asked about: the named-event lookup only. */
   TEST_ASSERT_EQUAL_INT(0, s_ext_mock.call_count_occurrences_in_range);
   TEST_ASSERT_EQUAL_INT(1, s_ext_mock.call_count_events_nearest);
   TEST_ASSERT_EQUAL_INT(0, s_ext_mock.call_count_occurrences_search);
   const focus_candidate_t *fc = NULL;
   for (int i = 0; i < result.candidate_count; i++)
      if (strcmp(result.candidates[i].source_id, "calendar_event") == 0)
         fc = &result.candidates[i];
   TEST_ASSERT_NOT_NULL(fc);
   TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.6f, fc->semantic_score);
   focus_result_free(&result);
}

static void test_calendar_consulted_without_query_embedding(void) {
   const time_t now = 1700000000;
   const int64_t cal = seed_basic_user_calendar(1);
   seed_occurrence(0, 5000, cal, "standup", now + 3600, NULL);
   s_ext_mock.occurrence_count = 1;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   /* requires_embedding=false → adapter STILL consulted with NULL embed. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "what's on my calendar", /*qembed*/ NULL,
                                                0, now, 5, &result));
   TEST_ASSERT_EQUAL_INT(1, s_ext_mock.call_count_occurrences_in_range);
   bool saw = false;
   for (int i = 0; i < result.candidate_count; i++)
      if (strcmp(result.candidates[i].source_id, "calendar_event") == 0)
         saw = true;
   TEST_ASSERT_TRUE(saw);
   focus_result_free(&result);
}

static bool has_event(const focus_compose_result_t *r, const char *needle) {
   for (int i = 0; i < r->candidate_count; i++) {
      if (strcmp(r->candidates[i].source_id, "calendar_event") == 0 &&
          strstr(r->candidates[i].text, needle) != NULL)
         return true;
   }
   return false;
}

/* A message about something else gets no calendar, however soon the events;
 * asked about tomorrow, it gets tomorrow's and not next week's. */
static void test_calendar_time_aware(void) {
   setenv("TZ", "UTC", 1);
   tzset();
   const time_t now = 1790769600; /* Wednesday 12:00 UTC */
   const int64_t cal = seed_basic_user_calendar(1);
   seed_occurrence(0, 5000, cal, "Pottery class", now + 3600, NULL);
   seed_occurrence(1, 5001, cal, "Chiropractor", now + 86400, NULL);
   seed_occurrence(2, 5002, cal, "Choir practice", now + 6 * 86400, NULL);
   s_ext_mock.occurrence_count = 3;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "recall the marigold project plan", NULL,
                                                0, now, 10, &result));
   TEST_ASSERT_FALSE(has_event(&result, "Pottery"));
   TEST_ASSERT_FALSE(has_event(&result, "Chiropractor"));
   focus_result_free(&result);

   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "what's tomorrow look like", NULL, 0, now,
                                                10, &result));
   TEST_ASSERT_TRUE(has_event(&result, "Chiropractor"));
   TEST_ASSERT_FALSE(has_event(&result, "Pottery"));
   TEST_ASSERT_FALSE(has_event(&result, "Choir practice"));
   focus_result_free(&result);

   /* Named, an event outside any window still comes back. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "when is choir practice", NULL, 0, now,
                                                10, &result));
   TEST_ASSERT_TRUE(has_event(&result, "Choir practice"));
   TEST_ASSERT_FALSE(has_event(&result, "Chiropractor"));
   focus_result_free(&result);
   unsetenv("TZ");
   tzset();
}

/* Events created separately under one title count as one title: the nearest
 * stands for them, and a word they share is still unique among titles. */
static void test_calendar_shared_titles(void) {
   const time_t now = 1700000000;
   const int64_t cal = seed_basic_user_calendar(1);
   seed_occurrence(0, 5000, cal, "Choir practice", now + 9 * 86400, NULL);
   seed_occurrence(1, 5001, cal, "Choir practice", now + 2 * 86400, NULL);
   seed_occurrence(2, 5002, cal, "Choir fundraiser", now + 20 * 86400, NULL);
   s_ext_mock.occurrence_count = 3;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         focus_compose(1, false, "when is practice", NULL, 0, now, 10, &result));
   int practices = 0;
   for (int i = 0; i < result.candidate_count; i++) {
      if (strcmp(result.candidates[i].source_id, "calendar_event") == 0 &&
          strstr(result.candidates[i].text, "Choir practice")) {
         practices++;
         TEST_ASSERT_EQUAL_STRING("calendar_occ:5001", result.candidates[i].item_id);
      }
   }
   TEST_ASSERT_EQUAL_INT(1, practices);
   TEST_ASSERT_FALSE(has_event(&result, "fundraiser"));
   focus_result_free(&result);
}

/* All-day events: in a window by date, found by name, shown as a date. */
static void test_calendar_all_day_events(void) {
   setenv("TZ", "America/New_York", 1);
   tzset();
   const time_t now = 1790769600; /* Wednesday 2026-09-30 08:00 EDT */
   const int64_t cal = seed_basic_user_calendar(1);
   /* Stored like the sync does: dtstart/dtend at the date's UTC midnight. */
   seed_occurrence(0, 5000, cal, "Quilt regatta", 1790812800, NULL); /* 2026-10-01 */
   s_ext_mock.occurrences[0].all_day = true;
   s_ext_mock.occurrences[0].dtend = 1790812800 + 86400;
   snprintf(s_ext_mock.occurrences[0].dtstart_date, sizeof(s_ext_mock.occurrences[0].dtstart_date),
            "2026-10-01");
   snprintf(s_ext_mock.occurrences[0].dtend_date, sizeof(s_ext_mock.occurrences[0].dtend_date),
            "2026-10-02");
   s_ext_mock.occurrence_count = 1;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   /* Today's window doesn't reach it (its UTC midnight is today evening locally). */
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         focus_compose(1, false, "what's on today", NULL, 0, now, 10, &result));
   TEST_ASSERT_FALSE(has_event(&result, "Quilt"));
   focus_result_free(&result);

   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         focus_compose(1, false, "anything tomorrow", NULL, 0, now, 10, &result));
   TEST_ASSERT_TRUE(has_event(&result, "[2026-10-01 all day] Quilt regatta"));
   focus_result_free(&result);

   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "when is the quilt regatta", NULL, 0, now,
                                                10, &result));
   TEST_ASSERT_TRUE(has_event(&result, "all day] Quilt regatta"));
   focus_result_free(&result);
   unsetenv("TZ");
   tzset();
}

/* The assistant named after a weekday: addressing it isn't asking about that
 * day, placing the word as a day is.  A failed named-event lookup still
 * leaves the window's events. */
static void test_calendar_assistant_weekday_name(void) {
   setenv("TZ", "UTC", 1);
   tzset();
   const time_t now = 1790769600; /* Wednesday 12:00 UTC */
   const int64_t cal = seed_basic_user_calendar(1);
   seed_occurrence(0, 5000, cal, "Pottery class", now + 2 * 86400, NULL); /* Friday */
   s_ext_mock.occurrence_count = 1;
   snprintf(g_config.general.ai_name, sizeof(g_config.general.ai_name), "friday");

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "friday, what's the weather", NULL, 0,
                                                now, 10, &result));
   TEST_ASSERT_FALSE(has_event(&result, "Pottery"));
   focus_result_free(&result);

   s_ext_mock.fail_events_nearest = true;
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "friday, anything on friday", NULL, 0,
                                                now, 10, &result));
   TEST_ASSERT_TRUE(has_event(&result, "Pottery"));
   focus_result_free(&result);
   g_config.general.ai_name[0] = '\0';
   unsetenv("TZ");
   tzset();
}

/* A generic word ("call") doesn't name an event; a distinctive one does. */
static void test_calendar_generic_title_words(void) {
   const time_t now = 1700000000;
   const int64_t cal = seed_basic_user_calendar(1);
   seed_occurrence(0, 5000, cal, "Call with the bank", now + 20 * 86400, NULL);
   s_ext_mock.occurrence_count = 1;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "call mom", NULL, 0, now, 5, &result));
   TEST_ASSERT_FALSE(has_event(&result, "bank"));
   focus_result_free(&result);
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "when do I talk to the bank", NULL, 0,
                                                now, 5, &result));
   TEST_ASSERT_TRUE(has_event(&result, "bank"));
   focus_result_free(&result);
}

/* An empty message asks about nothing: no calendar. */
static void test_calendar_empty_query_returns_nothing(void) {
   const time_t now = 1700000000;
   const int64_t cal = seed_basic_user_calendar(1);
   seed_occurrence(0, 5000, cal, "lunch", now + 3600, NULL);
   s_ext_mock.occurrence_count = 1;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "", NULL, 0, now, 5, &result));
   TEST_ASSERT_EQUAL_INT(0, s_ext_mock.call_count_occurrences_in_range);
   TEST_ASSERT_EQUAL_INT(0, s_ext_mock.call_count_events_nearest);
   for (int i = 0; i < result.candidate_count; i++)
      TEST_ASSERT_NOT_EQUAL(0, strcmp(result.candidates[i].source_id, "calendar_event"));
   focus_result_free(&result);
}

static void test_calendar_inactive_calendar_excluded(void) {
   const time_t now = 1700000000;
   seed_account(0, 1, 1, "primary");
   s_ext_mock.account_count = 1;
   seed_calendar(0, 100, 1, "active_cal", true);
   seed_calendar(1, 101, 1, "inactive_cal", false);
   s_ext_mock.calendar_count = 2;
   seed_occurrence(0, 5000, /*cal*/ 100, "active event", now + 3600, NULL);
   seed_occurrence(1, 5001, /*cal*/ 101, "inactive event", now + 3600, NULL);
   s_ext_mock.occurrence_count = 2;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "what's on my calendar", NULL, 0, now, 10,
                                                &result));
   bool saw_active = false, saw_inactive = false;
   for (int i = 0; i < result.candidate_count; i++) {
      if (strcmp(result.candidates[i].source_id, "calendar_event") != 0)
         continue;
      if (strstr(result.candidates[i].text, "active event"))
         saw_active = true;
      if (strstr(result.candidates[i].text, "inactive event"))
         saw_inactive = true;
   }
   TEST_ASSERT_TRUE_MESSAGE(saw_active, "active calendar's event must surface");
   TEST_ASSERT_FALSE_MESSAGE(saw_inactive, "inactive calendar's event must NOT surface");
   focus_result_free(&result);
}

/* =====================================================================
 * 11-13.  Empty-result behavior
 * ===================================================================== */

static void test_document_empty_db(void) {
   s_ext_mock.chunk_dim = EXT_MOCK_DIMS;
   s_ext_mock.embeddings_available = true;
   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "x", embed_q, EXT_MOCK_DIMS, 1700000000,
                                                5, &result));
   for (int i = 0; i < result.candidate_count; i++)
      TEST_ASSERT_NOT_EQUAL(0, strcmp(result.candidates[i].source_id, "document_chunk"));
   focus_result_free(&result);
}

static void test_calendar_empty_db(void) {
   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, NULL, NULL, 0, 1700000000, 5, &result));
   for (int i = 0; i < result.candidate_count; i++)
      TEST_ASSERT_NOT_EQUAL(0, strcmp(result.candidates[i].source_id, "calendar_event"));
   focus_result_free(&result);
}

static void test_unknown_user_zero_candidates(void) {
   /* Seed everything for user_id=1 but query as user_id=99999. */
   seed_chunk(0, 100, 1, "stuff", "f.txt", embed_v1, 1700000000);
   s_ext_mock.chunk_count = 1;
   s_ext_mock.chunk_dim = EXT_MOCK_DIMS;
   s_ext_mock.embeddings_available = true;
   const int64_t cal = seed_basic_user_calendar(1);
   seed_occurrence(0, 1, cal, "event", 1700000000 + 3600, NULL);
   s_ext_mock.occurrence_count = 1;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(/*user*/ 99999, false, "stuff", embed_q,
                                                EXT_MOCK_DIMS, 1700000000, 5, &result));
   TEST_ASSERT_EQUAL_INT(0, result.candidate_count);
   focus_result_free(&result);
}

/* =====================================================================
 * 14.  Network-call invariant
 *
 * Link-level enforcement: this TU does not provide stubs for any
 * service-layer symbols (calendar_service_*, email_service_*,
 * document_search) — if the adapter ever regresses to call those,
 * the link breaks.  Runtime belt-and-suspenders: assert that ONLY the
 * expected DB-layer counters incremented after a complete compose.
 * ===================================================================== */

static void test_no_network_calls_during_compose(void) {
   const time_t now = 1700000000;
   seed_chunk(0, 1, 1, "doc", "f.txt", embed_v1, now);
   s_ext_mock.chunk_count = 1;
   s_ext_mock.chunk_dim = EXT_MOCK_DIMS;
   s_ext_mock.embeddings_available = true;
   const int64_t cal = seed_basic_user_calendar(1);
   seed_occurrence(0, 1, cal, "evt", now + 3600, NULL);
   s_ext_mock.occurrence_count = 1;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS,
                         focus_compose(1, false, "doc", embed_q, EXT_MOCK_DIMS, now, 5, &result));
   /* Only DB-layer counters should be > 0. */
   TEST_ASSERT_EQUAL_INT(1, s_ext_mock.call_count_chunk_search_load);
   TEST_ASSERT_EQUAL_INT(1, s_ext_mock.call_count_account_list);
   TEST_ASSERT_TRUE(s_ext_mock.call_count_calendar_list >= 1);
   /* No time asked about: only the named-event lookup reads occurrences. */
   TEST_ASSERT_EQUAL_INT(0, s_ext_mock.call_count_occurrences_in_range);
   TEST_ASSERT_EQUAL_INT(1, s_ext_mock.call_count_events_nearest);
   TEST_ASSERT_EQUAL_INT(0, s_ext_mock.call_count_occurrences_search);
   focus_result_free(&result);
}

/* The chunk-embedding cache reads the database once per generation: a second
 * turn reuses it, and a change (new generation) or another user rebuilds it. */
static void test_document_embedding_cache_reuse(void) {
   const time_t now = 1700000000;
   seed_chunk(0, 1, 1, "doc", "f.txt", embed_v1, now);
   s_ext_mock.chunk_count = 1;
   s_ext_mock.chunk_dim = EXT_MOCK_DIMS;
   s_ext_mock.embeddings_available = true;
   s_ext_mock.pin_generation = true;
   s_ext_mock.generation = 1000;
   document_embed_cache_shutdown(); /* no copy from an earlier test */

   document_chunk_score_t top[4];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_embed_rank(1, embed_q, EXT_MOCK_DIMS, 4, top, &n, NULL));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_embed_rank(1, embed_q, EXT_MOCK_DIMS, 4, top, &n, NULL));
   TEST_ASSERT_EQUAL_INT(1, s_ext_mock.call_count_chunk_search_load);

   s_ext_mock.generation++;
   TEST_ASSERT_EQUAL_INT(SUCCESS, document_embed_rank(1, embed_q, EXT_MOCK_DIMS, 4, top, &n, NULL));
   TEST_ASSERT_EQUAL_INT(2, s_ext_mock.call_count_chunk_search_load);

   TEST_ASSERT_EQUAL_INT(SUCCESS, document_embed_rank(2, embed_q, EXT_MOCK_DIMS, 4, top, &n, NULL));
   TEST_ASSERT_EQUAL_INT(3, s_ext_mock.call_count_chunk_search_load);
   TEST_ASSERT_EQUAL_INT(0, n); /* the chunk is user 1's */

   document_embed_cache_shutdown();
}

/* =====================================================================
 * 15-16.  Failure / partial-failure cleanup — out-params zeroed
 * ===================================================================== */

static void test_document_failure_zeros_outparams(void) {
   s_ext_mock.chunk_dim = EXT_MOCK_DIMS;
   s_ext_mock.embeddings_available = true;
   s_ext_mock.fail_chunk_search = true; /* force FAILURE */

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   /* Per-adapter FAILUREs are logged but compose still SUCCEEDs;
    * the relevant assertion is that no document_chunk candidates
    * leaked through and no allocations were lost (ASan run covers
    * the leak side; this asserts the visible state). */
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "x", embed_q, EXT_MOCK_DIMS, 1700000000,
                                                5, &result));
   for (int i = 0; i < result.candidate_count; i++)
      TEST_ASSERT_NOT_EQUAL(0, strcmp(result.candidates[i].source_id, "document_chunk"));
   focus_result_free(&result);
}

static void test_calendar_failure_zeros_outparams(void) {
   const time_t now = 1700000000;
   seed_basic_user_calendar(1);
   s_ext_mock.fail_occurrences_in_range = true;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   /* A schedule question, so the failing window pull is reached. */
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "what's on my calendar", NULL, 0, now, 5,
                                                &result));
   TEST_ASSERT_EQUAL_INT(1, s_ext_mock.call_count_occurrences_in_range);
   for (int i = 0; i < result.candidate_count; i++)
      TEST_ASSERT_NOT_EQUAL(0, strcmp(result.candidates[i].source_id, "calendar_event"));
   focus_result_free(&result);
}

/* =====================================================================
 * 17.  Multi-account cap
 * ===================================================================== */

static void test_calendar_multi_account_cap(void) {
   const time_t now = 1700000000;
   /* User has 5 accounts — adapter should query only the first 3. */
   for (int i = 0; i < 5; i++) {
      char name[16];
      snprintf(name, sizeof(name), "acct%d", i);
      seed_account(i, 10 + i, 1, name);
      seed_calendar(i, 100 + i, 10 + i, "cal", true);
      seed_occurrence(i, 5000 + i, 100 + i, name, now + 3600, NULL);
   }
   s_ext_mock.account_count = 5;
   s_ext_mock.calendar_count = 5;
   s_ext_mock.occurrence_count = 5;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "what's on my calendar", NULL, 0, now, 32,
                                                &result));
   /* Only 3 calendar_list calls fire (cap = EXTERNAL_MAX_ACCOUNTS_PER_COMPOSE). */
   TEST_ASSERT_EQUAL_INT(3, s_ext_mock.call_count_calendar_list);
   /* Only the first-3 accounts' events surface. */
   bool saw_acct[5] = { false };
   for (int i = 0; i < result.candidate_count; i++) {
      if (strcmp(result.candidates[i].source_id, "calendar_event") != 0)
         continue;
      for (int j = 0; j < 5; j++) {
         char needle[16];
         snprintf(needle, sizeof(needle), "acct%d", j);
         if (strstr(result.candidates[i].text, needle))
            saw_acct[j] = true;
      }
   }
   TEST_ASSERT_TRUE(saw_acct[0] && saw_acct[1] && saw_acct[2]);
   TEST_ASSERT_FALSE(saw_acct[3]);
   TEST_ASSERT_FALSE(saw_acct[4]);
   focus_result_free(&result);
}

/* =====================================================================
 * 18-19.  ITEM_ID server-generation invariants
 * ===================================================================== */

static void test_document_item_id_format(void) {
   seed_chunk(0, 12345, 1, "x", "f.txt", embed_v1, 1700000000);
   s_ext_mock.chunk_count = 1;
   s_ext_mock.chunk_dim = EXT_MOCK_DIMS;
   s_ext_mock.embeddings_available = true;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "x", embed_q, EXT_MOCK_DIMS, 1700000000,
                                                5, &result));
   const focus_candidate_t *fc = NULL;
   for (int i = 0; i < result.candidate_count; i++)
      if (strcmp(result.candidates[i].source_id, "document_chunk") == 0)
         fc = &result.candidates[i];
   TEST_ASSERT_NOT_NULL(fc);
   TEST_ASSERT_EQUAL_STRING("document_chunk:12345", fc->item_id);
   focus_result_free(&result);
}

static void test_calendar_item_id_never_contains_ical_uid(void) {
   const time_t now = 1700000000;
   const int64_t cal = seed_basic_user_calendar(1);
   /* event_uid is upstream-controlled (CalDAV); item_id MUST be the
    * local DB row id (occurrence.id), NEVER built from event_uid. */
   const char *attacker_uid = "PAYLOAD-IGNORE-ALL-PRIOR";
   seed_occurrence(0, /*occ_id*/ 99, cal, "harmless title", now + 3600, attacker_uid);
   s_ext_mock.occurrence_count = 1;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "what's on my calendar", NULL, 0, now, 5,
                                                &result));
   const focus_candidate_t *fc = NULL;
   for (int i = 0; i < result.candidate_count; i++)
      if (strcmp(result.candidates[i].source_id, "calendar_event") == 0)
         fc = &result.candidates[i];
   TEST_ASSERT_NOT_NULL(fc);
   TEST_ASSERT_EQUAL_STRING("calendar_occ:99", fc->item_id);
   TEST_ASSERT_NULL_MESSAGE(strstr(fc->item_id, "PAYLOAD"),
                            "item_id must never contain user-influenceable iCal UID");
   focus_result_free(&result);
}

/* =====================================================================
 * 20-22.  Calendar event-time recency / window behavior
 * ===================================================================== */

static void test_calendar_today_higher_recency_than_far_future(void) {
   const time_t now = 1700000000;
   const int64_t cal = seed_basic_user_calendar(1);
   /* Today +1h vs day-6 (still inside the 7d window). */
   seed_occurrence(0, 1, cal, "today", now + 3600, NULL);
   seed_occurrence(1, 2, cal, "in_six_days", now + 6 * 86400, NULL);
   s_ext_mock.occurrence_count = 2;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "what's on my calendar", NULL, 0, now, 10,
                                                &result));
   const focus_candidate_t *today = NULL, *future = NULL;
   for (int i = 0; i < result.candidate_count; i++) {
      if (strcmp(result.candidates[i].source_id, "calendar_event") != 0)
         continue;
      if (strstr(result.candidates[i].text, "today"))
         today = &result.candidates[i];
      else if (strstr(result.candidates[i].text, "in_six_days"))
         future = &result.candidates[i];
   }
   TEST_ASSERT_NOT_NULL(today);
   TEST_ASSERT_NOT_NULL(future);
   TEST_ASSERT_TRUE_MESSAGE(today->recency_score > future->recency_score,
                            "today's event must score higher recency than a 6-day-out event");
   focus_result_free(&result);
}

static void test_calendar_yesterday_still_surfaces(void) {
   const time_t now = 1700000000;
   const int64_t cal = seed_basic_user_calendar(1);
   /* Half a day ago — inside the 1-day past window. */
   seed_occurrence(0, 1, cal, "earlier_today", now - 12 * 3600, NULL);
   s_ext_mock.occurrence_count = 1;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "what's on my calendar", NULL, 0, now, 5,
                                                &result));
   bool saw = false;
   for (int i = 0; i < result.candidate_count; i++)
      if (strcmp(result.candidates[i].source_id, "calendar_event") == 0 &&
          strstr(result.candidates[i].text, "earlier_today"))
         saw = true;
   TEST_ASSERT_TRUE(saw);
   focus_result_free(&result);
}

static void test_calendar_far_future_outside_range(void) {
   const time_t now = 1700000000;
   const int64_t cal = seed_basic_user_calendar(1);
   /* 6 months out — outside the 7-day forward window; range path
    * skips it.  No query_text → search path also doesn't fire. */
   seed_occurrence(0, 1, cal, "vacation", now + 180 * 86400, NULL);
   s_ext_mock.occurrence_count = 1;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, NULL, NULL, 0, now, 5, &result));
   for (int i = 0; i < result.candidate_count; i++)
      if (strcmp(result.candidates[i].source_id, "calendar_event") == 0)
         TEST_FAIL_MESSAGE("event 6 months out must NOT appear via the range-only path");
   focus_result_free(&result);
}

/* =====================================================================
 * 23-24.  Register-all + end-to-end
 * ===================================================================== */

static void test_register_all_external_succeeds(void) {
   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   /* Re-register without focus_unregister_all() must FAIL on the first
    * duplicate (document_chunk) per the framework's no-overwrite rule. */
   TEST_ASSERT_EQUAL_INT(FAILURE, external_focus_adapters_register_all());
}

static void test_end_to_end_compose_with_both_adapters(void) {
   const time_t now = 1700000000;
   seed_chunk(0, 1, 1, "doc body", "ref.pdf", embed_v1, now);
   s_ext_mock.chunk_count = 1;
   s_ext_mock.chunk_dim = EXT_MOCK_DIMS;
   s_ext_mock.embeddings_available = true;
   const int64_t cal = seed_basic_user_calendar(1);
   seed_occurrence(0, 5000, cal, "soon", now + 3600, NULL);
   s_ext_mock.occurrence_count = 1;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "the doc and my calendar", embed_q,
                                                EXT_MOCK_DIMS, now, 5, &result));
   bool saw_doc = false, saw_cal = false;
   for (int i = 0; i < result.candidate_count; i++) {
      if (strcmp(result.candidates[i].source_id, "document_chunk") == 0)
         saw_doc = true;
      if (strcmp(result.candidates[i].source_id, "calendar_event") == 0)
         saw_cal = true;
   }
   TEST_ASSERT_TRUE_MESSAGE(saw_doc, "document_chunk candidate must surface end-to-end");
   TEST_ASSERT_TRUE_MESSAGE(saw_cal, "calendar_event candidate must surface end-to-end");
   focus_result_free(&result);
}

/* =====================================================================
 * 25.  Memory ownership cycle — register/compose/free 1000x
 *
 * Production failure mode for missed `focus_result_free()` calls or
 * leaked mid-loop allocations is unbounded growth.  ASan picks up the
 * leak on any single iteration; the loop both stresses the contract
 * and serves as a regression for the failure-cleanup paths exercised
 * above.
 * ===================================================================== */

/* =====================================================================
 * Document-chunk size handling:
 *   - 3 KB chunks must pass through with full text intact (no
 *     silent clipping at the per-candidate cap).
 *   - Chunks whose rendered "[filename] text" overflows the
 *     per-candidate cap are truncated by focus_candidate_init's
 *     standard handler — NOT pre-rejected by the adapter.
 * ===================================================================== */

static void test_document_3kb_chunk_passthrough(void) {
   /* Seed a 3 KB chunk — well below the FOCUS_TEXT_MAX_BYTES per-
    * candidate cap.  Surfaces with full text, no truncation. */
   const size_t text_size = 3072;
   char *big_text = malloc(text_size + 1);
   TEST_ASSERT_NOT_NULL(big_text);
   memset(big_text, 'X', text_size);
   big_text[text_size] = '\0';

   /* seed_chunk strncpy's into chunks[idx].text (DOC_CHUNK_TEXT_MAX = 4096). */
   seed_chunk(0, 100, 1, big_text, "big.pdf", embed_v1, 1700000000);
   s_ext_mock.chunk_count = 1;
   s_ext_mock.chunk_dim = EXT_MOCK_DIMS;
   s_ext_mock.embeddings_available = true;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "anything", embed_q, EXT_MOCK_DIMS,
                                                1700000000, 5, &result));
   const focus_candidate_t *fc = NULL;
   for (int i = 0; i < result.candidate_count; i++)
      if (strcmp(result.candidates[i].source_id, "document_chunk") == 0)
         fc = &result.candidates[i];
   TEST_ASSERT_NOT_NULL_MESSAGE(fc, "3 KB chunk must surface (no pre-rejection, no clipping)");
   /* Rendered "[big.pdf] " (10 chars) + 3072 chars text = 3082 chars,
    * well under the new 4096 cap. */
   const size_t rendered_len = strlen(fc->text);
   TEST_ASSERT_TRUE_MESSAGE(rendered_len > 3000, "3 KB chunk text should be present in candidate");
   TEST_ASSERT_TRUE_MESSAGE(rendered_len <= 4096,
                            "rendered text must respect FOCUS_TEXT_MAX_BYTES cap");
   TEST_ASSERT_NOT_NULL(strstr(fc->text, "[big.pdf]"));
   /* Verify no truncation marker / no missing tail. */
   TEST_ASSERT_EQUAL_INT('X', fc->text[rendered_len - 1]);

   focus_result_free(&result);
   free(big_text);
}

static void test_document_max_chunk_delivered_in_full(void) {
   /* Seed the maximum chunk text the stub's strncpy can accept
    * (DOC_CHUNK_TEXT_MAX - 1 = 4095 bytes) plus a deliberately long
    * filename.  FOCUS_TEXT_MAX_BYTES (4608) is sized to exceed the
    * largest possible document candidate (DOC_CHUNK_TEXT_MAX 4096 +
    * DOC_FILENAME_MAX 256 + "[] " framing 3 = 4355), so the rendered
    * candidate is delivered IN FULL — no truncation.  The adapter must
    * surface it (NOT pre-reject — the bug this test guards against), and
    * the sizing invariant must hold: lowering the cap below 4355 would
    * re-introduce truncation and trip assertion (b). */
   const size_t text_size = 4095;
   char *big_text = malloc(text_size + 1);
   TEST_ASSERT_NOT_NULL(big_text);
   memset(big_text, 'Y', text_size);
   big_text[text_size] = '\0';

   /* "[" + "really_long_filename_to_force_overflow.pdf" (42) + "] " +
    * 4095 chars of text = 4140 bytes rendered — over the old 4096 cap,
    * comfortably under the new 4608 cap. */
   seed_chunk(0, 100, 1, big_text, "really_long_filename_to_force_overflow.pdf", embed_v1,
              1700000000);
   s_ext_mock.chunk_count = 1;
   s_ext_mock.chunk_dim = EXT_MOCK_DIMS;
   s_ext_mock.embeddings_available = true;

   TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
   focus_compose_result_t result = { 0 };
   TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "anything", embed_q, EXT_MOCK_DIMS,
                                                1700000000, 5, &result));
   const focus_candidate_t *fc = NULL;
   for (int i = 0; i < result.candidate_count; i++)
      if (strcmp(result.candidates[i].source_id, "document_chunk") == 0)
         fc = &result.candidates[i];
   /* (a) candidate surfaces (NOT pre-rejected by adapter) */
   TEST_ASSERT_NOT_NULL_MESSAGE(fc, "max-size chunk must surface — adapter must NOT pre-reject");
   /* (b) delivered in full — exactly the 4140-byte rendered string,
    * and within the per-candidate cap.  Proves the cap accommodates a
    * full document candidate (guards the 4608 sizing invariant). */
   TEST_ASSERT_EQUAL_INT_MESSAGE(4140, (int)strlen(fc->text),
                                 "rendered text must be delivered in full (no truncation) — "
                                 "FOCUS_TEXT_MAX_BYTES must fit a max chunk + long filename");
   TEST_ASSERT_TRUE_MESSAGE((int)strlen(fc->text) <= FOCUS_TEXT_MAX_BYTES,
                            "rendered candidate must fit within FOCUS_TEXT_MAX_BYTES");
   /* (c) opening "[filename]" prefix retained — content kept from
    * the head, not garbled by partial render. */
   TEST_ASSERT_TRUE(strstr(fc->text, "really_long_filename") != NULL);

   focus_result_free(&result);
   free(big_text);
}

static void test_memory_cycle_1000x(void) {
   const time_t now = 1700000000;
   seed_chunk(0, 1, 1, "doc", "f.txt", embed_v1, now);
   seed_chunk(1, 2, 1, "doc2", "g.txt", embed_v2, now);
   s_ext_mock.chunk_count = 2;
   s_ext_mock.chunk_dim = EXT_MOCK_DIMS;
   s_ext_mock.embeddings_available = true;
   const int64_t cal = seed_basic_user_calendar(1);
   seed_occurrence(0, 5000, cal, "evt1", now + 3600, NULL);
   seed_occurrence(1, 5001, cal, "evt2", now + 7200, NULL);
   s_ext_mock.occurrence_count = 2;

   for (int iter = 0; iter < 1000; iter++) {
      focus_unregister_all();
      TEST_ASSERT_EQUAL_INT(SUCCESS, external_focus_adapters_register_all());
      focus_compose_result_t result = { 0 };
      TEST_ASSERT_EQUAL_INT(SUCCESS, focus_compose(1, false, "doc", embed_q, EXT_MOCK_DIMS, now, 5,
                                                   &result));
      focus_result_free(&result);
   }
}

int main(void) {
   UNITY_BEGIN();

   /* Cross-user isolation */
   RUN_TEST(test_document_cross_user);
   RUN_TEST(test_calendar_cross_user);

   /* Document adapter happy paths */
   RUN_TEST(test_document_adapter_shape);
   RUN_TEST(test_document_no_embedding_without_keyword_hits);
   RUN_TEST(test_document_cap_honoring);
   RUN_TEST(test_document_relevance_gate);
   RUN_TEST(test_document_label_match_passes_the_gate);
   RUN_TEST(test_document_keyword_only_finds_named_note);
   RUN_TEST(test_document_per_document_cap);
   RUN_TEST(test_document_label_rule);
   RUN_TEST(test_document_rank_fuses_both_channels);

   /* Calendar adapter happy paths */
   RUN_TEST(test_calendar_range_only_path);
   RUN_TEST(test_calendar_named_event_found_beyond_the_window);
   RUN_TEST(test_calendar_consulted_without_query_embedding);
   RUN_TEST(test_calendar_empty_query_returns_nothing);
   RUN_TEST(test_calendar_time_aware);
   RUN_TEST(test_calendar_generic_title_words);
   RUN_TEST(test_calendar_shared_titles);
   RUN_TEST(test_calendar_all_day_events);
   RUN_TEST(test_calendar_assistant_weekday_name);
   RUN_TEST(test_calendar_inactive_calendar_excluded);

   /* Empty-result behavior */
   RUN_TEST(test_document_empty_db);
   RUN_TEST(test_calendar_empty_db);
   RUN_TEST(test_unknown_user_zero_candidates);

   /* Network-call invariant */
   RUN_TEST(test_no_network_calls_during_compose);
   RUN_TEST(test_document_embedding_cache_reuse);

   /* Failure / partial-failure cleanup */
   RUN_TEST(test_document_failure_zeros_outparams);
   RUN_TEST(test_calendar_failure_zeros_outparams);

   /* Multi-account cap */
   RUN_TEST(test_calendar_multi_account_cap);

   /* ITEM_ID server-generation invariants */
   RUN_TEST(test_document_item_id_format);
   RUN_TEST(test_calendar_item_id_never_contains_ical_uid);

   /* Calendar event-time recency */
   RUN_TEST(test_calendar_today_higher_recency_than_far_future);
   RUN_TEST(test_calendar_yesterday_still_surfaces);
   RUN_TEST(test_calendar_far_future_outside_range);

   /* End-to-end via framework */
   RUN_TEST(test_register_all_external_succeeds);
   RUN_TEST(test_end_to_end_compose_with_both_adapters);

   /* Document-chunk size handling */
   RUN_TEST(test_document_3kb_chunk_passthrough);
   RUN_TEST(test_document_max_chunk_delivered_in_full);

   /* Memory ownership cycle (ASan target) */
   RUN_TEST(test_memory_cycle_1000x);

   return UNITY_END();
}
