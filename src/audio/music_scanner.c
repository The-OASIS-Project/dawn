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
#include "audio/music_source.h"
#include "core/path_utils.h"
#include "dawn_error.h"
#include "logging.h"
#include "utils/string_utils.h"

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

/* Registered remote source providers */
#define MAX_PROVIDERS (MUSIC_SOURCE_COUNT - 1) /* Exclude LOCAL */
static const music_source_provider_t *g_providers[MAX_PROVIDERS];
static int g_provider_count = 0;

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

   OLOG_INFO("Music scanner thread started (interval: %d min)", g_scan_interval_min);

   pthread_mutex_lock(&g_scanner_mutex);

   while (g_running) {
      /* Run scan */
      pthread_mutex_unlock(&g_scanner_mutex);

      struct timespec scan_start, scan_end;
      clock_gettime(CLOCK_MONOTONIC, &scan_start);

      /* Phase 1: Local filesystem scan (if configured) */
      if (g_music_dir[0]) {
         OLOG_INFO("Starting local music scan: %s", g_music_dir);
         music_db_scan_stats_t stats;
         int result = music_db_scan(g_music_dir, &stats);
         if (result != 0) {
            OLOG_ERROR("Local music scan failed");
         }
      }

      /* Phase 2: Remote source syncs (registered providers) */
      for (int i = 0; i < g_provider_count; i++) {
         if (g_providers[i]->is_configured()) {
            OLOG_INFO("Syncing %s source...", music_source_name(g_providers[i]->source));
            g_providers[i]->sync();
         }
      }

      clock_gettime(CLOCK_MONOTONIC, &scan_end);
      double scan_secs = (scan_end.tv_sec - scan_start.tv_sec) +
                         (scan_end.tv_nsec - scan_start.tv_nsec) / 1e9;

      pthread_mutex_lock(&g_scanner_mutex);

      g_initial_scan_complete = true;
      int track_count = 0;
      music_db_get_track_count(&track_count);
      OLOG_INFO("Music scan cycle complete: %d tracks total (%.2fs)", track_count, scan_secs);

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
   OLOG_INFO("Music scanner thread exiting");
   return NULL;
}

/* =============================================================================
 * Public API Implementation
 * ============================================================================= */

int music_scanner_register_source(const music_source_provider_t *provider) {
   if (!provider)
      return FAILURE;
   if (g_running) {
      OLOG_ERROR("music_scanner: cannot register provider while scanner is running");
      return FAILURE;
   }
   if (g_provider_count >= MAX_PROVIDERS) {
      OLOG_ERROR("music_scanner: max providers (%d) reached", MAX_PROVIDERS);
      return FAILURE;
   }
   g_providers[g_provider_count++] = provider;
   OLOG_INFO("music_scanner: registered %s source provider", music_source_name(provider->source));
   return 0;
}

int music_scanner_start(const char *music_dir, int scan_interval_min, const char *db_path) {
   bool have_local = (music_dir && music_dir[0]);
   bool have_providers = (g_provider_count > 0);

   if (!have_local && !have_providers) {
      OLOG_ERROR("music_scanner_start: No music directory and no providers registered");
      return FAILURE;
   }

   char canonical_dir[MUSIC_DB_PATH_MAX] = { 0 };
   if (have_local) {
      /* Canonicalize path: expand tilde, resolve symlinks and ".." components */
      /* This prevents path traversal attacks via config (e.g., ~/Music/../../../etc) */
      if (!path_canonicalize(music_dir, canonical_dir, sizeof(canonical_dir))) {
         OLOG_ERROR("music_scanner_start: Cannot canonicalize path '%s' (does it exist?)",
                    music_dir);
         return FAILURE;
      }

      /* Validate music directory is accessible */
      struct stat st;
      if (stat(canonical_dir, &st) != 0) {
         OLOG_ERROR("music_scanner_start: Cannot access music directory '%s': %s", canonical_dir,
                    strerror(errno));
         return FAILURE;
      }
      if (!S_ISDIR(st.st_mode)) {
         OLOG_ERROR("music_scanner_start: Path is not a directory: %s", canonical_dir);
         return FAILURE;
      }
      if (access(canonical_dir, R_OK | X_OK) != 0) {
         OLOG_ERROR("music_scanner_start: No read/execute permission for directory '%s'",
                    canonical_dir);
         return FAILURE;
      }
   }

   if (!music_db_is_initialized()) {
      OLOG_ERROR("music_scanner_start: Music database not initialized");
      return FAILURE;
   }

   /* Initialize registered providers */
   for (int i = 0; i < g_provider_count; i++) {
      if (g_providers[i]->is_configured()) {
         if (g_providers[i]->init(db_path) != 0) {
            OLOG_WARNING("music_scanner: failed to init %s provider",
                         music_source_name(g_providers[i]->source));
         }
      }
   }

   pthread_mutex_lock(&g_scanner_mutex);

   if (g_running) {
      OLOG_WARNING("Music scanner already running");
      pthread_mutex_unlock(&g_scanner_mutex);
      return 0;
   }

   /* Store expanded path (empty string if no local dir) */
   if (have_local) {
      safe_strscpy(g_music_dir, canonical_dir);
   } else {
      g_music_dir[0] = '\0';
   }

   if (scan_interval_min < 0) {
      scan_interval_min = MUSIC_SCANNER_DEFAULT_INTERVAL_MIN;
   } else if (scan_interval_min > 0 && scan_interval_min < MUSIC_SCANNER_MIN_INTERVAL_MIN) {
      OLOG_WARNING("Scan interval %d min too low, using minimum of %d min", scan_interval_min,
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
      OLOG_ERROR("Failed to create scanner thread: %d", result);
      g_running = false;
      pthread_mutex_unlock(&g_scanner_mutex);
      return FAILURE;
   }

   pthread_mutex_unlock(&g_scanner_mutex);
   OLOG_INFO("Music scanner started: %s (interval: %d min, providers: %d)",
             have_local ? canonical_dir : "(no local dir)", scan_interval_min, g_provider_count);
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

   /* Cleanup registered providers */
   for (int i = 0; i < g_provider_count; i++) {
      g_providers[i]->cleanup();
   }

   OLOG_INFO("Music scanner stopped");
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
      OLOG_INFO("Music rescan requested");
   }

   pthread_mutex_unlock(&g_scanner_mutex);
}

bool music_scanner_initial_scan_complete(void) {
   pthread_mutex_lock(&g_scanner_mutex);
   bool complete = g_initial_scan_complete;
   pthread_mutex_unlock(&g_scanner_mutex);
   return complete;
}
