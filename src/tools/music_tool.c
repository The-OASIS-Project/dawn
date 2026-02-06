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
#include "dawn.h"
#include "logging.h"
#include "mosquitto_comms.h"
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
static pthread_t s_music_thread = -1;

/* Pause/resume position tracking */
static uint64_t s_paused_position = 0;    /* Position in samples when paused */
static uint32_t s_paused_sample_rate = 0; /* Sample rate for position conversion */

/* ========== Forward Declarations ========== */

static char *music_tool_callback(const char *action, char *value, int *should_respond);
static void music_tool_cleanup(void);

/* ========== Tool Parameter Definition ========== */

static const treg_param_t music_params[] = {
   {
       .name = "action",
       .description = "The playback action: 'play' (start/search and play), 'stop' (halt), "
                      "'pause' (halt, keeps position), 'resume' (restart current track), "
                      "'next' (skip forward), 'previous' (skip backward), "
                      "'list' (show current playback queue - empty if nothing playing), "
                      "'select' (jump to track N in queue), "
                      "'search' (find music without playing), "
                      "'library' (browse music collection - list artists/albums/stats)",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "play", "stop", "pause", "resume", "next", "previous", "list", "select",
                        "search", "library" },
       .enum_count = 10,
   },
   {
       .name = "query",
       .description = "For 'play'/'search': search terms matching artist, album, or track. "
                      "For 'select': track number (1-based). "
                      "For 'library': 'artists', 'albums', or omit for stats.",
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
};

/* ========== Tool Metadata ========== */

