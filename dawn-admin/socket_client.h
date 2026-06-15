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
 * Socket client for dawn-admin CLI to communicate with Dawn daemon.
 */

#ifndef DAWN_ADMIN_SOCKET_CLIENT_H
#define DAWN_ADMIN_SOCKET_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "auth/admin_socket.h"

/**
 * @brief Connect to the Dawn admin socket.
 *
 * Attempts to connect to the Dawn daemon's admin socket using the
 * abstract socket namespace (Linux) or filesystem socket fallback.
 *
 * @return Socket file descriptor on success, -1 on failure.
 */
int admin_client_connect(void);

/**
 * @brief Disconnect from the admin socket.
 *
 * @param fd Socket file descriptor to close.
 */
void admin_client_disconnect(int fd);

/**
 * @brief Send a ping message to verify daemon connectivity.
 *
 * @param fd Socket file descriptor.
 *
 * @return 0 on success (daemon responded), non-zero on failure.
 */
int admin_client_ping(int fd);

/**
 * @brief Validate a setup token with the daemon.
 *
 * @param fd    Socket file descriptor.
 * @param token The setup token to validate (DAWN-XXXX-XXXX-XXXX-XXXX format).
 *
 * @return Response code from daemon (ADMIN_RESP_SUCCESS on success).
 */
admin_resp_code_t admin_client_validate_token(int fd, const char *token);

/**
 * @brief Create a user account (atomic token validation + user creation).
 *
 * This combines setup token validation and user creation into a single
 * atomic operation to prevent race conditions.
 *
 * @param fd       Socket file descriptor.
 * @param token    The setup token (DAWN-XXXX-XXXX-XXXX-XXXX format).
 * @param username Username for the new account.
 * @param password Password for the new account.
 * @param is_admin Whether the user should have admin privileges.
 *
 * @return Response code from daemon (ADMIN_RESP_SUCCESS on success).
 */
admin_resp_code_t admin_client_create_user(int fd,
                                           const char *token,
                                           const char *username,
                                           const char *password,
                                           bool is_admin);

/**
 * @brief User entry from list response.
 */
typedef struct {
   int id;
   char username[64];
   bool is_admin;
   bool is_locked;
   int failed_attempts;
} admin_user_entry_t;

/**
 * @brief Session entry from list response.
 */
typedef struct {
   char token_prefix[9];
   char username[64];
   int64_t created_at;
   int64_t last_activity;
   char ip_address[64];
} admin_session_entry_t;

/**
 * @brief Callback for user list enumeration.
 */
typedef int (*admin_user_callback_t)(const admin_user_entry_t *user, void *ctx);

/**
 * @brief Callback for session list enumeration.
 */
typedef int (*admin_session_callback_t)(const admin_session_entry_t *session, void *ctx);

/**
 * @brief List all users.
 *
 * @param fd       Socket file descriptor.
 * @param callback Function called for each user.
 * @param ctx      User context passed to callback.
 *
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_list_users(int fd, admin_user_callback_t callback, void *ctx);

/**
 * @brief Delete a user (requires admin auth).
 *
 * @param fd             Socket file descriptor.
 * @param admin_user     Admin username for authorization.
 * @param admin_password Admin password for authorization.
 * @param target_user    Username to delete.
 *
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_delete_user(int fd,
                                           const char *admin_user,
                                           const char *admin_password,
                                           const char *target_user);

/**
 * @brief Change a user's password (requires admin auth).
 *
 * @param fd             Socket file descriptor.
 * @param admin_user     Admin username for authorization.
 * @param admin_password Admin password for authorization.
 * @param target_user    Username whose password to change.
 * @param new_password   New password.
 *
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_change_password(int fd,
                                               const char *admin_user,
                                               const char *admin_password,
                                               const char *target_user,
                                               const char *new_password);

/**
 * @brief Unlock a locked user account (requires admin auth).
 *
 * @param fd             Socket file descriptor.
 * @param admin_user     Admin username for authorization.
 * @param admin_password Admin password for authorization.
 * @param target_user    Username to unlock.
 *
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_unlock_user(int fd,
                                           const char *admin_user,
                                           const char *admin_password,
                                           const char *target_user);

/**
 * @brief List all active sessions.
 *
 * @param fd       Socket file descriptor.
 * @param callback Function called for each session.
 * @param ctx      User context passed to callback.
 *
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_list_sessions(int fd, admin_session_callback_t callback, void *ctx);

/**
 * @brief Revoke a session by token prefix (requires admin auth).
 *
 * @param fd             Socket file descriptor.
 * @param admin_user     Admin username for authorization.
 * @param admin_password Admin password for authorization.
 * @param token_prefix   8-character token prefix to revoke.
 *
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_revoke_session(int fd,
                                              const char *admin_user,
                                              const char *admin_password,
                                              const char *token_prefix);

/**
 * @brief Revoke all sessions for a user (requires admin auth).
 *
 * @param fd             Socket file descriptor.
 * @param admin_user     Admin username for authorization.
 * @param admin_password Admin password for authorization.
 * @param target_user    Username whose sessions to revoke.
 *
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_revoke_user_sessions(int fd,
                                                    const char *admin_user,
                                                    const char *admin_password,
                                                    const char *target_user);

/**
 * @brief Database statistics (matches auth_db_stats_t).
 */
