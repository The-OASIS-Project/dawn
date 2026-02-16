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
 * Authentication database interface.
 * Provides SQLite-backed storage for users, sessions, and audit logs.
 *
 * Thread Safety: All functions acquire s_db_mutex internally.
 * The database is opened with SQLITE_OPEN_FULLMUTEX for additional safety.
 */

#ifndef AUTH_DB_H
#define AUTH_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef ENABLE_AUTH
#include "auth/auth_crypto.h"
#else
/* When auth is disabled, define constants directly so structs compile
 * without libsodium. Values match auth_crypto.h definitions. */
#define AUTH_HASH_LEN 128 /* crypto_pwhash_STRBYTES */
#define AUTH_TOKEN_LEN 65 /* 32 bytes hex + null */
#endif

/**
 * @brief Default database path
 */
#define AUTH_DB_DEFAULT_PATH "/var/lib/dawn/auth.db"

/**
 * @brief Maximum username length (including null terminator)
 */
#define AUTH_USERNAME_MAX 64

/**
 * @brief Maximum user agent length (truncated if longer)
 */
#define AUTH_USER_AGENT_MAX 128

/**
 * @brief Maximum IP address length (IPv6 with scope)
 */
#define AUTH_IP_MAX 64

/**
 * @brief Session timeout in seconds (24 hours)
 */
#define AUTH_SESSION_TIMEOUT_SEC (24 * 60 * 60)

/**
 * @brief "Remember me" session timeout in seconds (30 days)
 */
#define AUTH_REMEMBER_ME_TIMEOUT_SEC (30 * 24 * 60 * 60)

/**
 * @brief Cleanup interval in seconds (5 minutes)
 *
 * Lazy cleanup runs during auth_db_get_session() if this much time has passed.
 */
#define AUTH_CLEANUP_INTERVAL_SEC 300

/**
 * @brief Maximum failed login attempts before account lockout
 */
#define AUTH_MAX_LOGIN_ATTEMPTS 5

/**
 * @brief Account lockout duration in seconds (15 minutes)
 */
#define AUTH_LOCKOUT_DURATION_SEC (15 * 60)

/**
 * @brief Session token prefix length for display/lookup by prefix
 *
 * Used when showing truncated tokens in UI or looking up sessions by prefix.
 */
#define AUTH_TOKEN_PREFIX_LEN 16

/**
 * @brief Database error codes
 */
#define AUTH_DB_SUCCESS 0
#define AUTH_DB_FAILURE 1
#define AUTH_DB_NOT_FOUND 2
#define AUTH_DB_DUPLICATE 3
#define AUTH_DB_INVALID 4
#define AUTH_DB_LOCKED 5
#define AUTH_DB_LAST_ADMIN 6 /**< Cannot delete/demote last admin */

/**
 * @brief User record structure
 */
typedef struct {
   int id;
   char username[AUTH_USERNAME_MAX];
   char password_hash[AUTH_HASH_LEN];
   bool is_admin;
   time_t created_at;
   time_t last_login;
   int failed_attempts;
   time_t lockout_until;
} auth_user_t;

/**
 * @brief Session record structure
 *
 * Note: This is for authentication sessions, distinct from session_t
 * in session_manager.h which manages conversation/client sessions.
 */
typedef struct {
   char token[AUTH_TOKEN_LEN];
   int user_id;
   char username[AUTH_USERNAME_MAX];
   bool is_admin;
   time_t created_at;
   time_t last_activity;
   time_t expires_at; /**< When session expires (0 = use legacy last_activity check) */
   char ip_address[AUTH_IP_MAX];
   char user_agent[AUTH_USER_AGENT_MAX];
} auth_session_t;

/**
 * @brief User summary structure (excludes password hash for security)
 *
 * Used for user enumeration - never exposes password_hash.
 */
typedef struct {
   int id;
   char username[AUTH_USERNAME_MAX];
   bool is_admin;
   time_t created_at;
   time_t last_login;
   int failed_attempts;
   time_t lockout_until;
} auth_user_summary_t;

/**
 * @brief Callback for user enumeration
 *
 * @param user User summary data
 * @param ctx User-provided context
 * @return 0 to continue iteration, non-zero to stop
 */
typedef int (*auth_user_summary_callback_t)(const auth_user_summary_t *user, void *ctx);

/**
 * @brief Session summary structure (excludes full token for security)
 *
 * Used for session enumeration - only exposes token prefix.
 */
typedef struct {
   char token_prefix[17]; /**< First 16 chars of token + null */
   int user_id;
   char username[AUTH_USERNAME_MAX];
   time_t created_at;
   time_t last_activity;
   char ip_address[AUTH_IP_MAX];
   char user_agent[AUTH_USER_AGENT_MAX]; /**< Browser/client identifier */
} auth_session_summary_t;

/**
 * @brief Callback for session enumeration
 *
 * @param session Session summary data
 * @param ctx User-provided context
 * @return 0 to continue iteration, non-zero to stop
 */
typedef int (*auth_session_summary_callback_t)(const auth_session_summary_t *session, void *ctx);

