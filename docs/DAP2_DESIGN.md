# Dawn Audio Protocol 2.0 (DAP 2) Design Document

**Status**: Implementation In Progress (Phase 0-3 Complete, Phase 3.5 In Progress)
**Version**: 0.9
**Date**: February 2026

## Executive Summary

DAP 2.0 represents a fundamental architectural shift from the "dumb satellite" model of DAP 1 to a "smart satellite" model where satellites run significant portions of the DAWN pipeline locally. This enables:

- **Seamless UX**: Local wake word detection, VAD, and TTS playback
- **Reduced latency**: Only LLM queries go to the central daemon
- **Offline resilience**: Basic functionality without network
- **Scalability**: Central daemon only handles LLM orchestration

**Key Architecture Decision**: All satellite tiers use **WebSocket** transport on the same port as the WebUI. The WebUI has already solved multi-client support, session management, streaming, reconnection, and bidirectional audio. Tier 1 (RPi) sends text (local ASR/TTS). Tier 2 (ESP32) sends PCM audio using the same binary message types as the WebUI (server-side ASR/TTS). DAP1 is fully eliminated — no custom binary protocol, no separate TCP server.

---

## Table of Contents

1. [Problem Statement](#1-problem-statement)
2. [Architecture Comparison](#2-architecture-comparison)
3. [WebUI Architecture Leverage](#3-webui-architecture-leverage)
4. [DAP 2.0 Architecture](#4-dap-20-architecture)
5. [Satellite Tiers](#5-satellite-tiers)
6. [Protocol Specification](#6-protocol-specification)
7. [Common Code Architecture](#7-common-code-architecture)
8. [Implementation Phases](#8-implementation-phases)
9. [Platform Considerations](#9-platform-considerations)
10. [Security Architecture](#10-security-architecture)
11. [Network Reliability](#11-network-reliability)
12. [Performance Validation](#12-performance-validation)
13. [Memory System Integration](#13-memory-system-integration)
14. [Open Questions](#14-open-questions)

---

## 1. Problem Statement

### DAP 1.0 Limitations

| Issue | Impact |
|-------|--------|
| **Audio round-trip** | User speaks → audio sent to server → ASR → LLM → TTS → audio sent back. High latency (~5-15s total). |
| **No local wake word** | Cannot activate hands-free; requires button press on ESP32. |
| **No local VAD** | Can't detect end of speech locally; relies on button release or timeout. |
| **Large audio payloads** | 30 seconds of 16kHz audio = ~960KB per interaction. |
| **Server bottleneck** | Central daemon blocks on each client during processing. |
| **No offline mode** | Satellite is useless without network connection. |

### DAP 2.0 Goals

1. **Local wake word detection** - Satellite continuously listens, activates on "Friday"
2. **Local VAD** - Satellite detects speech end, sends only transcribed text OR minimal audio
3. **Local TTS** - Satellite renders speech from text, no audio download
4. **Lightweight protocol** - Text-based messages, not audio streams
5. **Conversation continuity** - Central daemon maintains conversation history per satellite
6. **Graceful degradation** - Offline mode with local-only capabilities

---

## 2. Architecture Comparison

### DAP 1.0 Architecture (Current)

```
┌─────────────────────────────────────────────────────────────────────┐
│                          ESP32 Satellite                             │
│                                                                      │
│  ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐      │
│  │  Button  │───>│  Record  │───>│  Encode  │───>│   Send   │      │
│  │  Press   │    │  Audio   │    │   WAV    │    │  to DAP  │      │
│  └──────────┘    └──────────┘    └──────────┘    └──────────┘      │
│                                                                      │
│  ┌──────────┐    ┌──────────┐                                       │
│  │  Play    │<───│  Receive │<─────────────────── (large WAV)       │
│  │  Audio   │    │   WAV    │                                       │
│  └──────────┘    └──────────┘                                       │
└─────────────────────────────────────────────────────────────────────┘
         │                                      ▲
         │ ~960KB audio upload                  │ ~1-5MB audio download
         ▼                                      │
┌─────────────────────────────────────────────────────────────────────┐
│                        DAWN Central Daemon                           │
│                                                                      │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐   │
│  │  DAP    │─>│  ASR    │─>│  LLM    │─>│  TTS    │─>│  DAP    │   │
│  │ Receive │  │(Whisper)│  │(OpenAI) │  │ (Piper) │  │  Send   │   │
│  └─────────┘  └─────────┘  └─────────┘  └─────────┘  └─────────┘   │
│                                                                      │
│                 BLOCKS entire main loop for 10-15s                   │
└─────────────────────────────────────────────────────────────────────┘
```

### DAP 2.0 Architecture (Revised)

```
┌─────────────────────────────────────────────────────────────────────┐
│                     DAWN Tier 1 Satellite (RPi)                      │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │                    Local Processing Pipeline                   │   │
│  │                                                                │   │
│  │  ┌────────┐  ┌────────┐  ┌────────┐  ┌────────┐              │   │
│  │  │ Audio  │─>│  VAD   │─>│ Wake   │─>│  ASR   │              │   │
│  │  │Capture │  │(Silero)│  │  Word  │  │(Whisper│              │   │
│  │  │        │  │        │  │ Detect │  │  tiny) │              │   │
│  │  └────────┘  └────────┘  └────────┘  └────────┘              │   │
│  │                                           │                    │   │
│  │                                           ▼ (text: "turn on   │   │
│  │                                              the lights")     │   │
│  │  ┌────────┐  ┌────────┐  ┌────────────────────────────┐      │   │
│  │  │ Audio  │<─│  TTS   │<─│    WebSocket Client        │      │   │
│  │  │Playback│  │(Piper) │  │ (reuses WebUI protocol)    │      │   │
│  │  └────────┘  └────────┘  └────────────────────────────┘      │   │
│  │                                                                │   │
│  └──────────────────────────────────────────────────────────────┘   │
│                                                                      │
│  Local-only fallback: "I can't reach the server right now"          │
└─────────────────────────────────────────────────────────────────────┘
         │                                      ▲
         │ ~100-500 bytes (JSON/text)           │ ~100-2000 bytes (JSON/text)
         ▼                                      │
┌─────────────────────────────────────────────────────────────────────┐
│                        DAWN Central Daemon                           │
│                                                                      │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │              WebUI Server (Extended for Satellites)          │    │
│  │                                                               │    │
│  │  ┌───────────┐  ┌───────────┐  ┌───────────┐                │    │
│  │  │  Session  │  │    LLM    │  │  Command  │                │    │
│  │  │  Manager  │─>│ Interface │─>│  Parser   │                │    │
│  │  │(extended) │  │           │  │           │                │    │
│  │  └───────────┘  └───────────┘  └───────────┘                │    │
│  │        │                             │                        │    │
│  │        ▼                             ▼                        │    │
│  │  ┌───────────┐               ┌───────────┐                   │    │
│  │  │   Conv.   │               │   MQTT    │                   │    │
│  │  │  History  │               │  Publish  │                   │    │
│  │  └───────────┘               └───────────┘                   │    │
│  │                                                               │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                      │
│  Thread-per-client - non-blocking (existing WebUI model)             │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 3. WebUI Architecture Leverage

### What WebUI Already Has

The WebUI has already solved many infrastructure problems. Tier 1 satellites should leverage this:

| Feature | WebUI Status | DAP2 Benefit |
|---------|--------------|--------------|
| Multi-client support | ✅ Implemented | Satellites are just another client type |
| Session management | ✅ JWT tokens, reconnection | Extend with SESSION_TYPE_SATELLITE |
| Opus codec | ✅ 48kHz, quality tiers | Available if audio streaming needed |
| Streaming protocol | ✅ Binary message types | Reuse for response streaming |
| Response queue pattern | ✅ Thread-safe | Already proven |
| Per-client state | ✅ Music has per-session state | Same pattern for satellites |
| Keepalive/heartbeat | ✅ WebSocket ping/pong | Built into WebSocket |
| TLS/SSL | ✅ Optional | Security built-in |

### Why WebSocket for Tier 1?

| Aspect | Custom Protocol | WebSocket |
|--------|----------------|-----------|
| Implementation | Write from scratch | libwebsockets (proven) |
| Framing | Manual | Built-in |
| Fragmentation | Manual | Built-in |
| TLS | Manual | Built-in |
| Debugging | Custom tools | Browser dev tools, wscat |
| Firewall | Port 5000 | Port 443 (HTTPS) |
| Reconnection | Manual | Library support |
| Existing code | None | WebUI infrastructure |

**Decision**: All tiers use WebSocket. ESP32 can run the `esp_websocket_client` library (built into ESP-IDF) with minimal overhead (~100-200KB flash). This eliminates DAP1 entirely and unifies all satellite communication on a single port.

### What DAP2 Adds Beyond WebUI

The **text-first protocol** for Tier 1 is the key differentiator:

```
WebUI (browser):
User speaks → Audio sent to server → ASR → LLM → TTS → Audio sent back

DAP2 Tier 1 (satellite):
User speaks → [LOCAL: VAD → Wake Word → ASR] → Text sent to daemon → LLM → Text returned → [LOCAL: TTS]
```

**Why text-first matters for JARVIS vision:**
- **Latency**: ~100 bytes vs ~960KB per interaction
- **Offline**: Satellite can say "I can't reach the server" locally
- **Wake word**: Always listening without streaming audio 24/7
- **Privacy**: Audio never leaves the room unless needed
- **Bandwidth**: Multiple satellites don't saturate WiFi

---

## 4. DAP 2.0 Architecture

### Core Principles

1. **Capability-Based Routing**: Daemon routes based on `local_asr`/`local_tts` flags — text path for Tier 1, audio path for Tier 2
2. **Local Intelligence Where Possible**: ASR, VAD, wake word, and TTS run locally on Tier 1; server handles these for Tier 2
3. **Unified WebSocket Transport**: All tiers use WebSocket on the WebUI port — no separate servers or protocols
4. **Unified Session Model**: Extend existing session_manager, not separate system
5. **Streaming Support**: LLM responses streamed as text (Tier 1) or sentence-level TTS audio (Tier 2)
6. **Fallback Modes**: Graceful degradation when network unavailable

### Unified Session Model

Instead of separate DAP2 session management, extend the existing session system:

```
                    ┌─────────────────────────┐
                    │    Session Manager      │
                    │  (conversation history) │
                    └───────────┬─────────────┘
                                │
        ┌───────────────────────┼───────────────────────┐
        │                       │                       │
        ▼                       ▼                       ▼
┌───────────────┐     ┌───────────────┐     ┌───────────────┐
│ Local Session │     │ WebUI Session │     │ DAP2 Session  │
│  (dawn.c)     │     │  (browser)    │     │  (satellite)  │
└───────────────┘     └───────────────┘     └───────────────┘
```

```c
// Extend include/core/session_manager.h
typedef enum {
   SESSION_TYPE_LOCAL,      // Main daemon (microphone)
   SESSION_TYPE_WEBUI,  // Browser WebUI
   SESSION_TYPE_SATELLITE,  // DAP2 satellite (Tier 1 or Tier 2)
} session_type_t;

typedef struct {
   session_type_t type;
   char uuid[64];           // Satellite UUID
   char name[64];           // "Kitchen Assistant"
   char location[32];       // "kitchen"
   int tier;                // 1 or 2
   bool local_asr;          // Tier 1: true, Tier 2: false
   bool local_tts;          // Tier 1: true, Tier 2: false
   // ... existing session fields
} session_t;
```

### Capability-Based Routing

The daemon routes messages based on satellite capabilities declared at registration:

```c
// Routing based on capabilities
if (session->local_asr) {
   // Tier 1: expect satellite_query with text
   // Response: stream_delta text messages
} else {
   // Tier 2: expect binary audio frames (0x01/0x02)
   // Response: binary TTS audio frames (0x11/0x12) — same as WebUI
}
```

Both tiers share the same WebSocket connection, response queue, and session management. The only difference is whether the payload is text (JSON) or audio (binary frames).

---

## 5. Satellite Tiers

DAP 2.0 supports **two satellite tiers** based on hardware capabilities:

### Tier 1: Full Satellite (Raspberry Pi 4/5, Orange Pi 5)

Runs the complete local pipeline - only LLM queries go to the daemon. **Follows the same interaction model as the DAWN daemon** for a seamless user experience.

**Interaction Model**: Hands-free, VAD-based (no push button, no indicator LEDs). Users interact via voice only, identical to the main daemon experience.

| Component | Implementation | Memory | Notes |
|-----------|---------------|--------|-------|
| Audio Capture | ALSA | ~1 MB | Continuous 16kHz capture |
| VAD | Silero ONNX | ~2 MB | Real-time speech detection |
| Wake Word | Whisper tiny | ~77 MB | **Same implementation as DAWN daemon** |
| ASR | Whisper tiny/base | ~77-140 MB | Command transcription |
| TTS | Piper | ~60 MB | Local speech synthesis |
| **Total** | | **~220-280 MB** | Fits in 2GB+ RAM |

**Wake Word Detection**: Identical to DAWN daemon implementation:
1. VAD (Silero) continuously monitors for speech
2. On speech detection, run Whisper on audio chunk
3. Check transcript for wake word ("Friday")
4. If found, transition to command recording state
5. Uses same state machine: `SILENCE → WAKEWORD_LISTEN → COMMAND_RECORDING → PROCESSING → SPEAKING`

**Capabilities**:
- Hands-free wake word activation ("Friday") - same feel as talking to main daemon
- Local ASR - sends **text** to daemon
- Local TTS - receives **text** from daemon
- Offline fallback: TTS "I can't reach the server right now"

**Communication**: WebSocket (reuses WebUI infrastructure)

```
User: "Friday, turn on the lights"
  │
  ▼ (local processing)
Satellite: VAD → Wake Word → ASR → "turn on the lights"
  │
  ▼ (WebSocket JSON: ~100 bytes)
Daemon: LLM → "I'll turn on the lights for you"
  │
  ▼ (WebSocket JSON: ~200 bytes)
Satellite: TTS → Audio playback
```

### Tier 2: Audio Satellite (ESP32-S3)

Button-activated with server-side ASR/TTS — reuses the WebUI audio infrastructure over WebSocket.

**Interaction Model**: Push-to-talk with optional visual feedback via NeoPixel LEDs. Users press a button to record, and LEDs indicate state (listening, processing, speaking).

| Component | Implementation | Memory | Notes |
|-----------|---------------|--------|-------|
| Audio Capture | I2S ADC | ~64 KB | Button-activated, 16kHz mono |
| VAD | Energy-based | ~1 KB | Silence detection (end of speech) |
| Audio Playback | I2S DAC | ~64 KB | Streaming TTS from daemon |
| WebSocket Client | esp_websocket_client | ~100-200 KB | Built into ESP-IDF |
| Network Buffers | WiFi stack | ~50 KB | WebSocket framing |
| GPIO Button | libgpiod | ~1 KB | Push-to-talk activation |
| NeoPixel LEDs | SPI driver | ~4 KB | State indicators (optional) |
| **Total** | | **~300-400 KB** | Requires ESP32-S3 with PSRAM |

**Hardware Requirement**: ESP32-S3 with 8MB PSRAM (standard on most dev boards)

**Capabilities**:
- Button-to-talk activation (no wake word — ESP32 can't run Whisper)
- Local silence detection (energy-based VAD knows when you stopped speaking)
- Raw PCM audio upload over WebSocket (same binary framing as WebUI)
- Streaming TTS audio playback from daemon (sentence-level, plays while LLM generates)
- No music streaming in v1

**Communication**: WebSocket (same port as WebUI, same binary message types)

```
User: [presses button] "Turn on the lights" [releases or silence detected]
  │
  ▼ (WebSocket binary: ~320KB raw PCM for 10s @ 16kHz)
Daemon: ASR (Whisper) → LLM → TTS (Piper) → PCM audio
  │
  ▼ (WebSocket binary: sentence-level TTS audio chunks)
Satellite: Audio playback (streams while LLM still generating)
```

**Why WebSocket instead of custom binary protocol?**
- `esp_websocket_client` is built into ESP-IDF — zero external dependencies
- Reuses 100% of the daemon's WebUI audio pipeline (decode, ASR, TTS, encode, response queue)
- Eliminates DAP1 TCP server entirely — one fewer server thread, one fewer port
- WebSocket framing handles fragmentation, keepalive, and reconnection
- Same session management, authentication, and logging as all other clients

### Audio Codec (Tier 2 v1): Raw PCM

Tier 2 v1 uses **raw 16-bit PCM at 16kHz** — the simplest possible approach.

| Codec | Bandwidth (10s) | ESP32 Effort | Server Effort |
|-------|-----------------|--------------|---------------|
| **Raw PCM 16kHz** | 320 KB | Zero | Skip resample (already 16kHz) |
| ADPCM (future v2) | 80 KB | Low (ESP-IDF built-in) | ~50 lines decode |
| Opus (not planned) | 30-40 KB | High (~200KB flash) | Already implemented |

**Why raw PCM for v1?**
- **Zero codec work** on ESP32 — just capture I2S samples and send
- **Server already supports it** — WebUI has PCM fallback when Opus unavailable
- **320KB for 10s** is fine on home WiFi (takes <0.5s to transmit)
- **Simpler debugging** — raw audio is trivially inspectable
- ADPCM can be added later if bandwidth becomes a concern with many satellites

**PCM Format**:
- Sample rate: 16kHz
- Bit depth: 16-bit signed, little-endian
- Channels: Mono
- No header — raw samples in WebSocket binary frames

### Satellite Registration and Identification

Satellites self-identify on connection with a structured identity that enables:
- Session tracking across reconnections
- Per-satellite conversation history
- Location-aware responses ("the kitchen lights")
- Logging and debugging

**Tier 1 Registration (WebSocket JSON)**:

```json
{
  "type": "satellite_register",
  "payload": {
    "uuid": "550e8400-e29b-41d4-a716-446655440000",
    "name": "Kitchen Assistant",
    "location": "kitchen",
    "tier": 1,
    "capabilities": {
      "local_asr": true,
      "local_tts": true,
      "wake_word": true
    },
    "hardware": {
      "platform": "rpi5",
      "memory_mb": 4096
    },
    "protocol_version": "2.0"
  }
}
```

**Registration Response**:

```json
{
  "type": "satellite_register_ack",
  "payload": {
    "session_id": "sess-abc123",
    "daemon_name": "DAWN Central",
    "conversation_restored": true,
    "conversation_age_seconds": 120,
    "memory_enabled": true,
    "memory_user": "krisk"
  }
}
```

**Identity Fields**:

| Field | Required | Description | Example |
|-------|----------|-------------|---------|
| `uuid` | Yes | Unique identifier (persists across reboots) | UUID v4 or MAC-based |
| `name` | Yes | Human-readable name | "Kitchen Assistant" |
| `location` | Yes | Room/area — appended to system prompt as `Room=X` so LLM knows command origin | "kitchen", "bedroom", "office" |
| `hardware_id` | No | Hardware serial number | For inventory tracking |

**UUID Generation**:
- Tier 1 (RPi): Generate UUID v4 on first boot, store in `/etc/dawn-satellite-id`
- Tier 2 (ESP32): Generate random UUID v4 on first boot (`esp_fill_random`), persist in NVS flash (avoids MAC-derived UUIDs which are predictable from WiFi sniffing)

**Tier 2 Registration (WebSocket JSON)**:

```json
{
  "type": "satellite_register",
  "payload": {
    "uuid": "a3f1c9b2-7d4e-4a8f-b612-3c5d7e9f0a1b",
    "name": "Office Speaker",
    "location": "office",
    "tier": 2,
    "capabilities": {
      "local_asr": false,
      "local_tts": false,
      "wake_word": false,
      "push_to_talk": true
    },
    "hardware": {
      "platform": "esp32s3",
      "memory_mb": 2
    },
    "protocol_version": "2.0",
    "reconnect_secret": "previously-issued-secret-if-any"
  }
}
```

The daemon inspects `local_asr: false` and `audio_codecs` to route this client through the WebUI audio pipeline (binary PCM frames) instead of the text query path.

---

## 6. Protocol Specification

### 6.1 Tier 1: WebSocket Protocol

Tier 1 satellites use WebSocket with JSON messages, extending the existing WebUI protocol.

**Message Types**:

| Type | Direction | Purpose |
|------|-----------|---------|
| `satellite_register` | S→D | Initial registration with capabilities |
| `satellite_register_ack` | D→S | Registration confirmation |
| `satellite_query` | S→D | User's transcribed command |
| `satellite_ping` | S→D | Keepalive |
| `satellite_pong` | D→S | Keepalive response |
| `stream_start` | D→S | Begin streaming LLM response |
| `stream_delta` | D→S | Partial LLM response text |
| `stream_end` | D→S | End of streamed response |
| `state` | D→S | State update (thinking, idle) |
| `error` | D→S | Error notification |

**Query Message**:

```json
{
  "type": "satellite_query",
  "payload": {
    "text": "turn on the lights"
  }
}
```

**Streaming Response**:

```json
{"type": "stream_start", "payload": {"stream_id": 1}}
{"type": "stream_delta", "payload": {"stream_id": 1, "text": "I'll turn on the lights "}}
{"type": "stream_delta", "payload": {"stream_id": 1, "text": "for you."}}
{"type": "stream_end", "payload": {"stream_id": 1, "reason": "complete"}}
```

**Example Flow (Tier 1)**:

```
Satellite                                    Daemon
    │                                           │
    │──── WS Connect ──────────────────────────>│
    │                                           │
    │──── satellite_register ──────────────────>│
    │                                           │
    │<─── satellite_register_ack ──────────────│
    │                                           │
    │  [User says "Friday, turn on the lights"] │
    │  [Local: VAD → Wake Word → ASR]           │
    │                                           │
    │──── satellite_query ─────────────────────>│
    │     {"text": "turn on the lights"}        │
    │                                           │
    │              [Daemon: LLM processing]     │
    │                                           │
    │<─── stream_start ─────────────────────────│
    │     {"stream_id": 1}                      │
    │                                           │
    │<─── stream_delta ─────────────────────────│
    │     {"text": "I'll turn on the lights. "} │
    │                                           │
    │  [Satellite: TTS sentence 1]              │
    │                                           │
    │<─── stream_delta ─────────────────────────│
    │     {"text": "They should be on now."}    │
    │                                           │
    │  [Satellite: TTS sentence 2]              │
    │                                           │
    │<─── stream_end ───────────────────────────│
    │     {"reason": "complete"}                │
    │                                           │
```

### 6.2 Tier 2: WebSocket Audio Protocol

Tier 2 satellites use the **same WebSocket connection and binary message types** as the WebUI browser client. This eliminates the need for a custom protocol — the daemon's existing audio pipeline handles everything.

**Binary Message Types** (shared with WebUI, defined in `webui_server.h`):

| Value | Name | Direction | Payload |
|-------|------|-----------|---------|
| `0x01` | `WS_BIN_AUDIO_IN` | S→D | Raw PCM audio chunk |
| `0x02` | `WS_BIN_AUDIO_IN_END` | S→D | End of recording (triggers ASR) |
| `0x11` | `WS_BIN_AUDIO_OUT` | D→S | TTS audio chunk (PCM) |
| `0x12` | `WS_BIN_AUDIO_SEGMENT_END` | D→S | End of TTS sentence (play now) |

**Text Message Types** (JSON, shared with Tier 1):

| Type | Direction | Purpose |
|------|-----------|---------|
| `satellite_register` | S→D | Registration with `local_asr: false`, `audio_codecs: ["pcm"]` |
| `satellite_register_ack` | D→S | Session ID, capabilities confirmed |
| `satellite_ping` | S→D | Keepalive (10s interval) |
| `state` | D→S | State updates (listening, thinking, speaking, idle) |
| `error` | D→S | Error notification |

**Example Flow (Tier 2)**:

```
Satellite                                    Daemon
    │                                           │
    │──── WS Connect (port 3000) ──────────────>│
    │                                           │
    │──── satellite_register ──────────────────>│
    │     {tier: 2, local_asr: false,           │
    │      audio_codecs: ["pcm"]}               │
    │                                           │
    │<─── satellite_register_ack ──────────────│
    │     {session_id, use_opus: false}         │
    │                                           │
    │  [User presses button]                    │
    │                                           │
    │──── 0x01 [PCM audio chunk] ──────────────>│  (accumulates in audio buffer)
    │──── 0x01 [PCM audio chunk] ──────────────>│
    │──── 0x01 [PCM audio chunk] ──────────────>│
    │──── 0x02 [end of recording] ─────────────>│  (spawns audio worker thread)
    │                                           │
    │<─── state: "thinking" ────────────────────│  (ASR + LLM processing)
    │                                           │
    │<─── state: "speaking" ────────────────────│
    │<─── 0x11 [TTS audio: sentence 1] ────────│  (plays immediately)
    │<─── 0x12 [segment end] ──────────────────│
    │                                           │
    │<─── 0x11 [TTS audio: sentence 2] ────────│  (plays while LLM continues)
    │<─── 0x12 [segment end] ──────────────────│
    │                                           │
    │<─── state: "idle" ────────────────────────│
    │                                           │
```

**Audio Pipeline on Daemon** (reused from WebUI, `webui_audio.c`):

```
ESP32 → WS binary 0x01 → accumulate in audio buffer
                            ↓ (on 0x02)
                     spawn audio_worker_thread()
                            ↓
                     PCM @ 16kHz (skip resample — already 16kHz)
                            ↓
                     Whisper ASR → transcript
                            ↓
                     LLM processing (streaming)
                            ↓
                     sentence callback → TTS (Piper) → PCM
                            ↓
                     WS binary 0x11 → ESP32 playback
```

**Key difference from WebUI**: The WebUI sends Opus @ 48kHz (resampled to 16kHz on server). Tier 2 sends raw PCM @ 16kHz, so the server skips the Opus decode and resample steps entirely.

---

## 7. Common Code Architecture

### Monorepo Structure

Both the DAWN daemon and satellites share common code via a `common/` directory. This ensures implementations stay in sync and reduces maintenance burden.

```
dawn/
├── common/                        # SHARED CODE (daemon + satellites)
│   ├── include/
│   │   ├── asr/
│   │   │   ├── asr_engine.h       # Unified ASR abstraction
│   │   │   ├── asr_vosk.h         # Vosk streaming backend
│   │   │   ├── asr_whisper.h      # Whisper batch backend
│   │   │   ├── vad_silero.h       # Silero VAD
│   │   │   └── vosk_api.h         # Vosk C API (third-party)
│   │   ├── audio/
│   │   │   └── ring_buffer.h      # Thread-safe ring buffer
│   │   ├── tts/
│   │   │   ├── tts_piper.h        # Piper TTS
│   │   │   └── tts_preprocessing.h
│   │   ├── utils/
│   │   │   ├── sentence_buffer.h  # Sentence boundary detection
│   │   │   └── string_utils.h
│   │   ├── logging.h              # Shared logging (daemon + satellite)
│   │   └── logging_common.h       # Callback-based logging for common lib
│   ├── src/
│   │   ├── asr/
│   │   │   ├── asr_engine.c       # Engine dispatch (Whisper/Vosk)
│   │   │   ├── asr_vosk.c         # Vosk streaming
│   │   │   ├── asr_whisper.c      # Whisper batch
│   │   │   └── vad_silero.c       # Silero ONNX VAD
│   │   ├── audio/
│   │   │   └── ring_buffer.c
│   │   ├── tts/
│   │   │   ├── tts_piper.cpp
│   │   │   └── tts_preprocessing.cpp
│   │   ├── utils/
│   │   │   ├── sentence_buffer.c
│   │   │   └── string_utils.c
│   │   ├── logging.c              # Logging impl + bridge callback
│   │   └── logging_common.c
│   └── CMakeLists.txt             # Builds libdawn_common.a + optional libs
│
├── src/                           # DAEMON-SPECIFIC CODE
│   ├── dawn.c                     # Main daemon entry point
│   ├── llm/                       # LLM integration (daemon only)
│   ├── webui/
│   │   ├── webui_server.c         # Extended for satellite support
│   │   └── webui_satellite.c      # Satellite message handlers
│   ├── core/
│   │   └── session_manager.c      # Extended with SESSION_TYPE_DAP2
│   └── ...
│
├── dawn_satellite/                # TIER 1 SATELLITE (Raspberry Pi)
│   ├── src/
│   │   ├── main.c                 # Entry point, model loading, startup
│   │   ├── voice_processing.c     # Voice pipeline (~1100 lines)
│   │   ├── ws_client.c            # libwebsockets client + background thread
│   │   ├── audio_capture.c        # ALSA capture with ring buffer
│   │   ├── audio_playback.c       # ALSA playback with resampling
│   │   ├── satellite_config.c     # TOML config + identity persistence
│   │   ├── satellite_state.c      # State machine logic
│   │   ├── display.c              # Framebuffer display (optional)
│   │   ├── gpio_control.c         # GPIO input (optional)
│   │   ├── neopixel.c             # NeoPixel LEDs (optional)
│   │   └── toml.c                 # tomlc99 parser
│   ├── include/                   # Headers for all above
│   ├── config/
│   │   └── satellite.toml         # Default configuration
│   └── CMakeLists.txt             # Links common libs
│
├── models/                        # Shared models
│   ├── silero_vad_16k_op15.onnx
│   ├── ggml-tiny.en.bin           # Whisper (optional)
│   ├── vosk-model-small-en-us/    # Vosk (default)
│   └── en_GB-alba-medium.onnx*    # Piper TTS voice
│
└── CMakeLists.txt                 # Top-level build
```

### What Goes in Common vs Specific

| Component | Location | Reason |
|-----------|----------|--------|
| **Ring Buffer** | `common/` | Identical implementation |
| **VAD (Silero)** | `common/` | Identical implementation |
| **ASR Engine** | `common/` | Unified dispatch (Whisper/Vosk) |
| **ASR Whisper** | `common/` | Identical, just different model sizes |
| **ASR Vosk** | `common/` | Streaming backend |
| **TTS (Piper)** | `common/` | Identical implementation |
| **Sentence Buffer** | `common/` | Identical implementation |
| **Logging** | `common/` | Shared logging with bridge callback |
| **Audio Capture** | `specific/` | ALSA (daemon/RPi) vs I2S (ESP32) |
| **Audio Playback** | `specific/` | Platform-specific |
| **Voice Processing** | `satellite only` | VAD + wake word + ASR + TTS loop |
| **LLM Interface** | `daemon only` | Satellites don't call LLM |
| **WebSocket Server** | `daemon only` | Only daemon accepts connections |
| **WebSocket Client** | `satellite only` | Only satellites connect out |

### Build Configuration

The common library builds as multiple static libraries with optional components:

```cmake
# common/CMakeLists.txt - builds multiple libraries
dawn_common           # Core: ring_buffer, sentence_buffer, string_utils, logging
dawn_common_vad       # Optional: Silero VAD (requires ONNX Runtime)
dawn_common_asr       # Optional: Whisper ASR (requires whisper.cpp)
dawn_common_asr_vosk  # Optional: Vosk ASR (requires libvosk)
dawn_common_asr_engine # Optional: Unified ASR dispatch
dawn_common_tts       # Optional: Piper TTS (requires ONNX Runtime, piper-phonemize)
```

Satellite CMake options control which components are built:

```cmake
# dawn_satellite/CMakeLists.txt
option(ENABLE_VAD         "Enable Silero VAD"            ON)
option(ENABLE_VOSK_ASR    "Enable Vosk streaming ASR"    ON)   # Default for satellite
option(ENABLE_WHISPER_ASR "Enable Whisper batch ASR"     OFF)  # Optional
option(ENABLE_TTS         "Enable Piper TTS"             ON)
```

---

## 8. Implementation Phases

### Phase 0: Code Refactoring (Foundation)

**Status**: ✅ **COMPLETE**

**Goal**: Extract shared code to `common/` library without breaking daemon

This phase addresses the architecture reviewer's critical finding: existing code has tight coupling to daemon-specific infrastructure (logging, metrics, AEC). Must decouple before any satellite work.

**Code Standards**: All new code in `common/` MUST follow existing `.clang-format` rules. Run `./format_code.sh` before committing.

1. **Create logging abstraction**:
   ```c
   // common/include/logging_common.h
   typedef void (*log_callback_t)(int level, const char *fmt, va_list args);
   void dawn_common_set_logger(log_callback_t callback);

   // Macros that use the callback
   #define LOG_COMMON_INFO(fmt, ...) dawn_common_log(LOG_LEVEL_INFO, fmt, ##__VA_ARGS__)
   ```

2. **Create TTS C interface** (for satellite use):
   ```c
   // common/include/tts/tts_interface.h
   #ifdef __cplusplus
   extern "C" {
   #endif

   typedef struct TTSContext TTSContext;

   TTSContext *tts_init(const char *model_path);
   int tts_synthesize(TTSContext *ctx, const char *text, int16_t **audio, size_t *samples);
   void tts_cleanup(TTSContext *ctx);

   #ifdef __cplusplus
   }
   #endif
   ```

3. **Decouple TTS from daemon-specific features**:
   - Remove AEC reference feed (daemon-only)
   - Remove command callback integration (daemon-only)
   - Keep mutex-protected synthesis as core functionality

4. **Create `common/` directory structure**:
   - Move: ring_buffer, vad_silero, asr_whisper, asr_interface, chunking_manager
   - Move: text_to_speech, piper, sentence_buffer
   - Move: logging (with abstraction)
   - Create: `common/CMakeLists.txt` for `libdawn_common.a`

5. **Update daemon to use common library**:
   - Update daemon CMakeLists.txt to link `libdawn_common.a`
   - Provide daemon-specific callbacks for logging
   - **Verify daemon still builds and runs correctly**

**Deliverables**:
- `common/` directory with abstracted shared code
- `libdawn_common.a` static library
- `tts_interface.h` with C-compatible TTS API
- Daemon fully functional (no behavior change)

**Exit Criteria**:
- Daemon builds, runs, and behaves identically to pre-refactor

---

### Phase 1: WebSocket Satellite Protocol

**Status**: ✅ **COMPLETE**

**Goal**: Extend WebUI server for satellite connections

1. **Add satellite message types** to WebUI:
   ```c
   // src/webui/webui_satellite.c
   int handle_satellite_register(ws_client_t *client, cJSON *payload);
   int handle_satellite_query(ws_client_t *client, cJSON *payload);
   void send_satellite_response_stream(ws_client_t *client, const char *delta);
   void send_satellite_response_end(ws_client_t *client);
   ```

2. **Extend session manager**:
   ```c
   // include/core/session_manager.h
   typedef enum {
      SESSION_TYPE_LOCAL,
      SESSION_TYPE_WEBUI,
      SESSION_TYPE_SATELLITE,  // NEW
   } session_type_t;
   ```

3. **Wire up LLM integration**:
   - Route `satellite_query` to existing LLM pipeline
   - Stream responses back via WebSocket
   - Execute commands via existing MQTT/tool system

4. **Test with wscat or Python client**:
   ```bash
   # Test satellite registration
   wscat -c ws://localhost:8080
   > {"type": "satellite_register", "payload": {"uuid": "test-123", "name": "Test", "location": "office", "tier": 1, "capabilities": {"local_asr": true, "local_tts": true}}}
   < {"type": "satellite_register_ack", "payload": {"session_id": "sess-abc"}}
   > {"type": "satellite_query", "payload": {"text": "what time is it"}}
   < {"type": "satellite_response_stream", "payload": {"delta": "The current time is "}}
   < {"type": "satellite_response_stream", "payload": {"delta": "3:45 PM."}}
   < {"type": "satellite_response_end", "payload": {}}
   ```

**Deliverables**:
- `src/webui/webui_satellite.c` - Satellite message handlers
- Extended `session_manager.h/c` - SESSION_TYPE_SATELLITE
- Python test client for protocol validation

**Exit Criteria**: Text query via WebSocket returns streamed LLM response

---

### Phase 2: Tier 1 Satellite Binary

**Status**: ✅ **COMPLETE**

**Goal**: Full satellite with local ASR/TTS that feels identical to talking to the daemon

1. **Create `dawn_satellite/` directory** ✅
2. **Implement satellite state machine** (mirrors daemon) ✅:
   ```
   SILENCE → WAKEWORD_LISTEN → COMMAND_RECORDING → PROCESSING → SPEAKING
   ```
3. **Implement WebSocket client** (libwebsockets) ✅
4. **Implement wake word detection** (VAD + ASR transcript check) ✅
5. **Implement local ASR** (Vosk streaming default, Whisper optional) ✅
6. **Implement local TTS playback** (Piper with sentence-level streaming) ✅
7. **Implement offline fallback** ✅:
   - On network error: TTS "I can't reach the server right now"
8. **Time-of-day greeting** on startup ✅
9. **Clean Ctrl+C shutdown** across all states ✅
10. **Configuration file support** (`satellite.toml`) ✅:
   ```toml
   [identity]
   uuid = ""  # Auto-generated if empty
   name = "Kitchen Assistant"
   location = "kitchen"

   [server]
   host = "192.168.1.100"
   port = 8080
   ssl = false

   [audio]
   capture_device = "plughw:0,0"
   playback_device = "plughw:0,0"
   sample_rate = 16000

   [vad]
   enabled = true
   silence_duration_ms = 800
   threshold = 0.5

   [wake_word]
   enabled = true
   word = "friday"
   sensitivity = 0.5
   ```

**Deliverables**:
- `dawn_satellite/` with working code ✅
- `dawn_satellite` binary for RPi ✅
- TOML config file support ✅
- User documentation (see `docs/DAP2_SATELLITE.md`) ✅

**Exit Criteria**: User cannot distinguish satellite from daemon interaction

**Target Latencies**:
- Wake word detection: <500ms
- ASR transcription: <2s (Whisper tiny on RPi 5)
- Network round-trip: <100ms
- TTS first audio: <500ms

---

### Phase 3: Streaming Responses

**Status**: ✅ **COMPLETE**

**Goal**: Stream LLM responses sentence-by-sentence for lower perceived latency

1. **Sentence buffering on satellite** ✅:
   - `stream_delta` messages fed to sentence buffer
   - `on_sentence_complete` callback fires TTS per sentence
   - TTS plays while more deltas still arriving

2. **Streaming works end-to-end** ✅:
   - First audio plays while LLM still generating
   - 150ms pause between sentences for natural rhythm

3. **Unified logging** ✅:
   - `logging.h`/`logging.c` shared between daemon and satellite
   - Identical format (timestamps, colors, file:line)

**Deliverables**:
- Streaming protocol working end-to-end ✅
- Sentence-level TTS during streaming ✅
- Unified logging ✅

**Exit Criteria**: First audio plays during LLM generation ✅

---

### Phase 3.5: Touchscreen UI (SDL2)

**Status**: ✅ **COMPLETE**

**Goal**: Premium touchscreen interface for Tier 1 satellites with attached displays

**Platform**: Pi OS Lite with SDL2 KMSDRM backend (no X11, direct GPU rendering)

**Display Support**: 7" TFT touchscreen (primary target: 1024x600)

#### Core Components

1. **Visualization (ported from WebUI)**: ✅ COMPLETE
   - Core orb with radial gradient glow (pre-rendered texture)
   - Three-ring system: outer (status), middle (throughput), inner (waveform)
   - State-based colors matching WebUI: idle (gray), listening (green), thinking (amber), speaking (cyan)
   - Smooth transitions with easing functions
   - FFT spectrum bars during SPEAKING (Goertzel analysis, 64 bins, radial layout)
   - Spectrum oriented from 12 o'clock

2. **Transcript Display**: ✅ COMPLETE
   - Scrollable conversation history with cached text rendering (40 entries, 4KB per entry)
   - Touch-drag scrolling with auto-scroll on new messages, manual scroll disables auto-scroll
   - Mouse-drag fallback for desktop testing
   - Streaming live transcript during LLM response
   - Inline markdown rendering (bold, italic, code spans, bullet points)
   - AI name in ALL CAPS, emoji stripping (font lacks emoji support)
   - Date/time display (top-right, configurable 12h/24h format)
   - WiFi signal strength bars (polled from /proc/net/wireless)
   - Status detail line (tool calls, thinking info from daemon)
   - User transcription display ("You: ..." after ASR completes)

3. **Quick Actions Bar**: REMOVED (replaced by status bar icon pattern)
   - Features are accessed via icons in the transcript status bar instead
   - Icons appear only when features are available (no placeholders)
   - Currently: music icon (note glyph) toggles music panel
   - Future icons added as features ship (e.g., lights, thermostat)

4. **Media Player Panel**: ✅ COMPLETE
   - Three-tab design: Playing (transport + visualizer), Queue, Library (artists/albums/tracks)
   - Transport controls: play/pause, prev/next, shuffle/repeat toggles
   - Progress bar with drag-to-seek, elapsed/remaining time
   - Opus audio streaming from daemon with ALSA playback
   - Goertzel FFT visualizer from live audio stream (not simulated)
   - Library browsing with pagination (50 items/page, next/prev navigation)
   - Artist → tracks and album → tracks drill-down
   - SDL primitive icons (no image assets needed)
   - Touch-optimized with 44px minimum targets

5. **Settings Enhancements**: ✅ COMPLETE
   - Brightness slider (sysfs backlight or SDL software dimming fallback for HDMI)
   - Volume slider (ALSA mixer control)
   - 12/24-hour time format toggle (animated pill switch, updates transcript + screensaver clocks)
   - Persistent brightness/volume/time format across restarts (saved to config)

6. **Theme System**: ✅ COMPLETE
   - 5 runtime-switchable themes: Cyan (default), Purple, Blue, Green, Terminal
   - Dot picker in settings panel for theme selection
   - 200ms ease-out cubic crossfade transitions between themes
   - Per-theme accent, accent_dim, background (3 levels), and text (3 levels) colors
   - Terminal theme has distinct darker backgrounds and muted text for CRT aesthetic
   - State colors (listening/thinking/speaking/error) are fixed and never change with theme
   - Active theme persisted to `satellite.toml` under `[sdl_ui] theme = "cyan"`
   - All accent-colored elements update: orb tint, music controls, progress bars, tab underlines, transcript labels, slider fills, screensaver elements

6. **Screensaver/Ambient Mode**: ✅ COMPLETE
   - **Clock mode**: Time (80pt mono) and date (24pt cyan) centered with Lissajous drift for burn-in prevention
   - **"D.A.W.N." watermarks**: All four corners with sine-pulse fade animation (8s period)
   - **Fullscreen rainbow visualizer**: 64-bin Goertzel FFT with HSV color cycling (precomputed LUT), peak hold indicators, gradient reflections
   - **Track info pill**: Two-line layout — large title (36pt) above smaller album/artist (24pt) with bullet separator, fade-in on track change, fade-out after 5s
   - **dB-scale spectrum processing**: Matches WebUI (60dB range, 0.7 gamma curve)
   - **Auto mode switching**: Clock when idle, visualizer when music plays; manual trigger via tap on music panel visualizer
   - **Configurable idle timeout**: 30-600s (default 120s), `[screensaver]` config section
   - **Wake word dismissal**: Only dismissed by wake word detection (SILENCE→non-SILENCE), not simple VAD
   - **Frame rate management**: 30 FPS visualizer, 10 FPS clock, main scene skipped when screensaver opaque

#### Touch Interactions — ✅ COMPLETE

| Gesture | Action | Status |
|---------|--------|--------|
| Tap orb | Manual wake (bypass wake word) | ✅ |
| Long press orb | Cancel/stop current operation | ✅ |
| Drag transcript | Scroll conversation history | ✅ |
| Swipe down from top | Settings panel (server info, connection, uptime, brightness/volume) | ✅ |
| Tap music icon | Open music player panel | ✅ |
| Tap music visualizer | Fullscreen rainbow visualizer (closes music panel) | ✅ |
| Tap during screensaver | Dismiss screensaver (first touch swallowed) | ✅ |
| Swipe up from bottom | Unassigned (reserved) | Closes settings if open, otherwise no-op |
| Swipe left/right | Navigate conversation history | ❌ |

#### Layout

**7" (1024x600)**: Landscape, side-by-side (320px orb panel + 664px transcript), full visualization

#### Memory Budget (Pi Zero 2 W)

| Resource | Allocation |
|----------|------------|
| Texture cache | ~10 MB (glow frames, icons, font atlas) |
| Photo buffer | ~4 MB (2x scaled images for crossfade) |
| Frame rate | 30 FPS active/visualizer, 10 FPS clock screensaver, 10 FPS idle |

#### Color Palette (matching WebUI variables.css)

```c
// Backgrounds
COLOR_BG_PRIMARY   = #121417
COLOR_BG_SECONDARY = #1b1f24
COLOR_BG_TERTIARY  = #242a31

// Text
COLOR_TEXT_PRIMARY   = #e6e6e6
COLOR_TEXT_SECONDARY = #7b8794

// States
COLOR_ACCENT  = #2dd4bf  // Cyan (theme default)
COLOR_SUCCESS = #22c55e  // Green (listening)
COLOR_WARNING = #f0b429  // Amber (thinking)
COLOR_ERROR   = #ef4444  // Red
```

#### File Structure (Actual Implementation)

```
dawn_satellite/src/ui/
├── sdl_ui.c            # UI lifecycle, event loop, panel rendering (settings/quick actions)
├── ui_orb.c            # Orb visualization + FFT spectrum bars
├── ui_orb.h            # Orb context and API
├── ui_transcript.c     # Scrollable transcript with texture caching
├── ui_transcript.h     # Transcript panel types and API
├── ui_markdown.c       # Inline markdown renderer (bold/italic/code)
├── ui_markdown.h       # Markdown font set and render API
├── ui_music.c          # Music player panel (Playing/Queue/Library tabs)
├── ui_music.h          # Music panel types and API
├── music_types.h       # Shared music data structures
├── ui_slider.c         # Brightness/volume slider widgets
├── ui_slider.h         # Slider types and API
├── ui_touch.c          # Touch gesture recognition (tap/long-press/swipe)
├── ui_touch.h          # Touch state types
├── ui_screensaver.c    # Screensaver (clock + fullscreen rainbow visualizer)
├── ui_screensaver.h    # Screensaver types and API
├── ui_theme.c          # 5-theme table with 200ms crossfade transitions
├── ui_theme.h          # Theme system API (set, tick, accessors)
├── ui_colors.h         # Color constants, HSV conversion, easing utilities
├── backlight.c         # Display brightness (sysfs + software dimming fallback)
└── backlight.h         # Backlight control API
```

#### Dependencies

```bash
sudo apt install libsdl2-dev libsdl2-ttf-dev libsdl2-gfx-dev libdrm-dev
sudo usermod -aG video,render $USER
```

**Deliverables**:
- `dawn_satellite/src/ui/` with SDL2 implementation ✅
- Touch gesture system ✅
- Screensaver with clock + fullscreen rainbow visualizer ✅
- Media player panel (Playing/Queue/Library) ✅
- 5-theme system with dot picker, crossfade transitions, and TOML persistence ✅

**Exit Criteria**: UI indistinguishable from premium smart home device, <16ms frame time ✅

---

### Phase 4: Tier 2 Satellite (ESP32-S3)

**Status**: ✅ **COMPLETE**

**Goal**: Button-activated satellite using WebSocket + PCM audio (reuses WebUI audio pipeline)

**Hardware**: Adafruit ESP32-S3 TFT Feather (built-in 240x135 TFT + NeoPixels, 2MB OPI PSRAM) with MAX98357 I2S speaker amp, analog electret mic on ADC, push-to-talk button. See `dawn_satellite_arduino/README.md` for pin assignments and Arduino IDE setup.

**Platform**: Arduino IDE with arduinoWebSockets (Links2004), ArduinoJson v7+, Adafruit GFX/ST7789/NeoPixel libraries. Uses the Arduino ESP32 core (Espressif), not raw ESP-IDF, for faster development.

**Daemon-side changes** ✅:
1. **Route binary audio from satellite connections**: `webui_server.c` allows binary audio from `is_satellite` connections
2. **Handle 16kHz PCM input**: `webui_audio.c` skips resample when input is already 16kHz
3. **Send PCM TTS output**: Raw 22050Hz PCM via `0x11` frames (native Piper rate, satellite resamples to 48kHz for I2S)
4. **Queue head-of-line blocking fix**: Response queue scans forward past choked clients instead of blocking on head
5. **Fragment buffer expansion**: Continuation frames use realloc (matching initial frame path) instead of silently dropping data

**ESP32-side** ✅:
1. **`dawn_satellite_arduino/` directory** (Arduino sketch, not ESP-IDF)
2. **WebSocket client** using arduinoWebSockets (Links2004) with WSS (self-signed cert)
3. **`satellite_register`** with `local_asr: false`, `push_to_talk: true`, persistent UUID
4. **Button → record → send PCM**: ADC sampling @ 16kHz → `0x01` binary frames → `0x02` on release
5. **TTS ring buffer playback**: 524,288-sample power-of-two ring buffer in PSRAM (~1MB), spinlock-protected producer/consumer, 22050→48000Hz linear interpolation with cross-boundary carry
6. **TCP backpressure**: Skip `webSocket.loop()` when ring buffer >50% full, backs up TCP window
7. **NeoPixel LED feedback**: Idle cycling (HSV crossfade), red=recording, yellow=waiting, green=playing, pulsing orange=WiFi connecting; mode-change detection avoids redundant `strip.show()` on static modes
8. **TFT status display**: Large centered text (size 3) for status with partial `fillRect` redraws, scrolling log for setup
9. **NVS persistence**: Random UUID v4 (`esp_fill_random`) and reconnect_secret stored in ESP32 flash
10. **Credentials management**: WiFi SSID/password, server IP/port, satellite name/location in gitignored `arduino_secrets.h`

**Deliverables**:
- `dawn_satellite_arduino/` Arduino sketch with complete README ✅
- Working ESP32-S3 firmware with WebSocket audio ✅
- No custom binary protocol — 100% reuse of WebUI audio path ✅
- Daemon queue and fragment handling hardened ✅

**Exit Criteria**: Button-to-response <4s (p95)

---

### Phase 5: Multi-Satellite + Polish

**Goal**: Production-ready multi-satellite support

1. **Load testing**: 5+ simultaneous satellites
2. **Session persistence**: Conversation history survives daemon restart
3. **Documentation**:
   - User guide for satellite setup
   - Troubleshooting guide
   - Hardware recommendations

**Deliverables**:
- Multi-client support verified
- Performance benchmarks
- Complete documentation

---

### Phase 6: Optional Enhancements (Deferred)

Based on real-world measurements and user feedback:

1. **mDNS discovery**: Daemon advertises `_dawn._tcp.local`
2. **TLS encryption**: Certificate-based authentication
3. **UDP audio streaming**: Only if TCP latency >150ms p95

---

## 9. Platform Considerations

### Tier 1 Platforms

#### Raspberry Pi 5 (Recommended)

| Requirement | Available | Sufficient? |
|-------------|-----------|-------------|
| RAM | 4-8 GB | ✅ Yes (need ~280 MB) |
| CPU | Cortex-A76 2.4 GHz | ✅ Yes (Whisper tiny: ~1-2s) |
| Storage | 32+ GB SD | ✅ Yes (need ~200 MB for models) |
| Audio | USB mic + 3.5mm out | ✅ Yes |

**Estimated Performance**:
- Wake word detection: Real-time
- ASR (Whisper tiny): ~1-2s for 5s audio
- TTS (Piper): ~200-500ms per sentence

#### Raspberry Pi 4 (Supported)

| Requirement | Available | Sufficient? |
|-------------|-----------|-------------|
| RAM | 2-8 GB | ⚠️ 2GB tight, 4GB+ preferred |
| CPU | Cortex-A72 1.5 GHz | ✅ Yes (slower than Pi 5) |
| Storage | 32+ GB SD | ✅ Yes |
| Audio | USB mic + 3.5mm out | ✅ Yes |

**Estimated Performance**:
- Wake word detection: Real-time
- ASR (Whisper tiny): ~3-4s for 5s audio
- TTS (Piper): ~300-700ms per sentence

### Tier 2 Platforms

#### ESP32-S3 (Recommended)

| Requirement | Available | Sufficient? |
|-------------|-----------|-------------|
| RAM | 512 KB + 8 MB PSRAM | ✅ Yes for audio buffering |
| CPU | Xtensa LX7 240 MHz | ✅ Yes for ADPCM + networking |
| Storage | 16 MB flash | ✅ Yes for firmware |
| Audio | I2S mic + I2S DAC | ✅ Yes |

**Capabilities**:
- Button-activated recording
- Energy-based VAD (silence detection)
- WebSocket client (esp_websocket_client, built into ESP-IDF)
- Raw PCM audio streaming (16kHz, 16-bit) — server handles ASR/TTS
- Cannot run Whisper or Piper locally

---

## 10. Security Architecture

### Phase 1: Network Isolation (MVP)

For initial deployment, rely on network-level security:

**Phase 1 Requirements**:
- Document security limitations in user guide
- Recommend VLAN isolation for DAWN devices
- No authentication (trusted network assumption)
- WebSocket connection (can use WSS for encryption)

### Phase 2+: TLS and Authentication

**TLS 1.3 for All Connections**:
- WebSocket Secure (WSS) for all tiers (Tier 1 and Tier 2)
- Certificate-based authentication optional
- ESP32 supports TLS via mbedTLS (built into ESP-IDF)

---

## 11. Network Reliability

### Connection Management (Tier 1 WebSocket)

WebSocket provides built-in reliability features:
- Ping/pong heartbeat (configurable interval)
- Automatic reconnection (libwebsockets handles this)
- Connection state tracking

**Satellite Reconnection Strategy**:
```
1. Detect disconnect (socket error or missed pongs)
2. Exponential backoff: 1s, 2s, 4s, 8s, 16s, max 30s
3. On reconnect: send satellite_register with same UUID
4. Daemon restores conversation if available
5. During reconnection: TTS "I've lost connection to the server"
```

### Session Recovery

**On Satellite Disconnect**:
1. Daemon marks session as "disconnected" (not deleted)
2. Conversation history preserved for 10 minutes
3. Satellite UUID used to match reconnection

**On Satellite Reconnect**:
1. Satellite sends `satellite_register` with same UUID
2. Daemon matches UUID to existing session
3. Returns `conversation_restored: true` if history available
4. Satellite continues conversation seamlessly

---

## 12. Performance Validation

### Metrics Collection

**Satellite Metrics** (collected locally, reported to daemon):

| Metric | Description | Target |
|--------|-------------|--------|
| `wake_word_latency_ms` | Time from speech end to wake word detection | <500ms p95 |
| `asr_latency_ms` | Time to transcribe command | <2000ms p95 |
| `tts_latency_ms` | Time to synthesize first audio | <500ms p95 |
| `network_rtt_ms` | Round-trip time to daemon | <50ms p95 |

**End-to-End Targets** (user-perceived):

| Interaction | Target | Measurement |
|-------------|--------|-------------|
| Wake word → acknowledgment | <1.5s | First TTS audio plays |
| Command → first response audio | <2.5s | Streaming response starts |
| Total interaction | <8s | Response audio completes |

### Validation Plan

**Phase Completion Criteria**:

| Phase | Metric | Pass Criteria |
|-------|--------|---------------|
| Phase 2 | Tier 1 wake-to-response | <3s p95 |
| Phase 3 | Streaming first audio | <1.5s p95 |
| Phase 4 | Tier 2 button-to-response | <4s p95 |

---

## 13. Memory System Integration

### User Mapping for Satellites

DAP2 satellites operate as "guest" sessions by default and do NOT store memories. This prevents visitors or other household members from polluting a user's memory profile.

**Configuration for memory-enabled satellites:**

```toml
[dap2.satellites]
# Map satellite UUIDs to authenticated users for memory storage
# Only mapped satellites will have memories stored
# Unmapped satellites operate in guest mode (no memory)

[dap2.satellites."550e8400-e29b-41d4-a716-446655440000"]
name = "Kitchen Assistant"
user = "krisk"              # Map to user for memory storage

[dap2.satellites."660f9511-f3ac-52e5-b827-557766551111"]
name = "Office Assistant"
user = "tomp"               # Different user

[dap2.satellites."770a0622-g4bd-63f6-c938-668877662222"]
name = "Guest Room"
# No user mapping = guest mode, no memory storage
```

**Behavior:**
- **Mapped satellite**: Queries associated with configured user, memories stored and retrieved
- **Unmapped satellite**: Guest mode, no memory storage, core facts not injected
- **Registration response** includes memory status:
  ```json
  {
    "type": "satellite_register_ack",
    "payload": {
      "session_id": "sess-abc123",
      "memory_enabled": true,
      "memory_user": "krisk"
    }
  }
  ```

---

## 14. Open Questions

### Decided

| Question | Decision | Rationale |
|----------|----------|-----------|
| **Transport (Tier 1)** | WebSocket | Leverage existing WebUI infrastructure |
| **Transport (Tier 2)** | WebSocket | Reuse WebUI audio pipeline; esp_websocket_client built into ESP-IDF |
| **Audio Codec (Tier 2 v1)** | Raw PCM 16kHz | Zero ESP32 effort; ADPCM optional in v2 |
| **Wake Word (Tier 1)** | Same as daemon (VAD + Whisper) | Seamless UX, code reuse |
| **Wake Word (Tier 2)** | None (button-activated) | ESP32 can't run Whisper |
| **Session Management** | Extend existing session_manager | Unified architecture |
| **Offline Mode** | TTS "I can't reach the server" | Simple, clear user feedback |
| **Code Sharing** | Monorepo with `common/` | Single source of truth |
| **Satellite Identity** | UUID + name + location | Session tracking, conversation history |

### Still Open (Minor)

1. **LLM context per satellite**: Should conversation history have a maximum length per satellite?
2. **Model distribution**: How to push model updates to satellites?
3. **Metrics aggregation**: Should daemon aggregate satellite metrics for monitoring?

---

## Appendix A: Comparison with DAP 1.0

| Aspect | DAP 1.0 (Eliminated) | DAP 2.0 |
|--------|---------|---------|
| **Primary payload** | Audio (WAV) | Text (Tier 1) / PCM audio (Tier 2) |
| **Transport** | Custom TCP binary | WebSocket (all tiers, same port as WebUI) |
| **Streaming** | No | Yes (text deltas for Tier 1, TTS audio for Tier 2) |
| **Wake word** | No (button) | Yes (Tier 1) / No (Tier 2, button) |
| **VAD** | No (button) | Silero (Tier 1) / Energy-based (Tier 2) |
| **ASR location** | Server only | Local (Tier 1) / Server (Tier 2) |
| **TTS location** | Server only | Local (Tier 1) / Server (Tier 2) |
| **Offline mode** | No | Yes (Tier 1) / No (Tier 2) |
| **Bandwidth** | ~1 MB/interaction | ~1 KB (Tier 1) / ~320 KB (Tier 2) |
| **Server threads** | Dedicated TCP server | Shared WebUI WebSocket (no extra server) |
| **Music** | Not supported | Opus streaming (Tier 1) / Not in v1 (Tier 2) |

## Appendix B: Related Documents

- `remote_dawn/protocol_specification.md` - DAP 1.0 specification (historical, protocol eliminated)
- `dawn_multi_client_architecture.md` - Multi-client threading design
- `ARCHITECTURE.md` - DAWN system architecture
- `LLM_INTEGRATION_GUIDE.md` - LLM setup and configuration
- `WEBUI_DESIGN.md` - WebUI architecture

---

**Document History**:
- v0.1 (Nov 2025): Initial draft
- v0.2 (Feb 2026): Revised to use WebSocket for Tier 1, extend existing session manager
- v0.3 (Feb 2026): Updated to reflect implementation (Phase 0-2 complete), clarified Tier 1 is VAD-only (no button/NeoPixels), Tier 2 retains button and NeoPixel support
- v0.4 (Feb 2026): Phase 0-3 complete. Updated protocol messages to match implementation (stream_start/delta/end). Updated common/ structure (ASR engine, Vosk, unified logging). Phase 3.5 touchscreen UI in planning.
- v0.5 (Feb 2026): Phase 3.5 in progress. Core visualization complete (orb, rings, FFT spectrum bars, KMSDRM). Transcript panel complete (streaming, date/time, WiFi, status detail). Touch gestures and overlays remaining.
- v0.6 (Feb 2026): Touch scroll complete (drag-to-scroll with auto-scroll management). Settings panel complete (server info, connection status, device identity, IP, uptime, session duration). TTS playback queue (producer-consumer pipelining). Emoji stripping unified (display + TTS, full SMP coverage 0x1F000-0x1FFFF). Transcript capacity increased (40 entries, 4KB text).
- v0.7 (Feb 2026): Music player panel complete (Playing/Queue/Library tabs, transport controls, visualizer, Opus audio streaming). Brightness and volume sliders in settings panel. Software dimming fallback for HDMI displays (sysfs backlight + SDL overlay). Paginated library browsing (50 items/page). Daemon-side satellite auth whitelist for music message handlers. Persistent brightness/volume settings across restarts.
- v0.8 (Feb 2026): **Major architecture change** — Tier 2 (ESP32) now uses WebSocket + PCM audio instead of custom binary protocol. This completely eliminates DAP1. Both tiers connect to the same WebUI WebSocket port. Tier 2 reuses the WebUI's existing audio pipeline (binary message types 0x01/0x02/0x11/0x12, audio buffer, worker threads, sentence-level TTS streaming). Raw PCM at 16kHz for v1 (ADPCM optional for future v2). No music on Tier 2 v1.
- v0.9 (Feb 2026): Screensaver/ambient mode complete. Two modes: clock (time/date with Lissajous drift, D.A.W.N. corner watermarks) and fullscreen rainbow FFT visualizer (64 Goertzel bins, dB-scale matching WebUI, peak hold, gradient reflections, two-line track info pill). Configurable idle timeout, manual trigger via visualizer tap, wake-word-only dismissal.
- v1.0 (Feb 2026): Settings toggle for 12/24-hour time format (persisted, updates both clocks). Music progress bar now compensates for ring buffer + ALSA output latency (~680ms) so displayed position matches speaker output. Seek slider no longer snaps back during drag (incoming server position updates suppressed while seeking).
- v1.1 (Feb 2026): **Phase 3.5 COMPLETE.** 5-theme system (Cyan, Purple, Green, Blue, Terminal) with dot picker in settings, 200ms ease-out crossfade, per-theme accent/background/text colors, TOML persistence. SDL2_gfx circle primitives for smoother orb rendering. Music playback architecture replaced: lock-free SPSC ring buffer with LWS-thread drain eliminates relay thread and fixes underruns. ALSA start_threshold prevents initial underruns.
- v1.2 (Feb 2026): **Phase 4 COMPLETE.** Tier 2 ESP32-S3 satellite implemented as Arduino sketch (`dawn_satellite_arduino/`). Adafruit ESP32-S3 TFT Feather with MAX98357 I2S amp, analog mic, push button, NeoPixels. Power-of-two ring buffer with spinlock, 22050→48kHz resampling, TCP backpressure via webSocket.loop() gating, NVS-persistent UUID and reconnect_secret, TFT centered status display, gitignored arduino_secrets.h for credentials. Daemon hardened: queue head-of-line blocking fix, fragment buffer realloc, memory leak on queue-full.
