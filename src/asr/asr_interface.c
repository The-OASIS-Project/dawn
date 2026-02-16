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
 */

#include "asr/asr_interface.h"

#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef ENABLE_VOSK
#include "asr/asr_vosk.h"
#else
#include "asr/asr_whisper.h"
#endif
#include "logging.h"
#include "ui/metrics.h"

// ============================================================================
// Audio Normalization for ASR
// ============================================================================
// Normalizes quiet audio to improve ASR accuracy. Uses envelope-following
// gain control to prevent clicks/pops at chunk boundaries.

// Set to 0 to disable normalization entirely
#define NORM_ENABLED 0

// Normalization thresholds (as fraction of INT16_MAX = 32767)
#define NORM_THRESHOLD 0.15f  // Only normalize if peak < 15% (-16 dB)
#define NORM_TARGET 0.50f     // Target peak at 50% (-6 dB headroom)
#define NORM_MAX_GAIN 8.0f    // Maximum gain for soft mics (+18 dB)
#define NORM_MIN_SAMPLES 160  // Minimum samples to normalize (10ms at 16kHz)

// Gain smoothing parameters (at 16kHz sample rate)
#define NORM_ATTACK_MS 5.0f     // Fast attack to catch speech onset
#define NORM_RELEASE_MS 100.0f  // Slow release for smooth fade
#define NORM_SAMPLE_RATE 16000

// Static buffer for normalized audio (avoids allocation in hot path)
// Size: 30 seconds at 16kHz = 480000 samples (enough for any utterance)
#define NORM_BUFFER_SIZE 480000
static int16_t g_norm_buffer[NORM_BUFFER_SIZE];

// Normalization coefficients (computed once, shared across all contexts)
static float g_norm_attack_coeff = 0.0f;
static float g_norm_release_coeff = 0.0f;
static bool g_norm_coeffs_initialized = false;

/**
 * @brief Per-context normalization state for thread safety
 *
 * This state is stored per-context to allow concurrent ASR sessions
 * (e.g., multiple network clients) without race conditions.
 */
typedef struct {
   float current_gain;  // Current applied gain (1.0 = unity)
   float envelope;      // Peak envelope follower
} norm_state_t;

// ============================================================================
// ASR Audio Recording for Debugging
// ============================================================================
// Records pre-normalization and post-normalization audio to WAV files
// for analysis and tuning of the normalization parameters.

#define ASR_RECORDING_SAMPLE_RATE 16000
#define ASR_RECORDING_CHANNELS 1

static char g_asr_recording_dir[256] = "/tmp";
static bool g_asr_recording_enabled = false;
static bool g_asr_recording_active = false;
static FILE *g_asr_pre_file = NULL;   // Pre-normalization (raw input)
static FILE *g_asr_post_file = NULL;  // Post-normalization (what ASR sees)
static size_t g_asr_pre_samples = 0;
static size_t g_asr_post_samples = 0;
static pthread_mutex_t g_asr_recording_mutex = PTHREAD_MUTEX_INITIALIZER;

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
} asr_wav_header_t;

static void asr_write_wav_header(FILE *f, uint32_t sample_rate, uint16_t channels) {
   asr_wav_header_t header = { .riff = { 'R', 'I', 'F', 'F' },
                               .file_size = 0,  // Updated on close
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
                               .data_size = 0 };  // Updated on close
   fwrite(&header, sizeof(header), 1, f);
}

static void asr_finalize_wav_header(FILE *f, size_t num_samples, uint16_t channels) {
   uint32_t data_size = num_samples * channels * 2;
   uint32_t file_size = data_size + sizeof(asr_wav_header_t) - 8;

   fflush(f);  // Flush any buffered data first
   fseek(f, 4, SEEK_SET);
   fwrite(&file_size, 4, 1, f);

   fseek(f, 40, SEEK_SET);
   fwrite(&data_size, 4, 1, f);
   fflush(f);  // Ensure header is written
}

