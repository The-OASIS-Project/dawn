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
 * WebUI Internal Header - Shared state and helpers for webui_*.c modules
 *
 * This header is NOT part of the public API. It exposes internal state
 * and helper functions shared between webui_server.c and split handler
 * modules (webui_http.c, webui_admin.c, webui_history.c, etc.).
 *
 * All modules including this header MUST be compiled into the same binary.
 * Do not expose this header to external code.
 */

#ifndef WEBUI_INTERNAL_H
#define WEBUI_INTERNAL_H

#include <json-c/json.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "auth/auth_db.h"
#include "core/session_manager.h"
#include "webui/webui_server.h"

/* =============================================================================
 * Request Supersession Macro (used by worker threads)
 * ============================================================================= */

/**
 * @brief Check if a request has been superseded by a newer one
 *
 * A request is superseded if:
 * 1. The session was disconnected (user closed connection or clicked stop)
 * 2. A newer request was initiated (user sent new message before old completed)
 *
 * Workers should check this before and after long operations (LLM calls, etc.)
 * to avoid processing stale requests.
 *
 * @param session Pointer to session_t
 * @param expected_gen The request_generation captured when work was queued
 * @return true if request should be aborted, false if still valid
 */
#define REQUEST_SUPERSEDED(session, expected_gen) \
   (atomic_load(&(session)->disconnected) ||      \
    atomic_load(&(session)->request_generation) != (expected_gen))

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Internal Constants
 * ============================================================================= */

#define WS_SEND_BUFFER_SIZE 16384
#define HTTP_MAX_POST_BODY 4096
#define AUTH_COOKIE_NAME "dawn_session"
#define AUTH_COOKIE_MAX_AGE (24 * 60 * 60) /* 24 hours */
#define MAX_TOKEN_MAPPINGS 16
#define MODEL_CACHE_TTL 60 /* Cache refresh interval in seconds */

/* WebSocket text buffer limits */
#define WEBUI_TEXT_BUFFER_INITIAL_CAP 8192
#define WEBUI_TEXT_BUFFER_MAX_CAP \
   (8 * 1024 * 1024) /* 8MB for vision (4MB image + base64 overhead) */

/* =============================================================================
 * Per-WebSocket Connection Data
 * ============================================================================= */

typedef struct {
   struct lws *wsi;                             /* libwebsockets handle */
   session_t *session;                          /* Session manager reference */
   char session_token[WEBUI_SESSION_TOKEN_LEN]; /* Reconnection token */
   uint8_t *audio_buffer;                       /* Opus audio accumulation */
   size_t audio_buffer_len;
   size_t audio_buffer_capacity;
   bool in_binary_fragment; /* True if receiving fragmented binary frame */
   uint8_t binary_msg_type; /* Message type from first fragment */
   bool use_opus;           /* True if client supports Opus codec */
   bool tts_enabled;        /* True if TTS output enabled for this connection */

   /* Text message fragmentation support (for large JSON payloads) */
   char *text_buffer;      /* Accumulation buffer for fragmented text messages */
   size_t text_buffer_len; /* Current data length in text_buffer */
   size_t text_buffer_cap; /* Allocated capacity of text_buffer */

   /* Auth state (populated at WebSocket establishment from HTTP cookie) */
   bool authenticated;
   int auth_user_id;
   char auth_session_token[AUTH_TOKEN_LEN]; /* For DB re-validation */
   char username[AUTH_USERNAME_MAX];
   /* Note: is_admin NOT cached - re-validated from DB on each admin operation */

   /* Client IP address (captured at connection establishment for reliable logging) */
   char client_ip[64];

   /* Active conversation tracking (for memory extraction on switch) */
   int64_t active_conversation_id;
   bool active_conversation_private; /* If true, skip memory extraction */

   /* Music streaming state (per-session, owned by webui_music.c) */
   void *music_state; /* session_music_state_t*, NULL if not initialized */
} ws_connection_t;

/* =============================================================================
 * HTTP Session Data
 * ============================================================================= */

