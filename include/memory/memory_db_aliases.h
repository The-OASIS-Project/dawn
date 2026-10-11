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
 * Memory Database — entity-merge / user-identity-dedup alias surface (v43).
 *
 * Equivalence-class API layered on top of memory_db_entity_merge():
 *   - memory_db_entity_resolve_alias()          — Stage 1-6 cascade for
 *                                                 surface-form lookup at
 *                                                 extraction time
 *   - memory_db_entity_score_pair()             — pure pair scoring across
 *                                                 the five composite signals
 *                                                 + bonuses + veto
 *   - memory_db_entity_consider_auto_merge()    — composite scorer + threshold
 *                                                 band routing for a freshly-
 *                                                 created entity
 *   - memory_db_entity_alias_link()             — soft-link write path
 *                                                 (canonical_id + audit row +
 *                                                 entity-cache invalidation)
 *   - memory_db_entity_alias_unlink()           — split path (zero canonical_id
 *                                                 + stamp unlinked_at)
 *   - memory_db_relation_list_by_subject_class* — equivalence-class-aware
 *                                                 relation listing helpers
 *   - memory_alias_canonical_priority_compare() — order-independent canonical
 *                                                 selection rule (§9)
 *
 * See docs/ENTITY_MERGE_DESIGN.md §6 (cascade), §7 (composite formula), §9
 * (order-independence), §10 (relation re-pointing), §13 (audit trail).
 *
 * Threshold defaults (first-ship, conservative — Phase 2 calibration may
 * loosen them after corpus validation):
 *   auto-merge   ≥ 0.90  → soft link + memory_entity_aliases row
 *   review band  ≥ 0.70  → memory_entity_merge_proposals row, no entity write
 *   reject       <  0.70 → debug-log only
 */

#ifndef MEMORY_DB_ALIASES_H
#define MEMORY_DB_ALIASES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "memory/memory_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Composite scoring constants
 *
 * AUTO/REVIEW thresholds are RUNTIME-CONFIGURABLE via dawn.toml's
 * [memory.entity_merge] section (`auto_threshold`, `review_threshold`).
 * Production routing in `memory_db_entity_consider_auto_merge` reads
 * `g_config.memory.entity_merge_*_threshold`, NOT these constants.
 *
 * The #defines below are retained as:
 *   1. Test-reference values for unit tests that fix g_config to known
 *      defaults via config_set_defaults().
 *   2. Historical baseline ("first-ship" values when the config gate
 *      landed) — the runtime defaults in config_defaults.c match these
 *      for AUTO (0.90) but DIVERGE for REVIEW (runtime default 0.50,
 *      down from the original compile-time 0.70).  See the rationale
 *      in dawn.toml.example §[memory.entity_merge].
 *
 * If you bump AUTO_THRESHOLD here, also bump the runtime default in
 * config_defaults.c and the SETTINGS_SCHEMA range minimum.
 * ============================================================================= */

#define MEMORY_ALIAS_AUTO_THRESHOLD 0.90f
#define MEMORY_ALIAS_REVIEW_THRESHOLD 0.70f /* historical default — runtime default is 0.50 */

/* Threshold for promoting an existing entity to is_user_self=1 during
 * link-user-self when no is_user_self=1 row exists yet (design §8 Path B
 * step 1).  Sits below the auto-merge threshold because the synthetic
 * seed has no DB-resident relations, so exclusive_relation_overlap
 * forfeits to 0 — capping production composite at ~0.45-0.65 even for
 * a perfect-name match.  Operator has explicitly invoked link-user-self,
 * so a meaningful match (jaccard or substring + bonus) is sufficient
 * intent; result reports show what was promoted so operator can split
 * if wrong.  Tunable via this constant if false-positive rate becomes
 * an issue post-deployment. */
#define MEMORY_ALIAS_SELF_PROMOTION_THRESHOLD 0.30f

#define MEMORY_ALIAS_NAME_JACCARD_FLOOR 0.30f
#define MEMORY_ALIAS_COSINE_FLOOR 0.50f

#define MEMORY_ALIAS_W_NAME_JACCARD 0.30f
#define MEMORY_ALIAS_W_EMBEDDING_COSINE 0.30f
#define MEMORY_ALIAS_W_EXCLUSIVE_RELATION_OVERLAP 0.25f
#define MEMORY_ALIAS_W_CONTACT_FIELD_OVERLAP 0.10f
#define MEMORY_ALIAS_W_TYPE_MATCH 0.05f

#define MEMORY_ALIAS_BONUS_NAME_SUBSTRING 0.10f
#define MEMORY_ALIAS_BONUS_USER_SELF 0.20f

/* Cascade caps */
#define MEMORY_ALIAS_STAGE2_MAX_CANDIDATES 32
#define MEMORY_ALIAS_STAGE4_MAX_CANDIDATES 8
#define MEMORY_ALIAS_STAGE5_MAX_CANDIDATES 3

