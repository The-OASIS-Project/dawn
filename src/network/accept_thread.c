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
 * Accept Thread for multi-client support.
 * Handles TCP connection acceptance and dispatches clients to worker pool.
 *
 * DESIGN:
 *   - Uses select() with 60s timeout for periodic cleanup
 *   - Creates sessions for new clients
 *   - Assigns clients to worker pool (or sends NACK if busy)
 *   - Calls session_cleanup_expired() on select timeout
 *
 * THREAD SAFETY:
 *   - Accept thread owns the listening socket
 *   - Worker assignment through worker_pool API (thread-safe)
 *   - Session creation through session_manager API (thread-safe)
 */

#include "network/accept_thread.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/session_manager.h"
#include "core/worker_pool.h"
#include "logging.h"
#include "network/dawn_server.h"  // For PACKET_TYPE_NACK, protocol functions

// =============================================================================
// Module State
// =============================================================================

static pthread_t accept_thread;
static volatile bool thread_running = false;
static volatile bool shutdown_requested = false;

static int listen_fd = -1;
static uint16_t listen_port = ACCEPT_THREAD_PORT;

// Self-pipe for reliable shutdown signaling
static int shutdown_pipe[2] = { -1, -1 };

// Metrics
static uint32_t connections_accepted = 0;
static uint32_t connections_rejected = 0;

// =============================================================================
// Forward Declarations
// =============================================================================

static void *accept_thread_func(void *arg);
static int create_listening_socket(void);
static int handle_new_connection(int client_fd, struct sockaddr_in *client_addr);
static void send_nack(int client_fd);

// =============================================================================
// Lifecycle Functions
// =============================================================================

int accept_thread_start(asr_engine_type_t engine_type, const char *model_path) {
   if (thread_running) {
      LOG_WARNING("Accept thread already running");
      return 0;
   }

   LOG_INFO("Starting accept thread on port %d", listen_port);

   // Create shutdown pipe for reliable signal delivery
   if (pipe(shutdown_pipe) != 0) {
      LOG_ERROR("Failed to create shutdown pipe: %s", strerror(errno));
      return 1;
   }

   // Session manager is already initialized by dawn.c before calling this function

   // Initialize worker pool (EAGER: creates ASR contexts now)
   if (worker_pool_init(engine_type, model_path) != 0) {
      LOG_ERROR("Failed to initialize worker pool");
      close(shutdown_pipe[0]);
      close(shutdown_pipe[1]);
      shutdown_pipe[0] = shutdown_pipe[1] = -1;
      return 1;
   }

   // Create listening socket
   listen_fd = create_listening_socket();
   if (listen_fd < 0) {
      LOG_ERROR("Failed to create listening socket");
      worker_pool_shutdown();
      close(shutdown_pipe[0]);
      close(shutdown_pipe[1]);
      shutdown_pipe[0] = shutdown_pipe[1] = -1;
      return 1;
   }

   // Reset state
   shutdown_requested = false;
   connections_accepted = 0;
   connections_rejected = 0;

   // Start accept thread
   if (pthread_create(&accept_thread, NULL, accept_thread_func, NULL) != 0) {
      LOG_ERROR("Failed to create accept thread");
      close(listen_fd);
      listen_fd = -1;
      worker_pool_shutdown();
      close(shutdown_pipe[0]);
      close(shutdown_pipe[1]);
      shutdown_pipe[0] = shutdown_pipe[1] = -1;
      return 1;
   }

   thread_running = true;
   LOG_INFO("Accept thread started successfully");
   return 0;
}

void accept_thread_stop(void) {
   if (!thread_running) {
      return;
   }

   LOG_INFO("Stopping accept thread...");

   // Signal shutdown
   shutdown_requested = true;

   // Write to shutdown pipe to wake up select() immediately
   if (shutdown_pipe[1] >= 0) {
      char byte = 1;
      ssize_t written = write(shutdown_pipe[1], &byte, 1);
      (void)written;  // Ignore result, we're shutting down anyway
   }

   // Close listening socket as backup
   if (listen_fd >= 0) {
      close(listen_fd);
      listen_fd = -1;
   }

   // Wait for accept thread to exit
   pthread_join(accept_thread, NULL);
   thread_running = false;

   // Close shutdown pipe
   if (shutdown_pipe[0] >= 0) {
      close(shutdown_pipe[0]);
      shutdown_pipe[0] = -1;
   }
   if (shutdown_pipe[1] >= 0) {
      close(shutdown_pipe[1]);
      shutdown_pipe[1] = -1;
   }

   // Shutdown worker pool (waits for workers to finish)
   worker_pool_shutdown();

   // NOTE: session_manager_cleanup() is NOT called here.
   // The session manager was initialized by dawn.c (which owns the local session),
   // so dawn.c is responsible for cleanup after saving conversation history.

   LOG_INFO("Accept thread stopped");
}

bool accept_thread_is_running(void) {
   return thread_running;
}

// =============================================================================
// Configuration Functions
// =============================================================================

void accept_thread_set_port(uint16_t port) {
   if (!thread_running) {
      listen_port = port;
   } else {
      LOG_WARNING("Cannot change port while accept thread is running");
   }
}

uint16_t accept_thread_get_port(void) {
   return listen_port;
}

// =============================================================================
// Metrics
// =============================================================================

uint32_t accept_thread_connections_accepted(void) {
   return connections_accepted;
}

uint32_t accept_thread_connections_rejected(void) {
   return connections_rejected;
}

// =============================================================================
// Internal Functions
// =============================================================================

