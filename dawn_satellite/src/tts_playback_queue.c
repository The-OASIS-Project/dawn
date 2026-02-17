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
 *
 * TTS Playback Queue - Producer/consumer queue for pipelined TTS playback
 */

#include "tts_playback_queue.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include "logging.h"

/* Queue capacity — 8 sentences of buffered audio (~2-4MB peak on Pi 4) */
#define QUEUE_CAPACITY 8

/* Pause between sentences for natural speech rhythm */
#define SENTENCE_PAUSE_US 150000 /* 150ms */

typedef struct {
   int16_t *audio;
   size_t len;
   int sample_rate;
} tts_entry_t;

struct tts_playback_queue {
   tts_entry_t entries[QUEUE_CAPACITY];
   int head;
   int tail;
   int count;

   pthread_mutex_t mutex;
   pthread_cond_t not_empty; /* Consumer waits when queue empty */
   pthread_cond_t not_full;  /* Producer waits when queue full */

   bool done;    /* No more entries will be added */
   bool playing; /* Currently in audio_playback_play() — guarded by mutex */

   audio_playback_t *playback;
   atomic_int *stop_flag;

   pthread_t thread;
   atomic_bool thread_running;
};

/* =============================================================================
 * Playback Consumer Thread
 * ============================================================================= */

static void *playback_thread_func(void *arg) {
   tts_playback_queue_t *q = (tts_playback_queue_t *)arg;

   while (1) {
      tts_entry_t entry;

      /* Dequeue next entry and mark playing (single lock region) */
      pthread_mutex_lock(&q->mutex);
      while (q->count == 0 && !q->done) {
         pthread_cond_wait(&q->not_empty, &q->mutex);
      }

      if (q->count == 0 && q->done) {
         pthread_mutex_unlock(&q->mutex);
         break;
      }

      entry = q->entries[q->head];
      q->head = (q->head + 1) % QUEUE_CAPACITY;
      q->count--;
      q->playing = true;
      pthread_cond_signal(&q->not_full);
      pthread_mutex_unlock(&q->mutex);

      /* Check stop flag before playing */
      if (atomic_load(q->stop_flag)) {
         free(entry.audio);
         pthread_mutex_lock(&q->mutex);
         q->playing = false;
         pthread_mutex_unlock(&q->mutex);
         continue;
      }

      /* Play this sentence without draining — keeps ALSA in RUNNING state so the
       * next sentence streams seamlessly without a DAC restart transient. */
      audio_playback_play(q->playback, entry.audio, entry.len, entry.sample_rate, q->stop_flag,
                          false);
      free(entry.audio);

      pthread_mutex_lock(&q->mutex);
      q->playing = false;
      pthread_mutex_unlock(&q->mutex);

      /* Brief pause between sentences for natural rhythm */
      if (!atomic_load(q->stop_flag)) {
         usleep(SENTENCE_PAUSE_US);
      }
   }

   /* Drain after final sentence so remaining audio in the hardware buffer
    * plays out fully before returning (or drop immediately if stopped). */
   audio_playback_drain(q->playback, q->stop_flag);

   return NULL;
}

/* =============================================================================
 * Internal Helpers
 * ============================================================================= */

/** @brief Start the consumer thread. Caller must ensure no thread is running. */
static int start_thread(tts_playback_queue_t *q) {
   if (pthread_create(&q->thread, NULL, playback_thread_func, q) == 0) {
      atomic_store(&q->thread_running, true);
      return 0;
   }
   LOG_ERROR("TTS queue: failed to create playback thread");
   return 1;
}

/** @brief Stop and join the consumer thread. Sets stop_flag to interrupt playback. */
static void stop_and_join(tts_playback_queue_t *q) {
   if (!atomic_load(&q->thread_running))
      return;

   /* Interrupt current playback so join doesn't block for full sentence */
   atomic_store(q->stop_flag, 1);

   /* Flush queued entries and signal done */
   pthread_mutex_lock(&q->mutex);
   while (q->count > 0) {
      free(q->entries[q->head].audio);
      q->entries[q->head].audio = NULL;
      q->head = (q->head + 1) % QUEUE_CAPACITY;
      q->count--;
   }
   q->done = true;
   pthread_cond_signal(&q->not_empty);
   pthread_cond_signal(&q->not_full);
   pthread_mutex_unlock(&q->mutex);

   pthread_join(q->thread, NULL);
   atomic_store(&q->thread_running, false);

   /* Clear stop flag for next interaction */
   atomic_store(q->stop_flag, 0);
}

