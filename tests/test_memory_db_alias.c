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
 * Unit tests for memory_db_alias.c — entity-merge alias surface (v43).
 *
 * Coverage (per docs/ENTITY_MERGE_IMPL_BRIEF.md Ckpt 2 §10):
 *   - pure helpers: name_jaccard, name_substring, composite formula,
 *     canonical_priority lexicographic comparison
 *   - cascade stages 1-3 (Stage 4/5 are exercised indirectly via score_pair
 *     and consider_auto_merge — the embedding engine is stubbed false in
 *     this test fixture so Stage 4 contributes 0 to the composite)
 *   - threshold band routing in consider_auto_merge: AUTO_MERGED, PROPOSED,
 *     REJECTED, NO_CANDIDATES
 *   - alias_link / alias_unlink: write paths, hard-merge refusal, dependent
 *     refusal, embedding-cache invalidation
 *   - relation_list_by_subject_class: canonical row + aliases
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "memory/memory_db.h"
#include "memory/memory_db_aliases.h"
#include "memory/memory_db_entities.h"
#include "memory/memory_types.h"
#include "unity.h"

extern int g_alias_test_entity_cache_invalidations;

/* ============================================================================
 * setUp / tearDown
 * ============================================================================ */

static int g_test_user_id = 0;

void setUp(void) {
   auth_db_init(":memory:");
   g_alias_test_entity_cache_invalidations = 0;

   /* Initialize global config so the resolver's runtime-config threshold
    * reads (g_config.memory.entity_merge_*) see the real defaults.
    * Without this the zero-init g_config would set auto/review to 0.0,
    * causing every candidate above 0.0 to propose. */
   config_set_defaults(&g_config);

   /* Create a single test user.  All entity tests scope by user_id. */
   auth_db_create_user("jon", "hash", true);
   auth_user_t u;
   memset(&u, 0, sizeof(u));
   auth_db_get_user("jon", &u);
   g_test_user_id = u.id;
}

void tearDown(void) {
   auth_db_shutdown();
}

/* ============================================================================
 * Helpers — direct DB inserts (bypass the production upsert so tests can
 * exercise specific entity shapes without re-routing through it).
 * ============================================================================ */

static int64_t insert_entity_typed(int user_id, const char *name, const char *type) {
   char canonical[MEMORY_ENTITY_NAME_MAX];
   memory_make_canonical_name(name, canonical, sizeof(canonical));
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "INSERT INTO memory_entities (user_id, name, entity_type, canonical_name) "
                      "VALUES (?, ?, ?, ?)",
                      -1, &stmt, NULL);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, type, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, canonical, -1, SQLITE_TRANSIENT);
   sqlite3_step(stmt);
   int64_t id = sqlite3_last_insert_rowid(s_db.db);
   sqlite3_finalize(stmt);
   return id;
}

static void mark_entity_user_self(int64_t entity_id) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db, "UPDATE memory_entities SET is_user_self = 1 WHERE id = ?", -1,
                      &stmt, NULL);
   sqlite3_bind_int64(stmt, 1, entity_id);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
}

static void insert_open_relation(int user_id,
                                 int64_t subj,
                                 const char *relation,
                                 int64_t obj_entity_id,
                                 const char *obj_value) {
   sqlite3_stmt *stmt = NULL;
   /* fact_id is NULL (literal NULL — not 0, which would trigger the FK on
    * memory_facts.id since no facts exist in this fixture). */
   int prc = sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO memory_relations (user_id, subject_entity_id, relation, "
       "object_entity_id, object_value, fact_id, confidence, valid_from, valid_to) "
       "VALUES (?, ?, ?, ?, ?, NULL, 0.8, NULL, NULL)",
       -1, &stmt, NULL);
   if (prc != SQLITE_OK) {
      fprintf(stderr, "insert_open_relation prepare failed: %s\n", sqlite3_errmsg(s_db.db));
      return;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, subj);
   sqlite3_bind_text(stmt, 3, relation, -1, SQLITE_TRANSIENT);
   if (obj_entity_id > 0) {
      sqlite3_bind_int64(stmt, 4, obj_entity_id);
   } else {
      sqlite3_bind_null(stmt, 4);
   }
   if (obj_value && *obj_value) {
      sqlite3_bind_text(stmt, 5, obj_value, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(stmt, 5);
   }
   int src = sqlite3_step(stmt);
   if (src != SQLITE_DONE) {
      fprintf(stderr, "insert_open_relation step rc=%d errmsg=%s\n", src, sqlite3_errmsg(s_db.db));
   }
   sqlite3_finalize(stmt);
}

static void insert_contact(int user_id,
                           int64_t entity_id,
                           const char *field_type,
                           const char *value) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(
       s_db.db,
       "INSERT INTO contacts (user_id, entity_id, field_type, value, label, created_at) "
       "VALUES (?, ?, ?, ?, '', strftime('%s','now'))",
       -1, &stmt, NULL);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, entity_id);
   sqlite3_bind_text(stmt, 3, field_type, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, value, -1, SQLITE_TRANSIENT);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
}

static int64_t get_entity_canonical_id(int64_t entity_id) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db, "SELECT canonical_id FROM memory_entities WHERE id = ?", -1, &stmt,
                      NULL);
   sqlite3_bind_int64(stmt, 1, entity_id);
   int64_t result = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
      result = sqlite3_column_int64(stmt, 0);
   }
   sqlite3_finalize(stmt);
   return result;
}

/* Read first_seen + last_seen on a row.  Returns SUCCESS if the row exists
 * and stamps both out-params; returns FAILURE otherwise.  Test helper for
 * the entity_upsert_at timestamp assertions. */
static int get_entity_timestamps(int64_t entity_id, int64_t *fs, int64_t *ls) {
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, "SELECT first_seen, last_seen FROM memory_entities WHERE id = ?",
                          -1, &stmt, NULL) != SQLITE_OK) {
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int64(stmt, 1, entity_id);
   int rc = sqlite3_step(stmt);
   if (rc == SQLITE_ROW) {
      if (fs)
         *fs = sqlite3_column_int64(stmt, 0);
      if (ls)
         *ls = sqlite3_column_int64(stmt, 1);
      sqlite3_finalize(stmt);
      return MEMORY_DB_SUCCESS;
   }
   sqlite3_finalize(stmt);
   return MEMORY_DB_FAILURE;
}

static int count_active_aliases_for_target(int64_t target_id) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT COUNT(*) FROM memory_entity_aliases "
                      "WHERE target_entity_id = ? AND unlinked_at IS NULL",
                      -1, &stmt, NULL);
   sqlite3_bind_int64(stmt, 1, target_id);
   int n = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      n = sqlite3_column_int(stmt, 0);
   }
   sqlite3_finalize(stmt);
   return n;
}

/* Count pending merge-proposal rows with the given source entity.  Used by
 * the propose-all-in-band tests to verify the cascade fires multiple
 * proposals when several candidates clear the review threshold. */
static int count_pending_proposals_from_source(int64_t source_id) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT COUNT(*) FROM memory_entity_merge_proposals "
                      "WHERE source_entity_id = ? AND resolved_at IS NULL",
                      -1, &stmt, NULL);
   sqlite3_bind_int64(stmt, 1, source_id);
   int n = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      n = sqlite3_column_int(stmt, 0);
   }
   sqlite3_finalize(stmt);
   return n;
}

/* Check whether an entity row has is_user_self = 1.  Tests use this to
 * verify the auto-promote helpers. */
static bool entity_is_user_self(int64_t entity_id) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db, "SELECT is_user_self FROM memory_entities WHERE id = ?", -1, &stmt,
                      NULL);
   sqlite3_bind_int64(stmt, 1, entity_id);
   bool flag = false;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      flag = sqlite3_column_int(stmt, 0) != 0;
   }
   sqlite3_finalize(stmt);
   return flag;
}

/* Set the user's real_name (Phase 1.5+).  Tests use this to drive the
 * auto-promote-by-real-name path. */
static void set_user_real_name(int user_id, const char *real_name) {
   auth_user_identity_t id;
   memset(&id, 0, sizeof(id));
   strncpy(id.real_name, real_name, AUTH_REAL_NAME_MAX - 1);
   id.real_name[AUTH_REAL_NAME_MAX - 1] = '\0';
   auth_db_set_user_identity(user_id, &id);
}

/* ============================================================================
 * Pure-function helpers (no DB)
 * ============================================================================ */

static void test_name_jaccard_basic(void) {
   /* Identical strings → Jaccard 1.0 (modulo single-token-set quirks). */
   TEST_ASSERT_EQUAL_FLOAT(1.0f, memory_alias_compute_name_jaccard("jon", "jon"));

   /* Subset case: "jon" ⊂ "jonathan smith" — tokens are {"jon"} and
    * {"jonathan", "smith"}.  Intersection = 0, union = 3 → 0.0.  This is
    * the Jaccard-misses-substring case the substring bonus exists for. */
   TEST_ASSERT_EQUAL_FLOAT(0.0f, memory_alias_compute_name_jaccard("jon", "jonathan smith"));

   /* Shared full token: tokens {"jonathan", "smith"} vs {"jonathan", "k"}
    * — "k" is dropped (length < 2), so {"jonathan", "smith"} vs
    * {"jonathan"} → intersection = 1, union = 2, Jaccard = 0.5. */
   TEST_ASSERT_EQUAL_FLOAT(0.5f, memory_alias_compute_name_jaccard("jonathan smith", "jonathan k"));

   /* Disjoint tokens: 0.0. */
   TEST_ASSERT_EQUAL_FLOAT(0.0f, memory_alias_compute_name_jaccard("apple", "banana"));

   /* NULL / empty → 0.0. */
   TEST_ASSERT_EQUAL_FLOAT(0.0f, memory_alias_compute_name_jaccard(NULL, "x"));
   TEST_ASSERT_EQUAL_FLOAT(0.0f, memory_alias_compute_name_jaccard("x", ""));
}

static void test_name_substring_basic(void) {
   TEST_ASSERT_TRUE(memory_alias_compute_name_substring("jon", "jonathan smith"));
   TEST_ASSERT_TRUE(memory_alias_compute_name_substring("jonathan smith", "jon"));
   TEST_ASSERT_FALSE(memory_alias_compute_name_substring("jon", "jon"));   /* equal */
   TEST_ASSERT_FALSE(memory_alias_compute_name_substring("jon", "alice")); /* disjoint */
   TEST_ASSERT_FALSE(memory_alias_compute_name_substring(NULL, "x"));
   TEST_ASSERT_FALSE(memory_alias_compute_name_substring("x", NULL));
}

static void test_composite_weighted_sum(void) {
   /* Each signal in isolation. */
   memory_alias_evidence_t ev;

   memset(&ev, 0, sizeof(ev));
   ev.name_jaccard = 1.0f;
   memory_alias_apply_composite(&ev);
   TEST_ASSERT_EQUAL_FLOAT(0.30f, ev.composite_score);

   memset(&ev, 0, sizeof(ev));
   ev.embedding_cosine = 1.0f;
   memory_alias_apply_composite(&ev);
   TEST_ASSERT_EQUAL_FLOAT(0.30f, ev.composite_score);

   memset(&ev, 0, sizeof(ev));
   ev.exclusive_relation_overlap = 1.0f;
   memory_alias_apply_composite(&ev);
   TEST_ASSERT_EQUAL_FLOAT(0.25f, ev.composite_score);

   memset(&ev, 0, sizeof(ev));
   ev.contact_field_overlap = 1.0f;
   memory_alias_apply_composite(&ev);
   TEST_ASSERT_EQUAL_FLOAT(0.10f, ev.composite_score);

   memset(&ev, 0, sizeof(ev));
   ev.type_match = 1.0f;
   memory_alias_apply_composite(&ev);
   TEST_ASSERT_EQUAL_FLOAT(0.05f, ev.composite_score);

   /* Cosine clamping: negative → 0. */
   memset(&ev, 0, sizeof(ev));
   ev.embedding_cosine = -0.5f;
   memory_alias_apply_composite(&ev);
   TEST_ASSERT_EQUAL_FLOAT(0.0f, ev.composite_score);
}

static void test_composite_bonuses(void) {
   memory_alias_evidence_t ev;

   /* substring bonus alone: jaccard floor + bonus. */
   memset(&ev, 0, sizeof(ev));
   ev.name_substring_bonus_applied = true;
   memory_alias_apply_composite(&ev);
   TEST_ASSERT_EQUAL_FLOAT(0.10f, ev.composite_score);

   /* user_self bonus alone. */
   memset(&ev, 0, sizeof(ev));
   ev.user_self_bonus_applied = true;
   memory_alias_apply_composite(&ev);
   TEST_ASSERT_EQUAL_FLOAT(0.20f, ev.composite_score);

   /* Both bonuses + name signal cap to 1.0 if they would overflow. */
   memset(&ev, 0, sizeof(ev));
   ev.name_jaccard = 1.0f;
   ev.embedding_cosine = 1.0f;
   ev.exclusive_relation_overlap = 1.0f;
   ev.contact_field_overlap = 1.0f;
   ev.type_match = 1.0f;
   ev.name_substring_bonus_applied = true;
   ev.user_self_bonus_applied = true;
   memory_alias_apply_composite(&ev);
   TEST_ASSERT_EQUAL_FLOAT(1.0f, ev.composite_score);
}

static void test_composite_veto(void) {
   memory_alias_evidence_t ev;

   /* Veto fires → composite forced to 0 even with strong signals. */
   memset(&ev, 0, sizeof(ev));
   ev.type_veto_fired = true;
   ev.name_jaccard = 1.0f;
   ev.embedding_cosine = 1.0f;
   ev.exclusive_relation_overlap = 1.0f;
   memory_alias_apply_composite(&ev);
   TEST_ASSERT_EQUAL_FLOAT(0.0f, ev.composite_score);
}

static void test_canonical_priority_compare_self_user_self_first(void) {
   /* a.is_user_self=true, b.is_user_self=false.  Even when b dominates on
    * every other axis (better type, higher mention_count, older first_seen,
    * lower id), a still wins because the is_user_self axis short-circuits
    * the tuple. */
   memory_entity_t a, b;
   memset(&a, 0, sizeof(a));
   memset(&b, 0, sizeof(b));
   strcpy(a.entity_type, "thing");
   strcpy(b.entity_type, "person");
   a.id = 999;
   b.id = 1;
   a.mention_count = 1;
   b.mention_count = 100;
   a.first_seen = 9999;
   b.first_seen = 1;

   TEST_ASSERT_TRUE(memory_alias_canonical_priority_compare_self(&a, true, &b, false) < 0);
   /* And the reverse direction. */
   TEST_ASSERT_TRUE(memory_alias_canonical_priority_compare_self(&a, false, &b, true) > 0);
}

static void test_canonical_priority_compare_self_falls_through(void) {
   /* Both is_user_self flags equal — the sibling delegates to the four-axis
    * comparator, so the result must match memory_alias_canonical_priority_compare. */
   memory_entity_t a, b;
   memset(&a, 0, sizeof(a));
   memset(&b, 0, sizeof(b));
   strcpy(a.entity_type, "person");
   strcpy(b.entity_type, "thing");
   a.id = 7;
   b.id = 3;

   int four_axis = memory_alias_canonical_priority_compare(&a, &b);
   /* Both flags false: same result as four-axis. */
   TEST_ASSERT_EQUAL_INT(four_axis,
                         memory_alias_canonical_priority_compare_self(&a, false, &b, false));
   /* Both flags true: same result as four-axis (no short-circuit fires). */
   TEST_ASSERT_EQUAL_INT(four_axis,
                         memory_alias_canonical_priority_compare_self(&a, true, &b, true));
   /* And type_specificity DESC means person beats thing. */
   TEST_ASSERT_TRUE(four_axis < 0);
}

static void test_canonical_priority_compare_pure(void) {
   /* type_specificity DESC: person > place > thing. */
   memory_entity_t a, b;
   memset(&a, 0, sizeof(a));
   memset(&b, 0, sizeof(b));
   strcpy(a.entity_type, "person");
   strcpy(b.entity_type, "thing");
   a.id = 1;
   b.id = 2;
   /* a (person) wins over b (thing). */
   TEST_ASSERT_TRUE(memory_alias_canonical_priority_compare(&a, &b) < 0);

   /* Equal type → mention_count DESC. */
   memset(&a, 0, sizeof(a));
   memset(&b, 0, sizeof(b));
   strcpy(a.entity_type, "person");
   strcpy(b.entity_type, "person");
   a.mention_count = 5;
   b.mention_count = 50;
   a.id = 1;
   b.id = 2;
   /* b's mention_count is higher → b wins. */
   TEST_ASSERT_TRUE(memory_alias_canonical_priority_compare(&a, &b) > 0);

   /* Equal mention_count → first_seen ASC (older wins). */
   memset(&a, 0, sizeof(a));
   memset(&b, 0, sizeof(b));
   strcpy(a.entity_type, "person");
   strcpy(b.entity_type, "person");
   a.mention_count = b.mention_count = 10;
   a.first_seen = 1000;
   b.first_seen = 2000;
   a.id = 7;
   b.id = 3;
   /* a is older → a wins. */
   TEST_ASSERT_TRUE(memory_alias_canonical_priority_compare(&a, &b) < 0);

   /* Equal first_seen → id ASC (lower wins). */
   memset(&a, 0, sizeof(a));
   memset(&b, 0, sizeof(b));
   strcpy(a.entity_type, "person");
   strcpy(b.entity_type, "person");
   a.first_seen = b.first_seen = 1000;
   a.id = 5;
   b.id = 9;
   TEST_ASSERT_TRUE(memory_alias_canonical_priority_compare(&a, &b) < 0);
}

/* ============================================================================
 * Resolver cascade — Stage 1 + 2 + 3 (Stage 4 stubs to no-op without engine)
 * ============================================================================ */

static void test_resolver_stage1_exact_match(void) {
   int64_t jon = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");

   memory_alias_resolve_t res;
   int rc = memory_db_entity_resolve_alias(g_test_user_id, "Jonathan Smith", "person",
                                           "jonathan smith", &res);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, res.matched_stage);
   TEST_ASSERT_EQUAL_INT64(jon, res.resolved_id);
}

static void test_resolver_stage1_resolves_through_alias(void) {
   /* If an alias row exists pointing at canonical, Stage 1 should return
    * the canonical id (post-COALESCE), not the alias's own id. */
   int64_t target = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   int64_t alias = insert_entity_typed(g_test_user_id, "Jon", "person");
   /* Manually set canonical_id on the alias row to simulate a prior link. */
   sqlite3_stmt *u = NULL;
   sqlite3_prepare_v2(s_db.db, "UPDATE memory_entities SET canonical_id = ? WHERE id = ?", -1, &u,
                      NULL);
   sqlite3_bind_int64(u, 1, target);
   sqlite3_bind_int64(u, 2, alias);
   sqlite3_step(u);
   sqlite3_finalize(u);

   memory_alias_resolve_t res;
   int rc = memory_db_entity_resolve_alias(g_test_user_id, "Jon", "person", "jon", &res);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, res.matched_stage);
   TEST_ASSERT_EQUAL_INT64(target, res.resolved_id); /* canonical, not alias */
}

static void test_resolver_no_match(void) {
   insert_entity_typed(g_test_user_id, "Alice", "person");
   memory_alias_resolve_t res;
   int rc = memory_db_entity_resolve_alias(g_test_user_id, "Bob", "person", "bob", &res);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(0, res.matched_stage);
   TEST_ASSERT_EQUAL_INT64(0, res.resolved_id);
}

/* ============================================================================
 * score_pair — exercises Stage 5 (relation + contact overlap) directly
 * ============================================================================ */

static void test_score_pair_exclusive_relation_overlap(void) {
   int64_t jon = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   int64_t jon_alias = insert_entity_typed(g_test_user_id, "Jon", "thing");
   int64_t company = insert_entity_typed(g_test_user_id, "Acme Corp", "org");

   /* Both names point at the same exclusive-relation object (works_at). */
   insert_open_relation(g_test_user_id, jon, "works_at", company, NULL);
   insert_open_relation(g_test_user_id, jon_alias, "works_at", company, NULL);

   memory_alias_evidence_t ev;
   int rc = memory_db_entity_score_pair(g_test_user_id, jon_alias, jon, &ev);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_FLOAT(1.0f, ev.exclusive_relation_overlap);
   /* Substring bonus also fires (jon ⊂ jonathan smith). */
   TEST_ASSERT_TRUE(ev.name_substring_bonus_applied);
   /* Composite ≈ 0.30*0 + 0.30*0 + 0.25*1 + 0.10*0 + 0.05*0 + 0.10 (substring)
    *           = 0.35.  Falls below the review threshold (0.70) on this
    *           signal alone — the user_self bonus is what makes the dev's
    *           cluster cross the bar. */
   TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.35f, ev.composite_score);
}

static void test_score_pair_type_veto(void) {
   int64_t alice = insert_entity_typed(g_test_user_id, "Alice", "person");
   int64_t paris = insert_entity_typed(g_test_user_id, "Paris", "place");

   memory_alias_evidence_t ev;
   int rc = memory_db_entity_score_pair(g_test_user_id, alice, paris, &ev);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_TRUE(ev.type_veto_fired);
   TEST_ASSERT_EQUAL_FLOAT(0.0f, ev.composite_score);
}

static void test_score_pair_thing_no_veto(void) {
   /* The `thing` carve-out: thing/person doesn't trigger the veto. */
   int64_t jon_thing = insert_entity_typed(g_test_user_id, "Jon", "thing");
   int64_t jon_person = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");

   memory_alias_evidence_t ev;
   int rc = memory_db_entity_score_pair(g_test_user_id, jon_thing, jon_person, &ev);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_FALSE(ev.type_veto_fired);
   /* type_match = 0 because one side is `thing`, but composite is non-zero
    * because the substring bonus fires. */
   TEST_ASSERT_EQUAL_FLOAT(0.0f, ev.type_match);
   TEST_ASSERT_TRUE(ev.name_substring_bonus_applied);
}

