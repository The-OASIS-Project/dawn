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
 * Music Tool - music playback on a player, and search of the music library
 *
 * Playback actions run on the caller's player: a WebUI tab's or a Tier-1
 * satellite's own, else the local speaker for its owner.  Search and the
 * library need no player.
 */

#define _GNU_SOURCE
#include "tools/music_tool.h"

#include <json-c/json.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "audio/audio_decoder.h"
#include "audio/music_db.h"
#include "audio/music_speaker.h"
#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "core/strbuf.h"
#include "dawn.h"
#include "dawn_error.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "tools/music_player_tool.h"
#include "tools/music_search.h"
#include "tools/tool_registry.h"
#include "utils/string_utils.h"

/* ========== Constants ========== */

#define MAX_FILENAME_LENGTH 1024

/* Albums/artists per 'library' page. (Search page sizes live in music_search.h.) */
#define MUSIC_LIBRARY_PER_PAGE 50

/* ========== Forward Declarations ========== */

static char *music_tool_callback(const char *action, char *value, int *should_respond);

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
           "For 'play'/'search'/'enqueue': search terms — best as one artist, title, or album per "
           "query (see the tool description for search rules). A track in both libraries plays "
           "the local copy; add 'source:plex' or 'source:local' to use that library's copy. For "
           "'select'/'remove': track "
           "number (1-based); 'remove' also accepts a title to match. For 'shuffle': 'on' or "
           "'off'. For 'repeat': 'none', 'all', or 'one'. For 'library': 'artists', 'albums' "
           "(add artist:'X' for that artist's albums), or omit for stats.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
   {
       .name = "limit",
       .description = "For 'search': results per page (default 25, max 100; 0 = max). A batch "
                      "search (items:[...]) returns up to 25 per query regardless.",
       .type = TOOL_PARAM_TYPE_NUMBER,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "limit",
   },
   {
       .name = "page",
       .description = "For 'search' and 'library': page number (1-based, default 1). Every "
                      "result states how many were found and how many pages there are — request "
                      "the next page when you need more. 'search' pages hold 'limit' tracks; "
                      "'library' pages hold 50 items.",
       .type = TOOL_PARAM_TYPE_NUMBER,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "page",
   },
   {
       .name = "genre",
       .description = "For 'search': filter results to a genre (substring, e.g. 'rock', "
                      "'jazz'). Combine with query/items or use alone.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "genre",
   },
   {
       .name = "artist",
       .description = "For 'search': restrict matching to the ARTIST field (e.g. artist:'Queen' "
                      "returns the band, not songs with 'Queen' in the title). Narrows a broad "
                      "free-text query. For 'library' with query 'albums': list this artist's "
                      "albums.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "artist",
   },
   {
       .name = "title",
       .description = "For 'search': restrict matching to the TITLE field.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "title",
   },
   {
       .name = "album",
       .description = "For 'search': restrict matching to the ALBUM field (e.g. album:'Whatever "
                      "and Ever Amen' lists that album's tracks). Combine with artist: to "
                      "disambiguate.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "album",
   },
   {
       .name = "year_min",
       .description = "For 'search': earliest release year (inclusive). Use with year_max for a "
                      "decade, e.g. year_min:1980 year_max:1989.",
       .type = TOOL_PARAM_TYPE_NUMBER,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "year_min",
   },
   {
       .name = "year_max",
       .description = "For 'search': latest release year (inclusive).",
       .type = TOOL_PARAM_TYPE_NUMBER,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "year_max",
   },
   {
       .name = "items",
       .description =
           "For 'play'/'enqueue': a list of track names or file paths to queue (names resolve to "
           "the best match, and accept the same source: word; paths from a 'search' result are "
           "exact). For 'search': a list of "
           "queries to run in one batch. Build playlists by collecting names/paths and passing "
           "them here.",
       .type = TOOL_PARAM_TYPE_ARRAY,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "items",
   },
};

/* ========== Tool Metadata ========== */

static const tool_action_kind_entry_t s_music_action_kinds[] = {
   { "list", TOOL_KIND_READ, NULL },
   { "search", TOOL_KIND_READ, NULL },
   { "library", TOOL_KIND_READ, NULL },
};

