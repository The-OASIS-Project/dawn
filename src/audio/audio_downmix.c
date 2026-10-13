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
 * the project author(s).
 *
 * Multichannel-to-stereo downmix for the audio decoders (ITU-R BS.775).
 */

#include "audio_downmix.h"

#include <string.h>

#include "dawn_error.h"

/* Speaker positions a channel can carry. */
typedef enum {
   SPK_FL,
   SPK_FR,
   SPK_FC,
   SPK_LFE,
   SPK_BL,
   SPK_BR,
   SPK_SL,
   SPK_SR,
   SPK_BC,
} speaker_t;

/* -3 dB, the BS.775 weight for the centre and the surrounds. */
#define DOWNMIX_MINUS_3DB 0.70710678f

/* Default layouts by channel count (index = channels - 3), from the FLAC format
 * spec ("Channels") and the Vorbis I spec (section 4.3.9). */
static const speaker_t k_flac_layout[][AUDIO_DOWNMIX_MAX_CHANNELS] = {
   { SPK_FL, SPK_FR, SPK_FC },
   { SPK_FL, SPK_FR, SPK_BL, SPK_BR },
   { SPK_FL, SPK_FR, SPK_FC, SPK_BL, SPK_BR },
   { SPK_FL, SPK_FR, SPK_FC, SPK_LFE, SPK_BL, SPK_BR },
   { SPK_FL, SPK_FR, SPK_FC, SPK_LFE, SPK_BC, SPK_SL, SPK_SR },
   { SPK_FL, SPK_FR, SPK_FC, SPK_LFE, SPK_BL, SPK_BR, SPK_SL, SPK_SR },
};

static const speaker_t k_vorbis_layout[][AUDIO_DOWNMIX_MAX_CHANNELS] = {
   { SPK_FL, SPK_FC, SPK_FR },
   { SPK_FL, SPK_FR, SPK_BL, SPK_BR },
   { SPK_FL, SPK_FC, SPK_FR, SPK_BL, SPK_BR },
   { SPK_FL, SPK_FC, SPK_FR, SPK_BL, SPK_BR, SPK_LFE },
   { SPK_FL, SPK_FC, SPK_FR, SPK_SL, SPK_SR, SPK_BC, SPK_LFE },
   { SPK_FL, SPK_FC, SPK_FR, SPK_SL, SPK_SR, SPK_BL, SPK_BR, SPK_LFE },
};

static void speaker_weights(speaker_t spk, float *left, float *right) {
   switch (spk) {
      case SPK_FL:
         *left = 1.0f;
         *right = 0.0f;
         break;
      case SPK_FR:
         *left = 0.0f;
         *right = 1.0f;
         break;
      case SPK_FC:
         *left = DOWNMIX_MINUS_3DB;
         *right = DOWNMIX_MINUS_3DB;
         break;
      case SPK_BL:
      case SPK_SL:
         *left = DOWNMIX_MINUS_3DB;
         *right = 0.0f;
         break;
      case SPK_BR:
      case SPK_SR:
         *left = 0.0f;
         *right = DOWNMIX_MINUS_3DB;
         break;
      case SPK_BC:
         /* A rear centre is a surround split across both sides. */
         *left = DOWNMIX_MINUS_3DB * DOWNMIX_MINUS_3DB;
         *right = DOWNMIX_MINUS_3DB * DOWNMIX_MINUS_3DB;
         break;
      case SPK_LFE:
      default:
         /* BS.775 drops the LFE; most stereo playback can't reproduce it. */
         *left = 0.0f;
         *right = 0.0f;
         break;
   }
}

int audio_downmix_init(audio_downmix_t *dm, int channels, audio_channel_order_t order) {
   if (!dm || channels < 3 || channels > AUDIO_DOWNMIX_MAX_CHANNELS) {
      return FAILURE;
   }

   const speaker_t *layout = (order == AUDIO_CHANNEL_ORDER_VORBIS) ? k_vorbis_layout[channels - 3]
                                                                   : k_flac_layout[channels - 3];
   memset(dm, 0, sizeof(*dm));
   dm->channels = channels;

   float left_sum = 0.0f;
   float right_sum = 0.0f;
   for (int ch = 0; ch < channels; ch++) {
      speaker_weights(layout[ch], &dm->left[ch], &dm->right[ch]);
      left_sum += dm->left[ch];
      right_sum += dm->right[ch];
   }

   /* Normalize so every input at full scale, in phase, still fits. */
   for (int ch = 0; ch < channels; ch++) {
      if (left_sum > 1.0f) {
         dm->left[ch] /= left_sum;
      }
      if (right_sum > 1.0f) {
         dm->right[ch] /= right_sum;
      }
   }
   return SUCCESS;
}

static int16_t to_int16(float v) {
   float s = v * 32767.0f;
   if (s != s) {
      return 0; /* NaN from a malformed stream: casting it is undefined */
   }
   if (s > 32767.0f) {
      return INT16_MAX;
   }
   if (s < -32768.0f) {
      return INT16_MIN;
   }
   return (int16_t)s;
}

void audio_downmix_float(const audio_downmix_t *dm,
                         const float *const *planes,
                         size_t frames,
                         int16_t *out) {
   for (size_t i = 0; i < frames; i++) {
      float l = 0.0f;
      float r = 0.0f;
      for (int ch = 0; ch < dm->channels; ch++) {
         float s = planes[ch][i];
         l += dm->left[ch] * s;
         r += dm->right[ch] * s;
      }
      out[i * 2] = to_int16(l);
      out[i * 2 + 1] = to_int16(r);
   }
}

void audio_downmix_int32(const audio_downmix_t *dm,
                         const int32_t *const *planes,
                         unsigned bits_per_sample,
                         size_t frames,
                         int16_t *out) {
   /* Full scale for this depth maps to 1.0. */
   const float scale = 1.0f / (float)(1ULL << (bits_per_sample - 1));

   for (size_t i = 0; i < frames; i++) {
      float l = 0.0f;
      float r = 0.0f;
      for (int ch = 0; ch < dm->channels; ch++) {
         float s = (float)planes[ch][i] * scale;
         l += dm->left[ch] * s;
         r += dm->right[ch] * s;
      }
      out[i * 2] = to_int16(l);
      out[i * 2 + 1] = to_int16(r);
   }
}
