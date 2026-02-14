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
 * Music Scanner - Background metadata indexing thread
 *
 * Periodically scans the music directory to keep the metadata database
 * up to date with new/changed/deleted files.
 */

#include "audio/music_scanner.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "audio/music_db.h"
#include "core/path_utils.h"
#include "logging.h"

/* =============================================================================
 * Module State
 * ============================================================================= */

static pthread_t g_scanner_thread;
static pthread_mutex_t g_scanner_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_scanner_cond = PTHREAD_COND_INITIALIZER;

static bool g_running = false;
static bool g_rescan_requested = false;
static bool g_initial_scan_complete = false;

static char g_music_dir[MUSIC_DB_PATH_MAX] = { 0 };
static int g_scan_interval_min = MUSIC_SCANNER_DEFAULT_INTERVAL_MIN;

/* =============================================================================
 * Scanner Thread
 * ============================================================================= */

/**
 * @brief Scanner thread main function
 *
 * Runs periodic scans at the configured interval.
 * Can be woken up early via music_scanner_trigger_rescan().
 */
static void *scanner_thread_func(void *arg) {
   (void)arg;

   LOG_INFO("Music scanner thread started (interval: %d min)", g_scan_interval_min);

   pthread_mutex_lock(&g_scanner_mutex);

   while (g_running) {
      /* Run scan */
      pthread_mutex_unlock(&g_scanner_mutex);

      LOG_INFO("Starting music library scan: %s", g_music_dir);
      struct timespec scan_start, scan_end;
      clock_gettime(CLOCK_MONOTONIC, &scan_start);

      music_db_scan_stats_t stats;
      int result = music_db_scan(g_music_dir, &stats);

      clock_gettime(CLOCK_MONOTONIC, &scan_end);
      double scan_secs = (scan_end.tv_sec - scan_start.tv_sec) +
                         (scan_end.tv_nsec - scan_start.tv_nsec) / 1e9;

      pthread_mutex_lock(&g_scanner_mutex);

      if (result == 0) {
         g_initial_scan_complete = true;
         LOG_INFO("Music scan complete: %d tracks indexed (%.2fs)", music_db_get_track_count(),
                  scan_secs);
      } else {
         LOG_ERROR("Music scan failed");
      }

      /* Clear any pending rescan request that accumulated during this scan */
      g_rescan_requested = false;

      /* Wait for next interval or signal */
      if (g_running && g_scan_interval_min > 0) {
         struct timespec ts;
         clock_gettime(CLOCK_REALTIME, &ts);
         ts.tv_sec += g_scan_interval_min * 60;

         while (g_running && !g_rescan_requested) {
            int wait_result = pthread_cond_timedwait(&g_scanner_cond, &g_scanner_mutex, &ts);
            if (wait_result == ETIMEDOUT) {
               break; /* Time for next scan */
            }
            /* If signaled, check if rescan requested or stopping */
         }
      } else if (g_running) {
         /* scan_interval_min == 0: wait indefinitely for rescan signal */
         while (g_running && !g_rescan_requested) {
            pthread_cond_wait(&g_scanner_cond, &g_scanner_mutex);
         }
      }
   }

   pthread_mutex_unlock(&g_scanner_mutex);
   LOG_INFO("Music scanner thread exiting");
   return NULL;
}

/* =============================================================================
 * Public API Implementation
 * ============================================================================= */

