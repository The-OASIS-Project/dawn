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
 * Phase 1i.C bench binary — wraps build_focus_block end-to-end against a
 * fake-adapter synthetic memory pool, marshals the surfaced result to JSON
 * stdout for the multi-model fix-rate probe (benchmarks/bench_focus_fix_rate.py).
 *
 * Architecture (per coordinator pin C2 from 1i.C checkpoint):
 *   - Fake adapter (NOT in-memory SQLite).  Same pattern as 1i.B's
 *     test_focus_probe.c::fake_query — adapter emits the case's
 *     memory_seed array as focus_candidate_ts.
 *   - build_focus_block wrapper invoked, NOT raw focus_compose — dedup is
 *     part of the production failure mode being probed.
 *   - Synthetic embeddings derived from FNV-1a hash → deterministic
 *     xorshift float vector; no real model loads.  See synth_embed().
 *   - One case per process invocation (simplifies state management;
 *     matches the JSON-line bench protocol style).
 *
 * Wire schema:
 *
 *   FORWARD-COMPAT POLICY (M3): v1 consumers ignore unknown fields.
 *   Add fields freely without bumping major version.  Bump only on
 *   field rename / removal / type change.  The Python harness MUST
 *   tolerate stdout JSON with unknown keys silently.
 *
 *   STDIN — single JSON object:
 *   {
 *     "protocol_version": 1,
 *     "config": {
 *       "top_k": 8,                       -- focus_injection.top_k
 *       "min_score": 0.40,                -- focus_injection.min_score
 *       "focus_budget_bytes": 10240,      -- focus_injection.focus_budget_bytes
 *                                            ("focus_budget_tokens" accepted, x4)
 *       "weight_semantic": 1.0,
 *       "weight_recency":  0.30,
 *       "weight_importance": 0.20,
 *       "weight_source":   1.0,
 *       "source_weights": {
 *         "memory_fact":     1.0,
 *         "memory_entity":   0.9,
 *         "memory_relation": 0.85,
 *         "memory_summary":  0.7,
 *         "document_chunk":  0.7,
 *         "calendar_event":  0.6,
 *         "recent_email":    0.5,
 *         "dawn_background": 0.8
 *       },
 *       "dedup": {
 *         "recent_window_turns": 8,
 *         "score_uplift_factor": 1.5
 *       }
 *     },
 *     "memory_seed": [
 *       {
 *         "source_id":     "memory_fact",      -- one of the 8 known IDs
 *         "source_type":   "internal",         -- "internal"|"external"|"user-content"
 *         "text":          "...",
 *         "item_id":       "fact:1",           -- opaque, used for dedup keying
 *         "importance":    0.9,                -- 0..1, set by adapter
 *         "item_timestamp": 1700000000,         -- epoch seconds
 *         "semantic_override": null,            -- optional float; NULL→FNV-cosine
 *         "recency_override": null              -- optional float; NULL→exp decay
 *       }
 *     ],
 *     "query": "what's coming up?",
 *     "session_state": null                   -- reserved (v1: always null/absent;
 *                                                 prior-injection plumbing deferred)
 *   }
 *
 *   STDOUT — single JSON object on success:
 *   {
 *     "protocol_version": 1,
 *     "candidate_count": N,
 *     "candidates": [
 *       {
 *         "source_id": "...",
 *         "source_type": "internal",
 *         "text": "...",
 *         "item_id": "fact:1",
 *         "score": 0.78,
 *         "score_breakdown": {
 *           "semantic":   0.20,
 *           "recency":    0.18,
 *           "importance": 0.17,
 *           "source":     0.23,
 *           "applied_source_weight": 1.0,
 *           "final_score": 0.78
 *         }
 *       }
 *     ],
 *     "filter_rejections": [
 *       {"source_id": "...", "count": 1}
 *     ],
 *     "rendered_block": "[memory_fact] ...\n"
 *   }
 *
 *   STDERR — diagnostic messages.  Non-zero exit on hard failure
 *   (parse error, OOM, build_focus_block FAILURE).
 */

#include <json-c/json.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config/dawn_config.h"
#include "core/focus/focus_candidate_helpers.h"
#include "core/focus/focus_handles.h"
#include "core/focus/focus_incremental.h"
#include "core/focus/focus_source.h"
#include "core/focus/focus_source_internal.h"
#include "core/session_focus.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_embeddings.h"
#include "webui/build_focus_block.h"

/* The session a turn runs on: the prompt builder's argument (build_focus_block
 * reads the conversation's item handles from it). */
static session_t *s_dispatch;
static void set_dispatch(session_t *session) {
   s_dispatch = session;
}

/* =============================================================================
 * Synthetic embeddings — deterministic FNV-1a → xorshift float vector
 *
 * Produces the same 384-dim unit vector for the same input string every
 * call.  No model load, no GPU.  Probe correctness depends on this
 * being deterministic, NOT on it being a high-quality embedding —
 * cosine of two synthetic vectors is meaningful only when the two
 * inputs share text overlap (FNV will hash similarly under common
 * prefixes/substrings, but is fundamentally weaker than a real model).
 *
 * Tradeoff: cases that depend on TRUE semantic similarity (e.g.,
 * "wife" → "spouse") will not score well via FNV cosine.  Cases that
 * depend on TEXT OVERLAP (e.g., "Alice" → "Alice") will.  The synthetic
 * cases in benchmarks/focus_probe_cases.json are written with this in
 * mind — when a case needs a specific semantic_score, it sets the
 * `semantic_override` field on the seed.
 * ============================================================================= */

#define SYNTH_EMBED_DIM 384

static uint64_t fnv1a_64(const char *s) {
   uint64_t h = 0xcbf29ce484222325ULL;
   for (; *s != '\0'; s++) {
      h ^= (uint64_t)(unsigned char)*s;
      h *= 0x100000001b3ULL;
   }
   return h;
}

static uint64_t xorshift64(uint64_t *state) {
   uint64_t x = *state;
   x ^= x << 13;
   x ^= x >> 7;
   x ^= x << 17;
   *state = x;
   return x;
}