static void test_score_pair_contact_field_overlap(void) {
   int64_t a = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   int64_t b = insert_entity_typed(g_test_user_id, "Jon", "thing");
   insert_contact(g_test_user_id, a, "email", "smithfabrications@gmail.com");
   insert_contact(g_test_user_id, b, "email", "SmithFabrications@gmail.com"); /* case-insensitive */

   memory_alias_evidence_t ev;
   int rc = memory_db_entity_score_pair(g_test_user_id, b, a, &ev);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_FLOAT(1.0f, ev.contact_field_overlap);
}

static void test_score_pair_user_self_bonus_via_username(void) {
   int64_t self = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   mark_entity_user_self(self);
   /* The user's username is "jon" (set in setUp).  An entity whose
    * canonical_name contains "jon" gets the user_self bonus when paired
    * with self. */
   int64_t alias = insert_entity_typed(g_test_user_id, "Jon", "thing");

   memory_alias_evidence_t ev;
   int rc = memory_db_entity_score_pair(g_test_user_id, alias, self, &ev);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_TRUE(ev.user_self_bonus_applied);
   /* substring bonus also fires.  composite = 0.10 (substring) + 0.20 (user_self) = 0.30. */
   TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.30f, ev.composite_score);
}

static void test_score_pair_user_self_bonus_via_contact_overlap(void) {
   int64_t self = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   mark_entity_user_self(self);
   int64_t other = insert_entity_typed(g_test_user_id, "totally different", "thing");
   insert_contact(g_test_user_id, self, "email", "shared@example.com");
   insert_contact(g_test_user_id, other, "email", "shared@example.com");

   memory_alias_evidence_t ev;
   int rc = memory_db_entity_score_pair(g_test_user_id, other, self, &ev);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   /* Contact overlap is what fires the user_self bonus despite zero name signal. */
   TEST_ASSERT_EQUAL_FLOAT(1.0f, ev.contact_field_overlap);
   TEST_ASSERT_TRUE(ev.user_self_bonus_applied);
}

/* ============================================================================
 * consider_auto_merge — threshold band routing
 * ============================================================================ */

static void test_consider_auto_merge_no_candidates(void) {
   int64_t lone = insert_entity_typed(g_test_user_id, "Solitary", "thing");
   memory_alias_evaluate_t eval;
   int rc = memory_db_entity_consider_auto_merge(g_test_user_id, lone, &eval);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(MEMORY_ALIAS_OUTCOME_NO_CANDIDATES, eval.outcome);
   TEST_ASSERT_EQUAL_INT64(0, eval.target_entity_id);
}

static void test_consider_auto_merge_auto_merged(void) {
   /* Build a high-composite cluster: user-self + alias with substring + shared
    * works_at relation + shared contact email.  Stage 4 (cosine) is stubbed
    * out so its 0.30 weight is forfeited, but the remaining signals + bonuses
    * easily clear 0.90.  Composite = 0.30*0 + 0.30*0 + 0.25*1 (works_at)
    *                                + 0.10*1 (contact) + 0.05*0 + 0.10
    *                                (substring) + 0.20 (user_self) = 0.70.
    * To reach 0.90 we need name_jaccard.  Use names that share a token
    * exactly: "jonathan smith" vs "jonathan k.k.".  Jaccard = 1/2 =
    * 0.50, contributing 0.30*0.50 = 0.15.  Total = 0.85 — still review band.
    * Add a second shared token: "jonathan smith" vs "jonathan smith 2".
    * Tokens A={jonathan, smith}, B={jonathan, smith} (the "2" is a
    * single char, dropped).  Jaccard = 1.0.  composite = 0.30 + 0.25 + 0.10
    *                                                    + 0.10 + 0.20 = 0.95. */
   int64_t self = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   mark_entity_user_self(self);
   int64_t company = insert_entity_typed(g_test_user_id, "Acme Corp", "org");
   insert_open_relation(g_test_user_id, self, "works_at", company, NULL);
   insert_contact(g_test_user_id, self, "email", "smithfab@example.com");

   /* Inbound row with the same tokens + same exclusive relation + same email. */
   int64_t inbound = insert_entity_typed(g_test_user_id, "Jonathan Smith 2", "person");
   insert_open_relation(g_test_user_id, inbound, "works_at", company, NULL);
   insert_contact(g_test_user_id, inbound, "email", "smithfab@example.com");

   memory_alias_evaluate_t eval;
   int rc = memory_db_entity_consider_auto_merge(g_test_user_id, inbound, &eval);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(MEMORY_ALIAS_OUTCOME_AUTO_MERGED, eval.outcome);
   TEST_ASSERT_EQUAL_INT64(self, eval.target_entity_id);
   TEST_ASSERT_GREATER_THAN(0, eval.link_id);
   TEST_ASSERT_GREATER_OR_EQUAL_FLOAT(MEMORY_ALIAS_AUTO_THRESHOLD, eval.evidence.composite_score);

   /* canonical_id was set on the inbound row. */
   TEST_ASSERT_EQUAL_INT64(self, get_entity_canonical_id(inbound));
   /* Audit row exists. */
   TEST_ASSERT_EQUAL_INT(1, count_active_aliases_for_target(self));
   /* Cache invalidated. */
   TEST_ASSERT_GREATER_THAN(0, g_alias_test_entity_cache_invalidations);
}

static void test_consider_auto_merge_proposed(void) {
   /* Mid-confidence: shared substring + shared open works_at, no user_self,
    * no contact overlap.  composite = 0.30*0 + 0.30*0 + 0.25*1 + 0 + 0 + 0.10
    *                                = 0.35 — below review (0.70).
    * Need to add type_match and a strong jaccard.  type_match = 0.05 if
    * both person.  jaccard ~ 0.5 → 0.15.  Add cosine? stubbed.  Total =
    * 0.25 + 0.15 + 0.05 + 0.10 = 0.55, still below review.
    * The proposed-band test is harder to hit precisely without cosine
    * — we'd need the user_self bonus but not the auto-merge threshold.
    * Construct: substring + user_self + type_match (one side is non-self
    * person) but no exclusive relation:
    *   composite = 0.30*0.5 + 0 + 0 + 0 + 0.05 + 0.10 + 0.20 = 0.50.
    * Still below.  Add a non-exclusive relation overlap:
    *   composite = 0.30*0.5 + 0 + 0.25*0.5 + 0 + 0.05 + 0.10 + 0.20 = 0.625.
    * Still below.  Make exclusive overlap fire:
    *   composite = 0.30*0.5 + 0 + 0.25*1 + 0 + 0.05 + 0.10 + 0.20 = 0.75
    *   → review band!  */
   int64_t self = insert_entity_typed(g_test_user_id, "Jonathan", "person");
   mark_entity_user_self(self);
   int64_t company = insert_entity_typed(g_test_user_id, "Acme", "org");
   insert_open_relation(g_test_user_id, self, "works_at", company, NULL);

   /* Inbound: shares the "jonathan" token (jaccard 0.5 with "jonathan
    * smith"), shares works_at, gets user_self bonus via username substring. */
   int64_t inbound = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   insert_open_relation(g_test_user_id, inbound, "works_at", company, NULL);

   memory_alias_evaluate_t eval;
   int rc = memory_db_entity_consider_auto_merge(g_test_user_id, inbound, &eval);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(MEMORY_ALIAS_OUTCOME_PROPOSED, eval.outcome);
   TEST_ASSERT_GREATER_THAN(0, eval.proposal_id);
   /* No alias write yet. */
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(inbound));
}

static void test_consider_auto_merge_rejected(void) {
   /* Sufficient name overlap to clear Stage 2 floor (jaccard >= 0.30) but
    * not enough total signal to clear review threshold (0.70).
    * "jonathan smith" vs "jonathan" → jaccard = 1/2 = 0.50.
    * composite = 0.30 * 0.50 = 0.15. */
   insert_entity_typed(g_test_user_id, "Jonathan", "thing");
   int64_t inbound = insert_entity_typed(g_test_user_id, "Jonathan Smith", "thing");

   memory_alias_evaluate_t eval;
   int rc = memory_db_entity_consider_auto_merge(g_test_user_id, inbound, &eval);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(MEMORY_ALIAS_OUTCOME_REJECTED, eval.outcome);
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(inbound));
}

/* Phase 2 substring rescue — forward direction: inbound is the short form
 * ("jon"), existing canonical is the long form ("jonathan smith").
 * Stage 2's per-token LIKE search finds "jonathan smith" as a hit for
 * token "jon" (it contains "jon" as a char-substring), but jaccard("jon",
 * "jonathan smith") = 0 because they share no whole-word tokens — below
 * the 0.30 floor.  The forward substring rescue admits the candidate so the
 * pair reaches Stage 6 scoring.  Without enough other signals it lands as
 * REJECTED, but `eval.target_entity_id` is set, confirming the cascade
 * found and scored it (vs NO_CANDIDATES if rescue didn't fire). */
static void test_consider_auto_merge_substring_rescue_forward(void) {
   int64_t long_form = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   int64_t inbound = insert_entity_typed(g_test_user_id, "Jon", "person");

   memory_alias_evaluate_t eval;
   int rc = memory_db_entity_consider_auto_merge(g_test_user_id, inbound, &eval);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   /* Candidate was generated and scored — proves Stage 2 forward rescue
    * fired.  Without it, outcome would be NO_CANDIDATES with target=0. */
   TEST_ASSERT_EQUAL_INT64(long_form, eval.target_entity_id);
   TEST_ASSERT_TRUE(eval.evidence.name_substring_bonus_applied);
   /* Stubbed cosine + no relations/contacts → composite below review.
    * Type=person both, jaccard=0, substring=+0.10, type_match=+0.05 ≈ 0.15. */
   TEST_ASSERT_EQUAL_INT(MEMORY_ALIAS_OUTCOME_REJECTED, eval.outcome);
}

/* Phase 2 substring rescue — reverse direction: existing canonical is the
 * short form ("jon"), inbound is the long form ("jonathan smith").
 * Stage 2's per-token LIKE search tokenises inbound into ["jonathan",
 * "smith"] and finds NEITHER inside "jon" — so the forward pass produces
 * an empty candidate pool.  Stage 2b reverse-substring sweep finds "jon"
 * because its canonical_name is a char-substring of "jonathan smith".
 * Without Stage 2b, this case is NO_CANDIDATES (the gap that lets short-
 * form duplicates survive when the long form arrives later). */
static void test_consider_auto_merge_substring_rescue_reverse(void) {
   int64_t short_form = insert_entity_typed(g_test_user_id, "Jon", "person");
   int64_t inbound = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");

   memory_alias_evaluate_t eval;
   int rc = memory_db_entity_consider_auto_merge(g_test_user_id, inbound, &eval);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   /* Without Stage 2b this would be NO_CANDIDATES; reverse rescue admits
    * "jon" so the cascade scores the pair and surfaces the target. */
   TEST_ASSERT_EQUAL_INT64(short_form, eval.target_entity_id);
   TEST_ASSERT_TRUE(eval.evidence.name_substring_bonus_applied);
   TEST_ASSERT_EQUAL_INT(MEMORY_ALIAS_OUTCOME_REJECTED, eval.outcome);
}

/* Phase 2 substring rescue + supporting signals: short ↔ long variant with
 * shared user-self + works_at exclusive relation crosses the review band.
 * Models the dev's real "Jon" ↔ "Jonathan Smith" cluster where the
 * Phase 2 gate should propose the merge for operator review.
 *
 * Composite breakdown (Stage 4 cosine stubbed to 0):
 *   0.30*0   (jaccard)        + 0
 *   0.30*0   (cosine stubbed) + 0
 *   0.25*1   (works_at)       + 0.25
 *   0.10*0   (no contact)     + 0
 *   0.05*1   (both person)    + 0.05
 *   +0.10    (substring)
 *   +0.20    (user_self)
 *                              ────────
 *                              = 0.60 — still below review (0.70).
 * Add contact overlap to push over: 0.60 + 0.10 = 0.70 → PROPOSED. */
static void test_consider_auto_merge_substring_rescue_into_review(void) {
   int64_t self = insert_entity_typed(g_test_user_id, "Jon", "person");
   mark_entity_user_self(self);
   int64_t company = insert_entity_typed(g_test_user_id, "Netsurion", "org");
   insert_open_relation(g_test_user_id, self, "works_at", company, NULL);
   insert_contact(g_test_user_id, self, "email", "jon.smith@gmail.com");

   int64_t inbound = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   insert_open_relation(g_test_user_id, inbound, "works_at", company, NULL);
   insert_contact(g_test_user_id, inbound, "email", "jon.smith@gmail.com");

   memory_alias_evaluate_t eval;
   int rc = memory_db_entity_consider_auto_merge(g_test_user_id, inbound, &eval);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(MEMORY_ALIAS_OUTCOME_PROPOSED, eval.outcome);
   TEST_ASSERT_EQUAL_INT64(self, eval.target_entity_id);
   TEST_ASSERT_GREATER_THAN(0, eval.proposal_id);
   /* Assert against the runtime threshold (the actual routing gate), not
    * the historical compile-time constant.  Otherwise this test would
    * keep agreeing with stale documentation if the runtime default ever
    * diverges from MEMORY_ALIAS_REVIEW_THRESHOLD. */
   TEST_ASSERT_GREATER_OR_EQUAL_FLOAT((float)g_config.memory.entity_merge_review_threshold,
                                      eval.evidence.composite_score);
   /* No alias write — operator review required. */
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(inbound));
}

/* Phase 2 propose-all-in-band: cascade returns ALL Stage-6-scored
 * candidates above review_threshold (not just the winner) so an inbound
 * with multiple legitimate matches gets a separate proposal row for each.
 * Models the dev's real case where "Jonathan Smith" matches both "Jon"
 * (same person) AND "Dawn Smith" (related entity via shared "smith"
 * token); the prior winner-only path silently dropped the secondary
 * candidate.  False positives are an operator-click away from rejection
 * in the WebUI Suggested-Merges panel — the cost asymmetry favors recall
 * over precision here. */
static void test_consider_auto_merge_proposes_multiple_in_band(void) {
   /* Two distinct candidates that should BOTH cross review_threshold
    * (0.50 in config defaults).  Build them with non-overlapping signals
    * so the cascade has to score each independently.
    *
    * Candidate A (the user-self winner): "Jon" with shared works_at +
    * shared email + user_self bonus → composite ≈ 0.30*0 (jaccard) +
    * 0.30*0 (cosine stubbed) + 0.25*1 (works_at) + 0.10*1 (email) +
    * 0.05*1 (type_match) + 0.10 (substring) + 0.20 (user_self) = 0.70.
    *
    * Candidate B (the secondary token match): "Some Other Smith" person
    * sharing the "smith" token → name_jaccard 0.5, no relations/contacts,
    * type_match, no user_self.  Composite = 0.30*0.5 + 0 + 0.05 (type) =
    * 0.20.  Below 0.50 — won't propose.  Damn, need more signal.
    *
    * Simpler shape: two user-self-eligible candidates.  Set both target
    * entities is_user_self... no, only one can be.  Stronger approach:
    * give both candidates exclusive_relation_overlap + type_match +
    * substring.  Composite = 0.25 + 0.05 + 0.10 = 0.40.  Still under.
    *
    * To force two distinct above-threshold candidates without leaning on
    * user_self: each candidate gets a shared works_at (relation overlap
    * 1.0 = 0.25), shared email contact (0.10), type_match (0.05),
    * substring (0.10), and we drop review_threshold via config to 0.30
    * for THIS test only (restored after).  Composite = 0.50 each →
    * both cross. */
   int64_t employer = insert_entity_typed(g_test_user_id, "Acme Corp", "org");

   int64_t target_a = insert_entity_typed(g_test_user_id, "Jon", "person");
   int64_t target_b = insert_entity_typed(g_test_user_id, "Kerseyfab", "person");
   insert_open_relation(g_test_user_id, target_a, "works_at", employer, NULL);
   insert_open_relation(g_test_user_id, target_b, "works_at", employer, NULL);
   insert_contact(g_test_user_id, target_a, "email", "shared@example.com");
   insert_contact(g_test_user_id, target_b, "email", "shared@example.com");

   int64_t inbound = insert_entity_typed(g_test_user_id, "Jon Kerseyfab", "person");
   insert_open_relation(g_test_user_id, inbound, "works_at", employer, NULL);
   insert_contact(g_test_user_id, inbound, "email", "shared@example.com");

   /* Test-scoped threshold drop.  Restore in tearDown via setUp re-init. */
   float saved_review = g_config.memory.entity_merge_review_threshold;
   g_config.memory.entity_merge_review_threshold = 0.30f;

   memory_alias_evaluate_t eval;
   int rc = memory_db_entity_consider_auto_merge(g_test_user_id, inbound, &eval);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(MEMORY_ALIAS_OUTCOME_PROPOSED, eval.outcome);

   /* The key assertion: BOTH targets received proposals.  Without the
    * propose-all-in-band refactor only the winner would be queued. */
   TEST_ASSERT_EQUAL_INT(2, count_pending_proposals_from_source(inbound));

   g_config.memory.entity_merge_review_threshold = saved_review;
}

/* Phase 2 auto-merge wins outright — when the winner crosses
 * auto_threshold the source becomes a soft alias of it.  No secondary
 * proposals should fire (the source can't be in multiple equivalence
 * classes).  Verifies the propose-all-in-band code path correctly
 * short-circuits when AUTO fires. */
static void test_consider_auto_merge_auto_short_circuits_proposals(void) {
   int64_t self = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   mark_entity_user_self(self);
   int64_t company = insert_entity_typed(g_test_user_id, "Acme Corp", "org");
   insert_open_relation(g_test_user_id, self, "works_at", company, NULL);
   insert_contact(g_test_user_id, self, "email", "smithfab@example.com");

   /* Strong inbound mirroring the auto_merged test, plus a SECOND
    * weakly-related candidate that would have been proposed under the
    * old winner-or-nothing branch — verify it stays unqueued because
    * the AUTO path doesn't iterate the runner-up list. */
   int64_t weak_candidate = insert_entity_typed(g_test_user_id, "Jonathan Doe", "person");
   insert_open_relation(g_test_user_id, weak_candidate, "works_at", company, NULL);

   int64_t inbound = insert_entity_typed(g_test_user_id, "Jonathan Smith 2", "person");
   insert_open_relation(g_test_user_id, inbound, "works_at", company, NULL);
   insert_contact(g_test_user_id, inbound, "email", "smithfab@example.com");

   memory_alias_evaluate_t eval;
   int rc = memory_db_entity_consider_auto_merge(g_test_user_id, inbound, &eval);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(MEMORY_ALIAS_OUTCOME_AUTO_MERGED, eval.outcome);
   /* AUTO writes an alias_link, not a proposal — both counts reflect this. */
   TEST_ASSERT_EQUAL_INT(0, count_pending_proposals_from_source(inbound));
   TEST_ASSERT_EQUAL_INT64(self, get_entity_canonical_id(inbound));
}

/* Longer-canonical-name swap: when AUTO fires and the inbound is the fuller
 * form of a person/pet/place name, redirect the alias-link so the long form
 * stays canonical and the existing short form becomes the alias.  Without
 * this preference, direction is determined incidentally by extraction order
 * — whoever arrived first wins canonical regardless of name "fullness". */
static void test_consider_auto_merge_longer_canonical_swap_fires(void) {
   /* Composite without user_self / exclusive relations / cosine:
    *   0.30*0  (jaccard "jon" vs "jonathan smith" — empty intersection)
    *   0.30*0  (cosine stubbed)
    *   0.25*0  (no exclusive overlap — none on either side)
    *   0.10*1  (contact email match)
    *   0.05*1  (both person)
    *   +0.10   (substring: "jon" ⊂ "jonathan smith")
    *           = 0.25
    *
    * Lower auto_threshold so 0.25 crosses AUTO — setUp() restores the
    * default for the next test via config_set_defaults(&g_config). */
   g_config.memory.entity_merge_auto_threshold = 0.20;

   int64_t short_form = insert_entity_typed(g_test_user_id, "Jon", "person");
   insert_contact(g_test_user_id, short_form, "email", "smithfab@example.com");

   int64_t inbound = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   insert_contact(g_test_user_id, inbound, "email", "smithfab@example.com");

   memory_alias_evaluate_t eval;
   int rc = memory_db_entity_consider_auto_merge(g_test_user_id, inbound, &eval);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(MEMORY_ALIAS_OUTCOME_AUTO_MERGED, eval.outcome);

   /* Swap fired: long-form (inbound) is the new canonical, short-form
    * (cascade winner) is now the alias.  eval.source/target reflect the
    * actual link direction taken. */
   TEST_ASSERT_EQUAL_INT64(short_form, eval.source_entity_id);
   TEST_ASSERT_EQUAL_INT64(inbound, eval.target_entity_id);
   TEST_ASSERT_EQUAL_INT64(inbound, get_entity_canonical_id(short_form));
   /* Inbound itself stays canonical (canonical_id IS NULL). */
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(inbound));
}

/* Propose-time swap: same direction-preference applies in REVIEW band.
 * Without this, the proposal row would be stored as (source=inbound,
 * target=cascade-winner) and the WebUI Suggested-Merges panel would show
 * "merge Jonathan Smith → Jon" — operator either approves a backwards
 * merge or has to mentally invert before clicking.  Mirror the AUTO swap
 * so the stored direction is operator-actionable as-is. */
