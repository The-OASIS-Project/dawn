# D.A.W.N. - Digital Assistant for Workflow Neural-inference

**D.A.W.N.** (Digital Assistant for Workflow Neural-inference) is the central intelligence layer of the OASIS ecosystem, responsible for interpreting user intent, fusing data from every subsystem, and routing commands. At its core, DAWN performs neural-inference to understand context and drive decision-making, acting as OASIS's orchestration hub for MIRAGE, AURA, SPARK, STAT, and any future modules. Instead of handling a single task, DAWN evaluates the entire system's workflow — what the user wants, what the hardware is reporting, and what action it should take next — transforming raw data into coordinated behavior across the entire platform.

DAWN integrates modern speech recognition, large language models, and text-to-speech into a unified conversational AI system designed for embedded Linux platforms (Jetson, Raspberry Pi).

## Features

### Core Capabilities
- **Dual ASR Engine Support**
  - Whisper (base model) with GPU acceleration on Jetson (2.3x-5.5x speedup)
  - Vosk (legacy support, optional, GPU-accelerated)
  - Voice Activity Detection (VAD) using Silero model (included)
  - Intelligent chunking for long utterances

- **Multi-Provider LLM Integration**
  - Cloud: OpenAI GPT-4o, Anthropic Claude 4.5 Sonnet
  - Local: llama.cpp with optimized Qwen3-4B model (81.9% quality @ 138ms TTFT)
  - Streaming responses with real-time TTS integration
  - Sentence-boundary buffering for natural speech output

- **High-Quality Text-to-Speech**
  - Piper TTS with ONNX Runtime (en_GB-alba-medium model included)
  - Text preprocessing for natural phrasing
  - Streaming integration with LLM responses

- **Network Audio Processing**
  - Custom Dawn Audio Protocol (DAP) for ESP32 clients
  - Reliable binary protocol with checksums and retries
  - Remote voice command processing

- **MQTT Integration**
  - Device command/control system
  - Integration with other OASIS components or external systems
  - Extensible device callback architecture

- **Web UI**
  - Browser-based interface on port 3000
  - Push-to-talk voice input (requires HTTPS for remote access)
  - TTS audio playback through browser
  - FFT waveform visualization with lightning trail effect
  - Real-time WebSocket communication
  - Session persistence across page refresh (30-minute timeout)
  - Debug mode for viewing commands and tool results
  - **Settings panel** for live configuration editing
  - **Application restart** from browser when settings require it
  - Mobile-friendly responsive design

- **LLM Tools**
  - **Web Search** - Voice-activated search via SearXNG (self-hosted, privacy-focused)
  - **URL Fetcher** - Fetch and read web pages; large pages auto-summarized via local LLM
  - **Weather** - Real-time weather and forecasts via Open-Meteo API (free, no API key)
  - **Calculator** - Mathematical expression evaluation with tinyexpr engine
  - LLM automatically invokes tools and incorporates results into responses

### Performance Highlights
- **ASR Performance** (Jetson GPU acceleration):
  - Whisper tiny: RTF 0.079 (12.7x faster than realtime)
  - Whisper base: RTF 0.109 (9.2x faster than realtime)
  - Whisper small: RTF 0.225 (4.4x faster than realtime)

- **LLM Performance**:
  - Cloud (GPT-4o): 100% quality, ~3.1s latency
  - Cloud (Claude 4.5 Sonnet): 92.4% quality, ~3.5s latency
  - Local (Qwen3-4B Q4): 81.9% quality, 116-138ms TTFT, FREE
  - Streaming reduces perceived latency to ~1.3s (ASR + TTFT + TTS start)

## Directory Structure