/* Fill `out` with SYNTH_EMBED_DIM unit-norm floats deterministically
 * derived from `text`.  Always produces the same vector for the same
 * input string.  Caller-provided buffer must hold SYNTH_EMBED_DIM floats. */
static void synth_embed(const char *text, float *out) {
   uint64_t state = fnv1a_64(text != NULL ? text : "");
   if (state == 0)
      state = 0xdeadbeefcafebabeULL; /* xorshift forbids zero seed */
   double norm_sq = 0.0;
   for (int i = 0; i < SYNTH_EMBED_DIM; i++) {
      uint64_t r = xorshift64(&state);
      /* Map to [-1, 1] via the high 32 bits as a signed int. */
      int32_t s = (int32_t)(r >> 32);
      float v = (float)s / 2147483648.0f;
      out[i] = v;
      norm_sq += (double)v * (double)v;
   }
   const double inv = (norm_sq > 0.0) ? (1.0 / sqrt(norm_sq)) : 0.0;
   for (int i = 0; i < SYNTH_EMBED_DIM; i++)
      out[i] *= (float)inv;
}

static float cosine(const float *a, const float *b, int dim) {
   double dot = 0.0;
   for (int i = 0; i < dim; i++)
      dot += (double)a[i] * (double)b[i];
   /* Both vectors are unit-norm by construction, so cosine == dot. */
   return (float)dot;
}

/* Recency decay — same shape as production focus_recency, but parameterless
 * here (deterministic 14-day half-life keeps cases readable). */
#define BENCH_RECENCY_HALF_LIFE_SEC (14 * 86400)
static float recency_decay(time_t item_ts, time_t now) {
   if (item_ts <= 0 || now <= 0 || item_ts > now)
      return 0.0f;
   const double age = (double)(now - item_ts);
   const double half = (double)BENCH_RECENCY_HALF_LIFE_SEC;
   /* 0.5 ^ (age / half_life). */
   const double decay = exp(-age * 0.69314718055994530942 / half);
   if (decay < 0.0)
      return 0.0f;
   if (decay > 1.0)
      return 1.0f;
   return (float)decay;
}

/* =============================================================================
 * Synthetic seed pool — populated by parse_seed_array from stdin JSON.
 * Static state because the fake adapter callback is invoked by
 * focus_compose without a context pointer.
 * ============================================================================= */

#define BENCH_SEED_CAP 64

typedef struct {
   const char *source_id; /* points into the strdup'd source-id string below */
   focus_source_type_t source_type;
   char *text;    /* HEAP — owned by seed_pool, copied into candidates */
   char *item_id; /* HEAP — same lifetime */
   time_t item_timestamp;
   float importance;
   bool semantic_override_set;
   float semantic_override;
   bool recency_override_set;
   float recency_override;
   float embedding[SYNTH_EMBED_DIM]; /* synth_embed(text) cache */
} bench_seed_t;

static struct {
   bench_seed_t seeds[BENCH_SEED_CAP];
   int seed_count;
   float query_embedding[SYNTH_EMBED_DIM];
   time_t now;
} s_pool;

/* M1 collapse: single source-of-truth for the 8 known source_ids +
 * per-source query callbacks lives in BENCH_SOURCES[] further down.
 * Adding a 9th source touches that table only.  This intern helper
 * walks BENCH_SOURCES so the strings have exactly one home. */
static const char *intern_known_source_id(const char *s); /* defined below */

static focus_source_type_t parse_source_type(const char *s) {
   if (s == NULL)
      return FOCUS_SOURCE_INTERNAL;
   if (strcmp(s, "external") == 0)
      return FOCUS_SOURCE_EXTERNAL;
   if (strcmp(s, "user-content") == 0)
      return FOCUS_SOURCE_USER_CONTENT;
   return FOCUS_SOURCE_INTERNAL;
}

static const char *source_type_str(focus_source_type_t t) {
   switch (t) {
      case FOCUS_SOURCE_INTERNAL:
         return "internal";
      case FOCUS_SOURCE_EXTERNAL:
         return "external";
      case FOCUS_SOURCE_USER_CONTENT:
         return "user-content";
      default:
         return "internal";
   }
}

/* =============================================================================
 * Fake adapter — emits the seed pool's candidates whose source_id matches
 * the registered adapter's source_id.  One adapter per source_id; the
 * bench registers one adapter per distinct source_id present in the seed
 * pool.  This mirrors production where each adapter owns one source.
 *
 * Per-source adapter records — we need ONE adapter struct per source_id
 * so focus_compose's lookup_source_weight resolves correctly.  Each
 * adapter's query thunks back into a per-source emit function via a
 * file-static cursor.
 *
 * To keep things simple: the fake adapter for source_id X emits all
 * seeds whose source_id matches X.  We use a unique callback PER
 * source so the callback knows which source_id it serves (closure via
 * function-pointer identity). */

#define BENCH_MAX_REGISTERED 8

static int bench_emit_for_source(const char *source_id,
                                 int max_candidates,
                                 focus_candidate_t **out_candidates,
                                 int *out_count) {
   int match = 0;
   for (int i = 0; i < s_pool.seed_count; i++) {
      if (strcmp(s_pool.seeds[i].source_id, source_id) == 0)
         match++;
   }
   *out_candidates = NULL;
   *out_count = 0;
   if (match == 0)
      return SUCCESS;

   const int emit = (match < max_candidates) ? match : max_candidates;
   focus_candidate_t *arr = calloc((size_t)emit, sizeof(*arr));
   if (arr == NULL)
      return FAILURE;

   bool warned = false;
   int j = 0;
   for (int i = 0; i < s_pool.seed_count && j < emit; i++) {
      bench_seed_t *seed = &s_pool.seeds[i];
      if (strcmp(seed->source_id, source_id) != 0)
         continue;

      /* Compute synthetic per-candidate scores (or use overrides). */
      const float semantic = seed->semantic_override_set
                                 ? seed->semantic_override
                                 : cosine(s_pool.query_embedding, seed->embedding, SYNTH_EMBED_DIM);
      const float recency = seed->recency_override_set
                                ? seed->recency_override
                                : recency_decay(seed->item_timestamp, s_pool.now);

      if (focus_candidate_init(&arr[j], seed->source_id,
                               /* source_type captured at register time;
                                * focus_compose overwrites with the adapter's
                                * registered source_type anyway, but pass the
                                * seed's value for any future framework
                                * change that respects the candidate's. */
                               seed->source_type, seed->text, seed->item_id, seed->item_timestamp,
                               semantic, recency, seed->importance, &warned) != SUCCESS) {
         focus_adapter_failure_cleanup(arr, j, out_candidates, out_count);
         return FAILURE;
      }
      j++;
   }

   *out_candidates = arr;
   *out_count = j;
   return SUCCESS;
}

