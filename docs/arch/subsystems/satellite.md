# DAP2 Satellite Subsystem

Source: `dawn_satellite/`, `dawn_satellite_arduino/`, `common/`, `src/webui/webui_satellite.c`, `src/auth/auth_db_satellite.c`

Part of the [D.A.W.N. architecture](../../../ARCHITECTURE.md) — see the main doc for layer rules, threading model, and lock ordering.

**References**: [WEBSOCKET_PROTOCOL.md](../../WEBSOCKET_PROTOCOL.md) | [DAP2_SATELLITE.md](../../DAP2_SATELLITE.md) | [Arduino sketch README](../../../dawn_satellite_arduino/README.md)

---

**Purpose**: WebSocket protocol for satellite devices (Raspberry Pi Tier 1, ESP32 Tier 2).

## Unified Protocol

DAP2 is the unified WebSocket protocol for all remote access to the DAWN daemon. **A single WebSocket server on port 3000 serves all three client types** — browser WebUI, Tier 1 satellites (Raspberry Pi), and Tier 2 satellites (ESP32). There are no separate servers or additional ports. Each client connects to the same endpoint, registers its capabilities, and the daemon routes messages accordingly:

| Client     | Hardware | Transport                     | Server does     | Use Case                      |
| ---------- | -------- | ----------------------------- | --------------- | ----------------------------- |
| **WebUI**  | Browser  | Opus audio (48kHz) + JSON     | ASR + LLM + TTS | Browser voice/text interface  |
| **Tier 1** | RPi 4/5  | JSON text (`satellite_query`) | LLM only        | Hands-free (local ASR/TTS)    |
| **Tier 2** | ESP32-S3 | Binary PCM audio (16kHz)      | ASR + LLM + TTS | Push-to-talk (server ASR/TTS) |

This unified architecture means the session manager, response queue, LLM pipeline, tool system, and conversation history are shared infrastructure — adding a new client type requires only a registration handler and a routing decision, not a new server.

**Tier 2 audio reuses the WebUI audio subsystem** (see [webui-audio.md](webui-audio.md)) — same binary message types, same `webui_audio.c` worker threads, same ASR→LLM→TTS pipeline. The only difference is raw PCM at 16kHz instead of Opus at 48kHz (no codec or resample step on the server). TTS audio is sent at native Piper rate (22050Hz); the ESP32 resamples to 48kHz for I2S output. Music streaming (Opus over a dedicated WebSocket) is Tier 1 only, and runs under the same **closed-loop flow control** as the browser: the satellite reports its decoded-buffer depth (`music_buffer`) so the server's pacer holds a ~2s lead instead of free-running. The dedicated socket is the sole music transport — there is no main-WS music fallback.

**Tier 2 implementation**: `dawn_satellite_arduino/` — Arduino sketch for Adafruit ESP32-S3 TFT Feather. Uses arduinoWebSockets (Links2004), power-of-two ring buffer in PSRAM with spinlock producer/consumer, NVS-persistent UUID and reconnect_secret, TFT status display, NeoPixel state feedback. Credentials in gitignored `arduino_secrets.h`.

## Architecture: Local ASR/TTS + Remote LLM (Tier 1)

