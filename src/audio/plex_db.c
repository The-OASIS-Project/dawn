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
 * Plex Database Sync Provider
 *
 * SCHEMA OWNERSHIP: This module does NOT create or alter tables/indexes.
 * All DDL is owned by music_db.c. This module only performs
 * INSERT/UPDATE/DELETE on rows WHERE source = MUSIC_SOURCE_PLEX.
 *
 * Two-phase sync:
 *   Phase 1 (no DB lock): Fetch all track pages from Plex API into memory
 *   Phase 2 (DB locked):  Bulk insert rows, delete stale rows via sync_gen
 */

#include "audio/plex_db.h"

#include <json-c/json.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio/audio_decoder.h"
#include "audio/music_db.h"
#include "audio/music_source.h"
#include "audio/plex_client.h"
#include "core/path_utils.h"
#include "dawn_error.h"
#include "logging.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define PLEX_DB_MAX_TRACKS 100000
#define PLEX_SYNC_PAGE_SIZE 500
#define PLEX_COMMIT_INTERVAL 500

/* =============================================================================
 * Internal Track Struct (for Phase 1 in-memory storage)
 * ============================================================================= */

typedef struct {
   char path[MUSIC_DB_PATH_MAX];
   char title[AUDIO_METADATA_STRING_MAX];
   char artist[AUDIO_METADATA_STRING_MAX];
   char album[AUDIO_METADATA_STRING_MAX];
   char genre[AUDIO_METADATA_STRING_MAX];
   char rating_key[32];
   uint32_t duration_sec;
   time_t updated_at;
} plex_track_t;

/* =============================================================================
 * Module State
 * ============================================================================= */

static sqlite3 *g_plex_db = NULL;
static pthread_mutex_t g_plex_db_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_plex_initialized = false;
static atomic_bool g_initial_sync_complete = false;
static int g_sync_gen = 0;
static _Atomic time_t g_last_scanned_at = 0;

/* =============================================================================
 * Internal Helpers
 * ============================================================================= */

/** Safe string copy with truncation for DB bind */
static void safe_copy(char *dst, size_t dst_size, const char *src) {
   if (!src || !src[0]) {
      dst[0] = '\0';
      return;
   }
   size_t len = strlen(src);
   if (len >= dst_size)
      len = dst_size - 1;
   memcpy(dst, src, len);
   dst[len] = '\0';
}

/** Safe JSON string extraction */
static const char *json_str(json_object *obj, const char *key) {
   json_object *val;
   if (json_object_object_get_ex(obj, key, &val) && json_object_is_type(val, json_type_string))
      return json_object_get_string(val);
   return "";
}

/** Safe JSON int extraction */
static int json_int(json_object *obj, const char *key) {
   json_object *val;
   if (json_object_object_get_ex(obj, key, &val) && json_object_is_type(val, json_type_int))
      return json_object_get_int(val);
   return 0;
}

/* =============================================================================
 * Phase 1: Fetch from Plex API into memory
 * ============================================================================= */

/* =============================================================================
 * Phase 2: Bulk insert into music_metadata
 * ============================================================================= */