/* =============================================================================
 * Composite evidence
 *
 * The five raw signals plus the bonus + veto bookkeeping that produced the
 * final composite_score.  Used by tests, the WebUI preview path (Ckpt 5),
 * and the audit trail (evidence_json).  All fields are post-clamp /
 * post-formula values so a serialized JSON copy is self-describing.
 * ============================================================================= */
typedef struct {
   /* Raw signal values (each in [0, 1]). */
   float name_jaccard;
   float embedding_cosine;           /* clamped to [0, 1] from raw cosine */
   float exclusive_relation_overlap; /* {0, 0.5, 1.0} */
   float contact_field_overlap;      /* {0, 1} */
   float type_match;                 /* {0, 1} */

   /* Bonus + veto bookkeeping. */
   bool name_substring_bonus_applied;
   bool user_self_bonus_applied;
   bool type_veto_fired;

   /* Final composite (post-veto, post-weighted-sum, post-bonuses, capped at 1.0). */
   float composite_score;
} memory_alias_evidence_t;

/* =============================================================================
 * Resolver outcome
 * ============================================================================= */
typedef struct {
   int64_t resolved_id;              /* 0 = no resolution; > 0 = canonical id (post-COALESCE) */
   int matched_stage;                /* 0 = no match; 1..6 = which cascade stage produced it */
   memory_alias_evidence_t evidence; /* meaningful only when matched_stage >= 2 */
} memory_alias_resolve_t;

/* =============================================================================
 * Auto-merge gate outcome
 * ============================================================================= */
#define MEMORY_ALIAS_OUTCOME_NO_CANDIDATES 0
#define MEMORY_ALIAS_OUTCOME_REJECTED 1    /* composite < review threshold */
#define MEMORY_ALIAS_OUTCOME_PROPOSED 2    /* review band → proposal row inserted */
#define MEMORY_ALIAS_OUTCOME_AUTO_MERGED 3 /* >= auto threshold → soft link */

typedef struct {
   int outcome; /* MEMORY_ALIAS_OUTCOME_* */
   /* Alias side of the resulting link.  Equals the entity_id passed to
    * consider_auto_merge() in the common case; differs ONLY when the AUTO
    * branch's longer-canonical preference fires and swaps direction (the
    * inbound becomes canonical, the previous-winner becomes the alias).
    * Caller can detect a swap by comparing this to the entity_id it passed
    * in. */
   int64_t source_entity_id;
   /* Canonical side of the resulting link, or 0 if no candidate.  Subject
    * to the same swap semantics as source_entity_id. */
   int64_t target_entity_id;
   int64_t link_id;     /* memory_entity_aliases.id, or 0 */
   int64_t proposal_id; /* memory_entity_merge_proposals.id, or 0 */
   memory_alias_evidence_t evidence;
} memory_alias_evaluate_t;

/* =============================================================================
 * Public API
 * ============================================================================= */

