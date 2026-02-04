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
 * Session manager implementation for multi-client support.
 */

#include "core/session_manager.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_interface.h"
#include "llm/sentence_buffer.h"
#include "logging.h"
#include "memory/memory_extraction.h"
#include "tools/time_utils.h"
#ifdef ENABLE_WEBUI
#include "webui/webui_server.h"
#endif

// =============================================================================
// Streaming Callbacks
// =============================================================================

/* Command tag filtering is now handled entirely in webui_server.c:webui_send_stream_delta().
 * This consolidates all filtering logic in one place with proper state machine handling
 * of partial tags that span chunk boundaries. */

/**
 * @brief Streaming callback for LLM responses
 *
 * Sends text to WebUI for real-time display. Command tag filtering is handled
 * by webui_send_stream_delta() internally. Tracks timing metrics for TTFT
 * and token rate visualization.
 */
static void session_text_chunk_callback(const char *chunk, void *userdata) {
   session_t *session = (session_t *)userdata;

   /* Early exit if client disconnected */
   if (!session || session->disconnected) {
      return;
   }

   /* Chunks are accumulated by the streaming layer (llm_streaming.c)
    * and also sent to WebSocket/satellite for real-time display */
#ifdef ENABLE_WEBUI
   if ((session->type == SESSION_TYPE_WEBSOCKET || session->type == SESSION_TYPE_DAP2) && chunk &&
       chunk[0] != '\0') {
      uint64_t now_ms = get_time_ms();

      /* Track timing for first token (TTFT) - based on LLM output, not filtering */
      if (session->first_token_ms == 0) {
         session->first_token_ms = now_ms;
      }

      /* Track token count and timing for rate calculation */
      session->stream_token_count++;
      session->last_token_ms = now_ms;

      /* Calculate metrics (WebSocket only - satellites don't need browser metrics) */
      if (session->type == SESSION_TYPE_WEBSOCKET) {
         int ttft_ms = 0;
         float token_rate = 0.0f;

         if (session->stream_start_ms > 0) {
            ttft_ms = (int)(session->first_token_ms - session->stream_start_ms);

            /* Calculate tokens per second based on elapsed time since first token */
            uint64_t streaming_duration_ms = now_ms - session->first_token_ms;
            if (streaming_duration_ms > 0 && session->stream_token_count > 1) {
               token_rate = (float)(session->stream_token_count - 1) * 1000.0f /
                            (float)streaming_duration_ms;
            }
         }

         /* Send metrics update periodically (every 5 tokens to avoid flooding) */
         if (session->stream_token_count % 5 == 0 || session->stream_token_count == 1) {
            webui_send_metrics_update(session, "thinking", ttft_ms, token_rate, -1);
         }
      }

      /* Send to WebUI/satellite - filtering and stream_start handled internally */
      webui_send_stream_delta(session, chunk);
   }
#endif
}

// =============================================================================
// Static Variables
// =============================================================================

static session_t *sessions[MAX_SESSIONS];
static pthread_rwlock_t session_manager_rwlock = PTHREAD_RWLOCK_INITIALIZER;
static uint32_t next_session_id = 1;  // 0 is reserved for local session
static bool initialized = false;

/**
 * Thread-local command context - allows device callbacks to access the current session
 *
 * EXPECTED CALLERS:
 *   - Main thread: for local voice commands (dawn.c parse_llm_response_for_commands)
 *   - MQTT thread: for WebUI/DAP commands (mosquitto_comms.c execute_command_for_worker)
 *
 * THREAD SAFETY:
 *   Each thread has its own copy of this pointer. Callers must:
 *   1. Hold a session reference (via session_get/session_retain) while context is set
 *   2. Set context before invoking callbacks
 *   3. Clear context (set to NULL) after callbacks complete
 *   4. Release session reference after clearing context
 *
 * CRITICAL ASSUMPTION:
 *   This works because all paths that set/use this variable execute in the same thread:
 *   - Local voice: main thread sets context, calls parse_llm_response_for_commands(),
 *     callbacks execute in main thread, context cleared
 *   - WebUI/DAP: MQTT on_message() callback sets context, executes device callback,
 *     clears context - all in the single MQTT callback thread
 *
 *   Mosquitto must NOT be configured with MOSQ_OPT_THREADED or multiple event loops,
 *   as this would cause context to be set in one thread and read in another.
 */
static __thread session_t *tl_command_context = NULL;

// =============================================================================
// Internal Helper Functions
// =============================================================================

static session_t *session_alloc(void) {
   session_t *session = calloc(1, sizeof(session_t));
   if (!session) {
      LOG_ERROR("Failed to allocate session");
      return NULL;
   }

   // Initialize mutexes
   if (pthread_mutex_init(&session->history_mutex, NULL) != 0) {
      LOG_ERROR("Failed to init history_mutex");
      free(session);
      return NULL;
   }

   if (pthread_mutex_init(&session->fd_mutex, NULL) != 0) {
      LOG_ERROR("Failed to init fd_mutex");
      pthread_mutex_destroy(&session->history_mutex);
      free(session);
      return NULL;
   }

   if (pthread_mutex_init(&session->ref_mutex, NULL) != 0) {
      LOG_ERROR("Failed to init ref_mutex");
      pthread_mutex_destroy(&session->fd_mutex);
      pthread_mutex_destroy(&session->history_mutex);
      free(session);
      return NULL;
   }

   if (pthread_cond_init(&session->ref_zero_cond, NULL) != 0) {
      LOG_ERROR("Failed to init ref_zero_cond");
      pthread_mutex_destroy(&session->ref_mutex);
      pthread_mutex_destroy(&session->fd_mutex);
      pthread_mutex_destroy(&session->history_mutex);
      free(session);
      return NULL;
   }

   if (pthread_mutex_init(&session->llm_config_mutex, NULL) != 0) {
      LOG_ERROR("Failed to init llm_config_mutex");
      pthread_cond_destroy(&session->ref_zero_cond);
      pthread_mutex_destroy(&session->ref_mutex);
      pthread_mutex_destroy(&session->fd_mutex);
      pthread_mutex_destroy(&session->history_mutex);
      free(session);
      return NULL;
   }

   if (pthread_mutex_init(&session->metrics_mutex, NULL) != 0) {
      LOG_ERROR("Failed to init metrics_mutex");
      pthread_mutex_destroy(&session->llm_config_mutex);
      pthread_cond_destroy(&session->ref_zero_cond);
      pthread_mutex_destroy(&session->ref_mutex);
      pthread_mutex_destroy(&session->fd_mutex);
      pthread_mutex_destroy(&session->history_mutex);
      free(session);
      return NULL;
   }

   if (pthread_mutex_init(&session->tools_mutex, NULL) != 0) {
      LOG_ERROR("Failed to init tools_mutex");
      pthread_mutex_destroy(&session->metrics_mutex);
      pthread_mutex_destroy(&session->llm_config_mutex);
      pthread_cond_destroy(&session->ref_zero_cond);
      pthread_mutex_destroy(&session->ref_mutex);
      pthread_mutex_destroy(&session->fd_mutex);
      pthread_mutex_destroy(&session->history_mutex);
      free(session);
      return NULL;
   }

   // Initialize active tool tracking
   session->active_tool_count = 0;

   // Initialize metrics tracker (db_id = -1 means not yet saved to DB)
   session->metrics.db_id = -1;

   // Initialize conversation history as empty JSON array
   session->conversation_history = json_object_new_array();
   if (!session->conversation_history) {
      LOG_ERROR("Failed to create conversation history array");
      pthread_mutex_destroy(&session->tools_mutex);
      pthread_mutex_destroy(&session->metrics_mutex);
      pthread_mutex_destroy(&session->llm_config_mutex);
      pthread_cond_destroy(&session->ref_zero_cond);
      pthread_mutex_destroy(&session->ref_mutex);
      pthread_mutex_destroy(&session->fd_mutex);
      pthread_mutex_destroy(&session->history_mutex);
      free(session);
      return NULL;
   }

   // Initialize LLM config with defaults from dawn.toml
   llm_get_default_config(&session->llm_config);

   return session;
}

