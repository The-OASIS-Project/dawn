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

#include "config/dawn_config.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_interface.h"
#include "logging.h"

// =============================================================================
// Static Variables
// =============================================================================

static session_t *sessions[MAX_SESSIONS];
static pthread_rwlock_t session_manager_rwlock = PTHREAD_RWLOCK_INITIALIZER;
static uint32_t next_session_id = 1;  // 0 is reserved for local session
static bool initialized = false;

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

   // Initialize conversation history as empty JSON array
   session->conversation_history = json_object_new_array();
   if (!session->conversation_history) {
      LOG_ERROR("Failed to create conversation history array");
      pthread_cond_destroy(&session->ref_zero_cond);
      pthread_mutex_destroy(&session->ref_mutex);
      pthread_mutex_destroy(&session->fd_mutex);
      pthread_mutex_destroy(&session->history_mutex);
      free(session);
      return NULL;
   }

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

   // Free client-specific data if any
   if (session->client_data) {
      free(session->client_data);
      session->client_data = NULL;
   }

   // Destroy synchronization primitives
   pthread_cond_destroy(&session->ref_zero_cond);
   pthread_mutex_destroy(&session->ref_mutex);
   pthread_mutex_destroy(&session->fd_mutex);
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

         // Wait for ref_count to reach 0 (or 1 for local session)
         pthread_mutex_lock(&sessions[i]->ref_mutex);
         int target_ref = (i == 0) ? 1 : 0;  // Local session has base ref of 1
         while (sessions[i]->ref_count > target_ref) {
            pthread_cond_wait(&sessions[i]->ref_zero_cond, &sessions[i]->ref_mutex);
         }
         pthread_mutex_unlock(&sessions[i]->ref_mutex);

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

// =============================================================================
// LLM Integration
// =============================================================================

char *session_llm_call(session_t *session, const char *user_text) {
   if (!session || !user_text) {
      return NULL;
   }

   // Check for cancellation
   if (session->disconnected) {
      LOG_INFO("Session %u disconnected, aborting LLM call", session->session_id);
      return NULL;
   }

   // Add user message to history
   session_add_message(session, "user", user_text);

   // Update activity timestamp
   session_touch(session);

   // Get conversation history for LLM call
   struct json_object *history = session_get_history(session);
   if (!history) {
      LOG_ERROR("Session %u: Failed to get conversation history", session->session_id);
      return NULL;
   }

   // Prepare location context for DAP2 sessions
   char *input_with_context = NULL;
   if (session->type == SESSION_TYPE_DAP2 && strlen(session->identity.location) > 0) {
      // Prepend location context to help LLM understand where the user is
      size_t context_len = strlen(user_text) + strlen(session->identity.location) + 64;
      input_with_context = malloc(context_len);
      if (input_with_context) {
         snprintf(input_with_context, context_len, "[Location: %s] %s", session->identity.location,
                  user_text);
      }
   }

   const char *llm_input = input_with_context ? input_with_context : user_text;

   LOG_INFO("Session %u: Calling LLM with %d messages in history", session->session_id,
            json_object_array_length(history));

   // Check for cancellation before long LLM call
   if (session->disconnected) {
      LOG_INFO("Session %u disconnected before LLM call", session->session_id);
      json_object_put(history);
      if (input_with_context) {
         free(input_with_context);
      }
      return NULL;
   }

   // Call LLM (non-streaming for network clients - we need complete response for TTS)
   char *response = llm_chat_completion(history, llm_input, NULL, 0);

   // Clean up
   json_object_put(history);
   if (input_with_context) {
      free(input_with_context);
   }

   // Check for cancellation after LLM call
   if (session->disconnected) {
      LOG_INFO("Session %u disconnected during LLM call", session->session_id);
      if (response) {
         free(response);
      }
      return NULL;
   }

   if (!response) {
      LOG_ERROR("Session %u: LLM call failed", session->session_id);
      return NULL;
   }

   // Add assistant response to history
   session_add_message(session, "assistant", response);

   LOG_INFO("Session %u: LLM response received (%.50s%s)", session->session_id, response,
            strlen(response) > 50 ? "..." : "");

   return response;
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
