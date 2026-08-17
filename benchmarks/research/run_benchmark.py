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
"""Unattended deep-research benchmark driver (DEEP_RESEARCH_DESIGN.md §16).

Drives research runs headlessly through the `dawn-admin research` verb and
collects a per-task artifact for offline scoring:

    start (via dawn-admin, --brief-file) -> poll `research status` to a terminal
    state (timeout -> cancel) -> extract the report + claims from auth.db
    (read-only) -> write <out>/<task_id>.json

Scoring is a SEPARATE, offline step over these artifacts (score_exact_match.py,
score_deepresearch_bench.py) so a re-score never re-runs research.

Requires: the daemon running with [research].enabled + a search backend, and a
VALID --user (use a dedicated eval account, not the primary user — a benchmark
brief is third-party text).

Task file: {"queries": [{"id","brief", ...}]} (the smoke_queries.json shape) or a
bare list of the same objects.  Optional per-task "gold"/"reference"/"should_cover"
are copied into the artifact for the scorers; nothing else is required.

  ./run_benchmark.py --tasks smoke_queries.json --user 2
  ./run_benchmark.py --tasks smoke_queries.json --user 2 --only jetson_llm_serving
  ./run_benchmark.py --extract-only 15                 # re-extract an existing run
"""

import argparse
import json
import os
import re
import sqlite3
import subprocess
import sys
import tempfile
import time

DEFAULT_DB = "/var/lib/dawn/auth.db"
DEFAULT_ADMIN = "./build-debug/dawn-admin/dawn-admin"
TERMINAL = {"done", "failed", "cancelled"}
_KV = re.compile(r"(\w+)=(\S+)")

# dawn's own config search order (highest priority first).  The driver usually
# runs from the repo root where ./dawn.toml lives, so that is the common hit.
CONFIG_CANDIDATES = [
    os.path.expanduser("~/.config/dawn/dawn.toml"),
    "dawn.toml",
    "/etc/dawn/dawn.toml",
]
# The [research] budget/shape knobs worth pinning to a score (A1).
RESEARCH_STAMP_KEYS = (
    "max_rounds", "max_input_tokens", "min_sources", "round_digest_max_chars",
    "saturation_rounds", "plan_freeze_round", "stale_rounds", "critic_max_rearm",
    "max_tool_calls", "completion_commentary", "capture_revisions",
)


def _run_admin(admin, args, timeout=60):
    """Invoke `dawn-admin <args>`; return (rc, stdout, stderr)."""
    try:
        p = subprocess.run(
            [admin, *args], capture_output=True, text=True, timeout=timeout
        )
        return p.returncode, p.stdout.strip(), p.stderr.strip()
    except subprocess.TimeoutExpired:
        return 1, "", f"dawn-admin {args[0] if args else ''} timed out"
    except FileNotFoundError:
        sys.exit(f"error: dawn-admin not found at '{admin}' (pass --admin)")


def _parse_kv(text):
    """Parse a 'k=v k=v' status line into a dict of strings."""
    return {m.group(1): m.group(2) for m in _KV.finditer(text)}


def _start(admin, user_id, brief):
    """Spawn a run via --brief-file (avoids shell-quoting a long brief).
    Returns (run_id, conv_id) or (None, error_str)."""
    fd, path = tempfile.mkstemp(prefix="research_brief_", suffix=".txt")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(brief)
        rc, out, err = _run_admin(
            admin, ["research", "start", "--user", str(user_id), "--brief-file", path]
        )
    finally:
        os.unlink(path)
    if rc != 0:
        return None, (err or out or "start failed")
    kv = _parse_kv(out)
    if "run_id" not in kv:
        return None, f"unexpected start output: {out!r}"
    return int(kv["run_id"]), int(kv.get("conv_id", 0))


def _status(admin, user_id, run_id):
    """Return the parsed status dict, or None on error."""
    rc, out, err = _run_admin(
        admin, ["research", "status", "--user", str(user_id), str(run_id)]
    )
    if rc != 0:
        return None
    return _parse_kv(out)


def _cancel(admin, user_id, run_id):
    _run_admin(admin, ["research", "cancel", "--user", str(user_id), str(run_id)])


