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
 * Memory admin operations — destructive resets driven by dawn-admin.
 *
 * Source of truth (NEVER touched by these helpers): conversations + messages
 * + users (except the two reset flags below) + contacts + the user-set
 * preferences table.  Five derived tables ARE dropped: memory_facts,
 * memory_preferences (the auto-extracted ones, NOT user_settings),
 * memory_summaries, memory_entities, memory_relations.
 */

#ifndef MEMORY_DB_ADMIN_H
#define MEMORY_DB_ADMIN_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Row counts deleted/reset by memory_db_admin_reset_derived().
 */
typedef struct {
   int facts_deleted;
   int preferences_deleted;
   int summaries_deleted;
   int entities_deleted;
   int relations_deleted;
   int conversations_reset;
   int aliases_deleted;         /**< v43: memory_entity_aliases rows dropped */
   int merge_proposals_deleted; /**< v43: memory_entity_merge_proposals rows dropped */
} memory_db_admin_reset_counts_t;

/**
 * @brief Cost estimate for re-extracting all stuck conversations for a user.
 *
 * `rates_known` is false when the configured extraction model is local /
 * unrecognized — cost fields are zero and the caller should report the
 * estimate as "uncosted (local provider)" or similar.
 */
typedef struct {
   int conv_count;
   int message_count;
   double cost_low_usd;
   double cost_high_usd;
   double per_conv_cost_low_usd;
   double per_conv_cost_high_usd;
   char provider[16];
   char model[64];
   bool rates_known;
   /* v43: derived alias state that reset_derived will drop along with the
    * other memory tables.  Surfaced in the dry-run report so the operator
    * sees the alias graph they're losing before --confirm.  Populated by
    * memory_db_admin_estimate_reextract_cost. */
   int active_aliases;
   int pending_proposals;
} memory_db_admin_cost_estimate_t;

/**
 * @brief Status snapshot for the most-recent reextract operation.
 *
 * `cost_tracking_available` is false in v1 — recovery worker does not
 * thread through auth_db_metrics with a session_id label.  Carried debt
 * to surface in CLI output rather than fabricate a number.
 */
typedef struct {
   int total_conversations;
   int conversations_done;
   int conversations_pending;
   int conversations_failed;
   bool worker_running;
   bool cost_tracking_available;
   double cost_spent_usd;
   double per_conv_avg_seconds;
   int eta_seconds;
} memory_db_admin_status_t;

/**
 * @brief Drop the five derived memory tables and reset extraction
 *        bookkeeping for a single user, in one BEGIN IMMEDIATE
 *        transaction.
 *
 * Conversations and messages are NOT touched.  Resets:
 *   - DELETE FROM memory_facts WHERE user_id = ?
 *   - DELETE FROM memory_preferences WHERE user_id = ?
 *   - DELETE FROM memory_entities WHERE user_id = ?
 *   - DELETE FROM memory_relations WHERE user_id = ?
 *   - DELETE FROM memory_summaries WHERE user_id = ? (skipped if keep_summaries)
 *   - UPDATE conversations SET last_extracted_msg_count = 0,
 *            last_extracted_msg_id = 0, extraction_attempts = 0,
 *            extraction_last_attempt_at = 0 WHERE user_id = ?
 *   - UPDATE users SET embeddings_model_id = NULL WHERE id = ?
 *
 * Embeddings live on the memory_facts.embedding / memory_entities.embedding
 * columns and die with the row.  document_chunks belong to a different
 * subsystem and are intentionally left alone.
 *
 * After a successful transaction, the in-memory embedding caches are
 * invalidated (acquired AFTER auth_db lock release, leaf-lock order).
 *
 * @param user_id        Target user (> 0).
 * @param keep_summaries True to skip the summaries DELETE.
 * @param counts         Optional output; zeroed on failure.
 * @return SUCCESS or FAILURE.
 */
int memory_db_admin_reset_derived(int user_id,
                                  bool keep_summaries,
                                  memory_db_admin_reset_counts_t *counts);

/**
 * @brief Estimate the cost of re-extracting all currently-stuck
 *        conversations for a user.
 *
 * Stuck conversations are those where last_extracted_msg_count <
 * message_count.  After a reset_derived call this is "all of them"; a
 * later --dry-run can be called before reset and reports the same
 * worst-case number.  Token counts use the real extraction prompt
 * template size (memory_extraction_get_template_size_chars) plus a
 * per-message overhead estimate.
 *
 * @param user_id User (> 0).
 * @param out     Required output struct.
 * @return SUCCESS or FAILURE.
 */
int memory_db_admin_estimate_reextract_cost(int user_id, memory_db_admin_cost_estimate_t *out);

/**
 * @brief Return true if a reextract worker is currently running.
 *
 * Used by the admin handler as a concurrency guard — refuses to start a
 * second reextract while one is in flight.
 */
bool memory_db_admin_reextract_is_running(void);

/**
 * @brief Spawn a detached thread that runs memory_recovery_run_pass()
 *        once to drain the post-reset stuck-conversations backlog.
 *
 * CAS-guarded busy flag, joinable thread, releases flag on exit.  Caller must have already done the
 * concurrency-guard check, the backup, and the reset transaction.
 *
 * @return SUCCESS if thread spawned, FAILURE otherwise.
 */
int memory_db_admin_start_reextract_worker(void);

/**
 * @brief Signal the reextract worker thread to stop and wait briefly.
 *
 * Called during daemon shutdown.  No-op if no worker is running.
 */
void memory_db_admin_stop_reextract_worker(void);

/**
 * @brief Snapshot reextract progress for the given user.
 *
 * Reads conversation counters under the auth_db lock; produces a
 * point-in-time view.  cost_spent_usd is 0 with cost_tracking_available
 * = false in v1 — the recovery worker doesn't currently route metrics
 * through a labelable session_id (carried debt).
 *
 * @return SUCCESS or FAILURE.
 */
int memory_db_admin_query_status(int user_id, memory_db_admin_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_DB_ADMIN_H */