void asr_set_recording_dir(const char *dir) {
   if (dir) {
      strncpy(g_asr_recording_dir, dir, sizeof(g_asr_recording_dir) - 1);
      g_asr_recording_dir[sizeof(g_asr_recording_dir) - 1] = '\0';
   }
}

void asr_enable_recording(bool enable) {
   pthread_mutex_lock(&g_asr_recording_mutex);
   g_asr_recording_enabled = enable;
   if (!enable && g_asr_recording_active) {
      pthread_mutex_unlock(&g_asr_recording_mutex);
      asr_stop_recording();
      return;
   }
   pthread_mutex_unlock(&g_asr_recording_mutex);
}

bool asr_is_recording(void) {
   pthread_mutex_lock(&g_asr_recording_mutex);
   bool active = g_asr_recording_active;
   pthread_mutex_unlock(&g_asr_recording_mutex);
   return active;
}

bool asr_is_recording_enabled(void) {
   pthread_mutex_lock(&g_asr_recording_mutex);
   bool enabled = g_asr_recording_enabled;
   pthread_mutex_unlock(&g_asr_recording_mutex);
   return enabled;
}

int asr_start_recording(void) {
   pthread_mutex_lock(&g_asr_recording_mutex);

   if (!g_asr_recording_enabled) {
      pthread_mutex_unlock(&g_asr_recording_mutex);
      return 1;
   }

   if (g_asr_recording_active) {
      pthread_mutex_unlock(&g_asr_recording_mutex);
      return 1;
   }

   // Generate timestamp
   time_t now = time(NULL);
   struct tm tm_storage;
   struct tm *tm_info = localtime_r(&now, &tm_storage);
   char timestamp[32];
   strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);

   // Create filenames
   char pre_filename[512], post_filename[512];
   snprintf(pre_filename, sizeof(pre_filename), "%s/asr_pre_%s.wav", g_asr_recording_dir,
            timestamp);
   snprintf(post_filename, sizeof(post_filename), "%s/asr_post_%s.wav", g_asr_recording_dir,
            timestamp);

   // Open files
   g_asr_pre_file = fopen(pre_filename, "wb");
   g_asr_post_file = fopen(post_filename, "wb");

   if (!g_asr_pre_file || !g_asr_post_file) {
      LOG_ERROR("Failed to open ASR recording files");
      if (g_asr_pre_file) {
         fclose(g_asr_pre_file);
         g_asr_pre_file = NULL;
      }
      if (g_asr_post_file) {
         fclose(g_asr_post_file);
         g_asr_post_file = NULL;
      }
      pthread_mutex_unlock(&g_asr_recording_mutex);
      return 1;
   }

   // Write WAV headers
   asr_write_wav_header(g_asr_pre_file, ASR_RECORDING_SAMPLE_RATE, ASR_RECORDING_CHANNELS);
   asr_write_wav_header(g_asr_post_file, ASR_RECORDING_SAMPLE_RATE, ASR_RECORDING_CHANNELS);
   g_asr_pre_samples = 0;
   g_asr_post_samples = 0;
   g_asr_recording_active = true;

   LOG_INFO("ASR recording started: %s, %s", pre_filename, post_filename);
   pthread_mutex_unlock(&g_asr_recording_mutex);
   return 0;
}

void asr_stop_recording(void) {
   pthread_mutex_lock(&g_asr_recording_mutex);

   if (!g_asr_recording_active) {
      pthread_mutex_unlock(&g_asr_recording_mutex);
      return;
   }

   // Finalize and close pre-normalization file
   if (g_asr_pre_file) {
      asr_finalize_wav_header(g_asr_pre_file, g_asr_pre_samples, ASR_RECORDING_CHANNELS);
      fclose(g_asr_pre_file);
      g_asr_pre_file = NULL;
   }

   // Finalize and close post-normalization file
   if (g_asr_post_file) {
      asr_finalize_wav_header(g_asr_post_file, g_asr_post_samples, ASR_RECORDING_CHANNELS);
      fclose(g_asr_post_file);
      g_asr_post_file = NULL;
   }

   float pre_duration = (float)g_asr_pre_samples / ASR_RECORDING_SAMPLE_RATE;
   float post_duration = (float)g_asr_post_samples / ASR_RECORDING_SAMPLE_RATE;
   LOG_INFO("ASR recording stopped: pre=%.2fs, post=%.2fs", pre_duration, post_duration);

   g_asr_recording_active = false;
   pthread_mutex_unlock(&g_asr_recording_mutex);
}

