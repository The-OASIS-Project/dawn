---
name: install
description: Guide a user through installing, configuring, and verifying the DAWN voice assistant. Covers system dependencies, core libraries, building, models, configuration, admin account, SSL, and optional features.
disable-model-invocation: true
user-invocable: true
allowed-tools: Read, Grep, Glob, Bash, Write, Edit, AskUserQuestion, Agent
argument-hint: "[fresh|verify|deploy|uninstall|step-name]"
---

# DAWN Installation Skill

You are guiding the user through a complete installation of the DAWN voice assistant. Your job is to execute each phase, verify it succeeded, and move on. Be methodical and thorough.

**Automated script alternative**: If `scripts/install.sh` exists, you may run it with appropriate flags
instead of executing phases manually. The script handles platform detection, sudo, and all phases.
Use it when the user wants a streamlined install. Fall back to manual phases if the script fails or
for troubleshooting. Deploy commands: `./scripts/install.sh --deploy server` or `--deploy satellite`.

## Invocation Modes

- `/install` or `/install fresh` — Full installation from scratch
- `/install verify` — Skip installation, just run the verification suite against an existing install
- `/install deploy server` — Install the built daemon as a systemd service (runs `services/dawn-server/install.sh`)
- `/install deploy satellite` — Install the built satellite as a systemd service (runs `services/dawn-satellite/install.sh`)
- `/install uninstall server` — Uninstall the daemon systemd service (runs `services/dawn-server/install.sh --uninstall`)
- `/install uninstall satellite` — Uninstall the satellite systemd service (runs `services/dawn-satellite/install.sh --uninstall`)
- `/install <step>` — Resume from a specific step (e.g., `/install ssl`, `/install configure`)

## Ground Rules

1. **Auto-detect everything you can** — platform (`uname -m`, Jetson vs RPi vs x86, CUDA presence), already-installed packages (`dpkg -l`, `pkg-config`), existing config files, existing builds, existing models. Never ask the user something you can probe.
2. **Run all non-sudo commands directly.** For `sudo` commands, print the exact command and ask the user to run it — UNLESS the user says sudo doesn't require a password, in which case run them directly too.
3. **Ask the user early** about choices that require intent (see Phase 0 below).
4. **Verify aggressively** after every phase. Don't move on until the phase passes verification.
5. **If something fails**, diagnose it. Read error output, check logs, suggest fixes. Don't just say "it failed".
6. **Reference the project docs** for details. The key files are:
   - `GETTING_STARTED.md` — primary install guide
   - `docs/GETTING_STARTED_SERVER.md` — x86_64 server-mode + Docker install
   - `dawn.toml.example` — full config reference
   - `secrets.toml.example` — API keys template
   - `docs/DAP2_SATELLITE.md` — satellite setup
   - `docs/HOMEASSISTANT_SETUP.md` — Home Assistant integration
   - `docs/MESSAGING_CHANNELS_SETUP.md` — Telegram / Slack / Discord / SMS
   - `docs/PHONE_SMS_DESIGN.md` — phone calls & SMS via the ECHO modem daemon
   - `docs/CODING_PROJECTS.md` — Code Projects (coding harness) + cbm server
   - `docs/OTA_DESIGN.md` — satellite over-the-air updates
   - `generate_ssl_cert.sh` — SSL certificate generation

## Phase 0: Discovery and User Preferences

### Auto-detect (do NOT ask):
- Architecture: `uname -m` (aarch64 = ARM64, x86_64 = x86)
- Platform: Check for `/etc/nv_tegra_release` (Jetson), `/sys/firmware/devicetree/base/model` (RPi), or generic Linux
- CUDA: `ls /usr/local/cuda/include/cuda.h`, `nvcc --version`
- Already installed deps: `dpkg -l | grep <package>` for each system dependency
- Already built libs: Check `/usr/local/lib/` for `libonnxruntime.so`, `libpiper_phonemize.so`, `libespeak-ng.so`
- Existing repo clone: Check if we're in a dawn repo already (`git remote -v`)
- Existing config: Check for `dawn.toml`, `secrets.toml`
- Existing build: Check for `build/dawn` or `build-debug/dawn`
- Existing models: Check `models/whisper.cpp/` for `.bin` files
- CMake version: `cmake --version`
- Audio backend: Check for PulseAudio (`pactl info`) and ALSA (`arecord -L`)
- Audio capture devices: Check `arecord -l` for physical capture devices. If none found, flag as headless (will need snd-dummy workaround or server mode)
- Docker: `docker --version` and `docker compose version`
- Passwordless sudo: `sudo -n true 2>/dev/null`

