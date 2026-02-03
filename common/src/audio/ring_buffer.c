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

#include "audio/ring_buffer.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "logging_common.h"

ring_buffer_t *ring_buffer_create(size_t capacity) {
   ring_buffer_t *rb = (ring_buffer_t *)calloc(1, sizeof(ring_buffer_t));
   if (!rb) {
      DAWN_LOG_ERROR("Failed to allocate ring buffer structure");
      return NULL;
   }

   rb->buffer = (char *)malloc(capacity);
   if (!rb->buffer) {
      DAWN_LOG_ERROR("Failed to allocate ring buffer storage of %zu bytes", capacity);
      free(rb);
      return NULL;
   }

   rb->capacity = capacity;
   rb->head = 0;
   rb->tail = 0;
   rb->count = 0;

   if (pthread_mutex_init(&rb->mutex, NULL) != 0) {
      DAWN_LOG_ERROR("Failed to initialize ring buffer mutex");
      free(rb->buffer);
      free(rb);
      return NULL;
   }

   if (pthread_cond_init(&rb->cond, NULL) != 0) {
      DAWN_LOG_ERROR("Failed to initialize ring buffer condition variable");
      pthread_mutex_destroy(&rb->mutex);
      free(rb->buffer);
      free(rb);
      return NULL;
   }

   DAWN_LOG_INFO("Ring buffer created: capacity=%zu bytes (%.1f seconds at 16kHz mono)", capacity,
                 capacity / (16000.0 * 2.0));

   return rb;
}

void ring_buffer_free(ring_buffer_t *rb) {
   if (!rb) {
      return;
   }

   pthread_mutex_destroy(&rb->mutex);
   pthread_cond_destroy(&rb->cond);
   free(rb->buffer);
   free(rb);
}

size_t ring_buffer_write(ring_buffer_t *rb, const char *data, size_t len) {
   if (!rb || !data || len == 0) {
      return 0;
   }

   pthread_mutex_lock(&rb->mutex);

   size_t bytes_written = 0;

   /* If buffer would overflow, drop oldest data (move tail forward) */
   /* This is expected behavior when main loop is not reading (e.g., during SILENCE state) */
   if (rb->count + len > rb->capacity) {
      size_t overflow = (rb->count + len) - rb->capacity;
      rb->tail = (rb->tail + overflow) % rb->capacity;
      rb->count -= overflow;
      /* Overflow is normal operation - no logging needed */
   }

   /* Write data in one or two chunks (may wrap around) */
   size_t write_len = len;
   while (write_len > 0) {
      size_t chunk_len = write_len;
      if (rb->head + chunk_len > rb->capacity) {
         chunk_len = rb->capacity - rb->head;
      }

      memcpy(rb->buffer + rb->head, data + bytes_written, chunk_len);
      rb->head = (rb->head + chunk_len) % rb->capacity;
      bytes_written += chunk_len;
      write_len -= chunk_len;
   }

   rb->count += bytes_written;

   /* Signal any waiting readers */
   pthread_cond_signal(&rb->cond);
   pthread_mutex_unlock(&rb->mutex);

   return bytes_written;
}

size_t ring_buffer_read(ring_buffer_t *rb, char *data, size_t len) {
   if (!rb || !data || len == 0) {
      return 0;
   }

   pthread_mutex_lock(&rb->mutex);

   /* Read up to len bytes, or what's available */
   size_t read_len = (len < rb->count) ? len : rb->count;
   size_t bytes_read = 0;

   while (bytes_read < read_len) {
      size_t chunk_len = read_len - bytes_read;
      if (rb->tail + chunk_len > rb->capacity) {
         chunk_len = rb->capacity - rb->tail;
      }

      memcpy(data + bytes_read, rb->buffer + rb->tail, chunk_len);
      rb->tail = (rb->tail + chunk_len) % rb->capacity;
      bytes_read += chunk_len;
   }

   rb->count -= bytes_read;

   pthread_mutex_unlock(&rb->mutex);

   return bytes_read;
}

size_t ring_buffer_wait_for_data(ring_buffer_t *rb, size_t min_bytes, int timeout_ms) {
   if (!rb) {
      return 0;
   }

   pthread_mutex_lock(&rb->mutex);

   if (timeout_ms > 0) {
      struct timespec ts;
      struct timeval tv;
      gettimeofday(&tv, NULL);
      ts.tv_sec = tv.tv_sec + (timeout_ms / 1000);
      ts.tv_nsec = (tv.tv_usec * 1000) + ((timeout_ms % 1000) * 1000000);
      if (ts.tv_nsec >= 1000000000) {
         ts.tv_sec++;
         ts.tv_nsec -= 1000000000;
      }

      while (rb->count < min_bytes) {
         int rc = pthread_cond_timedwait(&rb->cond, &rb->mutex, &ts);
         if (rc != 0) {
            /* Timeout or error */
            break;
         }
      }
   } else {
      /* Wait forever */
      while (rb->count < min_bytes) {
         pthread_cond_wait(&rb->cond, &rb->mutex);
      }
   }

   size_t available = rb->count;
   pthread_mutex_unlock(&rb->mutex);

   return available;
}

size_t ring_buffer_bytes_available(ring_buffer_t *rb) {
   if (!rb) {
      return 0;
   }

   pthread_mutex_lock(&rb->mutex);
   size_t available = rb->count;
   pthread_mutex_unlock(&rb->mutex);

   return available;
}

size_t ring_buffer_bytes_free(ring_buffer_t *rb) {
   if (!rb) {
      return 0;
   }

   pthread_mutex_lock(&rb->mutex);
   size_t free_space = rb->capacity - rb->count;
   pthread_mutex_unlock(&rb->mutex);

   return free_space;
}

void ring_buffer_clear(ring_buffer_t *rb) {
   if (!rb) {
      return;
   }

   pthread_mutex_lock(&rb->mutex);
   rb->head = 0;
   rb->tail = 0;
   rb->count = 0;
   pthread_mutex_unlock(&rb->mutex);

   DAWN_LOG_INFO("Ring buffer cleared");
}
