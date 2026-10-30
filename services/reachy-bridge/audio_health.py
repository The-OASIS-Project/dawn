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
# The robot's mic and speaker health, decided without touching them: when a
# stalled mic array should be rebooted, and what a lopsided speaker should be
# set to.  Standard library only (tests/test_reachy_audio_health.py).

import re

BACK_AFTER_S = 2.0  # this long of sound ends an outage (not one click as the device returns)
RETRY_S = 60.0  # a second reboot this long after the first, then give up until sound
MAX_REBOOTS = 2  # a silent room may read as exact zeros too: never more than this
BALANCE_TOLERANCE = 655  # 1% of PulseAudio's 100% (65536)

_VOLUME = re.compile(r"[\w-]+:\s*(\d+)\s*/")


class MicWatch:
    """The mic array has stalled when it has sent nothing but exact zeros (or
    nothing at all) for stalled_after_s.  A working mic in a quiet room still
    reads a little noise.  An outage is rebooted at most MAX_REBOOTS times, only
    when the caller says DAWN is idle, and ends after BACK_AFTER_S of sound."""

    def __init__(self, stalled_after_s: float, now: float):
        self.stalled_after = stalled_after_s
        self.stalled = False
        self.reboots = 0
        self.next_reboot = 0.0
        self.gave_up = False
        self.last_sound = now
        self.sound_since = None

    def restart(self, now: float) -> None:
        """Reading from a fresh stream (a new source, or the mic just unmuted):
        it gets the whole window again."""
        self.last_sound = now
        self.sound_since = None

    def feed(self, silent: bool, now: float) -> bool:
        """One block read (or a read that timed out: silent).  True when this
        ends an outage."""
        if silent:
            self.sound_since = None
            return False
        self.last_sound = now
        if self.sound_since is None:
            self.sound_since = now
        if self.stalled and now - self.sound_since >= BACK_AFTER_S:
            self.stalled = False
            return True
        return False

    def due(self, now: float) -> bool:
        """Silent long enough to call it a stall (the caller rules out a mute first)."""
        return not self.stalled and now - self.last_sound >= self.stalled_after

    def cancel(self, now: float) -> None:
        """The zeros were asked for (a mute): no outage, and a fresh window."""
        self.stalled = False
        self.restart(now)

    def stall(self, now: float) -> None:
        self.stalled = True
        self.reboots = 0
        self.next_reboot = now
        self.gave_up = False

    def should_reboot(self, now: float, dawn_idle: bool) -> bool:
        # Idle only: the reboot drops the speaker too, so never mid-reply.
        return (self.stalled and dawn_idle and self.reboots < MAX_REBOOTS and
                now >= self.next_reboot)

    def rebooted(self, now: float) -> None:
        self.reboots += 1
        self.next_reboot = now + RETRY_S

    def give_up(self, now: float) -> bool:
        """True once, when the last reboot had its RETRY_S and the mic is still silent."""
        if (self.stalled and not self.gave_up and self.reboots >= MAX_REBOOTS and
                now >= self.next_reboot):
            self.gave_up = True
            return True
        return False


def parse_volumes(text: str) -> list[int]:
    """The per-channel raw volumes from `pactl get-sink-volume` (or -source-)."""
    line = text.splitlines()[0] if text else ""
    return [int(v) for v in _VOLUME.findall(line)]


def balance_target(volumes: list[int]) -> int | None:
    """The volume to set every channel to, or None when they already match.
    The mic array takes its echo-cancellation reference from the left channel,
    so a low left channel lets every reply back in as input; both go to the
    louder one, which keeps the loudness the user chose."""
    if len(volumes) < 2 or max(volumes) - min(volumes) <= BALANCE_TOLERANCE:
        return None
    return max(volumes)
