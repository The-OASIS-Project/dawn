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

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio/audio_decoder.h"
#include "audio/music_open.h"
#include "audio/music_source.h"
#include "audio/resampler.h"
#include "dawn_error.h"
#include "logging.h"
#include "webui/webui_internal.h"
#include "webui/webui_music.h"
#include "webui/webui_music_internal.h"

/**
 * @brief Open a queue path for the stream
 *
 * music_open_track() (library check, Plex download), then the formats the
 * stream can encode.  Blocks for the whole download: call without state_mutex.
 *
 * @param cancel Aborts a Plex download once set
 * @param unreachable Optional output: the Plex server couldn't be reached
 * @return Decoder, or NULL if it can't be opened or streamed
 */
static audio_decoder_t *stream_open_track(const char *path,
                                          const atomic_bool *cancel,
                                          bool *unreachable) {
   audio_decoder_t *decoder = music_open_track(path, cancel, unreachable);
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
   state->seek_pending = false;
   state->source_rate = info.sample_rate;
   state->source_channels = info.channels;
   state->source_format = info.format;
   state->position_frames = 0;
   state->resample_accum_count = 0;
   return SUCCESS;
}

/* ---- Plex prefetch ------------------------------------------------------ */

/* ---- Play history (queue_mutex, then state_mutex, held) ----------------- */

/* A history entry's queue position: the copy of path nearest its hint */
static int queue_find(void *ctx, const char *path, int hint) {
   return music_queue_find((const user_music_queue_t *)ctx, path, hint);
}

/* Another tab replaced the queue since this session picked its track */
static bool off_queue(const session_music_state_t *state, const user_music_queue_t *uq) {
   return state->track.path[0] != '\0' && state->track_epoch != uq->replace_epoch;
}

/* Note the track this session picked: the queue entry at index, or where
 * else the queue holds path; a path it doesn't hold (another tab replaced the
 * queue since the pick) is noted as off the queue */
static void set_track(session_music_state_t *state,
                      user_music_queue_t *uq,
                      int index,
                      const char *path) {
   state->track_epoch = uq->replace_epoch;
   int found = queue_find(uq, path, index);
   if (found >= 0) {
      state->track = uq->queue[found];
      state->queue_index = found;
   } else {
      memset(&state->track, 0, sizeof(state->track));
      snprintf(state->track.path, sizeof(state->track.path), "%s", path);
      state->track_epoch = uq->replace_epoch - 1;
   }
}

/* Where next steps from: queue_index.  After another tab replaced the queue,
 * the track's position in the new queue when it holds it (adopted: the
 * session is back on the queue), else -1, so the new queue's top follows.
 * *held: the queue holds the track (or none is picked). */
static int step_base(session_music_state_t *state, user_music_queue_t *uq, bool *held) {
   *held = true;
   if (off_queue(state, uq)) {
      int found = queue_find(uq, state->track.path, state->queue_index);
      if (found >= 0) {
         state->track_epoch = uq->replace_epoch;
         state->queue_index = found;
         return found;
      }
      *held = false;
      return -1;
   }
   return state->queue_index;
}

int webui_music_current(const session_music_state_t *state,
                        const user_music_queue_t *uq,
                        const music_queue_entry_t **track_out) {
   int index = state->queue_index;
   if (state->track.path[0] && (state->playing || state->paused)) {
      *track_out = &state->track;
      return queue_find((void *)uq, state->track.path, index);
   }
   if (index >= 0 && index < uq->queue_length) {
      *track_out = &uq->queue[index];
      return index;
   }
   *track_out = NULL;
   return -1;
}

/* The session's history, cleared first if the queue was replaced since */
static music_history_t *history_of(session_music_state_t *state, user_music_queue_t *uq) {
   music_history_sync(&state->history, uq->replace_epoch);
   return &state->history;
}

/* Record that queue position index plays, unless it's the history's current
 * entry already (a step through the history, or a replay); true when added */
