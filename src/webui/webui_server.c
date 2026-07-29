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
 * WebUI Server Implementation - libwebsockets HTTP + WebSocket handling
 */

#define _GNU_SOURCE /* For strcasestr */

#include "webui/webui_server.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <json-c/json.h>
#include <libwebsockets.h>
#include <limits.h>
#include <mosquitto.h>
#include <net/if.h>
#include <pthread.h>
#include <signal.h>
#include <sodium.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#include "core/focus/focus_candidate_helpers.h" /* FOCUS_TEXT_MAX_BYTES */
#include "core/focus/focus_source.h"            /* focus_compose_result_t */
#include "memory/memory_db_aliases.h"           /* memory_db_proposal_count_pending */
#include "webui/build_focus_block.h"
#include "webui/webui_image_rehydrate.h"
#include "webui/webui_internal.h"

#ifdef ENABLE_WEBUI_AUDIO
#include "webui/webui_audio.h"
#endif

#include "config/config_env.h"
#include "config/config_parser.h"
#include "config/dawn_config.h"
#include "core/command_router.h"
#include "core/conv_stream.h"
#include "core/missed_notifications_db.h"
#include "core/ocp_helpers.h"
#include "core/rate_limiter.h"
#include "core/scheduler.h"
#include "core/scheduler_db.h"
#include "core/session_manager.h"
#include "core/text_filter.h"
#include "core/worker_pool.h"
#include "dawn.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_context.h"
#include "llm/llm_interface.h"
#include "llm/llm_local_provider.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "state_machine.h"
#include "tts/tts_preprocessing.h"
#include "ui/metrics.h"
#include "utils/string_utils.h"
#include "version.h"
#include "webui/webui_always_on.h"
#include "webui/webui_music.h"

#ifdef ENABLE_AUTH
#include "auth/auth_crypto.h"
#include "auth/auth_db.h"
#include "core/buf_printf.h"
#include "memory/memory_context.h"
#include "webui/webui_doc_library.h"
#ifdef DAWN_ENABLE_CALENDAR_TOOL
#include "webui/webui_calendar.h"
#endif
#ifdef DAWN_ENABLE_PHONE_TOOL
#include "webui/webui_phone_config.h"
#endif
#ifdef DAWN_ENABLE_EMAIL_TOOL
#include "webui/webui_email.h"
#endif
#include "webui/webui_contacts.h"
#if defined(DAWN_ENABLE_CALENDAR_TOOL) || defined(DAWN_ENABLE_EMAIL_TOOL)
#include "webui/webui_oauth.h"
#endif
/* HTTP rate limiting and CSRF constants moved to webui_http.c */
#endif /* ENABLE_AUTH */

/* =============================================================================
 * Module State
 *
 * These variables are non-static to allow access from split handler modules
 * via webui_internal.h. They should NOT be accessed from outside webui_*.c.
 * ============================================================================= */

struct lws_context *s_lws_context = NULL;
static pthread_t s_webui_thread;
_Atomic int s_running = 0;  // atomic: written on shutdown, read in server-thread loop
volatile int s_client_count = 0;
int s_port = 0;
char s_www_path[256] = { 0 };
pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Config modification mutex - protects against concurrent config reads during writes */
pthread_rwlock_t s_config_rwlock = PTHREAD_RWLOCK_INITIALIZER;

/* HTTP rate limiting state (s_csrf_used, s_csrf_rate, s_login_rate)
 * moved to webui_http.c */

/* =============================================================================
 * Response Queue (worker -> WebUI thread)
 *
 * Workers cannot call lws_write() directly (not thread-safe).
 * They queue responses here, then call lws_cancel_service() to wake
 * the WebUI thread, which processes the queue in LWS_CALLBACK_EVENT_WAIT_CANCELLED.
 *
 * ws_response_t is defined in webui_internal.h
 * ============================================================================= */

ws_response_t s_response_queue[WEBUI_RESPONSE_QUEUE_SIZE];
int s_queue_head = 0;
int s_queue_tail = 0;
pthread_mutex_t s_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

/* =============================================================================
 * Token-to-Session Mapping (for reconnection)
 *
 * token_mapping_t is defined in webui_internal.h
 * ============================================================================= */

token_mapping_t s_token_map[MAX_TOKEN_MAPPINGS];
pthread_mutex_t s_token_mutex = PTHREAD_MUTEX_INITIALIZER;

/* =============================================================================
 * Active Connection Registry (for proactive notifications like force logout)
 *
 * Tracks all active WebSocket connections so we can find and notify specific
 * clients (e.g., when their auth session is revoked).
 * ============================================================================= */

/* MAX_ACTIVE_CONNECTIONS, s_active_connections, s_conn_registry_mutex are
 * declared in webui_internal.h so sibling modules (broadcasts.c, etc.)
 * can iterate the registry without duplicating storage. */
ws_connection_t *s_active_connections[MAX_ACTIVE_CONNECTIONS];
pthread_mutex_t s_conn_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

/* deliver_missed_notifications + webui_broadcast_silent_observation moved
 * to webui_broadcasts.c; declarations live in webui_internal.h. */

static void register_connection(ws_connection_t *conn) {
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      if (s_active_connections[i] == NULL) {
         s_active_connections[i] = conn;
         pthread_mutex_unlock(&s_conn_registry_mutex);
         return;
      }
   }
   /* Registry full - shouldn't happen if MAX_ACTIVE_CONNECTIONS >= max_clients */
   OLOG_WARNING("WebUI: Connection registry full, cannot track connection");
   pthread_mutex_unlock(&s_conn_registry_mutex);
}

static void unregister_connection(ws_connection_t *conn) {
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      if (s_active_connections[i] == conn) {
         s_active_connections[i] = NULL;
         pthread_mutex_unlock(&s_conn_registry_mutex);
         return;
      }
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);
}

/* Constant-time comparison via libsodium (replaces hand-rolled secure_token_compare) */

/**
 * @brief Expire stale token mappings (must be called with s_token_mutex held)
 */
static void cleanup_expired_tokens(void) {
   time_t now = time(NULL);
   for (int i = 0; i < MAX_TOKEN_MAPPINGS; i++) {
      if (s_token_map[i].in_use && (now - s_token_map[i].created) > AUTH_COOKIE_MAX_AGE) {
         OLOG_INFO("WebUI: Expiring token slot %d (age %lds)", i,
                   (long)(now - s_token_map[i].created));
         explicit_bzero(s_token_map[i].token, sizeof(s_token_map[i].token));
         s_token_map[i].in_use = false;
      }
   }
}

void register_token(const char *token, uint32_t session_id) {
   pthread_mutex_lock(&s_token_mutex);

   /* Lazy cleanup: expire stale tokens on each registration */
   cleanup_expired_tokens();

   /* Find existing or empty slot */
   int empty_slot = -1;
   for (int i = 0; i < MAX_TOKEN_MAPPINGS; i++) {
      if (s_token_map[i].in_use &&
          sodium_memcmp(s_token_map[i].token, token, WEBUI_SESSION_TOKEN_LEN - 1) == 0) {
         /* Update existing */
         s_token_map[i].session_id = session_id;
         s_token_map[i].created = time(NULL);
         pthread_mutex_unlock(&s_token_mutex);
         return;
      }
      if (!s_token_map[i].in_use && empty_slot < 0) {
         empty_slot = i;
      }
   }

   if (empty_slot >= 0) {
      strncpy(s_token_map[empty_slot].token, token, WEBUI_SESSION_TOKEN_LEN - 1);
      s_token_map[empty_slot].token[WEBUI_SESSION_TOKEN_LEN - 1] = '\0';
      s_token_map[empty_slot].session_id = session_id;
      s_token_map[empty_slot].created = time(NULL);
      s_token_map[empty_slot].in_use = true;
   } else {
      /* Table full - prefer evicting entries for destroyed sessions */
      int evict = -1;
      for (int i = 0; i < MAX_TOKEN_MAPPINGS; i++) {
         if (s_token_map[i].in_use) {
            session_t *s = session_get_for_reconnect(s_token_map[i].session_id);
            if (!s) {
               evict = i; /* Session gone — safe to evict */
               break;
            }
            session_release(s);
         }
      }
      if (evict < 0) {
         /* All entries have live sessions — fall back to oldest */
         evict = 0;
         for (int i = 1; i < MAX_TOKEN_MAPPINGS; i++) {
            if (s_token_map[i].created < s_token_map[evict].created) {
               evict = i;
            }
         }
      }
      explicit_bzero(s_token_map[evict].token, sizeof(s_token_map[evict].token));
      strncpy(s_token_map[evict].token, token, WEBUI_SESSION_TOKEN_LEN - 1);
      s_token_map[evict].token[WEBUI_SESSION_TOKEN_LEN - 1] = '\0';
      s_token_map[evict].session_id = session_id;
      s_token_map[evict].created = time(NULL);
      s_token_map[evict].in_use = true;
   }

   pthread_mutex_unlock(&s_token_mutex);
}

void unregister_tokens_for_session(uint32_t session_id) {
   pthread_mutex_lock(&s_token_mutex);

   for (int i = 0; i < MAX_TOKEN_MAPPINGS; i++) {
      if (s_token_map[i].in_use && s_token_map[i].session_id == session_id) {
         OLOG_INFO("WebUI: Clearing token slot %d for destroyed session %u", i, session_id);
         explicit_bzero(s_token_map[i].token, sizeof(s_token_map[i].token));
         s_token_map[i].in_use = false;
      }
   }

   pthread_mutex_unlock(&s_token_mutex);
}

session_t *lookup_session_by_token(const char *token) {
   if (!token) {
      return NULL;
   }

   pthread_mutex_lock(&s_token_mutex);

   for (int i = 0; i < MAX_TOKEN_MAPPINGS; i++) {
      /* Use constant-time comparison to prevent timing attacks */
      if (s_token_map[i].in_use &&
          sodium_memcmp(s_token_map[i].token, token, WEBUI_SESSION_TOKEN_LEN - 1) == 0) {
         /* Check token age against AUTH_COOKIE_MAX_AGE */
         if ((time(NULL) - s_token_map[i].created) > AUTH_COOKIE_MAX_AGE) {
            OLOG_INFO("WebUI: Token %.4s... expired (session %u)", token,
                      s_token_map[i].session_id);
            explicit_bzero(s_token_map[i].token, sizeof(s_token_map[i].token));
            s_token_map[i].in_use = false;
            pthread_mutex_unlock(&s_token_mutex);
            return NULL;
         }

         uint32_t session_id = s_token_map[i].session_id;
         pthread_mutex_unlock(&s_token_mutex);

         /* Look up actual session - use reconnect variant to allow disconnected sessions */
         session_t *session = session_get_for_reconnect(session_id);
         if (session) {
            /* Session exists - reconnect handler will clear disconnected flag */
            OLOG_INFO("WebUI: Found existing session %u for token %.4s...", session_id, token);
            return session;
         }

         /* Session destroyed — clean up the stale token map entry */
         pthread_mutex_lock(&s_token_mutex);
         if (s_token_map[i].in_use && s_token_map[i].session_id == session_id) {
            explicit_bzero(s_token_map[i].token, sizeof(s_token_map[i].token));
            s_token_map[i].in_use = false;
         }
         pthread_mutex_unlock(&s_token_mutex);

         OLOG_INFO("WebUI: Token %.4s... mapped to session %u but session destroyed (cleaned up)",
                   token, session_id);
         return NULL;
      }
   }

   pthread_mutex_unlock(&s_token_mutex);
   return NULL;
}

/* Discovery cache and allowed path prefixes moved to webui_config.c */

/* =============================================================================
 * Per-WebSocket Connection Data
 *
 * ws_connection_t is defined in webui_internal.h
 * ============================================================================= */

/* =============================================================================
 * MIME Type Mapping
 * ============================================================================= */

static const struct {
   const char *extension;
   const char *mime_type;
} s_mime_types[] = {
   { ".html", "text/html" },
   { ".htm", "text/html" },
   { ".css", "text/css" },
   { ".js", "application/javascript" },
   { ".json", "application/json" },
   { ".wasm", "application/wasm" },
   { ".png", "image/png" },
   { ".jpg", "image/jpeg" },
   { ".jpeg", "image/jpeg" },
   { ".gif", "image/gif" },
   { ".svg", "image/svg+xml" },
   { ".ico", "image/x-icon" },
   { ".woff", "font/woff" },
   { ".woff2", "font/woff2" },
   { ".ttf", "font/ttf" },
   { ".txt", "text/plain" },
   { NULL, NULL },
};

const char *get_mime_type(const char *path) {
   const char *ext = strrchr(path, '.');
   if (!ext) {
      return "application/octet-stream";
   }

   for (int i = 0; s_mime_types[i].extension != NULL; i++) {
      if (strcasecmp(ext, s_mime_types[i].extension) == 0) {
         return s_mime_types[i].mime_type;
      }
   }

   return "application/octet-stream";
}

/**
 * @brief Check if a path contains directory traversal patterns
 *
 * Checks for literal ".." as well as URL-encoded variants (%2e, %252e)
 * to prevent path traversal attacks.
 *
 * @param path The URL path to check
 * @return true if traversal pattern detected, false if path is safe
 */
bool contains_path_traversal(const char *path) {
   if (!path) {
      return false;
   }

   /* Check for literal ".." as a path component (not inside filenames like "file..ext") */
   const char *p = path;
   while ((p = strstr(p, "..")) != NULL) {
      /* Check if preceded by '/' or at start of string */
      bool at_start = (p == path) || (*(p - 1) == '/');
      /* Check if followed by '/' or null or end of string */
      bool at_end = (p[2] == '\0') || (p[2] == '/');
      if (at_start && at_end) {
         return true;
      }
      p += 2;
   }

   /* Check for URL-encoded variants (case-insensitive) */
   /* %2e = ".", so %2e%2e = ".." */
   if (strcasestr(path, "%2e%2e") != NULL) {
      return true;
   }

   /* Single encoded dot followed by literal dot or vice versa */
   if (strcasestr(path, "%2e.") != NULL || strcasestr(path, ".%2e") != NULL) {
      return true;
   }

   /* Double-encoded: %252e = "%2e" after first decode */
   if (strcasestr(path, "%252e") != NULL) {
      return true;
   }

   return false;
}

/**
 * @brief Validate that a resolved path is within the allowed directory
 *
 * Uses realpath() to resolve symlinks and relative paths, then verifies
 * the canonical path is within the www directory.
 *
 * @param filepath The filesystem path to validate
 * @param www_path The allowed base directory (www path)
 * @return true if path is safe (within www_path), false otherwise
 */
