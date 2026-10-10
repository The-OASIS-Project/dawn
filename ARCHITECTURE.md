# D.A.W.N. System Architecture

**D.A.W.N.** (Digital Assistant for Workflow Neural-inference) is the central intelligence layer of the OASIS ecosystem. It interprets user intent, fuses data from every subsystem, and routes commands. DAWN acts as OASIS's orchestration hub for MIRAGE, AURA, SPARK, STAT, and any future modules.

This document is the **architectural map**: directory layout, the cross-cutting rules every subsystem obeys (layering, threading, lock ordering, error handling, configuration), and a one-paragraph summary of each subsystem with a link to its detail doc. For the internals of any single subsystem — its components, data flow, DB schema, and tuning — open the linked file in [`docs/arch/subsystems/`](docs/arch/subsystems/).

**Last updated**: October 2026.

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
| **Core** (`src/` root + `src/core/`) | Main entry, MQTT integration, legacy command parsing. `src/dawn.c` hosts the state machine; `src/mosquitto_comms.c/h` wires MQTT; `src/text_to_command_nuevo.c/h` extracts `<command>` tags from LLM output; `src/word_to_number.c/h` converts "twenty-three" → 23; `src/core/` contains the session manager, scheduler, command executor/router, worker pool, and wake-word detector. Logging macros (`OLOG_INFO/WARNING/ERROR/DEBUG`) come from `common/include/logging.h`, shared with the satellite. | *(inlined above)* |
| **ASR** | Speech recognition abstraction (Strategy pattern) over Whisper and Vosk, plus Silero VAD and chunking for long utterances. Whisper on Jetson GPU is the default; Vosk is retained for CPU-only builds. | [asr.md](docs/arch/subsystems/asr.md) |
| **LLM** | Unified interface for OpenAI, Claude, Gemini, **OpenRouter** (a first-class `provider` fronting many vendors through one key; its `anthropic/` models use its Anthropic Messages endpoint on the Claude path, the rest its OpenAI-compatible one), and local (llama.cpp/Ollama). Streaming via SSE feeds a sentence buffer that hands complete sentences to TTS while the response is still generating. A conversation's request is append-only (`src/core/session_prefix.c`): its system prompt and tool set are frozen on its first turn, and later changes (instructions, the surface's directions, each turn's context) are appended where they happen, so providers cache the whole conversation and signed reasoning stays replayable. Runs on a dedicated worker thread so the main audio loop never blocks; wake-word interrupts abort in-flight API calls. | [llm.md](docs/arch/subsystems/llm.md) |
| **TTS** | Piper + ONNX Runtime with preprocessing for natural phrasing. Mutex-protected so the main loop, network server, and streaming buffer can all synthesize safely. | [tts.md](docs/arch/subsystems/tts.md) |
| **DAP2 Satellite** | WebSocket protocol for all remote clients: WebUI browser (Opus), Tier 1 Raspberry Pi (local ASR/TTS, text-only), and Tier 2 ESP32 (raw PCM). A single server on port 3000 serves all three — adding a new client type means a new registration handler, not a new server. | [satellite.md](docs/arch/subsystems/satellite.md) |
| **Satellite OTA** | Signed over-the-air updates for Tier 1 Pi (.deb) and Tier 2 ESP32 (device-apply). Releases are libsodium-signed binary manifests (TweetNaCl verify on the ESP32); the signing key lives offline and never on the daemon (`tools/ota_keytool.c`). Fleet rollout does a canary wave then deferred fan-out, driven off the main-loop 1-second heartbeat (no dedicated thread). WebUI fleet panel + `dawn-admin ota` CLI + runtime release rescan. | [OTA_DESIGN.md](docs/OTA_DESIGN.md) |
| **Audio** | Capture thread + thread-safe ring buffer, multi-format playback (FLAC/MP3/Ogg), and the unified music DB (local files + Plex) with source-aware dedup and background scanner. | [audio.md](docs/arch/subsystems/audio.md) |
| **WebUI Audio** | Browser-side Opus streaming via WebCodecs + server-side decode/resample/ASR/TTS/encode pipeline. TTS is **server-authoritative and multi-target**: a turn's synthesized speech is fanned to **every TTS-enabled browser viewing that conversation** (origin included), synthesized **once per sentence** regardless of listener count and encoded once per format under the connection-registry lock (the audio half of the cross-viewer fan-out — see [SERVER_AUTHORITATIVE_PERSISTENCE_DESIGN.md](https://github.com/The-OASIS-Project/atlas/blob/main/dawn/archive/SERVER_AUTHORITATIVE_PERSISTENCE_DESIGN.md)). Also hosts **always-on voice mode** (server-side VAD + wake word, no browser AI) and the **visual rendering tool** (inline SVG/HTML/Chart.js diagrams). Browser **music** playback decodes Opus **off the main thread** in a Web Worker (→ transferred `MessagePort` → AudioWorklet) over a dedicated music socket, with **closed-loop flow control** (client reports buffer depth; server paces a ~2s lead) — the same dedicated flow-controlled transport Tier-1 satellites use. | [webui-audio.md](docs/arch/subsystems/webui-audio.md) |
| **Vision & Documents** | Image upload (client compression, server filesystem storage with source/retention policies, zero-copy HTTP serving) and document upload (PDF via MuPDF, DOCX via libzip+libxml2, plain text client-side). | [vision-documents.md](docs/arch/subsystems/vision-documents.md) |
| **Memory** | Persistent user profile built by a **sleep-consolidation model**: extraction runs at session end, not during conversation, so chat latency is unchanged. Facts, preferences, summaries, entity graph, and contacts; hybrid keyword + semantic search via embeddings; nightly confidence decay. Conversation anchor on extraction (v42) resolves relative phrases ("yesterday", "last month") against the conversation's logical "now." | [memory.md](docs/arch/subsystems/memory.md). Historical design docs under [atlas/dawn/memory/](https://github.com/The-OASIS-Project/atlas/tree/main/dawn/memory). |
| **Document Search / RAG** | Upload → chunk → embed → search, literal `document_grep`, or paginated read via LLM tools. Structure-aware chunking (one record per chunk) for YAML/CSV. Shares `embedding_engine.c` with the memory subsystem; supports ONNX (local), Ollama, and OpenAI-compatible embedding providers. Raw uploads are retained in a generic **blob store** (`src/blob_store.c`, `documents.original_blob_id`) so originals can be downloaded/re-processed. | [rag.md](docs/arch/subsystems/rag.md) |
| **Notes / Reference Text** | First-class **notes** document kind (`DOC_KIND_NOTES`) on the same `document_db`/`embedding_engine` foundation: user- or LLM-authored reference text with hybrid lexical (BM25/FTS5 + Porter2 stemming, `libstemmer`) plus semantic search, surgical edit/append, version history with one-step undo (`document_versions`), and a notes↔memory bridge so fuzzy recall resolves to the right note. Filed note bodies are kept **out** of semantic memory by default (`note_extraction_guard`) so the canonical text lives only in the note store. | shares [rag.md](docs/arch/subsystems/rag.md) + [vision-documents.md](docs/arch/subsystems/vision-documents.md) |
| **Deep Research** (`src/tools/research_*`, `src/core/research_worker.c`) | **P0 shipped 2026-08-13; P1 (controller-driven convergence) shipped 2026-08-14, live-validated.** Confirmation-gated `deep_research` tool spawns a detached **`research_worker`** (a background-job sibling — reuses `job_manager_*`, its own spawn path, NOT `job_worker.c`) that runs an **IterResearch** loop on a **bare, memory-free** session: each round resets history to a bounded digest + top open questions, plans sub-questions, does web `search`/`url_fetch` under a **read-only tool allowlist** (enforced at advertise AND execute), and records injection-gated evidence **claims** to a SQLite ledger (`research_runs/questions/claims/report_revisions`, schema v75; v76 adds `resolution_reason`). A **deterministic C stop-controller** ends it — natural-end reasons first (`concluded` via the agent's `research_conclude` signal → distinct-source `coverage`, with `research_mark_unanswerable` + **P1 stale-question auto-retirement** clearing dead questions → `saturation` after N dry rounds) then the **input-token budget as a high runaway backstop** (a per-round overshoot guard keeps it honest) — so a run stops on why it's actually done, not on spend. P1 adds **plan-freeze** (no new top-level questions after round K) and a **fresh-context completeness critic** (§6 item 4) that may re-arm a stop-eligible run at an untried angle up to `critic_max_rearm` times (each re-arm resets the saturation streak so a multi-round gap gets a fresh window). A final **no-tools synthesis turn** writes the answer (exec summary + direct answer/recommendation + honest gaps) over the **claims evidence** (a view over `research_claims` — the ledger retains every recorded claim, so synthesis works from evidence rather than a lossy running summary; the render is bounded by a generous per-report claim cap with an explicit truncation marker beyond it), so every terminal path yields a real answer; it's filed to notes and persisted to the job conversation (never re-entering a tool session or `reinvoke_parent` — the untrusted-content boundary, §11). A voice/WebUI run posts a JARVIS-style **completion take** back into the originating conversation (`completion_commentary`). Off by default (`[research] enabled`). P2–P4 (parallel fan-out, attach/replay specialization, TUI) remain design-only. | [DEEP_RESEARCH_DESIGN.md](docs/DEEP_RESEARCH_DESIGN.md) |
| **CalDAV Calendar** | Multi-account RFC 4791 client with offline-first SQLite cache, pre-expanded RRULE occurrences, and background sync. Tested with Google, iCloud, Nextcloud, Radicale. | [calendar.md](docs/arch/subsystems/calendar.md) |
| **Email** | Dual backend — IMAP/SMTP for anything, Gmail REST API for OAuth accounts — read through one MIME reader (`email_mime.c`, GMime): bounded fetches, a pre-scan against parser bombs, charsets to UTF-8, attachments listed, sender text sanitized. Trash/archive move batches (one IMAP login, Gmail per message and paced) to the server-marked folder, never a bare EXPUNGE, with a 60 s undo; IMAP ids are pinned to their mailbox's UIDVALIDITY. Two-step confirmation on send and trash. Recipients resolved against the contacts system. | [email.md](docs/arch/subsystems/email.md) |
| **Messaging Channels** (`src/messaging/`) | Bidirectional text chat over Telegram, Discord, Slack, and SMS. Each provider is a `messaging_driver_t` with its own background listener thread; a channel answers only the person who linked it (the provider's sender id; several DAWN users can each link one group chat, each with their own session), an SMS link completes with a code texted to the number, and inbound messages bind to a per-channel "forever conversation" (`SESSION_TYPE_MESSAGING`, exempt from idle cleanup) with per-conversation LLM settings, and scheduler briefings can deliver to a channel via `deliver_to`. Discord additionally supports read-only channel read/summarize (`list_readable_channels`/`read_history`, `messaging_engine_read.c`). Engine split into core + `_session`/`_channels`/`_link`/`_inbound`/`_codes`/`_read` behind `messaging_engine_internal.h`. WebUI channel-management panel + `dawn-admin messaging` CLI. | [MESSAGING_CHANNELS_SETUP.md](docs/MESSAGING_CHANNELS_SETUP.md) |
| **Phone & SMS** (`src/tools/phone_*.c`) | Cellular calls and SMS via the external **ECHO** modem daemon (SIM7600G-H) over MQTT. `phone_tool.c` is the LLM interface; `phone_service.c` runs the call state machine (RINGING/ANSWERING/ACTIVE, atomic first-wins answer claim) + TTS announcements + HUD/WebUI banner; `phone_db.c` logs calls/SMS. Incoming-call context is posted to every interactive session via `session_broadcast_notice()` (rendered into each turn's volatile prompt block for 10 minutes, never written into a conversation history); a WebUI incoming-call banner + persistent in-call panel let any surface answer/reject/hang up. Two-way call **audio** bridge (Phase 5) is not yet shipped. | [PHONE_SMS_DESIGN.md](docs/PHONE_SMS_DESIGN.md) |
| **OAuth 2.0 & Crypto** | Shared OAuth client with PKCE S256 and `crypto_store.c` (libsodium `crypto_secretbox`) for encrypted token and password storage. Used by email and calendar. | [oauth-crypto.md](docs/arch/subsystems/oauth-crypto.md) |
| **Scheduler** | Timers, alarms, reminders, and scheduled tool execution. Background thread polls every second, fires with chime audio + WebUI banner notifications, supports recurrence and snooze/dismiss. | [scheduler.md](docs/arch/subsystems/scheduler.md) |
| **Home Assistant** | REST API client with entity cache, fuzzy name matching, and satellite area-awareness (a satellite's Home Assistant area sent with its standing directions). 16 tool actions spanning lights, climate, locks, covers, media, scenes, scripts, automations. | [homeassistant.md](docs/arch/subsystems/homeassistant.md) |
| **Per-User Settings** | Persona, location, timezone, units, theme — stored in `user_settings` and injected into the LLM system prompt at session start so every session is personalized to the authenticated user. | [user-settings.md](docs/arch/subsystems/user-settings.md) |

---

## Module Dependency Hierarchy

To prevent circular dependencies and maintain clean architecture, modules are organized into layers. **Modules may only depend on modules in lower layers.**

This map lists the layers and the exceptions to them, not every file: a file is named here only when it carries a rule you could break without knowing it. To find a file, grep, or open the subsystem's detail doc (see the [Subsystem Index](#subsystem-index)).

```
Layer 0 (Foundation) — no DAWN dependencies
├── common/src/logging.c, include/dawn_error.h   Logging (OLOG_*), SUCCESS/FAILURE codes
└── src/config/, include/config/                  Config structs, defaults, parsing, validation

Layer 1 (Core infrastructure) — deps: Layer 0
├── src/tools/tool_registry.c   Tool registration and lookup, and each action's kind (read, fetch, state,
│                               device, prepare, act), checked at registration and at build time
├── src/core/ primitives        Command routing/execution, worker pool, wake word, time parsing, utterance
│                               dedup, reply codes, text-input dispatch, prompt sections, input queue
├── include/core/turn_origin.h  Where a pending action was made; a confirm carries it out only in the same
│                               session, on the next turn, and never in a turn carrying an email the user
│                               attached
├── src/core/pending_slots.c    What a tool staged for the user's confirm: one item per session and kind, each
│                               with a new id its confirm must name; never evicts another session's item
├── src/core/tool_call_challenge.c  An action asked for by text, waiting for its reply code: one per channel,
│                               handed over once on the right code
└── src/core/session_reaper.c   session_destroy() only ends a session and never waits; the reaper joins its
                                compaction worker, waits for its last reference, then frees it

Layer 2 (Services) — deps: Layers 0-1 and each other, acyclic
├── include/prompts.h           Every model-facing prompt, as string literals; quotes llm/ and core/ tokens, so
│                               include it from Layer 2 up
├── src/llm/                    Providers, streaming, tool loop, turn blocks, compaction core, tool-result views
├── src/core/ services          Session unit (below), focus framework (src/core/focus/), prompt prefix
│                               (prefix_*), embeddings, crypto store, scheduler, tool-result store, OTA, images
├── src/core/tool_call_policy.c Who may make a tool call: the caller's kind of turn (user, unverified sender,
│                               background job, unattended, a user turn carrying an attached email) against the
│                               action's kind; decided once per call
├── src/memory/                 Persistent memory, contacts, extraction, forgetting
├── src/auth/                   Users, settings, conversations and messages; auth_db_messages.c is the one
│                               message insert (and the LLM replay read)
├── src/tts/, src/asr/, common/src/asr/   Speech in and out (shared engines in common/)
└── src/mosquitto_comms.c       MQTT

Layer 3 (Tools and channels) — deps: Layers 0-2
├── src/tools/                  Every tool and its service (email, calendar, Home Assistant, music, stocks,
│                               documents, research, MCP bridge, ...); email_mime.c is the only file that uses
│                               GMime
└── src/messaging/              Channel engine and provider drivers (Telegram, Slack, Discord, SMS)

Layer 4 (Application) — deps: everything below
├── src/dawn.c                  Main entry, local voice state machine
├── src/webui/                  Web interface and WebSocket server.  Optional (ENABLE_WEBUI): without it,
│                               webui_absent.c supplies the WebUI entry points the rest of DAWN calls, as
│                               no-ops, and the WebUI-only pieces are left out (messaging channels, the job and
│                               deep_research tools, the OAuth and code-project handlers, Home Assistant's
│                               realtime connection)
│   ├── webui_email_exec*.c     The WebUI's email executor: per-account tasks on 4 workers, the IMAP lease
│   │                           taken by ticket so no worker waits on an account, replies by session id; a
│   │                           session's moves queue in order on its MOVE slot, never replaced
│   ├── webui_email_ref.c       A text turn's attached email (email_refs): its account checked in the database
│   │                           at receipt, carried with the turn (the model reads the email with the tool),
│   │                           saved as messages.email_ref and sent back on every frame delivering the row
│   └── webui_email_panel*.c    The mail panel's verbs (email_list/_search/_read/_set_flags/_unread_counts;
│                               _archive/_trash/_undo in webui_email_panel_move.c), with the pure
│                               email_cursor.c (paging across accounts) and email_wire.c (rows, read frames
│                               within their size budget); webui_email_changed.c pushes email_changed to
│                               every tab of the user after a move or undo
└── src/core/{job_worker,research_worker}.c   Detached background-job sequencers*
```

**The session unit.** `session_manager.c` (the dispatch), `session_history.c` (history lifecycle),
`session_prefix.c` (the append-only request: frozen prefix, appended changes and turn context, live withdrawal),
`session_compaction.c`, `session_focus.c`, `prompt_builder.c` and `session_image_hold.c` call into each other: read
them as one Layer-2 unit, not as separate modules with a direction between them.

**Deliberate upward calls.** Each goes through a weak symbol or a registered function pointer, never an `#include`:

| From (lower) | To (higher) | How |
|---|---|---|
| `src/image_store.c` | `session_image_hold.c` (which unbound images a live session still holds) | weak `session_images_held` |
| `session_compaction.c` | the WebUI's compaction marker | weak `session_compaction_client_notice` |
| `session_focus.c` | the WebUI's context panel | weak `session_focus_client_notice` |
| `src/core/ota_rollout.c` | the satellite transport | `ota_rollout_set_push_fn` |
| lower-layer broadcasts (scheduler, jobs, calendar, phone, ...) | `webui_broadcasts.c` | weak no-op default, strong WebUI override |
| `src/tools/email_service_move.c` (a move or undo changed a mailbox) | `webui_email_changed.c` (email_changed to every tab of the user) | weak `email_changed_notify` |
| `src/tools/email_account_lease.c` (a queued lease ticket was granted) | the WebUI email executor, which runs that task | `email_lease_set_hook` |

\* **Orchestration-unit note.** `job_worker.c` and `research_worker.c` physically live in
`src/core/` but are **application-orchestration units**: each is a detached top-of-stack sequencer
that drives a whole background job (the generic tool loop, or `research_run_execute` at Layer 3).
They therefore include and call *upward* into Layer 3 (e.g. `research_worker.c` → `tools/research_run.h`).
This is a deliberate, **acyclic** exception to "downward only" — the Layer-3 controllers do not depend
back on the workers — mirroring how `dawn.c` (Layer 4) reaches every layer. Read them as Layer-4
sequencers that happen to sit in `src/core/` beside the job pool they build on, not as Layer-2 modules.

**Every build has sessions.** The session unit, turn queue, job pool and workers, prompt builder and worker pool
are compiled whether or not the WebUI is: the local microphone runs its turns on them, and the WebUI only adds the
server. The tools that start a background job (`job`, `deep_research`) are WebUI-only, since a job delivers its
result through the WebUI (the conversation it reports into, the missed-notification replay).

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

### Build-time invariant checks

Some rules can't be expressed in types, so scripts in `scripts/` enforce them. Each is a CMake target that `dawn`
depends on, so every build runs them, and they fail the build when broken:

| Target | Script | Rule |
|---|---|---|
| `no_ws_direct_write_check` | `check_no_ws_direct_write.sh` | Every WebSocket data frame goes through the WebUI's response queue, never a direct `lws_write()` |
| `messaging_send_funnel_check` | `check_messaging_send_funnel.sh` | Every messaging driver send goes through `messaging_deliver` (per-provider formatting and escaping) |
| `tool_action_kinds_check` | `check_tool_action_kinds.sh` | Every tool's action-kinds table names its own actions, and each prepare names a listed confirm |
| `llm_blocks_confined_check` | `check_llm_blocks_confined.sh` | Stored turn blocks (`messages.llm_blocks`) are read only to rebuild an LLM request |
| `message_kind_confined_check` | `check_message_kind_confined.sh` | Request-context rows (a `messages.kind`) are never shown, searched, counted or extracted |
| `user_removal_marked_check` | `check_user_removal_marked.sh` | A delete a user makes is marked as their removal, so it is withdrawn from conversations |
| `blob_marker_sync_check` | `check_blob_marker_sync.sh` | The chat-attachment blob marker agrees across its producer, parser and orphan sweep |
| `no_raw_strncpy_check` | `check_no_raw_strncpy.sh` | No raw `strncpy`/`strncat` in directories moved to `safe_strscpy` |
| `no_process_mgmt_check` | `check_no_process_mgmt.sh` | No process-management calls in the MCP bridge or code-project sources |

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
│  Compaction      — one per session, joinable: a long   │
│                    history summarized ahead of its turn │
│  DB storage      — auth.db's checkpoints (own connection,│
│                    off the global mutex) + free pages  │
│                    returned to the disk in chunks      │
│  Session reaper  — finishes destroyed sessions once    │
│                    their last reference is released    │
│  Forget/withdraw — on demand: removes a user's         │
│                    forgotten items from live and stored│
│                    conversations                       │
│  Embed backfill  — embeds facts stored without an      │
│                    embedding (request queue + startup  │
│                    sweep)                              │
│  Spec decode     — always-on WebUI, shadow mode only: a│
│                    decode at a pause, logged for       │
│                    agreement, never used               │
│  Satellite remap — detached per remap: saves the       │
│                    previous user's conversation,       │
│                    applies the new user                │
│  Conv memory     — detached: counts/forgets memories   │
│                    learned from a conversation made    │
│                    private                             │
│  Stocks refresh  — pushes quotes to open stocks panels │
│                    (every 30s in market hours); idle   │
│                    when no panel is open               │
│  Email exec      — 4 workers, started on the first     │
│                    WebUI email request and kept until  │
│                    shutdown: the panel's email work,   │
│                    off the lws thread                  │
│  Email fan-out   — transient, one per enabled account  │
│                    (≤16) for an all-accounts email     │
│                    search from the tool, joined before │
│                    the search returns                  │
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

Per-session locks (src/core/session_manager.c):
  session->history_mutex    — session conversation history
  session->metrics_mutex    — session-scoped metrics (tokens, timings)
  session->fd_mutex         — WebSocket file-descriptor state
  session->ref_mutex        — session reference counting
  session->llm_config_mutex — per-session LLM config overrides
  session->tools_mutex      — the tools running now, and a render_visual result waiting for the reply to save
  (llm_config_mutex, history_mutex, metrics_mutex and tools_mutex are leaves: never two held at once,
   copy under the lock; the order is session_manager_rwlock, ref_mutex, fd_mutex, then one of these)

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
  focus_source::s_registry_mutex (src/core/focus/focus_source.c) — the retrieval-adapter registry (LEAF: held only to
                                                                     register or clear an adapter; adapters register once at
                                                                     startup, so a turn's focus_compose reads the registry
                                                                     without it)
  llm_tools::llm_tools_mutex (src/llm/llm_tools*.c)         — the LLM tool table + cached schema hashes; taken BEFORE the tool registry's own mutex (schemas are built from registry lookups), never after it
  session_prefix::s_withdraw_mutex (src/core/session_prefix.c) — the withdraw worker's queue (LEAF: never held across a withdrawal)
  auth_db_storage::s_wake_mutex (src/auth/auth_db_storage.c) — wakes the storage thread (LEAF: taken by the WAL hook inside a
                                                                     commit, the auth_db mutex held; the thread never holds it while taking the auth_db mutex)
  llm_cache_monitor::s_keys_mutex + s_alert_mutex (src/llm/llm_cache_monitor.c) — cache keys, warning times, queued
                                                                     alerts (LEAVES: taken under history_mutex by a history
                                                                     rewrite; no other lock or callout while held)
  llm_cache_monitor::s_queue_mutex (src/llm/llm_cache_monitor.c) — usage records waiting for the 1-second flush (LEAF: held
                                                                     only to queue, copy out or remove rows; never across the
                                                                     database write)
  tool_call_challenge::s_mutex (src/core/tool_call_challenge.c) — actions waiting for a reply code (LEAF: held only to find,
                                                                     add, hand out or drop an entry; never across a tool call or a send)
  session_reaper::s_mutex (src/core/session_reaper.c)          — the reaper's list of destroyed sessions (LEAF: never held while
                                                                     finishing one; a release to zero wakes it after dropping ref_mutex)
  webui_music_server::s_registry_mutex (src/webui/webui_music_server.c) — authenticated music sockets (taken after
                                                                     s_conn_registry_mutex, before s_music_teardown_mutex; other threads only mark a
                                                                     socket to close and wake the music thread, which alone releases its session reference)
  tool_result_store::s_cache_mutex (src/core/tool_result_store.c) — the parsed-tree cache's slots (LEAF: may be taken under
                                                                     history_mutex; held only to find, pin, insert and release a slot, never across a render)
  tool_result_store::s_big_parse_mutex (src/core/tool_result_store.c) — one uncacheable tree parsed and used at a time (held across that
                                                                     read's render; takes no other lock, never taken under history_mutex or the auth_db lock)
  image sweep (src/blob_store.c → src/core/session_image_hold.c) — no lock of its own: the unbound-image sweep chooses a batch under the
                                                                     auth_db lock, RELEASES it, asks what holds the batch (the session registry's
                                                                     rwlock and then job_manager::s_pool_mutex, each only to snapshot ids, released;
                                                                     then each owner's session history_mutex in turn), and takes the auth_db lock
                                                                     again to delete.  Never run image_store_cleanup / image_store_reclaim_unbound
                                                                     while holding a history_mutex or the auth_db lock
  job_manager::s_pool_mutex (src/core/job_manager.c)       — background-job session pool (REGISTRY tier, like session_manager_rwlock: released before any ref-cond wait, session_free, or conv_db_*/scheduler_* callout)
  job_reinvoke::s_inflight_mutex (src/core/job_reinvoke.c) — per-parent reinvoke in-flight set (leaf)
  memory_embed_backfill::s_backfill_mutex (src/memory/memory_embed_backfill.c) — embedding-backfill request queue (LEAF; never held across an embed or DB call; joins an already-exited worker while held — safe only because the worker takes no lock after clearing s_backfill_running)
  document_embed_cache::s_cache.mutex (src/tools/document_embed_cache.c) — in-memory document-chunk embeddings (LEAF: never held across a database call; a rebuild reads pages outside it, then installs; scoring holds only this)
  memory_embeddings_entity::s_ent.mutex (src/memory/memory_embeddings_entity.c) — per-user entity-embedding copies (LEAF: held only to look up, score and install; a copy is read from the DB and name-stemmed with no cache lock held; invalidation is lock-free atomics, safe under the auth_db lock)
  memory_extraction::s_extraction_mutex + s_extraction_cond (src/memory/memory_extraction.c) — per-user extraction slots (leaf); the cond var lets a forget wait out a user's in-flight extraction (memory_extraction_hold_user)
  email_account_lease::s_mutex (src/tools/email_account_lease.c) — the per-account IMAP lease table (held only to queue, hand over or
                                                                     drop a waiter, never across I/O).  The LEASE itself is the outermost
                                                                     email lock: its holder may take the OAuth per-account mutex and then the
                                                                     auth_db lock; nothing takes it while holding those, the draft/trash
                                                                     mutexes or s_conn_registry_mutex; the release hook runs with no lease mutex held.
                                                                     A thread re-taking its own lease is caught; a ticket holder has no thread,
                                                                     so code running under a ticket's lease asserts email_lease_is_held
                                                                     (that the account's lease is held by someone, not that it is this
                                                                     ticket's: a debug check, not a guard)
  webui_email_exec::s_mutex (src/webui/webui_email_exec.c) — the WebUI email executor's run queue, sessions' slots and which
                                                                     user each worker serves.  Taken BEFORE the lease mutex (a worker asks for a
                                                                     lease, a cancel withdraws a ticket) and before a join's deliver_mutex; never
                                                                     held while releasing a lease or setting the lease hook (either may call the
                                                                     hook, which takes it) or while running account work.  A request's free_ctx takes no locks (it may
                                                                     run under s_mutex)
  webui_email_exec join->deliver_mutex (src/webui/webui_email_exec.c) — one per request: its cancel and its result's send are
                                                                     ordered by it (a cancelled request's result is never sent).  Order: s_mutex →
                                                                     deliver_mutex → the send queue's lock.  The reply's session is looked up (session
                                                                     registry, then its metrics_mutex for the owner check) BEFORE the deliver_mutex is
                                                                     taken.  A deliberate module-before-session exception to rule 1: the send only
                                                                     queues, so nothing waits on a session while it's held
  email_undo::s_mutex (src/tools/email_undo.c) — the trash/archive undo tokens (LEAF: held only to add, claim, finish or release
                                                                     a token; never across a call into anything else)
  email_service_move::s_pace_mutex (src/tools/email_service_move.c) — Gmail's per-account call pacing, shared by the tool and
                                                                     the panel (LEAF: held only to reserve a slot; the wait sleeps with it released)
  email_service_move::s_caps_mutex (src/tools/email_service_move.c) — which accounts can trash and archive, as last learned (LEAF:
                                                                     held only to find, note or read a slot; never across I/O or another lock)
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
   OLOG_ERROR("Failed to allocate response buffer");
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
      OLOG_ERROR("Invalid parameters");
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
   OLOG_WARNING("Send failed, retry %d/%d", retry_count + 1, MAX_RETRIES);
   sleep(1 << retry_count);  // 1s, 2s, 4s
   retry_count++;
}
```

**Graceful degradation** (feature availability):

```c
if (gpu_available) {
   ctx = asr_whisper_init(model_path);
} else {
   OLOG_WARNING("GPU not available, using CPU-only ASR");
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
provider = "openai"               # "openai", "anthropic", "gemini", "openrouter"
model = "gpt-4o"

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

**`dawn.h`** — compile-time fallbacks: `APPLICATION_NAME`, `AI_NAME`. Audio devices and MQTT broker defaults live in `src/config/config_defaults.c`.

**`include/prompts.h`** — every prompt and directive sent to a model, including the defaults `dawn.toml` can replace: the persona (`AI_PERSONA_TEMPLATE`), the voice-output directives (`DEFAULT_VOICE_OUTPUT_DIRECTIVE`, `_WEBUI`) and the ASR disambiguation hint.

**`models.toml`** — model context-window registry (per-model-prefix → max input tokens for OpenAI/Anthropic/Gemini). Read-only reference data loaded once by `llm_context.c` at startup; **exempt from the `dawn.toml` settings round-trip** (never rewritten). Edit + restart to update; no rebuild. OpenRouter/local windows are fetched live and not listed. See [MODELS_TOML_DESIGN.md](docs/MODELS_TOML_DESIGN.md).

**Tool registry** — tools are defined as compile-time `tool_metadata_t` structs in `src/tools/*.c` and registered in `src/tools/tools_init.c` via `tools_register_all()`. See [command-processing.md](docs/arch/command-processing.md).

### WebUI Settings Panel Mapping

The WebUI settings panel (`www/js/ui/settings.js`) defines a `SETTINGS_SCHEMA` that maps to `dawn.toml` sections:

| WebUI Section      | Config Section                        | Notes                                           |
| ------------------ | ------------------------------------- | ----------------------------------------------- |
| Language Model     | `[llm]`, `[llm.cloud]`, `[llm.local]` | Provider (incl. OpenRouter) + model selection |
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
| Deep Research      | `[research]`                          | Master enable (opt-in; the `deep_research` tool refuses until on) + per-run budgets (rounds, tool calls, input-token ceiling, round-digest cap, sources-to-answer, saturation-stop dry-round count, stale-question retire count, completeness-critic re-arm cap). A research run IS a background job, so it also obeys `[jobs]` caps. One debug knob (`capture_revisions`) is parsed + round-tripped but not yet enforced, so it is deliberately **not** surfaced in the panel |

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

AGX Orin 64GB MAXN. **TTFT is dominated by prompt size**, so a single latency
figure is meaningless without stating which prompt it used. DAWN has three
different ones in play:

| Context | Prompt size |
|---|---|
| `test_llama_performance.sh` (speed harness) | ~66 tokens |
| `test_llm_quality.py` (FRIDAY suite) | 1,145 tokens |
| **Real DAWN traffic** | **median 2,360**, p90 ~6k |

Prompt processing is near-linear, so the durable constant is a rate:
**Preset J processes ~482 tok/s**, giving `TTFT ≈ prompt_tokens ÷ 482`.

| Component                                | Latency    | Notes                              |
| ---------------------------------------- | ---------- | ---------------------------------- |
| ASR (Whisper base)                       | ~110 ms    | GPU accelerated                     |
| TTFT @ ~66-tok prompt                    | 264-332 ms | Preset J, benchmark only            |
| TTFT @ 1,145-tok suite prompt, **cached**| ~0 s       | prefix shared across tests → free   |
| **TTFT @ 2,360-tok production median**   | **~3.8 s** | Preset J, measured over 2,318 turns |
| TTS (Piper)                              | ~200 ms    | First sentence                      |
| **Total, a typical live voice turn**     | **~4.1 s** | ASR + production TTFT + TTS         |

Earlier revisions of this table claimed ~1.3 s total. That came from the
benchmark's small prompt, not production traffic; p90 in production is 12.7 s
and p99 is 26.4 s. Preset A (Qwen3 4B, no vision) is far better on this axis.

**Prompt caching is as significant as the model.** The FRIDAY suite reuses one
system prompt across all 13 tests, so after the first call the prefix is cached
and prompt processing costs ~nothing — its 0.75 s median total is essentially
generation time alone. Production is not so lucky: the same logs show 1,749
full re-process events from cache misses, and that is why the production median
is 3.8 s while a benchmark at a *larger* prompt finishes in under a second.
DAWN's append-only conversation requests (`session_prefix.c`: a frozen system
prompt, later changes appended) exist precisely to keep that prefix cacheable. Full detail, per-model rates and the
measured size→TTFT curve live in
[services/llama-server/README.md](services/llama-server/README.md#latency-what-actually-governs-perceived-speed).

**Streaming advantage**: with streaming LLM + TTS the user hears the *first
sentence* as soon as it is synthesized, rather than waiting for the whole
response (~3 s+). It is first-sentence time that gates perceived latency, not
total generation time — which is why TTFT, not tok/s, is the number to watch.

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
