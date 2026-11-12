#!/bin/bash
#
# DAWN Satellite Installation Script
# Installs dawn_satellite as a systemd service on Raspberry Pi
#

set -e

# Default configuration
BINARY_PATH=""
MODELS_DIR=""
FONTS_DIR=""
CONFIG_SRC=""
CA_CERT_SRC=""
OTA_PUBKEY_SRC=""
SYMLINK_MODELS=false
NO_DISPLAY=false
UNINSTALL=false
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SERVICE_USER="dawn"
SERVICE_NAME="dawn-satellite"
DATA_DIR="/var/lib/dawn-satellite"
CONFIG_DIR="/etc/dawn-satellite"
LIB_DIR="/usr/lib/dawn-satellite"   # bundled non-apt .so (whisper/ggml/onnx/piper)
LAUNCH_DIR="/usr/bin"               # frozen rollback launcher (root-owned, package layout)
LOG_DIR="/var/log/dawn-satellite"

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
RED='\033[0;31m'
NC='\033[0m'

# Helper functions
log() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1"
    exit 1
}

ask_yes_no() {
    local answer
    read -rp "$1 [y/N] " answer
    [[ "$answer" =~ ^[Yy] ]]
}

uninstall() {
    log "Uninstalling dawn-satellite..."

    # 1. Stop and disable service
    if systemctl is-active --quiet "$SERVICE_NAME.service" 2>/dev/null; then
        log "Stopping $SERVICE_NAME service"
        systemctl stop "$SERVICE_NAME.service"
    fi
    if systemctl is-enabled --quiet "$SERVICE_NAME.service" 2>/dev/null; then
        log "Disabling $SERVICE_NAME service"
        systemctl disable "$SERVICE_NAME.service"
    fi

    # 2. Remove systemd unit file
    if [ -f "/etc/systemd/system/$SERVICE_NAME.service" ]; then
        log "Removing systemd service file"
        rm -f "/etc/systemd/system/$SERVICE_NAME.service"
        systemctl daemon-reload
    fi

    # 3. Remove logrotate config
    if [ -f "/etc/logrotate.d/dawn-satellite" ]; then
        log "Removing logrotate configuration"
        rm -f "/etc/logrotate.d/dawn-satellite"
    fi

    # 4. Remove the launcher + any pre-migration binary location
    rm -f "$LAUNCH_DIR/dawn-satellite-launch"   # frozen launcher (/usr/bin)
    if [ -f "/usr/local/bin/dawn_satellite" ]; then
        log "Removing /usr/local/bin/dawn_satellite (pre-migration)"
        rm -f "/usr/local/bin/dawn_satellite"
    fi

    # 5. Remove data directory (models, fonts, identity)
    if [ -d "$DATA_DIR" ]; then
        log "Removing data directory: $DATA_DIR"
        rm -rf "$DATA_DIR"
    fi

    # 6. Remove log directory
    if [ -d "$LOG_DIR" ]; then
        log "Removing log directory: $LOG_DIR"
        rm -rf "$LOG_DIR"
    fi

    # 7. Prompt for configuration removal
    if [ -d "$CONFIG_DIR" ]; then
        if ask_yes_no "Remove configuration files? ($CONFIG_DIR/)"; then
            log "Removing configuration directory: $CONFIG_DIR"
            rm -rf "$CONFIG_DIR"
        else
            log "Keeping configuration: $CONFIG_DIR"
        fi
    fi

    # 8. Remove whisper/ggml libs and ld.so.conf.d entry — only if the
    #    server is not installed (it may share these libraries).
    if [ ! -f "/etc/systemd/system/dawn-server.service" ]; then
        # Bundled libs now live in $LIB_DIR; also sweep the pre-migration
        # /usr/local/lib copies for older installs.
        if [ -d "$LIB_DIR" ]; then
            log "Removing bundled library dir: $LIB_DIR"
            rm -rf "$LIB_DIR"
        fi
        local removed=0
        for lib in /usr/local/lib/libwhisper.so* /usr/local/lib/libggml*.so*; do
            [ -e "$lib" ] || continue
            rm -f "$lib"
            removed=$((removed + 1))
        done
        if [ "$removed" -gt 0 ]; then
            log "Removed $removed pre-migration whisper/ggml library file(s) from /usr/local/lib"
        fi

        rm -f /etc/ld.so.conf.d/dawn-satellite.conf /etc/ld.so.conf.d/dawn.conf 2>/dev/null || true
        ldconfig
    else
        log "Keeping /etc/ld.so.conf.d/dawn.conf and whisper/ggml libs (dawn-server is still installed)"
    fi

    log ""
    log "dawn-satellite has been uninstalled."
    log "Note: dawn system user was not removed (may be shared)."
    log "  Remove manually if desired: userdel dawn"
}

