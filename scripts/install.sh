#!/bin/bash
#
# DAWN Installation Script
# Automated installation of the DAWN voice assistant
#
# Usage:
#   ./scripts/install.sh                    # Full interactive install
#   ./scripts/install.sh --verify           # Run verification only
#   ./scripts/install.sh --resume-from libs # Resume from a specific phase
#   ./scripts/install.sh --config FILE      # Unattended with config file
#   ./scripts/install.sh -h                 # Show help
#

set -euo pipefail

# ─────────────────────────────────────────────────────────────────────────────
# Resolve paths
# ─────────────────────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LIB_DIR="$SCRIPT_DIR/lib"

# ─────────────────────────────────────────────────────────────────────────────
# Source library files
# ─────────────────────────────────────────────────────────────────────────────

# shellcheck source=lib/common.sh
source "$LIB_DIR/common.sh"
# shellcheck source=lib/deps.sh
source "$LIB_DIR/deps.sh"
# shellcheck source=lib/libs.sh
source "$LIB_DIR/libs.sh"
# shellcheck source=lib/build.sh
source "$LIB_DIR/build.sh"
# shellcheck source=lib/configure.sh
source "$LIB_DIR/configure.sh"
# shellcheck source=lib/services.sh
source "$LIB_DIR/services.sh"
# shellcheck source=lib/verify.sh
source "$LIB_DIR/verify.sh"
# shellcheck source=lib/uninstall.sh
source "$LIB_DIR/uninstall.sh"
# shellcheck source=lib/satellite.sh
source "$LIB_DIR/satellite.sh"

# ─────────────────────────────────────────────────────────────────────────────
# Default settings
# ─────────────────────────────────────────────────────────────────────────────

INTERACTIVE=true
VERBOSE=false
DRY_RUN=false
VERIFY_ONLY=false
FRESH_INSTALL=false
DEPLOY_TARGET=""
UNINSTALL=false
RESUME_FROM=""
CONFIG_FILE=""
CURRENT_PHASE=""
INSTALL_ADMIN_PASS=""
SETTINGS_LOADED=false

# User preferences (set via CLI, config file, or interactive prompts)
# Leave INSTALL_TARGET empty by default so main() can prompt in interactive
# mode. --target, install.conf, and load_state all populate it explicitly.
INSTALL_TARGET="${INSTALL_TARGET:-}"
LLM_PROVIDER="${LLM_PROVIDER:-}"
PRIMARY_PROVIDER="${PRIMARY_PROVIDER:-}"
OPENAI_KEY="${OPENAI_KEY:-}"
CLAUDE_KEY="${CLAUDE_KEY:-}"
GEMINI_KEY="${GEMINI_KEY:-}"
WHISPER_MODEL="${WHISPER_MODEL:-}"
BUILD_PRESET="${BUILD_PRESET:-}"
FEATURES="${FEATURES:-}"

# Installer version for state management
INSTALLER_SHA=$(git -C "$PROJECT_ROOT" rev-parse --short HEAD 2>/dev/null || echo "unknown")

# ─────────────────────────────────────────────────────────────────────────────
# Usage
# ─────────────────────────────────────────────────────────────────────────────

usage() {
   cat <<EOF
DAWN Installation Script

Usage: $(basename "$0") [OPTIONS]

Options:
  --target VALUE         Install target: server (default) or satellite (Raspberry Pi)
  --provider VALUE       LLM provider(s), comma-separated (openai,claude,gemini,local)
                         [server only]
  --whisper-model VALUE  Whisper model size (tiny, base, small)  [server only]
  --preset VALUE         Build preset (default, local, debug, server)  [server only]
  --features VALUE       Features, comma-separated or "all"  [server only]
                         (ssl,mqtt,searxng,flaresolverr,plex,homeassistant)
  --config FILE          Config file for unattended installation
  --unattended           Disable interactive prompts (use defaults)
  --fresh                Ignore cached settings from a previous run
  --resume-from PHASE    Resume from a specific phase
  --verify               Run verification suite only
  --deploy TARGET        Deploy as systemd service (server or satellite)
  -u, --uninstall        Remove DAWN-installed components (interactive)
  --dry-run              Show what would be done (not implemented yet)
  --verbose, -v          Verbose output
  -h, --help             Show this help

Phases (for --resume-from):
  discovery, deps, libs, build, models, configure, apikeys, ssl, admin,
  services, verify, deploy
  (satellite skips apikeys, ssl, admin, services)

Examples:
  # Interactive install with defaults (server/daemon)
  ./scripts/install.sh

  # Install a Tier 1 satellite on Raspberry Pi
  ./scripts/install.sh --target satellite

  # Unattended with Claude as primary provider
  ./scripts/install.sh --config scripts/install.conf.example

  # Resume after a build failure
  ./scripts/install.sh --resume-from build

  # Re-run ignoring previous choices
  ./scripts/install.sh --fresh

  # Just verify an existing installation
  ./scripts/install.sh --verify

  # Deploy as a systemd service (after building)
  ./scripts/install.sh --deploy server
  ./scripts/install.sh --deploy satellite
EOF
   exit 0
}

