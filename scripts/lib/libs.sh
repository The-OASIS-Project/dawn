#!/bin/bash
#
# DAWN Installation - Phase 2: Core Libraries
# spdlog, espeak-ng (rhasspy fork), ONNX Runtime, piper-phonemize
#
# Sourced by install.sh. Do not execute directly.
#

# ─────────────────────────────────────────────────────────────────────────────
# spdlog (from apt)
# ─────────────────────────────────────────────────────────────────────────────

install_spdlog() {
   if is_pkg_installed libspdlog-dev; then
      log "spdlog: already installed"
      return 0
   fi
   sudo_begin_phase "spdlog"
   run_sudo apt-get install -y libspdlog-dev || error "Failed to install libspdlog-dev"
   log "spdlog: installed"
}

# ─────────────────────────────────────────────────────────────────────────────
# espeak-ng (rhasspy fork — required for Piper TTS)
# ─────────────────────────────────────────────────────────────────────────────

install_espeak_ng() {
   # Check if the rhasspy fork is already installed by looking for its unique symbol
   local multiarch
   multiarch=$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || echo "")

   local found=false
   # Check unversioned and versioned .so paths
   local -a candidates=(/usr/local/lib/libespeak-ng.so "/usr/lib/${multiarch}/libespeak-ng.so")
   for f in /usr/local/lib/libespeak-ng.so.* "/usr/lib/${multiarch}"/libespeak-ng.so.*; do
      [ -f "$f" ] && candidates+=("$f")
   done
   for so_path in "${candidates[@]}"; do
      if [ -f "$so_path" ] && nm -D "$so_path" 2>/dev/null | grep -q TextToPhonemesWithTerminator; then
         log "espeak-ng: rhasspy fork already installed ($so_path)"
         found=true
         break
      fi
   done
   if [ "$found" = true ]; then
      # Also verify that the dev symlink exists (needed by piper-phonemize build)
      local has_dev=false
      [ -f /usr/local/lib/libespeak-ng.so ] && has_dev=true
      [ -f "/usr/lib/${multiarch}/libespeak-ng.so" ] && has_dev=true
      if [ "$has_dev" = false ]; then
         log "espeak-ng: shared lib found but dev symlink missing — installing libespeak-ng-dev"
         sudo_begin_phase "espeak-ng"
         run_sudo apt-get install -y libespeak-ng-dev 2>/dev/null || true
      fi
      return 0
   fi

   log "Building espeak-ng (rhasspy fork)..."

   # Purge system espeak-ng to avoid conflicts
   sudo_begin_phase "espeak-ng"
   run_sudo apt-get purge -y espeak-ng-data libespeak-ng1 libespeak-ng-libespeak1 2>/dev/null || true

   local tmpdir
   tmpdir=$(mktemp -d)
   register_cleanup "$tmpdir"

   git clone --depth 1 https://github.com/rhasspy/espeak-ng.git "$tmpdir/espeak-ng" ||
      error "Failed to clone espeak-ng"
   cd "$tmpdir/espeak-ng" || return

   ./autogen.sh || error "espeak-ng autogen failed"
   ./configure --prefix=/usr --libdir="/usr/lib/${multiarch}" || error "espeak-ng configure failed"
   make -j"$(nproc)" || error "espeak-ng build failed"

   run_sudo make install || error "espeak-ng install failed"
   sudo_keepalive
   run_sudo ldconfig

   cd "$PROJECT_ROOT" || return
   log "espeak-ng: installed successfully"
}

# ─────────────────────────────────────────────────────────────────────────────
# ONNX Runtime — dispatch: pre-built (RPi, x86) or source (Jetson CUDA)
# ─────────────────────────────────────────────────────────────────────────────

# Set to true if ONNX Runtime was built with CUDA (used by deploy to configure env file)
ONNX_HAS_CUDA=false

install_onnxruntime() {
   if has_lib "libonnxruntime.so" || [ -f /usr/local/lib/libonnxruntime.so.1 ]; then
      if has_header "/usr/local/include/onnxruntime_c_api.h"; then
         log "ONNX Runtime: already installed"
         # Check if the installed version has CUDA support
         if [ "$HAS_CUDA" = true ]; then
            ONNX_HAS_CUDA=true
         fi
         return 0
      fi
   fi

   # Build from source with CUDA on any platform that has CUDA toolkit
   if [ "$HAS_CUDA" = true ]; then
      install_onnxruntime_source
      ONNX_HAS_CUDA=true
   else
      install_onnxruntime_prebuilt
   fi
}