/* ============================================================================
 * Lifecycle Functions
 * ============================================================================ */

/**
 * @brief Initialize the authentication database
 *
 * Opens or creates the SQLite database at the specified path.
 * Creates schema if needed and prepares all statements.
 * Sets secure file permissions (0600) on the database file.
 *
 * @param db_path Path to database file (NULL uses AUTH_DB_DEFAULT_PATH)
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_init(const char *db_path);

/**
 * @brief Shutdown the authentication database
 *
 * Checkpoints WAL, finalizes statements, and closes database.
 * Safe to call multiple times or if not initialized.
 */
void auth_db_shutdown(void);

/**
 * @brief Check if database is initialized
 *
 * @return true if initialized and ready, false otherwise
 */
bool auth_db_is_ready(void);

/* ============================================================================
 * User Operations
 * ============================================================================ */

/**
 * @brief Create a new user
 *
 * @param username Username (1-63 chars, alphanumeric + underscore)
 * @param password_hash Pre-computed hash from auth_hash_password()
 * @param is_admin Whether user has admin privileges
 * @return AUTH_DB_SUCCESS, AUTH_DB_DUPLICATE, AUTH_DB_INVALID, or AUTH_DB_FAILURE
 */
int auth_db_create_user(const char *username, const char *password_hash, bool is_admin);

/**
 * @brief Get user by username
 *
 * @param username Username to look up
 * @param user_out Buffer to receive user data (can be NULL to check existence)
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int auth_db_get_user(const char *username, auth_user_t *user_out);

/**
 * @brief Get total user count
 *
 * Useful for checking if any users exist (first-run detection).
 *
 * @return Number of users, or -1 on error
 */
int auth_db_user_count(void);

/**
 * @brief Validate username format
 *
 * Valid usernames: 1-63 chars, alphanumeric plus underscore, hyphen, period.
 * Must start with a letter or underscore.
 *
 * @param username Username to validate
 * @return AUTH_DB_SUCCESS if valid, AUTH_DB_INVALID if not
 */
int auth_db_validate_username(const char *username);

/**
 * @brief Increment failed login attempts for user
 *
 * @param username Username
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_increment_failed_attempts(const char *username);

/**
 * @brief Reset failed login attempts for user
 *
 * Called after successful login.
 *
 * @param username Username
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_reset_failed_attempts(const char *username);

/**
 * @brief Update user's last login timestamp
 *
 * @param username Username
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_update_last_login(const char *username);

/**
 * @brief Set lockout time for user
 *
 * @param username Username
 * @param lockout_until Unix timestamp when lockout expires
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_set_lockout(const char *username, time_t lockout_until);

/**
 * @brief List all users (callback-based, excludes password hashes)
 *
 * @param callback Function called for each user
 * @param ctx User-provided context passed to callback
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_list_users(auth_user_summary_callback_t callback, void *ctx);

/**
 * @brief Count admin users
 *
 * @return Number of admin users, or -1 on error
 */
int auth_db_count_admins(void);

/**
 * @brief Delete a user account
 *
 * Fails with AUTH_DB_LAST_ADMIN if this is the only admin user.
 * Deletes all sessions for the user as part of the operation.
 *
 * @param username Username to delete
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, AUTH_DB_LAST_ADMIN, or AUTH_DB_FAILURE
 */
int auth_db_delete_user(const char *username);

/**
 * @brief Update user password (atomically invalidates all sessions)
 *
 * @param username Username
 * @param new_hash New password hash from auth_hash_password()
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int auth_db_update_password(const char *username, const char *new_hash);

/**
 * @brief Unlock a user account
 *
 * Wrapper for auth_db_set_lockout(username, 0).
 *
 * @param username Username
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int auth_db_unlock_user(const char *username);

/* ============================================================================
 * User Settings (Per-User Personalization)
 * ============================================================================ */

/**
 * @brief Maximum persona description length
 */
#define AUTH_PERSONA_DESC_MAX 512

/**
 * @brief Maximum location length
 */
#define AUTH_LOCATION_MAX 128

/**
 * @brief Maximum timezone length
 */
#define AUTH_TIMEZONE_MAX 64

/**
 * @brief Maximum units preference length
 */
#define AUTH_UNITS_MAX 16

/**
 * @brief Maximum TTS voice model path length
 */
#define AUTH_TTS_VOICE_MAX 128

/**
 * @brief TTS length scale bounds (speech rate multiplier)
 *
 * Values outside this range are clamped to prevent unusable speech.
 * 1.0 = normal speed, <1.0 = faster, >1.0 = slower
 */
#define AUTH_TTS_LENGTH_SCALE_MIN 0.25f
#define AUTH_TTS_LENGTH_SCALE_MAX 4.0f

/**
 * @brief Maximum persona mode length
 */
#define AUTH_PERSONA_MODE_MAX 16

/**
 * @brief Maximum theme name length
 */
#define AUTH_THEME_MAX 16

/**
 * @brief Per-user settings structure
 *
 * Stores user-specific preferences that override global defaults.
 * Empty strings indicate "use global default".
 */
