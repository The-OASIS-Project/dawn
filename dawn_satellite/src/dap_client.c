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

#include "dap_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* Logging macros */
#define LOG_INFO(fmt, ...) fprintf(stdout, "[DAP] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[DAP ERROR] " fmt "\n", ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...)                                       \
   do {                                                           \
      if (getenv("DAP_DEBUG"))                                    \
         fprintf(stdout, "[DAP DEBUG] " fmt "\n", ##__VA_ARGS__); \
   } while (0)

/* Internal functions */
static int dap_set_socket_timeout(int fd, int timeout_sec);
static int dap_read_exact(int fd, uint8_t *buf, size_t n, int timeout_sec);
static int dap_write_exact(int fd, const uint8_t *buf, size_t n);
static void dap_build_header(uint8_t *header, uint32_t length, uint8_t type, uint16_t checksum);
static int dap_parse_header(const uint8_t *header,
                            uint32_t *length,
                            uint8_t *type,
                            uint16_t *checksum);
static int dap_send_ack(int fd);
static int dap_send_nack(int fd);
static int dap_perform_handshake(dap_client_t *client);
static int dap_send_chunked(dap_client_t *client, const uint8_t *data, size_t size);
static int dap_receive_chunked(dap_client_t *client, uint8_t **data, size_t *size);

uint16_t dap_calculate_checksum(const uint8_t *data, size_t length) {
   if (!data || length == 0)
      return 0;

   uint16_t sum1 = 0;
   uint16_t sum2 = 0;

   for (size_t i = 0; i < length; i++) {
      sum1 = (sum1 + data[i]) % 255;
      sum2 = (sum2 + sum1) % 255;
   }

   return (sum2 << 8) | sum1;
}

static void dap_build_header(uint8_t *header, uint32_t length, uint8_t type, uint16_t checksum) {
   /* 4 bytes: length (big-endian) */
   header[0] = (length >> 24) & 0xFF;
   header[1] = (length >> 16) & 0xFF;
   header[2] = (length >> 8) & 0xFF;
   header[3] = length & 0xFF;

   /* 1 byte: protocol version */
   header[4] = DAP_PROTOCOL_VERSION;

   /* 1 byte: packet type */
   header[5] = type;

   /* 2 bytes: checksum (big-endian) */
   header[6] = (checksum >> 8) & 0xFF;
   header[7] = checksum & 0xFF;
}

static int dap_parse_header(const uint8_t *header,
                            uint32_t *length,
                            uint8_t *type,
                            uint16_t *checksum) {
   /* Extract length (big-endian) */
   *length = ((uint32_t)header[0] << 24) | ((uint32_t)header[1] << 16) |
             ((uint32_t)header[2] << 8) | (uint32_t)header[3];

   /* Check protocol version */
   if (header[4] != DAP_PROTOCOL_VERSION) {
      LOG_ERROR("Protocol version mismatch: got 0x%02X, expected 0x%02X", header[4],
                DAP_PROTOCOL_VERSION);
      return DAP_ERROR_PROTOCOL;
   }

   *type = header[5];
   *checksum = ((uint16_t)header[6] << 8) | (uint16_t)header[7];

   return DAP_SUCCESS;
}

static int dap_set_socket_timeout(int fd, int timeout_sec) {
   struct timeval tv;
   tv.tv_sec = timeout_sec;
   tv.tv_usec = 0;

   if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
      LOG_ERROR("Failed to set receive timeout: %s", strerror(errno));
      return DAP_ERROR;
   }

   if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
      LOG_ERROR("Failed to set send timeout: %s", strerror(errno));
      return DAP_ERROR;
   }

   return DAP_SUCCESS;
}

