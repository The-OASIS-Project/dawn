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
 * Memory provenance reader implementations.
 *
 * One single-record reader (`memory_db_fact_get_source`) and four batch
 * readers — one per record type (facts/relations/summaries/preferences) —
 * that return the source-conversation back-link recorded at extraction time.
 *
 * Privacy: each batch query JOINs `conversations c ON ... = c.id` and filters
 * `c.is_private = 0` in SQL, so private-conversation rows return
 * out_conv_ids[i] = 0 in the output array.  The single-record path checks
 * privacy via `conv_db_is_private()` after the lock is released (auth_db
 * mutex is non-reentrant).
 *
 * Truncation safety: each batch builder caps N at MAX_PROVENANCE_BATCH and
 * returns MEMORY_DB_FAILURE on overflow (fail-closed).  The static_assert
 * below ties the cap to the SQL buffer size — change either and the build
 * stops.
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include "memory/memory_db_provenance.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "core/session_manager.h" /* session_fact_source_t */
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_db_internal.h"
#include "utils/string_utils.h"

/* SQL buffer for the batch IN-clause builder.  Sized so that
 * MAX_PROVENANCE_BATCH IDs fit with comfortable headroom for the SELECT/JOIN
 * prefix and the ")" suffix.  Keep the cap aligned with this constant. */
#define PROV_SQL_BUF_SIZE 1024

/* Worst-case per-ID encoding: leading "," + up to 20 digits of int64. */
#define PROV_ID_MAX_CHARS 21

/* The fixed SELECT/JOIN prefix and the closing ")" must fit alongside N IDs.
 * Measured prefix sizes (longest table-name variant = "memory_preferences"):
 *   SELECT/JOIN/WHERE/AND ... ≈ 277 bytes; closing ")" + NUL ≈ 2 bytes.
 * Reserve 320 bytes (= 277 + 43 headroom) so a future variant that grows the
 * format string by a few %s still fits.  If you change build_in_clause's
 * format string, re-measure and bump this if needed. */
#define PROV_FIXED_OVERHEAD 320

_Static_assert(MAX_PROVENANCE_BATCH *PROV_ID_MAX_CHARS + PROV_FIXED_OVERHEAD <= PROV_SQL_BUF_SIZE,
               "MAX_PROVENANCE_BATCH × per-ID width + prefix exceeds SQL buffer; "
               "shrink the cap or grow PROV_SQL_BUF_SIZE.");

/* =============================================================================
 * Single-record fact source reader (moved verbatim from memory_db.c).
 * ============================================================================= */

