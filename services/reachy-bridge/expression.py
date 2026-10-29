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


# --------------------------------------------------------------------------
# Moments: clips the bridge starts itself, and when any clip ends
# --------------------------------------------------------------------------

GREET_EXPIRE_S = 30.0  # a greeting not used by then is dropped (a later alarm never greets)


def moment_seconds(moments: dict, key: str, default: float) -> float:
    """A [moments] duration, checked: positive seconds."""
    value = moments.get(key, default)
    if isinstance(value, bool) or not isinstance(value, (int, float)) or value <= 0:
        log.warning("[moments] %s %r must be positive seconds, using %s", key, value, default)
        return default
    return float(value)


def clip_should_end(kind: str, mode: str, tag_queued: bool, sleep_reason) -> bool:
    """Whether the playing clip should fade out now.  A tag from the model
    always wins; otherwise each kind lasts while its moment does."""
    if tag_queued:
        return True
    if kind == "model":
        return mode != "speaking"  # the expression lasts while she speaks
    if kind == "think":
        return mode != "thinking"
    if kind == "greet":
        return mode == "listening"  # the user started talking
    if kind == "yawn":
        return sleep_reason != "idle"  # something happened: stay awake
    return False


class ThinkTimer:
    """The long-think clip: after after_s of continuous thinking, and again only
    repeat_gap_s after the last one ended, so a long wait gets an occasional
    look, not a loop.  Each new stretch of thinking (a tool loop thinks, speaks
    a line, thinks again) starts over."""

    def __init__(self, after_s: float, repeat_gap_s: float):
        self.after = after_s
        self.gap = repeat_gap_s
        self.since = None
        self.last_end = None

    def update(self, thinking: bool, now: float) -> None:
        if not thinking:
            self.since = None
        elif self.since is None:
            self.since = now
            self.last_end = None

    def due(self, now: float) -> bool:
        return (self.since is not None and now - self.since >= self.after and
                (self.last_end is None or now - self.last_end >= self.gap))

    def clip_ended(self, now: float) -> None:
        self.last_end = now


class Greeting:
    """A wave when the robot wakes from sleep, shown on the next sentence DAWN
    says if that sentence brought no tag of its own (a tagged one wins, and the
    greeting is dropped, not saved).  DAWN starting is the same moment: by then
    the robot has gone to sleep, and DAWN's untagged boot line is the sentence."""

    def __init__(self, speaking_since: float):
        self.pending_at = None
        self.seen = speaking_since  # DAWN's last sentence start already seen

    def woke(self, now: float) -> None:
        self.pending_at = now

    def sentence(self, speaking_since: float, now: float, free: bool) -> bool:
        """Called every tick with DAWN's latest sentence start.  True when a new
        sentence should carry the greeting; @p free is no clip playing and no
        tag waiting."""
        if speaking_since == self.seen:
            return False
        self.seen = speaking_since
        pending, self.pending_at = self.pending_at, None
        return pending is not None and now - pending <= GREET_EXPIRE_S and free
