#!/bin/bash
#
# Check a dawn .deb as root inside a clean, systemd-booted container of the Ubuntu
# release it was built for.  check-deb.sh starts that container and runs this
# twice: `install`, then reboots the container, then `reboot`.
#
# Checks that the release's lintian finds no errors; that apt can install it with
# nothing else present; that every library resolves from /usr/lib/dawn or an apt
# package; that install enables dawn.service without starting it; that starting it
# downloads the models (dawn-models.service), makes the TLS certificate
# (dawn-tls), and boots the daemon (TTS loads, WebUI answers over HTTPS with the
# host's CA, dawn-admin answers); that install makes a satellite registration key;
# that a restart keeps the certificate; that an upgrade restarts the running daemon
# (unless policy-rc.d forbids it), keeps the config and gives it back to dawn; that
# it comes up by itself at boot; and that remove and purge stop it and keep or
# delete what they should.
set -euo pipefail

deb="$(realpath "$1")"
phase="$2"
export DEBIAN_FRONTEND=noninteractive
fail() {
   echo "SMOKE FAIL: $*" >&2
   journalctl -u dawn -u dawn-models --no-pager | tail -60 >&2
   exit 1
}

# up: wait until the daemon answers dawn-admin (its admin socket comes up last).
up() {
   for _ in $(seq 120); do
      dawn-admin ping >/dev/null 2>&1 && return
      sleep 1
   done
   fail "dawn did not come up"
}

# restarted PID WHAT: wait for postinst's queued restart to replace PID, then for
# the daemon to come up.
restarted() {
   for _ in $(seq 120); do
      [ "$(systemctl show -p MainPID --value dawn)" = "$1" ] || break
      sleep 1
   done
   [ "$(systemctl show -p MainPID --value dawn)" != "$1" ] || fail "$2 did not restart dawn"
   up
}