```
dawn/
├── src/                          # Source files (.c, .cpp)
│   ├── asr/                      # Speech recognition subsystem
│   │   ├── asr_interface.c       # ASR abstraction layer
│   │   ├── asr_whisper.c         # Whisper implementation
│   │   ├── asr_vosk.c            # Vosk implementation (optional)
│   │   ├── vad_silero.c          # Voice Activity Detection
│   │   └── chunking_manager.c    # Long utterance handling
│   ├── llm/                      # LLM integration subsystem
│   │   ├── llm_interface.c       # LLM abstraction layer
│   │   ├── llm_openai.c          # OpenAI API implementation
│   │   ├── llm_claude.c          # Claude API implementation
│   │   ├── llm_streaming.c       # Streaming response handler
│   │   ├── sse_parser.c          # Server-Sent Events parser
│   │   ├── sentence_buffer.c     # Sentence boundary detection
│   │   └── llm_command_parser.c  # JSON command extraction
│   ├── tts/                      # Text-to-speech subsystem
│   │   ├── text_to_speech.cpp    # TTS engine wrapper
│   │   └── piper.cpp             # Piper integration
│   ├── network/                  # Network server subsystem
│   │   ├── dawn_server.c         # Network audio server
│   │   ├── dawn_network_audio.c  # Network audio processing
│   │   └── dawn_wav_utils.c      # WAV file utilities
│   ├── audio/                    # Audio capture/playback subsystem
│   │   ├── audio_capture_thread.c # Dedicated capture thread
│   │   ├── ring_buffer.c         # Thread-safe audio buffer
│   │   ├── flac_playback.c       # Music playback
│   │   └── mic_passthrough.c     # Microphone passthrough
│   ├── tools/                    # LLM tool implementations
│   │   ├── web_search.c          # SearXNG web search integration
│   │   ├── weather_service.c     # Open-Meteo weather API
│   │   ├── calculator.c          # Math expression evaluation
│   │   └── tinyexpr.c            # Expression parser library
│   ├── webui/                    # Web UI server subsystem
│   │   └── webui_server.c        # HTTP/WebSocket server (libwebsockets)
│   └── (core files)              # Core application files
│       ├── dawn.c                # Main application & state machine
│       ├── logging.c             # Centralized logging
│       ├── mosquitto_comms.c     # MQTT integration
│       ├── text_to_command_nuevo.c # Command parsing
│       └── word_to_number.c      # Natural language number parsing
│
├── include/                      # Header files (mirrors src/)
│   ├── asr/
│   ├── llm/
│   ├── tts/
│   ├── network/
│   ├── audio/
│   ├── tools/                    # LLM tool headers
│   ├── webui/                    # Web UI headers
│   ├── utf8/                     # UTF-8 library for TTS
│   └── (core headers)
│
├── www/                          # Web UI static files
│   ├── index.html                # Main HTML page
│   ├── css/dawn.css              # Stylesheet
│   └── js/dawn.js                # JavaScript client
│
├── whisper.cpp/                  # Whisper ASR engine (submodule)
├── models/                       # ML models (TTS, VAD)
├── test_recordings/              # ASR test data & benchmarks
├── tests/                        # Test programs
├── llm_testing/                  # LLM testing infrastructure
├── services/                     # Systemd services (llama-server)
├── remote_dawn/                  # ESP32 client code
├── vosk-model-en-us-0.22/        # Vosk model (if using Vosk)
├── setup_models.sh               # Model download and symlink setup
├── format_code.sh                # Code formatting script
├── install-git-hooks.sh          # Git hooks installer
├── generate_ssl_cert.sh          # SSL certificate generator
├── commands_config_nuevo.json    # Device/action mappings
├── CMakeLists.txt                # Build configuration
├── CODING_STYLE_GUIDE.md         # Code formatting standards
├── LLM_INTEGRATION_GUIDE.md      # LLM setup guide
├── ARCHITECTURE.md               # System architecture (see this for details)
└── README.md                     # This file
```

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
| **Jetson Orin Nano** | ~$250 | 40 TOPS | Primary target, GPU Whisper ~0.1s RTF |
| **Jetson Orin NX** | ~$400 | 100 TOPS | Excellent headroom for all features |

#### Tier 2: Good (Usable with minor tradeoffs)

