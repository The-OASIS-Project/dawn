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
 * Audio Capture Thread - Runtime Backend Selection
 *
 * Uses the audio_backend abstraction for runtime selection between
 * ALSA and PulseAudio backends. The backend is selected via
 * audio_backend_init() called in main() before starting capture.
 */

#include "audio/audio_capture_thread.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "audio/resampler.h"
#include "dawn.h"
#include "logging.h"
#include "ui/metrics.h"
#include "utils/string_utils.h"

#ifdef ENABLE_AEC
#include "audio/aec_processor.h"
#endif

// ============================================================================
// Mic Recording Implementation (works with or without AEC)
// ============================================================================
#define MIC_RECORDING_SAMPLE_RATE 16000
#define MIC_RECORDING_CHANNELS 1

static char g_mic_recording_dir[256] = "/tmp";
static bool g_mic_recording_enabled = false;
static bool g_mic_recording_active = false;
static FILE *g_mic_recording_file = NULL;
static size_t g_mic_recording_samples = 0;
static pthread_mutex_t g_mic_recording_mutex = PTHREAD_MUTEX_INITIALIZER;

// WAV header structure
typedef struct __attribute__((packed)) {
   char riff[4];
   uint32_t file_size;
   char wave[4];
   char fmt[4];
   uint32_t fmt_size;
   uint16_t audio_format;
   uint16_t channels;
   uint32_t sample_rate;
   uint32_t byte_rate;
   uint16_t block_align;
   uint16_t bits_per_sample;
   char data[4];
   uint32_t data_size;
} mic_wav_header_t;

static void mic_write_wav_header(FILE *f, uint32_t sample_rate, uint16_t channels) {
   mic_wav_header_t header = {
      .riff = { 'R', 'I', 'F', 'F' },
      .file_size = 0,  // Will be updated when closing
      .wave = { 'W', 'A', 'V', 'E' },
      .fmt = { 'f', 'm', 't', ' ' },
      .fmt_size = 16,
      .audio_format = 1,  // PCM
      .channels = channels,
      .sample_rate = sample_rate,
      .byte_rate = sample_rate * channels * 2,
      .block_align = (uint16_t)(channels * 2),
      .bits_per_sample = 16,
      .data = { 'd', 'a', 't', 'a' },
      .data_size = 0  // Will be updated when closing
   };
   fwrite(&header, sizeof(header), 1, f);
}

static void mic_finalize_wav_header(FILE *f, size_t num_samples, uint16_t channels) {
   uint32_t data_size = num_samples * channels * 2;
   uint32_t file_size = data_size + sizeof(mic_wav_header_t) - 8;

   fseek(f, 4, SEEK_SET);
   fwrite(&file_size, 4, 1, f);

   fseek(f, 40, SEEK_SET);
   fwrite(&data_size, 4, 1, f);
}

// Audio format constants
// Capture at 48kHz for optimal AEC performance, downsample to 16kHz for ASR
#define CAPTURE_RATE 48000
#define ASR_RATE 16000
#define DEFAULT_CHANNELS 1
#define DEFAULT_FRAMES 1536 /* 32ms at 48kHz */

// Compile-time validation: AEC sample rate must match capture rate
#ifdef ENABLE_AEC
#if CAPTURE_RATE != AEC_SAMPLE_RATE
#error "AEC requires capture rate (CAPTURE_RATE) to match AEC_SAMPLE_RATE (48000 Hz)"
#endif
#endif

/**
 * @brief Set realtime priority for current thread
 */
static int set_realtime_priority(void) {
   struct sched_param param;
   int max_priority = sched_get_priority_max(SCHED_FIFO);

   if (max_priority == -1) {
      OLOG_WARNING("Failed to get max realtime priority: %s", strerror(errno));
      return 1;
   }

   // Use priority 90 (high, but not maximum to leave room for critical system tasks)
   param.sched_priority = (max_priority > 90) ? 90 : max_priority;

   if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
      OLOG_WARNING("Failed to set realtime priority (try: sudo setcap cap_sys_nice=ep ./dawn): %s",
                   strerror(errno));
      return 1;
   }

   OLOG_INFO("Audio capture thread running with SCHED_FIFO priority %d", param.sched_priority);
   return 0;
}

/**
 * @brief Audio capture thread function
 *
 * Continuously reads audio from device and writes to ring buffer.
 * Uses the audio_backend abstraction for runtime backend selection.
 */
