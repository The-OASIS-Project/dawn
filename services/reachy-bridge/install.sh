#!/bin/bash
#
# reachy-bridge installer: a Reachy Mini Lite as the body of DAWN's local
# interface.  Run as the user DAWN runs as (not root); it asks for sudo only to
# install the udev rule.
#
# Installs:
#   ~/.local/share/reachy-bridge/venv        uv-managed CPython 3.12 + reachy-mini
#   ~/.local/share/reachy-bridge/{reachy_bridge,expression,audio_health}.py
#   ~/.config/reachy-bridge/reachy-bridge.toml   (kept if it exists)
#   ~/.config/reachy-bridge/env                  (MQTT credentials, mode 600)
#   ~/.config/systemd/user/{reachy-mini-daemon,reachy-bridge}.service
#   /etc/udev/rules.d/99-reachy-mini.rules
#
# Usage: ./install.sh [--no-udev] [--no-enable]
#
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHARE_DIR="$HOME/.local/share/reachy-bridge"
CONFIG_DIR="$HOME/.config/reachy-bridge"
UNIT_DIR="$HOME/.config/systemd/user"
PYTHON_VERSION="3.12"
DO_UDEV=true
DO_ENABLE=true

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
log()   { echo -e "${GREEN}[INFO]${NC} $1"; }
warn()  { echo -e "${YELLOW}[WARNING]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

while [ $# -gt 0 ]; do
   case "$1" in
      --no-udev)   DO_UDEV=false; shift ;;
      --no-enable) DO_ENABLE=false; shift ;;
      *) error "Unknown option: $1" ;;
   esac
done

[ "$(id -u)" -ne 0 ] || error "Run as the user DAWN runs as, not root."

command -v parec >/dev/null || error "parec not found (sudo apt install pulseaudio-utils)"
# reachy-mini imports gi (PyGObject), which has no aarch64 wheel and builds
# from source against these.
pkg-config --exists gobject-introspection-1.0 cairo ||
   error "PyGObject build headers missing: sudo apt install libgirepository1.0-dev libcairo2-dev"
if ! command -v uv >/dev/null; then
   error "uv not found. Install it (https://docs.astral.sh/uv/getting-started/installation/), then re-run."
fi

log "Python ${PYTHON_VERSION} venv in ${SHARE_DIR}/venv"
mkdir -p "$SHARE_DIR" "$CONFIG_DIR" "$UNIT_DIR"
uv python install "$PYTHON_VERSION"
[ -d "$SHARE_DIR/venv" ] || uv venv --python "$PYTHON_VERSION" "$SHARE_DIR/venv"
uv pip install --python "$SHARE_DIR/venv/bin/python" -r "$SCRIPT_DIR/requirements.txt"

install -m 644 "$SCRIPT_DIR/reachy_bridge.py" "$SCRIPT_DIR/expression.py" \
   "$SCRIPT_DIR/audio_health.py" "$SHARE_DIR/"
if [ -f "$CONFIG_DIR/reachy-bridge.toml" ]; then
   log "Keeping existing $CONFIG_DIR/reachy-bridge.toml (new defaults: $SCRIPT_DIR/reachy-bridge.toml)"
else
   install -m 644 "$SCRIPT_DIR/reachy-bridge.toml" "$CONFIG_DIR/reachy-bridge.toml"
fi
if [ ! -f "$CONFIG_DIR/env" ]; then
   umask 077
   printf 'REACHY_BRIDGE_MQTT_USER=\nREACHY_BRIDGE_MQTT_PASSWORD=\n' > "$CONFIG_DIR/env"
   warn "Fill in MQTT credentials in $CONFIG_DIR/env (mode 600)"
fi

install -m 644 "$SCRIPT_DIR/reachy-mini-daemon.service" "$UNIT_DIR/"
install -m 644 "$SCRIPT_DIR/reachy-bridge.service" "$UNIT_DIR/"
systemctl --user daemon-reload

if $DO_UDEV; then
   log "Installing udev rule (sudo)"
   sudo install -m 644 "$SCRIPT_DIR/99-reachy-mini.rules" /etc/udev/rules.d/
   sudo udevadm control --reload-rules
   sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=38fb
   sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=2886
   id -nG | grep -qw plugdev || warn "Add yourself to plugdev: sudo usermod -aG plugdev $USER (then log in again)"
   id -nG | grep -qw dialout || warn "Add yourself to dialout: sudo usermod -aG dialout $USER (then log in again)"
fi

if $DO_ENABLE; then
   systemctl --user enable reachy-mini-daemon.service reachy-bridge.service
   log "Enabled. Start with: systemctl --user start reachy-mini-daemon reachy-bridge"
   log "To run at boot without a login: sudo loginctl enable-linger $USER"
fi
log "Logs: journalctl --user -u reachy-bridge -f"