bool is_path_within_www(const char *filepath, const char *www_path) {
   char resolved_path[PATH_MAX];
   char resolved_www[PATH_MAX];

   /* Resolve the www base path */
   if (realpath(www_path, resolved_www) == NULL) {
      OLOG_ERROR("WebUI: Cannot resolve www path: %s", www_path);
      return false;
   }

   /* Resolve the requested file path */
   if (realpath(filepath, resolved_path) == NULL) {
      /* File doesn't exist - check parent directory instead */
      /* This allows serving files that don't exist yet (404 handled elsewhere) */
      char *filepath_copy = strdup(filepath);
      if (!filepath_copy) {
         return false;
      }

      /* Find last slash to get parent directory */
      char *last_slash = strrchr(filepath_copy, '/');
      if (last_slash && last_slash != filepath_copy) {
         *last_slash = '\0';
         if (realpath(filepath_copy, resolved_path) == NULL) {
            free(filepath_copy);
            return false;
         }
      } else {
         free(filepath_copy);
         return false;
      }
      free(filepath_copy);
   }

   /* Ensure resolved path starts with resolved www path */
   size_t www_len = strlen(resolved_www);
   if (strncmp(resolved_path, resolved_www, www_len) != 0) {
      return false;
   }

   /* Ensure it's either exact match or followed by '/' */
   if (resolved_path[www_len] != '\0' && resolved_path[www_len] != '/') {
      return false;
   }

   return true;
}


/* =============================================================================
 * HTTP Session Data
 *
 * struct http_session_data and related constants are in webui_internal.h
 * ============================================================================= */

/* =============================================================================
 * Session Token Generation
 * ============================================================================= */

/**
 * @brief Generate a cryptographically secure session token
 *
 * @param token_out Buffer to store the hex-encoded token (must be WEBUI_SESSION_TOKEN_LEN)
 * @return 0 on success, 1 on failure (token_out will be empty string)
 */
int generate_session_token(char token_out[WEBUI_SESSION_TOKEN_LEN]) {
   uint8_t random_bytes[16];
   if (getrandom(random_bytes, 16, 0) != 16) {
      /* Security: fail instead of using weak random - getrandom should never fail on modern Linux
       */
      OLOG_ERROR("getrandom() failed - cannot generate secure session token");
      token_out[0] = '\0';
      return 1;
   }
   for (int i = 0; i < 16; i++) {
      snprintf(&token_out[i * 2], 3, "%02x", random_bytes[i]);
   }
   token_out[32] = '\0';
   return 0;
}

/* =============================================================================
 * JSON Message Handling
 * ============================================================================= */

/* handle_text_message, handle_cancel_message, handle_json_message moved
 * to webui_message_dispatch.c; declarations live in webui_internal.h. */
/* Config handlers: handle_get_config, handle_set_config, handle_set_secrets,
 * handle_get_audio_devices, handle_list_models, handle_list_interfaces
 * moved to webui_config.c (declarations in webui_internal.h) */
/* Tools handlers: handle_get_tools_config, handle_set_tools_config
 * moved to webui_tools.c (declarations in webui_internal.h) */
/* Audio handler: handle_binary_message
 * moved to webui_audio.c (declaration in webui_internal.h) */
/* handle_get_metrics and handle_smart_home_message moved-via-extern;
 * declarations live in webui_internal.h.  Definitions still in this file. */


/* Queue a JSON response for delivery via the response queue.
 * Safe to call from any context — the JSON is serialized and copied. */
void send_json_response(ws_connection_t *conn, json_object *response) {
   const char *json_str = json_object_to_json_string(response);
   if (!json_str) {
      OLOG_ERROR("WebUI: Failed to serialize JSON response");
      return;
   }

   ws_response_t resp = { 0 };
   resp.session = conn->session;
   resp.type = WS_RESP_JSON;
   resp.generic_json.json = strdup(json_str);
   if (!resp.generic_json.json) {
      OLOG_ERROR("WebUI: Failed to allocate JSON response string");
      return;
   }
   queue_response(&resp);
}


/* Audio device helpers, model/interface discovery moved to webui_config.c */


/* Tool configuration handlers moved to webui_tools.c */


/* =============================================================================
 * Metrics Handler
 * ============================================================================= */

void handle_get_metrics(ws_connection_t *conn) {
   if (!conn_require_auth(conn)) {
      return;
   }

   dawn_metrics_t snapshot;
   metrics_get_snapshot(&snapshot);

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("get_metrics_response"));

   json_object *payload = json_object_new_object();

   /* Session statistics */
   json_object *session = json_object_new_object();
   json_object_object_add(session, "uptime_seconds", json_object_new_int64(metrics_get_uptime()));
   json_object_object_add(session, "queries_total", json_object_new_int(snapshot.queries_total));
   json_object_object_add(session, "queries_cloud", json_object_new_int(snapshot.queries_cloud));
   json_object_object_add(session, "queries_local", json_object_new_int(snapshot.queries_local));
   json_object_object_add(session, "errors", json_object_new_int(snapshot.errors_count));
   json_object_object_add(session, "fallbacks", json_object_new_int(snapshot.fallbacks_count));
   json_object_object_add(session, "bargeins", json_object_new_int(snapshot.bargein_count));
   json_object_object_add(payload, "session", session);

   /* Token usage */
   json_object *tokens = json_object_new_object();
   json_object_object_add(tokens, "cloud_input",
                          json_object_new_int64(snapshot.tokens_cloud_input));
   json_object_object_add(tokens, "cloud_output",
                          json_object_new_int64(snapshot.tokens_cloud_output));
   json_object_object_add(tokens, "local_input",
                          json_object_new_int64(snapshot.tokens_local_input));
   json_object_object_add(tokens, "local_output",
                          json_object_new_int64(snapshot.tokens_local_output));
   json_object_object_add(tokens, "cached", json_object_new_int64(snapshot.tokens_cached));
   json_object_object_add(payload, "tokens", tokens);

   /* Last query timing */
   json_object *last = json_object_new_object();
   json_object_object_add(last, "vad_ms", json_object_new_double(snapshot.last_vad_time_ms));
   json_object_object_add(last, "asr_ms", json_object_new_double(snapshot.last_asr_time_ms));
   json_object_object_add(last, "asr_rtf", json_object_new_double(snapshot.last_asr_rtf));
   json_object_object_add(last, "llm_ttft_ms", json_object_new_double(snapshot.last_llm_ttft_ms));
   json_object_object_add(last, "llm_total_ms", json_object_new_double(snapshot.last_llm_total_ms));
   json_object_object_add(last, "tts_ms", json_object_new_double(snapshot.last_tts_time_ms));
   json_object_object_add(last, "pipeline_ms",
                          json_object_new_double(snapshot.last_total_pipeline_ms));
   json_object_object_add(payload, "last", last);

   /* Average timing */
   json_object *avg = json_object_new_object();
   json_object_object_add(avg, "vad_ms", json_object_new_double(snapshot.avg_vad_ms));
   json_object_object_add(avg, "asr_ms", json_object_new_double(snapshot.avg_asr_ms));
   json_object_object_add(avg, "asr_rtf", json_object_new_double(snapshot.avg_asr_rtf));
   json_object_object_add(avg, "llm_ttft_ms", json_object_new_double(snapshot.avg_llm_ttft_ms));
   json_object_object_add(avg, "llm_total_ms", json_object_new_double(snapshot.avg_llm_total_ms));
   json_object_object_add(avg, "tts_ms", json_object_new_double(snapshot.avg_tts_ms));
   json_object_object_add(avg, "pipeline_ms",
                          json_object_new_double(snapshot.avg_total_pipeline_ms));
   json_object_object_add(payload, "averages", avg);

   /* Real-time state */
   json_object *state = json_object_new_object();
   json_object_object_add(state, "current",
                          json_object_new_string(dawn_state_name(snapshot.current_state)));
   json_object_object_add(state, "vad_probability",
                          json_object_new_double(snapshot.current_vad_probability));
   json_object_object_add(state, "audio_buffer_fill",
                          json_object_new_double(snapshot.audio_buffer_fill_pct));
   json_object_object_add(payload, "state", state);

   /* AEC status */
   json_object *aec = json_object_new_object();
   json_object_object_add(aec, "enabled", json_object_new_boolean(snapshot.aec_enabled));
   json_object_object_add(aec, "calibrated", json_object_new_boolean(snapshot.aec_calibrated));
   json_object_object_add(aec, "delay_ms", json_object_new_int(snapshot.aec_delay_ms));
   json_object_object_add(aec, "correlation", json_object_new_double(snapshot.aec_correlation));
   json_object_object_add(payload, "aec", aec);

   /* Summarizer stats */
   json_object *summarizer = json_object_new_object();
   json_object_object_add(summarizer, "backend",
                          json_object_new_string(snapshot.summarizer_backend));
   json_object_object_add(summarizer, "threshold",
                          json_object_new_int64(snapshot.summarizer_threshold));
   json_object_object_add(summarizer, "calls", json_object_new_int(snapshot.summarizer_call_count));
   json_object_object_add(payload, "summarizer", summarizer);

   json_object_object_add(response, "payload", payload);
   send_json_response(conn, response);
   json_object_put(response);
}


/* handle_set_tools_config moved to webui_tools.c */


/* Admin handlers moved to webui_admin.c */


/* Personal settings handlers moved to webui_settings.c */


/* Session handlers moved to webui_session.c */


/* History handlers moved to webui_history.c */

/* =============================================================================
 * Smart Home Message Dispatch (Home Assistant)
 *
 * Extracted into a helper to avoid fragile #ifdef interleaving in the main
 * else-if message handler chain.  Returns true if the message was handled.
 * ============================================================================= */
bool handle_smart_home_message(ws_connection_t *conn,
                               const char *type,
                               struct json_object *payload) {
#ifdef DAWN_ENABLE_HOMEASSISTANT_TOOL
   if (strcmp(type, "ha_status") == 0) {
      handle_ha_status(conn);
      return true;
   }
   if (strcmp(type, "ha_test_connection") == 0) {
      handle_ha_test_connection(conn);
      return true;
   }
   if (strcmp(type, "ha_list_entities") == 0) {
      handle_ha_list_entities(conn);
      return true;
   }
   if (strcmp(type, "ha_refresh_entities") == 0) {
      handle_ha_refresh_entities(conn);
      return true;
   }
#endif /* DAWN_ENABLE_HOMEASSISTANT_TOOL */

#ifdef DAWN_ENABLE_PHONE_TOOL
   if (strcmp(type, "get_phone_audio_config") == 0) {
      handle_phone_audio_config_get(conn);
      return true;
   }
#endif /* DAWN_ENABLE_PHONE_TOOL */

   (void)payload; /* Suppress unused warning when the gated tools are all disabled */
   return false;
}

/* =============================================================================
 * WebSocket Protocol Callbacks
 * ============================================================================= */

/* Forward declaration — defined after handle_text_message */
static bool webui_conn_create_session(ws_connection_t *conn);

