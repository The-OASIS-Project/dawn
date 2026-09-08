# Getting Started — Server Mode (x86_64)

Step-by-step guide to building and running DAWN on an x86_64 Linux system in server mode.
Server mode provides WebUI + satellite connections without local audio hardware.

Tested on Ubuntu 24.04 LTS (x86_64) with NVIDIA GPU (optional).

---

## Docker Quickstart

The fastest way to run DAWN on any Linux system. Docker handles all dependencies — no manual compilation needed.

### Prerequisites

- [Docker Engine](https://docs.docker.com/engine/install/) (20.10+)
- For CUDA variant: [NVIDIA Container Toolkit](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/install-guide.html)

### CPU Image (amd64 + arm64, 297MB)

```bash
# Build
git clone --recursive https://github.com/The-OASIS-Project/dawn.git && cd dawn
docker build -t dawn .

# Run
docker run -d --name dawn \
   -p 3000:3000 \
   -v /path/to/models:/var/lib/dawn/models \
   -v /path/to/dawn.toml:/var/lib/dawn/dawn.toml:ro \
   -v /path/to/secrets.toml:/var/lib/dawn/secrets.toml:ro \
   -v dawn-data:/var/lib/dawn/db \
   dawn
```

### CUDA Image (GPU-accelerated ASR, 3.55GB)

Builds ONNX Runtime from source with CUDA support. First build takes 30-60 minutes; subsequent builds use Docker layer cache.

```bash
# Build (requires NVIDIA GPU or cross-compile)
docker build -f Dockerfile.cuda -t dawn:cuda .

# Run (requires nvidia-container-toolkit)
docker run -d --name dawn --gpus all \
   -p 3000:3000 \
   -v /path/to/models:/var/lib/dawn/models \
   -v /path/to/dawn.toml:/var/lib/dawn/dawn.toml:ro \
   -v /path/to/secrets.toml:/var/lib/dawn/secrets.toml:ro \
   -v dawn-data:/var/lib/dawn/db \
   dawn:cuda
```

### Required Volumes

| Mount | Purpose |
|-------|---------|
| `/var/lib/dawn/models` | Whisper ASR model, Piper voice model, Silero VAD model |
| `/var/lib/dawn/dawn.toml` | Runtime configuration (bind as read-only) |
| `/var/lib/dawn/secrets.toml` | API keys (bind as read-only) |
| `/var/lib/dawn/db` | SQLite databases (conversations, memory, scheduler, etc.) |

Run `./setup_models.sh` on the host first to download models, then mount the `models/` directory.

### Security Notes

- The container runs as a non-root `dawn` user
- Supports `--read-only --tmpfs /tmp` for read-only root filesystem
- Health monitored via `docker inspect` (HEALTHCHECK uses `dawn-admin ping`)
- TLS: bind-mount your certificates and configure paths in `dawn.toml`

### When to Use Docker vs Bare Metal

| Choose Docker if... | Choose bare metal if... |
|---------------------|------------------------|
| You want quick setup without compiling dependencies | You need local microphone + speaker (wake word) |
| You're deploying on a cloud server or VPS | You want maximum CUDA performance on Jetson |
| You want reproducible, isolated deployments | You're developing or modifying DAWN source code |
| You're running multiple services on one host | You need to minimize disk/memory overhead |

---

## Automated Install

The install script auto-detects x86_64 and handles everything:

```bash
git clone --recursive https://github.com/The-OASIS-Project/dawn.git && cd dawn
./scripts/install.sh --preset server
```

See [GETTING_STARTED.md — Automated Install](../GETTING_STARTED.md#automated-install-recommended) for full options. The manual steps below are for reference.

---

## 1. System Dependencies

### Build tools

```bash
sudo apt install cmake build-essential pkg-config
```

### Core libraries

```bash
sudo apt install libcurl4-openssl-dev libjson-c-dev libssl-dev libsqlite3-dev \
   libsodium-dev libspdlog-dev libwebsockets-dev libopus-dev \
   libmosquitto-dev libflac-dev libsamplerate0-dev uuid-dev libncurses-dev \
   libabsl-dev libstemmer-dev
```

> **`libstemmer-dev` note**: Required as of May 2026 — the memory subsystem's BM25 keyword index (`memory_stem.c`) links against it unconditionally, so the build fails without it. Not optional even in server mode.

> **Package name note**: On Ubuntu 22.04, the abseil package is `libabsl-dev`. On Ubuntu 24.04+, it may be named `libabseil-dev`.

### Audio (required at link time even without local hardware)

```bash
sudo apt install libasound2-dev libpulse-dev
```

### Optional libraries

```bash
# Music playback codecs
sudo apt install libmpg123-dev libvorbis-dev

# Document processing (RAG — PDF and DOCX support)
sudo apt install libmupdf-dev libfreetype-dev libharfbuzz-dev libzip-dev libxml2-dev

# MuPDF transitive dependencies (static linking)
sudo apt install libmujs-dev libgumbo-dev libopenjp2-7-dev libjbig2dec0-dev

# Calendar (CalDAV)
sudo apt install libical-dev
```

> **Code Projects (coding harness)**: the `debug`, `default`, and `full` presets compile in the
> code-projects feature (`DAWN_ENABLE_CODE_PROJECTS`), which links **libgit2 ≥ 1.6**. Jammy/Noble
> apt ship a version that's too old, so it must be built from source — `scripts/install.sh` does
> this automatically (skip with `INSTALL_LIBGIT2=false`). The `server` / `server-debug` / `local` /
> `ci` presets leave the feature off and don't need libgit2. See
> [docs/CODING_PROJECTS.md](CODING_PROJECTS.md).

### CUDA (optional — for GPU-accelerated Whisper ASR)

```bash
sudo apt install nvidia-cuda-toolkit
```

CUDA is auto-detected at configure time. If the CUDA toolkit is installed and `nvidia-smi`
shows a working GPU, Whisper will use GPU inference automatically. Without CUDA, Whisper
falls back to CPU (slower but functional).

---

## 2. ONNX Runtime

ONNX Runtime is required for TTS (Piper), VAD (Silero), and the embedding engine.

**CPU-only** — install the prebuilt x86_64 package:

> **Have an NVIDIA GPU?** If you want GPU-accelerated Whisper ASR, build ONNX Runtime from source with CUDA instead. See [GETTING_STARTED.md — Option B](../GETTING_STARTED.md#step-3-install-onnx-runtime) for build instructions. The install script (`scripts/install.sh`) detects CUDA automatically and handles the source build, including the GCC 14 workaround.

```bash
wget https://github.com/microsoft/onnxruntime/releases/download/v1.22.0/onnxruntime-linux-x64-1.22.0.tgz
tar xzf onnxruntime-linux-x64-1.22.0.tgz

# Install headers (note: -r is required for the core/ subdirectory)
sudo cp -r onnxruntime-linux-x64-1.22.0/include/* /usr/local/include/
sudo cp -a onnxruntime-linux-x64-1.22.0/lib/libonnxruntime.so* /usr/local/lib/
sudo ldconfig
```

---

## 3. espeak-ng (rhasspy fork)

Piper TTS requires the rhasspy fork of espeak-ng, which adds the
`espeak_TextToPhonemesWithTerminator` symbol not present in the system package.

**Important:** Purge the system espeak-ng first to avoid library conflicts:

```bash
sudo apt purge libespeak-ng1 libespeak-ng-libespeak1 espeak-ng-data
```

Install build dependencies and build from source:

```bash
sudo apt install autotools-dev automake autoconf libtool

git clone https://github.com/rhasspy/espeak-ng.git
cd espeak-ng
./autogen.sh && ./configure --prefix=/usr
make -j$(nproc)
sudo make LIBDIR=/usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH) install
sudo ldconfig
cd ..
```

Verify the rhasspy symbol is present:

```bash
nm -D /usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH)/libespeak-ng.so | grep TextToPhonemesWithTerminator
```

---

## 4. piper-phonemize

```bash
git clone https://github.com/rhasspy/piper-phonemize.git
cd piper-phonemize
mkdir build && cd build
cmake .. -DONNXRUNTIME_DIR=/usr/local -DESPEAK_NG_DIR=/usr
make -j$(nproc)
```

**Note:** The `libpiper_phonemize.so` library builds successfully. The test executable and
CLI may fail to link — this is expected and does not affect the library.

Install the library and headers:

```bash
sudo cp -a libpiper_phonemize.so* /usr/local/lib/
sudo mkdir -p /usr/local/include/piper-phonemize
sudo cp ../src/*.hpp /usr/local/include/piper-phonemize/
sudo cp ../src/uni_algo.h /usr/local/include/piper-phonemize/
sudo ldconfig
cd ../..
```

---

## 5. MQTT Broker

MQTT is required — the daemon initializes it at startup for device command routing.
If you already installed `mosquitto mosquitto-clients` with the core libraries above, just
enable and start the service:

```bash
sudo apt install mosquitto mosquitto-clients  # if not already installed
sudo systemctl enable mosquitto
sudo systemctl start mosquitto
```

If no home automation devices will connect, MQTT runs idle with negligible resource usage.

---

## 6. Clone and Build

```bash
git clone --recursive https://github.com/The-OASIS-Project/dawn.git
cd dawn
```

### Code formatting (required for commits)

```bash
# Formatter versions must match project standard
sudo apt install clang-format-14
npm install  # For Prettier (JS/CSS/HTML formatting)
```

### Configure and build

```bash
# Debug build (recommended for initial setup)
cmake --preset debug -DENABLE_AEC=OFF

# Build
make -C build-debug -j$(nproc)
```

For a server-only build that doesn't require audio libraries:

```bash
cmake --preset server-debug
make -C build-server-debug -j$(nproc)
```

### Available presets

| Preset | Description |
|--------|-------------|
| `debug` | Full build with debug symbols (all features) |
| `default` | Full release build |
| `server` | Server-only release (no local audio required) |
| `server-debug` | Server-only with debug symbols |
| `local` | Local-only (no WebUI) |
| `full` | Full release build |

---

## 7. Download Models

```bash
./setup_models.sh
```

This downloads:
- Whisper ASR model (`base.en`, ~142MB)
- Embedding model for semantic memory search (~23MB)
- Creates symlinks in all build directories

TTS (Piper) and VAD (Silero) models are committed to git — no download needed.

---

## 8. SSL Certificates

For HTTPS (recommended — required for browser microphone access):

```bash
./generate_ssl_cert.sh
```

This creates a self-signed CA and server certificate in `ssl/`. Configure in `dawn.toml`:

```toml
[webui]
https = true
ssl_cert_path = "ssl/dawn-chain.crt"
ssl_key_path = "ssl/dawn.key"
```

For browser access, you'll need to accept the self-signed certificate or install the CA
(`ssl/ca.crt`) in your browser's trust store.

---

## 9. Configuration

### secrets.toml

Create `secrets.toml` in the project root with your API keys:

```toml
claude_api_key = "sk-ant-..."
# openai_api_key = "sk-..."
# gemini_api_key = "..."
```

The cloud LLM provider is auto-detected from available keys (Claude > OpenAI > Gemini).
You can override with `[llm.cloud] provider = "claude"` in `dawn.toml`.

### dawn.toml (optional)

DAWN works with sensible defaults. A `dawn.toml` is only needed to customize settings.
Key settings for server mode:

```toml
[general]
ai_name = "friday"
mode = "server"       # Equivalent to --server CLI flag

[webui]
enabled = true        # Default: true
bind_address = "0.0.0.0"
port = 3000
https = true
ssl_cert_path = "ssl/dawn-chain.crt"
ssl_key_path = "ssl/dawn.key"

[llm]
type = "cloud"

[mqtt]
enabled = true
broker = "127.0.0.1"
port = 1883
```

---

## 10. Run

```bash
# Server mode via CLI flag (no dawn.toml mode setting needed)
LD_LIBRARY_PATH=/usr/local/lib ./build-debug/dawn --server

# Or if mode = "server" is set in dawn.toml
LD_LIBRARY_PATH=/usr/local/lib ./build-debug/dawn
```

Access WebUI at `https://your-server:3000`.

### First admin account

There is no default account or password. On the first run (when no admin user exists), DAWN prints a one-time **setup token** to **stderr** — a `DAWN-...` value in a "DAWN FIRST-RUN SETUP TOKEN" banner. The token is valid for **5 minutes** and is single-use; restart `dawn` to regenerate one as long as no admin exists yet.

> **Running DAWN as the systemd service?** The banner won't appear on your terminal — the `dawn-server` unit redirects the daemon's stderr to `/var/log/dawn/server.log` (`StandardError=append:/var/log/dawn/server.log`). Pull the token from there instead:
>
> ```bash
> sudo grep "Token:" /var/log/dawn/server.log | tail -1
> ```
>
> If it has already expired (5-minute TTL), `sudo systemctl restart dawn-server` and grab the freshly printed one. Alternatively, run the daemon once in the foreground (`./build-debug/dawn --server`) to read the banner directly, create the admin, then start the service.

Create the first admin with the `dawn-admin` CLI, pasting the token when prompted:

```bash
./build-debug/dawn-admin user create <username> --admin
# Enter the setup token when prompted, then set a password
```

For automated provisioning, pass the token and password non-interactively via environment variables:

```bash
DAWN_SETUP_TOKEN=DAWN-XXXXXXXXXXXX DAWN_PASSWORD='your-password' \
  ./build-debug/dawn-admin user create <username> --admin
```

Then log in at the WebUI with that account. See [GETTING_STARTED.md § Create Admin Account](../GETTING_STARTED.md#6-create-admin-account) for the full walkthrough.

### What server mode skips

- Local microphone capture and speaker playback
- AEC (acoustic echo cancellation)
- VAD (voice activity detection for local mic)
- Boot greeting TTS
- Music player and audio decoder initialization

### What server mode keeps

- TTS engine (generates audio for WebUI and satellite clients via WebSocket)
- ASR engine (transcribes audio from WebUI and satellite clients)
- WebUI (full functionality including voice chat)
- All network services: LLM, calendar, email, memory, scheduler, MQTT
- Messaging channels (Telegram / Slack / Discord / SMS), notes / reference store, and document search (RAG)
- Satellite connections (Tier 1 RPi, Tier 2 ESP32) — including signed over-the-air (OTA) updates
- Code Projects (coding harness) when built with the required flags

---

## Optional Features

Everything in the README's optional-features matrix works in server mode. Each has its own
setup guide — the common ones:

| Feature | Setup guide |
|---------|-------------|
| **Messaging Channels** (Telegram / Slack / Discord / SMS) | [MESSAGING_CHANNELS_SETUP.md](MESSAGING_CHANNELS_SETUP.md) |
| **OpenRouter** (one key, any cloud model) | Set `openrouter_api_key` in `secrets.toml` + `[llm.cloud] provider = "openrouter"` |
| **Tavily** (commercial search + URL extract) | [GETTING_STARTED.md — Tavily](../GETTING_STARTED.md#tavily-commercial-search--url-extract) |
| **Home Assistant** (smart home) | [HOMEASSISTANT_SETUP.md](HOMEASSISTANT_SETUP.md) |
| **Google OAuth** (Calendar + Gmail) | [GOOGLE_OAUTH_SETUP.md](GOOGLE_OAUTH_SETUP.md) |
| **Satellite OTA updates** | [OTA_DESIGN.md](OTA_DESIGN.md) |
| **Code Projects** (index repos for code Q&A) | [CODING_PROJECTS.md](CODING_PROJECTS.md) |
| **Email** (IMAP/SMTP + Gmail OAuth) | [GETTING_STARTED.md — Email](../GETTING_STARTED.md#email-imapsmtp) |

---

## Troubleshooting

### WebUI not accessible

- Check `[webui] enabled = true` in dawn.toml (default is true)
- If using HTTPS, ensure `ssl_cert_path` and `ssl_key_path` are set
- For browser mic access, HTTPS is required (or use `localhost`)

### No TTS audio in browser

- Click the TTS toggle button (speaker icon) to enable
- TTS state persists in browser localStorage but must be synced to server on each connection

### Cloud LLM not working

- Check `secrets.toml` has the correct API key
- Provider is auto-detected; if `dawn.toml` specifies a provider without a key, it falls
  back to the first available provider
- Check server logs for `Cloud provider auto-detected: Claude` (or similar)

### Whisper slow (high RTF)

- Without CUDA: CPU inference is expected to be 2-10x realtime
- With CUDA: ensure `nvidia-smi` works and shows your GPU
- Check server logs for `gpu: yes` in the Whisper init line
- A clean rebuild is needed after installing CUDA drivers

### PulseAudio warnings at startup

If you see `libpulse` warnings on stderr, these are harmless — PulseAudio's library
constructor attempts to connect to a daemon that may not be running on a headless server.

### Ctrl+C slow to exit

- If Whisper is mid-transcription on CPU, the process waits for it to finish
- With CUDA, transcription is fast enough that shutdown is near-instant
