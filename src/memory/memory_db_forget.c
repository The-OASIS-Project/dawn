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
 * Deleting what memory learned from a set of conversations, in one transaction.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <sqlite3.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_db_internal.h"
#include "memory/memory_db_provenance.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_embeddings_internal.h"
#include "memory/memory_stem.h"
#include "memory/memory_types.h"
#include "utils/string_utils.h"

/* One fact going, with the stems its keyword-index entry was written with. */
typedef struct {
   int64_t id;
   char stems[MEMORY_FACT_STEMS_MAX];
} forget_fact_t;

/* The set of facts to forget is decided again inside the delete transaction;
 * if a fact joined it after its text was collected, collect again. */
#define FORGET_ATTEMPTS 3

/* A forget transaction's outcome beyond MEMORY_DB_SUCCESS / _FAILURE /
 * _NOT_FOUND (0-2): the set changed since it was collected, collect again. */
#define FORGET_SET_CHANGED 3

/* Run one forget statement bound as ?1 = user_id, ?2 = the conversation ids
 * (statements that don't use ?2 reference it as "?2 IS NOT NULL" so every
 * statement binds the same way).  Caller holds the lock inside the transaction.
 * Rows changed into @p changed_out.  SUCCESS or FAILURE. */
static int run_forget_sql_locked(const char *sql, int user_id, const char *ids, int *changed_out) {
   *changed_out = 0;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      OLOG_WARNING("memory_db: prepare forget statement failed: %s", sqlite3_errmsg(s_db.db));
      return FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, ids, -1, SQLITE_STATIC);
   int rc = sqlite3_step(stmt);
   *changed_out = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? SUCCESS : FAILURE;
}

/* One-row integer query, bound like run_forget_sql_locked(), into @p out.
 * SUCCESS or FAILURE. */
static int query_int_locked(const char *sql, int user_id, const char *ids, int *out) {
   *out = 0;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      OLOG_WARNING("memory_db: prepare forget query failed: %s", sqlite3_errmsg(s_db.db));
      return FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, ids, -1, SQLITE_STATIC);
   const bool row = sqlite3_step(stmt) == SQLITE_ROW;
   if (row) {
      *out = sqlite3_column_int(stmt, 0);
   }
   sqlite3_finalize(stmt);
   return row ? SUCCESS : FAILURE;
}

/* Collect the facts that go with their full text, then stem it outside the lock
 * (the stemmer mutex is a leaf lock) exactly as the insert did: the whole text,
 * so a contentless FTS5 delete removes the tokens it was indexed with. */