static int callback_websocket(struct lws *wsi,
                              enum lws_callback_reasons reason,
                              void *user,
                              void *in,
                              size_t len) {
   ws_connection_t *conn = (ws_connection_t *)user;

   switch (reason) {
      case LWS_CALLBACK_ESTABLISHED: {
         /*
          * New WebSocket connection - defer session creation until init message.
          * This allows reconnecting clients (with valid token) to reuse their
          * existing session without counting against max_clients during the
          * brief overlap when refreshing a browser page.
          */
         memset(conn, 0, sizeof(*conn));
         conn->wsi = wsi;
         conn->session = NULL; /* Will be created/assigned on init message */
         conn->volume = 0.8f;  /* Default music volume (0.0-1.0) */

         /* Capture client IP at connection time for reliable logging later */
         lws_get_peer_simple(wsi, conn->client_ip, sizeof(conn->client_ip));
         if (conn->client_ip[0] == '\0') {
            strncpy(conn->client_ip, "(unknown)", sizeof(conn->client_ip) - 1);
         }

         /* Populate auth state from HTTP cookie (if present) */
         auth_session_t auth_session;
         if (is_request_authenticated(wsi, &auth_session)) {
            conn->authenticated = true;
            conn->auth_user_id = auth_session.user_id;
            strncpy(conn->auth_session_token, auth_session.token,
                    sizeof(conn->auth_session_token) - 1);
            conn->auth_session_token[sizeof(conn->auth_session_token) - 1] = '\0';
            strncpy(conn->username, auth_session.username, sizeof(conn->username) - 1);
            conn->username[sizeof(conn->username) - 1] = '\0';
            OLOG_INFO("WebUI: WebSocket authenticated as user '%s' (id=%d)", conn->username,
                      conn->auth_user_id);
         } else {
            OLOG_INFO("WebUI: WebSocket connection established (unauthenticated)");
         }

         /* Register in connection registry for proactive notifications */
         register_connection(conn);

         OLOG_INFO("WebUI: WebSocket connection established, awaiting init message");
         break;
      }

      case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE: {
         /* The peer (browser) sent a WebSocket CLOSE frame — decode its code +
          * reason. NOTE: a pure 1006 (abnormal, no close frame) NEVER reaches
          * here, so if a disconnect shows CLOSED with no preceding peer-close
          * log, the socket died at the transport layer (not a clean app close) —
          * that absence is itself the diagnostic signal for the 1006 drops. */
         uint16_t code = 0;
         char reason[128] = { 0 };
         if (in && len >= 2) {
            const unsigned char *p = (const unsigned char *)in;
            code = (uint16_t)((p[0] << 8) | p[1]);
            size_t rlen = len - 2;
            if (rlen > sizeof(reason) - 1)
               rlen = sizeof(reason) - 1;
            memcpy(reason, p + 2, rlen);
            reason[rlen] = '\0';
         }
         OLOG_INFO("WebUI: peer-initiated WS close: code=%u reason=\"%s\" (session %u)", code,
                   reason, conn && conn->session ? conn->session->session_id : 0);
         break; /* return 0: let lws finish the close handshake */
      }

      case LWS_CALLBACK_CLOSED: {
         /* Unregister from connection registry */
         unregister_connection(conn);
         /* WebSocket disconnected.
          * Atomic load: maintenance thread may have NULLed conn->session.
          * If no LWS_CALLBACK_WS_PEER_INITIATED_CLOSE was logged just above for
          * this connection, the close was transport-level (abnormal 1006) — the
          * lws NOTICE routed via webui_lws_log_emit should name the cause. */
         session_t *closing_session = conn_get_session(conn);
         OLOG_INFO("WebUI: WebSocket client disconnecting (session %u)",
                   closing_session ? closing_session->session_id : 0);

         if (closing_session) {
            /* Client left: gate emission only.  Post background-jobs Phase 1 the
             * in-flight turn KEEPS generating and is persisted server-side — we
             * do NOT cancel it here (that is session_teardown_flags' job, on
             * actual destroy).  A reconnect or a new request supersedes it. */
            session_mark_disconnected(closing_session);
            closing_session->client_data = NULL;

            /* Release our reference to the session.
             * Session manager will clean it up when ref_count reaches 0. */
            OLOG_INFO("WebUI: Releasing session reference...");
            session_release(closing_session);
            OLOG_INFO("WebUI: Session reference released");
            conn->session = NULL;
         }

         /* Clear wsi reference */
         conn->wsi = NULL;

         /* Free any pending audio buffer */
         if (conn->audio_buffer) {
            free(conn->audio_buffer);
            conn->audio_buffer = NULL;
         }

         /* Free any pending text fragment buffer */
         if (conn->text_buffer) {
            free(conn->text_buffer);
            conn->text_buffer = NULL;
            conn->text_buffer_len = 0;
            conn->text_buffer_cap = 0;
         }

         /* Clean up always-on state */
         if (conn->always_on) {
            always_on_destroy(conn->always_on);
            conn->always_on = NULL;
         }

         /* Clean up music streaming state */
         webui_music_session_cleanup(conn);

         /* Decrement client count if this connection was counted.
          * Uses conn->counted instead of checking session != NULL because
          * session may have been detached (set to NULL) by webui_detach_session
          * during session expiry while the WS connection stayed alive. */
         if (conn->counted) {
            pthread_mutex_lock(&s_mutex);
            if (s_client_count > 0) {
               s_client_count--;
            }
            pthread_mutex_unlock(&s_mutex);
            conn->counted = false;
         }

         OLOG_INFO("WebUI: WebSocket client disconnected (total: %d)", s_client_count);
         break;
      }

      case LWS_CALLBACK_RECEIVE: {
         /* Message received from client.
          * Atomic load: maintenance thread may have NULLed conn->session
          * via webui_detach_session() on session expiry. */
         if (!conn_get_session(conn)) {
            /* Session is NULL — either this is the first message (init/reconnect)
             * or the conversation session expired while the WS connection stayed alive.
             * If already authenticated, auto-create a new session so the user
             * doesn't have to log in again. */
            if (conn->authenticated) {
               OLOG_INFO("WebUI: Session expired for authenticated connection, auto-creating");
               if (!webui_conn_create_session(conn)) {
                  break; /* Error already sent to client */
               }
               /* Fall through to normal message handling below */
            } else {
               /*
                * Not yet authenticated - this must be the init/reconnect message.
                * Parse it to determine if we're reconnecting with a token or
                * need a new session. Check client limits here.
                */
               char *json_str = strndup((const char *)in, len);
               if (!json_str) {
                  OLOG_ERROR("WebUI: Failed to allocate init message buffer");
                  return LWS_CLOSE_CONNECTION;
               }

               struct json_object *root = json_tokener_parse(json_str);
               free(json_str);

               if (!root) {
                  OLOG_WARNING("WebUI: Invalid JSON in init message");
                  return LWS_CLOSE_CONNECTION;
               }

               struct json_object *type_obj;
               const char *type = NULL;
               if (json_object_object_get_ex(root, "type", &type_obj)) {
                  type = json_object_get_string(type_obj);
               }

               struct json_object *payload = NULL;
               json_object_object_get_ex(root, "payload", &payload);

               OLOG_INFO("WebUI: Init message received, type=%s", type ? type : "(null)");

               bool is_reconnect = false;
               session_t *existing_session = NULL;

               /* Check for reconnection token */
               if (type && strcmp(type, "reconnect") == 0 && payload) {
                  struct json_object *token_obj;
                  if (json_object_object_get_ex(payload, "token", &token_obj)) {
                     const char *token = json_object_get_string(token_obj);
                     if (token && strlen(token) > 0) {
                        existing_session = lookup_session_by_token(token);
                        if (existing_session) {
                           is_reconnect = true;
                           conn->session = existing_session;
                           existing_session->client_data = conn;
                           existing_session->disconnected = false;
                           strncpy(conn->session_token, token, WEBUI_SESSION_TOKEN_LEN - 1);
                           conn->session_token[WEBUI_SESSION_TOKEN_LEN - 1] = '\0';

                           /* Check for Opus codec support */
                           conn->use_opus = check_opus_capability(payload);

                           /* Check for TTS preference (default off) */
                           conn->tts_enabled = false;
                           struct json_object *tts_obj;
                           if (json_object_object_get_ex(payload, "tts_enabled", &tts_obj)) {
                              conn->tts_enabled = json_object_get_boolean(tts_obj);
                           }

                           /* Reconnections still count against client limit */
                           pthread_mutex_lock(&s_mutex);
                           s_client_count++;
                           pthread_mutex_unlock(&s_mutex);
                           conn->counted = true;

                           OLOG_INFO(
                               "WebUI: Reconnected to session %u with token %.4s... (total: %d, "
                               "opus: %s, tts: %s)",
                               existing_session->session_id, token, s_client_count,
                               conn->use_opus ? "yes" : "no", conn->tts_enabled ? "yes" : "no");

                           /* Queue init messages (one lws_write per callback) */
                           queue_init_messages(conn, token);
                        }
                     }
                  }
               }

               /* Check for satellite registration (DAP2 Tier 1) */
               if (type && strcmp(type, "satellite_register") == 0 && payload) {
                  pthread_mutex_lock(&s_mutex);
                  if (s_client_count >= g_config.webui.max_clients) {
                     pthread_mutex_unlock(&s_mutex);
                     OLOG_WARNING("WebUI: Satellite rejected - max clients reached (%d)",
                                  g_config.webui.max_clients);
                     send_error_impl(wsi, "MAX_CLIENTS",
                                     "Maximum clients reached. Please try again later.");
                     json_object_put(root);
                     return LWS_CLOSE_CONNECTION;
                  }
                  s_client_count++;
                  pthread_mutex_unlock(&s_mutex);
                  conn->counted = true;

                  /* Delegate to satellite handler for full registration
                   * (session_create_dap2 handles both new sessions and reconnection) */
                  conn->is_satellite = true;
                  handle_satellite_register(conn, payload);

                  /* If registration failed, session won't be set */
                  if (conn->session) {
                     /* Mark authenticated so broadcasts (scheduler notifications, etc.)
                      * include this satellite. Browser clients get this from cookie auth
                      * at LWS_CALLBACK_ESTABLISHED; satellites authenticate via
                      * registration key in handle_satellite_register instead. */
                     conn->authenticated = true;
                  }
                  if (!conn->session) {
                     OLOG_ERROR("WebUI: Failed to create satellite session");
                     pthread_mutex_lock(&s_mutex);
                     s_client_count--;
                     pthread_mutex_unlock(&s_mutex);
                     conn->counted = false;
                     json_object_put(root);
                     return LWS_CLOSE_CONNECTION;
                  }
                  json_object_put(root);
                  break;
               }

               /* If not reconnecting, create new session (with client limit check) */
               if (!is_reconnect) {
                  pthread_mutex_lock(&s_mutex);
                  if (s_client_count >= g_config.webui.max_clients) {
                     pthread_mutex_unlock(&s_mutex);
                     OLOG_WARNING("WebUI: Connection rejected - max clients reached (%d)",
                                  g_config.webui.max_clients);
                     send_error_impl(wsi, "MAX_CLIENTS",
                                     "Maximum WebUI clients reached. Please try again later.");
                     json_object_put(root);
                     return LWS_CLOSE_CONNECTION;
                  }
                  s_client_count++;
                  pthread_mutex_unlock(&s_mutex);
                  conn->counted = true;

                  conn->session = session_create(SESSION_TYPE_WEBUI, -1);
                  if (!conn->session) {
                     OLOG_ERROR("WebUI: Failed to create session");
                     send_error_impl(wsi, "SESSION_LIMIT", "Maximum sessions reached");
                     pthread_mutex_lock(&s_mutex);
                     s_client_count--;
                     pthread_mutex_unlock(&s_mutex);
                     conn->counted = false;
                     json_object_put(root);
                     return LWS_CLOSE_CONNECTION;
                  }

                  /* Set user_id for metrics and memory extraction */
                  session_set_metrics_user(conn->session, conn->auth_user_id);
                  /* Build personalized prompt with user settings + memory context */
                  char *prompt = session_manager_build_system_prompt_string(conn->auth_user_id);
                  session_init_system_prompt(conn->session,
                                             prompt ? prompt : get_remote_command_prompt());
                  free(prompt);
                  conn->session->client_data = conn;

                  /* Check for Opus codec support */
                  conn->use_opus = check_opus_capability(payload);

                  /* Check for TTS preference (default off) */
                  conn->tts_enabled = false;
                  struct json_object *tts_obj;
                  if (json_object_object_get_ex(payload, "tts_enabled", &tts_obj)) {
                     conn->tts_enabled = json_object_get_boolean(tts_obj);
                  }

                  if (generate_session_token(conn->session_token) != 0) {
                     OLOG_ERROR("WebUI: Failed to generate session token");
                     session_destroy(conn->session->session_id);
                     conn->session = NULL;
                     pthread_mutex_lock(&s_mutex);
                     s_client_count--;
                     pthread_mutex_unlock(&s_mutex);
                     conn->counted = false;
                     json_object_put(root);
                     return LWS_CLOSE_CONNECTION;
                  }
                  register_token(conn->session_token, conn->session->session_id);

                  OLOG_INFO("WebUI: New session %u created (token %.4s..., total: %d, opus: %s, "
                            "tts: %s)",
                            conn->session->session_id, conn->session_token, s_client_count,
                            conn->use_opus ? "yes" : "no", conn->tts_enabled ? "yes" : "no");

                  queue_init_messages(conn, conn->session_token);
               }

               json_object_put(root);
               break; /* Don't process this message further - it was just the init */
            }         /* end else (not authenticated — init message) */
         }

         session_touch(conn->session);

         /* Deliver any queued missed notifications once the session is ready and
          * the user is authenticated. One-shot per connection. */
         if (!conn->missed_notif_delivered && conn->authenticated && conn->auth_user_id > 0) {
            deliver_missed_notifications(conn);
            conn->missed_notif_delivered = true;
         }

         int is_final = lws_is_final_fragment(wsi);
         int is_binary = lws_frame_is_binary(wsi);

         if (is_binary) {
#ifdef ENABLE_WEBUI_AUDIO
            /* Handle WebSocket frame fragmentation for binary messages */
            if (conn->in_binary_fragment) {
               /* Continuation of a fragmented message - append ALL bytes as payload */
               const uint8_t *data = (const uint8_t *)in;
               size_t data_len = len;

               if (conn->binary_msg_type == WS_BIN_AUDIO_IN && data_len > 0) {
                  /* Append continuation data to audio buffer — expand if needed
                   * (mirrors the realloc logic in handle_binary_message) */
                  if (conn->audio_buffer &&
                      conn->audio_buffer_len + data_len > conn->audio_buffer_capacity) {
                     size_t new_capacity = conn->audio_buffer_capacity * 2;
                     while (new_capacity < conn->audio_buffer_len + data_len)
                        new_capacity *= 2;
                     if (new_capacity > WEBUI_AUDIO_MAX_CAPACITY) {
                        OLOG_WARNING("WebUI: Fragment would exceed max audio capacity (%d bytes)",
                                     WEBUI_AUDIO_MAX_CAPACITY);
                        send_error_impl(conn->wsi, "BUFFER_FULL", "Recording too long");
                        conn->in_binary_fragment = false;
                     } else {
                        uint8_t *new_buffer = realloc(conn->audio_buffer, new_capacity);
                        if (!new_buffer) {
                           OLOG_ERROR("WebUI: Failed to expand audio buffer in fragment");
                           send_error_impl(conn->wsi, "BUFFER_ERROR",
                                           "Audio buffer allocation failed");
                           conn->in_binary_fragment = false;
                        } else {
                           conn->audio_buffer = new_buffer;
                           conn->audio_buffer_capacity = new_capacity;
                        }
                     }
                  }
                  if (conn->audio_buffer && conn->in_binary_fragment &&
                      conn->audio_buffer_len + data_len <= conn->audio_buffer_capacity) {
                     memcpy(conn->audio_buffer + conn->audio_buffer_len, data, data_len);
                     conn->audio_buffer_len += data_len;
                     /* Per-fragment protocol tracing — fires ~20x/sec during
                      * raw-PCM always-on streaming (9600B chunks split by the
                      * lws RX buffer). Developer-only: OLOG_DEBUG is a no-op
                      * unless built with -DDEBUG. */
                     OLOG_DEBUG("WebUI: Fragment continuation, added %zu bytes (total: %zu)",
                                data_len, conn->audio_buffer_len);
                  }
               }

               /* Check if this is the final fragment */
               if (is_final) {
                  conn->in_binary_fragment = false;

                  /* Always-on: process the fully reassembled audio frame.
                   * conn->audio_buffer has accumulated all fragment payloads.
                   * Prepend the type byte so handle_binary_message can parse it. */
                  if (conn->always_on && conn->binary_msg_type == WS_BIN_AUDIO_IN &&
                      always_on_get_state(conn->always_on) != ALWAYS_ON_DISABLED) {
                     if (conn->audio_buffer && conn->audio_buffer_len > 0) {
                        always_on_process_audio(conn->always_on, conn->audio_buffer,
                                                conn->audio_buffer_len, conn->use_opus, conn);
                     }
                     /* Clear the accumulation buffer */
                     conn->audio_buffer_len = 0;
                  }
               }
            } else {
               /* New message - parse type byte and handle */
               handle_binary_message(conn, (const uint8_t *)in, len);

               /* If not final, track that we're in a fragmented message */
               if (!is_final && len > 0) {
                  conn->in_binary_fragment = true;
                  conn->binary_msg_type = ((const uint8_t *)in)[0];
               }

               /* Always-on: for non-fragmented complete messages, route audio
                * from the accumulation buffer (handle_binary_message appended it). */
               if (is_final && conn->always_on && len > 0 &&
                   ((const uint8_t *)in)[0] == WS_BIN_AUDIO_IN &&
                   always_on_get_state(conn->always_on) != ALWAYS_ON_DISABLED) {
                  if (conn->audio_buffer && conn->audio_buffer_len > 0) {
                     always_on_process_audio(conn->always_on, conn->audio_buffer,
                                             conn->audio_buffer_len, conn->use_opus, conn);
                     conn->audio_buffer_len = 0;
                  }
               }
            }
#else
            OLOG_WARNING("WebUI: Audio not enabled, ignoring binary message (%zu bytes)", len);
#endif
         } else {
            /* Text message (JSON control) - handle fragmentation */
            if (is_final && conn->text_buffer_len == 0) {
               /* Unfragmented message (common case) - process directly */
               handle_json_message(conn, (const char *)in, len);
            } else {
               /* Check for integer overflow before calculating needed size */
               if (len > SIZE_MAX - conn->text_buffer_len - 1) {
                  OLOG_ERROR("WebUI: Text buffer size overflow detected");
                  conn->text_buffer_len = 0;
                  break;
               }

               /* Fragmented message - accumulate in buffer */
               size_t needed = conn->text_buffer_len + len + 1; /* +1 for null terminator */
               if (needed > conn->text_buffer_cap) {
                  /* Grow buffer (exponential growth, capped for safety) */
                  size_t new_cap = conn->text_buffer_cap ? conn->text_buffer_cap * 2
                                                         : WEBUI_TEXT_BUFFER_INITIAL_CAP;
                  while (new_cap < needed) {
                     new_cap *= 2;
                  }
                  if (new_cap > WEBUI_TEXT_BUFFER_MAX_CAP) {
                     OLOG_ERROR("WebUI: Text message too large (>%d bytes), dropping",
                                WEBUI_TEXT_BUFFER_MAX_CAP);
                     conn->text_buffer_len = 0; /* Reset for next message */
                     break;
                  }
                  char *new_buf = realloc(conn->text_buffer, new_cap);
                  if (!new_buf) {
                     OLOG_ERROR("WebUI: Failed to allocate text buffer (%zu bytes)", new_cap);
                     conn->text_buffer_len = 0;
                     break;
                  }
                  conn->text_buffer = new_buf;
                  conn->text_buffer_cap = new_cap;
               }

               /* Append data */
               memcpy(conn->text_buffer + conn->text_buffer_len, in, len);
               conn->text_buffer_len += len;
               conn->text_buffer[conn->text_buffer_len] = '\0';

               if (is_final) {
                  /* Complete message received - process it */
                  handle_json_message(conn, conn->text_buffer, conn->text_buffer_len);
                  conn->text_buffer_len = 0; /* Reset for next message (keep buffer) */
               }
            }
         }
         break;
      }

      case LWS_CALLBACK_SERVER_WRITEABLE:
         /* Ready to send more data - process next queued response */
         process_one_response();
         break;

      case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
         /* lws_cancel_service() was called - process response queue */
         process_response_queue();
         break;

      default:
         break;
   }

   return 0;
}

