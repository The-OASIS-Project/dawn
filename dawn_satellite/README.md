# DAWN Satellite (Tier 1)

A smart voice satellite client for the DAWN voice assistant system. Runs on Raspberry Pi with local ASR, TTS, VAD, and wake word detection. Connects to the central DAWN daemon via WebSocket for LLM queries.

**Protocol**: DAP2 (Dawn Audio Protocol 2.0) - Text-first WebSocket

## Features

- **Hands-free activation** - Wake word detection ("Hey Friday")
- **Local speech processing** - Whisper ASR + Piper TTS on device
- **Text-first protocol** - Only text sent to daemon, not audio (~100 bytes vs ~1MB)
- **Offline fallback** - Local TTS "I can't reach the server"
- **Touchscreen UI** - SDL2-based interface with KMSDRM (no X11)
- **Full tool support** - Same capabilities as main daemon
- **Low latency** - <3s wake-to-response target

## Hardware Requirements

### Minimum (Headless)

| Component | Recommended | Notes |
|-----------|-------------|-------|
| SBC | Raspberry Pi Zero 2 W | 512MB RAM, WiFi |
| Microphone | USB mic or INMP441 | I2S or USB |
| Speaker | 3.5mm or MAX98357A | Built-in jack or I2S DAC |
| Storage | 16GB+ microSD | Class 10 or better |

### Recommended (With Display)

| Component | Recommended | Notes |
|-----------|-------------|-------|
| SBC | Raspberry Pi 4/5 | 2GB+ RAM recommended |
| Display | 7" 1024x600 TFT | Touchscreen, HDMI or DSI |
| Microphone | ReSpeaker 2-mic HAT | Or USB microphone |
| Speaker | 3W amplified speaker | Built-in or external |
| Storage | 32GB+ microSD | For models and photos |

### Memory Budget (Pi Zero 2 W)

| Component | RAM Usage |
|-----------|-----------|
| Pi OS Lite | ~100 MB |
| Whisper tiny | ~77 MB |
| Piper TTS | ~60 MB |
| Silero VAD | ~2 MB |
| Satellite app | ~20 MB |
| SDL2 UI | ~15 MB |
| **Available** | ~238 MB |

## Quick Start

### 1. Flash Pi OS Lite (64-bit)