/* =============================================================================
 * Public API
 * ============================================================================= */

tts_playback_queue_t *tts_playback_queue_create(audio_playback_t *playback, atomic_int *stop_flag) {
   if (!playback || !stop_flag)
      return NULL;

   tts_playback_queue_t *q = calloc(1, sizeof(tts_playback_queue_t));
   if (!q)
      return NULL;

   pthread_mutex_init(&q->mutex, NULL);
   pthread_cond_init(&q->not_empty, NULL);
   pthread_cond_init(&q->not_full, NULL);

   q->playback = playback;
   q->stop_flag = stop_flag;

   /* Start consumer thread immediately (idles on condvar until first push) */
   if (start_thread(q) != 0) {
      pthread_mutex_destroy(&q->mutex);
      pthread_cond_destroy(&q->not_empty);
      pthread_cond_destroy(&q->not_full);
      free(q);
      return NULL;
   }

   return q;
}

void tts_playback_queue_destroy(tts_playback_queue_t *q) {
   if (!q)
      return;

   stop_and_join(q);

   pthread_mutex_destroy(&q->mutex);
   pthread_cond_destroy(&q->not_empty);
   pthread_cond_destroy(&q->not_full);
   free(q);
}

int tts_playback_queue_push(tts_playback_queue_t *q, int16_t *audio, size_t len, int sample_rate) {
   if (!q || !audio)
      return 1;

   pthread_mutex_lock(&q->mutex);

   /* Back-pressure: block if queue is full */
   while (q->count == QUEUE_CAPACITY && !q->done && !atomic_load(q->stop_flag)) {
      pthread_cond_wait(&q->not_full, &q->mutex);
   }

   if (q->done || atomic_load(q->stop_flag)) {
      pthread_mutex_unlock(&q->mutex);
      free(audio);
      return 1;
   }

   q->entries[q->tail].audio = audio;
   q->entries[q->tail].len = len;
   q->entries[q->tail].sample_rate = sample_rate;
   q->tail = (q->tail + 1) % QUEUE_CAPACITY;
   q->count++;

   pthread_cond_signal(&q->not_empty);
   pthread_mutex_unlock(&q->mutex);
   return 0;
}

void tts_playback_queue_finish(tts_playback_queue_t *q) {
   if (!q)
      return;

   pthread_mutex_lock(&q->mutex);
   q->done = true;
   pthread_cond_signal(&q->not_empty);
   pthread_mutex_unlock(&q->mutex);
}

void tts_playback_queue_flush(tts_playback_queue_t *q) {
   if (!q)
      return;

   pthread_mutex_lock(&q->mutex);

   /* Free all queued audio buffers */
   while (q->count > 0) {
      free(q->entries[q->head].audio);
      q->entries[q->head].audio = NULL;
      q->head = (q->head + 1) % QUEUE_CAPACITY;
      q->count--;
   }

   q->done = true;
   pthread_cond_signal(&q->not_empty); /* Wake consumer so it can exit */
   pthread_cond_signal(&q->not_full);  /* Wake producer if blocked */
   pthread_mutex_unlock(&q->mutex);
}

bool tts_playback_queue_is_active(tts_playback_queue_t *q) {
   if (!q)
      return false;

   pthread_mutex_lock(&q->mutex);
   bool active = q->playing || q->count > 0;
   pthread_mutex_unlock(&q->mutex);

   return active;
}

void tts_playback_queue_reset(tts_playback_queue_t *q) {
   if (!q)
      return;

   /* Stop previous playback thread (interrupts current playback for fast join) */
   stop_and_join(q);

   /* Reset queue state for new interaction */
   pthread_mutex_lock(&q->mutex);
   q->head = 0;
   q->tail = 0;
   q->count = 0;
   q->done = false;
   q->playing = false;
   pthread_mutex_unlock(&q->mutex);

   /* Start fresh playback thread */
   start_thread(q);
}
