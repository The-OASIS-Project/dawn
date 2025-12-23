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
 * Session manager for multi-client support.
 * Manages per-client conversation context with reference counting.
 */

#ifndef SESSION_MANAGER_H
#define SESSION_MANAGER_H

#include <arpa/inet.h>
#include <json-c/json.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "llm/llm_interface.h"  // For session_llm_config_t

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SESSIONS 8
#define SESSION_TIMEOUT_SEC 1800  // 30 minute idle timeout
#define LOCAL_SESSION_ID 0        // Reserved for local microphone

/**
 * LOCK ACQUISITION ORDER (to prevent deadlocks):
 *
 * Level 1: session_manager_rwlock (module-level, read or write)
 * Level 2: session->ref_mutex (per-session, protects ref_count)
 * Level 3: session->fd_mutex (per-session, protects client_fd during reconnect)
 * Level 4: session->llm_config_mutex OR session->history_mutex
 *          (per-session, never held together - copy-under-mutex pattern)
 *
 * CRITICAL: NEVER acquire locks in reverse order
 * CRITICAL: NEVER hold session_manager_rwlock when acquiring per-session locks
 *           for extended operations (brief hold for slot operations is OK)
 *
 * External locks (tts_mutex, mqtt_mutex) are leaf locks and should only be
 * acquired when no session_manager locks are held.
 */

/**
 * @brief Session type enumeration
 */
typedef enum {
   SESSION_TYPE_LOCAL,      // Local microphone
   SESSION_TYPE_DAP,        // ESP32 satellite (DAP protocol)
   SESSION_TYPE_DAP2,       // DAP 2.0 satellite (Tier 1 or Tier 2)
   SESSION_TYPE_WEBSOCKET,  // WebUI client
} session_type_t;

/**
 * @brief DAP2 satellite tier (see DAP2_DESIGN.md Section 4)
 */
typedef enum {
   DAP2_TIER_1 = 1,  // Full satellite (RPi) - sends TEXT, receives TEXT
   DAP2_TIER_2 = 2,  // Audio satellite (ESP32) - sends ADPCM, receives ADPCM
} dap2_tier_t;

/**
 * @brief DAP2 satellite identity (from REGISTER message)
 */
typedef struct {
   char uuid[37];         // UUID string (e.g., "550e8400-e29b-41d4-a716-446655440000")
   char name[64];         // Human-readable name (e.g., "Kitchen Assistant")
   char location[32];     // Room/area (e.g., "kitchen") - used for context
   char hardware_id[64];  // Optional hardware serial
} dap2_identity_t;

/**
 * @brief DAP2 satellite capabilities
 */
typedef struct {
   bool local_asr;  // Satellite can transcribe locally
   bool local_tts;  // Satellite can synthesize locally
   bool wake_word;  // Satellite has wake word detection
} dap2_capabilities_t;

/**
 * @brief Session structure
 * @ownership Session manager owns all sessions
 * @thread_safety Protected by session_manager_rwlock
 */
typedef struct session {
   uint32_t session_id;
   session_type_t type;
   time_t created_at;
   time_t last_activity;

   // Conversation history (owned by session, protected by history_mutex)
   struct json_object *conversation_history;
   pthread_mutex_t history_mutex;

   // Client-specific data
   int client_fd;                    // Socket for network clients (-1 for local)
   pthread_mutex_t fd_mutex;         // Protects client_fd during reconnection
   void *client_data;                // Type-specific data (WebSocket state, etc.)
   char client_ip[INET_ADDRSTRLEN];  // Client IP for DAP1 session persistence

   // DAP2-specific fields (only valid when type == SESSION_TYPE_DAP2)
   dap2_tier_t tier;                  // Tier 1 (text) or Tier 2 (audio)
   dap2_identity_t identity;          // UUID, name, location
   dap2_capabilities_t capabilities;  // Local ASR/TTS/wake word

   // Cancellation
   volatile bool disconnected;  // Set on client disconnect

   // Reference counting for safe access (two-phase destruction pattern)
   int ref_count;
   pthread_mutex_t ref_mutex;
   pthread_cond_t ref_zero_cond;  // Signaled when ref_count reaches 0

   // Per-session LLM configuration (allows different LLM for each client)
   session_llm_config_t llm_config;
   pthread_mutex_t llm_config_mutex;  // Protects llm_config (lock level 4)
} session_t;

// =============================================================================
// Lifecycle Functions
// =============================================================================

/**
 * @brief Initialize the session manager
 *
 * Creates the rwlock and initializes the local session (session_id = 0).
 * Must be called before any other session_* functions.
 *
 * @return 0 on success, 1 on failure
 */
int session_manager_init(void);

/**
 * @brief Cleanup the session manager
 *
 * Destroys all active sessions and frees resources.
 * Should be called during application shutdown.
 */
void session_manager_cleanup(void);

// =============================================================================
// Session Creation and Retrieval
// =============================================================================

/**
 * @brief Create new session
 *
 * @param type Session type (LOCAL, DAP, DAP2, WEBSOCKET)
 * @param client_fd Client socket (-1 for local session)
 * @return New session pointer, or NULL if max sessions reached
 *
 * @locks session_manager_rwlock (write)
 * @lock_order 1
 */