int music_scanner_start(const char *music_dir, int scan_interval_min) {
   if (!music_dir || strlen(music_dir) == 0) {
      LOG_ERROR("music_scanner_start: Invalid music directory");
      return -1;
   }

   /* Canonicalize path: expand tilde, resolve symlinks and ".." components */
   /* This prevents path traversal attacks via config (e.g., ~/Music/../../../etc) */
   char canonical_dir[MUSIC_DB_PATH_MAX];
   if (!path_canonicalize(music_dir, canonical_dir, sizeof(canonical_dir))) {
      LOG_ERROR("music_scanner_start: Cannot canonicalize path '%s' (does it exist?)", music_dir);
      return -1;
   }

   /* Validate music directory is accessible */
   struct stat st;
   if (stat(canonical_dir, &st) != 0) {
      LOG_ERROR("music_scanner_start: Cannot access music directory '%s': %s", canonical_dir,
                strerror(errno));
      return -1;
   }
   if (!S_ISDIR(st.st_mode)) {
      LOG_ERROR("music_scanner_start: Path is not a directory: %s", canonical_dir);
      return -1;
   }
   if (access(canonical_dir, R_OK | X_OK) != 0) {
      LOG_ERROR("music_scanner_start: No read/execute permission for directory '%s'",
                canonical_dir);
      return -1;
   }

   if (!music_db_is_initialized()) {
      LOG_ERROR("music_scanner_start: Music database not initialized");
      return -1;
   }

   pthread_mutex_lock(&g_scanner_mutex);

   if (g_running) {
      LOG_WARNING("Music scanner already running");
      pthread_mutex_unlock(&g_scanner_mutex);
      return 0;
   }

   /* Store expanded path */
   strncpy(g_music_dir, canonical_dir, sizeof(g_music_dir) - 1);
   g_music_dir[sizeof(g_music_dir) - 1] = '\0';

   if (scan_interval_min < 0) {
      scan_interval_min = MUSIC_SCANNER_DEFAULT_INTERVAL_MIN;
   } else if (scan_interval_min > 0 && scan_interval_min < MUSIC_SCANNER_MIN_INTERVAL_MIN) {
      LOG_WARNING("Scan interval %d min too low, using minimum of %d min", scan_interval_min,
                  MUSIC_SCANNER_MIN_INTERVAL_MIN);
      scan_interval_min = MUSIC_SCANNER_MIN_INTERVAL_MIN;
   }
   g_scan_interval_min = scan_interval_min;

   /* Start scanner thread */
   g_running = true;
   g_initial_scan_complete = false;
   g_rescan_requested = false;

   int result = pthread_create(&g_scanner_thread, NULL, scanner_thread_func, NULL);
   if (result != 0) {
      LOG_ERROR("Failed to create scanner thread: %d", result);
      g_running = false;
      pthread_mutex_unlock(&g_scanner_mutex);
      return -1;
   }

   pthread_mutex_unlock(&g_scanner_mutex);
   LOG_INFO("Music scanner started: %s (interval: %d min)", canonical_dir, scan_interval_min);
   return 0;
}

void music_scanner_stop(void) {
   pthread_mutex_lock(&g_scanner_mutex);

   if (!g_running) {
      pthread_mutex_unlock(&g_scanner_mutex);
      return;
   }

   g_running = false;
   pthread_cond_signal(&g_scanner_cond);

   pthread_mutex_unlock(&g_scanner_mutex);

   /* Wait for thread to finish (outside of mutex to avoid deadlock) */
   pthread_join(g_scanner_thread, NULL);

   LOG_INFO("Music scanner stopped");
}

bool music_scanner_is_running(void) {
   pthread_mutex_lock(&g_scanner_mutex);
   bool running = g_running;
   pthread_mutex_unlock(&g_scanner_mutex);
   return running;
}

void music_scanner_trigger_rescan(void) {
   pthread_mutex_lock(&g_scanner_mutex);

   if (g_running) {
      g_rescan_requested = true;
      pthread_cond_signal(&g_scanner_cond);
      LOG_INFO("Music rescan requested");
   }

   pthread_mutex_unlock(&g_scanner_mutex);
}

bool music_scanner_initial_scan_complete(void) {
   pthread_mutex_lock(&g_scanner_mutex);
   bool complete = g_initial_scan_complete;
   pthread_mutex_unlock(&g_scanner_mutex);
   return complete;
}
