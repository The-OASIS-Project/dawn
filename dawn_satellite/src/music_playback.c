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
 * Music Playback Engine - Opus decode, PCM ring buffer, ALSA consumer thread
 *
 * Decodes Opus frames from the music WebSocket into a PCM ring buffer.
 * A consumer thread drains the buffer to ALSA at 48kHz stereo with volume scaling.
 */

#include "music_playback.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <opus/opus.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "logging.h"

/* Ring buffer: ~2.7 seconds at 48kHz stereo.
 * Must be power-of-two for lock-free SPSC masking (head & mask). */
#define RING_SAMPLES 262144 /* 2^18 = 131072 frames ≈ 2.73s at 48kHz stereo */
#define RING_BYTES (RING_SAMPLES * sizeof(int16_t))

/* Start playback after 500ms of data buffered (24000 frames) for network jitter tolerance */
#define BUFFER_THRESHOLD_FRAMES 24000

/* Max Opus frame: 960 samples per channel at 48kHz/20ms */
#define OPUS_MAX_FRAME_SAMPLES 960
#define OPUS_DECODE_BUF_SAMPLES (OPUS_MAX_FRAME_SAMPLES * 2) /* stereo */

struct music_playback {
   OpusDecoder *decoder;
   int16_t *ring;              /* PCM ring buffer (stereo interleaved, heap) */
   atomic_size_t ring_head;    /* Next write position — only producer writes */
   atomic_size_t ring_tail;    /* Next read position — only consumer writes */
   size_t ring_size;           /* Capacity (in stereo samples, power-of-two) */
   size_t ring_mask;           /* ring_size - 1 for fast modulo */
   atomic_int state;           /* music_pb_state_t */
   atomic_int volume;          /* 0-100, default 80 */
   audio_playback_t *audio;    /* Shared ALSA context (not owned) */
   pthread_mutex_t mutex;      /* Only for condvar waits (not data transfer) */
   pthread_cond_t data_ready;  /* Consumer waits for data */
   pthread_cond_t space_ready; /* Producer waits for space */
   pthread_t thread;
   volatile bool running;
   atomic_int stop_flag;          /* For audio_playback_play_stereo */
   atomic_bool paused_ack;        /* Consumer sets true when it reaches pause idle */
   pthread_cond_t pause_ack_cond; /* Signalled when consumer acknowledges pause */
};

/* =============================================================================
 * Lock-Free SPSC Ring Buffer
 *
 * Single-producer (lws thread) / single-consumer (ALSA thread).
 * Count is derived from head - tail (no shared counter).
 * Producer only writes ring_head; consumer only writes ring_tail.
 * The memcpy regions never overlap, so no mutex is needed for data transfer.
 * ============================================================================= */

static inline size_t ring_count(music_playback_t *ctx) {
   size_t head = atomic_load_explicit(&ctx->ring_head, memory_order_acquire);
   size_t tail = atomic_load_explicit(&ctx->ring_tail, memory_order_acquire);
   return (head - tail) & ctx->ring_mask;
}

static inline size_t ring_free(music_playback_t *ctx) {
   /* Reserve one slot so head never catches tail (disambiguates full vs empty) */
   return ctx->ring_size - 1 - ring_count(ctx);
}

/** @brief Producer: append samples. Caller must ensure ring_free() >= n_samples. */
static void ring_write(music_playback_t *ctx, const int16_t *data, size_t n_samples) {
   size_t head = atomic_load_explicit(&ctx->ring_head, memory_order_relaxed);
   size_t pos = head & ctx->ring_mask;
   size_t first = ctx->ring_size - pos;
   if (first > n_samples)
      first = n_samples;
   memcpy(ctx->ring + pos, data, first * sizeof(int16_t));
   if (n_samples > first) {
      memcpy(ctx->ring, data + first, (n_samples - first) * sizeof(int16_t));
   }
   /* Release: ensure memcpy is visible before consumer sees updated head */
   atomic_store_explicit(&ctx->ring_head, head + n_samples, memory_order_release);
}

/** @brief Consumer: read samples. Caller must ensure ring_count() >= n_samples. */
static void ring_read(music_playback_t *ctx, int16_t *out, size_t n_samples) {
   size_t tail = atomic_load_explicit(&ctx->ring_tail, memory_order_relaxed);
   size_t pos = tail & ctx->ring_mask;
   size_t first = ctx->ring_size - pos;
   if (first > n_samples)
      first = n_samples;
   memcpy(out, ctx->ring + pos, first * sizeof(int16_t));
   if (n_samples > first) {
      memcpy(out + first, ctx->ring, (n_samples - first) * sizeof(int16_t));
   }
   /* Release: ensure memcpy is done before producer sees updated tail */
   atomic_store_explicit(&ctx->ring_tail, tail + n_samples, memory_order_release);
}

