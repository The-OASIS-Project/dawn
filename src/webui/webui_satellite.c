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

#include "webui/webui_satellite.h"

#include <json-c/json.h>
#include <pthread.h>
#include <sodium.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/ota.h"
#include "core/ota_db.h"
#include "core/rate_limiter.h"
#include "core/session_manager.h"
#include "core/utterance_dedup.h"
#include "logging.h"
#include "tools/volume_tool.h"
#include "webui/webui_internal.h"

/* Maximum concurrent satellite worker threads (LLM calls).
 * request_generation prevents stale results, but each thread still holds
 * ~8MB stack until it checks the generation counter after the LLM call. */
#define MAX_SATELLITE_WORKERS 8
atomic_int g_active_satellite_workers = 0;

/* =============================================================================
 * Rate Limiting for Satellite Registration
 *
 * Rides on the canonical core/rate_limiter primitive (same path as
 * webui_http.c's csrf/login/service-token limiters): linear-scan slots
 * with LRU eviction (no hash buckets, so no hash-collision bypass), IPv6
 * /64 normalization, and fail-closed on truncation.
 *
 * Migrated from a per-file FNV-bucket reimplementation 2026-05-19 — see
 * docs/TODO.md "Extract shared webui_rate_limit.{c,h}".
 * ============================================================================= */

#define REG_RATE_LIMIT_SLOTS 64
#define REG_RATE_LIMIT_MAX 5
#define REG_RATE_LIMIT_WINDOW_SEC 60

static rate_limit_entry_t s_reg_rate_entries[REG_RATE_LIMIT_SLOTS];
static rate_limiter_t s_reg_rate_limiter = RATE_LIMITER_STATIC_INIT(s_reg_rate_entries,
                                                                    REG_RATE_LIMIT_SLOTS,
                                                                    REG_RATE_LIMIT_MAX,
                                                                    REG_RATE_LIMIT_WINDOW_SEC);

/**
 * Check if registration is rate-limited for given IP address.
 * Returns true if request should be denied, false if allowed.
 */