/**
 * @brief Resolve a surface form against existing canonical entities.
 *
 * Stage 1-6 cascade per docs/ENTITY_MERGE_DESIGN.md §6.  Used at extraction
 * time, BEFORE memory_db_entity_upsert(), to short-circuit duplicate
 * variants of an already-known canonical.
 *
 * Stages:
 *   1. Exact canonical_name match — returns the canonical id (post-JOIN).
 *   2. Token-Jaccard candidate generation (drops candidates with jaccard < 0.30).
 *   3. Type filter (`thing` carve-out: matches anything; person/place/pet/org
 *      must agree).
 *   4. Embedding cosine (drops candidates with cosine < 0.50; skipped silently
 *      if the embedding engine is unavailable).
 *   5. Exclusive-relation + contact overlap (top 3 by partial composite).
 *   6. Composite + threshold band routing.
 *
 * Returns MEMORY_DB_SUCCESS with `out_resolution->resolved_id == 0` to signal
 * "no match — caller should fall through to upsert."
 *
 * @param user_id User ID
 * @param name Display name (filter-checked by caller before this point)
 * @param entity_type Entity type ("person" / "place" / ...)
 * @param canonical_name Canonical (lowercased) form of @p name
 * @param out_resolution Output: populated on SUCCESS; resolved_id = 0 means no match
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_entity_resolve_alias(int user_id,
                                   const char *name,
                                   const char *entity_type,
                                   const char *canonical_name,
                                   memory_alias_resolve_t *out_resolution);

/**
 * @brief Resolve a synthetic-self seed against existing canonical entities.
 *
 * Phase 1.5 Ckpt C entry point.  Drives the same cascade as
 * memory_db_entity_resolve_alias() but in synthetic-self mode:
 *
 *   - Stage 2 uses directional overlap (|tokens_a ∩ tokens_b| / |tokens_b|)
 *     instead of standard Jaccard so single-token candidates aren't
 *     penalized by the verbose synthetic seed (real_name + aliases).
 *   - The canonical_name='user' allow-list is pre-added to the candidate
 *     pool before Stage 2 (per the empirical scan documented inline).
 *   - The inbound is treated as is_user_self=true so user_self_bonus
 *     fires correctly during Stage 6 scoring.
 *
 * @p canonical_name should already be the canonicalized union of
 * real_name + alias tokens (the same shape build_synthetic_self_entity()
 * produces for the link-user-self dry-run).  @p entity_type defaults to
 * "person" — pass NULL to accept the default.
 *
 * Unlike memory_db_entity_resolve_alias() (which commits only at the
 * auto-merge threshold), this entry point surfaces the cascade's best
 * Stage 6 candidate at ANY composite band — review-band and below-
 * threshold hits are reported so the Phase 2 caller can make its own
 * band-routing decision.  Inspect @p out_resolution->evidence
 * .composite_score to determine the band.
 *
 * @param user_id User ID
 * @param canonical_name Synthetic seed canonical (verbose, multi-token)
 * @param entity_type "person" or NULL (defaults to "person")
 * @param out_resolution Output: populated on SUCCESS; resolved_id > 0 when
 *                       any Stage 6 candidate was scored, 0 on clean miss
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_entity_resolve_alias_for_self(int user_id,
                                            const char *canonical_name,
                                            const char *entity_type,
                                            memory_alias_resolve_t *out_resolution);

/**
 * @brief Score a candidate pair across all five composite signals + bonuses.
 *
 * Pure-pair scoring (no cascade, no candidate generation).  Both entities
 * must already exist in @p user_id 's graph.  Used by the WebUI preview path
 * (`propose_entity_merge_preview`) and for tests that probe individual signals
 * without going through Stage 2's keyword search.
 *
 * The veto rule fires when both types are non-`thing` AND differ → composite
 * is forced to 0.  The five-term weighted sum + two additive bonuses are
 * applied per §7.  Final composite is clamped to [0, 1].
 *
 * @param user_id User ID (ownership is verified for both entities)
 * @param source_id Candidate that would become the alias if merged
 * @param target_id Candidate that would become the canonical
 * @param out_evidence Output: populated on SUCCESS
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_entity_score_pair(int user_id,
                                int64_t source_id,
                                int64_t target_id,
                                memory_alias_evidence_t *out_evidence);

/**
 * @brief Run the auto-merge gate against an existing entity.
 *
 * Loads @p entity_id, runs the resolver cascade (excluding entity_id from
 * its own candidate pool), and routes by composite score against the
 * RUNTIME-CONFIGURABLE thresholds in `[memory.entity_merge]`:
 *   - composite ≥ `auto_threshold`   (default 0.90) → soft link via
 *                                                     memory_db_entity_alias_link()
 *   - composite ≥ `review_threshold` (default 0.50) → proposal row inserted
 *                                                     for operator approval via
 *                                                     the WebUI Suggested-
 *                                                     Merges panel
 *   - else                                         → no DB write
 *
 * Propose-all-in-band: when AUTO doesn't fire, EVERY candidate scoring
 * ≥ review_threshold gets its own proposal row (not just the winner) so
 * operators can triage the full set in the UI.
 *
 * Phase 1 invokes this from the explicit operator paths (link-user-self,
 * the manual merge tool action).  Phase 2 wires it into
 * `process_extraction_response` after fresh entity creation +
 * post-relations storage.
 *
 * @param user_id User ID (ownership is verified)
 * @param entity_id The freshly-created or operator-supplied candidate
 * @param out_eval Output: populated on SUCCESS regardless of outcome.
 *                 `target_entity_id` is the winning candidate; `proposal_id`
 *                 is the first inserted proposal when outcome=PROPOSED.
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_entity_consider_auto_merge(int user_id,
                                         int64_t entity_id,
                                         memory_alias_evaluate_t *out_eval);

/**
 * @brief Soft-link source entity to target entity (canonical_id update +
 * audit row).
 *
 * Sets `memory_entities.canonical_id = target_id` on the source row,
 * inserts a `memory_entity_aliases` row capturing the link metadata, and
 * invalidates the entity-embedding cache (so subsequent reads pick up the
 * new alias state).  Both writes happen inside a BEGIN IMMEDIATE
 * transaction.  Refuses to link if either entity is missing, if the source
 * is itself a canonical for other rows (i.e. canonical of a non-empty
 * equivalence class), or if source_id == target_id.
 *
 * @param user_id User ID (ownership is verified for both entities)
 * @param source_id Entity to demote to alias
 * @param target_id Canonical entity to absorb the alias
 * @param link_kind "soft" or "hard" — hard reserved for the operator
 *                  consolidate path (Phase 3); Phase 1 always passes "soft"
 * @param reason "auto-merge" / "operator" / "seeded"
 * @param composite_score The score that triggered the link, or < 0 to omit
 * @param evidence_json Optional JSON blob (may be NULL)
 * @param out_link_id Output: memory_entity_aliases.id on success
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, MEMORY_DB_FAILURE
 */
