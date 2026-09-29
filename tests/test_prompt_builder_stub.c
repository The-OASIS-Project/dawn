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
 * Programmable stubs for test_prompt_builder.  build_focus_block is the
 * load-bearing 1e logic we want to drive in unit tests; the framework's
 * focus_compose, the embedding engine, and the project g_config global
 * are the dependencies it pulls in.  All three are stubbed here.
 */

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "core/focus/focus_handles.h"
#include "core/focus/focus_source.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "memory/memory_embeddings.h"

dawn_config_t g_config;

/* ----- programmable focus_compose stub ----------------------------------- */

typedef struct {
   const char *source_id;
   const char *text;
   const char *item_id;
} pb_seed_candidate_t;

typedef struct {
   /* Programmable behavior */
   bool fail; /* return FAILURE */
   pb_seed_candidate_t seeded[8];
   int seeded_count;
   int rejection_count;
   /* Last-call inspection */
   int call_count;
   int last_user_id;
   bool last_had_query_embedding;
   size_t last_embed_dim;
   char last_query_text[256];
} pb_focus_compose_mock_t;

static pb_focus_compose_mock_t s_focus = { 0 };

/* Per-call score override storage — see public functions further down
 * for the API.  Defined here so pb_focus_reset can scrub it.
 *
 * 1g-i adds an independently overridable `final_score` so the
 * uplift-base intent test can drive a case where final_score and
 * semantic_score diverge.  When `final_override` is false the stub
 * sets breakdowns[i].final_score = pb_focus_seed_score_for(i) (the
 * legacy behavior — they track each other). */
typedef struct {
   bool override;
   float final_score; /* legacy seed value, used as semantic_score AND
                         (when final_override is false) as final_score */
   bool final_override;
   float final_score_value; /* explicit final_score override for 1g-i tests */
} pb_seed_score_t;

static pb_seed_score_t s_seed_scores[8];

/* Forward decls — defined further down so the focus_compose stub can
 * use the seed-score override without re-ordering the public test
 * fixture functions. */
float pb_focus_seed_score_for(int idx);
float pb_focus_seed_final_score_for(int idx);

void pb_focus_reset(void) {
   memset(&s_focus, 0, sizeof(s_focus));
   memset(s_seed_scores, 0, sizeof(s_seed_scores));
}

pb_focus_compose_mock_t *pb_focus_state(void) {
   return &s_focus;
}

int focus_compose(int user_id,
                  bool include_private,
                  const char *query_text,
                  const float *query_embedding,
                  size_t embed_dim,
                  time_t now,
                  int per_source_max_candidates,
                  focus_compose_result_t *out_result) {
   (void)include_private;
   (void)now;
   (void)per_source_max_candidates;

   s_focus.call_count++;
   s_focus.last_user_id = user_id;
   s_focus.last_had_query_embedding = (query_embedding != NULL);
   s_focus.last_embed_dim = embed_dim;
   if (query_text != NULL) {
      const size_t copy = strlen(query_text);
      const size_t cap = sizeof(s_focus.last_query_text) - 1;
      const size_t n = copy < cap ? copy : cap;
      memcpy(s_focus.last_query_text, query_text, n);
      s_focus.last_query_text[n] = '\0';
   } else {
      s_focus.last_query_text[0] = '\0';
   }

   if (out_result == NULL)
      return FAILURE;
   out_result->candidates = NULL;
   out_result->candidate_count = 0;
   out_result->rejection_count = 0;

   if (s_focus.fail)
      return FAILURE;

   if (s_focus.seeded_count <= 0) {
      /* Optionally surface a non-zero rejection count even with no
       * survivors (matches framework behavior when filter blocked
       * every candidate). */
      out_result->rejection_count = (s_focus.rejection_count > 0) ? 1 : 0;
      if (out_result->rejection_count == 1) {
         out_result->rejections[0].source_id = "memory_fact";
         out_result->rejections[0].count = s_focus.rejection_count;
      }
      return SUCCESS;
   }

   focus_candidate_t *arr = calloc((size_t)s_focus.seeded_count, sizeof(*arr));
   if (arr == NULL)
      return FAILURE;
   /* Phase 1g-i: stub allocates the parallel score_breakdowns array
    * alongside candidates so build_focus_block can exercise the dedup-
    * uplift formula against the real (final_score) field. */
   focus_score_breakdown_t *breakdowns = calloc((size_t)s_focus.seeded_count, sizeof(*breakdowns));
   if (breakdowns == NULL) {
      free(arr);
      return FAILURE;
   }
   for (int i = 0; i < s_focus.seeded_count; i++) {
      arr[i].source_id = s_focus.seeded[i].source_id;
      arr[i].source_type = FOCUS_SOURCE_INTERNAL;
      /* semantic_score retained for back-compat (pre-1g-i tests read
       * it).  pb_focus_set_seed_score overrides; default 1.0 preserves
       * pre-1f test expectations. */
      arr[i].semantic_score = pb_focus_seed_score_for(i);
      arr[i].recency_score = 1.0f;
      arr[i].importance_score = 1.0f;
      arr[i].item_timestamp = 1700000000;
      arr[i].text = strdup(s_focus.seeded[i].text);
      arr[i].item_id = strdup(s_focus.seeded[i].item_id);
      if (arr[i].text == NULL || arr[i].item_id == NULL) {
         /* Mock OOM — clean up on failure path per contract. */
         for (int j = 0; j <= i; j++) {
            free(arr[j].text);
            free(arr[j].item_id);
         }
         free(arr);
         free(breakdowns);
         return FAILURE;
      }
      /* Default breakdown: final_score == seed score; contributions
       * default to the breakdown set by pb_focus_set_seed_breakdown
       * if any, otherwise a flat single-component populated to match.
       * The 1g-i intent test (final_score != semantic_score) overrides
       * via pb_focus_set_seed_breakdown. */
      breakdowns[i].semantic_contribution = pb_focus_seed_score_for(i);
      breakdowns[i].recency_contribution = 0.0f;
      breakdowns[i].importance_contribution = 0.0f;
      breakdowns[i].source_contribution = 0.0f;
      breakdowns[i].applied_source_weight = 1.0f;
      breakdowns[i].final_score = pb_focus_seed_final_score_for(i);
   }
   out_result->candidates = arr;
   out_result->candidate_count = s_focus.seeded_count;
   out_result->score_breakdowns = breakdowns;
   if (s_focus.rejection_count > 0) {
      out_result->rejection_count = 1;
      out_result->rejections[0].source_id = "memory_fact";
      out_result->rejections[0].count = s_focus.rejection_count;
   }
   return SUCCESS;
}

