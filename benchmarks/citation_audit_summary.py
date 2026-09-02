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


# focus_source.h FOCUS_SCORE_NA sentinel (no breakdown recorded) — excluded from stats.
FOCUS_SCORE_NA = -1.0


def scores(csv):
    """Split a CSV of per-item final_score floats, dropping the NA sentinel and junk."""
    out = []
    for x in (csv.split(",") if csv else []):
        x = x.strip()
        if not x:
            continue
        try:
            v = float(x)
        except ValueError:
            continue
        if v > FOCUS_SCORE_NA:  # drop -1.0 "no score recorded"
            out.append(v)
    return out


def pctile(sorted_vals, q):
    """Linear-interpolated percentile q in [0,1] of an already-sorted list."""
    if not sorted_vals:
        return None
    if len(sorted_vals) == 1:
        return sorted_vals[0]
    pos = q * (len(sorted_vals) - 1)
    lo = int(pos)
    frac = pos - lo
    hi = min(lo + 1, len(sorted_vals) - 1)
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * frac


def stat_line(label, vals):
    """One-line distribution summary: n / min / p25 / median / mean / p75 / max."""
    if not vals:
        return f" {label:<26}: (none)"
    s = sorted(vals)
    mean = sum(s) / len(s)
    return (f" {label:<26}: n={len(s):<4} min={s[0]:.3f} p25={pctile(s, .25):.3f} "
            f"med={pctile(s, .5):.3f} mean={mean:.3f} p75={pctile(s, .75):.3f} max={s[-1]:.3f}")


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
            f"injected_ids, cited_ids, injected_scores, dropped_count, "
            f"tool_surfaced_ids, dropped_tool_count "
            f"FROM memory_citation_audit {clause} ORDER BY id",
            params,
        ).fetchall()
    except sqlite3.Error as e:
        sys.exit(f"query failed (is the schema >= v79?): {e}")

    if not rows:
        print("No audit rows match. (Feature off, or no turns surfaced memories yet.)")
        return

    turns = len(rows)
    turns_cited = 0
    tot_inj = tot_cit = tot_drop = 0
    recall_inj = recall_cit = 0
    # Option B: tool-sourced citation universe (facts shown via a memory tool this
    # turn) vs the focus-injection universe.
    tot_tool_surf = tot_tool_cit = tot_drop_tool = 0
    turns_tool = 0  # turns that surfaced >=1 tool fact
    cited_kind = Counter()
    waste_kind = Counter()      # injected but not cited, by kind
    inj_kind = Counter()        # all injected, by kind
    cite_freq = Counter()       # how often each item_id is cited

    # Score distributions. "recall" = restricted to turns that cited >=1 item, the
    # clean-label population (a no-citation turn can't tell "unused" from "used-but-
    # -untagged", so its uncited scores are ambiguous and kept out of the cut-fitting).
    used_all, unused_all = [], []            # cited vs uncited item scores, every turn
    used_recall, unused_recall = [], []      # same, but only on citing turns
    used_by_kind = {}                        # kind -> [cited scores]  (citing turns)
    unused_by_kind = {}                      # kind -> [uncited scores] (citing turns)
    nocite_top, nocite_bot, nocite_avg = [], [], []  # per no-citation turn: max/min/mean
    scored_rows = 0                          # rows carrying aligned scores (>= v78)

    for (_id, _ts, _conv, _msg, _uid, inj_csv, cit_csv, sc_csv, drop, tool_csv,
         drop_tool) in rows:
        inj, cit = ids(inj_csv), ids(cit_csv)
        cited_set = set(cit)
        tot_inj += len(inj)
        tot_cit += len(cit)
        tot_drop += drop

        # Tool-sourced universe (Option B). tool_surfaced_ids may be NULL on pre-v79
        # rows; ids() tolerates that. A cited id is a "tool cite" when it is in the
        # tool universe (provenance derived by set membership).
        tool = ids(tool_csv)
        tool_set = set(tool)
        tot_tool_surf += len(tool)
        tot_drop_tool += (drop_tool or 0)
        if tool:
            turns_tool += 1
        tot_tool_cit += len(cited_set & tool_set)
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

        # Score attribution — only when injected_scores aligns 1:1 with injected_ids.
        sc = scores(sc_csv)
        if sc and len(sc) == len(inj):
            scored_rows += 1
            turn_used, turn_unused = [], []
            for it, s in zip(inj, sc):
                if it in cited_set:
                    turn_used.append(s)
                    used_all.append(s)
                else:
                    turn_unused.append(s)
                    unused_all.append(s)
            if cit:  # clean-label population
                used_recall.extend(turn_used)
                unused_recall.extend(turn_unused)
                for it, s in zip(inj, sc):
                    tgt = used_by_kind if it in cited_set else unused_by_kind
                    tgt.setdefault(kind(it), []).append(s)
            else:    # no-citation turn: profile top/bottom/avg of what we fed it
                nocite_top.append(max(sc))
                nocite_bot.append(min(sc))
                nocite_avg.append(sum(sc) / len(sc))

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
    # Option B: tool-sourced citation (facts shown via a memory search/recall tool).
    print(" TOOL-SOURCED (Option B)")
    print(f"   turns surfacing a tool fact   : {turns_tool}")
    print(f"   tool facts surfaced (total)   : {tot_tool_surf}")
    print(f"   tool facts cited (total)      : {tot_tool_cit}")
    print(f"   tool-cite precision           : {pct(tot_tool_cit, tot_tool_surf)}")
    print(f"   dropped tool ids (mis-copied) : {tot_drop_tool}   <- want 0")
    print()
    print(" cited by kind      :", dict(cited_kind) or "{}")
    print(" injected by kind   :", dict(inj_kind) or "{}")
    print(" WASTE by kind      :", dict(waste_kind), "(injected but never cited)")
    if cite_freq:
        top = ", ".join(f"{k}×{v}" for k, v in cite_freq.most_common(5))
        print(" most-cited items   :", top)

    # ---- Score distributions (v78+): used vs unused, and the injection-floor cut ----
    print()
    print(" SCORE DISTRIBUTIONS (injected final_score)")
    if scored_rows == 0:
        print("   (no rows carry injected_scores yet — needs schema v78 + fresh turns)")
    else:
        print(f"   rows with aligned scores        : {scored_rows}/{turns}")
        print(stat_line("cited (used)   all turns", used_all))
        print(stat_line("uncited (unused) all turns", unused_all))
        print(stat_line("cited (used)   citing-turns", used_recall))
        print(stat_line("uncited (unused) citing-turns", unused_recall))
        print()
        print("   per-kind, citing-turns only:")
        for k in sorted(set(used_by_kind) | set(unused_by_kind)):
            print(stat_line(f"  {k} used", used_by_kind.get(k, [])))
            print(stat_line(f"  {k} unused", unused_by_kind.get(k, [])))

        # No-citation turns: was even the best item weak (no-memory turn) or did a
        # strong item go unused (retrieval hit the model ignored / didn't tag)?
        print()
        print(f"   no-citation turns ({len(nocite_top)}): per-turn top / bottom / avg score")
        print(stat_line("  best item per turn", nocite_top))
        print(stat_line("  worst item per turn", nocite_bot))
        print(stat_line("  mean item per turn", nocite_avg))

        # Floor sweep on the clean-label population: at each candidate floor, how much
        # cited signal is kept vs how much uncited waste is cut.  Want high keep, high cut.
        if used_recall and unused_recall:
            print()
            print("   injection-floor sweep (citing-turns population):")
            print(f"   {'floor':>7} {'cited kept':>11} {'uncited cut':>12}")
            u_sorted = sorted(used_recall)
            lo = min(u_sorted[0], min(unused_recall))
            hi = max(u_sorted[-1], max(unused_recall))
            steps = 10
            for i in range(steps + 1):
                f = lo + (hi - lo) * i / steps
                kept = sum(1 for s in used_recall if s >= f)
                cut = sum(1 for s in unused_recall if s < f)
                print(f"   {f:>7.3f} {pct(kept, len(used_recall)):>11} {pct(cut, len(unused_recall)):>12}")
            print("   (pick the floor that keeps ~95% cited while cutting the most uncited)")

    # ---- Saturation telemetry (Phase 2 safety instrument, design §8/G9) ----
    # confidence IS the focus importance term AND the `ORDER BY confidence DESC`
    # retrieval key, so a +boost on citation moves both survival and rank.  Watch:
    # what fraction of facts have pinned at 1.0, and whether the pinned set is the
    # genuinely-cited set (acceptable) rather than the whole store (over-reinforced).
    # Reads memory_facts.last_cited (v83) directly — the audit table has no
    # confidence column.  All-NULL last_cited = reinforcement still inert.
    ucl = " WHERE user_id = ?" if args.user is not None else ""
    uargs = (args.user,) if args.user is not None else ()
    uand = " AND user_id = ?" if args.user is not None else ""
    print()
    print(" SATURATION (Phase 2 safety instrument)")
    try:
        n_facts, n_pinned, n_evercited = db.execute(
            "SELECT COUNT(*), COALESCE(SUM(confidence >= 1.0), 0), "
            "COALESCE(SUM(last_cited IS NOT NULL), 0) FROM memory_facts" + ucl, uargs
        ).fetchone()
        print(f"   total facts                   : {n_facts}")
        print(f"   pinned at confidence 1.0      : {n_pinned}  ({pct(n_pinned, n_facts)} of all facts)")
        print("     (note: explicit facts DEFAULT to 1.0 — the drift signal is the cited-vs-")
        print("      never-cited separation below, not this raw fraction)")
        print(f"   ever cited (last_cited set)   : {n_evercited}")
        if n_evercited:
            pinned_cited = db.execute(
                "SELECT COALESCE(SUM(confidence >= 1.0), 0) FROM memory_facts "
                "WHERE last_cited IS NOT NULL" + uand, uargs
            ).fetchone()[0]
            print(f"   pinned AMONG ever-cited       : {pinned_cited}  ({pct(pinned_cited, n_evercited)})"
                  "  <- want the pinned set to BE the cited set")
            if args.days is not None:
                cutoff = int(time.time() - args.days * 86400)
                win = db.execute(
                    "SELECT COUNT(*) FROM memory_facts WHERE last_cited >= ?" + uand,
                    (cutoff,) + uargs
                ).fetchone()[0]
                print(f"   cited within window           : {win}")
            # Confidence separation = the drift signal: reinforcement should lift the
            # cited population's confidence above the never-cited baseline over time.
            cited_conf = [r[0] for r in db.execute(
                "SELECT confidence FROM memory_facts WHERE last_cited IS NOT NULL" + uand, uargs)]
            never_conf = [r[0] for r in db.execute(
                "SELECT confidence FROM memory_facts WHERE last_cited IS NULL" + uand, uargs)]
            print(stat_line("  cited-fact confidence", cited_conf))
            print(stat_line("  never-cited confidence", never_conf))
            # Hot-fact rank drift: are the most-cited facts the ones pinning?
            hot = [(it, c) for it, c in cite_freq.most_common(8) if kind(it) == "fact"]
            if hot:
                print("   hot-fact drift (most-cited fact -> current confidence):")
                for it, c in hot:
                    r = db.execute("SELECT confidence FROM memory_facts WHERE id = ?",
                                   (int(it.split(":")[1]),)).fetchone()
                    conf = f"{r[0]:.3f}" if r else "gone"
                    pin = " PINNED" if (r and r[0] >= 1.0) else ""
                    print(f"     {it:>14} cited x{c:<3} conf={conf}{pin}")
        else:
            print("   (0 facts cited yet — reinforcement inert: "
                  "citation_reinforcement_boost = 0.0, or not enabled)")
    except sqlite3.Error as e:
        print(f"   (saturation stats unavailable — needs schema >= v83: {e})")

    print()
    print(f" --- {min(args.recent, turns)} most recent turns ---")
    print(f" {'when':<19} {'conv':>6} {'msg':>7} {'inj':>4} {'cit':>4} {'drop':>4} {'top':>6} {'used_lo':>7}")
    for row in rows[-args.recent:]:
        # Slice the leading fields this view needs; robust to trailing columns
        # (tool_surfaced_ids/dropped_tool_count and any future additions).
        _id, ts, conv, msg, _uid, inj_csv, cit_csv, sc_csv, drop = row[:9]
        when = datetime.fromtimestamp(ts).strftime("%Y-%m-%d %H:%M:%S") if ts else "?"
        inj, cit = ids(inj_csv), ids(cit_csv)
        sc = scores(sc_csv)
        top = f"{max(sc):.3f}" if sc else "-"
        # lowest score among items the model actually cited this turn (aligned rows only)
        used_lo = "-"
        if sc and len(sc) == len(inj):
            cset = set(cit)
            used = [s for it, s in zip(inj, sc) if it in cset]
            if used:
                used_lo = f"{min(used):.3f}"
        print(f" {when:<19} {conv:>6} {msg:>7} {len(inj):>4} {len(cit):>4} {drop:>4} {top:>6} {used_lo:>7}")


if __name__ == "__main__":
    main()