int memory_db_entity_alias_link(int user_id,
                                int64_t source_id,
                                int64_t target_id,
                                const char *link_kind,
                                const char *reason,
                                float composite_score,
                                const char *evidence_json,
                                int64_t *out_link_id);

/**
 * @brief Reverse a soft alias link (split).
 *
 * Zeros `canonical_id` on the source entity row and stamps `unlinked_at` /
 * `unlink_reason` on the audit row.  Refuses if the link row was hard-merged
 * (link_kind = 'hard') — the source row is gone and no graceful undo is
 * possible without `dawn-admin memory reextract`.
 *
 * @param user_id User ID (ownership is verified)
 * @param link_id The memory_entity_aliases.id to unlink
 * @param unlink_reason "split-by-operator" / "reextract-reset" / etc.
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, MEMORY_DB_FAILURE
 */
int memory_db_entity_alias_unlink(int user_id, int64_t link_id, const char *unlink_reason);

/**
 * @brief List relations whose subject is anywhere in the canonical's
 * equivalence class (canonical row + all current soft aliases).
 *
 * Equivalent to memory_db_relation_list_by_subject() except the subject
 * filter is `subject_entity_id IN (canonical_id, alias_ids...)`.  Returns
 * relations regardless of validity period — use the `_at` variant to apply
 * temporal filtering.
 *
 * The implementation form (IN-subquery vs UNION ALL) was chosen after
 * EXPLAIN QUERY PLAN verification — see docs/ENTITY_MERGE_DESIGN.md §15.
 *
 * @param user_id User ID
 * @param canonical_id Canonical entity id (must have canonical_id IS NULL)
 * @param out Output array
 * @param max Maximum results
 * @param count_out Output: number of relations populated
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_relation_list_by_subject_class(int user_id,
                                             int64_t canonical_id,
                                             memory_relation_t *out,
                                             int max,
                                             int *count_out);

/**
 * @brief Equivalence-class variant of memory_db_relation_list_by_subject_at.
 *
 * Returns rows valid at @p as_of_ts whose subject is in the equivalence
 * class.  Pass @p as_of_ts = 0 for "currently valid".
 */
int memory_db_relation_list_by_subject_class_at(int user_id,
                                                int64_t canonical_id,
                                                int64_t as_of_ts,
                                                memory_relation_t *out,
                                                int max,
                                                int *count_out);

/**
 * @brief Equivalence-class variant of memory_db_relation_list_by_object.
 *
 * Returns relations where any equivalence-class member is the object.
 */
int memory_db_relation_list_by_object_class(int user_id,
                                            int64_t canonical_id,
                                            memory_relation_t *out,
                                            int max,
                                            int *count_out);

/* The canonical form the LLM-emitted abstract "User" entity collapses to after
 * memory_make_canonical_name (lowercased, trimmed), which the alias helpers
 * link to the user's own entity.  Not a name: a query saying "user" doesn't
 * name the user (memory_db_entity_alias_names leaves it out). */
#define MEMORY_ALIAS_ABSTRACT_USER_CANONICAL "user"

/**
 * @brief One row of the alias listing for a canonical entity (active aliases
 * only — unlinked rows are filtered).  Populated by
 * memory_db_entity_alias_list().
 */
typedef struct {
   int64_t link_id;
   int64_t source_entity_id; /**< 0 if hard-merged and source row is gone */
   char source_canonical_name[MEMORY_ENTITY_NAME_MAX];
   char link_kind[8];     /**< "soft" / "hard" */
   float composite_score; /**< -1.0f if NULL in DB */
   int64_t linked_at;
} memory_alias_listing_row_t;

/**
 * @brief One row of the audit timeline for an entity (active + unlinked).
 * Populated by memory_db_entity_alias_history().
 */
typedef struct {
   int64_t link_id;
   char source_canonical_name[MEMORY_ENTITY_NAME_MAX];
   char target_canonical_name[MEMORY_ENTITY_NAME_MAX];
   char link_kind[8];
   char reason[16];
   int64_t linked_at;
   int64_t unlinked_at; /**< 0 if still active */
   char unlink_reason[24];
} memory_alias_history_row_t;

/**
 * @brief One row of an entity listing.  Populated by
 * memory_db_entity_list_for_admin().
 */
typedef struct {
   int64_t entity_id;
   char name[MEMORY_ENTITY_NAME_MAX];
   char entity_type[MEMORY_ENTITY_TYPE_MAX];
   int mention_count;
   bool is_user_self;
   bool is_alias;        /**< true if canonical_id IS NOT NULL */
   int64_t canonical_id; /**< parent canonical's id when is_alias=true; 0 otherwise */
} memory_alias_entity_row_t;

