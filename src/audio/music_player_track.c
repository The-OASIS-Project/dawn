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
 * Music player: opening tracks and choosing what plays.  The one place a
 * queue path becomes a playing decoder (music_open_track(): path and format
 * checks, a remote download), installing it, starting a track on request,
 * moving to the next one at a track's end (skipping ones that won't play),
 * and the queue actions that change what's playing.
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
#include "audio/music_player_internal.h"
#include "audio/music_source.h"
#include "dawn_error.h"
#include "logging.h"

/**
 * @brief Hand a freshly opened track to the output (no lock held: it may open
 *        a device)
 *
 * @param gapless A natural advance: its audio follows what's buffered
 * @param read_frames Out: frames per read, within the player's read buffer
 * @return SUCCESS, or FAILURE if it can't be played here
 */
static int begin_track(music_player_t *p,
                       audio_decoder_t *decoder,
                       const char *path,
                       bool gapless,
                       size_t *read_frames) {
   audio_decoder_info_t info;
   audio_decoder_get_info(decoder, &info);
   *read_frames = 0;
   if (info.channels < 1 || info.channels > 2 ||
       p->ops->track_begin(p->ops_ctx, &info, gapless, read_frames) != SUCCESS ||
       *read_frames < 1 || *read_frames > MUSIC_PLAYER_READ_FRAMES_MAX) {
      OLOG_ERROR("Music player: Can't play %s (%u Hz, %u channels)", path, info.sample_rate,
                 info.channels);
      return FAILURE;
   }
   return SUCCESS;
}

/**
 * @brief Install a decoder begun on the output (state_mutex held)
 */
static void music_install_decoder(music_player_t *p, audio_decoder_t *decoder, size_t read_frames) {
   audio_decoder_info_t info;
   audio_decoder_get_info(decoder, &info);
   p->read_frames = read_frames;
   p->decoder = decoder;
   p->seek_pending = false;
   p->source_rate = info.sample_rate;
   p->source_channels = info.channels;
   p->source_format = info.format;
   p->position_frames = 0;
}

/* Tell the output's listener (no queue or state lock held) */
static void notify_state(music_player_t *p, bool gapless) {
   music_event_t ev = { .type = MUSIC_EVENT_STATE, .gapless = gapless };
   p->ops->notify(p->ops_ctx, &ev);
}

static void notify_error(music_player_t *p, music_error_t error, const char *message) {
   music_event_t ev = { .type = MUSIC_EVENT_ERROR, .error = error, .message = message };
   p->ops->notify(p->ops_ctx, &ev);
}

/* ---- Play history (queue_mutex, then state_mutex, held) ----------------- */

/* A history entry's queue position: the copy of path nearest its hint */
static int queue_find(void *ctx, const char *path, int hint) {
   return music_queue_find((const user_music_queue_t *)ctx, path, hint);
}

/* Another player replaced the queue since this one picked its track */
static bool off_queue(const music_player_t *p, const user_music_queue_t *uq) {
   return p->track.path[0] != '\0' && p->track_epoch != uq->replace_epoch;
}

/* Note the track this player picked: the queue entry at index, or where
 * else the queue holds path; a path it doesn't hold (another player replaced the
 * queue since the pick) is noted as off the queue */
static void set_track(music_player_t *p, user_music_queue_t *uq, int index, const char *path) {
   p->track_epoch = uq->replace_epoch;
   int found = queue_find(uq, path, index);
   if (found >= 0) {
      p->track = uq->queue[found];
      p->pos.index = found;
   } else {
      memset(&p->track, 0, sizeof(p->track));
      snprintf(p->track.path, sizeof(p->track.path), "%s", path);
      p->track_epoch = uq->replace_epoch - 1;
   }
}

/* Where next steps from: the player's place.  After another player replaced
 * the queue, the track's place in the new queue when it holds it (adopted:
 * the player is back on the queue), else its top, lost.  When the track
 * at its place isn't this player's any more (another player removed it, or a
 * full queue's insert dropped it), that place, lost: next is the track
 * that took its place. */
static void step_base(music_player_t *p, user_music_queue_t *uq, music_nav_request_t *rq) {
   rq->lost = false;
   if (off_queue(p, uq)) {
      int found = queue_find(uq, p->track.path, p->pos.index);
      if (found >= 0) {
         p->track_epoch = uq->replace_epoch;
         p->pos.index = found;
         rq->pos = found;
         return;
      }
      rq->lost = true;
      rq->pos = 0;
      return;
   }
   rq->pos = p->pos.index;
   if (p->track.path[0] &&
       (rq->pos >= uq->queue_length || strcmp(uq->queue[rq->pos].path, p->track.path) != 0)) {
      rq->lost = true;
   }
}