/* =============================================================================
 * Consumer Thread — drains ring buffer to ALSA
 * ============================================================================= */

static void *consumer_thread(void *arg) {
   music_playback_t *ctx = (music_playback_t *)arg;
   size_t period_frames = ctx->audio->period_size;
   size_t period_samples = period_frames * 2; /* stereo */
   int16_t *pcm_buf = malloc(period_samples * sizeof(int16_t));

   if (!pcm_buf) {
      LOG_ERROR("Music playback: failed to allocate PCM buffer");
      return NULL;
   }

   /* Prepare ALSA once for continuous streaming */
   pthread_mutex_lock(&ctx->audio->alsa_mutex);
   snd_pcm_prepare((snd_pcm_t *)ctx->audio->handle);
   pthread_mutex_unlock(&ctx->audio->alsa_mutex);

   LOG_INFO("Music playback consumer thread started (period=%zu frames)", period_frames);

   while (ctx->running) {
      /* Fast path: check for data without locking */
      int st = atomic_load(&ctx->state);
      size_t avail = ring_count(ctx);

      if (st == MUSIC_PB_PLAYING && avail >= period_samples) {
         /* Data available — read lock-free and skip straight to ALSA write */
         ring_read(ctx, pcm_buf, period_samples);
         /* Wake producer if it was waiting for space (non-blocking signal) */
         pthread_mutex_lock(&ctx->mutex);
         pthread_cond_signal(&ctx->space_ready);
         pthread_mutex_unlock(&ctx->mutex);
         goto write_alsa;
      }

      /* Slow path: need to wait (idle, paused, buffering, or starved) */
      pthread_mutex_lock(&ctx->mutex);
      while (ctx->running) {
         st = atomic_load(&ctx->state);
         if (st == MUSIC_PB_IDLE) {
            pthread_cond_wait(&ctx->data_ready, &ctx->mutex);
            continue;
         }
         if (st == MUSIC_PB_PAUSED) {
            if (!atomic_load(&ctx->paused_ack)) {
               atomic_store(&ctx->paused_ack, true);
               pthread_cond_signal(&ctx->pause_ack_cond);
            }
            pthread_cond_wait(&ctx->data_ready, &ctx->mutex);
            continue;
         }
         if (st == MUSIC_PB_BUFFERING) {
            if (ring_count(ctx) < (size_t)(BUFFER_THRESHOLD_FRAMES * 2)) {
               pthread_cond_wait(&ctx->data_ready, &ctx->mutex);
               continue;
            }
            atomic_store(&ctx->state, MUSIC_PB_PLAYING);
            LOG_INFO("Music playback: buffering complete, starting playback");
         }
         /* PLAYING — recheck count (producer may have written while we waited) */
         if (ring_count(ctx) >= period_samples) {
            break;
         }
         pthread_cond_wait(&ctx->data_ready, &ctx->mutex);
      }

      if (!ctx->running) {
         pthread_mutex_unlock(&ctx->mutex);
         break;
      }

      /* Read under mutex (we were waiting, so this is already the slow path) */
      ring_read(ctx, pcm_buf, period_samples);
      pthread_cond_signal(&ctx->space_ready);
      pthread_mutex_unlock(&ctx->mutex);

write_alsa:

      /* Apply volume scaling */
      int vol = atomic_load(&ctx->volume);
      if (vol < 100) {
         for (size_t i = 0; i < period_samples; i++) {
            int32_t s = (int32_t)pcm_buf[i] * vol / 100;
            if (s > 32767)
               s = 32767;
            if (s < -32768)
               s = -32768;
            pcm_buf[i] = (int16_t)s;
         }
      }

      /* Write to ALSA (also updates spectrum/amplitude for visualizer) */
      audio_playback_play_stereo(ctx->audio, pcm_buf, period_frames, &ctx->stop_flag);
   }

   free(pcm_buf);
   LOG_INFO("Music playback consumer thread stopped");
   return NULL;
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

   /* Create Opus decoder: 48kHz stereo */
   int opus_err;
   ctx->decoder = opus_decoder_create(48000, 2, &opus_err);
   if (opus_err != OPUS_OK || !ctx->decoder) {
      LOG_ERROR("Music playback: opus_decoder_create failed: %s", opus_strerror(opus_err));
      free(ctx);
      return NULL;
   }

   /* Allocate ring buffer (power-of-two for lock-free masking) */
   ctx->ring_size = RING_SAMPLES;
   ctx->ring_mask = RING_SAMPLES - 1;
   ctx->ring = malloc(RING_BYTES);
   if (!ctx->ring) {
      LOG_ERROR("Music playback: ring buffer allocation failed (%zu bytes)", (size_t)RING_BYTES);
      opus_decoder_destroy(ctx->decoder);
      free(ctx);
      return NULL;
   }

   ctx->audio = audio;
   atomic_store(&ctx->state, MUSIC_PB_IDLE);
   atomic_store(&ctx->volume, 80);
   atomic_store(&ctx->stop_flag, 0);

   pthread_mutex_init(&ctx->mutex, NULL);
   pthread_cond_init(&ctx->data_ready, NULL);
   pthread_cond_init(&ctx->space_ready, NULL);
   pthread_cond_init(&ctx->pause_ack_cond, NULL);

   ctx->running = true;
   if (pthread_create(&ctx->thread, NULL, consumer_thread, ctx) != 0) {
      LOG_ERROR("Music playback: failed to create consumer thread");
      free(ctx->ring);
      opus_decoder_destroy(ctx->decoder);
      pthread_mutex_destroy(&ctx->mutex);
      pthread_cond_destroy(&ctx->data_ready);
      pthread_cond_destroy(&ctx->space_ready);
      pthread_cond_destroy(&ctx->pause_ack_cond);
      free(ctx);
      return NULL;
   }

   LOG_INFO("Music playback engine created (ring=%zu KB, threshold=%d ms)", RING_BYTES / 1024,
            BUFFER_THRESHOLD_FRAMES * 1000 / 48000);
   return ctx;
}

