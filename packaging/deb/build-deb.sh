#!/bin/bash
#
# Build the dawn .deb (x86-64) for an Ubuntu LTS release, in a container.
#
# The Dockerfile's dawn-deps stage, rebuilt on that release, supplies the build
# dependencies (the same pinned ONNX Runtime, espeak-ng fork and piper-phonemize as
# the Docker image).  stage.sh then builds DAWN and the package inside it, with the
# checkout mounted at /repo.  Needs docker and the whisper.cpp submodule.
#
#   [UBUNTU=24.04|26.04] packaging/deb/build-deb.sh [VERSION]
#   DEB_MAINTAINER="You <you@example.com>" packaging/deb/build-deb.sh 2.1.0
#
# Output: build-deb/dawn_<version>+ubuntu<release>_amd64.deb.  The suffix keeps
# the releases' packages apart and sorts the newer release's above the older's.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
UBUNTU="${UBUNTU:-24.04}"
case "$UBUNTU" in
   24.04) base=ubuntu:24.04@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac ;;
   26.04) base=ubuntu:26.04@sha256:c098726dd9b9733f3e885f8de0ef375206a311b0a317eadc40d347b1115fb944 ;;
   *) echo "UBUNTU must be 24.04 or 26.04" >&2; exit 1 ;;
esac
DEPS_IMAGE="${DEPS_IMAGE:-dawn-deb-deps:$UBUNTU}"
VERSION="${1:-${DEB_VERSION:-}}"
MAINTAINER="${DEB_MAINTAINER:-The OASIS Project <noreply@users.noreply.github.com>}"
GIT_SHA="$(git -C "$REPO_ROOT" rev-parse --short HEAD)"

if [ -z "$VERSION" ]; then
   # An untagged build sorts below version.h's version (~), the release it leads
   # to, and newer builds above older ones: <version.h>~git<date>.<count>.<sha>
   # (a bare <sha> sorts by hex, so dpkg could read a newer build as older).
   # Bump version.h after tagging, or builds after a release sort below it.
   base_ver="$(sed -n 's/^#define VERSION_NUMBER "\(.*\)"/\1/p' "$REPO_ROOT/include/version.h")"
   count="${GITHUB_RUN_NUMBER:-$(git -C "$REPO_ROOT" rev-list --count HEAD)}"
   VERSION="${base_ver}~git$(date -u +%Y%m%d).${count}.${GIT_SHA}"
fi
VERSION="${VERSION}+ubuntu${UBUNTU}"

[ -f "$REPO_ROOT/whisper.cpp/CMakeLists.txt" ] ||
   { echo "whisper.cpp submodule missing: git submodule update --init whisper.cpp" >&2; exit 1; }

echo "==> dependency image $DEPS_IMAGE ($base)…"
docker build --target dawn-deps \
   --build-arg DEPS_BASE="$base" \
   --build-arg ESPEAK_PREFIX=/usr/lib/dawn \
   -t "$DEPS_IMAGE" "$REPO_ROOT"

echo "==> building dawn $VERSION…"
docker run --rm -u "$(id -u):$(id -g)" \
   -e VERSION="$VERSION" -e MAINTAINER="$MAINTAINER" -e GIT_SHA="$GIT_SHA" \
   -v "$REPO_ROOT":/repo -w /repo \
   "$DEPS_IMAGE" packaging/deb/stage.sh