static const tool_metadata_t music_metadata = {
   .name = "music",
   .action_kinds = s_music_action_kinds,
   .action_kind_count = TOOL_KIND_COUNT(s_music_action_kinds),
   .default_kind = TOOL_KIND_DEVICE,
   .device_string = "music",
   .topic = "dawn",
   .aliases = { "audio", "player" },
   .alias_count = 2,

   .description =
       "Control music playback and build playlists from the local library. 'play' REPLACES the "
       "queue; 'enqueue' APPENDS (use it to add to an existing playlist). 'search' matches a query "
       "against artist/title/album/genre (punctuation and word order don't matter) and returns "
       "matches RANKED best-first (exact hits top, looser matches below — so you see options and "
       "choose), with album, year, and a path you can enqueue directly. Every search states how "
       "many tracks matched and how many pages there are; use page:N to see more. To narrow, "
       "restrict to a field: artist:'Queen' returns the band (not songs titled '…Queen…'), "
       "title:'…', album:'…'. 'search' also filters by genre:'jazz' and/or a year range "
       "(year_min/year_max, e.g. 1980..1989 for the 80s), with or without a text query. To see "
       "which albums an artist has, use library query:'albums' artist:'X' (one line per album, "
       "editions merged, with years and track counts) instead of paging through tracks. The "
       "library only reflects what's owned — for anything about music beyond it (discographies, "
       "releases, who fits a theme), use the web 'search' tool rather than memory. There is "
       "NO mood/vibe index — for a themed playlist, name concrete artists/songs, optionally "
       "narrow with genre/year, then batch-search "
       "(search items:[\"Prince\",\"Queen\"]) and enqueue the paths. All track numbers are "
       "1-based.",
   .params = music_params,
   .param_count = TOOL_PARAM_COUNT(music_params),

   .device_type = TOOL_DEVICE_TYPE_MUSIC,
   .capabilities = TOOL_CAP_FILESYSTEM | TOOL_CAP_SCHEDULABLE,
   .skip_followup = false,
   .default_remote = true,

   .config = NULL,
   .config_size = 0,
   .config_parser = NULL,
   .config_section = NULL,

   .init = NULL,
   .cleanup = NULL,
   .callback = music_tool_callback,
};


/* ========== Callback Implementation ========== */

/* No WebUI: no session has a player of its own */
__attribute__((weak)) music_player_t *music_player_for_session(struct session *session) {
   (void)session;
   return NULL;
}

/*
 * Run an action on a player (referenced; released here) and shape the reply:
 * silent in direct-only mode.  The player's control lock serializes it.
 */
static char *run_on_player(music_player_t *player,
                           const char *action,
                           char *value,
                           int *should_respond) {
   char *result = NULL;
   int ret = music_player_tool_execute(player, action, value, llm_tools_executing(), &result);
   music_player_release(player);
   if (ret != SUCCESS) {
      OLOG_WARNING("Music: '%s' failed on the player", action);
   }
   if (command_processing_mode == CMD_MODE_DIRECT_ONLY) {
      *should_respond = 0;
      free(result);
      return NULL;
   }
   return result ? result : strdup(ret == SUCCESS ? "OK" : "Music playback failed");
}