/**
 * @brief List active soft-link aliases of a canonical entity.
 *
 * @param user_id User ID (ownership check)
 * @param target_entity_id Canonical entity to enumerate aliases for
 * @param out Output array
 * @param max Maximum results
 * @param count_out Output: number populated
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_entity_alias_list(int user_id,
                                int64_t target_entity_id,
                                memory_alias_listing_row_t *out,
                                int max,
                                int *count_out);

/**
 * @brief Full audit timeline (active + unlinked) for an entity, querying
 * memory_entity_aliases on either source_entity_id or target_entity_id.
 *
 * @param user_id User ID (ownership check)
 * @param entity_id Entity to enumerate audit rows for
 * @param out Output array
 * @param max Maximum results
 * @param count_out Output: number populated
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_entity_alias_history(int user_id,
                                   int64_t entity_id,
                                   memory_alias_history_row_t *out,
                                   int max,
                                   int *count_out);

/**
 * @brief List entities for the user.  Default mode is canonical-only,
 * mention_count DESC.  When @p include_aliases is true the function runs a
 * second pass for aliases (canonical_id IS NOT NULL) ordered by canonical_id
 * ASC, so the caller can render aliases nested under their canonical without
 * starving them out of the row budget — the historic single-query path
 * truncated all aliases off the tail when the canonical count saturated the
 * @p max slots.  Aliases are appended after canonicals and have their parent
 * canonical's id in @p canonical_id.
 *
 * @p include_aliases is API-clarity, not a perf flag — Pass 2's overhead at
 * typical scale is one prepared statement and a few hundred rows, dwarfed
 * by the lock-and-prepare cost of Pass 1.  Pass `false` only when the
 * caller has no use for alias rows (e.g., legacy callers that pre-date the
 * soft-merge surface).
 *
 * @param user_id User ID
 * @param include_aliases When true, append alias rows (canonical_id != 0) after
 *                        the canonicals.
 * @param out Output array
 * @param max Maximum results across both passes
 * @param count_out Output: number populated.  Undefined on a non-SUCCESS
 *                  return — callers must check the return code before
 *                  trusting this value (currently `*count_out` is left
 *                  holding any partial Pass-1 count if Pass-2 prepare
 *                  fails, but that is an implementation detail not a
 *                  contract).
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_entity_list_for_admin(int user_id,
                                    bool include_aliases,
                                    memory_alias_entity_row_t *out,
                                    int max,
                                    int *count_out);

/**
 * @brief One row of the pending-proposal listing — joined to memory_entities
 * so the WebUI can render readable source/target names without a second
 * round-trip.  Populated by memory_db_proposal_list_pending().
 */
typedef struct {
   int64_t proposal_id;
   int64_t source_entity_id;
   int64_t target_entity_id;
   char source_canonical_name[MEMORY_ENTITY_NAME_MAX];
   char target_canonical_name[MEMORY_ENTITY_NAME_MAX];
   float composite_score;
   int64_t proposed_at;
} memory_alias_proposal_row_t;

/**
 * @brief List pending merge proposals for a user (resolved_at IS NULL),
 * sorted by proposed_at DESC.  Joined against memory_entities so the
 * source / target canonical_name fields are populated for display.
 *
 * @param user_id User ID
 * @param out Output array
 * @param max Maximum results
 * @param count_out Output: number populated
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_proposal_list_pending(int user_id,
                                    memory_alias_proposal_row_t *out,
                                    int max,
                                    int *count_out);

/**
 * @brief Count pending merge proposals for a user (resolved_at IS NULL).
 *
 * Lightweight COUNT(*) for the WebUI proposal-pending dot indicator and
 * any other path that just wants the cardinality without the full row
 * payload.  Cheap because of idx_memory_entity_merge_proposals_pending
 * (partial index on resolved_at IS NULL).
 *
 * @param user_id    User ID
 * @param count_out  Output: number of pending proposals
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE (count_out left at 0)
 */
int memory_db_proposal_count_pending(int user_id, int *count_out);

/**
 * @brief Look up the user_self anchor entity id for @p user_id.
 *
 * Thin getter over the partial UNIQUE index on memory_entities.is_user_self.
 * Returns the entity id of the row with is_user_self=1 for the user, or
 * 0 if no anchor has been set yet.  Used by extraction-time gates that
 * want to skip the auto-promote helper once an anchor exists (saving
 * N redundant lock acquisitions per extraction).
 *
 * @param user_id   User ID
 * @param out_id    Output: entity id (0 if no anchor)
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_entity_get_user_self_id(int user_id, int64_t *out_id);

/**
 * @brief Auto-promote a freshly-extracted entity to is_user_self=1 when its
 * canonical name matches the user's real_name (or one of their identity
 * aliases) AND no user_self row exists yet for the user.
 *
 * Called at extraction time so clean installs get the user_self_bonus
 * applied automatically — the `dawn-admin memory entity link-user-self`
 * CLI becomes optional rather than required.  Idempotent: no-op when a
 * user_self already exists or when the user has no real_name configured.
 * Compares canonical forms (memory_make_canonical_name applied to both
 * sides) so casing / whitespace variations don't miss the match.
 *
 * Identity_aliases (newline-separated) are also tried — `Jon` vs
 * `Jonathan Smith` works if either is the canonical and the other is
 * a configured alias.
 *
 * @param user_id          User ID
 * @param entity_id        The fresh entity being evaluated
 * @param canonical_name   Pre-computed canonical (lowercased) form
 * @param out_promoted     Optional: set true if the row was promoted
 * @return MEMORY_DB_SUCCESS even when no-op; MEMORY_DB_FAILURE on real error
 */