/* One-callback-per-source.  Generated via macro to avoid hand-typing 8
 * near-identical functions; each callback closes over its source_id. */
#define DEFINE_SOURCE_QUERY(NAME, SOURCE_ID)                                                 \
   static int bench_query_##NAME(int user_id, bool include_private, const char *query_text,  \
                                 const float *query_embedding, size_t embed_dim, time_t now, \
                                 int max_candidates, focus_candidate_t **out_candidates,     \
                                 int *out_count) {                                           \
      (void)user_id;                                                                         \
      (void)include_private;                                                                 \
      (void)query_text;                                                                      \
      (void)query_embedding;                                                                 \
      (void)embed_dim;                                                                       \
      (void)now;                                                                             \
      return bench_emit_for_source(SOURCE_ID, max_candidates, out_candidates, out_count);    \
   }

DEFINE_SOURCE_QUERY(memory_fact, "memory_fact")
DEFINE_SOURCE_QUERY(memory_entity, "memory_entity")
DEFINE_SOURCE_QUERY(memory_relation, "memory_relation")
DEFINE_SOURCE_QUERY(memory_summary, "memory_summary")
DEFINE_SOURCE_QUERY(document_chunk, "document_chunk")
DEFINE_SOURCE_QUERY(calendar_event, "calendar_event")
DEFINE_SOURCE_QUERY(recent_email, "recent_email")
DEFINE_SOURCE_QUERY(dawn_background, "dawn_background")

/* M1: single dispatch table — string + query.  intern_known_source_id
 * walks this table; register_adapters_for_seed_pool walks this table.
 * Adding a 9th source means: add one DEFINE_SOURCE_QUERY line above
 * + one entry below.  No third site to update. */
typedef struct {
   const char *source_id;
   focus_source_query_fn query_fn;
} bench_source_entry_t;

static const bench_source_entry_t BENCH_SOURCES[] = {
   { "memory_fact", bench_query_memory_fact },
   { "memory_entity", bench_query_memory_entity },
   { "memory_relation", bench_query_memory_relation },
   { "memory_summary", bench_query_memory_summary },
   { "document_chunk", bench_query_document_chunk },
   { "calendar_event", bench_query_calendar_event },
   { "recent_email", bench_query_recent_email },
   { "dawn_background", bench_query_dawn_background },
};
#define BENCH_SOURCES_COUNT (sizeof(BENCH_SOURCES) / sizeof(BENCH_SOURCES[0]))

static const char *intern_known_source_id(const char *s) {
   if (s == NULL)
      return NULL;
   for (size_t i = 0; i < BENCH_SOURCES_COUNT; i++) {
      if (strcmp(s, BENCH_SOURCES[i].source_id) == 0)
         return BENCH_SOURCES[i].source_id;
   }
   return NULL;
}

static focus_source_adapter_t s_registered_adapters[BENCH_MAX_REGISTERED];
static int s_registered_count = 0;

/* Register one fake adapter per distinct source_id present in the seed
 * pool.  Each adapter uses its source-specific query thunk so source-
 * weight lookup works correctly.  Walks BENCH_SOURCES (single source-
 * of-truth for source_id + query_fn pairing — M1). */
static int register_adapters_for_seed_pool(void) {
   bool seen[BENCH_SOURCES_COUNT] = { false };
   int registered = 0;

   for (int i = 0; i < s_pool.seed_count && registered < BENCH_MAX_REGISTERED; i++) {
      const char *source_id = s_pool.seeds[i].source_id;
      int dispatch_idx = -1;
      for (size_t j = 0; j < BENCH_SOURCES_COUNT; j++) {
         if (strcmp(BENCH_SOURCES[j].source_id, source_id) == 0) {
            dispatch_idx = (int)j;
            break;
         }
      }
      if (dispatch_idx < 0) {
         fprintf(stderr, "bench_focus_pipeline: unknown source_id '%s' in seed pool\n", source_id);
         return FAILURE;
      }
      if (seen[dispatch_idx])
         continue;
      seen[dispatch_idx] = true;

      focus_source_adapter_t *a = &s_registered_adapters[registered];
      memset(a, 0, sizeof(*a));
      a->source_id = BENCH_SOURCES[dispatch_idx].source_id;
      a->source_type = s_pool.seeds[i].source_type;
      a->requires_embedding = false;
      a->query = BENCH_SOURCES[dispatch_idx].query_fn;
      if (focus_register_source(a) != SUCCESS) {
         fprintf(stderr, "bench_focus_pipeline: focus_register_source failed for '%s'\n",
                 source_id);
         return FAILURE;
      }
      registered++;
   }

   s_registered_count = registered;
   return SUCCESS;
}

/* =============================================================================
 * Capture-broadcast — the ONLY way to observe the post-dedup
 * focus_compose_result_t before build_focus_block frees it.  Stub
 * webui_broadcast_context_injection deep-copies the result into a
 * file-static struct; main() marshals to stdout after build_focus_block
 * returns.
 *
 * M2 note: this is a STRONG-SYMBOL replacement for the production
 * webui_broadcast_context_injection in src/webui/webui_server.c.  If
 * webui_server.c ever gets pulled into this bench's link list, the
 * linker will fail with a duplicate-symbol error — that's INTENTIONAL.
 * The bench owns this surface; co-existing with the production broadcast
 * (which would try to enqueue WebSocket frames to non-existent
 * connections) is not a viable fallback.
 * ============================================================================= */