static int bulk_insert_tracks(plex_track_t *tracks, int count, int *inserted_out) {
   if (inserted_out)
      *inserted_out = 0;

   pthread_mutex_lock(&g_plex_db_mutex);

   if (!g_plex_db) {
      pthread_mutex_unlock(&g_plex_db_mutex);
      return 1;
   }

   g_sync_gen++;
   int current_gen = g_sync_gen;

   static const char *SQL_UPSERT =
       "INSERT OR REPLACE INTO music_metadata "
       "(path, mtime, title, artist, album, genre, duration_sec, source, rating_key, sync_gen) "
       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

   sqlite3_stmt *stmt = NULL;
   int rc = sqlite3_prepare_v2(g_plex_db, SQL_UPSERT, -1, &stmt, NULL);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("Plex sync: failed to prepare insert: %s", sqlite3_errmsg(g_plex_db));
      pthread_mutex_unlock(&g_plex_db_mutex);
      return 1;
   }

   char *err_msg = NULL;
   sqlite3_exec(g_plex_db, "BEGIN TRANSACTION", NULL, NULL, &err_msg);
   if (err_msg) {
      sqlite3_free(err_msg);
      err_msg = NULL;
   }

   int inserted = 0;
   for (int i = 0; i < count; i++) {
      plex_track_t *t = &tracks[i];

      sqlite3_bind_text(stmt, 1, t->path, -1, SQLITE_STATIC);
      sqlite3_bind_int64(stmt, 2, (sqlite3_int64)t->updated_at);
      sqlite3_bind_text(stmt, 3, t->title[0] ? t->title : NULL, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 4, t->artist[0] ? t->artist : NULL, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 5, t->album[0] ? t->album : NULL, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 6, t->genre[0] ? t->genre : NULL, -1, SQLITE_STATIC);
      sqlite3_bind_int(stmt, 7, (int)t->duration_sec);
      sqlite3_bind_int(stmt, 8, MUSIC_SOURCE_PLEX);
      sqlite3_bind_text(stmt, 9, t->rating_key[0] ? t->rating_key : NULL, -1, SQLITE_STATIC);
      sqlite3_bind_int(stmt, 10, current_gen);

      rc = sqlite3_step(stmt);
      if (rc == SQLITE_DONE) {
         inserted++;
      } else {
         OLOG_WARNING("Plex sync: insert failed for '%s': %s", t->path, sqlite3_errmsg(g_plex_db));
      }

      sqlite3_reset(stmt);

      /* Commit every PLEX_COMMIT_INTERVAL rows to avoid holding WAL lock too long */
      if ((i + 1) % PLEX_COMMIT_INTERVAL == 0) {
         sqlite3_exec(g_plex_db, "COMMIT", NULL, NULL, NULL);
         sqlite3_exec(g_plex_db, "BEGIN TRANSACTION", NULL, NULL, NULL);
      }
   }

   sqlite3_exec(g_plex_db, "COMMIT", NULL, NULL, NULL);
   sqlite3_finalize(stmt);

   /* Delete stale Plex rows (from previous syncs that are no longer in the library) */
   char delete_sql[256];
   snprintf(delete_sql, sizeof(delete_sql),
            "DELETE FROM music_metadata WHERE source = %d AND sync_gen != %d", MUSIC_SOURCE_PLEX,
            current_gen);
   sqlite3_exec(g_plex_db, delete_sql, NULL, NULL, &err_msg);
   if (err_msg) {
      OLOG_WARNING("Plex sync: stale deletion error: %s", err_msg);
      sqlite3_free(err_msg);
   }

   pthread_mutex_unlock(&g_plex_db_mutex);
   if (inserted_out)
      *inserted_out = inserted;
   return 0;
}

/* =============================================================================
 * Provider Interface Implementation
 * ============================================================================= */

