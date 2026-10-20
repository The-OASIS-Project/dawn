#!/bin/bash
#
# Build the dawn .deb (x86-64, Ubuntu 24.04) in a container.
#
# The Dockerfile's dawn-deps stage, rebuilt on ubuntu:24.04, supplies the build
# dependencies (the same pinned ONNX Runtime, espeak-ng fork and piper-phonemize as
# the Docker image).  stage.sh then builds DAWN and the package inside it, with the
# checkout mounted at /repo.  Needs docker and the whisper.cpp submodule.
#
#   packaging/deb/build-deb.sh [VERSION]
#   DEB_MAINTAINER="You <you@example.com>" packaging/deb/build-deb.sh 2.1.0
#
# Output: build-deb/dawn_<version>_amd64.deb
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEPS_IMAGE="${DEPS_IMAGE:-dawn-deb-deps:noble}"
VERSION="${1:-${DEB_VERSION:-}}"
MAINTAINER="${DEB_MAINTAINER:-The OASIS Project <noreply@users.noreply.github.com>}"
GIT_SHA="$(git -C "$REPO_ROOT" rev-parse --short HEAD)"

if [ -z "$VERSION" ]; then
   # An untagged build sorts below version.h's version (~), the release it leads
   # to, and newer builds above older ones: <version.h>~git<date>.<count>.<sha>
   # (a bare <sha> sorts by hex, so dpkg could read a newer build as older).
   # Bump version.h after tagging, or builds after a release sort below it.
   base="$(sed -n 's/^#define VERSION_NUMBER "\(.*\)"/\1/p' "$REPO_ROOT/include/version.h")"
   count="${GITHUB_RUN_NUMBER:-$(git -C "$REPO_ROOT" rev-list --count HEAD)}"
   VERSION="${base}~git$(date -u +%Y%m%d).${count}.${GIT_SHA}"
fi

[ -f "$REPO_ROOT/whisper.cpp/CMakeLists.txt" ] ||
   { echo "whisper.cpp submodule missing: git submodule update --init whisper.cpp" >&2; exit 1; }

echo "==> dependency image $DEPS_IMAGE (ubuntu:24.04)…"
docker build --target dawn-deps \
   --build-arg DEPS_BASE=ubuntu:24.04@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac \
   --build-arg ESPEAK_PREFIX=/usr/lib/dawn \
   -t "$DEPS_IMAGE" "$REPO_ROOT"

echo "==> building dawn $VERSION…"
docker run --rm -u "$(id -u):$(id -g)" \
   -e VERSION="$VERSION" -e MAINTAINER="$MAINTAINER" -e GIT_SHA="$GIT_SHA" \
   -v "$REPO_ROOT":/repo -w /repo \
   "$DEPS_IMAGE" packaging/deb/stage.sh
