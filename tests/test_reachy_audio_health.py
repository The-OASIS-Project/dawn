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
# The robot bridge's mic and speaker health decisions
# (services/reachy-bridge/audio_health.py).  Standard library only.

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "services",
                                "reachy-bridge"))
import audio_health as ah  # noqa: E402


def silence(watch, start, end, step=1.0):
    t = start
    while t <= end:
        watch.feed(True, t)
        t += step


class MicWatchTest(unittest.TestCase):
    def test_short_silence_is_not_a_stall(self):
        w = ah.MicWatch(60, 0.0)
        silence(w, 1, 59)
        self.assertFalse(w.due(59.0))
        w.feed(False, 59.5)
        self.assertFalse(w.due(110.0))
        self.assertTrue(w.due(119.5))

    def test_stall_reboots_only_when_idle(self):
        w = ah.MicWatch(60, 0.0)
        silence(w, 1, 60)
        self.assertTrue(w.due(60.0))
        w.stall(60.0)
        self.assertFalse(w.due(61.0))  # declared once
        self.assertFalse(w.should_reboot(60.0, dawn_idle=False))
        self.assertTrue(w.should_reboot(61.0, dawn_idle=True))

    def test_two_reboots_then_give_up_once(self):
        w = ah.MicWatch(60, 0.0)
        w.stall(60.0)
        w.rebooted(60.0)
        self.assertFalse(w.should_reboot(100.0, True))
        self.assertTrue(w.should_reboot(60.0 + ah.RETRY_S, True))
        w.rebooted(120.0)
        self.assertFalse(w.give_up(150.0))
        self.assertFalse(w.should_reboot(500.0, True))
        self.assertTrue(w.give_up(180.0))
        self.assertFalse(w.give_up(181.0))

    def test_sound_must_last_to_end_an_outage(self):
        w = ah.MicWatch(60, 0.0)
        w.stall(60.0)
        w.rebooted(60.0)
        self.assertFalse(w.feed(False, 61.0))  # a click as the device returns
        w.feed(True, 61.1)
        self.assertFalse(w.feed(False, 62.0))
        self.assertFalse(w.feed(False, 63.5))
        self.assertTrue(w.feed(False, 64.0))
        self.assertFalse(w.stalled)
        w.stall(200.0)  # the next outage starts its reboots over
        self.assertEqual(w.reboots, 0)
        self.assertTrue(w.should_reboot(200.0, True))

    def test_a_mute_cancels_the_outage(self):
        w = ah.MicWatch(60, 0.0)
        w.stall(60.0)
        w.cancel(61.0)
        self.assertFalse(w.should_reboot(61.0, True))
        self.assertFalse(w.due(120.0))
        self.assertTrue(w.due(121.0))

    def test_restart_gives_a_new_stream_the_whole_window(self):
        w = ah.MicWatch(60, 0.0)
        silence(w, 1, 59)
        w.restart(59.0)
        self.assertFalse(w.due(100.0))
        self.assertTrue(w.due(119.0))


class VolumeTest(unittest.TestCase):
    def test_parse_pactl(self):
        text = ("Volume: front-left: 64225 /  98% / -0.53 dB,   front-right: 32112 /  49% / "
                "-18.59 dB\n        balance -0.50\n")
        self.assertEqual(ah.parse_volumes(text), [64225, 32112])
        self.assertEqual(ah.parse_volumes("Volume: mono: 65536 / 100% / 0.00 dB\n"), [65536])
        self.assertEqual(ah.parse_volumes(""), [])

    def test_balance(self):
        self.assertIsNone(ah.balance_target([64225, 64224]))
        self.assertIsNone(ah.balance_target([65536]))
        self.assertIsNone(ah.balance_target([0, 0]))
        self.assertEqual(ah.balance_target([64225, 32112]), 64225)
        self.assertEqual(ah.balance_target([0, 64225]), 64225)


if __name__ == "__main__":
    unittest.main()