static bool history_record(session_music_state_t *state, user_music_queue_t *uq, int index) {
   music_history_t *h = history_of(state, uq);
   const char *current = music_history_current(h);
   if (current && strcmp(current, uq->queue[index].path) == 0) {
      return false;
   }
   music_history_push(h, uq->queue[index].path, index);
   return true;
}

/* The next track under shuffle: the history's next entry, else a new random
 * pick other than current (step_base()'s position, -1 for none), recorded
 * (q_len > 1) */
static int shuffle_next(session_music_state_t *state,
                        user_music_queue_t *uq,
                        int q_len,
                        int current) {
   int next = music_history_step(history_of(state, uq), true, true, queue_find, uq);
   if (next < 0) {
      next = webui_music_pick_random_index(current, q_len, &state->shuffle_seed);
      music_history_push(&state->history, uq->queue[next].path, next);
   }
   return next;
}

static void *prefetch_thread(void *arg) {
   session_music_state_t *state = (session_music_state_t *)arg;
   char path[MUSIC_PATH_MAX];
   pthread_mutex_lock(&state->state_mutex);
   snprintf(path, sizeof(path), "%s", state->prefetch_path);
   pthread_mutex_unlock(&state->state_mutex);

   audio_decoder_t *decoder = stream_open_track(path, &state->prefetch_cancel, NULL);

   pthread_mutex_lock(&state->state_mutex);
   bool keep = decoder && !atomic_load(&state->prefetch_cancel) &&
               strcmp(state->prefetch_path, path) == 0;
   if (keep) {
      state->prefetched = decoder;
      decoder = NULL;
   } else if (strcmp(state->prefetch_path, path) == 0) {
      /* Nothing kept: a later plan may fetch it again */
      state->prefetch_path[0] = '\0';
   }
   pthread_mutex_unlock(&state->state_mutex);
   if (decoder) {
      audio_decoder_close(decoder);
   }
   if (keep) {
      OLOG_INFO("WebUI music: Prefetched %s", path);
   }
   return NULL;
}

void webui_music_prefetch_stop(session_music_state_t *state) {
   if (!state->prefetch_running) {
      return;
   }
   atomic_store(&state->prefetch_cancel, true);
   pthread_join(state->prefetch_thread, NULL);
   state->prefetch_running = false;
}

/* Wait for a running helper without cancelling it: its track is the one
 * about to open.  After a stop, cancel it instead, so the stop's join stays
 * short. */
static void prefetch_finish(session_music_state_t *state) {
   if (atomic_load(&state->stop_requested)) {
      webui_music_prefetch_stop(state);
   } else if (state->prefetch_running) {
      pthread_join(state->prefetch_thread, NULL);
      state->prefetch_running = false;
   }
}

/* Drop a prefetch nothing is going to open (stream thread) */
static void prefetch_drop(session_music_state_t *state) {
   webui_music_prefetch_stop(state);
   pthread_mutex_lock(&state->state_mutex);
   if (state->prefetched) {
      audio_decoder_close(state->prefetched);
      state->prefetched = NULL;
   }
   state->prefetch_path[0] = '\0';
   pthread_mutex_unlock(&state->state_mutex);
}

/*
 * After a track starts: plan the one that follows, and start downloading it
 * if it's on Plex.  Repeat-one replays the current track, so nothing is
 * planned.  Prefetch errors are never reported; the real open reports them.
 */
