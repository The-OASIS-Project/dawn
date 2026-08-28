# WebUI Audio Subsystem

Source: `src/webui/webui_audio.c`, `src/webui/webui_always_on.c`, `src/tools/render_visual_tool.c`, `www/js/audio/`, `www/js/ui/`

Part of the [D.A.W.N. architecture](../../../ARCHITECTURE.md) — see the main doc for layer rules, threading model, and lock ordering.

---

**Purpose**: Bidirectional audio streaming for WebUI browsers and Tier 2 (ESP32) satellites, plus always-on voice mode and inline visual rendering.

This subsystem handles all server-side audio: decode incoming audio, run ASR, generate TTS, encode outgoing audio. The WebUI browser connects with **Opus** (48kHz); Tier 2 satellites connect with **raw PCM** (16kHz, skipping codec/resample). Both use the same binary WebSocket message types (0x01/0x02 audio in, 0x11/0x12 audio out) and the same worker thread pipeline in `webui_audio.c`. See [WEBSOCKET_PROTOCOL.md](../../WEBSOCKET_PROTOCOL.md) for the binary message type reference.

## Architecture: Opus Codec + WebCodecs API (WebUI Browser Path)

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

## Key Components

- **opus-worker.js**: Web Worker for encoding/decoding using WebCodecs API
   - Encodes browser microphone input (48kHz) to Opus frames
   - Decodes server TTS audio (Opus frames) to PCM for playback
   - Falls back to raw PCM if WebCodecs unavailable

- **Codec configuration**:
   - Sample rate: 48kHz (Opus native rate)
   - Channels: mono
   - Bitrate: adaptive (typically 24-32 kbps for voice)
   - Frame size: 20ms (960 samples at 48kHz)

- **Capability negotiation**:
   - Browser sends `audio_codecs: ["opus", "pcm"]` during WebSocket connect
   - Server selects best available codec
   - Graceful fallback to uncompressed PCM if Opus unavailable

## Data Flow (Browser Voice Input)

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

## Benefits of Opus Streaming

| Metric               | Raw PCM (16-bit) | Opus Compressed                     |
| -------------------- | ---------------- | ----------------------------------- |
| Bandwidth (1s audio) | ~192 KB          | ~3-4 KB                             |
| Latency              | Minimal          | +2-5ms encoding                     |
| Quality              | Lossless         | Near-lossless (voice optimized)     |
| Browser Support      | Universal        | WebCodecs (Chrome/Edge/Firefox 90+) |

---

## Multi-Target TTS (Cross-Viewer)

Source: `webui_audio.c` (`webui_sentence_audio_fanout_callback`), `webui_broadcasts.c` (`for_each_user_conn`).

