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
 * Session manager implementation for multi-client support.
 */

#include "core/session_manager.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "auth/auth_db.h"
#include "config/dawn_config.h"
#include "core/prompt_compose.h"
#include "core/turn_queue.h"
#include "dawn_error.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_interface.h"
#include "llm/llm_tools.h"
#include "logging.h"
#include "memory/memory_extraction.h"
#ifdef ENABLE_WEBUI
#include "webui/webui_server.h"
#endif

// =============================================================================
// Static Variables
// =============================================================================

static session_t *sessions[MAX_SESSIONS];
static pthread_rwlock_t session_manager_rwlock = PTHREAD_RWLOCK_INITIALIZER;
static _Atomic uint32_t next_session_id = 1;  // 0 is reserved for local session
// Job-pool resolver (background-jobs Phase 1): job_manager registers this so
// session_get()/_for_reconnect() can resolve a job session on interactive-array
// miss.  Read lock-free (set once at job_manager_init before any job exists).
static session_job_lookup_fn s_job_lookup_fn = NULL;
static bool initialized = false;

/* Weak seam: WebUI overrides this (webui_send.c) to scrub the response queue of a
 * session about to be freed, preventing a send-thread-vs-teardown use-after-free.
 * No-op without WebUI. */
__attribute__((weak)) void webui_send_purge_session(const session_t *s) {
   (void)s;
}

/**
 * Thread-local command context - allows device callbacks to access the current session
 *
 * EXPECTED CALLERS:
 *   - Main thread: for local voice commands (dawn.c parse_llm_response_for_commands)
 *   - MQTT thread: for WebUI/DAP commands (mosquitto_comms.c execute_command_for_worker)
 *
 * THREAD SAFETY:
 *   Each thread has its own copy of this pointer. Callers must:
 *   1. Hold a session reference (via session_get/session_retain) while context is set
 *   2. Set context before invoking callbacks
 *   3. Clear context (set to NULL) after callbacks complete
 *   4. Release session reference after clearing context
 *
 * CRITICAL ASSUMPTION:
 *   This works because all paths that set/use this variable execute in the same thread:
 *   - Local voice: main thread sets context, calls parse_llm_response_for_commands(),
 *     callbacks execute in main thread, context cleared
 *   - WebUI/DAP: MQTT on_message() callback sets context, executes device callback,
 *     clears context - all in the single MQTT callback thread
 *
 *   Mosquitto must NOT be configured with MOSQ_OPT_THREADED or multiple event loops,
 *   as this would cause context to be set in one thread and read in another.
 */
static __thread session_t *tl_command_context = NULL;

// =============================================================================
// Internal Helper Functions
// =============================================================================

/**
 * @brief Generate cryptographic random bytes as hex string
 *
 * Uses /dev/urandom for cryptographic randomness. Falls back to
 * time-based random if urandom is unavailable (with a warning).
 *
 * @param buf Output buffer (must be at least num_bytes*2 + 1)
 * @param num_bytes Number of random bytes (output is 2x this)
 */
static void generate_crypto_random_hex(char *buf, size_t num_bytes) {
   unsigned char bytes[32];
   if (num_bytes > sizeof(bytes)) {
      num_bytes = sizeof(bytes);
   }

   int fd = open("/dev/urandom", O_RDONLY);
   if (fd >= 0) {
      ssize_t n = read(fd, bytes, num_bytes);
      close(fd);
      if (n == (ssize_t)num_bytes) {
         for (size_t i = 0; i < num_bytes; i++) {
            sprintf(buf + i * 2, "%02x", bytes[i]);
         }
         return;
      }
   }

   /* Fail closed: refuse to generate a secret with weak randomness */
   OLOG_ERROR("Cannot generate secure session secret - /dev/urandom unavailable");
   buf[0] = '\0';
}

/**
 * @brief Constant-time comparison for fixed-length reconnect secrets (64 hex chars)
 *
 * Uses a known fixed length to avoid timing leaks from strlen().
 *
 * @param a First string (must be at least RECONNECT_SECRET_LEN chars)
 * @param b Second string (must be at least RECONNECT_SECRET_LEN chars)
 * @return true if strings are equal, false otherwise
 */
#define RECONNECT_SECRET_LEN 64 /* 32 bytes * 2 hex chars */

/* session_destroy ref-count wait ceiling.  teardown's cancel aborts a live
 * worker within ~1s; a hold past this means a wedged worker, so we leak the
 * session_t (recoverable) rather than free it under a still-running worker (UAF). */
#define SESSION_DESTROY_REF_WAIT_MAX_SEC 30

static bool constant_time_compare(const char *a, const char *b) {
   if (!a || !b) {
      return false;
   }

   volatile unsigned char result = 0;
   for (size_t i = 0; i < RECONNECT_SECRET_LEN; i++) {
      result |= (unsigned char)((unsigned char)a[i] ^ (unsigned char)b[i]);
   }

   return result == 0;
}

static session_t *session_alloc(void) {
   session_t *session = calloc(1, sizeof(session_t));
   if (!session) {
      OLOG_ERROR("Failed to allocate session");
      return NULL;
   }

   // Initialize mutexes
   if (pthread_mutex_init(&session->history_mutex, NULL) != 0) {
      OLOG_ERROR("Failed to init history_mutex");
      free(session);
      return NULL;
   }

   if (pthread_mutex_init(&session->fd_mutex, NULL) != 0) {
      OLOG_ERROR("Failed to init fd_mutex");
      pthread_mutex_destroy(&session->history_mutex);
      free(session);
      return NULL;
   }

   if (pthread_mutex_init(&session->ref_mutex, NULL) != 0) {
      OLOG_ERROR("Failed to init ref_mutex");
      pthread_mutex_destroy(&session->fd_mutex);
      pthread_mutex_destroy(&session->history_mutex);
      free(session);
      return NULL;
   }

   if (pthread_cond_init(&session->ref_zero_cond, NULL) != 0) {
      OLOG_ERROR("Failed to init ref_zero_cond");
      pthread_mutex_destroy(&session->ref_mutex);
      pthread_mutex_destroy(&session->fd_mutex);
      pthread_mutex_destroy(&session->history_mutex);
      free(session);
      return NULL;
   }

   if (pthread_mutex_init(&session->llm_config_mutex, NULL) != 0) {
      OLOG_ERROR("Failed to init llm_config_mutex");
      pthread_cond_destroy(&session->ref_zero_cond);
      pthread_mutex_destroy(&session->ref_mutex);
      pthread_mutex_destroy(&session->fd_mutex);
      pthread_mutex_destroy(&session->history_mutex);
      free(session);
      return NULL;
   }

   if (pthread_mutex_init(&session->metrics_mutex, NULL) != 0) {
      OLOG_ERROR("Failed to init metrics_mutex");
      pthread_mutex_destroy(&session->llm_config_mutex);
      pthread_cond_destroy(&session->ref_zero_cond);
      pthread_mutex_destroy(&session->ref_mutex);
      pthread_mutex_destroy(&session->fd_mutex);
      pthread_mutex_destroy(&session->history_mutex);
      free(session);
      return NULL;
   }

   if (pthread_mutex_init(&session->tools_mutex, NULL) != 0) {
      OLOG_ERROR("Failed to init tools_mutex");
      pthread_mutex_destroy(&session->metrics_mutex);
      pthread_mutex_destroy(&session->llm_config_mutex);
      pthread_cond_destroy(&session->ref_zero_cond);
      pthread_mutex_destroy(&session->ref_mutex);
      pthread_mutex_destroy(&session->fd_mutex);
      pthread_mutex_destroy(&session->history_mutex);
      free(session);
      return NULL;
   }

   // Initialize active tool tracking
   session->active_tool_count = 0;
   session->pending_visual = NULL;
   session->visual_modules_loaded[0] = '\0';

   // Initialize metrics tracker (db_id = -1 means not yet saved to DB)
   session->metrics.db_id = -1;

   // Initialize conversation history as empty JSON array
   session->conversation_history = json_object_new_array();
   if (!session->conversation_history) {
      OLOG_ERROR("Failed to create conversation history array");
      pthread_mutex_destroy(&session->tools_mutex);
      pthread_mutex_destroy(&session->metrics_mutex);
      pthread_mutex_destroy(&session->llm_config_mutex);
      pthread_cond_destroy(&session->ref_zero_cond);
      pthread_mutex_destroy(&session->ref_mutex);
      pthread_mutex_destroy(&session->fd_mutex);
      pthread_mutex_destroy(&session->history_mutex);
      free(session);
      return NULL;
   }

   // Initialize LLM config with defaults from dawn.toml
   llm_get_default_config(&session->llm_config);

   return session;
}

static void session_free(session_t *session) {
   if (!session) {
      return;
   }

   // Free conversation history
   if (session->conversation_history) {
      json_object_put(session->conversation_history);
      session->conversation_history = NULL;
   }

   // Free pending visual content
   free(session->pending_visual);
   session->pending_visual = NULL;

   // Free async compaction resources
   if (session->async_compact.pending_history) {
      json_object_put(session->async_compact.pending_history);
      session->async_compact.pending_history = NULL;
   }
   free(session->async_compact.result_summary);
   session->async_compact.result_summary = NULL;

   // Clear client_data pointer (don't free - WebSocket sessions use libwebsockets-managed memory)
   session->client_data = NULL;

   // Destroy synchronization primitives
   pthread_cond_destroy(&session->ref_zero_cond);
   pthread_mutex_destroy(&session->ref_mutex);
   pthread_mutex_destroy(&session->fd_mutex);
   pthread_mutex_destroy(&session->tools_mutex);
   pthread_mutex_destroy(&session->metrics_mutex);
   pthread_mutex_destroy(&session->llm_config_mutex);
   pthread_mutex_destroy(&session->history_mutex);

   free(session);
}

/* =============================================================================
 * Job-pool integration (background-jobs Phase 1)
 * ============================================================================= */

void session_manager_register_job_lookup(session_job_lookup_fn fn) {
   s_job_lookup_fn = fn;
}

session_t *session_manager_alloc_bare(void) {
   session_t *session = session_alloc();
   if (!session) {
      return NULL;
   }
   /* Fresh unique id (atomic — no session_manager_rwlock held here) + the same
    * baseline an interactive session gets from session_create(), minus placement
    * in sessions[].  The caller (job_manager) sets type/user binding and owns
    * teardown via session_manager_free_bare(). */
   session->session_id = atomic_fetch_add(&next_session_id, 1);
   session->ref_count = 1;
   session->created_at = time(NULL);
   session->last_activity = session->created_at;
   session->client_fd = -1;
   return session;
}

void session_manager_free_bare(session_t *session) {
   if (session) {
      session_free(session);
   }
}

static int find_free_slot(void) {
   for (int i = 0; i < MAX_SESSIONS; i++) {
      if (sessions[i] == NULL) {
         return i;
      }
   }
   return -1;
}

static session_t *find_session_by_uuid_unlocked(const char *uuid) {
   /* Prefer a LIVE (connected) session.  During reconnect churn (e.g. an OTA that
    * reboots the device several times) a stale, disconnected session for the same
    * UUID can linger at a lower slot index than the freshly-reconnected live one.
    * Returning the stale one makes session_find_by_uuid() report the device as
    * offline (its disconnected check trips) even though it is connected — which
    * surfaced as a spurious "Device is offline" on `ota push`.  Fall back to the
    * first disconnected match only if no live session exists, since the reconnect
    * path (create_dap2_session) relies on finding the disconnected session to
    * reclaim it. */
   session_t *fallback = NULL;
   for (int i = 0; i < MAX_SESSIONS; i++) {
      if (sessions[i] != NULL && sessions[i]->type == SESSION_TYPE_DAP2 &&
          strcmp(sessions[i]->identity.uuid, uuid) == 0) {
         if (!sessions[i]->disconnected) {
            return sessions[i];
         }
         if (!fallback) {
            fallback = sessions[i];
         }
      }
   }
   return fallback;
}

static session_t *find_session_by_ip_unlocked(const char *ip) {
   for (int i = 0; i < MAX_SESSIONS; i++) {
      if (sessions[i] != NULL && sessions[i]->type == SESSION_TYPE_DAP &&
          strcmp(sessions[i]->client_ip, ip) == 0) {
         return sessions[i];
      }
   }
   return NULL;
}

// =============================================================================
// Lifecycle Functions
// =============================================================================

