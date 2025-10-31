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

#ifndef DAWN_WAV_UTILS_H
#define DAWN_WAV_UTILS_H

#include <stdint.h>
#include <stddef.h>

// ESP32 Audio Buffer Limits
// These constants define the maximum audio buffer sizes for ESP32 clients
// based on their hardware constraints (30 seconds of 16kHz/16-bit audio)
#define ESP32_SAMPLE_RATE 16000
#define ESP32_BITS_PER_SAMPLE 16  
#define ESP32_MAX_RECORD_TIME 30
#define ESP32_BUFFER_SAMPLES (ESP32_SAMPLE_RATE * ESP32_MAX_RECORD_TIME)
#define ESP32_MAX_RESPONSE_BYTES (ESP32_BUFFER_SAMPLES * sizeof(int16_t) + 1024)
#define SAFE_RESPONSE_LIMIT (ESP32_MAX_RESPONSE_BYTES - 1024)

// Standard Error Messages for TTS Feedback
// These messages provide consistent user feedback for common error conditions
#define ERROR_MSG_LLM_TIMEOUT     "Sorry, the language model timed out. Please try again."
#define ERROR_MSG_TTS_FAILED      "Sorry, voice synthesis failed. Please try again."  
#define ERROR_MSG_SPEECH_FAILED   "Sorry, I could not understand your speech. Please try again."
#define ERROR_MSG_WAV_INVALID     "Sorry, invalid audio format received. Please try again."

// WAV File Header Structure (44 bytes)
// Represents a standard WAV file header with PCM audio format
// Packed to ensure correct memory layout for file I/O
typedef struct __attribute__((packed)) {
   char     riff_header[4];      // "RIFF"
   uint32_t wav_size;            // File size - 8 bytes
   char     wave_header[4];      // "WAVE"
   char     fmt_header[4];       // "fmt "
   uint32_t fmt_chunk_size;      // Format chunk size (16 for PCM)
   uint16_t audio_format;        // Audio format (1 = PCM)
   uint16_t num_channels;        // Number of channels
   uint32_t sample_rate;         // Sample rate in Hz
   uint32_t byte_rate;           // Bytes per second
   uint16_t block_align;         // Bytes per sample frame
   uint16_t bits_per_sample;     // Bits per sample
   char     data_header[4];      // "data"
   uint32_t data_bytes;          // Size of audio data
} WAVHeader;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Check if a WAV file fits within ESP32 buffer limits
 * 
 * @param wav_size Size of the WAV file in bytes
 * @return 1 if the WAV fits within limits, 0 if it exceeds limits
 */
int check_response_size_limit(size_t wav_size);

/**
 * Truncate a WAV file to fit within ESP32 buffer limits
 * 
 * This function creates a new, smaller WAV file by truncating the audio data
 * while preserving proper WAV format and sample alignment. The original WAV
 * data is not modified.
 * 
 * @param wav_data Pointer to the original WAV data
 * @param wav_size Size of the original WAV data in bytes
 * @param truncated_data_out Pointer to receive allocated truncated WAV data
 *                           Set to NULL if no truncation is needed
 *                           Caller must free this buffer if non-NULL
 * @param truncated_size_out Pointer to receive truncated WAV size
 *                           Set to 0 if no truncation is needed
 * 
 * @return 0 on success (check truncated_data_out: NULL = no truncation needed,
 *                      non-NULL = new buffer allocated that caller must free)
 *         -1 on error (invalid parameters, allocation failure, etc.)
 */
int truncate_wav_response(const uint8_t *wav_data, size_t wav_size,
                         uint8_t **truncated_data_out, size_t *truncated_size_out);

#ifdef __cplusplus
}
#endif

#endif // DAWN_WAV_UTILS_H