typedef struct {
   int user_count;
   int admin_count;
   int session_count;
   int locked_user_count;
   int failed_attempts_24h;
   int audit_log_count;
   int64_t db_size_bytes;
} admin_db_stats_t;

/**
 * @brief Get database statistics.
 *
 * @param fd    Socket file descriptor.
 * @param stats Pointer to stats structure to populate.
 *
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_get_stats(int fd, admin_db_stats_t *stats);

/**
 * @brief Compact the database (requires admin auth).
 *
 * Rate-limited to once per 24 hours.
 *
 * @param fd             Socket file descriptor.
 * @param admin_user     Admin username for authorization.
 * @param admin_password Admin password for authorization.
 *
 * @return Response code from daemon (ADMIN_RESP_RATE_LIMITED if too soon).
 */
admin_resp_code_t admin_client_db_compact(int fd,
                                          const char *admin_user,
                                          const char *admin_password);

/**
 * @brief Backup the database (requires admin auth).
 *
 * @param fd             Socket file descriptor.
 * @param admin_user     Admin username for authorization.
 * @param admin_password Admin password for authorization.
 * @param dest_path      Destination file path.
 *
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_db_backup(int fd,
                                         const char *admin_user,
                                         const char *admin_password,
                                         const char *dest_path);

/**
 * @brief Audit log entry from query response.
 */
typedef struct {
   int64_t timestamp;
   char event[32];
   char username[64];
   char ip_address[64];
   char details[256];
} admin_log_entry_t;

/**
 * @brief Callback for audit log enumeration.
 */
typedef int (*admin_log_callback_t)(const admin_log_entry_t *entry, void *ctx);

/**
 * @brief Audit log query filter.
 */
typedef struct {
   int64_t since;        /**< Only entries after this time (0 = no limit) */
   int64_t until;        /**< Only entries before this time (0 = no limit) */
   const char *event;    /**< Filter by event type (NULL = all) */
   const char *username; /**< Filter by username (NULL = all) */
   int limit;            /**< Max entries to return (0 = default 100) */
   int offset;           /**< Skip first N entries (for pagination) */
} admin_log_filter_t;

/**
 * @brief Query audit log with optional filters.
 *
 * @param fd       Socket file descriptor.
 * @param filter   Query filters (can be NULL for defaults).
 * @param callback Function called for each matching entry.
 * @param ctx      User context passed to callback.
 *
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_query_log(int fd,
                                         const admin_log_filter_t *filter,
                                         admin_log_callback_t callback,
                                         void *ctx);

/**
 * @brief IP status entry from list response.
 */
typedef struct {
   char ip_address[64];
   int failed_attempts;
   int64_t last_attempt;
} admin_ip_entry_t;

/**
 * @brief Callback for blocked IP list enumeration.
 */
typedef int (*admin_ip_callback_t)(const admin_ip_entry_t *entry, void *ctx);

