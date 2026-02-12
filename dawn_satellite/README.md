# DAWN Satellite (Tier 1)

A smart voice satellite client for the DAWN voice assistant system. Runs on Raspberry Pi with local ASR, TTS, VAD, and wake word detection. Connects to the central DAWN daemon via WebSocket for LLM queries.

**Protocol**: DAP2 (Dawn Audio Protocol 2.0) - Text-first WebSocket

## Features

- **Hands-free activation** - Wake word detection ("Hey Friday")
- **Local speech processing** - Vosk ASR (streaming) + Piper TTS on device
- **Near-instant ASR** - Vosk decodes audio incrementally during recording, ~0s finalize
- **Text-first protocol** - Only text sent to daemon, not audio (~100 bytes vs ~1MB)
- **Offline fallback** - Local TTS "I can't reach the server"
- **Full tool support** - Same capabilities as main daemon
- **Dual ASR engines** - Vosk (default, streaming) or Whisper (batch, higher accuracy)
- **Music streaming** - Opus audio from daemon with lock-free ring buffer + LWS-thread drain playback
- **Touchscreen UI** - SDL2 orb visualization, scrollable transcript, music player, screensaver, 5 themes
- **5 color themes** - Cyan, Purple, Green, Blue, Terminal with dot picker, crossfade, and TOML persistence
- **Screensaver** - Clock mode with Lissajous drift + fullscreen rainbow FFT visualizer
- **Brightness/volume control** - Sliders in settings panel, sysfs backlight + software dimming fallback

## ASR Engine Comparison

| Feature | Vosk (default) | Whisper |
|---------|---------------|---------|
| Mode | Streaming (incremental) | Batch (all-at-once) |
| Finalize latency | ~0s (already decoded) | ~4s on Pi 4 (tiny model) |
| Model size | ~40MB (small-en-us-0.15) | ~30MB (tiny.en-q5_1) |
| Accuracy | Good for commands | Slightly better |
| RAM usage | ~100MB | ~270MB |
| Dependencies | libvosk, json-c | whisper.cpp (compiled) |

**Vosk is the default** because the ~4s Whisper inference time on Pi hardware creates a noticeable delay between speaking and response. Vosk processes audio incrementally during recording, so when silence is detected the transcription is available instantly.

To use Whisper instead, set `engine = "whisper"` in `satellite.toml` and rebuild with the Whisper model path.

## Hardware Requirements

### Minimum (Headless Voice Satellite)

| Component | Recommended | Notes |
|-----------|-------------|-------|
| SBC | Raspberry Pi 4 (2GB+) | Cortex-A72, minimum for local ASR/TTS |
| Microphone | USB mic or INMP441 | I2S or USB |
| Speaker | 3.5mm or MAX98357A | Built-in jack or I2S DAC |
| Storage | 16GB+ microSD | Class 10 or better |

### Recommended (With Display)

| Component | Recommended | Notes |
|-----------|-------------|-------|
| SBC | Raspberry Pi 4/5 (4GB+) | Extra RAM for display + models |
| Display | 7" 1024x600 TFT | Touchscreen, HDMI or DSI |
| Microphone | ReSpeaker 2-mic HAT | Or USB microphone |
| Speaker | 3W amplified speaker | Built-in or external |
| Storage | 32GB+ microSD | For models and photos |

> **Note:** The Pi Zero 2 W (512MB, Cortex-A53) does not have enough RAM or CPU for simultaneous ASR + Piper TTS. Use a Pi 4 or better for Tier 1 satellites.

### Memory Budget (Pi 4, 2GB)

| Component | RAM Usage |
|-----------|-----------|
| Pi OS Lite | ~100 MB |
| Vosk model + recognizer | ~100 MB |
| Piper TTS | ~60 MB |
| Silero VAD | ~2 MB |
| Satellite app | ~20 MB |
| **Available** | ~1,720 MB |

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
    pkg-config \
    libspdlog-dev

# Audio dependencies
sudo apt install -y \
    libasound2-dev

# WebSocket client and JSON parsing
sudo apt install -y \
    libwebsockets-dev \
    libjson-c-dev

# Optional: SDL2 dependencies (for touchscreen UI - orb + transcript)
sudo apt install -y libsdl2-dev libsdl2-ttf-dev libsdl2-gfx-dev libdrm-dev

# Optional: Music streaming support (Opus audio playback from daemon)
sudo apt install -y libopus-dev

