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
 * Music player: its thread (decode the playing track, hand it to the
 * output), its lifecycle and the controls that need no track open.  Opening
 * tracks and choosing what plays next are in music_player_track.c.
 */

#include "audio/music_player.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "audio/audio_decoder.h"
#include "audio/music_player_internal.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/time_utils.h"

/** Position update interval in milliseconds */
#define POSITION_UPDATE_INTERVAL_MS 1000

/** Players whose thread is running */
static atomic_int s_running_players = 0;

/* =============================================================================
 * Stream thread: decodes the playing track and hands it to the output
 * ============================================================================= */

/**
 * @brief The player's thread
 *
 * Opens the pending track, applies a seek, reads the decoder and writes what
 * it read to the output; at a track's end it picks the next one.  Every
 * output function runs here (see music_output_ops_t).
 */
static void *music_stream_thread(void *arg) {
   music_player_t *p = (music_player_t *)arg;

   OLOG_INFO("Music player: Streaming thread started");
   atomic_fetch_add(&s_running_players, 1);

   /* One read's frames, room to expand mono to stereo in place */
   int16_t decode_buffer[MUSIC_PLAYER_READ_FRAMES_MAX * 2];
   bool was_sending = false; /* Diagnostic: track state transitions for logging */
   int failures = 0;         /* Tracks in a row that produced no audio */

   while (!atomic_load(&p->stop_requested)) {
      /* Quick check without lock first */
      if (!atomic_load(&p->streaming)) {
         usleep(50000);
         continue;
      }

      /* A requested or auto-advanced track waiting to be opened.  Once one is
       * installed it counts as sending, so a pause or stop before its first
       * write still halts the output (which may have opened its device for
       * it); a natural advance was sending already, so no halt between tracks */
      if (music_player_run_pending_open(p, &failures)) {
         pthread_mutex_lock(&p->state_mutex);
         was_sending = was_sending || p->decoder != NULL;
         pthread_mutex_unlock(&p->state_mutex);
         continue;
      }

      pthread_mutex_lock(&p->state_mutex);

      /* A seek, applied here between frames (paused too), so it never stops
       * this thread or what it's prefetching */
      if (p->seek_pending) {
         p->seek_pending = false;
         if (p->decoder && audio_decoder_seek(p->decoder, p->seek_frames) == 0) {
            p->position_frames = p->seek_frames;
         }
         pthread_mutex_unlock(&p->state_mutex);
         /* What's buffered is stale after the seek */
         p->ops->halt(p->ops_ctx, MUSIC_HALT_FLUSH);
         music_event_t ev = { .type = MUSIC_EVENT_STATE };
         p->ops->notify(p->ops_ctx, &ev);
         continue;
      }

      if (!p->decoder || !p->playing || p->paused) {
         if (was_sending) {
            OLOG_INFO("Music player: Streaming paused (decoder=%p playing=%d paused=%d)",
                      (void *)p->decoder, p->playing, p->paused);
            was_sending = false;
            music_halt_t kind = p->paused && p->decoder ? MUSIC_HALT_PAUSE : MUSIC_HALT_IDLE;
            pthread_mutex_unlock(&p->state_mutex);
            p->ops->halt(p->ops_ctx, kind);
         } else {
            pthread_mutex_unlock(&p->state_mutex);
         }
         usleep(50000); /* 50ms idle sleep */
         continue;
      }
      if (!was_sending) {
         OLOG_INFO("Music player: Streaming resumed");
         was_sending = true;
      }

      /* Grab what the read needs */
      audio_decoder_t *decoder = p->decoder;
      size_t want = p->read_frames;
      pthread_mutex_unlock(&p->state_mutex);

      /* Read from decoder WITHOUT holding mutex */
      ssize_t frames_read = audio_decoder_read(decoder, decode_buffer, want);

      if (frames_read <= 0) {
         /* End of track or error */
         pthread_mutex_lock(&p->state_mutex);
         OLOG_INFO("Music player: Track finished (read returned %zd)", frames_read);
         audio_decoder_close(p->decoder);
         p->decoder = NULL;
         bool no_audio = p->position_frames == 0;
         /* An auto-advance pick that produced nothing didn't play: previous
          * mustn't go back to it (as for one that wouldn't open) */
         const char *played = music_history_current(&p->history);
         if (no_audio && p->history_fresh && played && strcmp(played, p->track.path) == 0) {
            music_history_drop_current(&p->history);
         }
         pthread_mutex_unlock(&p->state_mutex);

         /* A track that produced no audio failed, whether it errored or hit
          * EOF at once (a truncated file): counting it is what stops a
          * repeat queue of them from spinning. */
         bool track_failed = frames_read < 0 || no_audio;
         if (track_failed) {
            OLOG_WARNING("Music player: Track failed (read returned %zd) %s", frames_read,
                         no_audio ? "before any audio" : "mid-track");
            if (no_audio) {
               failures++;
            }
            /* One toast per run of failures, not one per skipped track */
            if (!no_audio || failures == 1) {
               music_event_t ev = { .type = MUSIC_EVENT_ERROR,
                                    .error = MUSIC_ERROR_DECODE,
                                    .message = no_audio ? "Couldn't play this track"
                                                        : "Couldn't play the rest of this track" };
               p->ops->notify(p->ops_ctx, &ev);
            }
         }

         /* Picks the next track into the pending slot (opened above), or stops */
         music_player_advance(p, track_failed, &failures);
         continue;
      }
      failures = 0;

      pthread_mutex_lock(&p->state_mutex);
      p->position_frames += frames_read;
      pthread_mutex_unlock(&p->state_mutex);

      p->ops->write(p->ops_ctx, decode_buffer, (size_t)frames_read);

      /* Position about once a second */
      uint64_t now = get_time_ms();
      if (now - p->last_position_update_ms >= POSITION_UPDATE_INTERVAL_MS) {
         music_event_t ev = { .type = MUSIC_EVENT_POSITION,
                              .position_sec = p->source_rate > 0
                                                  ? (double)p->position_frames / p->source_rate
                                                  : 0.0,
                              .duration_sec = p->cached_duration_sec };
         p->ops->notify(p->ops_ctx, &ev);
         p->last_position_update_ms = now;
      }
   }

   /* A request still waiting resolves as cancelled, so a waiting tool never
    * hangs; the slot itself stays for whoever stopped the thread */
   music_player_cancel_pending_ticket(p);
   music_player_prefetch_stop(p);
   /* Stopped for a skip, a stop or a shutdown: what's buffered is stale */
   p->ops->halt(p->ops_ctx, MUSIC_HALT_FLUSH);
   OLOG_INFO("Music player: Streaming thread stopped");
   atomic_fetch_sub(&s_running_players, 1);
   atomic_store(&p->streaming, false);

   return NULL;
}