static void session_free(session_t *session) {
   if (!session) {
      return;
   }

   // Free conversation history
   if (session->conversation_history) {
      json_object_put(session->conversation_history);
      session->conversation_history = NULL;
   }

   // Clear client_data pointer (don't free - WebSocket sessions use libwebsockets-managed memory)
   session->client_data = NULL;

   // Destroy synchronization primitives
   pthread_cond_destroy(&session->ref_zero_cond);
   pthread_mutex_destroy(&session->ref_mutex);
   pthread_mutex_destroy(&session->fd_mutex);
   pthread_mutex_destroy(&session->tools_mutex);
   pthread_mutex_destroy(&session->metrics_mutex);
   pthread_mutex_destroy(&session->llm_config_mutex);
   pthread_mutex_destroy(&session->history_mutex);

   free(session);
}

static int find_free_slot(void) {
   for (int i = 0; i < MAX_SESSIONS; i++) {
      if (sessions[i] == NULL) {
         return i;
      }
   }
   return -1;
}

static session_t *find_session_by_uuid_unlocked(const char *uuid) {
   for (int i = 0; i < MAX_SESSIONS; i++) {
      if (sessions[i] != NULL && sessions[i]->type == SESSION_TYPE_DAP2 &&
          strcmp(sessions[i]->identity.uuid, uuid) == 0) {
         return sessions[i];
      }
   }
   return NULL;
}

static session_t *find_session_by_ip_unlocked(const char *ip) {
   for (int i = 0; i < MAX_SESSIONS; i++) {
      if (sessions[i] != NULL && sessions[i]->type == SESSION_TYPE_DAP &&
          strcmp(sessions[i]->client_ip, ip) == 0) {
         return sessions[i];
      }
   }
   return NULL;
}

// =============================================================================
// Lifecycle Functions
// =============================================================================

int session_manager_init(void) {
   if (initialized) {
      LOG_WARNING("Session manager already initialized");
      return 0;
   }

   // Initialize all slots to NULL
   memset(sessions, 0, sizeof(sessions));

   // Create local session (session_id = 0)
   session_t *local = session_alloc();
   if (!local) {
      LOG_ERROR("Failed to create local session");
      return 1;
   }

   local->session_id = LOCAL_SESSION_ID;
   local->type = SESSION_TYPE_LOCAL;
   local->created_at = time(NULL);
   local->last_activity = local->created_at;
   local->client_fd = -1;
   local->ref_count = 1;  // Local session always has ref_count >= 1

   sessions[0] = local;
   initialized = true;

   LOG_INFO("Session manager initialized with local session");
   return 0;
}

void session_manager_cleanup(void) {
   if (!initialized) {
      return;
   }

   pthread_rwlock_wrlock(&session_manager_rwlock);

   for (int i = 0; i < MAX_SESSIONS; i++) {
      if (sessions[i] != NULL) {
         // Mark as disconnected to abort any in-flight operations
         sessions[i]->disconnected = true;

         // For LOCAL session, wait for ref_count to reach 1 (workers may be using it)
         // For WebSocket/DAP sessions, force cleanup (ref_count is for reconnection support)
         if (sessions[i]->type == SESSION_TYPE_LOCAL) {
            pthread_mutex_lock(&sessions[i]->ref_mutex);
            while (sessions[i]->ref_count > 1) {
               pthread_cond_wait(&sessions[i]->ref_zero_cond, &sessions[i]->ref_mutex);
            }
            pthread_mutex_unlock(&sessions[i]->ref_mutex);
         }

         LOG_INFO("Destroying session %u (type=%s)", sessions[i]->session_id,
                  session_type_name(sessions[i]->type));
         session_free(sessions[i]);
         sessions[i] = NULL;
      }
   }

   initialized = false;
   pthread_rwlock_unlock(&session_manager_rwlock);

   LOG_INFO("Session manager cleanup complete");
}

// =============================================================================
// Session Creation and Retrieval
// =============================================================================

session_t *session_create(session_type_t type, int client_fd) {
   if (!initialized) {
      LOG_ERROR("Session manager not initialized");
      return NULL;
   }

   pthread_rwlock_wrlock(&session_manager_rwlock);

   int slot = find_free_slot();
   if (slot < 0) {
      pthread_rwlock_unlock(&session_manager_rwlock);
      LOG_WARNING("Max sessions reached (%d), rejecting new client", MAX_SESSIONS);
      return NULL;
   }

   session_t *session = session_alloc();
   if (!session) {
      pthread_rwlock_unlock(&session_manager_rwlock);
      return NULL;
   }

   session->session_id = next_session_id++;
   session->type = type;
   session->created_at = time(NULL);
   session->last_activity = session->created_at;
   session->client_fd = client_fd;
   session->ref_count = 1;  // Start with ref count of 1

   sessions[slot] = session;

   pthread_rwlock_unlock(&session_manager_rwlock);

   LOG_INFO("Created session %u (type=%s, fd=%d)", session->session_id, session_type_name(type),
            client_fd);

   return session;
}

session_t *session_create_dap2(int client_fd,
                               dap2_tier_t tier,
                               const dap2_identity_t *identity,
                               const dap2_capabilities_t *capabilities) {
   if (!initialized) {
      LOG_ERROR("Session manager not initialized");
      return NULL;
   }

   if (!identity || strlen(identity->uuid) == 0) {
      LOG_ERROR("DAP2 session requires valid identity with UUID");
      return NULL;
   }

   pthread_rwlock_wrlock(&session_manager_rwlock);

   // Check for existing session with same UUID (reconnection)
   session_t *existing = find_session_by_uuid_unlocked(identity->uuid);
   if (existing && !existing->disconnected) {
      // Reconnection: update socket and clear disconnected flag
      pthread_mutex_lock(&existing->fd_mutex);
      existing->client_fd = client_fd;
      existing->disconnected = false;
      existing->last_activity = time(NULL);
      pthread_mutex_unlock(&existing->fd_mutex);

      // Increment ref count
      pthread_mutex_lock(&existing->ref_mutex);
      existing->ref_count++;
      pthread_mutex_unlock(&existing->ref_mutex);

      // Capture session_id while holding rwlock (safe access)
      uint32_t session_id = existing->session_id;

      pthread_rwlock_unlock(&session_manager_rwlock);

      LOG_INFO("DAP2 reconnection: session %u (uuid=%s, name=%s)", session_id, identity->uuid,
               identity->name);

      return existing;
   }

   // New session
   int slot = find_free_slot();
   if (slot < 0) {
      pthread_rwlock_unlock(&session_manager_rwlock);
      LOG_WARNING("Max sessions reached (%d), rejecting DAP2 client", MAX_SESSIONS);
      return NULL;
   }

   session_t *session = session_alloc();
   if (!session) {
      pthread_rwlock_unlock(&session_manager_rwlock);
      return NULL;
   }

   session->session_id = next_session_id++;
   session->type = SESSION_TYPE_DAP2;
   session->created_at = time(NULL);
   session->last_activity = session->created_at;
   session->client_fd = client_fd;
   session->ref_count = 1;

   // DAP2-specific fields
   session->tier = tier;
   memcpy(&session->identity, identity, sizeof(dap2_identity_t));
   if (capabilities) {
      memcpy(&session->capabilities, capabilities, sizeof(dap2_capabilities_t));
   }

   sessions[slot] = session;

   pthread_rwlock_unlock(&session_manager_rwlock);

   // Initialize with remote command prompt (excludes HUD/helmet commands)
   session_init_system_prompt(session, get_remote_command_prompt());

   LOG_INFO("Created DAP2 session %u (tier=%d, uuid=%s, name=%s, location=%s)", session->session_id,
            tier, identity->uuid, identity->name, identity->location);

   return session;
}

