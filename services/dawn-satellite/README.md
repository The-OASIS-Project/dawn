# DAWN Satellite - Systemd Service Package

Production-ready systemd service for running `dawn_satellite` on Raspberry Pi.

## Quick Start

```bash
# Build the satellite (on RPi)
cd dawn_satellite && mkdir -p build && cd build
cmake ..   # default: VAD + Vosk ASR + Piper TTS
make -j$(nproc)
cd ../..

# Install as service
sudo ./services/dawn-satellite/install.sh

# Edit config
sudo nano /usr/local/etc/dawn-satellite/satellite.toml

# Restart with new config
sudo systemctl restart dawn-satellite
```

## Prerequisites

- Raspberry Pi 4 or newer
- Raspbian/Raspberry Pi OS (64-bit recommended)
- Built `dawn_satellite` binary
- Models: Vosk ASR model, Piper TTS voice, Silero VAD
- Audio hardware (USB sound card or onboard)

---

## Installation

### Automatic Installation

```bash
sudo ./install.sh
```

The script will:
1. Create `dawn` system user with audio group membership
2. Set up directory structure under `/var/lib/dawn-satellite/`
3. Copy binary, models, fonts, and config
4. Install systemd service and logrotate
5. Enable and start the service

### Options

| Flag | Description |
|------|-------------|
| `--binary PATH` | Path to dawn_satellite binary |
| `--models-dir PATH` | Path to models directory |
| `--fonts-dir PATH` | Path to fonts directory |
| `--symlink-models` | Symlink models instead of copying (saves disk) |
| `--no-display` | Headless mode: skip video/render/input groups |

### Manual Installation

1. Create service user:
   ```bash
   sudo useradd --system --home-dir /var/lib/dawn-satellite \
     --no-create-home --shell /usr/sbin/nologin dawn
   sudo usermod -a -G audio,video,render,input dawn
   ```

2. Create directories:
   ```bash
   sudo mkdir -p /var/lib/dawn-satellite/{models,assets/fonts}
   sudo mkdir -p /usr/local/etc/dawn-satellite
   sudo mkdir -p /var/log/dawn-satellite
   ```

3. Copy files:
   ```bash
   sudo cp dawn_satellite/build/dawn_satellite /usr/local/bin/
   sudo cp -r dawn_satellite/models/* /var/lib/dawn-satellite/models/
   sudo cp -r dawn_satellite/assets/fonts/* /var/lib/dawn-satellite/assets/fonts/
   sudo cp services/dawn-satellite/satellite.toml /usr/local/etc/dawn-satellite/
   sudo cp services/dawn-satellite/dawn-satellite.conf /usr/local/etc/dawn-satellite/
   ```

4. Set permissions:
   ```bash
   sudo chown -R dawn:dawn /var/lib/dawn-satellite /var/log/dawn-satellite
   sudo chown dawn:dawn /usr/local/etc/dawn-satellite/satellite.toml
   sudo chmod 664 /usr/local/etc/dawn-satellite/satellite.toml
   ```

5. Configure library path:
   ```bash
   sudo sh -c 'echo "/usr/local/lib" > /etc/ld.so.conf.d/dawn.conf'
   sudo ldconfig
   ```

6. Install service:
   ```bash
   sudo cp services/dawn-satellite/dawn-satellite.service /etc/systemd/system/
   sudo cp services/dawn-satellite/dawn-satellite-logrotate /etc/logrotate.d/dawn-satellite
   sudo systemctl daemon-reload
   sudo systemctl enable dawn-satellite
   sudo systemctl start dawn-satellite
   ```

---

## Configuration

Edit `/usr/local/etc/dawn-satellite/satellite.toml`. Essential settings:

### Server Connection
```toml
[server]
host = "192.168.1.100"    # DAWN daemon IP
port = 3000               # WebUI/WebSocket port
ssl = true                # Use wss://
registration_key = ""     # Pre-shared key (if daemon requires it)
```

### Identity
```toml
[identity]
name = "Kitchen"          # Display name in daemon logs/WebUI
location = "kitchen"      # Room for context-aware responses
```

### Audio Devices
```toml
[audio]
capture_device = "plughw:1,0"   # Use 'arecord -l' to list
playback_device = "plughw:1,0"  # Use 'aplay -l' to list
```

### Environment Variables

Edit `/usr/local/etc/dawn-satellite/dawn-satellite.conf` for environment overrides:

```bash
# Enable SDL display (uncomment for touchscreen)
SDL_VIDEODRIVER=KMSDRM
```

---

## Service Management

```bash
# Status
sudo systemctl status dawn-satellite

# Logs
sudo tail -f /var/log/dawn-satellite/satellite.log
sudo journalctl -u dawn-satellite -f

# Restart (after config changes)
sudo systemctl restart dawn-satellite

# Stop
sudo systemctl stop dawn-satellite

# Disable (prevent start on boot)
sudo systemctl disable dawn-satellite
```

---

## Testing

```bash
# Check service is running
systemctl is-active dawn-satellite

# Check satellite registered with daemon
# Look for registration message in log
grep "Registered" /var/log/dawn-satellite/satellite.log

# Test audio devices
sudo -u dawn arecord -D plughw:1,0 -f S16_LE -r 16000 -c 1 -d 3 /tmp/test.wav
sudo -u dawn aplay -D plughw:1,0 /tmp/test.wav
```

---

## Troubleshooting

### Service Won't Start

```bash
# Check detailed logs
journalctl -u dawn-satellite -n 50

# Verify binary exists and runs
/usr/local/bin/dawn_satellite --help

# Check library dependencies
ldd /usr/local/bin/dawn_satellite | grep "not found"
```

### No Audio

```bash
# List available devices
arecord -l
aplay -l

# Check dawn user can access audio
sudo -u dawn arecord -D plughw:0,0 -f S16_LE -r 16000 -c 1 -d 1 /dev/null

# Verify group membership
groups dawn
```

### Cannot Connect to Daemon

```bash
# Test network connectivity
ping <daemon-ip>
curl -k https://<daemon-ip>:3000/health

# Check SSL CA cert if ssl_verify = true
openssl s_client -connect <daemon-ip>:3000 -CAfile /path/to/ca.crt
```

### Display Issues (SDL UI)

```bash
# Uncomment SDL_VIDEODRIVER in environment config
sudo nano /usr/local/etc/dawn-satellite/dawn-satellite.conf

# Verify user has video/render/input groups
groups dawn

# Test SDL directly
sudo -u dawn SDL_VIDEODRIVER=KMSDRM /usr/local/bin/dawn_satellite --config /usr/local/etc/dawn-satellite/satellite.toml
```

---

## Files

| File | Installed Location | Purpose |
|------|-------------------|---------|
| `dawn-satellite.service` | `/etc/systemd/system/` | Systemd service unit |
| `dawn-satellite.conf` | `/usr/local/etc/dawn-satellite/` | Environment variables |
| `dawn-satellite-logrotate` | `/etc/logrotate.d/dawn-satellite` | Log rotation config |
| `satellite.toml` | `/usr/local/etc/dawn-satellite/` | Satellite configuration |
| Binary | `/usr/local/bin/dawn_satellite` | Satellite executable |
| Models | `/var/lib/dawn-satellite/models/` | ASR, TTS, VAD models |
| Fonts | `/var/lib/dawn-satellite/assets/fonts/` | TTF fonts for SDL UI |
| Identity | `/var/lib/dawn-satellite/.dawn_satellite_identity` | Auto-created at runtime |
| Logs | `/var/log/dawn-satellite/` | Service logs |
