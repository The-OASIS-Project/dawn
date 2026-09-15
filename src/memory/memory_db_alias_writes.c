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
 * Memory Database — entity-merge alias write paths.
 *
 * Phase 6b split from memory_db_alias.c — alias_link / alias_unlink writers,
 * consider_auto_merge (band routing + longer-canonical direction-swap +
 * propose-all-in-band), merge proposal storage + resolve, equivalence-class-
 * aware relation listings, admin/WebUI listing helpers, alias summary,
 * auto-promote-user-self orchestrators, and the link-user-self orchestrator.
 *
 * The longer-canonical swap logic is intentionally adjacent to
 * consider_auto_merge per the design — both the AUTO branch and the
 * propose-band loop share the same direction-preference helpers.  The
 * auto-promote-user-self helpers stay here because they are write-path
 * orchestrators (UPDATE is_user_self=1).
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <ctype.h>
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

/* Insert a memory_entity_merge_proposals row.  Used by consider_auto_merge
 * for review-band candidates AND by the link-user-self orchestrator's
 * commit path.  No entity-row mutation. */
int memory_alias_internal_insert_merge_proposal(int user_id,
                                                int64_t source_id,
                                                int64_t target_id,
                                                float composite_score,
                                                const char *evidence_json,
                                                int64_t *out_proposal_id) {
   if (!out_proposal_id)
      return MEMORY_DB_FAILURE;
   *out_proposal_id = 0;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   const char *sql = "INSERT INTO memory_entity_merge_proposals "
                     "(user_id, source_entity_id, target_entity_id, composite_score, "
                     " evidence_json, proposed_at) "
                     "VALUES (?, ?, ?, ?, ?, strftime('%s','now'))";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, source_id);
   sqlite3_bind_int64(stmt, 3, target_id);
   sqlite3_bind_double(stmt, 4, (double)composite_score);
   if (evidence_json && *evidence_json) {
      sqlite3_bind_text(stmt, 5, evidence_json, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_text(stmt, 5, "{}", -1, SQLITE_STATIC);
   }

   int rc = sqlite3_step(stmt);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db_alias: insert proposal failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   *out_proposal_id = sqlite3_last_insert_rowid(s_db.db);
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return MEMORY_DB_SUCCESS;
}

/* Whole-word token count — splits on whitespace, ignores empty runs.  Used by
 * the longer-canonical-name preference below to compare inbound vs winner. */
static int entity_count_whole_word_tokens(const char *name) {
   if (!name)
      return 0;
   int count = 0;
   bool in_token = false;
   for (const char *p = name; *p; p++) {
      bool is_ws = isspace((unsigned char)*p);
      if (!is_ws && !in_token) {
         count++;
         in_token = true;
      } else if (is_ws) {
         in_token = false;
      }
   }
   return count;
}

/* Same-token-count tiebreaker: returns true iff every positional token of
 * `a` is a string prefix of (or equal to) the corresponding token of `b`.
 * Catches cases like "Jon Smith" (a) → "Jonathan Smith" (b) where token
 * counts match but b is clearly the fuller form per-token, without
 * misfiring on variants like "Bob" vs "Robert" (bob is NOT a prefix of
 * robert).  Known limitation: nickname pairs like "Liz"/"Elizabeth" or
 * "Bill"/"William" where the short form isn't a prefix of the long — no
 * string-based heuristic catches those without a nickname dictionary. */
static bool entity_tokens_are_prefix_of(const char *a, const char *b) {
   if (!a || !b)
      return false;
   const char *pa = a;
   const char *pb = b;
   for (;;) {
      while (*pa && isspace((unsigned char)*pa))
         pa++;
      while (*pb && isspace((unsigned char)*pb))
         pb++;
      if (!*pa && !*pb)
         return true; /* both fully consumed pairwise */
      if (!*pa || !*pb)
         return false; /* token count mismatch */
      /* Compare a's token characters one-by-one against b's at this slot. */
      while (*pa && !isspace((unsigned char)*pa)) {
         if (!*pb || isspace((unsigned char)*pb))
            return false; /* b's token shorter than a's at this position */
         if (*pa != *pb)
            return false; /* character mismatch — a is not a prefix of b */
         pa++;
         pb++;
      }
      /* a's token ended; consume the rest of b's token (b may be longer). */
      while (*pb && !isspace((unsigned char)*pb))
         pb++;
   }
}

/* Types where the "fuller" name form is the natural canonical ("Jonathan
 * Smith" is the canonical form of "Jon"; "Lassie the Collie" is the
 * canonical form of "Lassie").  `org` is excluded because acronyms (IBM,
 * NASA) are typically the canonical form in common usage; `thing` is
 * excluded because the heuristic doesn't generalize.
 *
 * MIRROR: the JS-side equivalent lives in www/js/ui/memory_aliases.js as
 * `SWAP_ELIGIBLE_TYPES` (manual-link confirm-modal direction preference).
 * If this list changes, update the JS set too.  Drift is caught by
 * tests/check_swap_eligible_types_mirror.sh, wired into ctest -L ci. */
static bool entity_type_prefers_longer_canonical(const char *entity_type) {
   if (!entity_type)
      return false;
   return strcmp(entity_type, "person") == 0 || strcmp(entity_type, "pet") == 0 ||
          strcmp(entity_type, "place") == 0;
}

/* Returns true if the entity has any aliases pointing at it OR any open
 * exclusive relations as subject.  Used to gate the longer-canonical swap:
 * we only redirect the alias-link when the existing canonical is a "leaf"
 * (no equivalence-class subtree, no exclusive subject-side state that would
 * be surprising to orphan-by-soft-merge).  Fail-safe: returns true on lock
 * acquisition failure or any SQL error — the caller will skip the swap. */
static bool entity_has_canonical_dependents(int user_id, int64_t entity_id) {
   if (entity_id <= 0)
      return false;

   AUTH_DB_LOCK_OR_RETURN(true);

   sqlite3_stmt *q_alias = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT 1 FROM memory_entities "
                          "WHERE user_id = ? AND canonical_id = ? LIMIT 1",
                          -1, &q_alias, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return true;
   }
   sqlite3_bind_int(q_alias, 1, user_id);
   sqlite3_bind_int64(q_alias, 2, entity_id);
   bool has_alias = (sqlite3_step(q_alias) == SQLITE_ROW);
   sqlite3_finalize(q_alias);
   if (has_alias) {
      AUTH_DB_UNLOCK();
      return true;
   }

   sqlite3_stmt *q_rel = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT relation FROM memory_relations "
                          "WHERE user_id = ? AND subject_entity_id = ? "
                          "AND valid_to IS NULL",
                          -1, &q_rel, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return true;
   }
   sqlite3_bind_int(q_rel, 1, user_id);
   sqlite3_bind_int64(q_rel, 2, entity_id);
   bool has_excl_open = false;
   while (sqlite3_step(q_rel) == SQLITE_ROW) {
      const char *rel = (const char *)sqlite3_column_text(q_rel, 0);
      if (rel && memory_db_relation_is_exclusive(rel)) {
         has_excl_open = true;
         break;
      }
   }
   sqlite3_finalize(q_rel);
   AUTH_DB_UNLOCK();
   return has_excl_open;
}

/* Longer-canonical-name preference for person / pet / place entities.  When
 * the cascade picks the existing canonical as merge target but the inbound
 * is the "fuller" form, redirect the alias-link so the long form stays
 * canonical and the existing short form becomes the alias.  Direction is
 * otherwise determined incidentally by extraction order — "Jon" arriving
 * first locks itself in as canonical, and a later "Jonathan Smith"
 * collapses *into* it.  See dawn/docs/TODO.md "Entity merge: prefer longer
 * canonical name for person/pet/place" for rationale.
 *
 * Guard against orphaning a subtree: only swap when the target has no
 * aliases pointing at it AND no open exclusive relations as subject.  Hard-
 * merge / Phase 3 consolidate is the right tool when the target is the head
 * of an equivalence class.
 *
 * Lock pattern: TWO SEQUENTIAL lock cycles (one inside
 * memory_alias_internal_load_entity_full, one inside
 * entity_has_canonical_dependents), NOT one held across both.  Same shape
 * as memory_db_entity_alias_link's pair of memory_alias_internal_load_entity_full
 * calls.  Don't "optimize" them into a single critical section — minimizing
 * the cascade lock window matters more than saving two acquire/release
 * pairs at a path that only fires on AUTO. */
static bool should_swap_for_longer_canonical(int user_id,
                                             const memory_entity_t *inbound,
                                             int64_t winner_id) {
   if (!inbound || winner_id <= 0)
      return false;
   if (!entity_type_prefers_longer_canonical(inbound->entity_type))
      return false;

   memory_entity_t winner;
   bool winner_is_self = false;
   int64_t winner_canon = 0;
   if (memory_alias_internal_load_entity_full(user_id, winner_id, &winner, &winner_canon,
                                              &winner_is_self) != MEMORY_DB_SUCCESS) {
      return false;
   }
   if (!entity_type_prefers_longer_canonical(winner.entity_type))
      return false;

   int inbound_tokens = entity_count_whole_word_tokens(inbound->canonical_name);
   int winner_tokens = entity_count_whole_word_tokens(winner.canonical_name);
   if (inbound_tokens < winner_tokens)
      return false;
   if (inbound_tokens == winner_tokens) {
      /* Same-token-count tiebreaker: only swap when winner's tokens are
       * positionally a prefix of inbound's tokens.  Catches "Jon Smith"
       * (existing) → "Jonathan Smith" (inbound) but skips variants like
       * "Bob Smith" vs "Robert Smith" where neither is a prefix. */
      if (!entity_tokens_are_prefix_of(winner.canonical_name, inbound->canonical_name))
         return false;
   }

   if (entity_has_canonical_dependents(user_id, winner_id))
      return false;

   return true;
}