static void prefetch_plan(session_music_state_t *state) {
   user_music_queue_t *uq = state->shared_queue;
   char next_path[MUSIC_PATH_MAX] = { 0 };

   /* A stop must find no helper it didn't cancel */
   if (atomic_load(&state->stop_requested)) {
      return;
   }

   pthread_mutex_lock(&uq->queue_mutex);
   pthread_mutex_lock(&state->state_mutex);
   int q_len = uq->queue_length;
   int next = -1;
   if (q_len > 0 && uq->repeat_mode != MUSIC_REPEAT_ONE) {
      if (uq->shuffle && q_len > 1) {
         /* The history's next entry, else a new pick planned as that entry,
          * so next and auto-advance play the track prefetched here */
         bool held;
         int current = step_base(state, uq, &held); /* after a replace, its new position or none */
         music_history_t *h = history_of(state, uq);
         next = music_history_step(h, true, false, queue_find, uq);
         if (next < 0) {
            next = webui_music_pick_random_index(current, q_len, &state->shuffle_seed);
            music_history_plan(h, uq->queue[next].path, next);
         }
      } else if (uq->shuffle) {
         /* A single track under shuffle: nothing follows */
      } else {
         bool held;
         int base = step_base(state, uq, &held);
         if (base + 1 < q_len) {
            next = base + 1;
         } else if (uq->repeat_mode == MUSIC_REPEAT_ALL) {
            next = 0;
         }
      }
   }
   if (next >= 0) {
      snprintf(next_path, sizeof(next_path), "%s", uq->queue[next].path);
   }
   bool already = next_path[0] && strcmp(state->prefetch_path, next_path) == 0;
   pthread_mutex_unlock(&state->state_mutex);
   pthread_mutex_unlock(&uq->queue_mutex);

   if (already) {
      return;
   }
   /* A different track than any earlier prefetch (or none): drop that one */
   prefetch_drop(state);
   if (!next_path[0] || music_source_from_path(next_path) != MUSIC_SOURCE_PLEX) {
      return;
   }

   pthread_mutex_lock(&state->state_mutex);
   snprintf(state->prefetch_path, sizeof(state->prefetch_path), "%s", next_path);
   pthread_mutex_unlock(&state->state_mutex);

   atomic_store(&state->prefetch_cancel, false);
   if (pthread_create(&state->prefetch_thread, NULL, prefetch_thread, state) == 0) {
      state->prefetch_running = true;
   } else {
      pthread_mutex_lock(&state->state_mutex);
      state->prefetch_path[0] = '\0';
      pthread_mutex_unlock(&state->state_mutex);
   }
}

/* The prefetched decoder for path, if that's what was prefetched; any other
 * prefetch is dropped (stream thread) */
static audio_decoder_t *prefetch_take(session_music_state_t *state, const char *path) {
   pthread_mutex_lock(&state->state_mutex);
   bool same = strcmp(state->prefetch_path, path) == 0;
   pthread_mutex_unlock(&state->state_mutex);
   if (same) {
      prefetch_finish(state);
   } else {
      webui_music_prefetch_stop(state);
   }

   pthread_mutex_lock(&state->state_mutex);
   audio_decoder_t *decoder = NULL;
   if (state->prefetched && same) {
      decoder = state->prefetched;
   } else if (state->prefetched) {
      audio_decoder_close(state->prefetched);
   }
   state->prefetched = NULL;
   state->prefetch_path[0] = '\0';
   pthread_mutex_unlock(&state->state_mutex);
   return decoder;
}

/* Resolve a ticket: record its outcome and wake a waiting tool (state_mutex held) */
static void publish_open(session_music_state_t *state, uint32_t seq, music_open_result_t result) {
   state->open_done_seq = seq;
   if (seq == state->request_seq) {
      state->request_done_seq = seq;
      state->request_result = result;
   }
   pthread_cond_broadcast(&state->open_cond);
}

/* Fill the pending slot (state_mutex held); returns the new ticket */
static uint32_t set_pending(session_music_state_t *state, const char *path, bool is_auto) {
   snprintf(state->pending_path, sizeof(state->pending_path), "%s", path);
   state->pending_auto = is_auto;
   state->pending_seq = ++state->open_seq;
   /* A prefetched track opens at once: no download to show */
   bool prefetched = state->prefetched && strcmp(state->prefetch_path, path) == 0;
   state->loading = music_source_from_path(path) == MUSIC_SOURCE_PLEX && !prefetched;
   return state->pending_seq;
}