session_t *session_get_or_create_dap(int client_fd, const char *client_ip) {
   if (!initialized) {
      LOG_ERROR("Session manager not initialized");
      return NULL;
   }

   if (!client_ip || strlen(client_ip) == 0) {
      LOG_ERROR("DAP1 session requires valid client IP");
      return NULL;
   }

   pthread_rwlock_wrlock(&session_manager_rwlock);

   // Check for existing session with same IP (reconnection)
   session_t *existing = find_session_by_ip_unlocked(client_ip);
   if (existing && !existing->disconnected) {
      // Reconnection: update socket and clear disconnected flag
      pthread_mutex_lock(&existing->fd_mutex);
      existing->client_fd = client_fd;
      existing->disconnected = false;
      existing->last_activity = time(NULL);
      pthread_mutex_unlock(&existing->fd_mutex);

      // Increment ref count
      pthread_mutex_lock(&existing->ref_mutex);
      existing->ref_count++;
      pthread_mutex_unlock(&existing->ref_mutex);

      // Capture history length while holding rwlock (safe access)
      int history_len = json_object_array_length(existing->conversation_history);
      uint32_t session_id = existing->session_id;

      pthread_rwlock_unlock(&session_manager_rwlock);

      LOG_INFO("DAP1 reconnection: session %u (ip=%s, history=%d messages)", session_id, client_ip,
               history_len);

      return existing;
   }

   // New session
   int slot = find_free_slot();
   if (slot < 0) {
      pthread_rwlock_unlock(&session_manager_rwlock);
      LOG_WARNING("Max sessions reached (%d), rejecting DAP1 client", MAX_SESSIONS);
      return NULL;
   }

   session_t *session = session_alloc();
   if (!session) {
      pthread_rwlock_unlock(&session_manager_rwlock);
      return NULL;
   }

   session->session_id = next_session_id++;
   session->type = SESSION_TYPE_DAP;
   session->created_at = time(NULL);
   session->last_activity = session->created_at;
   session->client_fd = client_fd;
   session->ref_count = 1;

   // Store client IP for session persistence
   strncpy(session->client_ip, client_ip, INET_ADDRSTRLEN - 1);
   session->client_ip[INET_ADDRSTRLEN - 1] = '\0';

   sessions[slot] = session;

   pthread_rwlock_unlock(&session_manager_rwlock);

   // Initialize with remote command prompt (excludes HUD/helmet commands)
   session_init_system_prompt(session, get_remote_command_prompt());

   LOG_INFO("Created DAP1 session %u (ip=%s)", session->session_id, client_ip);

   return session;
}

session_t *session_get(uint32_t session_id) {
   if (!initialized) {
      return NULL;
   }

   pthread_rwlock_rdlock(&session_manager_rwlock);

   session_t *found = NULL;
   for (int i = 0; i < MAX_SESSIONS; i++) {
      if (sessions[i] != NULL && sessions[i]->session_id == session_id) {
         found = sessions[i];
         break;
      }
   }

   if (found) {
      // Check if session is disconnected (dying)
      if (found->disconnected) {
         pthread_rwlock_unlock(&session_manager_rwlock);
         return NULL;
      }

      // Increment ref count
      pthread_mutex_lock(&found->ref_mutex);
      found->ref_count++;
      pthread_mutex_unlock(&found->ref_mutex);
   }

   pthread_rwlock_unlock(&session_manager_rwlock);
   return found;
}

session_t *session_get_for_reconnect(uint32_t session_id) {
   if (!initialized) {
      return NULL;
   }

   pthread_rwlock_rdlock(&session_manager_rwlock);

   session_t *found = NULL;
   for (int i = 0; i < MAX_SESSIONS; i++) {
      if (sessions[i] != NULL && sessions[i]->session_id == session_id) {
         found = sessions[i];
         break;
      }
   }

   if (found) {
      // Increment ref count (even for disconnected sessions - allows reconnection)
      pthread_mutex_lock(&found->ref_mutex);
      found->ref_count++;
      pthread_mutex_unlock(&found->ref_mutex);
   }

   pthread_rwlock_unlock(&session_manager_rwlock);

   return found;
}

void session_retain(session_t *session) {
   if (!session) {
      return;
   }

   pthread_mutex_lock(&session->ref_mutex);
   session->ref_count++;
   pthread_mutex_unlock(&session->ref_mutex);
}

void session_release(session_t *session) {
   if (!session) {
      return;
   }

   pthread_mutex_lock(&session->ref_mutex);
   session->ref_count--;

   if (session->ref_count <= 0) {
      // Signal anyone waiting for ref_count to reach 0
      pthread_cond_broadcast(&session->ref_zero_cond);
   }

   pthread_mutex_unlock(&session->ref_mutex);
}

session_t *session_get_local(void) {
   if (!initialized) {
      return NULL;
   }

   // Local session is always at index 0 and never destroyed
   return sessions[0];
}

// =============================================================================
// Session Destruction
// =============================================================================

void session_destroy(uint32_t session_id) {
   if (!initialized) {
      return;
   }

   // Don't allow destroying local session
   if (session_id == LOCAL_SESSION_ID) {
      LOG_WARNING("Cannot destroy local session");
      return;
   }

   pthread_rwlock_wrlock(&session_manager_rwlock);

   // Find session
   int slot = -1;
   session_t *session = NULL;
   for (int i = 0; i < MAX_SESSIONS; i++) {
      if (sessions[i] != NULL && sessions[i]->session_id == session_id) {
         slot = i;
         session = sessions[i];
         break;
      }
   }

   if (!session) {
      pthread_rwlock_unlock(&session_manager_rwlock);
      LOG_WARNING("Session %u not found for destruction", session_id);
      return;
   }

   // Phase 1: Mark disconnected and remove from active list
   session->disconnected = true;
   sessions[slot] = NULL;

   pthread_rwlock_unlock(&session_manager_rwlock);

   // Phase 2: Wait for ref_count to reach 0
   pthread_mutex_lock(&session->ref_mutex);
   while (session->ref_count > 0) {
      LOG_INFO("Waiting for session %u ref_count (current=%d)", session_id, session->ref_count);
      pthread_cond_wait(&session->ref_zero_cond, &session->ref_mutex);
   }
   pthread_mutex_unlock(&session->ref_mutex);

   // Phase 3: Final metrics persist (updates ended_at timestamp)
   // Per-query metrics are already saved; this ensures ended_at is final.
   // Only persist if session had at least one query (db_id > 0).
   pthread_mutex_lock(&session->metrics_mutex);
   if (session->metrics.db_id > 0 && auth_db_is_ready()) {
      session_metrics_t db_metrics = { 0 };
      session_metrics_tracker_t *m = &session->metrics;

      db_metrics.id = m->db_id;
      db_metrics.session_id = session->session_id;
      db_metrics.user_id = m->user_id;
      strncpy(db_metrics.session_type, session_type_name(session->type),
              sizeof(db_metrics.session_type) - 1);
      db_metrics.started_at = session->created_at;
      db_metrics.ended_at = time(NULL);
      db_metrics.queries_total = m->queries_total;
      db_metrics.queries_cloud = m->queries_cloud;
      db_metrics.queries_local = m->queries_local;
      db_metrics.errors_count = m->errors_count;
      db_metrics.fallbacks_count = m->fallbacks_count;

      if (m->perf_sample_count > 0) {
         db_metrics.avg_asr_ms = m->asr_ms_sum / m->perf_sample_count;
         db_metrics.avg_llm_ttft_ms = m->llm_ttft_ms_sum / m->perf_sample_count;
         db_metrics.avg_llm_total_ms = m->llm_total_ms_sum / m->perf_sample_count;
         db_metrics.avg_tts_ms = m->tts_ms_sum / m->perf_sample_count;
         db_metrics.avg_pipeline_ms = m->pipeline_ms_sum / m->perf_sample_count;
      }

      auth_db_save_session_metrics(&db_metrics);
      LOG_INFO("Session %u: Final metrics saved (queries=%u, cloud=%u, local=%u)", session_id,
               m->queries_total, m->queries_cloud, m->queries_local);
   }
   pthread_mutex_unlock(&session->metrics_mutex);

   /* Trigger memory extraction for authenticated WebSocket sessions with queries */
   if (session->type == SESSION_TYPE_WEBSOCKET && session->metrics.user_id > 0 &&
       session->metrics.queries_total > 0 && g_config.memory.enabled) {
      /* Copy conversation history reference while we still have it */
      pthread_mutex_lock(&session->history_mutex);
      struct json_object *history = session->conversation_history;
      int message_count = history ? (int)json_object_array_length(history) : 0;
      int duration_seconds = (int)(time(NULL) - session->created_at);

      if (message_count > 2 && history) {
         /* Create session ID string for the summary */
         char session_id_str[32];
         snprintf(session_id_str, sizeof(session_id_str), "ws_%u", session->session_id);

         /* Trigger extraction (copies history internally)
          * Pass 0 for conversation_id - session doesn't track which DB conversation
          * it's associated with. Incremental extraction works when triggered from
          * WebUI with known conversation_id. */
         memory_trigger_extraction(session->metrics.user_id, 0, session_id_str, history,
                                   message_count, duration_seconds);
      }
      pthread_mutex_unlock(&session->history_mutex);
   }

   LOG_INFO("Destroying session %u (type=%s)", session_id, session_type_name(session->type));
   session_free(session);
}

