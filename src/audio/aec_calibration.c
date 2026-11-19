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
 * AEC Delay Calibration Implementation
 *
 * Uses cross-correlation to measure speaker-to-microphone acoustic delay
 * during boot greeting playback.
 */

#include "audio/aec_calibration.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"

/* Maximum supported delay search (buffer constraint) */
#define AEC_CAL_MAX_DELAY_LIMIT_MS 500

/* Calibration state */
typedef struct {
   /* Configuration */
   int sample_rate;
   int max_delay_samples;
   int max_delay_ms;

   /* Buffers - sized for ~2 seconds of audio plus margin */
   int16_t *ref_buffer;
   int16_t *mic_buffer;
   size_t ref_buffer_size; /* Max samples */
   size_t mic_buffer_size; /* Max samples */
   size_t ref_count;       /* Current sample count */
   size_t mic_count;       /* Current sample count */

   /* Thread safety */
   pthread_mutex_t ref_mutex;
   pthread_mutex_t mic_mutex;
   atomic_bool is_active;
   atomic_bool is_initialized;

   /* Results */
   float last_correlation;
} cal_state_t;

static cal_state_t g_cal = { 0 };

/* Pending calibration request flag - managed separately from calibration state
 * to decouple request (from main/dawn.c) from execution (in TTS callbacks) */
static atomic_bool g_calibration_pending = false;

/*
 * Compute normalized cross-correlation at a single lag
 *
 * Returns correlation coefficient in [-1.0, 1.0] range
 */
static float compute_correlation_at_lag(const int16_t *ref,
                                        size_t ref_len,
                                        const int16_t *mic,
                                        size_t mic_len,
                                        int lag) {
   /* Determine valid overlap region */
   size_t start = (lag > 0) ? 0 : (size_t)(-lag);
   size_t end = ref_len;
   if ((size_t)lag + ref_len > mic_len) {
      end = mic_len - (size_t)lag;
   }
   if (end <= start) {
      return 0.0f;
   }

   /* Compute correlation with Welford's online algorithm for numerical stability */
   double sum_xy = 0.0;
   double sum_x2 = 0.0;
   double sum_y2 = 0.0;

   for (size_t i = start; i < end; i++) {
      double x = (double)ref[i];
      double y = (double)mic[i + (size_t)lag];
      sum_xy += x * y;
      sum_x2 += x * x;
      sum_y2 += y * y;
   }

   double denom = sqrt(sum_x2 * sum_y2);
   if (denom < 1e-10) {
      return 0.0f;
   }

   return (float)(sum_xy / denom);
}

