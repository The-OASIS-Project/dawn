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
# A scripted OpenAI-compatible LLM for live tests: it answers each chat request
# with the next scripted response (text, a tool call, an HTTP status),
# or a default text when the script is empty, and records every request.  It
# also answers llama.cpp's /props so the daemon sees a local server with a
# known context window.

import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DEFAULT_CTX = 32768


def text(t):
    """A streamed text answer."""
    return {"text": t}


def tool_call(name, args):
    """A single tool call; the next request carries the tool's result."""
    return {"tool_call": (name, args)}


def status(code):
    """An HTTP error, e.g. 429 or 529."""
    return {"status": code}


def last_user_text(body):
    """The text of a chat request's last user message."""
    for m in reversed(body.get("messages", [])):
        if m.get("role") == "user":
            c = m.get("content")
            return c if isinstance(c, str) else json.dumps(c)
    return ""


class FakeLLM:
    """One fake provider on 127.0.0.1:port, running in a thread."""

    def __init__(self, port, name="fake", ctx=DEFAULT_CTX):
        self.port = port
        self.name = name
        self.ctx = ctx
        self.default = f"This is the {name} model answering."
        self._script = []
        self._lock = threading.Lock()
        self.requests = []  # [{"path", "body"}] for chat requests
        self._calls = 0
        self._server = None

    # -- control ------------------------------------------------------------

    def script(self, *responses):
        with self._lock:
            self._script = list(responses)

    def start(self):
        fake = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def _json(self, code, obj):
                body = json.dumps(obj).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                if self.path.startswith("/props"):
                    self._json(200, {"default_generation_settings": {"n_ctx": fake.ctx}})
                elif self.path.startswith("/v1/models") or self.path.startswith("/models"):
                    self._json(200, {"object": "list", "data": [{"id": fake.name}]})
                else:
                    self._json(404, {"error": "not found"})

            def do_POST(self):
                n = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(n) or b"{}")
                if not self.path.endswith("/chat/completions"):
                    self._json(404, {"error": "not found"})
                    return
                with fake._lock:
                    fake.requests.append({"path": self.path, "body": body})
                    step = fake._script.pop(0) if fake._script else text(fake.default)
                fake._answer(self, body, step)

        self._server = ThreadingHTTPServer(("127.0.0.1", self.port), Handler)
        self._server.daemon_threads = True
        threading.Thread(target=self._server.serve_forever, daemon=True).start()
        return self

    def stop(self):
        if self._server:
            self._server.shutdown()
            self._server.server_close()
            self._server = None

    # -- queries --------------------------------------------------------------

    def chat_requests(self, nonce=None):
        """Chat requests, or only those of the turn whose question carries @p nonce
        (its last user message; later turns and background calls don't count)."""
        with self._lock:
            reqs = list(self.requests)
        if nonce is None:
            return reqs
        return [r for r in reqs if nonce in last_user_text(r["body"])]

    # -- answering ------------------------------------------------------------

    def _answer(self, h, body, step):
        if "status" in step:
            h._json(step["status"], {"error": {"message": f"scripted {step['status']}"}})
            return
        stream = body.get("stream", False)
        if "tool_call" in step:
            name, args = step["tool_call"]
            with self._lock:
                self._calls += 1
                call_id = f"call_{self._calls}"
            call = {"index": 0, "id": call_id, "type": "function",
                    "function": {"name": name, "arguments": json.dumps(args)}}
            if stream:
                self._sse(h, [{"tool_calls": [call]}], "tool_calls")
            else:
                h._json(200, self._completion({"role": "assistant", "content": None,
                                               "tool_calls": [call]}, "tool_calls"))
            return
        words = step["text"].split(" ")
        if stream:
            deltas = [{"content": w + ("" if i == len(words) - 1 else " ")}
                      for i, w in enumerate(words)]
            self._sse(h, deltas, "stop")
        else:
            h._json(200, self._completion({"role": "assistant", "content": step["text"]}, "stop"))

    def _completion(self, message, finish):
        return {"id": "x", "object": "chat.completion", "model": self.name,
                "choices": [{"index": 0, "message": message, "finish_reason": finish}],
                "usage": {"prompt_tokens": 10, "completion_tokens": 8, "total_tokens": 18}}

    def _sse(self, h, deltas, finish):
        h.send_response(200)
        h.send_header("Content-Type", "text/event-stream")
        h.end_headers()
        for d in deltas:
            chunk = {"id": "x", "object": "chat.completion.chunk", "model": self.name,
                     "choices": [{"index": 0, "delta": d, "finish_reason": None}]}
            h.wfile.write(b"data: " + json.dumps(chunk).encode() + b"\n\n")
        end = {"id": "x", "object": "chat.completion.chunk", "model": self.name,
               "choices": [{"index": 0, "delta": {}, "finish_reason": finish}],
               "usage": {"prompt_tokens": 10, "completion_tokens": 8, "total_tokens": 18}}
        h.wfile.write(b"data: " + json.dumps(end).encode() + b"\n\n")
        h.wfile.write(b"data: [DONE]\n\n")
