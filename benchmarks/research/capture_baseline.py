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
"""Deep-research smoke baseline capture (DEEP_RESEARCH_DESIGN.md §15 Step 11).

READ-ONLY snapshot of executed research runs from DAWN's auth.db: per-run budgets
(rounds / tool calls / input tokens / wall-clock), coverage-ledger counts
(open/answered/unanswerable), claim + distinct-source counts, and the rendered
report — so the digest+synthesis quality (the named P0 mediocrity risk, §14) can
be eyeballed now and compared as the prompts evolve.

This does NOT run research (that needs the live daemon + LLM + web search — submit
the briefs conversationally per README.md first). It only reports what already ran.

  ./capture_baseline.py                       # last 10 runs → stdout summary
  ./capture_baseline.py --md baseline.md       # + full reports for eyeballing
  ./capture_baseline.py --out baseline.json    # + machine-readable metrics
  ./capture_baseline.py --run-id 42            # one run
  ./capture_baseline.py --db /path/to/auth.db  # non-default DB
"""

import argparse
import json
import sqlite3
import sys
from datetime import datetime, timezone

DEFAULT_DB = "/var/lib/dawn/auth.db"


def _connect_ro(path):
    """Open the DB strictly read-only so a capture can never mutate live data."""
    try:
        return sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    except sqlite3.OperationalError as e:
        sys.exit(f"error: cannot open {path} read-only: {e}")


def _has_research_tables(conn):
    row = conn.execute(
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='research_runs'"
    ).fetchone()
    return row and row[0] > 0


def _fmt_ts(ts):
    if not ts:
        return "-"
    return datetime.fromtimestamp(ts, tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%SZ")


def _collect_run(conn, run):
    """Build the metrics dict for one research_runs row (a sqlite3.Row)."""
    run_id = run["id"]

    # Question ledger, split by resolution_reason when the column exists (schema v76+):
    # 'stale' = controller auto-retired (Phase 2), 'agent' = research_mark_unanswerable.
    q = {"open": 0, "answered": 0, "unanswerable": 0, "stale": 0, "agent": 0, "total": 0}
    try:
        qrows = conn.execute(
            "SELECT status, resolution_reason, COUNT(*) c FROM research_questions "
            "WHERE run_id=? GROUP BY status, resolution_reason",
            (run_id,),
        ).fetchall()
        has_reason = True
    except sqlite3.OperationalError:
        qrows = conn.execute(
            "SELECT status, NULL resolution_reason, COUNT(*) c FROM research_questions "
            "WHERE run_id=? GROUP BY status",
            (run_id,),
        ).fetchall()
        has_reason = False
    for r in qrows:
        q[r["status"]] = q.get(r["status"], 0) + r["c"]
        q["total"] += r["c"]
        if r["resolution_reason"] in ("stale", "agent"):
            q[r["resolution_reason"]] += r["c"]

    # Completeness-critic activity: research_critic observe events on the run's
    # conversation. A re-arm = decision 'continue' with N gap sub-questions added;
    # otherwise the critic ran and confirmed the stop. No events => it never fired
    # (fuse stop, no natural end, or critic disabled). conversation_events predates
    # the research tables (v72 < v75), so this query is safe on any research DB.
    critic = {"rearms": 0, "gaps_added": 0, "confirmed": 0}
    for r in conn.execute(
        "SELECT payload FROM conversation_events "
        "WHERE conversation_id=? AND kind='research_critic'",
        (run["conversation_id"],),
    ):
        try:
            p = json.loads(r["payload"] or "{}")
        except (ValueError, TypeError):
            continue
        if p.get("decision") == "continue":
            critic["rearms"] += 1
            critic["gaps_added"] += int(p.get("gaps_added") or 0)
        else:
            critic["confirmed"] += 1

    claims = conn.execute(
        "SELECT COUNT(*) n, COUNT(DISTINCT source_url) src FROM research_claims WHERE run_id=?",
        (run_id,),
    ).fetchone()

    rev = conn.execute(
        "SELECT round, markdown, created_at FROM research_report_revisions "
        "WHERE run_id=? ORDER BY round DESC, id DESC LIMIT 1",
        (run_id,),
    ).fetchone()

    duration = None
    if run["finished_at"] and run["created_at"]:
        duration = run["finished_at"] - run["created_at"]

    return {
        "run_id": run_id,
        "conversation_id": run["conversation_id"],
        "user_id": run["user_id"],
        "brief": run["brief"],
        "mode": run["mode"],
        "status": run["status"],
        "stop_reason": run["stop_reason"],
        "report_doc_id": run["report_doc_id"],
        "rounds_run": run["rounds_run"],
        "tool_calls": run["tool_calls"],
        "input_tokens": run["input_tokens"],
        "created_at": run["created_at"],
        "finished_at": run["finished_at"],
        "duration_sec": duration,
        "questions": q,
        "has_reason": has_reason,
        "critic": critic,
        "claims": {"count": claims["n"], "distinct_sources": claims["src"]},
        "report": {
            "revision_round": rev["round"] if rev else None,
            "chars": len(rev["markdown"]) if rev else 0,
            "markdown": rev["markdown"] if rev else None,
        },
    }


def _print_summary(runs):
    if not runs:
        print("No research runs found. Submit the smoke briefs first (see README.md).")
        return
    hdr = (
        f"{'run':>4}  {'status':<12} {'stop':<12} {'rnds':>4} {'tools':>5} "
        f"{'tokens':>8} {'Q(a/t)':>7} {'crit':>6} {'claims':>6} {'src':>4}  brief"
    )
    print(hdr)
    print("-" * len(hdr))
    for r in runs:
        q = r["questions"]
        c = r["critic"]
        # 'Nr/Mg' = re-armed N times adding M gaps; 'stop' = ran + confirmed; '-' = never fired.
        crit = (
            f"{c['rearms']}r/{c['gaps_added']}g"
            if c["rearms"]
            else ("stop" if c["confirmed"] else "-")
        )
        print(
            f"{r['run_id']:>4}  {r['status']:<12} {(r['stop_reason'] or '-'):<12} "
            f"{r['rounds_run']:>4} {r['tool_calls']:>5} {r['input_tokens']:>8} "
            f"{str(q['answered']) + '/' + str(q['total']):>7} {crit:>6} "
            f"{r['claims']['count']:>6} {r['claims']['distinct_sources']:>4}  "
            f"{r['brief'][:60]}"
        )


def _write_markdown(runs, path):
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"# Deep-research smoke baseline — {_fmt_ts(datetime.now().timestamp())}\n\n")
        f.write(f"{len(runs)} run(s). Read each report for compression/synthesis quality.\n\n")
        for r in runs:
            f.write(f"---\n\n## Run #{r['run_id']} — {r['status']} ({r['stop_reason'] or '-'})\n\n")
            f.write(f"**Brief:** {r['brief']}\n\n")
            q = r["questions"]
            f.write(
                f"- rounds: {r['rounds_run']} · tool calls: {r['tool_calls']} · "
                f"input tokens: {r['input_tokens']} · duration: "
                f"{r['duration_sec'] if r['duration_sec'] is not None else '-'}s\n"
            )
            # Only show the stale/agent split when it fully accounts for the
            # unanswerables — legacy rows (pre-v76 resolution_reason) read NULL, so
            # "(0 stale, 0 agent)" would misleadingly imply neither when the split is
            # simply unrecorded (it lives in the observe events for those runs).
            if q["unanswerable"] and q["stale"] + q["agent"] == q["unanswerable"]:
                unans = (
                    f"{q['unanswerable']} unanswerable ({q['stale']} stale, {q['agent']} agent)"
                )
            else:
                unans = f"{q['unanswerable']} unanswerable"
            f.write(
                f"- questions: {q['answered']} answered / {q['open']} open / "
                f"{unans} (of {q['total']})\n"
            )
            c = r["critic"]
            if c["rearms"] or c["confirmed"]:
                f.write(
                    f"- critic: re-armed {c['rearms']}× (+{c['gaps_added']} gaps), "
                    f"confirmed stop {c['confirmed']}×\n"
                )
            f.write(
                f"- claims: {r['claims']['count']} from {r['claims']['distinct_sources']} "
                f"distinct sources · report_doc_id: {r['report_doc_id'] or '-'}\n\n"
            )
            f.write("### Rendered report\n\n")
            f.write((r["report"]["markdown"] or "_(no report revision persisted)_") + "\n\n")
    print(f"wrote markdown → {path}")