void music_playback_destroy(music_playback_t *ctx) {
   if (!ctx)
      return;

   /* Stop consumer thread */
   ctx->running = false;
   atomic_store(&ctx->stop_flag, 1);
   pthread_cond_broadcast(&ctx->data_ready);
   pthread_cond_broadcast(&ctx->space_ready);
   pthread_cond_broadcast(&ctx->pause_ack_cond);
   pthread_join(ctx->thread, NULL);

   opus_decoder_destroy(ctx->decoder);
   free(ctx->ring);
   pthread_mutex_destroy(&ctx->mutex);
   pthread_cond_destroy(&ctx->data_ready);
   pthread_cond_destroy(&ctx->space_ready);
   pthread_cond_destroy(&ctx->pause_ack_cond);
   free(ctx);

   LOG_INFO("Music playback engine destroyed");
}

int music_playback_push_opus(music_playback_t *ctx, const uint8_t *opus_data, int opus_len) {
   if (!ctx || !opus_data || opus_len <= 0)
      return -1;

   /* Silently drop frames while paused — the ring buffer isn't draining
    * so we'd just time out waiting for space and log false "decode errors". */
   int st = atomic_load(&ctx->state);
   if (st == MUSIC_PB_PAUSED)
      return 0; /* 0 = dropped, not an error */

   /* Decode Opus frame to PCM */
   int16_t pcm[OPUS_DECODE_BUF_SAMPLES];
   int frames = opus_decode(ctx->decoder, opus_data, opus_len, pcm, OPUS_MAX_FRAME_SAMPLES, 0);
   if (frames <= 0) {
      LOG_WARNING("Music playback: opus_decode error: %s", opus_strerror(frames));
      return -1;
   }

   size_t n_samples = (size_t)frames * 2; /* stereo */

   /* Fast path: write lock-free if space is available */
   if (ring_free(ctx) >= n_samples) {
      ring_write(ctx, pcm, n_samples);

      st = atomic_load(&ctx->state);
      if (st == MUSIC_PB_IDLE) {
         atomic_store(&ctx->state, MUSIC_PB_BUFFERING);
      }

      /* Wake consumer if it was waiting for data (non-blocking signal) */
      pthread_mutex_lock(&ctx->mutex);
      pthread_cond_signal(&ctx->data_ready);
      pthread_mutex_unlock(&ctx->mutex);
      return frames;
   }

   /* Slow path: wait for space with timeout to avoid blocking the LWS service
    * thread indefinitely (e.g., during TTS pause when consumer stops draining). */
   pthread_mutex_lock(&ctx->mutex);
   while (ring_free(ctx) < n_samples && ctx->running) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 100 * 1000 * 1000; /* 100ms timeout */
      if (ts.tv_nsec >= 1000000000) {
         ts.tv_sec += 1;
         ts.tv_nsec -= 1000000000;
      }
      int rc = pthread_cond_timedwait(&ctx->space_ready, &ctx->mutex, &ts);
      if (rc == ETIMEDOUT && ring_free(ctx) < n_samples) {
         pthread_mutex_unlock(&ctx->mutex);
         return -1;
      }
   }

   if (!ctx->running) {
      pthread_mutex_unlock(&ctx->mutex);
      return -1;
   }

   ring_write(ctx, pcm, n_samples);

   st = atomic_load(&ctx->state);
   if (st == MUSIC_PB_IDLE) {
      atomic_store(&ctx->state, MUSIC_PB_BUFFERING);
   }

   pthread_cond_signal(&ctx->data_ready);
   pthread_mutex_unlock(&ctx->mutex);

   return frames;
}

