#!/bin/bash
#
# Shared by the .deb packagers (packaging/deb/stage.sh for the daemon,
# dawn_satellite/package-deb.sh for the Tier-1 satellite).  Source it; don't run it.
#
# deb_bundle_closure BUNDLE_DIR BINARY...
#
#   Walks the shared-library closure of BINARY... (ldd, so transitive deps too) and
#   splits it the way dpkg-shlibdeps would:
#     * a library an apt package owns (dpkg -S)  -> that package is printed on stdout,
#       one per line, for the caller to turn into Depends:
#     * any other library (source-built: whisper/ggml, ONNX Runtime, piper-phonemize,
#       the rhasspy espeak-ng fork, wherever it was installed) -> copied into
#       BUNDLE_DIR under its SONAME, so the loader finds it by the name it asks for
#       with or without ldconfig.
#   Libraries already inside BUNDLE_DIR (the caller's own, resolved there through
#   LD_LIBRARY_PATH) are left alone.  Fails if any library is not found: a package
#   built from that closure would not start.
#
#   Ownership is asked of dpkg rather than guessed from the path, because a
#   source-built library can sit under /usr/lib (the espeak-ng fork does) and a
#   path rule would leave it neither bundled nor depended on.

deb_bundle_closure() {
   local -
   set -o pipefail
   local bundle="$1"
   shift
   local closure
   closure="$(ldd "$@")" || return 1

   if grep -q 'not found' <<<"$closure"; then
      echo "deb_bundle_closure: unresolved libraries:" >&2
      grep 'not found' <<<"$closure" >&2
      return 1
   fi

   local bundle_real so real pkg
   bundle_real="$(realpath "$bundle")"
   awk '/=> \//{print $3}' <<<"$closure" | sort -u | while read -r so; do
      real="$(realpath "$so")"
      case "$real" in "$bundle_real"/*) continue ;; esac
      pkg="$(dpkg -S "$real" 2>/dev/null | head -1 | cut -d: -f1 | cut -d, -f1 | tr -d ' ')"
      if [ -n "$pkg" ]; then
         echo "$pkg"
      else
         echo "    bundle (no apt package): $real" >&2
         install -m 644 "$so" "$bundle/$(basename "$so")" || return 1
      fi
   done | sort -u
}
