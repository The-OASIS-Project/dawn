# DAWN Android Satellite Client Design

**Always-on FRIDAY in your ear, via Android phone + Bluetooth/wired earpiece**

| Field | Value |
|---|---|
| Date | May 2026 |
| Status | Design / Pre-implementation — no Android client exists yet (checked 2026-11-27) |
| Target | Android phone (Android 8+) acting as a DAP2 satellite client |
| MVP Tier | Tier 2 (raw PCM streaming, server does ASR/LLM/TTS) |
| Future | Tier 1 path (on-device ASR/TTS via Sherpa-ONNX) — same app, swappable pipeline |
| Dev machine | x86_64 Linux laptop |

---

## Overview

A native Android app that acts as a DAP2 satellite client to the DAWN daemon, letting the user wear FRIDAY in their ear via a Bluetooth or wired headset all day for testing and daily use. The MVP is **Tier 2** — the phone captures raw 16 kHz PCM and streams it over WebSocket to the daemon, which runs ASR + LLM + TTS and streams audio frames back. This matches the existing ESP32 satellite path, so the server side requires zero new code.

The app is architected with a clean **`VoicePipeline`** abstraction so a future **Tier 1** implementation (on-device VAD/ASR/TTS via Sherpa-ONNX) can slot in behind the same interface without rewriting the app shell, transport, audio I/O, or UI. Tier migration is a runtime toggle that flips one field in the registration handshake.

Voice-first MVP. Text input, transcript history, and other affordances arrive in later phases.

---

## Why Tier 2 first

| | Tier 2 (MVP) | Tier 1 (future) |
|---|---|---|
| Server work | None — reuses ESP32 path | None — reuses RPi path |
| Client effort | ~1-2 weeks | ~3-4 weeks (Sherpa-ONNX integration) |
| Bandwidth | ~256 kbps PCM up | bytes per query (text only) |
| Battery | Modest (radio + audio I/O) | Higher (on-device ONNX inference) |
| Privacy | Raw audio leaves phone | Only transcribed text leaves phone |
| Offline tolerance | Fails on network drop | Survives short drops |
| APK size | ~10-20 MB | ~100-150 MB (ONNX runtime + models) |

Tier 2 ships fast and validates the whole loop. Tier 1 is the daily-driver upgrade once the basic surface is proven.

---

## Open decisions (pre-baked recommendations)

The following decisions are baked into the design below. Any of them can be revised in the next session before code is written.

| # | Decision | Recommendation | Rationale |
|---|---|---|---|
| 1 | Repo location | New repo `dawn_satellite_android/` as sibling to `dawn_satellite/` and `dawn_satellite_arduino/` at OASIS project root, **separate Git repo** (not in `dawn/`) | Matches existing satellite naming. Separate repo keeps Android build artifacts (Gradle, AAB, AAR) out of the daemon repo. The Arduino satellite is similarly its own repo. |
| 2 | Min SDK | Android 8.0 (API 26) | Covers >95% of devices; has all foreground service APIs (incl. `microphone` type). Older SDKs cost disproportionate effort. |
| 3 | TLS path | Mirror the Google OAuth FQDN pattern — Let's Encrypt cert on the Jetson FQDN, phone connects via `wss://<fqdn>:3000/ws` | Avoids pinned-cert + private-CA distribution problem. Already proven for OAuth. |
| 4 | UUID provisioning | First-launch client-side UUIDv4 (matches ESP32 pattern), persisted in EncryptedSharedPreferences | Server-assigned would require a new flow; client-side matches existing satellites. |
| 5 | UI framework | Jetpack Compose | Modern Android default, less boilerplate, Compose-native state flow integration. |
| 6 | Audio I/O | `AudioRecord` + `AudioTrack` for MVP | Pure Kotlin, no NDK. Migrate to Oboe only if BT latency demands it. |
| 7 | Dev machine | x86_64 Linux laptop | Confirmed by user. Keeps Jetson focused on the daemon. |

