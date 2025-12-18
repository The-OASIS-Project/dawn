/**
 * @file dawn_server.h
 * @brief DAWN Audio Protocol Server - Network protocol implementation
 *
 * This module implements the DAWN (Digital Assistant for Wearable Neutronics)
 * Audio Protocol server, which receives audio data from ESP32 clients over
 * TCP/IP, processes it through speech recognition and AI processing, and
 * returns synthesized audio responses.
 *
 * ARCHITECTURE:
 * ------------
 * - Single-threaded server running in a pthread
 * - Handles one client connection at a time (blocking)
 * - Integrates with main DAWN system via callback function
 * - Uses Fletcher-16 checksums for data integrity
 * - Implements retry logic with exponential backoff
 *
 * PROTOCOL FLOW:
 * --------------
 * 1. Client connects via TCP
 * 2. Handshake exchange (magic bytes verification)
 * 3. Client sends audio data in chunks (with sequence numbers)
 * 4. Server processes audio and generates response
 * 5. Server sends response audio in chunks
 * 6. Connection closes
 *
 * THREADING MODEL:
 * ----------------
 * - Server runs in its own pthread created by dawn_server_start()
 * - Client connections are handled sequentially (not concurrent)
 * - Callback to main DAWN system may block for 10-15 seconds during LLM processing
 *
 * MEMORY OWNERSHIP:
 * -----------------
 * - Server allocates buffers for received data
 * - Callback receives ownership of received data
 * - Callback returns allocated response data
 * - Server frees response data after transmission
 *
 * USAGE EXAMPLE:
 * --------------
 * @code
 * // 1. Register callback
 * dawn_server_set_audio_callback(my_audio_processor);
 *
 * // 2. Start server
 * if (dawn_server_start() != DAWN_SUCCESS) {
 *    fprintf(stderr, "Failed to start server\n");
 *    return -1;
 * }
 *
 * // 3. Server runs in background
 * while (running) {
 *    // Do other work
 *    sleep(1);
 * }
 *
 * // 4. Stop server
 * dawn_server_stop();
 * @endcode
 *
 * FUTURE ENHANCEMENTS:
 * --------------------
 * - Multi-client support via worker threads (see dawn_multi_client_architecture.md)
 * - Per-client session management with conversation history
 * - Non-blocking operation to avoid blocking main thread
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

#ifndef DAWN_SERVER_H
#define DAWN_SERVER_H

#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/socket.h>

// === Audio Protocol Configuration ===
#define PROTOCOL_VERSION 0x01
#define PACKET_HEADER_SIZE 8
#define PACKET_MAX_SIZE 8192  // 8KB chunks for optimal WiFi performance
#define PACKET_TYPE_HANDSHAKE 0x01
#define PACKET_TYPE_DATA 0x02
#define PACKET_TYPE_DATA_END 0x03
#define PACKET_TYPE_ACK 0x04
#define PACKET_TYPE_NACK 0x05
#define PACKET_TYPE_RETRY 0x06

// Server Configuration
#define SERVER_HOST "0.0.0.0"
#define SERVER_PORT 5000
#define MAX_CLIENTS 5
// Note: socket timeout is now configured via [network] socket_timeout_sec in config file
#define MAX_DATA_SIZE (10 * 1024 * 1024)  // 10MB max
#define MAX_RETRIES 5

// Magic bytes for handshake
#define MAGIC_BYTE_0 0xA5
#define MAGIC_BYTE_1 0x5A
#define MAGIC_BYTE_2 0xB2
#define MAGIC_BYTE_3 0x2B

// Return codes (positive per project coding standards)
#define DAWN_SUCCESS 0
#define DAWN_ERROR 1
#define DAWN_ERROR_MEMORY 2
#define DAWN_ERROR_SOCKET 3
#define DAWN_ERROR_PROTOCOL 4
#define DAWN_ERROR_TIMEOUT 5

// === Packet Header Structure ===
typedef struct {
   uint32_t data_length;  // Payload size in bytes (big-endian)
   uint8_t protocol_version;
   uint8_t packet_type;
   uint16_t checksum;  // Fletcher-16 checksum (big-endian)
} __attribute__((packed)) dawn_packet_header_t;

// === Client Session Structure ===
typedef struct {
   char client_ip[64];
   uint16_t send_sequence;
   uint16_t receive_sequence;
   int socket_fd;
   struct sockaddr_in addr;
} dawn_client_session_t;

// === Audio Processing Callback ===
/**
 * Callback function type for processing received network audio
 * @param audio_data Pointer to received WAV audio data
 * @param audio_size Size of received audio data in bytes
 * @param client_info String identifying the client (for logging)
 * @param response_size Pointer to store response data size
 * @return Pointer to response audio data, or NULL for error
 */
