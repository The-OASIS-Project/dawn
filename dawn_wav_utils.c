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

#include "dawn_wav_utils.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <endian.h>

/**
 * Check if a WAV file fits within ESP32 buffer limits
 * 
 * Compares the WAV file size against the safe response limit for ESP32 clients.
 * Logs appropriate warnings if the size exceeds the limit.
 * 
 * @param wav_size Size of the WAV file in bytes
 * @return 1 if within limits, 0 if exceeds limits
 */
int check_response_size_limit(size_t wav_size) {
   LOG_INFO("Response size: %zu bytes (limit: %ld bytes)", wav_size, SAFE_RESPONSE_LIMIT);

   if (wav_size <= SAFE_RESPONSE_LIMIT) {
      LOG_INFO("Response fits within ESP32 buffer limits");
      return 1;
   } else {
      LOG_WARNING("Response exceeds ESP32 buffer limits by %zu bytes", 
                  wav_size - SAFE_RESPONSE_LIMIT);
      return 0;
   }
}

/**
 * Truncate a WAV file to fit within ESP32 buffer limits
 * 
 * Creates a new WAV file by truncating the audio data from the original file.
 * The truncation maintains sample alignment (2-byte boundaries for 16-bit audio)
 * and updates the WAV header accordingly. Proper endianness conversion is applied
 * for cross-platform compatibility.
 * 
 * @param wav_data Pointer to original WAV data (must be valid WAV format)
 * @param wav_size Size of original WAV data in bytes (must be >= 44)
 * @param truncated_data_out Receives pointer to new truncated buffer (or NULL if not needed)
 * @param truncated_size_out Receives size of truncated buffer (or 0 if not needed)
 * @return 0 on success, -1 on error
 */
int truncate_wav_response(const uint8_t *wav_data, size_t wav_size,
                         uint8_t **truncated_data_out, size_t *truncated_size_out) {
   
   // Validate input parameters
   if (!wav_data || !truncated_data_out || !truncated_size_out) {
      LOG_ERROR("Invalid NULL parameters for WAV truncation");
      return -1;
   }

   if (wav_size < sizeof(WAVHeader)) {
      LOG_ERROR("WAV data too small (%zu bytes, minimum %zu bytes)", 
                wav_size, sizeof(WAVHeader));
      return -1;
   }

   // Parse the original WAV header
   const WAVHeader *original_header = (const WAVHeader *)wav_data;

   // Calculate sizes
   size_t header_size = sizeof(WAVHeader);
   size_t original_audio_data = wav_size - header_size;
   size_t max_audio_data = SAFE_RESPONSE_LIMIT - header_size;

   // Check if truncation is actually needed
   if (original_audio_data <= max_audio_data) {
      LOG_INFO("No truncation needed - WAV already fits within limits");
      *truncated_data_out = NULL;
      *truncated_size_out = 0;
      return 0;  // Success - no truncation needed
   }

   LOG_INFO("Truncating WAV from %zu to %zu bytes", wav_size, SAFE_RESPONSE_LIMIT);

   // Align to sample boundaries (2 bytes per sample for 16-bit mono audio)
   // This prevents cutting in the middle of a sample which would cause audio glitches
   max_audio_data = (max_audio_data / 2) * 2;
   size_t truncated_total_size = header_size + max_audio_data;

   // Calculate and log duration information
   uint32_t sample_rate = le32toh(original_header->sample_rate);
   if (sample_rate > 0) {
      double original_duration = (double)original_audio_data / (sample_rate * 2);
      double truncated_duration = (double)max_audio_data / (sample_rate * 2);
      LOG_INFO("Duration: %.2f -> %.2f seconds", original_duration, truncated_duration);
   }

   // Allocate buffer for truncated WAV
   uint8_t *truncated_data = (uint8_t *)malloc(truncated_total_size);
   if (!truncated_data) {
      LOG_ERROR("Failed to allocate %zu bytes for truncated WAV", truncated_total_size);
      return -1;
   }

   // Copy and modify the WAV header
   WAVHeader *new_header = (WAVHeader *)truncated_data;
   memcpy(new_header, original_header, header_size);

   // Update header fields with new sizes (applying proper endianness conversion)
   new_header->wav_size = htole32(truncated_total_size - 8);
   new_header->data_bytes = htole32(max_audio_data);

   // Copy the truncated audio data
   memcpy(truncated_data + header_size, wav_data + header_size, max_audio_data);

   // Return the truncated data to caller
   *truncated_data_out = truncated_data;
   *truncated_size_out = truncated_total_size;

   LOG_INFO("WAV truncation complete: %zu bytes allocated", truncated_total_size);
   return 0;  // Success - new buffer allocated
}
