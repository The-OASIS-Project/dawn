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
 * Music Tool - Audio playback with playlist management
 *
 * Supports actions: play, stop, next, previous, pause, resume, search, list
 * Searches music database by artist, title, or album.
 */

#define _GNU_SOURCE
#include "tools/music_tool.h"

#include <errno.h>
#include <json-c/json.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "audio/audio_decoder.h"
#include "audio/flac_playback.h"
#include "audio/music_db.h"
#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "core/strbuf.h"
#include "dawn.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/tool_registry.h"

/* WebUI music integration - route commands to browser when originating from WebUI */
#ifdef ENABLE_WEBUI
#include "webui/webui_internal.h"
#include "webui/webui_music.h"
#endif

/* ========== Constants ========== */

#define MAX_FILENAME_LENGTH 1024
#define MAX_PLAYLIST_LENGTH 100
#define MUSIC_CALLBACK_BUFFER_SIZE 512

/* Candidate window pulled from the DB for relevance ranking in the resolver. */
#define MUSIC_RESOLVE_CANDIDATES 8

/* ========== Types ========== */

/**
 * @struct Playlist
 * @brief Structure to hold the list of matching filenames and display names
 */
typedef struct {
   char filenames[MAX_PLAYLIST_LENGTH][MAX_FILENAME_LENGTH];
   char display_names[MAX_PLAYLIST_LENGTH]
                     [MAX_FILENAME_LENGTH]; /**< "Artist - Title" or filename */
   int count;
} Playlist;

/* ========== Static State ========== */

static Playlist s_playlist = { .count = 0 };
static int s_current_track = 0;
static pthread_t s_music_thread;
static bool s_thread_active = false;

/* Pause/resume position tracking */
static uint64_t s_paused_position = 0;    /* Position in samples when paused */
static uint32_t s_paused_sample_rate = 0; /* Sample rate for position conversion */

/* Protects s_playlist, s_current_track, s_paused_*, s_thread_active, s_music_thread */
static pthread_mutex_t s_music_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ========== Forward Declarations ========== */

static char *music_tool_callback(const char *action, char *value, int *should_respond);
static void music_tool_cleanup(void);

/* ========== Tool Parameter Definition ========== */

static const treg_param_t music_params[] = {
   {
       .name = "action",
       .description =
           "The action: 'play' (REPLACE the queue with search results or items, then play), "
           "'enqueue' (APPEND to the queue without interrupting — use this to add to an existing "
           "playlist), 'stop', 'pause', 'resume', 'next', 'previous', "
           "'list' (show queue + shuffle/repeat state), "
           "'select' (jump to track N, 1-based), "
           "'remove' (drop track N (1-based) or a title match from the queue), "
           "'clear' (empty the queue), "
           "'search' (find music without playing — returns paths), "
           "'library' (browse artists/albums/stats), "
           "'shuffle' ('on'/'off'; no arg reports current state), "
           "'repeat' ('none'/'all'/'one'; no arg reports current state)",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "play", "enqueue", "stop", "pause", "resume", "next", "previous", "list",
                        "select", "remove", "clear", "search", "library", "shuffle", "repeat" },
       .enum_count = 15,
   },
   {
       .name = "query",
       .description =
           "For 'play'/'search'/'enqueue': search terms — ONE artist OR title OR album per query "
           "(see the tool description for search rules). For 'select'/'remove': track number "
           "(1-based); 'remove' also accepts a title to match. For 'shuffle': 'on' or 'off'. "
           "For 'repeat': 'none', 'all', or 'one'. For 'library': 'artists', 'albums', or omit.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
   {
       .name = "limit",
       .description = "For 'search': maximum results to return (0 = all, default 10).",
       .type = TOOL_PARAM_TYPE_NUMBER,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "limit",
   },
   {
       .name = "page",
       .description = "For 'library': page number for pagination (1-based, default 1). "
                      "Each page returns up to 50 items. Use with 'artists' or 'albums'.",
       .type = TOOL_PARAM_TYPE_NUMBER,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "page",
   },
   /* ARRAY param MUST be declared last (terminal-slot contract — see
    * tool_registry.h). Only one ARRAY param per tool. */
   {
       .name = "items",
       .description =
           "For 'play'/'enqueue': a list of track names or file paths to queue (names resolve to "
           "the best match; paths from a 'search' result are exact). For 'search': a list of "
           "queries to run in one batch. Build playlists by collecting names/paths and passing "
           "them here.",
       .type = TOOL_PARAM_TYPE_ARRAY,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "items",
   },
};

/* ========== Tool Metadata ========== */