| Platform | Price | AI Performance | Notes |
|----------|-------|----------------|-------|
| **Raspberry Pi 5 (8GB)** | ~$80 | CPU-only, ~1.0 RTF | Whisper base: ~6s for 10s audio |
| **Raspberry Pi 5 + AI Kit** | ~$180 | 13 TOPS | Hailo-8L accelerator helps vision, not ASR |
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
| **Cost-conscious hobbyist** | Raspberry Pi 5 (4GB) ~$60 | Works with Whisper tiny/base, acceptable latency |
| **Better performance on budget** | Orange Pi 5 (8GB) ~$100 | RK3588 CPU faster than RPi 5 |
| **x86 preference** | Intel N100 Mini PC ~$150 | AVX2 support, can run local LLMs |
| **Production/commercial** | Jetson Orin Nano ~$250 | Best price/performance for real-time voice AI |
| **Maximum capability** | Jetson Orin NX ~$400+ | Handles everything including large local LLMs |

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
sudo mkdir -p /usr/local/include/onnxruntime
sudo cp include/onnxruntime/core/session/*.h /usr/local/include/onnxruntime
sudo ldconfig
```

#### piper-phonemize (required for TTS)
```bash
git clone https://github.com/rhasspy/piper-phonemize.git
cd piper-phonemize
mkdir build && cd build
cmake ..
make -j8
sudo make install
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

DAWN uses TOML-based configuration files for runtime settings. Configuration is optional - sensible defaults are built-in.

#### Configuration File Locations (searched in order)
1. Path specified via `--config` CLI option
2. `./dawn.toml` (current directory)
3. `~/.config/dawn/dawn.toml`
4. `/etc/dawn/dawn.toml`

#### dawn.toml (main configuration)

Create `dawn.toml` to customize settings. Only include settings you want to change:

```toml
[general]
ai_name = "friday"           # Wake word (lowercase)

[audio]
capture_device = "default"   # ALSA/Pulse device name
playback_device = "default"  # ALSA/Pulse device name
backend = "auto"             # "auto", "alsa", or "pulse"

[llm]
type = "local"               # "local" or "cloud"
max_tokens = 4096
streaming = true

[llm.local]
endpoint = "http://127.0.0.1:8080"
model = "qwen3"

[llm.cloud]
provider = "openai"          # "openai" or "claude"
openai_model = "gpt-4o"      # Model for OpenAI API
claude_model = "claude-sonnet-4-20250514"  # Model for Claude API

[mqtt]
enabled = true
broker = "127.0.0.1"
port = 1883

[paths]
music_dir = "/Music"
```

See `docs/CONFIG_FILE_DESIGN.md` for the complete configuration reference.

#### secrets.toml (API keys)

Create `secrets.toml` in the project root or `~/.config/dawn/secrets.toml` for API keys:

```toml
openai_api_key = "sk-your-openai-key-here"
claude_api_key = "sk-ant-your-claude-key-here"
```

**Note**: `secrets.toml` is already in `.gitignore` - never commit API keys!

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
- **Web Search** - Requires SearXNG (below). Ask "Search for the latest news about Tony Stark"
- **URL Fetcher** - Built-in. Ask "Read the article at example.com/page" or "What does this URL say?"

**How summarization works:** When search results or fetched pages exceed ~3KB, DAWN can summarize the content before passing it to the main LLM. This keeps context windows manageable and responses fast. Summarization can use `local` (dedicated llama-server) or `default` (same LLM as conversation). Configure via `[search.summarizer]` in `dawn.toml`.

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

#### Build tests
```bash
cmake -DBUILD_TESTS=ON ..
make -j8
```

### Code Formatting

**All code must be formatted with clang-format before committing!**

```bash
# Format all code
./format_code.sh

# Check formatting (CI mode)
./format_code.sh --check

# Install pre-commit hook (auto-format on commit)
./install-git-hooks.sh
```

See `CODING_STYLE_GUIDE.md` for detailed coding standards.

## Configuration

### Main Configuration (include/dawn.h)

Key settings to customize:

```c
#define AI_NAME "friday"                    // Wake word
#define OPENAI_MODEL "gpt-4o"              // Cloud LLM model
#define DEFAULT_PCM_PLAYBACK_DEVICE "..."  // ALSA playback device
#define DEFAULT_PCM_CAPTURE_DEVICE "..."   // ALSA capture device
#define MQTT_IP "192.168.1.100"            // MQTT broker IP
#define MQTT_PORT 1883                      // MQTT broker port
```

The `AI_DESCRIPTION` constant defines the system prompt/personality for the LLM.

### LLM Configuration

See `LLM_INTEGRATION_GUIDE.md` for detailed setup instructions for:
- OpenAI API (cloud) - GPT-4o
- Anthropic Claude API (cloud) - Claude 4.5 Sonnet
- llama.cpp local server (free, on-device)

**Recommended local configuration**:
- Model: Qwen3-4B-Instruct-2507-Q4_K_M.gguf
- Batch size: 768 (critical for quality!)
- Context: 1024
- Service: `services/llama-server/` (systemd service included)

### Web UI Configuration

The Web UI is enabled by default. Configure via `dawn.toml`:

```toml
[webui]
enabled = true              # Enable/disable Web UI server
port = 3000                 # HTTP/WebSocket port
host = "0.0.0.0"            # Bind address (0.0.0.0 = all interfaces)
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

## Running

### Local Mode (microphone input)
```bash
cd build
./dawn
```

The system listens for the wake word ("friday" by default), then captures your command, processes it through ASR → LLM → TTS, and speaks the response.

### Network Mode (ESP32 clients)

DAWN includes a network server that accepts connections from ESP32 clients using the Dawn Audio Protocol (DAP).

1. Build and flash the ESP32 client (`remote_dawn/remote_dawn.ino`)
2. Configure WiFi credentials and DAWN server IP
3. Run DAWN server (same `./dawn` command)
4. ESP32 client connects and sends voice commands over WiFi

See `remote_dawn/protocol_specification.md` for protocol details.

### Web UI Mode

The Web UI provides a browser-based interface for interacting with DAWN:

1. Start DAWN (the Web UI server starts automatically):
   ```bash
   cd build
   ./dawn
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

- **All settings** from `dawn.toml` organized by section (General, Audio, LLM, ASR, etc.)
- **API keys** with secure handling - password fields with show/hide toggle; only shows whether a key is set, never displays the actual value
- **Restart indicators** - settings that require a restart show a badge; when changes require restart, a "Restart DAWN" button appears
- **Live updates** - changes that don't require restart apply immediately
- **Backup protection** - creates `.bak` files before modifying config files

**How it works**:
- HTTP serves static files from `www/` directory
- WebSocket connection on the same port handles real-time communication
- Sessions are tracked via token stored in browser localStorage
- Each browser tab maintains its own conversation context

See `docs/WEBUI_DESIGN.md` for technical architecture details.

## Testing

### ASR Benchmarks
```bash
cd tests
./asr_benchmark ../test_recordings/
```

Results are documented in `test_recordings/BENCHMARK_RESULTS.md`.

### LLM Quality Testing
```bash
cd llm_testing/scripts
./test_single_model.sh <model_name>
./benchmark_all_models.sh
python3 test_cloud_baseline.py
```

See `llm_testing/docs/MODEL_TEST_ANALYSIS.md` for optimization results.

### Unit Tests
```bash
cd build
./test_sse_parser
./test_sentence_buffer
./test_streaming
```

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

### Network Protocol Issues
- **Client can't connect**: Check firewall, DAWN server IP/port
- **Checksum errors**: Verify client and server use same `PACKET_MAX_SIZE` (8192)
- **Timeout errors**: Check network latency, adjust retry settings

## Documentation

- **ARCHITECTURE.md** - System architecture and data flow
- **CODING_STYLE_GUIDE.md** - Code formatting and standards
- **LLM_INTEGRATION_GUIDE.md** - LLM setup (cloud and local)
- **services/llama-server/README.md** - Local LLM service setup
- **llm_testing/docs/** - LLM optimization research
- **remote_dawn/protocol_specification.md** - Network protocol spec
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

**Note**: This README reflects the reorganized codebase structure (as of November 2025). For historical reference, see git history before commit hash in `ARCHITECTURE.md`.