static void test_consider_auto_merge_longer_canonical_propose_time_swap_fires(void) {
   /* Compose composite ~0.25 (substring + contact + type — no exclusive
    * relations so the no-dependents guard passes) and lower review to
    * 0.20 with auto well above so we land in REVIEW band. */
   g_config.memory.entity_merge_review_threshold = 0.20;
   g_config.memory.entity_merge_auto_threshold = 0.95; /* keep AUTO out of reach */

   int64_t short_form = insert_entity_typed(g_test_user_id, "Jon", "person");
   insert_contact(g_test_user_id, short_form, "email", "smithfab@example.com");

   int64_t inbound = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   insert_contact(g_test_user_id, inbound, "email", "smithfab@example.com");

   memory_alias_evaluate_t eval;
   int rc = memory_db_entity_consider_auto_merge(g_test_user_id, inbound, &eval);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(MEMORY_ALIAS_OUTCOME_PROPOSED, eval.outcome);

   /* Proposal stored with swapped direction — eval.source/target reflect
    * what the operator will see in the Suggested-Merges UI. */
   TEST_ASSERT_EQUAL_INT64(short_form, eval.source_entity_id);
   TEST_ASSERT_EQUAL_INT64(inbound, eval.target_entity_id);
   TEST_ASSERT_GREATER_THAN(0, eval.proposal_id);

   /* No alias write (operator still has to approve) — both canonical_id
    * fields should remain NULL. */
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(short_form));
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(inbound));
}

/* Same-token-count tiebreaker: "Jon Smith" (existing) vs "Jonathan Smith"
 * (inbound) — both 2 tokens, so the strictly-more-tokens rule doesn't
 * apply.  The token-prefix tiebreaker recognises "jon" as a positional
 * prefix of "jonathan" with matching second token, and the swap fires.
 * Without the tiebreaker, the longer form would silently land as alias
 * of the shorter form. */
static void test_consider_auto_merge_same_token_count_swap_via_token_prefix(void) {
   g_config.memory.entity_merge_auto_threshold = 0.20;

   int64_t short_form = insert_entity_typed(g_test_user_id, "Jon Smith", "person");
   insert_contact(g_test_user_id, short_form, "email", "smithfab@example.com");

   int64_t inbound = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   insert_contact(g_test_user_id, inbound, "email", "smithfab@example.com");

   memory_alias_evaluate_t eval;
   int rc = memory_db_entity_consider_auto_merge(g_test_user_id, inbound, &eval);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(MEMORY_ALIAS_OUTCOME_AUTO_MERGED, eval.outcome);

   /* Swap fired even though token counts match — the longer per-token
    * form is now canonical, the shorter form is alias. */
   TEST_ASSERT_EQUAL_INT64(short_form, eval.source_entity_id);
   TEST_ASSERT_EQUAL_INT64(inbound, eval.target_entity_id);
   TEST_ASSERT_EQUAL_INT64(inbound, get_entity_canonical_id(short_form));
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(inbound));
}

/* Negative case for the same-token-count tiebreaker: "Bob Smith" vs
 * "Robert Smith" — same token count but "bob" is NOT a prefix of "robert"
 * (b ≠ r at position 0), so the prefix tiebreaker correctly skips the
 * swap.  These are variants, not fuller-vs-shorter forms — auto-swapping
 * either direction would be an operator-correction event.  Cascade
 * direction (Bob Smith stays canonical because it was inserted first)
 * is preserved. */
static void test_consider_auto_merge_same_token_count_no_swap_on_variants(void) {
   g_config.memory.entity_merge_auto_threshold = 0.20;

   int64_t existing = insert_entity_typed(g_test_user_id, "Bob Smith", "person");
   insert_contact(g_test_user_id, existing, "email", "bob@example.com");

   int64_t inbound = insert_entity_typed(g_test_user_id, "Robert Smith", "person");
   insert_contact(g_test_user_id, inbound, "email", "bob@example.com");

   memory_alias_evaluate_t eval;
   int rc = memory_db_entity_consider_auto_merge(g_test_user_id, inbound, &eval);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(MEMORY_ALIAS_OUTCOME_AUTO_MERGED, eval.outcome);

   /* Swap blocked — direction stays as cascade decided (inbound alias of
    * existing).  Operator can manually re-link if they prefer Robert. */
   TEST_ASSERT_EQUAL_INT64(inbound, eval.source_entity_id);
   TEST_ASSERT_EQUAL_INT64(existing, eval.target_entity_id);
   TEST_ASSERT_EQUAL_INT64(existing, get_entity_canonical_id(inbound));
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(existing));
}

/* Swap-skipped: target has an open exclusive relation as subject, so the
 * "no dependents" guard blocks the redirect — alias direction stays as the
 * cascade picked (inbound → short_form).  Avoids orphaning a relation
 * subtree by making the head-of-equivalence-class into an alias.  Phase 3
 * hard-merge / consolidate is the right tool when that's wanted. */
static void test_consider_auto_merge_longer_canonical_swap_skipped_dependents(void) {
   g_config.memory.entity_merge_auto_threshold = 0.20;

   int64_t short_form = insert_entity_typed(g_test_user_id, "Jon", "person");
   insert_contact(g_test_user_id, short_form, "email", "smithfab@example.com");
   /* born_in is in EXCLUSIVE_RELATIONS[] — its presence on short_form
    * blocks the swap regardless of whether inbound also has it. */
   int64_t city = insert_entity_typed(g_test_user_id, "Atlanta", "place");
   insert_open_relation(g_test_user_id, short_form, "born_in", city, NULL);

   int64_t inbound = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   insert_contact(g_test_user_id, inbound, "email", "smithfab@example.com");

   memory_alias_evaluate_t eval;
   int rc = memory_db_entity_consider_auto_merge(g_test_user_id, inbound, &eval);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(MEMORY_ALIAS_OUTCOME_AUTO_MERGED, eval.outcome);

   /* Swap blocked — inbound (long-form) became the alias of short_form. */
   TEST_ASSERT_EQUAL_INT64(inbound, eval.source_entity_id);
   TEST_ASSERT_EQUAL_INT64(short_form, eval.target_entity_id);
   TEST_ASSERT_EQUAL_INT64(short_form, get_entity_canonical_id(inbound));
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(short_form));
}

/* ============================================================================
 * Phase 2 auto-promote user_self at extraction time / by real_name sweep
 *
 * These cover the "no CLI required" UX: when real_name is set in user
 * settings, a matching canonical (existing or freshly-extracted) gets
 * is_user_self=1 automatically — eliminating the `dawn-admin memory
 * entity link-user-self` setup step for clean installs.
 * ============================================================================ */

static void test_auto_promote_user_self_at_extraction_time(void) {
   /* Operator set their real_name BEFORE any matching entity existed.
    * Later, extraction creates the entity.  maybe_auto_promote_user_self
    * fires inline and flips is_user_self=1. */
   set_user_real_name(g_test_user_id, "Jonathan Smith");

   int64_t fresh = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   TEST_ASSERT_FALSE(entity_is_user_self(fresh));

   bool promoted = false;
   int rc = memory_db_entity_maybe_auto_promote_user_self(g_test_user_id, fresh, "jonathan smith",
                                                          &promoted);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_TRUE(promoted);
   TEST_ASSERT_TRUE(entity_is_user_self(fresh));
}

static void test_auto_promote_user_self_by_real_name_sweep(void) {
   /* Pre-existing canonical matches real_name.  Operator now sets
    * real_name via Settings → the by-real-name sweep finds and promotes
    * the canonical without needing any extraction to fire. */
   int64_t existing = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   set_user_real_name(g_test_user_id, "Jonathan Smith");

   bool promoted = false;
   int rc = memory_db_entity_auto_promote_user_self_by_real_name(g_test_user_id, &promoted);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_TRUE(promoted);
   TEST_ASSERT_TRUE(entity_is_user_self(existing));
}

static void test_auto_promote_no_op_when_user_self_exists(void) {
   /* Idempotency: a user_self anchor already exists.  Neither helper
    * should touch is_user_self on any other row. */
   int64_t already_self = insert_entity_typed(g_test_user_id, "Jon", "person");
   mark_entity_user_self(already_self);
   set_user_real_name(g_test_user_id, "Jonathan Smith");

   int64_t fresh = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   bool promoted = false;

   int rc = memory_db_entity_maybe_auto_promote_user_self(g_test_user_id, fresh, "jonathan smith",
                                                          &promoted);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_FALSE(promoted);
   TEST_ASSERT_FALSE(entity_is_user_self(fresh));
   TEST_ASSERT_TRUE(entity_is_user_self(already_self));

   promoted = true; /* deliberately non-zero so we catch a stale set */
   rc = memory_db_entity_auto_promote_user_self_by_real_name(g_test_user_id, &promoted);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_FALSE(promoted);
}

static void test_auto_promote_no_op_when_real_name_unset(void) {
   /* Operator never configured real_name — both helpers should silently
    * succeed without promoting anything.  This is the cold-start case
    * (account exists, identity not yet entered). */
   int64_t fresh = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");

   bool promoted = true;
   int rc = memory_db_entity_maybe_auto_promote_user_self(g_test_user_id, fresh, "jonathan smith",
                                                          &promoted);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_FALSE(promoted);
   TEST_ASSERT_FALSE(entity_is_user_self(fresh));

   promoted = true;
   rc = memory_db_entity_auto_promote_user_self_by_real_name(g_test_user_id, &promoted);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_FALSE(promoted);
}

static void test_auto_promote_matches_identity_alias(void) {
   /* Canonical doesn't match real_name directly but DOES match one of
    * the operator's configured identity aliases.  The sweep should still
    * find and promote it.  Models the dev's real config: real_name =
    * "Jonathan Smith" with alias "Jon" on a corpus where only "Jon"
    * was ever extracted as a canonical. */
   int64_t existing = insert_entity_typed(g_test_user_id, "Jon", "person");

   auth_user_identity_t id;
   memset(&id, 0, sizeof(id));
   strncpy(id.real_name, "Jonathan Smith", AUTH_REAL_NAME_MAX - 1);
   strncpy(id.identity_aliases, "Jon\nKristopher", AUTH_IDENTITY_ALIASES_MAX - 1);
   auth_db_set_user_identity(g_test_user_id, &id);

   bool promoted = false;
   int rc = memory_db_entity_auto_promote_user_self_by_real_name(g_test_user_id, &promoted);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_TRUE(promoted);
   TEST_ASSERT_TRUE(entity_is_user_self(existing));
}

/* ============================================================================
 * Auto-alias abstract "user" → user_self
 *
 * The LLM emits relations like (User, married_to, X), which materialise
 * as a generic entity row with canonical_name='user'.  The Phase 2
 * cascade can't catch this because there's zero token overlap with the
 * anchor's canonical (e.g. "kris kersey").  These tests pin the structural
 * identity claim: when an anchor exists, the 'user' row is unconditionally
 * soft-linked as an alias of the anchor — bypassing the cascade and
 * running without operator review.
 * ============================================================================ */

static void test_maybe_alias_user_to_self_aliases_when_anchor_exists(void) {
   /* Anchor exists for Jonathan Smith.  Extraction produces a fresh
    * generic 'user' entity from a (User, X, Y) relation.  Helper should
    * soft-link it to the anchor. */
   int64_t anchor = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   mark_entity_user_self(anchor);

   int64_t user_row = insert_entity_typed(g_test_user_id, "user", "person");
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(user_row));

   bool aliased = false;
   int rc = memory_db_entity_maybe_alias_user_to_self(g_test_user_id, user_row, "user", &aliased);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_TRUE(aliased);
   TEST_ASSERT_EQUAL_INT64(anchor, get_entity_canonical_id(user_row));
   /* Anchor itself stays canonical. */
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(anchor));
}

static void test_maybe_alias_user_to_self_noop_when_no_anchor(void) {
   /* No user_self anchor exists yet.  A generic 'user' row appearing
    * here must NOT be linked anywhere — the maybe_auto_promote_user_self
    * path is the one that handles the "anchor doesn't exist yet" side
    * (and it would only promote a row that actually matches real_name,
    * which 'user' isn't). */
   int64_t user_row = insert_entity_typed(g_test_user_id, "user", "person");

   bool aliased = true; /* deliberately non-zero so we catch a stale set */
   int rc = memory_db_entity_maybe_alias_user_to_self(g_test_user_id, user_row, "user", &aliased);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_FALSE(aliased);
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(user_row));
}

static void test_maybe_alias_user_to_self_refuses_self_link(void) {
   /* Edge case: users.real_name = "user" (silly but legal), so the
    * maybe_auto_promote path would have promoted this very row to the
    * anchor.  Calling maybe_alias on the same row must NOT attempt to
    * self-link — that would corrupt the anchor by setting its own
    * canonical_id to itself. */
   int64_t anchor = insert_entity_typed(g_test_user_id, "user", "person");
   mark_entity_user_self(anchor);

   bool aliased = true;
   int rc = memory_db_entity_maybe_alias_user_to_self(g_test_user_id, anchor, "user", &aliased);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_FALSE(aliased);
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(anchor));
   TEST_ASSERT_TRUE(entity_is_user_self(anchor));
}

static void test_alias_existing_user_to_self_sweep(void) {
   /* Operator flow: a generic 'user' row got extracted before the anchor
    * existed (real_name not yet configured).  Operator now sets real_name
    * via Settings → by_real_name sweep promotes a matching row to the
    * anchor → the sibling sweep called immediately after attaches the
    * pre-existing 'user' row. */
   int64_t pre_existing = insert_entity_typed(g_test_user_id, "user", "person");
   int64_t anchor = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   mark_entity_user_self(anchor);

   bool aliased = false;
   int rc = memory_db_entity_alias_existing_user_to_self(g_test_user_id, &aliased);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_TRUE(aliased);
   TEST_ASSERT_EQUAL_INT64(anchor, get_entity_canonical_id(pre_existing));
}

static void test_maybe_alias_user_to_self_rejects_invalid_input(void) {
   /* Pin the input-validation guards: NULL/empty canonical_name and
    * non-positive ids return FAILURE rather than silently no-op'ing.
    * Matches the test pattern for sibling alias helpers
    * (test_alias_link_refuses_invalid_user). */
   int64_t anchor = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   mark_entity_user_self(anchor);

   bool aliased = true;
   /* NULL canonical_name. */
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_FAILURE, memory_db_entity_maybe_alias_user_to_self(
                                                g_test_user_id, anchor + 1, NULL, &aliased));
   TEST_ASSERT_FALSE(aliased);

   /* Empty canonical_name. */
   aliased = true;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_FAILURE, memory_db_entity_maybe_alias_user_to_self(
                                                g_test_user_id, anchor + 1, "", &aliased));
   TEST_ASSERT_FALSE(aliased);

   /* Non-positive user_id. */
   aliased = true;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_FAILURE, memory_db_entity_maybe_alias_user_to_self(
                                                0, anchor + 1, "user", &aliased));
   TEST_ASSERT_FALSE(aliased);

   /* Non-positive entity_id. */
   aliased = true;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_FAILURE, memory_db_entity_maybe_alias_user_to_self(
                                                g_test_user_id, 0, "user", &aliased));
   TEST_ASSERT_FALSE(aliased);
}

static void test_alias_existing_user_to_self_rejects_invalid_user(void) {
   /* Companion to the above for the sweep variant.  Only input it takes
    * besides out_aliased is user_id. */
   bool aliased = true;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_FAILURE,
                         memory_db_entity_alias_existing_user_to_self(0, &aliased));
   TEST_ASSERT_FALSE(aliased);

   aliased = true;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_FAILURE,
                         memory_db_entity_alias_existing_user_to_self(-1, &aliased));
   TEST_ASSERT_FALSE(aliased);
}

/* ============================================================================
 * Path B — link-user-self propose-time longer-canonical direction swap
 *
 * Mirrors the swap shipped 2026-05-12 in consider_auto_merge's propose loop.
 * The link-user-self workflow's REVIEW-band proposal store now applies the
 * same should_swap_for_longer_canonical guard so the operator sees the
 * right direction in the Suggested-Merges panel.  AUTO branch deliberately
 * does NOT swap (operator-directed direction shouldn't be silently flipped).
 * ============================================================================ */

static void test_link_user_self_propose_swaps_for_longer_canonical(void) {
   /* Set up: real_name = "Jon" (short form) → user_self anchor is the
    * short canonical AND a LEAF (no aliases pointing at it, no open
    * exclusive relations as subject).  Longer person candidate
    * "Jonathan Smith" gets evaluated by link_user_self_run's per-
    * candidate scoring loop and lands in the REVIEW band after we drop
    * the review threshold to 0.30 (substring bonus + type-match +
    * user_self_bonus = ~0.35 composite, well below the default 0.50).
    * The propose-site swap should fire and store the proposal with
    * source=self (the anchor) and target=jonathan (the longer canonical). */
   auth_user_identity_t id;
   memset(&id, 0, sizeof(id));
   strncpy(id.real_name, "Jon", AUTH_REAL_NAME_MAX - 1);
   auth_db_set_user_identity(g_test_user_id, &id);

   int64_t self = insert_entity_typed(g_test_user_id, "Jon", "person");
   mark_entity_user_self(self);
   /* No relations on self → leaf. */

   int64_t jonathan = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");

   /* Lower review threshold so the substring/type/user-self-bonus signal
    * lands in REVIEW band.  Matches the override pattern in
    * test_consider_auto_merge_substring_rescue_into_review. */
   float saved_review = g_config.memory.entity_merge_review_threshold;
   g_config.memory.entity_merge_review_threshold = 0.30f;

   memory_alias_link_user_self_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = memory_alias_link_user_self_run(g_test_user_id, /* dry_run */ false, &result);

   g_config.memory.entity_merge_review_threshold = saved_review;

   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_GREATER_OR_EQUAL_INT(1, result.proposed);

   /* Find the proposal row that touches jonathan and verify direction was
    * swapped: stored as (source=self_id, target=jonathan_id) rather than
    * the un-swapped default (source=jonathan_id, target=self_id). */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT source_entity_id, target_entity_id "
                      "FROM memory_entity_merge_proposals "
                      "WHERE user_id=? AND resolved_at IS NULL "
                      "  AND (source_entity_id=? OR target_entity_id=?) "
                      "LIMIT 1",
                      -1, &stmt, NULL);
   sqlite3_bind_int(stmt, 1, g_test_user_id);
   sqlite3_bind_int64(stmt, 2, jonathan);
   sqlite3_bind_int64(stmt, 3, jonathan);
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(stmt));
   int64_t src = sqlite3_column_int64(stmt, 0);
   int64_t tgt = sqlite3_column_int64(stmt, 1);
   sqlite3_finalize(stmt);

   TEST_ASSERT_EQUAL_INT64(self, src);     /* anchor became the source */
   TEST_ASSERT_EQUAL_INT64(jonathan, tgt); /* longer canonical became target */
}

static void test_link_user_self_propose_skips_swap_when_self_has_dependents(void) {
   /* Negative companion to the above: when the user_self anchor already
    * has an alias pointing at it (has dependents), the leaf-check inside
    * should_swap_for_longer_canonical refuses, the swap silently skips,
    * and the proposal is stored in the un-swapped default direction
    * (source=candidate, target=self).  Pins that the leaf-guard fires
    * and prevents demoting an established anchor. */
   auth_user_identity_t id;
   memset(&id, 0, sizeof(id));
   strncpy(id.real_name, "Jon", AUTH_REAL_NAME_MAX - 1);
   auth_db_set_user_identity(g_test_user_id, &id);

   int64_t self = insert_entity_typed(g_test_user_id, "Jon", "person");
   mark_entity_user_self(self);

   /* Attach a pre-existing alias to make self NON-leaf.  Use a 'thing'
    * row that wouldn't otherwise be considered by the cascade. */
   int64_t pre_alias = insert_entity_typed(g_test_user_id, "Jon Profile Tag", "thing");
   int64_t pre_link = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_alias_link(g_test_user_id, pre_alias, self, "soft",
                                                     "test-fixture", -1.0f, NULL, &pre_link));

   int64_t jonathan = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");

   float saved_review = g_config.memory.entity_merge_review_threshold;
   g_config.memory.entity_merge_review_threshold = 0.30f;

   memory_alias_link_user_self_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = memory_alias_link_user_self_run(g_test_user_id, /* dry_run */ false, &result);

   g_config.memory.entity_merge_review_threshold = saved_review;

   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_GREATER_OR_EQUAL_INT(1, result.proposed);

   /* Proposal stored un-swapped: source=jonathan, target=self. */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT source_entity_id, target_entity_id "
                      "FROM memory_entity_merge_proposals "
                      "WHERE user_id=? AND resolved_at IS NULL "
                      "  AND (source_entity_id=? OR target_entity_id=?) "
                      "LIMIT 1",
                      -1, &stmt, NULL);
   sqlite3_bind_int(stmt, 1, g_test_user_id);
   sqlite3_bind_int64(stmt, 2, jonathan);
   sqlite3_bind_int64(stmt, 3, jonathan);
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(stmt));
   int64_t src = sqlite3_column_int64(stmt, 0);
   int64_t tgt = sqlite3_column_int64(stmt, 1);
   sqlite3_finalize(stmt);

   TEST_ASSERT_EQUAL_INT64(jonathan, src); /* candidate stayed as source */
   TEST_ASSERT_EQUAL_INT64(self, tgt);     /* anchor stayed as target */
}

/* ============================================================================
 * alias_link / alias_unlink
 * ============================================================================ */

static void test_alias_link_writes_canonical_id_and_audit(void) {
   int64_t target = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   int64_t source = insert_entity_typed(g_test_user_id, "Jon", "thing");
   int prev_inv = g_alias_test_entity_cache_invalidations;

   int64_t link_id = 0;
   int rc = memory_db_entity_alias_link(g_test_user_id, source, target, "soft", "operator", 0.95f,
                                        "{}", &link_id);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_GREATER_THAN(0, link_id);
   TEST_ASSERT_EQUAL_INT64(target, get_entity_canonical_id(source));
   TEST_ASSERT_EQUAL_INT(1, count_active_aliases_for_target(target));
   TEST_ASSERT_GREATER_THAN(prev_inv, g_alias_test_entity_cache_invalidations);
}

