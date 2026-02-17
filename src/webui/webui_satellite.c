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
 * WebUI Satellite Handler - DAP2 Tier 1 satellite support via WebSocket
 */

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#include "core/session_manager.h"
#include "logging.h"
#include "webui/webui_internal.h"

/* Maximum concurrent satellite worker threads (LLM calls).
 * request_generation prevents stale results, but each thread still holds
 * ~8MB stack until it checks the generation counter after the LLM call. */
#define MAX_SATELLITE_WORKERS 8
atomic_int g_active_satellite_workers = 0;

/* =============================================================================
 * Rate Limiting for Satellite Registration
 *
 * Prevents DoS attacks by limiting registration attempts per IP address.
 * Uses a simple hash table with IP-based tracking.
 * ============================================================================= */

#define RATE_LIMIT_BUCKETS 64
#define RATE_LIMIT_MAX_REGISTRATIONS 5
#define RATE_LIMIT_WINDOW_SEC 60

typedef struct {
   uint32_t ip_hash;
   char ip_str[46]; /* Store actual IP for collision detection (IPv6 max 45 + null) */
   time_t window_start;
   int count;
} rate_limit_entry_t;

static rate_limit_entry_t g_rate_limits[RATE_LIMIT_BUCKETS] = { 0 };
static pthread_mutex_t g_rate_limit_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * Check if registration is rate-limited for given IP address.
 * Returns true if request should be denied, false if allowed.
 */
static bool is_rate_limited(const char *client_ip) {
   if (!client_ip)
      return false;

   /* Simple FNV-1a hash of IP string */
   uint32_t hash = 2166136261u;
   for (const char *p = client_ip; *p; p++) {
      hash ^= (unsigned char)*p;
      hash *= 16777619u;
   }

   int bucket = hash % RATE_LIMIT_BUCKETS;
   time_t now = time(NULL);

   pthread_mutex_lock(&g_rate_limit_mutex);

   rate_limit_entry_t *entry = &g_rate_limits[bucket];

   /* Check if this is a different IP (hash collision + string mismatch) or window expired */
   if (entry->ip_hash != hash || strcmp(entry->ip_str, client_ip) != 0 ||
       (now - entry->window_start) >= RATE_LIMIT_WINDOW_SEC) {
      /* Start new window */
      entry->ip_hash = hash;
      strncpy(entry->ip_str, client_ip, sizeof(entry->ip_str) - 1);
      entry->ip_str[sizeof(entry->ip_str) - 1] = '\0';
      entry->window_start = now;
      entry->count = 1;
      pthread_mutex_unlock(&g_rate_limit_mutex);
      return false;
   }

   /* Same IP within window - check count */
   if (++entry->count > RATE_LIMIT_MAX_REGISTRATIONS) {
      pthread_mutex_unlock(&g_rate_limit_mutex);
      LOG_WARNING("Rate limit exceeded for IP %s (bucket %d): %d registrations in %lds", client_ip,
                  bucket, entry->count, (long)(now - entry->window_start));
      return true;
   }

   pthread_mutex_unlock(&g_rate_limit_mutex);
   return false;
}

/* =============================================================================
 * Satellite Response Queue Functions
 *
 * Satellites receive the same message types as WebUI clients, so we reuse
 * the existing response queue infrastructure.
 * ============================================================================= */

void satellite_send_response(session_t *session, const char *text) {
   if (!session || session->type != SESSION_TYPE_DAP2 || !text) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_TRANSCRIPT,
                          .transcript = {
                              .role = strdup("satellite_response"),
                              .text = strdup(text),
                          } };

   if (!resp.transcript.role || !resp.transcript.text) {
      free(resp.transcript.role);
      free(resp.transcript.text);
      LOG_ERROR("Satellite: Failed to allocate response");
      return;
   }

   queue_response(&resp);
}

void satellite_send_stream_start(session_t *session) {
   if (!session || session->type != SESSION_TYPE_DAP2) {
      return;
   }

   uint32_t sid = atomic_fetch_add(&session->current_stream_id, 1) + 1;
   atomic_store(&session->llm_streaming_active, true);

   /* Reset command tag filter state for new stream */
   session->cmd_tag_filter.nesting_depth = 0;
   session->cmd_tag_filter.len = 0;

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_STREAM_START,
                          .stream = {
                              .stream_id = sid,
                              .text = "",
                          } };

   queue_response(&resp);
   LOG_INFO("Satellite: Stream start id=%u for session %u (satellite %s)", sid, session->session_id,
            session->identity.name);
}

