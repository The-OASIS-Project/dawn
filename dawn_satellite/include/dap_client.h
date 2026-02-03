/*
 * DAWN Audio Protocol Client - Linux Implementation
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
 */

#ifndef DAP_CLIENT_H
#define DAP_CLIENT_H

#include <stddef.h>
#include <stdint.h>

/* Protocol configuration - must match DAWN server */
#define DAP_PROTOCOL_VERSION 0x01
#define DAP_PACKET_HEADER_SIZE 8
#define DAP_PACKET_MAX_SIZE 8192 /* 8KB chunks for optimal WiFi performance */

/* Packet types */
#define DAP_PACKET_HANDSHAKE 0x01
#define DAP_PACKET_DATA 0x02
#define DAP_PACKET_DATA_END 0x03
#define DAP_PACKET_ACK 0x04
#define DAP_PACKET_NACK 0x05
#define DAP_PACKET_RETRY 0x06

/* Magic bytes for handshake */
#define DAP_MAGIC_0 0xA5
#define DAP_MAGIC_1 0x5A
#define DAP_MAGIC_2 0xB2
#define DAP_MAGIC_3 0x2B

/* Connection settings */
#define DAP_DEFAULT_PORT 5000
#define DAP_CONNECT_TIMEOUT_MS 3000
#define DAP_SOCKET_TIMEOUT_SEC 30
#define DAP_MAX_RETRIES 5
#define DAP_MAX_CONNECT_ATTEMPTS 3

/* Response timeout for AI processing */
#define DAP_AI_RESPONSE_TIMEOUT_SEC 30

/* Return codes */
#define DAP_SUCCESS 0
#define DAP_ERROR -1
#define DAP_ERROR_CONNECT -2
#define DAP_ERROR_HANDSHAKE -3
#define DAP_ERROR_SEND -4
#define DAP_ERROR_RECEIVE -5
#define DAP_ERROR_TIMEOUT -6
#define DAP_ERROR_PROTOCOL -7
#define DAP_ERROR_CHECKSUM -8
#define DAP_ERROR_MEMORY -9

/**
 * DAP client context
 */
typedef struct {
   int socket_fd;             /* Socket file descriptor */
   char server_ip[64];        /* Server IP address */
   uint16_t server_port;      /* Server port */
   uint16_t send_sequence;    /* Send sequence counter */
   uint16_t receive_sequence; /* Receive sequence counter */
   int connected;             /* Connection state */
} dap_client_t;

/**
 * Initialize DAP client context
 *
 * @param client Pointer to client context
 * @param server_ip Server IP address string
 * @param port Server port (use 0 for default)
 * @return DAP_SUCCESS on success, error code otherwise
 */
int dap_client_init(dap_client_t *client, const char *server_ip, uint16_t port);

/**
 * Clean up DAP client context
 *
 * @param client Pointer to client context
 */
void dap_client_cleanup(dap_client_t *client);

/**
 * Connect to DAWN server and perform handshake
 *
 * @param client Pointer to client context
 * @return DAP_SUCCESS on success, error code otherwise
 */
int dap_client_connect(dap_client_t *client);

/**
 * Disconnect from server
 *
 * @param client Pointer to client context
 */
void dap_client_disconnect(dap_client_t *client);

/**
 * Send audio data to server and receive response
 *
 * This is the main transaction function. It sends audio data (WAV format),
 * waits for AI processing, and receives the TTS response.
 *
 * @param client Pointer to client context
 * @param audio_data Input audio data (WAV format)
 * @param audio_size Size of input audio in bytes
 * @param response_data Pointer to receive response buffer (caller must free)
 * @param response_size Pointer to receive response size
 * @return DAP_SUCCESS on success, error code otherwise
 */
int dap_client_transact(dap_client_t *client,
                        const uint8_t *audio_data,
                        size_t audio_size,
                        uint8_t **response_data,
                        size_t *response_size);

/**
 * Check if client is connected
 *
 * @param client Pointer to client context
 * @return 1 if connected, 0 otherwise
 */
int dap_client_is_connected(dap_client_t *client);

/**
 * Calculate Fletcher-16 checksum
 *
 * @param data Data buffer
 * @param length Data length
 * @return 16-bit checksum
 */
uint16_t dap_calculate_checksum(const uint8_t *data, size_t length);

#endif /* DAP_CLIENT_H */