int memory_db_fact_get_source(int64_t fact_id,
                              int user_id,
                              int64_t *conv_id_out,
                              int64_t *start_out,
                              int64_t *end_out) {
   if (!conv_id_out || !start_out || !end_out)
      return MEMORY_DB_FAILURE;
   *conv_id_out = 0;
   *start_out = 0;
   *end_out = 0;

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT user_id, source_conversation_id, "
                               "       source_msg_id_start, source_msg_id_end "
                               "FROM memory_facts WHERE id = ?",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int64(stmt, 1, fact_id);
   rc = sqlite3_step(stmt);

   if (rc != SQLITE_ROW) {
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_NOT_FOUND;
   }

   int owner = sqlite3_column_int(stmt, 0);
   int col1_type = sqlite3_column_type(stmt, 1);
   int64_t conv_id = (col1_type != SQLITE_NULL) ? sqlite3_column_int64(stmt, 1) : 0;
   int64_t msg_start = (sqlite3_column_type(stmt, 2) != SQLITE_NULL) ? sqlite3_column_int64(stmt, 2)
                                                                     : 0;
   int64_t msg_end = (sqlite3_column_type(stmt, 3) != SQLITE_NULL) ? sqlite3_column_int64(stmt, 3)
                                                                   : 0;
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   /* Ownership check */
   if (owner != user_id)
      return MEMORY_DB_NOT_FOUND;

   /* No provenance recorded */
   if (col1_type == SQLITE_NULL || conv_id <= 0)
      return MEMORY_DB_NOT_FOUND;

   /* Privacy check: auth_db mutex is NOT re-entrant, so we must release it before
    * calling conv_db_is_private() which acquires the same mutex.  The race window
    * between the unlock above and this re-acquire is benign: the ownership check
    * already passed using the stored user_id column, and a concurrent privacy flip
    * would affect at most one stale read — acceptable per the design doc. */
   bool is_private = false;
   conv_db_is_private(conv_id, user_id, &is_private);
   if (is_private)
      return MEMORY_DB_NOT_FOUND;

   *conv_id_out = conv_id;
   *start_out = msg_start;
   *end_out = msg_end;
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * Batch source readers — one per record type.
 *
 * All four share the IN-clause SQL build pattern.  `build_in_clause` writes the
 * SELECT into `sql` for table `tbl` (must be a literal string, not user input)
 * with privacy JOIN.  Returns SUCCESS on a successful build, FAILURE on
 * truncation (which is now impossible given the static_assert, but the runtime
 * check is retained as a defense-in-depth fence).
 * ============================================================================= */

static int build_in_clause(char *sql,
                           size_t sql_size,
                           const char *tbl_alias, /* "f", "r", "s", or "p" */
                           const char *tbl_name,  /* table name (string literal) */
                           const int64_t *ids,
                           int n) {
   int off = snprintf(sql, sql_size,
                      "SELECT %s.id, %s.source_conversation_id, "
                      "%s.source_msg_id_start, %s.source_msg_id_end "
                      "FROM %s %s "
                      "JOIN conversations c ON %s.source_conversation_id = c.id "
                      "WHERE %s.user_id = ? AND c.is_private = 0 "
                      "AND %s.source_conversation_id IS NOT NULL "
                      "AND %s.id IN (%lld",
                      tbl_alias, tbl_alias, tbl_alias, tbl_alias, tbl_name, tbl_alias, tbl_alias,
                      tbl_alias, tbl_alias, tbl_alias, (long long)ids[0]);
   if (off < 0 || (size_t)off >= sql_size)
      return MEMORY_DB_FAILURE;
   for (int i = 1; i < n; i++) {
      int written = snprintf(sql + off, sql_size - (size_t)off, ",%lld", (long long)ids[i]);
      /* off and written are snprintf results bounded by a small SQL buffer */
      // NOLINTNEXTLINE(bugprone-misplaced-widening-cast)
      if (written < 0 || (size_t)(off + written) >= sql_size - 2)
         return MEMORY_DB_FAILURE;
      off += written;
   }
   /* off and written are snprintf results bounded by a small SQL buffer */
   // NOLINTNEXTLINE(bugprone-misplaced-widening-cast)
   if ((size_t)(off + 2) >= sql_size)
      return MEMORY_DB_FAILURE;
   sql[off++] = ')';
   sql[off] = '\0';
   return MEMORY_DB_SUCCESS;
}

/* Process one chunk (n ≤ MAX_PROVENANCE_BATCH).  Output indices are relative
 * to the chunk slice — caller advances output pointers between chunks.
 * Fail-closed if n > cap (the static_assert + this runtime guard form the
 * SQL-truncation defense-in-depth). */
static int batch_get_sources_one(int user_id,
                                 const char *tbl_alias,
                                 const char *tbl_name,
                                 const int64_t *ids,
                                 int n,
                                 int64_t *out_conv_ids,
                                 int64_t *out_starts,
                                 int64_t *out_ends) {
   if (n <= 0 || n > MAX_PROVENANCE_BATCH) {
      OLOG_WARNING("provenance batch oversize: n=%d cap=%d (table=%s); failing closed", n,
                   MAX_PROVENANCE_BATCH, tbl_name);
      return MEMORY_DB_FAILURE;
   }

   for (int i = 0; i < n; i++) {
      out_conv_ids[i] = 0;
      out_starts[i] = 0;
      out_ends[i] = 0;
   }

   /* IDs are int64 values from each table's AUTOINCREMENT column — no injection
    * risk.  Privacy filtered in SQL via JOIN on conversations.is_private. */
   char sql[PROV_SQL_BUF_SIZE];
   if (build_in_clause(sql, sizeof(sql), tbl_alias, tbl_name, ids, n) != MEMORY_DB_SUCCESS)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_FAIL();
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);

   while (sqlite3_step(stmt) == SQLITE_ROW) {
      int64_t row_id = sqlite3_column_int64(stmt, 0);
      for (int i = 0; i < n; i++) {
         if (ids[i] == row_id) {
            out_conv_ids[i] = sqlite3_column_int64(stmt, 1);
            out_starts[i] = sqlite3_column_int64(stmt, 2);
            out_ends[i] = sqlite3_column_int64(stmt, 3);
            break;
         }
      }
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return MEMORY_DB_SUCCESS;
}

/* Public-facing batch reader — accepts any positive N.  Slices the input into
 * MAX_PROVENANCE_BATCH-sized chunks and runs `batch_get_sources_one` per chunk.
 * Each chunk acquires its own AUTH_DB_LOCK cycle, so an N=500 bench query is
 * ~16 lock cycles — acceptable for a memory-search-cadence operation, well
 * above the typical LLM-tool top-K (≤ 10).
 *
 * The cap on a single SQL statement remains constant (defense-in-depth against
 * snprintf truncation); only the public surface chunks. */
static int batch_get_sources(int user_id,
                             const char *tbl_alias,
                             const char *tbl_name,
                             const int64_t *ids,
                             int n,
                             int64_t *out_conv_ids,
                             int64_t *out_starts,
                             int64_t *out_ends) {
   if (!ids || !out_conv_ids || !out_starts || !out_ends || n <= 0)
      return MEMORY_DB_FAILURE;

   int offset = 0;
   while (offset < n) {
      int chunk = n - offset;
      if (chunk > MAX_PROVENANCE_BATCH)
         chunk = MAX_PROVENANCE_BATCH;
      int rc = batch_get_sources_one(user_id, tbl_alias, tbl_name, ids + offset, chunk,
                                     out_conv_ids + offset, out_starts + offset, out_ends + offset);
      if (rc != MEMORY_DB_SUCCESS)
         return rc;
      offset += chunk;
   }
   return MEMORY_DB_SUCCESS;
}

int memory_db_facts_get_sources(int user_id,
                                const int64_t *fact_ids,
                                int n,
                                int64_t *out_conv_ids,
                                int64_t *out_starts,
                                int64_t *out_ends) {
   return batch_get_sources(user_id, "f", "memory_facts", fact_ids, n, out_conv_ids, out_starts,
                            out_ends);
}

int memory_db_relations_get_sources(int user_id,
                                    const int64_t *relation_ids,
                                    int n,
                                    int64_t *out_conv_ids,
                                    int64_t *out_starts,
                                    int64_t *out_ends) {
   return batch_get_sources(user_id, "r", "memory_relations", relation_ids, n, out_conv_ids,
                            out_starts, out_ends);
}

int memory_db_summaries_get_sources(int user_id,
                                    const int64_t *summary_ids,
                                    int n,
                                    int64_t *out_conv_ids,
                                    int64_t *out_starts,
                                    int64_t *out_ends) {
   return batch_get_sources(user_id, "s", "memory_summaries", summary_ids, n, out_conv_ids,
                            out_starts, out_ends);
}

int memory_db_prefs_get_sources(int user_id,
                                const int64_t *pref_ids,
                                int n,
                                int64_t *out_conv_ids,
                                int64_t *out_starts,
                                int64_t *out_ends) {
   return batch_get_sources(user_id, "p", "memory_preferences", pref_ids, n, out_conv_ids,
                            out_starts, out_ends);
}

/* =============================================================================
 * Provenance extend (paraphrase-dedup write path).
 *
 * Single-statement read-modify-write under the auth_db mutex.  Same-conv
 * branch widens the message range; cross-conv branch keeps most-recent.
 * Older or equal cross-conv mentions are silent no-ops so the dedup loop
 * does not bounce a fact's provenance back and forth on retroactive
 * extractions.
 * ============================================================================= */

/* memory_db_fact_provenance_extend() with the auth_db lock held; @p
 * learned_here also records the fact as learned in a conversation
 * (origin_unsourced = 0), in the same commit. */
static int provenance_extend_locked(int64_t fact_id,
                                    int user_id,
                                    int64_t new_conv_id,
                                    int64_t new_msg_start,
                                    int64_t new_msg_end,
                                    bool learned_here) {
   if (new_conv_id <= 0)
      return MEMORY_DB_FAILURE;

   /* Read existing provenance + ownership in one statement. */
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT user_id, source_conversation_id, "
                               "       source_msg_id_start, source_msg_id_end "
                               "FROM memory_facts WHERE id = ?",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int64(stmt, 1, fact_id);
   rc = sqlite3_step(stmt);
   if (rc != SQLITE_ROW) {
      sqlite3_finalize(stmt);
      return MEMORY_DB_NOT_FOUND;
   }
   int owner = sqlite3_column_int(stmt, 0);
   bool had_conv = (sqlite3_column_type(stmt, 1) != SQLITE_NULL);
   int64_t cur_conv = had_conv ? sqlite3_column_int64(stmt, 1) : 0;
   int64_t cur_start = (sqlite3_column_type(stmt, 2) != SQLITE_NULL) ? sqlite3_column_int64(stmt, 2)
                                                                     : 0;
   int64_t cur_end = (sqlite3_column_type(stmt, 3) != SQLITE_NULL) ? sqlite3_column_int64(stmt, 3)
                                                                   : 0;
   sqlite3_finalize(stmt);

   if (owner != user_id) {
      return MEMORY_DB_NOT_FOUND;
   }

   /* Every contributing conversation is kept (memory_fact_sources), whichever
    * one the fact's own source columns end up pointing at. */
   int64_t write_conv = cur_conv;
   int64_t write_start = cur_start;
   int64_t write_end = cur_end;

   if (!had_conv || cur_conv == 0) {
      /* No prior provenance — adopt the new triple wholesale. */
      write_conv = new_conv_id;
      write_start = new_msg_start;
      write_end = new_msg_end;
   } else if (cur_conv == new_conv_id) {
      /* Same conversation — widen to cover all messages that mention the
       * fact within that conversation. */
      if (new_msg_start > 0 && (write_start == 0 || new_msg_start < write_start))
         write_start = new_msg_start;
      if (new_msg_end > write_end)
         write_end = new_msg_end;
   } else if (new_conv_id > cur_conv) {
      /* More recent conversation reinforces the fact — replace.  Older
       * mention's provenance is dropped from this single-slot schema. */
      write_conv = new_conv_id;
      write_start = new_msg_start;
      write_end = new_msg_end;
   } else {
      /* Older or equal mention — keep existing provenance. */
      write_conv = cur_conv;
   }

   /* Nothing to change in the fact's columns (an older mention, or the same one
    * again) and nothing to record about where it was learned: just the source. */
   const bool columns_change = write_conv != cur_conv || write_start != cur_start ||
                               write_end != cur_end || !had_conv;
   if (!columns_change && !learned_here) {
      memory_db_internal_source_add_locked(MEMORY_SOURCE_FACT, fact_id, new_conv_id);
      return MEMORY_DB_SUCCESS;
   }

   /* The source and the fact's columns commit together (one commit, not two).
    * @p learned_here marks the fact learned in a conversation on every path
    * (the columns may already point at a newer one). */
   const bool sp = sqlite3_exec(s_db.db, "SAVEPOINT provenance_extend", NULL, NULL, NULL) ==
                   SQLITE_OK;
   memory_db_internal_source_add_locked(MEMORY_SOURCE_FACT, fact_id, new_conv_id);
   sqlite3_stmt *upd = NULL;
   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE memory_facts SET source_conversation_id = ?1, "
                           "source_msg_id_start = ?2, source_msg_id_end = ?3, "
                           "origin_unsourced = CASE WHEN ?6 THEN 0 ELSE origin_unsourced END "
                           "WHERE id = ?4 AND user_id = ?5",
                           -1, &upd, NULL);
   if (rc == SQLITE_OK) {
      sqlite3_bind_int64(upd, 1, write_conv);
      sqlite3_bind_int64(upd, 2, write_start);
      sqlite3_bind_int64(upd, 3, write_end);
      sqlite3_bind_int64(upd, 4, fact_id);
      sqlite3_bind_int(upd, 5, user_id);
      sqlite3_bind_int(upd, 6, learned_here ? 1 : 0);
      rc = sqlite3_step(upd);
      sqlite3_finalize(upd);
   }
   if (sp) {
      if (rc != SQLITE_DONE) {
         sqlite3_exec(s_db.db, "ROLLBACK TO provenance_extend", NULL, NULL, NULL);
      }
      sqlite3_exec(s_db.db, "RELEASE provenance_extend", NULL, NULL, NULL);
   }
   if (rc != SQLITE_DONE)
      return MEMORY_DB_FAILURE;
   return MEMORY_DB_SUCCESS;
}