void satellite_send_stream_end(session_t *session, const char *reason) {
   if (!session || session->type != SESSION_TYPE_DAP2) {
      return;
   }

   atomic_store(&session->llm_streaming_active, false);

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_STREAM_END,
                          .stream = {
                              .stream_id = session->current_stream_id,
                          } };

   const char *r = reason ? reason : "complete";
   strncpy(resp.stream.text, r, sizeof(resp.stream.text) - 1);
   resp.stream.text[sizeof(resp.stream.text) - 1] = '\0';

   queue_response(&resp);
   LOG_INFO("Satellite: Stream end id=%u reason=%s for session %u", session->current_stream_id, r,
            session->session_id);
}

void satellite_send_error(session_t *session, const char *code, const char *message) {
   if (!session || session->type != SESSION_TYPE_DAP2) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_ERROR,
                          .error = {
                              .code = strdup(code ? code : "UNKNOWN"),
                              .message = strdup(message ? message : "Unknown error"),
                          } };

   if (!resp.error.code || !resp.error.message) {
      free(resp.error.code);
      free(resp.error.message);
      LOG_ERROR("Satellite: Failed to allocate error response");
      return;
   }

   queue_response(&resp);
}

void satellite_send_state(session_t *session, const char *state) {
   if (!session || session->type != SESSION_TYPE_DAP2 || !state) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_STATE,
                          .state = {
                              .state = strdup(state),
                              .detail = NULL,
                              .tools_json = NULL,
                          } };

   if (!resp.state.state) {
      LOG_ERROR("Satellite: Failed to allocate state response");
      return;
   }

   queue_response(&resp);
}

/* =============================================================================
 * Satellite Worker Thread
 *
 * Processes text queries from satellites through LLM.
 * Uses the same streaming infrastructure as WebUI (webui_send_stream_*).
 * ============================================================================= */

typedef struct {
   session_t *session;
   char *text;
   unsigned int request_gen;
} satellite_work_t;

/**
 * @brief Strip command tags from text in-place (shared by satellite + audio workers)
 */
void strip_command_tags(char *text) {
   if (!text)
      return;

   char *cmd_start, *cmd_end;
   while ((cmd_start = strstr(text, "<command>")) != NULL) {
      cmd_end = strstr(cmd_start, "</command>");
      if (cmd_end) {
         cmd_end += strlen("</command>");
         memmove(cmd_start, cmd_end, strlen(cmd_end) + 1);
      } else {
         break;
      }
   }

   /* Also remove <end_of_turn> tags (local AI models) */
   char *match = strstr(text, "<end_of_turn>");
   if (match) {
      *match = '\0';
   }
}

