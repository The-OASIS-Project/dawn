# Getting Started with DAWN

Quick guide to get DAWN running. For full documentation, see [README.md](README.md).

## What You're Setting Up

DAWN provides two ways to interact:

1. **Local Voice Interface** - Uses your device's microphone and speakers. Requires audio device configuration. Say a wake phrase (e.g., "Hey Friday", "Okay Friday") followed by your command.

2. **Web UI** - Browser-based interface at `http://localhost:3000`. Supports both text and voice input. Voice requires HTTPS for remote access (see [SSL Setup](#ssl-setup-for-remote-voice)).

Both interfaces share the same AI backend (ASR, LLM, TTS).

## Prerequisites

- **OS**: Ubuntu 22.04+, Debian 12+, or Jetson Linux
- **RAM**: 4GB minimum (8GB recommended)
- **Audio**: Microphone and speakers (for local voice interface)
- **LLM**: Either a cloud API key (OpenAI, Claude, Gemini) or a local LLM server (llama.cpp, Ollama)

## 1. Install System Dependencies

```bash
sudo apt update && sudo apt install -y \
  build-essential cmake git pkg-config wget unzip autoconf automake libtool \
  libasound2-dev libpulse-dev libsndfile1-dev libflac-dev \
  libmosquitto-dev libjson-c-dev libcurl4-openssl-dev libssl-dev \
  libwebsockets-dev libopus-dev libsodium-dev libsqlite3-dev \
  libsamplerate0-dev libmpg123-dev libvorbis-dev libncurses-dev \
  meson ninja-build libabseil-dev
```

> **Jetson users**: CUDA runtime is pre-installed. For building GPU-accelerated components (ONNX Runtime, Whisper), you may need CUDA development headers. Check with `ls /usr/local/cuda/include/cuda.h`.

> **CMake version**: Presets require CMake 3.21+. Check with `cmake --version`.

## 2. Install Core Libraries

Four libraries are required. See [README.md](README.md#2-install-core-dependencies) for detailed instructions.

| Library | apt available? | Notes |
|---------|---------------|-------|
| spdlog | Yes (`libspdlog-dev`) | Logging library |
| espeak-ng | **No** - need rhasspy fork | Phoneme backend for TTS |
| ONNX Runtime | **No** | Inference engine for TTS/VAD |
| piper-phonemize | **No** | TTS frontend (requires ONNX + espeak-ng) |

**Step 1: Install spdlog from apt**
```bash
sudo apt install -y libspdlog-dev
```

**Step 2: Build espeak-ng from rhasspy fork** (apt version won't work)
```bash
sudo apt purge -y espeak-ng-data libespeak-ng1 2>/dev/null || true
git clone https://github.com/rhasspy/espeak-ng.git && cd espeak-ng
./autogen.sh && ./configure --prefix=/usr
make -j$(nproc) && sudo make LIBDIR=/usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH) install
cd ..
```

**Step 3: Install ONNX Runtime**

ONNX Runtime build varies by platform. See [README.md](README.md#onnx-runtime-with-cuda-support-for-jetson) for:
- Jetson with CUDA acceleration
- Raspberry Pi / ARM64 CPU-only
- x86-64 systems

> **Tip**: Pre-built packages may be available at [ONNX Runtime releases](https://github.com/microsoft/onnxruntime/releases).

**Step 4: Build piper-phonemize**
```bash
git clone https://github.com/rhasspy/piper-phonemize.git && cd piper-phonemize
mkdir build && cd build
cmake .. -DONNXRUNTIME_DIR=/usr/local -DESPEAK_NG_DIR=/usr
make -j$(nproc)

# Manual install (piper's make install has broken rules for system deps)
sudo cp -a libpiper_phonemize.so* /usr/local/lib/
sudo mkdir -p /usr/local/include/piper-phonemize
sudo cp ../src/*.hpp /usr/local/include/piper-phonemize/
sudo cp ../src/uni_algo.h /usr/local/include/piper-phonemize/
sudo ldconfig
cd ../..
```

## 3. Clone and Build

```bash
# Clone with submodules (required for Whisper and WebRTC AEC)
git clone --recursive https://github.com/The-OASIS-Project/dawn.git
cd dawn

# If you already cloned without --recursive:
git submodule update --init --recursive

# Build WebRTC audio processing (for echo cancellation)
cd webrtc-audio-processing && meson setup build && ninja -C build && cd ..

# Configure and build DAWN
cmake --preset default
cmake --build --preset default
```

> **Skip AEC**: If WebRTC build fails, you can disable it: `cmake --preset default -DENABLE_AEC=OFF`

The binary will be at `build/dawn`. Build time varies by platform and optimization level.

## 4. Download Speech Models

```bash
./setup_models.sh
```

This downloads Whisper ASR (~142MB). TTS and VAD models are already included.

**Options**:
- `--whisper-model tiny` — Faster, less accurate (~75MB)
- `--whisper-model small` — Slower, more accurate (~466MB)
- `--vosk` — Include legacy Vosk ASR (~1.8GB)

## 5. Configure

DAWN uses two configuration files. Example files are provided:

```bash
# Copy example configs (run from project root)
cp dawn.toml.example dawn.toml
cp secrets.toml.example secrets.toml

# Edit secrets.toml - uncomment and add your API key(s)
nano secrets.toml
```

**secrets.toml** - Uncomment at least one API key and add your key:
```toml
[secrets]
openai_api_key = "sk-your-openai-key"
# claude_api_key = "sk-ant-your-claude-key"
# gemini_api_key = "your-gemini-key"
```

**Alternative**: Use environment variables instead of secrets.toml:
```bash
export OPENAI_API_KEY="sk-your-openai-key"
# or: export CLAUDE_API_KEY="sk-ant-..." or GEMINI_API_KEY="..."
```

**dawn.toml** - Optional customization. Defaults work for most users. You can also adjust settings later via the Web UI.

> **Tip**: Many settings can be changed live in the Web UI without editing files.

## 6. Create Admin Account

The Web UI requires authentication. On first run, DAWN displays a setup token in the console:

```bash
# Run from project root (where dawn.toml is located)
./build/dawn
# Look for: "Setup token: XXXX-XXXX-XXXX"
```

Use the `dawn-admin` utility to create your admin account:

```bash
./build/dawn-admin user create <username> --admin
# Enter the setup token when prompted
# Set your password
```

## 7. Run DAWN

```bash
# Always run from project root (where config files are)
./build/dawn
```

**Expected output**:
```
[INFO] DAWN starting...
[INFO] ASR: Whisper base model loaded
[INFO] TTS: Piper engine ready
[INFO] WebUI: Listening on http://0.0.0.0:3000
[INFO] Listening for wake word "friday"...
```

**Local voice**: Say any supported wake phrase followed by your command.

**Web UI**: Open `http://localhost:3000` and log in with your admin account.

## SSL Setup (for Remote Voice)

Browsers require HTTPS to access the microphone from non-localhost origins. For voice input from other devices:

```bash
# Generate self-signed certificate
./generate_ssl_cert.sh

# This creates ssl/dawn.crt and ssl/dawn.key
# DAWN auto-detects these and enables HTTPS
```

Then access the Web UI at `https://<dawn-ip>:3000`. Accept the browser's certificate warning on first visit.

> **Production use**: Replace with a proper certificate from Let's Encrypt or your CA.

## Wake Words and Voice Commands

The default wake word is **"friday"** (configurable via `ai_name` in dawn.toml). DAWN recognizes these prefixes combined with the wake word:

| Prefix | Example |
|--------|---------|
| "hello" | "Hello Friday, ..." |
| "hey" | "Hey Friday, ..." |
| "hi" | "Hi Friday, ..." |
| "okay" | "Okay Friday, ..." |
| "alright" | "Alright Friday, ..." |
| "yeah" | "Yeah Friday, ..." |
| "good morning" | "Good morning Friday, ..." |
| "good day" | "Good day Friday, ..." |
| "good evening" | "Good evening Friday, ..." |

**Example commands**:
- "Hey Friday, what's the weather in Atlanta?"
- "Okay Friday, search for the latest tech news"
- "Hello Friday, what's 15% of 847?"
- "Hey Friday, turn on the living room lights" (requires MQTT setup)

## Optional Components

### Local LLM (Free, No API Key)

Running a local LLM is beyond the scope of this guide, but DAWN supports:

- **[llama.cpp](https://github.com/ggerganov/llama.cpp)** - Best performance, recommended for Jetson
- **[Ollama](https://ollama.ai)** - Easier setup, better model management

Configure in `dawn.toml`:
```toml
[llm]
type = "local"

[llm.local]
endpoint = "http://127.0.0.1:8080"  # llama.cpp default
# endpoint = "http://127.0.0.1:11434"  # Ollama default
```

### Web Search (SearXNG)

Enable voice-activated web search with [SearXNG](https://docs.searxng.org/), a self-hosted metasearch engine. See [README.md](README.md#searxng-setup-for-web-search) for Docker setup instructions.

### JavaScript-Heavy Sites (FlareSolverr)

For fetching content from sites that block simple requests, DAWN supports [FlareSolverr](https://github.com/FlareSolverr/FlareSolverr). See [README.md](README.md#7-optional-llm-tools-setup) for setup.

## Troubleshooting

| Issue | Solution |
|-------|----------|
| **No audio capture** | Check devices: `arecord -L`, set `[audio] capture_device` in `dawn.toml` |
| **No audio playback** | Check devices: `aplay -L`, set `[audio] playback_device` in `dawn.toml` |
| **Model not found** | Run `./setup_models.sh`, ensure `build/models` symlink exists |
| **API key error** | Verify `secrets.toml` format matches example, key is under `[secrets]` |
| **Wake word not detected** | Adjust mic volume: `alsamixer` or `pavucontrol` |
| **WebUI login fails** | Create admin account with `dawn-admin user create <name> --admin` |

## Next Steps

- **Full configuration**: See [README.md](README.md#configuration)
- **Local LLM setup**: [llama.cpp](https://github.com/ggerganov/llama.cpp) or [Ollama](https://ollama.ai)
- **Satellite devices**: See [docs/DAP2_SATELLITE.md](docs/DAP2_SATELLITE.md) for Tier 1 (RPi) and [docs/DAP2_DESIGN.md](docs/DAP2_DESIGN.md) for Tier 2 (ESP32)
- **System architecture**: See [ARCHITECTURE.md](ARCHITECTURE.md)

---

**Questions or issues?** Open an issue at [github.com/The-OASIS-Project/dawn/issues](https://github.com/The-OASIS-Project/dawn/issues)