static int provenance_extend_impl(int64_t fact_id,
                                  int user_id,
                                  int64_t new_conv_id,
                                  int64_t new_msg_start,
                                  int64_t new_msg_end,
                                  bool learned_here) {
   AUTH_DB_LOCK_OR_FAIL();
   const int rc = provenance_extend_locked(fact_id, user_id, new_conv_id, new_msg_start,
                                           new_msg_end, learned_here);
   AUTH_DB_UNLOCK();
   return rc;
}

void memory_db_fact_attach_sources(const session_fact_source_t *facts, int count, int64_t conv_id) {
   if (!facts || count <= 0 || conv_id <= 0) {
      return;
   }
   AUTH_DB_LOCK_OR_RETURN_VOID();
   /* One commit for the batch. */
   const bool sp = sqlite3_exec(s_db.db, "SAVEPOINT attach_sources", NULL, NULL, NULL) == SQLITE_OK;
   int checked_user = 0;
   bool owned = false;
   for (int i = 0; i < count; i++) {
      const session_fact_source_t *f = &facts[i];
      if (f->fact_id <= 0 || f->user_id <= 0) {
         continue;
      }
      /* A turn's conversation can be deleted while the turn still runs: never
       * point a fact at one that is gone (or isn't this user's). */
      if (f->user_id != checked_user) {
         checked_user = f->user_id;
         owned = conv_db_owned_locked(conv_id, f->user_id) == AUTH_DB_SUCCESS;
      }
      if (!owned || provenance_extend_locked(f->fact_id, f->user_id, conv_id, 0, 0, f->created) !=
                        MEMORY_DB_SUCCESS) {
         OLOG_WARNING("memory: could not record conversation %lld as fact %lld's source",
                      (long long)conv_id, (long long)f->fact_id);
      }
   }
   if (sp && sqlite3_exec(s_db.db, "RELEASE attach_sources", NULL, NULL, NULL) != SQLITE_OK) {
      OLOG_WARNING("memory: could not commit conversation %lld as facts' source: %s",
                   (long long)conv_id, sqlite3_errmsg(s_db.db));
      sqlite3_exec(s_db.db, "ROLLBACK TO attach_sources", NULL, NULL, NULL);
      sqlite3_exec(s_db.db, "RELEASE attach_sources", NULL, NULL, NULL);
   }
   AUTH_DB_UNLOCK();
}