int session_manager_init(void) {
   if (initialized) {
      OLOG_WARNING("Session manager already initialized");
      return 0;
   }

   // Initialize all slots to NULL
   memset(sessions, 0, sizeof(sessions));

   // Create local session (session_id = 0)
   session_t *local = session_alloc();
   if (!local) {
      OLOG_ERROR("Failed to create local session");
      return 1;
   }

   local->session_id = LOCAL_SESSION_ID;
   local->type = SESSION_TYPE_LOCAL;
   local->created_at = time(NULL);
   local->last_activity = local->created_at;
   local->client_fd = -1;
   local->ref_count = 1;  // Local session always has ref_count >= 1

   sessions[0] = local;
   initialized = true;

   OLOG_INFO("Session manager initialized with local session");
   return 0;
}

void session_manager_cleanup(void) {
   if (!initialized) {
      return;
   }

   pthread_rwlock_wrlock(&session_manager_rwlock);

   for (int i = 0; i < MAX_SESSIONS; i++) {
      if (sessions[i] != NULL) {
         // Shutdown: abort any in-flight operations AND gate emission (both flags).
         session_teardown_flags(sessions[i]);

         // For LOCAL session, wait for ref_count to reach 1 (workers may be using it)
         // For WebSocket/DAP sessions, force cleanup (ref_count is for reconnection support)
         if (sessions[i]->type == SESSION_TYPE_LOCAL) {
            pthread_mutex_lock(&sessions[i]->ref_mutex);
            while (sessions[i]->ref_count > 1) {
               pthread_cond_wait(&sessions[i]->ref_zero_cond, &sessions[i]->ref_mutex);
            }
            pthread_mutex_unlock(&sessions[i]->ref_mutex);
         }

         OLOG_INFO("Destroying session %u (type=%s)", sessions[i]->session_id,
                   session_type_name(sessions[i]->type));
         session_free(sessions[i]);
         sessions[i] = NULL;
      }
   }

   initialized = false;
   pthread_rwlock_unlock(&session_manager_rwlock);

   OLOG_INFO("Session manager cleanup complete");
}

// =============================================================================
// Session Creation and Retrieval
// =============================================================================

session_t *session_create(session_type_t type, int client_fd) {
   if (!initialized) {
      OLOG_ERROR("Session manager not initialized");
      return NULL;
   }

   pthread_rwlock_wrlock(&session_manager_rwlock);

   int slot = find_free_slot();
   if (slot < 0) {
      pthread_rwlock_unlock(&session_manager_rwlock);
      OLOG_WARNING("Max sessions reached (%d), rejecting new client", MAX_SESSIONS);
      return NULL;
   }

   session_t *session = session_alloc();
   if (!session) {
      pthread_rwlock_unlock(&session_manager_rwlock);
      return NULL;
   }

   session->session_id = next_session_id++;
   session->type = type;
   session->created_at = time(NULL);
   session->last_activity = session->created_at;
   session->client_fd = client_fd;
   session->ref_count = 1;  // Start with ref count of 1

   sessions[slot] = session;

   pthread_rwlock_unlock(&session_manager_rwlock);

   OLOG_INFO("Created session %u (type=%s, fd=%d)", session->session_id, session_type_name(type),
             client_fd);

   return session;
}

void session_append_room_context(session_t *session, const char *room) {
   if (!session || !room || room[0] == '\0') {
      return;
   }

   char *base = session_get_system_prompt(session);
   if (!base) {
      return;
   }

   /* Guard against double-append (e.g., reconnection without prompt reset) */
   if (strstr(base, "\nRoom=")) {
      free(base);
      return;
   }

   size_t len = strlen(base) + strlen(room) + 16;
   char *with_room = malloc(len);
   if (with_room) {
      snprintf(with_room, len, "%s\nRoom=%s.", base, room);
      session_update_system_prompt(session, with_room);
      free(with_room);
   }
   free(base);
}

void session_append_satellite_context(session_t *session, const char *room, const char *ha_area) {
   if (!session) {
      return;
   }

   char *base = session_get_system_prompt(session);
   if (!base) {
      return;
   }

   /* Guard against double-append */
   if (strstr(base, "\nRoom=")) {
      free(base);
      return;
   }

   /* Build context in stack buffer: Room + optional HA area */
   char ctx[192];
   ctx[0] = '\0';
   int len = 0;

   if (room && room[0]) {
      len = snprintf(ctx, sizeof(ctx), "\nRoom=%s.", room);
   }

   if (ha_area && ha_area[0] && len < (int)sizeof(ctx) - 1) {
      /* Sanitize ha_area: allowlist alphanumeric, spaces, hyphens, underscores */
      char safe_area[64];
      strncpy(safe_area, ha_area, sizeof(safe_area) - 1);
      safe_area[sizeof(safe_area) - 1] = '\0';
      for (char *p = safe_area; *p; p++) {
         if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
               *p == ' ' || *p == '-' || *p == '_'))
            *p = '_';
      }
      snprintf(ctx + len, sizeof(ctx) - len, "\nHomeAssistant_Area=[%s].", safe_area);
   }

   if (ctx[0] == '\0') {
      free(base);
      return;
   }

   size_t total = strlen(base) + strlen(ctx) + 1;
   char *with_ctx = malloc(total);
   if (with_ctx) {
      snprintf(with_ctx, total, "%s%s", base, ctx);
      session_update_system_prompt(session, with_ctx);
      free(with_ctx);
   }
   free(base);
}

session_t *session_create_dap2(int client_fd,
                               dap2_tier_t tier,
                               const dap2_identity_t *identity,
                               const dap2_capabilities_t *capabilities) {
   if (!initialized) {
      OLOG_ERROR("Session manager not initialized");
      return NULL;
   }

   if (!identity || strlen(identity->uuid) == 0) {
      OLOG_ERROR("DAP2 session requires valid identity with UUID");
      return NULL;
   }

   pthread_rwlock_wrlock(&session_manager_rwlock);

   /* Check for existing session with same UUID (potential reconnection)
    *
    * SECURITY: Reconnection requires BOTH UUID match AND secret match.
    * Without this, an attacker knowing a UUID could hijack sessions.
    * The secret is generated server-side and sent to client on first registration.
    */
   session_t *existing = find_session_by_uuid_unlocked(identity->uuid);
   if (existing) {
      /* Check if client provided a valid reconnect secret */
      bool has_valid_secret = identity->reconnect_secret[0] != '\0' &&
                              existing->identity.reconnect_secret[0] != '\0' &&
                              constant_time_compare(identity->reconnect_secret,
                                                    existing->identity.reconnect_secret);

      if (has_valid_secret && existing->disconnected) {
         /* SECURE RECONNECTION: UUID + secret match, and session was disconnected */
         pthread_mutex_lock(&existing->fd_mutex);
         existing->client_fd = client_fd;
         existing->disconnected = false;
         existing->last_activity = time(NULL);
         pthread_mutex_unlock(&existing->fd_mutex);

         /* Increment ref count */
         pthread_mutex_lock(&existing->ref_mutex);
         existing->ref_count++;
         pthread_mutex_unlock(&existing->ref_mutex);

         uint32_t session_id = existing->session_id;
         int history_len = json_object_array_length(existing->conversation_history);

         pthread_rwlock_unlock(&session_manager_rwlock);

         OLOG_INFO("DAP2 secure reconnection: session %u (uuid=%s, name=%s, history=%d msgs)",
                   session_id, identity->uuid, identity->name, history_len);

         return existing;
      } else if (has_valid_secret && !existing->disconnected) {
         /* SECURE RECONNECTION (still-active): Client proved identity but old session
          * hasn't been marked disconnected yet (common with flaky WiFi where TCP
          * dropped but server hasn't detected timeout). Reclaim the session. */
         pthread_mutex_lock(&existing->fd_mutex);
         existing->client_fd = client_fd;
         existing->last_activity = time(NULL);
         pthread_mutex_unlock(&existing->fd_mutex);

         /* Increment ref count */
         pthread_mutex_lock(&existing->ref_mutex);
         existing->ref_count++;
         pthread_mutex_unlock(&existing->ref_mutex);

         uint32_t session_id = existing->session_id;
         int history_len = json_object_array_length(existing->conversation_history);

         pthread_rwlock_unlock(&session_manager_rwlock);

         OLOG_INFO("DAP2 secure reconnection (active): session %u reclaimed "
                   "(uuid=%s, name=%s, history=%d msgs)",
                   session_id, identity->uuid, identity->name, history_len);

         return existing;
      } else if (!has_valid_secret && !existing->disconnected) {
         /* Existing active session, but client has no/wrong secret
          * This could be: (1) client restart without saved secret, or (2) hijack attempt
          * Create new session - old session will timeout eventually */
         OLOG_WARNING("DAP2: UUID %s has active session but client lacks valid secret - "
                      "creating new session (possible restart or hijack attempt)",
                      identity->uuid);
      } else if (!has_valid_secret && existing->disconnected) {
         /* Session disconnected but client has no/wrong secret - reject hijack */
         OLOG_WARNING("DAP2: UUID %s has disconnected session but client lacks valid secret - "
                      "creating new session to prevent hijacking",
                      identity->uuid);
      }
      /* Fall through to create new session */
   }

   /* NEW SESSION */
   int slot = find_free_slot();
   if (slot < 0) {
      pthread_rwlock_unlock(&session_manager_rwlock);
      OLOG_WARNING("Max sessions reached (%d), rejecting DAP2 client", MAX_SESSIONS);
      return NULL;
   }

   session_t *session = session_alloc();
   if (!session) {
      pthread_rwlock_unlock(&session_manager_rwlock);
      return NULL;
   }

   session->session_id = next_session_id++;
   session->type = SESSION_TYPE_DAP2;
   session->created_at = time(NULL);
   session->last_activity = session->created_at;
   session->client_fd = client_fd;
   session->ref_count = 1;

   /* DAP2-specific fields */
   session->tier = tier;
   memcpy(&session->identity, identity, sizeof(dap2_identity_t));
   if (capabilities) {
      memcpy(&session->capabilities, capabilities, sizeof(dap2_capabilities_t));
   }

   /* SECURITY: Generate cryptographic reconnect secret for this session
    * This secret MUST be returned to the client in registration_ack
    * and provided by client on reconnection attempts */
   generate_crypto_random_hex(session->identity.reconnect_secret, 32);

   sessions[slot] = session;

   pthread_rwlock_unlock(&session_manager_rwlock);

   /* Initialize with remote command prompt (excludes HUD/helmet commands).
    * Room/HA context is appended later by handle_satellite_register() after
    * the DB mapping lookup determines the HA area. */
   session_init_system_prompt(session, get_remote_command_prompt());

   OLOG_INFO("Created DAP2 session %u (tier=%d, uuid=%s, name=%s, location=%s)",
             session->session_id, tier, identity->uuid, identity->name, identity->location);

   return session;
}

char *session_get_reconnect_secret(session_t *session) {
   if (!session || session->type != SESSION_TYPE_DAP2) {
      return NULL;
   }

   if (session->identity.reconnect_secret[0] == '\0') {
      return NULL;
   }

   return strdup(session->identity.reconnect_secret);
}

session_t *session_get_or_create_dap(int client_fd, const char *client_ip) {
   if (!initialized) {
      OLOG_ERROR("Session manager not initialized");
      return NULL;
   }

   if (!client_ip || strlen(client_ip) == 0) {
      OLOG_ERROR("DAP1 session requires valid client IP");
      return NULL;
   }

   pthread_rwlock_wrlock(&session_manager_rwlock);

   // Check for existing session with same IP (reconnection)
   session_t *existing = find_session_by_ip_unlocked(client_ip);
   if (existing && !existing->disconnected) {
      // Reconnection: update socket and clear disconnected flag
      pthread_mutex_lock(&existing->fd_mutex);
      existing->client_fd = client_fd;
      existing->disconnected = false;
      existing->last_activity = time(NULL);
      pthread_mutex_unlock(&existing->fd_mutex);

      // Increment ref count
      pthread_mutex_lock(&existing->ref_mutex);
      existing->ref_count++;
      pthread_mutex_unlock(&existing->ref_mutex);

      // Capture history length while holding rwlock (safe access)
      int history_len = json_object_array_length(existing->conversation_history);
      uint32_t session_id = existing->session_id;

      pthread_rwlock_unlock(&session_manager_rwlock);

      OLOG_INFO("DAP1 reconnection: session %u (ip=%s, history=%d messages)", session_id, client_ip,
                history_len);

      return existing;
   }

   // New session
   int slot = find_free_slot();
   if (slot < 0) {
      pthread_rwlock_unlock(&session_manager_rwlock);
      OLOG_WARNING("Max sessions reached (%d), rejecting DAP1 client", MAX_SESSIONS);
      return NULL;
   }

   session_t *session = session_alloc();
   if (!session) {
      pthread_rwlock_unlock(&session_manager_rwlock);
      return NULL;
   }

   session->session_id = next_session_id++;
   session->type = SESSION_TYPE_DAP;
   session->created_at = time(NULL);
   session->last_activity = session->created_at;
   session->client_fd = client_fd;
   session->ref_count = 1;

   // Store client IP for session persistence
   strncpy(session->client_ip, client_ip, INET_ADDRSTRLEN - 1);
   session->client_ip[INET_ADDRSTRLEN - 1] = '\0';

   sessions[slot] = session;

   pthread_rwlock_unlock(&session_manager_rwlock);

   // Initialize with remote command prompt (excludes HUD/helmet commands)
   session_init_system_prompt(session, get_remote_command_prompt());

   OLOG_INFO("Created DAP1 session %u (ip=%s)", session->session_id, client_ip);

   return session;
}