typedef struct {
   char persona_description[AUTH_PERSONA_DESC_MAX]; /**< Custom AI persona */
   char persona_mode[AUTH_PERSONA_MODE_MAX];        /**< "append" (default) or "replace" */
   char location[AUTH_LOCATION_MAX];                /**< User's location */
   char timezone[AUTH_TIMEZONE_MAX];                /**< Timezone (e.g., "America/New_York") */
   char units[AUTH_UNITS_MAX];                      /**< "metric" or "imperial" */
   char tts_voice_model[AUTH_TTS_VOICE_MAX];        /**< TTS voice model path */
   float tts_length_scale;                          /**< TTS speech rate (1.0 = normal) */
   char theme[AUTH_THEME_MAX];                      /**< UI color theme */
} auth_user_settings_t;

/**
 * @brief Get user settings
 *
 * Returns user-specific settings. If the user has no settings record,
 * returns default values (empty strings, 1.0 scale).
 *
 * @param user_id User ID
 * @param settings_out Buffer to receive settings data
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_get_user_settings(int user_id, auth_user_settings_t *settings_out);

/**
 * @brief Set user settings
 *
 * Creates or updates user settings using UPSERT pattern.
 * Empty strings are stored as-is (UI interprets as "use default").
 *
 * @param user_id User ID
 * @param settings Settings to save
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_set_user_settings(int user_id, const auth_user_settings_t *settings);

/**
 * @brief Initialize default settings for a new user
 *
 * Called automatically when a user is created.
 *
 * @param user_id User ID
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_init_user_settings(int user_id);

/* ============================================================================
 * Session Operations
 * ============================================================================ */

/**
 * @brief Create a new session
 *
 * @param user_id User ID from auth_user_t
 * @param token Pre-generated token from auth_generate_token()
 * @param ip_address Client IP address (can be NULL)
 * @param user_agent Client user agent (can be NULL, truncated to AUTH_USER_AGENT_MAX)
 * @param remember_me If true, session expires in 30 days; otherwise 24 hours
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_create_session(int user_id,
                           const char *token,
                           const char *ip_address,
                           const char *user_agent,
                           bool remember_me);

/**
 * @brief Get session by token
 *
 * @param token Session token
 * @param session_out Buffer to receive session data
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int auth_db_get_session(const char *token, auth_session_t *session_out);

/**
 * @brief Update session last activity timestamp
 *
 * @param token Session token
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_update_session_activity(const char *token);

/**
 * @brief Delete a session (logout)
 *
 * @param token Session token
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_delete_session(const char *token);

/**
 * @brief Delete a session by token prefix
 *
 * Finds and deletes the session matching the 16-character token prefix.
 * Returns AUTH_DB_NOT_FOUND if no matching session exists.
 *
 * @param prefix 16-character token prefix
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int auth_db_delete_session_by_prefix(const char *prefix);

/**
 * @brief Check if a session belongs to a specific user
 *
 * Efficiently checks if a session with the given prefix belongs to the user.
 * More efficient than listing all sessions and searching.
 *
 * @param prefix 16-character token prefix
 * @param user_id User ID to check ownership against
 * @return true if session belongs to user, false otherwise
 */
bool auth_db_session_belongs_to_user(const char *prefix, int user_id);

/**
 * @brief Delete all sessions for a user by username
 *
 * Wrapper around auth_db_delete_user_sessions that looks up user by name.
 *
 * @param username Username
 * @return Number of sessions deleted, or -1 on error
 */
int auth_db_delete_sessions_by_username(const char *username);

/**
 * @brief Delete all sessions for a user
 *
 * Used for password change or account lockout.
 *
 * @param user_id User ID
 * @return Number of sessions deleted, or -1 on error
 */
int auth_db_delete_user_sessions(int user_id);

/**
 * @brief List all active sessions (callback-based, token prefix only)
 *
 * @param callback Function called for each session
 * @param ctx User-provided context passed to callback
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_list_sessions(auth_session_summary_callback_t callback, void *ctx);

/**
 * @brief List sessions for a specific user (callback-based)
 *
 * Used for session management UI - allows users to see their own sessions.
 *
 * @param user_id User ID to filter by
 * @param callback Function called for each session
 * @param ctx User-provided context passed to callback
 * @return AUTH_DB_SUCCESS, AUTH_DB_INVALID, or AUTH_DB_FAILURE
 */
int auth_db_list_user_sessions(int user_id, auth_session_summary_callback_t callback, void *ctx);

/**
 * @brief Count active sessions
 *
 * @return Number of active sessions, or -1 on error
 */
int auth_db_count_sessions(void);

/* ============================================================================
 * Rate Limiting
 * ============================================================================ */

/**
 * @brief Count recent failed login attempts from IP
 *
 * @param ip_address Client IP address
 * @param since Only count attempts after this timestamp
 * @return Number of failed attempts, or -1 on error
 */
int auth_db_count_recent_failures(const char *ip_address, time_t since);

