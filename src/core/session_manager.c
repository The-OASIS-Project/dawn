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
#include "core/focus/focus_handles.h"
#include "core/focus/focus_incremental.h"
#include "core/session_compaction.h"
#include "core/session_focus.h"
#include "core/session_prefix.h"
#include "core/session_reaper.h"
#include "core/tool_result_store.h"
#include "core/turn_queue.h"
#include "dawn_error.h"
#include "llm/llm_command_parser.h"
#include "llm/llm_history_kind.h"
#include "llm/llm_interface.h"
#include "llm/llm_tools.h"
#include "llm/llm_turn_blocks.h"
#include "logging.h"
#include "memory/memory_extraction.h"
#include "utils/string_utils.h"
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
 *   - Main thread: for local voice tool/direct-regex execution (dawn.c)
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
 *   - Local voice: main thread sets context, runs native tool / direct-regex
 *     execution, callbacks execute in main thread, context cleared
 *   - WebUI/DAP: MQTT on_message() callback sets context, executes device callback,
 *     clears context - all in the single MQTT callback thread
 *
 *   Mosquitto must NOT be configured with MOSQ_OPT_THREADED or multiple event loops,
 *   as this would cause context to be set in one thread and read in another.
 */
static __thread session_t *tl_command_context = NULL;

/* The running turn this thread works for (see session_turn_token()), and LLM
 * settings pinned for work that outlives its turn. */
static __thread uint64_t tl_turn_token = 0;
static __thread const session_t *tl_llm_override_session = NULL;
static __thread const session_llm_config_t *tl_llm_override = NULL;

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

   /* Its compaction first: a worker still summarizing (shutdown frees without
    * session_destroy) is cancelled and joined, and what it held freed. */
   session_compaction_teardown(session);

   // Free conversation history (and any turn state a torn-down turn left behind)
   if (session->turn_history) {
      json_object_put(session->turn_history);
      session->turn_history = NULL;
   }
   if (session->turn_reply) {
      json_object_put(session->turn_reply);
      session->turn_reply = NULL;
   }
   if (session->turn_reply_mirror) {
      json_object_put(session->turn_reply_mirror);
      session->turn_reply_mirror = NULL;
   }
   focus_handles_free(session->focus_handles);
   session->focus_handles = NULL;
   free(session->citation.prior);
   session->citation.prior = NULL;
   session->citation.prior_count = 0;
   tool_result_store_free(session);
   session_prefix_turn_free(session->prefix_turn);
   session->prefix_turn = NULL;
   json_object_put(session->withdraw_pending);
   session->withdraw_pending = NULL;
   free(session->turn_pending_user);
   session->turn_pending_user = NULL;
   free(session->turn_pending_reply);
   session->turn_pending_reply = NULL;
   free(session->turn_attached);
   session->turn_attached = NULL;
   free(session->unclaimed_user);
   session->unclaimed_user = NULL;
   free(session->unclaimed_reply);
   session->unclaimed_reply = NULL;
   free(session->turn_prior_user);
   session->turn_prior_user = NULL;
   free(session->turn_prior_reply);
   session->turn_prior_reply = NULL;
   json_object_put(session->unclaimed_reply_blocks);
   session->unclaimed_reply_blocks = NULL;
   json_object_put(session->turn_prior_reply_blocks);
   session->turn_prior_reply_blocks = NULL;
   for (int i = 0; i < session->parked_msg_count; i++) {
      json_object_put(session->parked_msgs[i]);
   }
   session->parked_msg_count = 0;
   struct json_object **held[] = { &session->turn_user_msg, &session->unclaimed_user_msg,
                                   &session->unclaimed_reply_msg, &session->claimed_user_msg,
                                   &session->claimed_reply_msg };
   for (size_t i = 0; i < sizeof(held) / sizeof(held[0]); i++) {
      if (*held[i]) {
         json_object_put(*held[i]);
         *held[i] = NULL;
      }
   }
   if (session->conversation_history) {
      json_object_put(session->conversation_history);
      session->conversation_history = NULL;
   }

   // Free pending visual content
   free(session->pending_visual);
   session->pending_visual = NULL;

   // Free any unconsumed cancel-at-buzzer stash (SERVER_AUTHORITATIVE §9/G4)
   free(session->cancelled_final_response);
   session->cancelled_final_response = NULL;

   // Free any unconsumed final-answer reasoning stash (SERVER_AUTHORITATIVE §6c-G1)
   session_final_answer_clear(session);


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

   /* Destroys finish on the reaper, never on their caller's thread (the
    * WebUI service thread among them): no reaper, no session manager. */
   if (session_reaper_start() != 0) {
      session_free(local);
      return 1;
   }
   sessions[0] = local;
   initialized = true;

   OLOG_INFO("Session manager initialized with local session");
   return 0;
}