static void asr_record_pre_samples(const int16_t *samples, size_t num_samples) {
   pthread_mutex_lock(&g_asr_recording_mutex);
   if (g_asr_recording_active && g_asr_pre_file && samples && num_samples > 0) {
      fwrite(samples, sizeof(int16_t), num_samples, g_asr_pre_file);
      g_asr_pre_samples += num_samples;
   }
   pthread_mutex_unlock(&g_asr_recording_mutex);
}

static void asr_record_post_samples(const int16_t *samples, size_t num_samples) {
   pthread_mutex_lock(&g_asr_recording_mutex);
   if (g_asr_recording_active && g_asr_post_file && samples && num_samples > 0) {
      fwrite(samples, sizeof(int16_t), num_samples, g_asr_post_file);
      g_asr_post_samples += num_samples;
   }
   pthread_mutex_unlock(&g_asr_recording_mutex);
}

/**
 * @brief Initialize normalization coefficients (called once)
 */
static void init_norm_coefficients(void) {
   if (!g_norm_coeffs_initialized) {
      float attack_samples = NORM_ATTACK_MS * NORM_SAMPLE_RATE / 1000.0f;
      float release_samples = NORM_RELEASE_MS * NORM_SAMPLE_RATE / 1000.0f;
      g_norm_attack_coeff = 1.0f - expf(-2.2f / attack_samples);
      g_norm_release_coeff = 1.0f - expf(-2.2f / release_samples);
      g_norm_coeffs_initialized = true;
   }
}

/**
 * @brief Normalize audio for ASR processing with smooth gain transitions
 *
 * Uses envelope-following gain control to prevent clicks/pops at chunk
 * boundaries. The gain ramps smoothly between target values rather than
 * jumping instantly.
 *
 * @param state Per-context normalization state (for thread safety)
 * @param audio_in Input audio samples
 * @param audio_out Output buffer for normalized audio (can be same as input)
 * @param num_samples Number of samples
 * @return Average gain applied
 */
static float normalize_audio_for_asr(norm_state_t *state,
                                     const int16_t *audio_in,
                                     int16_t *audio_out,
                                     size_t num_samples) {
   // Ensure coefficients are initialized
   init_norm_coefficients();

   if (num_samples < NORM_MIN_SAMPLES) {
      // Too few samples - just copy without normalization
      if (audio_in != audio_out) {
         memcpy(audio_out, audio_in, num_samples * sizeof(int16_t));
      }
      return 1.0f;
   }

   float gain_sum = 0.0f;

   // Process sample-by-sample with envelope following
   for (size_t i = 0; i < num_samples; i++) {
      float sample = (float)audio_in[i];
      float abs_sample = sample < 0 ? -sample : sample;

      // Update envelope (peak follower with attack/release)
      if (abs_sample > state->envelope) {
         // Attack: envelope rises quickly
         state->envelope += g_norm_attack_coeff * (abs_sample - state->envelope);
      } else {
         // Release: envelope falls slowly
         state->envelope += g_norm_release_coeff * (abs_sample - state->envelope);
      }

      // Calculate target gain based on current envelope
      float envelope_fraction = state->envelope / 32767.0f;
      float target_gain = 1.0f;

      if (envelope_fraction > 0.001f && envelope_fraction < NORM_THRESHOLD) {
         // Quiet audio - calculate gain to reach target
         target_gain = NORM_TARGET / envelope_fraction;
         if (target_gain > NORM_MAX_GAIN) {
            target_gain = NORM_MAX_GAIN;
         }
      }
      // else: loud enough or silent - use unity gain

      // Smooth gain transition (use release coefficient for both directions
      // to avoid pumping artifacts)
      if (target_gain > state->current_gain) {
         // Gain increasing - use attack (fast)
         state->current_gain += g_norm_attack_coeff * (target_gain - state->current_gain);
      } else {
         // Gain decreasing - use release (slow)
         state->current_gain += g_norm_release_coeff * (target_gain - state->current_gain);
      }

      // Apply smoothed gain with clipping protection
      int32_t output = (int32_t)(sample * state->current_gain);
      if (output > 32767) {
         output = 32767;
      } else if (output < -32768) {
         output = -32768;
      }
      audio_out[i] = (int16_t)output;

      gain_sum += state->current_gain;
   }

   float avg_gain = gain_sum / num_samples;

   // Log normalization (only occasionally to avoid spam)
   static int norm_log_counter = 0;
   if (avg_gain > 1.1f && ++norm_log_counter >= 50) {
      LOG_INFO("ASR: Audio normalized (envelope=%.1f%%, avg_gain=%.1fx)",
               (state->envelope / 32767.0f) * 100, avg_gain);
      norm_log_counter = 0;
   }

   return avg_gain;
}