/**
 * @brief Log a login attempt
 *
 * @param ip_address Client IP address
 * @param username Username attempted (can be NULL)
 * @param success Whether login succeeded
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_log_attempt(const char *ip_address, const char *username, bool success);

/**
 * @brief Clear login attempts for an IP address
 *
 * Used to unblock an IP that has been rate-limited.
 * If ip_address is NULL, clears all login attempts.
 *
 * @param ip_address Client IP address to unblock (NULL for all)
 * @return Number of deleted entries, or -1 on error
 */
int auth_db_clear_login_attempts(const char *ip_address);

/**
 * @brief IP rate limit status entry
 */
typedef struct {
   char ip_address[AUTH_IP_MAX];
   int failed_attempts;
   time_t last_attempt;
} auth_ip_status_t;

/**
 * @brief Callback for IP status enumeration
 *
 * @param status IP status entry
 * @param ctx User-provided context
 * @return 0 to continue iteration, non-zero to stop
 */
typedef int (*auth_ip_status_callback_t)(const auth_ip_status_t *status, void *ctx);

/**
 * @brief List IPs with recent failed login attempts
 *
 * Returns IPs that have failed attempts within the rate limit window.
 *
 * @param since Only include attempts after this timestamp
 * @param callback Function called for each IP
 * @param ctx User-provided context
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_list_blocked_ips(time_t since, auth_ip_status_callback_t callback, void *ctx);

/* ============================================================================
 * Audit Logging
 * ============================================================================ */

/**
 * @brief Log an authentication event
 *
 * @param event Event type (e.g., "USER_CREATED", "LOGIN_SUCCESS")
 * @param username Associated username (can be NULL)
 * @param ip_address Client IP address (can be NULL)
 * @param details Additional details as JSON or text (can be NULL)
 */
void auth_db_log_event(const char *event,
                       const char *username,
                       const char *ip_address,
                       const char *details);

/**
 * @brief Audit log query filter.
 */
typedef struct {
   time_t since;         /**< Only entries after this time (0 = no limit) */
   time_t until;         /**< Only entries before this time (0 = no limit) */
   const char *event;    /**< Filter by event type (NULL = all) */
   const char *username; /**< Filter by username (NULL = all) */
   int limit;            /**< Max entries to return (0 = default 100) */
   int offset;           /**< Skip first N entries (for pagination) */
} auth_log_filter_t;

/**
 * @brief Audit log entry.
 */
typedef struct {
   time_t timestamp;
   char event[32];
   char username[AUTH_USERNAME_MAX];
   char ip_address[AUTH_IP_MAX];
   char details[256];
} auth_log_entry_t;

/**
 * @brief Callback for audit log enumeration.
 */
typedef int (*auth_log_callback_t)(const auth_log_entry_t *entry, void *ctx);

/**
 * @brief Query audit log with optional filters.
 *
 * @param filter Query filters (can be NULL for defaults)
 * @param callback Function called for each matching entry
 * @param ctx User-provided context passed to callback
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_query_audit_log(const auth_log_filter_t *filter,
                            auth_log_callback_t callback,
                            void *ctx);

/**
 * @brief Default limit for audit log queries.
 */
#define AUTH_LOG_DEFAULT_LIMIT 100

/**
 * @brief Maximum limit for audit log queries.
 */
#define AUTH_LOG_MAX_LIMIT 1000

/* ============================================================================
 * Maintenance
 * ============================================================================ */

/**
 * @brief Run cleanup of expired data
 *
 * Deletes expired sessions, old login attempts, and old audit logs.
 * Normally called lazily during auth_db_get_session(), but can be
 * called manually if needed.
 *
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_run_cleanup(void);

/**
 * @brief Checkpoint WAL to main database
 *
 * Useful before backup or to reclaim disk space.
 *
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_checkpoint(void);

/**
 * @brief Passive WAL checkpoint (non-blocking).
 *
 * Checkpoints as much of the WAL as possible without waiting.
 * Suitable for background maintenance as it won't block other operations.
 *
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_checkpoint_passive(void);

/* ============================================================================
 * Statistics and Database Management
 * ============================================================================ */

/**
 * @brief Database statistics structure.
 */
typedef struct {
   int user_count;          /**< Total number of users */
   int admin_count;         /**< Number of admin users */
   int session_count;       /**< Number of active sessions */
   int locked_user_count;   /**< Number of locked accounts */
   int failed_attempts_24h; /**< Failed login attempts in last 24 hours */
   int audit_log_count;     /**< Total audit log entries */
   int64_t db_size_bytes;   /**< Database file size in bytes */
} auth_db_stats_t;

/**
 * @brief Get database statistics.
 *
 * @param stats Pointer to stats structure to populate.
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_get_stats(auth_db_stats_t *stats);

/**
 * @brief Vacuum (compact) the database.
 *
 * Rate-limited to once per 24 hours to prevent excessive I/O.
 *
 * @return AUTH_DB_SUCCESS, AUTH_DB_RATE_LIMITED, or AUTH_DB_FAILURE
 */
int auth_db_vacuum(void);

/**
 * @brief Backup the database to a file.
 *
 * Creates a backup with secure permissions (0600).
 * Uses SQLite stepped backup for minimal lock time.
 *
 * @param dest_path Destination file path (must not exist)
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_backup(const char *dest_path);

/**
 * @brief Error code for rate-limited operations.
 */
