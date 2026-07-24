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
 * WebUI send-side pipeline.
 *
 * Owns the entire server-to-client data path:
 *   - Response queue lifecycle (queue_response / free_response /
 *     process_response_queue / process_one_response)
 *   - WebSocket send helpers (send_state_impl, send_transcript_impl_*,
 *     send_audio_impl, send_*_impl) — every "emit X to client" helper.
 *     LWS service thread only.
 *   - LLM streaming impls (stream_start / delta / end)
 *   - Extended-thinking impls (thinking_start / delta / end /
 *     reasoning_summary)
 *
 * Split out of webui_server.c so that file can stay under the size
 * limits in CLAUDE.md.  Cross-file callers reach these helpers through
 * webui_internal.h.
 */

#include <json-c/json.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/scheduler.h"
#include "core/session_manager.h"
#include "dawn.h"
#include "logging.h"
#include "webui/webui_always_on.h"
#include "webui/webui_internal.h"
#include "webui/webui_server.h"

/* =============================================================================
 * Response Queue Functions
 * ============================================================================= */

void queue_response(ws_response_t *resp) {
   pthread_mutex_lock(&s_queue_mutex);

   int next_tail = (s_queue_tail + 1) % WEBUI_RESPONSE_QUEUE_SIZE;
   if (next_tail == s_queue_head) {
      /* Queue full - free oldest entry's heap allocations, then drop it */
      OLOG_WARNING("WebUI: Response queue full, dropping oldest entry");
      free_response(&s_response_queue[s_queue_head]);
      s_queue_head = (s_queue_head + 1) % WEBUI_RESPONSE_QUEUE_SIZE;
   }

   s_response_queue[s_queue_tail] = *resp;
   s_queue_tail = next_tail;

   /* Warn when queue utilization exceeds 75% — indicates potential drain bottleneck */
   int count = (s_queue_tail >= s_queue_head)
                   ? (s_queue_tail - s_queue_head)
                   : (WEBUI_RESPONSE_QUEUE_SIZE - s_queue_head + s_queue_tail);
   if (count > (WEBUI_RESPONSE_QUEUE_SIZE * 3 / 4) && (count % 128 == 0)) {
      OLOG_WARNING("WebUI: Response queue at %d%% (%d/%d entries)",
                   (count * 100) / WEBUI_RESPONSE_QUEUE_SIZE, count, WEBUI_RESPONSE_QUEUE_SIZE);
   }

   pthread_mutex_unlock(&s_queue_mutex);

   /* Wake up lws_service() to process queue */
   if (s_lws_context) {
      lws_cancel_service(s_lws_context);
   }
}

void free_response(ws_response_t *resp) {
   switch (resp->type) {
      case WS_RESP_STATE:
         free(resp->state.state);
         free(resp->state.detail);
         free(resp->state.tools_json);
         break;
      case WS_RESP_TRANSCRIPT:
         free(resp->transcript.role);
         free(resp->transcript.text);
         break;
      case WS_RESP_ERROR:
         free(resp->error.code);
         free(resp->error.message);
         break;
      case WS_RESP_SESSION:
         free(resp->session_token.token);
         break;
      case WS_RESP_AUDIO:
         free(resp->audio.data);
         break;
      case WS_RESP_AUDIO_END:
         /* No data to free */
         break;
      case WS_RESP_CONTEXT:
         /* No data to free - all inline values */
         break;
      case WS_RESP_STREAM_START:
      case WS_RESP_STREAM_DELTA:
      case WS_RESP_STREAM_END:
         /* No data to free - text[] is inline fixed buffer */
         break;
      case WS_RESP_METRICS_UPDATE:
         /* No data to free - all inline values */
         break;
      case WS_RESP_COMPACTION_COMPLETE:
         free(resp->compaction.summary);
         break;
      case WS_RESP_THINKING_START:
      case WS_RESP_THINKING_DELTA:
      case WS_RESP_THINKING_END:
         /* No data to free - text[] is inline fixed buffer */
         break;
      case WS_RESP_CONVERSATION_RESET:
         /* No data to free */
         break;
      case WS_RESP_MUSIC_DATA:
         free(resp->audio.data);
         break;
      case WS_RESP_MUSIC_POSITION:
         /* No data to free - all inline values */
         break;
      case WS_RESP_MUSIC_STATE:
      case WS_RESP_MUSIC_ERROR:
         free(resp->music_json.json);
         break;
      case WS_RESP_SCHEDULER_NOTIFICATION:
         free(resp->scheduler_json.json);
         break;
      case WS_RESP_JSON:
         free(resp->generic_json.json);
         break;
   }
}

