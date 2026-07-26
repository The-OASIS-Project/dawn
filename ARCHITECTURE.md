# D.A.W.N. System Architecture

**D.A.W.N.** (Digital Assistant for Workflow Neural-inference) is the central intelligence layer of the OASIS ecosystem. It interprets user intent, fuses data from every subsystem, and routes commands. DAWN acts as OASIS's orchestration hub for MIRAGE, AURA, SPARK, STAT, and any future modules.

This document is the **architectural map**: directory layout, the cross-cutting rules every subsystem obeys (layering, threading, lock ordering, error handling, configuration), and a one-paragraph summary of each subsystem with a link to its detail doc. For the internals of any single subsystem — its components, data flow, DB schema, and tuning — open the linked file in [`docs/arch/subsystems/`](docs/arch/subsystems/).

**Last updated**: June 2026.

## Table of Contents

- [Directory Structure](#directory-structure)
- [High-Level Overview](#high-level-overview)
- [Subsystem Index](#subsystem-index)
- [Module Dependency Hierarchy](#module-dependency-hierarchy)
- [Threading Model](#threading-model)
- [State Machine](#state-machine)
- [Mutex Lock Ordering Hierarchy](#mutex-lock-ordering-hierarchy)
- [Memory Management](#memory-management)
- [Error Handling](#error-handling)
- [File Organization Standards](#file-organization-standards)
- [Configuration Architecture](#configuration-architecture)
- [Performance Considerations](#performance-considerations)
- [DAP2 Satellite Protocol](#dap2-satellite-protocol)
- [Command Processing](#command-processing)
- [References](#references)

---

## Directory Structure

```
dawn/
├── src/                    # C/C++ source files
│   ├── asr/                # Speech recognition (Whisper, Vosk, VAD)
│   ├── llm/                # LLM integration (OpenAI, Claude, Gemini, local)
│   ├── memory/             # Persistent memory system
│   ├── tts/                # Text-to-speech (Piper)
│   ├── audio/              # Audio capture, playback, music
│   ├── core/               # Session manager, scheduler, embedding engine, crypto, OTA
│   ├── auth/               # User auth, per-user settings, satellite mappings
│   ├── messaging/          # Bidirectional chat channels (Telegram, Discord, Slack, SMS)
│   ├── tools/              # Modular LLM tools (search, weather, calendar, email, scheduler, etc.)
│   └── webui/              # Web UI server
│
├── include/                # Header files (mirrors src/)
├── common/                 # Shared library (VAD, ASR, TTS, logging, sentence buffer) for daemon + satellite
├── www/                    # Web UI static files (HTML, CSS, JS)
├── models/                 # ML models (TTS voices, VAD)
├── sound_assets/           # Notification chimes, ringtones, SFX
├── tool_instructions/      # Two-step instruction loader content (render_visual guidelines)
├── whisper.cpp/            # Whisper ASR engine (git submodule)
├── dawn_satellite/         # DAP2 Tier 1 satellite (Raspberry Pi, SDL2 UI)
├── dawn_satellite_arduino/ # DAP2 Tier 2 satellite (ESP32-S3, Arduino sketch)
├── dawn-admin/             # Admin CLI (socket client to daemon)
├── services/               # Systemd service files
├── scripts/                # Utility scripts (setup, tooling)
├── tests/                  # Unit and integration tests
├── benchmarks/             # Retrieval benchmark harness (LongMemEval, LoCoMo, ConvoMem)
├── llm_testing/            # LLM quality/latency benchmarking
├── docs/                   # Additional documentation
│   └── arch/               # Architecture detail docs (per-subsystem)
│
├── dawn.toml.example       # Configuration template
├── secrets.toml.example    # API keys template
└── CMakeLists.txt          # Build configuration
```

---

## High-Level Overview

DAWN is a modular voice assistant. A voice command flows through a pipeline of specialized subsystems:

```
┌─────────────────────────────────────────────────────────────┐
│                      DAWN Main Loop                         │
│  (src/dawn.c — State Machine: SILENCE → WAKEWORD → COMMAND  │
│   → PROCESSING)                                             │
└────────┬──────────────────────────────┬─────────────────────┘
         │ Local Audio                  │ WebSocket (WebUI + Satellites)
    ┌────▼─────────┐            ┌───────▼──────────┐
    │ Audio Capture│            │  WebUI Server    │
    │ Thread + RB  │            │ (libwebsockets)  │
    └────┬─────────┘            └───────┬──────────┘
         │                              │
    ┌────▼──────────┐           ┌───────▼──────────┐
    │  VAD (Silero) │           │ Session Manager  │
    └────┬──────────┘           │ + Audio Workers  │
         │                      └───────┬──────────┘
    ┌────▼──────────┐                   │
    │ ASR Interface │                   │
    │ (Vosk|Whisper)│                   │
    └────┬──────────┘                   │
         └───────────┬──────────────────┘
                     ▼
            ┌────────────────┐
            │ LLM Interface  │───► OpenAI / Claude / Gemini / llama.cpp
            └────────┬───────┘     (streaming)
                     │
            ┌────────▼────────┐
            │ SSE Parser +    │
            │ Sentence Buffer │
            └────────┬────────┘
                     │
            ┌────────▼────────┐
            │  TTS (Piper)    │───► ALSA / PulseAudio
            └─────────────────┘
```

### Core Design Principles

1. **Modularity**: each subsystem has a clear interface and can be replaced independently.
2. **Performance**: GPU acceleration on Jetson; optimized local LLM inference.
3. **Reliability**: retry logic, checksums, error recovery in network protocol.
4. **Flexibility**: multiple ASR engines, LLM providers, and audio backends.
5. **Embedded-first**: designed for resource-constrained platforms (static allocation preferred).

---

## Subsystem Index

Each row points to a detail doc in [`docs/arch/subsystems/`](docs/arch/subsystems/) that covers components, data flow, schemas, and tuning.

| Subsystem | Role | Detail doc |
|---|---|---|
| **Core** (`src/` root + `src/core/`) | Main entry, MQTT integration, legacy command parsing. `src/dawn.c` hosts the state machine; `src/mosquitto_comms.c/h` wires MQTT; `src/text_to_command_nuevo.c/h` extracts `<command>` tags from LLM output; `src/word_to_number.c/h` converts "twenty-three" → 23; `src/core/` contains the session manager, scheduler, command executor/router, worker pool, and wake-word detector. Logging macros (`LOG_INFO/WARNING/ERROR`) come from `common/include/logging.h`, shared with the satellite. | *(inlined above)* |
| **ASR** | Speech recognition abstraction (Strategy pattern) over Whisper and Vosk, plus Silero VAD and chunking for long utterances. Whisper on Jetson GPU is the default; Vosk is retained for CPU-only builds. | [asr.md](docs/arch/subsystems/asr.md) |
| **LLM** | Unified interface for OpenAI, Claude, Gemini, and local (llama.cpp/Ollama), plus an optional **OpenRouter gateway** (`[llm.cloud] use_openrouter`) that fronts all cloud traffic through one OpenAI-compatible endpoint with per-purpose model overrides. Streaming via SSE feeds a sentence buffer that hands complete sentences to TTS while the response is still generating. System prompts are composed in two segments — a stable prefix + a volatile tail (`src/core/prompt_compose.c`) — so providers can cache the prefix across turns. Runs on a dedicated worker thread so the main audio loop never blocks; wake-word interrupts abort in-flight API calls. | [llm.md](docs/arch/subsystems/llm.md) |
| **TTS** | Piper + ONNX Runtime with preprocessing for natural phrasing. Mutex-protected so the main loop, network server, and streaming buffer can all synthesize safely. | [tts.md](docs/arch/subsystems/tts.md) |
| **DAP2 Satellite** | WebSocket protocol for all remote clients: WebUI browser (Opus), Tier 1 Raspberry Pi (local ASR/TTS, text-only), and Tier 2 ESP32 (raw PCM). A single server on port 3000 serves all three — adding a new client type means a new registration handler, not a new server. | [satellite.md](docs/arch/subsystems/satellite.md) |
| **Satellite OTA** | Signed over-the-air updates for Tier 1 Pi (.deb) and Tier 2 ESP32 (device-apply). Releases are libsodium-signed binary manifests (TweetNaCl verify on the ESP32); the signing key lives offline and never on the daemon (`tools/ota_keytool.c`). Fleet rollout does a canary wave then deferred fan-out, driven off the main-loop 1-second heartbeat (no dedicated thread). WebUI fleet panel + `dawn-admin ota` CLI + runtime release rescan. | [OTA_DESIGN.md](docs/OTA_DESIGN.md) |
| **Audio** | Capture thread + thread-safe ring buffer, multi-format playback (FLAC/MP3/Ogg), and the unified music DB (local files + Plex) with source-aware dedup and background scanner. | [audio.md](docs/arch/subsystems/audio.md) |
| **WebUI Audio** | Browser-side Opus streaming via WebCodecs + server-side decode/resample/ASR/TTS/encode pipeline. Also hosts **always-on voice mode** (server-side VAD + wake word, no browser AI) and the **visual rendering tool** (inline SVG/HTML/Chart.js diagrams). | [webui-audio.md](docs/arch/subsystems/webui-audio.md) |
| **Vision & Documents** | Image upload (client compression, server filesystem storage with source/retention policies, zero-copy HTTP serving) and document upload (PDF via MuPDF, DOCX via libzip+libxml2, plain text client-side). | [vision-documents.md](docs/arch/subsystems/vision-documents.md) |
| **Memory** | Persistent user profile built by a **sleep-consolidation model**: extraction runs at session end, not during conversation, so chat latency is unchanged. Facts, preferences, summaries, entity graph, and contacts; hybrid keyword + semantic search via embeddings; nightly confidence decay. Conversation anchor on extraction (v42) resolves relative phrases ("yesterday", "last month") against the conversation's logical "now." | [memory.md](docs/arch/subsystems/memory.md). Historical design docs under [atlas/dawn/memory/](https://github.com/The-OASIS-Project/atlas/tree/main/dawn/memory). |
| **Document Search / RAG** | Upload → chunk → embed → search, literal `document_grep`, or paginated read via LLM tools. Structure-aware chunking (one record per chunk) for YAML/CSV. Shares `embedding_engine.c` with the memory subsystem; supports ONNX (local), Ollama, and OpenAI-compatible embedding providers. Raw uploads are retained in a generic **blob store** (`src/blob_store.c`, `documents.original_blob_id`) so originals can be downloaded/re-processed. | [rag.md](docs/arch/subsystems/rag.md) |
| **Notes / Reference Text** | First-class **notes** document kind (`DOC_KIND_NOTES`) on the same `document_db`/`embedding_engine` foundation: user- or LLM-authored reference text with hybrid lexical (BM25/FTS5 + Porter2 stemming, `libstemmer`) plus semantic search, surgical edit/append, version history with one-step undo (`document_versions`), and a notes↔memory bridge so fuzzy recall resolves to the right note. Filed note bodies are kept **out** of semantic memory by default (`note_extraction_guard`) so the canonical text lives only in the note store. | shares [rag.md](docs/arch/subsystems/rag.md) + [vision-documents.md](docs/arch/subsystems/vision-documents.md) |
| **CalDAV Calendar** | Multi-account RFC 4791 client with offline-first SQLite cache, pre-expanded RRULE occurrences, and background sync. Tested with Google, iCloud, Nextcloud, Radicale. | [calendar.md](docs/arch/subsystems/calendar.md) |
| **Email** | Dual backend — IMAP/SMTP for anything, Gmail REST API for OAuth accounts. Two-step confirmation on send and trash. Recipients resolved against the contacts system. | [email.md](docs/arch/subsystems/email.md) |
| **Messaging Channels** (`src/messaging/`) | Bidirectional text chat over Telegram, Discord, Slack, and SMS. Each provider is a `messaging_driver_t` with its own background listener thread; inbound messages bind to a per-channel "forever conversation" (`SESSION_TYPE_MESSAGING`, exempt from idle cleanup) with per-conversation LLM settings, and scheduler briefings can deliver to a channel via `deliver_to`. Discord additionally supports read-only channel read/summarize (`list_readable_channels`/`read_history`, `messaging_engine_read.c`). Engine split into core + `_session`/`_channels`/`_link`/`_inbound`/`_read` behind `messaging_engine_internal.h`. WebUI channel-management panel + `dawn-admin messaging` CLI. | [MESSAGING_CHANNELS_SETUP.md](docs/MESSAGING_CHANNELS_SETUP.md) |
| **Phone & SMS** (`src/tools/phone_*.c`) | Cellular calls and SMS via the external **ECHO** modem daemon (SIM7600G-H) over MQTT. `phone_tool.c` is the LLM interface; `phone_service.c` runs the call state machine (RINGING/ANSWERING/ACTIVE, atomic first-wins answer claim) + TTS announcements + HUD/WebUI banner; `phone_db.c` logs calls/SMS. Incoming-call context is fanned to every interactive session via `session_broadcast_system_message()`; a WebUI incoming-call banner + persistent in-call panel let any surface answer/reject/hang up. Two-way call **audio** bridge (Phase 5) is not yet shipped. | [PHONE_SMS_DESIGN.md](docs/PHONE_SMS_DESIGN.md) |
| **OAuth 2.0 & Crypto** | Shared OAuth client with PKCE S256 and `crypto_store.c` (libsodium `crypto_secretbox`) for encrypted token and password storage. Used by email and calendar. | [oauth-crypto.md](docs/arch/subsystems/oauth-crypto.md) |
| **Scheduler** | Timers, alarms, reminders, and scheduled tool execution. Background thread polls every second, fires with chime audio + WebUI banner notifications, supports recurrence and snooze/dismiss. | [scheduler.md](docs/arch/subsystems/scheduler.md) |
| **Home Assistant** | REST API client with entity cache, fuzzy name matching, and satellite area-awareness (`HomeAssistant_Area=[X]` injected into the LLM system prompt). 16 tool actions spanning lights, climate, locks, covers, media, scenes, scripts, automations. | [homeassistant.md](docs/arch/subsystems/homeassistant.md) |
| **Per-User Settings** | Persona, location, timezone, units, theme — stored in `user_settings` and injected into the LLM system prompt at session start so every session is personalized to the authenticated user. | [user-settings.md](docs/arch/subsystems/user-settings.md) |

---

## Module Dependency Hierarchy

To prevent circular dependencies and maintain clean architecture, modules are organized into layers. **Modules may only depend on modules in lower layers.**

```
Layer 0 (Foundation)
├── common/src/logging.c           - Logging macros (shared with satellite, no deps)
├── include/dawn_error.h           - SUCCESS/FAILURE return codes (no deps)
├── src/config/                    - Configuration parsing and defaults
│   ├── config_parser.c
│   ├── config_defaults.c
│   ├── config_env.c
│   └── config_validate.c
└── include/config/dawn_config.h   - Config struct definitions

Layer 1 (Core Infrastructure)
├── src/tools/tool_registry.c/h    - Tool registration and lookup (deps: logging, config)
├── src/core/command_router.c/h    - Request/response routing (deps: logging)
├── src/core/command_executor.c/h  - Unified command executor (deps: tool_registry)
├── src/core/session_manager.c/h   - Session lifecycle (deps: logging, config)
├── src/core/worker_pool.c/h       - Concurrent tool execution (deps: logging)
├── src/core/wake_word.c/h         - Wake-word matching (shared daemon + satellites)
├── src/core/time_query_parser.c/h - Stateless temporal-expression recognizer (deps: libc, math)
├── src/core/utterance_dedup.c/h   - Cross-device utterance dedup (leaf lock, deps: logging)
├── src/core/text_input_dispatch.c - Shared text-input → LLM entry path (deps: session_manager)
├── src/core/prompt_compose.c      - Two-segment prompt composer (stable prefix + volatile tail)
└── src/input_queue.c/h            - Thread-safe input queue (deps: logging)

Layer 2 (Services)
├── src/llm/                       - LLM providers and tools
│   ├── llm_interface.c            - Provider abstraction (deps: Layer 0-1)
│   ├── llm_openai.c               - OpenAI/Ollama/llama.cpp (deps: llm_interface)
│   ├── llm_claude.c               - Anthropic Claude (deps: llm_interface)
│   └── llm_tools.c                - Tool execution (deps: tool_registry)
├── src/core/embedding_engine.c    - Shared embedding infrastructure (deps: Layer 0-1)
├── src/core/crypto_store.c        - Shared libsodium encryption (deps: Layer 0)
├── src/core/scheduler.c           - Scheduler engine + background thread (deps: Layer 0-1)
├── src/core/session_manager_llm.c - LLM-call orchestration extracted from session_manager (deps: Layer 0-1, llm)
├── src/core/ota*.c                - OTA release store, signed manifests, fleet rollout (deps: Layer 0-1, crypto_store; rollout pushes via a registered fn pointer to avoid a Layer-4 dep)
├── src/tts/                       - Text-to-speech (deps: Layer 0-1)
├── src/asr/                       - Daemon-side ASR interface, Vosk, chunking (deps: Layer 0-1)
├── common/src/asr/                - Shared ASR engines (Whisper, VAD) used by daemon + satellite
├── src/mosquitto_comms.c          - MQTT integration (deps: Layer 0-1, tool_registry)
├── src/memory/                    - Persistent memory + contacts (deps: Layer 0-1, embedding_engine)
└── src/auth/                      - User auth, settings, per-user prefs (deps: Layer 0-1)

Layer 3 (Tools)
├── src/tools/weather_tool.c           - Weather API (deps: Layer 0-2)
├── src/tools/music_tool.c             - Music playback (deps: Layer 0-2)
├── src/tools/search_tool.c            - Web search (deps: Layer 0-2)
├── src/tools/memory_tool.c            - Memory commands (deps: Layer 0-2, memory/)
├── src/tools/document_search.c        - RAG semantic search (deps: Layer 0-2, embedding_engine)
├── src/tools/document_read.c          - Paginated doc reader (deps: Layer 0-2, document_db)
├── src/tools/document_db.c            - Document SQLite CRUD (deps: Layer 0-1, auth_db)
├── src/tools/email_service.c          - Email routing + two-step confirm (deps: Layer 0-2, oauth_client)
├── src/tools/email_client.c           - IMAP/SMTP backend (deps: Layer 0-1, crypto_store)
├── src/tools/gmail_client.c           - Gmail REST API backend (deps: Layer 0-1, oauth_client)
├── src/tools/oauth_client.c           - OAuth 2.0 + PKCE (deps: Layer 0-1, crypto_store)
├── src/tools/homeassistant_service.c  - HA REST API + entity cache (deps: Layer 0-1)
├── src/tools/calendar_service.c       - CalDAV business logic (deps: Layer 0-2, oauth_client)
├── src/messaging/messaging_engine.c   - Channel engine: sessions, binding, dispatch (deps: Layer 0-2, session_manager, auth_db, scheduler)
├── src/messaging/messaging_{telegram,slack,discord,sms}.c - Provider drivers behind messaging_driver_t (deps: curl/lws)
├── src/messaging/ws_reconnect.c       - Reconnect/backoff helper (pure data structure, no DAWN deps — Layer-1 leaf candidate)
└── src/tools/*.c                      - All other tools (deps: Layer 0-2)

Layer 4 (Application)
├── src/dawn.c                     - Main entry + voice state machine (deps: all layers)
└── src/webui/                     - Web interface + WebSocket server (deps: Layer 0-3)
```

### Dependency Rules

1. **Downward only**: a module may only `#include` headers from its own layer or lower.
2. **No cycles**: if A depends on B, B must not depend on A (directly or transitively).
3. **Interface segregation**: use forward declarations and callbacks to break potential cycles.
4. **Same-layer allowed**: modules in the same layer may depend on each other if acyclic.

### Common Patterns to Avoid Cycles

**Callback registration** (Layer 2 → Layer 3 without direct dependency):

```c
// In tool_registry.h (Layer 1)
typedef char *(*tool_callback_t)(const char *action, char *value, int *should_respond);

// In weather_tool.c (Layer 3) — registers callback at init
tool_registry_register(&weather_metadata);  // Passes function pointer up
```

**Forward declarations** (when header inclusion would create a cycle):

```c
// In llm_tools.h — avoid including full tool_registry.h
struct tool_metadata;  // Forward declaration
```

---

## Threading Model

DAWN keeps the thread count small. The main thread owns the voice state machine, ASR, TTS invocation, and MQTT. Dedicated worker threads handle anything that would otherwise block the audio loop.

```
┌────────────────────────────────────────────────────────┐
│                      Main Thread                       │
│  - State machine (SILENCE → WAKEWORD → COMMAND → PROC) │
│  - VAD + ASR processing                                │
│  - TTS synthesis (mutex-protected)                     │
│  - MQTT, session management                            │
└────────┬───────────────────────────────────────────────┘
         │
         │ spawns per-task workers as needed
         ▼
┌────────────────────────────────────────────────────────┐
│  Capture thread  — continuous ALSA → ring buffer       │
│  LLM worker      — blocking HTTP + interrupt polling   │
│  Memory extract  — session-end, background             │
│  Music scanner   — periodic local + Plex sync          │
│  Scheduler       — 1-second polling loop               │
│  CalDAV sync     — background event pull               │
│  WebUI audio     — per-connection ASR/TTS pipeline     │
│  Messaging recv  — per-provider listener (Telegram/    │
│                    Slack/Discord WS or poll)           │
│  Messaging work  — inbound dispatch + async outbound   │
│  Job worker      — detached per background job (ASR-   │
│                    less LLM tool loop on a pool session)│
│  Reinvoke worker — detached per re-engagement (runs a  │
│                    turn via the turn queue or detached) │
│  Job notify      — transient detached delivery of job  │
│                    completions (chime/banner/voice)     │
└────────────────────────────────────────────────────────┘
```

**Turn serialization (background-jobs era).** All WebUI LLM turns — user text, push-to-talk
voice, and background-job re-engagements — funnel through the per-session **turn queue**
(`src/core/turn_queue.c`), which guarantees at most one turn runs on a given `session_t` at a
time. This makes "two turns never touch one session's streaming state concurrently" a
structural property rather than a race to manage. The job worker, reinvoke worker, and
notify-delivery threads above are all detached; `jobs_monitor_tick` advances job lifecycle on
the main-loop 1-second heartbeat (dirty-gated — zero DB work when idle), adding no polling thread.

**Note on the thread budget.** Messaging is the one subsystem that meaningfully grows the
thread count — each enabled provider runs a persistent listener thread plus a shared worker.
**OTA fleet rollout deliberately adds no thread**: it advances on the main loop's existing
1-second heartbeat (`ota_rollout_tick`), so a canary/fan-out is in flight without a dedicated
thread to reason about.

**Synchronization primitives**:

- **Ring buffer**: thread-safe circular buffer for audio data (lock-free read/write pointers).
- **TTS mutex** (`tts_mutex`): protects Piper from concurrent access.
- **LLM mutex** (`llm_mutex`): guards request/response ownership transfer between main and worker.
- **Auth DB mutex**: serializes SQLite writes against the shared `auth.db` handle.
- **Embedding cache mutexes**: protect in-memory fact and entity embedding caches (see [memory.md](docs/arch/subsystems/memory.md)).

See [Mutex Lock Ordering Hierarchy](#mutex-lock-ordering-hierarchy) below for the acquire-order invariants.

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

| From State        | Event                 | To State          |
| ----------------- | --------------------- | ----------------- |
| SILENCE           | VAD detects speech    | WAKEWORD_LISTEN   |
| WAKEWORD_LISTEN   | Wake word detected    | COMMAND_RECORDING |
| WAKEWORD_LISTEN   | Timeout / false alarm | SILENCE           |
| COMMAND_RECORDING | VAD detects silence   | PROCESSING        |
| PROCESSING        | Pipeline complete     | SILENCE           |

During PROCESSING the LLM call runs on a worker thread. The main thread continues to service audio; a wake word during LLM inference triggers `llm_request_interrupt()`, which aborts the CURL transfer and rolls back conversation history.

---

## Mutex Lock Ordering Hierarchy

**CRITICAL**: to prevent deadlocks, the codebase follows a strict acquisition order when multiple locks are needed. Mutexes fall into three categories by scope:

```
Global daemon locks (src/dawn.c):
  llm_mutex              — LLM worker thread ↔ main thread buffer transfer
  tts_mutex              — TTS engine (Piper) serialization
  conversation_mutex     — conversation history list
  direct_mode_prompt_mutex — direct-mode prompt reload

Per-session locks (src/core/session_manager.c):
  session->history_mutex    — session conversation history
  session->metrics_mutex    — session-scoped metrics (tokens, timings)
  session->fd_mutex         — WebSocket file-descriptor state
  session->ref_mutex        — session reference counting
  session->llm_config_mutex — per-session LLM config overrides

Per-module locks (scoped to a single subsystem):
  auth_db mutex (src/auth/auth_db_core.c)         — SQLite serialization
  tool_registry::s_registry_mutex                 — tool lookup table
  embedding_engine::s_embed_mutex                 — embed provider serialization
  scheduler_mutex, ringing_mutex (scheduler.c)    — scheduler event queue
  worker_pool::pool_mutex                         — worker thread pool
  command_router::registry_mutex                  — request/response routing
  utterance_dedup::s_mutex (utterance_dedup.c)    — cross-device dedup slots (leaf)
  attention::s_mutex (src/core/attention/attention_core.c) — SAGE watch cache + event queue + metrics (leaf)
  turn_queue::s_turn_queue_mutex (src/core/turn_queue.c)   — per-session turn-serialization queue (LEAF; never held across the spawn/free closures)
  job_manager::s_pool_mutex (src/core/job_manager.c)       — background-job session pool (REGISTRY tier, like session_manager_rwlock: released before any ref-cond wait, session_free, or conv_db_*/scheduler_* callout)
  job_reinvoke::s_inflight_mutex (src/core/job_reinvoke.c) — per-parent reinvoke in-flight set (leaf)
  ...and similar per-tool mutexes in src/tools/*.c
```

### Lock Ordering Rules

1. **Global locks are acquired before per-session locks, which are acquired before per-module locks.** Never acquire a higher-scope lock while holding a lower-scope one.

2. **Never hold two global locks simultaneously.** Release one before acquiring another. The main thread holds at most one of `tts_mutex`, `llm_mutex`, `conversation_mutex` at a time.

3. **The `auth_db` mutex is a leaf lock** (no other locks held during SQLite writes). Copy data out, release, then continue.

4. **Keep critical sections minimal.** Copy data, release the lock, *then* process. Avoid I/O while holding locks.

5. **Prefer lock-free patterns for high-frequency updates.** The audio ring buffer uses volatile read/write pointers; state flags use `volatile` booleans or C11 atomics; `llm_processing` and `llm_interrupt_requested` are `volatile sig_atomic_t`.

### Testing Lock Discipline

Use **ThreadSanitizer** during development:

```bash
cd build
cmake -DCMAKE_C_FLAGS="-fsanitize=thread -g" ..
make
./dawn
```

ThreadSanitizer detects data races, lock order inversions, and use-after-free in threaded code.

---

## Memory Management

### Design Principles

1. **Prefer static allocation**: embedded systems benefit from predictable memory usage.
2. **Minimize dynamic allocation**: use `malloc`/`calloc` sparingly.
3. **Always check NULL**: verify dynamic allocation succeeded.
4. **Free and NULL**: set pointers to NULL after freeing.

### Memory Patterns

**Static buffers** (preferred):

```c
#define AUDIO_BUFFER_SIZE 16000
static int16_t audio_buffer[AUDIO_BUFFER_SIZE];
```

**Dynamic allocation** (when necessary):

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

### Memory Usage

"How big is the model?" and "how much RAM does DAWN use?" are different questions, and on a CUDA build the answers are far apart: the running daemon's footprint is dominated by the **CUDA runtime** (context + BLAS/DNN workspaces), not the model weights. This section covers both — first the per-artifact sizes, then the measured daemon footprint.

#### Model & buffer artifacts

| Component    | Memory Usage | Notes                      |
| ------------ | ------------ | -------------------------- |
| Whisper base | ~140 MB      | Model weights + context    |
| Vosk 0.22    | ~50 MB       | Smaller footprint          |
| Silero VAD   | ~2 MB        | Tiny ONNX model            |
| Piper TTS    | ~30 MB       | Voice model + ONNX runtime |
| Ring Buffer  | ~256 KB      | 16kHz × 16-bit × 8s buffer |
| Conversation | ~10 KB       | History for LLM context    |

These are the artifacts in isolation — they exclude the CUDA runtime, cuBLAS/cuDNN workspaces, and per-request heap, which is why the daemon's resident set is several times larger on a GPU build (below).

#### Measured daemon footprint

*(Jetson Orin, JetPack R36, release build, GPU Whisper base, cloud LLM, 1 WebUI + 1 satellite connected.)* Unified memory — the GPU shares system RAM — so the resident set splits into two pools:

| Pool | Approx | Contents |
|---|---|---|
| CPU-side RSS | ~435 MB | app heap, thread stacks + CUDA host-pinned + ONNX-Runtime arenas, resident library code, Whisper CPU mmap |
| GPU / unified | ~1.12 GB | CUDA context + cuBLAS/cuDNN workspaces + Whisper weights + ggml compute buffers |
| **Total resident (VmRSS)** | **~1.56 GB** | peak ~1.63 GB; the number `top`/`ps` report |

- **The CUDA runtime dominates, not the model.** Whisper *base* weights are ~140 MB; the rest of the GPU pool is the CUDA context + BLAS/DNN workspaces. A smaller Whisper model (`tiny`) trims the GPU pool modestly, not the fixed CUDA cost.
- **VSZ (~19 GB) is not real memory.** CUDA plus ~19 thread stacks reserve large virtual ranges that stay mostly uncommitted; ignore it.
- **The LLM isn't in this number with a cloud provider** (OpenAI/Claude/Gemini ≈ 0 local RAM). A **local** LLM (llama.cpp/Ollama) adds its model size on top — commonly **+2–8 GB**.
- **Threads / clients.** ~22 threads with two clients connected; the thread stacks cost only ~17 MB resident in total. Each additional connected client adds roughly one handler thread plus a small session-heap slice — a few MB for an active WebUI audio session (its per-connection ASR/TTS pipeline), ~1–2 MB for a text/satellite client. Client scaling is modest next to the fixed CUDA/model footprint.
- **A debug build runs ~75 MB heavier** on the CPU side (debug allocator + symbols); the GPU pool is identical.
- On Tegra the GPU pool counts toward VmRSS but is allocated via `nvmap`, so it does **not** appear in `/proc/<pid>/smaps`; per-library GPU attribution requires instrumenting allocations (an `LD_PRELOAD` `cudaMalloc` hook or Nsight Systems).

---

## Error Handling

### Error Code Convention

Central definitions in `include/dawn_error.h`:

```c
#define SUCCESS  0
#define FAILURE  1
```

Modules define specific error codes > 1 in their own headers (e.g., `AUTH_DB_FAILURE`, `MEMORY_DB_NOT_FOUND`, `SCHED_DB_USER_LIMIT`). Functions that return counts or IDs use an output parameter (`int *count_out`, `int64_t *id_out`) and return `SUCCESS`/`FAILURE`.

**IMPORTANT**: do NOT use negative return values (`-1`, `-errno`). Use positive error codes only. The sole exception is `LWS_CLOSE_CONNECTION` (-1) in lws callback functions, per the libwebsockets API contract.

### Patterns

**Function return codes**:

```c
int asr_process_audio(ASRContext *ctx, int16_t *audio, size_t samples) {
   if (ctx == NULL || audio == NULL) {
      LOG_ERROR("Invalid parameters");
      return FAILURE;
   }
   // ... processing ...
   return SUCCESS;
}
```

**Retry with exponential backoff** (network I/O):

```c
int retry_count = 0;
while (retry_count < MAX_RETRIES) {
   if (send_packet(packet) == SUCCESS) break;
   LOG_WARNING("Send failed, retry %d/%d", retry_count + 1, MAX_RETRIES);
   sleep(1 << retry_count);  // 1s, 2s, 4s
   retry_count++;
}
```

**Graceful degradation** (feature availability):

```c
if (gpu_available) {
   ctx = asr_whisper_init(model_path);
} else {
   LOG_WARNING("GPU not available, using CPU-only ASR");
   ctx = asr_vosk_init(model_path);
}
```

---

## File Organization Standards

### Size Limits

| File Type  | Soft Limit  | Hard Limit  |
| ---------- | ----------- | ----------- |
| C source   | 1,500 lines | 2,500 lines |
| JavaScript | 1,000 lines | 1,500 lines |
| CSS        | 1,000 lines | 2,000 lines |

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

The internal header contains `extern` declarations for shared state (defined in `_core.c`), internal helper function declarations, and shared macros (e.g., locking patterns).

### When Adding New Features

1. **Check file size first** — if the target file > 1,500 lines, consider creating a new file.
2. **Group by feature** — related functionality goes together in one module.
3. **Use internal headers** — share state via the `*_internal.h` pattern.
4. **Update build system** — add new source files immediately.

---

## Configuration Architecture

### Design Principles

1. **Config files as source of truth**: all DAWN application settings live in `dawn.toml` (runtime) or `secrets.toml` (credentials). The SQLite database is reserved for user-generated content — authentication, sessions, conversations, uploaded images. **Settings are never stored in the database.** This keeps configuration portable, version-controllable (minus secrets), and inspectable.

2. **WebUI settings exposure**: every setting in `dawn.toml` is surfaced in the WebUI settings panel unless explicitly excluded. Exclusions are limited to file system paths (security), internal debug flags, and restart-only settings that have no runtime effect.

3. **Secrets isolation**: credentials in `secrets.toml` stay separate from general config so `dawn.toml` can be shared safely, per-deployment secrets can differ, and credentials rotate without touching the main config.

4. **Compile-time vs runtime**: `dawn.h` provides compile-time defaults only. All user-configurable settings belong in TOML; `dawn.h` values serve as fallbacks when config is missing.

### Configuration File Hierarchy

```
~/.config/dawn/     # User-specific (highest priority)
./                  # Project root (fallback)
/etc/dawn/          # System-wide (lowest priority, future)
```

Higher-priority files override lower.

### Configuration Files

**`dawn.toml`** — runtime configuration, one section per subsystem:

```toml
[general]
ai_name = "friday"
timezone = "America/New_York"

[llm]
type = "cloud"                    # "cloud" or "local"

[llm.cloud]
provider = "openai"               # "openai", "anthropic", "gemini"
model = "gpt-4o"
use_openrouter = false            # route all cloud traffic through the OpenRouter gateway

[llm.local]
endpoint = "http://localhost:8080"
model = "qwen3-4b"

[asr]
model_path = "models/whisper.cpp/ggml-base.en.bin"
language = "en"
dedup_window_sec = 4              # cross-device utterance dedup (0 disables)

[tts]
model_path = "models/en_GB-alba-medium.onnx"
sample_rate = 22050

[webui]
bind_address = "0.0.0.0"
port = 3000

[messaging.sms]                   # Telegram/Slack/Discord load from tokens in secrets.toml
active_window_sec = 300           # bypass wake-word within this window after last message

[ota]
enabled = false                   # server→satellite over-the-air updates
release_dir = "/var/lib/dawn/ota"
```

**`secrets.toml`** — API keys and sensitive credentials (gitignored):

```toml
openai_api_key = "sk-..."
claude_api_key = "sk-ant-..."
gemini_api_key = "..."
```

**`dawn.h`** — compile-time fallbacks: `AI_NAME`, `AI_DESCRIPTION`, `DEFAULT_PCM_PLAYBACK_DEVICE`, `DEFAULT_PCM_CAPTURE_DEVICE`, `MQTT_IP`, `MQTT_PORT`.

**Tool registry** — tools are defined as compile-time `tool_metadata_t` structs in `src/tools/*.c` and registered in `src/tools/tools_init.c` via `tools_register_all()`. See [command-processing.md](docs/arch/command-processing.md).

### WebUI Settings Panel Mapping

The WebUI settings panel (`www/js/ui/settings.js`) defines a `SETTINGS_SCHEMA` that maps to `dawn.toml` sections:

| WebUI Section      | Config Section                        | Notes                                           |
| ------------------ | ------------------------------------- | ----------------------------------------------- |
| Language Model     | `[llm]`, `[llm.cloud]`, `[llm.local]` | Provider, model selection, OpenRouter gateway toggle |
| Speech Recognition | `[asr]`                               | Model, language, cross-device dedup window      |
| Text-to-Speech     | `[tts]`                               | Voice model, rate                               |
| Audio              | `[audio]`                             | Backend, devices                                |
| Tool Calling       | `[llm.tools]`                         | Mode, per-tool toggles                          |
| Network            | `[webui]`, `[dap]`, `[mqtt]`          | Ports, addresses                                |
| Images & Vision    | `[images]`, `[vision]`                | Storage retention, upload size/dimension limits |
| Documents          | `[documents]`                         | Upload size, page/index limits, chunking, hybrid-search weights |
| Messaging          | `[messaging.sms]` (+ tokens in `secrets.toml`) | Channel link/unlink/rename, per-channel reasoning/effort |
| OTA / Fleet        | `[ota]`                               | Release dir, download-token TTL, TLS requirement; fleet rollout lives in the OTA panel, not Settings |
| Code Projects      | `[code_projects]`                     | Coding harness: enable, source root, import permissions, clone caps, and `allowed_local_roots` (link-local allowlist). Compiled in only with `DAWN_ENABLE_CODE_PROJECTS`. |
| Scheduler & Watches | `[scheduler]`, `[attention]`         | Snooze/alarm defaults, per-user + global event caps, missed-task policy, retention; SAGE watch budgets (watch *rules* are DB-backed, not config) |
| Background Jobs    | `[jobs]`                              | Master enable, global/per-user/per-provider concurrency caps, per-job runtime reap, reinvoke caps. Job create/list/cancel/resume is conversational (the `job` tool + WebUI), never config. Four Phase-2/3 knobs are parsed and round-tripped but not yet enforced, so they are deliberately **not** surfaced in the panel |

### Adding a setting — read [docs/CONFIGURATION_GUIDE.md](docs/CONFIGURATION_GUIDE.md) first

Adding a `dawn.toml` setting touches **up to nine files**, and `SETTINGS_SCHEMA` is only one of them. The
full checklist, a worked example, and the verification steps live in
**[docs/CONFIGURATION_GUIDE.md](docs/CONFIGURATION_GUIDE.md)**.

The critical one: a WebUI settings save **rewrites the entire `dawn.toml`** from the in-memory config
(`webui_config.c` → `config_write_toml()`). Any section the parser reads but the writer never emits is
**silently deleted from the user's file** on the next save — of *any* setting, in *any* panel — reverting
those settings to defaults. Nothing fails to build and no test goes red; the user just loses their config.
So `config_to_json()` (GET), `config_write_toml()` (persist), and the `webui_config.c` POST handler always
move together. `tests/test_config_roundtrip.c` guards this in CI — add new sections to its `required[]` list.

Settings should be surfaced in the WebUI panel unless they fall under the exclusion criteria above (or are
parsed-but-not-yet-enforced — see the guide).

---

## Performance Considerations

### GPU Acceleration (Jetson)

- Automatic detection via `/etc/nv_tegra_release` in CMake.
- CUDA libraries (cuSPARSE, cuBLAS, cuSOLVER, cuRAND) linked automatically.
- Whisper GPU enabled with `GGML_CUDA=ON`; 2.3x–5.5x speedup over CPU.

### Perceived Latency

**Total** = ASR time + TTFT + TTS time.

| Component          | Latency (Whisper base GPU) | Notes                     |
| ------------------ | -------------------------- | ------------------------- |
| ASR (Whisper base) | ~110 ms                    | GPU accelerated           |
| TTFT (Qwen3-4B)    | ~138 ms                    | Local LLM first token     |
| TTS (Piper)        | ~200 ms                    | First sentence            |
| **Total**          | **~448 ms**                | User hears first response |

**Streaming advantage**: with streaming LLM + TTS, the user hears a response in <500ms instead of waiting for the complete LLM response (~3s).

### Platform Override

CMake auto-detects Jetson, Raspberry Pi, and generic ARM64. Force with:

```bash
cmake -DPLATFORM=JETSON ..  # Force Jetson (enables CUDA)
cmake -DPLATFORM=RPI ..     # Force RPi (disables CUDA)
```

---

## DAP2 Satellite Protocol

DAP2 is the unified WebSocket protocol for all remote access to the DAWN daemon. **A single WebSocket server on port 3000 serves all three client types**: browser WebUI, Tier 1 satellites (Raspberry Pi), and Tier 2 satellites (ESP32). There are no separate servers or ports — each client registers its capabilities and the daemon routes messages accordingly.

| Client     | Hardware | Transport                     | Server does     | Use Case                      |
| ---------- | -------- | ----------------------------- | --------------- | ----------------------------- |
| **WebUI**  | Browser  | Opus audio (48kHz) + JSON     | ASR + LLM + TTS | Browser voice/text interface  |
| **Tier 1** | RPi 4/5  | JSON text (`satellite_query`) | LLM only        | Hands-free (local ASR/TTS)    |
| **Tier 2** | ESP32-S3 | Binary PCM audio (16kHz)      | ASR + LLM + TTS | Push-to-talk (server ASR/TTS) |

The session manager, response queue, LLM pipeline, tool system, and conversation history are shared infrastructure. Adding a new client type needs only a registration handler and a routing decision — not a new server.

**OTA control plane.** Fleet rollout pushes signed-update availability to connected Tier 1/Tier 2 satellites over this same WebSocket server; the device downloads via a one-time token and applies (.deb for the Pi, device-apply for the ESP32). See [OTA_DESIGN.md](docs/OTA_DESIGN.md).

**Full details**: message types, connection lifecycle, UI patterns, satellite registration, and music streaming all live in [satellite.md](docs/arch/subsystems/satellite.md). The wire protocol itself is specified in [WEBSOCKET_PROTOCOL.md](docs/WEBSOCKET_PROTOCOL.md).

---

## Command Processing

DAWN supports three parallel command-processing paths — direct regex matching, native LLM tool calls, and legacy `<command>` tags — that all converge on a single unified executor (`command_execute()`).

- **Tool registry** (`src/tools/tool_registry.c`): self-registration with FNV-1a hash tables for O(1) lookup, automatic schema generation for multiple LLM providers, and capability flags (`TOOL_CAP_NETWORK`, `TOOL_CAP_DANGEROUS`).
- **Processing mode** is selected in `dawn.toml`: `direct_only`, `llm_only`, or `direct_first`.
- **Native tools** vs. **legacy `<command>` tags** use the same enable/disable flags and the same executor; only the transport differs.

**Full flowchart, tool list, and definition sources**: see [command-processing.md](docs/arch/command-processing.md).

---

## References

- **Piper TTS**: https://github.com/rhasspy/piper
- **Vosk ASR**: https://alphacephei.com/vosk/
- **Whisper**: https://github.com/ggerganov/whisper.cpp
- **Silero VAD**: https://github.com/snakers4/silero-vad
- **llama.cpp**: https://github.com/ggerganov/llama.cpp
- **ONNX Runtime**: https://github.com/microsoft/onnxruntime
