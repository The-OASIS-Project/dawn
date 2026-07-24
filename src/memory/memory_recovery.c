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
 * Memory Extraction Recovery Implementation
 */

#define AUTH_DB_INTERNAL_ALLOWED /* direct sqlite access for stuck-conv scan */

#include "memory/memory_recovery.h"

#include <json-c/json.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "config/dawn_config.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_extraction.h"
#include "memory/memory_history_loader.h"

/* Per-conversation timeouts.  Block on a single extraction up to this long
 * before moving on; a stuck/hanging extraction shouldn't pin the recovery
 * thread forever.  The poll interval is intentionally 1 s so shutdown
 * (s_running flip + pthread_join) is observed within one tick. */
#define RECOVERY_PER_CONV_WAIT_SEC 300 /* 5 min */
#define RECOVERY_POLL_INTERVAL_SEC 1
#define RECOVERY_TRIGGER_SETTLE_MS 200 /* slot-acquisition settle window */

/* Maximum stuck conversations to scan in one pass.  Acts as a defense
 * against pathologically large backlogs; subsequent passes pick up the
 * remainder. */
#define RECOVERY_PASS_LIMIT 500

/* Minimum total plain-text characters across the conversation history (after
 * stripping inline image data) for the LLM to have anything extractable.
 * Below this we mark the conversation as up-to-date and skip the LLM call.
 * 50 chars catches truly-empty conversations without rejecting short but
 * substantive ones (e.g. "What is this?  →  Looks like a chair to me."). */
#define RECOVERY_MIN_TEXT_CHARS 50

/* Worker thread state */
static pthread_t s_recovery_thread;
static _Atomic bool s_running = false;  // atomic: written on shutdown, read in loop
static pthread_mutex_t s_state_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Serialize concurrent run_pass invocations.  The standing recovery thread
 * (recovery_thread_func) and the admin-triggered reextract worker
 * (memory_db_admin_start_reextract_worker) both call memory_recovery_run_pass.
 * Without this mutex they would independently load the same stuck-conversation
 * rows and call memory_trigger_extraction in parallel — the per-user
 * extraction slot would gate double-extraction of any single conversation,
 * but the second caller still wastes SELECTs and increments
 * extraction_attempts on conversations the first caller is handling.
 * Acquired at run_pass entry, released at exit. */
static pthread_mutex_t s_pass_mutex = PTHREAD_MUTEX_INITIALIZER;

/* =============================================================================
 * Internal helpers
 * ============================================================================= */

typedef struct {
   int64_t conv_id;
   int user_id;
   int message_count;
   int extraction_attempts;
} stuck_conv_t;

typedef struct {
   stuck_conv_t *rows;
   int count;
   int capacity;
} stuck_list_t;

/**
 * @brief Query stuck conversations matching the recovery criteria.
 *
 * Caller frees out->rows.
 */