static const char *queue_path_at(void *ctx, int index) {
   return ((const user_music_queue_t *)ctx)->queue[index].path;
}

/* What plays next from the player's place (step_base()), by music_nav_pick() */
static void pick(music_player_t *p,
                 user_music_queue_t *uq,
                 music_nav_request_t *rq,
                 music_nav_pick_t *out) {
   step_base(p, uq, rq);
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
   music_nav_pick(&p->history, &q, rq, &p->shuffle_seed, out);
}

/* What the player reports as playing (music_player_current()) */
static int current(const music_player_t *p,
                   const user_music_queue_t *uq,
                   const music_queue_entry_t **track_out) {
   int index = p->pos.index;
   if (p->track.path[0] && (p->playing || p->paused)) {
      *track_out = &p->track;
      return queue_find((void *)uq, p->track.path, index);
   }
   if (index >= 0 && index < uq->queue_length) {
      *track_out = &uq->queue[index];
      return index;
   }
   *track_out = NULL;
   return -1;
}

int music_player_current(music_player_t *p, music_queue_entry_t *track_out) {
   pthread_mutex_lock(&p->state_mutex);
   const music_queue_entry_t *now = NULL;
   int index = current(p, p->shared_queue, &now);
   if (now) {
      *track_out = *now;
   } else {
      memset(track_out, 0, sizeof(*track_out));
   }
   pthread_mutex_unlock(&p->state_mutex);
   return index;
}

void music_player_snapshot(music_player_t *p, music_player_status_t *out) {
   user_music_queue_t *uq = p->shared_queue;
   memset(out, 0, sizeof(*out));
   /* Lock order: queue, then state */
   pthread_mutex_lock(&uq->queue_mutex);
   out->queue_length = uq->queue_length;
   out->shuffle = uq->shuffle;
   out->repeat_mode = uq->repeat_mode;
   pthread_mutex_lock(&p->state_mutex);
   const music_queue_entry_t *now = NULL;
   out->queue_index = current(p, uq, &now);
   if (now) {
      out->track = *now;
   }
   out->playing = p->playing;
   out->paused = p->paused;
   out->loading = p->loading;
   if (p->source_rate > 0) {
      out->position_sec = (double)p->position_frames / p->source_rate;
   }
   out->source_format = p->source_format;
   out->source_rate = p->source_rate;
   pthread_mutex_unlock(&p->state_mutex);
   pthread_mutex_unlock(&uq->queue_mutex);
}

/* The player's history, cleared first if the queue was replaced since */
static music_history_t *history_of(music_player_t *p, user_music_queue_t *uq) {
   music_history_sync(&p->history, uq->replace_epoch);
   return &p->history;
}

/* Record that queue position index plays, unless it's the history's current
 * entry already (a step through the history, or a replay); true when added */
static bool history_record(music_player_t *p, user_music_queue_t *uq, int index) {
   return music_history_record(history_of(p, uq), uq->queue[index].path, index);
}

/* ---- Remote-source prefetch -------------------------------------------- */

static void *prefetch_thread(void *arg) {
   music_player_t *p = (music_player_t *)arg;
   char path[MUSIC_PATH_MAX];
   pthread_mutex_lock(&p->state_mutex);
   snprintf(path, sizeof(path), "%s", p->prefetch_path);
   pthread_mutex_unlock(&p->state_mutex);

   audio_decoder_t *decoder = music_open_track(path, &p->prefetch_cancel, NULL);

   pthread_mutex_lock(&p->state_mutex);
   bool keep = decoder && !atomic_load(&p->prefetch_cancel) && strcmp(p->prefetch_path, path) == 0;
   if (keep) {
      p->prefetched = decoder;
      decoder = NULL;
   } else if (strcmp(p->prefetch_path, path) == 0) {
      /* Nothing kept: a later plan may fetch it again */
      p->prefetch_path[0] = '\0';
   }
   pthread_mutex_unlock(&p->state_mutex);
   if (decoder) {
      audio_decoder_close(decoder);
   }
   if (keep) {
      OLOG_INFO("Music player: Prefetched %s", path);
   }
   return NULL;
}