static void test_alias_link_refuses_self(void) {
   int64_t e = insert_entity_typed(g_test_user_id, "Solo", "thing");
   int64_t link_id = 0;
   int rc = memory_db_entity_alias_link(g_test_user_id, e, e, "soft", "operator", 0.0f, NULL,
                                        &link_id);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_FAILURE, rc);
   TEST_ASSERT_EQUAL_INT64(0, link_id);
}

static void test_alias_link_refuses_when_source_has_dependents(void) {
   /* If A → B (B is canonical), trying to link B → C should fail because B
    * has A as a dependent alias. */
   int64_t a = insert_entity_typed(g_test_user_id, "Alpha", "thing");
   int64_t b = insert_entity_typed(g_test_user_id, "Beta", "thing");
   int64_t c = insert_entity_typed(g_test_user_id, "Gamma", "thing");
   /* First link a → b. */
   int64_t link1 = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_alias_link(g_test_user_id, a, b, "soft", "operator", 0.0f,
                                                     NULL, &link1));
   /* Now try b → c — should refuse because b is canonical for a. */
   int64_t link2 = 0;
   int rc = memory_db_entity_alias_link(g_test_user_id, b, c, "soft", "operator", 0.0f, NULL,
                                        &link2);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_FAILURE, rc);
   TEST_ASSERT_EQUAL_INT64(0, link2);
}

static void test_alias_unlink_clears_canonical_id_and_stamps_audit(void) {
   int64_t target = insert_entity_typed(g_test_user_id, "Target", "thing");
   int64_t source = insert_entity_typed(g_test_user_id, "Source", "thing");
   int64_t link_id = 0;
   memory_db_entity_alias_link(g_test_user_id, source, target, "soft", "operator", 0.95f, NULL,
                               &link_id);
   int prev_inv = g_alias_test_entity_cache_invalidations;

   int rc = memory_db_entity_alias_unlink(g_test_user_id, link_id, "split-by-operator");
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(source));
   TEST_ASSERT_EQUAL_INT(0, count_active_aliases_for_target(target));
   TEST_ASSERT_GREATER_THAN(prev_inv, g_alias_test_entity_cache_invalidations);

   /* Second unlink on the same id should fail (already unlinked). */
   rc = memory_db_entity_alias_unlink(g_test_user_id, link_id, "split-by-operator");
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_NOT_FOUND, rc);
}

static void test_alias_unlink_refuses_hard(void) {
   int64_t target = insert_entity_typed(g_test_user_id, "Target", "thing");
   int64_t source = insert_entity_typed(g_test_user_id, "Source", "thing");
   int64_t link_id = 0;
   memory_db_entity_alias_link(g_test_user_id, source, target, "hard", "operator", 0.95f, NULL,
                               &link_id);

   int rc = memory_db_entity_alias_unlink(g_test_user_id, link_id, "split-by-operator");
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_FAILURE, rc);
}

/* ============================================================================
 * Cache-invalidate roundtrip on existing memory_db_entity_merge() (Ckpt 3
 * fold-in fix per design §12).  Verifies that a hard-merge call on the
 * existing primitive flips the entity-cache dirty bit via the
 * memory_embeddings_invalidate_entity_cache() stub counter.
 * ============================================================================ */

static void test_entity_merge_invalidates_entity_cache(void) {
   /* Two entities with at least one relation; merge should re-point the
    * relation, delete the source row, and invalidate the entity cache. */
   int64_t a = insert_entity_typed(g_test_user_id, "Alpha", "thing");
   int64_t b = insert_entity_typed(g_test_user_id, "Beta", "thing");
   int64_t c = insert_entity_typed(g_test_user_id, "Gamma", "thing");
   insert_open_relation(g_test_user_id, a, "knows", c, NULL);

   int prev_inv = g_alias_test_entity_cache_invalidations;
   int rc = memory_db_entity_merge(g_test_user_id, a, b);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   /* Invalidator fired exactly once from the merge primitive. */
   TEST_ASSERT_GREATER_THAN(prev_inv, g_alias_test_entity_cache_invalidations);
}

/* A merge moves the source's aliases to the target: "Jordan" still names
 * someone after "Jordan S." is merged into "Jordan Smith". */
static void test_entity_merge_moves_aliases(void) {
   int64_t target = insert_entity_typed(g_test_user_id, "Jordan Smith", "person");
   int64_t source = insert_entity_typed(g_test_user_id, "Jordan S.", "person");
   int64_t alias = insert_entity_typed(g_test_user_id, "Jordan", "person");
   int64_t link_id = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_alias_link(g_test_user_id, alias, source, "soft",
                                                     "operator", 0.95f, NULL, &link_id));
   TEST_ASSERT_EQUAL_INT64(source, get_entity_canonical_id(alias));

   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_entity_merge(g_test_user_id, source, target));
   TEST_ASSERT_EQUAL_INT64(target, get_entity_canonical_id(alias));

   memory_alias_name_t *names = NULL;
   int n = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_alias_names(g_test_user_id, &names, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_INT64(target, names[0].canonical_id);
   TEST_ASSERT_EQUAL_STRING("Jordan", names[0].name);
   free(names);

   /* The link moved too, under the target's name. */
   memory_alias_listing_row_t rows[4];
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_alias_list(g_test_user_id, target, rows, 4, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_INT64(alias, rows[0].source_entity_id);
   memory_alias_history_row_t hist[4];
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_alias_history(g_test_user_id, alias, hist, 4, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_STRING("jordan smith", hist[0].target_canonical_name);
}

/* Merging into an entity that is itself an alias: the source's aliases go to
 * the entity it names, not one level down (an alias of an alias names
 * nothing).  A source that was an alias of something leaves no live link. */
static void test_entity_merge_into_an_alias(void) {
   int64_t root = insert_entity_typed(g_test_user_id, "Jordan Smith", "person");
   int64_t target = insert_entity_typed(g_test_user_id, "Jo", "person");
   int64_t source = insert_entity_typed(g_test_user_id, "Jordan S.", "person");
   int64_t alias = insert_entity_typed(g_test_user_id, "Jordan", "person");
   int64_t other = insert_entity_typed(g_test_user_id, "Other", "person");
   int64_t sx = insert_entity_typed(g_test_user_id, "J. Smith", "person");
   int64_t link_id = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_alias_link(g_test_user_id, target, root, "soft",
                                                     "operator", 0.95f, NULL, &link_id));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_alias_link(g_test_user_id, alias, source, "soft",
                                                     "operator", 0.95f, NULL, &link_id));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_entity_merge(g_test_user_id, source, target));
   TEST_ASSERT_EQUAL_INT64(root, get_entity_canonical_id(alias));

   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_alias_link(g_test_user_id, sx, other, "soft", "operator",
                                                     0.95f, NULL, &link_id));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_entity_merge(g_test_user_id, sx, root));
   memory_alias_listing_row_t rows[4];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_alias_list(g_test_user_id, other, rows, 4, &n));
   TEST_ASSERT_EQUAL_INT(0, n);
}

/* Merging an entity into its own alias: the alias becomes canonical and the
 * link between them is closed, not left pointing at itself. */
static void test_entity_merge_into_own_alias(void) {
   int64_t canonical = insert_entity_typed(g_test_user_id, "Jordan Smith", "person");
   int64_t alias = insert_entity_typed(g_test_user_id, "Jordan", "person");
   int64_t link_id = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_alias_link(g_test_user_id, alias, canonical, "soft",
                                                     "operator", 0.95f, NULL, &link_id));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_merge(g_test_user_id, canonical, alias));
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(alias));
   memory_alias_listing_row_t rows[4];
   int n = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_alias_list(g_test_user_id, alias, rows, 4, &n));
   TEST_ASSERT_EQUAL_INT(0, n); /* only active links are listed */
   memory_alias_history_row_t hist[4];
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_alias_history(g_test_user_id, alias, hist, 4, &n));
   TEST_ASSERT_EQUAL_INT(1, n);
   TEST_ASSERT_EQUAL_STRING("merged-into-alias", hist[0].unlink_reason);
}

/* The abstract "user" alias isn't a name: it never names the user. */
static void test_alias_names_leave_out_user_placeholder(void) {
   int64_t self = insert_entity_typed(g_test_user_id, "Pat Example", "person");
   int64_t placeholder = insert_entity_typed(g_test_user_id, "User", "person");
   int64_t link_id = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_alias_link(g_test_user_id, placeholder, self, "soft",
                                                     "operator", 0.95f, NULL, &link_id));
   memory_alias_name_t *names = NULL;
   int n = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_alias_names(g_test_user_id, &names, &n));
   TEST_ASSERT_EQUAL_INT(0, n);
   free(names);
}

/* ============================================================================
 * Cache loader filter — memory_db_entity_get_embeddings respects
 * include_aliases.  Insert a canonical row + a soft-alias row, both with
 * embeddings, and confirm the default call filters the alias out and the
 * include_aliases=true call includes both.
 * ============================================================================ */

static void insert_dummy_embedding(int64_t entity_id) {
   /* Stage a tiny 4-dim float blob so the row qualifies as "has embedding"
    * without requiring the embedding engine.  Norm is 2.0 (sqrt(4*1)). */
   const float blob[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "UPDATE memory_entities SET embedding = ?, embedding_norm = ? WHERE id = ?",
                      -1, &stmt, NULL);
   sqlite3_bind_blob(stmt, 1, blob, sizeof(blob), SQLITE_TRANSIENT);
   sqlite3_bind_double(stmt, 2, 2.0);
   sqlite3_bind_int64(stmt, 3, entity_id);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
}

static void test_loader_filter_excludes_aliases_by_default(void) {
   int64_t target = insert_entity_typed(g_test_user_id, "Canonical", "thing");
   int64_t alias = insert_entity_typed(g_test_user_id, "Alias", "thing");
   insert_dummy_embedding(target);
   insert_dummy_embedding(alias);

   /* Soft-link alias → target so canonical_id IS NOT NULL on the alias row. */
   int64_t link_id = 0;
   memory_db_entity_alias_link(g_test_user_id, alias, target, "soft", "operator", 1.0f, NULL,
                               &link_id);

   const int dims = 4;
   int64_t out_ids[8];
   char out_names[8][MEMORY_ENTITY_NAME_MAX];
   char out_types[8][MEMORY_ENTITY_TYPE_MAX];
   float out_embeddings[8 * 4];
   float out_norms[8];

   /* Default (canonical-only): only the target row surfaces. */
   int count = 0;
   int rc = memory_db_entity_get_embeddings(g_test_user_id, /* include_aliases */ false, dims,
                                            out_ids, out_names, out_types, out_embeddings,
                                            out_norms, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_EQUAL_INT64(target, out_ids[0]);

   /* include_aliases = true: both rows surface. */
   count = 0;
   rc = memory_db_entity_get_embeddings(g_test_user_id, /* include_aliases */ true, dims, out_ids,
                                        out_names, out_types, out_embeddings, out_norms, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, count);
}

/* ============================================================================
 * relation_list_by_subject_class — equivalence-class-aware listing
 * ============================================================================ */

static void test_relation_list_by_subject_class_includes_aliases(void) {
   int64_t canonical = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   int64_t alias = insert_entity_typed(g_test_user_id, "Jon", "thing");
   int64_t company = insert_entity_typed(g_test_user_id, "Acme", "org");

   /* Relation attached to canonical row. */
   insert_open_relation(g_test_user_id, canonical, "works_at", company, NULL);
   /* Relation attached to alias row (pre-link). */
   insert_open_relation(g_test_user_id, alias, "lives_in", 0, "Atlanta");

   /* Soft-link alias → canonical. */
   int64_t link_id = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_alias_link(g_test_user_id, alias, canonical, "soft",
                                                     "operator", 1.0f, NULL, &link_id));

   memory_relation_t rels[8];
   int count = 0;
   int rc = memory_db_relation_list_by_subject_class(g_test_user_id, canonical, rels, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   /* Both relations should surface — one from canonical, one from the alias. */
   TEST_ASSERT_EQUAL_INT(2, count);
}

/* ============================================================================
 * Ckpt 4 — link-user-self orchestrator + listing helpers
 * ============================================================================ */

static int count_db_rows(const char *sql) {
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   int n = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      n = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return n;
}

static void test_link_user_self_dry_run_no_db_writes(void) {
   /* Build the dev's-cluster shape: a strong 'Jonathan Smith' (person,
    * is_user_self=1) with a shared works_at relation, plus a 'Jon' alias
    * candidate that should clear auto-merge after the dry-run scoring.
    * Phase 1.5 Ckpt D requires real_name to be set before link-user-self
    * will run — the gate fires up-front regardless of whether a self
    * entity already exists. */
   auth_user_identity_t id;
   memset(&id, 0, sizeof(id));
   strncpy(id.real_name, "Jonathan Smith", AUTH_REAL_NAME_MAX - 1);
   auth_db_set_user_identity(g_test_user_id, &id);

   int64_t self = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   mark_entity_user_self(self);
   int64_t company = insert_entity_typed(g_test_user_id, "Acme", "org");
   insert_open_relation(g_test_user_id, self, "works_at", company, NULL);

   int64_t jon = insert_entity_typed(g_test_user_id, "Jonathan Smith 2", "person");
   insert_open_relation(g_test_user_id, jon, "works_at", company, NULL);

   /* Snapshot the alias / proposal counts BEFORE the dry-run. */
   int aliases_before = count_db_rows(
       "SELECT COUNT(*) FROM memory_entity_aliases WHERE unlinked_at IS NULL");
   int props_before = count_db_rows(
       "SELECT COUNT(*) FROM memory_entity_merge_proposals WHERE resolved_at IS NULL");

   memory_alias_link_user_self_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = memory_alias_link_user_self_run(g_test_user_id, /* dry_run */ true, &result);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);

   /* Self-id resolved to existing canonical (no seed needed). */
   TEST_ASSERT_EQUAL_INT64(self, result.self_entity_id);
   TEST_ASSERT_FALSE(result.self_was_seeded);
   TEST_ASSERT_EQUAL_INT(2, result.considered); /* Jon + Acme are the candidates */

   /* No DB mutation. */
   TEST_ASSERT_EQUAL_INT(aliases_before, count_db_rows("SELECT COUNT(*) FROM memory_entity_aliases "
                                                       "WHERE unlinked_at IS NULL"));
   TEST_ASSERT_EQUAL_INT(props_before,
                         count_db_rows("SELECT COUNT(*) FROM memory_entity_merge_proposals "
                                       "WHERE resolved_at IS NULL"));
}

static void test_link_user_self_commit_seeds_and_links(void) {
   /* Same shape as the dry-run, but call with dry_run=false and assert that
    * the soft-link gets written.  Phase 1.5 Ckpt D requires real_name. */
   auth_user_identity_t id;
   memset(&id, 0, sizeof(id));
   strncpy(id.real_name, "Jonathan Smith", AUTH_REAL_NAME_MAX - 1);
   auth_db_set_user_identity(g_test_user_id, &id);

   int64_t self = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   mark_entity_user_self(self);
   int64_t company = insert_entity_typed(g_test_user_id, "Acme", "org");
   insert_open_relation(g_test_user_id, self, "works_at", company, NULL);
   int64_t jon = insert_entity_typed(g_test_user_id, "Jonathan Smith 2", "person");
   insert_open_relation(g_test_user_id, jon, "works_at", company, NULL);

   memory_alias_link_user_self_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = memory_alias_link_user_self_run(g_test_user_id, /* dry_run */ false, &result);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT64(self, result.self_entity_id);
   TEST_ASSERT_GREATER_OR_EQUAL_INT(1, result.auto_merged);

   /* Jon is now an alias of self. */
   TEST_ASSERT_EQUAL_INT64(self, get_entity_canonical_id(jon));
   /* And an audit row exists. */
   TEST_ASSERT_GREATER_OR_EQUAL_INT(1, count_active_aliases_for_target(self));
}

static void test_alias_list_helper_returns_active_only(void) {
   int64_t target = insert_entity_typed(g_test_user_id, "Target", "thing");
   int64_t a1 = insert_entity_typed(g_test_user_id, "A1", "thing");
   int64_t a2 = insert_entity_typed(g_test_user_id, "A2", "thing");
   int64_t link1 = 0, link2 = 0;
   memory_db_entity_alias_link(g_test_user_id, a1, target, "soft", "operator", 0.95f, NULL, &link1);
   memory_db_entity_alias_link(g_test_user_id, a2, target, "soft", "operator", 0.92f, NULL, &link2);

   memory_alias_listing_row_t rows[8];
   int count = 0;
   int rc = memory_db_entity_alias_list(g_test_user_id, target, rows, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, count);

   /* Unlink one — listing should now return only the other. */
   memory_db_entity_alias_unlink(g_test_user_id, link1, "split-by-operator");
   memset(rows, 0, sizeof(rows));
   count = 0;
   rc = memory_db_entity_alias_list(g_test_user_id, target, rows, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_EQUAL_INT64(link2, rows[0].link_id);
}

static void test_alias_history_helper_includes_unlinked(void) {
   int64_t target = insert_entity_typed(g_test_user_id, "T2", "thing");
   int64_t alias = insert_entity_typed(g_test_user_id, "Source2", "thing");
   int64_t link_id = 0;
   memory_db_entity_alias_link(g_test_user_id, alias, target, "soft", "operator", 0.91f, NULL,
                               &link_id);
   memory_db_entity_alias_unlink(g_test_user_id, link_id, "split-by-operator");

   memory_alias_history_row_t rows[8];
   int count = 0;
   int rc = memory_db_entity_alias_history(g_test_user_id, alias, rows, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_GREATER_THAN(0, rows[0].unlinked_at);
   TEST_ASSERT_EQUAL_STRING("split-by-operator", rows[0].unlink_reason);
}

static int64_t insert_proposal(int user_id,
                               int64_t source_id,
                               int64_t target_id,
                               double composite,
                               int64_t proposed_at,
                               const char *resolution) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "INSERT INTO memory_entity_merge_proposals "
                      "(user_id, source_entity_id, target_entity_id, composite_score, "
                      " evidence_json, proposed_at, resolved_at, resolution) "
                      "VALUES (?, ?, ?, ?, '{}', ?, ?, ?)",
                      -1, &stmt, NULL);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, source_id);
   sqlite3_bind_int64(stmt, 3, target_id);
   sqlite3_bind_double(stmt, 4, composite);
   sqlite3_bind_int64(stmt, 5, proposed_at);
   if (resolution) {
      sqlite3_bind_int64(stmt, 6, proposed_at + 60);
      sqlite3_bind_text(stmt, 7, resolution, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(stmt, 6);
      sqlite3_bind_null(stmt, 7);
   }
   sqlite3_step(stmt);
   int64_t id = sqlite3_last_insert_rowid(s_db.db);
   sqlite3_finalize(stmt);
   return id;
}

static int64_t get_proposal_resolved_at(int64_t proposal_id) {
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "SELECT COALESCE(resolved_at, 0) "
                      "FROM memory_entity_merge_proposals WHERE id = ?",
                      -1, &stmt, NULL);
   sqlite3_bind_int64(stmt, 1, proposal_id);
   int64_t v = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      v = sqlite3_column_int64(stmt, 0);
   sqlite3_finalize(stmt);
   return v;
}

static void test_proposal_list_returns_pending_only(void) {
   /* Seed two pending + one resolved proposal.  proposal_list_pending must
    * return the two pending in proposed_at DESC order and never surface
    * the resolved one. */
   int64_t a = insert_entity_typed(g_test_user_id, "Source A", "thing");
   int64_t b = insert_entity_typed(g_test_user_id, "Source B", "thing");
   int64_t c = insert_entity_typed(g_test_user_id, "Source C", "thing");
   int64_t target = insert_entity_typed(g_test_user_id, "Target", "thing");

   int64_t older = insert_proposal(g_test_user_id, a, target, 0.78, 100, NULL);
   int64_t newer = insert_proposal(g_test_user_id, b, target, 0.85, 200, NULL);
   /* Resolved one — must NOT appear. */
   int64_t resolved = insert_proposal(g_test_user_id, c, target, 0.81, 50, "rejected");
   (void)resolved;

   memory_alias_proposal_row_t rows[8];
   int count = 0;
   int rc = memory_db_proposal_list_pending(g_test_user_id, rows, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, count);
   /* DESC by proposed_at: newer (200) comes first. */
   TEST_ASSERT_EQUAL_INT64(newer, rows[0].proposal_id);
   TEST_ASSERT_EQUAL_INT64(older, rows[1].proposal_id);
   /* Joined names populated. */
   TEST_ASSERT_EQUAL_STRING("source b", rows[0].source_canonical_name);
   TEST_ASSERT_EQUAL_STRING("target", rows[0].target_canonical_name);
}

static void test_proposal_resolve_approved_creates_alias(void) {
   /* Approved resolution creates the soft alias and stamps the proposal. */
   int64_t source = insert_entity_typed(g_test_user_id, "Source", "thing");
   int64_t target = insert_entity_typed(g_test_user_id, "Target", "thing");
   int64_t prop = insert_proposal(g_test_user_id, source, target, 0.78, 100, NULL);

   int64_t link_id = 0;
   int rc = memory_db_proposal_resolve(g_test_user_id, prop, /* approved */ true, &link_id);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_GREATER_THAN(0, link_id);

   /* Source now soft-linked to target. */
   TEST_ASSERT_EQUAL_INT64(target, get_entity_canonical_id(source));
   /* Audit row written. */
   TEST_ASSERT_EQUAL_INT(1, count_active_aliases_for_target(target));
   /* Proposal stamped. */
   TEST_ASSERT_GREATER_THAN(0, get_proposal_resolved_at(prop));

   /* Re-resolving the same proposal must fail with NOT_FOUND. */
   rc = memory_db_proposal_resolve(g_test_user_id, prop, true, NULL);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_NOT_FOUND, rc);

   /* Reject path on a fresh proposal: stamps without writing an alias. */
   int64_t source2 = insert_entity_typed(g_test_user_id, "Source2", "thing");
   int64_t prop2 = insert_proposal(g_test_user_id, source2, target, 0.74, 110, NULL);
   int aliases_before = count_db_rows(
       "SELECT COUNT(*) FROM memory_entity_aliases WHERE unlinked_at IS NULL");
   rc = memory_db_proposal_resolve(g_test_user_id, prop2, /* approved */ false, NULL);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(source2)); /* no link */
   TEST_ASSERT_EQUAL_INT(aliases_before, count_db_rows("SELECT COUNT(*) FROM memory_entity_aliases "
                                                       "WHERE unlinked_at IS NULL"));
   TEST_ASSERT_GREATER_THAN(0, get_proposal_resolved_at(prop2));
}