static int dap_read_exact(int fd, uint8_t *buf, size_t n, int timeout_sec) {
   size_t total = 0;
   struct pollfd pfd = { .fd = fd, .events = POLLIN };
   int timeout_ms = timeout_sec * 1000;

   while (total < n) {
      int ret = poll(&pfd, 1, timeout_ms);
      if (ret < 0) {
         LOG_ERROR("Poll error: %s", strerror(errno));
         return DAP_ERROR;
      }
      if (ret == 0) {
         LOG_ERROR("Read timeout after %d seconds", timeout_sec);
         return DAP_ERROR_TIMEOUT;
      }

      ssize_t bytes = recv(fd, buf + total, n - total, 0);
      if (bytes <= 0) {
         if (bytes == 0) {
            LOG_ERROR("Connection closed by server");
         } else {
            LOG_ERROR("Recv error: %s", strerror(errno));
         }
         return DAP_ERROR;
      }
      total += bytes;
   }

   return DAP_SUCCESS;
}

static int dap_write_exact(int fd, const uint8_t *buf, size_t n) {
   size_t total = 0;

   while (total < n) {
      ssize_t bytes = send(fd, buf + total, n - total, 0);
      if (bytes <= 0) {
         if (errno == EPIPE) {
            LOG_ERROR("Connection closed (broken pipe)");
         } else {
            LOG_ERROR("Send error: %s", strerror(errno));
         }
         return DAP_ERROR;
      }
      total += bytes;
   }

   return DAP_SUCCESS;
}

static int dap_send_ack(int fd) {
   uint8_t header[DAP_PACKET_HEADER_SIZE];
   dap_build_header(header, 0, DAP_PACKET_ACK, 0);
   return dap_write_exact(fd, header, DAP_PACKET_HEADER_SIZE);
}

static int dap_send_nack(int fd) {
   uint8_t header[DAP_PACKET_HEADER_SIZE];
   dap_build_header(header, 0, DAP_PACKET_NACK, 0);
   return dap_write_exact(fd, header, DAP_PACKET_HEADER_SIZE);
}

int dap_client_init(dap_client_t *client, const char *server_ip, uint16_t port) {
   if (!client || !server_ip) {
      return DAP_ERROR;
   }

   memset(client, 0, sizeof(dap_client_t));
   strncpy(client->server_ip, server_ip, sizeof(client->server_ip) - 1);
   client->server_port = (port > 0) ? port : DAP_DEFAULT_PORT;
   client->socket_fd = -1;
   client->connected = 0;

   LOG_INFO("Client initialized for %s:%u", client->server_ip, client->server_port);
   return DAP_SUCCESS;
}

void dap_client_cleanup(dap_client_t *client) {
   if (client) {
      dap_client_disconnect(client);
      memset(client, 0, sizeof(dap_client_t));
      client->socket_fd = -1;
   }
}

static int dap_perform_handshake(dap_client_t *client) {
   uint8_t magic[4] = { DAP_MAGIC_0, DAP_MAGIC_1, DAP_MAGIC_2, DAP_MAGIC_3 };
   uint8_t header[DAP_PACKET_HEADER_SIZE];
   uint16_t checksum = dap_calculate_checksum(magic, sizeof(magic));

   /* Build and send handshake packet */
   dap_build_header(header, sizeof(magic), DAP_PACKET_HANDSHAKE, checksum);

   if (dap_write_exact(client->socket_fd, header, DAP_PACKET_HEADER_SIZE) != DAP_SUCCESS) {
      LOG_ERROR("Failed to send handshake header");
      return DAP_ERROR_HANDSHAKE;
   }

   if (dap_write_exact(client->socket_fd, magic, sizeof(magic)) != DAP_SUCCESS) {
      LOG_ERROR("Failed to send handshake magic bytes");
      return DAP_ERROR_HANDSHAKE;
   }

   LOG_DEBUG("Handshake sent, waiting for ACK...");

   /* Wait for ACK with timeout */
   uint8_t resp_header[DAP_PACKET_HEADER_SIZE];
   if (dap_read_exact(client->socket_fd, resp_header, DAP_PACKET_HEADER_SIZE, 5) != DAP_SUCCESS) {
      LOG_ERROR("Failed to receive handshake response");
      return DAP_ERROR_HANDSHAKE;
   }

   uint32_t resp_len;
   uint8_t resp_type;
   uint16_t resp_checksum;

   if (dap_parse_header(resp_header, &resp_len, &resp_type, &resp_checksum) != DAP_SUCCESS) {
      LOG_ERROR("Invalid handshake response header");
      return DAP_ERROR_HANDSHAKE;
   }

   if (resp_type != DAP_PACKET_ACK) {
      LOG_ERROR("Handshake not acknowledged (got type 0x%02X)", resp_type);
      return DAP_ERROR_HANDSHAKE;
   }

   LOG_INFO("Handshake successful");
   client->send_sequence = 0;
   client->receive_sequence = 0;

   return DAP_SUCCESS;
}