static int plex_init(const char *db_path) {
   /* Ensure API client is ready before first sync.
    * plex_client_init() is idempotent — safe if webui_music_init() calls it later. */
   if (plex_client_init() != 0) {
      OLOG_ERROR("Plex sync: failed to initialize Plex API client");
      return FAILURE;
   }

   pthread_mutex_lock(&g_plex_db_mutex);

   if (g_plex_initialized) {
      pthread_mutex_unlock(&g_plex_db_mutex);
      return 0;
   }

   /* Expand tilde in path */
   char expanded_path[MUSIC_DB_PATH_MAX];
   if (!path_expand_tilde(db_path, expanded_path, sizeof(expanded_path))) {
      OLOG_ERROR("Plex sync: failed to expand database path: %s", db_path);
      pthread_mutex_unlock(&g_plex_db_mutex);
      return FAILURE;
   }

   int rc = sqlite3_open(expanded_path, &g_plex_db);
   if (rc != SQLITE_OK) {
      OLOG_ERROR("Plex sync: failed to open database '%s': %s", expanded_path,
                 sqlite3_errmsg(g_plex_db));
      sqlite3_close(g_plex_db);
      g_plex_db = NULL;
      pthread_mutex_unlock(&g_plex_db_mutex);
      return FAILURE;
   }

   /* WAL mode for concurrent access with music_db.c's handle */
   sqlite3_exec(g_plex_db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
   sqlite3_exec(g_plex_db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
   sqlite3_busy_timeout(g_plex_db, 5000);

   g_plex_initialized = true;
   g_initial_sync_complete = false;
   g_sync_gen = 0;
   g_last_scanned_at = 0;

   OLOG_INFO("Plex sync: database handle opened: %s", expanded_path);
   pthread_mutex_unlock(&g_plex_db_mutex);
   return 0;
}

static void plex_cleanup(void) {
   pthread_mutex_lock(&g_plex_db_mutex);

   if (g_plex_db) {
      sqlite3_close(g_plex_db);
      g_plex_db = NULL;
   }
   g_plex_initialized = false;
   g_initial_sync_complete = false;

   pthread_mutex_unlock(&g_plex_db_mutex);
   OLOG_INFO("Plex sync: database handle closed");
}

static int plex_sync(void) {
   struct timespec sync_start;
   clock_gettime(CLOCK_MONOTONIC, &sync_start);

   /* Phase 0: Check if library has changed since last sync */
   time_t scanned_at = 0;
   if (plex_client_get_library_updated_at(&scanned_at) == 0) {
      if (scanned_at > 0 && scanned_at == g_last_scanned_at && g_initial_sync_complete) {
         OLOG_INFO("Plex sync: library unchanged (scannedAt=%ld), skipping", (long)scanned_at);
         return 0;
      }
   }

   /* Phase 1: Fetch all track pages from Plex API into memory */
   int total_tracks = 0;
   int capacity = 4096; /* Initial allocation */
   plex_track_t *tracks = malloc(capacity * sizeof(plex_track_t));
   if (!tracks) {
      OLOG_ERROR("Plex sync: failed to allocate track array");
      return FAILURE;
   }

   int offset = 0;
   bool fetching = true;
   while (fetching) {
      json_object *page = plex_client_list_all_tracks(offset, PLEX_SYNC_PAGE_SIZE);
      if (!page) {
         OLOG_ERROR("Plex sync: failed to fetch page at offset %d", offset);
         break;
      }

      /* Get total from first page */
      json_object *total_obj;
      if (json_object_object_get_ex(page, "total_count", &total_obj)) {
         int server_total = json_object_get_int(total_obj);
         if (server_total > 0 && capacity < server_total) {
            /* Resize to fit all tracks */
            int new_capacity = server_total < PLEX_DB_MAX_TRACKS ? server_total
                                                                 : PLEX_DB_MAX_TRACKS;
            plex_track_t *new_tracks = realloc(tracks, new_capacity * sizeof(plex_track_t));
            if (new_tracks) {
               tracks = new_tracks;
               capacity = new_capacity;
            }
         }
      }

      /* Extract tracks from the inner API response.
       * plex_client_list_all_tracks wraps the response with its own keys,
       * so we need to look inside the tracks array. */
      json_object *tracks_arr;
      int page_extracted = 0;
      if (json_object_object_get_ex(page, "tracks", &tracks_arr) &&
          json_object_is_type(tracks_arr, json_type_array)) {
         int page_count = (int)json_object_array_length(tracks_arr);

         for (int i = 0; i < page_count && total_tracks < capacity; i++) {
            json_object *item = json_object_array_get_idx(tracks_arr, i);
            plex_track_t *t = &tracks[total_tracks];
            memset(t, 0, sizeof(*t));

            /* Extract path */
            const char *path = json_str(item, "path");
            if (path[0]) {
               safe_copy(t->path, sizeof(t->path), path);
            } else {
               continue; /* Skip tracks without path */
            }

            safe_copy(t->title, sizeof(t->title), json_str(item, "title"));
            safe_copy(t->artist, sizeof(t->artist), json_str(item, "artist"));
            safe_copy(t->album, sizeof(t->album), json_str(item, "album"));
            safe_copy(t->rating_key, sizeof(t->rating_key), json_str(item, "rating_key"));

            const char *genre = json_str(item, "genre");
            if (genre[0])
               safe_copy(t->genre, sizeof(t->genre), genre);

            t->duration_sec = (uint32_t)json_int(item, "duration_sec");
            t->updated_at = (time_t)json_int(item, "updated_at");

            total_tracks++;
            page_extracted++;
         }
      }

      json_object_put(page);

      if (page_extracted < PLEX_SYNC_PAGE_SIZE) {
         fetching = false; /* Last page */
      } else if (total_tracks >= PLEX_DB_MAX_TRACKS) {
         OLOG_WARNING("Plex sync: track limit reached (%d), stopping fetch", PLEX_DB_MAX_TRACKS);
         fetching = false;
      } else {
         offset += PLEX_SYNC_PAGE_SIZE;
      }
   }

   if (total_tracks == 0) {
      OLOG_WARNING("Plex sync: no tracks fetched from Plex API");
      free(tracks);
      return FAILURE;
   }

   /* Phase 2: Bulk insert into database */
   int inserted = 0;
   if (bulk_insert_tracks(tracks, total_tracks, &inserted) != 0) {
      OLOG_ERROR("Plex sync: bulk insert failed");
      free(tracks);
      return FAILURE;
   }
   free(tracks);

   g_last_scanned_at = scanned_at;
   g_initial_sync_complete = true;

   struct timespec sync_end;
   clock_gettime(CLOCK_MONOTONIC, &sync_end);
   double sync_secs = (sync_end.tv_sec - sync_start.tv_sec) +
                      (sync_end.tv_nsec - sync_start.tv_nsec) / 1e9;

   OLOG_INFO("Plex sync: %d tracks synced (%.2fs)", inserted, sync_secs);
   return 0;
}

static bool plex_is_configured(void) {
   return plex_client_is_configured();
}

static bool plex_initial_sync_complete(void) {
   return g_initial_sync_complete;
}

/* =============================================================================
 * Provider Struct
 * ============================================================================= */

static const music_source_provider_t s_plex_provider = {
   .source = MUSIC_SOURCE_PLEX,
   .init = plex_init,
   .cleanup = plex_cleanup,
   .sync = plex_sync,
   .is_configured = plex_is_configured,
   .initial_sync_complete = plex_initial_sync_complete,
};

const music_source_provider_t *plex_db_get_provider(void) {
   return &s_plex_provider;
}