/**
 * @brief Pick the next track after the current one ended
 *
 * Playback stops once as many tracks in a row have produced no audio as the
 * queue holds, or at once under repeat-one after a failure, so a queue of
 * unplayable tracks can't spin.  A track that won't open is skipped by
 * webui_music_run_pending_open(), which calls back here.
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
      /* After another tab replaced the queue: from the ended track's position in
       * the new one, or its top when it doesn't hold the track (nothing to replay) */
      bool held;
      int next_index = step_base(state, uq, &held);
      bool repeat_one = uq->repeat_mode == MUSIC_REPEAT_ONE && held;
      bool recorded = false;
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
         next_index = (next_index + 1 + q_len) % q_len;
      } else if (uq->shuffle) {
         if (q_len > 1) {
            next_index = shuffle_next(state, uq, q_len, next_index);
            recorded = true;
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
            state->track.path[0] = '\0';
         }
         pthread_mutex_unlock(&state->state_mutex);
         pthread_mutex_unlock(&uq->queue_mutex);
         /* Nothing follows, so a prefetch (its track since removed) won't be used */
         prefetch_drop(state);
         if (stop_error) {
            webui_music_send_error(conn, "DECODE_ERROR", stop_error);
         }
         /* Tagged like a natural advance: the client still holds the ended
          * track's last ~2 s and plays them out before showing the stop */
         webui_music_send_state_impl(conn, state, true);
         return MUSIC_ADVANCE_STOPPED;
      }

      if (next_index < 0 || next_index >= q_len) {
         /* The queue shrank under a repeat-one or shuffle index, or was replaced
          * and nothing above picked from it (shuffle with one track) */
         next_index = 0;
      }
      /* A replay of the current track adds nothing (history_record checks) */
      state->history_fresh = recorded || history_record(state, uq, next_index);
      state->queue_index = next_index;
      set_track(state, uq, next_index, uq->queue[next_index].path);
      state->cached_duration_sec = uq->queue[next_index].duration_sec;
      state->position_frames = 0;
      set_pending(state, uq->queue[next_index].path, true);
      bool loading = state->loading;
      pthread_mutex_unlock(&state->state_mutex);
      pthread_mutex_unlock(&uq->queue_mutex);

      /* A Plex track downloads first: tell the client (tagged, so it applies
       * once the ended track's buffered audio has played) */
      if (loading) {
         webui_music_send_state_impl(conn, state, true);
      }
      return MUSIC_ADVANCE_PENDING;
   }
}

