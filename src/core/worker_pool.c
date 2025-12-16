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
#include <json-c/json.h>
#include <mosquitto.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "asr/asr_interface.h"
#include "config/dawn_config.h"
#include "core/command_router.h"
#include "core/session_manager.h"
#include "dawn.h"
#include "llm/llm_command_parser.h"
#include "logging.h"
#include "network/dawn_server.h"
#include "network/dawn_wav_utils.h"
#include "tts/text_to_speech.h"
#include "tts/tts_preprocessing.h"

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
#define SHUTDOWN_TIMEOUT_SEC 35

// =============================================================================
// Forward Declarations
// =============================================================================

static void *worker_thread(void *arg);
static int worker_handle_client(worker_context_t *ctx);
static char *process_commands_with_routing(const char *llm_response,
                                           int worker_id,
                                           session_t *session);

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
// Command Processing with Request/Response
// =============================================================================

#define MAX_TOOL_RESULTS 8
#define TOOL_RESULT_MSG_SIZE 2048

/**
 * @brief Process commands in LLM response with request/response routing
 *
 * Extracts <command> tags from LLM response, publishes each command with a
 * request_id via MQTT, waits for callback results, then sends results back
 * to the LLM for a natural language response.
 *
 * This mimics the local assistant's flow where the LLM sees callback results
 * and generates an appropriate response.
 *
 * @param llm_response Full LLM response text containing commands
 * @param worker_id Worker ID for request tracking
 * @param session Session for follow-up LLM call
 * @return LLM's response after seeing tool results, or NULL on error
 *
 * @note Caller must free returned string
 */