static void *capture_thread_func(void *arg) {
   audio_capture_context_t *ctx = (audio_capture_context_t *)arg;

   // Set realtime priority if requested
   if (ctx->use_realtime_priority) {
      set_realtime_priority();
   }

   // Allocate capture buffer
   char *buffer = (char *)malloc(ctx->buffer_size);
   if (!buffer) {
      OLOG_ERROR("Failed to allocate capture buffer of %zu bytes", ctx->buffer_size);
      atomic_store(&ctx->running, false);
      return NULL;
   }

   OLOG_INFO("Audio capture thread started (buffer=%zu bytes, device=%s, backend=%s, AEC=%s)",
             ctx->buffer_size, ctx->pcm_device, audio_backend_type_name(audio_backend_get_type()),
#ifdef ENABLE_AEC
             (aec_is_enabled() && ctx->aec_buffer && !ctx->aec_rate_mismatch) ? "enabled"
                                                                              : "disabled"
#else
             "not compiled"
#endif
   );

   // Update TUI metrics with AEC status
#ifdef ENABLE_AEC
   metrics_update_aec_enabled(aec_is_enabled() && ctx->aec_buffer && !ctx->aec_rate_mismatch);
#else
   metrics_update_aec_enabled(false);
#endif

   // Unified capture loop using audio_backend abstraction
   while (atomic_load(&ctx->running)) {
      ssize_t rc = audio_stream_capture_read(ctx->capture_handle, buffer, ctx->frames);

      if (rc > 0) {
         // Successful read at 48kHz
         size_t bytes_read_48k = (size_t)rc * DEFAULT_CHANNELS * 2;  // 2 bytes per sample (S16_LE)
         size_t samples_read_48k = bytes_read_48k / sizeof(int16_t);

#ifdef ENABLE_AEC
         // Process through AEC at 48kHz, then downsample to 16kHz for ASR
         if (aec_is_enabled() && ctx->aec_buffer && ctx->asr_buffer && ctx->downsample_resampler &&
             !ctx->aec_rate_mismatch && samples_read_48k <= ctx->aec_buffer_size) {
            // AEC processing at native 48kHz
            aec_process((int16_t *)buffer, ctx->aec_buffer, samples_read_48k);

            // Downsample 48kHz → 16kHz for ASR
            size_t samples_16k = resampler_process((resampler_t *)ctx->downsample_resampler,
                                                   ctx->aec_buffer, samples_read_48k,
                                                   ctx->asr_buffer, ctx->asr_buffer_size);

            if (samples_16k > 0) {
               ring_buffer_write(ctx->ring_buffer, (char *)ctx->asr_buffer,
                                 samples_16k * sizeof(int16_t));
               // Record what VAD sees (16kHz after AEC)
               mic_record_samples(ctx->asr_buffer, samples_16k);
            }
         } else if (ctx->asr_buffer && ctx->downsample_resampler) {
            // AEC disabled - still need to downsample 48kHz → 16kHz
            size_t samples_16k = resampler_process((resampler_t *)ctx->downsample_resampler,
                                                   (int16_t *)buffer, samples_read_48k,
                                                   ctx->asr_buffer, ctx->asr_buffer_size);

            if (samples_16k > 0) {
               ring_buffer_write(ctx->ring_buffer, (char *)ctx->asr_buffer,
                                 samples_16k * sizeof(int16_t));
               // Record what VAD sees (16kHz, no AEC)
               mic_record_samples(ctx->asr_buffer, samples_16k);
            }
         }
#else
         // Without AEC compiled, still need to downsample 48kHz → 16kHz for ASR
         if (ctx->asr_buffer && ctx->downsample_resampler) {
            size_t samples_16k = resampler_process((resampler_t *)ctx->downsample_resampler,
                                                   (int16_t *)buffer, samples_read_48k,
                                                   ctx->asr_buffer, ctx->asr_buffer_size);

            if (samples_16k > 0) {
               ring_buffer_write(ctx->ring_buffer, (char *)ctx->asr_buffer,
                                 samples_16k * sizeof(int16_t));
               // Record what VAD sees (16kHz)
               mic_record_samples(ctx->asr_buffer, samples_16k);
            }
         }
#endif
      } else if (rc < 0) {
         // Error occurred - map to error code
         int err = (int)(-rc);

         if (err == AUDIO_ERR_OVERRUN) {
            OLOG_WARNING("Audio capture overrun, recovering");
            audio_stream_capture_recover(ctx->capture_handle, err);
            continue;
         } else if (err == AUDIO_ERR_SUSPENDED) {
            OLOG_WARNING("Audio capture suspended, recovering");
            audio_stream_capture_recover(ctx->capture_handle, err);
            continue;
         } else {
            OLOG_ERROR("Audio capture read error: %s", audio_error_string((audio_error_t)err));
            // Continue trying despite error
            usleep(10000);  // 10ms delay before retry
         }
      }
      // rc == 0 means no data available, continue polling
   }

   free(buffer);
   OLOG_INFO("Audio capture thread stopped");

   return NULL;
}

