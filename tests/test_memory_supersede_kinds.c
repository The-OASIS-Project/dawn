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
 * Unit tests, on a real schema, for merged and replaced facts: a merged fact
 * is the same fact as the one kept, a replaced fact is its history.  Merge,
 * replace, forget (one fact or a conversation), restore and the prunes each
 * keep that.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "memory/memory_db.h"
#include "memory/memory_db_entities.h"
#include "memory/memory_db_provenance.h"
#include "unity.h"

static int s_user;
static int64_t s_ent;

void setUp(void) {
   config_set_defaults(&g_config);
   g_config.memory.enabled = true;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_init(":memory:"));
   auth_db_create_user("keeper", "hash", false);
   auth_user_t user;
   memset(&user, 0, sizeof(user));
   auth_db_get_user("keeper", &user);
   s_user = user.id;
   bool created = false;
   memory_db_entity_upsert(s_user, "pat", "person", "pat", &created, &s_ent);
}

void tearDown(void) {
   auth_db_shutdown();
}

static int64_t conv(void) {
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, conv_db_create(s_user, "Chat", &id));
   return id;
}

/* A fact learned in @p conv_id (0: none). */
static int64_t fact(const char *text, int64_t conv_id) {
   memory_provenance_t prov = { .conv_id = conv_id, .msg_id_start = 1, .msg_id_end = 1 };
   int64_t id = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_fact_create(s_user, text, 0.9f, "explicit", NULL, &prov, &id));
   return id;
}

/* An open relation pat @p relation @p value, stated by @p fact_id. */
static int64_t rel(const char *relation, const char *value, int64_t fact_id) {
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_relation_create(s_user, s_ent, relation, 0, value, fact_id, 0.9f,
                                                   0, 0, NULL));
   sqlite3_stmt *st = NULL;
   sqlite3_prepare_v2(s_db.db, "SELECT MAX(id) FROM memory_relations", -1, &st, NULL);
   sqlite3_step(st);
   const int64_t id = sqlite3_column_int64(st, 0);
   sqlite3_finalize(st);
   return id;
}

static int64_t q(const char *sql, int64_t id) {
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL));
   sqlite3_bind_int64(st, 1, id);
   const int64_t v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
   sqlite3_finalize(st);
   return v;
}

static void exec(const char *sql, int64_t id) {
   sqlite3_stmt *st = NULL;
   TEST_ASSERT_EQUAL_INT(SQLITE_OK, sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL));
   sqlite3_bind_int64(st, 1, id);
   TEST_ASSERT_EQUAL_INT(SQLITE_DONE, sqlite3_step(st));
   sqlite3_finalize(st);
}

/* -1 when the fact is gone, 0 when current. */
static int64_t sup(int64_t id) {
   return q("SELECT COALESCE(superseded_by, 0) FROM memory_facts WHERE id = ?", id);
}
static int64_t kind(int64_t id) {
   return q("SELECT superseded_kind FROM memory_facts WHERE id = ?", id);
}
static bool open_rel(int64_t id) {
   return q("SELECT valid_to IS NULL FROM memory_relations WHERE id = ?", id) == 1;
}
static int64_t closed_by(int64_t id) {
   return q("SELECT COALESCE(closed_by_fact_id, 0) FROM memory_relations WHERE id = ?", id);
}
static int64_t rel_fact(int64_t id) {
   return q("SELECT COALESCE(fact_id, 0) FROM memory_relations WHERE id = ?", id);
}

static memory_forget_result_t forget(int64_t id) {
   memory_forget_result_t r = { 0 };
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_forget(id, s_user, &r));
   return r;
}

/* Only current facts are superseded, and only onto current facts. */
void test_supersede_refuses_what_is_not_current(void) {
   const int64_t a = fact("Pat's desk is by the window", 0);
   const int64_t b = fact("Pat sits by the window", 0);
   const int64_t c = fact("Pat's desk faces the window", 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_merge(a, b, s_user));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_NOT_FOUND, memory_db_fact_merge(c, a, s_user));   /* kept */
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_NOT_FOUND, memory_db_fact_replace(a, c, s_user)); /* old */
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_NOT_FOUND, memory_db_fact_merge(b, a, s_user));   /* cycle */
   TEST_ASSERT_EQUAL_INT64(b, sup(a));
   TEST_ASSERT_EQUAL_INT64(0, sup(b));
   TEST_ASSERT_EQUAL_INT64(0, sup(c));
}