static char *process_commands_with_routing(const char *llm_response,
                                           int worker_id,
                                           session_t *session) {
   if (!llm_response || !session) {
      return NULL;
   }

   // Collect all tool results
   char *tool_results[MAX_TOOL_RESULTS];
   int num_results = 0;

   for (int i = 0; i < MAX_TOOL_RESULTS; i++) {
      tool_results[i] = NULL;
   }

   // Search for <command> tags and process each one
   const char *search_ptr = llm_response;
   const char *cmd_start;

   while ((cmd_start = strstr(search_ptr, "<command>")) != NULL && num_results < MAX_TOOL_RESULTS) {
      const char *cmd_end = strstr(cmd_start, "</command>");
      if (!cmd_end) {
         LOG_WARNING("Worker %d: Unclosed <command> tag", worker_id);
         break;
      }

      // Extract command JSON
      const char *json_start = cmd_start + strlen("<command>");
      size_t json_len = cmd_end - json_start;

      char *cmd_json = malloc(json_len + 1);
      if (!cmd_json) {
         LOG_ERROR("Worker %d: Failed to allocate command JSON", worker_id);
         break;
      }
      memcpy(cmd_json, json_start, json_len);
      cmd_json[json_len] = '\0';

      LOG_INFO("Worker %d: Processing command: %s", worker_id, cmd_json);

      // Parse JSON to extract device/action
      struct json_object *parsed_json = json_tokener_parse(cmd_json);
      if (!parsed_json) {
         LOG_WARNING("Worker %d: Invalid command JSON: %s", worker_id, cmd_json);
         free(cmd_json);
         search_ptr = cmd_end + strlen("</command>");
         continue;
      }

      // Get device and action for result formatting
      struct json_object *device_obj = NULL;
      struct json_object *action_obj = NULL;
      const char *device_name = "unknown";
      const char *action_name = "unknown";

      if (json_object_object_get_ex(parsed_json, "device", &device_obj)) {
         device_name = json_object_get_string(device_obj);
      }
      if (json_object_object_get_ex(parsed_json, "action", &action_obj)) {
         action_name = json_object_get_string(action_obj);
      }

      // Register pending request
      pending_request_t *req = command_router_register(worker_id);
      if (!req) {
         LOG_ERROR("Worker %d: Failed to register pending request", worker_id);
         json_object_put(parsed_json);
         free(cmd_json);
         search_ptr = cmd_end + strlen("</command>");
         continue;
      }

      const char *request_id = command_router_get_id(req);
      LOG_INFO("Worker %d: Registered request %s", worker_id, request_id);

      // Add request_id to command JSON
      json_object_object_add(parsed_json, "request_id", json_object_new_string(request_id));
      const char *cmd_with_id = json_object_to_json_string(parsed_json);

      // Publish command via MQTT (must use APPLICATION_NAME topic to match subscription)
      if (worker_mosq) {
         int rc = mosquitto_publish(worker_mosq, NULL, APPLICATION_NAME, strlen(cmd_with_id),
                                    cmd_with_id, 0, false);
         if (rc != MOSQ_ERR_SUCCESS) {
            LOG_ERROR("Worker %d: MQTT publish failed: %d", worker_id, rc);
            command_router_cancel(req);
            json_object_put(parsed_json);
            free(cmd_json);
            search_ptr = cmd_end + strlen("</command>");
            continue;
         }
         LOG_INFO("Worker %d: Published command to %s", worker_id, APPLICATION_NAME);
      } else {
         LOG_WARNING("Worker %d: No MQTT connection, cannot process command", worker_id);
         command_router_cancel(req);
         json_object_put(parsed_json);
         free(cmd_json);
         search_ptr = cmd_end + strlen("</command>");
         continue;
      }

      // Wait for result
      char *callback_result = command_router_wait(req, COMMAND_RESULT_TIMEOUT_MS);

      // Format result for LLM
      tool_results[num_results] = malloc(TOOL_RESULT_MSG_SIZE);
      if (tool_results[num_results]) {
         if (callback_result && strlen(callback_result) > 0) {
            LOG_INFO("Worker %d: Received callback result: %.50s%s", worker_id, callback_result,
                     strlen(callback_result) > 50 ? "..." : "");
            snprintf(tool_results[num_results], TOOL_RESULT_MSG_SIZE,
                     "[Tool Result: %s.%s returned: %s]", device_name, action_name,
                     callback_result);
         } else {
            LOG_WARNING("Worker %d: No callback result (timeout or empty)", worker_id);
            snprintf(tool_results[num_results], TOOL_RESULT_MSG_SIZE,
                     "[Tool Result: %s.%s completed successfully]", device_name, action_name);
         }
         num_results++;
      }

      if (callback_result) {
         free(callback_result);
      }

      json_object_put(parsed_json);
      free(cmd_json);
      search_ptr = cmd_end + strlen("</command>");
   }

   // If no results collected, return NULL (no commands processed)
   if (num_results == 0) {
      return NULL;
   }

   // Build combined tool results message for LLM
   size_t total_len = 1;  // For null terminator
   for (int i = 0; i < num_results; i++) {
      if (tool_results[i]) {
         total_len += strlen(tool_results[i]) + 1;  // +1 for newline
      }
   }

   char *combined_results = malloc(total_len);
   if (!combined_results) {
      LOG_ERROR("Worker %d: Failed to allocate combined results", worker_id);
      for (int i = 0; i < num_results; i++) {
         free(tool_results[i]);
      }
      return NULL;
   }

   combined_results[0] = '\0';
   for (int i = 0; i < num_results; i++) {
      if (tool_results[i]) {
         strcat(combined_results, tool_results[i]);
         if (i < num_results - 1) {
            strcat(combined_results, "\n");
         }
         free(tool_results[i]);
      }
   }

   LOG_INFO("Worker %d: Sending tool results to LLM: %s", worker_id, combined_results);

   // Make follow-up LLM call with tool results
   char *final_response = session_llm_call(session, combined_results);

   free(combined_results);

   if (!final_response) {
      LOG_ERROR("Worker %d: Follow-up LLM call failed", worker_id);
      return NULL;
   }

   LOG_INFO("Worker %d: LLM final response: %.50s%s", worker_id, final_response,
            strlen(final_response) > 50 ? "..." : "");

   return final_response;
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

      int result = worker_handle_client(ctx);

      if (result != 0) {
         LOG_WARNING("Worker %d: Client processing failed", ctx->worker_id);
      }

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

      // Unregister cleanup handler (execute=0 since we did manual cleanup)
      pthread_cleanup_pop(0);

      LOG_INFO("Worker %d: Returned to idle", ctx->worker_id);
   }

   LOG_INFO("Worker %d: Thread exiting", ctx->worker_id);
   return NULL;
}

/**
 * @brief Handle client request - full ASR → LLM → TTS pipeline
 *
 * This function implements the complete client processing pipeline:
 * 1. DAP handshake
 * 2. Receive audio data from client
 * 3. Extract PCM from WAV
 * 4. Process through ASR
 * 5. Call LLM with session context
 * 6. Generate TTS response
 * 7. Truncate if exceeds ESP32 limits
 * 8. Send response to client
 *
 * @param ctx Worker context with client_fd, session, asr_ctx
 * @return 0 on success, 1 on failure
 */