void music_player_prefetch_stop(music_player_t *p) {
   if (!p->prefetch_running) {
      return;
   }
   atomic_store(&p->prefetch_cancel, true);
   pthread_join(p->prefetch_thread, NULL);
   p->prefetch_running = false;
}

/* Wait for a running helper without cancelling it: its track is the one
 * about to open.  After a stop, cancel it instead, so the stop's join stays
 * short. */
static void prefetch_finish(music_player_t *p) {
   if (atomic_load(&p->stop_requested)) {
      music_player_prefetch_stop(p);
   } else if (p->prefetch_running) {
      pthread_join(p->prefetch_thread, NULL);
      p->prefetch_running = false;
   }
}

/* Drop a prefetch nothing is going to open (player's thread) */
static void prefetch_drop(music_player_t *p) {
   music_player_prefetch_stop(p);
   pthread_mutex_lock(&p->state_mutex);
   if (p->prefetched) {
      audio_decoder_close(p->prefetched);
      p->prefetched = NULL;
   }
   p->prefetch_path[0] = '\0';
   pthread_mutex_unlock(&p->state_mutex);
}

/*
 * After a track starts: plan the one that follows, and start downloading it
 * if it's from a remote source.  Repeat-one replays the current track, so nothing is
 * planned.  Prefetch errors are never reported; the real open reports them.
 */
static void prefetch_plan(music_player_t *p) {
   user_music_queue_t *uq = p->shared_queue;
   char next_path[MUSIC_PATH_MAX] = { 0 };

   /* A stop must find no helper it didn't cancel */
   if (atomic_load(&p->stop_requested)) {
      return;
   }

   pthread_mutex_lock(&uq->queue_mutex);
   pthread_mutex_lock(&p->state_mutex);
   /* A shuffle pick is planned as the history's next entry, so next and
    * auto-advance play the track prefetched here */
   music_nav_request_t rq = { .reason = MUSIC_NAV_PLAN };
   music_nav_pick_t planned;
   pick(p, uq, &rq, &planned);
   int next = planned.result == MUSIC_NAV_PLAY ? planned.index : -1;
   if (next >= 0) {
      snprintf(next_path, sizeof(next_path), "%s", uq->queue[next].path);
   }
   bool already = next_path[0] && strcmp(p->prefetch_path, next_path) == 0;
   pthread_mutex_unlock(&p->state_mutex);
   pthread_mutex_unlock(&uq->queue_mutex);

   if (already) {
      return;
   }
   /* A different track than any earlier prefetch (or none): drop that one */
   prefetch_drop(p);
   if (!next_path[0] || !music_source_is_remote(music_source_from_path(next_path))) {
      return;
   }

   pthread_mutex_lock(&p->state_mutex);
   snprintf(p->prefetch_path, sizeof(p->prefetch_path), "%s", next_path);
   pthread_mutex_unlock(&p->state_mutex);

   atomic_store(&p->prefetch_cancel, false);
   if (pthread_create(&p->prefetch_thread, NULL, prefetch_thread, p) == 0) {
      p->prefetch_running = true;
   } else {
      pthread_mutex_lock(&p->state_mutex);
      p->prefetch_path[0] = '\0';
      pthread_mutex_unlock(&p->state_mutex);
   }
}

/* The prefetched decoder for path, if that's what was prefetched; any other
 * prefetch is dropped (player's thread) */
static audio_decoder_t *prefetch_take(music_player_t *p, const char *path) {
   pthread_mutex_lock(&p->state_mutex);
   bool same = strcmp(p->prefetch_path, path) == 0;
   pthread_mutex_unlock(&p->state_mutex);
   if (same) {
      prefetch_finish(p);
   } else {
      music_player_prefetch_stop(p);
   }

   pthread_mutex_lock(&p->state_mutex);
   audio_decoder_t *decoder = NULL;
   if (p->prefetched && same) {
      decoder = p->prefetched;
   } else if (p->prefetched) {
      audio_decoder_close(p->prefetched);
   }
   p->prefetched = NULL;
   p->prefetch_path[0] = '\0';
   pthread_mutex_unlock(&p->state_mutex);
   return decoder;
}

/* Resolve a ticket: record its outcome and wake a waiting tool (state_mutex held) */
static void publish_open(music_player_t *p, uint32_t seq, music_open_result_t result) {
   p->open_done_seq = seq;
   if (seq == p->request_seq) {
      p->request_done_seq = seq;
      p->request_result = result;
   }
   pthread_cond_broadcast(&p->open_cond);
}