/* Replace closes what the old value stated, by the newer one. */
void test_replace_closes_all_old_relations(void) {
   const int64_t old = fact("Pat works at Acme in Denver", 0);
   const int64_t r1 = rel("works_at", "Acme", old);
   const int64_t r2 = rel("lives_in", "Denver", old);
   const int64_t newer = fact("Pat works at Initech in Austin", 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_replace(old, newer, s_user));
   TEST_ASSERT_EQUAL_INT64(newer, sup(old));
   TEST_ASSERT_EQUAL_INT64(MEMORY_SUPERSEDED_REPLACED, kind(old));
   TEST_ASSERT_FALSE(open_rel(r1));
   TEST_ASSERT_FALSE(open_rel(r2));
   TEST_ASSERT_EQUAL_INT64(newer, closed_by(r1));
   TEST_ASSERT_EQUAL_INT64(newer, closed_by(r2));
}

/* Merge hands the duplicate's relations and history to the fact kept. */
void test_merge_moves_relations_and_history(void) {
   const int64_t old = fact("Pat's number is 555-0101", 0);
   const int64_t old_rel = rel("phone_number_is", "555-0101", old);
   const int64_t dup = fact("Pat's number is 555-0202", 0);
   const int64_t dup_rel = rel("phone_number_is", "555-0202", dup);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_replace(old, dup, s_user));
   const int64_t keep = fact("Pat can be reached at 555-0202", 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_merge(dup, keep, s_user));
   TEST_ASSERT_EQUAL_INT64(keep, sup(old));
   TEST_ASSERT_EQUAL_INT64(MEMORY_SUPERSEDED_REPLACED, kind(old));
   TEST_ASSERT_EQUAL_INT64(MEMORY_SUPERSEDED_MERGED, kind(dup));
   /* What it closed stays on it: restore and reopen look through merges. */
   TEST_ASSERT_EQUAL_INT64(dup, closed_by(old_rel));
   TEST_ASSERT_EQUAL_INT64(keep, rel_fact(dup_rel));
   TEST_ASSERT_EQUAL_INT64(dup, q("SELECT merged_from_fact_id FROM memory_relations WHERE id = ?",
                                  dup_rel));
   TEST_ASSERT_EQUAL_INT64(dup, q("SELECT merged_from_id FROM memory_facts WHERE id = ?", old));

   /* Forgetting the kept fact takes the duplicate and brings the old value back. */
   const memory_forget_result_t r = forget(keep);
   TEST_ASSERT_EQUAL_INT(1, r.merged);
   TEST_ASSERT_EQUAL_INT(1, r.restored);
   TEST_ASSERT_EQUAL_INT64(old, r.restored_ids[0]);
   TEST_ASSERT_EQUAL_INT64(-1, sup(dup));
   TEST_ASSERT_EQUAL_INT64(0, sup(old));
   TEST_ASSERT_EQUAL_INT64(MEMORY_SUPERSEDED_MERGED, kind(old));
   TEST_ASSERT_TRUE(open_rel(old_rel));
}

/* Replace keeps the chain: forgetting the newest brings back only the one it
 * replaced. */
void test_forget_newest_restores_one_step(void) {
   const int64_t v1 = fact("Pat lives in Boston", 0);
   const int64_t r1 = rel("lives_in", "Boston", v1);
   const int64_t v2 = fact("Pat lives in Chicago", 0);
   const int64_t r2 = rel("lives_in", "Chicago", v2);
   const int64_t v3 = fact("Pat lives in Seattle", 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_replace(v1, v2, s_user));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_replace(v2, v3, s_user));
   const memory_forget_result_t r = forget(v3);
   TEST_ASSERT_EQUAL_INT(1, r.restored);
   TEST_ASSERT_EQUAL_INT64(v2, r.restored_ids[0]);
   TEST_ASSERT_EQUAL_INT64(0, sup(v2));
   TEST_ASSERT_EQUAL_INT64(v2, sup(v1));
   TEST_ASSERT_TRUE(open_rel(r2));
   TEST_ASSERT_FALSE(open_rel(r1));
   TEST_ASSERT_EQUAL_INT64(v2, closed_by(r1));
}