# Parse command line arguments
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            --binary)
                BINARY_PATH="$2"
                shift 2
                ;;
            --models-dir)
                MODELS_DIR="$2"
                shift 2
                ;;
            --fonts-dir)
                FONTS_DIR="$2"
                shift 2
                ;;
            --config)
                CONFIG_SRC="$2"
                shift 2
                ;;
            --ca-cert)
                CA_CERT_SRC="$2"
                shift 2
                ;;
            --ota-pubkey)
                OTA_PUBKEY_SRC="$2"
                shift 2
                ;;
            --symlink-models)
                SYMLINK_MODELS=true
                shift
                ;;
            --no-display)
                NO_DISPLAY=true
                shift
                ;;
            -u|--uninstall)
                UNINSTALL=true
                shift
                ;;
            -h|--help)
                echo "Usage: $0 [options]"
                echo ""
                echo "Options:"
                echo "  --binary PATH        Path to dawn_satellite binary"
                echo "                       (default: searches build dir and project root)"
                echo "  --models-dir PATH    Path to models directory"
                echo "                       (default: dawn_satellite/models/)"
                echo "  --fonts-dir PATH     Path to fonts directory"
                echo "                       (default: dawn_satellite/assets/fonts/)"
                echo "  --config PATH        Path to a pre-configured satellite.toml to install"
                echo "                       (default: the template at $SCRIPT_DIR/satellite.toml)"
                echo "  --ca-cert PATH       Daemon's ca.crt to install at /etc/dawn/ca.crt"
                echo "  --ota-pubkey PATH    OTA verify keyring (hex pubkeys) → /etc/dawn/ota_pubkey"
                echo "                       (required when the satellite config has ssl_verify=true)"
                echo "  --symlink-models     Symlink models instead of copying (saves disk)"
                echo "  --no-display         Skip video/render/input groups (headless satellite)"
                echo "  -u, --uninstall      Remove installed dawn-satellite components"
                echo "  -h, --help           Show this help message"
                echo ""
                echo "Installed paths:"
                echo "  Launcher: $LAUNCH_DIR/dawn-satellite-launch (systemd ExecStart)"
                echo "  Binary:   $DATA_DIR/bin/dawn_satellite (OTA-updatable)"
                echo "  Config:   $CONFIG_DIR/satellite.toml"
                echo "  Data:     $DATA_DIR/"
                echo "  Logs:     $LOG_DIR/"
                exit 0
                ;;
            *)
                error "Unknown option: $1"
                ;;
        esac
    done
}

# Find the satellite binary
find_binary() {
    if [ -n "$BINARY_PATH" ]; then
        if [ ! -f "$BINARY_PATH" ]; then
            error "Binary not found: $BINARY_PATH"
        fi
        return
    fi

    # Search common build locations (build-pi is the trixie-container output)
    local search_paths=(
        "$PROJECT_ROOT/dawn_satellite/build-pi/dawn_satellite"
        "$PROJECT_ROOT/dawn_satellite/build/dawn_satellite"
        "$PROJECT_ROOT/build-debug/dawn_satellite/dawn_satellite"
        "$PROJECT_ROOT/build-release/dawn_satellite/dawn_satellite"
    )

    for path in "${search_paths[@]}"; do
        if [ -f "$path" ]; then
            BINARY_PATH="$path"
            log "Found binary: $BINARY_PATH"
            return
        fi
    done

    error "dawn_satellite binary not found. Build it first or use --binary PATH.

Build instructions:
  cd dawn_satellite && mkdir -p build && cd build
  cmake .. -DENABLE_VOICE=ON -DENABLE_VOSK_ASR=ON
  make -j\$(nproc)"
}