void session_cleanup_expired(void) {
   if (!initialized) {
      return;
   }

   time_t now = time(NULL);

   pthread_rwlock_rdlock(&session_manager_rwlock);

   // Collect IDs of expired sessions (can't destroy while holding rwlock)
   uint32_t expired_ids[MAX_SESSIONS];
   int expired_count = 0;

   for (int i = 1; i < MAX_SESSIONS; i++) {  // Skip local session (i=0)
      if (sessions[i] != NULL) {
         time_t idle_time = now - sessions[i]->last_activity;
         if (idle_time > g_config.network.session_timeout_sec) {
            expired_ids[expired_count++] = sessions[i]->session_id;
         }
      }
   }

   pthread_rwlock_unlock(&session_manager_rwlock);

   // Destroy expired sessions
   for (int i = 0; i < expired_count; i++) {
      LOG_INFO("Session %u expired (idle > %d seconds)", expired_ids[i],
               g_config.network.session_timeout_sec);
      session_destroy(expired_ids[i]);
   }
}

// =============================================================================
// Conversation History
// =============================================================================

void session_add_message(session_t *session, const char *role, const char *content) {
   if (!session || !role || !content) {
      return;
   }

   pthread_mutex_lock(&session->history_mutex);

   struct json_object *message = json_object_new_object();
   if (!message) {
      pthread_mutex_unlock(&session->history_mutex);
      LOG_ERROR("Failed to create message object");
      return;
   }

   json_object_object_add(message, "role", json_object_new_string(role));
   json_object_object_add(message, "content", json_object_new_string(content));

   json_object_array_add(session->conversation_history, message);

   int count = json_object_array_length(session->conversation_history);
   LOG_INFO("Session %u: Added %s message to history (now %d messages)", session->session_id, role,
            count);

   pthread_mutex_unlock(&session->history_mutex);
}

void session_add_message_with_images(session_t *session,
                                     const char *role,
                                     const char *text,
                                     const char *const *vision_images,
                                     int vision_image_count) {
   if (!session || !role || !text) {
      return;
   }

   /* No images? Fall back to simple text message */
   if (!vision_images || vision_image_count <= 0) {
      session_add_message(session, role, text);
      return;
   }

   pthread_mutex_lock(&session->history_mutex);

   struct json_object *message = json_object_new_object();
   if (!message) {
      pthread_mutex_unlock(&session->history_mutex);
      LOG_ERROR("Failed to create message object");
      return;
   }

   json_object_object_add(message, "role", json_object_new_string(role));

   /* Build multi-part content array in OpenAI format */
   struct json_object *content = json_object_new_array();
   if (!content) {
      json_object_put(message);
      pthread_mutex_unlock(&session->history_mutex);
      LOG_ERROR("Failed to create content array");
      return;
   }

   /* Add text part first */
   struct json_object *text_part = json_object_new_object();
   json_object_object_add(text_part, "type", json_object_new_string("text"));
   json_object_object_add(text_part, "text", json_object_new_string(text));
   json_object_array_add(content, text_part);

   /* Add image parts */
   for (int i = 0; i < vision_image_count; i++) {
      if (!vision_images[i] || vision_images[i][0] == '\0') {
         continue;
      }

      struct json_object *image_part = json_object_new_object();
      json_object_object_add(image_part, "type", json_object_new_string("image_url"));

      struct json_object *image_url = json_object_new_object();

      /* Build data URI: data:image/jpeg;base64,<data> */
      size_t data_len = strlen(vision_images[i]);
      size_t uri_len = 23 + data_len + 1; /* "data:image/jpeg;base64," + data + null */
      char *data_uri = malloc(uri_len);
      if (data_uri) {
         snprintf(data_uri, uri_len, "data:image/jpeg;base64,%s", vision_images[i]);
         json_object_object_add(image_url, "url", json_object_new_string(data_uri));
         free(data_uri);
      }

      json_object_object_add(image_part, "image_url", image_url);
      json_object_array_add(content, image_part);
   }

   json_object_object_add(message, "content", content);
   json_object_array_add(session->conversation_history, message);

   LOG_INFO("Session %u: Added message with %d images to history", session->session_id,
            vision_image_count);

   pthread_mutex_unlock(&session->history_mutex);
}

struct json_object *session_get_history(session_t *session) {
   if (!session) {
      return NULL;
   }

   pthread_mutex_lock(&session->history_mutex);
   struct json_object *history = session->conversation_history;

   // Increment reference count so caller can use it
   json_object_get(history);

   pthread_mutex_unlock(&session->history_mutex);

   return history;
}

void session_clear_history(session_t *session) {
   if (!session) {
      return;
   }

   pthread_mutex_lock(&session->history_mutex);

   // Release old history
   if (session->conversation_history) {
      json_object_put(session->conversation_history);
   }

   // Create new empty array
   session->conversation_history = json_object_new_array();
   if (!session->conversation_history) {
      LOG_ERROR("Failed to create new conversation history array");
   }

   pthread_mutex_unlock(&session->history_mutex);
}

bool session_has_messages(session_t *session) {
   if (!session) {
      return false;
   }

   pthread_mutex_lock(&session->history_mutex);
   int count = session->conversation_history
                   ? (int)json_object_array_length(session->conversation_history)
                   : 0;
   pthread_mutex_unlock(&session->history_mutex);

   /* Require at least 2 messages (system prompt + user message) */
   return count >= 2;
}

void session_update_interaction_complete(session_t *session) {
   if (!session) {
      return;
   }
   session->last_interaction_complete = time(NULL);
}

int64_t session_save_voice_conversation(session_t *session) {
   if (!session) {
      return -1;
   }

   pthread_mutex_lock(&session->history_mutex);

   /* Check if there are messages to save */
   if (!session->conversation_history) {
      pthread_mutex_unlock(&session->history_mutex);
      return -1;
   }

   int msg_count = (int)json_object_array_length(session->conversation_history);
   if (msg_count < 2) {
      /* No user messages, just system prompt */
      pthread_mutex_unlock(&session->history_mutex);
      return -1;
   }

   /* Get user ID from config */
   int user_id = g_config.memory.default_voice_user_id;
   if (user_id <= 0) {
      user_id = 1; /* Fallback to admin */
   }

   /* Generate title from first user message */
   char title[128] = "Voice Conversation";
   for (int i = 0; i < msg_count; i++) {
      struct json_object *msg = json_object_array_get_idx(session->conversation_history, i);
      if (!msg)
         continue;

      struct json_object *role_obj;
      if (!json_object_object_get_ex(msg, "role", &role_obj))
         continue;

      const char *role = json_object_get_string(role_obj);
      if (role && strcmp(role, "user") == 0) {
         struct json_object *content_obj;
         if (json_object_object_get_ex(msg, "content", &content_obj)) {
            const char *content = json_object_get_string(content_obj);
            if (content && strlen(content) > 0) {
               /* Truncate to title length, add ellipsis if needed */
               size_t max_len = sizeof(title) - 4; /* Room for "..." */
               if (strlen(content) <= max_len) {
                  strncpy(title, content, sizeof(title) - 1);
               } else {
                  strncpy(title, content, max_len);
                  title[max_len] = '\0';
                  strcat(title, "...");
               }
               title[sizeof(title) - 1] = '\0';
            }
         }
         break;
      }
   }

   /* Create conversation in database with voice origin */
   int64_t conv_id = -1;
   int rc = conv_db_create_with_origin(user_id, title, "voice", &conv_id);
   if (rc != AUTH_DB_SUCCESS) {
      LOG_ERROR("Session %u: Failed to create voice conversation: %d", session->session_id, rc);
      pthread_mutex_unlock(&session->history_mutex);
      return -1;
   }

   /* Save all messages to database */
   for (int i = 0; i < msg_count; i++) {
      struct json_object *msg = json_object_array_get_idx(session->conversation_history, i);
      if (!msg)
         continue;

      struct json_object *role_obj, *content_obj;
      if (!json_object_object_get_ex(msg, "role", &role_obj))
         continue;
      if (!json_object_object_get_ex(msg, "content", &content_obj))
         continue;

      const char *role = json_object_get_string(role_obj);
      const char *content = json_object_get_string(content_obj);

      if (role && content) {
         conv_db_add_message(conv_id, user_id, role, content);
      }
   }

   /* Trigger memory extraction (async) if enabled */
   if (g_config.memory.enabled) {
      int duration_seconds = (int)(time(NULL) - session->created_at);
      char session_id_str[32];
      snprintf(session_id_str, sizeof(session_id_str), "voice_%u", session->session_id);

      /* Make a copy of history for extraction (it will be freed when we clear) */
      struct json_object *history_copy = NULL;
      const char *history_str = json_object_to_json_string(session->conversation_history);
      if (history_str) {
         history_copy = json_tokener_parse(history_str);
      }

      if (history_copy) {
         memory_trigger_extraction(user_id, conv_id, session_id_str, history_copy, msg_count,
                                   duration_seconds);
         /* Note: memory_trigger_extraction takes ownership and frees history_copy */
      }
   }

   pthread_mutex_unlock(&session->history_mutex);

   /* Clear session history and reset timestamp */
   session_clear_history(session);
   session->last_interaction_complete = 0;

   /* Re-initialize with system prompt (use local command prompt for voice sessions) */
   const char *system_prompt = get_local_command_prompt();
   if (system_prompt) {
      session_init_system_prompt(session, system_prompt);
   }

   LOG_INFO("Session %u: Saved voice conversation %lld (%d messages, user %d)", session->session_id,
            (long long)conv_id, msg_count, user_id);

   return conv_id;
}

