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
#include <string.h>
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
 * @param count_out Output: number of users
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_user_count(int *count_out);

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
 * @param count_out Output: number of admin users
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_count_admins(int *count_out);

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
   char theme[AUTH_THEME_MAX];                      /**< UI color theme */
} auth_user_settings_t;

/**
 * @brief Get user settings
 *
 * Returns user-specific settings. If the user has no settings record,
 * returns default values (empty strings).
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

/* =============================================================================
 * User identity fields (v44)
 *
 * Stored on the users table (NOT user_settings — these are user-record
 * metadata, not settings the user actively tunes).  Used by:
 *   - WebUI Settings → User section (real_name, preferred_address,
 *     identity_aliases)
 *   - LLM system-prompt injection in memory_context.c
 *   - link-user-self synthetic-seed scoring in memory_db_alias.c
 *
 * identity_aliases is a newline-separated list parsed at use-site (split
 * on \n, strip whitespace, drop empties, dedupe case-insensitive).
 * ============================================================================= */
#define AUTH_REAL_NAME_MAX 128
#define AUTH_PREFERRED_ADDRESS_MAX 64
#define AUTH_IDENTITY_ALIASES_MAX 1024

typedef struct {
   char real_name[AUTH_REAL_NAME_MAX];
   char preferred_address[AUTH_PREFERRED_ADDRESS_MAX];
   char identity_aliases[AUTH_IDENTITY_ALIASES_MAX]; /**< newline-separated */
} auth_user_identity_t;

/**
 * @brief Get a user's identity fields (real_name, preferred_address,
 * identity_aliases) from the users table.  Empty strings indicate "unset"
 * (the row's column is NULL).
 *
 * @param user_id User ID
 * @param out     Output struct (zeroed before fill)
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int auth_db_get_user_identity(int user_id, auth_user_identity_t *out);

/**
 * @brief Set a user's identity fields.  Empty strings are persisted as
 * NULL (column reset).
 *
 * @param user_id User ID
 * @param id      Source struct
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int auth_db_set_user_identity(int user_id, const auth_user_identity_t *id);

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
 * @param deleted_out Output: number of sessions deleted (can be NULL)
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_delete_sessions_by_username(const char *username, int *deleted_out);

/**
 * @brief Delete all sessions for a user
 *
 * Used for password change or account lockout.
 *
 * @param user_id User ID
 * @param deleted_out Output: number of sessions deleted (can be NULL)
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_delete_user_sessions(int user_id, int *deleted_out);

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
 * @param count_out Output: number of active sessions
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_count_sessions(int *count_out);

/* ============================================================================
 * Rate Limiting
 * ============================================================================ */

/**
 * @brief Count recent failed login attempts from IP
 *
 * @param ip_address Client IP address
 * @param since Only count attempts after this timestamp
 * @param count_out Output: number of failed attempts
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_count_recent_failures(const char *ip_address, time_t since, int *count_out);

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
 * @param deleted_out Output: number of deleted entries (can be NULL)
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_clear_login_attempts(const char *ip_address, int *deleted_out);

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
 * Satellite Mappings
 * ============================================================================ */

/**
 * @brief Maximum satellite UUID length (including null terminator)
 */
#define SATELLITE_UUID_MAX 37

/**
 * @brief Maximum satellite name length
 */
#define SATELLITE_NAME_MAX 64

/**
 * @brief Maximum satellite location/HA area length
 */
#define SATELLITE_LOCATION_MAX 64

/**
 * @brief Reserved UUID representing the daemon's own local mic/speaker.
 *
 * Stored in satellite_mappings as a pseudo-satellite so the admin can assign
 * the daemon's local audio to a specific user via the normal satellite
 * management page. When unassigned (user_id NULL), local audio plays for
 * everyone (backward-compatible default). Tier is set to 0 to distinguish
 * from real Tier 1 (RPi) and Tier 2 (ESP32) devices.
 */
#define LOCAL_PSEUDO_SATELLITE_UUID "00000000-0000-0000-0000-000000000000"
#define LOCAL_PSEUDO_SATELLITE_TIER 0

/**
 * @brief True if the given UUID is the reserved local pseudo-satellite.
 */
static inline bool satellite_is_local_pseudo(const char *uuid) {
   return uuid && strcmp(uuid, LOCAL_PSEUDO_SATELLITE_UUID) == 0;
}

/**
 * @brief Persistent satellite-to-user mapping
 *
 * Stored in satellite_mappings table. Created on first registration,
 * updated by admin via WebUI. User mapping takes effect on next reconnect.
 */
typedef struct {
   char uuid[SATELLITE_UUID_MAX];
   char name[SATELLITE_NAME_MAX];
   char location[SATELLITE_LOCATION_MAX];
   char ha_area[SATELLITE_LOCATION_MAX];
   int user_id;
   int tier;
   bool enabled;
   time_t last_seen;
   time_t created_at;
} satellite_mapping_t;

