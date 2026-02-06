/*
 * DAWN Satellite - ALSA Audio Playback
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

#include "audio_playback.h"

#include <alsa/asoundlib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_capture.h" /* For wav_header_t and audio_parse_wav */
#include "logging.h"

int audio_playback_init(audio_playback_t *ctx, const char *device) {
   if (!ctx)
      return -1;

   memset(ctx, 0, sizeof(audio_playback_t));

   const char *dev = device ? device : AUDIO_DEFAULT_PLAYBACK_DEVICE;
   strncpy(ctx->device, dev, sizeof(ctx->device) - 1);

   snd_pcm_t *handle;
   int err;

   /* Open PCM device for playback */
   err = snd_pcm_open(&handle, ctx->device, SND_PCM_STREAM_PLAYBACK, 0);
   if (err < 0) {
      LOG_ERROR("Cannot open playback device '%s': %s", ctx->device, snd_strerror(err));
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

   /* Set channels - stereo for I2S DAC */
   err = snd_pcm_hw_params_set_channels(handle, hw_params, AUDIO_PLAYBACK_CHANNELS);
   if (err < 0) {
      LOG_ERROR("Cannot set channels: %s", snd_strerror(err));
      snd_pcm_close(handle);
      return -1;
   }

   /* Set sample rate */
   unsigned int rate = AUDIO_PLAYBACK_RATE;
   err = snd_pcm_hw_params_set_rate_near(handle, hw_params, &rate, 0);
   if (err < 0) {
      LOG_ERROR("Cannot set sample rate: %s", snd_strerror(err));
      snd_pcm_close(handle);
      return -1;
   }

   if (rate != AUDIO_PLAYBACK_RATE) {
      LOG_INFO("Requested %u Hz, got %u Hz", AUDIO_PLAYBACK_RATE, rate);
   }
   ctx->sample_rate = rate;
   ctx->channels = AUDIO_PLAYBACK_CHANNELS;

   /* Set period size */
   snd_pcm_uframes_t period_size = 512;
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

   ctx->handle = handle;
   ctx->initialized = 1;

   LOG_INFO("Playback initialized: %s @ %u Hz stereo, %zu frame periods", ctx->device,
            ctx->sample_rate, ctx->period_size);

   return 0;
}

void audio_playback_cleanup(audio_playback_t *ctx) {
   if (ctx && ctx->handle) {
      snd_pcm_drain((snd_pcm_t *)ctx->handle);
      snd_pcm_close((snd_pcm_t *)ctx->handle);
      ctx->handle = NULL;
      ctx->initialized = 0;
      LOG_INFO("Playback cleaned up");
   }
}

void audio_playback_stop(audio_playback_t *ctx) {
   if (ctx && ctx->handle) {
      snd_pcm_drop((snd_pcm_t *)ctx->handle);
      snd_pcm_prepare((snd_pcm_t *)ctx->handle);
   }
}

int audio_playback_play(audio_playback_t *ctx,
                        const int16_t *samples,
                        size_t num_samples,
                        unsigned int sample_rate,
                        volatile int *stop_flag) {
   if (!ctx || !ctx->initialized || !samples || num_samples == 0) {
      return -1;
   }

   snd_pcm_t *handle = (snd_pcm_t *)ctx->handle;

   /* Prepare device */
   int err = snd_pcm_prepare(handle);
   if (err < 0) {
      LOG_ERROR("Cannot prepare for playback: %s", snd_strerror(err));
      return -1;
   }

   /* Calculate resampling parameters */
   double out_rate = (double)ctx->sample_rate;
   double in_rate = (double)sample_rate;
   double step = in_rate / out_rate; /* Input index advance per output frame */

   /* Calculate total output frames */
   size_t out_frames = (size_t)((double)num_samples * (out_rate / in_rate) + 0.5);

   LOG_INFO("Playing %zu samples @ %u Hz -> %zu frames @ %u Hz", num_samples, sample_rate,
            out_frames, ctx->sample_rate);

   /* Allocate output buffer (stereo) */
   size_t chunk_size = ctx->period_size;
   int16_t *out_buf = malloc(chunk_size * 2 * sizeof(int16_t)); /* Stereo */
   if (!out_buf) {
      LOG_ERROR("Failed to allocate output buffer");
      return -1;
   }

   double pos = 0.0; /* Position in input samples */
   size_t produced = 0;

   while (produced < out_frames) {
      /* Check stop flag */
      if (stop_flag && *stop_flag) {
         LOG_INFO("Playback stopped by flag");
         break;
      }

      size_t n = (out_frames - produced > chunk_size) ? chunk_size : (out_frames - produced);

      /* Resample and convert mono to stereo with linear interpolation */
      for (size_t j = 0; j < n; j++) {
         size_t i0 = (size_t)pos;
         double frac = pos - (double)i0;

         /* Clamp indices */
         if (i0 >= num_samples)
            i0 = num_samples - 1;
         size_t i1 = (i0 + 1 < num_samples) ? (i0 + 1) : (num_samples - 1);

         /* Linear interpolation */
         int16_t a = samples[i0];
         int16_t b = samples[i1];
         int16_t s = (int16_t)((1.0 - frac) * a + frac * b);

         /* Stereo: duplicate to both channels */
         out_buf[2 * j + 0] = s; /* Left */
         out_buf[2 * j + 1] = s; /* Right */

         pos += step;
      }

      /* Write to ALSA */
      snd_pcm_sframes_t frames = snd_pcm_writei(handle, out_buf, n);

      if (frames < 0) {
         if (frames == -EPIPE) {
            LOG_ERROR("Buffer underrun, recovering...");
            snd_pcm_prepare(handle);
            continue;
         } else if (frames == -EAGAIN) {
            usleep(1000);
            continue;
         } else {
            LOG_ERROR("Write error: %s", snd_strerror(frames));
            free(out_buf);
            return -1;
         }
      }

      produced += frames;
   }

   free(out_buf);

   /* Drain remaining audio */
   snd_pcm_drain(handle);

   LOG_INFO("Playback complete: %zu frames", produced);
   return 0;
}

int audio_playback_play_wav(audio_playback_t *ctx,
                            const uint8_t *wav_data,
                            size_t wav_size,
                            volatile int *stop_flag) {
   if (!ctx || !wav_data || wav_size == 0) {
      return -1;
   }

   const int16_t *pcm_data;
   size_t pcm_size;
   unsigned int sample_rate;
   unsigned int channels;

   if (audio_parse_wav(wav_data, wav_size, &pcm_data, &pcm_size, &sample_rate, &channels) != 0) {
      LOG_ERROR("Failed to parse WAV data");
      return -1;
   }

   /* Calculate number of samples (PCM size is in bytes) */
   size_t num_samples = pcm_size / sizeof(int16_t);

   /* Handle stereo input by taking just left channel */
   if (channels == 2) {
      LOG_INFO("Converting stereo to mono");
      /* For stereo, samples are interleaved L R L R... */
      /* We'll just use the left channel (every other sample) */
      int16_t *mono = malloc(num_samples / 2 * sizeof(int16_t));
      if (!mono) {
         LOG_ERROR("Failed to allocate mono buffer");
         return -1;
      }

      for (size_t i = 0; i < num_samples / 2; i++) {
         mono[i] = pcm_data[i * 2]; /* Left channel */
      }

      int ret = audio_playback_play(ctx, mono, num_samples / 2, sample_rate, stop_flag);
      free(mono);
      return ret;
   }

   return audio_playback_play(ctx, pcm_data, num_samples, sample_rate, stop_flag);
}