static int load_stuck_conversations(stuck_list_t *out, time_t idle_cutoff, int max_attempts) {
   out->rows = NULL;
   out->count = 0;
   out->capacity = 0;

   /* Criteria:
    *   - non-private
    *   - NOT a background job (job_status IS NULL): a job's conversation holds
    *     the goal + raw result, but memory should be based on the PARENT — the
    *     reinvoke surfaces the result INTO the parent conversation, whose own
    *     extraction captures it.  Extracting job convs directly floods memory
    *     with per-job research fragments duplicating the parent's summary.
    *   - has at least 2 messages (matches memory_trigger_extraction skip threshold)
    *   - extraction backlog: last_extracted_msg_count < message_count
    *   - idle: updated_at < idle_cutoff
    *   - under attempt cap, OR has fresh activity (updated_at > last attempt)
    *   - max_attempts == 0 means unlimited
    */
   const char *sql = "SELECT id, user_id, message_count, extraction_attempts "
                     "FROM conversations "
                     "WHERE is_private = 0 "
                     "  AND job_status IS NULL "
                     "  AND message_count >= 2 "
                     "  AND last_extracted_msg_count < message_count "
                     "  AND updated_at < ? "
                     "  AND (?2 = 0 OR extraction_attempts < ?2 "
                     "       OR extraction_last_attempt_at < updated_at) "
                     "ORDER BY updated_at DESC "
                     "LIMIT ?3";

   AUTH_DB_LOCK_OR_FAIL();

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("memory_recovery: prepare scan failed: %s", sqlite3_errmsg(s_db.db));
      AUTH_DB_UNLOCK();
      return FAILURE;
   }

   sqlite3_bind_int64(stmt, 1, (sqlite3_int64)idle_cutoff);
   sqlite3_bind_int(stmt, 2, max_attempts);
   sqlite3_bind_int(stmt, 3, RECOVERY_PASS_LIMIT);

   while (sqlite3_step(stmt) == SQLITE_ROW) {
      if (out->count >= out->capacity) {
         int new_cap = out->capacity == 0 ? 32 : out->capacity * 2;
         stuck_conv_t *grown = realloc(out->rows, (size_t)new_cap * sizeof(stuck_conv_t));
         if (!grown) {
            OLOG_ERROR("memory_recovery: out of memory growing stuck list");
            break;
         }
         out->rows = grown;
         out->capacity = new_cap;
      }
      out->rows[out->count].conv_id = sqlite3_column_int64(stmt, 0);
      out->rows[out->count].user_id = sqlite3_column_int(stmt, 1);
      out->rows[out->count].message_count = sqlite3_column_int(stmt, 2);
      out->rows[out->count].extraction_attempts = sqlite3_column_int(stmt, 3);
      out->count++;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return SUCCESS;
}

/**
 * @brief Increment extraction_attempts and stamp extraction_last_attempt_at.
 */
static void record_attempt(int64_t conv_id) {
   AUTH_DB_LOCK_OR_RETURN_VOID();

   sqlite3_stmt *stmt = NULL;
   const char *sql = "UPDATE conversations "
                     "SET extraction_attempts = extraction_attempts + 1, "
                     "    extraction_last_attempt_at = ? "
                     "WHERE id = ?";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, (sqlite3_int64)time(NULL));
      sqlite3_bind_int64(stmt, 2, conv_id);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
   } else {
      OLOG_WARNING("memory_recovery: record_attempt prepare failed: %s", sqlite3_errmsg(s_db.db));
   }

   AUTH_DB_UNLOCK();
}

/**
 * @brief Block until extraction completes for the given user, or timeout.
 *
 * Returns true if the slot cleared, false on timeout.
 */
static bool wait_for_user_extraction(int user_id) {
   int waited = 0;
   while (memory_extraction_in_progress(user_id) && s_running) {
      if (waited >= RECOVERY_PER_CONV_WAIT_SEC) {
         return false;
      }
      sleep(RECOVERY_POLL_INTERVAL_SEC);
      waited += RECOVERY_POLL_INTERVAL_SEC;
   }
   return true;
}

/**
 * @brief Process one stuck conversation.  Returns true if extraction was
 *        actually triggered (regardless of outcome).
 */
