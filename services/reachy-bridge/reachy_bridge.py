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


def pose_from_table(t: dict) -> Pose:
    return Pose(
        np.array(t.get("xyz_mm", [0, 0, 0]), dtype=float),
        np.array(t.get("rpy_deg", [0, 0, 0]), dtype=float),
        np.radians(np.array(t.get("antennas_deg", [0, 0]), dtype=float)),
    )


def pose_from_matrix(head: np.ndarray, antennas, neutral_antennas: np.ndarray) -> Pose:
    rpy = Rotation.from_matrix(head[:3, :3]).as_euler("xyz", degrees=True)
    return Pose(head[:3, 3] * 1000.0, rpy, np.asarray(antennas, dtype=float) - neutral_antennas)


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
        self.last_wake = time.monotonic()
        self.wake_at = 0.0
        self.expressions: "queue.Queue[str]" = queue.Queue(maxsize=4)
        self.last_expression = 0.0
        self.unknown_names: set[str] = set()

    def reset(self, online: bool) -> None:
        """DAWN came or went: nothing it said before still holds (a crash mid-reply
        never sends speaking_end)."""
        with self.lock:
            self.dawn_online = online
            self.state = "SILENCE"
            self.speaking = False
            self.listening_until = 0.0

    def handle(self, msg: dict, doa: "DoaTracker") -> None:
        event = msg.get("event")
        now = time.monotonic()
        with self.lock:
            if event == "state_change":
                state = msg.get("state")
                if not isinstance(state, str) or state not in (
                        "SILENCE", "WAKEWORD_LISTEN", "COMMAND_RECORDING", "PROCESS_COMMAND"):
                    return
                self.state = state
                if state == "WAKEWORD_LISTEN":
                    doa.open_window()
                elif state == "COMMAND_RECORDING":
                    self.listening_until = now + self.cfg["timing"]["listen_hold_s"]
            elif event == "wake_word":
                self.last_wake = now
                self.wake_at = now
                self.listening_until = now + self.cfg["timing"]["listen_hold_s"]
                doa.aim_from_window()
            elif event == "speaking_start":
                self.speaking = True
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

    def mode(self) -> str:
        """idle / listening / thinking / speaking, with the speaking gaps debounced."""
        now = time.monotonic()
        with self.lock:
            hold = self.cfg["timing"]["speaking_hold_s"]
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

    def yaw(self) -> float:
        with self.lock:
            age = time.monotonic() - self.gaze_at
            hold = self.cfg["gaze"]["hold_s"]
            fade = self.cfg["gaze"]["return_s"]
            if age < hold:
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
        self.clip = None  # (move, started, duration, scale)
        self.awake = None  # unknown until the first decision
        self.speech_sent = True  # clear what an earlier mixer may have left

    # -- layers ---------------------------------------------------------------

    def idle(self, t: float, mode: str) -> Pose:
        i = self.cfg["idle"]
        damp = {"listening": i["listening_damp"], "speaking": i["speaking_damp"]}.get(mode, 1.0)
        if time.monotonic() - self.dawn.last_wake > i["doze_after_s"]:
            damp *= i["doze_damp"]
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
        spec = self.cfg["expressions"][name]
        try:
            move = self.moves.get(spec["move"])
        except Exception as e:
            log.warning("expression %s: move %s not found: %s", name, spec["move"], e)
            return
        duration = min(move.duration, self.cfg["timing"]["clip_max_s"])
        self.clip = (move, time.monotonic(), duration, spec.get("scale", 1.0))
        log.info("expression %s (%s, %.1fs)", name, spec["move"], duration)

    def clip_pose(self, base: Pose) -> Pose:
        move, started, duration, scale = self.clip
        age = time.monotonic() - started
        if age >= duration:
            self.clip = None
            return base
        head, antennas, _ = move.evaluate(min(age, move.duration - 1e-3))
        pose = pose_from_matrix(head, antennas, self.neutral_antennas).scaled(scale)
        fade_in = self.cfg["timing"]["clip_fade_in_s"]
        fade_out = self.cfg["timing"]["clip_fade_out_s"]
        w = min(ease(age / fade_in), ease((duration - age) / fade_out))
        # On top of the posture and gaze: the head stays on the speaker.
        return base + pose.scaled(w)

    def speech_offsets(self, mode: str, t: float) -> list[float]:
        if mode != "speaking":
            return [0.0] * 6
        s = self.cfg["speech"]
        level = self.speech.level
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
        self.current = Pose()
        self.mini.client.send_command(SetSpeechOffsetsCmd(offsets=[0.0] * 6))
        self.speech_sent = False
        if awake:
            log.info("waking up")
            self.mini.wake_up()
        else:
            log.info("going to sleep")
            self.mini.goto_sleep()

    def run(self, stop: threading.Event) -> None:
        try:
            self.loop(stop)
        except Exception:
            log.exception("mixer stopped")

    def loop(self, stop: threading.Event) -> None:
        period = 1.0 / self.cfg["timing"]["mixer_hz"]
        smooth = self.cfg["timing"]["posture_smoothing"]
        t0 = time.monotonic()
        while not stop.is_set():
            tick = time.monotonic()
            self.set_awake(self.dawn.dawn_online)
            if self.awake:
                t = tick - t0
                mode = self.dawn.mode()
                # A clip plays out (cutting one off would jump the head); only the
                # newest of those waiting meanwhile is worth showing.
                if self.clip is None:
                    name = None
                    while True:
                        try:
                            name = self.dawn.expressions.get_nowait()
                        except queue.Empty:
                            break
                    if name is not None:
                        self.start_clip(name)

                target = self.postures[mode] + Pose(rpy=np.array([0.0, 0.0, self.doa.yaw()]))
                self.current = Pose.lerp(self.current, target, smooth)
                pose = self.current + self.idle(t, mode)
                if self.clip is not None:
                    pose = self.clip_pose(pose)
                self.send(pose)

                offsets = self.speech_offsets(mode, t)
                if any(offsets) or self.speech_sent:
                    self.mini.client.send_command(SetSpeechOffsetsCmd(offsets=offsets))
                    self.speech_sent = any(offsets)
            stop.wait(max(0.0, period - (time.monotonic() - tick)))
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

    dawn = DawnState(cfg)
    doa = DoaTracker(cfg)
    speech = SpeechEnvelope(cfg)
    link = Link(cfg, dawn, doa)
    link.start()

    moves = None
    try:
        moves = RecordedMoves(cfg["clips"]["dataset"])
    except Exception as e:
        log.warning("expression clips unavailable (%s): expressions are ignored", e)

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