/* Forward declaration for image session */
struct http_image_session;

struct http_session_data {
   char path[256]; /* Request path */
   char post_body[HTTP_MAX_POST_BODY];
   size_t post_body_len;
   bool is_post;
   struct http_image_session *image_session; /* For image uploads (NULL if not image request) */
};

/* =============================================================================
 * Response Queue (worker -> WebUI thread)
 * ============================================================================= */

typedef struct {
   session_t *session;
   ws_response_type_t type;
   union {
      struct {
         char *state;
         char *detail;     /* Optional detail message */
         char *tools_json; /* Optional JSON array of active tools */
      } state;
      struct {
         char *role;
         char *text;
      } transcript;
      struct {
         char *code;
         char *message;
      } error;
      struct {
         char *token;
      } session_token;
      struct {
         uint8_t *data;
         size_t len;
      } audio;
      struct {
         int current_tokens;
         int max_tokens;
         float threshold;
      } context;
      struct {
         uint32_t stream_id;
         char text[1024]; /* Buffer for delta/end text (increased for thinking) */
      } stream;
      struct {
         char state[16];   /* idle, listening, thinking, speaking, error */
         int ttft_ms;      /* Time to first token (ms) */
         float token_rate; /* Tokens per second */
         int context_pct;  /* Context utilization 0-100 */
      } metrics;
      struct {
         int tokens_before;
         int tokens_after;
         int messages_summarized;
         char *summary;
      } compaction;
      struct {
         double position_sec;
         uint32_t duration_sec;
      } music_position;
   };
} ws_response_t;

/* =============================================================================
 * Token-to-Session Mapping
 * ============================================================================= */

typedef struct {
   char token[WEBUI_SESSION_TOKEN_LEN];
   uint32_t session_id;
   time_t created;
   bool in_use;
} token_mapping_t;

/* =============================================================================
 * Discovery Cache (for model/interface scanning)
 * ============================================================================= */

typedef struct {
   struct json_object *models_response;     /* Cached list_models_response */
   struct json_object *interfaces_response; /* Cached list_interfaces_response */
   time_t models_cache_time;                /* When models were last scanned */
   time_t interfaces_cache_time;            /* When interfaces were last enumerated */
   pthread_mutex_t cache_mutex;             /* Protects cache access */
} discovery_cache_t;

/* =============================================================================
 * Extern Declarations for Module State (defined in webui_server.c)
 * ============================================================================= */

extern struct lws_context *s_lws_context;
extern volatile int s_running;
extern volatile int s_client_count;
extern int s_port;
extern char s_www_path[256];
extern pthread_mutex_t s_mutex;
extern pthread_rwlock_t s_config_rwlock;

/* Response queue */
extern ws_response_t s_response_queue[WEBUI_RESPONSE_QUEUE_SIZE];
extern int s_queue_head;
extern int s_queue_tail;
extern pthread_mutex_t s_queue_mutex;

/* Token mapping */
extern token_mapping_t s_token_map[MAX_TOKEN_MAPPINGS];
extern pthread_mutex_t s_token_mutex;

/* Discovery cache and allowed path prefixes are module-local in webui_config.c */

/* =============================================================================
 * Response Queue Functions
 * ============================================================================= */

/**
 * @brief Queue a response for delivery to WebSocket client
 *
 * Thread-safe. Wakes the LWS event loop via lws_cancel_service().
 *
 * @param resp Response to queue (copied, caller retains ownership)
 */
void queue_response(ws_response_t *resp);

/**
 * @brief Free dynamically allocated members of a response
 *
 * Called after response is sent. Frees strings/buffers based on type.
 *
 * @param resp Response to free
 */
void free_response(ws_response_t *resp);

/* =============================================================================
 * Token Mapping Functions
 * ============================================================================= */

/**
 * @brief Register a token->session_id mapping for reconnection
 *
 * Thread-safe. Evicts oldest if table is full.
 */
void register_token(const char *token, uint32_t session_id);

/**
 * @brief Look up session by reconnection token
 *
 * Thread-safe. Returns NULL if not found or session destroyed.
 */
