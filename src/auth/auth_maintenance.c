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
 * Background maintenance thread for authentication database.
 */

#include "auth/auth_maintenance.h"

#include <pthread.h>
#include <stdbool.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "dawn_error.h"
#include "document_original_store.h"
#include "image_store.h"
#include "logging.h"
#include "memory/memory_maintenance.h"
#include "tools/document_db.h"
#ifdef DAWN_ENABLE_STAT_TOOL
#include "core/stat_service.h"
#endif

/* Thread state */
static pthread_t s_maintenance_thread;
static _Atomic bool s_running = false;  // atomic: written on shutdown, read in loop
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Maintenance thread entry point.
 *
 * Runs at reduced priority to avoid impacting voice processing.
 * Performs periodic cleanup and checkpointing.
 */
static void *maintenance_thread_func(void *arg) {
   (void)arg;

   /* Lower thread priority - nice(10) for background work */
   if (nice(10) == -1) {
      OLOG_WARNING("auth_maintenance: failed to set nice level");
   }

   OLOG_INFO("auth_maintenance: thread started (interval=%ds)", AUTH_MAINTENANCE_INTERVAL_SEC);

   while (s_running) {
      /* Sleep in small increments for responsive shutdown */
      for (int i = 0; i < AUTH_MAINTENANCE_INTERVAL_SEC && s_running; i++) {
         sleep(1);
      }

      if (!s_running) {
         break;
      }

      /* Run cleanup of expired data */
      int cleanup_result = auth_db_run_cleanup();
      if (cleanup_result != AUTH_DB_SUCCESS) {
         OLOG_WARNING("auth_maintenance: cleanup failed");
      }

      /* Clean up old images past retention period */
      if (image_store_is_ready()) {
         int deleted = 0;
         image_store_cleanup(&deleted);
         if (deleted > 0) {
            OLOG_INFO("auth_maintenance: cleaned %d old images", deleted);
         }
      }

      /* Prune document/note version history past retention (v62 undo store).
       * Same retention-sweep shape as the image cleanup above; no-op when
       * versioning is disabled (version_retention_days <= 0). */
      {
         int v_deleted = 0;
         if (document_db_version_prune_expired(g_config.documents.version_retention_days,
                                               &v_deleted) == SUCCESS &&
             v_deleted > 0) {
            OLOG_INFO("auth_maintenance: pruned %d expired document version(s)", v_deleted);
         }
      }

      /* Document originals (v68): age-delete (off by default, retention_days=0)
       * plus orphan reclamation (uploaded-but-never-indexed + post-delete).
       * Gated on readiness only, NOT originals_enabled: if an operator disables
       * originals after files exist, the sweep must still reclaim them. */
      if (document_originals_ready()) {
         int aged = 0, orphans = 0;
         document_original_cleanup(&aged);
         document_original_cleanup_orphans(g_config.documents.original_grace_minutes * 60,
                                           &orphans);
         if (aged > 0 || orphans > 0) {
            OLOG_INFO("auth_maintenance: document originals — %d aged, %d orphan", aged, orphans);
         }
      }

      /* Flush the current STAT telemetry rollup bucket to stat.db + prune old
       * buckets.  Own DB/WAL, so ordering vs the auth.db checkpoint below is
       * irrelevant.  Failure-isolated: never blocks the steps that follow.
       * NOTE: telemetry history bucket granularity == AUTH_MAINTENANCE_INTERVAL_SEC
       * (this loop's period) — retuning that interval also changes STAT's history
       * resolution. */
#ifdef DAWN_ENABLE_STAT_TOOL
      stat_history_flush();
#endif

      /* Passive WAL checkpoint (non-blocking) */
      int checkpoint_result = auth_db_checkpoint_passive();
      if (checkpoint_result != AUTH_DB_SUCCESS) {
         OLOG_WARNING("auth_maintenance: checkpoint failed");
      }

      /* Save and clear idle session conversations (satellites, WebUI) */
      session_check_idle_conversations();

      /* Clean up expired sessions (normally done by accept_thread, but that
       * only runs when [network] is enabled — WebUI satellites need this too) */
      session_cleanup_expired();

      /* Nightly memory decay and pruning (no-ops if not time yet) */
      memory_run_nightly_decay();
   }

   OLOG_INFO("auth_maintenance: thread stopped");
   return NULL;
}

int auth_maintenance_start(void) {
   pthread_mutex_lock(&s_mutex);

   if (s_running) {
      pthread_mutex_unlock(&s_mutex);
      OLOG_WARNING("auth_maintenance: already running");
      return 0;
   }

   s_running = true;

   int rc = pthread_create(&s_maintenance_thread, NULL, maintenance_thread_func, NULL);
   if (rc != 0) {
      s_running = false;
      pthread_mutex_unlock(&s_mutex);
      OLOG_ERROR("auth_maintenance: failed to create thread: %d", rc);
      return FAILURE;
   }

   pthread_mutex_unlock(&s_mutex);
   return 0;
}

void auth_maintenance_stop(void) {
   pthread_mutex_lock(&s_mutex);

   if (!s_running) {
      pthread_mutex_unlock(&s_mutex);
      return;
   }

   s_running = false;
   pthread_mutex_unlock(&s_mutex);

   /* Wait for thread to exit */
   pthread_join(s_maintenance_thread, NULL);
}

int auth_maintenance_is_running(void) {
   return s_running ? 1 : 0;
}
