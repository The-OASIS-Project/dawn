#!/usr/bin/env bash
#
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
# By contributing to this project, you agree to license your contributions
# under the GPLv3 (or any later version) or any future licenses chosen by
# the project author(s).
#
# Runs live tests inside fresh user, network, mount and PID namespaces, so a test
# daemon can't reach the internet, the LAN or the real daemon (only loopback),
# can't see the real DAWN state on disk (empty tmpfs over it), and can't outlive
# the run (the namespace's first process is the test runner).
#
# Usage: tests/live/run.sh [test_module ...]   (default: every test_*.py here)
#   DAWN_LIVE_BUILD=<dir>   build with dawn and dawn-admin (default build-debug)
#   DAWN_LIVE_KEEP=1        keep each instance's run dir
# Exit 77 (skipped) when the namespaces or the Python modules aren't available.

set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
if [ "${DAWN_LIVE_INSIDE:-}" != 1 ]; then
   if ! python3 -c 'import requests, websocket' 2>/dev/null; then
      echo "live tests skipped: needs the Python modules requests and websocket-client" >&2
      exit 77
   fi
   if ! unshare -rnm --pid --fork --mount-proc ip link set lo up 2>/dev/null; then
      echo "live tests skipped: unprivileged user namespaces aren't available here" >&2
      exit 77
   fi
   exec unshare -rnm --pid --fork --mount-proc --kill-child env DAWN_LIVE_INSIDE=1 "$0" "$@"
fi
ip link set lo up
# Real DAWN state, whatever a default path points at, stays out of reach.
for d in /var/lib/dawn /etc/dawn /run "$HOME/.config/dawn" "$HOME/.local/share/dawn"; do
   if [ -d "$d" ]; then
      mount -t tmpfs -o mode=0755 tmpfs "$d"
   fi
done
cd "$here"
if [ $# -eq 0 ]; then
   exec python3 -m unittest discover -s . -p 'test_*.py' -v
fi
exec python3 -m unittest -v "$@"