/**
 * @brief List IPs with failed login attempts in the rate limit window.
 *
 * @param fd       Socket file descriptor.
 * @param callback Function called for each IP.
 * @param ctx      User context passed to callback.
 *
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_list_blocked_ips(int fd, admin_ip_callback_t callback, void *ctx);

/**
 * @brief Unblock an IP address by clearing its login attempts (requires admin auth).
 *
 * @param fd             Socket file descriptor.
 * @param admin_user     Admin username for authorization.
 * @param admin_password Admin password for authorization.
 * @param ip_address     IP address to unblock, or "--all" to clear all.
 *
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_unblock_ip(int fd,
                                          const char *admin_user,
                                          const char *admin_password,
                                          const char *ip_address);

/**
 * @brief Get a human-readable error message for a response code.
 *
 * @param code Response code from daemon.
 *
 * @return Static string describing the error.
 */
const char *admin_resp_strerror(admin_resp_code_t code);

/* =============================================================================
 * Phase 3: Session Metrics
 * =============================================================================
 */

/**
 * @brief Session metrics entry from list response.
 */
typedef struct {
   int64_t id;
   uint32_t session_id;
   int user_id;
   char session_type[16];
   int64_t started_at;
   int64_t ended_at;
   uint32_t queries_total;
   uint32_t queries_cloud;
   uint32_t queries_local;
   uint32_t errors_count;
   double avg_llm_total_ms;
} admin_metrics_entry_t;

/**
 * @brief Callback for metrics list enumeration.
 */
typedef int (*admin_metrics_callback_t)(const admin_metrics_entry_t *entry, void *ctx);

/**
 * @brief Metrics query filter.
 */
typedef struct {
   int user_id;      /**< Filter by user (0 = all) */
   const char *type; /**< Filter by session type (NULL = all) */
   int limit;        /**< Max entries to return (0 = default 20) */
} admin_metrics_filter_t;

/**
 * @brief List session metrics history.
 *
 * @param fd       Socket file descriptor.
 * @param filter   Query filters (can be NULL for defaults).
 * @param callback Function called for each metrics entry.
 * @param ctx      User context passed to callback.
 *
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_list_metrics(int fd,
                                            const admin_metrics_filter_t *filter,
                                            admin_metrics_callback_t callback,
                                            void *ctx);

/**
 * @brief Aggregate metrics totals.
 */
typedef struct {
   int session_count;
   uint64_t queries_total;
   uint64_t queries_cloud;
   uint64_t queries_local;
   uint64_t errors_total;
   double avg_llm_ms;
} admin_metrics_totals_t;

/**
 * @brief Get aggregate metrics totals.
 *
 * @param fd       Socket file descriptor.
 * @param filter   Query filters (can be NULL for all sessions).
 * @param totals   Output: aggregated totals.
 *
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_get_metrics_totals(int fd,
                                                  const admin_metrics_filter_t *filter,
                                                  admin_metrics_totals_t *totals);

/* =============================================================================
 * Phase 4: Conversation Management
 * =============================================================================
 */

/**
 * @brief Conversation list entry.
 */
typedef struct {
   int64_t id;
   char title[128];
   int64_t created_at;
   int64_t updated_at;
   int message_count;
   char username[64];
} admin_conversation_entry_t;

/**
 * @brief Callback for conversation list enumeration.
 */
typedef int (*admin_conversation_callback_t)(const admin_conversation_entry_t *conv, void *ctx);

/**
 * @brief Conversation query filter.
 */
typedef struct {
   int user_id;           /**< Filter by user (0 = all, requires admin) */
   int limit;             /**< Max entries to return (0 = default 20) */
   bool include_archived; /**< Include archived conversations */
} admin_conversation_filter_t;

/**
 * @brief List conversations.
 *
 * @param fd       Socket file descriptor.
 * @param filter   Query filters.
 * @param callback Function called for each conversation.
 * @param ctx      User context passed to callback.
 *
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_list_conversations(int fd,
                                                  const admin_conversation_filter_t *filter,
                                                  admin_conversation_callback_t callback,
                                                  void *ctx);

/**
 * @brief Message entry from conversation.
 */
