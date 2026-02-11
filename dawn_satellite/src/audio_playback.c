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
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_capture.h" /* For wav_header_t and audio_parse_wav */
#include "logging.h"

/* ALSA buffer sizing: more periods = more resilience to scheduler jitter */
#define ALSA_PERIOD_SIZE 512
#define ALSA_BUFFER_PERIODS 16 /* 16 × 512 = 8192 frames ≈ 170ms at 48kHz */

/* =============================================================================
 * Goertzel DFT - Pre-computed coefficients for spectrum visualization
 * ============================================================================= */

static float goertzel_coeff[SPECTRUM_BINS]; /* 2*cos(omega) per bin */
static _Atomic bool goertzel_initialized = false;

/**
 * Pre-compute Goertzel coefficients for the given sample rate.
 * Called once from audio_playback_init(). Bins are logarithmically spaced
 * from 30Hz to 16kHz, matching the WebUI's perceptual frequency emphasis.
 * Log spacing gives more bins to bass/mids where musical energy concentrates.
 */
static void init_goertzel_tables(unsigned int sample_rate) {
   if (atomic_exchange(&goertzel_initialized, true))
      return;

   float freq_lo = 30.0f;
   float freq_hi = 16000.0f;
   for (int k = 0; k < SPECTRUM_BINS; k++) {
      /* Logarithmic spacing: freq = 30 * (16000/30)^(k/63) */
      float t = (float)k / (float)(SPECTRUM_BINS - 1);
      float freq = freq_lo * powf(freq_hi / freq_lo, t);
      float nyquist = (float)sample_rate / 2.0f;
      if (freq > nyquist)
         freq = nyquist;
      float omega = 2.0f * (float)M_PI * freq / (float)sample_rate;
      goertzel_coeff[k] = 2.0f * cosf(omega);
   }
}

/**
 * Compute spectrum magnitudes using Goertzel algorithm.
 * Runs in the ALSA playback hot path (~30-50µs on Cortex-A76).
 *
 * @param ctx Playback context (writes ctx->spectrum[])
 * @param mono_buf Mono float samples in [-1, 1] range
 * @param n Number of samples
 */
static void compute_spectrum(audio_playback_t *ctx, const float *mono_buf, size_t n) {
   float raw[SPECTRUM_BINS];
   float peak = 0.0f;

   for (int k = 0; k < SPECTRUM_BINS; k++) {
      float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f;
      float coeff = goertzel_coeff[k];

      for (size_t i = 0; i < n; i++) {
         s0 = mono_buf[i] + coeff * s1 - s2;
         s2 = s1;
         s1 = s0;
      }

      /* Magnitude squared: s1^2 + s2^2 - coeff*s1*s2 */
      float mag_sq = s1 * s1 + s2 * s2 - coeff * s1 * s2;
      float mag = sqrtf(mag_sq > 0.0f ? mag_sq : 0.0f);
      raw[k] = mag;
      if (mag > peak)
         peak = mag;
   }

   /* Convert to absolute dB scale, matching WebUI's getByteFrequencyData behavior.
    * Uses fixed reference (N/2 = max Goertzel magnitude for full-scale sine)
    * so quiet passages produce genuinely low values instead of being peak-normalized. */
   float ref = (float)n * 0.5f; /* Max magnitude for a full-scale sine at bin freq */
   for (int k = 0; k < SPECTRUM_BINS; k++) {
      if (raw[k] < 1e-6f) {
         ctx->spectrum[k] = 0.0f;
         continue;
      }
      /* Absolute dB: 0dB = full-scale sine, negative = quieter.
       * Map [-60dB, 0dB] → [0.0, 1.0], floor below -60dB. */
      float db = 20.0f * log10f(raw[k] / ref);
      float val = 1.0f + db / 60.0f;
      if (val < 0.0f)
         val = 0.0f;
      /* Gamma correction matching WebUI (pow 0.7) */
      ctx->spectrum[k] = powf(val, 0.7f);
   }
}

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
   snd_pcm_uframes_t period_size = ALSA_PERIOD_SIZE;
   err = snd_pcm_hw_params_set_period_size_near(handle, hw_params, &period_size, 0);
   if (err < 0) {
      LOG_ERROR("Cannot set period size: %s", snd_strerror(err));
      snd_pcm_close(handle);
      return -1;
   }
   ctx->period_size = period_size;

   /* Set buffer size (periods × size — headroom for scheduler jitter) */
   snd_pcm_uframes_t buffer_size = period_size * ALSA_BUFFER_PERIODS;
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
   atomic_store(&ctx->volume, 80);
   pthread_mutex_init(&ctx->alsa_mutex, NULL);

   /* Pre-compute Goertzel DFT coefficients for spectrum visualization */
   init_goertzel_tables(ctx->sample_rate);

   LOG_INFO("Playback initialized: %s @ %u Hz stereo, %zu frame periods", ctx->device,
            ctx->sample_rate, ctx->period_size);

   return 0;
}