void session_init_system_prompt(session_t *session, const char *system_prompt) {
   if (!session || !system_prompt) {
      return;
   }

   pthread_mutex_lock(&session->history_mutex);

   // Release old history
   if (session->conversation_history) {
      json_object_put(session->conversation_history);
   }

   // Create new array with system message
   session->conversation_history = json_object_new_array();
   if (!session->conversation_history) {
      LOG_ERROR("Failed to create conversation history array");
      pthread_mutex_unlock(&session->history_mutex);
      return;
   }

   // Add system message
   struct json_object *system_message = json_object_new_object();
   if (system_message) {
      json_object_object_add(system_message, "role", json_object_new_string("system"));
      json_object_object_add(system_message, "content", json_object_new_string(system_prompt));
      json_object_array_add(session->conversation_history, system_message);
   }

   pthread_mutex_unlock(&session->history_mutex);

   LOG_INFO("Session %u: Initialized with system prompt (%zu chars)", session->session_id,
            strlen(system_prompt));
}

void session_update_system_prompt(session_t *session, const char *system_prompt) {
   if (!session || !system_prompt) {
      return;
   }

   pthread_mutex_lock(&session->history_mutex);

   if (!session->conversation_history) {
      pthread_mutex_unlock(&session->history_mutex);
      return;
   }

   /* Find existing system message */
   int len = json_object_array_length(session->conversation_history);
   struct json_object *system_msg = NULL;
   int system_idx = -1;

   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(session->conversation_history, i);
      struct json_object *role_obj;
      if (json_object_object_get_ex(msg, "role", &role_obj)) {
         const char *role = json_object_get_string(role_obj);
         if (role && strcmp(role, "system") == 0) {
            system_msg = msg;
            system_idx = i;
            break;
         }
      }
   }

   if (system_msg) {
      /* Update existing system message's content */
      json_object_object_del(system_msg, "content");
      json_object_object_add(system_msg, "content", json_object_new_string(system_prompt));
      LOG_INFO("Session %u: Updated system prompt (%zu chars)", session->session_id,
               strlen(system_prompt));
   } else {
      /* No system message found - insert at beginning */
      struct json_object *new_msg = json_object_new_object();
      if (new_msg) {
         json_object_object_add(new_msg, "role", json_object_new_string("system"));
         json_object_object_add(new_msg, "content", json_object_new_string(system_prompt));
         json_object_array_put_idx(session->conversation_history, 0, new_msg);
         LOG_INFO("Session %u: Inserted system prompt at start (%zu chars)", session->session_id,
                  strlen(system_prompt));
      }
   }

   pthread_mutex_unlock(&session->history_mutex);
}

char *session_get_system_prompt(session_t *session) {
   if (!session) {
      return NULL;
   }

   char *result = NULL;

   pthread_mutex_lock(&session->history_mutex);

   if (session->conversation_history) {
      int len = json_object_array_length(session->conversation_history);
      for (int i = 0; i < len; i++) {
         struct json_object *msg = json_object_array_get_idx(session->conversation_history, i);
         struct json_object *role_obj;
         if (json_object_object_get_ex(msg, "role", &role_obj)) {
            const char *role = json_object_get_string(role_obj);
            if (role && strcmp(role, "system") == 0) {
               struct json_object *content_obj;
               if (json_object_object_get_ex(msg, "content", &content_obj)) {
                  const char *content = json_object_get_string(content_obj);
                  if (content) {
                     result = strdup(content);
                  }
               }
               break;
            }
         }
      }
   }

   pthread_mutex_unlock(&session->history_mutex);

   return result;
}

// =============================================================================
// LLM Integration
// =============================================================================

/**
 * @brief Context for LLM call preparation (reduces duplication)
 */
typedef struct {
   struct json_object *history;
   char *input_with_context;
   const char *llm_input;
   llm_resolved_config_t resolved_config;
   char model_buf[LLM_MODEL_NAME_MAX]; /* Buffer for model name (outlives stack) */
   char endpoint_buf[128];             /* Buffer for endpoint (outlives stack) */
} llm_call_ctx_t;

/**
 * @brief Prepare for LLM call - common setup for all LLM call variants
 *
 * @param skip_add_message If true, don't add user message to history (caller already did)
 * @return 0 on success, non-zero on failure (session disconnected, config error, etc.)
 */
static int llm_call_prepare(session_t *session,
                            const char *user_text,
                            llm_call_ctx_t *ctx,
                            bool skip_add_message) {
   memset(ctx, 0, sizeof(*ctx));

   // Add user message to history (unless caller already did)
   if (!skip_add_message) {
      session_add_message(session, "user", user_text);
   }

   // Update activity timestamp
   session_touch(session);

   // Get conversation history
   ctx->history = session_get_history(session);
   if (!ctx->history) {
      LOG_ERROR("Session %u: Failed to get conversation history", session->session_id);
      return 1;
   }

   // Prepare location context for DAP2 sessions
   if (session->type == SESSION_TYPE_DAP2 && strlen(session->identity.location) > 0) {
      size_t context_len = strlen(user_text) + strlen(session->identity.location) + 64;
      ctx->input_with_context = malloc(context_len);
      if (ctx->input_with_context) {
         snprintf(ctx->input_with_context, context_len, "[Location: %s] %s",
                  session->identity.location, user_text);
      }
   }
   ctx->llm_input = ctx->input_with_context ? ctx->input_with_context : user_text;

   // Check for cancellation before LLM call
   if (session->disconnected) {
      LOG_INFO("Session %u disconnected before LLM call", session->session_id);
      return 2;
   }

   // Get and resolve LLM config
   session_llm_config_t session_config = { 0 }; /* Zero-init for safety */
   session_get_llm_config(session, &session_config);

   int resolve_rc = llm_resolve_config(&session_config, &ctx->resolved_config);
   if (resolve_rc != 0) {
      LOG_ERROR("Session %u: Failed to resolve LLM config (type=%d, provider=%d)",
                session->session_id, session_config.type, session_config.cloud_provider);
      return 3;
   }

   /* Copy model/endpoint to ctx buffers (resolved pointers may point to stack) */
   if (ctx->resolved_config.model && ctx->resolved_config.model[0] != '\0') {
      strncpy(ctx->model_buf, ctx->resolved_config.model, sizeof(ctx->model_buf) - 1);
      ctx->model_buf[sizeof(ctx->model_buf) - 1] = '\0';
      ctx->resolved_config.model = ctx->model_buf;
   }
   if (ctx->resolved_config.endpoint && ctx->resolved_config.endpoint[0] != '\0') {
      strncpy(ctx->endpoint_buf, ctx->resolved_config.endpoint, sizeof(ctx->endpoint_buf) - 1);
      ctx->endpoint_buf[sizeof(ctx->endpoint_buf) - 1] = '\0';
      ctx->resolved_config.endpoint = ctx->endpoint_buf;
   }

   // Set command context for tool callbacks
   session_set_command_context(session);

   // Reset streaming filter state for WebSocket sessions
   if (session->type == SESSION_TYPE_WEBSOCKET) {
      session->cmd_tag_filter.nesting_depth = 0;
      session->stream_had_content = false;
   }

   return 0;
}