install_phase() {
   apt-get update -qq

   echo "==> lintian"
   apt-get install -y -qq lintian >/dev/null
   lintian --fail-on error "$deb" || fail "lintian errors"

   echo "==> install"
   apt-get install -y -qq "$deb" >/dev/null
   units=(/usr/lib/systemd/system/dawn.service /usr/lib/systemd/system/dawn-models.service)
   out="$(systemd-analyze verify "${units[@]}" 2>&1)" || fail "units: $out"
   [ -z "$out" ] || fail "units: $out"
   [ "$(systemctl is-enabled dawn)" = enabled ] || fail "install did not enable dawn"
   grep -Eq '^satellite_registration_key = "[0-9a-f]{64}"$' /etc/dawn/secrets.toml ||
      fail "no satellite registration key in /etc/dawn/secrets.toml"
   ! systemctl -q is-active dawn || fail "install started dawn"

   echo "==> libraries"
   libs=(/usr/bin/dawn /usr/bin/dawn-admin /usr/lib/dawn/*.so*)
   ! ldd "${libs[@]}" | grep 'not found' || fail "unresolved libraries"
   stray="$(ldd "${libs[@]}" | awk '/=> \//{print $3}' | grep -Ev "^(/usr/lib|/lib)/" || true)"
   [ -z "$stray" ] || fail "libraries from outside /usr/lib (bundled or apt): $stray"

   echo "==> start (models, certificate, daemon)"
   systemctl start dawn
   up
   [ "$(systemctl show -p Result --value dawn-models)" = success ] || fail "dawn-models.service"
   [ -s /var/lib/dawn/models/whisper.cpp/ggml-base.en.bin ] || fail "Whisper model not downloaded"
   log="$(journalctl -u dawn --no-pager)"
   grep -q 'WebUI: Server started successfully' <<<"$log" || fail "WebUI never listened"
   grep -q 'Loaded TTS voice model' <<<"$log" || fail "TTS voice did not load"
   # espeak-ng finds its data through the path compiled into the bundled fork.
   ! grep -q 'Failed to initialize TTS engine' <<<"$log" || fail "TTS engine (espeak-ng)"
   curl -fsS --cacert /etc/dawn/ssl/ca.crt -o /dev/null https://localhost:3000/ ||
      fail "HTTPS with the host's CA"
   curl -fsS --cacert /etc/dawn/ssl/ca.crt -o /dev/null "https://$(hostname):3000/" ||
      fail "HTTPS by host name"
   dawn-fetch-models >/dev/null || fail "dawn-fetch-models as root" # drops to dawn

   echo "==> restart (the certificate is kept)"
   systemctl restart dawn
   up
   [ "$(journalctl -u dawn --no-pager | grep -c 'dawn-tls: issuing')" = 1 ] ||
      fail "dawn-tls reissued a current certificate"

   echo "==> reinstall, upgrade (the running daemon is restarted, the config kept and given back to dawn)"
   echo "# smoke-marker" >>/etc/dawn/dawn.toml
   pid="$(systemctl show -p MainPID --value dawn)"
   printf '#!/bin/sh\nexit 101\n' >/usr/sbin/policy-rc.d # the admin forbids restarts
   chmod 755 /usr/sbin/policy-rc.d
   apt-get install -y -qq --reinstall "$deb" >/dev/null
   rm /usr/sbin/policy-rc.d
   sleep 5
   # Same process, active, and no stop or restart queued: a slow stop would keep the PID.
   [ "$(systemctl show -p MainPID --value dawn)" = "$pid" ] &&
      [ "$(systemctl show -p ActiveState --value dawn)" = active ] &&
      [ -z "$(systemctl list-jobs --no-legend dawn.service)" ] ||
      fail "restarted despite policy-rc.d"
   apt-get install -y -qq --reinstall "$deb" >/dev/null
   restarted "$pid" reinstall
   chown root:root /etc/dawn/dawn.toml /etc/dawn/secrets.toml
   chmod 644 /etc/dawn/secrets.toml
   pid="$(systemctl show -p MainPID --value dawn)"
   dpkg-deb -R "$deb" /tmp/upgrade
   sed -i 's/^Version: .*/&+smoke1/' /tmp/upgrade/DEBIAN/control
   dpkg-deb --root-owner-group -b /tmp/upgrade /tmp/dawn-upgrade.deb >/dev/null
   apt-get install -y -qq /tmp/dawn-upgrade.deb >/dev/null
   restarted "$pid" upgrade
   grep -q smoke-marker /etc/dawn/dawn.toml || fail "upgrade replaced /etc/dawn/dawn.toml"
   [ "$(stat -c %U /etc/dawn/dawn.toml)" = dawn ] || fail "dawn.toml not owned by dawn"
   [ "$(stat -c %U /etc/dawn/secrets.toml)" = dawn ] || fail "secrets.toml not owned by dawn"
   [ "$(stat -c %a /etc/dawn/secrets.toml)" = 600 ] || fail "secrets.toml not mode 600"
}

# After check-deb.sh reboots the container.
reboot_phase() {
   echo "==> boot (dawn came up by itself)"
   up
   systemctl -q is-active dawn || fail "dawn not active after boot"

   echo "==> remove"
   apt-get remove -y -qq dawn >/dev/null
   ! systemctl -q is-active dawn || fail "remove left dawn running"
   [ -f /etc/dawn/dawn.toml ] || fail "remove deleted /etc/dawn/dawn.toml"
   [ -d /var/lib/dawn/db ] || fail "remove deleted /var/lib/dawn/db"

   echo "==> purge"
   apt-get purge -y -qq dawn >/dev/null
   [ ! -e /etc/dawn/dawn.toml ] || fail "purge left /etc/dawn/dawn.toml"
   [ ! -e /etc/dawn/ssl ] || fail "purge left /etc/dawn/ssl"
   [ -d /var/lib/dawn/db ] || fail "purge deleted /var/lib/dawn/db"
   [ ! -L /var/lib/dawn/tool_instructions ] || fail "purge left the tool_instructions link"
   [ -z "$(find /etc/systemd -name 'dawn*' -print -quit)" ] || fail "purge left systemd links"
}

case "$phase" in
   install) install_phase ;;
   reboot)
      reboot_phase
      echo "SMOKE OK"
      ;;
   *) fail "phase must be install or reboot" ;;
esac
