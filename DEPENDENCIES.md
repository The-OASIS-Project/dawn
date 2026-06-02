# DAWN Dependencies

This document tracks all third-party dependencies used by the DAWN project.

## WebUI (JavaScript)

| Library | Version | License | Source | Description |
|---------|---------|---------|--------|-------------|
| marked.js | 15.0.12 | MIT | [GitHub](https://github.com/markedjs/marked) | Markdown parser |
| DOMPurify | 3.3.1 | Apache 2.0 / MPL 2.0 | [GitHub](https://github.com/cure53/DOMPurify) | XSS sanitizer |
| Chart.js | 4.4.1 | MIT | [GitHub](https://github.com/chartjs/Chart.js) | Charts in the visual-render tool (loaded into a sandboxed iframe) |
| qrcode-generator | 2.0.4 | MIT | [GitHub](https://github.com/kazuhikoarase/qrcode-generator) | QR code generation for satellite pairing in the admin UI |

**Local copies**: `www/js/marked.min.js`, `www/js/purify.min.js`, `www/js/vendor/chart.umd.js`, `www/js/vendor/qrcode-generator-v2.0.4.js`

## WebUI (Fonts)

| Font | License | Source | Files |
|------|---------|--------|-------|
| IBM Plex Mono | SIL OFL 1.1 | [GitHub](https://github.com/IBM/plex) | `IBMPlexMono-Regular.woff2`, `IBMPlexMono-Medium.woff2` |
| Source Sans 3 | SIL OFL 1.1 | [GitHub](https://github.com/adobe-fonts/source-sans) | `SourceSans3-Regular.woff2`, `SourceSans3-Medium.woff2` |

**Local copies**: `www/fonts/`

## C/C++ Libraries

### Core Dependencies (Required)

| Library | License | Purpose |
|---------|---------|---------|
| json-c | MIT | JSON parsing |
| libcurl | MIT/X | HTTP client for API calls |
| OpenSSL | Apache 2.0 | Cryptography, TLS |
| libmosquitto | EPL/EDL | MQTT client |
| libwebsockets | MIT | WebSocket server for WebUI |
| pthread | LGPL | Threading |
| MuPDF | AGPL-3.0 (dual-licensed AGPL/commercial) | PDF text extraction for document upload |
| libzip | BSD-3-Clause | DOCX ZIP archive reading |
| libsodium | ISC | Encrypted credential/token storage (crypto_secretbox) |
| libical | LGPL 2.1 / MPL 2.0 | iCalendar parsing, RRULE expansion, timezone handling |
| libxml2 | MIT | XML parsing (CalDAV PROPFIND/REPORT responses) |
| freetype2 | FreeType (BSD-like) | MuPDF font rendering dependency |
| harfbuzz | MIT | MuPDF text shaping dependency |
| libmujs-dev, libgumbo-dev, libopenjp2-7-dev, libjbig2dec0-dev | Various (MIT/LGPL/BSD) | MuPDF static link dependencies |
| libstemmer (Snowball) | BSD-3-Clause | Porter2 stemming for BM25 keyword indexing (`src/memory/memory_stem.c`). Install: `sudo apt install libstemmer-dev`. |
| libgit2 (≥ 1.6) | GPLv2-with-linking-exception (GPL-compatible) | In-process git client for the coding-harness code-projects feature (`DAWN_ENABLE_CODE_PROJECTS`, default OFF). **Jammy apt ships 1.1 — too old**; build from source with the OpenSSL HTTPS backend (SSH off) via `INSTALL_LIBGIT2=true ./scripts/install.sh` (see `install_libgit2` in `scripts/lib/libs.sh`). Only required when the code-projects feature is enabled. |
| mosquitto | EPL/EDL | MQTT broker (runtime dependency) |

### Audio Processing

| Library | License | Purpose |
|---------|---------|---------|
| PulseAudio | LGPL 2.1+ | Audio capture/playback |
| FLAC | BSD-3-Clause | FLAC audio decoding |
| Opus | BSD-3-Clause | Audio codec for WebUI streaming |
| libsamplerate | BSD-2-Clause | Sample rate conversion |
| WebRTC Audio Processing | BSD-3-Clause | AEC3 echo cancellation (optional) |

### Speech Recognition (ASR)

| Library | Version | License | Purpose |
|---------|---------|---------|---------|
| Vosk | - | Apache 2.0 | Offline speech recognition |
| whisper.cpp | - | MIT | Alternative ASR engine |
| Kaldi | Apache 2.0 | Vosk dependency |

### Text-to-Speech (TTS)

| Library | License | Purpose |
|---------|---------|---------|
| Piper | MIT | Neural TTS engine |
| ONNX Runtime | MIT | ML inference for Piper |
| piper-phonemize | MIT | Text-to-phoneme conversion |
| espeak-ng | GPL 3.0 | Phonemizer backend |

### CUDA (Jetson Only)

| Library | License | Purpose |
|---------|---------|---------|
| cuBLAS | NVIDIA EULA | Matrix operations |
| cuSPARSE | NVIDIA EULA | Sparse matrix operations |
| cuSOLVER | NVIDIA EULA | Linear algebra |
| cuRAND | NVIDIA EULA | Random number generation |

### ESP32 Satellite Firmware (Tier 2, vendored)

Arduino-ecosystem libraries (WebSockets, ArduinoJson, Adafruit GFX/ST7789/NeoPixel) are listed in
`dawn_satellite_arduino/README.md`. The OTA device-apply path adds one **vendored** dependency:

| Library | License | Purpose |
|---------|---------|---------|
| TweetNaCl | Public domain | Ed25519 signature verification of the OTA manifest on the ESP32 (`crypto_sign_ed25519_tweet_open`; bundles SHA-512). Dropped into `dawn_satellite_arduino/tweetnacl.{c,h}` unmodified from upstream (<https://tweetnacl.cr.yp.to/>). Verify-only; the sketch supplies the `randombytes()` stub. Chosen over mbedTLS because Ed25519 isn't enabled in the stock arduino-esp32 mbedTLS config. |

## Build Tools

| Tool | Version | Purpose |
|------|---------|---------|
| CMake | 3.21+ (3.28+ for ONNX Runtime build) | Build system |
| Meson | 0.63+ | WebRTC AEC build |
| GCC/G++ | 11+ | C/C++ compiler |
| pkg-config | - | Library discovery |
| arduino-cli | 1.5+ | ESP32 Tier-2 satellite firmware build (`dawn_satellite_arduino/build-esp32.sh`, local + CI) |

## Optional Commercial APIs

| Service | Purpose | Selection | Notes |
|---------|---------|-----------|-------|
| OpenAI | Cloud LLM | `[llm.cloud] provider = "openai"` | Sign up at openai.com |
| Anthropic Claude | Cloud LLM | `[llm.cloud] provider = "claude"` | Sign up at anthropic.com |
| Google Gemini | Cloud LLM | `[llm.cloud] provider = "gemini"` | Sign up at ai.google.dev |
| **OpenRouter** | Single OpenAI-compatible gateway fronting hundreds of models from many vendors | `[llm.cloud] use_openrouter = true` (gateway mode — routes ALL cloud traffic through OpenRouter) | Sign up at openrouter.ai. Key in `secrets.toml` under `openrouter_api_key` (or `OPENROUTER_API_KEY` env). No new build deps (reuses the existing OpenAI request/SSE path). |
| **Tavily** | LLM-optimized web search + URL extract (opt-in alternative to local SearXNG + FlareSolverr) | `[search] engine = "tavily"` and/or `[url_fetcher] fallback = "tavily"` | Sign up at tavily.com — free tier 1000 calls/month. Key in `secrets.toml` under `tavily_api_key` (or `TAVILY_API_KEY` env). No new build deps (uses existing libcurl + json-c). |

### Security note on API keys via environment variables

All API key environment variables (`OPENAI_API_KEY`, `ANTHROPIC_API_KEY`, `GEMINI_API_KEY`, `TAVILY_API_KEY`, etc.) are subject to standard process-environment risks: they appear in `/proc/$PID/environ` (owner-readable on Linux), can be captured by core dumps, and may leak through diagnostic tooling. For production deployments, prefer `secrets.toml` with `chmod 600` over env-var configuration. DAWN's log redaction (`ENV_SECRET` macro) prevents the key value from appearing in INFO logs but cannot prevent leakage through the kernel or third-party tooling.

## Model Files (Not Libraries)

These are model files required at runtime, not source dependencies:

- `vosk-model-en-us-0.22/` - Vosk English model
- `models/*.onnx` - Piper TTS voice models
- `models/whisper.cpp/` - Whisper ASR models (optional)

## License Compatibility

All dependencies are compatible with GPLv3:
- MIT, BSD, Apache 2.0 - Permissive, GPL-compatible
- LGPL - Compatible when dynamically linked
- EPL (Mosquitto) - Compatible via explicit dual-license (EDL)
- espeak-ng GPL 3.0 - Same license as project
- AGPL-3.0 (MuPDF) — Compatible, project is GPLv3
- ISC (libsodium) — Permissive, GPL-compatible

## Updating Dependencies

### WebUI Libraries
```bash
# marked.js
curl -sL "https://cdn.jsdelivr.net/npm/marked@latest/marked.min.js" -o www/js/marked.min.js

# DOMPurify
curl -sL "https://cdn.jsdelivr.net/npm/dompurify@latest/dist/purify.min.js" -o www/js/purify.min.js
```

### System Libraries
System libraries should be updated through the package manager (apt on Ubuntu/Debian).

See `README.md` for full installation instructions.

## Architectural Influences

These are not runtime dependencies — no third-party code is linked, bundled, or executed at runtime from these sources. Listed for attribution because portions of DAWN's design (algorithms, prompt text, scoring formulas) are adapted from them.

| Project | License | Source | Adapted into |
|---------|---------|--------|--------------|
| [mem0ai/mem0](https://github.com/mem0ai/mem0) | Apache 2.0 | © 2023 Taranjeet Singh | Memory subsystem: BM25 sigmoid normalization (`memory_bm25_*`), extraction prompt specificity rules (`MEMORY_EXTRACTION_PROMPT_TEMPLATE`), additive scoring composition pattern. Per-file `Adapted from mem0ai/mem0` comments mark the borrowing point of use. See `NOTICE` for the full attribution, summary of changes (including deliberate non-adoptions: no-echo rule and spread-attenuated entity boost), and rationale for deviations. See [atlas/dawn/memory/MEM0_ARCHITECTURAL_PARITY.md](https://github.com/The-OASIS-Project/atlas/blob/main/dawn/memory/MEM0_ARCHITECTURAL_PARITY.md) for the planning context (sealed historical reference for the closed program, archived 2026-05-17). |