int memory_db_entity_maybe_auto_promote_user_self(int user_id,
                                                  int64_t entity_id,
                                                  const char *canonical_name,
                                                  bool *out_promoted);

/**
 * @brief Sweep existing entities for one matching the user's real_name (or
 * an identity_alias) and promote it to is_user_self=1.
 *
 * Counterpart to memory_db_entity_maybe_auto_promote_user_self() for the
 * case where real_name is set AFTER entities have already been created
 * (the common WebUI flow: operator goes to Settings, fills in their real
 * name, expects the system to recognise their existing canonical without
 * having to run a CLI command).  No-op when a user_self already exists
 * or no matching canonical is found — the maybe-promote helper will pick
 * it up at the next extraction.
 *
 * Does NOT propose merges for related entities — that's still the
 * link-user-self CLI's job for the heavy cluster-resolution flow.  This
 * helper does only the surgical "this row is me" promotion.
 *
 * @param user_id        User ID
 * @param out_promoted   Optional: set true if a row was promoted
 * @return MEMORY_DB_SUCCESS regardless of outcome (caller treats no-op
 *         as success); MEMORY_DB_FAILURE only on real DB errors
 */
int memory_db_entity_auto_promote_user_self_by_real_name(int user_id, bool *out_promoted);

/**
 * @brief Auto-alias a freshly-extracted abstract "user" entity to the
 * existing user_self anchor.
 *
 * The extraction LLM commonly emits relations like `(User, married_to, X)`
 * which materialise as a generic entity row with `canonical_name='user'`.
 * That row has zero token overlap with the anchor's canonical (e.g.
 * `kris kersey`), so the Phase 2 cascade can't surface it as a merge
 * candidate.  This helper closes the gap: when a fresh `user` row appears
 * AND a user_self anchor already exists, soft-link the row as an alias of
 * the anchor.  Definitionally the same entity — highest-confidence merge
 * in the system, bypasses the cascade and runs without operator review.
 *
 * Match criteria (after @ref memory_make_canonical_name normalization):
 *   - exact equality with the literal "user" (single token)
 *   - plural / phrasal forms ("users", "the user") deliberately not matched
 *
 * No-op when:
 *   - no user_self anchor exists for the user (the
 *     maybe_auto_promote_user_self path handles that side instead)
 *   - @p canonical_name is not the literal "user"
 *   - @p entity_id IS the user_self anchor itself (edge case where
 *     real_name was set to "user")
 *   - @p entity_id has dependents (alias_link's single-level invariant)
 *
 * Idempotent on benign races (e.g. row gone between check and link).
 *
 * @param user_id          User ID
 * @param entity_id        The fresh entity being evaluated
 * @param canonical_name   Pre-computed canonical (lowercased) form
 * @param out_aliased      Optional: set true if the row was aliased
 * @return MEMORY_DB_SUCCESS even when no-op; MEMORY_DB_FAILURE on real error
 */
int memory_db_entity_maybe_alias_user_to_self(int user_id,
                                              int64_t entity_id,
                                              const char *canonical_name,
                                              bool *out_aliased);

/**
 * @brief Sweep for an already-existing abstract "user" canonical and alias
 * it to the user_self anchor.
 *
 * Counterpart to @ref memory_db_entity_maybe_alias_user_to_self for the
 * settings-save flow: after the operator sets `users.real_name` and
 * @ref memory_db_entity_auto_promote_user_self_by_real_name promotes a
 * matching row to the anchor, any pre-existing generic `user` canonical
 * left behind from earlier extractions needs to be attached too.
 *
 * No-op when no user_self anchor exists or no canonical `user` row is
 * found (extraction's maybe-alias path will pick it up on the next
 * conversation).  Honours the same single-level alias invariant as the
 * sibling — refuses if the would-be source has dependents.
 *
 * @param user_id        User ID
 * @param out_aliased    Optional: set true if a row was aliased
 * @return MEMORY_DB_SUCCESS regardless of outcome (caller treats no-op
 *         as success); MEMORY_DB_FAILURE only on real DB errors
 */
int memory_db_entity_alias_existing_user_to_self(int user_id, bool *out_aliased);

