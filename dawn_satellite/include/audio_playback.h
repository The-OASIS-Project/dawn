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

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "spectrum_defs.h" /* For SPECTRUM_BINS */

/* Default playback device - I2S amp on Pi Zero 2 */
#define AUDIO_DEFAULT_PLAYBACK_DEVICE "plughw:0,0"

/* Playback configuration */
#define AUDIO_PLAYBACK_RATE 48000 /* Native I2S rate for most DACs */
#define AUDIO_PLAYBACK_CHANNELS 2 /* Stereo output for I2S DAC */

/**
 * Audio playback context
 */
typedef struct {
   void *handle;                           /* ALSA PCM handle */
   char device[64];                        /* Device name */
   unsigned int sample_rate;               /* Actual sample rate */
   unsigned int channels;                  /* Number of channels */
   size_t period_size;                     /* ALSA period size in frames */
   int initialized;                        /* Initialization state */
   volatile float amplitude;               /* Current RMS amplitude 0.0-1.0 (updated per chunk) */
   volatile float spectrum[SPECTRUM_BINS]; /* FFT magnitude bins 0.0-1.0 (updated per chunk) */
   pthread_mutex_t alsa_mutex;             /* Guards all snd_pcm_* calls */
   atomic_int volume;                      /* Master volume 0-100, default 80 */
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
 * @param drain If true, drain/drop ALSA buffer after writing (blocks until
 *              hardware finishes). Pass false when streaming consecutive
 *              sentences to avoid DAC restart transients between them.
 * @return 0 on success, -1 on error
 */
int audio_playback_play(audio_playback_t *ctx,
                        const int16_t *samples,
                        size_t num_samples,
                        unsigned int sample_rate,
                        atomic_int *stop_flag,
                        bool drain);

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
                            atomic_int *stop_flag);

/**
 * Play stereo PCM audio at native rate (48kHz)
 *
 * Takes interleaved stereo int16 at 48kHz — no resampling, no mono conversion.
 * Does NOT call snd_pcm_prepare() — caller must prepare once; only re-prepares
 * on underrun recovery. Acquires alsa_mutex for each period write.
 * Updates spectrum and amplitude for visualizer.
 *
 * @param ctx Pointer to playback context
 * @param stereo_samples Interleaved stereo int16 samples at 48kHz
 * @param num_frames Number of frames (each frame = 2 samples L+R)
 * @param stop_flag Optional flag to stop playback when set
 * @return Number of frames written, or -1 on error
 */
int audio_playback_play_stereo(audio_playback_t *ctx,
                               const int16_t *stereo_samples,
                               size_t num_frames,
                               atomic_int *stop_flag);

/**
 * Drain ALSA playback buffer (blocks until hardware finishes playing).
 *
 * Call after the last sentence when using audio_playback_play() with drain=false,
 * to ensure all queued audio reaches the speakers before moving on.
 *
 * @param ctx Pointer to playback context
 * @param stop_flag Optional flag — if set, drops audio immediately instead of draining
 */
void audio_playback_drain(audio_playback_t *ctx, atomic_int *stop_flag);

/**
 * Re-prepare ALSA device (e.g. after pause/TTS interruption).
 *
 * @param ctx Pointer to playback context
 */
void audio_playback_prepare(audio_playback_t *ctx);

/**
 * Stop any ongoing playback
 *
 * @param ctx Pointer to playback context
 */
void audio_playback_stop(audio_playback_t *ctx);

/**
 * Set master volume (0-100). Applies to TTS playback path.
 * Default is 80.
 */
void audio_playback_set_volume(audio_playback_t *ctx, int volume);

/**
 * Get current master volume (0-100).
 */
int audio_playback_get_volume(audio_playback_t *ctx);

/**
 * Get ALSA output delay in frames (buffered in hardware awaiting playback).
 * Returns 0 if device is not active or query fails.
 */
long audio_playback_get_delay_frames(audio_playback_t *ctx);

/**
 * Get writable space in ALSA output buffer (frames that can be written without blocking).
 * Recovers from underrun (EPIPE) automatically. Returns 0 on error or inactive device.
 */
long audio_playback_get_avail_frames(audio_playback_t *ctx);

#endif /* AUDIO_PLAYBACK_H */