session_t *session_create(session_type_t type, int client_fd);

/**
 * @brief Create new DAP2 session with identity (from REGISTER message)
 *
 * @param client_fd Client socket
 * @param tier DAP2_TIER_1 (text) or DAP2_TIER_2 (audio)
 * @param identity Satellite identity (UUID, name, location)
 * @param capabilities Satellite capabilities (local ASR/TTS/wake word)
 * @return New session, or existing session if UUID matches (reconnection)
 *
 * @locks session_manager_rwlock (write) for entire operation (atomic check-and-create)
 * @note If UUID matches an existing session:
 *       1. Acquires session->fd_mutex
 *       2. Updates client_fd (old socket is invalid after reconnect)
 *       3. Clears disconnected flag
 *       4. Releases fd_mutex
 *       5. Returns existing session with preserved conversation history
 */
session_t *session_create_dap2(int client_fd,
                               dap2_tier_t tier,
                               const dap2_identity_t *identity,
                               const dap2_capabilities_t *capabilities);

/**
 * @brief Get or create DAP1 session by client IP (legacy protocol)
 *
 * DAP1 clients don't send a REGISTER message with UUID, so we use IP address
 * as a simple session identifier for testing purposes. This allows conversation
 * history to persist across reconnections from the same IP.
 *
 * @param client_fd Client socket
 * @param client_ip Client IP address string
 * @return Existing session if IP matches, or new session if not found
 *
 * @locks session_manager_rwlock (write) for entire operation
 * @note If IP matches an existing DAP session:
 *       1. Updates client_fd
 *       2. Clears disconnected flag
 *       3. Increments ref_count
 *       4. Returns existing session with preserved conversation history
 * @note Caller MUST call session_release() when done
 * @warning This is a temporary solution for DAP1 testing. DAP2 uses proper UUIDs.
 */
session_t *session_get_or_create_dap(int client_fd, const char *client_ip);

/**
 * @brief Get session by ID (increments ref_count while holding rwlock)
 *
 * @param session_id Session ID to look up
 * @return Session pointer (caller must call session_release), or NULL if not found
 *
 * @locks session_manager_rwlock (read), then session->ref_mutex (brief)
 * @lock_order Acquires rwlock, increments ref, releases rwlock BEFORE caller uses session
 * @note Caller MUST call session_release() when done
 * @note Returns NULL for disconnected sessions (prevents new refs to dying sessions)
 */
session_t *session_get(uint32_t session_id);

/**
 * @brief Get session by ID for reconnection (allows disconnected sessions)
 *
 * Similar to session_get() but returns disconnected sessions to allow
 * WebSocket clients to reconnect with their existing conversation history.
 *
 * @param session_id Session ID to look up
 * @return Session pointer (caller must call session_release), or NULL if not found
 *
 * @note Caller should clear the disconnected flag after reconnecting
 */
session_t *session_get_for_reconnect(uint32_t session_id);

/**
 * @brief Retain session reference (increments ref_count)
 *
 * Use this when passing a session pointer to another thread or storing
 * it for later use. Prevents session destruction while reference is held.
 *
 * @param session Session to retain
 * @note Caller MUST call session_release() when done
 */
void session_retain(session_t *session);

/**
 * @brief Release session reference (decrements ref_count)
 *
 * @param session Session to release
 * @note Signals ref_zero_cond when ref_count reaches 0
 */
void session_release(session_t *session);

/**
 * @brief Get local session (always exists)
 *
 * @return Local session pointer (session_id = 0)
 * @note Does NOT increment ref_count (local session is never destroyed)
 */
session_t *session_get_local(void);

// =============================================================================
// Session Destruction
// =============================================================================

/**
 * @brief Mark session as disconnected and destroy when ref_count=0
 *
 * @param session_id Session ID to destroy
 *
 * @note Two-phase destruction:
 *       1. Mark disconnected + remove from active list (prevents new refs)
 *       2. Wait for ref_count=0 via ref_zero_cond, then free
 */
void session_destroy(uint32_t session_id);

/**
 * @brief Cleanup expired sessions (called periodically)
 *
 * Iterates through all sessions and destroys those that have exceeded
 * SESSION_TIMEOUT_SEC without activity.
 */
void session_cleanup_expired(void);

// =============================================================================
// Conversation History
// =============================================================================

/**
 * @brief Add message to session's conversation history
 *
 * @param session Session to update
 * @param role Message role ("user", "assistant", "system")
 * @param content Message content
 *
 * @locks session->history_mutex
 * @lock_order 3
 */
void session_add_message(session_t *session, const char *role, const char *content);

/**
 * @brief Get conversation history JSON (for LLM API calls)
 *
 * @param session Session to query
 * @return JSON array of messages (caller must json_object_put when done)
 *
 * @locks session->history_mutex
 * @note Returns a reference to the internal array, not a copy
 */
struct json_object *session_get_history(session_t *session);

/**
 * @brief Clear conversation history
 *
 * @param session Session to clear
 *
 * @locks session->history_mutex
 */
