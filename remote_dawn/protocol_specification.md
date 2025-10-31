# Dawn Audio Protocol Specification
Version 1.1

## 1. Introduction

Dawn Audio Protocol (DAP) is a reliable binary protocol designed for transferring audio data between embedded devices and servers, with special optimizations for AI assistant communication. It provides packet-based transmission with error detection, retransmission capabilities, and session management.

### 1.1 Purpose

The protocol addresses common challenges in IoT audio transmission for AI assistants:
- Unreliable network connections
- Limited device resources
- Need for data integrity verification
- Simplified session management
- Long AI processing times
- Optimized performance over WiFi

### 1.2 Key Features

- Handshake-based session establishment
- Chunked data transfer with sequence numbering
- Checksums for data integrity verification
- Acknowledgment mechanism with retries
- Packet type identification
- Variable payload size
- Optimized for AI assistant response latency
- WAV format validation and repair

## 2. Protocol Architecture

DAP follows a client-server model where typically:
- The client is an embedded device (e.g., ESP32) with audio recording capabilities
- The server is a more powerful system that processes the received audio

### 2.1 Communication Flow

```
â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”                          â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”
â”‚        â”‚     1. Handshake         â”‚        â”‚
â”‚        â”‚ â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â–ºâ”‚        â”‚
â”‚        â”‚                          â”‚        â”‚
â”‚        â”‚     2. Handshake ACK     â”‚        â”‚
â”‚        â”‚ â—„â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”‚        â”‚
â”‚        â”‚                          â”‚        â”‚
â”‚        â”‚     3. Data Chunks       â”‚        â”‚
â”‚ Client â”‚ â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â–ºâ”‚ Server â”‚
â”‚        â”‚                          â”‚        â”‚
â”‚        â”‚     4. ACK for Chunks    â”‚        â”‚
â”‚        â”‚ â—„â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”‚        â”‚
â”‚        â”‚                          â”‚        â”‚
â”‚        â”‚     5. Response Data     â”‚        â”‚
â”‚        â”‚ â—„â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”‚        â”‚
â”‚        â”‚                          â”‚        â”‚
â”‚        â”‚     6. ACK for Response  â”‚        â”‚
â”‚        â”‚ â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â–ºâ”‚        â”‚
â””â”€â”€â”€â”€â”€â”€â”€â”€â”˜                          â””â”€â”€â”€â”€â”€â”€â”€â”€â”˜
```

## 3. Packet Structure

Every packet in DAP consists of a header followed by an optional payload.

### 3.1 Header Format (8 bytes)

```
â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
â”‚                      Packet Header                         â”‚
â”œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¤
â”‚ Data Length â”‚  Protocol   â”‚   Packet    â”‚    Checksum     â”‚
â”‚  (4 bytes)  â”‚  Version    â”‚    Type     â”‚    (2 bytes)    â”‚
â”‚             â”‚  (1 byte)   â”‚   (1 byte)  â”‚                 â”‚
â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”´â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”´â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”´â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
```

**Field Descriptions:**

1. **Data Length** (4 bytes, big-endian): Size of the payload in bytes
2. **Protocol Version** (1 byte): DAP version identifier (current: 0x01)
3. **Packet Type** (1 byte): Identifies the packet's purpose
4. **Checksum** (2 bytes, big-endian): Fletcher-16 checksum of the payload

### 3.2 Packet Types

| Type ID | Name | Description |
|---------|------|-------------|
| 0x01 | HANDSHAKE | Session initialization |
| 0x02 | DATA | Regular data chunk |
| 0x03 | DATA_END | Final data chunk |
| 0x04 | ACK | Positive acknowledgment |
| 0x05 | NACK | Negative acknowledgment |
| 0x06 | RETRY | Retry request |

### 3.3 Sequence Numbers

For DATA and DATA_END packets, the first 2 bytes of the payload contain a sequence number (16-bit, big-endian). This allows the receiver to detect missing or out-of-order packets.

**IMPORTANT**: Sequence numbers are sent separately from the actual data and must NOT be included when reassembling the final data stream. This is critical to preserve the integrity of WAV headers and other binary data.

```
â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
â”‚                           Data Packet                             â”‚
â”œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
â”‚  Header    â”‚                     Wire Format                     â”‚
â”‚  (8 bytes) â”‚                                                     â”‚
â”œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¼â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¤
â”‚            â”‚   Sequence      â”‚         Actual Data              â”‚
â”‚            â”‚   (2 bytes)     â”‚                                  â”‚
â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”´â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”´â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
```