audio_capture_context_t *audio_capture_start(const char *pcm_device,
                                             size_t ring_buffer_size,
                                             int use_realtime_priority) {
   if (!pcm_device) {
      OLOG_ERROR("PCM device name cannot be NULL");
      return NULL;
   }

   // Check that audio backend has been initialized
   if (audio_backend_get_type() == AUDIO_BACKEND_NONE) {
      OLOG_ERROR("Audio backend not initialized. Call audio_backend_init() first.");
      return NULL;
   }

   audio_capture_context_t *ctx = (audio_capture_context_t *)calloc(
       1, sizeof(audio_capture_context_t));
   if (!ctx) {
      OLOG_ERROR("Failed to allocate capture context");
      return NULL;
   }

   // Initialize context
   ctx->pcm_device = strdup(pcm_device);
   ctx->use_realtime_priority = use_realtime_priority;
   atomic_init(&ctx->running, false);

   // Create ring buffer
   ctx->ring_buffer = ring_buffer_create(ring_buffer_size);
   if (!ctx->ring_buffer) {
      OLOG_ERROR("Failed to create ring buffer");
      free(ctx->pcm_device);
      free(ctx);
      return NULL;
   }

   // Set up stream parameters
   audio_stream_params_t params = { .sample_rate = CAPTURE_RATE,
                                    .channels = DEFAULT_CHANNELS,
                                    .format = AUDIO_FORMAT_S16_LE,
                                    .period_frames = DEFAULT_FRAMES,
                                    .buffer_frames = DEFAULT_FRAMES * 4 };

   // Open capture device using audio_backend abstraction
   ctx->capture_handle = audio_stream_capture_open(pcm_device, &params, &ctx->hw_params);
   if (!ctx->capture_handle) {
      OLOG_ERROR("Failed to open capture device: %s", pcm_device);
      ring_buffer_free(ctx->ring_buffer);
      free(ctx->pcm_device);
      free(ctx);
      return NULL;
   }

   // Use actual hardware parameters
   ctx->frames = ctx->hw_params.period_frames;
   ctx->buffer_size = ctx->frames * ctx->hw_params.channels * 2;  // 2 bytes per sample (S16_LE)

   OLOG_INFO("Audio capture opened: rate=%u ch=%u period=%zu buffer=%zu (backend=%s)",
             ctx->hw_params.sample_rate, ctx->hw_params.channels, ctx->hw_params.period_frames,
             ctx->hw_params.buffer_frames, audio_backend_type_name(audio_backend_get_type()));

   // Calculate input buffer size in samples (for resampler allocation)
   size_t input_samples = ctx->buffer_size / sizeof(int16_t);

   // Create resampler for 48kHz → 16kHz downsampling (always needed for ASR)
   ctx->downsample_resampler = resampler_create(CAPTURE_RATE, ASR_RATE, 1);
   if (!ctx->downsample_resampler) {
      OLOG_ERROR("Failed to create 48kHz→16kHz resampler");
   }

   // Pre-allocate ASR buffer for 16kHz output
   // Use resampler's calculation to include proper margin for filter delay
   if (ctx->downsample_resampler) {
      ctx->asr_buffer_size = resampler_get_output_size(ctx->downsample_resampler, input_samples);
   } else {
      // Fallback if resampler failed to create
      ctx->asr_buffer_size = (input_samples / 3) + 128;
   }
   ctx->asr_buffer = (int16_t *)malloc(ctx->asr_buffer_size * sizeof(int16_t));
   if (!ctx->asr_buffer) {
      OLOG_WARNING("Failed to allocate ASR buffer");
   }

#ifdef ENABLE_AEC
   // Pre-allocate AEC buffer for 48kHz processing (same size as capture buffer)
   ctx->aec_buffer_size = input_samples;
   if (ctx->aec_buffer_size > AEC_MAX_SAMPLES) {
      ctx->aec_buffer_size = AEC_MAX_SAMPLES;
   }
   ctx->aec_buffer = (int16_t *)malloc(ctx->aec_buffer_size * sizeof(int16_t));
   if (!ctx->aec_buffer) {
      OLOG_WARNING("Failed to allocate AEC buffer - continuing without AEC");
   }

   // Runtime sample rate validation for AEC
   ctx->aec_rate_mismatch = 0;
   if (ctx->hw_params.sample_rate != AEC_SAMPLE_RATE) {
      OLOG_WARNING("AEC requires %d Hz but device is %u Hz - AEC disabled for this session",
                   AEC_SAMPLE_RATE, ctx->hw_params.sample_rate);
      ctx->aec_rate_mismatch = 1;
   }

   OLOG_INFO("Audio capture: %dHz → AEC → %dHz for ASR (buffers: aec=%zu, asr=%zu samples)",
             CAPTURE_RATE, ASR_RATE, ctx->aec_buffer_size, ctx->asr_buffer_size);
#else
   OLOG_INFO("Audio capture: %dHz → %dHz for ASR (no AEC, buffer=%zu samples)", CAPTURE_RATE,
             ASR_RATE, ctx->asr_buffer_size);
#endif

   // Start capture thread
   atomic_store(&ctx->running, true);
   if (pthread_create(&ctx->thread, NULL, capture_thread_func, ctx) != 0) {
      OLOG_ERROR("Failed to create capture thread: %s", strerror(errno));
      audio_stream_capture_close(ctx->capture_handle);
      ring_buffer_free(ctx->ring_buffer);
      free(ctx->pcm_device);
      free(ctx);
      return NULL;
   }

   OLOG_INFO("Audio capture started successfully");
   return ctx;
}