# ─────────────────────────────────────────────────────────────────────────────
# Argument parsing
# ─────────────────────────────────────────────────────────────────────────────

parse_args() {
   while [ $# -gt 0 ]; do
      case "$1" in
         --target)
            case "$2" in
               server | satellite) INSTALL_TARGET="$2" ;;
               *) error "Invalid --target value: $2 (must be 'server' or 'satellite')" ;;
            esac
            shift 2
            ;;
         --provider)
            LLM_PROVIDER="$2"
            PRIMARY_PROVIDER="${LLM_PROVIDER%%,*}"
            shift 2
            ;;
         --whisper-model)
            WHISPER_MODEL="$2"
            shift 2
            ;;
         --preset)
            BUILD_PRESET="$2"
            shift 2
            ;;
         --features)
            FEATURES="$2"
            shift 2
            ;;
         --config)
            CONFIG_FILE="$2"
            INTERACTIVE=false
            shift 2
            ;;
         --unattended)
            INTERACTIVE=false
            shift
            ;;
         --fresh)
            FRESH_INSTALL=true
            shift
            ;;
         --resume-from)
            RESUME_FROM="$2"
            shift 2
            ;;
         --verify)
            VERIFY_ONLY=true
            shift
            ;;
         --deploy)
            DEPLOY_TARGET="$2"
            shift 2
            ;;
         -u | --uninstall)
            UNINSTALL=true
            shift
            ;;
         --dry-run)
            DRY_RUN=true
            shift
            ;;
         --verbose | -v)
            VERBOSE=true
            shift
            ;;
         -h | --help)
            usage
            ;;
         *)
            error "Unknown option: $1 (use -h for help)"
            ;;
      esac
   done
}

# ─────────────────────────────────────────────────────────────────────────────
# Config file validation and loading
# ─────────────────────────────────────────────────────────────────────────────

load_config_file() {
   local file="$1"

   if [ ! -f "$file" ]; then
      error "Config file not found: $file"
   fi

   # Syntax check
   if ! bash -n "$file" 2>/dev/null; then
      error "Config file has syntax errors: $file"
   fi

   # Security check: only allow variable assignments (no commands)
   local suspicious
   suspicious=$(grep -vE '^\s*#|^\s*$|^\s*[A-Z_]+=' "$file" | head -5 || true)
   if [ -n "$suspicious" ]; then
      warn "Config file contains non-assignment lines:"
      echo "$suspicious"
      error "Config file must only contain KEY=VALUE assignments"
   fi

   # Check permissions if it contains API keys
   if grep -qE '(KEY|key|secret)' "$file"; then
      local perms
      perms=$(stat -c '%a' "$file" 2>/dev/null || echo "???")
      if [ "$perms" != "600" ] && [ "$perms" != "400" ]; then
         warn "Config file contains keys but has permissive mode ($perms). Consider: chmod 600 $file"
      fi
   fi

   # shellcheck disable=SC1090
   source "$file"
   PRIMARY_PROVIDER="${LLM_PROVIDER%%,*}"
   log "Loaded config from $file"
}

# ─────────────────────────────────────────────────────────────────────────────
# Phase 4: Download Models (delegates to setup_models.sh)
# ─────────────────────────────────────────────────────────────────────────────

run_models() {
   header "Phase 4: Download Models"
   CURRENT_PHASE="models"

   local model_script="$PROJECT_ROOT/setup_models.sh"
   if [ ! -x "$model_script" ]; then
      chmod +x "$model_script" 2>/dev/null || true
   fi

   if [ ! -f "$model_script" ]; then
      error "setup_models.sh not found at $PROJECT_ROOT"
   fi

   local model_args=()
   if [ -n "${WHISPER_MODEL:-}" ]; then
      model_args+=(--whisper-model "$WHISPER_MODEL")
   fi

   log "Running setup_models.sh ${model_args[*]:-}..."
   "$model_script" "${model_args[@]}" || error "Model download failed"

   # Verify
   local model_found=false
   local f
   for f in "$PROJECT_ROOT"/models/whisper.cpp/ggml-*.bin; do
      [ -f "$f" ] && model_found=true && break
   done
   if [ "$model_found" = false ]; then
      warn "Whisper model not found after setup_models.sh"
   fi

   log "Phase 4 complete"
}

# ─────────────────────────────────────────────────────────────────────────────
# Phase 7: SSL Setup (delegates to generate_ssl_cert.sh)
# ─────────────────────────────────────────────────────────────────────────────

ssl_certs_valid() {
   # Check that SSL certs exist, are non-empty, and parseable
   local cert="$PROJECT_ROOT/ssl/dawn.crt"
   local key="$PROJECT_ROOT/ssl/dawn.key"
   [ -f "$cert" ] && [ -s "$cert" ] && [ -f "$key" ] && [ -s "$key" ] &&
      openssl x509 -in "$cert" -noout -checkend 0 >/dev/null 2>&1
}

