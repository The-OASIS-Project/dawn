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
 * Memory citation signal — capture side.  See include/memory/memory_citation.h.
 */

#include "memory/memory_citation.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define AUTH_DB_INTERNAL_ALLOWED   /* this module owns a memory-table writer */
#include "auth/auth_db_internal.h" /* s_db, AUTH_DB_LOCK_* */
#include "config/dawn_config.h"    /* g_config */
#include "core/session_manager.h"  /* session_t, citation_stash_t, MAX_CITATION_STASH */
#include "logging.h"

/* CSV buffer for injected/cited item_ids.  Each item_id <= 64 bytes; 16 max
 * ordinals -> ~1 KB is generous headroom. */
#define CITE_CSV_MAX 1280

/* WebUI Context-panel gold-highlight delivery.  Weak no-op here keeps this
 * Layer-2 capture WebUI-agnostic; webui_broadcasts.c provides the strong impl
 * (Layer 4).  When ENABLE_WEBUI is off (or no browser is connected) the call is
 * a no-op and the audit still runs. */
__attribute__((weak)) void webui_broadcast_context_citations(int user_id,
                                                             int64_t conv_id,
                                                             int64_t turn_id,
                                                             const char *cited_ids_csv) {
   (void)user_id;
   (void)conv_id;
   (void)turn_id;
   (void)cited_ids_csv;
}

/* Append `s` to a comma-separated CSV buffer, bounded (never overflows). */
static void csv_append(char *buf, size_t bufsz, size_t *len, const char *s) {
   if (*len >= bufsz - 1) {
      return;
   }
   int n = snprintf(buf + *len, bufsz - *len, "%s%s", (*len > 0) ? "," : "", s);
   if (n > 0) {
      *len += ((size_t)n < bufsz - *len) ? (size_t)n : (bufsz - *len - 1);
   }
}

