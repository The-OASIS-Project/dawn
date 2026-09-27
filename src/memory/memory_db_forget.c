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

/* forget_in_tx() outcomes beyond SUCCESS/FAILURE. */
#define FORGET_SET_CHANGED 2

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
   bool ok = sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK;
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
      (void)sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      return FORGET_SET_CHANGED;
   }

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
         field += n;                                                       \
      } else {                                                             \
         ok = false;                                                       \
      }                                                                    \
   } while (0)
   int ignored = 0;
   FORGET_STEP("DELETE FROM memory_summaries" FORGET_IN_CONVS, d->summaries);
   FORGET_STEP("DELETE FROM memory_relations WHERE user_id = ?1 AND ?2 IS NOT NULL AND id IN "
               "(SELECT id FROM forget_relations)",
               d->relations);
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
   /* An entity an alias still targets stays: the alias keeps it as a merge
    * target (and its target column can't be nulled). */
   FORGET_STEP("DELETE FROM memory_entities WHERE user_id = ?1 AND ?2 IS NOT NULL AND "
               "is_user_self = 0 AND id IN (SELECT id FROM forget_entities) AND "
               /* One index seek each ("+user_id" keeps the planner off the
                * user_id index, which scans every relation per entity). */
               "NOT EXISTS (SELECT 1 FROM memory_relations r WHERE "
               "            r.subject_entity_id = memory_entities.id AND +r.user_id = ?1) AND "
               "NOT EXISTS (SELECT 1 FROM memory_relations r WHERE "
               "            r.object_entity_id = memory_entities.id AND +r.user_id = ?1) AND "
               "NOT EXISTS (SELECT 1 FROM memory_facts f WHERE f.user_id = ?1 AND "
               "            f.subject_entity_id = memory_entities.id) AND "
               "NOT EXISTS (SELECT 1 FROM contacts c WHERE c.entity_id = memory_entities.id) AND "
               "NOT EXISTS (SELECT 1 FROM memory_entities a WHERE "
               "            a.canonical_id = memory_entities.id) AND "
               "NOT EXISTS (SELECT 1 FROM memory_entity_aliases al WHERE al.user_id = ?1 AND "
               "            al.target_entity_id = memory_entities.id)",
               d->entities);
#undef FORGET_STEP
   (void)sqlite3_exec(s_db.db,
                      "DELETE FROM forget_collected; DELETE FROM forget_facts; "
                      "DELETE FROM forget_relations; DELETE FROM forget_entities;",
                      NULL, NULL, NULL);
   if (ok && sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
      ok = false;
   }
   if (!ok) {
      OLOG_ERROR("memory_db: forget for user %d failed: %s", user_id, sqlite3_errmsg(s_db.db));
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
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
   char ids[MEMORY_DB_IDS_JSON_SIZE(CONV_CHAIN_MAX)];
   if (user_id <= 0 || !conv_ids || n_conv <= 0 || n_conv > CONV_CHAIN_MAX ||
       !memory_db_internal_ids_json(conv_ids, n_conv, ids, sizeof(ids))) {
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
      memory_embeddings_invalidate_entity_cache();
   }
   if (d.facts + d.outdated > 0) {
      memory_embeddings_invalidate_cache_for_user(user_id); /* another user's copy stays */
   }
   if (deleted_out) {
      *deleted_out = d;
   }
   return MEMORY_DB_SUCCESS;
}

#undef FORGET_IN_CONVS
