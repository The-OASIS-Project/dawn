#!/bin/bash
#
# DAWN Installation - Phase 1: System Dependencies
# apt packages, CMake version check, Meson version check, Docker detection
#
# Sourced by install.sh. Do not execute directly.
#

# ─────────────────────────────────────────────────────────────────────────────
# Package lists
# ─────────────────────────────────────────────────────────────────────────────

# Full apt package list from GETTING_STARTED.md
APT_PACKAGES=(
   # Build tools
   build-essential cmake git pkg-config wget unzip
   autoconf automake libtool python3-pip
   # Audio
   libasound2-dev libpulse-dev libsndfile1-dev libflac-dev
   # MQTT
   libmosquitto-dev mosquitto mosquitto-clients
   # Networking / crypto
   libjson-c-dev libcurl4-openssl-dev libssl-dev
   libwebsockets-dev libopus-dev libsodium-dev libsqlite3-dev
   # Audio codecs
   libsamplerate0-dev libmpg123-dev libvorbis-dev
   # TUI
   libncurses-dev
   # Build systems
   meson ninja-build
   # Document processing (PDF, DOCX)
   libmupdf-dev libfreetype-dev libharfbuzz-dev
   libzip-dev libmujs-dev libgumbo-dev libopenjp2-7-dev libjbig2dec0-dev
   libxml2-dev
   # Calendar
   libical-dev
   # Email: reading messages (MIME parts, charsets)
   libgmime-3.0-dev
   # Memory BM25 keyword index (Porter2 stemming) — hard build dep, links memory_stem.c
   libstemmer-dev
   # Logging
   libspdlog-dev
)

MISSING_PKGS=()
INSTALLED_PKGS=()

# ─────────────────────────────────────────────────────────────────────────────
# Package detection
# ─────────────────────────────────────────────────────────────────────────────

detect_missing_apt_packages() {
   MISSING_PKGS=()
   INSTALLED_PKGS=()

   for pkg in "${APT_PACKAGES[@]}"; do
      if is_pkg_installed "$pkg"; then
         INSTALLED_PKGS+=("$pkg")
      else
         MISSING_PKGS+=("$pkg")
      fi
   done

   # Handle abseil naming variation (libabsl-dev vs libabseil-dev)
   if ! is_pkg_installed libabsl-dev && ! is_pkg_installed libabseil-dev; then
      if apt-cache show libabseil-dev >/dev/null 2>&1; then
         MISSING_PKGS+=("libabseil-dev")
      elif apt-cache show libabsl-dev >/dev/null 2>&1; then
         MISSING_PKGS+=("libabsl-dev")
      else
         warn "Neither libabsl-dev nor libabseil-dev found in apt. Abseil may need manual install."
      fi
   fi

   log "${#INSTALLED_PKGS[@]} packages already installed, ${#MISSING_PKGS[@]} missing"
}

# ─────────────────────────────────────────────────────────────────────────────
# Install missing packages
# ─────────────────────────────────────────────────────────────────────────────

