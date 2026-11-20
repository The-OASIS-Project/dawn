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
# A WebUI-protocol client for live tests: logs in like the browser, opens the
# WebSocket, and runs turns, returning what the user would see.

import json
import time

import requests
import websocket  # websocket-client

# Mirrors AUTH_COOKIE_NAME in include/webui/webui_internal.h
COOKIE_NAME = "__Host-dawn_session"
SUBPROTOCOL = "dawn-1.0"
# A turn refused on receipt (no echo) gets its error and no idle: it's over
# once nothing more comes for this long.
REFUSED_GRACE_SEC = 2.0


class LoginError(Exception):
    pass


def login(base, user, password):
    """CSRF then login; returns the session cookie's value.  Mirrors login() in
    scripts/ws_observer.py: a protocol change goes in both."""
    with requests.Session() as s:
        s.trust_env = False  # no proxy or netrc from the caller's environment
        r = s.get(f"{base}/api/auth/csrf", timeout=10)
        r.raise_for_status()
        r = s.post(f"{base}/api/auth/login",
                   json={"username": user, "password": password,
                         "csrf_token": r.json()["csrf_token"]}, timeout=10)
        body = r.json()
        if not body.get("success"):
            raise LoginError(body.get("error", r.text))
        # The cookie is Secure, so the jar won't send it over plain http; it's
        # passed explicitly from here on.
        tok = s.cookies.get(COOKIE_NAME)
    if not tok:
        raise LoginError(f"no {COOKIE_NAME} cookie")
    return tok


class Turn:
    """What one turn showed: the reply text, the error frames, every frame."""

    def __init__(self):
        self.frames = []
        self.reply = ""
        self.errors = []
        self.seconds = 0.0


class Client:
    def __init__(self, port, user, password, host="127.0.0.1"):
        self.base = f"http://{host}:{port}"
        token = login(self.base, user, password)
        self.ws = websocket.create_connection(
            f"ws://{host}:{port}/", subprotocols=[SUBPROTOCOL],
            header=[f"Cookie: {COOKIE_NAME}={token}"], timeout=5)
        self._ref = 0
        self.send("get_config", {})  # any first message starts the session
        self._drain(2.0)

    def close(self):
        self.ws.close()

    def send(self, msg_type, payload):
        self.ws.send(json.dumps({"type": msg_type, "payload": payload}))

    def _recv(self, deadline):
        """The next JSON frame, or None at the deadline."""
        while time.monotonic() < deadline:
            self.ws.settimeout(max(0.1, deadline - time.monotonic()))
            try:
                msg = self.ws.recv()
            except websocket.WebSocketTimeoutException:
                return None
            if isinstance(msg, (bytes, bytearray)):
                continue  # audio
            return json.loads(msg)
        return None

    def _drain(self, seconds):
        deadline = time.monotonic() + seconds
        while self._recv(deadline):
            pass

    def new_conversation(self, timeout=10):
        """Start a conversation; returns its id."""
        self.send("new_conversation", {"save_current": False})
        deadline = time.monotonic() + timeout
        while True:
            f = self._recv(deadline)
            if f is None:
                raise TimeoutError("no new_conversation_response")
            if f.get("type") == "new_conversation_response":
                return f["payload"]["conversation_id"]

    def load_conversation(self, conv_id, timeout=10):
        """Open a stored conversation, as clicking it in the sidebar does; returns
        the response payload (its llm_settings among it)."""
        self.send("load_conversation", {"conversation_id": conv_id})
        deadline = time.monotonic() + timeout
        while True:
            f = self._recv(deadline)
            if f is None:
                raise TimeoutError("no load_conversation_response")
            if f.get("type") == "load_conversation_response":
                return f["payload"]

    def turn(self, text, timeout=60):
        """Send @p text as a typed message and wait for its turn to end."""
        self._ref += 1
        ref = str(self._ref)
        t = Turn()
        start = time.monotonic()
        self.send("text", {"text": text, "client_ref": ref})
        deadline = start + timeout
        ours = False  # the turn's own echo or error was seen
        echoed = False
        while True:
            refused = not echoed and any(e.get("severity", "error") == "error" for e in t.errors)
            f = self._recv(min(deadline, time.monotonic() + REFUSED_GRACE_SEC) if refused
                           else deadline)
            if f is None:
                if refused:
                    t.seconds = time.monotonic() - start
                    return t
                raise TimeoutError(f"turn {ref} didn't end in {timeout} s")
            t.frames.append(f)
            kind, p = f.get("type"), f.get("payload", {})
            if kind == "transcript" and p.get("role") == "user" and p.get("client_ref") == ref:
                ours = echoed = True
            elif kind == "error" and p.get("client_ref") == ref:
                ours = True
                t.errors.append(p)
            elif kind == "stream_delta":
                t.reply += p.get("delta", "")
            elif kind == "transcript" and p.get("role") == "assistant" and not p.get("replay"):
                t.reply = p.get("text", "")
            elif kind == "state" and p.get("state") == "idle" and ours:
                t.seconds = time.monotonic() - start
                return t
