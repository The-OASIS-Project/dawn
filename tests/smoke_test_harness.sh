#!/usr/bin/env bash

###############################################################################
# smoke_test_harness.sh - End-to-end smoke test for the coding harness.
#
# Drives the running DAWN daemon through a real import: clone a local bare-repo
# fixture (no network) via the admin socket, then poll until the project reaches
# "ready" (clone + cbm index complete). Optionally probes the cbm MCP server.
#
# Prerequisites (this harness does NOT start them):
#   - dawn daemon running, built with -DDAWN_ENABLE_MCP_BRIDGE_TOOL=ON
#     -DDAWN_ENABLE_CODE_PROJECTS=ON, and [code_projects] enabled = true.
#   - dawn-admin binary on PATH or at ./build-debug/dawn-admin.
#   - For a full pass: an operator-launched cbm MCP server reachable at the
#     URL in [[mcp.server]] (default http://localhost:9000/sse).
#
# In CI set DAWN_SMOKE_REQUIRE_HARNESS=1 to make a missing daemon a failure
# rather than a skip.
#
# Usage:  ./tests/smoke_test_harness.sh
# Exit:   0 = pass (or skipped when prerequisites absent), 1 = failure
###############################################################################

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DAWN_ADMIN="${DAWN_ADMIN:-}"
PROJECT_NAME="smoketest"
CBM_URL="${CBM_URL:-http://localhost:9000/sse}"
REQUIRE="${DAWN_SMOKE_REQUIRE_HARNESS:-0}"

skip_or_fail() {
   echo "SKIP/FAIL: $1"
   if [ "$REQUIRE" = "1" ]; then
      exit 1
   fi
   echo "(set DAWN_SMOKE_REQUIRE_HARNESS=1 to treat this as a failure)"
   exit 0
}

# Resolve dawn-admin.
if [ -z "$DAWN_ADMIN" ]; then
   if command -v dawn-admin >/dev/null 2>&1; then
      DAWN_ADMIN="dawn-admin"
   elif [ -x "$REPO_ROOT/build-debug/dawn-admin/dawn-admin" ]; then
      DAWN_ADMIN="$REPO_ROOT/build-debug/dawn-admin/dawn-admin"
   else
      skip_or_fail "dawn-admin not found"
   fi
fi

# Daemon reachable?
if ! "$DAWN_ADMIN" mcp list >/dev/null 2>&1; then
   skip_or_fail "DAWN daemon admin socket not reachable (is the daemon running with the coding harness enabled?)"
fi

# Build a local bare-repo fixture to clone (file:// — no network).
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SRC="$WORK/src.git"
git init -q --bare "$SRC"
# Seed one commit through a temp working clone.
CLONE="$WORK/seed"
git clone -q "$SRC" "$CLONE"
echo "# smoke" >"$CLONE/README.md"
git -C "$CLONE" add README.md
git -C "$CLONE" -c user.email=smoke@test -c user.name=smoke commit -qm init
git -C "$CLONE" push -q origin HEAD:refs/heads/main

echo "== Importing file://$SRC as '$PROJECT_NAME' =="
"$DAWN_ADMIN" code-project delete "$PROJECT_NAME" >/dev/null 2>&1 || true
if ! "$DAWN_ADMIN" code-project import "file://$SRC" --name "$PROJECT_NAME"; then
   echo "FAIL: import command rejected"
   exit 1
fi

echo "== Polling for 'ready' (clone + index) =="
deadline=$((SECONDS + 120))
status=""
while [ "$SECONDS" -lt "$deadline" ]; do
   line="$("$DAWN_ADMIN" code-project list 2>/dev/null | grep -F "$PROJECT_NAME" || true)"
   status="$(printf '%s' "$line" | awk -F'\t' '{print $2}')"
   case "$status" in
      ready)
         echo "PASS: project '$PROJECT_NAME' reached 'ready'."
         "$DAWN_ADMIN" code-project delete "$PROJECT_NAME" >/dev/null 2>&1 || true
         exit 0
         ;;
      error)
         echo "FAIL: project entered 'error' state: $line"
         exit 1
         ;;
   esac
   sleep 2
done

echo "FAIL: timed out waiting for 'ready' (last status: '${status:-unknown}')."
echo "Note: indexing requires a reachable cbm server at $CBM_URL."
exit 1
