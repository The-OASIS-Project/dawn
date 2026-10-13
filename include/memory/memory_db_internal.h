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
 * Internal header shared by memory_db*.c TUs.
 *
 * Phase 6 source split — these helpers were `static` in memory_db.c before
 * the file was decomposed into per-noun sources (facts / summaries / prefs /
 * entities / relations).  Promoted to non-static so the new TUs can share
 * them without duplicating the code.  NOT a public API — callers outside
 * src/memory/memory_db*.c must not include this header.
 *
 * All helpers in this header assume the caller holds AUTH_DB_LOCK except
 * where noted.  See memory_db.c for the canonical implementations.
 */
#ifndef DAWN_MEMORY_DB_INTERNAL_H
#define DAWN_MEMORY_DB_INTERNAL_H

#include <sqlite3.h>
#include <stddef.h>
#include <stdint.h>

#include "memory/memory_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bind provenance columns to sequential params at (base, base+1, base+2).
 * Emits SQLITE_NULL for all three when prov is NULL or prov->conv_id <= 0.
 * Caller must hold AUTH_DB_LOCK (operates on a prepared statement). */
void memory_db_internal_bind_provenance(struct sqlite3_stmt *stmt,
                                        int base,
                                        const memory_provenance_t *prov);

/* Build a LIKE pattern wrapping `keywords` with `%` and escaping the LIKE
 * metacharacters %, _, \\ with backslash.  Output is NUL-terminated even on
 * overflow.  Pure helper — no locks involved. */
void memory_db_internal_build_like_pattern(const char *keywords, char *out_pattern, size_t max_len);

/* Remove a fact's keyword-index (FTS5) entry.  Needs the stems it was indexed
 * with (stem the fact text outside the lock).  Caller must hold AUTH_DB_LOCK.
 * Returns 0 on success, non-zero on failure (logged; search degrades). */
int memory_db_internal_fts5_delete_fact_locked(int64_t fact_id, const char *fact_stems);

/* Run @p sql with ?1 = @p user_id, ?2 = @p a, ?3 = @p b (those it has; a
 * statement without ?2 ignores @p a); the sqlite3_step result.  @p sql must be
 * a string literal: the prepared statement is kept, keyed by its address, for
 * the next call (a per-fact delete runs the same few statements many times
 * under the lock).  Caller holds the lock. */
int memory_db_internal_exec_locked(const char *sql, int user_id, int64_t a, int64_t b);

/* A row's sources all lie in conversation set @p convs (SQL of a list of
 * ids): no source row (@p src_table by @p src_col) outside it, and its own
 * source_conversation_id unset or in it.  Row aliased @p alias. */
#define MEMORY_ROW_TAUGHT_ONLY_BY(alias, src_table, src_col, convs)                              \
   "NOT EXISTS (SELECT 1 FROM " src_table " x WHERE x." src_col " = " alias ".id AND "           \
   "x.conversation_id NOT IN " convs ") AND (" alias ".source_conversation_id IS NULL OR " alias \
   ".source_conversation_id IN " convs ")"

/* The fact's whole text when @p user_id owns it, else NULL (caller frees),
 * with its keyword-index stems written into @p stems.  Call with no lock held
 * (the stemmer mutex is a leaf). */
char *memory_db_internal_fact_stems(int64_t fact_id, int user_id, char *stems, size_t size);

/* Delete one fact as memory_db_fact_delete does (its keyword-index rows from
 * @p stems, its relations unless another conversation taught them, the
 * entities only it named), in a savepoint of its own: inside the caller's
 * transaction, or alone.  The caller invalidates the embedding caches after
 * unlocking.  MEMORY_DB_SUCCESS (with *deleted false when there was no such
 * fact) or MEMORY_DB_FAILURE.  Caller holds the lock. */
int memory_db_internal_fact_delete_locked(int user_id,
                                          int64_t fact_id,
                                          const char *stems,
                                          int *entities_gone,
                                          bool *deleted);

/* The columns memory_db_internal_reopen_rows_locked reads, from closed
 * relations of the user (?1); callers append their conditions. */
#define MEMORY_REOPEN_SELECT                                                 \
   "SELECT id, subject_entity_id, relation, COALESCE(object_entity_id, 0), " \
   "COALESCE(object_value, '') FROM memory_relations WHERE user_id = ?1 AND valid_to IS NOT NULL "