run_ssl() {
   header "Phase 7: SSL Setup"
   CURRENT_PHASE="ssl"

   if ! echo "${FEATURES:-}" | grep -q "ssl"; then
      log "SSL: not selected, skipping"
      return 0
   fi

   # Check if valid certs already exist (not just that files are present)
   if ssl_certs_valid; then
      log "SSL: certificates already exist and are valid"
      configure_ssl
      return 0
   fi

   # If cert files exist but are invalid (e.g., 0-byte from a failed run), clean up
   if [ -f "$PROJECT_ROOT/ssl/dawn.crt" ] || [ -f "$PROJECT_ROOT/ssl/dawn.key" ]; then
      if ! ssl_certs_valid; then
         warn "SSL: found invalid/partial certificate files — removing"
         rm -f "$PROJECT_ROOT/ssl/dawn.crt" "$PROJECT_ROOT/ssl/dawn.key" \
               "$PROJECT_ROOT/ssl/dawn-chain.crt" "$PROJECT_ROOT/ssl/dawn.csr" \
               "$PROJECT_ROOT/ssl/dawn.ext" "$PROJECT_ROOT/ssl/ca.srl"
      fi
   fi

   local ssl_script="$PROJECT_ROOT/generate_ssl_cert.sh"
   if [ ! -f "$ssl_script" ]; then
      warn "SSL: generate_ssl_cert.sh not found — skipping"
      return 0
   fi
   chmod +x "$ssl_script" 2>/dev/null || true

   if [ "$INTERACTIVE" = false ]; then
      warn "SSL: generate_ssl_cert.sh requires interactive input (CA passphrase)"
      warn "SSL: skipping in unattended mode — run ./generate_ssl_cert.sh manually"
      return 0
   fi

   # Try up to 2 times (user may mistype CA passphrase)
   local attempt max_attempts=2
   for ((attempt = 1; attempt <= max_attempts; attempt++)); do
      if [ "$attempt" -gt 1 ]; then
         echo ""
         warn "SSL: retrying certificate generation (attempt $attempt/$max_attempts)..."
      else
         log "Running SSL certificate generator..."
         log "You will be prompted for a CA passphrase."
      fi

      if "$ssl_script"; then
         # Verify the result is actually valid
         if ssl_certs_valid; then
            configure_ssl

            # Install CA into system trust store
            if [ -f "$PROJECT_ROOT/ssl/ca.crt" ]; then
               sudo_begin_phase "ssl"
               run_sudo cp "$PROJECT_ROOT/ssl/ca.crt" /usr/local/share/ca-certificates/dawn-ca.crt
               run_sudo update-ca-certificates 2>/dev/null || true
               log "CA certificate installed in system trust store"
            fi

            log "Phase 7 complete"
            return 0
         else
            warn "SSL: certificate files created but failed validation"
         fi
      else
         warn "SSL: certificate generation failed (attempt $attempt/$max_attempts)"
      fi
   done

   # All attempts failed — warn but continue installation
   warn "SSL: could not generate valid certificates after $max_attempts attempts"
   warn "SSL: run ./generate_ssl_cert.sh manually to set up HTTPS"
   log "Phase 7 complete (SSL skipped)"
}

# ─────────────────────────────────────────────────────────────────────────────
# Phase 8: Create Admin Account
# ─────────────────────────────────────────────────────────────────────────────