If any of these change, update this doc before starting the project skeleton.

---

## Goals & non-goals

**Goals (MVP):**
- Push-to-talk voice interaction with FRIDAY via DAP2 over WebSocket
- Foreground service for stable always-on operation
- Bluetooth headset (HFP/SCO) and wired earpiece support
- Automatic reconnection across network changes
- Device-token authentication reusing existing `auth_db` flow
- Self-contained: no manual ADB pushes, no platform-specific hacks

**Non-goals (MVP):**
- Wake-word ("hey friday") detection — Phase 3
- Text input / transcript history — Phase 4
- On-device ASR/TTS — Phase 5 (Tier 1)
- Music streaming visualization
- WebUI feature parity (settings panel, calendar, etc.)
- Play Store distribution (sideload-only initially)

---

## Design principle

**One app, one WebSocket connection, swappable voice pipeline.** The transport layer, audio I/O, foreground service, UI, auth, and reconnection logic are all tier-agnostic. What changes between Tier 2 and Tier 1 is a single concrete `VoicePipeline` implementation and the registration handshake's `tier` field.

---

## Module layout

```
dawn_satellite_android/
├── app/                              # UI + lifecycle (Kotlin + Compose)
│   ├── ui/{main,settings,auth}
│   ├── service/FridayForegroundService.kt
│   └── FridayApplication.kt
│
├── core-transport/                   # DAP2 over WebSocket
│   ├── Dap2Client.kt                 # OkHttp WS, reconnect, ping, lifecycle
│   ├── Dap2Protocol.kt               # registration, message routing
│   ├── BinaryFrame.kt                # 0x01 AUDIO_IN, 0x02 AUDIO_IN_END, 0x10 AUDIO_OUT
│   └── messages/                     # @Serializable client + server data classes
│
├── core-audio/                       # AudioRecord + AudioTrack wrappers
│   ├── AudioCapture.kt               # 16 kHz S16LE mono, 20 ms frames (640 B)
│   ├── AudioPlayback.kt              # Dynamic-rate (22050 Hz TTS / 16 kHz mic monitor)
│   └── BluetoothAudioRouter.kt       # SCO setup + hot-swap state machine
│
├── core-voice/                       # ★ The swappable seam
│   ├── VoicePipeline.kt              # interface
│   ├── VoiceEvent.kt                 # sealed class
│   ├── tier2/Tier2VoicePipeline.kt   # MVP: PCM up, audio frames down
│   └── tier1/                        # (empty until Phase 5)
│       └── Tier1VoicePipeline.kt
│
├── core-auth/                        # Device token + login
│   ├── AuthRepository.kt
│   └── DeviceTokenStore.kt           # androidx.security EncryptedSharedPreferences
│
└── core-wake/                        # (empty until Phase 3)
    └── WakeWordDetector.kt           # interface; Porcupine impl later
```

---

## The seam — `VoicePipeline`

The single abstraction that lets Tier 1 slot in later without app-layer rewrites.

```kotlin
interface VoicePipeline {
    val state: StateFlow<PipelineState>      // Idle, Listening, Streaming,
                                             // Thinking, Speaking, Error
    val events: SharedFlow<VoiceEvent>       // Sealed class:
                                             //   PartialTranscript(text)
                                             //   FinalTranscript(text)
                                             //   AssistantStreamDelta(text)
                                             //   AssistantSpeakingStart
                                             //   AssistantSpeakingEnd
                                             //   ToolRunning(name)
                                             //   Error(reason)

    suspend fun startUtterance()             // PTT-down or wake-triggered
    suspend fun stopUtterance()              // PTT-up or VAD-end
    suspend fun shutdown()
}
```

**`Tier2VoicePipeline`** (MVP) consumes `AudioCapture` PCM frames, ships them as `0x01` binary frames, ships `0x02` on stop, and renders incoming `0x10` audio frames via `AudioPlayback`. JSON state/transcript events are forwarded as `VoiceEvent`s.