/**
 * @brief Clean up LLM call context resources
 */
static void llm_call_cleanup(llm_call_ctx_t *ctx) {
   session_set_command_context(NULL);
   if (ctx->history) {
      json_object_put(ctx->history);
   }
   if (ctx->input_with_context) {
      free(ctx->input_with_context);
   }
}

/**
 * @brief Finalize LLM call - handle WebSocket streaming end and add to history
 *
 * @return Response string on success, NULL on failure (takes ownership of response)
 */
static char *llm_call_finalize(session_t *session, char *response, llm_call_ctx_t *ctx) {
#ifdef ENABLE_WEBUI
   // End WebSocket streaming
   if (session->type == SESSION_TYPE_WEBSOCKET) {
      // Send idle state update (rate is calculated accurately by streaming layer from usage stats)
      // Only send if we had streaming activity
      if (session->stream_token_count > 0 && session->first_token_ms > 0) {
         int ttft_ms = (int)(session->first_token_ms - session->stream_start_ms);
         // Send idle state with TTFT but rate=0 (accurate rate already sent by streaming layer)
         webui_send_metrics_update(session, "idle", ttft_ms, 0.0f, -1);
      }

      if (session->llm_streaming_active) {
         webui_send_stream_end(session, response ? "complete" : "error");
      }
      // Fallback for non-streaming LLMs
      if (response && !session->stream_had_content && session->cmd_tag_filter.nesting_depth == 0) {
         LOG_INFO("Session %u: LLM didn't stream, sending full transcript", session->session_id);
         webui_send_transcript(session, "assistant", response);
      }
   }
#endif

   // Clean up context
   llm_call_cleanup(ctx);

   // Check for cancellation after LLM call
   if (session->disconnected) {
      LOG_INFO("Session %u disconnected during LLM call", session->session_id);
      free(response);
      return NULL;
   }

   if (!response) {
      LOG_ERROR("Session %u: LLM call failed", session->session_id);
      return NULL;
   }

   // Add assistant response to history (only if non-empty to avoid Claude API errors)
   if (*response) {
      session_add_message(session, "assistant", response);
   } else {
      LOG_WARNING("Session %u: LLM returned empty response, not adding to history",
                  session->session_id);
   }

   LOG_INFO("Session %u: LLM response received (%.50s%s)", session->session_id, response,
            strlen(response) > 50 ? "..." : "");

   return response;
}

char *session_llm_call(session_t *session, const char *user_text) {
   if (!session || !user_text) {
      return NULL;
   }

   if (session->disconnected) {
      LOG_INFO("Session %u disconnected, aborting LLM call", session->session_id);
      return NULL;
   }

   llm_call_ctx_t ctx;
   if (llm_call_prepare(session, user_text, &ctx, false) != 0) {
      llm_call_cleanup(&ctx);
      return NULL;
   }

   LOG_INFO("Session %u: Calling LLM with %d messages in history", session->session_id,
            json_object_array_length(ctx.history));

   /* Initialize streaming metrics before LLM call */
   session->stream_start_ms = get_time_ms();
   session->first_token_ms = 0;
   session->last_token_ms = 0;
   session->stream_token_count = 0;

   /* Set per-session cancel flag for multi-user WebUI support */
   llm_set_cancel_flag(&session->disconnected);

   char *response = llm_chat_completion_streaming_with_config(ctx.history, ctx.llm_input, NULL,
                                                              NULL, 0, session_text_chunk_callback,
                                                              session, &ctx.resolved_config);

   /* Clear per-session cancel flag */
   llm_set_cancel_flag(NULL);

   return llm_call_finalize(session, response, &ctx);
}

/**
 * @brief Combined streaming context for text display + sentence audio
 *
 * Used by session_llm_call_with_tts() to simultaneously:
 * 1. Stream text chunks to WebUI for real-time display
 * 2. Buffer chunks into sentences for TTS audio generation
 */
typedef struct {
   session_t *session;                     // Session for text streaming
   sentence_buffer_t *sentence_buffer;     // Sentence detection for TTS
   session_sentence_callback sentence_cb;  // User's sentence callback
   void *sentence_userdata;                // User's callback context
} combined_stream_ctx_t;

/**
 * @brief Internal sentence callback - forwards to user's callback
 */
static void combined_sentence_callback(const char *sentence, void *userdata) {
   combined_stream_ctx_t *ctx = (combined_stream_ctx_t *)userdata;
   if (ctx->sentence_cb) {
      ctx->sentence_cb(sentence, ctx->sentence_userdata);
   }
}

/**
 * @brief Combined chunk callback - does text streaming AND feeds sentence buffer
 *
 * Each chunk is:
 * 1. Filtered to remove <command>...</command> tags
 * 2. Sent to WebUI for real-time text display
 * 3. Fed to sentence buffer for TTS audio generation
 *
 * By filtering once and passing to both consumers, we avoid duplicate
 * command tag tracking in the sentence buffer.
 */
static void combined_chunk_callback(const char *chunk, void *userdata) {
   combined_stream_ctx_t *ctx = (combined_stream_ctx_t *)userdata;
   session_t *session = ctx->session;

   // Early exit if client disconnected (avoid unnecessary TTS/WebSocket work)
   if (!session || session->disconnected || !chunk || !chunk[0]) {
      return;
   }

   uint64_t now_ms = get_time_ms();

   /* Track timing for first token (TTFT) - based on LLM output, not filtering */
   if (session->first_token_ms == 0) {
      session->first_token_ms = now_ms;
   }

   /* Track token count and timing for rate calculation */
   session->stream_token_count++;
   session->last_token_ms = now_ms;

   /* Filter command tags ONCE for both consumers (WebUI and TTS).
    * This updates session state and returns filtered text.
    * Both consumers use the same filtered output for consistency. */
   char filtered[256];
   int len;

   if (session->cmd_tag_filter_bypass) {
      /* Native tools mode: no command tags to filter, pass through */
      size_t chunk_len = strlen(chunk);
      len = chunk_len < sizeof(filtered) - 1 ? (int)chunk_len : (int)(sizeof(filtered) - 1);
      memcpy(filtered, chunk, (size_t)len);
      filtered[len] = '\0';
   } else {
      /* Legacy mode: filter <command>...</command> tags */
      len = text_filter_command_tags_to_buffer(&session->cmd_tag_filter, chunk, filtered,
                                               sizeof(filtered));
   }

   if (len > 0) {
#ifdef ENABLE_WEBUI
      // 1. Send filtered text to WebUI for real-time display
      if (session->type == SESSION_TYPE_WEBSOCKET) {
         /* Calculate metrics */
         int ttft_ms = 0;
         float token_rate = 0.0f;

         if (session->stream_start_ms > 0) {
            ttft_ms = (int)(session->first_token_ms - session->stream_start_ms);

            uint64_t streaming_duration_ms = now_ms - session->first_token_ms;
            if (streaming_duration_ms > 0 && session->stream_token_count > 1) {
               token_rate = (float)(session->stream_token_count - 1) * 1000.0f /
                            (float)streaming_duration_ms;
            }
         }

         /* Send metrics update periodically */
         if (session->stream_token_count % 5 == 0 || session->stream_token_count == 1) {
            webui_send_metrics_update(session, "thinking", ttft_ms, token_rate, -1);
         }

         /* Send pre-filtered text (no command tags to filter) */
         webui_send_stream_delta(session, filtered);
      }
#endif

      // 2. Feed filtered text to sentence buffer for TTS
      if (ctx->sentence_buffer) {
         sentence_buffer_feed(ctx->sentence_buffer, filtered);
      }
   }
}

