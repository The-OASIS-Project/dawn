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
 * Music Playback Engine - Ring Buffer + LWS-Thread Drain
 *
 * Decodes Opus frames into a 2.7-second PCM ring buffer. The LWS service
 * thread drains the ring into ALSA via music_playback_drain(), called after
 * each lws_service() iteration. Both producer (push_opus) and consumer
 * (drain) run on the same LWS service thread — no concurrency on the hot
 * path. Cross-thread access (stop/pause/flush) only resets ring pointers
 * and state atomics.
 */

#include "music_playback.h"

#include <opus/opus.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"

/* Max Opus frame: 960 samples per channel at 48kHz/20ms */
#define OPUS_MAX_FRAME_SAMPLES 960
#define OPUS_DECODE_BUF_SAMPLES (OPUS_MAX_FRAME_SAMPLES * 2) /* stereo */

/* Ring buffer: 2.73s at 48kHz stereo (131072 frames x 2 channels = 262144 samples) */
#define RING_SIZE (1 << 18) /* 262144 int16_t samples (power-of-two) */
#define RING_MASK (RING_SIZE - 1)

_Static_assert((RING_SIZE & (RING_SIZE - 1)) == 0, "RING_SIZE must be power of two");
_Static_assert(sizeof(size_t) >= 8, "64-bit size_t required for free-running ring indices");

/* Max period size for drain stack buffer (must >= audio period_size) */
#define DRAIN_MAX_PERIOD 512
#define DRAIN_BUF_SAMPLES (DRAIN_MAX_PERIOD * 2) /* stereo */

struct music_playback {
   OpusDecoder *decoder;
   audio_playback_t *audio; /* Shared ALSA context (not owned) */
   atomic_int state;        /* music_pb_state_t */
   atomic_int volume;       /* 0-100 */
   atomic_int stop_flag;    /* Aborts in-progress play_stereo */

   int16_t *ring;           /* PCM ring buffer (stereo interleaved, heap) */
   atomic_size_t ring_head; /* Write position (free-running, wraps naturally) */
   atomic_size_t ring_tail; /* Read position (free-running, wraps naturally) */

   /* When true, a dedicated music_stream producer is active on its own thread.
    * The ws_client fallback path must not call push_opus concurrently. */
   atomic_bool dedicated_producer;
};

/* =============================================================================
 * Ring Buffer Helpers (single-threaded hot path — relaxed ordering)
 * ============================================================================= */

static inline size_t ring_count(music_playback_t *ctx) {
   size_t h = atomic_load_explicit(&ctx->ring_head, memory_order_relaxed);
   size_t t = atomic_load_explicit(&ctx->ring_tail, memory_order_relaxed);
   return h - t; /* unsigned subtraction handles wrap */
}

static inline size_t ring_free(music_playback_t *ctx) {
   return RING_SIZE - ring_count(ctx);
}

static void ring_write(music_playback_t *ctx, const int16_t *data, size_t n) {
   size_t h = atomic_load_explicit(&ctx->ring_head, memory_order_relaxed);
   size_t pos = h & RING_MASK;
   size_t first = RING_SIZE - pos;
   if (first >= n) {
      memcpy(&ctx->ring[pos], data, n * sizeof(int16_t));
   } else {
      memcpy(&ctx->ring[pos], data, first * sizeof(int16_t));
      memcpy(&ctx->ring[0], data + first, (n - first) * sizeof(int16_t));
   }
   atomic_store_explicit(&ctx->ring_head, h + n, memory_order_relaxed);
}

static void ring_read(music_playback_t *ctx, int16_t *data, size_t n) {
   size_t t = atomic_load_explicit(&ctx->ring_tail, memory_order_relaxed);
   size_t pos = t & RING_MASK;
   size_t first = RING_SIZE - pos;
   if (first >= n) {
      memcpy(data, &ctx->ring[pos], n * sizeof(int16_t));
   } else {
      memcpy(data, &ctx->ring[pos], first * sizeof(int16_t));
      memcpy(data + first, &ctx->ring[0], (n - first) * sizeof(int16_t));
   }
   atomic_store_explicit(&ctx->ring_tail, t + n, memory_order_relaxed);
}

