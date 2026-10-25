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
# The bridge's expression logic that needs no robot: turning the [expressions]
# config into something safe to play, and picking a move for a name.  Standard
# library only, so its tests run anywhere (tests/test_reachy_expression.py).

import logging
import random

log = logging.getLogger("reachy-bridge")

RECENT_MOVES = 3  # avoided, across all names, while that still leaves a choice


def normalize_expressions(table: dict, known_moves) -> dict:
    """The [expressions] config as {name: {"moves": [...], "scale": s}}, keeping
    only moves the library has.  A name left with no move is dropped (DAWN's
    expression for it then logs as unmapped).  Warns once per problem."""
    known = set(known_moves) if known_moves is not None else None
    out = {}
    for name, spec in table.items():
        moves = spec.get("moves") if isinstance(spec, dict) else None
        if isinstance(spec, dict) and "move" in spec and moves is None:
            log.warning("expression %s: old `move =` form; copy [expressions] from the shipped "
                        "reachy-bridge.toml", name)
            continue
        if not isinstance(moves, list) or not moves:
            log.warning("expression %s: needs moves = [...]", name)
            continue
        good = []
        for move in moves:
            if not isinstance(move, str):
                log.warning("expression %s: %r is not a move name", name, move)
            elif known is not None and move not in known:
                log.warning("expression %s: no move %r in the library", name, move)
            else:
                good.append(move)
        if not good:
            continue
        scale = spec.get("scale", 1.0)
        if not isinstance(scale, (int, float)) or not 0.0 < scale <= 1.5:
            log.warning("expression %s: scale %r out of range, using 1.0", name, scale)
            scale = 1.0
        out[name] = {"moves": good, "scale": float(scale)}
    return out


def clip_sway(moments: dict) -> float:
    """[moments] clip_sway, checked: the share of speech motion kept under a clip."""
    value = moments.get("clip_sway", 0.3)
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not 0.0 <= value <= 1.0:
        log.warning("[moments] clip_sway %r must be 0..1, using 0.3", value)
        return 0.3
    return float(value)


class MovePicker:
    """A random move for a name: never the one just played (by any name), and none
    of the last RECENT_MOVES while at least two others remain.  The same gesture
    twice running reads as a loop."""

    def __init__(self, rng: random.Random | None = None):
        self.rng = rng or random.Random()
        self.recent: list[str] = []

    def pick(self, moves: list[str]) -> str:
        # Recent moves are avoided only while that still leaves a choice: one
        # candidate left would turn the list into a fixed rotation.
        fresh = [m for m in moves if m not in self.recent]
        not_last = [m for m in moves if not self.recent or m != self.recent[-1]]
        candidates = fresh if len(fresh) >= 2 else not_last
        move = self.rng.choice(candidates or moves)  # only a one-move name repeats
        self.recent = (self.recent + [move])[-RECENT_MOVES:]
        return move
