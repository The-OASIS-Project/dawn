#!/bin/bash
#
# ota-release.sh — sign + stage (+ optionally push) a satellite OTA release.
#
# Keys live on the daemon/admin host at a fixed, root-only location so the
# private key is OFF the daemon (a daemon-user compromise cannot read it, let
# alone forge updates):
#     /etc/dawn/signing/ota_signing.key   root:root 0600   (PRIVATE — never on a device)
#     /etc/dawn/signing/ota_signing.pub   root:root 0644   (PUBLIC  — deploy to each
#                                                            device's /etc/dawn/ota_pubkey)
# Stronger still: keep the .key on removable/offline media and point --key at it.
#
#   sudo ./ota-release.sh keygen                 # one-time: make the keypair (won't overwrite)
#   sudo ./ota-release.sh --version 2.1.0        # sign build-pi/dawn_satellite as rpi/2.1.0, stage it
#   sudo ./ota-release.sh --version 2.1.0 --push <uuid>   # …and push to one device
#
# Options:
#   --version X.Y.Z     (required for a release) semver of this image
#   --image PATH        binary to ship (default: dawn_satellite/build-pi/dawn_satellite)
#   --platform rpi|esp32  (default rpi)        --tier N (default 1)
#   --abi-tag TAG       (default debian-trixie-aarch64 — must match the device's computed tag)
#   --min-version X.Y.Z anti-SKIP floor: the minimum version a device must ALREADY
#                       be running to accept this update (forces sequential upgrades
#                       through a required migration milestone). Default: empty (no
#                       gate — any device can take it). NOTE: anti-ROLLBACK (refusing
#                       an image older than what's installed) is automatic and needs
#                       no floor; set this ONLY to gate a milestone.
#   --key PATH          signing key (default /etc/dawn/signing/ota_signing.key)
#   --release-dir DIR   daemon release store (default: [ota].release_dir from dawn.toml, else /var/lib/dawn/ota)
#   --push UUID         after staging, dawn-admin ota push to this device
#   --allow-downgrade   permit pushing an older version than the device runs
#                       (only meaningful with --push; passed to dawn-admin ota push)
#   --keytool PATH      ota-keytool (default: build-debug/ota-keytool)
#   --force             sign even if --version != the binary's compiled version
#                       (the device would report a different version than the daemon
#                       expects — use only for deliberate test mismatches)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SIGN_DIR="/etc/dawn/signing"
KEY="$SIGN_DIR/ota_signing.key"
PUB="$SIGN_DIR/ota_signing.pub"
KEYTOOL="$REPO_ROOT/build-debug/ota-keytool"
VERSION_HEADER="$REPO_ROOT/dawn_satellite/include/satellite_version.h"
DEFAULT_IMAGE="$REPO_ROOT/dawn_satellite/build-pi/dawn_satellite"
PLATFORM="rpi"; TIER="1"; ABI="debian-trixie-aarch64"
IMAGE="$DEFAULT_IMAGE"
VERSION=""; MIN_VERSION=""; PUSH_UUID=""; RELEASE_DIR=""; FORCE=0; ALLOW_DOWNGRADE=0

die() { echo "ota-release: $*" >&2; exit 1; }

# Echo the firmware version compiled into a satellite binary (empty if the marker
# is absent — i.e. a binary built before DAWN_SAT_FW_VERSION_MARKER existed).
image_fw_version() {
   if command -v strings >/dev/null 2>&1; then
      strings -a "$1" 2>/dev/null | sed -n 's/^DAWN_SAT_FW_VERSION=//p' | head -1
   else
      grep -aom1 'DAWN_SAT_FW_VERSION=[0-9A-Za-z._-]*' "$1" 2>/dev/null |
         sed 's/^DAWN_SAT_FW_VERSION=//'
   fi
}