A turn's synthesized speech is **not bound to the connection that started it**. When more than one browser
views the same conversation, the reply is spoken on **every TTS-enabled WEBUI browser** viewing it — the
origin included if its own TTS is on. This is the audio half of server-authoritative multi-viewer fan-out
(the transcript/tool-step halves are covered by the persistence subsystem; see
[SERVER_AUTHORITATIVE_PERSISTENCE_DESIGN.md](https://github.com/The-OASIS-Project/atlas/blob/main/dawn/archive/SERVER_AUTHORITATIVE_PERSISTENCE_DESIGN.md) §12i).

**The one invariant that makes it cheap:** synthesis (`text_to_speech_to_pcm`, under the global
`tts_mutex`) runs **once per sentence** regardless of how many browsers listen. The 22kHz→48kHz resample
and the Opus encode also run once each and are **shared** across all recipients of that format (a browser
without WebCodecs reuses the same 48kHz PCM buffer). So adding listeners costs one memcpy + queue per
listener, never another synthesis — synthesis load stays flat vs. viewer count.

**Per-sentence flow:**

```
1. Phase-1 scan  — walk the connection registry: which recipients exist, which formats (Opus / PCM48)?
   ↓  (registry lock released)
2. Synthesize the sentence PCM ONCE   (tts_mutex)
   ↓
3. Resample 22k→48k ONCE; Opus-encode ONCE   (only the formats the scan found)
   ↓
4. Phase-2 send  — re-walk the registry UNDER the lock, re-validating each recipient, and queue its
                   format's bytes. Queuing under the registry lock (which session-destroy also holds to
                   purge the queue) is what makes a mid-synth disconnect safe — no dangling send.
```

- **Membership** is one shared predicate, `conn_is_audio_target`: TTS-enabled, WEBUI, and either the origin
  (unconditional, so it hears its own reply regardless of which conversation it is currently viewing) or a
  non-origin viewer whose active conversation matches this turn's.
- **State bracket:** `state:speaking` fans per sentence and `state:idle` fans once at turn end (from each
  worker's teardown funnel) to the **same** recipient set, so a bystander's speaking indicator opens and
  closes cleanly.
- **Arming:** a turn synthesizes when the origin has TTS on **or** any other speaker-capable viewer exists,
  so a silent origin (someone typing with TTS off) still speaks the reply on a listener's device.
- **Reinvoke and Tier-2 satellites keep the single-target path** (`webui_sentence_audio_callback`): a
  background-job re-engagement picks exactly one viewer (§6d), and a satellite speaks its own turn.
- **v1 scope is browser-to-browser.** A non-origin Tier-2 satellite is not yet a fan target — a satellite
  has no per-conversation membership signal to match (`webui_get_active_conversation_id` returns 0 for
  non-WEBUI sessions). A satellite that *originates* a turn is unaffected.

---

## Always-On Voice Mode

Source: `src/webui/webui_always_on.c`, `www/js/audio/always-on.js`

**Purpose**: Continuous wake word listening via WebUI browser — local client parity.

The always-on subsystem enables hands-free voice interaction from the browser, matching the local microphone's wake word detection without any browser-side AI models. The browser streams audio continuously; all VAD and wake word detection runs server-side.

### State Machine

```
DISABLED → LISTENING → WAKE_CHECK → WAKE_PENDING → PROCESSING → LISTENING
                  ↑        ↓                              ↓
                  ↑   RECORDING → PROCESSING ─────────────┘
                  ↑        (wake word only, no inline command)
                  └────────────────────────────────────────┘
```

- **LISTENING**: VAD monitors for speech onset (Silero VAD, 32ms chunks at 16kHz)
- **WAKE_CHECK**: Speech detected — buffer audio, wait for 1.5s silence, then dispatch ASR
- **WAKE_PENDING**: ASR running on worker thread (async)
- **RECORDING**: Wake word confirmed but no inline command — record follow-up command
- **PROCESSING**: Command sent to LLM; audio muted to prevent TTS echo

### Key Design Decisions

- **Server-side VAD + wake word**: no production voice assistant runs wake word detection in the browser. Server-side matches the proven local session architecture.
- **Shared `wake_word.c`**: same wake word matching logic used by local mic (`dawn.c`) and satellites.
- **Audio chunk cadence**: browser sends audio in configurable chunks (default 100ms via `audio_chunk_ms`). Smaller chunks reduce end-of-speech detection jitter.
- **TTS echo prevention**: audio capture is muted during PROCESSING state and deferred until TTS playback completes.
- **Per-connection context**: each WebSocket connection gets its own VAD, Opus decoder, resampler, and circular buffer. No shared state between clients.

### Unified Action Button (Browser UI)

The WebUI uses a single split button with a dropdown for input mode selection:

| Mode | Button Behavior | Events |
|------|----------------|--------|
| **Send Text** (default) | Click sends text | `click` → `handleSend()` |
| **Hold to Talk** | Hold to record, release to send | `mousedown`/`mouseup` → start/stop capture |
| **Continuous Listening** | Click toggles always-on | `click` → `toggle()` |

A single `resolveButtonState()` function determines the button label with priority: Cancel (red, during processing) > PTT recording > text override > mode-specific label. Smart typing override temporarily shows "Send" when text is present in any voice mode.

---

## Visual Rendering Tool

Source: `src/tools/render_visual_tool.c`, `www/js/ui/visual-render.js`

**Purpose**: Inline SVG/HTML diagrams generated by the LLM during conversation.

The visual rendering tool enables the LLM to generate flowcharts, architecture diagrams, data charts, interactive widgets, UI mockups, and illustrations directly in the conversation. It uses the **two-step instruction loader pattern**: the LLM loads design guidelines from markdown files on disk before generating visuals, keeping the system prompt lightweight.

### Architecture

```
LLM calls render_visual_load_guidelines(modules)
    ↓
instruction_loader.c reads _core.md + module.md from tool_instructions/
    ↓
Guidelines returned to LLM context (~10KB of design rules)
    ↓
LLM calls render_visual(title, type, code)
    ↓
render_visual_tool.c wraps code in <dawn-visual> tag
    ↓
webui_server.c sends as role:"visual" + stashes pending_visual on session
    ↓
dawn.js renders immediately into streaming entry (splits text around visual)
    ↓
streaming.js finalize saves pre-visual + <dawn-visual> + post-visual to DB
    ↓
On replay, transcript.js extractVisuals splits and renders inline
```

### Key Components

- **instruction_loader.c/h**: generic two-step loader — reads `_core.md` + comma-separated modules from `tool_instructions/{tool}/`. Pre-scan allocation, path traversal sanitization, 128KB cap. Reusable by any future two-step tool.
- **render_visual_tool.c**: two tool registrations (`render_visual_load_guidelines` + `render_visual`). Title sanitization, `</dawn-visual>` tag breakout prevention, JSON parsing of `details` parameter.
- **visual-render.js**: sandboxed iframe rendering (`sandbox="allow-scripts"`), theme CSS injection from computed parent variables, 9 color ramps with automatic light/dark mode, vendor script inlining (Chart.js cached and embedded), ResizeObserver height auto-sizing via debounced postMessage, `sendPrompt()` bridge with WeakSet source validation, download button.
- **Guideline modules** (`tool_instructions/render_visual/`): `_core.md` (universal design system), `diagram.md`, `chart.md`, `interactive.md`, `art.md`, `mockup.md`.
- **History persistence**: server stashes `pending_visual` on session (protected by `tools_mutex`), client interleaves visual content between pre/post text in streaming save, replay splits at tag boundary for inline positioning.