def main():
    ap = argparse.ArgumentParser(description="Capture a deep-research smoke baseline (read-only).")
    ap.add_argument("--db", default=DEFAULT_DB, help=f"auth.db path (default {DEFAULT_DB})")
    ap.add_argument("--run-id", type=int, help="only this run")
    ap.add_argument("--limit", type=int, default=10, help="most recent N runs (default 10)")
    ap.add_argument("--all", action="store_true", help="every run (overrides --limit)")
    ap.add_argument("--out", help="write per-run metrics JSON here")
    ap.add_argument("--md", help="write full reports as markdown here (for eyeballing)")
    args = ap.parse_args()

    conn = _connect_ro(args.db)
    conn.row_factory = sqlite3.Row
    if not _has_research_tables(conn):
        sys.exit("error: no research_runs table — is this a schema-v75+ DAWN auth.db?")

    if args.run_id:
        rows = conn.execute("SELECT * FROM research_runs WHERE id=?", (args.run_id,)).fetchall()
    elif args.all:
        rows = conn.execute("SELECT * FROM research_runs ORDER BY id").fetchall()
    else:
        rows = conn.execute(
            "SELECT * FROM research_runs ORDER BY id DESC LIMIT ?", (args.limit,)
        ).fetchall()
        rows = list(reversed(rows))

    runs = [_collect_run(conn, r) for r in rows]
    conn.close()

    _print_summary(runs)

    if args.out:
        # Drop the full markdown from the metrics JSON — it lives in --md.
        slim = []
        for r in runs:
            c = dict(r)
            c["report"] = {k: v for k, v in r["report"].items() if k != "markdown"}
            slim.append(c)
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump({"captured_at": datetime.now(tz=timezone.utc).isoformat(), "runs": slim},
                      f, indent=2)
        print(f"wrote metrics → {args.out}")

    if args.md:
        _write_markdown(runs, args.md)


if __name__ == "__main__":
    main()
