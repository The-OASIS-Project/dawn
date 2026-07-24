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
 * WebUI History Handlers - Conversation history management
 *
 * This module handles WebSocket messages for conversation history:
 * - list_conversations, new_conversation, load_conversation
 * - delete_conversation, rename_conversation, search_conversations
 * - save_message, update_context, clear_session, continue_conversation
 */

#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/conv_stream.h"
#include "core/ocp_helpers.h"
#include "core/session_manager.h"
#include "image_store.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "memory/memory_extraction.h"
#include "version.h"
#include "webui/webui_image_rehydrate.h"
#include "webui/webui_internal.h"
#include "webui/webui_server.h" /* For WEBUI_MAX_THUMBNAIL_BASE64 */

/* =============================================================================
 * Image Marker Validation (Security)
 * ============================================================================ */

/* Safe data URI prefixes for thumbnails (SVG explicitly excluded for XSS prevention) */
static const char *SAFE_IMAGE_PREFIXES[] = { "data:image/jpeg;base64,", "data:image/png;base64,",
                                             "data:image/gif;base64,", "data:image/webp;base64,",
                                             NULL };

/* Valid base64 character lookup table (A-Z, a-z, 0-9, +, /, =) */
static const unsigned char BASE64_VALID[256] = {
   ['A'] = 1, ['B'] = 1, ['C'] = 1, ['D'] = 1, ['E'] = 1, ['F'] = 1, ['G'] = 1, ['H'] = 1,
   ['I'] = 1, ['J'] = 1, ['K'] = 1, ['L'] = 1, ['M'] = 1, ['N'] = 1, ['O'] = 1, ['P'] = 1,
   ['Q'] = 1, ['R'] = 1, ['S'] = 1, ['T'] = 1, ['U'] = 1, ['V'] = 1, ['W'] = 1, ['X'] = 1,
   ['Y'] = 1, ['Z'] = 1, ['a'] = 1, ['b'] = 1, ['c'] = 1, ['d'] = 1, ['e'] = 1, ['f'] = 1,
   ['g'] = 1, ['h'] = 1, ['i'] = 1, ['j'] = 1, ['k'] = 1, ['l'] = 1, ['m'] = 1, ['n'] = 1,
   ['o'] = 1, ['p'] = 1, ['q'] = 1, ['r'] = 1, ['s'] = 1, ['t'] = 1, ['u'] = 1, ['v'] = 1,
   ['w'] = 1, ['x'] = 1, ['y'] = 1, ['z'] = 1, ['0'] = 1, ['1'] = 1, ['2'] = 1, ['3'] = 1,
   ['4'] = 1, ['5'] = 1, ['6'] = 1, ['7'] = 1, ['8'] = 1, ['9'] = 1, ['+'] = 1, ['/'] = 1,
   ['='] = 1
};

/**
 * @brief Check if string is a valid image ID
 *
 * Image ID format: "img_" + 12 alphanumeric characters (16 total)
 *
 * @param str String to check
 * @param len Length of string
 * @return true if valid image ID format
 */
static bool is_valid_image_id(const char *str, size_t len) {
   /* Must be exactly 16 characters: "img_" + 12 alphanumeric */
   if (len != 16) {
      return false;
   }

   /* Must start with "img_" */
   if (strncmp(str, "img_", 4) != 0) {
      return false;
   }

   /* Characters 4-15 must be alphanumeric */
   for (int i = 4; i < 16; i++) {
      char c = str[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
         return false;
      }
   }

   return true;
}

/**
 * @brief Validate a single image marker
 *
 * Accepts two formats:
 * 1. Image ID: [IMAGE:img_xxxxxxxxxxxx] - server-stored image reference
 * 2. Data URI: [IMAGE:data:image/jpeg;base64,...] - legacy inline data
 *
 * @param marker_start Pointer to start of "[IMAGE:" marker
 * @param marker_end Output: pointer to closing ']' if found
 * @return true if marker is valid, false if malicious/oversized
 */
static bool validate_single_image_marker(const char *marker_start, const char **marker_end) {
   /* Find the closing bracket */
   const char *end = strchr(marker_start + 7, ']');
   if (!end) {
      return false; /* Malformed marker */
   }
   *marker_end = end;

   /* Extract content (skip "[IMAGE:" prefix) */
   const char *content = marker_start + 7;
   size_t content_len = end - content;

   /* Check if it's an image ID (new format: img_xxxxxxxxxxxx) */
   if (is_valid_image_id(content, content_len)) {
      return true; /* Valid image ID reference */
   }

   /* Otherwise, validate as legacy data URI */

   /* Check against safe prefixes */
   bool has_safe_prefix = false;
   for (int i = 0; SAFE_IMAGE_PREFIXES[i] != NULL; i++) {
      size_t prefix_len = strlen(SAFE_IMAGE_PREFIXES[i]);
      if (content_len > prefix_len && strncmp(content, SAFE_IMAGE_PREFIXES[i], prefix_len) == 0) {
         has_safe_prefix = true;
         break;
      }
   }

   if (!has_safe_prefix) {
      OLOG_WARNING("WebUI: Rejected message with unsafe image data URI prefix");
      return false;
   }

   /* Check size (base64 portion only) */
   const char *base64_start = strchr(content, ',');
   if (!base64_start || base64_start >= end) {
      return false; /* Malformed data URI */
   }
   base64_start++; /* Skip comma */

   size_t base64_len = end - base64_start;
   if (base64_len > WEBUI_MAX_THUMBNAIL_BASE64) {
      OLOG_WARNING("WebUI: Rejected oversized thumbnail (%zu > %d bytes)", base64_len,
                   WEBUI_MAX_THUMBNAIL_BASE64);
      return false;
   }

   /* Validate base64 characters (prevents injection via malformed data) */
   for (size_t i = 0; i < base64_len; i++) {
      unsigned char c = (unsigned char)base64_start[i];
      if (!BASE64_VALID[c]) {
         OLOG_WARNING("WebUI: Rejected thumbnail with invalid base64 character at position %zu", i);
         return false;
      }
   }

   return true;
}

/**
 * @brief Validate ALL embedded image markers in message content
 *
 * Accepts two marker formats:
 * - Image ID: [IMAGE:img_xxxxxxxxxxxx] (server-stored reference)
 * - Data URI: [IMAGE:data:image/jpeg;base64,...] (legacy inline)
 *
 * For image IDs: validates format (img_ + 12 alphanumeric)
 * For data URIs: validates prefix, size, and base64 characters
 *
 * SECURITY: Validates every marker, not just the first, to prevent bypass
 * attacks where a valid first image masks a malicious second image.
 *
 * @param content Message content to validate
 * @return true if all markers are safe, false if any malicious/invalid marker found
 */