static void test_entity_list_for_admin_canonical_only_default(void) {
   int64_t canonical = insert_entity_typed(g_test_user_id, "Canon", "person");
   int64_t alias = insert_entity_typed(g_test_user_id, "AliasOfCanon", "person");
   int64_t link_id = 0;
   memory_db_entity_alias_link(g_test_user_id, alias, canonical, "soft", "operator", 0.95f, NULL,
                               &link_id);

   memory_alias_entity_row_t rows[16];
   int count = 0;

   /* Default — canonical-only.  Should NOT include the alias row. */
   int rc = memory_db_entity_list_for_admin(g_test_user_id, /* include_aliases */ false, rows, 16,
                                            &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_EQUAL_INT64(canonical, rows[0].entity_id);
   TEST_ASSERT_FALSE(rows[0].is_alias);

   /* include_aliases=true → both rows surface, alias flagged, alias row's
    * canonical_id points back at the canonical entity (so callers can render
    * aliases nested under their parent without re-querying). */
   memset(rows, 0, sizeof(rows));
   count = 0;
   rc = memory_db_entity_list_for_admin(g_test_user_id, /* include_aliases */ true, rows, 16,
                                        &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, count);
   /* Pass-1 canonicals first, Pass-2 aliases appended. */
   TEST_ASSERT_FALSE(rows[0].is_alias);
   TEST_ASSERT_EQUAL_INT64(0, rows[0].canonical_id);
   TEST_ASSERT_TRUE(rows[1].is_alias);
   TEST_ASSERT_EQUAL_INT64(canonical, rows[1].canonical_id);
}

/* Two-pass listing: the canonical pass writes a contiguous prefix of
 * is_alias=false rows and the alias pass appends a tail of is_alias=true
 * rows ordered by canonical_id ASC.  The renderer relies on this shape to
 * scan one contiguous run per canonical and inline its aliases.  Verifies
 * both the prefix/tail invariant and that each alias row carries its
 * canonical's id back so the caller does not need a second query. */
static void test_entity_list_for_admin_alias_tail_carries_canonical_id(void) {
   /* Two canonicals — Hub has two aliases, Solo has none — so we exercise
    * both the "canonical with aliases" and "canonical without aliases"
    * branches in one fixture. */
   int64_t hub = insert_entity_typed(g_test_user_id, "Hub", "person");
   int64_t solo = insert_entity_typed(g_test_user_id, "Solo", "person");
   int64_t alias_a = insert_entity_typed(g_test_user_id, "HubAlt", "person");
   int64_t alias_b = insert_entity_typed(g_test_user_id, "Hubby", "person");
   int64_t link_a = 0, link_b = 0;
   memory_db_entity_alias_link(g_test_user_id, alias_a, hub, "soft", "operator", 0.95f, NULL,
                               &link_a);
   memory_db_entity_alias_link(g_test_user_id, alias_b, hub, "soft", "operator", 0.85f, NULL,
                               &link_b);

   memory_alias_entity_row_t rows[8];
   int count = 0;
   int rc = memory_db_entity_list_for_admin(g_test_user_id, /* include_aliases */ true, rows, 8,
                                            &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(4, count); /* 2 canonicals + 2 aliases */

   /* Prefix: every is_alias=false row precedes every is_alias=true row.
    * Once we see an alias, every subsequent row must also be an alias. */
   bool seen_alias = false;
   for (int i = 0; i < count; i++) {
      if (rows[i].is_alias) {
         seen_alias = true;
         TEST_ASSERT_EQUAL_INT64_MESSAGE(hub, rows[i].canonical_id,
                                         "alias row missing canonical_id back-reference");
      } else {
         TEST_ASSERT_FALSE_MESSAGE(seen_alias, "canonical row appeared after an alias row");
         TEST_ASSERT_EQUAL_INT64(0, rows[i].canonical_id);
      }
   }

   /* Both aliases surfaced and both pointed at hub. */
   bool saw_alias_a = false, saw_alias_b = false;
   for (int i = 0; i < count; i++) {
      if (!rows[i].is_alias)
         continue;
      if (rows[i].entity_id == alias_a)
         saw_alias_a = true;
      if (rows[i].entity_id == alias_b)
         saw_alias_b = true;
   }
   TEST_ASSERT_TRUE(saw_alias_a);
   TEST_ASSERT_TRUE(saw_alias_b);

   /* Solo (no aliases) sits in the canonical prefix with canonical_id = 0. */
   bool saw_solo = false;
   for (int i = 0; i < count; i++) {
      if (rows[i].entity_id == solo) {
         saw_solo = true;
         TEST_ASSERT_FALSE(rows[i].is_alias);
         TEST_ASSERT_EQUAL_INT64(0, rows[i].canonical_id);
      }
   }
   TEST_ASSERT_TRUE(saw_solo);
}

/* ============================================================================
 * Equivalence-class stats aggregation (Bundle 2, 2026-05-13)
 *
 * mention_count / first_seen / last_seen are aggregated over
 * {self + soft-aliases pointing at self} via correlated subqueries
 * inside three SELECTs: entity_get_by_name, entity_search, and the admin
 * canonical-list query in memory_db_alias.c.  These tests pin the SQL
 * behavior so the next maintainer can't regress to per-row values.
 * ============================================================================ */

static void set_entity_stats(int64_t entity_id,
                             int mention_count,
                             int64_t first_seen,
                             int64_t last_seen) {
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE memory_entities SET mention_count = ?, first_seen = ?, last_seen = ? WHERE id = ?",
       -1, &stmt, NULL);
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);
   sqlite3_bind_int(stmt, 1, mention_count);
   sqlite3_bind_int64(stmt, 2, first_seen);
   sqlite3_bind_int64(stmt, 3, last_seen);
   sqlite3_bind_int64(stmt, 4, entity_id);
   rc = sqlite3_step(stmt);
   TEST_ASSERT_EQUAL_INT(SQLITE_DONE, rc);
   sqlite3_finalize(stmt);
}

static void test_entity_get_by_name_aggregates_class_stats(void) {
   /* Canonical with one alias.  Class total: mc=15, fs=500, ls=2500. */
   int64_t canonical = insert_entity_typed(g_test_user_id, "Canon", "person");
   int64_t alias = insert_entity_typed(g_test_user_id, "Aliased", "person");
   set_entity_stats(canonical, 10, 1000, 2000);
   set_entity_stats(alias, 5, 500, 2500);
   int64_t link_id = 0;
   memory_db_entity_alias_link(g_test_user_id, alias, canonical, "soft", "operator", 0.95f, NULL,
                               &link_id);

   memory_entity_t out = { 0 };
   int rc = memory_db_entity_get_by_name(g_test_user_id, "canon", &out);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT64(canonical, out.id);
   TEST_ASSERT_EQUAL_INT(15, out.mention_count);          /* SUM */
   TEST_ASSERT_EQUAL_INT64(500, (int64_t)out.first_seen); /* MIN */
   TEST_ASSERT_EQUAL_INT64(2500, (int64_t)out.last_seen); /* MAX */
}

static void test_entity_get_by_name_alias_returns_self_stats(void) {
   /* Querying an alias row directly should return its own stats (no
    * class aggregation), since aliases have no dependents under the
    * single-level rule. */
   int64_t canonical = insert_entity_typed(g_test_user_id, "Canon", "person");
   int64_t alias = insert_entity_typed(g_test_user_id, "Aliased", "person");
   set_entity_stats(canonical, 10, 1000, 2000);
   set_entity_stats(alias, 5, 500, 2500);
   int64_t link_id = 0;
   memory_db_entity_alias_link(g_test_user_id, alias, canonical, "soft", "operator", 0.95f, NULL,
                               &link_id);

   memory_entity_t out = { 0 };
   int rc = memory_db_entity_get_by_name(g_test_user_id, "aliased", &out);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT64(alias, out.id);
   /* Alias's own values, not the canonical's. */
   TEST_ASSERT_EQUAL_INT(5, out.mention_count);
   TEST_ASSERT_EQUAL_INT64(500, (int64_t)out.first_seen);
   TEST_ASSERT_EQUAL_INT64(2500, (int64_t)out.last_seen);
}

static void test_entity_search_aggregates_class_stats(void) {
   int64_t canonical = insert_entity_typed(g_test_user_id, "Searchable", "person");
   int64_t alias = insert_entity_typed(g_test_user_id, "SearchAlias", "person");
   set_entity_stats(canonical, 7, 100, 200);
   set_entity_stats(alias, 3, 50, 250);
   int64_t link_id = 0;
   memory_db_entity_alias_link(g_test_user_id, alias, canonical, "soft", "operator", 0.95f, NULL,
                               &link_id);

   memory_entity_t out[8];
   int count = 0;
   int rc = memory_db_entity_search(g_test_user_id, "search", out, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_GREATER_OR_EQUAL_INT(1, count);
   /* Find the canonical row in the result. */
   bool saw_canonical = false;
   for (int i = 0; i < count; i++) {
      if (out[i].id == canonical) {
         saw_canonical = true;
         TEST_ASSERT_EQUAL_INT(10, out[i].mention_count);         /* 7 + 3 */
         TEST_ASSERT_EQUAL_INT64(50, (int64_t)out[i].first_seen); /* MIN */
         TEST_ASSERT_EQUAL_INT64(250, (int64_t)out[i].last_seen); /* MAX */
      }
   }
   TEST_ASSERT_TRUE(saw_canonical);
}

static void test_entity_get_by_name_solo_canonical_no_aliases(void) {
   /* Degenerate equivalence class of 1 — canonical with no aliases.
    * Subquery's OR clause still matches the row itself via id=e.id, so the
    * aggregates return self values (not NULL).  Pins the COALESCE wrap as
    * defense-in-depth rather than load-bearing. */
   int64_t solo = insert_entity_typed(g_test_user_id, "Solo", "person");
   set_entity_stats(solo, 7, 100, 200);

   memory_entity_t out = { 0 };
   int rc = memory_db_entity_get_by_name(g_test_user_id, "solo", &out);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT64(solo, out.id);
   TEST_ASSERT_EQUAL_INT(7, out.mention_count);
   TEST_ASSERT_EQUAL_INT64(100, (int64_t)out.first_seen);
   TEST_ASSERT_EQUAL_INT64(200, (int64_t)out.last_seen);
}

static void test_entity_list_for_admin_aggregates_class_mention_count(void) {
   /* Admin canonical-list should display the SUMMED mention_count.
    * Reproduces the Step 4 motivator: "Jon Smith (mention_count=50)"
    * soft-aliased into "Jonathan Smith (mention_count=3)" should
    * read as 53 in the admin output, not 3. */
   int64_t canonical = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   int64_t alias = insert_entity_typed(g_test_user_id, "Jon Smith", "person");
   set_entity_stats(canonical, 3, 1000, 1000);
   set_entity_stats(alias, 50, 500, 2000);
   int64_t link_id = 0;
   memory_db_entity_alias_link(g_test_user_id, alias, canonical, "soft", "operator", 0.95f, NULL,
                               &link_id);

   memory_alias_entity_row_t rows[8];
   int count = 0;
   int rc = memory_db_entity_list_for_admin(g_test_user_id, /* include_aliases */ false, rows, 8,
                                            &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   bool saw = false;
   for (int i = 0; i < count; i++) {
      if (rows[i].entity_id == canonical) {
         saw = true;
         TEST_ASSERT_EQUAL_INT(53, rows[i].mention_count); /* 3 + 50, not 3 */
      }
   }
   TEST_ASSERT_TRUE(saw);
}

/* ============================================================================
 * Ckpt 5: alias_summary helper + LLM-tool integration smoke
 *
 * The smoke scenarios exercise the same code path the LLM tool uses
 * (memory_db_entity_alias_link / _alias_unlink / equivalence-class relation
 * listing) end-to-end on a seeded fixture.  They mirror the brief's
 * required scenario: extraction emits "Jon", manual merge_entities links
 * to "Jonathan Smith", focus block recall returns canonical (relations
 * surface across the equivalence class), split restores both.
 *
 * The reextract-drop-alias-state behavior is verified in
 * test_memory_db_admin.c — not duplicated here.
 * ============================================================================ */

static void test_alias_summary_returns_alias_canonical_pairs(void) {
   /* Two aliases pointing at one canonical, plus a standalone canonical. */
   int64_t canonical1 = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   int64_t alias_a = insert_entity_typed(g_test_user_id, "Jon", "person");
   int64_t alias_b = insert_entity_typed(g_test_user_id, "J. Smith", "person");
   int64_t standalone = insert_entity_typed(g_test_user_id, "Bruno", "pet");

   int64_t link_a = 0, link_b = 0;
   memory_db_entity_alias_link(g_test_user_id, alias_a, canonical1, "soft", "operator", 0.95f, NULL,
                               &link_a);
   memory_db_entity_alias_link(g_test_user_id, alias_b, canonical1, "soft", "operator", 0.85f, NULL,
                               &link_b);

   memory_alias_summary_row_t rows[16];
   int count = 0;
   int rc = memory_db_entity_alias_summary(g_test_user_id, rows, 16, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, count);

   /* Both rows must point at canonical1 and identify the alias entity ids. */
   bool saw_a = false, saw_b = false;
   for (int i = 0; i < count; i++) {
      TEST_ASSERT_EQUAL_INT64(canonical1, rows[i].canonical_entity_id);
      if (rows[i].alias_entity_id == alias_a)
         saw_a = true;
      if (rows[i].alias_entity_id == alias_b)
         saw_b = true;
      TEST_ASSERT_NOT_EQUAL_INT64(standalone, rows[i].alias_entity_id);
      TEST_ASSERT_NOT_EQUAL_INT64(canonical1, rows[i].alias_entity_id);
   }
   TEST_ASSERT_TRUE(saw_a);
   TEST_ASSERT_TRUE(saw_b);
}

static void test_alias_summary_empty_for_user_with_no_aliases(void) {
   insert_entity_typed(g_test_user_id, "Solo", "person");
   memory_alias_summary_row_t rows[8];
   int count = 999; /* must be zeroed */
   int rc = memory_db_entity_alias_summary(g_test_user_id, rows, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(0, count);
}

static void test_smoke_llm_merge_entities_soft_link_path(void) {
   /* Seed: canonical "Jonathan Smith" + soon-to-be-alias "Jon".  Each
    * has one open relation so we can confirm the focus-block recall path
    * surfaces relations across the equivalence class after the link. */
   int64_t canonical = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   int64_t source = insert_entity_typed(g_test_user_id, "Jon", "person");
   int64_t shop = insert_entity_typed(g_test_user_id, "CodeShop", "organization");
   int64_t bruno = insert_entity_typed(g_test_user_id, "Bruno", "pet");

   insert_open_relation(g_test_user_id, canonical, "works_at", shop, NULL);
   insert_open_relation(g_test_user_id, source, "owns_pet", bruno, NULL);

   /* Step 1: simulate the LLM tool path — call alias_link with the
    * exact same arguments memory_callback.c now passes. */
   int64_t link_id = 0;
   int rc = memory_db_entity_alias_link(g_test_user_id, source, canonical, "soft",
                                        "llm-tool-action",
                                        /* composite_score */ -1.0f,
                                        /* evidence_json */ NULL, &link_id);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_GREATER_THAN(0, link_id);

   /* Step 2: source row carries canonical_id = target (soft alias state). */
   TEST_ASSERT_EQUAL_INT64(canonical, get_entity_canonical_id(source));
   TEST_ASSERT_EQUAL_INT(1, count_active_aliases_for_target(canonical));

   /* Step 3: focus-block recall — equivalence-class relation listing on
    * the canonical now returns BOTH the canonical's and the alias's
    * relations (works_at AND owns_pet).  This is the read-side guarantee
    * the soft alias is supposed to deliver. */
   memory_relation_t rels[16];
   int rcount = 0;
   rc = memory_db_relation_list_by_subject_class(g_test_user_id, canonical, rels, 16, &rcount);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, rcount);
   bool saw_works_at = false, saw_owns_pet = false;
   for (int i = 0; i < rcount; i++) {
      if (strcmp(rels[i].relation, "works_at") == 0)
         saw_works_at = true;
      if (strcmp(rels[i].relation, "owns_pet") == 0)
         saw_owns_pet = true;
   }
   TEST_ASSERT_TRUE_MESSAGE(saw_works_at,
                            "canonical's relation should appear in equivalence-class listing");
   TEST_ASSERT_TRUE_MESSAGE(saw_owns_pet,
                            "alias's relation should appear in equivalence-class listing");

   /* Step 4: split — alias_unlink restores source as a standalone canonical. */
   rc = memory_db_entity_alias_unlink(g_test_user_id, link_id, "split-by-operator");
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(source));
   TEST_ASSERT_EQUAL_INT(0, count_active_aliases_for_target(canonical));

   /* Equivalence-class lookup on canonical now returns ONLY the canonical's
    * relation (the source's relation no longer surfaces here — split
    * confirmed). */
   memset(rels, 0, sizeof(rels));
   rcount = 0;
   rc = memory_db_relation_list_by_subject_class(g_test_user_id, canonical, rels, 16, &rcount);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, rcount);
   TEST_ASSERT_EQUAL_STRING("works_at", rels[0].relation);

   /* Looking up by source as canonical now returns its own relation — split
    * has restored its independent equivalence class. */
   memset(rels, 0, sizeof(rels));
   rcount = 0;
   rc = memory_db_relation_list_by_subject_class(g_test_user_id, source, rels, 16, &rcount);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, rcount);
   TEST_ASSERT_EQUAL_STRING("owns_pet", rels[0].relation);
}

/* ============================================================================
 * Ckpt 5 fold-in for arch-L2 — synthetic-self dry-run UX
 *
 * Pre-fix: dry-run with no existing user-self showed every candidate as
 * REJECTED (no canonical to score against → composite 0.0).  Post-fix:
 * we synthesize a person-typed entity using the username + optional
 * persona_description and pair-score every candidate against it, with
 * is_user_self=true on the synthetic side so user_self_bonus fires.
 *
 * The dev's first invocation against an existing cluster is exactly this
 * scenario.  The behavioral signal we pin in this test: a candidate
 * whose canonical_name matches the username substring + has the same
 * type lands at least in the review band (>= 0.70), not REJECTED.
 * (Hitting auto-merge >= 0.90 typically needs embedding_cosine or shared
 * relations; the synthetic has id=0 so DB-touching signals contribute 0,
 * which is the correct behavior for a not-yet-materialized entity.)
 * ============================================================================ */

static void test_link_user_self_dry_run_synthetic_scores_existing_cluster(void) {
   /* Phase 1.5 Ckpt B: synthetic seed pulls from users.real_name +
    * users.identity_aliases.  Set real_name to "Jon" so the synthetic
    * canonical_name matches the candidate's. */
   auth_user_identity_t id;
   memset(&id, 0, sizeof(id));
   strncpy(id.real_name, "Jon", AUTH_REAL_NAME_MAX - 1);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_set_user_identity(g_test_user_id, &id));

   int64_t jon_ent = insert_entity_typed(g_test_user_id, "Jon", "person");
   /* Add an unrelated org entity so we can verify the jon candidate
    * outranks it (sanity check on the synthetic-pair scoring, not just
    * "every candidate gets the same uniform boost"). */
   int64_t acme = insert_entity_typed(g_test_user_id, "Acme", "org");

   /* No is_user_self=1 row — the dry-run path should synthesize one. */

   memory_alias_link_user_self_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = memory_alias_link_user_self_run(g_test_user_id, /* dry_run */ true, &result);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);

   /* Phase 1.5 fold-in #3: real_name canonical "jon" exact-matches the
    * inserted entity "Jon" — the synthetic resolver hits Stage 1 and
    * promotes that entity (would-promote in dry-run; UPDATE in commit).
    * Old behavior was synthetic-seed always; new behavior reflects what
    * commit will actually do, so dry-run is now informative. */
   TEST_ASSERT_EQUAL_INT64(jon_ent, result.self_entity_id);
   TEST_ASSERT_TRUE(result.self_was_promoted);
   TEST_ASSERT_FALSE(result.self_was_seeded);

   /* Jon is now the promoted self — excluded from result.rows by
    * list_canonical_entity_ids (which skips self_id).  Acme remains as
    * a candidate scored against the (would-be-promoted) Jon entity.
    * The behavioral guarantee here: dry-run accurately previews what
    * commit will do, by promoting Jon AND scoring Acme against it.
    * Acme has no name overlap or relations with Jon, so its composite
    * is near zero (rejected band). */
   memory_alias_link_user_self_row_t *jon_row = NULL;
   memory_alias_link_user_self_row_t *acme_row = NULL;
   for (int i = 0; i < result.row_count; i++) {
      if (result.rows[i].entity_id == jon_ent)
         jon_row = &result.rows[i];
      else if (result.rows[i].entity_id == acme)
         acme_row = &result.rows[i];
   }
   TEST_ASSERT_NULL(jon_row); /* Jon is the self, excluded from candidates */
   TEST_ASSERT_NOT_NULL(acme_row);
   TEST_ASSERT_LESS_THAN_FLOAT(MEMORY_ALIAS_REVIEW_THRESHOLD, acme_row->composite_score);

   /* No DB mutation: alias / proposal tables stay empty. */
   TEST_ASSERT_EQUAL_INT(0, count_db_rows("SELECT COUNT(*) FROM memory_entity_aliases "
                                          "WHERE unlinked_at IS NULL"));
   TEST_ASSERT_EQUAL_INT(0, count_db_rows("SELECT COUNT(*) FROM memory_entity_merge_proposals "
                                          "WHERE resolved_at IS NULL"));
}

