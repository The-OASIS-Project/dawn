/*
 * DAWN Satellite - ALSA Audio Capture with Thread
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
 *
 * Uses a dedicated capture thread that reads from ALSA and writes to a
 * ring buffer. The main thread reads from the ring buffer (non-blocking).
 */

#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Audio configuration matching DAWN server expectations */
#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_CHANNELS 1
#define AUDIO_BITS_PER_SAMPLE 16
#define AUDIO_BYTES_PER_SAMPLE 2

/* Default capture device - I2S mic on Pi Zero 2 */
#define AUDIO_DEFAULT_CAPTURE_DEVICE "plughw:0,0"

/* Maximum recording time in seconds */
#define AUDIO_MAX_RECORD_TIME 30

/* WAV header structure */
typedef struct __attribute__((packed)) {
   char riff[4];             /* "RIFF" */
   uint32_t chunk_size;      /* File size - 8 */
   char wave[4];             /* "WAVE" */
   char fmt[4];              /* "fmt " */
   uint32_t subchunk1_size;  /* 16 for PCM */
   uint16_t audio_format;    /* 1 for PCM */
   uint16_t num_channels;    /* 1 for mono */
   uint32_t sample_rate;     /* 16000 Hz */
   uint32_t byte_rate;       /* sample_rate * channels * bits/8 */
   uint16_t block_align;     /* channels * bits/8 */
   uint16_t bits_per_sample; /* 16 bits */
   char data[4];             /* "data" */
   uint32_t subchunk2_size;  /* Audio data size */
} wav_header_t;

/**
 * Audio capture context (opaque)
 */
typedef struct audio_capture audio_capture_t;

/**
 * Initialize audio capture with dedicated capture thread
 *
 * Creates a capture thread that continuously reads from ALSA and
 * writes to an internal ring buffer. Use audio_capture_read() to
 * read from the ring buffer (non-blocking).
 *
 * @param ctx_out Pointer to receive allocated context
 * @param device ALSA device name (NULL for default)
 * @return 0 on success, -1 on error
 */
int audio_capture_init(audio_capture_t **ctx_out, const char *device);

/**
 * Clean up audio capture
 *
 * Stops capture thread and frees all resources.
 *
 * @param ctx Capture context
 */
void audio_capture_cleanup(audio_capture_t *ctx);

/**
 * Read audio from ring buffer (non-blocking)
 *
 * Reads up to max_samples from the internal ring buffer.
 * Returns immediately with available data (may be 0 if buffer empty).
 *
 * @param ctx Capture context
 * @param buffer Output buffer for PCM samples
 * @param max_samples Maximum number of samples to read
 * @return Number of samples read, 0 if no data available, -1 on error
 */
ssize_t audio_capture_read(audio_capture_t *ctx, int16_t *buffer, size_t max_samples);

/**
 * Wait for audio data to become available
 *
 * Blocks until at least min_samples are available or timeout occurs.
 *
 * @param ctx Capture context
 * @param min_samples Minimum samples to wait for
 * @param timeout_ms Timeout in milliseconds (0 = wait forever)
 * @return Number of samples available, or 0 on timeout
 */
size_t audio_capture_wait_for_data(audio_capture_t *ctx, size_t min_samples, int timeout_ms);

/**
 * Get number of samples currently available in ring buffer
 *
 * @param ctx Capture context
 * @return Number of samples available to read
 */
size_t audio_capture_bytes_available(audio_capture_t *ctx);

/**
 * Clear all data from ring buffer
 *
 * Useful after playing TTS to discard any captured audio from playback.
 *
 * @param ctx Capture context
 */
void audio_capture_clear(audio_capture_t *ctx);

/**
 * Create WAV file from PCM samples
 *
 * @param samples PCM sample buffer
 * @param num_samples Number of samples
 * @param wav_data Pointer to receive WAV data (caller must free)
 * @param wav_size Pointer to receive WAV data size
 * @return 0 on success, -1 on error
 */
int audio_create_wav(const int16_t *samples,
                     size_t num_samples,
                     uint8_t **wav_data,
                     size_t *wav_size);

/**
 * Parse WAV header and extract PCM data pointer
 *
 * @param wav_data WAV file data
 * @param wav_size WAV file size
 * @param pcm_data Pointer to receive PCM data start
 * @param pcm_size Pointer to receive PCM data size
 * @param sample_rate Pointer to receive sample rate (can be NULL)
 * @param channels Pointer to receive channel count (can be NULL)
 * @return 0 on success, -1 on invalid WAV
 */
int audio_parse_wav(const uint8_t *wav_data,
                    size_t wav_size,
                    const int16_t **pcm_data,
                    size_t *pcm_size,
                    unsigned int *sample_rate,
                    unsigned int *channels);

#endif /* AUDIO_CAPTURE_H */
