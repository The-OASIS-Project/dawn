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
 * Memory Database — entity-merge cascade (design §6 Stages 1-6).
 *
 * Phase 6b split from memory_db_alias.c — the six-stage resolver cascade
 * (exact-match → token-Jaccard candidates → type filter → embedding
 * cosine → exclusive-relation / contact overlap → composite-band routing)
 * plus the two public resolver entry points that wrap it.
 *
 * Tripwire kept at this TU because cascade_internal is the function the
 * insertion sort lives inside. */

#define AUTH_DB_INTERNAL_ALLOWED

#include <string.h>

#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_db_alias_internal.h"
#include "memory/memory_db_aliases.h"
#include "memory/memory_db_entities.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_types.h"
#include "utils/string_utils.h"

/* Tripwire: the cascade's insertion sort on the scored array (see
 * cascade_internal) is justified by N being tiny.  If MEMORY_ALIAS_STAGE5_
 * MAX_CANDIDATES ever bumps above 16, the O(N²) cost becomes worth
 * replacing with a partial heap-sort.  Catch the bump at compile time. */
_Static_assert(MEMORY_ALIAS_STAGE5_MAX_CANDIDATES <= 16,
               "scored-array insertion sort assumes small N (<=16)");

/* =============================================================================
 * Cascade — Stage 1 through Stage 6 per design §6
 *
 * Two entry points:
 *   - resolve_alias()      : surface-form lookup, no inbound entity_id.  Stage
 *                            5 contributes 0 (nothing to compare on the
 *                            inbound side).  Returns the canonical the surface
 *                            form should bind to (or 0 = no resolution).
 *   - consider_auto_merge() : a freshly-created entity is the inbound; full
 *                             cascade including Stage 5 against existing rows.
 * ============================================================================= */

/* Stage 1: exact canonical_name match.  Returns the resolved canonical id
 * (post-COALESCE on canonical_id) on hit, or 0 on miss. */
static int stage1_exact_match(int user_id,
                              const char *canonical_name,
                              int64_t exclude_id,
                              int64_t *out_resolved_id) {
   *out_resolved_id = 0;
   if (!canonical_name || !*canonical_name)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   const char *sql = "SELECT id, COALESCE(canonical_id, id) FROM memory_entities "
                     "WHERE user_id = ? AND canonical_name = ?";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, canonical_name, -1, SQLITE_TRANSIENT);

   int64_t resolved = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      int64_t hit_id = sqlite3_column_int64(stmt, 0);
      int64_t canonical = sqlite3_column_int64(stmt, 1);
      if (hit_id != exclude_id) {
         resolved = canonical;
      }
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   *out_resolved_id = resolved;
   return MEMORY_DB_SUCCESS;
}

/* Stage 2 helper: per-token candidate generator using existing keyword search.
 * Aggregates up to MEMORY_ALIAS_STAGE2_MAX_CANDIDATES unique IDs across all
 * tokens, computes name_jaccard for each, drops below floor. */
typedef struct {
   int64_t entity_id;
   char canonical_name[MEMORY_ENTITY_NAME_MAX];
   char entity_type[MEMORY_ENTITY_TYPE_MAX];
   int mention_count;
   time_t first_seen;
   bool is_user_self;
   /* Allow-listed self-reference token (currently just "user").  Receives
    * user_self_bonus regardless of username/alias substring conditions
    * per the Phase 1.5 design intent — synth_self_allow_list_user() sets
    * this true; standard Stage 2 candidates leave it false. */
   bool is_user_self_token;
   float name_jaccard;
   float embedding_cosine;
} alias_candidate_t;