static bool validate_image_marker(const char *content) {
   if (!content)
      return true;

   const char *search_pos = content;
   const char *marker_start;
   int marker_count = 0;

   /* Iterate through ALL [IMAGE: markers in content */
   while ((marker_start = strstr(search_pos, "[IMAGE:")) != NULL) {
      const char *marker_end = NULL;

      if (!validate_single_image_marker(marker_start, &marker_end)) {
         OLOG_WARNING("WebUI: Rejected invalid image marker #%d in message", marker_count + 1);
         return false;
      }

      marker_count++;

      /* Limit total markers to prevent DoS (matches vision.max_images cap) */
      if (marker_count > WEBUI_MAX_VISION_IMAGES_CAP) {
         OLOG_WARNING("WebUI: Rejected message with too many image markers (%d > %d)", marker_count,
                      WEBUI_MAX_VISION_IMAGES_CAP);
         return false;
      }

      /* Move search position past this marker */
      search_pos = marker_end + 1;
   }

   return true;
}

/* =============================================================================
 * Conversation History Handlers (Authenticated Users)
 * ============================================================================ */

/* Callback for conversation enumeration */
static int list_conv_callback(const conversation_t *conv, void *context) {
   json_object *conv_array = (json_object *)context;
   json_object *conv_obj = json_object_new_object();

   json_object_object_add(conv_obj, "id", json_object_new_int64(conv->id));
   json_object_object_add(conv_obj, "title", json_object_new_string(conv->title));
   json_object_object_add(conv_obj, "created_at", json_object_new_int64(conv->created_at));
   json_object_object_add(conv_obj, "updated_at", json_object_new_int64(conv->updated_at));
   json_object_object_add(conv_obj, "message_count", json_object_new_int(conv->message_count));
   json_object_object_add(conv_obj, "is_archived", json_object_new_boolean(conv->is_archived));
   json_object_object_add(conv_obj, "is_private", json_object_new_boolean(conv->is_private));
   json_object_object_add(conv_obj, "is_pinned", json_object_new_boolean(conv->is_pinned));
   json_object_object_add(conv_obj, "origin",
                          json_object_new_string(conv->origin[0] ? conv->origin : "webui"));

   /* Continuation indicator for history panel chain icon */
   if (conv->continued_from > 0) {
      json_object_object_add(conv_obj, "continued_from",
                             json_object_new_int64(conv->continued_from));
   }

   json_object_array_add(conv_array, conv_obj);
   return 0;
}

/**
 * @brief List conversations for the current user
 */
void handle_list_conversations(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("list_conversations_response"));
   json_object *resp_payload = json_object_new_object();
   json_object *conv_array = json_object_new_array();

   /* Parse pagination params if present */
   conv_pagination_t pagination = { 0, 0 };
   if (payload) {
      json_object *limit_obj, *offset_obj;
      if (json_object_object_get_ex(payload, "limit", &limit_obj)) {
         pagination.limit = json_object_get_int(limit_obj);
      }
      if (json_object_object_get_ex(payload, "offset", &offset_obj)) {
         pagination.offset = json_object_get_int(offset_obj);
      }
   }

   /* Include archived conversations so users can see the full chain */
   int result = conv_db_list(conn->auth_user_id, true, &pagination, list_conv_callback, conv_array);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "conversations", conv_array);

      /* Include total count for pagination */
      int total = 0;
      conv_db_count(conn->auth_user_id, &total);
      json_object_object_add(resp_payload, "total", json_object_new_int(total));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list conversations"));
      json_object_put(conv_array);
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Privacy Check Helper
 * ============================================================================ */

/**
 * @brief Check if memory extraction should be skipped for the active conversation
 *
 * Centralizes the privacy check logic and handles race conditions by re-verifying
 * from the database when needed. Also updates the cached state if stale.
 *
 * @param conn WebSocket connection with conversation context
 * @return true if memory extraction should be skipped, false otherwise
 */
static bool should_skip_memory_extraction(ws_connection_t *conn) {
   /* No conversation to extract from */
   if (conn->active_conversation_id <= 0) {
      return true;
   }

   /* Memory system disabled */
   if (!g_config.memory.enabled) {
      return true;
   }

   /* Check cached privacy flag first */
   if (conn->active_conversation_private) {
      return true;
   }

   /* Re-verify from database to handle race conditions (e.g., set_private in flight) */
   bool db_private = false;
   int priv_rc = conv_db_is_private(conn->active_conversation_id, conn->auth_user_id, &db_private);
   if (priv_rc == AUTH_DB_SUCCESS && db_private) {
      /* Update cached state to match database */
      conn->active_conversation_private = true;
      OLOG_INFO("WebUI: privacy check found stale cache, conversation %lld is private",
                (long long)conn->active_conversation_id);
      return true;
   }

   /* Not private, error, or not found - proceed with extraction */
   return false;
}

/**
 * @brief Create a new conversation
 */
void handle_new_conversation(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   /* Trigger memory extraction for old conversation before creating new one (async, non-blocking).
    * This captures the conversation state before switching to a fresh context.
    * Strip _provider_state first — see llm_history_strip_provider_state docs. */
   if (conn->session && !should_skip_memory_extraction(conn)) {
      struct json_object *old_history = session_get_history(conn->session);
      if (old_history) {
         int msg_count = json_object_array_length(old_history);
         if (msg_count >= 2) {
            struct json_object *clean = llm_history_strip_provider_state(old_history);
            if (clean) {
               memory_extraction_fallback_t fb;
               memory_extraction_build_fallback(conn->session, &fb);
               OLOG_INFO("WebUI: Triggering memory extraction for conversation %lld before new",
                         (long long)conn->active_conversation_id);
               memory_trigger_extraction(conn->auth_user_id, conn->active_conversation_id, NULL,
                                         clean, msg_count, 0, &fb);
               json_object_put(clean);
            }
         }
         json_object_put(old_history);
      }
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("new_conversation_response"));
   json_object *resp_payload = json_object_new_object();

   /* Optional title from payload */
   const char *title = NULL;
   if (payload) {
      json_object *title_obj;
      if (json_object_object_get_ex(payload, "title", &title_obj)) {
         title = json_object_get_string(title_obj);
      }
   }

   int64_t conv_id;
   int result = conv_db_create(conn->auth_user_id, title, &conv_id);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "conversation_id", json_object_new_int64(conv_id));

      /* NOTE: We intentionally do NOT clear session history here.
       *
       * The client sends "new_conversation" AFTER sending the first text message.
       * The server may have already added that message to session history and started
       * the LLM call. Clearing the history here would wipe out the user's message
       * mid-request, breaking conversation continuity.
       *
       * The session history is the active in-memory context for the LLM.
       * The database conversation is for persistence across sessions.
       * These serve different purposes and should not be coupled.
       *
       * Session history is cleared only when:
       * - User explicitly requests clear_history
       * - User loads a different conversation (load_conversation)
       * - User starts a new chat via UI (which sends clear_history first)
       */

      auth_db_log_event("CONVERSATION_CREATED", conn->username, conn->client_ip,
                        "New conversation");

      /* Update active conversation tracking */
      conn->active_conversation_id = conv_id;

      /* Back-fill the in-flight turn's conversation tag if it was dispatched
       * before this row existed (fresh-chat first message: the text is dispatched,
       * then this new_conversation creates the row).  Only overwrite the
       * uninitialized (0) value so an established turn's tag is never clobbered.
       * This is the server-side safety net for the client's pre-create ordering. */
      if (conn->session && conn->session->stream_conversation_id == 0) {
         conn->session->stream_conversation_id = conv_id;
      }
   } else if (result == AUTH_DB_LIMIT_EXCEEDED) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Maximum conversation limit reached"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to create conversation"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/**
 * @brief Clear session history for a fresh start
 *
 * Called when user starts a new conversation to clear the in-memory
 * session history. This prevents stale messages from being sent to
 * new LLM conversations. Sends acknowledgment to client.
 */