/**
 * @brief Insert or update a satellite mapping
 *
 * On conflict (UUID exists), updates name, location, tier, last_seen.
 * Does NOT overwrite user_id, ha_area, or enabled (admin-managed fields).
 *
 * @param mapping Satellite mapping data
 * @return AUTH_DB_SUCCESS, AUTH_DB_INVALID, or AUTH_DB_FAILURE
 */
int satellite_db_upsert(const satellite_mapping_t *mapping);

/**
 * @brief Ensure the reserved local pseudo-satellite row exists.
 *
 * Called during init. Safe to call repeatedly — the upsert preserves
 * admin-managed fields (user_id, ha_area, enabled), so existing assignments
 * are not clobbered. If the row is missing, creates it unassigned and
 * enabled with tier=LOCAL_PSEUDO_SATELLITE_TIER.
 *
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int satellite_db_ensure_local_pseudo(void);

/**
 * @brief Decide whether the daemon's local speaker should play for this event.
 *
 * Consults the local pseudo-satellite mapping:
 *  - mapping disabled         → false (silent)
 *  - mapping unassigned       → true  (backward-compat: play for everyone)
 *  - assigned user matches    → true
 *  - assigned user mismatched → false
 *  - system events (user_id<=0) → true (always play)
 *  - any DB error             → true (fail open: never go silent on error)
 *
 * @param event_user_id  Event's user_id (0 for system events)
 * @return true if the local speaker should play
 */
bool satellite_local_speaker_plays_for_user(int event_user_id);

/**
 * @brief Get satellite mapping by UUID
 *
 * @param uuid Satellite UUID (36 chars)
 * @param out Buffer to receive mapping data
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int satellite_db_get(const char *uuid, satellite_mapping_t *out);

/**
 * @brief Delete a satellite mapping
 *
 * @param uuid Satellite UUID
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int satellite_db_delete(const char *uuid);

/**
 * @brief Update satellite user assignment
 *
 * @param uuid Satellite UUID
 * @param user_id New user ID (0 = unassociated)
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int satellite_db_update_user(const char *uuid, int user_id);

/**
 * @brief Update satellite location and HA area
 *
 * @param uuid Satellite UUID
 * @param location Room/location name
 * @param ha_area Home Assistant area (can be NULL/empty)
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int satellite_db_update_location(const char *uuid, const char *location, const char *ha_area);

/**
 * @brief Enable or disable a satellite
 *
 * Disabled satellites are rejected on registration.
 *
 * @param uuid Satellite UUID
 * @param enabled true to enable, false to disable
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int satellite_db_set_enabled(const char *uuid, bool enabled);

/**
 * @brief Update satellite last_seen timestamp to now
 *
 * Called only on registration events (not pings) to avoid write wear.
 *
 * @param uuid Satellite UUID
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int satellite_db_update_last_seen(const char *uuid);

/**
 * @brief List all satellite mappings (callback-based)
 *
 * @param callback Function called for each satellite
 * @param ctx User-provided context passed to callback
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int satellite_db_list(int (*callback)(const satellite_mapping_t *, void *), void *ctx);

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
   int context_tokens;               /**< Last known context token count */
   int context_max;                  /**< Context window size */
   int64_t continued_from;           /**< Parent conversation ID (0 = none) */
   char *compaction_summary;         /**< Summary from parent (NULL if not a continuation) */
   int64_t context_watermark_msg_id; /**< v67: last compacted msg id; 0 = none (load all) */
   /* Per-conversation LLM settings (v11) - empty string means use defaults */
   char llm_type[16];         /**< "local" or "cloud" */
   char cloud_provider[16];   /**< "openai" or "claude" */
   char model[64];            /**< Model name */
   char tools_mode[16];       /**< "native", "command_tags", or "disabled" */
   char thinking_mode[16];    /**< "disabled"/"auto"/"enabled" or "low"/"medium"/"high" */
   char reasoning_effort[16]; /**< "none"/"minimal"/"low"/"medium"/"high"/"xhigh" (v36) */
   bool is_private;           /**< If true, no memory extraction for this conversation (v16) */
   char origin[32];           /**< "webui" / "voice" (v17) / "briefing" /
                                   "messaging:<provider>" (e.g. "messaging:discord",
                                   "messaging:telegram" — needs > 16 bytes). */
   bool is_pinned;            /**< If true, pinned to the top of the WebUI list (v69) */
} conversation_t;

/**
 * @brief Conversation message structure
 */
