#!/bin/bash
#
# agent-gateway installer: lets DAWN hand coding tasks to Claude Code running
# as a separate, sandboxed user.  Runs as root because it creates the users and
# writes /etc (sudoers, the agent's managed settings, the systemd unit):
#   sudo UV=$(command -v uv) ./install.sh
#
# Installs:
#   /usr/local/lib/agent-gateway/{venv,python,claude,claude-as-agent,claude-agent-exec,
#                                 agent-gitconfig}
#   /usr/local/etc/agent-gateway/{agent-gateway.toml,env}   (kept if they exist)
#   /etc/claude-code/managed-settings.json                  (the agent's fixed rules)
#   /etc/sudoers.d/agent-gateway                            (dawn may run as dawn-agent)
#   /etc/systemd/system/agent-gateway.service
#
# Users: the gateway runs as `dawn` (shared with cbm-mcp); the agent runs as
# `dawn-agent`, which can't read the gateway's token or DAWN's data.
#
# Usage: sudo ./install.sh [--no-start]
#
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB_DIR="/usr/local/lib/agent-gateway"
ETC_DIR="/usr/local/etc/agent-gateway"
STATE_DIR="/var/lib/dawn-gateway"
AGENT_HOME="/var/lib/dawn-agent"
MANAGED="/etc/claude-code/managed-settings.json"
PYTHON_VERSION="3.12"
DO_START=true

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
log()   { echo -e "${GREEN}[INFO]${NC} $1"; }
warn()  { echo -e "${YELLOW}[WARNING]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

while [ $# -gt 0 ]; do
   case "$1" in
      --no-start) DO_START=false; shift ;;
      *) error "Unknown option: $1" ;;
   esac
done

[ "$(id -u)" -eq 0 ] || error "Run as root: sudo UV=\$(command -v uv) ./install.sh"
for cmd in bwrap socat git sudo openssl; do
   command -v "$cmd" >/dev/null ||
      error "$cmd not found (sudo apt install bubblewrap socat git sudo openssl)"
done
# uv runs as root here, so it is taken only from root's PATH or named explicitly.
UV="${UV:-$(command -v uv || true)}"
[ -x "$UV" ] || error "uv not found; run as: sudo UV=\$(command -v uv) ./install.sh"

# --- users --------------------------------------------------------------
if ! id -u dawn &>/dev/null; then
   log "Creating system user dawn"
   useradd --system --no-create-home --shell /usr/sbin/nologin dawn
fi
if ! id -u dawn-agent &>/dev/null; then
   log "Creating system user dawn-agent"
   useradd --system --create-home --home-dir "$AGENT_HOME" --shell /bin/bash dawn-agent
fi
chmod 750 "$AGENT_HOME"
# The gateway reads the agent's clones (to publish) and enters its worktrees.
usermod -aG dawn-agent dawn
install -d -o dawn-agent -g dawn-agent -m 750 "$AGENT_HOME/work"
# The agent user clones from the mirrors, so it may pass through the state dir
# (not list it) and read the mirrors through its group.
install -d -o dawn -g dawn -m 711 "$STATE_DIR"
install -d -o dawn -g dawn-agent -m 2750 "$STATE_DIR/mirrors"