#define AUTH_DB_RATE_LIMITED 7

/* ============================================================================
 * Conversation History
 * ============================================================================ */

/**
 * @brief Maximum conversation title length
 */
#define CONV_TITLE_MAX 256

/**
 * @brief Maximum message content length (64KB)
 */
#define CONV_MESSAGE_MAX 65536

/**
 * @brief Maximum role string length
 */
#define CONV_ROLE_MAX 16

/**
 * @brief Default conversation limit per list query
 */
#define CONV_LIST_DEFAULT_LIMIT 50

/**
 * @brief Maximum conversation limit per list query
 */
#define CONV_LIST_MAX_LIMIT 100

/**
 * @brief Maximum conversations per user (0 = unlimited)
 * Default 1000 to prevent potential DoS via conversation spam.
 * Users can archive old conversations to free up slots.
 */
#define CONV_MAX_PER_USER 1000

/**
 * @brief Maximum compaction summary length
 */
#define CONV_SUMMARY_MAX 4096

/**
 * @brief Conversation metadata structure
 *
 * Conversation Continuation Architecture:
 * When context compaction occurs:
 * 1. Original conversation is archived (is_archived = 1)
 * 2. New conversation created with continued_from = original_id
 * 3. Summary stored in compaction_summary, added as system message
 *
 * Design Decision: Archived conversations are READ-ONLY
 * - Cannot add new messages to archived conversations
 * - Continuations are independent - their context is summary + their own messages
 * - Model changes only affect future compaction decisions
 *
 * KNOWN LIMITATION: Context window waste on model upgrade
 * If user switches from 8K->128K model, continued conversations still only
 * have the summary, not full parent history. This is intentional simplicity.
 */
typedef struct {
   int64_t id;
   int user_id;
   char title[CONV_TITLE_MAX];
   time_t created_at;
   time_t updated_at;
   int message_count;
   bool is_archived;
   int context_tokens;       /**< Last known context token count */
   int context_max;          /**< Context window size */
   int64_t continued_from;   /**< Parent conversation ID (0 = none) */
   char *compaction_summary; /**< Summary from parent (NULL if not a continuation) */
   /* Per-conversation LLM settings (v11) - empty string means use defaults */
   char llm_type[16];       /**< "local" or "cloud" */
   char cloud_provider[16]; /**< "openai" or "claude" */
   char model[64];          /**< Model name */
   char tools_mode[16];     /**< "native", "command_tags", or "disabled" */
   char thinking_mode[16];  /**< "disabled"/"auto"/"enabled" or "low"/"medium"/"high" */
   bool is_private;         /**< If true, no memory extraction for this conversation (v16) */
   char origin[16];         /**< "webui" or "voice" (v17) */
} conversation_t;

/**
 * @brief Conversation message structure
 */
typedef struct {
   int64_t id;
   int64_t conversation_id;
   char role[CONV_ROLE_MAX]; /**< "system", "user", or "assistant" */
   char *content;            /**< Dynamically allocated, caller must free */
   time_t created_at;
} conversation_message_t;

/**
 * @brief Pagination parameters for conversation listing
 */
typedef struct {
   int limit;  /**< Max results (0 = default) */
   int offset; /**< Skip first N results */
} conv_pagination_t;

/**
 * @brief Callback for conversation enumeration
 *
 * @param conv Conversation metadata
 * @param ctx User-provided context
 * @return 0 to continue iteration, non-zero to stop
 */
typedef int (*conversation_callback_t)(const conversation_t *conv, void *ctx);

/**
 * @brief Callback for message enumeration
 *
 * @param msg Message data (content is valid only during callback)
 * @param ctx User-provided context
 * @return 0 to continue iteration, non-zero to stop
 */
typedef int (*message_callback_t)(const conversation_message_t *msg, void *ctx);

/**
 * @brief Callback for admin list all conversations (includes username)
 *
 * @param conv Conversation metadata
 * @param username Owner's username
 * @param ctx User-provided context
 * @return 0 to continue iteration, non-zero to stop
 */
typedef int (*conversation_all_callback_t)(const conversation_t *conv,
                                           const char *username,
                                           void *ctx);

/**
 * @brief Create a new conversation
 *
 * @param user_id User ID who owns the conversation
 * @param title Initial title (can be NULL for "New Conversation")
 * @param conv_id_out Receives the new conversation ID
 * @return AUTH_DB_SUCCESS, AUTH_DB_LIMIT_EXCEEDED, or AUTH_DB_FAILURE
 */
int conv_db_create(int user_id, const char *title, int64_t *conv_id_out);

/**
 * @brief Create a new conversation with origin field
 *
 * Used for voice conversations saved from Session 0 or DAP clients.
 * The origin field distinguishes between 'webui' and 'voice' conversations.
 *
 * @param user_id User ID who owns the conversation
 * @param title Initial title (can be NULL for "New Conversation")
 * @param origin Origin of conversation: "webui" or "voice"
 * @param conv_id_out Receives the new conversation ID
 * @return AUTH_DB_SUCCESS, AUTH_DB_LIMIT_EXCEEDED, or AUTH_DB_FAILURE
 */
