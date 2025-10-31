# remote_dawn - Network Audio Testing & Verification

This directory contains standalone test implementations and verification tools for the DAWN Audio Protocol and ESP32 client integration. These components are used to validate the network audio system before integration into the main DAWN codebase.

## Contents

### Server Implementations

**remote_dawn_server/** - Standalone voice assistant server for protocol verification
- Complete Audio→Text→LLM→TTS pipeline
- Full Vosk speech recognition integration
- Piper neural TTS synthesis
- Network audio server with chunked transfer protocol
- Build with `make` - see Makefile for dependencies

**echo_test_server.py** - Python echo server for basic protocol testing
- Echoes received audio back to client
- Useful for validating ESP32 client implementation
- Minimal dependencies (Python 3 + socket)

**echo_test_server_c/** - C implementation of echo server
- Lightweight protocol validation
- Build with `make test`

### Client Implementation

**remote_dawn.ino** - ESP32 client (Arduino/ESP-IDF)
- Audio capture via I2S microphone
- Network transmission using DAWN Audio Protocol
- TFT display feedback (ESP32-S3 TFT Feather)
- Audio playback via I2S amplifier

### Documentation

**protocol_specification.md** - Complete protocol documentation
- Packet format and header structure
- Handshaking and acknowledgment flow
- Error handling and retry logic

**protocol_purpose.md** - Design rationale
- Why the protocol was designed this way
- AI assistant communication requirements
- Embedded device constraints

**dawn_multi_client_architecture.md** - Future multi-client design
- Planned architecture for concurrent ESP32 clients
- Session management and threading model
- Migration strategy from current single-client model

## Use Cases

### Protocol Verification
Use the echo servers to validate that ESP32 clients correctly implement the DAWN Audio Protocol without requiring the full AI pipeline.

### Integration Testing
Use `remote_dawn_server/` to verify end-to-end voice assistant functionality with real ESP32 hardware before merging network audio support into mainline DAWN.

### Client Development
The Arduino sketch serves as reference implementation for ESP32-based DAWN clients and can be adapted for other embedded platforms.

## Testing Workflow

1. **Protocol Validation**: Test ESP32 client against `echo_test_server.py`
2. **Audio Pipeline**: Test against `remote_dawn_server/voice_assistant_server`
3. **Integration**: Merge verified components into mainline DAWN

## Dependencies

### Server
- Vosk speech recognition models
- Piper TTS models and libraries
- llama.cpp or compatible LLM server
- CUDA libraries (optional, for GPU acceleration)

### Client
- Arduino framework or ESP-IDF
- ESP32-S3 with I2S support
- Optional: TFT display libraries

## Building

```bash
# Standalone server
cd remote_dawn_server
make
./voice_assistant_server

# Echo server (C)
cd echo_test_server_c  
make test

# ESP32 client
# Open remote_dawn.ino in Arduino IDE and upload to ESP32-S3
```

## Status

This code has been successfully tested and the network audio components are being merged into the main DAWN repository. Once integration is complete, these test implementations will remain as reference and verification tools.