void session_manager_cleanup(void) {
   if (!initialized) {
      return;
   }

   /* Shutdown: abort any in-flight operations AND gate emission (both flags). */
   session_t *going[MAX_SESSIONS] = { 0 };
   pthread_rwlock_wrlock(&session_manager_rwlock);
   for (int i = 0; i < MAX_SESSIONS; i++) {
      going[i] = sessions[i];
      if (going[i] != NULL) {
         session_teardown_flags(going[i]);
      }
   }
   pthread_rwlock_unlock(&session_manager_rwlock);

   /* For the LOCAL session, wait for ref_count to reach 1 (workers may be using
    * it, and may look sessions up meanwhile: no manager lock is held while
    * waiting).  WebSocket/DAP sessions are freed regardless (ref_count is for
    * reconnection support); session_free joins what still runs on them. */
   for (int i = 0; i < MAX_SESSIONS; i++) {
      if (going[i] != NULL && going[i]->type == SESSION_TYPE_LOCAL) {
         pthread_mutex_lock(&going[i]->ref_mutex);
         while (going[i]->ref_count > 1) {
            pthread_cond_wait(&going[i]->ref_zero_cond, &going[i]->ref_mutex);
         }
         pthread_mutex_unlock(&going[i]->ref_mutex);
      }
   }

   /* Sessions destroyed and not yet finished: the WebUI is down, so their
    * connections have released what they held. */
   session_reaper_stop();

   pthread_rwlock_wrlock(&session_manager_rwlock);
   for (int i = 0; i < MAX_SESSIONS; i++) {
      sessions[i] = NULL;
   }
   initialized = false;
   pthread_rwlock_unlock(&session_manager_rwlock);

   for (int i = 0; i < MAX_SESSIONS; i++) {
      if (going[i] != NULL) {
         OLOG_INFO("Destroying session %u (type=%s)", going[i]->session_id,
                   session_type_name(going[i]->type));
         session_free(going[i]);
      }
   }

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

   /* A new context: its first turn freezes the prompt it runs under, and the
    * satellite's room reaches it as a standing direction. */
   session_clear_history(session);

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
   safe_strscpy(session->client_ip, client_ip);

   sessions[slot] = session;

   pthread_rwlock_unlock(&session_manager_rwlock);

   // A new context: its first turn freezes the prompt it runs under.
   session_clear_history(session);

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
   /* A destroyed session's last reference: the reaper finishes it.  Decided
    * under the lock; the session isn't touched after the unlock (the reaper
    * may free it at once). */
   const bool last_of_destroyed = session->ref_count <= 0 && atomic_load(&session->being_destroyed);

   /* Every release: a waiter may be waiting for a count other than 0 (shutdown
    * waits for the local session's to reach 1). */
   pthread_cond_broadcast(&session->ref_zero_cond);

   pthread_mutex_unlock(&session->ref_mutex);
   if (last_of_destroyed) {
      session_reaper_wake();
   }
}

/* A usable owner key: at least SESSION_OWNER_KEY_LEN characters. */
static bool owner_key_given(const char *key) {
   return key && strlen(key) >= SESSION_OWNER_KEY_LEN;
}