session_t *session_get(uint32_t session_id) {
   if (!initialized) {
      return NULL;
   }

   pthread_rwlock_rdlock(&session_manager_rwlock);

   session_t *found = NULL;
   for (int i = 0; i < MAX_SESSIONS; i++) {
      if (sessions[i] != NULL && sessions[i]->session_id == session_id) {
         found = sessions[i];
         break;
      }
   }

   if (found) {
      // Check if session is disconnected (dying)
      if (found->disconnected) {
         pthread_rwlock_unlock(&session_manager_rwlock);
         return NULL;
      }

      // Increment ref count
      pthread_mutex_lock(&found->ref_mutex);
      found->ref_count++;
      pthread_mutex_unlock(&found->ref_mutex);
      pthread_rwlock_unlock(&session_manager_rwlock);
      return found;
   }

   pthread_rwlock_unlock(&session_manager_rwlock);
   /* Interactive-array miss: fall back to the job pool (background-jobs Phase 1).
    * Called after releasing session_manager_rwlock so the resolver's job_pool
    * lock never nests under it. */
   if (s_job_lookup_fn) {
      return s_job_lookup_fn(session_id, false);
   }
   return NULL;
}

session_t *session_find_by_uuid(const char *uuid) {
   if (!initialized || !uuid || !uuid[0]) {
      return NULL;
   }

   pthread_rwlock_rdlock(&session_manager_rwlock);

   session_t *found = find_session_by_uuid_unlocked(uuid);

   if (found) {
      if (found->disconnected) {
         pthread_rwlock_unlock(&session_manager_rwlock);
         return NULL;
      }

      pthread_mutex_lock(&found->ref_mutex);
      found->ref_count++;
      pthread_mutex_unlock(&found->ref_mutex);
   }

   pthread_rwlock_unlock(&session_manager_rwlock);
   return found;
}

session_t *session_get_for_reconnect(uint32_t session_id) {
   if (!initialized) {
      return NULL;
   }

   pthread_rwlock_rdlock(&session_manager_rwlock);

   session_t *found = NULL;
   for (int i = 0; i < MAX_SESSIONS; i++) {
      if (sessions[i] != NULL && sessions[i]->session_id == session_id) {
         found = sessions[i];
         break;
      }
   }

   if (found) {
      // Increment ref count (even for disconnected sessions - allows reconnection)
      pthread_mutex_lock(&found->ref_mutex);
      found->ref_count++;
      pthread_mutex_unlock(&found->ref_mutex);
      pthread_rwlock_unlock(&session_manager_rwlock);
      return found;
   }

   pthread_rwlock_unlock(&session_manager_rwlock);
   /* Interactive-array miss: fall back to the job pool (background-jobs Phase 1). */
   if (s_job_lookup_fn) {
      return s_job_lookup_fn(session_id, true);
   }
   return NULL;
}

void session_retain(session_t *session) {
   if (!session) {
      return;
   }

   pthread_mutex_lock(&session->ref_mutex);
   session->ref_count++;
   pthread_mutex_unlock(&session->ref_mutex);
}

void session_release(session_t *session) {
   if (!session) {
      return;
   }

   pthread_mutex_lock(&session->ref_mutex);
   session->ref_count--;

   if (session->ref_count <= 0) {
      // Signal anyone waiting for ref_count to reach 0
      pthread_cond_broadcast(&session->ref_zero_cond);
   }

   pthread_mutex_unlock(&session->ref_mutex);
}

session_t *session_get_local(void) {
   if (!initialized) {
      return NULL;
   }

   // Local session is always at index 0 and never destroyed
   return sessions[0];
}

// =============================================================================
// Session Destruction
// =============================================================================

void session_destroy(uint32_t session_id) {
   if (!initialized) {
      return;
   }

   // Don't allow destroying local session
   if (session_id == LOCAL_SESSION_ID) {
      OLOG_WARNING("Cannot destroy local session");
      return;
   }

   pthread_rwlock_wrlock(&session_manager_rwlock);

   // Find session
   int slot = -1;
   session_t *session = NULL;
   for (int i = 0; i < MAX_SESSIONS; i++) {
      if (sessions[i] != NULL && sessions[i]->session_id == session_id) {
         slot = i;
         session = sessions[i];
         break;
      }
   }

   if (!session) {
      pthread_rwlock_unlock(&session_manager_rwlock);
      OLOG_WARNING("Session %u not found for destruction", session_id);
      return;
   }

   // Phase 1: Abort the turn + gate emission, and remove from active list.
   // Mark being_destroyed FIRST (before teardown flags): a queued turn spawned
   // into the tiny pop-to-run window reads this and declines to resurrect the
   // session's flags (see the turn-queue dequeue wrappers).
   atomic_store(&session->being_destroyed, true);
   // teardown sets BOTH cancel_requested (so an in-flight LLM/compaction worker
   // aborts promptly and unrefs — the ref wait below then completes cleanly) and
   // disconnected (emission gate).  A destroy MUST cancel: unlike a bare client
   // disconnect (which the turn survives), a destroy frees the session_t.
   session_teardown_flags(session);
   sessions[slot] = NULL;

   pthread_rwlock_unlock(&session_manager_rwlock);

   /* Phase 1.25: Close the turn queue for this session BEFORE the ref-count wait.
    * Marks the session closing (no new turns can enqueue, the in-flight turn's
    * turn_done won't chain a successor) and drops every already-QUEUED turn —
    * each queued turn holds a session retain, so dropping them here lets the
    * ref-count wait below actually reach zero instead of timing out. */
   turn_queue_purge_session(session_id);

   /* Phase 1.5: Detach WebUI connections before waiting for ref_count.
    * This NULLs out conn->session pointers and calls session_release()
    * for each detached connection, allowing the ref_count wait below
    * to complete without timing out. */
#ifdef ENABLE_WEBUI
   webui_detach_session(session);
#endif

   /* Phase 1.75: Join async compaction thread if running.
    * The bg thread's CURL cancel flag is &session->cancel_requested, which
    * Phase 1's session_teardown_flags() just set, so any in-flight transfer
    * aborts and the join returns promptly. Must happen before Phase 2 to
    * release the thread's ref. */
   if (atomic_load(&session->async_compact.state) != ASYNC_COMPACT_IDLE &&
       session->async_compact.thread_active) {
      OLOG_INFO("Session %u: joining async compaction thread", session_id);
      pthread_join(session->async_compact.thread_id, NULL);
      session->async_compact.thread_active = false;
      OLOG_INFO("Session %u: async compaction thread joined", session_id);
   }

   // Phase 2: Wait for ref_count to reach 0 (bounded retry; leak rather than
   // free-under-a-worker).  teardown set cancel_requested, so a live LLM/CURL
   // worker aborts within ~1s and releases its ref — 30s is generous.  A hold
   // PAST that means a genuinely wedged worker: leaking a few-KB session_t is
   // recoverable, but freeing it while a worker still dereferences it is a UAF.
   // On the leak path we deliberately return WITHOUT Phase 3 (metrics +
   // extraction) or session_free, logging ERROR so the leak is visible.
   pthread_mutex_lock(&session->ref_mutex);
   int waited_sec = 0;
   while (session->ref_count > 0) {
      struct timespec timeout;
      clock_gettime(CLOCK_REALTIME, &timeout);
      timeout.tv_sec += 3;

      int rc = pthread_cond_timedwait(&session->ref_zero_cond, &session->ref_mutex, &timeout);
      if (rc == ETIMEDOUT) {
         waited_sec += 3;
         OLOG_WARNING("Session %u: ref_count wait %ds (ref_count=%d)", session_id, waited_sec,
                      session->ref_count);
         if (waited_sec >= SESSION_DESTROY_REF_WAIT_MAX_SEC) {
            OLOG_ERROR("Session %u: ref_count stuck at %d after %ds — leaking session instead of "
                       "freeing (UAF guard)",
                       session_id, session->ref_count, waited_sec);
            pthread_mutex_unlock(&session->ref_mutex);
            return; /* deliberate leak: skip metrics/extraction/session_free */
         }
      }
   }
   pthread_mutex_unlock(&session->ref_mutex);

   // Phase 3: Final metrics persist (updates ended_at timestamp)
   // Per-query metrics are already saved; this ensures ended_at is final.
   // Only persist if session had at least one query (db_id > 0).
   pthread_mutex_lock(&session->metrics_mutex);
   if (session->metrics.db_id > 0 && auth_db_is_ready()) {
      session_metrics_t db_metrics = { 0 };
      session_metrics_tracker_t *m = &session->metrics;

      db_metrics.id = m->db_id;
      db_metrics.session_id = session->session_id;
      db_metrics.user_id = m->user_id;
      strncpy(db_metrics.session_type, session_type_name(session->type),
              sizeof(db_metrics.session_type) - 1);
      db_metrics.started_at = session->created_at;
      db_metrics.ended_at = time(NULL);
      db_metrics.queries_total = m->queries_total;
      db_metrics.queries_cloud = m->queries_cloud;
      db_metrics.queries_local = m->queries_local;
      db_metrics.errors_count = m->errors_count;
      db_metrics.fallbacks_count = m->fallbacks_count;

      if (m->perf_sample_count > 0) {
         db_metrics.avg_asr_ms = m->asr_ms_sum / m->perf_sample_count;
         db_metrics.avg_llm_ttft_ms = m->llm_ttft_ms_sum / m->perf_sample_count;
         db_metrics.avg_llm_total_ms = m->llm_total_ms_sum / m->perf_sample_count;
         db_metrics.avg_tts_ms = m->tts_ms_sum / m->perf_sample_count;
         db_metrics.avg_pipeline_ms = m->pipeline_ms_sum / m->perf_sample_count;
      }

      auth_db_save_session_metrics(&db_metrics);
      OLOG_INFO("Session %u: Final metrics saved (queries=%u, cloud=%u, local=%u)", session_id,
                m->queries_total, m->queries_cloud, m->queries_local);
   }
   pthread_mutex_unlock(&session->metrics_mutex);

   /* Trigger memory extraction for authenticated sessions (WebUI / DAP2
    * / MESSAGING) with queries.  MESSAGING sessions reach this path on
    * /new reset (engine evicts the slot and calls session_destroy),
    * LRU eviction in the engine, and engine shutdown. */
   if ((session->type == SESSION_TYPE_WEBUI || session->type == SESSION_TYPE_DAP2 ||
        session->type == SESSION_TYPE_MESSAGING) &&
       session->metrics.user_id > 0 && session->metrics.queries_total > 0 &&
       g_config.memory.enabled) {
      /* Copy conversation history reference while we still have it */
      pthread_mutex_lock(&session->history_mutex);
      struct json_object *history = session->conversation_history;
      int message_count = history ? (int)json_object_array_length(history) : 0;
      int duration_seconds = (int)(time(NULL) - session->created_at);

      if (message_count > 2 && history) {
         /* Create session ID string for the summary */
         char session_id_str[32];
         snprintf(session_id_str, sizeof(session_id_str), "ws_%u", session->session_id);

         /* Strip _provider_state (OpenAI Responses encrypted reasoning blobs) before
          * forwarding to the extraction LLM, which may be a different provider.
          * See llm_history_strip_provider_state docs. */
         struct json_object *clean = llm_history_strip_provider_state(history);
         if (clean) {
            /* Trigger extraction (copies history internally)
             * Pass 0 for conversation_id - session doesn't track which DB conversation
             * it's associated with. Incremental extraction works when triggered from
             * WebUI with known conversation_id. */
            memory_extraction_fallback_t fb;
            memory_extraction_build_fallback(session, &fb);

            memory_trigger_extraction(session->metrics.user_id, 0, session_id_str, clean,
                                      message_count, duration_seconds, &fb);
            json_object_put(clean);
         }
      }
      pthread_mutex_unlock(&session->history_mutex);
   }

   OLOG_INFO("Destroying session %u (type=%s)", session_id, session_type_name(session->type));
   /* The turn queue was already closed + drained in Phase 1.25 (before the
    * ref-count wait).  Invalidate any queued WebUI responses (TSan UAF fix)
    * before the free. */
   webui_send_purge_session(session);
   session_free(session);
}