typedef struct {
   char role[16];
   char content[ADMIN_MSG_CONTENT_MAX + 1];
   int64_t created_at;
} admin_message_entry_t;

/**
 * @brief Callback for message enumeration.
 */
typedef int (*admin_message_callback_t)(const admin_message_entry_t *msg, void *ctx);

/**
 * @brief Get a conversation with its messages.
 *
 * @param fd       Socket file descriptor.
 * @param conv_id  Conversation ID.
 * @param callback Function called for each message.
 * @param ctx      User context passed to callback.
 *
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_get_conversation(int fd,
                                                int64_t conv_id,
                                                admin_message_callback_t callback,
                                                void *ctx);

/**
 * @brief Delete a conversation (requires admin auth).
 *
 * @param fd             Socket file descriptor.
 * @param admin_user     Admin username for authorization.
 * @param admin_password Admin password for authorization.
 * @param conv_id        Conversation ID to delete.
 *
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_delete_conversation(int fd,
                                                   const char *admin_user,
                                                   const char *admin_password,
                                                   int64_t conv_id);

/* =============================================================================
 * Phase 5: Music Database
 * =============================================================================
 */

/**
 * @brief Get music database statistics.
 *
 * @param fd       Socket file descriptor.
 * @param response Output buffer for response text.
 * @param resp_len Size of response buffer.
 *
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_music_stats(int fd, char *response, size_t resp_len);

/**
 * @brief Search music database by query.
 *
 * @param fd       Socket file descriptor.
 * @param query    Search query string.
 * @param response Output buffer for response text.
 * @param resp_len Size of response buffer.
 *
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_music_search(int fd,
                                            const char *query,
                                            char *response,
                                            size_t resp_len);

/**
 * @brief List tracks in music database.
 *
 * @param fd       Socket file descriptor.
 * @param limit    Max tracks to return (0 = default).
 * @param response Output buffer for response text.
 * @param resp_len Size of response buffer.
 *
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_music_list(int fd, int limit, char *response, size_t resp_len);

/**
 * @brief Trigger immediate music library rescan.
 *
 * @param fd       Socket file descriptor.
 * @param response Output buffer for response text.
 * @param resp_len Size of response buffer.
 *
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_music_rescan(int fd, char *response, size_t resp_len);

/**
 * @brief Trigger LLM recategorization of general facts for a user.
 *
 * @param fd Connected socket FD.
 * @param username Username whose facts to recategorize.
 * @param response Output buffer for daemon response text.
 * @param resp_len Size of response buffer.
 *
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_memory_recategorize(int fd,
                                                   const char *username,
                                                   char *response,
                                                   size_t resp_len);

/**
 * @brief Backfill memory→note bridge glosses for a user's existing notes.
 *
 * One-time Phase 9 remediation; non-destructive and idempotent (skips notes
 * that already have a gloss).
 *
 * @param fd Connected admin socket.
 * @param username Target user.
 * @param response Output buffer for the daemon's text response.
 * @param resp_len Size of response buffer.
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_memory_backfill_note_glosses(int fd,
                                                            const char *username,
                                                            char *response,
                                                            size_t resp_len);

/**
 * @brief Rebuild the document search (FTS) index from scratch (v61 recovery).
 *
 * Global (the FTS index spans all users); no payload.  Idempotent recovery path
 * for a partial migration backfill or FTS orphans.
 *
 * @param fd Connected admin socket.
 * @param response Output buffer for the daemon's text response.
 * @param resp_len Size of response buffer.
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_memory_rebuild_document_fts(int fd, char *response, size_t resp_len);

/**
 * @brief Bulk-delete pre-existing meta-fact rows by LIKE pattern.
 *
 * Targets interaction-event entries (e.g. "User asked about X") that were
 * stored before the May 2026 prompt fix added the explicit reject-meta-
 * facts guideline.  Patterns are hardcoded server-side; the operator only
 * controls username + dry-run flag.  Default mode is dry-run (count + sample
 * surfaced, nothing written) — pass dry_run=false to execute.
 *
 * @param fd Connected socket FD.
 * @param username Target user.
 * @param dry_run When true, count only; when false, delete matched rows.
 * @param response Output buffer for daemon response text.
 * @param resp_len Size of response buffer.
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_memory_cleanup_meta_facts(int fd,
                                                         const char *username,
                                                         bool dry_run,
                                                         char *response,
                                                         size_t resp_len);

/**
 * @brief Summarize-missing — backfill summary rows for conversations whose
 * extraction never produced one.  Dry-run (the default) reports the count;
 * --confirm starts a background worker on the daemon that loops through the
 * missing-summary set, runs the canonical extraction prompt per conversation,
 * and stores summary+topics only.  Progress is logged on the daemon; the
 * synchronous response is the start/dry-run banner.
 *
 * @param fd         Connected socket FD.
 * @param username   Target user.
 * @param dry_run    When true, return count only; when false, start the worker.
 * @param max_count  Cap on conversations processed this run (0 = unlimited).
 * @param response   Output buffer for daemon response text.
 * @param resp_len   Size of response buffer.
 * @return Response code from daemon.
 */
