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
# By contributing to this project, you agree to license your contributions
# under the GPLv3 (or any later version) or any future licenses chosen by
# the project author(s).
"""Drive scripted multi-turn conversations through a RUNNING DAWN daemon.

Unlike llm_testing/ (which calls provider APIs directly with its own prompt),
every turn here goes through DAWN's real request path: WebUI WebSocket text
input -> session history -> focus-block retrieval -> provider formatter -> tool
loop -> persistence.  That makes it the right instrument for A/B-testing changes
to how DAWN builds requests (prompt layout, caching, thinking replay).

Per (model, scenario) it:
  1. opens a private conversation (private => no memory extraction, so runs
     don't feed back into the eval account's memory) with the model set
     per-session (the daemon's global config is untouched);
  2. sends each scripted turn and waits for the turn to finish ("idle");
  3. reads the persisted turn (final answer, tool calls, citation-audit rows)
     back from auth.db read-only, plus the daemon-log lines written during the
     turn (best-effort cache/usage telemetry);
  4. writes <out>/<model>/<scenario>.json.

Grading is a separate offline step (grade_ab.py), so re-grading never re-runs
the conversations.

Credentials come from DAWN_AB_USER / DAWN_AB_PASS, or ~/.config/dawn/dawn_ab.env
(same keys, KEY=VALUE, chmod 600) — never argv, never logged.
Use a dedicated eval account: --reseed-memory WIPES that account's memory and
loads memory_fixture.json, and refuses to run if the account holds any memory
that did not come from an import.

  DAWN_AB_USER=benchmark DAWN_AB_PASS=... ./run_ab.py --reseed-memory \\
      --models claude:claude-opus-5-5,claude:claude-sonnet-5 --out results/baseline
"""

import argparse
import glob
import json
import os
import re
import sqlite3
import ssl
import subprocess
import sys
import time
from datetime import datetime

try:
    import requests
    import urllib3
    import websocket
except ImportError as e:
    sys.exit(f"error: missing module ({e}); pip3 install requests websocket-client")

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_DB = "/var/lib/dawn/auth.db"
WS_SUBPROTOCOL = "dawn-1.0"
CRED_FILE = os.path.expanduser("~/.config/dawn/dawn_ab.env")
ANSI = re.compile(r"\x1b\[[0-9;]*m")
# Daemon-log lines worth keeping per turn: provider usage/cache lines, context size,
# errors.  Best-effort only — a concurrent session on the same daemon interleaves here.
LOG_KEEP = re.compile(r"cache|usage|Context: |ERROR|WARN|400|binding|input_transformations|"
                      r"Tool views?: |result_read: ", re.IGNORECASE)


# ----------------------------------------------------------------------------- daemon client