/* Reopen the relations @p sel selects (MEMORY_REOPEN_SELECT, bound), unless a
 * value is open again already (that one, any for a one-at-a-time relation, or
 * its contradiction on the same object; counted in *kept_closed), the number
 * reopened into *reopened.  @p sel is finalized.  MEMORY_DB_SUCCESS or
 * MEMORY_DB_FAILURE.  Caller holds the lock inside its transaction. */
int memory_db_internal_reopen_rows_locked(int user_id,
                                          sqlite3_stmt *sel,
                                          int *reopened,
                                          int *kept_closed);

/* A user's forget, as one transaction (memory_db_fact_settle.c): the caller
 * clears temp table fact_going (memory_db_internal_going_reset_locked) and
 * fills it with every fact that goes; memory_db_internal_settle_going_locked()
 * runs before the deletes (it reads links the deletes clear); the caller
 * deletes the facts and their relations;
 * memory_db_internal_restore_settled_locked() runs after the relation
 * deletes (a going fact's open relation would block a reopen) and before the
 * entity sweep, and reports the replaced values now current (up to @p max ids
 * into @p ids, the full count into *count).  All run inside the caller's
 * transaction and return MEMORY_DB_SUCCESS or MEMORY_DB_FAILURE (roll back).
 * The caller invalidates the embedding caches after unlocking. */
int memory_db_internal_going_reset_locked(void);
int memory_db_internal_settle_going_locked(int user_id);
int memory_db_internal_restore_settled_locked(int user_id, int64_t *ids, int max, int *count);
/* How many replaced values forgetting fact_going would bring back (no writes
 * but the temp tables), and the first of them into *first_id (may be NULL):
 * the forget preview. */
int memory_db_internal_settle_count_locked(int user_id, int *restored, int64_t *first_id);

/* Prune the facts @p seed_sql selects ("SELECT id, 0 FROM memory_facts ...",
 * ?1 the user, ?2 @p cutoff, ?3 @p threshold, those it uses) with their whole
 * history (every fact superseded under them, merged or replaced): pruning says
 * a fact aged out, not that it was wrong, so nothing comes back.  Deleted as
 * memory_db_fact_delete does, deepest first, in transactions of a bounded
 * number of facts (the lock is let go between them), each checking the set
 * is still what the seed selects.  @p what names it in the log.  Takes the
 * lock itself. */
int memory_db_internal_prune_with_history(int user_id,
                                          const char *seed_sql,
                                          int64_t cutoff,
                                          double threshold,
                                          const char *what,
                                          int *count_out);

#define MEMORY_DB_STR_(x) #x
#define MEMORY_DB_STR(x) MEMORY_DB_STR_(x)

/* MEMORY_SUPERSEDED_* as SQL literals. */
#define MEMORY_MERGED_SQL MEMORY_DB_STR(MEMORY_SUPERSEDED_MERGED)
#define MEMORY_REPLACED_SQL MEMORY_DB_STR(MEMORY_SUPERSEDED_REPLACED)

/* Steps any walk along superseded_by takes.  Chains reach tens of links (a
 * fact merged into one later merged again), so only a cycle (the code before
 * superseding was guarded allowed them) reaches it; a walk that does treats
 * the chain as having no head that stays. */
#define MEMORY_CHAIN_DEPTH_MAX 256
#define MEMORY_CHAIN_DEPTH_SQL MEMORY_DB_STR(MEMORY_CHAIN_DEPTH_MAX)

/* A walk's "was any link on the way a replace" (@p acc) with @p alias's link
 * to its keeper folded in: no link when it has no keeper (a kind left behind
 * by a delete that cleared superseded_by through the foreign key isn't one). */
#define MEMORY_LINK_KIND(alias, acc)                                                 \
   "CASE WHEN " alias ".superseded_by IS NULL THEN " acc " ELSE MAX(" acc ", " alias \
   ".superseded_kind) END"

/* CTE @p name(id, d): fact @p root_sql and the facts merged into it, at any
 * depth (the facts that are the same fact).  @p user_sql the user's param. */
