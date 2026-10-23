#!/bin/bash
#
# DAWN Installation - Phases 5-6: Configuration and API Key Validation
# dawn.toml, secrets.toml, headless audio setup, API key testing
#
# Sourced by install.sh. Do not execute directly.
#

# ─────────────────────────────────────────────────────────────────────────────
# TOML editing helper — section-scoped sed
# ─────────────────────────────────────────────────────────────────────────────

# Set a value in a TOML file, scoped to a specific section.
# Handles both uncommenting existing lines and replacing values.
# Only works with single-line key = value patterns.
#
# Usage: sed_safe_set <file> <section> <key> <value>
# Example: sed_safe_set dawn.toml "llm" "type" "cloud"
# Escape characters that have meaning in sed's replacement string so a
# user-supplied value can't break out of the substitution. Specifically:
#   - backslash (must be first — it escapes everything else)
#   - & (sed reference to the matched text)
#   - | (our s-command delimiter)
#   - newline (would terminate the sed command; GNU sed's `e` flag on a
#     later line could then execute shell — refuse outright)
# Returns escaped string on stdout, or exits 1 if a newline is present.
_sed_escape_replacement() {
   local s="$1"
   if [[ "$s" == *$'\n'* ]]; then
      echo "_sed_escape_replacement: value contains newline — refusing" >&2
      return 1
   fi
   s="${s//\\/\\\\}"
   s="${s//&/\\&}"
   s="${s//|/\\|}"
   printf '%s' "$s"
}

sed_safe_set() {
   local file="$1" section="$2" key="$3" value="$4"

   # Determine if value needs quoting. TOML bools and numbers stay bare;
   # everything else is double-quoted as a TOML string.
   local quoted_value
   if [[ "$value" =~ ^(true|false|-?[0-9]+(\.[0-9]+)?)$ ]]; then
      quoted_value="$value"
   else
      quoted_value="\"$value\""
   fi

   # Escape sed metacharacters in the replacement before building the
   # sed command. Without this, a value containing |, &, or \ could
   # break out of the s/// form — and a newline could inject a separate
   # sed command entirely (including w/e which GNU sed can use to write
   # files or run shell). Key and section are regex-safe here because
   # they're controlled by code, not user input, and `section` has dots
   # escaped a few lines down.
   local sed_replacement
   sed_replacement=$(_sed_escape_replacement "$quoted_value") ||
      { echo "sed_safe_set: refusing unsafe value for $section.$key" >&2; return 1; }

   # Escape dots in section name for regex (e.g., "llm.cloud" → "llm\.cloud")
   local section_re="${section//./\\.}"

   # The sed range /^\[section\]/,/^\[/ scopes every edit to the section
   # (stops at the next section header), so keys that also appear in
   # other sections (e.g. `model_path` in [vad], [asr], [tts]) aren't
   # clobbered cross-section.
   if grep -qP "^\[${section_re}\]" "$file" 2>/dev/null; then
      # Case 1: the key already exists uncommented — replace its value.
      if sed -n "/^\[${section_re}\]/,/^\[/p" "$file" | grep -qP "^${key} *=" 2>/dev/null; then
         sed -i "/^\[${section_re}\]/,/^\[/{s|^${key} *=.*|${key} = ${sed_replacement}|}" "$file"
      # Case 2: the key exists only as a commented-out line — uncomment and set.
      elif sed -n "/^\[${section_re}\]/,/^\[/p" "$file" | grep -qP "^# *${key} *=" 2>/dev/null; then
         sed -i "/^\[${section_re}\]/,/^\[/{s|^# *${key} *=.*|${key} = ${sed_replacement}|}" "$file"
      # Case 3: the key isn't present at all under this section — append.
      # `a` takes a literal string, not a regex, but still interprets
      # backslash escapes. Use the already-escaped form.
      else
         sed -i "/^\[${section_re}\]/a ${key} = ${sed_replacement}" "$file"
      fi
   else
      # Section doesn't exist — append it with the key. Plain file write,
      # no sed interpretation needed so use the raw quoted_value.
      {
         echo ""
         echo "[$section]"
         echo "${key} = ${quoted_value}"
      } >>"$file"
   fi
}