# --- python + gateway ---------------------------------------------------
log "Python ${PYTHON_VERSION} venv in ${LIB_DIR}/venv"
install -d -m 755 "$LIB_DIR"
export UV_PYTHON_INSTALL_DIR="$LIB_DIR/python"
"$UV" python install "$PYTHON_VERSION"
[ -d "$LIB_DIR/venv" ] || "$UV" venv --python "$PYTHON_VERSION" "$LIB_DIR/venv"
"$UV" pip install --python "$LIB_DIR/venv/bin/python" -r "$SCRIPT_DIR/requirements.txt"
SITE="$("$LIB_DIR/venv/bin/python" -c 'import sysconfig; print(sysconfig.get_paths()["purelib"])')"
rm -rf "$SITE/agent_gateway"
cp -r "$SCRIPT_DIR/agent_gateway" "$SITE/"
find "$SITE/agent_gateway" -name __pycache__ -prune -exec rm -rf {} +
chmod -R a+rX "$LIB_DIR"
install -m 755 "$SCRIPT_DIR/claude-as-agent" "$SCRIPT_DIR/claude-agent-exec" "$LIB_DIR/"
install -m 644 "$SCRIPT_DIR/agent-gitconfig" "$LIB_DIR/"
# Claude Code as the SDK ships it: pinned with the SDK, root-owned.
ln -sf "$SITE/claude_agent_sdk/_bundled/claude" "$LIB_DIR/claude"
"$LIB_DIR/claude" --version >/dev/null || error "the SDK's bundled claude doesn't run"

# --- the agent's fixed rules ---------------------------------------------
install -d -m 755 /etc/claude-code
if [ -f "$MANAGED" ] && ! cmp -s "$MANAGED" "$SCRIPT_DIR/managed-settings.json"; then
   cp "$MANAGED" "$MANAGED.bak.$(date +%Y%m%d%H%M%S)"
   warn "Replaced $MANAGED (previous copy kept beside it)"
fi
install -m 644 "$SCRIPT_DIR/managed-settings.json" "$MANAGED"

log "sudoers: dawn may run commands as dawn-agent"
install -m 440 "$SCRIPT_DIR/agent-gateway.sudoers" /etc/sudoers.d/agent-gateway
visudo -cf /etc/sudoers.d/agent-gateway >/dev/null || {
   rm -f /etc/sudoers.d/agent-gateway; error "sudoers file failed visudo"; }

# --- configuration -------------------------------------------------------
install -d -o root -g dawn -m 750 "$ETC_DIR"
if [ -f "$ETC_DIR/agent-gateway.toml" ]; then
   log "Keeping $ETC_DIR/agent-gateway.toml (new defaults: $SCRIPT_DIR/agent-gateway.toml)"
else
   install -o root -g dawn -m 640 "$SCRIPT_DIR/agent-gateway.toml" "$ETC_DIR/"
fi
if [ ! -f "$ETC_DIR/env" ]; then
   log "Generating the bearer token DAWN will send ($ETC_DIR/env)"
   umask 077
   {
      echo "AGENT_GATEWAY_TOKEN=$(openssl rand -hex 32)"
      echo "# Fine-grained GitHub token (these repos: Contents read/write, Pull requests"
      echo "# read/write).  Without it the gateway works but can't publish."
      echo "AGENT_GATEWAY_GITHUB_TOKEN="
   } > "$ETC_DIR/env"
   chown dawn:dawn "$ETC_DIR/env"
   chmod 600 "$ETC_DIR/env"
fi

# --- service ---------------------------------------------------------------
install -m 644 "$SCRIPT_DIR/agent-gateway.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable agent-gateway.service >/dev/null

if [ ! -f "$AGENT_HOME/.claude/.credentials.json" ] && \
   [ ! -f "$AGENT_HOME/.config/anthropic.env" ]; then
   warn "dawn-agent has no credential yet.  Either (from a directory dawn-agent can read):"
   echo "   cd / && sudo -u dawn-agent -H $LIB_DIR/claude      # then /login"
   echo "   or put ANTHROPIC_API_KEY=... in $AGENT_HOME/.config/anthropic.env (mode 600)"
   echo "   and set [agent] auth = \"api_key\" in $ETC_DIR/agent-gateway.toml"
fi

if $DO_START; then
   systemctl restart agent-gateway.service
   log "agent-gateway running on 127.0.0.1:9760 (journalctl -u agent-gateway)"
else
   log "Installed; start with: sudo systemctl start agent-gateway"
fi
log "Edit [[repos]] in $ETC_DIR/agent-gateway.toml to list the repos the agent may work in."