/* Fill the pending slot (state_mutex held); returns the new ticket */
static uint32_t set_pending(music_player_t *p, const char *path, bool is_auto) {
   snprintf(p->pending_path, sizeof(p->pending_path), "%s", path);
   p->pending_auto = is_auto;
   p->pending_seq = ++p->open_seq;
   /* A prefetched track opens at once: no download to show */
   bool prefetched = p->prefetched && strcmp(p->prefetch_path, path) == 0;
   p->loading = music_source_is_remote(music_source_from_path(path)) && !prefetched;
   return p->pending_seq;
}

/**
 * @brief Pick the next track after the current one ended
 *
 * Playback stops once as many tracks in a row have produced no audio as the
 * queue holds, or at once under repeat-one after a failure, so a queue of
 * unplayable tracks can't spin.  A track that won't open is skipped by
 * music_player_run_pending_open(), which calls back here.
 *
 * @param p Player (decoder already closed and NULL)
 * @param track_failed The track that just ended did so on a decode error
 * @param failures In/out: tracks in a row that produced no audio
 * @return What happened
 */
music_advance_t music_player_advance(music_player_t *p, bool track_failed, int *failures) {
   user_music_queue_t *uq = p->shared_queue;

   for (;;) {
      /* Pick the next track.  Lock order: queue, then state. */
      pthread_mutex_lock(&uq->queue_mutex);
      pthread_mutex_lock(&p->state_mutex);
      if (atomic_load(&p->stop_requested)) {
         /* Whoever stopped us decides what plays next; until then nothing is */
         p->playing = false;
         pthread_mutex_unlock(&p->state_mutex);
         pthread_mutex_unlock(&uq->queue_mutex);
         return MUSIC_ADVANCE_EXIT;
      }

      /* After another player replaced the queue: from the ended track's position in
       * the new one, or its top when it doesn't hold the track (nothing to replay) */
      music_nav_request_t rq = {
         .reason = MUSIC_NAV_AUTO,
         .failures = *failures,
         .track_failed = track_failed,
      };
      music_nav_pick_t next;
      pick(p, uq, &rq, &next);
      bool stop = next.result != MUSIC_NAV_PLAY;
      bool end_of_queue = next.result == MUSIC_NAV_STOP_END;
      const char *stop_error = NULL;
      if (next.result == MUSIC_NAV_STOP_ALL_FAILED) {
         stop_error = "None of the queued tracks would play";
      } else if (next.result == MUSIC_NAV_STOP_TOO_MANY) {
         stop_error = "Stopped: several tracks in a row wouldn't play";
      }

      if (stop) {
         p->playing = false;
         if (end_of_queue) {
            p->pos.index = 0;
            p->position_frames = 0; /* nothing is loaded; don't report the old end */
            p->track.path[0] = '\0';
         }
         pthread_mutex_unlock(&p->state_mutex);
         pthread_mutex_unlock(&uq->queue_mutex);
         /* Nothing follows, so a prefetch (its track since removed) won't be used */
         prefetch_drop(p);
         if (stop_error) {
            notify_error(p, MUSIC_ERROR_DECODE, stop_error);
         }
         /* Tagged like a natural advance: the listener still holds the ended
          * track's last ~2 s and plays them out before showing the stop */
         notify_state(p, true);
         return MUSIC_ADVANCE_STOPPED;
      }

      int next_index = next.index;
      p->history_fresh = next.fresh;
      p->pos.index = next_index;
      set_track(p, uq, next_index, uq->queue[next_index].path);
      p->cached_duration_sec = uq->queue[next_index].duration_sec;
      p->position_frames = 0;
      set_pending(p, uq->queue[next_index].path, true);
      bool loading = p->loading;
      pthread_mutex_unlock(&p->state_mutex);
      pthread_mutex_unlock(&uq->queue_mutex);

      /* A remote track downloads first: tell the listener (tagged, so it applies
       * once the ended track's buffered audio has played) */
      if (loading) {
         notify_state(p, true);
      }
      return MUSIC_ADVANCE_PENDING;
   }
}