bool webui_music_run_pending_open(session_music_state_t *state,
                                  ws_connection_t *conn,
                                  int *failures) {
   char path[MUSIC_PATH_MAX];
   pthread_mutex_lock(&state->state_mutex);
   if (!state->pending_path[0]) {
      pthread_mutex_unlock(&state->state_mutex);
      return false;
   }
   snprintf(path, sizeof(path), "%s", state->pending_path);
   bool is_auto = state->pending_auto;
   uint32_t seq = state->pending_seq;
   pthread_mutex_unlock(&state->state_mutex);

   /* Open without the lock: a Plex track is downloaded first.  A request that
    * stops this thread sets stop_requested, which cancels the download, so its
    * join waits about a second at most.  Every path that replaces the decoder
    * stops this thread first, so nothing else installs one meanwhile. */
   bool unreachable = false;
   audio_decoder_t *decoder = prefetch_take(state, path);
   if (!decoder) {
      decoder = stream_open_track(path, &state->stop_requested, &unreachable);
   }

   pthread_mutex_lock(&state->state_mutex);
   if (atomic_load(&state->stop_requested)) {
      /* Leave the slot for whoever stopped us: it replaces or drops it */
      pthread_mutex_unlock(&state->state_mutex);
      if (decoder) {
         audio_decoder_close(decoder);
      }
      return true;
   }

   state->pending_path[0] = '\0';
   state->loading = false;
   if (decoder && music_install_decoder(state, decoder) == SUCCESS) {
      publish_open(state, seq, MUSIC_OPEN_PLAYING);
      OLOG_INFO("WebUI music: Playing %s (%s %u Hz, %u ch)", path,
                audio_decoder_format_name(state->source_format), state->source_rate,
                state->source_channels);
      pthread_mutex_unlock(&state->state_mutex);
      /* An auto-advance stays tagged so the client keeps its buffered lead */
      webui_music_send_state_impl(conn, state, is_auto);
      prefetch_plan(state);
      return true;
   }
   publish_open(state, seq, unreachable ? MUSIC_OPEN_UNREACHABLE : MUSIC_OPEN_FAILED);
   /* An auto-advance pick that didn't play: previous mustn't go back to it.
    * The advance below records what plays instead.  A track the user picked
    * stays: playback stops on it, and previous goes to the one before. */
   const char *current = music_history_current(&state->history);
   if (is_auto && state->history_fresh && current && strcmp(current, path) == 0) {
      music_history_drop_current(&state->history);
   }

   if (!is_auto) {
      /* A track the user picked: say so and stop, don't skip it */
      state->playing = false;
      state->paused = false;
      pthread_mutex_unlock(&state->state_mutex);
      if (unreachable) {
         webui_music_send_error(conn, "PLEX_UNREACHABLE", "The Plex server isn't reachable");
      } else {
         webui_music_send_error(conn, "PLAYBACK_ERROR", "Couldn't play this track");
      }
      webui_music_send_state(conn, state);
      return true;
   }
   pthread_mutex_unlock(&state->state_mutex);

   /* Auto-advance: skip it, unless every queued track is on an unreachable
    * Plex server (each would wait out the same connect timeout) */
   user_music_queue_t *uq = state->shared_queue;
   pthread_mutex_lock(&uq->queue_mutex);
   bool queue_has_local = false;
   for (int i = 0; i < uq->queue_length && !queue_has_local; i++) {
      queue_has_local = music_source_from_path(uq->queue[i].path) != MUSIC_SOURCE_PLEX;
   }
   bool repeat_one = uq->repeat_mode == MUSIC_REPEAT_ONE;
   pthread_mutex_unlock(&uq->queue_mutex);

   if (unreachable && !queue_has_local) {
      pthread_mutex_lock(&state->state_mutex);
      state->playing = false;
      pthread_mutex_unlock(&state->state_mutex);
      webui_music_send_error(conn, "PLEX_UNREACHABLE", "The Plex server isn't reachable");
      /* Tagged: the client plays out the ended track's last seconds first */
      webui_music_send_state_impl(conn, state, true);
      return true;
   }

   OLOG_WARNING("WebUI music: Couldn't open %s%s", path, repeat_one ? "" : ", skipping it");
   (*failures)++;
   if (*failures == 1) {
      webui_music_send_error(conn, "DECODE_ERROR",
                             repeat_one ? "Couldn't play this track"
                                        : "Couldn't play a track, skipping it");
   }
   /* Picks the next one into the slot (or stops); the thread opens it next */
   webui_music_advance(state, conn, true, failures);
   return true;
}

void webui_music_cancel_pending_ticket(session_music_state_t *state) {
   pthread_mutex_lock(&state->state_mutex);
   if (state->pending_path[0] && state->open_done_seq != state->pending_seq) {
      publish_open(state, state->pending_seq, MUSIC_OPEN_CANCELLED);
   }
   pthread_mutex_unlock(&state->state_mutex);
}