run_admin() {
   header "Phase 8: Create Admin Account"
   CURRENT_PHASE="admin"

   if [ -z "${BUILD_DIR:-}" ] || [ ! -f "$BUILD_DIR/dawn" ]; then
      warn "Admin: binary not found, skipping admin creation"
      return 0
   fi

   if [ ! -f "$BUILD_DIR/dawn-admin" ]; then
      warn "Admin: dawn-admin binary not found, skipping"
      return 0
   fi

   # Check if admin already exists (dev location or service location)
   local db_path="$HOME/.local/share/dawn/auth.db"
   local service_db="/var/lib/dawn/db/auth.db"
   if [ -f "$db_path" ] || [ -f "$service_db" ]; then
      log "Admin: auth.db already exists, skipping account creation"
      return 0
   fi

   # Stop any existing dawn-server service to avoid socket conflicts
   if systemctl is-active --quiet dawn-server 2>/dev/null; then
      log "Stopping existing dawn-server service..."
      run_sudo systemctl stop dawn-server
      sleep 1
   fi

   # Ensure XDG data directory exists (fresh installs may lack ~/.local/share)
   mkdir -p "$HOME/.local/share"

   log "Starting DAWN daemon to obtain setup token..."

   local log_file="/tmp/dawn-install-startup-$$.log"

   # Start daemon directly (not via run_dawn wrapper) so $! captures the
   # actual binary PID, not a subshell.  A backgrounded function call creates
   # a subshell whose PID is what $! returns — killing it orphans the real
   # process.
   LD_LIBRARY_PATH=/usr/local/lib "$BUILD_DIR/dawn" >"$log_file" 2>&1 &
   local dawn_pid=$!

   # Poll for setup token with timeout
   local token="" timeout=30 elapsed=0
   while [ $elapsed -lt $timeout ]; do
      token=$(grep -oP 'Token: \K[A-Z0-9-]+' "$log_file" 2>/dev/null || true)
      [ -n "$token" ] && break

      # Check daemon is still alive
      if ! kill -0 "$dawn_pid" 2>/dev/null; then
         echo ""
         warn "Daemon exited during startup. Last 20 lines of output:"
         tail -20 "$log_file"
         rm -f "$log_file"
         error "Cannot create admin account — daemon failed to start"
      fi

      sleep 1
      ((++elapsed))
   done

   if [ -z "$token" ]; then
      # Daemon is running but no token appeared
      kill "$dawn_pid" 2>/dev/null
      wait "$dawn_pid" 2>/dev/null || true
      warn "Setup token not found after ${timeout}s. Last 20 lines:"
      tail -20 "$log_file"
      rm -f "$log_file"
      error "Cannot create admin account — setup token not found"
   fi

   log "Setup token obtained: $token"

   # Wait for admin socket to be ready (daemon may still be initializing)
   local ready=false ping_timeout=10 ping_elapsed=0
   while [ $ping_elapsed -lt $ping_timeout ]; do
      if run_dawn "$BUILD_DIR/dawn-admin" ping >/dev/null 2>&1; then
         ready=true
         break
      fi
      sleep 1
      ((++ping_elapsed))
   done

   if [ "$ready" != true ]; then
      kill "$dawn_pid" 2>/dev/null
      wait "$dawn_pid" 2>/dev/null || true
      rm -f "$log_file"
      warn "Daemon started but admin socket not ready after ${ping_timeout}s"
      warn "Create admin manually: dawn-admin user create admin --admin"
      log "Phase 8 complete (admin not created)"
      return 0
   fi

   log "Creating admin account..."

   # Generate a 16-char alphanumeric password. Read /dev/urandom as an UNBOUNDED
   # stream so the alnum filter always reaches 16 chars: only ~24% of random
   # bytes are [A-Za-z0-9], so a fixed 64-byte block averages ~15.5 survivors
   # and falls short of 16 about half the time (and under the 8-char minimum
   # occasionally, which dawn-admin then rejects). `head -c 16` closing the pipe
   # sends SIGPIPE to `tr`; `|| true` keeps that from tripping `set -o pipefail`.
   local admin_pass
   admin_pass=$(LC_ALL=C tr -dc 'A-Za-z0-9' </dev/urandom | head -c 16) || true

   if DAWN_SETUP_TOKEN="$token" DAWN_PASSWORD="$admin_pass" \
      run_dawn "$BUILD_DIR/dawn-admin" user create admin --admin; then
      # Store password globally so the final summary can reprint it
      INSTALL_ADMIN_PASS="$admin_pass"
      show_admin_credentials "admin" "$admin_pass"
   else
      warn "Failed to create admin account"
      # Show DB init errors (often early in startup, missed by tail)
      if grep -q "ERR.*auth_db\|Failed to initialize database" "$log_file" 2>/dev/null; then
         warn "Database initialization errors found:"
         grep "ERR.*auth_db\|Failed to initialize database\|Hint:" "$log_file" 2>/dev/null
      fi
      warn "Last 10 lines of daemon output:"
      tail -10 "$log_file"
      warn "Create admin manually after starting DAWN:"
      warn "  dawn-admin user create admin --admin"
   fi

   # Clean shutdown — ensure daemon is fully stopped before service deploy
   kill "$dawn_pid" 2>/dev/null
   # Wait up to 10s for graceful shutdown
   local shutdown_tries=0
   while kill -0 "$dawn_pid" 2>/dev/null && [ $shutdown_tries -lt 10 ]; do
      sleep 1
      ((++shutdown_tries))
   done
   # Force kill if still running
   if kill -0 "$dawn_pid" 2>/dev/null; then
      warn "Daemon did not stop gracefully — sending SIGKILL"
      kill -9 "$dawn_pid" 2>/dev/null || true
   fi
   wait "$dawn_pid" 2>/dev/null || true
   rm -f "$log_file"

   log "Phase 8 complete"
}

# ─────────────────────────────────────────────────────────────────────────────
# CUDA environment file configuration
# ─────────────────────────────────────────────────────────────────────────────