bool music_player_run_pending_open(music_player_t *p, int *failures) {
   char path[MUSIC_PATH_MAX];
   pthread_mutex_lock(&p->state_mutex);
   if (!p->pending_path[0]) {
      pthread_mutex_unlock(&p->state_mutex);
      return false;
   }
   snprintf(path, sizeof(path), "%s", p->pending_path);
   bool is_auto = p->pending_auto;
   uint32_t seq = p->pending_seq;
   pthread_mutex_unlock(&p->state_mutex);

   /* Open without the lock: a remote track is downloaded first.  A request that
    * stops this thread sets stop_requested, which cancels the download, so its
    * join waits about a second at most.  Every path that replaces the decoder
    * stops this thread first, so nothing else installs one meanwhile. */
   bool unreachable = false;
   audio_decoder_t *decoder = prefetch_take(p, path);
   if (!decoder) {
      decoder = music_open_track(path, &p->stop_requested, &unreachable);
   }
   size_t read_frames = 0;
   if (decoder && !atomic_load(&p->stop_requested) &&
       begin_track(p, decoder, path, is_auto, &read_frames) != SUCCESS) {
      audio_decoder_close(decoder);
      decoder = NULL;
   }

   pthread_mutex_lock(&p->state_mutex);
   if (atomic_load(&p->stop_requested)) {
      /* Leave the slot for whoever stopped us: it replaces or drops it */
      pthread_mutex_unlock(&p->state_mutex);
      if (decoder) {
         audio_decoder_close(decoder);
      }
      return true;
   }

   p->pending_path[0] = '\0';
   p->loading = false;
   if (decoder) {
      music_install_decoder(p, decoder, read_frames);
      publish_open(p, seq, MUSIC_OPEN_PLAYING);
      OLOG_INFO("Music player: Playing %s (%s %u Hz, %u ch)", path,
                audio_decoder_format_name(p->source_format), p->source_rate, p->source_channels);
      pthread_mutex_unlock(&p->state_mutex);
      /* An auto-advance stays tagged so the listener keeps its buffered lead */
      notify_state(p, is_auto);
      prefetch_plan(p);
      return true;
   }
   publish_open(p, seq, unreachable ? MUSIC_OPEN_UNREACHABLE : MUSIC_OPEN_FAILED);
   /* An auto-advance pick that didn't play: previous mustn't go back to it.
    * The advance below records what plays instead.  A track the user picked
    * stays: playback stops on it, and previous goes to the one before. */
   const char *current = music_history_current(&p->history);
   if (is_auto && p->history_fresh && current && strcmp(current, path) == 0) {
      music_history_drop_current(&p->history);
   }

   if (!is_auto) {
      /* A track the user picked: say so and stop, don't skip it */
      p->playing = false;
      p->paused = false;
      pthread_mutex_unlock(&p->state_mutex);
      if (unreachable) {
         notify_error(p, MUSIC_ERROR_UNREACHABLE, "The Plex server isn't reachable");
      } else {
         notify_error(p, MUSIC_ERROR_PLAYBACK, "Couldn't play this track");
      }
      notify_state(p, false);
      return true;
   }
   pthread_mutex_unlock(&p->state_mutex);

   /* Auto-advance: skip it, unless every queued track is from the same
    * unreachable server (each would wait out the same connect timeout) */
   user_music_queue_t *uq = p->shared_queue;
   music_source_t source = music_source_from_path(path);
   pthread_mutex_lock(&uq->queue_mutex);
   bool queue_has_other = !unreachable; /* only an unreachable server stops it */
   for (int i = 0; i < uq->queue_length && !queue_has_other; i++) {
      queue_has_other = music_source_from_path(uq->queue[i].path) != source;
   }
   bool repeat_one = uq->repeat_mode == MUSIC_REPEAT_ONE;
   pthread_mutex_unlock(&uq->queue_mutex);

   if (unreachable && !queue_has_other) {
      pthread_mutex_lock(&p->state_mutex);
      p->playing = false;
      pthread_mutex_unlock(&p->state_mutex);
      notify_error(p, MUSIC_ERROR_UNREACHABLE, "The Plex server isn't reachable");
      /* Tagged: the listener plays out the ended track's last seconds first */
      notify_state(p, true);
      return true;
   }

   OLOG_WARNING("Music player: Couldn't open %s%s", path, repeat_one ? "" : ", skipping it");
   (*failures)++;
   if (*failures == 1) {
      notify_error(p, MUSIC_ERROR_DECODE,
                   repeat_one ? "Couldn't play this track" : "Couldn't play a track, skipping it");
   }
   /* Picks the next one into the slot (or stops); the thread opens it next */
   music_player_advance(p, true, failures);
   return true;
}

