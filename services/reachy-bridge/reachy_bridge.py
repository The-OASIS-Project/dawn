#!/usr/bin/env python3
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
# reachy-bridge: gives DAWN's local voice interface a body.
#
# Reads DAWN's OCP events (dawn/events, dawn/status) and drives a Reachy Mini
# through Pollen's SDK.  One mixer thread is the only thing that moves the
# robot: it composes idle motion, the posture for what DAWN is doing, the gaze
# toward whoever said the wake word, and expression clips, and adds small head
# offsets that follow DAWN's speech (read from the PulseAudio monitor of its
# playback sink).  It accepts nothing from MQTT but DAWN's state, a short list
# of speaking events and expression names from a fixed vocabulary.

import argparse
import json
import logging
import math
import os
import queue
import select
import signal
import socket
import subprocess
import sys
import threading
import time
import tomllib
import urllib.request
from dataclasses import dataclass, field

import numpy as np
import paho.mqtt.client as mqtt
import websockets.exceptions
from scipy.spatial.transform import Rotation

# The bridge's own modules sit beside it (python -I leaves its directory off sys.path).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audio_health  # noqa: E402
import expression  # noqa: E402

from reachy_mini import ReachyMini
from reachy_mini.io.protocol import SetSpeechOffsetsCmd
from reachy_mini.media.audio_control_utils import init_respeaker_usb
from reachy_mini.media.audio_doa import AudioDoA
from reachy_mini.motion.recorded_move import RecordedMoves
from reachy_mini.utils import create_head_pose

log = logging.getLogger("reachy-bridge")

STATUS_TOPIC = "reachy/status"
EVENTS_TOPIC = "dawn/events"
DAWN_STATUS_TOPIC = "dawn/status"
HEARTBEAT_SEC = 30
HEALTHY_S = 10.0  # connected this long: the daemon is really back
MAX_PAYLOAD = 4096  # DAWN's events are a few hundred bytes
VERSION = "0.1.0"


# --------------------------------------------------------------------------
# Poses: (x, y, z) in mm and (roll, pitch, yaw) in degrees, added layer by layer
# --------------------------------------------------------------------------


@dataclass
class Pose:
    xyz: np.ndarray = field(default_factory=lambda: np.zeros(3))
    rpy: np.ndarray = field(default_factory=lambda: np.zeros(3))
    antennas: np.ndarray = field(default_factory=lambda: np.zeros(2))  # rad, offsets

    def __add__(self, other: "Pose") -> "Pose":
        return Pose(self.xyz + other.xyz, self.rpy + other.rpy, self.antennas + other.antennas)

    def scaled(self, k: float) -> "Pose":
        return Pose(self.xyz * k, self.rpy * k, self.antennas * k)

    @staticmethod
    def lerp(a: "Pose", b: "Pose", t: float) -> "Pose":
        return a.scaled(1.0 - t) + b.scaled(t)

    def close_to(self, other: "Pose") -> bool:
        """Within what the servos can show (about 0.1 degree, a tenth of a mm)."""
        return (np.max(np.abs(self.xyz - other.xyz)) < 0.05 and
                np.max(np.abs(self.rpy - other.rpy)) < 0.05 and
                np.max(np.abs(self.antennas - other.antennas)) < 0.001)


def pose_from_table(t: dict) -> Pose:
    return Pose(
        np.array(t.get("xyz_mm", [0, 0, 0]), dtype=float),
        np.array(t.get("rpy_deg", [0, 0, 0]), dtype=float),
        np.radians(np.array(t.get("antennas_deg", [0, 0]), dtype=float)),
    )


def pose_from_matrix(head: np.ndarray, antennas, neutral_antennas: np.ndarray) -> Pose:
    rpy = Rotation.from_matrix(head[:3, :3]).as_euler("xyz", degrees=True)
    return Pose(head[:3, 3] * 1000.0, rpy, np.asarray(antennas, dtype=float) - neutral_antennas)


@dataclass
class Clip:
    kind: str     # model (a tag), think, greet or yawn: each ends its own way
    move: object  # a RecordedMove
    name: str     # the move's name, for the log
    started: float
    end: float    # seconds into the clip; shortened when it fades out early
    scale: float


