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
 * WebSocket message handlers and LLM tool integration for music streaming.
 * Split from webui_music.c for maintainability.
 */

#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#include "audio/music_db.h"
#include "audio/music_source.h"
#include "core/path_utils.h"
#include "core/strbuf.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/volume_tool.h"
#include "webui/webui_music_internal.h"
#include "webui/webui_music_queue_db.h"
#include "webui/webui_server.h"

/* =============================================================================
 * Helpers
 * ============================================================================= */

/**
 * @brief Callback context for adjusting sibling session queue_index on remove
 */
typedef struct {
   int removed_index;
   ws_connection_t *exclude;
} remove_adjust_ctx_t;

/**
 * @brief Adjust queue_index for sibling sessions after a track removal
 *
 * Called via webui_for_each_conn_by_user for each sibling connection.
 * Locks state_mutex individually per session to adjust queue_index.
 */
static void adjust_index_on_remove(ws_connection_t *conn, void *ctx) {
   remove_adjust_ctx_t *rctx = (remove_adjust_ctx_t *)ctx;
   if (conn == rctx->exclude) {
      return;
   }
   session_music_state_t *s = (session_music_state_t *)conn->music_state;
   if (!s) {
      return;
   }
   pthread_mutex_lock(&s->state_mutex);
   if (rctx->removed_index < s->queue_index) {
      s->queue_index--;
   } else if (s->queue_index >= s->shared_queue->queue_length &&
              s->shared_queue->queue_length > 0) {
      s->queue_index = s->shared_queue->queue_length - 1;
   }
   pthread_mutex_unlock(&s->state_mutex);
}

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
   if (!conn->music_state) {
      if (webui_music_session_init(conn) != 0) {
         webui_music_send_error(conn, "INIT_ERROR", "Failed to initialize music session");
         return;
      }
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
         pthread_mutex_lock(&state->state_mutex);
         state->quality = new_quality;
         state->bitrate_mode = new_bitrate_mode;
         pthread_mutex_unlock(&state->state_mutex);

         /* Set pending values for streaming thread to reconfigure encoder safely */
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

   /* Stop playback and streaming */
   pthread_mutex_lock(&state->state_mutex);
   state->playing = false;
   state->paused = false;
   pthread_mutex_unlock(&state->state_mutex);

   webui_music_stop_streaming(state);

   OLOG_INFO("WebUI music: Client unsubscribed");
}

