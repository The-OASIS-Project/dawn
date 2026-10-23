#!/usr/bin/env bash

###############################################################################
# check_no_ws_direct_write.sh - CI invariant for the WebUI send path
#
# Enforces the WebSocket-send funnel rule: every server->client WS DATA frame
# on the main WebUI connection goes through webui_send.c's write_frame()
# (send_json_message() / send_binary_message(), reached from the response queue
# or a receive handler), NEVER a direct lws_write().
#
# WHY: a frame written while earlier output is still buffered on the socket
# leaves libwebsockets mid-frame, and the next write goes out with no WebSocket
# header.  The browser then sees "Invalid frame header" / "Could not decode a
# text frame as UTF-8", drops the socket, reconnects, reloads the same large
# conversation, and can loop.  write_frame() holds a frame until the connection
# is writable; a direct lws_write() skips that.  This bug was a pair of direct
# lws_write() calls in webui_config.c (2026-06).
#
# RULE: no LWS_WRITE_TEXT / LWS_WRITE_BINARY in src/webui/*.c except the files
# that legitimately own a write path:
#   - webui_send.c          write_frame() and its held-frame send (the one
#                           sanctioned writer for the main connection)
#   - webui_music.c         music streaming write_pending() — SEPARATE music lws
#                           context, driven by its own WRITEABLE callback
#   - webui_music_server.c  music handshake auth_ok/auth_failed — SEPARATE music
#                           lws context, single fixed frame, pre-stream
#
# HTTP responses (LWS_WRITE_HTTP*) are a different code path and are NOT checked.
#
# Matching strips // line and single-line /* */ comments first so prose
# references don't false-positive (same approach as check_no_process_mgmt.sh).
#
# Usage:   ./scripts/check_no_ws_direct_write.sh
# Exit:    0 = clean, 1 = forbidden direct WS-frame write found
###############################################################################

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# Files allowed to call lws_write() with a WS data-frame flag.
ALLOWLIST='webui_send\.c|webui_music\.c|webui_music_server\.c'

# WS data-frame write flags (NOT LWS_WRITE_HTTP*, which are legitimate).
FORBIDDEN='LWS_WRITE_(TEXT|BINARY)'

# Strip C comments (multi-line /* */ and // line) while preserving line numbers,
# so a doc comment mentioning LWS_WRITE_TEXT can't false-positive.
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
               break;
            } else {
               out = out substr(line, i, 1); i++;
            }
         }
         print out;
      }' "$1"
}

violations=0
scanned=0
while IFS= read -r f; do
   base="$(basename "$f")"
   if [[ "$base" =~ ^($ALLOWLIST)$ ]]; then
      continue
   fi
   scanned=$((scanned + 1))
   hits="$(strip_comments "$f" | grep -nE "$FORBIDDEN" || true)"
   if [ -n "$hits" ]; then
      echo "VIOLATION in $f:"
      printf '%s\n' "$hits" | sed 's/^/  /'
      violations=$((violations + 1))
   fi
done < <(find src/webui -type f -name '*.c' 2>/dev/null)

if [ "$violations" -gt 0 ]; then
   echo ""
   echo "check_no_ws_direct_write: FAILED -- $violations file(s) write a WebSocket data"
   echo "frame directly with lws_write().  Route server->client frames through"
   echo "send_json_response() / queue_response() instead (see webui_send.c).  A direct"
   echo "write interleaves with large queued frames and loops the client."
   exit 1
fi

echo "check_no_ws_direct_write: OK -- ${scanned} file(s) scanned, no direct WS-frame writes."
exit 0
