/*
 * DAWN Satellite - ALSA Audio Capture with Thread
 *
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
 * Uses a dedicated capture thread that reads from ALSA and writes to a
 * ring buffer. The main thread reads from the ring buffer (non-blocking).
 */

#include "audio_capture.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio/ring_buffer.h"

#define LOG_INFO(fmt, ...) fprintf(stdout, "[CAPTURE] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[CAPTURE ERROR] " fmt "\n", ##__VA_ARGS__)

/* Ring buffer size: 2 seconds of 16kHz mono audio */
#define RING_BUFFER_SIZE (AUDIO_SAMPLE_RATE * 2 * sizeof(int16_t))

/* Capture thread context */
struct audio_capture {
   char device[64];
   snd_pcm_t *handle;
   unsigned int sample_rate;
   unsigned int channels;
   size_t period_size;
   int initialized;

   /* Thread and ring buffer */
   pthread_t thread;
   atomic_bool running;
   ring_buffer_t *ring_buffer;
};

/**
 * @brief Capture thread function
 *
 * Continuously reads from ALSA and writes to ring buffer.
 */
static void *capture_thread_func(void *arg) {
   audio_capture_t *ctx = (audio_capture_t *)arg;

   /* Allocate capture buffer (one period) */
   size_t buffer_bytes = ctx->period_size * ctx->channels * sizeof(int16_t);
   int16_t *buffer = (int16_t *)malloc(buffer_bytes);
   if (!buffer) {
      LOG_ERROR("Failed to allocate capture buffer");
      atomic_store(&ctx->running, false);
      return NULL;
   }

   LOG_INFO("Capture thread started (period=%zu frames, buffer=%zu bytes)", ctx->period_size,
            buffer_bytes);

   static int debug_count = 0;
   while (atomic_load(&ctx->running)) {
      /* Read one period from ALSA (blocking) */
      snd_pcm_sframes_t frames = snd_pcm_readi(ctx->handle, buffer, ctx->period_size);

      if (frames > 0) {
         /* Write to ring buffer */
         size_t bytes = frames * ctx->channels * sizeof(int16_t);
         ring_buffer_write(ctx->ring_buffer, (char *)buffer, bytes);

         /* Debug: log periodically */
         debug_count++;
         if (debug_count % 100 == 0) {
            LOG_INFO("Captured %d frames (total %d periods)", (int)frames, debug_count);
         }
      } else if (frames < 0) {
         if (frames == -EPIPE) {
            /* Buffer overrun - recover */
            LOG_ERROR("Capture overrun, recovering...");
            snd_pcm_prepare(ctx->handle);
         } else if (frames == -EAGAIN) {
            /* No data available (shouldn't happen in blocking mode) */
            usleep(1000);
         } else if (frames == -ESTRPIPE) {
            /* Suspended - wait for resume */
            LOG_ERROR("Capture suspended, waiting...");
            while (snd_pcm_resume(ctx->handle) == -EAGAIN) {
               usleep(100000);
            }
            snd_pcm_prepare(ctx->handle);
         } else {
            LOG_ERROR("Read error: %s", snd_strerror(frames));
            usleep(10000);
         }
      }
   }

   free(buffer);
   LOG_INFO("Capture thread stopped");
   return NULL;
}