/* =============================================================================
 * Protocol Definitions
 * ============================================================================= */

static struct lws_protocols s_protocols[] = {
   /* HTTP protocol (must be first) */
   {
       .name = "http",
       .callback = callback_http,
       .per_session_data_size = sizeof(struct http_session_data),
       .rx_buffer_size = 0,
   },
   /* WebSocket protocol */
   {
       .name = WEBUI_SUBPROTOCOL,
       .callback = callback_websocket,
       .per_session_data_size = sizeof(ws_connection_t),
       .rx_buffer_size = 8192, /* Match DAP packet size */
   },
   /* Terminator */
   { NULL, NULL, 0, 0 },
};

/* =============================================================================
 * Server Thread
 * ============================================================================= */

static void *webui_thread_func(void *arg) {
   (void)arg;

   OLOG_INFO("WebUI: Server thread started");

   while (s_running) {
      /* Process events with 5ms timeout (fast for music streaming ~50fps).
       * lws_cancel_service() interrupts the wait when responses are queued. */
      lws_service(s_lws_context, 5);

      /* Process one pending response per iteration.
       * The writeable callback chain handles additional responses. */
      process_response_queue();

      /* Always-on timeout checks (~1Hz, not every 5ms iteration) */
      {
         static int64_t last_timeout_check_ms = 0;
         struct timespec ts;
         clock_gettime(CLOCK_MONOTONIC, &ts);
         int64_t now_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
         if (now_ms - last_timeout_check_ms >= 1000) {
            last_timeout_check_ms = now_ms;
            pthread_mutex_lock(&s_conn_registry_mutex);
            for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
               ws_connection_t *c = s_active_connections[i];
               if (c && c->always_on) {
                  if (always_on_check_timeouts(c->always_on, c)) {
                     /* Auto-disabled — clean up */
                     send_always_on_state(c->wsi, "disabled");
                     always_on_destroy(c->always_on);
                     c->always_on = NULL;
                  }
               }
            }
            pthread_mutex_unlock(&s_conn_registry_mutex);
         }
      }
   }

   OLOG_INFO("WebUI: Server thread exiting");
   return NULL;
}

/* =============================================================================
 * Tool Execution Callback (for debug display)
 * ============================================================================= */

/**
 * @brief Callback for native tool execution notifications
 *
 * Sends tool call/result information to the WebUI for debug display.
 * Called by llm_tools module when tools are executed.
 */
/**
 * @brief Send LLM state update to WebSocket client
 *
 * Pushes the current LLM configuration to the client so it can update
 * the UI controls dynamically (e.g., after switch_llm tool call).
 */
static void webui_send_llm_state_update(session_t *session) {
   if (!session || session->type != SESSION_TYPE_WEBUI) {
      return;
   }

   /* Get session's current LLM config */
   session_llm_config_t config;
   session_get_llm_config(session, &config);

   /* Build JSON response */
   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("llm_state_update"));

   json_object *payload = json_object_new_object();
   json_object_object_add(payload, "success", json_object_new_boolean(1));

   const char *type_str = config.type == LLM_LOCAL ? "local" : "cloud";

   json_object_object_add(payload, "type", json_object_new_string(type_str));
   json_object_object_add(payload, "provider",
                          json_object_new_string(cloud_provider_to_string(config.cloud_provider)));
   json_object_object_add(payload, "model", json_object_new_string(config.model));
   json_object_object_add(payload, "openai_available",
                          json_object_new_boolean(llm_has_openai_key()));
   json_object_object_add(payload, "claude_available",
                          json_object_new_boolean(llm_has_claude_key()));
   json_object_object_add(payload, "gemini_available",
                          json_object_new_boolean(llm_has_gemini_key()));

   json_object_object_add(response, "payload", payload);

   /* Queue response for WebSocket send */
   ws_response_t resp = { .session = session,
                          .type = WS_RESP_TRANSCRIPT,
                          .transcript = { .role = strdup("__llm_state__"),
                                          .text = strdup(json_object_to_json_string(response)) } };

   json_object_put(response);

   if (resp.transcript.role && resp.transcript.text) {
      queue_response(&resp);
   } else {
      free(resp.transcript.role);
      free(resp.transcript.text);
   }
}

/**
 * @brief Build JSON array of active tools for a session
 * @param session Session to query (tools_mutex must NOT be held by caller)
 * @return Allocated JSON string like "[{\"name\":\"weather\",\"status\":\"running\"}]"
 *         or NULL if no active tools. Caller must free().
 */
static char *build_active_tools_json(session_t *session) {
   if (!session) {
      return NULL;
   }

   pthread_mutex_lock(&session->tools_mutex);
   if (session->active_tool_count == 0) {
      pthread_mutex_unlock(&session->tools_mutex);
      return NULL;
   }

   struct json_object *arr = json_object_new_array();
   for (int i = 0; i < session->active_tool_count; i++) {
      struct json_object *tool = json_object_new_object();
      json_object_object_add(tool, "name", json_object_new_string(session->active_tools[i]));
      json_object_object_add(tool, "status", json_object_new_string("running"));
      json_object_array_add(arr, tool);
   }
   pthread_mutex_unlock(&session->tools_mutex);

   const char *json_str = json_object_to_json_string(arr);
   char *result = strdup(json_str);
   json_object_put(arr);
   return result;
}

/**
 * @brief Add a tool to session's active tools list
 * @return true if added, false if already present or list full
 */
static bool session_add_active_tool(session_t *session, const char *tool_name) {
   if (!session || !tool_name) {
      return false;
   }

   pthread_mutex_lock(&session->tools_mutex);

   /* Check if already in list */
   for (int i = 0; i < session->active_tool_count; i++) {
      if (strcmp(session->active_tools[i], tool_name) == 0) {
         pthread_mutex_unlock(&session->tools_mutex);
         return false; /* Already tracking */
      }
   }

   /* Add if space available */
   if (session->active_tool_count < 8) {
      strncpy(session->active_tools[session->active_tool_count], tool_name, 31);
      session->active_tools[session->active_tool_count][31] = '\0';
      session->active_tool_count++;
      pthread_mutex_unlock(&session->tools_mutex);
      return true;
   }

   pthread_mutex_unlock(&session->tools_mutex);
   return false;
}

/**
 * @brief Remove a tool from session's active tools list
 * @return true if removed, false if not found
 */
static bool session_remove_active_tool(session_t *session, const char *tool_name) {
   if (!session || !tool_name) {
      return false;
   }

   pthread_mutex_lock(&session->tools_mutex);

   for (int i = 0; i < session->active_tool_count; i++) {
      if (strcmp(session->active_tools[i], tool_name) == 0) {
         /* Shift remaining tools down */
         for (int j = i; j < session->active_tool_count - 1; j++) {
            strcpy(session->active_tools[j], session->active_tools[j + 1]);
         }
         session->active_tool_count--;
         pthread_mutex_unlock(&session->tools_mutex);
         return true;
      }
   }

   pthread_mutex_unlock(&session->tools_mutex);
   return false;
}

/**
 * @brief Send state update with current active tools
 */
static void send_state_with_tools(session_t *session, const char *state) {
   if (!session || session->type != SESSION_TYPE_WEBUI) {
      return;
   }

   char *tools_json = build_active_tools_json(session);

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_STATE,
                          .state = {
                              .state = strdup(state),
                              .detail = NULL,
                              .tools_json = tools_json, /* Takes ownership */
                              .conversation_id = session->stream_conversation_id,
                          } };

   if (!resp.state.state) {
      OLOG_ERROR("WebUI: Failed to allocate state response");
      free(tools_json);
      return;
   }

   queue_response(&resp);
}

/**
 * @brief Build a descriptive display name for a tool based on its arguments
 *
 * For tools that may run in parallel with different parameters (e.g., search),
 * this extracts a qualifier to distinguish them in the UI.
 *
 * @param tool_name Base tool name (e.g., "search", "weather")
 * @param tool_args JSON arguments string
 * @param out Output buffer for display name
 * @param out_size Size of output buffer
 */
static void build_tool_display_name(const char *tool_name,
                                    const char *tool_args,
                                    char *out,
                                    size_t out_size) {
   if (!tool_name || !out || out_size == 0) {
      return;
   }

   /* Default to just the tool name */
   strncpy(out, tool_name, out_size - 1);
   out[out_size - 1] = '\0';

   if (!tool_args || tool_args[0] == '\0') {
      return;
   }

   /* Parse arguments to extract qualifier */
   struct json_object *args = json_tokener_parse(tool_args);
   if (!args) {
      return;
   }

   const char *qualifier = NULL;
   struct json_object *val;
   char domain_buf[32]; /* Buffer for URL domain extraction (function scope for safety) */

   /* Tool-specific qualifier extraction */
   if (strcmp(tool_name, "search") == 0) {
      /* For search: use the category (news, social, science, etc.).  The arg
       * key is "action" (the search tool's action-mapped selector); accept the
       * legacy "category" key too for any in-flight/historical tool calls. */
      if (json_object_object_get_ex(args, "action", &val) ||
          json_object_object_get_ex(args, "category", &val)) {
         qualifier = json_object_get_string(val);
      }
   } else if (strcmp(tool_name, "weather") == 0) {
      /* For weather: use location (truncated) */
      if (json_object_object_get_ex(args, "location", &val)) {
         qualifier = json_object_get_string(val);
      }
   } else if (strcmp(tool_name, "url") == 0) {
      /* For url: use domain extracted from URL (uses shared extract_url_host) */
      if (json_object_object_get_ex(args, "url", &val)) {
         const char *url = json_object_get_string(val);
         if (url) {
            extract_url_host(url, domain_buf, sizeof(domain_buf));
            qualifier = domain_buf;
         }
      }
   }

   /* Build display name with qualifier if found */
   if (qualifier && qualifier[0] != '\0') {
      /* Truncate qualifier if too long */
      char short_qual[16];
      strncpy(short_qual, qualifier, sizeof(short_qual) - 1);
      short_qual[sizeof(short_qual) - 1] = '\0';
      snprintf(out, out_size, "%s:%s", tool_name, short_qual);
   }

   json_object_put(args);
}

static void webui_tool_execution_callback(void *session_ptr,
                                          const char *tool_name,
                                          const char *tool_args,
                                          const char *result,
                                          bool success) {
   session_t *session = (session_t *)session_ptr;
   if (!session || (session->type != SESSION_TYPE_WEBUI && session->type != SESSION_TYPE_JOB)) {
      return;
   }

   /* Build descriptive display name (e.g., "search:news" instead of just "search") */
   char display_name[48];
   build_tool_display_name(tool_name, tool_args, display_name, sizeof(display_name));

   /* result==NULL indicates tool execution is starting, not ending */
   if (result == NULL) {
      /* Tool is starting - add to active list and send state with tools array */
      session_add_active_tool(session, display_name);
      send_state_with_tools(session, "thinking");
      return;
   }

   /* Tool execution completed - remove from active list */
   session_remove_active_tool(session, display_name);

   /* If the tool result contains a <dawn-visual> tag, send the full result
    * as a visible transcript entry (not debug-only). The WebUI visual renderer
    * extracts and renders the tag as a sandboxed iframe. The result may exceed
    * the debug_msg buffer size (8KB), so we send the raw result directly. */
   if (success && result && strstr(result, "<dawn-visual") != NULL) {
      webui_send_transcript(session, "visual", result);

      /* Stash visual content on the session so it gets appended to the
       * assistant message when saved to the conversation DB. This enables
       * visual replay when the conversation is reloaded.
       * Protected by tools_mutex — consumed by handle_save_message. */
      pthread_mutex_lock(&session->tools_mutex);
      free(session->pending_visual);
      session->pending_visual = strdup(result);
      pthread_mutex_unlock(&session->tools_mutex);
   }

   /* Format as debug entry for transcript - use "tool" role (not "assistant")
    * to avoid confusion with actual LLM responses and ensure proper JS routing */
   char debug_msg[LLM_TOOLS_RESULT_LEN + 256];
   snprintf(debug_msg, sizeof(debug_msg), "[Tool Call: %s(%s) -> %s%s]", tool_name,
            tool_args ? tool_args : "", success ? "" : "FAILED: ", result ? result : "(null)");
   webui_send_transcript(session, "tool", debug_msg);

   /* Send updated state (may still have other active tools) */
   pthread_mutex_lock(&session->tools_mutex);
   int remaining = session->active_tool_count;
   pthread_mutex_unlock(&session->tools_mutex);

   if (remaining > 0) {
      /* More tools running - update state with remaining tools */
      send_state_with_tools(session, "thinking");
   }
   /* Note: Don't send idle here - let the LLM flow handle that */

   /* If this was a switch_llm call, send LLM state update to client */
   if (success && strcmp(tool_name, "switch_llm") == 0) {
      webui_send_llm_state_update(session);
   }
}

/* =============================================================================
 * Public API
 * ============================================================================= */

/* Route libwebsockets' own diagnostics into DAWN's log instead of the default
 * stderr emitter, so connection-close reasons (abnormal 1006 closes, TLS
 * errors, internal timeouts, HANGUPs) are captured in dawn.log with timestamps
 * rather than vanishing. Called by lws on the service thread; OLOG is
 * thread-safe. lws appends a trailing newline we strip. */