bool session_set_owner(session_t *session, const char *key) {
   if (!session || !owner_key_given(key)) {
      return false;
   }
   pthread_rwlock_wrlock(&session_manager_rwlock);
   bool owns;
   if (session->owner_key[0] == '\0') {
      memcpy(session->owner_key, key, SESSION_OWNER_KEY_LEN);
      session->owner_key[SESSION_OWNER_KEY_LEN] = '\0';
      owns = true;
   } else {
      owns = memcmp(session->owner_key, key, SESSION_OWNER_KEY_LEN) == 0;
   }
   pthread_rwlock_unlock(&session_manager_rwlock);
   return owns;
}

bool session_owner_matches(session_t *session, const char *key) {
   if (!session) {
      return false;
   }
   pthread_rwlock_rdlock(&session_manager_rwlock);
   const bool matches = owner_key_given(key)
                            ? session->owner_key[0] != '\0' &&
                                  memcmp(session->owner_key, key, SESSION_OWNER_KEY_LEN) == 0
                            : session->owner_key[0] == '\0';
   pthread_rwlock_unlock(&session_manager_rwlock);
   return matches;
}

bool session_owner_is(session_t *session, const char *key) {
   return owner_key_given(key) && session_owner_matches(session, key);
}

int session_manager_list_owned(session_owned_t *out, int max) {
   if (!initialized || !out || max <= 0) {
      return 0;
   }
   int n = 0;
   pthread_rwlock_rdlock(&session_manager_rwlock);
   for (int i = 0; i < MAX_SESSIONS && n < max; i++) {
      session_t *s = sessions[i];
      if (s != NULL && s->owner_key[0] != '\0') {
         out[n].session_id = s->session_id;
         memcpy(out[n].owner_key, s->owner_key, sizeof(out[n].owner_key));
         n++;
      }
   }
   pthread_rwlock_unlock(&session_manager_rwlock);
   return n;
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

   /* Close the session's turn queue: no new turns enqueue, the in-flight
    * turn's turn_done won't chain a successor, and every already-QUEUED turn is
    * dropped — each holds a session retain, so the reaper's wait for the last
    * reference can finish. */
   turn_queue_purge_session(session_id);

   /* Detach its WebUI connections: their reconnect tokens go, each attached
    * connection's reference is released, and a music socket bound to it is
    * asked to close (on the music thread, which releases that reference). */
#ifdef ENABLE_WEBUI
   webui_detach_session(session);
#endif

   /* The rest waits for the session's workers and its last reference, which
    * a caller here may be the one to release (the WebUI service thread for a
    * connection it has yet to close): the reaper finishes it. */
   session_reaper_enqueue(session);
}