int conv_db_create_with_origin(int user_id,
                               const char *title,
                               const char *origin,
                               int64_t *conv_id_out);

/**
 * @brief Reassign a conversation to a different user (admin only)
 *
 * Used to reassign voice conversations to different users.
 *
 * @param conv_id Conversation ID
 * @param new_user_id New user ID to own the conversation
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int conv_db_reassign(int64_t conv_id, int new_user_id);

/**
 * @brief Get conversation by ID
 *
 * @param conv_id Conversation ID
 * @param user_id User ID (for authorization check)
 * @param conv_out Buffer to receive conversation data
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, AUTH_DB_FORBIDDEN, or AUTH_DB_FAILURE
 */
int conv_db_get(int64_t conv_id, int user_id, conversation_t *conv_out);

/**
 * @brief Free dynamically allocated fields in a conversation_t
 *
 * Must be called after using conv_db_get() to free compaction_summary.
 * Safe to call with NULL fields or multiple times.
 *
 * @param conv Conversation to clean up (not freed itself, only contents)
 */
void conv_free(conversation_t *conv);

/**
 * @brief Create a continuation of an existing conversation
 *
 * Used during context compaction. The parent conversation is archived and
 * a new conversation is created with a reference to the parent.
 *
 * @param user_id User ID
 * @param parent_id Parent conversation ID to continue from
 * @param compaction_summary Summary of the parent conversation context
 * @param conv_id_out Receives the new conversation ID
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND (parent), AUTH_DB_FORBIDDEN, or AUTH_DB_FAILURE
 */
int conv_db_create_continuation(int user_id,
                                int64_t parent_id,
                                const char *compaction_summary,
                                int64_t *conv_id_out);

/**
 * @brief List conversations for a user
 *
 * @param user_id User ID
 * @param include_archived Include archived conversations
 * @param pagination Pagination parameters (can be NULL for defaults)
 * @param callback Function called for each conversation
 * @param ctx User-provided context passed to callback
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int conv_db_list(int user_id,
                 bool include_archived,
                 const conv_pagination_t *pagination,
                 conversation_callback_t callback,
                 void *ctx);

/**
 * @brief List all conversations (admin only)
 *
 * Lists conversations across all users with owner username.
 *
 * @param include_archived Whether to include archived conversations
 * @param pagination Pagination parameters (can be NULL for defaults)
 * @param callback Function called for each conversation (includes username)
 * @param ctx User-provided context passed to callback
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int conv_db_list_all(bool include_archived,
                     const conv_pagination_t *pagination,
                     conversation_all_callback_t callback,
                     void *ctx);

/**
 * @brief Rename a conversation
 *
 * @param conv_id Conversation ID
 * @param user_id User ID (for authorization check)
 * @param new_title New title
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, AUTH_DB_FORBIDDEN, or AUTH_DB_FAILURE
 */
int conv_db_rename(int64_t conv_id, int user_id, const char *new_title);

/**
 * @brief Set private mode for a conversation
 *
 * Private conversations are excluded from memory extraction.
 *
 * @param conv_id Conversation ID
 * @param user_id User ID (for authorization check)
 * @param is_private True to enable private mode, false to disable
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, AUTH_DB_FORBIDDEN, or AUTH_DB_FAILURE
 */
int conv_db_set_private(int64_t conv_id, int user_id, bool is_private);

/**
 * @brief Check if a conversation is private
 *
 * Lightweight query that only checks the is_private flag without loading
 * the full conversation. Used by memory extraction to re-verify privacy
 * status from the database (prevents race conditions).
 *
 * @param conv_id Conversation ID
 * @param user_id User ID (for authorization check)
 * @return 1 if private, 0 if not private, -1 on error/not found
 */
int conv_db_is_private(int64_t conv_id, int user_id);

/**
 * @brief Update context usage for a conversation
 *
 * @param conv_id Conversation ID
 * @param user_id User ID (for authorization check)
 * @param context_tokens Current token count
 * @param context_max Maximum context window size
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, AUTH_DB_FORBIDDEN, or AUTH_DB_FAILURE
 */
int conv_db_update_context(int64_t conv_id, int user_id, int context_tokens, int context_max);

/**
 * @brief Lock LLM settings for a conversation
 *
 * Updates LLM settings only if message_count is 0 (first message lock).
 * This prevents race conditions and ensures settings are captured when
 * the first message is sent.
 *
 * @param conv_id Conversation ID
 * @param user_id User ID (for authorization check)
 * @param llm_type "local" or "cloud" (or NULL to keep current)
 * @param cloud_provider "openai" or "claude" (or NULL to keep current)
 * @param model Model name (or NULL to keep current)
 * @param tools_mode "native", "command_tags", or "disabled"
 * @param thinking_mode "disabled", "auto", "enabled", "low", "medium", or "high"
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND (no row updated), or AUTH_DB_FAILURE
 */
