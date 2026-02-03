# DAP2 Satellite System

This document describes the DAP2 (Dawn Audio Protocol 2.0) satellite architecture for distributed voice assistants - the "JARVIS in every room" vision.

## Quick Start (Raspberry Pi)

```bash
# 1. Install dependencies
sudo apt update
sudo apt install -y build-essential cmake pkg-config \
  libasound2-dev libwebsockets-dev libjson-c-dev git

# 2. Clone and build
git clone https://github.com/YOUR_REPO/dawn.git
cd dawn/dawn_satellite
mkdir build && cd build
cmake .. -DENABLE_DAP2=ON -DENABLE_NEOPIXEL=OFF -DCMAKE_BUILD_TYPE=Release
make -j2  # Use -j2 on Pi Zero 2 W (512MB RAM), -j4 on Pi 3/4/5

# 3. Install
sudo make install

# 4. Configure (set your daemon IP)
sudo nano /etc/dawn/satellite.toml
# Edit: host = "192.168.1.100"  (your DAWN daemon IP)
# Edit: name = "Kitchen"        (this satellite's name)
# Edit: location = "kitchen"    (room location)

# 5. Test connection
dawn_satellite --keyboard --verbose

# 6. Set up auto-start
sudo tee /etc/systemd/system/dawn-satellite.service << 'EOF'
[Unit]
Description=DAWN Voice Satellite
After=network-online.target sound.target
Wants=network-online.target

[Service]
Type=simple
User=pi
ExecStart=/usr/local/bin/dawn_satellite
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl enable dawn-satellite
sudo systemctl start dawn-satellite
```

## Overview

DAP2 enables lightweight satellite devices (Raspberry Pi, etc.) to extend DAWN's voice assistant capabilities to multiple rooms. Unlike the original DAP protocol which streams audio, DAP2 uses a **text-first approach**: satellites handle speech recognition and synthesis locally, sending only text to the daemon.