static void *satellite_worker_thread(void *arg) {
   satellite_work_t *work = (satellite_work_t *)arg;
   session_t *session = work->session;
   char *text = work->text;
   unsigned int expected_gen = work->request_gen;
   char *response = NULL;

   /* Worker count already incremented in handle_satellite_query before pthread_create */

   /* Check if session is still valid or if this request was superseded */
   if (!session || REQUEST_SUPERSEDED(session, expected_gen)) {
      LOG_INFO("Satellite: Session disconnected or request superseded, aborting");
      goto cleanup;
   }

   LOG_INFO("Satellite: Processing query from %s (len=%zu)", session->identity.name, strlen(text));

   /* Send "thinking" state with detail */
   webui_send_state_with_detail(session, "thinking", "Processing your request...");

   /* Add user message to history */
   session_add_message(session, "user", text);

   /* Call LLM with TTS callback for Tier 2 satellites (server-side TTS).
    * Tier 1 satellites have local TTS and only need the text response, but
    * Tier 2 devices need the daemon to synthesize speech and send PCM audio. */
   bool needs_server_tts = !session->capabilities.local_tts;
   response = session_llm_call_with_tts_vision_no_add(
       session, text, NULL, NULL, NULL, 0, needs_server_tts ? webui_sentence_audio_callback : NULL,
       needs_server_tts ? session : NULL);

   /* Check if request was superseded during LLM call */
   if (REQUEST_SUPERSEDED(session, expected_gen)) {
      LOG_INFO("Satellite: Request superseded during LLM call");
      goto cleanup;
   }

   if (!response) {
      satellite_send_error(session, "LLM_ERROR", "Failed to get response from AI");
      satellite_send_state(session, "idle");
      goto cleanup;
   }

   /* Check for command tags and process them using the existing infrastructure */
   if (strstr(response, "<command>")) {
      LOG_INFO("Satellite: Response contains commands, processing...");

      char *processed = webui_process_commands(response, session);
      if (processed && !REQUEST_SUPERSEDED(session, expected_gen) && !session->disconnected) {
         /* Recursively process if the follow-up also contains commands */
         int iterations = 0;
         const int MAX_ITERATIONS = 5;

         while (strstr(processed, "<command>") && !REQUEST_SUPERSEDED(session, expected_gen) &&
                !session->disconnected) {
            if (++iterations > MAX_ITERATIONS) {
               LOG_WARNING("Satellite: Command loop limit reached (%d iterations)", MAX_ITERATIONS);
               break;
            }

            LOG_INFO("Satellite: Follow-up contains more commands, processing (iter %d/%d)",
                     iterations, MAX_ITERATIONS);

            char *next_processed = webui_process_commands(processed, session);
            free(processed);
            if (!next_processed) {
               processed = NULL;
               break;
            }
            processed = next_processed;
         }

         if (processed) {
            free(response);
            response = processed;
         }
      } else {
         free(processed);
      }
   }

   if (REQUEST_SUPERSEDED(session, expected_gen))
      goto cleanup;

   /* Strip any remaining command tags from final response */
   strip_command_tags(response);

   /* Send stream end if streaming was active */
   if (atomic_load(&session->llm_streaming_active)) {
      satellite_send_stream_end(session, "complete");
   }

   /* Return to idle state */
   satellite_send_state(session, "idle");

   /* Mark interaction complete for conversation idle timeout tracking */
   session_update_interaction_complete(session);

cleanup:
   if (session)
      session_release(session);
   free(response);
   free(text);
   free(work);
   atomic_fetch_sub(&g_active_satellite_workers, 1);
   return NULL;
}

/* =============================================================================
 * Message Handlers
 * ============================================================================= */