typedef struct {
   int64_t id;
   int64_t conversation_id;
   char role[CONV_ROLE_MAX]; /**< "system", "user", "assistant", or "tool" */
   char *content;            /**< Borrowed column pointer; valid only during the callback */
   char *tool_calls;   /**< assistant rows: OpenAI tool_calls JSON array, else NULL (borrowed) */
   char *tool_call_id; /**< role='tool' rows: matching tool_call id, else NULL (borrowed) */
   char *reasoning;    /**< assistant rows: display-only reasoning JSON, else NULL (borrowed).
                            Delivered to the browser only — never read into the LLM context. */
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

/* =============================================================================
 * Background Job Accessors (v72/v73)
 *
 * A background job IS a parented conversation: its lifecycle lives in the job
 * columns on `conversations` (parent_id, spawn_mode, on_complete,
 * on_complete_fired, job_status, job_error, spawn_depth, reinvoke_count,
 * started_at, finished_at, deliver_to).  job_status IS NULL means "not a job".
 * These accessors are the read/write surface over those columns; the shared
 * conversation_t / conv_db_get read path is deliberately left untouched (the
 * job columns are NULL for ~every normal conversation).  See
 * docs/BACKGROUND_JOBS_DESIGN.md and src/core/job_manager.c.
 * ============================================================================= */

#define JOB_STATUS_MAX 16      /**< "interrupted" is the longest status */
#define JOB_SPAWN_MODE_MAX 16  /**< "detached" (v1) */
#define JOB_ON_COMPLETE_MAX 24 /**< "reinvoke_parent" (Phase 3) */
#define JOB_DELIVER_TO_MAX 64  /**< messaging channel display_name */
#define JOB_ERROR_MAX 256      /**< truncated failure reason */

/**
 * @brief A background job's lifecycle row (subset of `conversations` columns).
 */
typedef struct {
   int64_t id;        /**< conversation id of the job */
   int user_id;       /**< owner (inherited from the spawner, non-overridable) */
   int64_t parent_id; /**< spawning conversation id (0 = root/user-initiated) */
   char title[CONV_TITLE_MAX];
   char spawn_mode[JOB_SPAWN_MODE_MAX];
   char on_complete[JOB_ON_COMPLETE_MAX]; /**< "notify" | "none" (v1) */
   bool on_complete_fired;
   char job_status[JOB_STATUS_MAX]; /**< queued|running|done|failed|interrupted|cancelled */
   char job_error[JOB_ERROR_MAX];
   char deliver_to[JOB_DELIVER_TO_MAX]; /**< empty = local; else messaging channel */
   int spawn_depth;
   int reinvoke_count;
   time_t started_at;
   time_t finished_at;
   time_t created_at;
} job_record_t;

/**
 * @brief Create a background-job conversation (job_status='queued').
 *
 * The child inherits @p user_id (the caller never supplies a foreign user_id).
 *
 * @param user_id Owner (the spawner's user_id).
 * @param title Job title (NULL → default).
 * @param parent_id Spawning conversation id (0 = root).
 * @param spawn_mode "detached" (v1).
 * @param on_complete "notify" | "none".
 * @param deliver_to Messaging channel display_name, or NULL/"" for local delivery.
 * @param spawn_depth parent.spawn_depth + 1 (0 = root).
 * @param conv_id_out Receives the new job conversation id.
 * @return AUTH_DB_SUCCESS, AUTH_DB_LIMIT_EXCEEDED, or AUTH_DB_FAILURE.
 */
int conv_db_create_job(int user_id,
                       const char *title,
                       int64_t parent_id,
                       const char *spawn_mode,
                       const char *on_complete,
                       const char *deliver_to,
                       int spawn_depth,
                       const char *goal,
                       int64_t *conv_id_out);

/**
 * @brief Ownership-scoped: the instruction a job was created with.
 *
 * Allocates *out (caller frees) with the job's `job_goal`. Returns
 * AUTH_DB_NOT_FOUND when the row is absent, not owned by @p user_id, or has no
 * stored goal — the last case being a pre-v74 job that never dispatched, and so
 * has neither a goal column nor a first user message to recover one from.
 *
 * Deliberately not a job_record_t field: only the resume path needs it, and
 * job_record_t is bulk-copied in arrays for the WebUI job snapshot.
 */
int conv_db_job_get_goal(int64_t conv_id, int user_id, char **out);

/**
 * @brief System-caller: mark a job running and stamp started_at.
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int conv_db_job_set_running(int64_t conv_id, time_t started_at);

/**
 * @brief System-caller: set a terminal status + error + finished_at in one write.
 * @param status One of done|failed|interrupted|cancelled.
 * @param error_or_null Failure detail (truncated), or NULL.
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int conv_db_job_set_terminal(int64_t conv_id,
                             const char *status,
                             const char *error_or_null,
                             time_t finished_at);

/**
 * @brief System-caller: set on_complete_fired=1 (idempotency / follow-up guard).
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int conv_db_job_mark_fired(int64_t conv_id);

/**
 * @brief Ownership-checked: load a job's lifecycle record.
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND (not a job), AUTH_DB_FORBIDDEN, AUTH_DB_FAILURE.
 */
int conv_db_job_get(int64_t conv_id, int user_id, job_record_t *out);

/**
 * @brief Ownership-checked status probe (for the delete guard).
 *
 * Writes the empty string to @p status_out when the conversation exists but is
 * not a job (job_status IS NULL).
 *
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, AUTH_DB_FORBIDDEN, AUTH_DB_FAILURE.
 */
int conv_db_job_get_status(int64_t conv_id, int user_id, char *status_out, size_t n);

/**
 * @brief Ownership-scoped: list a user's jobs (newest first), up to @p max.
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int conv_db_job_list_by_user(int user_id, job_record_t *out, int max, int *count_out);

/**
 * @brief System-caller: jobs in a terminal state awaiting their follow-up
 *        (job_status IN (done,failed,interrupted) AND on_complete_fired=0).
 *
 * Copies up to @p max rows out under the DB lock so the caller can release it
 * before performing blocking delivery.
 *
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int conv_db_job_list_pending_followups(int max, job_record_t *out, int *count_out);

/**
 * @brief The parent's still-pending reinvoke_parent jobs (done/failed/interrupted,
 *        on_complete_fired=0, on_complete='reinvoke_parent'), ordered finished_at ASC.
 *
 * Re-queried at reinvoke DISPATCH time (not monitor time) so a coalesced
 * re-engagement picks up sibling jobs that finished while it was queued behind an
 * active turn.  Ownership-scoped to @p user_id.
 *
 * @return AUTH_DB_SUCCESS / AUTH_DB_INVALID / AUTH_DB_FAILURE.
 */
int conv_db_job_pending_reinvoke_for_parent(int64_t parent_id,
                                            int user_id,
                                            job_record_t *out,
                                            int max,
                                            int *count_out);

/**
 * @brief System-caller: all jobs still marked running/queued (boot scan +
 *        counter/queue rebuild).
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int conv_db_job_scan_active(job_record_t *out, int max, int *count_out);

/**
 * @brief System-caller: mark several jobs' on_complete_fired=1 in one transaction.
 *
 * Used by the reinvoke worker to claim a coalesced group atomically alongside
 * the reply persist (shrinks the crash-window that could double-deliver).  A
 * NULL/empty list is a no-op success.
 *
 * @return AUTH_DB_SUCCESS, AUTH_DB_INVALID (ids NULL with n>0), or AUTH_DB_FAILURE.
 */
int conv_db_job_mark_fired_many(const int64_t *ids, int n);

/**
 * @brief System-caller: increment a job's reinvoke_count (attempt counter).
 *
 * Bumped at each reinvoke ATTEMPT (not only on success) so a persistently
 * failing reinvoke trips the runaway ceiling instead of retrying forever.
 *
 * @param new_count_out Receives the post-increment value (may be NULL).
 * @return AUTH_DB_SUCCESS, AUTH_DB_INVALID (conv_id <= 0), or AUTH_DB_FAILURE.
 */
int conv_db_job_bump_reinvoke(int64_t conv_id, int *new_count_out);

/**
 * @brief Atomically claim an interrupted/failed job for re-dispatch.
 *
 * Resets the row to 'queued' and clears job_error, started_at, finished_at and
 * **on_complete_fired** — the last of which is what re-arms delivery, since the
 * interrupted-notify consumed it and the monitor skips a fired row.
 *
 * The resumable-status test and the owner test are both part of the UPDATE, so
 * this is a self-authorizing claim: exactly one of two concurrent callers
 * succeeds, and a foreign row cannot be claimed even if a caller forgot to check.
 *
 * @param allow_cancelled Admit 'cancelled' to the resumable set as well as
 *        'interrupted' and 'failed'.  True only on the human-driven WebUI path:
 *        restarting an interrupted job restores the user's intent, whereas
 *        restarting a cancelled one reverses it, which the model must not do on
 *        its own behalf.
 * @return AUTH_DB_SUCCESS (claimed), AUTH_DB_NOT_FOUND (absent, not owned by
 *         @p user_id, not a job, or not in a resumable state), AUTH_DB_INVALID,
 *         or AUTH_DB_FAILURE.
 */
int conv_db_job_reset_for_resume(int64_t conv_id, int user_id, bool allow_cancelled);

/**
 * @brief Ownership-scoped: a user's ACTIVE (queued/running) jobs, oldest first.
 *
 * A complete SET, not a page.  job_manager gates every reservation on the global
 * `max_active_jobs` (clamped <= 256) before the per-user cap, so this result is
 * structurally bounded and cannot truncate in practice — which is what makes it
 * safe to derive per-parent "jobs running" counts from these rows.  Pair with
 * conv_db_job_list_history_by_user(); see the block comment in auth_db_jobs.c.
 *
 * @return AUTH_DB_SUCCESS, AUTH_DB_INVALID, or AUTH_DB_FAILURE.
 */
int conv_db_job_list_active_by_user(int user_id, job_record_t *out, int max, int *count_out);

/**
 * @brief Ownership-scoped: the DIRECT job children of a parent conversation.
 *
 * A batch of up to @p max children (any status), ordered by id.  Single level —
 * job->job spawning is blocked today, so a direct child has no children of its
 * own.  Used by the parent-delete cascade: call in a loop, deleting the returned
 * rows, until it returns 0.
 *
 * @return AUTH_DB_SUCCESS, AUTH_DB_INVALID, or AUTH_DB_FAILURE.
 */
int conv_db_job_list_children(int64_t parent_id,
                              int user_id,
                              job_record_t *out,
                              int max,
                              int *count_out);

/**
 * @brief Ownership-scoped: a user's TERMINAL jobs, newest first, keyset-paginated.
 *
 * Unbounded feed — never derive a count or a "how many are running" answer from
 * a page of this.  Pass @p before_created_at == 0 for the first page; afterwards
 * pass the (created_at, id) of the last row received to get the next page.
 *
 * @return AUTH_DB_SUCCESS, AUTH_DB_INVALID, or AUTH_DB_FAILURE.
 */
int conv_db_job_list_history_by_user(int user_id,
                                     int64_t before_created_at,
                                     int64_t before_id,
                                     job_record_t *out,
                                     int max,
                                     int *count_out);

/**
 * @brief Ownership-scoped: copy a job's last assistant message text (its result).
 *
 * Allocates *out (caller frees) with the content of the most recent
 * role='assistant' message in the job conversation, or sets *out=NULL when the
 * job produced no assistant message.  The conversation is ownership-JOINed to
 * @p user_id.
 *
 * A conversation not owned by @p user_id (or with no assistant message) simply
 * yields *out=NULL — the ownership filter is the SQL JOIN, so there is no
 * distinct FORBIDDEN result.
 *
 * @return AUTH_DB_SUCCESS (even when *out is NULL), AUTH_DB_INVALID, or AUTH_DB_FAILURE.
 */
int conv_db_job_last_assistant_text(int64_t conv_id, int user_id, char **out);

/* =============================================================================
 * Conversation event log (background-jobs Phase 2 — the observe half)
 *
 * Step-granular, NEVER token-granular: live tokens live only in the in-memory
 * conv_stream replay ring, and final assistant text stays in `messages`.  This
 * table is what makes a background job inspectable — and what a TUI tails.
 *
 * `seq` is per-conversation monotonic, assigned MAX(seq)+1 inside the insert so
 * it is atomic under the auth_db write lock; the v72 UNIQUE(conversation_id,seq)
 * index is a backstop, not the mechanism.  There is no user_id column (mirrors
 * `messages`): every read JOINs `conversations` and filters user_id
 * (§8.3) — never a bare WHERE conversation_id=?.
 *
 * Payloads are secret-redacted and size-capped BEFORE they arrive here (§8.6);
 * this layer stores what it is given.  See docs/BACKGROUND_JOBS_DESIGN.md §6/§8.
 * ============================================================================= */

#define CONV_EVENT_KIND_MAX 16 /**< "terminal_chunk" is the longest kind */

/** One durable step in a conversation's event log. */
typedef struct {
   int64_t id;
   int64_t conversation_id;
   int64_t seq;                    /**< per-conversation monotonic */
   char kind[CONV_EVENT_KIND_MAX]; /**< status|tool_call|tool_result|
                                    *   terminal_chunk|spawn|complete */
   char *payload;                  /**< heap, caller frees; NULL once pruned */
   time_t created_at;
} conv_event_t;

/**
 * @brief Append one event, assigning the next per-conversation seq atomically.
 *
 * @param conv_id   Conversation the event belongs to.
 * @param kind      One of the §6.2 kinds (not validated here; keep the set tight).
 * @param payload   Kind-specific JSON, already redacted + capped. May be NULL.
 * @param seq_out   Receives the assigned seq (may be NULL).
 * @return AUTH_DB_SUCCESS, AUTH_DB_INVALID, or AUTH_DB_FAILURE.
 */
int conv_db_event_append(int64_t conv_id, const char *kind, const char *payload, int64_t *seq_out);

/**
 * @brief Ownership-scoped: events for a conversation with seq > @p after_seq, ASC.
 *
 * The ownership filter is the SQL JOIN on conversations.user_id, so a foreign
 * conv_id yields zero rows rather than a distinct FORBIDDEN result — same
 * contract as conv_db_get_messages.  Caller frees each row's `payload`.
 *
 * @param after_seq Exclusive lower bound (0 replays the whole log).
 * @return AUTH_DB_SUCCESS, AUTH_DB_INVALID, or AUTH_DB_FAILURE.
 */
int conv_db_event_list(int64_t conv_id,
                       int user_id,
                       int64_t after_seq,
                       int max,
                       conv_event_t *out,
                       int *count_out);

/** @brief Free the heap members of @p n rows filled by conv_db_event_list. */
void conv_db_event_rows_free(conv_event_t *rows, int n);

/**
 * @brief Kind-aware retention sweep (§8.8).
 *
 * Payload-nulls aged `tool_call`/`tool_result`/`terminal_chunk` rows (keeping the
 * row so the seq chain — and therefore replay ordering — stays coherent) and
 * DELETEs aged `status` rows outright, since for those the payload *is* the
 * content and seq gaps are harmless (reads are `seq > last_seq`; nothing needs
 * density).  `spawn`/`complete` are kept intact: they are the tree skeleton and
 * the disposition.
 *
 * @param retention_days Age threshold; <= 0 disables the sweep entirely.
 * @param nulled_out     Payloads cleared (may be NULL).
 * @param deleted_out    Rows removed (may be NULL).
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE.
 */
int conv_db_event_prune_expired(int retention_days, int *nulled_out, int *deleted_out);

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
 * @brief Pin or unpin a conversation
 *
 * Pinned conversations float to a dedicated section at the top of the WebUI
 * conversation list.
 *
 * @param conv_id Conversation ID
 * @param user_id User ID (for authorization check)
 * @param is_pinned True to pin, false to unpin
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, AUTH_DB_INVALID, or AUTH_DB_FAILURE
 */
int conv_db_set_pinned(int64_t conv_id, int user_id, bool is_pinned);

/**
 * @brief Check if a conversation is private
 *
 * Lightweight query that only checks the is_private flag without loading
 * the full conversation. Used by memory extraction to re-verify privacy
 * status from the database (prevents race conditions).
 *
 * @param conv_id Conversation ID
 * @param user_id User ID (for authorization check)
 * @param is_private_out Output: true if private, false if not private
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int conv_db_is_private(int64_t conv_id, int user_id, bool *is_private_out);

/**
 * Sentinel value for "no anchor recorded" on conversations.anchor_date.
 * Legacy rows pre-v42, non-conversation extraction paths, and any code path
 * that hasn't recorded an anchor all surface as this value.  Extraction code
 * skips the "Conversation anchor:" prompt line when the value equals this.
 */
#define ANCHOR_DATE_NONE ((int64_t)0)

/**
 * @brief Get the conversation's anchor date (v42)
 *
 * The anchor_date column carries the conversation's logical "now" timestamp —
 * what the LLM treats as the present moment when resolving relative temporal
 * phrases ("yesterday", "last month", "five years ago") during memory
 * extraction.  Production writers populate it at insert time with
 * time(NULL); the bench harness passes parsed session dates.
 *
 * @param conv_id Conversation ID
 * @param anchor_out Output: epoch seconds, or ANCHOR_DATE_NONE if no anchor recorded
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int conv_db_get_anchor_date(int64_t conv_id, int64_t *anchor_out);

/**
 * @brief Fetch the conversation's created_at timestamp.
 *
 * Used by memory extraction to time-stamp derived records (facts,
 * summaries) with the source conversation's creation time rather than
 * "now".  This keeps the natural temporal spread that recency-based
 * retrieval relies on intact across a full `dawn-admin memory reextract`
 * — otherwise every record in the corpus collapses into the reextract
 * window and recency-ordered LIMITs / weight_recency tiebreaks all
 * degenerate.
 *
 * @param conv_id Conversation ID
 * @param created_at_out Output: epoch seconds; 0 if conv has no created_at
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int conv_db_get_created_at(int64_t conv_id, int64_t *created_at_out);

/**
 * @brief Force-set the conversation's anchor date (v42; bench-only, unsafe)
 *
 * Production must NEVER call this — anchor_date is populated once at insert
 * time and is meant to remain immutable thereafter.  Calling this after
 * extraction has already run against the conversation will desync extracted
 * facts (which were resolved against the prior anchor) from the new anchor.
 *
 * Authorization: requires user_id and verifies ownership in the WHERE clause,
 * matching conv_db_is_private's pattern.  This guards against the future-misuse
 * case where this function is wired to a user-facing endpoint by a contributor
 * who didn't read the doxygen warnings.
 *
 * The only legitimate caller is the bench harness, which uses this to override
 * the default time(NULL) anchor with a LoCoMo session_X_date_time value before
 * triggering extraction.
 *
 * @param conv_id Conversation ID
 * @param user_id User ID (must own the conversation)
 * @param anchor Epoch seconds; pass ANCHOR_DATE_NONE to clear (extraction will
 *               then omit the prompt anchor line)
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, AUTH_DB_FORBIDDEN, or AUTH_DB_FAILURE
 */
int conv_db_force_anchor_date_unsafe(int64_t conv_id, int user_id, int64_t anchor);

/**
 * @brief Auto-title a conversation (atomic check-and-set)
 *
 * Sets the title only if title_locked = 0 (first extraction or never manually renamed).
 * On success, also sets title_locked = 1 to prevent future overwrites.
 * Single SQL round-trip eliminates TOCTOU race conditions.
 *
 * @param conv_id Conversation ID
 * @param user_id User ID (for authorization check)
 * @param title New title (should be <= 40 UTF-8 bytes)
 * @return AUTH_DB_SUCCESS if updated, AUTH_DB_NOT_FOUND if already locked or not found
 */
int conv_db_auto_title(int64_t conv_id, int user_id, const char *title);

/**
 * @brief Set or clear the title_locked flag on a conversation
 *
 * Used by manual rename to prevent future auto-title overwrites.
 *
 * @param conv_id Conversation ID
 * @param user_id User ID (for authorization check)
 * @param locked 1 to lock, 0 to unlock
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int conv_db_set_title_locked(int64_t conv_id, int user_id, int locked);

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
 * @brief Persist a compaction watermark + summary on a conversation (v67).
 *
 * Replaces fork-on-compaction: records @watermark_msg_id (the last compacted
 * message id) and @summary on the SAME conversation row. Reload then bounds
 * context to messages with id > watermark + the summary. Single atomic UPDATE
 * with a monotonic guard (a stale watermark <= the stored one is a no-op).
 *
 * @param conv_id Conversation id (> 0).
 * @param user_id Owner id (ownership-checked in the UPDATE).
 * @param summary Latest compaction summary (may be NULL).
 * @param watermark_msg_id Last compacted message id (> 0; <= 0 returns AUTH_DB_INVALID).
 * @return AUTH_DB_SUCCESS (incl. benign no-op), AUTH_DB_INVALID, or AUTH_DB_FAILURE.
 */
int conv_db_set_compaction_watermark(int64_t conv_id,
                                     int user_id,
                                     const char *summary,
                                     int64_t watermark_msg_id);

/**
 * @brief Format the reload context line for a (watermarked) conversation.
 *
 * Writes a `[COMPACTED conv=N msgs=X-Y node=Z depth=D] Previous conversation
 * context (summarized): <summary>` marker into @out when a summary node exists
 * (so a reloaded LLM keeps a context_expand handle to the compacted originals),
 * else a plain summary line. @out is always NUL-terminated. Empty @summary
 * yields an empty string.
 *
 * @param conv_id Conversation id (for summary-node lookup + the marker).
 * @param summary The conversation's compaction_summary text (may be NULL).
 * @param out Output buffer.
 * @param out_len Size of @out.
 */
void conv_db_format_compaction_context(int64_t conv_id,
                                       const char *summary,
                                       char *out,
                                       size_t out_len);

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
                              const char *thinking_mode,
                              const char *reasoning_effort);