int audio_capture_init(audio_capture_t **ctx_out, const char *device) {
   if (!ctx_out)
      return -1;

   audio_capture_t *ctx = calloc(1, sizeof(audio_capture_t));
   if (!ctx)
      return -1;

   const char *dev = device ? device : AUDIO_DEFAULT_CAPTURE_DEVICE;
   strncpy(ctx->device, dev, sizeof(ctx->device) - 1);

   int err;

   /* Open PCM device for capture (blocking mode for thread) */
   err = snd_pcm_open(&ctx->handle, ctx->device, SND_PCM_STREAM_CAPTURE, 0);
   if (err < 0) {
      LOG_ERROR("Cannot open capture device '%s': %s", ctx->device, snd_strerror(err));
      free(ctx);
      return -1;
   }

   /* Set hardware parameters */
   snd_pcm_hw_params_t *hw_params;
   snd_pcm_hw_params_alloca(&hw_params);

   err = snd_pcm_hw_params_any(ctx->handle, hw_params);
   if (err < 0) {
      LOG_ERROR("Cannot initialize hw params: %s", snd_strerror(err));
      goto error;
   }

   /* Set access type - interleaved */
   err = snd_pcm_hw_params_set_access(ctx->handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
   if (err < 0) {
      LOG_ERROR("Cannot set access type: %s", snd_strerror(err));
      goto error;
   }

   /* Set format - 16-bit signed little-endian */
   err = snd_pcm_hw_params_set_format(ctx->handle, hw_params, SND_PCM_FORMAT_S16_LE);
   if (err < 0) {
      LOG_ERROR("Cannot set format: %s", snd_strerror(err));
      goto error;
   }

   /* Set channels - mono */
   err = snd_pcm_hw_params_set_channels(ctx->handle, hw_params, AUDIO_CHANNELS);
   if (err < 0) {
      LOG_ERROR("Cannot set channels: %s", snd_strerror(err));
      goto error;
   }

   /* Set sample rate */
   unsigned int rate = AUDIO_SAMPLE_RATE;
   err = snd_pcm_hw_params_set_rate_near(ctx->handle, hw_params, &rate, 0);
   if (err < 0) {
      LOG_ERROR("Cannot set sample rate: %s", snd_strerror(err));
      goto error;
   }

   if (rate != AUDIO_SAMPLE_RATE) {
      LOG_INFO("Requested %u Hz, got %u Hz", AUDIO_SAMPLE_RATE, rate);
   }
   ctx->sample_rate = rate;
   ctx->channels = AUDIO_CHANNELS;

   /* Set period size for low latency (~32ms at 16kHz) */
   snd_pcm_uframes_t period_size = 512;
   err = snd_pcm_hw_params_set_period_size_near(ctx->handle, hw_params, &period_size, 0);
   if (err < 0) {
      LOG_ERROR("Cannot set period size: %s", snd_strerror(err));
      goto error;
   }
   ctx->period_size = period_size;

   /* Set buffer size (4 periods) */
   snd_pcm_uframes_t buffer_size = period_size * 4;
   err = snd_pcm_hw_params_set_buffer_size_near(ctx->handle, hw_params, &buffer_size);
   if (err < 0) {
      LOG_ERROR("Cannot set buffer size: %s", snd_strerror(err));
      goto error;
   }

   /* Apply hardware parameters */
   err = snd_pcm_hw_params(ctx->handle, hw_params);
   if (err < 0) {
      LOG_ERROR("Cannot apply hw params: %s", snd_strerror(err));
      goto error;
   }

   /* Create ring buffer */
   ctx->ring_buffer = ring_buffer_create(RING_BUFFER_SIZE);
   if (!ctx->ring_buffer) {
      LOG_ERROR("Failed to create ring buffer");
      goto error;
   }

   /* Prepare the device */
   err = snd_pcm_prepare(ctx->handle);
   if (err < 0) {
      LOG_ERROR("Cannot prepare device: %s", snd_strerror(err));
      ring_buffer_free(ctx->ring_buffer);
      goto error;
   }

   /* Start capture thread */
   atomic_init(&ctx->running, true);
   if (pthread_create(&ctx->thread, NULL, capture_thread_func, ctx) != 0) {
      LOG_ERROR("Failed to create capture thread");
      ring_buffer_free(ctx->ring_buffer);
      goto error;
   }

   ctx->initialized = 1;

   LOG_INFO("Capture initialized: %s @ %u Hz, %zu frame periods (threaded)", ctx->device,
            ctx->sample_rate, ctx->period_size);
   LOG_INFO("Ring buffer at %p (capacity %zu bytes)", (void *)ctx->ring_buffer, RING_BUFFER_SIZE);

   *ctx_out = ctx;
   return 0;

error:
   snd_pcm_close(ctx->handle);
   free(ctx);
   return -1;
}

void audio_capture_cleanup(audio_capture_t *ctx) {
   if (!ctx)
      return;

   /* Stop capture thread */
   if (ctx->initialized) {
      atomic_store(&ctx->running, false);
      pthread_join(ctx->thread, NULL);
   }

   /* Close ALSA */
   if (ctx->handle) {
      snd_pcm_drop(ctx->handle);
      snd_pcm_close(ctx->handle);
   }

   /* Free ring buffer */
   if (ctx->ring_buffer) {
      ring_buffer_free(ctx->ring_buffer);
   }

   free(ctx);
   LOG_INFO("Capture cleaned up");
}

ssize_t audio_capture_read(audio_capture_t *ctx, int16_t *buffer, size_t max_samples) {
   if (!ctx || !ctx->initialized || !buffer || !ctx->ring_buffer) {
      return -1;
   }

   /* Read from ring buffer (non-blocking) */
   size_t bytes_wanted = max_samples * sizeof(int16_t);
   size_t bytes_read = ring_buffer_read(ctx->ring_buffer, (char *)buffer, bytes_wanted);

   return (ssize_t)(bytes_read / sizeof(int16_t));
}

size_t audio_capture_wait_for_data(audio_capture_t *ctx, size_t min_samples, int timeout_ms) {
   if (!ctx || !ctx->ring_buffer) {
      return 0;
   }

   size_t min_bytes = min_samples * sizeof(int16_t);
   size_t bytes = ring_buffer_wait_for_data(ctx->ring_buffer, min_bytes, timeout_ms);
   return bytes / sizeof(int16_t);
}

size_t audio_capture_bytes_available(audio_capture_t *ctx) {
   if (!ctx || !ctx->ring_buffer) {
      return 0;
   }
   return ring_buffer_bytes_available(ctx->ring_buffer);
}

void audio_capture_clear(audio_capture_t *ctx) {
   if (!ctx || !ctx->ring_buffer) {
      return;
   }
   ring_buffer_clear(ctx->ring_buffer);
}

int audio_create_wav(const int16_t *samples,
                     size_t num_samples,
                     uint8_t **wav_data,
                     size_t *wav_size) {
   if (!samples || !wav_data || !wav_size) {
      return -1;
   }

   size_t pcm_size = num_samples * sizeof(int16_t);
   size_t total_size = sizeof(wav_header_t) + pcm_size;

   uint8_t *buffer = malloc(total_size);
   if (!buffer) {
      LOG_ERROR("Failed to allocate WAV buffer");
      return -1;
   }

   /* Fill WAV header */
   wav_header_t *header = (wav_header_t *)buffer;
   memcpy(header->riff, "RIFF", 4);
   header->chunk_size = total_size - 8;
   memcpy(header->wave, "WAVE", 4);
   memcpy(header->fmt, "fmt ", 4);
   header->subchunk1_size = 16;
   header->audio_format = 1; /* PCM */
   header->num_channels = AUDIO_CHANNELS;
   header->sample_rate = AUDIO_SAMPLE_RATE;
   header->byte_rate = AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * AUDIO_BYTES_PER_SAMPLE;
   header->block_align = AUDIO_CHANNELS * AUDIO_BYTES_PER_SAMPLE;
   header->bits_per_sample = AUDIO_BITS_PER_SAMPLE;
   memcpy(header->data, "data", 4);
   header->subchunk2_size = pcm_size;

   /* Copy PCM data */
   memcpy(buffer + sizeof(wav_header_t), samples, pcm_size);

   *wav_data = buffer;
   *wav_size = total_size;

   LOG_INFO("Created WAV: %zu bytes, %zu samples", total_size, num_samples);
   return 0;
}

int audio_parse_wav(const uint8_t *wav_data,
                    size_t wav_size,
                    const int16_t **pcm_data,
                    size_t *pcm_size,
                    unsigned int *sample_rate,
                    unsigned int *channels) {
   if (!wav_data || wav_size < sizeof(wav_header_t) || !pcm_data || !pcm_size) {
      return -1;
   }

   /* Validate WAV header */
   if (memcmp(wav_data, "RIFF", 4) != 0) {
      LOG_ERROR("Invalid WAV: missing RIFF header");
      return -1;
   }

   if (memcmp(wav_data + 8, "WAVE", 4) != 0) {
      LOG_ERROR("Invalid WAV: missing WAVE format");
      return -1;
   }

   const wav_header_t *header = (const wav_header_t *)wav_data;

   if (header->audio_format != 1) {
      LOG_ERROR("Unsupported WAV format: %u (expected PCM)", header->audio_format);
      return -1;
   }

   if (header->bits_per_sample != 16) {
      LOG_ERROR("Unsupported bit depth: %u (expected 16)", header->bits_per_sample);
      return -1;
   }

   *pcm_data = (const int16_t *)(wav_data + sizeof(wav_header_t));
   *pcm_size = header->subchunk2_size;

   if (sample_rate)
      *sample_rate = header->sample_rate;
   if (channels)
      *channels = header->num_channels;

   LOG_INFO("Parsed WAV: %u Hz, %u ch, %zu bytes PCM", header->sample_rate, header->num_channels,
            *pcm_size);
   return 0;
}
