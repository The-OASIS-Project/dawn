#!/usr/bin/env bash

###############################################################################
# check_no_process_mgmt.sh - CI invariant for the DAWN coding harness
#
# Enforces the operator-locked rule (2026-05-29): code added for the MCP bridge
# and code-projects subsystem must NEVER call process-creation/management
# primitives. The bridge is socket-only; git is in-process (libgit2).
#
# SCOPE (locked, Phase 0): harness module files ONLY -- NOT the whole daemon.
# DAWN has three pre-existing, sanctioned subprocess sites that predate the
# harness and are deliberately out of scope:
#   - src/dawn.c            execve("/proc/self/exe", ...)  self-restart
#   - src/tools/shutdown_tool.c   system("sudo shutdown ...")  shutdown command
#   - src/webui/webui_config.c    popen/pclose  audio-device enumeration
#
# nftw() is explicitly ALLOWED (in-process POSIX directory walk).
#
# Matching is call-syntax only (`name(`); // line comments and single-line
# /* ... */ block comments are stripped first so prose references like "restart
# via execve()" do not trip the check. A forbidden mention inside a MULTI-line
# block comment can still false-positive -- reword it; do not loosen this script.
#
# Usage:   ./scripts/check_no_process_mgmt.sh
# Exit:    0 = clean (or no harness files yet), 1 = forbidden call found
###############################################################################

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# Filename globs identifying coding-harness source files (Components 1-5).
# Broad `mcp_*` is intentional: every mcp_* file in this tree is harness code.
HARNESS_GLOBS=(
   'mcp_*'
   'code_project_*'
   'code_projects*'
   'code_graph_provider*'
   'auth_db_mcp.*'
   'admin_socket_mcp.*'
   'admin_socket_code_project.*'
   'auth_db_migrations_v64.*'
   'auth_db_migrations_v65.*'
   'auth_db_migrations_v66.*'
   'webui_code_projects.*'
   'main.*'          # dawn-admin CLI: mcp + code-project subcommands
   'socket_client.*' # dawn-admin: admin_client_mcp_* / _code_proj_* clients
)

# Forbidden process-creation / management primitives (call syntax only).
FORBIDDEN='\b(fork|vfork|clone|clone3|posix_spawn|posix_spawnp|execl|execlp|execle|execv|execvp|execvpe|execve|popen|pclose|system|pidfd_open|pidfd_send_signal|waitpid|wait3|wait4|daemon|setsid|setpgid|unshare|io_uring_setup|dlopen)[[:space:]]*\('

# Collect existing harness files across the first-party source roots.
files=()
for glob in "${HARNESS_GLOBS[@]}"; do
   while IFS= read -r f; do
      files+=("$f")
   done < <(find src include common dawn-admin -type f -name "$glob" 2>/dev/null)
done

if [ ${#files[@]} -eq 0 ]; then
   echo "check_no_process_mgmt: no harness files present yet -- OK (0 files scanned)."
   exit 0
fi

# Strip C comments (multi-line /* */ and // line) while preserving line numbers,
# so prose like "from daemon (...)" in a doc comment can't false-positive on the
# call-syntax patterns below. The `inblk` state persists across lines.
strip_comments() {
   awk '
      {
         line = $0; out = ""; i = 1; L = length(line);
         while (i <= L) {
            c2 = substr(line, i, 2);
            if (inblk) {
               if (c2 == "*/") { inblk = 0; i += 2 } else { i++ }
            } else if (c2 == "/*") {
               inblk = 1; i += 2;
            } else if (c2 == "//") {
               break;  # rest of the line is a comment
            } else {
               out = out substr(line, i, 1); i++;
            }
         }
         print out;
      }' "$1"
}

violations=0
for f in "${files[@]}"; do
   hits="$(strip_comments "$f" | grep -nE "$FORBIDDEN" || true)"
   if [ -n "$hits" ]; then
      echo "VIOLATION in $f:"
      printf '%s\n' "$hits" | sed 's/^/  /'
      violations=$((violations + 1))
   fi
done

if [ "$violations" -gt 0 ]; then
   echo ""
   echo "check_no_process_mgmt: FAILED -- $violations harness file(s) contain forbidden"
   echo "process-management calls. The MCP bridge must be socket-only and git must use"
   echo "libgit2 in-process. nftw() is allowed. See docs/CODING_HARNESS_DESIGN.md."
   exit 1
fi

echo "check_no_process_mgmt: OK -- ${#files[@]} harness file(s) scanned, no forbidden calls."
exit 0