/**
 * @brief Update LLM settings on an existing conversation (any message count).
 *
 * Unlike conv_db_lock_llm_settings, this works regardless of message_count.
 * Called when the user changes model/provider mid-conversation so the DB
 * always reflects the last-used LLM config.
 */
int conv_db_update_llm_settings(int64_t conv_id,
                                int user_id,
                                const char *llm_type,
                                const char *cloud_provider,
                                const char *model,
                                const char *tools_mode,
                                const char *thinking_mode,
                                const char *reasoning_effort);

/**
 * @brief Fill only the LLM-settings columns that are currently NULL/empty.
 *
 * Unlike conv_db_lock_llm_settings (message_count==0 gate, overwrites) and
 * conv_db_update_llm_settings (overwrites unconditionally), this per-column
 * fill-if-empty never clobbers an existing value and has no message_count
 * dependency — making it race-proof against the first-turn message persist. Used
 * to stamp a fresh conversation with the session's resolved config at creation so
 * no conversation persists NULL LLM columns.
 *
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND (no matching row), or AUTH_DB_FAILURE
 */
int conv_db_fill_llm_settings_if_empty(int64_t conv_id,
                                       int user_id,
                                       const char *llm_type,
                                       const char *cloud_provider,
                                       const char *model,
                                       const char *tools_mode,
                                       const char *thinking_mode,
                                       const char *reasoning_effort);

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
 * @brief Add a message and return its assigned row ID.
 *
 * Same as conv_db_add_message() but writes the inserted message's row ID to
 * *msg_id_out on success (set to 0 on failure or when msg_id_out is NULL).
 * Use this when the caller needs to stamp the ID into an in-memory history entry.
 */