void handle_music_control(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
      return;
   }

   session_music_state_t *state = (session_music_state_t *)conn->music_state;
   if (!state) {
      /* Auto-initialize on first control message */
      if (webui_music_session_init(conn) != 0) {
         webui_music_send_error(conn, "INIT_ERROR", "Failed to initialize music session");
         return;
      }
      state = (session_music_state_t *)conn->music_state;
   }

   struct json_object *action_obj;
   if (!json_object_object_get_ex(payload, "action", &action_obj)) {
      webui_music_send_error(conn, "INVALID_REQUEST", "Missing action");
      return;
   }

   const char *action = json_object_get_string(action_obj);
   OLOG_INFO("WebUI music: Control action '%s'", action);

   if (strcmp(action, "play") == 0) {
      /* Play specific track by path, or search query */
      struct json_object *path_obj;
      struct json_object *query_obj;

      if (json_object_object_get_ex(payload, "path", &path_obj)) {
         /* Play specific track by path - add to top of queue and play */
         const char *path = json_object_get_string(path_obj);

         /* Security: validate path is within music library */
         if (!webui_music_is_path_valid(path)) {
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
         webui_music_stop_streaming(state);
         pthread_mutex_lock(&state->state_mutex);
         if (state->decoder) {
            audio_decoder_close(state->decoder);
            state->decoder = NULL;
         }
         pthread_mutex_unlock(&state->state_mutex);

         /* Mutate shared queue */
         user_music_queue_t *uq = state->shared_queue;
         pthread_mutex_lock(&uq->queue_mutex);

         /* Shift queue down and insert at position 0 */
         if (uq->queue_length < WEBUI_MUSIC_MAX_QUEUE) {
            memmove(&uq->queue[1], &uq->queue[0], uq->queue_length * sizeof(music_queue_entry_t));
            uq->queue_length++;
         } else {
            /* Queue full - shift and lose last item */
            memmove(&uq->queue[1], &uq->queue[0],
                    (WEBUI_MUSIC_MAX_QUEUE - 1) * sizeof(music_queue_entry_t));
         }

         /* Insert track at top */
         music_queue_entry_t *entry = &uq->queue[0];
         safe_strncpy(entry->path, track_info.path, sizeof(entry->path));
         safe_strncpy(entry->title, track_info.title, sizeof(entry->title));
         safe_strncpy(entry->artist, track_info.artist, sizeof(entry->artist));
         safe_strncpy(entry->album, track_info.album, sizeof(entry->album));
         entry->duration_sec = track_info.duration_sec;
         uq->generation++;
         music_queue_db_save(uq->user_id, uq);
         pthread_mutex_unlock(&uq->queue_mutex);

         /* Set per-session index */
         pthread_mutex_lock(&state->state_mutex);
         state->queue_index = 0;
         pthread_mutex_unlock(&state->state_mutex);

         /* Start playback */
         if (webui_music_start_playback(state, path) != 0) {
            webui_music_send_error(conn, "PLAYBACK_ERROR", "Failed to start playback");
            return;
         }

         /* Send state to this tab, broadcast to others */
         webui_music_send_state(conn, state);
         webui_music_broadcast_queue_state(uq, conn);

      } else if (json_object_object_get_ex(payload, "query", &query_obj)) {
         const char *query = json_object_get_string(query_obj);

         /* Stop any current playback */
         webui_music_stop_streaming(state);
         pthread_mutex_lock(&state->state_mutex);
         if (state->decoder) {
            audio_decoder_close(state->decoder);
            state->decoder = NULL;
         }
         pthread_mutex_unlock(&state->state_mutex);

         /* Search unified database (all sources) */
         music_search_result_t *results = malloc(WEBUI_MUSIC_MAX_QUEUE *
                                                 sizeof(music_search_result_t));
         if (!results) {
            webui_music_send_error(conn, "MEMORY_ERROR", "Failed to allocate search buffer");
            return;
         }

         int count = 0;
         if (music_db_search(query, results, WEBUI_MUSIC_MAX_QUEUE, &count) != SUCCESS ||
             count <= 0) {
            free(results);
            webui_music_send_error(conn, "NOT_FOUND", "No music found matching query");
            return;
         }

         user_music_queue_t *uq = state->shared_queue;
         pthread_mutex_lock(&uq->queue_mutex);
         uq->queue_length = 0;
         for (int i = 0; i < count && uq->queue_length < WEBUI_MUSIC_MAX_QUEUE; i++) {
            music_queue_entry_t *entry = &uq->queue[uq->queue_length];
            safe_strncpy(entry->path, results[i].path, sizeof(entry->path));
            safe_strncpy(entry->title, results[i].title, sizeof(entry->title));
            safe_strncpy(entry->artist, results[i].artist, sizeof(entry->artist));
            safe_strncpy(entry->album, results[i].album, sizeof(entry->album));
            entry->duration_sec = results[i].duration_sec;
            uq->queue_length++;
         }
         uq->generation++;
         music_queue_db_save(uq->user_id, uq);
         char first_path[WEBUI_MUSIC_PATH_MAX];
         snprintf(first_path, sizeof(first_path), "%s", uq->queue[0].path);
         pthread_mutex_unlock(&uq->queue_mutex);
         free(results);

         pthread_mutex_lock(&state->state_mutex);
         state->queue_index = 0;
         pthread_mutex_unlock(&state->state_mutex);

         /* Start playback */
         if (webui_music_start_playback(state, first_path) != 0) {
            webui_music_send_error(conn, "PLAYBACK_ERROR", "Failed to start playback");
            return;
         }

         /* Send state to this tab, broadcast to others */
         webui_music_send_state(conn, state);
         webui_music_broadcast_queue_state(uq, conn);

      } else if (state->paused) {
         /* Resume from pause */
         pthread_mutex_lock(&state->state_mutex);
         state->paused = false;
         pthread_mutex_unlock(&state->state_mutex);
         webui_music_send_state(conn, state);
      } else if (!state->playing) {
         /* Stopped with a populated queue: start the current queue track.
          * A bare "play" (no path/query) after a stop should resume the
          * queue, not silently echo state. */
         user_music_queue_t *uq = state->shared_queue;
         char play_path[WEBUI_MUSIC_PATH_MAX] = { 0 };

         /* Lock hierarchy: queue → state. Both released before start_playback,
          * which acquires them itself. */
         pthread_mutex_lock(&uq->queue_mutex);
         int q_len = uq->queue_length;
         pthread_mutex_lock(&state->state_mutex);
         if (q_len > 0) {
            if (state->queue_index < 0 || state->queue_index >= q_len) {
               state->queue_index = 0;
            }
            snprintf(play_path, sizeof(play_path), "%s", uq->queue[state->queue_index].path);
         }
         pthread_mutex_unlock(&state->state_mutex);
         pthread_mutex_unlock(&uq->queue_mutex);

         if (play_path[0]) {
            if (webui_music_start_playback(state, play_path) != 0) {
               webui_music_send_error(conn, "PLAYBACK_ERROR", "Failed to start playback");
               return;
            }
         }
         /* Empty queue: nothing to start; just echo state. */
         webui_music_send_state(conn, state);
      } else {
         /* Already playing - just send state */
         webui_music_send_state(conn, state);
      }

   } else if (strcmp(action, "pause") == 0) {
      pthread_mutex_lock(&state->state_mutex);
      state->paused = true;
      pthread_mutex_unlock(&state->state_mutex);
      webui_music_send_state(conn, state);

   } else if (strcmp(action, "stop") == 0) {
      /* Stop streaming thread completely before closing decoder. */
      webui_music_stop_streaming(state);

      pthread_mutex_lock(&state->state_mutex);
      state->playing = false;
      state->paused = false;
      if (state->decoder) {
         audio_decoder_close(state->decoder);
         state->decoder = NULL;
      }
      state->position_frames = 0;
      pthread_mutex_unlock(&state->state_mutex);
      webui_music_send_state(conn, state);

   } else if (strcmp(action, "next") == 0) {
      user_music_queue_t *uq = state->shared_queue;
      bool can_advance = false;
      char next_path[WEBUI_MUSIC_PATH_MAX] = { 0 };

      /* Hold queue_mutex across the full computation (hierarchy: queue → state) */
      pthread_mutex_lock(&uq->queue_mutex);
      int q_len = uq->queue_length;
      bool q_shuffle = uq->shuffle;
      music_repeat_mode_t q_repeat = uq->repeat_mode;

      pthread_mutex_lock(&state->state_mutex);
      if (q_len > 0) {
         if (q_shuffle && q_len > 1) {
            state->queue_index = webui_music_pick_random_index(state->queue_index, q_len,
                                                               &state->shuffle_seed);
            can_advance = true;
         } else if (state->queue_index < q_len - 1) {
            state->queue_index++;
            can_advance = true;
         } else if (q_repeat == MUSIC_REPEAT_ALL) {
            state->queue_index = 0;
            can_advance = true;
         }
      }
      if (can_advance && state->queue_index < q_len) {
         snprintf(next_path, sizeof(next_path), "%s", uq->queue[state->queue_index].path);
      }
      pthread_mutex_unlock(&state->state_mutex);
      pthread_mutex_unlock(&uq->queue_mutex);

      if (can_advance && next_path[0]) {
         webui_music_start_playback(state, next_path);
      }

      webui_music_send_state(conn, state);

   } else if (strcmp(action, "previous") == 0) {
      user_music_queue_t *uq = state->shared_queue;
      bool can_go_back = false;
      char prev_path[WEBUI_MUSIC_PATH_MAX] = { 0 };

      /* Hold queue_mutex across the full computation (hierarchy: queue → state) */
      pthread_mutex_lock(&uq->queue_mutex);
      int q_len = uq->queue_length;
      bool q_shuffle = uq->shuffle;
      music_repeat_mode_t q_repeat = uq->repeat_mode;

      pthread_mutex_lock(&state->state_mutex);
      if (q_len > 0) {
         if (q_shuffle && q_len > 1) {
            state->queue_index = webui_music_pick_random_index(state->queue_index, q_len,
                                                               &state->shuffle_seed);
            can_go_back = true;
         } else if (state->queue_index > 0) {
            state->queue_index--;
            can_go_back = true;
         } else if (q_repeat == MUSIC_REPEAT_ALL) {
            state->queue_index = q_len - 1;
            can_go_back = true;
         }
      }
      if (can_go_back && state->queue_index < q_len) {
         snprintf(prev_path, sizeof(prev_path), "%s", uq->queue[state->queue_index].path);
      }
      pthread_mutex_unlock(&state->state_mutex);
      pthread_mutex_unlock(&uq->queue_mutex);

      if (can_go_back && prev_path[0]) {
         webui_music_start_playback(state, prev_path);
      }

      webui_music_send_state(conn, state);

   } else if (strcmp(action, "toggle_shuffle") == 0) {
      user_music_queue_t *uq = state->shared_queue;
      pthread_mutex_lock(&uq->queue_mutex);
      uq->shuffle = !uq->shuffle;
      uq->generation++;
      music_queue_db_save_state(uq->user_id, uq);
      pthread_mutex_unlock(&uq->queue_mutex);
      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(uq, conn);

   } else if (strcmp(action, "cycle_repeat") == 0) {
      user_music_queue_t *uq = state->shared_queue;
      pthread_mutex_lock(&uq->queue_mutex);
      uq->repeat_mode = (music_repeat_mode_t)((uq->repeat_mode + 1) % 3);
      uq->generation++;
      music_queue_db_save_state(uq->user_id, uq);
      pthread_mutex_unlock(&uq->queue_mutex);
      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(uq, conn);

   } else if (strcmp(action, "seek") == 0) {
      struct json_object *position_obj;
      if (json_object_object_get_ex(payload, "position_sec", &position_obj)) {
         double position_sec = json_object_get_double(position_obj);

         /* Stop streaming thread to safely access decoder
          * This prevents race conditions where thread reads while we seek */
         bool was_streaming = atomic_load(&state->streaming);
         if (was_streaming) {
            webui_music_stop_streaming(state);
         }

         pthread_mutex_lock(&state->state_mutex);
         if (state->decoder && state->source_rate > 0) {
            uint64_t sample_pos = (uint64_t)(position_sec * state->source_rate);
            if (audio_decoder_seek(state->decoder, sample_pos) == 0) {
               state->position_frames = sample_pos;
               /* Clear accumulation buffer after seek */
               state->resample_accum_count = 0;
            }
         }
         pthread_mutex_unlock(&state->state_mutex);
         webui_music_send_state(conn, state);

         /* Restart streaming if it was active */
         if (was_streaming && state->playing) {
            webui_music_start_streaming(state);
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

      user_music_queue_t *uq = state->shared_queue;
      char play_path[WEBUI_MUSIC_PATH_MAX] = { 0 };
      pthread_mutex_lock(&uq->queue_mutex);
      if (play_index < 0 || play_index >= uq->queue_length) {
         pthread_mutex_unlock(&uq->queue_mutex);
         webui_music_send_error(conn, "INVALID_INDEX", "Index out of range");
         return;
      }
      snprintf(play_path, sizeof(play_path), "%s", uq->queue[play_index].path);
      pthread_mutex_unlock(&uq->queue_mutex);

      pthread_mutex_lock(&state->state_mutex);
      state->queue_index = play_index;
      pthread_mutex_unlock(&state->state_mutex);

      if (webui_music_start_playback(state, play_path) != 0) {
         webui_music_send_error(conn, "PLAYBACK_ERROR", "Failed to start playback");
         return;
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

      if (!webui_music_is_path_valid(path)) {
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

      user_music_queue_t *uq = state->shared_queue;
      pthread_mutex_lock(&uq->queue_mutex);
      if (uq->queue_length < WEBUI_MUSIC_MAX_QUEUE) {
         music_queue_entry_t *entry = &uq->queue[uq->queue_length];
         safe_strncpy(entry->path, track_info.path, sizeof(entry->path));
         safe_strncpy(entry->title, track_info.title, sizeof(entry->title));
         safe_strncpy(entry->artist, track_info.artist, sizeof(entry->artist));
         safe_strncpy(entry->album, track_info.album, sizeof(entry->album));
         entry->duration_sec = track_info.duration_sec;
         uq->queue_length++;
      }
      uq->generation++;
      music_queue_db_save(uq->user_id, uq);
      pthread_mutex_unlock(&uq->queue_mutex);
      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(uq, conn);

   } else if (strcmp(action, "remove_from_queue") == 0) {
      /* Remove track from queue by index */
      struct json_object *index_obj;
      if (!json_object_object_get_ex(payload, "index", &index_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing index");
         return;
      }
      int remove_index = json_object_get_int(index_obj);

      user_music_queue_t *uq = state->shared_queue;

      /* Snapshot per-session state under state_mutex first (lock hierarchy) */
      pthread_mutex_lock(&state->state_mutex);
      int my_index = state->queue_index;
      bool was_playing = state->playing && !state->paused;
      pthread_mutex_unlock(&state->state_mutex);

      bool removing_current = (remove_index == my_index);

      /* Stop streaming before modifying queue to prevent use-after-free */
      if (removing_current) {
         webui_music_stop_streaming(state);
      }

      pthread_mutex_lock(&uq->queue_mutex);
      if (remove_index < 0 || remove_index >= uq->queue_length) {
         pthread_mutex_unlock(&uq->queue_mutex);
         webui_music_send_error(conn, "INVALID_INDEX", "Index out of range");
         return;
      }

      /* Shift queue entries after the removed index */
      if (remove_index < uq->queue_length - 1) {
         memmove(&uq->queue[remove_index], &uq->queue[remove_index + 1],
                 (uq->queue_length - remove_index - 1) * sizeof(music_queue_entry_t));
      }
      uq->queue_length--;
      uq->generation++;

      /* Adjust THIS session's index under state_mutex (hierarchy: queue → state) */
      pthread_mutex_lock(&state->state_mutex);
      if (remove_index < state->queue_index) {
         state->queue_index--;
      } else if (removing_current) {
         if (state->decoder) {
            audio_decoder_close(state->decoder);
            state->decoder = NULL;
         }
         if (uq->queue_length > 0) {
            if (state->queue_index >= uq->queue_length) {
               state->queue_index = uq->queue_length - 1;
            }
         } else {
            state->playing = false;
            state->paused = false;
            state->queue_index = 0;
         }
      }
      /* Re-read for auto-play path */
      bool should_autoplay = removing_current && was_playing && uq->queue_length > 0;
      char next_path[WEBUI_MUSIC_PATH_MAX] = { 0 };
      if (should_autoplay && state->queue_index < uq->queue_length) {
         snprintf(next_path, sizeof(next_path), "%s", uq->queue[state->queue_index].path);
      }
      pthread_mutex_unlock(&state->state_mutex);

      music_queue_db_save(uq->user_id, uq);
      pthread_mutex_unlock(&uq->queue_mutex);

      /* Adjust sibling sessions' queue_index */
      remove_adjust_ctx_t rctx = { .removed_index = remove_index, .exclude = conn };
      webui_for_each_conn_by_user(uq->user_id, adjust_index_on_remove, &rctx);

      /* Auto-play next track if we were playing the removed one */
      if (should_autoplay && next_path[0]) {
         webui_music_start_playback(state, next_path);
      }

      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(uq, conn);

   } else if (strcmp(action, "clear_queue") == 0) {
      /* Clear entire queue and stop playback */
      webui_music_stop_streaming(state);

      user_music_queue_t *uq = state->shared_queue;
      pthread_mutex_lock(&uq->queue_mutex);
      uq->queue_length = 0;
      uq->generation++;
      music_queue_db_save(uq->user_id, uq);
      pthread_mutex_unlock(&uq->queue_mutex);

      pthread_mutex_lock(&state->state_mutex);
      if (state->decoder) {
         audio_decoder_close(state->decoder);
         state->decoder = NULL;
      }
      state->playing = false;
      state->paused = false;
      state->queue_index = 0;
      state->position_frames = 0;
      pthread_mutex_unlock(&state->state_mutex);

      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(uq, conn);

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

      user_music_queue_t *uq = state->shared_queue;
      pthread_mutex_lock(&uq->queue_mutex);
      int added = 0;
      for (int i = 0; i < count && uq->queue_length < WEBUI_MUSIC_MAX_QUEUE; i++) {
         music_queue_entry_t *entry = &uq->queue[uq->queue_length];
         safe_strncpy(entry->path, tracks[i].path, sizeof(entry->path));
         safe_strncpy(entry->title, tracks[i].title, sizeof(entry->title));
         safe_strncpy(entry->artist, tracks[i].artist, sizeof(entry->artist));
         safe_strncpy(entry->album, tracks[i].album, sizeof(entry->album));
         entry->duration_sec = tracks[i].duration_sec;
         uq->queue_length++;
         added++;
      }
      uq->generation++;
      music_queue_db_save(uq->user_id, uq);
      pthread_mutex_unlock(&uq->queue_mutex);

      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(uq, conn);

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

      user_music_queue_t *uq = state->shared_queue;
      pthread_mutex_lock(&uq->queue_mutex);
      int added = 0;
      for (int i = 0; i < count && uq->queue_length < WEBUI_MUSIC_MAX_QUEUE; i++) {
         music_queue_entry_t *entry = &uq->queue[uq->queue_length];
         safe_strncpy(entry->path, tracks[i].path, sizeof(entry->path));
         safe_strncpy(entry->title, tracks[i].title, sizeof(entry->title));
         safe_strncpy(entry->artist, tracks[i].artist, sizeof(entry->artist));
         safe_strncpy(entry->album, tracks[i].album, sizeof(entry->album));
         entry->duration_sec = tracks[i].duration_sec;
         uq->queue_length++;
         added++;
      }
      uq->generation++;
      music_queue_db_save(uq->user_id, uq);
      pthread_mutex_unlock(&uq->queue_mutex);

      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(uq, conn);

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
      if (limit <= 0 || limit > WEBUI_MUSIC_MAX_QUEUE) {
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
   music_db_search(query, results, limit, &count);

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

/* =============================================================================
 * Shared queue-mutation helpers (transport-free) — see webui_music_internal.h.
 * Called by both handle_music_queue() (LWS thread) and webui_music_execute_tool()
 * (LLM worker thread). They mutate + persist + bump generation only; the caller
 * does transport.
 * ============================================================================= */

int webui_music_queue_apply_paths(session_music_state_t *state,
                                  const char *const *paths,
                                  int n,
                                  bool replace,
                                  queue_apply_outcome_t *outcomes) {
   if (!state || !state->shared_queue || !paths || n < 0) {
      return FAILURE;
   }

   /* Resolve metadata BEFORE taking the queue lock (DB lookups are slow and the
    * queue_mutex is on the playback hot path). */
   music_search_result_t *resolved = NULL;
   bool *ok = NULL;
   if (n > 0) {
      resolved = calloc((size_t)n, sizeof(*resolved));
      ok = calloc((size_t)n, sizeof(*ok));
      if (!resolved || !ok) {
         free(resolved);
         free(ok);
         return FAILURE;
      }
   }
   for (int i = 0; i < n; i++) {
      bool found = false;
      if (paths[i] && webui_music_is_path_valid(paths[i]) &&
          music_db_get_by_path(paths[i], &resolved[i], &found) == SUCCESS && found) {
         ok[i] = true;
      } else if (outcomes) {
         outcomes[i] = QUEUE_APPLY_NOT_FOUND;
      }
   }

   user_music_queue_t *uq = state->shared_queue;
   pthread_mutex_lock(&uq->queue_mutex);
   if (replace) {
      uq->queue_length = 0;
   }

   for (int i = 0; i < n; i++) {
      if (!ok[i]) {
         continue; /* outcome already set to NOT_FOUND */
      }

      /* Dedup against current queue contents (also catches within-batch dupes). */
      bool dup = false;
      for (int j = 0; j < uq->queue_length; j++) {
         if (strcmp(uq->queue[j].path, resolved[i].path) == 0) {
            dup = true;
            break;
         }
      }
      if (dup) {
         if (outcomes) {
            outcomes[i] = QUEUE_APPLY_DUPLICATE;
         }
         continue;
      }
      if (uq->queue_length >= WEBUI_MUSIC_MAX_QUEUE) {
         if (outcomes) {
            outcomes[i] = QUEUE_APPLY_FULL;
         }
         continue;
      }

      music_queue_entry_t *entry = &uq->queue[uq->queue_length];
      safe_strncpy(entry->path, resolved[i].path, sizeof(entry->path));
      safe_strncpy(entry->title, resolved[i].title, sizeof(entry->title));
      safe_strncpy(entry->artist, resolved[i].artist, sizeof(entry->artist));
      safe_strncpy(entry->album, resolved[i].album, sizeof(entry->album));
      entry->duration_sec = resolved[i].duration_sec;
      uq->queue_length++;
      if (outcomes) {
         outcomes[i] = QUEUE_APPLY_ADDED;
      }
   }
   uq->generation++;
   music_queue_db_save(uq->user_id, uq);
   pthread_mutex_unlock(&uq->queue_mutex);

   free(resolved);
   free(ok);
   return SUCCESS;
}

int webui_music_queue_remove_index(session_music_state_t *state, int index0) {
   if (!state || !state->shared_queue) {
      return FAILURE;
   }
   user_music_queue_t *uq = state->shared_queue;

   int rc = FAILURE;
   pthread_mutex_lock(&uq->queue_mutex);
   if (index0 >= 0 && index0 < uq->queue_length) {
      for (int i = index0; i < uq->queue_length - 1; i++) {
         uq->queue[i] = uq->queue[i + 1];
      }
      uq->queue_length--;
      uq->generation++;

      /* Adjust this session's index under state_mutex (queue -> state order) */
      pthread_mutex_lock(&state->state_mutex);
      if (state->queue_index > index0) {
         state->queue_index--;
      } else if (state->queue_index >= uq->queue_length) {
         state->queue_index = uq->queue_length > 0 ? uq->queue_length - 1 : 0;
      }
      pthread_mutex_unlock(&state->state_mutex);

      music_queue_db_save(uq->user_id, uq);
      rc = SUCCESS;
   }
   pthread_mutex_unlock(&uq->queue_mutex);

   /* Adjust sibling sessions' queue_index (after releasing queue_mutex). */
   if (rc == SUCCESS) {
      remove_adjust_ctx_t rctx = { .removed_index = index0, .exclude = state->conn };
      webui_for_each_conn_by_user(uq->user_id, adjust_index_on_remove, &rctx);
   }
   return rc;
}

int webui_music_queue_clear(session_music_state_t *state, int *removed_out) {
   if (!state || !state->shared_queue) {
      return FAILURE;
   }
   webui_music_stop_streaming(state);

   user_music_queue_t *uq = state->shared_queue;
   pthread_mutex_lock(&uq->queue_mutex);
   if (removed_out) {
      *removed_out = uq->queue_length;
   }
   uq->queue_length = 0;
   uq->generation++;
   music_queue_db_save(uq->user_id, uq);
   pthread_mutex_unlock(&uq->queue_mutex);

   pthread_mutex_lock(&state->state_mutex);
   state->queue_index = 0;
   state->playing = false;
   state->position_frames = 0;
   if (state->decoder) {
      audio_decoder_close(state->decoder);
      state->decoder = NULL;
   }
   pthread_mutex_unlock(&state->state_mutex);
   return SUCCESS;
}

void handle_music_queue(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_is_satellite_session(conn) && !conn_require_auth(conn)) {
      return;
   }

   session_music_state_t *state = (session_music_state_t *)conn->music_state;
   if (!state) {
      webui_music_send_error(conn, "NOT_INITIALIZED", "Music session not initialized");
      return;
   }

   struct json_object *action_obj;
   if (!json_object_object_get_ex(payload, "action", &action_obj)) {
      webui_music_send_error(conn, "INVALID_REQUEST", "Missing action");
      return;
   }

   const char *action = json_object_get_string(action_obj);

   if (strcmp(action, "list") == 0) {
      /* Return current queue */
      struct json_object *response = json_object_new_object();
      json_object_object_add(response, "type", json_object_new_string("music_queue_response"));

      struct json_object *resp_payload = json_object_new_object();
      user_music_queue_t *uq = state->shared_queue;

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
      json_object_object_add(resp_payload, "current_index",
                             json_object_new_int(state->queue_index));
      json_object_object_add(resp_payload, "length", json_object_new_int(uq->queue_length));

      pthread_mutex_unlock(&uq->queue_mutex);

      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);

   } else if (strcmp(action, "clear") == 0) {
      webui_music_queue_clear(state, NULL);
      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(state->shared_queue, conn);

   } else if (strcmp(action, "add") == 0) {
      struct json_object *path_obj;
      if (!json_object_object_get_ex(payload, "path", &path_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing path");
         return;
      }

      const char *path = json_object_get_string(path_obj);
      queue_apply_outcome_t oc = QUEUE_APPLY_NOT_FOUND;
      webui_music_queue_apply_paths(state, &path, 1, false, &oc);
      if (oc == QUEUE_APPLY_NOT_FOUND) {
         webui_music_send_error(conn, "NOT_FOUND", "Track not found in music library");
         return;
      }

      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(state->shared_queue, conn);

   } else if (strcmp(action, "remove") == 0) {
      struct json_object *index_obj;
      if (!json_object_object_get_ex(payload, "index", &index_obj)) {
         webui_music_send_error(conn, "INVALID_REQUEST", "Missing index");
         return;
      }

      int index = json_object_get_int(index_obj);
      if (webui_music_queue_remove_index(state, index) != SUCCESS) {
         webui_music_send_error(conn, "INVALID_INDEX", "Queue index out of range");
         return;
      }

      webui_music_send_state(conn, state);
      webui_music_broadcast_queue_state(state->shared_queue, conn);

   } else {
      webui_music_send_error(conn, "UNKNOWN_ACTION", "Unknown queue action");
   }
}
