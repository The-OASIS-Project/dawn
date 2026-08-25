#!/bin/bash
#
# DAWN Server Installation Script
# Installs dawn as a systemd service on Jetson (or other Linux systems)
#

set -e

# Default configuration
BINARY_PATH=""
MODELS_DIR=""
WWW_DIR=""
SSL_DIR=""
CONFIG_PATH=""
SECRETS_PATH=""
SYMLINK_MODELS=false
SYMLINK_WWW=false
UNINSTALL=false
ADMIN_CREATED=false
ADMIN_PASS=""
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SERVICE_USER="dawn"
SERVICE_NAME="dawn-server"
DATA_DIR="/var/lib/dawn"
CONFIG_DIR="/usr/local/etc/dawn"
LOG_DIR="/var/log/dawn"

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
    log "Uninstalling dawn-server..."

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
    if [ -f "/etc/logrotate.d/dawn-server" ]; then
        log "Removing logrotate configuration"
        rm -f "/etc/logrotate.d/dawn-server"
    fi

    # 4. Remove binary
    if [ -f "/usr/local/bin/dawn" ]; then
        log "Removing /usr/local/bin/dawn"
        rm -f "/usr/local/bin/dawn"
    fi

    # 5. Remove secrets.toml symlink
    if [ -L "$DATA_DIR/secrets.toml" ]; then
        log "Removing secrets.toml symlink from $DATA_DIR"
        rm -f "$DATA_DIR/secrets.toml"
    fi

    # 5b. Remove models.toml symlink
    if [ -L "$DATA_DIR/models.toml" ]; then
        log "Removing models.toml symlink from $DATA_DIR"
        rm -f "$DATA_DIR/models.toml"
    fi

    # 6. Prompt for database removal
    local remove_db=false
    local db_dir="$DATA_DIR/db"
    if [ -d "$db_dir" ] && ls "$db_dir"/*.db >/dev/null 2>&1; then
        if ask_yes_no "Remove databases? ($db_dir/*.db)"; then
            remove_db=true
        else
            log "Keeping databases in $db_dir"
        fi
    fi

    # 7. Remove data directory (models, www, ssl)
    if [ -d "$DATA_DIR" ]; then
        if [ "$remove_db" = true ]; then
            log "Removing data directory: $DATA_DIR"
            rm -rf "$DATA_DIR"
        else
            log "Removing data directory contents (preserving db/)"
            find "$DATA_DIR" -mindepth 1 -not -path "$db_dir" -not -path "$db_dir/*" -delete 2>/dev/null || true
        fi
    fi

    # 8. Remove log directory
    if [ -d "$LOG_DIR" ]; then
        log "Removing log directory: $LOG_DIR"
        rm -rf "$LOG_DIR"
    fi

    # 9. Prompt for configuration removal
    if [ -d "$CONFIG_DIR" ]; then
        if ask_yes_no "Remove configuration files? ($CONFIG_DIR/)"; then
            log "Removing configuration directory: $CONFIG_DIR"
            rm -rf "$CONFIG_DIR"
        else
            log "Keeping configuration: $CONFIG_DIR"
        fi
    fi

    # 10. Remove ld.so.conf.d entry (only if satellite is not installed)
    if [ -f "/etc/ld.so.conf.d/dawn.conf" ]; then
        if [ ! -f "/etc/systemd/system/dawn-satellite.service" ]; then
            log "Removing /etc/ld.so.conf.d/dawn.conf"
            rm -f "/etc/ld.so.conf.d/dawn.conf"
            ldconfig
        else
            log "Keeping /etc/ld.so.conf.d/dawn.conf (dawn-satellite is still installed)"
        fi
    fi

    log ""
    log "dawn-server has been uninstalled."
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
            --www-dir)
                WWW_DIR="$2"
                shift 2
                ;;
            --ssl-dir)
                SSL_DIR="$2"
                shift 2
                ;;
            --config)
                CONFIG_PATH="$2"
                shift 2
                ;;
            --secrets)
                SECRETS_PATH="$2"
                shift 2
                ;;
            --symlink-models)
                SYMLINK_MODELS=true
                shift
                ;;
            --symlink-www)
                SYMLINK_WWW=true
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
                echo "  --binary PATH        Path to dawn binary"
                echo "                       (default: searches build-debug/, build-release/)"
                echo "  --models-dir PATH    Path to models directory"
                echo "                       (default: models/)"
                echo "  --www-dir PATH       Path to www directory (WebUI static files)"
                echo "                       (default: www/)"
                echo "  --ssl-dir PATH       Path to SSL certificates directory"
                echo "                       (default: ssl/)"
                echo "  --config PATH        Path to dawn.toml config file"
                echo "                       (default: dawn.toml in project root)"
                echo "  --secrets PATH       Path to secrets.toml"
                echo "                       (default: secrets.toml in project root)"
                echo "  --symlink-models     Symlink models instead of copying"
                echo "  --symlink-www        Symlink www instead of copying"
                echo "  -u, --uninstall      Remove installed dawn-server components"
                echo "  -h, --help           Show this help message"
                echo ""
                echo "Installed paths:"
                echo "  Binary:  /usr/local/bin/dawn"
                echo "  Config:  $CONFIG_DIR/dawn.toml"
                echo "  Secrets: $CONFIG_DIR/secrets.toml (mode 0600)"
                echo "  Data:    $DATA_DIR/"
                echo "  Logs:    $LOG_DIR/"
                exit 0
                ;;
            *)
                error "Unknown option: $1"
                ;;
        esac
    done
}

# Find the dawn binary
find_binary() {
    if [ -n "$BINARY_PATH" ]; then
        if [ ! -f "$BINARY_PATH" ]; then
            error "Binary not found: $BINARY_PATH"
        fi
        return
    fi

    local search_paths=(
        "$PROJECT_ROOT/build-debug/dawn"
        "$PROJECT_ROOT/build-release/dawn"
        "$PROJECT_ROOT/build/dawn"
    )

    for path in "${search_paths[@]}"; do
        if [ -f "$path" ]; then
            BINARY_PATH="$path"
            log "Found binary: $BINARY_PATH"
            return
        fi
    done

    error "dawn binary not found. Build it first or use --binary PATH.

Build instructions:
  cmake --preset debug
  make -C build-debug -j\$(nproc)"
}

# Find www directory
find_www() {
    if [ -n "$WWW_DIR" ]; then
        if [ ! -d "$WWW_DIR" ]; then
            error "www directory not found: $WWW_DIR"
        fi
        return
    fi

    if [ -d "$PROJECT_ROOT/www" ]; then
        WWW_DIR="$PROJECT_ROOT/www"
        log "Found www: $WWW_DIR"
    else
        warn "No www directory found. WebUI will not work until you copy files to $DATA_DIR/www/"
    fi
}

# Find models directory
find_models() {
    if [ -n "$MODELS_DIR" ]; then
        if [ ! -d "$MODELS_DIR" ]; then
            error "Models directory not found: $MODELS_DIR"
        fi
        return
    fi

    if [ -d "$PROJECT_ROOT/models" ]; then
        MODELS_DIR="$PROJECT_ROOT/models"
        log "Found models: $MODELS_DIR"
    else
        warn "No models directory found. You will need to copy models to $DATA_DIR/models/"
    fi
}

# Find SSL certificates
find_ssl() {
    if [ -n "$SSL_DIR" ]; then
        if [ ! -d "$SSL_DIR" ]; then
            error "SSL directory not found: $SSL_DIR"
        fi
        return
    fi

    if [ -d "$PROJECT_ROOT/ssl" ]; then
        SSL_DIR="$PROJECT_ROOT/ssl"
        log "Found SSL certs: $SSL_DIR"
    else
        warn "No ssl/ directory found. TLS will not work until you generate certificates."
        warn "Run: ./generate_ssl_cert.sh"
    fi
}

# Find config files
find_config() {
    if [ -z "$CONFIG_PATH" ] && [ -f "$PROJECT_ROOT/dawn.toml" ]; then
        CONFIG_PATH="$PROJECT_ROOT/dawn.toml"
        log "Found config: $CONFIG_PATH"
    fi

    if [ -z "$SECRETS_PATH" ] && [ -f "$PROJECT_ROOT/secrets.toml" ]; then
        SECRETS_PATH="$PROJECT_ROOT/secrets.toml"
        log "Found secrets: $SECRETS_PATH"
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
find_www
find_models
find_ssl
find_config

# Create service user if it doesn't exist
if ! id -u "$SERVICE_USER" &>/dev/null; then
    log "Creating service user: $SERVICE_USER"
    useradd --system --home-dir "$DATA_DIR" --no-create-home --shell /usr/sbin/nologin "$SERVICE_USER"
else
    log "Service user $SERVICE_USER already exists"
fi

# Add user to hardware groups
log "Configuring group membership"
for group in audio video render; do
    if getent group "$group" >/dev/null; then
        if ! groups "$SERVICE_USER" 2>/dev/null | grep -q "\b$group\b"; then
            usermod -a -G "$group" "$SERVICE_USER"
            log "Added $SERVICE_USER to $group group"
        fi
    fi
done

# Create directory structure
log "Creating directory structure"
mkdir -p "$DATA_DIR/models"
mkdir -p "$DATA_DIR/www"
mkdir -p "$DATA_DIR/ssl"
mkdir -p "$DATA_DIR/db"
mkdir -p "$CONFIG_DIR"
mkdir -p "$LOG_DIR"
chown "$SERVICE_USER:$SERVICE_USER" "$LOG_DIR"
chmod 750 "$LOG_DIR"

# Install binary and shared libraries via cmake --install
# This installs dawn, dawn-admin, and all subproject libraries (whisper, ggml)
# with proper RPATH set to /usr/local/lib (build-tree paths stripped automatically)
BUILD_DIR="$(dirname "$BINARY_PATH")"
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    log "Installing via cmake --install (prefix: /usr/local)"
    cmake --install "$BUILD_DIR" --prefix /usr/local 2>&1 | while IFS= read -r line; do
        log "  $line"
    done
    ldconfig
else
    error "CMake build directory not found (no CMakeCache.txt in $(dirname "$BINARY_PATH")).

Build with cmake first, then deploy:
  cmake --preset default && cmake --build --preset default
  sudo $0 --binary <build-dir>/dawn"
fi

# Install www (WebUI static files)
if [ -n "$WWW_DIR" ]; then
    if [ "$SYMLINK_WWW" = true ]; then
        log "Symlinking www from $WWW_DIR"
        if [ -d "$DATA_DIR/www" ] && [ ! -L "$DATA_DIR/www" ]; then
            rmdir "$DATA_DIR/www" 2>/dev/null || true
        fi
        ln -sfn "$WWW_DIR" "$DATA_DIR/www"
    else
        log "Copying www from $WWW_DIR"
        cp -r "$WWW_DIR"/. "$DATA_DIR/www/"
    fi
fi

# Install models
if [ -n "$MODELS_DIR" ]; then
    if [ "$SYMLINK_MODELS" = true ]; then
        log "Symlinking models from $MODELS_DIR"
        if [ -d "$DATA_DIR/models" ] && [ ! -L "$DATA_DIR/models" ]; then
            rmdir "$DATA_DIR/models" 2>/dev/null || true
        fi
        ln -sfn "$MODELS_DIR" "$DATA_DIR/models"
    else
        log "Copying models from $MODELS_DIR"
        # Remove stale symlinks at destination before copying
        find "$DATA_DIR/models" -maxdepth 1 -type l -delete 2>/dev/null || true
        # Copy with symlink dereferencing; skip broken symlinks
        for item in "$MODELS_DIR"/*; do
            if [ -e "$item" ]; then
                cp -rL "$item" "$DATA_DIR/models/" 2>/dev/null || true
            fi
        done
    fi
fi

# Install SSL certificates
if [ -n "$SSL_DIR" ]; then
    log "Copying SSL certificates from $SSL_DIR"
    cp -r "$SSL_DIR"/. "$DATA_DIR/ssl/"
    chmod 600 "$DATA_DIR/ssl/"*.key 2>/dev/null || true
    chmod 644 "$DATA_DIR/ssl/"*.crt 2>/dev/null || true
fi

# Install configuration (never overwrite existing)
if [ -f "$CONFIG_DIR/dawn.toml" ]; then
    warn "Config file already exists: $CONFIG_DIR/dawn.toml (not overwriting)"
    if [ -n "$CONFIG_PATH" ]; then
        warn "New config saved to: $CONFIG_DIR/dawn.toml.new"
        cp "$CONFIG_PATH" "$CONFIG_DIR/dawn.toml.new"
    fi
else
    if [ -n "$CONFIG_PATH" ]; then
        log "Installing dawn.toml from $CONFIG_PATH"
        cp "$CONFIG_PATH" "$CONFIG_DIR/dawn.toml"
    else
        log "Installing template dawn.toml"
        cp "$SCRIPT_DIR/dawn.toml" "$CONFIG_DIR/dawn.toml"
    fi
fi
chmod 644 "$CONFIG_DIR/dawn.toml"

# Set service-appropriate paths in config (data_dir defaults to ~/.local/share/dawn
# which is wrong for a system service — use /var/lib/dawn/db instead)
if grep -q '^# *data_dir' "$CONFIG_DIR/dawn.toml"; then
    sed -i 's|^# *data_dir *= *".*"|data_dir = "/var/lib/dawn/db"|' "$CONFIG_DIR/dawn.toml"
    log "Set data_dir = /var/lib/dawn/db"
elif ! grep -q '^data_dir' "$CONFIG_DIR/dawn.toml"; then
    # [paths] section might not exist — append if needed
    if grep -q '^\[paths\]' "$CONFIG_DIR/dawn.toml"; then
        sed -i '/^\[paths\]/a data_dir = "/var/lib/dawn/db"' "$CONFIG_DIR/dawn.toml"
    else
        printf '\n[paths]\ndata_dir = "/var/lib/dawn/db"\n' >> "$CONFIG_DIR/dawn.toml"
    fi
    log "Set data_dir = /var/lib/dawn/db"
fi

# Install secrets (never overwrite existing, restrictive permissions)
if [ -f "$CONFIG_DIR/secrets.toml" ]; then
    warn "Secrets file already exists: $CONFIG_DIR/secrets.toml (not overwriting)"
    if [ -n "$SECRETS_PATH" ]; then
        warn "New secrets saved to: $CONFIG_DIR/secrets.toml.new"
        cp "$SECRETS_PATH" "$CONFIG_DIR/secrets.toml.new"
        chmod 600 "$CONFIG_DIR/secrets.toml.new"
        chown "$SERVICE_USER:$SERVICE_USER" "$CONFIG_DIR/secrets.toml.new"
    fi
else
    if [ -n "$SECRETS_PATH" ]; then
        log "Installing secrets.toml"
        cp "$SECRETS_PATH" "$CONFIG_DIR/secrets.toml"
    else
        log "Creating empty secrets.toml (add your API keys later)"
        cat > "$CONFIG_DIR/secrets.toml" << 'SECRETS'
# DAWN API Keys and Credentials
# Add your keys here and restart the service.
#
# openai_api_key = "sk-..."
# claude_api_key = "sk-ant-..."
SECRETS
    fi
fi
chmod 600 "$CONFIG_DIR/secrets.toml"
chown "$SERVICE_USER:$SERVICE_USER" "$CONFIG_DIR/secrets.toml"

# Create symlink so config_load_secrets_from_search() finds secrets.toml in WorkingDirectory
if [ ! -L "$DATA_DIR/secrets.toml" ]; then
    log "Creating secrets.toml symlink in $DATA_DIR"
    ln -sf "$CONFIG_DIR/secrets.toml" "$DATA_DIR/secrets.toml"
fi

# Install models.toml (model context-window registry) and symlink into the
# WorkingDirectory so llm_context's cwd-relative search finds it (same pattern as
# secrets.toml above). A missing file only degrades to per-provider defaults.
# Do NOT overwrite an operator-edited copy on re-install — the file is an
# edit-to-update surface (drop a .new beside it instead), mirroring the dawn.toml
# handling above.
if [ -f "$PROJECT_ROOT/models.toml" ]; then
    if [ -f "$CONFIG_DIR/models.toml" ]; then
        if ! cmp -s "$PROJECT_ROOT/models.toml" "$CONFIG_DIR/models.toml"; then
            warn "models.toml already exists: $CONFIG_DIR/models.toml (not overwriting)"
            warn "Shipped version saved to: $CONFIG_DIR/models.toml.new"
            cp "$PROJECT_ROOT/models.toml" "$CONFIG_DIR/models.toml.new"
            chown "$SERVICE_USER:$SERVICE_USER" "$CONFIG_DIR/models.toml.new"
        fi
    else
        log "Installing models.toml"
        cp "$PROJECT_ROOT/models.toml" "$CONFIG_DIR/models.toml"
        chmod 644 "$CONFIG_DIR/models.toml"
        chown "$SERVICE_USER:$SERVICE_USER" "$CONFIG_DIR/models.toml"
    fi
    if [ ! -L "$DATA_DIR/models.toml" ]; then
        log "Creating models.toml symlink in $DATA_DIR"
        ln -sf "$CONFIG_DIR/models.toml" "$DATA_DIR/models.toml"
    fi
fi

# Install environment file
log "Installing environment configuration"
cp "$SCRIPT_DIR/dawn-server.conf" "$CONFIG_DIR/"
chmod 644 "$CONFIG_DIR/dawn-server.conf"

# Set permissions
log "Setting permissions"
chown -R "$SERVICE_USER:$SERVICE_USER" "$DATA_DIR"
chmod -R 755 "$DATA_DIR"
chown -R root:root "$CONFIG_DIR"
chown "$SERVICE_USER:$SERVICE_USER" "$CONFIG_DIR/dawn.toml"
chown "$SERVICE_USER:$SERVICE_USER" "$CONFIG_DIR/secrets.toml"

# Ensure library path is configured
log "Configuring library path"
if [ ! -f /etc/ld.so.conf.d/dawn.conf ]; then
    echo "/usr/local/lib" > /etc/ld.so.conf.d/dawn.conf
    ldconfig
    log "Added /usr/local/lib to library path"
fi

# Install systemd service
log "Installing systemd service"
cp "$SCRIPT_DIR/dawn-server.service" /etc/systemd/system/

# Install logrotate configuration
log "Installing logrotate configuration"
cp "$SCRIPT_DIR/dawn-server-logrotate" /etc/logrotate.d/dawn-server
chmod 644 /etc/logrotate.d/dawn-server

# Copy existing auth.db from dev install (Phase 8 creates admin there)
# This avoids needing to re-create admin for the service
dev_auth_db=""
for candidate in \
    "/home/$SUDO_USER/.local/share/dawn/auth.db" \
    "$HOME/.local/share/dawn/auth.db"; do
    if [ -n "$candidate" ] && [ -f "$candidate" ]; then
        dev_auth_db="$candidate"
        break
    fi
done

if [ -n "$dev_auth_db" ] && [ ! -f "$DATA_DIR/db/auth.db" ]; then
    log "Copying auth.db from $dev_auth_db"
    cp "$dev_auth_db" "$DATA_DIR/db/auth.db"
    # Also copy WAL/SHM if present (ensures all data is captured)
    [ -f "${dev_auth_db}-wal" ] && cp "${dev_auth_db}-wal" "$DATA_DIR/db/auth.db-wal"
    [ -f "${dev_auth_db}-shm" ] && cp "${dev_auth_db}-shm" "$DATA_DIR/db/auth.db-shm"
    chown "$SERVICE_USER:$SERVICE_USER" "$DATA_DIR/db/auth.db"*
    chmod 600 "$DATA_DIR/db/auth.db"*
    ADMIN_CREATED=carried
    log "Admin account carried over from development install"
fi

# Enable and start service
log "Enabling and starting service"
systemctl daemon-reload
systemctl enable "$SERVICE_NAME.service"
systemctl restart "$SERVICE_NAME.service"

# Check service status
sleep 3
if systemctl is-active --quiet "$SERVICE_NAME.service"; then
    log "Service successfully started"

    # Create admin account if no users exist yet
    has_users=false
    if [ -f "$DATA_DIR/db/auth.db" ]; then
        if command -v sqlite3 >/dev/null 2>&1; then
            user_count=$(sqlite3 "$DATA_DIR/db/auth.db" "SELECT COUNT(*) FROM users;" 2>/dev/null) || user_count=0
            [ "$user_count" -gt 0 ] 2>/dev/null && has_users=true
        else
            # No sqlite3 CLI — use dawn-admin to check
            # Match only non-zero counts (e.g., "1 user(s) total" but NOT "0 user(s) total")
            if dawn-admin ping >/dev/null 2>&1 && \
               dawn-admin user list 2>/dev/null | grep -qE "^[1-9][0-9]* user\(s\) total"; then
                has_users=true
            fi
        fi
    fi
    if [ "$has_users" = false ] && command -v dawn-admin >/dev/null 2>&1; then
        log ""
        log "Creating admin account..."

        # Wait for daemon to be fully ready (model loading can be slow on Pi)
        ready=false timeout=60 elapsed=0
        while [ $elapsed -lt $timeout ]; do
            if dawn-admin ping >/dev/null 2>&1; then
                ready=true
                break
            fi
            sleep 2
            ((++elapsed+=2))
        done

        if [ "$ready" != true ]; then
            warn "Daemon not ready after ${timeout}s"
            warn "Create admin manually after service is running:"
            warn "  1. Find the setup token: grep 'Token:' $LOG_DIR/server.log"
            warn "  2. Run: DAWN_SETUP_TOKEN=<token> dawn-admin user create admin --admin"
        else
            # Extract setup token from service log
            token=$(grep -oP 'Token: \K[A-Z0-9-]+' "$LOG_DIR/server.log" 2>/dev/null | tail -1 || true)

            if [ -n "$token" ]; then
                admin_pass=$(dd if=/dev/urandom bs=64 count=1 status=none | tr -dc 'A-Za-z0-9' | head -c 16)
                if DAWN_SETUP_TOKEN="$token" DAWN_PASSWORD="$admin_pass" \
                    dawn-admin user create admin --admin >/dev/null 2>&1; then
                    ADMIN_CREATED=true
                    ADMIN_PASS="$admin_pass"
                else
                    warn "Admin account creation failed (may already exist)"
                fi
            else
                warn "Setup token not found in logs (admin may already exist)"
                warn "If needed, create admin manually:"
                warn "  1. Find the setup token: grep 'Token:' $LOG_DIR/server.log"
                warn "  2. Run: DAWN_SETUP_TOKEN=<token> dawn-admin user create admin --admin"
            fi
        fi
    fi

    log ""
    log "Management commands:"
    log "  Status:  systemctl status $SERVICE_NAME"
    log "  Logs:    tail -f $LOG_DIR/server.log"
    log "  Restart: systemctl restart $SERVICE_NAME"
    log "  Stop:    systemctl stop $SERVICE_NAME"
    log ""
    log "Configuration files:"
    log "  Main:    $CONFIG_DIR/dawn.toml"
    log "  Secrets: $CONFIG_DIR/secrets.toml"
    log "  Env:     $CONFIG_DIR/dawn-server.conf"
    log ""
    ip=$(hostname -I 2>/dev/null | awk '{print $1}' || echo "localhost")
    proto="http"
    grep -q 'https *= *true' "$CONFIG_DIR/dawn.toml" 2>/dev/null && proto="https"

    echo ""
    echo -e "${GREEN}╔═══════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║              DAWN is ready!                   ║${NC}"
    echo -e "${GREEN}╚═══════════════════════════════════════════════╝${NC}"
    echo ""
    echo -e "  WebUI:     ${GREEN}${proto}://${ip}:3000${NC}"
    if [ "${ADMIN_CREATED:-false}" = true ]; then
        echo ""
        echo -e "  Username:  ${GREEN}admin${NC}"
        echo -e "  Password:  ${GREEN}${ADMIN_PASS}${NC}"
        echo ""
        warn "Save this password now — it will not be shown again."
        warn "Change it after first login via Settings > Account."
    elif [ "${ADMIN_CREATED:-false}" = "carried" ]; then
        echo ""
        log "Admin account carried over — use the password from Phase 8 above."
    fi
    echo ""
    log "Wake word: \"Hey Friday\" or \"Okay Friday\""
    if [ "$proto" = "https" ]; then
        echo ""
        log "Install ssl/ca.crt in your browser to avoid certificate warnings"
    fi
else
    warn "Service failed to start. Check logs:"
    warn "  journalctl -u $SERVICE_NAME -n 50"
    warn "  cat $LOG_DIR/server.log"
    exit 1
fi
