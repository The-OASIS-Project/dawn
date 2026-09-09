#!/bin/bash
#
# DAWN Installation - Phase 10: Verification Suite
# Comprehensive check table for all components
#
# Sourced by install.sh. Do not execute directly.
#

# ─────────────────────────────────────────────────────────────────────────────
# Verification check functions
# Each prints "STATUS|DETAIL" where STATUS is PASS, FAIL, or SKIP
# ─────────────────────────────────────────────────────────────────────────────

check_binary() {
   if [ -n "${BUILD_DIR:-}" ] && [ -f "$BUILD_DIR/dawn" ]; then
      local size
      size=$(du -sh "$BUILD_DIR/dawn" | cut -f1)
      echo "PASS|$BUILD_DIR/dawn ($size)"
   else
      # Search common build dirs
      for dir in build build-debug build-release build-local build-full build-server; do
         if [ -f "$PROJECT_ROOT/$dir/dawn" ]; then
            BUILD_DIR="$PROJECT_ROOT/$dir"
            local size
            size=$(du -sh "$BUILD_DIR/dawn" | cut -f1)
            echo "PASS|$BUILD_DIR/dawn ($size)"
            return
         fi
      done
      echo "FAIL|Not found in any build directory"
   fi
}

check_binary_runs() {
   if [ -z "${BUILD_DIR:-}" ] || [ ! -f "$BUILD_DIR/dawn" ]; then
      echo "SKIP|No binary found"
      return
   fi
   if run_dawn "$BUILD_DIR/dawn" --dump-config >/dev/null 2>&1; then
      echo "PASS|Runs successfully"
   else
      echo "FAIL|Binary exists but failed to run"
   fi
}

check_dawn_admin() {
   if [ -n "${BUILD_DIR:-}" ] && [ -f "$BUILD_DIR/dawn-admin" ]; then
      echo "PASS|$BUILD_DIR/dawn-admin"
   else
      echo "FAIL|Not found"
   fi
}

check_config_loads() {
   if [ ! -f "$PROJECT_ROOT/dawn.toml" ]; then
      echo "FAIL|dawn.toml not found"
      return
   fi
   if [ -n "${BUILD_DIR:-}" ] && [ -f "$BUILD_DIR/dawn" ]; then
      if run_dawn "$BUILD_DIR/dawn" --dump-config >/dev/null 2>&1; then
         echo "PASS|dawn.toml parses correctly"
      else
         echo "FAIL|dawn.toml exists but --dump-config failed"
      fi
   else
      echo "PASS|dawn.toml exists (binary not available to test)"
   fi
}

check_whisper_model() {
   local f
   for f in "$PROJECT_ROOT"/models/whisper.cpp/ggml-*.bin; do
      if [ -f "$f" ]; then
         local name size
         name=$(basename "$f")
         size=$(du -sh "$f" | cut -f1)
         echo "PASS|$name ($size)"
         return
      fi
   done
   echo "FAIL|No whisper model found"
}

