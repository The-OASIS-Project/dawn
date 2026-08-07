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
 * Audio Resampler Implementation
 *
 * Uses libsamplerate (Secret Rabbit Code) for high-quality resampling.
 * All buffers are pre-allocated at creation time.
 */

#include "audio/resampler.h"

#include <samplerate.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"

struct resampler_t {
   SRC_STATE *src_state;
   double ratio;
   int channels;

   // Pre-allocated buffers (sized for RESAMPLER_MAX_SAMPLES)
   float *in_float;
   float *out_float;
   size_t buffer_capacity;
};

resampler_t *resampler_create(int src_rate, int dst_rate, int channels) {
   return resampler_create_ex(src_rate, dst_rate, channels, RESAMPLER_QUALITY_BEST);
}

resampler_t *resampler_create_ex(int src_rate,
                                 int dst_rate,
                                 int channels,
                                 resampler_quality_t quality) {
   if (src_rate <= 0 || dst_rate <= 0 || channels <= 0) {
      OLOG_ERROR("Invalid resampler parameters: src=%d dst=%d ch=%d", src_rate, dst_rate, channels);
      return NULL;
   }

   resampler_t *rs = calloc(1, sizeof(resampler_t));
   if (!rs) {
      OLOG_ERROR("Failed to allocate resampler");
      return NULL;
   }

   rs->ratio = (double)dst_rate / (double)src_rate;
   rs->channels = channels;

   // BEST for cleanest 48kHz→16kHz downsampling (ASR aliasing → "underwater" audio)
   // and hi-fi music. MEDIUM (~4-5x cheaper) is inaudible for speech and keeps a large
   // TTS resample from spiking CPU and starving the real-time music stream thread.
   int converter = (quality == RESAMPLER_QUALITY_MEDIUM) ? SRC_SINC_MEDIUM_QUALITY
                                                         : SRC_SINC_BEST_QUALITY;

   int error;
   rs->src_state = src_new(converter, channels, &error);
   if (!rs->src_state) {
      OLOG_ERROR("Failed to create SRC state: %s", src_strerror(error));
      free(rs);
      return NULL;
   }

   // Pre-allocate for maximum expected size
   // Output can be larger than input if upsampling, so size for worst case
   size_t max_out = (size_t)(RESAMPLER_MAX_SAMPLES * rs->ratio) + 64;
   rs->buffer_capacity = (max_out > RESAMPLER_MAX_SAMPLES) ? max_out : RESAMPLER_MAX_SAMPLES;

   rs->in_float = malloc(rs->buffer_capacity * sizeof(float));
   rs->out_float = malloc(rs->buffer_capacity * sizeof(float));

   if (!rs->in_float || !rs->out_float) {
      OLOG_ERROR("Failed to allocate resampler buffers (%zu samples)", rs->buffer_capacity);
      src_delete(rs->src_state);
      free(rs->in_float);
      free(rs->out_float);
      free(rs);
      return NULL;
   }

   OLOG_INFO("Resampler created: %d -> %d Hz (ratio %.4f, buffer %zu samples)", src_rate, dst_rate,
             rs->ratio, rs->buffer_capacity);
   return rs;
}

void resampler_destroy(resampler_t *rs) {
   if (!rs)
      return;

   if (rs->src_state) {
      src_delete(rs->src_state);
   }
   free(rs->in_float);
   free(rs->out_float);
   free(rs);
}

size_t resampler_process(resampler_t *rs,
                         const int16_t *in,
                         size_t in_samples,
                         int16_t *out,
                         size_t out_samples_max) {
   if (!rs || !in || !out || in_samples == 0) {
      return 0;
   }

   // Enforce maximum to prevent buffer overflow
   if (in_samples > RESAMPLER_MAX_SAMPLES) {
      OLOG_ERROR("Resampler input too large: %zu > %d", in_samples, RESAMPLER_MAX_SAMPLES);
      return 0;
   }

   // Verify output buffer is sufficient
   size_t required_out = resampler_get_output_size(rs, in_samples);
   if (out_samples_max < required_out) {
      OLOG_ERROR("Resampler output buffer too small: %zu < %zu", out_samples_max, required_out);
      return 0;
   }

   // Convert int16 to float (libsamplerate works with float)
   src_short_to_float_array(in, rs->in_float, (int)in_samples);

   // Perform resampling
   SRC_DATA src_data = { .data_in = rs->in_float,
                         .data_out = rs->out_float,
                         .input_frames = (long)(in_samples / rs->channels),
                         .output_frames = (long)(out_samples_max / rs->channels),
                         .src_ratio = rs->ratio,
                         .end_of_input = 0 };

   int error = src_process(rs->src_state, &src_data);
   if (error) {
      OLOG_ERROR("Resampler error: %s", src_strerror(error));
      return 0;
   }

   size_t out_samples = (size_t)src_data.output_frames_gen * rs->channels;

   // Convert float back to int16
   src_float_to_short_array(rs->out_float, out, (int)out_samples);

   return out_samples;
}

size_t resampler_get_output_size(resampler_t *rs, size_t in_samples) {
   if (!rs)
      return 0;

   // Guard against integer overflow (max ratio ~2.18x, so /4 is conservative)
   if (in_samples > SIZE_MAX / 4) {
      OLOG_ERROR("Resampler: Input size %zu too large, overflow risk", in_samples);
      return 0;
   }

   // Add generous margin for rounding, filter delay, and chunked processing
   // Each chunk can add rounding error, so margin scales with expected chunks
   size_t chunk_margin = (in_samples / RESAMPLER_MAX_SAMPLES + 1) * 16;
   return (size_t)(in_samples * rs->ratio) + 64 + chunk_margin;
}

void resampler_reset(resampler_t *rs) {
   if (!rs || !rs->src_state)
      return;
   src_reset(rs->src_state);
}