**`Tier1VoicePipeline`** (future) consumes the same `AudioCapture`, runs local Silero VAD + Whisper/Vosk, sends `satellite_query` text messages over the same WebSocket, runs local Piper on streaming text deltas. App layer is none the wiser.

The app and UI never depend on which pipeline is active — they only see `VoicePipeline`.

---

## MVP wire flow (push-to-talk)

```
[PTT down]
  AudioCapture.start()                                   # 16 kHz S16LE mono
  └─ 20 ms PCM frames (640 B = 320 samples * 2 bytes)
     └─ Dap2Client.sendBinary(0x01 + pcm_chunk)
  ... loop while PTT held ...

[PTT up]
  Dap2Client.sendBinary(0x02 + empty)                    # AUDIO_IN_END
  AudioCapture.stop()

[server] (no client-side change required):
  ASR (Whisper) → LLM (streaming) → TTS (Piper sentence-buffered)
  emits stream_start, stream_delta×N, audio_out frames (0x10),
        state, transcript, stream_end

[client]:
  AudioPlayback queues incoming 22050 Hz raw PCM frames
  UI updates from state/transcript JSON events
```

**Frame sizing:** 20 ms frames are a good default — small enough for low latency, large enough that WebSocket overhead doesn't dominate. Existing ESP32 client uses similar sizing.

---

## Connection lifecycle

```
[App start, foreground service launched]
  Dap2Client.connect(url, token)
    ↓
  WebSocket onOpen
    ↓
  send satellite_register {
    uuid: <persisted UUIDv4>,
    name: "Phone — <model>",
    tier: 2,
    capabilities: { local_asr: false, local_tts: false, wake_word: false },
    reconnect_secret: <last cached secret, if any>
  }
    ↓
  receive satellite_register_ack { reconnect_secret: <new> }
    → cache new secret, mark pipeline Ready
    ↓
  every 10 s: send satellite_ping → receive satellite_pong

[Network drop / WS close]
  reconnect with exponential backoff: 1s, 2s, 4s, 8s, 30s (capped)
  only while foreground service alive
  on reconnect, replay registration with cached reconnect_secret
```

