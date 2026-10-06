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
 * Document chunk focus adapter (per-turn context injection and recall).
 *
 * source_id          = "document_chunk"
 * source_type        = FOCUS_SOURCE_EXTERNAL
 * requires_embedding = false (keyword matching still runs without one)
 *
 * Pipeline:
 *   1. Rank the chunks user_id can access (own + shared) with document_rank:
 *      the top chunks by cosine plus every keyword hit, fused into one order,
 *      the same ranking the document_search tool uses.
 *   2. Keep a chunk when either check says it's relevant:
 *        - semantic: clearly above the corpus-typical similarity
 *          (document_min_relevance);
 *        - label: its document's label contains the query's content words
 *          (document_label_matches), so a note named in the query is found even
 *          when a short query's similarity sits under the semantic bar.
 *   3. At most DOCUMENT_PER_DOC_MAX chunks per document, so one matching note
 *      can't fill every slot; the rest is a document_read away.
 *   4. Render "[<filename>] <chunk_text>" through focus_candidate_init, which
 *      truncates to FOCUS_TEXT_MAX_BYTES.  semantic_score is the chunk's cosine,
 *      the scale every other source uses, so documents don't gain or lose
 *      ground against facts and entities in the combined ranking.
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

#include <math.h>
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
#include "tools/document_rank.h"

/* Importance score for every document chunk.  Documents lack a
 * confidence-style intrinsic per-chunk signal (unlike facts); 0.5
 * keeps documents below curated memory facts (1.0) while above
 * future low-importance sources. */
#define DOCUMENT_DEFAULT_IMPORTANCE 0.5f

/* Most candidates this adapter returns (callers pass top_k, normally <= 32). */
#define DOCUMENT_TOP_CAP 64

/* Most chunks of one document returned. */
#define DOCUMENT_PER_DOC_MAX 2

/* Whether the ranked chunk passes either relevance check. */
static bool chunk_relevant(const document_ranking_t *r, const document_ranked_t *it) {
   if (document_label_matches(it->label_terms, r->query_terms)) {
      return true;
   }
   if (!it->has_cosine) {
      return false;
   }
   /* Embedding models put unrelated text at a model-specific baseline
    * similarity (bge-small ~0.43, MiniLM ~0.1), so a raw cosine floor doesn't
    * transfer between models.  Measure from the corpus-typical level instead:
    * relevance = (cos - pool_mean) / (1 - pool_mean).  A small corpus gives no
    * meaningful baseline, so it isn't gated. */
   const float min_rel = g_config.memory.focus_injection.document_min_relevance;
   if (min_rel <= 0.0f || r->stats.pool < EMBEDDING_RELEVANCE_MIN_POOL) {
      return true;
   }
   return embedding_corpus_relevance(it->cosine, r->stats.cosine_sum, r->stats.pool) >= min_rel;
}

/* Chunks of @p document_id already chosen. */
static int doc_count(document_ranked_t *const *chosen, int n, int64_t document_id) {
   int count = 0;
   for (int i = 0; i < n; i++) {
      if (chosen[i]->document_id == document_id) {
         count++;
      }
   }
   return count;
}

