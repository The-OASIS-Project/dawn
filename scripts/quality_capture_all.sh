#!/bin/bash
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# Capture the requests the LLM quality suite replays: for each model and WebUI
# surface, a 1-turn and a 2-turn conversation as the quality-fixture user
# (scripts/quality_capture.py --standard), into a new directory under <parent>.
#
# Run it as the daemon's user (e.g. sudo -u dawn for a service install): the
# daemon only writes into a directory its own user owns.
#
# Prerequisites (once): a running daemon built with request capture and the
# quality-fixture user (non-admin, created in the WebUI's user admin).  The
# script sets the fixture's profile itself: the default persona, or the one in
# [persona-file] (replace mode), recorded as persona.txt in the capture.
#
# Usage: scripts/quality_capture_all.sh <parent-dir> [persona-file]
#   dawn-admin asks for admin credentials once to arm the capture; the fixture
#   password comes from $DAWN_FIXTURE_PASSWORD or a prompt.  The arming stops on
#   its own after 30 minutes, or: dawn-admin llm capture --stop

set -euo pipefail

PARENT="${1:?usage: $0 <parent-dir> [persona-file]}"
PERSONA="${2:-}"
mkdir -p "$PARENT"
OUT="$(mktemp -d "$(cd "$PARENT" && pwd)/capture-XXXXXX")"  # new, empty, mode 0700
HERE="$(cd "$(dirname "$0")" && pwd)"
ADMIN="${DAWN_ADMIN:-$HERE/../build-debug/dawn-admin}"

# provider:model pairs to capture (override with QUALITY_MODELS="p:m p:m").
MODELS=(${QUALITY_MODELS:-openai:gpt-5.6-luna claude:claude-haiku-4-5 claude:claude-haiku-5-5})
MODEL_ARGS=()
for m in "${MODELS[@]}"; do MODEL_ARGS+=(--model "$m"); done

# Each model x 2 surfaces x 3 turns is one request per turn; arm twice that so
# an extra request (a retry) can't leave a turn uncaptured.  The suite matches
# requests to turns by their text.
REQUESTS=$(( ${#MODELS[@]} * 2 * 3 * 2 ))
(( REQUESTS > 50 )) && REQUESTS=50

if [ -z "${DAWN_FIXTURE_PASSWORD:-}" ]; then
   read -r -s -p "Password for quality-fixture: " DAWN_FIXTURE_PASSWORD
   echo
   export DAWN_FIXTURE_PASSWORD
fi

SETUP_ARGS=(--setup)
if [ -n "$PERSONA" ]; then
   SETUP_ARGS+=(--persona-file "$PERSONA")
fi
python3 "$HERE/quality_capture.py" "${SETUP_ARGS[@]}"

echo "Arming capture of up to $REQUESTS requests into $OUT"
"$ADMIN" llm capture --user quality-fixture --out "$OUT" --requests "$REQUESTS"

python3 "$HERE/quality_capture.py" --standard --manifest "$OUT/manifest.jsonl" "${MODEL_ARGS[@]}"

# Which persona this set was captured with (written after: arming needs the
# directory empty).
if [ -n "$PERSONA" ]; then cp "$PERSONA" "$OUT/persona.txt"; else echo "default" > "$OUT/persona.txt"; fi
echo "Captured $(ls "$OUT"/[0-9]*.json 2>/dev/null | wc -l) request(s) into $OUT"