static void webui_lws_log_emit(int level, const char *line) {
   if (!line)
      return;
   char buf[512];
   size_t n = strlen(line);
   if (n > sizeof(buf) - 1)
      n = sizeof(buf) - 1;
   memcpy(buf, line, n);
   while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
      n--;
   buf[n] = '\0';
   if (level & LLL_ERR) {
      OLOG_ERROR("lws: %s", buf);
   } else if (level & LLL_WARN) {
      OLOG_WARNING("lws: %s", buf);
   } else {
      OLOG_INFO("lws: %s", buf);
   }
}

int webui_server_init(int port, const char *www_path) {
   struct lws_context_creation_info info;

   pthread_mutex_lock(&s_mutex);
   if (s_running) {
      pthread_mutex_unlock(&s_mutex);
      OLOG_WARNING("WebUI: Server already running");
      return WEBUI_ERROR_ALREADY_RUNNING;
   }
   pthread_mutex_unlock(&s_mutex);

   /* Determine port */
   if (port <= 0) {
      port = g_config.webui.port;
      if (port <= 0) {
         port = WEBUI_DEFAULT_PORT;
      }
   }

   /* Determine www path */
   if (www_path && www_path[0] != '\0') {
      strncpy(s_www_path, www_path, sizeof(s_www_path) - 1);
   } else if (g_config.webui.www_path[0] != '\0') {
      strncpy(s_www_path, g_config.webui.www_path, sizeof(s_www_path) - 1);
   } else {
      strncpy(s_www_path, WEBUI_DEFAULT_WWW_PATH, sizeof(s_www_path) - 1);
   }
   s_www_path[sizeof(s_www_path) - 1] = '\0';

   /* Configure libwebsockets context */
   memset(&info, 0, sizeof(info));
   info.port = port;
   info.protocols = s_protocols;
   info.gid = -1;
   info.uid = -1;
   /* Increase service buffer for large WebSocket messages (conversation history).
    * Default is ~4KB which causes OVERSIZED_PAYLOAD errors on HTTP/2 connections. */
   info.pt_serv_buf_size = 128 * 1024; /* 128KB - enough for large conversation loads */
#if LWS_LIBRARY_VERSION_NUMBER >= 4004000
   info.ws_ping_pong_interval = 0; /* Disabled: satellites use app-level pings instead */
#endif
   /* Note: Not using LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE
    * because it sets CSP: default-src 'none' which blocks WebAssembly for Opus codec.
    * Security headers are added manually via webui_add_security_headers() and
    * webui_get_static_security_headers() for lws_serve_http_file(). */
   info.options = 0;

   /* Initialize security headers string (uses g_config.webui.https) */
   webui_security_headers_init();
   /* Note: session_manager_set_user_prompt_builder() is called earlier in
    * dawn.c, before mosquitto_loop_start, to close the init-order race
    * where MQTT-triggered refreshes could observe a NULL builder. */

   /* Configure HTTPS if enabled */
   bool use_https = g_config.webui.https;
   if (use_https) {
      if (g_config.webui.ssl_cert_path[0] == '\0' || g_config.webui.ssl_key_path[0] == '\0') {
         OLOG_ERROR("WebUI: HTTPS enabled but ssl_cert_path or ssl_key_path not set");
         OLOG_ERROR("  Hint: Set ssl_cert_path and ssl_key_path in dawn.toml [webui], or run "
                    "./generate_ssl_cert.sh");
         return WEBUI_ERROR_SOCKET;
      }

      /* Verify certificate files exist */
      if (access(g_config.webui.ssl_cert_path, R_OK) != 0) {
         OLOG_ERROR("WebUI: Cannot read SSL certificate: %s", g_config.webui.ssl_cert_path);
         OLOG_ERROR("  Hint: Check file exists and permissions. Regenerate with "
                    "./generate_ssl_cert.sh --renew if expired");
         return WEBUI_ERROR_SOCKET;
      }
      if (access(g_config.webui.ssl_key_path, R_OK) != 0) {
         OLOG_ERROR("WebUI: Cannot read SSL private key: %s", g_config.webui.ssl_key_path);
         OLOG_ERROR("  Hint: Check file exists and permissions. Key should be readable by the DAWN "
                    "process user");
         return WEBUI_ERROR_SOCKET;
      }

      info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
      info.ssl_cert_filepath = g_config.webui.ssl_cert_path;
      info.ssl_private_key_filepath = g_config.webui.ssl_key_path;

      /* Force HTTP/1.1 only via ALPN to avoid HTTP/2 frame size limits (16KB).
       * WebSocket over HTTP/2 has stricter frame size requirements that cause
       * OVERSIZED_PAYLOAD errors with large conversation messages. */
      info.alpn = "http/1.1";

      OLOG_INFO("WebUI: HTTPS enabled with cert: %s (HTTP/1.1 only)", g_config.webui.ssl_cert_path);
   }

   OLOG_INFO("WebUI: Initializing %s server on port %d, serving from: %s",
             use_https ? "HTTPS" : "HTTP", port, s_www_path);

   /* Route lws error/warn/notice into DAWN's log (see webui_lws_log_emit).
    * NOTICE is included so connection-close reasons (which lws emits at NOTICE)
    * are captured for diagnosing abnormal 1006 disconnects; INFO/DEBUG stay off
    * to avoid the noisy per-connection lifecycle spam. */
   lws_set_log_level(LLL_ERR | LLL_WARN | LLL_NOTICE, webui_lws_log_emit);

   /* Create context */
   s_lws_context = lws_create_context(&info);
   if (!s_lws_context) {
      OLOG_ERROR("WebUI: Failed to create libwebsockets context");
      return WEBUI_ERROR_SOCKET;
   }

   s_port = port;
   s_running = 1;
   s_client_count = 0;

   /* Initialize audio subsystem (optional - continues if not available) */
#ifdef ENABLE_WEBUI_AUDIO
   if (webui_audio_init() != WEBUI_AUDIO_SUCCESS) {
      OLOG_WARNING("WebUI: Audio subsystem not available, voice input disabled");
   }
#endif

   /* Initialize music streaming subsystem (optional) */
   if (webui_music_init() != 0) {
      OLOG_WARNING("WebUI: Music streaming subsystem not available");
   }

   /* Register tool execution callback for debug display */
   llm_tools_set_execution_callback(webui_tool_execution_callback);

   /* Wire silent-observe listener BEFORE spawning the server thread.  This
    * avoids a startup-window race where another subsystem (scheduler, recovery
    * worker, etc.) emits a silent observation in the few microseconds between
    * pthread_create and the listener registration.  Pure pointer write under
    * a mutex — safe to do at any point after llm_silent_observe.c is linked. */
   llm_silent_observe_set_event_listener(webui_broadcast_silent_observation);

   /* Start server thread */
   if (pthread_create(&s_webui_thread, NULL, webui_thread_func, NULL) != 0) {
      OLOG_ERROR("WebUI: Failed to create server thread");
      llm_silent_observe_set_event_listener(NULL);
      webui_music_cleanup();
#ifdef ENABLE_WEBUI_AUDIO
      webui_audio_cleanup();
#endif
      lws_context_destroy(s_lws_context);
      s_lws_context = NULL;
      s_running = 0;
      return WEBUI_ERROR_THREAD;
   }

   OLOG_INFO("WebUI: Server started successfully on port %d", port);

   /* Warn if satellite registration is open (no pre-shared key configured) */
   const secrets_config_t *secrets = config_get_secrets();
   if (!secrets || !secrets->satellite_registration_key[0]) {
      OLOG_WARNING("WebUI: Satellite registration is OPEN (no registration key set)");
      OLOG_WARNING("WebUI: Any device on the network can register as a satellite.");
      OLOG_WARNING("WebUI: Generate a key with: ./generate_ssl_cert.sh --gen-key");
   } else {
      OLOG_INFO("WebUI: Satellite registration key is active");
   }

   return WEBUI_SUCCESS;
}

void webui_server_shutdown(void) {
   pthread_mutex_lock(&s_mutex);
   if (!s_running) {
      pthread_mutex_unlock(&s_mutex);
      return;
   }

   OLOG_INFO("WebUI: Shutting down server...");
   s_running = 0;
   pthread_mutex_unlock(&s_mutex);

   /* Tear down silent-observe listener so post-shutdown calls don't dispatch
    * into freed/closing connection state.  Pairs with the registration in
    * webui_server_init(). */
   llm_silent_observe_set_event_listener(NULL);

   /* Wait for satellite worker threads to finish (max 5 seconds) */
   {
      extern atomic_int g_active_satellite_workers;
      int wait_ms = 0;
      while (atomic_load(&g_active_satellite_workers) > 0 && wait_ms < 5000) {
         if (wait_ms == 0)
            OLOG_INFO("WebUI: Waiting for %d satellite workers to finish...",
                      atomic_load(&g_active_satellite_workers));
         usleep(50000); /* 50ms */
         wait_ms += 50;
      }
      int remaining = atomic_load(&g_active_satellite_workers);
      if (remaining > 0)
         OLOG_WARNING("WebUI: %d satellite workers still active after 5s timeout", remaining);
   }

   /* Wake up lws_service() to process shutdown */
   if (s_lws_context) {
      lws_cancel_service(s_lws_context);
   }

   /* Wait for thread to exit with timeout */
   OLOG_INFO("WebUI: Waiting for server thread to exit (max 2 seconds)...");
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_sec += 2;

   int join_result = pthread_timedjoin_np(s_webui_thread, NULL, &ts);
   if (join_result == ETIMEDOUT) {
      OLOG_WARNING("WebUI: Server thread did not exit in time, cancelling...");
      pthread_cancel(s_webui_thread);
      pthread_join(s_webui_thread, NULL);
      OLOG_INFO("WebUI: Server thread cancelled and joined");
   } else if (join_result != 0) {
      OLOG_ERROR("WebUI: pthread_timedjoin_np failed: %d", join_result);
   } else {
      OLOG_INFO("WebUI: Server thread exited cleanly");
   }

   /* Destroy context */
   if (s_lws_context) {
      lws_context_destroy(s_lws_context);
      s_lws_context = NULL;
   }

   /* Cleanup music streaming subsystem */
   webui_music_cleanup();

   /* Cleanup audio subsystem */
#ifdef ENABLE_WEBUI_AUDIO
   webui_audio_cleanup();
#endif

   s_port = 0;
   s_client_count = 0;

   OLOG_INFO("WebUI: Server shutdown complete");
}

bool webui_server_is_running(void) {
   bool running;
   pthread_mutex_lock(&s_mutex);
   running = s_running != 0;
   pthread_mutex_unlock(&s_mutex);
   return running;
}

int webui_server_client_count(void) {
   int count;
   pthread_mutex_lock(&s_mutex);
   count = s_client_count;
   pthread_mutex_unlock(&s_mutex);
   return count;
}

int webui_server_get_port(void) {
   return s_port;
}

int webui_get_queue_fill_pct(void) {
   pthread_mutex_lock(&s_queue_mutex);
   int count;
   if (s_queue_tail >= s_queue_head) {
      count = s_queue_tail - s_queue_head;
   } else {
      count = WEBUI_RESPONSE_QUEUE_SIZE - s_queue_head + s_queue_tail;
   }
   pthread_mutex_unlock(&s_queue_mutex);
   return (count * 100) / WEBUI_RESPONSE_QUEUE_SIZE;
}

/* webui_clear_login_rate_limit moved to webui_http.c */

/* =============================================================================
 * Worker-Callable Response Functions (Thread-Safe)
 * ============================================================================= */

/**
 * @brief Send a transcript message to the WebUI client
 *
 * Valid roles and their WebUI behavior:
 *   - "user"      : User input, saved to conversation history, displayed normally
 *   - "assistant" : LLM response, saved to conversation history, displayed normally
 *   - "tool"      : Tool execution debug info, NOT saved to history, debug-only display
 *   - "system"    : System/error messages, displayed with error styling
 *   - "streaming" : Partial assistant response (handled separately by streaming code)
 *
 * The "tool" role is specifically for internal debug output (tool calls, results)
 * that should be visible in debug mode but not pollute conversation history.
 * This prevents LLM context pollution when debug output is replayed.
 *
 * @param session WebSocket session to send to
 * @param role    Message role (see above)
 * @param text    Message content
 */
void webui_send_transcript_ex(session_t *session,
                              const char *role,
                              const char *text,
                              bool server_saved) {
   if (!session || (session->type != SESSION_TYPE_WEBUI && session->type != SESSION_TYPE_JOB)) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_TRANSCRIPT,
                          .transcript = {
                              .role = strdup(role),
                              .text = strdup(text),
                              .server_saved = server_saved,
                              .conversation_id = session->stream_conversation_id,
                          } };

   if (!resp.transcript.role || !resp.transcript.text) {
      free(resp.transcript.role);
      free(resp.transcript.text);
      OLOG_ERROR("WebUI: Failed to allocate transcript response");
      return;
   }

   queue_response(&resp);
}

void webui_send_transcript(session_t *session, const char *role, const char *text) {
   webui_send_transcript_ex(session, role, text, false);
}

void webui_send_state_with_detail(session_t *session, const char *state, const char *detail) {
   if (!session || (session->type != SESSION_TYPE_WEBUI && session->type != SESSION_TYPE_DAP2)) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_STATE,
                          .state = {
                              .state = strdup(state),
                              .detail = detail ? strdup(detail) : NULL,
                              .conversation_id = session->stream_conversation_id,
                          } };

   if (!resp.state.state) {
      OLOG_ERROR("WebUI: Failed to allocate state response");
      return;
   }

   queue_response(&resp);
}

void webui_send_state(session_t *session, const char *state) {
   webui_send_state_with_detail(session, state, NULL);

   /* Also send metrics update with state change */
   int context_pct = -1; /* -1 = no data, JS will keep previous value */
   llm_context_usage_t usage;
   session_llm_config_t llm_cfg;
   session_get_llm_config(session, &llm_cfg);
   if (llm_context_get_usage(session->session_id, llm_cfg.type, llm_cfg.cloud_provider, NULL,
                             &usage) == 0 &&
       usage.max_tokens > 0) {
      context_pct = (int)((float)usage.current_tokens / (float)usage.max_tokens * 100.0f);
   }
   webui_send_metrics_update(session, state, 0, 0.0f, context_pct);
}

void webui_send_context(session_t *session, int current_tokens, int max_tokens, float threshold) {
   /* If session is NULL, broadcast to all WebSocket sessions */
   if (!session) {
      /* Get local session as default */
      session = session_get_local();
   }

   if (!session || session->type != SESSION_TYPE_WEBUI) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_CONTEXT,
                          .context = {
                              .current_tokens = current_tokens,
                              .max_tokens = max_tokens,
                              .threshold = threshold,
                          } };

   queue_response(&resp);
}

void webui_send_error(session_t *session, const char *code, const char *message) {
   if (!session || (session->type != SESSION_TYPE_WEBUI && session->type != SESSION_TYPE_DAP2)) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_ERROR,
                          .error = {
                              .code = strdup(code),
                              .message = strdup(message),
                          } };

   if (!resp.error.code || !resp.error.message) {
      free(resp.error.code);
      free(resp.error.message);
      OLOG_ERROR("WebUI: Failed to allocate error response");
      return;
   }

   queue_response(&resp);
}