/**
 * @brief Resolve a pending merge proposal.
 *
 * On @p approved = true, runs both writes inside a single BEGIN IMMEDIATE
 * transaction:
 *   1. memory_db_entity_alias_link with reason="operator-approved-from-proposal"
 *   2. UPDATE memory_entity_merge_proposals SET resolved_at = now(),
 *      resolution = 'approved'
 * On @p approved = false, only the UPDATE fires with resolution='rejected'.
 *
 * Refuses already-resolved proposals (returns MEMORY_DB_NOT_FOUND).  Refuses
 * if the underlying alias_link would refuse (e.g. source has dependents) —
 * the proposal stays unresolved so the operator can reject it explicitly.
 *
 * @param user_id User ID (ownership check)
 * @param proposal_id Target proposal
 * @param approved true = approve + soft-link, false = reject (stamp only)
 * @param out_link_id On approve, populated with the new alias link id (may be NULL)
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND, or MEMORY_DB_FAILURE
 */
int memory_db_proposal_resolve(int user_id,
                               int64_t proposal_id,
                               bool approved,
                               int64_t *out_link_id);

/**
 * @brief One row of the bulk alias-summary listing.  Identifies an alias
 * row (`canonical_id IS NOT NULL`) and the canonical it points at.  Used by
 * the WebUI Graph tab to drop alias rows from the canonical list and tally
 * per-canonical alias counts in a single SQL round-trip.
 */
typedef struct {
   int64_t alias_entity_id;     /**< memory_entities.id (the alias row) */
   int64_t canonical_entity_id; /**< memory_entities.canonical_id (the target) */
} memory_alias_summary_row_t;

/**
 * @brief List all currently-active aliases for a user.
 *
 * Returns one row per `memory_entities` row where `canonical_id IS NOT
 * NULL`.  Driven by the partial index `idx_memory_entities_canonical`.
 * Cheap (a few microseconds at the dev's 2k-entity scale).
 *
 * @param user_id User ID
 * @param out Output array
 * @param max Maximum results
 * @param count_out Output: number populated
 * @return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE
 */
int memory_db_entity_alias_summary(int user_id,
                                   memory_alias_summary_row_t *out,
                                   int max,
                                   int *count_out);

/**
 * @brief Outcome bookkeeping for a single entity-vs-user_self evaluation
 * during link_user_self_run().  Lined up so the orchestrator can render a
 * tabular report.
 */
typedef struct {
   int64_t entity_id;
   char canonical_name[MEMORY_ENTITY_NAME_MAX];
   char entity_type[MEMORY_ENTITY_TYPE_MAX];
   int outcome; /**< MEMORY_ALIAS_OUTCOME_* */
   float composite_score;
   /**< Phase 1.5 fold-in: surface the user_self_bonus flag from the
    *   per-candidate evidence so callers can verify the bonus fired —
    *   particularly relevant for the canonical-name='user' allow-list
    *   token, whose composite is otherwise indistinguishable from "no
    *   match" without inspecting the bonus signal directly. */
   bool user_self_bonus_applied;
   int64_t link_id;     /**< Set when outcome = AUTO_MERGED */
   int64_t proposal_id; /**< Set when outcome = PROPOSED */
} memory_alias_link_user_self_row_t;

#define MEMORY_ALIAS_LINK_USER_SELF_MAX_ROWS 256

typedef struct {
   int64_t self_entity_id; /**< The user-self canonical (existing or just-seeded) */
   bool self_was_seeded;   /**< true if a fresh row was created vs reused/promoted */
   /**< true if an existing entity was promoted (is_user_self=1 set) instead of
    *   inserting a fresh seed.  Implies !self_was_seeded.  Per design §8 Path B
    *   step 1: when an existing entity matches the synthetic strongly enough,
    *   USE that entity as canonical rather than creating a duplicate row. */
   bool self_was_promoted;
   char self_canonical_name[MEMORY_ENTITY_NAME_MAX];
   int considered; /**< Total entities scored (excludes self) */
   int auto_merged;
   int proposed;
   int rejected;
   memory_alias_link_user_self_row_t rows[MEMORY_ALIAS_LINK_USER_SELF_MAX_ROWS];
   int row_count;
} memory_alias_link_user_self_result_t;

/**
 * @brief Run the §8 Path B backfill: find or create the user-self canonical,
 * score every other entity for the user against the resolver, and route by
 * threshold band.
 *
 * Side effects in @p dry_run = false mode:
 *   - if the user has no is_user_self=1 row, create one (display-name-from-
 *     username) and stamp it with is_user_self=1.  Sets @p result->self_was_seeded.
 *   - for each candidate scoring ≥ MEMORY_ALIAS_AUTO_THRESHOLD: write a soft
 *     link via memory_db_entity_alias_link().
 *   - for each candidate in the review band: write a row to
 *     memory_entity_merge_proposals.
 *
 * In @p dry_run = true mode: no DB writes occur (no canonical seeded, no
 * links written, no proposals queued).  The @p result is populated as if
 * the writes had happened so the caller can render an accurate preview.
 *
 * @param user_id User to backfill (> 0)
 * @param dry_run When true, perform no DB mutations
 * @param result Output struct (populated even on partial failure)
 * @return MEMORY_DB_SUCCESS, MEMORY_DB_NOT_FOUND (user missing), or
 *         MEMORY_DB_FAILURE
 */
