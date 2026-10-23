# Memory audit: re-extract chosen stored conversations into a scratch database.
#
# Runs DAWN's own extraction (the bench binary, --memory-pipeline) on real
# conversations: each one is copied read-only from auth.db into the bench's
# scratch DB and extracted the way the recovery pass does.  The result is saved
# under data/ for `classify` and `report`, so a prompt variant can be compared
# with the baseline on the same input.
#
# The conversations are extracted in id order into one memory, so each sees the
# facts stored from the ones before it (dedup, supersede), as production does.
# Runs compare fairly only over the same set: pick once, reuse the ids file.
#
# Privacy: conversation text goes only to the bench binary and the extraction
# model's API.  The bench's log lines (which can quote facts) are read and
# dropped; this prints ids and counts only.
#
# License: GPLv3, same as DAWN.

import json
import os
import pathlib
import re
import select
import sqlite3
import subprocess
import time
from collections import Counter, defaultdict

from .storage import private_open

READY_TIMEOUT_S = 60
EXTRACT_TIMEOUT_S = 660  # the bench gives up on one extraction at 600 s
USAGE_RE = re.compile(rb"LLM cache: provider=\S+ model=\S+ kind=\S+ .*? prompt=(\d+) "
                      rb".* output=(\d+)")


def pick_conversations(db, user_id, n, since, min_messages):
    """Up to n recent conversations, taken in turn from each origin (webui,
    voice, messaging...) so one surface doesn't fill the set.  Private and
    background-job conversations are left out."""
    con = sqlite3.connect(pathlib.Path(db).resolve().as_uri() + "?mode=ro", uri=True)
    rows = con.execute(
        "SELECT id, origin FROM conversations WHERE user_id = ? AND is_private = 0 "
        "AND job_status IS NULL AND message_count >= ? AND created_at >= ? "
        "ORDER BY created_at DESC", (user_id, min_messages, since)).fetchall()
    con.close()
    by_origin = defaultdict(list)
    for cid, origin in rows:
        by_origin[origin or "?"].append(cid)
    chosen = []
    while len(chosen) < n and any(by_origin.values()):
        for origin in sorted(by_origin):
            if by_origin[origin] and len(chosen) < n:
                chosen.append(by_origin[origin].pop(0))
    return sorted(chosen)


class Bench:
    """One bench_retrieval --memory-pipeline process, spoken to over JSON lines."""

    def __init__(self, binary, run_dir, provider, model, effort, config=""):
        # The bench runs from run_dir: resolve a relative binary path here.
        cmd = [os.path.abspath(binary), "--memory-pipeline", "--extraction-provider", provider,
               "--extraction-model", model]
        if effort:
            cmd += ["--extraction-effort", effort]
        if config:
            cmd += ["--config", os.path.abspath(config)]
        # umask 077: the scratch DB and the saved snapshot hold copied
        # conversations, so nothing the bench creates is readable by others.
        self.proc = subprocess.Popen(cmd, cwd=run_dir, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                     bufsize=0, preexec_fn=lambda: os.umask(0o077))
        self.buf = b""
        self.usage = Counter()  # model tokens, from the bench's numeric "LLM cache:" lines
        self.ready = self.reply(READY_TIMEOUT_S)
        if self.ready.get("status") != "ready":
            raise SystemExit(f"bench did not start: {self.ready.get('message', '?')}")

    def reply(self, timeout):
        """The next status line.  The bench's log lines are dropped unread, but for
        the token counts on its per-call "LLM cache:" lines (numbers only)."""
        deadline = time.monotonic() + timeout
        while True:
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                if line.startswith(b'{"status"'):
                    try:
                        return json.loads(line)
                    except ValueError:  # a reply we can't read must not desync the stream
                        return {"status": "error", "message": "unreadable reply"}
                m = USAGE_RE.search(line)
                if m:
                    self.usage["calls"] += 1
                    self.usage["in"] += int(m.group(1))
                    self.usage["out"] += int(m.group(2))
            left = deadline - time.monotonic()
            if left <= 0:
                return {"status": "error", "message": "timed out"}
            r, _, _ = select.select([self.proc.stdout], [], [], left)
            if not r:
                continue
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            if not chunk:
                return {"status": "error", "message": "bench exited"}
            self.buf += chunk

    def send(self, msg, timeout=60):
        self.proc.stdin.write((json.dumps(msg) + "\n").encode())
        return self.reply(timeout)

    def close(self):
        try:
            self.proc.stdin.write(b'{"cmd":"quit"}\n')
            self.proc.stdin.close()
            self.proc.wait(timeout=120)
        except Exception:
            self.proc.kill()