typedef struct {
   bool fired;
   int user_id;
   int64_t conv_id;
   int64_t turn_id;
   int candidate_count;
   focus_candidate_t *candidates;             /* shallow heap copy */
   focus_score_breakdown_t *score_breakdowns; /* shallow heap copy */
   int rejection_count;
   focus_filter_rejection_t rejections[MAX_FOCUS_SOURCES];
} captured_t;

static captured_t s_captured;

static void captured_release(void) {
   if (s_captured.candidates != NULL) {
      for (int i = 0; i < s_captured.candidate_count; i++) {
         free(s_captured.candidates[i].text);
         free(s_captured.candidates[i].item_id);
      }
      free(s_captured.candidates);
   }
   free(s_captured.score_breakdowns);
   memset(&s_captured, 0, sizeof(s_captured));
}

/* The session's stable citation handles (core/focus/focus_handles.c needs the
 * database): each call numbers its items 1, 2, 3, the old per-turn numbering. */
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

/* Latent stub: build_focus_block.c calls session_turn_conversation() to
 * refresh the broadcast conv_id from a live dispatch pointer.  In the bench,
 * no session is passed (NULL), so this
 * stub never actually fires — but the symbol must resolve at link time.
 * Signature mirrors the declaration in include/core/session_manager.h.  Same
 * shape as tests/test_prompt_builder_stub.c. */
struct session;
char *session_previous_question_dup(struct session *session) {
   (void)session;
   return NULL;
}

int64_t session_turn_conversation(struct session *session) {
   (void)session;
   return 0;
}

void webui_broadcast_context_injection(int user_id,
                                       int64_t conv_id,
                                       int64_t turn_id,
                                       const focus_compose_result_t *result,
                                       const char *const *states) {
   (void)states;
   captured_release(); /* idempotent if first call */
   s_captured.fired = true;
   s_captured.user_id = user_id;
   s_captured.conv_id = conv_id;
   s_captured.turn_id = turn_id;
   if (result == NULL)
      return;
   s_captured.candidate_count = result->candidate_count;
   if (result->candidate_count > 0 && result->candidates != NULL) {
      s_captured.candidates = calloc((size_t)result->candidate_count, sizeof(focus_candidate_t));
      if (s_captured.candidates == NULL)
         return;
      s_captured.score_breakdowns = calloc((size_t)result->candidate_count,
                                           sizeof(focus_score_breakdown_t));
      if (s_captured.score_breakdowns == NULL) {
         free(s_captured.candidates);
         s_captured.candidates = NULL;
         return;
      }
      for (int i = 0; i < result->candidate_count; i++) {
         s_captured.candidates[i].source_id = result->candidates[i].source_id;
         s_captured.candidates[i].source_type = result->candidates[i].source_type;
         s_captured.candidates[i].semantic_score = result->candidates[i].semantic_score;
         s_captured.candidates[i].recency_score = result->candidates[i].recency_score;
         s_captured.candidates[i].importance_score = result->candidates[i].importance_score;
         s_captured.candidates[i].item_timestamp = result->candidates[i].item_timestamp;
         /* H2: strdup NULL-check.  Silent NULL would skew judge verdicts —
          * substring match against an empty rendered_block reads as "context
          * didn't surface this item" (false PASS for failure cases, false
          * FAIL for negative-empty).  Fail-closed: release any partial
          * capture, clear candidate_count so stdout marshalling reports
          * zero, leave fired = true so the harness can distinguish
          * "broadcast fired with OOM" from "broadcast skipped due to
          * conv_id == 0". */
         if (result->candidates[i].text != NULL) {
            s_captured.candidates[i].text = strdup(result->candidates[i].text);
            if (s_captured.candidates[i].text == NULL) {
               captured_release();
               s_captured.fired = true;
               s_captured.user_id = user_id;
               s_captured.conv_id = conv_id;
               s_captured.turn_id = turn_id;
               return;
            }
         }
         if (result->candidates[i].item_id != NULL) {
            s_captured.candidates[i].item_id = strdup(result->candidates[i].item_id);
            if (s_captured.candidates[i].item_id == NULL) {
               captured_release();
               s_captured.fired = true;
               s_captured.user_id = user_id;
               s_captured.conv_id = conv_id;
               s_captured.turn_id = turn_id;
               return;
            }
         }
         s_captured.candidates[i].provenance = result->candidates[i].provenance;
         if (result->score_breakdowns != NULL)
            s_captured.score_breakdowns[i] = result->score_breakdowns[i];
      }
   }
   s_captured.rejection_count = result->rejection_count;
   for (int i = 0; i < result->rejection_count && i < MAX_FOCUS_SOURCES; i++) {
      s_captured.rejections[i] = result->rejections[i];
   }
}

/* =============================================================================
 * memory_embeddings stubs — return SYNTH_EMBED_DIM, embed via synth_embed.
 * build_focus_block calls these BEFORE focus_compose; the embedding is
 * passed through to adapters.  Our adapters compute their own per-
 * candidate cosine in bench_emit_for_source via cached embeddings — the
 * embedding from the framework is unused but must be plausibly populated.
 * ============================================================================= */

bool memory_embeddings_available(void) {
   return true;
}

int memory_embeddings_dims(void) {
   return SYNTH_EMBED_DIM;
}

int memory_embeddings_embed(const char *text, float *out, int *out_dims) {
   if (out == NULL)
      return FAILURE;
   synth_embed(text != NULL ? text : "", out);
   if (out_dims != NULL)
      *out_dims = SYNTH_EMBED_DIM;
   return SUCCESS;
}

/* g_config — single owner, populated from JSON config block before build. */
dawn_config_t g_config;

/* =============================================================================
 * Log-capture for dedup_suppressed
 *
 * The builder no longer drops items it sent on earlier turns (a turn's seam
 * decides that, against the conversation), so its summary line carries no
 * "dedup_suppressed=N" and the field reads 0.  The capture and the field stay
 * so the probe scripts that read them keep working.
 * ============================================================================= */

static char s_log_path[64];

/* Open a per-pid temp file and route the daemon logger into it.  Console
 * remains suppressed; log lines go to s_log_path.  Returns SUCCESS on
 * success; on failure leaves logging suppressed but unrouted. */