/* =============================================================================
 * Public API
 * ============================================================================= */

music_playback_t *music_playback_create(audio_playback_t *audio) {
   if (!audio || !audio->initialized) {
      LOG_ERROR("Music playback: audio context not initialized");
      return NULL;
   }

   music_playback_t *ctx = calloc(1, sizeof(music_playback_t));
   if (!ctx)
      return NULL;

   int opus_err;
   ctx->decoder = opus_decoder_create(48000, 2, &opus_err);
   if (opus_err != OPUS_OK || !ctx->decoder) {
      LOG_ERROR("Music playback: opus_decoder_create failed: %s", opus_strerror(opus_err));
      free(ctx);
      return NULL;
   }

   ctx->ring = malloc(RING_SIZE * sizeof(int16_t));
   if (!ctx->ring) {
      LOG_ERROR("Music playback: failed to allocate ring buffer (%zu KB)",
                RING_SIZE * sizeof(int16_t) / 1024);
      opus_decoder_destroy(ctx->decoder);
      free(ctx);
      return NULL;
   }

   ctx->audio = audio;
   atomic_store(&ctx->state, MUSIC_PB_IDLE);
   atomic_store(&ctx->volume, 80);
   atomic_store(&ctx->stop_flag, 0);
   atomic_store(&ctx->ring_head, 0);
   atomic_store(&ctx->ring_tail, 0);
   atomic_store(&ctx->dedicated_producer, false);

   LOG_INFO("Music playback engine created (ring buffer + LWS drain, %.1fs capacity)",
            (float)RING_SIZE / (48000.0f * 2));
   return ctx;
}

void music_playback_destroy(music_playback_t *ctx) {
   if (!ctx)
      return;

   atomic_store(&ctx->stop_flag, 1);
   opus_decoder_destroy(ctx->decoder);
   free(ctx->ring);
   free(ctx);

   LOG_INFO("Music playback engine destroyed");
}

int music_playback_push_opus(music_playback_t *ctx, const uint8_t *opus_data, int opus_len) {
   if (!ctx || !opus_data || opus_len <= 0)
      return -1;

   int st = atomic_load(&ctx->state);
   if (st == MUSIC_PB_PAUSED)
      return 0;

   /* Decode Opus frame to stack buffer */
   int16_t pcm[OPUS_DECODE_BUF_SAMPLES];
   int frames = opus_decode(ctx->decoder, opus_data, opus_len, pcm, OPUS_MAX_FRAME_SAMPLES, 0);
   if (frames <= 0) {
      LOG_WARNING("Music playback: opus_decode error: %s", opus_strerror(frames));
      return -1;
   }

   size_t n_samples = (size_t)frames * 2; /* stereo interleaved */

   /* Check ring space — drop frame if full (never block) */
   if (ring_free(ctx) < n_samples) {
      LOG_WARNING("Music playback: ring buffer full, dropping frame (%d frames)", frames);
      return 0;
   }

   ring_write(ctx, pcm, n_samples);

   /* First frame auto-starts playback */
   if (st == MUSIC_PB_IDLE) {
      audio_playback_prepare(ctx->audio); /* ALSA may need re-prepare after TTS */
      atomic_store(&ctx->state, MUSIC_PB_PLAYING);
   }

   return frames;
}

void music_playback_drain(music_playback_t *ctx) {
   if (!ctx || atomic_load(&ctx->state) != MUSIC_PB_PLAYING)
      return;

   long alsa_avail = audio_playback_get_avail_frames(ctx->audio);
   size_t ring_samples = ring_count(ctx);
   size_t ring_frames = ring_samples / 2; /* stereo: 2 samples per frame */

   size_t n = (size_t)alsa_avail < ring_frames ? (size_t)alsa_avail : ring_frames;

   size_t period = ctx->audio->period_size;
   if (period == 0 || period > DRAIN_MAX_PERIOD)
      period = DRAIN_MAX_PERIOD;

   if (n < period)
      return;

   /* Volume scaling: pre-compute Q15 multiplier */
   int vol = atomic_load(&ctx->volume);
   int vol_fp = (vol << 15) / 100;
   bool scale = (vol < 100);

   /* Drain in period-sized chunks — never exceeds what ALSA can accept */
   int16_t pcm[DRAIN_BUF_SAMPLES];
   while (n >= period) {
      if (atomic_load(&ctx->stop_flag))
         break;

      size_t samples = period * 2;
      ring_read(ctx, pcm, samples);

      if (scale) {
         for (size_t i = 0; i < samples; i++)
            pcm[i] = (int16_t)(((int32_t)pcm[i] * vol_fp) >> 15);
      }

      audio_playback_play_stereo(ctx->audio, pcm, period, &ctx->stop_flag);
      n -= period;
   }
}