bool session_manager_conv_has_turn_in_flight(int64_t conv_id) {
   if (!initialized || conv_id <= 0) {
      return false;
   }
   bool busy = false;
   pthread_rwlock_rdlock(&session_manager_rwlock);
   for (int i = 0; i < MAX_SESSIONS; i++) {
      session_t *s = sessions[i];
      if (s != NULL && atomic_load(&s->turn_in_flight) > 0 &&
          atomic_load(&s->stream_conversation_id) == conv_id) {
         busy = true;
         break;
      }
   }
   pthread_rwlock_unlock(&session_manager_rwlock);
   return busy;
}

void session_cleanup_expired(void) {
   if (!initialized) {
      return;
   }

   time_t now = time(NULL);

   pthread_rwlock_rdlock(&session_manager_rwlock);

   // Collect IDs of expired sessions (can't destroy while holding rwlock)
   uint32_t expired_ids[MAX_SESSIONS];
   int expired_count = 0;

   for (int i = 1; i < MAX_SESSIONS; i++) {  // Skip local session (i=0)
      if (sessions[i] != NULL) {
         /* Skip sessions whose lifetime is managed by an external
          * subsystem.  Two gates, both honored: (a) SESSION_TYPE_MESSAGING
          * — the messaging engine maintains a long-lived (provider,
          * address) → session_t map for SMS / chat-app conversations
          * that may sit idle between messages for hours; (b) the
          * idle_timeout_exempt escape hatch for any other subsystem
          * that needs the same semantics without claiming a dedicated
          * type.  Destroying these here would leave dangling pointers
          * in the owner's map after session_destroy's 3-sec ref-count
          * wait times out. */
         if (sessions[i]->type == SESSION_TYPE_MESSAGING || sessions[i]->idle_timeout_exempt) {
            continue;
         }
         /* Do not reap a session with a turn in flight.  Post background-jobs
          * Phase 1 a disconnected client no longer aborts its turn, so a
          * still-generating worker routinely coexists with an idle-looking
          * (stale last_activity) session; destroying it here would abort the
          * survivor and race its worker.  The worker clears this on exit; the
          * session is reaped on the next sweep once truly idle. */
         if (atomic_load(&sessions[i]->turn_in_flight) > 0) {
            continue;
         }
         time_t idle_time = now - sessions[i]->last_activity;
         if (idle_time > g_config.network.session_timeout_sec) {
            expired_ids[expired_count++] = sessions[i]->session_id;
         }
      }
   }

   pthread_rwlock_unlock(&session_manager_rwlock);

   // Destroy expired sessions
   for (int i = 0; i < expired_count; i++) {
      OLOG_INFO("Session %u expired (idle > %d seconds)", expired_ids[i],
                g_config.network.session_timeout_sec);
      session_destroy(expired_ids[i]);
   }
}

void session_check_idle_conversations(void) {
   if (!initialized || !g_config.memory.enabled ||
       g_config.memory.conversation_idle_timeout_min <= 0) {
      return;
   }

   time_t now = time(NULL);
   time_t timeout_sec = g_config.memory.conversation_idle_timeout_min * 60;

   /* Collect idle sessions and retain them under rwlock.
    * Can't call session_save_voice_conversation while holding rwlock (it takes
    * history_mutex and does DB I/O), so we retain and process outside the lock.
    * Uses direct retain instead of session_get() because we also want to save
    * conversations on disconnected sessions before they get destroyed. */
   session_t *idle_sessions[MAX_SESSIONS];
   int idle_count = 0;

   pthread_rwlock_rdlock(&session_manager_rwlock);

   for (int i = 1; i < MAX_SESSIONS; i++) { /* Skip local session (i=0) */
      session_t *s = sessions[i];
      if (!s || s->last_interaction_complete == 0) {
         continue;
      }
      /* WebUI sessions preserve history — users see chat on screen and expect
       * continuity. They have explicit "New Conversation" for manual reset. */
      if (s->type == SESSION_TYPE_WEBUI) {
         continue;
      }
      time_t idle_sec = now - s->last_interaction_complete;
      if (idle_sec >= timeout_sec && session_has_messages(s)) {
         session_retain(s);
         idle_sessions[idle_count++] = s;
      }
   }

   pthread_rwlock_unlock(&session_manager_rwlock);

   /* Save and clear idle conversations */
   for (int i = 0; i < idle_count; i++) {
      session_t *s = idle_sessions[i];
      OLOG_INFO("Session %u: Idle timeout after %d minutes, saving conversation", s->session_id,
                g_config.memory.conversation_idle_timeout_min);
      int64_t conv_id = 0;
      if (session_save_voice_conversation(s, &conv_id) == 0 && conv_id > 0) {
         OLOG_INFO("Session %u: Saved as conversation %lld", s->session_id, (long long)conv_id);
      }
      session_release(s);
   }
}

// =============================================================================
// Conversation History
// =============================================================================

void session_add_message(session_t *session, const char *role, const char *content) {
   if (!session || !role || !content) {
      return;
   }

   pthread_mutex_lock(&session->history_mutex);

   /* Guard against corrupted or NULL history — recover by creating a fresh array */
   if (!session->conversation_history ||
       !json_object_is_type(session->conversation_history, json_type_array)) {
      OLOG_ERROR("Session %u: conversation_history is %s (expected array), recreating",
                 session->session_id,
                 session->conversation_history
                     ? json_type_to_name(json_object_get_type(session->conversation_history))
                     : "NULL");
      if (session->conversation_history) {
         json_object_put(session->conversation_history);
      }
      session->conversation_history = json_object_new_array();
      if (!session->conversation_history) {
         pthread_mutex_unlock(&session->history_mutex);
         OLOG_ERROR("Session %u: Failed to recreate conversation history", session->session_id);
         return;
      }
   }

   struct json_object *message = json_object_new_object();
   if (!message) {
      pthread_mutex_unlock(&session->history_mutex);
      OLOG_ERROR("Failed to create message object");
      return;
   }

   json_object_object_add(message, "role", json_object_new_string(role));
   json_object_object_add(message, "content", json_object_new_string(content));

   json_object_array_add(session->conversation_history, message);

   int count = json_object_array_length(session->conversation_history);
   OLOG_INFO("Session %u: Added %s message to history (now %d messages)", session->session_id, role,
             count);

   pthread_mutex_unlock(&session->history_mutex);
}

static int broadcast_system_message_impl(const char *content, bool include_local) {
   if (!initialized || !content || content[0] == '\0') {
      return 0;
   }

   /* Snapshot session IDs under the read lock, then apply after releasing it —
    * same discipline as session_manager_refresh_all_prompts(): never hold a
    * per-session lock while the module lock is held. */
   uint32_t snapshot_ids[MAX_SESSIONS];
   int count = 0;

   pthread_rwlock_rdlock(&session_manager_rwlock);
   for (int i = 0; i < MAX_SESSIONS; i++) {
      session_t *s = sessions[i];
      if (!s) {
         continue;
      }
      snapshot_ids[count++] = s->session_id;
   }
   pthread_rwlock_unlock(&session_manager_rwlock);

   int delivered = 0;
   for (int i = 0; i < count; i++) {
      session_t *s = session_get(snapshot_ids[i]); /* Retains */
      if (!s) {
         continue;
      }

      /* Interactive surfaces only.  Messaging-channel forever-conversations are
       * excluded so a device event doesn't leak into an unrelated chat.  The
       * LOCAL session is skipped when include_local is false so an off-main-thread
       * caller doesn't race the unlocked main-path append (see the _nonlocal
       * variant's header doc). */
      bool is_local = (s->type == SESSION_TYPE_LOCAL);
      bool interactive = is_local || s->type == SESSION_TYPE_DAP || s->type == SESSION_TYPE_DAP2 ||
                         s->type == SESSION_TYPE_WEBUI;
      if (interactive && (include_local || !is_local)) {
         session_add_message(s, "system", content);
         delivered++;
      }

      session_release(s);
   }

   return delivered;
}

int session_broadcast_system_message(const char *content) {
   return broadcast_system_message_impl(content, true);
}

int session_broadcast_system_message_nonlocal(const char *content) {
   return broadcast_system_message_impl(content, false);
}

void session_stamp_last_message_id(session_t *session, const char *role, int64_t msg_id) {
   if (!session || !role || msg_id <= 0)
      return;

   pthread_mutex_lock(&session->history_mutex);

   if (!session->conversation_history)
      goto unlock;

   int len = (int)json_object_array_length(session->conversation_history);
   for (int i = len - 1; i >= 0; i--) {
      struct json_object *entry = json_object_array_get_idx(session->conversation_history, i);
      if (!entry)
         continue;
      struct json_object *role_obj;
      if (!json_object_object_get_ex(entry, "role", &role_obj))
         continue;
      if (strcmp(json_object_get_string(role_obj), role) != 0)
         continue;
      /* Skip entries that already have an ID stamped */
      if (json_object_object_get_ex(entry, "id", NULL))
         continue;
      json_object_object_add(entry, "id", json_object_new_int64(msg_id));
      break;
   }

   /* Phase 1g-i: mirror the user-msg id into a session field so the
    * WebUI prompt builder can read it without parsing JSON history.
    * Other roles (assistant / tool / system) don't drive turn_id —
    * focus injection rebuilds on USER turns only. */
   if (strcmp(role, "user") == 0)
      session->last_user_msg_id = msg_id;

unlock:
   pthread_mutex_unlock(&session->history_mutex);
}

int64_t session_get_last_user_msg_id(session_t *session) {
   if (!session)
      return 0;
   pthread_mutex_lock(&session->history_mutex);
   int64_t id = session->last_user_msg_id;
   pthread_mutex_unlock(&session->history_mutex);
   return id;
}

void session_add_message_with_images(session_t *session,
                                     const char *role,
                                     const char *text,
                                     const char *const *vision_images,
                                     int vision_image_count) {
   if (!session || !role || !text) {
      return;
   }

   /* No images? Fall back to simple text message */
   if (!vision_images || vision_image_count <= 0) {
      session_add_message(session, role, text);
      return;
   }

   pthread_mutex_lock(&session->history_mutex);

   /* Guard against corrupted or NULL history */
   if (!session->conversation_history ||
       !json_object_is_type(session->conversation_history, json_type_array)) {
      OLOG_ERROR("Session %u: conversation_history corrupt in add_message_with_images, recreating",
                 session->session_id);
      if (session->conversation_history) {
         json_object_put(session->conversation_history);
      }
      session->conversation_history = json_object_new_array();
      if (!session->conversation_history) {
         pthread_mutex_unlock(&session->history_mutex);
         return;
      }
   }

   struct json_object *message = json_object_new_object();
   if (!message) {
      pthread_mutex_unlock(&session->history_mutex);
      OLOG_ERROR("Failed to create message object");
      return;
   }

   json_object_object_add(message, "role", json_object_new_string(role));

   /* Build multi-part content array in OpenAI format */
   struct json_object *content = json_object_new_array();
   if (!content) {
      json_object_put(message);
      pthread_mutex_unlock(&session->history_mutex);
      OLOG_ERROR("Failed to create content array");
      return;
   }

   /* Add text part first.  INVARIANT: this text part must always precede any
    * image part, so a user-attached image message never collapses to a
    * lone-image content array — llm_tools.c's is_capture_image_message()
    * relies on that shape (single-element array, image-only) to distinguish
    * ambient tool captures (eviction-eligible) from images the user
    * deliberately attached (never evicted). Keep this ordering if this
    * function is ever changed to allow an empty/absent text argument. */
   struct json_object *text_part = json_object_new_object();
   json_object_object_add(text_part, "type", json_object_new_string("text"));
   json_object_object_add(text_part, "text", json_object_new_string(text));
   json_object_array_add(content, text_part);

   /* Add image parts */
   for (int i = 0; i < vision_image_count; i++) {
      if (!vision_images[i] || vision_images[i][0] == '\0') {
         continue;
      }

      struct json_object *image_part = json_object_new_object();
      json_object_object_add(image_part, "type", json_object_new_string("image_url"));

      struct json_object *image_url = json_object_new_object();

      /* Build data URI: data:image/jpeg;base64,<data> */
      size_t data_len = strlen(vision_images[i]);
      size_t uri_len = 23 + data_len + 1; /* "data:image/jpeg;base64," + data + null */
      char *data_uri = malloc(uri_len);
      if (data_uri) {
         snprintf(data_uri, uri_len, "data:image/jpeg;base64,%s", vision_images[i]);
         json_object_object_add(image_url, "url", json_object_new_string(data_uri));
         free(data_uri);
      }

      json_object_object_add(image_part, "image_url", image_url);
      json_object_array_add(content, image_part);
   }

   json_object_object_add(message, "content", content);
   json_object_array_add(session->conversation_history, message);

   OLOG_INFO("Session %u: Added message with %d images to history", session->session_id,
             vision_image_count);

   pthread_mutex_unlock(&session->history_mutex);
}