char *session_llm_call_with_tts(session_t *session,
                                const char *user_text,
                                session_sentence_callback sentence_cb,
                                void *userdata) {
   if (!session || !user_text) {
      return NULL;
   }

   if (session->disconnected) {
      LOG_INFO("Session %u disconnected, aborting LLM call", session->session_id);
      return NULL;
   }

   llm_call_ctx_t ctx;
   if (llm_call_prepare(session, user_text, &ctx, false) != 0) {
      llm_call_cleanup(&ctx);
      return NULL;
   }

   LOG_INFO("Session %u: Calling LLM (TTS streaming) with %d messages in history",
            session->session_id, json_object_array_length(ctx.history));

   /* Initialize streaming metrics before LLM call */
   session->stream_start_ms = get_time_ms();
   session->first_token_ms = 0;
   session->last_token_ms = 0;
   session->stream_token_count = 0;

   // Create combined streaming context for text + sentence audio
   combined_stream_ctx_t stream_ctx = { .session = session,
                                        .sentence_cb = sentence_cb,
                                        .sentence_userdata = userdata,
                                        .sentence_buffer = NULL };

   // Create sentence buffer for TTS
   stream_ctx.sentence_buffer = sentence_buffer_create(combined_sentence_callback, &stream_ctx);
   if (!stream_ctx.sentence_buffer) {
      LOG_ERROR("Session %u: Failed to create sentence buffer", session->session_id);
      llm_call_cleanup(&ctx);
      return NULL;
   }

   /* Set per-session cancel flag for multi-user WebUI support */
   llm_set_cancel_flag(&session->disconnected);

   // Call LLM with combined callback (text streaming + sentence buffering)
   char *response = llm_chat_completion_streaming_with_config(ctx.history, ctx.llm_input, NULL,
                                                              NULL, 0, combined_chunk_callback,
                                                              &stream_ctx, &ctx.resolved_config);

   /* Clear per-session cancel flag */
   llm_set_cancel_flag(NULL);

   // Flush remaining sentence and free buffer
   sentence_buffer_flush(stream_ctx.sentence_buffer);
   sentence_buffer_free(stream_ctx.sentence_buffer);

   return llm_call_finalize(session, response, &ctx);
}

char *session_llm_call_with_tts_vision_no_add(session_t *session,
                                              const char *user_text,
                                              const char **vision_images,
                                              const size_t *vision_image_sizes,
                                              const char (*vision_mimes)[24],
                                              int vision_image_count,
                                              session_sentence_callback sentence_cb,
                                              void *userdata) {
   (void)vision_mimes; /* MIME types passed to LLM layer via provider-specific handling */

   if (!session || !user_text) {
      return NULL;
   }

   if (session->disconnected) {
      LOG_INFO("Session %u disconnected, aborting LLM call", session->session_id);
      return NULL;
   }

   llm_call_ctx_t ctx;
   if (llm_call_prepare(session, user_text, &ctx, true) != 0) {
      llm_call_cleanup(&ctx);
      return NULL;
   }

   /* Log call details */
   const char *mode = sentence_cb ? (vision_image_count > 0 ? "TTS+vision" : "TTS") : "no-add";
   if (vision_image_count > 0) {
      size_t total_bytes = 0;
      for (int i = 0; i < vision_image_count; i++) {
         if (vision_image_sizes) {
            total_bytes += vision_image_sizes[i];
         }
      }
      LOG_INFO("Session %u: Calling LLM (%s) with %d messages + %d images (%zu bytes)",
               session->session_id, mode, json_object_array_length(ctx.history), vision_image_count,
               total_bytes);
   } else {
      LOG_INFO("Session %u: Calling LLM (%s) with %d messages in history", session->session_id,
               mode, json_object_array_length(ctx.history));
   }

   /* Initialize streaming metrics before LLM call */
   session->stream_start_ms = get_time_ms();
   session->first_token_ms = 0;
   session->last_token_ms = 0;
   session->stream_token_count = 0;

   /* Set per-session cancel flag for multi-user WebUI support */
   llm_set_cancel_flag(&session->disconnected);

   char *response;

   if (sentence_cb) {
      /* TTS enabled: use sentence buffering for per-sentence audio */
      combined_stream_ctx_t stream_ctx = { .session = session,
                                           .sentence_cb = sentence_cb,
                                           .sentence_userdata = userdata,
                                           .sentence_buffer = NULL };

      stream_ctx.sentence_buffer = sentence_buffer_create(combined_sentence_callback, &stream_ctx);
      if (!stream_ctx.sentence_buffer) {
         LOG_ERROR("Session %u: Failed to create sentence buffer", session->session_id);
         llm_set_cancel_flag(NULL);
         llm_call_cleanup(&ctx);
         return NULL;
      }

      response = llm_chat_completion_streaming_with_config(
          ctx.history, ctx.llm_input, vision_images, vision_image_sizes, vision_image_count,
          combined_chunk_callback, &stream_ctx, &ctx.resolved_config);

      sentence_buffer_flush(stream_ctx.sentence_buffer);
      sentence_buffer_free(stream_ctx.sentence_buffer);
   } else {
      /* No TTS: use simple text streaming callback */
      response = llm_chat_completion_streaming_with_config(
          ctx.history, ctx.llm_input, vision_images, vision_image_sizes, vision_image_count,
          session_text_chunk_callback, session, &ctx.resolved_config);
   }

   /* Clear per-session cancel flag */
   llm_set_cancel_flag(NULL);

   return llm_call_finalize(session, response, &ctx);
}

// =============================================================================
// Per-Session LLM Configuration
// =============================================================================

int session_set_llm_config(session_t *session, const session_llm_config_t *config) {
   if (!session || !config) {
      return 1;
   }

   // Validate that requested provider has API key
   if (config->type == LLM_CLOUD) {
      if (config->cloud_provider == CLOUD_PROVIDER_OPENAI && !llm_has_openai_key()) {
         LOG_WARNING("Session %u: Cannot set OpenAI provider - no API key configured",
                     session->session_id);
         return 1;
      }
      if (config->cloud_provider == CLOUD_PROVIDER_CLAUDE && !llm_has_claude_key()) {
         LOG_WARNING("Session %u: Cannot set Claude provider - no API key configured",
                     session->session_id);
         return 1;
      }
      if (config->cloud_provider == CLOUD_PROVIDER_GEMINI && !llm_has_gemini_key()) {
         LOG_WARNING("Session %u: Cannot set Gemini provider - no API key configured",
                     session->session_id);
         return 1;
      }
   }

   pthread_mutex_lock(&session->llm_config_mutex);
   memcpy(&session->llm_config, config, sizeof(session_llm_config_t));
   pthread_mutex_unlock(&session->llm_config_mutex);

   LOG_INFO("Session %u: LLM config updated (type=%d, provider=%d)", session->session_id,
            config->type, config->cloud_provider);

   return 0;
}

void session_get_llm_config(session_t *session, session_llm_config_t *config) {
   if (!session || !config) {
      return;
   }

   pthread_mutex_lock(&session->llm_config_mutex);
   memcpy(config, &session->llm_config, sizeof(session_llm_config_t));
   pthread_mutex_unlock(&session->llm_config_mutex);
}

void session_clear_llm_config(session_t *session) {
   if (!session) {
      return;
   }

   pthread_mutex_lock(&session->llm_config_mutex);
   llm_get_default_config(&session->llm_config);
   pthread_mutex_unlock(&session->llm_config_mutex);

   LOG_INFO("Session %u: LLM config reset to defaults", session->session_id);
}

// =============================================================================
// Utility Functions
// =============================================================================

void session_touch(session_t *session) {
   if (!session) {
      return;
   }
   // Protect last_activity write with history_mutex for thread safety
   // time_t may not be atomic on all platforms
   pthread_mutex_lock(&session->history_mutex);
   session->last_activity = time(NULL);
   pthread_mutex_unlock(&session->history_mutex);
}