check_tts_models() {
   local count=0
   local f
   for f in "$PROJECT_ROOT"/models/*.onnx; do
      [ -f "$f" ] && ((++count))
   done
   if [ "$count" -gt 0 ]; then
      echo "PASS|$count voice model(s)"
   else
      echo "FAIL|No TTS models found"
   fi
}

check_vad_model() {
   local f
   for f in "$PROJECT_ROOT"/models/silero_vad*.onnx; do
      [ -f "$f" ] && echo "PASS|Present" && return
   done
   echo "FAIL|silero_vad*.onnx not found"
}

check_secrets_permissions() {
   if [ ! -f "$PROJECT_ROOT/secrets.toml" ]; then
      echo "FAIL|secrets.toml not found"
      return
   fi
   local perms
   perms=$(stat -c '%a' "$PROJECT_ROOT/secrets.toml" 2>/dev/null || echo "???")
   if [ "$perms" = "600" ]; then
      echo "PASS|Mode 600"
   else
      echo "FAIL|Mode $perms (should be 600)"
   fi
}

check_api_key() {
   local provider="$1" key="$2"
   if [ -z "$key" ]; then
      echo "SKIP|Not configured"
      return
   fi
   local status=""

   case "$provider" in
      openai)
         status=$(curl -s -o /dev/null -w "%{http_code}" --max-time 10 \
            -H "Authorization: Bearer $key" \
            https://api.openai.com/v1/models 2>/dev/null || echo "000")
         ;;
      claude)
         status=$(curl -s -o /dev/null -w "%{http_code}" --max-time 10 \
            -H "x-api-key: $key" \
            -H "anthropic-version: 2023-06-01" \
            -H "content-type: application/json" \
            -d '{"model":"claude-haiku-4-5-20251001","max_tokens":1,"messages":[{"role":"user","content":"hi"}]}' \
            https://api.anthropic.com/v1/messages 2>/dev/null || echo "000")
         ;;
      gemini)
         status=$(curl -s -o /dev/null -w "%{http_code}" --max-time 10 \
            "https://generativelanguage.googleapis.com/v1beta/models?key=$key" 2>/dev/null || echo "000")
         ;;
   esac

   if [ "$status" = "200" ]; then
      echo "PASS|Valid"
   elif [ "$status" = "000" ]; then
      echo "FAIL|Unreachable"
   else
      echo "FAIL|HTTP $status"
   fi
}

check_ssl_certs() {
   if [ ! -f "$PROJECT_ROOT/ssl/dawn.crt" ]; then
      echo "SKIP|Not configured"
      return
   fi
   if [ ! -s "$PROJECT_ROOT/ssl/dawn.crt" ]; then
      echo "FAIL|Certificate file is empty (run ./generate_ssl_cert.sh)"
      return
   fi
   local expiry
   expiry=$(openssl x509 -in "$PROJECT_ROOT/ssl/dawn.crt" -noout -enddate 2>/dev/null |
      sed 's/notAfter=//' || echo "")
   if [ -n "$expiry" ]; then
      # Check if expired
      if openssl x509 -in "$PROJECT_ROOT/ssl/dawn.crt" -noout -checkend 0 >/dev/null 2>&1; then
         echo "PASS|Valid until $expiry"
      else
         echo "FAIL|EXPIRED ($expiry)"
      fi
   else
      echo "FAIL|Cannot parse certificate (run ./generate_ssl_cert.sh)"
   fi
}

check_audio_capture() {
   if has_command arecord && arecord -l 2>/dev/null | grep -q "card"; then
      local device
      device=$(arecord -l 2>/dev/null | grep "card" | head -1 | sed 's/^  //')
      echo "PASS|$device"
   else
      echo "FAIL|No capture device"
   fi
}

check_audio_playback() {
   if has_command aplay && aplay -l 2>/dev/null | grep -q "card"; then
      local device
      device=$(aplay -l 2>/dev/null | grep "card" | head -1 | sed 's/^  //')
      echo "PASS|$device"
   else
      echo "FAIL|No playback device"
   fi
}

check_admin_account() {
   # Dev/foreground installs keep the DB under the user's XDG data dir; the
   # systemd service sets data_dir = /var/lib/dawn/db (see
   # services/dawn-server/install.sh). Check both so verify matches the
   # Phase 8 existence guard in install.sh.
   local dev_db="$HOME/.local/share/dawn/auth.db"
   local service_db="/var/lib/dawn/db/auth.db"
   if [ -f "$dev_db" ]; then
      echo "PASS|auth.db exists ($dev_db)"
   elif [ -f "$service_db" ]; then
      echo "PASS|auth.db exists ($service_db)"
   else
      echo "SKIP|No auth database (admin not created yet)"
   fi
}

check_searxng() {
   if ! echo "${FEATURES:-}" | grep -q "searxng"; then
      echo "SKIP|Not configured"
      return
   fi
   # Reachability alone is NOT enough: a SearXNG whose upstream engines all time out
   # (default request_timeout too tight — see GETTING_STARTED.md settings.yml) returns
   # HTTP 200 with an EMPTY results array, which passed this check while silently
   # returning nothing (and breaking the Tavily->SearXNG fallback). So verify results.
   local body
   body="$(curl -sf "http://localhost:8384/search?q=wikipedia&format=json" 2>/dev/null)" || {
      echo "FAIL|Not reachable"
      return
   }
   if ! echo "$body" | grep -q '"url"'; then
      echo "FAIL|Reachable but 0 results — engines timing out? add outgoing.request_timeout (GETTING_STARTED.md)"
   else
      echo "PASS|Reachable + returning results (port 8384)"
   fi
}

check_flaresolverr() {
   if ! echo "${FEATURES:-}" | grep -q "flaresolverr"; then
      echo "SKIP|Not configured"
      return
   fi
   if curl -sf http://localhost:8191/health >/dev/null 2>&1; then
      echo "PASS|Healthy (port 8191)"
   else
      echo "FAIL|Not reachable"
   fi
}

check_mqtt() {
   if ! echo "${FEATURES:-}" | grep -q "mqtt"; then
      echo "SKIP|Not configured"
      return
   fi
   if systemctl is-active --quiet mosquitto 2>/dev/null; then
      echo "PASS|Active"
   else
      echo "FAIL|Not running"
   fi
}

check_homeassistant() {
   if ! echo "${FEATURES:-}" | grep -q "homeassistant"; then
      echo "SKIP|Not configured"
      return
   fi
   echo "SKIP|Token not validated (configure in secrets.toml)"
}

check_submodules() {
   if [ -d "$PROJECT_ROOT/whisper.cpp/.git" ] || [ -f "$PROJECT_ROOT/whisper.cpp/.git" ]; then
      echo "PASS|Initialized"
   else
      echo "FAIL|Not initialized"
   fi
}

check_service_server() {
   if systemctl is-active --quiet dawn-server 2>/dev/null; then
      echo "PASS|Active"
   elif systemctl is-enabled --quiet dawn-server 2>/dev/null; then
      echo "FAIL|Enabled but not active"
   else
      echo "SKIP|Not installed"
   fi
}

check_service_satellite() {
   if systemctl is-active --quiet dawn-satellite 2>/dev/null; then
      echo "PASS|Active"
   elif systemctl is-enabled --quiet dawn-satellite 2>/dev/null; then
      echo "FAIL|Enabled but not active"
   else
      echo "SKIP|Not installed"
   fi
}

# ─────────────────────────────────────────────────────────────────────────────
# Verification table renderer
# ─────────────────────────────────────────────────────────────────────────────

run_verify() {
   header "Verification Suite"
   CURRENT_PHASE="verify"

   # Read API keys from secrets.toml for validation (without exposing them)
   local openai_key="" claude_key="" gemini_key=""
   if [ -f "$PROJECT_ROOT/secrets.toml" ]; then
      openai_key=$(grep -oP '^openai_api_key\s*=\s*"\K[^"]+' "$PROJECT_ROOT/secrets.toml" 2>/dev/null || echo "")
      claude_key=$(grep -oP '^claude_api_key\s*=\s*"\K[^"]+' "$PROJECT_ROOT/secrets.toml" 2>/dev/null || echo "")
      gemini_key=$(grep -oP '^gemini_api_key\s*=\s*"\K[^"]+' "$PROJECT_ROOT/secrets.toml" 2>/dev/null || echo "")
   fi

   # Define checks: "Label|function|args..."
   local -a checks=(
      "Binary exists|check_binary"
      "Binary runs|check_binary_runs"
      "dawn-admin|check_dawn_admin"
      "Config loads|check_config_loads"
      "Whisper model|check_whisper_model"
      "TTS models|check_tts_models"
      "VAD model|check_vad_model"
      "secrets.toml perms|check_secrets_permissions"
      "API: OpenAI|check_api_key|openai|$openai_key"
      "API: Claude|check_api_key|claude|$claude_key"
      "API: Gemini|check_api_key|gemini|$gemini_key"
      "SSL certificates|check_ssl_certs"
      "Audio capture|check_audio_capture"
      "Audio playback|check_audio_playback"
      "Admin account|check_admin_account"
      "SearXNG|check_searxng"
      "FlareSolverr|check_flaresolverr"
      "MQTT broker|check_mqtt"
      "Home Assistant|check_homeassistant"
      "Git submodules|check_submodules"
      "dawn-server.service|check_service_server"
      "dawn-satellite.service|check_service_satellite"
   )

   local pass=0 fail=0 skip=0

   printf "  %-24s %-8s %s\n" "Check" "Status" "Details"
   printf "  %-24s %-8s %s\n" "─────" "──────" "───────"

   for entry in "${checks[@]}"; do
      IFS='|' read -r label func args1 args2 <<<"$entry"

      local result nargs
      # Count pipe-delimited fields to determine argument count
      nargs=$(awk -F'|' '{print NF}' <<<"$entry")
      if [ "$nargs" -ge 4 ]; then
         result=$($func "${args1:-}" "${args2:-}" 2>/dev/null || echo "FAIL|Error running check")
      elif [ "$nargs" -ge 3 ]; then
         result=$($func "${args1:-}" 2>/dev/null || echo "FAIL|Error running check")
      else
         result=$($func 2>/dev/null || echo "FAIL|Error running check")
      fi

      local status="${result%%|*}"
      local detail="${result#*|}"

      local color
      case "$status" in
         PASS) color="$GREEN"; ((++pass)) ;;
         FAIL) color="$RED"; ((++fail)) ;;
         SKIP) color="$YELLOW"; ((++skip)) ;;
         *) color="$RED"; status="FAIL"; ((++fail)) ;;
      esac

      printf "  %-24s ${color}%-8s${NC} %s\n" "$label" "$status" "$detail"
   done

   echo ""
   log "Results: ${GREEN}$pass passed${NC}, ${RED}$fail failed${NC}, ${YELLOW}$skip skipped${NC}"

   # Print start instructions if binary exists
   if [ -n "${BUILD_DIR:-}" ] && [ -f "$BUILD_DIR/dawn" ]; then
      echo ""
      log "How to start DAWN:"
      echo ""
      echo "  cd $PROJECT_ROOT"
      if [ "$LDCONFIG_DONE" = true ]; then
         echo "  $BUILD_DIR/dawn"
      else
         echo "  LD_LIBRARY_PATH=/usr/local/lib $BUILD_DIR/dawn"
      fi

      if [ "${BUILD_PRESET:-default}" != "local" ]; then
         local proto="http"
         echo "${FEATURES:-}" | grep -q "ssl" && proto="https"
         local ip
         ip=$(hostname -I 2>/dev/null | awk '{print $1}' || echo "localhost")
         echo ""
         log "WebUI: ${proto}://${ip}:3000"
         if [ "$proto" = "https" ]; then
            info "Install ssl/ca.crt in your browser to avoid certificate warnings"
         fi
      fi

      echo ""
      log "Wake word: \"Hey Friday\" or \"Okay Friday\""
      echo ""

      # Deployment hint
      log "To deploy as a systemd service:"
      echo "  sudo ./services/dawn-server/install.sh"
   fi

   if [ "$fail" -gt 0 ]; then
      echo ""
      warn "Some checks failed — review the items above"
      return 1
   fi
   return 0
}
