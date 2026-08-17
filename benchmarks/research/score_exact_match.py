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
"""Deterministic short-answer scorer for research artifacts (DEEP_RESEARCH_DESIGN.md §16.5).

For BrowseComp/GAIA-style tasks that carry a short `gold` answer: checks whether
the gold string appears anywhere in the run's report or recorded claims — a loose
but deterministic 'did it surface the answer' signal, needing no judge/API key.
This is the honest FLOOR the design calls for (research runs report, not pinpoint,
so a gold appearing in the prose is a proxy, not a strict match). For report
QUALITY use score_deepresearch_bench.py (LLM-judge).

  ./score_exact_match.py results/            # every artifact carrying a `gold`
  ./score_exact_match.py results/foo.json
"""

import argparse
import glob
import json
import os
import re
import sys
import unicodedata


def _norm(s):
    s = unicodedata.normalize("NFKC", s or "").casefold()
    return re.sub(r"\s+", " ", s).strip()


def _score_one(art):
    gold = art.get("gold")
    if not gold:
        return None  # no gold → not an exact-match task; skip
    golds = gold if isinstance(gold, list) else [gold]
    hay = _norm(art.get("report_md", "")) + " " + " ".join(
        _norm(c.get("claim", "")) for c in art.get("claims", [])
    )
    hit = any(_norm(g) and _norm(g) in hay for g in golds)
    return {
        "task_id": art.get("task_id"),
        "hit": hit,
        "status": art.get("status"),
        "stop_reason": art.get("stop_reason"),
    }


def _artifacts(path):
    if os.path.isdir(path):
        return sorted(glob.glob(os.path.join(path, "*.json")))
    return [path]


def main():
    ap = argparse.ArgumentParser(description="Deterministic short-answer (exact-match) scorer.")
    ap.add_argument("path", help="artifact dir or a single artifact JSON")
    args = ap.parse_args()

    rows, scored, hits = [], 0, 0
    for f in _artifacts(args.path):
        try:
            art = json.load(open(f))
        except (OSError, json.JSONDecodeError) as e:
            print(f"skip {f}: {e}", file=sys.stderr)
            continue
        r = _score_one(art)
        if r is None:
            continue
        scored += 1
        hits += r["hit"]
        rows.append(r)

    for r in rows:
        print(f"  {str(r['task_id']):<28} {'HIT' if r['hit'] else 'miss':<5} "
              f"({r['status']}/{r['stop_reason']})")
    if scored:
        print(f"\nexact-match: {hits}/{scored} = {100 * hits / scored:.1f}%")
    else:
        print("No tasks with a `gold` field found — exact-match scoring needs one.")


if __name__ == "__main__":
    main()
