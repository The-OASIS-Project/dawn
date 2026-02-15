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

#ifndef DAWN_COMMON_SENTENCE_BUFFER_H
#define DAWN_COMMON_SENTENCE_BUFFER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback function type for complete sentences
 *
 * Called when a complete sentence is detected and ready for TTS.
 *
 * @param sentence Complete sentence text (null-terminated)
 * @param userdata User-provided context pointer
 */
typedef void (*sentence_callback)(const char *sentence, void *userdata);

/**
 * @brief Sentence buffer (opaque type)
 *
 * Accumulates text chunks and extracts complete sentences based on
 * punctuation boundaries (., !, ?, :) followed by space or end of input.
 *
 * Note: Callers should pre-filter any content (e.g., <command> tags)
 * before feeding to this buffer.
 */
typedef struct sentence_buffer sentence_buffer_t;

/**
 * @brief Create a new sentence buffer
 *
 * @param callback Function to call for each complete sentence
 * @param userdata User context pointer passed to callback
 * @return Newly allocated sentence buffer, or NULL on error
 */
sentence_buffer_t *sentence_buffer_create(sentence_callback callback, void *userdata);

/**
 * @brief Free a sentence buffer
 *
 * Flushes any remaining text before freeing.
 *
 * @param buf Sentence buffer to free
 */
void sentence_buffer_free(sentence_buffer_t *buf);

/**
 * @brief Feed a text chunk to the sentence buffer
 *
 * Accumulates the chunk and extracts any complete sentences.
 * Complete sentences are sent to the callback immediately.
 *
 * @param buf Sentence buffer
 * @param chunk Text chunk to add
 */
void sentence_buffer_feed(sentence_buffer_t *buf, const char *chunk);

/**
 * @brief Flush remaining buffered text
 *
 * Sends any remaining incomplete sentence to the callback.
 * Call this when the stream is complete to handle the final sentence.
 *
 * @param buf Sentence buffer
 */
void sentence_buffer_flush(sentence_buffer_t *buf);

/**
 * @brief Clear the buffer without invoking callback
 *
 * Discards any buffered text. Use when starting a new stream.
 *
 * @param buf Sentence buffer
 */
void sentence_buffer_clear(sentence_buffer_t *buf);

#ifdef __cplusplus
}
#endif

#endif /* DAWN_COMMON_SENTENCE_BUFFER_H */