install_onnxruntime_prebuilt() {
   local ort_version ort_tarball ort_dir

   if [ "$ARCH" = "aarch64" ]; then
      ort_version="1.19.2"
      ort_tarball="onnxruntime-linux-aarch64-${ort_version}.tgz"
      ort_dir="onnxruntime-linux-aarch64-${ort_version}"
   else
      # x86_64 — version per GETTING_STARTED_SERVER.md
      ort_version="1.22.0"
      ort_tarball="onnxruntime-linux-x64-${ort_version}.tgz"
      ort_dir="onnxruntime-linux-x64-${ort_version}"
   fi

   local url="https://github.com/microsoft/onnxruntime/releases/download/v${ort_version}/${ort_tarball}"
   local tmpdir
   tmpdir=$(mktemp -d)
   register_cleanup "$tmpdir"

   log "Downloading ONNX Runtime v${ort_version} (pre-built, $ARCH)..."
   wget -q --show-progress -O "$tmpdir/$ort_tarball" "$url" ||
      error "Failed to download ONNX Runtime from $url"
   tar xzf "$tmpdir/$ort_tarball" -C "$tmpdir"

   sudo_begin_phase "onnxruntime"
   # Install headers (note: -r for core/ subdirectory if present)
   run_sudo cp -r "$tmpdir/$ort_dir/include/"* /usr/local/include/ ||
      error "Failed to install ONNX Runtime headers"
   run_sudo cp -a "$tmpdir/$ort_dir/lib/libonnxruntime"*.so* /usr/local/lib/ ||
      error "Failed to install ONNX Runtime libraries"
   run_sudo ldconfig

   log "ONNX Runtime v${ort_version}: installed (pre-built)"
}

