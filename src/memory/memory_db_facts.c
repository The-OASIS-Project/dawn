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
 * Memory Database — Fact CRUD operations.
 *
 * Phase 6 split from memory_db.c — fact-side CRUD, search (keyword + BM25),
 * embedding storage, date-filtered windows, and fact-text deduplication
 * helpers.  Shared init/teardown + cross-domain helpers stay in memory_db.c.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_bm25.h"
#include "memory/memory_db.h"
#include "memory/memory_db_internal.h"
#include "memory/memory_db_provenance.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_embeddings_internal.h"
#include "memory/memory_similarity.h"
#include "memory/memory_stem.h"
#include "utils/string_utils.h"

/* Canonical fact category labels (v34).  Single source of truth referenced by
 * extraction allowlist (memory_extraction.c), tool enum_values (memory_tool.c),
 * and centroid seed table (memory_embeddings.c).  See memory_types.h. */
const char *const MEMORY_FACT_CATEGORIES[] = { "personal",    "professional", "relationships",
                                               "health",      "interests",    "practical",
                                               "preferences", "general",      NULL };
const int MEMORY_FACT_CATEGORY_COUNT = 8;

/* =============================================================================
 * Helper: Populate fact from statement row
 * ============================================================================= */

/* Reads from SELECTs ordered: id, user_id, fact_text, confidence, source,
 * created_at, last_accessed, access_count, superseded_by, category.
 * category appended last (col 9, v34) so existing column indices are preserved. */
static void populate_fact_from_row(sqlite3_stmt *stmt, memory_fact_t *fact) {
   fact->id = sqlite3_column_int64(stmt, 0);
   fact->user_id = sqlite3_column_int(stmt, 1);

   const char *text = (const char *)sqlite3_column_text(stmt, 2);
   if (text) {
      safe_strscpy(fact->fact_text, text);
   }

   fact->confidence = (float)sqlite3_column_double(stmt, 3);

   const char *source = (const char *)sqlite3_column_text(stmt, 4);
   if (source) {
      safe_strscpy(fact->source, source);
   }

   fact->created_at = (time_t)sqlite3_column_int64(stmt, 5);
   fact->last_accessed = (time_t)sqlite3_column_int64(stmt, 6);
   fact->access_count = sqlite3_column_int(stmt, 7);
   fact->superseded_by = sqlite3_column_int64(stmt, 8);

   const char *category = (const char *)sqlite3_column_text(stmt, 9);
   if (category) {
      safe_strscpy(fact->category, category);
   } else {
      safe_strscpy(fact->category, "general");
   }

   /* expires_at (v58) is NOT in the shared 10-column projection — the
    * keyword/list retrieval statements filter expired rows in SQL via the
    * expiry guard, so anything reaching this reader is non-expired.  Default to
    * 0 (durable); only memory_db_fact_get's by-id projection carries the column
    * and overwrites this after the call (the un-guarded semantic-path fetch). */
   fact->expires_at = 0;
}

/* Expiry retrieval guard: the timestamp the guarded retrieval statements compare
 * memory_facts.expires_at against.  When [memory] expire_enabled is off, returns
 * 0 so `(expires_at IS NULL OR expires_at >= 0)` admits every fact — disabling is
 * instant and non-mutating.  When on, returns wall-clock now so facts past their
 * expires_at drop out of retrieval (soft phase; the nightly prune_expired pass
 * does the hard delete).  Centralized so every guarded statement binds the same
 * value.  See atlas/dawn/memory/EPHEMERALITY_DESIGN.md (C3). */
static int64_t fact_expiry_guard_now(void) {
   return g_config.memory.expire_enabled ? (int64_t)time(NULL) : 0;
}

bool memory_db_fact_expiry_hidden(int64_t expires_at) {
   /* Mirror the SQL guard: kept when (expires_at IS NULL OR expires_at >= now);
    * hidden otherwise.  Disabled config or durable (0) fact → never hidden. */
   if (!g_config.memory.expire_enabled || expires_at <= 0)
      return false;
   return expires_at < (int64_t)time(NULL);
}

/* v48 FTS5 maintenance helper — caller must hold AUTH_DB_LOCK.  Forward
 * declared so memory_db_fact_create can call it (the delete counterpart is
 * memory_db_internal_fts5_delete_fact_locked, shared with memory_db_forget.c). */
static int fts5_insert_fact_stems_locked(int64_t fact_id, const char *fact_stems);

/* =============================================================================
 * Fact Operations
 * ============================================================================= */

int memory_db_fact_create_at(int user_id,
                             const char *fact_text,
                             float confidence,
                             const char *source,
                             const char *category,
                             const memory_provenance_t *prov,
                             int64_t created_at_override,
                             int64_t *id_out) {
   if (id_out)
      *id_out = 0;
   if (!fact_text || !source) {
      return MEMORY_DB_FAILURE;
   }

   /* category may be NULL — schema default 'general' applies via the column DEFAULT,
    * but we explicitly bind to keep the SQL pure (no default-fallback ambiguity
    * across SQLite versions). */
   const char *cat = (category && *category) ? category : "general";

   /* 0 sentinel = "use NOW()".  Callers that want extraction-time temporal
    * fidelity pass the source conversation's created_at; everyone else gets
    * the legacy time(NULL) behavior.  int64_t matches anchor_date /
    * valid_from / on-disk SQLite column type for consistency. */
   const int64_t created_at = (created_at_override > 0) ? created_at_override : (int64_t)time(NULL);

   /* Compute normalized hash for deduplication */
   uint32_t normalized_hash = memory_normalize_and_hash(fact_text);

   /* v48: pre-stem fact_text OUTSIDE the auth_db lock so the stemmer's
    * mutex stays a leaf lock (per ARCHITECTURE.md §"Mutex Lock Ordering
    * Hierarchy"; auth_db must remain leaf for SQLite writes).  Doing this
    * before AUTH_DB_LOCK_OR_FAIL trades a few microseconds of redundant
    * computation (no leak — discarded if INSERT fails) for cleaner lock
    * hierarchy and reduced lock hold time. */
   char fact_stems[MEMORY_FACT_STEMS_MAX];
   (void)memory_stem_string(fact_text, fact_stems, sizeof(fact_stems));

   AUTH_DB_LOCK_OR_FAIL();

   /* The fact, its keyword-index entry and its source commit together (a
    * savepoint nests inside a caller's transaction). */
   const bool sp = sqlite3_exec(s_db.db, "SAVEPOINT fact_create", NULL, NULL, NULL) == SQLITE_OK;

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_create;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, fact_text, -1, SQLITE_STATIC);
   sqlite3_bind_double(stmt, 3, confidence);
   sqlite3_bind_text(stmt, 4, source, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 5, cat, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 6, created_at);
   sqlite3_bind_int64(stmt, 7, (int64_t)normalized_hash);
   memory_db_internal_bind_provenance(stmt, 8, prov);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: fact_create failed: %s", sqlite3_errmsg(s_db.db));
      if (sp) {
         sqlite3_exec(s_db.db, "ROLLBACK TO fact_create", NULL, NULL, NULL);
         sqlite3_exec(s_db.db, "RELEASE fact_create", NULL, NULL, NULL);
      }
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   int64_t id = sqlite3_last_insert_rowid(s_db.db);

   /* v48: keep memory_facts_fts in sync so BM25 search can find this
    * fact.  Failure logged inside the helper but not propagated — search
    * works from the JOIN against memory_facts; a missing FTS5 row just
    * means this fact won't surface for keyword-rank queries until next
    * recompute.  Stems were computed pre-lock; pass them through to keep
    * the stemmer mutex out of the auth_db critical section. */
   (void)fts5_insert_fact_stems_locked(id, fact_stems);
   if (prov) {
      memory_db_internal_source_add_locked(MEMORY_SOURCE_FACT, id, prov->conv_id);
   }
   if (sp) {
      sqlite3_exec(s_db.db, "RELEASE fact_create", NULL, NULL, NULL);
   }

   AUTH_DB_UNLOCK();

   if (id_out)
      *id_out = id;

   OLOG_INFO("memory_db: created fact %ld for user %d (hash=%u, category=%s)", (long)id, user_id,
             normalized_hash, cat);
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_create(int user_id,
                          const char *fact_text,
                          float confidence,
                          const char *source,
                          const char *category,
                          const memory_provenance_t *prov,
                          int64_t *id_out) {
   return memory_db_fact_create_at(user_id, fact_text, confidence, source, category, prov,
                                   /*created_at_override*/ 0, id_out);
}