# After service deploy, enable CUDA paths in the environment file if GPU is present.
configure_cuda_env_file() {
   local env_file="$1"

   if [ "${ONNX_HAS_CUDA:-false}" = false ] && [ "$HAS_CUDA" = false ]; then
      return 0
   fi

   if [ ! -f "$env_file" ]; then
      return 0
   fi

   log "CUDA detected — configuring GPU library paths in $env_file"

   # Build the LD_LIBRARY_PATH with CUDA paths
   local ld_path="/usr/local/lib"

   # Add CUDA toolkit library path
   if [ -d /usr/local/cuda/lib64 ]; then
      ld_path="${ld_path}:/usr/local/cuda/lib64"
   fi

   # Add Jetson Tegra libraries (only on Jetson)
   if [ "$PLATFORM" = "jetson" ]; then
      local tegra_lib="/usr/lib/aarch64-linux-gnu/tegra"
      if [ -d "$tegra_lib" ]; then
         ld_path="${ld_path}:${tegra_lib}"
      fi
   fi

   # Update the LD_LIBRARY_PATH line (replace the default one)
   sudo_begin_phase "cuda-env"
   run_sudo sed -i "s|^LD_LIBRARY_PATH=.*|LD_LIBRARY_PATH=${ld_path}|" "$env_file"

   # Uncomment CUDA_VISIBLE_DEVICES if it's commented out
   run_sudo sed -i 's/^#CUDA_VISIBLE_DEVICES=/CUDA_VISIBLE_DEVICES=/' "$env_file"

   # Restart service to pick up the new environment
   # Derive service name from env filename (dawn-server.conf → dawn-server)
   local service_name
   service_name=$(basename "$env_file" .conf)
   if systemctl is-active --quiet "$service_name" 2>/dev/null; then
      log "Restarting $service_name to apply CUDA configuration..."
      run_sudo systemctl restart "$service_name"
      sleep 3
   fi

   log "CUDA environment configured: $ld_path"
}

# ─────────────────────────────────────────────────────────────────────────────
# Deploy as systemd service
# ─────────────────────────────────────────────────────────────────────────────

run_deploy() {
   local target="$1"

   case "$target" in
      server)
         header "Deploy: DAWN Server Service"
         local script="$PROJECT_ROOT/services/dawn-server/install.sh"
         if [ ! -f "$script" ]; then
            error "Service install script not found: $script"
         fi

         # Build deploy arguments from what we know
         local deploy_args=()
         if [ -n "${BUILD_DIR:-}" ] && [ -f "$BUILD_DIR/dawn" ]; then
            deploy_args+=(--binary "$BUILD_DIR/dawn")
         fi
         if [ -d "$PROJECT_ROOT/models" ]; then
            deploy_args+=(--models-dir "$PROJECT_ROOT/models")
         fi
         if [ -d "$PROJECT_ROOT/www" ]; then
            deploy_args+=(--www-dir "$PROJECT_ROOT/www")
         fi
         if [ -d "$PROJECT_ROOT/ssl" ]; then
            deploy_args+=(--ssl-dir "$PROJECT_ROOT/ssl")
         fi
         if [ -f "$PROJECT_ROOT/dawn.toml" ]; then
            deploy_args+=(--config "$PROJECT_ROOT/dawn.toml")
         fi
         if [ -f "$PROJECT_ROOT/secrets.toml" ]; then
            deploy_args+=(--secrets "$PROJECT_ROOT/secrets.toml")
         fi

         log "Running: sudo $script ${deploy_args[*]}"

         if [ "$SUDO_PASSWORDLESS" = true ]; then
            run_sudo bash "$script" "${deploy_args[@]}"
         else
            log "The service installer requires root. You will be prompted for your password."
            sudo bash "$script" "${deploy_args[@]}"
         fi

         # If CUDA is available, update the environment file with GPU library paths
         configure_cuda_env_file "/usr/local/etc/dawn/dawn-server.conf"
         ;;

      satellite)
         header "Deploy: DAWN Satellite Service"
         local script="$PROJECT_ROOT/services/dawn-satellite/install.sh"
         if [ ! -f "$script" ]; then
            error "Service install script not found: $script"
         fi

         local deploy_args=()
         # Satellite binary could be in multiple locations
         for bin_path in \
            "$PROJECT_ROOT/dawn_satellite/build/dawn_satellite" \
            "$PROJECT_ROOT/build-debug/dawn_satellite/dawn_satellite" \
            "$PROJECT_ROOT/build/dawn_satellite/dawn_satellite"; do
            if [ -f "$bin_path" ]; then
               deploy_args+=(--binary "$bin_path")
               break
            fi
         done
         if [ -d "$PROJECT_ROOT/models" ]; then
            deploy_args+=(--models-dir "$PROJECT_ROOT/models")
         fi
         # If run_configure_satellite has written a configured satellite.toml
         # in the build tree, pass it to the service installer. Otherwise the
         # installer falls back to the default template (which has
         # host=localhost and engine=vosk — almost never what the user wants).
         local configured_toml="$PROJECT_ROOT/dawn_satellite/build/satellite.toml"
         if [ -f "$configured_toml" ]; then
            deploy_args+=(--config "$configured_toml")
         fi
         # Pass through the daemon's ca.crt when SSL + verify are on.
         # Service installer copies this to /etc/dawn/ca.crt.
         if [ -n "${SAT_CA_CERT_SRC:-}" ] && [ -f "$SAT_CA_CERT_SRC" ]; then
            deploy_args+=(--ca-cert "$SAT_CA_CERT_SRC")
         fi

         log "Running: sudo $script ${deploy_args[*]}"

         if [ "$SUDO_PASSWORDLESS" = true ]; then
            run_sudo bash "$script" "${deploy_args[@]}"
         else
            log "The service installer requires root. You will be prompted for your password."
            sudo bash "$script" "${deploy_args[@]}"
         fi
         ;;

      *)
         error "Unknown deploy target: $target (use 'server' or 'satellite')"
         ;;
   esac

   # Post-deploy verification
   verify_deployed_service "$target"
}