/* =============================================================================
 * WebSocket Send Helpers — LWS SERVICE THREAD ONLY
 *
 * These functions call lws_write() directly. libwebsockets is NOT thread-safe
 * for writes — lws_write() must only be called from within an LWS callback
 * (the service thread) or from process_one_response() which runs on that thread.
 *
 * To send from other threads (LLM workers, music streaming, tool execution),
 * use queue_response() to enqueue a ws_response_t. The LWS service thread
 * will dequeue and send it via process_one_response().
 * ============================================================================= */

/* LWS requires LWS_PRE bytes before the buffer for protocol framing.
 * WS_SEND_BUFFER_SIZE is defined in webui_internal.h */

int send_json_message(struct lws *wsi, const char *json) {
   size_t len = strlen(json);

   /* Log messages that may be too large for HTTP/2 frames (default 16KB) */
   if (len > 12000) {
      /* Try to extract message type for better logging.
       * Handle both "type":"value" and "type": "value" (with optional space). */
      const char *type_start = strstr(json, "\"type\":");
      char type_buf[32] = "unknown";
      if (type_start) {
         type_start += 7; /* skip past "type": */
         while (*type_start == ' ')
            type_start++; /* skip optional whitespace */
         if (*type_start == '"')
            type_start++; /* skip opening quote */
         const char *type_end = strchr(type_start, '"');
         if (type_end && (size_t)(type_end - type_start) < sizeof(type_buf) - 1) {
            strncpy(type_buf, type_start, (size_t)(type_end - type_start));
            type_buf[type_end - type_start] = '\0';
         }
      }
      OLOG_INFO("WebUI: Large message via send_json_message: type=%s, size=%zu bytes", type_buf,
                len);
   }

   /* For messages that fit in the stack buffer, use the fast path */
   if (len < WS_SEND_BUFFER_SIZE - LWS_PRE) {
      unsigned char buf[LWS_PRE + WS_SEND_BUFFER_SIZE];
      memcpy(&buf[LWS_PRE], json, len);

      int written = lws_write(wsi, &buf[LWS_PRE], len, LWS_WRITE_TEXT);
      if (written < (int)len) {
         OLOG_ERROR("WebUI: lws_write failed (wrote %d of %zu)", written, len);
         return LWS_CLOSE_CONNECTION;
      }
      return 0;
   }

   /* Large messages (e.g. transcript with attached documents): heap-allocate */
   unsigned char *buf = malloc(LWS_PRE + len);
   if (!buf) {
      OLOG_ERROR("WebUI: Failed to allocate send buffer (%zu bytes)", LWS_PRE + len);
      return LWS_CLOSE_CONNECTION;
   }

   memcpy(&buf[LWS_PRE], json, len);
   int written = lws_write(wsi, &buf[LWS_PRE], len, LWS_WRITE_TEXT);
   free(buf);

   if (written < (int)len) {
      OLOG_ERROR("WebUI: lws_write failed (wrote %d of %zu)", written, len);
      return LWS_CLOSE_CONNECTION;
   }

   return 0;
}

int send_binary_message(struct lws *wsi, uint8_t msg_type, const uint8_t *data, size_t len) {
   if (!wsi) {
      OLOG_ERROR("WebUI: send_binary_message called with NULL wsi");
      return LWS_CLOSE_CONNECTION;
   }

   /* Allocate buffer with LWS_PRE padding + 1 byte for type + data */
   size_t total_len = 1 + len;
   unsigned char *buf = malloc(LWS_PRE + total_len);
   if (!buf) {
      OLOG_ERROR("WebUI: Failed to allocate binary send buffer (%zu bytes)", LWS_PRE + total_len);
      return LWS_CLOSE_CONNECTION;
   }

   buf[LWS_PRE] = msg_type;
   if (data && len > 0) {
      memcpy(&buf[LWS_PRE + 1], data, len);
   }

   int written = lws_write(wsi, &buf[LWS_PRE], total_len, LWS_WRITE_BINARY);
   free(buf);

   if (written < 0) {
      OLOG_ERROR("WebUI: lws_write binary failed with error %d", written);
      return LWS_CLOSE_CONNECTION;
   }

   if (written < (int)total_len) {
      OLOG_ERROR("WebUI: lws_write binary partial write (%d of %zu)", written, total_len);
      return LWS_CLOSE_CONNECTION;
   }

   return 0;
}

