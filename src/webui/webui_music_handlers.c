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
 * WebUI Music Streaming - Message Handlers
 *
 * WebSocket message handlers for music streaming.
 * Split from webui_music.c for maintainability.
 */

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "audio/music_db.h"
#include "audio/music_open.h"
#include "audio/music_source.h"
#include "core/path_utils.h"
#include "core/strbuf.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/volume_tool.h"
#include "utils/string_utils.h"
#include "webui/webui_music_internal.h"
#include "webui/webui_server.h"

/* =============================================================================
 * Helpers
 * ============================================================================= */

/* =============================================================================
 * Message Handlers
 * ============================================================================= */

void handle_music_subscribe(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
      return;
   }

   if (!webui_music_is_available()) {
      webui_music_send_error(conn, "UNAVAILABLE", "Music streaming is not available");
      return;
   }

   /* Initialize session state if needed */
   if (!webui_music_state_get_or_init(conn)) {
      webui_music_send_error(conn, "INIT_ERROR", "Failed to initialize music session");
      return;
   }

   session_music_state_t *state = (session_music_state_t *)conn->music_state;

   /* Parse quality and bitrate mode settings */
   if (payload) {
      bool reconfigure = false;
      music_quality_t new_quality = state->quality;
      music_bitrate_mode_t new_bitrate_mode = state->bitrate_mode;

      struct json_object *quality_obj;
      if (json_object_object_get_ex(payload, "quality", &quality_obj)) {
         new_quality = webui_music_parse_quality(json_object_get_string(quality_obj));
         if (new_quality != state->quality) {
            reconfigure = true;
         }
      }

      struct json_object *bitrate_mode_obj;
      if (json_object_object_get_ex(payload, "bitrate_mode", &bitrate_mode_obj)) {
         const char *mode_str = json_object_get_string(bitrate_mode_obj);
         if (mode_str && strcmp(mode_str, "cbr") == 0) {
            new_bitrate_mode = MUSIC_BITRATE_CBR;
         } else {
            new_bitrate_mode = MUSIC_BITRATE_VBR;
         }
         if (new_bitrate_mode != state->bitrate_mode) {
            reconfigure = true;
         }
      }

      if (reconfigure) {
         /* Update state values immediately for display purposes */
         state->quality = new_quality;
         state->bitrate_mode = new_bitrate_mode;

         /* Set pending values for the player's thread to reconfigure the encoder
          * safely: the values first, then the flag that publishes them */
         state->pending_quality = new_quality;
         state->pending_bitrate_mode = new_bitrate_mode;
         atomic_store(&state->reconfigure_requested, true);
      }
   }

   /* Send current state (send_state manages its own locking) */
   webui_music_send_state(conn, state);

   OLOG_INFO("WebUI music: Client subscribed (quality: %s, bitrate: %s)",
             QUALITY_NAMES[state->quality],
             state->bitrate_mode == MUSIC_BITRATE_VBR ? "VBR" : "CBR");
}

void handle_music_unsubscribe(ws_connection_t *conn) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
      return;
   }

   session_music_state_t *state = (session_music_state_t *)conn->music_state;
   if (!state) {
      return;
   }

   /* Stop playback and streaming (refused only for a closed player: this
    * thread closes it, in cleanup) */
   if (music_player_control_lock(state->player, true)) {
      music_player_stop(state->player);
      music_player_control_unlock(state->player);
   }

   OLOG_INFO("WebUI music: Client unsubscribed");
}

static void music_control_locked(ws_connection_t *conn,
                                 session_music_state_t *state,
                                 struct json_object *payload);

/* music_player_remove(), telling the client if the track that took the
 * removed one's place couldn't start */
static int remove_index(ws_connection_t *conn, music_player_t *player, int index0) {
   bool start_failed = false;
   int rc = music_player_remove(player, index0, &start_failed);
   if (start_failed) {
      webui_music_send_error(conn, "PLAYBACK_ERROR", "Failed to start playback");
   }
   return rc;
}