class DawnClient:
    """Minimal WebUI client: cookie login + one WebSocket, JSON frames only."""

    def __init__(self, base_url, insecure, turn_timeout):
        self.base = base_url.rstrip("/")
        self.insecure = insecure
        self.turn_timeout = turn_timeout
        self.token = None
        self.ws = None

    def login(self, user, password):
        s = requests.Session()
        s.verify = not self.insecure
        r = s.get(self.base + "/api/auth/csrf", timeout=15)
        r.raise_for_status()
        csrf = r.json()["csrf_token"]
        r = s.post(self.base + "/api/auth/login",
                   json={"csrf_token": csrf, "username": user, "password": password}, timeout=15)
        body = r.json() if r.headers.get("content-type", "").startswith("application/json") else {}
        self.token = s.cookies.get("__Host-dawn_session")
        if r.status_code != 200 or not self.token:
            raise RuntimeError(f"login failed (HTTP {r.status_code}): {body.get('error', '')}")

    def connect(self):
        ws_url = re.sub(r"^http", "ws", self.base) + "/ws"
        sslopt = {"cert_reqs": ssl.CERT_NONE, "check_hostname": False} if self.insecure else {}
        self.ws = websocket.create_connection(
            ws_url, subprotocols=[WS_SUBPROTOCOL], header=[f"Cookie: __Host-dawn_session={self.token}"],
            sslopt=sslopt, suppress_origin=True, timeout=5)
        # First frame creates the session server-side; TTS off so no binary audio arrives.
        self.send("init", {"tts_enabled": False})
        self.drain(2.0)

    def close(self):
        if self.ws:
            try:
                self.ws.close()
            except Exception:
                pass
            self.ws = None

    def send(self, msg_type, payload=None):
        frame = {"type": msg_type}
        if payload is not None:
            frame["payload"] = payload
        self.ws.send(json.dumps(frame))

    def recv(self, timeout):
        """One JSON frame, or None on timeout. Binary frames are ignored."""
        self.ws.settimeout(timeout)
        try:
            op, data = self.ws.recv_data()
        except websocket.WebSocketTimeoutException:
            return None
        if op != websocket.ABNF.OPCODE_TEXT:
            return {}
        try:
            return json.loads(data)
        except (ValueError, UnicodeDecodeError):
            return {}

    def drain(self, seconds):
        end = time.time() + seconds
        frames = []
        while time.time() < end:
            f = self.recv(max(0.05, end - time.time()))
            if f is None:
                break
            frames.append(f)
        return frames

    def request(self, msg_type, payload, resp_type, timeout=15):
        """Send and wait for the named response frame; returns its payload."""
        self.send(msg_type, payload)
        end = time.time() + timeout
        while time.time() < end:
            f = self.recv(max(0.05, end - time.time()))
            if f and f.get("type") == resp_type:
                return f.get("payload") or {}
        raise RuntimeError(f"no {resp_type} within {timeout}s")

    def run_turn(self, text, conv_id):
        """Send one user turn; collect frames until the turn returns to idle."""
        self.send("text", {"text": text, "conversation_id": conv_id})
        started = time.time()
        frames, busy, errors, streams, contexts, injected = [], False, [], {}, [], []
        while True:
            left = self.turn_timeout - (time.time() - started)
            if left <= 0:
                errors.append({"code": "HARNESS_TIMEOUT", "message": f"no idle after {self.turn_timeout}s"})
                break
            f = self.recv(min(left, 5.0))
            if f is None:
                continue
            t = f.get("type")
            p = f.get("payload") or {}
            if t in ("stream_start", "stream_delta", "stream_end", "state", "error", "context"):
                frames.append({"t": round(time.time() - started, 3), "type": t})
            if t == "state":
                state = p.get("state")
                if state and state != "idle":
                    busy = True
                elif state == "idle" and busy:
                    break
            elif t in ("stream_start", "stream_delta"):
                busy = True
                sid = str(p.get("stream_id", 0))
                streams[sid] = streams.get(sid, "") + (p.get("delta") or "")
            elif t == "error":
                errors.append({"code": p.get("code"), "message": p.get("message")})
            elif t == "context":
                contexts.append(p)
            elif t == "context_injection":
                # Focus block injected this turn, with per-item score breakdown
                # (top-level fields, not under "payload").
                for it in f.get("items") or []:
                    bd = it.get("score_breakdown") or {}
                    injected.append({"source": it.get("source_id"), "item": it.get("item_id"),
                                     "score": it.get("score"), "semantic": bd.get("semantic"),
                                     "text": (it.get("text") or "")[:120]})
        return {"elapsed_s": round(time.time() - started, 3), "errors": errors,
                "streams": streams, "context_frames": contexts, "injected": injected,
                "frame_timeline": frames}


# ----------------------------------------------------------------------------- db (read-only)

def db_connect_ro(path):
    return sqlite3.connect(f"file:{path}?mode=ro", uri=True, timeout=10)


def account_user_id(db, username):
    row = db.execute("SELECT id FROM users WHERE username = ?", (username,)).fetchone()
    return row[0] if row else None


def account_is_admin(db, user_id):
    row = db.execute("SELECT is_admin FROM users WHERE id = ?", (user_id,)).fetchone()
    return bool(row and row[0])


def non_import_memory_count(db, user_id):
    facts = db.execute("SELECT COUNT(*) FROM memory_facts WHERE user_id = ? AND "
                       "COALESCE(source,'') != 'import'", (user_id,)).fetchone()[0]
    summaries = db.execute("SELECT COUNT(*) FROM memory_summaries WHERE user_id = ?",
                           (user_id,)).fetchone()[0]
    return facts + summaries