static const tool_metadata_t music_metadata = {
   .name = "music",
   .device_string = "music",
   .topic = "dawn",
   .aliases = { "audio", "player" },
   .alias_count = 2,

   .description = "Control music playback. Actions: 'play' (search and play), 'stop' (halt), "
                  "'pause' (halt keeping position), 'resume' (restart current), "
                  "'next'/'previous' (skip tracks), 'list' (show playlist), "
                  "'select' (jump to track N), 'search' (find without playing).",
   .params = music_params,
   .param_count = 4,

   .device_type = TOOL_DEVICE_TYPE_MUSIC,
   .capabilities = TOOL_CAP_FILESYSTEM,
   .is_getter = false,
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
 * @brief Extract custom parameter from value string
 *
 * Value format: "base_value::field_name::field_value::..."
 * Returns the value for field_name, or NULL if not found.
 *
 * @param value Full value string (may contain custom params)
 * @param field_name Name of field to extract
 * @param out_value Buffer for extracted value
 * @param out_len Size of out_value buffer
 * @return true if found, false otherwise
 */
static bool extract_custom_param(const char *value,
                                 const char *field_name,
                                 char *out_value,
                                 size_t out_len) {
   if (!value || !field_name || !out_value)
      return false;

   /* Search for ::field_name:: pattern */
   char pattern[64];
   snprintf(pattern, sizeof(pattern), "::%s::", field_name);

   const char *pos = strstr(value, pattern);
   if (!pos)
      return false;

   /* Skip past the pattern to get to the value */
   const char *val_start = pos + strlen(pattern);

   /* Value ends at next :: or end of string */
   const char *val_end = strstr(val_start, "::");
   size_t val_len = val_end ? (size_t)(val_end - val_start) : strlen(val_start);

   if (val_len >= out_len)
      val_len = out_len - 1;

   strncpy(out_value, val_start, val_len);
   out_value[val_len] = '\0';
   return true;
}

/**
 * @brief Extract base value (before any custom params)
 *
 * @param value Full value string
 * @param out_base Buffer for base value
 * @param out_len Size of out_base buffer
 */
static void extract_base_value(const char *value, char *out_base, size_t out_len) {
   if (!value || !out_base)
      return;

   /* Base value ends at first :: */
   const char *delim = strstr(value, "::");
   size_t base_len = delim ? (size_t)(delim - value) : strlen(value);

   if (base_len >= out_len)
      base_len = out_len - 1;

   strncpy(out_base, value, base_len);
   out_base[base_len] = '\0';
}

/**
 * @brief Stop current playback and wait for thread to finish
 */
static void stop_current_playback(void) {
   if (s_music_thread != (pthread_t)-1) {
      setMusicPlay(0);
      int join_result = pthread_join(s_music_thread, NULL);
      if (join_result == 0) {
         s_music_thread = -1;
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
      LOG_ERROR("Failed to allocate PlaybackArgs");
      return NULL;
   }

   args->sink_name = getPcmPlaybackDevice();
   args->file_name = s_playlist.filenames[s_current_track];
   args->start_time = start_time;

   LOG_INFO("Playing from %us: %s on %s", start_time, args->file_name, args->sink_name);

   if (pthread_create(&s_music_thread, NULL, playFlacAudio, args)) {
      LOG_ERROR("Error creating music thread");
      free(args);
      return NULL;
   }

   return NULL;
}

/**
 * @brief Start playback of current track
 * @return Allocated result string, or NULL
 */
static char *start_playback(bool report_result) {
   PlaybackArgs *args = malloc(sizeof(PlaybackArgs));
   if (!args) {
      LOG_ERROR("Failed to allocate PlaybackArgs");
      return report_result ? strdup("Failed to start music playback") : NULL;
   }

   args->sink_name = getPcmPlaybackDevice();
   args->file_name = s_playlist.filenames[s_current_track];
   args->start_time = 0;

   LOG_INFO("Playing: %s %s %d", args->sink_name, args->file_name, args->start_time);

   if (pthread_create(&s_music_thread, NULL, playFlacAudio, args)) {
      LOG_ERROR("Error creating music thread");
      free(args);
      return report_result ? strdup("Failed to start music playback") : NULL;
   }

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
      LOG_WARNING("Music database not initialized - search unavailable");
      return 0;
   }

   /* Allocate results on heap to avoid ~300KB stack usage */
   music_search_result_t *results = malloc(MAX_PLAYLIST_LENGTH * sizeof(music_search_result_t));
   if (!results) {
      LOG_ERROR("Failed to allocate search results buffer");
      return -1;
   }

   int count = music_db_search(query, results, MAX_PLAYLIST_LENGTH);

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

/* ========== Callback Implementation ========== */

static char *music_tool_callback(const char *action, char *value, int *should_respond) {
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

      if (ret == 0) {
         /* WebUI handled successfully */
         LOG_INFO("Music: Routed '%s' to WebUI session", action);
         if (direct_mode) {
            *should_respond = 0;
            free(webui_result);
            return NULL;
         }
         return webui_result ? webui_result : strdup("OK");
      } else if (ret > 0) {
         /* WebUI handler returned error */
         LOG_WARNING("Music: WebUI handler failed for '%s'", action);
         if (direct_mode) {
            *should_respond = 0;
            free(webui_result);
            return NULL;
         }
         return webui_result ? webui_result : strdup("Music playback failed");
      }
      /* ret == -1 means not handled, fall through to local handler */
      free(webui_result);
      LOG_INFO("Music: WebUI deferred '%s' to local handler", action);
   }
#endif

   if (strcmp(action, "play") == 0) {
      /* Early validation - check before doing any work */
      if (!value || value[0] == '\0') {
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         return strdup("Please specify what to search for.");
      }

      if ((strlen(value) + 8) > MAX_FILENAME_LENGTH) {
         LOG_ERROR("\"%s\" is too long to search for.", value);
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         result = malloc(MUSIC_CALLBACK_BUFFER_SIZE);
         if (result) {
            snprintf(result, MUSIC_CALLBACK_BUFFER_SIZE, "Search term '%s' is too long", value);
         }
         return result;
      }

      /* Stop any current playback */
      stop_current_playback();

      /* Reset playlist and paused state */
      s_playlist.count = 0;
      s_current_track = 0;
      s_paused_position = 0;
      s_paused_sample_rate = 0;

      /* Search by artist, title, album via database */
      search_music_database(value, &s_playlist);
      LOG_INFO("Search found %d results for: %s", s_playlist.count, value);

      LOG_INFO("New playlist (%d tracks):", s_playlist.count);
      for (int i = 0; i < s_playlist.count; i++) {
         LOG_INFO("\t%s", s_playlist.filenames[i]);
      }

      if (s_playlist.count > 0) {
         if (direct_mode) {
            *should_respond = 0;
            start_playback(false);
            return NULL;
         }

         start_playback(false);
         const char *filename = extract_filename(s_playlist.filenames[s_current_track]);
         result = malloc(MUSIC_CALLBACK_BUFFER_SIZE);
         if (result) {
            snprintf(result, MUSIC_CALLBACK_BUFFER_SIZE,
                     "Now playing: %s (track 1 of %d matching '%s')", filename, s_playlist.count,
                     value);
         }
         return result;
      } else {
         LOG_WARNING("No music matching that description was found.");
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         result = malloc(MUSIC_CALLBACK_BUFFER_SIZE);
         if (result) {
            snprintf(result, MUSIC_CALLBACK_BUFFER_SIZE, "No music found matching '%s'", value);
         }
         return result;
      }

   } else if (strcmp(action, "stop") == 0) {
      LOG_INFO("Stopping music playback.");
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
         LOG_INFO("Paused at position %llu samples (rate: %u Hz)",
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
            LOG_INFO("Resuming from %u seconds (position: %llu samples)", start_seconds,
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
         return strdup("Failed to allocate playlist buffer");
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

      if (!value || !*value) {
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         return strdup("Search requires a query");
      }

      /* Extract query and optional limit from value */
      char query[MAX_FILENAME_LENGTH];
      extract_base_value(value, query, sizeof(query));

      char limit_str[16] = "";
      int limit = 10; /* Default: show 10 results */
      if (extract_custom_param(value, "limit", limit_str, sizeof(limit_str))) {
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
         return strdup("Search term too long");
      }

      /* Allocate search results on heap (~100KB struct) */
      search_results = malloc(sizeof(Playlist));
      if (!search_results) {
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         return strdup("Failed to allocate search buffer");
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
         return strdup("Failed to allocate result buffer");
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
         return strdup("Music database not available");
      }

      /* Extract page parameter (1-based, default 1) */
      int page = 1;
      char page_str[16] = "";
      if (extract_custom_param(value, "page", page_str, sizeof(page_str))) {
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
         extract_base_value(value, lib_query, sizeof(lib_query));
      }

      /* Default: show stats and first page of artists */
      if (!lib_query[0] || strcasecmp(lib_query, "stats") == 0) {
         music_db_stats_t stats;
         if (music_db_get_stats(&stats) != 0) {
            return strdup("Failed to get music library stats");
         }

         char artists[50][AUDIO_METADATA_STRING_MAX];
         int artist_count = music_db_list_artists(artists, per_page, db_offset);

         size_t buf_size = 4096;
         result = malloc(buf_size);
         if (!result)
            return strdup("Memory allocation failed");

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
            return strdup("Failed to get music library stats");
         }

         char artists[50][AUDIO_METADATA_STRING_MAX];
         int count = music_db_list_artists(artists, per_page, db_offset);

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
            return strdup("Memory allocation failed");

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
            return strdup("Failed to get music library stats");
         }

         char albums[50][AUDIO_METADATA_STRING_MAX];
         int count = music_db_list_albums(albums, per_page, db_offset);

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
            return strdup("Memory allocation failed");

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
         return strdup("Unknown library query. Use 'artists', 'albums', or omit for stats.");
      }
   }

   return NULL;
}

/* ========== Lifecycle Functions ========== */

static void music_tool_cleanup(void) {
   stop_current_playback();
   s_playlist.count = 0;
   s_current_track = 0;
}

/* ========== Public API ========== */

void set_music_directory(const char *path) {
   /* Deprecated: Music directory is now configured in dawn.toml [paths] section */
   /* The database-backed scanner uses that config value automatically */
   if (path) {
      LOG_WARNING("set_music_directory() is deprecated - configure [paths] music_dir in dawn.toml");
   }
}

int music_tool_register(void) {
   return tool_registry_register(&music_metadata);
}