void music_player_cancel_pending_ticket(music_player_t *p) {
   pthread_mutex_lock(&p->state_mutex);
   if (p->pending_path[0] && p->open_done_seq != p->pending_seq) {
      publish_open(p, p->pending_seq, MUSIC_OPEN_CANCELLED);
   }
   pthread_mutex_unlock(&p->state_mutex);
}

music_open_result_t music_player_await(music_player_t *p, uint32_t ticket, int timeout_ms) {
   struct timespec deadline;
   clock_gettime(CLOCK_MONOTONIC, &deadline); /* open_cond runs on CLOCK_MONOTONIC */
   deadline.tv_sec += timeout_ms / 1000;
   deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
   if (deadline.tv_nsec >= 1000000000L) {
      deadline.tv_sec++;
      deadline.tv_nsec -= 1000000000L;
   }

   music_open_result_t result = MUSIC_OPEN_PENDING;
   pthread_mutex_lock(&p->state_mutex);
   for (;;) {
      if (p->request_done_seq == ticket) {
         result = (music_open_result_t)p->request_result;
         break;
      }
      if (p->request_seq != ticket) {
         result = MUSIC_OPEN_CANCELLED; /* a newer request replaced it */
         break;
      }
      if (atomic_load(&p->control_cancel)) {
         break; /* a preempting request wants the lock; the open carries on */
      }
      if (pthread_cond_timedwait(&p->open_cond, &p->state_mutex, &deadline) == ETIMEDOUT) {
         break;
      }
   }
   pthread_mutex_unlock(&p->state_mutex);
   return result;
}

/* Where a next/previous step lands, -1 for nowhere.  With commit the step is
 * made (the history moves, or the pick is recorded); without, nothing changes
 * but an adoption after another player's replace and stale history entries, and a
 * shuffle next returns 0 to mean "a move happens" (its pick is drawn only when
 * committed). */
static int nav_pick(music_player_t *p, user_music_queue_t *uq, music_nav_dir_t dir, bool commit) {
   /* After another player's replace, from where the new queue holds the track
    * (adopted), so a shuffle pick doesn't land on it again */
   music_nav_request_t rq = {
      .reason = MUSIC_NAV_USER,
      .dir = dir,
      .commit = commit,
   };
   music_nav_pick_t next;
   pick(p, uq, &rq, &next);
   if (next.result != MUSIC_NAV_PLAY) {
      return -1;
   }
   return next.index >= 0 ? next.index : 0; /* a shuffle pick drawn only when committed */
}

bool music_player_step(music_player_t *p,
                       music_nav_dir_t dir,
                       char *path_out,
                       size_t path_size,
                       char *title_out,
                       size_t title_size) {
   user_music_queue_t *uq = p->shared_queue;
   path_out[0] = '\0';
   if (title_out && title_size > 0) {
      title_out[0] = '\0';
   }

   /* Lock order: queue, then state */
   pthread_mutex_lock(&uq->queue_mutex);
   pthread_mutex_lock(&p->state_mutex);
   int target = nav_pick(p, uq, dir, false);
   pthread_mutex_unlock(&p->state_mutex);
   pthread_mutex_unlock(&uq->queue_mutex);
   if (target < 0) {
      return false; /* nothing to move to: the current track plays on */
   }

   /* Stop the player's thread before moving, so an auto-advance can't change
    * the index or the history between the pick and the new track's start */
   music_player_stop_thread(p);

   pthread_mutex_lock(&uq->queue_mutex);
   pthread_mutex_lock(&p->state_mutex);
   target = nav_pick(p, uq, dir, true);
   if (target >= 0 && target < uq->queue_length) {
      p->pos.index = target;
      snprintf(path_out, path_size, "%s", uq->queue[target].path);
      if (title_out && title_size > 0) {
         snprintf(title_out, title_size, "%s", uq->queue[target].title);
      }
   }
   pthread_mutex_unlock(&p->state_mutex);
   pthread_mutex_unlock(&uq->queue_mutex);
   if (!path_out[0]) {
      /* Another player changed the queue during the stop: nothing to move to
       * after all, so the current track plays on (if the track had just ended,
       * there's nothing left to play and the restarted thread idles) */
      music_player_start_thread(p);
      return false;
   }
   return true;
}

