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

#ifndef AUDIO_PLAYBACK_H
#define AUDIO_PLAYBACK_H

#include <stddef.h>
#include <stdint.h>

/* Default playback device - I2S amp on Pi Zero 2 */
#define AUDIO_DEFAULT_PLAYBACK_DEVICE "plughw:0,0"

/* Playback configuration */
#define AUDIO_PLAYBACK_RATE 48000 /* Native I2S rate for most DACs */
#define AUDIO_PLAYBACK_CHANNELS 2 /* Stereo output for I2S DAC */

/**
 * Audio playback context
 */
typedef struct {
   void *handle;             /* ALSA PCM handle */
   char device[64];          /* Device name */
   unsigned int sample_rate; /* Actual sample rate */
   unsigned int channels;    /* Number of channels */
   size_t period_size;       /* ALSA period size in frames */
   int initialized;          /* Initialization state */
   volatile float amplitude; /* Current RMS amplitude 0.0-1.0 (updated per chunk) */
} audio_playback_t;

/**
 * Initialize audio playback
 *
 * @param ctx Pointer to playback context
 * @param device ALSA device name (NULL for default)
 * @return 0 on success, -1 on error
 */
int audio_playback_init(audio_playback_t *ctx, const char *device);

/**
 * Clean up audio playback
 *
 * @param ctx Pointer to playback context
 */
void audio_playback_cleanup(audio_playback_t *ctx);

/**
 * Play PCM audio samples
 *
 * Plays 16-bit mono PCM audio, converting to stereo and resampling
 * if necessary to match the output device rate.
 *
 * @param ctx Pointer to playback context
 * @param samples PCM sample buffer (16-bit signed mono)
 * @param num_samples Number of samples
 * @param sample_rate Sample rate of input audio
 * @param stop_flag Optional flag to stop playback when set
 * @return 0 on success, -1 on error
 */
int audio_playback_play(audio_playback_t *ctx,
                        const int16_t *samples,
                        size_t num_samples,
                        unsigned int sample_rate,
                        volatile int *stop_flag);

/**
 * Play WAV data directly
 *
 * Parses WAV header and plays the audio content.
 *
 * @param ctx Pointer to playback context
 * @param wav_data WAV file data
 * @param wav_size WAV file size
 * @param stop_flag Optional flag to stop playback when set
 * @return 0 on success, -1 on error
 */
int audio_playback_play_wav(audio_playback_t *ctx,
                            const uint8_t *wav_data,
                            size_t wav_size,
                            volatile int *stop_flag);

/**
 * Stop any ongoing playback
 *
 * @param ctx Pointer to playback context
 */
void audio_playback_stop(audio_playback_t *ctx);

#endif /* AUDIO_PLAYBACK_H */
