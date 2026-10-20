#!/bin/bash
#
# Build DAWN and package it as dawn_<version>_amd64.deb.  Run from the repository
# root inside the dependency image (build-deb.sh does this), or on an Ubuntu LTS
# host with the same dependencies installed.
#
#   VERSION=2.1.0 packaging/deb/stage.sh
#
# Env:
#   VERSION      package version (required)
#   MAINTAINER   Maintainer: field
#   GIT_SHA      stamped into the binary's version string
#   ESPEAK_DATA  the espeak-ng fork's data (default /usr/lib/dawn/share/espeak-ng-data)
#   LICENSE_DIR  the source-built libraries' licenses (default /usr/local/share/doc)
#                -- both where the dependency image installs them
#
# Layout: programs in /usr/bin, whisper/ggml and the bundled libraries in the
# private /usr/lib/dawn, read-only data in /usr/share/dawn.
set -euo pipefail

: "${VERSION:?set VERSION}"
MAINTAINER="${MAINTAINER:-The OASIS Project <noreply@users.noreply.github.com>}"
ESPEAK_DATA="${ESPEAK_DATA:-/usr/lib/dawn/share/espeak-ng-data}"
LICENSE_DIR="${LICENSE_DIR:-/usr/local/share/doc}"

pkg=packaging/deb
build=build-deb
stage="$build/stage"
lib="$stage/usr/lib/dawn"
share="$stage/usr/share/dawn"
doc="$stage/usr/share/doc/dawn"

. packaging/deb_common.sh

# --- build (the deb preset: /usr prefix, private libdir, portable ggml) ---
cmake --preset deb --fresh ${GIT_SHA:+-DGIT_SHA="$GIT_SHA"} # no stale cache in a release
cmake --build "$build" -j"$(nproc)"
rm -rf "$build/install" "$stage"
DESTDIR="$PWD/$build/install" cmake --install "$build"
installed="$build/install/usr"

# Read once, not piped into grep -q: grep stopping early would SIGPIPE readelf
# and, under pipefail, fail the check.
dynamic="$(readelf -d "$installed/bin/dawn")"

# The binary must find its libraries in /usr/lib/dawn, not the build machine's.
grep -q 'R.*PATH.*\[/usr/lib/dawn\]' <<<"$dynamic" ||
   { echo "ERROR: dawn's RUNPATH is not /usr/lib/dawn" >&2; exit 1; }

# --- programs and libraries ---
mkdir -p "$stage/DEBIAN" "$stage/usr/bin" "$stage/usr/sbin" "$lib/share" "$share/conf" "$share/models" \
   "$stage/usr/lib/systemd/system" "$doc/licenses"
install -m 755 "$installed/bin/dawn" "$installed/bin/dawn-admin" "$pkg/dawn-fetch-models" \
   "$stage/usr/bin/"
install -m 755 "$pkg/dawn-tls" "$stage/usr/sbin/"
cp -a "$installed/lib/dawn/"*.so* "$lib/" # whisper + ggml

echo "==> runtime libraries (apt-owned → Depends, source-built → /usr/lib/dawn)…"
shlib_deps="$(LD_LIBRARY_PATH="$lib" deb_bundle_closure "$lib" \
   "$stage/usr/bin/dawn" "$stage/usr/bin/dawn-admin" | paste -sd, - | sed 's/,/, /g')"
[ -n "$shlib_deps" ] || { echo "ERROR: empty Depends closure" >&2; exit 1; }

# Each library finds the others in the same private directory, without an
# ld.so.conf.d entry that would put the espeak-ng fork in front of every program.
find "$lib" -maxdepth 1 -type f -name '*.so*' -exec patchelf --set-rpath /usr/lib/dawn {} +

# Stripped, as dh_strip does (Policy 10.1).
strip --remove-section=.comment --remove-section=.note "$stage/usr/bin/dawn" \
   "$stage/usr/bin/dawn-admin"