int conv_db_add_message_ex(int64_t conv_id,
                           int user_id,
                           const char *role,
                           const char *content,
                           int64_t *msg_id_out);

/**
 * @brief Add a message with structured tool fields (OpenAI-canonical).
 *
 * Like conv_db_add_message_ex() but also persists the structured tool columns:
 * @p tool_calls (assistant rows — the OpenAI tool_calls JSON array), @p tool_call_id
 * (role='tool' rows — the matching call id), and @p reasoning (assistant rows —
 * display-only reasoning JSON; never read into the LLM context). Pass NULL for any
 * when not applicable; conv_db_add_message_ex() delegates here with all NULL.
 */
int conv_db_add_message_with_tools(int64_t conv_id,
                                   int user_id,
                                   const char *role,
                                   const char *content,
                                   const char *tool_calls,
                                   const char *tool_call_id,
                                   const char *reasoning,
                                   int64_t *msg_id_out);

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
 * @brief Like conv_db_get_messages but only messages with id > @after_id.
 *
 * The compaction-watermark restore path (v67): load only post-watermark messages
 * into the LLM context. Same full column set / ownership check / chronological
 * order as conv_db_get_messages. @after_id = 0 returns all messages.
 *
 * @param conv_id Conversation ID
 * @param user_id User ID (for authorization check)
 * @param after_id Exclusive lower bound on message id (0 = all)
 * @param callback Function called for each message
 * @param ctx User-provided context passed to callback
 * @return AUTH_DB_SUCCESS, AUTH_DB_INVALID, or AUTH_DB_FAILURE
 */