music_open_result_t webui_music_await_track(session_music_state_t *state,
                                            uint32_t ticket,
                                            int timeout_ms) {
   struct timespec deadline;
   clock_gettime(CLOCK_MONOTONIC, &deadline); /* open_cond runs on CLOCK_MONOTONIC */
   deadline.tv_sec += timeout_ms / 1000;
   deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
   if (deadline.tv_nsec >= 1000000000L) {
      deadline.tv_sec++;
      deadline.tv_nsec -= 1000000000L;
   }

   music_open_result_t result = MUSIC_OPEN_PENDING;
   pthread_mutex_lock(&state->state_mutex);
   for (;;) {
      if (state->request_done_seq == ticket) {
         result = (music_open_result_t)state->request_result;
         break;
      }
      if (state->request_seq != ticket) {
         result = MUSIC_OPEN_CANCELLED; /* a newer request replaced it */
         break;
      }
      if (atomic_load(&state->control_cancel)) {
         break; /* a browser control wants the lock; the open carries on */
      }
      if (pthread_cond_timedwait(&state->open_cond, &state->state_mutex, &deadline) == ETIMEDOUT) {
         break;
      }
   }
   pthread_mutex_unlock(&state->state_mutex);
   return result;
}

/* Where a next/previous step lands, -1 for nowhere.  With commit the step is
 * made (the history moves, or the pick is recorded); without, nothing changes
 * but an adoption after another tab's replace and stale history entries, and a
 * shuffle next returns 0 to mean "a move happens" (its pick is drawn only when
 * committed). */
static int nav_pick(session_music_state_t *state,
                    user_music_queue_t *uq,
                    music_nav_dir_t dir,
                    bool commit) {
   int q_len = uq->queue_length;
   if (q_len == 0) {
      return -1;
   }
   /* After another tab's replace, from where the new queue holds the track
    * (adopted), so a shuffle pick doesn't land on it again */
   bool held;
   int current = step_base(state, uq, &held);
   if (uq->shuffle && dir == MUSIC_NAV_PREVIOUS) {
      /* Back through what played, whatever the queue's size: nothing earlier
       * is nothing earlier, not a wrap */
      return music_history_step(history_of(state, uq), false, commit, queue_find, uq);
   }
   if (uq->shuffle && q_len > 1) {
      return commit ? shuffle_next(state, uq, q_len, current) : 0;
   }
   /* -1 after a replace whose new queue doesn't hold the track: next is its
    * top, nothing is before it */
   if (current > q_len - 1) {
      current = q_len - 1; /* another tab's removes left the index past the end */
   }
   int next = -1;
   if (dir == MUSIC_NAV_NEXT && current < q_len - 1) {
      next = current + 1;
   } else if (dir == MUSIC_NAV_PREVIOUS && current > 0) {
      next = current - 1;
   } else if (uq->repeat_mode == MUSIC_REPEAT_ALL) {
      next = dir == MUSIC_NAV_NEXT ? 0 : q_len - 1;
   }
   if (next >= 0 && commit) {
      history_record(state, uq, next);
   }
   return next;
}

bool webui_music_nav_step(session_music_state_t *state,
                          music_nav_dir_t dir,
                          char *path_out,
                          size_t path_size,
                          char *title_out,
                          size_t title_size) {
   user_music_queue_t *uq = state->shared_queue;
   path_out[0] = '\0';
   if (title_out && title_size > 0) {
      title_out[0] = '\0';
   }

   /* Lock order: queue, then state */
   pthread_mutex_lock(&uq->queue_mutex);
   pthread_mutex_lock(&state->state_mutex);
   int target = nav_pick(state, uq, dir, false);
   pthread_mutex_unlock(&state->state_mutex);
   pthread_mutex_unlock(&uq->queue_mutex);
   if (target < 0) {
      return false; /* nothing to move to: the current track plays on */
   }

   /* Stop the stream thread before moving, so an auto-advance can't change
    * the index or the history between the pick and the new track's start */
   webui_music_stop_streaming(state);

   pthread_mutex_lock(&uq->queue_mutex);
   pthread_mutex_lock(&state->state_mutex);
   target = nav_pick(state, uq, dir, true);
   if (target >= 0 && target < uq->queue_length) {
      state->queue_index = target;
      snprintf(path_out, path_size, "%s", uq->queue[target].path);
      if (title_out && title_size > 0) {
         snprintf(title_out, title_size, "%s", uq->queue[target].title);
      }
   }
   pthread_mutex_unlock(&state->state_mutex);
   pthread_mutex_unlock(&uq->queue_mutex);
   if (!path_out[0]) {
      /* Another tab changed the queue during the stop: nothing to move to
       * after all, so the current track plays on (if the track had just ended,
       * there's nothing left to play and the restarted thread idles) */
      webui_music_start_streaming(state);
      return false;
   }
   return true;
}

