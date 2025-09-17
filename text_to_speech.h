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
extern "C" {
#endif

// Enumeration for playback state
typedef enum {
    TTS_PLAYBACK_IDLE = 0,    /**< Idle playback state */
    TTS_PLAYBACK_PLAY,        /**< Playing state */
    TTS_PLAYBACK_PAUSE,       /**< Pause playback */
    TTS_PLAYBACK_DISCARD      /**< Discard current playback */
} tts_playback_state_t;

// Declare the shared variables as extern
extern pthread_cond_t tts_cond;
extern pthread_mutex_t tts_mutex;
extern volatile sig_atomic_t tts_playback_state;

/**
 * @brief Initializes the text-to-speech system.
 *
 * This function loads the voice model, initializes the TTS engine, sets up the
 * audio device, and starts the worker thread that processes TTS requests.
 *
 * @param pcm_device The name of the PCM device to use for audio playback.
 */
void initialize_text_to_speech(char* pcm_device);

/**
 * @brief Enqueues a text string for conversion to speech.
 *
 * This function can be safely called from multiple threads. It adds the provided
 * text to a queue that is processed by a dedicated worker thread.
 *
 * @param text The text to be converted to speech.
 */
void text_to_speech(char* text);

/**
 * @brief Generate WAV audio data from text for network transmission
 *
 * This function generates WAV audio using the same Piper instance as local TTS,
 * but returns the audio data in memory instead of playing it locally.
 *
 * @param text The text to be converted to WAV audio
 * @param wav_data_out Pointer to receive allocated WAV data (caller must free)
 * @param wav_size_out Pointer to receive WAV data size in bytes
 * @return 0 on success, -1 on error
 */
int text_to_speech_to_wav(const char* text, uint8_t** wav_data_out, size_t* wav_size_out);

uint8_t* error_to_wav(const char* error_message, size_t* tts_size_out);

/**
 * @brief Cleans up the text-to-speech system.
 *
 * This function signals the worker thread to terminate, waits for it to finish,
 * and then releases all resources used by the TTS engine.
 */
void cleanup_text_to_speech();

/**
 * @brief Removes all occurrences of specified characters from a string.
 *
 * This function modifies the input string `str` in place by removing any characters
 * that are present in the `remove_chars` string. The resulting string will be a subset
 * of the original, excluding the unwanted characters.
 *
 * The function operates by iterating over each character in `str` and copying it
 * to the destination position if it is not found in `remove_chars`. It does not
 * allocate additional memory and adjusts the string in place.
 *
 * @param str          The string to be modified. Must be a null-terminated mutable string.
 * @param remove_chars A null-terminated string containing characters to remove from `str`.
 *
 * @note
 * - The input string `str` must be mutable and large enough to hold the modified string.
 * - If `str` is `NULL`, the function has no effect.
 * - If `remove_chars` is `NULL` or empty, `str` remains unchanged.
 * - The function compares characters based on their exact value and does not account for locale-specific variations.
 */
void remove_chars(char *str, const char *remove_chars);

/**
 * @brief Checks if a Unicode code point represents an emoji character.
 *
 * This helper function determines whether a given Unicode code point falls within
 * common emoji ranges. It is used internally by `remove_emojis` to identify emojis.
 *
 * @param codepoint The Unicode code point to check.
 * @return `true` if the code point is an emoji, `false` otherwise.
 *
 * @note
 * - The emoji ranges checked are not exhaustive but cover commonly used emojis.
 * - This function does not account for all possible emojis, including those that
 *   require variation selectors or are represented by sequences of code points.
 */
void remove_emojis(char *str);

#ifdef __cplusplus
}
#endif

#endif // TEXT_TO_SPEECH_H
