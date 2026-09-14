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
 * Memory admin operations — reset_derived + cost estimate + reextract
 * worker spawn.  Sits as a small leaf module in src/memory/ so it can
 * pull in auth_db_internal.h for the s_db handle without polluting
 * memory_db.c (which is already over the soft-line cap).
 */

#define _GNU_SOURCE /* pthread_timedjoin_np */
#define AUTH_DB_INTERNAL_ALLOWED

#include "memory/memory_db_admin.h"

#include <errno.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_extraction.h"
#include "memory/memory_recovery.h"
#include "utils/string_utils.h"

/* =============================================================================
 * Worker state — CAS-guarded busy flag, mirrors memory_recategorize.c.
 *
 * Shutdown coordination: the admin worker calls memory_recovery_run_pass(),
 * whose inner loops poll the recovery module's `s_running` flag once per
 * second.  memory_recovery_stop() sets that flag to false during daemon
 * shutdown — both the standing recovery thread and our admin worker
 * observe it and exit cleanly.  Therefore dawn.c must call
 * memory_recovery_stop BEFORE memory_db_admin_stop_reextract_worker so the
 * admin worker's pass observes the flag flip and the join below succeeds
 * without resorting to pthread_cancel (which can corrupt curl/SSL/json-c
 * state mid-LLM-call).
 * ============================================================================= */

static atomic_bool s_reextract_running = false;
static pthread_t s_reextract_thread;

/* Worker-thread join timeout — generous enough for the recovery pass to
 * observe s_running=false (1s polling interval inside run_pass) plus the
 * latency of the per-conversation wait completing.  No pthread_cancel
 * fallback: dawn.c orders memory_recovery_stop before this stop. */
#define REEXTRACT_JOIN_TIMEOUT_SEC 30

/* =============================================================================
 * Cost rates — small embedded table for known providers.  No dawn.toml
 * config knob today; Phase 2 may add `[memory.cost_rates]` if we grow
 * more providers.  Rates are in USD per 1M tokens.  Keep sorted by
 * model-prefix specificity (longer match wins).
 * ============================================================================= */

typedef struct {
   const char *provider;
   const char *model_prefix;
   double in_usd_per_million;
   double out_usd_per_million;
} cost_entry_t;

static const cost_entry_t COST_TABLE[] = {
   /* Anthropic — March 2026 published rates */
   { "claude", "claude-haiku-4-5", 1.00, 5.00 },
   { "claude", "claude-3-5-haiku", 0.80, 4.00 },
   { "claude", "claude-sonnet", 3.00, 15.00 },
   { "claude", "claude-opus", 15.00, 75.00 },
   { "claude", "claude", 1.00, 5.00 }, /* generic Claude fallback */
   /* OpenAI */
   { "openai", "gpt-4o-mini", 0.15, 0.60 },
   { "openai", "gpt-4o", 2.50, 10.00 },
   { "openai", "gpt-5", 2.50, 10.00 },
   { "openai", "gpt-4", 10.00, 30.00 },
   /* Local / Ollama / unknown — zero cost, rates_known stays false */
};

static bool lookup_rates(const char *provider,
                         const char *model,
                         double *in_rate,
                         double *out_rate) {
   if (!provider || !model)
      return false;
   /* Skip lookup entirely for local providers — recovery worker can use
    * any local model, and we don't want to fabricate a cost number. */
   if (strcmp(provider, "local") == 0 || strcmp(provider, "ollama") == 0)
      return false;
   for (size_t i = 0; i < sizeof(COST_TABLE) / sizeof(COST_TABLE[0]); i++) {
      if (strcmp(provider, COST_TABLE[i].provider) != 0)
         continue;
      if (strncmp(model, COST_TABLE[i].model_prefix, strlen(COST_TABLE[i].model_prefix)) == 0) {
         *in_rate = COST_TABLE[i].in_usd_per_million;
         *out_rate = COST_TABLE[i].out_usd_per_million;
         return true;
      }
   }
   return false;
}

/* =============================================================================
 * reset_derived — single transaction.
 * ============================================================================= */