void handle_clear_session(ws_connection_t *conn) {
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("clear_session_response"));
   json_object *resp_payload = json_object_new_object();

   if (!conn || !conn->session) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("No active session"));
      json_object_object_add(response, "payload", resp_payload);
      if (conn) {
         send_json_response(conn, response);
      }
      json_object_put(response);
      return;
   }

   /* Trigger memory extraction before clearing (captures conversation state).
    * Strip _provider_state first — see llm_history_strip_provider_state docs. */
   if (!should_skip_memory_extraction(conn)) {
      struct json_object *old_history = session_get_history(conn->session);
      if (old_history) {
         int msg_count = json_object_array_length(old_history);
         if (msg_count >= 2) {
            struct json_object *clean = llm_history_strip_provider_state(old_history);
            if (clean) {
               memory_extraction_fallback_t fb;
               memory_extraction_build_fallback(conn->session, &fb);
               OLOG_INFO("WebUI: Triggering memory extraction for conversation %lld before clear",
                         (long long)conn->active_conversation_id);
               memory_trigger_extraction(conn->auth_user_id, conn->active_conversation_id, NULL,
                                         clean, msg_count, 0, &fb);
               json_object_put(clean);
            }
         }
         json_object_put(old_history);
      }
   }

   /* Reset conversation tracking — prevents handle_new_conversation from
    * re-triggering extraction on the same (now-cleared) conversation */
   conn->active_conversation_id = 0;
   conn->active_conversation_private = false;

   session_clear_history(conn->session);

   /* Phase 1f: history clear is a SESSION_START boundary — clear dedup
    * state so the new conversation admits all candidates fresh. */
   session_injected_set_clear(conn->session);

   /* Re-add system prompt for the new conversation */
   char *prompt = session_manager_build_system_prompt_string(conn->auth_user_id);
   session_add_message(conn->session, "system", prompt ? prompt : get_remote_command_prompt());
   free(prompt);

   json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);

   OLOG_INFO("WebUI: Session history cleared for user '%s'",
             conn->username ? conn->username : "unknown");
}

/**
 * @brief Continue a conversation (after context compaction)
 *
 * Archives the current conversation and creates a new one linked to it.
 * Called by the client when server notifies that context was compacted.
 */
void handle_continue_conversation(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   /* DORMANT as of v67: compaction now records an in-conversation watermark instead
    * of forking (see llm_context_compact + conv_db_set_compaction_watermark). The
    * WebUI client no longer sends `continue_conversation` on compaction. This handler
    * is retained only for backward-compat; if it ever fires, the (archiving) split
    * path is still live — log it so we can confirm the client is the only caller
    * before removing this code. */
   OLOG_WARNING("WebUI: dormant continue_conversation handler invoked (v67 uses watermarks; "
                "this still archives — investigate the caller)");

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("continue_conversation_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get conversation ID */
   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "conversation_id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing conversation_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int64_t old_conv_id = json_object_get_int64(id_obj);

   /* Get summary */
   const char *summary = "";
   json_object *summary_obj;
   if (json_object_object_get_ex(payload, "summary", &summary_obj)) {
      summary = json_object_get_string(summary_obj);
   }

   /* Create continuation */
   int64_t new_conv_id;
   int result = conv_db_create_continuation(conn->auth_user_id, old_conv_id, summary, &new_conv_id);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "old_conversation_id",
                             json_object_new_int64(old_conv_id));
      json_object_object_add(resp_payload, "new_conversation_id",
                             json_object_new_int64(new_conv_id));
      json_object_object_add(resp_payload, "summary", json_object_new_string(summary));

      OLOG_INFO("WebUI: Conversation %lld continued as %lld for user %s", (long long)old_conv_id,
                (long long)new_conv_id, conn->username);

      auth_db_log_event("CONVERSATION_CONTINUED", conn->username, conn->client_ip,
                        "Context compacted");
   } else if (result == AUTH_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Conversation not found"));
   } else if (result == AUTH_DB_FORBIDDEN) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Access denied"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to continue conversation"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* Callback for message enumeration - includes message ID for pagination */
static int load_msg_callback(const conversation_message_t *msg, void *context) {
   json_object *msg_array = (json_object *)context;
   json_object *msg_obj = json_object_new_object();

   json_object_object_add(msg_obj, "id", json_object_new_int64(msg->id));
   json_object_object_add(msg_obj, "role", json_object_new_string(msg->role));
   json_object_object_add(msg_obj, "content",
                          json_object_new_string(msg->content ? msg->content : ""));
   json_object_object_add(msg_obj, "created_at", json_object_new_int64(msg->created_at));
   /* Surface structured tool fields so the browser can render tool calls/results
    * under debug-mode on reload (E2). tool_calls is sent as parsed JSON. */
   if (msg->tool_calls && msg->tool_calls[0]) {
      json_object *tc = json_tokener_parse(msg->tool_calls);
      if (tc) {
         json_object_object_add(msg_obj, "tool_calls", tc);
      }
   }
   if (msg->tool_call_id && msg->tool_call_id[0]) {
      json_object_object_add(msg_obj, "tool_call_id", json_object_new_string(msg->tool_call_id));
   }
   /* Surface display-only reasoning JSON as a parsed `reasoning` field so the browser
    * reconstructs the "AI thought" panel at this row's position (E3).  Display-only —
    * delivered here but NOT in webui_session_restore_msg_cb (the LLM-context path). */
   if (msg->reasoning && msg->reasoning[0]) {
      json_object *r = json_tokener_parse(msg->reasoning);
      if (r) {
         json_object_object_add(msg_obj, "reasoning", r);
      }
   }

   json_object_array_add(msg_array, msg_obj);
   return 0;
}

/**
 * @brief Load a conversation and all of its messages
 *
 * Returns the WHOLE conversation in one response (no pagination): message text is cheap
 * and the daemon already fetches the full history for LLM-context restore, so paging would
 * only risk splitting a tool call/result pair (and its reasoning panel) across a page
 * boundary.  The former cursor-based "scroll up to load more" path was retired once full
 * load became the default.
 *
 * Response includes:
 * - messages: Array of messages (oldest first)
 * - total: Total message count in conversation
 */