void music_player_stop(music_player_t *p) {
   music_player_stop_thread(p);
   pthread_mutex_lock(&p->state_mutex);
   if (p->decoder) {
      audio_decoder_close(p->decoder);
      p->decoder = NULL;
   }
   if (p->prefetched) {
      audio_decoder_close(p->prefetched);
      p->prefetched = NULL;
   }
   p->prefetch_path[0] = '\0';
   p->track.path[0] = '\0';
   p->seek_pending = false;
   p->playing = false;
   p->paused = false;
   p->position_frames = 0;
   p->loading = false;
   if (p->pending_path[0] && p->open_done_seq != p->pending_seq) {
      publish_open(p, p->pending_seq, MUSIC_OPEN_CANCELLED);
   }
   p->pending_path[0] = '\0';
   pthread_mutex_unlock(&p->state_mutex);
}

int music_player_play(music_player_t *p, const char *path, int hint, uint32_t *ticket_out) {
   /* Stop the current track's thread first: it reads the decoder we replace */
   music_player_stop_thread(p);

   /* Set everything the caller's own state frame reports, so it describes
    * the new track (the thread's frame once it plays then matches it, and the
    * listener doesn't flush twice).  Lock order: queue, then state. */
   user_music_queue_t *uq = p->shared_queue;
   pthread_mutex_lock(&uq->queue_mutex);
   pthread_mutex_lock(&p->state_mutex);
   if (hint >= 0 && hint < uq->queue_length) {
      p->pos.index = hint;
   }
   /* The copy of path nearest the place: another player's insert or remove
    * during the stop may have moved it from the hint */
   uint32_t duration = 0;
   int at = queue_find(uq, path, p->pos.index);
   if (at >= 0) {
      duration = uq->queue[at].duration_sec;
      history_record(p, uq, at);
   }
   set_track(p, uq, p->pos.index, path);
   pthread_mutex_unlock(&uq->queue_mutex);

   if (p->decoder) {
      audio_decoder_close(p->decoder);
      p->decoder = NULL;
   }
   if (p->pending_path[0] && p->open_done_seq != p->pending_seq) {
      publish_open(p, p->pending_seq, MUSIC_OPEN_CANCELLED);
   }
   uint32_t ticket = set_pending(p, path, false);
   p->history_fresh = false; /* a user's pick: its failure leaves the history */
   p->request_seq = ticket;
   p->playing = true;
   p->paused = false;
   p->position_frames = 0;
   p->cached_duration_sec = duration;
   pthread_mutex_unlock(&p->state_mutex);
   if (ticket_out) {
      *ticket_out = ticket;
   }

   if (music_player_start_thread(p) != 0) {
      pthread_mutex_lock(&p->state_mutex);
      if (p->pending_seq == ticket) {
         p->pending_path[0] = '\0';
         p->loading = false;
         p->playing = false;
         publish_open(p, ticket, MUSIC_OPEN_FAILED);
      }
      pthread_mutex_unlock(&p->state_mutex);
      return FAILURE;
   }
   return SUCCESS;
}

music_resume_t music_player_resume_or_start(music_player_t *p) {
   pthread_mutex_lock(&p->state_mutex);
   bool loaded = p->decoder != NULL || p->pending_path[0] != '\0';
   if (p->paused && loaded) {
      p->paused = false;
      pthread_mutex_unlock(&p->state_mutex);
      return MUSIC_RESUME_RESUMED;
   }
   bool playing = p->playing;
   pthread_mutex_unlock(&p->state_mutex);
   if (playing && loaded) {
      return MUSIC_RESUME_UNCHANGED;
   }

   /* Stopped, or nothing loaded (the current track was removed or failed),
    * with a populated queue: start the current queue track.  Lock order:
    * queue, then state; both released before music_player_play(), which
    * takes them itself. */
   user_music_queue_t *uq = p->shared_queue;
   char play_path[MUSIC_PATH_MAX] = { 0 };
   pthread_mutex_lock(&uq->queue_mutex);
   int q_len = uq->queue_length;
   pthread_mutex_lock(&p->state_mutex);
   if (q_len > 0) {
      if (p->pos.index < 0 || p->pos.index >= q_len) {
         p->pos.index = 0;
      }
      snprintf(play_path, sizeof(play_path), "%s", uq->queue[p->pos.index].path);
   }
   pthread_mutex_unlock(&p->state_mutex);
   pthread_mutex_unlock(&uq->queue_mutex);

   if (!play_path[0]) {
      return MUSIC_RESUME_UNCHANGED; /* empty queue: nothing to start */
   }
   return music_player_play(p, play_path, -1, NULL) == SUCCESS ? MUSIC_RESUME_STARTED
                                                               : MUSIC_RESUME_START_FAILED;
}