void webui_send_compaction_complete(session_t *session,
                                    int tokens_before,
                                    int tokens_after,
                                    int messages_summarized,
                                    const char *summary,
                                    int level) {
   if (!session || session->type != SESSION_TYPE_WEBUI) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_COMPACTION_COMPLETE,
                          .compaction = {
                              .tokens_before = tokens_before,
                              .tokens_after = tokens_after,
                              .messages_summarized = messages_summarized,
                              .level = level,
                              .summary = summary ? strdup(summary) : NULL,
                          } };

   queue_response(&resp);
}

/**
 * @brief Queue audio data for sending to WebSocket client
 *
 * Copies the audio data to the response queue. The WebUI thread will
 * send it as binary WebSocket frames. Large audio is chunked to avoid
 * overwhelming libwebsockets with huge single writes.
 *
 * @param session WebSocket session
 * @param data PCM audio data
 * @param len Length of audio data
 */
#define AUDIO_CHUNK_SIZE \
   8192 /* 8KB chunks for WebSocket frames - keep small for lws compatibility */

void webui_send_audio(session_t *session, const uint8_t *data, size_t len) {
   if (!session || !data || len == 0) {
      return;
   }
   if (session->type != SESSION_TYPE_WEBUI && session->type != SESSION_TYPE_DAP2) {
      return;
   }

   /* Send audio in chunks to avoid overwhelming lws_write with huge buffers */
   size_t offset = 0;
   int chunk_num = 0;

   while (offset < len) {
      size_t chunk_len = len - offset;
      if (chunk_len > AUDIO_CHUNK_SIZE) {
         chunk_len = AUDIO_CHUNK_SIZE;
      }

      uint8_t *chunk_copy = malloc(chunk_len);
      if (!chunk_copy) {
         OLOG_ERROR("WebUI: Failed to allocate audio chunk buffer");
         return;
      }
      memcpy(chunk_copy, data + offset, chunk_len);

      ws_response_t resp = { .session = session,
                             .type = WS_RESP_AUDIO,
                             .audio = {
                                 .data = chunk_copy,
                                 .len = chunk_len,
                             } };

      queue_response(&resp);
      offset += chunk_len;
      chunk_num++;
   }
}

/**
 * @brief Queue end-of-audio marker for WebSocket client
 *
 * @param session WebSocket session
 */
void webui_send_audio_end(session_t *session, bool is_opus) {
   if (!session || (session->type != SESSION_TYPE_WEBUI && session->type != SESSION_TYPE_DAP2)) {
      return;
   }

   ws_response_t resp = { .session = session, .type = WS_RESP_AUDIO_END };
   resp.audio.is_opus = is_opus;

   queue_response(&resp);
}

/* =============================================================================
 * LLM Streaming Functions (ChatGPT-style real-time text)
 *
 * These functions provide real-time token streaming to WebUI clients.
 * Protocol:
 *   1. stream_start - Create new assistant entry, enter streaming state
 *   2. stream_delta - Append text to current entry (multiple calls)
 *   3. stream_end   - Finalize entry, exit streaming state
 *
 * Stream IDs prevent stale deltas from cancelled streams from being displayed.
 * ============================================================================= */

void webui_send_stream_start(session_t *session) {
   if (!session || (session->type != SESSION_TYPE_WEBUI && session->type != SESSION_TYPE_DAP2 &&
                    session->type != SESSION_TYPE_JOB)) {
      return;
   }

   /* Increment stream ID and mark streaming active */
   uint32_t sid = atomic_fetch_add(&session->current_stream_id, 1) + 1;
   atomic_store(&session->llm_streaming_active, true);

   /* The turn's conversation was captured at dispatch (before thinking frames);
    * read it here rather than re-reading the live view, which may have changed if
    * the client switched during thinking.  Publish the turn into the
    * conversation-keyed replay ring.  0 for non-WebUI (DAP2) — a harmless no-op. */
   int64_t conv_id = session->stream_conversation_id;
   conv_stream_begin(conv_id, sid);

   /* Reset command tag filter state for new stream */
   session->cmd_tag_filter.nesting_depth = 0;
   session->cmd_tag_filter.len = 0;

   /* Reset sentence spacing tracker */
   session->stream_last_char = '\0';

   /* Cache whether to bypass filtering (native tools enabled) */
   session->cmd_tag_filter_bypass = llm_tools_enabled(NULL);

   /* Transition to "speaking" state when streaming begins */
   webui_send_state(session, "speaking");

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_STREAM_START,
                          .stream = {
                              .stream_id = sid,
                              .conversation_id = conv_id,
                              .text = "",
                          } };

   queue_response(&resp);
   OLOG_INFO("WebUI: Stream start id=%u conv=%lld for session %u", sid, (long long)conv_id,
             session->session_id);
}

/* Command tag filter uses shared constants from core/text_filter.h */

/**
 * @brief Check if character is a sentence terminator
 */
static inline bool is_sentence_terminator(char c) {
   return c == '.' || c == '!' || c == '?' || c == ':';
}

/**
 * @brief Check and fix sentence spacing for streaming text
 *
 * LLM streaming sometimes omits spaces after sentence terminators.
 * This function detects when the previous chunk ended with a terminator
 * and the new chunk starts with a letter, prepending a space if needed.
 *
 * @param session Session with stream_last_char tracking
 * @param text Input text
 * @param out_buf Output buffer (can overlap with text if no space needed)
 * @param out_size Size of output buffer
 * @return Pointer to text to send (either out_buf with space, or original text)
 */
static const char *fix_sentence_spacing(session_t *session,
                                        const char *text,
                                        char *out_buf,
                                        size_t out_size) {
   if (!session || !text || !text[0] || !out_buf || out_size < 2) {
      return text;
   }

   /* Check if we need to add a space:
    * - Previous chunk ended with sentence terminator
    * - This chunk starts with a letter */
   char first = text[0];
   bool needs_space = is_sentence_terminator(session->stream_last_char) &&
                      ((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z'));

   if (needs_space) {
      /* Prepend space to the text */
      size_t text_len = strlen(text);
      if (text_len + 2 <= out_size) {
         out_buf[0] = ' ';
         memcpy(out_buf + 1, text, text_len + 1); /* Include null terminator */
         return out_buf;
      }
   }

   return text;
}

/**
 * @brief Update the last character tracker after sending text
 */
static inline void update_stream_last_char(session_t *session, const char *text) {
   if (session && text) {
      size_t len = strlen(text);
      if (len > 0) {
         session->stream_last_char = text[len - 1];
      }
   }
}

/**
 * @brief Output callback for WebUI streaming (adapter for text_filter API)
 *
 * This callback adapts the shared text_filter's signature to WebUI needs.
 * Session is passed via ctx parameter.
 */
/* True if [text, text+len) is empty or only whitespace.  Used to avoid opening a
 * streaming bubble for whitespace-only content (e.g. the "\n\n" tool-iteration flush
 * on a tool-only iteration), which would otherwise leave an empty bubble. */
static bool stream_text_is_all_whitespace(const char *text, size_t len) {
   for (size_t i = 0; i < len; i++) {
      if (!isspace((unsigned char)text[i])) {
         return false;
      }
   }
   return true;
}

static void webui_filter_output(const char *text, size_t len, void *ctx) {
   session_t *session = (session_t *)ctx;
   if (!session || !text || len == 0) {
      return;
   }

   /* Don't open a bubble for whitespace-only first content (iteration-boundary flush). */
   if (!session->llm_streaming_active && stream_text_is_all_whitespace(text, len)) {
      return;
   }

   /* Start stream on first content (lazy initialization) */
   if (!session->llm_streaming_active) {
      webui_send_stream_start(session);
   }

   /* Null-terminate the input for processing */
   char temp_buf[4096];
   size_t safe_len = len < sizeof(temp_buf) - 1 ? len : sizeof(temp_buf) - 1;
   memcpy(temp_buf, text, safe_len);
   temp_buf[safe_len] = '\0';

   /* Fix sentence spacing (LLM sometimes omits space after period) */
   char spaced_buf[4098]; /* Extra room for prepended space */
   const char *fixed_text = fix_sentence_spacing(session, temp_buf, spaced_buf, sizeof(spaced_buf));

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_STREAM_DELTA,
                          .stream = {
                              .stream_id = session->current_stream_id,
                              .conversation_id = session->stream_conversation_id,
                          } };

   snprintf(resp.stream.text, sizeof(resp.stream.text), "%s", fixed_text);

   session->stream_had_content = true;
   update_stream_last_char(session, resp.stream.text);
   conv_stream_append(session->stream_conversation_id, session->current_stream_id,
                      resp.stream.text);
   queue_response(&resp);
}

/**
 * @brief Filter command tags and return filtered text
 *
 * Public function for callers that need the filtered text (e.g., TTS).
 * Uses the same state machine as WebUI streaming.
 *
 * @param session Session with filter state
 * @param text Input text to filter
 * @param out_buf Output buffer for filtered text
 * @param out_size Size of output buffer
 * @return Length of filtered text written to out_buf
 */
int webui_filter_command_tags(session_t *session,
                              const char *text,
                              char *out_buf,
                              size_t out_size) {
   if (!session || !text || !out_buf || out_size == 0) {
      return 0;
   }

   /* Native tools mode: no command tags to filter */
   if (session->cmd_tag_filter_bypass) {
      size_t len = strlen(text);
      size_t copy = len < out_size - 1 ? len : out_size - 1;
      memcpy(out_buf, text, copy);
      out_buf[copy] = '\0';
      return (int)copy;
   }

   /* Use shared text filter implementation */
   return text_filter_command_tags_to_buffer(&session->cmd_tag_filter, text, out_buf, out_size);
}

/**
 * @brief Send streaming text to WebUI with command tag filtering
 *
 * Filters <command>...</command> tags when in legacy mode (native tools disabled).
 * Automatically starts the stream on first content. Thread-safe per session.
 */
void webui_send_stream_delta(session_t *session, const char *text) {
   if (!session || (session->type != SESSION_TYPE_WEBUI && session->type != SESSION_TYPE_DAP2 &&
                    session->type != SESSION_TYPE_JOB)) {
      return;
   }

   if (!text || text[0] == '\0') {
      return;
   }

   /* If native tools are enabled, pass through without filtering */
   if (session->cmd_tag_filter_bypass) {
      /* Don't open a bubble for whitespace-only first content (iteration-boundary flush). */
      if (!session->llm_streaming_active && stream_text_is_all_whitespace(text, strlen(text))) {
         return;
      }
      if (!session->llm_streaming_active) {
         webui_send_stream_start(session);
      }

      /* Fix sentence spacing (LLM sometimes omits space after period) */
      char spaced_buf[4098];
      const char *fixed_text = fix_sentence_spacing(session, text, spaced_buf, sizeof(spaced_buf));

      ws_response_t resp = { .session = session,
                             .type = WS_RESP_STREAM_DELTA,
                             .stream = {
                                 .stream_id = session->current_stream_id,
                                 .conversation_id = session->stream_conversation_id,
                             } };
      snprintf(resp.stream.text, sizeof(resp.stream.text), "%s", fixed_text);
      session->stream_had_content = true;
      update_stream_last_char(session, resp.stream.text);
      /* Accumulate into the replay ring so a client attaching mid-turn (or the
       * one that switched away) can replay the partial. */
      conv_stream_append(session->stream_conversation_id, session->current_stream_id,
                         resp.stream.text);
      queue_response(&resp);
      return;
   }

   /* Legacy command tag mode: filter using shared state machine */
   text_filter_command_tags(&session->cmd_tag_filter, text, webui_filter_output, session);
}

void webui_send_stream_end(session_t *session, const char *reason) {
   if (!session || (session->type != SESSION_TYPE_WEBUI && session->type != SESSION_TYPE_DAP2 &&
                    session->type != SESSION_TYPE_JOB)) {
      return;
   }

   /* Mark streaming inactive */
   atomic_store(&session->llm_streaming_active, false);

   /* Finalize the replay-ring partial (kept for a short grace window). */
   conv_stream_end(session->stream_conversation_id, atomic_load(&session->current_stream_id));

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_STREAM_END,
                          .stream = {
                              .stream_id = atomic_load(&session->current_stream_id),
                              .conversation_id = session->stream_conversation_id,
                          } };

   /* Copy reason into fixed buffer (no malloc/free churn) */
   const char *r = reason ? reason : "complete";
   strncpy(resp.stream.text, r, sizeof(resp.stream.text) - 1);
   resp.stream.text[sizeof(resp.stream.text) - 1] = '\0';

   queue_response(&resp);
   OLOG_INFO("WebUI: Stream end id=%u reason=%s for session %u", session->current_stream_id, r,
             session->session_id);
}

/* =============================================================================
 * Extended Thinking Public API
 * ============================================================================= */

void webui_send_thinking_start(session_t *session, const char *provider) {
   if (!session || (session->type != SESSION_TYPE_WEBUI && session->type != SESSION_TYPE_JOB)) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_THINKING_START,
                          .stream = {
                              .stream_id = session->current_stream_id,
                              .conversation_id = session->stream_conversation_id,
                          } };

   /* Store provider name in text buffer */
   const char *p = provider ? provider : "unknown";
   strncpy(resp.stream.text, p, sizeof(resp.stream.text) - 1);
   resp.stream.text[sizeof(resp.stream.text) - 1] = '\0';

   queue_response(&resp);
   OLOG_INFO("WebUI: Thinking start id=%u provider=%s for session %u", session->current_stream_id,
             p, session->session_id);
}

void webui_send_thinking_delta(session_t *session, const char *text) {
   if (!session || (session->type != SESSION_TYPE_WEBUI && session->type != SESSION_TYPE_JOB)) {
      return;
   }

   if (!text || text[0] == '\0') {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_THINKING_DELTA,
                          .stream = {
                              .stream_id = session->current_stream_id,
                              .conversation_id = session->stream_conversation_id,
                          } };

   strncpy(resp.stream.text, text, sizeof(resp.stream.text) - 1);
   resp.stream.text[sizeof(resp.stream.text) - 1] = '\0';

   queue_response(&resp);
}

void webui_send_thinking_end(session_t *session, bool has_content) {
   if (!session || (session->type != SESSION_TYPE_WEBUI && session->type != SESSION_TYPE_JOB)) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_THINKING_END,
                          .stream = {
                              .stream_id = session->current_stream_id,
                              .conversation_id = session->stream_conversation_id,
                          } };

   /* Use text[0] to store has_content flag */
   resp.stream.text[0] = has_content ? '1' : '0';
   resp.stream.text[1] = '\0';

   queue_response(&resp);
   OLOG_INFO("WebUI: Thinking end id=%u has_content=%s for session %u", session->current_stream_id,
             has_content ? "true" : "false", session->session_id);
}

