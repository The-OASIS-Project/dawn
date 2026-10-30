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
 * The local speaker: a music player's output on the audio backend.  The
 * device opens when a track starts (or on the first write after a pause or
 * seek), stays open across a natural advance so tracks play without a gap,
 * and closes on every halt: a pause or the end drains it, a seek or a stop
 * drops it.  The blocking write paces the player.
 */

#include "audio/music_speaker.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "audio/audio_backend.h"
#include "audio/audio_converter.h"
#include "audio/audio_utils.h"
#include "audio/music_queue.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/time_utils.h"

/** Rates the speaker plays (the same bounds as the WebUI stream: a crafted
 *  file can declare any rate, and the resampler takes an int ratio) */
#define SPEAKER_MIN_SOURCE_RATE 8000
#define SPEAKER_MAX_SOURCE_RATE 768000

/** Device buffer, as the old per-track player used: ~85 ms at 48 kHz */
#define SPEAKER_PERIOD_FRAMES 1024
#define SPEAKER_BUFFER_FRAMES 4096

/** Volume while someone speaks, as a share of the set volume */
#define SPEAKER_DUCK_FACTOR 0.3f

/** At most one "can't open the device" log per this long */
#define SPEAKER_OPEN_LOG_MS 5000

/** How long an interruption's pause waits for the lock, then for the device
 *  to close */
#define SPEAKER_PAUSE_WAIT_MS 250

/* ---- The output (player's thread only, except the atomics) --------------- */

typedef struct {
   music_player_t *player;                 /**< Whose output this is (s_speaker_mutex) */
   audio_stream_playback_handle_t *handle; /**< Open device, or NULL */
   atomic_bool open;                       /**< handle is open (read anywhere) */
   unsigned hw_rate, hw_channels;          /**< What the device opened at */
   unsigned src_rate, src_channels;        /**< The track's format */
   audio_converter_t *converter;           /**< src → hw, NULL when not needed */
   bool converter_stale;                   /**< Rebuild before the next write */
   int16_t *out_buf;                       /**< Converted audio */
   size_t out_samples;                     /**< out_buf's size in samples */
   uint64_t last_open_log_ms;              /**< Throttles the open-failure log */
   uint64_t last_write_fail_ms;            /**< Throttles write-failure logs (and reopens') */
   unsigned device_gen;                    /**< s_device_gen when the device opened */
} speaker_output_t;

static speaker_output_t s_out;
static _Atomic float s_volume = 0.5f;
static atomic_bool s_ducked = false;

/* ---- The speaker (s_speaker_mutex) -------------------------------------- */

static pthread_mutex_t s_speaker_mutex = PTHREAD_MUTEX_INITIALIZER;
static music_player_t *s_speaker;
static int s_speaker_owner;
static bool s_cleaned;
static char s_device[256];

/* The device's name, switched at runtime (s_device_mutex, a LEAF: the
 * player's thread reads it at each open; s_device_gen tells it to reopen) */
static pthread_mutex_t s_device_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint s_device_gen;

void setMusicVolume(float val) {
   atomic_store(&s_volume, val < 0.0f ? 0.0f : val > 2.0f ? 2.0f : val);
}

float getMusicVolume(void) {
   return atomic_load(&s_volume);
}

void music_speaker_set_ducked(bool ducked) {
   atomic_store(&s_ducked, ducked);
}

bool music_speaker_is_playing(void) {
   return atomic_load(&s_out.open);
}

/* Open the device at the configured rate; false (logged, throttled) when it
 * won't open */
static bool device_open(speaker_output_t *o) {
   char device[sizeof(s_device)];
   pthread_mutex_lock(&s_device_mutex);
   snprintf(device, sizeof(device), "%s", s_device);
   o->device_gen = atomic_load(&s_device_gen);
   pthread_mutex_unlock(&s_device_mutex);
   audio_stream_params_t params = { .sample_rate = audio_conv_get_output_rate(),
                                    .channels = audio_conv_get_output_channels(),
                                    .format = AUDIO_FORMAT_S16_LE,
                                    .period_frames = SPEAKER_PERIOD_FRAMES,
                                    .buffer_frames = SPEAKER_BUFFER_FRAMES };
   audio_hw_params_t hw;
   memset(&hw, 0, sizeof(hw));
   o->handle = audio_stream_playback_open(device, &params, &hw);
   if (!o->handle) {
      uint64_t now = get_time_ms();
      if (now - o->last_open_log_ms >= SPEAKER_OPEN_LOG_MS || o->last_open_log_ms == 0) {
         OLOG_ERROR("Music speaker: Can't open playback device %s", device);
         o->last_open_log_ms = now;
      }
      return false;
   }
   if (hw.sample_rate != o->hw_rate || hw.channels != o->hw_channels) {
      o->converter_stale = true;
   }
   o->hw_rate = hw.sample_rate;
   o->hw_channels = hw.channels;
   atomic_store(&o->open, true);
   /* Quiet while a device that opens fails every write (one line per period) */
   if (get_time_ms() - o->last_write_fail_ms >= SPEAKER_OPEN_LOG_MS) {
      OLOG_INFO("Music speaker: Opened %s (%u Hz, %u ch, buffer %zu frames)", device,
                hw.sample_rate, hw.channels, hw.buffer_frames);
   }
   return true;
}