static bool is_rate_limited(const char *client_ip) {
   if (!client_ip)
      return false;

   /* /64 normalization for IPv6 prevents per-address bypass within a
    * single network; IPv4 passes through unchanged. */
   char normalized_ip[RATE_LIMIT_IP_SIZE];
   rate_limiter_normalize_ip(client_ip, normalized_ip, sizeof(normalized_ip));

   if (rate_limiter_check(&s_reg_rate_limiter, normalized_ip)) {
      OLOG_WARNING("Rate limit exceeded for IP %s (normalized: %s)", client_ip, normalized_ip);
      return true;
   }
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
      OLOG_ERROR("Satellite: Failed to allocate response");
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
   OLOG_INFO("Satellite: Stream start id=%u for session %u (satellite %s)", sid,
             session->session_id, session->identity.name);
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
   OLOG_INFO("Satellite: Stream end id=%u reason=%s for session %u", session->current_stream_id, r,
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
      OLOG_ERROR("Satellite: Failed to allocate error response");
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
      OLOG_ERROR("Satellite: Failed to allocate state response");
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
      OLOG_INFO("Satellite: Session disconnected or request superseded, aborting");
      goto cleanup;
   }

   /* Cross-device dedup: if the local mic or another satellite already produced
    * a command event within the window, this one is a duplicate of the same
    * spoken utterance — drop it and return to idle. */
   if (utterance_dedup_check(session->session_id)) {
      OLOG_INFO("Satellite: Dedup suppressed duplicate utterance from %s (len=%zu)",
                session->identity.name, strlen(text));
      /* The satellite sits in VOICE_STATE_WAITING until a stream_end fires its
       * response_complete flag; a bare "idle" state only bumps its activity
       * timer, so it would spin until the 50s response timeout.  Send stream_end
       * (empty response = nothing spoken) to release it immediately. */
      satellite_send_stream_end(session, "complete");
      satellite_send_state(session, "idle");
      goto cleanup;
   }

   OLOG_INFO("Satellite: Processing query from %s (len=%zu)", session->identity.name, strlen(text));

   /* Send "thinking" state with detail */
   webui_send_state_with_detail(session, "thinking", "Processing your request...");

   /* Add user message to history */
   session_add_message(session, "user", text);

   /* Phase 1e: per-turn focus injection.  Synchronous; runs on this
    * satellite_worker_thread (spawned via pthread_create from
    * handle_satellite_query — NEVER on the lws service thread). */
   session_dispatch_user_turn(session, text);

   /* Call LLM with TTS callback for Tier 2 satellites (server-side TTS).
    * Tier 1 satellites have local TTS and only need the text response, but
    * Tier 2 devices need the daemon to synthesize speech and send PCM audio. */
   bool needs_server_tts = !session->capabilities.local_tts;
   response = session_llm_call_with_tts_vision_no_add(
       session, text, NULL, NULL, NULL, 0, needs_server_tts ? webui_sentence_audio_callback : NULL,
       needs_server_tts ? session : NULL);

   /* Check if request was superseded during LLM call */
   if (REQUEST_SUPERSEDED(session, expected_gen)) {
      OLOG_INFO("Satellite: Request superseded during LLM call");
      goto cleanup;
   }

   if (!response) {
      satellite_send_error(session, "LLM_ERROR", "Failed to get response from AI");
      satellite_send_state(session, "idle");
      goto cleanup;
   }

   if (REQUEST_SUPERSEDED(session, expected_gen))
      goto cleanup;

   /* Native tool calling actuated any device actions during the LLM call.
    * Defensively strip residual tags from the final response. */
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
      OLOG_WARNING("Satellite: Invalid register request");
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

   /* Validate registration key if daemon has one configured */
   {
      const secrets_config_t *secrets = config_get_secrets();
      if (secrets && secrets->satellite_registration_key[0]) {
         struct json_object *key_obj;
         const char *provided_key = NULL;
         if (json_object_object_get_ex(payload, "registration_key", &key_obj)) {
            provided_key = json_object_get_string(key_obj);
         }

         if (!provided_key || provided_key[0] == '\0') {
            OLOG_WARNING("Satellite: Registration rejected — no key provided (uuid=%s)", uuid);
            send_error_impl(conn->wsi, "registration_key_required",
                            "Server requires a registration key. "
                            "Set registration_key in satellite config.");
            return;
         }

         size_t key_len = strlen(secrets->satellite_registration_key);
         if (strlen(provided_key) != key_len ||
             sodium_memcmp(provided_key, secrets->satellite_registration_key, key_len) != 0) {
            OLOG_WARNING("Satellite: Registration rejected — invalid key (uuid=%s)", uuid);
            send_error_impl(conn->wsi, "invalid_registration_key",
                            "Registration key does not match. "
                            "Check satellite_registration_key in secrets.toml.");
            return;
         }
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
      struct json_object *ota_obj;
      if (json_object_object_get_ex(caps_obj, "ota", &ota_obj)) {
         caps.ota = json_object_get_boolean(ota_obj);
      }
   }

   /* Firmware version (optional — legacy firmware omits it).  Recorded in
    * ota_device_state for fleet visibility and the OTA register-time finalize
    * check (Phase 2).  See docs/OTA_DESIGN.md §1. */
   /* Satellite-supplied firmware version; width-clamped at the DB chokepoint
    * (ota_db_report_version) before it's persisted. */
   const char *firmware_version = "";
   struct json_object *fw_obj;
   if (json_object_object_get_ex(payload, "firmware_version", &fw_obj)) {
      const char *fw = json_object_get_string(fw_obj);
      if (fw)
         firmware_version = fw;
   }

   /* Validate tier matches declared capabilities to prevent resource abuse.
    * Tier 2 relies on server-side ASR+TTS, so must NOT claim local capabilities. */
   if (tier == 2 && (caps.local_asr || caps.local_tts)) {
      OLOG_WARNING("Satellite: Tier 2 registration rejected — claims local_asr=%d local_tts=%d",
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

   /* Sanitize name/location to [a-zA-Z0-9 _.\-].  These fields surface in
    * the WebUI scheduler panel and other client-rendered surfaces; anything
    * outside this allowlist (notably HTML metachars `<>&"'/`) would be an XSS
    * vector if a satellite were compromised.  Allowlist also matches the
    * operational intent — these are short device labels like "kitchen". */
   for (char *p = identity.name; *p; p++) {
      unsigned char c = (unsigned char)*p;
      bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == ' ' || c == '_' || c == '.' || c == '-';
      if (!ok)
         *p = '_';
   }
   for (char *p = identity.location; *p; p++) {
      unsigned char c = (unsigned char)*p;
      bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == ' ' || c == '_' || c == '.' || c == '-';
      if (!ok)
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

   /* Look up persistent satellite mapping and apply user assignment */
   if (auth_db_is_ready()) {
      satellite_mapping_t mapping;
      int db_rc = satellite_db_get(uuid, &mapping);

      if (db_rc == AUTH_DB_SUCCESS) {
         /* Existing mapping found */
         if (!mapping.enabled) {
            OLOG_WARNING("Satellite: Disabled satellite rejected (uuid=%s)", uuid);
            send_error_impl(conn->wsi, "SATELLITE_DISABLED",
                            "This satellite has been disabled by an administrator.");
            session_destroy(session->session_id);
            conn->session = NULL;
            return;
         }

         /* Apply user mapping if assigned */
         if (mapping.user_id > 0) {
#ifdef ENABLE_MULTI_CLIENT
            session_set_metrics_user(session, mapping.user_id);
#endif
            conn->auth_user_id = mapping.user_id;

            /* Phase 1f: SESSION_START builder boundary on satellite
             * rebind — clear dedup state so the next PER_TURN starts
             * fresh against the new user's persona/memory. */
            session_injected_set_clear(session);
            /* Build personalized system prompt with user memories */
            char *user_prompt = session_manager_build_system_prompt_string(mapping.user_id);
            if (user_prompt) {
               session_update_system_prompt(session, user_prompt);
               free(user_prompt);
            }

            OLOG_INFO("Satellite: Applied user mapping user_id=%d for %s (%s)", mapping.user_id,
                      identity.name, uuid);
         }

         /* Append satellite context (room + HA area) */
         session_append_satellite_context(session, identity.location, mapping.ha_area);

         /* Sync name/location if changed on satellite side (already sanitized above) */
         if (strcmp(mapping.name, identity.name) != 0 ||
             strcmp(mapping.location, identity.location) != 0) {
            satellite_mapping_t updated = mapping;
            strncpy(updated.name, identity.name, sizeof(updated.name) - 1);
            strncpy(updated.location, identity.location, sizeof(updated.location) - 1);
            updated.last_seen = time(NULL);
            satellite_db_upsert(&updated);
         } else {
            satellite_db_update_last_seen(uuid);
         }
      } else if (db_rc == AUTH_DB_NOT_FOUND) {
         /* First-time registration — create mapping with user_id=0 */
         satellite_mapping_t new_mapping;
         memset(&new_mapping, 0, sizeof(new_mapping));
         strncpy(new_mapping.uuid, uuid, sizeof(new_mapping.uuid) - 1);
         strncpy(new_mapping.name, identity.name, sizeof(new_mapping.name) - 1);
         strncpy(new_mapping.location, identity.location, sizeof(new_mapping.location) - 1);
         new_mapping.tier = tier;
         new_mapping.enabled = true;
         new_mapping.created_at = time(NULL);
         new_mapping.last_seen = new_mapping.created_at;

         satellite_db_upsert(&new_mapping);

         /* No user mapping yet — use default room context */
         session_append_satellite_context(session, identity.location, NULL);

         OLOG_INFO("Satellite: Auto-registered new satellite %s (%s)", identity.name, uuid);
      }

      /* Record the reported firmware version (lazily creates the OTA state row).
       * Separate write from the mapping upsert above — ota_device_state has no FK
       * to satellite_mappings and the version is advisory, so a torn write across
       * a crash is self-healing on the next registration. */
      ota_db_report_version(uuid, firmware_version);

      /* Server-owned OTA commit: if this device just came back reporting the
       * version we pushed, mark the update successful (a device must prove the
       * new image runs by reconnecting on it — it never self-declares success). */
      ota_finalize_on_register(uuid, firmware_version);
   }

   /* Get reconnect secret for client to save */
   char *session_secret = session_get_reconnect_secret(session);

   OLOG_INFO("Satellite: Registered '%s' (%s) tier=%d location='%s' fw=%s session=%u user=%d",
             identity.name, identity.uuid, tier, identity.location,
             firmware_version[0] ? firmware_version : "?", session->session_id, conn->auth_user_id);

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
   send_json_response(conn, response);
   json_object_put(response);
}

void handle_satellite_query(ws_connection_t *conn, struct json_object *payload) {
   if (!conn || !payload) {
      OLOG_WARNING("Satellite: Invalid query request");
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
      OLOG_WARNING("Satellite: Worker limit reached (%d), rejecting query from %s",
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
      OLOG_ERROR("Satellite: Failed to create worker thread: %d", ret);
      session_release(session);
      free(work->text);
      free(work);
      send_error_impl(conn->wsi, "INTERNAL_ERROR", "Failed to start processing");
      return;
   }

   OLOG_INFO("Satellite: Query queued for %s: %.50s%s", session->identity.name, text,
             strlen(text) > 50 ? "..." : "");
}

void handle_satellite_ping(ws_connection_t *conn) {
   if (!conn) {
      return;
   }

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("satellite_pong"));
   send_json_response(conn, response);
   json_object_put(response);

   /* Touch session if exists */
   if (conn->session) {
      session_touch(conn->session);
   }
}

/* =============================================================================
 * Volume Tool Integration (called from volume_tool.c via session routing)
 * ============================================================================= */

void handle_satellite_volume_state(ws_connection_t *conn, struct json_object *payload) {
   if (!conn || !conn->is_satellite) {
      OLOG_WARNING("Satellite: volume_state rejected (not a satellite connection)");
      return;
   }

   struct json_object *level_obj;
   if (!json_object_object_get_ex(payload, "level", &level_obj)) {
      OLOG_WARNING("Satellite: volume_state missing 'level'");
      return;
   }

   int level = json_object_get_int(level_obj);
   if (level < 0)
      level = 0;
   if (level > 100)
      level = 100;

   conn->volume = (float)level / 100.0f;
   OLOG_INFO("Satellite: Volume state updated to %d%% for %s", level,
             conn->session ? conn->session->identity.name : "(unknown)");
}

char *satellite_volume_execute_tool(ws_connection_t *conn,
                                    const char *action,
                                    const char *value,
                                    int *should_respond) {
   *should_respond = 1;

   if (strcmp(action, "get") == 0) {
      char *result = malloc(64);
      if (result)
         snprintf(result, 64, "Volume is at %.0f%%", conn->volume * 100.0f);
      return result;
   }

   /* Action: set */
   if (!value || !*value) {
      return strdup("Error: 'level' parameter is required for 'set' action.");
   }

   float vol = parse_volume_level(value);
   if (vol < 0.0f) {
      char *result = malloc(128);
      if (result)
         snprintf(result, 128, "Invalid volume level '%s'. Use a number 0-100.", value);
      return result;
   }

   conn->volume = vol;
   int level_int = (int)(vol * 100.0f + 0.5f);

   /* Send volume_set command to satellite via response queue */
   struct json_object *msg = json_object_new_object();
   json_object_object_add(msg, "type", json_object_new_string("volume_set"));
   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "level", json_object_new_int(level_int));
   json_object_object_add(msg, "payload", payload);

   const char *json_str = json_object_to_json_string(msg);
   char *json_copy = strdup(json_str);
   json_object_put(msg);

   if (json_copy && conn->session) {
      ws_response_t resp = { .session = conn->session,
                             .type = WS_RESP_MUSIC_STATE,
                             .music_json = { .json = json_copy } };
      queue_response(&resp);
   } else {
      free(json_copy);
   }

   OLOG_INFO("Satellite: Volume set to %d%% for %s", level_int,
             conn->session ? conn->session->identity.name : "(unknown)");

   char *result = malloc(64);
   if (result)
      snprintf(result, 64, "Volume set to %d%%", level_int);
   return result;
}