### Ask the user:
Present these as a checklist with defaults. Let the user answer all at once.

1. **What are you installing?**
   - [ ] DAWN server daemon (default: yes)
   - [ ] Tier 1 satellite (Raspberry Pi) (default: no)

2. **LLM provider?**
   - [ ] OpenAI (cloud)
   - [ ] Claude / Anthropic (cloud)
   - [ ] Google Gemini (cloud)
   - [ ] Local LLM (llama.cpp or Ollama)
   - (Can select multiple — first selected becomes default)

3. **API keys** — For each selected cloud provider, ask for the API key now. They can also add them later.

4. **Whisper model size?**
   - tiny (~75MB, fastest, least accurate)
   - base (~142MB, default, good balance)
   - small (~466MB, slower, more accurate)

5. **Optional features** — enable/disable:
   - [ ] SSL/TLS for HTTPS (default: yes if WebUI enabled)
   - [ ] SearXNG web search (default: no, requires Docker)
   - [ ] Tavily commercial search + URL extract (default: no, key-only — alternative to SearXNG/FlareSolverr)
   - [ ] Plex music integration (default: no)
   - [ ] Home Assistant smart home (default: no)
   - [ ] FlareSolverr for JS-heavy sites (default: no, requires Docker)
   - [ ] Email (IMAP/SMTP + Gmail OAuth) (default: no, needs `-DDAWN_ENABLE_EMAIL_TOOL=ON`)
   - [ ] Messaging channels — Telegram / Slack / Discord / SMS (default: no, per-provider bot tokens)
   - [ ] Phone & SMS via ECHO modem daemon (default: no, needs ECHO + `[phone] enabled`)
   - [ ] OpenRouter provider — one key fronts any cloud model (default: no)
   - [ ] MQTT integration (default: yes)

   For the key-only integrations (Tavily, OpenRouter, messaging bot tokens), collect the key/token
   now if selected and write it to `secrets.toml` in Phase 5. Point the user at the matching setup
   section in `GETTING_STARTED.md` / `docs/MESSAGING_CHANNELS_SETUP.md` / `docs/PHONE_SMS_DESIGN.md`
   for the ones that need external service setup.

6. **Build preset?**
   - `default` — Release with WebUI (recommended)
   - `local` — Local microphone only, no WebUI
   - `debug` — Debug symbols, for development

## Phase 1: System Dependencies

Reference: `GETTING_STARTED.md` section 1.

The full package list is:
```
build-essential cmake git pkg-config wget unzip autoconf automake libtool python3-pip
libasound2-dev libpulse-dev libsndfile1-dev libflac-dev
libmosquitto-dev mosquitto mosquitto-clients
libjson-c-dev libcurl4-openssl-dev libssl-dev
libwebsockets-dev libopus-dev libsodium-dev libsqlite3-dev
libsamplerate0-dev libmpg123-dev libvorbis-dev libncurses-dev
meson ninja-build libabsl-dev
libmupdf-dev libfreetype-dev libharfbuzz-dev
libzip-dev libmujs-dev libgumbo-dev libopenjp2-7-dev libjbig2dec0-dev
libical-dev libspdlog-dev libxml2-dev libstemmer-dev
```

**Known package name variations:**
- `libabsl-dev` (Ubuntu 22.04 / Jetson Linux R36) vs `libabseil-dev` (Ubuntu 24.04+). If one isn't found, try the other via `apt-cache search abseil`.

**Required-since notes:**
- `libstemmer-dev` — required since May 2026; the memory BM25 index (`memory_stem.c`) links against it unconditionally, so the build fails without it.
- **libgit2 ≥ 1.6** is needed only for the Code Projects feature (`DAWN_ENABLE_CODE_PROJECTS`, enabled by the `default`/`full`/`debug` presets). Jammy/Noble apt ship too old a version, so `scripts/install.sh` builds it from source (skip with `INSTALL_LIBGIT2=false` for the `server`/`local`/`ci` presets). Not an apt package.