# ─────────────────────────────────────────────────────────────────────────────
# Config file setup
# ─────────────────────────────────────────────────────────────────────────────

setup_config_files() {
   cd "$PROJECT_ROOT" || return

   # dawn.toml
   if [ -f "dawn.toml" ]; then
      log "dawn.toml: already exists (preserving)"
   else
      if [ -f "dawn.toml.example" ]; then
         cp dawn.toml.example dawn.toml
         log "dawn.toml: created from template"
      else
         error "dawn.toml.example not found"
      fi
   fi

   # secrets.toml — create securely with atomic permissions
   if [ -f "secrets.toml" ]; then
      log "secrets.toml: already exists (preserving)"
      # Ensure permissions are correct even on existing file
      chmod 600 secrets.toml
   else
      if [ -f "secrets.toml.example" ]; then
         install -m 600 /dev/null secrets.toml
         cat secrets.toml.example >secrets.toml
         log "secrets.toml: created with mode 600"
      else
         error "secrets.toml.example not found"
      fi
   fi
}

# ─────────────────────────────────────────────────────────────────────────────
# Write user config choices to dawn.toml
# ─────────────────────────────────────────────────────────────────────────────

write_user_config() {
   local config="$PROJECT_ROOT/dawn.toml"

   # LLM provider
   if echo "${LLM_PROVIDER:-}" | grep -q "local"; then
      sed_safe_set "$config" "llm" "type" "local"
   else
      sed_safe_set "$config" "llm" "type" "cloud"
      sed_safe_set "$config" "llm.cloud" "provider" "${PRIMARY_PROVIDER:-claude}"
   fi

   # WebUI (enabled for all presets except "local")
   if [ "${BUILD_PRESET:-default}" != "local" ]; then
      sed_safe_set "$config" "webui" "enabled" "true"
   fi

   log "dawn.toml: configured (LLM: ${PRIMARY_PROVIDER:-local}, preset: ${BUILD_PRESET:-default})"
}

# ─────────────────────────────────────────────────────────────────────────────
# Write API keys to secrets.toml
# ─────────────────────────────────────────────────────────────────────────────

# Top-level secrets.toml key setter. Value is escaped against sed
# replacement metacharacters (same rationale as sed_safe_set).
_write_top_level_key() {
   local file="$1" key="$2" value="$3"
   local escaped
   escaped=$(_sed_escape_replacement "$value") ||
      { warn "secrets.toml: refusing unsafe value for $key"; return 1; }
   # Uncomment-or-replace the assignment, matching either form
   sed -i "s|^# *${key} *=.*|${key} = \"${escaped}\"|" "$file"
   sed -i "s|^${key} *=.*|${key} = \"${escaped}\"|" "$file"
}

write_api_keys() {
   local secrets="$PROJECT_ROOT/secrets.toml"
   local keys_set=0

   if [ -n "${OPENAI_KEY:-}" ]; then
      _write_top_level_key "$secrets" openai_api_key "$OPENAI_KEY" &&
         { log "secrets.toml: OpenAI API key set"; ((++keys_set)); }
   fi
   if [ -n "${CLAUDE_KEY:-}" ]; then
      _write_top_level_key "$secrets" claude_api_key "$CLAUDE_KEY" &&
         { log "secrets.toml: Claude API key set"; ((++keys_set)); }
   fi
   if [ -n "${GEMINI_KEY:-}" ]; then
      _write_top_level_key "$secrets" gemini_api_key "$GEMINI_KEY" &&
         { log "secrets.toml: Gemini API key set"; ((++keys_set)); }
   fi

   if [ "$keys_set" -eq 0 ]; then
      info "No API keys provided — add them to secrets.toml later"
   fi

   # Ensure permissions
   chmod 600 "$secrets"
}

# ─────────────────────────────────────────────────────────────────────────────
# Headless audio setup (snd-dummy for systems without a microphone)
# ─────────────────────────────────────────────────────────────────────────────

