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
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "core/text_filter.h"   // For cmd_tag_filter_state_t
#include "llm/llm_interface.h"  // For session_llm_config_t

#define SESSION_PROVIDER_MAX 16
#define SESSION_MAX_PROVIDERS 4

/**
 * @brief Per-provider token tracking for a session
 */
typedef struct {
   char provider[SESSION_PROVIDER_MAX];  // "openai", "claude", "local"
   uint64_t tokens_input;
   uint64_t tokens_output;
   uint64_t tokens_cached;
   uint32_t queries;
} session_provider_tokens_t;

/**
 * @brief Per-session metrics tracker
 *
 * Tracks metrics during session lifetime. Saved to database after each query
 * using UPSERT pattern (INSERT first query, UPDATE thereafter).
 */
typedef struct {
   int64_t db_id;  // Database row ID (-1 = not yet saved)
   int user_id;    // User ID (0 = anonymous/local)

   // Query counts
   uint32_t queries_total;
   uint32_t queries_cloud;
   uint32_t queries_local;
   uint32_t errors_count;
   uint32_t fallbacks_count;

   // Per-provider token tracking
   session_provider_tokens_t providers[SESSION_MAX_PROVIDERS];
   int provider_count;

   // Performance tracking (sums + counts for computing averages)
   double asr_ms_sum;
   double llm_ttft_ms_sum;
   double llm_total_ms_sum;
   double tts_ms_sum;
   double pipeline_ms_sum;
   uint32_t perf_sample_count;  // Number of samples for averaging
} session_metrics_tracker_t;

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
 * Level 4: session->llm_config_mutex, session->history_mutex, session->metrics_mutex,
 *          or session->tools_mutex
 *          (per-session leaf locks, never held together - copy-under-mutex pattern)
 *
 * CRITICAL: NEVER acquire locks in reverse order
 * CRITICAL: NEVER hold session_manager_rwlock when acquiring per-session locks
 *           for extended operations (brief hold for slot operations is OK)
 * CRITICAL: NEVER hold multiple Level 4 locks simultaneously
 *
 * External locks (tts_mutex, mqtt_mutex) are leaf locks and should only be
 * acquired when no session_manager locks are held.
 */

/**
 * @brief Session type enumeration
 */
