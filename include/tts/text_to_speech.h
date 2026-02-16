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

#ifndef TEXT_TO_SPEECH_H
#define TEXT_TO_SPEECH_H

#ifdef __cplusplus
#include <atomic>
extern "C" {
#endif

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

// Enumeration for playback state
typedef enum {
   TTS_PLAYBACK_IDLE = 0, /**< Idle playback state */
   TTS_PLAYBACK_PLAY,     /**< Playing state */
   TTS_PLAYBACK_PAUSE,    /**< Pause playback */
   TTS_PLAYBACK_DISCARD   /**< Discard current playback */
} tts_playback_state_t;

// Declare the shared variables as extern
extern pthread_cond_t tts_cond;
extern pthread_mutex_t tts_mutex;

// TTS playback state (mutex-protected, no atomics to avoid C/C++ boundary issues)
extern int tts_playback_state;

/**
 * @brief Initializes the text-to-speech system.
 *
 * This function loads the voice model, initializes the TTS engine, sets up the
 * audio device, and starts the worker thread that processes TTS requests.
 *
 * @param pcm_device The name of the PCM device to use for audio playback.
 */
void initialize_text_to_speech(char *pcm_device);

/**
 * @brief Enqueues a text string for conversion to speech.
 *
 * This function can be safely called from multiple threads. It adds the provided
 * text to a queue that is processed by a dedicated worker thread.
 *
 * @param text The text to be converted to speech.
 */
void text_to_speech(char *text);

/**
 * @brief Generate raw PCM audio data from text
 *
 * This function generates raw PCM audio samples (16-bit signed, mono, 22050Hz)
 * using Piper TTS. Unlike text_to_speech_to_wav(), this returns raw samples
 * without a WAV header, making it efficient for encoding to Opus/ADPCM.
 *
 * @param text The text to be converted to audio
 * @param pcm_data_out Pointer to receive allocated PCM data (caller must free)
 * @param pcm_samples_out Pointer to receive number of samples (not bytes)
 * @param sample_rate_out Pointer to receive sample rate (always 22050)
 * @return 0 on success, -1 on error
 *
 * @note Output is 16-bit signed little-endian mono PCM at 22050Hz
 * @note Thread-safe via tts_mutex
 */
int text_to_speech_to_pcm(const char *text,
                          int16_t **pcm_data_out,
                          size_t *pcm_samples_out,
                          uint32_t *sample_rate_out);

/**
 * @brief Generate WAV audio data from text for network transmission
 *
 * This function generates WAV audio by calling text_to_speech_to_pcm() and
 * wrapping the result with a standard WAV header. Use this for clients that
 * expect WAV format (e.g., DAP1 ESP32 satellites).
 *
 * @param text The text to be converted to WAV audio
 * @param wav_data_out Pointer to receive allocated WAV data (caller must free)
 * @param wav_size_out Pointer to receive WAV data size in bytes
 * @return 0 on success, -1 on error
 *
 * @note Thread-safe via tts_mutex
 */
int text_to_speech_to_wav(const char *text, uint8_t **wav_data_out, size_t *wav_size_out);

/**
 * @brief Speaks the greeting with AEC delay calibration
 *
 * This function plays the greeting TTS and uses it to calibrate the
 * acoustic delay for echo cancellation. The measured delay is used
 * to update the AEC delay hint for optimal performance.
 *
 * Should be called for the boot greeting to measure actual acoustic delay.
 * If AEC is disabled or calibration fails, falls back to normal TTS.
 *
 * @param greeting The greeting text to speak
 */
void tts_speak_greeting_with_calibration(const char *greeting);

/**
 * @brief Wait for TTS queue to empty and playback to complete
 *
 * Blocks until all queued TTS has finished playing. Used during boot
 * to ensure the greeting completes before enabling barge-in.
 *
 * @param timeout_ms Maximum time to wait in milliseconds (0 = no timeout)
 * @return 0 if TTS completed, -1 if timeout or not initialized
 */
int tts_wait_for_completion(int timeout_ms);

/**
 * @brief Cleans up the text-to-speech system.
 *
 * This function signals the worker thread to terminate, waits for it to finish,
 * and then releases all resources used by the TTS engine.
 */
void cleanup_text_to_speech();

#ifdef __cplusplus
}
#endif

#endif  // TEXT_TO_SPEECH_H
