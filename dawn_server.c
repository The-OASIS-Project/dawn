/**
 * @file dawn_server.c
 * @brief DAWN Audio Protocol Server Implementation
 *
 * Implements the DAWN Audio Protocol for receiving audio from ESP32 clients,
 * processing through the DAWN AI pipeline, and returning synthesized responses.
 *
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
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <time.h>
#include <signal.h>

#include "dawn_server.h"
#include "logging.h"

// === Global Server State ===
static pthread_t server_thread;
static volatile int server_running = 0;
static int server_socket_fd = -1;
static pthread_mutex_t server_mutex = PTHREAD_MUTEX_INITIALIZER;

// === Audio Processing Integration ===
static dawn_audio_processor_callback_t audio_processor_callback = NULL;
static pthread_mutex_t callback_mutex = PTHREAD_MUTEX_INITIALIZER;

// === Forward Declarations ===
static void *dawn_server_thread(void *arg);
static int dawn_handle_client_connection(int client_fd, struct sockaddr_in *client_addr);
static int dawn_handle_handshake(dawn_client_session_t *session);
static int dawn_receive_data_chunks(dawn_client_session_t *session, uint8_t **data_out, size_t *size_out);
static int dawn_send_data_chunks(dawn_client_session_t *session, const uint8_t *data, size_t size);
static int dawn_send_ack(int socket_fd);
static int dawn_send_nack(int socket_fd);

// === Utility Functions ===

uint16_t dawn_calculate_checksum(const uint8_t *data, size_t length) {
    if (!data || length == 0) return 0;

    uint16_t sum1 = 0;
    uint16_t sum2 = 0;

    for (size_t i = 0; i < length; i++) {
        sum1 = (sum1 + data[i]) % 255;
        sum2 = (sum2 + sum1) % 255;
    }

    return (sum2 << 8) | sum1;
}

void dawn_build_packet_header(uint8_t *header, uint32_t data_length, 
                             uint8_t packet_type, uint16_t checksum) {
    if (!header) return;

    // 4 bytes: data length (big-endian)
    header[0] = (data_length >> 24) & 0xFF;
    header[1] = (data_length >> 16) & 0xFF;
    header[2] = (data_length >> 8) & 0xFF;
    header[3] = data_length & 0xFF;

    // 1 byte: protocol version
    header[4] = PROTOCOL_VERSION;

    // 1 byte: packet type
    header[5] = packet_type;

    // 2 bytes: checksum (big-endian)
    header[6] = (checksum >> 8) & 0xFF;
    header[7] = checksum & 0xFF;
}

int dawn_parse_packet_header(const uint8_t *header, dawn_packet_header_t *parsed) {
    if (!header || !parsed) {
        return DAWN_ERROR;
    }

    // Parse data length (big-endian)
    parsed->data_length = ((uint32_t)header[0] << 24) |
                         ((uint32_t)header[1] << 16) |
                         ((uint32_t)header[2] << 8) |
                         (uint32_t)header[3];

    // Parse protocol version
    parsed->protocol_version = header[4];

    // Verify protocol version
    if (parsed->protocol_version != PROTOCOL_VERSION) {
        LOG_ERROR("Invalid protocol version: 0x%02X (expected 0x%02X)",
               parsed->protocol_version, PROTOCOL_VERSION);
        return DAWN_ERROR_PROTOCOL;
    }

    // Parse packet type and checksum
    parsed->packet_type = header[5];
    parsed->checksum = ((uint16_t)header[6] << 8) | (uint16_t)header[7];

    return DAWN_SUCCESS;
}

int dawn_read_exact(int socket_fd, uint8_t *buffer, size_t n) {
    if (!buffer || n == 0) return DAWN_ERROR;

    size_t total_read = 0;

    while (total_read < n) {
        ssize_t bytes_read = recv(socket_fd, buffer + total_read, n - total_read, 0);

        if (bytes_read <= 0) {
            if (bytes_read == 0) {
                return DAWN_ERROR;
            } else if (errno == EWOULDBLOCK || errno == EAGAIN) {
                return DAWN_ERROR_TIMEOUT;
            } else {
                LOG_ERROR("Socket read error: %s", strerror(errno));
                return DAWN_ERROR_SOCKET;
            }
        }

        total_read += bytes_read;
    }

    return DAWN_SUCCESS;
}

int dawn_send_exact(int socket_fd, const uint8_t *buffer, size_t n) {
    if (!buffer || n == 0) return DAWN_ERROR;

    size_t total_sent = 0;

    while (total_sent < n) {
        ssize_t bytes_sent = send(socket_fd, buffer + total_sent, n - total_sent, 0);

        if (bytes_sent <= 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                return DAWN_ERROR_TIMEOUT;
            } else if (errno == EPIPE) {
                return DAWN_ERROR_SOCKET;
            } else {
                LOG_ERROR("Socket write error: %s", strerror(errno));
                return DAWN_ERROR_SOCKET;
            }
        }

        total_sent += bytes_sent;
    }

    return DAWN_SUCCESS;
}

// === Protocol Implementation ===

static int dawn_send_ack(int socket_fd) {
    uint8_t header[PACKET_HEADER_SIZE];
    dawn_build_packet_header(header, 0, PACKET_TYPE_ACK, 0);
    return dawn_send_exact(socket_fd, header, PACKET_HEADER_SIZE);
}

static int dawn_send_nack(int socket_fd) {
    uint8_t header[PACKET_HEADER_SIZE];
    dawn_build_packet_header(header, 0, PACKET_TYPE_NACK, 0);
    return dawn_send_exact(socket_fd, header, PACKET_HEADER_SIZE);
}

static int dawn_handle_handshake(dawn_client_session_t *session) {
    if (!session) return DAWN_ERROR;

    // Read handshake header
    uint8_t header_buffer[PACKET_HEADER_SIZE];
    int result = dawn_read_exact(session->socket_fd, header_buffer, PACKET_HEADER_SIZE);
    if (result != DAWN_SUCCESS) {
        return result;
    }

    // Parse header
    dawn_packet_header_t header;
    result = dawn_parse_packet_header(header_buffer, &header);
    if (result != DAWN_SUCCESS || header.packet_type != PACKET_TYPE_HANDSHAKE) {
        LOG_WARNING("%s: Invalid handshake header", session->client_ip);
        return DAWN_ERROR_PROTOCOL;
    }

    // Read handshake data
    if (header.data_length != 4) {
        LOG_WARNING("%s: Invalid handshake data length: %u", session->client_ip, header.data_length);
        return DAWN_ERROR_PROTOCOL;
    }

    uint8_t magic_bytes[4];
    result = dawn_read_exact(session->socket_fd, magic_bytes, 4);
    if (result != DAWN_SUCCESS) {
        return result;
    }

    // Verify checksum
    uint16_t actual_checksum = dawn_calculate_checksum(magic_bytes, 4);
    if (actual_checksum != header.checksum) {
        LOG_WARNING("%s: Handshake checksum mismatch", session->client_ip);
        return DAWN_ERROR_PROTOCOL;
    }

    // Verify magic bytes
    if (magic_bytes[0] != MAGIC_BYTE_0 || magic_bytes[1] != MAGIC_BYTE_1 ||
        magic_bytes[2] != MAGIC_BYTE_2 || magic_bytes[3] != MAGIC_BYTE_3) {
        LOG_WARNING("%s: Invalid magic bytes", session->client_ip);
        return DAWN_ERROR_PROTOCOL;
    }

    // Initialize sequence counters
    session->send_sequence = 0;
    session->receive_sequence = 0;

    // Client synchronization delays
    usleep(50000); // 50ms

    // Send ACK
    result = dawn_send_ack(session->socket_fd);
    if (result != DAWN_SUCCESS) {
        return result;
    }

    usleep(50000); // 50ms

    return DAWN_SUCCESS;
}

#define MAX_SEQUENCE_RETRIES 10
#define MAX_PACKETS_PER_TRANSFER 10000  // ~80MB at 8KB chunks

static int dawn_receive_data_chunks(dawn_client_session_t *session, uint8_t **data_out, size_t *size_out) {
    if (!session || !data_out || !size_out) return DAWN_ERROR;

    // Allocate receive buffer
    uint8_t *buffer = malloc(MAX_DATA_SIZE);
    if (!buffer) {
        LOG_ERROR("%s: Failed to allocate receive buffer", session->client_ip);
        return DAWN_ERROR_MEMORY;
    }

    int packet_count = 0;
    size_t total_received = 0;

    while (1) {
        packet_count++;
        if (packet_count > MAX_PACKETS_PER_TRANSFER) {
            LOG_ERROR("%s: Too many packets in transfer, aborting", session->client_ip);
            free(buffer);
            return DAWN_ERROR_PROTOCOL;
        }

        // Read packet header
        uint8_t header_buffer[PACKET_HEADER_SIZE];
        int result = dawn_read_exact(session->socket_fd, header_buffer, PACKET_HEADER_SIZE);
        if (result != DAWN_SUCCESS) {
            free(buffer);
            return result;
        }

        // Parse header
        dawn_packet_header_t header;
        result = dawn_parse_packet_header(header_buffer, &header);
        if (result != DAWN_SUCCESS) {
            dawn_send_nack(session->socket_fd);
            free(buffer);
            return result;
        }

        // Validate data length
        if (header.data_length > PACKET_MAX_SIZE) {
            LOG_WARNING("%s: Packet too large (%u bytes)", session->client_ip, header.data_length);
            dawn_send_nack(session->socket_fd);
            free(buffer);
            return DAWN_ERROR_PROTOCOL;
        }

        if (total_received + header.data_length > MAX_DATA_SIZE) {
            LOG_WARNING("%s: Total data exceeds maximum", session->client_ip);
            dawn_send_nack(session->socket_fd);
            free(buffer);
            return DAWN_ERROR_PROTOCOL;
        }

        // Read sequence number
        uint8_t seq_bytes[2];
        result = dawn_read_exact(session->socket_fd, seq_bytes, 2);
        if (result != DAWN_SUCCESS) {
            dawn_send_nack(session->socket_fd);
            free(buffer);
            return result;
        }

        uint16_t packet_sequence = ((uint16_t)seq_bytes[0] << 8) | (uint16_t)seq_bytes[1];

        // Verify sequence number with max retry
        int sequence_retry_count = 0;
        if (packet_sequence != session->receive_sequence) {
            LOG_WARNING("%s: Sequence mismatch: expected %u, got %u (retry %d/%d)",
                       session->client_ip, session->receive_sequence,
                       packet_sequence, sequence_retry_count, MAX_SEQUENCE_RETRIES);
            dawn_send_nack(session->socket_fd);

            sequence_retry_count++;
            if (sequence_retry_count >= MAX_SEQUENCE_RETRIES) {
                LOG_ERROR("%s: Too many sequence errors, aborting", session->client_ip);
                free(buffer);
                return DAWN_ERROR_PROTOCOL;
            }
            continue;
        }

        // Reset counter on successful packet
        sequence_retry_count = 0;

        // Read chunk data
        uint8_t *chunk_buffer = malloc(header.data_length);
        if (!chunk_buffer) {
            dawn_send_nack(session->socket_fd);
            free(buffer);
            return DAWN_ERROR_MEMORY;
        }

        result = dawn_read_exact(session->socket_fd, chunk_buffer, header.data_length);
        if (result != DAWN_SUCCESS) {
            dawn_send_nack(session->socket_fd);
            free(chunk_buffer);
            free(buffer);
            return result;
        }

        // Verify checksum
        uint16_t actual_checksum = dawn_calculate_checksum(chunk_buffer, header.data_length);
        if (actual_checksum != header.checksum) {
            dawn_send_nack(session->socket_fd);
            free(chunk_buffer);
            continue;
        }

        // Send ACK
        result = dawn_send_ack(session->socket_fd);
        if (result != DAWN_SUCCESS) {
            free(chunk_buffer);
            free(buffer);
            return result;
        }

        // Append data to buffer
        memcpy(buffer + total_received, chunk_buffer, header.data_length);
        total_received += header.data_length;
        session->receive_sequence++;

        free(chunk_buffer);

        // Check if this was the last packet
        if (header.packet_type == PACKET_TYPE_DATA_END) {
            break;
        }
    }

    *data_out = buffer;
    *size_out = total_received;
    return DAWN_SUCCESS;
}

/**
 * @brief Send data to client in chunks with retry logic
 *
 * Breaks large data into PACKET_MAX_SIZE chunks and sends them sequentially
 * with sequence numbers. Implements retry logic with exponential backoff on
 * ACK timeout or NACK reception.
 *
 * BLOCKING: This function blocks until all data is sent or max retries exceeded.
 * Total blocking time can be significant for large transfers:
 * - 1MB transfer = ~130 chunks
 * - With retries: potentially minutes
 *
 * THREAD SAFETY: Not thread-safe. Session must not be accessed concurrently.
 *
 * MEMORY: Does not allocate or free any memory. Data pointer must remain valid
 * for the entire call duration.
 *
 * @param session Client session with initialized socket and sequence counters
 * @param data Pointer to data to send (must remain valid for call duration)
 * @param size Total size of data in bytes (must be > 0)
 *
 * @return DAWN_SUCCESS on complete transmission
 * @return DAWN_ERROR on invalid parameters
 * @return DAWN_ERROR_SOCKET on unrecoverable socket error
 * @return DAWN_ERROR_TIMEOUT if max retries exceeded
 *
 * @pre session != NULL && session->socket_fd >= 0
 * @pre data != NULL && size > 0
 * @post session->send_sequence incremented by number of chunks sent
 * @post All data transmitted or error occurred
 *
 * @note Large transfers may block for extended periods
 * @note Logs progress for transfers > 50KB
 * @note Uses 2-second ACK timeout per chunk
 * @note Maximum 5 retries per chunk with exponential backoff
 */