int aec_cal_init(int sample_rate, int max_delay_ms) {
   if (sample_rate <= 0 || max_delay_ms <= 0) {
      OLOG_ERROR("AEC calibration: invalid parameters (rate=%d, max_delay=%d)", sample_rate,
                 max_delay_ms);
      return AEC_CAL_ERR_INVALID_PARAM;
   }

   /* Validate max_delay_ms against buffer constraints.
    * With 2-second buffers, we need room for:
    * - min greeting duration (500ms for reliable correlation)
    * - delay search range (max_delay_ms)
    * Total: greeting + delay <= 2000ms, so max_delay <= 1500ms
    * We use a conservative 500ms limit to ensure good results. */
   if (max_delay_ms > AEC_CAL_MAX_DELAY_LIMIT_MS) {
      OLOG_ERROR("AEC calibration: max_delay_ms (%d) exceeds limit (%d)", max_delay_ms,
                 AEC_CAL_MAX_DELAY_LIMIT_MS);
      return AEC_CAL_ERR_INVALID_PARAM;
   }

   /* Clean up any existing state */
   aec_cal_cleanup();

   g_cal.sample_rate = sample_rate;
   g_cal.max_delay_ms = max_delay_ms;
   g_cal.max_delay_samples = (sample_rate * max_delay_ms) / 1000;

   /* Allocate buffers for ~2 seconds of audio */
   g_cal.ref_buffer_size = (size_t)(sample_rate * 2);
   g_cal.mic_buffer_size = (size_t)(sample_rate * 2) + (size_t)g_cal.max_delay_samples;

   g_cal.ref_buffer = (int16_t *)malloc(g_cal.ref_buffer_size * sizeof(int16_t));
   g_cal.mic_buffer = (int16_t *)malloc(g_cal.mic_buffer_size * sizeof(int16_t));

   if (!g_cal.ref_buffer || !g_cal.mic_buffer) {
      OLOG_ERROR("AEC calibration: failed to allocate buffers (ref=%zu, mic=%zu KB)",
                 g_cal.ref_buffer_size * sizeof(int16_t) / 1024,
                 g_cal.mic_buffer_size * sizeof(int16_t) / 1024);
      free(g_cal.ref_buffer);
      free(g_cal.mic_buffer);
      g_cal.ref_buffer = NULL;
      g_cal.mic_buffer = NULL;
      return AEC_CAL_ERR_OUT_OF_MEMORY;
   }

   if (pthread_mutex_init(&g_cal.ref_mutex, NULL) != 0) {
      OLOG_ERROR("AEC calibration: failed to init ref mutex");
      free(g_cal.ref_buffer);
      free(g_cal.mic_buffer);
      return AEC_CAL_ERR_OUT_OF_MEMORY;
   }

   if (pthread_mutex_init(&g_cal.mic_mutex, NULL) != 0) {
      OLOG_ERROR("AEC calibration: failed to init mic mutex");
      pthread_mutex_destroy(&g_cal.ref_mutex);
      free(g_cal.ref_buffer);
      free(g_cal.mic_buffer);
      return AEC_CAL_ERR_OUT_OF_MEMORY;
   }

   g_cal.ref_count = 0;
   g_cal.mic_count = 0;
   g_cal.last_correlation = 0.0f;
   atomic_store(&g_cal.is_active, false);
   atomic_store(&g_cal.is_initialized, true);

   OLOG_INFO("AEC calibration: initialized (rate=%dHz, max_delay=%dms, buffers=%zu KB)",
             sample_rate, max_delay_ms,
             (g_cal.ref_buffer_size + g_cal.mic_buffer_size) * sizeof(int16_t) / 1024);

   return AEC_CAL_SUCCESS;
}

void aec_cal_start(void) {
   if (!atomic_load(&g_cal.is_initialized)) {
      return;
   }

   /* Clear buffers */
   pthread_mutex_lock(&g_cal.ref_mutex);
   g_cal.ref_count = 0;
   pthread_mutex_unlock(&g_cal.ref_mutex);

   pthread_mutex_lock(&g_cal.mic_mutex);
   g_cal.mic_count = 0;
   pthread_mutex_unlock(&g_cal.mic_mutex);

   atomic_store(&g_cal.is_active, true);
   OLOG_INFO("AEC calibration: started capture");
}

void aec_cal_add_reference(const int16_t *samples, size_t num_samples) {
   if (!atomic_load(&g_cal.is_active) || !samples || num_samples == 0) {
      return;
   }

   pthread_mutex_lock(&g_cal.ref_mutex);

   size_t space = g_cal.ref_buffer_size - g_cal.ref_count;
   size_t to_copy = (num_samples < space) ? num_samples : space;

   if (to_copy > 0) {
      memcpy(&g_cal.ref_buffer[g_cal.ref_count], samples, to_copy * sizeof(int16_t));
      g_cal.ref_count += to_copy;
   }

   pthread_mutex_unlock(&g_cal.ref_mutex);
}

void aec_cal_add_mic(const int16_t *samples, size_t num_samples) {
   if (!atomic_load(&g_cal.is_active) || !samples || num_samples == 0) {
      return;
   }

   pthread_mutex_lock(&g_cal.mic_mutex);

   size_t space = g_cal.mic_buffer_size - g_cal.mic_count;
   size_t to_copy = (num_samples < space) ? num_samples : space;

   if (to_copy > 0) {
      memcpy(&g_cal.mic_buffer[g_cal.mic_count], samples, to_copy * sizeof(int16_t));
      g_cal.mic_count += to_copy;
   }

   pthread_mutex_unlock(&g_cal.mic_mutex);
}