/**
 * @brief Main accept thread function
 *
 * Uses select() with 60s timeout:
 * - On activity: accept connection, create session, dispatch to worker
 * - On timeout: call session_cleanup_expired()
 * - On shutdown: exit loop
 */
static void *accept_thread_func(void *arg) {
   (void)arg;

   LOG_INFO("Accept thread running");

   while (!shutdown_requested) {
      // Capture fds locally to avoid race with shutdown
      int fd = listen_fd;
      int pipe_fd = shutdown_pipe[0];

      // Check if socket was closed (shutdown in progress)
      if (fd < 0 || pipe_fd < 0 || shutdown_requested) {
         break;
      }

      fd_set read_fds;
      FD_ZERO(&read_fds);
      FD_SET(fd, &read_fds);
      FD_SET(pipe_fd, &read_fds);

      // Calculate max fd for select
      int max_fd = (fd > pipe_fd) ? fd : pipe_fd;

      struct timeval timeout;
      timeout.tv_sec = ACCEPT_THREAD_TIMEOUT_SEC;
      timeout.tv_usec = 0;

      int result = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);

      if (result < 0) {
         if (errno == EINTR || errno == EBADF || shutdown_requested) {
            // Interrupted, fd closed, or shutdown - check shutdown flag
            continue;
         }
         LOG_ERROR("Accept select() failed: %s", strerror(errno));
         break;
      }

      if (result == 0) {
         // Timeout - run periodic cleanup
         session_cleanup_expired();
         continue;
      }

      // Check if shutdown was signaled via pipe
      if (FD_ISSET(pipe_fd, &read_fds)) {
         LOG_INFO("Accept thread received shutdown signal");
         break;
      }

      // Activity on listening socket - accept connection
      if (FD_ISSET(fd, &read_fds)) {
         struct sockaddr_in client_addr;
         socklen_t addr_len = sizeof(client_addr);

         int client_fd = accept(fd, (struct sockaddr *)&client_addr, &addr_len);
         if (client_fd < 0) {
            if (errno == EINTR || shutdown_requested) {
               continue;
            }
            LOG_ERROR("Accept failed: %s", strerror(errno));
            continue;
         }

         // Handle new connection
         handle_new_connection(client_fd, &client_addr);
      }
   }

   LOG_INFO("Accept thread exiting");
   return NULL;
}

/**
 * @brief Create and bind the listening socket
 */
static int create_listening_socket(void) {
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0) {
      LOG_ERROR("Failed to create socket: %s", strerror(errno));
      return -1;
   }

   // Allow address reuse
   int opt = 1;
   if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
      LOG_WARNING("Failed to set SO_REUSEADDR: %s", strerror(errno));
   }

   struct sockaddr_in addr;
   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = INADDR_ANY;
   addr.sin_port = htons(listen_port);

   if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
      LOG_ERROR("Failed to bind to port %d: %s", listen_port, strerror(errno));
      close(fd);
      return -1;
   }

   if (listen(fd, ACCEPT_THREAD_BACKLOG) < 0) {
      LOG_ERROR("Failed to listen: %s", strerror(errno));
      close(fd);
      return -1;
   }

   LOG_INFO("Listening on port %d", listen_port);
   return fd;
}

/**
 * @brief Handle a new client connection
 *
 * 1. Create session for client
 * 2. Try to assign to worker pool
 * 3. If all workers busy, send NACK and close
 *
 * @param client_fd Client socket
 * @param client_addr Client address
 * @return 0 on success, 1 on failure
 */
static int handle_new_connection(int client_fd, struct sockaddr_in *client_addr) {
   char client_ip[INET_ADDRSTRLEN];
   inet_ntop(AF_INET, &client_addr->sin_addr, client_ip, sizeof(client_ip));

   LOG_INFO("New connection from %s:%d (fd=%d)", client_ip, ntohs(client_addr->sin_port),
            client_fd);

   // Disable Nagle's algorithm for low-latency protocol responses
   int flag = 1;
   setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

   // Set socket timeout
   struct timeval timeout;
   timeout.tv_sec = 30;
   timeout.tv_usec = 0;
   setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
   setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

   // Get or create session for this client using IP-based persistence
   // DAP1 clients don't send a REGISTER message, so we use IP address
   // to allow conversation history to persist across reconnections.
   // Phase 5 will add DAP2 protocol with proper UUID-based identity.
   session_t *session = session_get_or_create_dap(client_fd, client_ip);
   if (!session) {
      LOG_ERROR("Failed to get/create session for client %s", client_ip);
      send_nack(client_fd);
      close(client_fd);
      connections_rejected++;
      return 1;
   }

   // Try to assign to worker pool
   if (worker_pool_assign_client(client_fd, session) != 0) {
      LOG_WARNING("All workers busy, rejecting client %s", client_ip);
      send_nack(client_fd);
      // Just release our ref - don't destroy session
      // For new sessions: will be cleaned up by session_cleanup_expired()
      // For reconnections: session persists with its previous owner
      // This avoids race condition where destroy could hang waiting for other refs
      session_release(session);
      close(client_fd);
      connections_rejected++;
      return 1;
   }

   connections_accepted++;
   LOG_INFO("Client %s assigned to worker", client_ip);
   return 0;
}

/**
 * @brief Send NACK packet to client (server busy)
 *
 * Uses DAP protocol format from dawn_server.h
 */
static void send_nack(int client_fd) {
   uint8_t header[PACKET_HEADER_SIZE];
   dawn_build_packet_header(header, 0, PACKET_TYPE_NACK, 0);
   send(client_fd, header, PACKET_HEADER_SIZE, MSG_NOSIGNAL);
}
