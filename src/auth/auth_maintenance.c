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
#include "core/session_manager.h"
#include "image_store.h"
#include "logging.h"

/* Thread state */
static pthread_t s_maintenance_thread;
static volatile bool s_running = false;
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
      LOG_WARNING("auth_maintenance: failed to set nice level");
   }

   LOG_INFO("auth_maintenance: thread started (interval=%ds)", AUTH_MAINTENANCE_INTERVAL_SEC);

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
         LOG_WARNING("auth_maintenance: cleanup failed");
      }

      /* Clean up old images past retention period */
      if (image_store_is_ready()) {
         int deleted = image_store_cleanup();
         if (deleted > 0) {
            LOG_INFO("auth_maintenance: cleaned %d old images", deleted);
         }
      }

      /* Passive WAL checkpoint (non-blocking) */
      int checkpoint_result = auth_db_checkpoint_passive();
      if (checkpoint_result != AUTH_DB_SUCCESS) {
         LOG_WARNING("auth_maintenance: checkpoint failed");
      }

      /* Save and clear idle session conversations (satellites, WebUI) */
      session_check_idle_conversations();

      /* Clean up expired sessions (normally done by accept_thread, but that
       * only runs when [network] is enabled — WebUI satellites need this too) */
      session_cleanup_expired();
   }

   LOG_INFO("auth_maintenance: thread stopped");
   return NULL;
}

int auth_maintenance_start(void) {
   pthread_mutex_lock(&s_mutex);

   if (s_running) {
      pthread_mutex_unlock(&s_mutex);
      LOG_WARNING("auth_maintenance: already running");
      return 0;
   }

   s_running = true;

   int rc = pthread_create(&s_maintenance_thread, NULL, maintenance_thread_func, NULL);
   if (rc != 0) {
      s_running = false;
      pthread_mutex_unlock(&s_mutex);
      LOG_ERROR("auth_maintenance: failed to create thread: %d", rc);
      return -1;
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
