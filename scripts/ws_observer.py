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
# DAWN WebSocket observer — a read-only client that logs in like the WebUI,
# opens the DAP2 WebSocket, and dumps every frame the server pushes. Built for
# validating server->client emits (get_config llm_runtime, error severity,
# config music port, ...) without driving a browser. Reusable by any front-end.
#
# Password precedence: --password  >  $DAWN_OBSERVER_PASSWORD  >  interactive prompt.
# Prefer the env var or prompt — a CLI password is visible in ps/shell history.
#
# Usage:
#   DAWN_OBSERVER_PASSWORD=secret python3 scripts/ws_observer.py --get-config
#   python3 scripts/ws_observer.py                       # prompts for the password
#   python3 scripts/ws_observer.py --host 10.0.0.5 --user kris \
#       --only config,error,llm_state_update
#   python3 scripts/ws_observer.py --attach 1051         # replay+watch a conversation
#
# Requires: requests, websocket-client  (both already present on the Jetson).

import argparse
import getpass
import json
import os
import ssl
import sys
from datetime import datetime

try:
    import requests
    import websocket  # websocket-client
except ImportError as e:
    sys.exit(f"Missing dependency: {e}. Install with: pip install requests websocket-client")

# Mirrors AUTH_COOKIE_NAME in include/webui/webui_internal.h
COOKIE_NAME = "dawn_session"
SUBPROTOCOL = "dawn-1.0"

# ANSI colors (disabled when not a tty)
_TTY = sys.stdout.isatty()
def _c(code, s):
    return f"\033[{code}m{s}\033[0m" if _TTY else s

# Frame types worth calling out during Batch-1 validation.
HIGHLIGHT = {
    "config": "33",             # music port / audio_chunk_ms
    "get_config_response": "33",  # llm_runtime reasoning fields
    "error": "31",              # severity field
    "llm_state_update": "36",
    "state": "35",
}


def stamp():
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]


def login(base, user, password, verify):
    """CSRF -> login. Returns a requests.Session carrying the dawn_session cookie."""
    s = requests.Session()
    s.verify = verify
    r = s.get(f"{base}/api/auth/csrf", timeout=10)
    r.raise_for_status()
    csrf = r.json()["csrf_token"]
    r = s.post(
        f"{base}/api/auth/login",
        json={"username": user, "password": password, "csrf_token": csrf},
        timeout=10,
    )
    body = r.json()
    if not body.get("success"):
        sys.exit(f"Login failed: {body.get('error', r.text)}")
    tok = s.cookies.get(COOKIE_NAME)
    if not tok:
        sys.exit(f"Login succeeded but no {COOKIE_NAME} cookie was set.")
    return tok


