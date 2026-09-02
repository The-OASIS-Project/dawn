#!/usr/bin/env python3
# This program is free software: you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version.  Distributed WITHOUT ANY WARRANTY; see <https://www.gnu.org/licenses/>.
"""
Find candidate TRANSIENT DEVICE/SYSTEM-STATE facts (READ-ONLY report).

Background: before the extraction-prompt transient-state rule shipped (Memory
Citation Phase 2), point-in-time device/system readings — battery %, on/off/locked
status, live light counts, "current time" — were sometimes extracted as durable
memory_facts.  They read like facts but are stale the moment they're stored, and
get recalled-and-cited instead of triggering a live tool call (the 2026-09
luna/haiku recall probe found both models citing a stale front-door-lock fact).

This is a REPORTING helper only — it never mutates the database.  The regex is a
broad *finder*, NOT an executioner: it over-matches on purpose (any "N%", any
"currently ...") so nothing is missed, which means the output MUST be reviewed by
a human/agent before anything is removed.  Known over-match classes it will flag
that are usually NOT transient state: design specs (opacity %), market-share data,
comprehensive device *inventories* (durable structure welded to live values —
re-extract, don't delete), and interaction/progress events (a different junk
class).  The [DURABLE-SHAPE? review] tag hints at some of these.

Usage:
    python3 scripts/find_transient_state_facts.py            # report, user 1
    python3 scripts/find_transient_state_facts.py --user 1 --ids   # bare id list too
    python3 scripts/find_transient_state_facts.py --db /path/to/auth.db
"""

import argparse
import re
import sqlite3
import sys
from datetime import datetime

DEFAULT_DB = "/var/lib/dawn/auth.db"

# Each pattern is (label, compiled regex).  A fact matching ANY pattern is a
# candidate.  Deliberately broad; the human review is the authority.
SIGNALS = [
    # Require a level/status context so durable "battery charger project" facts
    # don't match — only live battery *readings* ("98% battery", "battery: 98%").
    ("battery-level", re.compile(r"\d{1,3}\s*%\s*battery|battery[\s:)]+\d{1,3}\s*%|"
                                 r"battery\s*(?:status|level)\b", re.I)),
    ("locked-state", re.compile(r"\((?:un)?locked", re.I)),
    ("on/off-state", re.compile(r"\((?:on|off)(?:\s+at\b|\)|,)", re.I)),
    # Word-boundary after the state word so "currently on" does NOT match inside
    # "currently only ..." (that bug caught durable preference/finding facts).
    ("currently-on/off", re.compile(r"currently\s+(?:on|off|locked|unlocked)\b", re.I)),
    ("live-light-count", re.compile(r"\d+\s+lights?\s+(?:currently\s+)?on\b|lights?\s+on:", re.I)),
    # Only "on at N%" / "N% brightness" — NOT a bare "at N%", which matched opacity,
    # memory-load %, market-share %, etc.
    ("brightness-level", re.compile(r"on\s+at\s+\d{1,3}\s*%|\d{1,3}\s*%\s*brightness|"
                                    r"brightness[\s:)]+\d{1,3}\s*%", re.I)),
    ("current-time", re.compile(r"current\s+time\b|the\s+time\s+is\b", re.I)),
    ("current-weather", re.compile(r"current\s+weather\b|currently\s+\d+\s*°", re.I)),
]

# Durable-shape phrases that argue AGAINST removal (usage patterns / existence /
# comprehensive inventory, not a live reading).  A hint for the reviewer only.
DURABLE_HINT = re.compile(
    r"controls?\b.*\bvia\b|has\s+a\s+.*\block\b|prefers?\b|wants?\b|by\s+voice\b|"
    r"systematically|stress-test|configured\s+to|entities\b|is\s+equipped\s+with|"
    r"monitoring\b|placeholder|market\s+share|opacity",
    re.I,
)


def classify(text):
    return [name for name, rx in SIGNALS if rx.search(text)]


def main():
    ap = argparse.ArgumentParser(description="Report candidate transient device-state facts (read-only).")
    ap.add_argument("--db", default=DEFAULT_DB, help=f"auth.db path (default {DEFAULT_DB})")
    ap.add_argument("--user", type=int, default=1, help="user_id (default 1)")
    ap.add_argument("--ids", action="store_true", help="also print a bare comma-separated id list")
    args = ap.parse_args()

    try:
        db = sqlite3.connect(f"file:{args.db}?mode=ro", uri=True)
    except sqlite3.Error as e:
        sys.exit(f"cannot open {args.db}: {e}")

    rows = db.execute(
        "SELECT id, category, created_at, confidence, fact_text FROM memory_facts "
        "WHERE user_id = ? AND superseded_by IS NULL ORDER BY created_at",
        (args.user,),
    ).fetchall()

    hits = []
    for fid, cat, created, conf, text in rows:
        labels = classify(text or "")
        if labels:
            hits.append((fid, cat, created, conf, labels, text or ""))

    print("=" * 78)
    print(f" Transient device/system-state fact candidates — user {args.user}  (READ-ONLY)")
    print(f" {len(hits)} candidate(s) of {len(rows)} live facts.  REVIEW before removing anything.")
    print("=" * 78)
    for fid, cat, created, conf, labels, text in hits:
        when = datetime.fromtimestamp(created).strftime("%Y-%m-%d") if created else "?"
        durable = "  [DURABLE-SHAPE? review]" if DURABLE_HINT.search(text) else ""
        snippet = text[:88].replace("\n", " ")
        print(f" #{fid:<6} {when} {cat:<12} conf={conf:.2f} [{','.join(labels)}]{durable}")
        print(f"        {snippet}")

    if not hits:
        print(" (none — clean)")
    elif args.ids:
        print("\n ids:", ",".join(str(h[0]) for h in hits))


if __name__ == "__main__":
    main()
