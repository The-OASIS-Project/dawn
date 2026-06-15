# D.A.W.N. — Your Own Personal, Private AI Assistant

**D.A.W.N.** is the central intelligence layer of [The OASIS Project](https://github.com/The-OASIS-Project) — an open-source voice-controlled AI assistant built for embedded Linux hardware. Think JARVIS from *Iron Man*, but real: an always-listening, conversational AI that controls your smart home, answers questions, searches the web, plays music, and more — running on hardware you own.

DAWN was designed from the ground up to be the closest thing to a production-quality JARVIS that actually exists. It listens for a wake word, understands natural speech, reasons about your request, and responds in a natural voice — all in real time. It remembers you — your preferences, your routines, the people and things you care about — and gets better the more you use it. It can run entirely offline with a local LLM, or connect to cloud providers like OpenAI, Claude, and Gemini. Multi-room satellite devices bring voice control to every room in your home, just like having JARVIS in every room of 10880 Malibu Point.

Unlike cloud-dependent assistants and plugin-based AI agents, DAWN runs entirely on your hardware. There is no extension marketplace, no third-party plugins, no arbitrary code execution. Every tool is compiled into the binary and audited at the source level. It can run fully offline — your voice, your data, your network, never leaving your home.

DAWN runs on platforms from a Jetson Orin to a Raspberry Pi 5 and supports multiple interfaces: a local microphone, a browser-based Web UI with voice and text input, and DAP2 satellite devices (Raspberry Pi or ESP32).

> **New to DAWN?** See **[GETTING_STARTED.md](GETTING_STARTED.md)** for a step-by-step setup guide.

<!-- TODO: Hero screenshot of WebUI conversation view -->

---

## Part of The OASIS Project

DAWN is one of seven components in **[The OASIS Project](https://github.com/The-OASIS-Project)** — an open-source effort to build a real, working Iron Man suit ecosystem. Each component is its own repo and works independently, but together they form a fully integrated system. DAWN is the brain; the others are the body.

DAWN itself is the closest thing to a real **JARVIS or FRIDAY** that exists today. A natural-voice assistant that listens, reasons across conversations, calls tools, controls the rest of the suit, and remembers you across sessions — your routines, the people you care about, the things you ask for again and again. 20+ built-in tools (calendar, email, smart home, music, search, memory, vision, more). Multi-room satellites put her in every room of the house. Cloud LLM or fully local; you pick.

The sibling components:

| Component | Role | Repo |
|---|---|---|
| **MIRAGE** | Heads-up display — dual micro-displays, switchable HUD screens, live video | [mirage](https://github.com/The-OASIS-Project/mirage) |
| **STAT** | Telemetry daemon — battery, power, system metrics, fault detection | [stat](https://github.com/The-OASIS-Project/stat) |
| **ECHO** | Cellular daemon — routes phone calls and SMS into DAWN (SIM7600G-H modem) | [echo](https://github.com/The-OASIS-Project/echo) |
| **AURA** | Helmet sensors — IMU, GPS, air quality, temperature, servo faceplate (ESP32-S3, FreeRTOS) | [aura](https://github.com/The-OASIS-Project/aura) |
| **SPARK** | Hand firmware — gesture detection, repulsor LED animations, wireless | [spark](https://github.com/The-OASIS-Project/spark) |
| **BEACON** | 3D-printable parts library — enclosures, mounts, chassis | [beacon](https://github.com/The-OASIS-Project/beacon) |

Everything is GPLv3. Cloud LLMs are optional — DAWN runs fully local if you want it to.

---

## See DAWN in Action

<!-- TODO: Demo video or animated GIF showing a voice interaction (wake word → question → spoken response) -->

---

## Features

### Voice Intelligence

- **Speech Recognition** — Whisper ASR with GPU acceleration on Jetson (2.3x–5.5x faster than real-time). Intelligent voice activity detection knows when you're speaking and when you've stopped.
- **Multi-Provider LLM** — Cloud: OpenAI GPT-5, Anthropic Claude 4.6, Google Gemini 2.5/3, or any model through the OpenRouter gateway. Local: llama.cpp or Ollama for fully offline operation. Runtime model switching via WebUI or voice.
- **Text-to-Speech** — Piper TTS with ONNX Runtime. Multiple voices included. Text preprocessing for natural phrasing.
- **Extended Thinking** — Deep reasoning mode for complex queries:
   - Claude: thinking budget control with collapsible blocks
   - OpenAI: reasoning effort (low/medium/high) for o1/o3/o4 models
   - Gemini: thinking mode for Gemini 2.5 Flash/Pro
   - Local: Qwen3 thinking mode with native template support
- **Cloud Rate Limiting** — Built-in throttling prevents you from getting rate-limited by cloud providers.
- **Streaming Responses** — DAWN starts speaking while still thinking, just like a real conversation. Perceived latency drops to ~1.3 seconds.

### Multi-Room Voice (DAP2 Satellites)

- **One Server, One Port, Three Client Types** — The WebUI server on port 3000 is the single entry point for browsers, Raspberry Pi satellites, and ESP32 satellites. No extra servers or ports required.
- **Tier 1 (Raspberry Pi)** — Local ASR/TTS, hands-free with wake word. Sends only text to the server. SDL2 touchscreen UI with KMSDRM backend.
- **Tier 2 (ESP32-S3)** — Streams raw PCM audio; server handles ASR/TTS. Push-to-talk operation.
- **Music Streaming** — Stream music to every room through your Tier 1 satellites.
- **No Double Answers** — When your local microphone and a nearby satellite both hear the same command, DAWN recognizes the duplicate and only one of them responds. Configurable dedup window under `[asr]`.
- **Over-the-Air Updates** — Push signed firmware and software updates from the daemon to your satellites without touching each device — Tier 1 (Raspberry Pi) and Tier 2 (ESP32-S3) both supported. Releases are signed offline (the daemon never holds the signing key) and verified against a baked-in public key on the device. Roll out to one device or the whole fleet from the WebUI admin panel or the `dawn-admin ota` CLI, with per-device version tracking and anti-rollback protection.
- **Easy Pairing** — Provision a new satellite by scanning a QR code from the WebUI admin panel — no manual token copying.

<!-- TODO: Screenshot of satellite SDL2 UI on a Raspberry Pi touchscreen -->

### Web Interface

- **Browser-Based** — Accessible on port 3000 from any device on your network.
- **Voice & Text Input** — Three input modes via unified action button: Send Text, Hold to Talk (push-to-talk), and Continuous Listening (always-on wake word detection with server-side VAD). Smart typing override temporarily switches voice modes to Send when text is present.
- **Real-Time Communication** — WebSocket-based with session persistence across page refreshes (30-minute timeout).
- **Settings Panel** — Live configuration editing from the browser. API keys with secure handling. Restart indicators for settings that require it.
- **User Management** — Cookie-based authentication with "Remember Me" (30-day sessions). Admin tools: create/delete users, reset passwords, unlock accounts. Per-user persona, location, timezone, units, and TTS speed preferences.
- **Conversation History** — Browse, search, continue, delete, and export (JSON/HTML) past conversations. One-click message copy. Per-conversation LLM settings (reasoning mode, tools mode).
- **7 Color Themes** — Cyan, Purple, Green, Orange, Red, Blue, and Terminal.
- **Vision & Documents** — Send images for AI analysis (upload, paste, drag-and-drop, camera capture). Attach text documents (Markdown, CSV, JSON, source code, and more) for LLM context.
- **Accessibility** — Keyboard navigation, screen reader support (ARIA), reduced motion preferences, WCAG-compliant touch targets. Mobile-friendly responsive design.
- **Debug Mode** — Toggle to view tool results, commands, and system messages.

<!-- TODO: Screenshot of WebUI on desktop and mobile -->

### Messaging Channels (Chat Apps & SMS)

- **Talk to DAWN from the apps you already use** — Link a **Telegram**, **Slack**, **Discord**, or **SMS** conversation to your account and get the full assistant (tools, memory, scheduler) from your phone or desktop chat client, no WebUI required.
- **One-time link flow** — Generate a link code in the WebUI or via `dawn-admin`, send `/link CODE` from the chat app, and you're connected. Drivers load only when their token is configured.
- **Forever-conversations** — Each channel maps to one persistent conversation that shows up in WebUI history and feeds memory extraction like any other session. `/new` resets the thread.
- **Scheduler delivery** — Briefings, reminders, and alarms can be delivered to a messaging channel instead of spoken aloud — "send me a morning briefing on Telegram."
- **Channel management** — Rename, unlink (history preserved), and re-enable channels from the WebUI panel or the `dawn-admin messaging` operator commands.
- **Setup**: [docs/MESSAGING_CHANNELS_SETUP.md](docs/MESSAGING_CHANNELS_SETUP.md).

<!-- TODO: Screenshot of a Telegram/Slack conversation with DAWN -->

### Smart Home & IoT

- **Home Assistant** — Control lights (on/off/brightness/color/color temperature), climate, locks, covers, media players. Activate scenes, trigger scripts and automations. Entity status queries with area-aware responses.
- **Fuzzy Name Matching** — "Turn on the living room light" works even if the entity is named slightly differently.
- **MQTT Device Control** — Extensible device callback architecture for integration with other OASIS components or external systems.

### LLM Tools

DAWN's LLM automatically invokes tools and incorporates results into responses. Multiple tool calls execute concurrently for faster answers.

- **Web Search** — Voice-activated search via SearXNG (self-hosted, privacy-focused) or optional [Tavily](https://tavily.com) (commercial, LLM-optimized; free tier 1000 calls/month). Categories: web, news, social, science, IT, Q&A, dictionary, academic papers. Time filtering for recent results (today, this week, this month).
- **Image Search** — Search and display images from the web. Images are fetched server-side (SSRF-protected with DNS pinning), cached locally, and served via zero-copy file serving. Click-to-enlarge lightbox in WebUI. Results displayed as inline markdown images.
- **URL Fetcher** — Fetch and read web pages. Optional FlareSolverr (headless browser) or Tavily `/extract` (commercial, LLM-optimized) fallback for JavaScript-heavy sites. SSRF-protected. Untrusted-content framing on tool results.
- **Weather** — Real-time weather and forecasts via Open-Meteo API (free, no API key required).
- **Calculator** — Expression evaluation (`+ - * / ^`, `sqrt`, trig, `factorial`) plus a digit-perfect **exact mode** for arbitrary-precision integer results — every digit of `52!` or `2^256`, no scientific-notation rounding. Also unit conversion, base conversion, and random numbers.
- **CalDAV Calendar** — CalDAV calendar integration (Google Calendar via OAuth 2.0, iCloud, Nextcloud, Radicale). Query today's events, search by keyword, add/update/delete events across multiple accounts. Filter queries by calendar name.
- **Email** — Multi-account IMAP/SMTP email (Gmail via OAuth 2.0, iCloud, Outlook, Fastmail, self-hosted). Check inbox, read messages, search, send with two-step confirmation, trash with confirmation, archive. Pagination for large result sets. Per-account read-only flag. Contacts system for recipient resolution.
- **Scheduler** — Timers, alarms, reminders, and scheduled tool execution.
   - "Set a 10 minute timer", "Wake me up at 7 AM", "Remind me to call Mom at 3pm"
   - Recurring events (daily, weekdays, weekends, weekly, custom days)
   - Audible chimes with configurable volume, snooze, and dismiss via voice or WebUI
   - Scheduled tasks: "Turn off the lights at midnight" (executes any registered tool)
- **Sound Effects** — Play sound effects from the OASIS armor system (SPARK repulsor → AURA → MIRAGE → DAWN). Concurrent playback (4 slots), path traversal protection, configurable sound directory.
- **Multi-Step Plans** — Complex workflows execute locally in a single LLM round trip. The plan executor interprets a JSON DSL with conditional logic, loops, and variable binding — "If I don't have any alarms, set one for 7am" runs as one plan instead of multiple back-and-forth tool calls.
- **Visual Diagrams** — The LLM generates inline SVG/HTML visuals to explain concepts. Flowcharts, architecture diagrams, data charts (Chart.js), interactive widgets, UI mockups, and illustrations render directly in the conversation with clickable nodes for drill-down. Visuals persist across page refresh and render inline within the message text. Design guidelines loaded on demand via the two-step instruction pattern keep the system prompt lightweight. Download button for SVG/HTML export.
- **Document Search (RAG)** — Upload documents and ask questions about their content.
   - Semantic search across uploaded PDFs, DOCX, TXT, and Markdown files
   - Paginated document reading for full-document summaries
   - Hybrid scoring: vector cosine similarity + keyword boosting
   - Per-user document isolation with admin-controlled global sharing
   - WebUI Document Library panel with drag-and-drop upload and admin document management
- **Notes & Reference Text** — Ask DAWN to file notes and reference text it can search and recall later ("save this as my public bio", "what's on my packing list?").
   - Hybrid keyword search tuned for short, named notes (built on the same Document Search store)
   - Surgical edits: append to or replace part of a note without re-saving the whole thing
   - Version history with one-step undo — recover the previous version or restore a deleted note
   - Filed reference text is kept out of semantic memory by default, so the canonical copy lives only in the note store (configurable under `[memory]`)

### Persistent Memory

DAWN remembers facts, preferences, and relationships about its users across sessions — and forgets them on request.

- **Memory Tool** — "Remember that I'm vegetarian", "What do you know about me?"
- **Hybrid Search** — Finds the right memory whether you use the exact words or just describe what you're looking for. Keyword and meaning-based search working together.
- **Entity Graph** — Automatically learns the people, places, pets, and projects in your life and how they relate to each other.
- **Automated Extraction** — Facts, preferences, entities, and relations extracted at session end.
- **Crash-Resilient Extraction** — Conversations interrupted by a daemon crash mid-extraction are picked up by a background recovery worker, configurable under `[memory.recovery]`.
- **Privacy Toggle** — Mark conversations as private to prevent memory extraction (Ctrl+Shift+P in WebUI).
- **Confidence Decay** — Unused memories naturally fade; accessed memories are reinforced.
- **Contacts** — Store email addresses, phone numbers, and addresses for people DAWN knows. Upload contact photos (compressed and circular-cropped in the WebUI) that appear as thumbnails in the contact list and are sent as base64 in HUD notifications for incoming calls and SMS. Linked to the entity graph. Used by the email and phone systems for contact resolution. Managed via voice ("Save Bob's email") or the WebUI Contacts tab.
- **Entity Merge** — Combine duplicate entities ("Dawn" + "Dawn Smith"), transferring all relations and contacts. Available via voice or WebUI.
- **Memory Viewer** — Browse, search, and manage all memory types in the WebUI. Five tabs: Facts, Preferences, Summaries, Graph, Contacts.
- **Import / Export** — Export memories as DAWN JSON (lossless backup) or human-readable text (portable). Import from Claude, ChatGPT, or other AIs — paste text or upload a file. Preview before committing with automatic duplicate detection.
- **Per-User Isolation** — Each user's memory is separate in multi-user households.

### Music Playback

- **Multi-Format** — FLAC, MP3, and Ogg Vorbis with auto-format detection and mixed-format playlists.
- **Local + Plex Unified Library** — Local files and Plex Media Server tracks in a single searchable library. Duplicates automatically merged. Search by artist, title, album, or genre.
- **LLM Playlist Builder** — AI can search, add, remove, and clear queue tracks by voice. Genre-aware search.
- **Resume Playback** — Pause saves position, resume continues where you left off.
- **Opus Streaming** — Stream music to WebUI and DAP2 satellites via WebSocket.
- **Media Key Controls** — Hardware media keys and lock-screen/OS media controls play, pause, and skip DAWN's music, with track metadata shown in the OS now-playing UI.

---

## Hardware Recommendations

DAWN runs on a range of hardware. These tables can help you choose the right platform.

> **Runtime memory:** the daemon's resident set is roughly **~1.6 GB** on a Jetson with GPU-accelerated Whisper and a cloud LLM — most of it the CUDA runtime, not the models. A local LLM adds its model size on top (commonly +2–8 GB). See [ARCHITECTURE.md — Memory Management](ARCHITECTURE.md#memory-management) for the full breakdown.

### Server Hardware (runs the DAWN daemon)

#### Tier 1: Excellent (Production Ready)

| Platform | Price | AI Performance | Notes |
|----------|-------|----------------|-------|
| **Jetson Orin Nano Super** | ~$249 | 67 TOPS | Primary target. GPU Whisper ~0.1s RTF |
| **Jetson Orin NX** | ~$1,000 | 100 TOPS | Excellent headroom for all features |
| **Jetson AGX Orin 64GB** | ~$2,000 | 275 TOPS | Best for running large local LLMs alongside DAWN |

#### Tier 2: Good (Usable with minor tradeoffs)

| Platform | Price | AI Performance | Notes |
|----------|-------|----------------|-------|
| **Raspberry Pi 5 (8GB)** | ~$125 | CPU-only, ~1.0 RTF | Whisper base: ~6s for 10s audio |
| **Raspberry Pi 5 + AI Kit** | ~$200 | 13 TOPS | Hailo-8L helps vision, not ASR |

#### Tier 3: Not Recommended for Server

| Platform | Issue |
|----------|-------|
| **Raspberry Pi 4** | May work as a satellite, too slow for server use |
| **Raspberry Pi 3 / Zero 2** | Too slow, insufficient RAM |
| **32-bit ARM systems** | Limited memory, slow inference |

### Recommendations by Use Case

| Use Case | Recommended Platform | Why |
|----------|---------------------|-----|
| **Cost-conscious hobbyist** | Raspberry Pi 5 (8GB) ~$125 | Works with Whisper tiny/base, acceptable latency |
| **Best value** | Jetson Orin Nano Super ~$249 | GPU acceleration, real-time voice AI |
| **Production / commercial** | Jetson AGX Orin 32GB/64GB | Run 7B–13B local LLMs alongside the full voice pipeline |

---

## Getting Started

Choose the guide that matches your deployment:

| Deployment | Guide | Description |
|------------|-------|-------------|
| **Docker** | [docs/GETTING_STARTED_SERVER.md — Docker](docs/GETTING_STARTED_SERVER.md#docker-quickstart) | Fastest path — pull and run with Docker (CPU or CUDA) |
| **Jetson / Embedded** | [GETTING_STARTED.md](GETTING_STARTED.md) | Full setup with local microphone, speaker, wake word, and GPU |
| **x86_64 Server** | [docs/GETTING_STARTED_SERVER.md](docs/GETTING_STARTED_SERVER.md) | WebUI + satellite connections, no local audio hardware needed |
| **Cloud / x86_64 Server** | [docs/CLOUD_DEPLOYMENT.md](docs/CLOUD_DEPLOYMENT.md) | Platform independence, feature matrix, and architecture notes |

---

## Optional Features

These features are not required but extend what DAWN can do. Each links to its setup guide.

| Feature | What it does | Setup guide |
|---------|-------------|-------------|
| **CalDAV Calendar** | Query and manage calendar events by voice | [GETTING_STARTED.md — CalDAV](GETTING_STARTED.md#caldav-calendar) |
| **Email (IMAP/SMTP)** | Check, read, search, send, trash, and archive email by voice | [GETTING_STARTED.md — Email](GETTING_STARTED.md#email-imapsmtp) |
| **Messaging Channels** | Chat with DAWN from Telegram, Slack, Discord, or SMS | [docs/MESSAGING_CHANNELS_SETUP.md](docs/MESSAGING_CHANNELS_SETUP.md) |
| **Google OAuth** | Connect Google Calendar and Gmail via OAuth 2.0 (no app password needed) | [docs/GOOGLE_OAUTH_SETUP.md](docs/GOOGLE_OAUTH_SETUP.md) |
| **Home Assistant** | Control smart home devices by voice | [docs/HOMEASSISTANT_SETUP.md](docs/HOMEASSISTANT_SETUP.md) |
| **Code Projects** | Index repositories so the assistant can answer questions about your code (via the external cbm code-graph server) | [docs/CODING_PROJECTS.md](docs/CODING_PROJECTS.md) |
| **SearXNG Web Search** | Privacy-focused voice-activated web search | [GETTING_STARTED.md — SearXNG](GETTING_STARTED.md#searxng-setup-for-web-search) |
| **Plex Music Source** | Unified music library with local + Plex tracks | [GETTING_STARTED.md — Plex](GETTING_STARTED.md#plex-music-source) |
| **FlareSolverr** | Fetch JavaScript-heavy web pages | [GETTING_STARTED.md — FlareSolverr](GETTING_STARTED.md#javascript-heavy-sites-flaresolverr) |
| **Tavily** | Opt-in commercial search + URL extract (alternative to SearXNG / FlareSolverr) | [GETTING_STARTED.md — Tavily](GETTING_STARTED.md#tavily-commercial-search--url-extract) |
| **Local LLM** | Fully offline operation with llama.cpp or Ollama | [GETTING_STARTED.md — Local LLM](GETTING_STARTED.md#local-llm-free-no-api-key) |

---

## Documentation

| Document | Description |
|----------|-------------|
| **[GETTING_STARTED.md](GETTING_STARTED.md)** | Jetson / embedded installation and setup guide |
| **[docs/GETTING_STARTED_SERVER.md](docs/GETTING_STARTED_SERVER.md)** | x86_64 server mode setup guide |
| **[docs/CLOUD_DEPLOYMENT.md](docs/CLOUD_DEPLOYMENT.md)** | Cloud deployment architecture and status |
| **[ARCHITECTURE.md](ARCHITECTURE.md)** | System architecture, data flow, and threading model |
| **[CODING_STYLE_GUIDE.md](CODING_STYLE_GUIDE.md)** | Code formatting and development standards |
| **[docs/LLM_INTEGRATION_GUIDE.md](docs/LLM_INTEGRATION_GUIDE.md)** | LLM setup for cloud and local providers |
| **[docs/HOMEASSISTANT_SETUP.md](docs/HOMEASSISTANT_SETUP.md)** | Home Assistant integration setup |
| **[docs/MESSAGING_CHANNELS_SETUP.md](docs/MESSAGING_CHANNELS_SETUP.md)** | Link Telegram / Slack / Discord / SMS to DAWN |
| **[docs/WEBSOCKET_PROTOCOL.md](docs/WEBSOCKET_PROTOCOL.md)** | WebSocket protocol reference (all message types) |
| **[docs/DAP2_SATELLITE.md](docs/DAP2_SATELLITE.md)** | Tier 1 satellite (RPi) install, build, config, deployment, troubleshooting |
| **[dawn_satellite_arduino/README.md](dawn_satellite_arduino/README.md)** | Tier 2 satellite (ESP32-S3, Arduino) setup |
| **[docs/OTA_DESIGN.md](docs/OTA_DESIGN.md)** | Server→satellite over-the-air update system (design + operator guide) |
| **[docs/GOOGLE_OAUTH_SETUP.md](docs/GOOGLE_OAUTH_SETUP.md)** | Google OAuth 2.0 setup for Calendar and Email |
| **[docs/TOOL_DEVELOPMENT_GUIDE.md](docs/TOOL_DEVELOPMENT_GUIDE.md)** | Guide for adding new LLM tools |
| **[docs/CODING_PROJECTS.md](docs/CODING_PROJECTS.md)** | Code Projects (coding harness): import/link repos + cbm server setup |
| **[atlas archive](https://github.com/The-OASIS-Project/atlas/tree/main/dawn/archive)** | Historical design docs (memory, RAG, user auth, plan executor, scheduler, image search, CalDAV, email, etc.) |
| **[services/llama-server/README.md](services/llama-server/README.md)** | Local LLM service setup |
| **[test_recordings/BENCHMARK_RESULTS.md](test_recordings/BENCHMARK_RESULTS.md)** | ASR performance benchmarks |

---

## Contributing

Contributions are welcome! DAWN is part of The OASIS Project and is licensed under GPLv3.

- Follow the coding standards in [CODING_STYLE_GUIDE.md](CODING_STYLE_GUIDE.md)
- Format code with `./format_code.sh` before committing
- Add tests for new features
- See [ARCHITECTURE.md](ARCHITECTURE.md) for system design context

---

## Credits

- **[Piper TTS](https://github.com/rhasspy/piper)** (MIT License)
- **[Vosk ASR](https://alphacephei.com/vosk/)** (Apache 2.0)
- **[Whisper.cpp](https://github.com/ggerganov/whisper.cpp)** (MIT License)
- **[Silero VAD](https://github.com/snakers4/silero-vad)** (MIT License)
- **[ONNX Runtime](https://github.com/microsoft/onnxruntime)** (MIT License)
- **[espeak-ng](https://github.com/espeak-ng/espeak-ng)** (GPL v3+)
- **[Chart.js](https://www.chartjs.org/)** (MIT License)

## License

D.A.W.N. (Digital Assistant for Workflow Neural-inference) is licensed under the **GNU General Public License v3.0 or later**.

See individual dependencies for their respective licenses.

## Disclaimer

DAWN is provided "as is" without warranty of any kind. The maintainers and contributors of this project are not responsible for any costs, charges, or fees incurred through the use of third-party cloud services, including but not limited to OpenAI, Anthropic, Google, Plex, or any other API provider. Users are solely responsible for monitoring and managing their own API usage, rate limits, and associated billing. By using DAWN with cloud-based providers, you acknowledge that you have read and agreed to the respective provider's terms of service and accept full responsibility for any charges resulting from API calls made by this software.
