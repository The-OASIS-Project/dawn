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
 * Memory Database — entity-merge alias scorer.
 *
 * Phase 6b split from memory_db_alias.c — pair scoring used by the cascade
 * (Stage 6 best-candidate scoring), consider_auto_merge (band routing),
 * and the WebUI two-click preview surface.  Shared helpers (tokenizer /
 * type filter / composite formula / DB-accessing signal computations) live
 * in memory_db_alias.c via memory_db_alias_internal.h.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <string.h>

#include "auth/auth_db_internal.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_db_alias_internal.h"
#include "memory/memory_db_aliases.h"
#include "memory/memory_db_entities.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_types.h"

/* =============================================================================
 * Pair scoring — used by score_pair, consider_auto_merge, and the WebUI
 * preview surface (Ckpt 5).
 *
 * Synthetic-entity contract: callers may pass a memory_entity_t with
 * id == 0 to represent a not-yet-materialized canonical (the dry-run-
 * with-no-existing-self path uses this).  All DB-touching helpers below
 * (memory_alias_internal_compute_exclusive_relation_overlap, _contact_field_overlap,
 * memory_embeddings_entity_cosine) MUST short-circuit on id <= 0 and
 * leave the corresponding signal at 0.  Any future signal that probes DB
 * by id must follow the same rule.
 * ============================================================================= */

void memory_alias_internal_score_pair_full(int user_id,
                                           const memory_entity_t *src,
                                           bool src_is_user_self,
                                           const memory_entity_t *tgt,
                                           bool tgt_is_user_self,
                                           bool other_is_allow_list_token,
                                           memory_alias_evidence_t *out) {
   memset(out, 0, sizeof(*out));

   /* Veto first — short-circuits everything else. */
   out->type_veto_fired = memory_alias_internal_type_veto_fires(src->entity_type, tgt->entity_type);
   /* id 0 = a synthetic entity: the self-identity seed (see the contract above). */
   out->name_veto_fired = memory_alias_internal_name_veto_fires(src->canonical_name,
                                                                tgt->canonical_name,
                                                                src->id <= 0 || tgt->id <= 0);
   if (out->type_veto_fired || out->name_veto_fired) {
      memory_alias_apply_composite(out);
      return;
   }

   /* Name signals. */
   out->name_jaccard = memory_alias_compute_name_jaccard(src->canonical_name, tgt->canonical_name);
   out->name_substring_bonus_applied = memory_alias_compute_name_substring(src->canonical_name,
                                                                           tgt->canonical_name);

   /* Type signal. */
   out->type_match = memory_alias_internal_type_match_signal(src->entity_type, tgt->entity_type);

   /* Stage 5 signals — relation + contact overlap.  Each takes the auth_db
    * lock independently; never nested. */
   out->exclusive_relation_overlap = memory_alias_internal_compute_exclusive_relation_overlap(
       user_id, src->id, tgt->id);
   out->contact_field_overlap = memory_alias_internal_compute_contact_field_overlap(user_id,
                                                                                    src->id,
                                                                                    tgt->id);

   /* Embedding cosine — gracefully zero if the engine isn't available
    * (tests run without ONNX) or if either side lacks an embedding.  We
    * embed the source canonical_name once and compare against the target's
    * cached vector. */
   if (memory_embeddings_available()) {
      float query_emb[MAX_EMBEDDING_DIMS];
      int dims = 0;
      if (memory_embeddings_embed(src->canonical_name, query_emb, &dims) == 0 && dims > 0) {
         float norm = memory_embeddings_l2_norm(query_emb, dims);
         float cosine = 0.0f;
         if (memory_embeddings_entity_cosine(user_id, tgt->id, query_emb, dims, norm, &cosine) ==
             SUCCESS) {
            out->embedding_cosine = cosine;
         }
      }
   }

   /* User-self bonus.  Pair-score callers that don't traffic in allow-
    * list tokens (the public memory_db_entity_score_pair WebUI preview)
    * pass false; link-user-self's synth-self path passes true when the
    * candidate is the canonical-name='user' allow-list token. */
   out->user_self_bonus_applied = memory_alias_internal_user_self_bonus_applies(
       user_id, src_is_user_self, tgt_is_user_self, src->canonical_name, tgt->canonical_name,
       out->contact_field_overlap > 0.0f, other_is_allow_list_token);

   memory_alias_apply_composite(out);
}

int memory_db_entity_score_pair(int user_id,
                                int64_t source_id,
                                int64_t target_id,
                                memory_alias_evidence_t *out_evidence) {
   if (!out_evidence || source_id <= 0 || target_id <= 0)
      return MEMORY_DB_FAILURE;
   if (source_id == target_id) {
      memset(out_evidence, 0, sizeof(*out_evidence));
      return MEMORY_DB_FAILURE;
   }

   memory_entity_t src, tgt;
   int64_t src_canonical_id = 0, tgt_canonical_id = 0;
   bool src_is_self = false, tgt_is_self = false;
   int rc = memory_alias_internal_load_entity_full(user_id, source_id, &src, &src_canonical_id,
                                                   &src_is_self);
   if (rc != MEMORY_DB_SUCCESS)
      return rc;
   rc = memory_alias_internal_load_entity_full(user_id, target_id, &tgt, &tgt_canonical_id,
                                               &tgt_is_self);
   if (rc != MEMORY_DB_SUCCESS)
      return rc;

   /* Refuse to score a pair where either entity is itself a soft alias.
    * The canonical-only entity-embedding cache means
    * memory_embeddings_entity_cosine() returns 0 for alias rows, which
    * would silently mis-route the band decision in the WebUI two-click
    * preview.  Surface a distinct return code so callers can prompt the
    * operator to pick the canonical instead. */
   if (src_canonical_id > 0 || tgt_canonical_id > 0) {
      memset(out_evidence, 0, sizeof(*out_evidence));
      return MEMORY_DB_INVALID_ALIAS_TARGET;
   }

   memory_alias_internal_score_pair_full(user_id, &src, src_is_self, &tgt, tgt_is_self,
                                         /* other_is_allow_list_token */ false, out_evidence);
   return MEMORY_DB_SUCCESS;
}