1. Check which packages from the apt install list are already installed
2. Report what's missing
3. **Before generating the install command**, verify each missing package exists in apt-cache. Flag any that aren't found and search for alternatives.
4. Generate the `sudo apt install` command for only the missing packages
5. Run it (or ask user to run it if sudo requires password)
6. **Verify**: Confirm all packages are installed with `dpkg -l`

### CMake Version Check

After apt install, check `cmake --version`. DAWN presets need 3.21+, but ONNX Runtime (built from source) needs 3.28+. Debian 13+ and Ubuntu 24.04+ ship recent enough versions. If the system CMake is too old (Ubuntu 22.04 ships 3.22):

```bash
# aarch64 (Jetson, RPi)
wget https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-aarch64.tar.gz
tar xzf cmake-3.31.6-linux-aarch64.tar.gz
sudo cp -r cmake-3.31.6-linux-aarch64/bin/* /usr/local/bin/
sudo cp -r cmake-3.31.6-linux-aarch64/share/* /usr/local/share/

# x86_64
wget https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-x86_64.tar.gz
# ... same pattern
```

### Meson Version Check

WebRTC AEC requires Meson 0.63+. Ubuntu 22.04 ships 0.61. If too old:
```bash
pip3 install --user meson --upgrade
```
Then use `~/.local/bin/meson` or ensure `~/.local/bin` is in PATH.

## Phase 2: Core Libraries

Reference: `GETTING_STARTED.md` section 2.

For each of the 4 required libraries, check if already installed before building:

### 2a. spdlog
- Check: `dpkg -l libspdlog-dev`
- Install: `sudo apt install -y libspdlog-dev`

### 2b. espeak-ng (rhasspy fork)
- Check: `pkg-config --modversion espeak-ng` and verify it's the rhasspy fork (check for `/usr/lib/*/libespeak-ng.so`)
- Build: Clone rhasspy/espeak-ng, autogen, configure, make, sudo make install

### 2c. ONNX Runtime
- Check: `ls /usr/local/lib/libonnxruntime.so*` and `ls /usr/local/include/onnxruntime_c_api.h`
- **Pre-built packages (recommended for RPi and x86_64)**:
  - **aarch64** (RPi 5, non-CUDA ARM boards):
    ```bash
    wget https://github.com/microsoft/onnxruntime/releases/download/v1.19.2/onnxruntime-linux-aarch64-1.19.2.tgz
    tar xzf onnxruntime-linux-aarch64-1.19.2.tgz
    sudo cp -a onnxruntime-linux-aarch64-1.19.2/lib/libonnxruntime*.so* /usr/local/lib/
    sudo cp onnxruntime-linux-aarch64-1.19.2/include/*.h /usr/local/include/
    sudo ldconfig
    ```
  - **x86_64**: See `docs/GETTING_STARTED_SERVER.md` section 2
- **From source** (any system with CUDA — Jetson, x86_64 with NVIDIA GPU):
  - **CRITICAL**: Clone with `--branch v1.19.2 --depth 1` (NOT main). The `main` branch pulls abseil lts_20250814 which requires GCC 12+, breaking on Ubuntu 22.04 (GCC 11).
  - **GCC 14+ workaround**: v1.19.2 fails with `-Werror=template-id-cdtor` on GCC 14+. Install `gcc-12 g++-12` and build with `CC=gcc-12 CXX=g++-12`. The `scripts/install.sh` handles this automatically.
  - Build with CUDA flags: `CC=gcc-12 CXX=g++-12 ./build.sh --use_cuda --cudnn_home /usr/local/cuda --cuda_home /usr/local/cuda --config MinSizeRel --update --build --parallel --build_shared_lib`
  - **Known issue**: Eigen download from GitLab may fail (hash mismatch or 403). Workaround: download Eigen manually and pass `--cmake_extra_defines FETCHCONTENT_SOURCE_DIR_EIGEN=/path/to/eigen-source`
  - This is the longest build — warn the user it may take 30-60 minutes

### 2d. piper-phonemize
- Check: `ls /usr/local/lib/libpiper_phonemize.so*`
- Build: Clone rhasspy/piper-phonemize, cmake, make, manual install

**Verify each**: Check that `.so` files exist in `/usr/local/lib/` and headers are in place.

## Phase 3: Clone and Build

Reference: `GETTING_STARTED.md` section 3.