admin_resp_code_t admin_client_memory_summarize_missing(int fd,
                                                        const char *username,
                                                        bool dry_run,
                                                        uint32_t max_count,
                                                        char *response,
                                                        size_t resp_len);

/**
 * @brief Reextract — drop derived memory tables for a user and re-extract.
 *
 * Wire format encoded in this function — see admin_socket.h
 * "MEMORY_REEXTRACT payload" block.  In dry-run (confirm=false) the daemon
 * replies with a planning report and mutates nothing; with confirm=true it
 * runs backup + reset transaction + spawns the recovery worker.
 *
 * @param fd                 Connected socket FD.
 * @param username           Target user (1..ADMIN_REEXTRACT_USERNAME_MAX bytes).
 * @param confirm            true to execute, false for dry-run.
 * @param keep_summaries     true to keep memory_summaries during reset.
 * @param backup_path        NULL or "" to use the daemon-side default
 *                           (/var/lib/dawn/auth.db.reextract.<ts>).
 *                           Must start with /var/lib/dawn/ or /tmp/.
 * @param max_cost_usd       Hard cost cap; 0 = no cap.  Aborts the run when
 *                           the daemon's high-end estimate exceeds this.
 * @param response           Output buffer for daemon response text.
 * @param resp_len           Size of response buffer.
 * @return Daemon response code.
 */
admin_resp_code_t admin_client_memory_reextract(int fd,
                                                const char *username,
                                                bool confirm,
                                                bool keep_summaries,
                                                const char *backup_path,
                                                double max_cost_usd,
                                                char *response,
                                                size_t resp_len);

/**
 * @brief Reextract status — query progress for a user.
 *
 * @param fd        Connected socket FD.
 * @param username  Target user.
 * @param response  Output buffer for daemon response text.
 * @param resp_len  Size of response buffer.
 * @return Daemon response code.
 */
admin_resp_code_t admin_client_memory_reextract_status(int fd,
                                                       const char *username,
                                                       char *response,
                                                       size_t resp_len);

/* =============================================================================
 * Memory entity subcommands (v43, dawn-admin memory entity *)
 *
 * All six wrappers send the shared admin_memory_entity_payload_t and expect a
 * text response.  See dawn-admin/main.c for argument-parsing context.
 * ============================================================================= */

admin_resp_code_t admin_client_memory_entity_merge(int fd,
                                                   const char *username,
                                                   int64_t source_id,
                                                   int64_t target_id,
                                                   const char *reason,
                                                   char *response,
                                                   size_t resp_len);

admin_resp_code_t admin_client_memory_entity_split(int fd,
                                                   const char *username,
                                                   int64_t link_id,
                                                   const char *reason,
                                                   char *response,
                                                   size_t resp_len);

admin_resp_code_t admin_client_memory_entity_aliases(int fd,
                                                     const char *username,
                                                     int64_t entity_id,
                                                     char *response,
                                                     size_t resp_len);