int memory_db_entity_consider_auto_merge(int user_id,
                                         int64_t entity_id,
                                         memory_alias_evaluate_t *out_eval) {
   if (!out_eval || entity_id <= 0)
      return MEMORY_DB_FAILURE;
   memset(out_eval, 0, sizeof(*out_eval));
   out_eval->source_entity_id = entity_id;

   /* Load the inbound entity. */
   memory_entity_t inbound;
   bool inbound_is_self = false;
   int64_t inbound_canon = 0;
   int rc = memory_alias_internal_load_entity_full(user_id, entity_id, &inbound, &inbound_canon,
                                                   &inbound_is_self);
   if (rc != MEMORY_DB_SUCCESS)
      return rc;

   /* If inbound is already a soft alias, no auto-merge work to do. */
   if (inbound_canon != 0) {
      out_eval->outcome = MEMORY_ALIAS_OUTCOME_NO_CANDIDATES;
      return MEMORY_DB_SUCCESS;
   }

   int64_t winner_id = 0;
   int matched_stage = 0;
   memory_alias_evidence_t ev;
   memset(&ev, 0, sizeof(ev));

   /* Capture every scored candidate so we can propose ALL above review
    * (not just the winner) when auto-merge doesn't fire.  Sized to match
    * MEMORY_ALIAS_STAGE5_MAX_CANDIDATES so the cascade never has to drop
    * scored survivors before we see them. */
   scored_candidate_t scored[MEMORY_ALIAS_STAGE5_MAX_CANDIDATES];
   int scored_count = 0;

   rc = memory_alias_internal_cascade(user_id, inbound.canonical_name, inbound.entity_type,
                                      entity_id, inbound_is_self, /* use_synth_self */ false,
                                      &winner_id, &matched_stage, &ev, scored,
                                      MEMORY_ALIAS_STAGE5_MAX_CANDIDATES, &scored_count);
   if (rc != MEMORY_DB_SUCCESS)
      return rc;

   if (matched_stage == 0 || winner_id == 0) {
      out_eval->outcome = MEMORY_ALIAS_OUTCOME_NO_CANDIDATES;
      return MEMORY_DB_SUCCESS;
   }

   /* Stage 1 hit short-circuits to auto-merge — exact canonical_name match
    * means the row is a duplicate of an existing canonical.  Synthesize a
    * full-score evidence struct so the audit row has something to record. */
   if (matched_stage == 1) {
      memset(&ev, 0, sizeof(ev));
      ev.name_jaccard = 1.0f;
      ev.composite_score = 1.0f;
   }

   out_eval->target_entity_id = winner_id;
   out_eval->evidence = ev;

   /* Don't merge an entity into itself or into one of its own aliases. */
   if (winner_id == entity_id) {
      out_eval->outcome = MEMORY_ALIAS_OUTCOME_NO_CANDIDATES;
      out_eval->target_entity_id = 0;
      return MEMORY_DB_SUCCESS;
   }

   const float auto_thresh = (float)g_config.memory.entity_merge_auto_threshold;
   const float review_thresh = (float)g_config.memory.entity_merge_review_threshold;

   /* Auto-merge: winner crosses auto threshold.  Sole outcome — the
    * source becomes an alias of the winner, so other "would-be" proposals
    * are moot.  Soft-link writes happen here.
    *
    * Direction swap: for person/pet/place where the inbound is the fuller
    * form and the existing canonical is a "leaf" (no aliases, no open
    * exclusive relations as subject), flip the alias-link so the long form
    * stays canonical.  Updates out_eval->source/target to reflect the
    * actual link direction taken — caller can detect a swap by comparing
    * out_eval->target_entity_id against the entity_id passed in. */
   if (ev.composite_score >= auto_thresh) {
      int64_t link_src = entity_id;
      int64_t link_tgt = winner_id;
      if (should_swap_for_longer_canonical(user_id, &inbound, winner_id)) {
         link_src = winner_id;
         link_tgt = entity_id;
         OLOG_INFO("memory_db_alias: auto-merge direction swapped "
                   "(longer canonical preferred): src %lld→%lld, tgt %lld→%lld",
                   (long long)entity_id, (long long)link_src, (long long)winner_id,
                   (long long)link_tgt);
      }

      int64_t link_id = 0;
      int link_rc = memory_db_entity_alias_link(user_id, link_src, link_tgt, "soft", "auto-merge",
                                                ev.composite_score, NULL, &link_id);
      if (link_rc != MEMORY_DB_SUCCESS) {
         OLOG_WARNING("memory_db_alias: auto-merge soft-link failed for src=%lld tgt=%lld",
                      (long long)link_src, (long long)link_tgt);
         out_eval->outcome = MEMORY_ALIAS_OUTCOME_REJECTED;
         return MEMORY_DB_SUCCESS;
      }
      out_eval->source_entity_id = link_src;
      out_eval->target_entity_id = link_tgt;
      out_eval->outcome = MEMORY_ALIAS_OUTCOME_AUTO_MERGED;
      out_eval->link_id = link_id;
      return MEMORY_DB_SUCCESS;
   }

   /* No auto-merge: propose EVERY candidate above review_threshold, not
    * just the winner.  The previous "best-match-wins" semantics hid
    * secondary legitimate matches — e.g. "Jonathan Smith" matches both
    * "Jon" (same person) AND "Dawn Smith" (related entity); the
    * winner-only path picked one and silently discarded the other.  False-
    * positive cost is one click to reject in the Suggested-Merges UI, so
    * we trade precision for recall here.  Stage-1 short-circuit ALREADY
    * synthesized a fake evidence struct above (composite=1.0); it goes
    * through the AUTO branch and never reaches this code path. */
   int proposed_count = 0;
   int64_t first_proposal_id = 0;
   int64_t first_proposal_src = 0;
   int64_t first_proposal_tgt = 0;
   for (int i = 0; i < scored_count; i++) {
      if (scored[i].evidence.composite_score < review_thresh)
         break; /* sorted DESC, nothing more above threshold */
      if (scored[i].entity_id == entity_id)
         continue; /* self-link guard (cascade should already exclude) */
      if (scored[i].evidence.type_veto_fired)
         continue; /* type mismatch — don't propose */

      /* Direction preference mirrors the AUTO branch — if the inbound is
       * the fuller person/pet/place form and the candidate is a leaf
       * (no aliases pointing at it, no open exclusive relations as
       * subject), store the proposal pre-swapped so the operator sees
       * the right direction in the Suggested-Merges UI.  Without this
       * the operator would have to mentally invert "merge Jonathan
       * Smith → Jon" before clicking approve. */
      int64_t prop_src = entity_id;
      int64_t prop_tgt = scored[i].entity_id;
      if (should_swap_for_longer_canonical(user_id, &inbound, scored[i].entity_id)) {
         prop_src = scored[i].entity_id;
         prop_tgt = entity_id;
      }

      int64_t prop_id = 0;
      if (memory_alias_internal_insert_merge_proposal(user_id, prop_src, prop_tgt,
                                                      scored[i].evidence.composite_score, NULL,
                                                      &prop_id) == MEMORY_DB_SUCCESS) {
         proposed_count++;
         if (first_proposal_id == 0) {
            first_proposal_id = prop_id;
            first_proposal_src = prop_src;
            first_proposal_tgt = prop_tgt;
         }
         if (scored[i].entity_id != winner_id) {
            /* Log secondary proposals so operators can see the multi-
             * proposal behavior in extraction logs.  The winner gets
             * logged by the caller (memory_extraction.c) via out_eval.
             * Use the post-swap direction for honest log output. */
            OLOG_INFO("memory_db_alias: also proposed %lld → %lld (composite=%.2f)",
                      (long long)prop_src, (long long)prop_tgt,
                      (double)scored[i].evidence.composite_score);
         }
      }
   }

   if (proposed_count > 0) {
      out_eval->outcome = MEMORY_ALIAS_OUTCOME_PROPOSED;
      out_eval->proposal_id = first_proposal_id;
      /* Reflect the first (= winner-band) proposal's stored direction so
       * the caller's log line shows what the operator will see in the
       * Suggested-Merges panel.  Matches the AUTO branch's contract:
       * source/target track the resulting row, not the cascade input. */
      out_eval->source_entity_id = first_proposal_src;
      out_eval->target_entity_id = first_proposal_tgt;
      return MEMORY_DB_SUCCESS;
   }

   out_eval->outcome = MEMORY_ALIAS_OUTCOME_REJECTED;
   return MEMORY_DB_SUCCESS;
}