When reassembling the data:
- Store sequence numbers separately
- Only append the actual data payload to the output buffer
- This ensures binary formats like WAV headers remain intact

## 4. Checksum Algorithm

DAP uses Fletcher-16 for checksums, which provides reasonable error detection with minimal computational overhead.

### 4.1 Fletcher-16 Implementation

```
function calculateChecksum(data, length):
    sum1 = 0
    sum2 = 0
    
    for i = 0 to length-1:
        sum1 = (sum1 + data[i]) % 255
        sum2 = (sum2 + sum1) % 255
    
    return (sum2 << 8) | sum1
```

## 5. Protocol Operations

### 5.1 Connection Establishment

Before data transfer, a handshake must be completed:

1. **Client initiates handshake**:
   - Sends HANDSHAKE packet with magic bytes: 0xA5, 0x5A, 0xB2, 0x2B
   - The payload length is 4 bytes
   - Calculates checksum over the magic bytes

2. **Server validates and responds**:
   - Verifies magic bytes and checksum
   - Sends ACK packet with empty payload (length = 0)

### 5.2 Data Transfer

After a successful handshake, the client can send audio data:

1. **Client prepares data**:
   - Divides large data into chunks (recommended: â‰¤1024 bytes)
   - Sends chunks sequentially with DATA packet type
   - For the final chunk, uses DATA_END packet type
   - Each chunk includes a sequence number starting from 0

2. **Server processes chunks**:
   - Validates checksum for each received chunk
   - Verifies sequence numbers are continuous
   - Sends ACK for valid chunks or NACK for invalid ones

3. **Client handles acknowledgments**:
   - If ACK received, sends next chunk
   - If NACK received or ACK times out, retransmits the chunk
   - Uses exponential backoff for retries

### 5.3 Server Response

After receiving the complete data, the server processes it and may send a response:

1. **Server sends response**:
   - Uses same chunking mechanism as client data transfer
   - Includes sequence numbers starting from 0

2. **Client acknowledges response**:
   - Validates checksums and sequence numbers
   - Sends ACK for valid chunks or NACK for invalid ones

## 6. Error Handling

### 6.1 Retry Mechanism

When a packet is lost or corrupted:

1. The receiver sends NACK or fails to send ACK
2. The sender retransmits after a timeout
3. Retries use exponential backoff: timeout = base_timeout * (2^retry_count)
4. After maximum retries (typically 5), the session is considered failed

### 6.2 Session Termination

A session can end in several ways:

1. **Normal completion**: After all data is successfully exchanged
2. **Timeout**: No response received within the expected timeframe
3. **Connection loss**: TCP connection is closed unexpectedly
4. **Maximum retries exceeded**: Too many failed transmission attempts

## 7. Implementation Guidelines

### 7.1 Client Implementation

1. **Connection**:
   - Establish TCP connection to server
   - Allow multiple connection attempts with backoff

2. **Handshake**:
   - Send handshake with magic bytes
   - Implement timeout for handshake response
   - Consider multiple handshake attempts

3. **Sending Data**:
   - Chunk data into manageable sizes
   - Include sequence numbers in order
   - Verify ACKs match sent sequence
   - Implement retry with exponential backoff

4. **Receiving Response**:
   - Process header to verify protocol version and type
   - Validate checksums and sequence numbers
   - Send ACK/NACK appropriately
   - Reassemble chunks into complete response

### 7.2 Server Implementation

1. **Connection Handling**:
   - Accept TCP connections
   - Consider connection limits and timeouts

2. **Handshake Processing**:
   - Validate handshake magic bytes and checksum
   - Send proper acknowledgment

3. **Receiving Data**:
   - Process headers and validate checksums
   - Track sequence numbers for continuity
   - Send ACK/NACK appropriately
   - Reassemble chunks into complete data

4. **Sending Response**:
   - Chunk response data appropriately
   - Include sequence numbers
   - Implement retry mechanism for failed ACKs

## 8. Example Packet Bytes

### 8.1 Handshake Packet (Client to Server)