int music_player_remove(music_player_t *p, int index0, bool *start_failed) {
   user_music_queue_t *uq = p->shared_queue;
   *start_failed = false;

   pthread_mutex_lock(&uq->queue_mutex);
   pthread_mutex_lock(&p->state_mutex);
   bool in_range = index0 >= 0 && index0 < uq->queue_length;
   /* playing covers the gap at a track's end, before the next one is picked;
    * a track the queue no longer holds is at no position */
   const music_queue_entry_t *now = NULL;
   bool removing_current = in_range && index0 == current(p, uq, &now) &&
                           (p->decoder || p->pending_path[0] || p->playing);
   bool was_playing = p->playing && !p->paused;
   bool gone = false; /* another player or tool call removed it meanwhile */
   char gone_next[MUSIC_PATH_MAX] = { 0 };
   if (removing_current) {
      char target[MUSIC_PATH_MAX];
      snprintf(target, sizeof(target), "%s", uq->queue[index0].path);
      pthread_mutex_unlock(&p->state_mutex);
      pthread_mutex_unlock(&uq->queue_mutex);
      /* Unload it before the queue moves under it (the thread reads it) */
      music_player_stop(p);
      pthread_mutex_lock(&uq->queue_mutex);
      pthread_mutex_lock(&p->state_mutex);
      /* The shared queue may have changed while it was unlocked: remove the
       * entry by what it is, not where it was (the copy nearest its old place,
       * if the path is queued twice) */
      index0 = music_queue_find(uq, target, index0);
      in_range = index0 >= 0;
      gone = !in_range;
      if (in_range) {
         p->pos.index = index0; /* the track that takes its place plays on */
      } else if (was_playing && p->pos.index < uq->queue_length) {
         /* Removed elsewhere meanwhile, and already unloaded here: play on with
          * the track now in its place (its place followed every edit meanwhile;
          * after a replace, the new queue's top) */
         snprintf(gone_next, sizeof(gone_next), "%s", uq->queue[p->pos.index].path);
      }
   }

   char next_path[MUSIC_PATH_MAX] = { 0 };
   if (in_range) {
      /* Every player's place follows the remove: one after it moves up */
      bool after_it = p->pos.index > index0;
      music_queue_remove_at(uq, index0);

      if (!after_it && removing_current && was_playing) {
         /* Play on from the track that took its place; past the end, follow
          * the end of the queue: wrap under repeat-all, else stop */
         if (index0 >= uq->queue_length && uq->repeat_mode == MUSIC_REPEAT_ALL) {
            p->pos.index = 0;
         }
         if (p->pos.index < uq->queue_length) {
            snprintf(next_path, sizeof(next_path), "%s", uq->queue[p->pos.index].path);
         } else {
            p->pos.index = 0;
         }
      } else if (!after_it && !p->track.path[0] && p->pos.index >= uq->queue_length) {
         /* No track here: keep a selection in range.  A track the queue lost
          * keeps its place past the end (nothing follows it). */
         p->pos.index = uq->queue_length > 0 ? uq->queue_length - 1 : 0;
      }
   }
   pthread_mutex_unlock(&p->state_mutex);
   if (in_range) {
      music_queue_changed(uq, true); /* off state_mutex: the thread takes it */
   }
   pthread_mutex_unlock(&uq->queue_mutex);
   int rc = in_range || gone ? SUCCESS : FAILURE;
   if (gone_next[0] && music_player_play(p, gone_next, -1, NULL) != SUCCESS) {
      *start_failed = true;
   }
   if (next_path[0] && music_player_play(p, next_path, -1, NULL) != SUCCESS) {
      *start_failed = true;
   }
   return rc;
}

void music_player_clear_queue(music_player_t *p, int *removed_out) {
   music_player_stop(p);

   user_music_queue_t *uq = p->shared_queue;
   pthread_mutex_lock(&uq->queue_mutex);
   if (removed_out) {
      *removed_out = uq->queue_length;
   }
   music_queue_clear(uq); /* every player's place goes to 0 */
   music_queue_changed(uq, true);
   pthread_mutex_unlock(&uq->queue_mutex);
}