int memory_db_entity_alias_link(int user_id,
                                int64_t source_id,
                                int64_t target_id,
                                const char *link_kind,
                                const char *reason,
                                float composite_score,
                                const char *evidence_json,
                                int64_t *out_link_id) {
   if (out_link_id)
      *out_link_id = 0;
   if (source_id <= 0 || target_id <= 0 || source_id == target_id)
      return MEMORY_DB_FAILURE;
   if (!link_kind || !reason)
      return MEMORY_DB_FAILURE;
   if (strcmp(link_kind, "soft") != 0 && strcmp(link_kind, "hard") != 0)
      return MEMORY_DB_FAILURE;
   /* Phase 1 ships soft-link only; "hard" is reserved for the operator
    * consolidate path that lands in Phase 3.  Refuse it here so a future
    * caller can't bypass the unimplemented promotion logic by passing
    * "hard" through this entry point. */
   if (strcmp(link_kind, "hard") == 0) {
      OLOG_WARNING("memory_db_alias: link_kind='hard' is reserved for Phase 3 consolidate; "
                   "refusing");
      return MEMORY_DB_FAILURE;
   }

   /* Load both entities (verifies ownership AND captures canonical_name
    * snapshots for the audit row).  Each load takes the lock on its own. */
   memory_entity_t src, tgt;
   int64_t src_canon = 0, tgt_canon = 0;
   int rc = memory_alias_internal_load_entity_full(user_id, source_id, &src, &src_canon, NULL);
   if (rc != MEMORY_DB_SUCCESS)
      return rc;
   rc = memory_alias_internal_load_entity_full(user_id, target_id, &tgt, &tgt_canon, NULL);
   if (rc != MEMORY_DB_SUCCESS)
      return rc;

   /* Refuse to link if the source already has aliases pointing at it (it's
    * canonical for an equivalence class) — would orphan its dependents. */
   {
      AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
      sqlite3_stmt *chk = NULL;
      if (sqlite3_prepare_v2(s_db.db,
                             "SELECT 1 FROM memory_entities WHERE user_id = ? AND "
                             "canonical_id = ? LIMIT 1",
                             -1, &chk, NULL) != SQLITE_OK) {
         AUTH_DB_UNLOCK();
         return MEMORY_DB_FAILURE;
      }
      sqlite3_bind_int(chk, 1, user_id);
      sqlite3_bind_int64(chk, 2, source_id);
      bool has_dependents = (sqlite3_step(chk) == SQLITE_ROW);
      sqlite3_finalize(chk);
      AUTH_DB_UNLOCK();
      if (has_dependents) {
         OLOG_WARNING("memory_db_alias: refusing to link entity %lld — has dependent aliases",
                      (long long)source_id);
         return MEMORY_DB_FAILURE;
      }
   }

   /* Atomic write: UPDATE source.canonical_id + INSERT audit row, all
    * inside BEGIN IMMEDIATE.  Rollback on any failure. */
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   char *errmsg = NULL;
   if (sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("memory_db_alias: BEGIN IMMEDIATE failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   int64_t link_id = 0;
   int result_rc = MEMORY_DB_SUCCESS;

   /* UPDATE memory_entities SET canonical_id = ?. */
   sqlite3_stmt *upd = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE memory_entities SET canonical_id = ? "
                          "WHERE id = ? AND user_id = ?",
                          -1, &upd, NULL) != SQLITE_OK) {
      result_rc = MEMORY_DB_FAILURE;
      goto link_fail;
   }
   sqlite3_bind_int64(upd, 1, target_id);
   sqlite3_bind_int64(upd, 2, source_id);
   sqlite3_bind_int(upd, 3, user_id);
   if (sqlite3_step(upd) != SQLITE_DONE) {
      OLOG_ERROR("memory_db_alias: UPDATE canonical_id failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_finalize(upd);
      result_rc = MEMORY_DB_FAILURE;
      goto link_fail;
   }
   sqlite3_finalize(upd);

   /* INSERT INTO memory_entity_aliases. */
   sqlite3_stmt *ins = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "INSERT INTO memory_entity_aliases "
                          "(user_id, source_entity_id, target_entity_id, "
                          " source_canonical_name, target_canonical_name, "
                          " link_kind, reason, composite_score, evidence_json, "
                          " linked_at) "
                          "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, strftime('%s','now'))",
                          -1, &ins, NULL) != SQLITE_OK) {
      result_rc = MEMORY_DB_FAILURE;
      goto link_fail;
   }
   sqlite3_bind_int(ins, 1, user_id);
   sqlite3_bind_int64(ins, 2, source_id);
   sqlite3_bind_int64(ins, 3, target_id);
   sqlite3_bind_text(ins, 4, src.canonical_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(ins, 5, tgt.canonical_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(ins, 6, link_kind, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(ins, 7, reason, -1, SQLITE_TRANSIENT);
   if (composite_score >= 0.0f) {
      sqlite3_bind_double(ins, 8, (double)composite_score);
   } else {
      sqlite3_bind_null(ins, 8);
   }
   if (evidence_json && *evidence_json) {
      sqlite3_bind_text(ins, 9, evidence_json, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(ins, 9);
   }
   if (sqlite3_step(ins) != SQLITE_DONE) {
      OLOG_ERROR("memory_db_alias: INSERT alias failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_finalize(ins);
      result_rc = MEMORY_DB_FAILURE;
      goto link_fail;
   }
   link_id = sqlite3_last_insert_rowid(s_db.db);
   sqlite3_finalize(ins);

   if (sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("memory_db_alias: COMMIT failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      result_rc = MEMORY_DB_FAILURE;
      goto link_fail;
   }

   AUTH_DB_UNLOCK();

   /* Post-commit: invalidate the entity-embedding cache so subsequent
    * reads see the new alias state.  Safe outside the lock — the
    * invalidator is just an atomic dirty-bit flip (memory_embeddings.c). */
   memory_embeddings_invalidate_entity_cache();

   if (out_link_id)
      *out_link_id = link_id;

   OLOG_INFO("memory_db_alias: linked %lld -> %lld (%s, %s, composite=%.2f, link_id=%lld)",
             (long long)source_id, (long long)target_id, link_kind, reason,
             (composite_score < 0.0f) ? -1.0 : (double)composite_score, (long long)link_id);
   return MEMORY_DB_SUCCESS;

link_fail:
   sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
   AUTH_DB_UNLOCK();
   return result_rc;
}

int memory_db_entity_alias_unlink(int user_id, int64_t link_id, const char *unlink_reason) {
   if (link_id <= 0)
      return MEMORY_DB_FAILURE;
   const char *reason = (unlink_reason && *unlink_reason) ? unlink_reason : "split-by-operator";

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* Load + validate the link row.  Refuse if hard-merged or already unlinked. */
   sqlite3_stmt *load = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT source_entity_id, link_kind, unlinked_at "
                          "FROM memory_entity_aliases WHERE id = ? AND user_id = ?",
                          -1, &load, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int64(load, 1, link_id);
   sqlite3_bind_int(load, 2, user_id);

   int rc = sqlite3_step(load);
   if (rc == SQLITE_DONE) {
      sqlite3_finalize(load);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_NOT_FOUND;
   }
   if (rc != SQLITE_ROW) {
      sqlite3_finalize(load);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   int64_t source_id = sqlite3_column_int64(load, 0);
   const unsigned char *kind = sqlite3_column_text(load, 1);
   bool is_hard = kind && strcmp((const char *)kind, "hard") == 0;
   bool already_unlinked = sqlite3_column_type(load, 2) != SQLITE_NULL;
   sqlite3_finalize(load);

   if (is_hard) {
      AUTH_DB_UNLOCK();
      OLOG_WARNING("memory_db_alias: refusing to split hard-merged link %lld", (long long)link_id);
      return MEMORY_DB_FAILURE;
   }
   if (already_unlinked) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_NOT_FOUND;
   }

   /* Atomic split: UPDATE memory_entities.canonical_id = NULL + UPDATE
    * memory_entity_aliases SET unlinked_at, unlink_reason. */
   char *errmsg = NULL;
   if (sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("memory_db_alias: BEGIN IMMEDIATE failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   int result_rc = MEMORY_DB_SUCCESS;

   sqlite3_stmt *clr = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE memory_entities SET canonical_id = NULL "
                          "WHERE id = ? AND user_id = ?",
                          -1, &clr, NULL) != SQLITE_OK) {
      result_rc = MEMORY_DB_FAILURE;
      goto split_fail;
   }
   sqlite3_bind_int64(clr, 1, source_id);
   sqlite3_bind_int(clr, 2, user_id);
   if (sqlite3_step(clr) != SQLITE_DONE) {
      sqlite3_finalize(clr);
      result_rc = MEMORY_DB_FAILURE;
      goto split_fail;
   }
   sqlite3_finalize(clr);

   sqlite3_stmt *audit = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE memory_entity_aliases SET unlinked_at = strftime('%s','now'), "
                          "       unlink_reason = ? WHERE id = ?",
                          -1, &audit, NULL) != SQLITE_OK) {
      result_rc = MEMORY_DB_FAILURE;
      goto split_fail;
   }
   sqlite3_bind_text(audit, 1, reason, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(audit, 2, link_id);
   if (sqlite3_step(audit) != SQLITE_DONE) {
      sqlite3_finalize(audit);
      result_rc = MEMORY_DB_FAILURE;
      goto split_fail;
   }
   sqlite3_finalize(audit);

   if (sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("memory_db_alias: COMMIT failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      result_rc = MEMORY_DB_FAILURE;
      goto split_fail;
   }

   AUTH_DB_UNLOCK();

   /* Post-commit cache invalidation; same contract as alias_link. */
   memory_embeddings_invalidate_entity_cache();

   OLOG_INFO("memory_db_alias: split link %lld (source=%lld, reason=%s)", (long long)link_id,
             (long long)source_id, reason);
   return MEMORY_DB_SUCCESS;

split_fail:
   sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
   AUTH_DB_UNLOCK();
   return result_rc;
}

/* =============================================================================
 * Equivalence-class-aware relation listings (design §5)
 *
 * The IN-subquery form keeps the API shape compatible with
 * memory_db_relation_list_by_subject.  EXPLAIN QUERY PLAN was used to
 * confirm the planner uses idx_memory_entities_canonical for the inner
 * SELECT (see ckpt summary).  The fallback UNION-ALL form was deemed
 * unnecessary after the verification step.
 * ============================================================================= */

/* Bounded helper: populate a memory_relation_t row from the standard
 * outgoing-relation SELECT shape used below.  Keeps the three list
 * variants free of struct-copy boilerplate.
 *
 * SELECT column layout:
 *   0:id, 1:subject_entity_id, 2:relation, 3:object_entity_id,
 *   4:object_name, 5:confidence, 6:valid_from, 7:valid_to, 8:mention_count
 *
 * mention_count appended in v49.  Must stay column-symmetric with
 * populate_relation_from_row() in memory_db_relations.c — both helpers read
 * the same column layout. */
static void populate_relation_outgoing_row(sqlite3_stmt *stmt, memory_relation_t *out) {
   memset(out, 0, sizeof(*out));
   out->id = sqlite3_column_int64(stmt, 0);
   out->subject_entity_id = sqlite3_column_int64(stmt, 1);
   const char *rel = (const char *)sqlite3_column_text(stmt, 2);
   if (rel) {
      safe_strscpy(out->relation, rel);
   }
   out->object_entity_id = (sqlite3_column_type(stmt, 3) == SQLITE_NULL)
                               ? 0
                               : sqlite3_column_int64(stmt, 3);
   const char *obj_name = (const char *)sqlite3_column_text(stmt, 4);
   if (obj_name) {
      safe_strscpy(out->object_name, obj_name);
   }
   out->confidence = (float)sqlite3_column_double(stmt, 5);
   out->valid_from = (sqlite3_column_type(stmt, 6) == SQLITE_NULL) ? 0
                                                                   : sqlite3_column_int64(stmt, 6);
   out->valid_to = (sqlite3_column_type(stmt, 7) == SQLITE_NULL) ? 0
                                                                 : sqlite3_column_int64(stmt, 7);
   out->mention_count = sqlite3_column_int(stmt, 8);
}

int memory_db_relation_list_by_subject_class(int user_id,
                                             int64_t canonical_id,
                                             memory_relation_t *out,
                                             int max,
                                             int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || !count_out || max <= 0 || canonical_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* The IN-subquery shape passes EXPLAIN QUERY PLAN with both
    * idx_memory_entities_user (for id = ?) and idx_memory_entities_canonical
    * (for canonical_id = ?) on the inner SELECT.  See ckpt summary. */
   const char *sql = "SELECT r.id, r.subject_entity_id, r.relation, r.object_entity_id, "
                     "       COALESCE(e.name, r.object_value, ''), r.confidence, "
                     "       r.valid_from, r.valid_to, r.mention_count "
                     "FROM memory_relations r "
                     "LEFT JOIN memory_entities e ON e.id = r.object_entity_id "
                     "WHERE r.user_id = ? AND r.subject_entity_id IN ( "
                     "   SELECT id FROM memory_entities WHERE user_id = ? AND id = ? "
                     "   UNION ALL "
                     "   SELECT id FROM memory_entities WHERE user_id = ? AND canonical_id = ? "
                     ") "
                     "ORDER BY r.id ASC LIMIT ?";

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, user_id);
   sqlite3_bind_int64(stmt, 3, canonical_id);
   sqlite3_bind_int(stmt, 4, user_id);
   sqlite3_bind_int64(stmt, 5, canonical_id);
   sqlite3_bind_int(stmt, 6, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_relation_outgoing_row(stmt, &out[n]);
      n++;
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   *count_out = n;
   return MEMORY_DB_SUCCESS;
}

int memory_db_relation_list_by_subject_class_at(int user_id,
                                                int64_t canonical_id,
                                                int64_t as_of_ts,
                                                memory_relation_t *out,
                                                int max,
                                                int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || !count_out || max <= 0 || canonical_id <= 0)
      return MEMORY_DB_FAILURE;

   if (as_of_ts == 0)
      as_of_ts = (int64_t)time(NULL);

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   const char *sql = "SELECT r.id, r.subject_entity_id, r.relation, r.object_entity_id, "
                     "       COALESCE(e.name, r.object_value, ''), r.confidence, "
                     "       r.valid_from, r.valid_to, r.mention_count "
                     "FROM memory_relations r "
                     "LEFT JOIN memory_entities e ON e.id = r.object_entity_id "
                     "WHERE r.user_id = ? AND r.subject_entity_id IN ( "
                     "   SELECT id FROM memory_entities WHERE user_id = ? AND id = ? "
                     "   UNION ALL "
                     "   SELECT id FROM memory_entities WHERE user_id = ? AND canonical_id = ? "
                     ") "
                     "  AND (r.valid_from IS NULL OR r.valid_from <= ?) "
                     "  AND (r.valid_to IS NULL OR r.valid_to > ?) "
                     "ORDER BY r.id ASC LIMIT ?";

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, user_id);
   sqlite3_bind_int64(stmt, 3, canonical_id);
   sqlite3_bind_int(stmt, 4, user_id);
   sqlite3_bind_int64(stmt, 5, canonical_id);
   sqlite3_bind_int64(stmt, 6, as_of_ts);
   sqlite3_bind_int64(stmt, 7, as_of_ts);
   sqlite3_bind_int(stmt, 8, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_relation_outgoing_row(stmt, &out[n]);
      n++;
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   *count_out = n;
   return MEMORY_DB_SUCCESS;
}

int memory_db_relation_list_by_object_class(int user_id,
                                            int64_t canonical_id,
                                            memory_relation_t *out,
                                            int max,
                                            int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || !count_out || max <= 0 || canonical_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* Incoming-relation listing — the object_name field carries the SUBJECT's
    * resolved name (matches existing memory_db_relation_list_by_object
    * convention). */
   const char *sql = "SELECT r.id, r.subject_entity_id, r.relation, r.object_entity_id, "
                     "       COALESCE(s.name, ''), r.confidence, r.valid_from, r.valid_to, "
                     "       r.mention_count "
                     "FROM memory_relations r "
                     "LEFT JOIN memory_entities s ON s.id = r.subject_entity_id "
                     "WHERE r.user_id = ? AND r.object_entity_id IN ( "
                     "   SELECT id FROM memory_entities WHERE user_id = ? AND id = ? "
                     "   UNION ALL "
                     "   SELECT id FROM memory_entities WHERE user_id = ? AND canonical_id = ? "
                     ") "
                     "ORDER BY r.id ASC LIMIT ?";

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, user_id);
   sqlite3_bind_int64(stmt, 3, canonical_id);
   sqlite3_bind_int(stmt, 4, user_id);
   sqlite3_bind_int64(stmt, 5, canonical_id);
   sqlite3_bind_int(stmt, 6, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_relation_outgoing_row(stmt, &out[n]);
      n++;
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   *count_out = n;
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * Listing helpers used by the dawn-admin entity subcommands and the WebUI
 * REST/WebSocket handlers (Ckpt 4).  These three are read-only views into
 * the alias surface — no DB writes — so they live in the alias module
 * rather than reach into s_db from outside the memory layer.
 * ============================================================================= */

int memory_db_entity_alias_list(int user_id,
                                int64_t target_entity_id,
                                memory_alias_listing_row_t *out,
                                int max,
                                int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || max <= 0 || target_entity_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   const char *sql = "SELECT id, COALESCE(source_entity_id, 0), source_canonical_name, "
                     "       link_kind, COALESCE(composite_score, -1.0), linked_at "
                     "FROM memory_entity_aliases "
                     "WHERE user_id = ? AND target_entity_id = ? AND unlinked_at IS NULL "
                     "ORDER BY linked_at DESC LIMIT ?";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, target_entity_id);
   sqlite3_bind_int(stmt, 3, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
      memory_alias_listing_row_t *row = &out[n];
      memset(row, 0, sizeof(*row));
      row->link_id = sqlite3_column_int64(stmt, 0);
      row->source_entity_id = sqlite3_column_int64(stmt, 1);
      const char *src = (const char *)sqlite3_column_text(stmt, 2);
      if (src) {
         safe_strscpy(row->source_canonical_name, src);
      }
      const char *kind = (const char *)sqlite3_column_text(stmt, 3);
      if (kind) {
         safe_strscpy(row->link_kind, kind);
      }
      row->composite_score = (float)sqlite3_column_double(stmt, 4);
      row->linked_at = sqlite3_column_int64(stmt, 5);
      n++;
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = n;
   return MEMORY_DB_SUCCESS;
}

int memory_db_entity_alias_history(int user_id,
                                   int64_t entity_id,
                                   memory_alias_history_row_t *out,
                                   int max,
                                   int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || max <= 0 || entity_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   const char *sql = "SELECT id, source_canonical_name, target_canonical_name, "
                     "       link_kind, reason, linked_at, "
                     "       COALESCE(unlinked_at, 0), COALESCE(unlink_reason, '') "
                     "FROM memory_entity_aliases "
                     "WHERE user_id = ? AND (source_entity_id = ? OR target_entity_id = ?) "
                     "ORDER BY linked_at ASC LIMIT ?";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, entity_id);
   sqlite3_bind_int64(stmt, 3, entity_id);
   sqlite3_bind_int(stmt, 4, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
      memory_alias_history_row_t *row = &out[n];
      memset(row, 0, sizeof(*row));
      row->link_id = sqlite3_column_int64(stmt, 0);
      const char *src = (const char *)sqlite3_column_text(stmt, 1);
      if (src) {
         safe_strscpy(row->source_canonical_name, src);
      }
      const char *tgt = (const char *)sqlite3_column_text(stmt, 2);
      if (tgt) {
         safe_strscpy(row->target_canonical_name, tgt);
      }
      const char *kind = (const char *)sqlite3_column_text(stmt, 3);
      if (kind) {
         safe_strscpy(row->link_kind, kind);
      }
      const char *reason = (const char *)sqlite3_column_text(stmt, 4);
      if (reason) {
         safe_strscpy(row->reason, reason);
      }
      row->linked_at = sqlite3_column_int64(stmt, 5);
      row->unlinked_at = sqlite3_column_int64(stmt, 6);
      const char *unlink_r = (const char *)sqlite3_column_text(stmt, 7);
      if (unlink_r) {
         safe_strscpy(row->unlink_reason, unlink_r);
      }
      n++;
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = n;
   return MEMORY_DB_SUCCESS;
}

int memory_db_entity_list_for_admin(int user_id,
                                    bool include_aliases,
                                    memory_alias_entity_row_t *out,
                                    int max,
                                    int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || max <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* Pass 1 — canonicals (canonical_id IS NULL), equivalence-class
    * mention_count DESC.  Before this aggregation a soft-merged operator
    * who linked "Jon Smith (50)" → "Jonathan Smith (3)" saw the
    * canonical display "3 mentions" while the class total was 53; the
    * canonical-only display masked the actual merge state.  Subquery
    * sums mention_count across {self + aliases pointing at self}, and
    * ORDER BY uses the aggregated value so high-traffic equivalence
    * classes still surface to the top of the admin list.
    *
    * Sort-key asymmetry vs entity_search (own mc): admin list is the
    * operator's source of truth; entity_search needs an indexable sort
    * key for hot-path queries — see auth_db_core.c entity_search prep. */
   const char *sql_canonical =
       "SELECT e.id, e.name, e.entity_type, "
       "  (SELECT COALESCE(SUM(mention_count), 0) FROM memory_entities "
       "     WHERE user_id = e.user_id AND (id = e.id OR canonical_id = e.id)) "
       "    AS class_mention_count, "
       "  e.is_user_self "
       "FROM memory_entities e "
       "WHERE e.user_id = ? AND e.canonical_id IS NULL "
       "ORDER BY class_mention_count DESC, e.id ASC LIMIT ?";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql_canonical, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
      memory_alias_entity_row_t *row = &out[n];
      memset(row, 0, sizeof(*row));
      row->entity_id = sqlite3_column_int64(stmt, 0);
      const char *name = (const char *)sqlite3_column_text(stmt, 1);
      if (name) {
         safe_strscpy(row->name, name);
      }
      const char *etype = (const char *)sqlite3_column_text(stmt, 2);
      if (etype) {
         safe_strscpy(row->entity_type, etype);
      }
      row->mention_count = sqlite3_column_int(stmt, 3);
      row->is_user_self = sqlite3_column_int(stmt, 4) != 0;
      row->is_alias = false;
      row->canonical_id = 0;
      n++;
   }
   sqlite3_finalize(stmt);

   /* Pass 2 — aliases, ordered by canonical_id so callers can iterate
    * canonicals and find their aliases by linear scan over a contiguous
    * tail.  Separate budget from canonicals: with the historic single-query
    * shape, 254 canonicals saturated max=200 and silently dropped every
    * alias.  Quota now lives in `max - n`, so the operator's link state
    * surfaces even on dense user graphs. */
   if (include_aliases && n < max) {
      const char *sql_aliases =
          "SELECT id, name, entity_type, mention_count, is_user_self, canonical_id "
          "FROM memory_entities "
          "WHERE user_id = ? AND canonical_id IS NOT NULL "
          "ORDER BY canonical_id ASC, mention_count DESC, id ASC LIMIT ?";
      if (sqlite3_prepare_v2(s_db.db, sql_aliases, -1, &stmt, NULL) != SQLITE_OK) {
         AUTH_DB_UNLOCK();
         if (count_out)
            *count_out = n;
         return MEMORY_DB_FAILURE;
      }
      sqlite3_bind_int(stmt, 1, user_id);
      sqlite3_bind_int(stmt, 2, max - n);

      while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
         memory_alias_entity_row_t *row = &out[n];
         memset(row, 0, sizeof(*row));
         row->entity_id = sqlite3_column_int64(stmt, 0);
         const char *name = (const char *)sqlite3_column_text(stmt, 1);
         if (name) {
            safe_strscpy(row->name, name);
         }
         const char *etype = (const char *)sqlite3_column_text(stmt, 2);
         if (etype) {
            safe_strscpy(row->entity_type, etype);
         }
         row->mention_count = sqlite3_column_int(stmt, 3);
         row->is_user_self = sqlite3_column_int(stmt, 4) != 0;
         row->canonical_id = sqlite3_column_int64(stmt, 5);
         row->is_alias = true;
         n++;
      }
      sqlite3_finalize(stmt);
   }

   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = n;
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * Bulk alias summary — single-shot scan of every alias row for a user.
 *
 * Used by the WebUI Graph-tab list handler to:
 *   1. Drop alias rows from the canonical-only list (the (alias_entity_id)
 *      set tells the caller which `memory_db_entity_list` rows to skip);
 *   2. Tally per-canonical alias counts so canonical cards render the
 *      "X aliases" badge without an N+1 round-trip.
 *
 * Driven by the partial index `idx_memory_entities_canonical`, which already
 * filters to `canonical_id IS NOT NULL`.  At the dev's 2k-entity scale this
 * is a sub-millisecond scan over a small partial index; even with thousands
 * of aliases the cost stays negligible.
 * ============================================================================= */

int memory_db_entity_alias_summary(int user_id,
                                   memory_alias_summary_row_t *out,
                                   int max,
                                   int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || max <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   /* Partial index `idx_memory_entities_canonical` filters to
    * canonical_id IS NOT NULL — restate the predicate so the planner picks it. */
   const char *sql = "SELECT id, canonical_id "
                     "FROM memory_entities "
                     "WHERE user_id = ? AND canonical_id IS NOT NULL "
                     "ORDER BY id ASC "
                     "LIMIT ?";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
      out[n].alias_entity_id = sqlite3_column_int64(stmt, 0);
      out[n].canonical_entity_id = sqlite3_column_int64(stmt, 1);
      n++;
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = n;
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * Merge-proposal listing + resolve (Phase 1 fold-in to Ckpt 4).
 *
 * link-user-self queues 0.70-0.90 candidates as proposals; the WebUI surfaces
 * them via these two helpers.  Phase 2's auto-merge gate at extraction time
 * will be the dominant producer once it ships.
 * ============================================================================= */

int memory_db_proposal_count_pending(int user_id, int *count_out) {
   if (count_out)
      *count_out = 0;
   if (user_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT COUNT(*) FROM memory_entity_merge_proposals "
                          "WHERE user_id = ? AND resolved_at IS NULL",
                          -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   int n = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      n = sqlite3_column_int(stmt, 0);
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   if (count_out)
      *count_out = n;
   return MEMORY_DB_SUCCESS;
}

/* Forward declaration — promote_to_user_self_entity is defined further
 * down (near memory_alias_link_user_self_run).  Both auto-promote helpers
 * are public-API callers of that static. */
static int promote_to_user_self_entity(int user_id, int64_t entity_id);

int memory_db_entity_get_user_self_id(int user_id, int64_t *out_id) {
   int64_t self_id = 0;
   int rc = memory_alias_internal_find_user_self_id(user_id, &self_id);
   if (out_id)
      *out_id = (rc == MEMORY_DB_SUCCESS) ? self_id : 0;
   /* find_user_self_id returns MEMORY_DB_NOT_FOUND when no row exists;
    * the public API normalizes that to SUCCESS with out_id=0 since "no
    * anchor yet" is a valid no-op state, not an error. */
   if (rc == MEMORY_DB_NOT_FOUND)
      return MEMORY_DB_SUCCESS;
   return rc;
}

int memory_db_entity_maybe_auto_promote_user_self(int user_id,
                                                  int64_t entity_id,
                                                  const char *canonical_name,
                                                  bool *out_promoted) {
   if (out_promoted)
      *out_promoted = false;
   if (user_id <= 0 || entity_id <= 0 || !canonical_name || !*canonical_name)
      return MEMORY_DB_FAILURE;

   /* Skip if a user_self already exists.  The partial UNIQUE index would
    * reject the UPDATE anyway, but checking first avoids the noisy
    * "promote matched no rows" warning from promote_to_user_self_entity
    * when the user already has their self anchor set. */
   int64_t existing_self = 0;
   if (memory_alias_internal_find_user_self_id(user_id, &existing_self) == MEMORY_DB_SUCCESS &&
       existing_self > 0)
      return MEMORY_DB_SUCCESS;

   /* Look up users.real_name (and identity_aliases). */
   auth_user_identity_t identity;
   memset(&identity, 0, sizeof(identity));
   if (auth_db_get_user_identity(user_id, &identity) != AUTH_DB_SUCCESS)
      return MEMORY_DB_SUCCESS; /* no identity row — caller hasn't set real_name yet */
   if (identity.real_name[0] == '\0')
      return MEMORY_DB_SUCCESS; /* real_name not configured — operator must set it first */

   /* Canonicalize real_name and each newline-separated alias, compare
    * against the inbound canonical.  Match on equality (after canon
    * normalization) — substring would be ambiguous for short names. */
   char rn_canon[MEMORY_ENTITY_NAME_MAX];
   memory_make_canonical_name(identity.real_name, rn_canon, sizeof(rn_canon));

   bool matches = (rn_canon[0] != '\0' && strcmp(rn_canon, canonical_name) == 0);
   if (!matches && identity.identity_aliases[0] != '\0') {
      /* Walk newline-separated aliases. */
      const char *p = identity.identity_aliases;
      while (!matches && *p) {
         while (*p == '\n' || *p == '\r')
            p++;
         if (!*p)
            break;
         const char *line_start = p;
         while (*p && *p != '\n' && *p != '\r')
            p++;
         size_t len = (size_t)(p - line_start);
         if (len == 0 || len >= MEMORY_ENTITY_NAME_MAX)
            continue;
         char line[MEMORY_ENTITY_NAME_MAX];
         memcpy(line, line_start, len);
         line[len] = '\0';
         char alias_canon[MEMORY_ENTITY_NAME_MAX];
         memory_make_canonical_name(line, alias_canon, sizeof(alias_canon));
         if (alias_canon[0] != '\0' && strcmp(alias_canon, canonical_name) == 0)
            matches = true;
      }
   }
   if (!matches)
      return MEMORY_DB_SUCCESS;

   /* Promote.  Reuses the existing helper that enforces canonical_id IS
    * NULL and the no-existing-user_self UNIQUE constraint.  NOT_FOUND
    * here means the row no longer qualifies (deleted between check and
    * UPDATE, already promoted, or has canonical_id set — all benign no-
    * ops, not failures); SUCCESS_with_out_promoted=false matches the
    * docstring contract. */
   int rc = promote_to_user_self_entity(user_id, entity_id);
   if (rc == MEMORY_DB_NOT_FOUND)
      return MEMORY_DB_SUCCESS;
   if (rc != MEMORY_DB_SUCCESS)
      return rc;

   if (out_promoted)
      *out_promoted = true;
   OLOG_INFO("memory_db_alias: auto-promoted entity %lld to is_user_self=1 (canonical='%s' "
             "matched users.real_name)",
             (long long)entity_id, canonical_name);
   return MEMORY_DB_SUCCESS;
}

/* Lookup helper: find a canonical entity for @p user_id whose
 * canonical_name equals @p canonical (exact match, canonical_id IS NULL).
 * Returns the entity id via @p out_id (0 if no match).  Used by the
 * by-real-name promotion sweep. */
static int find_canonical_by_name(int user_id, const char *canonical, int64_t *out_id) {
   if (!out_id)
      return MEMORY_DB_FAILURE;
   *out_id = 0;
   if (user_id <= 0 || !canonical || !*canonical)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT id FROM memory_entities "
                          "WHERE user_id = ? AND canonical_id IS NULL "
                          "  AND canonical_name = ? LIMIT 1",
                          -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, canonical, -1, SQLITE_TRANSIENT);
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      *out_id = sqlite3_column_int64(stmt, 0);
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return MEMORY_DB_SUCCESS;
}

int memory_db_entity_auto_promote_user_self_by_real_name(int user_id, bool *out_promoted) {
   if (out_promoted)
      *out_promoted = false;
   if (user_id <= 0)
      return MEMORY_DB_FAILURE;

   int64_t existing_self = 0;
   if (memory_alias_internal_find_user_self_id(user_id, &existing_self) == MEMORY_DB_SUCCESS &&
       existing_self > 0)
      return MEMORY_DB_SUCCESS; /* already set */

   auth_user_identity_t identity;
   memset(&identity, 0, sizeof(identity));
   if (auth_db_get_user_identity(user_id, &identity) != AUTH_DB_SUCCESS)
      return MEMORY_DB_SUCCESS;
   if (identity.real_name[0] == '\0')
      return MEMORY_DB_SUCCESS;

   /* Try real_name first.  Canonicalize before lookup so the match
    * survives casing / whitespace differences between Settings input and
    * extraction-stored canonical_name. */
   char canon[MEMORY_ENTITY_NAME_MAX];
   memory_make_canonical_name(identity.real_name, canon, sizeof(canon));
   int64_t found_id = 0;
   if (canon[0] != '\0') {
      find_canonical_by_name(user_id, canon, &found_id);
   }

   /* Fall through to aliases on miss.  Same newline-separated parsing the
    * maybe-promote helper uses. */
   if (found_id == 0 && identity.identity_aliases[0] != '\0') {
      const char *p = identity.identity_aliases;
      while (*p && found_id == 0) {
         while (*p == '\n' || *p == '\r')
            p++;
         if (!*p)
            break;
         const char *line_start = p;
         while (*p && *p != '\n' && *p != '\r')
            p++;
         size_t len = (size_t)(p - line_start);
         if (len == 0 || len >= MEMORY_ENTITY_NAME_MAX)
            continue;
         char line[MEMORY_ENTITY_NAME_MAX];
         memcpy(line, line_start, len);
         line[len] = '\0';
         memory_make_canonical_name(line, canon, sizeof(canon));
         if (canon[0] != '\0') {
            find_canonical_by_name(user_id, canon, &found_id);
         }
      }
   }

   if (found_id == 0)
      return MEMORY_DB_SUCCESS; /* no matching canonical yet — extraction will catch it later */

   /* NOT_FOUND from promote means the row stopped qualifying between the
    * find_canonical_by_name lookup and the UPDATE — benign race, treat
    * as no-op to honour the docstring's "SUCCESS on no-op" contract. */
   int rc = promote_to_user_self_entity(user_id, found_id);
   if (rc == MEMORY_DB_NOT_FOUND)
      return MEMORY_DB_SUCCESS;
   if (rc != MEMORY_DB_SUCCESS)
      return rc;

   if (out_promoted)
      *out_promoted = true;
   OLOG_INFO("memory_db_alias: auto-promoted entity %lld to is_user_self=1 by real_name lookup "
             "(user_id=%d)",
             (long long)found_id, user_id);
   return MEMORY_DB_SUCCESS;
}

/* The literal canonical form the LLM-emitted abstract "User" entity collapses
 * to after memory_make_canonical_name (lowercased, whitespace-trimmed).
 * Single source of truth so the maybe / sweep helpers stay in sync if the
 * canonicalizer ever changes its rules.
 *
 * Intentionally .c-only (not exported in memory_db_aliases.h).  External
 * consumers — including tests in test_memory_db_alias.c — exercise the
 * literal through the public helpers; hardcoding "user" in those test
 * call sites is the right shape since the tests are pinning that exact
 * canonical form as the contract.  Promote to the header only if a
 * non-test consumer in a different TU needs to assert against the
 * literal directly. */
#define MEMORY_ALIAS_ABSTRACT_USER_CANONICAL "user"

int memory_db_entity_maybe_alias_user_to_self(int user_id,
                                              int64_t entity_id,
                                              const char *canonical_name,
                                              bool *out_aliased) {
   if (out_aliased)
      *out_aliased = false;
   if (user_id <= 0 || entity_id <= 0 || !canonical_name || !*canonical_name)
      return MEMORY_DB_FAILURE;

   /* Strict exact-match on the literal "user".  Multi-token ("the user") and
    * plural ("users") forms are deliberately excluded — they're ambiguous as
    * generic self-references and would invite false-positive aliasing. */
   if (strcmp(canonical_name, MEMORY_ALIAS_ABSTRACT_USER_CANONICAL) != 0)
      return MEMORY_DB_SUCCESS;

   /* Must have a user_self anchor to alias TO.  The maybe-promote helper
    * handles the "no anchor yet" side; calling both in the extraction loop
    * gives full coverage without overlap. */
   int64_t self_id = 0;
   if (memory_alias_internal_find_user_self_id(user_id, &self_id) != MEMORY_DB_SUCCESS ||
       self_id == 0)
      return MEMORY_DB_SUCCESS;

   /* Don't self-link.  The edge case here is users.real_name = "user" — the
    * maybe-promote path would have promoted this very row to is_user_self,
    * and the canonical match check above would let us through.  Refuse. */
   if (entity_id == self_id)
      return MEMORY_DB_SUCCESS;

   /* Soft-link the fresh row → anchor.  alias_link enforces the single-level
    * alias invariant (refuses if source has dependents), the
    * source != target check, and ownership; we pass the structural-identity
    * reason so the audit log distinguishes this path from cascade-driven
    * auto-merges.  composite_score < 0 = omit (no scoring fired). */
   int64_t link_id = 0;
   int rc = memory_db_entity_alias_link(user_id, entity_id, self_id, "soft",
                                        "auto-alias-user-to-self", -1.0f, NULL, &link_id);
   if (rc == MEMORY_DB_NOT_FOUND) {
      /* Benign race — row was deleted or already aliased between our check
       * and the link.  Honour the docstring's no-op contract. */
      return MEMORY_DB_SUCCESS;
   }
   if (rc != MEMORY_DB_SUCCESS)
      return rc;

   if (out_aliased)
      *out_aliased = true;
   OLOG_INFO("memory_db_alias: auto-aliased abstract 'user' entity %lld → user_self %lld "
             "(user_id=%d)",
             (long long)entity_id, (long long)self_id, user_id);
   return MEMORY_DB_SUCCESS;
}

int memory_db_entity_alias_existing_user_to_self(int user_id, bool *out_aliased) {
   if (out_aliased)
      *out_aliased = false;
   if (user_id <= 0)
      return MEMORY_DB_FAILURE;

   /* Anchor must exist; sweep is meaningless otherwise. */
   int64_t self_id = 0;
   if (memory_alias_internal_find_user_self_id(user_id, &self_id) != MEMORY_DB_SUCCESS ||
       self_id == 0)
      return MEMORY_DB_SUCCESS;

   /* Look up a canonical (canonical_id IS NULL) row whose name is exactly
    * "user".  Reuses the same lookup helper the by-real-name promote uses;
    * exact-match SQL keeps this conservative — we don't want to sweep up
    * "the user" or "users" by mistake. */
   int64_t found_id = 0;
   if (find_canonical_by_name(user_id, MEMORY_ALIAS_ABSTRACT_USER_CANONICAL, &found_id) !=
           MEMORY_DB_SUCCESS ||
       found_id == 0)
      return MEMORY_DB_SUCCESS;

   /* If the canonical 'user' row IS the anchor (real_name = "user" edge
    * case), nothing to do. */
   if (found_id == self_id)
      return MEMORY_DB_SUCCESS;

   int64_t link_id = 0;
   int rc = memory_db_entity_alias_link(user_id, found_id, self_id, "soft",
                                        "auto-alias-user-to-self", -1.0f, NULL, &link_id);
   if (rc == MEMORY_DB_NOT_FOUND)
      return MEMORY_DB_SUCCESS;
   if (rc != MEMORY_DB_SUCCESS)
      return rc;

   if (out_aliased)
      *out_aliased = true;
   OLOG_INFO("memory_db_alias: swept pre-existing 'user' entity %lld → user_self %lld "
             "(user_id=%d)",
             (long long)found_id, (long long)self_id, user_id);
   return MEMORY_DB_SUCCESS;
}

int memory_db_proposal_list_pending(int user_id,
                                    memory_alias_proposal_row_t *out,
                                    int max,
                                    int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || max <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   /* JOIN once with COALESCE so soft-deleted entity rows render gracefully
    * (canonical_name = '' rather than NULL).  idx_merge_proposals_pending
    * (partial on resolved_at IS NULL) drives the outer scan. */
   const char *sql = "SELECT p.id, p.source_entity_id, p.target_entity_id, "
                     "       COALESCE(s.canonical_name, ''), "
                     "       COALESCE(t.canonical_name, ''), "
                     "       p.composite_score, p.proposed_at "
                     "FROM memory_entity_merge_proposals p "
                     "LEFT JOIN memory_entities s ON s.id = p.source_entity_id "
                     "LEFT JOIN memory_entities t ON t.id = p.target_entity_id "
                     "WHERE p.user_id = ? AND p.resolved_at IS NULL "
                     "ORDER BY p.proposed_at DESC LIMIT ?";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
      memory_alias_proposal_row_t *row = &out[n];
      memset(row, 0, sizeof(*row));
      row->proposal_id = sqlite3_column_int64(stmt, 0);
      row->source_entity_id = sqlite3_column_int64(stmt, 1);
      row->target_entity_id = sqlite3_column_int64(stmt, 2);
      const char *src = (const char *)sqlite3_column_text(stmt, 3);
      if (src) {
         safe_strscpy(row->source_canonical_name, src);
      }
      const char *tgt = (const char *)sqlite3_column_text(stmt, 4);
      if (tgt) {
         safe_strscpy(row->target_canonical_name, tgt);
      }
      row->composite_score = (float)sqlite3_column_double(stmt, 5);
      row->proposed_at = sqlite3_column_int64(stmt, 6);
      n++;
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = n;
   return MEMORY_DB_SUCCESS;
}

int memory_db_proposal_resolve(int user_id,
                               int64_t proposal_id,
                               bool approved,
                               int64_t *out_link_id) {
   if (out_link_id)
      *out_link_id = 0;
   if (proposal_id <= 0)
      return MEMORY_DB_FAILURE;

   /* Race-discipline (Ckpt 5 fold-in for arch-M2 / emb-L4):
    *
    *   Phase 1: claim the proposal atomically — UPDATE …
    *   resolution=? WHERE resolved_at IS NULL inside BEGIN IMMEDIATE +
    *   COMMIT, capturing changes().  If changes()==0 a concurrent operator
    *   resolved it first and we return NOT_FOUND with no further side
    *   effects.
    *
    *   Phase 2: only when changes()==1 AND approved==true, call
    *   memory_db_entity_alias_link.  If alias_link fails post-claim, the
    *   proposal stays stamped resolved="approved" but no link materialized
    *   — log loudly so an operator can split-and-reproposal-or-reject; the
    *   row is still in a clean closed state.
    *
    * Inverted ordering vs Ckpt 4 fold-in: previously alias_link ran first
    * and a no-op UPDATE risked leaving an orphan alias.  Claim-first
    * eliminates the orphan-on-success-with-noop-update window. */

   /* Phase 1: claim. */
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* BEGIN IMMEDIATE so we hold the reserved-lock across the load+update
    * pair; this is also the explicit signal that we're stamping ownership
    * before any later side effect. */
   if (sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   /* Load source/target so we can fire alias_link after commit. */
   sqlite3_stmt *load = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT source_entity_id, target_entity_id "
                          "FROM memory_entity_merge_proposals "
                          "WHERE id = ? AND user_id = ? AND resolved_at IS NULL",
                          -1, &load, NULL) != SQLITE_OK) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int64(load, 1, proposal_id);
   sqlite3_bind_int(load, 2, user_id);

   int rc = sqlite3_step(load);
   if (rc == SQLITE_DONE) {
      sqlite3_finalize(load);
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_NOT_FOUND;
   }
   if (rc != SQLITE_ROW) {
      sqlite3_finalize(load);
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   int64_t source_id = sqlite3_column_int64(load, 0);
   int64_t target_id = sqlite3_column_int64(load, 1);
   sqlite3_finalize(load);

   /* Stamp resolved_at + resolution.  Idempotent against concurrent
    * resolvers via the resolved_at IS NULL predicate. */
   sqlite3_stmt *upd = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE memory_entity_merge_proposals "
                          "SET resolved_at = strftime('%s','now'), resolution = ? "
                          "WHERE id = ? AND user_id = ? AND resolved_at IS NULL",
                          -1, &upd, NULL) != SQLITE_OK) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_text(upd, 1, approved ? "approved" : "rejected", -1, SQLITE_STATIC);
   sqlite3_bind_int64(upd, 2, proposal_id);
   sqlite3_bind_int(upd, 3, user_id);
   rc = sqlite3_step(upd);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_finalize(upd);

   if (rc != SQLITE_DONE) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   if (changes == 0) {
      /* Lost the race — another operator stamped the row between our load
       * and our UPDATE (only possible if BEGIN IMMEDIATE was deferred or
       * if a non-AUTH_DB_LOCK writer beat us — rare in practice but the
       * predicate guards correctness either way). */
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_NOT_FOUND;
   }
   if (sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   AUTH_DB_UNLOCK();

   /* Phase 2: on approve, materialize the alias link.  If this fails
    * after a successful claim the proposal stays stamped but no alias was
    * created — log loudly so an operator can intervene. */
   int64_t link_id = 0;
   if (approved) {
      int link_rc = memory_db_entity_alias_link(user_id, source_id, target_id, "soft",
                                                "operator-approved-from-proposal", -1.0f, NULL,
                                                &link_id);
      if (link_rc != MEMORY_DB_SUCCESS) {
         OLOG_WARNING("memory_db_alias: proposal %lld claimed approved but alias_link failed "
                      "(rc=%d, source=%lld, target=%lld) — proposal stays stamped, no alias "
                      "materialized; operator may need to manually re-link or split",
                      (long long)proposal_id, link_rc, (long long)source_id, (long long)target_id);
         return MEMORY_DB_FAILURE;
      }
   }

   if (out_link_id)
      *out_link_id = link_id;
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * link-user-self orchestrator (design §8 Path B)
 *
 * Synchronous — at the dev's ~2k entity scale the resolver cascade per
 * entity (~10-20ms) puts the call in the tens-of-seconds range, fine for an
 * operator-driven backfill.  Phase 2 may revisit with a detached-thread +
 * status-query pattern if entities-per-user crosses ~10k.
 * ============================================================================= */

/* Insert a fresh user-self canonical row.  Acquires its own auth_db lock. */
static int seed_user_self_entity(int user_id,
                                 const char *display_name,
                                 const char *canonical_name,
                                 int64_t *out_id) {
   if (out_id)
      *out_id = 0;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   const char *sql = "INSERT INTO memory_entities "
                     "(user_id, name, entity_type, canonical_name, is_user_self, mention_count) "
                     "VALUES (?, ?, 'person', ?, 1, 0)";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, display_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, canonical_name, -1, SQLITE_TRANSIENT);

   int rc = sqlite3_step(stmt);
   if (rc != SQLITE_DONE) {
      int err = sqlite3_extended_errcode(s_db.db);
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();
      /* UNIQUE(user_id, canonical_name) collision — see MEMORY_DB_SELF_NAME_COLLISION
       * in memory_types.h for resolution path.  Caller surfaces the
       * actionable hint to the operator. */
      if (err == SQLITE_CONSTRAINT_UNIQUE || err == SQLITE_CONSTRAINT_PRIMARYKEY) {
         OLOG_WARNING("memory_db_alias: seed user-self UNIQUE collision on canonical_name "
                      "'%s' for user %d — composite scored below promotion threshold; "
                      "operator must promote manually or change real_name",
                      canonical_name, user_id);
         return MEMORY_DB_SELF_NAME_COLLISION;
      }
      OLOG_ERROR("memory_db_alias: seed user-self failed: %s", sqlite3_errmsg(s_db.db));
      return MEMORY_DB_FAILURE;
   }
   if (out_id)
      *out_id = sqlite3_last_insert_rowid(s_db.db);
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return MEMORY_DB_SUCCESS;
}

/* Set is_user_self=1 on an existing entity (Path B step 1 — promote existing
 * match instead of inserting a fresh seed).  Caller is responsible for
 * ensuring no other entity for this user currently has is_user_self=1
 * (the partial UNIQUE index would otherwise block the UPDATE).  The SQL
 * also predicates `canonical_id IS NULL` so this helper refuses to
 * promote an alias row to user_self — alias rows are shadowed by their
 * canonical and would create inconsistent state.  Returns MEMORY_DB_NOT_
 * FOUND when no row matches all three predicates (already-self, alias,
 * or wrong user). */
static int promote_to_user_self_entity(int user_id, int64_t entity_id) {
   if (entity_id <= 0 || user_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   const char *sql = "UPDATE memory_entities SET is_user_self = 1 "
                     "WHERE id = ? AND user_id = ? AND is_user_self = 0 "
                     "  AND canonical_id IS NULL";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int64(stmt, 1, entity_id);
   sqlite3_bind_int(stmt, 2, user_id);

   int rc = sqlite3_step(stmt);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db_alias: promote-to-user-self failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   int changes = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   if (changes == 0) {
      OLOG_WARNING("memory_db_alias: promote-to-user-self matched no rows (entity_id=%lld, "
                   "user_id=%d, may already be is_user_self=1 or canonical_id != NULL)",
                   (long long)entity_id, user_id);
      return MEMORY_DB_NOT_FOUND;
   }
   return MEMORY_DB_SUCCESS;
}

/* Canonical-only entity ids for the user, mention_count DESC. */
static int list_canonical_entity_ids(int user_id,
                                     int64_t exclude_id,
                                     int64_t *out_ids,
                                     int max,
                                     int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_ids || max <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   const char *sql = "SELECT id FROM memory_entities WHERE user_id = ? AND id != ? "
                     "  AND canonical_id IS NULL "
                     "ORDER BY mention_count DESC, id ASC LIMIT ?";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, exclude_id);
   sqlite3_bind_int(stmt, 3, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
      out_ids[n++] = sqlite3_column_int64(stmt, 0);
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   *count_out = n;
   return MEMORY_DB_SUCCESS;
}

/* Load the user-identity fields (real_name, preferred_address,
 * identity_aliases) via the public auth_db_get_user_identity() helper.
 * UTF-8 trim is applied to every TEXT field so a downstream tokenizer
 * doesn't see a malformed leader at any column boundary.
 *
 * Returns MEMORY_DB_SUCCESS with all-empty fields if the user row exists
 * but no identity fields are populated; MEMORY_DB_NOT_FOUND if the user
 * doesn't exist; MEMORY_DB_FAILURE on bind/step error. */
static int load_user_identity(int user_id, auth_user_identity_t *out_identity) {
   if (!out_identity)
      return MEMORY_DB_FAILURE;
   memset(out_identity, 0, sizeof(*out_identity));

   int rc = auth_db_get_user_identity(user_id, out_identity);
   if (rc == AUTH_DB_NOT_FOUND)
      return MEMORY_DB_NOT_FOUND;
   if (rc != AUTH_DB_SUCCESS)
      return MEMORY_DB_FAILURE;

   /* Trailing-partial-UTF-8 trim on every TEXT field — auth_db_get_user_identity
    * applies a hard byte cap on each column, which can split a multi-byte
    * sequence at the cap boundary.  The downstream tokenizer (memory_make_
    * canonical_name) is byte-oriented and needs valid input. */
   memory_alias_internal_trim_trailing_partial_utf8(out_identity->real_name);
   memory_alias_internal_trim_trailing_partial_utf8(out_identity->preferred_address);
   memory_alias_internal_trim_trailing_partial_utf8(out_identity->identity_aliases);
   return MEMORY_DB_SUCCESS;
}

/* Build the synthetic memory_entity_t for the dry-run-with-no-self path
 * from the user's identity record.
 *
 *   name           = real_name (display)
 *   entity_type    = "person"
 *   canonical_name = tokens(real_name) ∪ tokens(each alias line)
 *
 * preferred_address is intentionally NOT used here — it's display-only
 * (system prompt, UI), not a search anchor.  If the same string also
 * appears on a candidate's canonical_name, token-Jaccard will still pick
 * it up via real_name overlap.
 *
 * Aliases parsing matches the brief: split on '\n', strip whitespace per
 * token, drop empties, dedupe case-insensitive, canonicalize each kept
 * line, append unique tokens to the synthetic canonical_name (whitespace-
 * separated so memory_alias_compute_name_jaccard's tokenizer sees them as
 * separate tokens). */
static void build_synthetic_self_entity(int user_id,
                                        const auth_user_identity_t *identity,
                                        memory_entity_t *out_synth) {
   memset(out_synth, 0, sizeof(*out_synth));
   out_synth->id = 0; /* sentinel — score_pair_full's DB-touching helpers
                       * (relations, contacts, embedding cache) all return
                       * 0 for id=0, which is the correct behavior for a
                       * not-yet-materialized entity. */
   out_synth->user_id = user_id;
   safe_strscpy(out_synth->name, identity->real_name);
   safe_strscpy(out_synth->entity_type, "person");

   /* Start canonical_name with real_name canonicalized; this is the
    * primary token surface. */
   char canon[MEMORY_ENTITY_NAME_MAX];
   memory_make_canonical_name(identity->real_name, canon, sizeof(canon));

   /* Union with alias-line tokens.  Up to 16 aliases tracked for dedupe. */
   if (identity->identity_aliases[0] != '\0') {
      char buf[AUTH_IDENTITY_ALIASES_MAX];
      safe_strscpy(buf, identity->identity_aliases);

      char *seen[16];
      int seen_count = 0;
      int dropped_overflow = 0;
      char *save = NULL;
      for (char *line = strtok_r(buf, "\n", &save); line != NULL;
           line = strtok_r(NULL, "\n", &save)) {
         if (seen_count >= 16) {
            dropped_overflow++;
            continue;
         }
         while (*line == ' ' || *line == '\t' || *line == '\r')
            line++;
         char *end = line + strlen(line);
         while (end > line && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
            end--;
         *end = '\0';
         if (*line == '\0')
            continue;
         /* Case-insensitive dedupe. */
         bool dup = false;
         for (int i = 0; i < seen_count; i++) {
            if (strcasecmp(seen[i], line) == 0) {
               dup = true;
               break;
            }
         }
         if (dup)
            continue;
         seen[seen_count++] = line;

         char canon_alias[MEMORY_ENTITY_NAME_MAX];
         memory_make_canonical_name(line, canon_alias, sizeof(canon_alias));
         if (!canon_alias[0])
            continue;
         size_t cur = strlen(canon);
         size_t alias_len = strlen(canon_alias);
         size_t need = (cur > 0 ? 1 : 0) + alias_len;
         if (cur + need + 1 >= sizeof(canon))
            break;
         if (cur > 0) {
            canon[cur++] = ' ';
         }
         /* The guard above proved cur + alias_len + 1 <= sizeof(canon), so the
          * alias plus its NUL fits exactly. memcpy, not strncat: the length is
          * exact, so strncat's truncation semantics (which the -O2 optimizer flags
          * as a possible cut it can't rule out) don't apply. */
         memcpy(canon + cur, canon_alias, alias_len + 1);
      }
      if (dropped_overflow > 0) {
         OLOG_WARNING("identity_aliases truncated: %d alias line(s) past the 16-entry "
                      "dedupe cap were silently dropped (user %d)",
                      dropped_overflow, user_id);
      }
   }
   safe_strscpy(out_synth->canonical_name, canon);

   out_synth->mention_count = 0;
   out_synth->first_seen = time(NULL);
   out_synth->last_seen = 0;
}

int memory_alias_link_user_self_run(int user_id,
                                    bool dry_run,
                                    memory_alias_link_user_self_result_t *result) {
   if (!result || user_id <= 0)
      return MEMORY_DB_FAILURE;
   memset(result, 0, sizeof(*result));

   /* Step 1: locate (or seed) the user-self canonical. */
   int64_t self_id = 0;
   int find_rc = memory_alias_internal_find_user_self_id(user_id, &self_id);
   if (find_rc == MEMORY_DB_FAILURE)
      return MEMORY_DB_FAILURE;

   /* Load the user's identity record up-front — used for both the seed
    * path (when no is_user_self=1 row exists) and the synthetic-self
    * dry-run path.  v44 (Phase 1.5) replaces the previous username-as-
    * identity reach-around. */
   auth_user_identity_t identity;
   memset(&identity, 0, sizeof(identity));
   int id_rc = load_user_identity(user_id, &identity);
   if (id_rc == MEMORY_DB_NOT_FOUND)
      return MEMORY_DB_NOT_FOUND;
   if (id_rc != MEMORY_DB_SUCCESS)
      return MEMORY_DB_FAILURE;

   /* Phase 1.5 Ckpt D gate: real_name must be set (non-empty, not all
    * whitespace) for either the synthetic seed OR the on-commit seeded
    * canonical to have a meaningful name to anchor.  Without it the run
    * would degrade silently; the explicit error code lets the admin
    * handler surface a "Configure WebUI Settings → User → Real name"
    * hint to the operator. */
   const char *rn = identity.real_name;
   while (*rn == ' ' || *rn == '\t' || *rn == '\r' || *rn == '\n')
      rn++;
   if (*rn == '\0')
      return MEMORY_DB_REAL_NAME_REQUIRED;

   if (find_rc == MEMORY_DB_SUCCESS && self_id > 0) {
      memory_entity_t self_ent;
      if (memory_alias_internal_load_entity_full(user_id, self_id, &self_ent, NULL, NULL) !=
          MEMORY_DB_SUCCESS) {
         return MEMORY_DB_FAILURE;
      }
      result->self_entity_id = self_id;
      result->self_was_seeded = false;
      safe_strscpy(result->self_canonical_name, self_ent.canonical_name);
   } else {
      /* No is_user_self=1 row exists — per design §8 Path B step 1, first
       * try to find an existing entity that matches the synthetic strongly
       * (composite ≥ MEMORY_ALIAS_SELF_PROMOTION_THRESHOLD) and promote it
       * (UPDATE is_user_self=1).  Only if no strong match exists do we seed
       * a fresh entity.  Phase 1.5 Ckpt D added an explicit real_name gate
       * above this point; an empty real_name has already returned an error. */
      char display_name[MEMORY_ENTITY_NAME_MAX];
      safe_strscpy(display_name, identity.real_name);

      char canonical_name[MEMORY_ENTITY_NAME_MAX];
      memory_make_canonical_name(display_name, canonical_name, sizeof(canonical_name));

      /* Step 1 — search for a promotion candidate via the synthetic resolver.
       * Reuses the Stage 1-6 cascade with use_synth_self=true; returns the
       * highest-composite match (or 0 if no candidate cleared the cascade).
       *
       * Stage 1 (exact canonical_name match) is a fast-path that bypasses
       * scoring — composite_score stays 0, but the match itself is the
       * strongest possible signal (the user's real_name canonicalizes to
       * an entity that already exists).  Treat Stage 1 as an unconditional
       * promote-trigger; the composite-threshold gate applies only to
       * Stage 6 winners (fuzzy matches via name similarity + bonuses). */
      memory_alias_resolve_t self_resolve;
      memset(&self_resolve, 0, sizeof(self_resolve));
      int resolve_rc = memory_db_entity_resolve_alias_for_self(user_id, canonical_name, "person",
                                                               &self_resolve);

      bool stage1_hit = (resolve_rc == MEMORY_DB_SUCCESS && self_resolve.resolved_id > 0 &&
                         self_resolve.matched_stage == 1);
      bool stage6_strong = (resolve_rc == MEMORY_DB_SUCCESS && self_resolve.resolved_id > 0 &&
                            self_resolve.matched_stage == 6 &&
                            self_resolve.evidence.composite_score >=
                                MEMORY_ALIAS_SELF_PROMOTION_THRESHOLD);
      bool promoted = false;
      if (stage1_hit || stage6_strong) {
         /* Strong match — promote it (commit) or report it (dry-run). */
         if (!dry_run) {
            int prc = promote_to_user_self_entity(user_id, self_resolve.resolved_id);
            if (prc != MEMORY_DB_SUCCESS) {
               return MEMORY_DB_FAILURE;
            }
            self_id = self_resolve.resolved_id;
         } else {
            self_id = self_resolve.resolved_id;
         }
         promoted = true;
         result->self_was_promoted = true;
         result->self_was_seeded = false;

         /* Pull the canonical_name from the entity row so the report
          * accurately reflects what got promoted. */
         memory_entity_t self_ent;
         if (memory_alias_internal_load_entity_full(user_id, self_id, &self_ent, NULL, NULL) ==
             MEMORY_DB_SUCCESS) {
            safe_strscpy(result->self_canonical_name, self_ent.canonical_name);
         } else {
            safe_strscpy(result->self_canonical_name, canonical_name);
         }
         result->self_entity_id = self_id;
      }

      if (!promoted) {
         /* Step 2 — fall through to fresh seed (commit) or no-self preview (dry-run). */
         if (!dry_run) {
            int seed_rc = seed_user_self_entity(user_id, display_name, canonical_name, &self_id);
            if (seed_rc == MEMORY_DB_SELF_NAME_COLLISION) {
               return MEMORY_DB_SELF_NAME_COLLISION;
            }
            if (seed_rc != MEMORY_DB_SUCCESS || self_id <= 0) {
               return MEMORY_DB_FAILURE;
            }
            result->self_was_seeded = true;
            result->self_was_promoted = false;
         } else {
            /* Dry-run preview when no canonical exists yet: self_id stays 0
             * (the report renders "(would create)") and the candidate scoring
             * loop below uses a synthetic entity for accurate band routing. */
            self_id = 0;
            result->self_was_seeded = true;
            result->self_was_promoted = false;
         }
         result->self_entity_id = self_id;
         safe_strscpy(result->self_canonical_name, canonical_name);
      }
   }

   /* Build the self-entity used for dry-run pair scoring.  Three cases:
    *   (a) dry-run + no canonical (self_id == 0): build synthetic from
    *       real_name + identity_aliases (use_synth_self path).
    *   (b) dry-run + would-promote (self_id > 0 && self_was_promoted):
    *       load the would-promoted entity but treat it as user_self for
    *       scoring (the DB UPDATE hasn't fired yet in dry-run, so
    *       memory_db_entity_score_pair would see is_user_self=0 on
    *       both sides and the bonus couldn't fire).  Use score_pair_full
    *       directly with src_is_user_self=true.
    *   (c) commit (any flavor) or pre-existing canonical: fall through
    *       to memory_db_entity_score_pair / consider_auto_merge which
    *       reads is_user_self from the DB directly. */
   memory_entity_t self_ent_for_dryrun;
   bool use_synth_self = (dry_run && self_id == 0);
   bool use_promote_dryrun = (dry_run && self_id > 0 && result->self_was_promoted);
   if (use_synth_self) {
      build_synthetic_self_entity(user_id, &identity, &self_ent_for_dryrun);
   } else if (use_promote_dryrun) {
      if (memory_alias_internal_load_entity_full(user_id, self_id, &self_ent_for_dryrun, NULL,
                                                 NULL) != MEMORY_DB_SUCCESS) {
         return MEMORY_DB_FAILURE;
      }
   }

   /* Step 2: iterate every other canonical entity for the user. */
   int64_t entity_ids[MEMORY_ALIAS_LINK_USER_SELF_MAX_ROWS];
   int entity_count = 0;
   if (list_canonical_entity_ids(user_id, self_id, entity_ids, MEMORY_ALIAS_LINK_USER_SELF_MAX_ROWS,
                                 &entity_count) != MEMORY_DB_SUCCESS) {
      return MEMORY_DB_FAILURE;
   }
   result->considered = entity_count;

   for (int i = 0; i < entity_count; i++) {
      int64_t eid = entity_ids[i];

      memory_entity_t cand;
      if (memory_alias_internal_load_entity_full(user_id, eid, &cand, NULL, NULL) !=
          MEMORY_DB_SUCCESS) {
         continue;
      }

      memory_alias_link_user_self_row_t *row = NULL;
      if (result->row_count < MEMORY_ALIAS_LINK_USER_SELF_MAX_ROWS) {
         row = &result->rows[result->row_count++];
         row->entity_id = eid;
         safe_strscpy(row->canonical_name, cand.canonical_name);
         safe_strscpy(row->entity_type, cand.entity_type);
      }

      if (dry_run) {
         memory_alias_evidence_t ev;
         memset(&ev, 0, sizeof(ev));
         if (use_synth_self || use_promote_dryrun) {
            /* Both dry-run cases use score_pair_full directly so we can
             * flag the self side as user_self even when the DB doesn't
             * carry is_user_self=1 yet (synthetic has id=0; promote-dryrun
             * is "would promote", UPDATE deferred until commit).  This
             * lets user_self_bonus_applies fire correctly on candidates
             * whose name matches the username/alias-substring conditions
             * or who are the "user" allow-list token.
             *
             * Phase 1.5 fold-in: pass the allow-list token flag through
             * for the canonical "user" entity — the unconditional bonus
             * branch fires regardless of whether the operator's username
             * happens to be a substring of "user". */
            bool other_token = (strcmp(cand.canonical_name, "user") == 0);
            memory_alias_internal_score_pair_full(user_id, &cand, /* src_is_user_self */ false,
                                                  &self_ent_for_dryrun,
                                                  /* tgt_is_user_self */ true, other_token, &ev);
         } else if (memory_db_entity_score_pair(user_id, eid, self_id, &ev) != MEMORY_DB_SUCCESS) {
            if (row)
               row->outcome = MEMORY_ALIAS_OUTCOME_REJECTED;
            result->rejected++;
            continue;
         }
         if (row) {
            row->composite_score = ev.composite_score;
            row->user_self_bonus_applied = ev.user_self_bonus_applied;
         }
         if (ev.composite_score >= (float)g_config.memory.entity_merge_auto_threshold) {
            if (row)
               row->outcome = MEMORY_ALIAS_OUTCOME_AUTO_MERGED;
            result->auto_merged++;
         } else if (ev.composite_score >= (float)g_config.memory.entity_merge_review_threshold) {
            if (row)
               row->outcome = MEMORY_ALIAS_OUTCOME_PROPOSED;
            result->proposed++;
         } else {
            if (row)
               row->outcome = MEMORY_ALIAS_OUTCOME_REJECTED;
            result->rejected++;
         }
      } else {
         /* Commit path: score the candidate against the user-self
          * specifically, NOT via consider_auto_merge (which would find
          * the candidate's best generic match — for "llama 3.1" that's
          * its sibling "llama", not the user-self).  link-user-self
          * semantics: cluster scoring is always against the chosen
          * canonical, with band routing to alias_link / proposal. */
         memory_alias_evidence_t ev;
         memset(&ev, 0, sizeof(ev));
         memory_entity_t self_ent;
         if (memory_alias_internal_load_entity_full(user_id, self_id, &self_ent, NULL, NULL) !=
             MEMORY_DB_SUCCESS) {
            if (row)
               row->outcome = MEMORY_ALIAS_OUTCOME_REJECTED;
            result->rejected++;
            continue;
         }
         /* Allow-list flag: candidate is "user" canonical → unconditional bonus.
          * Self side is the would-be-canonical; src_is_user_self=false (cand) and
          * tgt_is_user_self=true (self) so user_self_bonus_applies fires correctly. */
         bool other_token = (strcmp(cand.canonical_name, "user") == 0);
         memory_alias_internal_score_pair_full(user_id, &cand, /* src_is_user_self */ false,
                                               &self_ent,
                                               /* tgt_is_user_self */ true, other_token, &ev);

         int outcome = MEMORY_ALIAS_OUTCOME_REJECTED;
         int64_t link_id = 0, proposal_id = 0;
         if (ev.composite_score >= (float)g_config.memory.entity_merge_auto_threshold) {
            /* Soft-link: alias source (cand) to target (self).  Reason
             * tag identifies the operator workflow that fired this. */
            int link_rc = memory_db_entity_alias_link(user_id, eid, self_id, "soft",
                                                      "link-user-self", ev.composite_score,
                                                      /* evidence_json */ NULL, &link_id);
            if (link_rc == MEMORY_DB_SUCCESS) {
               outcome = MEMORY_ALIAS_OUTCOME_AUTO_MERGED;
               result->auto_merged++;
            } else {
               outcome = MEMORY_ALIAS_OUTCOME_REJECTED;
               result->rejected++;
            }
         } else if (ev.composite_score >= (float)g_config.memory.entity_merge_review_threshold) {
            /* Queue for operator review via the WebUI Suggested-Merges panel.
             *
             * Direction preference: same rule the standard cascade's propose
             * path uses (consider_auto_merge, search for `prop_src`).  If the
             * candidate is the fuller person/pet/place form and the user_self
             * anchor is currently a leaf, store the proposal pre-swapped so
             * the operator sees the right direction.  Approving will then
             * flip the alias direction accordingly.  Leaf-check inside the
             * helper protects the common case where the anchor already has
             * dependents — swap silently skips and the operator gets the
             * un-swapped direction (correct: can't demote an established
             * anchor without breaking the single-level alias invariant).
             *
             * Deliberately NOT applied to the AUTO branch above: AUTO with
             * link-user-self reason is operator-directed direction (the
             * `dawn-admin memory entity link-user-self` flow with the
             * operator's chosen self_id as anchor) and silent flipping
             * would override intent.  Propose-time defers to operator
             * review, so a swap suggestion is honest. */
            int64_t prop_src = eid;
            int64_t prop_tgt = self_id;
            if (should_swap_for_longer_canonical(user_id, &cand, self_id)) {
               prop_src = self_id;
               prop_tgt = eid;
            }
            int prop_rc = memory_alias_internal_insert_merge_proposal(user_id, prop_src, prop_tgt,
                                                                      ev.composite_score,
                                                                      /* evidence_json */ NULL,
                                                                      &proposal_id);
            if (prop_rc == MEMORY_DB_SUCCESS) {
               outcome = MEMORY_ALIAS_OUTCOME_PROPOSED;
               result->proposed++;
            } else {
               outcome = MEMORY_ALIAS_OUTCOME_REJECTED;
               result->rejected++;
            }
         } else {
            outcome = MEMORY_ALIAS_OUTCOME_REJECTED;
            result->rejected++;
         }

         if (row) {
            row->outcome = outcome;
            row->composite_score = ev.composite_score;
            row->user_self_bonus_applied = ev.user_self_bonus_applied;
            row->link_id = link_id;
            row->proposal_id = proposal_id;
         }
      }
   }
   return MEMORY_DB_SUCCESS;
}