admin_resp_code_t admin_client_memory_entity_history(int fd,
                                                     const char *username,
                                                     int64_t entity_id,
                                                     char *response,
                                                     size_t resp_len);

admin_resp_code_t admin_client_memory_entity_list(int fd,
                                                  const char *username,
                                                  bool include_aliases,
                                                  char *response,
                                                  size_t resp_len);

admin_resp_code_t admin_client_memory_entity_link_user_self(int fd,
                                                            const char *username,
                                                            bool dry_run,
                                                            char *response,
                                                            size_t resp_len);

/* =============================================================================
 * Messaging channels (dawn-admin messaging *)
 * ============================================================================= */

/**
 * @brief Generate a one-time link code on behalf of a user.
 *
 * The daemon issues the code via messaging_engine_generate_link_code(),
 * persisting to messaging_link_codes with the standard 10-minute TTL.
 * On success the response text contains the issued code on the first
 * line and an "Expires in N seconds" line below it.
 *
 * @param fd             Connected socket FD.
 * @param username       Target user (1..ADMIN_MESSAGING_USERNAME_MAX bytes).
 * @param provider_hint  NULL or "" for no hint; one of
 *                       "telegram"/"discord"/"slack"/"sms" otherwise.
 * @param response       Output buffer for daemon response text.
 * @param resp_len       Size of response buffer.
 * @return Daemon response code.
 */
admin_resp_code_t admin_client_messaging_generate_link_code(int fd,
                                                            const char *username,
                                                            const char *provider_hint,
                                                            char *response,
                                                            size_t resp_len);

/**
 * @brief List a user's linked channels (response is a JSON array).
 *
 * @param fd        Connected socket FD.
 * @param username  Target user (1..ADMIN_MESSAGING_USERNAME_MAX bytes).
 * @param response  Output buffer for the JSON response.
 * @param resp_len  Size of response buffer.
 * @return Daemon response code.
 */
admin_resp_code_t admin_client_messaging_list_channels(int fd,
                                                       const char *username,
                                                       char *response,
                                                       size_t resp_len);

/**
 * @brief Soft-delete (unlink) a user's channel by display name.
 *
 * @param fd            Connected socket FD.
 * @param username      Target user.
 * @param display_name  Channel display name to unlink.
 * @param response      Output buffer for daemon response text.
 * @param resp_len      Size of response buffer.
 * @return Daemon response code.
 */
admin_resp_code_t admin_client_messaging_unlink_channel(int fd,
                                                        const char *username,
                                                        const char *display_name,
                                                        char *response,
                                                        size_t resp_len);

/**
 * @brief Fetch recent /link attempts (abuse review) as an aligned table.
 *
 * @param fd              Connected socket FD.
 * @param provider_filter NULL/"" = all providers; else exact match.
 * @param limit           0 = daemon default (50); else max rows.
 * @param response        Output buffer for the table text.
 * @param resp_len        Size of response buffer.
 * @return Daemon response code.
 */
admin_resp_code_t admin_client_messaging_link_attempts(int fd,
                                                       const char *provider_filter,
                                                       int limit,
                                                       char *response,
                                                       size_t resp_len);

/**
 * @brief Re-enable a previously unlinked channel by display name.
 *
 * @param fd            Connected socket FD.
 * @param username      Target user.
 * @param display_name  Soft-deleted channel display name to re-enable.
 * @param response      Output buffer for daemon response text.
 * @param resp_len      Size of response buffer.
 * @return Daemon response code.
 */
admin_resp_code_t admin_client_messaging_reenable_channel(int fd,
                                                          const char *username,
                                                          const char *display_name,
                                                          char *response,
                                                          size_t resp_len);

/* =============================================================================
 * OTA updates (dawn-admin ota *)
 * ============================================================================= */

/**
 * @brief List the daemon's available OTA releases (aligned text table).
 *
 * @param fd        Connected socket FD.
 * @param response  Output buffer for the table text.
 * @param resp_len  Size of response buffer.
 * @return Daemon response code.
 */
admin_resp_code_t admin_client_ota_list(int fd, char *response, size_t resp_len);