/* Timing callback adapter: common lib signature -> daemon metrics */
#ifndef ENABLE_VOSK
static void whisper_timing_callback(double processing_time_ms, double rtf, void *user_data) {
   (void)user_data;
   metrics_record_asr_timing(processing_time_ms, rtf);
}
#endif

/**
 * @brief ASR context structure
 *
 * Engine selection is compile-time (#ifdef ENABLE_VOSK), so dispatch
 * is direct calls rather than function pointers.
 */
struct asr_context {
   asr_engine_type_t engine_type;
#ifdef ENABLE_VOSK
   void *engine_context;
#else
   whisper_asr_context_t *whisper_ctx;
#endif
   norm_state_t norm_state;
};

asr_context_t *asr_init(asr_engine_type_t engine_type, const char *model_path, int sample_rate) {
   if (!model_path) {
      LOG_ERROR("ASR: model_path cannot be NULL");
      return NULL;
   }

   asr_context_t *ctx = (asr_context_t *)calloc(1, sizeof(asr_context_t));
   if (!ctx) {
      LOG_ERROR("ASR: Failed to allocate context");
      return NULL;
   }

   ctx->engine_type = engine_type;

   switch (engine_type) {
#ifdef ENABLE_VOSK
      case ASR_ENGINE_VOSK:
         LOG_INFO("ASR: Initializing Vosk engine (model: %s, sample_rate: %d)", model_path,
                  sample_rate);
         ctx->engine_context = asr_vosk_init(model_path, sample_rate);
         if (!ctx->engine_context) {
            LOG_ERROR("ASR: Vosk initialization failed");
            free(ctx);
            return NULL;
         }
         break;
#else
      case ASR_ENGINE_WHISPER: {
         LOG_INFO("ASR: Initializing Whisper engine (model: %s, sample_rate: %d)", model_path,
                  sample_rate);
         asr_whisper_config_t wcfg = asr_whisper_default_config();
         wcfg.model_path = model_path;
         wcfg.sample_rate = sample_rate;
#ifdef HAVE_CUDA_GPU
         wcfg.use_gpu = 1;
#endif
         ctx->whisper_ctx = asr_whisper_init(&wcfg);
         if (!ctx->whisper_ctx) {
            LOG_ERROR("ASR: Whisper initialization failed");
            free(ctx);
            return NULL;
         }
         asr_whisper_set_timing_callback(ctx->whisper_ctx, whisper_timing_callback, NULL);
         break;
      }
#endif

      default:
         LOG_ERROR("ASR: Unknown engine type: %d", engine_type);
         free(ctx);
         return NULL;
   }

   // Initialize per-context normalization state
   ctx->norm_state.current_gain = 1.0f;
   ctx->norm_state.envelope = 0.0f;

   LOG_INFO("ASR: %s engine initialized successfully", asr_engine_name(engine_type));
   return ctx;
}

