#!/usr/bin/env python3
# This program is free software: you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version.  Distributed WITHOUT ANY WARRANTY; see <https://www.gnu.org/licenses/>.
"""
Summarize the memory-citation audit table (Phase 1 telemetry).

Reads `memory_citation_audit` and reports the injection-precision signal:
citation rate, injection precision (overall vs on recall turns), dropped-ordinal
count (should stay 0), and injected-but-never-cited "waste" broken down by item
kind.  Read-only; safe to run against a live daemon's DB.

Usage:
    python3 benchmarks/citation_audit_summary.py                 # default DB, all time
    python3 benchmarks/citation_audit_summary.py --days 3        # last 3 days
    python3 benchmarks/citation_audit_summary.py --user 1 --recent 15
    python3 benchmarks/citation_audit_summary.py --db /path/to/auth.db
"""

import argparse
import sqlite3
import sys
import time
from collections import Counter
from datetime import datetime

DEFAULT_DB = "/var/lib/dawn/auth.db"


def ids(csv):
    """Split a CSV item_id field into a list, dropping empties."""
    return [x for x in csv.split(",") if x] if csv else []


def kind(item_id):
    """'fact:12' -> 'fact'."""
    return item_id.split(":", 1)[0] if ":" in item_id else item_id


def main():
    ap = argparse.ArgumentParser(description="Summarize the memory-citation audit table.")
    ap.add_argument("--db", default=DEFAULT_DB, help=f"auth.db path (default {DEFAULT_DB})")
    ap.add_argument("--days", type=float, default=None, help="only turns from the last N days")
    ap.add_argument("--user", type=int, default=None, help="restrict to one user_id")
    ap.add_argument("--recent", type=int, default=10, help="show the N most recent turns (default 10)")
    args = ap.parse_args()

    try:
        db = sqlite3.connect(f"file:{args.db}?mode=ro", uri=True)
    except sqlite3.Error as e:
        sys.exit(f"cannot open {args.db}: {e}")

    where, params = [], []
    if args.days is not None:
        where.append("ts >= ?")
        params.append(int(time.time() - args.days * 86400))
    if args.user is not None:
        where.append("user_id = ?")
        params.append(args.user)
    clause = ("WHERE " + " AND ".join(where)) if where else ""

    try:
        rows = db.execute(
            f"SELECT id, ts, conversation_id, message_id, user_id, "
            f"injected_ids, cited_ids, dropped_count "
            f"FROM memory_citation_audit {clause} ORDER BY id",
            params,
        ).fetchall()
    except sqlite3.Error as e:
        sys.exit(f"query failed (is the schema >= v77?): {e}")

    if not rows:
        print("No audit rows match. (Feature off, or no turns surfaced memories yet.)")
        return

    turns = len(rows)
    turns_cited = 0
    tot_inj = tot_cit = tot_drop = 0
    recall_inj = recall_cit = 0
    cited_kind = Counter()
    waste_kind = Counter()      # injected but not cited, by kind
    inj_kind = Counter()        # all injected, by kind
    cite_freq = Counter()       # how often each item_id is cited

    for _id, _ts, _conv, _msg, _uid, inj_csv, cit_csv, drop in rows:
        inj, cit = ids(inj_csv), ids(cit_csv)
        cited_set = set(cit)
        tot_inj += len(inj)
        tot_cit += len(cit)
        tot_drop += drop
        for it in inj:
            inj_kind[kind(it)] += 1
            if it not in cited_set:
                waste_kind[kind(it)] += 1
        for it in cit:
            cited_kind[kind(it)] += 1
            cite_freq[it] += 1
        if cit:
            turns_cited += 1
            recall_inj += len(inj)
            recall_cit += len(cit)

    def pct(a, b):
        return f"{(a / b):.1%}" if b else "n/a"

    print("=" * 64)
    print(" Memory-Citation Audit Summary")
    if args.days is not None:
        print(f" window: last {args.days} day(s)" + (f", user {args.user}" if args.user else ""))
    print("=" * 64)
    print(f" turns audited (surfaced [M#])   : {turns}")
    print(f" turns with a citation           : {turns_cited}  ({pct(turns_cited, turns)} citation rate)")
    print(f" dropped ordinals (hallucinated) : {tot_drop}   <- want 0")
    print()
    print(f" injected item-slots (total)     : {tot_inj}")
    print(f" cited (total)                   : {tot_cit}")
    print(f" injection precision (all turns) : {pct(tot_cit, tot_inj)}")
    print(f" injection precision (recall only): {pct(recall_cit, recall_inj)}   ({recall_cit}/{recall_inj})")
    print()
    print(" cited by kind      :", dict(cited_kind) or "{}")
    print(" injected by kind   :", dict(inj_kind) or "{}")
    print(" WASTE by kind      :", dict(waste_kind), "(injected but never cited)")
    if cite_freq:
        top = ", ".join(f"{k}×{v}" for k, v in cite_freq.most_common(5))
        print(" most-cited items   :", top)

    print()
    print(f" --- {min(args.recent, turns)} most recent turns ---")
    print(f" {'when':<19} {'conv':>6} {'msg':>7} {'inj':>4} {'cit':>4} {'drop':>4}")
    for _id, ts, conv, msg, _uid, inj_csv, cit_csv, drop in rows[-args.recent:]:
        when = datetime.fromtimestamp(ts).strftime("%Y-%m-%d %H:%M:%S") if ts else "?"
        print(f" {when:<19} {conv:>6} {msg:>7} {len(ids(inj_csv)):>4} {len(ids(cit_csv)):>4} {drop:>4}")


if __name__ == "__main__":
    main()