# ---- keygen subcommand --------------------------------------------------------
if [ "${1:-}" = "keygen" ]; then
   [ "$(id -u)" -eq 0 ] || die "run keygen as root (writes $SIGN_DIR)"
   [ -e "$KEY" ] && die "$KEY already exists — refusing to overwrite (back it up / remove deliberately)"
   install -d -m 700 -o root -g root "$SIGN_DIR"
   "${2:-$KEYTOOL}" keygen --out-dir "$SIGN_DIR" >/dev/null 2>&1 || "$KEYTOOL" keygen --out-dir "$SIGN_DIR"
   chmod 600 "$KEY"; chmod 644 "$PUB"; chown root:root "$KEY" "$PUB"
   echo "Keypair written:"
   echo "  $KEY   (PRIVATE — keep root-only / offline)"
   echo "  $PUB   (PUBLIC  — deploy to each device: sudo install -m644 -o root '$PUB' /etc/dawn/ota_pubkey)"
   exit 0
fi

# ---- option parsing -----------------------------------------------------------
while [ $# -gt 0 ]; do
   case "$1" in
      --version)     VERSION="$2"; shift 2 ;;
      --image)       IMAGE="$2"; shift 2 ;;
      --platform)    PLATFORM="$2"; shift 2 ;;
      --tier)        TIER="$2"; shift 2 ;;
      --abi-tag)     ABI="$2"; shift 2 ;;
      --min-version) MIN_VERSION="$2"; shift 2 ;;
      --key)         KEY="$2"; shift 2 ;;
      --release-dir) RELEASE_DIR="$2"; shift 2 ;;
      --push)        PUSH_UUID="$2"; shift 2 ;;
      --allow-downgrade) ALLOW_DOWNGRADE=1; shift ;;
      --keytool)     KEYTOOL="$2"; shift 2 ;;
      --force)       FORCE=1; shift ;;
      -h|--help)     sed -n '3,36p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
      *) die "unknown option: $1" ;;
   esac
done

[ -n "$VERSION" ] || die "--version X.Y.Z is required (or: ota-release.sh keygen)"

# Validate operator-supplied tokens to a safe charset BEFORE they reach the sed
# header-bump, the keytool args, or the release-store path.  Blocks shell/sed
# metacharacters and path traversal in $VERSION / $ABI / $MIN_VERSION.
safe_token() { case "$1" in '' | *[!0-9A-Za-z._-]*) return 1 ;; *) return 0 ;; esac; }
safe_token "$VERSION" || die "--version must match [0-9A-Za-z._-] (got '$VERSION')"
safe_token "$ABI" || die "--abi-tag must match [0-9A-Za-z._-] (got '$ABI')"
[ -z "$MIN_VERSION" ] || safe_token "$MIN_VERSION" ||
   die "--min-version must match [0-9A-Za-z._-] (got '$MIN_VERSION')"
case "$PLATFORM" in rpi | esp32) ;; *) die "--platform must be rpi or esp32 (got '$PLATFORM')" ;; esac
case "$TIER" in '' | *[!0-9]*) die "--tier must be numeric (got '$TIER')" ;; esac

[ "$(id -u)" -eq 0 ] || die "run as root (reads $KEY, writes the daemon release store)"
[ -f "$KEY" ] || die "no signing key at $KEY — run: sudo $0 keygen"
[ -x "$KEYTOOL" ] || die "ota-keytool not found at $KEYTOOL (build it: make -C build-debug ota-keytool)"
[ -f "$IMAGE" ] || die "image not found: $IMAGE (build it: SAT_BUILD=full ./dawn_satellite/build-pi.sh)"