1. If not already in a dawn repo, clone with `--recursive`
2. If in repo but submodules missing, run `git submodule update --init --recursive`
3. Build WebRTC audio processing:
   - **Must use `--default-library=static`** — CMakeLists.txt expects `.a` files, not `.so`
   - Check Meson version first — needs 0.63+ (upgrade via pip if needed)
   - Command: `meson setup build --default-library=static && ninja -C build`
   - If build fails, note that AEC can be disabled with `-DENABLE_AEC=OFF`
4. Run `cmake --preset <selected-preset>`
   - If CMake fails with missing `freetype2` or `harfbuzz`, install `libfreetype-dev libharfbuzz-dev`
5. Run `cmake --build --preset <selected-preset>` (or `make -C <build-dir> -j$(nproc)`)

**Verify**:
- Binary exists: `ls -la build/dawn` (path depends on preset)
- Binary runs: `LD_LIBRARY_PATH=/usr/local/lib ./build/dawn --help` or `--dump-config` (should print config and exit).
  **Note**: `LD_LIBRARY_PATH=/usr/local/lib` is needed when core libraries (ONNX Runtime, espeak-ng, piper-phonemize) are installed to `/usr/local/lib/`. Use it for all `./build/dawn` and `./build/dawn-admin` invocations.
- `dawn-admin` exists: `ls -la build/dawn-admin`

## Phase 4: Download Models

Reference: `GETTING_STARTED.md` section 4.

1. Run `./setup_models.sh --whisper-model <selected-size>`
2. If user selected Vosk, add `--vosk`

**Verify**:
- Whisper model exists: `ls models/whisper.cpp/ggml-*.bin`
- TTS models exist: `ls models/*.onnx`
- VAD model exists: `ls models/silero_vad.onnx`

## Phase 5: Configure

Reference: `GETTING_STARTED.md` section 5.

1. Copy `dawn.toml.example` to `dawn.toml` (if not exists)
2. Copy `secrets.toml.example` to `secrets.toml` (if not exists)
3. Set file permissions: `chmod 600 secrets.toml`
4. Write API keys the user provided in Phase 0 into `secrets.toml`
5. Configure `dawn.toml` based on user's choices:
   - Set `[llm] type` based on cloud vs local selection
   - Set `[llm.cloud] provider` to their primary cloud provider
   - Set `[webui] enabled = true` if not using `local` preset
   - Set audio devices if auto-detected
   - **Headless (no capture device detected)**: Load `snd-dummy` kernel module, persist via
     `/etc/modules-load.d/snd-dummy.conf`, and set `capture_device = "plughw:CARD=Dummy,DEV=0"`.
     This allows the daemon to start; voice input works via WebUI and satellites.
   - Enable/disable MQTT based on selection
6. If Plex was selected, ask for Plex token and configure `[music.plex]`
7. If Home Assistant was selected, ask for HA token and configure