static int dawn_send_data_chunks(dawn_client_session_t *session, const uint8_t *data, size_t size) {
    if (!session || !data || size == 0) return DAWN_ERROR;

    // Client synchronization delay
    usleep(100000); // 100ms

    size_t total_sent = 0;

    while (total_sent < size) {
        size_t remaining = size - total_sent;
        size_t current_chunk_size = (remaining > PACKET_MAX_SIZE) ? PACKET_MAX_SIZE : remaining;
        int is_last_chunk = (total_sent + current_chunk_size >= size);

        uint8_t packet_type = is_last_chunk ? PACKET_TYPE_DATA_END : PACKET_TYPE_DATA;
        const uint8_t *chunk_data = data + total_sent;
        uint16_t checksum = dawn_calculate_checksum(chunk_data, current_chunk_size);

        // Build header and sequence bytes
        uint8_t header[PACKET_HEADER_SIZE];
        uint8_t sequence_bytes[2];

        dawn_build_packet_header(header, current_chunk_size, packet_type, checksum);
        sequence_bytes[0] = (session->send_sequence >> 8) & 0xFF;
        sequence_bytes[1] = session->send_sequence & 0xFF;

        // Retry logic
        int chunk_sent = 0;
        for (int retry = 0; retry < MAX_RETRIES && !chunk_sent; retry++) {
            if (retry > 0) {
                int delay_ms = 100 * (1 << retry);
                if (delay_ms > 2000) delay_ms = 2000;
                usleep(delay_ms * 1000);
            }

            // Send header, sequence, and data
            int result = dawn_send_exact(session->socket_fd, header, PACKET_HEADER_SIZE);
            if (result != DAWN_SUCCESS) continue;

            result = dawn_send_exact(session->socket_fd, sequence_bytes, 2);
            if (result != DAWN_SUCCESS) continue;

            result = dawn_send_exact(session->socket_fd, chunk_data, current_chunk_size);
            if (result != DAWN_SUCCESS) continue;

            // Wait for ACK with timeout
            struct timeval timeout;
            timeout.tv_sec = 2;
            timeout.tv_usec = 0;
            if (setsockopt(session->socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
                LOG_WARNING("%s: Failed to set receive timeout: %s", __func__, strerror(errno));
            }

            uint8_t ack_header[PACKET_HEADER_SIZE];
            result = dawn_read_exact(session->socket_fd, ack_header, PACKET_HEADER_SIZE);

            // Reset timeout
            timeout.tv_sec = SOCKET_TIMEOUT_SEC;
            timeout.tv_usec = 0;
            if (setsockopt(session->socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
                LOG_WARNING("%s: Failed to set send timeout: %s", __func__, strerror(errno));
            }

            if (result != DAWN_SUCCESS) continue;

            dawn_packet_header_t ack_info;
            result = dawn_parse_packet_header(ack_header, &ack_info);
            if (result != DAWN_SUCCESS) continue;

            if (ack_info.packet_type == PACKET_TYPE_ACK) {
                chunk_sent = 1;
                break;
            } else if (ack_info.packet_type == PACKET_TYPE_NACK) {
                continue;
            }
        }

        if (!chunk_sent) {
            LOG_ERROR("%s: Failed to send chunk after %d retries", 
                   session->client_ip, MAX_RETRIES);
            return DAWN_ERROR;
        }

        total_sent += current_chunk_size;
        session->send_sequence++;

        // Progress report for large transfers
        if (size > 50000) {
            int percent = (int)((total_sent * 100) / size);
            LOG_INFO("%s: Sent %zu/%zu bytes (%d%%)",
                   session->client_ip, total_sent, size, percent);
        }
    }

    return DAWN_SUCCESS;
}

// === Client Connection Handler ===

static int dawn_handle_client_connection(int client_fd, struct sockaddr_in *client_addr) {
    dawn_client_session_t session;
    memset(&session, 0, sizeof(session));

    // Initialize session
    session.socket_fd = client_fd;
    session.addr = *client_addr;
    const char *ip_str = inet_ntop(AF_INET, &client_addr->sin_addr,
                              session.client_ip, sizeof(session.client_ip));
    if (ip_str == NULL) {
        LOG_ERROR("Failed to convert client IP address: %s", strerror(errno));
        snprintf(session.client_ip, sizeof(session.client_ip), "unknown");
    }
    LOG_INFO("%s: Client connected", session.client_ip);

    // Set socket timeout
    struct timeval timeout;
    timeout.tv_sec = SOCKET_TIMEOUT_SEC;
    timeout.tv_usec = 0;
    if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        LOG_WARNING("%s: Failed to set receive timeout: %s", session.client_ip, strerror(errno));
    }
    if (setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        LOG_WARNING("%s: Failed to set send timeout: %s", session.client_ip, strerror(errno));
    }

    int result = DAWN_SUCCESS;
    uint8_t *received_data = NULL;
    size_t received_size = 0;

    do {
        // Step 1: Handle handshake
        result = dawn_handle_handshake(&session);
        if (result != DAWN_SUCCESS) {
            LOG_WARNING("%s: Handshake failed", session.client_ip);
            break;
        }

        // Step 2: Receive data
        result = dawn_receive_data_chunks(&session, &received_data, &received_size);
        if (result != DAWN_SUCCESS) {
            LOG_WARNING("%s: Failed to receive data", session.client_ip);
            break;
        }

        LOG_INFO("%s: Received %zu bytes", session.client_ip, received_size);

        // === Audio Processing Integration ===
        pthread_mutex_lock(&callback_mutex);
        dawn_audio_processor_callback_t current_callback = audio_processor_callback;
        pthread_mutex_unlock(&callback_mutex);

        uint8_t *response_data = NULL;
        size_t response_size = 0;

        if (current_callback != NULL) {
            // Call the audio processor
            response_data = current_callback(received_data, received_size, 
                                           session.client_ip, &response_size);

            if (response_data != NULL && response_size > 0) {
                LOG_INFO("%s: Audio processor returned %zu bytes",
                       session.client_ip, response_size);
            } else {
                // Echo fallback
                response_data = received_data;
                response_size = received_size;
                LOG_INFO("%s: Using echo fallback", session.client_ip);
            }
        } else {
            // Echo mode (no processor)
            response_data = received_data;
            response_size = received_size;
        }

        // Step 3: Send response
        result = dawn_send_data_chunks(&session, response_data, response_size);
        if (result != DAWN_SUCCESS) {
            LOG_WARNING("%s: Failed to send response", session.client_ip);
        } else {
            LOG_INFO("%s: Response sent successfully", session.client_ip);
        }

        // Clean up callback-allocated data
        if (current_callback != NULL && response_data != received_data && response_data != NULL) {
            free(response_data);
        }

    } while (0);

    // Cleanup
    if (received_data) {
        free(received_data);
    }

    close(client_fd);

    if (result == DAWN_SUCCESS) {
        LOG_INFO("%s: Connection completed", session.client_ip);
    } else {
        LOG_ERROR("%s: Connection failed", session.client_ip);
    }

    return result;
}

// === Server Thread ===

static void *dawn_server_thread(void *arg) {
    (void)arg;

    LOG_INFO("Voice Assistant Server starting");
    LOG_INFO("Protocol: v0x%02X, Host: %s:%d", 
           PROTOCOL_VERSION, SERVER_HOST, SERVER_PORT);

    // Create socket
    server_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_fd < 0) {
        LOG_ERROR("Failed to create socket: %s", strerror(errno));
        server_running = 0;
        return NULL;
    }

    // Set socket options
    int opt = 1;
    if (setsockopt(server_socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOG_WARNING("Failed to set SO_REUSEADDR: %s", strerror(errno));
    }

    // Bind socket
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);

    if (bind(server_socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        LOG_ERROR("Failed to bind to %s:%d: %s", 
               SERVER_HOST, SERVER_PORT, strerror(errno));
        close(server_socket_fd);
        server_socket_fd = -1;
        server_running = 0;
        return NULL;
    }

    // Listen for connections
    if (listen(server_socket_fd, MAX_CLIENTS) < 0) {
        LOG_ERROR("Failed to listen: %s", strerror(errno));
        close(server_socket_fd);
        server_socket_fd = -1;
        server_running = 0;
        return NULL;
    }

    LOG_INFO("Server listening on %s:%d", SERVER_HOST, SERVER_PORT);

    // Main server loop
    while (server_running) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);

        // Accept client connection
        int client_fd = accept(server_socket_fd, (struct sockaddr*)&client_addr, &client_addr_len);

        if (client_fd < 0) {
            if (server_running && errno != EINTR) {
                LOG_ERROR("Accept failed: %s", strerror(errno));
            }
            continue;
        }

        // Handle client connection
        dawn_handle_client_connection(client_fd, &client_addr);
    }

    // Cleanup
    if (server_socket_fd >= 0) {
        close(server_socket_fd);
        server_socket_fd = -1;
    }

    LOG_INFO("Server thread stopped");
    return NULL;
}