/* Forgetting a middle value hands its history up to the value after it. */
void test_forget_middle_value_hands_up(void) {
   const int64_t v1 = fact("Pat drives a Civic", 0);
   const int64_t r1 = rel("owns_vehicle", "Civic", v1);
   const int64_t v2 = fact("Pat drives a Corolla", 0);
   const int64_t v3 = fact("Pat drives a Mazda", 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_replace(v1, v2, s_user));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_replace(v2, v3, s_user));
   const memory_forget_result_t r = forget(v2);
   TEST_ASSERT_EQUAL_INT(0, r.restored);
   TEST_ASSERT_EQUAL_INT64(v3, sup(v1));
   TEST_ASSERT_EQUAL_INT64(MEMORY_SUPERSEDED_REPLACED, kind(v1));
   TEST_ASSERT_FALSE(open_rel(r1));
   TEST_ASSERT_EQUAL_INT64(v3, closed_by(r1));
}

/* A relation-level replace leaves the old fact current (it states more); forgetting
 * the replacer reopens what it closed. */
void test_forget_reopens_a_relation_level_replace(void) {
   const int64_t f1 = fact("Pat lives in Reno and works at Acme", 0);
   const int64_t lives = rel("lives_in", "Reno", f1);
   const int64_t works = rel("works_at", "Acme", f1);
   const int64_t f2 = fact("Pat moved from Reno to Tahoe", 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_relation_supersede_ex(s_user, s_ent, "lives_in", 0, "Tahoe", f2,
                                                         0.9f, 0, 0, "Reno", NULL, NULL));
   TEST_ASSERT_FALSE(open_rel(lives));
   TEST_ASSERT_EQUAL_INT64(f2, closed_by(lives));
   TEST_ASSERT_EQUAL_INT64(0, sup(f1)); /* still backs works_at */
   const memory_forget_result_t r = forget(f2);
   TEST_ASSERT_EQUAL_INT(0, r.restored);
   TEST_ASSERT_TRUE(open_rel(lives));
   TEST_ASSERT_TRUE(open_rel(works));
}

/* A one-at-a-time relation reopens only once the value that closed it is gone. */
void test_forget_reopens_one_at_a_time_after_the_delete(void) {
   const int64_t f1 = fact("Pat is married to Vic", 0);
   const int64_t r1 = rel("married_to", "Vic", f1);
   const int64_t f2 = fact("Pat is married to Wes", 0);
   int64_t old_fact = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_relation_supersede(s_user, s_ent, "married_to", 0, "Wes", f2,
                                                      0.9f, 0, 0, NULL, &old_fact));
   TEST_ASSERT_EQUAL_INT64(f1, old_fact);
   TEST_ASSERT_EQUAL_INT64(MEMORY_SUPERSEDED_REPLACED, kind(f1));
   const memory_forget_result_t r = forget(f2);
   TEST_ASSERT_EQUAL_INT(1, r.restored);
   TEST_ASSERT_EQUAL_INT64(0, sup(f1));
   TEST_ASSERT_TRUE(open_rel(r1));
   TEST_ASSERT_EQUAL_INT64(0, q("SELECT COUNT(*) FROM memory_relations WHERE fact_id = ?", f2));
}

/* Forgetting a conversation brings back the value its fact replaced, and the
 * confirm screen's count says so beforehand. */