def _poll(admin, user_id, run_id, timeout, interval):
    """Poll until terminal or timeout (then cancel).  Returns the final status
    dict (best-effort)."""
    start = time.monotonic()
    last = None
    while True:
        st = _status(admin, user_id, run_id)
        if st is not None:
            last = st
            if st.get("status") in TERMINAL:
                return st
        if time.monotonic() - start > timeout:
            print(f"    [timeout] run {run_id} exceeded {timeout}s — cancelling", flush=True)
            _cancel(admin, user_id, run_id)
            # give the controller a boundary to stop + persist, then read once more
            time.sleep(min(interval, 15))
            return _status(admin, user_id, run_id) or last
        time.sleep(interval)


def _connect_ro(db):
    try:
        return sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    except sqlite3.OperationalError as e:
        sys.exit(f"error: cannot open {db} read-only: {e}")


def _extract(conn, run_id):
    """Read the run header, final rendered report, and per-claim evidence for a
    run (read-only).  Returns a dict, or None if the run row is missing."""
    conn.row_factory = sqlite3.Row
    run = conn.execute(
        "SELECT id, conversation_id, brief, status, stop_reason, rounds_run, "
        "input_tokens, report_doc_id FROM research_runs WHERE id=?",
        (run_id,),
    ).fetchone()
    if run is None:
        return None

    # Final assembled report = the newest revision (always written at synthesis;
    # capture_revisions only gates the per-round snapshots).
    rev = conn.execute(
        "SELECT markdown FROM research_report_revisions WHERE run_id=? "
        "ORDER BY round DESC, id DESC LIMIT 1",
        (run_id,),
    ).fetchone()

    # Per-claim projection for the FACT citation scorer (claim -> source_url).
    claims = [
        {
            "claim": r["claim"],
            "source_url": r["source_url"],
            "quote": r["quote"],
            "question_id": r["question_id"],
            "round": r["round"],
        }
        for r in conn.execute(
            "SELECT claim, source_url, quote, question_id, round FROM research_claims "
            "WHERE run_id=? ORDER BY question_id, id",
            (run_id,),
        ).fetchall()
    ]

    q = {"open": 0, "answered": 0, "unanswerable": 0}
    for r in conn.execute(
        "SELECT status, COUNT(*) c FROM research_questions WHERE run_id=? GROUP BY status",
        (run_id,),
    ).fetchall():
        q[r["status"]] = r["c"]

    return {
        "run_id": run["id"],
        "conversation_id": run["conversation_id"],
        "status": run["status"],
        "stop_reason": run["stop_reason"],
        "rounds_run": run["rounds_run"],
        "input_tokens": run["input_tokens"],
        "report_doc_id": run["report_doc_id"],
        "coverage": q,
        "distinct_sources": len({c["source_url"] for c in claims if c["source_url"]}),
        "n_claims": len(claims),
        "report_md": rev["markdown"] if rev else "",
        "claims": claims,
    }


def _artifact_path(out_dir, task_id):
    safe = re.sub(r"[^A-Za-z0-9_.-]", "_", str(task_id))
    return os.path.join(out_dir, f"{safe}.json")


def _git_rev():
    """Short HEAD sha of the repo the driver runs in, '-dirty' if the tree has
    uncommitted changes (so a B2 prompt-tuning run isn't scored as if it were the
    committed prompt).  None if git is unavailable."""
    try:
        p = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, timeout=5,
        )
        if p.returncode != 0:
            return None
        rev = p.stdout.strip()
        dirty = subprocess.run(
            ["git", "status", "--porcelain"], capture_output=True, text=True, timeout=5
        )
        if dirty.returncode == 0 and dirty.stdout.strip():
            rev += "-dirty"
        return rev
    except (OSError, subprocess.SubprocessError):
        return None


def _find_config(explicit):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in CONFIG_CANDIDATES:
        if os.path.exists(c):
            return c
    return None


