#!/usr/bin/env python3
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
# Drive the turns the LLM quality suite captures (llm_testing/quality): logs in
# to the WebUI as the quality-fixture user, sets its fictional profile, and sends
# scripted turns in a new private conversation while the daemon captures them
# (dawn-admin llm capture --user quality-fixture --out <dir>).
#
# The fixture user has no memories, so a captured turn's context carries none:
# the suite can swap the question without leaving stale recall in the request.
#
# Password: $DAWN_FIXTURE_PASSWORD, else an interactive prompt.
#
# Usage:
#   python3 scripts/quality_capture.py --setup             # set the fixture profile
#   python3 scripts/quality_capture.py --surface webui-text \
#       --turn "What's the weather like?" --turn "And tomorrow?"
#   python3 scripts/quality_capture.py --surface webui-spoken --turn "Play some jazz"
#   python3 scripts/quality_capture.py --model claude:claude-haiku-5-5 \
#       --manifest captures/manifest.jsonl --turn "Remind me to call the shop at 3pm"
#   python3 scripts/quality_capture.py --standard --manifest captures/manifest.jsonl \
#       --model openai:gpt-5.6-luna --model claude:claude-haiku-5-5
#
# --standard captures, for every model and both WebUI surfaces, a 1-turn and a
# 2-turn conversation of small talk (no tools, so one request per turn).  All
# conversations run over one connection: each new connection would hold one of
# the daemon's few session slots until its idle timeout.
#
# A capture is per model: DAWN's request (thinking, sampling) depends on the
# model, so the suite never swaps the model in a captured request.

import argparse
import getpass
import json
import os
import ssl
import sys
import time

import websocket  # websocket-client

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_observer import COOKIE_NAME, SUBPROTOCOL, login  # noqa: E402

FIXTURE_USER = "quality-fixture"

# A fictional profile: an invented person, with a common placeholder city.
FIXTURE_SETTINGS = {
    "persona_description": "",
    "persona_mode": "append",
    "location": "Springfield, Oregon",
    "timezone": "America/Los_Angeles",
    "units": "imperial",
    "theme": "cyan",
    "real_name": "Alex Rivera",
    "identity_aliases": "",
    "preferred_address": "boss",
}

TURN_TIMEOUT_S = 180

# --standard: (turns) per conversation, run for every model and surface.
STANDARD_CONVERSATIONS = [["Hey, quick check: are you there?"],
                          ["Good morning.", "Thanks, that's all for now."]]
SURFACES = ["webui-text", "webui-spoken"]
LOOPBACK = ("localhost", "127.0.0.1", "::1")


def open_socket(args):
    scheme_http = "https" if args.tls else "http"
    scheme_ws = "wss" if args.tls else "ws"
    base = f"{scheme_http}://{args.host}:{args.port}"
    remote = args.host not in LOOPBACK
    if remote and not args.tls and not args.insecure:
        sys.exit("Refusing to send the fixture password unencrypted to a remote host "
                 "(--insecure to allow).")
    verify = args.tls and remote and not args.insecure
    password = os.environ.get("DAWN_FIXTURE_PASSWORD") or getpass.getpass(
        f"Password for {args.user}: ")
    token = login(base, args.user, password, verify=verify)
    sslopt = None if (not args.tls or verify) else {"cert_reqs": ssl.CERT_NONE}
    return websocket.create_connection(f"{scheme_ws}://{args.host}:{args.port}/",
                                       subprotocols=[SUBPROTOCOL],
                                       header=[f"Cookie: {COOKIE_NAME}={token}"],
                                       sslopt=sslopt, timeout=TURN_TIMEOUT_S)


def send(ws, msg_type, payload):
    ws.send(json.dumps({"type": msg_type, "payload": payload}))


def wait_for(ws, want, deadline):
    """Read frames until one of type @p want arrives; return its payload."""
    while time.time() < deadline:
        msg = ws.recv()
        if isinstance(msg, (bytes, bytearray)):
            continue
        obj = json.loads(msg)
        if obj.get("type") == want:
            return obj.get("payload", {})
        if obj.get("type") == "error":
            print(f"  error frame: {obj.get('payload')}", file=sys.stderr)
    raise TimeoutError(f"no {want} frame")