void test_conversation_forget_restores_and_preview_agrees(void) {
   const int64_t c_old = conv();
   const int64_t c_new = conv();
   const int64_t v1 = fact("Pat's gym is Planet Fitness", c_old);
   const int64_t r1 = rel("member_of", "Planet Fitness", v1);
   const int64_t v2 = fact("Pat's gym is Gold's", c_new);
   const int64_t dup = fact("Pat goes to Gold's gym", c_new);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_replace(v1, v2, s_user));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_merge(dup, v2, s_user));

   memory_conv_learned_t preview = { 0 };
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_conversations_learned_count(s_user, &c_new, 1, &preview));
   memory_conv_learned_t done = { 0 };
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_conversations_forget(s_user, &c_new, 1, &done));
   TEST_ASSERT_EQUAL_INT(1, preview.restored);
   TEST_ASSERT_EQUAL_INT(preview.restored, done.restored);
   TEST_ASSERT_EQUAL_INT(preview.facts, done.facts);
   TEST_ASSERT_EQUAL_INT(preview.outdated, done.outdated);
   TEST_ASSERT_EQUAL_INT64(-1, sup(v2));
   TEST_ASSERT_EQUAL_INT64(-1, sup(dup));
   TEST_ASSERT_EQUAL_INT64(0, sup(v1));
   TEST_ASSERT_EQUAL_INT64(MEMORY_SUPERSEDED_MERGED, kind(v1));
   TEST_ASSERT_TRUE(open_rel(r1));
}

/* A prune drops a fact with its whole history; nothing comes back. */
static void make_history(int64_t *head, int64_t *dup, int64_t *old) {
   *old = fact("Pat's locker is 12", 0);
   *head = fact("Pat's locker is 14", 0);
   *dup = fact("Pat uses locker 14", 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_replace(*old, *head, s_user));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_merge(*dup, *head, s_user));
}

static void assert_all_gone(int64_t head, int64_t dup, int64_t old) {
   TEST_ASSERT_EQUAL_INT64(-1, sup(head));
   TEST_ASSERT_EQUAL_INT64(-1, sup(dup));
   TEST_ASSERT_EQUAL_INT64(-1, sup(old));
}

void test_prune_low_confidence_takes_history(void) {
   int64_t head, dup, old;
   make_history(&head, &dup, &old);
   exec("UPDATE memory_facts SET confidence = 0.05 WHERE id = ?", head);
   int n = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_prune_low_confidence(s_user, 0.1f, &n));
   assert_all_gone(head, dup, old);
}

void test_prune_stale_takes_history(void) {
   int64_t head, dup, old;
   make_history(&head, &dup, &old);
   exec("UPDATE memory_facts SET confidence = 0.2, last_accessed = 1000 WHERE id = ?", head);
   int n = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_prune_stale(s_user, 30, 0.5f, &n));
   assert_all_gone(head, dup, old);
}

void test_prune_expired_takes_history(void) {
   int64_t head, dup, old;
   make_history(&head, &dup, &old);
   exec("UPDATE memory_facts SET expires_at = 1000 WHERE id = ?", head);
   int n = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_prune_expired(s_user, 0, &n));
   assert_all_gone(head, dup, old);
}

/* Past the retention, duplicates go; older values stay while their head does. */
void test_prune_superseded_keeps_replaced(void) {
   int64_t head, dup, old;
   make_history(&head, &dup, &old);
   exec("UPDATE memory_facts SET superseded_at = 1000 WHERE superseded_by = ?", head);
   int n = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_prune_superseded(s_user, 1, &n));
   TEST_ASSERT_EQUAL_INT64(-1, sup(dup));
   TEST_ASSERT_EQUAL_INT64(head, sup(old));
   TEST_ASSERT_EQUAL_INT64(0, sup(head));
}

/* Restore says a newer value stays current beside it, through a merged link too. */
void test_restore_reports_a_newer_value_through_a_merge(void) {
   const int64_t old = fact("Pat's coach is Sam", 0);
   const int64_t mid = fact("Pat's coach is Lee", 0);
   const int64_t keep = fact("Lee coaches Pat", 0);
   const int64_t dup = fact("Pat is coached by Lee", 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_replace(old, mid, s_user));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_merge(mid, keep, s_user));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_merge(dup, keep, s_user));
   memory_restore_result_t r = { 0 };
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_restore(s_user, old, &r));
   TEST_ASSERT_TRUE(r.replaced);
   memset(&r, 0, sizeof(r));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_restore(s_user, dup, &r));
   TEST_ASSERT_FALSE(r.replaced);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_NOT_FOUND, memory_db_fact_restore(s_user + 1, mid, &r));
}

