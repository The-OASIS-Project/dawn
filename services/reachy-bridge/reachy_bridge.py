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
import signal
import socket
import subprocess
import sys
import threading
import time
import tomllib
from dataclasses import dataclass, field

import numpy as np
import paho.mqtt.client as mqtt
from scipy.spatial.transform import Rotation

# The bridge's own modules sit beside it (python -I leaves its directory off sys.path).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import expression  # noqa: E402

from reachy_mini import ReachyMini
from reachy_mini.io.protocol import SetSpeechOffsetsCmd
from reachy_mini.media.audio_doa import AudioDoA
from reachy_mini.motion.recorded_move import RecordedMoves
from reachy_mini.utils import create_head_pose

log = logging.getLogger("reachy-bridge")

STATUS_TOPIC = "reachy/status"
EVENTS_TOPIC = "dawn/events"
DAWN_STATUS_TOPIC = "dawn/status"
HEARTBEAT_SEC = 30
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
        self.offline_since = 0.0
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
                    self.last_activity = now  # a typed command wakes it too
                if state == "WAKEWORD_LISTEN":
                    doa.open_window()
                elif state == "COMMAND_RECORDING":
                    self.listening_until = now + self.cfg["timing"]["listen_hold_s"]
            elif event == "wake_word":
                self.last_activity = now
                self.listening_until = now + self.cfg["timing"]["listen_hold_s"]
                doa.aim_from_window()
            elif event == "speaking_start":
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

    def awake_wanted(self) -> bool:
        """Awake while DAWN is online and something happened in the last
        sleep_after_s.  Speech near the mic alone (WAKEWORD_LISTEN: the TV,
        people talking) doesn't count, or it would never sleep."""
        # Briefly offline counts as online: a quick reconnect (its will says
        # offline, then online again) shouldn't put it through a sleep and wake.
        if (not self.dawn_online and
                time.monotonic() - self.offline_since >= self.cfg["timing"]["offline_grace_s"]):
            return False
        if self.mode() != "idle":
            return True
        return time.monotonic() - self.last_activity < self.cfg["idle"].get("sleep_after_s", 900)

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
        try:
            self.doa = AudioDoA()
            if not self.doa.available:
                log.warning("DoA: no ReSpeaker found; gaze stays centred")
                self.doa = None
        except Exception as e:  # pyusb permissions, firmware
            log.warning("DoA unavailable: %s", e)
            self.doa = None

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
        if self.doa is None:
            return
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
            try:
                reading = self.doa.get_DoA()
            except Exception as e:
                log.warning("DoA read failed: %s", e)
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


