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
 * DAWN Audio Converter - Configurable sample rate and channel output
 */

#include "audio/audio_converter.h"

#include <samplerate.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "logging.h"

/* =============================================================================
 * Config Accessor Functions
 * ============================================================================= */
unsigned int audio_conv_get_output_rate(void) {
   const dawn_config_t *cfg = config_get();
   if (cfg && cfg->audio.output_rate > 0) {
      return cfg->audio.output_rate;
   }
   return AUDIO_CONV_DEFAULT_OUTPUT_RATE;
}

unsigned int audio_conv_get_output_channels(void) {
   const dawn_config_t *cfg = config_get();
   if (cfg && cfg->audio.output_channels > 0) {
      return cfg->audio.output_channels;
   }
   return AUDIO_CONV_DEFAULT_OUTPUT_CHANNELS;
}

/* Internal converter structure */
struct audio_converter {
   unsigned int input_rate;
   unsigned int input_channels;
   unsigned int output_channels; /* Cached at creation time */
   double ratio;                 /* output_rate / input_rate */
   int needs_resample;           /* Rate conversion needed */
   int needs_channel_conv;       /* Channel conversion needed */
   SRC_STATE *resampler;         /* libsamplerate state (NULL if no resampling) */

   /* Intermediate buffers for conversion pipeline */
   float *float_in;  /* Input converted to float (for resampler) */
   float *float_out; /* Resampler output */
   size_t float_in_size;
   size_t float_out_size;
};

audio_converter_t *audio_converter_create(const audio_converter_params_t *params) {
   return audio_converter_create_ex(params, audio_conv_get_output_rate(),
                                    audio_conv_get_output_channels());
}

audio_converter_t *audio_converter_create_ex(const audio_converter_params_t *params,
                                             unsigned int output_rate,
                                             unsigned int output_channels) {
   if (!params || params->sample_rate == 0 || params->channels == 0 || params->channels > 2) {
      OLOG_ERROR("audio_converter: invalid params (rate=%u, ch=%u)",
                 params ? params->sample_rate : 0, params ? params->channels : 0);
      return NULL;
   }

   if (output_rate == 0 || output_channels == 0 || output_channels > 2) {
      OLOG_ERROR("audio_converter: invalid output params (rate=%u, ch=%u)", output_rate,
                 output_channels);
      return NULL;
   }

   audio_converter_t *conv = calloc(1, sizeof(audio_converter_t));
   if (!conv) {
      OLOG_ERROR("audio_converter: allocation failed");
      return NULL;
   }

   conv->input_rate = params->sample_rate;
   conv->input_channels = params->channels;
   conv->output_channels = output_channels; /* Cache for hot path */
   conv->ratio = (double)output_rate / (double)params->sample_rate;
   conv->needs_resample = (params->sample_rate != output_rate);
   conv->needs_channel_conv = (params->channels != output_channels);

   /* Create resampler if rate conversion needed */
   if (conv->needs_resample) {
      int error;
      /* Use SRC_SINC_FASTEST for good quality with low CPU */
      conv->resampler = src_new(SRC_SINC_FASTEST, params->channels, &error);
      if (!conv->resampler) {
         OLOG_ERROR("audio_converter: failed to create resampler: %s", src_strerror(error));
         free(conv);
         return NULL;
      }
   }

   /* Allocate intermediate buffers */
   /* Input buffer: max frames * input channels */
   conv->float_in_size = AUDIO_CONV_MAX_INPUT_FRAMES * params->channels;
   conv->float_in = malloc(conv->float_in_size * sizeof(float));

   /* Resampler output: max frames * ratio, in the input's channels (the
    * channel stage runs after it), with margin */
   conv->float_out_size = (size_t)((double)AUDIO_CONV_MAX_INPUT_FRAMES * conv->ratio * 1.1) *
                          params->channels;
   conv->float_out = malloc(conv->float_out_size * sizeof(float));

   if (!conv->float_in || !conv->float_out) {
      OLOG_ERROR("audio_converter: buffer allocation failed");
      audio_converter_destroy(conv);
      return NULL;
   }

   OLOG_INFO("audio_converter: created %uHz/%uch -> %uHz/%uch (ratio=%.4f, resample=%d, "
             "channel_conv=%d)",
             params->sample_rate, params->channels, output_rate, output_channels, conv->ratio,
             conv->needs_resample, conv->needs_channel_conv);

   return conv;
}

