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
 * Admin socket implementation for dawn-admin CLI communication.
 */

#define _GNU_SOURCE

#include "auth/admin_socket.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define ADMIN_SOCKET_INTERNAL_ALLOWED

#include "audio/music_db.h"
#include "audio/music_scanner.h"
#include "auth/admin_socket_internal.h"
#include "auth/auth_crypto.h"
#include "auth/auth_db.h"
#include "core/path_utils.h"
#include "dawn_error.h"
#include "image_store.h"
#include "logging.h"
#ifdef ENABLE_WEBUI
#include "webui/webui_server.h"
#endif

/* =============================================================================
 * Module State
 * =============================================================================
 */

static pthread_t s_listener_thread;
static volatile bool s_thread_running = false;
static volatile bool s_shutdown_requested = false;

static int s_listen_fd = -1;
static int s_shutdown_pipe[2] = { -1, -1 };

/* Setup token state */
static struct {
   char token[SETUP_TOKEN_LENGTH];
   time_t generated_at;
   time_t expires_at;
   int failed_attempts;
   bool used;
   bool valid;
} s_token_state = { 0 };

static pthread_mutex_t s_token_mutex = PTHREAD_MUTEX_INITIALIZER;

/* =============================================================================
 * Forward Declarations
 * =============================================================================
 */

static void *listener_thread_func(void *arg);
static int create_listening_socket(void);
static int generate_setup_token(void);
static int load_lockout_state(void);
static int save_lockout_state(void);
static int validate_peer_credentials(int client_fd);
static int handle_client(int client_fd);
static int handle_ping(int client_fd);
static int handle_validate_token(int client_fd, const char *payload, uint16_t payload_len);
static int handle_create_user(int client_fd, const char *payload, uint16_t payload_len);
static int handle_list_users(int client_fd);
static int handle_delete_user(int client_fd, const char *payload, uint16_t payload_len);
static int handle_change_password(int client_fd, const char *payload, uint16_t payload_len);
static int handle_unlock_user(int client_fd, const char *payload, uint16_t payload_len);
static int handle_list_sessions(int client_fd);
static int handle_revoke_session(int client_fd, const char *payload, uint16_t payload_len);
static int handle_revoke_user_sessions(int client_fd, const char *payload, uint16_t payload_len);
static int handle_get_stats(int client_fd);
static int handle_db_compact(int client_fd, const char *payload, uint16_t payload_len);
static int handle_db_backup(int client_fd, const char *payload, uint16_t payload_len);
static int handle_query_log(int client_fd, const char *payload, uint16_t payload_len);
static int handle_list_blocked_ips(int client_fd);
static int handle_unblock_ip(int client_fd, const char *payload, uint16_t payload_len);
static int handle_list_metrics(int client_fd, const char *payload, uint16_t payload_len);
static int handle_get_metrics_totals(int client_fd, const char *payload, uint16_t payload_len);
static int handle_list_conversations(int client_fd, const char *payload, uint16_t payload_len);
static int handle_get_conversation(int client_fd, const char *payload, uint16_t payload_len);
static int handle_delete_conversation(int client_fd, const char *payload, uint16_t payload_len);
/* send_response, send_text_response, the Phase 6.5 entity handlers
 * (handle_memory_entity_*), and the memory-maintenance handlers
 * (handle_memory_{recategorize,cleanup_meta_facts,summarize_missing,
 * reextract,reextract_status}) are declared in
 * auth/admin_socket_internal.h. */
static int send_list_response(int client_fd,
                              admin_resp_code_t code,
                              const void *data,
                              uint16_t data_len,
                              uint16_t item_count,
                              uint16_t flags);
static int verify_admin_auth(const char *payload, uint16_t payload_len, size_t *auth_size);
static int secure_compare(const char *a, const char *b, size_t len);

/* =============================================================================
 * Public API
 * =============================================================================
 */

int admin_socket_init(void) {
   if (s_thread_running) {
      OLOG_WARNING("Admin socket already running");
      return 0;
   }

   OLOG_INFO("Initializing admin socket...");

   /* Load any existing lockout state from previous run */
   load_lockout_state();

   /* Check if admin users already exist - skip token generation if so */
   int user_count = 0;
   auth_db_user_count(&user_count);
   bool skip_token = (user_count > 0);

   if (skip_token) {
      OLOG_INFO("Admin user(s) exist, skipping setup token generation");
      /* Mark token as invalid so validation fails */
      pthread_mutex_lock(&s_token_mutex);
      s_token_state.valid = false;
      pthread_mutex_unlock(&s_token_mutex);
   } else {
      /* Generate setup token using getrandom() - no fallback */
      if (generate_setup_token() != 0) {
         OLOG_ERROR("Failed to generate setup token - entropy failure");
         return 1;
      }
   }

   /* Create shutdown pipe for reliable signal delivery */
   if (pipe(s_shutdown_pipe) != 0) {
      OLOG_ERROR("Failed to create shutdown pipe: %s", strerror(errno));
      return 1;
   }

   /* Create listening socket (abstract namespace on Linux) */
   s_listen_fd = create_listening_socket();
   if (s_listen_fd < 0) {
      OLOG_ERROR("Failed to create admin socket");
      close(s_shutdown_pipe[0]);
      close(s_shutdown_pipe[1]);
      s_shutdown_pipe[0] = s_shutdown_pipe[1] = -1;
      return 1;
   }

   /* Reset state */
   s_shutdown_requested = false;

   /* Start listener thread */
   if (pthread_create(&s_listener_thread, NULL, listener_thread_func, NULL) != 0) {
      OLOG_ERROR("Failed to create admin socket listener thread");
      close(s_listen_fd);
      s_listen_fd = -1;
      close(s_shutdown_pipe[0]);
      close(s_shutdown_pipe[1]);
      s_shutdown_pipe[0] = s_shutdown_pipe[1] = -1;
      return 1;
   }

   s_thread_running = true;

   /* Print setup token to stderr only if no admin exists (NEVER to log files) */
   if (!skip_token) {
      fprintf(stderr, "\n");
      fprintf(stderr, "╔═══════════════════════════════════════════════════════════╗\n");
      fprintf(stderr, "║              DAWN FIRST-RUN SETUP TOKEN                   ║\n");
      fprintf(stderr, "╠═══════════════════════════════════════════════════════════╣\n");
      fprintf(stderr, "║                                                           ║\n");
      fprintf(stderr, "║   Token: %s                         ║\n", s_token_state.token);
      fprintf(stderr, "║                                                           ║\n");
      fprintf(stderr, "║   Valid for 5 minutes. Use with:                          ║\n");
      fprintf(stderr, "║   dawn-admin user create <username> --admin               ║\n");
      fprintf(stderr, "║                                                           ║\n");
      fprintf(stderr, "╚═══════════════════════════════════════════════════════════╝\n");
      fprintf(stderr, "\n");
      fflush(stderr);
   }

   OLOG_INFO("Admin socket initialized successfully");
   return 0;
}

void admin_socket_shutdown(void) {
   if (!s_thread_running) {
      return;
   }

   OLOG_INFO("Stopping admin socket...");

   /* Signal shutdown */
   s_shutdown_requested = true;

   /* Write to shutdown pipe to wake up select() */
   if (s_shutdown_pipe[1] >= 0) {
      char byte = 1;
      ssize_t written = write(s_shutdown_pipe[1], &byte, 1);
      if (written != 1) {
         OLOG_WARNING("Shutdown pipe write failed: %s", strerror(errno));
      }
   }

   /* Wait for listener thread with timeout */
#ifdef __GLIBC__
   struct timespec timeout;
   clock_gettime(CLOCK_REALTIME, &timeout);
   timeout.tv_sec += 2;

   int join_result = pthread_timedjoin_np(s_listener_thread, NULL, &timeout);
   if (join_result == ETIMEDOUT) {
      OLOG_WARNING("Admin socket thread did not exit in time, cancelling...");
      pthread_cancel(s_listener_thread);
      pthread_join(s_listener_thread, NULL);
   }
#else
   pthread_join(s_listener_thread, NULL);
#endif

   s_thread_running = false;

   /* Close listening socket */
   if (s_listen_fd >= 0) {
      close(s_listen_fd);
      s_listen_fd = -1;
   }

   /* Close shutdown pipe */
   if (s_shutdown_pipe[0] >= 0) {
      close(s_shutdown_pipe[0]);
      s_shutdown_pipe[0] = -1;
   }
   if (s_shutdown_pipe[1] >= 0) {
      close(s_shutdown_pipe[1]);
      s_shutdown_pipe[1] = -1;
   }

   /* Clear sensitive token data */
   pthread_mutex_lock(&s_token_mutex);
   explicit_bzero(s_token_state.token, sizeof(s_token_state.token));
   s_token_state.valid = false;
   pthread_mutex_unlock(&s_token_mutex);

   OLOG_INFO("Admin socket stopped");
}

bool admin_socket_is_running(void) {
   return s_thread_running;
}

int admin_socket_get_setup_token(char *buf, size_t buflen) {
   if (!buf || buflen < SETUP_TOKEN_LENGTH) {
      return 1;
   }

   pthread_mutex_lock(&s_token_mutex);
   if (!s_token_state.valid) {
      pthread_mutex_unlock(&s_token_mutex);
      return 1;
   }
   memcpy(buf, s_token_state.token, SETUP_TOKEN_LENGTH);
   pthread_mutex_unlock(&s_token_mutex);

   return 0;
}

/* =============================================================================
 * Socket Creation
 * =============================================================================
 */

static int create_listening_socket(void) {
   int fd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (fd < 0) {
      OLOG_ERROR("Failed to create Unix socket: %s", strerror(errno));
      return -1;
   }

   struct sockaddr_un addr;
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;

   /* Use abstract socket namespace (Linux-specific) */
   /* First byte is null, followed by the name */
   addr.sun_path[0] = '\0';
   strncpy(addr.sun_path + 1, ADMIN_SOCKET_ABSTRACT_NAME, sizeof(addr.sun_path) - 2);

   /* Calculate address length for abstract socket */
   socklen_t addr_len = offsetof(struct sockaddr_un, sun_path) + 1 +
                        strlen(ADMIN_SOCKET_ABSTRACT_NAME);

   if (bind(fd, (struct sockaddr *)&addr, addr_len) != 0) {
      OLOG_ERROR("Failed to bind admin socket: %s", strerror(errno));
      close(fd);
      return -1;
   }

   /* Only allow one connection at a time */
   if (listen(fd, ADMIN_MAX_CONNECTIONS) != 0) {
      OLOG_ERROR("Failed to listen on admin socket: %s", strerror(errno));
      close(fd);
      return -1;
   }

   OLOG_INFO("Admin socket listening (abstract: @%s)", ADMIN_SOCKET_ABSTRACT_NAME);
   return fd;
}

/* =============================================================================
 * Setup Token Generation
 * =============================================================================
 */

