/*
 * DAWN Satellite - ALSA Audio Capture
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
 */

#include "audio_capture.h"

#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_INFO(fmt, ...) fprintf(stdout, "[CAPTURE] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[CAPTURE ERROR] " fmt "\n", ##__VA_ARGS__)

int audio_capture_init(audio_capture_t *ctx, const char *device) {
   if (!ctx)
      return -1;

   memset(ctx, 0, sizeof(audio_capture_t));

   const char *dev = device ? device : AUDIO_DEFAULT_CAPTURE_DEVICE;
   strncpy(ctx->device, dev, sizeof(ctx->device) - 1);

   snd_pcm_t *handle;
   int err;

   /* Open PCM device for capture */
   err = snd_pcm_open(&handle, ctx->device, SND_PCM_STREAM_CAPTURE, 0);
   if (err < 0) {
      LOG_ERROR("Cannot open capture device '%s': %s", ctx->device, snd_strerror(err));
      return -1;
   }

   /* Set hardware parameters */
   snd_pcm_hw_params_t *hw_params;
   snd_pcm_hw_params_alloca(&hw_params);

   err = snd_pcm_hw_params_any(handle, hw_params);
   if (err < 0) {
      LOG_ERROR("Cannot initialize hw params: %s", snd_strerror(err));
      snd_pcm_close(handle);
      return -1;
   }

   /* Set access type - interleaved */
   err = snd_pcm_hw_params_set_access(handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
   if (err < 0) {
      LOG_ERROR("Cannot set access type: %s", snd_strerror(err));
      snd_pcm_close(handle);
      return -1;
   }

   /* Set format - 16-bit signed little-endian */
   err = snd_pcm_hw_params_set_format(handle, hw_params, SND_PCM_FORMAT_S16_LE);
   if (err < 0) {
      LOG_ERROR("Cannot set format: %s", snd_strerror(err));
      snd_pcm_close(handle);
      return -1;
   }

   /* Set channels - mono */
   err = snd_pcm_hw_params_set_channels(handle, hw_params, AUDIO_CHANNELS);
   if (err < 0) {
      LOG_ERROR("Cannot set channels: %s", snd_strerror(err));
      snd_pcm_close(handle);
      return -1;
   }

   /* Set sample rate */
   unsigned int rate = AUDIO_SAMPLE_RATE;
   err = snd_pcm_hw_params_set_rate_near(handle, hw_params, &rate, 0);
   if (err < 0) {
      LOG_ERROR("Cannot set sample rate: %s", snd_strerror(err));
      snd_pcm_close(handle);
      return -1;
   }

   if (rate != AUDIO_SAMPLE_RATE) {
      LOG_INFO("Requested %u Hz, got %u Hz", AUDIO_SAMPLE_RATE, rate);
   }
   ctx->sample_rate = rate;
   ctx->channels = AUDIO_CHANNELS;

   /* Set period size for low latency */
   snd_pcm_uframes_t period_size = 512; /* ~32ms at 16kHz */
   err = snd_pcm_hw_params_set_period_size_near(handle, hw_params, &period_size, 0);
   if (err < 0) {
      LOG_ERROR("Cannot set period size: %s", snd_strerror(err));
      snd_pcm_close(handle);
      return -1;
   }
   ctx->period_size = period_size;

   /* Set buffer size */
   snd_pcm_uframes_t buffer_size = period_size * 4;
   err = snd_pcm_hw_params_set_buffer_size_near(handle, hw_params, &buffer_size);
   if (err < 0) {
      LOG_ERROR("Cannot set buffer size: %s", snd_strerror(err));
      snd_pcm_close(handle);
      return -1;
   }

   /* Apply hardware parameters */
   err = snd_pcm_hw_params(handle, hw_params);
   if (err < 0) {
      LOG_ERROR("Cannot apply hw params: %s", snd_strerror(err));
      snd_pcm_close(handle);
      return -1;
   }

   /* Prepare the device */
   err = snd_pcm_prepare(handle);
   if (err < 0) {
      LOG_ERROR("Cannot prepare device: %s", snd_strerror(err));
      snd_pcm_close(handle);
      return -1;
   }

   ctx->handle = handle;
   ctx->initialized = 1;

   LOG_INFO("Capture initialized: %s @ %u Hz, %zu frame periods", ctx->device, ctx->sample_rate,
            ctx->period_size);

   return 0;
}

void audio_capture_cleanup(audio_capture_t *ctx) {
   if (ctx && ctx->handle) {
      snd_pcm_drop((snd_pcm_t *)ctx->handle);
      snd_pcm_close((snd_pcm_t *)ctx->handle);
      ctx->handle = NULL;
      ctx->initialized = 0;
      LOG_INFO("Capture cleaned up");
   }
}

ssize_t audio_capture_record(audio_capture_t *ctx,
                             int16_t *buffer,
                             size_t max_samples,
                             volatile int *stop_flag) {
   if (!ctx || !ctx->initialized || !buffer) {
      return -1;
   }

   snd_pcm_t *handle = (snd_pcm_t *)ctx->handle;
   size_t total_samples = 0;
   snd_pcm_sframes_t frames;

   /* Ensure device is ready */
   int err = snd_pcm_prepare(handle);
   if (err < 0) {
      LOG_ERROR("Cannot prepare for recording: %s", snd_strerror(err));
      return -1;
   }

   LOG_INFO("Recording started (max %zu samples)...", max_samples);

   while (total_samples < max_samples) {
      /* Check stop flag */
      if (stop_flag && *stop_flag) {
         LOG_INFO("Recording stopped by flag");
         break;
      }

      /* Calculate how many frames we can read */
      size_t remaining = max_samples - total_samples;
      size_t to_read = (remaining > ctx->period_size) ? ctx->period_size : remaining;

      /* Read audio frames */
      frames = snd_pcm_readi(handle, buffer + total_samples, to_read);

      if (frames < 0) {
         /* Handle buffer overrun */
         if (frames == -EPIPE) {
            LOG_ERROR("Buffer overrun, recovering...");
            snd_pcm_prepare(handle);
            continue;
         } else if (frames == -EAGAIN) {
            /* No data available, wait a bit */
            usleep(1000);
            continue;
         } else {
            LOG_ERROR("Read error: %s", snd_strerror(frames));
            return -1;
         }
      }

      total_samples += frames;
   }

   LOG_INFO("Recording complete: %zu samples (%.2f seconds)", total_samples,
            (float)total_samples / ctx->sample_rate);

   return (ssize_t)total_samples;
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