static int run_simple_delete(const char *sql, int user_id, int *out_changes) {
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db_admin: prepare failed (%s): %s", sql, sqlite3_errmsg(s_db.db));
      return FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   rc = sqlite3_step(stmt);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db_admin: step failed (%s): %s", sql, sqlite3_errmsg(s_db.db));
      return FAILURE;
   }
   if (out_changes)
      *out_changes = changes;
   return SUCCESS;
}

int memory_db_admin_reset_derived(int user_id,
                                  bool keep_summaries,
                                  memory_db_admin_reset_counts_t *counts) {
   if (counts)
      memset(counts, 0, sizeof(*counts));

   if (user_id <= 0)
      return FAILURE;

   AUTH_DB_LOCK_OR_RETURN(FAILURE);

   /* The whole reset (7 DELETEs + 2 UPDATEs after the v43 entity-merge
    * fold-in: facts, preferences, entities, relations, summaries plus
    * memory_entity_aliases + memory_entity_merge_proposals; UPDATEs zero
    * conversation extraction cursors and per-user backfill flags) runs
    * under the auth_db leaf lock.  For a power user with thousands of
    * facts/entities this can plausibly hold the lock 100-500ms on Jetson
    * eMMC with a cold page cache while every other auth_db consumer
    * (sessions, scheduler, calendar sync) blocks.  Acceptable for a
    * one-shot admin op invoked manually.
    *
    * Phase 2 chunking note: at the dev's 2k-entity scale all seven
    * DELETEs finish well under the 500ms target.  When chunking becomes
    * necessary (~10k facts or ~5k entities), the first table to split is
    * memory_entity_aliases — its row count grows quadratically with
    * mergeable entity pairs and Phase 2's auto-merge gate at extraction
    * time will be the dominant producer once it ships.  Chunk by
    * `LIMIT N + relock between batches` per the existing reset_derived
    * pattern. */
   char *errmsg = NULL;
   int rc = sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db_admin: BEGIN IMMEDIATE failed: %s", errmsg ? errmsg : "?");
      sqlite3_free(errmsg);
      AUTH_DB_UNLOCK();
      return FAILURE;
   }

   /* FK-safe order: aliases + proposals -> relations -> entities -> facts ->
    * preferences -> summaries.  Aliases / proposals are derived state from
    * the v43 entity-merge workstream and must be dropped along with the
    * underlying entities they reference (proposals FK→entities CASCADE,
    * aliases FK→entities SET NULL — clearing the rows explicitly first
    * gives accurate counts and keeps the audit trail tied to its targets). */
   int n_rel = 0, n_ent = 0, n_fact = 0, n_pref = 0, n_sum = 0;
   int n_alias = 0, n_prop = 0;

   if (run_simple_delete("DELETE FROM memory_entity_aliases WHERE user_id = ?", user_id,
                         &n_alias) != SUCCESS)
      goto rollback;
   if (run_simple_delete("DELETE FROM memory_entity_merge_proposals WHERE user_id = ?", user_id,
                         &n_prop) != SUCCESS)
      goto rollback;
   if (run_simple_delete("DELETE FROM memory_relations WHERE user_id = ?", user_id, &n_rel) !=
       SUCCESS)
      goto rollback;
   if (run_simple_delete("DELETE FROM memory_entities WHERE user_id = ?", user_id, &n_ent) !=
       SUCCESS)
      goto rollback;
   if (run_simple_delete("DELETE FROM memory_facts WHERE user_id = ?", user_id, &n_fact) != SUCCESS)
      goto rollback;
   if (run_simple_delete("DELETE FROM memory_preferences WHERE user_id = ?", user_id, &n_pref) !=
       SUCCESS)
      goto rollback;
   if (!keep_summaries) {
      if (run_simple_delete("DELETE FROM memory_summaries WHERE user_id = ?", user_id, &n_sum) !=
          SUCCESS)
         goto rollback;
   }

   /* Reset conversations extraction high-water marks.  Keep last_extracted_msg_id
    * in sync with last_extracted_msg_count = 0 so the ID-based filter (v40+)
    * sees the conversation as "never extracted." */
   sqlite3_stmt *up_conv = NULL;
   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE conversations SET last_extracted_msg_count = 0, "
                           "last_extracted_msg_id = 0, extraction_attempts = 0, "
                           "extraction_last_attempt_at = 0 WHERE user_id = ?",
                           -1, &up_conv, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db_admin: prepare conv UPDATE failed: %s", sqlite3_errmsg(s_db.db));
      goto rollback;
   }
   sqlite3_bind_int(up_conv, 1, user_id);
   rc = sqlite3_step(up_conv);
   int n_conv = sqlite3_changes(s_db.db);
   sqlite3_finalize(up_conv);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db_admin: conv UPDATE step failed: %s", sqlite3_errmsg(s_db.db));
      goto rollback;
   }

   /* Reset users.categories_backfilled_at and users.embeddings_model_id so the
    * lazy fact-category backfill and the embedding-recompute worker re-run
    * after re-extraction populates fresh data. */
   sqlite3_stmt *up_user = NULL;
   rc = sqlite3_prepare_v2(s_db.db,
                           "UPDATE users SET categories_backfilled_at = 0, "
                           "embeddings_model_id = NULL WHERE id = ?",
                           -1, &up_user, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db_admin: prepare user UPDATE failed: %s", sqlite3_errmsg(s_db.db));
      goto rollback;
   }
   sqlite3_bind_int(up_user, 1, user_id);
   rc = sqlite3_step(up_user);
   sqlite3_finalize(up_user);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db_admin: user UPDATE step failed: %s", sqlite3_errmsg(s_db.db));
      goto rollback;
   }

   rc = sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, &errmsg);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db_admin: COMMIT failed: %s", errmsg ? errmsg : "?");
      sqlite3_free(errmsg);
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return FAILURE;
   }

   AUTH_DB_UNLOCK();

   /* Drop in-memory embedding caches AFTER releasing the auth_db lock.
    * embedding_cache mutex is a leaf lock per ARCHITECTURE.md. */
   memory_embeddings_invalidate_all();

   if (counts) {
      counts->relations_deleted = n_rel;
      counts->entities_deleted = n_ent;
      counts->facts_deleted = n_fact;
      counts->preferences_deleted = n_pref;
      counts->summaries_deleted = n_sum;
      counts->conversations_reset = n_conv;
      counts->aliases_deleted = n_alias;
      counts->merge_proposals_deleted = n_prop;
   }

   OLOG_INFO("memory_db_admin: reset user=%d facts=%d prefs=%d sums=%d ents=%d rels=%d convs=%d "
             "aliases=%d proposals=%d (keep_summaries=%d)",
             user_id, n_fact, n_pref, n_sum, n_ent, n_rel, n_conv, n_alias, n_prop,
             keep_summaries ? 1 : 0);
   return SUCCESS;