def fact_embedding_coverage(db, user_id):
    total, embedded = db.execute(
        "SELECT COUNT(*), SUM(embedding IS NOT NULL) FROM memory_facts WHERE user_id = ? "
        "AND superseded_by IS NULL", (user_id,)).fetchone()
    return int(total or 0), int(embedded or 0)


def conversation_rows(db, conv_id):
    cur = db.execute("SELECT id, role, content, tool_calls, tool_call_id, reasoning, is_error, "
                     "created_at FROM messages WHERE conversation_id = ? AND kind IS NULL ORDER BY id",
                     (conv_id,))
    cols = [d[0] for d in cur.description]
    return [dict(zip(cols, r)) for r in cur.fetchall()]


def audit_rows(db, conv_id):
    # referenced_ids: items already in context the turn named as still relevant
    # (newer daemons); read as empty where the column doesn't exist.
    have = {r[1] for r in db.execute("PRAGMA table_info(memory_citation_audit)")}
    ref = "referenced_ids" if "referenced_ids" in have else "NULL AS referenced_ids"
    cur = db.execute("SELECT id, message_id, ts, injected_ids, cited_ids, dropped_count, "
                     f"tool_surfaced_ids, dropped_tool_count, {ref} FROM memory_citation_audit "
                     "WHERE conversation_id = ? ORDER BY id", (conv_id,))
    cols = [d[0] for d in cur.description]
    return [dict(zip(cols, r)) for r in cur.fetchall()]


def context_rows(db, conv_id):
    """The per-turn context rows (kind-marked user rows) and their sizes."""
    cur = db.execute("SELECT id, kind, length(content) AS chars FROM messages "
                     "WHERE conversation_id = ? AND kind IN ('turn_context', 'memory') ORDER BY id",
                     (conv_id,))
    return [{"id": r[0], "kind": r[1], "chars": r[2] or 0} for r in cur.fetchall()]


def conversation_meta(db, conv_id):
    cur = db.execute("SELECT llm_type, cloud_provider, model, thinking_mode, reasoning_effort, "
                     "is_private, context_tokens, context_max FROM conversations WHERE id = ?",
                     (conv_id,))
    row = cur.fetchone()
    return dict(zip([d[0] for d in cur.description], row)) if row else {}


def split_turns(rows):
    """Group persisted rows into turns, each starting at a user row."""
    turns, cur = [], None
    for r in rows:
        if r["role"] == "user":
            cur = {"user_row": r, "rows": []}
            turns.append(cur)
        elif cur is not None:
            cur["rows"].append(r)
    out = []
    for t in turns:
        calls = []
        for r in t["rows"]:
            if r["role"] == "assistant" and r["tool_calls"]:
                try:
                    for c in json.loads(r["tool_calls"]):
                        fn = c.get("function") or {}
                        calls.append({"name": fn.get("name"), "arguments": fn.get("arguments")})
                except ValueError:
                    calls.append({"name": None, "arguments": r["tool_calls"], "_unparsed": True})
        finals = [r for r in t["rows"] if r["role"] == "assistant" and not r["tool_calls"]]
        out.append({
            "user_msg_id": t["user_row"]["id"],
            "user_text": t["user_row"]["content"],
            "tool_calls": calls,
            "tool_results": [{"tool_call_id": r["tool_call_id"], "is_error": r["is_error"],
                              "content": (r["content"] or "")[:2000]}
                             for r in t["rows"] if r["role"] == "tool"],
            "final_answer": finals[-1]["content"] if finals else None,
            "has_reasoning": any(r["reasoning"] for r in t["rows"] if r["role"] == "assistant"),
            "msg_ids": [t["user_row"]["id"]] + [r["id"] for r in t["rows"]],
        })
    return out


# ----------------------------------------------------------------------------- daemon log

def newest_daemon_log():
    logs = sorted(glob.glob(os.path.join(REPO, "logs", "dawn_run_*.log")), key=os.path.getmtime)
    return logs[-1] if logs else None


class LogTail:
    """Byte-offset tail of the daemon log: lines appended between mark() and take()."""

    def __init__(self, path):
        self.path = path
        self.off = os.path.getsize(path) if path and os.path.exists(path) else 0

    def mark(self):
        if self.path and os.path.exists(self.path):
            self.off = os.path.getsize(self.path)

    def take(self):
        if not self.path or not os.path.exists(self.path):
            return []
        with open(self.path, "rb") as f:
            f.seek(self.off)
            data = f.read()
        self.off += len(data)
        lines = ANSI.sub("", data.decode("utf-8", "replace")).splitlines()
        return [ln.strip() for ln in lines if LOG_KEEP.search(ln)]