void session_manager_finalize(session_t *session) {
   const uint32_t session_id = session->session_id;

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
      safe_strscpy(db_metrics.session_type, session_type_name(session->type));
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
       g_config.memory.enabled && auth_db_is_ready()) {
      /* Copy the history and what it holds in one critical section, then work
       * outside the lock (extraction does DB lookups). */
      int64_t conv_id = 0;
      int message_count = 0;
      struct json_object *clean = session_snapshot_history(session, &conv_id, &message_count);
      int duration_seconds = (int)(time(NULL) - session->created_at);

      /* Extract the conversation this history belongs to, by id, so
       * memory_trigger_extraction applies its privacy and background-job checks
       * and advances the incremental cursor.  A history spanning several
       * conversations can't be attributed to any one of them, so it is left to
       * memory_recovery, which extracts each (non-private) conversation from the
       * DB.  0 = no DB conversation: a satellite's history is never persisted, so
       * it can't be private; a WebUI or messaging history always belongs to a DB
       * conversation, so 0 there means it could not be attributed — skip it
       * (anything persisted is extracted by recovery). */
      const bool persisted_type = session->type == SESSION_TYPE_WEBUI ||
                                  session->type == SESSION_TYPE_MESSAGING;
      if (conv_id == SESSION_HISTORY_CONV_MIXED || (conv_id <= 0 && persisted_type)) {
         OLOG_INFO("Session %u: history not attributable to one conversation; leaving "
                   "memory extraction to recovery",
                   session->session_id);
      } else if (message_count > 2 && clean) {
         /* Create session ID string for the summary */
         char session_id_str[32];
         snprintf(session_id_str, sizeof(session_id_str), "ws_%u", session->session_id);

         /* The snapshot has provider state stripped (see
          * llm_history_strip_internal); extraction copies it again. */
         memory_extraction_fallback_t fb;
         memory_extraction_build_fallback(session, &fb);
         memory_trigger_extraction(session->metrics.user_id, conv_id, session_id_str, clean,
                                   message_count, duration_seconds, &fb);
      }
      if (clean) {
         json_object_put(clean);
      }
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
// System-message broadcast and per-turn tool hooks
// (conversation-history functions live in session_history.c)
// =============================================================================

int session_effective_user_id(session_t *session) {
   if (!session) {
      return 0;
   }
   pthread_mutex_lock(&session->metrics_mutex);
   const int user_id = session->metrics.user_id;
   pthread_mutex_unlock(&session->metrics_mutex);
   if (user_id > 0) {
      return user_id;
   }
   return session->type == SESSION_TYPE_LOCAL ? session_default_voice_user_id() : 0;
}

int session_default_voice_user_id(void) {
   return g_config.memory.default_voice_user_id > 0 ? g_config.memory.default_voice_user_id : 1;
}

/* The local device's requested owner, applied by the main loop between turns. */
static atomic_int s_local_owner_request = 0;
static atomic_bool s_local_owner_pending = false;

void session_request_local_owner(int user_id) {
   atomic_store(&s_local_owner_request, user_id > 0 ? user_id : 0);
   atomic_store(&s_local_owner_pending, true); /* after the value: a taker sees it */
}

bool session_take_local_owner(int *user_id_out) {
   if (!user_id_out || !atomic_exchange(&s_local_owner_pending, false)) {
      return false;
   }
   *user_id_out = atomic_load(&s_local_owner_request);
   return true;
}

/* session_broadcast_notice() / _for_user(): @p user_id 0 = every user. */
static int broadcast_notice(int user_id, const char *content) {
   if (!initialized || !content || content[0] == '\0') {
      return 0;
   }

   /* Snapshot session IDs under the read lock, then apply after releasing it —
    * the usual discipline: never hold a
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
       * excluded so a device event doesn't leak into an unrelated chat. */
      bool interactive = s->type == SESSION_TYPE_LOCAL || s->type == SESSION_TYPE_DAP ||
                         s->type == SESSION_TYPE_DAP2 || s->type == SESSION_TYPE_WEBUI;
      if (interactive && (user_id <= 0 || session_effective_user_id(s) == user_id)) {
         session_post_notice_for(s, content, user_id);
         delivered++;
      }

      session_release(s);
   }

   return delivered;
}

void session_manager_for_each_session_any(void (*fn)(session_t *session, void *ctx), void *ctx) {
   if (!initialized || !fn) {
      return;
   }
   /* Retain each session under the read lock (disconnected or not), then
    * visit them with it released. */
   session_t *snapshot[MAX_SESSIONS];
   int count = 0;
   pthread_rwlock_rdlock(&session_manager_rwlock);
   for (int i = 0; i < MAX_SESSIONS; i++) {
      if (sessions[i]) {
         pthread_mutex_lock(&sessions[i]->ref_mutex);
         sessions[i]->ref_count++;
         pthread_mutex_unlock(&sessions[i]->ref_mutex);
         snapshot[count++] = sessions[i];
      }
   }
   pthread_rwlock_unlock(&session_manager_rwlock);
   for (int i = 0; i < count; i++) {
      fn(snapshot[i], ctx);
      session_release(snapshot[i]);
   }
}

int session_broadcast_notice(const char *content) {
   return broadcast_notice(0, content);
}