void handle_load_conversation(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("load_conversation_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get conversation ID */
   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "conversation_id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing conversation_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int64_t conv_id = json_object_get_int64(id_obj);

   bool needs_session_context = (conn->session != NULL);

   /* Trigger memory extraction for old conversation before switching (async, non-blocking).
    * Only triggers on an actual conversation switch, not on reloading the same conversation. */
   if (conn->active_conversation_id != conv_id && conn->session &&
       !should_skip_memory_extraction(conn)) {
      struct json_object *old_history = session_get_history(conn->session);
      if (old_history) {
         int msg_count = json_object_array_length(old_history);
         if (msg_count >= 2) {
            /* Strip _provider_state before forwarding to extraction (cross-provider
             * leak guard — see llm_history_strip_provider_state docs). */
            struct json_object *clean = llm_history_strip_provider_state(old_history);
            if (clean) {
               memory_extraction_fallback_t fb;
               memory_extraction_build_fallback(conn->session, &fb);
               OLOG_INFO("WebUI: Triggering memory extraction for conversation %lld before switch",
                         (long long)conn->active_conversation_id);
               memory_trigger_extraction(conn->auth_user_id, conn->active_conversation_id, NULL,
                                         clean, msg_count, 0, &fb);
               json_object_put(clean);
            }
         }
         json_object_put(old_history);
      }
   }

   /* Get conversation metadata */
   conversation_t conv;
   int result = conv_db_get(conv_id, conn->auth_user_id, &conv);

   if (result == AUTH_DB_SUCCESS) {
      json_object *msg_array = json_object_new_array();
      json_object *all_msgs = NULL; /* For session context restoration */
      int total_messages = 0;

      /* Full conversation in one query (no pagination — see function doc).  Active sessions
       * fetch into all_msgs so the same array feeds both UI display and context restore;
       * archived / no-session loads go straight into the display array. */
      if (needs_session_context && !conv.is_archived) {
         all_msgs = json_object_new_array();
         result = conv_db_get_messages(conv_id, conn->auth_user_id, load_msg_callback, all_msgs);

         if (result == AUTH_DB_SUCCESS) {
            total_messages = json_object_array_length(all_msgs);
            for (int i = 0; i < total_messages; i++) {
               json_object *msg = json_object_array_get_idx(all_msgs, i);
               json_object_get(msg); /* ref before adding to the display array */
               json_object_array_add(msg_array, msg);
            }
         }
      } else {
         /* conv_db_get_messages returns oldest-first — no reverse needed. */
         result = conv_db_get_messages(conv_id, conn->auth_user_id, load_msg_callback, msg_array);
         if (result == AUTH_DB_SUCCESS) {
            total_messages = json_object_array_length(msg_array);
         }
      }

      if (result == AUTH_DB_SUCCESS) {
         /* Restore to session context on initial load of non-archived conversations.
          * Skip if session already has conversation history (e.g., auto-create already
          * restored this conversation — re-restoring would wipe any user messages
          * that were added between the auto-create restore and this load request). */
         if (needs_session_context && !conv.is_archived && conn->session) {
            struct json_object *existing = session_get_history(conn->session);
            int existing_count = existing ? json_object_array_length(existing) : 0;
            if (existing)
               json_object_put(existing);

            /* Restore (replacing the session's in-memory history) on a fresh
             * session OR a switch to a DIFFERENT conversation.  Only skip when
             * re-loading the SAME conversation the session already holds — the
             * auto-create-already-restored case, where re-restoring would wipe
             * messages added since.  `active_conversation_id` still holds the
             * OLD conv here (it's updated to conv_id further below), so
             * `!= conv_id` reliably means "the user switched conversations".
             *
             * The previous guard skipped on message-count alone (existing_count
             * > 1), which wrongly skipped genuine switches: the session kept the
             * previous conversation's history for the LLM while the UI displayed
             * the newly-loaded one — messages then went to the wrong thread. */
            if (existing_count <= 1 || conn->active_conversation_id != conv_id) {
               /* v67: when a compaction watermark is set, the display array (all_msgs)
                * is the FULL transcript, but the LLM context must be bounded to
                * post-watermark messages.  Pass NULL so restore does its own bounded
                * fetch (conv_db_get_messages_after); display stays full. */
               json_object *restore_msgs = (conv.context_watermark_msg_id > 0) ? NULL : all_msgs;
               int restored = webui_restore_conversation_context(conn, &conv, conv_id,
                                                                 restore_msgs);
               if (restored >= 0) {
                  OLOG_INFO("WebUI: Restored %d messages to session %u context (conv %lld)",
                            restored, conn->session->session_id, (long long)conv_id);
               }
            } else {
               OLOG_INFO("WebUI: Skipped redundant restore for session %u "
                         "(same conversation %lld, already has %d messages)",
                         conn->session->session_id, (long long)conv_id, existing_count);
            }
         }

         /* Free all_msgs if it was allocated */
         if (all_msgs) {
            json_object_put(all_msgs);
         }

         if (conv.is_archived) {
            OLOG_INFO("WebUI: Loaded archived conversation %lld (read-only)", (long long)conv.id);
         }

         /* Build response */
         json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
         json_object_object_add(resp_payload, "conversation_id", json_object_new_int64(conv.id));
         json_object_object_add(resp_payload, "messages", msg_array);
         json_object_object_add(resp_payload, "total", json_object_new_int(total_messages));

         /* Conversation metadata (always included — full load, no pagination). */
         {
            json_object_object_add(resp_payload, "is_archived",
                                   json_object_new_boolean(conv.is_archived));
            json_object_object_add(resp_payload, "title", json_object_new_string(conv.title));
            json_object_object_add(resp_payload, "message_count",
                                   json_object_new_int(total_messages));
            json_object_object_add(resp_payload, "context_tokens",
                                   json_object_new_int(conv.context_tokens));
            json_object_object_add(resp_payload, "context_max",
                                   json_object_new_int(conv.context_max));

            /* Per-conversation LLM settings */
            json_object *llm_settings = json_object_new_object();
            json_object_object_add(llm_settings, "llm_type",
                                   json_object_new_string(conv.llm_type[0] ? conv.llm_type : ""));
            json_object_object_add(llm_settings, "cloud_provider",
                                   json_object_new_string(
                                       conv.cloud_provider[0] ? conv.cloud_provider : ""));
            json_object_object_add(llm_settings, "model",
                                   json_object_new_string(conv.model[0] ? conv.model : ""));
            json_object_object_add(llm_settings, "tools_mode",
                                   json_object_new_string(conv.tools_mode[0] ? conv.tools_mode
                                                                             : ""));
            json_object_object_add(llm_settings, "thinking_mode",
                                   json_object_new_string(conv.thinking_mode[0] ? conv.thinking_mode
                                                                                : ""));
            json_object_object_add(llm_settings, "reasoning_effort",
                                   json_object_new_string(
                                       conv.reasoning_effort[0] ? conv.reasoning_effort : ""));
            json_object_object_add(resp_payload, "llm_settings", llm_settings);

            json_object_object_add(resp_payload, "llm_locked",
                                   json_object_new_boolean(total_messages > 0));

            /* Privacy flag */
            json_object_object_add(resp_payload, "is_private",
                                   json_object_new_boolean(conv.is_private));

            /* Continuation data */
            if (conv.continued_from > 0) {
               json_object_object_add(resp_payload, "continued_from",
                                      json_object_new_int64(conv.continued_from));
               if (conv.compaction_summary) {
                  json_object_object_add(resp_payload, "compaction_summary",
                                         json_object_new_string(conv.compaction_summary));
               }
            }

            /* For archived conversations, find continuation ID */
            if (conv.is_archived) {
               int64_t continuation_id = 0;
               if (conv_db_find_continuation(conv.id, conn->auth_user_id, &continuation_id) ==
                       AUTH_DB_SUCCESS &&
                   continuation_id > 0) {
                  json_object_object_add(resp_payload, "continued_by",
                                         json_object_new_int64(continuation_id));
               }
            }
         }

         /* Update active conversation tracking */
         conn->active_conversation_id = conv_id;
         conn->active_conversation_private = conv.is_private;

         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn, response);
         json_object_put(response);

         /* If this conversation has an IN-FLIGHT turn, replay its partial so the
          * client shows the mid-stream state and resumes live rendering.  Only
          * when still active — a finished turn's final text is already in the
          * loaded messages above, so resuming it would double-render. */
         bool sr_active = false, sr_trunc = false;
         uint32_t sr_sid = 0;
         char *sr_partial = conv_stream_dup_partial(conv_id, &sr_active, &sr_sid, &sr_trunc);
         if (sr_partial != NULL) {
            if (sr_active) {
               struct json_object *sr = json_object_new_object();
               struct json_object *sp = json_object_new_object();
               json_object_object_add(sp, "conversation_id", json_object_new_int64(conv_id));
               json_object_object_add(sp, "stream_id", json_object_new_int((int32_t)sr_sid));
               json_object_object_add(sp, "partial", json_object_new_string(sr_partial));
               json_object_object_add(sp, "truncated", json_object_new_boolean(sr_trunc));
               json_object_object_add(sr, "type", json_object_new_string("stream_resume"));
               json_object_object_add(sr, "payload", sp);
               send_json_response(conn, sr);
               json_object_put(sr);
            }
            free(sr_partial);
         }

         conv_free(&conv);
         return;
      } else {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Failed to load messages"));
         json_object_put(msg_array);
      }

      conv_free(&conv);
   } else if (result == AUTH_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Conversation not found"));
   } else if (result == AUTH_DB_FORBIDDEN) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Access denied"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to load conversation"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/**
 * @brief Delete a conversation
 */
/* Accumulator for the conversation's referenced image ids.  We must NOT call
 * image_store_delete() from inside the conv_db_get_messages() callback: that
 * function holds the (non-recursive) auth_db mutex during iteration, and
 * image_store_delete() re-acquires it → deadlock.  So the callback only collects
 * ids (string copy, no lock), and the deletes run after the lock is released. */
typedef struct {
   char (*ids)[IMAGE_ID_LEN];
   int count;
   int cap;
} conv_image_id_acc_t;

static int collect_conv_images_cb(const conversation_message_t *msg, void *ctx) {
   conv_image_id_acc_t *acc = (conv_image_id_acc_t *)ctx;
   if (!msg || !msg->content) {
      return 0;
   }
   /* Per-message scan: the collect cap equals the per-turn upload cap
    * (WEBUI_MAX_VISION_IMAGES_CAP), so a single message can never carry more ids than
    * this buffer holds — no markers are missed for the cascade-delete. */
   char ids[WEBUI_MAX_VISION_IMAGES_CAP][IMAGE_ID_LEN];
   int count = 0;
   if (webui_collect_image_ids(msg->content, ids, WEBUI_MAX_VISION_IMAGES_CAP, &count) != SUCCESS) {
      return 0;
   }
   for (int i = 0; i < count; i++) {
      if (acc->count == acc->cap) {
         int new_cap = acc->cap ? acc->cap * 2 : 16;
         char(*grown)[IMAGE_ID_LEN] = realloc(acc->ids, (size_t)new_cap * IMAGE_ID_LEN);
         if (!grown) {
            return 0; /* OOM — delete what we collected; never block the conv delete */
         }
         acc->ids = grown;
         acc->cap = new_cap;
      }
      memcpy(acc->ids[acc->count], ids[i], IMAGE_ID_LEN);
      acc->count++;
   }
   return 0; /* continue iteration */
}

void handle_delete_conversation(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("delete_conversation_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get conversation ID */
   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "conversation_id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing conversation_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int64_t conv_id = json_object_get_int64(id_obj);

   /* Refuse to delete a live background-job conversation out from under its
    * running worker (which is still writing to it).  Sidebar-hiding is
    * presentation, not authorization — a conv id learned from `job list` is
    * still directly deletable, so guard here.  Cancel the job first. */
   char job_status[JOB_STATUS_MAX];
   if (conv_db_job_get_status(conv_id, conn->auth_user_id, job_status, sizeof(job_status)) ==
           AUTH_DB_SUCCESS &&
       (strcmp(job_status, "running") == 0 || strcmp(job_status, "queued") == 0)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string(
                                 "Cancel the background job before deleting it."));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   /* Cascade-delete referenced images BEFORE the row cascade removes the markers
    * (referenced images are conversation-lifecycle-owned, bumped to PERMANENT at save).
    * Collect ids UNDER conv_db_get_messages' lock, then delete AFTER it returns —
    * image_store_delete re-takes the same non-recursive auth_db mutex, so calling it
    * from inside the callback would deadlock. */
   conv_image_id_acc_t img_acc = { 0 };
   conv_db_get_messages(conv_id, conn->auth_user_id, collect_conv_images_cb, &img_acc);
   for (int i = 0; i < img_acc.count; i++) {
      image_store_delete(img_acc.ids[i], conn->auth_user_id);
   }
   free(img_acc.ids);

   int result = conv_db_delete(conv_id, conn->auth_user_id);

   if (result == AUTH_DB_SUCCESS) {
      /* Free any live-partial replay-ring entry for the deleted conversation. */
      conv_stream_clear(conv_id);

      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message",
                             json_object_new_string("Conversation deleted"));

      char details[64];
      snprintf(details, sizeof(details), "Deleted conversation %lld", (long long)conv_id);
      auth_db_log_event("CONVERSATION_DELETED", conn->username, conn->client_ip, details);
   } else if (result == AUTH_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Conversation not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to delete conversation"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/**
 * @brief Rename a conversation
 */
void handle_rename_conversation(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("rename_conversation_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get conversation ID and new title */
   json_object *id_obj, *title_obj;
   if (!json_object_object_get_ex(payload, "conversation_id", &id_obj) ||
       !json_object_object_get_ex(payload, "title", &title_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing conversation_id or title"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int64_t conv_id = json_object_get_int64(id_obj);
   const char *title = json_object_get_string(title_obj);

   if (!title || strlen(title) == 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Title cannot be empty"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int result = conv_db_rename(conv_id, conn->auth_user_id, title);

   if (result == AUTH_DB_SUCCESS) {
      conv_db_set_title_locked(conv_id, conn->auth_user_id, 1);
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message",
                             json_object_new_string("Conversation renamed"));
   } else if (result == AUTH_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Conversation not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to rename conversation"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/**
 * @brief Set private mode for a conversation
 *
 * Private conversations are excluded from memory extraction.
 */
void handle_set_private(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("set_private_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get conversation ID and private flag */
   json_object *id_obj, *private_obj;
   if (!json_object_object_get_ex(payload, "conversation_id", &id_obj) ||
       !json_object_object_get_ex(payload, "is_private", &private_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing conversation_id or is_private"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int64_t conv_id = json_object_get_int64(id_obj);
   bool is_private = json_object_get_boolean(private_obj);

   int result = conv_db_set_private(conv_id, conn->auth_user_id, is_private);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "conversation_id", json_object_new_int64(conv_id));
      json_object_object_add(resp_payload, "is_private", json_object_new_boolean(is_private));
      json_object_object_add(resp_payload, "message",
                             json_object_new_string(is_private ? "Conversation marked private"
                                                               : "Conversation marked public"));

      /* Update active conversation tracking if this is the current conversation */
      if (conn->active_conversation_id == conv_id) {
         conn->active_conversation_private = is_private;
      }
   } else if (result == AUTH_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Conversation not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to update privacy"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/**
 * @brief Pin or unpin a conversation
 *
 * Pinned conversations float to a dedicated section at the top of the WebUI
 * conversation list.
 */
void handle_set_pinned(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("set_pinned_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get conversation ID and pinned flag */
   json_object *id_obj, *pinned_obj;
   if (!json_object_object_get_ex(payload, "conversation_id", &id_obj) ||
       !json_object_object_get_ex(payload, "is_pinned", &pinned_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing conversation_id or is_pinned"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int64_t conv_id = json_object_get_int64(id_obj);
   bool is_pinned = json_object_get_boolean(pinned_obj);

   int result = conv_db_set_pinned(conv_id, conn->auth_user_id, is_pinned);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "conversation_id", json_object_new_int64(conv_id));
      json_object_object_add(resp_payload, "is_pinned", json_object_new_boolean(is_pinned));
      json_object_object_add(resp_payload, "message",
                             json_object_new_string(is_pinned ? "Conversation pinned"
                                                              : "Conversation unpinned"));
   } else if (result == AUTH_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Conversation not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to update pinned state"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/**
 * @brief Search conversations by title or content
 */
void handle_search_conversations(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("search_conversations_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get search query */
   json_object *query_obj;
   if (!json_object_object_get_ex(payload, "query", &query_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing query"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   const char *query = json_object_get_string(query_obj);
   json_object *conv_array = json_object_new_array();

   /* Check if we should search message content */
   bool search_content = false;
   json_object *search_content_obj;
   if (json_object_object_get_ex(payload, "search_content", &search_content_obj)) {
      search_content = json_object_get_boolean(search_content_obj);
   }

   /* Parse pagination params */
   conv_pagination_t pagination = { 0, 0 };
   json_object *limit_obj, *offset_obj;
   if (json_object_object_get_ex(payload, "limit", &limit_obj)) {
      pagination.limit = json_object_get_int(limit_obj);
   }
   if (json_object_object_get_ex(payload, "offset", &offset_obj)) {
      pagination.offset = json_object_get_int(offset_obj);
   }

   int result;
   if (search_content) {
      result = conv_db_search_content(conn->auth_user_id, query, &pagination, list_conv_callback,
                                      conv_array);
   } else {
      result = conv_db_search(conn->auth_user_id, query, &pagination, list_conv_callback,
                              conv_array);
   }

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "conversations", conv_array);
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to search conversations"));
      json_object_put(conv_array);
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/*
 * MESSAGE-ROW OWNERSHIP MAP (one tool-using turn writes several `messages` rows from
 * three different writers — keep these partitions disjoint to avoid double-writes/drops):
 *
 *   - User turn .......... CLIENT here (handle_save_message), EXCEPT a vision turn, which
 *                          text_input_dispatch.c persists server-side (it owns the image
 *                          markers); the client skips the save when vision_image_count > 0.
 *   - Tool iteration rows  DAEMON, via the persist hook (persist_appended_tool_turn →
 *     (assistant+tool) .... webui_tool_persist_cb), once per LLM tool-loop iteration; this
 *                          is also where per-iteration display-only `reasoning` is attached.
 *   - Final answer + its .. CLIENT here (handle_save_message); the in-memory final answer in
 *     reasoning ........... session_manager_llm.c is NOT written to conv_db (no double-write).
 *
 * Any change to one writer must preserve this partition; the tool-call-storage-refactor
 * TODO touches all three.
 */

/**
 * @brief Save a message to a conversation
 */
void handle_save_message(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("save_message_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get required fields */
   json_object *conv_id_obj, *role_obj, *content_obj;
   if (!json_object_object_get_ex(payload, "conversation_id", &conv_id_obj) ||
       !json_object_object_get_ex(payload, "role", &role_obj) ||
       !json_object_object_get_ex(payload, "content", &content_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing conversation_id, role, or content"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int64_t conv_id = json_object_get_int64(conv_id_obj);
   const char *role = json_object_get_string(role_obj);
   const char *content = json_object_get_string(content_obj);

   /* SECURITY: whitelist the role a client may persist.  Clients only ever save their
    * own 'user' turns and the assistant's visible answer; 'system'/'tool' rows are
    * daemon-owned (persisted server-side via the tool-persist hook).  Without this, a
    * crafted payload could write a 'system' row into the user's OWN conversation that
    * the restore path would later replay into their LLM context (self-prompt-shaping).
    * The DB CHECK constraint is a backstop, not the gate. */
   if (!role || (strcmp(role, "user") != 0 && strcmp(role, "assistant") != 0)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Invalid role"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   /* Optional display-only reasoning JSON for the final answer (E3).  Bounded by the same
    * limit as message content; over-limit is dropped (the "AI thought" panel simply won't
    * render for that message rather than storing an unbounded blob). */
   const char *reasoning = NULL;
   json_object *reasoning_obj;
   if (json_object_object_get_ex(payload, "reasoning", &reasoning_obj) &&
       json_object_is_type(reasoning_obj, json_type_string)) {
      const char *r = json_object_get_string(reasoning_obj);
      if (r && r[0] && strlen(r) <= CONV_MESSAGE_MAX) {
         reasoning = r;
      }
   }

   /* SECURITY: Validate any embedded image thumbnails (size limit, safe prefix) */
   if (!validate_image_marker(content)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Invalid or oversized image data"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   /* Clear pending_visual if present — the client-side streaming save
    * already interleaves the visual content at the correct position
    * (pre-visual text + <dawn-visual> tag + post-visual text). */
   if (strcmp(role, "assistant") == 0 && conn->session) {
      pthread_mutex_lock(&conn->session->tools_mutex);
      free(conn->session->pending_visual);
      conn->session->pending_visual = NULL;
      pthread_mutex_unlock(&conn->session->tools_mutex);
   }

   int64_t msg_id = 0;
   int result = conv_db_add_message_with_tools(conv_id, conn->auth_user_id, role, content, NULL,
                                               NULL, reasoning, &msg_id);

   if (result == AUTH_DB_SUCCESS) {
      if (conn->session && msg_id > 0)
         session_stamp_last_message_id(conn->session, role, msg_id);
      /* Promote any referenced images to PERMANENT so they survive age/LRU eviction
       * for the life of the conversation (conversation-lifecycle-owned).  Owner-checked
       * via conn->auth_user_id; an injected foreign id no-ops.  Per-message scan: the
       * collect cap equals the per-turn upload cap, so no referenced id is missed. */
      char img_ids[WEBUI_MAX_VISION_IMAGES_CAP][IMAGE_ID_LEN];
      int img_count = 0;
      if (webui_collect_image_ids(content, img_ids, WEBUI_MAX_VISION_IMAGES_CAP, &img_count) ==
          SUCCESS) {
         for (int i = 0; i < img_count; i++) {
            image_store_update_retention(img_ids[i], conn->auth_user_id, IMAGE_RETAIN_PERMANENT);
         }
      }
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   } else if (result == AUTH_DB_FORBIDDEN) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Access denied to conversation"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to save message"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/**
 * @brief Update context usage for a conversation
 */
void handle_update_context(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   /* Get required fields */
   json_object *conv_id_obj, *tokens_obj, *max_obj;
   if (!json_object_object_get_ex(payload, "conversation_id", &conv_id_obj) ||
       !json_object_object_get_ex(payload, "context_tokens", &tokens_obj) ||
       !json_object_object_get_ex(payload, "context_max", &max_obj)) {
      /* Silently ignore incomplete updates - context is optional */
      return;
   }

   int64_t conv_id = json_object_get_int64(conv_id_obj);
   int context_tokens = json_object_get_int(tokens_obj);
   int context_max = json_object_get_int(max_obj);

   /* Update in database - no response needed */
   conv_db_update_context(conv_id, conn->auth_user_id, context_tokens, context_max);
}

/**
 * @brief Lock LLM settings for a conversation
 *
 * Called when the first message is sent in a conversation.
 * Stores the current LLM settings and locks them for the conversation's lifetime.
 */
void handle_lock_conversation_llm(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("lock_conversation_llm_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get conversation ID */
   json_object *conv_id_obj;
   if (!json_object_object_get_ex(payload, "conversation_id", &conv_id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing conversation_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }
   int64_t conv_id = json_object_get_int64(conv_id_obj);

   /* Get LLM settings object */
   json_object *settings_obj;
   if (!json_object_object_get_ex(payload, "llm_settings", &settings_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing llm_settings"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   /* Extract settings fields */
   const char *llm_type = NULL;
   const char *cloud_provider = NULL;
   const char *model = NULL;
   const char *tools_mode = NULL;
   const char *thinking_mode = NULL;
   const char *reasoning_effort = NULL;

   json_object *val;
   if (json_object_object_get_ex(settings_obj, "llm_type", &val)) {
      llm_type = json_object_get_string(val);
   }
   if (json_object_object_get_ex(settings_obj, "cloud_provider", &val)) {
      cloud_provider = json_object_get_string(val);
   }
   if (json_object_object_get_ex(settings_obj, "model", &val)) {
      model = json_object_get_string(val);
   }
   if (json_object_object_get_ex(settings_obj, "tools_mode", &val)) {
      tools_mode = json_object_get_string(val);
   }
   if (json_object_object_get_ex(settings_obj, "thinking_mode", &val)) {
      thinking_mode = json_object_get_string(val);
   }
   if (json_object_object_get_ex(settings_obj, "reasoning_effort", &val)) {
      reasoning_effort = json_object_get_string(val);
   }

   /* Validate input lengths against database field sizes */
   if ((llm_type && strlen(llm_type) > 15) || (cloud_provider && strlen(cloud_provider) > 15) ||
       (model && strlen(model) > 63) || (tools_mode && strlen(tools_mode) > 15) ||
       (thinking_mode && strlen(thinking_mode) > 15) ||
       (reasoning_effort && strlen(reasoning_effort) > 15)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Field value too long"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   /* Lock settings in database (only works if message_count == 0) */
   int result = conv_db_lock_llm_settings(conv_id, conn->auth_user_id, llm_type, cloud_provider,
                                          model, tools_mode, thinking_mode, reasoning_effort);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "locked", json_object_new_boolean(1));
      OLOG_INFO("WebUI: Locked LLM settings for conversation %lld (user %d)", (long long)conv_id,
                conn->auth_user_id);
   } else if (result == AUTH_DB_NOT_FOUND) {
      /* Conversation already has messages - settings already locked */
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "locked", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "already_locked", json_object_new_boolean(1));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to lock settings"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/**
 * @brief Reassign a conversation to a different user (admin only)
 */
void handle_reassign_conversation(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_admin(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("reassign_conversation_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get required fields */
   json_object *conv_id_obj, *new_user_id_obj;
   if (!json_object_object_get_ex(payload, "conversation_id", &conv_id_obj) ||
       !json_object_object_get_ex(payload, "new_user_id", &new_user_id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing conversation_id or new_user_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int64_t conv_id = json_object_get_int64(conv_id_obj);
   int new_user_id = json_object_get_int(new_user_id_obj);

   if (conv_id <= 0 || new_user_id <= 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Invalid conversation_id or user_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   /* Perform the reassignment */
   int result = conv_db_reassign(conv_id, new_user_id);

   if (result == AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "conversation_id", json_object_new_int64(conv_id));
      json_object_object_add(resp_payload, "new_user_id", json_object_new_int(new_user_id));
      json_object_object_add(resp_payload, "message",
                             json_object_new_string("Conversation reassigned successfully"));
      OLOG_INFO("WebUI: Admin %s reassigned conversation %lld to user %d", conn->username,
                (long long)conv_id, new_user_id);
   } else if (result == AUTH_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Conversation not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to reassign conversation"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Conversation Export
 * ============================================================================ */

/**
 * @brief Format a time_t as ISO 8601 UTC string
 */
static void time_to_iso8601(time_t t, char *buf, size_t buf_size) {
   struct tm utc;
   gmtime_r(&t, &utc);
   strftime(buf, buf_size, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

/**
 * @brief Message callback for export — builds messages with ISO 8601 timestamps
 */
static int export_msg_callback(const conversation_message_t *msg, void *context) {
   json_object *msg_array = (json_object *)context;
   json_object *msg_obj = json_object_new_object();

   json_object_object_add(msg_obj, "role", json_object_new_string(msg->role));
   json_object_object_add(msg_obj, "content",
                          json_object_new_string(msg->content ? msg->content : ""));

   char ts[32];
   time_to_iso8601(msg->created_at, ts, sizeof(ts));
   json_object_object_add(msg_obj, "timestamp", json_object_new_string(ts));

   json_object_array_add(msg_array, msg_obj);
   return 0;
}

/**
 * @brief Export a conversation as a self-contained JSON document
 *
 * Returns the full conversation with metadata and all messages in a format
 * suitable for backup, sharing, or analysis. Messages include ISO 8601
 * timestamps. Image markers appear as text; base64 data is not included.
 */
/* Guard against concurrent exports to prevent memory spikes on Jetson shared memory */
static volatile int s_export_in_progress = 0;

void handle_export_conversation(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   /* Concurrency guard: only one export at a time to limit peak memory */
   if (__atomic_exchange_n(&s_export_in_progress, 1, __ATOMIC_SEQ_CST)) {
      json_object *busy = json_object_new_object();
      json_object_object_add(busy, "type", json_object_new_string("export_conversation_response"));
      json_object *bp = json_object_new_object();
      json_object_object_add(bp, "success", json_object_new_boolean(0));
      json_object_object_add(bp, "error",
                             json_object_new_string("Another export is in progress, try again"));
      json_object_object_add(busy, "payload", bp);
      send_json_response(conn, busy);
      json_object_put(busy);
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("export_conversation_response"));
   json_object *resp_payload = json_object_new_object();

   /* Extract conversation ID */
   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "conversation_id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing conversation_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      __atomic_store_n(&s_export_in_progress, 0, __ATOMIC_SEQ_CST);
      return;
   }

   int64_t conv_id = json_object_get_int64(id_obj);

   /* Fetch conversation metadata */
   conversation_t conv;
   int result = conv_db_get(conv_id, conn->auth_user_id, &conv);

   if (result != AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      const char *err = (result == AUTH_DB_NOT_FOUND)   ? "Conversation not found"
                        : (result == AUTH_DB_FORBIDDEN) ? "Access denied"
                                                        : "Database error";
      json_object_object_add(resp_payload, "error", json_object_new_string(err));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      __atomic_store_n(&s_export_in_progress, 0, __ATOMIC_SEQ_CST);
      return;
   }

   /* Check export message cap */
   int max_msgs = g_config.webui.export_max_messages;
   if (max_msgs > 0 && conv.message_count > max_msgs) {
      char err_buf[128];
      snprintf(err_buf, sizeof(err_buf),
               "Conversation has %d messages, exceeding export limit of %d", conv.message_count,
               max_msgs);
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string(err_buf));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      conv_free(&conv);
      __atomic_store_n(&s_export_in_progress, 0, __ATOMIC_SEQ_CST);
      return;
   }

   /* Fetch all messages */
   json_object *messages = json_object_new_array();
   result = conv_db_get_messages(conv_id, conn->auth_user_id, export_msg_callback, messages);

   if (result != AUTH_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to load messages"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      json_object_put(messages);
      conv_free(&conv);
      __atomic_store_n(&s_export_in_progress, 0, __ATOMIC_SEQ_CST);
      return;
   }

   /* Build export document */
   json_object *export_doc = json_object_new_object();

   /* Top-level metadata */
   char now_str[32];
   time_to_iso8601(time(NULL), now_str, sizeof(now_str));
   json_object_object_add(export_doc, "exported_at", json_object_new_string(now_str));
   json_object_object_add(export_doc, "dawn_version", json_object_new_string(VERSION_NUMBER));

   /* Conversation metadata */
   json_object *conv_obj = json_object_new_object();
   json_object_object_add(conv_obj, "id", json_object_new_int64(conv.id));
   json_object_object_add(conv_obj, "title", json_object_new_string(conv.title));

   char ts_buf[32];
   time_to_iso8601(conv.created_at, ts_buf, sizeof(ts_buf));
   json_object_object_add(conv_obj, "created_at", json_object_new_string(ts_buf));
   time_to_iso8601(conv.updated_at, ts_buf, sizeof(ts_buf));
   json_object_object_add(conv_obj, "updated_at", json_object_new_string(ts_buf));

   json_object_object_add(conv_obj, "message_count", json_object_new_int(conv.message_count));
   json_object_object_add(conv_obj, "origin", json_object_new_string(conv.origin));
   json_object_object_add(conv_obj, "is_private", json_object_new_boolean(conv.is_private));
   json_object_object_add(conv_obj, "continued_from",
                          conv.continued_from ? json_object_new_int64(conv.continued_from) : NULL);

   /* LLM settings */
   json_object *llm_obj = json_object_new_object();
   json_object_object_add(llm_obj, "llm_type", json_object_new_string(conv.llm_type));
   json_object_object_add(llm_obj, "cloud_provider", json_object_new_string(conv.cloud_provider));
   json_object_object_add(llm_obj, "model", json_object_new_string(conv.model));
   json_object_object_add(llm_obj, "tools_mode", json_object_new_string(conv.tools_mode));
   json_object_object_add(llm_obj, "thinking_mode", json_object_new_string(conv.thinking_mode));
   json_object_object_add(conv_obj, "llm_settings", llm_obj);

   json_object_object_add(export_doc, "conversation", conv_obj);
   json_object_object_add(export_doc, "messages", messages);

   /* Echo requested format (client uses this to select JSON vs HTML download) */
   json_object *fmt_obj;
   const char *format = "json";
   if (json_object_object_get_ex(payload, "format", &fmt_obj)) {
      const char *req_fmt = json_object_get_string(fmt_obj);
      if (req_fmt && (strcmp(req_fmt, "json") == 0 || strcmp(req_fmt, "html") == 0)) {
         format = req_fmt;
      }
   }

   /* Send response */
   json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   json_object_object_add(resp_payload, "format", json_object_new_string(format));
   json_object_object_add(resp_payload, "data", export_doc);
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);

   OLOG_INFO("WebUI: Exported conversation %lld (%d messages) for user %s", (long long)conv_id,
             conv.message_count, conn->username);

   conv_free(&conv);
   __atomic_store_n(&s_export_in_progress, 0, __ATOMIC_SEQ_CST);
}