static int stage2_candidates(int user_id,
                             const char *canonical_name,
                             int64_t exclude_id,
                             bool use_synth_self,
                             alias_candidate_t *out,
                             int max,
                             int *out_count) {
   /* Append-mode: respect any pre-added candidates (e.g. the synthetic-
    * self "user" allow-list).  Callers that want a fresh pool zero
    * *out_count before calling. */
   if (!canonical_name || !*canonical_name || max <= 0)
      return MEMORY_DB_SUCCESS; /* empty result, not an error */

   alias_token_set_t tokens;
   memory_alias_internal_tokenize(canonical_name, &tokens);
   if (tokens.count == 0)
      return MEMORY_DB_SUCCESS;

   /* Per-token keyword search via the existing entity-search helper.
    * Aggregate unique IDs, capped at @p max.  Seed seen_ids with any
    * pre-added candidates so we don't double-add the same entity. */
   int64_t seen_ids[MEMORY_ALIAS_STAGE2_MAX_CANDIDATES];
   int seen_count = 0;
   for (int p = 0; p < *out_count && p < MEMORY_ALIAS_STAGE2_MAX_CANDIDATES; p++) {
      seen_ids[seen_count++] = out[p].entity_id;
   }

   for (int t = 0; t < tokens.count && seen_count < max; t++) {
      memory_entity_t hits[8];
      int hit_count = 0;
      if (memory_db_entity_search(user_id, tokens.tokens[t], hits, 8, &hit_count) !=
          MEMORY_DB_SUCCESS) {
         continue;
      }
      for (int i = 0; i < hit_count && seen_count < max; i++) {
         if (hits[i].id == exclude_id)
            continue;
         /* Dedup against seen_ids. */
         bool dup = false;
         for (int s = 0; s < seen_count; s++) {
            if (seen_ids[s] == hits[i].id) {
               dup = true;
               break;
            }
         }
         if (dup)
            continue;
         seen_ids[seen_count++] = hits[i].id;
      }
   }

   /* Score each candidate by jaccard; drop below floor; populate `out`. */
   for (int i = 0; i < seen_count; i++) {
      memory_entity_t e;
      bool is_self = false;
      int64_t canon_id = 0;
      if (memory_alias_internal_load_entity_full(user_id, seen_ids[i], &e, &canon_id, &is_self) !=
          MEMORY_DB_SUCCESS) {
         continue;
      }
      /* Skip alias rows — they're not canonical candidates.  (At Ckpt 2
       * time no aliases exist in production yet, but link-user-self in
       * Ckpt 4 will create them, so the filter belongs here too — matches
       * design §15's "cache loader filter" cohort, applied at the resolver
       * level since the cache filter ships in Ckpt 3.) */
      if (canon_id != 0)
         continue;

      /* Phase 1.5 Ckpt C: directional overlap when scoring against the
       * synthetic-self seed (verbose canonical from real_name + aliases),
       * standard Jaccard otherwise.  Both apply the same 0.30 floor. */
      float overlap;
      if (use_synth_self) {
         overlap = memory_alias_internal_directional_overlap(canonical_name, e.canonical_name);
      } else {
         overlap = memory_alias_compute_name_jaccard(canonical_name, e.canonical_name);
      }
      if (overlap < MEMORY_ALIAS_NAME_JACCARD_FLOOR) {
         /* Substring rescue (Phase 2): short ↔ long name variants like
          * "jon" ⊂ "jonathan smith" or "dawn" ⊂ "dawn smith"
          * have Jaccard ≈ 0 (no shared whole-word tokens) and would be
          * dropped here before Stage 4's embedding match can fire.  Admit
          * them as candidates when one canonical is a character-level
          * substring of the other so Stages 4-6 can score them properly.
          * The +0.10 substring bonus in Stage 6 then lifts the composite
          * into the review (≥0.70) or auto (≥0.90) band ONLY when other
          * signals (embedding cosine, exclusive-relation overlap, contact
          * field overlap) corroborate — substring alone won't merge
          * unrelated names that happen to share a prefix. */
         if (!memory_alias_compute_name_substring(canonical_name, e.canonical_name))
            continue;
      }

      alias_candidate_t *c = &out[*out_count];
      c->entity_id = e.id;
      safe_strscpy(c->canonical_name, e.canonical_name);
      safe_strscpy(c->entity_type, e.entity_type);
      c->mention_count = e.mention_count;
      c->first_seen = e.first_seen;
      c->is_user_self = is_self;
      c->name_jaccard = overlap;
      c->embedding_cosine = 0.0f;
      (*out_count)++;
   }
   return MEMORY_DB_SUCCESS;
}