App-level `satellite_ping` every 10 s is required by the protocol (server's lws does not respond to WS-level pings reliably — see DAWN memory note on satellite ws_client).

---

## Foreground service

Android 14+ requires foreground services to declare a type. This service uses the combination:

```xml
<service
    android:name=".service.FridayForegroundService"
    android:foregroundServiceType="microphone|mediaPlayback|connectedDevice"
    android:exported="false" />
```

- `microphone` — required for background mic capture on Android 10+
- `mediaPlayback` — required for TTS audio output while screen off
- `connectedDevice` — required for Bluetooth headset audio routing

Notification surface:
- Persistent, non-dismissable while running
- Shows current state (Idle / Listening / Speaking / Disconnected)
- Quick actions: Mute mic, Disconnect, Open app

The service owns the `Dap2Client`, `AudioCapture`, `AudioPlayback`, and `VoicePipeline` instances — they survive Activity recreation. The UI Activity binds via `ServiceConnection` and observes flows.

---

## Bluetooth handling

The messy part. SCO setup is async (1-2 s on most phones), quality depends on headset profile (HFP narrowband 8 kHz vs mSBC/LC3 wideband 16 kHz), and SCO + A2DP simultaneity is buggy on many devices.

**Approach:**
- `BluetoothAudioRouter` exposes `StateFlow<AudioRoute>`: Internal | BluetoothSco | WiredHeadset | Speaker
- Detects connected BT headsets via `AudioManager.getDevices(GET_DEVICES_INPUTS)` filtering `TYPE_BLUETOOTH_SCO`
- State machine for SCO setup: Requested → Connecting → Active → Disconnecting
- Re-routes `AudioRecord` / `AudioTrack` via `setPreferredDevice()` on hot-swap (headphone yanked / BT reconnected)
- Settings: device picker, "always prefer BT headset when available" toggle

**Testing strategy:** wired USB-C earbud first (sidesteps SCO entirely), then add BT support once the rest of the audio path is proven.

---

## Authentication

Reuses the existing DAWN `auth_db` device-token mechanism.

**First launch:**
1. App detects no stored device token → shows Login screen
2. User enters daemon URL (`wss://dawn.example.com:3000/ws`) + username + password
3. App POSTs to existing DAWN auth HTTP endpoint → receives device token
4. Token stored in `EncryptedSharedPreferences` (Android Keystore-backed)
5. Generate UUIDv4 for this device, persist alongside token

**Subsequent launches:**
- Read token + UUID from EncryptedSharedPreferences
- Open WebSocket with token (transport mechanism: see protocol verification step in next session — likely URL query param or first-message authenticate)
- Re-register with cached `reconnect_secret`

---

## Tech stack

| Concern | Choice | Notes |
|---|---|---|
| Language | Kotlin only (MVP) | No NDK until Tier 1 |
| Min SDK | 26 (Android 8.0) | All foreground service APIs |
| Target SDK | 34 (Android 14) | Current at time of writing |
| UI | Jetpack Compose | Material 3 theming, Compose Navigation |
| Async | Kotlin Coroutines + Flow | Fits WS event streams naturally |
| WebSocket | OkHttp 4.x | Native binary frame support |
| JSON | kotlinx.serialization | Compile-time codegen, type-safe |
| Audio | `AudioRecord` + `AudioTrack` | Simplest. Migrate to Oboe (NDK) only if needed |
| Encrypted storage | androidx.security:security-crypto | Keystore-backed `EncryptedSharedPreferences` |
| DI | Hilt (app), constructor injection (libs) | Standard 2026 Android |
| Logging | Timber | Tagged, debuggable |
| Build | Gradle KTS + version catalog | Standard |

---

## Phasing

### Phase 1 — MVP (1-2 weeks)

- Project skeleton: Gradle KTS, Compose, Hilt, version catalog, multi-module
- `Dap2Client` with reconnection + ping
- `AudioCapture` (16 kHz S16LE mono, 20 ms frames)
- `AudioPlayback` (dynamic-rate 22050 Hz / 16 kHz)
- `Tier2VoicePipeline` push-to-talk implementation
- `FridayForegroundService` with notification + quick actions
- Login screen → device-token storage
- Main UI: large PTT button, status indicator, latest assistant response
- Settings: daemon URL only

**Exit criteria:** can hold PTT, hear FRIDAY respond through wired USB-C earbud, app survives screen lock and reconnects after network blip.

### Phase 2 — Daily-driver hardening (3-5 days)

- `BluetoothAudioRouter` SCO setup + hot-swap
- Settings: audio device picker, "prefer BT headset" toggle
- Network-change-aware reconnection (Wi-Fi → cellular handoff)
- Notification quick actions wired (mute, disconnect)
- Battery profiling pass

**Exit criteria:** can wear a BT headset all afternoon without manual intervention.

### Phase 3 — Wake word (3-5 days)

- Porcupine SDK ("friday" custom keyword, free for personal use)
- Local Silero VAD via ONNX Runtime (gates streaming on speech only)
- Always-on capture mode toggle
- Visual privacy indicator (notification icon flips when streaming)

**Exit criteria:** "Hey FRIDAY, what time is it?" works without touching the phone.

### Phase 4 — Text input + transcript view (2-3 days)

- Text composer (uses existing WebUI text message types — DAP2 supports it)
- Conversation transcript view with history
- Conversation reset button

### Phase 5 — Tier 1 (3-4 weeks, separate effort)

- Sherpa-ONNX dependency (Apache 2.0, single AAR — bundles VAD + ASR + Piper TTS)
- Model bundling: Whisper tiny.en or Vosk small en + Piper voice + Silero VAD
- `Tier1VoicePipeline` implementation: local VAD/ASR/TTS, sends `satellite_query` text
- Settings toggle: "Process speech on device"
- Same app, same WS connection — registration handshake flips `tier=1` and capabilities

---

## Dev environment requirements

**On the laptop (x86_64 Linux):**

| Tool | Source | Notes |
|---|---|---|
| Android Studio | https://developer.android.com/studio | Bundles JDK 17, Gradle, SDK manager. ~4 GB |
| Android SDK Platform 34 | Via SDK Manager | Compile target |
| Android Build Tools 34 | Via SDK Manager | |
| Android Platform Tools | Via SDK Manager | `adb`, `fastboot` |
| `scrcpy` (optional) | `apt install scrcpy` | Screen mirror during dev |
| Git | system | Already installed |

**Disk budget:** ~25 GB total (Studio + SDK + Gradle caches accumulate over time).

**Not needed for MVP:** NDK, CMake, C++ toolchain, separate JDK install. All only relevant to Phase 5 (Tier 1) when Sherpa-ONNX integration begins.

**On the phone:**
- Developer Options enabled, USB debugging on
- Wireless debugging on (Android 11+) is convenient
- Same Wi-Fi as the Jetson, or daemon FQDN reachable from cellular if testing externally
- USB-C cable to the laptop
- A wired USB-C earbud for first-week testing

**On the Jetson (DAWN daemon side):**
- No changes required. Tier 2 client uses the existing ESP32 satellite path.
- Verify the daemon FQDN serves `wss://` correctly from the phone's network.

---

## Protocol references

For verification in the next session — these are the key spots in the existing DAWN codebase:

| What | Where |
|---|---|
| DAP2 wire protocol spec | `docs/WEBSOCKET_PROTOCOL.md` |
| Binary frame types (0x01 AUDIO_IN, 0x02 AUDIO_IN_END) | `docs/WEBSOCKET_PROTOCOL.md` binary frame table (~line 74) |
| `satellite_register` message shape | `docs/WEBSOCKET_PROTOCOL.md` § `satellite_register` |
| `satellite_query` text message (Tier 1 path) | `docs/WEBSOCKET_PROTOCOL.md` § `satellite_query` |
| `satellite_ping` keep-alive | `docs/WEBSOCKET_PROTOCOL.md` § `satellite_ping` |
| Satellite subsystem overview | `docs/arch/subsystems/satellite.md` |
| Tier 2 server-side audio handling | `src/webui/webui_audio.c` (`DAP2_TIER_2` branches, ASR input ~line 1459) |
| Tier 2 server-side TTS encoding (22050 Hz native) | `src/webui/webui_audio.c` (~line 969) |
| ESP32 reference client (Arduino/C++) | `dawn_satellite_arduino/` |
| Pi reference client (C/C++, Tier 1) | `dawn_satellite/` |

The ESP32 client is the closest existing reference for the Tier 2 audio path. The Pi client is the reference for the Tier 1 message flow when Phase 5 begins.

---

## Things to verify in the next session

These are details I didn't fully chase down before context handed off:

1. **Auth token transport on the WS** — URL query param? `Sec-WebSocket-Protocol` header? First-message authenticate? Need to read `webui_server.c` / `webui_session.c` to confirm before wiring login.
2. **HTTP auth endpoint shape** — exact endpoint path + JSON body for username/password → device token exchange. Verify against existing `auth_*` HTTP handlers.
3. **Tier 2 audio_out frame format details** — confirm whether 0x10 frames carry just raw 22050 Hz S16LE mono PCM or have a header. Cross-reference ESP32 client's playback code.
4. **DAP2 protocol for music streaming** — Tier 1 only per `satellite.md` line 25. Confirm whether voice client should reject music messages or just ignore.
5. **Reconnect secret rotation** — does the daemon issue a new secret on every register_ack, or only on first register? Affects EncryptedSharedPreferences write frequency.

---

## Status

**Pre-implementation.** Architecture agreed in design session 2026-05-07/08. Next step: spin up the Gradle skeleton on the laptop and start Phase 1.

As of 2026-11-27 no Android client has been built; this document is the design only.
