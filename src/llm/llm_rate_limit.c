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
 * Process-wide sliding window rate limiter for cloud LLM API calls.
 *
 * Uses a circular buffer of CLOCK_MONOTONIC timestamps to track API calls
 * within a 60-second window. When the window is full, callers sleep in
 * 250ms chunks (checking for interrupts, via llm_sleep_with_interrupt_check)
 * until a slot frees up.
 */

#include "llm/llm_rate_limit.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#include "llm/llm_interface.h"
#include "logging.h"

#define WINDOW_SECONDS 60

/* Chunk size for the interrupt-aware sleep: below the ~400ms wake-word-feels-laggy
 * threshold, and 2.5x fewer scheduler wakeups than 100ms on a multi-second wait. */
#define LLM_INTERRUPT_SLEEP_CHUNK_MS 250

typedef struct {
   pthread_mutex_t mutex;
   struct timespec timestamps[LLM_RATE_LIMIT_MAX_SLOTS];
   int head;  /* Next write position */
   int count; /* Number of valid entries */
   atomic_int max_rpm;
} rate_limiter_t;

static rate_limiter_t s_limiter = {
   .mutex = PTHREAD_MUTEX_INITIALIZER,
   .head = 0,
   .count = 0,
   .max_rpm = 0,
};

/** Get the index of the tail (oldest entry) in the circular buffer */
static int tail_index(int head, int count) {
   int idx = head - count;
   if (idx < 0)
      idx += LLM_RATE_LIMIT_MAX_SLOTS;
   return idx;
}

/** Returns elapsed seconds between two timespecs */
static double timespec_diff_sec(const struct timespec *a, const struct timespec *b) {
   return (double)(a->tv_sec - b->tv_sec) + (double)(a->tv_nsec - b->tv_nsec) / 1e9;
}

/** Discard entries older than 60 seconds. Must be called with mutex held. */
static void expire_old_entries(rate_limiter_t *lim) {
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);

   while (lim->count > 0) {
      int tidx = tail_index(lim->head, lim->count);
      double age = timespec_diff_sec(&now, &lim->timestamps[tidx]);
      if (age < (double)WINDOW_SECONDS)
         break;
      lim->count--;
   }
}

void llm_rate_limit_init(int max_rpm) {
   if (max_rpm > LLM_RATE_LIMIT_MAX_SLOTS) {
      OLOG_WARNING("Rate limit RPM %d exceeds max slots %d, clamping", max_rpm,
                   LLM_RATE_LIMIT_MAX_SLOTS);
      max_rpm = LLM_RATE_LIMIT_MAX_SLOTS;
   }

   pthread_mutex_lock(&s_limiter.mutex);
   atomic_store(&s_limiter.max_rpm, max_rpm);
   s_limiter.head = 0;
   s_limiter.count = 0;
   memset(s_limiter.timestamps, 0, sizeof(s_limiter.timestamps));
   pthread_mutex_unlock(&s_limiter.mutex);

   if (max_rpm > 0) {
      OLOG_INFO("LLM rate limiter initialized: %d RPM", max_rpm);
   } else {
      OLOG_INFO("LLM rate limiter disabled");
   }
}

bool llm_interrupt_ctx_triggered(const llm_interrupt_ctx_t *ctx) {
   if (!ctx) {
      /* Legacy global-only behavior (foreground wake-word / Ctrl+C). */
      return llm_is_interrupt_requested();
   }
   if (ctx->session_flag && atomic_load(ctx->session_flag)) {
      return true;
   }
   return ctx->honor_global && llm_is_interrupt_requested();
}

int llm_sleep_with_interrupt_check_ctx(int total_ms, const llm_interrupt_ctx_t *ctx) {
   /* 250ms chunks: below the ~400ms wake-word-feels-laggy threshold, and 2.5x
    * fewer scheduler wakeups than 100ms on a multi-second backoff / slot wait.
    * Integer-ms counting avoids float drift; under heavy load a chunk may
    * oversleep, which only lengthens the wait — safe for both callers (a longer
    * backoff, or a longer rate-limit wait whose retry loop re-checks the count). */
   struct timespec chunk = { .tv_sec = 0, .tv_nsec = LLM_INTERRUPT_SLEEP_CHUNK_MS * 1000000L };
   int elapsed_ms = 0;
   while (elapsed_ms < total_ms) {
      if (llm_interrupt_ctx_triggered(ctx)) {
         return 1;
      }
      nanosleep(&chunk, NULL);
      elapsed_ms += LLM_INTERRUPT_SLEEP_CHUNK_MS;
   }
   return 0;
}