int dap_client_connect(dap_client_t *client) {
   if (!client)
      return DAP_ERROR;

   if (client->connected) {
      LOG_DEBUG("Already connected");
      return DAP_SUCCESS;
   }

   struct sockaddr_in server_addr;
   memset(&server_addr, 0, sizeof(server_addr));
   server_addr.sin_family = AF_INET;
   server_addr.sin_port = htons(client->server_port);

   if (inet_pton(AF_INET, client->server_ip, &server_addr.sin_addr) <= 0) {
      LOG_ERROR("Invalid server IP: %s", client->server_ip);
      return DAP_ERROR_CONNECT;
   }

   for (int attempt = 1; attempt <= DAP_MAX_CONNECT_ATTEMPTS; attempt++) {
      LOG_INFO("Connection attempt %d/%d to %s:%u", attempt, DAP_MAX_CONNECT_ATTEMPTS,
               client->server_ip, client->server_port);

      /* Create socket */
      client->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
      if (client->socket_fd < 0) {
         LOG_ERROR("Socket creation failed: %s", strerror(errno));
         continue;
      }

      /* Set TCP_NODELAY for low latency */
      int flag = 1;
      setsockopt(client->socket_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

      /* Set socket timeout */
      dap_set_socket_timeout(client->socket_fd, DAP_SOCKET_TIMEOUT_SEC);

      /* Connect with timeout using non-blocking connect */
      int flags = fcntl(client->socket_fd, F_GETFL, 0);
      fcntl(client->socket_fd, F_SETFL, flags | O_NONBLOCK);

      int ret = connect(client->socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
      if (ret < 0 && errno != EINPROGRESS) {
         LOG_ERROR("Connect failed: %s", strerror(errno));
         close(client->socket_fd);
         client->socket_fd = -1;
         usleep(500000); /* 500ms delay before retry */
         continue;
      }

      /* Wait for connection with timeout */
      struct pollfd pfd = { .fd = client->socket_fd, .events = POLLOUT };
      ret = poll(&pfd, 1, DAP_CONNECT_TIMEOUT_MS);

      if (ret <= 0) {
         LOG_ERROR("Connection timeout");
         close(client->socket_fd);
         client->socket_fd = -1;
         continue;
      }

      /* Check if connection succeeded */
      int error = 0;
      socklen_t len = sizeof(error);
      getsockopt(client->socket_fd, SOL_SOCKET, SO_ERROR, &error, &len);
      if (error != 0) {
         LOG_ERROR("Connection error: %s", strerror(error));
         close(client->socket_fd);
         client->socket_fd = -1;
         continue;
      }

      /* Restore blocking mode */
      fcntl(client->socket_fd, F_SETFL, flags);

      LOG_INFO("Connected to server");

      /* Perform handshake */
      if (dap_perform_handshake(client) == DAP_SUCCESS) {
         client->connected = 1;
         return DAP_SUCCESS;
      }

      /* Handshake failed, close and retry */
      close(client->socket_fd);
      client->socket_fd = -1;
      usleep(500000);
   }

   LOG_ERROR("All connection attempts failed");
   return DAP_ERROR_CONNECT;
}

void dap_client_disconnect(dap_client_t *client) {
   if (client && client->socket_fd >= 0) {
      close(client->socket_fd);
      client->socket_fd = -1;
      client->connected = 0;
      LOG_INFO("Disconnected from server");
   }
}

int dap_client_is_connected(dap_client_t *client) {
   return client && client->connected;
}

static int dap_send_chunked(dap_client_t *client, const uint8_t *data, size_t size) {
   size_t total_sent = 0;

   LOG_INFO("Sending %zu bytes in chunks", size);

   while (total_sent < size) {
      size_t remaining = size - total_sent;
      size_t chunk_size = (remaining > DAP_PACKET_MAX_SIZE) ? DAP_PACKET_MAX_SIZE : remaining;
      int is_last = (total_sent + chunk_size >= size);

      uint8_t packet_type = is_last ? DAP_PACKET_DATA_END : DAP_PACKET_DATA;
      const uint8_t *chunk = data + total_sent;
      uint16_t checksum = dap_calculate_checksum(chunk, chunk_size);

      /* Build header */
      uint8_t header[DAP_PACKET_HEADER_SIZE];
      dap_build_header(header, chunk_size, packet_type, checksum);

      /* Sequence bytes */
      uint8_t seq[2];
      seq[0] = (client->send_sequence >> 8) & 0xFF;
      seq[1] = client->send_sequence & 0xFF;

      /* Retry loop */
      int sent = 0;
      for (int retry = 0; retry < DAP_MAX_RETRIES && !sent; retry++) {
         if (retry > 0) {
            int delay_ms = 100 * (1 << retry);
            if (delay_ms > 2000)
               delay_ms = 2000;
            LOG_DEBUG("Retry %d after %d ms", retry, delay_ms);
            usleep(delay_ms * 1000);
         }

         /* Send header + sequence + data */
         if (dap_write_exact(client->socket_fd, header, DAP_PACKET_HEADER_SIZE) != DAP_SUCCESS)
            continue;
         if (dap_write_exact(client->socket_fd, seq, 2) != DAP_SUCCESS)
            continue;
         if (dap_write_exact(client->socket_fd, chunk, chunk_size) != DAP_SUCCESS)
            continue;

         /* Wait for ACK */
         uint8_t ack_header[DAP_PACKET_HEADER_SIZE];
         if (dap_read_exact(client->socket_fd, ack_header, DAP_PACKET_HEADER_SIZE, 2) !=
             DAP_SUCCESS) {
            LOG_DEBUG("ACK timeout, retrying...");
            continue;
         }

         uint32_t ack_len;
         uint8_t ack_type;
         uint16_t ack_checksum;
         if (dap_parse_header(ack_header, &ack_len, &ack_type, &ack_checksum) != DAP_SUCCESS)
            continue;

         if (ack_type == DAP_PACKET_ACK) {
            sent = 1;
         } else if (ack_type == DAP_PACKET_NACK) {
            LOG_DEBUG("Received NACK, retrying...");
         }
      }

      if (!sent) {
         LOG_ERROR("Failed to send chunk after %d retries", DAP_MAX_RETRIES);
         return DAP_ERROR_SEND;
      }

      total_sent += chunk_size;
      client->send_sequence++;

      /* Progress logging for large transfers */
      if (size > 50000) {
         int percent = (int)((total_sent * 100) / size);
         LOG_INFO("Sent %zu/%zu bytes (%d%%)", total_sent, size, percent);
      }
   }

   LOG_INFO("Send complete: %zu bytes", total_sent);
   return DAP_SUCCESS;
}

static int dap_receive_chunked(dap_client_t *client, uint8_t **data, size_t *size) {
   /* Initial buffer allocation */
   size_t buf_size = 1024 * 1024; /* 1MB initial buffer */
   uint8_t *buffer = malloc(buf_size);
   if (!buffer) {
      LOG_ERROR("Failed to allocate receive buffer");
      return DAP_ERROR_MEMORY;
   }

   size_t total_received = 0;

   LOG_INFO("Waiting for response (up to %d seconds)...", DAP_AI_RESPONSE_TIMEOUT_SEC);

   while (1) {
      /* Read header with longer timeout for first packet (AI processing) */
      int timeout = (total_received == 0) ? DAP_AI_RESPONSE_TIMEOUT_SEC : 5;

      uint8_t header[DAP_PACKET_HEADER_SIZE];
      int ret = dap_read_exact(client->socket_fd, header, DAP_PACKET_HEADER_SIZE, timeout);
      if (ret != DAP_SUCCESS) {
         free(buffer);
         return ret;
      }

      uint32_t data_len;
      uint8_t packet_type;
      uint16_t expected_checksum;

      if (dap_parse_header(header, &data_len, &packet_type, &expected_checksum) != DAP_SUCCESS) {
         dap_send_nack(client->socket_fd);
         free(buffer);
         return DAP_ERROR_PROTOCOL;
      }

      if (data_len > DAP_PACKET_MAX_SIZE) {
         LOG_ERROR("Packet too large: %u bytes", data_len);
         dap_send_nack(client->socket_fd);
         free(buffer);
         return DAP_ERROR_PROTOCOL;
      }

      /* Read sequence number */
      uint8_t seq[2];
      if (dap_read_exact(client->socket_fd, seq, 2, 2) != DAP_SUCCESS) {
         free(buffer);
         return DAP_ERROR_RECEIVE;
      }

      uint16_t packet_seq = ((uint16_t)seq[0] << 8) | seq[1];
      if (packet_seq != client->receive_sequence) {
         LOG_ERROR("Sequence mismatch: expected %u, got %u", client->receive_sequence, packet_seq);
         dap_send_nack(client->socket_fd);
         continue;
      }

      /* Expand buffer if needed */
      if (total_received + data_len > buf_size) {
         buf_size *= 2;
         uint8_t *new_buf = realloc(buffer, buf_size);
         if (!new_buf) {
            LOG_ERROR("Failed to expand receive buffer");
            free(buffer);
            return DAP_ERROR_MEMORY;
         }
         buffer = new_buf;
      }

      /* Read chunk data */
      if (dap_read_exact(client->socket_fd, buffer + total_received, data_len, 5) != DAP_SUCCESS) {
         free(buffer);
         return DAP_ERROR_RECEIVE;
      }

      /* Verify checksum */
      uint16_t actual_checksum = dap_calculate_checksum(buffer + total_received, data_len);
      if (actual_checksum != expected_checksum) {
         LOG_ERROR("Checksum mismatch: expected 0x%04X, got 0x%04X", expected_checksum,
                   actual_checksum);
         dap_send_nack(client->socket_fd);
         continue;
      }

      /* Send ACK */
      dap_send_ack(client->socket_fd);

      total_received += data_len;
      client->receive_sequence++;

      LOG_DEBUG("Received chunk: %u bytes (total: %zu)", data_len, total_received);

      /* Check for end of data */
      if (packet_type == DAP_PACKET_DATA_END) {
         break;
      }
   }

   LOG_INFO("Receive complete: %zu bytes", total_received);

   *data = buffer;
   *size = total_received;
   return DAP_SUCCESS;
}

int dap_client_transact(dap_client_t *client,
                        const uint8_t *audio_data,
                        size_t audio_size,
                        uint8_t **response_data,
                        size_t *response_size) {
   if (!client || !audio_data || !response_data || !response_size) {
      return DAP_ERROR;
   }

   if (!client->connected) {
      LOG_ERROR("Not connected to server");
      return DAP_ERROR_CONNECT;
   }

   /* Reset response pointers */
   *response_data = NULL;
   *response_size = 0;

   /* Send audio data */
   int ret = dap_send_chunked(client, audio_data, audio_size);
   if (ret != DAP_SUCCESS) {
      LOG_ERROR("Failed to send audio data");
      return ret;
   }

   /* Receive response */
   ret = dap_receive_chunked(client, response_data, response_size);
   if (ret != DAP_SUCCESS) {
      LOG_ERROR("Failed to receive response");
      return ret;
   }

   return DAP_SUCCESS;
}