# ----------------------------------------------------------------------------- memory seed

EMBED_WAIT_S = 120


def reseed_memory(client, db_path, user_id, fixture_path, force=False):
    with db_connect_ro(db_path) as db:
        if account_is_admin(db, user_id):
            # A wipe on an admin account is a wipe of a real person's memory.
            sys.exit("refusing --reseed-memory: the eval account is an admin; use a dedicated "
                     "non-admin account")
        foreign = non_import_memory_count(db, user_id)
    if foreign and force:
        print(f"--force-reseed: wiping {foreign} non-import memory rows from the eval account")
    elif foreign:
        sys.exit(f"refusing --reseed-memory: account holds {foreign} memory rows that did not "
                 "come from an import (is this really a dedicated eval account?)")
    with open(fixture_path) as f:
        fixture = json.load(f)
    fixture.pop("_comment", None)
    p = client.request("delete_all_memories", {"confirm": "DELETE"}, "delete_all_memories_response")
    if not p.get("success"):
        sys.exit(f"memory wipe failed: {p.get('error')}")
    p = client.request("import_memories", {"format": "json", "commit": True, "data": fixture},
                       "import_memories_response", timeout=60)
    if not p.get("success"):
        sys.exit(f"memory import failed: {p.get('error')}")
    # Imports are embedded in the background; wait for them so the first turns
    # retrieve the same way as the rest (keyword-only recall otherwise).
    deadline = time.time() + EMBED_WAIT_S
    while True:
        with db_connect_ro(db_path) as db:
            total, embedded = fact_embedding_coverage(db, user_id)
        if embedded >= total or time.time() >= deadline:
            break
        time.sleep(1)
    return {"facts": total, "facts_embedded": embedded}


# ----------------------------------------------------------------------------- run

def git_state():
    def git(*a):
        return subprocess.run(["git", "-C", REPO, *a], capture_output=True, text=True).stdout.strip()
    return {"rev": git("rev-parse", "HEAD"), "branch": git("rev-parse", "--abbrev-ref", "HEAD"),
            "dirty_files": len([ln for ln in git("status", "--porcelain", "--untracked-files=no")
                                .splitlines() if ln])}


def load_credentials():
    """Env vars first, then CRED_FILE (KEY=VALUE lines, keep it chmod 600)."""
    user, password = os.environ.get("DAWN_AB_USER"), os.environ.get("DAWN_AB_PASS")
    if (not user or not password) and os.path.exists(CRED_FILE):
        with open(CRED_FILE) as f:
            kv = dict(ln.strip().split("=", 1) for ln in f if "=" in ln and not ln.startswith("#"))
        user = user or kv.get("DAWN_AB_USER", "").strip().strip("'\"")
        password = password or kv.get("DAWN_AB_PASS", "").strip().strip("'\"")
    return user, password


def parse_models(spec):
    out = []
    for item in spec.split(","):
        item = item.strip()
        if not item:
            continue
        provider, _, model = item.partition(":")
        if not model:
            sys.exit(f"bad --models entry '{item}' (want provider:model)")
        out.append((provider, model))
    return out