// === Public API Implementation ===

void dawn_server_set_audio_callback(dawn_audio_processor_callback_t callback) {
    pthread_mutex_lock(&callback_mutex);
    audio_processor_callback = callback;
    pthread_mutex_unlock(&callback_mutex);

    if (callback != NULL) {
        LOG_INFO("Audio processing callback registered");
    } else {
        LOG_INFO("Audio processing callback cleared");
    }
}

int dawn_server_start(void) {
    pthread_mutex_lock(&server_mutex);

    if (server_running) {
        LOG_WARNING("Server is already running");
        pthread_mutex_unlock(&server_mutex);
        return DAWN_SUCCESS;
    }

    server_running = 1;

    int result = pthread_create(&server_thread, NULL, dawn_server_thread, NULL);
    if (result != 0) {
        LOG_ERROR("Failed to create server thread (error code %d)", result);
        server_running = 0;
        pthread_mutex_unlock(&server_mutex);
        return DAWN_ERROR;
    }

    pthread_mutex_unlock(&server_mutex);

    // Give server time to initialize
    usleep(100000); // 100ms

    return DAWN_SUCCESS;
}

void dawn_server_stop(void) {
    pthread_mutex_lock(&server_mutex);

    if (!server_running) {
        pthread_mutex_unlock(&server_mutex);
        return;
    }

    LOG_INFO("Stopping server...\n");
    server_running = 0;

    // Close server socket to wake up accept() - under mutex protection
    int fd_to_close = server_socket_fd;
    server_socket_fd = -1;

    pthread_mutex_unlock(&server_mutex);

    // Close server socket to wake up accept()
    if (fd_to_close >= 0) {
        shutdown(fd_to_close, SHUT_RDWR);
        close(fd_to_close);
    }

    // Wait for server thread to complete
    pthread_join(server_thread, NULL);
    LOG_INFO("Server stopped");
}

int dawn_server_is_running(void) {
    pthread_mutex_lock(&server_mutex);
    int running = server_running;
    pthread_mutex_unlock(&server_mutex);
    return running;
}