void send_audio_impl(struct lws *wsi, const uint8_t *data, size_t len) {
   int ret = send_binary_message(wsi, WS_BIN_AUDIO_OUT, data, len);
   if (ret != 0) {
      OLOG_ERROR("WebUI: Failed to send audio chunk (%zu bytes)", len);
   }
}

void send_audio_end_impl(struct lws *wsi, bool is_opus) {
   uint8_t codec_flag = is_opus ? 1 : 0;
   send_binary_message(wsi, WS_BIN_AUDIO_SEGMENT_END, &codec_flag, 1);
}

/**
 * @brief Check if client capabilities include Opus audio codec support
 *
 * Parses the payload for capabilities.audio_codecs array and checks
 * if "opus" is in the list. Falls back to PCM if not specified.
 *
 * @param payload JSON object from init/reconnect message
 * @return true if client supports Opus, false otherwise
 */
bool check_opus_capability(struct json_object *payload) {
   if (!payload) {
      return false;
   }

   struct json_object *capabilities;
   if (!json_object_object_get_ex(payload, "capabilities", &capabilities)) {
      return false;
   }

   struct json_object *audio_codecs;
   if (!json_object_object_get_ex(capabilities, "audio_codecs", &audio_codecs)) {
      return false;
   }

   if (!json_object_is_type(audio_codecs, json_type_array)) {
      return false;
   }

   int len = json_object_array_length(audio_codecs);
   /* Defensive bound: no client should send more than 16 codecs */
   if (len > 16) {
      OLOG_WARNING("WebUI: Too many audio codecs in capability list (%d), ignoring", len);
      return false;
   }

   for (int i = 0; i < len; i++) {
      struct json_object *codec = json_object_array_get_idx(audio_codecs, i);
      if (codec && json_object_is_type(codec, json_type_string)) {
         const char *codec_str = json_object_get_string(codec);
         if (codec_str && strcmp(codec_str, "opus") == 0) {
            return true;
         }
      }
   }

   return false;
}

void send_state_impl_full(struct lws *wsi,
                          const char *state,
                          const char *detail,
                          const char *tools_json,
                          int64_t conversation_id) {
   /* Build state message with proper JSON escaping using json-c */
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "state", json_object_new_string(state));
   if (conversation_id > 0) {
      json_object_object_add(payload, "conversation_id", json_object_new_int64(conversation_id));
   }

   if (detail && detail[0] != '\0') {
      json_object_object_add(payload, "detail", json_object_new_string(detail));
   }

   if (tools_json && tools_json[0] != '\0') {
      /* tools_json is already a JSON array string like "[{...},{...}]" */
      struct json_object *tools = json_tokener_parse(tools_json);
      if (tools) {
         json_object_object_add(payload, "tools", tools);
      }
   }

   json_object_object_add(obj, "type", json_object_new_string("state"));
   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

void send_state_impl(struct lws *wsi, const char *state, const char *detail) {
   send_state_impl_full(wsi, state, detail, NULL, 0);
}

