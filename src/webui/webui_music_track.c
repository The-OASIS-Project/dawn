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
 * the project author(s).
 *
 * WebUI music: opening tracks.  The one place a queue path becomes a playing
 * decoder: path and format checks, the Plex download, installing the decoder,
 * starting a track on request and moving to the next one at the end of a
 * track (skipping ones that won't play).
 */

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "audio/audio_decoder.h"
#include "audio/music_source.h"
#include "audio/plex_client.h"
#include "audio/resampler.h"
#include "dawn_error.h"
#include "logging.h"
#include "webui/webui_internal.h"
#include "webui/webui_music.h"
#include "webui/webui_music_internal.h"

/**
 * @brief Open a queue path for decoding
 *
 * Every track the stream plays goes through here, so it is the one place a
 * path is checked against the library and a decoder against what the stream
 * can handle.  A plex: path is downloaded to a temp file first; the file is
 * unlinked once the decoder has it open (the decoder keeps its descriptor), so
 * nothing is left on disk.  Blocks for the whole download: call without
 * state_mutex.
 *
 * @param path Queue path (local file or plex:/library/...)
 * @param cancel Aborts a Plex download once set (NULL = not cancellable)
 * @param unreachable Optional output: set when the Plex server couldn't be
 *        reached, so the next Plex track would fail the same way
 * @return Decoder, or NULL if the path, the download or the open failed
 */
static audio_decoder_t *music_open_track(const char *path,
                                         const atomic_bool *cancel,
                                         bool *unreachable) {
   if (unreachable) {
      *unreachable = false;
   }
   if (!webui_music_is_path_valid(path)) {
      OLOG_WARNING("WebUI music: Refusing to open %s (not in the music library)", path);
      return NULL;
   }

   audio_decoder_t *decoder = NULL;
   if (music_source_from_path(path) == MUSIC_SOURCE_PLEX) {
      char local_path[PATH_MAX];
      const char *part_key = path + strlen(music_source_path_prefix(MUSIC_SOURCE_PLEX));
      int rc = plex_client_download_track(part_key, cancel, local_path, sizeof(local_path));
      if (rc != SUCCESS) {
         OLOG_ERROR("WebUI music: Plex download failed for: %s", path);
         if (unreachable && rc == PLEX_DOWNLOAD_UNREACHABLE) {
            *unreachable = true;
         }
         return NULL;
      }
      decoder = audio_decoder_open(local_path);
      unlink(local_path);
   } else {
      decoder = audio_decoder_open(path);
   }
   if (!decoder) {
      return NULL;
   }

   /* Below the minimum one Opus frame needs more source frames than a read
    * buffer holds; decoders hand out mono or stereo only. */
   audio_decoder_info_t info;
   audio_decoder_get_info(decoder, &info);
   if (info.sample_rate < WEBUI_MUSIC_MIN_SOURCE_RATE ||
       info.sample_rate > WEBUI_MUSIC_MAX_SOURCE_RATE || info.channels < 1 || info.channels > 2) {
      OLOG_ERROR("WebUI music: Can't stream %s (%u Hz, %u channels)", path, info.sample_rate,
                 info.channels);
      audio_decoder_close(decoder);
      return NULL;
   }
   return decoder;
}

/**
 * @brief Tell the client a Plex track is downloading before it can play
 *
 * Sends music_state with loading:true and the track at the session's queue
 * index (position 0).  Only for plex: paths; a local open is immediate.  The
 * caller clears state->loading once the open returns.
 *
 * @param advance_auto Tag the frame as part of a natural advance
 */
static void music_announce_loading(ws_connection_t *conn,
                                   session_music_state_t *state,
                                   const char *path,
                                   bool advance_auto) {
   if (music_source_from_path(path) != MUSIC_SOURCE_PLEX) {
      return;
   }
   pthread_mutex_lock(&state->state_mutex);
   state->loading = true;
   state->position_frames = 0;
   pthread_mutex_unlock(&state->state_mutex);
   webui_music_send_state_impl(conn, state, advance_auto);
}

/**
 * @brief Install a freshly opened decoder (state_mutex held)
 *
 * The resampler always runs stereo: the stream thread expands mono before
 * resampling.
 *
 * @return SUCCESS, or FAILURE if no resampler could be made (decoder closed)
 */
static int music_install_decoder(session_music_state_t *state, audio_decoder_t *decoder) {
   audio_decoder_info_t info;
   audio_decoder_get_info(decoder, &info);

   if (state->resampler) {
      resampler_destroy(state->resampler);
      state->resampler = NULL;
   }
   if (info.sample_rate != OPUS_SAMPLE_RATE) {
      state->resampler = resampler_create(info.sample_rate, OPUS_SAMPLE_RATE, 2);
      if (!state->resampler) {
         OLOG_ERROR("WebUI music: Failed to create resampler");
         audio_decoder_close(decoder);
         return FAILURE;
      }
   }

   state->decoder = decoder;
   state->advance_opening = false;
   state->source_rate = info.sample_rate;
   state->source_channels = info.channels;
   state->source_format = info.format;
   state->position_frames = 0;
   state->resample_accum_count = 0;
   return SUCCESS;
}

/**
 * @brief Move to the next track after the current one ended
 *
 * A track that won't open is skipped.  Playback stops once as many tracks in a
 * row have produced no audio as the queue holds, or at once under repeat-one
 * after a failure, so a queue of unplayable tracks can't spin.
 *
 * @param state Session music state (decoder already closed and NULL)
 * @param conn Connection to report to
 * @param track_failed The track that just ended did so on a decode error
 * @param failures In/out: tracks in a row that produced no audio
 * @return What happened
 */
music_advance_t webui_music_advance(session_music_state_t *state,
                                    ws_connection_t *conn,
                                    bool track_failed,
                                    int *failures) {
   user_music_queue_t *uq = state->shared_queue;

   for (;;) {
      /* Pick the next track.  Lock order: queue, then state. */
      pthread_mutex_lock(&uq->queue_mutex);
      pthread_mutex_lock(&state->state_mutex);
      if (atomic_load(&state->stop_requested)) {
         /* Whoever stopped us decides what plays next; until then nothing is */
         state->playing = false;
         pthread_mutex_unlock(&state->state_mutex);
         pthread_mutex_unlock(&uq->queue_mutex);
         return MUSIC_ADVANCE_EXIT;
      }

      int q_len = uq->queue_length;
      int next_index = state->queue_index;
      bool repeat_one = uq->repeat_mode == MUSIC_REPEAT_ONE;
      bool stop = false;
      bool end_of_queue = false;
      const char *stop_error = NULL;

      if (q_len == 0) {
         stop = true;
         end_of_queue = true;
      } else if (track_failed && repeat_one) {
         stop = true; /* replaying it would fail the same way */
      } else if (*failures >= q_len) {
         stop = true;
         stop_error = "None of the queued tracks would play";
      } else if (*failures >= WEBUI_MUSIC_MAX_FAILED_TRACKS) {
         stop = true;
         stop_error = "Stopped: several tracks in a row wouldn't play";
      } else if (repeat_one) {
         /* Repeat one: replay the same track */
      } else if (uq->shuffle && *failures > 0) {
         /* Step past a failure in order: random picks can repeat, so a few
          * playable tracks could go untried before the cap is reached */
         next_index = (next_index + 1) % q_len;
      } else if (uq->shuffle) {
         if (q_len > 1) {
            next_index = webui_music_pick_random_index(next_index, q_len, &state->shuffle_seed);
         }
      } else if (++next_index >= q_len) {
         if (uq->repeat_mode == MUSIC_REPEAT_ALL) {
            next_index = 0;
         } else {
            stop = true;
            end_of_queue = true;
         }
      }

      if (stop) {
         state->playing = false;
         if (end_of_queue) {
            state->queue_index = 0;
            state->position_frames = 0; /* nothing is loaded; don't report the old end */
         }
         pthread_mutex_unlock(&state->state_mutex);
         pthread_mutex_unlock(&uq->queue_mutex);
         if (stop_error) {
            webui_music_send_error(conn, "DECODE_ERROR", stop_error);
         }
         /* Tagged like a natural advance: the client still holds the ended
          * track's last ~2 s and plays them out before showing the stop */
         webui_music_send_state_impl(conn, state, true);
         return MUSIC_ADVANCE_STOPPED;
      }

      if (next_index >= q_len) {
         next_index = 0; /* the queue shrank under a repeat-one or shuffle index */
      }
      char next_path[WEBUI_MUSIC_PATH_MAX];
      snprintf(next_path, sizeof(next_path), "%s", uq->queue[next_index].path);
      /* A mixed queue keeps playing its local tracks if Plex goes down */
      bool queue_has_local = false;
      for (int i = 0; i < q_len && !queue_has_local; i++) {
         queue_has_local = music_source_from_path(uq->queue[i].path) != MUSIC_SOURCE_PLEX;
      }
      state->queue_index = next_index;
      state->cached_duration_sec = uq->queue[next_index].duration_sec;
      state->advance_opening = true;
      pthread_mutex_unlock(&state->state_mutex);
      pthread_mutex_unlock(&uq->queue_mutex);

      /* Open without the lock: a Plex track is downloaded first.  A control
       * request that stops this thread sets stop_requested, which cancels the
       * download, so its join waits about a second at most.  Every path that
       * replaces the decoder stops this thread first, so nothing else installs
       * one meanwhile. */
      music_announce_loading(conn, state, next_path, true);
      bool unreachable = false;
      audio_decoder_t *next_decoder = music_open_track(next_path, &state->stop_requested,
                                                       &unreachable);

      pthread_mutex_lock(&state->state_mutex);
      state->loading = false;
      if (atomic_load(&state->stop_requested)) {
         /* Whoever stopped us decides what plays next; until then nothing is */
         state->playing = false;
         state->position_frames = 0;
         pthread_mutex_unlock(&state->state_mutex);
         if (next_decoder) {
            audio_decoder_close(next_decoder);
         }
         return MUSIC_ADVANCE_EXIT;
      }
      if (unreachable && !queue_has_local) {
         /* Every track is on Plex and would wait out the same connect timeout */
         state->advance_opening = false;
         state->playing = false;
         pthread_mutex_unlock(&state->state_mutex);
         webui_music_send_error(conn, "PLEX_UNREACHABLE", "The Plex server isn't reachable");
         webui_music_send_state_impl(conn, state, true); /* see the stop above */
         return MUSIC_ADVANCE_STOPPED;
      }
      if (!next_decoder || music_install_decoder(state, next_decoder) != SUCCESS) {
         state->advance_opening = false;
         pthread_mutex_unlock(&state->state_mutex);
         OLOG_WARNING("WebUI music: Couldn't open %s%s", next_path,
                      repeat_one ? "" : ", skipping it");
         (*failures)++;
         track_failed = true;
         if (*failures == 1) {
            webui_music_send_error(conn, "DECODE_ERROR",
                                   repeat_one ? "Couldn't play this track"
                                              : "Couldn't play a track, skipping it");
         }
         continue;
      }
      pthread_mutex_unlock(&state->state_mutex);

      /* Tag as a natural auto-advance so the client keeps its buffered lead
       * (gapless) instead of flushing. */
      webui_music_send_state_impl(conn, state, true);
      return MUSIC_ADVANCE_PLAYING;
   }
}

/**
 * @brief Start playing a track
 *
 * IMPORTANT: Stops any existing streaming thread before changing decoder/resampler.
 * This prevents race conditions where the old thread uses freed resources.
 */
int webui_music_start_playback(session_music_state_t *state, const char *path) {
   /* Stop existing streaming thread first - this is critical to prevent crashes
    * where the streaming thread accesses freed decoder/resampler */
   webui_music_stop_streaming(state);

   /* Blocks the caller for a Plex download.  On the WebSocket thread that's
    * acceptable on a home LAN; on another thread (the LLM tool) a WebSocket
    * request waiting for control_mutex cancels it via control_cancel. */
   music_announce_loading(state->conn, state, path, false);
   audio_decoder_t *decoder = music_open_track(path, &state->control_cancel, NULL);

   pthread_mutex_lock(&state->state_mutex);
   state->loading = false;

   /* Close any existing decoder */
   if (state->decoder) {
      audio_decoder_close(state->decoder);
      state->decoder = NULL;
   }

   if (!decoder || music_install_decoder(state, decoder) != SUCCESS) {
      /* Nothing is playing now; callers report this state to the client */
      state->playing = false;
      state->paused = false;
      pthread_mutex_unlock(&state->state_mutex);
      OLOG_ERROR("WebUI music: Failed to open: %s", path);
      return 1;
   }

   OLOG_INFO("WebUI music: Playing %s (%s %u Hz, %u ch)", path,
             audio_decoder_format_name(state->source_format), state->source_rate,
             state->source_channels);

   state->playing = true;
   state->paused = false;
   int qidx = state->queue_index;
   user_music_queue_t *uq = state->shared_queue;
   pthread_mutex_unlock(&state->state_mutex);

   /* Cache the track duration for the 1 Hz position updates. Do NOT hold
    * state_mutex while taking queue_mutex — the documented lock order is
    * queue_mutex → state_mutex, so holding state here and then taking queue
    * (state → queue) inverts it and can deadlock against paths that correctly
    * nest queue → state (e.g. next/previous). Snapshot the index under state
    * above, read the duration under queue alone, then store it under state. */
   if (uq) {
      bool have_dur = false;
      uint32_t dur = 0;
      pthread_mutex_lock(&uq->queue_mutex);
      if (qidx >= 0 && qidx < uq->queue_length) {
         dur = uq->queue[qidx].duration_sec;
         have_dur = true;
      }
      pthread_mutex_unlock(&uq->queue_mutex);
      if (have_dur) {
         pthread_mutex_lock(&state->state_mutex);
         state->cached_duration_sec = dur;
         pthread_mutex_unlock(&state->state_mutex);
      }
   }

   /* Start streaming thread if not already running */
   return webui_music_start_streaming(state);
}
