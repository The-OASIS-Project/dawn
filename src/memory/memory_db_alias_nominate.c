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
 * The consolidator's entity merges: the pairs a new entity may be the same as
 * (memory_db_entity_link_candidates) and a decision on a pair someone
 * nominated (memory_db_entity_consider_nominated_merge), on the extraction
 * gate's cascade and ladder.
 */

#include <string.h>

#include "config/dawn_config.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_db_alias_internal.h"
#include "memory/memory_db_aliases.h"
#include "memory/memory_types.h"

int memory_db_entity_link_candidates(int user_id,
                                     int64_t entity_id,
                                     int64_t *out_ids,
                                     int max,
                                     int *count_out) {
   *count_out = 0;
   if (!out_ids || max <= 0 || entity_id <= 0) {
      return MEMORY_DB_FAILURE;
   }
   memory_entity_t inbound;
   bool inbound_is_self = false;
   int64_t inbound_canon = 0;
   int rc = memory_alias_internal_load_entity_full(user_id, entity_id, &inbound, &inbound_canon,
                                                   &inbound_is_self);
   if (rc != MEMORY_DB_SUCCESS || inbound_canon != 0) {
      return rc == MEMORY_DB_SUCCESS ? MEMORY_DB_SUCCESS : rc;
   }
   int64_t winner_id = 0;
   int matched_stage = 0;
   memory_alias_evidence_t ev;
   memset(&ev, 0, sizeof(ev));
   scored_candidate_t scored[MEMORY_ALIAS_STAGE5_MAX_CANDIDATES];
   int scored_count = 0;
   rc = memory_alias_internal_cascade(user_id, inbound.canonical_name, inbound.entity_type,
                                      entity_id, inbound_is_self, false, &winner_id, &matched_stage,
                                      &ev, scored, MEMORY_ALIAS_STAGE5_MAX_CANDIDATES,
                                      &scored_count);
   if (rc != MEMORY_DB_SUCCESS) {
      return rc;
   }
   int n = 0;
   if (matched_stage == 1 && winner_id > 0 && winner_id != entity_id) {
      out_ids[n++] = winner_id;
   }
   /* A scored winner is always in scored: every candidate the cascade scores
    * is listed there (it keeps at most STAGE5_MAX, the array's size), after
    * stage 3 dropped the vetoed ones. */
   const float review = (float)g_config.memory.entity_merge_review_threshold;
   for (int i = 0; i < scored_count && n < max; i++) {
      if (scored[i].evidence.composite_score < review) {
         break; /* sorted best first */
      }
      /* A stage-1 winner is already first. */
      if (scored[i].entity_id == entity_id ||
          (matched_stage == 1 && scored[i].entity_id == winner_id)) {
         continue;
      }
      out_ids[n++] = scored[i].entity_id;
   }
   *count_out = n;
   return MEMORY_DB_SUCCESS;
}

int memory_db_entity_consider_nominated_merge(int user_id,
                                              int64_t source_id,
                                              int64_t target_id,
                                              bool propose_only,
                                              memory_alias_evaluate_t *out_eval) {
   if (!out_eval || source_id <= 0 || target_id <= 0) {
      return MEMORY_DB_FAILURE;
   }
   memset(out_eval, 0, sizeof(*out_eval));
   out_eval->source_entity_id = source_id;
   out_eval->target_entity_id = target_id;
   /* [memory.entity_merge] enabled = false: nothing is linked or proposed. */
   if (source_id == target_id || !g_config.memory.entity_merge_enabled) {
      out_eval->outcome = MEMORY_ALIAS_OUTCOME_NO_CANDIDATES;
      return MEMORY_DB_SUCCESS;
   }
   memory_entity_t source;
   memory_entity_t target;
   bool src_self = false;
   bool tgt_self = false;
   int64_t src_canon = 0;
   int64_t tgt_canon = 0;
   int rc = memory_alias_internal_load_entity_full(user_id, source_id, &source, &src_canon,
                                                   &src_self);
   if (rc == MEMORY_DB_SUCCESS) {
      rc = memory_alias_internal_load_entity_full(user_id, target_id, &target, &tgt_canon,
                                                  &tgt_self);
   }
   if (rc != MEMORY_DB_SUCCESS) {
      return rc;
   }
   /* Already one class, or one of them an alias: nothing to link here. */
   if (src_canon != 0 || tgt_canon != 0) {
      out_eval->outcome = MEMORY_ALIAS_OUTCOME_NO_CANDIDATES;
      return MEMORY_DB_SUCCESS;
   }

   /* The same ladder as the extraction gate's, on the nominated pair: the
    * same canonical name is the same entity; otherwise the composite score
    * decides, and differing types or names veto. */
   memory_alias_evidence_t ev;
   memset(&ev, 0, sizeof(ev));
   if (strcmp(source.canonical_name, target.canonical_name) == 0) {
      ev.name_jaccard = 1.0f;
      ev.composite_score = 1.0f;
   } else {
      rc = memory_db_entity_score_pair(user_id, source_id, target_id, &ev);
      if (rc != MEMORY_DB_SUCCESS) {
         return rc;
      }
   }
   out_eval->evidence = ev;
   if (ev.type_veto_fired || ev.name_veto_fired) {
      out_eval->outcome = MEMORY_ALIAS_OUTCOME_REJECTED;
      return MEMORY_DB_SUCCESS;
   }

   /* The longer name stays canonical (person, pet, place). */
   int64_t link_src = source_id;
   int64_t link_tgt = target_id;
   if (memory_alias_internal_should_swap(user_id, &source, target_id)) {
      link_src = target_id;
      link_tgt = source_id;
   }
   out_eval->source_entity_id = link_src;
   out_eval->target_entity_id = link_tgt;

   if (!propose_only && ev.composite_score >= (float)g_config.memory.entity_merge_auto_threshold) {
      int64_t link_id = 0;
      rc = memory_db_entity_alias_link(user_id, link_src, link_tgt, "soft", "consolidation",
                                       ev.composite_score, NULL, &link_id);
      if (rc == MEMORY_DB_SUCCESS) {
         out_eval->outcome = MEMORY_ALIAS_OUTCOME_AUTO_MERGED;
         out_eval->link_id = link_id;
         return MEMORY_DB_SUCCESS;
      }
      OLOG_WARNING("memory_db_alias: nominated link failed for src=%lld tgt=%lld",
                   (long long)link_src, (long long)link_tgt);
   }
   /* Short of the ladder (or not to be linked unseen): the user decides. */
   int64_t prop_id = 0;
   rc = memory_alias_internal_insert_merge_proposal(user_id, link_src, link_tgt, ev.composite_score,
                                                    NULL, &prop_id);
   if (rc != MEMORY_DB_SUCCESS) {
      out_eval->outcome = MEMORY_ALIAS_OUTCOME_REJECTED;
      return MEMORY_DB_SUCCESS;
   }
   out_eval->outcome = MEMORY_ALIAS_OUTCOME_PROPOSED;
   out_eval->proposal_id = prop_id;
   return MEMORY_DB_SUCCESS;
}
