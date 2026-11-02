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
# One isolated DAWN daemon for live tests: its own run dir (config, secrets,
# data, home), started with --server and a clean environment, inside the
# namespaces run.sh creates (only loopback, so nothing reaches the internet,
# the LAN or the real daemon's admin socket; the real DAWN directories masked).

import os
import re
import secrets
import shutil
import signal
import sqlite3
import subprocess
import tempfile
import time
import unittest

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BUILD = os.environ.get("DAWN_LIVE_BUILD", os.path.join(REPO, "build-debug"))
ASSETS = ("models", "tool_instructions", "www", "sound_assets")
WEBUI_PORT = 3000
# Ports for fake providers (fake_llm.FakeLLM); inside the namespace, so fixed.
LOCAL_LLM_PORT = 18080
CLOUD_LLM_PORT = 18081
ADMIN_USER = "livetest"


def require_isolation():
    """Refuse to run outside run.sh's namespaces: only loopback, real DAWN state masked."""
    with open("/proc/net/route") as f:
        routes = f.read().strip().splitlines()[1:]
    with open("/proc/net/dev") as f:
        ifaces = [line.split(":")[0].strip() for line in f.read().splitlines()[2:]]
    real_state = os.path.isdir("/var/lib/dawn") and os.listdir("/var/lib/dawn")
    if routes or ifaces != ["lo"] or real_state:
        raise SystemExit("live tests run only inside tests/live/run.sh's namespaces")