void session_add_message_multipart(session_t *session, struct json_object *message) {
   if (!session || !message) {
      if (message) {
         json_object_put(message);
      }
      return;
   }

   pthread_mutex_lock(&session->history_mutex);

   /* Guard against corrupted or NULL history (mirrors add_message_with_images). */
   if (!session->conversation_history ||
       !json_object_is_type(session->conversation_history, json_type_array)) {
      OLOG_ERROR("Session %u: conversation_history corrupt in add_message_multipart, recreating",
                 session->session_id);
      if (session->conversation_history) {
         json_object_put(session->conversation_history);
      }
      session->conversation_history = json_object_new_array();
      if (!session->conversation_history) {
         pthread_mutex_unlock(&session->history_mutex);
         json_object_put(message);
         return;
      }
   }

   json_object_array_add(session->conversation_history, message); /* takes ownership */

   pthread_mutex_unlock(&session->history_mutex);
}

void session_set_tool_persist_hook(session_t *session, session_tool_persist_fn cb, void *userdata) {
   if (!session) {
      return;
   }
   /* Read/written only on the worker thread that owns the turn (set before the LLM
    * call, cleared after) — no lock needed, and no other thread fires the hook. */
   session->tool_persist_cb = cb;
   session->tool_persist_userdata = userdata;
}

void session_set_tool_iteration_hook(session_t *session,
                                     session_tool_iteration_fn cb,
                                     void *userdata) {
   if (!session) {
      return;
   }
   /* Same threading contract as the persist hook above: worker-thread-only. */
   session->tool_iteration_cb = cb;
   session->tool_iteration_userdata = userdata;
}

struct json_object *session_get_history(session_t *session) {
   if (!session) {
      return NULL;
   }

   pthread_mutex_lock(&session->history_mutex);
   struct json_object *history = session->conversation_history;

   // Increment reference count so caller can use it
   json_object_get(history);

   pthread_mutex_unlock(&session->history_mutex);

   return history;
}

void session_clear_history(session_t *session) {
   if (!session) {
      return;
   }

   pthread_mutex_lock(&session->history_mutex);

   // Release old history
   if (session->conversation_history) {
      json_object_put(session->conversation_history);
   }

   // Create new empty array
   session->conversation_history = json_object_new_array();
   if (!session->conversation_history) {
      OLOG_ERROR("Failed to create new conversation history array");
   }

   // Clear visual guideline cache — LLM no longer has them in context
   session->visual_modules_loaded[0] = '\0';

   pthread_mutex_unlock(&session->history_mutex);
}

bool session_has_messages(session_t *session) {
   if (!session) {
      return false;
   }

   pthread_mutex_lock(&session->history_mutex);
   int count = session->conversation_history
                   ? (int)json_object_array_length(session->conversation_history)
                   : 0;
   pthread_mutex_unlock(&session->history_mutex);

   /* Require at least 2 messages (system prompt + user message) */
   return count >= 2;
}

void session_update_interaction_complete(session_t *session) {
   if (!session) {
      return;
   }
   session->last_interaction_complete = time(NULL);
}

int session_save_voice_conversation(session_t *session, int64_t *conv_id_out) {
   if (conv_id_out)
      *conv_id_out = 0;

   if (!session || !conv_id_out) {
      return 1;
   }

   pthread_mutex_lock(&session->history_mutex);

   /* Check if there are messages to save */
   if (!session->conversation_history) {
      pthread_mutex_unlock(&session->history_mutex);
      return 1;
   }

   int msg_count = (int)json_object_array_length(session->conversation_history);
   if (msg_count < 2) {
      /* No user messages, just system prompt */
      pthread_mutex_unlock(&session->history_mutex);
      return 1;
   }

   /* Get user ID: prefer session's mapped user, fall back to config default */
   int user_id = session->metrics.user_id;
   if (user_id <= 0) {
      user_id = g_config.memory.default_voice_user_id;
   }
   if (user_id <= 0) {
      user_id = 1; /* Fallback to admin */
   }

   /* Generate title from first user message */
   char title[128] = "Voice Conversation";
   for (int i = 0; i < msg_count; i++) {
      struct json_object *msg = json_object_array_get_idx(session->conversation_history, i);
      if (!msg)
         continue;

      struct json_object *role_obj;
      if (!json_object_object_get_ex(msg, "role", &role_obj))
         continue;

      const char *role = json_object_get_string(role_obj);
      if (role && strcmp(role, "user") == 0) {
         struct json_object *content_obj;
         if (json_object_object_get_ex(msg, "content", &content_obj)) {
            const char *content = json_object_get_string(content_obj);
            if (content && strlen(content) > 0) {
               /* Truncate to title length, add ellipsis if needed */
               size_t max_len = sizeof(title) - 4; /* Room for "..." */
               if (strlen(content) <= max_len) {
                  strncpy(title, content, sizeof(title) - 1);
               } else {
                  strncpy(title, content, max_len);
                  title[max_len] = '\0';
                  strcat(title, "...");
               }
               title[sizeof(title) - 1] = '\0';
            }
         }
         break;
      }
   }

   /* Create conversation in database with voice origin */
   int64_t conv_id = 0;
   int rc = conv_db_create_with_origin(user_id, title, "voice", &conv_id);
   if (rc != AUTH_DB_SUCCESS) {
      OLOG_ERROR("Session %u: Failed to create voice conversation: %d", session->session_id, rc);
      pthread_mutex_unlock(&session->history_mutex);
      return 1;
   }

   /* Save all messages to database */
   for (int i = 0; i < msg_count; i++) {
      struct json_object *msg = json_object_array_get_idx(session->conversation_history, i);
      if (!msg)
         continue;

      struct json_object *role_obj, *content_obj;
      if (!json_object_object_get_ex(msg, "role", &role_obj))
         continue;
      if (!json_object_object_get_ex(msg, "content", &content_obj))
         continue;

      const char *role = json_object_get_string(role_obj);
      const char *content = json_object_get_string(content_obj);

      if (role && content) {
         int64_t msg_id = 0;
         if (conv_db_add_message_ex(conv_id, user_id, role, content, &msg_id) == AUTH_DB_SUCCESS &&
             msg_id > 0) {
            json_object_object_add(msg, "id", json_object_new_int64(msg_id));
         }
      }
   }

   /* Trigger memory extraction (async) if enabled */
   if (g_config.memory.enabled) {
      int duration_seconds = (int)(time(NULL) - session->created_at);
      char session_id_str[32];
      snprintf(session_id_str, sizeof(session_id_str), "voice_%u", session->session_id);

      /* Deep-copy history with provider-private fields stripped. The OpenAI
       * Responses path stashes encrypted reasoning blobs under _provider_state
       * which are session-bound to OpenAI and must not be forwarded to the
       * memory-extraction LLM (which may be Claude/Gemini/local). */
      struct json_object *history_copy = llm_history_strip_provider_state(
          session->conversation_history);

      if (history_copy) {
         memory_extraction_fallback_t fb;
         memory_extraction_build_fallback(session, &fb);

         memory_trigger_extraction(user_id, conv_id, session_id_str, history_copy, msg_count,
                                   duration_seconds, &fb);
      }
   }

   pthread_mutex_unlock(&session->history_mutex);

   /* Clear session history and reset timestamp */
   session_clear_history(session);
   session->last_interaction_complete = 0;

   /* Re-initialize with system prompt (use local command prompt for voice sessions) */
   const char *system_prompt = get_local_command_prompt();
   if (system_prompt) {
      session_init_system_prompt(session, system_prompt);
   }

   OLOG_INFO("Session %u: Saved voice conversation %lld (%d messages, user %d)",
             session->session_id, (long long)conv_id, msg_count, user_id);

   *conv_id_out = conv_id;
   return 0;
}

void session_init_system_prompt(session_t *session, const char *system_prompt) {
   if (!session || !system_prompt) {
      return;
   }

   pthread_mutex_lock(&session->history_mutex);

   // Release old history
   if (session->conversation_history) {
      json_object_put(session->conversation_history);
   }

   // Create new array with system message
   session->conversation_history = json_object_new_array();
   if (!session->conversation_history) {
      OLOG_ERROR("Failed to create conversation history array");
      pthread_mutex_unlock(&session->history_mutex);
      return;
   }

   // Add system message
   struct json_object *system_message = json_object_new_object();
   if (system_message) {
      json_object_object_add(system_message, "role", json_object_new_string("system"));
      json_object_object_add(system_message, "content", json_object_new_string(system_prompt));
      json_object_array_add(session->conversation_history, system_message);
   }

   /* Phase 1f: a fresh system prompt on this session is by definition a
    * SESSION_START boundary — clear dedup state under the same lock so
    * the next PER_TURN admits all candidates fresh.  Inline memset (not
    * session_injected_set_clear) because we already hold history_mutex
    * here and the helper is self-locking. */
   memset(&session->injected_set, 0, sizeof(session->injected_set));

   pthread_mutex_unlock(&session->history_mutex);

   OLOG_INFO("Session %u: Initialized with system prompt (%zu chars)", session->session_id,
             strlen(system_prompt));
}

void session_update_system_prompt(session_t *session, const char *system_prompt) {
   if (!session || !system_prompt) {
      return;
   }

   pthread_mutex_lock(&session->history_mutex);

   if (!session->conversation_history) {
      pthread_mutex_unlock(&session->history_mutex);
      return;
   }

   /* Find existing system message */
   int len = json_object_array_length(session->conversation_history);
   struct json_object *system_msg = NULL;
   int system_idx = -1;

   for (int i = 0; i < len; i++) {
      struct json_object *msg = json_object_array_get_idx(session->conversation_history, i);
      struct json_object *role_obj;
      if (json_object_object_get_ex(msg, "role", &role_obj)) {
         const char *role = json_object_get_string(role_obj);
         if (role && strcmp(role, "system") == 0) {
            system_msg = msg;
            system_idx = i;
            break;
         }
      }
   }

   if (system_msg) {
      /* Update existing system message's content */
      json_object_object_del(system_msg, "content");
      json_object_object_add(system_msg, "content", json_object_new_string(system_prompt));
      OLOG_INFO("Session %u: Updated system prompt (%zu chars)", session->session_id,
                strlen(system_prompt));
   } else {
      /* No system message found - insert at index 0.
       *
       * IMPORTANT: json-c 0.15 doesn't expose json_object_array_insert_idx,
       * and json_object_array_put_idx REPLACES whatever's at the given
       * position rather than inserting before it.  Using put_idx(0, new)
       * on a non-empty array would silently clobber the first existing
       * message — corrupted "Hi friend" → vanished if a user message
       * happened to land at index 0 first.  This bit messaging-backed
       * sessions specifically because they start with an empty history,
       * have a user message appended on first inbound, and then this
       * function fires for per-turn focus injection BEFORE any prior
       * system prompt exists.  WebUI/voice sessions never tripped it
       * because they always have a system message at index 0 from
       * earlier turns.
       *
       * Two cases:
       *   - Empty history → put_idx(0, ...) safely grows the array.
       *   - Non-empty history (no system message anywhere) → rebuild
       *     a new array with system at index 0 followed by the
       *     existing messages, then swap. */
      struct json_object *new_msg = json_object_new_object();
      if (new_msg) {
         json_object_object_add(new_msg, "role", json_object_new_string("system"));
         json_object_object_add(new_msg, "content", json_object_new_string(system_prompt));

         if (len == 0) {
            json_object_array_put_idx(session->conversation_history, 0, new_msg);
         } else {
            struct json_object *rebuilt = json_object_new_array();
            if (!rebuilt) {
               json_object_put(new_msg);
               OLOG_ERROR("Session %u: failed to allocate rebuilt history", session->session_id);
               pthread_mutex_unlock(&session->history_mutex);
               return;
            }
            json_object_array_add(rebuilt, new_msg);
            for (int i = 0; i < len; i++) {
               struct json_object *old = json_object_array_get_idx(session->conversation_history,
                                                                   i);
               /* Bump refcount so the message survives the array put()
                * below; json_object_array_add doesn't increment, so the
                * old reference is what carries through. */
               json_object_get(old);
               json_object_array_add(rebuilt, old);
            }
            json_object_put(session->conversation_history);
            session->conversation_history = rebuilt;
         }
         OLOG_INFO("Session %u: Inserted system prompt at start (%zu chars)", session->session_id,
                   strlen(system_prompt));
      }
   }

   pthread_mutex_unlock(&session->history_mutex);
}