static int collect_facts(int user_id, const char *ids, forget_fact_t **out, int *n_out) {
   *out = NULL;
   *n_out = 0;
   forget_fact_t *facts = NULL;
   char **texts = NULL;
   int n = 0;
   int cap = 0;
   bool failed = false;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT id, fact_text FROM memory_facts WHERE user_id = ?1 AND "
                               "id IN (" MEMORY_FORGET_FACT_IDS ")",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("memory_db: prepare forget list failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, ids, -1, SQLITE_STATIC);
   while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
      if (n == cap) {
         int ncap = cap ? cap * 2 : 64;
         forget_fact_t *gf = realloc(facts, (size_t)ncap * sizeof(*facts));
         if (gf) {
            facts = gf;
         }
         char **gt = gf ? realloc(texts, (size_t)ncap * sizeof(*texts)) : NULL;
         if (!gf || !gt) {
            failed = true;
            break;
         }
         texts = gt;
         cap = ncap;
      }
      const unsigned char *t = sqlite3_column_text(stmt, 1);
      texts[n] = strdup(t ? (const char *)t : "");
      if (!texts[n]) {
         failed = true;
         break;
      }
      facts[n].id = sqlite3_column_int64(stmt, 0);
      n++;
   }
   if (!failed && rc != SQLITE_DONE) {
      failed = true;
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   for (int i = 0; i < n; i++) {
      if (!failed) {
         (void)memory_stem_string(texts[i], facts[i].stems, sizeof(facts[i].stems));
      }
      free(texts[i]);
   }
   free(texts);
   if (failed) {
      free(facts);
      return MEMORY_DB_FAILURE;
   }
   *out = facts;
   *n_out = n;
   return MEMORY_DB_SUCCESS;
}

#define FORGET_IN_CONVS " WHERE user_id = ?1 AND source_conversation_id IN " MEMORY_FORGET_IN_SET

/* Delete everything in one transaction.  What goes is decided again inside it
 * (forget_facts / forget_relations): a source another conversation added since
 * the facts were collected keeps its row, and a fact that joined the set since
 * makes this return FORGET_SET_CHANGED (rolled back) so the caller collects
 * again.  Caller holds the lock. */
static int forget_in_tx_locked(int user_id,
                               const char *ids,
                               const forget_fact_t *facts,
                               int n_facts,
                               memory_conv_learned_t *d) {
   memset(d, 0, sizeof(*d));
   bool ok = auth_db_txn_begin_locked("memory_db: forget") == AUTH_DB_SUCCESS;
   ok = ok && sqlite3_exec(s_db.db,
                           "CREATE TEMP TABLE IF NOT EXISTS forget_collected(id INTEGER PRIMARY "
                           "KEY);"
                           "CREATE TEMP TABLE IF NOT EXISTS forget_facts(id INTEGER PRIMARY KEY);"
                           "CREATE TEMP TABLE IF NOT EXISTS forget_relations(id INTEGER "
                           "PRIMARY KEY);"
                           "CREATE TEMP TABLE IF NOT EXISTS forget_entities(id INTEGER "
                           "PRIMARY KEY);"
                           "DELETE FROM forget_collected; DELETE FROM forget_facts; "
                           "DELETE FROM forget_relations; DELETE FROM forget_entities;",
                           NULL, NULL, NULL) == SQLITE_OK;

   sqlite3_stmt *add = NULL;
   ok = ok && sqlite3_prepare_v2(s_db.db, "INSERT OR IGNORE INTO forget_collected VALUES (?)", -1,
                                 &add, NULL) == SQLITE_OK;
   for (int i = 0; ok && i < n_facts; i++) {
      sqlite3_reset(add);
      sqlite3_bind_int64(add, 1, facts[i].id);
      ok = sqlite3_step(add) == SQLITE_DONE;
   }
   sqlite3_finalize(add);

   int n = 0;
   ok = ok && run_forget_sql_locked("INSERT INTO forget_facts " MEMORY_FORGET_FACT_IDS, user_id,
                                    ids, &n) == SUCCESS;
   int joined = 0;
   ok = ok && query_int_locked("SELECT EXISTS (SELECT 1 FROM forget_facts WHERE ?2 IS "
                               "NOT NULL AND ?1 > 0 AND id NOT IN (SELECT id FROM "
                               "forget_collected))",
                               user_id, ids, &joined) == SUCCESS;
   if (ok && joined) {
      return auth_db_txn_end_locked(FORGET_SET_CHANGED, "memory_db: forget");
   }
   /* What the going facts replaced, or what stays under them: settled before
    * the deletes clear the links. */
   ok = ok && memory_db_internal_going_reset_locked() == MEMORY_DB_SUCCESS &&
        run_forget_sql_locked("INSERT INTO fact_going SELECT id FROM forget_facts WHERE ?1 > 0 "
                              "AND ?2 IS NOT NULL",
                              user_id, ids, &n) == SUCCESS &&
        memory_db_internal_settle_going_locked(user_id) == MEMORY_DB_SUCCESS;

   /* Relations are decided before their facts go (that clears their fact_id). */
   ok = ok && run_forget_sql_locked(
                  "INSERT OR IGNORE INTO forget_relations " MEMORY_FORGET_RELATION_IDS_IN(
                      "SELECT id FROM forget_facts"),
                  user_id, ids, &n) == SUCCESS;
   /* Entities the forgotten rows point at: removed below if nothing else does. */
   ok = ok && run_forget_sql_locked(
                  "INSERT OR IGNORE INTO forget_entities "
                  "SELECT subject_entity_id FROM memory_relations WHERE user_id = ?1 AND "
                  "?2 IS NOT NULL AND id IN (SELECT id FROM forget_relations) AND "
                  "subject_entity_id IS NOT NULL "
                  "UNION SELECT object_entity_id FROM memory_relations WHERE user_id = ?1 AND "
                  "id IN (SELECT id FROM forget_relations) AND object_entity_id IS NOT NULL "
                  "UNION SELECT subject_entity_id FROM memory_facts WHERE user_id = ?1 AND "
                  "id IN (SELECT id FROM forget_facts) AND subject_entity_id IS NOT NULL",
                  user_id, ids, &n) == SUCCESS;
   ok = ok && query_int_locked("SELECT COUNT(*) FROM memory_facts WHERE user_id = ?1 AND "
                               "?2 IS NOT NULL AND superseded_by IS NOT NULL AND id IN "
                               "(SELECT id FROM forget_facts)",
                               user_id, ids, &d->outdated) == SUCCESS;

   sqlite3_stmt *still = NULL;
   ok = ok && sqlite3_prepare_v2(s_db.db, "SELECT 1 FROM forget_facts WHERE id = ?", -1, &still,
                                 NULL) == SQLITE_OK;
   int deleted_facts = 0;
   for (int i = 0; ok && i < n_facts; i++) {
      sqlite3_reset(still);
      sqlite3_bind_int64(still, 1, facts[i].id);
      if (sqlite3_step(still) != SQLITE_ROW) {
         continue; /* another conversation taught it meanwhile */
      }
      /* A stale keyword-index entry would let the forgotten words match a later
       * fact that reuses the id, so a failed delete fails the forget. */
      if (memory_db_internal_fts5_delete_fact_locked(facts[i].id, facts[i].stems) != 0 &&
          s_db.stmt_memory_facts_fts_delete) {
         ok = false;
         break;
      }
      sqlite3_stmt *del = s_db.stmt_memory_fact_delete;
      sqlite3_reset(del);
      sqlite3_bind_int64(del, 1, facts[i].id);
      sqlite3_bind_int(del, 2, user_id);
      ok = sqlite3_step(del) == SQLITE_DONE;
      deleted_facts += ok ? sqlite3_changes(s_db.db) : 0;
      sqlite3_reset(del);
   }
   sqlite3_finalize(still);
   d->facts = deleted_facts - d->outdated;

#define FORGET_STEP(sql, field)                                            \
   do {                                                                    \
      if (ok && run_forget_sql_locked(sql, user_id, ids, &n) == SUCCESS) { \
         (field) += n;                                                     \
      } else {                                                             \
         ok = false;                                                       \
      }                                                                    \
   } while (0)
   int ignored = 0;
   FORGET_STEP("DELETE FROM memory_summaries" FORGET_IN_CONVS, d->summaries);
   FORGET_STEP("DELETE FROM memory_relations WHERE user_id = ?1 AND ?2 IS NOT NULL AND id IN "
               "(SELECT id FROM forget_relations)",
               d->relations);
   /* After the going relations (one still open would block a reopen), before
    * the entity sweep (a fact back keeps its entities). */
   ok = ok && memory_db_internal_restore_settled_locked(user_id, NULL, 0, &d->restored) ==
                  MEMORY_DB_SUCCESS;
   FORGET_STEP("DELETE FROM memory_preferences WHERE id IN (SELECT p.id FROM "
               "memory_preferences p WHERE " MEMORY_FORGET_PREF_WHERE ")",
               d->preferences);
   /* Rows that stay (other conversations taught them too) lose these sources
    * and point at a remaining one. */
   FORGET_STEP("DELETE FROM memory_fact_sources WHERE conversation_id IN " MEMORY_FORGET_IN_SET
               " AND EXISTS (SELECT 1 FROM memory_facts f WHERE f.id = fact_id AND "
               "f.user_id = ?1)",
               ignored);
   FORGET_STEP("DELETE FROM memory_relation_sources WHERE conversation_id IN " MEMORY_FORGET_IN_SET
               " AND EXISTS (SELECT 1 FROM memory_relations r WHERE r.id = relation_id AND "
               "r.user_id = ?1)",
               ignored);
   FORGET_STEP(
       "DELETE FROM memory_preference_sources WHERE conversation_id IN " MEMORY_FORGET_IN_SET
       " AND EXISTS (SELECT 1 FROM memory_preferences p WHERE p.id = preference_id AND "
       "p.user_id = ?1)",
       ignored);
   FORGET_STEP("UPDATE memory_facts SET source_conversation_id = (SELECT MAX(x.conversation_id) "
               "FROM memory_fact_sources x WHERE x.fact_id = memory_facts.id), "
               "source_msg_id_start = NULL, source_msg_id_end = NULL" FORGET_IN_CONVS,
               ignored);
   FORGET_STEP(
       "UPDATE memory_relations SET source_conversation_id = (SELECT "
       "MAX(x.conversation_id) FROM memory_relation_sources x WHERE x.relation_id = "
       "memory_relations.id), source_msg_id_start = NULL, source_msg_id_end = NULL" FORGET_IN_CONVS,
       ignored);
   FORGET_STEP("UPDATE memory_preferences SET source_conversation_id = (SELECT "
               "MAX(x.conversation_id) FROM memory_preference_sources x WHERE "
               "x.preference_id = memory_preferences.id), source_msg_id_start = NULL, "
               "source_msg_id_end = NULL" FORGET_IN_CONVS,
               ignored);
   FORGET_STEP("DELETE FROM memory_entities WHERE user_id = ?1 AND ?2 IS NOT NULL AND "
               "id IN (SELECT id FROM forget_entities) AND " MEMORY_ENTITY_UNREFERENCED,
               d->entities);
   /* Forgetting means what was learned so far: the conversations count as
    * extracted, so memory recovery doesn't learn them again (what is said in
    * them from now on is extracted as usual). */
   FORGET_STEP("UPDATE conversations SET " CONV_MARK_EXTRACTED_SET_SQL
               " WHERE user_id = ?1 AND id IN " MEMORY_FORGET_IN_SET,
               ignored);
#undef FORGET_STEP
   (void)sqlite3_exec(s_db.db,
                      "DELETE FROM forget_collected; DELETE FROM forget_facts; "
                      "DELETE FROM forget_relations; DELETE FROM forget_entities; "
                      "DELETE FROM fact_going;",
                      NULL, NULL, NULL);
   if (!ok) {
      OLOG_ERROR("memory_db: forget for user %d failed: %s", user_id, sqlite3_errmsg(s_db.db));
   }
   if (auth_db_txn_end_locked(ok ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE, "memory_db: forget") !=
       AUTH_DB_SUCCESS) {
      return MEMORY_DB_FAILURE;
   }
   return MEMORY_DB_SUCCESS;
}

int memory_db_conversations_forget(int user_id,
                                   const int64_t *conv_ids,
                                   int n_conv,
                                   memory_conv_learned_t *deleted_out) {
   if (deleted_out) {
      memset(deleted_out, 0, sizeof(*deleted_out));
   }
   char ids[AUTH_DB_IDS_JSON_SIZE(CONV_CHAIN_MAX)];
   if (user_id <= 0 || !conv_ids || n_conv <= 0 || n_conv > CONV_CHAIN_MAX ||
       !auth_db_internal_ids_json_into(conv_ids, n_conv, ids, sizeof(ids))) {
      return MEMORY_DB_FAILURE;
   }

   memory_conv_learned_t d;
   int rc = FORGET_SET_CHANGED;
   for (int attempt = 0; attempt < FORGET_ATTEMPTS && rc == FORGET_SET_CHANGED; attempt++) {
      forget_fact_t *facts = NULL;
      int n_facts = 0;
      if (collect_facts(user_id, ids, &facts, &n_facts) != MEMORY_DB_SUCCESS) {
         return MEMORY_DB_FAILURE;
      }
      AUTH_DB_LOCK_OR_RETURN((free(facts), MEMORY_DB_FAILURE));
      rc = forget_in_tx_locked(user_id, ids, facts, n_facts, &d);
      AUTH_DB_UNLOCK();
      free(facts);
   }
   if (rc != MEMORY_DB_SUCCESS) {
      if (rc == FORGET_SET_CHANGED) {
         OLOG_WARNING("memory_db: forget for user %d gave up: memory kept changing", user_id);
      }
      return MEMORY_DB_FAILURE;
   }
   if (d.entities > 0) {
      memory_embeddings_invalidate_entity_cache_for_user(user_id);
   }
   if (d.facts + d.outdated + d.restored > 0) {
      memory_embeddings_invalidate_cache_for_user(user_id); /* another user's copy stays */
   }
   if (deleted_out) {
      *deleted_out = d;
   }
   return MEMORY_DB_SUCCESS;
}

#undef FORGET_IN_CONVS

/* =============================================================================
 * One fact
 * ============================================================================= */

/* Facts one forget takes at most (the fact and what was merged into it). */
#define FORGET_TREE_MAX 512

/* @p fact_id and the facts merged into it, at any depth, deepest first and
 * @p fact_id last (a merge cycle can't put it earlier), into @p ids, how many
 * into *n_out (0: no such fact).  MEMORY_DB_FAILURE on error or when it holds
 * more than @p max.  Caller holds the lock. */
static int forget_tree_locked(int user_id, int64_t fact_id, int64_t *ids, int max, int *n_out) {
   *n_out = 0;
   sqlite3_stmt *st = NULL;
   int n = 0;
   int rc = SQLITE_ERROR;
   if (sqlite3_prepare_v2(s_db.db,
                          "WITH RECURSIVE tree(id, depth) AS ("
                          "  SELECT id, 0 FROM memory_facts WHERE id = ?2 AND user_id = ?1 "
                          "  UNION "
                          "  SELECT f.id, t.depth + 1 FROM memory_facts f JOIN tree t ON "
                          "  f.superseded_by = t.id WHERE +f.user_id = ?1 AND "
                          "  f.superseded_kind = " MEMORY_MERGED_SQL " AND t.depth < ?3) "
                          "SELECT id FROM tree GROUP BY id ORDER BY id = ?2, MAX(depth) DESC, id "
                          "LIMIT ?3",
                          -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int(st, 1, user_id);
      sqlite3_bind_int64(st, 2, fact_id);
      sqlite3_bind_int(st, 3, max + 1);
      while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
         if (n == max) {
            rc = SQLITE_FULL; /* more than a forget takes at once */
            break;
         }
         ids[n++] = sqlite3_column_int64(st, 0);
      }
   }
   sqlite3_finalize(st);
   if (rc != SQLITE_DONE) {
      return MEMORY_DB_FAILURE;
   }
   *n_out = n;
   return MEMORY_DB_SUCCESS;
}

/* The tree and each fact's keyword-index stems, stemmed outside the lock (the
 * stemmer mutex is a leaf).  MEMORY_DB_FAILURE when the tree is too big. */
static int forget_collect(int user_id,
                          int64_t fact_id,
                          int64_t *ids,
                          forget_fact_t **facts_out,
                          int *n_out) {
   *facts_out = NULL;
   *n_out = 0;
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   int n = 0;
   const int rc = forget_tree_locked(user_id, fact_id, ids, FORGET_TREE_MAX, &n);
   AUTH_DB_UNLOCK();
   if (rc != MEMORY_DB_SUCCESS || n == 0) {
      return rc;
   }
   forget_fact_t *facts = calloc((size_t)n, sizeof(*facts));
   if (!facts) {
      return MEMORY_DB_FAILURE;
   }
   for (int i = 0; i < n; i++) {
      facts[i].id = ids[i];
      free(memory_db_internal_fact_stems(ids[i], user_id, facts[i].stems, sizeof(facts[i].stems)));
   }
   *facts_out = facts;
   *n_out = n;
   return MEMORY_DB_SUCCESS;
}

/* The forget itself, in one transaction: the tree again (FORGET_SET_CHANGED
 * when it changed since it was stemmed), settle, the deletes, then bring back
 * what is current again.  Caller holds the lock. */
static int forget_fact_tx_locked(int user_id,
                                 int64_t fact_id,
                                 const forget_fact_t *facts,
                                 int n,
                                 int64_t *ids,
                                 memory_forget_result_t *out,
                                 int *entities_gone) {
   if (auth_db_txn_begin_locked("memory_db: forget fact") != AUTH_DB_SUCCESS) {
      return MEMORY_DB_FAILURE;
   }
   int now = 0;
   bool same = forget_tree_locked(user_id, fact_id, ids, FORGET_TREE_MAX, &now) ==
                   MEMORY_DB_SUCCESS &&
               now == n;
   for (int i = 0; same && i < n; i++) {
      same = ids[i] == facts[i].id;
   }
   if (!same) {
      return auth_db_txn_end_locked(FORGET_SET_CHANGED, "memory_db: forget fact");
   }
   bool ok = memory_db_internal_going_reset_locked() == MEMORY_DB_SUCCESS;
   for (int i = 0; ok && i < n; i++) {
      ok = memory_db_internal_exec_locked("INSERT INTO fact_going VALUES (?2)", user_id,
                                          facts[i].id, 0) == SQLITE_DONE;
   }
   ok = ok && memory_db_internal_settle_going_locked(user_id) == MEMORY_DB_SUCCESS;
   /* Deepest first, the fact last: a merged fact is never left pointing at a
    * deleted one. */
   for (int i = 0; ok && i < n; i++) {
      int gone = 0;
      bool deleted = false;
      ok = memory_db_internal_fact_delete_locked(user_id, facts[i].id, facts[i].stems, &gone,
                                                 &deleted) == MEMORY_DB_SUCCESS;
      *entities_gone += gone;
      if (ok && deleted && facts[i].id != fact_id && out) {
         out->merged++;
      }
   }
   int restored = 0;
   ok = ok && memory_db_internal_restore_settled_locked(user_id, out ? out->restored_ids : NULL,
                                                        out ? MEMORY_FORGET_RESTORED_MAX : 0,
                                                        &restored) == MEMORY_DB_SUCCESS;
   if (out) {
      out->restored = restored;
   }
   (void)sqlite3_exec(s_db.db, "DELETE FROM fact_going;", NULL, NULL, NULL);
   return auth_db_txn_end_locked(ok ? AUTH_DB_SUCCESS : AUTH_DB_FAILURE,
                                 "memory_db: forget fact") == AUTH_DB_SUCCESS
              ? MEMORY_DB_SUCCESS
              : MEMORY_DB_FAILURE;
}

int memory_db_fact_forget(int64_t fact_id, int user_id, memory_forget_result_t *out) {
   if (out) {
      memset(out, 0, sizeof(*out));
   }
   if (user_id <= 0 || fact_id <= 0) {
      return MEMORY_DB_FAILURE;
   }
   int64_t *ids = malloc(FORGET_TREE_MAX * sizeof(*ids));
   if (!ids) {
      return MEMORY_DB_FAILURE;
   }
   int rc = FORGET_SET_CHANGED;
   int entities_gone = 0;
   for (int attempt = 0; attempt < FORGET_ATTEMPTS && rc == FORGET_SET_CHANGED; attempt++) {
      forget_fact_t *facts = NULL;
      int n = 0;
      if (forget_collect(user_id, fact_id, ids, &facts, &n) != MEMORY_DB_SUCCESS) {
         OLOG_WARNING("memory_db: forget of fact %lld for user %d: merge tree unreadable or "
                      "over %d facts; fact kept",
                      (long long)fact_id, user_id, FORGET_TREE_MAX);
         rc = MEMORY_DB_FAILURE;
         break;
      }
      if (n == 0) {
         rc = MEMORY_DB_NOT_FOUND;
         break;
      }
      if (out) {
         memset(out, 0, sizeof(*out));
      }
      entities_gone = 0;
      AUTH_DB_LOCK_OR_RETURN((free(facts), free(ids), MEMORY_DB_FAILURE));
      rc = forget_fact_tx_locked(user_id, fact_id, facts, n, ids, out, &entities_gone);
      AUTH_DB_UNLOCK();
      free(facts);
   }
   free(ids);
   if (rc == FORGET_SET_CHANGED) {
      OLOG_WARNING("memory_db: forget of fact %lld for user %d gave up: memory kept changing",
                   (long long)fact_id, user_id);
      rc = MEMORY_DB_FAILURE;
   }
   if (rc == MEMORY_DB_SUCCESS) {
      if (entities_gone > 0) {
         memory_embeddings_invalidate_entity_cache_for_user(user_id);
      }
      memory_embeddings_invalidate_cache_for_user(user_id);
   } else if (out) {
      memset(out, 0, sizeof(*out));
   }
   return rc;
}

int memory_db_fact_forget_preview(int64_t fact_id,
                                  int user_id,
                                  int *merged,
                                  int *restored,
                                  int64_t *first_restored) {
   *merged = 0;
   *restored = 0;
   *first_restored = 0;
   if (user_id <= 0 || fact_id <= 0) {
      return MEMORY_DB_FAILURE;
   }
   int64_t *ids = malloc(FORGET_TREE_MAX * sizeof(*ids));
   if (!ids) {
      return MEMORY_DB_FAILURE;
   }
   AUTH_DB_LOCK_OR_RETURN((free(ids), MEMORY_DB_FAILURE));
   int n = 0;
   bool ok = forget_tree_locked(user_id, fact_id, ids, FORGET_TREE_MAX, &n) == MEMORY_DB_SUCCESS;
   ok = ok && memory_db_internal_going_reset_locked() == MEMORY_DB_SUCCESS;
   for (int i = 0; ok && i < n; i++) {
      ok = memory_db_internal_exec_locked("INSERT INTO fact_going VALUES (?2)", user_id, ids[i],
                                          0) == SQLITE_DONE;
   }
   ok = ok && memory_db_internal_settle_count_locked(user_id, restored, first_restored) ==
                  MEMORY_DB_SUCCESS;
   (void)sqlite3_exec(s_db.db, "DELETE FROM fact_going;", NULL, NULL, NULL);
   AUTH_DB_UNLOCK();
   free(ids);
   if (!ok || n == 0) {
      *restored = 0;
      *first_restored = 0;
      return ok ? MEMORY_DB_NOT_FOUND : MEMORY_DB_FAILURE;
   }
   *merged = n - 1;
   return MEMORY_DB_SUCCESS;
}