void webui_send_reasoning_summary(session_t *session, int reasoning_tokens) {
   if (!session || (session->type != SESSION_TYPE_WEBUI && session->type != SESSION_TYPE_JOB)) {
      return;
   }

   if (reasoning_tokens <= 0) {
      return;
   }

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_REASONING_SUMMARY,
                          .stream = {
                              .stream_id = session->current_stream_id,
                              .conversation_id = session->stream_conversation_id,
                          } };

   /* Store reasoning_tokens as string in text buffer */
   snprintf(resp.stream.text, sizeof(resp.stream.text), "%d", reasoning_tokens);

   queue_response(&resp);
   OLOG_INFO("WebUI: Reasoning summary id=%u tokens=%d for session %u", session->current_stream_id,
             reasoning_tokens, session->session_id);
}

void webui_fanout_job_stream_response(ws_response_t *resp) {
   if (!resp) {
      return;
   }
   /* queue_response never enqueues a job-session frame, so this fan-out OWNS it and
    * must free_response() it exactly once (a no-op for the inline stream/thinking
    * types, correct for any heap-carrying type — transcript, state, …).
    *
    * Frames a viewer should see, mirroring a normal turn:
    *  - stream/thinking/reasoning: inline text[], carry conversation_id — heap-free copy.
    *  - transcript ("tool"/"visual"): the native-tool debug + visual entries, same path
    *    a WEBUI turn uses; heap role/text, so each recipient gets its own strdup.
    * Any other type (e.g. the tools-running state pill) is dropped but still freed.
    * conversation_id lives in a different union member per type — read it per type. */
   int64_t conv_id;
   switch (resp->type) {
      case WS_RESP_STREAM_START:
      case WS_RESP_STREAM_DELTA:
      case WS_RESP_STREAM_END:
      case WS_RESP_THINKING_START:
      case WS_RESP_THINKING_DELTA:
      case WS_RESP_THINKING_END:
      case WS_RESP_REASONING_SUMMARY:
         conv_id = resp->stream.conversation_id;
         break;
      case WS_RESP_TRANSCRIPT:
         conv_id = resp->transcript.conversation_id;
         break;
      default:
         free_response(resp);
         return;
   }
   int user_id = resp->session ? (int)resp->session->metrics.user_id : 0;
   if (conv_id <= 0 || user_id <= 0) {
      free_response(resp);
      return;
   }

   /* conn_registry -> response-queue lock order, matching broadcast_json_to_user_ex.
    * The copies target real WEBUI sessions, so queue_response takes its ordinary
    * path (no recursion back into this fan-out). */
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (!conn || !conn->session || !conn->authenticated || conn->is_satellite) {
         continue;
      }
      if (conn->auth_user_id != user_id || conn->session->type != SESSION_TYPE_WEBUI) {
         continue;
      }
      /* Read this connection's view directly — webui_get_active_conversation_id()
       * resolves via session->client_data, which collapses to a single connection
       * when two tabs share one session, mis-targeting the other tab. */
      if (conn->active_conversation_id != conv_id) {
         continue; /* not looking at this job — it'll replay from the ring on open */
      }
      ws_response_t copy = *resp;
      copy.session = conn->session; /* retarget from the fd-less job session to a viewer */
      if (resp->type == WS_RESP_TRANSCRIPT) {
         /* Heap frame — each recipient owns its own strings (freed by free_response
          * after send).  Skip this recipient on OOM rather than share a pointer. */
         copy.transcript.role = resp->transcript.role ? strdup(resp->transcript.role) : NULL;
         copy.transcript.text = resp->transcript.text ? strdup(resp->transcript.text) : NULL;
         if ((resp->transcript.role && !copy.transcript.role) ||
             (resp->transcript.text && !copy.transcript.text)) {
            free(copy.transcript.role);
            free(copy.transcript.text);
            continue;
         }
      }
      queue_response(&copy);
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);

   free_response(resp); /* fan-out owns the original; free its heap exactly once */
}

void webui_send_session_json(session_t *session, const char *json_str) {
   if (!session || session->type != SESSION_TYPE_WEBUI || !json_str) {
      return;
   }

   ws_response_t resp = { 0 };
   resp.session = session;
   resp.type = WS_RESP_JSON;
   resp.generic_json.json = strdup(json_str);
   if (!resp.generic_json.json) {
      OLOG_ERROR("WebUI: Failed to allocate JSON response string");
      return;
   }
   queue_response(&resp);
}

void webui_send_conversation_reset(session_t *session) {
   if (!session || session->type != SESSION_TYPE_WEBUI) {
      return;
   }

   ws_response_t resp = { .session = session, .type = WS_RESP_CONVERSATION_RESET };

   queue_response(&resp);
   OLOG_INFO("WebUI: Conversation reset notification for session %u", session->session_id);
}

/* =============================================================================
 * Session Detach (called before session destruction)
 * ============================================================================= */

void webui_detach_session(session_t *session) {
   if (!session)
      return;

   /* Clean up stale token registry entries for this session */
   unregister_tokens_for_session(session->session_id);

   int detached = 0;
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (!conn || conn->session != session)
         continue;

      /* Detach session pointer — the connection stays alive.
       * On the next message, webui_conn_create_session() will
       * transparently create a fresh session so the user can
       * keep working without logging in again.
       * Atomic store: this runs on the maintenance thread while
       * the LWS thread may read conn->session concurrently. */
      conn_set_session(conn, NULL);
      detached++;
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);

   if (detached > 0) {
      OLOG_INFO("WebUI: Detached %d connection(s) from expiring session %u", detached,
                session->session_id);

      /* Release the references that the connections held.
       * This unblocks session_destroy's ref_count wait. */
      for (int i = 0; i < detached; i++) {
         session_release(session);
      }
   }
}

/* =============================================================================
 * Plan Progress (session-targeted)
 * ============================================================================= */

int64_t webui_get_active_conversation_id(session_t *session) {
   if (!session || session->type != SESSION_TYPE_WEBUI || !session->client_data)
      return 0;
   ws_connection_t *conn = (ws_connection_t *)session->client_data;
   return conn->active_conversation_id;
}

void webui_broadcast_plan_progress(session_t *session, const char *json_str) {
   if (!session || session->type != SESSION_TYPE_WEBUI || !json_str)
      return;

   ws_response_t resp = { .session = session,
                          .type = WS_RESP_JSON,
                          .generic_json = { .json = strdup(json_str) } };
   if (!resp.generic_json.json)
      return;

   queue_response(&resp);
}

/* =============================================================================
 * Real-Time Metrics for UI Visualization
 *
 * Provides metrics for multi-ring visualization. Sent on:
 * - State changes (immediate)
 * - Token chunk events (during streaming)
 * - Periodic heartbeat (1Hz when idle)
 * ============================================================================= */

void webui_send_metrics_update(session_t *session,
                               const char *state,
                               int ttft_ms,
                               float token_rate,
                               int context_percent) {
   if (!session || session->type != SESSION_TYPE_WEBUI) {
      return;
   }

   ws_response_t resp = { .session = session, .type = WS_RESP_METRICS_UPDATE };

   /* Copy state into fixed buffer */
   const char *s = state ? state : "idle";
   strncpy(resp.metrics.state, s, sizeof(resp.metrics.state) - 1);
   resp.metrics.state[sizeof(resp.metrics.state) - 1] = '\0';

   resp.metrics.ttft_ms = ttft_ms;
   resp.metrics.token_rate = token_rate;
   resp.metrics.context_pct = context_percent;
   resp.metrics.conversation_id = atomic_load(&session->stream_conversation_id);

   queue_response(&resp);
}


/* =============================================================================
 * Force Logout (for session revocation)
 *
 * Finds all WebSocket connections with matching auth_session_token prefix
 * and sends them a force_logout message.
 * ============================================================================= */

int webui_force_logout_by_auth_token(const char *auth_token_prefix) {
   if (!auth_token_prefix || strlen(auth_token_prefix) < AUTH_TOKEN_PREFIX_LEN) {
      return 0;
   }

   int count = 0;
   pthread_mutex_lock(&s_conn_registry_mutex);

   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (conn && conn->wsi && conn->authenticated) {
         /* Check if auth token prefix matches */
         if (strncmp(conn->auth_session_token, auth_token_prefix, AUTH_TOKEN_PREFIX_LEN) == 0) {
            OLOG_INFO("WebUI: Forcing logout for connection with auth token %.4s...",
                      auth_token_prefix);
            send_force_logout_impl(conn->wsi, "Session revoked");
            /* Mark as unauthenticated to prevent further requests */
            conn->authenticated = false;
            count++;
         }
      }
   }

   pthread_mutex_unlock(&s_conn_registry_mutex);

   if (count > 0) {
      OLOG_INFO("WebUI: Sent force_logout to %d connection(s)", count);
   }

   return count;
}

int webui_destroy_sessions_by_auth_token(const char *auth_token_prefix) {
   if (!auth_token_prefix || strlen(auth_token_prefix) < AUTH_TOKEN_PREFIX_LEN)
      return 0;

   /* Collect matching session IDs under the conn mutex, then destroy after
    * releasing (session_destroy blocks on ref_count and itself calls
    * webui_detach_session → s_conn_registry_mutex; recursive would deadlock). */
   uint32_t ids[MAX_ACTIVE_CONNECTIONS];
   int n = 0;

   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (!conn || !conn->session)
         continue;
      if (strncmp(conn->auth_session_token, auth_token_prefix, AUTH_TOKEN_PREFIX_LEN) != 0)
         continue;
      /* Skip satellites — their session lifecycle is managed by the satellite
       * protocol, not the auth cookie. */
      if (conn->is_satellite)
         continue;
      uint32_t sid = conn->session->session_id;
      bool dup = false;
      for (int j = 0; j < n; j++) {
         if (ids[j] == sid) {
            dup = true;
            break;
         }
      }
      if (!dup && n < MAX_ACTIVE_CONNECTIONS)
         ids[n++] = sid;
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);

   for (int i = 0; i < n; i++) {
      session_destroy(ids[i]);
   }

   if (n > 0)
      OLOG_INFO("WebUI: Destroyed %d session slot(s) for auth token %.4s...", n, auth_token_prefix);

   return n;
}


/* =============================================================================
 * Connection Iterator (for per-user broadcasting)
 * ============================================================================= */

void webui_for_each_conn_by_user(int user_id,
                                 void (*callback)(ws_connection_t *conn, void *ctx),
                                 void *ctx) {
   if (user_id <= 0 || !callback) {
      return;
   }

   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (conn && conn->authenticated && conn->auth_user_id == user_id && conn->music_state) {
         callback(conn, ctx);
      }
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);
}

int webui_collect_conns_by_user(int user_id, ws_connection_t **out, int max_out) {
   if (user_id <= 0 || !out || max_out <= 0) {
      return 0;
   }

   int count = 0;
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (conn && conn->authenticated && conn->auth_user_id == user_id && conn->music_state) {
         if (count < max_out) {
            out[count] = conn;
         }
         count++;
      }
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);
   return count;
}

/* =============================================================================
 * Satellite Connection Helpers
 * ============================================================================= */

bool webui_is_satellite_online(const char *uuid) {
   if (!uuid)
      return false;

   bool online = false;
   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (conn && conn->is_satellite && conn->session &&
          strcmp(conn->session->identity.uuid, uuid) == 0) {
         online = true;
         break;
      }
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);
   return online;
}

/* NOTE: Must be called from the LWS service thread (admin handlers run within
 * LWS callbacks, so current callers are safe). Do NOT call from worker threads. */
void webui_force_disconnect_satellite(const char *uuid) {
   if (!uuid)
      return;

   pthread_mutex_lock(&s_conn_registry_mutex);
   for (int i = 0; i < MAX_ACTIVE_CONNECTIONS; i++) {
      ws_connection_t *conn = s_active_connections[i];
      if (conn && conn->is_satellite && conn->session && conn->wsi &&
          strcmp(conn->session->identity.uuid, uuid) == 0) {
         OLOG_INFO("WebUI: Force-disconnecting satellite %s (disabled by admin)", uuid);
         lws_close_reason(conn->wsi, LWS_CLOSE_STATUS_POLICY_VIOLATION, (unsigned char *)"disabled",
                          8);
         lws_set_timeout(conn->wsi, PENDING_TIMEOUT_CLOSE_SEND, 3);
         break;
      }
   }
   pthread_mutex_unlock(&s_conn_registry_mutex);
}

/* =============================================================================
 * JSON Message Handler Implementation
 * ============================================================================= */

/* Skip leading ASCII whitespace; returns the first non-whitespace char. */
static const char *restore_skip_ws(const char *s) {
   while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
      s++;
   }
   return s;
}

/* If @p s begins with "<dawn:" then (optional whitespace) @p kw, return a pointer just past
 * @p kw; else NULL.  Whitespace-tolerant so a malformed imitated marker ("<dawn: reasoning")
 * still matches. */
static const char *restore_match_dawn_open(const char *s, const char *kw) {
   static const char prefix[] = "<dawn:";
   if (strncmp(s, prefix, sizeof(prefix) - 1) != 0) {
      return NULL;
   }
   const char *p = restore_skip_ws(s + sizeof(prefix) - 1);
   size_t klen = strlen(kw);
   return (strncmp(p, kw, klen) == 0) ? p + klen : NULL;
}

/* Find the position just past the next "</dawn:" (ws?) "thinking" (ws?) ">" close tag, or
 * NULL if none.  Whitespace-tolerant to match the imitated-marker variants. */
static const char *restore_find_dawn_close_thinking(const char *s) {
   for (const char *c = strstr(s, "</dawn:"); c; c = strstr(c + 1, "</dawn:")) {
      const char *p = restore_skip_ws(c + 7); /* strlen("</dawn:") */
      if (strncmp(p, "thinking", 8) != 0) {
         continue;
      }
      p = restore_skip_ws(p + 8);
      if (*p == '>') {
         return p + 1;
      }
   }
   return NULL;
}

/*
 * Strip ONLY leading legacy display markers from assistant content before it enters the
 * session's LLM-facing history.  The pre-E3 client persistence path prepended
 * "<dawn:reasoning .../>" and "<dawn:thinking ...>...</dawn:thinking>" blocks to assistant
 * content; they are a DISPLAY artifact (the reload path extracts them into a panel) and must
 * never reach the LLM — a reasoning model restored onto such a conversation imitates the
 * marker format in its own output (observed 2026-06-04 after a provider switch, where the
 * model emitted a fabricated "<dawn: reasoning tokens=...">).
 *
 * Scope is deliberately tight: only the LEADING marker block(s) (the legacy prepend
 * position), and the caller applies this to ASSISTANT messages only.  A mid-message mention
 * of these tags (e.g. discussing DAWN's code) is left untouched.  Returns a newly-allocated
 * cleaned copy, or NULL if nothing was stripped (caller keeps the original pointer).
 */