void webui_music_clear_track(session_music_state_t *state) {
   webui_music_stop_streaming(state);
   pthread_mutex_lock(&state->state_mutex);
   if (state->decoder) {
      audio_decoder_close(state->decoder);
      state->decoder = NULL;
   }
   if (state->prefetched) {
      audio_decoder_close(state->prefetched);
      state->prefetched = NULL;
   }
   state->prefetch_path[0] = '\0';
   state->track.path[0] = '\0';
   state->seek_pending = false;
   state->playing = false;
   state->paused = false;
   state->position_frames = 0;
   state->loading = false;
   if (state->pending_path[0] && state->open_done_seq != state->pending_seq) {
      publish_open(state, state->pending_seq, MUSIC_OPEN_CANCELLED);
   }
   state->pending_path[0] = '\0';
   pthread_mutex_unlock(&state->state_mutex);
}

int webui_music_request_track(session_music_state_t *state,
                              const char *path,
                              uint32_t *ticket_out) {
   /* Stop the current track's thread first: it reads the decoder we replace */
   webui_music_stop_streaming(state);

   /* Set everything the caller's own state frame reports, so it describes
    * the new track (the thread's frame once it plays then matches it, and the
    * client doesn't flush twice).  Lock order: queue, then state. */
   user_music_queue_t *uq = state->shared_queue;
   pthread_mutex_lock(&uq->queue_mutex);
   pthread_mutex_lock(&state->state_mutex);
   uint32_t duration = 0;
   if (state->queue_index >= 0 && state->queue_index < uq->queue_length) {
      duration = uq->queue[state->queue_index].duration_sec;
      if (strcmp(uq->queue[state->queue_index].path, path) == 0) {
         history_record(state, uq, state->queue_index);
      }
   }
   set_track(state, uq, state->queue_index, path);
   pthread_mutex_unlock(&uq->queue_mutex);

   if (state->decoder) {
      audio_decoder_close(state->decoder);
      state->decoder = NULL;
   }
   if (state->pending_path[0] && state->open_done_seq != state->pending_seq) {
      publish_open(state, state->pending_seq, MUSIC_OPEN_CANCELLED);
   }
   uint32_t ticket = set_pending(state, path, false);
   state->history_fresh = false; /* a user's pick: its failure leaves the history */
   state->request_seq = ticket;
   state->playing = true;
   state->paused = false;
   state->position_frames = 0;
   state->cached_duration_sec = duration;
   pthread_mutex_unlock(&state->state_mutex);
   if (ticket_out) {
      *ticket_out = ticket;
   }

   if (webui_music_start_streaming(state) != 0) {
      pthread_mutex_lock(&state->state_mutex);
      if (state->pending_seq == ticket) {
         state->pending_path[0] = '\0';
         state->loading = false;
         state->playing = false;
         publish_open(state, ticket, MUSIC_OPEN_FAILED);
      }
      pthread_mutex_unlock(&state->state_mutex);
      return FAILURE;
   }
   return SUCCESS;
}

int webui_music_start_playback(session_music_state_t *state, const char *path) {
   return webui_music_request_track(state, path, NULL);
}
