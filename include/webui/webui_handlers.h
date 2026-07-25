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
 * the project author(s).
 *
 * WebUI handler function declarations.
 *
 * Cross-module API for every `handle_*` function defined in a sibling
 * webui_*.c file (webui_admin.c, webui_history.c, webui_memory.c,
 * webui_doc_library.c, webui_calendar.c, webui_config.c,
 * webui_session.c, webui_settings.c, webui_tools.c, webui_audio.c,
 * webui_music.c, webui_homeassistant.c, webui_satellite.c,
 * webui_admin_satellite.c).  Carved out of webui_internal.h so the
 * dispatcher and other consumers can pull just the handler surface.
 *
 * INTERNAL TO THE WEBUI MODULE.  Every handler in this file expects to
 * be called from the LWS dispatch path in webui_message_dispatch.c.
 * Handlers self-gate auth via conn_require_auth / conn_require_admin
 * unless explicitly noted in the per-function comment.
 */

#ifndef WEBUI_HANDLERS_H
#define WEBUI_HANDLERS_H

#include "webui/webui_internal.h" /* ws_connection_t */

#ifdef __cplusplus
extern "C" {
#endif

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
 * Conversation Context Restore (defined in webui_server.c)
 * ============================================================================= */

/**
 * @brief Restore conversation context into a session from DB
 *
 * Clears session history, rebuilds with system prompt + compaction summary +
 * stored messages, and restores LLM config (type, provider, model, tools,
 * thinking).
 *
 * Used by both webui_conn_create_session (session expiry recovery) and
 * handle_load_conversation (sidebar click). Caller owns the conversation_t
 * and must call conv_free() after this returns.
 *
 * @param conn Connection with authenticated user
 * @param conv Conversation metadata (already fetched via conv_db_get)
 * @param conv_id Conversation ID
 * @param preloaded_msgs Optional pre-fetched message array (json_object array
 *        with "role" and "content" fields per element). If NULL, messages are
 *        fetched from the DB. Caller retains ownership; not freed by this function.
 * @return Number of messages restored, or LWS_CLOSE_CONNECTION on error
 */
int webui_restore_conversation_context(ws_connection_t *conn,
                                       const conversation_t *conv,
                                       int64_t conv_id,
                                       json_object *preloaded_msgs);

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
 * @brief Replay a conversation's durable event log to @p conn (§6 attach step 2).
 *
 * Sends one `conversation_events` frame carrying events at seq > @p last_seq, or
 * nothing when there are none.  Ownership-scoped through conv_db_event_list's
 * JOIN, so a conversation this connection's user does not own replays as empty.
 *
 * Called from handle_load_conversation between the message batch and the
 * in-memory ring replay — that ordering is the attach contract (§6), not an
 * implementation detail.
 *
 * @param last_seq Exclusive cursor; 0 replays the whole log.
 */
void webui_send_conversation_events(ws_connection_t *conn, int64_t conv_id, int64_t last_seq);

/**
 * @brief Send the user's complete ACTIVE job set (connect/reconnect rehydration).
 *
 * The client keys jobs by conversation id and derives the "jobs running" pill
 * counts from this set, which is only sound because the active set is bounded
 * and arrives whole — see the lifetime-split note atop src/webui/webui_jobs.c.
 */
void webui_jobs_send_snapshot(ws_connection_t *conn);

/**
 * @brief Send one keyset-paginated page of the user's TERMINAL jobs (history).
 *
 * Unbounded feed, deliberately separate from the snapshot: a page of this must
 * never be counted.  Pass @p before_created_at == 0 for the first page;
 * afterwards echo back the `next_before_*` cursor from the previous response.
 * @p limit <= 0 takes the default page size.
 */
void webui_jobs_send_history(ws_connection_t *conn,
                             int64_t before_created_at,
                             int64_t before_id,
                             int limit);

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
 * @brief Pin or unpin a conversation
 *
 * Pinned conversations float to a dedicated section at the top of the WebUI list.
 */
void handle_set_pinned(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Export a conversation as a self-contained JSON document
 */
void handle_export_conversation(ws_connection_t *conn, struct json_object *payload);

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
 * @brief List memory preferences for the current user (paginated)
 */
void handle_list_memory_preferences(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Delete a memory preference by category
 */
void handle_delete_memory_preference(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief List memory summaries for the current user (paginated)
 */
void handle_list_memory_summaries(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Delete a memory summary
 */
void handle_delete_memory_summary(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Search memory facts and summaries by keyword
 */
void handle_search_memory(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief List memory entities with relations (paginated)
 */
void handle_list_memory_entities(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Delete a memory entity and its relations
 */
void handle_delete_memory_entity(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Merge two memory entities (source absorbed into target)
 */
void handle_merge_memory_entities(ws_connection_t *conn, struct json_object *payload);

/* =============================================================================
 * Entity-merge alias surface (v43) — five WebSocket handlers per
 * docs/ENTITY_MERGE_DESIGN.md §14.
 * ============================================================================= */
void handle_entity_aliases_request(ws_connection_t *conn, struct json_object *payload);
void handle_entity_merge_proposal_list_request(ws_connection_t *conn, struct json_object *payload);
void handle_entity_link_request(ws_connection_t *conn, struct json_object *payload);
void handle_entity_unlink_request(ws_connection_t *conn, struct json_object *payload);
void handle_entity_proposal_resolve_request(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Delete all memories for the current user
 */
void handle_delete_all_memories(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Export all memories for the current user
 *
 * Supports "json" (DAWN lossless) and "text" (human-readable) formats.
 */
void handle_export_memories(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Import memories from JSON or plain text
 *
 * Supports preview mode (commit=false) and deduplication.
 */
void handle_import_memories(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Return verbatim source messages for a fact (v40 memory provenance)
 *
 * Returns the conversation messages that produced the fact during extraction.
 * Returns not_available if: fact is legacy (no provenance), private, or deleted.
 */
void handle_get_memory_fact_source(ws_connection_t *conn, struct json_object *payload);

/* =============================================================================
 * Document Library Handler Functions (defined in webui_doc_library.c)
 * ============================================================================= */
void handle_doc_library_list(ws_connection_t *conn, struct json_object *payload);
void handle_doc_library_delete(ws_connection_t *conn, struct json_object *payload);
void handle_doc_library_index(ws_connection_t *conn, struct json_object *payload);

/* =============================================================================
 * Scheduler Queue Handler Functions (defined in webui_scheduler.c)
 *
 * Surfaces the per-user scheduled-events queue (timers / alarms / reminders /
 * tasks / briefings) to the WebUI panel.  Routed under the existing
 * scheduler_action sub-action umbrella so the dispatcher only adds one case.
 * ============================================================================= */

/**
 * @brief List the user's active queue (pending/snoozed/ringing) plus missed entries
 *
 * Returns a scheduler_events_response payload with an events[] array.  When
 * the caller is an unassigned satellite (auth_user_id <= 0) the events array
 * is empty rather than an error — keeps satellite reconnect flows quiet.
 */
void handle_scheduler_list_events(ws_connection_t *conn);

/**
 * @brief Cancel a scheduled event the caller owns (breaks the recurrence chain)
 *
 * Enforces event.user_id == conn->auth_user_id with NO satellite carve-out
 * (cancel is not a live-alarm operation).  Sends NOT_FOUND for both
 * "doesn't exist" and "not yours" to avoid ownership-existence leakage.
 */
void handle_scheduler_cancel_event(ws_connection_t *conn, int64_t event_id);

/**
 * @brief Cancel just this occurrence; keep the recurrence chain alive
 */
void handle_scheduler_cancel_occurrence(ws_connection_t *conn, int64_t event_id);

/**
 * @brief Acknowledge a missed event (status='missed' → 'dismissed')
 *
 * Used by the panel's per-row Acknowledge button and the Clear All Missed
 * footer.  Same auth shape as cancel — no satellite carve-out.
 */
void handle_scheduler_clear_missed(ws_connection_t *conn, int64_t event_id);

/* =============================================================================
 * Calendar Handler Functions (defined in webui_calendar.c)
 * ============================================================================= */

#ifdef DAWN_ENABLE_CALENDAR_TOOL
void handle_calendar_list_accounts(ws_connection_t *conn);
void handle_calendar_add_account(ws_connection_t *conn, struct json_object *payload);
void handle_calendar_edit_account(ws_connection_t *conn, struct json_object *payload);
void handle_calendar_remove_account(ws_connection_t *conn, struct json_object *payload);
void handle_calendar_test_account(ws_connection_t *conn, struct json_object *payload);
void handle_calendar_sync_account(ws_connection_t *conn, struct json_object *payload);
void handle_calendar_list_calendars(ws_connection_t *conn, struct json_object *payload);
void handle_calendar_toggle_calendar(ws_connection_t *conn, struct json_object *payload);
void handle_calendar_toggle_read_only(ws_connection_t *conn, struct json_object *payload);
void handle_calendar_set_enabled(ws_connection_t *conn, struct json_object *payload);
#endif /* DAWN_ENABLE_CALENDAR_TOOL */

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
 * Home Assistant Handler Functions (defined in webui_homeassistant.c)
 * ============================================================================= */

#ifdef DAWN_ENABLE_HOMEASSISTANT_TOOL
/**
 * @brief Get Home Assistant connection status
 */
void handle_ha_status(ws_connection_t *conn);

/**
 * @brief Test Home Assistant connection
 */
void handle_ha_test_connection(ws_connection_t *conn);

/**
 * @brief List Home Assistant entities
 */
void handle_ha_list_entities(ws_connection_t *conn);

/**
 * @brief Force refresh Home Assistant entity cache
 */
void handle_ha_refresh_entities(ws_connection_t *conn);
#endif /* DAWN_ENABLE_HOMEASSISTANT_TOOL */

/* =============================================================================
 * Satellite Connection Helpers (defined in webui_server.c)
 * ============================================================================= */

/**
 * @brief Check if a satellite with the given UUID is currently connected
 *
 * Thread-safe. Scans the connection registry for an active satellite connection
 * whose session identity UUID matches.
 *
 * @param uuid Satellite UUID string (36 chars)
 * @return true if online, false if not found or offline
 */
bool webui_is_satellite_online(const char *uuid);

/**
 * @brief Force-disconnect a satellite by UUID
 *
 * Thread-safe. Finds the satellite connection and closes it with a policy
 * violation reason. Used when an admin disables a satellite.
 *
 * @param uuid Satellite UUID string (36 chars)
 */
void webui_force_disconnect_satellite(const char *uuid);

/* =============================================================================
 * Satellite Admin Handler Functions (defined in webui_admin_satellite.c)
 * ============================================================================= */

/**
 * @brief List all satellite mappings with online status (admin only)
 */
void handle_list_satellites(ws_connection_t *conn);

/**
 * @brief Update satellite mapping (user, ha_area, enabled) (admin only)
 */
void handle_update_satellite(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Delete a satellite mapping (admin only)
 */
void handle_delete_satellite(ws_connection_t *conn, struct json_object *payload);

/**
 * @brief Return the satellite registration key to the admin client (admin only)
 *
 * Surfaces secrets->satellite_registration_key for QR-code pairing in the
 * WebUI. Rate-limited per source IP (5 fetches/min, 30/hour) and audit-logged
 * via the auth event log. The key value itself never appears in any log line.
 *
 * @param conn Authenticated WebSocket connection; admin privilege is enforced
 *             internally via conn_require_admin().
 */
void handle_get_satellite_registration_key(ws_connection_t *conn);

/* =============================================================================
 * Messaging Channel Handler Functions (defined in webui_messaging.c)
 *
 * User-scoped (conn_require_auth + conn->auth_user_id) channel management
 * for the Settings panel.  Mutations key on the stable row id.
 * ============================================================================= */

/** @brief List the authenticated user's linked messaging channels. */
void handle_list_channels(ws_connection_t *conn);

/** @brief Issue a one-time link code for the authenticated user (optional provider). */
void handle_create_link_code(ws_connection_t *conn, struct json_object *payload);

/** @brief Unlink (soft-delete) one of the user's channels by row id. */
void handle_unlink_channel(ws_connection_t *conn, struct json_object *payload);

/** @brief Rename one of the user's channels by row id. */
void handle_rename_channel(ws_connection_t *conn, struct json_object *payload);

/** @brief Re-enable one of the user's soft-deleted channels by row id. */
void handle_reenable_channel(ws_connection_t *conn, struct json_object *payload);

/** @brief Set the per-channel LLM settings (writes the channel's conversation row). */
void handle_set_channel_llm(ws_connection_t *conn, struct json_object *payload);

/* =============================================================================
 * Satellite Handler Functions (defined in webui_satellite.c)
 * ============================================================================= */

/**
 * @brief Strip <command>...</command> and <end_of_turn> tags from text in-place
 *
 * Shared utility used by satellite worker and audio sentence callback.
 */
void strip_command_tags(char *text);

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

/**
 * @brief Handle volume_state message from satellite
 */
void handle_satellite_volume_state(ws_connection_t *conn, struct json_object *payload);

#ifdef __cplusplus
}
#endif

#endif /* WEBUI_HANDLERS_H */
