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
 * Internal header shared by memory_db_alias*.c TUs.
 *
 * Phase 6b source split — the entity-merge cascade was a single 3800-line
 * TU; this header exposes the helpers and types that were `static` before
 * the split and are now needed by sibling TUs (scorer / cascade / writes).
 * NOT a public API — callers outside src/memory/memory_db_alias*.c must
 * not include this header.
 *
 * All helpers in this header assume the caller does NOT hold AUTH_DB_LOCK
 * except where noted on individual declarations.  Each helper acquires
 * the lock on its own per the alias surface's "per-call lock" contract
 * (alias work runs off the conversational hot path).
 */
#ifndef DAWN_MEMORY_DB_ALIAS_INTERNAL_H
#define DAWN_MEMORY_DB_ALIAS_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* memory_db_aliases.h: provides memory_alias_evidence_t struct (used as value
 * at memory_alias_internal_score_pair_ctx.evidence below — full type needed).
 * memory_types.h: provides memory_entity_t (used as pointer only — full type
 * also needed via memory_db_aliases.h's transitive include for now).
 * memory_db_entities.h NOT included here — its surface is reached via the
 * pointer-only functions in this internal header. */
#include "memory/memory_db_aliases.h"
#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Tokenizer (used by cascade Stage 2 + shared Jaccard signal).
 * ============================================================================= */

#define ALIAS_TOKEN_MAX 32 /* per-token byte cap (matches MEMORY_ENTITY_NAME_MAX/2) */
#define ALIAS_MAX_TOKENS 16
#define ALIAS_MIN_TOKEN_LEN 2

typedef struct {
   char tokens[ALIAS_MAX_TOKENS][ALIAS_TOKEN_MAX];
   int count;
} alias_token_set_t;

void memory_alias_internal_tokenize(const char *s, alias_token_set_t *out);

/* Directional overlap: |tokens_a ∩ tokens_b| / |tokens_b|.  Used by cascade
 * Stage 2 in the synthetic-self path so a candidate isn't penalized by a
 * verbose synthetic seed (real_name + aliases). */
float memory_alias_internal_directional_overlap(const char *a, const char *b);

/* =============================================================================
 * Type filter helpers (cascade Stage 3 + composite type_match signal).
 * ============================================================================= */

bool memory_alias_internal_type_veto_fires(const char *a_type, const char *b_type);
float memory_alias_internal_type_match_signal(const char *a_type, const char *b_type);

/* =============================================================================
 * DB-accessing helpers shared across scorer / cascade / writes.
 * ============================================================================= */

/* Load a single entity by id, plus its canonical_id and is_user_self flag.
 *   canonical_id_out: 0 = NULL = self is canonical; > 0 = soft alias of that id.
 *   is_user_self_out: optional (NULL OK). */
int memory_alias_internal_load_entity_full(int user_id,
                                           int64_t entity_id,
                                           memory_entity_t *out_entity,
                                           int64_t *out_canonical_id,
                                           bool *out_is_user_self);

/* Find the user-self entity id for @p user_id (rows with is_user_self = 1).
 * Returns id > 0 on SUCCESS; 0 with NOT_FOUND if seeding hasn't run. */
int memory_alias_internal_find_user_self_id(int user_id, int64_t *out_id);

/* Compute exclusive_relation_overlap signal between two entities.
 *   1.0 if any open merge-evidence relation (memory_db_relation_is_merge_evidence)
 *       shares object_entity_id (or canonical-equal object_value)
 *   0.5 if any other relation shares object
 *   0   otherwise */
float memory_alias_internal_compute_exclusive_relation_overlap(int user_id,
                                                               int64_t a_id,
                                                               int64_t b_id);

/* Compute contact_field_overlap signal: 1.0 if any contact email/phone is
 * shared between the two entities (after normalization), 0 otherwise. */
float memory_alias_internal_compute_contact_field_overlap(int user_id, int64_t a_id, int64_t b_id);

/* Determine whether the user_self_bonus applies between two entities — see
 * the canonical implementation in memory_db_alias.c for branch ordering. */
bool memory_alias_internal_user_self_bonus_applies(int user_id,
                                                   bool a_is_user_self,
                                                   bool b_is_user_self,
                                                   const char *a_canonical_name,
                                                   const char *b_canonical_name,
                                                   bool contact_overlap_fired,
                                                   bool other_is_allow_list_token);

/* Trim a trailing partial UTF-8 sequence (continuation byte without a
 * complete leader pair) so downstream tokenizers don't see a malformed
 * leader at a strncpy split point.  In-place. */
void memory_alias_internal_trim_trailing_partial_utf8(char *buf);

/* =============================================================================
 * Scorer entry — used by cascade (Stage 6 pair scoring) and writes
 * (link-user-self dry-run + commit-mode focused scoring).
 *
 * Synthetic-entity contract: callers may pass a memory_entity_t with
 * id == 0 to represent a not-yet-materialized canonical (the dry-run-
 * with-no-existing-self path uses this).  All DB-touching helpers
 * short-circuit on id <= 0 and leave the corresponding signal at 0.
 * ============================================================================= */

void memory_alias_internal_score_pair_full(int user_id,
                                           const memory_entity_t *src,
                                           bool src_is_user_self,
                                           const memory_entity_t *tgt,
                                           bool tgt_is_user_self,
                                           bool other_is_allow_list_token,
                                           memory_alias_evidence_t *out);

/* =============================================================================
 * Cascade entry — used by writes (consider_auto_merge passes scored array
 * so it can propose every above-threshold candidate, not just the winner).
 *
 * The resolver entry points (memory_db_entity_resolve_alias and
 * memory_db_entity_resolve_alias_for_self) live in the cascade TU and
 * call this directly without going through the internal API.
 * ============================================================================= */

/* Per-candidate Stage-6 score record.  cascade_internal's optional
 * `out_scored` array sorted by composite DESC after return. */
typedef struct {
   int64_t entity_id;
   memory_alias_evidence_t evidence;
} scored_candidate_t;

int memory_alias_internal_cascade(int user_id,
                                  const char *canonical_name,
                                  const char *entity_type,
                                  int64_t inbound_id,
                                  bool inbound_is_user_self,
                                  bool use_synth_self,
                                  int64_t *out_winner_id,
                                  int *out_matched_stage,
                                  memory_alias_evidence_t *out_evidence,
                                  scored_candidate_t *out_scored,
                                  int out_scored_max,
                                  int *out_scored_count);

/* =============================================================================
 * Writes helper — used by cascade-cousin (consider_auto_merge) and
 * link-user-self orchestrator.
 * ============================================================================= */

/* Insert a memory_entity_merge_proposals row.  No entity-row mutation. */
int memory_alias_internal_insert_merge_proposal(int user_id,
                                                int64_t source_id,
                                                int64_t target_id,
                                                float composite_score,
                                                const char *evidence_json,
                                                int64_t *out_proposal_id);

#ifdef __cplusplus
}
#endif

#endif /* DAWN_MEMORY_DB_ALIAS_INTERNAL_H */