/* Stage 2b: reverse-substring candidate generator.  Finds entities whose
 * canonical_name is a char-level substring of @p canonical_name (e.g.,
 * existing "jon" entity when inbound is "jonathan smith").  The forward
 * stage2_candidates() pathway tokenises the inbound and runs LIKE '%token%'
 * for each, which only finds entities containing one of the inbound's
 * tokens.  That misses the short-form-already-exists, long-form-arrives-
 * later case entirely — the existing "Jon" row never contains "jonathan"
 * or "smith".  This helper closes the gap.
 *
 * Idempotent against pre-added seen_ids.  Appends to @p out up to @p max.
 * Skips alias rows (canonical_id IS NULL filter) so the resolver stays on
 * canonicals only.  Capped via ORDER BY mention_count DESC so high-traffic
 * canonicals win when the inbound is unusually long and many short names
 * happen to fit.  No-op when inbound canonical_name is empty or already
 * past the candidate cap. */
/* Inbound shorter than this can't usefully match any candidate via
 * reverse-substring: there's nothing meaningful Stage 2b can find that
 * Stage 2's forward per-token search didn't already pick up.  Skipping
 * also avoids the per-extraction scan cost for short queries.  The cap
 * is per-character (post-canonicalization), not per-token. */
#define MEMORY_ALIAS_STAGE2B_MIN_INBOUND_LEN 8

static int stage2_reverse_substring_candidates(int user_id,
                                               const char *canonical_name,
                                               int64_t exclude_id,
                                               alias_candidate_t *out,
                                               int max,
                                               int *out_count) {
   if (!canonical_name || !*canonical_name || max <= 0 || *out_count >= max)
      return MEMORY_DB_SUCCESS;

   /* Short-inbound skip — substring scan on a 4-char inbound finds
    * 1-3 char fragments that aren't real candidates. */
   size_t inbound_len = strlen(canonical_name);
   if (inbound_len < MEMORY_ALIAS_STAGE2B_MIN_INBOUND_LEN)
      return MEMORY_DB_SUCCESS;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   /* instr(?, canonical_name) > 0 means canonical_name appears as a
    * substring inside the inbound.  Two length predicates bound the scan:
    *   length(canonical_name) > 1           — guards trivial 1-char hits
    *   length(canonical_name) <= length(?)  — a canonical longer than the
    *                                          inbound can't be a substring
    *                                          of it; lets the planner drop
    *                                          obviously-too-long rows
    *                                          without running instr().
    * Both are unindexable scalar predicates so they don't change the row-
    * scan plan, but they cut instr() calls roughly in half on a real
    * corpus where canonical lengths vary widely.  Excludes the inbound
    * itself and any soft-aliases (canonical_id IS NOT NULL). */
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT id FROM memory_entities "
                          "WHERE user_id = ? AND id != ? AND canonical_id IS NULL "
                          "  AND length(canonical_name) > 1 "
                          "  AND length(canonical_name) <= ? "
                          "  AND instr(?, canonical_name) > 0 "
                          "ORDER BY mention_count DESC "
                          "LIMIT ?",
                          -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, exclude_id);
   sqlite3_bind_int(stmt, 3, (int)inbound_len);
   sqlite3_bind_text(stmt, 4, canonical_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 5, max);

   int64_t hits[MEMORY_ALIAS_STAGE2_MAX_CANDIDATES];
   int hit_count = 0;
   while (hit_count < max && sqlite3_step(stmt) == SQLITE_ROW) {
      hits[hit_count++] = sqlite3_column_int64(stmt, 0);
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   /* Append unique hits to the candidate pool, skipping those already
    * present from the forward pass. */
   for (int i = 0; i < hit_count && *out_count < max; i++) {
      bool dup = false;
      for (int p = 0; p < *out_count; p++) {
         if (out[p].entity_id == hits[i]) {
            dup = true;
            break;
         }
      }
      if (dup)
         continue;

      memory_entity_t e;
      bool is_self = false;
      int64_t canon_id = 0;
      if (memory_alias_internal_load_entity_full(user_id, hits[i], &e, &canon_id, &is_self) !=
          MEMORY_DB_SUCCESS)
         continue;
      if (canon_id != 0)
         continue; /* defensive: alias filter already in SQL, but re-check */

      alias_candidate_t *c = &out[*out_count];
      c->entity_id = e.id;
      safe_strscpy(c->canonical_name, e.canonical_name);
      safe_strscpy(c->entity_type, e.entity_type);
      c->mention_count = e.mention_count;
      c->first_seen = e.first_seen;
      c->is_user_self = is_self;
      c->is_user_self_token = false;
      /* name_jaccard stays at 0 — the candidate didn't earn it via Stage 2's
       * forward Jaccard.  Stage 6's pair scoring re-computes it anyway. */
      c->name_jaccard = 0.0f;
      c->embedding_cosine = 0.0f;
      (*out_count)++;
   }
   return MEMORY_DB_SUCCESS;
}