void session_clear_history(session_t *session);

/**
 * @brief Initialize session with system prompt
 *
 * Clears any existing history and adds the system message.
 * Used for session initialization and conversation reset.
 *
 * @param session Session to initialize
 * @param system_prompt System prompt content (AI description/personality)
 *
 * @locks session->history_mutex
 */
void session_init_system_prompt(session_t *session, const char *system_prompt);

/**
 * @brief Get the system prompt from a session
 *
 * Returns the content of the first "system" role message in the conversation
 * history. Useful for debugging to see what instructions the LLM received.
 *
 * @param session Session to query
 * @return System prompt string, or NULL if not found. Caller must free().
 *
 * @locks session->history_mutex
 */
char *session_get_system_prompt(session_t *session);

// =============================================================================
// LLM Integration
// =============================================================================

/**
 * @brief Call LLM with session's conversation history
 *
 * @param session Session with conversation context
 * @param user_text User's query text
 * @return LLM response (caller must free), or NULL on error/cancel
 *
 * @note Adds user message before call, assistant response after call
 * @note Returns NULL if session->disconnected is set (cancel)
 * @note Prepends location context if session->identity.location is set
 * @note Uses session's own LLM config (copied from defaults at session creation)
 */
char *session_llm_call(session_t *session, const char *user_text);

// =============================================================================
// Per-Session LLM Configuration
// =============================================================================

/**
 * @brief Set per-session LLM configuration
 *
 * Validates that requested provider has API key before applying.
 *
 * @param session Session to configure
 * @param config LLM configuration to apply
 * @return 0 on success, 1 if requested provider lacks API key
 *
 * @locks session->llm_config_mutex
 */
int session_set_llm_config(session_t *session, const session_llm_config_t *config);

/**
 * @brief Get session's current LLM configuration
 *
 * @param session Session to query
 * @param config Output: copy of session's LLM config
 *
 * @locks session->llm_config_mutex
 */
void session_get_llm_config(session_t *session, session_llm_config_t *config);

/**
 * @brief Reset session LLM config to defaults from dawn.toml
 *
 * Resets session to use default settings from configuration file.
 * Changes only affect this session, not others.
 *
 * @param session Session to reset
 *
 * @locks session->llm_config_mutex
 */
void session_clear_llm_config(session_t *session);

// =============================================================================
// Utility Functions
// =============================================================================

/**
 * @brief Update session's last activity timestamp
 *
 * @param session Session to update
 */
void session_touch(session_t *session);

/**
 * @brief Get session count (for metrics)
 *
 * @return Number of active sessions
 */
int session_count(void);

/**
 * @brief Get session type name as string
 *
 * @param type Session type
 * @return Human-readable type name
 */
const char *session_type_name(session_type_t type);

/**
 * @brief Save all active sessions' conversation histories
 *
 * Iterates through all sessions and saves non-empty conversation histories
 * to timestamped JSON files. Called during shutdown to preserve chat logs.
 * Files are named: chat_history_session{id}_{type}_{timestamp}.json
 *
 * @note Thread-safe: acquires session_manager_rwlock (read)
 */
void session_manager_save_all_histories(void);

// =============================================================================
// Command Context (Thread-Local)
// =============================================================================

/**
 * @brief Set the current command context session for this thread
 *
 * Call this before processing commands to establish which session's
 * LLM config should be used by device callbacks (e.g., LLM switch commands).
 * The context is thread-local and should be cleared after command processing.
 *
 * @param session Session to use for command context (NULL to clear)
 */
void session_set_command_context(session_t *session);

/**
 * @brief Get the current command context session for this thread
 *
 * Used by device callbacks (e.g., localLLMCallback) to get the session
 * whose LLM config should be modified by the command.
 *
 * @return Current command context session, or NULL if not set
 */
session_t *session_get_command_context(void);

/* =============================================================================
 * Command Context Scope Guard (GCC/Clang cleanup attribute)
 *
 * Provides automatic cleanup of command context when a scope exits, even on
 * early returns. Uses __attribute__((cleanup)) for RAII-style safety in C.
 *
 * Usage:
 *   {
 *      SESSION_SCOPED_COMMAND_CONTEXT(my_session);
 *      // ... do work with command context set ...
 *      // Context automatically cleared when scope exits
 *   }
 * ============================================================================= */

/**
 * @brief Cleanup function for scope guard - clears command context
 * @param ctx Pointer to session pointer (unused, just for cleanup signature)
 */
static inline void session_command_context_cleanup(session_t **ctx) {
   (void)ctx;
   session_set_command_context(NULL);
}

/**
 * @brief Scope guard macro for command context
 *
 * Sets the command context and ensures it's cleared when the current scope exits.
 * This prevents context leaks on early returns or exceptions.
 *
 * @param session Session to use for command context
 */
#define SESSION_SCOPED_COMMAND_CONTEXT(session)                                       \
   session_t *_scoped_ctx_##__LINE__                                                  \
       __attribute__((cleanup(session_command_context_cleanup), unused)) = (session); \
   session_set_command_context(session)

#ifdef __cplusplus
}
#endif

#endif  // SESSION_MANAGER_H
