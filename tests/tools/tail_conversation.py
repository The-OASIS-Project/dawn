#!/usr/bin/env python3
"""Tail a DAWN conversation's event stream from outside the browser.

This is the Phase-2 contract gate for background jobs: the deliberately dumbest
possible consumer of the attach/replay protocol.  If this ~140-line script can
follow a background job end to end — every tool step, the answer body, and the
final disposition — then the wire contract is sufficient for the Phase-5 TUI,
and the WebUI is one renderer of a general protocol rather than the protocol
itself.

It is a verification tool, not a product, and is deliberately NOT in CI: it
needs a running daemon and real credentials.

What it exercises
-----------------
  attach_conversation {conversation_id, last_seq}
      -> load_conversation_response   (messages)
      -> conversation_events          (durable replay, seq > last_seq)
      -> stream_resume                (in-memory partial, if mid-turn)
      -> then live: conversation_event / message_appended

Replay alone is verifiable against a job that already finished, which costs no
LLM tokens.  Live-tailing needs a job actually running.

Usage
-----
    ./tail_conversation.py --conv 1006 --user admin --password ... --insecure
    ./tail_conversation.py --conv 1006 --user admin --password ... --from-seq 12

The WebUI is TLS-only by default; --insecure skips verification of the
self-signed cert a typical LAN deployment uses.

Requires: pip install websocket-client
"""

import argparse
import json
import re
import sys
import urllib.request

try:
    import websocket  # websocket-client
except ImportError:
    sys.exit("need: pip install websocket-client")

# §8.7 requires every renderer to strip control sequences: tool_result and
# terminal_chunk payloads are attacker-influenced (web fetches, command output),
# and this one prints straight to a terminal where an ANSI escape would be
# executed rather than displayed.  The TUI has the same obligation, so the
# reference consumer demonstrates it.
WS_SUBPROTOCOL = "dawn-1.0"  # must match DawnConfig.WS_SUBPROTOCOL / WEBUI_SUBPROTOCOL

_CTRL = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]|[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]")


def clean(s, limit=None):
    if s is None:
        return "<expired>"  # a pruned payload keeps its place but has no body
    s = _CTRL.sub("", str(s))
    if limit and len(s) > limit:
        s = s[:limit] + "…"
    return s


def login(host, user, password, insecure):
    """POST /api/auth/login, returning the dawn_session cookie."""
    ctx = None
    if host.startswith("https") and insecure:
        import ssl

        ctx = ssl._create_unverified_context()

    csrf_req = urllib.request.Request(f"{host}/api/auth/csrf")
    with urllib.request.urlopen(csrf_req, context=ctx) as r:
        cookies = r.headers.get_all("Set-Cookie") or []
        csrf = json.loads(r.read()).get("csrf_token", "")

    body = json.dumps({"username": user, "password": password, "csrf_token": csrf}).encode()
    req = urllib.request.Request(
        f"{host}/api/auth/login", data=body, headers={"Content-Type": "application/json"}
    )
    if cookies:
        req.add_header("Cookie", "; ".join(c.split(";")[0] for c in cookies))
    with urllib.request.urlopen(req, context=ctx) as r:
        cookies += r.headers.get_all("Set-Cookie") or []
        if not json.loads(r.read()).get("success", False):
            sys.exit("login failed")

    for c in cookies:
        if c.startswith("dawn_session="):
            return c.split(";")[0]
    sys.exit("no dawn_session cookie returned")


def render(msg, state):
    """Print one server frame.  Ordering is whatever the server sent — a dumb
    consumer appends, which is exactly why the attach order is part of the
    contract rather than an implementation detail."""
    kind = msg.get("type")
    p = msg.get("payload") or {}

    if kind == "load_conversation_response":
        msgs = p.get("messages") or []
        print(f"── {len(msgs)} message(s) in history ──")
        for m in msgs:
            # Track ids so the completeness check below knows the answer arrived.
            # On REPLAY of a finished job the body comes in this batch; only a
            # LIVE turn produces message_appended.  Both satisfy the contract.
            if m.get("id"):
                state["msg_ids"].add(int(m["id"]))
        for m in msgs[-3:]:
            print(f"   [{m.get('role')}] {clean(m.get('content'), 100)}")

    elif kind == "conversation_events":
        evs = p.get("events") or []
        print(f"── replay: {len(evs)} event(s) (more={p.get('has_more')}) ──")
        for e in evs:
            show_event(e, state)

    elif kind == "conversation_event":
        # Live.  Dedup against the replay batch: a frame can arrive between the
        # attach read and its delivery, so seq — not arrival order — is truth.
        show_event(p, state, live=True)

    elif kind == "message_appended":
        # §6.3 U-Crit: without this an event-only consumer never learns the answer.
        print(f"\n★ ANSWER (msg {p.get('message_id')}):")
        print(f"   {clean(p.get('text'), 400)}\n")
        state["got_answer"] = True

    elif kind == "stream_resume":
        print(f"── resuming partial: {clean(p.get('partial'), 80)} ──")