void focus_result_free(focus_compose_result_t *result) {
   if (result == NULL)
      return;
   if (result->candidates != NULL) {
      for (int i = 0; i < result->candidate_count; i++) {
         free(result->candidates[i].text);
         free(result->candidates[i].item_id);
      }
      free(result->candidates);
      result->candidates = NULL;
   }
   /* Phase 1g-i: free the parallel array.  Plain floats — no per-element
    * teardown.  Free unconditionally (paranoia: a path that allocated
    * one without the other would still get cleaned up). */
   free(result->score_breakdowns);
   result->score_breakdowns = NULL;
   result->candidate_count = 0;
   result->rejection_count = 0;
}

/* These are framework symbols that build_focus_block does NOT call but
 * the framework header declares.  Provide trivial stubs so the link
 * succeeds. */
int focus_register_source(const focus_source_adapter_t *adapter) {
   (void)adapter;
   return SUCCESS;
}

/* ----- programmable focus_compose stub: per-call score override ----------
 *
 * Phase 1f score-uplift formula needs the test to control final_score
 * per candidate per call.  Storage (`s_seed_scores`) is declared near
 * the top of this file alongside `s_focus` so `pb_focus_reset` can
 * scrub it; the public API lives down here next to the rest. */

void pb_focus_set_seed_score(int idx, float score) {
   if (idx < 0 || idx >= (int)(sizeof(s_seed_scores) / sizeof(s_seed_scores[0])))
      return;
   s_seed_scores[idx].override = true;
   s_seed_scores[idx].final_score = score;
}

/* 1g-i: independent final_score override for the uplift-base intent
 * test.  When unset, stub falls back to the legacy seed score so all
 * pre-1g-i tests stay byte-stable. */
void pb_focus_set_seed_final_score(int idx, float final_score) {
   if (idx < 0 || idx >= (int)(sizeof(s_seed_scores) / sizeof(s_seed_scores[0])))
      return;
   s_seed_scores[idx].final_override = true;
   s_seed_scores[idx].final_score_value = final_score;
}

void pb_focus_clear_seed_scores(void) {
   memset(s_seed_scores, 0, sizeof(s_seed_scores));
}

float pb_focus_seed_score_for(int idx) {
   if (idx < 0 || idx >= (int)(sizeof(s_seed_scores) / sizeof(s_seed_scores[0])))
      return 1.0f;
   return s_seed_scores[idx].override ? s_seed_scores[idx].final_score : 1.0f;
}

float pb_focus_seed_final_score_for(int idx) {
   if (idx < 0 || idx >= (int)(sizeof(s_seed_scores) / sizeof(s_seed_scores[0])))
      return 1.0f;
   if (s_seed_scores[idx].final_override)
      return s_seed_scores[idx].final_score_value;
   /* Legacy behavior: final_score tracks the seed (raw) score. */
   return s_seed_scores[idx].override ? s_seed_scores[idx].final_score : 1.0f;
}