verify_deployed_service() {
   local target="$1"
   local service_name

   case "$target" in
      server) service_name="dawn-server" ;;
      satellite) service_name="dawn-satellite" ;;
   esac

   header "Post-Deploy Verification: $service_name"

   local pass=0 fail=0

   # Check 1: Service unit file exists
   if [ -f "/etc/systemd/system/${service_name}.service" ]; then
      log "  Service unit file: EXISTS"
      ((++pass))
   else
      warn "  Service unit file: MISSING"
      ((++fail))
   fi

   # Check 2: Service is enabled
   if systemctl is-enabled --quiet "$service_name" 2>/dev/null; then
      log "  Service enabled: YES"
      ((++pass))
   else
      warn "  Service enabled: NO"
      ((++fail))
   fi

   # Check 3: Service is active
   if systemctl is-active --quiet "$service_name" 2>/dev/null; then
      log "  Service active: YES"
      ((++pass))
   else
      warn "  Service active: NO"
      ((++fail))
      warn "  Check logs: journalctl -u $service_name -n 30"
   fi

   # Check 4: Binary installed
   local bin_path
   case "$target" in
      server) bin_path="/usr/local/bin/dawn" ;;
      satellite) bin_path="/usr/local/bin/dawn_satellite" ;;
   esac
   if [ -f "$bin_path" ]; then
      log "  Binary: $bin_path"
      ((++pass))
   else
      warn "  Binary: NOT FOUND at $bin_path"
      ((++fail))
   fi

   # Check 5: Config exists
   local config_dir
   case "$target" in
      server) config_dir="/usr/local/etc/dawn" ;;
      satellite) config_dir="/usr/local/etc/dawn-satellite" ;;
   esac
   if [ -d "$config_dir" ]; then
      log "  Config dir: $config_dir"
      ((++pass))
   else
      warn "  Config dir: NOT FOUND"
      ((++fail))
   fi

   # Check 6: Secrets permissions (server only)
   if [ "$target" = "server" ]; then
      local secrets_path="$config_dir/secrets.toml"
      if [ -f "$secrets_path" ]; then
         local perms
         perms=$(stat -c '%a' "$secrets_path" 2>/dev/null || echo "???")
         if [ "$perms" = "600" ]; then
            log "  Secrets permissions: 600 (correct)"
            ((++pass))
         else
            warn "  Secrets permissions: $perms (should be 600)"
            ((++fail))
         fi
      fi
   fi

   # Check 7: Logs directory
   local log_dir
   case "$target" in
      server) log_dir="/var/log/dawn" ;;
      satellite) log_dir="/var/log/dawn-satellite" ;;
   esac
   if [ -d "$log_dir" ]; then
      log "  Log dir: $log_dir"
      ((++pass))
   else
      warn "  Log dir: NOT FOUND"
      ((++fail))
   fi

   # Check 8: Recent log output (if service is active)
   if systemctl is-active --quiet "$service_name" 2>/dev/null; then
      local log_file
      case "$target" in
         server) log_file="$log_dir/server.log" ;;
         satellite) log_file="$log_dir/satellite.log" ;;
      esac
      if [ -f "$log_file" ] && [ -s "$log_file" ]; then
         local errors
         errors=$(grep -ci "\[ERR\]" "$log_file" 2>/dev/null) || errors=0
         if [ "$errors" -eq 0 ]; then
            log "  Log health: clean (no errors)"
            ((++pass))
         else
            warn "  Log health: $errors error(s) found"
            warn "  Review: tail -30 $log_file"
            ((++fail))
         fi
      else
         info "  Log health: no log output yet"
      fi
   fi

   # Check 9: WebUI port responding (server only, if HTTPS)
   if [ "$target" = "server" ]; then
      sleep 2
      local port=3000
      if curl -sk --max-time 5 "https://localhost:$port/" >/dev/null 2>&1 ||
         curl -s --max-time 5 "http://localhost:$port/" >/dev/null 2>&1; then
         log "  WebUI port $port: RESPONDING"
         ((++pass))
      else
         warn "  WebUI port $port: NOT RESPONDING (may still be starting)"
         ((++fail))
      fi
   fi

   # Check 10: ldconfig entry
   if [ -f /etc/ld.so.conf.d/dawn.conf ]; then
      log "  ldconfig: configured"
      ((++pass))
   else
      warn "  ldconfig: /etc/ld.so.conf.d/dawn.conf missing"
      ((++fail))
   fi

   echo ""
   log "Post-deploy: $pass passed, $fail failed"

   if [ "$fail" -eq 0 ]; then
      echo ""
      log "$service_name is running!"
      echo ""
      log "Management commands:"
      echo "  sudo systemctl status $service_name"
      echo "  sudo systemctl restart $service_name"
      echo "  sudo journalctl -u $service_name -f"
      if [ "$target" = "server" ]; then
         local ip
         ip=$(hostname -I 2>/dev/null | awk '{print $1}' || echo "localhost")
         echo ""
         log "WebUI: https://${ip}:3000"
      fi
   else
      warn "Some post-deploy checks failed — review above"
   fi
}

