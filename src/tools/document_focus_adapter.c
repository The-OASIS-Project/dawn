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
 * Document chunk focus adapter (per-turn context injection).
 *
 * source_id          = "document_chunk"
 * source_type        = FOCUS_SOURCE_EXTERNAL
 * requires_embedding = true
 *
 * Pipeline:
 *   1. Rank every chunk accessible to user_id (own + shared) by cosine to
 *      query_embedding (document_embed_rank, from the in-memory embedding copy),
 *      keeping the top max_candidates plus the pool's cosine mean.
 *   2. Drop chunks not clearly above the corpus-typical similarity
 *      (document_min_relevance).
 *   3. Fetch text + filename for the rest via document_db_chunks_get_by_ids
 *      (one JOIN, no per-chunk N+1).
 *   4. Render "[<filename>] <chunk_text>" through focus_candidate_init
 *      which truncates to FOCUS_TEXT_MAX_BYTES.
 *
 * Memory shape: the ranker returns only (id, cosine) pairs; the chunk structs
 * (~5 KB each) are heap-allocated for the top hits only.
 *
 * Provenance: {0,0,0} sentinel — documents have no conv-based source
 * linkage; the WebUI surfaces filename via the rendered text.
 *
 * No network calls: ranking and fetch are in-memory + SQLite only.
 *
 * Filter-on-retrieval is FRAMEWORK-OWNED + trust-tier-gated.  This
 * adapter does NOT call `memory_filter_check()` — `focus_compose()`
 * decides based on `source_type`.  Document chunks are
 * FOCUS_SOURCE_EXTERNAL (user-uploaded, trusted) and pass through
 * without filtering.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config/dawn_config.h"
#include "core/embedding_engine.h"
#include "core/focus/focus_candidate_helpers.h"
#include "core/focus/focus_recency.h"
#include "core/focus/focus_source.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_embeddings.h"
#include "tools/document_db.h"
#include "tools/document_embed_cache.h"

/* Constants — file-static, all TODO(1j) for bench-driven tuning. */

/* Importance score for every document chunk in v1.  Documents lack a
 * confidence-style intrinsic per-chunk signal (unlike facts); 0.5
 * keeps documents below curated memory facts (1.0) while above
 * future low-importance sources.  TODO(1j). */
#define DOCUMENT_DEFAULT_IMPORTANCE 0.5f

/* Most candidates this adapter returns (callers pass top_k, normally <= 32). */
#define DOCUMENT_TOP_CAP 64