/* v104 marks as replaced a fact whose relation its keeper closed; a plain
 * merge stays a merge. */
void test_v104_backfill(void) {
   const int64_t keep = fact("Pat's team is Blue", 0);
   const int64_t replaced = fact("Pat's team was Red", 0);
   const int64_t merged = fact("Pat is on team Blue", 0);
   const int64_t r = rel("member_of", "Red", replaced);
   exec("UPDATE memory_facts SET superseded_by = (SELECT MIN(id) FROM memory_facts), "
        "superseded_kind = 0 WHERE id = ?",
        replaced);
   exec("UPDATE memory_facts SET superseded_by = (SELECT MIN(id) FROM memory_facts), "
        "superseded_kind = 0 WHERE id = ?",
        merged);
   exec("UPDATE memory_relations SET valid_to = 1000, closed_by_fact_id = (SELECT MIN(id) FROM "
        "memory_facts) WHERE id = ?",
        r);
   /* An older value whose keeper an old merge hid: it moves up to the fact
    * kept, with what the merged fact closed. */
   const int64_t older = fact("Pat's team was Green", 0);
   const int64_t r_older = rel("member_of", "Green", older);
   exec("UPDATE memory_facts SET superseded_by = ?, superseded_kind = 0 WHERE id = "
        "(SELECT MAX(id) FROM memory_facts)",
        merged);
   exec("UPDATE memory_relations SET valid_to = 1000, closed_by_fact_id = (SELECT "
        "superseded_by FROM memory_facts WHERE id = (SELECT MAX(id) FROM memory_facts)) "
        "WHERE id = ?",
        r_older);
   TEST_ASSERT_EQUAL_INT64(merged, sup(older));
   TEST_ASSERT_EQUAL_INT64(keep, sup(replaced));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_migrations_v104(s_db.db));
   TEST_ASSERT_EQUAL_INT64(MEMORY_SUPERSEDED_REPLACED, kind(replaced));
   TEST_ASSERT_EQUAL_INT64(MEMORY_SUPERSEDED_MERGED, kind(merged));
   TEST_ASSERT_EQUAL_INT64(MEMORY_SUPERSEDED_REPLACED, kind(older));
   TEST_ASSERT_EQUAL_INT64(keep, sup(older));
   TEST_ASSERT_EQUAL_INT64(merged, closed_by(r_older));
   TEST_ASSERT_EQUAL_INT64(merged,
                           q("SELECT merged_from_id FROM memory_facts WHERE id = ?", older));
}

/* A legacy merge chain longer than a few links moves to its head in one go. */
void test_v104_long_chain(void) {
   enum {
      LINKS = 30
   };
   int64_t ids[LINKS + 1];
   char text[64];
   for (int i = 0; i <= LINKS; i++) {
      snprintf(text, sizeof(text), "Pat's locker number, take %d", i);
      ids[i] = fact(text, 0);
   }
   const int64_t r = rel("locker_is", "17", ids[0]);
   for (int i = 0; i < LINKS; i++) { /* the old code moved nothing on a merge */
      char sql[160];
      snprintf(sql, sizeof(sql),
               "UPDATE memory_facts SET superseded_by = %lld, superseded_kind = 0 WHERE id = ?",
               (long long)ids[i + 1]);
      exec(sql, ids[i]);
   }
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_migrations_v104(s_db.db));
   TEST_ASSERT_EQUAL_INT64(ids[LINKS], rel_fact(r));
   TEST_ASSERT_EQUAL_INT64(ids[0],
                           q("SELECT merged_from_fact_id FROM memory_relations WHERE id = ?", r));
   /* Forgetting the head takes the whole chain. */
   const memory_forget_result_t fr = forget(ids[LINKS]);
   TEST_ASSERT_EQUAL_INT(LINKS, fr.merged);
   TEST_ASSERT_EQUAL_INT64(-1, rel_fact(r));
}

/* Restoring a duplicate takes back what the merge moved: what it stated and
 * the values it replaced.  Forgetting it then takes what it stated. */