/**
 * @brief Re-scan the daemon's release directory into its in-memory store.
 *
 * Makes a freshly-staged release pushable without restarting the daemon.
 *
 * @param fd        Connected socket FD.
 * @param response  Output buffer for the daemon status text.
 * @param resp_len  Size of response buffer.
 * @return Daemon response code.
 */
admin_resp_code_t admin_client_ota_rescan(int fd, char *response, size_t resp_len);

/**
 * @brief Start a canary-then-rollout of a version to a whole platform/tier.
 *
 * @param fd              Connected socket FD.
 * @param tier            Target tier (1 = RPi, 2 = ESP32; platform derived).
 * @param version         Release version to roll out.
 * @param allow_downgrade Permit offering older-than-installed.
 * @param response        Output buffer for the daemon summary text.
 * @param resp_len        Size of response buffer.
 * @return Daemon response code.
 */
admin_resp_code_t admin_client_ota_push_all(int fd,
                                            int tier,
                                            const char *version,
                                            bool allow_downgrade,
                                            char *response,
                                            size_t resp_len);

/** @brief Fetch the current/last fleet-rollout status (text). */
admin_resp_code_t admin_client_ota_rollout_status(int fd, char *response, size_t resp_len);

/** @brief Abort an in-progress fleet rollout. */
admin_resp_code_t admin_client_ota_rollout_abort(int fd, char *response, size_t resp_len);

/**
 * @brief Push an update offer to one satellite by uuid.
 *
 * The daemon resolves the device's tier→platform, mints a one-time download
 * token, and delivers the ota_offer over the device's live WS session (the
 * device must be online).
 *
 * @param fd              Connected socket FD.
 * @param uuid            Target satellite uuid (1..ADMIN_OTA_UUID_MAX bytes).
 * @param version         Release version to offer (1..ADMIN_OTA_VERSION_MAX).
 * @param allow_downgrade Permit offering an older version than the device runs.
 * @param response        Output buffer for daemon response text.
 * @param resp_len        Size of response buffer.
 * @return Daemon response code.
 */
admin_resp_code_t admin_client_ota_push(int fd,
                                        const char *uuid,
                                        const char *version,
                                        bool allow_downgrade,
                                        char *response,
                                        size_t resp_len);

/* MCP bridge (coding harness) operator commands. */
admin_resp_code_t admin_client_mcp_list(int fd, char *response, size_t resp_len);
admin_resp_code_t admin_client_mcp_reset(int fd, char *response, size_t resp_len);
admin_resp_code_t admin_client_mcp_grant(int fd,
                                         const char *username,
                                         const char *alias,
                                         char *response,
                                         size_t resp_len);
admin_resp_code_t admin_client_mcp_revoke(int fd,
                                          const char *username,
                                          const char *alias,
                                          char *response,
                                          size_t resp_len);

/* Code projects (coding harness) operator commands. */
admin_resp_code_t admin_client_code_proj_list(int fd, char *response, size_t resp_len);
admin_resp_code_t admin_client_code_proj_import(int fd,
                                                const char *url,
                                                const char *name,
                                                const char *branch,
                                                bool global,
                                                char *response,
                                                size_t resp_len);
admin_resp_code_t admin_client_code_proj_refresh(int fd,
                                                 const char *name,
                                                 char *response,
                                                 size_t resp_len);
admin_resp_code_t admin_client_code_proj_delete(int fd,
                                                const char *name,
                                                char *response,
                                                size_t resp_len);
admin_resp_code_t admin_client_code_proj_rebuild(int fd,
                                                 const char *name,
                                                 char *response,
                                                 size_t resp_len);
admin_resp_code_t admin_client_code_proj_set_branch(int fd,
                                                    const char *name,
                                                    const char *branch,
                                                    char *response,
                                                    size_t resp_len);
admin_resp_code_t admin_client_code_proj_link(int fd,
                                              const char *path,
                                              const char *name,
                                              char *response,
                                              size_t resp_len);

#endif /* DAWN_ADMIN_SOCKET_CLIENT_H */