static int document_adapter_query(int user_id,
                                  bool include_private,
                                  const char *query_text,
                                  const float *query_embedding,
                                  size_t embed_dim,
                                  time_t now,
                                  int max_candidates,
                                  focus_candidate_t **out_candidates,
                                  int *out_count) {
   (void)include_private; /* ranking covers own docs + shared ones; documents
                             have no link to a private conversation. */
   (void)query_text;      /* Vector-only adapter; query_text consumed
                             upstream to compute query_embedding. */
   *out_candidates = NULL;
   *out_count = 0;
   if (max_candidates <= 0 || query_embedding == NULL || embed_dim == 0 ||
       !memory_embeddings_available())
      return SUCCESS;

   const int dims = (int)embed_dim;
   if (dims <= 0)
      return SUCCESS;

   /* Rank EVERY accessible chunk by cosine and keep the top `keep`; the same
    * pass gives the pool's cosine mean.  (Loading a fixed number of chunks in
    * scan order and ranking only those left most of a larger corpus
    * unconsidered.) */
   const int keep = (max_candidates > DOCUMENT_TOP_CAP) ? DOCUMENT_TOP_CAP : max_candidates;
   document_chunk_score_t top[DOCUMENT_TOP_CAP];
   int n_top = 0;
   document_rank_stats_t stats;

   document_chunk_t *chunks = NULL;
   focus_candidate_t *out = NULL;
   int rc = SUCCESS;
   int produced = 0;

   if (document_embed_rank(user_id, query_embedding, dims, keep, top, &n_top, &stats) != SUCCESS) {
      OLOG_ERROR("document_adapter: chunk ranking failed (user_id=%d)", user_id);
      return FAILURE;
   }
   if (n_top == 0) {
      return SUCCESS; /* zero candidates */
   }

   /* Relevance gate.  Embedding models put unrelated text at a model-specific
    * baseline similarity (bge-small ~0.43, MiniLM ~0.1), so a raw cosine floor
    * doesn't transfer between models and the ranker's final score (which adds
    * constant source/importance priors) never gated at all: unrelated chunks were
    * injected on every turn.  Measure each chunk from the corpus-typical level
    * instead, relevance = (cos - pool_mean) / (1 - pool_mean), and keep only
    * those clearly above it.  A small corpus gives no meaningful baseline, so the
    * gate needs EMBEDDING_RELEVANCE_MIN_POOL chunks. */
   const float min_rel = g_config.memory.focus_injection.document_min_relevance;
   if (min_rel > 0.0f && stats.pool >= EMBEDDING_RELEVANCE_MIN_POOL) {
      int kept_n = 0;
      for (int i = 0; i < n_top; i++) {
         if (embedding_corpus_relevance(top[i].cosine, stats.cosine_sum, stats.pool) >= min_rel) {
            top[kept_n++] = top[i];
         }
      }
      OLOG_DEBUG("document_adapter: pool=%d mean=%.3f top=%.3f kept %d/%d above relevance %.2f",
                 stats.pool, stats.cosine_sum / stats.pool, top[0].cosine, kept_n, n_top, min_rel);
      n_top = kept_n;
      if (n_top == 0) {
         return SUCCESS; /* nothing relevant enough */
      }
   }

   int64_t ids[DOCUMENT_TOP_CAP];
   for (int i = 0; i < n_top; i++) {
      ids[i] = top[i].chunk_id;
   }
   chunks = calloc((size_t)n_top, sizeof(*chunks));
   if (chunks == NULL) {
      OLOG_ERROR("document_adapter: OOM allocating chunks buffer (n=%d)", n_top);
      return FAILURE;
   }
   int loaded = 0;
   if (document_db_chunks_get_by_ids(user_id, ids, n_top, chunks, &loaded) != SUCCESS) {
      OLOG_ERROR("document_adapter: chunk fetch failed (user_id=%d)", user_id);
      rc = FAILURE;
      goto cleanup;
   }

   out = calloc((size_t)n_top, sizeof(*out));
   if (out == NULL) {
      OLOG_ERROR("document_adapter: OOM allocating candidate array (n=%d)", n_top);
      rc = FAILURE;
      goto cleanup;
   }

   bool truncated_warned = false;
   for (int i = 0; i < n_top; i++) {
      const document_chunk_t *c = NULL;
      for (int j = 0; j < loaded; j++) {
         if (chunks[j].id == top[i].chunk_id) {
            c = &chunks[j];
            break;
         }
      }
      if (c == NULL) {
         continue; /* deleted between ranking and fetch */
      }

      /* Render "[<filename>] <chunk_text>".  filename comes from the
       * JOIN inside document_db_chunks_get_by_ids — no per-chunk N+1.
       *
       * Sizing: filename ≤ DOC_FILENAME_MAX (256), `[` + `] ` = 3,
       * chunk text ≤ DOC_CHUNK_TEXT_MAX (4096), terminator = 1.
       * Worst-case 4356 bytes; buffer is FOCUS_TEXT_MAX_BYTES +
       * DOC_FILENAME_MAX + 16 (4368) so snprintf cannot truncate
       * even with maximum-length filename and chunk text together.
       * If the rendered string ever exceeds FOCUS_TEXT_MAX_BYTES,
       * focus_candidate_init's truncation handler downstream caps
       * and logs once via `truncated_warned`.  We do NOT pre-reject
       * here: pre-guarding work the framework already does correctly
       * silently dropped content the framework would have truncated
       * cleanly. */
      char rendered[FOCUS_TEXT_MAX_BYTES + DOC_FILENAME_MAX + 16];
      const char *fname = (c->doc_filename[0] != '\0') ? c->doc_filename : "(document)";
      (void)snprintf(rendered, sizeof(rendered), "[%s] %s", fname, c->text);

      char item_id[FOCUS_ITEM_ID_BUFLEN];
      if (focus_candidate_format_item_id(item_id, sizeof(item_id), "document_chunk", c->id) !=
          SUCCESS) {
         OLOG_ERROR("document_adapter: item_id formatting failed (chunk_id=%lld)",
                    (long long)c->id);
         focus_adapter_failure_cleanup(out, produced, out_candidates, out_count);
         out = NULL; /* ownership transferred to failure-cleanup */
         rc = FAILURE;
         goto cleanup;
      }

      const float recency = focus_recency_decay_uniform(c->created_at, now);
      if (focus_candidate_init(&out[produced], "document_chunk", FOCUS_SOURCE_EXTERNAL, rendered,
                               item_id, c->created_at, top[i].cosine, recency,
                               DOCUMENT_DEFAULT_IMPORTANCE, &truncated_warned) != SUCCESS) {
         OLOG_ERROR("document_adapter: focus_candidate_init failed (chunk_id=%lld)",
                    (long long)c->id);
         focus_adapter_failure_cleanup(out, produced, out_candidates, out_count);
         out = NULL; /* ownership transferred to failure-cleanup */
         rc = FAILURE;
         goto cleanup;
      }
      /* Provenance intentionally zeroed — documents have no
       * conv-based provenance.  Filename is rendered into text. */
      produced++;
   }

cleanup:
   free(chunks);
   if (rc == SUCCESS && out != NULL) {
      *out_candidates = out;
      *out_count = produced;
   }
   /* On FAILURE, focus_adapter_failure_cleanup already zeroed the
    * out-params and freed `out`; on SUCCESS-with-no-candidates
    * (loaded==0), out_candidates/out_count remain NULL/0 from the
    * function's top-of-body initialization. */
   return rc;
}

static const focus_source_adapter_t k_document_focus_adapter = {
   .source_id = "document_chunk",
   .source_type = FOCUS_SOURCE_EXTERNAL,
   .requires_embedding = true,
   .query = document_adapter_query,
};

int document_focus_adapter_register(void) {
   return focus_register_source(&k_document_focus_adapter);
}