# Version consistency: the version we SIGN as must equal the version COMPILED into the
# binary (what the device reports at registration).  A mismatch means the daemon can
# never finalize the push as success — the exact silent-failure this guards against.
bin_version="$(image_fw_version "$IMAGE")"
if [ "$bin_version" != "$VERSION" ]; then
   shown="${bin_version:-<no DAWN_SAT_FW_VERSION marker — stale/old binary>}"
   if [ "$FORCE" -eq 1 ]; then
      echo "ota-release: WARNING (--force): signing as $VERSION over a binary reporting '$shown'" >&2
   elif [ -t 0 ] && [ -t 1 ]; then
      {
         echo "Version mismatch:"
         echo "  sign as (--version): $VERSION"
         echo "  binary reports:      $shown"
         echo "A device flashed with this image would report '$shown', so the daemon could"
         echo "never finalize the $VERSION push as success."
      } >&2
      if [ "$IMAGE" = "$DEFAULT_IMAGE" ]; then
         printf "Bump %s to %s and rebuild now? [y/N] " "$(basename "$VERSION_HEADER")" "$VERSION" >&2
         read -r ans
         case "$ans" in
            y | Y | yes | YES)
               [ -w "$VERSION_HEADER" ] || die "cannot write $VERSION_HEADER"
               sed -i "s/^\(#define DAWN_SATELLITE_FIRMWARE_VERSION \).*/\1\"$VERSION\"/" "$VERSION_HEADER"
               echo "==> bumped $(basename "$VERSION_HEADER") → $VERSION; rebuilding (SAT_BUILD=full)…" >&2
               SAT_BUILD=full "$REPO_ROOT/dawn_satellite/build-pi.sh"
               bin_version="$(image_fw_version "$IMAGE")"
               [ "$bin_version" = "$VERSION" ] ||
                  die "after rebuild the binary still reports '${bin_version:-<none>}' (expected $VERSION)"
               ;;
            *) die "aborted — bump $(basename "$VERSION_HEADER") to $VERSION and rebuild, or pass --force" ;;
         esac
      else
         die "custom --image can't be auto-rebuilt; rebuild it at $VERSION or pass --force"
      fi
   else
      die "version mismatch: --version=$VERSION but binary reports '$shown'. Bump $(basename "$VERSION_HEADER") to $VERSION and rebuild (SAT_BUILD=full ./dawn_satellite/build-pi.sh), or pass --force."
   fi
fi

# min_version defaults to EMPTY (no anti-skip gate) so a routine release installs on
# any older device — the whole point of OTA.  Anti-rollback (refuse an OLDER image) is
# automatic in ota_manifest_rollback_ok via the version<installed check; it does NOT
# depend on this floor.  Set --min-version only to gate a required-migration milestone.

# release_dir: explicit > dawn.toml [ota] > default
if [ -z "$RELEASE_DIR" ]; then
   toml="$REPO_ROOT/dawn.toml"
   RELEASE_DIR="$(awk '/^\[ota\]/{f=1;next} /^\[/{f=0} f && /^[[:space:]]*release_dir[[:space:]]*=/ {
                        sub(/^[^=]*=[[:space:]]*/,""); gsub(/"/,""); print; exit }' "$toml" 2>/dev/null || true)"
   RELEASE_DIR="${RELEASE_DIR:-/var/lib/dawn/ota}"
fi

DIR="$RELEASE_DIR/$PLATFORM/$VERSION"
echo "==> staging $PLATFORM/$VERSION into $DIR"
install -d -m 755 "$DIR"
install -m 644 "$IMAGE" "$DIR/image"
sign_args=(sign --sk "$KEY" --image "$DIR/image" --version "$VERSION" \
   --platform "$PLATFORM" --tier "$TIER" --abi-tag "$ABI" --out-dir "$DIR")
[ -n "$MIN_VERSION" ] && sign_args+=(--min-version "$MIN_VERSION")
"$KEYTOOL" "${sign_args[@]}"
echo "==> staged: $DIR/{image,manifest,manifest.sig}  (min_version=${MIN_VERSION:-none} abi=$ABI)"

admin="$REPO_ROOT/build-debug/dawn-admin"
[ -x "$admin" ] || admin="dawn-admin"

# Tell a running daemon to re-scan so this release is pushable without a restart.
# Best-effort: if the daemon is down (or OTA disabled) this just prints an error
# and staging still succeeded — the next daemon start scans release_dir anyway.
echo "==> rescanning daemon release store"
if ! "$admin" ota rescan; then
   echo "    NOTE: rescan failed (daemon down?) — restart the daemon so it sees this release." >&2
fi

if [ -n "$PUSH_UUID" ]; then
   push_args=(ota push --uuid "$PUSH_UUID" --version "$VERSION")
   dg_note=""
   if [ "$ALLOW_DOWNGRADE" -eq 1 ]; then
      push_args+=(--allow-downgrade)
      dg_note=" (allow-downgrade)"
   fi
   echo "==> pushing $VERSION to $PUSH_UUID$dg_note"
   "$admin" "${push_args[@]}"
fi