static void device_close(speaker_output_t *o, bool drain) {
   if (!o->handle) {
      return;
   }
   if (drain) {
      audio_stream_playback_drain(o->handle);
   } else {
      audio_stream_playback_drop(o->handle);
   }
   audio_stream_playback_close(o->handle);
   o->handle = NULL;
   atomic_store(&o->open, false);
   if (o->converter) {
      audio_converter_reset(o->converter); /* what follows doesn't continue this */
   }
}

/* Match the converter to the track and the device; false when it can't be
 * built */
static bool converter_update(speaker_output_t *o) {
   if (!o->converter_stale) {
      return true;
   }
   audio_converter_destroy(o->converter);
   o->converter = NULL;
   audio_converter_params_t in = { .sample_rate = o->src_rate, .channels = o->src_channels };
   if (audio_converter_needed_ex(&in, o->hw_rate, o->hw_channels)) {
      o->converter = audio_converter_create_ex(&in, o->hw_rate, o->hw_channels);
      if (!o->converter) {
         OLOG_ERROR("Music speaker: Can't convert %u Hz %u ch to %u Hz %u ch", o->src_rate,
                    o->src_channels, o->hw_rate, o->hw_channels);
         return false;
      }
      /* Sized per converter: a lower-rate track needs more room per read */
      size_t need = audio_converter_max_output_frames(o->converter, MUSIC_PLAYER_READ_FRAMES_MAX) *
                    o->hw_channels;
      if (need > o->out_samples) {
         int16_t *buf = realloc(o->out_buf, need * sizeof(int16_t));
         if (!buf) {
            OLOG_ERROR("Music speaker: Out of memory for the output buffer");
            return false;
         }
         o->out_buf = buf;
         o->out_samples = need;
      }
   }
   o->converter_stale = false;
   return true;
}

static int speaker_track_begin(void *ctx,
                               const audio_decoder_info_t *info,
                               bool gapless,
                               size_t *read_frames) {
   (void)gapless; /* the device stays open across a natural advance */
   speaker_output_t *o = ctx;
   if (info->sample_rate < SPEAKER_MIN_SOURCE_RATE || info->sample_rate > SPEAKER_MAX_SOURCE_RATE) {
      return FAILURE;
   }
   if (info->sample_rate != o->src_rate || info->channels != o->src_channels) {
      o->converter_stale = true;
   }
   o->src_rate = info->sample_rate;
   o->src_channels = info->channels;
   *read_frames = MUSIC_PLAYER_READ_FRAMES_MAX;
   /* Open now, so a device that won't open fails the track (the player's
    * rules then stop or skip); not while paused (a phone call may want it,
    * and a gapless advance paused during its open closes it here) */
   if (music_player_interrupted(o->player)) {
      device_close(o, false);
   } else if (!o->handle && !device_open(o)) {
      return FAILURE;
   }
   return SUCCESS;
}

/* Write all of buf, retrying a partial write and an underrun once.  On a
 * failure the device closes (the next write reopens it) and the rest of buf
 * takes its playing time, so the queue doesn't race on in silence. */
static void device_write(speaker_output_t *o, const int16_t *buf, size_t frames) {
   bool recovered = false;
   while (frames > 0) {
      ssize_t n = audio_stream_playback_write(o->handle, buf, frames);
      if (n > 0) {
         buf += (size_t)n * o->hw_channels;
         frames -= (size_t)n;
         continue;
      }
      int err = (int)(-n);
      if (n < 0 && err == AUDIO_ERR_UNDERRUN && !recovered) {
         audio_stream_playback_recover(o->handle, err);
         recovered = true;
         continue;
      }
      uint64_t now = get_time_ms();
      if (now - o->last_write_fail_ms >= SPEAKER_OPEN_LOG_MS) {
         OLOG_WARNING("Music speaker: Write failed (%s); reopening",
                      n < 0 ? audio_error_string((audio_error_t)err) : "no progress");
      }
      o->last_write_fail_ms = now;
      device_close(o, false); /* the next write reopens it */
      usleep((useconds_t)(frames * 1000000ULL / (o->hw_rate ? o->hw_rate : 48000)));
      return;
   }
}

