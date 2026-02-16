// Enable GNU extensions for pthread_timedjoin_np
#define _GNU_SOURCE

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
 * Worker Thread Pool for multi-client support.
 * Each worker handles a complete client pipeline: Audio → ASR → LLM → TTS → Response.
 *
 * DESIGN:
 *   - EAGER initialization: All ASR contexts created at startup
 *   - Worker threads wait on condition variable for client assignment
 *   - Each worker exclusively owns its session during processing
 *   - Graceful shutdown with timeout (35s > 30s LLM timeout)
 *
 * THREAD SAFETY:
 *   - Pool-level mutex for worker assignment
 *   - Per-worker mutex + condition for client assignment
 *   - Session exclusive ownership during worker operation
 */

#include "core/worker_pool.h"

#include <errno.h>
#include <mosquitto.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "asr/asr_interface.h"
#include "config/dawn_config.h"
#include "core/session_manager.h"
#include "dawn.h"
#include "logging.h"
#include "tts/text_to_speech.h"

// =============================================================================
// Module State
// =============================================================================

// Static array sized to maximum; actual_worker_count determines how many are used
static worker_context_t workers[WORKER_POOL_MAX_SIZE];
static int actual_worker_count = 0;  // Set from config at init
static bool pool_initialized = false;

// Pool-level mutex for finding/assigning workers
static pthread_mutex_t pool_mutex = PTHREAD_MUTEX_INITIALIZER;

// Mosquitto instance for command processing
static struct mosquitto *worker_mosq = NULL;

// Shutdown timeout: 35 seconds (> 30s LLM timeout to allow graceful finish)
// This is a safety net for worst-case scenarios. In practice, workers exit quickly
// once they check pool_initialized flag. For restart scenarios, the main LLM thread
// is already interrupted with a 5s timeout before we reach worker pool shutdown.
#define SHUTDOWN_TIMEOUT_SEC 35

// Timeout for borrowing ASR context (milliseconds)
// WebUI clients will wait this long for a worker before failing
#define WORKER_BORROW_TIMEOUT_MS 5000

// Condition variable for signaling worker availability
static pthread_cond_t pool_available_cond = PTHREAD_COND_INITIALIZER;

// =============================================================================
// Forward Declarations
// =============================================================================

static void *worker_thread(void *arg);

// =============================================================================
// Lifecycle Functions
// =============================================================================

int worker_pool_init(asr_engine_type_t engine_type, const char *model_path) {
   if (pool_initialized) {
      LOG_WARNING("Worker pool already initialized");
      return 0;
   }

   if (!model_path) {
      LOG_ERROR("Worker pool init: model_path is NULL");
      return 1;
   }

   // Determine worker count from config (clamped to valid range)
   int config_workers = g_config.network.workers;
   if (config_workers <= 0) {
      actual_worker_count = WORKER_POOL_DEFAULT_SIZE;
   } else if (config_workers > WORKER_POOL_MAX_SIZE) {
      LOG_WARNING("Config network.workers=%d exceeds max %d, clamping", config_workers,
                  WORKER_POOL_MAX_SIZE);
      actual_worker_count = WORKER_POOL_MAX_SIZE;
   } else {
      actual_worker_count = config_workers;
   }

   LOG_INFO("Initializing worker pool with %d workers (%s engine)", actual_worker_count,
            asr_engine_name(engine_type));

   // Initialize all worker contexts
   for (int i = 0; i < actual_worker_count; i++) {
      worker_context_t *w = &workers[i];

      w->worker_id = i;
      w->client_fd = -1;
      w->session = NULL;
      w->state = WORKER_STATE_IDLE;

      // Initialize synchronization primitives
      if (pthread_mutex_init(&w->mutex, NULL) != 0) {
         LOG_ERROR("Worker %d: Failed to init mutex", i);
         goto cleanup_workers;
      }

      if (pthread_cond_init(&w->client_ready_cond, NULL) != 0) {
         LOG_ERROR("Worker %d: Failed to init condition variable", i);
         pthread_mutex_destroy(&w->mutex);
         goto cleanup_workers;
      }

      // EAGER: Create ASR context now (fail fast)
      w->asr_ctx = asr_init(engine_type, model_path, 16000);
      if (!w->asr_ctx) {
         LOG_ERROR("Worker %d: Failed to initialize ASR context", i);
         pthread_cond_destroy(&w->client_ready_cond);
         pthread_mutex_destroy(&w->mutex);
         goto cleanup_workers;
      }

      // Spawn worker thread
      if (pthread_create(&w->thread, NULL, worker_thread, w) != 0) {
         LOG_ERROR("Worker %d: Failed to create thread", i);
         asr_cleanup(w->asr_ctx);
         w->asr_ctx = NULL;
         pthread_cond_destroy(&w->client_ready_cond);
         pthread_mutex_destroy(&w->mutex);
         goto cleanup_workers;
      }

      LOG_INFO("Worker %d: Initialized and started", i);
   }

   pool_initialized = true;
   LOG_INFO("Worker pool initialized: %d workers ready", actual_worker_count);
   return 0;

cleanup_workers:
   // Clean up already-initialized workers (up to how many we tried to init)
   for (int j = 0; j < actual_worker_count; j++) {
      worker_context_t *w = &workers[j];

      if (w->asr_ctx) {
         // Set shutdown and signal to terminate thread
         pthread_mutex_lock(&w->mutex);
         w->state = WORKER_STATE_SHUTDOWN;
         pthread_cond_signal(&w->client_ready_cond);
         pthread_mutex_unlock(&w->mutex);

         // Wait for thread to exit
         pthread_join(w->thread, NULL);

         asr_cleanup(w->asr_ctx);
         w->asr_ctx = NULL;
         pthread_cond_destroy(&w->client_ready_cond);
         pthread_mutex_destroy(&w->mutex);
      }
   }

   return 1;
}