find "$lib" -maxdepth 1 -type f -name '*.so*' \
   -exec strip --remove-section=.comment --remove-section=.note --strip-unneeded {} +

# --- runtime files ---
cp -a www "$share/www"
cp -a tool_instructions "$share/tool_instructions"
cp -a sound_assets "$share/sound_assets"
cp -a "$ESPEAK_DATA" "$lib/share/espeak-ng-data" # the path compiled into the fork
install -m 644 models/*.onnx models/*.onnx.json "$share/models/"
# .in: the repository ignores every dawn.toml.  The example secrets set nothing, so
# they are the starting secrets.toml, every key documented in place.
install -m 644 "$pkg/dawn.toml.in" "$share/conf/dawn.toml"
install -m 644 secrets.toml.example "$share/conf/secrets.toml"
install -m 644 "$pkg/dawn.service" "$pkg/dawn-models.service" "$stage/usr/lib/systemd/system/"

# --- documentation and licenses ---
espeak_commit="$(sed -n 's/^ARG ESPEAK_COMMIT=//p' Dockerfile)"
sed "s/@ESPEAK_COMMIT@/$espeak_commit/" "$pkg/copyright" >"$doc/copyright"
install -m 644 dawn.toml.example secrets.toml.example NOTICE "$doc/"
lic="$doc/licenses"
cp -a "$LICENSE_DIR/onnxruntime" "$LICENSE_DIR/espeak-ng" "$LICENSE_DIR/piper-phonemize" "$lic/"
install -D -m 644 whisper.cpp/LICENSE "$lic/whisper.cpp/LICENSE"
install -D -m 644 third_party/piper/LICENSE "$lic/piper/LICENSE"
install -D -m 644 -t "$lic/models" models/*_LICENSE
install -m 644 /usr/share/doc/libmupdf-dev/copyright "$lic/mupdf.copyright"
install -m 644 LICENSES/Apache-2.0.txt "$lic/"
# A native package's changelog (Policy 12.7); the release notes are on GitHub.
cat >"$doc/changelog" <<EOF
dawn ($VERSION) $(. /etc/os-release && echo "$VERSION_CODENAME"); urgency=medium

  * DAWN $VERSION.  Release notes:
    https://github.com/The-OASIS-Project/dawn/releases

 -- $MAINTAINER  $(date -R)
EOF
gzip -9n "$doc/changelog"

# --- control, checksums, maintainer scripts ---
chmod -R go-w "$stage"
# Built-Using names the source of a statically linked library (Policy 7.8):
# MuPDF, on releases that ship it only static (24.04).  26.04's is shared.
built_using='/^Built-Using: @BUILT_USING@$/d'
if ! grep -q 'NEEDED.*libmupdf' <<<"$dynamic"; then
   built_using="s/@BUILT_USING@/$(dpkg-query -W -f='${source:Package} (= ${source:Version})' libmupdf-dev)/"
fi
size="$(du -sk "$stage" | cut -f1)"
sed -e "s/@VERSION@/$VERSION/" -e "s|@MAINTAINER@|$MAINTAINER|" \
   -e "s/@SHLIB_DEPS@/$shlib_deps/" -e "$built_using" \
   -e "s/@INSTALLED_SIZE@/$size/" \
   "$pkg/debian/control" >"$stage/DEBIAN/control"
(cd "$stage" && find usr -type f -print0 | sort -z | xargs -0 md5sum) >"$stage/DEBIAN/md5sums"
for s in preinst postinst prerm postrm; do
   install -m 755 "$pkg/debian/$s" "$stage/DEBIAN/$s"
done
chmod 644 "$stage/DEBIAN/control" "$stage/DEBIAN/md5sums"

out="$build/dawn_${VERSION}_amd64.deb"
dpkg-deb --root-owner-group --build "$stage" "$out"
echo "==> built $out"
dpkg-deb --info "$out" | sed -n '/Package:/,$p'