# Optional: GPIO button support (Tier 2 style, not needed for Tier 1)
# sudo apt install -y libgpiod-dev
```

### 4. Voice Processing Dependencies

Tier 1 satellites run local VAD, ASR, and TTS. Install these dependencies for voice processing.

#### Vosk (default ASR engine - streaming)

Vosk is the default ASR engine for Tier 1 satellites. It processes audio incrementally during recording, so transcription is available near-instantly when silence is detected.

```bash
# Download pre-built ARM64 libvosk
# Check https://alphacephei.com/vosk/install for latest version
wget https://github.com/alphacep/vosk-api/releases/download/v0.3.45/vosk-linux-aarch64-0.3.45.zip
unzip vosk-linux-aarch64-0.3.45.zip
cd vosk-linux-aarch64-0.3.45

# Install library and header
sudo cp libvosk.so /usr/local/lib/
sudo cp vosk_api.h /usr/local/include/
sudo ldconfig
cd ..
```

> **Note:** If the pre-built binary isn't available for your platform, Vosk can also be built from source. See https://alphacephei.com/vosk/install for instructions.

json-c is also required (already installed in step 3 above via `libjson-c-dev`).

#### ONNX Runtime (required for VAD and TTS)

For Raspberry Pi (ARM64 CPU-only):

```bash
# Download pre-built ARM64 release (recommended)
# Check https://github.com/microsoft/onnxruntime/releases for latest version
wget https://github.com/microsoft/onnxruntime/releases/download/v1.16.3/onnxruntime-linux-aarch64-1.16.3.tgz
tar xzf onnxruntime-linux-aarch64-1.16.3.tgz
cd onnxruntime-linux-aarch64-1.16.3