Tier 1 satellites handle speech recognition and text-to-speech locally and send only text to the daemon over WebSocket. Tier 2 satellites stream raw audio to the daemon for server-side processing.

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
│  webui_music.c: music control, unified library browse, Opus stream   │
└─────────────────────────────────────────────────────────────┘
```

## Key Components

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
   - Periodic `music_buffer` reports (closed-loop flow control, ~200 ms cadence)
   - Goertzel FFT analysis on live audio for visualizer

- **`dawn_satellite/src/ui/sdl_ui.c`, `dawn_satellite/include/sdl_ui.h`**: Touchscreen UI coordinator (SDL2 + KMSDRM)
   - Animated orb visualizer with state-driven colors
   - Scrollable markdown transcript with word-wrap
   - Touch gesture system (swipe, tap, long-press)
   - Status bar icon pattern for feature access (see below)
   - Companion UI modules in `dawn_satellite/src/ui/`: `ui_orb`, `ui_transcript`, `ui_markdown`, `ui_touch`, `ui_theme`, `ui_screensaver`, `ui_alarm`, `ui_slider`

- **`dawn_satellite/src/ui/ui_music.c/h`**: Music control panel
   - Three-tab layout: Playing / Queue / Library
   - Transport controls, seek bar, FFT visualizer
   - Paginated library browsing with artist/album drill-down

- **`dawn_satellite/src/ui/backlight.c/h`**: Display brightness control
   - sysfs backlight control for DSI displays
   - Software dimming fallback for HDMI displays

**Daemon side** (`src/webui/`, `src/auth/`):

- **webui_satellite.c**: Satellite message handlers
   - Registration with DB lookup for persistent user mapping
   - Query routing, streaming response relay
   - Session management with UUID-based identification
   - HA area + room sent with the satellite's standing directions (`append_satellite_context`, `src/core/prompt_builder.c`)
   - Satellite auth whitelist for music WebSocket messages

- **webui_admin_satellite.c**: Admin satellite management (CRUD)
   - List/update/delete satellite-to-user mappings (admin-only)
   - HA area assignment (dropdown from entity cache when HA enabled)
   - Force-disconnect on config change (satellite picks up new config on reconnect)

- **auth_db_satellite.c**: Satellite mapping persistence (`satellite_mappings` table)
   - Upsert, get, delete, update user/location, list with callback
   - Auto-registration on first connect (user_id=NULL, populated via admin panel)

**Shared** (`common/`):

- **common/src/asr/**: ASR engine abstraction (Whisper batch, Vosk streaming)
- **common/src/tts/**: Piper TTS with preprocessing and emoji stripping
- **common/src/asr/vad_silero.c**: Silero VAD with ONNX runtime
- **common/src/logging_common.c**, `common/include/logging_common.h`: `DAWN_LOG_INFO/ERROR/WARNING` macros

## DAP2 Protocol Messages

| Type                     | Direction          | Purpose                                  |
| ------------------------ | ------------------ | ---------------------------------------- |
| `satellite_register`     | Satellite → Daemon | Registration with UUID, name, location   |
| `satellite_register_ack` | Daemon → Satellite | Session ID, reconnect secret, music token |
| `satellite_query`        | Satellite → Daemon | User's transcribed text                  |
| `stream_start`           | Daemon → Satellite | Streaming response begins                |
| `stream_delta`           | Daemon → Satellite | Partial response text                    |
| `stream_end`             | Daemon → Satellite | Response complete                        |
| `satellite_ping`         | Satellite → Daemon | App-level keep-alive                     |
| `list_satellites`        | Admin → Daemon     | Request all satellite mappings + status  |
| `update_satellite`       | Admin → Daemon     | Update user assignment or HA area        |
| `delete_satellite`       | Admin → Daemon     | Remove satellite mapping from DB         |
| `music_control`          | Satellite → Daemon | Play/pause/stop/next/prev/seek           |
| `music_library`          | Satellite → Daemon | Browse artists/albums/tracks (paginated) |
| `music_state`            | Daemon → Satellite | Playback state update                    |

## Data Flow (Satellite Voice Command)

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

## Satellite UI Design Patterns

**Status Bar Icon Pattern**: Features are accessed via small icons in the transcript status bar (top-right area), not via a dedicated quick actions panel. This mirrors the WebUI's icon-bar approach.

- Icons sit inline in the existing status bar alongside WiFi and date/time.
- Each icon is an 18x18 SDL primitive texture with a 48x48 hit area (Apple HIG minimum for touch).
- Default color: secondary text color; active state: cyan accent.
- Rendering technique: build icon as a **white texture**, then tint at draw time via `SDL_SetTextureColorMod()` — one cached texture serves both idle and active color states without re-rendering.
- Icons only appear when the feature is available (e.g., music icon requires Opus support). No placeholder or "coming soon" icons.
- Tapping an icon toggles its associated slide-in panel (e.g., music panel slides in from the right).

**Current icons**: Music (note glyph, toggles music panel).

**Panel system**: Two panel types remain:

- Settings panel: swipe down from top edge (hamburger indicator).
- Music panel: tap music icon in status bar (slides from right).

Swipe-up from the bottom edge is currently unassigned (reserved for future use).