def scratch_facts(db_path):
    """Live facts from the saved scratch DB.  Extraction stores each fact with
    its conversation's time, so created_at is already the conversation's."""
    con = sqlite3.connect(pathlib.Path(db_path).resolve().as_uri() + "?mode=ro",
                          uri=True)
    con.row_factory = sqlite3.Row
    rows = con.execute(
        "SELECT id, fact_text, source, created_at, confidence, expires_at, "
        "source_conversation_id FROM memory_facts WHERE superseded_by IS NULL "
        "ORDER BY id").fetchall()
    con.close()
    return [{"id": r["id"], "text": r["fact_text"], "source": r["source"],
             "created_at": r["created_at"],
             "date": time.strftime("%Y-%m-%d", time.gmtime(r["created_at"])),
             "confidence": r["confidence"], "expires_at": r["expires_at"],
             "access_count": 0, "last_cited": None,
             "conversation": r["source_conversation_id"]} for r in rows]


def run(a, data):
    """The reextract command.  data(name) -> path under data/."""
    convs_path = data(a.convs)
    source = {"db": os.path.realpath(a.db), "user_id": a.user_id}
    if os.path.exists(convs_path):
        saved = json.load(open(convs_path))
        # A list saved before it recorded its source has none to check; the
        # bench still refuses any conversation not owned by --user-id.
        if isinstance(saved, dict):
            if saved.get("source") != source:
                raise SystemExit(f"{convs_path} was picked from {saved.get('source')}, not "
                                 f"{source}; use another --convs file")
            convs = saved["ids"]
        else:
            convs = saved
        print(f"{len(convs)} conversations from {convs_path}")
    else:
        since = int(time.mktime(time.strptime(a.since, "%Y-%m-%d")))
        convs = pick_conversations(a.db, a.user_id, a.n, since, a.min_messages)
        with private_open(convs_path) as f:
            json.dump({"source": source, "ids": convs}, f)
        print(f"picked {len(convs)} conversations since {a.since} -> {convs_path}")
    if not convs:
        raise SystemExit("no conversations to extract")

    bench = Bench(a.binary, a.run_dir, a.provider, a.model, a.effort, a.config)
    ready = bench.ready
    sha = ready.get("extraction_prompt_sha256", "")[:8]
    model, effort = ready.get("extraction_model", "?"), ready.get("extraction_effort") or "off"
    # A filename: an OpenRouter model is a vendor/model slug.
    label = re.sub(r"[^A-Za-z0-9.-]+", "_", a.label or f"{sha}_{model}_{effort}")
    db_out, map_out = data(f"reextract_{label}.db"), data(f"reextract_{label}.map.json")
    # What the bench reports it runs, not what was asked for.
    print(f"prompt {sha}, {ready.get('extraction_provider', '?')} {model} effort {effort} "
          f"-> {db_out}")

    outcome = Counter()
    t0 = time.time()
    try:
        for i, cid in enumerate(convs, 1):
            r = bench.send({"cmd": "import_conversation", "source_db": a.db, "conv_id": cid,
                            "owner_id": a.user_id})
            if r.get("status") != "ok" or not r.get("messages"):
                outcome["import failed"] += 1
                print(f"  {i}/{len(convs)} conv {cid}: import failed "
                      f"({r.get('message', 'no messages')})")
                continue
            r = bench.send({"cmd": "extract_stored", "conv_id": cid}, EXTRACT_TIMEOUT_S)
            if r.get("status") != "ok":
                # A timed-out extraction is still running and would leak into
                # the next conversation's numbers: stop rather than go on.
                raise SystemExit(f"conv {cid}: extraction failed ({r.get('message')}); "
                                 "run stopped, nothing saved")
            if r.get("transient"):
                what = "failed (transient)"
            elif r.get("ran"):
                what = "extracted"
            else:
                what = f"skipped ({r.get('skip', 'nothing extracted')})"
            outcome[what] += 1
            print(f"  {i}/{len(convs)} conv {cid}: {what}, {r.get('live_facts_added', 0):+d} "
                  f"live facts ({r.get('live_facts', 0)} total)  {time.time() - t0:.0f}s")
        r = bench.send({"cmd": "snapshot_save", "db_path": db_out, "map_path": map_out}, 300)
    finally:
        bench.close()
    if r.get("status") != "ok":
        raise SystemExit(f"snapshot failed: {r.get('message')}")
    for p in (db_out, map_out):
        os.chmod(p, 0o600)

    facts = scratch_facts(db_out)
    facts_out = data(f"reextract_{label}_facts.jsonl")
    with private_open(facts_out) as f:
        for x in facts:
            f.write(json.dumps(x) + "\n")
    per = Counter(x["conversation"] for x in facts)
    print(f"done in {time.time() - t0:.0f}s: " +
          ", ".join(f"{k} {v}" for k, v in outcome.items()))
    print(f"model calls {bench.usage['calls']}: tokens in {bench.usage['in']} "
          f"out {bench.usage['out']}")
    print(f"{len(facts)} live facts from {len(per)} conversations "
          f"({len(facts) / max(1, outcome['extracted']):.1f} per extracted conversation) "
          f"-> {facts_out}")
    print(f"next: classify --set {os.path.basename(facts_out)}, then report --set "
          f"{os.path.basename(facts_out)}")