/* Search and the library: no player needed */
static char *music_tool_browse(const char *action, char *value, int *should_respond) {
   char *result = NULL;
   bool direct_mode = (command_processing_mode == CMD_MODE_DIRECT_ONLY);

   if (strcmp(action, "search") == 0) {
      /* Search without playing - return matching tracks */
      /* Optional filters shared by the batch and single-query paths. */
      music_search_filters_t filters;
      music_search_parse_filters(value, &filters);

      /* Batch search: items[] of queries → grouped results with paths. */
      if (value && value[0]) {
         size_t cap = strlen(value) + 1;
         char *items_json = malloc(cap);
         if (items_json && tool_param_extract_custom(value, "items", items_json, cap)) {
            struct json_object *arr = json_tokener_parse(items_json);
            free(items_json);
            /* Only treat this as a batch when items[] is NON-empty; an empty
             * items[] falls through to the single/filter path so a genre- or
             * year-only search (no query text, no items) still runs. */
            if (arr && json_object_is_type(arr, json_type_array) &&
                json_object_array_length(arr) > 0) {
               strbuf_t sb;
               strbuf_init(&sb, 1024);
               int n = (int)json_object_array_length(arr);
               if (n > MUSIC_BATCH_MAX_QUERIES) {
                  n = MUSIC_BATCH_MAX_QUERIES; /* each query is a full-library scan */
               }
               /* Heap-allocate the per-query result window (~2.5 KB/row) rather
                * than a large stack array. */
               music_search_result_t *r = malloc(MUSIC_BATCH_SEARCH_LIMIT *
                                                 sizeof(music_search_result_t));
               if (!r) {
                  json_object_put(arr);
                  strbuf_free(&sb);
                  if (direct_mode) {
                     *should_respond = 0;
                     return NULL;
                  }
                  return strdup(TOOL_RESULT_ERROR_MARK "Failed to allocate search buffer");
               }
               for (int i = 0; i < n; i++) {
                  struct json_object *e = json_object_array_get_idx(arr, i);
                  const char *q = e ? json_object_get_string(e) : NULL;
                  if (q && q[0]) {
                     music_search_append_batch_entry(&sb, q, &filters, r);
                  }
               }
               free(r);
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

      /* Single-query (or filter-only) search via the ranked query core. */
      char query[MAX_FILENAME_LENGTH];
      tool_param_extract_base(value ? value : "", query, sizeof(query));
      /* limit:0 means the largest page; an absent/garbled limit keeps the default. */
      int limit = music_search_parse_int(value, "limit", MUSIC_SEARCH_DEFAULT_LIMIT);
      int page_no = music_search_parse_int(value, "page", 1);

      if (!query[0] && !music_search_filters_any(&filters)) {
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         return strdup(TOOL_RESULT_ERROR_MARK "Search requires a query, artist, title, album, "
                                              "genre, or year range");
      }
      if ((strlen(query) + 8) > MAX_FILENAME_LENGTH) {
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         return strdup(TOOL_RESULT_ERROR_MARK "Search term too long");
      }
      if (direct_mode) {
         *should_respond = 0;
         return NULL;
      }
      char *out = music_search_page_text(query, &filters, page_no, limit);
      return out ? out : strdup(TOOL_RESULT_ERROR_MARK "Music search failed");

   } else if (strcmp(action, "library") == 0) {
      /* Browse music library - show stats, artists, or albums */
      if (!music_db_is_initialized()) {
         if (direct_mode) {
            *should_respond = 0;
            return NULL;
         }
         return strdup(TOOL_RESULT_ERROR_MARK "Music database not available");
      }

      /* Page parameter (1-based, default 1; clamped before any int math) */
      int page = music_search_parse_int(value, "page", 1);
      if (page < 1) {
         page = 1;
      }
      int per_page = MUSIC_LIBRARY_PER_PAGE;
      int db_offset = (page - 1) * per_page;

      /* Extract base query (strip ::page::N suffix) */
      char lib_query[64] = "";
      if (value && *value) {
         tool_param_extract_base(value, lib_query, sizeof(lib_query));
      }

      /* One artist's albums (discography view) — the right answer to "which of
       * X's albums do I have", instead of paging through duplicate-heavy tracks. */
      music_search_filters_t lib_filters;
      music_search_parse_filters(value, &lib_filters);
      if (strcasecmp(lib_query, "albums") == 0 && lib_filters.artist[0]) {
         result = music_library_albums_text(lib_filters.artist, page, per_page);
         if (direct_mode) {
            *should_respond = 0;
            free(result);
            return NULL;
         }
         return result ? result : strdup(TOOL_RESULT_ERROR_MARK "Failed to list albums");
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

static char *music_tool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1;
   /* A session with a player of its own (a WebUI tab, a Tier-1 satellite) */
   bool needs_player = strcmp(action, "search") != 0 && strcmp(action, "library") != 0;
   if (!needs_player) {
      return music_tool_browse(action, value, should_respond);
   }
   session_t *session = session_get_command_context();
   music_player_t *player = music_player_for_session(session);
   if (!player) {
      /* Else the local speaker, for its owner only, and never from a surface
       * with its own speaker (a closed tab, a satellite) or a job */
      session_t *local = session_get_local();
      int owner = local ? session_effective_user_id(local) : 0;
      if (!music_tool_speaker_allowed(session, tool_get_current_user_id(), owner)) {
         return strdup(TOOL_RESULT_ERROR_MARK "Music can't play from here; nothing changed");
      }
      player = music_speaker_acquire(owner);
      if (!player) {
         return strdup(TOOL_RESULT_ERROR_MARK "There's no speaker to play music on; nothing "
                                              "changed");
      }
   }
   return run_on_player(player, action, value, should_respond);
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