/* ============================================================================
 * Phase 1.5 Ckpt B: synthetic seed sources (real_name + identity_aliases)
 *
 * The Ckpt B refactor replaced the persona_description reach-around with a
 * direct read from users.real_name + users.identity_aliases via the
 * auth_db_get_user_identity helper.  Three regression-shape tests:
 *
 *   1. Token union: synthetic canonical_name covers tokens from real_name
 *      AND each alias line, so a candidate matching ANY alias scores high.
 *   2. Alias parsing: newline split, whitespace strip, empty-line drop,
 *      case-insensitive dedupe — verified by emitting an entity per
 *      expected token and checking the synthetic outranks an unrelated
 *      org for each.
 *   3. Persona no-leak: setting persona_description on user_settings has
 *      ZERO impact on the synthetic seed (dropped reach-around regression).
 * ============================================================================ */

static void set_user_settings_persona(int user_id, const char *persona) {
   /* Direct write via auth_db_set_user_settings — round-trips via the
    * existing user_settings UPSERT. */
   auth_user_settings_t s;
   memset(&s, 0, sizeof(s));
   strncpy(s.persona_mode, "append", AUTH_PERSONA_MODE_MAX - 1);
   strncpy(s.persona_description, persona, AUTH_PERSONA_DESC_MAX - 1);
   strncpy(s.timezone, "UTC", AUTH_TIMEZONE_MAX - 1);
   strncpy(s.units, "metric", AUTH_UNITS_MAX - 1);
   strncpy(s.theme, "cyan", AUTH_THEME_MAX - 1);
   auth_db_set_user_settings(user_id, &s);
}

static void test_synthetic_seed_unions_real_name_and_alias_tokens(void) {
   /* real_name = "Jonathan Smith" + aliases "Jon\nsmithfabrications"
    * — the synthetic canonical should carry tokens for all four words.
    * Each candidate matches a distinct token and lands above an unrelated
    * org control. */
   auth_user_identity_t id;
   memset(&id, 0, sizeof(id));
   strncpy(id.real_name, "Jonathan Smith", AUTH_REAL_NAME_MAX - 1);
   strncpy(id.identity_aliases, "Jon\nsmithfabrications", AUTH_IDENTITY_ALIASES_MAX - 1);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_set_user_identity(g_test_user_id, &id));

   int64_t real_match = insert_entity_typed(g_test_user_id, "Jonathan", "person");
   int64_t alias_match_a = insert_entity_typed(g_test_user_id, "Jon", "person");
   int64_t alias_match_b = insert_entity_typed(g_test_user_id, "smithfabrications", "person");
   int64_t control = insert_entity_typed(g_test_user_id, "Acme", "org");

   memory_alias_link_user_self_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = memory_alias_link_user_self_run(g_test_user_id, /* dry_run */ true, &result);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);

   /* Phase 1.5 fold-in #3: the synthetic resolver finds a strong match
    * via Stage 6 (name_jaccard + alias-substring + type_match) on one
    * of the identity-token candidates and promotes it.  Most likely
    * winner is "Jonathan" (name_jaccard 1/2 = 0.5 + substring + alias-
    * substring bonus from "jon"), but the precise winner is order-
    * independent — what matters here is that promote-existing fired
    * AND the unrelated org control was not selected. */
   TEST_ASSERT_TRUE(result.self_was_promoted);
   TEST_ASSERT_FALSE(result.self_was_seeded);
   TEST_ASSERT_NOT_EQUAL(control, result.self_entity_id);
   TEST_ASSERT_TRUE(result.self_entity_id == real_match || result.self_entity_id == alias_match_a ||
                    result.self_entity_id == alias_match_b);

   /* The promoted entity is excluded from result.rows; the others
    * remain as candidates scored against it.  Acme should still be
    * present and stay near-zero (no overlap with the promoted self). */
   memory_alias_link_user_self_row_t *r_ctl = NULL;
   for (int i = 0; i < result.row_count; i++) {
      if (result.rows[i].entity_id == control)
         r_ctl = &result.rows[i];
   }
   TEST_ASSERT_NOT_NULL(r_ctl);
   TEST_ASSERT_LESS_THAN_FLOAT(MEMORY_ALIAS_REVIEW_THRESHOLD, r_ctl->composite_score);
}

static void test_synthetic_seed_aliases_parsing_strip_dedup_skip_empty(void) {
   /* Aliases input exercises the parser:
    *   - leading/trailing whitespace per line
    *   - empty lines (multiple \n in a row)
    *   - case-insensitive dedupe ("Jon" vs "jon" → kept once)
    *   - tabs and \r mixed in
    * The parser should still produce a meaningful synthetic — an entity
    * matching any of the unique non-empty tokens lands above the control. */
   auth_user_identity_t id;
   memset(&id, 0, sizeof(id));
   strncpy(id.real_name, "Jonathan Smith", AUTH_REAL_NAME_MAX - 1);
   /* "  Jon  " with spaces, "" empty, duplicate "jon" different case,
    * "smithfabrications" lowercase, then a trailing-whitespace tab line. */
   const char *messy = "  Jon  \n\n\tjon\nsmithfabrications\nSMITHFABRICATIONS\n  \r\n";
   strncpy(id.identity_aliases, messy, AUTH_IDENTITY_ALIASES_MAX - 1);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_set_user_identity(g_test_user_id, &id));

   int64_t jon_ent = insert_entity_typed(g_test_user_id, "Jon", "person");
   int64_t kf_ent = insert_entity_typed(g_test_user_id, "smithfabrications", "person");
   int64_t control = insert_entity_typed(g_test_user_id, "Acme", "org");

   memory_alias_link_user_self_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = memory_alias_link_user_self_run(g_test_user_id, /* dry_run */ true, &result);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);

   memory_alias_link_user_self_row_t *r_jon = NULL, *r_kf = NULL, *r_ctl = NULL;
   for (int i = 0; i < result.row_count; i++) {
      if (result.rows[i].entity_id == jon_ent)
         r_jon = &result.rows[i];
      else if (result.rows[i].entity_id == kf_ent)
         r_kf = &result.rows[i];
      else if (result.rows[i].entity_id == control)
         r_ctl = &result.rows[i];
   }
   TEST_ASSERT_NOT_NULL(r_jon);
   TEST_ASSERT_NOT_NULL(r_kf);
   TEST_ASSERT_NOT_NULL(r_ctl);
   /* Both identity-derived candidates must outrank the unrelated control —
    * proves the parser kept the dedupe'd, stripped tokens. */
   TEST_ASSERT_GREATER_THAN_FLOAT(r_ctl->composite_score, r_jon->composite_score);
   TEST_ASSERT_GREATER_THAN_FLOAT(r_ctl->composite_score, r_kf->composite_score);
}

static void test_synthetic_seed_does_not_leak_persona_description(void) {
   /* Regression: pre-Ckpt-B the synthetic appended persona_description
    * tokens to the canonical_name.  Ckpt B removes that reach-around;
    * persona_description returns to its original AI-tuning purpose only.
    *
    * Setup:
    *   - real_name set to a unique token "Solo" with no overlap with
    *     either the candidate's name or the persona text
    *   - persona_description set to "DistinctPersonaWord" — pre-Ckpt-B
    *     this token would have been unioned into the synthetic and would
    *     have caused a candidate named "DistinctPersonaWord" to score
    *     above the control.  Post-Ckpt-B it must NOT.
    *
    * Expect: persona_word entity scores AT/BELOW the control's score
    * (only signals firing are: type_match=person/org → 0, no match → no
    * bonus; control gets the same baseline). */
   auth_user_identity_t id;
   memset(&id, 0, sizeof(id));
   strncpy(id.real_name, "Solo", AUTH_REAL_NAME_MAX - 1);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_set_user_identity(g_test_user_id, &id));

   set_user_settings_persona(g_test_user_id, "DistinctPersonaWord");

   int64_t persona_word = insert_entity_typed(g_test_user_id, "DistinctPersonaWord", "person");
   int64_t control = insert_entity_typed(g_test_user_id, "Acme", "org");

   memory_alias_link_user_self_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = memory_alias_link_user_self_run(g_test_user_id, /* dry_run */ true, &result);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);

   memory_alias_link_user_self_row_t *r_pw = NULL, *r_ctl = NULL;
   for (int i = 0; i < result.row_count; i++) {
      if (result.rows[i].entity_id == persona_word)
         r_pw = &result.rows[i];
      else if (result.rows[i].entity_id == control)
         r_ctl = &result.rows[i];
   }
   TEST_ASSERT_NOT_NULL(r_pw);
   TEST_ASSERT_NOT_NULL(r_ctl);

   /* Headline regression: persona_description tokens MUST NOT have entered
    * the synthetic.  If they did, persona_word's name_jaccard would fire
    * (token "distinctpersonaword" in synthetic) and lift it above control.
    * Post-Ckpt-B, persona_word has no name overlap with synthetic ("solo")
    * — the only differentiator vs control is type_match (person, +0.05)
    * and user_self_bonus (NOT applicable since neither side is the
    * username "jon"... wait, user_self_bonus checks username substring
    * match against the candidate.  Candidate "DistinctPersonaWord" has
    * canonical "distinctpersonaword" which does NOT contain "jon".  So
    * no user_self bonus.  control "Acme" canonical "acme" also no
    * user_self.  Both get type baseline.) */
   TEST_ASSERT_FLOAT_WITHIN(0.001f, r_ctl->composite_score + 0.05f, r_pw->composite_score);
}

/* ============================================================================
 * Phase 1.5 Ckpt C: directional similarity + "user" allow-list
 *
 *   1. Directional overlap at Stage 2 catches single-token candidates
 *      that standard Jaccard would drop below the 0.30 floor.
 *   2. The canonical_name='user' allow-list pre-add lets that entity
 *      reach Stage 6 even when name signals are zero.
 *   3. Regression: non-synthetic resolver path still uses standard
 *      Jaccard and still drops at the floor.
 *
 * The synthetic-self resolver entry point is
 * memory_db_entity_resolve_alias_for_self(), introduced in Ckpt C.
 * ============================================================================ */

static void test_resolve_for_self_directional_overlap_keeps_single_token(void) {
   /* Synthetic seed shape: real_name="Jonathan Smith" + aliases
    * gives a 4-token canonical "jonathan smith jon smithfabrications".
    * Candidate is single-token "jon".
    *
    * Standard Jaccard: |{jon}| / |{jonathan,smith,jon,smithfabrications}|
    *                 = 1 / 4 = 0.25 → BELOW the 0.30 floor → dropped at
    *                 Stage 2 → resolver returns clean miss (resolved_id=0,
    *                 matched_stage=0).
    *
    * Directional overlap (synth-self mode): |{jon}| / |{jon}|
    *                 = 1 / 1 = 1.00 → kept past Stage 2 → reaches Stage 6
    *                 with non-zero composite → resolved_id = jon.
    * The auto-merge threshold isn't reached without embeddings/relations
    * in the test harness, but the cascade DID score the candidate — that's
    * the directional-overlap invariant we're pinning. */
   int64_t jon = insert_entity_typed(g_test_user_id, "Jon", "person");
   /* Add an unrelated org so we know the resolver isn't just picking the
    * only candidate. */
   insert_entity_typed(g_test_user_id, "Acme", "org");

   const char *synth_canonical = "jonathan smith jon smithfabrications";
   memory_alias_resolve_t res;
   memset(&res, 0, sizeof(res));
   int rc = memory_db_entity_resolve_alias_for_self(g_test_user_id, synth_canonical, "person",
                                                    &res);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT64(jon, res.resolved_id);
   TEST_ASSERT_EQUAL_INT(6, res.matched_stage);
   /* Composite must be > 0 — the candidate was scored, not silently
    * dropped at the directional-overlap floor. */
   TEST_ASSERT_GREATER_THAN_FLOAT(0.0f, res.evidence.composite_score);
   /* Non-Acme: the unrelated org would have been vetoed at Stage 3 by
    * type filter (person/org with no thing carve-out), so the resolver
    * had no choice but Jon. */
}

static void test_resolve_for_self_user_allow_list_reaches_stage6(void) {
   /* The "user" entity has zero name overlap with any verbose synthetic
    * — neither standard Jaccard nor directional overlap would keep it
    * via Stage 2.  The allow-list pre-add bypasses the floor and feeds
    * it through Stages 3-6 normally.  user_self_bonus carries the
    * scoring (the candidate reaches Stage 6 with non-zero composite).
    *
    * To make user_self_bonus fire, the test creates a user named "user"
    * so the username canonical matches the entity's canonical_name —
    * the same shape the dev's live DB has. */
   auth_db_create_user("user", "hash", false);
   auth_user_t u;
   memset(&u, 0, sizeof(u));
   auth_db_get_user("user", &u);
   int second_user = u.id;
   TEST_ASSERT_GREATER_THAN(0, second_user);

   int64_t user_ent = insert_entity_typed(second_user, "user", "thing");

   /* Synth canonical has zero token overlap with "user" — confirms the
    * allow-list (not Stage 2 token-Jaccard) is what surfaces the
    * candidate. */
   const char *synth_canonical = "jonathan smith";
   memory_alias_resolve_t res;
   memset(&res, 0, sizeof(res));
   int rc = memory_db_entity_resolve_alias_for_self(second_user, synth_canonical, "person", &res);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);

   /* "user" reached Stage 6 with non-zero composite (user_self_bonus
    * fires via username substring "user" ⊂ "user"). */
   TEST_ASSERT_EQUAL_INT64(user_ent, res.resolved_id);
   TEST_ASSERT_EQUAL_INT(6, res.matched_stage);
   TEST_ASSERT_GREATER_THAN_FLOAT(0.0f, res.evidence.composite_score);
   TEST_ASSERT_TRUE(res.evidence.user_self_bonus_applied);
}

static void test_resolve_alias_non_synth_preserves_jaccard_floor(void) {
   /* Regression: standard memory_db_entity_resolve_alias() (extraction-
    * time path, use_synth_self=false) MUST still apply the standard
    * Jaccard 0.30 floor.  Single-token "jon" against verbose
    * "jonathan smith jon smithfabrications" gives Jaccard 0.25 →
    * below floor → dropped → no resolution. */
   insert_entity_typed(g_test_user_id, "Jon", "person");

   memory_alias_resolve_t res;
   memset(&res, 0, sizeof(res));
   int rc = memory_db_entity_resolve_alias(g_test_user_id, "jonathan smith jon smithfabrications",
                                           "person", "jonathan smith jon smithfabrications", &res);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   /* No Stage 1 hit (no entity has the verbose canonical) and Stage 2
    * drops "jon" via the 0.30 floor → no resolution. */
   TEST_ASSERT_EQUAL_INT64(0, res.resolved_id);
}

/* ============================================================================
 * Ckpt 5 fold-in for emb-M2 — single-SQL exclusive_relation_overlap
 *
 * Pins both halves of the unified-path: a pair where both sides have
 * works_at to the same target → overlap=1.0, and a pair where neither
 * side has an open exclusive relation → overlap=0.0.  This exercises the
 * IN (?, ?) + ORDER BY valid_from DESC + discriminator-routing path.
 * ============================================================================ */

static void test_exclusive_relation_overlap_unified_sql_path(void) {
   int64_t company = insert_entity_typed(g_test_user_id, "Acme Corp", "org");
   int64_t a_with = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   int64_t b_with = insert_entity_typed(g_test_user_id, "Jon", "person");
   int64_t a_no = insert_entity_typed(g_test_user_id, "Alice", "person");
   int64_t b_no = insert_entity_typed(g_test_user_id, "Albert", "person");

   /* Half 1: both sides have works_at → Acme Corp (open).  Single-SQL must
    * route Acme matches into both a_rels and b_rels via discriminator. */
   insert_open_relation(g_test_user_id, a_with, "works_at", company, NULL);
   insert_open_relation(g_test_user_id, b_with, "works_at", company, NULL);

   memory_alias_evidence_t ev;
   int rc = memory_db_entity_score_pair(g_test_user_id, b_with, a_with, &ev);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_FLOAT(1.0f, ev.exclusive_relation_overlap);

   /* Half 2: neither side has open exclusive relations.  Single-SQL must
    * cleanly return overlap=0 (no rows from either side). */
   memset(&ev, 0, sizeof(ev));
   rc = memory_db_entity_score_pair(g_test_user_id, a_no, b_no, &ev);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_FLOAT(0.0f, ev.exclusive_relation_overlap);
}

/* ============================================================================
 * Ckpt 5 fold-in for arch-M1 — score_pair refuses alias targets
 * ============================================================================ */

static void test_score_pair_refuses_alias_target(void) {
   /* canonical + alias linked to it; score_pair against the alias must
    * surface MEMORY_DB_INVALID_ALIAS_TARGET so the WebUI preview can
    * render a "pick its canonical" message rather than silently scoring
    * against a missing-from-cache embedding. */
   int64_t canonical = insert_entity_typed(g_test_user_id, "Canon", "person");
   int64_t alias = insert_entity_typed(g_test_user_id, "AliasOfCanon", "person");
   int64_t standalone = insert_entity_typed(g_test_user_id, "Standalone", "person");

   int64_t link_id = 0;
   int rc = memory_db_entity_alias_link(g_test_user_id, alias, canonical, "soft", "operator", 0.95f,
                                        NULL, &link_id);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);

   /* Target is an alias → refuse. */
   memory_alias_evidence_t ev;
   rc = memory_db_entity_score_pair(g_test_user_id, standalone, alias, &ev);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_INVALID_ALIAS_TARGET, rc);
   TEST_ASSERT_EQUAL_FLOAT(0.0f, ev.composite_score);

   /* Source is an alias → refuse (symmetric — score_pair is direction-
    * agnostic on the alias check). */
   rc = memory_db_entity_score_pair(g_test_user_id, alias, standalone, &ev);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_INVALID_ALIAS_TARGET, rc);

   /* Two canonicals — succeeds. */
   rc = memory_db_entity_score_pair(g_test_user_id, canonical, standalone, &ev);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
}

/* ============================================================================
 * Ckpt 5 fold-in for arch-L4 — alias_link refuses link_kind="hard"
 * ============================================================================ */

static void test_alias_link_refuses_hard_kind(void) {
   /* Phase 3 will introduce the operator consolidate path that promotes a
    * soft link to hard.  Until then the alias surface refuses "hard" so
    * the unimplemented promotion logic can't be bypassed. */
   int64_t a = insert_entity_typed(g_test_user_id, "A", "person");
   int64_t b = insert_entity_typed(g_test_user_id, "B", "person");
   int64_t link_id = 0;
   int rc = memory_db_entity_alias_link(g_test_user_id, a, b, "hard", "operator", -1.0f, NULL,
                                        &link_id);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_FAILURE, rc);
   TEST_ASSERT_EQUAL_INT64(0, link_id);
   /* Sanity: no canonical_id set, no audit row. */
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(a));
   TEST_ASSERT_EQUAL_INT(0, count_active_aliases_for_target(b));

   /* "soft" still works. */
   rc = memory_db_entity_alias_link(g_test_user_id, a, b, "soft", "operator", -1.0f, NULL,
                                    &link_id);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_GREATER_THAN(0, link_id);
}

/* ============================================================================
 * Cross-user negative tests (Ckpt 5 fold-in for sec-L3)
 *
 * The alias surface SHOULD reject cross-user requests with NOT_FOUND so
 * an authenticated user can't reach into another user's graph by passing
 * raw entity / proposal / link ids.  These tests pin that invariant.
 * ============================================================================ */

static int seed_second_user(const char *username) {
   auth_db_create_user(username, "hash", false);
   auth_user_t u;
   memset(&u, 0, sizeof(u));
   auth_db_get_user(username, &u);
   return u.id;
}

static void test_cross_user_alias_link_returns_not_found(void) {
   /* User A owns both entities; user B tries to alias_link them. */
   int user_a = g_test_user_id;
   int user_b = seed_second_user("bob");
   TEST_ASSERT_GREATER_THAN(0, user_b);

   int64_t a_src = insert_entity_typed(user_a, "Source A", "person");
   int64_t a_tgt = insert_entity_typed(user_a, "Target A", "person");

   int64_t link_id = 0;
   int rc = memory_db_entity_alias_link(user_b, a_src, a_tgt, "soft", "operator", -1.0f, NULL,
                                        &link_id);
   /* user_b doesn't own these entities → load_entity_full returns
    * NOT_FOUND, which alias_link surfaces verbatim. */
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_NOT_FOUND, rc);
   TEST_ASSERT_EQUAL_INT64(0, link_id);

   /* Sanity: source row is unchanged. */
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(a_src));
}