void audio_capture_stop(audio_capture_context_t *ctx) {
   if (!ctx) {
      return;
   }

   OLOG_INFO("Stopping audio capture thread...");

   // Signal thread to stop
   atomic_store(&ctx->running, false);

   // Wait for thread to exit
   pthread_join(ctx->thread, NULL);

   // Close audio device using audio_backend abstraction
   if (ctx->capture_handle) {
      audio_stream_capture_close(ctx->capture_handle);
      ctx->capture_handle = NULL;
   }

   // Free ring buffer
   ring_buffer_free(ctx->ring_buffer);

   // Free ASR buffer (always allocated)
   if (ctx->asr_buffer) {
      free(ctx->asr_buffer);
      ctx->asr_buffer = NULL;
   }

   // Destroy resampler (always allocated)
   if (ctx->downsample_resampler) {
      resampler_destroy((resampler_t *)ctx->downsample_resampler);
      ctx->downsample_resampler = NULL;
   }

#ifdef ENABLE_AEC
   // Free AEC buffer
   if (ctx->aec_buffer) {
      free(ctx->aec_buffer);
      ctx->aec_buffer = NULL;
   }
#endif

   // Free device name
   free(ctx->pcm_device);

   // Free context
   free(ctx);

   OLOG_INFO("Audio capture stopped and resources freed");
}

size_t audio_capture_read(audio_capture_context_t *ctx, char *data, size_t len) {
   if (!ctx || !ctx->ring_buffer) {
      return 0;
   }
   return ring_buffer_read(ctx->ring_buffer, data, len);
}

size_t audio_capture_wait_for_data(audio_capture_context_t *ctx, size_t min_bytes, int timeout_ms) {
   if (!ctx || !ctx->ring_buffer) {
      return 0;
   }
   return ring_buffer_wait_for_data(ctx->ring_buffer, min_bytes, timeout_ms);
}

size_t audio_capture_bytes_available(audio_capture_context_t *ctx) {
   if (!ctx || !ctx->ring_buffer) {
      return 0;
   }
   return ring_buffer_bytes_available(ctx->ring_buffer);
}

int audio_capture_is_running(audio_capture_context_t *ctx) {
   if (!ctx) {
      return 0;
   }
   return atomic_load(&ctx->running);
}

