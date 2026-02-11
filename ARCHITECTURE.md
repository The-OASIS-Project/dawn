# D.A.W.N. System Architecture

This document describes the architecture of the D.A.W.N. (Digital Assistant for Workflow Neural-inference) voice assistant system.

**D.A.W.N.** is the central intelligence layer of the OASIS ecosystem, responsible for interpreting user intent, fusing data from every subsystem, and routing commands. At its core, DAWN performs neural-inference to understand context and drive decision-making, acting as OASIS's orchestration hub for MIRAGE, AURA, SPARK, STAT, and any future modules.

**Last Updated**: February 11, 2026 (DAP1 eliminated, unified DAP2 WebSocket for all satellites)

## Table of Contents

- [High-Level Overview](#high-level-overview)
- [Subsystem Architecture](#subsystem-architecture)
- [Data Flow](#data-flow)
- [Threading Model](#threading-model)
- [State Machine](#state-machine)
- [Network Protocol](#network-protocol)
- [DAP2 Satellite Protocol](#dap2-satellite-protocol)
- [Command Processing Architecture](#command-processing-architecture)
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
         │ Local Audio                  │ WebSocket (WebUI + Satellites)
         │                              │
    ┌────▼─────────┐            ┌───────▼──────────┐
    │ Audio Capture│            │  WebUI Server    │
    │ Thread       │            │ (libwebsockets)  │
    │ + Ring Buffer│            │ Multi-client     │
    └────┬─────────┘            └───────┬──────────┘
         │                              │
    ┌────▼──────────┐           ┌───────▼──────────┐
    │  VAD (Silero) │           │ Session Manager  │
    │  Speech/Noise │           │ + Audio Workers  │
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
         ┌──────────┼─────────┬──────────┐
         │          │         │          │
    ┌────▼───┐ ┌───▼────┐ ┌──▼───┐ ┌───▼──────┐
    │ OpenAI │ │ Claude │ │Gemini│ │ llama.cpp│
    │ GPT-4o │ │ 4.5    │ │ 2.5  │ │ (local)  │
    └────┬───┘ └───┬────┘ └──┬───┘ └───┬──────┘
         │         │         │         │
         └─────────┴────┬────┴─────────┘
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

- **Strategy**: Multiple LLM providers (OpenAI, Claude, Gemini, local) via unified interface
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
  - Supports llama.cpp local server (OpenAI-compatible endpoint)
  - Supports Ollama with runtime model switching
  - Supports Google Gemini (via OpenAI-compatible endpoint)
  - Both blocking and streaming modes
  - Conversation history management
  - Extended thinking support (reasoning_effort for OpenAI/Gemini models)

- **llm_claude.c/h**: Claude API implementation
  - Supports Claude 4.5 Sonnet, Claude 3 Opus
  - Streaming support
  - Different API format than OpenAI (Messages API)
  - Extended thinking support with configurable token budget
  - Full thinking content visibility (unlike OpenAI/Gemini)

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
| Gemini 2.5 Flash        | ~90%    | ~250ms    | ~2.5s   | ~$0.002/query |
| llama.cpp (Qwen3-4B Q4) | 81.9%   | 116-138ms | ~1.5s   | FREE          |
| Ollama (Qwen3-4B Q4)    | 81.9%   | ~150ms    | ~1.6s   | FREE          |

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

### 5. Network Subsystem (`src/network/`, `include/network/`) — DEPRECATED

**Purpose**: Legacy DAP1 network audio server for ESP32 clients

**Status**: **DEPRECATED** — DAP1 is eliminated. All satellite communication (including ESP32 Tier 2) now uses WebSocket via the WebUI server. See [Section 5b: DAP2 Satellite Subsystem](#5b-dap2-satellite-subsystem) for the unified replacement. The code in `src/network/` and `remote_dawn/` is retained for historical reference only.

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

### 5b. DAP2 Satellite Subsystem (`dawn_satellite/`, `common/`, `src/webui/webui_satellite.c`)

**Purpose**: Text-first WebSocket protocol for Tier 1 satellite devices (Raspberry Pi 4+)

**Design Documents**: [DAP2_DESIGN.md](docs/DAP2_DESIGN.md) | [DAP2_SATELLITE.md](docs/DAP2_SATELLITE.md)

#### Architecture: **Local ASR/TTS + Remote LLM**

Unlike DAP1 (which streams raw audio to the daemon), DAP2 satellites handle speech recognition and text-to-speech locally and send only text to the daemon over WebSocket.

```
┌─────────────────────────────────────────────────────────────┐
│                    DAWN Satellite (Tier 1)                    │
│                                                              │
│  Audio Capture → VAD (Silero) → Wake Word → ASR (Vosk)      │
│                                                   │ text     │
│  Audio Playback ← TTS (Piper) ← sentence_buffer ←│          │
│                                                              │
│  ┌────────────────────────────────────────────────────────┐  │
│  │  SDL2 Touchscreen UI (Optional, KMSDRM backend)       │  │
│  │  Orb Visualizer | Transcript | Music Panel | Settings  │  │
│  └────────────────────────────────────────────────────────┘  │
│                                                              │
│  ┌────────────────────────────────────────────────────────┐  │
│  │  Music Stream (separate WebSocket)                     │  │
│  │  Opus decode → ALSA playback → Goertzel FFT visualizer │  │
│  └────────────────────────────────────────────────────────┘  │
└──────────────────────────┬───────────────────────────────────┘
                           │ WebSocket JSON (control)
                           │ WebSocket binary (Opus music audio)
                           ▼
┌─────────────────────────────────────────────────────────────┐
│                       DAWN Daemon                            │
│  webui_satellite.c: register, query, streaming response      │
│  webui_music.c: music control, library browse, Opus stream   │
└─────────────────────────────────────────────────────────────┘
```

#### Key Components

**Satellite side** (`dawn_satellite/`):

- **ws_client.c/h**: WebSocket client for daemon communication
  - JSON message protocol over existing WebUI port
  - Reconnection with exponential backoff
  - App-level ping/pong keep-alive (10s interval)
  - Music subscribe/control/library/queue messages

- **voice_processing.c/h**: Local voice pipeline
  - VAD → wake word → ASR → send text query
  - Receive streaming response → sentence buffer → TTS
  - Producer-consumer TTS playback queue (synthesize N+1 while playing N)

- **music_stream.c/h**: Dedicated Opus audio WebSocket
  - Separate connection for music audio streaming
  - Opus decode → ALSA playback
  - Goertzel FFT analysis on live audio for visualizer

- **sdl_ui.c/h**: Touchscreen UI coordinator (SDL2 + KMSDRM)
  - Animated orb visualizer with state-driven colors
  - Scrollable markdown transcript with word-wrap
  - Touch gesture system (swipe, tap, long-press)
  - Status bar icon pattern for feature access (see below)

- **ui_music.c/h**: Music control panel
  - Three-tab layout: Playing / Queue / Library
  - Transport controls, seek bar, FFT visualizer
  - Paginated library browsing with artist/album drill-down

- **backlight.c/h**: Display brightness control
  - sysfs backlight control for DSI displays
  - Software dimming fallback for HDMI displays

**Daemon side** (`src/webui/`):

- **webui_satellite.c**: Satellite message handlers
  - Registration, query routing, streaming response relay
  - Session management with UUID-based identification
  - Satellite auth whitelist for music WebSocket messages

**Shared** (`common/`):

- **common/src/asr/**: ASR engine abstraction (Whisper batch, Vosk streaming)
- **common/src/tts/**: Piper TTS with preprocessing and emoji stripping
- **common/src/vad/**: Silero VAD with ONNX runtime
- **common/src/logging/**: `DAWN_LOG_INFO/ERROR/WARNING` macros

#### DAP2 Protocol Messages

| Type | Direction | Purpose |
|------|-----------|---------|
| `satellite_register` | Satellite → Daemon | Registration with UUID, name, location |
| `satellite_register_ack` | Daemon → Satellite | Session ID, memory enabled flag |
| `satellite_query` | Satellite → Daemon | User's transcribed text |
| `stream_start` | Daemon → Satellite | Streaming response begins |
| `stream_delta` | Daemon → Satellite | Partial response text |
| `stream_end` | Daemon → Satellite | Response complete |
| `satellite_ping` | Satellite → Daemon | App-level keep-alive |
| `music_control` | Satellite → Daemon | Play/pause/stop/next/prev/seek |
| `music_library` | Satellite → Daemon | Browse artists/albums/tracks (paginated) |
| `music_state` | Daemon → Satellite | Playback state update |

#### Data Flow (Satellite Voice Command)

```
1. Satellite: Microphone → VAD → Wake Word Detection → ASR (Vosk streaming)
   ↓
2. satellite_query {text: "turn on the lights"} → WebSocket → Daemon
   ↓
3. Daemon: LLM processing → Tool execution → Streaming response
   ↓
4. stream_delta {delta: "I'll turn on"} → WebSocket → Satellite
   ↓
5. Satellite: Sentence buffer → TTS (Piper) → ALSA playback
   ↓
6. stream_end → Satellite marks response complete
```

#### Satellite UI Design Patterns

**Status Bar Icon Pattern**: Features are accessed via small icons in the
transcript status bar (top-right area), not via a dedicated quick actions panel.
This mirrors the WebUI's icon-bar approach.

- Icons sit inline in the existing status bar alongside WiFi and date/time
- Each icon is an 18x18 SDL primitive texture with a 48x48 hit area (Apple HIG
  minimum for touch)
- Default color: secondary text color; active state: cyan accent
- Rendering technique: build icon as a **white texture**, then tint at draw time
  via `SDL_SetTextureColorMod()` — one cached texture serves both idle and active
  color states without re-rendering
- Icons only appear when the feature is available (e.g., music icon requires
  Opus support). No placeholder or "coming soon" icons.
- Tapping an icon toggles its associated slide-in panel (e.g., music panel
  slides in from the right)

**Current icons**: Music (note glyph, toggles music panel)

**Panel system**: Two panel types remain:
- Settings panel: swipe down from top edge (hamburger indicator)
- Music panel: tap music icon in status bar (slides from right)

Swipe-up from the bottom edge is currently unassigned (reserved for future use).

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

- **music_db.c/h**: Music metadata database
  - SQLite-based cache for artist/title/album tags
  - Indexed search across metadata fields
  - Incremental scanning (only reparse changed files by mtime)
  - Automatic cleanup of deleted files

- **music_scanner.c/h**: Background music library scanner
  - Dedicated thread for non-blocking scans
  - Configurable scan interval (default: 60 minutes)
  - Manual rescan trigger via admin socket
  - Mutex/condvar synchronization for thread safety

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

### 7. WebUI Audio Subsystem

**Purpose**: Bidirectional audio streaming for WebUI browsers and Tier 2 (ESP32) satellites

This subsystem handles all server-side audio: decode incoming audio, run ASR, generate TTS, encode outgoing audio. The WebUI browser connects with **Opus** (48kHz); Tier 2 satellites connect with **raw PCM** (16kHz, skipping codec/resample). Both use the same binary WebSocket message types (0x01/0x02 audio in, 0x11/0x12 audio out) and the same worker thread pipeline in `webui_audio.c`. See [DAP2_DESIGN.md](docs/DAP2_DESIGN.md) Section 6.2 for the Tier 2 audio protocol details.

#### Architecture: **Opus Codec + WebCodecs API** (WebUI Browser Path)

The WebUI uses Opus audio compression for efficient bidirectional audio streaming between browser and server.

```
┌───────────────────────────────────────────────────────────────────────┐
│                        Browser (WebUI)                                 │
│                                                                        │
│  ┌───────────────┐    ┌───────────────┐    ┌───────────────┐          │
│  │ getUserMedia  │───>│ Opus Encoder  │───>│  WebSocket    │──────────┼──>
│  │ (48kHz input) │    │ (Web Worker)  │    │ Binary Send   │          │
│  └───────────────┘    └───────────────┘    └───────────────┘          │
│                                                                        │
│  ┌───────────────┐    ┌───────────────┐    ┌───────────────┐          │
│  │ AudioContext  │<───│ Opus Decoder  │<───│  WebSocket    │<─────────┼──
│  │ (48kHz output)│    │ (Web Worker)  │    │ Binary Recv   │          │
│  └───────────────┘    └───────────────┘    └───────────────┘          │
└───────────────────────────────────────────────────────────────────────┘
                                │
                                │ Opus frames (length-prefixed)
                                │
                                ▼
┌───────────────────────────────────────────────────────────────────────┐
│                        DAWN Server                                     │
│                                                                        │
│  ┌───────────────┐    ┌───────────────┐    ┌───────────────┐          │
│  │ Opus Decoder  │───>│  Resampler    │───>│     ASR       │          │
│  │ (libopus)     │    │ 48kHz → 16kHz │    │   (Whisper)   │          │
│  └───────────────┘    └───────────────┘    └───────────────┘          │
│                                                                        │
│  ┌───────────────┐    ┌───────────────┐    ┌───────────────┐          │
│  │ Opus Encoder  │<───│  Resampler    │<───│     TTS       │          │
│  │ (libopus)     │    │ 22kHz → 48kHz │    │   (Piper)     │          │
│  └───────────────┘    └───────────────┘    └───────────────┘          │
└───────────────────────────────────────────────────────────────────────┘
```

#### Key Components

- **opus-worker.js**: Web Worker for encoding/decoding using WebCodecs API
  - Encodes browser microphone input (48kHz) to Opus frames
  - Decodes server TTS audio (Opus frames) to PCM for playback
  - Falls back to raw PCM if WebCodecs unavailable

- **Codec Configuration**:
  - Sample rate: 48kHz (Opus native rate)
  - Channels: Mono
  - Bitrate: Adaptive (typically 24-32 kbps for voice)
  - Frame size: 20ms (960 samples at 48kHz)

- **Capability Negotiation**:
  - Browser sends `audio_codecs: ["opus", "pcm"]` during WebSocket connect
  - Server selects best available codec
  - Graceful fallback to uncompressed PCM if Opus unavailable

#### Data Flow (Browser Voice Input)

```
1. getUserMedia() captures audio at 48kHz
   ↓
2. AudioWorklet sends PCM frames to Opus worker
   ↓
3. Worker encodes to Opus frames (length-prefixed)
   ↓
4. WebSocket sends binary data to server
   ↓
5. Server decodes Opus → resamples to 16kHz → ASR
   ↓
6. LLM processing → TTS generation
   ↓
7. Server encodes TTS audio to Opus → sends to browser
   ↓
8. Worker decodes Opus → AudioContext plays at 48kHz
```

#### Benefits of Opus Streaming

| Metric | Raw PCM (16-bit) | Opus Compressed |
|--------|------------------|-----------------|
| Bandwidth (1s audio) | ~192 KB | ~3-4 KB |
| Latency | Minimal | +2-5ms encoding |
| Quality | Lossless | Near-lossless (voice optimized) |
| Browser Support | Universal | WebCodecs (Chrome/Edge/Firefox 90+) |

### 8. Vision/Image Subsystem (`src/image_store.c`, `src/webui/webui_images.c`, `www/js/ui/vision.js`)

**Purpose**: Image upload, storage, and vision AI integration for the WebUI

#### Architecture: **Client-Side Compression + Server-Side BLOB Storage**

```
┌───────────────────────────────────────────────────────────────────────┐
│                        Browser (WebUI)                                 │
│                                                                        │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │                    Input Methods                                 │  │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐        │  │
│  │  │  File    │  │  Paste   │  │  Drag &  │  │  Camera  │        │  │
│  │  │  Upload  │  │  (Ctrl+V)│  │  Drop    │  │  Capture │        │  │
│  │  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘        │  │
│  │       └─────────────┴─────────────┴─────────────┘               │  │
│  │                           │                                      │  │
│  │                    ┌──────▼──────┐                               │  │
│  │                    │ Compression │ (max 1024px, JPEG 85%)        │  │
│  │                    └──────┬──────┘                               │  │
│  └───────────────────────────┼─────────────────────────────────────┘  │
│                              │                                        │
│  ┌───────────────────────────▼─────────────────────────────────────┐  │
│  │ POST /api/images (multipart/form-data)                          │  │
│  └───────────────────────────┬─────────────────────────────────────┘  │
└──────────────────────────────┼────────────────────────────────────────┘
                               │
                               ▼
┌───────────────────────────────────────────────────────────────────────┐
│                        DAWN Server                                     │
│                                                                        │
│  ┌───────────────────────────────────────────────────────────────┐    │
│  │ webui_images.c - HTTP Endpoint Handler                        │    │
│  │                                                               │    │
│  │  POST /api/images  → Validate → Store → Return image ID       │    │
│  │  GET /api/images/:id → Retrieve BLOB → Return image data      │    │
│  └───────────────────────────┬───────────────────────────────────┘    │
│                              │                                        │
│  ┌───────────────────────────▼───────────────────────────────────┐    │
│  │ image_store.c - SQLite BLOB Storage                           │    │
│  │                                                               │    │
│  │  Table: images (id, user_id, mime_type, data BLOB, created)   │    │
│  │  - Thread-safe via auth_db mutex                              │    │
│  │  - Automatic cleanup of old images (retention policy)         │    │
│  │  - Per-user image limits                                      │    │
│  └───────────────────────────────────────────────────────────────┘    │
│                                                                        │
│  ┌───────────────────────────────────────────────────────────────┐    │
│  │ LLM Vision Integration                                        │    │
│  │                                                               │    │
│  │  llm_openai.c: data:image/jpeg;base64,... format              │    │
│  │  llm_claude.c: source.type="base64", source.media_type=...    │    │
│  └───────────────────────────────────────────────────────────────┘    │
└───────────────────────────────────────────────────────────────────────┘
```

#### Key Components

- **vision.js**: Client-side image handling (1,400+ lines)
  - Input methods: file picker, clipboard paste, drag-and-drop, camera capture
  - Camera API with front/rear switching (`navigator.mediaDevices.getUserMedia`)
  - Client-side compression via Canvas API (max 1024px longest edge, JPEG 85%)
  - Multi-image support (up to 5 images per message)
  - LocalStorage caching of uploaded images by ID
  - Security: SVG explicitly excluded to prevent XSS attacks
  - Accessibility: ARIA announcements, keyboard navigation

- **image_store.c/h**: Server-side image storage
  - SQLite BLOB storage (uses auth_db for thread safety)
  - Image ID format: `img_` + 12 alphanumeric characters
  - Configurable limits: max size, max per user, retention days
  - Automatic cleanup of expired images

- **webui_images.c/h**: HTTP endpoint handlers
  - `POST /api/images`: Upload image, returns `{id, mime_type, size}`
  - `GET /api/images/:id`: Retrieve image by ID
  - Authentication required (uses session validation)

#### Data Flow (Image Upload)

```
1. User selects/pastes/drops/captures image
   ↓
2. Browser validates type (JPEG, PNG, GIF, WebP - NO SVG)
   ↓
3. Canvas API compresses to max 1024px, JPEG 85%
   ↓
4. POST /api/images with multipart form data
   ↓
5. Server validates auth, stores BLOB in SQLite
   ↓
6. Server returns image ID (e.g., "img_a1b2c3d4e5f6")
   ↓
7. Browser caches in localStorage, shows preview
   ↓
8. On send: Full base64 data sent to LLM with message
   ↓
9. History stores image ID reference (not inline data)
```

#### Vision Model Support

The system auto-detects vision capability based on model name:

| Model Pattern | Provider | Vision Support |
|---------------|----------|----------------|
| `gpt-4o`, `gpt-4-vision`, `gpt-4-turbo` | OpenAI | Yes |
| `claude-3-*` | Anthropic | Yes |
| `gemini-*` | Google | Yes |
| `llava-*`, `qwen-vl-*`, `cogvlm-*` | Local | Yes |
| Other models | Various | No (button disabled) |

#### Security Measures

- **SVG Exclusion**: SVG files explicitly blocked to prevent XSS via embedded scripts
- **Data URI Validation**: Only `data:image/{jpeg,png,gif,webp};base64,` prefixes accepted
- **Base64 Character Validation**: Only `[A-Za-z0-9+/=]` allowed in base64 portion
- **Authentication Required**: All image endpoints require valid session
- **Per-User Limits**: Configurable maximum images per user

### 9. Memory Subsystem (`src/memory/`, `include/memory/`)

**Purpose**: Persistent memory system for user facts, preferences, and conversation summaries

#### Architecture: **Sleep Consolidation Model**

Memory extraction happens at session end, not during conversation. This adds zero latency to conversations while building a persistent user profile.

```
┌───────────────────────────────────────────────────────────────────────┐
│                    DURING CONVERSATION                                 │
├───────────────────────────────────────────────────────────────────────┤
│  • Full conversation in LLM context window                            │
│  • Core facts + preferences pre-loaded at session start               │
│  • Memory tool available for explicit remember/search/forget          │
│  • Zero extraction overhead                                           │
└───────────────────────────────────────────────────────────────────────┘
                                │
                                │ Session ends (WebSocket disconnect/timeout)
                                ▼
┌───────────────────────────────────────────────────────────────────────┐
│                    SESSION END EXTRACTION                              │
├───────────────────────────────────────────────────────────────────────┤
│  • Load conversation messages from database                           │
│  • Build extraction prompt with transcript + existing profile         │
│  • Call extraction LLM (can differ from conversation model)           │
│  • Parse JSON: facts, preferences, corrections, summary               │
│  • Store in SQLite (skip if conversation marked private)              │
│  • Runs in background thread (non-blocking)                           │
└───────────────────────────────────────────────────────────────────────┘
```

#### Key Components

- **memory_types.h**: Data structures (`memory_fact_t`, `memory_preference_t`, `memory_summary_t`)

- **memory_db.c/h**: SQLite CRUD operations
  - Prepared statements for all memory tables
  - Similarity detection for duplicate prevention
  - Access counting for confidence reinforcement

- **memory_context.c/h**: Session start context builder
  - `memory_build_context()` builds ~800 token block
  - Loads preferences, top facts by confidence, recent summaries
  - Injected into LLM system prompt

- **memory_extraction.c/h**: Session end extraction
  - Triggered via `memory_trigger_extraction()`
  - Spawns background thread for non-blocking extraction
  - Parses LLM JSON response, stores facts/preferences/summaries
  - Respects conversation privacy flag

- **memory_callback.c**: Tool handler for `MEMORY` device type
  - `search`: Keyword search across all memory tables
  - `recent`: Time-based retrieval (e.g., "24h", "7d", "1w")
  - `remember`: Immediate fact storage with guardrails
  - `forget`: Delete matching facts

#### Database Schema

Three tables in the auth database (`/var/lib/dawn/auth.db`):

```sql
-- Facts: discrete pieces of information
memory_facts (id, user_id, fact_text, confidence, source, created_at,
              last_accessed, access_count, superseded_by)

-- Preferences: communication style preferences
memory_preferences (id, user_id, category, value, confidence, source,
                    created_at, updated_at, reinforcement_count)

-- Summaries: conversation digests
memory_summaries (id, user_id, session_id, summary, topics, sentiment,
                  created_at, message_count, duration_seconds, consolidated)
```

#### Privacy Toggle

Users can mark conversations as private to skip memory extraction:

- `is_private` column in `conversations` table
- Set via WebSocket message or Ctrl+Shift+P keyboard shortcut
- Can be set before conversation starts (pending state)
- Visual badge in conversation history list

#### Security Guardrails

Memory content flows into future prompts, creating potential attack vectors:

```c
// Blocked patterns (hardcoded in memory_callback.c)
const char *MEMORY_BLOCKED_PATTERNS[] = {
   "whenever", "always", "you should", "you must",
   "ignore", "forget", "disregard", "pretend",
   "act as if", "system prompt", "instructions",
   "password", "api key", "token", "secret",
   NULL
};
```

#### Configuration

```toml
[memory]
enabled = true
context_budget_tokens = 800
session_timeout_minutes = 15

[memory.extraction]
provider = "local"        # "local", "openai", "claude", "ollama"
model = "qwen2.5:7b"      # Model for extraction
```

#### Data Flow (Memory Lifecycle)

```
1. Session Start (WebSocket connect)
   ↓
2. Load user profile: memory_build_context(user_id)
   ↓
3. Inject facts/preferences into LLM system prompt
   ↓
4. During conversation:
   - User: "Remember I'm vegetarian" → memory_remember() → immediate storage
   - User: "What do you know about me?" → memory_search() → return facts
   ↓
5. Session End (WebSocket disconnect/timeout)
   ↓
6. Check privacy flag: if private, skip extraction
   ↓
7. memory_trigger_extraction() → background thread
   ↓
8. Load conversation, build extraction prompt
   ↓
9. Call extraction LLM, parse JSON response
   ↓
10. Store new facts, update preferences, save summary
```

---

## Module Dependency Hierarchy

To prevent circular dependencies and maintain clean architecture, modules are organized into layers. **Modules may only depend on modules in lower layers.**

```
Layer 0 (Foundation)
├── logging.c/h           - Logging macros (no dependencies)
├── config/               - Configuration parsing and defaults
│   ├── dawn_config.h
│   ├── config_parser.c
│   └── config_defaults.c
└── tools/string_utils.c  - String helpers

Layer 1 (Core Infrastructure)
├── tool_registry.c/h     - Tool registration and lookup (depends: logging, config)
├── command_router.c/h    - Request/response routing (depends: logging)
├── session_manager.c/h   - Session lifecycle (depends: logging, config)
└── input_queue.c/h       - Thread-safe input queue (depends: logging)

Layer 2 (Services)
├── llm/                  - LLM providers and tools
│   ├── llm_interface.c   - Provider abstraction (depends: Layer 0-1)
│   ├── llm_openai.c      - OpenAI/Ollama/llama.cpp (depends: llm_interface)
│   ├── llm_claude.c      - Anthropic Claude (depends: llm_interface)
│   └── llm_tools.c       - Tool execution (depends: tool_registry)
├── tts/                  - Text-to-speech (depends: Layer 0-1)
├── asr/                  - Speech recognition (depends: Layer 0-1)
├── mosquitto_comms.c     - MQTT integration (depends: Layer 0-1, tool_registry)
└── memory/               - Persistent memory (depends: Layer 0-1)

Layer 3 (Tools)
├── tools/weather_tool.c  - Weather API (depends: Layer 0-2)
├── tools/music_tool.c    - Music playback (depends: Layer 0-2)
├── tools/search_tool.c   - Web search (depends: Layer 0-2)
├── tools/memory_tool.c   - Memory commands (depends: Layer 0-2, memory/)
└── tools/*.c             - All other tools (depends: Layer 0-2)

Layer 4 (Application)
├── dawn.c                - Main entry point (depends: all layers)
├── worker_pool.c         - Concurrent processing (depends: Layer 0-3)
├── state_machine.c       - Voice state machine (depends: Layer 0-3)
└── webui/                - Web interface (depends: Layer 0-3)
```

### Dependency Rules

1. **Downward only**: A module may only `#include` headers from its own layer or lower layers
2. **No cycles**: If A depends on B, B must not depend on A (directly or transitively)
3. **Interface segregation**: Use forward declarations and callbacks to break potential cycles
4. **Same-layer allowed**: Modules in the same layer may depend on each other if acyclic

### Common Patterns to Avoid Cycles

**Callback registration** (Layer 2 → Layer 3 without direct dependency):
```c
// In tool_registry.h (Layer 1)
typedef char *(*tool_callback_t)(const char *action, char *value, int *should_respond);

// In weather_tool.c (Layer 3) - registers callback at init
tool_registry_register(&weather_metadata);  // Passes function pointer up
```

**Forward declarations** (when header inclusion would create cycle):
```c
// In llm_tools.h - avoid including full tool_registry.h
struct tool_metadata;  // Forward declaration
```

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

### Remote Voice Command Flow (WebSocket Clients)

```
1. Client connects via WebSocket (WebUI browser, Tier 1 or Tier 2 satellite)
   ↓
2. Session registration (satellite_register with capabilities)
   ↓
   ┌─────────── Tier 1 (Text Path) ────────────┐  ┌──── Tier 2 / WebUI (Audio Path) ────┐
   │ 3a. Local ASR on satellite                 │  │ 3b. Binary audio frames (0x01)       │
   │ 4a. satellite_query {text: "..."}          │  │ 4b. Audio end marker (0x02)          │
   │                                            │  │ 5b. Server-side ASR (Whisper/Vosk)   │
   └────────────────────┬───────────────────────┘  └──────────────────┬───────────────────┘
                        │                                             │
                        └──────────────┬──────────────────────────────┘
                                       ↓
   6. LLM Processing (same as local flow)
      ↓
   7. Streaming response (stream_start → stream_delta → stream_end)
      ↓
   ┌─────────── Tier 1 (Text Path) ────────────┐  ┌──── Tier 2 / WebUI (Audio Path) ────┐
   │ 8a. Satellite receives text deltas         │  │ 8b. Server-side TTS (Piper)          │
   │ 9a. Local TTS synthesis (Piper)            │  │ 9b. Binary audio frames (0x11/0x12)  │
   │ 10a. Local playback                        │  │ 10b. Client playback                 │
   └────────────────────────────────────────────┘  └──────────────────────────────────────┘
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

> **Note**: DAP1 (the original TCP binary protocol for ESP32 clients) has been **eliminated**. All client communication — WebUI browsers, Tier 1 satellites (Raspberry Pi), and Tier 2 satellites (ESP32) — now uses **WebSocket** via the unified WebUI server on port 3000. The DAP1 packet format below is retained for historical reference only. See [DAP2 Satellite Protocol](#dap2-satellite-protocol) for the current design.

<details>
<summary>DAP1 Packet Format (Historical Reference — Eliminated)</summary>

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

</details>

---

## DAP2 Satellite Protocol

**Status**: ✅ **Implemented** (January–February 2026)

DAP2 is the unified WebSocket protocol for all satellite devices. Every satellite connects to the same WebSocket port as the WebUI (default 3000) and registers with its capabilities. The daemon uses **capability-based routing** to choose the appropriate path:

| Tier | Hardware | What it sends | Server does | Use Case |
|------|----------|---------------|-------------|----------|
| **Tier 1** | RPi 4/5 | JSON text (`satellite_query`) | LLM only | Hands-free (local ASR/TTS) |
| **Tier 2** | ESP32-S3 | Binary PCM audio (0x01/0x02) | ASR + LLM + TTS | Push-to-talk (server ASR/TTS) |

**Tier 2 audio reuses the WebUI audio subsystem** ([Section 7](#7-webui-audio-subsystem)) — same binary message types, same `webui_audio.c` worker threads, same ASR→LLM→TTS pipeline. The only difference is raw PCM at 16kHz instead of Opus at 48kHz (no codec or resample step on server). Music streaming (Opus over a dedicated WebSocket) is Tier 1 only.

For connection lifecycle diagrams, message format details, and the complete protocol specification, see:
- **[DAP2_DESIGN.md](docs/DAP2_DESIGN.md)** — Protocol spec (both tiers, audio framing, registration, codec details)
- **[DAP2_SATELLITE.md](docs/DAP2_SATELLITE.md)** — Tier 1 implementation guide (build, config, deployment)

---

## Command Processing Architecture

DAWN supports three parallel command processing paths that all converge on a unified executor.

### Tool Registry System

The **tool_registry** (`src/tools/tool_registry.c`) is the primary mechanism for registering modular tools. Each tool is a self-contained module with its own metadata, parameters, and callback:

```c
static const tool_metadata_t my_tool_metadata = {
   .name = "my_tool",              // API name for LLM tool calls
   .device_string = "my device",   // Internal device identifier
   .description = "Tool description for LLM schema",
   .params = my_tool_params,       // Parameter definitions
   .param_count = 2,
   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = TOOL_CAP_NETWORK,
   .is_getter = true,
   .default_remote = true,
   .callback = my_tool_callback,
};
```

**Key Features:**
- **O(1) Lookup**: FNV-1a hash tables for name, device_string, and aliases
- **Self-Registration**: Each tool calls `tool_registry_register()` during init
- **LLM Schema Generation**: `tool_registry_generate_llm_tools()` builds provider-specific schemas
- **Capability Flags**: `TOOL_CAP_NETWORK`, `TOOL_CAP_DANGEROUS`, etc. for safety classification

**Registered Tools** (as of January 2026):
- audio_tools, calculator_tool, datetime_tool, hud_tools, llm_status_tool
- memory_tool, music_tool, reset_conversation_tool, search_tool, shutdown_tool
- smartthings_tool, switch_llm_tool, url_tool, viewing_tool, volume_tool, weather_tool

### Command Flow

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           USER INPUT (Voice/Text)                           │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                      PROCESSING MODE (dawn.toml: commands.processing_mode)  │
│                                                                             │
│   direct_only ──────► Pattern match only, no LLM                            │
│   llm_only ─────────► Send everything to LLM                                │
│   direct_first ─────► Try patterns, fallback to LLM                         │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                    ┌─────────────────┴─────────────────┐
                    ▼                                   ▼
┌──────────────────────────────┐       ┌──────────────────────────────────────┐
│   PATH 1: DIRECT MATCHING    │       │         PATH 2: LLM INVOCATION       │
│   (text_to_command_nuevo.c)  │       │                                      │
│                              │       │    ┌────────────────────────────┐    │
│  Regex patterns from JSON:   │       │    │  native_enabled = true?    │    │
│  "turn on %device_name%"     │       │    └────────────┬───────────────┘    │
│  "play %value%"              │       │                 │                    │
│                              │       │     ┌───────────┴───────────┐        │
│  Extracts device/action/val  │       │     ▼                       ▼        │
└──────────────┬───────────────┘       │ ┌───────────┐       ┌─────────────┐  │
               │                       │ │PATH 2A:   │       │PATH 2B:     │  │
               │                       │ │NATIVE     │       │LEGACY       │  │
               │                       │ │TOOLS      │       │<command>    │  │
               │                       │ │           │       │TAGS         │  │
               │                       │ │LLM returns│       │             │  │
               │                       │ │structured │       │LLM returns  │  │
               │                       │ │tool_calls │       │<command>JSON│  │
               │                       │ └─────┬─────┘       └──────┬──────┘  │
               │                       │       │                    │         │
               │                       └───────┼────────────────────┼─────────┘
               │                               │                    │
               │                               ▼                    ▼
               │                       ┌─────────────┐      ┌─────────────────┐
               │                       │llm_tools_   │      │webui_process_   │
               │                       │execute()    │      │commands()       │
               │                       │             │      │                 │
               │                       │Parses tool  │      │Parses <command> │
               │                       │call struct  │      │tags from text   │
               │                       └─────┬───────┘      └────────┬────────┘
               │                             │                       │
               └─────────────────────────────┼───────────────────────┘
                                             │
                                             ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                    UNIFIED COMMAND EXECUTOR (command_executor.c)            │
│                                                                             │
│   command_execute(device, action, value, mosq, &result)                     │
│                                                                             │
│   1. Look up device in command_registry                                     │
│   2. If has_callback → invoke deviceCallbackArray[type].callback()          │
│   3. If mqtt_only → publish JSON to MQTT topic                              │
│   4. If sync_wait → use command_router for response (viewing)               │
└─────────────────────────────────────────────────────────────────────────────┘
                                             │
               ┌─────────────────────────────┼─────────────────────────────┐
               ▼                             ▼                             ▼
┌──────────────────────┐       ┌──────────────────────┐       ┌─────────────────┐
│ C CALLBACKS          │       │ MQTT-ONLY            │       │ SYNC WAIT       │
│ (mosquitto_comms.c)  │       │ (Hardware)           │       │ (viewing)       │
│                      │       │                      │       │                 │
│ deviceCallbackArray: │       │ Publish to topic:    │       │ Wait for MQTT   │
│ - weather → get_wea  │       │ - "hud" → helmet     │       │ response via    │
│ - music → play_music │       │ - "helmet" → helmet  │       │ command_router  │
│ - search → web_sear  │       │ - "smartthings"      │       │                 │
│ - date → get_date    │       │                      │       │                 │
└──────────────────────┘       └──────────────────────┘       └─────────────────┘
```

### Command Definition Sources

Commands are defined in two places:

1. **JSON-Defined** (`commands_config_nuevo.json`)
   - Device definitions with types, aliases, MQTT topics
   - Optional `tool` blocks that become native LLM tools
   - Action patterns for direct matching ("turn on %device_name%")

2. **C-Defined** (`mosquitto_comms.c`)
   - `deviceCallbackArray[]` maps device types to C functions
   - Execution callbacks: weather, music, search, smartthings, etc.

### Native Tools vs Legacy `<command>` Tags

| Aspect | Native Tools | Legacy `<command>` Tags |
|--------|--------------|------------------------|
| **Definition** | command_registry → llm_tools | Raw JSON in prompt |
| **Prompt** | Minimal (tools sent as API params) | Full `<command>` instructions |
| **Response** | Structured `tool_calls` array | Text with `<command>JSON</command>` |
| **Filtering** | `enabled_local`/`enabled_remote` per tool | Same flags |
| **Execution** | `command_execute()` | `command_execute()` |

### Tool Enable/Disable

Tools can be enabled/disabled per session type (local vs remote):
- Settings UI provides per-tool toggles
- Legacy `<command>` prompt is filtered by the same enabled flags
- Disabled tools are omitted from both native tool schemas and legacy prompt

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
   if (config.llm.type == LLM_TYPE_CLOUD) {
      if (config.llm.cloud.provider == PROVIDER_OPENAI) {
         return llm_openai_init();  // OpenAI models (GPT-4o, etc.)
      } else if (config.llm.cloud.provider == PROVIDER_GEMINI) {
         return llm_openai_init();  // Gemini via OpenAI-compatible endpoint
      } else {
         return llm_claude_init();  // Claude models
      }
   } else {
      // Local: Both llama.cpp and Ollama use OpenAI-compatible endpoint
      return llm_openai_init();
   }
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

## File Organization Standards

### Size Limits

To prevent files from becoming unmaintainable monoliths, follow these limits:

| File Type | Soft Limit | Hard Limit |
|-----------|------------|------------|
| C source | 1,500 lines | 2,500 lines |
| JavaScript | 1,000 lines | 1,500 lines |
| CSS | 1,000 lines | 2,000 lines |

### Module Split Pattern (C)

When a C file exceeds limits, split by feature using an internal header:

```
src/subsystem/
├── subsystem_core.c       # Init, shutdown, shared state
├── subsystem_feature1.c   # Feature area 1
├── subsystem_feature2.c   # Feature area 2
└── ...

include/subsystem/
├── subsystem.h            # Public API (unchanged)
└── subsystem_internal.h   # Shared state, internal helpers
```

The internal header contains:
- `extern` declarations for shared state (defined in `_core.c`)
- Internal helper function declarations
- Shared macros (e.g., locking patterns)

### When Adding New Features

1. **Check file size first** - If target file > 1,500 lines, consider creating a new file
2. **Group by feature** - Related functionality goes together in one module
3. **Use internal headers** - Share state via `*_internal.h` pattern
4. **Update build system** - Add new source files immediately

---

## Configuration Architecture

### Design Principles

1. **Config Files as Source of Truth**: All DAWN application settings are defined in `dawn.toml` (runtime configuration) or `secrets.toml` (credentials and API keys). The SQLite database is reserved for:
   - User authentication and sessions
   - Conversation history
   - Uploaded images
   - Other user-generated content

   **Settings are NEVER stored in the database.** This separation ensures configuration is portable, version-controllable (minus secrets), and easily inspectable.

2. **WebUI Settings Exposure**: All settings defined in `dawn.toml` are exposed in the WebUI settings panel unless explicitly excluded. Exclusions are limited to:
   - File system paths (security risk if editable remotely)
   - Internal debugging flags
   - Settings that require restart and have no runtime effect

3. **Secrets Isolation**: Credentials (`secrets.toml`) are kept separate from general configuration (`dawn.toml`) to allow:
   - Sharing `dawn.toml` without exposing API keys
   - Different secrets per deployment (dev/staging/prod)
   - Credential rotation without touching main config

4. **Compile-Time vs Runtime**: `dawn.h` provides compile-time defaults only. All user-configurable settings should be in TOML files, with `dawn.h` values serving as fallbacks when config is missing.

### Configuration File Hierarchy

```
~/.config/dawn/          # User-specific (highest priority)
├── dawn.toml
└── secrets.toml

./                       # Project root (fallback)
├── dawn.toml
└── secrets.toml

/etc/dawn/               # System-wide (lowest priority, future)
├── dawn.toml
└── secrets.toml
```

Settings are merged with higher-priority files overriding lower ones.

### Configuration Files

#### dawn.toml (Runtime Configuration)

Primary configuration file with sections for each subsystem:

```toml
[general]
ai_name = "friday"
timezone = "America/New_York"

[llm]
type = "cloud"                    # "cloud" or "local"

[llm.cloud]
provider = "openai"               # "openai", "anthropic", "gemini"
model = "gpt-4o"

[llm.local]
endpoint = "http://localhost:8080"
model = "qwen3-4b"

[asr]
model_path = "models/whisper.cpp/ggml-base.en.bin"
language = "en"

[tts]
model_path = "models/en_GB-alba-medium.onnx"
sample_rate = 22050

[webui]
bind_address = "0.0.0.0"
port = 3000
```

#### secrets.toml (Credentials)

API keys and sensitive credentials (gitignored):

```toml
openai_api_key = "sk-..."
claude_api_key = "sk-ant-..."
gemini_api_key = "..."

[smartthings]
access_token = "..."
```

**Note**: Already in `.gitignore` - never commit API keys!

#### dawn.h (Compile-Time Defaults)

Fallback values when config files are missing:

- `AI_NAME`: Default wake word ("friday")
- `AI_DESCRIPTION`: System prompt for LLM
- `DEFAULT_PCM_PLAYBACK_DEVICE`: ALSA playback device
- `DEFAULT_PCM_CAPTURE_DEVICE`: ALSA capture device
- `MQTT_IP` / `MQTT_PORT`: MQTT broker defaults

#### commands_config_nuevo.json (Device/Tool Definitions)

Device types, actions, and LLM tool definitions:

```json
{
  "devices": [
    {
      "type": "light",
      "name": "living_room",
      "actions": ["on", "off", "dim"],
      "tool": {
        "description": "Control living room light",
        "parameters": [...]
      }
    }
  ]
}
```

### WebUI Settings Panel Mapping

The WebUI settings panel (`www/js/ui/settings.js`) defines a `SETTINGS_SCHEMA` that maps to `dawn.toml` sections:

| WebUI Section | Config Section | Notes |
|---------------|----------------|-------|
| Language Model | `[llm]`, `[llm.cloud]`, `[llm.local]` | Provider, model selection |
| Speech Recognition | `[asr]` | Model, language |
| Text-to-Speech | `[tts]` | Voice model, rate |
| Audio | `[audio]` | Backend, devices |
| Tool Calling | `[llm.tools]` | Mode, per-tool toggles |
| Network | `[webui]`, `[dap]`, `[mqtt]` | Ports, addresses |

**Implementation Note**: When adding new settings to `dawn.toml`, also add corresponding entries to `SETTINGS_SCHEMA` to expose them in the WebUI, unless they fall under the exclusion criteria above.

---

## Future Improvements

### Completed

1. ✅ **Multi-Client Network Server** — Worker thread pool, per-client sessions, non-blocking main loop
2. ✅ **Conversation Persistence** — SQLite-backed conversation history with search/rename/delete
3. ✅ **DAP2 Satellite System** — Text-first WebSocket protocol, Tier 1 RPi satellite with SDL2 UI
4. ✅ **Music Streaming to Satellites** — Opus audio over dedicated WebSocket, Goertzel FFT visualizer

### Planned Features

1. **Improved Error Recovery**
   - Auto-reconnect for MQTT
   - Network protocol timeout tuning
   - ASR fallback on GPU failure

2. **CI/CD Pipeline**
   - GitHub Actions for automated builds
   - Regression testing
   - Code quality checks

3. **Enhanced Testing**
   - Integration tests (end-to-end)
   - Stress tests (concurrent clients, memory leaks)
   - Automated regression tests

4. **DAP Device Token Authentication** (Phase 4.3)
   - Certificate-based satellite authentication
   - Replace session-based auth for headless devices

---

## References

- **Piper TTS**: https://github.com/rhasspy/piper
- **Vosk ASR**: https://alphacephei.com/vosk/
- **Whisper**: https://github.com/ggerganov/whisper.cpp
- **Silero VAD**: https://github.com/snakers4/silero-vad
- **llama.cpp**: https://github.com/ggerganov/llama.cpp
- **ONNX Runtime**: https://github.com/microsoft/onnxruntime

---

**Document Version**: 1.8
**Last Updated**: February 11, 2026 (DAP1 eliminated, unified DAP2 WebSocket for all satellites)
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