int conv_db_lock_llm_settings(int64_t conv_id,
                              int user_id,
                              const char *llm_type,
                              const char *cloud_provider,
                              const char *model,
                              const char *tools_mode,
                              const char *thinking_mode);

/**
 * @brief Delete a conversation and all its messages
 *
 * @param conv_id Conversation ID
 * @param user_id User ID (for authorization check)
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, AUTH_DB_FORBIDDEN, or AUTH_DB_FAILURE
 */
int conv_db_delete(int64_t conv_id, int user_id);

/**
 * @brief Delete a conversation (admin only, no ownership check)
 *
 * For admin CLI tools that need to delete any conversation.
 *
 * @param conv_id Conversation ID
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int conv_db_delete_admin(int64_t conv_id);

/**
 * @brief Search conversations by title
 *
 * @param user_id User ID
 * @param query Search query (substring match)
 * @param pagination Pagination parameters (can be NULL for defaults)
 * @param callback Function called for each matching conversation
 * @param ctx User-provided context passed to callback
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int conv_db_search(int user_id,
                   const char *query,
                   const conv_pagination_t *pagination,
                   conversation_callback_t callback,
                   void *ctx);

/**
 * @brief Search conversations by message content
 *
 * Searches message content for the query string.
 * Returns conversations that have at least one message matching the query.
 * Slower than conv_db_search which only searches titles.
 *
 * @param user_id User ID
 * @param query Search query (searches message content)
 * @param pagination Pagination parameters (can be NULL for defaults)
 * @param callback Function called for each matching conversation
 * @param ctx User-provided context passed to callback
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int conv_db_search_content(int user_id,
                           const char *query,
                           const conv_pagination_t *pagination,
                           conversation_callback_t callback,
                           void *ctx);

/**
 * @brief Add a message to a conversation
 *
 * Also updates the conversation's updated_at and message_count.
 *
 * @param conv_id Conversation ID
 * @param user_id User ID (for authorization check)
 * @param role Message role ("system", "user", or "assistant")
 * @param content Message content
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, AUTH_DB_FORBIDDEN, or AUTH_DB_FAILURE
 */
int conv_db_add_message(int64_t conv_id, int user_id, const char *role, const char *content);

/**
 * @brief Get all messages in a conversation
 *
 * Messages are returned in chronological order.
 * The content pointer in each message is only valid during the callback.
 *
 * @param conv_id Conversation ID
 * @param user_id User ID (for authorization check)
 * @param callback Function called for each message
 * @param ctx User-provided context passed to callback
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, AUTH_DB_FORBIDDEN, or AUTH_DB_FAILURE
 */
int conv_db_get_messages(int64_t conv_id, int user_id, message_callback_t callback, void *ctx);

/**
 * @brief Get messages for a conversation with pagination (cursor-based)
 *
 * Returns messages in reverse chronological order (newest first) for efficient
 * "scroll up to load more" pagination. The caller should reverse the results
 * for display in chronological order.
 *
 * @param conv_id Conversation ID
 * @param user_id User ID (for authorization check)
 * @param limit Maximum number of messages to return
 * @param before_id Only return messages with ID < before_id (0 for latest messages)
 * @param callback Function called for each message (newest first)
 * @param ctx User-provided context passed to callback
 * @param total_out Output: total number of messages in conversation (can be NULL)
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, AUTH_DB_FORBIDDEN, or AUTH_DB_FAILURE
 */
int conv_db_get_messages_paginated(int64_t conv_id,
                                   int user_id,
                                   int limit,
                                   int64_t before_id,
                                   message_callback_t callback,
                                   void *ctx,
                                   int *total_out);

/**
 * @brief Get messages for a conversation (admin only, no ownership check)
 *
 * For admin CLI tools that need to view any conversation.
 *
 * @param conv_id Conversation ID
 * @param callback Function called for each message
 * @param ctx User-provided context passed to callback
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int conv_db_get_messages_admin(int64_t conv_id, message_callback_t callback, void *ctx);

/**
 * @brief Count conversations for a user
 *
 * @param user_id User ID
 * @return Number of conversations, or -1 on error
 */
int conv_db_count(int user_id);

/**
 * @brief Find the continuation conversation for an archived conversation
 *
 * Searches for a conversation where continued_from = parent_id.
 * Used to provide "View continuation" link for archived conversations.
 *
 * @param parent_id Parent conversation ID (the archived one)
 * @param user_id User ID (for ownership check)
 * @param continuation_id_out Output: ID of continuation conversation (0 if none)
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int conv_db_find_continuation(int64_t parent_id, int user_id, int64_t *continuation_id_out);

/**
 * @brief Generate title from first message content
 *
 * Extracts first ~50 characters, truncating at word boundary with ellipsis.
 *
 * @param content Message content
 * @param title_out Buffer to receive title
 * @param max_len Maximum title length
 */
void conv_generate_title(const char *content, char *title_out, size_t max_len);

/**
 * @brief Error code for forbidden access (user doesn't own resource)
 */
#define AUTH_DB_FORBIDDEN 8

/**
 * @brief Error code for limit exceeded
 */
