#!/bin/bash
#
# Build the dawn-satellite .deb (Tier-1, Debian trixie arm64).
#
# Runs the staging + dpkg-deb INSIDE the trixie-arm64 build container (same one
# build-pi.sh uses), because that's where the freshly-built binary AND its
# non-apt runtime libs (whisper/ggml/onnxruntime/piper, installed under
# /usr/local/lib in the base image) actually live.  We `ldd` the binary and
# bundle every lib that isn't an apt-provided system lib; the apt ones are
# declared as Depends in debian/control.
#
# Prereq: a full build must exist (run `SAT_BUILD=full ./build-pi.sh` first; we
# auto-run it if build-pi/dawn_satellite is missing).  Output:
#   dawn_satellite/build-pi/dawn-satellite_<version>_arm64.deb
#
#   ./package-deb.sh [VERSION]
#   DEB_MAINTAINER="You <you@example.com>" ./package-deb.sh 2.1.0
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BASE_IMAGE_TAG="${BASE_IMAGE_TAG:-ghcr.io/the-oasis-project/dawn-satellite-build:trixie-arm64}"
ARCH="arm64"
VERSION="${1:-${DEB_VERSION:-$(git -C "$REPO_ROOT" describe --tags --always 2>/dev/null | sed 's/^v//')}}"
VERSION="${VERSION:-0.0.0}"
MAINTAINER="${DEB_MAINTAINER:-The OASIS Project <noreply@users.noreply.github.com>}"

# Always do a FULL (SDL) build — never trust a possibly-stale build-pi/ binary
# (a leftover headless compile-check binary would otherwise be packaged with no
# GUI).  build-pi.sh reconfigures the cmake flags and the make is incremental,
# so this is cheap when already full-built.
echo "==> ensuring a full (VAD+ASR+TTS+SDL) build via build-pi.sh…"
SAT_BUILD=full "$SCRIPT_DIR/build-pi.sh"

echo "==> packaging dawn-satellite $VERSION ($ARCH) in $BASE_IMAGE_TAG…"

# The staging + dpkg-deb run inside the container (has /usr/local/lib + dpkg-deb).
docker run --rm --platform linux/arm64 \
   -u "$(id -u):$(id -g)" \
   -e VERSION="$VERSION" -e ARCH="$ARCH" -e MAINTAINER="$MAINTAINER" \
   -v "$REPO_ROOT":/repo \
   "$BASE_IMAGE_TAG" bash -euo pipefail -c '
   cd /repo
   bin=dawn_satellite/build-pi/dawn_satellite
   launch=dawn_satellite/build-pi/dawn-satellite-launch
   pkgdir=dawn_satellite/packaging
   svc=services/dawn-satellite
   [ -x "$bin" ]    || { echo "missing $bin (build first)"; exit 1; }
   [ -x "$launch" ] || { echo "missing $launch (build first)"; exit 1; }

   stage="$(mktemp -d)"
   trap "rm -rf $stage" EXIT

   # --- layout ---
   mkdir -p "$stage/DEBIAN" \
            "$stage/usr/bin" \
            "$stage/usr/lib/dawn-satellite" \
            "$stage/etc/dawn-satellite" \
            "$stage/etc/ld.so.conf.d" \
            "$stage/lib/systemd/system" \
            "$stage/etc/logrotate.d" \
            "$stage/usr/share/doc/dawn-satellite"

   # frozen launcher + operator model-fetch helper → /usr/bin (root, package-owned)
   install -m 755 "$launch" "$stage/usr/bin/dawn-satellite-launch"
   install -m 755 "$pkgdir/dawn-satellite-fetch-models" "$stage/usr/bin/dawn-satellite-fetch-models"

   # pristine binary (apt-owned); postinst seeds the live /var/lib copy from it
   install -m 755 "$bin" "$stage/usr/lib/dawn-satellite/dawn_satellite.dist"

   # Runtime libs: apt-owned → Depends, source-built → bundled into
   # /usr/lib/dawn-satellite (see packaging/deb_closure.sh).
   echo "==> partitioning runtime libs (apt → Depends, private → bundle)…"
   . packaging/deb_closure.sh
   shlib_deps="$(deb_bundle_closure "$stage/usr/lib/dawn-satellite" "$bin" "$launch" |
      paste -sd"," - | sed "s/,/, /g")"
   [ -n "$shlib_deps" ] || { echo "ERROR: empty Depends closure"; exit 1; }
   echo "==> Depends: $shlib_deps, adduser"

   # config (conffiles), env file, ld path, unit, logrotate
   cfg="$svc/satellite.toml"; [ -f "$cfg" ] || cfg="dawn_satellite/config/satellite.toml"
   install -m 644 "$cfg" "$stage/etc/dawn-satellite/satellite.toml"
   install -m 644 "$svc/dawn-satellite.conf" "$stage/etc/dawn-satellite/dawn-satellite.conf"
   echo "/usr/lib/dawn-satellite" > "$stage/etc/ld.so.conf.d/dawn-satellite.conf"
   install -m 644 "$svc/dawn-satellite.service" "$stage/lib/systemd/system/dawn-satellite.service"
   if [ -f "$svc/dawn-satellite-logrotate" ]; then
      install -m 644 "$svc/dawn-satellite-logrotate" "$stage/etc/logrotate.d/dawn-satellite"
   fi

   # minimal copyright (GPLv3) for lintian/policy
   cat > "$stage/usr/share/doc/dawn-satellite/copyright" <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: dawn
Source: https://github.com/The-OASIS-Project/dawn

Files: *
Copyright: The OASIS Project
License: GPL-3+
 This program is free software: you can redistribute it and/or modify it under
 the terms of the GNU General Public License as published by the Free Software
 Foundation, either version 3 of the License, or (at your option) any later
 version.  On Debian systems the full text is in /usr/share/common-licenses/GPL-3.
EOF

   # DEBIAN control + maintainer scripts  ($shlib_deps computed in the partition pass above)
   sed -e "s/@VERSION@/$VERSION/" -e "s/@ARCH@/$ARCH/" -e "s|@MAINTAINER@|$MAINTAINER|" \
       -e "s/@SHLIB_DEPS@/$shlib_deps/" \
       "$pkgdir/debian/control" > "$stage/DEBIAN/control"
   install -m 644 "$pkgdir/debian/conffiles" "$stage/DEBIAN/conffiles"
   for s in postinst prerm postrm; do
      install -m 755 "$pkgdir/debian/$s" "$stage/DEBIAN/$s"
   done

   out="dawn_satellite/build-pi/dawn-satellite_${VERSION}_${ARCH}.deb"
   dpkg-deb --root-owner-group --build "$stage" "$out"
   echo "==> built $out"
   dpkg-deb --info "$out" | sed -n "1,20p"
'
echo "==> Done: dawn_satellite/build-pi/dawn-satellite_${VERSION}_${ARCH}.deb"