/* Helper: scan the conversation history under @p session->history_mutex
 * (CALLER holds it) and rebuild the array so the leading system
 * messages match @p stable + @p volatile.  Existing non-system
 * messages keep their relative order, just sit at indexes 2+ (or 1+
 * if only stable is present).  Either input may be NULL/empty to
 * omit that segment.
 *
 * Returns SUCCESS / FAILURE; on FAILURE the existing history is left
 * intact (no partial mutation). */
static int rebuild_history_with_two_system_messages_locked(session_t *session,
                                                           const char *stable_prefix,
                                                           const char *volatile_block) {
   if (session == NULL || session->conversation_history == NULL)
      return FAILURE;

   const bool have_stable = stable_prefix != NULL && stable_prefix[0] != '\0';
   const bool have_volatile = volatile_block != NULL && volatile_block[0] != '\0';
   if (!have_stable && !have_volatile)
      return FAILURE;

   struct json_object *rebuilt = json_object_new_array();
   if (rebuilt == NULL)
      return FAILURE;

   if (have_stable) {
      struct json_object *msg = json_object_new_object();
      if (msg == NULL) {
         json_object_put(rebuilt);
         return FAILURE;
      }
      json_object_object_add(msg, "role", json_object_new_string("system"));
      json_object_object_add(msg, "content", json_object_new_string(stable_prefix));
      json_object_array_add(rebuilt, msg);
   }
   if (have_volatile) {
      struct json_object *msg = json_object_new_object();
      if (msg == NULL) {
         json_object_put(rebuilt);
         return FAILURE;
      }
      json_object_object_add(msg, "role", json_object_new_string("system"));
      json_object_object_add(msg, "content", json_object_new_string(volatile_block));
      json_object_array_add(rebuilt, msg);
   }

   /* Preserve every non-system message in original order. */
   const int len = json_object_array_length(session->conversation_history);
   for (int i = 0; i < len; i++) {
      struct json_object *old = json_object_array_get_idx(session->conversation_history, i);
      struct json_object *role_obj = NULL;
      const char *role = NULL;
      if (json_object_object_get_ex(old, "role", &role_obj))
         role = json_object_get_string(role_obj);
      if (role != NULL && strcmp(role, "system") == 0)
         continue;
      /* Refcount bump: json_object_array_add takes ownership but old
       * is still referenced by conversation_history until we put it. */
      json_object_get(old);
      json_object_array_add(rebuilt, old);
   }

   json_object_put(session->conversation_history);
   session->conversation_history = rebuilt;
   return SUCCESS;
}

void session_update_system_messages(session_t *session,
                                    const char *stable_prefix,
                                    const char *volatile_block) {
   if (session == NULL)
      return;
   const bool have_stable = stable_prefix != NULL && stable_prefix[0] != '\0';
   const bool have_volatile = volatile_block != NULL && volatile_block[0] != '\0';
   if (!have_stable && !have_volatile)
      return; /* nothing to push — leave system messages as-is */

   pthread_mutex_lock(&session->history_mutex);
   if (session->conversation_history == NULL) {
      session->conversation_history = json_object_new_array();
      if (session->conversation_history == NULL) {
         pthread_mutex_unlock(&session->history_mutex);
         return;
      }
   }

   if (rebuild_history_with_two_system_messages_locked(session, stable_prefix, volatile_block) !=
       SUCCESS) {
      OLOG_ERROR("Session %u: failed to rebuild history with two-system-messages shape",
                 session->session_id);
      pthread_mutex_unlock(&session->history_mutex);
      return;
   }

   /* Drift detection.  Hash the stable prefix and compare to the
    * session's last-known value.  First call post-init (prior hash 0)
    * always "drifts"; the drift_logged gate suppresses that boot-time
    * log so the operator only sees genuine cache-busting events
    * (settings edits, code regressions that leak volatile into stable).
    * Cheap — FNV-1a over ~3.7 KB takes microseconds. */
   const uint32_t new_hash = have_stable ? prompt_compose_fnv1a(stable_prefix) : 0u;
   const uint32_t prev_hash = session->stable_prefix_hash;
   if (prev_hash != 0u && prev_hash != new_hash && !session->stable_prefix_drift_logged) {
      OLOG_INFO("Session %u: stable prefix drifted (was %08x, now %08x) — Anthropic cache "
                "invalidated for one turn",
                session->session_id, prev_hash, new_hash);
      session->stable_prefix_drift_logged = true;
   }
   session->stable_prefix_hash = new_hash;

   pthread_mutex_unlock(&session->history_mutex);

   OLOG_INFO("Session %u: Updated system messages (stable=%zu, volatile=%zu chars)",
             session->session_id, have_stable ? strlen(stable_prefix) : 0,
             have_volatile ? strlen(volatile_block) : 0);
}

void session_append_to_volatile_segment(session_t *session, const char *text) {
   if (session == NULL || text == NULL || text[0] == '\0')
      return;

   pthread_mutex_lock(&session->history_mutex);

   if (session->conversation_history == NULL) {
      pthread_mutex_unlock(&session->history_mutex);
      return;
   }

   /* Walk backward to find the LAST role:"system" message.  In the
    * two-segment shape that's the volatile block; in legacy single-
    * message sessions it's the lone system message.  Either way the
    * cacheable stable prefix at messages[0] is untouched whenever the
    * two-message shape applies. */
   const int len = json_object_array_length(session->conversation_history);
   struct json_object *target = NULL;
   for (int i = len - 1; i >= 0; i--) {
      struct json_object *msg = json_object_array_get_idx(session->conversation_history, i);
      struct json_object *role_obj = NULL;
      if (!json_object_object_get_ex(msg, "role", &role_obj))
         continue;
      const char *role = json_object_get_string(role_obj);
      if (role != NULL && strcmp(role, "system") == 0) {
         target = msg;
         break;
      }
   }
   if (target == NULL) {
      pthread_mutex_unlock(&session->history_mutex);
      return;
   }

   struct json_object *content_obj = NULL;
   if (!json_object_object_get_ex(target, "content", &content_obj)) {
      pthread_mutex_unlock(&session->history_mutex);
      return;
   }
   const char *current = json_object_get_string(content_obj);
   if (current == NULL)
      current = "";

   const size_t cur_len = strlen(current);
   const size_t hint_len = strlen(text);
   char *augmented = malloc(cur_len + hint_len + 3);
   if (augmented == NULL) {
      pthread_mutex_unlock(&session->history_mutex);
      return;
   }
   memcpy(augmented, current, cur_len);
   augmented[cur_len] = '\n';
   augmented[cur_len + 1] = '\n';
   memcpy(augmented + cur_len + 2, text, hint_len);
   augmented[cur_len + 2 + hint_len] = '\0';

   json_object_object_del(target, "content");
   json_object_object_add(target, "content", json_object_new_string(augmented));
   free(augmented);

   pthread_mutex_unlock(&session->history_mutex);
}

char *session_get_system_prompt(session_t *session) {
   if (!session) {
      return NULL;
   }

   char *result = NULL;

   pthread_mutex_lock(&session->history_mutex);

   if (session->conversation_history) {
      int len = json_object_array_length(session->conversation_history);
      for (int i = 0; i < len; i++) {
         struct json_object *msg = json_object_array_get_idx(session->conversation_history, i);
         struct json_object *role_obj;
         if (json_object_object_get_ex(msg, "role", &role_obj)) {
            const char *role = json_object_get_string(role_obj);
            if (role && strcmp(role, "system") == 0) {
               struct json_object *content_obj;
               if (json_object_object_get_ex(msg, "content", &content_obj)) {
                  const char *content = json_object_get_string(content_obj);
                  if (content) {
                     result = strdup(content);
                  }
               }
               break;
            }
         }
      }
   }

   pthread_mutex_unlock(&session->history_mutex);

   return result;
}

char *session_get_full_system_prompt(session_t *session) {
   if (session == NULL)
      return NULL;

   char *result = NULL;

   pthread_mutex_lock(&session->history_mutex);

   if (session->conversation_history != NULL) {
      /* First pass: count system messages + total content bytes so we
       * can size one allocation. */
      const int len = json_object_array_length(session->conversation_history);
      int sys_count = 0;
      size_t total_bytes = 0;
      for (int i = 0; i < len; i++) {
         struct json_object *msg = json_object_array_get_idx(session->conversation_history, i);
         struct json_object *role_obj = NULL;
         if (!json_object_object_get_ex(msg, "role", &role_obj))
            continue;
         const char *r = json_object_get_string(role_obj);
         if (r == NULL || strcmp(r, "system") != 0)
            continue;
         struct json_object *content_obj = NULL;
         if (!json_object_object_get_ex(msg, "content", &content_obj))
            continue;
         const char *content = json_object_get_string(content_obj);
         if (content == NULL)
            continue;
         total_bytes += strlen(content);
         /* Add a "\n\n" separator between adjacent system messages. */
         if (sys_count > 0)
            total_bytes += 2;
         sys_count++;
      }

      if (sys_count > 0) {
         result = malloc(total_bytes + 1);
         if (result != NULL) {
            size_t off = 0;
            int written = 0;
            for (int i = 0; i < len; i++) {
               struct json_object *msg = json_object_array_get_idx(session->conversation_history,
                                                                   i);
               struct json_object *role_obj = NULL;
               if (!json_object_object_get_ex(msg, "role", &role_obj))
                  continue;
               const char *r = json_object_get_string(role_obj);
               if (r == NULL || strcmp(r, "system") != 0)
                  continue;
               struct json_object *content_obj = NULL;
               if (!json_object_object_get_ex(msg, "content", &content_obj))
                  continue;
               const char *content = json_object_get_string(content_obj);
               if (content == NULL)
                  continue;
               if (written > 0) {
                  result[off++] = '\n';
                  result[off++] = '\n';
               }
               const size_t clen = strlen(content);
               memcpy(result + off, content, clen);
               off += clen;
               written++;
            }
            result[off] = '\0';
         }
      }
   }

   pthread_mutex_unlock(&session->history_mutex);

   return result;
}

char *session_get_last_message_content(session_t *session, const char *role) {
   if (!session || !role) {
      return NULL;
   }

   char *result = NULL;
   pthread_mutex_lock(&session->history_mutex);

   if (session->conversation_history) {
      int len = json_object_array_length(session->conversation_history);
      for (int i = len - 1; i >= 0; i--) {
         struct json_object *msg = json_object_array_get_idx(session->conversation_history, i);
         struct json_object *role_obj = NULL;
         if (!json_object_object_get_ex(msg, "role", &role_obj)) {
            continue;
         }
         const char *r = json_object_get_string(role_obj);
         if (!r || strcmp(r, role) != 0) {
            continue;
         }
         /* Only handle plain-string content here.  Multi-part vision
          * messages store an array; callers that care about those
          * should walk the array themselves. */
         struct json_object *content_obj = NULL;
         if (json_object_object_get_ex(msg, "content", &content_obj) && content_obj &&
             json_object_is_type(content_obj, json_type_string)) {
            const char *content = json_object_get_string(content_obj);
            if (content) {
               result = strdup(content);
            }
         }
         break;
      }
   }

   pthread_mutex_unlock(&session->history_mutex);
   return result;
}