def _read_toml_flat(path):
    """Minimal reader for keys under [section] headers — enough for dawn.toml's
    [llm*]/[research] scalars (bool/int/quoted-string, trailing '# comments'
    tolerated) PLUS multi-line string arrays (the <provider>_models lists, needed to
    resolve the active cloud model).  NOT a general TOML parser; returns {section:
    {key: value}} with values coerced to bool/int/str/list-of-str."""
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    sections, cur, i = {}, None, 0
    while i < len(lines):
        line = lines[i].strip()
        i += 1
        if not line or line.startswith("#"):
            continue
        m = re.match(r"\[([^\]]+)\]", line)
        if m:
            cur = m.group(1).strip()
            sections.setdefault(cur, {})
            continue
        if cur is None or "=" not in line:
            continue
        key, _, val = line.partition("=")
        key, val = key.strip(), val.strip()
        if val.startswith("["):
            # string array, possibly multi-line: collect quoted items through ']'.
            items = re.findall(r'"([^"]*)"', val)
            while "]" not in val and i < len(lines):
                val = lines[i].strip()
                i += 1
                items += re.findall(r'"([^"]*)"', val)
            sections[cur][key] = items
            continue
        if val.startswith('"'):
            end = val.find('"', 1)
            sections[cur][key] = val[1:end] if end > 0 else val.strip('"')
            continue
        tok = val.split("#", 1)[0].split()
        tok = tok[0] if tok else ""
        if tok in ("true", "false"):
            sections[cur][key] = (tok == "true")
        else:
            try:
                sections[cur][key] = int(tok)
            except ValueError:
                sections[cur][key] = tok
    return sections


def _resolve_cloud_model(lc):
    """The ACTIVE cloud model: <provider>_models[<provider>_default_model_idx], or
    the openrouter_models list when the gateway is on — dawn.toml selects the cloud
    model by array index, not a single 'model' key.  Falls back to a bare 'model'."""
    prov = "openrouter" if lc.get("use_openrouter") is True else lc.get("provider", "")
    arr = lc.get(f"{prov}_models")
    idx = lc.get(f"{prov}_default_model_idx", 0)
    if isinstance(arr, list) and isinstance(idx, int) and 0 <= idx < len(arr):
        return arr[idx]
    return lc.get("model")


def _run_config(config_path, prompt_tag):
    """Snapshot what produced this batch: code revision, the daemon's active model,
    and the [research] budgets — stamped on every artifact so a score is never a
    naked number (A1).  Best-effort: a missing/unreadable config still yields the
    code_rev + prompt_tag, with a note."""
    rc = {"code_rev": _git_rev(), "prompt_tag": prompt_tag, "config_path": config_path}
    if not config_path:
        rc["note"] = "no dawn.toml found (pass --config); model/budgets not stamped"
        return rc
    try:
        cfg = _read_toml_flat(config_path)
    except OSError as e:
        rc["note"] = f"config read failed: {e}"
        return rc
    llm = cfg.get("llm", {})
    ltype = llm.get("type", "cloud")
    lc = cfg.get("llm.local" if ltype == "local" else "llm.cloud", {})
    if ltype == "local":
        # local dawn.toml names no model (the endpoint serves it) — record the
        # server so a local-mode artifact still identifies what ran.
        rc["llm"] = {"type": "local", "model": lc.get("model"), "endpoint": lc.get("endpoint")}
    else:
        rc["llm"] = {"type": "cloud", "provider": lc.get("provider"),
                     "model": _resolve_cloud_model(lc)}
        if "use_openrouter" in lc:
            rc["llm"]["use_openrouter"] = lc["use_openrouter"]
    res = cfg.get("research", {})
    rc["research"] = {k: res[k] for k in RESEARCH_STAMP_KEYS if k in res}
    return rc


def _load_tasks(path):
    with open(path) as f:
        data = json.load(f)
    tasks = data["queries"] if isinstance(data, dict) else data
    out = []
    for t in tasks:
        if isinstance(t, dict) and t.get("brief") and t.get("id"):
            out.append(t)
    if not out:
        sys.exit(f"error: no usable tasks (need id + brief) in {path}")
    return out