static int generate_setup_token(void) {
   unsigned char entropy[SETUP_TOKEN_RANDOM_CHARS];

   /* Use getrandom() with no fallback - fail closed on entropy failure */
   ssize_t ret = getrandom(entropy, sizeof(entropy), 0);
   if (ret != sizeof(entropy)) {
      OLOG_ERROR("getrandom() failed: %s (got %zd bytes, expected %zu)", strerror(errno), ret,
                 sizeof(entropy));
      return 1;
   }

   pthread_mutex_lock(&s_token_mutex);

   /* Format: DAWN-XXXX-XXXX-XXXX-XXXX */
   char *p = s_token_state.token;
   memcpy(p, "DAWN-", 5);
   p += 5;

   for (int i = 0; i < SETUP_TOKEN_RANDOM_CHARS; i++) {
      if (i > 0 && i % 4 == 0) {
         *p++ = '-';
      }
      *p++ = SETUP_TOKEN_CHARSET[entropy[i] % SETUP_TOKEN_CHARSET_LEN];
   }
   *p = '\0';

   /* Set validity period */
   s_token_state.generated_at = time(NULL);
   s_token_state.expires_at = s_token_state.generated_at + SETUP_TOKEN_VALIDITY_SEC;
   s_token_state.used = false;
   s_token_state.valid = true;

   /* Don't reset failed_attempts here - loaded from persistent state */

   pthread_mutex_unlock(&s_token_mutex);

   /* Clear entropy from stack */
   explicit_bzero(entropy, sizeof(entropy));

   return 0;
}

/* =============================================================================
 * Rate Limit Persistence
 * =============================================================================
 */

typedef struct __attribute__((packed)) {
   uint8_t attempt_count;
   time_t first_attempt_time;
   time_t lockout_until;
} lockout_state_file_t;

static int load_lockout_state(void) {
   int fd = open(SETUP_TOKEN_LOCKOUT_FILE, O_RDONLY);
   if (fd < 0) {
      /* No prior state - start fresh */
      s_token_state.failed_attempts = 0;
      return 0;
   }

   lockout_state_file_t state;
   ssize_t n = read(fd, &state, sizeof(state));
   close(fd);

   if (n != sizeof(state)) {
      /* Corrupted, reset */
      s_token_state.failed_attempts = 0;
      unlink(SETUP_TOKEN_LOCKOUT_FILE);
      return 0;
   }

   /* Check if lockout has expired */
   time_t now = time(NULL);
   if (state.lockout_until > 0 && now >= state.lockout_until) {
      /* Lockout expired, reset */
      s_token_state.failed_attempts = 0;
      unlink(SETUP_TOKEN_LOCKOUT_FILE);
      return 0;
   }

   s_token_state.failed_attempts = state.attempt_count;
   OLOG_INFO("Loaded lockout state: %d failed attempts", s_token_state.failed_attempts);
   return 0;
}

static int save_lockout_state(void) {
   /* Create directory if needed */
   if (!path_ensure_parent_dir_mode(SETUP_TOKEN_LOCKOUT_FILE, 0700)) {
      OLOG_WARNING("Failed to create lockout state directory");
      return 1;
   }

   int fd = open(SETUP_TOKEN_LOCKOUT_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0) {
      OLOG_WARNING("Failed to save lockout state: %s", strerror(errno));
      return 1;
   }

   lockout_state_file_t state = { 0 };
   state.attempt_count = (uint8_t)s_token_state.failed_attempts;
   state.first_attempt_time = time(NULL);
   if (s_token_state.failed_attempts >= SETUP_TOKEN_MAX_ATTEMPTS) {
      /* Lockout for 1 hour */
      state.lockout_until = state.first_attempt_time + 3600;
   }

   ssize_t written = write(fd, &state, sizeof(state));
   close(fd);

   if (written != sizeof(state)) {
      OLOG_WARNING("Failed to write complete lockout state");
      return 1;
   }

   return 0;
}

/* =============================================================================
 * Peer Credential Validation
 * =============================================================================
 */

static int validate_peer_credentials(int client_fd) {
   struct ucred cred;
   socklen_t len = sizeof(cred);

   if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
      OLOG_ERROR("Failed to get peer credentials: %s", strerror(errno));
      return 1;
   }

   /* Allow root (uid 0), the daemon's own user, or members of the daemon's group */
   uid_t daemon_uid = getuid();
   gid_t daemon_gid = getgid();

   if (cred.uid == 0 || cred.uid == daemon_uid) {
      OLOG_INFO("Admin socket client authenticated: uid=%d, pid=%d", cred.uid, cred.pid);
      return 0;
   }

   /* Check if the connecting user is in the daemon's primary group.
    * This allows e.g. users in the 'dawn' group to run dawn-admin without sudo. */
   struct passwd pwd_buf;
   struct passwd *pw = NULL;
   char pw_storage[256];
   if (getpwuid_r(cred.uid, &pwd_buf, pw_storage, sizeof(pw_storage), &pw) == 0 && pw) {
      int ngroups = 64;
      gid_t groups_stack[64];
      gid_t *groups = groups_stack;

      if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &ngroups) == -1) {
         /* User has more than 64 groups — retry with heap allocation */
         groups = malloc(ngroups * sizeof(gid_t));
         if (groups) {
            getgrouplist(pw->pw_name, pw->pw_gid, groups, &ngroups);
         } else {
            ngroups = 0;
         }
      }

      for (int i = 0; i < ngroups; i++) {
         if (groups[i] == daemon_gid) {
            OLOG_INFO("Admin socket client authenticated via group: uid=%d, gid=%d, pid=%d",
                      cred.uid, daemon_gid, cred.pid);
            if (groups != groups_stack)
               free(groups);
            return 0;
         }
      }

      if (groups != groups_stack)
         free(groups);
   }

   OLOG_WARNING(
       "Admin socket connection rejected: unauthorized UID %d (expected 0, %d, or group %d)",
       cred.uid, daemon_uid, daemon_gid);
   return 1;
}

/* =============================================================================
 * Constant-Time Comparison
 * =============================================================================
 */

static int secure_compare(const char *a, const char *b, size_t len) {
   volatile unsigned char result = 0;
   volatile const unsigned char *va = (volatile const unsigned char *)a;
   volatile const unsigned char *vb = (volatile const unsigned char *)b;

   for (size_t i = 0; i < len; i++) {
      result |= va[i] ^ vb[i];
   }

   return result;
}

/* =============================================================================
 * Message Handlers
 * =============================================================================
 */

int send_response(int client_fd, admin_resp_code_t code) {
   admin_msg_response_t resp = { 0 };
   resp.version = ADMIN_PROTOCOL_VERSION;
   resp.response_code = (uint8_t)code;
   resp.reserved = 0;

   ssize_t sent = write(client_fd, &resp, sizeof(resp));
   return (sent == sizeof(resp)) ? 0 : 1;
}

int send_text_response(int client_fd, admin_resp_code_t code, const char *text) {
   /* Send response header with content length in reserved field */
   uint16_t text_len = text ? (uint16_t)strlen(text) : 0;
   if (text_len > ADMIN_MSG_CONTENT_MAX) {
      text_len = ADMIN_MSG_CONTENT_MAX;
   }

   admin_msg_response_t resp = { 0 };
   resp.version = ADMIN_PROTOCOL_VERSION;
   resp.response_code = (uint8_t)code;
   resp.reserved = text_len;

   ssize_t sent = write(client_fd, &resp, sizeof(resp));
   if (sent != sizeof(resp)) {
      return 1;
   }

   /* Send text content if present */
   if (text_len > 0) {
      sent = write(client_fd, text, text_len);
      if (sent != text_len) {
         return 1;
      }
   }

   return 0;
}

static int handle_ping(int client_fd) {
   return send_response(client_fd, ADMIN_RESP_SUCCESS);
}