session_t *lookup_session_by_token(const char *token);

/* =============================================================================
 * WebSocket Send Helpers (called from WebUI thread only)
 * ============================================================================= */

/**
 * @brief Send JSON text message to WebSocket client
 *
 * @param wsi WebSocket instance
 * @param json JSON string to send
 * @return 0 on success, -1 on error
 */
int send_json_message(struct lws *wsi, const char *json);

/**
 * @brief Send binary message with type byte prefix
 *
 * @param wsi WebSocket instance
 * @param msg_type Message type (WS_BIN_* constant)
 * @param data Payload data (may be NULL if len=0)
 * @param len Payload length
 * @return 0 on success, -1 on error
 */
int send_binary_message(struct lws *wsi, uint8_t msg_type, const uint8_t *data, size_t len);

/**
 * @brief Send state update to WebSocket client
 */
void send_state_impl(struct lws *wsi, const char *state, const char *detail);

/**
 * @brief Send state update with optional tools JSON
 */
void send_state_impl_full(struct lws *wsi,
                          const char *state,
                          const char *detail,
                          const char *tools_json);

/**
 * @brief Send audio chunk to WebSocket client
 */
void send_audio_impl(struct lws *wsi, const uint8_t *data, size_t len);

/**
 * @brief Send audio end marker to WebSocket client
 */
void send_audio_end_impl(struct lws *wsi);

/* =============================================================================
 * Path Security Helpers
 * ============================================================================= */

/**
 * @brief Get MIME type for file extension
 */
const char *get_mime_type(const char *path);

/**
 * @brief Check if path contains directory traversal patterns
 */
bool contains_path_traversal(const char *path);

/**
 * @brief Validate path is within www directory after symlink resolution
 */
bool is_path_within_www(const char *filepath, const char *www_path);

/* =============================================================================
 * HTTP Protocol Handler (defined in webui_http.c)
 * ============================================================================= */

#ifdef ENABLE_AUTH
/**
 * @brief Check if HTTP request has valid session cookie
 *
 * @param wsi HTTP connection
 * @param session_out If not NULL, filled with session info on success
 * @return true if authenticated, false otherwise
 */
bool is_request_authenticated(struct lws *wsi, auth_session_t *session_out);
#endif

/**
 * @brief HTTP protocol callback for libwebsockets
 *
 * Handles static file serving, authentication endpoints, and OAuth callbacks.
 */
int callback_http(struct lws *wsi,
                  enum lws_callback_reasons reason,
                  void *user,
                  void *in,
                  size_t len);

/* =============================================================================
 * Session Token Generation
 * ============================================================================= */

/**
 * @brief Generate cryptographically secure session token
 *
 * @param token_out Buffer (WEBUI_SESSION_TOKEN_LEN bytes)
 * @return 0 on success, 1 on failure
 */
int generate_session_token(char token_out[WEBUI_SESSION_TOKEN_LEN]);

/* =============================================================================
 * Capability Helpers
 * ============================================================================= */

/**
 * @brief Check if client supports Opus codec
 *
 * Parses capabilities.audio_codecs array from init/reconnect payload.
 */
bool check_opus_capability(struct json_object *payload);

/* =============================================================================
 * Authentication Helpers
 * ============================================================================= */

/**
 * @brief Check if WebSocket connection is authenticated
 *
 * Re-validates session against database. Sends error if not authenticated.
 *
 * @param conn WebSocket connection
 * @return true if authenticated, false otherwise (error sent)
 */
bool conn_require_auth(ws_connection_t *conn);

/**
 * @brief Check if WebSocket connection has admin privileges
 *
 * Re-validates is_admin from database. Sends error if not admin.
 *
 * @param conn WebSocket connection
 * @return true if admin, false otherwise (error sent)
 */
bool conn_require_admin(ws_connection_t *conn);

/**
 * @brief Send JSON response to WebSocket client
 *
 * Handles both small (stack) and large (heap) responses.
 */
void send_json_response(struct lws *wsi, json_object *response);