setup_headless() {
   if [ "$HEADLESS" = false ]; then
      return 0
   fi

   log "Headless mode: no capture device detected"
   log "Loading snd-dummy kernel module for virtual audio..."

   sudo_begin_phase "headless"

   # Load module
   run_sudo modprobe snd-dummy 2>/dev/null || warn "Failed to load snd-dummy (may need kernel headers)"

   # Persist across reboots
   if [ ! -f /etc/modules-load.d/snd-dummy.conf ] ||
      ! grep -q "snd-dummy" /etc/modules-load.d/snd-dummy.conf; then
      echo "snd-dummy" | run_sudo tee /etc/modules-load.d/snd-dummy.conf >/dev/null
   fi

   # Configure capture device in dawn.toml
   sed_safe_set "$PROJECT_ROOT/dawn.toml" "audio" "capture_device" "plughw:CARD=Dummy,DEV=0"

   log "Headless: snd-dummy configured, capture_device set to Dummy"
   info "Voice input works via WebUI and DAP2 satellites"
   info "When you add a physical mic, update [audio] capture_device in dawn.toml"
}

# ─────────────────────────────────────────────────────────────────────────────
# SSL configuration (update dawn.toml after certs are generated)
# ─────────────────────────────────────────────────────────────────────────────

configure_ssl() {
   local config="$PROJECT_ROOT/dawn.toml"

   sed_safe_set "$config" "webui" "https" "true"
   sed_safe_set "$config" "webui" "ssl_cert_path" "ssl/dawn-chain.crt"
   sed_safe_set "$config" "webui" "ssl_key_path" "ssl/dawn.key"

   log "dawn.toml: SSL paths configured"
}

# ─────────────────────────────────────────────────────────────────────────────
# API key validation
# ─────────────────────────────────────────────────────────────────────────────

validate_api_key() {
   local provider="$1" key="$2"
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
            -H "anthropic-version: 2026-10-21" \
            -H "content-type: application/json" \
            -d '{"model":"claude-haiku-4-5-20251001","max_tokens":1,"messages":[{"role":"user","content":"hi"}]}' \
            https://api.anthropic.com/v1/messages 2>/dev/null || echo "000")
         ;;
      gemini)
         status=$(curl -s -o /dev/null -w "%{http_code}" --max-time 10 \
            "https://generativelanguage.googleapis.com/v1beta/models?key=$key" 2>/dev/null || echo "000")
         ;;
      local)
         status=$(curl -s -o /dev/null -w "%{http_code}" --max-time 5 \
            http://127.0.0.1:8080/v1/models 2>/dev/null || echo "000")
         ;;
   esac

   if [ "$status" = "200" ]; then
      log "  $provider: VALID (HTTP 200)"
      return 0
   elif [ "$status" = "000" ]; then
      warn "  $provider: UNREACHABLE (connection failed)"
      return 1
   else
      warn "  $provider: FAILED (HTTP $status)"
      return 1
   fi
}

run_apikeys() {
   header "Phase 6: API Key Validation"
   CURRENT_PHASE="apikeys"

   local any_configured=false

   if [ -n "${OPENAI_KEY:-}" ]; then
      validate_api_key openai "$OPENAI_KEY" || true
      any_configured=true
   fi
   if [ -n "${CLAUDE_KEY:-}" ]; then
      validate_api_key claude "$CLAUDE_KEY" || true
      any_configured=true
   fi
   if [ -n "${GEMINI_KEY:-}" ]; then
      validate_api_key gemini "$GEMINI_KEY" || true
      any_configured=true
   fi
   if echo "${LLM_PROVIDER:-}" | grep -q "local"; then
      validate_api_key local "" || true
      any_configured=true
   fi

   if [ "$any_configured" = false ]; then
      info "No API keys configured — skipping validation"
      info "Add keys to secrets.toml before running DAWN"
   fi

   log "Phase 6 complete"
}

# ─────────────────────────────────────────────────────────────────────────────
# Phase 5 entry point
# ─────────────────────────────────────────────────────────────────────────────

run_configure() {
   header "Phase 5: Configure"
   CURRENT_PHASE="configure"
   setup_config_files
   write_user_config
   write_api_keys
   setup_headless
   log "Phase 5 complete"
}