void worker_pool_shutdown(void) {
   if (!pool_initialized) {
      return;
   }

   LOG_INFO("Shutting down worker pool...");

   // Wake any threads waiting to borrow ASR contexts so they can fail gracefully
   pthread_mutex_lock(&pool_mutex);
   pthread_cond_broadcast(&pool_available_cond);
   pthread_mutex_unlock(&pool_mutex);

   // Phase 1: Signal all workers to shutdown
   for (int i = 0; i < actual_worker_count; i++) {
      worker_context_t *w = &workers[i];

      pthread_mutex_lock(&w->mutex);
      w->state = WORKER_STATE_SHUTDOWN;

      // Mark session as disconnected to abort LLM calls
      if (w->session) {
         w->session->disconnected = true;
      }

      pthread_cond_signal(&w->client_ready_cond);
      pthread_mutex_unlock(&w->mutex);
   }

   // Phase 2: Wait for workers to finish (with timeout)
   struct timespec deadline;
   clock_gettime(CLOCK_REALTIME, &deadline);
   deadline.tv_sec += SHUTDOWN_TIMEOUT_SEC;

   for (int i = 0; i < actual_worker_count; i++) {
      worker_context_t *w = &workers[i];

      // Try timed join
      int result = pthread_timedjoin_np(w->thread, NULL, &deadline);

      if (result == ETIMEDOUT) {
         LOG_WARNING("Worker %d: Timeout waiting for shutdown, canceling", i);
         pthread_cancel(w->thread);
         pthread_join(w->thread, NULL);
      } else if (result != 0) {
         LOG_ERROR("Worker %d: pthread_timedjoin_np failed: %d", i, result);
         pthread_cancel(w->thread);
         pthread_join(w->thread, NULL);
      }

      // Close client socket if still open
      if (w->client_fd >= 0) {
         close(w->client_fd);
         w->client_fd = -1;
      }

      // Clean up ASR context
      if (w->asr_ctx) {
         asr_cleanup(w->asr_ctx);
         w->asr_ctx = NULL;
      }

      pthread_cond_destroy(&w->client_ready_cond);
      pthread_mutex_destroy(&w->mutex);

      LOG_INFO("Worker %d: Shutdown complete", i);
   }

   pool_initialized = false;
   LOG_INFO("Worker pool shutdown complete");
}

// =============================================================================
// Client Assignment
// =============================================================================

int worker_pool_assign_client(int client_fd, session_t *session) {
   if (!pool_initialized) {
      LOG_ERROR("Cannot assign client: worker pool not initialized");
      return 1;
   }

   if (client_fd < 0 || !session) {
      LOG_ERROR("Invalid client assignment: fd=%d, session=%p", client_fd, (void *)session);
      return 1;
   }

   pthread_mutex_lock(&pool_mutex);

   // Find an idle worker
   worker_context_t *available = NULL;
   for (int i = 0; i < actual_worker_count; i++) {
      if (workers[i].state == WORKER_STATE_IDLE) {
         available = &workers[i];
         break;
      }
   }

   if (!available) {
      pthread_mutex_unlock(&pool_mutex);
      LOG_WARNING("All workers busy, cannot accept client (fd=%d)", client_fd);
      return 1;
   }

   // Assign client to worker
   pthread_mutex_lock(&available->mutex);

   available->client_fd = client_fd;
   available->session = session;
   available->state = WORKER_STATE_BUSY;

   // Signal worker thread
   pthread_cond_signal(&available->client_ready_cond);
   pthread_mutex_unlock(&available->mutex);

   pthread_mutex_unlock(&pool_mutex);

   LOG_INFO("Client (fd=%d) assigned to worker %d", client_fd, available->worker_id);
   return 0;
}