void memory_db_fact_attach_source(int64_t fact_id, int user_id, bool created, int64_t conv_id) {
   const session_fact_source_t f = { .fact_id = fact_id, .user_id = user_id, .created = created };
   memory_db_fact_attach_sources(&f, 1, conv_id);
}

int memory_db_fact_provenance_extend(int64_t fact_id,
                                     int user_id,
                                     int64_t new_conv_id,
                                     int64_t new_msg_start,
                                     int64_t new_msg_end) {
   return provenance_extend_impl(fact_id, user_id, new_conv_id, new_msg_start, new_msg_end, false);
}

/* =============================================================================
 * Per-conversation lookups
 *
 * Used when a user marks a conversation private and asks to forget what was
 * learned from it.  Rare, user-initiated calls, so ad-hoc prepares.  The
 * conversation set binds as a JSON array (json_each).
 * ============================================================================= */

/* Run a single-int COUNT query bound to (user_id, conv_ids_json). */
/* One-row count query bound as ?1 = user_id, ?2 = the conversation ids; the
 * row's first column into @p out, and its second (when @p out2) into @p out2. */
static int count_by_conversations2(const char *sql,
                                   int user_id,
                                   const char *ids,
                                   int *out,
                                   int *out2) {
   *out = 0;
   if (out2) {
      *out2 = 0;
   }
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      OLOG_WARNING("memory_db: prepare learned-count failed: %s", sqlite3_errmsg(s_db.db));
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, ids, -1, SQLITE_STATIC);
   int rc = sqlite3_step(stmt);
   if (rc == SQLITE_ROW) {
      *out = sqlite3_column_int(stmt, 0);
      if (out2) {
         *out2 = sqlite3_column_int(stmt, 1);
      }
   }
   sqlite3_finalize(stmt);
   return (rc == SQLITE_ROW) ? MEMORY_DB_SUCCESS : MEMORY_DB_FAILURE;
}

