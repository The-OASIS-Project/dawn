#!/bin/bash
#
# agent-gateway installer: lets DAWN hand coding tasks to Claude Code running
# as a separate, sandboxed user.  Runs as root because it creates the users and
# writes /etc (sudoers, the agent's managed settings, the systemd unit):
#   sudo UV=$(command -v uv) ./install.sh
#
# Installs:
#   /usr/local/lib/agent-gateway/{venvs/<version>,current,python,bin/python3,uv,
#                                 update.py,claude,tests,claude-as-agent,
#                                 claude-agent-exec,agent-gitconfig}
#   /usr/local/etc/agent-gateway/{agent-gateway.toml,env}   (kept if they exist)
#   /etc/claude-code/managed-settings.json                  (the agent's fixed rules)
#   /etc/sudoers.d/agent-gateway                            (dawn may run as dawn-agent)
#   /etc/systemd/system/agent-gateway.service, agent-gateway-update.{service,timer}
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
# Each Claude Code version (= claude-agent-sdk release) gets its own venv in
# venvs/<version>; `current` points at the one that runs.  The updater moves
# it forward after its checks; this installer never moves it back.
log "Python ${PYTHON_VERSION} and the gateway in ${LIB_DIR}"
install -d -m 755 "$LIB_DIR" "$LIB_DIR/venvs"
install -m 755 "$UV" "$LIB_DIR/uv"   # the updater's own copy
export UV_PYTHON_INSTALL_DIR="$LIB_DIR/python"
"$LIB_DIR/uv" python install "$PYTHON_VERSION"
# The updater runs as root on this interpreter alone (no venv, no PyPI code).
install -d -m 755 "$LIB_DIR/bin"
ln -sfn "$("$LIB_DIR/uv" python find --managed-python "$PYTHON_VERSION")" "$LIB_DIR/bin/python3"
PIN="$(sed -n 's/^claude-agent-sdk==//p' "$SCRIPT_DIR/requirements.txt")"
VER="$PIN"
if [ -L "$LIB_DIR/current" ]; then
   CUR="$(basename "$(readlink -f "$LIB_DIR/current")")"
   [ "$(printf '%s\n%s\n' "$CUR" "$PIN" | sort -V | tail -1)" = "$CUR" ] && VER="$CUR"
fi
VENV="$LIB_DIR/venvs/$VER"
[ -x "$VENV/bin/python" ] || "$LIB_DIR/uv" venv -q --python "$PYTHON_VERSION" "$VENV"
grep -v -e '^claude-agent-sdk' -e '^#' "$SCRIPT_DIR/requirements.txt" > "$LIB_DIR/.reqs"
"$LIB_DIR/uv" pip install -q --python "$VENV/bin/python" -r "$LIB_DIR/.reqs" \
   "claude-agent-sdk==$VER" pytest==9.2.0 httpx==0.28.1
rm -f "$LIB_DIR/.reqs"
SITE="$("$VENV/bin/python" -c 'import sysconfig; print(sysconfig.get_paths()["purelib"])')"
rm -rf "$SITE/agent_gateway" "$LIB_DIR/tests"
cp -r "$SCRIPT_DIR/agent_gateway" "$SITE/"
cp -r "$SCRIPT_DIR/tests" "$LIB_DIR/tests"   # the updater runs them on each new version
find "$SITE/agent_gateway" "$LIB_DIR/tests" -name __pycache__ -prune -exec rm -rf {} +
install -m 644 "$SCRIPT_DIR/requirements.txt" "$SCRIPT_DIR/agent-gitconfig" \
   "$SCRIPT_DIR/agent_gateway/update.py" "$LIB_DIR/"
install -m 755 "$SCRIPT_DIR/claude-as-agent" "$SCRIPT_DIR/claude-agent-exec" "$LIB_DIR/"
chmod -R a+rX "$LIB_DIR"
ln -sfn "venvs/$VER" "$LIB_DIR/current"
# Claude Code as the SDK ships it, root-owned; through `current`, so a switch
# moves it too.
ln -sfn "current/${SITE#"$VENV"/}/claude_agent_sdk/_bundled/claude" "$LIB_DIR/claude"
"$LIB_DIR/claude" --version >/dev/null || error "the SDK's bundled claude doesn't run"
log "Claude Code $("$LIB_DIR/claude" --version | cut -d' ' -f1) (claude-agent-sdk $VER)"

# --- the agent's fixed rules ---------------------------------------------
install -d -m 755 /etc/claude-code
# The same rules the gateway passes with every run, as a second layer.
if [ -f "$MANAGED" ] && ! cmp -s "$MANAGED" "$SCRIPT_DIR/agent_gateway/floor.json"; then
   cp "$MANAGED" "$MANAGED.bak.$(date +%Y%m%d%H%M%S)"
   warn "Replaced $MANAGED (previous copy kept beside it)"
fi
install -m 644 "$SCRIPT_DIR/agent_gateway/floor.json" "$MANAGED"

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
install -m 644 "$SCRIPT_DIR/agent-gateway.service" "$SCRIPT_DIR/agent-gateway-update.service" \
   "$SCRIPT_DIR/agent-gateway-update.timer" /etc/systemd/system/
systemctl daemon-reload
systemctl enable agent-gateway.service >/dev/null
systemctl enable --now agent-gateway-update.timer >/dev/null

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