/* Per-fact category UPDATE used by the centroid backfill pass (v34).
 * Caller batches these inside a transaction to amortize lock cost. */
int memory_db_fact_update_category(int64_t fact_id, int user_id, const char *category) {
   if (!category || !*category || user_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   /* SQL filters on (id, user_id) — defense-in-depth CWE-639. */
   sqlite3_stmt *stmt = s_db.stmt_memory_fact_update_category;
   sqlite3_reset(stmt);
   sqlite3_bind_text(stmt, 1, category, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 2, fact_id);
   sqlite3_bind_int(stmt, 3, user_id);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();

   return (rc == SQLITE_DONE) ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_fact_list_general_embedded(int user_id,
                                         int64_t after_id,
                                         int dims,
                                         int max,
                                         int64_t *ids_out,
                                         float *embs_out,
                                         int *count_out,
                                         int64_t *last_id_out) {
   if (count_out)
      *count_out = 0;
   if (last_id_out)
      *last_id_out = after_id;
   if (user_id <= 0 || dims <= 0 || max <= 0 || !ids_out || !embs_out || !count_out)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT id, embedding FROM memory_facts "
                          "WHERE user_id = ? AND superseded_by IS NULL "
                          "  AND embedding IS NOT NULL AND id > ? "
                          "  AND category = 'general' "
                          "ORDER BY id ASC LIMIT ?",
                          -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, after_id);
   sqlite3_bind_int(stmt, 3, max);

   const int want = dims * (int)sizeof(float);
   int n = 0;
   int64_t last = after_id;
   int rc;
   while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
      const int64_t id = sqlite3_column_int64(stmt, 0);
      if (id > last)
         last = id;
      const void *blob = sqlite3_column_blob(stmt, 1);
      if (blob && sqlite3_column_bytes(stmt, 1) == want && n < max) {
         ids_out[n] = id;
         memcpy(embs_out + (size_t)n * (size_t)dims, blob, (size_t)want);
         n++;
      }
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   if (rc != SQLITE_DONE)
      return MEMORY_DB_FAILURE;
   *count_out = n;
   if (last_id_out)
      *last_id_out = last;
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_set_categories(int user_id,
                                  const int64_t *ids,
                                  const char *const *categories,
                                  int n,
                                  int *written_out) {
   if (written_out)
      *written_out = 0;
   if (user_id <= 0 || !ids || !categories || n < 0)
      return MEMORY_DB_FAILURE;
   if (n == 0)
      return MEMORY_DB_SUCCESS;

   AUTH_DB_LOCK_OR_FAIL();
   if (sqlite3_exec(s_db.db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) {
      OLOG_ERROR("memory_db: set_categories BEGIN failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   int written = 0;
   bool ok = true;
   sqlite3_stmt *stmt = s_db.stmt_memory_fact_update_category;
   for (int i = 0; i < n && ok; i++) {
      if (!categories[i] || !*categories[i])
         continue;
      sqlite3_reset(stmt);
      sqlite3_bind_text(stmt, 1, categories[i], -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(stmt, 2, ids[i]);
      sqlite3_bind_int(stmt, 3, user_id);
      ok = sqlite3_step(stmt) == SQLITE_DONE;
      /* An UPDATE that matches nothing still returns SQLITE_DONE; count real writes. */
      written += ok ? sqlite3_changes(s_db.db) : 0;
   }
   sqlite3_reset(stmt);
   if (!ok || sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
      OLOG_ERROR("memory_db: set_categories failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   AUTH_DB_UNLOCK();
   if (written_out)
      *written_out = written;
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_list_general(int user_id,
                                int64_t cursor_id,
                                memory_fact_t *out_facts,
                                int max_facts,
                                int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_facts || max_facts <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_list_general;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, cursor_id);
   sqlite3_bind_int(stmt, 3, max_facts);

   int count = 0;
   while (count < max_facts && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_fact_from_row(stmt, &out_facts[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_count_general(int user_id, int *count_out) {
   if (count_out)
      *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_count_general;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);

   int count = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      count = sqlite3_column_int(stmt, 0);
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_get(int64_t fact_id, int user_id, memory_fact_t *out_fact) {
   if (!out_fact || user_id <= 0) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* SQL filters on (id, user_id) — defense-in-depth CWE-639.  Foreign
    * rowids return zero rows → MEMORY_DB_NOT_FOUND (same response a
    * legitimately-missing fact would get; no oracle). */
   sqlite3_stmt *stmt = s_db.stmt_memory_fact_get;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, fact_id);
   sqlite3_bind_int(stmt, 2, user_id);

   int result = MEMORY_DB_NOT_FOUND;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      populate_fact_from_row(stmt, out_fact);
      /* This statement's projection carries expires_at as col 10 (the only
       * fact reader that does — see stmt_memory_fact_get).  populate_fact_from_row
       * defaulted it to 0; overwrite with the stored value (NULL → 0) so the
       * semantic-path expiry guard can decide.  v58. */
      out_fact->expires_at = sqlite3_column_int64(stmt, 10);
      result = MEMORY_DB_SUCCESS;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   return result;
}

int memory_db_fact_list(int user_id,
                        memory_fact_t *out_facts,
                        int max_facts,
                        int offset,
                        int *count_out) {
   return memory_db_fact_list_sorted(user_id, MEMORY_SORT_DEFAULT, out_facts, max_facts, offset,
                                     count_out);
}

int memory_db_fact_list_sorted(int user_id,
                               memory_list_sort_t sort,
                               memory_fact_t *out_facts,
                               int max_facts,
                               int offset,
                               int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_facts || max_facts <= 0) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* All three statements share the ?1 user / ?2 limit / ?3 offset / ?4 expiry
    * numbering (see auth_db_statements.c), so one bind block serves any of them. */
   sqlite3_stmt *stmt;
   switch (sort) {
      case MEMORY_SORT_CREATED_DESC:
         stmt = s_db.stmt_memory_fact_list_created_desc;
         break;
      case MEMORY_SORT_CREATED_ASC:
         stmt = s_db.stmt_memory_fact_list_created_asc;
         break;
      default:
         stmt = s_db.stmt_memory_fact_list;
         break;
   }
   if (!stmt) {
      AUTH_DB_UNLOCK();
      OLOG_ERROR("memory_db_fact_list_sorted: statement not prepared (sort=%d)", (int)sort);
      return MEMORY_DB_FAILURE;
   }

   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max_facts);
   sqlite3_bind_int(stmt, 3, offset);
   sqlite3_bind_int64(stmt, 4, fact_expiry_guard_now()); /* expiry guard (v58) */

   int count = 0;
   while (count < max_facts && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_fact_from_row(stmt, &out_facts[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

/* v48: FTS5 maintenance helpers.  Called from fact_create / fact_delete
 * to keep memory_facts_fts in sync.  No SQL trigger because stemming
 * runs in C (libstemmer cannot be invoked from SQL).
 *
 * Both variants take PRE-STEMMED input.  Stemming MUST run outside the
 * auth_db lock to keep the stemmer mutex a leaf lock — see
 * ARCHITECTURE.md §"Mutex Lock Ordering Hierarchy".  Callers compute
 * stems via memory_stem_string() before AUTH_DB_LOCK_OR_FAIL().
 *
 * Caller must already hold AUTH_DB_LOCK.  Returns 0 on success, non-zero
 * on FTS5 failure (logged at warning — search degrades gracefully when
 * an entry is missing or stale, so a failure here is not fatal). */
static int fts5_insert_fact_stems_locked(int64_t fact_id, const char *fact_stems) {
   if (fact_id <= 0 || !fact_stems)
      return 1;
   sqlite3_stmt *stmt = s_db.stmt_memory_facts_fts_insert;
   if (!stmt)
      return 1; /* migration not yet run on this DB */
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, fact_id);
   sqlite3_bind_text(stmt, 2, fact_stems, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);
   if (rc != SQLITE_DONE) {
      OLOG_WARNING("memory_db: FTS5 insert fact_id=%lld failed: %s", (long long)fact_id,
                   sqlite3_errmsg(s_db.db));
      return 1;
   }
   return 0;
}

/* Contentless FTS5 'delete' command requires the rowid AND the original
 * indexed content so FTS5 can decrement its token postings.  Caller must
 * already hold AUTH_DB_LOCK.  Caller must provide pre-stemmed content
 * (typically obtained by calling memory_stem_string on the live fact_text
 * BEFORE taking the lock — same stems the original insert used). */
int memory_db_internal_fts5_delete_fact_locked(int64_t fact_id, const char *fact_stems) {
   if (fact_id <= 0 || !fact_stems)
      return 1;
   sqlite3_stmt *del = s_db.stmt_memory_facts_fts_delete;
   if (!del)
      return 1; /* migration not yet run */
   sqlite3_reset(del);
   sqlite3_bind_int64(del, 1, fact_id);
   sqlite3_bind_text(del, 2, fact_stems, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(del);
   sqlite3_reset(del);
   if (rc != SQLITE_DONE) {
      OLOG_WARNING("memory_db: FTS5 delete fact_id=%lld failed: %s", (long long)fact_id,
                   sqlite3_errmsg(s_db.db));
      return 1;
   }
   return 0;
}

/* build_fts5_match_expr was promoted to memory_bm25_build_match_expr (memory_bm25.c)
 * when document search became the second FTS5 consumer.  See memory_bm25.h. */

/* v48: BM25-ranked fact search via FTS5.
 *
 * Pipeline: query → memory_stem_string (lowercase + Porter2 stem) →
 *           build_fts5_match_expr (quoted OR-tokens) → FTS5 MATCH →
 *           sigmoid-normalize raw BM25 score per row.
 *
 * `out_scores[i]` ends up in [0, 1] from memory_bm25_normalize using
 * the (midpoint, steepness) row picked for `count_out` query terms.
 *
 * On migration-incomplete DBs (stmt_memory_fact_search_bm25 NULL), or
 * empty queries / zero stems, returns SUCCESS with *count_out=0 so the
 * caller can fall back to the legacy LIKE path. */
int memory_db_fact_search_bm25(int user_id,
                               const char *query,
                               memory_fact_t *out_facts,
                               float *out_scores,
                               int max_facts,
                               int *count_out) {
   return memory_db_fact_search_bm25_since(user_id, query, /*since_ts*/ 0, out_facts, out_scores,
                                           max_facts, count_out);
}

int memory_db_fact_search_bm25_since(int user_id,
                                     const char *query,
                                     time_t since_ts,
                                     memory_fact_t *out_facts,
                                     float *out_scores,
                                     int max_facts,
                                     int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!query || !out_facts || !out_scores || max_facts <= 0)
      return MEMORY_DB_FAILURE;

   /* 1. Stem the query and count terms for sigmoid param selection.
    * Sized to MEMORY_FACT_STEMS_MAX so query and fact-side stem buffers
    * use the same constant; in practice queries are much shorter than
    * facts. */
   char stems[MEMORY_FACT_STEMS_MAX];
   int n_terms = memory_stem_string(query, stems, sizeof(stems));
   if (n_terms <= 0)
      return MEMORY_DB_SUCCESS; /* nothing to search; not an error */

   /* 2. Build the FTS5 MATCH expression. */
   char match_expr[2048];
   int n_emitted = memory_bm25_build_match_expr(stems, match_expr, sizeof(match_expr));
   if (n_emitted <= 0)
      return MEMORY_DB_SUCCESS;
   (void)n_terms; /* kept for diagnostics; n_emitted is the authoritative count */

   /* 3. Pick sigmoid parameters once for the whole result set.  Use
    * n_emitted (the actual count of OR-tokens FTS5 ranks against), not
    * n_terms (pre-truncation stem count) — they diverge only when
    * build_fts5_match_expr hits its buffer cap, but the MATCH-side
    * count is the one the BM25 score reflects. */
   float midpoint = 0.0f;
   float steepness = 0.0f;
   memory_bm25_get_params(n_emitted, &midpoint, &steepness);

   AUTH_DB_LOCK_OR_FAIL();
   const bool windowed = (since_ts > 0);
   sqlite3_stmt *stmt = windowed ? s_db.stmt_memory_fact_search_bm25_since
                                 : s_db.stmt_memory_fact_search_bm25;
   if (!stmt) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_SUCCESS; /* migration not yet run */
   }
   sqlite3_reset(stmt);
   sqlite3_bind_text(stmt, 1, match_expr, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, user_id);
   if (windowed) {
      sqlite3_bind_int64(stmt, 3, (int64_t)since_ts);
      sqlite3_bind_int(stmt, 4, max_facts);
      sqlite3_bind_int64(stmt, 5, fact_expiry_guard_now()); /* expiry guard (v58) */
   } else {
      sqlite3_bind_int(stmt, 3, max_facts);
      sqlite3_bind_int64(stmt, 4, fact_expiry_guard_now()); /* expiry guard (v58) */
   }

   int count = 0;
   while (count < max_facts && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_fact_from_row(stmt, &out_facts[count]);
      /* Column 10 carries bm25() — negative-for-relevant per FTS5
       * convention.  Flip sign so memory_bm25_normalize sees the
       * "larger is better" convention it (and Mem0) expect. */
      double raw = sqlite3_column_double(stmt, 10);
      out_scores[count] = memory_bm25_normalize((float)(-raw), midpoint, steepness);
      count++;
   }
   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();

   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_search(int user_id,
                          const char *keywords,
                          memory_fact_t *out_facts,
                          int max_facts,
                          int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!keywords || !out_facts || max_facts <= 0) {
      return MEMORY_DB_FAILURE;
   }

   char pattern[MEMORY_FACT_TEXT_MAX];
   memory_db_internal_build_like_pattern(keywords, pattern, sizeof(pattern));

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_search;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 3, max_facts);
   sqlite3_bind_int64(stmt, 4, fact_expiry_guard_now()); /* expiry guard (v58) */

   int count = 0;
   while (count < max_facts && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_fact_from_row(stmt, &out_facts[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_update_access(int64_t fact_id, int user_id) {
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_update_access;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, (int64_t)time(NULL));
   sqlite3_bind_double(stmt, 2, (double)g_config.memory.access_reinforcement_boost);
   sqlite3_bind_int64(stmt, 3, fact_id);
   sqlite3_bind_int(stmt, 4, user_id);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();
   return (rc == SQLITE_DONE) ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_fact_reinforce_citation(int64_t fact_id, int user_id) {
   if (user_id <= 0 || fact_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* Binds: 1=boost, 2=id, 3=user_id.  The 1 h cooldown + first-cite-bumps logic
    * lives in the statement's WHERE (see auth_db_statements.c) — a cooldown-blocked
    * or foreign-user call is a legitimate no-op (0 rows changed), not a failure. */
   sqlite3_stmt *stmt = s_db.stmt_memory_fact_reinforce_citation;
   sqlite3_reset(stmt);
   sqlite3_bind_double(stmt, 1, (double)g_config.memory.citation_reinforcement_boost);
   sqlite3_bind_int64(stmt, 2, fact_id);
   sqlite3_bind_int(stmt, 3, user_id);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();
   return (rc == SQLITE_DONE) ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_fact_mark_unsourced(int64_t fact_id, int user_id) {
   if (fact_id <= 0 || user_id <= 0) {
      return MEMORY_DB_FAILURE;
   }
   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "UPDATE memory_facts SET origin_unsourced = 1 WHERE id = ? AND "
                               "user_id = ? AND origin_unsourced = 0",
                               -1, &stmt, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, fact_id);
      sqlite3_bind_int(stmt, 2, user_id);
      rc = sqlite3_step(stmt);
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return rc == SQLITE_DONE ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_fact_update_confidence(int64_t fact_id, int user_id, float confidence) {
   if (user_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* SQL filters on (id, user_id) — defense-in-depth CWE-639. */
   sqlite3_stmt *stmt = s_db.stmt_memory_fact_update_confidence;
   sqlite3_reset(stmt);
   sqlite3_bind_double(stmt, 1, confidence);
   sqlite3_bind_int64(stmt, 2, fact_id);
   sqlite3_bind_int(stmt, 3, user_id);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();
   return (rc == SQLITE_DONE) ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

/* Phase 0 v47: write the subject_entity_id FK onto an existing fact row.
 * Ad-hoc prepared statement (not cached in s_db.stmt_*) because this runs
 * once per fact insert during extraction — bounded by extraction cadence,
 * not on a hot retrieval path.  Move to cached statement if profiling
 * shows it's significant. */
int memory_db_fact_set_subject_entity(int64_t fact_id, int user_id, int64_t entity_id) {
   if (fact_id <= 0 || entity_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   sqlite3_stmt *stmt = NULL;
   /* CWE-639 defense-in-depth: EXISTS subquery refuses to write a
    * subject_entity_id that belongs to a different user.  SQLite FK
    * constraints would already catch a cross-user FK at write time, but
    * matching the shape used by memory_db_fact_supersede keeps the
    * defense at the SQL layer regardless of FK pragma state. */
   int rc = sqlite3_prepare_v2(s_db.db,
                               "UPDATE memory_facts SET subject_entity_id = ? "
                               "WHERE id = ? AND user_id = ? AND EXISTS "
                               "(SELECT 1 FROM memory_entities WHERE id = ? AND user_id = ?)",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("memory_db: prepare fact_set_subject_entity failed: %s",
                   sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int64(stmt, 1, entity_id);
   sqlite3_bind_int64(stmt, 2, fact_id);
   sqlite3_bind_int(stmt, 3, user_id);
   sqlite3_bind_int64(stmt, 4, entity_id);
   sqlite3_bind_int(stmt, 5, user_id);
   int step_rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   if (step_rc != SQLITE_DONE)
      return MEMORY_DB_FAILURE;
   return (changes > 0) ? MEMORY_DB_SUCCESS : MEMORY_DB_NOT_FOUND;
}

int memory_db_fact_set_expires_at(int64_t fact_id, int user_id, int64_t expires_at) {
   if (fact_id <= 0 || user_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   /* Ad-hoc prepare (mirrors set_subject_entity) — the expiry-set path runs at
    * most once per freshly-extracted transient fact, not on the hot retrieval
    * path, so a cached statement isn't warranted.  CWE-639: (id, user_id) scope.
    * expires_at <= 0 stores NULL (clears any expiry → durable). */
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db, "UPDATE memory_facts SET expires_at = ? WHERE id = ? AND user_id = ?", -1, &stmt,
       NULL);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("memory_db: prepare fact_set_expires_at failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   if (expires_at > 0)
      sqlite3_bind_int64(stmt, 1, expires_at);
   else
      sqlite3_bind_null(stmt, 1);
   sqlite3_bind_int64(stmt, 2, fact_id);
   sqlite3_bind_int(stmt, 3, user_id);
   int step_rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   if (step_rc != SQLITE_DONE)
      return MEMORY_DB_FAILURE;
   return (changes > 0) ? MEMORY_DB_SUCCESS : MEMORY_DB_NOT_FOUND;
}

int memory_db_fact_set_note_doc_id(int64_t fact_id, int user_id, int64_t note_doc_id) {
   if (fact_id <= 0 || user_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   /* Ad-hoc prepare (mirrors set_expires_at) — the memory→note bridge link (v61)
    * is set once at note-save time, never on the hot path.  CWE-639: the fact is
    * (id, user_id)-scoped AND the linked document is ownership-validated via the
    * EXISTS subquery (mirrors set_subject_entity) — the schema FK references
    * documents(id) with no user predicate, so a cross-user link must be refused
    * here.  note_doc_id <= 0 stores NULL (clears the link); the EXISTS guard is
    * skipped on a NULL clear (?1 is bound NULL → the OR ?1 IS NULL branch). */
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "UPDATE memory_facts SET note_doc_id = ?1 WHERE id = ?2 AND user_id = ?3 "
       "AND (?1 IS NULL OR EXISTS (SELECT 1 FROM documents WHERE id = ?1 AND user_id = ?3))",
       -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("memory_db: prepare fact_set_note_doc_id failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   if (note_doc_id > 0)
      sqlite3_bind_int64(stmt, 1, note_doc_id);
   else
      sqlite3_bind_null(stmt, 1);
   sqlite3_bind_int64(stmt, 2, fact_id);
   sqlite3_bind_int(stmt, 3, user_id);
   int step_rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   if (step_rc != SQLITE_DONE)
      return MEMORY_DB_FAILURE;
   return (changes > 0) ? MEMORY_DB_SUCCESS : MEMORY_DB_NOT_FOUND;
}

int memory_db_fact_find_by_note_doc_id(int user_id, int64_t note_doc_id, int64_t *fact_id_out) {
   if (fact_id_out)
      *fact_id_out = 0;
   if (user_id <= 0 || note_doc_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
   /* Ad-hoc prepare — runs once per note save/delete, not on the hot path.
    * At most one gloss per note (the bridge upserts), so LIMIT 1. */
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db, "SELECT id FROM memory_facts WHERE user_id = ? AND note_doc_id = ? LIMIT 1", -1,
       &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("memory_db: prepare fact_find_by_note_doc_id failed: %s",
                   sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, note_doc_id);
   int64_t id = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      id = sqlite3_column_int64(stmt, 0);
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   if (fact_id_out)
      *fact_id_out = id;
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_supersede(int64_t old_fact_id, int64_t new_fact_id, int user_id) {
   if (user_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* SQL enforces (a) old_fact_id is owned by user_id AND (b) new_fact_id
    * is owned by user_id via EXISTS subquery.  CWE-639 defense-in-depth
    * + cross-user pointer prevention (foreign new_fact_id would let a
    * caller "hide" another user's fact from their own retrieval). */
   sqlite3_stmt *stmt = s_db.stmt_memory_fact_supersede;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, new_fact_id);
   sqlite3_bind_int64(stmt, 2, old_fact_id);
   sqlite3_bind_int(stmt, 3, user_id);
   sqlite3_bind_int64(stmt, 4, new_fact_id);
   sqlite3_bind_int(stmt, 5, user_id);

   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();
   return (rc == SQLITE_DONE) ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_fact_delete(int64_t fact_id, int user_id) {
   /* Step 1: look up the fact under the auth_db lock so we know the
    * owner + can pull the live fact_text for FTS5 maintenance.  Released
    * BEFORE stemming so the stemmer mutex stays a leaf lock per the
    * ARCHITECTURE.md acquire-order invariant. */
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* stmt_memory_fact_get now filters on (id, user_id) at the SQL layer
    * (CWE-639 defense-in-depth) — a foreign rowid lookup returns zero
    * rows here, so the existing `found` gate doubles as the wrong-user
    * gate (and the legacy `owner != user_id` post-check is now
    * redundant but kept for belt-and-braces clarity). */
   sqlite3_stmt *get = s_db.stmt_memory_fact_get;
   sqlite3_reset(get);
   sqlite3_bind_int64(get, 1, fact_id);
   sqlite3_bind_int(get, 2, user_id);
   int owner = 0;
   /* The whole text: the insert stemmed all of it, and a contentless FTS5
    * delete must name exactly the tokens it indexed. */
   char *text_buf = NULL;
   bool found = false;
   if (sqlite3_step(get) == SQLITE_ROW) {
      owner = sqlite3_column_int(get, 1);
      const unsigned char *t = sqlite3_column_text(get, 2);
      text_buf = strdup(t ? (const char *)t : "");
      found = text_buf != NULL;
   }
   sqlite3_reset(get);
   AUTH_DB_UNLOCK();

   /* SECURITY: verify ownership before any side effects — the SQL filter
    * above is the primary defense (CWE-639); this is belt-and-braces.
    * Don't distinguish "wrong user" from "doesn't exist" — same response
    * a legitimate not-found request would get. */
   if (!found || owner != user_id) {
      free(text_buf);
      return MEMORY_DB_NOT_FOUND;
   }

   /* Step 2: stem outside any lock so the stemmer mutex doesn't compose
    * with auth_db.  Same stems the original insert used because fact_text
    * is immutable post-create. */
   char fact_stems[MEMORY_FACT_STEMS_MAX];
   (void)memory_stem_string(text_buf, fact_stems, sizeof(fact_stems));
   free(text_buf);

   /* Step 3: re-take the lock and run the FTS5 delete + facts DELETE
    * atomically (within the same critical section). */
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   (void)memory_db_internal_fts5_delete_fact_locked(fact_id, fact_stems);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_delete;
   sqlite3_reset(stmt);
   sqlite3_bind_int64(stmt, 1, fact_id);
   sqlite3_bind_int(stmt, 2, user_id);

   int rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      return MEMORY_DB_FAILURE;
   }
   if (changes > 0) {
      /* The embedding cache holds an (id, embedding) entry for this now-deleted
       * fact — mark it dirty so the next access reloads (mirrors the invalidation
       * in cleanup_meta_facts).  Without this, a deleted fact lingers in the cache
       * and re-surfaces in cosine search / find_duplicates. */
      memory_embeddings_invalidate_cache();
      return MEMORY_DB_SUCCESS;
   }
   return MEMORY_DB_NOT_FOUND;
}

int memory_db_fact_find_similar(int user_id,
                                const char *fact_text,
                                memory_fact_t *out_facts,
                                int max_facts,
                                int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!fact_text || !out_facts || max_facts <= 0) {
      return MEMORY_DB_FAILURE;
   }

   /* Extract key words from fact for similarity search */
   char pattern[MEMORY_FACT_TEXT_MAX];
   memory_db_internal_build_like_pattern(fact_text, pattern, sizeof(pattern));

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_find_similar;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_STATIC);

   int count = 0;
   while (count < max_facts && sqlite3_step(stmt) == SQLITE_ROW) {
      out_facts[count].id = sqlite3_column_int64(stmt, 0);
      const char *text = (const char *)sqlite3_column_text(stmt, 1);
      if (text) {
         safe_strscpy(out_facts[count].fact_text, text);
      }
      out_facts[count].confidence = (float)sqlite3_column_double(stmt, 2);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_find_by_hash(int user_id,
                                uint32_t hash,
                                memory_fact_t *out_facts,
                                int max_facts,
                                int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_facts || max_facts <= 0 || hash == 0) {
      return MEMORY_DB_FAILURE;
   }

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_find_by_hash;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, (int64_t)hash);

   int count = 0;
   while (count < max_facts && sqlite3_step(stmt) == SQLITE_ROW) {
      out_facts[count].id = sqlite3_column_int64(stmt, 0);
      const char *text = (const char *)sqlite3_column_text(stmt, 1);
      if (text) {
         safe_strscpy(out_facts[count].fact_text, text);
      }
      out_facts[count].confidence = (float)sqlite3_column_double(stmt, 2);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

/* v48 FTS5 orphan cleanup status (Phase 9.5):
 *   memory_db_facts_delete_by_patterns NOW fires fts5_delete_fact_stems_locked
 *   per victim before the bulk DELETE — see the FTS5 sync pre-pass.
 *   memory_db_fact_prune_superseded / memory_db_fact_prune_stale below still
 *   leave orphan FTS5 rows.  Live search recall is unaffected because the
 *   BM25 search JOIN's `memory_facts mf ON mf.id = memory_facts_fts.rowid`
 *   filter drops orphans, but they accumulate token-posting storage and bias
 *   global IDF (~1-5% orphan ratio at realistic prune cadences — below
 *   bench-noise threshold).  Close both prune paths in the same pattern when
 *   pruning gets exercised in production. */
int memory_db_fact_prune_superseded(int user_id, int retention_days, int *count_out) {
   if (count_out)
      *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   time_t cutoff = time(NULL) - (retention_days * 24 * 60 * 60);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_prune_superseded;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, (int64_t)cutoff);

   int rc = sqlite3_step(stmt);
   int deleted = sqlite3_changes(s_db.db);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: prune_superseded failed: %s", sqlite3_errmsg(s_db.db));
      return MEMORY_DB_FAILURE;
   }

   if (count_out)
      *count_out = deleted;
   if (deleted > 0) {
      OLOG_INFO("memory_db: pruned %d superseded facts for user %d", deleted, user_id);
   }
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_prune_stale(int user_id, int stale_days, float min_confidence, int *count_out) {
   if (count_out)
      *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   time_t cutoff = time(NULL) - (stale_days * 24 * 60 * 60);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_prune_stale;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, (int64_t)cutoff);
   sqlite3_bind_double(stmt, 3, min_confidence);

   int rc = sqlite3_step(stmt);
   int deleted = sqlite3_changes(s_db.db);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: prune_stale failed: %s", sqlite3_errmsg(s_db.db));
      return MEMORY_DB_FAILURE;
   }

   if (count_out)
      *count_out = deleted;
   if (deleted > 0) {
      OLOG_INFO("memory_db: pruned %d stale facts for user %d", deleted, user_id);
   }
   return MEMORY_DB_SUCCESS;
}

/* v58 (C3): hard-delete facts whose expiry passed more than @p retention_days
 * ago — the "hard phase" of fact ephemerality.  The retrieval guard already
 * hides them (soft phase) the moment expires_at < now; this reclaims the row
 * after the recoverable window.  retention_days = 0 collapses to hard-expire on
 * the reference date (cutoff == now).  Leaf-lock single DELETE, copies nothing
 * out — mirrors prune_superseded.  Carries the same known FTS5-orphan debt as
 * prune_superseded / prune_stale (see the block comment above prune_superseded);
 * close all three together when pruning is exercised in production. */
int memory_db_fact_prune_expired(int user_id, int retention_days, int *count_out) {
   if (count_out)
      *count_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   time_t cutoff = time(NULL) - (retention_days * 24 * 60 * 60);

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_prune_expired;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, (int64_t)cutoff);

   int rc = sqlite3_step(stmt);
   int deleted = sqlite3_changes(s_db.db);
   sqlite3_reset(stmt);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: prune_expired failed: %s", sqlite3_errmsg(s_db.db));
      return MEMORY_DB_FAILURE;
   }

   if (count_out)
      *count_out = deleted;
   if (deleted > 0) {
      OLOG_INFO("memory_db: pruned %d expired facts for user %d", deleted, user_id);
   }
   return MEMORY_DB_SUCCESS;
}

int memory_db_facts_delete_by_patterns(int user_id,
                                       const char *const *patterns,
                                       int n_patterns,
                                       bool dry_run,
                                       int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!patterns || n_patterns <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   /* Build a single statement: SELECT count first (for both modes — surface
    * the would-delete count even on dry-run), then conditionally DELETE.
    * Patterns OR'd via repeated `fact_text LIKE ?` clauses, all under the
    * same user_id scope.  ESCAPE '\\' lets a future caller embed literal
    * %/_ via backslash without surprise.
    *
    * Cap at MAX_DELETE_PATTERNS to keep the SQL prepared-statement size
    * bounded and prevent a malicious admin client from constructing a
    * multi-MB query.  64 is well above what any caller actually needs. */
   const int MAX_DELETE_PATTERNS = 64;
   if (n_patterns > MAX_DELETE_PATTERNS) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   /* Construct WHERE clause: " AND (fact_text LIKE ? ESCAPE '\\' OR ... )"
    * Worst case at MAX=64: ~30 bytes/pattern × 64 + base = ~2 KB.  Use a
    * fixed buffer comfortably larger. */
   char where_buf[4096];
   int off = snprintf(where_buf, sizeof(where_buf), "(");
   for (int i = 0; i < n_patterns; i++) {
      int n = snprintf(where_buf + off, sizeof(where_buf) - off, "%sfact_text LIKE ? ESCAPE '\\'",
                       i == 0 ? "" : " OR ");
      if (n < 0 || n >= (int)(sizeof(where_buf) - off)) {
         AUTH_DB_UNLOCK();
         return MEMORY_DB_FAILURE;
      }
      off += n;
   }
   if (off + 2 >= (int)sizeof(where_buf)) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   where_buf[off++] = ')';
   where_buf[off] = '\0';

   /* Exempt memory→note bridge glosses (v61) from pattern bulk-delete — a
    * gloss naming a saved note must not be caught by a fact_text pattern.
    * Appended to where_buf so the COUNT, victim-select, and DELETE below
    * (all "WHERE user_id = ? AND %s") share the exclusion. */
   int ex = snprintf(where_buf + off, sizeof(where_buf) - off, " AND note_doc_id IS NULL");
   if (ex < 0 || ex >= (int)(sizeof(where_buf) - off)) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   off += ex;

   /* COUNT pass — needed for both --dry-run report and post-DELETE
    * confirmation; SQLite's `changes()` after DELETE would give us the
    * same number on the actual-delete path, but running COUNT first lets
    * the dry-run path skip the DELETE entirely. */
   char sql[5120];
   int sql_n = snprintf(sql, sizeof(sql),
                        "SELECT COUNT(*) FROM memory_facts WHERE user_id = ? AND %s", where_buf);
   if (sql_n < 0 || sql_n >= (int)sizeof(sql)) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_stmt *count_stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &count_stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(count_stmt, 1, user_id);
   for (int i = 0; i < n_patterns; i++) {
      sqlite3_bind_text(count_stmt, 2 + i, patterns[i], -1, SQLITE_STATIC);
   }

   int matched = 0;
   if (sqlite3_step(count_stmt) == SQLITE_ROW)
      matched = sqlite3_column_int(count_stmt, 0);
   sqlite3_finalize(count_stmt);

   if (dry_run || matched == 0) {
      AUTH_DB_UNLOCK();
      if (count_out)
         *count_out = matched;
      return MEMORY_DB_SUCCESS;
   }

   /* FTS5 sync pre-pass (CWE-672 close): pull (id, fact_stems) for every
    * victim row and fire fts5_delete_fact_stems_locked per row BEFORE the
    * bulk DELETE.  Without this, the FTS5 virtual table keeps orphan
    * rowids that point to deleted rows.  The post-DELETE search SQL
    * defends via JOIN-back to memory_facts with user_id, so live recall
    * stays correct — but any future caller of memory_facts_fts that skips
    * the JOIN guard would see deleted-fact stems reattributed (rowids can
    * be reused).  Closing the symptom at write time keeps the invariant
    * local to this writer rather than spread across every reader. */
   /* fact_stems is NOT a column on memory_facts — it lives only on the FTS5
    * virtual table memory_facts_fts.  Read fact_text and recompute stems in C
    * the same way the writers do (memory_db_fact_create line 134, single-row
    * delete line 764). */
   sql_n = snprintf(sql, sizeof(sql),
                    "SELECT id, fact_text FROM memory_facts WHERE user_id = ? AND %s", where_buf);
   if (sql_n < 0 || sql_n >= (int)sizeof(sql)) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_stmt *victim_stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &victim_stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(victim_stmt, 1, user_id);
   for (int i = 0; i < n_patterns; i++) {
      sqlite3_bind_text(victim_stmt, 2 + i, patterns[i], -1, SQLITE_STATIC);
   }

   sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
   while (sqlite3_step(victim_stmt) == SQLITE_ROW) {
      int64_t fid = sqlite3_column_int64(victim_stmt, 0);
      const unsigned char *text = sqlite3_column_text(victim_stmt, 1);
      char stems_buf[MEMORY_FACT_STEMS_MAX];
      (void)memory_stem_string(text ? (const char *)text : "", stems_buf, sizeof(stems_buf));
      (void)memory_db_internal_fts5_delete_fact_locked(fid, stems_buf);
   }
   sqlite3_finalize(victim_stmt);

   /* DELETE pass.  Same transaction as the FTS5 sync above — a mid-pattern
    * failure rolls back both the index sync and the row delete. */
   sql_n = snprintf(sql, sizeof(sql), "DELETE FROM memory_facts WHERE user_id = ? AND %s",
                    where_buf);
   if (sql_n < 0 || sql_n >= (int)sizeof(sql)) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_stmt *del_stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &del_stmt, NULL) != SQLITE_OK) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(del_stmt, 1, user_id);
   for (int i = 0; i < n_patterns; i++) {
      sqlite3_bind_text(del_stmt, 2 + i, patterns[i], -1, SQLITE_STATIC);
   }
   int rc = sqlite3_step(del_stmt);
   int deleted = sqlite3_changes(s_db.db);
   sqlite3_finalize(del_stmt);

   if (rc != SQLITE_DONE) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL);
   AUTH_DB_UNLOCK();

   /* Cache invalidation — embedding cache holds (id, embedding) pairs that
    * are now stale.  Mark dirty so the next access reloads. */
   memory_embeddings_invalidate_cache();

   if (count_out)
      *count_out = deleted;
   OLOG_INFO("memory_db: cleanup_meta_facts deleted %d rows for user %d (n_patterns=%d)", deleted,
             user_id, n_patterns);
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * Date-Filtered Fact Queries
 * ============================================================================= */

int memory_db_fact_search_since(int user_id,
                                const char *keywords,
                                time_t since_ts,
                                memory_fact_t *out_facts,
                                int max_facts,
                                int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!keywords || !out_facts || max_facts <= 0) {
      return MEMORY_DB_FAILURE;
   }

   char pattern[MEMORY_FACT_TEXT_MAX];
   memory_db_internal_build_like_pattern(keywords, pattern, sizeof(pattern));

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = s_db.stmt_memory_fact_search_since;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_STATIC);
   sqlite3_bind_int64(stmt, 3, (int64_t)since_ts);
   sqlite3_bind_int(stmt, 4, max_facts);
   sqlite3_bind_int64(stmt, 5, fact_expiry_guard_now()); /* expiry guard (v58) */

   int count = 0;
   while (count < max_facts && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_fact_from_row(stmt, &out_facts[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_list_window(int user_id,
                               time_t since_ts,
                               time_t until_ts,
                               bool sort_asc,
                               memory_fact_t *out_facts,
                               int max_facts,
                               int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_facts || max_facts <= 0) {
      return MEMORY_DB_FAILURE;
   }

   /* until_ts == 0 means "until now"; resolve to INT64_MAX so the prepared
    * statement's `created_at <= ?` bound is permissive without needing a
    * separate "no upper bound" code path. */
   int64_t until_resolved = (until_ts > 0) ? (int64_t)until_ts : INT64_MAX;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = sort_asc ? s_db.stmt_memory_fact_list_window_asc
                                 : s_db.stmt_memory_fact_list_window_desc;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, (int64_t)since_ts);
   sqlite3_bind_int64(stmt, 3, until_resolved);
   sqlite3_bind_int(stmt, 4, max_facts);
   sqlite3_bind_int64(stmt, 5, fact_expiry_guard_now()); /* expiry guard (v58) */

   int count = 0;
   while (count < max_facts && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_fact_from_row(stmt, &out_facts[count]);
      count++;
   }

   sqlite3_reset(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * Fact Embedding Operations (Semantic Search)
 * ============================================================================= */

int memory_db_fact_update_embedding(int user_id,
                                    int64_t fact_id,
                                    const float *embedding,
                                    int dims,
                                    float norm) {
   if (!embedding || dims <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_fact_update_embedding);
   sqlite3_bind_blob(s_db.stmt_memory_fact_update_embedding, 1, embedding,
                     dims * (int)sizeof(float), SQLITE_TRANSIENT);
   sqlite3_bind_double(s_db.stmt_memory_fact_update_embedding, 2, (double)norm);
   sqlite3_bind_int64(s_db.stmt_memory_fact_update_embedding, 3, fact_id);
   sqlite3_bind_int(s_db.stmt_memory_fact_update_embedding, 4, user_id);

   int rc = sqlite3_step(s_db.stmt_memory_fact_update_embedding);
   sqlite3_reset(s_db.stmt_memory_fact_update_embedding);

   AUTH_DB_UNLOCK();

   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db: update_embedding failed for fact %ld: %s", (long)fact_id,
                 sqlite3_errmsg(s_db.db));
      return MEMORY_DB_FAILURE;
   }

   return MEMORY_DB_SUCCESS;
}

/* The facts the in-memory cache may load: current (not superseded, not expired)
 * with an embedding of the expected size.  ?1 user, ?2 expiry guard
 * (fact_expiry_guard_now), ?3 embedding bytes. */
#define FACT_CACHE_WHERE                                                   \
   "user_id = ?1 AND superseded_by IS NULL AND embedding IS NOT NULL AND " \
   "length(embedding) = ?3 AND (expires_at IS NULL OR expires_at >= ?2)"
#define FACT_CACHE_COLUMNS "id, embedding, embedding_norm, created_at, note_doc_id"

/* Caller holds the auth_db lock.  Prepare @p sql and bind the shared WHERE's
 * parameters; NULL on failure. */
static sqlite3_stmt *fact_cache_prepare(const char *sql, int user_id, int expected_dims) {
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("memory_db: preparing a fact cache query failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_finalize(st);
      return NULL;
   }
   sqlite3_bind_int(st, 1, user_id);
   sqlite3_bind_int64(st, 2, fact_expiry_guard_now());
   sqlite3_bind_int(st, 3, expected_dims * (int)sizeof(float));
   return st;
}

int memory_db_fact_foreach_embedding(int user_id,
                                     int expected_dims,
                                     int limit,
                                     memory_fact_embedding_fn fn,
                                     void *ctx) {
   if (!fn || limit <= 0 || expected_dims <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   /* How many there are decides the query.  Usually they all fit and order
    * doesn't matter, so a plain scan.  Over the limit, rank ids without their
    * embeddings (sorting the blobs is what's expensive), then fetch the kept
    * ones by id. */
   int total = 0;
   sqlite3_stmt *st = fact_cache_prepare(
       "SELECT COUNT(*) FROM memory_facts WHERE " FACT_CACHE_WHERE, user_id, expected_dims);
   int step = st ? sqlite3_step(st) : SQLITE_ERROR;
   if (step == SQLITE_ROW) {
      total = sqlite3_column_int(st, 0);
   }
   sqlite3_finalize(st);
   st = NULL;
   if (step != SQLITE_ROW) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   if (total <= limit) {
      st = fact_cache_prepare("SELECT " FACT_CACHE_COLUMNS
                              " FROM memory_facts WHERE " FACT_CACHE_WHERE,
                              user_id, expected_dims);
   } else {
      /* ?4 recently-used cutoff, ?5 now, ?6 recency scale, ?7 limit (see
       * MEMORY_CACHE_RECENT_USE_SEC for the order). */
      st = fact_cache_prepare(
          "SELECT " FACT_CACHE_COLUMNS " FROM memory_facts WHERE id IN ("
          "SELECT id FROM memory_facts WHERE " FACT_CACHE_WHERE " "
          "ORDER BY (MAX(COALESCE(last_accessed, 0), COALESCE(last_cited, 0)) >= ?4 "
          "OR COALESCE(note_doc_id, 0) > 0) DESC, "
          "COALESCE(confidence, 0) / (1.0 + MAX(0, ?5 - MAX(created_at, COALESCE(last_accessed, "
          "0), "
          "COALESCE(last_cited, 0))) / ?6) DESC, "
          "id DESC LIMIT ?7)",
          user_id, expected_dims);
      if (st) {
         const int64_t now = (int64_t)time(NULL);
         sqlite3_bind_int64(st, 4, now - MEMORY_CACHE_RECENT_USE_SEC);
         sqlite3_bind_int64(st, 5, now);
         sqlite3_bind_double(st, 6, (double)MEMORY_CACHE_RECENCY_SCALE_SEC);
         sqlite3_bind_int(st, 7, limit);
      }
   }
   if (!st) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   int rc = MEMORY_DB_SUCCESS;
   while ((step = sqlite3_step(st)) == SQLITE_ROW) {
      const memory_fact_embedding_row_t row = {
         .id = sqlite3_column_int64(st, 0),
         .embedding = sqlite3_column_blob(st, 1),
         .norm = (float)sqlite3_column_double(st, 2),
         .created_at = sqlite3_column_int64(st, 3),
         .note_doc_id = sqlite3_column_int64(st, 4), /* NULL reads back as 0 */
      };
      if (!row.embedding || fn(&row, total, ctx) != SUCCESS) {
         rc = MEMORY_DB_FAILURE;
         break;
      }
   }
   if (rc == MEMORY_DB_SUCCESS && step != SQLITE_DONE) {
      OLOG_ERROR("memory_db: loading fact embeddings for user %d failed: %s", user_id,
                 sqlite3_errmsg(s_db.db));
      rc = MEMORY_DB_FAILURE;
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();
   return rc;
}

int memory_db_fact_list_without_embedding(int user_id,
                                          int64_t after_id,
                                          int expected_dims,
                                          int64_t *out_ids,
                                          char out_texts[][512],
                                          int max_count,
                                          int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_ids || !out_texts || max_count <= 0 || expected_dims <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_reset(s_db.stmt_memory_fact_list_without_embedding);
   sqlite3_bind_int(s_db.stmt_memory_fact_list_without_embedding, 1, user_id);
   sqlite3_bind_int64(s_db.stmt_memory_fact_list_without_embedding, 2, after_id);
   sqlite3_bind_int(s_db.stmt_memory_fact_list_without_embedding, 3, expected_dims);
   sqlite3_bind_int(s_db.stmt_memory_fact_list_without_embedding, 4, max_count);

   int count = 0;
   while (count < max_count &&
          sqlite3_step(s_db.stmt_memory_fact_list_without_embedding) == SQLITE_ROW) {
      out_ids[count] = sqlite3_column_int64(s_db.stmt_memory_fact_list_without_embedding, 0);
      const char *text = (const char *)sqlite3_column_text(
          s_db.stmt_memory_fact_list_without_embedding, 1);
      if (text) {
         safe_strscpy(out_texts[count], text);
      } else {
         out_texts[count][0] = '\0';
      }
      count++;
   }

   sqlite3_reset(s_db.stmt_memory_fact_list_without_embedding);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}

int memory_db_fact_users_needing_backfill(int expected_dims,
                                          int after_user_id,
                                          int *out_user_ids,
                                          int max_count,
                                          int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_user_ids || max_count <= 0 || expected_dims <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();

   /* Sweep-only (at boot, after a re-index, or to continue an overflowed sweep),
    * so an ad-hoc prepare is cheaper than a statement held for the process
    * lifetime.  Second arm: users whose one-shot category pass hasn't run yet
    * (reset by an admin re-extract, or pre-v34 data) but whose facts are all
    * embedded — the pass runs at the end of a backfill, so they need queuing too. */
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT user_id FROM memory_facts "
       "WHERE superseded_by IS NULL AND fact_text != '' AND user_id > ?1 "
       "AND (embedding IS NULL OR length(embedding)/4 != ?2) "
       "UNION "
       "SELECT u.id FROM users u WHERE u.id > ?1 AND u.categories_backfilled_at = 0 "
       "AND EXISTS (SELECT 1 FROM memory_facts f WHERE f.user_id = u.id "
       "AND f.superseded_by IS NULL) "
       "ORDER BY 1 LIMIT ?3",
       -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_WARNING("memory_db: prepare fact_users_needing_backfill failed: %s",
                   sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, after_user_id);
   sqlite3_bind_int(stmt, 2, expected_dims);
   sqlite3_bind_int(stmt, 3, max_count);

   int count = 0;
   while (count < max_count && sqlite3_step(stmt) == SQLITE_ROW) {
      out_user_ids[count++] = sqlite3_column_int(stmt, 0);
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = count;
   return MEMORY_DB_SUCCESS;
}
