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
#include "core/session_manager.h"  /* session_t, citation_stash_t, tool_cited_set_t */
#include "logging.h"
#include "memory/memory_citation_internal.h" /* memory_citation_csv_append + resolve_cited */
#include "memory/memory_db.h"                /* memory_db_fact_reinforce_citation (Phase 2) */

/* CSV buffer for the injected / tool-surfaced / cited id lists.  Sized for the
 * realistic worst case: the tool universe can hold MAX_TOOL_CITED_FACTS (96)
 * "fact:<id>," entries (~25 B each at 19-digit ids => ~2.4 KB), wider than the
 * 64-slot focus universe.  Beyond this the CSV truncates bounded-safe (the
 * analyzer counts what's present); an absurd all-items citation is the only way
 * to reach it. */
#define CITE_CSV_MAX 2560

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

/* Insert one audit row.  Best-effort telemetry: logs and returns on any failure. */
static void audit_insert(int64_t conv_id,
                         int64_t msg_id,
                         int user_id,
                         const char *injected,
                         const char *cited,
                         const char *injected_scores,
                         const char *tool_surfaced,
                         int dropped,
                         int dropped_tool) {
   AUTH_DB_LOCK_OR_RETURN_VOID();
   sqlite3_stmt *stmt = NULL;
   const char *sql =
       "INSERT INTO memory_citation_audit "
       "(conversation_id, message_id, user_id, ts, injected_ids, cited_ids, injected_scores, "
       "tool_surfaced_ids, dropped_count, dropped_tool_count) "
       "VALUES (?, ?, ?, strftime('%s','now'), ?, ?, ?, ?, ?, ?)";
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
   sqlite3_bind_text(stmt, 7, tool_surfaced ? tool_surfaced : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 8, dropped);
   sqlite3_bind_int(stmt, 9, dropped_tool);
   if (sqlite3_step(stmt) != SQLITE_DONE) {
      OLOG_WARNING("memory_citation: audit insert failed: %s", sqlite3_errmsg(s_db.db));
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
}

/* Record a tool-surfaced fact into the per-turn set (Option B).  See header. */
void memory_citation_record_tool_fact(session_t *session, int64_t fact_id) {
   if (session == NULL || fact_id <= 0 || !g_config.memory.citation_enabled) {
      return;
   }
   pthread_mutex_lock(&session->history_mutex);
   tool_cited_set_t *set = &session->tool_cited_set;
   for (int i = 0; i < set->count; i++) {
      if (set->entries[i].fact_id == fact_id) {
         pthread_mutex_unlock(&session->history_mutex); /* already recorded this turn */
         return;
      }
   }
   if (set->count >= MAX_TOOL_CITED_FACTS) {
      pthread_mutex_unlock(&session->history_mutex);
      OLOG_WARNING("memory_citation: tool-cited set full (%d) — fact %lld not citeable this turn",
                   MAX_TOOL_CITED_FACTS, (long long)fact_id);
      return;
   }
   set->entries[set->count].fact_id = fact_id;
   set->entries[set->count].kind = MEM_CITED_KIND_FACT;
   set->count++;
   pthread_mutex_unlock(&session->history_mutex);
}

void memory_citation_record_tool_fact_current(int64_t fact_id) {
   memory_citation_record_tool_fact(session_get_command_context(), fact_id);
}

bool memory_citation_enabled(void) {
   return g_config.memory.citation_enabled;
}


void memory_citation_capture(session_t *session, const char *response_text) {
   if (session == NULL || response_text == NULL || !g_config.memory.citation_enabled) {
      return;
   }

   /* Snapshot BOTH per-turn citation structures under ONE history_mutex critical
    * section (they share the lock) so validation sees a consistent pair.  The tool
    * set is written by parallel tool workers, so this copy-out MUST be under lock.
    * INVARIANT: every finalizer call site is preceded, on the SAME session, by
    * session_dispatch_user_turn()'s clear of both — else a stale set attributes to
    * the wrong turn. */
   citation_stash_t stash;
   tool_cited_set_t tool_set;
   pthread_mutex_lock(&session->history_mutex);
   stash = session->citation_stash;
   tool_set = session->tool_cited_set;
   pthread_mutex_unlock(&session->history_mutex);

   /* Proceed when EITHER channel surfaced something.  A tool-only turn (focus
    * disabled / short-circuited but memory searched) is exactly what Option B
    * exists to measure, so an empty focus stash is no longer a bail-out. */
   if (stash.count <= 0 && tool_set.count <= 0) {
      return;
   }
   if (stash.count > MAX_CITATION_STASH) {
      stash.count = MAX_CITATION_STASH; /* defensive */
   }
   if (tool_set.count > MAX_TOOL_CITED_FACTS) {
      tool_set.count = MAX_TOOL_CITED_FACTS; /* defensive */
   }

   /* Focus-injected item_ids -> injected CSV + a parallel per-item final_score CSV
    * (same order) — unchanged from Phase 1 (focus injection precision).  Both share
    * the CITE_CSV_MAX bound; the analyzer attributes scores only when the two
    * lengths match, so a truncation loses analytics but never mis-pairs. */
   char injected[CITE_CSV_MAX];
   char inj_scores[CITE_CSV_MAX];
   size_t inj_len = 0;
   size_t inj_scores_len = 0;
   injected[0] = '\0';
   inj_scores[0] = '\0';
   for (int i = 0; i < stash.count; i++) {
      memory_citation_csv_append(injected, sizeof(injected), &inj_len, stash.entries[i].item_id);
      char score_str[16];
      snprintf(score_str, sizeof(score_str), "%.4f", stash.entries[i].final_score);
      memory_citation_csv_append(inj_scores, sizeof(inj_scores), &inj_scores_len, score_str);
   }

   /* Tool-surfaced facts -> the tool universe CSV, canonical "fact:<id>" (the
    * analyzer derives tool-cite precision from cited ∩ this; summaries would slot
    * in as "summary:x" with no schema change). */
   char tool_surfaced[CITE_CSV_MAX];
   size_t tool_len = 0;
   tool_surfaced[0] = '\0';
   for (int i = 0; i < tool_set.count; i++) {
      char canon[32];
      snprintf(canon, sizeof(canon), "fact:%lld", (long long)tool_set.entries[i].fact_id);
      memory_citation_csv_append(tool_surfaced, sizeof(tool_surfaced), &tool_len, canon);
   }

   /* Parse EVERY <cited>…</cited> block and validate against BOTH sets (extracted
    * to memory_citation_resolve_cited so the tokenizer is unit-testable).
    * cited_all (focus + tool) feeds the audit; cited_focus (focus only) feeds the
    * Aurora broadcast — a tool fact has no Context-panel row, so it must NOT ride
    * the context_citations frame (keeps B1 wire-clean). */
   char cited_all[CITE_CSV_MAX];
   char cited_focus[CITE_CSV_MAX];
   int cited_focus_count = 0;
   int cited_tool_count = 0;
   int dropped = 0;
   int dropped_tool = 0;
   memory_citation_resolve_cited(response_text, &stash, &tool_set, cited_all, sizeof(cited_all),
                                 cited_focus, sizeof(cited_focus), &cited_focus_count,
                                 &cited_tool_count, &dropped, &dropped_tool);

   int64_t conv_id = atomic_load(&session->stream_conversation_id);
   int64_t msg_id = session_get_last_user_msg_id(session); /* the user turn this reply answers */
   pthread_mutex_lock(&session->metrics_mutex);
   int user_id = session->metrics.user_id;
   pthread_mutex_unlock(&session->metrics_mutex);

   audit_insert(conv_id, msg_id, user_id, injected, cited_all, inj_scores, tool_surfaced, dropped,
                dropped_tool);

   /* Phase 2 — citation-driven confidence reinforcement.  Bump every FACT the model
    * actually cited (facts only in v1; cited summaries/relations are audited above but
    * NOT reinforced — a Phase-3 decision).  Reinforce from BOTH surfaces: cited_all
    * already unions focus + tool ids, so a fact cited via either path is covered.
    * Gated on a non-zero boost so the whole block is inert at the default (0.0) until
    * deliberately enabled AFTER the legacy device-state cleanup — enabling it while
    * stale facts are live would entrench them via the confidence->rank loop.  The DB
    * primitive self-limits via a 1 h cooldown on last_cited and ceilings at 1.0. */
   if (g_config.memory.citation_reinforcement_boost > 0.0f) {
      int64_t fact_ids[MAX_CITATION_STASH + MAX_TOOL_CITED_FACTS];
      int nf = memory_citation_extract_fact_ids(cited_all, fact_ids,
                                                (int)(sizeof(fact_ids) / sizeof(fact_ids[0])));
      for (int i = 0; i < nf; i++) {
         memory_db_fact_reinforce_citation(fact_ids[i], user_id);
      }
      if (nf > 0) {
         OLOG_INFO("memory_citation: citation reinforcement applied to %d cited fact(s) "
                   "(user=%d boost=%.3f, per-fact 1h cooldown)",
                   nf, user_id, (double)g_config.memory.citation_reinforcement_boost);
      }
   }

   /* Feed the Context panel: gold-highlight ONLY the focus-injected rows the model
    * cited (a tool fact has no panel row).  Focus subset only — keeps the reviewed
    * context_citations wire contract unchanged for B1. */
   if (cited_focus_count > 0) {
      webui_broadcast_context_citations(user_id, conv_id, msg_id, cited_focus);
   }

   OLOG_INFO("memory_citation: turn audited (user=%d conv=%lld injected=%d tool_surfaced=%d "
             "cited_focus=%d cited_tool=%d dropped=%d dropped_tool=%d)",
             user_id, (long long)conv_id, stash.count, tool_set.count, cited_focus_count,
             cited_tool_count, dropped, dropped_tool);
}