def run_scenario(client, db_path, log, provider, model, scen, args):
    client.request("clear_session", None, "clear_session_response")
    llm = {"type": "cloud", "provider": provider, "model": model}
    if args.thinking_mode:
        llm["thinking_mode"] = args.thinking_mode
    if args.effort:
        llm["reasoning_effort"] = args.effort
    llm_resp = client.request("set_session_llm", llm, "set_session_llm_response")
    if not llm_resp.get("success", True):
        raise RuntimeError(f"set_session_llm rejected: {llm_resp}")
    conv = client.request("new_conversation", {"title": f"dawn_ab {scen['id']} {model}"},
                          "new_conversation_response")
    conv_id = conv.get("conversation_id")
    if not conv.get("success") or not conv_id:
        raise RuntimeError(f"new_conversation failed: {conv}")
    priv = client.request("set_private", {"conversation_id": conv_id, "is_private": True},
                          "set_private_response")
    if not priv.get("success"):
        raise RuntimeError(f"set_private failed (refusing to run unprivate): {priv}")

    live, current = [], f"{provider}:{model}"
    for i, turn in enumerate(scen["turns"]):
        switched = None
        if turn.get("switch_to_alternate"):
            # Mid-conversation model switch: the first alternate that isn't the
            # model in use, so every --models entry exercises a real switch.
            alt = next((a for a in args.alternates if a != current), None)
            if alt is None:
                raise RuntimeError("switch_to_alternate: no --alternates entry differs")
            ap_, am_ = alt.split(":", 1)
            sw = dict(llm, provider=ap_, model=am_)
            sw_resp = client.request("set_session_llm", sw, "set_session_llm_response")
            if not sw_resp.get("success", True):
                raise RuntimeError(f"set_session_llm (switch) rejected: {sw_resp}")
            switched, current = alt, alt
        log.mark()
        wall = datetime.now().astimezone()
        ts0 = int(time.time())
        res = client.run_turn(turn["text"], conv_id)
        time.sleep(args.settle)
        res.update({"index": i, "model": current, "switched_to": switched, "sent_at": wall.isoformat(timespec="seconds"),
                    "sent_weekday": wall.strftime("%A"), "ts_start": ts0,
                    "ts_end": int(time.time()), "daemon_log": log.take()})
        live.append(res)
        print(f"    turn {i + 1}/{len(scen['turns'])}: {res['elapsed_s']:.1f}s"
              + (f"  ERRORS {res['errors']}" if res["errors"] else ""), flush=True)
        time.sleep(args.gap)

    with db_connect_ro(db_path) as db:
        persisted = split_turns(conversation_rows(db, conv_id))
        audits = audit_rows(db, conv_id)
        ctx = context_rows(db, conv_id)
        meta = conversation_meta(db, conv_id)

    for i, t in enumerate(persisted):
        # Context rows belong to the turn whose user row precedes them.
        lo = t["user_msg_id"]
        hi = persisted[i + 1]["user_msg_id"] if i + 1 < len(persisted) else float("inf")
        mine = [c for c in ctx if lo < c["id"] < hi]
        t["context_chars"] = {k: sum(c["chars"] for c in mine if c["kind"] == k)
                              for k in ("turn_context", "memory")}
        # Audit rows belong to a turn by message id (preferred) or by time window.
        t["citation_audit"] = [a for a in audits
                               if a["message_id"] in t["msg_ids"]
                               or (not a["message_id"] and i < len(live)
                                   and live[i]["ts_start"] - 1 <= a["ts"] <= live[i]["ts_end"] + 2)]
    return {"conversation_id": conv_id, "conversation_meta": meta, "set_session_llm": llm_resp,
            "turns": [{"script": scen["turns"][i], "live": live[i] if i < len(live) else None,
                       "persisted": persisted[i] if i < len(persisted) else None}
                      for i in range(len(scen["turns"]))],
            "persisted_turn_count": len(persisted)}


