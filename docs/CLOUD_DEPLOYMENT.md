# Cloud Deployment Guide

DAWN can run on a standard x86_64 Linux cloud server. All subsystems — WebUI, cloud LLM,
calendar, email, memory, contacts, documents, scheduler, MQTT — are network-first and work
without modification. Local audio capture (microphone/wake word) is the only feature that
requires physical hardware.

---

## Platform Independence

### CUDA — Auto-detected

CUDA is auto-detected on any platform via the shared `cmake/DawnCUDA.cmake` module. It searches
for the CUDA toolkit at `/usr/local/cuda*`, `/opt/cuda`, or `nvcc` on PATH. If found, Whisper
uses GPU-accelerated inference. Use `-DENABLE_CUDA=OFF` to disable explicitly.

### ARM NEON — Optional

`src/core/embedding_engine.c` uses NEON intrinsics for vectorized cosine similarity when
`__ARM_NEON` is defined. On x86_64, the scalar fallback path compiles automatically. No
performance-critical hot loop depends on NEON — embedding queries are infrequent.

### Platform Detection

`CMakeLists.txt` auto-detects platform via filesystem probes:
1. `/etc/nv_tegra_release` → Jetson
2. `/sys/firmware/devicetree/base/model` → Raspberry Pi
3. Otherwise → generic (no platform-specific flags set)

On x86_64 cloud servers, detection falls through cleanly. Optionally pass `-DPLATFORM=AUTO`.

### Argon2id Tuning

Password hashing uses 16MB memory cost on non-RPi platforms (8MB on RPi). Cloud servers have
plenty of RAM — no adjustment needed.

---

## ML Inference on CPU

All three ML components support CPU-only inference:

| Component | Library | GPU Path | CPU Path |
|-----------|---------|----------|----------|
| ASR (Whisper) | whisper.cpp / GGML | CUDA (Jetson) | CPU (automatic when `GGML_CUDA=OFF`) |
| TTS (Piper) | ONNX Runtime | CUDA EP | CPU EP (default on x86_64) |
| Embeddings | ONNX Runtime | CUDA EP | CPU EP (default on x86_64) |

CPU inference is slower but functional. For cloud deployments using `llm.type = "cloud"`, ASR
and TTS are only relevant if satellites connect to the server (Tier 2 audio streaming). If the
server is WebUI-only, ASR/TTS are unused at runtime.

---

## Required Dependencies

All packages are available in standard Ubuntu/Debian x86_64 repositories or as prebuilt
binaries.

### Core (always required)

```bash
# Build tools
sudo apt install cmake build-essential pkg-config

# Core libraries
sudo apt install libcurl4-openssl-dev libjson-c-dev libssl-dev libsqlite3-dev
sudo apt install libsodium-dev libwebsockets-dev libopus-dev libspdlog-dev

# MQTT
sudo apt install libmosquitto-dev

# Audio (libraries needed at link time even if no hardware present)
sudo apt install libasound2-dev libpulse-dev libflac-dev libsamplerate0-dev
```

### ML / Speech

```bash
# ONNX Runtime (CPU) — for Piper TTS and embedding engine
# Install from https://github.com/microsoft/onnxruntime/releases (x86_64 Linux CPU)
# Or: sudo apt install libonnxruntime-dev  (if packaged)

# Piper TTS dependencies
sudo apt install libespeak-ng-dev
# piper-phonemize: build from source or install prebuilt
# https://github.com/rhasspy/piper-phonemize

# Whisper.cpp — built as submodule, no separate install needed
```

### Optional

```bash
# Music playback codecs
sudo apt install libmpg123-dev libvorbis-dev

# Document processing (RAG)
sudo apt install libmupdf-dev libzip-dev libxml2-dev

# Calendar (CalDAV)
sudo apt install libical-dev

# Email (reading messages; required unless built with -DDAWN_ENABLE_EMAIL_TOOL=OFF)
sudo apt install libgmime-3.0-dev
```

---

## MQTT Broker

MQTT is always required — there is no compile-time flag to disable it. The daemon initializes
MQTT at startup and uses it for device command routing.

For cloud deployments:

```bash
# Install and start Mosquitto
sudo apt install mosquitto mosquitto-clients
sudo systemctl enable mosquitto
sudo systemctl start mosquitto
```

Configure in `dawn.toml`:
```toml
[mqtt]
broker = "localhost"
port = 1883
```

If no home automation devices will connect, MQTT runs idle with negligible resource usage.

---

## Audio Subsystem

When audio dev packages are installed, ALSA and PulseAudio are linked at build time. With
`SERVER_ONLY=ON`, audio libraries are optional — if not found, linking is skipped entirely.

At runtime in server mode (`--server` flag or `[general] mode = "server"`):
- Local audio capture and playback are skipped entirely
- TTS engine still initializes (for WebUI/satellite audio via WebSocket)
- ASR engine still initializes (for satellite speech recognition)
- No local microphone, speaker, AEC, VAD, or boot greeting