static int log_capture_open(void) {
   const int n = snprintf(s_log_path, sizeof(s_log_path), "/tmp/bench_focus_pipeline_%d.log",
                          (int)getpid());
   if (n < 0 || (size_t)n >= sizeof(s_log_path)) {
      s_log_path[0] = '\0';
      return FAILURE;
   }
   if (init_logging(s_log_path, LOG_TO_FILE) != 0) {
      s_log_path[0] = '\0';
      return FAILURE;
   }
   return SUCCESS;
}

/* Close + read the captured log file, parse `dedup_suppressed=N` from
 * any line that contains it (typically the privacy-safe summary line
 * from log_focus_summary in build_focus_block.c).  Returns the integer
 * on success, 0 if no marker found.  Always unlinks the temp file. */
static int log_capture_extract_dedup_suppressed(void) {
   if (s_log_path[0] == '\0')
      return 0;
   close_logging();

   int suppressed = 0;
   FILE *f = fopen(s_log_path, "r");
   if (f != NULL) {
      char line[1024];
      while (fgets(line, sizeof(line), f) != NULL) {
         const char *p = strstr(line, "dedup_suppressed=");
         if (p == NULL)
            continue;
         /* %n%*c absorbs the trailing space/tab; bare %d is sufficient
          * since the value is whitespace-terminated in production. */
         int v = 0;
         if (sscanf(p + sizeof("dedup_suppressed=") - 1, "%d", &v) == 1) {
            suppressed = v;
         }
      }
      fclose(f);
   }
   unlink(s_log_path);
   s_log_path[0] = '\0';
   return suppressed;
}

/* =============================================================================
 * stdin parsing
 * ============================================================================= */

#define BENCH_STDIN_MAX (4 * 1024 * 1024)

static char *slurp_stdin(size_t *out_len) {
   size_t cap = 65536;
   char *buf = malloc(cap);
   if (buf == NULL)
      return NULL;
   size_t len = 0;
   while (1) {
      if (len + 1 >= cap) {
         if (cap >= BENCH_STDIN_MAX) {
            free(buf);
            return NULL;
         }
         cap *= 2;
         char *resized = realloc(buf, cap);
         if (resized == NULL) {
            free(buf);
            return NULL;
         }
         buf = resized;
      }
      size_t chunk = fread(buf + len, 1, cap - 1 - len, stdin);
      len += chunk;
      if (chunk == 0)
         break;
   }
   buf[len] = '\0';
   *out_len = len;
   return buf;
}

static double json_get_double(struct json_object *obj, const char *key, double fallback) {
   struct json_object *v = NULL;
   if (!json_object_object_get_ex(obj, key, &v))
      return fallback;
   if (json_object_is_type(v, json_type_null))
      return fallback;
   return json_object_get_double(v);
}

static int json_get_int(struct json_object *obj, const char *key, int fallback) {
   struct json_object *v = NULL;
   if (!json_object_object_get_ex(obj, key, &v))
      return fallback;
   if (json_object_is_type(v, json_type_null))
      return fallback;
   return json_object_get_int(v);
}

static const char *json_get_string(struct json_object *obj, const char *key) {
   struct json_object *v = NULL;
   if (!json_object_object_get_ex(obj, key, &v))
      return NULL;
   if (json_object_is_type(v, json_type_null))
      return NULL;
   return json_object_get_string(v);
}

/* M4: bounds for untrusted JSON config fields.  Match production
 * config_validate.c clamps where they exist; pick conservative caps
 * for everything else.  Out-of-range values fail closed with stderr
 * + non-zero exit at the parse boundary.
 *
 * The bench reads stdin from a Python harness — the harness is
 * trusted, but a bug in case-authoring or a corrupted case file
 * shouldn't drive build_focus_block into pathological configurations
 * (e.g. negative top_k → focus_compose silent no-op masquerading as
 * a real result). */
#define BENCH_TOP_K_MIN 1
#define BENCH_TOP_K_MAX 64
/* Same range config_validate.c enforces on the production knob. */
#define BENCH_BUDGET_BYTES_MIN 1024
#define BENCH_BUDGET_BYTES_MAX 65536
#define BENCH_WEIGHT_MIN 0.0f
#define BENCH_WEIGHT_MAX 10.0f

static int validate_int_range(const char *field, int v, int lo, int hi) {
   if (v < lo || v > hi) {
      fprintf(stderr, "bench_focus_pipeline: '%s' = %d out of range [%d, %d]\n", field, v, lo, hi);
      return FAILURE;
   }
   return SUCCESS;
}

static int validate_float_range(const char *field, float v, float lo, float hi) {
   if (!(v >= lo) || !(v <= hi)) { /* NaN-safe */
      fprintf(stderr, "bench_focus_pipeline: '%s' = %g out of range [%g, %g]\n", field, (double)v,
              (double)lo, (double)hi);
      return FAILURE;
   }
   return SUCCESS;
}