def ease(t: float) -> float:
    t = min(max(t, 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


# --------------------------------------------------------------------------
# What DAWN is doing (fed by MQTT, read by the mixer)
# --------------------------------------------------------------------------


class DawnState:
    def __init__(self, cfg: dict):
        self.lock = threading.Lock()
        self.cfg = cfg
        self.dawn_online = False
        self.state = "SILENCE"
        self.listening_until = 0.0
        self.speaking = False
        self.speaking_end_at = 0.0
        self.speaking_since = 0.0
        self.ack_next = False  # a bare wake word: DAWN's next sentence acknowledges it
        self.sentence_is_ack = False  # the sentence now starting is that acknowledgement
        # As if DAWN had only just gone: until its retained status arrives, a
        # starting bridge mustn't put the robot through a sleep and a rise.
        self.offline_since = time.monotonic()
        self.last_activity = time.monotonic()  # a turn or speech: keeps the robot awake
        self.expressions: "queue.Queue[str]" = queue.Queue(maxsize=4)
        self.last_expression = 0.0
        self.unknown_names: set[str] = set()
        self.changed = threading.Event()  # wakes the mixer: a slow tick never delays a reaction

    def reset(self, online: bool) -> None:
        """DAWN came or went: nothing it said before still holds (a crash mid-reply
        never sends speaking_end)."""
        with self.lock:
            if online and not self.dawn_online:
                self.last_activity = time.monotonic()  # DAWN starting wakes it
            if not online and self.dawn_online:
                self.offline_since = time.monotonic()
            self.dawn_online = online
            self.state = "SILENCE"
            self.speaking = False
            self.listening_until = 0.0
        self.drain_expressions()
        self.changed.set()

    def drain_expressions(self) -> None:
        """Expressions waiting belong to speech that is over."""
        while True:
            try:
                self.expressions.get_nowait()
            except queue.Empty:
                return

    def handle(self, msg: dict, doa: "DoaTracker") -> None:
        self.apply(msg, doa)
        self.changed.set()

    def apply(self, msg: dict, doa: "DoaTracker") -> None:
        event = msg.get("event")
        now = time.monotonic()
        with self.lock:
            if event == "state_change":
                state = msg.get("state")
                if not isinstance(state, str) or state not in (
                        "SILENCE", "WAKEWORD_LISTEN", "COMMAND_RECORDING", "PROCESS_COMMAND"):
                    return
                self.state = state
                if state == "PROCESS_COMMAND":
                    self.ack_next = False  # its "Hello." never came (a discard): not the reply
                    self.last_activity = now  # a typed command wakes it too
                if state == "WAKEWORD_LISTEN":
                    doa.open_window()
                elif state == "COMMAND_RECORDING":
                    self.listening_until = now + self.cfg["timing"]["listen_hold_s"]
            elif event == "wake_word":
                self.ack_next = msg.get("with_command") is False  # DAWN says "Hello." next
                self.last_activity = now
                self.listening_until = now + self.cfg["timing"]["listen_hold_s"]
                doa.aim_from_window()
            elif event == "speaking_start":
                self.sentence_is_ack, self.ack_next = self.ack_next, False
                self.speaking = True
                self.speaking_since = now
                self.last_activity = now  # an alarm or announcement wakes it
            elif event == "speaking_end":
                self.speaking = False
                self.speaking_end_at = now
            elif event == "expression":
                name = msg.get("name")
                if not isinstance(name, str):
                    return
                if name not in self.cfg["expressions"]:
                    # DAWN's vocabulary grew past this config: say so once.
                    if name not in self.unknown_names and len(self.unknown_names) < 32:
                        self.unknown_names.add(name)
                        log.warning("expression %r has no move in [expressions]", name[:32])
                    return
                if now - self.last_expression < self.cfg["timing"]["expression_min_gap_s"]:
                    return
                self.last_expression = now
                try:
                    self.expressions.put_nowait(name)
                except queue.Full:
                    pass

    def sleep_reason(self) -> str | None:
        """None while it should be awake: DAWN online and something happened in
        the last sleep_after_s.  "idle" when the timer ran out (the robot yawns
        first), "offline" when DAWN is gone (it sleeps at once).  Speech near the
        mic alone (WAKEWORD_LISTEN: the TV, people talking) doesn't count, or it
        would never sleep."""
        # Briefly offline counts as online: a quick reconnect (its will says
        # offline, then online again) shouldn't put it through a sleep and wake.
        if (not self.dawn_online and
                time.monotonic() - self.offline_since >= self.cfg["timing"]["offline_grace_s"]):
            return "offline"
        if self.mode() != "idle":
            return None
        if time.monotonic() - self.last_activity < self.cfg["idle"].get("sleep_after_s", 900):
            return None
        return "idle"

    def mode(self) -> str:
        """idle / listening / thinking / speaking, with the speaking gaps debounced."""
        now = time.monotonic()
        with self.lock:
            hold = self.cfg["timing"]["speaking_hold_s"]
            # A speaking_end that never came (DAWN's link dropped mid-sentence;
            # the events aren't retained) ends with the sentence that set it.
            if self.speaking and now - self.speaking_since > self.cfg["timing"].get("speaking_max_s", 60):
                self.speaking = False
                self.speaking_end_at = now
            if self.speaking or now - self.speaking_end_at < hold:
                return "speaking"
            if self.state == "PROCESS_COMMAND":
                return "thinking"
            if self.state == "COMMAND_RECORDING" or now < self.listening_until:
                return "listening"
            return "idle"


# --------------------------------------------------------------------------
# Direction of arrival, read straight from the XVF3800 (the daemon runs --no-media)
# --------------------------------------------------------------------------


class DoaTracker:
    REOPEN_S = 5.0

    def __init__(self, cfg: dict):
        self.cfg = cfg
        self.lock = threading.Lock()
        self.window: list[float] = []
        self.collecting = False
        self.window_opened = 0.0
        self.active = threading.Event()  # polling: only around a wake word
        self.gaze_yaw = 0.0  # degrees, left positive
        self.gaze_at = 0.0
        self.doa = None
        self.doa_down_logged = False
        self.next_open = 0.0
        self.open()

    def open(self) -> None:
        """Open the mic array's direction reading.  Switching the robot off resets
        the array (it stays on USB power), which leaves an open handle dead, so this
        runs again after any failed read; at most every REOPEN_S."""
        self.next_open = time.monotonic() + self.REOPEN_S
        try:
            doa = AudioDoA()
            if not doa.available:
                doa = None
        except Exception:  # pyusb permissions, firmware, the device mid-reset
            doa = None
        self.doa = doa
        if doa is None and not self.doa_down_logged:
            log.warning("DoA: no ReSpeaker reachable; gaze stays centred until it is")
            self.doa_down_logged = True

    def open_window(self) -> None:
        with self.lock:
            self.window = []
            self.collecting = True
            self.window_opened = time.monotonic()
        self.active.set()

    def aim_from_window(self) -> None:
        self.active.clear()
        with self.lock:
            self.collecting = False
            if not self.window:
                return
            angle = float(np.median(self.window))
            # 0 = left, pi/2 = front, pi = right; the head's yaw is left-positive.
            yaw = math.degrees(math.pi / 2.0 - angle)
            limit = self.cfg["gaze"]["max_yaw_deg"]
            self.gaze_yaw = max(-limit, min(limit, yaw))
            self.gaze_at = time.monotonic()
            log.info("gaze: %.0f deg from %d readings", self.gaze_yaw, len(self.window))

    def run(self, stop: threading.Event) -> None:
        period = 1.0 / self.cfg["gaze"]["poll_hz"]
        window_max = self.cfg["gaze"]["window_max_s"]
        while not stop.is_set():
            # The mic array carries DAWN's capture too: no traffic on it except
            # while someone may be saying the wake word.
            if not self.active.wait(0.5):
                continue
            if time.monotonic() - self.window_opened > window_max:
                self.active.clear()  # speech that held no wake word
                continue
            if self.doa is None:
                if time.monotonic() >= self.next_open:
                    self.open()
                if self.doa is None:
                    stop.wait(period)
                    continue
            try:
                reading = self.doa.get_DoA()
                if self.doa_down_logged:  # a good read, not just an open
                    log.info("DoA: ReSpeaker reachable again")
                    self.doa_down_logged = False
            except Exception as e:
                if not self.doa_down_logged:
                    log.warning("DoA read failed (%s); reopening", e)
                    self.doa_down_logged = True
                try:
                    self.doa.close()
                except Exception:
                    pass
                self.doa = None
                reading = None
            if reading is not None:
                angle, speech = reading
                with self.lock:
                    if self.collecting and speech:
                        self.window.append(angle)
                        del self.window[:-50]
            stop.wait(period)

    def yaw(self, engaged: bool) -> float:
        """The gaze: held through the turn the wake word began (@p engaged: DAWN is
        listening, thinking or speaking), then hold_s more, then back to centre.
        Only a gaze still held is extended: speech with no wake word of its own
        (an alarm) doesn't swing the head back to an old bearing."""
        with self.lock:
            now = time.monotonic()
            age = now - self.gaze_at
            hold = self.cfg["gaze"]["hold_s"]
            fade = self.cfg["gaze"]["return_s"]
            if age < hold:
                if engaged:
                    self.gaze_at = now
                return self.gaze_yaw
            return self.gaze_yaw * (1.0 - ease((age - hold) / fade))


# --------------------------------------------------------------------------
# Speech envelope from the PulseAudio monitor of DAWN's playback sink
# --------------------------------------------------------------------------


PA_ENV = {**os.environ, "LC_ALL": "C"}  # pactl translates its output otherwise ("Stumm: ja")


def pactl(*args: str) -> str:
    """A pactl command's output; empty when PulseAudio can't be reached."""
    try:
        return subprocess.run(["pactl", *args], capture_output=True, text=True, timeout=5,
                              env=PA_ENV).stdout
    except (OSError, subprocess.TimeoutExpired):
        return ""


class PaError(Exception):
    """pactl gave no answer: whether the device is there is unknown."""


def pa_find(kind: str, wanted) -> tuple[str, str] | None:
    """The first source or sink whose name passes @p wanted, as (index, name),
    or None when there is none.  The index changes every time a device comes
    back.  Raises PaError when pactl answers nothing (there are always monitors)."""
    out = pactl("list", "short", kind)
    if not out:
        raise PaError(f"pactl list {kind} gave nothing")
    for line in out.splitlines():
        cols = line.split("\t")
        if len(cols) > 1 and wanted(cols[1]):
            return cols[0], cols[1]
    return None


def spawn_parec(name: str, latency_ms: int) -> subprocess.Popen | None:
    """16 kHz mono s16 from one named source."""
    try:
        return subprocess.Popen(
            ["parec", "--raw", "--format=s16le", "--rate=16000", "--channels=1",
             f"--latency-msec={latency_ms}", "-d", name],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    except OSError as e:
        log.error("parec: %s", e)
        return None


class PaEvents:
    """One `pactl subscribe` for the bridge.  When the robot's audio drops (a
    reboot, a power cycle), PulseAudio moves running streams to another source,
    the mic included, rather than ending them, so the readers must re-find their
    device.  A source, sink or server change bumps `generation` and the readers
    look again only then, instead of running pactl every second."""

    def __init__(self):
        self.generation = 0

    def run(self, stop: threading.Event) -> None:
        while not stop.is_set():
            try:
                proc = subprocess.Popen(["pactl", "subscribe"], stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, text=True, env=PA_ENV)
            except OSError as e:
                log.error("pactl subscribe: %s", e)
                stop.wait(5)
                continue
            self.generation += 1  # anything may have changed while it wasn't running
            try:
                for line in proc.stdout:  # "Event 'change' on sink #217"
                    what = line.split(" on ", 1)
                    if len(what) == 2 and what[1].split(" ", 1)[0] in ("source", "sink", "server"):
                        self.generation += 1
                    if stop.is_set():
                        break
            finally:
                proc.kill()
                proc.wait()
            stop.wait(1)


class SpeechEnvelope:
    FRAME = 320  # 20 ms

    def __init__(self, cfg: dict, events: PaEvents):
        self.source = cfg["speech"]["monitor_source"]
        self.events = events
        self.level = 0.0
        self.attack = cfg["speech"]["attack"]
        self.release = cfg["speech"]["release"]
        self.gain = cfg["speech"]["gain"]

    def resolve(self) -> tuple[str, str] | None:
        """The monitor to read; @DEFAULT_MONITOR@ is resolved here, not by parec,
        so the reader can tell when it should move."""
        name = self.source
        if name == "@DEFAULT_MONITOR@":
            sink = pactl("get-default-sink").strip()
            if not sink:
                return None
            name = sink + ".monitor"
        return pa_find("sources", lambda n: n == name)

    def run(self, stop: threading.Event) -> None:
        while not stop.is_set():
            seen = self.events.generation
            try:
                source = self.resolve()
            except PaError:
                source = None
            proc = spawn_parec(source[1], 20) if source else None
            if proc is None:
                self.level = 0.0
                stop.wait(2)
                continue
            log.info("speech envelope: reading %s", source[1])
            pending = b""  # an odd byte left by a short read
            try:
                fd = proc.stdout.fileno()
                while not stop.is_set():
                    if self.events.generation != seen:
                        seen = self.events.generation
                        try:
                            if self.resolve() != source:
                                break
                        except PaError:
                            seen = -1  # ask again next time round
                    if not select.select([fd], [], [], 0.5)[0]:
                        continue
                    data = os.read(fd, self.FRAME * 2)
                    if not data:
                        log.warning("parec ended; retrying")
                        stop.wait(1)
                        break
                    data, pending = pending + data, b""
                    if len(data) % 2:
                        data, pending = data[:-1], data[-1:]
                    x = np.frombuffer(data, dtype=np.int16).astype(np.float32) / 32768.0
                    rms = float(np.sqrt(np.mean(x * x))) if x.size else 0.0
                    target = min(1.0, rms * self.gain)
                    k = self.attack if target > self.level else self.release
                    self.level += (target - self.level) * k
            finally:
                proc.kill()
                proc.wait()
            self.level = 0.0


# --------------------------------------------------------------------------
# The mic array's health: a stalled mic rebooted, the speaker kept balanced
# --------------------------------------------------------------------------


class AudioHealth:
    """The XVF3800 can stall: PulseAudio keeps delivering samples, all zero,
    with no USB event, and DAWN hears nothing for hours.  Its REBOOT control
    restarts it (~1 s) and PulseAudio puts DAWN's streams back.  Separately,
    the robot's speaker drifts out of balance (PulseAudio restoring an old
    per-channel volume, a settings panel), and a low left channel (the
    echo-cancellation reference) lets every reply back in as input."""

    BLOCK = 3200  # 100 ms of mono s16

    def __init__(self, cfg: dict, dawn: DawnState, events: PaEvents):
        audio = cfg.get("audio", {})
        self.match = audio.get("device_match", "Pollen_Robotics_Reachy_Mini_Audio")
        self.watch = audio_health.MicWatch(float(audio.get("stalled_after_s", 60)),
                                           time.monotonic())
        self.dawn = dawn
        self.events = events
        self.seen = None  # the PulseAudio generation last checked
        self.proc = None
        self.source = None  # (index, name) the reader is on
        self.muted_logged = False

    def robot(self, prefix: str):
        return lambda name: name.startswith(prefix) and self.match in name

    def stop_reader(self) -> None:
        if self.proc is not None:
            self.proc.kill()
            self.proc.wait()
            self.proc = None

    def follow_source(self, now: float) -> None:
        """Read the robot's mic, and only it."""
        source = pa_find("sources", self.robot("alsa_input."))
        if source == self.source and self.proc is not None:
            return
        self.stop_reader()
        self.source = source
        self.watch.restart(now)
        if source is not None:
            self.proc = spawn_parec(source[1], 100)

    def read_second(self, stop: threading.Event) -> None:
        """About a second of the mic into the watch; no data at all counts as silence."""
        end = time.monotonic() + 1.0
        while not stop.is_set():
            left = end - time.monotonic()
            if left <= 0:
                return
            if self.proc is None:
                stop.wait(left)
                self.watch.feed(True, time.monotonic())
                return
            fd = self.proc.stdout.fileno()
            if not select.select([fd], [], [], left)[0]:
                self.watch.feed(True, time.monotonic())
                return
            data = os.read(fd, self.BLOCK)
            if not data:  # parec ended: look for the mic again
                self.stop_reader()
                self.seen = None
                stop.wait(1)
                return
            if self.watch.feed(data.count(0) == len(data), time.monotonic()):
                log.info("Reachy mic back after %d reboot(s)", self.watch.reboots)

    def muted(self, now: float) -> bool:
        """Zeros the user asked for (muted, or at 0%, in PulseAudio): not a stall."""
        if self.source is None:
            return False
        name = self.source[1]
        volumes = audio_health.parse_volumes(pactl("get-source-volume", name))
        if "yes" not in pactl("get-source-mute", name) and not (volumes and max(volumes) == 0):
            self.muted_logged = False
            return False
        if not self.muted_logged:
            log.info("Reachy mic muted in PulseAudio: not checking it")
            self.muted_logged = True
        self.watch.cancel(now)
        return True

    def check_mic(self, now: float) -> None:
        if self.source is not None and self.watch.due(now) and not self.muted(now):
            self.watch.stall(now)
            log.warning("Reachy mic sent only zeros for %.0f s: rebooting the mic array",
                        self.watch.stalled_after)
        if self.watch.should_reboot(now, self.dawn.mode() == "idle"):
            if self.muted(now):  # muted since the stall began
                return
            self.watch.rebooted(now)
            self.reboot()
        elif self.watch.give_up(now):
            log.error("Reachy mic still silent after %d reboots: power-cycle the robot",
                      self.watch.reboots)

    @staticmethod
    def reboot() -> None:
        dev = init_respeaker_usb()
        if dev is None:
            log.error("Reachy mic: no mic array on USB to reboot")
            return
        try:
            dev.write("REBOOT", [1])
        except Exception as e:  # the chip usually resets before the transfer completes
            log.debug("REBOOT transfer: %s", e)
        try:
            dev.close()
        except Exception:
            pass

    def check_balance(self) -> None:
        """On any sink change (a volume change is one): channels kept equal."""
        sink = pa_find("sinks", self.robot("alsa_output."))
        if sink is None:
            return
        volumes = audio_health.parse_volumes(pactl("get-sink-volume", sink[1]))
        target = audio_health.balance_target(volumes)
        if target is None:
            return
        pactl("set-sink-volume", sink[1], *[str(target)] * len(volumes))
        log.warning("Reachy speaker channels were %s: set all to %d%% (the mic array's echo "
                    "cancellation listens to the left one)",
                    "/".join(f"{round(v * 100 / 65536)}%" for v in volumes),
                    round(target * 100 / 65536))

    def run(self, stop: threading.Event) -> None:
        try:
            while not stop.is_set():
                try:
                    if self.events.generation != self.seen:
                        self.seen = self.events.generation
                        self.follow_source(time.monotonic())
                        self.check_balance()
                    self.read_second(stop)
                    self.check_mic(time.monotonic())
                except Exception as e:  # pyusb, pactl not answering: keep watching
                    log.warning("audio health: %s", e)
                    self.seen = None  # look at the devices again
                    stop.wait(5)
        finally:
            self.stop_reader()


# --------------------------------------------------------------------------
# The mixer: the only sender of motion
# --------------------------------------------------------------------------


class Mixer:
    def __init__(self, cfg: dict, mini: ReachyMini, dawn: DawnState, doa: DoaTracker,
                 speech: SpeechEnvelope, moves: RecordedMoves | None):
        self.cfg = cfg
        self.mini = mini
        self.dawn = dawn
        self.doa = doa
        self.speech = speech
        self.moves = moves
        self.neutral_antennas = np.radians(np.array(cfg["neutral"]["antennas_deg"], dtype=float))
        self.postures = {k: pose_from_table(v) for k, v in cfg["posture"].items()}
        self.current = Pose()
        self.clip: Clip | None = None
        self.clip_weight = 0.0  # how much of the clip is showing (its fades)
        self.picker = expression.MovePicker()
        moments = cfg["moments"]
        self.think = expression.ThinkTimer(moments["think_after_s"], moments["think_repeat_gap_s"])
        self.greeting = expression.Greeting(dawn.speaking_since)
        self.yawned = False
        self.awake = None  # unknown until the first decision
        self.speech_sent = True  # clear what an earlier mixer may have left

    # -- layers ---------------------------------------------------------------

    def idle(self, t: float, mode: str) -> Pose:
        i = self.cfg["idle"]
        damp = {"listening": i["listening_damp"], "speaking": i["speaking_damp"]}.get(mode, 1.0)
        breathe = math.sin(2 * math.pi * t / i["breath_period_s"])
        drift = math.sin(2 * math.pi * t / i["antenna_period_s"])
        return Pose(
            np.array([0.0, 0.0, i["breath_z_mm"] * breathe]),
            np.array([0.0, i["breath_pitch_deg"] * breathe, 0.0]),
            np.radians(np.array([i["antenna_drift_deg"], -i["antenna_drift_deg"]])) * drift,
        ).scaled(damp)

    def start_clip(self, kind: str, name: str, spec: dict | None) -> None:
        """@p spec: a normalized {"moves", "scale"} entry (every move exists), or
        None when that expression or moment isn't configured."""
        if self.moves is None or spec is None:
            return
        move_name = self.picker.pick(spec["moves"])
        try:
            move = self.moves.get(move_name)
        except Exception as e:
            log.warning("%s %s: move %s failed to load: %s", kind, name, move_name, e)
            return
        self.clip = Clip(kind, move, move_name, time.monotonic(), move.duration, spec["scale"])
        if kind == "model":
            log.info("expression %s (%s, up to %.1fs)", name, move_name, move.duration)
        else:
            log.info("moment %s (%s, up to %.1fs)", kind, move_name, move.duration)

    def end_clip(self) -> None:
        """Fade the clip out now.  A tag's clip first has its minimum hold (its
        sentence's speech may not have started yet when it does)."""
        age = time.monotonic() - self.clip.started
        if self.clip.kind != "model" or age >= self.cfg["timing"].get("clip_min_s", 1.0):
            self.clip.end = min(self.clip.end, age + self.cfg["timing"]["clip_fade_out_s"])

    def clip_pose(self, base: Pose) -> Pose:
        clip = self.clip
        age = time.monotonic() - clip.started
        if age >= clip.end:
            # How much of each clip gets shown, to judge whether picking moves
            # by sentence length is worth it.
            log.info("clip %s %s played %.1f of %.1f s", clip.kind, clip.name, clip.end,
                     clip.move.duration)
            if clip.kind == "think":
                self.think.clip_ended(time.monotonic())
            self.clip = None
            self.clip_weight = 0.0
            return base
        head, antennas, _ = clip.move.evaluate(min(age, clip.move.duration - 1e-3))
        pose = pose_from_matrix(head, antennas, self.neutral_antennas).scaled(clip.scale)
        fade_in = self.cfg["timing"]["clip_fade_in_s"]
        fade_out = self.cfg["timing"]["clip_fade_out_s"]
        self.clip_weight = min(ease(age / fade_in), ease((clip.end - age) / fade_out))
        # On top of the posture and gaze: the head stays on the speaker.
        return base + pose.scaled(self.clip_weight)

    def speech_offsets(self, mode: str, t: float) -> list[float]:
        if mode != "speaking":
            return [0.0] * 6
        s = self.cfg["speech"]
        # Quieter under a clip, so the gesture reads (reachy-animation does the same).
        sway = self.cfg["moments"]["clip_sway"]  # checked in main()
        level = self.speech.level * (1.0 - (1.0 - sway) * self.clip_weight)
        wobble = math.sin(2 * math.pi * t * s["wobble_hz"])
        return [
            0.0,
            0.0,
            s["z_mm"] * level / 1000.0,
            math.radians(s["roll_deg"] * level * wobble),
            math.radians(s["pitch_deg"] * level),
            math.radians(s["yaw_deg"] * level * wobble * 0.5),
        ]

    # -- the loop -------------------------------------------------------------

    def send(self, pose: Pose) -> None:
        lim = self.cfg["limits"]
        rpy = np.clip(pose.rpy, -np.array(lim["rpy_deg"]), np.array(lim["rpy_deg"]))
        xyz = np.clip(pose.xyz, -np.array(lim["xyz_mm"]), np.array(lim["xyz_mm"]))
        head = create_head_pose(*xyz, *rpy, mm=True, degrees=True)
        antennas = self.neutral_antennas + np.clip(pose.antennas, -math.radians(lim["antenna_deg"]),
                                                   math.radians(lim["antenna_deg"]))
        self.mini.set_target(head=head, antennas=list(antennas))

    def set_awake(self, awake: bool) -> None:
        if awake == self.awake:
            return
        self.awake = awake
        self.clip = None
        self.clip_weight = 0.0
        self.current = Pose()
        if not awake:
            self.dawn.drain_expressions()  # waking keeps the tag of the sentence that woke it
        self.mini.client.send_command(SetSpeechOffsetsCmd(offsets=[0.0] * 6))
        self.speech_sent = False
        # The daemon starts with torque off; asleep, the robot rests in its sleep
        # pose without it (no motors energized all night).  enable_motors pins
        # the targets to the present pose, so nothing jumps.
        if awake:
            log.info("waking up")
            self.mini.enable_motors()
            # The rise only: the SDK's wake_up() ends with a quick 20 degree head
            # flick, which reads as a snap.
            self.mini.goto_target(create_head_pose(0, 0, 0, 0, 0, 0, mm=True, degrees=True),
                                  antennas=list(self.neutral_antennas), duration=2.0)
        else:
            log.info("going to sleep")
            self.mini.goto_sleep()
            self.mini.disable_motors()

    def stay_awake(self, reason: str | None) -> bool:
        """The sleep timer running out yawns first: awake until the yawn (or a
        clip already playing) is over.  DAWN going offline sleeps at once."""
        if reason is None:
            self.yawned = False
            return True
        if reason == "idle" and self.awake:
            if self.clip is None and not self.yawned:
                self.yawned = True
                self.start_clip("yawn", "yawn", self.cfg["moments"]["yawn"])
            # Someone spoke as it was falling asleep: DAWN only knows whether it was the
            # wake word once the utterance ends, so sleep waits for that check
            # (speech alone still doesn't keep it awake).
            return self.clip is not None or self.dawn.state == "WAKEWORD_LISTEN"
        return False

    def moments(self, mode: str, reason: str | None, now: float) -> None:
        """Which clip plays: the model's tags first, then the bridge's own."""
        queued = not self.dawn.expressions.empty()
        # A clip fades out when its moment is over, or for a newer tag (cutting
        # it off would jump the head).
        if self.clip is not None and expression.clip_should_end(self.clip.kind, mode, queued,
                                                                 reason):
            self.end_clip()
        if self.clip is None:
            name = None  # only the newest of the tags waiting is worth showing
            while True:
                try:
                    name = self.dawn.expressions.get_nowait()
                except queue.Empty:
                    break
            if name is not None:
                self.start_clip("model", name, self.cfg["expressions"][name])
        self.think.update(mode == "thinking", now)
        free = self.clip is None and self.dawn.expressions.empty()
        # Not on the acknowledgement of a bare wake word: the user's command comes
        # next, so the wave would be cut off at once (the rise says it).
        started = self.dawn.speaking_since  # first: apply() sets the ack flag before it
        greet_ok = free and not self.dawn.sentence_is_ack
        if self.greeting.sentence(started, now, greet_ok):
            self.start_clip("greet", "greeting", self.cfg["expressions"].get("greeting"))
        elif free and self.think.due(now):
            self.start_clip("think", "thinking", self.cfg["expressions"].get("thinking"))
            if self.clip is None:  # none configured, or it failed to load: not every tick
                self.think.clip_ended(now)

    def run(self, stop: threading.Event) -> None:
        try:
            self.loop(stop)
        # The daemon went away: the robot switched off (the SDK's ConnectionError) or a
        # daemon restart (its websocket's ConnectionClosed).
        except (ConnectionError, websockets.exceptions.ConnectionClosed) as e:
            log.warning("mixer stopped: %s", e)
        except Exception:
            log.exception("mixer stopped")

    def loop(self, stop: threading.Event) -> None:
        timing = self.cfg["timing"]
        rate = timing["mixer_hz"]
        fast = 1.0 / rate
        slow = 1.0 / timing.get("idle_hz", 15)
        smooth = timing["posture_smoothing"]  # per tick at mixer_hz
        t0 = last = time.monotonic()
        while not stop.is_set():
            tick = time.monotonic()
            # Capped at the slow tick: a stall (a slow call) must not turn into one
            # big step of the smoothing, which would snap the head.
            dt, last = min(tick - last, slow), tick
            self.dawn.changed.clear()
            was_awake = self.awake
            reason = self.dawn.sleep_reason()
            self.set_awake(self.stay_awake(reason))
            if self.awake != was_awake:
                # Waking blocked ~2 s in the rise: start timing afresh.
                tick = last = time.monotonic()
                dt = fast
                if self.awake and was_awake is False:  # not the bridge's own first decision
                    self.greeting.woke(tick)
            period = slow
            if self.awake:
                t = tick - t0
                mode = self.dawn.mode()
                self.moments(mode, reason, tick)

                gaze = self.doa.yaw(engaged=mode != "idle")
                target = self.postures[mode] + Pose(rpy=np.array([0.0, 0.0, gaze]))
                # Smoothing by elapsed time, so the slow idle tick changes nothing.
                self.current = Pose.lerp(self.current, target, 1.0 - (1.0 - smooth)**(dt * rate))
                pose = self.current + self.idle(t, mode)
                if self.clip is not None:
                    pose = self.clip_pose(pose)
                self.send(pose)

                offsets = self.speech_offsets(mode, t)
                if any(offsets) or self.speech_sent:
                    self.mini.client.send_command(SetSpeechOffsetsCmd(offsets=offsets))
                    self.speech_sent = any(offsets)
                # Only breathing (nothing else left to move): the slow tick is
                # enough for motion this small and slow.
                settled = (mode == "idle" and self.clip is None and not self.speech_sent and
                           abs(gaze) < 0.05 and self.current.close_to(target))
                period = slow if settled else fast
            self.dawn.changed.wait(max(0.0, period - (time.monotonic() - tick)))
            # Woken early by an event, still never faster than mixer_hz (a flood
            # of messages can't drive the robot faster).
            stop.wait(max(0.0, fast - (time.monotonic() - tick)))
        # The only motion sender puts it to sleep on the way out.
        self.set_awake(False)


# --------------------------------------------------------------------------
# Recovery: the daemon stays in its error state once the motors stop answering
# (the robot switched off), so powering it back on does nothing until it restarts
# --------------------------------------------------------------------------


class DaemonRecovery:
    FIRST_S = 10.0
    MAX_S = 60.0

    def __init__(self, port: int):
        self.base = f"http://127.0.0.1:{port}/api/daemon"
        self.backoff = self.FIRST_S
        self.next_try = 0.0
        self.down_logged = False

    def connected(self) -> None:
        if self.down_logged:
            log.info("Reachy Mini daemon back")
        self.backoff, self.next_try, self.down_logged = self.FIRST_S, 0.0, False

    def failed(self, error: Exception) -> None:
        if not self.down_logged:  # once per outage, not every 2 s
            log.error("Reachy Mini: %s", error)
            self.down_logged = True

    def maybe_restart(self) -> None:
        """Ask a daemon stuck in its error state to restart, backing off from FIRST_S
        to MAX_S while the robot stays off (each restart retries the motors)."""
        now = time.monotonic()
        if now < self.next_try:
            return
        self.next_try = now + self.backoff
        self.backoff = min(self.backoff * 2.0, self.MAX_S)
        try:
            with urllib.request.urlopen(self.base + "/status", timeout=2) as r:
                state = json.load(r).get("state")
            if state != "error":
                return
            req = urllib.request.Request(self.base + "/restart", method="POST")
            with urllib.request.urlopen(req, timeout=5):
                pass
            log.info("Reachy Mini daemon in error (motors off?): asked it to restart")
        except Exception as e:  # daemon not running (systemd restarts it) or busy
            log.debug("daemon recovery: %s", e)


# --------------------------------------------------------------------------
# MQTT: DAWN's events in, our status out
# --------------------------------------------------------------------------


class Link:
    def __init__(self, cfg: dict, dawn: DawnState, doa: DoaTracker):
        self.cfg = cfg
        self.dawn = dawn
        self.doa = doa
        self.online = False
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                  client_id=f"reachy-bridge-{socket.gethostname()}")
        # A bad message must never take the network thread down with it.
        self.client.suppress_exceptions = True
        self.client.will_set(STATUS_TOPIC, self.status_payload("offline", lwt=True), qos=1,
                             retain=True)
        # Credentials only from the environment (an EnvironmentFile, mode 600).
        user = os.environ.get("REACHY_BRIDGE_MQTT_USER")
        if user:
            self.client.username_pw_set(user, os.environ.get("REACHY_BRIDGE_MQTT_PASSWORD"))
        if cfg["mqtt"].get("tls"):
            self.client.tls_set(ca_certs=cfg["mqtt"].get("ca_file") or None)
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message

    @staticmethod
    def status_payload(status: str, lwt: bool = False) -> str:
        return json.dumps({
            "device": "reachy",
            "msg_type": "status",
            "status": status,
            "timestamp": 0 if lwt else int(time.time() * 1000),
            "version": VERSION,
        })

    def on_connect(self, client, userdata, flags, reason_code, properties) -> None:
        if reason_code.is_failure:
            log.error("MQTT connect failed: %s", reason_code)
            return
        log.info("MQTT connected")
        # Events missed while away can't be trusted; the presence stands until
        # dawn/status (retained) says otherwise.
        self.dawn.reset(self.dawn.dawn_online)
        client.subscribe([(EVENTS_TOPIC, 0), (DAWN_STATUS_TOPIC, 1)])
        if self.online:
            self.publish_status(True)

    def on_message(self, client, userdata, msg) -> None:
        try:
            self.dispatch(msg)
        except Exception:
            log.exception("dropped a message on %s", msg.topic)

    def dispatch(self, msg) -> None:
        if len(msg.payload) > MAX_PAYLOAD:
            return
        data = json.loads(msg.payload)
        if not isinstance(data, dict) or data.get("device") != "dawn":
            return
        if msg.topic == DAWN_STATUS_TOPIC:
            online = data.get("status") == "online"
            if online != self.dawn.dawn_online:
                self.dawn.reset(online)
                log.info("DAWN is %s", "online" if online else "offline")
        elif msg.topic == EVENTS_TOPIC and data.get("msg_type") == "event":
            if data.get("surface") == "local":
                self.dawn.handle(data, self.doa)

    def publish_status(self, online: bool) -> None:
        self.online = online
        self.client.publish(STATUS_TOPIC, self.status_payload("online" if online else "offline"),
                            qos=1, retain=True)

    def start(self) -> None:
        m = self.cfg["mqtt"]
        self.client.connect_async(m["host"], m["port"], keepalive=60)
        self.client.loop_start()

    def stop(self) -> None:
        self.publish_status(False)
        self.client.loop_stop()
        self.client.disconnect()


# --------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description="Reachy Mini body for DAWN's local interface")
    ap.add_argument("-c", "--config", default="reachy-bridge.toml")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    with open(args.config, "rb") as f:
        cfg = tomllib.load(f)

    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    signal.signal(signal.SIGINT, lambda *_: stop.set())

    moves = None
    try:
        moves = RecordedMoves(cfg["clips"]["dataset"])
    except Exception as e:
        log.warning("expression clips unavailable (%s): expressions are ignored", e)
    # Before anything reads it: only names with a move the library has.
    cfg["expressions"] = expression.normalize_expressions(
        cfg["expressions"], moves.list_moves() if moves else None)
    m = cfg.setdefault("moments", {})
    m["clip_sway"] = expression.clip_sway(m)
    m["think_after_s"] = expression.moment_seconds(m, "think_after_s", 2.5)
    m["think_repeat_gap_s"] = expression.moment_seconds(m, "think_repeat_gap_s", 4.0)
    yawn = m.get("yawn", {"moves": ["tired1"], "scale": 0.6})  # "" turns it off
    m["yawn"] = expression.normalize_expressions(
        {"yawn": yawn}, moves.list_moves() if moves else None).get("yawn") if yawn else None

    dawn = DawnState(cfg)
    doa = DoaTracker(cfg)
    events = PaEvents()
    speech = SpeechEnvelope(cfg, events)
    health = AudioHealth(cfg, dawn, events)
    link = Link(cfg, dawn, doa)
    link.start()

    threads = [threading.Thread(target=events.run, args=(stop,), daemon=True),
               threading.Thread(target=doa.run, args=(stop,), daemon=True),
               threading.Thread(target=speech.run, args=(stop,), daemon=True),
               threading.Thread(target=health.run, args=(stop,), daemon=True)]
    for th in threads:
        th.start()

    grace = cfg["timing"]["offline_grace_s"]
    down_since = None
    recovery = DaemonRecovery(cfg["daemon"]["port"])
    while not stop.is_set():
        try:
            with ReachyMini(connection_mode="localhost_only", media_backend="no_media",
                            port=cfg["daemon"]["port"]) as mini:
                log.info("connected to the Reachy Mini daemon")
                down_since = None
                connected_at = time.monotonic()
                link.publish_status(True)
                mixer = Mixer(cfg, mini, dawn, doa, speech, moves)
                last_beat = time.monotonic()
                mixer_thread = threading.Thread(target=mixer.run, args=(stop,), daemon=True)
                mixer_thread.start()
                while not stop.is_set() and mixer_thread.is_alive():
                    stop.wait(1)
                    # Healthy only once it has held: a daemon that accepts and then
                    # fails again mustn't reset the restart backoff each time.
                    if connected_at and time.monotonic() - connected_at >= HEALTHY_S:
                        recovery.connected()
                        connected_at = 0.0
                    if time.monotonic() - last_beat >= HEARTBEAT_SEC:
                        link.publish_status(True)
                        last_beat = time.monotonic()
                if stop.is_set():
                    mixer_thread.join()  # it puts the robot to sleep itself
        except Exception as e:
            recovery.failed(e)
        if stop.is_set():
            break
        recovery.maybe_restart()
        # A transient drop (the XVF3800 wedging, a daemon restart) shouldn't flap
        # DAWN's prompt: say offline only if it stays down.
        now = time.monotonic()
        if down_since is None:
            down_since = now
        if link.online and now - down_since >= grace:
            link.publish_status(False)
        stop.wait(2)

    link.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
