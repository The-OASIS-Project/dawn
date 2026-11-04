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
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Memory Maintenance Implementation
 *
 * Orchestrates nightly confidence decay and pruning across all users.
 * Called from auth_maintenance thread at configurable hour.
 */

#include "memory/memory_maintenance.h"

#include <time.h>
#include <unistd.h>

#include "config/dawn_config.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_db_consolidation.h"
#include "memory/memory_db_entities.h"
#include "memory/memory_extraction.h"
#include "memory/memory_forget.h"

/* Maximum users to process in one run */
#define MAX_USERS 64

/* Minimum gap between runs (20 hours) to prevent double-execution */
#define MIN_RUN_GAP_SECONDS (20 * 3600)

/* The consolidation record keeps a batch (its ops, and a candidate set aside
 * in it) MEMORY_CONSOLIDATE_RECORD_DAYS, whatever decay is set to: it holds
 * conversation text.  A candidate set aside expires after
 * MEMORY_CONSOLIDATE_DEFERRED_DAYS. */
static void prune_consolidation_record(time_t now) {
   int batches = 0;
   if (memory_db_consolidation_prune((int64_t)now - (int64_t)MEMORY_CONSOLIDATE_RECORD_DAYS * 86400,
                                     &batches) != MEMORY_DB_SUCCESS) {
      OLOG_WARNING("memory_decay: pruning the consolidation record failed; it runs again tomorrow");
   } else if (batches > 0) {
      OLOG_INFO("memory_decay: %d consolidation batches past their record time removed", batches);
   }
   /* A candidate set aside that long: no later batch takes it up (the user
    * may still keep it until its batch is pruned). */
   int expired = 0;
   if (memory_db_consolidation_expire_deferred(
           (int64_t)now - (int64_t)MEMORY_CONSOLIDATE_DEFERRED_DAYS * 86400, &expired) !=
       MEMORY_DB_SUCCESS) {
      OLOG_WARNING("memory_decay: expiring set-aside candidates failed; it runs again tomorrow");
   } else if (expired > 0) {
      OLOG_INFO("memory_decay: %d set-aside candidates expired", expired);
   }
}

/* How old an entity nothing names must be before the sweep removes it: past
 * any batch that could still name it, and long enough that one the user just
 * saw in the Graph tab doesn't vanish under them. */
#define ORPHAN_ENTITY_MIN_AGE_DAYS 7

/* Entities nothing names (no fact, relation, alias, proposal, contact or
 * record row), left before batches removed their own: noise in focus and the
 * graph that undo and forget never reach.  Each user's extraction is held so
 * a running batch's new entity isn't taken before it is named. */
static void sweep_orphan_entities(time_t now) {
   int user_ids[MAX_USERS];
   int user_count = 0;
   memory_db_get_all_user_ids(user_ids, MAX_USERS, &user_count);
   for (int i = 0; i < user_count; i++) {
      if (!memory_extraction_hold_user(user_ids[i], MEMORY_FORGET_WAIT_SEC)) {
         continue; /* runs again tomorrow */
      }
      int deleted = 0;
      const int rc = memory_db_entity_sweep_orphans(
          user_ids[i], (int64_t)now - (int64_t)ORPHAN_ENTITY_MIN_AGE_DAYS * 86400, &deleted);
      memory_extraction_release_user(user_ids[i]);
      if (rc == MEMORY_DB_SUCCESS && deleted > 0) {
         OLOG_INFO("memory_decay: user %d: %d entities nothing names removed", user_ids[i],
                   deleted);
      }
   }
}

void memory_run_nightly_decay(void) {
   /* Check if current hour matches configured decay hour */
   time_t now = time(NULL);
   struct tm local_time;
   localtime_r(&now, &local_time);

   if (local_time.tm_hour != g_config.memory.decay_hour) {
      return;
   }

   /* Prevent double-execution within 20 hours.
    * Note: single-caller assumption — only called from auth_maintenance thread. */
   static time_t last_run = 0;
   if (last_run > 0 && (now - last_run) < MIN_RUN_GAP_SECONDS) {
      return;
   }
   prune_consolidation_record(now);
   if (g_config.memory.enabled) {
      sweep_orphan_entities(now);
   }

   /* Check if memory and decay are enabled */
   if (!g_config.memory.enabled || !g_config.memory.decay_enabled) {
      last_run = now;
      return;
   }

   /* Get all users with memory data */
   int user_ids[MAX_USERS];
   int user_count = 0;
   memory_db_get_all_user_ids(user_ids, MAX_USERS, &user_count);
   if (user_count <= 0) {
      last_run = now;
      return;
   }

   int total_decayed = 0;
   int total_pruned = 0;
   int total_summaries = 0;
   int total_expired = 0;

   for (int i = 0; i < user_count; i++) {
      int uid = user_ids[i];

      /* Apply fact decay */
      int decayed = 0;
      memory_db_apply_fact_decay(uid, g_config.memory.decay_inferred_weekly,
                                 g_config.memory.decay_explicit_weekly,
                                 g_config.memory.decay_inferred_floor,
                                 g_config.memory.decay_explicit_floor, &decayed);
      if (decayed > 0)
         total_decayed += decayed;

      /* Apply preference decay */
      memory_db_apply_pref_decay(uid, g_config.memory.decay_preference_weekly,
                                 g_config.memory.decay_preference_floor, NULL);

      /* Prune low-confidence facts */
      int pruned = 0;
      memory_db_prune_low_confidence(uid, g_config.memory.decay_prune_threshold, &pruned);
      if (pruned > 0)
         total_pruned += pruned;

      /* Prune old superseded facts */
      memory_db_fact_prune_superseded(uid, g_config.memory.prune_superseded_days, NULL);

      /* Prune expired facts past their recoverable window (v58, C3).  Gated —
       * off by default until recall-validated.  Ordered after prune_superseded
       * and before the summary prune so it doesn't shift which rows the
       * confidence/stale passes above see (those run on superseded_by/confidence,
       * orthogonal to expires_at).  Idempotent under the 20h guard. */
      if (g_config.memory.expire_enabled) {
         int expired = 0;
         memory_db_fact_prune_expired(uid, g_config.memory.prune_expired_days, &expired);
         if (expired > 0)
            total_expired += expired;
      }

      /* Prune old summaries */
      int summaries = 0;
      memory_db_prune_old_summaries(uid, g_config.memory.summary_retention_days, &summaries);
      if (summaries > 0)
         total_summaries += summaries;

      /* Courtesy yield between users */
      usleep(1000);
   }

   last_run = now;

   OLOG_INFO("memory_decay: completed — %d users, %d facts decayed, %d pruned, %d expired, "
             "%d summaries cleaned",
             user_count, total_decayed, total_pruned, total_expired, total_summaries);
}
