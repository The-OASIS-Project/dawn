#ifndef DAWN_SERVER_H
#define DAWN_SERVER_H

#include <stdint.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

// === Audio Protocol Configuration ===
#define PROTOCOL_VERSION       0x01
#define PACKET_HEADER_SIZE     8
#define PACKET_MAX_SIZE        8192    // 8KB chunks for optimal WiFi performance
#define PACKET_TYPE_HANDSHAKE  0x01
#define PACKET_TYPE_DATA       0x02
#define PACKET_TYPE_DATA_END   0x03
#define PACKET_TYPE_ACK        0x04
#define PACKET_TYPE_NACK       0x05
#define PACKET_TYPE_RETRY      0x06

// Server Configuration
#define SERVER_HOST            "0.0.0.0"
#define SERVER_PORT            5000
#define MAX_CLIENTS            5
#define SOCKET_TIMEOUT_SEC     30
#define MAX_DATA_SIZE          (10 * 1024 * 1024)  // 10MB max
#define MAX_RETRIES            5

// Magic bytes for handshake
#define MAGIC_BYTE_0           0xA5
#define MAGIC_BYTE_1           0x5A
#define MAGIC_BYTE_2           0xB2
#define MAGIC_BYTE_3           0x2B

// Return codes
#define DAWN_SUCCESS           0
#define DAWN_ERROR            -1
#define DAWN_ERROR_MEMORY     -2
#define DAWN_ERROR_SOCKET     -3
#define DAWN_ERROR_PROTOCOL   -4
#define DAWN_ERROR_TIMEOUT    -5

// === Packet Header Structure ===
typedef struct {
    uint32_t data_length;    // Payload size in bytes (big-endian)
    uint8_t  protocol_version;
    uint8_t  packet_type;
    uint16_t checksum;       // Fletcher-16 checksum (big-endian)
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
typedef uint8_t* (*dawn_audio_processor_callback_t)(const uint8_t *audio_data, 
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
void dawn_build_packet_header(uint8_t *header, uint32_t data_length, 
                             uint8_t packet_type, uint16_t checksum);

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

#endif // DAWN_SERVER_H
