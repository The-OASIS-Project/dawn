# DAP2 Satellite System

This document describes the DAP2 (Dawn Audio Protocol 2.0) satellite architecture for distributed voice assistants - the "JARVIS in every room" vision.

## Quick Start (Raspberry Pi)

On a fresh Pi OS Lite (64-bit) install, from the repository root:

```bash
git clone --recursive https://github.com/The-OASIS-Project/dawn.git
cd dawn
./scripts/install.sh --target satellite
```

The unified installer walks you through an interactive setup:

1. **Discovery** — detects your Pi model, architecture, RAM, and audio devices; recommends an ASR engine based on available memory
2. **Dependencies** — installs apt packages and builds ONNX Runtime, libvosk, espeak-ng (rhasspy fork), and piper-phonemize
3. **Models** — downloads Vosk (and optionally Whisper) models via `setup_models.sh`
4. **Build & configure** — builds `dawn_satellite` with your chosen feature set and writes `satellite.toml`
5. **Deploy** — installs the systemd service via `services/dawn-satellite/install.sh`

**Before running the installer** — if your daemon uses SSL (the default) the installer will prompt for the path to the daemon's `ca.crt` on this Pi. Copy it over first:

```bash
scp user@daemon:/path/to/dawn/ssl/ca.crt /tmp/ca.crt
```

Then give `/tmp/ca.crt` at the prompt. The installer stages it at `/etc/dawn/ca.crt` and points `satellite.toml` at that path. You can skip the prompt and set `ca_cert_path` manually later if you prefer.

### Useful installer flags

| Flag | Purpose |
|---|---|
| `--target satellite` | Required for a satellite install (default is `server`). |
| `--resume-from PHASE` | Pick up from a specific phase after a failure. Phases: `deps`, `libs`, `build`, `models`, `configure`, `verify`, `deploy`. |
| `--fresh` | Ignore cached settings and re-run the prompts from scratch. |
| `--verify` | Run post-install verification only. |
| `--deploy satellite` | Run only the deploy phase (re-stage binary/config/models/service). |
| `--uninstall` | Interactive uninstall — stops the service, removes files, prompts before deleting config. |

