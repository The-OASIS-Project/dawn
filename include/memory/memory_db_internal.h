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

#include <stddef.h>
#include <stdint.h>

#include "memory/memory_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations to avoid pulling in sqlite3.h at every include site. */
struct sqlite3_stmt;

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
 * ?2 = the conversation ids as a JSON array (memory_db_internal_ids_json).
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
   " WHERE user_id = ?1 AND source_conversation_id IN " MEMORY_FORGET_IN_SET ") AND NOT "         \
   "EXISTS (SELECT 1 FROM " src_table " x WHERE x." src_col " = " alias ".id AND "                \
   "x.conversation_id NOT IN " MEMORY_FORGET_IN_SET ") AND (" alias                               \
   ".source_conversation_id IS NULL OR " alias ".source_conversation_id IN " MEMORY_FORGET_IN_SET \
   "))"
/* memory_facts aliased "f": learned only from the set. */
#define MEMORY_FORGET_FACT_WHERE \
   MEMORY_FORGET_ONLY_FROM("f", "memory_facts", "memory_fact_sources", "fact_id")
/* Ids of the facts that go: learned only from the set, plus every fact one of
 * them superseded, down the chain.  Deleting a correction would otherwise
 * revive the fact it replaced as current (superseded_by is ON DELETE SET NULL).
 * The chain stops at a fact learned outside any conversation (an import, a
 * save with no conversation): it stays, and is current again once its
 * correction is gone.
 * "+p.user_id" keeps the planner on idx_memory_facts_superseded_by: on user_id
 * it scans all of the user's facts per step of the recursion (seconds, under the
 * global database lock).  tests/test_memory_provenance.c pins the plan. */
#define MEMORY_FORGET_FACT_IDS                                                           \
   "WITH RECURSIVE forget_gone(id) AS (SELECT f.id FROM memory_facts f "                 \
   "WHERE " MEMORY_FORGET_FACT_WHERE                                                     \
   " UNION SELECT p.id FROM memory_facts p JOIN forget_gone g ON "                       \
   "p.superseded_by = g.id WHERE +p.user_id = ?1 AND p.origin_unsourced = 0) SELECT id " \
   "FROM forget_gone"
/* Ids of the relations that go (@p facts: SQL selecting the fact ids that go):
 * learned only from the set, or tied to a fact that goes and learned from no
 * conversation outside the set.  One tied to a fact but also taught elsewhere
 * stays (its fact_id is cleared).  A UNION so each arm uses its own index
 * ("+r.user_id": the fact arm seeks idx_memory_relations_fact). */
#define MEMORY_FORGET_RELATION_IDS_IN(facts)                                                  \
   "SELECT r.id FROM memory_relations r WHERE " MEMORY_FORGET_ONLY_FROM(                      \
       "r", "memory_relations", "memory_relation_sources",                                    \
       "relation_id") " UNION SELECT r.id FROM memory_relations r WHERE +r.user_id = ?1 AND " \
                      "r.fact_id IN (" facts ") AND r.origin_unsourced = 0 AND "              \
                      "(r.source_conversation_id IS NULL OR r.source_conversation_id "        \
                      "IN " MEMORY_FORGET_IN_SET ") AND NOT EXISTS (SELECT 1 FROM "           \
                      "memory_relation_sources x WHERE x.relation_id = r.id AND "             \
                      "x.conversation_id NOT IN " MEMORY_FORGET_IN_SET ")"
#define MEMORY_FORGET_RELATION_IDS MEMORY_FORGET_RELATION_IDS_IN(MEMORY_FORGET_FACT_IDS)
/* memory_preferences aliased "p" (sources: every conversation that gave its
 * current value). */
#define MEMORY_FORGET_PREF_WHERE \
   MEMORY_FORGET_ONLY_FROM("p", "memory_preferences", "memory_preference_sources", "preference_id")

/* Size for memory_db_internal_ids_json() output: "[", n ids of up to 20 digits
 * plus a comma each, "]", NUL. */
#define MEMORY_DB_IDS_JSON_SIZE(n) ((size_t)(n)*21 + 3)

/* Render @p ids as a JSON array ("[1,2,3]") for binding to json_each().
 * Returns false if @p out (of @p size bytes) is too small.  Pure helper. */
bool memory_db_internal_ids_json(const int64_t *ids, int n, char *out, size_t size);

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