/**
 * @brief Send error message implementation
 */
void send_error_impl(struct lws *wsi, const char *code, const char *message);

/**
 * @brief Force logout connections by auth session token prefix
 *
 * Finds all WebSocket connections with matching auth_session_token prefix
 * and sends them a force_logout message. Used when a session is revoked.
 *
 * @param auth_token_prefix First AUTH_TOKEN_PREFIX_LEN chars of auth token
 * @return Number of connections notified
 */
int webui_force_logout_by_auth_token(const char *auth_token_prefix);

/* =============================================================================
 * Prompt Construction Helpers
 * ============================================================================= */

/**
 * @brief Build user-specific system prompt with persona settings
 *
 * @param user_id User ID (0 for unauthenticated - returns base prompt copy)
 * @return Allocated prompt string (caller must free), or NULL on error
 */
char *build_user_prompt(int user_id);

/**
 * @brief Process command tags in LLM response
 *
 * Extracts <command> tags, publishes to MQTT, and collects results.
 *
 * @param llm_response The LLM response containing command tags
 * @param session The session for context
 * @return Allocated string with follow-up response, or NULL on error
 */
char *webui_process_commands(const char *llm_response, session_t *session);

/* =============================================================================
 * Admin Handler Functions (defined in webui_admin.c)
 * ============================================================================= */

/**
 * @brief List all users (admin only)
 */
void handle_list_users(ws_connection_t *conn);

/**
 * @brief Create a new user (admin only)
 */
