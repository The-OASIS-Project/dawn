#!/usr/bin/env bash

###############################################################################
# check_llm_selection_resolved.sh - build invariant for LLM selections
#
# A session's LLM settings may leave the selection unset (LLM_UNDEFINED: follow
# the default from dawn.toml).  Readers get it resolved from
# session_get_llm_config(); only a read-modify-write that stores the settings
# back may read them unresolved, so an unset selection stays unset.
#
# RULES:
#   1. session_get_llm_config_raw() is called only by the settings writers listed
#      below (and defined in the session manager).
#   2. A session's llm_config and turn_llm_config fields are read only by the
#      session manager and the session history (which resolve them).
#
# Usage:   ./scripts/check_llm_selection_resolved.sh
# Exit:    0 = clean, 1 = violation found, 2 = grep error
###############################################################################

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

ALLOW='src/core/session_manager\.c|include/core/session_manager\.h|src/webui/webui_message_dispatch\.c'
FIELD_ALLOW='src/core/session_manager\.c|src/core/session_history\.c'

# find <regex> <allowlist regex over repo paths>: the hits outside the allowlist
find_hits() {
   local rc=0 hits
   hits="$(grep -rnE "$1" src include)" || rc=$?
   if [ "$rc" -gt 1 ]; then
      echo "check_llm_selection_resolved: grep failed" >&2
      exit 2
   fi
   printf '%s\n' "$hits" | grep -vE "^($2):" | grep -v '^$' || true
}

hits="$(find_hits 'session_get_llm_config_raw[[:space:]]*\(' "$ALLOW")"
fields="$(find_hits '(->|\.)(turn_)?llm_config\b' "$FIELD_ALLOW")"

if [ -n "$hits" ] || [ -n "$fields" ]; then
   echo "VIOLATION (unresolved LLM settings read outside a settings writer):"
   [ -n "$hits" ] && printf '  %s\n' "$hits"
   [ -n "$fields" ] && printf '  %s\n' "$fields"
   echo ""
   echo "check_llm_selection_resolved: FAILED.  Read a session's LLM settings with"
   echo "session_get_llm_config(); the raw read is for writers that store them back."
   exit 1
fi

echo "check_llm_selection_resolved: OK -- unresolved LLM settings read only by their writers."
exit 0
