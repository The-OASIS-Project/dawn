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
 * Authentication Database - Conversation Event Log (background jobs Phase 2)
 *
 * The durable, step-granular record that makes a background job inspectable —
 * and the stream a TUI tails.  Never token-granular: live tokens live only in
 * the in-memory conv_stream ring, and final assistant text stays in `messages`.
 *
 * Kept out of auth_db_conv.c (already > 1500 lines) for the same cohesion
 * reason auth_db_jobs.c is, and follows that file's conventions exactly:
 * ad-hoc prepare/step/finalize (every path here is low-frequency — a handful of
 * events per turn, measured ~8/hour on the developer's live DB), constant or
 * parameterized SQL only, and the auth_db mutex held across each statement.
 *
 * SECURITY:
 *  - No user_id column (mirrors `messages`).  Every READ JOINs `conversations`
 *    and filters user_id — §8.3.  A bare WHERE conversation_id=? is a bug.
 *  - Payloads arrive already secret-redacted and size-capped (§8.6); this layer
 *    stores what it is given and never inspects it.
 *
 * See docs/BACKGROUND_JOBS_DESIGN.md §6 (contract) and §8 (invariants).
 */

#define AUTH_DB_INTERNAL_ALLOWED
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db_internal.h"
#include "logging.h"

/* =============================================================================
 * Append
 * ============================================================================= */

int conv_db_event_append(int64_t conv_id, const char *kind, const char *payload, int64_t *seq_out) {
   if (seq_out) {
      *seq_out = 0;
   }
   if (conv_id <= 0 || kind == NULL || kind[0] == '\0') {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* seq is assigned INSIDE the insert, so "read max, then write" can never
    * interleave: the whole statement runs under the auth_db write lock, and the
    * v72 UNIQUE(conversation_id, seq) index is the backstop if that assumption
    * ever breaks.  The correlated subquery is scoped to this conversation, so it
    * rides the same index rather than scanning the table.
    *
    * RETURNING gives us the assigned seq without a second round-trip — needed
    * because the live fan-out frame must carry the authoritative seq (a client
    * dedups on it). */
   static const char *SQL =
       "INSERT INTO conversation_events (conversation_id, seq, kind, payload, created_at) "
       "VALUES (?, (SELECT COALESCE(MAX(seq), 0) + 1 FROM conversation_events WHERE "
       "conversation_id = ?), ?, ?, ?) "
       "RETURNING seq";

   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, SQL, -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db_events: prepare append failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, conv_id);
   sqlite3_bind_int64(st, 2, conv_id);
   sqlite3_bind_text(st, 3, kind, -1, SQLITE_TRANSIENT);
   if (payload) {
      sqlite3_bind_text(st, 4, payload, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(st, 4);
   }
   sqlite3_bind_int64(st, 5, (int64_t)time(NULL));

   int rc = sqlite3_step(st);
   int64_t assigned = (rc == SQLITE_ROW) ? sqlite3_column_int64(st, 0) : 0;
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();

   if (rc != SQLITE_ROW) {
      OLOG_ERROR("auth_db_events: append (conv %lld, kind %s) failed", (long long)conv_id, kind);
      return AUTH_DB_FAILURE;
   }
   if (seq_out) {
      *seq_out = assigned;
   }
   return AUTH_DB_SUCCESS;
}

/* =============================================================================
 * Ownership-scoped read
 * ============================================================================= */

void conv_db_event_rows_free(conv_event_t *rows, int n) {
   if (rows == NULL) {
      return;
   }
   for (int i = 0; i < n; i++) {
      free(rows[i].payload);
      rows[i].payload = NULL;
   }
}

int conv_db_event_list(int64_t conv_id,
                       int user_id,
                       int64_t after_seq,
                       int max,
                       conv_event_t *out,
                       int *count_out) {
   if (count_out) {
      *count_out = 0;
   }
   if (conv_id <= 0 || user_id <= 0 || max <= 0 || out == NULL || count_out == NULL) {
      return AUTH_DB_INVALID;
   }

   AUTH_DB_LOCK_OR_FAIL();

   /* §8.3: the JOIN + user_id bind IS the ownership check.  A conversation the
    * caller doesn't own yields zero rows — same contract as conv_db_get_messages,
    * deliberately not a distinct FORBIDDEN result. */
   static const char *SQL =
       "SELECT e.id, e.conversation_id, e.seq, e.kind, e.payload, e.created_at "
       "FROM conversation_events e "
       "JOIN conversations c ON c.id = e.conversation_id "
       "WHERE e.conversation_id = ? AND c.user_id = ? AND e.seq > ? "
       "ORDER BY e.seq ASC LIMIT ?";

   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(s_db.db, SQL, -1, &st, NULL) != SQLITE_OK) {
      OLOG_ERROR("auth_db_events: prepare list failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return AUTH_DB_FAILURE;
   }
   sqlite3_bind_int64(st, 1, conv_id);
   sqlite3_bind_int(st, 2, user_id);
   sqlite3_bind_int64(st, 3, after_seq);
   sqlite3_bind_int(st, 4, max);

   int n = 0;
   while (n < max && sqlite3_step(st) == SQLITE_ROW) {
      conv_event_t *r = &out[n];
      memset(r, 0, sizeof(*r));
      r->id = sqlite3_column_int64(st, 0);
      r->conversation_id = sqlite3_column_int64(st, 1);
      r->seq = sqlite3_column_int64(st, 2);

      const unsigned char *kind = sqlite3_column_text(st, 3);
      if (kind) {
         snprintf(r->kind, sizeof(r->kind), "%s", (const char *)kind);
      }
      /* payload is NULL for a pruned row — the caller renders that as expired
       * rather than blank, so preserve the distinction instead of "". */
      const unsigned char *payload = sqlite3_column_text(st, 4);
      if (payload) {
         r->payload = strdup((const char *)payload);
         if (r->payload == NULL) {
            OLOG_ERROR("auth_db_events: OOM duplicating payload (conv %lld seq %lld)",
                       (long long)r->conversation_id, (long long)r->seq);
            sqlite3_finalize(st);
            AUTH_DB_UNLOCK();
            conv_db_event_rows_free(out, n);
            return AUTH_DB_FAILURE;
         }
      }
      r->created_at = (time_t)sqlite3_column_int64(st, 5);
      n++;
   }
   sqlite3_finalize(st);
   AUTH_DB_UNLOCK();

   *count_out = n;
   return AUTH_DB_SUCCESS;
}

/* =============================================================================
 * Retention (§8.8) — kind-aware
 * ============================================================================= */

int conv_db_event_prune_expired(int retention_days, int *nulled_out, int *deleted_out) {
   if (nulled_out) {
      *nulled_out = 0;
   }
   if (deleted_out) {
      *deleted_out = 0;
   }
   if (retention_days <= 0) {
      return AUTH_DB_SUCCESS; /* disabled — never prune */
   }

   AUTH_DB_LOCK_OR_FAIL();

   const int64_t cutoff = (int64_t)time(NULL) - ((int64_t)retention_days * 86400);

   /* Payload-null the bulky, secret-risk-bearing kinds but KEEP the row: the seq
    * chain stays dense enough that replay ordering and the client's last_seq
    * bookkeeping are unaffected, and the step is still visible as "payload
    * expired" rather than vanishing from the job's history. */
   static const char *SQL_NULL = "UPDATE conversation_events SET payload = NULL "
                                 "WHERE created_at < ? AND payload IS NOT NULL "
                                 "AND kind IN ('tool_call', 'tool_result', 'terminal_chunk')";

   /* status rows are pure heartbeat — the payload IS the content, so nulling
    * would leave a dead row.  Deleting is safe because reads are `seq > last_seq`
    * and nothing requires seq density. */
   static const char *SQL_DELETE =
       "DELETE FROM conversation_events WHERE created_at < ? AND kind = 'status'";

   int rc = AUTH_DB_SUCCESS;
   sqlite3_stmt *st = NULL;

   if (sqlite3_prepare_v2(s_db.db, SQL_NULL, -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, cutoff);
      if (sqlite3_step(st) == SQLITE_DONE && nulled_out) {
         *nulled_out = sqlite3_changes(s_db.db);
      }
      sqlite3_finalize(st);
   } else {
      OLOG_ERROR("auth_db_events: prepare prune-null failed: %s", sqlite3_errmsg(s_db.db));
      rc = AUTH_DB_FAILURE;
   }

   st = NULL;
   if (sqlite3_prepare_v2(s_db.db, SQL_DELETE, -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, cutoff);
      if (sqlite3_step(st) == SQLITE_DONE && deleted_out) {
         *deleted_out = sqlite3_changes(s_db.db);
      }
      sqlite3_finalize(st);
   } else {
      OLOG_ERROR("auth_db_events: prepare prune-delete failed: %s", sqlite3_errmsg(s_db.db));
      rc = AUTH_DB_FAILURE;
   }

   AUTH_DB_UNLOCK();
   return rc;
}