int session_count(void) {
   if (!initialized) {
      return 0;
   }

   pthread_rwlock_rdlock(&session_manager_rwlock);

   int count = 0;
   for (int i = 0; i < MAX_SESSIONS; i++) {
      if (sessions[i] != NULL) {
         count++;
      }
   }

   pthread_rwlock_unlock(&session_manager_rwlock);

   return count;
}

const char *session_type_name(session_type_t type) {
   switch (type) {
      case SESSION_TYPE_LOCAL:
         return "LOCAL";
      case SESSION_TYPE_DAP:
         return "DAP";
      case SESSION_TYPE_DAP2:
         return "DAP2";
      case SESSION_TYPE_WEBSOCKET:
         return "WEBSOCKET";
      default:
         return "UNKNOWN";
   }
}

// =============================================================================
// Command Context (Thread-Local)
// =============================================================================

void session_set_command_context(session_t *session) {
   /* Debug logging for context transitions - helps trace command routing issues */
#ifdef DEBUG_COMMAND_CONTEXT
   if (session) {
      LOG_INFO("Command context set: session %u (%s)", session->session_id,
               session_type_name(session->type));
   } else if (tl_command_context) {
      LOG_INFO("Command context cleared (was session %u)", tl_command_context->session_id);
   }
#endif
   tl_command_context = session;
}

session_t *session_get_command_context(void) {
   return tl_command_context;
}

// =============================================================================
// Per-Session Metrics
// =============================================================================

/**
 * @brief Find or create provider entry in session metrics
 *
 * @param session Session to search
 * @param provider Provider name ("openai", "claude", "local")
 * @return Pointer to provider entry, or NULL if full
 *
 * @note Caller must hold session->metrics_mutex
 */
static session_provider_tokens_t *find_or_create_provider(session_t *session,
                                                          const char *provider) {
   session_metrics_tracker_t *m = &session->metrics;

   // Search for existing provider
   for (int i = 0; i < m->provider_count; i++) {
      if (strcmp(m->providers[i].provider, provider) == 0) {
         return &m->providers[i];
      }
   }

   // Create new entry if space available
   if (m->provider_count < SESSION_MAX_PROVIDERS) {
      session_provider_tokens_t *p = &m->providers[m->provider_count++];
      strncpy(p->provider, provider, SESSION_PROVIDER_MAX - 1);
      p->provider[SESSION_PROVIDER_MAX - 1] = '\0';
      return p;
   }

   LOG_WARNING("Session %u: Max providers (%d) reached, can't add '%s'", session->session_id,
               SESSION_MAX_PROVIDERS, provider);
   return NULL;
}

/**
 * @brief Persist session metrics to database
 *
 * Uses UPSERT pattern: INSERT on first call, UPDATE on subsequent calls.
 *
 * @param session Session with metrics to save
 *
 * @note Caller must hold session->metrics_mutex
 */
static void persist_session_metrics(session_t *session) {
   if (!auth_db_is_ready()) {
      return;
   }

   session_metrics_tracker_t *m = &session->metrics;

   // Build session_metrics_t for database
   session_metrics_t db_metrics = { 0 };
   db_metrics.id = m->db_id;  // -1 for INSERT, >0 for UPDATE
   db_metrics.session_id = session->session_id;
   db_metrics.user_id = m->user_id;
   strncpy(db_metrics.session_type, session_type_name(session->type),
           sizeof(db_metrics.session_type) - 1);
   db_metrics.started_at = session->created_at;
   db_metrics.ended_at = time(NULL);

   db_metrics.queries_total = m->queries_total;
   db_metrics.queries_cloud = m->queries_cloud;
   db_metrics.queries_local = m->queries_local;
   db_metrics.errors_count = m->errors_count;
   db_metrics.fallbacks_count = m->fallbacks_count;

   // Calculate averages from sums
   if (m->perf_sample_count > 0) {
      db_metrics.avg_asr_ms = m->asr_ms_sum / m->perf_sample_count;
      db_metrics.avg_llm_ttft_ms = m->llm_ttft_ms_sum / m->perf_sample_count;
      db_metrics.avg_llm_total_ms = m->llm_total_ms_sum / m->perf_sample_count;
      db_metrics.avg_tts_ms = m->tts_ms_sum / m->perf_sample_count;
      db_metrics.avg_pipeline_ms = m->pipeline_ms_sum / m->perf_sample_count;
   }

   // Save to database (INSERT or UPDATE based on db_metrics.id)
   if (auth_db_save_session_metrics(&db_metrics) == AUTH_DB_SUCCESS) {
      // Store returned ID for subsequent UPDATEs
      if (m->db_id < 0) {
         m->db_id = db_metrics.id;
         LOG_INFO("Session %u: Created metrics row (id=%lld)", session->session_id,
                  (long long)m->db_id);
      }

      // Save per-provider metrics (delete existing + re-insert)
      if (m->provider_count > 0 && m->db_id > 0) {
         session_provider_metrics_t providers[SESSION_MAX_PROVIDERS];
         for (int i = 0; i < m->provider_count; i++) {
            providers[i].session_metrics_id = m->db_id;
            strncpy(providers[i].provider, m->providers[i].provider, CLOUD_PROVIDER_MAX - 1);
            providers[i].provider[CLOUD_PROVIDER_MAX - 1] = '\0';
            providers[i].tokens_input = m->providers[i].tokens_input;
            providers[i].tokens_output = m->providers[i].tokens_output;
            providers[i].tokens_cached = m->providers[i].tokens_cached;
            providers[i].queries = m->providers[i].queries;
         }
         auth_db_save_provider_metrics(m->db_id, providers, m->provider_count);
      }
   }
}

void session_record_query(session_t *session,
                          const char *provider,
                          uint64_t tokens_in,
                          uint64_t tokens_out,
                          uint64_t tokens_cached,
                          double llm_ttft_ms,
                          double llm_total_ms,
                          bool is_error) {
   if (!session || !provider) {
      return;
   }

   pthread_mutex_lock(&session->metrics_mutex);

   session_metrics_tracker_t *m = &session->metrics;

   // Update query counts
   m->queries_total++;
   if (strcmp(provider, "local") == 0) {
      m->queries_local++;
   } else {
      m->queries_cloud++;
   }
   if (is_error) {
      m->errors_count++;
   }

   // Update per-provider token tracking
   session_provider_tokens_t *p = find_or_create_provider(session, provider);
   if (p) {
      p->tokens_input += tokens_in;
      p->tokens_output += tokens_out;
      p->tokens_cached += tokens_cached;
      p->queries++;
   }

   // Update LLM performance sums
   m->llm_ttft_ms_sum += llm_ttft_ms;
   m->llm_total_ms_sum += llm_total_ms;
   m->perf_sample_count++;

   // Persist to database
   persist_session_metrics(session);

   pthread_mutex_unlock(&session->metrics_mutex);
}

void session_record_asr_timing(session_t *session, double asr_ms) {
   if (!session) {
      return;
   }

   pthread_mutex_lock(&session->metrics_mutex);
   session->metrics.asr_ms_sum += asr_ms;
   pthread_mutex_unlock(&session->metrics_mutex);
}

void session_record_tts_timing(session_t *session, double tts_ms) {
   if (!session) {
      return;
   }

   pthread_mutex_lock(&session->metrics_mutex);
   session->metrics.tts_ms_sum += tts_ms;
   pthread_mutex_unlock(&session->metrics_mutex);
}

void session_record_pipeline_timing(session_t *session, double pipeline_ms) {
   if (!session) {
      return;
   }

   pthread_mutex_lock(&session->metrics_mutex);
   session->metrics.pipeline_ms_sum += pipeline_ms;
   pthread_mutex_unlock(&session->metrics_mutex);
}

void session_set_metrics_user(session_t *session, int user_id) {
   if (!session) {
      return;
   }

   pthread_mutex_lock(&session->metrics_mutex);
   session->metrics.user_id = user_id;
   pthread_mutex_unlock(&session->metrics_mutex);
}

/* Note: File-based history saving (session_manager_save_all_histories) has been removed.
 * WebUI sessions persist to auth.db (conversations/messages tables) during the session.
 * LOCAL/DAP sessions do not persist conversation history.
 * See docs/NEXT_STEPS.md Section 13 for future export functionality via dawn-admin. */