static const tool_metadata_t music_metadata = {
   .name = "music",
   .device_string = "music",
   .topic = "dawn",
   .aliases = { "audio", "player" },
   .alias_count = 2,

   .description =
       "Control music playback and build playlists from the local library. 'play' REPLACES the "
       "queue; 'enqueue' APPENDS (use it to add to an existing playlist). search/play match a "
       "query as a case-insensitive substring against ONE field (artist, title, album, or genre); "
       "spaces are wildcards WITHIN one field, so search by artist alone or title alone — never "
       "combine 'Artist Title' in one query. The library has NO decade/genre-mood/vibe index: "
       "'search 80s' only matches text literally containing '80s'. To build a themed/decade/mood "
       "playlist: (1) think of concrete artist & song names (use the web 'search' tool — distinct "
       "from this tool's 'search' action — if unsure who fits); (2) browse 'library' "
       "artists/albums to see what exists; (3) batch-search (search "
       "items:[\"Prince\",\"Madonna\"]) "
       "with ONE artist/title per query to get paths; (4) enqueue items:[paths] to add them. "
       "All track numbers are 1-based.",
   .params = music_params,
   .param_count = 5,

   .device_type = TOOL_DEVICE_TYPE_MUSIC,
   .capabilities = TOOL_CAP_FILESYSTEM | TOOL_CAP_SCHEDULABLE,
   .skip_followup = false,
   .default_remote = true,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = music_tool_cleanup,
   .callback = music_tool_callback,
};

/* ========== Helper Functions ========== */

/**
 * @brief Extract just the filename from a full path
 */
static const char *extract_filename(const char *path) {
   const char *filename = strrchr(path, '/');
   return filename ? filename + 1 : path;
}

/**
 * @brief Stop current playback and wait for thread to finish
 *
 * Note: When called from the playback thread itself (auto-advance via
 * music_tool_auto_advance), pthread_join returns EDEADLK (self-join).
 * In that case we clear s_thread_active so start_playback() can proceed;
 * the old thread exits naturally after music_tool_auto_advance() returns.
 */
static void stop_current_playback(void) {
   if (s_thread_active) {
      setMusicPlay(0);
      int join_result = pthread_join(s_music_thread, NULL);
      if (join_result == 0 || join_result == EDEADLK) {
         s_thread_active = false;
      }
   }
}

/**
 * @brief Start playback of current track at a specific position
 *
 * @param start_time Time in seconds to start playback from
 * @return Allocated result string, or NULL
 */
static char *start_playback_at(unsigned int start_time) {
   PlaybackArgs *args = malloc(sizeof(PlaybackArgs));
   if (!args) {
      OLOG_ERROR("Failed to allocate PlaybackArgs");
      return NULL;
   }

   args->sink_name = getPcmPlaybackDevice();
   args->file_name = s_playlist.filenames[s_current_track];
   args->start_time = start_time;

   OLOG_INFO("Playing from %us: %s on %s", start_time, args->file_name, args->sink_name);

   if (pthread_create(&s_music_thread, NULL, playFlacAudio, args)) {
      OLOG_ERROR("Error creating music thread");
      free(args);
      return NULL;
   }
   s_thread_active = true;

   return NULL;
}

/**
 * @brief Start playback of current track
 * @return Allocated result string, or NULL
 */
static char *start_playback(bool report_result) {
   PlaybackArgs *args = malloc(sizeof(PlaybackArgs));
   if (!args) {
      OLOG_ERROR("Failed to allocate PlaybackArgs");
      return report_result ? strdup("Failed to start music playback") : NULL;
   }

   args->sink_name = getPcmPlaybackDevice();
   args->file_name = s_playlist.filenames[s_current_track];
   args->start_time = 0;

   OLOG_INFO("Playing: %s %s %d", args->sink_name, args->file_name, args->start_time);

   if (pthread_create(&s_music_thread, NULL, playFlacAudio, args)) {
      OLOG_ERROR("Error creating music thread");
      free(args);
      return report_result ? strdup("Failed to start music playback") : NULL;
   }
   s_thread_active = true;

   if (!report_result) {
      return NULL;
   }

   const char *filename = extract_filename(s_playlist.filenames[s_current_track]);
   char *result = malloc(MUSIC_CALLBACK_BUFFER_SIZE);
   if (result) {
      snprintf(result, MUSIC_CALLBACK_BUFFER_SIZE, "Playing: %s", filename);
   }
   return result;
}

/**
 * @brief Search for music using the metadata database
 *
 * Falls back to filename-only search if database is not initialized.
 *
 * @param query Search query (space-separated terms)
 * @param playlist Output playlist to populate
 * @return Number of results found
 */