void handle_satellite_register(ws_connection_t *conn, struct json_object *payload) {
   if (!conn || !payload) {
      LOG_WARNING("Satellite: Invalid register request");
      return;
   }

   /* Rate limiting check - prevent DoS via registration spam */
   if (is_rate_limited(conn->client_ip)) {
      send_error_impl(conn->wsi, "RATE_LIMITED",
                      "Too many registration attempts. Try again in 60 seconds.");
      return;
   }

   /* Extract registration fields */
   struct json_object *uuid_obj, *name_obj, *location_obj, *tier_obj, *caps_obj;

   if (!json_object_object_get_ex(payload, "uuid", &uuid_obj)) {
      send_error_impl(conn->wsi, "INVALID_MESSAGE", "Missing 'uuid' in satellite_register");
      return;
   }

   const char *uuid = json_object_get_string(uuid_obj);
   if (!uuid || strlen(uuid) != 36) {
      send_error_impl(conn->wsi, "INVALID_MESSAGE", "Invalid UUID format");
      return;
   }

   /* Validate UUID format: 8-4-4-4-12 hex pattern (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx) */
   {
      static const int dash_pos[] = { 8, 13, 18, 23 };
      bool valid = true;
      for (int i = 0; i < 36 && valid; i++) {
         if (i == dash_pos[0] || i == dash_pos[1] || i == dash_pos[2] || i == dash_pos[3]) {
            valid = (uuid[i] == '-');
         } else {
            char c = uuid[i];
            valid = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
         }
      }
      if (!valid) {
         send_error_impl(conn->wsi, "INVALID_MESSAGE", "UUID must be hex with dashes (8-4-4-4-12)");
         return;
      }
   }

   /* Optional fields with defaults (null-check for non-string JSON types) */
   const char *name = "Satellite";
   if (json_object_object_get_ex(payload, "name", &name_obj)) {
      const char *n = json_object_get_string(name_obj);
      if (n)
         name = n;
   }

   const char *location = "";
   if (json_object_object_get_ex(payload, "location", &location_obj)) {
      const char *l = json_object_get_string(location_obj);
      if (l)
         location = l;
   }

   int tier = 1; /* Default to Tier 1 */
   if (json_object_object_get_ex(payload, "tier", &tier_obj)) {
      tier = json_object_get_int(tier_obj);
   }

   if (tier != 1 && tier != 2) {
      send_error_impl(conn->wsi, "INVALID_MESSAGE", "Invalid tier (must be 1 or 2)");
      return;
   }

   /* Parse capabilities (defaults match Tier 1: local ASR + TTS) */
   dap2_capabilities_t caps = { .local_asr = true, .local_tts = true, .wake_word = true };
   if (json_object_object_get_ex(payload, "capabilities", &caps_obj)) {
      struct json_object *asr_obj, *tts_obj, *ww_obj;
      if (json_object_object_get_ex(caps_obj, "local_asr", &asr_obj)) {
         caps.local_asr = json_object_get_boolean(asr_obj);
      }
      if (json_object_object_get_ex(caps_obj, "local_tts", &tts_obj)) {
         caps.local_tts = json_object_get_boolean(tts_obj);
      }
      if (json_object_object_get_ex(caps_obj, "wake_word", &ww_obj)) {
         caps.wake_word = json_object_get_boolean(ww_obj);
      }
   }

   /* Validate tier matches declared capabilities to prevent resource abuse.
    * Tier 2 relies on server-side ASR+TTS, so must NOT claim local capabilities. */
   if (tier == 2 && (caps.local_asr || caps.local_tts)) {
      LOG_WARNING("Satellite: Tier 2 registration rejected — claims local_asr=%d local_tts=%d",
                  caps.local_asr, caps.local_tts);
      send_error_impl(conn->wsi, "INVALID_MESSAGE",
                      "Tier 2 satellites must not declare local_asr or local_tts");
      return;
   }

   /* Check for reconnect_secret (provided during reconnection attempts) */
   const char *reconnect_secret = "";
   struct json_object *secret_obj;
   if (json_object_object_get_ex(payload, "reconnect_secret", &secret_obj)) {
      const char *s = json_object_get_string(secret_obj);
      if (s)
         reconnect_secret = s;
   }

   /* Build identity */
   dap2_identity_t identity;
   memset(&identity, 0, sizeof(identity));
   strncpy(identity.uuid, uuid, sizeof(identity.uuid) - 1);
   strncpy(identity.name, name, sizeof(identity.name) - 1);
   strncpy(identity.location, location, sizeof(identity.location) - 1);

   /* Sanitize name/location: strip non-printable chars to prevent log injection */
   for (char *p = identity.name; *p; p++) {
      if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E)
         *p = '_';
   }
   for (char *p = identity.location; *p; p++) {
      if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E)
         *p = '_';
   }

   /* Include reconnect_secret if client provided one (for session reclamation) */
   if (reconnect_secret && reconnect_secret[0]) {
      strncpy(identity.reconnect_secret, reconnect_secret, sizeof(identity.reconnect_secret) - 1);
   }

   /* Create or reconnect session */
   dap2_tier_t dap2_tier = (tier == 1) ? DAP2_TIER_1 : DAP2_TIER_2;
   session_t *session = session_create_dap2(-1, /* No raw socket, using WebSocket */
                                            dap2_tier, &identity, &caps);

   if (!session) {
      send_error_impl(conn->wsi, "SESSION_ERROR", "Failed to create satellite session");
      return;
   }

   /* Attach session to WebSocket connection */
   conn->session = session;
   session->client_data = conn;

   /* Tier 2 satellites rely on server-side TTS — enable audio output.
    * Tier 1 does local TTS so leave tts_enabled at its default (false). */
   if (tier == 2) {
      conn->tts_enabled = true;
   }

   /* Get reconnect secret for client to save */
   char *session_secret = session_get_reconnect_secret(session);

   LOG_INFO("Satellite: Registered '%s' (%s) tier=%d location='%s' session=%u", identity.name,
            identity.uuid, tier, identity.location, session->session_id);

   /* Send registration acknowledgment with reconnect secret
    * SECURITY: Client MUST save this secret and provide it on reconnection.
    * Without the correct secret, reconnection attempts create new sessions. */
   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("satellite_register_ack"));

   struct json_object *resp_payload = json_object_new_object();
   json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   json_object_object_add(resp_payload, "session_id", json_object_new_int(session->session_id));

   /* Include reconnect_secret for secure session reclamation */
   if (session_secret) {
      json_object_object_add(resp_payload, "reconnect_secret",
                             json_object_new_string(session_secret));
      free(session_secret);
   }

   /* Generate session token for music WebSocket auth (same pattern as WebUI init).
    * Satellites need this to authenticate to the music streaming port (main_port + 1). */
   char music_token[WEBUI_SESSION_TOKEN_LEN];
   if (generate_session_token(music_token) == 0) {
      register_token(music_token, session->session_id);
      strncpy(conn->session_token, music_token, WEBUI_SESSION_TOKEN_LEN - 1);
      conn->session_token[WEBUI_SESSION_TOKEN_LEN - 1] = '\0';
      json_object_object_add(resp_payload, "session_token", json_object_new_string(music_token));
   }

   json_object_object_add(resp_payload, "message",
                          json_object_new_string("Satellite registered successfully"));

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

