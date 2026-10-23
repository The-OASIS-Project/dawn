#!/bin/bash
#
# Check a dawn .deb in a clean container of the Ubuntu release it was built for,
# booted with systemd so the package's units run as they would on a real host.
# Runs smoke-test.sh inside it, then removes the container.  Needs docker.
#
# Never --privileged: that hands the container the host's devices, and a systemd
# booting with the host's console took down a desktop session.  Instead the
# container gets CAP_SYS_ADMIN (which systemd needs, also for dawn.service's own
# sandboxing) and unconfined AppArmor, only so its init can make its own private
# cgroup tree writable.  It still gets no host devices, its own cgroup namespace,
# read-only /sys and /proc/sys, and Docker's default seccomp profile.
#
#   packaging/deb/check-deb.sh build-deb/dawn_<version>+ubuntu24.04_amd64.deb
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
deb="$(realpath --relative-to="$REPO_ROOT" "$1")" # as the container sees it, under /repo
case "$deb" in ../*) echo "the .deb must be inside the repository: $1" >&2; exit 1 ;; esac
ubuntu="$(sed -n 's/.*+ubuntu\([0-9.]*\)_amd64\.deb$/\1/p' <<<"$deb")"
[ -n "$ubuntu" ] || { echo "not a dawn_*+ubuntu<release>_amd64.deb: $deb" >&2; exit 1; }
image="dawn-systemd:$ubuntu"
name="dawn-check-$$"

# The ubuntu images' policy-rc.d refuses every service action from maintainer
# scripts; a real host has none, so neither does this one.  SIGRTMIN+3 is
# systemd's "shut down", so the restart below is a clean reboot.
docker build -q -t "$image" - >/dev/null <<EOF
FROM ubuntu:$ubuntu
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
       --no-install-recommends systemd systemd-sysv \
    && rm -rf /var/lib/apt/lists/* /usr/sbin/policy-rc.d
STOPSIGNAL SIGRTMIN+3
CMD ["sh", "-c", "mount -o remount,rw /sys/fs/cgroup && exec /sbin/init"]
EOF

trap 'docker rm -f "$name" >/dev/null 2>&1 || true' EXIT
docker run -d --name "$name" --cap-add SYS_ADMIN --security-opt apparmor=unconfined \
   --tmpfs /run --tmpfs /run/lock -v "$REPO_ROOT":/repo:ro "$image" >/dev/null
# booted: wait for systemd to start, then for its boot to finish (running, or
# degraded: some units want hardware a container lacks).
booted() {
   docker exec "$name" timeout 600 sh -c 'until [ -S /run/systemd/private ]; do sleep 0.2; done
      systemctl is-system-running --wait' >/dev/null || true
}
booted
docker exec -w /repo "$name" packaging/deb/smoke-test.sh "$deb" install
docker restart -t 90 "$name" >/dev/null # a reboot: the enabled service must start itself
booted
docker exec -w /repo "$name" packaging/deb/smoke-test.sh "$deb" reboot