static int search_music_database(const char *query, Playlist *playlist) {
   if (!music_db_is_initialized()) {
      OLOG_WARNING("Music database not initialized - search unavailable");
      return 0;
   }

   /* Allocate results on heap to avoid ~300KB stack usage */
   music_search_result_t *results = malloc(MAX_PLAYLIST_LENGTH * sizeof(music_search_result_t));
   if (!results) {
      OLOG_ERROR("Failed to allocate search results buffer");
      return FAILURE;
   }

   int count = 0;
   if (music_db_search(query, results, MAX_PLAYLIST_LENGTH, &count) != SUCCESS) {
      free(results);
      return FAILURE;
   }

   if (count <= 0) {
      free(results);
      return count;
   }

   playlist->count = 0;
   for (int i = 0; i < count && playlist->count < MAX_PLAYLIST_LENGTH; i++) {
      strncpy(playlist->filenames[playlist->count], results[i].path, MAX_FILENAME_LENGTH - 1);
      playlist->filenames[playlist->count][MAX_FILENAME_LENGTH - 1] = '\0';

      strncpy(playlist->display_names[playlist->count], results[i].display_name,
              MAX_FILENAME_LENGTH - 1);
      playlist->display_names[playlist->count][MAX_FILENAME_LENGTH - 1] = '\0';

      playlist->count++;
   }

   free(results);
   return playlist->count;
}

/**
 * @brief Resolve one free-text item (or path) to its best-match library track
 *
 * Mirrors the WebUI resolver: best-match search, retry on the title portion of
 * "Artist - Title" (spaces are intra-field wildcards so the combined form
 * usually misses). Returns 1 on a match (fills @p out), 0 otherwise.
 */
static int local_resolve_item(const char *item, music_search_result_t *out) {
   if (!item || !item[0]) {
      return 0;
   }

   /* Absolute path → exact lookup (paths from 'search' are exact handles). The
    * local speaker plays files directly, so only local paths are resolvable
    * here; a plex: path falls through to search and won't match, which is
    * correct (local playback can't stream Plex). */
   if (item[0] == '/') {
      bool found = false;
      if (music_db_get_by_path(item, out, &found) == SUCCESS && found) {
         return 1;
      }
      return 0;
   }

   music_search_result_t r[MUSIC_RESOLVE_CANDIDATES];
   int count = 0;

   /* Split an optional "Artist - Title"; search the title (or whole item) and
    * rank candidates by relevance so a bare/generic title ("Africa") doesn't
    * pick the DB's alphabetical top row. */
   const char *dash = strstr(item, " - ");
   char artist[sizeof(out->artist)] = { 0 };
   const char *title_query = item;
   if (dash) {
      size_t alen = (size_t)(dash - item);
      if (alen >= sizeof(artist)) {
         alen = sizeof(artist) - 1;
      }
      memcpy(artist, item, alen);
      artist[alen] = '\0';
      title_query = dash + 3;
   }

   if (music_db_search(title_query, r, MUSIC_RESOLVE_CANDIDATES, &count) == SUCCESS && count > 0) {
      int pick = music_db_pick_best_match(r, count, title_query, dash ? artist : NULL);
      *out = r[pick];
      return 1;
   }
   return 0;
}

/**
 * @brief Fill s_playlist from a JSON array of item names/paths (local playback)
 *
 * Caller holds s_music_mutex. When @p replace is true the playlist is cleared
 * first. Returns the number of tracks added.
 */
static int local_fill_playlist_from_items(const char *items_json, bool replace) {
   struct json_object *arr = json_tokener_parse(items_json);
   if (!arr || !json_object_is_type(arr, json_type_array)) {
      if (arr) {
         json_object_put(arr);
      }
      return 0;
   }
   if (replace) {
      s_playlist.count = 0;
      s_current_track = 0;
   }
   int n = (int)json_object_array_length(arr);
   int added = 0;
   for (int i = 0; i < n && s_playlist.count < MAX_PLAYLIST_LENGTH; i++) {
      struct json_object *e = json_object_array_get_idx(arr, i);
      const char *item = e ? json_object_get_string(e) : NULL;
      music_search_result_t r;
      if (local_resolve_item(item, &r)) {
         snprintf(s_playlist.filenames[s_playlist.count], MAX_FILENAME_LENGTH, "%s", r.path);
         snprintf(s_playlist.display_names[s_playlist.count], MAX_FILENAME_LENGTH, "%s",
                  r.display_name);
         s_playlist.count++;
         added++;
      }
   }
   json_object_put(arr);
   return added;
}

/* ========== Callback Implementation ========== */

static char *music_tool_callback_inner(const char *action, char *value, int *should_respond);