static char *restore_strip_leading_dawn_markers(const char *content) {
   if (!content) {
      return NULL;
   }
   const char *p = content;
   for (;;) {
      const char *q = restore_skip_ws(p);
      if (restore_match_dawn_open(q, "reasoning")) {
         /* Self-closing tag: advance past its '>'. */
         const char *gt = strchr(q, '>');
         if (!gt) {
            break; /* malformed/unterminated — stop, keep the remainder intact */
         }
         p = gt + 1;
         continue;
      }
      if (restore_match_dawn_open(q, "thinking")) {
         const char *close = restore_find_dawn_close_thinking(q);
         if (!close) {
            break; /* unterminated block — stop, don't eat the real answer */
         }
         p = close;
         continue;
      }
      break;
   }
   if (p == content) {
      return NULL; /* no leading markers */
   }
   return strdup(restore_skip_ws(p)); /* trim the blank line before the real answer */
}

/**
 * @brief Message callback for session context restoration.
 * Builds JSON objects with role + content for iterating into session_add_message.
 */
static int webui_session_restore_msg_cb(const conversation_message_t *msg, void *context) {
   json_object *arr = (json_object *)context;
   json_object *obj = json_object_new_object();
   json_object_object_add(obj, "role", json_object_new_string(msg->role));
   json_object_object_add(obj, "content", json_object_new_string(msg->content ? msg->content : ""));
   /* Carry structured tool fields so the restore loop can rebuild OpenAI-canonical
    * tool messages (assistant tool_calls / role:tool) for the LLM (E2). */
   if (msg->tool_calls && msg->tool_calls[0]) {
      json_object *tc = json_tokener_parse(msg->tool_calls);
      if (tc) {
         json_object_object_add(obj, "tool_calls", tc);
      }
   }
   if (msg->tool_call_id && msg->tool_call_id[0]) {
      json_object_object_add(obj, "tool_call_id", json_object_new_string(msg->tool_call_id));
   }
   json_object_array_add(arr, obj);
   return 0;
}

/**
 * @brief Restore conversation context into a session from DB
 *
 * Shared implementation used by both session expiry recovery and sidebar load.
 */
int webui_restore_conversation_context(ws_connection_t *conn,
                                       const conversation_t *conv,
                                       int64_t conv_id,
                                       json_object *preloaded_msgs) {
   json_object *all_msgs = NULL;
   bool owns_msgs = false;

   if (preloaded_msgs) {
      all_msgs = preloaded_msgs;
   } else {
      all_msgs = json_object_new_array();
      owns_msgs = true;
      int rc;
      if (conv->context_watermark_msg_id > 0) {
         /* v67: bound restored context to messages after the compaction watermark;
          * the injected summary (below) stands in for the compacted prefix. The
          * full transcript is still shown in the UI (display load is unbounded). */
         rc = conv_db_get_messages_after(conv_id, conn->auth_user_id,
                                         conv->context_watermark_msg_id,
                                         webui_session_restore_msg_cb, all_msgs);
      } else {
         rc = conv_db_get_messages(conv_id, conn->auth_user_id, webui_session_restore_msg_cb,
                                   all_msgs);
      }
      if (rc != AUTH_DB_SUCCESS) {
         json_object_put(all_msgs);
         return LWS_CLOSE_CONNECTION;
      }
   }

   int count = json_object_array_length(all_msgs);

   /* Check if stored messages include a system prompt */
   bool has_system = false;
   if (count > 0) {
      json_object *first = json_object_array_get_idx(all_msgs, 0);
      json_object *role_obj;
      if (json_object_object_get_ex(first, "role", &role_obj)) {
         const char *role = json_object_get_string(role_obj);
         if (role && strcmp(role, "system") == 0)
            has_system = true;
      }
   }

   session_clear_history(conn->session);

   if (!has_system) {
      /* Phase 1f: SESSION_START builder boundary — clear dedup state
       * so the next PER_TURN admits all candidates fresh. */
      session_injected_set_clear(conn->session);
      char *prompt = session_manager_build_system_prompt_string(conn->auth_user_id);
      session_add_message(conn->session, "system", prompt ? prompt : get_remote_command_prompt());
      free(prompt);
   }

   if (conv->compaction_summary && strlen(conv->compaction_summary) > 0) {
      /* v67: prepend a reconstructed [COMPACTED ...] marker (when a summary node
       * exists) so the reloaded LLM keeps a context_expand handle to the
       * compacted originals — not just the summary text.
       *
       * ASSISTANT role, NOT system: session_update_system_messages rebuilds the
       * leading context into exactly two system messages (stable prefix + volatile
       * focus block) every turn and DROPS any other system message — so a
       * system-role summary never reaches the LLM. The live compaction marker
       * (llm_context.c) is an assistant message for the same reason; matching it
       * here makes the summary survive the per-turn rebuild. */
      char summary[CONV_SUMMARY_MAX];
      conv_db_format_compaction_context(conv_id, conv->compaction_summary, summary,
                                        sizeof(summary));
      session_add_message(conn->session, "assistant", summary);
   }

   for (int i = 0; i < count; i++) {
      json_object *msg = json_object_array_get_idx(all_msgs, i);
      json_object *role_obj, *content_obj;
      if (json_object_object_get_ex(msg, "role", &role_obj) &&
          json_object_object_get_ex(msg, "content", &content_obj)) {
         const char *role = json_object_get_string(role_obj);
         const char *content = json_object_get_string(content_obj);

         /* Strip leading legacy <dawn:reasoning/>/<dawn:thinking> display markers from
          * assistant content so the LLM never sees them (and stops imitating the format).
          * Assistant-only, leading-only — see restore_strip_leading_dawn_markers. */
         char *stripped_content = NULL;
         if (role && strcmp(role, "assistant") == 0) {
            stripped_content = restore_strip_leading_dawn_markers(content);
            if (stripped_content) {
               content = stripped_content;
            }
         }

         json_object *tc_obj, *tcid_obj;
         bool has_tc = json_object_object_get_ex(msg, "tool_calls", &tc_obj);
         bool has_tcid = json_object_object_get_ex(msg, "tool_call_id", &tcid_obj);
         if (has_tc || has_tcid) {
            /* Rebuild the OpenAI-canonical tool message in-memory so the LLM sees the
             * structured call/result on reload.  Orphans (a tool result whose call
             * didn't restore, etc.) are dropped later by filter_orphaned_tool_messages
             * at request-build time. */
            json_object *m = json_object_new_object();
            json_object_object_add(m, "role", json_object_new_string(role));
            json_object_object_add(m, "content", json_object_new_string(content ? content : ""));
            if (has_tc) {
               json_object_object_add(m, "tool_calls", json_object_get(tc_obj));
            }
            if (has_tcid) {
               json_object_object_add(m, "tool_call_id", json_object_get(tcid_obj));
            }
            session_add_message_multipart(conn->session, m);
         } else {
            /* Rehydrate [IMAGE:img_id] markers into LLM-faithful image_url content
             * (owner-checked); no-marker messages fall back to a plain text add. */
            webui_rehydrate_message_into_session(conn->session, conn->auth_user_id, role, content);
         }
         free(stripped_content);
      }
   }

   /* Restore LLM config from conversation DB (reflects last-used settings) */
   if (conv->llm_type[0] != '\0' || conv->tools_mode[0] != '\0') {
      session_llm_config_t cfg;
      session_get_llm_config(conn->session, &cfg);

      if (conv->llm_type[0] != '\0') {
         if (strcmp(conv->llm_type, "local") == 0)
            cfg.type = LLM_LOCAL;
         else if (strcmp(conv->llm_type, "cloud") == 0)
            cfg.type = LLM_CLOUD;
      }
      if (conv->cloud_provider[0] != '\0') {
         if (strcmp(conv->cloud_provider, "openai") == 0)
            cfg.cloud_provider = CLOUD_PROVIDER_OPENAI;
         else if (strcmp(conv->cloud_provider, "claude") == 0)
            cfg.cloud_provider = CLOUD_PROVIDER_CLAUDE;
         else if (strcmp(conv->cloud_provider, "gemini") == 0)
            cfg.cloud_provider = CLOUD_PROVIDER_GEMINI;
         else if (strcmp(conv->cloud_provider, "openrouter") == 0)
            cfg.cloud_provider = CLOUD_PROVIDER_OPENROUTER;
      }
      if (conv->model[0] != '\0') {
         strncpy(cfg.model, conv->model, sizeof(cfg.model) - 1);
         cfg.model[sizeof(cfg.model) - 1] = '\0';

         /* Infer provider from model name if not explicitly stored.  Skipped under
          * the OpenRouter gateway (model-name prefixes don't apply to "vendor/model"
          * OpenRouter IDs — the gateway override below forces OPENROUTER anyway). */
         if (conv->cloud_provider[0] == '\0' && !llm_openrouter_gateway_enabled()) {
            if (strncmp(conv->model, "gpt-", 4) == 0 || strncmp(conv->model, "o1-", 3) == 0 ||
                strncmp(conv->model, "o3-", 3) == 0) {
               cfg.cloud_provider = CLOUD_PROVIDER_OPENAI;
            } else if (strncmp(conv->model, "claude-", 7) == 0) {
               cfg.cloud_provider = CLOUD_PROVIDER_CLAUDE;
            } else if (strncmp(conv->model, "gemini-", 7) == 0) {
               cfg.cloud_provider = CLOUD_PROVIDER_GEMINI;
            }
         }
      }

      /* OpenRouter gateway is the single authority: a restored conversation always runs
       * through OpenRouter regardless of its stored/inferred provider.  Force the provider
       * enum; a stored bare model ID is remapped to the right vendor/model slug canonically
       * in llm_resolve_config at request time (the single choke point). */
      if (cfg.type == LLM_CLOUD && llm_openrouter_gateway_enabled()) {
         cfg.cloud_provider = CLOUD_PROVIDER_OPENROUTER;
      }
      if (conv->tools_mode[0] != '\0') {
         strncpy(cfg.tool_mode, conv->tools_mode, sizeof(cfg.tool_mode) - 1);
         cfg.tool_mode[sizeof(cfg.tool_mode) - 1] = '\0';
      }
      /* Fix #6: Restore thinking_mode from conversation DB */
      if (conv->thinking_mode[0] != '\0') {
         strncpy(cfg.thinking_mode, conv->thinking_mode, sizeof(cfg.thinking_mode) - 1);
         cfg.thinking_mode[sizeof(cfg.thinking_mode) - 1] = '\0';
      }
      session_set_llm_config(conn->session, &cfg);
   }

   if (owns_msgs)
      json_object_put(all_msgs);
   return count;
}

/**
 * @brief Create a new conversation session for an authenticated connection.
 *
 * Called when conn->session is NULL (e.g., after session expiry) but the user
 * is still authenticated. Creates a fresh session so the user can keep working
 * without logging in again. If the connection had an active conversation, its
 * messages and LLM config are silently restored from the DB.
 *
 * @return true if session was created, false on failure (error sent to client)
 */
static bool webui_conn_create_session(ws_connection_t *conn) {
   conn->session = session_create(SESSION_TYPE_WEBUI, -1);
   if (!conn->session) {
      send_error_impl(conn->wsi, "SESSION_LIMIT", "Maximum sessions reached");
      return false;
   }

   session_set_metrics_user(conn->session, conn->auth_user_id);
   conn->session->client_data = conn;

   /* Re-enable missed-notification replay on the new session. Any notifications
    * that arrived during the conversation-session-expired window would have
    * been queued as missed (no attached session), so re-delivery is needed. */
   conn->missed_notif_delivered = false;

   /* Generate and register a new session token */
   if (generate_session_token(conn->session_token) != 0) {
      OLOG_ERROR("WebUI: Failed to generate session token");
      session_destroy(conn->session->session_id);
      conn->session = NULL;
      return false;
   }
   register_token(conn->session_token, conn->session->session_id);

   OLOG_INFO("WebUI: Auto-created session %u for connection (user %d, token %.4s...)",
             conn->session->session_id, conn->auth_user_id, conn->session_token);

   /* Send new session token and init messages so the client updates seamlessly */
   queue_init_messages(conn, conn->session_token);

   /* Restore the active conversation's context into the new session.
    * Uses the shared helper (same logic as sidebar load, but without
    * sending a UI response — the browser still has the messages displayed). */
   bool restored = false;
   if (conn->active_conversation_id > 0) {
      conversation_t conv;
      int rc = conv_db_get(conn->active_conversation_id, conn->auth_user_id, &conv);
      if (rc == AUTH_DB_SUCCESS) {
         if (!conv.is_archived) {
            int count = webui_restore_conversation_context(conn, &conv,
                                                           conn->active_conversation_id, NULL);
            if (count >= 0) {
               restored = true;
               OLOG_INFO("WebUI: Restored conversation %lld (%d messages) into new session %u",
                         (long long)conn->active_conversation_id, count, conn->session->session_id);
            }
         }
         conv_free(&conv);
      }
   }

   /* If no conversation was restored, initialize with the user's system prompt */
   if (!restored) {
      char *prompt = session_manager_build_system_prompt_string(conn->auth_user_id);
      session_init_system_prompt(conn->session, prompt ? prompt : get_remote_command_prompt());
      free(prompt);
   }

   return true;
}

void handle_text_message(ws_connection_t *conn,
                         const char *text,
                         size_t len,
                         const char **vision_images,
                         const size_t *vision_image_sizes,
                         const char **vision_mimes,
                         int vision_image_count) {
   (void)len; /* Length already validated by caller */

   /* SECURITY: Require authentication for text processing */
   if (!conn_require_auth(conn)) {
      return; /* conn_require_auth already sent error */
   }

   /* Auto-create session if expired (user is still authenticated).
    * Atomic load: maintenance thread may have NULLed conn->session. */
   if (!conn_get_session(conn)) {
      OLOG_INFO("WebUI: Session expired for authenticated connection, creating new session");
      if (!webui_conn_create_session(conn)) {
         return;
      }
   }

   if (vision_image_count > 0) {
      size_t total_bytes = 0;
      for (int i = 0; i < vision_image_count; i++) {
         total_bytes += vision_image_sizes[i];
      }
      OLOG_INFO("WebUI: Text+Vision input from session %u: %s (%d images, %zu total bytes)",
                conn->session->session_id, text, vision_image_count, total_bytes);
   } else {
      OLOG_INFO("WebUI: Text input from session %u: %s", conn->session->session_id, text);
   }

   /* Typed input (not ASR): pass input_was_voice=false so the worker stamps the
    * flag right before dispatch and the prompt builder omits the ASR hint. */
   int ret = webui_process_text_input_with_vision(conn->session, text, vision_images,
                                                  vision_image_sizes, vision_mimes,
                                                  vision_image_count, /*input_was_voice=*/false);
   if (ret != 0) {
      send_error_impl(conn->wsi, "PROCESSING_ERROR", "Failed to process text input");
   }
}

/* Audio handlers: handle_binary_message, audio_worker_thread, webui_sentence_audio_callback
 * moved to webui_audio.c (declaration in webui_internal.h) */
