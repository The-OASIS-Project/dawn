# DAWN Server - Systemd Service Package

Production-ready systemd service for running the DAWN voice assistant daemon on Jetson (or other Linux systems with CUDA).

## Quick Start

```bash
# Build the daemon
cmake --preset debug
make -C build-debug -j$(nproc)

# Install as service
sudo ./services/dawn-server/install.sh

# Edit config (add your API keys)
sudo nano /usr/local/etc/dawn/secrets.toml
sudo nano /usr/local/etc/dawn/dawn.toml

# Restart with new config
sudo systemctl restart dawn-server
```

## Prerequisites

- NVIDIA Jetson (or Linux system with CUDA)
- Built `dawn` binary with all dependencies
- Models: Whisper ASR, Piper TTS voice, Silero VAD
- SSL certificates (for WebUI/satellite connections)
- MQTT broker (Mosquitto) if using home automation

---

## Installation

### Automatic Installation

```bash
sudo ./install.sh
```

The script will:
1. Create `dawn` system user with audio/video/render groups
2. Set up directory structure under `/var/lib/dawn/`
3. Copy binary, www/, models/, SSL certs, and config
4. Create secrets.toml symlink in WorkingDirectory
5. Install systemd service and logrotate
6. Enable and start the service

### Options

| Flag | Description |
|------|-------------|
| `--binary PATH` | Path to dawn binary |
| `--models-dir PATH` | Path to models directory |
| `--www-dir PATH` | Path to WebUI static files |
| `--ssl-dir PATH` | Path to SSL certificates |
| `--config PATH` | Path to dawn.toml |
| `--secrets PATH` | Path to secrets.toml |
| `--symlink-models` | Symlink models instead of copying |
| `--symlink-www` | Symlink www instead of copying |

### Examples

```bash
# Use symlinks for large directories (dev workflow)
sudo ./install.sh --symlink-models --symlink-www

# Specify paths explicitly
sudo ./install.sh --binary ./build/dawn --config ./dawn.toml --secrets ./secrets.toml
```

### Manual Installation

1. Create service user:
   ```bash
   sudo useradd --system --home-dir /var/lib/dawn \
     --no-create-home --shell /usr/sbin/nologin dawn
   sudo usermod -a -G audio,video,render dawn
   ```

2. Create directories:
   ```bash
   sudo mkdir -p /var/lib/dawn/{models,www,ssl}
   sudo mkdir -p /usr/local/etc/dawn
   sudo mkdir -p /var/log/dawn
   ```

3. Copy files:
   ```bash
   sudo cp build-debug/dawn /usr/local/bin/
   sudo cp -r www/* /var/lib/dawn/www/
   sudo cp -r models/* /var/lib/dawn/models/
   sudo cp -r ssl/* /var/lib/dawn/ssl/
   sudo cp dawn.toml /usr/local/etc/dawn/
   sudo cp secrets.toml /usr/local/etc/dawn/
   sudo ln -sf /usr/local/etc/dawn/secrets.toml /var/lib/dawn/secrets.toml
   sudo cp services/dawn-server/dawn-server.conf /usr/local/etc/dawn/
   ```

4. Set permissions:
   ```bash
   sudo chown -R dawn:dawn /var/lib/dawn /var/log/dawn
   sudo chmod 600 /usr/local/etc/dawn/secrets.toml
   sudo chown dawn:dawn /usr/local/etc/dawn/secrets.toml
   sudo chmod 600 /var/lib/dawn/ssl/*.key
   ```

5. Configure library path:
   ```bash
   sudo sh -c 'echo "/usr/local/lib" > /etc/ld.so.conf.d/dawn.conf'
   sudo ldconfig
   ```

6. Install service:
   ```bash
   sudo cp services/dawn-server/dawn-server.service /etc/systemd/system/
   sudo cp services/dawn-server/dawn-server-logrotate /etc/logrotate.d/dawn-server
   sudo systemctl daemon-reload
   sudo systemctl enable dawn-server
   sudo systemctl start dawn-server
   ```

---

## Configuration

### dawn.toml

Main configuration at `/usr/local/etc/dawn/dawn.toml`. Key sections:

```toml
[general]
ai_name = "friday"

[llm]
type = "cloud"        # "cloud" or "local"

[llm.cloud]
provider = "openai"   # "openai", "claude", "gemini" or "openrouter"
openai_models = ["gpt-6-luna", "gpt-6.1-terra"]   # models to choose from
openai_default_model_idx = 0                      # the one used by default

[webui]
enabled = true
bind_address = "0.0.0.0"
port = 3000
https = true
ssl_cert_path = "ssl/dawn-chain.crt"    # Relative to WorkingDirectory
ssl_key_path = "ssl/dawn.key"

[mqtt]
broker = "localhost"
port = 1883
```

### secrets.toml

API keys at `/usr/local/etc/dawn/secrets.toml` (mode 0600):

```toml
[secrets]
openai_api_key = "sk-..."
claude_api_key = "sk-ant-..."
```

A symlink at `/var/lib/dawn/secrets.toml` points here so the daemon's config search finds it.

### Environment Variables

Edit `/usr/local/etc/dawn/dawn-server.conf`:

```bash
LD_LIBRARY_PATH=/usr/local/lib:/usr/local/cuda/lib64:/usr/lib/aarch64-linux-gnu/tegra
CUDA_VISIBLE_DEVICES=0
HOME=/var/lib/dawn
```

---

## Service Management

```bash
# Status
sudo systemctl status dawn-server

# Logs
sudo tail -f /var/log/dawn/server.log
sudo journalctl -u dawn-server -f

# Restart (after config changes)
sudo systemctl restart dawn-server

# Stop
sudo systemctl stop dawn-server

# Disable (prevent start on boot)
sudo systemctl disable dawn-server
```

---

## Testing

```bash
# Check service is running
systemctl is-active dawn-server

# WebUI health check
curl -k https://localhost:3000/health

# Check MQTT connectivity
mosquitto_sub -t "dawn/#" -v

# Verify GPU access
sudo -u dawn nvidia-smi
```

---

## Troubleshooting

### Service Won't Start

```bash
# Check detailed logs
journalctl -u dawn-server -n 50

# Verify binary
/usr/local/bin/dawn --help

# Check library dependencies
ldd /usr/local/bin/dawn | grep "not found"

# Verify CUDA
sudo -u dawn nvidia-smi
```

### CUDA / GPU Issues

```bash
# Check GPU access groups
groups dawn

# Verify CUDA libraries
ls -la /usr/local/cuda/lib64/libcuda*
ls -la /usr/lib/aarch64-linux-gnu/tegra/

# Out of memory - reboot to clear GPU fragmentation
sudo reboot
```

### Secrets Not Found

The daemon searches for `secrets.toml` in the working directory first. Verify the symlink:

```bash
ls -la /var/lib/dawn/secrets.toml
# Should show: secrets.toml -> /usr/local/etc/dawn/secrets.toml
```

### WebUI Not Accessible

```bash
# Check port binding
ss -tlnp | grep 3000

# Verify www/ files exist
ls /var/lib/dawn/www/

# Check SSL certificates
ls -la /var/lib/dawn/ssl/
openssl x509 -in /var/lib/dawn/ssl/dawn.crt -noout -dates
```

### Satellites Can't Connect

```bash
# Verify WebSocket port is open
curl -k https://<server-ip>:3000/health

# Check firewall
sudo ufw status
sudo iptables -L -n | grep 3000
```

---

## Files

| File | Installed Location | Purpose |
|------|-------------------|---------|
| `dawn-server.service` | `/etc/systemd/system/` | Systemd service unit |
| `dawn-server.conf` | `/usr/local/etc/dawn/` | Environment variables |
| `dawn-server-logrotate` | `/etc/logrotate.d/dawn-server` | Log rotation config |
| `dawn.toml` | `/usr/local/etc/dawn/` | Main configuration |
| `secrets.toml` | `/usr/local/etc/dawn/` (mode 0600) | API keys and credentials |
| Binary | `/usr/local/bin/dawn` | Server executable |
| WebUI | `/var/lib/dawn/www/` | Static web files |
| Models | `/var/lib/dawn/models/` | ASR, TTS, VAD models |
| SSL | `/var/lib/dawn/ssl/` | TLS certificates |
| Logs | `/var/log/dawn/` | Service logs |