static char *music_tool_callback(const char *action, char *value, int *should_respond) {
   pthread_mutex_lock(&s_music_mutex);
   char *result = music_tool_callback_inner(action, value, should_respond);
   pthread_mutex_unlock(&s_music_mutex);
   return result;
}

static char *music_tool_callback_inner(const char *action, char *value, int *should_respond) {
   char *result = NULL;

   *should_respond = 1;

   /* Get command processing mode from global */
   bool direct_mode = (command_processing_mode == CMD_MODE_DIRECT_ONLY);

#ifdef ENABLE_WEBUI
   /* Check if request originated from WebUI session - route to browser streaming */
   session_t *session = session_get_command_context();
   if (session && session->client_data) {
      ws_connection_t *conn = (ws_connection_t *)session->client_data;

      /* Try WebUI music handler for playback actions */
      char *webui_result = NULL;
      int ret = webui_music_execute_tool(conn, action, value, &webui_result);

      if (ret == SUCCESS) {
         /* WebUI handled successfully */
         OLOG_INFO("Music: Routed '%s' to WebUI session", action);
         if (direct_mode) {
            *should_respond = 0;
            free(webui_result);
            return NULL;
         }
         return webui_result ? webui_result : strdup("OK");
      } else if (ret > 0) {
         /* WebUI handler returned error */
         OLOG_WARNING("Music: WebUI handler failed for '%s'", action);
         if (direct_mode) {
            *should_respond = 0;
            free(webui_result);
            return NULL;
         }
         return webui_result ? webui_result : strdup("Music playback failed");
      }
      /* ret == MUSIC_NOT_HANDLED means not handled, fall through to local handler */
      free(webui_result);
      OLOG_INFO("Music: WebUI deferred '%s' to local handler", action);
   }
#endif

   if (strcmp(action, "play") == 0) {
      /* items[] → explicit track list (resolve each, REPLACE, play). */
      if (value && value[0]) {
         size_t cap = strlen(value) + 1;
         char *items_json = malloc(cap);
         if (items_json && tool_param_extract_custom_tail(value, "items", items_json, cap)) {
            stop_current_playback();
            s_paused_position = 0;
            s_paused_sample_rate = 0;
            int added = local_fill_playlist_from_items(items_json, true);
            free(items_json);
            if (s_playlist.count > 0) {
               s_current_track = 0;
               if (direct_mode) {
                  *should_respond = 0;
                  start_playback(false);
                  return NULL;
               }
               start_playback(false);
               result = malloc(MUSIC_CALLBACK_BUFFER_SIZE);
               if (result) {
                  snprintf(result, MUSIC_CALLBACK_BUFFER_SIZE,
                           "Now playing %d track%s from your list", added, added == 1 ? "" : "s");
               }
               return result;
            }
            if (direct_mode) {
               *should_respond = 0;
               return NULL;
            }
            return strdup(TOOL_RESULT_ERROR_MARK "None of those tracks were found in the library");
         }
         free(items_json);
      }

      /* Single-query play: search the base query (strip any custom-field suffix). */
      char base[MAX_FILENAME_LENGTH];
      tool_param_extract_base(value ? value : "", base, sizeof(base));

      /* Early validation - check before doing any work */
      if (base[0] == '\0') {
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         return strdup("Please specify what to play, or pass items:[...] for a track list.");
      }

      /* Stop any current playback */
      stop_current_playback();

      /* Reset playlist and paused state */
      s_playlist.count = 0;
      s_current_track = 0;
      s_paused_position = 0;
      s_paused_sample_rate = 0;

      /* Search by artist, title, album via database */
      search_music_database(base, &s_playlist);
      OLOG_INFO("Search found %d results for: %s", s_playlist.count, base);

      if (s_playlist.count > 0) {
         if (direct_mode) {
            *should_respond = 0;
            start_playback(false);
            return NULL;
         }

         start_playback(false);
         const char *filename = extract_filename(s_playlist.filenames[s_current_track]);
         size_t rsz = 2 * MAX_FILENAME_LENGTH + 64;
         result = malloc(rsz);
         if (result) {
            snprintf(result, rsz, "Now playing: %s (track 1 of %d matching '%s')", filename,
                     s_playlist.count, base);
         }
         return result;
      } else {
         OLOG_WARNING("No music matching that description was found.");
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         size_t rsz = MAX_FILENAME_LENGTH + 64;
         result = malloc(rsz);
         if (result) {
            snprintf(result, rsz, "No music found matching '%s'", base);
         }
         return result;
      }

   } else if (strcmp(action, "enqueue") == 0 || strcmp(action, "clear") == 0 ||
              strcmp(action, "remove") == 0 || strcmp(action, "shuffle") == 0 ||
              strcmp(action, "repeat") == 0) {
      /* Queue editing + shuffle/repeat require the per-user shared queue that
       * only the WebUI streaming path maintains. The local speaker has no queue
       * model — be honest rather than silently no-op. */
      if (direct_mode) {
         *should_respond = 0;
         return NULL;
      }
      return strdup("Queue editing and shuffle/repeat are available on the web interface.");

   } else if (strcmp(action, "stop") == 0) {
      OLOG_INFO("Stopping music playback.");
      stop_current_playback();

      if (direct_mode) {
         *should_respond = 0;
         return NULL;
      }
      return strdup("Music playback stopped");

   } else if (strcmp(action, "next") == 0) {
      stop_current_playback();

      /* Clear paused state - new track starts from beginning */
      s_paused_position = 0;
      s_paused_sample_rate = 0;

      if (s_playlist.count > 0) {
         s_current_track++;
         if (s_current_track >= s_playlist.count) {
            s_current_track = 0;
         }

         if (direct_mode) {
            *should_respond = 0;
            start_playback(false);
            return NULL;
         }

         start_playback(false);
         const char *filename = extract_filename(s_playlist.filenames[s_current_track]);
         result = malloc(MUSIC_CALLBACK_BUFFER_SIZE);
         if (result) {
            snprintf(result, MUSIC_CALLBACK_BUFFER_SIZE, "Playing next track: %s", filename);
         }
         return result;
      } else {
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         return strdup("No playlist available");
      }

   } else if (strcmp(action, "previous") == 0) {
      stop_current_playback();

      /* Clear paused state - new track starts from beginning */
      s_paused_position = 0;
      s_paused_sample_rate = 0;

      if (s_playlist.count > 0) {
         s_current_track--;
         if (s_current_track < 0) {
            s_current_track = s_playlist.count - 1;
         }

         if (direct_mode) {
            *should_respond = 0;
            start_playback(false);
            return NULL;
         }

         start_playback(false);
         const char *filename = extract_filename(s_playlist.filenames[s_current_track]);
         result = malloc(MUSIC_CALLBACK_BUFFER_SIZE);
         if (result) {
            snprintf(result, MUSIC_CALLBACK_BUFFER_SIZE, "Playing previous track: %s", filename);
         }
         return result;
      } else {
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         return strdup("No playlist available");
      }

   } else if (strcmp(action, "pause") == 0) {
      /* Pause halts playback but keeps track position for resume */
      if (s_playlist.count > 0 && getMusicPlay()) {
         /* Capture current position and sample rate before stopping */
         s_paused_position = audio_playback_get_position();
         s_paused_sample_rate = audio_playback_get_sample_rate();

         stop_current_playback();
         OLOG_INFO("Paused at position %llu samples (rate: %u Hz)",
                   (unsigned long long)s_paused_position, s_paused_sample_rate);
      } else {
         stop_current_playback();
      }

      if (direct_mode) {
         *should_respond = 0;
         return NULL;
      }
      return strdup("Music paused");

   } else if (strcmp(action, "resume") == 0) {
      /* Resume playback from paused position */
      if (s_playlist.count > 0) {
         stop_current_playback();

         /* Calculate start time from saved position */
         unsigned int start_seconds = 0;
         if (s_paused_position > 0 && s_paused_sample_rate > 0) {
            start_seconds = (unsigned int)(s_paused_position / s_paused_sample_rate);
            OLOG_INFO("Resuming from %u seconds (position: %llu samples)", start_seconds,
                      (unsigned long long)s_paused_position);
         }

         /* Clear paused state after using it */
         s_paused_position = 0;
         s_paused_sample_rate = 0;

         if (direct_mode) {
            *should_respond = 0;
            start_playback_at(start_seconds);
            return NULL;
         }

         start_playback_at(start_seconds);
         const char *filename = extract_filename(s_playlist.filenames[s_current_track]);
         result = malloc(MUSIC_CALLBACK_BUFFER_SIZE);
         if (result) {
            if (start_seconds > 0) {
               snprintf(result, MUSIC_CALLBACK_BUFFER_SIZE, "Resuming %s from %u:%02u", filename,
                        start_seconds / 60, start_seconds % 60);
            } else {
               snprintf(result, MUSIC_CALLBACK_BUFFER_SIZE, "Resuming: %s", filename);
            }
         }
         return result;
      } else {
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         return strdup("No track to resume");
      }

   } else if (strcmp(action, "list") == 0) {
      /* Return numbered list of current playlist */
      if (s_playlist.count == 0) {
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         return strdup("Playlist is empty");
      }

      /* Build playlist string: "1. Artist - Title\n2. Artist - Title\n..." */
      size_t buf_size = s_playlist.count * 256 + 64;
      result = malloc(buf_size);
      if (!result) {
         return strdup(TOOL_RESULT_ERROR_MARK "Failed to allocate playlist buffer");
      }

      int offset = snprintf(result, buf_size, "Playlist (%d tracks):\n", s_playlist.count);
      for (int i = 0; i < s_playlist.count && offset < (int)buf_size - 128; i++) {
         /* Use display name (Artist - Title) if available, else filename */
         const char *display = s_playlist.display_names[i][0]
                                   ? s_playlist.display_names[i]
                                   : extract_filename(s_playlist.filenames[i]);
         const char *marker = (i == s_current_track) ? " [playing]" : "";
         offset += snprintf(result + offset, buf_size - offset, "%d. %s%s\n", i + 1, display,
                            marker);
      }

      if (direct_mode) {
         *should_respond = 0;
         free(result);
         return NULL;
      }
      return result;

   } else if (strcmp(action, "select") == 0) {
      /* Jump to specific track by number (1-based) */
      if (s_playlist.count == 0) {
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         return strdup("No playlist available");
      }

      if (!value || !*value) {
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         return strdup("Please specify a track number");
      }
      int track_num = atoi(value);
      if (track_num < 1 || track_num > s_playlist.count) {
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         result = malloc(MUSIC_CALLBACK_BUFFER_SIZE);
         if (result) {
            snprintf(result, MUSIC_CALLBACK_BUFFER_SIZE, "Invalid track number. Choose 1-%d",
                     s_playlist.count);
         }
         return result;
      }

      stop_current_playback();
      s_current_track = track_num - 1;

      /* Clear paused state - new track starts from beginning */
      s_paused_position = 0;
      s_paused_sample_rate = 0;

      if (direct_mode) {
         *should_respond = 0;
         start_playback(false);
         return NULL;
      }

      start_playback(false);
      const char *filename = extract_filename(s_playlist.filenames[s_current_track]);
      result = malloc(MUSIC_CALLBACK_BUFFER_SIZE);
      if (result) {
         snprintf(result, MUSIC_CALLBACK_BUFFER_SIZE, "Now playing track %d: %s", track_num,
                  filename);
      }
      return result;

   } else if (strcmp(action, "search") == 0) {
      /* Search without playing - return matching tracks */
      Playlist *search_results = NULL;

      /* Batch search: items[] of queries → grouped results with paths. */
      if (value && value[0]) {
         size_t cap = strlen(value) + 1;
         char *items_json = malloc(cap);
         if (items_json && tool_param_extract_custom_tail(value, "items", items_json, cap)) {
            struct json_object *arr = json_tokener_parse(items_json);
            free(items_json);
            if (arr && json_object_is_type(arr, json_type_array)) {
               strbuf_t sb;
               strbuf_init(&sb, 1024);
               int n = (int)json_object_array_length(arr);
               if (n > MAX_PLAYLIST_LENGTH) {
                  n = MAX_PLAYLIST_LENGTH; /* bound per-call DB work */
               }
               for (int i = 0; i < n; i++) {
                  struct json_object *e = json_object_array_get_idx(arr, i);
                  const char *q = e ? json_object_get_string(e) : NULL;
                  if (!q || !q[0]) {
                     continue;
                  }
                  strbuf_appendf(&sb, "%s:\n", q);
                  music_search_result_t r[5];
                  int count = 0;
                  if (music_db_search(q, r, 5, &count) == SUCCESS && count > 0) {
                     for (int j = 0; j < count; j++) {
                        strbuf_appendf(&sb, "  %s  [%s]\n", r[j].display_name, r[j].path);
                     }
                  } else {
                     strbuf_appendf(&sb, "  No match: %s\n", q);
                  }
               }
               json_object_put(arr);
               if (direct_mode) {
                  *should_respond = 0;
                  strbuf_free(&sb);
                  return NULL;
               }
               char *out = strbuf_oom(&sb) ? NULL : strbuf_steal(&sb);
               if (!out) {
                  strbuf_free(&sb);
                  return strdup(TOOL_RESULT_ERROR_MARK "Failed to build search results");
               }
               return out;
            }
            if (arr) {
               json_object_put(arr);
            }
         } else {
            free(items_json);
         }
      }

      if (!value || !*value) {
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         return strdup(TOOL_RESULT_ERROR_MARK "Search requires a query");
      }

      /* Extract query and optional limit from value */
      char query[MAX_FILENAME_LENGTH];
      tool_param_extract_base(value, query, sizeof(query));

      char limit_str[16] = "";
      int limit = 10; /* Default: show 10 results */
      if (tool_param_extract_custom(value, "limit", limit_str, sizeof(limit_str))) {
         char *endptr;
         long parsed = strtol(limit_str, &endptr, 10);
         /* Check if conversion was successful (not empty and fully consumed) */
         if (endptr != limit_str && *endptr == '\0') {
            if (parsed == 0) {
               limit = MAX_PLAYLIST_LENGTH; /* 0 = show all */
            } else if (parsed < 0 || parsed > MAX_PLAYLIST_LENGTH) {
               limit = MAX_PLAYLIST_LENGTH;
            } else {
               limit = (int)parsed;
            }
         }
         /* If parsing failed, keep default limit of 10 */
      }

      /* Validate search term length */
      if ((strlen(query) + 8) > MAX_FILENAME_LENGTH) {
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         return strdup(TOOL_RESULT_ERROR_MARK "Search term too long");
      }

      /* Allocate search results on heap (~100KB struct) */
      search_results = malloc(sizeof(Playlist));
      if (!search_results) {
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         return strdup(TOOL_RESULT_ERROR_MARK "Failed to allocate search buffer");
      }
      search_results->count = 0;

      /* Search by artist, title, album via database */
      search_music_database(query, search_results);

      if (search_results->count == 0) {
         free(search_results);
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         result = malloc(MUSIC_CALLBACK_BUFFER_SIZE);
         if (result) {
            snprintf(result, MUSIC_CALLBACK_BUFFER_SIZE, "No music found matching '%.100s'", query);
         }
         return result;
      }

      /* Build result string using display names from database */
      size_t buf_size = search_results->count * 128 + 128;
      result = malloc(buf_size);
      if (!result) {
         free(search_results);
         return strdup(TOOL_RESULT_ERROR_MARK "Failed to allocate result buffer");
      }

      int offset = snprintf(result, buf_size, "Found %d tracks matching '%s':\n",
                            search_results->count, query);
      int max_show = (search_results->count > limit) ? limit : search_results->count;
      for (int i = 0; i < max_show && offset < (int)buf_size - 64; i++) {
         offset += snprintf(result + offset, buf_size - offset, "- %s\n",
                            search_results->display_names[i]);
      }
      if (search_results->count > max_show) {
         snprintf(result + offset, buf_size - offset, "... and %d more",
                  search_results->count - max_show);
      }

      free(search_results);

      if (direct_mode) {
         *should_respond = 0;
         free(result);
         return NULL;
      }
      return result;

   } else if (strcmp(action, "library") == 0) {
      /* Browse music library - show stats, artists, or albums */
      if (!music_db_is_initialized()) {
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         return strdup(TOOL_RESULT_ERROR_MARK "Music database not available");
      }

      /* Extract page parameter (1-based, default 1) */
      int page = 1;
      char page_str[16] = "";
      if (tool_param_extract_custom(value, "page", page_str, sizeof(page_str))) {
         char *endptr;
         long parsed = strtol(page_str, &endptr, 10);
         if (endptr != page_str && *endptr == '\0' && parsed >= 1) {
            page = (int)parsed;
         }
      }

      int per_page = 50;
      int db_offset = (page - 1) * per_page;

      /* Extract base query (strip ::page::N suffix) */
      char lib_query[64] = "";
      if (value && *value) {
         tool_param_extract_base(value, lib_query, sizeof(lib_query));
      }

      /* Default: show stats and first page of artists */
      if (!lib_query[0] || strcasecmp(lib_query, "stats") == 0) {
         music_db_stats_t stats;
         if (music_db_get_stats(&stats) != 0) {
            return strdup(TOOL_RESULT_ERROR_MARK "Failed to get music library stats");
         }

         char artists[50][AUDIO_METADATA_STRING_MAX];
         int artist_count = 0;
         if (music_db_list_artists(artists, per_page, db_offset, &artist_count) != SUCCESS) {
            return strdup(TOOL_RESULT_ERROR_MARK "Failed to list artists");
         }

         size_t buf_size = 4096;
         result = malloc(buf_size);
         if (!result)
            return strdup(TOOL_RESULT_ERROR_MARK "Memory allocation failed");

         int total_pages = (stats.artist_count + per_page - 1) / per_page;
         int off = snprintf(result, buf_size,
                            "Music library: %d tracks, %d artists, %d albums\n\n"
                            "Artists (page %d of %d, showing %d-%d of %d):\n",
                            stats.track_count, stats.artist_count, stats.album_count, page,
                            total_pages, db_offset + 1, db_offset + artist_count,
                            stats.artist_count);

         for (int i = 0; i < artist_count && off < (int)buf_size - 128; i++) {
            off += snprintf(result + off, buf_size - off, "- %s\n", artists[i]);
         }
         if (page < total_pages) {
            snprintf(result + off, buf_size - off, "\nUse page=%d to see more artists.", page + 1);
         }

         if (direct_mode) {
            *should_respond = 0;
            free(result);
            return NULL;
         }
         return result;

      } else if (strcasecmp(lib_query, "artists") == 0) {
         music_db_stats_t stats;
         if (music_db_get_stats(&stats) != 0) {
            return strdup(TOOL_RESULT_ERROR_MARK "Failed to get music library stats");
         }

         char artists[50][AUDIO_METADATA_STRING_MAX];
         int count = 0;
         if (music_db_list_artists(artists, per_page, db_offset, &count) != SUCCESS) {
            return strdup(TOOL_RESULT_ERROR_MARK "Failed to list artists");
         }

         if (count <= 0 && page == 1) {
            return strdup("No artists found in library");
         }
         if (count <= 0) {
            return strdup("No more artists (past last page)");
         }

         int total_pages = (stats.artist_count + per_page - 1) / per_page;
         size_t buf_size = count * 128 + 256;
         result = malloc(buf_size);
         if (!result)
            return strdup(TOOL_RESULT_ERROR_MARK "Memory allocation failed");

         int off = snprintf(result, buf_size,
                            "Artists (page %d of %d, showing %d-%d of %d total):\n", page,
                            total_pages, db_offset + 1, db_offset + count, stats.artist_count);
         for (int i = 0; i < count && off < (int)buf_size - 128; i++) {
            off += snprintf(result + off, buf_size - off, "- %s\n", artists[i]);
         }
         if (page < total_pages) {
            snprintf(result + off, buf_size - off, "\nUse page=%d to see more artists.", page + 1);
         }

         if (direct_mode) {
            *should_respond = 0;
            free(result);
            return NULL;
         }
         return result;

      } else if (strcasecmp(lib_query, "albums") == 0) {
         music_db_stats_t stats;
         if (music_db_get_stats(&stats) != 0) {
            return strdup(TOOL_RESULT_ERROR_MARK "Failed to get music library stats");
         }

         char albums[50][AUDIO_METADATA_STRING_MAX];
         int count = 0;
         if (music_db_list_albums(albums, per_page, db_offset, &count) != SUCCESS) {
            return strdup(TOOL_RESULT_ERROR_MARK "Failed to list albums");
         }

         if (count <= 0 && page == 1) {
            return strdup("No albums found in library");
         }
         if (count <= 0) {
            return strdup("No more albums (past last page)");
         }

         int total_pages = (stats.album_count + per_page - 1) / per_page;
         size_t buf_size = count * 128 + 256;
         result = malloc(buf_size);
         if (!result)
            return strdup(TOOL_RESULT_ERROR_MARK "Memory allocation failed");

         int off = snprintf(result, buf_size,
                            "Albums (page %d of %d, showing %d-%d of %d total):\n", page,
                            total_pages, db_offset + 1, db_offset + count, stats.album_count);
         for (int i = 0; i < count && off < (int)buf_size - 128; i++) {
            off += snprintf(result + off, buf_size - off, "- %s\n", albums[i]);
         }
         if (page < total_pages) {
            snprintf(result + off, buf_size - off, "\nUse page=%d to see more albums.", page + 1);
         }

         if (direct_mode) {
            *should_respond = 0;
            free(result);
            return NULL;
         }
         return result;

      } else {
         return strdup(TOOL_RESULT_ERROR_MARK
                       "Unknown library query. Use 'artists', 'albums', or omit for stats.");
      }
   }

   return NULL;
}

/* ========== Auto-Advance (called from playback thread) ========== */

void music_tool_auto_advance(void) {
   pthread_mutex_lock(&s_music_mutex);
   int should_respond = 0;
   char *result = music_tool_callback_inner("next", NULL, &should_respond);
   free(result);
   pthread_mutex_unlock(&s_music_mutex);
}

/* ========== Lifecycle Functions ========== */

static void music_tool_cleanup(void) {
   pthread_mutex_lock(&s_music_mutex);
   stop_current_playback();
   s_playlist.count = 0;
   s_current_track = 0;
   pthread_mutex_unlock(&s_music_mutex);
}

/* ========== Public API ========== */

void set_music_directory(const char *path) {
   /* Deprecated: Music directory is now configured in dawn.toml [paths] section */
   /* The database-backed scanner uses that config value automatically */
   if (path) {
      OLOG_WARNING(
          "set_music_directory() is deprecated - configure [paths] music_dir in dawn.toml");
   }
}

int music_tool_register(void) {
   return tool_registry_register(&music_metadata);
}