void music_playback_stop(music_playback_t *ctx) {
   if (!ctx)
      return;

   atomic_store(&ctx->state, MUSIC_PB_IDLE);
   atomic_store(&ctx->stop_flag, 1);
   audio_playback_stop(ctx->audio);

   /* Reset ring buffer */
   atomic_store_explicit(&ctx->ring_head, 0, memory_order_release);
   atomic_store_explicit(&ctx->ring_tail, 0, memory_order_release);

   atomic_store(&ctx->stop_flag, 0);
   opus_decoder_ctl(ctx->decoder, OPUS_RESET_STATE);

   LOG_INFO("Music playback stopped");
}

void music_playback_flush(music_playback_t *ctx) {
   if (!ctx)
      return;

   atomic_store(&ctx->stop_flag, 1);
   audio_playback_stop(ctx->audio);

   /* Reset ring buffer */
   atomic_store_explicit(&ctx->ring_head, 0, memory_order_release);
   atomic_store_explicit(&ctx->ring_tail, 0, memory_order_release);

   atomic_store(&ctx->stop_flag, 0);
   opus_decoder_ctl(ctx->decoder, OPUS_RESET_STATE);
}

void music_playback_pause(music_playback_t *ctx) {
   if (!ctx)
      return;

   int st = atomic_load(&ctx->state);
   if (st != MUSIC_PB_PLAYING && st != MUSIC_PB_BUFFERING)
      return;

   atomic_store(&ctx->stop_flag, 1);
   atomic_store(&ctx->state, MUSIC_PB_PAUSED);
   audio_playback_stop(ctx->audio);
   atomic_store(&ctx->stop_flag, 0);

   LOG_INFO("Music playback paused (ring data preserved)");
}

void music_playback_resume(music_playback_t *ctx) {
   if (!ctx)
      return;

   if (atomic_load(&ctx->state) != MUSIC_PB_PAUSED)
      return;

   audio_playback_prepare(ctx->audio);
   atomic_store(&ctx->state, MUSIC_PB_PLAYING);

   LOG_INFO("Music playback resumed");
}

void music_playback_set_volume(music_playback_t *ctx, int volume) {
   if (!ctx)
      return;
   if (volume < 0)
      volume = 0;
   if (volume > 100)
      volume = 100;
   atomic_store(&ctx->volume, volume);
}

int music_playback_get_volume(music_playback_t *ctx) {
   return ctx ? atomic_load(&ctx->volume) : 0;
}

music_pb_state_t music_playback_get_state(music_playback_t *ctx) {
   return ctx ? (music_pb_state_t)atomic_load(&ctx->state) : MUSIC_PB_IDLE;
}

bool music_playback_is_playing(music_playback_t *ctx) {
   return ctx && atomic_load(&ctx->state) == MUSIC_PB_PLAYING;
}

int music_playback_get_buffered_ms(music_playback_t *ctx) {
   if (!ctx)
      return 0;

   /* Ring buffer frames + ALSA output delay frames */
   size_t ring_frames = ring_count(ctx) / 2;
   long alsa_frames = audio_playback_get_delay_frames(ctx->audio);
   return (int)((ring_frames + (size_t)alsa_frames) * 1000 / 48000);
}

void music_playback_set_dedicated_producer(music_playback_t *ctx, bool active) {
   if (ctx)
      atomic_store(&ctx->dedicated_producer, active);
}

bool music_playback_has_dedicated_producer(music_playback_t *ctx) {
   return ctx && atomic_load(&ctx->dedicated_producer);
}