static int parse_config(struct json_object *cfg) {
   focus_injection_config_t *fi = &g_config.memory.focus_injection;
   /* Production defaults; JSON overrides any field present. */
   fi->enabled = true;
   fi->top_k = json_get_int(cfg, "top_k", 8);
   fi->min_score = (float)json_get_double(cfg, "min_score", 0.4);
   /* The budget is bytes; "focus_budget_tokens" is the deprecated estimated-token
    * name, converted x4 exactly as config_parser.c does for dawn.toml. */
   fi->focus_budget_bytes = json_get_int(cfg, "focus_budget_bytes", 10240);
   struct json_object *legacy_budget = NULL;
   if (!json_object_object_get_ex(cfg, "focus_budget_bytes", NULL) &&
       json_object_object_get_ex(cfg, "focus_budget_tokens", &legacy_budget))
      fi->focus_budget_bytes = json_object_get_int(legacy_budget) * 4;
   fi->weight_semantic = (float)json_get_double(cfg, "weight_semantic", 1.0);
   fi->weight_recency = (float)json_get_double(cfg, "weight_recency", 0.3);
   fi->weight_importance = (float)json_get_double(cfg, "weight_importance", 0.2);
   fi->weight_source = (float)json_get_double(cfg, "weight_source", 1.0);
   fi->classifier_enabled = false;

   /* M4: clamp untrusted fields. */
   if (validate_int_range("top_k", fi->top_k, BENCH_TOP_K_MIN, BENCH_TOP_K_MAX) != SUCCESS)
      return FAILURE;
   if (validate_int_range("focus_budget_bytes", fi->focus_budget_bytes, BENCH_BUDGET_BYTES_MIN,
                          BENCH_BUDGET_BYTES_MAX) != SUCCESS)
      return FAILURE;
   if (validate_float_range("min_score", fi->min_score, 0.0f, 1.0f) != SUCCESS)
      return FAILURE;
   if (validate_float_range("weight_semantic", fi->weight_semantic, BENCH_WEIGHT_MIN,
                            BENCH_WEIGHT_MAX) != SUCCESS)
      return FAILURE;
   if (validate_float_range("weight_recency", fi->weight_recency, BENCH_WEIGHT_MIN,
                            BENCH_WEIGHT_MAX) != SUCCESS)
      return FAILURE;
   if (validate_float_range("weight_importance", fi->weight_importance, BENCH_WEIGHT_MIN,
                            BENCH_WEIGHT_MAX) != SUCCESS)
      return FAILURE;
   if (validate_float_range("weight_source", fi->weight_source, BENCH_WEIGHT_MIN,
                            BENCH_WEIGHT_MAX) != SUCCESS)
      return FAILURE;

   /* Default per-source weights match dawn.toml.example. */
   fi->source_weights.memory_fact = 1.0f;
   fi->source_weights.memory_entity = 0.9f;
   fi->source_weights.memory_relation = 0.85f;
   fi->source_weights.memory_summary = 0.7f;
   fi->source_weights.document_chunk = 0.7f;
   fi->source_weights.calendar_event = 0.6f;
   fi->source_weights.recent_email = 0.5f;
   fi->source_weights.dawn_background = 0.8f;

   struct json_object *sw = NULL;
   if (json_object_object_get_ex(cfg, "source_weights", &sw)) {
      fi->source_weights.memory_fact = (float)json_get_double(sw, "memory_fact", 1.0);
      fi->source_weights.memory_entity = (float)json_get_double(sw, "memory_entity", 0.9);
      fi->source_weights.memory_relation = (float)json_get_double(sw, "memory_relation", 0.85);
      fi->source_weights.memory_summary = (float)json_get_double(sw, "memory_summary", 0.7);
      fi->source_weights.document_chunk = (float)json_get_double(sw, "document_chunk", 0.7);
      fi->source_weights.calendar_event = (float)json_get_double(sw, "calendar_event", 0.6);
      fi->source_weights.recent_email = (float)json_get_double(sw, "recent_email", 0.5);
      fi->source_weights.dawn_background = (float)json_get_double(sw, "dawn_background", 0.8);
   }

   /* A "dedup" object is accepted and ignored: which items a turn sends is
    * decided against the conversation at the turn's seam, not here. */

   /* Dominant-token over-inclusion heuristic — Phase B-ii.  Mirrors
    * production defaults (config_defaults.c): enabled=true,
    * threshold=0.60, base_penalty=0.40.  Bench callers override via the
    * "dominant_token_heuristic" sub-object in the focus config JSON. */
   struct json_object *dth = NULL;
   fi->dominant_token_heuristic.enabled = true;
   fi->dominant_token_heuristic.threshold = 0.60f;
   fi->dominant_token_heuristic.base_penalty = 0.40f;
   if (json_object_object_get_ex(cfg, "dominant_token_heuristic", &dth)) {
      struct json_object *enabled_obj = NULL;
      if (json_object_object_get_ex(dth, "enabled", &enabled_obj) &&
          json_object_is_type(enabled_obj, json_type_boolean)) {
         fi->dominant_token_heuristic.enabled = json_object_get_boolean(enabled_obj);
      }
      fi->dominant_token_heuristic.threshold = (float)json_get_double(dth, "threshold", 0.60);
      fi->dominant_token_heuristic.base_penalty = (float)json_get_double(dth, "base_penalty", 0.40);
   }
   if (validate_float_range("dominant_token_heuristic.threshold",
                            fi->dominant_token_heuristic.threshold, 0.0f, 1.0f) != SUCCESS)
      return FAILURE;
   if (validate_float_range("dominant_token_heuristic.base_penalty",
                            fi->dominant_token_heuristic.base_penalty, 0.0f, 1.0f) != SUCCESS)
      return FAILURE;
   return SUCCESS;
}