/* Stage 3: type-filter survivors in place.  Compacts the array by removing
 * candidates that fail the type-match rule (`thing` carve-out applies). */
static void stage3_type_filter(const char *inbound_type, alias_candidate_t *cands, int *count) {
   int kept = 0;
   for (int i = 0; i < *count; i++) {
      if (memory_alias_internal_type_veto_fires(inbound_type, cands[i].entity_type)) {
         continue; /* drop */
      }
      if (kept != i) {
         cands[kept] = cands[i];
      }
      kept++;
   }
   *count = kept;
}

/* Stage 4: embed inbound canonical_name once, compute cosine vs each
 * candidate's cached embedding, drop those below the cosine floor.
 * Caps the survivor list at MEMORY_ALIAS_STAGE4_MAX_CANDIDATES. */
static void stage4_cosine(int user_id,
                          const char *canonical_name,
                          alias_candidate_t *cands,
                          int *count) {
   if (!memory_embeddings_available()) {
      /* No engine — Stage 4 is a no-op; pass survivors through unchanged
       * but truncate to cap so Stage 5 stays bounded. */
      if (*count > MEMORY_ALIAS_STAGE4_MAX_CANDIDATES)
         *count = MEMORY_ALIAS_STAGE4_MAX_CANDIDATES;
      return;
   }

   float query_emb[MAX_EMBEDDING_DIMS];
   int dims = 0;
   if (memory_embeddings_embed(canonical_name, query_emb, &dims) != 0 || dims <= 0) {
      if (*count > MEMORY_ALIAS_STAGE4_MAX_CANDIDATES)
         *count = MEMORY_ALIAS_STAGE4_MAX_CANDIDATES;
      return;
   }
   float norm = memory_embeddings_l2_norm(query_emb, dims);

   int kept = 0;
   for (int i = 0; i < *count; i++) {
      float cos = 0.0f;
      if (memory_embeddings_entity_cosine(user_id, cands[i].entity_id, query_emb, dims, norm,
                                          &cos) == SUCCESS) {
         cands[i].embedding_cosine = cos;
      } else {
         /* No cached embedding for this candidate — score 0. */
         cands[i].embedding_cosine = 0.0f;
      }
      if (cands[i].embedding_cosine < MEMORY_ALIAS_COSINE_FLOOR)
         continue;
      if (kept != i)
         cands[kept] = cands[i];
      kept++;
   }
   *count = kept;
   if (*count > MEMORY_ALIAS_STAGE4_MAX_CANDIDATES)
      *count = MEMORY_ALIAS_STAGE4_MAX_CANDIDATES;
}

/* Sort candidates by partial composite (name_jaccard + embedding_cosine)
 * descending, then truncate to top N for Stage 5. */