def run_turn(ws, text):
    """Send one user turn and wait for its final answer to finish."""
    send(ws, "text", {"text": text})
    deadline = time.time() + TURN_TIMEOUT_S
    saw_end = False
    failed = None
    while time.time() < deadline:
        msg = ws.recv()
        if isinstance(msg, (bytes, bytearray)):
            continue
        obj = json.loads(msg)
        kind = obj.get("type")
        if kind == "stream_end":
            saw_end = True
        elif kind == "state" and obj.get("payload", {}).get("state") == "idle":
            if saw_end:
                return
            if failed:
                raise RuntimeError(f"turn failed: {text!r}: {failed}")
        elif kind == "error":
            failed = obj.get("payload")
            print(f"  error frame: {failed}", file=sys.stderr)
    raise TimeoutError(f"turn did not finish: {text!r}")


def capture_conversation(ws, surface, model, turns, manifest):
    """One new private conversation on @p model and @p surface, its turns sent."""
    if model:
        provider, _, name = model.partition(":")
        send(ws, "set_session_llm", {"type": "cloud", "provider": provider, "model": name})
        resp = wait_for(ws, "set_session_llm_response", time.time() + 30)
        if not resp.get("success"):
            raise RuntimeError(f"could not switch to {model}: {resp.get('error', resp)}")
    # A spoken WebUI turn is one with TTS on: DAWN then sends the WebUI voice
    # directive.  Text turns run with TTS off.
    send(ws, "set_tts_enabled", {"enabled": surface == "webui-spoken"})
    send(ws, "new_conversation", {"title": f"quality capture ({surface})"})
    send(ws, "set_private", {"conversation_id": 0, "is_private": True})
    for i, text in enumerate(turns, 1):
        print(f"  turn {i}: {text}")
        run_turn(ws, text)
    if manifest:
        provider, _, name = (model or ":").partition(":")
        with open(manifest, "a") as f:
            f.write(json.dumps({"surface": surface, "provider": provider or None,
                                "model": name or None, "turns": turns,
                                "finished_at": int(time.time())}) + "\n")


def main():
    p = argparse.ArgumentParser(description="Drive quality-suite capture turns as the fixture user.")
    p.add_argument("--host", default="localhost")
    p.add_argument("--port", type=int, default=3000)
    p.add_argument("--user", default=FIXTURE_USER)
    p.add_argument("--no-tls", dest="tls", action="store_false", default=True)
    p.add_argument("--insecure", action="store_true",
                   help="Skip TLS verification for a remote host (localhost never verifies).")
    p.add_argument("--setup", action="store_true",
                   help="Set the fixture's fictional profile (the default persona unless "
                        "--persona-file is given).")
    p.add_argument("--persona-file",
                   help="With --setup: a persona text file the fixture uses in replace mode.")
    p.add_argument("--surface", choices=SURFACES, default="webui-text")
    p.add_argument("--turn", action="append", default=[], help="A user turn (repeatable).")
    p.add_argument("--model", action="append", default=[],
                   help="provider:model for the session (repeatable with --standard).")
    p.add_argument("--standard", action="store_true",
                   help="Capture the standard set for every --model and both surfaces.")
    p.add_argument("--manifest", help="Append each conversation's surface, model and turns.")
    args = p.parse_args()
    if args.persona_file and not args.setup:
        p.error("--persona-file goes with --setup")
    if not (args.setup or args.turn or args.standard):
        p.error("nothing to do: pass --setup, --turn or --standard")
    if args.standard and not args.model:
        p.error("--standard needs at least one --model")

    ws = open_socket(args)
    try:
        if args.setup:
            settings = dict(FIXTURE_SETTINGS)
            if args.persona_file:
                with open(args.persona_file) as f:
                    settings["persona_description"] = f.read().strip()
                settings["persona_mode"] = "replace"
            send(ws, "set_my_settings", settings)
            print("fixture profile set (persona: %s)" % (args.persona_file or "default"))
        if args.standard:
            for model in args.model:
                for surface in SURFACES:
                    for turns in STANDARD_CONVERSATIONS:
                        print(f"== {model} / {surface}")
                        capture_conversation(ws, surface, model, turns, args.manifest)
        elif args.turn:
            capture_conversation(ws, args.surface, args.model[0] if args.model else None,
                                 args.turn, args.manifest)
        print("done")
    finally:
        ws.close()


if __name__ == "__main__":
    main()