void test_restore_takes_back_what_a_merge_moved(void) {
   const int64_t keep = fact("Pat's phone is 555-0111", 0);
   const int64_t keep_rel = rel("phone_number_is", "555-0111", keep);
   const int64_t older = fact("Pat's phone was 555-0100", 0);
   const int64_t older_rel = rel("phone_number_is", "555-0100", older);
   const int64_t dup = fact("Pat's phone is 555-0222", 0);
   const int64_t dup_rel = rel("phone_number_is", "555-0222", dup);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_replace(older, dup, s_user));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_merge(dup, keep, s_user));
   TEST_ASSERT_EQUAL_INT64(keep, rel_fact(dup_rel));
   TEST_ASSERT_EQUAL_INT64(keep, sup(older));

   memory_restore_result_t rr = { 0 };
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_restore(s_user, dup, &rr));
   TEST_ASSERT_EQUAL_INT64(dup, rel_fact(dup_rel));
   TEST_ASSERT_EQUAL_INT64(0, q("SELECT COALESCE(merged_from_fact_id, 0) FROM memory_relations "
                                "WHERE id = ?",
                                dup_rel));
   TEST_ASSERT_EQUAL_INT64(dup, sup(older));
   TEST_ASSERT_EQUAL_INT64(0, q("SELECT COALESCE(merged_from_id, 0) FROM memory_facts WHERE id = ?",
                                older));

   const memory_forget_result_t r = forget(dup);
   TEST_ASSERT_EQUAL_INT(1, r.restored);
   TEST_ASSERT_EQUAL_INT64(older, r.restored_ids[0]);
   TEST_ASSERT_EQUAL_INT64(-1, rel_fact(dup_rel)); /* the forgotten number is gone */
   TEST_ASSERT_TRUE(open_rel(keep_rel));
   TEST_ASSERT_EQUAL_INT64(0, sup(keep));
   TEST_ASSERT_TRUE(open_rel(older_rel));
}

/* Restoring an older value reopens what was closed by the fact it was under,
 * even when that fact was since merged into another. */
void test_restore_reopens_through_a_merge(void) {
   const int64_t older = fact("Pat's gym was Crunch", 0);
   const int64_t older_rel = rel("member_of", "Crunch", older);
   const int64_t newer = fact("Pat's gym is Equinox", 0);
   const int64_t keep = fact("Pat goes to Equinox", 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_replace(older, newer, s_user));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_merge(newer, keep, s_user));
   TEST_ASSERT_EQUAL_INT64(keep, sup(older));
   memory_restore_result_t rr = { 0 };
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_restore(s_user, older, &rr));
   TEST_ASSERT_EQUAL_INT(1, rr.reopened);
   TEST_ASSERT_TRUE(rr.replaced);
   TEST_ASSERT_TRUE(open_rel(older_rel));
}

/* A duplicate whose kept fact was later replaced: restoring it reopens what
 * the replacement closed, as restoring the kept fact would. */
void test_restore_reopens_what_a_later_replace_closed(void) {
   const int64_t a = fact("Pat's bike is a Trek", 0);
   const int64_t r = rel("owns_vehicle", "Trek", a);
   const int64_t b = fact("Pat rides a Trek", 0);
   const int64_t c = fact("Pat rides a Specialized now", 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_merge(a, b, s_user));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_replace(b, c, s_user));
   TEST_ASSERT_FALSE(open_rel(r));
   TEST_ASSERT_EQUAL_INT64(c, closed_by(r));
   memory_restore_result_t rr = { 0 };
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_restore(s_user, a, &rr));
   TEST_ASSERT_TRUE(rr.replaced);
   TEST_ASSERT_EQUAL_INT64(a, rel_fact(r));
   TEST_ASSERT_TRUE(open_rel(r));
}

/* A fact restored on its own owes nothing to the merge that moved it:
 * restoring that duplicate later leaves it current. */
void test_restored_fact_drops_its_merge_mark(void) {
   const int64_t older = fact("Pat's dentist was Dr. Ames", 0);
   const int64_t newer = fact("Pat's dentist is Dr. Bell", 0);
   const int64_t keep = fact("Dr. Bell is Pat's dentist", 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_replace(older, newer, s_user));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_merge(newer, keep, s_user));
   memory_restore_result_t rr = { 0 };
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_restore(s_user, older, &rr));
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_restore(s_user, newer, &rr));
   TEST_ASSERT_EQUAL_INT64(0, sup(older));
   TEST_ASSERT_EQUAL_INT64(0, q("SELECT COALESCE(merged_from_id, 0) FROM memory_facts WHERE id = ?",
                                older));
}