#define MEMORY_MERGED_UNDER_CTE(name, root_sql, user_sql)                                        \
   name "(id, d) AS (SELECT " root_sql ", 0 UNION SELECT f.id, m.d + 1 FROM memory_facts f "     \
        "JOIN " name " m ON f.superseded_by = m.id WHERE f.superseded_kind = " MEMORY_MERGED_SQL \
        " AND +f.user_id = " user_sql " AND m.d < " MEMORY_CHAIN_DEPTH_SQL ")"

/* Mark @p old_id superseded by @p new_id, as @p kind (MEMORY_SUPERSEDED_*).
 * Both the user's and both current (a superseded fact is never re-pointed,
 * and never kept under a hidden one: no cycles).  *changed says whether a row
 * changed.  The sqlite3_step result.  Caller holds the lock. */
int memory_db_internal_fact_supersede_locked(int64_t old_id,
                                             int64_t new_id,
                                             int user_id,
                                             int kind,
                                             bool *changed);

/* Which *_sources table memory_db_internal_source_add_locked() writes. */
typedef enum {
   MEMORY_SOURCE_FACT,
   MEMORY_SOURCE_RELATION,
   MEMORY_SOURCE_PREFERENCE,
} memory_source_kind_t;

/* Record that row @p row_id of @p kind was learned from @p conv_id (no-op for
 * conv_id <= 0, or before the v89 migration).  Caller must hold AUTH_DB_LOCK. */
void memory_db_internal_source_add_locked(memory_source_kind_t kind,
                                          int64_t row_id,
                                          int64_t conv_id);

/* Forgetting a set of conversations: SQL fragments bound as ?1 = user_id,
 * ?2 = the conversation ids as a JSON array (auth_db_internal_ids_json_into).
 * Shared by the count shown before a forget and the forget itself, so the two
 * always agree.
 *
 * A row goes only if every conversation it was learned from is in the set (one
 * other conversations also taught stays; it just loses these sources).  The
 * sources are the kind's *_sources table plus the row's own
 * source_conversation_id (either one naming a conversation outside the set keeps
 * the row, so the two can't disagree into a loss), and a row also learned outside
 * any conversation (origin_unsourced: "remember", an import) always stays; the
 * set's rows are found
 * through the source indexes,
 * so the cost follows the conversations' size, not the user's whole memory. */
#define MEMORY_FORGET_IN_SET "(SELECT value FROM json_each(?2))"
#define MEMORY_FORGET_ONLY_FROM(alias, table, src_table, src_col)                                 \
   "(" alias ".user_id = ?1 AND " alias ".origin_unsourced = 0 AND " alias                        \
   ".id IN (SELECT " src_col " FROM " src_table " WHERE conversation_id IN " MEMORY_FORGET_IN_SET \
   " UNION ALL SELECT id FROM " table                                                             \
   " WHERE user_id = ?1 AND source_conversation_id IN " MEMORY_FORGET_IN_SET                      \
   ") AND " MEMORY_ROW_TAUGHT_ONLY_BY(alias, src_table, src_col, MEMORY_FORGET_IN_SET) ")"
/* memory_facts aliased "f": learned only from the set. */
#define MEMORY_FORGET_FACT_WHERE \
   MEMORY_FORGET_ONLY_FROM("f", "memory_facts", "memory_fact_sources", "fact_id")
/* Ids of the facts that go: learned only from the set, plus the duplicates
 * merged into them, down the chain (a merged fact is the fact it went into).
 * The values they replaced are history, not the facts themselves: they stay,
 * and memory_db_settle_going_locked() brings them back or hands them on.  The
 * chain stops at a fact learned outside any conversation (an import, a save
 * with no conversation): it stays.
 * "+p.user_id" keeps the planner on idx_memory_facts_superseded_by: on user_id
 * it scans all of the user's facts per step of the recursion (seconds, under the
 * global database lock).  tests/test_memory_provenance.c pins the plan. */
#define MEMORY_FORGET_FACT_IDS                                                    \
   "WITH RECURSIVE forget_gone(id) AS (SELECT f.id FROM memory_facts f "          \
   "WHERE " MEMORY_FORGET_FACT_WHERE                                              \
   " UNION SELECT p.id FROM memory_facts p JOIN forget_gone g ON "                \
   "p.superseded_by = g.id WHERE +p.user_id = ?1 AND p.origin_unsourced = 0 AND " \
   "p.superseded_kind = " MEMORY_MERGED_SQL ") SELECT id FROM forget_gone"