int aec_cal_finish(int *delay_ms) {
   if (!delay_ms) {
      return AEC_CAL_ERR_INVALID_PARAM;
   }

   *delay_ms = 0;

   if (!atomic_load(&g_cal.is_active)) {
      OLOG_WARNING("AEC calibration: finish called but not active");
      return AEC_CAL_ERR_NOT_ACTIVE;
   }

   atomic_store(&g_cal.is_active, false);

   /* Take snapshots of buffer counts under lock */
   pthread_mutex_lock(&g_cal.ref_mutex);
   size_t ref_count = g_cal.ref_count;
   pthread_mutex_unlock(&g_cal.ref_mutex);

   pthread_mutex_lock(&g_cal.mic_mutex);
   size_t mic_count = g_cal.mic_count;
   pthread_mutex_unlock(&g_cal.mic_mutex);

   OLOG_INFO("AEC calibration: analyzing %zu ref samples, %zu mic samples", ref_count, mic_count);

   /* No microphone audio at all: the mic wasn't there (missing at start, or
    * lost).  Nothing was measured; the caller asks again. */
   if (mic_count == 0) {
      OLOG_WARNING("AEC calibration: no microphone audio; calibrating again later");
      return AEC_CAL_ERR_NO_MIC_DATA;
   }

   /* Calculate RMS levels for diagnostic logging.
    *
    * RMS (Root Mean Square) indicates the average signal energy:
    * - ref_rms: TTS reference signal energy. Should be high (>1000) if audio played.
    *   Low values indicate muted playback or missing reference data.
    * - mic_rms: Microphone signal energy. Should be moderate (>100) if echo present.
    *   Very low values with high ref_rms suggest mic muted, headphones, or timing issue.
    *
    * These are diagnostic only - correlation quality check (AEC_CAL_MIN_CORRELATION)
    * is the authoritative validation. RMS helps identify WHY correlation might be low. */
   double ref_sum_sq = 0.0, mic_sum_sq = 0.0;
   size_t check_samples = (ref_count < mic_count) ? ref_count : mic_count;
   if (check_samples > 48000)
      check_samples = 48000; /* Check first 1 second */
   for (size_t i = 0; i < check_samples; i++) {
      ref_sum_sq += (double)g_cal.ref_buffer[i] * g_cal.ref_buffer[i];
      mic_sum_sq += (double)g_cal.mic_buffer[i] * g_cal.mic_buffer[i];
   }
   double ref_rms = sqrt(ref_sum_sq / check_samples);
   double mic_rms = sqrt(mic_sum_sq / check_samples);
   OLOG_INFO("AEC calibration: RMS levels - ref=%.1f, mic=%.1f (checked %zu samples)", ref_rms,
             mic_rms, check_samples);

   /* Need at least 100ms of reference audio */
   size_t min_samples = (size_t)(g_cal.sample_rate / 10);
   if (ref_count < min_samples) {
      OLOG_WARNING("AEC calibration: insufficient reference data (%zu < %zu)", ref_count,
                   min_samples);
      return AEC_CAL_ERR_INSUFFICIENT_DATA;
   }

   /* Handle timing differences between mic capture and TTS playback.
    *
    * With ALSA: mic capture typically lags behind TTS (mic_count < ref_count)
    * With PulseAudio: mic capture may run ahead (mic_count >= ref_count) because
    * PA capture starts immediately while PA playback has buffering latency.
    *
    * In all cases, we need to ensure enough search range for the acoustic delay.
    * We reduce usable_ref_count to allow searching up to max_delay_samples.
    */
   size_t delay_margin = (size_t)g_cal.max_delay_samples;
   size_t usable_ref_count = ref_count;
   const int16_t *mic_ptr = g_cal.mic_buffer;
   size_t effective_mic_count = mic_count;

   if (mic_count >= ref_count) {
      /* Mic has equal or more samples - need to ensure full delay search range.
       * With PulseAudio, even if counts are similar, playback latency means
       * the TTS echo could be up to max_delay_samples later in mic buffer. */
      if (ref_count > delay_margin + min_samples) {
         /* Reduce reference to allow full delay search */
         usable_ref_count = ref_count - delay_margin;
         OLOG_INFO(
             "AEC calibration: mic_count >= ref_count (%zu >= %zu), using %zu ref for full search",
             mic_count, ref_count, usable_ref_count);
      } else {
         /* Reference too short to reduce, use as-is but search range limited */
         usable_ref_count = ref_count;
         OLOG_INFO("AEC calibration: short reference, limited search range");
      }
   } else { /* mic_count < ref_count */
      /* Mic capture lagged - use reduced reference length */
      OLOG_INFO("AEC calibration: mic capture lagged (mic=%zu, ref=%zu), adjusting", mic_count,
                ref_count);
      /* Need at least 50% of original reference for reliable correlation.
       * Reduced from 80% to handle mic capture pipeline latency - the mic
       * thread may lag behind TTS playback by 700ms+ depending on:
       * - Audio device startup latency
       * - ALSA/PulseAudio buffering
       * - Thread scheduling delays
       * Cross-correlation can still find delay with 1+ seconds of audio. */
      if (mic_count < (ref_count / 2)) {
         OLOG_WARNING("AEC calibration: insufficient mic data (%zu < 50%% of %zu)", mic_count,
                      ref_count);
         return AEC_CAL_ERR_INSUFFICIENT_DATA;
      }
      /* Use mic_count minus delay search margin as reference length */
      if (mic_count <= delay_margin) {
         OLOG_WARNING("AEC calibration: mic data too short for delay search");
         return AEC_CAL_ERR_INSUFFICIENT_DATA;
      }
      usable_ref_count = mic_count - delay_margin;
      if (usable_ref_count < min_samples) {
         OLOG_WARNING("AEC calibration: usable reference too short (%zu < %zu)", usable_ref_count,
                      min_samples);
         return AEC_CAL_ERR_INSUFFICIENT_DATA;
      }
      effective_mic_count = mic_count;
      OLOG_INFO("AEC calibration: using %zu of %zu reference samples", usable_ref_count, ref_count);
   }

   /* Limit search range to what we can actually correlate */
   int max_lag = (int)(effective_mic_count - usable_ref_count);
   if (max_lag > g_cal.max_delay_samples) {
      max_lag = g_cal.max_delay_samples;
   }
   if (max_lag <= 0) {
      OLOG_WARNING("AEC calibration: not enough mic data for correlation (max_lag=%d)", max_lag);
      return AEC_CAL_ERR_INSUFFICIENT_DATA;
   }

   /* Search for peak correlation */
   float best_corr = -1.0f;
   int best_lag = 0;
   float second_best_corr = -1.0f;
   int second_best_lag = 0;

   /* Calculate minimum search lag from physical constraints.
    * Delays below AEC_CAL_MIN_DELAY_MS are physically impossible and
    * searching there finds false peaks from DC offset or crosstalk. */
   int min_lag = (g_cal.sample_rate * AEC_CAL_MIN_DELAY_MS) / 1000;
   if (min_lag >= max_lag) {
      OLOG_WARNING("AEC calibration: search range too small (min=%d, max=%d samples)", min_lag,
                   max_lag);
      return AEC_CAL_ERR_INSUFFICIENT_DATA;
   }

   OLOG_INFO("AEC calibration: searching delay range %d-%d ms (%d-%d samples)",
             AEC_CAL_MIN_DELAY_MS, (max_lag * 1000) / g_cal.sample_rate, min_lag, max_lag);

   /* Step through lags - use coarse search first, then refine */
   int step = (max_lag > 1000) ? 10 : 1;

   for (int lag = min_lag; lag <= max_lag; lag += step) {
      float corr = compute_correlation_at_lag(g_cal.ref_buffer, usable_ref_count, mic_ptr,
                                              effective_mic_count, lag);
      if (corr > best_corr) {
         second_best_corr = best_corr;
         second_best_lag = best_lag;
         best_corr = corr;
         best_lag = lag;
      } else if (corr > second_best_corr) {
         second_best_corr = corr;
         second_best_lag = lag;
      }
   }

   /* Fine search around best lag if we used coarse search */
   if (step > 1 && best_lag > 0) {
      int fine_start = (best_lag - step > min_lag) ? (best_lag - step) : min_lag;
      int fine_end = (best_lag + step < max_lag) ? (best_lag + step) : max_lag;

      for (int lag = fine_start; lag <= fine_end; lag++) {
         float corr = compute_correlation_at_lag(g_cal.ref_buffer, usable_ref_count, mic_ptr,
                                                 effective_mic_count, lag);
         if (corr > best_corr) {
            best_corr = corr;
            best_lag = lag;
         }
      }
   }

   g_cal.last_correlation = best_corr;

   /* Check correlation quality */
   if (best_corr < AEC_CAL_MIN_CORRELATION) {
      OLOG_WARNING("AEC calibration: weak correlation (%.3f < %.3f) - possibly muted speakers",
                   best_corr, AEC_CAL_MIN_CORRELATION);
      return AEC_CAL_ERR_LOW_CORRELATION;
   }

   /* Check for ambiguous peaks - warn but proceed with best peak (approximate delay helps AEC) */
   if (second_best_corr > 0 && second_best_corr > (best_corr * AEC_CAL_AMBIGUITY_RATIO)) {
      int best_lag_ms = (best_lag * 1000) / g_cal.sample_rate;
      int second_lag_ms = (second_best_lag * 1000) / g_cal.sample_rate;
      OLOG_WARNING("AEC calibration: ambiguous peaks (%.3f@%dms vs %.3f@%dms) - reverberant room, "
                   "using best",
                   best_corr, best_lag_ms, second_best_corr, second_lag_ms);
      /* Continue anyway - approximate delay is better than default */
   }

   /* Convert lag to milliseconds */
   int measured_delay = (best_lag * 1000) / g_cal.sample_rate;

   /* Sanity check - delay should be reasonable */
   if (measured_delay < 0 || measured_delay > g_cal.max_delay_ms) {
      OLOG_WARNING("AEC calibration: delay out of range (%d ms)", measured_delay);
      return AEC_CAL_ERR_OUT_OF_RANGE;
   }

   *delay_ms = measured_delay;
   OLOG_INFO("AEC calibration: SUCCESS - measured delay = %d ms (correlation = %.3f)",
             measured_delay, best_corr);

   return AEC_CAL_SUCCESS;
}