Use [Raspberry Pi Imager](https://www.raspberrypi.com/software/) to flash **Pi OS Lite (64-bit)**.

> **Important**: Use Pi OS **Lite** (no desktop) for KMSDRM display support.

In the imager settings (gear icon), configure:
- Hostname: `dawn-satellite`
- Enable SSH
- Set username/password
- Configure WiFi credentials

### 2. First Boot Setup

```bash
# SSH into the Pi
ssh pi@dawn-satellite.local

# Update system
sudo apt update && sudo apt full-upgrade -y

# Add user to required groups
sudo usermod -aG video,render,audio,input,spi,gpio $USER

# Reboot to apply group changes
sudo reboot
```

### 3. Install Dependencies

```bash
# Core build dependencies
sudo apt install -y \
    build-essential \
    cmake \
    git \
    pkg-config

# Audio dependencies
sudo apt install -y \
    libasound2-dev

# WebSocket client and JSON parsing
sudo apt install -y \
    libwebsockets-dev \
    libjson-c-dev

# SDL2 dependencies (for touchscreen UI)
sudo apt install -y \
    libsdl2-dev \
    libsdl2-ttf-dev \
    libsdl2-image-dev \
    libdrm-dev

# Optional: GPIO button support (Tier 2 style, not needed for Tier 1)
# sudo apt install -y libgpiod-dev
```

### 4. Build the Common Library

```bash
# Clone the repository
git clone https://github.com/The-OASIS-Project/dawn.git
cd dawn

# Build the common library
cd common
mkdir build && cd build
cmake ..
make -j$(nproc)
sudo make install
cd ../..
```

### 5. Build the Satellite

```bash
cd dawn_satellite
mkdir build && cd build
cmake ..
make -j$(nproc)
```

#### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `-DENABLE_UI=ON` | ON | SDL2 touchscreen UI |
| `-DENABLE_DAP2=ON` | ON | WebSocket protocol (vs legacy DAP1) |
| `-DCMAKE_BUILD_TYPE=Release` | Release | Optimization level |

Example with UI disabled (headless):
```bash
cmake -DENABLE_UI=OFF ..
```

### 6. Configure

```bash
# Create config directory
sudo mkdir -p /etc/dawn

# Copy default config
sudo cp ../config/satellite.toml /etc/dawn/satellite.toml

# Edit configuration
sudo nano /etc/dawn/satellite.toml
```

**Essential settings to configure:**

```toml
[identity]
name = "Living Room"        # Human-readable name for logs/WebUI
location = "living_room"    # Room identifier for context

[server]
host = "192.168.1.100"      # Your DAWN daemon IP address
port = 8080                 # WebUI port (where WebSocket runs)

[audio]
capture_device = "plughw:1,0"   # Your microphone (run 'arecord -l')
playback_device = "plughw:0,0" # Your speaker (run 'aplay -l')
```

### 7. Test Run

```bash
# Run manually to test
./dawn_satellite --config /etc/dawn/satellite.toml

# Expected output:
# [INFO] DAWN Satellite v0.1.0 starting...
# [INFO] Loading config from /etc/dawn/satellite.toml
# [INFO] Connecting to ws://192.168.1.100:8080
# [INFO] WebSocket connected
# [INFO] Registered as "Living Room" (location: living_room)
# [INFO] Ready - say "Hey Friday" to activate
```

### 8. Install as System Service

```bash
# Create systemd service
sudo tee /etc/systemd/system/dawn-satellite.service << 'EOF'
[Unit]
Description=DAWN Voice Satellite
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=pi
Group=pi
ExecStart=/usr/local/bin/dawn_satellite --config /etc/dawn/satellite.toml
Restart=always
RestartSec=5
Environment=SDL_VIDEODRIVER=KMSDRM

[Install]
WantedBy=multi-user.target
EOF

# Install binary
sudo cp dawn_satellite /usr/local/bin/

# Enable and start
sudo systemctl daemon-reload
sudo systemctl enable dawn-satellite
sudo systemctl start dawn-satellite

# Check status
sudo systemctl status dawn-satellite

# View logs
journalctl -u dawn-satellite -f
```

## Configuration Reference

See `config/satellite.toml` for the full configuration file with comments.

### Key Sections

#### [identity]
```toml
uuid = ""                    # Auto-generated UUID on first run
name = "Kitchen Assistant"   # Display name in daemon logs/WebUI
location = "kitchen"         # Room for context-aware responses
```

#### [server]
```toml
host = "192.168.1.100"       # DAWN daemon IP or hostname
port = 8080                  # WebUI port
ssl = false                  # Use wss:// (requires daemon SSL config)
reconnect_delay_ms = 5000    # Reconnection backoff
max_reconnect_attempts = 0   # 0 = retry forever
```

#### [audio]
```toml
capture_device = "plughw:1,0"   # ALSA capture device
playback_device = "plughw:0,0" # ALSA playback device
sample_rate = 16000            # Must be 16kHz for Whisper
max_record_seconds = 30        # Safety timeout
```

#### [vad]
```toml
enabled = true
silence_duration_ms = 800    # Silence before end-of-speech
min_speech_ms = 250          # Minimum valid utterance
threshold = 0.5              # 0.0-1.0, higher = stricter
```

#### [wake_word]
```toml
enabled = true
word = "friday"              # Wake word (matches daemon)
sensitivity = 0.5            # 0.0-1.0, higher = more false positives
```

#### [display]
```toml
enabled = true               # Enable SDL2 UI
width = 1024                 # Display width
height = 600                 # Display height
fullscreen = true            # KMSDRM fullscreen mode
screensaver_timeout_sec = 300  # Screensaver after 5 min idle
```

## Audio Device Setup

### Find Your Devices

```bash
# List capture devices (microphones)
arecord -l

# List playback devices (speakers)
aplay -l

# Example output:
# card 1: Device [USB Audio Device], device 0: USB Audio [USB Audio]
#   Subdevices: 1/1
#   Subdevice #0: subdevice #0
```

The device name format is `plughw:CARD,DEVICE`, e.g., `plughw:1,0`.

### Test Audio

```bash
# Record 5 seconds
arecord -D plughw:1,0 -f S16_LE -r 16000 -c 1 -d 5 /tmp/test.wav

# Play it back
aplay -D plughw:0,0 /tmp/test.wav
```

### Common Device Configurations

| Hardware | Capture Device | Playback Device |
|----------|----------------|-----------------|
| USB sound card | `plughw:1,0` | `plughw:1,0` |
| ReSpeaker 2-mic | `plughw:1,0` | `plughw:0,0` (3.5mm) |
| I2S mic + DAC | `plughw:0,0` | `plughw:0,0` |
| HDMI + USB mic | `plughw:1,0` | `plughw:0,0` |

### I2S Audio Setup

For I2S microphone (INMP441) and DAC (MAX98357A), add to `/boot/config.txt`:

```ini
# Enable I2S
dtparam=i2s=on

# I2S microphone
dtoverlay=googlevoicehat-soundcard
# or for generic I2S mic:
# dtoverlay=i2s-mmap

# I2S DAC (if using MAX98357A)
dtoverlay=hifiberry-dac
```

Reboot after changes.

## Touchscreen Display Setup

### Requirements

- Pi OS **Lite** (no desktop environment)
- User in `video` and `render` groups
- SDL2 with KMSDRM backend

### Verify DRM Access

```bash
# Check DRM devices exist
ls -la /dev/dri/
# Should show: card0, card1, renderD128

# Check group membership
groups
# Should include: video render
```

### Official Raspberry Pi 7" Display

Works automatically. If needed, add to `/boot/config.txt`:

```ini
dtoverlay=vc4-kms-v3d
max_framebuffers=2
```

### HDMI Touchscreen (1024x600)

Add to `/boot/config.txt`:

```ini
# Force HDMI output
hdmi_force_hotplug=1

# Custom resolution
hdmi_group=2
hdmi_mode=87
hdmi_cvt=1024 600 60 3 0 0 0

# Enable KMS driver
dtoverlay=vc4-kms-v3d
```

### Touch Input Calibration

```bash
# Install calibration tools
sudo apt install libts-bin

# Run calibration
sudo ts_calibrate

# Test touch
ts_test
```

### Test SDL2 KMSDRM

```bash
# Force KMSDRM backend
export SDL_VIDEODRIVER=KMSDRM

# Run satellite
./dawn_satellite --config /etc/dawn/satellite.toml
```

## Troubleshooting

### Connection Issues

| Symptom | Solution |
|---------|----------|
| "Connection refused" | Check daemon is running, verify IP/port |
| "Connection timeout" | Check network, firewall (port 8080) |
| "WebSocket error" | Ensure daemon WebUI is enabled |

```bash
# Test connectivity
ping 192.168.1.100
curl -v http://192.168.1.100:8080/

# Test WebSocket (install wscat: npm install -g wscat)
wscat -c ws://192.168.1.100:8080
```

### Audio Issues

| Symptom | Solution |
|---------|----------|
| "Cannot open capture device" | Check device name with `arecord -l` |
| No sound output | Check `alsamixer`, unmute channels |
| Distorted audio | Reduce volume, check sample rate |

```bash
# Check ALSA mixer levels
alsamixer

# List all audio controls
amixer contents
```

### Display Issues

| Symptom | Solution |
|---------|----------|
| "No available video device" | Add user to `video` group, reboot |
| Black screen | Check HDMI connection, `/boot/config.txt` |
| "DRM: permission denied" | Add user to `render` group |
| Touch not working | Check `/dev/input/event*`, add to `input` group |

```bash
# Check video group
groups | grep -E "video|render"

# List input devices
ls -la /dev/input/

# Test touch events
evtest /dev/input/event0
```

### Service Issues

```bash
# Check service status
sudo systemctl status dawn-satellite

# View recent logs
journalctl -u dawn-satellite -n 50

# Follow logs live
journalctl -u dawn-satellite -f

# Restart service
sudo systemctl restart dawn-satellite
```

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                      DAWN Satellite (Tier 1)                    │
│                                                                 │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │                 Local Processing Pipeline                  │ │
│  │                                                            │ │
│  │  ┌────────┐  ┌────────┐  ┌──────────┐  ┌────────┐        │ │
│  │  │ Audio  │─▶│  VAD   │─▶│ Wake Word│─▶│  ASR   │        │ │
│  │  │Capture │  │(Silero)│  │(Whisper) │  │(Whisper│        │ │
│  │  └────────┘  └────────┘  └──────────┘  └───┬────┘        │ │
│  │                                             │ text        │ │
│  │  ┌────────┐  ┌────────┐                    ▼             │ │
│  │  │ Audio  │◀─│  TTS   │◀───────────[satellite_query]     │ │
│  │  │Playback│  │(Piper) │                                   │ │
│  │  └────────┘  └────────┘◀───────────[satellite_response]   │ │
│  │                                                            │ │
│  └───────────────────────────────────────────────────────────┘ │
│                              │                                  │
│                              │ WebSocket (JSON, ~100 bytes)     │
│                              ▼                                  │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │                     ws_client.c                            │ │
│  │  satellite_register ──▶                                    │ │
│  │  satellite_query ─────▶        ◀── satellite_response      │ │
│  │                                ◀── stream_delta (streaming)│ │
│  └───────────────────────────────────────────────────────────┘ │
│                              │                                  │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │              SDL2 Touchscreen UI (Optional)                │ │
│  │                                                            │ │
│  │  ┌──────────┐ ┌─────────────┐ ┌────────────┐ ┌──────────┐│ │
│  │  │   Orb    │ │ Transcript  │ │   Quick    │ │  Media   ││ │
│  │  │Visualize │ │  Display    │ │  Actions   │ │  Player  ││ │
│  │  └──────────┘ └─────────────┘ └────────────┘ └──────────┘│ │
│  │                                                            │ │
│  │  Screensaver: Photo Frame │ Clock │ Ambient Orb            │ │
│  └───────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
                               │
                               │ WiFi
                               ▼
┌─────────────────────────────────────────────────────────────────┐
│                         DAWN Daemon                             │
│                                                                 │
│    LLM Processing │ Tool Execution │ Conversation History       │
│    Command Parser │ MQTT Control   │ Memory System              │
└─────────────────────────────────────────────────────────────────┘
```

## Protocol Reference

### Message Types (WebSocket JSON)

| Type | Direction | Purpose |
|------|-----------|---------|
| `satellite_register` | Satellite → Daemon | Initial registration |
| `satellite_register_ack` | Daemon → Satellite | Registration confirmed |
| `satellite_query` | Satellite → Daemon | User's transcribed text |
| `stream_start` | Daemon → Satellite | Response streaming begins |
| `stream_delta` | Daemon → Satellite | Partial response text |
| `stream_end` | Daemon → Satellite | Response complete |
| `satellite_status` | Both | Health check, metrics |

### Example Flow

```
Satellite                              Daemon
    │                                     │
    │──── satellite_register ────────────▶│
    │     {uuid, name, location, tier:1}  │
    │                                     │
    │◀─── satellite_register_ack ─────────│
    │     {session_id, memory_enabled}    │
    │                                     │
    │  [User: "Hey Friday, lights on"]    │
    │  [Local: VAD → Wake → ASR]          │
    │                                     │
    │──── satellite_query ───────────────▶│
    │     {text: "lights on"}             │
    │                                     │
    │◀─── stream_start ───────────────────│
    │◀─── stream_delta ───────────────────│
    │     {delta: "I'll turn"}            │
    │  [TTS: "I'll turn"]                 │
    │◀─── stream_delta ───────────────────│
    │     {delta: " on the lights."}      │
    │  [TTS: " on the lights."]           │
    │◀─── stream_end ─────────────────────│
    │                                     │
```

## Development

### Debug Build

```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)

# Run with verbose logging
./dawn_satellite --config satellite.toml --verbose
```

### Protocol Testing

```bash
# Python test client
python3 ../tests/test_satellite_protocol.py --host 192.168.1.100 --port 8080
```

### Cross-Compilation (from x86_64)

```bash
# Install cross-compiler
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu

# Build
mkdir build-cross && cd build-cross
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-aarch64.cmake ..
make -j$(nproc)

# Copy to Pi
scp dawn_satellite pi@dawn-satellite.local:/home/pi/
```

## UI Customization

### Screensaver Photos

Place images in `/etc/dawn/photos/` or configure path in `satellite.toml`:

```toml
[display]
photos_path = "/home/pi/photos"
photo_interval_sec = 30
```

Supported formats: JPEG, PNG (will be scaled to display resolution).

### Theme Colors

The UI uses the same color palette as the DAWN WebUI. To customize, edit `src/ui/colors.h`:

```c
// Accent color (default: cyan)
static const dawn_color_t COLOR_ACCENT = {0x2D, 0xD4, 0xBF, 0xFF};  // #2dd4bf

// Alternative themes available:
// Purple: {0xA8, 0x55, 0xF7, 0xFF}  // #a855f7
// Green:  {0x7F, 0xFF, 0x7F, 0xFF}  // #7fff7f (terminal)
```

## License

GPLv3 or later. See LICENSE file in repository root.

## See Also

- [DAP2_DESIGN.md](../docs/DAP2_DESIGN.md) - Protocol specification
- [DAP2_SATELLITE.md](../docs/DAP2_SATELLITE.md) - Implementation details
- [DAWN WebUI](../www/) - Web interface (shares visual design)