# Find models directory
find_models() {
    if [ -n "$MODELS_DIR" ]; then
        if [ ! -d "$MODELS_DIR" ]; then
            error "Models directory not found: $MODELS_DIR"
        fi
        return
    fi

    local search_paths=(
        "$PROJECT_ROOT/dawn_satellite/models"
        "$PROJECT_ROOT/models"
    )

    for path in "${search_paths[@]}"; do
        if [ -d "$path" ]; then
            MODELS_DIR="$path"
            log "Found models: $MODELS_DIR"
            return
        fi
    done

    warn "No models directory found. You will need to copy models manually to $DATA_DIR/models/"
}

# Extract a TOML string value for KEY under [SECTION] from FILE.
# Prints the unquoted value, or nothing if not found.
parse_toml_str() {
    local section="$1" key="$2" file="$3"
    awk -v want_section="[$section]" -v key="$key" '
        /^\[.*\][[:space:]]*$/ { in_section = ($0 == want_section); next }
        in_section && $1 == key && $2 == "=" {
            # everything after the =
            v = substr($0, index($0, "=") + 1)
            # strip leading/trailing whitespace
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", v)
            # strip surrounding double quotes if present
            gsub(/^"|"$/, "", v)
            print v
            exit
        }
    ' "$file"
}

# Copy only the model files/dirs that the satellite config actually
# references (VAD, ASR, TTS model, TTS config). Avoids dragging in ~2GB
# of unused Vosk data or broken symlinks for models the user never
# downloaded. Source paths are relative to $MODELS_DIR; config paths
# are expected to look like "models/foo/bar".
install_selected_models() {
    local cfg="$1"

    # Roots a model may resolve into (see the symlink check below).
    local MODELS_REAL PROJECT_REAL
    MODELS_REAL=$(realpath -e "$MODELS_DIR") || error "Cannot resolve $MODELS_DIR"
    PROJECT_REAL=$(realpath -e "$PROJECT_ROOT") || error "Cannot resolve $PROJECT_ROOT"

    local -a model_paths=()
    local vad_path asr_path tts_model tts_config
    vad_path=$(parse_toml_str vad model_path "$cfg")
    asr_path=$(parse_toml_str asr model_path "$cfg")
    tts_model=$(parse_toml_str tts model_path "$cfg")
    tts_config=$(parse_toml_str tts config_path "$cfg")

    [ -n "$vad_path" ] && model_paths+=("$vad_path")
    [ -n "$asr_path" ] && model_paths+=("$asr_path")
    [ -n "$tts_model" ] && model_paths+=("$tts_model")
    [ -n "$tts_config" ] && model_paths+=("$tts_config")

    if [ ${#model_paths[@]} -eq 0 ]; then
        warn "No model paths found in config — skipping model install"
        return 0
    fi

    local installed=0 missing=0
    for path in "${model_paths[@]}"; do
        # Expect "models/..." — anything else is either absolute or ~/-style,
        # which won't resolve at runtime anyway. Log and skip.
        if [[ "$path" != models/* ]]; then
            warn "Skipping non-relative model path: $path"
            continue
        fi
        local rel="${path#models/}"
        # Reject any ../ component — a hostile satellite.toml with
        # model_path = "models/../../etc/shadow" would otherwise be
        # cp'd as root from /etc/shadow to /var/lib/dawn-satellite/etc/shadow.
        case "/$rel/" in
            */../*|*/..) warn "Skipping model path with parent traversal: $path"; continue ;;
        esac
        local src="$MODELS_DIR/$rel"
        local dst="$DATA_DIR/models/$rel"

        if [ ! -e "$src" ]; then
            warn "Referenced model not found in source: $path"
            missing=$((missing + 1))
            continue
        fi

        # Resolve symlinks (at any level of $src) and copy the target only if
        # it stays inside the models dir or the checkout: setup_models.sh
        # links models/vosk-model-* -> ../vosk-model-* and models/whisper.cpp
        # -> ../whisper.cpp/models. We run as root and chmod -R 755 below, so
        # a link resolving anywhere else could expose a sensitive file via
        # $DATA_DIR — refuse it.
        local real
        real=$(realpath -e "$src" 2>/dev/null) || real=""
        case "$real" in
            "$MODELS_REAL"/* | "$PROJECT_REAL"/*) src="$real" ;;
            *)
                warn "Refusing model path resolving outside $MODELS_DIR and $PROJECT_ROOT: $path"
                continue
                ;;
        esac
        if [ -d "$src" ] && [ -n "$(find "$src" -type l -print -quit 2>/dev/null)" ]; then
            warn "Refusing model directory containing symlinks: $path"
            continue
        fi

        mkdir -p "$(dirname "$dst")"
        if [ -d "$src" ]; then
            # Vosk model is a directory bundle — copy recursively.
            # --no-dereference preserves any symlinks that slipped past
            # the pre-check above (defense in depth).
            rm -rf "$dst"
            cp -r --no-dereference "$src" "$dst"
        else
            cp -f --no-dereference "$src" "$dst"
        fi
        log "  + ${rel}"
        installed=$((installed + 1))
    done

    log "Installed $installed referenced model(s)${missing:+, $missing missing}"
    if [ "$missing" -gt 0 ]; then
        warn "Some referenced models were missing — run setup_models.sh on the source host and re-deploy"
    fi
}

# Find fonts directory
find_fonts() {
    if [ -n "$FONTS_DIR" ]; then
        if [ ! -d "$FONTS_DIR" ]; then
            error "Fonts directory not found: $FONTS_DIR"
        fi
        return
    fi

    local search_paths=(
        "$PROJECT_ROOT/dawn_satellite/assets/fonts"
        "$PROJECT_ROOT/assets/fonts"
    )

    for path in "${search_paths[@]}"; do
        if [ -d "$path" ]; then
            FONTS_DIR="$path"
            log "Found fonts: $FONTS_DIR"
            return
        fi
    done

    if [ "$NO_DISPLAY" = false ]; then
        warn "No fonts directory found. SDL UI will not have fonts until you copy them to $DATA_DIR/assets/fonts/"
    fi
}

# ============================================================================
# Main
# ============================================================================

# Check if running as root
if [ "$(id -u)" -ne 0 ]; then
    error "This script must be run as root"
fi

# Parse command line arguments
parse_args "$@"

# Handle uninstall
if [ "$UNINSTALL" = true ]; then
    uninstall
    exit 0
fi

# Find files
find_binary
# The OTA rollback launcher is built alongside the binary and is the service's
# ExecStart: without it the service cannot start, so stop before changing anything.
LAUNCHER_PATH="$(dirname "$BINARY_PATH")/dawn-satellite-launch"
if [ ! -f "$LAUNCHER_PATH" ]; then
    error "OTA launcher not found: $LAUNCHER_PATH (the satellite build produces it next to dawn_satellite; rebuild)"
fi
find_models
find_fonts

# Create service user if it doesn't exist
if ! id -u "$SERVICE_USER" &>/dev/null; then
    log "Creating service user: $SERVICE_USER"
    useradd --system --home-dir "$DATA_DIR" --no-create-home --shell /usr/sbin/nologin "$SERVICE_USER"
else
    log "Service user $SERVICE_USER already exists"
fi

# Add user to hardware groups
log "Configuring group membership"
for group in audio; do
    if getent group "$group" >/dev/null; then
        if ! groups "$SERVICE_USER" 2>/dev/null | grep -q "\b$group\b"; then
            usermod -a -G "$group" "$SERVICE_USER"
            log "Added $SERVICE_USER to $group group"
        fi
    fi
done

if [ "$NO_DISPLAY" = false ]; then
    for group in video render input; do
        if getent group "$group" >/dev/null; then
            if ! groups "$SERVICE_USER" 2>/dev/null | grep -q "\b$group\b"; then
                usermod -a -G "$group" "$SERVICE_USER"
                log "Added $SERVICE_USER to $group group"
            fi
        fi
    done
fi

# Create directory structure
log "Creating directory structure"
mkdir -p "$DATA_DIR/models"
mkdir -p "$DATA_DIR/assets/fonts"
mkdir -p "$DATA_DIR/bin"          # OTA: dawn-owned writable bin (atomic self-swap)
mkdir -p "$DATA_DIR/ota/tmp"      # OTA: staging for downloads
mkdir -p "$DATA_DIR/ota/rollback" # OTA: kept-aside previous binary
mkdir -p "$CONFIG_DIR"
mkdir -p "$LOG_DIR"

# Resolve config source early — model install below reads it to pick
# the correct subset of models to copy. --config wins if given;
# otherwise fall back to the shipped template.
CONFIG_EXPLICIT=false
if [ -n "$CONFIG_SRC" ]; then
    if [ ! -f "$CONFIG_SRC" ]; then
        error "Config source not found: $CONFIG_SRC"
    fi
    CONFIG_EXPLICIT=true
    log "Using custom config: $CONFIG_SRC"
else
    CONFIG_SRC="$SCRIPT_DIR/satellite.toml"
fi
# The config the service will run with: an existing one is kept unless --config
# was given (see "Install configuration" below), so models and the Whisper-libs
# check follow it, not the template.
ACTIVE_CONFIG="$CONFIG_SRC"
if [ -f "$CONFIG_DIR/satellite.toml" ] && [ "$CONFIG_EXPLICIT" = false ]; then
    ACTIVE_CONFIG="$CONFIG_DIR/satellite.toml"
fi

# Stop the running service before touching binaries / libraries. Linux
# refuses `cp` over a running executable (Text file busy), and stopping
# here also means we don't have to worry about the service reloading a
# half-installed library set between steps.
if systemctl is-active --quiet "$SERVICE_NAME.service" 2>/dev/null; then
    log "Stopping running $SERVICE_NAME service before replacing files"
    systemctl stop "$SERVICE_NAME.service"
fi

# Install binary into the dawn-owned writable bin dir (OTA atomic self-swap
# needs the binary + rollback copy + staging on one filesystem the dawn user
# can write).  NOT /usr/local/bin (root-owned, read-only under ProtectSystem).
log "Installing binary to $DATA_DIR/bin/dawn_satellite"
cp "$BINARY_PATH" "$DATA_DIR/bin/dawn_satellite"
chmod 755 "$DATA_DIR/bin/dawn_satellite"

# Seed the rollback slot with the freshly-installed binary so the very first
# OTA always has a known-good restore target.
cp "$BINARY_PATH" "$DATA_DIR/ota/rollback/dawn_satellite"
chmod 755 "$DATA_DIR/ota/rollback/dawn_satellite"

# Install the OTA rollback launcher (the systemd ExecStart) to the root-owned,
# package-layout path /usr/bin — frozen, never OTA-updated, and NOT in the
# dawn-writable tree so the service can't tamper with its own rollback agent.
log "Installing OTA launcher to $LAUNCH_DIR/dawn-satellite-launch"
install -m 755 -o root -g root "$LAUNCHER_PATH" "$LAUNCH_DIR/dawn-satellite-launch"
# Drop any pre-migration launcher from the old dawn-writable location.
rm -f "$DATA_DIR/bin/dawn-satellite-launch" 2>/dev/null || true

# Remove any pre-migration binary so the old read-only path can't shadow the
# new writable one (the service now ExecStarts the launcher).
rm -f /usr/local/bin/dawn_satellite 2>/dev/null || true

# Install whisper.cpp / ggml shared libraries.
#
# These are built inside the satellite build tree (dawn_satellite/build/
# contains whisper.cpp/src/libwhisper.so* and ggml/src/libggml*.so*) and
# the binary links against them dynamically. Without this step the service
# fails on startup with:
#   "libwhisper.so.1: cannot open shared object file"
# even though the binary itself is installed.
#
# Only required when the build actually uses Whisper. For vosk-only builds
# these libs aren't produced and their absence is expected — quiet the
# "no libs found" warning in that case to avoid false alarms.
BUILD_DIR="$(dirname "$BINARY_PATH")"
configured_engine=$(parse_toml_str asr engine "$ACTIVE_CONFIG" 2>/dev/null || echo "")
log "Installing whisper/ggml shared libraries from $BUILD_DIR to $LIB_DIR"
mkdir -p "$LIB_DIR"
installed_libs=0
while IFS= read -r -d '' lib; do
    cp -a "$lib" "$LIB_DIR/"
    installed_libs=$((installed_libs + 1))
done < <(find "$BUILD_DIR" \
    \( -name 'libwhisper.so*' -o -name 'libggml*.so*' \) \
    -print0 2>/dev/null)
if [ "$installed_libs" -eq 0 ]; then
    if [ "$configured_engine" = "whisper" ]; then
        warn "No whisper/ggml shared libraries found under $BUILD_DIR"
        warn "This build claims Whisper ASR but the libs are missing — the service will fail to start."
    else
        log "No whisper/ggml libs to install (expected for vosk-only builds)"
    fi
else
    log "Installed $installed_libs whisper/ggml library file(s)"
fi

# Install models
if [ -n "$MODELS_DIR" ]; then
    if [ "$SYMLINK_MODELS" = true ]; then
        log "Symlinking models from $MODELS_DIR"
        # Remove existing models dir if it's not a symlink
        if [ -d "$DATA_DIR/models" ] && [ ! -L "$DATA_DIR/models" ]; then
            rmdir "$DATA_DIR/models" 2>/dev/null || true
        fi
        ln -sfn "$MODELS_DIR" "$DATA_DIR/models"
    else
        # Selective model install: read the satellite config and copy only
        # the models it references (VAD, ASR, TTS model+config). This skips
        # the other TTS voice, any unused Vosk/Whisper variants, the
        # embedding model (not used by satellites), and any broken
        # symlinks left behind by previous setup_models.sh runs.
        #
        # Wipe the destination models dir first — stale broken symlinks
        # from earlier `cp -r` deploys would otherwise block the copy
        # ("cannot overwrite non-directory with directory").
        if [ -d "$DATA_DIR/models" ]; then
            find "$DATA_DIR/models" -mindepth 1 -delete 2>/dev/null || true
        fi

        log "Installing models referenced by $ACTIVE_CONFIG"
        install_selected_models "$ACTIVE_CONFIG"
    fi
fi

# Install fonts
if [ -n "$FONTS_DIR" ]; then
    log "Copying fonts from $FONTS_DIR"
    cp -r "$FONTS_DIR"/. "$DATA_DIR/assets/fonts/"
fi

# Install the daemon's CA certificate at /etc/dawn/ca.crt if provided.
# The satellite config's ca_cert_path is expected to point at this path.
#
# Security: this script runs as root. Without the checks below, a non-root
# caller (who owns their install-state.env or writes install.conf) could
# set SAT_CA_CERT_SRC=/root/.ssh/id_rsa (or a symlink into /etc/shadow)
# and have the root `cp` stage that file at /etc/dawn/ca.crt with mode
# 644 (world-readable) — a root-level arbitrary file disclosure.
# Mitigations:
#  1. Reject symlinks outright (-L check before -f).
#  2. Canonicalize via realpath to catch ../ traversal surprises.
#  3. Require the source to be owned by the invoking non-root user (the
#     user who ran sudo to get here). Prevents a compromised local
#     account from laundering root-owned files through this path.
#  4. Sanity-check that it looks like a PEM CA cert.
if [ -n "$CA_CERT_SRC" ]; then
    if [ -L "$CA_CERT_SRC" ]; then
        error "CA cert source is a symlink (refused): $CA_CERT_SRC"
    fi
    if [ ! -f "$CA_CERT_SRC" ]; then
        error "CA cert source not found or not a regular file: $CA_CERT_SRC"
    fi
    ca_src_real=$(realpath -e --no-symlinks "$CA_CERT_SRC" 2>/dev/null) || ca_src_real=""
    if [ -z "$ca_src_real" ]; then
        error "CA cert source path could not be canonicalized: $CA_CERT_SRC"
    fi
    # Short-circuit: source already IS the installed destination. Happens on
    # re-installs where the state file still points at /etc/dawn/ca.crt from
    # the original run. No copy needed, and no privilege-laundering risk
    # since the root-owned file is already in its intended location.
    ca_dst_real=$(realpath -e --no-symlinks /etc/dawn/ca.crt 2>/dev/null || true)
    if [ -n "$ca_dst_real" ] && [ "$ca_src_real" = "$ca_dst_real" ]; then
        log "CA cert already installed at /etc/dawn/ca.crt — skipping copy"
        CA_CERT_SRC=""  # skip the copy block below
    fi
    # Ownership check: block root-owned files (privilege laundering).
    # SUDO_UID is set by sudo; if unset, we're invoked directly as root,
    # in which case skip the check.
    if [ -n "$CA_CERT_SRC" ] && [ -n "${SUDO_UID:-}" ]; then
        src_uid=$(stat -c '%u' "$ca_src_real")
        if [ "$src_uid" != "$SUDO_UID" ] && [ "$src_uid" != 0 ]; then
            error "CA cert source is owned by uid=$src_uid (expected $SUDO_UID) — refusing"
        fi
        # Extra: if source is root-owned but SUDO_UID is set, that's
        # exactly the privilege-laundering case we want to block.
        if [ "$src_uid" = 0 ] && [ "$SUDO_UID" != 0 ]; then
            error "CA cert source is owned by root; refuse to stage it when invoked via sudo from uid=$SUDO_UID"
        fi
    fi
    if [ -n "$CA_CERT_SRC" ]; then
        # Content sanity — refuse anything that doesn't look like a PEM cert.
        if ! head -n 1 "$ca_src_real" | grep -q '^-----BEGIN CERTIFICATE-----'; then
            error "CA cert source doesn't look like a PEM certificate: $ca_src_real"
        fi
        log "Installing daemon CA certificate to /etc/dawn/ca.crt"
        mkdir -p /etc/dawn
        # cp --no-dereference is belt-and-braces after the symlink check above.
        cp --no-dereference "$ca_src_real" /etc/dawn/ca.crt
        chmod 644 /etc/dawn/ca.crt
        chown root:root /etc/dawn/ca.crt
    fi
fi

# Install the OTA verify keyring at /etc/dawn/ota_pubkey (root-owned, read-only
# to the dawn-uid service — it is NOT under the service's ReadWritePaths, so the
# satellite can read but never modify its own trust anchor).  Each operator
# provisions THEIR OWN pubkey here (from `ota-keytool keygen`); a prebuilt binary
# ships with no baked-in key.  Without this file the satellite refuses all OTA.
if [ -n "$OTA_PUBKEY_SRC" ]; then
    if [ -L "$OTA_PUBKEY_SRC" ]; then
        error "OTA pubkey source is a symlink (refused): $OTA_PUBKEY_SRC"
    fi
    if [ ! -f "$OTA_PUBKEY_SRC" ]; then
        error "OTA pubkey source not found or not a regular file: $OTA_PUBKEY_SRC"
    fi
    ota_pk_real=$(realpath -e --no-symlinks "$OTA_PUBKEY_SRC" 2>/dev/null) || ota_pk_real=""
    if [ -z "$ota_pk_real" ]; then
        error "OTA pubkey source path could not be canonicalized: $OTA_PUBKEY_SRC"
    fi
    # Sanity: at least one 64-hex-char line (an Ed25519 pubkey).
    if ! grep -qE '^[0-9a-fA-F]{64}$' "$ota_pk_real"; then
        error "OTA pubkey source has no 64-hex-char key line: $ota_pk_real"
    fi
    log "Installing OTA verify keyring to /etc/dawn/ota_pubkey"
    mkdir -p /etc/dawn
    cp --no-dereference "$ota_pk_real" /etc/dawn/ota_pubkey
    chmod 644 /etc/dawn/ota_pubkey
    chown root:root /etc/dawn/ota_pubkey
elif [ ! -f /etc/dawn/ota_pubkey ]; then
    warn "No OTA keyring at /etc/dawn/ota_pubkey — OTA updates will be REFUSED until you provision \
one (ota-keytool keygen → install the pubkey there). Pass --ota-pubkey PATH to set it now."
fi

# Install configuration. Default rule: preserve an existing satellite.toml
# (manual edits shouldn't be clobbered by a bare re-run of this script).
# Exception: when --config was explicitly passed, the caller is the top-level
# installer feeding us a freshly-generated TOML — overwrite and back up the
# old one. Otherwise a fresh `--target satellite` flow would write config
# choices to .toml.new where nothing picks them up.
if [ -f "$CONFIG_DIR/satellite.toml" ] && [ "$CONFIG_EXPLICIT" = false ]; then
    warn "Config file already exists: $CONFIG_DIR/satellite.toml (not overwriting)"
    warn "New version saved to: $CONFIG_DIR/satellite.toml.new"
    cp "$CONFIG_SRC" "$CONFIG_DIR/satellite.toml.new"
else
    if [ -f "$CONFIG_DIR/satellite.toml" ]; then
        backup="$CONFIG_DIR/satellite.toml.bak.$(date +%s)"
        cp "$CONFIG_DIR/satellite.toml" "$backup"
        log "Backed up existing config to $backup"
    fi
    log "Installing satellite.toml from $CONFIG_SRC"
    cp "$CONFIG_SRC" "$CONFIG_DIR/satellite.toml"
fi
# Config must be writable by dawn user (satellite saves UI prefs back)
chmod 664 "$CONFIG_DIR/satellite.toml"
chown "$SERVICE_USER:$SERVICE_USER" "$CONFIG_DIR/satellite.toml"

# Install environment file
log "Installing environment configuration"
cp "$SCRIPT_DIR/dawn-satellite.conf" "$CONFIG_DIR/"
chmod 644 "$CONFIG_DIR/dawn-satellite.conf"

# Set permissions
log "Setting permissions"
chown -R "$SERVICE_USER:$SERVICE_USER" "$DATA_DIR"
chmod -R 755 "$DATA_DIR"
# OTA staging dirs must be 0700 — set AFTER the recursive 755 above (which would
# otherwise clobber them); they hold transient downloads + the rollback copy.
chmod 700 "$DATA_DIR/ota/tmp" "$DATA_DIR/ota/rollback"
# The satellite's identity file ($HOME of the service user) holds its UUID and
# reconnect secret. The satellite creates it 0600 but never re-chmods an
# existing file, so restore owner-only after the recursive 755 above.
if [ -f "$DATA_DIR/.dawn_satellite_identity" ]; then
    chmod 600 "$DATA_DIR/.dawn_satellite_identity"
fi
chown "$SERVICE_USER:$SERVICE_USER" "$LOG_DIR"
chmod 755 "$LOG_DIR"
chown -R root:root "$CONFIG_DIR"
chown "$SERVICE_USER:$SERVICE_USER" "$CONFIG_DIR/satellite.toml"

# Ensure the bundled-lib path is on the linker search path.
log "Configuring library path ($LIB_DIR)"
echo "$LIB_DIR" > /etc/ld.so.conf.d/dawn-satellite.conf
rm -f /etc/ld.so.conf.d/dawn.conf 2>/dev/null || true   # retire the pre-migration /usr/local/lib entry
# Always rebuild the linker cache — we may have just installed new
# libwhisper.so* / libggml*.so* files above.
ldconfig

# Install systemd service
log "Installing systemd service"
cp "$SCRIPT_DIR/dawn-satellite.service" /etc/systemd/system/

# Install logrotate configuration
log "Installing logrotate configuration"
cp "$SCRIPT_DIR/dawn-satellite-logrotate" /etc/logrotate.d/dawn-satellite
chmod 644 /etc/logrotate.d/dawn-satellite

# Enable and start service
log "Enabling and starting service"
systemctl daemon-reload
systemctl enable "$SERVICE_NAME.service"
systemctl restart "$SERVICE_NAME.service"

# Check service status
sleep 2
if systemctl is-active --quiet "$SERVICE_NAME.service"; then
    log "Service successfully started"
    log ""
    log "Management commands:"
    log "  Status:  systemctl status $SERVICE_NAME"
    log "  Logs:    tail -f $LOG_DIR/satellite.log"
    log "  Restart: systemctl restart $SERVICE_NAME"
    log "  Stop:    systemctl stop $SERVICE_NAME"
    log ""
    log "Next steps:"
    log "  1. Edit $CONFIG_DIR/satellite.toml"
    log "     - Set [server] host to your DAWN daemon IP"
    log "     - Set [identity] name and location"
    log "     - Configure [audio] capture/playback devices"
    log "  2. Restart: sudo systemctl restart $SERVICE_NAME"
else
    warn "Service failed to start. Check logs:"
    warn "  journalctl -u $SERVICE_NAME -n 50"
    warn "  cat $LOG_DIR/satellite.log"
    exit 1
fi
