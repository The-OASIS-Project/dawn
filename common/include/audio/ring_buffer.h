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

#ifndef DAWN_COMMON_RING_BUFFER_H
#define DAWN_COMMON_RING_BUFFER_H

#include <pthread.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Thread-safe ring buffer for audio data
 *
 * A circular buffer designed for producer-consumer pattern where
 * the audio capture thread writes data and the main thread reads it.
 * Implements overflow policy: oldest data is dropped when buffer fills.
 */
typedef struct {
   char *buffer;          /**< Buffer storage */
   size_t capacity;       /**< Total buffer size in bytes */
   size_t head;           /**< Write position (producer) */
   size_t tail;           /**< Read position (consumer) */
   size_t count;          /**< Current bytes in buffer */
   pthread_mutex_t mutex; /**< Mutex for thread safety */
   pthread_cond_t cond;   /**< Condition variable to signal data available */
} ring_buffer_t;

/**
 * @brief Create a new ring buffer
 *
 * @param capacity Size of the buffer in bytes (recommend 32KB-64KB for 1-2 seconds of audio)
 * @return Pointer to newly allocated ring buffer, or NULL on error
 */
ring_buffer_t *ring_buffer_create(size_t capacity);

/**
 * @brief Free a ring buffer and all associated resources
 *
 * @param rb Ring buffer to free
 */
void ring_buffer_free(ring_buffer_t *rb);

/**
 * @brief Write data to ring buffer (producer/audio capture thread)
 *
 * Writes up to len bytes to the buffer. If buffer is full, oldest data
 * is overwritten (overflow policy: drop old data to keep recording).
 *
 * Thread-safe: can be called concurrently with ring_buffer_read().
 *
 * @param rb Ring buffer
 * @param data Data to write
 * @param len Number of bytes to write
 * @return Number of bytes actually written
 */
size_t ring_buffer_write(ring_buffer_t *rb, const char *data, size_t len);

/**
 * @brief Read data from ring buffer (consumer/main thread)
 *
 * Reads up to len bytes from the buffer. If less data is available,
 * reads what's available. Non-blocking.
 *
 * Thread-safe: can be called concurrently with ring_buffer_write().
 *
 * @param rb Ring buffer
 * @param data Destination buffer
 * @param len Maximum bytes to read
 * @return Number of bytes actually read (0 if buffer empty)
 */
size_t ring_buffer_read(ring_buffer_t *rb, char *data, size_t len);

/**
 * @brief Wait for data to become available in ring buffer
 *
 * Blocks until at least min_bytes are available or timeout occurs.
 * Useful for consumer to wait for producer to fill buffer.
 *
 * @param rb Ring buffer
 * @param min_bytes Minimum bytes to wait for
 * @param timeout_ms Timeout in milliseconds (0 = wait forever)
 * @return Number of bytes available, or 0 on timeout
 */
size_t ring_buffer_wait_for_data(ring_buffer_t *rb, size_t min_bytes, int timeout_ms);

/**
 * @brief Get number of bytes currently in buffer
 *
 * Thread-safe snapshot of current buffer occupancy.
 *
 * @param rb Ring buffer
 * @return Number of bytes available to read
 */
size_t ring_buffer_bytes_available(ring_buffer_t *rb);

/**
 * @brief Get amount of free space in buffer
 *
 * Thread-safe snapshot of remaining capacity.
 *
 * @param rb Ring buffer
 * @return Number of bytes that can be written without overflow
 */
size_t ring_buffer_bytes_free(ring_buffer_t *rb);

/**
 * @brief Clear all data from buffer
 *
 * Resets head/tail pointers and count. Useful for recovering from errors.
 *
 * @param rb Ring buffer
 */
void ring_buffer_clear(ring_buffer_t *rb);

#ifdef __cplusplus
}
#endif

#endif /* DAWN_COMMON_RING_BUFFER_H */