install_onnxruntime_source() {
   warn "Building ONNX Runtime from source with CUDA support"
   warn "This may take 30-60 minutes"

   # Check GCC version — v1.19.2 fails on GCC 14+ with -Werror=template-id-cdtor
   local gcc_major
   gcc_major=$(gcc -dumpversion 2>/dev/null | cut -d. -f1)
   local use_alt_gcc=false

   if [ -n "$gcc_major" ] && [ "$gcc_major" -ge 14 ]; then
      log "GCC $gcc_major detected — ONNX Runtime v1.19.2 requires GCC <=13"
      log "Installing gcc-12/g++-12 for the ONNX build..."
      sudo_begin_phase "onnxruntime-gcc"
      if ! is_pkg_installed gcc-12; then
         run_sudo apt-get install -y gcc-12 g++-12 || error "Failed to install gcc-12 g++-12"
      fi
      use_alt_gcc=true
      log "Will use gcc-12/g++-12 for ONNX Runtime build only"
   fi

   local ort_src="/tmp/onnxruntime"
   if [ ! -d "$ort_src" ]; then
      log "Cloning ONNX Runtime v1.19.2..."
      git clone --recursive --branch v1.19.2 --depth 1 \
         https://github.com/microsoft/onnxruntime.git "$ort_src" ||
         error "Failed to clone ONNX Runtime"
   fi

   cd "$ort_src" || return

   # Set compiler override if needed
   local env_prefix=()
   if [ "$use_alt_gcc" = true ]; then
      env_prefix=(env CC=gcc-12 CXX=g++-12)
   fi

   # Build with CUDA. If Eigen download fails, try manual workaround.
   local build_cmd=(./build.sh --use_cuda
      --cudnn_home /usr/local/cuda --cuda_home /usr/local/cuda
      --config MinSizeRel --update --build --parallel --build_shared_lib)

   if ! "${env_prefix[@]}" "${build_cmd[@]}"; then
      warn "Build failed — trying Eigen workaround..."
      local eigen_dir="/tmp/eigen-src"
      if [ ! -d "$eigen_dir" ]; then
         wget -O /tmp/eigen.zip \
            "https://gitlab.com/libeigen/eigen/-/archive/e7248b26a1ed53fa030c5c459f7ea095dfd276ac/eigen-e7248b26a1ed53fa030c5c459f7ea095dfd276ac.zip" ||
            error "Failed to download Eigen"
         unzip -q /tmp/eigen.zip -d "$eigen_dir"
      fi
      local eigen_path
      eigen_path=$(find "$eigen_dir" -maxdepth 1 -type d -name "eigen-*" | head -1)
      "${env_prefix[@]}" "${build_cmd[@]}" --cmake_extra_defines "FETCHCONTENT_SOURCE_DIR_EIGEN=$eigen_path" ||
         error "ONNX Runtime build failed even with Eigen workaround"
   fi

   sudo_begin_phase "onnxruntime"
   run_sudo cp -a build/Linux/MinSizeRel/libonnxruntime.so* /usr/local/lib/
   run_sudo cp include/onnxruntime/core/session/*.h /usr/local/include/
   run_sudo ldconfig

   cd "$PROJECT_ROOT" || return
   log "ONNX Runtime v1.19.2: built and installed with CUDA"
}

# ─────────────────────────────────────────────────────────────────────────────
# piper-phonemize
# ─────────────────────────────────────────────────────────────────────────────

install_piper_phonemize() {
   if has_lib "libpiper_phonemize.so"; then
      log "piper-phonemize: already installed"
      return 0
   fi

   log "Building piper-phonemize..."
   local tmpdir
   tmpdir=$(mktemp -d)
   register_cleanup "$tmpdir"

   git clone --depth 1 https://github.com/rhasspy/piper-phonemize.git "$tmpdir/piper-phonemize" ||
      error "Failed to clone piper-phonemize"
   cd "$tmpdir/piper-phonemize" || return
   mkdir -p build && cd build || return
   cmake .. -DONNXRUNTIME_DIR=/usr/local -DESPEAK_NG_DIR=/usr ||
      error "piper-phonemize cmake failed"
   make -j"$(nproc)" || error "piper-phonemize build failed"

   sudo_begin_phase "piper-phonemize"
   run_sudo cp -a libpiper_phonemize.so* /usr/local/lib/
   run_sudo mkdir -p /usr/local/include/piper-phonemize
   run_sudo cp ../src/*.hpp /usr/local/include/piper-phonemize/
   run_sudo cp ../src/uni_algo.h /usr/local/include/piper-phonemize/
   run_sudo ldconfig

   cd "$PROJECT_ROOT" || return
   log "piper-phonemize: installed successfully"
}

# ─────────────────────────────────────────────────────────────────────────────
# libvosk — pre-built release (used by satellite for streaming ASR)
#
# The daemon does not use libvosk (Whisper only); this lives in libs.sh for
# reuse by satellite.sh. Version tracked here; check
# https://github.com/alphacep/vosk-api/releases for updates.
# ─────────────────────────────────────────────────────────────────────────────

install_libvosk() {
   if has_lib "libvosk.so" && has_header "/usr/local/include/vosk_api.h"; then
      log "libvosk: already installed"
      return 0
   fi

   local vosk_version vosk_dir vosk_tarball
   vosk_version="0.3.45"
   case "$ARCH" in
      aarch64)
         vosk_dir="vosk-linux-aarch64-${vosk_version}"
         vosk_tarball="${vosk_dir}.zip"
         ;;
      x86_64)
         vosk_dir="vosk-linux-x86_64-${vosk_version}"
         vosk_tarball="${vosk_dir}.zip"
         ;;
      *)
         error "libvosk: no pre-built release for architecture '$ARCH'. Vosk requires aarch64 or x86_64. Build from source at https://alphacephei.com/vosk/install"
         ;;
   esac

   local url="https://github.com/alphacep/vosk-api/releases/download/v${vosk_version}/${vosk_tarball}"
   local tmpdir
   tmpdir=$(mktemp -d)
   register_cleanup "$tmpdir"

   log "Downloading libvosk v${vosk_version} (pre-built, $ARCH)..."
   wget -q --show-progress -O "$tmpdir/$vosk_tarball" "$url" ||
      error "Failed to download libvosk from $url"
   unzip -q "$tmpdir/$vosk_tarball" -d "$tmpdir" ||
      error "Failed to extract libvosk archive"

   sudo_begin_phase "libvosk"
   run_sudo cp "$tmpdir/$vosk_dir/libvosk.so" /usr/local/lib/ ||
      error "Failed to install libvosk.so"
   run_sudo cp "$tmpdir/$vosk_dir/vosk_api.h" /usr/local/include/ ||
      error "Failed to install vosk_api.h"
   run_sudo ldconfig

   log "libvosk v${vosk_version}: installed (pre-built)"
}

# ─────────────────────────────────────────────────────────────────────────────
# Library path setup (ensures /usr/local/lib is in ldconfig)
# ─────────────────────────────────────────────────────────────────────────────

# ─────────────────────────────────────────────────────────────────────────────
# libgit2 — in-process git client for the coding-harness code-projects feature.
# Jammy apt ships 1.1; we need >= 1.6, so build from source with the OpenSSL
# HTTPS backend (matches DAWN's TLS stack) and SSH disabled (Phase 1 is HTTPS
# public clone only). Opt-in: only built when INSTALL_LIBGIT2=true, since the
# code-projects feature is OFF by default (DAWN_ENABLE_CODE_PROJECTS).
# ─────────────────────────────────────────────────────────────────────────────

install_libgit2() {
   local libgit2_version="1.8.1"  # >= 1.6 required; pinned stable

   if has_lib "libgit2.so"; then
      local ver
      ver=$(pkg-config --modversion libgit2 2>/dev/null || echo "0")
      if dpkg --compare-versions "$ver" ge "1.6.0" 2>/dev/null; then
         log "libgit2: already installed (v$ver)"
         return 0
      fi
      log "libgit2: found v$ver (< 1.6) — building v${libgit2_version} from source"
   fi

   log "Building libgit2 v${libgit2_version} (OpenSSL backend, SSH off)..."
   sudo_begin_phase "libgit2"
   run_sudo apt-get install -y libssl-dev zlib1g-dev pkg-config 2>/dev/null || true

   local tmpdir
   tmpdir=$(mktemp -d)
   register_cleanup "$tmpdir"

   git clone --depth 1 --branch "v${libgit2_version}" https://github.com/libgit2/libgit2.git \
      "$tmpdir/libgit2" || error "Failed to clone libgit2 v${libgit2_version}"
   mkdir -p "$tmpdir/libgit2/build"
   cd "$tmpdir/libgit2/build" || return

   # USE_HTTPS=OpenSSL pins the TLS backend (eff-16); USE_SSH=OFF avoids a
   # libssh2 dependency we don't need; tests off for build speed.
   cmake .. \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=ON \
      -DUSE_HTTPS=OpenSSL \
      -DUSE_SSH=OFF \
      -DBUILD_TESTS=OFF \
      -DCMAKE_INSTALL_PREFIX=/usr/local || error "libgit2 cmake failed"
   make -j"$(nproc)" || error "libgit2 build failed"

   run_sudo make install || error "libgit2 install failed"
   sudo_keepalive
   run_sudo ldconfig

   cd "$PROJECT_ROOT" || return
   log "libgit2: installed successfully (v${libgit2_version}, OpenSSL backend)"
}

setup_library_path() {
   local ld_conf="/etc/ld.so.conf.d/dawn.conf"
   if [ -f "$ld_conf" ] && grep -q "/usr/local/lib" "$ld_conf"; then
      log "Library path: already configured"
   else
      log "Configuring /usr/local/lib in ldconfig..."
      sudo_begin_phase "ldconfig"
      echo "/usr/local/lib" | run_sudo tee "$ld_conf" >/dev/null
      run_sudo ldconfig
   fi
   LDCONFIG_DONE=true
}

# ─────────────────────────────────────────────────────────────────────────────
# Phase 2 entry point
# ─────────────────────────────────────────────────────────────────────────────

run_libs() {
   header "Phase 2: Core Libraries"
   CURRENT_PHASE="libs"
   sudo_begin_phase "libs"

   install_spdlog
   install_espeak_ng
   sudo_keepalive
   install_onnxruntime
   sudo_keepalive
   install_piper_phonemize
   # Coding-harness code-projects dependency; opt-in (feature OFF by default).
   if [ "${INSTALL_LIBGIT2:-false}" = "true" ]; then
      sudo_keepalive
      install_libgit2
   fi
   setup_library_path

   # Final verification
   local failed=()
   is_pkg_installed libspdlog-dev || failed+=("spdlog")

   local multiarch
   multiarch=$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || echo "")
   if ! has_lib "libespeak-ng.so" && ! [ -f "/usr/lib/${multiarch}/libespeak-ng.so" ] \
      && ! compgen -G "/usr/lib/${multiarch}/libespeak-ng.so.*" >/dev/null 2>&1; then
      failed+=("espeak-ng")
   fi
   has_lib "libonnxruntime.so" || [ -f /usr/local/lib/libonnxruntime.so.1 ] || failed+=("onnxruntime")
   has_lib "libpiper_phonemize.so" || failed+=("piper-phonemize")
   if [ "${INSTALL_LIBGIT2:-false}" = "true" ]; then
      has_lib "libgit2.so" || failed+=("libgit2")
   fi

   if [ ${#failed[@]} -gt 0 ]; then
      error "Core libraries missing after install: ${failed[*]}"
   fi

   log "Phase 2 complete — all core libraries verified"
}