/* Insert one audit row.  Best-effort telemetry: logs and returns on any failure. */
static void audit_insert(int64_t conv_id,
                         int64_t msg_id,
                         int user_id,
                         const char *injected,
                         const char *cited,
                         const char *injected_scores,
                         int dropped) {
   AUTH_DB_LOCK_OR_RETURN_VOID();
   sqlite3_stmt *stmt = NULL;
   const char *sql =
       "INSERT INTO memory_citation_audit "
       "(conversation_id, message_id, user_id, ts, injected_ids, cited_ids, injected_scores, "
       "dropped_count) "
       "VALUES (?, ?, ?, strftime('%s','now'), ?, ?, ?, ?)";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      OLOG_ERROR("memory_citation: audit insert prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return;
   }
   sqlite3_bind_int64(stmt, 1, conv_id);
   sqlite3_bind_int64(stmt, 2, msg_id);
   sqlite3_bind_int(stmt, 3, user_id);
   sqlite3_bind_text(stmt, 4, injected ? injected : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, cited ? cited : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 6, injected_scores ? injected_scores : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 7, dropped);
   if (sqlite3_step(stmt) != SQLITE_DONE) {
      OLOG_WARNING("memory_citation: audit insert failed: %s", sqlite3_errmsg(s_db.db));
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
}

void memory_citation_capture(session_t *session, const char *response_text) {
   if (session == NULL || response_text == NULL || !g_config.memory.citation_enabled) {
      return;
   }

   /* Snapshot the per-turn stash under history_mutex (its documented guard).
    * INVARIANT: correctness depends on every finalizer call site being preceded,
    * on the SAME session, by session_dispatch_user_turn()'s stash-clear.  A new
    * finalizer caller that reaches here without a dispatch-clear would attribute
    * a stale injected/cited set to the wrong turn. */
   citation_stash_t stash;
   pthread_mutex_lock(&session->history_mutex);
   stash = session->citation_stash;
   pthread_mutex_unlock(&session->history_mutex);

   if (stash.count <= 0) {
      return; /* no [M#] surfaced this turn — nothing to audit */
   }
   if (stash.count > MAX_CITATION_STASH) {
      stash.count = MAX_CITATION_STASH; /* defensive */
   }

   /* All surfaced item_ids -> injected CSV, with a parallel CSV of the per-item
    * final_score (same order) so the audit can split score by used-vs-unused.
    * Both share the CITE_CSV_MAX bound; under extreme pressure `injected` (wider
    * elements) truncates first, so a boundary row's two CSVs can differ in count.
    * The analyzer only attributes scores when the two lengths match exactly
    * (else it skips the row), so a mismatch loses that row's analytics but never
    * mis-pairs a score to the wrong id. */
   char injected[CITE_CSV_MAX];
   char inj_scores[CITE_CSV_MAX];
   size_t inj_len = 0;
   size_t inj_scores_len = 0;
   injected[0] = '\0';
   inj_scores[0] = '\0';
   for (int i = 0; i < stash.count; i++) {
      csv_append(injected, sizeof(injected), &inj_len, stash.entries[i].item_id);
      char score_str[16];
      snprintf(score_str, sizeof(score_str), "%.4f", stash.entries[i].final_score);
      csv_append(inj_scores, sizeof(inj_scores), &inj_scores_len, score_str);
   }

   /* Parse EVERY <cited>…</cited> block, validating ordinals against the stash.
    * A well-behaved model emits one trailing tag, but iterate all so a stray
    * extra tag can't silently drop citations; `seen[]` dedups across blocks. */
   char cited[CITE_CSV_MAX];
   size_t cit_len = 0;
   cited[0] = '\0';
   int cited_count = 0;
   int dropped = 0;
   bool seen[MAX_CITATION_STASH + 1] = { false }; /* dedup by ordinal */

   const char *scan = response_text;
   const char *open;
   while ((open = strstr(scan, "<cited>")) != NULL) {
      const char *inner = open + strlen("<cited>");
      const char *close = strstr(inner, "</cited>");
      const char *end = (close != NULL) ? close : (inner + strlen(inner)); /* orphan-tolerant */
      const char *p = inner;
      while (p < end) {
         while (p < end && !isdigit((unsigned char)*p)) {
            p++; /* tolerate the 'M'/'m' prefix, commas, spaces */
         }
         if (p >= end) {
            break;
         }
         int ord = 0;
         while (p < end && isdigit((unsigned char)*p)) {
            ord = ord * 10 + (*p - '0');
            p++;
            if (ord > 100000) {
               break; /* overflow guard for a garbage run of digits */
            }
         }
         if (ord >= 1 && ord <= stash.count && !seen[ord]) {
            seen[ord] = true;
            csv_append(cited, sizeof(cited), &cit_len, stash.entries[ord - 1].item_id);
            cited_count++;
         } else {
            dropped++; /* out-of-range (hallucinated/stale) or duplicate ordinal */
         }
      }
      if (close == NULL) {
         break; /* orphan opener — no further well-formed tags */
      }
      scan = close + strlen("</cited>");
   }

   int64_t conv_id = atomic_load(&session->stream_conversation_id);
   int64_t msg_id = session_get_last_user_msg_id(session); /* the user turn this reply answers */
   pthread_mutex_lock(&session->metrics_mutex);
   int user_id = session->metrics.user_id;
   pthread_mutex_unlock(&session->metrics_mutex);

   audit_insert(conv_id, msg_id, user_id, injected, cited, inj_scores, dropped);

   /* Feed the Context panel: gold-highlight the rows the model actually cited.
    * turn_id == msg_id (both last_user_msg_id) aligns this to the context_injection
    * frame; per-row match is by item_id.  Only when something was cited. */
   if (cited_count > 0) {
      webui_broadcast_context_citations(user_id, conv_id, msg_id, cited);
   }

   OLOG_INFO("memory_citation: turn audited (user=%d conv=%lld injected=%d cited=%d dropped=%d)",
             user_id, (long long)conv_id, stash.count, cited_count, dropped);
}