static int parse_seed_array(struct json_object *seeds_arr, const char *query) {
   if (!json_object_is_type(seeds_arr, json_type_array)) {
      fprintf(stderr, "bench_focus_pipeline: 'memory_seed' must be a JSON array\n");
      return FAILURE;
   }
   const int n = json_object_array_length(seeds_arr);
   /* M6: defend against negative casts on malformed JSON.
    * json_object_array_length returns int — a corrupt array could
    * conceivably yield a negative value, which would skip the upper-
    * bound check below and run the parse loop with a wraparound count. */
   if (n < 0 || n > BENCH_SEED_CAP) {
      fprintf(stderr,
              "bench_focus_pipeline: seed pool length %d out of range [0, %d] "
              "(bump cap or split case)\n",
              n, BENCH_SEED_CAP);
      return FAILURE;
   }

   /* Pre-compute query embedding once. */
   synth_embed(query != NULL ? query : "", s_pool.query_embedding);

   for (int i = 0; i < n; i++) {
      struct json_object *seed = json_object_array_get_idx(seeds_arr, i);
      const char *raw_source = json_get_string(seed, "source_id");
      const char *interned = intern_known_source_id(raw_source);
      if (interned == NULL) {
         fprintf(stderr,
                 "bench_focus_pipeline: seed[%d] has unknown source_id %s "
                 "(must be one of memory_fact/memory_entity/.../dawn_background)\n",
                 i, raw_source != NULL ? raw_source : "(null)");
         return FAILURE;
      }

      bench_seed_t *s = &s_pool.seeds[i];
      memset(s, 0, sizeof(*s));
      s->source_id = interned;
      s->source_type = parse_source_type(json_get_string(seed, "source_type"));

      const char *text = json_get_string(seed, "text");
      if (text == NULL || text[0] == '\0') {
         fprintf(stderr, "bench_focus_pipeline: seed[%d] missing 'text'\n", i);
         return FAILURE;
      }
      s->text = strdup(text);
      if (s->text == NULL)
         return FAILURE;

      const char *iid = json_get_string(seed, "item_id");
      s->item_id = strdup(iid != NULL ? iid : "");
      if (s->item_id == NULL)
         return FAILURE;

      /* M5: read item_timestamp as int64 so values past 2038-01-19
       * round-trip on 32-bit time_t platforms.  time_t retains its
       * semantic; the JSON read uses the 64-bit accessor. */
      {
         struct json_object *ts_obj = NULL;
         if (json_object_object_get_ex(seed, "item_timestamp", &ts_obj) &&
             !json_object_is_type(ts_obj, json_type_null)) {
            s->item_timestamp = (time_t)json_object_get_int64(ts_obj);
         } else {
            s->item_timestamp = 0;
         }
      }
      s->importance = (float)json_get_double(seed, "importance", 0.5);
      if (validate_float_range("seed.importance", s->importance, 0.0f, 1.0f) != SUCCESS)
         return FAILURE;

      struct json_object *override_v = NULL;
      if (json_object_object_get_ex(seed, "semantic_override", &override_v) &&
          !json_object_is_type(override_v, json_type_null)) {
         s->semantic_override_set = true;
         s->semantic_override = (float)json_object_get_double(override_v);
      }
      if (json_object_object_get_ex(seed, "recency_override", &override_v) &&
          !json_object_is_type(override_v, json_type_null)) {
         s->recency_override_set = true;
         s->recency_override = (float)json_object_get_double(override_v);
      }

      synth_embed(s->text, s->embedding);
   }

   s_pool.seed_count = n;
   return SUCCESS;
}

static void seed_pool_release(void) {
   for (int i = 0; i < s_pool.seed_count; i++) {
      free(s_pool.seeds[i].text);
      free(s_pool.seeds[i].item_id);
   }
   memset(&s_pool, 0, sizeof(s_pool));
}

/* =============================================================================
 * stdout marshalling
 * ============================================================================= */

static void emit_json_result(const char *rendered_block, int dedup_suppressed) {
   struct json_object *root = json_object_new_object();
   /* M3: forward-compat policy — v1 consumers ignore unknown fields.
    * Add fields freely without bumping major version.  Bump only on
    * field rename / removal / type change. */
   json_object_object_add(root, "protocol_version", json_object_new_int(1));
   json_object_object_add(root, "candidate_count", json_object_new_int(s_captured.candidate_count));
   /* H3: explicit broadcast-fired signal so harness distinguishes
    * "build_focus_block ran with conv_id > 0 and broadcast happened" from
    * "build_focus_block short-circuited or broadcast skipped".  In v1
    * the bench rejects conv_id <= 0 at parse time so this is always
    * true on a successful run; field exists for forward-compat with
    * future SESSION_START schema. */
   json_object_object_add(root, "broadcast_fired", json_object_new_boolean(s_captured.fired));
   /* H4: dedup_suppressed surfaced from the privacy-safe summary log
    * line build_focus_block emits.  0 when no dedup ran (no prior
    * injections) or when log capture failed to open the temp file. */
   json_object_object_add(root, "dedup_suppressed", json_object_new_int(dedup_suppressed));

   struct json_object *items = json_object_new_array();
   for (int i = 0; i < s_captured.candidate_count; i++) {
      const focus_candidate_t *c = &s_captured.candidates[i];
      const focus_score_breakdown_t *b = &s_captured.score_breakdowns[i];
      struct json_object *item = json_object_new_object();
      json_object_object_add(item, "source_id",
                             json_object_new_string(c->source_id != NULL ? c->source_id : ""));
      json_object_object_add(item, "source_type",
                             json_object_new_string(source_type_str(c->source_type)));
      json_object_object_add(item, "text", json_object_new_string(c->text != NULL ? c->text : ""));
      json_object_object_add(item, "item_id",
                             json_object_new_string(c->item_id != NULL ? c->item_id : ""));
      json_object_object_add(item, "score", json_object_new_double((double)b->final_score));

      struct json_object *bd = json_object_new_object();
      json_object_object_add(bd, "semantic",
                             json_object_new_double((double)b->semantic_contribution));
      json_object_object_add(bd, "recency",
                             json_object_new_double((double)b->recency_contribution));
      json_object_object_add(bd, "importance",
                             json_object_new_double((double)b->importance_contribution));
      json_object_object_add(bd, "source", json_object_new_double((double)b->source_contribution));
      json_object_object_add(bd, "applied_source_weight",
                             json_object_new_double((double)b->applied_source_weight));
      json_object_object_add(bd, "final_score", json_object_new_double((double)b->final_score));
      json_object_object_add(item, "score_breakdown", bd);

      json_object_array_add(items, item);
   }
   json_object_object_add(root, "candidates", items);

   struct json_object *rejections = json_object_new_array();
   for (int i = 0; i < s_captured.rejection_count; i++) {
      const focus_filter_rejection_t *r = &s_captured.rejections[i];
      if (r->source_id == NULL || r->count <= 0)
         continue;
      struct json_object *rej = json_object_new_object();
      json_object_object_add(rej, "source_id", json_object_new_string(r->source_id));
      json_object_object_add(rej, "count", json_object_new_int(r->count));
      json_object_array_add(rejections, rej);
   }
   json_object_object_add(root, "filter_rejections", rejections);

   json_object_object_add(root, "rendered_block",
                          json_object_new_string(rendered_block != NULL ? rendered_block : ""));

   const char *out = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);
   fputs(out, stdout);
   fputc('\n', stdout);
   json_object_put(root);
}

/* =============================================================================
 * main
 * ============================================================================= */