// =============================================================================
// ASR Context Borrowing
// =============================================================================

asr_context_t *worker_pool_borrow_asr(void) {
   if (!pool_initialized) {
      LOG_ERROR("Cannot borrow ASR: worker pool not initialized");
      return NULL;
   }

   pthread_mutex_lock(&pool_mutex);

   // Calculate absolute timeout
   struct timespec timeout;
   clock_gettime(CLOCK_REALTIME, &timeout);
   timeout.tv_sec += WORKER_BORROW_TIMEOUT_MS / 1000;
   timeout.tv_nsec += (WORKER_BORROW_TIMEOUT_MS % 1000) * 1000000;
   if (timeout.tv_nsec >= 1000000000) {
      timeout.tv_sec++;
      timeout.tv_nsec -= 1000000000;
   }

   // Find an idle worker, waiting if necessary
   worker_context_t *available = NULL;
   while (!available) {
      for (int i = 0; i < actual_worker_count; i++) {
         if (workers[i].state == WORKER_STATE_IDLE) {
            available = &workers[i];
            break;
         }
      }

      if (!available) {
         // Wait for a worker to become available (with timeout)
         LOG_INFO("All workers busy, waiting up to %dms for availability...",
                  WORKER_BORROW_TIMEOUT_MS);
         int wait_result = pthread_cond_timedwait(&pool_available_cond, &pool_mutex, &timeout);
         if (wait_result == ETIMEDOUT) {
            pthread_mutex_unlock(&pool_mutex);
            LOG_WARNING("Timed out waiting for ASR worker (all busy for %dms)",
                        WORKER_BORROW_TIMEOUT_MS);
            return NULL;
         } else if (wait_result != 0) {
            pthread_mutex_unlock(&pool_mutex);
            LOG_ERROR("Error waiting for ASR worker: %s", strerror(wait_result));
            return NULL;
         }
         // Check if pool is shutting down (broadcast wakes us during shutdown)
         if (!pool_initialized) {
            pthread_mutex_unlock(&pool_mutex);
            LOG_INFO("Worker pool shutting down, aborting ASR borrow");
            return NULL;
         }
         // Loop back to check for available worker
      }
   }

   // Mark worker as busy (but don't signal thread - we're using ASR directly)
   pthread_mutex_lock(&available->mutex);
   available->state = WORKER_STATE_BUSY;
   pthread_mutex_unlock(&available->mutex);

   asr_context_t *ctx = available->asr_ctx;

   pthread_mutex_unlock(&pool_mutex);

   LOG_INFO("Borrowed ASR context from worker %d", available->worker_id);
   return ctx;
}

void worker_pool_return_asr(asr_context_t *ctx) {
   if (!ctx || !pool_initialized) {
      return;
   }

   pthread_mutex_lock(&pool_mutex);

   // Find the worker that owns this ASR context
   for (int i = 0; i < actual_worker_count; i++) {
      if (workers[i].asr_ctx == ctx) {
         pthread_mutex_lock(&workers[i].mutex);
         workers[i].state = WORKER_STATE_IDLE;
         pthread_mutex_unlock(&workers[i].mutex);

         LOG_INFO("Returned ASR context to worker %d", i);

         // Signal any threads waiting for a worker
         pthread_cond_signal(&pool_available_cond);
         break;
      }
   }

   pthread_mutex_unlock(&pool_mutex);
}

// =============================================================================
// Metrics
// =============================================================================

int worker_pool_active_count(void) {
   if (!pool_initialized) {
      return 0;
   }

   int count = 0;
   pthread_mutex_lock(&pool_mutex);

   for (int i = 0; i < actual_worker_count; i++) {
      if (workers[i].state == WORKER_STATE_BUSY) {
         count++;
      }
   }

   pthread_mutex_unlock(&pool_mutex);
   return count;
}

int worker_pool_size(void) {
   return actual_worker_count;
}

