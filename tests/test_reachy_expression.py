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
# The robot bridge's expression logic (services/reachy-bridge/expression.py):
# config normalization and move picking.  Standard library only.

import logging
import os
import random
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "services",
                                "reachy-bridge"))
import expression  # noqa: E402

logging.disable(logging.CRITICAL)


class NormalizeTest(unittest.TestCase):
    def test_keeps_known_moves_and_scale(self):
        out = expression.normalize_expressions(
            {"nod": {"moves": ["yes1", "ghost1"], "scale": 0.5}}, ["yes1", "no1"])
        self.assertEqual(out, {"nod": {"moves": ["yes1"], "scale": 0.5}})

    def test_drops_names_with_nothing_playable(self):
        table = {
            "a": {"moves": ["ghost1"]},       # no such move
            "b": {"move": "yes1"},            # the old single-move form
            "c": {"moves": []},
            "d": "yes1",                      # not a table
            "e": {"moves": [7, "yes1"]},      # a non-string entry is skipped
        }
        out = expression.normalize_expressions(table, ["yes1"])
        self.assertEqual(sorted(out), ["e"])
        self.assertEqual(out["e"]["moves"], ["yes1"])

    def test_bad_scale_falls_back(self):
        out = expression.normalize_expressions({"n": {"moves": ["yes1"], "scale": 9}}, None)
        self.assertEqual(out["n"]["scale"], 1.0)

    def test_old_single_move_form_is_named(self):
        with self.assertLogs("reachy-bridge", level="WARNING") as logs:
            logging.disable(logging.NOTSET)
            try:
                expression.normalize_expressions({"n": {"move": "yes1"}}, None)
            finally:
                logging.disable(logging.CRITICAL)
        self.assertIn("old `move =` form", logs.output[0])

    def test_clip_sway_checked(self):
        self.assertEqual(expression.clip_sway({}), 0.3)
        self.assertEqual(expression.clip_sway({"clip_sway": 0.5}), 0.5)
        for bad in ("0.3", -1, 2, True):
            self.assertEqual(expression.clip_sway({"clip_sway": bad}), 0.3)

    def test_without_a_library_every_name_is_kept(self):
        out = expression.normalize_expressions({"n": {"moves": ["anything1"]}}, None)
        self.assertEqual(out["n"]["moves"], ["anything1"])


class PickerTest(unittest.TestCase):
    def test_never_the_same_move_twice_running(self):
        picker = expression.MovePicker(random.Random(1))
        last = None
        for _ in range(500):
            move = picker.pick(["a", "b"])
            self.assertNotEqual(move, last)
            last = move

    def test_shared_move_not_repeated_across_names(self):
        picker = expression.MovePicker(random.Random(2))
        for _ in range(200):
            first = picker.pick(["shared", "x"])
            second = picker.pick(["shared", "y"])
            if first == "shared":
                self.assertNotEqual(second, "shared")

    def test_recent_moves_avoided_while_a_choice_remains(self):
        picker = expression.MovePicker(random.Random(3))
        moves = ["a", "b", "c", "d", "e", "f"]  # six: excluding three leaves a choice
        for _ in range(300):
            before = list(picker.recent)
            move = picker.pick(moves)
            self.assertNotIn(move, before)

    def test_four_moves_are_not_a_fixed_rotation(self):
        picker = expression.MovePicker(random.Random(5))
        picks = [picker.pick(["a", "b", "c", "d"]) for _ in range(200)]
        windows = {tuple(picks[i:i + 4]) for i in range(len(picks) - 3)}
        self.assertGreater(len(windows), 4)

    def test_one_move_name_still_plays(self):
        picker = expression.MovePicker(random.Random(4))
        self.assertEqual([picker.pick(["only"]) for _ in range(3)], ["only"] * 3)


if __name__ == "__main__":
    unittest.main()