void handle_satellite_query(ws_connection_t *conn, struct json_object *payload) {
   if (!conn || !payload) {
      LOG_WARNING("Satellite: Invalid query request");
      return;
   }

   /* Verify session exists and is DAP2 */
   session_t *session = conn->session;
   if (!session || session->type != SESSION_TYPE_DAP2) {
      send_error_impl(conn->wsi, "NOT_REGISTERED",
                      "Satellite must register before sending queries");
      return;
   }

   /* Extract query text */
   struct json_object *text_obj;
   if (!json_object_object_get_ex(payload, "text", &text_obj)) {
      send_error_impl(conn->wsi, "INVALID_MESSAGE", "Missing 'text' in satellite_query");
      return;
   }

   const char *text = json_object_get_string(text_obj);
   if (!text || strlen(text) == 0) {
      send_error_impl(conn->wsi, "INVALID_MESSAGE", "Empty query text");
      return;
   }

   /* Cap query length to prevent resource exhaustion (memory + LLM API cost) */
   if (strlen(text) > 8192) {
      send_error_impl(conn->wsi, "INVALID_MESSAGE", "Query text too long (max 8192 chars)");
      return;
   }

   /* Increment request generation (supersedes any pending request) */
   atomic_fetch_add(&session->request_generation, 1);

   /* Create work item */
   satellite_work_t *work = calloc(1, sizeof(satellite_work_t));
   if (!work) {
      send_error_impl(conn->wsi, "INTERNAL_ERROR", "Memory allocation failed");
      return;
   }

   session_retain(session);
   work->session = session;
   work->text = strdup(text);
   work->request_gen = session->request_generation;

   if (!work->text) {
      session_release(session);
      free(work);
      send_error_impl(conn->wsi, "INTERNAL_ERROR", "Memory allocation failed");
      return;
   }

   /* Atomically claim a worker slot (prevents TOCTOU race on the limit check) */
   int prev = atomic_fetch_add(&g_active_satellite_workers, 1);
   if (prev >= MAX_SATELLITE_WORKERS) {
      atomic_fetch_sub(&g_active_satellite_workers, 1);
      LOG_WARNING("Satellite: Worker limit reached (%d), rejecting query from %s",
                  MAX_SATELLITE_WORKERS, session->identity.name);
      send_error_impl(conn->wsi, "BUSY", "Server busy processing other requests");
      session_release(session);
      free(work->text);
      free(work);
      return;
   }

   /* Launch worker thread (512KB stack — worker does HTTP LLM call, tool execution
    * including memory search with ~30KB stack arrays, and string processing) */
   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   pthread_attr_setstacksize(&attr, 512 * 1024);

   int ret = pthread_create(&thread, &attr, satellite_worker_thread, work);
   pthread_attr_destroy(&attr);

   if (ret != 0) {
      atomic_fetch_sub(&g_active_satellite_workers, 1);
      LOG_ERROR("Satellite: Failed to create worker thread: %d", ret);
      session_release(session);
      free(work->text);
      free(work);
      send_error_impl(conn->wsi, "INTERNAL_ERROR", "Failed to start processing");
      return;
   }

   LOG_INFO("Satellite: Query queued for %s: %.50s%s", session->identity.name, text,
            strlen(text) > 50 ? "..." : "");
}

void handle_satellite_ping(ws_connection_t *conn) {
   if (!conn) {
      return;
   }

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("satellite_pong"));
   send_json_response(conn->wsi, response);
   json_object_put(response);

   /* Touch session if exists */
   if (conn->session) {
      session_touch(conn->session);
   }
}