static void rank_and_truncate(alias_candidate_t *cands, int *count, int top_n) {
   /* Insertion sort — N <= 8, simplest and warm for tiny arrays. */
   for (int i = 1; i < *count; i++) {
      alias_candidate_t tmp = cands[i];
      float key = tmp.name_jaccard + tmp.embedding_cosine;
      int j = i - 1;
      while (j >= 0 && (cands[j].name_jaccard + cands[j].embedding_cosine) < key) {
         cands[j + 1] = cands[j];
         j--;
      }
      cands[j + 1] = tmp;
   }
   if (*count > top_n)
      *count = top_n;
}

/* Internal cascade.
 * @p inbound_id: 0 if the inbound has no row yet (resolver pre-upsert),
 *                else the id of the inbound entity (consider_auto_merge).
 *                When non-zero, Stage 5 fires (full overlap signals).
 * @p inbound_is_user_self: only relevant when inbound_id != 0; ignored otherwise.
 *
 * Picks the highest-composite candidate; out_winner_id = 0 on no match.
 * out_winner_evidence holds the final composite for routing. */
/* Phase 1.5 Ckpt C: pre-add the canonical-name='user' entity to the
 * candidate pool when running synthetic-self resolve.
 *
 *   DB scan against /var/lib/dawn/auth.db (May 2026) found exactly one
 *   self-reference entity: 'user' (thing, 292 mentions).  No 'me' /
 *   'myself' / 'admin' / 'operator' / etc. exist as entities.  Extending
 *   this list requires evidence from a real DB scan, not theoretical
 *   alternatives.
 *
 * Adds the row before Stage 2 so it survives the directional-overlap
 * floor (which it would fail by name signal alone — "user" has no token
 * overlap with a real_name like "Jonathan Smith").  Stage 3 then
 * applies the type-veto rule (the carve-out for 'thing' type lets it
 * through against 'person' synthetics), Stage 4-6 apply normal scoring
 * + user_self_bonus.  If the entity doesn't exist in this user's graph
 * the function is a no-op. */