**Note:** If ALSA/PulseAudio libraries are linked but no audio daemon is running (common on
headless servers), `libpulse` may log harmless warnings to stderr at startup due to its
constructor function attempting to connect. These can be ignored.

For WebUI-only deployments, local audio is irrelevant. Satellite devices (Tier 1 RPi, Tier 2
ESP32) connect over WebSocket and handle their own audio I/O.

---

## Build

```bash
# Clone with submodules (whisper.cpp)
git clone --recursive https://github.com/The-OASIS-Project/dawn.git
cd dawn

# Configure (CPU-only, no CUDA)
cmake --preset debug

# Build
make -C build-debug -j$(nproc)
```

The build should complete without errors on x86_64 Ubuntu 22.04+.

---

## Configuration

Minimal `dawn.toml` for a cloud WebUI server:

```toml
[general]
ai_name = "friday"
timezone = "America/New_York"

[llm]
type = "cloud"

[llm.cloud]
# provider is auto-detected from available API keys (Claude > OpenAI > Gemini)
# Uncomment to override: provider = "claude"  # or "openai", "gemini"
model = "claude-sonnet-4-20250514"

[asr]
engine = "whisper"
model_path = "models/whisper.cpp/ggml-base.en.bin"

[tts]
voice_model = "models/en_US-lessac-medium.onnx"

[audio]
backend = "pulse"             # or "alsa" — required at compile time

[webui]
enabled = true
bind_address = "0.0.0.0"     # Listen on all interfaces
port = 3000
# For HTTPS (recommended for cloud):
# https_enabled = true
# ssl_cert_path = "/etc/letsencrypt/live/yourdomain/fullchain.pem"
# ssl_key_path = "/etc/letsencrypt/live/yourdomain/privkey.pem"

[mqtt]
broker = "localhost"
port = 1883

[network]
session_timeout_minutes = 30
```

API keys in `secrets.toml`:
```toml
claude_api_key = "sk-ant-..."
# openai_api_key = "sk-..."
```

---

## Run

```bash
LD_LIBRARY_PATH=/usr/local/lib ./build-debug/dawn --config dawn.toml
```

Access WebUI at `http://your-server:3000` (or `https://` if SSL configured).

---

## What Works on Cloud

| Feature | Status | Notes |
|---------|--------|-------|
| WebUI | Works | Full functionality |
| Cloud LLM (OpenAI, Claude, Gemini) | Works | Primary use case |
| Local LLM (Ollama) | Works | Install Ollama on same server |
| Calendar (CalDAV/Google) | Works | Network-only |
| Email (IMAP/SMTP/Gmail) | Works | Network-only |
| Memory / Contacts | Works | SQLite, local storage |
| Document Search (RAG) | Works | CPU embedding inference |
| Scheduler | Works | Timers, alarms, reminders |
| MQTT commands | Works | Install Mosquitto locally |
| OAuth 2.0 flows | Works | Needs valid redirect URL |
| Satellite connections (Tier 1) | Works | RPi satellites connect via WebSocket |
| Satellite connections (Tier 2) | Works | ESP32 satellites connect via WebSocket |
| ASR (Whisper) | Works | GPU with CUDA, CPU fallback |
| TTS (Piper) | Works | CPU inference |
| Local microphone / wake word | No | Requires audio hardware |
| CUDA acceleration | Works | Auto-detected when toolkit + driver present |

---

## x86_64 Port — Implementation Status (2026-03-20)

All blockers and problems from the original compatibility audit have been resolved.
See `docs/GETTING_STARTED_SERVER.md` for the full setup guide.

### What was implemented

**Build system (Package A):**
- CUDA auto-detection via shared `cmake/DawnCUDA.cmake` (works on Jetson, x86_64, any platform)
- Harfbuzz discovery via `pkg_check_modules` (no hardcoded arch paths)
- `SERVER_ONLY` CMake option makes audio libraries optional
- `server` and `server-debug` CMake presets
- `clang-format-14` enforced for consistent formatting across platforms

**Server runtime mode (Package B):**
- `--server` CLI flag and `[general] mode = "server"` config option
- `AUDIO_BACKEND_NONE` skips local audio hardware
- TTS engine initializes without local playback device (WebSocket output only)
- ASR engine initializes for WebUI/satellite audio processing
- Boot greeting, AEC, VAD, music player skipped in server mode
- Hardcoded `/home/jetson/` paths replaced with `$HOME`-based paths (fail-closed when unset)

**LLM provider auto-detection:**
- Cloud provider auto-detected from available API keys (Claude > OpenAI > Gemini)
- Graceful fallback when configured provider unavailable
- Default config no longer hardcodes OpenAI as cloud provider

**Additional fixes:**
- Fresh-install DB schema includes all tables (memory, calendar, contacts, email, etc.)
- TTS shutdown handles all blocking states (synthesis, pause, audio write)
- LWS log level set to errors/warnings only (suppresses lifecycle noise)
- WebSocket reconnect preserves TTS and Opus capability flags
- `setup_models.sh` downloads embeddings by default, finds all build directories