void audio_playback_cleanup(audio_playback_t *ctx) {
   if (ctx && ctx->handle) {
      pthread_mutex_lock(&ctx->alsa_mutex);
      snd_pcm_drain((snd_pcm_t *)ctx->handle);
      snd_pcm_close((snd_pcm_t *)ctx->handle);
      ctx->handle = NULL;
      ctx->initialized = 0;
      pthread_mutex_unlock(&ctx->alsa_mutex);
      pthread_mutex_destroy(&ctx->alsa_mutex);
      LOG_INFO("Playback cleaned up");
   }
}

void audio_playback_stop(audio_playback_t *ctx) {
   if (ctx && ctx->handle) {
      pthread_mutex_lock(&ctx->alsa_mutex);
      snd_pcm_drop((snd_pcm_t *)ctx->handle);
      snd_pcm_prepare((snd_pcm_t *)ctx->handle);
      pthread_mutex_unlock(&ctx->alsa_mutex);
   }
}

int audio_playback_play(audio_playback_t *ctx,
                        const int16_t *samples,
                        size_t num_samples,
                        unsigned int sample_rate,
                        atomic_int *stop_flag) {
   if (!ctx || !ctx->initialized || !samples || num_samples == 0) {
      return -1;
   }

   snd_pcm_t *handle = (snd_pcm_t *)ctx->handle;

   /* Prepare device */
   pthread_mutex_lock(&ctx->alsa_mutex);
   int err = snd_pcm_prepare(handle);
   pthread_mutex_unlock(&ctx->alsa_mutex);
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
      if (stop_flag && atomic_load(stop_flag)) {
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

      /* Apply master volume scaling (Q15 fixed-point to avoid per-sample division) */
      {
         int vol = atomic_load(&ctx->volume);
         if (vol < 100) {
            int vol_fp = (vol << 15) / 100; /* Q15: 0..32767 maps to 0.0..~1.0 */
            for (size_t j = 0; j < n * 2; j++) {
               out_buf[j] = (int16_t)(((int32_t)out_buf[j] * vol_fp) >> 15);
            }
         }
      }

      /* Compute RMS amplitude and spectrum for visualization (mono channel only).
       * Reuses the same float conversion for both RMS and Goertzel DFT. */
      {
         float mono_float[n]; /* VLA - period_size is typically 512 */
         float sum_sq = 0.0f;
         for (size_t j = 0; j < n; j++) {
            float s = out_buf[2 * j] / 32768.0f;
            mono_float[j] = s;
            sum_sq += s * s;
         }
         ctx->amplitude = sqrtf(sum_sq / (float)n);
         compute_spectrum(ctx, mono_float, n);
      }

      /* Write to ALSA (mutex protects PCM handle) */
      pthread_mutex_lock(&ctx->alsa_mutex);
      snd_pcm_sframes_t frames = snd_pcm_writei(handle, out_buf, n);
      if (frames == -EPIPE) {
         LOG_ERROR("Buffer underrun, recovering...");
         snd_pcm_prepare(handle);
         pthread_mutex_unlock(&ctx->alsa_mutex);
         continue;
      }
      pthread_mutex_unlock(&ctx->alsa_mutex);

      if (frames < 0) {
         if (frames == -EAGAIN) {
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

   /* Drain remaining audio (skip if stopped - drain blocks until buffer empties) */
   pthread_mutex_lock(&ctx->alsa_mutex);
   if (stop_flag && atomic_load(stop_flag)) {
      snd_pcm_drop(handle);
   } else {
      snd_pcm_drain(handle);
   }
   pthread_mutex_unlock(&ctx->alsa_mutex);

   ctx->amplitude = 0.0f;
   for (int k = 0; k < SPECTRUM_BINS; k++) {
      ctx->spectrum[k] = 0.0f;
   }
   LOG_INFO("Playback complete: %zu frames", produced);
   return 0;
}

int audio_playback_play_wav(audio_playback_t *ctx,
                            const uint8_t *wav_data,
                            size_t wav_size,
                            atomic_int *stop_flag) {
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

int audio_playback_play_stereo(audio_playback_t *ctx,
                               const int16_t *stereo_samples,
                               size_t num_frames,
                               atomic_int *stop_flag) {
   if (!ctx || !ctx->initialized || !stereo_samples || num_frames == 0) {
      return -1;
   }

   snd_pcm_t *handle = (snd_pcm_t *)ctx->handle;
   size_t chunk = ctx->period_size;
   size_t written = 0;

   while (written < num_frames) {
      if (stop_flag && atomic_load(stop_flag)) {
         break;
      }

      size_t n = (num_frames - written > chunk) ? chunk : (num_frames - written);
      const int16_t *buf = stereo_samples + written * 2;

      /* Compute spectrum from left channel for visualizer.
       * Decimate: only compute every SPECTRUM_DECIMATE periods (~100ms)
       * to avoid wasting CPU — UI only consumes at ~8 Hz (VIZ_UPDATE_MS=120). */
      {
         static unsigned int spectrum_counter = 0;
#define SPECTRUM_DECIMATE 5 /* ~53ms at 512 frames / 48kHz period */
         if (++spectrum_counter >= SPECTRUM_DECIMATE) {
            spectrum_counter = 0;
            float mono_float[n]; /* VLA — period_size typically 512 */
            float sum_sq = 0.0f;
            for (size_t j = 0; j < n; j++) {
               float s = buf[2 * j] / 32768.0f;
               mono_float[j] = s;
               sum_sq += s * s;
            }
            ctx->amplitude = sqrtf(sum_sq / (float)n);
            compute_spectrum(ctx, mono_float, n);
         }
      }

      /* Write to ALSA (mutex protects PCM handle) */
      pthread_mutex_lock(&ctx->alsa_mutex);
      snd_pcm_sframes_t frames = snd_pcm_writei(handle, buf, n);
      if (frames == -EPIPE) {
         LOG_WARNING("Stereo playback underrun, recovering...");
         snd_pcm_prepare(handle);
         pthread_mutex_unlock(&ctx->alsa_mutex);
         continue;
      }
      if (frames == -EBADFD) {
         LOG_WARNING("Stereo playback: bad state, re-preparing");
         snd_pcm_prepare(handle);
         pthread_mutex_unlock(&ctx->alsa_mutex);
         continue;
      }
      if (frames == -ESTRPIPE) {
         LOG_WARNING("Stereo playback: suspended, resuming");
         while (snd_pcm_resume(handle) == -EAGAIN)
            usleep(100000);
         snd_pcm_prepare(handle);
         pthread_mutex_unlock(&ctx->alsa_mutex);
         continue;
      }
      pthread_mutex_unlock(&ctx->alsa_mutex);

      if (frames < 0) {
         if (frames == -EAGAIN) {
            usleep(1000);
            continue;
         }
         LOG_ERROR("Stereo write error: %s", snd_strerror(frames));
         return -1;
      }

      written += (size_t)frames;
   }

   return (int)written;
}

void audio_playback_set_volume(audio_playback_t *ctx, int volume) {
   if (!ctx)
      return;
   if (volume < 0)
      volume = 0;
   if (volume > 100)
      volume = 100;
   atomic_store(&ctx->volume, volume);
}

int audio_playback_get_volume(audio_playback_t *ctx) {
   return ctx ? atomic_load(&ctx->volume) : 80;
}