# Install
sudo cp -a lib/libonnxruntime.so* /usr/local/lib/
sudo cp include/*.h /usr/local/include/
sudo ldconfig
cd ..
```

Alternatively, build from source (takes longer on Pi):
```bash
git clone --recursive https://github.com/microsoft/onnxruntime
cd onnxruntime
./build.sh --config MinSizeRel --update --build --parallel --build_shared_lib
sudo cp -a build/Linux/MinSizeRel/libonnxruntime.so* /usr/local/lib/
sudo cp include/onnxruntime/core/session/*.h /usr/local/include/
sudo ldconfig
cd ..
```

#### espeak-ng (required for TTS - must use rhasspy fork)

```bash
# Remove apt version if installed (won't work with Piper)
sudo apt purge -y espeak-ng-data libespeak-ng1 2>/dev/null || true

# Install build dependencies
sudo apt install -y autoconf automake libtool

# Build rhasspy fork
git clone https://github.com/rhasspy/espeak-ng.git
cd espeak-ng
./autogen.sh && ./configure --prefix=/usr
make -j$(nproc) && sudo make LIBDIR=/usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH) install
cd ..
```

#### piper-phonemize (required for TTS)

```bash
git clone https://github.com/rhasspy/piper-phonemize.git
cd piper-phonemize
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

### 5. Clone Repository

```bash
git clone --recursive https://github.com/The-OASIS-Project/dawn.git
cd dawn
```

> **Note:** The `--recursive` flag is important to fetch the whisper.cpp submodule (needed even if using Vosk, as the build system requires it).

### 5.1 Download Models

Use the setup script from the project root to download ASR models:

```bash
# Recommended: Vosk small model (~40MB, satellite default) + Whisper base
./setup_models.sh --vosk-small

# Or: Vosk small + Whisper tiny (best for Pi 4)
./setup_models.sh --vosk-small --whisper-model tiny-q5_1

# Or: Large Vosk model (~1.8GB, better accuracy) + Whisper base
./setup_models.sh --vosk

# See all options
./setup_models.sh --help
```

> **Vosk model recommendations for Pi 4:**
> - `vosk-model-small-en-us-0.15` (~40MB) - Recommended, fast and accurate for voice commands
> - `vosk-model-en-us-0.22` (~1.8GB) - Best accuracy, higher RAM usage

> **Whisper model recommendations for Pi 4** (only needed if `engine = "whisper"`):
> - `tiny.en-q5_1` (~30MB) - Fastest, but ~4s inference on Pi 4
> - `tiny.en` (~77MB) - Slightly more accurate, ~5s inference

#### VAD and TTS Models

The VAD and TTS models are already included in the repository:

| Model | Path | Purpose |
|-------|------|---------|
| Silero VAD | `models/silero_vad_16k_op15.onnx` | Voice activity detection |
| Alba voice | `models/en_GB-alba-medium.onnx` | TTS (Friday persona) |
| Northern English Male | `models/en_GB-northern_english_male-medium.onnx` | TTS (Jarvis persona) |

Configure the model paths in your `satellite.toml` (supports `~/` paths):

```toml
[vad]
model_path = "~/dawn/models/silero_vad_16k_op15.onnx"

[asr]
engine = "vosk"
model_path = "~/dawn/models/vosk-model-small-en-us-0.15"

[tts]
model_path = "~/dawn/models/en_GB-alba-medium.onnx"
config_path = "~/dawn/models/en_GB-alba-medium.onnx.json"
```

### 6. Build the Satellite

```bash
cd dawn_satellite
mkdir build && cd build
cmake ..
make -j$(nproc)
```

> The common library is built automatically as part of the satellite build process.

This builds with the default configuration: **Vosk ASR + Whisper ASR** (both engines, runtime selectable via config). The `engine` field in `satellite.toml` selects which one is used.

#### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `-DENABLE_DAP2=ON` | ON | WebSocket protocol for daemon communication |
| `-DENABLE_VAD=ON` | ON | Voice Activity Detection (Silero, requires ONNX Runtime) |
| `-DENABLE_VOSK_ASR=ON` | ON | Vosk ASR engine (streaming, near-instant) |
| `-DENABLE_WHISPER_ASR=OFF` | OFF | Whisper ASR engine (requires whisper.cpp submodule) |
| `-DENABLE_TTS=ON` | ON | Piper TTS (requires ONNX Runtime) |
| `-DENABLE_DISPLAY=OFF` | OFF | SPI display support via framebuffer |
| `-DENABLE_SDL_UI=OFF` | OFF | SDL2 touchscreen UI (orb + transcript + music player) |
| `-DENABLE_NEOPIXEL=ON` | ON | NeoPixel/WS2812 LED support via SPI |
| `-DCMAKE_BUILD_TYPE=Release` | Release | Optimization level |
| *(auto-detected)* | — | Music streaming via Opus (requires `libopus-dev`) |

Example: Add Whisper alongside Vosk (runtime switching via config):
```bash
cmake -DENABLE_WHISPER_ASR=ON ..
```

Example: Text-only mode (keyboard input, no mic/speaker — for debugging):
```bash
cmake -DENABLE_VAD=OFF -DENABLE_VOSK_ASR=OFF -DENABLE_TTS=OFF ..
```

Example with SDL2 touchscreen UI:
```bash
cmake -DENABLE_SDL_UI=ON ..
```

Example headless with no LEDs:
```bash
cmake -DENABLE_NEOPIXEL=OFF ..
```

### 7. Configure

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
port = 3000                 # WebUI port (where WebSocket runs)

[audio]
capture_device = "plughw:1,0"   # Your microphone (run 'arecord -l')
playback_device = "plughw:0,0" # Your speaker (run 'aplay -l')
```

### 8. Test Run

```bash
# Run manually to test
./dawn_satellite --config /etc/dawn/satellite.toml

# Expected output:
# [INFO] DAWN Satellite v0.1.0 starting...
# [INFO] Loading config from /etc/dawn/satellite.toml
# [INFO] ASR engine: vosk, model: models/vosk-model-small-en-us-0.15
# [INFO] Connecting to wss://192.168.1.100:3000
# [INFO] WebSocket connected
# [INFO] Registered as "Living Room" (location: living_room)
# [INFO] Ready - say "Hey Friday" to activate
```

### 9. Install as System Service

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

#### [general]
```toml
ai_name = "friday"           # Must match server dawn.toml
```

#### [identity]
```toml
uuid = ""                    # Auto-generated UUID on first run
name = "Kitchen Assistant"   # Display name in daemon logs/WebUI
location = "kitchen"         # Room for context-aware responses
```

#### [server]
```toml
host = "192.168.1.100"       # DAWN daemon IP or hostname
port = 3000                  # WebUI port
ssl = true                   # Use wss:// (requires daemon SSL config)
ssl_verify = true            # Verify SSL certs (set false for self-signed)
reconnect_delay_ms = 5000    # Reconnection backoff
max_reconnect_attempts = 0   # 0 = retry forever
```

#### [audio]
```toml
capture_device = "plughw:1,0"   # ALSA capture device
playback_device = "plughw:0,0"  # ALSA playback device
sample_rate = 16000             # Must be 16kHz
max_record_seconds = 30         # Safety timeout
```

#### [vad]
```toml
enabled = true
model_path = "models/silero_vad_16k_op15.onnx"
silence_duration_ms = 800    # Silence before end-of-speech
min_speech_ms = 250          # Minimum valid utterance
threshold = 0.5              # 0.0-1.0, higher = stricter
```

#### [wake_word]
```toml
enabled = true
word = "friday"              # Wake word (matches daemon ai_name)
sensitivity = 0.5            # 0.0-1.0, higher = more false positives
```

#### [asr]
```toml
engine = "vosk"              # "vosk" (streaming, default) or "whisper" (batch)
model_path = "models/vosk-model-small-en-us-0.15"  # Vosk model directory
language = "en"              # Language code (Whisper only)
n_threads = 4                # Processing threads (Whisper only)
max_audio_seconds = 15       # Max recording duration
```

#### [tts]
```toml
model_path = "models/en_GB-alba-medium.onnx"
config_path = "models/en_GB-alba-medium.onnx.json"
espeak_data = "/usr/share/espeak-ng-data"
length_scale = 0.85          # Speech speed (1.0 = normal, lower = faster)
```

#### [sdl_ui]
```toml
enabled = false              # Enable SDL2 orb + transcript + music UI
width = 1024                 # Display width (default: 1024)
height = 600                 # Display height (default: 600)
font_dir = "assets/fonts"    # Path to TTF font files
brightness = 255             # Display brightness 0-255 (persisted across restarts)
volume = 80                  # System volume 0-100 (persisted across restarts)
theme = "cyan"               # Color theme: cyan, purple, green, blue, terminal
```

#### [processing]
```toml
mode = "voice_activated"     # "voice_activated" or "text_only"
```

#### [logging]
```toml
level = "info"               # "error", "warning", "info", "debug"
use_syslog = false           # Log to syslog instead of stdout
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

For I2S microphone (INMP441) and DAC (MAX98357A), add to `/boot/firmware/config.txt`:

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

Works automatically. If needed, add to `/boot/firmware/config.txt`:

```ini
dtoverlay=vc4-kms-v3d
max_framebuffers=2
```

### HDMI Touchscreen (1024x600)

Add to `/boot/firmware/config.txt`:

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
| "Connection timeout" | Check network, firewall (port 3000 or configured port) |
| "WebSocket error" | Ensure daemon WebUI is enabled |

```bash
# Test connectivity
ping 192.168.1.100
curl -v http://192.168.1.100:3000/

# Test WebSocket (install wscat: npm install -g wscat)
wscat -c ws://192.168.1.100:3000
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

### ASR Issues

| Symptom | Solution |
|---------|----------|
| "Failed to load model" | Check model path in config, verify file exists |
| Empty transcriptions | Check microphone level, try recording a test WAV |
| "Unsupported engine type" | Rebuild with the correct ASR engine enabled |

```bash
# Verify Vosk model exists
ls -la models/vosk-model-small-en-us-0.15/

# Test microphone input
arecord -D plughw:1,0 -f S16_LE -r 16000 -c 1 -d 3 /tmp/test.wav
aplay /tmp/test.wav  # Should hear your voice clearly
```

### Display Issues

| Symptom | Solution |
|---------|----------|
| "No available video device" | Add user to `video` group, reboot |
| Black screen | Check HDMI connection, `/boot/firmware/config.txt` |
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
+------------------------------------------------------------------+
|                      DAWN Satellite (Tier 1)                      |
|                                                                   |
|  +-------------------------------------------------------------+ |
|  |                 Local Processing Pipeline                    | |
|  |                                                              | |
|  |  +--------+  +--------+  +----------+  +---------+          | |
|  |  | Audio  |->|  VAD   |->| Wake Word|->|   ASR   |          | |
|  |  |Capture |  |(Silero)|  |  (ASR)   |  |  (Vosk  |          | |
|  |  +--------+  +--------+  +----------+  |streaming)|         | |
|  |                                         +----+----+          | |
|  |                                              | text          | |
|  |  +--------+  +--------+                     v               | |
|  |  | Audio  |<-|  TTS   |<-----------[satellite_query]        | |
|  |  |Playback|  |(Piper) |                                     | |
|  |  +--------+  +--------+<-----------[satellite_response]     | |
|  |                                                              | |
|  +-------------------------------------------------------------+ |
|                              |                                    |
|                              | WebSocket (JSON, ~100 bytes)       |
|                              v                                    |
|  +-------------------------------------------------------------+ |
|  |                     ws_client.c                              | |
|  |  satellite_register -->                                      | |
|  |  satellite_query ------>        <-- satellite_response       | |
|  |                                 <-- stream_delta (streaming) | |
|  +-------------------------------------------------------------+ |
|                              |                                    |
|  +-------------------------------------------------------------+ |
|  |              SDL2 Touchscreen UI (Optional)                  | |
|  |                                                              | |
|  |  +----------+ +-------------+ +----------+ +-----------+    | |
|  |  |   Orb    | | Transcript  | |  Music   | |Screensaver|    | |
|  |  |Visualize | |  Display    | |  Panel   | |Clock+FFT  |    | |
|  |  +----------+ +-------------+ +----------+ +-----------+    | |
|  |                                                              | |
|  |  Settings: Brightness/Volume/Theme | 5 Themes + Dot Picker  | |
|  +-------------------------------------------------------------+ |
|                              |                                    |
|  +-------------------------------------------------------------+ |
|  |             music_playback.c + music_stream.c               | |
|  |  Opus decode -> SPSC ring buffer -> LWS-thread ALSA drain  | |
|  |  Goertzel FFT visualizer from live audio stream             | |
|  +-------------------------------------------------------------+ |
+------------------------------------------------------------------+
                               |
                               | WiFi (JSON control + Opus audio)
                               v
+------------------------------------------------------------------+
|                         DAWN Daemon                               |
|                                                                   |
|    LLM Processing | Tool Execution | Conversation History        |
|    Command Parser | MQTT Control   | Music / Opus Streaming      |
+------------------------------------------------------------------+
```

## Protocol Reference

### Message Types (WebSocket JSON)

| Type | Direction | Purpose |
|------|-----------|---------|
| `satellite_register` | Satellite -> Daemon | Initial registration |
| `satellite_register_ack` | Daemon -> Satellite | Registration confirmed |
| `satellite_query` | Satellite -> Daemon | User's transcribed text |
| `stream_start` | Daemon -> Satellite | Response streaming begins |
| `stream_delta` | Daemon -> Satellite | Partial response text |
| `stream_end` | Daemon -> Satellite | Response complete |
| `satellite_status` | Both | Health check, metrics |
| `music_subscribe` | Satellite -> Daemon | Subscribe to music state updates |
| `music_control` | Satellite -> Daemon | Transport commands (play/pause/stop/next/prev/seek) |
| `music_library` | Satellite -> Daemon | Browse library (artists/albums/tracks, paginated) |
| `music_queue` | Satellite -> Daemon | Queue operations (add/remove/clear) |
| `music_state` | Daemon -> Satellite | Playback state update (track, position, status) |
| `music_position` | Daemon -> Satellite | Periodic position update during playback |
| `music_queue_response` | Daemon -> Satellite | Current queue contents |
| `music_library_response` | Daemon -> Satellite | Library browse results (paginated) |
| `music_error` | Daemon -> Satellite | Music operation error |

### Example Flow

```
Satellite                              Daemon
    |                                     |
    |---- satellite_register ------------>|
    |     {uuid, name, location, tier:1}  |
    |                                     |
    |<--- satellite_register_ack ---------|
    |     {session_id, memory_enabled}    |
    |                                     |
    |  [User: "Hey Friday, lights on"]    |
    |  [Local: VAD -> Wake -> ASR (Vosk)] |
    |                                     |
    |---- satellite_query --------------->|
    |     {text: "lights on"}             |
    |                                     |
    |<--- stream_start -------------------|
    |<--- stream_delta -------------------|
    |     {delta: "I'll turn"}            |
    |  [TTS: "I'll turn"]                 |
    |<--- stream_delta -------------------|
    |     {delta: " on the lights."}      |
    |  [TTS: " on the lights."]           |
    |<--- stream_end ---------------------|
    |                                     |
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
python3 ../tests/test_satellite_protocol.py --host 192.168.1.100 --port 3000
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

The UI ships with 5 switchable themes, selectable from the settings panel dot picker. The active theme is persisted in `satellite.toml` under `[sdl_ui] theme = "cyan"`.

| Theme | Accent | Description |
|-------|--------|-------------|
| Cyan | `#2DD4BF` | Default — matches DAWN WebUI |
| Purple | `#A855F7` | Vibrant purple accent |
| Green | `#4ADE80` | Green accent (distinct from Listening state) |
| Blue | `#3B82F6` | Cool blue accent |
| Terminal | `#7FFF7F` | CRT-green with darker backgrounds and muted text |

Theme changes apply to all accent-colored elements (orb tint, music controls, progress bars, tab underlines, transcript labels) with a 200ms ease-out crossfade. State colors (listening/thinking/speaking/error) are fixed and never change with theme.

See `src/ui/ui_theme.h` for the theme API and `src/ui/ui_colors.h` for fixed state colors.

## License

GPLv3 or later. See LICENSE file in repository root.

## See Also

- [DAP2_DESIGN.md](../docs/DAP2_DESIGN.md) - Protocol specification
- [DAP2_SATELLITE.md](../docs/DAP2_SATELLITE.md) - Implementation details
- [DAWN WebUI](../www/) - Web interface (shares visual design)