bool session_replace_last_message_content(session_t *session,
                                          const char *role,
                                          const char *new_content) {
   if (!session || !role || !new_content) {
      return false;
   }

   bool replaced = false;
   pthread_mutex_lock(&session->history_mutex);

   if (session->conversation_history) {
      int len = json_object_array_length(session->conversation_history);
      for (int i = len - 1; i >= 0; i--) {
         struct json_object *msg = json_object_array_get_idx(session->conversation_history, i);
         struct json_object *role_obj = NULL;
         if (!json_object_object_get_ex(msg, "role", &role_obj)) {
            continue;
         }
         const char *r = json_object_get_string(role_obj);
         if (!r || strcmp(r, role) != 0) {
            continue;
         }
         /* Only mutate plain-string content; refuse to overwrite
          * multi-part array content (would silently drop attached
          * images). */
         struct json_object *content_obj = NULL;
         if (json_object_object_get_ex(msg, "content", &content_obj) && content_obj &&
             json_object_is_type(content_obj, json_type_string)) {
            json_object_object_del(msg, "content");
            json_object_object_add(msg, "content", json_object_new_string(new_content));
            replaced = true;
         }
         break;
      }
   }

   pthread_mutex_unlock(&session->history_mutex);
   return replaced;
}

// =============================================================================
// System Prompt Refresh (broadcast on capability change)
// =============================================================================

/*
 * Prompt-builder callbacks registered by higher layers. Kept as callbacks so
 * session_manager (Layer 1) doesn't pull in webui/ (Layer 4) or reach up into
 * dawn.c for command_processing_mode. Atomic pointers ensure a cross-thread
 * publication from init thread (main) is visible to callers on MQTT /
 * worker threads without stale-NULL reads on weakly ordered architectures.
 */
/* Phase 1e: structured per-user builder replaces the legacy
 * session_user_prompt_builder_t (int) → char *.  Local-mic builder
 * is unchanged — local-mic SESSION_TYPE_LOCAL focus injection is
 * a follow-up phase.
 *
 * Atomic pointers ensure cross-thread publication from init thread
 * (main) is visible to refresh callers on MQTT / worker threads
 * without stale-NULL reads on weakly ordered architectures. */
static _Atomic(session_prompt_builder_t) s_prompt_builder = NULL;
static _Atomic(session_local_prompt_builder_t) s_local_prompt_builder = NULL;

void session_manager_set_prompt_builder(session_prompt_builder_t fn) {
   atomic_store_explicit(&s_prompt_builder, fn, memory_order_release);
}

void session_manager_set_local_prompt_builder(session_local_prompt_builder_t fn) {
   atomic_store_explicit(&s_local_prompt_builder, fn, memory_order_release);
}

/* =============================================================================
 * Phase 1e: composed_prompt_t lifecycle + composer
 *
 * Implementation extracted to src/core/prompt_compose.c so the composer
 * can be unit-tested without dragging the full session-manager runtime
 * (auth_db, conv_db, satellite_db, ...) into the test link.  The
 * session_manager-namespaced functions below are thin wrappers that
 * forward to the extracted helpers — public API is unchanged.
 * ============================================================================= */

void composed_prompt_free(composed_prompt_t *p) {
   prompt_compose_free(p);
}

char *session_manager_compose_prompt_string(const composed_prompt_t *blocks) {
   return prompt_compose_to_string(blocks);
}

/* Run the registered builder with kind=SESSION_START and flatten.
 * Convenience wrapper for capability-change refresh callers that need
 * a `char *` to hand to session_update_system_prompt — preserves the
 * legacy `build_user_prompt(int)` ownership semantics so the 9
 * existing call sites need a one-line rename rather than a full
 * composed_prompt_t lifecycle dance. */
char *session_manager_build_system_prompt_string(int user_id) {
   session_prompt_builder_t builder = atomic_load_explicit(&s_prompt_builder, memory_order_acquire);
   if (builder == NULL)
      return NULL;

   composed_prompt_t cp = { 0 };
   if (builder(user_id, /*user_turn_text*/ NULL, PROMPT_REFRESH_SESSION_START, &cp) != SUCCESS) {
      composed_prompt_free(&cp);
      return NULL;
   }
   /* Capability-change refresh path wants a single flattened string —
    * its callers go through the legacy `session_update_system_prompt`
    * single-message API.  The cache-eligible per-turn path uses
    * `session_update_system_messages` instead so messages[0] stays
    * stable across turns. */
   char *flat = session_manager_compose_prompt_string(&cp);
   composed_prompt_free(&cp);
   return flat;
}

/* =============================================================================
 * Phase 1e: per-turn refresh dispatch
 * ============================================================================= */

int session_dispatch_user_turn(session_t *session, const char *user_turn_text) {
   /* Phase 1f defense: erase any stale dispatch-session pointer before
    * doing anything else.  Today's only writers (the four explicit
    * clear-on-return paths in this function) cover all return points,
    * but a future signal handler / pthread_cancel / setjmp unwind path
    * could leave the slot non-NULL for the next dispatch on this
    * thread.  This belt-and-suspenders clear makes the setter
    * idempotent against stale state — security audit MEDIUM. */
   session_set_dispatch_session(NULL);

   if (session == NULL || user_turn_text == NULL)
      return SUCCESS;

   session_prompt_builder_t builder = atomic_load_explicit(&s_prompt_builder, memory_order_acquire);
   if (builder == NULL)
      return SUCCESS; /* No builder registered — leave system prompt as-is. */

   /* Re-read user_id under metrics_mutex so satellite rebinds that
    * occurred since last turn are picked up.  Session manager design
    * pattern (matches refresh_all_prompts at line 1700-1702). */
   pthread_mutex_lock(&session->metrics_mutex);
   int user_id = session->metrics.user_id;
   pthread_mutex_unlock(&session->metrics_mutex);
   /* The LOCAL mic session carries no authenticated user_id (metrics.user_id
    * stays 0); its memory is bound to the configured default voice user —
    * resolved the same way as the extraction / voice-conversation-save path
    * (session_save_voice_conversation, get_current_user_id).  This is what
    * gives the local mic the same per-turn memory + focus injection every
    * other surface already gets. */
   if (user_id <= 0 && session->type == SESSION_TYPE_LOCAL) {
      user_id = g_config.memory.default_voice_user_id > 0 ? g_config.memory.default_voice_user_id
                                                          : 1;
   }
   if (user_id <= 0)
      return SUCCESS; /* Unauthenticated — base prompt only; nothing to refresh. */

   /* Phase 1f: publish the dispatch session into TLS so build_focus_block
    * can find the dedup set without needing session plumbed through the
    * builder typedef.  Cleared on every return path below; the worker
    * thread's TLS slot returns to NULL before the next dispatch.  Order
    * matters: set BEFORE the builder call, clear AFTER all uses (system-
    * prompt swap completes in the success path). */
   session_set_dispatch_session(session);

   composed_prompt_t cp = { 0 };
   const int rc = builder(user_id, user_turn_text, PROMPT_REFRESH_PER_TURN, &cp);
   if (rc != SUCCESS) {
      /* Builder failure — focus_block guaranteed NULL by builder
       * contract.  Skip the system-prompt swap entirely; LLM dispatch
       * proceeds with last-good system prompt.  The previous turn's
       * focus_block content NEVER leaks into this turn because the
       * last-good prompt is whatever was set at session start (no
       * stale focus from a prior PER_TURN call). */
      OLOG_WARNING("session_dispatch_user_turn: builder failed (user_id=%d, kind=PER_TURN) — "
                   "skipping system-prompt swap, LLM dispatch proceeds with last-good prompt",
                   user_id);
      composed_prompt_free(&cp);
      session_set_dispatch_session(NULL);
      return SUCCESS; /* LLM dispatch never blocked by focus errors. */
   }

   /* Push the two-segment shape: messages[0]=stable, messages[1]=volatile.
    * Anthropic attaches cache_control to messages[0] in llm_claude_format;
    * the volatile message[1] is never cached.  OpenAI Responses
    * concatenates the two via extract_system_instructions; chat-completions
    * tolerates two consecutive system messages.
    *
    * DAP2 Room/HomeAssistant_Area suffix has already been baked INTO
    * cp.stable_prefix by dawn_build_prompt (via append_satellite_
    * context_to_stable, gated on the dispatch session's type).  No
    * post-rebuild append here — doing so would mutate the cached
    * prefix after the drift hash was computed, hiding mid-session
    * ha_area changes from the drift-log signal (architecture review
    * 2026-05-28).  Session-START / refresh-all-prompts paths still
    * use session_append_satellite_context directly; only the per-turn
    * dispatch path now relies on the in-builder append. */
   session_update_system_messages(session, cp.stable_prefix, cp.volatile_block);
   composed_prompt_free(&cp);

   session_set_dispatch_session(NULL);
   return SUCCESS;
}

void session_manager_refresh_all_prompts(void) {
   if (!initialized) {
      return;
   }

   /* Snapshot only session IDs under the read lock. We intentionally do NOT
    * snapshot user_id here — on a satellite rebind the session object can be
    * reassigned to a different user between snapshot and apply, and using a
    * stale user_id would cause user A's persona + memory context to be
    * written into user B's session. Instead, user_id is re-read under
    * metrics_mutex during apply (after session_get() retains the session). */
   uint32_t snapshot_ids[MAX_SESSIONS];
   int count = 0;

   pthread_rwlock_rdlock(&session_manager_rwlock);
   for (int i = 0; i < MAX_SESSIONS; i++) {
      session_t *s = sessions[i];
      if (!s) {
         continue;
      }
      snapshot_ids[count++] = s->session_id;
   }
   pthread_rwlock_unlock(&session_manager_rwlock);

   session_local_prompt_builder_t local_builder = atomic_load_explicit(&s_local_prompt_builder,
                                                                       memory_order_acquire);
   /* Phase 1e: structured builder.  Loaded inside the loop on demand
    * via session_manager_build_system_prompt_string() so each session's
    * SESSION_START rebuild picks up the per-user persona/memory blocks
    * and the (currently-empty for SESSION_START) focus block. */

   int refreshed = 0;
   for (int i = 0; i < count; i++) {
      session_t *s = session_get(snapshot_ids[i]); /* Retains */
      if (!s) {
         continue;
      }

      /* Re-read user_id and type under the session's own locks so rebinds
       * that have occurred since the snapshot are picked up. */
      session_type_t type = s->type;
      pthread_mutex_lock(&s->metrics_mutex);
      int user_id = s->metrics.user_id;
      pthread_mutex_unlock(&s->metrics_mutex);

      const char *static_prompt = NULL;
      char *owned_prompt = NULL;

      if (type == SESSION_TYPE_LOCAL) {
         /* Local mic session — delegate to dawn.c so the prompt matches the
          * active command_processing_mode (direct-only vs LLM).  Local-mic
          * focus injection is a follow-up phase; SESSION_TYPE_LOCAL stays
          * on the legacy single-string peer. */
         static_prompt = local_builder ? local_builder() : get_local_command_prompt();
      } else if (user_id > 0) {
         /* Authenticated WebUI / satellite — rebuild with per-user persona
          * via the structured builder (PER_TURN focus block is empty for
          * a capability-change refresh — this is a SESSION_START path).
          *
          * Phase 1f: clear the session's focus-injection dedup state
          * BEFORE invoking the builder so the next PER_TURN admits all
          * candidates fresh.  The builder itself stays kind-agnostic —
          * the clear is the session_manager's responsibility, not the
          * builder's.  Self-locking; safe under read-lock. */
         session_injected_set_clear(s);
         owned_prompt = session_manager_build_system_prompt_string(user_id);
         if (owned_prompt == NULL) {
            /* Builder unregistered or failed — fall back to base remote
             * prompt so the session keeps a usable system message. */
            static_prompt = get_remote_command_prompt();
         }
      } else {
         /* Unauthenticated remote or WebUI not registered — base remote prompt */
         static_prompt = get_remote_command_prompt();
      }

      const char *use_prompt = owned_prompt ? owned_prompt : static_prompt;
      if (use_prompt) {
         session_update_system_prompt(s, use_prompt);

         /* DAP2 satellites originally have room + HA area appended after
          * registration via session_append_satellite_context(). Refresh
          * replaces the whole prompt, so we must re-apply that suffix or
          * Home Assistant commands lose their area scoping. The append
          * helper is a no-op if the suffix is already present. */
         if (type == SESSION_TYPE_DAP2 && s->identity.uuid[0] != '\0') {
            satellite_mapping_t mapping;
            if (satellite_db_get(s->identity.uuid, &mapping) == 0) {
               session_append_satellite_context(s, s->identity.location, mapping.ha_area);
            } else {
               /* No mapping row — still re-append room from identity if set */
               session_append_satellite_context(s, s->identity.location, NULL);
            }
         }

         refreshed++;
      }

      free(owned_prompt);
      session_release(s);
   }

   if (refreshed > 0) {
      OLOG_INFO("Refreshed system prompt on %d active session(s)", refreshed);
   }
}


