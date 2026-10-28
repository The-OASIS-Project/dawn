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
#include "dawn_error.h"
#include "logging.h"
#include "webui/webui_internal.h"
#include "webui/webui_music.h"
#include "webui/webui_music_internal.h"

/**
 * @brief Hand a freshly opened track to the output (no lock held: it may open
 *        a device)
 *
 * @param gapless A natural advance: its audio follows what's buffered
 * @param read_frames Out: frames per read, within the player's read buffer
 * @return SUCCESS, or FAILURE if it can't be played here
 */
static int begin_track(session_music_state_t *state,
                       audio_decoder_t *decoder,
                       const char *path,
                       bool gapless,
                       size_t *read_frames) {
   audio_decoder_info_t info;
   audio_decoder_get_info(decoder, &info);
   *read_frames = 0;
   if (info.channels < 1 || info.channels > 2 ||
       state->ops->track_begin(state->ops_ctx, &info, gapless, read_frames) != SUCCESS ||
       *read_frames < 1 || *read_frames > MUSIC_PLAYER_READ_FRAMES_MAX) {
      OLOG_ERROR("WebUI music: Can't play %s (%u Hz, %u channels)", path, info.sample_rate,
                 info.channels);
      return FAILURE;
   }
   return SUCCESS;
}

/**
 * @brief Install a decoder begun on the output (state_mutex held)
 */
static void music_install_decoder(session_music_state_t *state,
                                  audio_decoder_t *decoder,
                                  size_t read_frames) {
   audio_decoder_info_t info;
   audio_decoder_get_info(decoder, &info);
   state->read_frames = read_frames;
   state->decoder = decoder;
   state->seek_pending = false;
   state->source_rate = info.sample_rate;
   state->source_channels = info.channels;
   state->source_format = info.format;
   state->position_frames = 0;
}

/* Tell the output's listener (no queue or state lock held) */
static void notify_state(session_music_state_t *state, bool gapless) {
   music_event_t ev = { .type = MUSIC_EVENT_STATE, .gapless = gapless };
   state->ops->notify(state->ops_ctx, &ev);
}

static void notify_error(session_music_state_t *state, music_error_t error, const char *message) {
   music_event_t ev = { .type = MUSIC_EVENT_ERROR, .error = error, .message = message };
   state->ops->notify(state->ops_ctx, &ev);
}

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
      state->pos.index = found;
   } else {
      memset(&state->track, 0, sizeof(state->track));
      snprintf(state->track.path, sizeof(state->track.path), "%s", path);
      state->track_epoch = uq->replace_epoch - 1;
   }
}

/* Where next steps from: the session's place.  After another tab replaced
 * the queue, the track's place in the new queue when it holds it (adopted:
 * the session is back on the queue), else its top, lost.  When the track
 * at its place isn't this session's any more (another tab removed it, or a
 * full queue's insert dropped it), that place, lost: next is the track
 * that took its place. */
static void step_base(session_music_state_t *state,
                      user_music_queue_t *uq,
                      music_nav_request_t *rq) {
   rq->lost = false;
   if (off_queue(state, uq)) {
      int found = queue_find(uq, state->track.path, state->pos.index);
      if (found >= 0) {
         state->track_epoch = uq->replace_epoch;
         state->pos.index = found;
         rq->pos = found;
         return;
      }
      rq->lost = true;
      rq->pos = 0;
      return;
   }
   rq->pos = state->pos.index;
   if (state->track.path[0] &&
       (rq->pos >= uq->queue_length || strcmp(uq->queue[rq->pos].path, state->track.path) != 0)) {
      rq->lost = true;
   }
}

static const char *queue_path_at(void *ctx, int index) {
   return ((const user_music_queue_t *)ctx)->queue[index].path;
}