static int handle_validate_token(int client_fd, const char *payload, uint16_t payload_len) {
   pthread_mutex_lock(&s_token_mutex);

   /* Check rate limit first */
   if (s_token_state.failed_attempts >= SETUP_TOKEN_MAX_ATTEMPTS) {
      pthread_mutex_unlock(&s_token_mutex);
      OLOG_WARNING("Setup token rate limited");
      return send_response(client_fd, ADMIN_RESP_RATE_LIMITED);
   }

   /* Check if token is still valid */
   if (!s_token_state.valid) {
      pthread_mutex_unlock(&s_token_mutex);
      OLOG_WARNING("Setup token not available");
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* Check expiry */
   time_t now = time(NULL);
   if (now > s_token_state.expires_at) {
      pthread_mutex_unlock(&s_token_mutex);
      OLOG_WARNING("Setup token expired");
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* Check if already used */
   if (s_token_state.used) {
      pthread_mutex_unlock(&s_token_mutex);
      OLOG_INFO("Setup token already used");
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* Verify payload length matches expected token length */
   size_t expected_len = strlen(s_token_state.token);
   if (payload_len != expected_len) {
      s_token_state.failed_attempts++;
      save_lockout_state();
      pthread_mutex_unlock(&s_token_mutex);
      OLOG_WARNING("Invalid token length: %u (expected %zu)", payload_len, expected_len);
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* Constant-time comparison */
   if (secure_compare(payload, s_token_state.token, expected_len) != 0) {
      s_token_state.failed_attempts++;
      save_lockout_state();
      pthread_mutex_unlock(&s_token_mutex);
      OLOG_WARNING("Invalid setup token attempt (%d/%d)", s_token_state.failed_attempts,
                   SETUP_TOKEN_MAX_ATTEMPTS);
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* Token valid - mark as used */
   s_token_state.used = true;
   pthread_mutex_unlock(&s_token_mutex);

   OLOG_INFO("Setup token validated successfully");
   return send_response(client_fd, ADMIN_RESP_SUCCESS);
}

/**
 * @brief Handle ADMIN_MSG_CREATE_USER - atomic token validation + user creation.
 *
 * This combines token validation and user creation into a single atomic operation
 * to prevent the TOCTOU race condition identified in security review.
 *
 * The token is only marked as used after successful user creation.
 */
static int handle_create_user(int client_fd, const char *payload, uint16_t payload_len) {
   /* Minimum payload size: header struct */
   if (payload_len < sizeof(admin_create_user_payload_t)) {
      OLOG_WARNING("CREATE_USER payload too small: %u", payload_len);
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   const admin_create_user_payload_t *hdr = (const admin_create_user_payload_t *)payload;

   /* Validate lengths */
   if (hdr->username_len == 0 || hdr->username_len > ADMIN_USERNAME_MAX_LEN) {
      OLOG_WARNING("CREATE_USER invalid username length: %u", hdr->username_len);
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   if (hdr->password_len < ADMIN_PASSWORD_MIN_LEN || hdr->password_len > ADMIN_PASSWORD_MAX_LEN) {
      OLOG_WARNING("CREATE_USER invalid password length: %u", hdr->password_len);
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* Verify total payload size matches header + strings */
   size_t expected_len = sizeof(admin_create_user_payload_t) + hdr->username_len +
                         hdr->password_len;
   if (payload_len != expected_len) {
      OLOG_WARNING("CREATE_USER payload size mismatch: got %u, expected %zu", payload_len,
                   expected_len);
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* Extract username and password with explicit null termination */
   char username[ADMIN_USERNAME_MAX_LEN + 1] = { 0 };
   char password[ADMIN_PASSWORD_MAX_LEN + 1] = { 0 };

   const char *username_ptr = payload + sizeof(admin_create_user_payload_t);
   const char *password_ptr = username_ptr + hdr->username_len;

   memcpy(username, username_ptr, hdr->username_len);
   memcpy(password, password_ptr, hdr->password_len);

   bool is_admin = (hdr->is_admin != 0);

   /* Validate setup token first (under mutex) */
   pthread_mutex_lock(&s_token_mutex);

   /* Check rate limit */
   if (s_token_state.failed_attempts >= SETUP_TOKEN_MAX_ATTEMPTS) {
      pthread_mutex_unlock(&s_token_mutex);
      auth_secure_zero(password, sizeof(password));
      OLOG_WARNING("CREATE_USER rate limited");
      return send_response(client_fd, ADMIN_RESP_RATE_LIMITED);
   }

   /* Check if token is valid */
   if (!s_token_state.valid) {
      pthread_mutex_unlock(&s_token_mutex);
      auth_secure_zero(password, sizeof(password));
      OLOG_WARNING("CREATE_USER: setup token not available");
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* Check expiry */
   time_t now = time(NULL);
   if (now > s_token_state.expires_at) {
      pthread_mutex_unlock(&s_token_mutex);
      auth_secure_zero(password, sizeof(password));
      OLOG_WARNING("CREATE_USER: setup token expired");
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* Check if already used */
   if (s_token_state.used) {
      pthread_mutex_unlock(&s_token_mutex);
      auth_secure_zero(password, sizeof(password));
      OLOG_INFO("CREATE_USER: setup token already used");
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* Null-terminate the token from payload for comparison */
   char provided_token[SETUP_TOKEN_LENGTH] = { 0 };
   memcpy(provided_token, hdr->setup_token, SETUP_TOKEN_LENGTH - 1);

   /* Constant-time token comparison */
   size_t token_len = strlen(s_token_state.token);
   if (secure_compare(provided_token, s_token_state.token, token_len) != 0) {
      s_token_state.failed_attempts++;
      save_lockout_state();
      pthread_mutex_unlock(&s_token_mutex);
      auth_secure_zero(password, sizeof(password));
      OLOG_WARNING("CREATE_USER: invalid token (%d/%d)", s_token_state.failed_attempts,
                   SETUP_TOKEN_MAX_ATTEMPTS);
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* Token is valid - now create the user */
   /* Hash password */
   char password_hash[AUTH_HASH_LEN];
   int hash_result = auth_hash_password(password, password_hash);

   /* Clear password from memory immediately */
   auth_secure_zero(password, sizeof(password));

   if (hash_result != AUTH_CRYPTO_SUCCESS) {
      pthread_mutex_unlock(&s_token_mutex);
      OLOG_ERROR("CREATE_USER: password hashing failed: %d", hash_result);
      return send_response(client_fd, ADMIN_RESP_SERVICE_ERROR);
   }

   /* Create user in database */
   int db_result = auth_db_create_user(username, password_hash, is_admin);

   /* Clear hash from memory */
   auth_secure_zero(password_hash, sizeof(password_hash));

   if (db_result != AUTH_DB_SUCCESS) {
      pthread_mutex_unlock(&s_token_mutex);
      if (db_result == AUTH_DB_DUPLICATE) {
         OLOG_WARNING("CREATE_USER: username already exists: %s", username);
      } else {
         OLOG_ERROR("CREATE_USER: database error: %d", db_result);
      }
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   /* User created successfully - NOW mark token as used */
   s_token_state.used = true;

   /* Log the event */
   auth_db_log_event("USER_CREATED", username, NULL,
                     is_admin ? "{\"is_admin\":true}" : "{\"is_admin\":false}");

   pthread_mutex_unlock(&s_token_mutex);

   OLOG_INFO("CREATE_USER: user '%s' created successfully (admin=%d)", username, is_admin);
   return send_response(client_fd, ADMIN_RESP_SUCCESS);
}

/**
 * @brief Send extended list response for enumeration operations.
 */
static int send_list_response(int client_fd,
                              admin_resp_code_t code,
                              const void *data,
                              uint16_t data_len,
                              uint16_t item_count,
                              uint16_t flags) {
   admin_list_response_t resp = { 0 };
   resp.version = ADMIN_PROTOCOL_VERSION;
   resp.response_code = (uint8_t)code;
   resp.payload_len = data_len;
   resp.item_count = item_count;
   resp.flags = flags;

   /* Send header */
   ssize_t sent = write(client_fd, &resp, sizeof(resp));
   if (sent != sizeof(resp)) {
      return 1;
   }

   /* Send data if present */
   if (data && data_len > 0) {
      sent = write(client_fd, data, data_len);
      if (sent != data_len) {
         return 1;
      }
   }

   return 0;
}

/**
 * @brief Dummy hash for timing attack prevention.
 *
 * When a user is not found or is not an admin, we still perform password
 * verification against this dummy hash to maintain constant timing and
 * prevent user enumeration attacks.
 *
 * Parameters must match AUTH_MEMLIMIT/AUTH_OPSLIMIT from auth_crypto.h:
 * - Jetson: m=16384 (16MB), t=3, p=1
 * - Pi:     m=8192  (8MB),  t=4, p=1
 */
#ifdef PLATFORM_RPI
static const char DUMMY_PASSWORD_HASH[] = "$argon2id$v=19$m=8192,t=4,p=1$AAAAAAAAAAAAAAAAAAAAAA$"
                                          "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
#else
static const char DUMMY_PASSWORD_HASH[] = "$argon2id$v=19$m=16384,t=3,p=1$AAAAAAAAAAAAAAAAAAAAAA$"
                                          "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
#endif

/**
 * @brief Verify admin authentication from payload prefix.
 *
 * @param payload Raw payload containing admin_auth_prefix_t
 * @param payload_len Total payload length
 * @param auth_size Output: size consumed by auth prefix (header + credentials)
 * @return 0 on success, non-zero on failure
 */
static int verify_admin_auth(const char *payload, uint16_t payload_len, size_t *auth_size) {
   if (payload_len < sizeof(admin_auth_prefix_t)) {
      return FAILURE;
   }

   const admin_auth_prefix_t *auth = (const admin_auth_prefix_t *)payload;

   /* Validate lengths */
   if (auth->admin_username_len == 0 || auth->admin_username_len > ADMIN_USERNAME_MAX_LEN) {
      return FAILURE;
   }
   if (auth->admin_password_len < ADMIN_PASSWORD_MIN_LEN ||
       auth->admin_password_len > ADMIN_PASSWORD_MAX_LEN) {
      return FAILURE;
   }

   size_t total_auth_size = sizeof(admin_auth_prefix_t) + auth->admin_username_len +
                            auth->admin_password_len;
   if (payload_len < total_auth_size) {
      return FAILURE;
   }

   /* Extract credentials */
   char username[ADMIN_USERNAME_MAX_LEN + 1] = { 0 };
   char password[ADMIN_PASSWORD_MAX_LEN + 1] = { 0 };

   const char *username_ptr = payload + sizeof(admin_auth_prefix_t);
   const char *password_ptr = username_ptr + auth->admin_username_len;

   memcpy(username, username_ptr, auth->admin_username_len);
   memcpy(password, password_ptr, auth->admin_password_len);

   /* Look up user - track failures to ensure consistent timing */
   auth_user_t user;
   int rc = auth_db_get_user(username, &user);
   bool user_found = (rc == AUTH_DB_SUCCESS);
   bool is_admin = user_found && user.is_admin;

   /* Always verify password to prevent timing attacks.
    * Use dummy hash if user not found or not admin. */
   const char *hash_to_verify = (user_found && is_admin) ? user.password_hash : DUMMY_PASSWORD_HASH;
   int verify_result = auth_verify_password(password, hash_to_verify);

   auth_secure_zero(password, sizeof(password));

   /* Now check all conditions and log appropriately */
   if (!user_found) {
      auth_db_log_event("ADMIN_AUTH_FAILED", username, NULL, "user not found");
      return FAILURE;
   }

   if (!is_admin) {
      auth_db_log_event("ADMIN_AUTH_FAILED", username, NULL, "not an admin");
      return FAILURE;
   }

   if (verify_result != 0) {
      auth_db_log_event("ADMIN_AUTH_FAILED", username, NULL, "wrong password");
      return FAILURE;
   }

   if (auth_size) {
      *auth_size = total_auth_size;
   }

   return 0;
}

/* Context for user list callback */
typedef struct {
   char *buffer;
   size_t buffer_size;
   size_t offset;
   uint16_t count;
   bool truncated;
   time_t now;
} user_list_ctx_t;

static int user_list_callback(const auth_user_summary_t *user, void *ctx) {
   user_list_ctx_t *lctx = (user_list_ctx_t *)ctx;

   /* Each user entry is packed as:
    * 4 bytes: id
    * 1 byte: username_len
    * 1 byte: is_admin
    * 1 byte: is_locked (lockout_until > now)
    * 4 bytes: failed_attempts
    * N bytes: username
    */
   size_t uname_len = strlen(user->username);
   size_t entry_size = 4 + 1 + 1 + 1 + 4 + uname_len;

   if (lctx->offset + entry_size > lctx->buffer_size) {
      lctx->truncated = true;
      return 1; /* Stop - buffer full */
   }

   char *p = lctx->buffer + lctx->offset;

   /* Pack id (little-endian) */
   uint32_t id = (uint32_t)user->id;
   memcpy(p, &id, 4);
   p += 4;

   /* Pack username_len */
   *p++ = (uint8_t)uname_len;

   /* Pack is_admin */
   *p++ = user->is_admin ? 1 : 0;

   /* Pack is_locked */
   *p++ = (user->lockout_until > lctx->now) ? 1 : 0;

   /* Pack failed_attempts (little-endian) */
   uint32_t failed = (uint32_t)user->failed_attempts;
   memcpy(p, &failed, 4);
   p += 4;

   /* Pack username */
   memcpy(p, user->username, uname_len);

   lctx->offset += entry_size;
   lctx->count++;

   return 0;
}

static int handle_list_users(int client_fd) {
   /* Allocate buffer for response (max 4KB should be plenty) */
   char buffer[4096];
   user_list_ctx_t ctx = { .buffer = buffer,
                           .buffer_size = sizeof(buffer),
                           .offset = 0,
                           .count = 0,
                           .truncated = false,
                           .now = time(NULL) };

   int rc = auth_db_list_users(user_list_callback, &ctx);
   if (rc != AUTH_DB_SUCCESS) {
      /* Use list response format even for errors - client expects 8-byte header */
      return send_list_response(client_fd, ADMIN_RESP_SERVICE_ERROR, NULL, 0, 0, 0);
   }

   uint16_t flags = ctx.truncated ? ADMIN_LIST_FLAG_TRUNCATED : 0;
   return send_list_response(client_fd, ADMIN_RESP_SUCCESS, buffer, (uint16_t)ctx.offset, ctx.count,
                             flags);
}

static int handle_delete_user(int client_fd, const char *payload, uint16_t payload_len) {
   /* Verify admin auth */
   size_t auth_size = 0;
   if (verify_admin_auth(payload, payload_len, &auth_size) != 0) {
      OLOG_WARNING("DELETE_USER: admin auth failed");
      return send_response(client_fd, ADMIN_RESP_UNAUTHORIZED);
   }

   /* Extract target username (rest of payload after auth) */
   const char *target_ptr = payload + auth_size;
   uint16_t target_len = payload_len - (uint16_t)auth_size;

   if (target_len == 0 || target_len > ADMIN_USERNAME_MAX_LEN) {
      OLOG_WARNING("DELETE_USER: invalid target username length");
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   char target[ADMIN_USERNAME_MAX_LEN + 1] = { 0 };
   memcpy(target, target_ptr, target_len);

   /* Purge the user's images (rows + files) BEFORE the user delete: the images FK
    * cascades the rows on user delete but leaks the files, and after the cascade
    * there are no rows left to enumerate.  Edge: if the delete is then rejected
    * (last admin), the images are gone but the account survives — acceptable, since
    * deleting the last admin is a blocked misuse anyway. */
   auth_user_t del_user;
   if (auth_db_get_user(target, &del_user) == AUTH_DB_SUCCESS && del_user.id > 0) {
      if (image_store_delete_user(del_user.id) != IMAGE_STORE_SUCCESS) {
         /* Surface an incomplete purge — rows or files may have leaked, which matters
          * for a deletion guarantee.  The FK cascade still removes rows on user delete. */
         OLOG_WARNING("DELETE_USER: image purge for user %d incomplete (rows/files may remain)",
                      del_user.id);
      }
   }

   /* Delete the user */
   int rc = auth_db_delete_user(target);

   if (rc == AUTH_DB_LAST_ADMIN) {
      OLOG_WARNING("DELETE_USER: cannot delete last admin: %s", target);
      return send_response(client_fd, ADMIN_RESP_LAST_ADMIN);
   } else if (rc == AUTH_DB_NOT_FOUND) {
      OLOG_WARNING("DELETE_USER: user not found: %s", target);
      return send_response(client_fd, ADMIN_RESP_NOT_FOUND);
   } else if (rc != AUTH_DB_SUCCESS) {
      OLOG_ERROR("DELETE_USER: database error: %d", rc);
      return send_response(client_fd, ADMIN_RESP_SERVICE_ERROR);
   }

   auth_db_log_event("USER_DELETED", target, NULL, NULL);
   OLOG_INFO("DELETE_USER: user '%s' deleted", target);
   return send_response(client_fd, ADMIN_RESP_SUCCESS);
}

static int handle_change_password(int client_fd, const char *payload, uint16_t payload_len) {
   /* Verify admin auth */
   size_t auth_size = 0;
   if (verify_admin_auth(payload, payload_len, &auth_size) != 0) {
      OLOG_WARNING("CHANGE_PASSWORD: admin auth failed");
      return send_response(client_fd, ADMIN_RESP_UNAUTHORIZED);
   }

   /* Payload after auth: 1 byte username_len, 1 byte password_len, username, password */
   const char *rest = payload + auth_size;
   uint16_t rest_len = payload_len - (uint16_t)auth_size;

   if (rest_len < 2) {
      OLOG_WARNING("CHANGE_PASSWORD: payload too short");
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   uint8_t target_uname_len = (uint8_t)rest[0];
   uint8_t new_pass_len = (uint8_t)rest[1];

   if (target_uname_len == 0 || target_uname_len > ADMIN_USERNAME_MAX_LEN) {
      OLOG_WARNING("CHANGE_PASSWORD: invalid username length");
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   if (new_pass_len < ADMIN_PASSWORD_MIN_LEN || new_pass_len > ADMIN_PASSWORD_MAX_LEN) {
      OLOG_WARNING("CHANGE_PASSWORD: invalid password length");
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   if (rest_len != 2 + target_uname_len + new_pass_len) {
      OLOG_WARNING("CHANGE_PASSWORD: payload size mismatch");
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   char target[ADMIN_USERNAME_MAX_LEN + 1] = { 0 };
   char new_password[ADMIN_PASSWORD_MAX_LEN + 1] = { 0 };

   memcpy(target, rest + 2, target_uname_len);
   memcpy(new_password, rest + 2 + target_uname_len, new_pass_len);

   /* Hash new password */
   char new_hash[AUTH_HASH_LEN];
   if (auth_hash_password(new_password, new_hash) != 0) {
      auth_secure_zero(new_password, sizeof(new_password));
      OLOG_ERROR("CHANGE_PASSWORD: failed to hash password");
      return send_response(client_fd, ADMIN_RESP_SERVICE_ERROR);
   }
   auth_secure_zero(new_password, sizeof(new_password));

   /* Update password (also invalidates all sessions) */
   int rc = auth_db_update_password(target, new_hash);

   if (rc == AUTH_DB_NOT_FOUND) {
      OLOG_WARNING("CHANGE_PASSWORD: user not found: %s", target);
      return send_response(client_fd, ADMIN_RESP_NOT_FOUND);
   } else if (rc != AUTH_DB_SUCCESS) {
      OLOG_ERROR("CHANGE_PASSWORD: database error: %d", rc);
      return send_response(client_fd, ADMIN_RESP_SERVICE_ERROR);
   }

   auth_db_log_event("PASSWORD_CHANGED", target, NULL, NULL);
   OLOG_INFO("CHANGE_PASSWORD: password changed for '%s'", target);
   return send_response(client_fd, ADMIN_RESP_SUCCESS);
}

static int handle_unlock_user(int client_fd, const char *payload, uint16_t payload_len) {
   /* Verify admin auth */
   size_t auth_size = 0;
   if (verify_admin_auth(payload, payload_len, &auth_size) != 0) {
      OLOG_WARNING("UNLOCK_USER: admin auth failed");
      return send_response(client_fd, ADMIN_RESP_UNAUTHORIZED);
   }

   /* Extract target username */
   const char *target_ptr = payload + auth_size;
   uint16_t target_len = payload_len - (uint16_t)auth_size;

   if (target_len == 0 || target_len > ADMIN_USERNAME_MAX_LEN) {
      OLOG_WARNING("UNLOCK_USER: invalid target username length");
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   char target[ADMIN_USERNAME_MAX_LEN + 1] = { 0 };
   memcpy(target, target_ptr, target_len);

   /* Unlock the user */
   int rc = auth_db_unlock_user(target);

   if (rc == AUTH_DB_NOT_FOUND) {
      OLOG_WARNING("UNLOCK_USER: user not found: %s", target);
      return send_response(client_fd, ADMIN_RESP_NOT_FOUND);
   } else if (rc != AUTH_DB_SUCCESS) {
      OLOG_ERROR("UNLOCK_USER: database error: %d", rc);
      return send_response(client_fd, ADMIN_RESP_SERVICE_ERROR);
   }

   auth_db_log_event("USER_UNLOCKED", target, NULL, NULL);
   OLOG_INFO("UNLOCK_USER: user '%s' unlocked", target);
   return send_response(client_fd, ADMIN_RESP_SUCCESS);
}

/* Context for session list callback */
typedef struct {
   char *buffer;
   size_t buffer_size;
   size_t offset;
   uint16_t count;
   bool truncated;
} session_list_ctx_t;

static int session_list_callback(const auth_session_summary_t *session, void *ctx) {
   session_list_ctx_t *lctx = (session_list_ctx_t *)ctx;

   /* Each session entry is packed as:
    * 8 bytes: token_prefix (null padded)
    * 1 byte: username_len
    * 8 bytes: created_at
    * 8 bytes: last_activity
    * 1 byte: ip_len
    * N bytes: username
    * M bytes: ip_address
    */
   size_t uname_len = strlen(session->username);
   size_t ip_len = strlen(session->ip_address);
   size_t entry_size = 8 + 1 + 8 + 8 + 1 + uname_len + ip_len;

   if (lctx->offset + entry_size > lctx->buffer_size) {
      lctx->truncated = true;
      return 1; /* Stop - buffer full */
   }

   char *p = lctx->buffer + lctx->offset;

   /* Pack token_prefix (8 bytes, null padded) */
   memset(p, 0, 8);
   memcpy(p, session->token_prefix, 8);
   p += 8;

   /* Pack username_len */
   *p++ = (uint8_t)uname_len;

   /* Pack created_at (little-endian) */
   int64_t ts = (int64_t)session->created_at;
   memcpy(p, &ts, 8);
   p += 8;

   /* Pack last_activity (little-endian) */
   ts = (int64_t)session->last_activity;
   memcpy(p, &ts, 8);
   p += 8;

   /* Pack ip_len */
   *p++ = (uint8_t)ip_len;

   /* Pack username */
   memcpy(p, session->username, uname_len);
   p += uname_len;

   /* Pack ip_address */
   memcpy(p, session->ip_address, ip_len);

   lctx->offset += entry_size;
   lctx->count++;

   return 0;
}

static int handle_list_sessions(int client_fd) {
   /* Allocate buffer for response */
   char buffer[8192];
   session_list_ctx_t ctx = { .buffer = buffer,
                              .buffer_size = sizeof(buffer),
                              .offset = 0,
                              .count = 0,
                              .truncated = false };

   int rc = auth_db_list_sessions(session_list_callback, &ctx);
   if (rc != AUTH_DB_SUCCESS) {
      /* Use list response format even for errors - client expects 8-byte header */
      return send_list_response(client_fd, ADMIN_RESP_SERVICE_ERROR, NULL, 0, 0, 0);
   }

   uint16_t flags = ctx.truncated ? ADMIN_LIST_FLAG_TRUNCATED : 0;
   return send_list_response(client_fd, ADMIN_RESP_SUCCESS, buffer, (uint16_t)ctx.offset, ctx.count,
                             flags);
}

/**
 * @brief Handle REVOKE_SESSION message.
 *
 * Wire format:
 *   admin_auth_prefix_t (admin credentials)
 *   uint8_t token_prefix_len (should be 8)
 *   char token_prefix[token_prefix_len]
 */
static int handle_revoke_session(int client_fd, const char *payload, uint16_t payload_len) {
   /* Verify admin credentials */
   size_t auth_size = 0;
   int auth_result = verify_admin_auth(payload, payload_len, &auth_size);
   if (auth_result != 0) {
      return send_response(client_fd,
                           (auth_result == -2) ? ADMIN_RESP_UNAUTHORIZED : ADMIN_RESP_FAILURE);
   }

   /* Parse token prefix after auth */
   const char *remaining = payload + auth_size;
   size_t remaining_len = payload_len - auth_size;

   if (remaining_len < 1) {
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   uint8_t prefix_len = (uint8_t)remaining[0];
   if (prefix_len < 8 || remaining_len < 1 + prefix_len) {
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   char prefix[9] = { 0 };
   memcpy(prefix, remaining + 1, 8);

   /* Delete session by prefix */
   int rc = auth_db_delete_session_by_prefix(prefix);
   if (rc == AUTH_DB_NOT_FOUND) {
      return send_response(client_fd, ADMIN_RESP_NOT_FOUND);
   } else if (rc != AUTH_DB_SUCCESS) {
      return send_response(client_fd, ADMIN_RESP_SERVICE_ERROR);
   }

   /* Log the revocation */
   auth_db_log_event("SESSION_REVOKED", NULL, NULL, prefix);

   return send_response(client_fd, ADMIN_RESP_SUCCESS);
}

/**
 * @brief Handle REVOKE_USER_SESSIONS message.
 *
 * Wire format:
 *   admin_auth_prefix_t (admin credentials)
 *   uint8_t username_len
 *   char username[username_len]
 */
static int handle_revoke_user_sessions(int client_fd, const char *payload, uint16_t payload_len) {
   /* Verify admin credentials */
   size_t auth_size = 0;
   int auth_result = verify_admin_auth(payload, payload_len, &auth_size);
   if (auth_result != 0) {
      return send_response(client_fd,
                           (auth_result == -2) ? ADMIN_RESP_UNAUTHORIZED : ADMIN_RESP_FAILURE);
   }

   /* Parse username after auth */
   const char *remaining = payload + auth_size;
   size_t remaining_len = payload_len - auth_size;

   if (remaining_len < 1) {
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   uint8_t username_len = (uint8_t)remaining[0];
   if (username_len < 1 || username_len > ADMIN_USERNAME_MAX_LEN ||
       remaining_len < 1 + username_len) {
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   char username[ADMIN_USERNAME_MAX_LEN + 1] = { 0 };
   memcpy(username, remaining + 1, username_len);

   /* Delete all sessions for user */
   int count = 0;
   int del_rc = auth_db_delete_sessions_by_username(username, &count);
   if (del_rc != AUTH_DB_SUCCESS) {
      return send_response(client_fd, ADMIN_RESP_SERVICE_ERROR);
   }

   /* Log the revocation */
   char details[64];
   snprintf(details, sizeof(details), "%d sessions revoked", count);
   auth_db_log_event("USER_SESSIONS_REVOKED", username, NULL, details);

   return send_response(client_fd, ADMIN_RESP_SUCCESS);
}

/**
 * @brief Handle GET_STATS message.
 *
 * No authentication required for read-only stats.
 */
static int handle_get_stats(int client_fd) {
   auth_db_stats_t stats;
   int rc = auth_db_get_stats(&stats);
   if (rc != AUTH_DB_SUCCESS) {
      /* Use list response format even for errors - client expects 8-byte header */
      return send_list_response(client_fd, ADMIN_RESP_SERVICE_ERROR, NULL, 0, 0, 0);
   }

   /* Send stats as extended response (no truncation possible) */
   return send_list_response(client_fd, ADMIN_RESP_SUCCESS, &stats, sizeof(stats), 1, 0);
}

/**
 * @brief Handle DB_COMPACT (vacuum) message.
 *
 * Requires admin auth. Rate-limited to once per 24 hours.
 */
static int handle_db_compact(int client_fd, const char *payload, uint16_t payload_len) {
   /* Verify admin credentials */
   size_t auth_size = 0;
   int auth_result = verify_admin_auth(payload, payload_len, &auth_size);
   if (auth_result != 0) {
      return send_response(client_fd,
                           (auth_result == -2) ? ADMIN_RESP_UNAUTHORIZED : ADMIN_RESP_FAILURE);
   }

   int rc = auth_db_vacuum();
   if (rc == AUTH_DB_RATE_LIMITED) {
      return send_response(client_fd, ADMIN_RESP_RATE_LIMITED);
   } else if (rc != AUTH_DB_SUCCESS) {
      return send_response(client_fd, ADMIN_RESP_SERVICE_ERROR);
   }

   auth_db_log_event("DB_COMPACT", NULL, NULL, "VACUUM completed");
   return send_response(client_fd, ADMIN_RESP_SUCCESS);
}

/**
 * @brief Handle DB_BACKUP message.
 *
 * Requires admin auth.
 * Wire format: admin_auth_prefix + path_len (1 byte) + path
 */
static int handle_db_backup(int client_fd, const char *payload, uint16_t payload_len) {
   /* Verify admin credentials */
   size_t auth_size = 0;
   int auth_result = verify_admin_auth(payload, payload_len, &auth_size);
   if (auth_result != 0) {
      return send_response(client_fd,
                           (auth_result == -2) ? ADMIN_RESP_UNAUTHORIZED : ADMIN_RESP_FAILURE);
   }

   /* Parse path after auth */
   const char *remaining = payload + auth_size;
   size_t remaining_len = payload_len - auth_size;

   if (remaining_len < 2) {
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   uint8_t path_len = (uint8_t)remaining[0];
   if (path_len < 1 || remaining_len < 1 + path_len) {
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   char dest_path[256] = { 0 };
   if (path_len >= sizeof(dest_path)) {
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }
   memcpy(dest_path, remaining + 1, path_len);

   int rc = auth_db_backup(dest_path);
   if (rc != AUTH_DB_SUCCESS) {
      return send_response(client_fd, ADMIN_RESP_SERVICE_ERROR);
   }

   auth_db_log_event("DB_BACKUP", NULL, NULL, dest_path);
   return send_response(client_fd, ADMIN_RESP_SUCCESS);
}

/**
 * @brief Context for log query callback to serialize entries.
 */
typedef struct {
   char *buffer;
   size_t buffer_size;
   size_t offset;
   uint16_t count;
   bool truncated;
} log_query_ctx_t;

/**
 * @brief Callback to serialize log entries.
 *
 * Wire format per entry:
 *   int64_t timestamp (8 bytes)
 *   uint8_t event_len
 *   uint8_t username_len
 *   uint8_t ip_len
 *   uint8_t details_len
 *   char event[event_len]
 *   char username[username_len]
 *   char ip[ip_len]
 *   char details[details_len]
 */
static int log_query_callback(const auth_log_entry_t *entry, void *ctx) {
   log_query_ctx_t *qctx = (log_query_ctx_t *)ctx;

   size_t event_len = strlen(entry->event);
   size_t user_len = strlen(entry->username);
   size_t ip_len = strlen(entry->ip_address);
   size_t details_len = strlen(entry->details);

   /* Truncate if too long */
   if (event_len > 31)
      event_len = 31;
   if (user_len > 63)
      user_len = 63;
   if (ip_len > 45)
      ip_len = 45;
   if (details_len > 255)
      details_len = 255;

   size_t entry_size = 8 + 4 + event_len + user_len + ip_len + details_len;

   if (qctx->offset + entry_size > qctx->buffer_size) {
      qctx->truncated = true;
      return 1; /* Stop - buffer full */
   }

   char *p = qctx->buffer + qctx->offset;

   /* Timestamp */
   int64_t ts = (int64_t)entry->timestamp;
   memcpy(p, &ts, 8);
   p += 8;

   /* Lengths */
   *p++ = (uint8_t)event_len;
   *p++ = (uint8_t)user_len;
   *p++ = (uint8_t)ip_len;
   *p++ = (uint8_t)details_len;

   /* Strings */
   memcpy(p, entry->event, event_len);
   p += event_len;
   memcpy(p, entry->username, user_len);
   p += user_len;
   memcpy(p, entry->ip_address, ip_len);
   p += ip_len;
   memcpy(p, entry->details, details_len);
   p += details_len;

   qctx->offset = p - qctx->buffer;
   qctx->count++;

   return 0;
}

/**
 * @brief Handle QUERY_LOG message.
 *
 * Wire format (filter):
 *   int64_t since (0 = no limit)
 *   int64_t until (0 = no limit)
 *   uint8_t event_len (0 = all events)
 *   uint8_t username_len (0 = all users)
 *   uint16_t limit (0 = default)
 *   uint16_t offset
 *   char event[event_len]
 *   char username[username_len]
 */
static int handle_query_log(int client_fd, const char *payload, uint16_t payload_len) {
   auth_log_filter_t filter = { 0 };
   char event_buf[32] = { 0 };
   char user_buf[64] = { 0 };

   /* Parse filter from payload (if provided) */
   if (payload_len >= 20) {
      const char *p = payload;

      int64_t since, until;
      memcpy(&since, p, 8);
      p += 8;
      memcpy(&until, p, 8);
      p += 8;

      uint8_t event_len = (uint8_t)*p++;
      uint8_t user_len = (uint8_t)*p++;

      uint16_t limit_val, offset_val;
      memcpy(&limit_val, p, 2);
      p += 2;
      memcpy(&offset_val, p, 2);
      p += 2;

      filter.since = (time_t)since;
      filter.until = (time_t)until;
      filter.limit = limit_val;
      filter.offset = offset_val;

      /* Read strings if present */
      if (event_len > 0 && event_len < sizeof(event_buf) &&
          p + event_len <= payload + payload_len) {
         memcpy(event_buf, p, event_len);
         filter.event = event_buf;
         p += event_len;
      }

      if (user_len > 0 && user_len < sizeof(user_buf) && p + user_len <= payload + payload_len) {
         memcpy(user_buf, p, user_len);
         filter.username = user_buf;
      }
   }

   /* Allocate buffer for response */
   char buffer[16384];
   log_query_ctx_t ctx = { .buffer = buffer,
                           .buffer_size = sizeof(buffer),
                           .offset = 0,
                           .count = 0,
                           .truncated = false };

   int rc = auth_db_query_audit_log(&filter, log_query_callback, &ctx);
   if (rc != AUTH_DB_SUCCESS) {
      /* Use list response format even for errors - client expects 8-byte header */
      return send_list_response(client_fd, ADMIN_RESP_SERVICE_ERROR, NULL, 0, 0, 0);
   }

   uint16_t flags = ctx.truncated ? ADMIN_LIST_FLAG_TRUNCATED : 0;
   return send_list_response(client_fd, ADMIN_RESP_SUCCESS, buffer, (uint16_t)ctx.offset, ctx.count,
                             flags);
}

/* Context for blocked IP list callback */
typedef struct {
   char *buffer;
   size_t buffer_size;
   size_t offset;
   uint16_t count;
   bool truncated;
} blocked_ip_list_ctx_t;

static int blocked_ip_list_callback(const auth_ip_status_t *status, void *ctx) {
   blocked_ip_list_ctx_t *lctx = (blocked_ip_list_ctx_t *)ctx;

   /* Each entry is packed as:
    * 1 byte: ip_len
    * 4 bytes: failed_attempts (little-endian)
    * 8 bytes: last_attempt (little-endian)
    * N bytes: ip_address
    */
   size_t ip_len = strlen(status->ip_address);
   size_t entry_size = 1 + 4 + 8 + ip_len;

   if (lctx->offset + entry_size > lctx->buffer_size) {
      lctx->truncated = true;
      return 1; /* Stop - buffer full */
   }

   char *p = lctx->buffer + lctx->offset;

   /* Pack ip_len */
   *p++ = (uint8_t)ip_len;

   /* Pack failed_attempts (little-endian) */
   int32_t attempts = (int32_t)status->failed_attempts;
   memcpy(p, &attempts, 4);
   p += 4;

   /* Pack last_attempt (little-endian) */
   int64_t ts = (int64_t)status->last_attempt;
   memcpy(p, &ts, 8);
   p += 8;

   /* Pack ip_address */
   memcpy(p, status->ip_address, ip_len);

   lctx->offset += entry_size;
   lctx->count++;

   return 0;
}

static int handle_list_blocked_ips(int client_fd) {
   /* Use rate limit window from webui_server.c (15 minutes) */
   time_t window_start = time(NULL) - (15 * 60);

   char buffer[4096];
   blocked_ip_list_ctx_t ctx = { .buffer = buffer,
                                 .buffer_size = sizeof(buffer),
                                 .offset = 0,
                                 .count = 0,
                                 .truncated = false };

   int rc = auth_db_list_blocked_ips(window_start, blocked_ip_list_callback, &ctx);

   if (rc != AUTH_DB_SUCCESS) {
      return send_list_response(client_fd, ADMIN_RESP_SERVICE_ERROR, NULL, 0, 0, 0);
   }

   uint16_t flags = ctx.truncated ? ADMIN_LIST_FLAG_TRUNCATED : 0;
   return send_list_response(client_fd, ADMIN_RESP_SUCCESS, buffer, (uint16_t)ctx.offset, ctx.count,
                             flags);
}

static int handle_unblock_ip(int client_fd, const char *payload, uint16_t payload_len) {
   /* Verify admin auth */
   size_t auth_size = 0;
   if (verify_admin_auth(payload, payload_len, &auth_size) != 0) {
      OLOG_WARNING("UNBLOCK_IP: admin auth failed");
      return send_response(client_fd, ADMIN_RESP_UNAUTHORIZED);
   }

   /* Extract IP address (or "--all" to clear all) */
   const char *ip_ptr = payload + auth_size;
   uint16_t ip_len = payload_len - (uint16_t)auth_size;

   char ip_address[AUTH_IP_MAX] = { 0 };
   bool clear_all = false;

   if (ip_len == 0) {
      OLOG_WARNING("UNBLOCK_IP: no IP address provided");
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   if (ip_len >= AUTH_IP_MAX) {
      OLOG_WARNING("UNBLOCK_IP: IP address too long");
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   memcpy(ip_address, ip_ptr, ip_len);

   /* Check for --all flag */
   if (strcmp(ip_address, "--all") == 0) {
      clear_all = true;
   }

   /* Clear login attempts from database */
   int deleted = 0;
   int clear_rc = auth_db_clear_login_attempts(clear_all ? NULL : ip_address, &deleted);

   if (clear_rc != AUTH_DB_SUCCESS) {
      OLOG_ERROR("UNBLOCK_IP: database error");
      return send_response(client_fd, ADMIN_RESP_SERVICE_ERROR);
   }

   /* Also clear in-memory rate limiter (only when WebUI is enabled) */
#ifdef ENABLE_WEBUI
   webui_clear_login_rate_limit(clear_all ? NULL : ip_address);
#endif

   if (clear_all) {
      auth_db_log_event("IP_UNBLOCKED", NULL, NULL, "All IPs unblocked");
      OLOG_INFO("UNBLOCK_IP: cleared %d login attempts (all IPs)", deleted);
   } else {
      auth_db_log_event("IP_UNBLOCKED", NULL, ip_address, NULL);
      OLOG_INFO("UNBLOCK_IP: cleared %d login attempts for IP '%s'", deleted, ip_address);
   }

   return send_response(client_fd, ADMIN_RESP_SUCCESS);
}

/* =============================================================================
 * Phase 3: Metrics Handlers
 * =============================================================================
 */

/* Context for metrics list callback */
typedef struct {
   char *buffer;
   size_t buffer_size;
   size_t offset;
   int count;
   bool truncated;
} metrics_list_ctx_t;

static int metrics_list_callback(const session_metrics_t *m, void *ctx) {
   metrics_list_ctx_t *mctx = (metrics_list_ctx_t *)ctx;

   /* Pack entry: 8 id, 4 session_id, 4 user_id, 1 type_len, N type,
    * 8 started_at, 8 ended_at, 4 queries_total, 4 queries_cloud, 4 queries_local,
    * 4 errors, 8 avg_llm_ms */
   size_t type_len = strlen(m->session_type);
   size_t entry_size = 8 + 4 + 4 + 1 + type_len + 8 + 8 + 4 + 4 + 4 + 4 + 8;

   if (mctx->offset + entry_size > mctx->buffer_size) {
      mctx->truncated = true;
      return 1; /* Stop iteration */
   }

   char *p = mctx->buffer + mctx->offset;

   memcpy(p, &m->id, 8);
   p += 8;
   uint32_t sid = m->session_id;
   memcpy(p, &sid, 4);
   p += 4;
   int32_t uid = m->user_id;
   memcpy(p, &uid, 4);
   p += 4;
   *p++ = (uint8_t)type_len;
   memcpy(p, m->session_type, type_len);
   p += type_len;
   int64_t ts = m->started_at;
   memcpy(p, &ts, 8);
   p += 8;
   ts = m->ended_at;
   memcpy(p, &ts, 8);
   p += 8;
   uint32_t val = m->queries_total;
   memcpy(p, &val, 4);
   p += 4;
   val = m->queries_cloud;
   memcpy(p, &val, 4);
   p += 4;
   val = m->queries_local;
   memcpy(p, &val, 4);
   p += 4;
   val = m->errors_count;
   memcpy(p, &val, 4);
   p += 4;
   memcpy(p, &m->avg_llm_total_ms, 8);
   p += 8;

   mctx->offset = p - mctx->buffer;
   mctx->count++;
   return 0;
}

static int handle_list_metrics(int client_fd, const char *payload, uint16_t payload_len) {
   /* Parse filter: 4 bytes user_id, 4 bytes limit, 1 byte type_len, N bytes type */
   session_metrics_filter_t filter = { 0 };
   char type_buf[16] = { 0 };

   if (payload_len >= 9) {
      int32_t user_id;
      memcpy(&user_id, payload, 4);
      filter.user_id = user_id;

      int32_t limit;
      memcpy(&limit, payload + 4, 4);
      filter.limit = (limit > 0) ? limit : 20;

      uint8_t type_len = (uint8_t)payload[8];
      if (type_len > 0 && type_len < 16 && payload_len >= 9 + type_len) {
         memcpy(type_buf, payload + 9, type_len);
         filter.type = type_buf;
      }
   } else {
      filter.limit = 20;
   }

   /* Allocate buffer for response */
   char buffer[8192];
   metrics_list_ctx_t ctx = { .buffer = buffer,
                              .buffer_size = sizeof(buffer),
                              .offset = 0,
                              .count = 0,
                              .truncated = false };

   int rc = auth_db_list_session_metrics(&filter, metrics_list_callback, &ctx);
   if (rc != AUTH_DB_SUCCESS) {
      return send_list_response(client_fd, ADMIN_RESP_SERVICE_ERROR, NULL, 0, 0, 0);
   }

   uint16_t flags = ctx.truncated ? ADMIN_LIST_FLAG_TRUNCATED : 0;
   return send_list_response(client_fd, ADMIN_RESP_SUCCESS, buffer, (uint16_t)ctx.offset, ctx.count,
                             flags);
}

static int handle_get_metrics_totals(int client_fd, const char *payload, uint16_t payload_len) {
   /* Parse filter: 4 bytes user_id, 1 byte type_len, N bytes type */
   session_metrics_filter_t filter = { 0 };
   char type_buf[16] = { 0 };

   if (payload_len >= 5) {
      int32_t user_id;
      memcpy(&user_id, payload, 4);
      filter.user_id = user_id;

      uint8_t type_len = (uint8_t)payload[4];
      if (type_len > 0 && type_len < 16 && payload_len >= 5 + type_len) {
         memcpy(type_buf, payload + 5, type_len);
         filter.type = type_buf;
      }
   }

   session_metrics_t totals = { 0 };
   int rc = auth_db_get_metrics_aggregate(&filter, &totals);
   if (rc != AUTH_DB_SUCCESS) {
      return send_response(client_fd, ADMIN_RESP_SERVICE_ERROR);
   }

   /* Send simple response + data
    * Data: 4 session_count, 8 queries_total, 8 queries_cloud, 8 queries_local,
    *       8 errors_total, 8 avg_llm_ms = 48 bytes
    */
   send_response(client_fd, ADMIN_RESP_SUCCESS);

   char data[48];
   char *p = data;

   int32_t count = (int32_t)totals.session_id; /* Repurposed for count in aggregate */
   memcpy(p, &count, 4);
   p += 4;

   uint64_t val = totals.queries_total;
   memcpy(p, &val, 8);
   p += 8;
   val = totals.queries_cloud;
   memcpy(p, &val, 8);
   p += 8;
   val = totals.queries_local;
   memcpy(p, &val, 8);
   p += 8;
   val = totals.errors_count;
   memcpy(p, &val, 8);
   p += 8;
   memcpy(p, &totals.avg_llm_total_ms, 8);

   ssize_t written = write(client_fd, data, sizeof(data));
   if (written != sizeof(data)) {
      OLOG_ERROR("Failed to write metrics totals: %zd/%zu", written, sizeof(data));
      return FAILURE;
   }
   return 0;
}

/* =============================================================================
 * Phase 4: Conversation Handlers
 * =============================================================================
 */

/* Context for conversation list callback */
typedef struct {
   char *buffer;
   size_t buffer_size;
   size_t offset;
   int count;
   bool truncated;
} conv_list_ctx_t;

/* Helper to pack conversation entry into buffer */
static int pack_conv_entry(conv_list_ctx_t *cctx,
                           const conversation_t *conv,
                           const char *username) {
   /* Pack entry: 8 id, 1 title_len, N title, 8 created_at, 8 updated_at,
    * 4 message_count, 1 uname_len, N username */
   size_t title_len = strlen(conv->title);
   if (title_len > 127)
      title_len = 127;

   size_t uname_len = username ? strlen(username) : 0;
   if (uname_len > 63)
      uname_len = 63;

   size_t entry_size = 8 + 1 + title_len + 8 + 8 + 4 + 1 + uname_len;

   if (cctx->offset + entry_size > cctx->buffer_size) {
      cctx->truncated = true;
      return 1; /* Stop iteration */
   }

   char *p = cctx->buffer + cctx->offset;

   memcpy(p, &conv->id, 8);
   p += 8;
   *p++ = (uint8_t)title_len;
   memcpy(p, conv->title, title_len);
   p += title_len;
   int64_t ts = conv->created_at;
   memcpy(p, &ts, 8);
   p += 8;
   ts = conv->updated_at;
   memcpy(p, &ts, 8);
   p += 8;
   int32_t mc = conv->message_count;
   memcpy(p, &mc, 4);
   p += 4;
   *p++ = (uint8_t)uname_len;
   if (uname_len > 0) {
      memcpy(p, username, uname_len);
      p += uname_len;
   }

   cctx->offset = p - cctx->buffer;
   cctx->count++;
   return 0;
}

static int conv_list_callback(const conversation_t *conv, void *ctx) {
   conv_list_ctx_t *cctx = (conv_list_ctx_t *)ctx;
   return pack_conv_entry(cctx, conv, NULL);
}

static int conv_list_all_callback(const conversation_t *conv, const char *username, void *ctx) {
   conv_list_ctx_t *cctx = (conv_list_ctx_t *)ctx;
   return pack_conv_entry(cctx, conv, username);
}

static int handle_list_conversations(int client_fd, const char *payload, uint16_t payload_len) {
   /* Parse filter: 4 bytes user_id, 4 bytes limit, 1 byte include_archived */
   int user_id = 0;
   bool include_archived = false;
   conv_pagination_t pagination = { .limit = 20, .offset = 0 };

   if (payload_len >= 8) {
      int32_t val;
      memcpy(&val, payload, 4);
      user_id = val;
      memcpy(&val, payload + 4, 4);
      pagination.limit = (val > 0) ? val : 20;
   }
   if (payload_len >= 9) {
      include_archived = (payload[8] != 0);
   }

   /* Allocate buffer for response */
   char buffer[8192];
   conv_list_ctx_t ctx = { .buffer = buffer,
                           .buffer_size = sizeof(buffer),
                           .offset = 0,
                           .count = 0,
                           .truncated = false };

   int rc;
   if (user_id > 0) {
      /* Filter by specific user */
      rc = conv_db_list(user_id, include_archived, &pagination, conv_list_callback, &ctx);
   } else {
      /* Admin: list all users' conversations */
      rc = conv_db_list_all(include_archived, &pagination, conv_list_all_callback, &ctx);
   }

   if (rc != AUTH_DB_SUCCESS) {
      return send_list_response(client_fd, ADMIN_RESP_SERVICE_ERROR, NULL, 0, 0, 0);
   }

   uint16_t flags = ctx.truncated ? ADMIN_LIST_FLAG_TRUNCATED : 0;
   return send_list_response(client_fd, ADMIN_RESP_SUCCESS, buffer, (uint16_t)ctx.offset, ctx.count,
                             flags);
}

/* Context for message list callback */
typedef struct {
   char *buffer;
   size_t buffer_size;
   size_t offset;
   int count;
   bool truncated;
} msg_list_ctx_t;

static int msg_list_callback(const conversation_message_t *msg, void *ctx) {
   msg_list_ctx_t *mctx = (msg_list_ctx_t *)ctx;

   /* Pack entry: 1 role_len, N role, 2 content_len, N content, 8 created_at */
   size_t role_len = strlen(msg->role);
   if (role_len > 15)
      role_len = 15;

   size_t content_len = msg->content ? strlen(msg->content) : 0;
   if (content_len > ADMIN_MSG_CONTENT_MAX)
      content_len = ADMIN_MSG_CONTENT_MAX;

   size_t entry_size = 1 + role_len + 2 + content_len + 8;

   if (mctx->offset + entry_size > mctx->buffer_size) {
      mctx->truncated = true;
      return 1; /* Stop iteration */
   }

   char *p = mctx->buffer + mctx->offset;

   *p++ = (uint8_t)role_len;
   memcpy(p, msg->role, role_len);
   p += role_len;
   uint16_t cl = (uint16_t)content_len;
   memcpy(p, &cl, 2);
   p += 2;
   if (content_len > 0 && msg->content) {
      memcpy(p, msg->content, content_len);
   }
   p += content_len;
   int64_t ts = msg->created_at;
   memcpy(p, &ts, 8);
   p += 8;

   mctx->offset = p - mctx->buffer;
   mctx->count++;
   return 0;
}

static int handle_get_conversation(int client_fd, const char *payload, uint16_t payload_len) {
   if (payload_len < 8) {
      return send_list_response(client_fd, ADMIN_RESP_FAILURE, NULL, 0, 0, 0);
   }

   int64_t conv_id;
   memcpy(&conv_id, payload, 8);

   /* Allocate buffer for messages */
   char buffer[32768];
   msg_list_ctx_t ctx = { .buffer = buffer,
                          .buffer_size = sizeof(buffer),
                          .offset = 0,
                          .count = 0,
                          .truncated = false };

   /* Admin access - no ownership check */
   int rc = conv_db_get_messages_admin(conv_id, msg_list_callback, &ctx);
   if (rc == AUTH_DB_NOT_FOUND) {
      return send_list_response(client_fd, ADMIN_RESP_NOT_FOUND, NULL, 0, 0, 0);
   } else if (rc != AUTH_DB_SUCCESS) {
      return send_list_response(client_fd, ADMIN_RESP_SERVICE_ERROR, NULL, 0, 0, 0);
   }

   uint16_t flags = ctx.truncated ? ADMIN_LIST_FLAG_TRUNCATED : 0;
   return send_list_response(client_fd, ADMIN_RESP_SUCCESS, buffer, (uint16_t)ctx.offset, ctx.count,
                             flags);
}

static int handle_delete_conversation(int client_fd, const char *payload, uint16_t payload_len) {
   /* Verify admin auth */
   size_t auth_size = 0;
   if (verify_admin_auth(payload, payload_len, &auth_size) != 0) {
      OLOG_WARNING("DELETE_CONVERSATION: admin auth failed");
      return send_response(client_fd, ADMIN_RESP_UNAUTHORIZED);
   }

   /* Extract conv_id */
   if (payload_len < auth_size + 8) {
      OLOG_WARNING("DELETE_CONVERSATION: payload too short");
      return send_response(client_fd, ADMIN_RESP_FAILURE);
   }

   int64_t conv_id;
   memcpy(&conv_id, payload + auth_size, 8);

   /* Admin access - no ownership check */
   int rc = conv_db_delete_admin(conv_id);

   if (rc == AUTH_DB_NOT_FOUND) {
      OLOG_WARNING("DELETE_CONVERSATION: conversation %lld not found", (long long)conv_id);
      return send_response(client_fd, ADMIN_RESP_NOT_FOUND);
   } else if (rc != AUTH_DB_SUCCESS) {
      OLOG_ERROR("DELETE_CONVERSATION: database error: %d", rc);
      return send_response(client_fd, ADMIN_RESP_SERVICE_ERROR);
   }

   auth_db_log_event("CONVERSATION_DELETED", NULL, NULL, "Deleted conversation");
   OLOG_INFO("DELETE_CONVERSATION: conversation %lld deleted", (long long)conv_id);
   return send_response(client_fd, ADMIN_RESP_SUCCESS);
}

/* =============================================================================
 * Music Database Handlers
 * =============================================================================
 */

static int handle_music_stats(int client_fd) {
   if (!music_db_is_initialized()) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR,
                                "Music database not initialized");
   }

   music_db_stats_t stats;
   if (music_db_get_stats(&stats) != 0) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "Failed to get music stats");
   }

   bool scanner_running = music_scanner_is_running();
   bool initial_complete = music_scanner_initial_scan_complete();

   char response[ADMIN_MSG_CONTENT_MAX];
   snprintf(response, sizeof(response),
            "Music Database Statistics\n"
            "-------------------------\n"
            "Tracks:  %d\n"
            "Artists: %d\n"
            "Albums:  %d\n"
            "Scanner: %s\n"
            "Status:  %s",
            stats.track_count, stats.artist_count, stats.album_count,
            scanner_running ? "running" : "stopped", initial_complete ? "ready" : "indexing");

   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, response);
}

static int handle_music_search(int client_fd, const char *payload, uint16_t len) {
   if (len == 0 || len > 200) {
      return send_text_response(client_fd, ADMIN_RESP_FAILURE, "Invalid search query");
   }

   if (!music_db_is_initialized()) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR,
                                "Music database not initialized");
   }

   /* Allocate results on heap */
   music_search_result_t *results = malloc(50 * sizeof(music_search_result_t));
   if (!results) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "Memory allocation failed");
   }

   int count = 0;
   if (music_db_search(payload, results, 50, &count) != SUCCESS) {
      free(results);
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "Search failed");
   }

   if (count == 0) {
      free(results);
      return send_text_response(client_fd, ADMIN_RESP_SUCCESS, "No results found");
   }

   /* Build response */
   char response[ADMIN_MSG_CONTENT_MAX];
   int offset = snprintf(response, sizeof(response), "Found %d result(s):\n", count);

   for (int i = 0; i < count && offset < (int)sizeof(response) - 100; i++) {
      offset += snprintf(response + offset, sizeof(response) - offset, "%d. %s\n", i + 1,
                         results[i].display_name);
   }

   free(results);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, response);
}

static int handle_music_list(int client_fd, const char *payload, uint16_t len) {
   if (!music_db_is_initialized()) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR,
                                "Music database not initialized");
   }

   /* Parse limit from payload (0 or empty = all tracks) */
   int limit = 0;
   if (len > 0) {
      limit = atoi(payload);
   }
   /* 0 means show all, cap at reasonable max for response size */
   if (limit <= 0) {
      limit = 1000;
   } else if (limit > 1000) {
      limit = 1000;
   }

   /* List all tracks (no pattern filtering) */
   music_search_result_t *results = malloc(limit * sizeof(music_search_result_t));
   if (!results) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "Memory allocation failed");
   }

   int count = 0;
   if (music_db_list(results, limit, &count) != SUCCESS) {
      free(results);
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "List failed");
   }

   if (count == 0) {
      free(results);
      return send_text_response(client_fd, ADMIN_RESP_SUCCESS, "No tracks in database");
   }

   /* Build response */
   char response[ADMIN_MSG_CONTENT_MAX];
   int offset = snprintf(response, sizeof(response), "Showing %d track(s):\n", count);

   for (int i = 0; i < count && offset < (int)sizeof(response) - 100; i++) {
      offset += snprintf(response + offset, sizeof(response) - offset, "%d. %s\n", i + 1,
                         results[i].display_name);
   }

   free(results);
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, response);
}

static int handle_music_rescan(int client_fd) {
   if (!music_scanner_is_running()) {
      return send_text_response(client_fd, ADMIN_RESP_SERVICE_ERROR, "Music scanner not running");
   }

   music_scanner_trigger_rescan();
   return send_text_response(client_fd, ADMIN_RESP_SUCCESS, "Rescan triggered");
}

/* =============================================================================
 * Client Handler
 * =============================================================================
 */

static int handle_client(int client_fd) {
   /* Set receive timeout */
   struct timeval tv;
   tv.tv_sec = ADMIN_CONN_TIMEOUT_SEC;
   tv.tv_usec = 0;
   setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

   /* Validate peer credentials */
   if (validate_peer_credentials(client_fd) != 0) {
      send_response(client_fd, ADMIN_RESP_UNAUTHORIZED);
      return 1;
   }

   /* Read message header */
   admin_msg_header_t header;
   ssize_t n = read(client_fd, &header, sizeof(header));
   if (n != sizeof(header)) {
      OLOG_WARNING("Failed to read admin message header");
      return 1;
   }

   /* Validate protocol version */
   if (header.version != ADMIN_PROTOCOL_VERSION) {
      OLOG_WARNING("Protocol version mismatch: got 0x%02x, expected 0x%02x", header.version,
                   ADMIN_PROTOCOL_VERSION);
      send_response(client_fd, ADMIN_RESP_VERSION_MISMATCH);
      return 1;
   }

   /* Validate payload length */
   if (header.payload_len > ADMIN_MSG_MAX_PAYLOAD) {
      OLOG_WARNING("Payload too large: %u (max %d)", header.payload_len, ADMIN_MSG_MAX_PAYLOAD);
      send_response(client_fd, ADMIN_RESP_FAILURE);
      return 1;
   }

   /* Read payload if present */
   char payload[ADMIN_MSG_MAX_PAYLOAD + 1] = { 0 };
   if (header.payload_len > 0) {
      n = read(client_fd, payload, header.payload_len);
      if (n != header.payload_len) {
         OLOG_WARNING("Failed to read payload: got %zd, expected %u", n, header.payload_len);
         return 1;
      }
   }

   /* Dispatch by message type */
   switch (header.msg_type) {
      case ADMIN_MSG_PING:
         return handle_ping(client_fd);

      case ADMIN_MSG_VALIDATE_SETUP_TOKEN:
         return handle_validate_token(client_fd, payload, header.payload_len);

      case ADMIN_MSG_CREATE_USER:
         return handle_create_user(client_fd, payload, header.payload_len);

      /* Phase 2: User management */
      case ADMIN_MSG_LIST_USERS:
         return handle_list_users(client_fd);

      case ADMIN_MSG_DELETE_USER:
         return handle_delete_user(client_fd, payload, header.payload_len);

      case ADMIN_MSG_CHANGE_PASSWORD:
         return handle_change_password(client_fd, payload, header.payload_len);

      case ADMIN_MSG_UNLOCK_USER:
         return handle_unlock_user(client_fd, payload, header.payload_len);

      /* Phase 2: Session management */
      case ADMIN_MSG_LIST_SESSIONS:
         return handle_list_sessions(client_fd);

      case ADMIN_MSG_REVOKE_SESSION:
         return handle_revoke_session(client_fd, payload, header.payload_len);

      case ADMIN_MSG_REVOKE_USER_SESSIONS:
         return handle_revoke_user_sessions(client_fd, payload, header.payload_len);

      /* Phase 2: Database management */
      case ADMIN_MSG_GET_STATS:
         return handle_get_stats(client_fd);

      case ADMIN_MSG_DB_COMPACT:
         return handle_db_compact(client_fd, payload, header.payload_len);

      case ADMIN_MSG_DB_BACKUP:
         return handle_db_backup(client_fd, payload, header.payload_len);

      case ADMIN_MSG_QUERY_LOG:
         return handle_query_log(client_fd, payload, header.payload_len);

      /* Phase 2: IP management */
      case ADMIN_MSG_LIST_BLOCKED_IPS:
         return handle_list_blocked_ips(client_fd);

      case ADMIN_MSG_UNBLOCK_IP:
         return handle_unblock_ip(client_fd, payload, header.payload_len);

      /* Phase 3: Metrics */
      case ADMIN_MSG_LIST_METRICS:
         return handle_list_metrics(client_fd, payload, header.payload_len);

      case ADMIN_MSG_GET_METRICS_TOTALS:
         return handle_get_metrics_totals(client_fd, payload, header.payload_len);

      /* Phase 4: Conversations */
      case ADMIN_MSG_LIST_CONVERSATIONS:
         return handle_list_conversations(client_fd, payload, header.payload_len);

      case ADMIN_MSG_GET_CONVERSATION:
         return handle_get_conversation(client_fd, payload, header.payload_len);

      case ADMIN_MSG_DELETE_CONVERSATION:
         return handle_delete_conversation(client_fd, payload, header.payload_len);

      /* Phase 5: Music Database */
      case ADMIN_MSG_MUSIC_STATS:
         return handle_music_stats(client_fd);

      case ADMIN_MSG_MUSIC_SEARCH:
         return handle_music_search(client_fd, payload, header.payload_len);

      case ADMIN_MSG_MUSIC_LIST:
         return handle_music_list(client_fd, payload, header.payload_len);

      case ADMIN_MSG_MUSIC_RESCAN:
         return handle_music_rescan(client_fd);

      /* Phase 6: Memory */
      case ADMIN_MSG_MEMORY_RECATEGORIZE:
         return handle_memory_recategorize(client_fd, payload, header.payload_len);

      case ADMIN_MSG_MEMORY_REEXTRACT:
         return handle_memory_reextract(client_fd, payload, header.payload_len);

      case ADMIN_MSG_MEMORY_REEXTRACT_STATUS:
         return handle_memory_reextract_status(client_fd, payload, header.payload_len);

      /* Phase 6.5: Entity-merge alias surface (v43) */
      case ADMIN_MSG_MEMORY_ENTITY_MERGE:
         return handle_memory_entity_merge(client_fd, payload, header.payload_len);

      case ADMIN_MSG_MEMORY_ENTITY_SPLIT:
         return handle_memory_entity_split(client_fd, payload, header.payload_len);

      case ADMIN_MSG_MEMORY_ENTITY_ALIASES:
         return handle_memory_entity_aliases(client_fd, payload, header.payload_len);

      case ADMIN_MSG_MEMORY_ENTITY_HISTORY:
         return handle_memory_entity_history(client_fd, payload, header.payload_len);

      case ADMIN_MSG_MEMORY_ENTITY_LIST:
         return handle_memory_entity_list(client_fd, payload, header.payload_len);

      case ADMIN_MSG_MEMORY_ENTITY_LINK_USER_SELF:
         return handle_memory_entity_link_user_self(client_fd, payload, header.payload_len);

      /* Phase 6.6: Memory cleanup utilities */
      case ADMIN_MSG_MEMORY_CLEANUP_META_FACTS:
         return handle_memory_cleanup_meta_facts(client_fd, payload, header.payload_len);

      /* Phase 6.7: Summary backfill */
      case ADMIN_MSG_MEMORY_SUMMARIZE_MISSING:
         return handle_memory_summarize_missing(client_fd, payload, header.payload_len);

      /* Phase 9: memory→note bridge gloss backfill */
      case ADMIN_MSG_MEMORY_BACKFILL_NOTE_GLOSSES:
         return handle_memory_backfill_note_glosses(client_fd, payload, header.payload_len);

      /* v61: rebuild the document search (FTS) index */
      case ADMIN_MSG_MEMORY_REBUILD_DOCUMENT_FTS:
         return handle_memory_rebuild_document_fts(client_fd, payload, header.payload_len);

      /* Phase 7: Messaging-channels operator commands */
      case ADMIN_MSG_MESSAGING_GENERATE_LINK_CODE:
         return handle_messaging_generate_link_code(client_fd, payload, header.payload_len);
      case ADMIN_MSG_MESSAGING_LIST_CHANNELS:
         return handle_messaging_list_channels(client_fd, payload, header.payload_len);
      case ADMIN_MSG_MESSAGING_UNLINK_CHANNEL:
         return handle_messaging_unlink_channel(client_fd, payload, header.payload_len);
      case ADMIN_MSG_MESSAGING_LINK_ATTEMPTS:
         return handle_messaging_link_attempts(client_fd, payload, header.payload_len);
      case ADMIN_MSG_MESSAGING_REENABLE_CHANNEL:
         return handle_messaging_reenable_channel(client_fd, payload, header.payload_len);

      /* Phase 8: OTA operator commands */
      case ADMIN_MSG_OTA_LIST:
         return handle_ota_list_cmd(client_fd);
      case ADMIN_MSG_OTA_PUSH:
         return handle_ota_push_cmd(client_fd, payload, header.payload_len);
      case ADMIN_MSG_OTA_RESCAN:
         return handle_ota_rescan_cmd(client_fd);
      case ADMIN_MSG_OTA_PUSH_ALL:
         return handle_ota_push_all_cmd(client_fd, payload, header.payload_len);
      case ADMIN_MSG_OTA_ROLLOUT_STATUS:
         return handle_ota_rollout_status_cmd(client_fd);
      case ADMIN_MSG_OTA_ROLLOUT_ABORT:
         return handle_ota_rollout_abort_cmd(client_fd);

#ifdef DAWN_ENABLE_MCP_BRIDGE_TOOL
      case ADMIN_MSG_MCP_LIST:
         return handle_mcp_list(client_fd, payload, header.payload_len);
      case ADMIN_MSG_MCP_STATUS:
         return handle_mcp_status(client_fd, payload, header.payload_len);
      case ADMIN_MSG_MCP_GRANT:
         return handle_mcp_grant(client_fd, payload, header.payload_len);
      case ADMIN_MSG_MCP_REVOKE:
         return handle_mcp_revoke(client_fd, payload, header.payload_len);
      case ADMIN_MSG_MCP_RESET:
         return handle_mcp_reset(client_fd, payload, header.payload_len);
#endif

#ifdef DAWN_ENABLE_CODE_PROJECTS
      case ADMIN_MSG_CODE_PROJ_LIST:
         return handle_code_proj_list(client_fd, payload, header.payload_len);
      case ADMIN_MSG_CODE_PROJ_IMPORT:
         return handle_code_proj_import(client_fd, payload, header.payload_len);
      case ADMIN_MSG_CODE_PROJ_REFRESH:
         return handle_code_proj_refresh(client_fd, payload, header.payload_len);
      case ADMIN_MSG_CODE_PROJ_DELETE:
         return handle_code_proj_delete(client_fd, payload, header.payload_len);
#endif

      default:
         OLOG_WARNING("Unknown message type: 0x%02x", header.msg_type);
         send_response(client_fd, ADMIN_RESP_FAILURE);
         return 1;
   }
}

/* =============================================================================
 * Listener Thread
 * =============================================================================
 */

static void *listener_thread_func(void *arg) {
   (void)arg;

   OLOG_INFO("Admin socket listener thread running");

   pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
   pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

   while (!s_shutdown_requested) {
      pthread_testcancel();

      int fd = s_listen_fd;
      int pipe_fd = s_shutdown_pipe[0];

      if (fd < 0 || pipe_fd < 0 || s_shutdown_requested) {
         break;
      }

      fd_set read_fds;
      FD_ZERO(&read_fds);
      FD_SET(fd, &read_fds);
      FD_SET(pipe_fd, &read_fds);

      int max_fd = (fd > pipe_fd) ? fd : pipe_fd;

      struct timeval timeout;
      timeout.tv_sec = 60;
      timeout.tv_usec = 0;

      int result = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);

      if (result < 0) {
         if (errno == EINTR || errno == EBADF || s_shutdown_requested) {
            continue;
         }
         OLOG_ERROR("Admin socket select() failed: %s", strerror(errno));
         break;
      }

      if (result == 0) {
         /* Timeout - just continue */
         continue;
      }

      /* Check shutdown pipe */
      if (FD_ISSET(pipe_fd, &read_fds)) {
         OLOG_INFO("Admin socket received shutdown signal");
         break;
      }

      /* Accept connection */
      if (FD_ISSET(fd, &read_fds)) {
         struct sockaddr_un client_addr;
         socklen_t addr_len = sizeof(client_addr);

         int client_fd = accept(fd, (struct sockaddr *)&client_addr, &addr_len);
         if (client_fd < 0) {
            if (errno == EINTR || s_shutdown_requested) {
               continue;
            }
            OLOG_ERROR("Admin socket accept failed: %s", strerror(errno));
            continue;
         }

         /* Handle client (single-threaded, one at a time) */
         handle_client(client_fd);
         close(client_fd);
      }
   }

   OLOG_INFO("Admin socket listener thread exiting");
   return NULL;
}
