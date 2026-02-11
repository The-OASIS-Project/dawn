# D.A.W.N. - Digital Assistant for Workflow Neural-inference

**D.A.W.N.** (Digital Assistant for Workflow Neural-inference) is the central intelligence layer of the OASIS ecosystem, responsible for interpreting user intent, fusing data from every subsystem, and routing commands. At its core, DAWN performs neural-inference to understand context and drive decision-making, acting as OASIS's orchestration hub for MIRAGE, AURA, SPARK, STAT, and any future modules. Instead of handling a single task, DAWN evaluates the entire system's workflow — what the user wants, what the hardware is reporting, and what action it should take next — transforming raw data into coordinated behavior across the entire platform.

**In plain English:** DAWN is a voice-controlled AI assistant that runs on embedded hardware. Talk to it, and it understands you (speech recognition), thinks about what you said (LLM), and talks back (text-to-speech). It can control your smart home, answer questions, search the web, and more — all by voice, without sending your data to the cloud if you run a local LLM.

DAWN is designed for embedded Linux platforms (Jetson, Raspberry Pi) and supports multiple interfaces: a local microphone, a browser-based Web UI with voice and text input, and DAP2 satellite devices (Raspberry Pi or ESP32) that can be placed throughout your home.

> **New to DAWN?** See **[GETTING_STARTED.md](GETTING_STARTED.md)** for a 10-minute quickstart guide.

## Features

### Core Capabilities
- **Dual ASR Engine Support**
  - Whisper (base model) with GPU acceleration on Jetson (2.3x-5.5x speedup)
  - Vosk (legacy support, optional, GPU-accelerated)
  - Voice Activity Detection (VAD) using Silero model (included)
  - Intelligent chunking for long utterances