void handle_create_user(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Delete a user (admin only)
 */
void handle_delete_user(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Change user password (admin for any user, or user for self)
 */
void handle_change_password(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Unlock a locked user account (admin only)
 */
void handle_unlock_user(ws_connection_t *conn, struct json_object *payload);

/* =============================================================================
 * History Handler Functions (defined in webui_history.c)
 * ============================================================================= */

/**
 * @brief List conversations for the current user
 */
void handle_list_conversations(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Create a new conversation
 */
void handle_new_conversation(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Clear session history for a fresh start
 */
void handle_clear_session(ws_connection_t *conn);

/**
 * @brief Continue a conversation (after context compaction)
 */
void handle_continue_conversation(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Load a conversation and its messages
 */
void handle_load_conversation(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Delete a conversation
 */
void handle_delete_conversation(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Rename a conversation
 */
void handle_rename_conversation(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Set private mode for a conversation
 *
 * Private conversations are excluded from memory extraction.
 */
void handle_set_private(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Search conversations by title or content
 */
void handle_search_conversations(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Save a message to a conversation
 */
void handle_save_message(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Update context usage for a conversation
 */
void handle_update_context(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Lock LLM settings for a conversation
 *
 * Called when first message is sent. Stores the current LLM settings.
 */
void handle_lock_conversation_llm(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Reassign a conversation to a different user (admin only)
 *
 * Used to reassign voice conversations to different users after they
 * have been saved from local/DAP sessions.
 */
void handle_reassign_conversation(ws_connection_t *conn, struct json_object *payload);

/* =============================================================================
 * Memory Handler Functions (defined in webui_memory.c)
 * ============================================================================= */

/**
 * @brief Get memory statistics for the current user
 */
void handle_get_memory_stats(ws_connection_t *conn);

/**
 * @brief List memory facts for the current user (paginated)
 */
void handle_list_memory_facts(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Delete a memory fact
 */
void handle_delete_memory_fact(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief List memory preferences for the current user
 */
void handle_list_memory_preferences(ws_connection_t *conn);

/**
 * @brief Delete a memory preference by category
 */
void handle_delete_memory_preference(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief List memory summaries for the current user
 */
void handle_list_memory_summaries(ws_connection_t *conn);

/**
 * @brief Delete a memory summary
 */
void handle_delete_memory_summary(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Search memory facts and summaries by keyword
 */
void handle_search_memory(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Delete all memories for the current user
 */
void handle_delete_all_memories(ws_connection_t *conn, struct json_object *payload);

/* =============================================================================
 * Config Handler Functions (defined in webui_config.c)
 * ============================================================================= */

/**
 * @brief Get current configuration
 */
void handle_get_config(ws_connection_t *conn);

/**
 * @brief Set configuration values
 */
void handle_set_config(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Set secrets (API keys, passwords)
 */
void handle_set_secrets(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Get available audio devices
 */
void handle_get_audio_devices(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief List available ASR and TTS models
 */
void handle_list_models(ws_connection_t *conn);

/**
 * @brief List available network interfaces
 */
void handle_list_interfaces(ws_connection_t *conn);

/**
 * @brief List available local LLM models (Ollama/llama.cpp)
 */
void handle_list_llm_models(ws_connection_t *conn);

/* =============================================================================
 * Session Handler Functions (defined in webui_session.c)
 * ============================================================================= */

/**
 * @brief List current user's active sessions
 */
void handle_list_my_sessions(ws_connection_t *conn);

/**
 * @brief Revoke a session by token prefix
 */
void handle_revoke_session(ws_connection_t *conn, struct json_object *payload);

/* =============================================================================
 * Settings Handler Functions (defined in webui_settings.c)
 * ============================================================================= */

/**
 * @brief Get current user's personal settings
 */
void handle_get_my_settings(ws_connection_t *conn);

/**
 * @brief Update current user's personal settings
 */
void handle_set_my_settings(ws_connection_t *conn, struct json_object *payload);

/* =============================================================================
 * Tools Handler Functions (defined in webui_tools.c)
 * ============================================================================= */

/**
 * @brief Get tool configuration (enabled states)
 */
void handle_get_tools_config(ws_connection_t *conn);

/**
 * @brief Update tool enabled states
 */
void handle_set_tools_config(ws_connection_t *conn, struct json_object *payload);

/* =============================================================================
 * Audio Handler Functions (defined in webui_audio.c)
 * ============================================================================= */

#ifdef ENABLE_WEBUI_AUDIO
/**
 * @brief Handle binary WebSocket message (audio data)
 */
void handle_binary_message(ws_connection_t *conn, const uint8_t *data, size_t len);
#endif

/* =============================================================================
 * Music Handler Functions (defined in webui_music.c)
 * ============================================================================= */

/**
 * @brief Handle music_subscribe message
 */
void handle_music_subscribe(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Handle music_unsubscribe message
 */
void handle_music_unsubscribe(ws_connection_t *conn);

/**
 * @brief Handle music_control message
 */
void handle_music_control(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Handle music_search message
 */
void handle_music_search(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Handle music_library message
 */
void handle_music_library(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Handle music_queue message
 */
void handle_music_queue(ws_connection_t *conn, struct json_object *payload);

/* =============================================================================
 * Satellite Handler Functions (defined in webui_satellite.c)
 * ============================================================================= */

/**
 * @brief Handle satellite_register message
 */
void handle_satellite_register(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Handle satellite_query message
 */
void handle_satellite_query(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Handle satellite_ping message
 */
void handle_satellite_ping(ws_connection_t *conn);

/* =============================================================================
 * Audio Send Functions (defined in webui_server.c, used by webui_audio.c)
 * ============================================================================= */

/**
 * @brief Queue audio data for WebSocket client
 *
 * @param session WebSocket session
 * @param data Audio data (Opus or PCM depending on client capability)
 * @param len Data length in bytes
 */
void webui_send_audio(session_t *session, const uint8_t *data, size_t len);

/**
 * @brief Queue end-of-audio marker for WebSocket client
 *
 * @param session WebSocket session
 */
void webui_send_audio_end(session_t *session);

/**
 * @brief TTS sentence callback for LLM streaming
 *
 * Called for each complete sentence during LLM response streaming.
 * Generates TTS audio and sends immediately, enabling real-time playback.
 * Respects conn->tts_enabled flag (no audio if disabled).
 *
 * @param sentence Complete sentence text
 * @param userdata Session pointer (cast to session_t*)
 */
void webui_sentence_audio_callback(const char *sentence, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_INTERNAL_H */