/*
 * True for a request that stops or replaces what's playing: only these take the
 * control lock, ending the wait of an LLM tool call holding it.  A read, a
 * volume change, a seek or a queue add never waits on that call.
 */
static bool replaces_playback(struct json_object *payload) {
   static const char *const k_actions[] = { "play",        "play_index", "next",
                                            "previous",    "stop",       "remove_from_queue",
                                            "clear_queue", "remove",     "clear" };
   struct json_object *action_obj;
   if (!json_object_object_get_ex(payload, "action", &action_obj)) {
      return false;
   }
   const char *action = json_object_get_string(action_obj);
   for (size_t i = 0; action && i < sizeof(k_actions) / sizeof(k_actions[0]); i++) {
      if (strcmp(action, k_actions[i]) == 0) {
         return true;
      }
   }
   return false;
}

void handle_music_control(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
      return;
   }

   /* Auto-initialize on first control message */
   session_music_state_t *state = webui_music_state_get_or_init(conn);
   if (!state) {
      webui_music_send_error(conn, "INIT_ERROR", "Failed to initialize music session");
      return;
   }

   /* Only an action that stops or replaces playback needs the control lock
    * (and ends the wait of an LLM tool call holding it); the rest touch the
    * queue and state under their own mutexes, so the WebSocket thread never
    * waits. */
   bool replaces = replaces_playback(payload);
   if (replaces && !music_player_control_lock(state->player, true)) {
      return; /* closed: only this thread closes it, in cleanup */
   }
   music_control_locked(conn, state, payload);
   if (replaces) {
      music_player_control_unlock(state->player);
   }
}

