#!/bin/bash
#
# Install-test a dawn .deb in a clean Ubuntu 24.04 container, as root:
#
#   docker run --rm -v "$PWD":/repo -w /repo ubuntu:24.04 \
#      packaging/deb/smoke-test.sh build-deb/dawn_<version>_amd64.deb
#
# Checks that apt can install it with nothing else present; that every library
# resolves from /usr/lib/dawn or an apt package; that the daemon boots with the
# packaged config and unit (TTS voice loads, WebUI listens, dawn-admin answers);
# and that reinstall, upgrade, remove and purge keep what they should.  The
# container has no systemd, so starting the service itself is not covered here.
set -euo pipefail

deb="$(realpath "$1")"
export DEBIAN_FRONTEND=noninteractive
fail() {
   echo "SMOKE FAIL: $*" >&2
   exit 1
}

echo "==> install"
apt-get update -qq
apt-get install -y -qq "$deb" >/dev/null

echo "==> libraries"
libs=(/usr/bin/dawn /usr/bin/dawn-admin /usr/lib/dawn/*.so*)
! ldd "${libs[@]}" | grep 'not found' || fail "unresolved libraries"
stray="$(ldd "${libs[@]}" | awk '/=> \//{print $3}' | grep -Ev "^(/usr/lib|/lib)/" || true)"
[ -z "$stray" ] || fail "libraries from outside /usr/lib (bundled or apt): $stray"

echo "==> boot (the unit's own command, environment and working directory)"
dawn-fetch-models
mosquitto -d # no systemd here to start the broker the package depends on
unit=/usr/lib/systemd/system/dawn.service
exec_start="$(sed -n 's/^ExecStart=//p' "$unit")"
pre="$(sed -n 's/^ExecStartPre=+//p' "$unit")"
$pre
[ -z "$($pre)" ] || fail "dawn-tls reissued a current certificate"
mapfile -t unit_env < <(sed -n 's/^Environment=//p' "$unit")
log=/tmp/dawn-smoke.log
(cd "$(sed -n 's/^WorkingDirectory=//p' "$unit")" &&
   exec runuser -u dawn -- env "${unit_env[@]}" $exec_start) >"$log" 2>&1 &
pid=$!
# Started: the admin socket answers (it comes up after the WebUI).
for _ in $(seq 90); do
   dawn-admin ping >/dev/null 2>&1 && break
   kill -0 "$pid" 2>/dev/null || { cat "$log"; fail "daemon exited during startup"; }
   sleep 1
done
dawn-admin ping || { cat "$log"; fail "dawn-admin ping"; }
grep -q 'WebUI: Server started successfully' "$log" || { cat "$log"; fail "WebUI never listened"; }
grep -q 'Loaded TTS voice model' "$log" || { cat "$log"; fail "TTS voice did not load"; }
# espeak-ng finds its data through the path compiled into the bundled fork.
! grep -q 'Failed to initialize TTS engine' "$log" || { cat "$log"; fail "TTS engine (espeak-ng)"; }
curl -fsS --cacert /etc/dawn/ssl/ca.crt -o /dev/null https://localhost:3000/ ||
   fail "HTTPS with the host's CA"
curl -fsS --cacert /etc/dawn/ssl/ca.crt -o /dev/null "https://$(hostname):3000/" ||
   fail "HTTPS by host name"
kill "$pid"
wait "$pid" || true

echo "==> reinstall, upgrade"
echo "# smoke-marker" >>/etc/dawn/dawn.toml
apt-get install -y -qq --reinstall "$deb" >/dev/null
up=/tmp/upgrade
dpkg-deb -R "$deb" "$up"
sed -i 's/^Version: .*/&+smoke1/' "$up/DEBIAN/control"
dpkg-deb --root-owner-group -b "$up" /tmp/dawn-upgrade.deb >/dev/null
apt-get install -y -qq /tmp/dawn-upgrade.deb >/dev/null
grep -q smoke-marker /etc/dawn/dawn.toml || fail "upgrade replaced /etc/dawn/dawn.toml"
[ "$(stat -c %U /etc/dawn/dawn.toml)" = dawn ] || fail "dawn.toml not owned by dawn"
[ "$(stat -c %a /etc/dawn/secrets.toml)" = 600 ] || fail "secrets.toml not mode 600"

echo "==> remove"
apt-get remove -y -qq dawn >/dev/null
[ -f /etc/dawn/dawn.toml ] || fail "remove deleted /etc/dawn/dawn.toml"
[ -d /var/lib/dawn/db ] || fail "remove deleted /var/lib/dawn/db"

echo "==> purge"
apt-get purge -y -qq dawn >/dev/null
[ ! -e /etc/dawn/dawn.toml ] || fail "purge left /etc/dawn/dawn.toml"
[ ! -e /etc/dawn/ssl ] || fail "purge left /etc/dawn/ssl"
[ -d /var/lib/dawn/db ] || fail "purge deleted /var/lib/dawn/db"
[ ! -L /var/lib/dawn/tool_instructions ] || fail "purge left the tool_instructions link"

echo "SMOKE OK"