int memory_alias_link_user_self_run(int user_id,
                                    bool dry_run,
                                    memory_alias_link_user_self_result_t *result);

/**
 * @brief qsort()-shaped comparator for memory_alias_link_user_self_row_t.
 *
 * Sorts rows by composite_score DESC, with mention_count DESC tiebreak.
 * Used by the dry-run report renderer to surface closest-match candidates
 * first (otherwise high-mention generic entities like "user" / "dawn"
 * appear before actual cluster members in the rejected-section listing).
 *
 * @param a const memory_alias_link_user_self_row_t *
 * @param b const memory_alias_link_user_self_row_t *
 * @return negative / 0 / positive per qsort(3) contract
 */
int memory_alias_row_compare_by_composite_desc(const void *a, const void *b);

/**
 * @brief Compare two entities by the canonical_priority tuple — TYPE-AND-AGE
 * AXES ONLY.
 *
 * @warning memory_entity_t does not carry the @c is_user_self column.  This
 * function compares only the lower four axes:
 *   (type_specificity, mention_count, first_seen, id)
 * with type_specificity person=4 > place=3 = pet=3 = org=3 > thing=1.  If
 * either entity in the cluster might have @c is_user_self = 1, callers
 * MUST use @ref memory_alias_canonical_priority_compare_self instead — that
 * variant short-circuits on the user-self axis (highest priority per design
 * §9) before delegating to the four axes here.
 *
 * Returns a negative value if @p a should be canonical over @p b, zero if
 * they compare equal at every level (only happens for identical id), and
 * a positive value if @p b should be canonical over @p a.  Pure function;
 * no DB access; safe to call without auth_db lock.
 *
 * @param a First entity
 * @param b Second entity
 * @return < 0 if a wins, 0 if tied, > 0 if b wins
 */
int memory_alias_canonical_priority_compare(const memory_entity_t *a, const memory_entity_t *b);

/**
 * @brief Compare two entities by the FULL canonical_priority tuple from
 * design §9, including the @c is_user_self axis at the highest priority.
 *
 * Tuple (DESC for is_user_self / type_specificity / mention_count, ASC for
 * first_seen / id):
 *   (is_user_self, type_specificity, mention_count, first_seen, id)
 *
 * Used by the cascade resolver as a tiebreaker among candidates that
 * scored at the same composite_score above the auto-merge threshold —
 * the user_self_bonus usually pushes the seeded user-self entity to a
 * strictly higher composite, but for the rare exact tie the comparator
 * provides defense-in-depth so the order-independence invariant (§9)
 * holds regardless of insertion order.
 *
 * Short-circuits on the @c is_user_self difference; otherwise delegates
 * to @ref memory_alias_canonical_priority_compare for the remaining axes.
 * Pure function; no DB access; safe to call without auth_db lock.
 *
 * @param a First entity
 * @param a_is_user_self True if entity @p a has @c is_user_self = 1 in DB
 * @param b Second entity
 * @param b_is_user_self True if entity @p b has @c is_user_self = 1 in DB
 * @return < 0 if a wins, 0 if tied, > 0 if b wins
 */
int memory_alias_canonical_priority_compare_self(const memory_entity_t *a,
                                                 bool a_is_user_self,
                                                 const memory_entity_t *b,
                                                 bool b_is_user_self);

/* =============================================================================
 * Pure-function helpers exposed for unit testing
 *
 * These don't touch the DB and aren't intended for external use beyond
 * tests, but are public so test_memory_db_alias.c can exercise them in
 * isolation without a fixture.
 * ============================================================================= */

/**
 * @brief Compute token-Jaccard between two canonical names.  Tokens are
 * whitespace + ASCII-punctuation split; tokens of length < 2 are dropped.
 * Returns 0.0 for any NULL/empty input.
 */
float memory_alias_compute_name_jaccard(const char *a, const char *b);

/**
 * @brief True iff one canonical_name is a strict (non-equal) substring of
 * the other.  Returns false for equal strings, NULL inputs, or empty inputs.
 */
bool memory_alias_compute_name_substring(const char *a, const char *b);

/**
 * @brief Apply the §7 composite formula to a populated evidence struct.
 *
 * Mutates `evidence->composite_score`.  Veto rule + weighted sum + bonuses
 * + clamp-to-[0, 1].  Pure function; no DB access.
 */
void memory_alias_apply_composite(memory_alias_evidence_t *evidence);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_DB_ALIASES_H */