/**
 * @brief Start a player's thread
 *
 * Stack requirements: the thread's read buffer (~4 KB) plus whatever the
 * output's write() puts on the stack (the WebUI output's ~20 KB).  The default
 * pthread stack (2-8 MB on Linux) is enough.
 */
int music_player_start_thread(music_player_t *p) {
   if (atomic_load(&p->streaming)) {
      return SUCCESS; /* Already running */
   }

   atomic_store(&p->stop_requested, false);
   atomic_store(&p->streaming, true);

   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

   int ret = pthread_create(&p->stream_thread, &attr, music_stream_thread, p);
   pthread_attr_destroy(&attr);

   if (ret != 0) {
      OLOG_ERROR("Music player: Failed to create streaming thread");
      atomic_store(&p->streaming, false);
      return FAILURE;
   }

   return SUCCESS;
}

/**
 * @brief Stop a player's thread
 */
void music_player_stop_thread(music_player_t *p) {
   if (!atomic_load(&p->streaming)) {
      return;
   }

   atomic_store(&p->stop_requested, true);
   /* The stream thread joins its prefetch helper on the way out: cancel the
    * helper's download too, so this join stays short */
   atomic_store(&p->prefetch_cancel, true);

   /* Wait for thread to finish */
   pthread_join(p->stream_thread, NULL);
}

/* =============================================================================
 * Player lifecycle and controls
 * ============================================================================= */

music_player_t *music_player_create(const music_output_ops_t *ops,
                                    void *ctx,
                                    user_music_queue_t *uq) {
   music_player_t *p = calloc(1, sizeof(*p));
   if (!p) {
      OLOG_ERROR("Music player: Failed to allocate a player");
      return NULL;
   }
   pthread_mutex_init(&p->state_mutex, NULL);
   pthread_mutex_init(&p->control_mutex, NULL);
   /* Monotonic, so a wall-clock step can't stretch the LLM tool's wait */
   pthread_condattr_t open_attr;
   pthread_condattr_init(&open_attr);
   pthread_condattr_setclock(&open_attr, CLOCK_MONOTONIC);
   pthread_cond_init(&p->open_cond, &open_attr);
   pthread_condattr_destroy(&open_attr);
   music_history_init(&p->history);
   atomic_store(&p->refs, 1);
   p->ops = ops;
   p->ops_ctx = ctx;
   p->shuffle_seed = (unsigned int)time(NULL) ^ (unsigned int)(uintptr_t)p;
   p->shared_queue = uq;
   music_queue_attach(uq, &p->pos); /* its place follows other players' edits */
   return p;
}