/* handle_music_control's body, with the player's control lock held */
static void music_control_locked(ws_connection_t *conn,
                                 session_music_state_t *state,
                                 struct json_object *payload) {
   struct json_object *action_obj;
   if (!json_object_object_get_ex(payload, "action", &action_obj)) {
      webui_music_send_error(conn, "INVALID_REQUEST", "Missing action");
      return;
   }

   const char *action = json_object_get_string(action_obj);
   OLOG_INFO("WebUI music: Control action '%s'", action);
   music_player_t *player = state->player;
   user_music_queue_t *uq = music_player_queue(player);

   if (strcmp(action, "play") == 0) {
      /* Play specific track by path, or search query */
      struct json_object *path_obj;
      struct json_object *query_obj;

      if (json_object_object_get_ex(payload, "path", &path_obj)) {
         /* Play specific track by path - add to top of queue and play */
         const char *path = json_object_get_string(path_obj);

         /* Security: validate path is within music library */
         if (!music_path_is_valid(path)) {
            webui_music_send_error(conn, "INVALID_PATH", "Path not in music library");
            return;
         }

         /* Get track metadata from unified DB (all sources) */
         music_search_result_t track_info;
         bool found = false;
         if (music_db_get_by_path(path, &track_info, &found) != SUCCESS || !found) {
            /* Fallback: use path as title */
            safe_strncpy(track_info.path, path, sizeof(track_info.path));
            safe_strncpy(track_info.title, path, sizeof(track_info.title));
            track_info.artist[0] = '\0';
            track_info.album[0] = '\0';
            track_info.duration_sec = 0;
         }

         /* Stop any current playback */
         music_player_stop(player);

         /* Mutate shared queue */
         pthread_mutex_lock(&uq->queue_mutex);
         music_queue_insert(uq, 0, &track_info); /* at the top (a full queue drops its last) */
         music_queue_changed(uq, true);
         pthread_mutex_unlock(&uq->queue_mutex);

         /* Start playback */
         if (music_player_play(player, path, 0, NULL) != SUCCESS) {
            webui_music_send_error(conn, "PLAYBACK_ERROR", "Failed to start playback");
         }

         /* Send state to this tab, broadcast to others */
         webui_music_send_state(conn, state);
         webui_music_broadcast_queue_state(uq, state->player);

      } else if (json_object_object_get_ex(payload, "query", &query_obj)) {
         const char *query = json_object_get_string(query_obj);

         /* Search unified database (all sources) first: a query that finds
          * nothing leaves what's playing alone */
         music_search_result_t *results = malloc(MUSIC_QUEUE_MAX * sizeof(music_search_result_t));
         if (!results) {
            webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate search buffer");
            return;
         }

         int count = 0;
         music_query_t mq = { .text = query };
         if (music_db_query(&mq, results, MUSIC_QUEUE_MAX, &count) != SUCCESS || count <= 0) {
            free(results);
            webui_music_send_error(conn, "NOT_FOUND", "No music found matching query");
            return;
         }

         /* Stop any current playback */
         music_player_stop(player);

         music_queue_add(uq, results, count, true, NULL, NULL);
         char first_path[MUSIC_PATH_MAX];
         snprintf(first_path, sizeof(first_path), "%s", results[0].path);
         free(results);

         /* Start playback */
         if (music_player_play(player, first_path, 0, NULL) != SUCCESS) {
            webui_music_send_error(conn, "PLAYBACK_ERROR", "Failed to start playback");
         }

         /* Send state to this tab, broadcast to others */
         webui_music_send_state(conn, state);
         webui_music_broadcast_queue_state(uq, state->player);

      } else {
         /* A bare "play": resume a pause, or start the queue's current track
          * when stopped or nothing is loaded (it was removed or failed),
          * rather than silently echo state */
         if (music_player_resume_or_start(player, NULL) == MUSIC_RESUME_START_FAILED) {
            webui_music_send_error(conn, "PLAYBACK_ERROR", "Failed to start playback");
         }
         webui_music_send_state(conn, state);
      }

   } else if (strcmp(action, "pause") == 0) {
      music_player_set_paused(player, true);
      webui_music_send_state(conn, state);

   } else if (strcmp(action, "stop") == 0) {
      music_player_stop(player);
      webui_music_send_state(conn, state);

   } else if (strcmp(action, "next") == 0 || strcmp(action, "previous") == 0) {
      music_nav_dir_t dir = strcmp(action, "next") == 0 ? MUSIC_NAV_NEXT : MUSIC_NAV_PREVIOUS;
      char path[MUSIC_PATH_MAX];
      if (!music_player_step(player, dir, path, sizeof(path), NULL, 0)) {
         if (dir == MUSIC_NAV_PREVIOUS) {
            /* Shuffle with no history yet, or the queue's start: say so */
            webui_music_send_error(conn, "NOTHING_EARLIER", "Nothing earlier to go back to");
         }
      } else if (music_player_play(player, path, -1, NULL) != SUCCESS) {
         webui_music_send_error(conn, "PLAYBACK_ERROR", "Failed to start playback");
      }
      webui_music_send_state(conn, state);

   } else if (strcmp(action, "toggle_shuffle") == 0) {
      pthread_mutex_lock(&uq->queue_mutex);
      uq->shuffle = !uq->shuffle;
      music_queue_changed(uq, false);
      pthread_mutex_unlock(&uq->queue_mutex);
      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(uq, state->player);

   } else if (strcmp(action, "cycle_repeat") == 0) {
      pthread_mutex_lock(&uq->queue_mutex);
      uq->repeat_mode = (music_repeat_mode_t)((uq->repeat_mode + 1) % 3);
      music_queue_changed(uq, false);
      pthread_mutex_unlock(&uq->queue_mutex);
      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(uq, state->player);

   } else if (strcmp(action, "seek") == 0) {
      struct json_object *position_obj;
      if (json_object_object_get_ex(payload, "position_sec", &position_obj)) {
         /* The player's thread applies it and sends the state */
         if (!music_player_seek(player, json_object_get_double(position_obj))) {
            webui_music_send_state(conn, state);
         }
      }

   } else if (strcmp(action, "play_index") == 0) {
      /* Play specific track from queue by index */
      struct json_object *index_obj;
      if (!json_object_object_get_ex(payload, "index", &index_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing index");
         return;
      }
      int play_index = json_object_get_int(index_obj);

      char play_path[MUSIC_PATH_MAX] = { 0 };
      pthread_mutex_lock(&uq->queue_mutex);
      if (play_index < 0 || play_index >= uq->queue_length) {
         pthread_mutex_unlock(&uq->queue_mutex);
         webui_music_send_error(conn, "INVALID_INDEX", "Index out of range");
         return;
      }
      snprintf(play_path, sizeof(play_path), "%s", uq->queue[play_index].path);
      pthread_mutex_unlock(&uq->queue_mutex);

      if (music_player_play(player, play_path, play_index, NULL) != SUCCESS) {
         webui_music_send_error(conn, "PLAYBACK_ERROR", "Failed to start playback");
      }

      webui_music_send_state(conn, state);

   } else if (strcmp(action, "add_to_queue") == 0) {
      /* Add track to end of queue without starting playback */
      struct json_object *path_obj;
      if (!json_object_object_get_ex(payload, "path", &path_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing path");
         return;
      }
      const char *path = json_object_get_string(path_obj);

      if (!music_path_is_valid(path)) {
         webui_music_send_error(conn, "INVALID_PATH", "Path not in music library");
         return;
      }

      music_search_result_t track_info;
      bool found = false;
      if (music_db_get_by_path(path, &track_info, &found) != SUCCESS || !found) {
         safe_strncpy(track_info.path, path, sizeof(track_info.path));
         safe_strncpy(track_info.title, path, sizeof(track_info.title));
         track_info.artist[0] = '\0';
         track_info.album[0] = '\0';
         track_info.duration_sec = 0;
      }

      pthread_mutex_lock(&uq->queue_mutex);
      music_queue_insert(uq, uq->queue_length, &track_info); /* at the end, unless full */
      music_queue_changed(uq, true);
      pthread_mutex_unlock(&uq->queue_mutex);
      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(uq, state->player);

   } else if (strcmp(action, "remove_from_queue") == 0) {
      /* Remove track from queue by index */
      struct json_object *index_obj;
      if (!json_object_object_get_ex(payload, "index", &index_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing index");
         return;
      }
      if (remove_index(conn, player, json_object_get_int(index_obj)) != SUCCESS) {
         webui_music_send_error(conn, "INVALID_INDEX", "Index out of range");
         return;
      }
      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(uq, state->player);

   } else if (strcmp(action, "clear_queue") == 0) {
      /* Clear entire queue and stop playback */
      music_player_clear_queue(player, NULL);
      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(uq, state->player);

   } else if (strcmp(action, "add_artist") == 0) {
      /* Add all tracks by an artist to queue */
      struct json_object *artist_obj;
      if (!json_object_object_get_ex(payload, "artist", &artist_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing artist");
         return;
      }
      const char *artist_name = json_object_get_string(artist_obj);

      music_search_result_t *tracks = malloc(100 * sizeof(music_search_result_t));
      if (!tracks) {
         webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate track list");
         return;
      }
      int count = 0;
      if (music_db_get_by_artist(artist_name, tracks, 100, &count) != SUCCESS || count == 0) {
         free(tracks);
         webui_music_send_error(conn, "NOT_FOUND", "No tracks found for artist");
         return;
      }

      pthread_mutex_lock(&uq->queue_mutex);
      int added = 0;
      while (added < count && music_queue_insert(uq, uq->queue_length, &tracks[added]) == SUCCESS) {
         added++;
      }
      music_queue_changed(uq, true);
      pthread_mutex_unlock(&uq->queue_mutex);

      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(uq, state->player);

      free(tracks);
      OLOG_INFO("WebUI music: Added %d tracks by '%s' to queue", added, artist_name);

   } else if (strcmp(action, "add_album") == 0) {
      /* Add all tracks from an album to queue */
      struct json_object *album_obj;
      if (!json_object_object_get_ex(payload, "album", &album_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing album");
         return;
      }
      const char *album_name = json_object_get_string(album_obj);

      music_search_result_t *tracks = malloc(50 * sizeof(music_search_result_t));
      if (!tracks) {
         webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate track list");
         return;
      }
      int count = 0;
      music_db_get_by_album(album_name, tracks, 50, &count);

      pthread_mutex_lock(&uq->queue_mutex);
      int added = 0;
      while (added < count && music_queue_insert(uq, uq->queue_length, &tracks[added]) == SUCCESS) {
         added++;
      }
      music_queue_changed(uq, true);
      pthread_mutex_unlock(&uq->queue_mutex);

      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(uq, state->player);

      free(tracks);
      OLOG_INFO("WebUI music: Added %d tracks from album '%s' to queue", added, album_name);

   } else if (strcmp(action, "volume") == 0) {
      /* Set music volume for this session */
      struct json_object *level_obj;
      if (!json_object_object_get_ex(payload, "level", &level_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing level");
         return;
      }
      double level = json_object_get_double(level_obj);
      if (level < 0.0)
         level = 0.0;
      if (level > 1.0)
         level = 1.0;
      conn->volume = (float)level;

      /* Send updated state so client confirms the value */
      webui_music_send_state(conn, state);
      OLOG_INFO("WebUI music: Volume set to %.0f%%", level * 100.0);

   } else {
      webui_music_send_error(conn, "UNKNOWN_ACTION", "Unknown control action");
   }
}

void handle_music_search(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
      return;
   }

   struct json_object *query_obj;
   if (!json_object_object_get_ex(payload, "query", &query_obj)) {
      webui_music_send_error(conn, "INVALID_REQUEST", "Missing query");
      return;
   }

   const char *query = json_object_get_string(query_obj);
   int limit = 50; /* Default limit */

   struct json_object *limit_obj;
   if (json_object_object_get_ex(payload, "limit", &limit_obj)) {
      limit = json_object_get_int(limit_obj);
      if (limit <= 0 || limit > MUSIC_QUEUE_MAX) {
         limit = 50;
      }
   }

   /* Search unified database (all sources) */
   if (!music_db_is_initialized()) {
      webui_music_send_error(conn, "UNAVAILABLE", "Music database not available");
      return;
   }
   music_search_result_t *results = malloc(limit * sizeof(music_search_result_t));
   if (!results) {
      webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate search buffer");
      return;
   }

   int count = 0;
   /* Ranked, path-clean query so the browser search box matches the assistant's
    * results (real artist first, folder-name false-positives dropped). */
   music_query_t mq = { .text = query, .list_source = true };
   if (music_db_query(&mq, results, limit, &count) != SUCCESS) {
      free(results);
      webui_music_send_error(conn, "SEARCH_FAILED", "Music search failed");
      return;
   }

   /* Build response */
   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("music_search_response"));

   struct json_object *resp_payload = json_object_new_object();
   json_object_object_add(resp_payload, "query", json_object_new_string(query));
   json_object_object_add(resp_payload, "count", json_object_new_int(count));

   struct json_object *results_arr = json_object_new_array();
   for (int i = 0; i < count; i++) {
      struct json_object *track = json_object_new_object();
      json_object_object_add(track, "path", json_object_new_string(results[i].path));
      json_object_object_add(track, "title", json_object_new_string(results[i].title));
      json_object_object_add(track, "artist", json_object_new_string(results[i].artist));
      json_object_object_add(track, "album", json_object_new_string(results[i].album));
      json_object_object_add(track, "display_name",
                             json_object_new_string(results[i].display_name));
      json_object_object_add(track, "duration_sec", json_object_new_int(results[i].duration_sec));
      json_object_array_add(results_arr, track);
   }
   json_object_object_add(resp_payload, "results", results_arr);

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);

   free(results);
}

/**
 * @brief Extract and clamp pagination parameters from JSON payload
 */
static void parse_pagination(struct json_object *payload, int *limit, int *offset) {
   *limit = 50;
   *offset = 0;

   struct json_object *limit_obj, *offset_obj;
   if (payload && json_object_object_get_ex(payload, "limit", &limit_obj))
      *limit = json_object_get_int(limit_obj);
   if (payload && json_object_object_get_ex(payload, "offset", &offset_obj))
      *offset = json_object_get_int(offset_obj);

   if (*limit < 1)
      *limit = 1;
   if (*limit > 200)
      *limit = 200;
   if (*offset < 0)
      *offset = 0;
}

void handle_music_library(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
      return;
   }

   const char *browse_type = "stats"; /* Default */

   struct json_object *type_obj;
   if (payload && json_object_object_get_ex(payload, "type", &type_obj)) {
      browse_type = json_object_get_string(type_obj);
   }

   /* Unified database (all sources) */
   if (!music_db_is_initialized()) {
      webui_music_send_error(conn, "UNAVAILABLE", "Music database not available");
      return;
   }

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("music_library_response"));

   struct json_object *resp_payload = json_object_new_object();
   json_object_object_add(resp_payload, "browse_type", json_object_new_string(browse_type));

   if (strcmp(browse_type, "stats") == 0) {
      music_db_stats_t stats;
      music_db_get_stats(&stats);

      json_object_object_add(resp_payload, "track_count", json_object_new_int(stats.track_count));
      json_object_object_add(resp_payload, "artist_count", json_object_new_int(stats.artist_count));
      json_object_object_add(resp_payload, "album_count", json_object_new_int(stats.album_count));

   } else if (strcmp(browse_type, "tracks") == 0) {
      /* Paginated track listing */
      int limit, offset;
      parse_pagination(payload, &limit, &offset);

      music_search_result_t *tracks = malloc(limit * sizeof(music_search_result_t));
      if (!tracks) {
         webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate track list");
         json_object_put(response);
         return;
      }
      int count = 0;
      music_db_list_paged(tracks, limit, offset, &count);

      /* Include total count for pagination */
      music_db_stats_t stats;
      music_db_get_stats(&stats);

      struct json_object *tracks_arr = json_object_new_array();
      for (int i = 0; i < count; i++) {
         struct json_object *track = json_object_new_object();
         json_object_object_add(track, "path", json_object_new_string(tracks[i].path));
         json_object_object_add(track, "title", json_object_new_string(tracks[i].title));
         json_object_object_add(track, "artist", json_object_new_string(tracks[i].artist));
         json_object_object_add(track, "album", json_object_new_string(tracks[i].album));
         json_object_object_add(track, "duration_sec", json_object_new_int(tracks[i].duration_sec));
         json_object_array_add(tracks_arr, track);
      }
      json_object_object_add(resp_payload, "tracks", tracks_arr);
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      json_object_object_add(resp_payload, "total_count", json_object_new_int(stats.track_count));
      json_object_object_add(resp_payload, "offset", json_object_new_int(offset));
      free(tracks);

   } else if (strcmp(browse_type, "artists") == 0) {
      /* Paginated artist listing */
      int limit, offset;
      parse_pagination(payload, &limit, &offset);

      music_artist_info_t *artists = malloc(limit * sizeof(music_artist_info_t));
      if (!artists) {
         webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate artist list");
         json_object_put(response);
         return;
      }
      int count = 0;
      music_db_list_artists_with_stats(artists, limit, offset, &count);

      /* Include total count for pagination */
      music_db_stats_t stats;
      music_db_get_stats(&stats);

      struct json_object *artists_arr = json_object_new_array();
      for (int i = 0; i < count; i++) {
         struct json_object *artist = json_object_new_object();
         json_object_object_add(artist, "name", json_object_new_string(artists[i].name));
         json_object_object_add(artist, "album_count", json_object_new_int(artists[i].album_count));
         json_object_object_add(artist, "track_count", json_object_new_int(artists[i].track_count));
         json_object_array_add(artists_arr, artist);
      }
      json_object_object_add(resp_payload, "artists", artists_arr);
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      json_object_object_add(resp_payload, "total_count", json_object_new_int(stats.artist_count));
      json_object_object_add(resp_payload, "offset", json_object_new_int(offset));
      free(artists);

   } else if (strcmp(browse_type, "albums") == 0) {
      /* Paginated album listing */
      int limit, offset;
      parse_pagination(payload, &limit, &offset);

      music_album_info_t *albums = malloc(limit * sizeof(music_album_info_t));
      if (!albums) {
         webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate album list");
         json_object_put(response);
         return;
      }
      int count = 0;
      music_db_list_albums_with_stats(albums, limit, offset, &count);

      /* Include total count for pagination */
      music_db_stats_t stats;
      music_db_get_stats(&stats);

      struct json_object *albums_arr = json_object_new_array();
      for (int i = 0; i < count; i++) {
         struct json_object *album = json_object_new_object();
         json_object_object_add(album, "name", json_object_new_string(albums[i].name));
         json_object_object_add(album, "artist", json_object_new_string(albums[i].artist));
         json_object_object_add(album, "track_count", json_object_new_int(albums[i].track_count));
         json_object_array_add(albums_arr, album);
      }
      json_object_object_add(resp_payload, "albums", albums_arr);
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      json_object_object_add(resp_payload, "total_count", json_object_new_int(stats.album_count));
      json_object_object_add(resp_payload, "offset", json_object_new_int(offset));
      free(albums);

   } else if (strcmp(browse_type, "tracks_by_artist") == 0) {
      /* Get tracks for a specific artist */
      struct json_object *artist_obj;
      if (!json_object_object_get_ex(payload, "artist", &artist_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing artist name");
         json_object_put(response);
         return;
      }
      const char *artist_name = json_object_get_string(artist_obj);

      music_search_result_t *tracks = malloc(100 * sizeof(music_search_result_t));
      if (!tracks) {
         webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate track list");
         json_object_put(response);
         return;
      }
      int count = 0;
      music_db_get_by_artist(artist_name, tracks, 100, &count);

      struct json_object *tracks_arr = json_object_new_array();
      for (int i = 0; i < count; i++) {
         struct json_object *track = json_object_new_object();
         json_object_object_add(track, "path", json_object_new_string(tracks[i].path));
         json_object_object_add(track, "title", json_object_new_string(tracks[i].title));
         json_object_object_add(track, "artist", json_object_new_string(tracks[i].artist));
         json_object_object_add(track, "album", json_object_new_string(tracks[i].album));
         json_object_object_add(track, "duration_sec", json_object_new_int(tracks[i].duration_sec));
         json_object_array_add(tracks_arr, track);
      }
      json_object_object_add(resp_payload, "tracks", tracks_arr);
      json_object_object_add(resp_payload, "artist", json_object_new_string(artist_name));
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      free(tracks);

   } else if (strcmp(browse_type, "tracks_by_album") == 0) {
      /* Get tracks for a specific album */
      struct json_object *album_obj;
      if (!json_object_object_get_ex(payload, "album", &album_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing album name");
         json_object_put(response);
         return;
      }
      const char *album_name = json_object_get_string(album_obj);

      music_search_result_t *tracks = malloc(50 * sizeof(music_search_result_t));
      if (!tracks) {
         webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate track list");
         json_object_put(response);
         return;
      }
      int count = 0;
      music_db_get_by_album(album_name, tracks, 50, &count);

      struct json_object *tracks_arr = json_object_new_array();
      for (int i = 0; i < count; i++) {
         struct json_object *track = json_object_new_object();
         json_object_object_add(track, "path", json_object_new_string(tracks[i].path));
         json_object_object_add(track, "title", json_object_new_string(tracks[i].title));
         json_object_object_add(track, "artist", json_object_new_string(tracks[i].artist));
         json_object_object_add(track, "album", json_object_new_string(tracks[i].album));
         json_object_object_add(track, "duration_sec", json_object_new_int(tracks[i].duration_sec));
         json_object_array_add(tracks_arr, track);
      }
      json_object_object_add(resp_payload, "tracks", tracks_arr);
      json_object_object_add(resp_payload, "album", json_object_new_string(album_name));
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      free(tracks);
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

static void music_queue_locked(ws_connection_t *conn,
                               session_music_state_t *state,
                               struct json_object *payload);

void handle_music_queue(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
      return;
   }

   session_music_state_t *state = (session_music_state_t *)conn->music_state;
   if (!state) {
      webui_music_send_error(conn, "NOT_INITIALIZED", "Music session not initialized");
      return;
   }

   bool replaces = replaces_playback(payload);
   if (replaces && !music_player_control_lock(state->player, true)) {
      return; /* closed: only this thread closes it, in cleanup */
   }
   music_queue_locked(conn, state, payload);
   if (replaces) {
      music_player_control_unlock(state->player);
   }
}

/* handle_music_queue's body, with the player's control lock held */
static void music_queue_locked(ws_connection_t *conn,
                               session_music_state_t *state,
                               struct json_object *payload) {
   struct json_object *action_obj;
   if (!json_object_object_get_ex(payload, "action", &action_obj)) {
      webui_music_send_error(conn, "INVALID_REQUEST", "Missing action");
      return;
   }

   const char *action = json_object_get_string(action_obj);
   music_player_t *player = state->player;
   user_music_queue_t *uq = music_player_queue(player);

   if (strcmp(action, "list") == 0) {
      /* Return current queue */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type", json_object_new_string("music_queue_response"));

      struct json_object *resp_payload = json_object_new_object();

      pthread_mutex_lock(&uq->queue_mutex);

      struct json_object *queue_arr = json_object_new_array();
      for (int i = 0; i < uq->queue_length; i++) {
         music_queue_entry_t *entry = &uq->queue[i];
         struct json_object *track = json_object_new_object();
         json_object_object_add(track, "path", json_object_new_string(entry->path));
         json_object_object_add(track, "title", json_object_new_string(entry->title));
         json_object_object_add(track, "artist", json_object_new_string(entry->artist));
         json_object_object_add(track, "album", json_object_new_string(entry->album));
         json_object_object_add(track, "duration_sec", json_object_new_int(entry->duration_sec));
         json_object_array_add(queue_arr, track);
      }

      json_object_object_add(resp_payload, "queue", queue_arr);
      music_queue_entry_t now;
      int current_index = music_player_current(player, &now);
      json_object_object_add(resp_payload, "current_index", json_object_new_int(current_index));
      json_object_object_add(resp_payload, "length", json_object_new_int(uq->queue_length));

      pthread_mutex_unlock(&uq->queue_mutex);

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);

   } else if (strcmp(action, "clear") == 0) {
      music_player_clear_queue(player, NULL);
      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(uq, state->player);

   } else if (strcmp(action, "add") == 0) {
      struct json_object *path_obj;
      if (!json_object_object_get_ex(payload, "path", &path_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing path");
         return;
      }

      const char *path = json_object_get_string(path_obj);
      music_queue_outcome_t oc = MUSIC_QUEUE_NOT_FOUND;
      music_queue_apply_paths(uq, &path, 1, false, &oc);
      if (oc == MUSIC_QUEUE_NOT_FOUND) {
         webui_music_send_error(conn, "NOT_FOUND", "Track not found in music library");
         return;
      }

      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(uq, state->player);

   } else if (strcmp(action, "remove") == 0) {
      struct json_object *index_obj;
      if (!json_object_object_get_ex(payload, "index", &index_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing index");
         return;
      }

      int index = json_object_get_int(index_obj);
      if (remove_index(conn, player, index) != SUCCESS) {
         webui_music_send_error(conn, "INVALID_INDEX", "Queue index out of range");
         return;
      }

      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(uq, state->player);

   } else {
      webui_music_send_error(conn, "UNKNOWN_ACTION", "Unknown queue action");
   }
}