/* A correction files the old fact as replaced but leaves what it stated open. */
void test_correction_leaves_relations_open(void) {
   const int64_t old = fact("Pat is an engineer at Acme", 0);
   const int64_t works = rel("works_at", "Acme", old);
   const int64_t fix = fact("Pat is CTO at Acme", 0);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS, memory_db_fact_correct(old, fix, s_user));
   TEST_ASSERT_EQUAL_INT64(fix, sup(old));
   TEST_ASSERT_EQUAL_INT64(MEMORY_SUPERSEDED_REPLACED, kind(old));
   TEST_ASSERT_TRUE(open_rel(works));
   const memory_forget_result_t r = forget(fix);
   TEST_ASSERT_EQUAL_INT(1, r.restored);
   TEST_ASSERT_EQUAL_INT64(0, sup(old));
   TEST_ASSERT_TRUE(open_rel(works));
}

/* The forget preview names what a forget would bring back and take. */
void test_forget_preview_matches_the_forget(void) {
   int64_t head, dup, old;
   make_history(&head, &dup, &old);
   int merged = 0;
   int back = 0;
   int64_t first = 0;
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_SUCCESS,
                         memory_db_fact_forget_preview(head, s_user, &merged, &back, &first));
   TEST_ASSERT_EQUAL_INT(1, merged);
   TEST_ASSERT_EQUAL_INT(1, back);
   TEST_ASSERT_EQUAL_INT64(old, first);
   TEST_ASSERT_EQUAL_INT64(head, sup(dup)); /* nothing changed */
   const memory_forget_result_t r = forget(head);
   TEST_ASSERT_EQUAL_INT(merged, r.merged);
   TEST_ASSERT_EQUAL_INT(back, r.restored);
}

/* A note's gloss is never a kept fact: its note deletes it without handing on. */
void test_gloss_is_never_kept(void) {
   const int64_t gloss = fact("Pat's packing list note", 0);
   const int64_t other = fact("Pat keeps a packing list", 0);
   sqlite3_exec(s_db.db, "PRAGMA foreign_keys=OFF", NULL, NULL, NULL); /* no such note */
   exec("UPDATE memory_facts SET note_doc_id = 7 WHERE id = ?", gloss);
   sqlite3_exec(s_db.db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
   TEST_ASSERT_EQUAL_INT(MEMORY_DB_NOT_FOUND, memory_db_fact_merge(other, gloss, s_user));
}

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_supersede_refuses_what_is_not_current);
   RUN_TEST(test_replace_closes_all_old_relations);
   RUN_TEST(test_merge_moves_relations_and_history);
   RUN_TEST(test_forget_newest_restores_one_step);
   RUN_TEST(test_forget_middle_value_hands_up);
   RUN_TEST(test_forget_reopens_a_relation_level_replace);
   RUN_TEST(test_forget_reopens_one_at_a_time_after_the_delete);
   RUN_TEST(test_conversation_forget_restores_and_preview_agrees);
   RUN_TEST(test_prune_low_confidence_takes_history);
   RUN_TEST(test_prune_stale_takes_history);
   RUN_TEST(test_prune_expired_takes_history);
   RUN_TEST(test_prune_superseded_keeps_replaced);
   RUN_TEST(test_restore_reports_a_newer_value_through_a_merge);
   RUN_TEST(test_v104_backfill);
   RUN_TEST(test_v104_long_chain);
   RUN_TEST(test_restore_takes_back_what_a_merge_moved);
   RUN_TEST(test_restore_reopens_through_a_merge);
   RUN_TEST(test_restore_reopens_what_a_later_replace_closed);
   RUN_TEST(test_restored_fact_drops_its_merge_mark);
   RUN_TEST(test_correction_leaves_relations_open);
   RUN_TEST(test_forget_preview_matches_the_forget);
   RUN_TEST(test_gloss_is_never_kept);
   return UNITY_END();
}