static void synth_self_allow_list_user(int user_id,
                                       int64_t exclude_id,
                                       alias_candidate_t *out,
                                       int max,
                                       int *count) {
   if (!out || max <= 0 || *count >= max)
      return;

   AUTH_DB_LOCK_OR_RETURN_VOID();
   sqlite3_stmt *stmt = NULL;
   const char *sql = "SELECT id FROM memory_entities "
                     "WHERE user_id = ? AND lower(canonical_name) = 'user' "
                     "  AND canonical_id IS NULL "
                     "LIMIT 1";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   int64_t hit_id = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      hit_id = sqlite3_column_int64(stmt, 0);
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   if (hit_id <= 0 || hit_id == exclude_id)
      return;

   /* Skip if already in pool. */
   for (int i = 0; i < *count; i++) {
      if (out[i].entity_id == hit_id)
         return;
   }

   /* Load full row + push.  memory_alias_internal_load_entity_full takes its own lock. */
   memory_entity_t e;
   bool is_self = false;
   int64_t canon_id = 0;
   if (memory_alias_internal_load_entity_full(user_id, hit_id, &e, &canon_id, &is_self) !=
       MEMORY_DB_SUCCESS)
      return;
   if (canon_id != 0)
      return; /* alias row — not a canonical candidate */

   alias_candidate_t *c = &out[*count];
   memset(c, 0, sizeof(*c));
   c->entity_id = e.id;
   safe_strscpy(c->canonical_name, e.canonical_name);
   safe_strscpy(c->entity_type, e.entity_type);
   c->mention_count = e.mention_count;
   c->first_seen = e.first_seen;
   c->is_user_self = is_self;
   /* Allow-listed self-reference token — Stage 6 will fire user_self_bonus
    * unconditionally (regardless of username substring or alias match).
    * Matches the Phase 1.5 brief's "passes through Stage 3-6 normally
    * with normal scoring + user_self_bonus" requirement. */
   c->is_user_self_token = true;
   /* Score via directional overlap so the entity isn't penalized by
    * name-signal at Stages 5-6.  For "user" against any verbose
    * synthetic, intersection = 0 → overlap = 0; the user_self_bonus
    * carries it instead. */
   c->name_jaccard = 0.0f;
   c->embedding_cosine = 0.0f;
   (*count)++;
}

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
                                  int *out_scored_count) {
   if (out_scored_count)
      *out_scored_count = 0;
   *out_winner_id = 0;
   *out_matched_stage = 0;
   memset(out_evidence, 0, sizeof(*out_evidence));

   /* Stage 1: exact canonical_name match (resolver-only fast path). */
   int64_t exact_id = 0;
   if (stage1_exact_match(user_id, canonical_name, inbound_id, &exact_id) == MEMORY_DB_SUCCESS &&
       exact_id > 0) {
      *out_winner_id = exact_id;
      *out_matched_stage = 1;
      /* Stage 1 hit — composite N/A; caller short-circuits without the
       * threshold band check. */
      return MEMORY_DB_SUCCESS;
   }

   /* Stage 2: token-Jaccard (or directional overlap, when synth-self)
    * candidate generation.  When synth-self, also pre-add the "user"
    * canonical-name allow-list entity. */
   alias_candidate_t cands[MEMORY_ALIAS_STAGE2_MAX_CANDIDATES];
   int cand_count = 0;
   if (use_synth_self) {
      synth_self_allow_list_user(user_id, inbound_id, cands, MEMORY_ALIAS_STAGE2_MAX_CANDIDATES,
                                 &cand_count);
   }
   stage2_candidates(user_id, canonical_name, inbound_id, use_synth_self, cands,
                     MEMORY_ALIAS_STAGE2_MAX_CANDIDATES, &cand_count);
   /* Stage 2b: reverse-substring sweep — finds short-form canonicals
    * (existing "jon") when the inbound is a long form ("jonathan
    * smith") whose tokens don't appear in any shorter canonical's name.
    * Skipped in synth_self mode: that path uses directional Jaccard
    * specifically tuned for the verbose synthetic seed and the "user"
    * allow-list already covers the short-form case for user-self. */
   if (!use_synth_self) {
      stage2_reverse_substring_candidates(user_id, canonical_name, inbound_id, cands,
                                          MEMORY_ALIAS_STAGE2_MAX_CANDIDATES, &cand_count);
   }
   if (cand_count == 0)
      return MEMORY_DB_SUCCESS; /* clean miss */

   /* Stage 3: type filter (in-place). */
   stage3_type_filter(entity_type, cands, &cand_count);
   if (cand_count == 0)
      return MEMORY_DB_SUCCESS;

   /* Stage 4: embedding cosine.  Drops cands < 0.50, caps at 8. */
   stage4_cosine(user_id, canonical_name, cands, &cand_count);
   if (cand_count == 0)
      return MEMORY_DB_SUCCESS;

   /* Rank by partial composite (jaccard + cosine), keep top 3 for Stage 5. */
   rank_and_truncate(cands, &cand_count, MEMORY_ALIAS_STAGE5_MAX_CANDIDATES);

   /* Stage 5 + Stage 6: full pair-score each candidate, keep best composite.
    * On exact composite ties — usually the rare case where multiple
    * candidates clear the auto-merge threshold simultaneously — break the
    * tie with canonical_priority_compare_self per design §9, so the
    * binding is order-independent regardless of insertion order. */
   float best_composite = -1.0f;
   int best_idx = -1;
   memory_alias_evidence_t best_ev;
   memset(&best_ev, 0, sizeof(best_ev));

   for (int i = 0; i < cand_count; i++) {
      memory_alias_evidence_t ev;
      memset(&ev, 0, sizeof(ev));

      /* Stage 5 signals only fire when the inbound has an entity_id. */
      if (inbound_id > 0) {
         ev.exclusive_relation_overlap = memory_alias_internal_compute_exclusive_relation_overlap(
             user_id, inbound_id, cands[i].entity_id);
         ev.contact_field_overlap = memory_alias_internal_compute_contact_field_overlap(
             user_id, inbound_id, cands[i].entity_id);
      }

      ev.name_jaccard = cands[i].name_jaccard;
      ev.embedding_cosine = cands[i].embedding_cosine;
      ev.type_match = memory_alias_internal_type_match_signal(entity_type, cands[i].entity_type);
      ev.type_veto_fired = memory_alias_internal_type_veto_fires(entity_type, cands[i].entity_type);
      ev.name_substring_bonus_applied = memory_alias_compute_name_substring(
          canonical_name, cands[i].canonical_name);
      /* In synth-self mode the inbound is the synthetic (user_self side)
       * and the candidate is the "other" — pass the candidate's allow-
       * list flag through so the unconditional-bonus branch inside
       * user_self_bonus_applies fires for the "user" canonical. */
      bool other_token = inbound_is_user_self ? cands[i].is_user_self_token : false;
      ev.user_self_bonus_applied = memory_alias_internal_user_self_bonus_applies(
          user_id, inbound_is_user_self, cands[i].is_user_self, canonical_name,
          cands[i].canonical_name, ev.contact_field_overlap > 0.0f, other_token);

      memory_alias_apply_composite(&ev);

      bool replace = false;
      if (ev.composite_score > best_composite) {
         replace = true;
      } else if (best_idx >= 0 && ev.composite_score == best_composite) {
         /* Composite tie — disambiguate via canonical_priority_compare_self.
          * Synthesize memory_entity_t shims from the candidate's stored
          * fields (alias_candidate_t carries enough). */
         memory_entity_t cur, cand;
         memset(&cur, 0, sizeof(cur));
         memset(&cand, 0, sizeof(cand));
         cur.id = cands[best_idx].entity_id;
         safe_strscpy(cur.entity_type, cands[best_idx].entity_type);
         cur.mention_count = cands[best_idx].mention_count;
         cur.first_seen = cands[best_idx].first_seen;
         cand.id = cands[i].entity_id;
         safe_strscpy(cand.entity_type, cands[i].entity_type);
         cand.mention_count = cands[i].mention_count;
         cand.first_seen = cands[i].first_seen;
         /* Returns > 0 when @p b (the new candidate) should be canonical. */
         if (memory_alias_canonical_priority_compare_self(&cur, cands[best_idx].is_user_self, &cand,
                                                          cands[i].is_user_self) > 0) {
            replace = true;
         }
      }
      if (replace) {
         best_composite = ev.composite_score;
         best_idx = i;
         best_ev = ev;
      }

      /* Also remember EVERY scored candidate for the multi-proposal path.
       * Bounded by out_scored_max; we drop late entries when the array is
       * full (the cascade already truncates to STAGE5_MAX_CANDIDATES so the
       * total is small). */
      if (out_scored && out_scored_count && *out_scored_count < out_scored_max) {
         out_scored[*out_scored_count].entity_id = cands[i].entity_id;
         out_scored[*out_scored_count].evidence = ev;
         (*out_scored_count)++;
      }
   }

   /* Sort the scored array by composite DESC so callers can iterate from
    * strongest match to weakest without re-sorting.  Insertion sort —
    * MEMORY_ALIAS_STAGE5_MAX_CANDIDATES is tiny so the O(n²) cost is
    * negligible and keeps the routine alloc-free. */
   if (out_scored && out_scored_count && *out_scored_count > 1) {
      for (int i = 1; i < *out_scored_count; i++) {
         scored_candidate_t tmp = out_scored[i];
         int j = i - 1;
         while (j >= 0 && out_scored[j].evidence.composite_score < tmp.evidence.composite_score) {
            out_scored[j + 1] = out_scored[j];
            j--;
         }
         out_scored[j + 1] = tmp;
      }
   }

   if (best_idx >= 0 && best_composite > 0.0f) {
      *out_winner_id = cands[best_idx].entity_id;
      *out_evidence = best_ev;
      *out_matched_stage = 6;
   }
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * Public API impl — resolver entry points
 * ============================================================================= */

int memory_db_entity_resolve_alias(int user_id,
                                   const char *name,
                                   const char *entity_type,
                                   const char *canonical_name,
                                   memory_alias_resolve_t *out_resolution) {
   if (!out_resolution)
      return MEMORY_DB_FAILURE;
   memset(out_resolution, 0, sizeof(*out_resolution));
   if (!canonical_name || !*canonical_name)
      return MEMORY_DB_SUCCESS; /* clean miss */
   (void)name;                  /* display-form not needed by the cascade today */

   int64_t winner_id = 0;
   int matched_stage = 0;
   memory_alias_evidence_t ev;
   memset(&ev, 0, sizeof(ev));

   int rc = memory_alias_internal_cascade(user_id, canonical_name,
                                          entity_type ? entity_type : "thing",
                                          /* inbound_id */ 0, /* inbound_is_user_self */ false,
                                          /* use_synth_self */ false, &winner_id, &matched_stage,
                                          &ev, /* out_scored */ NULL, 0, NULL);
   if (rc != MEMORY_DB_SUCCESS)
      return rc;

   if (matched_stage == 1) {
      /* Stage 1 hit always wins — exact canonical_name match.  Stage 1
       * already returns post-JOIN canonical_id, so the resolver caller
       * can't distinguish alias from canonical anyway. */
      out_resolution->resolved_id = winner_id;
      out_resolution->matched_stage = 1;
      return MEMORY_DB_SUCCESS;
   }

   /* Stage 6: only commit to a resolution when the composite cleared the
    * auto-merge threshold.  Mid-confidence (review band) and below are
    * "no resolution" from the resolver's perspective — the caller should
    * still fall through to upsert; consider_auto_merge() handles the
    * proposal-queue case.  Runtime config overrides the compile-time
    * fallback default. */
   if (matched_stage == 6 &&
       ev.composite_score >= (float)g_config.memory.entity_merge_auto_threshold) {
      out_resolution->resolved_id = winner_id;
      out_resolution->matched_stage = 6;
      out_resolution->evidence = ev;
   }
   return MEMORY_DB_SUCCESS;
}

int memory_db_entity_resolve_alias_for_self(int user_id,
                                            const char *canonical_name,
                                            const char *entity_type,
                                            memory_alias_resolve_t *out_resolution) {
   if (!out_resolution)
      return MEMORY_DB_FAILURE;
   memset(out_resolution, 0, sizeof(*out_resolution));
   if (!canonical_name || !*canonical_name)
      return MEMORY_DB_SUCCESS; /* clean miss */

   int64_t winner_id = 0;
   int matched_stage = 0;
   memory_alias_evidence_t ev;
   memset(&ev, 0, sizeof(ev));

   /* Synthetic-self mode: directional overlap at Stage 2, "user" allow-list
    * pre-add, inbound treated as is_user_self=true so user_self_bonus
    * fires.  inbound_id=0 (no materialized self yet — that's what
    * "synthetic" means here). */
   int rc = memory_alias_internal_cascade(user_id, canonical_name,
                                          entity_type ? entity_type : "person",
                                          /* inbound_id */ 0, /* inbound_is_user_self */ true,
                                          /* use_synth_self */ true, &winner_id, &matched_stage,
                                          &ev, /* out_scored */ NULL, 0, NULL);
   if (rc != MEMORY_DB_SUCCESS)
      return rc;

   /* Unlike memory_db_entity_resolve_alias() (which only commits to a
    * resolution at the auto-merge threshold), the synthetic-self resolver
    * surfaces the cascade's best Stage 6 candidate at any composite band
    * — Phase 2 callers need to see review-band and below-threshold hits
    * to make their own band-routing decisions (similar to
    * consider_auto_merge's evaluate_t shape). */
   if (matched_stage == 1) {
      out_resolution->resolved_id = winner_id;
      out_resolution->matched_stage = 1;
      return MEMORY_DB_SUCCESS;
   }
   if (matched_stage == 6 && winner_id > 0) {
      out_resolution->resolved_id = winner_id;
      out_resolution->matched_stage = 6;
      out_resolution->evidence = ev;
   }
   return MEMORY_DB_SUCCESS;
}