static int emit_candidates(document_ranked_t *const *chosen,
                           int n,
                           time_t now,
                           focus_candidate_t **out_candidates,
                           int *out_count) {
   focus_candidate_t *out = calloc((size_t)n, sizeof(*out));
   if (out == NULL) {
      OLOG_ERROR("document_adapter: OOM allocating candidate array (n=%d)", n);
      return FAILURE;
   }
   bool truncated_warned = false;
   int produced = 0;
   for (int i = 0; i < n; i++) {
      const document_ranked_t *c = chosen[i];
      if (!c->text) {
         continue; /* deleted since it was ranked */
      }

      /* Render "[<filename>] <chunk_text>".  Sizing: filename <= DOC_FILENAME_MAX
       * (256), "[" + "] " = 3, chunk text <= DOC_CHUNK_TEXT_MAX (4096),
       * terminator = 1: worst case 4356 bytes, under the buffer, so snprintf
       * can't truncate.  focus_candidate_init caps anything past
       * FOCUS_TEXT_MAX_BYTES and logs once via truncated_warned. */
      char rendered[FOCUS_TEXT_MAX_BYTES + DOC_FILENAME_MAX + 16];
      const char *fname = (c->filename && c->filename[0] != '\0') ? c->filename : "(document)";
      (void)snprintf(rendered, sizeof(rendered), "[%s] %s", fname, c->text);

      char item_id[FOCUS_ITEM_ID_BUFLEN];
      if (focus_candidate_format_item_id(item_id, sizeof(item_id), "document_chunk", c->chunk_id) !=
          SUCCESS) {
         OLOG_ERROR("document_adapter: item_id formatting failed (chunk_id=%lld)",
                    (long long)c->chunk_id);
         focus_adapter_failure_cleanup(out, produced, out_candidates, out_count);
         return FAILURE;
      }

      const float recency = focus_recency_decay_uniform(c->created_at, now);
      /* Scores are 0..1 and FOCUS_SCORE_NA is a negative sentinel, so a
       * negative cosine (possible with some models) reads as no similarity. */
      const float semantic = c->has_cosine ? fmaxf(c->cosine, 0.0f) : FOCUS_SCORE_NA;
      if (focus_candidate_init(&out[produced], "document_chunk", FOCUS_SOURCE_EXTERNAL, rendered,
                               item_id, c->created_at, semantic, recency,
                               DOCUMENT_DEFAULT_IMPORTANCE, &truncated_warned) != SUCCESS) {
         OLOG_ERROR("document_adapter: focus_candidate_init failed (chunk_id=%lld)",
                    (long long)c->chunk_id);
         focus_adapter_failure_cleanup(out, produced, out_candidates, out_count);
         return FAILURE;
      }
      /* Provenance intentionally zeroed: documents have no conversation
       * provenance.  The filename is rendered into the text. */
      produced++;
   }
   *out_candidates = out;
   *out_count = produced;
   return SUCCESS;
}

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
   *out_candidates = NULL;
   *out_count = 0;
   if (max_candidates <= 0 || query_text == NULL || query_text[0] == '\0') {
      return SUCCESS;
   }
   /* Without a usable embedding, only the keyword channel runs. */
   const bool semantic = query_embedding != NULL && embed_dim > 0 && memory_embeddings_available();

   const int keep = (max_candidates > DOCUMENT_TOP_CAP) ? DOCUMENT_TOP_CAP : max_candidates;
   const document_rank_opts_t opts = {
      .semantic_top = keep,
      .lexical_limit = DOCUMENT_RANK_LEXICAL_MAX,
      .phrase_top = keep,
      .body_phrase = false, /* order within this source only; no text needed to rank */
      .temporal = false,    /* recency is weighed by the focus ranker */
   };
   document_ranking_t ranking;
   if (document_rank_hybrid(user_id, query_text, semantic ? query_embedding : NULL,
                            semantic ? (int)embed_dim : 0, &opts, &ranking) != SUCCESS) {
      OLOG_ERROR("document_adapter: chunk ranking failed (user_id=%d)", user_id);
      return FAILURE;
   }

   document_ranked_t *chosen[DOCUMENT_TOP_CAP];
   int n = 0;
   int label_kept = 0;
   for (int i = 0; i < ranking.count && n < keep; i++) {
      document_ranked_t *it = &ranking.items[i];
      if (!chunk_relevant(&ranking, it) ||
          doc_count(chosen, n, it->document_id) >= DOCUMENT_PER_DOC_MAX) {
         continue;
      }
      if (document_label_matches(it->label_terms, ranking.query_terms)) {
         label_kept++;
      }
      chosen[n++] = it;
   }
   OLOG_DEBUG("document_adapter: ranked %d (pool=%d, query words=%d), kept %d (%d by label)",
              ranking.count, ranking.stats.pool, ranking.query_terms, n, label_kept);

   int rc = SUCCESS;
   if (n > 0) {
      /* Text only for the chunks kept. */
      rc = document_ranking_load_text(user_id, &ranking, chosen, n);
      if (rc != SUCCESS) {
         OLOG_ERROR("document_adapter: chunk fetch failed (user_id=%d)", user_id);
      } else {
         rc = emit_candidates(chosen, n, now, out_candidates, out_count);
      }
   }
   document_ranking_free(&ranking);
   return rc;
}

static const focus_source_adapter_t k_document_focus_adapter = {
   .source_id = "document_chunk",
   .source_type = FOCUS_SOURCE_EXTERNAL,
   .requires_embedding = false,
   .query = document_adapter_query,
};

int document_focus_adapter_register(void) {
   return focus_register_source(&k_document_focus_adapter);
}