/* Ids of the relations that go (@p facts: SQL selecting the fact ids that go):
 * learned only from the set, or tied to a fact that goes and learned from no
 * conversation outside the set.  One tied to a fact but also taught elsewhere
 * stays (its fact_id is cleared).  A UNION so each arm uses its own index
 * ("+r.user_id": the fact arm seeks idx_memory_relations_fact). */
#define MEMORY_FORGET_RELATION_IDS_IN(facts)                                                  \
   "SELECT r.id FROM memory_relations r WHERE " MEMORY_FORGET_ONLY_FROM(                      \
       "r", "memory_relations", "memory_relation_sources",                                    \
       "relation_id") " UNION SELECT r.id FROM memory_relations r WHERE +r.user_id = ?1 AND " \
                      "r.fact_id IN (" facts                                                  \
                      ") AND r.origin_unsourced = 0 AND " MEMORY_ROW_TAUGHT_ONLY_BY(          \
                          "r", "memory_relation_sources", "relation_id", MEMORY_FORGET_IN_SET)
#define MEMORY_FORGET_RELATION_IDS MEMORY_FORGET_RELATION_IDS_IN(MEMORY_FORGET_FACT_IDS)
/* A memory_entities row nothing else names any more (?1: the user): not the
 * user's own (is_user_self; a generic "user" entity is recreated on its next
 * mention), no relation or fact about it, no contact, alias or merge pointing at
 * it.  An entity an alias still targets stays (its target column can't be
 * nulled).  One index seek each ("+user_id" keeps the planner off the user_id
 * index, which scans every relation per entity). */
#define MEMORY_ENTITY_UNREFERENCED                                                     \
   "memory_entities.is_user_self = 0 AND "                                             \
   "NOT EXISTS (SELECT 1 FROM memory_relations r WHERE "                               \
   "            r.subject_entity_id = memory_entities.id AND +r.user_id = ?1) AND "    \
   "NOT EXISTS (SELECT 1 FROM memory_relations r WHERE "                               \
   "            r.object_entity_id = memory_entities.id AND +r.user_id = ?1) AND "     \
   "NOT EXISTS (SELECT 1 FROM memory_facts f WHERE f.user_id = ?1 AND "                \
   "            f.subject_entity_id = memory_entities.id) AND "                        \
   "NOT EXISTS (SELECT 1 FROM contacts c WHERE c.entity_id = memory_entities.id) AND " \
   "NOT EXISTS (SELECT 1 FROM memory_entities a WHERE "                                \
   "            a.canonical_id = memory_entities.id) AND "                             \
   "NOT EXISTS (SELECT 1 FROM memory_entity_aliases al WHERE al.user_id = ?1 AND "     \
   "            al.target_entity_id = memory_entities.id)"
/* memory_preferences aliased "p" (sources: every conversation that gave its
 * current value). */
#define MEMORY_FORGET_PREF_WHERE \
   MEMORY_FORGET_ONLY_FROM("p", "memory_preferences", "memory_preference_sources", "preference_id")

/* Lightweight FK existence check (diagnostic — used by the relation_supersede
 * FK probe to pinpoint which FK fired on SQLITE_CONSTRAINT_FOREIGNKEY).
 * Returns 1 if a row exists, 0 if missing, -1 if the prepare itself failed.
 * The -1 is intentionally not SUCCESS/FAILURE — this is diagnostic-only and
 * the caller logs it verbatim ("-1=check_failed").  Caller must hold
 * AUTH_DB_LOCK.
 *
 * `user_id_scope`: when > 0, the probe also requires user_id = user_id_scope
 * (CWE-209 close — without this, the probe would report "subj_ok=1" for an
 * entity belonging to a DIFFERENT user, misleading operator triage and
 * weakly leaking cross-user row existence into the FK error log).  When 0
 * (legacy unscoped), the probe matches any user — appropriate for the
 * `users` table where the row itself IS the user. */
int memory_db_internal_fk_row_exists(const char *table, int64_t id, int user_id_scope);

#ifdef __cplusplus
}
#endif

#endif /* DAWN_MEMORY_DB_INTERNAL_H */