rollback:
   sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
   AUTH_DB_UNLOCK();
   return FAILURE;
}

/* =============================================================================
 * Cost estimate — uses real prompt template size, NOT a hardcoded guess.
 * ============================================================================= */

/* Per-message overhead estimate.  A typical conversation message in the
 * extraction prompt's JSON form is ~120-280 chars (role + content + JSON
 * delimiters).  We bracket the estimate with a low/high range so the CLI
 * can show realistic min/max numbers. */
#define PER_MSG_CHARS_LOW 120
#define PER_MSG_CHARS_HIGH 280
/* Output token estimate per extraction (the LLM emits a JSON envelope of
 * facts/preferences/entities/relations/summary; varies by conversation
 * richness).  Bracketed similarly. */
#define OUT_TOKENS_LOW 200
#define OUT_TOKENS_HIGH 800
/* Conservative chars-per-token ratio (English text + JSON; LLM tokenizers
 * average ~3.5-4.5 chars/token).  Use 4 for the estimate. */
#define CHARS_PER_TOKEN 4

int memory_db_admin_estimate_reextract_cost(int user_id, memory_db_admin_cost_estimate_t *out) {
   if (!out || user_id <= 0)
      return FAILURE;
   memset(out, 0, sizeof(*out));

   const char *provider = g_config.memory.extraction_provider;
   const char *model = g_config.memory.extraction_model;
   if (provider) {
      safe_strscpy(out->provider, provider);
   }
   if (model) {
      safe_strscpy(out->model, model);
   }

   AUTH_DB_LOCK_OR_RETURN(FAILURE);

   /* Size the POST-RESET work, not the current eligible-for-recovery state.
    * After --confirm runs reset_derived, every non-private conversation
    * with message_count >= 2 will have its high-water mark zeroed and
    * become eligible.  Pre-fix this query also gated on
    * last_extracted_msg_count < message_count, which underestimated by
    * excluding the (typically large) set of already-extracted
    * conversations.  Post-fix mirrors the post-reset eligibility
    * predicate exactly: every non-private user conv with at least 2
    * messages will be re-run.  See coordinator hot-patch. */
   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db,
                               "SELECT COUNT(*), COALESCE(SUM(message_count), 0) "
                               "FROM conversations "
                               "WHERE user_id = ? AND message_count >= 2 "
                               "  AND COALESCE(is_private, 0) = 0",
                               -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db_admin: estimate prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      out->conv_count = sqlite3_column_int(stmt, 0);
      out->message_count = sqlite3_column_int(stmt, 1);
   }
   sqlite3_finalize(stmt);

   /* v43: count active aliases + pending merge proposals for the dry-run
    * report.  Both tables are dropped by reset_derived; surfacing them
    * here lets the operator see what alias graph state they're losing
    * before --confirm runs.  Same auth_db lock as the conv-count query
    * — both partial indexes (idx_memory_entity_aliases_user_target,
    * idx_merge_proposals_pending) make these COUNTs cheap. */
   sqlite3_stmt *cnt = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT COUNT(*) FROM memory_entity_aliases "
                          "WHERE user_id = ? AND unlinked_at IS NULL",
                          -1, &cnt, NULL) == SQLITE_OK) {
      sqlite3_bind_int(cnt, 1, user_id);
      if (sqlite3_step(cnt) == SQLITE_ROW)
         out->active_aliases = sqlite3_column_int(cnt, 0);
      sqlite3_finalize(cnt);
   }
   cnt = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT COUNT(*) FROM memory_entity_merge_proposals "
                          "WHERE user_id = ? AND resolved_at IS NULL",
                          -1, &cnt, NULL) == SQLITE_OK) {
      sqlite3_bind_int(cnt, 1, user_id);
      if (sqlite3_step(cnt) == SQLITE_ROW)
         out->pending_proposals = sqlite3_column_int(cnt, 0);
      sqlite3_finalize(cnt);
   }

   AUTH_DB_UNLOCK();

   if (out->conv_count <= 0)
      return SUCCESS; /* nothing to extract — zero cost is correct */

   /* Per-conv input chars = template + per_msg × msgs (existing_profile
    * starts ~empty after reset).  Per-conv output is OUT_TOKENS_*. */
   size_t template_chars = memory_extraction_get_template_size_chars();
   double avg_msgs_per_conv = (double)out->message_count / (double)out->conv_count;

   double in_chars_low = (double)template_chars + PER_MSG_CHARS_LOW * avg_msgs_per_conv;
   double in_chars_high = (double)template_chars + PER_MSG_CHARS_HIGH * avg_msgs_per_conv;
   double in_tokens_low = in_chars_low / CHARS_PER_TOKEN;
   double in_tokens_high = in_chars_high / CHARS_PER_TOKEN;

   double in_rate = 0.0, out_rate = 0.0;
   out->rates_known = lookup_rates(out->provider, out->model, &in_rate, &out_rate);
   if (!out->rates_known)
      return SUCCESS; /* zero-cost when rates aren't known */

   double per_conv_low = (in_tokens_low * in_rate + OUT_TOKENS_LOW * out_rate) / 1e6;
   double per_conv_high = (in_tokens_high * in_rate + OUT_TOKENS_HIGH * out_rate) / 1e6;
   out->per_conv_cost_low_usd = per_conv_low;
   out->per_conv_cost_high_usd = per_conv_high;
   out->cost_low_usd = per_conv_low * out->conv_count;
   out->cost_high_usd = per_conv_high * out->conv_count;
   return SUCCESS;
}