**Verify**:
- `dawn.toml` exists and parses: `./build/dawn --dump-config` (should show effective config)
- `secrets.toml` exists with correct permissions: `stat -c '%a' secrets.toml` should be `600`
- API keys are set (check they're not placeholder values)

## Phase 6: API Key Validation

For each configured API key, make a minimal test call:

### OpenAI
```bash
curl -s -o /dev/null -w "%{http_code}" https://api.openai.com/v1/models \
  -H "Authorization: Bearer <key>"
```
Expected: `200`

### Claude
```bash
curl -s -o /dev/null -w "%{http_code}" https://api.anthropic.com/v1/messages \
  -H "x-api-key: <key>" \
  -H "anthropic-version: 2023-06-01" \
  -H "content-type: application/json" \
  -d '{"model":"claude-haiku-4-5-20251001","max_tokens":1,"messages":[{"role":"user","content":"hi"}]}'
```
Expected: `200`

### Gemini
```bash
curl -s -o /dev/null -w "%{http_code}" \
  "https://generativelanguage.googleapis.com/v1beta/models?key=<key>"
```
Expected: `200`

### Local LLM
```bash
curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8080/v1/models
```
Expected: `200` (if server is running)

Report results: which keys are valid, which failed, and what the error means.

## Phase 7: SSL Setup (if selected)

Reference: `GETTING_STARTED.md` section 7.

1. Run `./generate_ssl_cert.sh` (interactive — prompts for CA passphrase)
2. Update `dawn.toml` with SSL paths
3. Optionally generate satellite registration key: `./generate_ssl_cert.sh --gen-key`

**Verify**:
- Certificate files exist: `ls ssl/ca.crt ssl/dawn.crt ssl/dawn.key ssl/dawn-chain.crt`
- Certificate is valid: `openssl x509 -in ssl/dawn.crt -noout -dates -subject`
- dawn.toml has correct SSL paths

## Phase 8: Create Admin Account

Reference: `GETTING_STARTED.md` section 6.

The admin creation requires the daemon running (it generates a setup token). Use environment
variables for fully non-interactive creation:

1. Start DAWN in the background, capturing all output (the token appears in the combined
   stdout/stderr stream, printed by the admin socket after full initialization):
   ```bash
   LD_LIBRARY_PATH=/usr/local/lib ./build/dawn > /tmp/dawn_startup.log 2>&1 &
   DAWN_PID=$!
   sleep 6
   TOKEN=$(grep -oP 'Token: \K[A-Z0-9-]+' /tmp/dawn_startup.log)
   ```
   If TOKEN is empty, check `/tmp/dawn_startup.log` for startup errors (audio device, missing
   model, etc.) and fix them before retrying.
2. Create the admin account using env vars (no interactive prompts):
   ```bash
   ADMIN_PASS=$(tr -dc 'A-Za-z0-9' </dev/urandom | head -c 16)
   DAWN_SETUP_TOKEN="$TOKEN" DAWN_PASSWORD="$ADMIN_PASS" \
     LD_LIBRARY_PATH=/usr/local/lib ./build/dawn-admin user create admin --admin
   ```
3. Stop the background daemon: `kill $DAWN_PID`
4. Display the generated password prominently and warn the user to save it (it will not be shown again)

**Verify**:
- User database exists: `ls ~/.local/share/dawn/auth.db` (dev) or `/var/lib/dawn/db/auth.db` (service)
- Admin account exists (requires daemon running): `./build/dawn-admin user list`

## Phase 9: Optional Features Setup

### SearXNG (if selected)
Reference: `GETTING_STARTED.md` — SearXNG section.

1. Check Docker is available
2. Create directory structure and docker-compose.yml
3. Start container: `docker compose up -d`
4. **Verify**: `curl -s "http://localhost:8384/search?q=test&format=json" | head -c 200`

### FlareSolverr (if selected)
1. `docker run -d --name flaresolverr -p 8191:8191 ghcr.io/flaresolverr/flaresolverr:latest`
2. Update `dawn.toml` with FlareSolverr settings
3. **Verify**: `curl -s http://localhost:8191/health`

### Home Assistant (if selected)
1. Token should already be in `secrets.toml` from Phase 5
2. Configure HA URL in `dawn.toml`
3. **Verify**: Test HA API connection with curl

### Plex (if selected)
1. Token and config should already be set from Phase 5
2. **Verify**: Test Plex API connection with curl

### Tavily (if selected)
1. `tavily_api_key` (starts `tvly-`) should be in `secrets.toml` from Phase 5
2. Set `[search] engine = "tavily"` and/or `[url_fetcher] fallback = "tavily"` in `dawn.toml`
3. **Verify**: `curl -s -o /dev/null -w "%{http_code}" -H "Content-Type: application/json" -d '{"api_key":"<key>","query":"test"}' https://api.tavily.com/search` → `200`

### OpenRouter (if selected)
1. `openrouter_api_key` (starts `sk-or-`) should be in `secrets.toml` from Phase 5
2. Set `[llm.cloud] provider = "openrouter"` in `dawn.toml`
3. **Verify**: `curl -s -o /dev/null -w "%{http_code}" https://openrouter.ai/api/v1/models -H "Authorization: Bearer <key>"` → `200`

### Messaging channels (if selected)
1. Bot/app tokens should be in `secrets.toml` from Phase 5 (`telegram_bot_token`, `discord_bot_token`, `slack_app_token` + `slack_bot_token`; SMS routes through ECHO, no token). Drivers load only when their token is present.
2. After first run, link a channel: `./build/dawn-admin messaging generate-link-code --user <username>`, then send `/link CODE` from the chat app (Slack: `link CODE`).
3. Full per-provider bot setup: `docs/MESSAGING_CHANNELS_SETUP.md`.

### Email (if selected)
1. Requires a build with `-DDAWN_ENABLE_EMAIL_TOOL=ON` and `[email] enabled = true` in `dawn.toml`.
2. Accounts are added in the WebUI (**Settings → Email Accounts**), not the installer — app password or Google OAuth.
3. Reference: `GETTING_STARTED.md` — Email section.

### Phone & SMS via ECHO (if selected)
1. ECHO is a separate daemon (`~/code/The-OASIS-Project/echo`) that owns the modem; it must share DAWN's MQTT broker.
2. Set `[phone] enabled = true` (+ `user_id`, optional `audio_device`) in `dawn.toml`.
3. Reference: `docs/PHONE_SMS_DESIGN.md`. Note: two-way call audio through the browser is not yet shipped.

## Phase 10: Deploy as Systemd Service (optional)

This phase is only run when explicitly requested via `/install deploy` or when the user asks
to deploy during installation. It installs the built binary as a production systemd service.

Both scripts require root. They are interactive (ask yes/no questions about config removal, etc.),
so guide the user through running them manually or run with sudo if passwordless.

### Deploy Server

Script: `services/dawn-server/install.sh`

The script auto-discovers the binary, models, www, ssl, and config files from the project tree.
Override paths with flags if needed.

```bash
sudo ./services/dawn-server/install.sh [options]
```

Key options:
- `--binary PATH` — Path to dawn binary (default: auto-search build dirs)
- `--models-dir PATH` — Models directory (default: `models/`)
- `--www-dir PATH` — WebUI static files (default: `www/`)
- `--ssl-dir PATH` — SSL certificates (default: `ssl/`)
- `--config PATH` — dawn.toml path (default: `dawn.toml`)
- `--secrets PATH` — secrets.toml path (default: `secrets.toml`)
- `--symlink-models` — Symlink models instead of copying (saves disk, requires source to stay)
- `--symlink-www` — Symlink www instead of copying (useful for development)

Installed paths:
- Binary: `/usr/local/bin/dawn`
- Config: `/usr/local/etc/dawn/dawn.toml`
- Secrets: `/usr/local/etc/dawn/secrets.toml` (mode 0600)
- Data: `/var/lib/dawn/` (models, www, ssl)
- Logs: `/var/log/dawn/`
- Service: `dawn-server.service`

**Verify** (run ALL of these post-deploy checks):
- `systemctl is-active dawn-server` should return `active`
- `systemctl is-enabled dawn-server` should return `enabled`
- Binary exists: `ls /usr/local/bin/dawn`
- Config exists: `ls /usr/local/etc/dawn/dawn.toml`
- Secrets permissions: `stat -c '%a' /usr/local/etc/dawn/secrets.toml` should be `600`
- Log directory: `ls /var/log/dawn/`
- ldconfig entry: `cat /etc/ld.so.conf.d/dawn.conf` should contain `/usr/local/lib`
- Clean logs: `grep -c "\[ERR\]" /var/log/dawn/server.log` should be 0
- WebUI responds: `curl -sk https://localhost:3000/ | head -c 100` (wait a few seconds after start)
- `journalctl -u dawn-server -n 20` shows clean startup

**CUDA note**: If CUDA was detected during install, `scripts/install.sh --deploy server` automatically
configures `/usr/local/etc/dawn/dawn-server.conf` with the correct GPU library paths and enables
`CUDA_VISIBLE_DEVICES`. For manual deploy, uncomment the CUDA lines in `dawn-server.conf`.

### Deploy Satellite

Script: `services/dawn-satellite/install.sh`

```bash
sudo ./services/dawn-satellite/install.sh [options]
```

Key options:
- `--binary PATH` — Path to dawn_satellite binary
- `--models-dir PATH` — Models directory
- `--fonts-dir PATH` — Fonts for SDL UI (default: `dawn_satellite/assets/fonts/`)
- `--symlink-models` — Symlink models instead of copying
- `--no-display` — Skip video/render/input groups (headless satellite)

Installed paths:
- Binary: `/usr/local/bin/dawn_satellite`
- Config: `/usr/local/etc/dawn-satellite/satellite.toml`
- Data: `/var/lib/dawn-satellite/` (models, fonts)
- Logs: `/var/log/dawn-satellite/`
- Service: `dawn-satellite.service`

**After deploy**, remind user to edit the config:
1. Set `[server] host` to the DAWN daemon IP
2. Set `[identity] name` and `location`
3. Configure `[audio]` capture/playback devices
4. Restart: `sudo systemctl restart dawn-satellite`

**Verify** (run ALL of these post-deploy checks):
- `systemctl is-active dawn-satellite` should return `active`
- `systemctl is-enabled dawn-satellite` should return `enabled`
- Binary exists: `ls /usr/local/bin/dawn_satellite`
- Config exists: `ls /usr/local/etc/dawn-satellite/satellite.toml`
- Log directory: `ls /var/log/dawn-satellite/`
- ldconfig entry: `cat /etc/ld.so.conf.d/dawn.conf` should contain `/usr/local/lib`
- Clean logs: `grep -c "\[ERR\]" /var/log/dawn-satellite/satellite.log` should be 0
- `journalctl -u dawn-satellite -n 20` shows clean startup

## Uninstall

When `/install uninstall server` or `/install uninstall satellite` is used, run the
corresponding script with `--uninstall`. These are interactive (prompt about database
and config removal), so always let the user run them or confirm each step.

### Uninstall Server
```bash
sudo ./services/dawn-server/install.sh --uninstall
```

What it removes:
1. Stops and disables `dawn-server.service`
2. Removes systemd unit file and logrotate config
3. Removes `/usr/local/bin/dawn`
4. Removes `/var/lib/dawn/` (prompts about database preservation)
5. Removes `/var/log/dawn/`
6. Prompts about `/usr/local/etc/dawn/` config removal
7. Removes `/etc/ld.so.conf.d/dawn.conf` (only if satellite is not installed)
8. Does NOT remove the `dawn` system user (may be shared with satellite)

### Uninstall Satellite
```bash
sudo ./services/dawn-satellite/install.sh --uninstall
```

What it removes:
1. Stops and disables `dawn-satellite.service`
2. Removes systemd unit file and logrotate config
3. Removes `/usr/local/bin/dawn_satellite`
4. Removes `/var/lib/dawn-satellite/`
5. Removes `/var/log/dawn-satellite/`
6. Prompts about `/usr/local/etc/dawn-satellite/` config removal
7. Removes `/etc/ld.so.conf.d/dawn.conf` (only if server is not installed)
8. Does NOT remove the `dawn` system user

**After uninstall**, report what was removed and what was preserved (config, database, user).
Note that the source code, build artifacts, and core libraries (ONNX, espeak-ng, piper-phonemize)
are NOT removed by uninstall — those are separate from the systemd service installation.

## Phase 11: Final Verification Suite

Run ALL of these checks and present a summary table:

| Check | Status | Details |
|-------|--------|---------|
| Binary exists | | Path and size |
| Binary runs | | Version string |
| dawn-admin exists | | Path |
| Config loads | | `--dump-config` exit code |
| Whisper model | | Model name and size |
| TTS models | | Voice model name |
| VAD model | | File exists |
| secrets.toml permissions | | Should be 600 |
| API key: OpenAI | | Valid / Invalid / Not configured |
| API key: Claude | | Valid / Invalid / Not configured |
| API key: Gemini | | Valid / Invalid / Not configured |
| Local LLM | | Reachable / Not configured |
| SSL certificates | | Valid / Not configured / Expired |
| WebUI port | | Start daemon briefly, check port responds |
| Audio capture | | Device detected |
| Audio playback | | Device detected |
| Admin account | | Exists |
| SearXNG | | Reachable / Not configured |
| FlareSolverr | | Reachable / Not configured |
| MQTT broker | | Reachable / Not configured |
| Home Assistant | | Reachable / Not configured |
| Plex | | Reachable / Not configured |
| Git submodules | | All initialized |

### Summary
At the end, print:
1. The verification table
2. Any warnings or issues that need attention
3. How to start DAWN: the exact command to run
4. How to access the WebUI: the URL with correct protocol (http/https) and port
5. Quick tips: wake word, first voice command to try

If `/install verify` was used, skip all installation phases and jump directly to Phase 11.

If a systemd service is deployed, also check service-specific paths:

| Check | Status | Details |
|-------|--------|---------|
| dawn-server.service | | active / inactive / not installed |
| dawn-satellite.service | | active / inactive / not installed |
| Service binary | | `/usr/local/bin/dawn` exists |
| Service config | | `/usr/local/etc/dawn/dawn.toml` exists |
| Service secrets | | `/usr/local/etc/dawn/secrets.toml` permissions |
| Service logs | | Recent entries clean |