For a manual step-by-step install (no installer script), jump to [Building for Raspberry Pi](#building-for-raspberry-pi). For the detailed build/config reference, read on.

## Installing from a .deb (prebuilt)

A prebuilt Tier-1 package is published on the GitHub release page
(`dawn-satellite_<version>_arm64.deb`, Debian trixie / Raspberry Pi OS 64-bit):

```bash
sudo apt install ./dawn-satellite_2.1.0_arm64.deb   # pulls runtime Depends
```

This installs everything except the ML models (fetched separately — see below) and lays down the
**Debian-correct layout**:

| Path | What |
|---|---|
| `/usr/bin/dawn-satellite-launch` | frozen OTA rollback launcher (the systemd `ExecStart`; root-owned, never OTA-updated) |
| `/var/lib/dawn-satellite/bin/dawn_satellite` | the binary (dawn-owned; **this** is what OTA swaps) |
| `/usr/lib/dawn-satellite/*.so` | bundled non-apt libs (whisper/ggml/onnxruntime/piper/espeak-ng) |
| `/etc/dawn-satellite/satellite.toml` | config (edit server URL, ASR/TTS, etc.) |
| `/etc/dawn/{ca.crt,ota_pubkey}` | daemon CA + OTA verify key (root-owned, you provision these) |
| `/var/lib/dawn-satellite/models/` | ML models (see *Fetching models*) |

After install the service starts in text-only mode. To finish setup:

```bash
# 1. daemon CA (if your daemon uses SSL) + server URL
sudo cp /tmp/ca.crt /etc/dawn/ca.crt && sudo chown root:root /etc/dawn/ca.crt
sudoedit /etc/dawn-satellite/satellite.toml          # set server host/port, ca_cert_path
# 2. OTA verify key (see "Over-the-air updates")  3. models (see below)
sudo systemctl restart dawn-satellite
```

The prebuilt binary ships **unsigned** — OTA trust is rooted in *your* key, not the publisher's
(see below). Verify the download against the release `SHA256SUMS` before installing.

### Fetching models

```bash
sudo dawn-satellite-fetch-models                 # VAD + Piper TTS + Vosk-small (matches the shipped config)
sudo dawn-satellite-fetch-models --asr whisper --whisper-model base   # use Whisper instead
```

Models are fetched (not bundled) so the package stays small and you accept their licenses. The helper
downloads straight from upstream over HTTPS — Silero VAD + the Piper "Alba" voice from the DAWN repo,
and your chosen ASR (Vosk from alphacephei.com, or Whisper from HuggingFace) — into
`/var/lib/dawn-satellite/models`. Pick the ASR that matches `[asr] engine` in `satellite.toml`
(default `vosk`), then `systemctl restart dawn-satellite`.

## Over-the-air updates (OTA)

The satellite applies signed binary updates pushed from the daemon (`dawn-admin ota push` or the
WebUI satellite panel). The control plane runs over the existing DAP2 WebSocket; the binary is
pulled over HTTPS, **Ed25519-verified against a keyring you provision**, atomically swapped, and the
device self-restarts. A frozen launcher (`/usr/bin/dawn-satellite-launch`) does boot-count rollback —
including for a binary that won't even launch — so a bad update can't brick the device.

**One-time per fleet — provision your verify key (offline signing):**

```bash
./build-debug/ota-keytool keygen --out-dir /tmp/otakeys     # private key stays OFFLINE
sudo install -m644 -o root -g root /tmp/otakeys/ota_signing.pub /etc/dawn/ota_pubkey
sudo systemctl restart dawn-satellite
```

`/etc/dawn/ota_pubkey` is the root-owned, read-only **trust anchor** (the `dawn` service can read but
not modify it). **Without it, the satellite refuses all OTA updates** (fail-closed). To roll out an
update: build → `ota-keytool sign --image <binary> --version X.Y.Z --platform rpi --tier 1 --abi-tag
debian-trixie-aarch64 --sk /tmp/otakeys/ota_signing.key` → place in the daemon's `[ota] release_dir`
→ `dawn-admin ota push --uuid <uuid> --version X.Y.Z`. **Sign production releases with a non-empty
`--min-version`** (the anti-rollback floor). See [OTA_DESIGN.md](OTA_DESIGN.md).

## Overview

DAP2 enables satellite devices to extend DAWN's voice assistant capabilities to multiple rooms. All satellites use **WebSocket** on the same port as the WebUI (default 3000). This document covers **Tier 1** (Raspberry Pi) satellites, which handle ASR/TTS locally and send only text to the daemon. For the complete wire protocol reference (all message types, payloads, and binary framing), see [WEBSOCKET_PROTOCOL.md](WEBSOCKET_PROTOCOL.md).

Tier 1 satellites are **fully hands-free** — wake word detection triggers listening, VAD detects end-of-speech, and responses are spoken via local TTS. No buttons or LEDs required.

```
┌─────────────────────────────────────────────────────────────────┐
│                        DAWN Daemon                               │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────────────────┐ │
│  │ WebUI   │  │   LLM   │  │  Tools  │  │ Satellite Protocol  │ │
│  │ Server  │  │ Pipeline│  │ Registry│  │    (WebSocket)      │ │
│  └────┬────┘  └────┬────┘  └────┬────┘  └──────────┬──────────┘ │
│       │            │            │                   │            │
│       └────────────┴────────────┴───────────────────┘            │
│                              │                                    │
└──────────────────────────────┼────────────────────────────────────┘
                               │ WebSocket (ws://host:3000)
           ┌───────────────────┼───────────────────┐
           │                   │                   │
           ▼                   ▼                   ▼
    ┌─────────────┐     ┌─────────────┐     ┌─────────────┐
    │  Kitchen    │     │  Bedroom    │     │   Office    │
    │  Satellite  │     │  Satellite  │     │  Satellite  │
    │             │     │             │     │             │
    │ ┌─────────┐ │     │ ┌─────────┐ │     │ ┌─────────┐ │
    │ │Wake Word│ │     │ │Wake Word│ │     │ │Wake Word│ │
    │ │  VAD    │ │     │ │  VAD    │ │     │ │  VAD    │ │
    │ │Local ASR│ │     │ │Local ASR│ │     │ │Local ASR│ │
    │ │Local TTS│ │     │ │Local TTS│ │     │ │Local TTS│ │
    │ └─────────┘ │     │ └─────────┘ │     │ └─────────┘ │
    └─────────────┘     └─────────────┘     └─────────────┘
```

## Architecture

### Tier 1 Voice Flow

```
┌─────────┐    wake word    ┌───────────┐    VAD silence    ┌────────────┐
│  IDLE   │ ──────────────► │ LISTENING │ ────────────────► │ PROCESSING │
│(monitor)│                 │ (record)  │                   │   (ASR)    │
└─────────┘                 └───────────┘                   └─────┬──────┘
     ▲                                                            │
     │                      ┌───────────┐    TTS complete         │
     └───────────────────── │ SPEAKING  │ ◄───────────────────────┘
                            │  (TTS)    │      daemon response
                            └───────────┘
```

1. **IDLE**: Always listening for wake word ("Hey Friday")
2. **LISTENING**: Recording speech, VAD monitors for silence
3. **PROCESSING**: Local ASR transcribes, sends text to daemon
4. **SPEAKING**: Local TTS renders daemon's text response

### Why Text-First?

| Aspect | Audio Path (Tier 2, ESP32) | Text Path (Tier 1, RPi) |
|--------|------------------------------|------------------------------|
| Transport | WebSocket binary PCM | WebSocket JSON |
| Bandwidth | ~960KB per interaction | ~100 bytes |
| Latency | Network + server ASR | Local ASR only |
| Offline | No response possible | "I can't reach the server" |
| Privacy | Audio leaves room | Text only over network |
| Wake Word | Push-to-talk (button) | Always listening locally |
| Interaction | Button press required | Fully hands-free |
| Music | Not supported (v1) | Opus streaming via dedicated WS |

## Protocol Specification

DAP2 uses JSON messages over WebSocket, connecting to the same port as the WebUI (default 3000). The satellite lifecycle is:

1. **Register** — `satellite_register` with UUID, name, location, tier, and capabilities
2. **Query** — `satellite_query` with transcribed text
3. **Receive** — `stream_start` / `stream_delta` / `stream_end` for streaming responses, `state` for status updates
4. **Keepalive** — `satellite_ping` / `satellite_pong` every 10 seconds

For the complete message reference with payloads and examples, see [WEBSOCKET_PROTOCOL.md](WEBSOCKET_PROTOCOL.md#dap2-satellite-messages).

## Directory Structure

```
dawn_satellite/
├── assets/fonts/       # TTF fonts (Source Sans 3, IBM Plex Mono)
├── config/             # Default satellite.toml configuration
├── include/            # Public headers
│   └── ui/             # SDL2 UI headers (music_types, orb, transcript, etc.)
├── src/                # Implementation
│   └── ui/             # SDL2 touchscreen UI (optional, ENABLE_SDL_UI)
└── CMakeLists.txt

common/                 # Shared library (daemon + satellite)
├── include/
│   ├── asr/            # ASR engine abstraction (Whisper, Vosk, VAD)
│   ├── audio/          # Thread-safe ring buffer
│   ├── tts/            # Piper TTS + text preprocessing
│   └── utils/          # Sentence buffer, string utilities
├── src/                # Implementations matching include/ layout
└── CMakeLists.txt
```

## Configuration

### Config File Locations

The satellite searches for configuration in this order:
1. `./satellite.toml` (current directory)
2. `/etc/dawn/satellite.toml` (system-wide)
3. `~/.config/dawn/satellite.toml` (user-specific)

Use `--config /path/to/file.toml` to specify explicitly. The systemd service (both the `.deb` and
`install.sh`) passes `--config /etc/dawn-satellite/satellite.toml` — that is the canonical config for
a deployed satellite; the search order above applies only to ad-hoc manual runs.

### Configuration Reference

```toml
# =============================================================================
# Identity - UNIQUE PER SATELLITE
# =============================================================================
[identity]
# UUID auto-generated if empty. Set explicitly for persistent identity:
#   uuidgen | tr '[:upper:]' '[:lower:]'
uuid = ""
name = "Satellite"
location = ""  # e.g., "kitchen", "bedroom", "office"

# =============================================================================
# Server Connection
# =============================================================================
[server]
host = "192.168.1.100"  # DAWN daemon IP
port = 3000             # WebUI port
ssl = true              # Use wss:// instead of ws://
ssl_verify = true       # Verify SSL certificates (default: true)
ca_cert_path = "/etc/dawn/ca.crt"  # Path to DAWN private CA certificate
reconnect_delay_ms = 5000
max_reconnect_attempts = 0  # 0 = infinite

# =============================================================================
# Audio
# =============================================================================
[audio]
# Use 'arecord -l' and 'aplay -l' to list devices
capture_device = "plughw:1,0"   # ReSpeaker or USB mic
playback_device = "plughw:1,0"
sample_rate = 16000
max_record_seconds = 30

# =============================================================================
# Voice Activity Detection (VAD)
# =============================================================================
[vad]
enabled = true
silence_duration_ms = 800   # Silence to trigger end-of-speech
min_speech_ms = 250         # Minimum speech before accepting
threshold = 0.5             # 0.0-1.0, higher = more strict

# =============================================================================
# Wake Word Detection
# =============================================================================
[wake_word]
enabled = true
word = "friday"             # Must match daemon's AI_NAME
sensitivity = 0.5           # 0.0-1.0, higher = more false positives

# =============================================================================
# GPIO (optional - for Tier 2 push-to-talk or development)
# =============================================================================
[gpio]
# Tier 1 uses VAD + wake word, not push-to-talk
enabled = false
chip = "gpiochip0"
button_pin = 17
button_active_low = true

# =============================================================================
# NeoPixel LEDs (optional - for custom builds)
# =============================================================================
[neopixel]
# Tier 1 RPi satellites typically don't use indicator LEDs
enabled = false
spi_device = "/dev/spidev0.0"
num_leds = 8
brightness = 64  # 0-255

# =============================================================================
# Display (optional)
# =============================================================================
[display]
enabled = false
device = "/dev/fb1"

# =============================================================================
# Screensaver / Ambient Mode
# =============================================================================
[screensaver]
enabled = true   # Activate after idle timeout
timeout = 120    # Seconds of inactivity before activation (30-600)

# =============================================================================
# SDL2 Touchscreen UI
# =============================================================================
[sdl_ui]
enabled = false       # Requires libsdl2-dev libsdl2-ttf-dev, build with -DENABLE_SDL_UI=ON
width = 1024          # Display resolution
height = 600
brightness = 100      # Saved brightness (10-100)
volume_pct = 80       # Saved volume (0-100)
time_24h = false      # 12-hour (false) or 24-hour (true) clock format
theme = "cyan"        # UI color theme: cyan, purple, green, blue, terminal

# =============================================================================
# Logging
# =============================================================================
[logging]
level = "info"  # error, warning, info, debug
use_syslog = false
```

### Command-Line Overrides

Command-line arguments override config file values:

```bash
./dawn_satellite \
  --config /etc/dawn/satellite.toml \
  --server 192.168.1.100 \
  --port 3000 \
  --name "Kitchen" \
  --location "kitchen" \
  --capture "plughw:1,0" \
  --playback "plughw:1,0" \
  --num-leds 8 \
  --verbose
```

| Option | Description |
|--------|-------------|
| `-C, --config FILE` | Config file path |
| `-s, --server IP` | Daemon hostname/IP |
| `-p, --port PORT` | WebUI port |
| `-S, --ssl` | Use secure WebSocket |
| `-N, --name NAME` | Satellite name |
| `-L, --location LOC` | Room location |
| `-c, --capture DEV` | ALSA capture device |
| `-o, --playback DEV` | ALSA playback device |
| `-k, --keyboard` | Use keyboard for testing (disables VAD) |
| `--ca-cert FILE` | Path to CA certificate for SSL verification |
| `--registration-key KEY` | Pre-shared key for satellite registration |
| `-I, --no-ssl-verify` | Disable SSL cert verification (dev only) |
| `-v, --verbose` | Enable debug output |

## TLS Setup

DAWN uses a private Certificate Authority (CA) to secure WebSocket connections. The daemon's `generate_ssl_cert.sh` creates a CA and signs the server certificate with it. Each client type needs the CA certificate to validate the server.

### RPi Satellite

1. On the daemon machine, generate certificates (if not already done):
   ```bash
   ./generate_ssl_cert.sh
   ```

2. Copy the CA certificate to the satellite:
   ```bash
   scp ssl/ca.crt pi@satellite-ip:/etc/dawn/ca.crt
   ```

3. Configure the satellite (`/etc/dawn/satellite.toml`):
   ```toml
   [server]
   ssl = true
   ssl_verify = true
   ca_cert_path = "/etc/dawn/ca.crt"
   ```

4. Or use CLI flags:
   ```bash
   dawn_satellite --ssl --ca-cert /etc/dawn/ca.crt
   ```

### ESP32 Satellite

The CA certificate is embedded at compile time:

1. Run `./generate_ssl_cert.sh` on the daemon — this auto-generates `dawn_satellite_arduino/ca_cert.h`
2. Flash the ESP32 with the updated firmware
3. The ESP32 validates the server certificate against the embedded CA

> **Security note**: WiFi credentials and the session reconnect secret are stored in plaintext on the ESP32 (compiled into the binary and NVS respectively). An attacker with physical access to the device can extract them. For production deployments, consider WiFi provisioning (BLE/SoftAP) to remove credentials from the binary, and ESP-IDF flash encryption to protect NVS contents. These are beyond the current scope of this project.

### Browser

Install the CA certificate in your OS trust store:

```bash
# Linux
sudo cp ssl/ca.crt /usr/local/share/ca-certificates/dawn-ca.crt
sudo update-ca-certificates

# macOS
sudo security add-trusted-cert -d -r trustRoot \
  -k /Library/Keychains/System.keychain ssl/ca.crt

# Windows
certutil -addstore -f "ROOT" ssl\ca.crt
```

After installing, restart your browser and access `https://<dawn-ip>:3000` with no security warnings.

### Certificate Renewal

When the daemon's IP changes or the server certificate expires (1 year):

```bash
./generate_ssl_cert.sh --renew   # Regenerates server cert, reuses CA
./generate_ssl_cert.sh --check   # Show expiry dates
```

To include an external IP or domain name (e.g., for port forwarding):

```bash
./generate_ssl_cert.sh --renew --san IP:203.0.113.50 --san DNS:dawn.example.com
```

Only the server certificate needs renewal. The CA (10-year validity) remains unchanged, so clients don't need updating.

## Registration Key

A pre-shared key prevents unauthorized devices from registering as satellites. Without a key, any device on the LAN can register.

### Generate a Key

On the daemon machine:

```bash
./generate_ssl_cert.sh --gen-key
```

This generates a 32-byte hex key and appends it to `secrets.toml`. Running it again shows the existing key without regenerating.

### Configure Satellites

**RPi satellite** (`satellite.toml`):
```toml
[server]
registration_key = "your-64-char-hex-key-here"
```

Or via CLI: `--registration-key <key>`

**ESP32 satellite** (`arduino_secrets.h`):
```c
#define SECRET_REGISTRATION_KEY "your-64-char-hex-key-here"
```

**Environment variable** (alternative for all client types):
```bash
export DAWN_SATELLITE_KEY="your-64-char-hex-key-here"
```

### Behavior

- **Key set on daemon**: Satellites must provide a matching `registration_key` in their `satellite_register` message. Mismatched or missing keys are rejected with a clear error.
- **No key on daemon**: Registration is open (backward-compatible). The daemon logs a warning at startup.

## Building for Raspberry Pi

### Supported Hardware

| Model | RAM | CPU | Status | Notes |
|-------|-----|-----|--------|-------|
| Pi Zero 2 W | 512MB | Cortex-A53 (quad) | Not recommended | Too slow for local ASR/TTS |
| Pi 3B/3B+ | 1GB | Cortex-A53 (quad) | Supported | Good balance of cost/performance |
| **Pi 4B** | 2-8GB | Cortex-A72 (quad) | **Primary target** | Recommended for satellite |
| Pi 5 | 4-8GB | Cortex-A76 (quad) | Supported | Fastest, best for local Whisper ASR |

### Step 1: Raspberry Pi OS Setup

Flash **Pi OS Lite (64-bit)** using the [Raspberry Pi Imager](https://www.raspberrypi.com/software/). Pi OS Lite is required for SDL2 KMSDRM touchscreen support; the Desktop variant brings X11 which conflicts with direct DRM rendering.

**Preferred: configure via the Imager** (click the gear icon before writing the SD card):

- Hostname: `kitchen-satellite` (or `bedroom-satellite`, `office-satellite`, etc.)
- Enable SSH with key or password auth
- Set username and password
- Configure WiFi SSID and password

<details>
<summary>Alternative: configure WiFi manually on the SD card</summary>

After flashing, with the boot partition mounted (`/boot` on older images, `/bootfs` on newer):

```bash
# Enable SSH on first boot
touch /boot/ssh

# Configure WiFi — replace with your actual SSID and password
cat > /boot/wpa_supplicant.conf << 'EOF'
country=US
ctrl_interface=DIR=/var/run/wpa_supplicant GROUP=netdev
update_config=1

network={
    ssid="MyHomeWiFi"
    psk="mySecretPassword123"
}
EOF
```

</details>

**First boot:**
```bash
# SSH into the Pi
ssh pi@kitchen-satellite.local

# Update system
sudo apt update && sudo apt full-upgrade -y

# Add your user to the groups needed for audio, video, and GPIO access
sudo usermod -aG video,render,audio,input,spi,gpio $USER

# Reboot to apply group changes
sudo reboot
```

### Step 2: Install Dependencies

> **Tip**: `./scripts/install.sh --target satellite` installs everything in this section and the next automatically. The manual procedure is here for reference and for advanced setups.

**Required apt packages** — all satellites need these:

```bash
sudo apt install -y \
  build-essential cmake git pkg-config \
  libasound2-dev \
  libwebsockets-dev \
  libjson-c-dev \
  libspdlog-dev \
  libopus-dev
```

**Optional apt packages** — install only if you want the matching feature:

```bash
# SDL2 touchscreen UI (orb, transcript, music panel, screensaver, themes)
sudo apt install -y libsdl2-dev libsdl2-ttf-dev libsdl2-gfx-dev libdrm-dev

# GPIO support (push-to-talk button, custom LED setups)
sudo apt install -y libgpiod-dev
```

**Dependency versions (Raspberry Pi OS Bookworm):**

| Package | Version | Required? | Notes |
|---------|---------|-----------|-------|
| cmake | 3.25+ | Yes | Build system |
| libwebsockets | 4.3+ | Yes | WebSocket client (DAP2) |
| libjson-c | 0.16+ | Yes | JSON message parsing |
| libasound2 | 1.2+ | Yes | ALSA audio capture/playback |
| libspdlog | 1.10+ | Yes | Required by piper-phonemize at build time |
| libopus | 1.3+ | Yes | Music streaming audio codec |
| libsdl2 | 2.0.20+ | Optional | Touchscreen UI |
| libsdl2-ttf | 2.0.18+ | Optional | UI text rendering |
| libsdl2-gfx | 1.0.4+ | Optional | Smooth circle primitives (fallback: scanline fill) |
| libgpiod | 1.6+ | Optional | GPIO button / LED control |

### Step 2.5: Voice Processing Dependencies

Tier 1 satellites run local VAD (Silero), ASR (Vosk or Whisper), and TTS (Piper). These require four libraries that aren't packaged in apt: **ONNX Runtime**, **libvosk**, **espeak-ng (rhasspy fork)**, and **piper-phonemize**. All are built or downloaded once and live in `/usr/local/`.

If you skip any of these, CMake will silently disable the feature and your satellite will start with missing capabilities. Watch for these lines in the CMake output — all must be `ON`:

```
VAD (Silero): ON
ASR (Vosk): ON
TTS (Piper): ON
Music (Opus): ON
```

#### ONNX Runtime (required for VAD and TTS)

Download the prebuilt aarch64 release:

```bash
cd ~
wget https://github.com/microsoft/onnxruntime/releases/download/v1.19.2/onnxruntime-linux-aarch64-1.19.2.tgz
tar xzf onnxruntime-linux-aarch64-1.19.2.tgz
cd onnxruntime-linux-aarch64-1.19.2
sudo cp -a lib/libonnxruntime.so* /usr/local/lib/
sudo cp include/*.h /usr/local/include/
sudo ldconfig
cd ~
```

Check https://github.com/microsoft/onnxruntime/releases for newer versions. Building from source on a Pi takes 30–60 minutes and is rarely necessary.

#### libvosk (required for Vosk ASR — the default)

```bash
cd ~
wget https://github.com/alphacep/vosk-api/releases/download/v0.3.45/vosk-linux-aarch64-0.3.45.zip
unzip vosk-linux-aarch64-0.3.45.zip
cd vosk-linux-aarch64-0.3.45
sudo cp libvosk.so /usr/local/lib/
sudo cp vosk_api.h /usr/local/include/
sudo ldconfig
cd ~
```

See https://alphacephei.com/vosk/install for latest versions and other architectures.

#### espeak-ng (rhasspy fork — required for Piper TTS)

Piper's phonemizer uses a specific fork of espeak-ng. The apt version won't work.

```bash
# Remove apt version if previously installed
sudo apt purge -y espeak-ng-data libespeak-ng1 2>/dev/null || true

# Install build tools
sudo apt install -y autoconf automake libtool

# Build the rhasspy fork
cd ~
git clone https://github.com/rhasspy/espeak-ng.git
cd espeak-ng
./autogen.sh && ./configure --prefix=/usr
make -j$(nproc)
sudo make LIBDIR=/usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH) install
cd ~
```

#### piper-phonemize (required for TTS)

Depends on both ONNX Runtime (above) and espeak-ng (above).

```bash
cd ~
git clone https://github.com/rhasspy/piper-phonemize.git
cd piper-phonemize
mkdir build && cd build
cmake .. -DONNXRUNTIME_DIR=/usr/local -DESPEAK_NG_DIR=/usr
make -j$(nproc)

# Manual install — piper-phonemize's `make install` has broken rules for system deps
sudo cp -a libpiper_phonemize.so* /usr/local/lib/
sudo mkdir -p /usr/local/include/piper-phonemize
sudo cp ../src/*.hpp /usr/local/include/piper-phonemize/
sudo cp ../src/uni_algo.h /usr/local/include/piper-phonemize/
sudo ldconfig
cd ~
```

### Step 3: Clone the Repository

```bash
git clone --recursive https://github.com/The-OASIS-Project/dawn.git
cd dawn
```

The `--recursive` flag fetches the whisper.cpp submodule. The build system links against whisper.cpp even when only Vosk ASR is enabled, so the submodule must be present.

### Step 3.5: Download Models

Run the project-root helper to download ASR models. The script places models under `models/` and handles both Vosk and Whisper.

```bash
# Recommended starting point for most Pis (Vosk small, ~40MB)
./setup_models.sh --vosk-small

# Add Whisper tiny if you also want Whisper as an option (Pi 4 can handle it)
./setup_models.sh --vosk-small --whisper-model tiny-q5_1

# Full Vosk model for higher accuracy (~1.8GB — Pi 4 with 4GB+ RAM only)
./setup_models.sh --vosk

# See all options
./setup_models.sh --help
```

**Model recommendations by hardware** (the installer picks these automatically from `/proc/meminfo` and the Pi model):

| Hardware | Recommended | Why |
|---|---|---|
| Pi Zero / Zero 2 W | Not supported | 512MB RAM is too tight for simultaneous VAD + ASR + TTS |
| Pi 3B (1GB) | Not recommended | CPU is too slow for a responsive voice pipeline |
| Pi 4 (2GB) | Vosk small | `vosk-small` fits the memory budget; Whisper tiny pushes RAM too close to the limit |
| Pi 4 (4–8GB) | Vosk small (default), Whisper tiny-q5_1 (optional) | Pi 4 CPU limits whisper.cpp tiny-q5_1 to RTF ~0.9 (≈4s finalize). Vosk streaming is noticeably snappier |
| Pi 5 (4GB) | Whisper tiny-q5_1 | Pi 5 CPU hits RTF ~0.3 (≈1s finalize for a 3s utterance); quality beats Vosk small |
| Pi 5 (8GB) | Whisper tiny-q5_1 (default) or base | `tiny-q5_1` is the installer default; `base` is viable on 8GB — measured RTF 0.65 (JFK, beam=5) / 0.20 (greedy, 15s real speech). Sustained continuous transcription can thermal-throttle per [ACM 2025](https://dl.acm.org/doi/10.1145/3769102.3774244); intermittent voice-command workloads are fine with active cooling |

Benchmark sources: [ACM 2025 Whisper-on-Pi evaluation](https://dl.acm.org/doi/10.1145/3769102.3774244), [whisper.cpp Pi 4 issue #89](https://github.com/ggml-org/whisper.cpp/issues/89). RTF = real-time factor; lower is faster (&lt; 1.0 means faster than realtime).

**Measured performance (Raspberry Pi 5, 8GB, 4 threads, active cooling):**

| Config | Sample | Wall time | RTF | Notes |
|---|---|---|---|---|
| Whisper base, beam=5 best_of=5 | JFK 11s (whisper.cpp reference) | 7196 ms | **0.65** | `whisper-cli` defaults — directly comparable to published numbers |
| Whisper base, greedy | 8-15s real speech | 2.5-3.0 s | **0.19-0.28** | Satellite runtime config (greedy decode) |
| Whisper base, greedy | 2-3s short utterances | 2.0-2.1 s | **0.70-0.85** | Fixed encoder cost dominates on short audio |

Encoder is ~85% of wall time on CPU. Beam search (the `whisper-cli` default) roughly halves effective RTF by fanning out decode paths; the satellite uses greedy decode for latency, so real-world numbers are faster than the JFK figure.

Sustained continuous transcription (not typical for voice-command workloads) can thermal-throttle `whisper base` on Pi 5 per the ACM 2025 paper — the numbers above reflect intermittent utterance workloads with active cooling.

Reproduce with:
```bash
cd ~/dawn/whisper.cpp && cmake -B build -DWHISPER_BUILD_EXAMPLES=ON -DGGML_NATIVE=ON
cmake --build build -j4 --target whisper-cli
./build/bin/whisper-cli -m /var/lib/dawn-satellite/models/whisper.cpp/ggml-base.en.bin \
  -f samples/jfk.wav -t 4
```

Silero VAD and Piper voice models ship with the repository under `models/` — no download needed:

| Model | Path | Purpose |
|---|---|---|
| Silero VAD | `models/silero_vad_16k_op15.onnx` | Voice activity detection |
| Alba (en_GB) | `models/en_GB-alba-medium.onnx` | TTS (Friday persona) |
| Northern English Male | `models/en_GB-northern_english_male-medium.onnx` | TTS (Jarvis persona) |

### Step 4: Build the Satellite

```bash
cd dawn_satellite
mkdir build && cd build

cmake .. \
  -DENABLE_DAP2=ON \
  -DENABLE_NEOPIXEL=OFF \
  -DCMAKE_BUILD_TYPE=Release

# Pi 3/4/5: -j4   |  Pi Zero 2 W: -j2 (avoid OOM)
make -j4
```

Confirm the CMake summary shows all voice features enabled:

```
-- === DAWN Satellite Configuration ===
-- VAD (Silero): ON       ← required
-- ASR (Whisper): OFF     ← OK to leave OFF unless you chose Whisper
-- ASR (Vosk): ON         ← required (default ASR)
-- TTS (Piper): ON        ← required
-- Music (Opus): ON       ← required for music streaming
-- =====================================
```

If any required feature shows `OFF`, revisit [Step 2.5](#step-25-voice-processing-dependencies) — the matching library is missing. See [Troubleshooting → Build fails with DISABLED warnings](#build-fails-with-disabled-warnings) for specific package-to-feature mapping.

**Build times (approximate):**

| Model | Build Time |
|-------|------------|
| Pi Zero 2 W | 8–10 minutes |
| Pi 3B+ | 3–4 minutes |
| Pi 4B | 1–2 minutes |
| Pi 5 | ~1 minute |

### Step 5: Install as a Service

Use the service installer — it creates a dedicated `dawn` system user, installs the binary and models to standard paths, deploys the systemd unit, and sets up logrotate:

```bash
sudo ./services/dawn-satellite/install.sh
```

This installs:

- `/usr/local/bin/dawn_satellite` — main binary
- `/usr/local/etc/dawn-satellite/satellite.toml` — configuration (edit this after install)
- `/var/lib/dawn-satellite/` — runtime state (UUID, reconnect secret)
- `/var/log/dawn-satellite/` — logs with logrotate
- `/etc/systemd/system/dawn-satellite.service` — systemd unit

See [`services/dawn-satellite/README.md`](../services/dawn-satellite/README.md) for the manual install steps, path customization, and troubleshooting.

### Build Options

| Option | Default | Description |
|---|---|---|
| `ENABLE_DAP2` | ON | WebSocket text protocol (required for Tier 1) |
| `ENABLE_VAD` | ON | Silero VAD (requires ONNX Runtime) |
| `ENABLE_VOSK_ASR` | ON | Vosk streaming ASR (default, low-latency) |
| `ENABLE_WHISPER_ASR` | OFF | Whisper batch ASR (optional, slightly more accurate, ~4s inference on Pi 4) |
| `ENABLE_TTS` | ON | Piper TTS (requires ONNX Runtime + piper-phonemize) |
| `ENABLE_NEOPIXEL` | ON | NeoPixel/WS2812 LED support via SPI (pass `OFF` for headless builds) |
| `ENABLE_SDL_UI` | OFF | SDL2 touchscreen UI with 5-theme system |
| `ENABLE_DISPLAY` | OFF | SPI framebuffer display support |
| `CMAKE_BUILD_TYPE` | Release | Use `Debug` for development |

Default `cmake ..` gives: VAD + Vosk + TTS + NeoPixel (no Whisper, no SDL UI).

Add Whisper alongside Vosk (runtime-selectable via config):

```bash
cmake .. -DENABLE_WHISPER_ASR=ON -DENABLE_NEOPIXEL=OFF
```

Text-only mode for debugging (keyboard input, no mic/speaker):

```bash
cmake .. -DENABLE_VAD=OFF -DENABLE_VOSK_ASR=OFF -DENABLE_TTS=OFF
```

Enable the touchscreen UI:

```bash
cmake .. -DENABLE_SDL_UI=ON
```

### Cross-Compilation (Optional)

For faster builds, cross-compile on a desktop Linux machine:

```bash
# Install cross-compiler (Ubuntu/Debian)
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu

# Configure for cross-compilation
cmake .. \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
  -DENABLE_DAP2=ON \
  -DENABLE_NEOPIXEL=OFF

make -j$(nproc)

# Copy to Pi
scp dawn_satellite pi@kitchen-satellite.local:/usr/local/bin/
```

**Note:** Cross-compilation requires matching library versions. Native compilation on the Pi is simpler.

## Audio Hardware Setup

### Recommended: USB Audio Adapter

The Pi Zero 2 W has no built-in audio output. Use a USB audio adapter:

```bash
# List audio devices
arecord -l  # Capture devices
aplay -l    # Playback devices

# Example output:
# card 1: Device [USB Audio Device], device 0: USB Audio [USB Audio]
#   Subdevices: 1/1
#   Subdevice #0: subdevice #0
```

Configure in `/usr/local/etc/dawn-satellite/satellite.toml` (after running the service installer):

```toml
[audio]
capture_device = "plughw:1,0"   # USB mic
playback_device = "plughw:1,0" # USB speaker
```

### Test Audio

```bash
# Test recording (5 seconds)
arecord -D plughw:1,0 -f S16_LE -r 16000 -c 1 -d 5 test.wav

# Test playback
aplay -D plughw:1,0 test.wav

# Adjust volume
alsamixer
```

### ReSpeaker Support

For ReSpeaker 2-Mic HAT or similar I2S microphone arrays:

```bash
# Install drivers (ReSpeaker 2-Mic)
git clone https://github.com/respeaker/seeed-voicecard
cd seeed-voicecard
sudo ./install.sh
sudo reboot

# Device will appear as:
# card 0: seeed2micvoicec [seeed-2mic-voicecard], device 0: ...
```

Configure:
```toml
[audio]
capture_device = "plughw:0,0"
playback_device = "plughw:0,0"
```

## Deployment

### Systemd Service

[Step 5](#step-5-install-as-a-service) above covers the standard install via `services/dawn-satellite/install.sh`. That script creates a dedicated `dawn` user, deploys the binary + config + systemd unit + logrotate in one call. See [`services/dawn-satellite/README.md`](../services/dawn-satellite/README.md) for service-level details (file paths, logrotate, manual steps, troubleshooting).

After install, manage the service with standard systemd commands:

```bash
sudo systemctl enable --now dawn-satellite    # enable at boot + start now
sudo systemctl restart dawn-satellite         # after editing satellite.toml
journalctl -u dawn-satellite -f               # follow logs
```

### Multi-Satellite Deployment

**Strategy 1: Per-device config files**
```bash
# On each satellite, customize identity after service install:
sudo nano /usr/local/etc/dawn-satellite/satellite.toml
# Set: name = "Kitchen", location = "kitchen"
sudo systemctl restart dawn-satellite
```

**Strategy 2: Base config + service overrides**
```bash
# Same config everywhere
# Customize in systemd:
sudo systemctl edit dawn-satellite
```
```ini
[Service]
ExecStart=
ExecStart=/usr/local/bin/dawn_satellite \
  --name "Kitchen" --location "kitchen"
```

**Strategy 3: Hostname-based identity**
```bash
# Set hostname to match location
sudo hostnamectl set-hostname kitchen-satellite

# In config or startup script:
name = "$(hostname)"
```

## Testing

### Python Test Client

For protocol testing without audio hardware:

```bash
pip3 install websocket-client
python3 tests/test_satellite_protocol.py --host localhost --port 3000
```

Commands:
- `/register [name] [location]` - Register satellite
- `/query <text>` - Send query
- `/ping` - Send keepalive
- `<text>` - Direct query (shortcut)

### Manual Testing with wscat

```bash
npm install -g wscat
wscat -c ws://localhost:3000

# Register
{"type":"satellite_register","payload":{"uuid":"test-123","name":"Test","location":"test","tier":1,"capabilities":{"local_asr":true,"local_tts":true}}}

# Query
{"type":"satellite_query","payload":{"text":"what time is it"}}
```

### Satellite Binary Testing

```bash
# Development mode: keyboard input instead of VAD
./dawn_satellite --keyboard --verbose

# Type text at the prompt to simulate transcribed speech
# This bypasses wake word and VAD for testing

# Production mode: VAD + wake word (requires ASR/TTS integration)
./dawn_satellite --verbose
# Say "Hey Friday" to activate, speak your query
```

## Current Status

### Implemented — Core Voice Pipeline

1. **Local VAD** - Silero ONNX model for real-time speech detection
2. **Wake Word Detection** - VAD + ASR transcription checked for wake word ("Hey Friday")
3. **Local ASR** - Vosk streaming (default, near-instant) or Whisper batch
4. **Local TTS** - Piper with sentence-level streaming during LLM response
5. **Session Reconnection** - Cryptographic reconnect secrets, conversation history preserved
6. **Offline Fallback** - TTS "I can't reach the server right now" on connection loss
7. **Time-of-Day Greeting** - Spoken on startup/reconnection (morning/day/evening)
8. **Unified Logging** - Same format as daemon (timestamps, colors, file:line)
9. **Clean Shutdown** - Ctrl+C responsive in all states (model loading, TTS playback, idle)
10. **Music Library Pagination** - Browse full library via voice (50 items per page)

### Implemented — SDL2 Touchscreen UI

11. **Orb visualization** - Animated orb with state-driven colors (idle/listening/thinking/speaking)
12. **FFT spectrum bars** - Real-time frequency visualization around orb during speech playback
13. **Streaming transcript** - Live-updating conversation history (40 entries, 4KB per entry)
14. **Inline markdown rendering** - Bold, italic, code spans, and bullet points in AI responses
15. **Touch-drag scrolling** - Finger and mouse scrolling with auto-scroll on new messages
16. **User transcription display** - Shows "You: ..." after ASR completes
17. **Touch gestures** - Tap orb to cancel/listen, drag to scroll, swipe for panels
18. **Status bar** - WiFi signal quality, connection status, tool call info, date/time
19. **KMSDRM backend** - Direct rendering without X11, suitable for headless Pi setups
20. **UI persists across reconnections** - SDL lifecycle independent of WebSocket connection
21. **Settings panel** - Pull-down panel with server address, connection status, device name, IP, uptime, session duration
22. **TTS playback queue** - Producer-consumer pipeline: synthesize sentence N+1 while playing sentence N

### Implemented — Music Player

23. **Music player panel** - Three-tab design: Playing (transport + visualizer), Queue, Library
24. **Transport controls** - Play/pause, prev/next, shuffle/repeat toggles, progress bar with drag-to-seek
25. **Opus audio streaming** - Daemon streams music via Opus over WebSocket, satellite decodes to ALSA
26. **Live FFT visualizer** - Goertzel analysis on audio stream (not simulated), 16 radial bars
27. **Library browsing** - Artists, albums, and tracks with paginated navigation (50 items/page)
28. **Artist/album drill-down** - Tap artist to see tracks, tap album to see tracks
29. **SDL primitive icons** - All UI icons drawn with SDL (no image assets needed)
30. **Daemon satellite auth** - Per-handler whitelist allows satellite access to music endpoints

### Implemented — Settings & Display

31. **Brightness slider** - sysfs backlight control for DSI displays, software dimming fallback for HDMI
32. **Volume slider** - ALSA mixer control from settings panel
33. **Persistent settings** - Brightness, volume, time format, and theme saved to config file across restarts
34. **12/24-hour time toggle** - Animated toggle in settings panel; applies to both main clock and screensaver
35. **Buffer-compensated position** - Music progress bar subtracts ring buffer + ALSA delay for accurate display
36. **5-theme system** - Cyan, Purple, Green, Blue, Terminal with dot picker, 200ms crossfade, TOML persistence
37. **SDL2_gfx circle primitives** - Smooth anti-aliased circles for orb rendering (fallback to scanline fill without library)
38. **Lock-free music playback** - SPSC ring buffer with LWS-thread drain eliminates relay thread and playback underruns

### Implemented — Screensaver / Ambient Mode

36. **Clock mode** - Time and date centered with Lissajous drift for burn-in prevention
37. **"D.A.W.N." watermarks** - Corner watermarks with sine-pulse fade animation
38. **Fullscreen rainbow visualizer** - 64-bin Goertzel FFT spectrum with HSV color cycling, peak hold, gradient reflections
39. **Track info pill** - Two-line display (large title, smaller album/artist) with fade-in/out on track change
40. **dB-scale spectrum** - Matches WebUI's getByteFrequencyData approach (60dB range, 0.7 gamma)
41. **Auto-activation** - Configurable idle timeout (default 120s), panels block timer
42. **Manual trigger** - Tap music panel visualizer to enter fullscreen mode
43. **Wake word dismissal** - Only dismissed by wake word detection, not simple VAD

### Implemented — Reliability & Bug Fixes

23. **App-level keep-alive** - `satellite_ping` every 10s (WS-level pings disabled for lws 4.3.5 compat)
24. **Ping-query race fix** - `ws_client_ping()` skips if tx_buffer already has pending data
25. **Response timeout** - Returns to idle after 30s (no data) or 120s (streaming stalled)
26. **Sentence breaking** - Breaks on `.!?`, bullets (`\n-`, `\n*`), numbered lists, paragraphs, `:\n`
27. **Emoji stripping** - Full SMP coverage (0x1F000-0x1FFFF) for both TTS and display rendering
28. **Atomic init guards** - Thread-safe `_Atomic bool` with `atomic_exchange()` for lookup tables

### Planned Features

1. [ ] **Barge-in support** - Interrupt TTS by speaking (stubbed, not connected)
2. [x] ~~**Quick actions panel**~~ - Replaced by status bar icon pattern (icons appear in transcript header as features ship)
3. [x] ~~**Screensaver / ambient mode**~~ - Clock with Lissajous drift + fullscreen rainbow FFT visualizer
4. [x] ~~**Theme support**~~ - 5-theme system (Cyan, Purple, Green, Blue, Terminal) with dot picker, crossfade, TOML persistence
5. [x] ~~**TTS ducking during music**~~ - Volume ducks to 30% during voice activity (wake word, recording, processing), hard-pauses during TTS
6. [x] ~~**Multi-satellite routing**~~ - Per-session room context in system prompt; daemon knows which room each query originates from
7. [ ] **Speaker identification** - Personalized responses per user
8. [x] ~~**Location-aware queries**~~ - Room context sent with the turn's standing directions (`build_directives` in `prompt_builder.c`), as a sentence: the local session's room from `dawn.toml` `room`, a satellite's `location` field and its admin-assigned Home Assistant area
9. [x] ~~**Satellite user management**~~ - Persistent satellite-to-user mapping via `satellite_mappings` DB table (schema v21). WebUI admin panel for user assignment, HA area dropdown, delete. Mapped satellites inherit user's music queue, memory extraction, and personalized prompt. Force-reconnect on config change.

## Troubleshooting

### Build fails with DISABLED warnings

If `cmake ..` reports warnings like the ones below, the matching library is missing. Each line maps to an apt package or a step in [Step 2.5](#step-25-voice-processing-dependencies):

```
CMake Warning: ONNX Runtime not found - VAD support disabled
CMake Warning: ONNX Runtime not found - TTS support disabled
CMake Warning: libvosk not found - Vosk ASR support disabled
CMake Warning: piper-phonemize not found - TTS support disabled
Music streaming (Opus): DISABLED (install libopus-dev to enable)
GPIO support: DISABLED (install libgpiod-dev to enable)
```

| Warning | Fix |
|---|---|
| `ONNX Runtime not found` | [Install ONNX Runtime prebuilt aarch64](#onnx-runtime-required-for-vad-and-tts) |
| `libvosk not found` | [Install libvosk prebuilt aarch64](#libvosk-required-for-vosk-asr--the-default) |
| `piper-phonemize not found` | [Install piper-phonemize from source](#piper-phonemize-required-for-tts) — requires ONNX Runtime + espeak-ng rhasspy fork first |
| `Music streaming (Opus): DISABLED` | `sudo apt install libopus-dev`, then re-run cmake |
| `GPIO support: DISABLED` | Only needed for push-to-talk or custom GPIO LEDs — ignore for typical Tier 1 builds |
| `spdlog not found` | `sudo apt install libspdlog-dev`, then re-run cmake |

After installing missing libraries, delete the build directory and re-configure from scratch:

```bash
cd dawn_satellite
rm -rf build && mkdir build && cd build
cmake .. -DENABLE_DAP2=ON -DENABLE_NEOPIXEL=OFF -DCMAKE_BUILD_TYPE=Release
```

CMake caches its detection results, so adding a library after the first `cmake ..` requires a fresh build directory (or at minimum `rm build/CMakeCache.txt`).

### Models not found at runtime

If the satellite starts but logs `Failed to load model` or `[vosk] ERROR: failed to open`:

```bash
# From the dawn repo root, not the build directory
./setup_models.sh --help
./setup_models.sh --vosk-small   # for default Vosk ASR
```

After the service installer has run, models also need to be accessible to the `dawn` service user. `services/dawn-satellite/install.sh` handles this by default (symlinks models into `/usr/local/share/dawn-satellite/models/`). Verify with:

```bash
ls -la /usr/local/share/dawn-satellite/models/
```

Check the paths in your config match what's on disk:

```bash
grep -E 'model_path|config_path' /usr/local/etc/dawn-satellite/satellite.toml
```

### Connection Issues

```bash
# Check daemon is running and WebUI is accessible
curl http://192.168.1.100:3000/

# Test WebSocket with verbose output
./dawn_satellite --server 192.168.1.100 --verbose
```

### Audio Issues

```bash
# List capture devices
arecord -l

# List playback devices
aplay -l

# Test capture
arecord -D plughw:1,0 -f S16_LE -r 16000 -c 1 test.wav

# Test playback
aplay -D plughw:1,0 test.wav
```

### Service Issues

```bash
# Check service status
sudo systemctl status dawn-satellite

# View logs
journalctl -u dawn-satellite -f

# Check if binary exists
which dawn_satellite
ls -la /usr/local/bin/dawn_satellite

# Check config file
cat /usr/local/etc/dawn-satellite/satellite.toml
```

### Memory Issues (Pi Zero 2 W)

The Pi Zero 2 W has only 512MB RAM. If builds fail:

```bash
# Create swap file
sudo fallocate -l 1G /swapfile
sudo chmod 600 /swapfile
sudo mkswap /swapfile
sudo swapon /swapfile

# Make permanent
echo '/swapfile none swap sw 0 0' | sudo tee -a /etc/fstab

# Build with fewer parallel jobs
make -j1
```

### Network Issues

```bash
# Check WiFi connection
iwconfig wlan0
ping -c 3 google.com

# Check if daemon is reachable
ping -c 3 192.168.1.100  # Your daemon IP
nc -zv 192.168.1.100 3000  # Test port

# Check firewall on daemon
sudo ufw status  # On daemon machine
```

## Tier 2 Satellite (ESP32-S3)

Tier 2 satellites are push-to-talk devices with server-side ASR and TTS. The implementation lives in `dawn_satellite_arduino/` as an Arduino sketch (not ESP-IDF).

### Hardware

| Component | Part | Notes |
|-----------|------|-------|
| MCU | Adafruit ESP32-S3 TFT Feather | Built-in 240x135 TFT + NeoPixels, 2MB OPI PSRAM |
| Speaker amp | MAX98357 I2S breakout | 3.3V logic, mono, 48kHz stereo output |
| Microphone | Analog electret mic module | Connected to ADC pin (GPIO 1) |
| Button | Momentary push button | Push-to-talk trigger (GPIO 18) |

### Quick Start

1. Install the Arduino IDE with ESP32 board support (see `dawn_satellite_arduino/README.md`)
2. Copy `arduino_secrets.h.example` to `arduino_secrets.h` and fill in WiFi/server credentials
3. Select board "Adafruit Feather ESP32-S3 TFT" with OPI PSRAM enabled
4. Upload and open Serial Monitor at 115200 baud

### How It Works

```
User presses button → ADC samples at 16kHz → PCM chunks sent via WS binary 0x01
User releases button → End marker 0x02 sent
Daemon: Whisper ASR → LLM → Piper TTS → 22050Hz PCM via WS binary 0x11/0x12
ESP32: Ring buffer → 22050→48kHz resample → I2S stereo → Speaker
```

### Key Implementation Details

- **Ring buffer**: 524,288-sample power-of-two buffer in PSRAM (~1MB), spinlock-protected
- **Resampling**: 22050→48000Hz linear interpolation with cross-boundary carry sample
- **TCP backpressure**: Skips `webSocket.loop()` when ring buffer >50% full
- **NVS persistence**: Random UUID v4 and reconnect_secret survive power cycles
- **Credentials**: `arduino_secrets.h` is gitignored; template in `.h.example`
- **I2S channel**: Created once in `setup()`, enable/disable per playback (avoids DMA descriptor fragmentation)
- **NeoPixels**: Mode-change detection — static modes (recording/playing/waiting) only call `strip.show()` once on transition

For full details, see `dawn_satellite_arduino/README.md`.

## Related Documentation

- `docs/WEBSOCKET_PROTOCOL.md` - Complete wire protocol reference (all message types and payloads)
- `dawn_satellite_arduino/README.md` - Tier 2 Arduino sketch setup and usage
- `ARCHITECTURE.md` - System architecture with DAP2 protocol overview
- `CODING_STYLE_GUIDE.md` - Code style requirements