install_apt_packages() {
   detect_missing_apt_packages

   if [ ${#MISSING_PKGS[@]} -eq 0 ]; then
      log "All apt packages already installed"
      return 0
   fi

   # Verify each missing package exists in apt-cache
   local verified_pkgs=()
   local not_found=()
   for pkg in "${MISSING_PKGS[@]}"; do
      if apt-cache show "$pkg" >/dev/null 2>&1; then
         verified_pkgs+=("$pkg")
      else
         not_found+=("$pkg")
      fi
   done

   if [ ${#not_found[@]} -gt 0 ]; then
      warn "Packages not found in apt (may need alternative names): ${not_found[*]}"
   fi

   if [ ${#verified_pkgs[@]} -eq 0 ]; then
      log "No packages to install"
      return 0
   fi

   log "Installing ${#verified_pkgs[@]} packages..."
   sudo_begin_phase "deps"
   run_sudo apt-get update -qq || warn "apt-get update had warnings (continuing)"
   run_sudo apt-get install -y "${verified_pkgs[@]}" || error "apt-get install failed"

   # Verify installation
   local failed=()
   for pkg in "${verified_pkgs[@]}"; do
      if ! is_pkg_installed "$pkg"; then
         failed+=("$pkg")
      fi
   done
   if [ ${#failed[@]} -gt 0 ]; then
      error "Failed to install packages: ${failed[*]}"
   fi
   log "All packages installed successfully"
}

# ─────────────────────────────────────────────────────────────────────────────
# CMake version check
# ─────────────────────────────────────────────────────────────────────────────

check_cmake_version() {
   # Refresh version after apt install
   if has_command cmake; then
      CMAKE_VERSION=$(cmake --version 2>/dev/null | head -1 | grep -oP '[0-9]+\.[0-9]+\.[0-9]+' || echo "")
   fi

   if [ -z "$CMAKE_VERSION" ]; then
      error "CMake not found after apt install"
   fi

   # DAWN presets need 3.21+. ONNX Runtime from source needs 3.28+.
   local required_minor=21
   if [ "$PLATFORM" = "jetson" ] && [ "$HAS_ONNXRUNTIME" = false ]; then
      required_minor=28
   fi

   local major minor
   major=$(echo "$CMAKE_VERSION" | cut -d. -f1)
   minor=$(echo "$CMAKE_VERSION" | cut -d. -f2)

   if [ "$major" -lt 3 ] || { [ "$major" -eq 3 ] && [ "$minor" -lt "$required_minor" ]; }; then
      log "CMake $CMAKE_VERSION is too old (need 3.$required_minor+). Upgrading..."
      upgrade_cmake
   else
      log "CMake $CMAKE_VERSION meets requirements (need 3.$required_minor+)"
   fi
}

upgrade_cmake() {
   local cmake_ver="3.31.6"
   local tarball
   if [ "$ARCH" = "aarch64" ]; then
      tarball="cmake-${cmake_ver}-linux-aarch64.tar.gz"
   else
      tarball="cmake-${cmake_ver}-linux-x86_64.tar.gz"
   fi

   local url="https://github.com/Kitware/CMake/releases/download/v${cmake_ver}/${tarball}"
   local tmpdir
   tmpdir=$(mktemp -d)
   register_cleanup "$tmpdir"

   log "Downloading CMake $cmake_ver..."
   wget -q --show-progress -O "$tmpdir/$tarball" "$url" || error "Failed to download CMake"
   tar xzf "$tmpdir/$tarball" -C "$tmpdir"

   local cmake_dir="$tmpdir/${tarball%.tar.gz}"
   sudo_begin_phase "cmake-upgrade"
   run_sudo cp -r "$cmake_dir/bin/"* /usr/local/bin/
   run_sudo cp -r "$cmake_dir/share/"* /usr/local/share/

   # Update cached version
   CMAKE_VERSION=$(cmake --version 2>/dev/null | head -1 | grep -oP '[0-9]+\.[0-9]+\.[0-9]+' || echo "")
   log "CMake upgraded to $CMAKE_VERSION"
}

# ─────────────────────────────────────────────────────────────────────────────
# Meson version check
# ─────────────────────────────────────────────────────────────────────────────

check_meson_version() {
   # Refresh version after apt install
   if has_command meson; then
      MESON_VERSION=$(meson --version 2>/dev/null || echo "")
   fi

   if [ -z "$MESON_VERSION" ]; then
      warn "Meson not found after apt install"
      return 0
   fi

   local required="0.63"
   if [ "$(printf '%s\n' "$required" "$MESON_VERSION" | sort -V | head -1)" != "$required" ]; then
      log "Meson $MESON_VERSION is too old (need $required+). Upgrading via pip..."
      pip3 install --user meson --upgrade || error "Failed to upgrade meson via pip"
      export PATH="$HOME/.local/bin:$PATH"
      MESON_VERSION=$(meson --version 2>/dev/null || echo "")
      log "Meson upgraded to $MESON_VERSION"
   else
      log "Meson $MESON_VERSION meets requirements (need $required+)"
   fi
}

# ─────────────────────────────────────────────────────────────────────────────
# Docker install (if needed for selected features)
# ─────────────────────────────────────────────────────────────────────────────

install_docker() {
   log "Installing Docker Engine..."
   sudo_begin_phase "docker"

   # Official Docker install: https://docs.docker.com/engine/install/debian/
   # Works for Debian, Ubuntu, and Raspberry Pi OS (all Debian-based)
   run_sudo apt-get update -qq
   run_sudo apt-get install -y ca-certificates curl gnupg || error "Failed to install Docker prerequisites"

   # Add Docker's official GPG key
   run_sudo install -m 0755 -d /etc/apt/keyrings
   if [ ! -f /etc/apt/keyrings/docker.gpg ]; then
      curl -fsSL "https://download.docker.com/linux/$OS_ID/gpg" | \
         run_sudo gpg --dearmor -o /etc/apt/keyrings/docker.gpg
      run_sudo chmod a+r /etc/apt/keyrings/docker.gpg
   fi

   # Raspberry Pi OS reports as "raspbian" but Docker repo uses "debian"
   local docker_os_id="$OS_ID"
   if [ "$docker_os_id" = "raspbian" ]; then
      docker_os_id="debian"
   fi

   # Add Docker repository
   local arch_deb
   arch_deb=$(dpkg --print-architecture)
   local codename
   codename=$(. /etc/os-release && echo "${VERSION_CODENAME:-}")

   # Some rolling releases (e.g., Debian trixie) may not have a Docker repo yet.
   # Fall back to the latest stable codename.
   if [ -z "$codename" ] || \
      ! curl -fsSL "https://download.docker.com/linux/$docker_os_id/dists/$codename/Release" \
         >/dev/null 2>&1; then
      local fallback=""
      case "$docker_os_id" in
         debian) fallback="bookworm" ;;
         ubuntu) fallback="noble" ;;
      esac
      if [ -n "$fallback" ]; then
         warn "Docker repo not available for '$codename', using '$fallback'"
         codename="$fallback"
      fi
   fi

   echo "deb [arch=$arch_deb signed-by=/etc/apt/keyrings/docker.gpg] \
https://download.docker.com/linux/$docker_os_id $codename stable" | \
      run_sudo tee /etc/apt/sources.list.d/docker.list >/dev/null

   run_sudo apt-get update -qq
   run_sudo apt-get install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin \
      || error "Failed to install Docker packages"

   # Add current user to docker group (avoids needing sudo for docker commands)
   local target_user="${SUDO_USER:-$USER}"
   if ! groups "$target_user" 2>/dev/null | grep -q docker; then
      run_sudo usermod -aG docker "$target_user"
      log "Added $target_user to docker group"
      # Apply group membership for this session so Phase 9 can use docker
      if [ -n "${SUDO_USER:-}" ]; then
         # Running under sudo — newgrp won't help, but sg will for subcommands
         log "Note: Docker group takes effect in new shells; using sudo for this session"
      fi
   fi

   # Start and enable Docker
   run_sudo systemctl enable docker
   run_sudo systemctl start docker

   # Re-detect Docker availability
   if has_command docker; then
      HAS_DOCKER=true
      if docker compose version >/dev/null 2>&1 || sudo docker compose version >/dev/null 2>&1; then
         HAS_DOCKER_COMPOSE=true
      fi
   fi

   log "Docker installed successfully"
}

check_docker() {
   local needs_docker=false
   if echo "${FEATURES:-}" | grep -qE "searxng|flaresolverr"; then
      needs_docker=true
   fi

   if [ "$needs_docker" = true ] && [ "$HAS_DOCKER" = false ]; then
      install_docker
   fi
}

# ─────────────────────────────────────────────────────────────────────────────
# Phase 1 entry point
# ─────────────────────────────────────────────────────────────────────────────

run_deps() {
   header "Phase 1: System Dependencies"
   CURRENT_PHASE="deps"
   install_apt_packages
   check_cmake_version
   check_meson_version
   check_docker
   log "Phase 1 complete"
}