typedef enum {
   SESSION_TYPE_LOCAL,  // Local microphone
   SESSION_TYPE_DAP,    // ESP32 satellite (DAP protocol)
   SESSION_TYPE_DAP2,   // DAP 2.0 satellite (Tier 1 or Tier 2)
   SESSION_TYPE_WEBUI,  // WebUI browser client
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
   char uuid[37];              // UUID string (e.g., "550e8400-e29b-41d4-a716-446655440000")
   char name[64];              // Human-readable name (e.g., "Kitchen Assistant")
   char location[32];          // Room/area (e.g., "kitchen") - used for context
   char hardware_id[64];       // Optional hardware serial
   char reconnect_secret[65];  // 32 bytes hex-encoded (prevents session hijacking)
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
   time_t last_interaction_complete;  // Last wake-word-to-response cycle (for idle timeout)

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

   // Cancellation (atomic for cross-thread visibility on ARM64)
   atomic_bool disconnected;  // Set on client disconnect
   atomic_uint
       request_generation;  // Incremented on each new request, used to detect superseded requests

   // LLM streaming state (atomic for cross-thread visibility on ARM64)
   atomic_bool llm_streaming_active;  // True while streaming LLM response
   atomic_bool stream_had_content;    // True if any deltas were sent (for fallback)
   atomic_uint current_stream_id;     // Monotonic ID to detect stale deltas
   atomic_bool thinking_active;       // True while streaming thinking/reasoning content

   // Streaming metrics for UI visualization
   uint64_t stream_start_ms;     // Timestamp when LLM call started
   uint64_t first_token_ms;      // Timestamp of first token (0 if none yet)
   uint64_t last_token_ms;       // Timestamp of most recent token
   uint32_t stream_token_count;  // Token count for current stream
   char stream_last_char;        // Last character sent (for sentence spacing fix)

   // Per-session metrics (saved to database after each query)
   session_metrics_tracker_t metrics;
   pthread_mutex_t metrics_mutex;  // Protects metrics (lock level 4)

   // Command tag filter state (strips <command>...</command> from stream)
   // Used when native tool calling is disabled (legacy command tag mode)
   cmd_tag_filter_state_t cmd_tag_filter;  // State for text_filter_command_tags()
   bool cmd_tag_filter_bypass;             // Cached: true if native tools enabled (skip filtering)

   // Active tool tracking (for parallel tool status display)
   char active_tools[8][32];     // Tool names currently executing (max 8 parallel)
   int active_tool_count;        // Number of tools currently executing
   pthread_mutex_t tools_mutex;  // Protects active_tools (lock level 4)

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

#ifdef ENABLE_MULTI_CLIENT
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
#endif /* ENABLE_MULTI_CLIENT */

// =============================================================================
// Session Creation and Retrieval
// =============================================================================

#ifdef ENABLE_MULTI_CLIENT
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
 * @return New session, or existing session if UUID+secret matches (reconnection)
 *
 * @locks session_manager_rwlock (write) for entire operation (atomic check-and-create)
 * @note For NEW sessions: generates reconnect_secret in session->identity
 * @note For RECONNECTION (UUID matches existing session):
 *       - If identity->reconnect_secret matches existing: reclaims session
 *       - If secret is empty or doesn't match: creates NEW session (security)
 *       - On reclaim: updates client_fd, clears disconnected, returns existing
 */
session_t *session_create_dap2(int client_fd,
                               dap2_tier_t tier,
                               const dap2_identity_t *identity,
                               const dap2_capabilities_t *capabilities);

/**
 * @brief Get the reconnect secret for a session
 *
 * Returns the secret that must be included in reconnection requests.
 * Caller must free the returned string.
 *
 * @param session Session to query
 * @return Allocated secret string (caller frees), or NULL if not a DAP2 session
 */
char *session_get_reconnect_secret(session_t *session);

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
#endif /* ENABLE_MULTI_CLIENT */

// =============================================================================
// Session Destruction
// =============================================================================

#ifdef ENABLE_MULTI_CLIENT

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

/**
 * @brief Check all sessions for idle conversation timeout
 *
 * Iterates non-local sessions and saves+clears conversation history
 * for any session that has been idle longer than conversation_idle_timeout_min.
 * Called periodically from auth_maintenance thread.
 */
void session_check_idle_conversations(void);
#endif /* ENABLE_MULTI_CLIENT */

// =============================================================================
// Conversation History
// =============================================================================

#ifdef ENABLE_MULTI_CLIENT

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
 * @brief Add message with images to session's conversation history
 *
 * Creates a multi-part content message in OpenAI format:
 * { "role": "user", "content": [
 *     { "type": "text", "text": "..." },
 *     { "type": "image_url", "image_url": { "url": "data:image/jpeg;base64,..." } },
 *     ...
 * ]}
 *
 * This allows images to persist across conversation turns for follow-up questions.
 *
 * @param session Session to update
 * @param role Message role ("user" typically)
 * @param text Text content of the message
 * @param vision_images Array of base64-encoded image strings
 * @param vision_image_count Number of images in array
 *
 * @locks session->history_mutex
 * @lock_order 3
 */
void session_add_message_with_images(session_t *session,
                                     const char *role,
                                     const char *text,
                                     const char *const *vision_images,
                                     int vision_image_count);

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
 * @brief Check if session has user messages (beyond system prompt)
 *
 * @param session Session to check
 * @return true if session has at least 2 messages (system + user), false otherwise
 *
 * @locks session->history_mutex
 */
bool session_has_messages(session_t *session);

/**
 * @brief Update last interaction complete timestamp
 *
 * Called after a successful wake-word-to-response cycle completes.
 * Used for idle timeout detection in voice conversations.
 *
 * @param session Session to update
 */
void session_update_interaction_complete(session_t *session);

/**
 * @brief Save voice conversation to database and start fresh
 *
 * Called on idle timeout for Session 0 (local mic) and DAP clients.
 * Creates a new conversation in the database with origin='voice',
 * saves all messages, triggers memory extraction, then clears session history.
 *
 * @param session Session to save
 * @return Conversation ID on success, -1 on failure
 *
 * @locks session->history_mutex
 */
int64_t session_save_voice_conversation(session_t *session);

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
 * @brief Append room context to session's system prompt
 *
 * Appends "\nRoom=<room>." to the existing system prompt so the LLM knows
 * which room the voice command originates from. No-op if room is NULL or empty.
 *
 * @param session Session to update
 * @param room Room name (e.g., "kitchen", "office")
 *
 * @locks session->history_mutex (via session_get/update_system_prompt)
 */
void session_append_room_context(session_t *session, const char *room);

/**
 * @brief Update the system prompt without clearing conversation history
 *
 * Finds the existing system message in the conversation history and updates
 * its content to the new prompt. If no system message exists, creates one
 * at the beginning. Unlike session_init_system_prompt(), this preserves
 * the existing conversation.
 *
 * @param session Session to update
 * @param system_prompt New system prompt content
 *
 * @locks session->history_mutex
 */
void session_update_system_prompt(session_t *session, const char *system_prompt);

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
#endif /* ENABLE_MULTI_CLIENT */

// =============================================================================
// LLM Integration
// =============================================================================

#ifdef ENABLE_MULTI_CLIENT

/**
 * @brief Call LLM with session's conversation history
 *
 * @param session Session with conversation context
 * @param user_text User's query text
 * @return LLM response (caller must free), or NULL on error/cancel
 *
 * @note Adds user message before call, assistant response after call
 * @note Returns NULL if session->disconnected is set (cancel)
 * @note Room context is in the system prompt (not prepended to user text)
 * @note Uses session's own LLM config (copied from defaults at session creation)
 */
char *session_llm_call(session_t *session, const char *user_text);

/**
 * @brief Sentence callback for TTS streaming
 *
 * Called for each complete sentence detected in the LLM response.
 * Use this to generate and send audio sentence-by-sentence.
 *
 * @param sentence Complete sentence text
 * @param userdata User-provided context pointer
 */
typedef void (*session_sentence_callback)(const char *sentence, void *userdata);

/**
 * @brief Call LLM with sentence-by-sentence TTS streaming
 *
 * Like session_llm_call(), but uses sentence buffering to call the provided
 * callback for each complete sentence. This enables real-time audio streaming
 * where TTS is generated and sent per-sentence rather than waiting for the
 * full response.
 *
 * @param session Session with conversation context
 * @param user_text User's query text
 * @param sentence_cb Callback for each complete sentence (for TTS)
 * @param userdata Context passed to sentence callback
 * @return LLM response (caller must free), or NULL on error/cancel
 *
 * @note Does NOT do WebSocket text streaming (use for audio-input sessions)
 * @note Adds user message before call, assistant response after call
 */
char *session_llm_call_with_tts(session_t *session,
                                const char *user_text,
                                session_sentence_callback sentence_cb,
                                void *userdata);

/**
 * @brief Unified LLM call with optional TTS and vision, without adding user message
 *
 * Flexible LLM call that supports:
 * - Optional vision images (pass NULL/0 for text-only)
 * - Optional TTS sentence streaming (pass NULL for no TTS)
 *
 * Use when caller has already added the message before the call.
 * This ensures message is in history even if the call is cancelled.
 *
 * @param session Session context
 * @param user_text User input text
 * @param vision_images Array of base64 encoded image data (NULL for text-only)
 * @param vision_image_sizes Array of image sizes (NULL for text-only)
 * @param vision_mimes Array of MIME type strings (NULL for text-only)
 * @param vision_image_count Number of images (0 for text-only)
 * @param sentence_cb Callback for each complete sentence (NULL to disable TTS)
 * @param userdata Context passed to sentence callback
 * @return LLM response (caller must free), or NULL on failure
 */
char *session_llm_call_with_tts_vision_no_add(session_t *session,
                                              const char *user_text,
                                              const char **vision_images,
                                              const size_t *vision_image_sizes,
                                              const char (*vision_mimes)[24],
                                              int vision_image_count,
                                              session_sentence_callback sentence_cb,
                                              void *userdata);
#endif /* ENABLE_MULTI_CLIENT */

// =============================================================================
// Per-Session LLM Configuration
// =============================================================================

#ifdef ENABLE_MULTI_CLIENT

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
#endif /* ENABLE_MULTI_CLIENT */

// =============================================================================
// Per-Session Metrics
// =============================================================================

#ifdef ENABLE_MULTI_CLIENT

/**
 * @brief Record a completed LLM query in session metrics
 *
 * Updates session metrics with query counts, token usage, and performance data.
 * Automatically persists to database using UPSERT pattern (INSERT on first call,
 * UPDATE on subsequent calls).
 *
 * @param session Session to update
 * @param provider LLM provider used ("openai", "claude", "local")
 * @param tokens_in Input tokens consumed
 * @param tokens_out Output tokens generated
 * @param tokens_cached Cached tokens (prompt caching)
 * @param llm_ttft_ms Time to first token in milliseconds
 * @param llm_total_ms Total LLM response time in milliseconds
 * @param is_error Whether the query resulted in an error
 *
 * @locks session->metrics_mutex
 */
void session_record_query(session_t *session,
                          const char *provider,
                          uint64_t tokens_in,
                          uint64_t tokens_out,
                          uint64_t tokens_cached,
                          double llm_ttft_ms,
                          double llm_total_ms,
                          bool is_error);

/**
 * @brief Record ASR timing for session metrics
 *
 * @param session Session to update
 * @param asr_ms ASR processing time in milliseconds
 *
 * @locks session->metrics_mutex
 */
void session_record_asr_timing(session_t *session, double asr_ms);

/**
 * @brief Record TTS timing for session metrics
 *
 * @param session Session to update
 * @param tts_ms TTS processing time in milliseconds
 *
 * @locks session->metrics_mutex
 */
void session_record_tts_timing(session_t *session, double tts_ms);

/**
 * @brief Record full pipeline timing for session metrics
 *
 * @param session Session to update
 * @param pipeline_ms Total pipeline time (ASR + LLM + TTS) in milliseconds
 *
 * @locks session->metrics_mutex
 */
void session_record_pipeline_timing(session_t *session, double pipeline_ms);

/**
 * @brief Set user ID for session metrics
 *
 * Called when WebSocket session is authenticated to associate metrics with user.
 *
 * @param session Session to update
 * @param user_id Authenticated user ID
 *
 * @locks session->metrics_mutex
 */
void session_set_metrics_user(session_t *session, int user_id);

#endif /* ENABLE_MULTI_CLIENT */

// =============================================================================
// Utility Functions
// =============================================================================

#ifdef ENABLE_MULTI_CLIENT

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

#endif /* ENABLE_MULTI_CLIENT */

// =============================================================================
// Command Context (Thread-Local)
// =============================================================================

#ifdef ENABLE_MULTI_CLIENT

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
#endif /* ENABLE_MULTI_CLIENT */

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

#ifdef ENABLE_MULTI_CLIENT
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
#else
/* Local-only mode: scope guard is a no-op */
static inline void session_command_context_cleanup(session_t **ctx) {
   (void)ctx;
}
#define SESSION_SCOPED_COMMAND_CONTEXT(session)                                       \
   session_t *_scoped_ctx_##__LINE__                                                  \
       __attribute__((cleanup(session_command_context_cleanup), unused)) = (session); \
   (void)_scoped_ctx_##__LINE__
#endif /* ENABLE_MULTI_CLIENT */

/* =============================================================================
 * Stub Implementations for Local-Only Mode (no network features)
 *
 * When both ENABLE_DAP and ENABLE_WEBUI are disabled, session_manager.c
 * is not compiled. These inline stubs provide the minimal API needed by
 * code that calls session functions unconditionally.
 * ============================================================================= */

#ifndef ENABLE_MULTI_CLIENT

/* Stub: No sessions in local-only mode */
static inline session_t *session_get_command_context(void) {
   return NULL;
}

static inline void session_set_command_context(session_t *session) {
   (void)session;
}

static inline session_t *session_get(uint32_t session_id) {
   (void)session_id;
   return NULL;
}

/**
 * @brief Get local session for local-only mode (lazy initialization)
 *
 * Creates a static session with conversation history on first call.
 * This allows local-only builds to maintain conversation context.
 *
 * @return Pointer to static local session (never NULL after first call)
 */
static inline session_t *session_get_local(void) {
   static session_t local_stub = { 0 };
   static bool initialized = false;

   if (!initialized) {
      local_stub.session_id = LOCAL_SESSION_ID;
      local_stub.type = SESSION_TYPE_LOCAL;
      local_stub.client_fd = -1;
      local_stub.conversation_history = json_object_new_array();
      pthread_mutex_init(&local_stub.history_mutex, NULL);
      pthread_mutex_init(&local_stub.llm_config_mutex, NULL);
      llm_get_default_config(&local_stub.llm_config);
      initialized = true;
   }
   return &local_stub;
}

static inline int session_manager_init(void) {
   return 0;
}

static inline void session_manager_cleanup(void) {
}

static inline void session_cleanup_expired(void) {
}

static inline void session_check_idle_conversations(void) {
}

static inline int session_count(void) {
   return 0;
}

static inline int session_set_llm_config(session_t *session, const session_llm_config_t *config) {
   (void)session;
   (void)config;
   return 1; /* Not supported in local-only mode */
}

static inline void session_get_llm_config(session_t *session, session_llm_config_t *config) {
   (void)session;
   /* In local-only mode, use global defaults */
   if (config) {
      llm_get_default_config(config);
   }
}

/**
 * @brief Initialize session with system prompt (local-only mode)
 *
 * Clears existing history and adds the system message.
 * Provides conversation context for LLM in local-only builds.
 */
static inline void session_init_system_prompt(session_t *session, const char *system_prompt) {
   if (!session || !session->conversation_history || !system_prompt)
      return;

   /* Clear existing messages */
   size_t len = json_object_array_length(session->conversation_history);
   for (size_t i = len; i > 0; i--) {
      json_object_array_del_idx(session->conversation_history, i - 1, 1);
   }

   /* Add system message */
   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "role", json_object_new_string("system"));
   json_object_object_add(msg, "content", json_object_new_string(system_prompt));
   json_object_array_add(session->conversation_history, msg);
}

static inline void session_release(session_t *session) {
   (void)session;
}

static inline void session_retain(session_t *session) {
   (void)session;
}

#endif /* !ENABLE_MULTI_CLIENT */

#ifdef __cplusplus
}
#endif

#endif  // SESSION_MANAGER_H