class SpeechEnvelope:
    RATE = 16000
    FRAME = 320  # 20 ms

    def __init__(self, cfg: dict):
        self.source = cfg["speech"]["monitor_source"]
        self.level = 0.0
        self.attack = cfg["speech"]["attack"]
        self.release = cfg["speech"]["release"]
        self.gain = cfg["speech"]["gain"]

    def run(self, stop: threading.Event) -> None:
        cmd = ["parec", "--raw", "--format=s16le", f"--rate={self.RATE}", "--channels=1",
               "--latency-msec=20", "-d", self.source]
        while not stop.is_set():
            try:
                proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            except OSError as e:
                log.error("parec: %s", e)
                stop.wait(5)
                continue
            log.info("speech envelope: reading %s", self.source)
            try:
                while not stop.is_set():
                    data = proc.stdout.read(self.FRAME * 2)
                    if not data:
                        break
                    x = np.frombuffer(data, dtype=np.int16).astype(np.float32) / 32768.0
                    rms = float(np.sqrt(np.mean(x * x))) if x.size else 0.0
                    target = min(1.0, rms * self.gain)
                    k = self.attack if target > self.level else self.release
                    self.level += (target - self.level) * k
            finally:
                proc.kill()
                proc.wait()
            if not stop.is_set():
                log.warning("parec ended; retrying")
                stop.wait(2)


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

    def start_clip(self, name: str) -> None:
        if self.moves is None:
            return
        spec = self.cfg["expressions"][name]  # normalized: every move exists
        move_name = self.picker.pick(spec["moves"])
        try:
            move = self.moves.get(move_name)
        except Exception as e:
            log.warning("expression %s: move %s failed to load: %s", name, move_name, e)
            return
        self.clip = Clip(move, move_name, time.monotonic(), move.duration, spec["scale"])
        log.info("expression %s (%s, up to %.1fs)", name, move_name, move.duration)

    def end_clip(self) -> None:
        """Fade the clip out now, once it has had its minimum hold (its sentence's
        speech may not have started yet when it does)."""
        age = time.monotonic() - self.clip.started
        if age >= self.cfg["timing"].get("clip_min_s", 1.0):
            self.clip.end = min(self.clip.end, age + self.cfg["timing"]["clip_fade_out_s"])

    def clip_pose(self, base: Pose) -> Pose:
        clip = self.clip
        age = time.monotonic() - clip.started
        if age >= clip.end:
            # How much of each clip gets shown, to judge whether picking moves
            # by sentence length is worth it.
            log.info("clip %s played %.1f of %.1f s", clip.name, clip.end, clip.move.duration)
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
        self.dawn.drain_expressions()
        self.mini.client.send_command(SetSpeechOffsetsCmd(offsets=[0.0] * 6))
        self.speech_sent = False
        # The daemon starts with torque off; asleep, the robot rests in its sleep
        # pose without it (no motors energized all night).  enable_motors pins
        # the targets to the present pose, so nothing jumps.
        if awake:
            log.info("waking up")
            self.mini.enable_motors()
            self.mini.wake_up()
        else:
            log.info("going to sleep")
            self.mini.goto_sleep()
            self.mini.disable_motors()

    def run(self, stop: threading.Event) -> None:
        try:
            self.loop(stop)
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
            self.set_awake(self.dawn.awake_wanted())
            if self.awake != was_awake:
                # Waking blocked ~2.5 s in the wake-up move: start timing afresh.
                tick = last = time.monotonic()
                dt = fast
            period = slow
            if self.awake:
                t = tick - t0
                mode = self.dawn.mode()
                # An expression lasts while she speaks, up to its clip's length.  A
                # newer one fades it out first (cutting it off would jump the head);
                # only the newest of those waiting is worth showing.
                if self.clip is not None and (mode != "speaking" or
                                              not self.dawn.expressions.empty()):
                    self.end_clip()
                if self.clip is None:
                    name = None
                    while True:
                        try:
                            name = self.dawn.expressions.get_nowait()
                        except queue.Empty:
                            break
                    if name is not None:
                        self.start_clip(name)

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
    cfg.setdefault("moments", {})
    cfg["moments"]["clip_sway"] = expression.clip_sway(cfg["moments"])

    dawn = DawnState(cfg)
    doa = DoaTracker(cfg)
    speech = SpeechEnvelope(cfg)
    link = Link(cfg, dawn, doa)
    link.start()

    threads = [threading.Thread(target=doa.run, args=(stop,), daemon=True),
               threading.Thread(target=speech.run, args=(stop,), daemon=True)]
    for th in threads:
        th.start()

    grace = cfg["timing"]["offline_grace_s"]
    down_since = None
    while not stop.is_set():
        try:
            with ReachyMini(connection_mode="localhost_only", media_backend="no_media",
                            port=cfg["daemon"]["port"]) as mini:
                log.info("connected to the Reachy Mini daemon")
                down_since = None
                link.publish_status(True)
                mixer = Mixer(cfg, mini, dawn, doa, speech, moves)
                last_beat = time.monotonic()
                mixer_thread = threading.Thread(target=mixer.run, args=(stop,), daemon=True)
                mixer_thread.start()
                while not stop.is_set() and mixer_thread.is_alive():
                    stop.wait(1)
                    if time.monotonic() - last_beat >= HEARTBEAT_SEC:
                        link.publish_status(True)
                        last_beat = time.monotonic()
                if stop.is_set():
                    mixer_thread.join()  # it puts the robot to sleep itself
        except Exception as e:
            log.error("Reachy Mini: %s", e)
        if stop.is_set():
            break
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
