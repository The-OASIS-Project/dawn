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
 */

#ifndef WORKER_POOL_H
#define WORKER_POOL_H

#include <mosquitto.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "asr/asr_interface.h"
#include "core/session_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

// Maximum worker pool size (compile-time limit for static allocation)
#define WORKER_POOL_MAX_SIZE 8

// Default worker count if not specified in config
#define WORKER_POOL_DEFAULT_SIZE 4

#define WORKER_LLM_TIMEOUT_MS 30000  // 30 second LLM timeout

/**
 * @brief Worker state enumeration
 */
typedef enum {
   WORKER_STATE_IDLE,      // Waiting for client assignment
   WORKER_STATE_BUSY,      // Processing a client request
   WORKER_STATE_SHUTDOWN,  // Shutdown requested
} worker_state_t;

/**
 * @brief Per-worker context - everything needed for full pipeline
 *
 * @ownership Worker pool owns all worker contexts
 * @thread_safety Each worker thread exclusively owns its context during operation
 */
typedef struct {
   int worker_id;
   pthread_t thread;

   // Client connection (assigned per-request)
   int client_fd;
   session_t *session;  // Session with conversation history

   // Per-worker resources (created at init, reused)
   asr_context_t *asr_ctx;  // Own ASR context (Vosk or Whisper)

   // Synchronization for client assignment
   pthread_mutex_t mutex;
   pthread_cond_t client_ready_cond;

   // State
   volatile worker_state_t state;
} worker_context_t;

// =============================================================================
// Lifecycle Functions
// =============================================================================

/**
 * @brief Initialize worker pool (EAGER initialization)
 *
 * All worker resources are allocated at startup:
 * - Worker count from config (clamped to WORKER_POOL_MAX_SIZE)
 * - ASR contexts created immediately for each worker
 * - Worker threads spawned and waiting for clients
 * - Fail fast if model load fails (don't wait for first client)
 *
 * @param engine_type ASR engine to use (ASR_ENGINE_VOSK or ASR_ENGINE_WHISPER)
 * @param model_path Path to ASR model (Vosk model dir or Whisper .bin file)
 * @return 0 on success, 1 on failure
 *
 * @note Rationale: Lazy init would cause 1-2s latency on first client
 *       (especially Whisper model load). For embedded system, eager init
 *       ensures predictable behavior and simpler error handling.
 */
int worker_pool_init(asr_engine_type_t engine_type, const char *model_path);

/**
 * @brief Shutdown worker pool gracefully
 *
 * Shutdown sequence:
 * 1. Set shutdown state on all workers
 * 2. Signal all worker condition variables to wake them
 * 3. Set session->disconnected on all active sessions (aborts LLM calls)
 * 4. Wait for workers to finish (up to 35s > 30s LLM timeout)
 * 5. Use pthread_cancel() as last resort if still blocked
 * 6. Clean up ASR contexts and close sockets
 */
void worker_pool_shutdown(void);

// =============================================================================
// Client Assignment
// =============================================================================

/**
 * @brief Assign client to available worker
 *
 * @param client_fd Client socket
 * @param session Client session (created by accept thread)
 * @return 0 on success, 1 if all workers busy
 *
 * @note Session ownership transfers to worker until request completes
 * @note Caller should send NACK to client if this returns failure
 */
int worker_pool_assign_client(int client_fd, session_t *session);

// =============================================================================
// ASR Context Borrowing (for WebUI and other non-DAP clients)
// =============================================================================

/**
 * @brief Borrow an idle worker's ASR context for direct use
 *
 * This allows WebUI and other clients to use the pre-initialized ASR contexts
 * from the worker pool instead of creating their own (which wastes GPU memory).
 *
 * The worker is marked as BUSY while its ASR context is borrowed, preventing
 * it from being assigned to DAP clients.
 *
 * @return Pointer to borrowed ASR context, or NULL if all workers busy
 *
 * @note Caller MUST call worker_pool_return_asr() when done
 * @note Thread-safe (uses pool mutex)
 */
asr_context_t *worker_pool_borrow_asr(void);

/**
 * @brief Return a borrowed ASR context to the worker pool
 *
 * Marks the owning worker as IDLE, allowing it to accept new clients.
 *
 * @param ctx ASR context previously obtained from worker_pool_borrow_asr()
 *
 * @note Safe to call with NULL (no-op)
 * @note Thread-safe (uses pool mutex)
 */
void worker_pool_return_asr(asr_context_t *ctx);

// =============================================================================
// Metrics
// =============================================================================

/**
 * @brief Get total worker count (from config)
 *
 * @return Total number of workers in the pool
 */
int worker_pool_size(void);

/**
 * @brief Get worker utilization for metrics
 *
 * @return Number of active (busy) workers
 */
int worker_pool_active_count(void);

/**
 * @brief Get worker state for metrics display
 *
 * @param worker_id Worker ID (0 to worker_pool_size()-1)
 * @return Worker state, or WORKER_STATE_SHUTDOWN if invalid ID
 */
worker_state_t worker_pool_get_state(int worker_id);

/**
 * @brief Check if worker pool is initialized
 *
 * @return true if initialized, false otherwise
 */
bool worker_pool_is_initialized(void);

/**
 * @brief Set mosquitto instance for command processing
 *
 * Worker pool needs access to mosquitto to execute parsed commands
 * (e.g., "get time" -> MQTT message to get system time).
 *
 * @param mosq Mosquitto client instance
 */
void worker_pool_set_mosq(struct mosquitto *mosq);

/**
 * @brief Get mosquitto instance for command processing
 *
 * Used by other modules (e.g., WebUI) that need to publish MQTT commands.
 *
 * @return Mosquitto client instance, or NULL if not set
 */
struct mosquitto *worker_pool_get_mosq(void);

#ifdef __cplusplus
}
#endif

#endif  // WORKER_POOL_H