def main():
    ap = argparse.ArgumentParser(description="DAWN daemon-driven multi-turn A/B driver")
    ap.add_argument("--url", default="https://localhost:3000")
    ap.add_argument("--secure", action="store_true", help="verify TLS (default: self-signed ok)")
    ap.add_argument("--db", default=DEFAULT_DB)
    ap.add_argument("--models", default="claude:claude-opus-5-5,claude:claude-sonnet-5",
                    help="comma list of provider:model")
    ap.add_argument("--scenarios", default=os.path.join(HERE, "scenarios.json"))
    ap.add_argument("--only", help="comma list of scenario ids")
    ap.add_argument("--repeat", type=int, default=1, help="run each scenario N times")
    ap.add_argument("--out", required=True, help="output directory for this run")
    ap.add_argument("--label", default="", help="free-text label stored in the manifest")
    ap.add_argument("--reseed-memory", action="store_true",
                    help="wipe the eval account's memory and load memory_fixture.json first")
    ap.add_argument("--fixture", default=os.path.join(HERE, "memory_fixture.json"))
    ap.add_argument("--force-reseed", action="store_true",
                    help="with --reseed-memory: wipe even memory that didn't come from an import "
                         "(only for the dedicated eval account, e.g. rows a daemon bug leaked in)")
    ap.add_argument("--thinking-mode", choices=["disabled", "auto", "enabled"])
    ap.add_argument("--effort", help="reasoning effort passed to set_session_llm")
    ap.add_argument("--daemon-log", default="auto", help="daemon log path, 'auto', or 'none'")
    ap.add_argument("--alternates", default="claude:claude-sonnet-5,claude:claude-opus-5-5",
                    help="comma list of provider:model a switch_to_alternate turn switches to "
                         "(the first that isn't the model in use)")
    ap.add_argument("--turn-timeout", type=float, default=300)
    ap.add_argument("--settle", type=float, default=1.5, help="seconds after idle before reading DB")
    ap.add_argument("--gap", type=float, default=1.0, help="seconds between turns")
    args = ap.parse_args()
    args.alternates = [a.strip() for a in args.alternates.split(",") if a.strip()]

    user, password = load_credentials()
    if not user or not password:
        sys.exit(f"set DAWN_AB_USER and DAWN_AB_PASS, or put them in {CRED_FILE} "
                 "(dedicated eval account)")
    if not args.secure:
        urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

    with open(args.scenarios) as f:
        scenarios = json.load(f)["scenarios"]
    if args.only:
        wanted = set(args.only.split(","))
        scenarios = [s for s in scenarios if s["id"] in wanted]
    else:
        # An opt-in scenario needs something a default run can't assume (web
        # access, a particular model window): it runs only when named.
        scenarios = [s for s in scenarios if not s.get("opt_in")]
        missing = wanted - {s["id"] for s in scenarios}
        if missing:
            sys.exit(f"unknown scenario ids: {sorted(missing)}")
    models = parse_models(args.models)

    with db_connect_ro(args.db) as db:
        user_id = account_user_id(db, user)
    if user_id is None:
        sys.exit(f"account '{user}' not found in {args.db}")

    log_path = newest_daemon_log() if args.daemon_log == "auto" else (
        None if args.daemon_log == "none" else args.daemon_log)
    log = LogTail(log_path)

    client = DawnClient(args.url, insecure=not args.secure, turn_timeout=args.turn_timeout)
    client.login(user, password)
    client.connect()

    os.makedirs(args.out, exist_ok=True)
    manifest = {"started_at": datetime.now().astimezone().isoformat(timespec="seconds"),
                "label": args.label, "git": git_state(), "url": args.url, "account_user_id": user_id,
                "models": [f"{p}:{m}" for p, m in models], "scenarios": [s["id"] for s in scenarios],
                "repeat": args.repeat, "thinking_mode": args.thinking_mode, "effort": args.effort,
                "daemon_log": log_path, "memory_seed": None}
    if args.reseed_memory:
        manifest["memory_seed"] = reseed_memory(client, args.db, user_id, args.fixture,
                                                args.force_reseed)
        seed = manifest["memory_seed"]
        print(f"memory reseeded: {seed['facts']} facts ({seed['facts_embedded']} embedded)")

    try:
        for provider, model in models:
            mdir = os.path.join(args.out, model.replace("/", "_"))
            os.makedirs(mdir, exist_ok=True)
            for rep in range(args.repeat):
                for scen in scenarios:
                    tag = scen["id"] if args.repeat == 1 else f"{scen['id']}.r{rep + 1}"
                    print(f"[{model}] {tag}", flush=True)
                    try:
                        art = run_scenario(client, args.db, log, provider, model, scen, args)
                    except Exception as e:  # keep going; record the failure
                        art = {"harness_error": str(e)}
                        print(f"    HARNESS ERROR: {e}", flush=True)
                        client.close()
                        client.connect()
                    art.update({"scenario": scen["id"], "rep": rep + 1, "provider": provider,
                                "model": model})
                    with open(os.path.join(mdir, f"{tag}.json"), "w") as f:
                        json.dump(art, f, indent=2)
    finally:
        manifest["finished_at"] = datetime.now().astimezone().isoformat(timespec="seconds")
        with open(os.path.join(args.out, "manifest.json"), "w") as f:
            json.dump(manifest, f, indent=2)
        client.close()
    print(f"done -> {args.out}  (grade with: ./grade_ab.py {args.out})")


if __name__ == "__main__":
    main()
