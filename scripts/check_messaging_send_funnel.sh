#!/usr/bin/env bash

###############################################################################
# check_messaging_send_funnel.sh - invariant for the messaging send path
#
# Every driver->send_text() CALL goes through the messaging_deliver /
# engine_send_async funnel, which renders the canonical markdown into the
# driver's out_format and escapes it.  A direct call from anywhere else ships
# raw markdown and, for Telegram (parse_mode=HTML), gets the whole message
# rejected with HTTP 400.  Drivers aren't reachable outside src/messaging
# (find_driver is engine-internal), so only the engine files below may call it.
#
# Runs on every build (CMake target) and in CI.
#
# Usage:   ./scripts/check_messaging_send_funnel.sh
# Exit:    0 = clean, 1 = a call outside the funnel
###############################################################################

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

bad=$(grep -rn -- '->send_text(' src/messaging/ \
   | grep -vE 'messaging_engine\.c|messaging_engine_link\.c|messaging_engine_inbound\.c' \
   || true)
if [ -n "$bad" ]; then
   echo "$bad"
   echo "ERROR: driver->send_text() called outside the messaging_deliver funnel."
   echo "Route outbound replies through messaging_deliver() or messaging_engine_send()."
   exit 1
fi
exit 0