int main(int argc, char *argv[]) {
   (void)argc;
   (void)argv;

   /* Silence the daemon logger's stdout output — the wire protocol uses
    * stdout for the JSON result, and OLOG_INFO calls inside focus_source.c
    * + build_focus_block.c would otherwise contaminate it.  Errors still
    * surface via fprintf(stderr, ...) at our framing level.  We pair this
    * with log_capture_open() which routes all daemon-logger output to a
    * temp file for dedup_suppressed extraction (H4). */
   logging_suppress_console(1);
   if (log_capture_open() != SUCCESS) {
      fprintf(stderr, "bench_focus_pipeline: warning: log capture unavailable; "
                      "dedup_suppressed will report 0\n");
   }

   memset(&g_config, 0, sizeof(g_config));
   memset(&s_pool, 0, sizeof(s_pool));
   memset(&s_captured, 0, sizeof(s_captured));

   size_t in_len = 0;
   char *in_buf = slurp_stdin(&in_len);
   if (in_buf == NULL || in_len == 0) {
      fprintf(stderr, "bench_focus_pipeline: empty or oversized stdin\n");
      free(in_buf);
      return 1;
   }

   struct json_object *root = json_tokener_parse(in_buf);
   free(in_buf);
   if (root == NULL) {
      fprintf(stderr, "bench_focus_pipeline: invalid JSON on stdin\n");
      return 1;
   }

   const int proto = json_get_int(root, "protocol_version", 0);
   if (proto != 1) {
      fprintf(stderr, "bench_focus_pipeline: protocol_version %d unsupported (expected 1)\n",
              proto);
      json_object_put(root);
      return 1;
   }

   struct json_object *cfg = NULL;
   if (!json_object_object_get_ex(root, "config", &cfg)) {
      fprintf(stderr, "bench_focus_pipeline: missing 'config' object\n");
      json_object_put(root);
      return 1;
   }
   if (parse_config(cfg) != SUCCESS) {
      json_object_put(root);
      return 1;
   }

   const char *query = json_get_string(root, "query");
   if (query == NULL || query[0] == '\0') {
      fprintf(stderr, "bench_focus_pipeline: missing or empty 'query'\n");
      json_object_put(root);
      return 1;
   }

   /* `now` defaults to a fixed test epoch so synthetic recency_decay is
    * deterministic across runs.  Override via stdin "now" field.
    * M5: read via int64 so values past 2038-01-19 round-trip on 32-bit
    * time_t platforms (Jetson is 64-bit but the wire schema must be
    * forward-compatible with any future 32-bit consumer). */
   {
      struct json_object *now_obj = NULL;
      if (json_object_object_get_ex(root, "now", &now_obj) &&
          !json_object_is_type(now_obj, json_type_null)) {
         s_pool.now = (time_t)json_object_get_int64(now_obj);
      } else {
         s_pool.now = (time_t)1700000000;
      }
   }

   /* H3: conv_id of 0 disables the broadcast inside build_focus_block —
    * stdout would marshal an all-zeros captured result indistinguishable
    * from a hard failure.  Reject at the parse boundary so SESSION_START
    * semantics (conv_id == 0) require an explicit future schema bump. */
   const int64_t conv_id = (int64_t)json_get_int(root, "conv_id", 1);
   if (conv_id <= 0) {
      fprintf(stderr,
              "bench_focus_pipeline: 'conv_id' must be > 0 (got %lld); "
              "SESSION_START semantics not yet supported\n",
              (long long)conv_id);
      json_object_put(root);
      return 1;
   }
   const int64_t turn_id = (int64_t)json_get_int(root, "turn_id", 1);
   const int user_id = json_get_int(root, "user_id", 1);

   struct json_object *seeds_arr = NULL;
   if (!json_object_object_get_ex(root, "memory_seed", &seeds_arr)) {
      fprintf(stderr, "bench_focus_pipeline: missing 'memory_seed' array\n");
      json_object_put(root);
      return 1;
   }
   if (parse_seed_array(seeds_arr, query) != SUCCESS) {
      seed_pool_release();
      json_object_put(root);
      return 1;
   }

   focus_unregister_all();
   if (register_adapters_for_seed_pool() != SUCCESS) {
      focus_unregister_all();
      seed_pool_release();
      json_object_put(root);
      return 1;
   }

   /* Set up a dispatch session so build_focus_block's apply_dedup_locked
    * has a place to record per-turn injections.  v1 always starts from a
    * clean session; session_state plumbing for prior injections is
    * deferred to v2. */
   session_t s;
   memset(&s, 0, sizeof(s));
   s.session_id = 1;
   pthread_mutex_init(&s.history_mutex, NULL);
   set_dispatch(&s);

   /* The turn's items, rendered as a conversation that shows none of them
    * yet would send them; conv_id > 0 (validated above) so the panel hook
    * fires the broadcast we capture. */
   composed_prompt_t cp = { 0 };
   const int rc = build_focus_block(s_dispatch, user_id, conv_id, turn_id, query, &cp);
   char *block = NULL;
   if (rc == SUCCESS) {
      focus_scan_t none = { 0 };
      focus_selection_t sel;
      if (focus_incremental_select(cp.focus_items, cp.n_focus_items, NULL, &none, &sel) == 0) {
         block = focus_incremental_render(cp.focus_items, &sel, false);
      }
      focus_selection_free(&sel);
      session_focus_client_notice(s_dispatch, &cp, NULL, 0);
   }
   composed_prompt_free(&cp);

   /* H4: extract dedup_suppressed from the captured log file.  Must run
    * BEFORE emit_json_result because the JSON shape carries the value. */
   const int dedup_suppressed = log_capture_extract_dedup_suppressed();

   const int exit_code = (rc == SUCCESS) ? 0 : 2;
   if (rc == SUCCESS) {
      emit_json_result(block != NULL ? block : "", dedup_suppressed);
   } else {
      fprintf(stderr, "bench_focus_pipeline: build_focus_block returned FAILURE\n");
   }

   free(block);
   set_dispatch(NULL);
   pthread_mutex_destroy(&s.history_mutex);
   focus_unregister_all();
   captured_release();
   seed_pool_release();
   json_object_put(root);
   return exit_code;
}