// =============================================================================
// Per-Session LLM Configuration
// =============================================================================

int session_set_llm_config(session_t *session, const session_llm_config_t *config) {
   if (!session || !config) {
      return 1;
   }

   // Validate that requested provider has API key; fall back to available provider
   session_llm_config_t fallback_config;
   if (config->type == LLM_CLOUD) {
      bool has_key = false;
      if (config->cloud_provider == CLOUD_PROVIDER_OPENAI)
         has_key = llm_has_openai_key();
      else if (config->cloud_provider == CLOUD_PROVIDER_CLAUDE)
         has_key = llm_has_claude_key();
      else if (config->cloud_provider == CLOUD_PROVIDER_GEMINI)
         has_key = llm_has_gemini_key();
      else if (config->cloud_provider == CLOUD_PROVIDER_OPENROUTER)
         has_key = llm_has_openrouter_key();

      if (!has_key) {
         // Try to fall back to an available provider
         cloud_provider_t fallback = llm_detect_available_provider();

         if (fallback == CLOUD_PROVIDER_NONE) {
            OLOG_WARNING("Session %u: No cloud provider has an API key configured",
                         session->session_id);
            return 1;
         }

         OLOG_INFO("Session %u: %s provider unavailable, falling back to %s", session->session_id,
                   cloud_provider_to_string(config->cloud_provider),
                   cloud_provider_to_string(fallback));
         memcpy(&fallback_config, config, sizeof(session_llm_config_t));
         fallback_config.cloud_provider = fallback;
         fallback_config.model[0] =
             '\0'; /* Clear model — let resolver pick default for new provider */
         config = &fallback_config;
      }
   }

   pthread_mutex_lock(&session->llm_config_mutex);
   memcpy(&session->llm_config, config, sizeof(session_llm_config_t));
   pthread_mutex_unlock(&session->llm_config_mutex);

   OLOG_INFO("Session %u: LLM config updated (type=%d, provider=%d)", session->session_id,
             config->type, config->cloud_provider);

   return 0;
}

void session_get_llm_config(session_t *session, session_llm_config_t *config) {
   if (!session || !config) {
      return;
   }

   pthread_mutex_lock(&session->llm_config_mutex);
   memcpy(config, &session->llm_config, sizeof(session_llm_config_t));
   pthread_mutex_unlock(&session->llm_config_mutex);
}

void session_clear_llm_config(session_t *session) {
   if (!session) {
      return;
   }

   pthread_mutex_lock(&session->llm_config_mutex);
   llm_get_default_config(&session->llm_config);
   pthread_mutex_unlock(&session->llm_config_mutex);

   OLOG_INFO("Session %u: LLM config reset to defaults", session->session_id);
}

// =============================================================================
// Utility Functions
// =============================================================================

void session_touch(session_t *session) {
   if (!session) {
      return;
   }
   /* last_activity is _Atomic — a lock-free atomic store is sufficient (and this
    * runs on every WS callback, so the old history_mutex round-trip was both
    * redundant and on a hot path).  The reader (session_cleanup_expired) holds a
    * different lock, so the atomicity — not a shared mutex — is what makes them
    * race-free. */
   atomic_store(&session->last_activity, time(NULL));
}

int session_count(void) {
   if (!initialized) {
      return 0;
   }

   pthread_rwlock_rdlock(&session_manager_rwlock);

   int count = 0;
   for (int i = 0; i < MAX_SESSIONS; i++) {
      if (sessions[i] != NULL) {
         count++;
      }
   }

   pthread_rwlock_unlock(&session_manager_rwlock);

   return count;
}

const char *session_type_name(session_type_t type) {
   switch (type) {
      case SESSION_TYPE_LOCAL:
         return "LOCAL";
      case SESSION_TYPE_DAP:
         return "DAP";
      case SESSION_TYPE_DAP2:
         return "DAP2";
      case SESSION_TYPE_WEBUI:
         return "WEBUI";
      case SESSION_TYPE_MESSAGING:
         return "MESSAGING";
      case SESSION_TYPE_JOB:
         return "JOB";
      default:
         return "UNKNOWN";
   }
}

// =============================================================================
// Command Context (Thread-Local)
// =============================================================================

void session_set_command_context(session_t *session) {
   /* Debug logging for context transitions - helps trace command routing issues */
#ifdef DEBUG_COMMAND_CONTEXT
   if (session) {
      OLOG_INFO("Command context set: session %u (%s)", session->session_id,
                session_type_name(session->type));
   } else if (tl_command_context) {
      OLOG_INFO("Command context cleared (was session %u)", tl_command_context->session_id);
   }
#endif
   tl_command_context = session;
}

session_t *session_get_command_context(void) {
   return tl_command_context;
}

// =============================================================================
// Per-Session Metrics
// =============================================================================

/**
 * @brief Find or create provider entry in session metrics
 *
 * @param session Session to search
 * @param provider Provider name ("openai", "claude", "local")
 * @return Pointer to provider entry, or NULL if full
 *
 * @note Caller must hold session->metrics_mutex
 */
static session_provider_tokens_t *find_or_create_provider(session_t *session,
                                                          const char *provider) {
   session_metrics_tracker_t *m = &session->metrics;

   // Search for existing provider
   for (int i = 0; i < m->provider_count; i++) {
      if (strcmp(m->providers[i].provider, provider) == 0) {
         return &m->providers[i];
      }
   }

   // Create new entry if space available
   if (m->provider_count < SESSION_MAX_PROVIDERS) {
      session_provider_tokens_t *p = &m->providers[m->provider_count++];
      strncpy(p->provider, provider, SESSION_PROVIDER_MAX - 1);
      p->provider[SESSION_PROVIDER_MAX - 1] = '\0';
      return p;
   }

   OLOG_WARNING("Session %u: Max providers (%d) reached, can't add '%s'", session->session_id,
                SESSION_MAX_PROVIDERS, provider);
   return NULL;
}

/**
 * @brief Persist session metrics to database
 *
 * Uses UPSERT pattern: INSERT on first call, UPDATE on subsequent calls.
 *
 * @param session Session with metrics to save
 *
 * @note Caller must hold session->metrics_mutex
 */
static void persist_session_metrics(session_t *session) {
   if (!auth_db_is_ready()) {
      return;
   }

   session_metrics_tracker_t *m = &session->metrics;

   // Build session_metrics_t for database
   session_metrics_t db_metrics = { 0 };
   db_metrics.id = m->db_id;  // -1 for INSERT, >0 for UPDATE
   db_metrics.session_id = session->session_id;
   db_metrics.user_id = m->user_id;
   strncpy(db_metrics.session_type, session_type_name(session->type),
           sizeof(db_metrics.session_type) - 1);
   db_metrics.started_at = session->created_at;
   db_metrics.ended_at = time(NULL);

   db_metrics.queries_total = m->queries_total;
   db_metrics.queries_cloud = m->queries_cloud;
   db_metrics.queries_local = m->queries_local;
   db_metrics.errors_count = m->errors_count;
   db_metrics.fallbacks_count = m->fallbacks_count;

   // Calculate averages from sums
   if (m->perf_sample_count > 0) {
      db_metrics.avg_asr_ms = m->asr_ms_sum / m->perf_sample_count;
      db_metrics.avg_llm_ttft_ms = m->llm_ttft_ms_sum / m->perf_sample_count;
      db_metrics.avg_llm_total_ms = m->llm_total_ms_sum / m->perf_sample_count;
      db_metrics.avg_tts_ms = m->tts_ms_sum / m->perf_sample_count;
      db_metrics.avg_pipeline_ms = m->pipeline_ms_sum / m->perf_sample_count;
   }

   // Save to database (INSERT or UPDATE based on db_metrics.id)
   if (auth_db_save_session_metrics(&db_metrics) == AUTH_DB_SUCCESS) {
      // Store returned ID for subsequent UPDATEs
      if (m->db_id < 0) {
         m->db_id = db_metrics.id;
         OLOG_INFO("Session %u: Created metrics row (id=%lld)", session->session_id,
                   (long long)m->db_id);
      }

      // Save per-provider metrics (delete existing + re-insert)
      if (m->provider_count > 0 && m->db_id > 0) {
         session_provider_metrics_t providers[SESSION_MAX_PROVIDERS];
         for (int i = 0; i < m->provider_count; i++) {
            providers[i].session_metrics_id = m->db_id;
            strncpy(providers[i].provider, m->providers[i].provider, CLOUD_PROVIDER_MAX - 1);
            providers[i].provider[CLOUD_PROVIDER_MAX - 1] = '\0';
            providers[i].tokens_input = m->providers[i].tokens_input;
            providers[i].tokens_output = m->providers[i].tokens_output;
            providers[i].tokens_cached = m->providers[i].tokens_cached;
            providers[i].queries = m->providers[i].queries;
         }
         auth_db_save_provider_metrics(m->db_id, providers, m->provider_count);
      }
   }
}

void session_record_query(session_t *session,
                          const char *provider,
                          uint64_t tokens_in,
                          uint64_t tokens_out,
                          uint64_t tokens_cached,
                          double llm_ttft_ms,
                          double llm_total_ms,
                          bool is_error) {
   if (!session || !provider) {
      return;
   }

   pthread_mutex_lock(&session->metrics_mutex);

   session_metrics_tracker_t *m = &session->metrics;

   // Update query counts
   m->queries_total++;
   if (strcmp(provider, "local") == 0) {
      m->queries_local++;
   } else {
      m->queries_cloud++;
   }
   if (is_error) {
      m->errors_count++;
   }

   // Update per-provider token tracking
   session_provider_tokens_t *p = find_or_create_provider(session, provider);
   if (p) {
      p->tokens_input += tokens_in;
      p->tokens_output += tokens_out;
      p->tokens_cached += tokens_cached;
      p->queries++;
   }

   // Update LLM performance sums
   m->llm_ttft_ms_sum += llm_ttft_ms;
   m->llm_total_ms_sum += llm_total_ms;
   m->perf_sample_count++;

   // Persist to database
   persist_session_metrics(session);

   pthread_mutex_unlock(&session->metrics_mutex);
}

void session_record_asr_timing(session_t *session, double asr_ms) {
   if (!session) {
      return;
   }

   pthread_mutex_lock(&session->metrics_mutex);
   session->metrics.asr_ms_sum += asr_ms;
   pthread_mutex_unlock(&session->metrics_mutex);
}

void session_record_tts_timing(session_t *session, double tts_ms) {
   if (!session) {
      return;
   }

   pthread_mutex_lock(&session->metrics_mutex);
   session->metrics.tts_ms_sum += tts_ms;
   pthread_mutex_unlock(&session->metrics_mutex);
}

void session_record_pipeline_timing(session_t *session, double pipeline_ms) {
   if (!session) {
      return;
   }

   pthread_mutex_lock(&session->metrics_mutex);
   session->metrics.pipeline_ms_sum += pipeline_ms;
   pthread_mutex_unlock(&session->metrics_mutex);
}

void session_set_metrics_user(session_t *session, int user_id) {
   if (!session) {
      return;
   }

   pthread_mutex_lock(&session->metrics_mutex);
   session->metrics.user_id = user_id;
   pthread_mutex_unlock(&session->metrics_mutex);
}

/* Note: File-based history saving (session_manager_save_all_histories) has been removed.
 * WebUI sessions persist to auth.db (conversations/messages tables) during the session.
 * LOCAL/DAP sessions do not persist conversation history.
 * WebUI export (JSON/HTML) is available via conversation history panel. */