static int worker_handle_client(worker_context_t *ctx) {
   int result = 1;  // Default to failure
   uint8_t *audio_data = NULL;
   size_t audio_size = 0;
   NetworkPCMData *pcm = NULL;
   asr_result_t *asr_result = NULL;
   char *llm_response = NULL;
   uint8_t *tts_wav = NULL;
   size_t tts_size = 0;
   uint8_t *response_wav = NULL;
   size_t response_size = 0;
   uint8_t *truncated_wav = NULL;
   size_t truncated_size = 0;

   // Check for disconnection at start
   if (ctx->session && ctx->session->disconnected) {
      LOG_INFO("Worker %d: Session disconnected at start", ctx->worker_id);
      return 1;
   }

   LOG_INFO("Worker %d: Starting client pipeline (fd=%d)", ctx->worker_id, ctx->client_fd);

   // Initialize DAP client session for protocol communication
   dawn_client_session_t dap_session = {
      .socket_fd = ctx->client_fd,
      .send_sequence = 0,
      .receive_sequence = 0,
   };
   snprintf(dap_session.client_ip, sizeof(dap_session.client_ip), "worker_%d", ctx->worker_id);

   // Step 1: DAP Handshake
   if (dawn_handle_handshake(&dap_session) != DAWN_SUCCESS) {
      LOG_ERROR("Worker %d: Handshake failed", ctx->worker_id);
      goto cleanup;
   }
   LOG_INFO("Worker %d: Handshake complete", ctx->worker_id);

   // Check for disconnection after handshake
   if (ctx->session && ctx->session->disconnected) {
      LOG_INFO("Worker %d: Session disconnected after handshake", ctx->worker_id);
      goto cleanup;
   }

   // Step 2: Receive audio data
   if (dawn_receive_data_chunks(&dap_session, &audio_data, &audio_size) != DAWN_SUCCESS) {
      LOG_ERROR("Worker %d: Failed to receive audio data", ctx->worker_id);
      goto cleanup;
   }
   LOG_INFO("Worker %d: Received %zu bytes of audio data", ctx->worker_id, audio_size);

   // Check for disconnection after receive
   if (ctx->session && ctx->session->disconnected) {
      LOG_INFO("Worker %d: Session disconnected after receive", ctx->worker_id);
      goto cleanup;
   }

   // Step 3: Extract PCM from WAV
   pcm = extract_pcm_from_network_wav(audio_data, audio_size);
   if (!pcm || !pcm->is_valid) {
      LOG_ERROR("Worker %d: Invalid WAV format", ctx->worker_id);
      // Send error response
      tts_wav = error_to_wav(ERROR_MSG_WAV_INVALID, &tts_size);
      if (tts_wav) {
         dawn_send_data_chunks(&dap_session, tts_wav, tts_size);
         free(tts_wav);
      }
      goto cleanup;
   }
   LOG_INFO("Worker %d: Extracted %zu bytes PCM (%uHz, %u channels)", ctx->worker_id, pcm->pcm_size,
            pcm->sample_rate, pcm->num_channels);

   // Step 4: ASR Processing
   LOG_INFO("Worker %d: Starting ASR processing", ctx->worker_id);

   // Reset ASR state for new utterance
   asr_reset(ctx->asr_ctx);

   // Feed all audio to ASR
   size_t num_samples = pcm->pcm_size / sizeof(int16_t);
   asr_process_partial(ctx->asr_ctx, (const int16_t *)pcm->pcm_data, num_samples);

   // Get final transcription
   asr_result = asr_finalize(ctx->asr_ctx);
   if (!asr_result || !asr_result->text || strlen(asr_result->text) == 0) {
      LOG_WARNING("Worker %d: ASR returned empty result", ctx->worker_id);
      // Send error response
      tts_wav = error_to_wav(ERROR_MSG_SPEECH_FAILED, &tts_size);
      if (tts_wav) {
         dawn_send_data_chunks(&dap_session, tts_wav, tts_size);
         free(tts_wav);
      }
      goto cleanup;
   }
   LOG_INFO("Worker %d: ASR result: \"%s\" (%.1fms)", ctx->worker_id, asr_result->text,
            asr_result->processing_time);

   // Check for disconnection before LLM (long operation)
   if (ctx->session && ctx->session->disconnected) {
      LOG_INFO("Worker %d: Session disconnected before LLM", ctx->worker_id);
      goto cleanup;
   }

   // Step 5: LLM Processing
   LOG_INFO("Worker %d: Calling LLM", ctx->worker_id);
   session_touch(ctx->session);  // Update activity timestamp

   llm_response = session_llm_call(ctx->session, asr_result->text);
   if (!llm_response) {
      LOG_ERROR("Worker %d: LLM call failed or cancelled", ctx->worker_id);
      // Send error response
      tts_wav = error_to_wav(ERROR_MSG_LLM_TIMEOUT, &tts_size);
      if (tts_wav) {
         dawn_send_data_chunks(&dap_session, tts_wav, tts_size);
         free(tts_wav);
      }
      goto cleanup;
   }
   LOG_INFO("Worker %d: LLM response: \"%.50s%s\"", ctx->worker_id, llm_response,
            strlen(llm_response) > 50 ? "..." : "");

   // Step 5b: Process commands and prepare TTS text
   // Use request/response routing for remote clients to get callback results
   // Results are sent back to LLM for natural language response (like local assistant)
   char *tts_text = NULL;

   if (strstr(llm_response, "<command>") != NULL) {
      // Has commands - process with routing and get LLM's final response
      tts_text = process_commands_with_routing(llm_response, ctx->worker_id, ctx->session);
      if (!tts_text) {
         LOG_WARNING("Worker %d: Command processing returned no response", ctx->worker_id);
         // Fall back to original response with tags stripped (handled below)
         tts_text = strdup(llm_response);
         if (!tts_text) {
            LOG_ERROR("Worker %d: strdup failed in command fallback", ctx->worker_id);
            goto cleanup;
         }
      }
   } else {
      // No commands - just use LLM response as-is
      tts_text = strdup(llm_response);
      if (!tts_text) {
         LOG_ERROR("Worker %d: strdup failed for TTS text", ctx->worker_id);
         goto cleanup;
      }
   }

   // Strip any remaining command tags (in case of fallback or nested commands)
   char *cmd_start, *cmd_end;
   while ((cmd_start = strstr(tts_text, "<command>")) != NULL) {
      cmd_end = strstr(cmd_start, "</command>");
      if (cmd_end) {
         cmd_end += strlen("</command>");
         memmove(cmd_start, cmd_end, strlen(cmd_end) + 1);
      } else {
         break;
      }
   }

   // Remove <end_of_turn> tags (local AI models)
   char *match = NULL;
   if ((match = strstr(tts_text, "<end_of_turn>")) != NULL) {
      *match = '\0';
   }

   // Remove special characters that cause TTS problems
   remove_chars(tts_text, "*");
   remove_emojis(tts_text);

   // Trim trailing whitespace
   size_t len = strlen(tts_text);
   while (len > 0 && (tts_text[len - 1] == ' ' || tts_text[len - 1] == '\t' ||
                      tts_text[len - 1] == '\n' || tts_text[len - 1] == '\r')) {
      tts_text[--len] = '\0';
   }

   // Check if there's any text left to speak
   if (len == 0) {
      LOG_INFO("Worker %d: No speech text after command processing", ctx->worker_id);
      free(tts_text);
      // Send success - command was executed, just no speech response
      result = 0;
      goto cleanup;
   }

   // Check for disconnection before TTS
   if (ctx->session && ctx->session->disconnected) {
      LOG_INFO("Worker %d: Session disconnected before TTS", ctx->worker_id);
      free(tts_text);
      goto cleanup;
   }

   // Step 6: TTS Generation
   LOG_INFO("Worker %d: Generating TTS for: \"%.50s%s\"", ctx->worker_id, tts_text,
            strlen(tts_text) > 50 ? "..." : "");
   if (text_to_speech_to_wav(tts_text, &tts_wav, &tts_size) != 0 || !tts_wav) {
      free(tts_text);
      LOG_ERROR("Worker %d: TTS generation failed", ctx->worker_id);
      // Send error response
      uint8_t *err_wav = error_to_wav(ERROR_MSG_TTS_FAILED, &tts_size);
      if (err_wav) {
         dawn_send_data_chunks(&dap_session, err_wav, tts_size);
         free(err_wav);
      }
      goto cleanup;
   }
   LOG_INFO("Worker %d: TTS generated %zu bytes", ctx->worker_id, tts_size);
   free(tts_text);  // Done with TTS text

   // Step 7: Check ESP32 buffer limits and truncate if needed
   response_wav = tts_wav;
   response_size = tts_size;

   if (!check_response_size_limit(tts_size)) {
      LOG_WARNING("Worker %d: Response exceeds ESP32 limits, truncating", ctx->worker_id);
      if (truncate_wav_response(tts_wav, tts_size, &truncated_wav, &truncated_size) == 0 &&
          truncated_wav) {
         response_wav = truncated_wav;
         response_size = truncated_size;
         LOG_INFO("Worker %d: Truncated to %zu bytes", ctx->worker_id, response_size);
      }
   }

   // Step 8: Send response to client
   LOG_INFO("Worker %d: Sending %zu bytes response", ctx->worker_id, response_size);
   if (dawn_send_data_chunks(&dap_session, response_wav, response_size) != DAWN_SUCCESS) {
      LOG_ERROR("Worker %d: Failed to send response", ctx->worker_id);
      goto cleanup;
   }

   LOG_INFO("Worker %d: Client pipeline complete", ctx->worker_id);
   result = 0;  // Success

cleanup:
   // Free all allocated resources
   if (audio_data) {
      free(audio_data);
   }
   if (pcm) {
      free_network_pcm_data(pcm);
   }
   if (asr_result) {
      asr_result_free(asr_result);
   }
   if (llm_response) {
      free(llm_response);
   }
   if (tts_wav) {
      free(tts_wav);
   }
   if (truncated_wav) {
      free(truncated_wav);
   }

   return result;
}