/* ----- webui_broadcast_context_injection stub ----------------------------
 *
 * build_focus_block calls this when conv_id > 0; in unit tests we
 * record the last-call args + a per-test counter so tests can assert
 * "broadcast fired" / "broadcast skipped" / "args match expectation"
 * without linking the full webui_server.c (which pulls auth_db, conv_db,
 * libwebsockets, and the rest of the kitchen sink).
 *
 * The stub deliberately copies a SHALLOW snapshot — the result struct
 * itself stays owned by the caller; we just remember (count, scores)
 * for assertion purposes. */
typedef struct {
   int call_count;
   int last_user_id;
   int64_t last_conv_id;
   int64_t last_turn_id;
   int last_candidate_count;
   float last_first_final_score; /* 0 when last call had zero candidates */
   bool last_had_breakdowns;
} pb_broadcast_mock_t;

static pb_broadcast_mock_t s_broadcast = { 0 };

void pb_broadcast_reset(void) {
   memset(&s_broadcast, 0, sizeof(s_broadcast));
}

pb_broadcast_mock_t *pb_broadcast_state(void) {
   return &s_broadcast;
}

/* The session's stable citation handles (core/focus/focus_handles.c needs the
 * database): each call numbers its items 1, 2, 3, the old per-turn numbering. */
/* No conversation here: no tag secret to mask. */
char *session_prefix_mask_secret(struct session *session, char *text) {
   (void)session;
   return text;
}

int focus_handles_assign(struct session *session,
                         int64_t conv_id,
                         int user_id,
                         conv_focus_handle_t *items,
                         int count) {
   (void)session;
   (void)conv_id;
   (void)user_id;
   for (int i = 0; i < count; i++) {
      items[i].handle = i + 1;
      items[i].is_new = true;
   }
   return 0;
}

/* session_turn_conversation stub — build_focus_block calls this just before
 * the broadcast to re-read the dispatching turn's conversation (first-turn race
 * fix).  Tests do not publish a dispatch session, so
 * no session is passed (NULL) and this stub never fires — but
 * the symbol must resolve at link time. */
/* The previous question session_previous_question_dup gives, or NULL. */
const char *pb_previous_question = NULL;

char *session_previous_question_dup(struct session *session) {
   (void)session;
   return pb_previous_question ? strdup(pb_previous_question) : NULL;
}

int64_t session_turn_conversation(struct session *session) {
   (void)session;
   return 0;
}

void webui_broadcast_context_injection(int user_id,
                                       int64_t conv_id,
                                       int64_t turn_id,
                                       const focus_compose_result_t *result) {
   s_broadcast.call_count++;
   s_broadcast.last_user_id = user_id;
   s_broadcast.last_conv_id = conv_id;
   s_broadcast.last_turn_id = turn_id;
   s_broadcast.last_had_breakdowns = (result != NULL && result->score_breakdowns != NULL);
   if (result != NULL) {
      s_broadcast.last_candidate_count = result->candidate_count;
      if (result->candidate_count > 0 && result->score_breakdowns != NULL)
         s_broadcast.last_first_final_score = result->score_breakdowns[0].final_score;
      else
         s_broadcast.last_first_final_score = 0.0f;
   } else {
      s_broadcast.last_candidate_count = 0;
      s_broadcast.last_first_final_score = 0.0f;
   }
}

/* ----- session_t test fixtures -------------------------------------------
 *
 * Phase 1f+: tests need a real session_t with at least history_mutex
 * inited (session_dedup.c locks it via session_injected_set_clear).
 * Other session_t fields are zeroed and unused by build_focus_block. */

void pb_session_init(session_t *s, uint32_t session_id) {
   memset(s, 0, sizeof(*s));
   s->session_id = session_id;
   pthread_mutex_init(&s->history_mutex, NULL);
}

void pb_session_destroy(session_t *s) {
   pthread_mutex_destroy(&s->history_mutex);
   memset(s, 0, sizeof(*s));
}

/* ----- programmable embedding engine stub ------------------------------- */

typedef struct {
   bool available;
   int dims;
   bool fail_embed;
   int embed_call_count;
   char last_text[512]; /* the text last embedded */
} pb_embed_mock_t;

static pb_embed_mock_t s_embed = { 0 };

void pb_embed_reset(void) {
   memset(&s_embed, 0, sizeof(s_embed));
}

pb_embed_mock_t *pb_embed_state(void) {
   return &s_embed;
}

bool memory_embeddings_available(void) {
   return s_embed.available;
}

int memory_embeddings_dims(void) {
   return s_embed.available ? s_embed.dims : 0;
}

int memory_embeddings_embed(const char *text, float *out, int *out_dims) {
   snprintf(s_embed.last_text, sizeof(s_embed.last_text), "%s", text ? text : "");
   s_embed.embed_call_count++;
   if (s_embed.fail_embed)
      return FAILURE;
   for (int i = 0; i < s_embed.dims; i++)
      out[i] = (i == 0) ? 1.0f : 0.0f;
   if (out_dims != NULL)
      *out_dims = s_embed.dims;
   return SUCCESS;
}
