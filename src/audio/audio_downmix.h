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
 * Multichannel-to-stereo downmix for the audio decoders.
 *
 * Every decoder hands out mono or stereo (audio_decoder_info_t.channels), so a
 * 3-8 channel file is folded to stereo inside the decoder with ITU-R BS.775
 * weights: centre and surrounds at -3 dB, LFE dropped, each output normalized
 * so a full-scale input can't clip.  Internal to src/audio/.
 */

#ifndef AUDIO_DOWNMIX_H
#define AUDIO_DOWNMIX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Most channels a file may have and still be played (FLAC's own limit). */
#define AUDIO_DOWNMIX_MAX_CHANNELS 8

/** The channel order a format stores multichannel audio in. */
typedef enum {
   AUDIO_CHANNEL_ORDER_FLAC,   /**< FLAC / WAV: L R C LFE ... */
   AUDIO_CHANNEL_ORDER_VORBIS, /**< Vorbis: L C R ... LFE last */
} audio_channel_order_t;

/** Per-channel weights into the left and right outputs. */
typedef struct {
   int channels;
   float left[AUDIO_DOWNMIX_MAX_CHANNELS];
   float right[AUDIO_DOWNMIX_MAX_CHANNELS];
} audio_downmix_t;

/**
 * @brief Set up the weights for a channel count and order
 * @param dm Output weights
 * @param channels Input channels, 3 to AUDIO_DOWNMIX_MAX_CHANNELS
 * @param order The format's channel order
 * @return SUCCESS, or FAILURE for an unsupported channel count
 */
int audio_downmix_init(audio_downmix_t *dm, int channels, audio_channel_order_t order);

/**
 * @brief Downmix planar float samples (nominal range -1..1) to interleaved stereo
 * @param dm Weights from audio_downmix_init()
 * @param planes dm->channels arrays, each holding at least frames samples
 * @param frames Frames to convert
 * @param out Interleaved stereo output, frames * 2 samples
 */
void audio_downmix_float(const audio_downmix_t *dm,
                         const float *const *planes,
                         size_t frames,
                         int16_t *out);

/**
 * @brief Downmix planar integer samples of a given bit depth to interleaved stereo
 * @param dm Weights from audio_downmix_init()
 * @param planes dm->channels arrays, each holding at least frames samples
 * @param bits_per_sample Bit depth of the input (1-32)
 * @param frames Frames to convert
 * @param out Interleaved stereo output, frames * 2 samples
 */
void audio_downmix_int32(const audio_downmix_t *dm,
                         const int32_t *const *planes,
                         unsigned bits_per_sample,
                         size_t frames,
                         int16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_DOWNMIX_H */