bool aec_cal_is_active(void) {
   return atomic_load(&g_cal.is_active);
}

bool aec_cal_is_initialized(void) {
   return atomic_load(&g_cal.is_initialized);
}

float aec_cal_get_last_correlation(void) {
   return g_cal.last_correlation;
}

void aec_cal_cleanup(void) {
   if (!atomic_load(&g_cal.is_initialized)) {
      return;
   }

   /* Stop any active calibration */
   atomic_store(&g_cal.is_active, false);

   /* Free buffers with lock held to avoid races */
   pthread_mutex_lock(&g_cal.ref_mutex);
   free(g_cal.ref_buffer);
   g_cal.ref_buffer = NULL;
   g_cal.ref_count = 0;
   pthread_mutex_unlock(&g_cal.ref_mutex);

   pthread_mutex_lock(&g_cal.mic_mutex);
   free(g_cal.mic_buffer);
   g_cal.mic_buffer = NULL;
   g_cal.mic_count = 0;
   pthread_mutex_unlock(&g_cal.mic_mutex);

   pthread_mutex_destroy(&g_cal.ref_mutex);
   pthread_mutex_destroy(&g_cal.mic_mutex);

   atomic_store(&g_cal.is_initialized, false);

   OLOG_INFO("AEC calibration: cleaned up");
}

void aec_cal_set_pending(void) {
   atomic_store(&g_calibration_pending, true);
}

bool aec_cal_check_and_clear_pending(void) {
   return atomic_exchange(&g_calibration_pending, false);
}

bool aec_cal_is_pending(void) {
   return atomic_load(&g_calibration_pending);
}