static void control_lock_raw(music_player_t *p, bool preempt);

void music_player_shutdown(music_player_t *p) {
   /* Let a control request already holding the lock (the LLM tool, waiting
    * for its track to open) finish first; the preempt ends its wait.  Then
    * stop the thread: after this, no output function runs. */
   atomic_store(&p->closing, true);
   control_lock_raw(p, true);
   music_player_stop_thread(p);
   p->closed = true;
   pthread_mutex_unlock(&p->control_mutex);
}

void music_player_ref(music_player_t *p) {
   atomic_fetch_add(&p->refs, 1);
}

/* Free a player nothing references any more */
static void destroy(music_player_t *p) {
   if (p->decoder) {
      audio_decoder_close(p->decoder);
   }
   if (p->prefetched) {
      audio_decoder_close(p->prefetched);
   }
   music_queue_detach(p->shared_queue, &p->pos);
   music_queue_release(p->shared_queue);
   pthread_cond_destroy(&p->open_cond);
   pthread_mutex_destroy(&p->state_mutex);
   pthread_mutex_destroy(&p->control_mutex);
   music_history_clear(&p->history);
   free(p);
}

void music_player_release(music_player_t *p) {
   if (p && atomic_fetch_sub(&p->refs, 1) == 1) {
      destroy(p);
   }
}

user_music_queue_t *music_player_queue(const music_player_t *p) {
   return p->shared_queue;
}

int music_player_running(void) {
   return atomic_load(&s_running_players);
}

static void control_lock_raw(music_player_t *p, bool preempt) {
   if (preempt) {
      atomic_store(&p->control_cancel, true);
      /* Wake an LLM tool call waiting for its track to open (it holds
       * control_mutex while it waits).  Under state_mutex, which the waiter
       * checks the flag under, so the wakeup can't be missed. */
      pthread_mutex_lock(&p->state_mutex);
      pthread_cond_broadcast(&p->open_cond);
      pthread_mutex_unlock(&p->state_mutex);
   }
   pthread_mutex_lock(&p->control_mutex);
   if (preempt) {
      /* Only one thread preempts, one request at a time, so this is
       * its own request being retired.  A non-preempting caller that gets the
       * lock first leaves it set: it doesn't wait for its track either, so the
       * waiting request isn't held up. */
      atomic_store(&p->control_cancel, false);
   }
}

bool music_player_control_lock(music_player_t *p, bool preempt) {
   control_lock_raw(p, preempt);
   if (p->closed) {
      pthread_mutex_unlock(&p->control_mutex);
      return false;
   }
   return true;
}

void music_player_control_unlock(music_player_t *p) {
   pthread_mutex_unlock(&p->control_mutex);
}

void music_player_notify_state(music_player_t *p) {
   music_event_t ev = { .type = MUSIC_EVENT_STATE };
   p->ops->notify(p->ops_ctx, &ev);
}

void music_player_notify_error(music_player_t *p, music_error_t error, const char *message) {
   music_event_t ev = { .type = MUSIC_EVENT_ERROR, .error = error, .message = message };
   p->ops->notify(p->ops_ctx, &ev);
}

bool music_player_set_paused(music_player_t *p, bool paused) {
   pthread_mutex_lock(&p->state_mutex);
   bool changed = paused ? p->playing && !p->paused : p->paused;
   if (changed) {
      p->paused = paused;
   }
   pthread_mutex_unlock(&p->state_mutex);
   return changed;
}

bool music_player_seek(music_player_t *p, double position_sec) {
   /* The thread applies it between reads and tells the listener.  A track
    * still opening has nothing to seek in yet. */
   pthread_mutex_lock(&p->state_mutex);
   if (p->cached_duration_sec > 0 && position_sec > p->cached_duration_sec) {
      position_sec = p->cached_duration_sec;
   }
   /* Bounded so the frame count can't overflow (also rejects NaN) */
   bool queued = p->decoder && p->source_rate > 0 && position_sec >= 0 &&
                 position_sec <= (double)UINT32_MAX && p->pending_path[0] == '\0';
   if (queued) {
      p->seek_frames = (uint64_t)(position_sec * p->source_rate);
      p->seek_pending = true;
   }
   pthread_mutex_unlock(&p->state_mutex);
   return queued;
}

bool music_player_interrupted(music_player_t *p) {
   pthread_mutex_lock(&p->state_mutex);
   bool interrupted = p->paused || !p->playing;
   pthread_mutex_unlock(&p->state_mutex);
   return interrupted || atomic_load(&p->stop_requested);
}