def toml_value(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return str(v)
    if isinstance(v, (list, tuple)):
        return "[" + ", ".join(toml_value(x) for x in v) + "]"
    return '"' + str(v).replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_toml(path, sections):
    with open(path, "w") as f:
        for name, keys in sections.items():
            f.write(f"[{name}]\n")
            for k, v in keys.items():
                f.write(f"{k} = {toml_value(v)}\n")
            f.write("\n")


class Instance:
    """Start with start(); config overrides as {section: {key: value}}."""

    def __init__(self, config=None, secrets_toml="", keep=False):
        self.overrides = config or {}
        self.secrets_toml = secrets_toml
        self.keep = keep
        self.password = secrets.token_urlsafe(18)
        self.run = None
        self.proc = None
        self._log = None

    # -- lifecycle ------------------------------------------------------------

    def start(self, timeout=120):
        require_isolation()
        self.run = tempfile.mkdtemp(prefix="dawn-live-")
        for d in ("data", "home", "music"):
            os.makedirs(os.path.join(self.run, d))
        for a in ASSETS:
            os.symlink(os.path.join(REPO, a), os.path.join(self.run, a))
        write_toml(os.path.join(self.run, "dawn.toml"), self._config())
        # Always present: the daemon reads ./secrets.toml first and would
        # otherwise fall through to ~/.config/dawn or /etc/dawn.
        with open(os.path.join(self.run, "secrets.toml"), "w") as f:
            f.write("# live test instance\n" + self.secrets_toml)
        # Not the caller's environment: the daemon reads API keys, bot tokens and
        # DAWN_* settings from it.
        env = {"PATH": "/usr/bin:/bin",
               "LD_LIBRARY_PATH": os.environ.get("LD_LIBRARY_PATH", "/usr/local/lib"),
               "LANG": "C.UTF-8", "HOME": os.path.join(self.run, "home")}
        self._log = open(self.log_path, "w")
        self.proc = subprocess.Popen(
            [os.path.join(BUILD, "dawn"), "--config", os.path.join(self.run, "dawn.toml"),
             "--server"], cwd=self.run, env=env, stdin=subprocess.DEVNULL,
            stdout=self._log, stderr=subprocess.STDOUT)
        try:
            if not self.wait_log(r"Server mode active", timeout):
                raise RuntimeError("daemon didn't start")
            self._create_admin(env)
        except Exception:
            self.stop(failed=True)
            raise
        return self

    def stop(self, failed=False):
        """Stop the daemon; returns its exit status (0 is a clean shutdown)."""
        rc = None
        if self.proc:
            if self.proc.poll() is None:
                self.proc.send_signal(signal.SIGINT)
                try:
                    self.proc.wait(60)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait()
            rc = self.proc.returncode
            self.proc = None
        if self._log:
            self._log.close()
            self._log = None
        if self.run and not (failed or self.keep or rc not in (0, None)):
            shutil.rmtree(self.run, ignore_errors=True)
        elif self.run:
            print(f"live test run dir kept: {self.run}")
        self.run = None
        return rc

    def _config(self):
        r = self.run
        cfg = {
            "general": {"log_file": ""},
            "paths": {"data_dir": f"{r}/data", "music_dir": f"{r}/music"},
            "asr": {"model": "tiny"},
            "network": {"workers": 1},
            "webui": {"enabled": True, "port": WEBUI_PORT, "https": False,
                      "bind_address": "127.0.0.1"},
            "mqtt": {"enabled": False},
            "ota": {"enabled": False},
            "attention": {"enabled": False},
            "research": {"enabled": False},
            "code_projects": {"enabled": False},
            "memory": {"enabled": False},
            "memory.recovery": {"enabled": False},
            "stat": {"db_path": f"{r}/data/stat.db"},
        }
        for section, keys in self.overrides.items():
            cfg.setdefault(section, {}).update(keys)
        return cfg

    def _create_admin(self, env):
        m = re.search(r"Token:\s+(\S+)", self.log())
        if not m:
            raise RuntimeError("no setup token in the daemon's output")
        e = dict(env, DAWN_SETUP_TOKEN=m.group(1), DAWN_PASSWORD=self.password)
        subprocess.run([os.path.join(BUILD, "dawn-admin"), "user", "create", ADMIN_USER, "--admin"],
                       env=e, check=True, stdin=subprocess.DEVNULL, capture_output=True)

    # -- observing ------------------------------------------------------------

    @property
    def log_path(self):
        return os.path.join(self.run, "dawn.out")

    def log(self):
        with open(self.log_path, errors="replace") as f:
            return f.read()

    def log_mark(self):
        """A position in the log; log_since() reads what came after it."""
        return os.path.getsize(self.log_path)

    def log_since(self, mark):
        with open(self.log_path, errors="replace") as f:
            f.seek(mark)
            return f.read()

    def wait_log(self, pattern, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if re.search(pattern, self.log()):
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.25)
        return False

    def db(self, sql, args=()):
        """Rows from the instance's auth.db, read-only."""
        con = sqlite3.connect(f"file:{self.run}/data/auth.db?mode=ro", uri=True)
        try:
            return con.execute(sql, args).fetchall()
        finally:
            con.close()


class LiveTestCase(unittest.TestCase):
    """A test class on one daemon (start_instance in setUpClass).  Its tests share
    the instance and run in name order.  The run dir is kept when one of them fails
    or the daemon doesn't shut down cleanly (DAWN_LIVE_KEEP=1 keeps it anyway)."""

    failures_in_class = 0

    @classmethod
    def start_instance(cls, config, secrets_toml="", fakes=()):
        """Start @p fakes and a daemon, all stopped when the class ends."""
        for fake in fakes:
            fake.start()
            cls.addClassCleanup(fake.stop)
        cls.inst = Instance(config, secrets_toml, keep=os.environ.get("DAWN_LIVE_KEEP") == "1")
        cls.inst.start()
        cls.addClassCleanup(cls._stop_instance)
        from dawn_client import Client  # here: instance.py stays usable without it

        try:
            cls.client = Client(WEBUI_PORT, ADMIN_USER, cls.inst.password)
        except Exception:
            cls.failures_in_class += 1  # keep the run dir: its log says why
            raise
        cls.addClassCleanup(cls.client.close)

    @classmethod
    def _stop_instance(cls):
        rc = cls.inst.stop(failed=cls.failures_in_class > 0)
        if rc != 0:
            raise RuntimeError(f"the daemon exited with status {rc}")

    def run(self, result=None):
        before = len(result.failures) + len(result.errors) if result else 0
        result = super().run(result)
        if result is not None and len(result.failures) + len(result.errors) > before:
            type(self).failures_in_class += 1
        return result

    def shown_roles(self, conv):
        """The roles of a conversation's rows a user sees (not request context)."""
        rows = self.inst.db("SELECT role FROM messages WHERE conversation_id = ? AND kind IS NULL "
                            "ORDER BY id", (conv,))
        return [r[0] for r in rows]

    def assertNoErrorFrames(self, turn):
        """No error frame at all during @p turn, tagged with its ref or not."""
        self.assertEqual([f["payload"] for f in turn.frames if f.get("type") == "error"], [])