- **Multi-Provider LLM Integration**
  - Cloud: OpenAI GPT-5 series, Anthropic Claude 4.5 (Sonnet/Opus/Haiku), Google Gemini 2.5/3
  - Local: llama.cpp or Ollama (setup is beyond this guide — see [llama.cpp](https://github.com/ggerganov/llama.cpp) or [Ollama](https://ollama.ai))
  - Runtime model switching via WebUI or voice commands
  - Streaming responses with real-time TTS integration
  - Real-time token streaming with generation speed metrics (tokens/sec)
  - Sentence-boundary buffering for natural speech output
  - **Extended Thinking/Reasoning** — Enable deep reasoning mode for complex queries:
    - Claude: Thinking budget control (collapsible thinking blocks)
    - OpenAI: Reasoning effort (low/medium/high) for o1/o3/o4 models
    - Gemini: Thinking mode for Gemini 2.5 Flash/Pro
    - Local: Qwen3 thinking mode with native template support

- **High-Quality Text-to-Speech**
  - Piper TTS with ONNX Runtime
  - Two voices included: en_GB-alba-medium (Friday) and en_GB-northern_english_male-medium (Jarvis)
  - Text preprocessing for natural phrasing
  - Streaming integration with LLM responses

- **Multi-Format Music Playback**
  - FLAC (always available via libFLAC)
  - MP3 (optional, via libmpg123)
  - Ogg Vorbis (optional, via libvorbis)
  - Unified decoder abstraction with auto-format detection
  - Mixed-format playlists supported
  - **Metadata search** - Search by artist, title, or album (not just filename)
  - **Background indexing** - SQLite metadata cache with configurable scan interval
  - **Resume playback** - Pause saves position, resume continues from there
  - **Opus streaming** - Stream music to WebUI and DAP2 satellites via WebSocket
  - **Paginated library** - Browse artists/albums/tracks with 50-item pages

- **DAP2 Satellite System**
  - Unified WebSocket protocol for all satellite tiers (same port as WebUI)
  - **Tier 1** (RPi): Text-first — local ASR/TTS, hands-free with wake word
  - **Tier 2** (ESP32): Audio path — streams raw PCM, server-side ASR/TTS, push-to-talk
  - Capability-based routing: daemon auto-selects text or audio path per satellite
  - SDL2 touchscreen UI with KMSDRM backend (no X11 required)
  - Music streaming via Opus audio over a dedicated WebSocket (Tier 1)
  - Goertzel FFT visualizer driven by live audio stream
  - Brightness and volume sliders with sysfs backlight + software dimming fallback
  - See [dawn_satellite/README.md](dawn_satellite/README.md) for details

- **MQTT Integration**
  - Device command/control system
  - Integration with other OASIS components or external systems
  - Extensible device callback architecture

- **Web UI**
  - Browser-based interface on port 3000
  - Push-to-talk voice input (requires HTTPS for remote access)
  - **Opus audio streaming** - Low-latency bidirectional audio via WebCodecs API (48kHz native, server resamples to 16kHz for ASR)
  - TTS audio playback through browser
  - FFT waveform visualization with lightning trail effect
  - Real-time WebSocket communication
  - Session persistence across page refresh (30-minute timeout)
  - Debug mode for viewing commands and tool results
  - **Settings panel** for live configuration editing
  - **Application restart** from browser when settings require it
  - **User authentication** with cookie-based sessions
  - **"Remember Me" login** - 30-day persistent sessions across browser restarts
  - **Multi-user support** with separate conversation contexts
  - **User Management** (admin-only) - create/delete users, reset passwords
  - **My Settings** - per-user persona, location, timezone, units, TTS speed
  - **Conversation history** - browse, search, continue, and delete past conversations
  - **Per-conversation LLM settings** - Reasoning mode and Tools mode lock after first message, inherited by continuations
  - **7 color themes** - cyan, purple, green, orange, red, blue, and terminal
  - **Accessibility** - keyboard navigation, screen reader support (ARIA), reduced motion preferences, WCAG-compliant touch targets
  - Mobile-friendly responsive design
  - **Vision/Image Support** - Send images for AI analysis:
    - Multiple input methods: file upload, paste, drag-and-drop, camera capture
    - Camera capture with front/rear camera switching (mobile-friendly)
    - Multi-image support (up to 5 images per message)
    - Client-side compression (max 1024px, JPEG 85%) for efficient uploads
    - SQLite BLOB storage for conversation history persistence
    - Auto-detects vision-capable models (GPT-4o, Claude 3, Gemini, LLaVA, etc.)
    - Security: SVG excluded to prevent XSS attacks

- **LLM Tools**
  - **Web Search** - Voice-activated search via SearXNG (self-hosted, privacy-focused)
    - Multiple search categories: web, news, social, science, IT, Q&A, dictionary, academic papers
    - Host-based deduplication (max 2 results per domain)
    - Relevance-based reranking with quality engine boosting
  - **URL Fetcher** - Fetch and read web pages; large pages auto-summarized via TF-IDF
  - **Weather** - Real-time weather and forecasts via Open-Meteo API (free, no API key)
  - **Calculator** - Mathematical expression evaluation with tinyexpr engine
  - **Memory Tool** - Search, remember, and forget facts across sessions
  - **Parallel Tool Execution** - Multiple tool calls execute concurrently (e.g., weather + search in ~1s vs ~3s sequential)
  - LLM automatically invokes tools and incorporates results into responses

- **Persistent Memory System**
  - DAWN remembers facts and preferences about users across sessions
  - **Memory Tool** - "Remember that I'm vegetarian", "What do you know about me?"
  - **Tokenized Search** - Multi-word queries match per-word with dedup and rank-by-match-count
  - **Recent Query** - Retrieve memories by time period (e.g., "24h", "7d", "1w")
  - **Automated Extraction** - Facts automatically extracted at session end
  - **Context Injection** - User facts loaded into system prompt at session start
  - **Privacy Toggle** - Mark conversations as private to prevent memory extraction (Ctrl+Shift+P)
  - Per-user memory isolation for multi-user households
  - Guardrails prevent instruction injection via memory content
  - See `docs/MEMORY_SYSTEM_DESIGN.md` for full architecture

### Performance Highlights
- **ASR Performance** (Jetson GPU acceleration):
  - Whisper tiny: RTF 0.079 (12.7x faster than realtime)
  - Whisper base: RTF 0.109 (9.2x faster than realtime)
  - Whisper small: RTF 0.225 (4.4x faster than realtime)

- **LLM Performance**:
  - Cloud providers: ~2-4s response latency depending on provider and model
  - Local (llama.cpp): ~100-200ms time-to-first-token on Jetson with quantized models
  - Streaming reduces perceived latency significantly (response starts as tokens arrive)

## Directory Structure

```
dawn/
├── src/                    # C/C++ source files
│   ├── asr/                # Speech recognition (Whisper, Vosk, VAD)
│   ├── llm/                # LLM integration (OpenAI, Claude, Gemini, local)
│   ├── memory/             # Persistent memory system
│   ├── tts/                # Text-to-speech (Piper)
│   ├── network/            # Legacy DAP1 server (deprecated, retained for reference)
│   ├── audio/              # Audio capture, playback, music
│   ├── tools/              # Modular LLM tools (search, weather, calculator, etc.)
│   └── webui/              # Web UI server
│
├── include/                # Header files (mirrors src/)
├── www/                    # Web UI static files (HTML, CSS, JS)
├── models/                 # ML models (TTS voices, VAD)
├── whisper.cpp/            # Whisper ASR engine (git submodule)
├── common/                 # Shared library (VAD, ASR, TTS, logging) for daemon + satellite
├── dawn_satellite/         # DAP2 Tier 1 satellite (Raspberry Pi, SDL2 UI)
├── remote_dawn/            # Legacy DAP1 ESP32 client (deprecated, retained for reference)
├── services/               # Systemd service files
├── tests/                  # Test programs
├── llm_testing/            # LLM benchmarking tools
├── docs/                   # Additional documentation
│
├── dawn.toml.example       # Configuration template
├── secrets.toml.example    # API keys template
├── setup_models.sh         # Model download script
├── format_code.sh          # Code formatting script
├── generate_ssl_cert.sh    # SSL certificate generator
└── CMakeLists.txt          # Build configuration
```

For detailed architecture, see [ARCHITECTURE.md](ARCHITECTURE.md).

## System Requirements

### Hardware Requirements Summary

| Component | Minimum | Recommended | Optimal |
|-----------|---------|-------------|---------|
| **RAM** | 2 GB | 4 GB | 8 GB |
| **Storage** | 4 GB | 8 GB | 16+ GB |
| **CPU** | 4-core ARM64/x86-64 | 4-core 2.0+ GHz | Jetson GPU |
| **GPU** | None (CPU-only) | RK3588 NPU / Coral TPU | NVIDIA CUDA |

### Platform Recommendations

#### Tier 1: Excellent (Production Ready)

| Platform | Price | AI Performance | Notes |
|----------|-------|----------------|-------|
| **Jetson Orin Nano Super** | ~$249 | 67 TOPS | Primary target, GPU Whisper ~0.1s RTF |
| **Jetson Orin NX** | ~$400 | 100 TOPS | Excellent headroom for all features |
| **Jetson AGX Orin** | ~$999+ | 275 TOPS | Best for running large local LLMs alongside DAWN |

#### Tier 2: Good (Usable with minor tradeoffs)

| Platform | Price | AI Performance | Notes |
|----------|-------|----------------|-------|
| **Raspberry Pi 5 (8GB)** | ~$90 | CPU-only, ~1.0 RTF | Whisper base: ~6s for 10s audio |
| **Raspberry Pi 5 + AI Kit** | ~$190 | 13 TOPS | Hailo-8L accelerator helps vision, not ASR |
| **Orange Pi 5 (RK3588)** | ~$100-150 | 6 TOPS NPU | Requires RKNN conversion for NPU; ONNX runs on CPU |
| **Intel N100 Mini PC** | ~$150 | CPU AVX2 | Whisper tiny: ~1.5s; base: ~5-8s |

#### Tier 3: Marginal (Works but slow)

| Platform | Price | AI Performance | Notes |
|----------|-------|----------------|-------|
| **Raspberry Pi 4 (4GB)** | ~$55 | CPU-only | Whisper tiny: ~11s for 11s audio (barely real-time) |
| **Raspberry Pi 4 (8GB)** | ~$75 | CPU-only | Same speed, more RAM headroom |
| **Generic ARM64 SBC** | Varies | CPU-only | Performance depends on CPU speed |

#### Tier 4: Not Recommended

| Platform | Issue |
|----------|-------|
| **Raspberry Pi 3/Zero 2** | Too slow, insufficient RAM |
| **32-bit ARM systems** | Limited memory, slow inference |
| **Low-power x86 (Atom)** | Slower than ARM64 alternatives |

### Recommendations by Use Case

| Use Case | Recommended Platform | Why |
|----------|---------------------|-----|
| **Cost-conscious hobbyist** | Raspberry Pi 5 (4GB) ~$65 | Works with Whisper tiny/base, acceptable latency |
| **Better performance on budget** | Orange Pi 5 (8GB) ~$100 | RK3588 CPU faster than RPi 5 |
| **x86 preference** | Intel N100 Mini PC ~$150 | AVX2 support, can run small local LLMs |
| **Production/commercial** | Jetson Orin Nano Super ~$249 | Best price/performance for real-time voice AI |
| **Local LLM + DAWN** | Jetson AGX Orin ~$999+ | Run 7B-13B models alongside voice pipeline |
| **Maximum capability** | Jetson Thor (future) | Designed for humanoid robotics and large models |

### Software Requirements
- Debian/Ubuntu-based distribution (tested on Ubuntu 20.04+ and Jetson Linux)
- CMake 3.10+
- GCC/G++ with C++17 support
- CUDA 12.6 (for Jetson GPU acceleration, optional on other platforms)
- Python 3.8+ (for testing tools)

## Installation

### 1. Install System Dependencies

```bash
# Core build tools
sudo apt update
sudo apt install -y build-essential cmake git pkg-config

# Audio libraries
sudo apt install -y libasound2-dev libpulse-dev libsndfile1-dev libflac-dev

# Optional: MP3 and Ogg Vorbis decoding for music playback
sudo apt install -y libmpg123-dev libvorbis-dev

# MQTT
sudo apt install -y libmosquitto-dev

# JSON parsing
sudo apt install -y libjson-c-dev

# CURL for HTTP/API calls
sudo apt install -y libcurl4-openssl-dev

# OpenSSL
sudo apt install -y libssl-dev

# libwebsockets (for Web UI)
sudo apt install -y libwebsockets-dev

# Authentication (required for WebUI or DAP network features)
sudo apt install -y libsodium-dev libsqlite3-dev
```

### 2. Install Core Dependencies

#### CMake 3.27.1 (if needed)
```bash
wget https://github.com/Kitware/CMake/releases/download/v3.27.1/cmake-3.27.1.tar.gz
tar xvf cmake-3.27.1.tar.gz
cd cmake-3.27.1
./configure --system-curl
make -j8
sudo make install
```

#### spdlog
```bash
git clone https://github.com/gabime/spdlog.git
cd spdlog
mkdir build && cd build
cmake .. && make -j8
sudo make install
```

#### espeak-ng (required for TTS)
```bash
# Remove conflicting packages
sudo apt purge espeak-ng-data libespeak-ng1 speech-dispatcher-espeak-ng

# Build from source
git clone https://github.com/rhasspy/espeak-ng.git
cd espeak-ng
./autogen.sh
./configure --prefix=/usr
make -j8 src/espeak-ng src/speak-ng
make
sudo make LIBDIR=/usr/lib/aarch64-linux-gnu install
```

#### ONNX Runtime (with CUDA support for Jetson)
```bash
git clone --recursive https://github.com/microsoft/onnxruntime
cd onnxruntime

# For Jetson with CUDA 12.6
./build.sh --use_cuda --cudnn_home /usr/local/cuda-12.6 --cuda_home /usr/local/cuda-12.6 \
           --config MinSizeRel --update --build --parallel --build_shared_lib

# Install
sudo cp -a build/Linux/MinSizeRel/libonnxruntime.so* /usr/local/lib/
sudo cp include/onnxruntime/core/session/*.h /usr/local/include/
sudo ldconfig
```

#### piper-phonemize (required for TTS)
```bash
git clone https://github.com/rhasspy/piper-phonemize.git
cd piper-phonemize
mkdir build && cd build
cmake .. -DONNXRUNTIME_DIR=/usr/local -DESPEAK_NG_DIR=/usr
make -j8

# Manual install (piper's make install has broken rules for system deps)
sudo cp -a libpiper_phonemize.so* /usr/local/lib/
sudo mkdir -p /usr/local/include/piper-phonemize
sudo cp ../src/*.hpp /usr/local/include/piper-phonemize/
sudo cp ../src/uni_algo.h /usr/local/include/piper-phonemize/
sudo ldconfig
```

### 3. Install ASR Engine (Whisper)

Whisper is included as a git submodule and will be built automatically by CMake.

```bash
# Initialize submodules when cloning DAWN
git submodule update --init --recursive
```

### 4. (Optional) Install Vosk ASR

If you want legacy Vosk support:

#### Install Kaldi (long build!)
```bash
sudo apt-get install sox subversion
sudo git clone -b vosk --single-branch --depth=1 https://github.com/alphacep/kaldi /opt/kaldi
sudo chown -R $USER /opt/kaldi
cd /opt/kaldi/tools

# Edit Makefile: Remove `-msse -msse2` from `openfst_add_CXXFLAGS`
vim Makefile

make openfst cub  # Long build, -j doesn't work here
./extras/install_openblas_clapack.sh

cd ../src
./configure --mathlib=OPENBLAS_CLAPACK --shared
make -j8 online2 lm rnnlm
```

#### Install Vosk API
```bash
git clone https://github.com/alphacep/vosk-api --depth=1
cd vosk-api/src
KALDI_ROOT=/opt/kaldi make -j8

cd ../c
# Edit Makefile to add CUDA libs to LDFLAGS if needed
make

# Copy library and header
sudo cp libvosk.so /usr/local/lib/
sudo cp vosk_api.h /usr/local/include/
sudo ldconfig
```

#### Download Vosk Model
```bash
cd /path/to/dawn
wget https://alphacephei.com/vosk/models/vosk-model-en-us-0.22.zip
unzip vosk-model-en-us-0.22.zip
```

### 5. Download Models

#### Quick Setup (Recommended)

Use the setup script to download Whisper models and create required symlinks:

```bash
# Standard setup - Whisper base model
./setup_models.sh

# Use a smaller/faster Whisper model
./setup_models.sh --whisper-model tiny

# Include legacy Vosk ASR support (~1.8GB additional download)
./setup_models.sh --vosk

# See all options
./setup_models.sh --help
```

The script will:
- Download the Whisper ASR model to `whisper.cpp/models/` (if not present)
- Create symlinks in `models/` directory
- Create `build/models` symlink for runtime
- Optionally download Vosk model

Note: TTS (Piper) and VAD (Silero) models are already committed to git.

#### Model Directory Structure

After setup, the `models/` directory will look like:
```
models/
├── en_GB-alba-medium.onnx       # Piper TTS voice (committed to git)
├── en_GB-alba-medium.onnx.json  # TTS voice config (committed to git)
├── silero_vad_16k_op15.onnx     # Silero VAD model (committed to git)
├── whisper.cpp -> ../whisper.cpp/models  # Symlink to Whisper models
└── vosk-model -> ../vosk-model-en-us-0.22  # Symlink (if Vosk installed)
```

#### Manual Download (Alternative)

If you prefer to download models manually:

**Whisper ASR:** (required download)
```bash
# Download to whisper.cpp/models/
cd whisper.cpp/models
./download-ggml-model.sh base  # Or use wget:
# wget https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin

# Create symlink
cd ../../models
ln -s ../whisper.cpp/models whisper.cpp

# Create build symlink
ln -s ../models build/models
```

**TTS and VAD:** (already committed to git - no download needed)

The following models are already in the repository:
- `models/en_GB-alba-medium.onnx` - Piper TTS voice
- `models/silero_vad_16k_op15.onnx` - Silero VAD model

**Additional TTS Voices:**
```bash
# Browse available voices:
# https://huggingface.co/rhasspy/piper-voices/tree/main/en

# Example: Download US English voice
wget https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx
wget https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx.json
```

#### Model Size Reference

| Model | Size | Notes |
|-------|------|-------|
| Whisper tiny | ~75MB | Fastest, lower accuracy |
| Whisper base | ~142MB | Recommended for Jetson GPU |
| Whisper small | ~466MB | Better accuracy, slower |
| Whisper medium | ~1.5GB | Best accuracy, slowest |
| Piper alba-medium | ~63MB | Default TTS voice |
| Silero VAD | ~1.3MB | Voice activity detection |
| Vosk en-us-0.22 | ~1.8GB | Optional legacy ASR |

### 6. Create Configuration Files

DAWN uses TOML-based configuration files. Example files are provided — copy and customize:

```bash
cp dawn.toml.example dawn.toml      # Main configuration
cp secrets.toml.example secrets.toml # API keys
```

#### Configuration File Locations (searched in order)
1. Path specified via `--config` CLI option
2. `./dawn.toml` (current directory)
3. `~/.config/dawn/dawn.toml`
4. `/etc/dawn/dawn.toml`

#### dawn.toml (main configuration)

The `dawn.toml.example` file contains all available settings with documentation. Key sections:

- `[general]` — Wake word, logging
- `[audio]` — Capture/playback devices, backend selection
- `[llm]` — Provider (cloud/local), model selection, streaming
- `[llm.cloud]` — Cloud provider settings (OpenAI, Claude, Gemini)
- `[llm.local]` — Local LLM endpoint (llama.cpp or Ollama)
- `[asr]` — Whisper model selection
- `[tts]` — Voice model, speech speed
- `[webui]` — Port, SSL settings

Most settings can also be changed via the Web UI settings panel.

#### secrets.toml (API keys)

The `secrets.toml.example` shows the required format:

```toml
[secrets]
openai_api_key = "sk-your-openai-key-here"
claude_api_key = "sk-ant-your-claude-key-here"
gemini_api_key = "your-gemini-api-key-here"
```

**Security notes:**
- `secrets.toml` is in `.gitignore` — never commit API keys
- Set file permissions: `chmod 600 secrets.toml`
- Alternative: use environment variables (`OPENAI_API_KEY`, `CLAUDE_API_KEY`, `GEMINI_API_KEY`)

#### Environment Variables

All config options can be overridden via environment variables:
```bash
export DAWN_AUDIO_CAPTURE_DEVICE="hw:1,0"
export DAWN_LLM_TYPE="cloud"
export DAWN_OPENAI_API_KEY="sk-..."
```

#### commands_config_nuevo.json (MQTT devices)
See the example in the repository. Configure your MQTT broker and device mappings.

### 7. (Optional) LLM Tools Setup

DAWN includes several tools that the LLM can invoke automatically:

- **Calculator** - Built-in, no setup required. Ask "What's 15% of 847?" or "Calculate the square root of 144"
- **Weather** - Uses Open-Meteo API (free, no API key). Ask "What's the weather in Atlanta?" or "Will it rain tomorrow?"
- **Web Search** - Requires SearXNG (below). Supports multiple search categories:
  - `web` - General web search (default)
  - `news` - Recent news articles
  - `social` - Social media (Reddit, Twitter)
  - `science` - Scientific content
  - `it` - Tech/programming content
  - `qa` - Q&A sites (StackOverflow, SuperUser)
  - `facts` - Wikipedia infoboxes
  - `papers` - Academic papers (arXiv, Google Scholar, Semantic Scholar)

  Example queries: "Search for the latest news about CES 2026", "What's Reddit saying about the new iPhone?", "Find scientific papers on quantum computing"
- **URL Fetcher** - Built-in. Ask "Read the article at example.com/page" or "What does this URL say?"

**How summarization works:** When search results or fetched pages exceed ~3KB, DAWN summarizes the content before passing it to the main LLM. This keeps context windows manageable and responses fast.

Summarization backends (configured via `[search.summarizer]` in `dawn.toml`):
- **`tfidf`** (default) - Fast local TF-IDF extractive summarization. Selects the most important sentences using term frequency analysis with MMR (Maximal Marginal Relevance) for diversity. Filters out chart descriptions, ad elements, and sentence fragments. No LLM required, processes in milliseconds.
- **`local`** - Uses a dedicated llama-server for abstractive summarization
- **`default`** - Uses the same LLM as the conversation
- **`disabled`** - Pass content through without summarization

**FlareSolverr (optional):** For JavaScript-heavy sites that block simple fetches, DAWN supports [FlareSolverr](https://github.com/FlareSolverr/FlareSolverr) as a headless browser proxy. Enable via `[url_fetcher.flaresolverr]` in `dawn.toml`.

#### SearXNG Setup (for Web Search)

DAWN can perform web searches via voice commands using [SearXNG](https://docs.searxng.org/), a self-hosted metasearch engine.

#### Prerequisites

Install Docker and Docker Compose first. Follow the official instructions:
- [Docker Engine Install](https://docs.docker.com/engine/install/)
- [Docker Compose Install](https://docs.docker.com/compose/install/)

On Jetson/Ubuntu:
```bash
# Add yourself to the docker group (avoids needing sudo)
sudo usermod -aG docker $USER
# Log out and back in for group changes to take effect
```

#### Install SearXNG

```bash
# Create directory structure
mkdir -p ~/docker/searxng/searxng
cd ~/docker/searxng

# Generate a secret key
SECRET_KEY=$(openssl rand -hex 32)
echo "Generated secret key: $SECRET_KEY"

# Create docker-compose.yml
cat > docker-compose.yml << 'EOF'
services:
  searxng:
    image: searxng/searxng:latest
    container_name: searxng
    restart: unless-stopped
    ports:
      - "8384:8080"
    volumes:
      - ./searxng:/etc/searxng:rw
    environment:
      - SEARXNG_BASE_URL=http://localhost:8384/
    cap_drop:
      - ALL
    cap_add:
      - CHOWN
      - SETGID
      - SETUID
    logging:
      driver: "json-file"
      options:
        max-size: "1m"
        max-file: "1"
EOF

# Create settings.yml (replace SECRET_KEY_HERE with your generated key)
cat > searxng/settings.yml << EOF
use_default_settings: true

general:
  instance_name: "DAWN Search"
  debug: false

server:
  secret_key: "$SECRET_KEY"
  bind_address: "0.0.0.0"
  port: 8080
  method: "GET"
  image_proxy: false
  limiter: false
  public_instance: false

search:
  safe_search: 1
  default_lang: "en"
  autocomplete: ""
  formats:
    - json
  max_page: 1

ui:
  static_use_hash: true

engines:
  # Web search engines
  - name: google
    disabled: false
  - name: duckduckgo
    disabled: false
  - name: brave
    disabled: false
  - name: wikipedia
    disabled: false
  - name: bing
    disabled: false
  # News-specific engines
  - name: bing news
    disabled: false
  - name: google news
    disabled: false
  - name: duckduckgo news
    disabled: false
  - name: yahoo news
    disabled: false
EOF

# Start SearXNG
docker compose up -d

# Verify it's working (wait a few seconds for startup)
sleep 5
curl -s "http://localhost:8384/search?q=test&format=json" | jq '.results[0].title'
```

If you see a search result title, SearXNG is ready. DAWN will automatically use it when you ask to search for something.

## Building

### Standard Build (Whisper only, GPU on Jetson)
```bash
mkdir build
cd build
cmake ..
make -j8
```

### Build Options

#### Enable Vosk (legacy ASR)
```bash
cmake -DENABLE_VOSK=ON ..
make -j8
```

#### Use ALSA instead of PulseAudio
```bash
cmake -DUSE_ALSA=ON ..
make -j8
```

#### Force platform (override auto-detection)
```bash
cmake -DPLATFORM=JETSON ..  # or PLATFORM=RPI
make -j8
```

### Deployment Modes

DAWN supports 4 deployment modes with different feature sets:

| Mode | Local Mic | WebUI + Satellites | Use Case |
|------|-----------|-------------------|----------|
| **1** | ✓ | ✗ | Embedded / armor suit - no network |
| **2** | ✓ | ✓ | Full deployment - WebUI, Tier 1 + Tier 2 satellites |

#### Using CMake Presets (Recommended)

```bash
# List available presets
cmake --list-presets

# Configure with a preset
cmake --preset mode1-local    # Mode 1: Local only (smallest binary)
cmake --preset default        # Mode 2: Full (WebUI + satellites)

# Build
cmake --build --preset mode1-local
```

#### Manual CMake Options

```bash
# Mode 1: Local only (no network features)
cmake -DENABLE_WEBUI=OFF ..

# Mode 2: Full (default) - WebUI + all satellite support
cmake -DENABLE_WEBUI=ON ..
# or just: cmake ..
```

**Notes:**
- `ENABLE_WEBUI`: Controls the WebUI server on port 3000 (also serves DAP2 satellites via WebSocket)
- `ENABLE_AUTH`: Automatically enabled when WebUI is active
- All satellite communication (Tier 1 RPi + Tier 2 ESP32) uses the WebUI WebSocket port — no separate DAP server needed

#### Build tests
```bash
cmake -DBUILD_TESTS=ON ..
make -j8
```

### Code Formatting

**All code must be formatted before committing!**

```bash
# Format all C/C++ and web files
./format_code.sh

# Check formatting (CI mode)
./format_code.sh --check

# Install pre-commit hook (auto-format on commit)
./install-git-hooks.sh

# For web file formatting (optional, requires npm)
npm install  # One-time setup for Prettier
```

**Formatters used:**
- **C/C++**: clang-format (3-space indent, 100-char lines, K&R style)
- **JS/CSS/HTML**: Prettier (matching 3-space indent, 100-char lines)

See `CODING_STYLE_GUIDE.md` for detailed coding standards.

## Configuration

### Main Configuration (dawn.toml)

Key settings to customize:

```toml
[general]
ai_name = "friday"              # Wake word

[audio]
capture_device = "default"      # Microphone (use `arecord -L` to list)
playback_device = "default"     # Speaker (use `aplay -L` to list)

[llm.cloud]
provider = "openai"             # "openai", "claude", or "gemini"

[mqtt]
broker = "192.168.1.100"        # MQTT broker IP
port = 1883                     # MQTT broker port
```

See `dawn.toml.example` for all available options. The `[persona]` section allows customizing the AI's personality.

### LLM Configuration

See `docs/LLM_INTEGRATION_GUIDE.md` for detailed setup instructions for:
- OpenAI API (cloud) - GPT-5 series (gpt-5-mini default)
- Anthropic Claude API (cloud) - Claude Sonnet 4.5 (default)
- Google Gemini API (cloud) - Gemini 2.5 Flash (default), Gemini 3
- llama.cpp local server (free, on-device)

**Recommended local configuration**:
- Model: Qwen3-4B-Instruct-2507-Q4_K_M.gguf
- Batch size: 768 (critical for quality!)
- Context: 8192 (Varies based on available memory.)
- Service: `services/llama-server/` (systemd service included)

### Web UI Configuration

The Web UI is compiled in by default but disabled at runtime. Enable it in `dawn.toml`:

```toml
[webui]
enabled = true                  # Enable Web UI server
port = 3000                     # HTTP/WebSocket port
bind_address = "0.0.0.0"        # Bind address (0.0.0.0 = all interfaces)
```

### SSL/HTTPS Setup (for remote voice input)

Browsers require HTTPS (secure context) to access the microphone from non-localhost origins. If accessing the Web UI from another device on your network:

```bash
# Generate self-signed certificate
./generate_ssl_cert.sh

# This creates ssl/dawn.crt and ssl/dawn.key
# DAWN will automatically use HTTPS when these files exist
```

**Note**: You'll need to accept the self-signed certificate warning in your browser on first visit.

### Optional Features

**Terminal UI (TUI)**: An ncurses-based dashboard for monitoring DAWN status:
```toml
[tui]
enabled = true
```

**Shutdown Control**: Allow voice commands to shut down the system (security consideration):
```toml
[shutdown]
enabled = true
passphrase = "optional-security-phrase"  # Recommended if enabled
```

**SmartThings Integration**: For Samsung SmartThings home automation, add OAuth credentials to `secrets.toml`:
```toml
[secrets.smartthings]
client_id = "your-client-id"
client_secret = "your-client-secret"
```

## Running

### Local Mode (microphone input)
```bash
# Run from project root (where dawn.toml is located)
./build/dawn
```

The system listens for the wake word ("friday" by default), then captures your command, processes it through ASR → LLM → TTS, and speaks the response.

### Satellite Mode (DAP2)

DAWN supports satellite devices that extend voice assistant capabilities to multiple rooms. All satellites connect via WebSocket to the same port as the WebUI (default 3000).

- **Tier 1 (Raspberry Pi)**: Handles ASR/TTS locally, sends only text to daemon. See [dawn_satellite/README.md](dawn_satellite/README.md)
- **Tier 2 (ESP32)**: Streams raw PCM audio, daemon handles ASR/TTS server-side. See [docs/DAP2_DESIGN.md](docs/DAP2_DESIGN.md)

See [docs/DAP2_SATELLITE.md](docs/DAP2_SATELLITE.md) for deployment guide.

### Web UI Mode

The Web UI provides a browser-based interface for interacting with DAWN:

1. Start DAWN (the Web UI server starts automatically if enabled):
   ```bash
   # Run from project root
   ./build/dawn
   ```

2. Open a browser and navigate to:
   ```
   http://localhost:3000
   ```
   Or from another device on your network:
   ```
   http://<dawn-ip-address>:3000
   ```

**Features**:
- **Text input** - Type messages instead of speaking
- **Voice input** - Hold the mic button to speak (requires HTTPS for remote access)
- **Audio playback** - Hear DAWN's responses through your browser
- **Visual feedback** - FFT waveform with lightning trail effect animates when DAWN speaks
- **Real-time responses** - See LLM responses stream in real-time
- **Session persistence** - Your conversation history persists across page refreshes (30-minute timeout)
- **Debug mode** - Toggle debug view to see tool results, commands, and system messages
- **Mobile friendly** - Responsive design works on phones and tablets

**Settings Panel**:

Click the gear icon in the header to access the settings panel. This provides a browser-based interface for editing DAWN's configuration without manually editing TOML files:

- **User Management** (admin-only) - create users, delete users, reset passwords, unlock accounts
- **My Settings** - personal preferences (persona, location, timezone, units, TTS speed) that override global defaults
- **All settings** from `dawn.toml` organized by section (General, Audio, LLM, ASR, etc.)
- **API keys** with secure handling - password fields with show/hide toggle; only shows whether a key is set, never displays the actual value
- **Restart indicators** - settings that require a restart show a badge; when changes require restart, a "Restart DAWN" button appears
- **Live updates** - changes that don't require restart apply immediately
- **Backup protection** - creates `.bak` files before modifying config files
- **Role-based visibility** - admin-only sections hidden from regular users

**How it works**:
- HTTP serves static files from `www/` directory
- WebSocket connection on the same port handles real-time communication
- Sessions are tracked via token stored in browser localStorage
- Each browser tab maintains its own conversation context

See `docs/WEBUI_DESIGN.md` for technical architecture details.

### User Management (dawn-admin)

When authentication is enabled (WebUI mode), you'll need to create user accounts before logging in. The `dawn-admin` CLI tool handles user management:

```bash
# First-time setup: create initial admin account (requires setup token shown in DAWN console)
./dawn-admin user create admin --admin

# Common commands
./dawn-admin user list              # List all users
./dawn-admin user passwd <user>     # Change password
./dawn-admin session list           # View active sessions
./dawn-admin db status              # Database statistics
./dawn-admin --help                 # Full command reference

# Music library management
./dawn-admin music stats              # Database statistics
./dawn-admin music search "pink floyd" # Search by artist/title/album
./dawn-admin music list --limit 50    # List tracks
./dawn-admin music rescan             # Trigger immediate rescan
```

See `docs/USER_AUTH_DESIGN.md` for complete authentication system documentation.

## Performance Optimization

### ASR Performance Tips
- Use Whisper **base** model (best accuracy/speed on Jetson GPU)
- Enable GPU acceleration (automatic on Jetson)
- Adjust VAD sensitivity in `include/asr/vad_silero.h`

### LLM Performance Tips
- For local LLM: Use batch size 768 and context 1024 (CRITICAL!)
- Temperature, top-k, top-p have minimal effect on quality
- See `llm_testing/scripts/model_configs.conf` for optimal settings

### Latency Reduction
- Streaming LLM + TTS reduces perceived latency to ~1.3s
- GPU acceleration provides 2-5x speedup on ASR
- Use Whisper tiny for fastest response (slight accuracy tradeoff)

## Troubleshooting

### Build Issues
- **CUDA not found**: Check `/usr/local/cuda-12.6` symlink, adjust `CMakeLists.txt` if needed
- **ONNX Runtime errors**: Verify library is in `/usr/local/lib`, run `sudo ldconfig`
- **Missing headers**: Ensure all dependencies installed in correct paths

### Runtime Issues
- **No audio capture**: Check ALSA device names with `arecord -L`
- **No audio playback**: Check ALSA device names with `aplay -L`
- **Wake word not detected**: Adjust microphone sensitivity, check `pactl` or `alsamixer` levels
- **LLM timeout**: Check API keys in `secrets.toml`, network connectivity
- **Slow ASR**: Verify GPU acceleration enabled (check logs for CUDA messages)

### Satellite Connection Issues
- **Satellite can't connect**: Check firewall, DAWN server IP/port (default 3000)
- **WebSocket errors**: Check `libwebsockets` version, verify daemon is running
- **Timeout errors**: Check network latency, verify satellite_ping/pong (10s interval)

## Documentation

- **ARCHITECTURE.md** - System architecture and data flow
- **CODING_STYLE_GUIDE.md** - Code formatting and standards
- **docs/LLM_INTEGRATION_GUIDE.md** - LLM setup (cloud and local)
- **docs/MEMORY_SYSTEM_DESIGN.md** - Memory system architecture and design
- **services/llama-server/README.md** - Local LLM service setup
- **llm_testing/docs/** - LLM optimization research
- **docs/DAP2_SATELLITE.md** - DAP2 satellite architecture and deployment
- **dawn_satellite/README.md** - Satellite build, config, and usage guide
- **docs/DAP2_DESIGN.md** - DAP2 protocol specification (Tier 1 + Tier 2)
- **test_recordings/BENCHMARK_RESULTS.md** - ASR performance benchmarks

## Development

### Project Structure
- Core application: `src/dawn.c` (main loop, state machine)
- Subsystems: `src/{asr,llm,tts,network,audio}/`
- Headers: `include/` (mirrors src/)
- Tests: `tests/`
- Documentation: `*.md` files and `docs/`

### Adding New Features
1. Follow coding standards in `CODING_STYLE_GUIDE.md`
2. Format code with `./format_code.sh`
3. Add unit tests in `tests/`
4. Update documentation
5. Test on target platform (Jetson/RPi)

### Contributing
This is part of The OASIS Project. Contributions welcome!
- Follow existing code style
- Add tests for new features
- Update documentation
- Use descriptive commit messages

## Design Notes

### MCP (Model Context Protocol) Not Supported

DAWN does not implement MCP. While MCP has become an industry standard for connecting LLMs to external tools in composable applications, DAWN's architecture serves different goals:

- **Native C/C++ implementation**: DAWN is implemented entirely in C/C++ — a deliberate choice for reliability, deterministic timing, and single-binary deployment. MCP's ecosystem (SDKs, servers, tooling) is built around TypeScript and Python. No C implementation exists, and integrating one would require either writing an MCP client from scratch or embedding a managed runtime, negating the architectural benefits.

- **Voice-first responsiveness**: MCP's process-per-server model with JSON-RPC communication introduces latency and unpredictability. Voice assistants require sub-second response times with consistent behavior. DAWN's direct function calls and shared-memory architecture eliminate IPC overhead entirely.

- **Integrated tool system**: DAWN's native tool execution provides parallel thread-pool execution, automatic schema generation for multiple LLM providers (OpenAI, Claude, llama.cpp), session-scoped filtering, and built-in iterative tool loops. MCP defines a transport protocol — these capabilities remain the host's responsibility.

- **Self-contained design**: DAWN is a complete voice assistant, not a plugin framework. All tools are local (MQTT devices, system commands, media control, vision) with no architectural need for external server composition.

## Credits

- **Piper TTS**: https://github.com/rhasspy/piper (MIT License)
- **Vosk ASR**: https://alphacephei.com/vosk/ (Apache 2.0)
- **Whisper**: https://github.com/ggerganov/whisper.cpp (MIT License)
- **Silero VAD**: https://github.com/snakers4/silero-vad (MIT License)
- **ONNX Runtime**: https://github.com/microsoft/onnxruntime (MIT License)
- **espeak-ng**: https://github.com/espeak-ng/espeak-ng (GPL v3+)

## License

D.A.W.N. is licensed under the **GNU General Public License v3.0 or later**.

See individual dependencies for their respective licenses.

---

**Note**: This README reflects the reorganized codebase structure (as of February 2026, including modular CSS/JS, Prettier formatting, and DAP2 satellite system). For historical reference, see git history before commit hash in `ARCHITECTURE.md`.