void audio_capture_clear(audio_capture_context_t *ctx) {
   if (!ctx || !ctx->ring_buffer) {
      return;
   }
   ring_buffer_clear(ctx->ring_buffer);
}

// ============================================================================
// Mic Recording API Implementation
// ============================================================================

void mic_set_recording_dir(const char *dir) {
   pthread_mutex_lock(&g_mic_recording_mutex);
   if (dir) {
      safe_strscpy(g_mic_recording_dir, dir);
   }
   OLOG_INFO("Mic recording directory set to: %s", g_mic_recording_dir);
   pthread_mutex_unlock(&g_mic_recording_mutex);
}

void mic_enable_recording(bool enable) {
   pthread_mutex_lock(&g_mic_recording_mutex);
   g_mic_recording_enabled = enable;
   OLOG_INFO("Mic recording %s", enable ? "enabled" : "disabled");

   // If disabling while recording is active, stop it
   if (!enable && g_mic_recording_active) {
      pthread_mutex_unlock(&g_mic_recording_mutex);
      mic_stop_recording();
      return;
   }
   pthread_mutex_unlock(&g_mic_recording_mutex);
}

bool mic_is_recording(void) {
   pthread_mutex_lock(&g_mic_recording_mutex);
   bool active = g_mic_recording_active;
   pthread_mutex_unlock(&g_mic_recording_mutex);
   return active;
}

bool mic_is_recording_enabled(void) {
   pthread_mutex_lock(&g_mic_recording_mutex);
   bool enabled = g_mic_recording_enabled;
   pthread_mutex_unlock(&g_mic_recording_mutex);
   return enabled;
}

int mic_start_recording(void) {
   pthread_mutex_lock(&g_mic_recording_mutex);

   if (!g_mic_recording_enabled) {
      pthread_mutex_unlock(&g_mic_recording_mutex);
      return 1;
   }

   if (g_mic_recording_active) {
      OLOG_WARNING("Mic recording already active");
      pthread_mutex_unlock(&g_mic_recording_mutex);
      return 1;
   }

   // Generate timestamp for filename
   time_t now = time(NULL);
   struct tm tm_storage;
   struct tm *tm_info = localtime_r(&now, &tm_storage);
   char timestamp[32];
   strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);

   // Create filename
   char filename[512];
   snprintf(filename, sizeof(filename), "%s/mic_capture_%s.wav", g_mic_recording_dir, timestamp);

   // Open file
   g_mic_recording_file = fopen(filename, "wb");
   if (!g_mic_recording_file) {
      OLOG_ERROR("Failed to open mic recording file: %s", filename);
      pthread_mutex_unlock(&g_mic_recording_mutex);
      return 1;
   }

   // Write WAV header (will be finalized when stopping)
   mic_write_wav_header(g_mic_recording_file, MIC_RECORDING_SAMPLE_RATE, MIC_RECORDING_CHANNELS);
   g_mic_recording_samples = 0;
   g_mic_recording_active = true;

   OLOG_INFO("Mic recording started: %s", filename);
   pthread_mutex_unlock(&g_mic_recording_mutex);
   return 0;
}

void mic_stop_recording(void) {
   pthread_mutex_lock(&g_mic_recording_mutex);

   if (!g_mic_recording_active || !g_mic_recording_file) {
      pthread_mutex_unlock(&g_mic_recording_mutex);
      return;
   }

   // Finalize WAV header with actual size
   mic_finalize_wav_header(g_mic_recording_file, g_mic_recording_samples, MIC_RECORDING_CHANNELS);
   fclose(g_mic_recording_file);
   g_mic_recording_file = NULL;

   float duration_secs = (float)g_mic_recording_samples / MIC_RECORDING_SAMPLE_RATE;
   OLOG_INFO("Mic recording stopped: %.2f seconds captured", duration_secs);

   g_mic_recording_active = false;
   pthread_mutex_unlock(&g_mic_recording_mutex);
}

void mic_record_samples(const int16_t *samples, size_t num_samples) {
   pthread_mutex_lock(&g_mic_recording_mutex);

   if (g_mic_recording_active && g_mic_recording_file && samples && num_samples > 0) {
      size_t written = fwrite(samples, sizeof(int16_t), num_samples, g_mic_recording_file);
      g_mic_recording_samples += written;
   }

   pthread_mutex_unlock(&g_mic_recording_mutex);
}