int memory_db_conversations_learned_count(int user_id,
                                          const int64_t *conv_ids,
                                          int n_conv,
                                          memory_conv_learned_t *out) {
   if (!out || user_id <= 0 || !conv_ids || n_conv <= 0) {
      return MEMORY_DB_FAILURE;
   }
   memset(out, 0, sizeof(*out));
   char ids[AUTH_DB_IDS_JSON_SIZE(CONV_CHAIN_MAX)];
   if (n_conv > CONV_CHAIN_MAX ||
       !auth_db_internal_ids_json_into(conv_ids, n_conv, ids, sizeof(ids))) {
      return MEMORY_DB_FAILURE;
   }
#define IN_CONVS " WHERE user_id = ?1 AND source_conversation_id IN " MEMORY_FORGET_IN_SET
   AUTH_DB_LOCK_OR_FAIL();
   /* Current and outdated (superseded) facts in one evaluation of the set. */
   int rc = count_by_conversations2("SELECT COALESCE(SUM(superseded_by IS NULL), 0), "
                                    "COALESCE(SUM(superseded_by IS NOT NULL), 0) FROM "
                                    "memory_facts WHERE user_id = ?1 AND id IN "
                                    "(" MEMORY_FORGET_FACT_IDS ")",
                                    user_id, ids, &out->facts, &out->outdated);
   if (rc == MEMORY_DB_SUCCESS) {
      rc = count_by_conversations2("SELECT COUNT(*) FROM memory_summaries" IN_CONVS, user_id, ids,
                                   &out->summaries, NULL);
   }
   if (rc == MEMORY_DB_SUCCESS) {
      rc = count_by_conversations2("SELECT COUNT(*) FROM (" MEMORY_FORGET_RELATION_IDS ")", user_id,
                                   ids, &out->relations, NULL);
   }
   if (rc == MEMORY_DB_SUCCESS) {
      rc = count_by_conversations2(
          "SELECT COUNT(*) FROM memory_preferences p WHERE " MEMORY_FORGET_PREF_WHERE, user_id, ids,
          &out->preferences, NULL);
   }
#undef IN_CONVS
   AUTH_DB_UNLOCK();
   return rc;
}