#define AUTH_DB_LIMIT_EXCEEDED 9

/* ============================================================================
 * Session Metrics (schema v8+)
 * ============================================================================ */

/**
 * @brief Default retention period for session metrics (90 days)
 */
#define SESSION_METRICS_RETENTION_DAYS 90

/**
 * @brief Maximum session type string length
 */
#define SESSION_TYPE_MAX 16

/**
 * @brief Maximum LLM type string length
 */
#define LLM_TYPE_MAX 16

/**
 * @brief Maximum cloud provider string length
 */
#define CLOUD_PROVIDER_MAX 16

/**
 * @brief Session metrics structure
 *
 * Stores aggregated metrics for a completed session. Saved to database
 * when a session ends for historical analysis and reporting.
 *
 * Token usage is tracked per-provider in session_metrics_providers table
 * to handle sessions that use multiple providers (e.g., OpenAI + Claude).
 */
typedef struct {
   int64_t id;                          /**< Database row ID (0 if not saved) */
   uint32_t session_id;                 /**< Runtime session ID (ephemeral) */
   int user_id;                         /**< User ID (0 for LOCAL/DAP sessions) */
   char session_type[SESSION_TYPE_MAX]; /**< 'LOCAL', 'DAP', 'DAP2', 'WEBSOCKET' */
   time_t started_at;                   /**< Session start time */
   time_t ended_at;                     /**< Session end time */

   /* Query counts */
   uint32_t queries_total;
   uint32_t queries_cloud;
   uint32_t queries_local;
   uint32_t errors_count;
   uint32_t fallbacks_count;

   /* Performance averages (milliseconds) */
   double avg_asr_ms;
   double avg_llm_ttft_ms;
   double avg_llm_total_ms;
   double avg_tts_ms;
   double avg_pipeline_ms;
} session_metrics_t;

/**
 * @brief Per-provider token usage for a session
 *
 * Tracks token usage broken down by LLM provider. Multiple entries
 * can exist per session if the user switches providers mid-session.
 */
typedef struct {
   int64_t session_metrics_id;        /**< Parent session_metrics.id */
   char provider[CLOUD_PROVIDER_MAX]; /**< 'openai', 'claude', 'local' */
   uint64_t tokens_input;
   uint64_t tokens_output;
   uint64_t tokens_cached;
   uint32_t queries; /**< Queries using this provider */
} session_provider_metrics_t;

/**
 * @brief Maximum number of providers per session
 */
#define MAX_PROVIDERS_PER_SESSION 4

/**
 * @brief Callback for session metrics enumeration
 *
 * @param metrics Session metrics entry
 * @param ctx User-provided context
 * @return 0 to continue iteration, non-zero to stop
 */
typedef int (*session_metrics_callback_t)(const session_metrics_t *metrics, void *ctx);

/**
 * @brief Session metrics query filter
 */
typedef struct {
   int user_id;      /**< Filter by user (0 = all users) */
   const char *type; /**< Filter by session type (NULL = all) */
   time_t since;     /**< Only sessions starting after this time (0 = no limit) */
   time_t until;     /**< Only sessions starting before this time (0 = no limit) */
   int limit;        /**< Max entries to return (0 = default 20) */
   int offset;       /**< Skip first N entries (for pagination) */
} session_metrics_filter_t;

/**
 * @brief Save session metrics to database
 *
 * Called when a session ends to persist metrics for historical analysis.
 * The metrics->id field will be updated with the new row ID on success.
 *
 * @param metrics Metrics to save
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_save_session_metrics(session_metrics_t *metrics);

/**
 * @brief Save per-provider token metrics
 *
 * Called after auth_db_save_session_metrics() to save provider breakdown.
 * Uses the session_metrics.id returned from the parent save call.
 *
 * @param session_metrics_id Parent session_metrics.id
 * @param providers Array of provider metrics
 * @param count Number of providers in array
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_save_provider_metrics(int64_t session_metrics_id,
                                  const session_provider_metrics_t *providers,
                                  int count);

/**
 * @brief Query session metrics history
 *
 * @param filter Query filters (can be NULL for defaults)
 * @param callback Function called for each matching entry
 * @param ctx User-provided context passed to callback
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_list_session_metrics(const session_metrics_filter_t *filter,
                                 session_metrics_callback_t callback,
                                 void *ctx);

/**
 * @brief Get aggregate metrics across all sessions
 *
 * Calculates totals and averages across multiple sessions for reporting.
 *
 * @param filter Query filters (can be NULL for all sessions)
 * @param totals Output: aggregated totals (queries, tokens, etc.)
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_get_metrics_aggregate(const session_metrics_filter_t *filter,
                                  session_metrics_t *totals);

/**
 * @brief Delete old session metrics (retention cleanup)
 *
 * Deletes metrics older than the specified number of days.
 * Called automatically during auth_db_run_cleanup().
 *
 * @param retention_days Delete metrics older than this many days
 * @return Number of deleted entries, or -1 on error
 */
int auth_db_cleanup_session_metrics(int retention_days);

#endif /* AUTH_DB_H */