int llm_sleep_with_interrupt_check(int total_ms) {
   return llm_sleep_with_interrupt_check_ctx(total_ms, NULL);
}

int llm_rate_limit_wait_ctx(const llm_interrupt_ctx_t *ctx) {
   /* Fast path: disabled — zero overhead */
   if (atomic_load(&s_limiter.max_rpm) <= 0)
      return 0;

   pthread_mutex_lock(&s_limiter.mutex);
   expire_old_entries(&s_limiter);

   int current_rpm = atomic_load(&s_limiter.max_rpm);

   /* TOCTOU re-check loop: another thread may have filled the window while we slept */
   while (s_limiter.count >= current_rpm && current_rpm > 0) {
      /* Compute how long until the oldest entry expires */
      int tidx = tail_index(s_limiter.head, s_limiter.count);
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      double age = timespec_diff_sec(&now, &s_limiter.timestamps[tidx]);
      double wait_sec = (double)WINDOW_SECONDS - age;

      if (wait_sec <= 0) {
         /* Entry already expired, just re-expire */
         expire_old_entries(&s_limiter);
         continue;
      }

      OLOG_WARNING("Rate limit: %d/%d RPM, waiting %.1fs for slot", s_limiter.count, current_rpm,
                   wait_sec);

      pthread_mutex_unlock(&s_limiter.mutex);

      /* Wait for the slot to free up, interruptibly (shared helper — 250ms
       * chunks).  The chunk granularity rounds the wait up to the next chunk,
       * which is fine: the re-locked loop below re-checks the count anyway.
       * wait_sec is strictly > 0 here (the <= 0 case continued above); floor to
       * 1ms so a sub-millisecond wait still sleeps a chunk instead of spinning
       * the retry loop until the tail entry ages out. */
      int wait_ms = (int)(wait_sec * 1000.0);
      if (wait_ms < 1) {
         wait_ms = 1;
      }
      if (llm_sleep_with_interrupt_check_ctx(wait_ms, ctx)) {
         OLOG_INFO("Rate limit wait interrupted");
         return 1;
      }

      /* Re-lock and re-expire (TOCTOU fix: loop will re-check count) */
      pthread_mutex_lock(&s_limiter.mutex);
      expire_old_entries(&s_limiter);
      current_rpm = atomic_load(&s_limiter.max_rpm);
   }

   /* Record this request's timestamp */
   s_limiter.timestamps[s_limiter.head] = (struct timespec){ 0 };
   clock_gettime(CLOCK_MONOTONIC, &s_limiter.timestamps[s_limiter.head]);
   s_limiter.head = (s_limiter.head + 1) % LLM_RATE_LIMIT_MAX_SLOTS;
   if (s_limiter.count < LLM_RATE_LIMIT_MAX_SLOTS)
      s_limiter.count++;

   pthread_mutex_unlock(&s_limiter.mutex);
   return 0;
}

int llm_rate_limit_wait(void) {
   return llm_rate_limit_wait_ctx(NULL);
}

void llm_rate_limit_set_rpm(int max_rpm) {
   if (max_rpm > LLM_RATE_LIMIT_MAX_SLOTS) {
      OLOG_WARNING("Rate limit RPM %d exceeds max slots %d, clamping", max_rpm,
                   LLM_RATE_LIMIT_MAX_SLOTS);
      max_rpm = LLM_RATE_LIMIT_MAX_SLOTS;
   }

   int old_rpm = atomic_exchange(&s_limiter.max_rpm, max_rpm);

   if (old_rpm != max_rpm) {
      if (max_rpm > 0) {
         OLOG_INFO("LLM rate limit updated: %d -> %d RPM", old_rpm, max_rpm);
      } else {
         OLOG_INFO("LLM rate limit disabled (was %d RPM)", old_rpm);
      }
   }
}

void llm_rate_limit_cleanup(void) {
   pthread_mutex_lock(&s_limiter.mutex);
   atomic_store(&s_limiter.max_rpm, 0);
   s_limiter.count = 0;
   s_limiter.head = 0;
   pthread_mutex_unlock(&s_limiter.mutex);
   pthread_mutex_destroy(&s_limiter.mutex);
}