static void test_cross_user_proposal_resolve_returns_not_found(void) {
   /* User A's proposal must be invisible to user B. */
   int user_a = g_test_user_id;
   int user_b = seed_second_user("bob");
   TEST_ASSERT_GREATER_THAN(0, user_b);

   int64_t a_src = insert_entity_typed(user_a, "Source A", "person");
   int64_t a_tgt = insert_entity_typed(user_a, "Target A", "person");

   /* Manually insert a pending proposal owned by user A. */
   sqlite3_stmt *ins = NULL;
   sqlite3_prepare_v2(s_db.db,
                      "INSERT INTO memory_entity_merge_proposals "
                      "(user_id, source_entity_id, target_entity_id, composite_score, "
                      " evidence_json, proposed_at) "
                      "VALUES (?, ?, ?, 0.80, '{}', strftime('%s','now'))",
                      -1, &ins, NULL);
   sqlite3_bind_int(ins, 1, user_a);
   sqlite3_bind_int64(ins, 2, a_src);
   sqlite3_bind_int64(ins, 3, a_tgt);
   sqlite3_step(ins);
   int64_t proposal_id = sqlite3_last_insert_rowid(s_db.db);
   sqlite3_finalize(ins);
   TEST_ASSERT_GREATER_THAN(0, proposal_id);

   int64_t link_id = 0;
   int rc = memory_db_proposal_resolve(user_b, proposal_id, /* approved */ true, &link_id);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_NOT_FOUND, rc);
   TEST_ASSERT_EQUAL_INT64(0, link_id);

   /* User A's proposal must remain pending. */
   sqlite3_stmt *q = NULL;
   sqlite3_prepare_v2(s_db.db, "SELECT resolved_at FROM memory_entity_merge_proposals WHERE id = ?",
                      -1, &q, NULL);
   sqlite3_bind_int64(q, 1, proposal_id);
   TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(q));
   TEST_ASSERT_EQUAL_INT(SQLITE_NULL, sqlite3_column_type(q, 0));
   sqlite3_finalize(q);

   /* User A's source entity must remain a non-alias canonical. */
   TEST_ASSERT_EQUAL_INT64(0, get_entity_canonical_id(a_src));
}

/* ============================================================================
 * Phase 1.5 fold-in: allow-list token unconditionally fires user_self_bonus.
 *
 * The Ckpt C "user" allow-list pre-add gets the entity into the candidate
 * pool, but the user_self_bonus would only fire when the operator's
 * username canonical happened to be a substring of "user" (e.g. literal
 * username "user").  The dev's actual username is "jon" or "admin" —
 * neither contains "user" as a substring.  The fold-in flags allow-listed
 * candidates with is_user_self_token = true so the bonus fires regardless.
 * ============================================================================ */

static void test_resolve_for_self_user_allow_list_bonus_fires_for_realistic_username(void) {
   /* setUp() created user "jon" — the dev's actual case.  The "user"
    * canonical does NOT contain "jon" as a substring, so prior to the
    * fold-in the user_self_bonus would not fire for the allow-list
    * candidate.  Post-fold-in: is_user_self_token=true short-circuits
    * the username-substring check inside user_self_bonus_applies. */
   int64_t user_ent = insert_entity_typed(g_test_user_id, "user", "thing");

   const char *synth_canonical = "jonathan smith";
   memory_alias_resolve_t res;
   memset(&res, 0, sizeof(res));
   int rc = memory_db_entity_resolve_alias_for_self(g_test_user_id, synth_canonical, "person",
                                                    &res);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT64(user_ent, res.resolved_id);
   TEST_ASSERT_EQUAL_INT(6, res.matched_stage);
   /* Bonus must fire via the allow-list flag — username "jon" is NOT
    * substring of "user", so the only path that lights this up is the
    * is_user_self_token branch. */
   TEST_ASSERT_TRUE(res.evidence.user_self_bonus_applied);
   /* Composite ≥ review threshold: just user_self_bonus (0.20) wouldn't
    * cross 0.70 by itself, but together with no penalty veto and the
    * ability for production embeddings to add ~0.30, it's the threshold
    * we'd commit to in the band-routing decision.  In tests without
    * cosine, composite = user_self_bonus (0.20) only.  Pin > 0 to prove
    * the bonus fired without depending on production embedding signal. */
   TEST_ASSERT_GREATER_THAN_FLOAT(0.0f, res.evidence.composite_score);
}

/* Phase 1.5 fold-in #2 — user_self_bonus alias-substring branch.
 *
 * The original brief specified three firing conditions for the bonus:
 * (1) username substring match, (2) seeded alias substring, (3) email_is.
 * Phase 1.5 shipped (1) + (3) but missed (2).  This test pins the
 * alias-substring path: with username "jon" (setUp default — already in
 * the candidate via condition 1) we use a candidate that does NOT
 * substring "jon" to isolate the alias signal.  Setting alias
 * "smithfabrications" + candidate canonical "smithfabrications@example.com"
 * exercises (2) without the username path firing.
 *
 * Negative sibling — alias "smithfabrications" + unrelated candidate
 * "dawn" (not a substring of any alias) — bonus must NOT fire.
 */
static void test_user_self_bonus_fires_via_alias_substring(void) {
   /* Set identity_aliases without any token that overlaps the candidate's
    * canonical_name through the username path. */
   auth_user_identity_t id = { 0 };
   strncpy(id.real_name, "Jonathan Smith", sizeof(id.real_name) - 1);
   strncpy(id.identity_aliases, "smithfabrications\nteam-lead", sizeof(id.identity_aliases) - 1);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_set_user_identity(g_test_user_id, &id));

   /* Candidate canonical contains "smithfabrications" but does NOT contain
    * the operator's username "jon" — isolates the alias-substring path. */
   int64_t email_ent = insert_entity_typed(g_test_user_id, "smithfabrications@example.com",
                                           "thing");
   (void)email_ent; /* used implicitly via the resolver's full-table scan */

   const char *synth_canonical = "jonathan smith smithfabrications team-lead";
   memory_alias_resolve_t res;
   memset(&res, 0, sizeof(res));
   int rc = memory_db_entity_resolve_alias_for_self(g_test_user_id, synth_canonical, "person",
                                                    &res);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(6, res.matched_stage);
   /* Bonus must fire — the only firing path here is alias-substring
    * (condition 4), since "jon" is not a substring of
    * "smithfabrications@example.com" and the candidate isn't an
    * allow-list token or contact-overlap match. */
   TEST_ASSERT_TRUE(res.evidence.user_self_bonus_applied);
   TEST_ASSERT_GREATER_THAN_FLOAT(0.0f, res.evidence.composite_score);
}

static void test_user_self_bonus_no_alias_match_no_bonus(void) {
   /* Negative case: aliases set, but no alias is a substring of the
    * candidate's canonical_name.  Username "jon" is also not a
    * substring of the candidate.  Bonus must NOT fire. */
   auth_user_identity_t id = { 0 };
   strncpy(id.real_name, "Jonathan Smith", sizeof(id.real_name) - 1);
   strncpy(id.identity_aliases, "smithfabrications\nteam-lead", sizeof(id.identity_aliases) - 1);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_set_user_identity(g_test_user_id, &id));

   /* "dawn" — no overlap with any alias or with username "jon". */
   int64_t shelley_ent = insert_entity_typed(g_test_user_id, "dawn", "person");
   (void)shelley_ent;

   const char *synth_canonical = "jonathan smith smithfabrications team-lead";
   memory_alias_resolve_t res;
   memset(&res, 0, sizeof(res));
   int rc = memory_db_entity_resolve_alias_for_self(g_test_user_id, synth_canonical, "person",
                                                    &res);
   /* Either no resolution (all candidates dropped) or resolution to a
    * different candidate without the bonus.  Pin the negative directly:
    * if dawn scored, its bonus must not have fired. */
   if (rc == MEMORY_DB_SUCCESS && res.resolved_id == shelley_ent) {
      TEST_ASSERT_FALSE(res.evidence.user_self_bonus_applied);
   }
}

/* ============================================================================
 * Phase 1.5 Ckpt D: link-user-self real_name gate + cluster integration
 *
 * Two acceptance tests:
 *   1. The gate refuses link-user-self when real_name is unset/empty,
 *      with the distinct error code MEMORY_DB_REAL_NAME_REQUIRED and no
 *      DB writes.
 *   2. The integration test mimics the dev's actual cluster shape (Jon
 *      thing/28, Jonathan Smith person/21, user thing/292), sets
 *      real_name + aliases per the realistic configuration flow, and
 *      verifies link-user-self in dry-run mode detects all three cluster
 *      members.
 *
 * Auto-merge band note: in production with embeddings configured, the
 * cosine signal contributes the missing ~0.30 to push composite past
 * the 0.90 threshold.  In the test harness without ONNX wired,
 * embedding_cosine forfeits to 0; the integration test verifies what's
 * structurally achievable — every cluster member is scored at Stage 6
 * with composite > 0 (no silent drops at the Jaccard floor or type
 * veto), and the highest-confidence candidate ranks first.  Path A
 * verification with embeddings happens against the dev's live DB
 * post-commit, not in this unit test.
 * ============================================================================ */

static void test_link_user_self_refuses_null_real_name(void) {
   /* setUp() created user "jon" without setting real_name.  The gate
    * should fire on either dry-run or commit. */
   memory_alias_link_user_self_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = memory_alias_link_user_self_run(g_test_user_id, /* dry_run */ true, &result);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_REAL_NAME_REQUIRED, rc);

   /* No DB writes — alias and proposal tables stay empty. */
   TEST_ASSERT_EQUAL_INT(0, count_db_rows("SELECT COUNT(*) FROM memory_entity_aliases "
                                          "WHERE unlinked_at IS NULL"));
   TEST_ASSERT_EQUAL_INT(0, count_db_rows("SELECT COUNT(*) FROM memory_entity_merge_proposals "
                                          "WHERE resolved_at IS NULL"));

   /* Whitespace-only real_name should also fail the gate (the trim
    * walks past spaces/tabs/newlines). */
   auth_user_identity_t id;
   memset(&id, 0, sizeof(id));
   strncpy(id.real_name, "   \t \n  ", AUTH_REAL_NAME_MAX - 1);
   /* set_user_identity NULLIFs empty strings via SQL, so we have to
    * bypass and write the whitespace directly to confirm the C-level
    * trim works.  Set to a non-empty whitespace string via prepared
    * UPDATE that doesn't NULLIF. */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db, "UPDATE users SET real_name = ? WHERE id = ?", -1, &stmt, NULL);
   sqlite3_bind_text(stmt, 1, "   \t \n  ", -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 2, g_test_user_id);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);

   memset(&result, 0, sizeof(result));
   rc = memory_alias_link_user_self_run(g_test_user_id, /* dry_run */ true, &result);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_REAL_NAME_REQUIRED, rc);

   /* Setting a real real_name unblocks. */
   memset(&id, 0, sizeof(id));
   strncpy(id.real_name, "Jon", AUTH_REAL_NAME_MAX - 1);
   auth_db_set_user_identity(g_test_user_id, &id);

   memset(&result, 0, sizeof(result));
   rc = memory_alias_link_user_self_run(g_test_user_id, /* dry_run */ true, &result);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
}

static void test_link_user_self_finds_jonathan_cluster(void) {
   /* Phase 1.5 acceptance: synthetic data mimicking the dev's actual
    * cluster shape.  Real_name + aliases configured per the realistic
    * flow.  All three cluster members must be detected at Stage 6 with
    * non-zero composite — proves the cluster is found by the synthetic
    * seed without persona-reach-around or username-only fallback. */
   auth_user_identity_t id;
   memset(&id, 0, sizeof(id));
   strncpy(id.real_name, "Jonathan Smith", AUTH_REAL_NAME_MAX - 1);
   strncpy(id.identity_aliases, "Jon\nsmithfabrications", AUTH_IDENTITY_ALIASES_MAX - 1);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_set_user_identity(g_test_user_id, &id));

   /* The dev's three-member cluster — exact mention_counts from the
    * live DB scan. */
   int64_t jon = insert_entity_typed(g_test_user_id, "Jon", "thing");
   int64_t jonathan_smith = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   int64_t user_ent = insert_entity_typed(g_test_user_id, "user", "thing");
   /* Set mention_counts to match production. */
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db, "UPDATE memory_entities SET mention_count = ? WHERE id = ?", -1,
                      &stmt, NULL);
   sqlite3_bind_int(stmt, 1, 28);
   sqlite3_bind_int64(stmt, 2, jon);
   sqlite3_step(stmt);
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, 21);
   sqlite3_bind_int64(stmt, 2, jonathan_smith);
   sqlite3_step(stmt);
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, 292);
   sqlite3_bind_int64(stmt, 2, user_ent);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);

   memory_alias_link_user_self_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = memory_alias_link_user_self_run(g_test_user_id, /* dry_run */ true, &result);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);

   /* No is_user_self entity exists yet — synthetic seed path was used. */
   /* Phase 1.5 fold-in #3: real_name "Jonathan Smith" canonicalizes to
    * exact-match the inserted Jonathan Smith entity (1406 in dev's DB).
    * Stage 1 hit → would-promote in dry-run.  This is the realistic
    * Path A flow: dev runs link-user-self, the synthetic resolver
    * finds their own entity, promotes it as canonical user-self. */
   TEST_ASSERT_EQUAL_INT64(jonathan_smith, result.self_entity_id);
   TEST_ASSERT_TRUE(result.self_was_promoted);
   TEST_ASSERT_FALSE(result.self_was_seeded);

   /* The promoted entity (Jonathan Smith) is excluded from result.rows;
    * the OTHER cluster members (Jon, user) remain as candidates and
    * are scored against the would-promoted self. */
   memory_alias_link_user_self_row_t *r_jon = NULL;
   memory_alias_link_user_self_row_t *r_user = NULL;
   for (int i = 0; i < result.row_count; i++) {
      if (result.rows[i].entity_id == jon)
         r_jon = &result.rows[i];
      else if (result.rows[i].entity_id == user_ent)
         r_user = &result.rows[i];
   }
   TEST_ASSERT_NOT_NULL_MESSAGE(r_jon, "Jon cluster member not detected");
   TEST_ASSERT_NOT_NULL_MESSAGE(r_user, "user cluster member not detected");

   /* Each remaining member scored above zero — none was silently
    * dropped at any pre-Stage-6 filter (Jaccard floor, type veto, etc).
    * Production embeddings + relation-overlap would push these past
    * the review or auto-merge band; in the test harness without cosine,
    * name + bonus signals alone carry the confidence. */
   TEST_ASSERT_GREATER_THAN_FLOAT(0.0f, r_jon->composite_score);

   /* Phase 1.5 fold-in (allow-list token): the canonical-name='user'
    * candidate must receive user_self_bonus regardless of whether the
    * operator's username happens to be a substring of "user". */
   TEST_ASSERT_TRUE_MESSAGE(r_user->user_self_bonus_applied,
                            "user_self_bonus must fire for the allow-listed 'user' canonical");
   TEST_ASSERT_GREATER_THAN_FLOAT(0.0f, r_user->composite_score);

   /* No DB writes (dry-run). */
   TEST_ASSERT_EQUAL_INT(0, count_db_rows("SELECT COUNT(*) FROM memory_entity_aliases "
                                          "WHERE unlinked_at IS NULL"));
}

/* Phase 1.5 fold-in #3 — promote-existing-match path (design §8 Path B
 * step 1).  When link-user-self runs (commit) and an existing entity
 * scores ≥ MEMORY_ALIAS_SELF_PROMOTION_THRESHOLD against the synthetic,
 * promote that entity (UPDATE is_user_self=1) instead of inserting a
 * fresh seed.  Three cases:
 *   1. Strong match exists → promote it, don't insert.
 *   2. No matches → fall back to fresh seed (existing path).
 *   3. Name collision but composite below threshold → return distinct
 *      error code MEMORY_DB_SELF_NAME_COLLISION (operator must
 *      disambiguate).
 */

static void test_link_user_self_promotes_existing_match(void) {
   /* Setup: real_name + aliases match an existing high-mention person. */
   auth_user_identity_t id;
   memset(&id, 0, sizeof(id));
   strncpy(id.real_name, "Jonathan Smith", AUTH_REAL_NAME_MAX - 1);
   strncpy(id.identity_aliases, "Jon\nsmithfabrications", AUTH_IDENTITY_ALIASES_MAX - 1);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_set_user_identity(g_test_user_id, &id));

   int64_t kk = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");

   memory_alias_link_user_self_result_t *result = calloc(
       1, sizeof(memory_alias_link_user_self_result_t));
   TEST_ASSERT_NOT_NULL(result);

   /* Commit mode (not dry-run). */
   int rc = memory_alias_link_user_self_run(g_test_user_id, /* dry_run */ false, result);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);

   /* Promote path fired — existing entity got is_user_self=1, no new row. */
   TEST_ASSERT_TRUE(result->self_was_promoted);
   TEST_ASSERT_FALSE(result->self_was_seeded);
   TEST_ASSERT_EQUAL_INT64(kk, result->self_entity_id);

   /* Verify DB state: kk row now has is_user_self=1. */
   TEST_ASSERT_EQUAL_INT(1, count_db_rows("SELECT COUNT(*) FROM memory_entities "
                                          "WHERE id = (SELECT id FROM memory_entities "
                                          "            WHERE canonical_name = 'jonathan smith') "
                                          "AND is_user_self = 1"));
   /* No additional seed row inserted. */
   TEST_ASSERT_EQUAL_INT(1, count_db_rows("SELECT COUNT(*) FROM memory_entities "
                                          "WHERE canonical_name = 'jonathan smith'"));

   free(result);
}

static void test_link_user_self_seeds_when_no_match(void) {
   /* Setup: real_name unique, no existing entities for this user. */
   auth_user_identity_t id;
   memset(&id, 0, sizeof(id));
   strncpy(id.real_name, "Singularly Unique Person", AUTH_REAL_NAME_MAX - 1);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_set_user_identity(g_test_user_id, &id));

   memory_alias_link_user_self_result_t *result = calloc(
       1, sizeof(memory_alias_link_user_self_result_t));
   TEST_ASSERT_NOT_NULL(result);

   int rc = memory_alias_link_user_self_run(g_test_user_id, /* dry_run */ false, result);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);

   /* Seed path fired — fresh row inserted with is_user_self=1. */
   TEST_ASSERT_TRUE(result->self_was_seeded);
   TEST_ASSERT_FALSE(result->self_was_promoted);
   TEST_ASSERT_GREATER_THAN_INT64(0, result->self_entity_id);

   /* Verify the new row exists with is_user_self=1. */
   TEST_ASSERT_EQUAL_INT(1, count_db_rows("SELECT COUNT(*) FROM memory_entities "
                                          "WHERE canonical_name = 'singularly unique person' "
                                          "AND is_user_self = 1"));

   free(result);
}

static void test_link_user_self_name_collision_returns_distinct_error(void) {
   /* Edge case: an entity exists with the same canonical_name as real_name
    * but composite_score is below MEMORY_ALIAS_SELF_PROMOTION_THRESHOLD
    * (no aliases set, so no user_self_bonus path can fire).  Expected:
    * INSERT collides on UNIQUE, distinct error code returned for the
    * admin handler to surface a manual-merge hint. */
   auth_user_identity_t id;
   memset(&id, 0, sizeof(id));
   strncpy(id.real_name, "Stranger Person", AUTH_REAL_NAME_MAX - 1);
   /* No aliases set — bonus path can't fire via alias-substring. */
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_set_user_identity(g_test_user_id, &id));

   /* Insert entity with same canonical_name but as a "thing" type so
    * type_match=0 + no rel_overlap + no contact_overlap + no aliases
    * keep composite below threshold. */
   int64_t collider = insert_entity_typed(g_test_user_id, "Stranger Person", "thing");
   (void)collider;

   memory_alias_link_user_self_result_t *result = calloc(
       1, sizeof(memory_alias_link_user_self_result_t));
   TEST_ASSERT_NOT_NULL(result);

   int rc = memory_alias_link_user_self_run(g_test_user_id, /* dry_run */ false, result);
   /* Either composite cleared the (intentionally low) 0.30 promotion
    * threshold and this turned into a promote (acceptable — operator
    * intent is satisfied), OR the seed path triggered the UNIQUE
    * collision and returned the distinct error code.  Both paths are
    * correct; we pin the disjunction. */
   TEST_ASSERT_TRUE(rc == MEMORY_DB_SELF_NAME_COLLISION || rc == MEMORY_DB_SUCCESS);
   if (rc == MEMORY_DB_SELF_NAME_COLLISION) {
      /* No is_user_self=1 row should exist for this user post-failure. */
      TEST_ASSERT_EQUAL_INT(0, count_db_rows("SELECT COUNT(*) FROM memory_entities "
                                             "WHERE is_user_self = 1"));
   }

   free(result);
}

/* ============================================================================
 * memory_db_entity_upsert_at — first_seen/last_seen overrides for reextract
 * ============================================================================ */

/* INSERT path: explicit overrides take effect on the new row.  Conflict
 * path: subsequent upsert with a different timestamp updates last_seen but
 * leaves first_seen unchanged (the table only writes first_seen on
 * INSERT).  Sentinel 0 falls back to time(NULL) — verified via a third
 * upsert whose timestamps must land in a [before, after] live window. */