/* What plays next from the session's place (step_base()), by music_nav_pick() */
static void pick(session_music_state_t *state,
                 user_music_queue_t *uq,
                 music_nav_request_t *rq,
                 music_nav_pick_t *out) {
   step_base(state, uq, rq);
   music_nav_queue_t q = {
      .find = queue_find,
      .path_at = queue_path_at,
      .ctx = uq,
      .length = uq->queue_length,
      .epoch = uq->replace_epoch,
      .shuffle = uq->shuffle,
      .repeat_all = uq->repeat_mode == MUSIC_REPEAT_ALL,
      .repeat_one = uq->repeat_mode == MUSIC_REPEAT_ONE,
   };
   music_nav_pick(&state->history, &q, rq, &state->shuffle_seed, out);
}

int webui_music_current(const session_music_state_t *state,
                        const user_music_queue_t *uq,
                        const music_queue_entry_t **track_out) {
   int index = state->pos.index;
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
   return music_history_record(history_of(state, uq), uq->queue[index].path, index);
}

/* ---- Remote-source prefetch -------------------------------------------- */

static void *prefetch_thread(void *arg) {
   session_music_state_t *state = (session_music_state_t *)arg;
   char path[MUSIC_PATH_MAX];
   pthread_mutex_lock(&state->state_mutex);
   snprintf(path, sizeof(path), "%s", state->prefetch_path);
   pthread_mutex_unlock(&state->state_mutex);

   audio_decoder_t *decoder = music_open_track(path, &state->prefetch_cancel, NULL);

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
 * if it's from a remote source.  Repeat-one replays the current track, so nothing is
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
   /* A shuffle pick is planned as the history's next entry, so next and
    * auto-advance play the track prefetched here */
   music_nav_request_t rq = { .reason = MUSIC_NAV_PLAN };
   music_nav_pick_t planned;
   pick(state, uq, &rq, &planned);
   int next = planned.result == MUSIC_NAV_PLAY ? planned.index : -1;
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
   if (!next_path[0] || !music_source_is_remote(music_source_from_path(next_path))) {
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
   state->loading = music_source_is_remote(music_source_from_path(path)) && !prefetched;
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
 * @param track_failed The track that just ended did so on a decode error
 * @param failures In/out: tracks in a row that produced no audio
 * @return What happened
 */
music_advance_t webui_music_advance(session_music_state_t *state,
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

      /* After another tab replaced the queue: from the ended track's position in
       * the new one, or its top when it doesn't hold the track (nothing to replay) */
      music_nav_request_t rq = {
         .reason = MUSIC_NAV_AUTO,
         .failures = *failures,
         .track_failed = track_failed,
      };
      music_nav_pick_t next;
      pick(state, uq, &rq, &next);
      bool stop = next.result != MUSIC_NAV_PLAY;
      bool end_of_queue = next.result == MUSIC_NAV_STOP_END;
      const char *stop_error = NULL;
      if (next.result == MUSIC_NAV_STOP_ALL_FAILED) {
         stop_error = "None of the queued tracks would play";
      } else if (next.result == MUSIC_NAV_STOP_TOO_MANY) {
         stop_error = "Stopped: several tracks in a row wouldn't play";
      }

      if (stop) {
         state->playing = false;
         if (end_of_queue) {
            state->pos.index = 0;
            state->position_frames = 0; /* nothing is loaded; don't report the old end */
            state->track.path[0] = '\0';
         }
         pthread_mutex_unlock(&state->state_mutex);
         pthread_mutex_unlock(&uq->queue_mutex);
         /* Nothing follows, so a prefetch (its track since removed) won't be used */
         prefetch_drop(state);
         if (stop_error) {
            notify_error(state, MUSIC_ERROR_DECODE, stop_error);
         }
         /* Tagged like a natural advance: the client still holds the ended
          * track's last ~2 s and plays them out before showing the stop */
         notify_state(state, true);
         return MUSIC_ADVANCE_STOPPED;
      }

      int next_index = next.index;
      state->history_fresh = next.fresh;
      state->pos.index = next_index;
      set_track(state, uq, next_index, uq->queue[next_index].path);
      state->cached_duration_sec = uq->queue[next_index].duration_sec;
      state->position_frames = 0;
      set_pending(state, uq->queue[next_index].path, true);
      bool loading = state->loading;
      pthread_mutex_unlock(&state->state_mutex);
      pthread_mutex_unlock(&uq->queue_mutex);

      /* A remote track downloads first: tell the client (tagged, so it applies
       * once the ended track's buffered audio has played) */
      if (loading) {
         notify_state(state, true);
      }
      return MUSIC_ADVANCE_PENDING;
   }
}

bool webui_music_run_pending_open(session_music_state_t *state, int *failures) {
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
      decoder = music_open_track(path, &state->stop_requested, &unreachable);
   }
   size_t read_frames = 0;
   if (decoder && !atomic_load(&state->stop_requested) &&
       begin_track(state, decoder, path, is_auto, &read_frames) != SUCCESS) {
      audio_decoder_close(decoder);
      decoder = NULL;
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
   if (decoder) {
      music_install_decoder(state, decoder, read_frames);
      publish_open(state, seq, MUSIC_OPEN_PLAYING);
      OLOG_INFO("WebUI music: Playing %s (%s %u Hz, %u ch)", path,
                audio_decoder_format_name(state->source_format), state->source_rate,
                state->source_channels);
      pthread_mutex_unlock(&state->state_mutex);
      /* An auto-advance stays tagged so the client keeps its buffered lead */
      notify_state(state, is_auto);
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
         notify_error(state, MUSIC_ERROR_UNREACHABLE, "The Plex server isn't reachable");
      } else {
         notify_error(state, MUSIC_ERROR_PLAYBACK, "Couldn't play this track");
      }
      notify_state(state, false);
      return true;
   }
   pthread_mutex_unlock(&state->state_mutex);

   /* Auto-advance: skip it, unless every queued track is from the same
    * unreachable server (each would wait out the same connect timeout) */
   user_music_queue_t *uq = state->shared_queue;
   music_source_t source = music_source_from_path(path);
   pthread_mutex_lock(&uq->queue_mutex);
   bool queue_has_other = !unreachable; /* only an unreachable server stops it */
   for (int i = 0; i < uq->queue_length && !queue_has_other; i++) {
      queue_has_other = music_source_from_path(uq->queue[i].path) != source;
   }
   bool repeat_one = uq->repeat_mode == MUSIC_REPEAT_ONE;
   pthread_mutex_unlock(&uq->queue_mutex);

   if (unreachable && !queue_has_other) {
      pthread_mutex_lock(&state->state_mutex);
      state->playing = false;
      pthread_mutex_unlock(&state->state_mutex);
      notify_error(state, MUSIC_ERROR_UNREACHABLE, "The Plex server isn't reachable");
      /* Tagged: the client plays out the ended track's last seconds first */
      notify_state(state, true);
      return true;
   }

   OLOG_WARNING("WebUI music: Couldn't open %s%s", path, repeat_one ? "" : ", skipping it");
   (*failures)++;
   if (*failures == 1) {
      notify_error(state, MUSIC_ERROR_DECODE,
                   repeat_one ? "Couldn't play this track" : "Couldn't play a track, skipping it");
   }
   /* Picks the next one into the slot (or stops); the thread opens it next */
   webui_music_advance(state, true, failures);
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
   /* After another tab's replace, from where the new queue holds the track
    * (adopted), so a shuffle pick doesn't land on it again */
   music_nav_request_t rq = {
      .reason = MUSIC_NAV_USER,
      .dir = dir,
      .commit = commit,
   };
   music_nav_pick_t next;
   pick(state, uq, &rq, &next);
   if (next.result != MUSIC_NAV_PLAY) {
      return -1;
   }
   return next.index >= 0 ? next.index : 0; /* a shuffle pick drawn only when committed */
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
      state->pos.index = target;
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
   if (state->pos.index >= 0 && state->pos.index < uq->queue_length) {
      duration = uq->queue[state->pos.index].duration_sec;
      if (strcmp(uq->queue[state->pos.index].path, path) == 0) {
         history_record(state, uq, state->pos.index);
      }
   }
   set_track(state, uq, state->pos.index, path);
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