int conv_db_get_messages_after(int64_t conv_id,
                               int user_id,
                               int64_t after_id,
                               message_callback_t callback,
                               void *ctx);

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
 * @brief Get message IDs for a conversation (ordered by creation)
 *
 * Returns an array of message database IDs. Used by LCM Phase 3 to map
 * in-memory array indices to DB IDs at compaction time.
 *
 * @param conv_id Conversation ID
 * @param user_id User ID (for ownership check)
 * @param ids_out Output: heap-allocated array of message IDs (caller frees)
 * @param count_out Output: number of IDs in the array
 * @return AUTH_DB_SUCCESS, AUTH_DB_FORBIDDEN, or AUTH_DB_FAILURE
 */
int conv_db_get_message_ids(int64_t conv_id, int user_id, int64_t **ids_out, int *count_out);

/**
 * @brief Get messages by ID range (for context expansion).
 *
 * Same as conv_db_get_messages but filtered to a specific ID range.
 *
 * Privacy: by default, the SQL JOIN excludes rows from `is_private = 1`
 * conversations as defense-in-depth — even if an upstream caller forgets to
 * filter (e.g. the provenance source-fetch path), private content cannot
 * surface here.  Set `include_private = true` only when the caller is
 * operating on the user's own active session (e.g. the context_expand tool
 * resolving a `[COMPACTED ...]` block within a private conversation the
 * user is currently chatting in).  Ownership (user_id match) is enforced
 * regardless.
 *
 * @param conv_id Conversation ID
 * @param user_id User ID (for ownership check)
 * @param start_id First message ID (inclusive)
 * @param end_id Last message ID (inclusive)
 * @param include_private If false (default for memory/provenance callers),
 *                        rows whose conversation has `is_private = 1` are
 *                        suppressed in SQL.  If true, ownership-only.
 * @param callback Function called for each message
 * @param ctx User context passed to callback
 * @return AUTH_DB_SUCCESS, AUTH_DB_FORBIDDEN, or AUTH_DB_FAILURE
 */