asr_result_t *asr_process_partial(asr_context_t *ctx,
                                  const int16_t *audio_data,
                                  size_t num_samples) {
   if (!ctx || !audio_data) {
      LOG_ERROR("ASR: Invalid parameters to asr_process_partial");
      return NULL;
   }

   // Record pre-normalization audio (if recording enabled)
   asr_record_pre_samples(audio_data, num_samples);

   // Normalize audio for better ASR accuracy (using per-context state)
   const int16_t *normalized_audio = audio_data;
#if NORM_ENABLED
   if (num_samples <= NORM_BUFFER_SIZE) {
      normalize_audio_for_asr(&ctx->norm_state, audio_data, g_norm_buffer, num_samples);
      normalized_audio = g_norm_buffer;
   } else {
      LOG_WARNING("ASR: Audio chunk too large for normalization (%zu > %d), using raw audio",
                  num_samples, NORM_BUFFER_SIZE);
   }
#endif

   // Record post-normalization audio (if recording enabled)
   asr_record_post_samples(normalized_audio, num_samples);

#ifdef ENABLE_VOSK
   return asr_vosk_process_partial(ctx->engine_context, normalized_audio, num_samples);
#else
   return asr_whisper_process(ctx->whisper_ctx, normalized_audio, num_samples);
#endif
}

asr_result_t *asr_finalize(asr_context_t *ctx) {
   if (!ctx) {
      LOG_ERROR("ASR: Invalid context in asr_finalize");
      return NULL;
   }

#ifdef ENABLE_VOSK
   asr_result_t *result = asr_vosk_finalize(ctx->engine_context);
#else
   asr_result_t *result = asr_whisper_finalize(ctx->whisper_ctx);
#endif

   // Stop recording after finalize to create complete per-utterance WAV files
   if (asr_is_recording()) {
      asr_stop_recording();
   }

   return result;
}

int asr_reset(asr_context_t *ctx) {
   if (!ctx) {
      LOG_ERROR("ASR: Invalid context in asr_reset");
      return ASR_FAILURE;
   }

   // Stop any previous recording before starting new one
   if (asr_is_recording()) {
      asr_stop_recording();
   }

   // Start new recording for this utterance (if enabled)
   if (asr_is_recording_enabled()) {
      asr_start_recording();
   }

   // Reset per-context normalization state for new utterance
   ctx->norm_state.current_gain = 1.0f;
   ctx->norm_state.envelope = 0.0f;

#ifdef ENABLE_VOSK
   return asr_vosk_reset(ctx->engine_context);
#else
   return asr_whisper_reset(ctx->whisper_ctx);
#endif
}

void asr_result_free(asr_result_t *result) {
   if (!result) {
      return;
   }

   if (result->text) {
      free(result->text);
   }

   free(result);
}

void asr_cleanup(asr_context_t *ctx) {
   if (!ctx) {
      return;
   }

   LOG_INFO("ASR: Cleaning up %s engine", asr_engine_name(ctx->engine_type));

#ifdef ENABLE_VOSK
   if (ctx->engine_context) {
      asr_vosk_cleanup(ctx->engine_context);
   }
#else
   if (ctx->whisper_ctx) {
      asr_whisper_cleanup(ctx->whisper_ctx);
   }
#endif

   free(ctx);
}

const char *asr_engine_name(asr_engine_type_t engine_type) {
   switch (engine_type) {
#ifdef ENABLE_VOSK
      case ASR_ENGINE_VOSK:
         return "Vosk";
#else
      case ASR_ENGINE_WHISPER:
         return "Whisper";
#endif
      default:
         return "Unknown";
   }
}

asr_engine_type_t asr_get_engine_type(asr_context_t *ctx) {
   if (!ctx) {
      LOG_ERROR("ASR: asr_get_engine_type() called with NULL context");
      return (asr_engine_type_t)-1;
   }
   return ctx->engine_type;
}