/* =============================================================================
 * Status query.
 * ============================================================================= */

int memory_db_admin_query_status(int user_id, memory_db_admin_status_t *out) {
   if (!out || user_id <= 0)
      return FAILURE;
   memset(out, 0, sizeof(*out));
   out->worker_running = atomic_load(&s_reextract_running);
   /* v1 carried debt — recovery-worker LLM calls don't currently land in
    * auth_db_metrics with a queryable session_id label (session_metrics has
    * a uint32_t ephemeral session_id, not a "recovery_<conv_id>" string).
    * Surface as not-tracked rather than fabricate a number. */
   out->cost_tracking_available = false;
   out->cost_spent_usd = 0.0;

   AUTH_DB_LOCK_OR_RETURN(FAILURE);

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(
       s_db.db,
       "SELECT "
       "  COUNT(*), "
       "  SUM(CASE WHEN last_extracted_msg_count >= message_count THEN 1 ELSE 0 END), "
       "  SUM(CASE WHEN extraction_attempts >= ? AND last_extracted_msg_count = 0 THEN 1 ELSE 0 "
       "END) "
       "FROM conversations WHERE user_id = ? AND message_count >= 2 "
       "  AND COALESCE(is_private, 0) = 0",
       -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_db_admin: status prepare failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return FAILURE;
   }
   int max_attempts = g_config.memory.recovery_max_attempts;
   if (max_attempts <= 0)
      max_attempts = 2;
   sqlite3_bind_int(stmt, 1, max_attempts);
   sqlite3_bind_int(stmt, 2, user_id);
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      out->total_conversations = sqlite3_column_int(stmt, 0);
      out->conversations_done = sqlite3_column_int(stmt, 1);
      out->conversations_failed = sqlite3_column_int(stmt, 2);
      int pending = out->total_conversations - out->conversations_done - out->conversations_failed;
      out->conversations_pending = pending > 0 ? pending : 0;
   }
   sqlite3_finalize(stmt);

   AUTH_DB_UNLOCK();
   return SUCCESS;
}