int conv_db_get_messages_by_range(int64_t conv_id,
                                  int user_id,
                                  int64_t start_id,
                                  int64_t end_id,
                                  bool include_private,
                                  message_callback_t callback,
                                  void *ctx);

/**
 * @brief Return MAX(messages.id) for a conversation (v40).
 *
 * Used by the extraction thread to compute the provenance source range.
 * Returns 0 in max_id_out if the conversation has no messages.
 *
 * @param conv_id Conversation ID
 * @param user_id User ID (for ownership check)
 * @param max_id_out Output: highest message ID (0 if no messages)
 * @return AUTH_DB_SUCCESS, AUTH_DB_FORBIDDEN, or AUTH_DB_FAILURE
 */
int conv_db_get_max_msg_id(int64_t conv_id, int user_id, int64_t *max_id_out);

/* =============================================================================
 * Summary Nodes (LCM Phase 4 — hierarchical summaries)
 * ============================================================================= */

typedef struct {
   int64_t id;
   int64_t conversation_id;
   int64_t prior_node_id;
   int depth;
   int64_t msg_id_start;
   int64_t msg_id_end;
   int level;
   char *summary_text;
   int token_count;
   time_t created_at;
} summary_node_t;

/**
 * @brief Create a summary node after compaction
 *
 * @param node Node data (id field is ignored, set on output)
 * @param node_id_out Output: inserted node ID
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int summary_node_create(const summary_node_t *node, int64_t *node_id_out);

/**
 * @brief Get a summary node by ID
 *
 * @param node_id Node ID
 * @param node_out Output: node data (summary_text is heap-allocated, caller frees)
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int summary_node_get(int64_t node_id, summary_node_t *node_out);

/**
 * @brief Get the most recent summary node for a conversation
 *
 * Queries summary_nodes for the given conversation ID only.
 * The caller (llm_context_compact) handles continuation chain
 * traversal via continued_from if no node is found.
 *
 * @param conv_id Conversation ID to query
 * @param node_out Output: latest node (summary_text is heap-allocated, caller frees)
 * @return AUTH_DB_SUCCESS, AUTH_DB_NOT_FOUND, or AUTH_DB_FAILURE
 */
int summary_node_get_latest(int64_t conv_id, summary_node_t *node_out);

/**
 * @brief Free heap-allocated fields in a summary_node_t
 */
void summary_node_free(summary_node_t *node);

/**
 * @brief Count conversations for a user
 *
 * @param user_id User ID
 * @param count_out Output: number of conversations
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int conv_db_count(int user_id, int *count_out);

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
 * @param deleted_out Output: number of deleted entries (can be NULL)
 * @return AUTH_DB_SUCCESS or AUTH_DB_FAILURE
 */
int auth_db_cleanup_session_metrics(int retention_days, int *deleted_out);

#endif /* AUTH_DB_H */