worker_state_t worker_pool_get_state(int worker_id) {
   if (!pool_initialized || worker_id < 0 || worker_id >= actual_worker_count) {
      return WORKER_STATE_SHUTDOWN;
   }

   return workers[worker_id].state;
}

bool worker_pool_is_initialized(void) {
   return pool_initialized;
}

void worker_pool_set_mosq(struct mosquitto *mosq) {
   worker_mosq = mosq;
}

struct mosquitto *worker_pool_get_mosq(void) {
   return worker_mosq;
}

// =============================================================================
// Worker Thread
// =============================================================================

/**
 * @brief Pthread cleanup handler for worker thread cancellation
 *
 * Called when worker is canceled via pthread_cancel() during shutdown.
 * Ensures socket is closed and session reference is released to prevent leaks.
 */
static void worker_cleanup_handler(void *arg) {
   worker_context_t *ctx = (worker_context_t *)arg;

   LOG_INFO("Worker %d: Cleanup handler invoked (cancellation)", ctx->worker_id);

   // Close client socket if open
   if (ctx->client_fd >= 0) {
      close(ctx->client_fd);
      ctx->client_fd = -1;
   }

   // Release session reference
   if (ctx->session) {
      session_release(ctx->session);
      ctx->session = NULL;
   }

   // Reset state
   ctx->state = WORKER_STATE_IDLE;

   // Signal any threads waiting for a worker.
   // NOTE: Using trylock because we're in a pthread cancellation context where the
   // thread may have been holding mutexes. If trylock fails (mutex held elsewhere),
   // the signal is skipped. This is acceptable because:
   // 1. Cancellation is only used during shutdown as a last resort
   // 2. worker_pool_shutdown() broadcasts to all waiters before canceling threads
   // 3. Borrowers have a 5-second timeout and will eventually wake up
   if (pthread_mutex_trylock(&pool_mutex) == 0) {
      pthread_cond_signal(&pool_available_cond);
      pthread_mutex_unlock(&pool_mutex);
   }
}

/**
 * @brief Main worker thread function
 *
 * Worker lifecycle:
 * 1. Wait for client assignment (condition variable)
 * 2. Process client request (ASR → LLM → TTS → Response)
 * 3. Mark self as idle
 * 4. Repeat until shutdown
 */
static void *worker_thread(void *arg) {
   worker_context_t *ctx = (worker_context_t *)arg;

   LOG_INFO("Worker %d: Thread started", ctx->worker_id);

   while (1) {
      pthread_mutex_lock(&ctx->mutex);

      // Wait for client assignment or shutdown
      while (ctx->state == WORKER_STATE_IDLE) {
         pthread_cond_wait(&ctx->client_ready_cond, &ctx->mutex);
      }

      // Check for shutdown
      if (ctx->state == WORKER_STATE_SHUTDOWN) {
         pthread_mutex_unlock(&ctx->mutex);
         LOG_INFO("Worker %d: Shutdown signal received", ctx->worker_id);
         break;
      }

      pthread_mutex_unlock(&ctx->mutex);

      // Register cleanup handler for pthread_cancel() during client processing
      // This ensures resources are released even if thread is forcibly canceled
      pthread_cleanup_push(worker_cleanup_handler, ctx);

      // Process the client (state is BUSY)
      LOG_INFO("Worker %d: Processing client (fd=%d)", ctx->worker_id, ctx->client_fd);

      // Clean up and return to idle
      pthread_mutex_lock(&ctx->mutex);

      // Close client socket
      if (ctx->client_fd >= 0) {
         close(ctx->client_fd);
         ctx->client_fd = -1;
      }

      // Release session reference (decrements ref_count)
      // Session was created by accept thread with ref_count=1, worker must release
      if (ctx->session) {
         session_release(ctx->session);
         ctx->session = NULL;
      }
      ctx->state = WORKER_STATE_IDLE;

      pthread_mutex_unlock(&ctx->mutex);

      // Signal any threads waiting for a worker (e.g., WebUI ASR requests)
      pthread_mutex_lock(&pool_mutex);
      pthread_cond_signal(&pool_available_cond);
      pthread_mutex_unlock(&pool_mutex);

      // Unregister cleanup handler (execute=0 since we did manual cleanup)
      pthread_cleanup_pop(0);

      LOG_INFO("Worker %d: Returned to idle", ctx->worker_id);
   }

   LOG_INFO("Worker %d: Thread exiting", ctx->worker_id);
   return NULL;
}