int session_broadcast_notice_for_user(int user_id, const char *content) {
   return user_id > 0 ? broadcast_notice(user_id, content) : 0;
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

void session_update_interaction_complete(session_t *session) {
   if (!session) {
      return;
   }
   session->last_interaction_complete = time(NULL);
}

void session_init_system_prompt(session_t *session, const char *system_prompt) {
   if (!session || !system_prompt) {
      return;
   }
   pthread_mutex_lock(&session->history_mutex);
   /* A fresh system prompt is a SESSION_START boundary: a new context (it
    * shows no retrieved items, so the next turn sends every relevant one). */
   session_new_context_locked(session, system_prompt);
   pthread_mutex_unlock(&session->history_mutex);

   OLOG_INFO("Session %u: Initialized with system prompt (%zu chars)", session->session_id,
             strlen(system_prompt));
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

void session_manager_set_prompt_builder(session_prompt_builder_t fn) {
   atomic_store_explicit(&s_prompt_builder, fn, memory_order_release);
}

int session_dispatch_user_turn(session_t *session, const char *user_turn_text) {
   return session_dispatch_user_turn_ex(session, user_turn_text, NULL);
}

/* @p note with the note naming the email the running turn attached
 * (session_turn_attach_email) after it: heap, or NULL when there is neither
 * (or on allocation failure, logged: the turn then goes without them). */
static char *turn_note_with_email(session_t *session, const char *note) {
   pthread_mutex_lock(&session->history_mutex);
   char *email = session->turn_attached ? strdup(session->turn_attached) : NULL;
   const bool attached = session->turn_attached != NULL;
   pthread_mutex_unlock(&session->history_mutex);
   if (attached && !email) {
      OLOG_ERROR("Session %u: out of memory naming the attached email", session->session_id);
   }
   if (!email) {
      return note ? strdup(note) : NULL;
   }
   if (!note || !*note) {
      return email;
   }
   const size_t len = strlen(note) + 1 + strlen(email) + 1;
   char *both = malloc(len);
   if (both) {
      snprintf(both, len, "%s\n%s", note, email);
   } else {
      OLOG_ERROR("Session %u: out of memory naming the attached email", session->session_id);
   }
   free(email);
   return both;
}

static int dispatch_with_note(session_t *session, const char *user_turn_text, const char *note);

int session_dispatch_user_turn_ex(session_t *session,
                                  const char *user_turn_text,
                                  const char *turn_note) {
   if (session == NULL || user_turn_text == NULL)
      return SUCCESS;
   char *note = turn_note_with_email(session, turn_note);
   const int rc = dispatch_with_note(session, user_turn_text, note);
   free(note);
   return rc;
}

/* session_dispatch_user_turn_ex() with the turn's whole note (the channel's
 * and the attached email's). */
static int dispatch_with_note(session_t *session,
                              const char *user_turn_text,
                              const char *turn_note) {
   /* Memory citation signal: clear the per-turn [M#]->item_id stash at the start
    * of every dispatch.  The turn's seam sets it below iff citation is enabled
    * and this turn has memory items; clearing here means a turn without them
    * cannot inherit the previous turn's map. */
   session_citation_stash_clear(session);

   /* Reset the live <cited> stream-strip filter at the same turn boundary.  It
    * must be clean before this turn's first stream delta; resetting in
    * webui_send_stream_start would be too late (the strip runs before start is
    * triggered on first content) and could wipe a mid-turn held-back partial. */
   text_filter_cited_reset(&session->cited_tag_filter);

   session_prompt_builder_t builder = atomic_load_explicit(&s_prompt_builder, memory_order_acquire);
   if (builder == NULL) {
      /* No builder (a build without one): the base prompt is what a new
       * context freezes; a frozen one keeps its own. */
      composed_prompt_t base = { .stable_prefix = get_command_prompt_dup() };
      session_compaction_prepare(session, turn_note ? (int)(strlen(turn_note) / 4) : 0);
      session_prefix_apply_turn(session, base.stable_prefix ? &base : NULL, turn_note);
      composed_prompt_free(&base);
      return SUCCESS;
   }

   /* Re-read user_id under metrics_mutex so satellite rebinds that
    * occurred since last turn are picked up.  A guest (0: an unmapped
    * satellite, a local mic with no voice user) gets the base prompt, its
    * surface's standing directions and the time; the builder adds nothing of
    * any user's (settings, memory, retrieval are gated on a user). */
   const int user_id = session_effective_user_id(session);

   composed_prompt_t cp = { 0 };
   if (builder(session, user_id, user_turn_text, &cp) != SUCCESS) {
      /* The history keeps the prompt it has; the turn still gets its device
       * events and note.  Dispatch is never blocked by a builder failure. */
      OLOG_WARNING("session_dispatch_user_turn: builder failed (user_id=%d); the turn runs on "
                   "the prompt its history has",
                   user_id);
      composed_prompt_free(&cp);
      session_compaction_prepare(session, turn_note ? (int)(strlen(turn_note) / 4) : 0);
      session_prefix_apply_turn(session, NULL, turn_note);
      return SUCCESS;
   }

   /* A history this turn would take past its window is compacted at this
    * seam, sized with what the turn adds (session_compaction.h). */
   const size_t adds = (cp.context_head ? strlen(cp.context_head) : 0) +
                       focus_incremental_items_bytes(cp.focus_items, cp.n_focus_items) +
                       (cp.context_tail ? strlen(cp.context_tail) : 0) +
                       (cp.memory_body ? strlen(cp.memory_body) : 0) +
                       (cp.directives ? strlen(cp.directives) : 0) +
                       (turn_note ? strlen(turn_note) : 0);
   session_compaction_prepare(session, (int)(adds / 4));

   /* The conversation's frozen prompt, what changed appended after the
    * question, and the turn's context in front of it (session_prefix.h). */
   session_prefix_apply_turn(session, &cp, turn_note);
   composed_prompt_free(&cp);
   return SUCCESS;
}

// =============================================================================
// Per-Session LLM Configuration
// =============================================================================

/* @p config, or a copy in @p fallback with a cloud provider that has an API key
 * when @p config's doesn't; NULL when no cloud provider has one. */
static const session_llm_config_t *usable_llm_config(const session_t *session,
                                                     const session_llm_config_t *config,
                                                     session_llm_config_t *fallback) {
   if (config->type != LLM_CLOUD) {
      return config;
   }
   bool has_key = false;
   if (config->cloud_provider == CLOUD_PROVIDER_OPENAI)
      has_key = llm_has_openai_key();
   else if (config->cloud_provider == CLOUD_PROVIDER_CLAUDE)
      has_key = llm_has_claude_key();
   else if (config->cloud_provider == CLOUD_PROVIDER_GEMINI)
      has_key = llm_has_gemini_key();
   else if (config->cloud_provider == CLOUD_PROVIDER_OPENROUTER)
      has_key = llm_has_openrouter_key();
   if (has_key) {
      return config;
   }

   cloud_provider_t alt = llm_detect_available_provider();
   if (alt == CLOUD_PROVIDER_NONE) {
      OLOG_WARNING("Session %u: No cloud provider has an API key configured", session->session_id);
      return NULL;
   }
   OLOG_INFO("Session %u: %s provider unavailable, falling back to %s", session->session_id,
             cloud_provider_to_string(config->cloud_provider), cloud_provider_to_string(alt));
   memcpy(fallback, config, sizeof(session_llm_config_t));
   fallback->cloud_provider = alt;
   fallback->model[0] = '\0'; /* Clear model — let resolver pick default for new provider */
   return fallback;
}

/* Caller holds llm_config_mutex.  True for the running turn's own code: its
 * thread and the tool threads carrying its token. */
static bool is_turn_caller_locked(const session_t *session) {
   return session->turn_llm_config_set && tl_turn_token != 0 && tl_turn_token == session->turn_gen;
}

int session_set_llm_config(session_t *session, const session_llm_config_t *config) {
   if (!session || !config) {
      return 1;
   }
   session_llm_config_t fallback_config;
   config = usable_llm_config(session, config, &fallback_config);
   if (!config) {
      return 1;
   }

   /* A turn changing its own settings (switch_llm): the turn's settings change;
    * the session's only when the turn runs on the session's live history, i.e.
    * on the conversation being viewed. */
   const bool on_own_copy = session_turn_on_own_history(session);
   pthread_mutex_lock(&session->llm_config_mutex);
   const bool turn_caller = is_turn_caller_locked(session);
   if (turn_caller) {
      memcpy(&session->turn_llm_config, config, sizeof(session_llm_config_t));
   }
   if (!turn_caller || !on_own_copy) {
      memcpy(&session->llm_config, config, sizeof(session_llm_config_t));
   }
   pthread_mutex_unlock(&session->llm_config_mutex);

   OLOG_INFO("Session %u: LLM config updated (type=%d, provider=%d)", session->session_id,
             config->type, config->cloud_provider);

   return 0;
}

int session_set_turn_llm_config(session_t *session, const session_llm_config_t *config) {
   if (!session || !config) {
      return 1;
   }
   session_llm_config_t fallback_config;
   config = usable_llm_config(session, config, &fallback_config);
   if (!config) {
      return 1;
   }
   pthread_mutex_lock(&session->llm_config_mutex);
   const bool turn_caller = is_turn_caller_locked(session);
   if (turn_caller) {
      memcpy(&session->turn_llm_config, config, sizeof(session_llm_config_t));
   }
   pthread_mutex_unlock(&session->llm_config_mutex);
   if (!turn_caller) {
      OLOG_WARNING("Session %u: turn-only LLM change outside the running turn; ignored",
                   session->session_id);
      return 1;
   }
   OLOG_INFO("Session %u: LLM config updated for this turn only (type=%d, provider=%d)",
             session->session_id, config->type, config->cloud_provider);
   return 0;
}

void session_get_llm_config(session_t *session, session_llm_config_t *config) {
   if (!session || !config) {
      return;
   }

   if (tl_llm_override && tl_llm_override_session == session) {
      memcpy(config, tl_llm_override, sizeof(session_llm_config_t));
      return;
   }
   pthread_mutex_lock(&session->llm_config_mutex);
   if (session->turn_llm_config_set && tl_turn_token != 0 && tl_turn_token == session->turn_gen) {
      /* Part of the running turn: its own settings (see session_t.turn_llm_config). */
      memcpy(config, &session->turn_llm_config, sizeof(session_llm_config_t));
   } else {
      memcpy(config, &session->llm_config, sizeof(session_llm_config_t));
   }
   pthread_mutex_unlock(&session->llm_config_mutex);
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

uint64_t session_turn_token(void) {
   return tl_turn_token;
}

void session_set_turn_token(uint64_t token) {
   tl_turn_token = token;
}

void session_set_llm_config_override(const session_t *session, const session_llm_config_t *config) {
   tl_llm_override_session = config ? session : NULL;
   tl_llm_override = session ? config : NULL;
}

// =============================================================================
// Per-Session Metrics
// =============================================================================

/**
 * @brief Find or create provider entry in session metrics
 *
 * @param session Session to search
 * @param provider Provider name ("local", or a cloud_provider_to_string() name)
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
      safe_strscpy(p->provider, provider);
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
   safe_strscpy(db_metrics.session_type, session_type_name(session->type));
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
            safe_strscpy(providers[i].provider, m->providers[i].provider);
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

void session_metrics_totals(session_t *session, uint64_t *tokens_in_out, uint32_t *queries_out) {
   if (tokens_in_out) {
      *tokens_in_out = 0;
   }
   if (queries_out) {
      *queries_out = 0;
   }
   if (!session) {
      return;
   }
   uint64_t tok = 0;
   uint32_t q = 0;
   pthread_mutex_lock(&session->metrics_mutex);
   for (int i = 0; i < session->metrics.provider_count && i < SESSION_MAX_PROVIDERS; i++) {
      tok += session->metrics.providers[i].tokens_input;
      q += session->metrics.providers[i].queries;
   }
   pthread_mutex_unlock(&session->metrics_mutex);
   if (tokens_in_out) {
      *tokens_in_out = tok;
   }
   if (queries_out) {
      *queries_out = q;
   }
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
