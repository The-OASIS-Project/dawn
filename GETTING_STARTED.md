# Getting Started with DAWN

Quick guide to get DAWN running. For a project overview, see [README.md](README.md).

## What You're Setting Up

DAWN provides two ways to interact:

1. **Local Voice Interface** - Uses your device's microphone and speakers. Requires audio device configuration. Say a wake phrase (e.g., "Hey Friday", "Okay Friday") followed by your command.

2. **Web UI** - Browser-based interface at `http://localhost:3000`. Supports both text and voice input. Voice requires HTTPS for remote access (see [SSL Setup](#7-ssl-setup-for-remote-voice)).

Both interfaces share the same AI backend (ASR, LLM, TTS).

## Prerequisites

- **OS**: Ubuntu 22.04+, Debian 12+, or Jetson Linux (Raspberry Pi: **Raspberry Pi OS Lite 64-bit** recommended)
- **RAM**: 4GB minimum (8GB recommended)
- **Audio**: Microphone and speakers (for local voice interface; not required for WebUI-only — see [Headless Setup](#headless-setup-no-microphone))
- **LLM**: Either a cloud API key (OpenAI, Claude, Gemini) or a local LLM server (llama.cpp, Ollama)

## Automated Install (Recommended)

The install script handles platform detection, dependencies, library builds, configuration, and verification automatically. It works on Jetson, Raspberry Pi, and x86_64 systems.

```bash
# Clone the repository first (if you haven't already)
git clone --recursive https://github.com/The-OASIS-Project/dawn.git
cd dawn

# Run the interactive installer
./scripts/install.sh
```

The script will detect your platform, ask a few questions (LLM provider, model size, optional features), then run through all installation phases. It handles sudo prompts automatically — works with or without passwordless sudo.

**Other modes:**
```bash
./scripts/install.sh --verify              # Verify an existing installation
./scripts/install.sh --resume-from build   # Resume after a failure
./scripts/install.sh --deploy server       # Deploy as a systemd service
./scripts/install.sh --config install.conf # Unattended (see scripts/install.conf.example)
./scripts/install.sh -h                    # Full usage
```

If the automated installer completes successfully, skip to [Run DAWN](#8-run-dawn). The manual steps below are provided for reference or if you prefer step-by-step control.

---

## Manual Installation

The following sections walk through each step individually.

## 1. Install System Dependencies

```bash
sudo apt update && sudo apt install -y \
  build-essential cmake git pkg-config wget unzip autoconf automake libtool \
  python3-pip \
  libasound2-dev libpulse-dev libsndfile1-dev libflac-dev \
  libmosquitto-dev mosquitto mosquitto-clients \
  libjson-c-dev libcurl4-openssl-dev libssl-dev \
  libwebsockets-dev libopus-dev libsodium-dev libsqlite3-dev \
  libsamplerate0-dev libmpg123-dev libvorbis-dev libncurses-dev \
  meson ninja-build libabsl-dev \
  libmupdf-dev libfreetype-dev libharfbuzz-dev \
  libzip-dev libmujs-dev libgumbo-dev libopenjp2-7-dev libjbig2dec0-dev \
  libical-dev libxml2-dev libstemmer-dev
```

> **`libstemmer-dev` note**: Provides Porter2 stemming for the memory subsystem's BM25 keyword index. Required as of May 2026 — without it the build fails to link `memory_stem.c`.

> **Package name note**: On Ubuntu 22.04 (including Jetson Linux R36), the abseil package is `libabsl-dev`. On Ubuntu 24.04+, it may be named `libabseil-dev`. If one isn't found, try the other.

> **CUDA users** (Jetson, x86_64 with NVIDIA GPU): For GPU-accelerated Whisper ASR, DAWN needs ONNX Runtime built with CUDA support. The install script detects CUDA automatically. Verify your CUDA toolkit is installed: `ls /usr/local/cuda/include/cuda.h`.

> **CMake version**: DAWN presets require CMake 3.21+, but building ONNX Runtime from source requires CMake 3.28+. Check with `cmake --version`. Debian 13+ and Ubuntu 24.04+ ship recent enough versions. If your system CMake is too old (Ubuntu 22.04 ships 3.22), install a newer version:
> ```bash
> # Option 1: Official binary (aarch64)
> wget https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-aarch64.tar.gz
> tar xzf cmake-3.31.6-linux-aarch64.tar.gz
> sudo cp -r cmake-3.31.6-linux-aarch64/bin/* /usr/local/bin/
> sudo cp -r cmake-3.31.6-linux-aarch64/share/* /usr/local/share/
>
> # Option 2: Official binary (x86_64)
> wget https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-x86_64.tar.gz
> tar xzf cmake-3.31.6-linux-x86_64.tar.gz
> sudo cp -r cmake-3.31.6-linux-x86_64/bin/* /usr/local/bin/
> sudo cp -r cmake-3.31.6-linux-x86_64/share/* /usr/local/share/
>
> # Option 3: Via pip
> pip3 install cmake --upgrade
> ```

## 2. Install Core Libraries

Four libraries are required. Build instructions for each are below.

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

Pre-built packages are available for **x86_64** and **aarch64** (Raspberry Pi, non-CUDA ARM boards). Systems with CUDA (Jetson, x86_64 with NVIDIA GPU) should build from source for GPU-accelerated Whisper ASR.

**Option A: Pre-built package (recommended for RPi and x86_64)**

```bash
# aarch64 (Raspberry Pi 5, etc.)
wget https://github.com/microsoft/onnxruntime/releases/download/v1.19.2/onnxruntime-linux-aarch64-1.19.2.tgz
tar xzf onnxruntime-linux-aarch64-1.19.2.tgz
sudo cp -a onnxruntime-linux-aarch64-1.19.2/lib/libonnxruntime*.so* /usr/local/lib/
sudo cp onnxruntime-linux-aarch64-1.19.2/include/*.h /usr/local/include/
sudo ldconfig

# x86_64 — see docs/GETTING_STARTED_SERVER.md for the x86_64 package URL
```

**Option B: Build from source (any system with CUDA)**

> **Important**: Use a release tag (e.g., `v1.19.2`), not the `main` branch. The latest `main` may pull abseil versions that require GCC 12+, which is not available on Ubuntu 22.04.

> **GCC 14 / Debian 13 note**: Building ONNX Runtime v1.19.2 from source on GCC 14+ (Debian 13 Trixie, Ubuntu 25.04+) fails with `-Werror=template-id-cdtor`. Workaround: install GCC 12 alongside your system compiler and use it for the ONNX build only:
> ```bash
> sudo apt install gcc-12 g++-12
> CC=gcc-12 CXX=g++-12 ./build.sh --use_cuda ...  # same flags as below
> ```
> The install script (`scripts/install.sh`) handles this automatically.

```bash
git clone --recursive --branch v1.19.2 --depth 1 https://github.com/microsoft/onnxruntime.git
cd onnxruntime

./build.sh --use_cuda --cudnn_home /usr/local/cuda --cuda_home /usr/local/cuda \
  --config MinSizeRel --update --build --parallel --build_shared_lib
```

> **Eigen download failure**: If the build fails downloading Eigen from GitLab (hash mismatch or connection refused), download it manually and point the build at it:
> ```bash
> wget -O /tmp/eigen.zip "https://gitlab.com/libeigen/eigen/-/archive/e7248b26a1ed53fa030c5c459f7ea095dfd276ac/eigen-e7248b26a1ed53fa030c5c459f7ea095dfd276ac.zip"
> unzip -q /tmp/eigen.zip -d /tmp/eigen-src
> # Re-run the build command above, adding:
> #   --cmake_extra_defines FETCHCONTENT_SOURCE_DIR_EIGEN=/tmp/eigen-src/eigen-e7248b26a1ed53fa030c5c459f7ea095dfd276ac
> ```

After building:
```bash
sudo cp -a build/Linux/MinSizeRel/libonnxruntime.so* /usr/local/lib/
sudo cp include/onnxruntime/core/session/*.h /usr/local/include/
sudo ldconfig
cd ..
```

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
# Requires Meson 0.63+ (Ubuntu 22.04 ships 0.61; upgrade with: pip3 install --user meson --upgrade)
cd webrtc-audio-processing && meson setup build --default-library=static && ninja -C build && cd ..

# Configure and build DAWN
cmake --preset default
cmake --build --preset default
```

> **Skip AEC**: If WebRTC build fails, you can disable it: `cmake --preset default -DENABLE_AEC=OFF`

> **Build presets**: `default`/`full` build everything; `local` (`ENABLE_WEBUI=OFF`) is a microphone-only build with no network features; `debug` adds symbols; `server` skips local audio (see [server mode](docs/GETTING_STARTED_SERVER.md)). List them with `cmake --list-presets`. Common toggles: `-DENABLE_WEBUI=OFF`, `-DENABLE_AEC=OFF`, `-DDAWN_ENABLE_EMAIL_TOOL=ON`.

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
# openrouter_api_key = "sk-or-your-key"   # Optional: OpenRouter gateway (one key, any model) — set [llm.cloud] use_openrouter = true
# plex_token = "your-plex-token"          # Optional: adds Plex library to unified music DB
# home_assistant_token = "your-ha-token"  # Optional: for Home Assistant smart home control
# tavily_api_key = "tvly-your-tavily-key" # Optional: commercial search + URL extract (see Tavily section)
```

> **Plex users**: To get your Plex token, see the [Plex Music Source](#plex-music-source) section below. When configured, Plex tracks are automatically synced into the unified music database alongside local files, with priority-based deduplication.

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

## 7. SSL Setup (Recommended)

> **Security notice**: Without HTTPS, all traffic between your browser and DAWN — including authentication cookies, session tokens, API keys entered in the settings panel, and voice audio — is transmitted in plaintext. Anyone on your local network can intercept it. HTTPS is strongly recommended for all deployments, not just remote access.

HTTPS is also required by browsers to access the microphone from non-localhost origins (WebUI voice input won't work over plain HTTP from another device).

> **Do not expose DAWN directly to the public internet.** DAWN is designed for private LAN use. If you need remote access, use a VPN (WireGuard or Tailscale) so DAWN stays behind your firewall. See [docs/SECURITY_HARDENING_GUIDE.md — Internet Exposure](docs/SECURITY_HARDENING_GUIDE.md#internet-exposure) for a detailed assessment of what's safe and what's not.

DAWN uses a private Certificate Authority (CA) so all clients (browsers, RPi satellites, ESP32 devices) can validate the server certificate without security warnings.

```bash
# Generate CA + server certificate (prompts for CA passphrase)
./generate_ssl_cert.sh

# If you have an external IP or domain (e.g., port forwarding), add extra SANs:
# ./generate_ssl_cert.sh --san IP:203.0.113.50 --san DNS:dawn.example.com

# This creates:
#   ssl/ca.crt          — CA certificate (distribute to clients)
#   ssl/ca.key          — CA private key (keep secret!)
#   ssl/dawn.crt        — Server certificate (signed by CA)
#   ssl/dawn.key        — Server private key
#   ssl/dawn-chain.crt  — Full chain file (use in dawn.toml)
```

Configure `dawn.toml` to use the chain file:
```toml
[webui]
https = true
ssl_cert_path = "ssl/dawn-chain.crt"
ssl_key_path = "ssl/dawn.key"
```

**Install the CA certificate** in your OS trust store to eliminate browser warnings:

```bash
# Linux
sudo cp ssl/ca.crt /usr/local/share/ca-certificates/dawn-ca.crt && sudo update-ca-certificates

# macOS
sudo security add-trusted-cert -d -r trustRoot -k /Library/Keychains/System.keychain ssl/ca.crt

# Windows
certutil -addstore -f "ROOT" ssl\ca.crt
```

After installing, restart your browser and access `https://<dawn-ip>:3000` — no security warning.

> **Note**: Browsers cache certificate trust state. You may need to fully restart the browser (not just refresh the tab) for the new CA to take effect.

**Satellite setup**: See [DAP2_SATELLITE.md](docs/DAP2_SATELLITE.md#tls-setup) for RPi and ESP32 TLS configuration.

**Certificate renewal**: When the daemon's IP changes or the cert expires, run `./generate_ssl_cert.sh --renew`. Clients don't need updating since the CA stays the same.

**Satellite registration key** (optional but recommended): Generate a pre-shared key to prevent unauthorized satellite registration:

```bash
./generate_ssl_cert.sh --gen-key
```

This appends a `satellite_registration_key` to `secrets.toml`. Copy the key to each satellite's config. See [DAP2_SATELLITE.md](docs/DAP2_SATELLITE.md#registration-key) for details.

**Service token for MIRAGE** (optional): If running a MIRAGE HUD that needs to fetch images from DAWN, generate a service token:

```bash
openssl rand -hex 32
```

Add to DAWN's `secrets.toml`:
```toml
[secrets]
service_token = "<paste the 64-character hex string>"
```

Add the same token to MIRAGE's `secrets.json`:
```json
{
  "dawn_url": "https://127.0.0.1:3000",
  "dawn_service_token": "<same 64-character hex string>"
}
```

The token grants read-only access to non-private images (search results, generated images) via Bearer auth on the image API. Minimum 32 characters required. TLS recommended.

## 8. Run DAWN

```bash
# Always run from project root (where config files are)
./build/dawn
```

**Expected output**:
```
[INFO] DAWN starting...
[INFO] ASR: Whisper base model loaded
[INFO] TTS: Piper engine ready
[INFO] WebUI: Listening on https://0.0.0.0:3000
[INFO] Listening for wake word "friday"...
```

**Local voice**: Say any supported wake phrase followed by your command.

**Web UI**: Open `https://localhost:3000` and log in with your admin account.

### Headless Setup (No Microphone)

If your device has no physical microphone (e.g., a Raspberry Pi used as a WebUI-only server, or a board where you plan to add audio hardware later), the daemon will exit on startup because ALSA cannot open the default capture device.

**Workaround**: Load the kernel dummy sound driver to provide a virtual capture device:

```bash
# Load the dummy ALSA driver
sudo modprobe snd-dummy

# Persist across reboots
echo "snd-dummy" | sudo tee /etc/modules-load.d/snd-dummy.conf
```

Then configure `dawn.toml` to use the dummy device:

```toml
[audio]
capture_device = "plughw:CARD=Dummy,DEV=0"
```

Voice input still works through the WebUI and DAP2 satellites — only the local microphone path uses the dummy device. When you later connect a USB microphone or audio HAT, update `capture_device` to the real device (use `arecord -L` to list available devices) and remove the snd-dummy module.

> **Tip**: For a dedicated x86_64 server with no local audio hardware at all, consider [server mode](docs/GETTING_STARTED_SERVER.md) which skips local audio initialization entirely.

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

### SearXNG Setup (for Web Search)

DAWN can perform web searches via voice commands using [SearXNG](https://docs.searxng.org/), a self-hosted metasearch engine. Search categories include web, news, social, science, IT, Q&A, dictionary, academic papers, and images. The image search tool fetches and caches images server-side so clients never contact external servers directly.

#### Prerequisites

Install Docker and Docker Compose:

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
  max_page: 3

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
    weight: 2
  - name: google news
    disabled: false
    weight: 2
  - name: duckduckgo news
    disabled: true
  - name: yahoo news
    disabled: false

  # Image search engines (for image_search tool)
  - name: google images
    disabled: false
  - name: bing images
    disabled: false
  - name: duckduckgo images
    disabled: false

  # IT category deprioritization.
  # The default SearXNG 'it' category is dominated by DockerHub and MDN matches
  # for any query containing "release", "benchmark", "model", "image", etc., which
  # drowns out the actually-useful Stack Overflow / GitHub / news results. Lower
  # their weight so they only surface when nothing better matches.
  - name: docker hub
    weight: 0.2
  - name: mdn
    weight: 0.4
EOF

# Start SearXNG
docker compose up -d

# Verify it's working (wait a few seconds for startup)
sleep 5
curl -s "http://localhost:8384/search?q=test&format=json" | jq '.results[0].title'
```

If you see a search result title, SearXNG is ready. DAWN will automatically use it when you ask to search for something.

### Plex Music Source

DAWN can index and stream music from a Plex Media Server alongside your local music library. When both sources are configured, DAWN builds a unified library with priority-based deduplication — if the same track exists locally and on Plex, the local copy is preferred.

**1. Get your Plex authentication token:**

Sign into Plex Web, open any media item, click the `...` menu → "Get Info" → "View XML". The URL will contain `X-Plex-Token=xxxxxxxxxxxxxxxxxxxx`. Copy this token value.

Alternatively, retrieve it via curl:

```bash
curl -s -X POST 'https://plex.tv/users/sign_in.json' \
  -H 'X-Plex-Client-Identifier: dawn-assistant' \
  -H 'X-Plex-Product: DAWN' \
  -d 'user[login]=YOUR_EMAIL&user[password]=YOUR_PASSWORD' | \
  python3 -c "import sys,json; print(json.load(sys.stdin)['user']['authToken'])"
```

**2. Add the token to secrets.toml:**

```toml
[secrets]
plex_token = "xxxxxxxxxxxxxxxxxxxx"
```

Or enter it in the WebUI Settings → Secrets → Plex Token field.

**3. Configure the Plex connection in dawn.toml:**

```toml
[music.plex]
host = "192.168.1.100"    # Your Plex server IP or hostname
port = 32400              # Default Plex port
ssl = false               # Set to true if your server uses HTTPS
ssl_verify = true         # Set to false for self-signed certificates
music_section_id = 0      # 0 = auto-discover, or specify your library section ID
```

Or configure everything in WebUI Settings → Music & Media → Plex Connection fields.

**4. Verify:** Restart DAWN. The Plex library will sync automatically on startup (typically 3–5 seconds for ~3,000 tracks on LAN). Open the music panel in WebUI — the Library tab stats should show combined track/artist/album counts from both sources with duplicates merged.

### Home Assistant (Smart Home)

Control lights, switches, climate, locks, and more through voice commands. Requires a Home Assistant instance on your network. See [docs/HOMEASSISTANT_SETUP.md](docs/HOMEASSISTANT_SETUP.md) for bare metal setup on Jetson, or use any existing HA installation. Add your Long-Lived Access Token to `secrets.toml`:

```toml
[secrets]
home_assistant_token = "your-long-lived-access-token"
```

Then configure the HA connection URL in the WebUI admin panel (Settings → Home Assistant).

### Code Projects (Coding Harness)

Let the assistant answer questions about your source code by indexing repositories
into a code graph. Requires the external **cbm** (`codebase-memory-mcp`) server
running alongside DAWN, the `DAWN_ENABLE_CODE_PROJECTS` build flag, and
`[code_projects] enabled = true`. Full setup — cbm service, import vs. link-local,
branch tracking, refresh/rebuild, permissions/sandbox, and sharing with a coding
assistant — is in **[docs/CODING_PROJECTS.md](docs/CODING_PROJECTS.md)**.

### JavaScript-Heavy Sites (FlareSolverr)

For fetching content from sites that block simple requests, DAWN supports [FlareSolverr](https://github.com/FlareSolverr/FlareSolverr) as a headless browser proxy.

```bash
# Run FlareSolverr via Docker
docker run -d --name flaresolverr -p 8191:8191 ghcr.io/flaresolverr/flaresolverr:latest
```

Enable in `dawn.toml`:
```toml
[url_fetcher.flaresolverr]
enabled = true
endpoint = "http://localhost:8191"
```

### Tavily (Commercial Search + URL Extract)

[Tavily](https://tavily.com) is an LLM-optimized search and content-extraction API. It is an **opt-in, paid alternative** to the default SearXNG (search) + FlareSolverr (URL fetch) stack. Free tier covers 1000 calls/month — enough for most personal-use cases. No new build dependencies (uses existing libcurl + json-c).

When and why you'd want it:

- **More reliable search results** — SearXNG depends on scraping public engines that intermittently rate-limit or block; Tavily provides a stable LLM-tuned ranking.
- **JavaScript-rendered content without running a headless browser** — Tavily's `/extract` returns clean article text from sites that would otherwise need FlareSolverr.
- **No local containers to run** — replaces both Docker services if you don't want the operational overhead.

**1. Get a Tavily API key:** sign up at [tavily.com](https://tavily.com) and copy your API key (starts with `tvly-`).

**2. Add the key to `secrets.toml`:**

```toml
[secrets]
tavily_api_key = "tvly-your-tavily-key"
```

Or enter it in WebUI Settings → Secrets → Tavily API Key.

**3. Enable Tavily as the search engine and/or URL-fetch fallback in `dawn.toml`:**

```toml
[search]
engine = "tavily"           # "searxng" (default) | "tavily" | "disabled"

[url_fetcher]
# Direct libcurl fetch is always tried first; fallback engages on 403/redirect-loop/
# parse-failure/very-small-body. "tavily" or "flaresolverr".
fallback = "tavily"         # "flaresolverr" (default) | "tavily" | "none"

[url_fetcher.tavily]
timeout_sec = 30
max_response_bytes = 1048576
extract_depth = "advanced"  # "basic" | "advanced" (2× credit cost; better on heavy-chrome sites)
```

The two toggles are independent — you can use Tavily for search while keeping FlareSolverr for URL fetch, or vice versa.

**Resilience:** if Tavily returns an error or you exhaust the per-user rate limit (10/min, 100/hr), DAWN falls back to SearXNG/FlareSolverr automatically when those are also configured. The Tavily handoff is gated by DAWN's local SSRF allowlist (`url_is_blocked()`) — internal IPs are never forwarded upstream.

**Test it:** Ask DAWN to *"search for the latest news on Mars exploration"* or *"fetch the article at https://example.com/long-page"*. Watch the daemon log for `tavily_search` / `tavily_extract` lines to confirm the path.

### MQTT Security (Authentication + TLS)

By default, MQTT traffic is sent in plaintext on port 1883 with no authentication. For hardened deployments, OASIS supports MQTT username/password authentication and TLS encryption across all components (DAWN, MIRAGE, STAT).

For the complete setup guide covering broker configuration, certificate generation, and client configuration for all OASIS components, see **[docs/MQTT_SETUP.md](docs/MQTT_SETUP.md)**.

Quick reference for DAWN-specific config:

**`dawn.toml`:**
```toml
[mqtt]
broker = "127.0.0.1"
port = 8883
tls = true
tls_ca_cert = "/etc/mosquitto/certs/ca.crt"
```

**`secrets.toml`:**
```toml
[secrets]
mqtt_username = "oasis"
mqtt_password = "your-password"
```

### CalDAV Calendar

DAWN can query, create, and manage calendar events via the CalDAV protocol. It works with Google Calendar (OAuth 2.0), Apple iCloud, Nextcloud, Radicale, and any other RFC 4791-compliant server. The calendar tool is enabled by default in the CMake build.

All calendar accounts are managed through the **WebUI Settings > Calendar Accounts** panel — no manual TOML editing required for account setup.

#### Google Calendar (OAuth 2.0 — Recommended)

Google Calendar uses OAuth 2.0 for authentication. See **[docs/GOOGLE_OAUTH_SETUP.md](docs/GOOGLE_OAUTH_SETUP.md)** for the full setup guide (Google Cloud Console project, API enablement, credentials, and WebUI connection flow).

#### Other Providers (App Password)

For iCloud, Nextcloud, Radicale, and other CalDAV servers:

1. Open the WebUI → **Settings > Calendar Accounts**
2. Click **Add Account**
3. Select **App Password** (default tab)
4. Fill in the account details:

| Provider | URL | Password |
|----------|-----|----------|
| **Apple iCloud** | `https://caldav.icloud.com/` | App-specific password from [appleid.apple.com](https://appleid.apple.com) (Sign-In and Security → App-Specific Passwords) |
| **Nextcloud** | `https://your-server.com/remote.php/dav/` | Your Nextcloud password (or app password) |
| **Radicale** | `http://localhost:5232/` | Your Radicale password |

5. Click **Save**, then **Test Connection** to verify

**Test it:** Ask DAWN: *"What's on my calendar today?"*

### Email (IMAP/SMTP)

DAWN can check, read, search, send, trash, and archive email via voice commands. It works with any IMAP/SMTP provider (Gmail, iCloud, Outlook, Fastmail, Proton Mail via Bridge, self-hosted). Supports both app password and Google OAuth 2.0 (XOAUTH2) authentication.

The email tool is disabled by default (compile-time and runtime gates) because it has `TOOL_CAP_DANGEROUS` — email send is irreversible.

#### Enable Email

```bash
# Build with email enabled
cmake --preset debug -DDAWN_ENABLE_EMAIL_TOOL=ON
make -C build-debug -j8
```

Enable at runtime in `dawn.toml`:
```toml
[email]
enabled = true
```

#### Add an Email Account

All accounts are managed through the **WebUI Settings > Email Accounts** panel.

**Gmail (Google OAuth — Recommended)**:
1. Follow [docs/GOOGLE_OAUTH_SETUP.md](docs/GOOGLE_OAUTH_SETUP.md) — enable the **Gmail API** and add the `https://mail.google.com/` scope
2. In WebUI → **Settings > Email Accounts** → **Add Account** → **Google OAuth** tab
3. Click **Connect with Google**, authorize, then **Save Account**

**Gmail (App Password)**:
1. Generate an app password at Google Account → Security → 2-Step Verification → App Passwords
2. In WebUI → **Settings > Email Accounts** → **Add Account** → **App Password** tab
3. Select **Gmail** preset (auto-fills server/port/SSL), enter username and app password

**Other Providers**:

| Provider | IMAP Server | SMTP Server | Auth |
|----------|------------|-------------|------|
| **iCloud** | imap.mail.me.com:993 | smtp.mail.me.com:587 | App-specific password from [appleid.apple.com](https://appleid.apple.com) |
| **Outlook/365** | outlook.office365.com:993 | smtp.office365.com:587 | App password |
| **Fastmail** | imap.fastmail.com:993 | smtp.fastmail.com:465 | App password |
| **Proton Mail** | 127.0.0.1:1143 (Bridge) | 127.0.0.1:1025 (Bridge) | Proton Bridge password |

**Test it:**
- *"Check my email"*
- *"Search for emails from Alice"*
- *"Send an email to bob@example.com about the meeting tomorrow"* (two-step: draft → confirm)
- *"Delete that email"* (two-step: pending → confirm)
- *"Archive that email"* (single-step, non-destructive)

### Messaging Channels (Telegram / Slack / Discord / SMS)

Chat with DAWN from the apps you already use. Link a Telegram, Slack, Discord, or SMS conversation to your account and the full assistant — tools, memory, scheduler — is available from that chat client. Each channel becomes a persistent conversation that appears in your WebUI history.

Happy path:

1. Add the provider's bot/app token to `secrets.toml` (Telegram `telegram_bot_token`, Discord `discord_bot_token`, Slack `slack_app_token` + `slack_bot_token`; SMS routes through ECHO and needs no token). Restart DAWN — the driver loads only when its token is present.
2. Generate a one-time link code:
   ```bash
   ./build/dawn-admin messaging generate-link-code --user <username>
   ```
   (or use the WebUI **Settings → Messaging Channels** panel).
3. From the chat app, send `/link CODE` to the bot (Slack: `link CODE`, no slash).

After linking, just talk — no wake word needed. Send `/new` to reset the conversation. See **[docs/MESSAGING_CHANNELS_SETUP.md](docs/MESSAGING_CHANNELS_SETUP.md)** for per-provider bot/app creation, the SMS active-conversation window, scheduler delivery, and channel management.

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

- **Feature overview**: See [README.md](README.md)
- **Local LLM setup**: [llama.cpp](https://github.com/ggerganov/llama.cpp) or [Ollama](https://ollama.ai)
- **Satellite devices**: See [docs/DAP2_SATELLITE.md](docs/DAP2_SATELLITE.md) for Tier 1 (RPi) and [dawn_satellite_arduino/README.md](dawn_satellite_arduino/README.md) for Tier 2 (ESP32)
- **Satellite OTA updates**: To push signed firmware updates to satellites, generate an offline signing keypair once with `sudo ./dawn_satellite/ota-release.sh keygen` (writes `/etc/dawn/signing/ota_signing.{key,pub}` — the private key never lives on a device), deploy the `.pub` to each device's `/etc/dawn/ota_pubkey`, and set `[ota] enabled = true` + `release_dir` in `dawn.toml`. See [docs/OTA_DESIGN.md](docs/OTA_DESIGN.md).
- **Smart home**: See [docs/HOMEASSISTANT_SETUP.md](docs/HOMEASSISTANT_SETUP.md) for Home Assistant integration
- **System architecture**: See [ARCHITECTURE.md](ARCHITECTURE.md)

---

**Questions or issues?** Open an issue at [github.com/The-OASIS-Project/dawn/issues](https://github.com/The-OASIS-Project/dawn/issues)