void audio_converter_destroy(audio_converter_t *conv) {
   if (!conv) {
      return;
   }

   if (conv->resampler) {
      src_delete(conv->resampler);
   }
   free(conv->float_in);
   free(conv->float_out);
   free(conv);
}

size_t audio_converter_max_output_frames(audio_converter_t *conv, size_t input_frames) {
   if (!conv) {
      return 0;
   }
   /* Add 10% margin for resampler rounding */
   return (size_t)((double)input_frames * conv->ratio * 1.1) + 16;
}

int audio_converter_process(audio_converter_t *conv,
                            const int16_t *input,
                            size_t input_frames,
                            int16_t *output,
                            size_t output_max_frames,
                            size_t *frames_out) {
   if (frames_out)
      *frames_out = 0;

   if (!conv || !input || !output || input_frames == 0 || !frames_out) {
      return 1;
   }

   if (input_frames > AUDIO_CONV_MAX_INPUT_FRAMES) {
      OLOG_ERROR("audio_converter: input too large (%zu > %d)", input_frames,
                 AUDIO_CONV_MAX_INPUT_FRAMES);
      return 1;
   }

   size_t output_frames = 0;
   size_t in_samples = input_frames * conv->input_channels;

   /* Step 1: Convert S16 to float for processing */
   src_short_to_float_array(input, conv->float_in, (int)in_samples);

   /* Step 2: Resample if needed */
   float *resampled = conv->float_in;
   size_t resampled_frames = input_frames;

   if (conv->needs_resample) {
      SRC_DATA src_data;
      src_data.data_in = conv->float_in;
      src_data.input_frames = (long)input_frames;
      src_data.data_out = conv->float_out;
      src_data.output_frames = (long)(conv->float_out_size / conv->input_channels);
      src_data.src_ratio = conv->ratio;
      src_data.end_of_input = 0;

      int error = src_process(conv->resampler, &src_data);
      if (error) {
         OLOG_ERROR("audio_converter: resample error: %s", src_strerror(error));
         return 1;
      }

      resampled = conv->float_out;
      resampled_frames = (size_t)src_data.output_frames_gen;
   }

   /* Step 3: Channel conversion */
   /* Use cached output_channels to avoid function call in hot path */
   unsigned int output_channels = conv->output_channels;

   if (output_max_frames < resampled_frames) {
      OLOG_ERROR("audio_converter: output buffer too small (%zu < %zu)", output_max_frames,
                 resampled_frames);
      return 1;
   }

   /* Float to S16, clamped; one or two channels in, one or two out (create
    * refuses anything else): mono is copied to both sides, stereo is averaged
    * down to mono */
   const unsigned int in_ch = conv->input_channels;
   for (size_t i = 0; i < resampled_frames; i++) {
      const float *frame = &resampled[i * in_ch];
      for (unsigned int c = 0; c < output_channels; c++) {
         float sample;
         if (in_ch == output_channels) {
            sample = frame[c];
         } else if (in_ch == 1) {
            sample = frame[0];
         } else {
            sample = 0.5f * (frame[0] + frame[1]);
         }
         if (sample > 1.0f) {
            sample = 1.0f;
         } else if (sample < -1.0f) {
            sample = -1.0f;
         }
         output[i * output_channels + c] = (int16_t)(sample * 32767.0f);
      }
   }
   output_frames = resampled_frames;

   *frames_out = output_frames;
   return 0;
}

void audio_converter_reset(audio_converter_t *conv) {
   if (conv && conv->resampler) {
      src_reset(conv->resampler);
   }
}

int audio_converter_needed(const audio_converter_params_t *params) {
   if (!params) {
      return 1;
   }
   return (params->sample_rate != audio_conv_get_output_rate() ||
           params->channels != audio_conv_get_output_channels());
}

int audio_converter_needed_ex(const audio_converter_params_t *params,
                              unsigned int output_rate,
                              unsigned int output_channels) {
   if (!params) {
      return 1;
   }
   return (params->sample_rate != output_rate || params->channels != output_channels);
}

double audio_converter_get_ratio(audio_converter_t *conv) {
   if (!conv) {
      return 1.0;
   }
   return conv->ratio;
}