static void speaker_write(void *ctx, int16_t *pcm, size_t frames) {
   speaker_output_t *o = ctx;
   if (o->handle && o->device_gen != atomic_load(&s_device_gen)) {
      device_close(o, false); /* the playback device was switched: move to it */
   }
   if (!o->handle && !device_open(o)) {
      /* Keep the player at playing speed, so the queue doesn't race on in
       * silence while the device is busy */
      usleep((useconds_t)(frames * 1000000ULL / (o->src_rate ? o->src_rate : 48000)));
      return;
   }
   if (!converter_update(o)) {
      usleep((useconds_t)(frames * 1000000ULL / o->src_rate));
      return;
   }
   float vol = atomic_load(&s_volume) * (atomic_load(&s_ducked) ? SPEAKER_DUCK_FACTOR : 1.0f);
   audio_apply_volume(pcm, frames, o->src_channels, vol);
   if (o->converter) {
      size_t out = 0;
      if (audio_converter_process(o->converter, pcm, frames, o->out_buf,
                                  o->out_samples / o->hw_channels, &out) != 0) {
         OLOG_WARNING("Music speaker: Conversion failed");
         usleep((useconds_t)(frames * 1000000ULL / o->src_rate)); /* at playing speed */
         return;
      }
      device_write(o, o->out_buf, out);
   } else {
      device_write(o, pcm, frames);
   }
}

static void speaker_halt(void *ctx, music_halt_t kind) {
   /* A pause or the end lets what's buffered play out (~85 ms); a seek or a
    * stop drops it */
   device_close(ctx, kind != MUSIC_HALT_FLUSH);
}

static void speaker_notify(void *ctx, const music_event_t *ev) {
   (void)ctx;
   /* The listener hears outcomes in the tool's reply; errors are logged */
   if (ev->type == MUSIC_EVENT_ERROR && ev->message) {
      OLOG_WARNING("Music speaker: %s", ev->message);
   }
}

static const music_output_ops_t s_speaker_ops = {
   .track_begin = speaker_track_begin,
   .write = speaker_write,
   .halt = speaker_halt,
   .notify = speaker_notify,
};

/* ---- The speaker -------------------------------------------------------- */

void music_speaker_set_device(const char *device) {
   pthread_mutex_lock(&s_device_mutex);
   snprintf(s_device, sizeof(s_device), "%s", device ? device : "");
   atomic_fetch_add(&s_device_gen, 1);
   pthread_mutex_unlock(&s_device_mutex);
}

/* Close the speaker's player (s_speaker_mutex held) */
static void speaker_close(void) {
   if (s_speaker) {
      music_player_shutdown(s_speaker);
      music_player_release(s_speaker);
      s_speaker = NULL;
      s_speaker_owner = 0;
   }
}

music_player_t *music_speaker_acquire(int owner_user_id) {
   pthread_mutex_lock(&s_speaker_mutex);
   pthread_mutex_lock(&s_device_mutex);
   bool no_device = !s_device[0];
   pthread_mutex_unlock(&s_device_mutex);
   if (s_cleaned || no_device || audio_backend_get_type() == AUDIO_BACKEND_NONE) {
      pthread_mutex_unlock(&s_speaker_mutex);
      return NULL;
   }
   if (s_speaker && s_speaker_owner != owner_user_id) {
      OLOG_INFO("Music speaker: Now playing for user %d (was %d)", owner_user_id, s_speaker_owner);
      speaker_close(); /* the old owner's music stops */
   }
   if (!s_speaker) {
      user_music_queue_t *uq = music_queue_acquire(owner_user_id);
      music_player_t *p = uq ? music_player_create(&s_speaker_ops, &s_out, uq) : NULL;
      if (!p) {
         if (uq) {
            music_queue_release(uq);
         }
         pthread_mutex_unlock(&s_speaker_mutex);
         OLOG_ERROR("Music speaker: Can't create the player");
         return NULL;
      }
      s_out.player = p; /* before its thread starts: the output reads it there */
      s_speaker = p;
      s_speaker_owner = owner_user_id;
   }
   music_player_t *p = s_speaker;
   music_player_ref(p);
   pthread_mutex_unlock(&s_speaker_mutex);
   return p;
}

bool music_speaker_pause(void) {
   /* Every speaker call holds the mutex briefly (the first one across a
    * queue load): retry within the budget rather than skip the pause */
   uint64_t deadline = get_time_ms() + SPEAKER_PAUSE_WAIT_MS;
   while (pthread_mutex_trylock(&s_speaker_mutex) != 0) {
      if (get_time_ms() >= deadline) {
         OLOG_WARNING("Music speaker: Busy; couldn't pause");
         return false;
      }
      usleep(5000);
   }
   music_player_t *p = s_speaker;
   if (p) {
      music_player_ref(p);
   }
   pthread_mutex_unlock(&s_speaker_mutex);
   if (!p) {
      return false;
   }
   bool paused = music_player_set_paused(p, true);
   /* Let the device close (one write, then a drain), so whoever needs it next
    * finds it free */
   while (paused && music_speaker_is_playing() && get_time_ms() < deadline) {
      usleep(5000);
   }
   music_player_release(p);
   return paused;
}

void music_speaker_cleanup(void) {
   pthread_mutex_lock(&s_speaker_mutex);
   s_cleaned = true;
   speaker_close(); /* the thread's last halt closes the device */
   audio_converter_destroy(s_out.converter);
   s_out.converter = NULL;
   free(s_out.out_buf);
   s_out.out_buf = NULL;
   s_out.out_samples = 0;
   pthread_mutex_unlock(&s_speaker_mutex);
}