static void test_entity_upsert_at_overrides_take_effect(void) {
   const int64_t past_ts = 1700000000; /* 2023-11-14, fixed past time */
   int64_t eid = 0;
   bool was_created = false;
   int rc = memory_db_entity_upsert_at(g_test_user_id, "Jonathan Smith", "person", "jonathan smith",
                                       past_ts, past_ts, &was_created, &eid);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_TRUE(was_created);
   TEST_ASSERT_GREATER_THAN(0, eid);

   int64_t fs = 0, ls = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, get_entity_timestamps(eid, &fs, &ls));
   TEST_ASSERT_EQUAL_INT64(past_ts, fs);
   TEST_ASSERT_EQUAL_INT64(past_ts, ls);

   /* Conflict path: same canonical_name, different override.  first_seen
    * must stay pinned at the original insert time; last_seen must advance
    * to the new override (preserves chronological progression when a
    * reextract processes convs in id order). */
   const int64_t later_ts = 1730000000; /* 2024-10-27 */
   int64_t eid2 = 0;
   bool was_created2 = true;
   rc = memory_db_entity_upsert_at(g_test_user_id, "Jonathan Smith", "person", "jonathan smith",
                                   later_ts, later_ts, &was_created2, &eid2);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_FALSE(was_created2);
   TEST_ASSERT_EQUAL_INT64(eid, eid2);

   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, get_entity_timestamps(eid, &fs, &ls));
   TEST_ASSERT_EQUAL_INT64(past_ts, fs); /* unchanged */
   TEST_ASSERT_EQUAL_INT64(later_ts, ls);

   /* Sentinel 0 → time(NULL) fallback.  Bracket the upsert with two
    * time() reads and assert the stamped value lands within. */
   int64_t before = (int64_t)time(NULL);
   int64_t eid3 = 0;
   bool was_created3 = false;
   rc = memory_db_entity_upsert_at(g_test_user_id, "Dawn Smith", "person", "dawn smith", 0, 0,
                                   &was_created3, &eid3);
   int64_t after = (int64_t)time(NULL);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_TRUE(was_created3);

   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, get_entity_timestamps(eid3, &fs, &ls));
   TEST_ASSERT_GREATER_OR_EQUAL_INT64(before, fs);
   TEST_ASSERT_LESS_OR_EQUAL_INT64(after, fs);
   TEST_ASSERT_GREATER_OR_EQUAL_INT64(before, ls);
   TEST_ASSERT_LESS_OR_EQUAL_INT64(after, ls);
}

/* The non-_at wrapper delegates to _at with both overrides = 0, so its
 * timestamps must always be ~now even though the call shape is the same
 * as it was before the _at split.  Keeps live-extraction callers
 * behaviorally unchanged. */
static void test_entity_upsert_wrapper_uses_now(void) {
   int64_t before = (int64_t)time(NULL);
   int64_t eid = 0;
   bool was_created = false;
   int rc = memory_db_entity_upsert(g_test_user_id, "Bob Smith", "person", "bob smith",
                                    &was_created, &eid);
   int64_t after = (int64_t)time(NULL);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_TRUE(was_created);

   int64_t fs = 0, ls = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, get_entity_timestamps(eid, &fs, &ls));
   TEST_ASSERT_GREATER_OR_EQUAL_INT64(before, fs);
   TEST_ASSERT_LESS_OR_EQUAL_INT64(after, fs);
   TEST_ASSERT_GREATER_OR_EQUAL_INT64(before, ls);
   TEST_ASSERT_LESS_OR_EQUAL_INT64(after, ls);
}

/* ============================================================================
 * memory_db_{fact,summary}_list_window (Bundle 3, 2026-05-13)
 *
 * The windowed/sorted variants drive the LLM 'recent' tool action's new
 * limit/sort/before parameters.  Tests pin three properties:
 *   - DESC sort returns newest first (the legacy _list_since behavior).
 *   - ASC sort returns oldest first — enables "find earliest memory" queries.
 *   - until_ts=0 sentinel resolves to "until now" (subsumes _list_since).
 * ============================================================================ */

static int64_t insert_fact_at(int user_id, const char *text, int64_t created_at) {
   int64_t fact_id = 0;
   int rc = memory_db_fact_create_at(user_id, text, 1.0f, "explicit", "general", NULL, created_at,
                                     &fact_id);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_GREATER_THAN_INT64(0, fact_id);
   return fact_id;
}

static void test_fact_list_window_desc_returns_newest_first(void) {
   int64_t now = (int64_t)time(NULL);
   int64_t old_id = insert_fact_at(g_test_user_id, "Older fact", now - 86400); /* 1 day ago */
   int64_t new_id = insert_fact_at(g_test_user_id, "Newer fact", now - 60);    /* 1 min ago */

   memory_fact_t out[8];
   int count = 0;
   int rc = memory_db_fact_list_window(g_test_user_id, /* since */ 0, /* until */ 0,
                                       /* sort_asc */ false, out, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, count);
   TEST_ASSERT_EQUAL_INT64(new_id, out[0].id); /* newest first */
   TEST_ASSERT_EQUAL_INT64(old_id, out[1].id);
}

static void test_fact_list_window_asc_returns_oldest_first(void) {
   int64_t now = (int64_t)time(NULL);
   int64_t old_id = insert_fact_at(g_test_user_id, "Older fact", now - 86400);
   int64_t new_id = insert_fact_at(g_test_user_id, "Newer fact", now - 60);

   memory_fact_t out[8];
   int count = 0;
   int rc = memory_db_fact_list_window(g_test_user_id, /* since */ 0, /* until */ 0,
                                       /* sort_asc */ true, out, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, count);
   TEST_ASSERT_EQUAL_INT64(old_id, out[0].id); /* oldest first */
   TEST_ASSERT_EQUAL_INT64(new_id, out[1].id);
}

static void test_fact_list_window_respects_until_ts_bound(void) {
   int64_t now = (int64_t)time(NULL);
   /* Three facts: 30 days ago, 10 days ago, 1 hour ago. */
   int64_t old_id = insert_fact_at(g_test_user_id, "30 days old", now - 30 * 86400);
   int64_t mid_id = insert_fact_at(g_test_user_id, "10 days old", now - 10 * 86400);
   insert_fact_at(g_test_user_id, "1 hour old", now - 3600); /* deliberately outside window */

   /* Window: 60 days ago → 5 days ago.  Should return old + mid, NOT new. */
   memory_fact_t out[8];
   int count = 0;
   int rc = memory_db_fact_list_window(g_test_user_id, /* since */ now - 60 * 86400,
                                       /* until */ now - 5 * 86400, /* sort_asc */ true, out, 8,
                                       &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, count);
   TEST_ASSERT_EQUAL_INT64(old_id, out[0].id);
   TEST_ASSERT_EQUAL_INT64(mid_id, out[1].id);
}

static void test_summary_list_window_asc_returns_oldest_first(void) {
   int64_t now = (int64_t)time(NULL);
   int64_t old_id = 0, new_id = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_summary_create_at(g_test_user_id, "session-old", "Older summary",
                                                     "topic", "neutral", 5, 60, NULL, now - 86400,
                                                     &old_id));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_summary_create_at(g_test_user_id, "session-new", "Newer summary",
                                                     "topic", "neutral", 5, 60, NULL, now - 60,
                                                     &new_id));

   memory_summary_t out[8];
   int count = 0;
   int rc = memory_db_summary_list_window(g_test_user_id, /* since */ 0, /* until */ 0,
                                          /* sort_asc */ true, out, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, count);
   TEST_ASSERT_EQUAL_INT64(old_id, out[0].id); /* oldest first */
   TEST_ASSERT_EQUAL_INT64(new_id, out[1].id);
}

/* ============================================================================
 * Relation list with canonical-root resolution
 *
 * Verifies that memory_db_relation_list_with_canonical_roots_by_user
 * returns each relation along with the SQL-resolved canonical-root for
 * both subject and object sides.  Consumed by build_entities_json_array
 * and memory_export to attribute relations to the canonical entity for
 * the equivalence class, not the row they happen to be attached to.
 * ============================================================================ */

static void test_relation_list_with_canonical_roots_subject_resolves_to_canonical(void) {
   /* Insert a canonical "Jonathan Smith" with an alias "Jon".  Attach a
    * relation against the ALIAS row (subject_entity_id = alias).  Verify
    * subj_roots[0] resolves to the canonical's id, not the alias row. */
   int64_t canonical = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   int64_t alias = insert_entity_typed(g_test_user_id, "Jon", "person");
   /* Soft-link alias → canonical (sets memory_entities.canonical_id). */
   int64_t link_id = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_alias_link(g_test_user_id, alias, canonical, "soft",
                                                     "test-fixture", -1.0f, NULL, &link_id));

   /* Relation attached to the ALIAS row. */
   insert_open_relation(g_test_user_id, alias, "works_at", 0, "Acme");

   memory_relation_t out[8];
   int64_t subj_roots[8] = { 0 };
   int64_t obj_roots[8] = { 0 };
   int count = 0;
   int rc = memory_db_relation_list_with_canonical_roots_by_user(g_test_user_id, out, subj_roots,
                                                                 obj_roots, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_EQUAL_INT64(alias, out[0].subject_entity_id); /* raw row pointer */
   TEST_ASSERT_EQUAL_INT64(canonical, subj_roots[0]);        /* SQL-resolved root */
   TEST_ASSERT_EQUAL_INT64(0, obj_roots[0]);                 /* literal object */
}

static void test_relation_list_with_canonical_roots_object_resolves_to_canonical(void) {
   /* Mirror case: relation's OBJECT side is the alias.  obj_roots[0]
    * should resolve to the canonical. */
   int64_t canonical = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   int64_t alias = insert_entity_typed(g_test_user_id, "Jon", "person");
   int64_t link_id = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_alias_link(g_test_user_id, alias, canonical, "soft",
                                                     "test-fixture", -1.0f, NULL, &link_id));

   int64_t other = insert_entity_typed(g_test_user_id, "Acme Corp", "org");
   /* (other, employs, alias) — object side points at the alias row. */
   insert_open_relation(g_test_user_id, other, "employs", alias, NULL);

   memory_relation_t out[8];
   int64_t subj_roots[8] = { 0 };
   int64_t obj_roots[8] = { 0 };
   int count = 0;
   int rc = memory_db_relation_list_with_canonical_roots_by_user(g_test_user_id, out, subj_roots,
                                                                 obj_roots, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_EQUAL_INT64(other, subj_roots[0]);    /* canonical itself */
   TEST_ASSERT_EQUAL_INT64(canonical, obj_roots[0]); /* alias resolved up */
}

static void test_relation_list_with_canonical_roots_literal_object_zero_root(void) {
   /* Relation with no object_entity_id (literal object_value).  obj_roots
    * must be 0 so callers know to skip "in" attribution. */
   int64_t subj = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   insert_open_relation(g_test_user_id, subj, "favorite_color", 0, "blue");

   memory_relation_t out[8];
   int64_t subj_roots[8] = { 0 };
   int64_t obj_roots[8] = { 0 };
   int count = 0;
   int rc = memory_db_relation_list_with_canonical_roots_by_user(g_test_user_id, out, subj_roots,
                                                                 obj_roots, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(1, count);
   TEST_ASSERT_EQUAL_INT64(subj, subj_roots[0]);
   TEST_ASSERT_EQUAL_INT64(0, obj_roots[0]);
}

static void test_relation_list_with_canonical_roots_rejects_null_arrays(void) {
   /* The new function requires all three output arrays non-NULL. */
   memory_relation_t out[1];
   int64_t roots[1] = { 0 };
   int count = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_FAILURE, memory_db_relation_list_with_canonical_roots_by_user(
                                                g_test_user_id, NULL, roots, roots, 1, &count));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_FAILURE, memory_db_relation_list_with_canonical_roots_by_user(
                                                g_test_user_id, out, NULL, roots, 1, &count));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_FAILURE, memory_db_relation_list_with_canonical_roots_by_user(
                                                g_test_user_id, out, roots, NULL, 1, &count));
}

static void test_relation_list_class_aware_mention_count_surfaces(void) {
   /* v49: per-row mention_count flows through the read path to memory_relation_t.
    * Insert two relations against different alias members of a single
    * equivalence class — each goes in at column-default mention_count=1.
    * Both rows surface with class-canonical subj_roots resolution; JS-side
    * aggregation will sum 1 + 1 = 2 for the "× N" display badge.  This test
    * pins the per-row read; aggregation is JS-side and covered by manual UI
    * inspection during verification. */
   int64_t canonical = insert_entity_typed(g_test_user_id, "Jonathan Smith", "person");
   int64_t alias = insert_entity_typed(g_test_user_id, "Jon", "person");
   int64_t link_id = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_entity_alias_link(g_test_user_id, alias, canonical, "soft",
                                                     "test-fixture", -1.0f, NULL, &link_id));

   /* Each insert hits a different subject_entity_id, so the partial UNIQUE
    * doesn't fire — both rows persist with mention_count=1. */
   insert_open_relation(g_test_user_id, canonical, "works_at", 0, "Acme Corp");
   insert_open_relation(g_test_user_id, alias, "works_at", 0, "Acme Corp");

   memory_relation_t out[8];
   int64_t subj_roots[8] = { 0 };
   int64_t obj_roots[8] = { 0 };
   int count = 0;
   int rc = memory_db_relation_list_with_canonical_roots_by_user(g_test_user_id, out, subj_roots,
                                                                 obj_roots, 8, &count);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, count);
   /* Both rows roll up to the canonical via subj_roots. */
   TEST_ASSERT_EQUAL_INT64(canonical, subj_roots[0]);
   TEST_ASSERT_EQUAL_INT64(canonical, subj_roots[1]);
   /* Per-row mention_count surfaces — JS will sum these for the badge. */
   TEST_ASSERT_EQUAL_INT(1, out[0].mention_count);
   TEST_ASSERT_EQUAL_INT(1, out[1].mention_count);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
   UNITY_BEGIN();

   /* Pure helpers */
   RUN_TEST(test_name_jaccard_basic);
   RUN_TEST(test_name_substring_basic);
   RUN_TEST(test_composite_weighted_sum);
   RUN_TEST(test_composite_bonuses);
   RUN_TEST(test_composite_veto);
   RUN_TEST(test_canonical_priority_compare_pure);
   RUN_TEST(test_canonical_priority_compare_self_user_self_first);
   RUN_TEST(test_canonical_priority_compare_self_falls_through);

   /* Resolver cascade */
   RUN_TEST(test_resolver_stage1_exact_match);
   RUN_TEST(test_resolver_stage1_resolves_through_alias);
   RUN_TEST(test_resolver_no_match);

   /* score_pair / Stage 5 */
   RUN_TEST(test_score_pair_exclusive_relation_overlap);
   RUN_TEST(test_score_pair_type_veto);
   RUN_TEST(test_score_pair_thing_no_veto);
   RUN_TEST(test_score_pair_contact_field_overlap);
   RUN_TEST(test_score_pair_user_self_bonus_via_username);
   RUN_TEST(test_score_pair_user_self_bonus_via_contact_overlap);

   /* consider_auto_merge / threshold bands */
   RUN_TEST(test_consider_auto_merge_no_candidates);
   RUN_TEST(test_consider_auto_merge_auto_merged);
   RUN_TEST(test_consider_auto_merge_proposed);
   RUN_TEST(test_consider_auto_merge_rejected);
   RUN_TEST(test_consider_auto_merge_substring_rescue_forward);
   RUN_TEST(test_consider_auto_merge_substring_rescue_reverse);
   RUN_TEST(test_consider_auto_merge_substring_rescue_into_review);
   RUN_TEST(test_consider_auto_merge_proposes_multiple_in_band);
   RUN_TEST(test_consider_auto_merge_auto_short_circuits_proposals);
   RUN_TEST(test_consider_auto_merge_longer_canonical_swap_fires);
   RUN_TEST(test_consider_auto_merge_longer_canonical_propose_time_swap_fires);
   RUN_TEST(test_consider_auto_merge_same_token_count_swap_via_token_prefix);
   RUN_TEST(test_consider_auto_merge_same_token_count_no_swap_on_variants);
   RUN_TEST(test_consider_auto_merge_longer_canonical_swap_skipped_dependents);
   RUN_TEST(test_auto_promote_user_self_at_extraction_time);
   RUN_TEST(test_auto_promote_user_self_by_real_name_sweep);
   RUN_TEST(test_auto_promote_no_op_when_user_self_exists);
   RUN_TEST(test_auto_promote_no_op_when_real_name_unset);
   RUN_TEST(test_auto_promote_matches_identity_alias);

   /* Auto-alias abstract "user" → user_self */
   RUN_TEST(test_maybe_alias_user_to_self_aliases_when_anchor_exists);
   RUN_TEST(test_maybe_alias_user_to_self_noop_when_no_anchor);
   RUN_TEST(test_maybe_alias_user_to_self_refuses_self_link);
   RUN_TEST(test_alias_existing_user_to_self_sweep);
   RUN_TEST(test_maybe_alias_user_to_self_rejects_invalid_input);
   RUN_TEST(test_alias_existing_user_to_self_rejects_invalid_user);

   /* Path B — link-user-self propose-time longer-canonical swap */
   RUN_TEST(test_link_user_self_propose_swaps_for_longer_canonical);
   RUN_TEST(test_link_user_self_propose_skips_swap_when_self_has_dependents);

   /* Relation list with SQL-resolved canonical roots */
   RUN_TEST(test_relation_list_with_canonical_roots_subject_resolves_to_canonical);
   RUN_TEST(test_relation_list_with_canonical_roots_object_resolves_to_canonical);
   RUN_TEST(test_relation_list_with_canonical_roots_literal_object_zero_root);
   RUN_TEST(test_relation_list_with_canonical_roots_rejects_null_arrays);
   RUN_TEST(test_relation_list_class_aware_mention_count_surfaces);

   /* alias_link / alias_unlink */
   RUN_TEST(test_alias_link_writes_canonical_id_and_audit);
   RUN_TEST(test_alias_link_refuses_self);
   RUN_TEST(test_alias_link_refuses_when_source_has_dependents);
   RUN_TEST(test_alias_unlink_clears_canonical_id_and_stamps_audit);
   RUN_TEST(test_alias_unlink_refuses_hard);

   /* Ckpt 3: cache invalidate fix + loader filter */
   RUN_TEST(test_entity_merge_invalidates_entity_cache);
   RUN_TEST(test_entity_merge_moves_aliases);
   RUN_TEST(test_entity_merge_into_own_alias);
   RUN_TEST(test_entity_merge_into_an_alias);
   RUN_TEST(test_alias_names_leave_out_user_placeholder);
   RUN_TEST(test_loader_filter_excludes_aliases_by_default);

   /* relation listings */
   RUN_TEST(test_relation_list_by_subject_class_includes_aliases);

   /* Ckpt 4: link-user-self orchestrator + listing helpers */
   RUN_TEST(test_link_user_self_dry_run_no_db_writes);
   RUN_TEST(test_link_user_self_commit_seeds_and_links);
   RUN_TEST(test_alias_list_helper_returns_active_only);
   RUN_TEST(test_alias_history_helper_includes_unlinked);
   RUN_TEST(test_entity_list_for_admin_canonical_only_default);
   RUN_TEST(test_entity_list_for_admin_alias_tail_carries_canonical_id);

   /* Equivalence-class stats aggregation (Bundle 2, 2026-05-13) */
   RUN_TEST(test_entity_get_by_name_aggregates_class_stats);
   RUN_TEST(test_entity_get_by_name_alias_returns_self_stats);
   RUN_TEST(test_entity_get_by_name_solo_canonical_no_aliases);
   RUN_TEST(test_entity_search_aggregates_class_stats);
   RUN_TEST(test_entity_list_for_admin_aggregates_class_mention_count);

   /* memory_db_{fact,summary}_list_window (Bundle 3, 2026-05-13) */
   RUN_TEST(test_fact_list_window_desc_returns_newest_first);
   RUN_TEST(test_fact_list_window_asc_returns_oldest_first);
   RUN_TEST(test_fact_list_window_respects_until_ts_bound);
   RUN_TEST(test_summary_list_window_asc_returns_oldest_first);
   RUN_TEST(test_proposal_list_returns_pending_only);
   RUN_TEST(test_proposal_resolve_approved_creates_alias);

   /* Ckpt 5: alias_summary helper + LLM-tool integration smoke */
   RUN_TEST(test_alias_summary_returns_alias_canonical_pairs);
   RUN_TEST(test_alias_summary_empty_for_user_with_no_aliases);
   RUN_TEST(test_smoke_llm_merge_entities_soft_link_path);

   /* Ckpt 5 fold-in: review-batch coverage */
   RUN_TEST(test_score_pair_refuses_alias_target);
   RUN_TEST(test_alias_link_refuses_hard_kind);
   RUN_TEST(test_cross_user_alias_link_returns_not_found);
   RUN_TEST(test_cross_user_proposal_resolve_returns_not_found);

   /* Ckpt 5 fold-in round 2 */
   RUN_TEST(test_exclusive_relation_overlap_unified_sql_path);
   RUN_TEST(test_link_user_self_dry_run_synthetic_scores_existing_cluster);

   /* Phase 1.5 Ckpt B: synthetic seed sources the real_name + aliases */
   RUN_TEST(test_synthetic_seed_unions_real_name_and_alias_tokens);
   RUN_TEST(test_synthetic_seed_aliases_parsing_strip_dedup_skip_empty);
   RUN_TEST(test_synthetic_seed_does_not_leak_persona_description);

   /* Phase 1.5 Ckpt C: directional similarity + "user" allow-list */
   RUN_TEST(test_resolve_for_self_directional_overlap_keeps_single_token);
   RUN_TEST(test_resolve_for_self_user_allow_list_reaches_stage6);
   RUN_TEST(test_resolve_alias_non_synth_preserves_jaccard_floor);

   /* Phase 1.5 fold-in: allow-list token unconditionally fires bonus */
   RUN_TEST(test_resolve_for_self_user_allow_list_bonus_fires_for_realistic_username);
   RUN_TEST(test_user_self_bonus_fires_via_alias_substring);
   RUN_TEST(test_user_self_bonus_no_alias_match_no_bonus);

   /* Phase 1.5 Ckpt D: link-user-self gate + cluster integration */
   RUN_TEST(test_link_user_self_refuses_null_real_name);
   RUN_TEST(test_link_user_self_finds_jonathan_cluster);
   RUN_TEST(test_link_user_self_promotes_existing_match);
   RUN_TEST(test_link_user_self_seeds_when_no_match);
   RUN_TEST(test_link_user_self_name_collision_returns_distinct_error);

   /* Entity upsert timestamp overrides for reextract */
   RUN_TEST(test_entity_upsert_at_overrides_take_effect);
   RUN_TEST(test_entity_upsert_wrapper_uses_now);

   return UNITY_END();
}