def main():
    ap = argparse.ArgumentParser(description="Unattended deep-research benchmark driver.")
    ap.add_argument("--tasks", help="task set JSON ({queries:[...]} or a bare list)")
    ap.add_argument("--user", type=int, help="eval account user_id (required to run tasks)")
    ap.add_argument("--db", default=DEFAULT_DB, help=f"auth.db path (default {DEFAULT_DB})")
    ap.add_argument("--admin", default=DEFAULT_ADMIN, help="path to the dawn-admin binary")
    ap.add_argument("--out", default="benchmarks/research/results", help="artifact output dir")
    ap.add_argument("--timeout", type=int, default=3600, help="per-run wall-clock cap (s)")
    ap.add_argument("--poll", type=int, default=20, help="status poll interval (s)")
    ap.add_argument("--only", help="run only the task with this id")
    ap.add_argument("--force", action="store_true", help="re-run tasks whose artifact exists")
    ap.add_argument("--config", help="dawn.toml to stamp on artifacts (default: dawn's search path)")
    ap.add_argument("--prompt-tag", help="free-text marker for the prompt/config variant under test")
    ap.add_argument("--extract-only", type=int, metavar="RUN_ID",
                    help="re-extract an existing run to stdout and exit (no spawn)")
    args = ap.parse_args()

    # Extraction-only: validate the read path against an existing run, no daemon call.
    if args.extract_only is not None:
        conn = _connect_ro(args.db)
        art = _extract(conn, args.extract_only)
        conn.close()
        if art is None:
            sys.exit(f"error: no research_runs row for id {args.extract_only}")
        print(json.dumps(art, indent=2))
        return

    if not args.tasks or args.user is None:
        ap.error("--tasks and --user are required to run tasks (or use --extract-only)")
    if args.user <= 0:
        ap.error("--user must be a real, non-primary eval account")

    tasks = _load_tasks(args.tasks)
    if args.only:
        tasks = [t for t in tasks if t["id"] == args.only]
        if not tasks:
            sys.exit(f"error: no task with id '{args.only}'")
    os.makedirs(args.out, exist_ok=True)

    # Snapshot code rev + daemon model + [research] budgets ONCE for the batch —
    # the whole batch runs against one daemon config, so this is accurate (A1).
    run_cfg = _run_config(_find_config(args.config), args.prompt_tag)
    if "note" in run_cfg:
        print(f"[config] {run_cfg['note']}", flush=True)
    else:
        _m = run_cfg.get("llm", {}).get("model")
        print(f"[config] code={run_cfg.get('code_rev')} model={_m} "
              f"config={run_cfg.get('config_path')}", flush=True)

    summary = []
    for i, task in enumerate(tasks, 1):
        tid = task["id"]
        path = _artifact_path(args.out, tid)
        if os.path.exists(path) and not args.force:
            print(f"[{i}/{len(tasks)}] {tid}: artifact exists — skipping (use --force)", flush=True)
            summary.append((tid, "skipped", "-"))
            continue

        print(f"[{i}/{len(tasks)}] {tid}: starting…", flush=True)
        run_id, conv_id = _start(args.admin, args.user, task["brief"])
        if run_id is None:
            print(f"    start failed: {conv_id}", flush=True)  # conv_id holds the error
            summary.append((tid, "start_failed", "-"))
            continue
        print(f"    run_id={run_id} conv_id={conv_id} — polling (timeout {args.timeout}s)", flush=True)

        st = _poll(args.admin, args.user, run_id, args.timeout, args.poll)
        final = st.get("status", "unknown") if st else "unknown"
        stop = st.get("stop_reason", "-") if st else "-"

        conn = _connect_ro(args.db)
        art = _extract(conn, run_id)
        conn.close()
        if art is None:
            print(f"    extraction failed (no run row for {run_id})", flush=True)
            summary.append((tid, final, "no-extract"))
            continue

        art["task_id"] = tid
        art["brief"] = task["brief"]
        art["run_config"] = run_cfg
        for k in ("gold", "reference", "should_cover", "shape"):
            if k in task:
                art[k] = task[k]
        with open(path, "w") as f:
            json.dump(art, f, indent=2)

        print(
            f"    {final}/{stop} — {art['n_claims']} claims, {art['distinct_sources']} sources, "
            f"answered {art['coverage'].get('answered', 0)} → {path}",
            flush=True,
        )
        summary.append((tid, f"{final}/{stop}", art["n_claims"]))

    print("\n=== summary ===")
    for tid, status, claims in summary:
        print(f"  {tid:<28} {status:<20} claims={claims}")


if __name__ == "__main__":
    main()