```
00 00 00 04   01 01   D6 DD   A5 5A B2 2B
â”‚             â”‚  â”‚    â”‚       â”‚
â”‚             â”‚  â”‚    â”‚       â””â”€ Payload: Magic bytes
â”‚             â”‚  â”‚    â””â”€â”€â”€â”€â”€â”€â”€â”€â”€ Checksum (Fletcher-16)
â”‚             â”‚  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ Packet Type: HANDSHAKE (0x01)
â”‚             â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ Protocol Version: 0x01
â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ Data Length: 4 bytes
```

### 8.2 ACK Packet (Server to Client)

```
00 00 00 00   01 04   00 00
â”‚             â”‚  â”‚    â”‚
â”‚             â”‚  â”‚    â””â”€â”€â”€â”€â”€â”€â”€â”€â”€ Checksum (empty payload)
â”‚             â”‚  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ Packet Type: ACK (0x04)
â”‚             â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ Protocol Version: 0x01
â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ Data Length: 0 bytes
```

### 8.3 Data Packet (Client to Server)

```
00 00 04 00   01 02   XX XX   00 00   [1024 bytes of data]
â”‚             â”‚  â”‚    â”‚       â”‚       â”‚
â”‚             â”‚  â”‚    â”‚       â”‚       â””â”€ Actual payload data
â”‚             â”‚  â”‚    â”‚       â””â”€â”€â”€â”€â”€â”€â”€â”€â”€ Sequence number: 0
â”‚             â”‚  â”‚    â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ Checksum of payload
â”‚             â”‚  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ Packet Type: DATA (0x02)
â”‚             â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ Protocol Version: 0x01
â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ Data Length: 1024 bytes
```

## 9. Performance Considerations

### 9.1 Chunk Size

The protocol supports variable chunk sizes, with tradeoffs:

- **Small chunks** (1KB or less): More overhead, but better recovery from errors
- **Medium chunks** (4KB): Good balance for most applications
- **Large chunks** (8-16KB): Better throughput, but requires more memory
- **Recommended**: 4KB for reliable performance, 8KB for higher throughput over WiFi

**CRITICAL**: Both client and server MUST agree on the maximum allowed chunk size (`PACKET_MAX_SIZE`).

### 9.2 Timeout Values

Timeout values should be adjusted based on the application needs:

- **Handshake timeout**: 2-3 seconds recommended
- **Data ACK timeout**: 1-2 seconds initially, with exponential backoff
- **Connection attempt timeout**: 3-5 seconds
- **First packet response timeout**: For AI processing, 30 seconds or more is recommended

### 9.3 TCP Optimizations

For optimal performance over WiFi:

- **Disable Nagle's Algorithm**: Use `setNoDelay(true)` for both client and server
- **Prioritize real-time data**: This reduces latency at the expense of throughput
- **Use smaller packets for initial exchanges**: 1KB for handshake, larger for data transfer

### 9.4 Resource Constraints

- Embedded clients should use buffer sizes appropriate for their memory
- Consider stack usage when implementing recursive retry logic
- Preallocate buffers when possible to avoid heap fragmentation
- Use local buffers for incoming chunks to separate sequence numbers from data

## 10. Security Considerations

The basic DAP protocol doesn't include security features. For production environments, consider these additions:

- TLS for transport security
- Authentication mechanism during handshake
- Payload encryption
- Rate limiting and denial-of-service protection

## 11. Protocol Extensions

The protocol can be extended for specific needs:

### 11.1 Compression

Add a compression flag in an extended header to indicate payload compression.

### 11.2 Metadata

Include metadata about audio format in a reserved section of the first data packet.

### 11.3 Bidirectional Streaming

Extend the protocol to support simultaneous bidirectional streaming with separate sequence counters.

### 11.4 WAV Format Validation

The protocol includes built-in validation and repair for WAV audio data:

- Clients verify WAV headers before playback
- Servers ensure valid WAV headers before transmission
- Corrupted headers can be repaired during transmission
- Raw PCM fallback for completely corrupted WAV data

### 11.5 AI Assistant Extensions

For AI assistant applications, additional extensions include:

- Extended first-packet timeout to accommodate AI processing time
- Status message packets to provide feedback during long AI operations
- Speech recognition metadata in the response header
- Priority flags for urgent responses

## 12. Compatibility

- Protocol version field allows for future extensions
- Implementations should reject packets with unknown protocol versions
- Consider fallback mechanisms for backward compatibility
