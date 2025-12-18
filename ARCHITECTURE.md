# D.A.W.N. System Architecture

This document describes the architecture of the D.A.W.N. (Digital Assistant for Workflow Neural-inference) voice assistant system.

**D.A.W.N.** is the central intelligence layer of the OASIS ecosystem, responsible for interpreting user intent, fusing data from every subsystem, and routing commands. At its core, DAWN performs neural-inference to understand context and drive decision-making, acting as OASIS's orchestration hub for MIRAGE, AURA, SPARK, STAT, and any future modules.

**Last Updated**: November 25, 2025 (after LLM interrupt implementation)

## Table of Contents

- [High-Level Overview](#high-level-overview)
- [Subsystem Architecture](#subsystem-architecture)
- [Data Flow](#data-flow)
- [Threading Model](#threading-model)
- [State Machine](#state-machine)
- [Network Protocol](#network-protocol)
- [Component Interactions](#component-interactions)
- [Memory Management](#memory-management)
- [Error Handling](#error-handling)

---

## High-Level Overview

D.A.W.N. is a modular voice assistant system that processes voice commands through a pipeline of specialized subsystems:

```
┌─────────────────────────────────────────────────────────────┐
│                      DAWN Main Loop                         │
│  (src/dawn.c - State Machine: SILENCE → WAKEWORD → COMMAND  │
│   → PROCESSING)                                             │
└────────┬──────────────────────────────┬─────────────────────┘
         │                              │
         │ Local Audio                  │ Network Audio
         │                              │
    ┌────▼─────────┐            ┌───────▼──────────┐
    │ Audio Capture│            │  Network Server  │
    │ Thread       │            │  (dawn_server)   │
    │ + Ring Buffer│            │  Single-threaded │
    └────┬─────────┘            └───────┬──────────┘
         │                              │
    ┌────▼──────────┐           ┌───────▼──────────┐
    │  VAD (Silero) │           │ Network Audio    │
    │  Speech/Noise │           │ Processing       │
    └────┬──────────┘           └───────┬──────────┘
         │                              │
    ┌────▼──────────┐                   │
    │ ASR Interface │                   │
    │ (abstraction) │                   │
    └────┬──────────┘                   │
         │                              │
    ┌────▼──────────┐                   │
    │ Vosk | Whisper│                   │
    │ (+ GPU accel) │                   │
    └────┬──────────┘                   │
         │                              │
         └──────────┬───────────────────┘
                    │
            ┌───────▼──────────┐
            │  LLM Interface   │
            │  (abstraction)   │
            └───────┬──────────┘
                    │
         ┌──────────┼──────────┐
         │          │          │
    ┌────▼───┐ ┌───▼────┐ ┌───▼──────┐
    │ OpenAI │ │ Claude │ │ llama.cpp│
    │ GPT-4o │ │ 4.5    │ │ (local)  │
    └────┬───┘ └───┬────┘ └───┬──────┘
         │         │          │
         └─────────┼──────────┘
                   │ (streaming)
           ┌───────▼──────────┐
           │  SSE Parser +    │
           │ Sentence Buffer  │
           └───────┬──────────┘
                   │
           ┌───────▼──────────┐
           │   TTS (Piper)    │
           │  + Preprocessing │
           └───────┬──────────┘
                   │
           ┌───────▼──────────┐
           │ ALSA/PulseAudio  │
           │   Playback       │
           └──────────────────┘
```

### Core Design Principles

1. **Modularity**: Each subsystem has a clear interface and can be replaced independently
2. **Performance**: GPU acceleration on Jetson, optimized local LLM inference
3. **Reliability**: Retry logic, checksums, error recovery in network protocol
4. **Flexibility**: Support for multiple ASR engines, LLM providers, and audio backends
5. **Embedded-First**: Designed for resource-constrained platforms (static allocation preferred)

---

## Subsystem Architecture

### 1. Core Subsystem (`src/` root)

**Purpose**: Main application logic, logging, utilities, MQTT integration

#### Key Components

- **dawn.c/h**: Main application entry point
  - State machine for local audio processing
  - Integration point for all subsystems
  - Conversation history management
  - Application lifecycle control

- **logging.c/h**: Centralized logging system
  - Macros: `LOG_INFO()`, `LOG_WARNING()`, `LOG_ERROR()`
  - Timestamp formatting
  - Consistent log formatting across all subsystems

- **mosquitto_comms.c/h**: MQTT integration
  - MQTT client for pub/sub messaging
  - Device callback registration system
  - Command routing to device handlers
  - Integration with other OASIS components or external systems

- **text_to_command_nuevo.c/h**: Command parsing and execution
  - Parses LLM responses for `<command>` JSON tags
  - Routes commands to appropriate device callbacks
  - Supports both direct pattern matching and LLM-based commands

- **word_to_number.c/h**: Natural language number parsing
  - Converts text numbers to integers ("twenty-three" → 23)
  - Used for command parsing

### 2. ASR Subsystem (`src/asr/`, `include/asr/`)

**Purpose**: Speech recognition with multiple engine support

#### Architecture Pattern: **Strategy Pattern**

The ASR subsystem uses an abstraction layer (`asr_interface`) to support multiple ASR engines (Whisper, Vosk) interchangeably.

#### Key Components

- **asr_interface.c/h**: ASR abstraction layer
  - `ASRContext` struct: Engine-agnostic context
  - `asr_init()`: Initialize selected ASR engine
  - `asr_process_audio()`: Process audio through selected engine
  - `asr_cleanup()`: Clean up resources
  - Engine selection based on compile-time flags (`ENABLE_VOSK`)

- **asr_whisper.c/h**: Whisper ASR implementation
  - Uses whisper.cpp library
  - GPU acceleration on Jetson (CUDA)
  - Support for multiple model sizes (tiny, base, small)
  - Recommended: **base.en** (best balance of speed/accuracy)
  - VAD-driven pause detection for natural speech boundaries

- **asr_vosk.c/h**: Vosk ASR implementation (optional, legacy)
  - Uses Vosk API with Kaldi backend
  - GPU-accelerated when available (via `vosk_gpu_init()`)
  - Smaller memory footprint than Whisper
  - Compiled only when `ENABLE_VOSK=ON`

- **vad_silero.c/h**: Voice Activity Detection
  - Uses Silero VAD ONNX model
  - Real-time speech/silence classification
  - Drives pause detection for chunking
  - Configurable sensitivity threshold

- **chunking_manager.c/h**: Long utterance handling
  - Manages multi-chunk speech sequences
  - VAD-driven pause detection
  - Assembles partial results into complete transcriptions
  - Prevents premature cutoff of long commands

#### Data Flow

```
Audio Input → VAD (Silero) → Chunking Manager → ASR Engine (Whisper/Vosk) → Transcript
                   ↓                                      ↑
              Speech/Silence                         GPU Acceleration
              Classification                         (Jetson + Vosk support)
```

#### Performance

| Model        | Platform   | RTF    | Speedup | Accuracy  |
|--------------|------------|--------|---------|-----------|
| Whisper tiny | Jetson GPU | 0.079  | 12.7x   | Good      |
| Whisper base | Jetson GPU | 0.109  | 9.2x    | Excellent |
| Whisper small| Jetson GPU | 0.225  | 4.4x    | Best      |
| Vosk 0.22    | CPU/GPU    | ~0.15  | 6.7x    | Good      |

**RTF = Real-Time Factor** (lower is faster; 1.0 = realtime, <1.0 = faster than realtime)

### 3. LLM Subsystem (`src/llm/`, `include/llm/`)

**Purpose**: Large Language Model integration with streaming support

#### Architecture Pattern: **Strategy + Observer Patterns**

- **Strategy**: Multiple LLM providers (OpenAI, Claude, local) via unified interface
- **Observer**: Streaming responses notify sentence buffer for real-time TTS

#### Key Components

- **llm_interface.c/h**: LLM abstraction layer
  - `LLMContext` struct: Provider-agnostic context
  - `llm_init()`: Initialize selected provider
  - `llm_send_message()`: Send message, get complete response (blocking)
  - `llm_send_message_streaming()`: Send message, stream response chunks
  - Provider selection based on configuration (`OPENAI_MODEL`, `ANTHROPIC_MODEL`)

- **llm_openai.c/h**: OpenAI API implementation
  - Supports GPT-4o, GPT-4, GPT-3.5
  - Also supports llama.cpp local server (OpenAI-compatible endpoint)
  - Both blocking and streaming modes
  - Conversation history management

- **llm_claude.c/h**: Claude API implementation
  - Supports Claude 4.5 Sonnet, Claude 3 Opus
  - Streaming support
  - Different API format than OpenAI (Messages API)

- **llm_streaming.c/h**: Streaming response handler
  - Manages Server-Sent Events (SSE) connections
  - Buffers and parses incoming chunks
  - Notifies sentence buffer for TTS integration

- **sse_parser.c/h**: Server-Sent Events parser
  - Parses SSE format: `data: {...}\n\n`
  - Extracts JSON content from events
  - Handles partial events across network chunks

- **sentence_buffer.c/h**: Sentence boundary detection
  - Buffers streaming text until complete sentence
  - Detects sentence boundaries (`.`, `!`, `?`)
  - Sends complete sentences to TTS for natural phrasing
  - Reduces perceived latency (speak while generating)

- **llm_command_parser.c/h**: JSON command extraction
  - Extracts `<command>` JSON tags from LLM responses
  - Validates JSON structure
  - Handles malformed JSON gracefully

#### Data Flow (Streaming Mode)

```
User Query → LLM Provider (OpenAI/Claude/Local)
                    ↓ (SSE stream)
            SSE Parser → Streaming Handler
                    ↓ (text chunks)
            Sentence Buffer → TTS (as sentences complete)
                    ↓ (complete response)
            Command Parser → MQTT Commands
```

#### Performance Comparison

| Provider                | Quality | TTFT      | Latency | Cost          |
|-------------------------|---------|-----------|---------|---------------|
| OpenAI GPT-4o           | 100%    | ~300ms    | ~3.1s   | ~$0.01/query  |
| Claude 4.5 Sonnet       | 92.4%   | ~400ms    | ~3.5s   | ~$0.015/query |
| llama.cpp (Qwen3-4B Q4) | 81.9%   | 116-138ms | ~1.5s   | FREE          |

**TTFT = Time To First Token** (lower = faster perceived response)

### 4. TTS Subsystem (`src/tts/`, `include/tts/`)

**Purpose**: High-quality text-to-speech synthesis

#### Key Components

- **text_to_speech.cpp/h**: TTS engine wrapper
  - Thread-safe interface with mutex protection (`tts_mutex`)
  - Converts text to WAV audio
  - Text preprocessing for natural phrasing (em-dash conversion, etc.)
  - Supports streaming integration with LLM sentence buffer

- **piper.cpp**: Piper TTS integration
  - Uses Piper library with ONNX Runtime
  - Phoneme-based synthesis
  - Multiple voice models supported
  - Recommended: `en_GB-alba-medium` (good quality, reasonable speed)

#### Data Flow

```
Text → Preprocessing → Piper Phonemization → ONNX Inference → WAV Audio → ALSA/Pulse
```

#### Thread Safety

TTS is protected by a global mutex (`tts_mutex`) to prevent concurrent access from:
- Main loop (local audio)
- Network server thread (remote audio)
- Streaming LLM sentence buffer

### 5. Network Subsystem (`src/network/`, `include/network/`)

**Purpose**: Network audio server for remote ESP32 clients

#### Architecture: **Single-Threaded Server** (currently)

**Note**: Multi-client support is planned (see `remote_dawn/dawn_multi_client_architecture.md`) but not yet implemented.

#### Key Components

- **dawn_server.c/h**: Network audio server
  - TCP server listening on configurable port (default: 8080)
  - Implements Dawn Audio Protocol (DAP v1.1)
  - Single client at a time (blocks main loop during processing)
  - Handles handshake, data reception, ACK/NACK, retries

- **dawn_network_audio.c/h**: Network audio processing
  - Extracts PCM data from received WAV files
  - Processes audio through ASR → LLM → TTS pipeline
  - Generates WAV response for client playback

- **dawn_wav_utils.c/h**: WAV file utilities
  - WAV header parsing and generation
  - Format validation
  - PCM data extraction

#### Dawn Audio Protocol (DAP)

**Version**: 1.1
**Type**: Binary protocol with reliability features

**Packet Structure**:
```
┌────────────┬─────────────┬──────────────┬───────────┬─────────────┐
│ Data Length│  Protocol   │ Packet Type  │  Checksum │  Payload    │
│  (4 bytes) │ Version (1) │     (1)      │ (2 bytes) │ (variable)  │
└────────────┴─────────────┴──────────────┴───────────┴─────────────┘
```

**Packet Types**:
- `HANDSHAKE` (0x01): Connection establishment
- `DATA` (0x02): Audio data chunk
- `DATA_END` (0x03): End of audio stream
- `ACK` (0x04): Acknowledgment
- `NACK` (0x05): Negative acknowledgment
- `RETRY` (0x06): Retransmission request

**Reliability Features**:
- Fletcher-16 checksums for data integrity
- Sequence numbers for ordered delivery
- ACK/NACK handshake for each packet
- Configurable retry logic with exponential backoff

**Critical Requirement**: Client and server MUST use identical `PACKET_MAX_SIZE` (default: 8192 bytes)

#### Data Flow (Network Client)

```
ESP32 Client → TCP Connection → Handshake → Audio Stream (DAP packets)
                                                    ↓
                                            Server Validates
                                                    ↓
                                            ASR → LLM → TTS
                                                    ↓
                                            WAV Response → ESP32
                                                    ↓
                                            ESP32 Playback
```

### 6. Audio Subsystem (`src/audio/`, `include/audio/`)

**Purpose**: Audio capture, buffering, and playback

#### Key Components

- **audio_capture_thread.c/h**: Dedicated audio capture thread
  - Runs in separate thread to avoid blocking main loop
  - Continuous capture from ALSA/PulseAudio device
  - Writes to ring buffer for main loop consumption
  - Handles capture errors gracefully

- **ring_buffer.c/h**: Thread-safe circular buffer
  - Lock-free or mutex-protected (implementation dependent)
  - Fixed-size buffer for audio samples
  - Overwrite policy when buffer full
  - Used for smooth audio streaming between threads

- **flac_playback.c/h**: Music/audio file playback
  - FLAC audio file decoding
  - ALSA/PulseAudio output
  - Used for notification sounds, music playback

- **mic_passthrough.c/h**: Microphone passthrough
  - Direct microphone → speaker routing
  - Used for testing, debugging
  - Useful for verifying audio capture/playback setup

#### Threading Model

```
┌──────────────────┐
│ Capture Thread   │ (continuous capture)
│                  │
│ ALSA/Pulse → RB  │ (write to ring buffer)
└────────┬─────────┘
         │
    Ring Buffer (thread-safe)
         │
┌────────▼─────────┐
│   Main Thread    │ (state machine)
│                  │
│   RB → VAD → ASR │ (read from ring buffer)
└──────────────────┘
```

**RB = Ring Buffer**

---

## Data Flow

### Local Voice Command Flow

```
1. Microphone Capture
   ↓
2. Ring Buffer (Capture Thread → Main Thread)
   ↓
3. VAD Detection (Silero) - Detect speech/silence
   ↓
4. State Machine Transition (SILENCE → WAKEWORD_LISTEN)
   ↓
5. Wake Word Detection (Whisper/Vosk) - "friday"
   ↓
6. State Machine Transition (WAKEWORD_LISTEN → COMMAND_RECORDING)
   ↓
7. Command Recording (with VAD monitoring for silence)
   ↓
8. ASR Processing (Whisper/Vosk with GPU acceleration)
   ↓
9. Transcript → LLM (OpenAI GPT-4o/Claude 4.5/local with streaming)
   ↓
10. LLM Response Streaming
    ↓
11. Sentence Buffer (accumulate until sentence boundary)
    ↓
12. TTS Synthesis (Piper with preprocessing) - Per-sentence
    ↓
13. Audio Playback (ALSA/PulseAudio)
    ↓
14. Command Parsing (extract JSON commands)
    ↓
15. MQTT Publish (execute OASIS component commands)
    ↓
16. State Machine Transition (PROCESSING → SILENCE)
```

### Network Voice Command Flow

```
1. ESP32 Client Connection (TCP)
   ↓
2. DAP Handshake (establish protocol version)
   ↓
3. Audio Stream Reception (DAP packets with checksums)
   ↓
4. WAV Assembly (from packets)
   ↓
5. PCM Extraction (from WAV)
   ↓
6. ASR Processing (Whisper/Vosk)
   ↓
7. LLM Processing (same as local flow)
   ↓
8. TTS Synthesis (generate response WAV)
   ↓
9. WAV Transmission (DAP packets to ESP32)
   ↓
10. ESP32 Playback
```

---

## Threading Model

### Current Threading Architecture

D.A.W.N. uses a **minimal threading model** for simplicity on embedded systems:

```
┌──────────────────────────────────────────────────────────┐
│                      Main Thread                         │
│                                                          │
│  - State machine (SILENCE → WAKEWORD → COMMAND → PROC)   │
│  - ASR processing (Whisper/Vosk)                         │
│  - LLM communication (blocking or streaming)             │
│  - TTS synthesis (mutex protected)                       │
│  - MQTT communication                                    │
│  - Network server (single client, blocks during proc)    │
└──────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────┐
│                   Capture Thread                         │
│                                                          │
│  - Continuous audio capture (ALSA/PulseAudio)            │
│  - Write to ring buffer                                  │
│  - No processing logic                                   │
└──────────────────────────────────────────────────────────┘
```

### Thread Synchronization

- **Ring Buffer**: Thread-safe circular buffer for audio data
- **TTS Mutex** (`tts_mutex`): Protects TTS engine from concurrent access
- **No locks in main loop**: State machine is single-threaded

### Planned Improvements (Multi-Client Architecture)

See `remote_dawn/dawn_multi_client_architecture.md` for details:

- **Worker Thread Pool**: Handle multiple network clients concurrently
- **Per-Client Session**: Separate conversation history and state
- **Non-Blocking Main Loop**: Local audio processing not blocked by network clients

---

## State Machine

The main application (`src/dawn.c`) implements a state machine for local voice processing:

```
                    ┌─────────────┐
                    │   SILENCE   │ (Listening for wake word)
                    └──────┬──────┘
                           │ VAD detects speech
                           ↓
                    ┌─────────────────────┐
                    │  WAKEWORD_LISTEN    │ (Detecting wake word)
                    └──────┬──────────────┘
                           │ Wake word detected ("friday")
                           ↓
                    ┌─────────────────────┐
                    │ COMMAND_RECORDING   │ (Recording user command)
                    └──────┬──────────────┘
                           │ VAD detects silence (end of command)
                           ↓
                    ┌─────────────────────┐
                    │    PROCESSING       │ (ASR → LLM → TTS → MQTT)
                    └──────┬──────────────┘
                           │ Processing complete
                           ↓
                    ┌─────────────┐
                    │   SILENCE   │ (Return to listening)
                    └─────────────┘
```

### State Descriptions

- **SILENCE**: Waiting for speech detection (VAD monitors audio)
- **WAKEWORD_LISTEN**: Detected speech, listening for wake word ("friday")
- **COMMAND_RECORDING**: Wake word confirmed, recording user command until silence
- **PROCESSING**: Command recorded, processing through ASR → LLM → TTS → MQTT pipeline

### State Transitions

| From State        | Event                    | To State          |
|-------------------|--------------------------|-------------------|
| SILENCE           | VAD detects speech       | WAKEWORD_LISTEN   |
| WAKEWORD_LISTEN   | Wake word detected       | COMMAND_RECORDING |
| WAKEWORD_LISTEN   | Timeout / false alarm    | SILENCE           |
| COMMAND_RECORDING | VAD detects silence      | PROCESSING        |
| PROCESSING        | Pipeline complete        | SILENCE           |

---

## Network Protocol

### Dawn Audio Protocol (DAP) v1.1

**Design Goals**:
- Reliable audio transmission over potentially lossy networks
- Simple implementation on resource-constrained ESP32
- Minimal overhead for embedded systems

### Packet Format

```
Byte Offset:   0       4        5        6          8      ...
             ┌────────┬────────┬────────┬──────────┬─────────────┐
             │ Length │ Ver    │ Type   │ Checksum │  Payload    │
             │ (4B)   │ (1B)   │ (1B)   │ (2B)     │  (variable) │
             └────────┴────────┴────────┴──────────┴─────────────┘

Length:   uint32_t - Payload length (little-endian)
Version:  uint8_t  - Protocol version (0x01)
Type:     uint8_t  - Packet type (see below)
Checksum: uint16_t - Fletcher-16 checksum of payload
Payload:  uint8_t[] - Packet data (length specified in header)
```

### Packet Types

```c
#define PACKET_TYPE_HANDSHAKE  0x01  // Connection establishment
#define PACKET_TYPE_DATA       0x02  // Audio data chunk
#define PACKET_TYPE_DATA_END   0x03  // End of audio stream
#define PACKET_TYPE_ACK        0x04  // Acknowledgment
#define PACKET_TYPE_NACK       0x05  // Negative acknowledgment
#define PACKET_TYPE_RETRY      0x06  // Retransmission request
```

### Connection Flow

```
Client                                Server
  │                                      │
  │─────── HANDSHAKE ───────────────────>│
  │                                      │
  │<─────────── ACK ─────────────────────│
  │                                      │
  │─────── DATA (chunk 1) ──────────────>│
  │                                      │
  │<─────────── ACK ─────────────────────│
  │                                      │
  │─────── DATA (chunk 2) ──────────────>│
  │                                      │
  │<─────────── ACK ─────────────────────│
  │                                      │
  │           ... (more chunks)          │
  │                                      │
  │─────── DATA_END ────────────────────>│
  │                                      │
  │<─────────── ACK ─────────────────────│
  │                                      │
  │         (server processing...)       │
  │                                      │
  │<────── DATA (response WAV) ──────────│
  │                                      │
  │─────────── ACK ─────────────────────>│
  │                                      │
  │<────── DATA_END ─────────────────────│
  │                                      │
  │─────────── ACK ─────────────────────>│
  │                                      │
```

### Error Handling

**Checksum Mismatch**:
```
Client → DATA (bad checksum) → Server
Client ← NACK ← Server
Client → RETRY (resend) → Server
Client ← ACK ← Server
```

**Timeout**:
- Client waits up to 5 seconds for ACK
- After 3 failed retries, connection terminates
- Server waits up to 30 seconds for client data

### Configuration Requirements

**CRITICAL**: Client and server MUST agree on:
- `PACKET_MAX_SIZE` (default: 8192 bytes)
- `PROTOCOL_VERSION` (0x01)

Mismatch causes connection failure or data corruption.

---

## Component Interactions

### ASR Engine Selection

```c
// src/asr/asr_interface.c
ASRContext *asr_init(const char *model_path) {
#ifdef ENABLE_VOSK
   return asr_vosk_init(model_path);  // Use Vosk if enabled
#else
   return asr_whisper_init(model_path);  // Otherwise use Whisper
#endif
}
```

### LLM Provider Selection

```c
// src/llm/llm_interface.c
LLMContext *llm_init() {
   if (strstr(OPENAI_MODEL, "gpt")) {
      return llm_openai_init();  // OpenAI models (GPT-4o, etc.)
   } else if (strstr(OPENAI_API_BASE, "localhost")) {
      return llm_openai_init();  // llama.cpp local server (OpenAI-compatible)
   } else if (ANTHROPIC_API_KEY[0] != '\0') {
      return llm_claude_init();  // Claude models
   }
   return NULL;
}
```

### TTS Thread Safety

```c
// src/tts/text_to_speech.cpp
void text_to_speech(const char *text) {
   pthread_mutex_lock(&tts_mutex);  // Protect TTS engine

   // Piper synthesis
   piper_synthesize(text);

   pthread_mutex_unlock(&tts_mutex);
}
```

### Streaming LLM → TTS Integration

```c
// src/llm/llm_streaming.c
void on_llm_chunk_received(const char *chunk) {
   sentence_buffer_append(chunk);  // Accumulate text

   if (sentence_buffer_has_complete_sentence()) {
      char *sentence = sentence_buffer_get_sentence();
      text_to_speech(sentence);  // Speak complete sentence immediately
      free(sentence);
   }
}
```

---

## Memory Management

### Design Principles

1. **Prefer Static Allocation**: Embedded systems benefit from predictable memory usage
2. **Minimize Dynamic Allocation**: Use malloc/calloc sparingly
3. **Always Check NULL**: Verify dynamic allocation succeeded
4. **Free and NULL**: Set pointers to NULL after freeing

### Memory Patterns

#### Static Buffers (Preferred)
```c
// Example: Audio buffer
#define AUDIO_BUFFER_SIZE 16000
static int16_t audio_buffer[AUDIO_BUFFER_SIZE];
```

#### Dynamic Allocation (When Necessary)
```c
char *response = malloc(response_len);
if (response == NULL) {
   LOG_ERROR("Failed to allocate response buffer");
   return FAILURE;
}
// ... use response ...
free(response);
response = NULL;
```

#### Ring Buffer (Lock-Free)
```c
// Circular buffer with atomic read/write pointers
typedef struct {
   int16_t *buffer;
   size_t size;
   volatile size_t read_idx;
   volatile size_t write_idx;
} RingBuffer;
```

### Memory Usage Estimates

| Component       | Memory Usage       | Notes                          |
|-----------------|--------------------|--------------------------------|
| Whisper base    | ~140 MB            | Model weights + context        |
| Vosk 0.22       | ~50 MB             | Smaller footprint              |
| Silero VAD      | ~2 MB              | Tiny ONNX model                |
| Piper TTS       | ~30 MB             | Voice model + ONNX runtime     |
| Ring Buffer     | ~256 KB            | 16kHz × 16-bit × 8s buffer     |
| Conversation    | ~10 KB             | History for LLM context        |

**Total (Whisper)**: ~230 MB RAM minimum
**Total (Vosk)**: ~140 MB RAM minimum

---

## Error Handling

### Error Code Convention

```c
#define SUCCESS  0
#define FAILURE  1
// Additional specific errors > 1
#define ERROR_INVALID_FORMAT  2
#define ERROR_NETWORK_TIMEOUT 3
// ... etc
```

**IMPORTANT**: Do NOT use negative return values (-1, -errno). Use positive error codes only.

### Error Handling Patterns

#### Function Return Codes
```c
int asr_process_audio(ASRContext *ctx, int16_t *audio, size_t samples) {
   if (ctx == NULL || audio == NULL) {
      LOG_ERROR("Invalid parameters");
      return FAILURE;
   }

   // ... processing ...

   if (error_occurred) {
      LOG_ERROR("ASR processing failed: %s", error_msg);
      return FAILURE;
   }

   return SUCCESS;
}
```

#### Network Protocol Errors
```c
// Retry logic with exponential backoff
int retry_count = 0;
while (retry_count < MAX_RETRIES) {
   if (send_packet(packet) == SUCCESS) {
      break;
   }
   LOG_WARNING("Send failed, retry %d/%d", retry_count + 1, MAX_RETRIES);
   sleep(1 << retry_count);  // Exponential backoff: 1s, 2s, 4s
   retry_count++;
}
```

#### Graceful Degradation
```c
// Example: Fall back to simpler ASR if GPU unavailable
if (gpu_available) {
   ctx = asr_whisper_init(model_path);
} else {
   LOG_WARNING("GPU not available, using CPU-only ASR");
   ctx = asr_vosk_init(model_path);
}
```

---

## Performance Considerations

### GPU Acceleration (Jetson)

- **Automatic Detection**: CMake detects Jetson platform via `/etc/nv_tegra_release`
- **CUDA Libraries**: cuSPARSE, cuBLAS, cuSOLVER, cuRAND linked automatically
- **Whisper GPU**: Enabled with `GGML_CUDA=ON` for Whisper.cpp
- **Performance Gain**: 2.3x - 5.5x speedup over CPU

### Latency Optimization

**Total Perceived Latency** = ASR Time + TTFT + TTS Time

| Component         | Latency (Whisper base GPU) | Notes                    |
|-------------------|----------------------------|--------------------------|
| ASR (Whisper base)| ~110 ms                    | GPU accelerated          |
| TTFT (Qwen3-4B)   | ~138 ms                    | Local LLM first token    |
| TTS (Piper)       | ~200 ms                    | First sentence           |
| **Total**         | **~448 ms**                | User hears first response|

**Streaming Advantage**: With streaming LLM + TTS, user hears response in <500ms instead of waiting for complete LLM response (~3s).

### Optimization Tips

1. **Use Whisper base model**: Best balance of speed/accuracy on Jetson
2. **Enable GPU acceleration**: Automatic on Jetson, verify in logs
3. **Streaming LLM + TTS**: Reduces perceived latency significantly
4. **Optimize batch size**: For local LLM, batch=768 critical for quality
5. **VAD tuning**: Adjust sensitivity to reduce false positives

---

## Platform Support

### NVIDIA Jetson

- **Auto-Detection**: CMake checks `/etc/nv_tegra_release`
- **GPU Acceleration**: Enabled automatically (CUDA 12.6)
- **Recommended Models**: Whisper base, Qwen3-4B Q4
- **Performance**: Excellent (GPU acceleration for ASR)

### Raspberry Pi

- **Auto-Detection**: CMake checks `/sys/firmware/devicetree/base/model`
- **CPU-Only**: No CUDA support
- **Recommended Models**: Vosk 0.22, smaller Whisper models
- **Performance**: Good (CPU-only, slower ASR)

### Generic ARM64

- **Fallback Detection**: CMake checks `CMAKE_SYSTEM_PROCESSOR`
- **Assumed Platform**: RPI (no GPU)
- **Performance**: Varies by hardware

### Platform Override

```bash
cmake -DPLATFORM=JETSON ..  # Force Jetson (enables CUDA)
cmake -DPLATFORM=RPI ..     # Force RPi (disables CUDA)
```

---

## Configuration Files

### secrets.toml (API Keys)

```toml
# Create in project root or ~/.config/dawn/secrets.toml
openai_api_key = "sk-..."           # For GPT-4o
claude_api_key = "sk-ant-..."       # For Claude 4.5 Sonnet
```

**Note**: Already in `.gitignore` - never commit API keys!

### dawn.h (System Configuration)

Key settings:
- `AI_NAME`: Wake word ("friday")
- `AI_DESCRIPTION`: System prompt for LLM
- `OPENAI_MODEL`: LLM model name
- `DEFAULT_PCM_PLAYBACK_DEVICE`: ALSA playback device
- `DEFAULT_PCM_CAPTURE_DEVICE`: ALSA capture device
- `MQTT_IP`: MQTT broker address
- `MQTT_PORT`: MQTT broker port

### commands_config_nuevo.json (Device Mappings)

```json
{
  "devices": [
    {
      "type": "light",
      "name": "living_room",
      "actions": ["on", "off", "dim"]
    }
  ]
}
```

---

## Future Improvements

### Planned Features

1. **Multi-Client Network Server** (see `remote_dawn/dawn_multi_client_architecture.md`)
   - Worker thread pool for concurrent clients
   - Per-client conversation history
   - Non-blocking main loop

2. **Conversation Persistence**
   - Save conversation history to disk
   - Resume conversations across restarts

3. **Improved Error Recovery**
   - Auto-reconnect for MQTT
   - Network protocol timeout tuning
   - ASR fallback on GPU failure

4. **CI/CD Pipeline**
   - GitHub Actions for automated builds
   - Regression testing
   - Code quality checks

5. **Enhanced Testing**
   - Integration tests (end-to-end)
   - Stress tests (concurrent clients, memory leaks)
   - Automated regression tests

---

## References

- **Piper TTS**: https://github.com/rhasspy/piper
- **Vosk ASR**: https://alphacephei.com/vosk/
- **Whisper**: https://github.com/ggerganov/whisper.cpp
- **Silero VAD**: https://github.com/snakers4/silero-vad
- **llama.cpp**: https://github.com/ggerganov/llama.cpp
- **ONNX Runtime**: https://github.com/microsoft/onnxruntime

---

**Document Version**: 1.0
**Last Updated**: November 25, 2025 (after LLM interrupt implementation)
**Reorganization Commit**: [Git SHA to be added after commit]

### LLM Threading Architecture (Post-Interrupt Implementation)

**Status**: ✅ **Implemented** (November 2025)

D.A.W.N. now uses **non-blocking LLM processing** via a dedicated worker thread:

```
┌───────────────────────────────────────────────────────────┐
│                      Main Thread                          │
│                                                           │
│  - State machine (NEVER blocks on LLM)                    │
│  - Audio capture + VAD  (continuous, 50ms intervals)      │
│  - ASR processing (Whisper/Vosk)                          │
│  - TTS synthesis (mutex protected)                        │
│  - LLM completion detection (polling llm_processing flag) │
│  - MQTT communication                                     │
└────────────┬──────────────────────────────────────────────┘
             │
             │ Spawns on-demand, max 1 concurrent
             ↓
┌───────────────────────────────────────────────────────────┐
│                   LLM Worker Thread                       │
│                                                           │
│  - Blocking CURL call to LLM API                          │
│  - CURL progress callback (checks interrupt flag)         │
│  - Returns response via shared buffer                     │
│  - Thread-safe via llm_mutex                              │
└───────────────────────────────────────────────────────────┘
```

#### Key Components

- `pthread_t llm_thread` - Worker thread handle
- `pthread_mutex_t llm_mutex` - Protects shared request/response buffers
- `volatile int llm_processing` - Atomic flag: 1 = running, 0 = idle
- `volatile sig_atomic_t llm_interrupt_requested` - Signal-safe interrupt flag

#### Memory Ownership Transfer

Request and response buffers use **ownership transfer** to prevent data races:

```c
// Main thread → Worker thread:
llm_request_text = command_text;
command_text = NULL;  // Ownership transferred

// Worker thread → Main thread:
char *response = llm_response_text;
llm_response_text = NULL;  // Ownership transferred back
```

**Rules**:
- Worker thread **owns** request buffer, frees after use
- Main thread **owns** response buffer, frees after processing
- Mutex held only during transfer, not during processing

#### LLM Interrupt Mechanism

**Purpose**: Allow users to interrupt ongoing LLM requests by saying the wake word.

**Implementation**:
- CURL progress callback checks `llm_interrupt_requested` flag periodically
- Wake word detection in main loop sets flag via `llm_request_interrupt()`
- Returns non-zero from callback to abort CURL transfer
- Main thread detects interrupt, discards partial response, rolls back conversation history

**See**: `LLM_INTERRUPT_IMPLEMENTATION.md` for complete implementation details.

---

## Mutex Lock Ordering Hierarchy

**CRITICAL**: To prevent deadlocks, always acquire mutexes in this order when multiple locks needed:

```
Level 1 (acquire first):   metrics_mutex       (TUI metrics - future)
Level 2 (acquire second):   llm_mutex           (LLM thread communication)
                            tts_mutex           (TTS playback state)
                            processing_mutex    (Network processing state)
Level 3 (acquire last):     network_processing_mutex  (Network PCM buffer)
```

### Lock Ordering Rules

1. **Never acquire a lower-level lock while holding a higher-level lock**
   - ❌ BAD: Hold `tts_mutex` → acquire `metrics_mutex` (2→1 violates order)
   - ✅ GOOD: Acquire `metrics_mutex` → then `tts_mutex` (1→2 correct)

2. **Never hold multiple level-2 locks simultaneously**
   - ❌ BAD: Hold `llm_mutex` → acquire `tts_mutex` (both level-2)
   - ✅ GOOD: Release `llm_mutex` before acquiring `tts_mutex`

3. **Keep critical sections minimal**
   - Copy data, release lock, **then** process data
   - Avoid I/O operations while holding locks

4. **Prefer lock-free patterns for high-frequency updates**
   - VAD probability: Use C11 atomics instead of mutex (future)
   - State flags: Use `volatile` types for simple booleans

### Testing Lock Discipline

Use **ThreadSanitizer** during development:

```bash
cd build
cmake -DCMAKE_C_FLAGS="-fsanitize=thread -g" ..
make
./dawn
```

ThreadSanitizer detects:
- Data races (unsynchronized shared variable access)
- Lock order inversions (potential deadlocks)
- Use-after-free in threaded code

---

## Architectural Recommendations

### High Priority (Before TUI Implementation)

1. ✅ **Document lock ordering** (completed in this file)
2. **ncurses non-blocking mode**: Set `nodelay(stdscr, TRUE)` to prevent keyboard blocking
3. **ThreadSanitizer testing**: Run with `-fsanitize=thread` during TUI development

### Medium Priority (During TUI Implementation)

4. **Lock-free VAD probability**: Use C11 `atomic_uint` for high-frequency updates
5. **Batch metrics updates**: Acquire `metrics_mutex` once for multiple changes

### Low Priority (Post-TUI)

6. **Session layer for multi-client**: Design per-client conversation history isolation
7. **SIGSEGV crash stats**: Add signal handler to export metrics on crash (best-effort)

### Performance Characteristics (Updated)

| Component         | CPU Impact | Memory Impact | Notes                 |
|-------------------|------------|---------------|-----------------------|
| Main audio loop   | 15-20%     | Varies        | VAD + ASR processing  |
| LLM worker thread | 0.01%      | ~8KB          | CURL callback polling |
| TTS worker thread | 5-10%      | ~8KB          | During synthesis      |

**LLM Threading Benefit**: Main audio loop **never blocks** during LLM processing, maintaining responsive wake word detection even during 10-15 second LLM calls.

---