void music_playback_stop(music_playback_t *ctx) {
   if (!ctx)
      return;

   pthread_mutex_lock(&ctx->mutex);
   atomic_store(&ctx->state, MUSIC_PB_IDLE);
   atomic_store_explicit(&ctx->ring_head, 0, memory_order_release);
   atomic_store_explicit(&ctx->ring_tail, 0, memory_order_release);
   opus_decoder_ctl(ctx->decoder, OPUS_RESET_STATE);
   pthread_cond_signal(&ctx->data_ready);
   pthread_mutex_unlock(&ctx->mutex);

   LOG_INFO("Music playback stopped");
}

void music_playback_flush(music_playback_t *ctx) {
   if (!ctx)
      return;

   pthread_mutex_lock(&ctx->mutex);
   atomic_store_explicit(&ctx->ring_head, 0, memory_order_release);
   atomic_store_explicit(&ctx->ring_tail, 0, memory_order_release);
   opus_decoder_ctl(ctx->decoder, OPUS_RESET_STATE);
   pthread_cond_signal(&ctx->space_ready);
   pthread_mutex_unlock(&ctx->mutex);
}

void music_playback_pause(music_playback_t *ctx) {
   if (!ctx)
      return;

   int st = atomic_load(&ctx->state);
   if (st == MUSIC_PB_PLAYING || st == MUSIC_PB_BUFFERING) {
      pthread_mutex_lock(&ctx->mutex);
      atomic_store(&ctx->paused_ack, false);
      atomic_store(&ctx->state, MUSIC_PB_PAUSED);
      pthread_cond_signal(&ctx->data_ready); /* Wake consumer to see new state */

      /* Wait for consumer to acknowledge pause (max 200ms) */
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 200 * 1000 * 1000;
      if (ts.tv_nsec >= 1000000000) {
         ts.tv_sec += 1;
         ts.tv_nsec -= 1000000000;
      }
      while (!atomic_load(&ctx->paused_ack) && ctx->running) {
         if (pthread_cond_timedwait(&ctx->pause_ack_cond, &ctx->mutex, &ts) == ETIMEDOUT)
            break;
      }
      pthread_mutex_unlock(&ctx->mutex);
      LOG_INFO("Music playback paused");
   }
}

void music_playback_resume(music_playback_t *ctx) {
   if (!ctx)
      return;

   if (atomic_load(&ctx->state) == MUSIC_PB_PAUSED) {
      /* Re-prepare ALSA since TTS may have called snd_pcm_prepare during pause */
      pthread_mutex_lock(&ctx->audio->alsa_mutex);
      snd_pcm_prepare((snd_pcm_t *)ctx->audio->handle);
      pthread_mutex_unlock(&ctx->audio->alsa_mutex);

      atomic_store(&ctx->paused_ack, false);
      atomic_store(&ctx->state, MUSIC_PB_PLAYING);
      pthread_cond_signal(&ctx->data_ready);
      LOG_INFO("Music playback resumed");
   }
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

   size_t ring_frames = ring_count(ctx) / 2; /* stereo samples → frames */

   long alsa_frames = audio_playback_get_delay_frames(ctx->audio);

   long total_frames = (long)ring_frames + alsa_frames;
   return (int)(total_frames * 1000 / 48000);
}