def run(args):
    scheme_http = "https" if args.tls else "http"
    scheme_ws = "wss" if args.tls else "ws"
    base = f"{scheme_http}://{args.host}:{args.port}"
    verify = not args.insecure

    if args.tls and not verify:
        print(_c("33", f"[{stamp()}] WARNING: TLS certificate verification disabled "
                       f"(--insecure). Use --verify-tls for a trusted chain."))
    print(_c("2", f"[{stamp()}] logging in to {base} as {args.user} ..."))
    token = login(base, args.user, args.password, verify)
    print(_c("2", f"[{stamp()}] authenticated; opening {scheme_ws}://{args.host}:{args.port} "
                  f"(subprotocol {SUBPROTOCOL})"))

    only = set(t.strip() for t in args.only.split(",")) if args.only else None
    drop = set(t.strip() for t in args.drop.split(",")) if args.drop else set()

    def show(frame_type, obj):
        if only and frame_type not in only:
            return
        if frame_type in drop:
            return
        color = HIGHLIGHT.get(frame_type)
        label = _c(color, frame_type) if color else frame_type
        if args.compact:
            payload = obj.get("payload", obj)
            print(f"[{stamp()}] {label}  {json.dumps(payload, separators=(',', ':'))}")
        else:
            print(f"[{stamp()}] {label}")
            print(json.dumps(obj.get("payload", obj), indent=2))

    def on_open(ws):
        print(_c("32", f"[{stamp()}] connected"))
        if args.attach:
            ws.send(json.dumps({"type": "attach_conversation",
                                "payload": {"conversation_id": args.attach, "last_seq": 0}}))
            print(_c("2", f"[{stamp()}] sent attach_conversation for conv {args.attach}"))
        # get_config is the frame that carries llm_runtime (Batch-1 #1). Ask for it.
        if args.get_config:
            ws.send(json.dumps({"type": "get_config", "payload": {}}))
            print(_c("2", f"[{stamp()}] sent get_config"))

    def on_message(ws, message):
        if isinstance(message, (bytes, bytearray)):
            if not args.binary:
                return
            print(f"[{stamp()}] {_c('34', 'BINARY')}  {len(message)} bytes")
            return
        try:
            obj = json.loads(message)
        except json.JSONDecodeError:
            print(f"[{stamp()}] {_c('31', 'non-JSON')}  {message[:200]}")
            return
        show(obj.get("type", "?"), obj)

    def on_error(ws, err):
        print(_c("31", f"[{stamp()}] error: {err}"))

    def on_close(ws, code, msg):
        print(_c("2", f"[{stamp()}] closed (code={code} msg={msg})"))

    ws = websocket.WebSocketApp(
        f"{scheme_ws}://{args.host}:{args.port}/",
        subprotocols=[SUBPROTOCOL],
        header=[f"Cookie: {COOKIE_NAME}={token}"],
        on_open=on_open,
        on_message=on_message,
        on_error=on_error,
        on_close=on_close,
    )
    sslopt = {"cert_reqs": ssl.CERT_NONE} if (args.tls and args.insecure) else None
    try:
        ws.run_forever(sslopt=sslopt, ping_interval=0)
    except KeyboardInterrupt:
        print(_c("2", f"\n[{stamp()}] interrupted; closing"))
        ws.close()


def main():
    p = argparse.ArgumentParser(description="DAWN WebSocket frame observer (read-only).")
    p.add_argument("--host", default="localhost")
    p.add_argument("--port", type=int, default=3000)
    p.add_argument("--user", default="admin")
    p.add_argument("--password",
                   help="Password. Falls back to $DAWN_OBSERVER_PASSWORD, then a "
                        "getpass prompt. Avoid passing on the CLI (visible in ps/history).")
    tls = p.add_mutually_exclusive_group()
    tls.add_argument("--tls", dest="tls", action="store_true", default=True,
                     help="Use https/wss (default; DAWN ships TLS on).")
    tls.add_argument("--no-tls", dest="tls", action="store_false",
                     help="Plain http/ws (only if [webui] https = false).")
    p.add_argument("--insecure", action="store_true", default=True,
                   help="Skip TLS cert verification (default on; DAWN uses a self-signed CA).")
    p.add_argument("--verify-tls", dest="insecure", action="store_false",
                   help="Verify the TLS certificate chain.")
    p.add_argument("--only", help="Comma list: show ONLY these frame types.")
    p.add_argument("--drop", help="Comma list: hide these frame types.")
    p.add_argument("--attach", type=int, help="attach_conversation to this conv id (replay+watch).")
    p.add_argument("--get-config", dest="get_config", action="store_true",
                   help="Send get_config on connect (fetches llm_runtime).")
    p.add_argument("--compact", action="store_true", help="One line per frame.")
    p.add_argument("--binary", action="store_true", help="Also note binary frames (audio/music).")
    args = p.parse_args()
    if not args.password:
        args.password = os.environ.get("DAWN_OBSERVER_PASSWORD")
    if not args.password:
        args.password = getpass.getpass(f"Password for {args.user}: ")
    if not args.password:
        sys.exit("No password provided (--password, $DAWN_OBSERVER_PASSWORD, or prompt).")
    run(args)


if __name__ == "__main__":
    main()
