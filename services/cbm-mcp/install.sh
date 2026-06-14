#!/bin/bash
#
# Codebase Memory MCP (cbm-mcp) installer for DAWN.
#
# Builds the cbm binary (libgit2 disabled — see README.md), installs mcp-proxy
# into a dedicated venv, and registers cbm-mcp as a systemd service that
# re-exposes cbm over SSE for DAWN's MCP bridge to connect to.
#
# The service runs as the 'dawn' user so it can read DAWN's clone tree
# (/var/lib/dawn/source) and write its graph cache under /var/lib/dawn.
#
# Usage:
#   sudo ./install.sh                      # build + install + enable
#   sudo ./install.sh --cbm-src DIR        # cbm source checkout (default: ~/code/codebase-memory-mcp)
#   sudo ./install.sh --port N --host ADDR # override SSE bind (default 127.0.0.1:9750)
#
set -e

# ── Paths / defaults ─────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVICE_USER="dawn"
SERVICE_GROUP="dawn"
if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != "root" ]; then
   CBM_SRC="/home/$SUDO_USER/code/codebase-memory-mcp"
else
   CBM_SRC="$HOME/code/codebase-memory-mcp"
fi
CBM_BIN_DEST="/usr/local/bin/codebase-memory-mcp"
VENV_DIR="/usr/local/lib/cbm-mcp/venv"
CONFIG_DIR="/usr/local/etc/cbm-mcp"
CONFIG_FILE="$CONFIG_DIR/cbm-mcp.conf"
LOG_DIR="/var/log/dawn"
DATA_DIR="/var/lib/dawn"
CACHE_DIR="$DATA_DIR/cbm-cache"
HOST="127.0.0.1"
PORT="9750"

# ── Colors / helpers ─────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log()   { echo -e "${GREEN}[INFO]${NC} $1"; }
warn()  { echo -e "${YELLOW}[WARNING]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

# Run a command as the invoking (non-root) user when possible — used for the
# in-tree cbm build so artifacts aren't left root-owned in the user's repo.
run_as_user() {
   if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != "root" ]; then
      sudo -u "$SUDO_USER" "$@"
   else
      "$@"
   fi
}

# ── Parse args ───────────────────────────────────────────────────
while [ $# -gt 0 ]; do
   case "$1" in
      --cbm-src) CBM_SRC="$2"; shift 2 ;;
      --port)    PORT="$2"; shift 2 ;;
      --host)    HOST="$2"; shift 2 ;;
      -h|--help)
         echo "Usage: sudo ./install.sh [--cbm-src DIR] [--port N] [--host ADDR]"
         exit 0 ;;
      *) error "Unknown argument: $1 (try --help)" ;;
   esac
done

# ── Preconditions ────────────────────────────────────────────────
[ "$(id -u)" -eq 0 ] || error "Run as root: sudo ./install.sh"
command -v python3 >/dev/null || error "python3 not found"
python3 -m venv --help >/dev/null 2>&1 || \
   error "python3 venv module missing — install it (e.g. sudo apt install python3-venv)"

# ── 1. Build cbm if not already built ───────────────────────────
if [ ! -x "$CBM_SRC/build/c/codebase-memory-mcp" ]; then
   [ -f "$CBM_SRC/scripts/build.sh" ] || \
      error "cbm source not found at $CBM_SRC — clone it or pass --cbm-src DIR"
   log "Building cbm (libgit2 disabled) in $CBM_SRC"
   run_as_user bash -c "cd '$CBM_SRC' && scripts/build.sh LIBGIT2_FLAGS= LIBGIT2_LIBS="
fi
[ -x "$CBM_SRC/build/c/codebase-memory-mcp" ] || error "cbm build produced no binary"

# ── 2. Install cbm binary ───────────────────────────────────────
log "Installing cbm binary -> $CBM_BIN_DEST"
install -m 755 "$CBM_SRC/build/c/codebase-memory-mcp" "$CBM_BIN_DEST"

# ── 3. mcp-proxy venv ───────────────────────────────────────────
log "Creating mcp-proxy venv -> $VENV_DIR"
mkdir -p "$(dirname "$VENV_DIR")"
python3 -m venv "$VENV_DIR"
"$VENV_DIR/bin/pip" install --quiet --upgrade pip
"$VENV_DIR/bin/pip" install --quiet mcp-proxy
[ -x "$VENV_DIR/bin/mcp-proxy" ] || error "mcp-proxy install failed"

# ── 4. Service user + directories ───────────────────────────────
if ! id -u "$SERVICE_USER" &>/dev/null; then
   log "Creating system user '$SERVICE_USER'"
   useradd --system --no-create-home --shell /usr/sbin/nologin "$SERVICE_USER"
fi
mkdir -p "$DATA_DIR" "$CACHE_DIR" "$LOG_DIR"
chown "$SERVICE_USER:$SERVICE_GROUP" "$DATA_DIR" "$CACHE_DIR" "$LOG_DIR"
chmod 755 "$DATA_DIR" "$CACHE_DIR" "$LOG_DIR"

# ── 5. Config file (preserve existing edits) ────────────────────
mkdir -p "$CONFIG_DIR"
if [ -f "$CONFIG_FILE" ]; then
   warn "Config exists, leaving as-is: $CONFIG_FILE"
else
   log "Installing config -> $CONFIG_FILE"
   sed -e "s|^HOST=.*|HOST=$HOST|" -e "s|^PORT=.*|PORT=$PORT|" \
      "$SCRIPT_DIR/cbm-mcp.conf" > "$CONFIG_FILE"
   chmod 644 "$CONFIG_FILE"
fi

# ── 6. systemd unit + logrotate ─────────────────────────────────
log "Installing systemd unit + logrotate"
cp "$SCRIPT_DIR/cbm-mcp.service" /etc/systemd/system/
cp "$SCRIPT_DIR/cbm-mcp-logrotate" /etc/logrotate.d/cbm-mcp
chmod 644 /etc/logrotate.d/cbm-mcp

# ── 7. Enable + start ───────────────────────────────────────────
log "Enabling and starting cbm-mcp"
systemctl daemon-reload
systemctl enable cbm-mcp.service
systemctl restart cbm-mcp.service

sleep 2
echo ""
if systemctl is-active --quiet cbm-mcp.service; then
   echo -e "${GREEN}cbm-mcp is running on http://$HOST:$PORT/sse${NC}"
   echo ""
   log "Add this to dawn.toml so DAWN connects to it:"
   cat <<EOF

    [mcp]
    enabled = true
    dev_mode = true            # required for tls_verify = false on plain-http localhost

    [[mcp.server]]
    alias = "cbm"              # MUST be exactly "cbm" — the code-graph provider keys on it
    url = "http://$HOST:$PORT/sse"
    transport = "http+sse"
    enabled = true
    capabilities = "dangerous"
    tls_verify = false

EOF
   log "Then in dawn.toml also set: [code_projects] enabled = true"
   echo ""
   log "Management commands:"
   log "  Status:   systemctl status cbm-mcp"
   log "  Logs:     journalctl -u cbm-mcp -f   (or /var/log/dawn/cbm-mcp.log)"
   log "  Restart:  systemctl restart cbm-mcp"
   log "  Stop:     systemctl stop cbm-mcp"
   echo ""
   log "Verify from DAWN:  dawn-admin mcp status   (expect: cbm: connected)"
else
   warn "Service failed to start. Check logs:"
   warn "  journalctl -u cbm-mcp -f"
   exit 1
fi