static bool process_stuck_conv(const stuck_conv_t *row, int max_attempts) {
   /* Read pre-trigger counter so we can detect successful advancement. */
   int last_extracted_before = 0;
   memory_db_get_last_extracted(row->conv_id, &last_extracted_before);

   /* If extraction is already running for this user (e.g. live session),
    * skip this row this pass — the live extraction will handle it. */
   if (memory_extraction_in_progress(row->user_id)) {
      OLOG_INFO("memory_recovery: user %d busy, deferring conv %lld", row->user_id,
                (long long)row->conv_id);
      return false;
   }

   size_t total_text_len = 0;
   struct json_object *history = memory_history_load_from_db(row->conv_id, row->user_id,
                                                             &total_text_len);
   if (!history) {
      OLOG_WARNING("memory_recovery: failed to load history for conv %lld",
                   (long long)row->conv_id);
      record_attempt(row->conv_id); /* count toward cap; protects against permanent failures */
      return false;
   }

   /* No-extractable-content gate: image-only or otherwise empty conversations
    * have nothing the LLM can turn into facts.  Mark up-to-date so the
    * recovery scan stops re-evaluating, and skip the LLM call. */
   if (total_text_len < RECOVERY_MIN_TEXT_CHARS) {
      OLOG_INFO("memory_recovery: conv %lld has %zu chars of text after image strip "
                "(< %d threshold); marking as extracted (no-op)",
                (long long)row->conv_id, total_text_len, RECOVERY_MIN_TEXT_CHARS);
      memory_db_set_last_extracted(row->conv_id, row->message_count, 0);
      json_object_put(history);
      return false;
   }

   /* Stamp the attempt before triggering — if the daemon crashes during
    * extraction, the next pass sees the bumped counter and won't re-loop. */
   record_attempt(row->conv_id);

   /* Recovery-tagged session_id lets extraction_thread recognise this as a
    * background re-extraction and suppress user-facing failure toasts. */
   char session_id[MEMORY_SESSION_ID_MAX];
   snprintf(session_id, sizeof(session_id), "recovery_%lld", (long long)row->conv_id);

   int rc = memory_trigger_extraction(row->user_id, row->conv_id, session_id, history,
                                      row->message_count, 0, NULL);
   json_object_put(history);

   if (rc != SUCCESS) {
      OLOG_WARNING("memory_recovery: trigger failed for conv %lld (rc=%d)", (long long)row->conv_id,
                   rc);
      return false;
   }

   /* Wait briefly to confirm extraction actually started (vs. a no-op skip). */
   usleep(RECOVERY_TRIGGER_SETTLE_MS * 1000);
   if (!memory_extraction_in_progress(row->user_id)) {
      /* No-op skip — extraction was disabled, conv too short, or capacity hit.
       * Attempts already counted; future passes will hit the cap and stop. */
      OLOG_INFO("memory_recovery: trigger no-op for conv %lld", (long long)row->conv_id);
      return false;
   }

   /* Block until done so the next conv doesn't compete for the per-user slot. */
   bool finished = wait_for_user_extraction(row->user_id);
   if (!finished) {
      OLOG_WARNING("memory_recovery: extraction for conv %lld exceeded %d s, moving on",
                   (long long)row->conv_id, RECOVERY_PER_CONV_WAIT_SEC);
      return true;
   }

   /* Inspect counter advancement for log clarity. */
   int last_extracted_after = 0;
   memory_db_get_last_extracted(row->conv_id, &last_extracted_after);
   if (last_extracted_after > last_extracted_before) {
      OLOG_INFO("memory_recovery: extracted conv %lld (%d -> %d msgs)", (long long)row->conv_id,
                last_extracted_before, last_extracted_after);
   } else {
      /* Extraction ran but didn't produce a counter advance.  Two
       * sub-cases: (a) transient network failure — roll back the attempt
       * stamp so the next pass picks the conv up again; (b) genuine
       * non-advance — apply the standard give-up-at-cap heuristic. */
      if (memory_extraction_consume_last_transient(row->user_id)) {
         OLOG_WARNING("memory_recovery: conv %lld extraction hit transient network "
                      "failure (cloud unreachable); rolling back attempt counter so the "
                      "next pass can retry",
                      (long long)row->conv_id);
         memory_db_undo_extraction_attempt(row->conv_id);
      } else {
         bool last_attempt = (max_attempts > 0 && row->extraction_attempts + 1 >= max_attempts);
         if (last_attempt) {
            OLOG_INFO("memory_recovery: conv %lld hit attempt cap (%d) without success; "
                      "marking as extracted (give-up)",
                      (long long)row->conv_id, max_attempts);
            memory_db_set_last_extracted(row->conv_id, row->message_count, 0);
         } else {
            OLOG_INFO("memory_recovery: conv %lld extraction completed without advancing "
                      "counter (attempt %d/%d)",
                      (long long)row->conv_id, row->extraction_attempts + 1, max_attempts);
         }
      }
   }
   return true;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

void memory_recovery_run_pass(int *triggered_out, int *skipped_out) {
   int triggered = 0;
   int skipped = 0;

   /* Serialize against any concurrent caller (standing recovery thread vs.
    * admin-triggered reextract worker).  The pass exits early on
    * !s_running so the lock-hold is bounded by the longest single
    * conversation's extraction (capped at RECOVERY_PER_CONV_WAIT_SEC). */
   pthread_mutex_lock(&s_pass_mutex);

   if (!g_config.memory.enabled || !g_config.memory.recovery_enabled) {
      goto out;
   }

   int idle_threshold = g_config.memory.recovery_idle_threshold_seconds;
   int max_attempts = g_config.memory.recovery_max_attempts;
   if (idle_threshold < 60) {
      idle_threshold = 60; /* defensive lower bound mirroring validator */
   }

   time_t idle_cutoff = time(NULL) - idle_threshold;

   stuck_list_t list = { 0 };
   if (load_stuck_conversations(&list, idle_cutoff, max_attempts) != SUCCESS) {
      goto out;
   }

   if (list.count == 0) {
      free(list.rows);
      list.rows = NULL;
      goto out;
   }

   OLOG_INFO("memory_recovery: pass starting — %d stuck conversation(s) to process", list.count);

   for (int i = 0; i < list.count && s_running; i++) {
      if (process_stuck_conv(&list.rows[i], max_attempts)) {
         triggered++;
      } else {
         skipped++;
      }
   }

   free(list.rows);
   list.rows = NULL;

   OLOG_INFO("memory_recovery: pass complete — %d triggered, %d skipped", triggered, skipped);

out:
   if (triggered_out) {
      *triggered_out = triggered;
   }
   if (skipped_out) {
      *skipped_out = skipped;
   }
   pthread_mutex_unlock(&s_pass_mutex);
}

static void *recovery_thread_func(void *arg) {
   (void)arg;

   /* Run nice(10) — this is background work that should yield to voice. */
   if (nice(10) == -1) {
      OLOG_WARNING("memory_recovery: failed to set nice level");
   }

   OLOG_INFO("memory_recovery: thread started");

   /* Initial startup pass — give the daemon a beat to finish booting before
    * we start hammering the LLM with extraction work. */
   for (int i = 0; i < 30 && s_running; i++) {
      sleep(1);
   }
   if (s_running) {
      memory_recovery_run_pass(NULL, NULL);
   }

   while (s_running) {
      int interval = g_config.memory.recovery_recurring_interval_seconds;
      if (interval <= 0) {
         /* Recurring disabled — wait for shutdown. */
         while (s_running) {
            sleep(1);
         }
         break;
      }

      for (int i = 0; i < interval && s_running; i++) {
         sleep(1);
      }
      if (!s_running) {
         break;
      }

      memory_recovery_run_pass(NULL, NULL);
   }

   OLOG_INFO("memory_recovery: thread stopped");
   return NULL;
}

int memory_recovery_start(void) {
   pthread_mutex_lock(&s_state_mutex);

   if (s_running) {
      pthread_mutex_unlock(&s_state_mutex);
      return SUCCESS;
   }

   if (!g_config.memory.enabled || !g_config.memory.recovery_enabled) {
      OLOG_INFO("memory_recovery: disabled in config, skipping");
      pthread_mutex_unlock(&s_state_mutex);
      return SUCCESS;
   }

   s_running = true;
   if (pthread_create(&s_recovery_thread, NULL, recovery_thread_func, NULL) != 0) {
      s_running = false;
      OLOG_ERROR("memory_recovery: failed to spawn worker thread");
      pthread_mutex_unlock(&s_state_mutex);
      return FAILURE;
   }

   pthread_mutex_unlock(&s_state_mutex);
   return SUCCESS;
}

void memory_recovery_stop(void) {
   pthread_mutex_lock(&s_state_mutex);

   if (!s_running) {
      pthread_mutex_unlock(&s_state_mutex);
      return;
   }

   s_running = false;
   pthread_mutex_unlock(&s_state_mutex);

   pthread_join(s_recovery_thread, NULL);
}