typedef uint8_t *(*dawn_audio_processor_callback_t)(const uint8_t *audio_data,
                                                    size_t audio_size,
                                                    const char *client_info,
                                                    size_t *response_size);

/**
 * Set the audio processing callback function
 * @param callback Function to call when audio is received from clients
 */
void dawn_server_set_audio_callback(dawn_audio_processor_callback_t callback);

// === Public API ===
/**
 * Start the audio server in a separate thread
 * @return DAWN_SUCCESS on success, error code on failure
 */
int dawn_server_start(void);

/**
 * Stop the audio server and cleanup resources
 */
void dawn_server_stop(void);

/**
 * Check if the server is currently running
 * @return 1 if running, 0 if not running
 */
int dawn_server_is_running(void);

// === Protocol Functions ===
/**
 * Calculate Fletcher-16 checksum for data verification
 * @param data Pointer to data buffer
 * @param length Number of bytes to checksum
 * @return 16-bit Fletcher-16 checksum
 */
uint16_t dawn_calculate_checksum(const uint8_t *data, size_t length);

/**
 * Build a packet header with proper endianness
 * @param header Output buffer for 8-byte header
 * @param data_length Payload size in bytes
 * @param packet_type Type of packet (PACKET_TYPE_*)
 * @param checksum Fletcher-16 checksum of payload
 */
void dawn_build_packet_header(uint8_t *header,
                              uint32_t data_length,
                              uint8_t packet_type,
                              uint16_t checksum);

/**
 * Parse a packet header and validate protocol version
 * @param header Input buffer containing 8-byte header
 * @param parsed Output structure with parsed values
 * @return DAWN_SUCCESS on success, error code on failure
 */
int dawn_parse_packet_header(const uint8_t *header, dawn_packet_header_t *parsed);

/**
 * Read exactly n bytes from a socket
 * @param socket_fd Socket file descriptor
 * @param buffer Output buffer
 * @param n Number of bytes to read
 * @return DAWN_SUCCESS on success, error code on failure
 */
int dawn_read_exact(int socket_fd, uint8_t *buffer, size_t n);

/**
 * Send exactly n bytes to a socket
 * @param socket_fd Socket file descriptor
 * @param buffer Data buffer to send
 * @param n Number of bytes to send
 * @return DAWN_SUCCESS on success, error code on failure
 */
int dawn_send_exact(int socket_fd, const uint8_t *buffer, size_t n);

// =============================================================================
// DAP Protocol Functions (for worker thread use)
// =============================================================================

/**
 * Handle DAP handshake with client
 * @param session Client session with initialized socket
 * @return DAWN_SUCCESS on success, error code on failure
 */
int dawn_handle_handshake(dawn_client_session_t *session);

/**
 * Receive data chunks from client
 * @param session Client session with initialized socket
 * @param data_out Pointer to receive allocated data buffer (caller must free)
 * @param size_out Pointer to receive data size
 * @return DAWN_SUCCESS on success, error code on failure
 */
int dawn_receive_data_chunks(dawn_client_session_t *session, uint8_t **data_out, size_t *size_out);

/**
 * Send data to client in chunks with retry logic
 * @param session Client session with initialized socket
 * @param data Data to send
 * @param size Size of data in bytes
 * @return DAWN_SUCCESS on success, error code on failure
 */
int dawn_send_data_chunks(dawn_client_session_t *session, const uint8_t *data, size_t size);

/**
 * Send ACK packet to client
 * @param socket_fd Client socket
 * @return DAWN_SUCCESS on success, error code on failure
 */
int dawn_send_ack(int socket_fd);

/**
 * Send NACK packet to client
 * @param socket_fd Client socket
 * @return DAWN_SUCCESS on success, error code on failure
 */
int dawn_send_nack(int socket_fd);

#endif  // DAWN_SERVER_H