void send_conversation_reset_impl(struct lws *wsi) {
   struct json_object *obj = json_object_new_object();

   json_object_object_add(obj, "type", json_object_new_string("conversation_reset"));

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

void send_music_position_impl(struct lws *wsi, double position_sec, uint32_t duration_sec) {
   struct json_object *obj = json_object_new_object();

   json_object_object_add(obj, "type", json_object_new_string("music_position"));

   struct json_object *payload = json_object_new_object();
   json_object_object_add(payload, "position_sec", json_object_new_double(position_sec));
   json_object_object_add(payload, "duration_sec", json_object_new_int(duration_sec));

   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

void send_transcript_impl_ex(struct lws *wsi,
                             const char *role,
                             const char *text,
                             bool replay,
                             bool server_saved,
                             int64_t conversation_id) {
   /* Escape JSON special characters in text */
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "role", json_object_new_string(role));
   json_object_object_add(payload, "text", json_object_new_string(text));
   if (conversation_id > 0) {
      json_object_object_add(payload, "conversation_id", json_object_new_int64(conversation_id));
   }
   if (replay) {
      json_object_object_add(payload, "replay", json_object_new_boolean(true));
   }
   if (server_saved) {
      json_object_object_add(payload, "server_saved", json_object_new_boolean(true));
   }
   json_object_object_add(obj, "type", json_object_new_string("transcript"));
   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

static void send_transcript_impl(struct lws *wsi, const char *role, const char *text) {
   send_transcript_impl_ex(wsi, role, text, false, false, 0);
}

void send_error_impl(struct lws *wsi, const char *code, const char *message) {
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "code", json_object_new_string(code));
   json_object_object_add(payload, "message", json_object_new_string(message));
   json_object_object_add(payload, "recoverable", json_object_new_boolean(1));
   json_object_object_add(obj, "type", json_object_new_string("error"));
   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

void send_force_logout_impl(struct lws *wsi, const char *reason) {
   char json[256];
   snprintf(json, sizeof(json), "{\"type\":\"force_logout\",\"payload\":{\"reason\":\"%s\"}}",
            reason ? reason : "Session revoked");
   send_json_message(wsi, json);
}

void send_session_token_impl(ws_connection_t *conn, const char *token) {
   char json[512];

   /* Include auth state in session response to avoid separate config fetch */
   if (conn->authenticated) {
      /* Fetch is_admin from DB (not cached to prevent stale state) */
      auth_session_t auth_session;
      bool is_admin = false;
      if (auth_db_get_session(conn->auth_session_token, &auth_session) == AUTH_DB_SUCCESS) {
         is_admin = auth_session.is_admin;
      }
      snprintf(json, sizeof(json),
               "{\"type\":\"session\",\"payload\":{\"token\":\"%s\","
               "\"authenticated\":true,\"username\":\"%s\",\"is_admin\":%s}}",
               token, conn->username, is_admin ? "true" : "false");
   } else {
      snprintf(json, sizeof(json),
               "{\"type\":\"session\",\"payload\":{\"token\":\"%s\","
               "\"authenticated\":false}}",
               token);
   }
   send_json_message(conn->wsi, json);
}

static void send_config_impl(struct lws *wsi) {
   char json[256];
   snprintf(json, sizeof(json), "{\"type\":\"config\",\"payload\":{\"audio_chunk_ms\":%d}}",
            g_config.webui.audio_chunk_ms);
   send_json_message(wsi, json);
}

/* Compile-time constant: feature flags sent to all clients on connect */
static const char s_server_features_json[] = "{\"type\":\"server_features\",\"payload\":{"
                                             "\"home_assistant\":"
#ifdef DAWN_ENABLE_HOMEASSISTANT_TOOL
                                             "true"
#else
                                             "false"
#endif
                                             "}}";

static void send_server_features(struct lws *wsi) {
   send_json_message(wsi, s_server_features_json);
}

/**
 * Queue the 4 init messages through the response queue instead of calling
 * lws_write() directly. This ensures one write per writeable callback,
 * preventing WebSocket frame corruption ("Invalid frame header").
 *
 * Messages queued: session token, config, server_features, state(idle).
 */
void queue_init_messages(ws_connection_t *conn, const char *token) {
   session_t *session = conn->session;

   /* 1. Session token — build the same JSON as send_session_token_impl() */
   {
      ws_response_t resp = { 0 };
      resp.session = session;
      resp.type = WS_RESP_SESSION;
      resp.session_token.token = strdup(token);
      queue_response(&resp);
   }

   /* 2. Config — pre-serialize as JSON string */
   {
      char json[256];
      snprintf(json, sizeof(json), "{\"type\":\"config\",\"payload\":{\"audio_chunk_ms\":%d}}",
               g_config.webui.audio_chunk_ms);
      ws_response_t resp = { 0 };
      resp.session = session;
      resp.type = WS_RESP_JSON;
      resp.generic_json.json = strdup(json);
      queue_response(&resp);
   }

   /* 3. Server features — compile-time constant string */
   {
      ws_response_t resp = { 0 };
      resp.session = session;
      resp.type = WS_RESP_JSON;
      resp.generic_json.json = strdup(s_server_features_json);
      queue_response(&resp);
   }

   /* 4. State: idle */
   {
      ws_response_t resp = { 0 };
      resp.session = session;
      resp.type = WS_RESP_STATE;
      resp.state.state = strdup("idle");
      resp.state.detail = NULL;
      resp.state.tools_json = NULL;
      queue_response(&resp);
   }
}

void send_context_impl(struct lws *wsi, int current_tokens, int max_tokens, float threshold) {
   char json[256];
   float usage_pct = (max_tokens > 0) ? (float)current_tokens / (float)max_tokens * 100.0f : 0;
   snprintf(json, sizeof(json),
            "{\"type\":\"context\",\"payload\":{\"current\":%d,\"max\":%d,\"usage\":%.1f,"
            "\"threshold\":%.0f}}",
            current_tokens, max_tokens, usage_pct, threshold * 100.0f);
   send_json_message(wsi, json);
}

void send_metrics_impl(struct lws *wsi,
                       const char *state,
                       int ttft_ms,
                       float token_rate,
                       int context_pct,
                       int64_t conversation_id) {
   char json[256];
   /* conversation_id lets the client gate the footer to the active view — a
    * background turn's tok/s/TTFT must not update the footer of an idle view. */
   snprintf(json, sizeof(json),
            "{\"type\":\"metrics_update\",\"payload\":{\"state\":\"%s\",\"ttft_ms\":%d,"
            "\"token_rate\":%.1f,\"context_percent\":%d,\"conversation_id\":%lld}}",
            state, ttft_ms, token_rate, context_pct, (long long)conversation_id);
   send_json_message(wsi, json);
}

void send_compaction_impl(struct lws *wsi,
                          int tokens_before,
                          int tokens_after,
                          int messages_summarized,
                          const char *summary,
                          int level) {
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "tokens_before", json_object_new_int(tokens_before));
   json_object_object_add(payload, "tokens_after", json_object_new_int(tokens_after));
   json_object_object_add(payload, "messages_summarized", json_object_new_int(messages_summarized));
   json_object_object_add(payload, "level", json_object_new_int(level));
   if (summary) {
      json_object_object_add(payload, "summary", json_object_new_string(summary));
   }
   json_object_object_add(obj, "type", json_object_new_string("context_compacted"));
   json_object_object_add(obj, "payload", payload);

   const char *json = json_object_to_json_string(obj);
   send_json_message(wsi, json);
   json_object_put(obj);
}

/* =============================================================================
 * LLM Streaming Impl Functions (ChatGPT-style real-time text)
 *
 * Protocol:
 *   stream_start - Create new assistant entry, enter streaming state
 *   stream_delta - Append text to current entry
 *   stream_end   - Finalize entry, exit streaming state
 * ============================================================================= */

void send_stream_start_impl(struct lws *wsi, uint32_t stream_id, int64_t conversation_id) {
   char json[160];
   snprintf(json, sizeof(json),
            "{\"type\":\"stream_start\",\"payload\":{\"stream_id\":%u,\"conversation_id\":%lld}}",
            stream_id, (long long)conversation_id);
   send_json_message(wsi, json);
}

void send_stream_delta_impl(struct lws *wsi,
                            uint32_t stream_id,
                            int64_t conversation_id,
                            const char *text) {
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "stream_id", json_object_new_int((int32_t)stream_id));
   json_object_object_add(payload, "conversation_id", json_object_new_int64(conversation_id));
   json_object_object_add(payload, "delta", json_object_new_string(text));
   json_object_object_add(obj, "type", json_object_new_string("stream_delta"));
   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

void send_stream_end_impl(struct lws *wsi,
                          uint32_t stream_id,
                          int64_t conversation_id,
                          const char *reason) {
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "stream_id", json_object_new_int((int32_t)stream_id));
   json_object_object_add(payload, "conversation_id", json_object_new_int64(conversation_id));
   json_object_object_add(payload, "reason", json_object_new_string(reason ? reason : "complete"));
   json_object_object_add(obj, "type", json_object_new_string("stream_end"));
   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

/* =============================================================================
 * Extended Thinking WebSocket Helpers
 * ============================================================================= */

void send_thinking_start_impl(struct lws *wsi,
                              uint32_t stream_id,
                              int64_t conversation_id,
                              const char *provider) {
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "stream_id", json_object_new_int((int32_t)stream_id));
   json_object_object_add(payload, "conversation_id", json_object_new_int64(conversation_id));
   json_object_object_add(payload, "provider",
                          json_object_new_string(provider ? provider : "unknown"));
   json_object_object_add(obj, "type", json_object_new_string("thinking_start"));
   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

void send_thinking_delta_impl(struct lws *wsi,
                              uint32_t stream_id,
                              int64_t conversation_id,
                              const char *text) {
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "stream_id", json_object_new_int((int32_t)stream_id));
   json_object_object_add(payload, "conversation_id", json_object_new_int64(conversation_id));
   json_object_object_add(payload, "delta", json_object_new_string(text));
   json_object_object_add(obj, "type", json_object_new_string("thinking_delta"));
   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

void send_thinking_end_impl(struct lws *wsi,
                            uint32_t stream_id,
                            int64_t conversation_id,
                            int has_content) {
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "stream_id", json_object_new_int((int32_t)stream_id));
   json_object_object_add(payload, "conversation_id", json_object_new_int64(conversation_id));
   json_object_object_add(payload, "has_content", json_object_new_boolean(has_content));
   json_object_object_add(obj, "type", json_object_new_string("thinking_end"));
   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

void send_reasoning_summary_impl(struct lws *wsi,
                                 uint32_t stream_id,
                                 int64_t conversation_id,
                                 int reasoning_tokens) {
   struct json_object *obj = json_object_new_object();
   struct json_object *payload = json_object_new_object();

   json_object_object_add(payload, "stream_id", json_object_new_int((int32_t)stream_id));
   if (conversation_id > 0) {
      json_object_object_add(payload, "conversation_id", json_object_new_int64(conversation_id));
   }
   json_object_object_add(payload, "reasoning_tokens", json_object_new_int(reasoning_tokens));
   json_object_object_add(obj, "type", json_object_new_string("reasoning_summary"));
   json_object_object_add(obj, "payload", payload);

   const char *json_str = json_object_to_json_string(obj);
   send_json_message(wsi, json_str);
   json_object_put(obj);
}

/* =============================================================================
 * Response Queue Processing (called from WebUI thread)
 *
 * IMPORTANT: libwebsockets only allows ONE lws_write() per writeable callback.
 * This function processes only ONE response per call. If more responses are
 * pending, it requests a writeable callback via lws_callback_on_writable().
 * ============================================================================= */

/* Invalidate any queued responses that reference a session about to be freed, so
 * the send thread never dereferences a dangling session pointer.  Called by
 * session_destroy (weak seam) BEFORE session_free; the nulled entries are reaped
 * by process_one_response's dead-entry path.  Synchronizes the send-queue's
 * session reads with the free via s_queue_mutex. */
void webui_send_purge_session(const session_t *s) {
   if (!s) {
      return;
   }
   pthread_mutex_lock(&s_queue_mutex);
   int i = s_queue_head;
   while (i != s_queue_tail) {
      if (s_response_queue[i].session == s) {
         s_response_queue[i].session = NULL;
      }
      i = (i + 1) % WEBUI_RESPONSE_QUEUE_SIZE;
   }
   pthread_mutex_unlock(&s_queue_mutex);
}

void process_one_response(void) {
   pthread_mutex_lock(&s_queue_mutex);

   if (s_queue_head == s_queue_tail) {
      pthread_mutex_unlock(&s_queue_mutex);
      return;
   }

   /* Scan forward from head to find the first entry whose socket isn't choked.
    * This prevents a slow client (e.g. ESP32 Tier 2 over WiFi/SSL) from
    * blocking the entire response queue for all other sessions (WebUI, Tier 1).
    * Per-session FIFO ordering is preserved: if Session A is choked, ALL entries
    * for Session A are skipped (since choke is per-socket, not per-message). */
   int queue_len = (s_queue_tail >= s_queue_head)
                       ? (s_queue_tail - s_queue_head)
                       : (WEBUI_RESPONSE_QUEUE_SIZE - s_queue_head + s_queue_tail);

   /* Cap scan to first 64 entries to bound mutex hold time. If all 64 are
    * choked, we'll retry on the next writable callback. */
   int scan_limit = queue_len < 64 ? queue_len : 64;
   int found_offset = -1;
   for (int i = 0; i < scan_limit; i++) {
      int idx = (s_queue_head + i) % WEBUI_RESPONSE_QUEUE_SIZE;
      ws_response_t *candidate = &s_response_queue[idx];

      /* Disconnected sessions can be dequeued immediately (freed below) */
      if (!candidate->session || candidate->session->disconnected) {
         found_offset = i;
         break;
      }

      /* Check flow control — skip choked sockets, schedule writeable retry */
      ws_connection_t *cand_conn = (ws_connection_t *)candidate->session->client_data;
      if (cand_conn && cand_conn->wsi && lws_send_pipe_choked(cand_conn->wsi)) {
         lws_callback_on_writable(cand_conn->wsi);
         continue;
      }

      found_offset = i;
      break;
   }

   if (found_offset < 0) {
      /* All queued entries are for choked sockets — nothing to send now */
      pthread_mutex_unlock(&s_queue_mutex);
      return;
   }

   /* Dequeue the found entry. Shift earlier (choked) entries forward to fill
    * the gap, then advance head. Preserves FIFO ordering for all sessions. */
   int found_idx = (s_queue_head + found_offset) % WEBUI_RESPONSE_QUEUE_SIZE;
   ws_response_t resp = s_response_queue[found_idx];
   /* Capture the session's fields UNDER the queue lock — after we unlock,
    * session_destroy may purge + free resp.session (it takes this same lock), so
    * resp.session must never be dereferenced past the unlock. */
   bool resp_disconnected = (resp.session == NULL) || atomic_load(&resp.session->disconnected);
   ws_connection_t *resp_conn = resp.session ? (ws_connection_t *)resp.session->client_data : NULL;
   for (int i = found_offset; i > 0; i--) {
      int dst = (s_queue_head + i) % WEBUI_RESPONSE_QUEUE_SIZE;
      int src = (s_queue_head + i - 1) % WEBUI_RESPONSE_QUEUE_SIZE;
      s_response_queue[dst] = s_response_queue[src];
   }
   /* Zero the vacated head slot to prevent double-free: the shift copied
    * pointer members forward, so the old head still holds stale pointers. */
   memset(&s_response_queue[s_queue_head], 0, sizeof(ws_response_t));
   s_queue_head = (s_queue_head + 1) % WEBUI_RESPONSE_QUEUE_SIZE;
   bool more_pending = (s_queue_head != s_queue_tail);
   pthread_mutex_unlock(&s_queue_mutex);

   /* Find connection for this session (using values captured under the lock —
    * resp.session may already be freed by now). */
   if (resp_disconnected) {
      free_response(&resp);
      /* If more responses, schedule another callback */
      if (more_pending && s_lws_context) {
         lws_cancel_service(s_lws_context);
      }
      return;
   }

   ws_connection_t *conn = resp_conn;
   if (!conn || !conn->wsi) {
      free_response(&resp);
      if (more_pending && s_lws_context) {
         lws_cancel_service(s_lws_context);
      }
      return;
   }

   /* Send via lws_write (one write per callback!) */
   switch (resp.type) {
      case WS_RESP_STATE:
         send_state_impl_full(conn->wsi, resp.state.state, resp.state.detail, resp.state.tools_json,
                              resp.state.conversation_id);
         /* If LLM pipeline sent "idle" and always-on is in PROCESSING, resume listening.
          * processing_complete resets internal state (buffer, VAD, cooldown) but set_state
          * does NOT send a WebSocket message. We must notify the client via the response
          * queue (not lws_write directly, since we already wrote once in this callback). */
         if (conn->always_on && always_on_get_state(conn->always_on) == ALWAYS_ON_PROCESSING &&
             strcmp(resp.state.state, "idle") == 0) {
            always_on_processing_complete(conn->always_on);
            ws_response_t ao_resp = { 0 };
            ao_resp.session = conn->session;
            ao_resp.type = WS_RESP_JSON;
            ao_resp.generic_json.json = strdup(
                "{\"type\":\"always_on_state\",\"payload\":{\"state\":\"listening\"}}");
            queue_response(&ao_resp);
         }
         free(resp.state.state);
         free(resp.state.detail);
         free(resp.state.tools_json);
         break;
      case WS_RESP_TRANSCRIPT:
         send_transcript_impl_ex(conn->wsi, resp.transcript.role, resp.transcript.text, false,
                                 resp.transcript.server_saved, resp.transcript.conversation_id);
         free(resp.transcript.role);
         free(resp.transcript.text);
         break;
      case WS_RESP_ERROR:
         send_error_impl(conn->wsi, resp.error.code, resp.error.message);
         free(resp.error.code);
         free(resp.error.message);
         break;
      case WS_RESP_SESSION:
         send_session_token_impl(conn, resp.session_token.token);
         free(resp.session_token.token);
         break;
      case WS_RESP_AUDIO:
         send_audio_impl(conn->wsi, resp.audio.data, resp.audio.len);
         free(resp.audio.data);
         /* Progress signal for the always-on PROCESSING watchdog: a long,
          * TTS-paced reply keeps streaming audio, so treat each drained frame as
          * progress (prevents a false "LLM stalled" timeout). Done HERE on the
          * LWS service thread — same thread as always_on_destroy — so touching
          * conn->always_on is lifetime-safe (a worker-thread bump would race the
          * ctx free; security/arch review). NULL-safe + PROCESSING-gated inside. */
         always_on_note_tts_activity(conn->always_on);
         break;
      case WS_RESP_AUDIO_END:
         send_audio_end_impl(conn->wsi, resp.audio.is_opus);
         break;
      case WS_RESP_MUSIC_DATA:
         send_binary_message(conn->wsi, WS_BIN_MUSIC_DATA, resp.audio.data, resp.audio.len);
         free(resp.audio.data);
         break;
      case WS_RESP_CONTEXT:
         send_context_impl(conn->wsi, resp.context.current_tokens, resp.context.max_tokens,
                           resp.context.threshold);
         break;
      case WS_RESP_STREAM_START:
         send_stream_start_impl(conn->wsi, resp.stream.stream_id, resp.stream.conversation_id);
         break;
      case WS_RESP_STREAM_DELTA:
         send_stream_delta_impl(conn->wsi, resp.stream.stream_id, resp.stream.conversation_id,
                                resp.stream.text);
         /* text[] is inline buffer - no free needed */
         break;
      case WS_RESP_STREAM_END:
         send_stream_end_impl(conn->wsi, resp.stream.stream_id, resp.stream.conversation_id,
                              resp.stream.text);
         /* text[] is inline buffer - no free needed */
         break;
      case WS_RESP_METRICS_UPDATE:
         send_metrics_impl(conn->wsi, resp.metrics.state, resp.metrics.ttft_ms,
                           resp.metrics.token_rate, resp.metrics.context_pct,
                           resp.metrics.conversation_id);
         break;
      case WS_RESP_COMPACTION_COMPLETE:
         send_compaction_impl(conn->wsi, resp.compaction.tokens_before,
                              resp.compaction.tokens_after, resp.compaction.messages_summarized,
                              resp.compaction.summary, resp.compaction.level);
         free(resp.compaction.summary);
         break;
      case WS_RESP_THINKING_START:
         send_thinking_start_impl(conn->wsi, resp.stream.stream_id, resp.stream.conversation_id,
                                  resp.stream.text);
         /* text[] reused for provider name - no free needed */
         break;
      case WS_RESP_THINKING_DELTA:
         send_thinking_delta_impl(conn->wsi, resp.stream.stream_id, resp.stream.conversation_id,
                                  resp.stream.text);
         /* text[] is inline buffer - no free needed */
         break;
      case WS_RESP_THINKING_END:
         send_thinking_end_impl(conn->wsi, resp.stream.stream_id, resp.stream.conversation_id,
                                resp.stream.text[0] == '1'); /* has_content flag */
         /* text[] reused for flag - no free needed */
         break;
      case WS_RESP_REASONING_SUMMARY:
         send_reasoning_summary_impl(conn->wsi, resp.stream.stream_id, resp.stream.conversation_id,
                                     atoi(resp.stream.text)); /* reasoning_tokens as string */
         /* text[] reused for token count - no free needed */
         break;
      case WS_RESP_CONVERSATION_RESET:
         send_conversation_reset_impl(conn->wsi);
         break;
      case WS_RESP_MUSIC_POSITION:
         send_music_position_impl(conn->wsi, resp.music_position.position_sec,
                                  resp.music_position.duration_sec);
         break;
      case WS_RESP_MUSIC_STATE:
      case WS_RESP_MUSIC_ERROR:
         /* Pre-serialized JSON from webui_music.c — just send the string */
         if (resp.music_json.json) {
            send_json_message(conn->wsi, resp.music_json.json);
            free(resp.music_json.json);
            resp.music_json.json = NULL;
         }
         break;
      case WS_RESP_SCHEDULER_NOTIFICATION:
         /* Pre-serialized JSON from scheduler — just send the string */
         if (resp.scheduler_json.json) {
            send_json_message(conn->wsi, resp.scheduler_json.json);
            free(resp.scheduler_json.json);
            resp.scheduler_json.json = NULL;
         }
         break;
      case WS_RESP_JSON:
         /* Generic pre-serialized JSON (init messages, etc.) */
         if (resp.generic_json.json) {
            send_json_message(conn->wsi, resp.generic_json.json);
            free(resp.generic_json.json);
            resp.generic_json.json = NULL;
         }
         break;
   }

   /* If more responses pending, ensure they get processed.
    * lws_callback_on_writable() handles responses for THIS connection.
    * lws_cancel_service() wakes the main loop for responses to OTHER connections. */
   if (more_pending) {
      lws_callback_on_writable(conn->wsi);
      if (s_lws_context) {
         lws_cancel_service(s_lws_context);
      }
   }
}

/* Legacy name for backward compatibility */
void process_response_queue(void) {
   process_one_response();
}


/* HTTP auth handlers and protocol callback moved to webui_http.c */