**Tier 1 satellites are fully hands-free** - wake word detection triggers listening, VAD detects end-of-speech, and responses are spoken via local TTS. No buttons or LEDs required.

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
                               │ WebSocket (ws://host:8080)
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

### Tier System

| Tier | Hardware | Local Processing | Protocol | Use Case |
|------|----------|------------------|----------|----------|
| **Tier 1** | RPi 4/5, Jetson | Wake Word, VAD, ASR, TTS | DAP2 (WebSocket, text) | Hands-free room extension |
| **Tier 2** | ESP32-S3 | Button + silence detection | DAP (TCP, audio) | Budget push-to-talk |

This implementation focuses on **Tier 1** satellites.

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

| Aspect | Audio Streaming (DAP/Tier 2) | Text Protocol (DAP2/Tier 1) |
|--------|------------------------------|------------------------------|
| Bandwidth | ~960KB per interaction | ~100 bytes |
| Latency | Network + server ASR | Local ASR only |
| Offline | No response possible | "I can't reach the server" |
| Privacy | Audio leaves room | Text only over network |
| Wake Word | Push-to-talk or stream 24/7 | Always listening locally |
| Interaction | Button press required | Fully hands-free |

## Protocol Specification

DAP2 uses JSON messages over WebSocket, connecting to the same port as the WebUI (default 8080).

### Message Types

#### Satellite → Daemon

**Registration** (required before queries):
```json
{
  "type": "satellite_register",
  "payload": {
    "uuid": "550e8400-e29b-41d4-a716-446655440000",
    "name": "Kitchen Assistant",
    "location": "kitchen",
    "tier": 1,
    "capabilities": {
      "local_asr": true,
      "local_tts": true,
      "wake_word": true
    }
  }
}
```

**Query** (transcribed speech):
```json
{
  "type": "satellite_query",
  "payload": {
    "text": "turn on the kitchen lights"
  }
}
```

**Ping** (keepalive):
```json
{
  "type": "satellite_ping"
}
```

#### Daemon → Satellite

**Registration Acknowledgment**:
```json
{
  "type": "satellite_register_ack",
  "payload": {
    "success": true,
    "session_id": 12345,
    "message": "Registered as Kitchen Assistant"
  }
}
```

**State Updates**:
```json
{
  "type": "state",
  "payload": {
    "state": "thinking",
    "detail": "Processing query..."
  }
}
```

**Streaming Response**:
```json
{"type": "stream_start", "payload": {"stream_id": 1}}
{"type": "stream_delta", "payload": {"text": "I'll turn "}}
{"type": "stream_delta", "payload": {"text": "on the lights."}}
{"type": "stream_end", "payload": {"reason": "complete"}}
```

**Complete Response** (for TTS):
```json
{
  "type": "transcript",
  "payload": {
    "role": "satellite_response",
    "text": "I'll turn on the kitchen lights."
  }
}
```

**Pong**:
```json
{
  "type": "satellite_pong"
}
```

**Error**:
```json
{
  "type": "error",
  "payload": {
    "code": "NOT_REGISTERED",
    "message": "Must register before sending queries"
  }
}
```

## Directory Structure

```
dawn_satellite/
├── config/
│   └── satellite.toml      # Default configuration
├── include/
│   ├── audio_capture.h     # ALSA audio capture
│   ├── audio_playback.h    # ALSA audio playback
│   ├── neopixel.h          # WS2812 LED support
│   ├── satellite_config.h  # TOML configuration
│   ├── satellite_state.h   # State machine
│   ├── toml.h              # TOML parser (tomlc99)
│   └── ws_client.h         # WebSocket client API
├── src/
│   ├── main.c              # Entry point, CLI parsing
│   ├── audio_capture.c     # ALSA capture implementation
│   ├── audio_playback.c    # ALSA playback implementation
│   ├── neopixel.c          # SPI-based NeoPixel driver
│   ├── satellite_config.c  # Config file loading
│   ├── satellite_state.c   # State machine logic
│   ├── toml.c              # TOML parser
│   └── ws_client.c         # libwebsockets client
└── CMakeLists.txt
```

## Configuration

### Config File Locations

The satellite searches for configuration in this order:
1. `./satellite.toml` (current directory)
2. `/etc/dawn/satellite.toml` (system-wide)
3. `~/.config/dawn/satellite.toml` (user-specific)

Use `--config /path/to/file.toml` to specify explicitly.

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
port = 8080             # WebUI port
ssl = false             # Use wss:// instead of ws://
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
# GPIO (optional - for Tier 2 or development)
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
  --port 8080 \
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
| `-v, --verbose` | Enable debug output |

## Building for Raspberry Pi

### Supported Hardware

| Model | RAM | CPU | Status | Notes |
|-------|-----|-----|--------|-------|
| **Pi Zero 2 W** | 512MB | Cortex-A53 (quad) | Primary target | Compile on-device is slow (~10 min) |
| Pi 3B/3B+ | 1GB | Cortex-A53 (quad) | Supported | Good balance of cost/performance |
| Pi 4B | 2-8GB | Cortex-A72 (quad) | Supported | Best performance, overkill for satellite |
| Pi 5 | 4-8GB | Cortex-A76 (quad) | Supported | Fastest, best for local Whisper ASR |

### Step 1: Raspberry Pi OS Setup

**Flash Raspberry Pi OS Lite (64-bit)** - no desktop needed for headless satellite.

```bash
# Use Raspberry Pi Imager or:
# Download: https://www.raspberrypi.com/software/operating-systems/

# Before first boot, enable SSH and configure WiFi:
# In /boot (or /bootfs on newer images):
touch ssh
cat > wpa_supplicant.conf << 'EOF'
country=US
ctrl_interface=DIR=/var/run/wpa_supplicant GROUP=netdev
update_config=1

network={
    ssid="YOUR_WIFI_SSID"
    psk="YOUR_WIFI_PASSWORD"
}
EOF
```

**First boot setup:**
```bash
# SSH into the Pi (default: pi@raspberrypi.local)
ssh pi@raspberrypi.local

# Update system
sudo apt update && sudo apt upgrade -y

# Set hostname for easy identification
sudo hostnamectl set-hostname kitchen-satellite  # or bedroom, office, etc.

# Expand filesystem if needed
sudo raspi-config --expand-rootfs

# Reboot
sudo reboot
```

### Step 2: Install Dependencies

```bash
# Build tools
sudo apt install -y \
  build-essential \
  cmake \
  pkg-config \
  git

# Required libraries
sudo apt install -y \
  libasound2-dev \
  libwebsockets-dev \
  libjson-c-dev

# Optional: GPIO support (for development/Tier 2)
sudo apt install -y libgpiod-dev
```

**Dependency versions (Raspberry Pi OS Bookworm):**
| Package | Version | Notes |
|---------|---------|-------|
| cmake | 3.25+ | Sufficient for project |
| libwebsockets | 4.3+ | WebSocket client |
| libjson-c | 0.16+ | JSON parsing |
| libasound2 | 1.2+ | ALSA audio |

### Step 3: Clone and Build

```bash
# Clone the repository
git clone https://github.com/YOUR_REPO/dawn.git
cd dawn/dawn_satellite

# Create build directory
mkdir build && cd build

# Configure (Tier 1, no NeoPixels)
cmake .. \
  -DENABLE_DAP2=ON \
  -DENABLE_NEOPIXEL=OFF \
  -DCMAKE_BUILD_TYPE=Release

# Build
# Pi Zero 2 W: use -j2 to avoid OOM (512MB RAM)
# Pi 3/4/5: use -j4
make -j2
```

**Build times (approximate):**
| Model | Build Time |
|-------|------------|
| Pi Zero 2 W | ~8-10 minutes |
| Pi 3B+ | ~3-4 minutes |
| Pi 4B | ~1-2 minutes |

### Step 4: Install

```bash
sudo make install
```

This installs:
- `/usr/local/bin/dawn_satellite` - Main binary
- `/etc/dawn/satellite.toml` - Configuration file

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `ENABLE_DAP2` | ON | WebSocket text protocol (Tier 1) |
| `ENABLE_NEOPIXEL` | OFF | NeoPixel LED support (disabled for Tier 1) |
| `ENABLE_DISPLAY` | OFF | SPI display support |
| `CMAKE_BUILD_TYPE` | Release | Use `Debug` for development |

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

Configure in `/etc/dawn/satellite.toml`:
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

Create `/etc/systemd/system/dawn-satellite.service`:

```ini
[Unit]
Description=DAWN Voice Satellite
After=network-online.target sound.target
Wants=network-online.target

[Service]
Type=simple
User=pi
ExecStart=/usr/local/bin/dawn_satellite --config /etc/dawn/satellite.toml
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Enable and start:
```bash
sudo systemctl enable dawn-satellite
sudo systemctl start dawn-satellite
```

### Multi-Satellite Deployment

**Strategy 1: Per-device config files**
```bash
# On each satellite, customize identity:
sudo nano /etc/dawn/satellite.toml
# Set: name = "Kitchen", location = "kitchen"
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
python3 tests/test_satellite_protocol.py --host localhost --port 8080
```

Commands:
- `/register [name] [location]` - Register satellite
- `/query <text>` - Send query
- `/ping` - Send keepalive
- `<text>` - Direct query (shortcut)

### Manual Testing with wscat

```bash
npm install -g wscat
wscat -c ws://localhost:8080

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

## Current Limitations

### Not Yet Implemented

1. **Local ASR** - Whisper integration placeholder exists but not connected
2. **Local TTS** - Piper integration placeholder exists but not connected
3. **Wake Word Detection** - State machine supports it, detector not integrated
4. **VAD Integration** - Silero VAD code exists in common/, needs wiring
5. **Reconnection** - Basic reconnect logic exists, needs hardening

### Planned Features (Priority Order)

1. [ ] **VAD integration** - Wire Silero VAD for end-of-speech detection
2. [ ] **Wake word detection** - OpenWakeWord or Porcupine for "Hey Friday"
3. [ ] **Whisper.cpp integration** - Local ASR on RPi 4/5
4. [ ] **Piper integration** - Local TTS on RPi 4/5
5. [ ] Automatic reconnection with exponential backoff
6. [ ] Per-satellite conversation context
7. [ ] Room-aware responses ("the kitchen lights are now on")
8. [ ] Barge-in support (interrupt TTS with new wake word)

## Troubleshooting

### Connection Issues

```bash
# Check daemon is running and WebUI is accessible
curl http://192.168.1.100:8080/

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
cat /etc/dawn/satellite.toml
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
nc -zv 192.168.1.100 8080  # Test port

# Check firewall on daemon
sudo ufw status  # On daemon machine
```

## Related Documentation

- `docs/DAP2_DESIGN.md` - Original design proposal and rationale
- `remote_dawn/protocol_specification.md` - DAP (Tier 2) protocol spec
- `CODING_STYLE_GUIDE.md` - Code style requirements