# ─────────────────────────────────────────────────────────────────────────────
# Main orchestration
# ─────────────────────────────────────────────────────────────────────────────

main() {
   parse_args "$@"

   echo ""
   echo -e "${BOLD}╔═══════════════════════════════════════════════╗${NC}"
   echo -e "${BOLD}║       D.A.W.N. Installation Script            ║${NC}"
   echo -e "${BOLD}╚═══════════════════════════════════════════════╝${NC}"
   echo ""

   # Verify-only mode
   if [ "$VERIFY_ONLY" = true ]; then
      run_discovery
      run_verify
      exit $?
   fi

   # Uninstall mode
   if [ "$UNINSTALL" = true ]; then
      run_uninstall
      exit $?
   fi

   # Deploy-only mode (delegates to existing service install scripts)
   if [ -n "$DEPLOY_TARGET" ]; then
      run_discovery
      run_deploy "$DEPLOY_TARGET"
      exit $?
   fi

   # Load config file if specified
   if [ -n "$CONFIG_FILE" ]; then
      load_config_file "$CONFIG_FILE"
   fi

   # Load cached settings from a previous run (unless --fresh)
   if [ "$FRESH_INSTALL" = true ]; then
      if [ -f "$STATE_FILE" ]; then
         info "Ignoring cached settings (--fresh)"
         rm -f "$STATE_FILE"
      fi
      SETTINGS_LOADED=false
   elif load_state; then
      echo -e "${BLUE}[INFO]${NC} Loaded settings from previous install (last phase: ${LAST_COMPLETED_PHASE:-none})"
      echo -e "       Target: ${INSTALL_TARGET:-unset}"
      echo -e "       Provider: ${LLM_PROVIDER:-unset}, Whisper: ${WHISPER_MODEL:-unset}, Preset: ${BUILD_PRESET:-unset}"
      echo -e "       Features: ${FEATURES:-none}"
      echo ""
      # If the previous install completed fully, ask whether to reuse settings
      if [ "$INTERACTIVE" = true ] && [ "${LAST_COMPLETED_PHASE:-}" = "verify" ]; then
         if ! ask_yes_no "Use these cached settings?"; then
            info "Re-entering installation options..."
            # Clear loaded settings so the target prompt + present_choices
            # re-run. Using `unset ${!SAT_*}` means we don't have to keep
            # this list in sync with save_state's SAT_* block manually.
            INSTALL_TARGET=""
            LLM_PROVIDER="" PRIMARY_PROVIDER="" WHISPER_MODEL="" BUILD_PRESET="" FEATURES=""
            # shellcheck disable=SC2086
            unset ${!SAT_*}
            SETTINGS_LOADED=false
            rm -f "$STATE_FILE"
         fi
      else
         echo -e "       To start over: ${BOLD}./scripts/install.sh --fresh${NC}"
      fi
      echo ""
   fi

   # Load state for resume logic
   if [ -n "$RESUME_FROM" ] && [ "$SETTINGS_LOADED" = false ]; then
      warn "No state file found — running from $RESUME_FROM anyway"
   fi

   # Resolve install target. Source of truth order:
   #   1. --target CLI flag (set by parse_args)
   #   2. INSTALL_TARGET from config file (loaded via --config)
   #   3. INSTALL_TARGET from cached state (load_state)
   #   4. Interactive prompt (if none of the above set it and --unattended is off)
   #   5. Fall back to "server" in unattended mode
   if [ -z "${INSTALL_TARGET:-}" ]; then
      if [ "$INTERACTIVE" = true ]; then
         echo ""
         echo "Install target:"
         echo "  server     Full DAWN daemon — LLM, WebUI, tools, MQTT. The main device."
         echo "  satellite  Tier 1 voice satellite for Raspberry Pi. Connects to a running server."
         echo ""
         INSTALL_TARGET=$(ask_value "Target" "server")
         case "$INSTALL_TARGET" in
            server | satellite) ;;
            *) error "Invalid target: $INSTALL_TARGET (must be 'server' or 'satellite')" ;;
         esac
         echo ""
      else
         INSTALL_TARGET="server"
         info "No --target specified — defaulting to 'server'"
      fi
   fi

   # Phase 0: Discovery
   run_discovery

   # Interactive preferences (skip if settings already loaded from cache/config/CLI)
   if [ "$INTERACTIVE" = true ] && [ -z "$RESUME_FROM" ] && [ "$SETTINGS_LOADED" = false ]; then
      if [ "$INSTALL_TARGET" = "satellite" ]; then
         run_satellite_discovery
         present_satellite_choices
      else
         present_choices
      fi
   fi

   # Set defaults for any unset preferences (server-only — satellite sets its
   # own SAT_* defaults inside present_satellite_choices)
   if [ "$INSTALL_TARGET" != "satellite" ]; then
      : "${LLM_PROVIDER:=claude}"
      : "${PRIMARY_PROVIDER:=${LLM_PROVIDER%%,*}}"
      : "${WHISPER_MODEL:=base}"
      : "${BUILD_PRESET:=default}"
      : "${FEATURES:=ssl,mqtt}"
   fi

   save_state "discovery"

   # Phase 1: System Dependencies
   if should_run_phase "deps"; then
      if [ "$INSTALL_TARGET" = "satellite" ]; then
         run_deps_satellite
      else
         run_deps
      fi
      save_state "deps"
   else
      info "Skipping Phase 1 (deps)"
   fi

   # Phase 2: Core Libraries
   if should_run_phase "libs"; then
      if [ "$INSTALL_TARGET" = "satellite" ]; then
         run_libs_satellite
      else
         run_libs
      fi
      save_state "libs"
   else
      info "Skipping Phase 2 (libs)"
   fi

   # Phase 3: Build
   if should_run_phase "build"; then
      if [ "$INSTALL_TARGET" = "satellite" ]; then
         run_build_satellite
      else
         run_build
      fi
      save_state "build"
   else
      info "Skipping Phase 3 (build)"
   fi

   # Phase 4: Models
   if should_run_phase "models"; then
      if [ "$INSTALL_TARGET" = "satellite" ]; then
         run_models_satellite
      else
         run_models
      fi
      save_state "models"
   else
      info "Skipping Phase 4 (models)"
   fi

   # Phase 5: Configure
   if should_run_phase "configure"; then
      if [ "$INSTALL_TARGET" = "satellite" ]; then
         run_configure_satellite
      else
         run_configure
      fi
      save_state "configure"
   else
      info "Skipping Phase 5 (configure)"
   fi

   # Phases 6–9 are server-only (apikeys, ssl, admin, services).
   if [ "$INSTALL_TARGET" != "satellite" ]; then
      # Phase 6: API Key Validation
      if should_run_phase "apikeys"; then
         run_apikeys
         save_state "apikeys"
      else
         info "Skipping Phase 6 (apikeys)"
      fi

      # Phase 7: SSL
      if should_run_phase "ssl"; then
         run_ssl
         save_state "ssl"
      else
         info "Skipping Phase 7 (ssl)"
      fi

      # Phase 8: Admin Account
      if should_run_phase "admin"; then
         run_admin
         save_state "admin"
      else
         info "Skipping Phase 8 (admin)"
      fi

      # Phase 9: Optional Features
      if should_run_phase "services"; then
         run_services
         save_state "services"
      else
         info "Skipping Phase 9 (services)"
      fi
   else
      info "Skipping Phases 6–9 (apikeys/ssl/admin/services — not applicable to satellites)"
   fi

   # Phase 10: Verification
   if should_run_phase "verify"; then
      if [ "$INSTALL_TARGET" = "satellite" ]; then
         run_verify_satellite || true
      else
         run_verify || true
      fi
      save_state "verify"
   fi

   # Phase 11: Deploy (systemd service install)
   #   - Satellite: always run (satellite is useless without deploy)
   #   - Server interactive: ask
   #   - Server unattended: skip (preserve prior behavior — dev-install via binary)
   if should_run_phase "deploy"; then
      echo ""
      echo -e "${BOLD}═══════════════════════════════════════════════${NC}"
      echo ""
      if [ "$INSTALL_TARGET" = "satellite" ]; then
         run_deploy "satellite"
      elif [ "$INTERACTIVE" = true ]; then
         if ask_yes_no "Deploy DAWN as a systemd service?"; then
            run_deploy "server"
         else
            echo ""
            log "To deploy as a service later:"
            echo "  ./scripts/install.sh --deploy server"
            echo "  # or: sudo ./services/dawn-server/install.sh"
         fi
      else
         info "Skipping deploy in unattended mode — run: ./scripts/install.sh --deploy server"
      fi
      save_state "deploy"
   fi

   echo ""
   log "Installation complete!"

   # Reprint admin credentials LAST, on every path (interactive/unattended,
   # server/satellite, deploy-or-not), so the one-time password is the final
   # thing on screen instead of scrolled off above the verify/deploy output.
   if [ -n "${INSTALL_ADMIN_PASS:-}" ]; then
      show_admin_credentials "admin" "$INSTALL_ADMIN_PASS"
   fi
}

main "$@"