def show_event(e, state, live=False):
    seq = e.get("seq")
    if seq is not None:
        if seq in state["seen"]:
            return  # replayed AND pushed live — collapse
        state["seen"].add(seq)

    kind = e.get("kind")
    tag = "live" if live else "  ↺"
    body = e.get("payload")
    try:
        body = json.loads(body) if isinstance(body, str) else body
    except (ValueError, TypeError):
        pass

    if kind == "status":
        print(f"{tag} [{seq:>3}] status      {body.get('state') if isinstance(body, dict) else body}")
    elif kind == "tool_call":
        t = body.get("tool") if isinstance(body, dict) else "?"
        args = json.dumps(body.get("args")) if isinstance(body, dict) else ""
        print(f"{tag} [{seq:>3}] tool_call   {t}  {clean(args, 90)}")
    elif kind == "tool_result":
        r = body.get("result") if isinstance(body, dict) else body
        print(f"{tag} [{seq:>3}] tool_result {clean(r, 90)}")
    elif kind == "spawn":
        print(f"{tag} [{seq:>3}] spawn       job conv {body.get('conv_id') if isinstance(body, dict) else '?'}")
    elif kind == "complete":
        d = body.get("disposition") if isinstance(body, dict) else "?"
        fid = body.get("final_message_id") if isinstance(body, dict) else None
        print(f"{tag} [{seq:>3}] COMPLETE    {d}  final_msg={fid or ''}")
        state["done"] = True
        state["final_msg_id"] = fid
    else:
        print(f"{tag} [{seq:>3}] {kind}  {clean(body, 90)}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--conv", type=int, required=True)
    ap.add_argument("--user", required=True)
    ap.add_argument("--password", required=True)
    # DAWN's WebUI is TLS-only in the default config (ssl_cert_path is set), so
    # https is the right default; plain http is simply refused.  Self-signed
    # certs are the norm on a LAN deployment, hence --insecure existing at all.
    ap.add_argument("--host", default="https://localhost:3000")
    ap.add_argument("--from-seq", type=int, default=0, help="replay cursor (0 = whole log)")
    ap.add_argument("--insecure", action="store_true", help="skip TLS verify (self-signed)")
    ap.add_argument("--follow", action="store_true", help="keep tailing after 'complete'")
    args = ap.parse_args()

    cookie = login(args.host, args.user, args.password, args.insecure)
    ws_url = args.host.replace("https://", "wss://").replace("http://", "ws://")

    # The subprotocol is REQUIRED, not decorative: libwebsockets selects its
    # protocol handler by name, so a connection that omits "dawn-1.0" is routed
    # to the HTTP handler and every frame is silently discarded — it connects
    # cleanly and then nothing ever answers.
    ws = websocket.create_connection(
        ws_url,
        header=[f"Cookie: {cookie}"],
        subprotocols=[WS_SUBPROTOCOL],
        sslopt={"cert_reqs": 0} if args.insecure else None,
    )
    ws.send(json.dumps({
        "type": "attach_conversation",
        "payload": {"conversation_id": args.conv, "last_seq": args.from_seq},
    }))

    state = {"seen": set(), "msg_ids": set(), "done": False, "got_answer": False,
             "final_msg_id": None}
    print(f"── attached to conv {args.conv} (from seq {args.from_seq}) ──")
    try:
        while True:
            raw = ws.recv()
            if not raw:
                break
            try:
                render(json.loads(raw), state)
            except json.JSONDecodeError:
                continue
            if state["done"] and not args.follow:
                print("── job reached a terminal state ──")
                break
    except KeyboardInterrupt:
        pass
    finally:
        ws.close()

    # The contract gate (§6 U-Crit): did the answer BODY reach us at all?  Two
    # legitimate routes — the messages batch on attach (finished job) or a live
    # message_appended (turn completing while attached).  Failing only when
    # BOTH are absent is the real invariant; requiring message_appended
    # specifically would false-alarm on every replay.
    fid = state.get("final_msg_id")
    if state["done"]:
        if state["got_answer"]:
            print("✓ answer body delivered live (message_appended)")
        elif fid and int(fid) in state["msg_ids"]:
            print(f"✓ answer body delivered in the attach batch (msg {fid})")
        elif fid:
            print(f"\n⚠ complete cited final_message_id={fid} but that body never arrived "
                  f"— an event-only consumer would be blind to the answer (§6 U-Crit)")
        else:
            print("\n⚠ complete carried no final_message_id — cannot correlate an answer")


if __name__ == "__main__":
    main()