/* =============================================================================
 * Worker thread — drains the post-reset stuck-conversation backlog by
 * calling memory_recovery_run_pass() once.  Detached / fire-and-forget;
 * mirrors memory_recategorize_start.
 * ============================================================================= */

static void *reextract_thread_fn(void *arg) {
   (void)arg;
   OLOG_INFO("memory_db_admin: reextract worker started");
   memory_recovery_run_pass(NULL, NULL);
   OLOG_INFO("memory_db_admin: reextract worker finished");
   atomic_store(&s_reextract_running, false);
   return NULL;
}

int memory_db_admin_start_reextract_worker(void) {
   bool expected = false;
   if (!atomic_compare_exchange_strong(&s_reextract_running, &expected, true)) {
      OLOG_WARNING("memory_db_admin: reextract worker already running");
      return FAILURE;
   }

   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

   if (pthread_create(&s_reextract_thread, &attr, reextract_thread_fn, NULL) != 0) {
      OLOG_ERROR("memory_db_admin: failed to create reextract thread");
      atomic_store(&s_reextract_running, false);
      pthread_attr_destroy(&attr);
      return FAILURE;
   }

   pthread_attr_destroy(&attr);
   return SUCCESS;
}

bool memory_db_admin_reextract_is_running(void) {
   return atomic_load(&s_reextract_running);
}

void memory_db_admin_stop_reextract_worker(void) {
   if (!atomic_load(&s_reextract_running))
      return;
   /* Cooperative shutdown: dawn.c calls memory_recovery_stop() before this,
    * which flips the recovery module's s_running flag.  run_pass observes
    * the flag in its inner loops (1s polling) and exits cleanly, so we
    * just need to join the thread.  Generous timeout covers the case
    * where the worker is mid-extraction-wait when the flag flips. */
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_sec += REEXTRACT_JOIN_TIMEOUT_SEC;

   int ret = pthread_timedjoin_np(s_reextract_thread, NULL, &ts);
   if (ret == ETIMEDOUT) {
      /* Last-resort detach so we don't block daemon exit indefinitely.  We
       * deliberately do NOT pthread_cancel — interrupting curl/SSL/json-c
       * mid-extraction can corrupt heap state.  The detached thread will
       * complete on its own once the in-flight LLM call returns; the
       * leaked join handle is acceptable since the daemon is exiting. */
      OLOG_WARNING("memory_db_admin: reextract thread did not stop in %ds; detaching",
                   REEXTRACT_JOIN_TIMEOUT_SEC);
      pthread_detach(s_reextract_thread);
   }
   atomic_store(&s_reextract_running, false);
}
