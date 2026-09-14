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
###############################################################################
# classify_strncpy.py - inventory raw strncpy()/strncat() call sites
#
# Standardizing the codebase on safe_strncpy() (see
# docs/SAFE_STRNCPY_STANDARDIZATION_DESIGN.md) requires knowing which of the
# hundreds of raw strncpy() sites are mechanically convertible and which need a
# hand review.  The signal is the 3rd (bound) argument:
#
#   sizeof(dst)-1        -> convert: safe_strncpy(dst, src, sizeof(dst))
#   MACRO-1 / NAME-1     -> convert with sizeof(dst) (NOT the macro: many buffers
#                           are [M+1], so MACRO != sizeof(dst))
#   var-1  (capacity)    -> convert: safe_strncpy(dst, src, var)   [pointer dst]
#   sizeof(dst) (no -1)  -> REVIEW: bound == capacity, may not null-terminate
#   MACRO / literal      -> REVIEW: confirm capacity relationship
#   strlen()/len/len+1   -> DO NOT auto-convert: substring/prefix copy, the 3rd
#                           arg is a copy length, not the buffer capacity
#
# A line grep undercounts because clang-format wraps calls across lines, so this
# joins tokens until parentheses balance.
#
# Usage:   scripts/classify_strncpy.py [ROOT ...]        (default: src)
#          scripts/classify_strncpy.py src tests dawn-admin
# Output:  per-category counts, a per-directory breakdown, and the full
#          review-list (every site NOT in the provably-mechanical bucket).
# Exit:    0 always (inventory tool, not a gate; the CI gate is
#          check_no_raw_strncpy.sh).
###############################################################################
import collections
import os
import re
import sys

ROOTS = sys.argv[1:] or ["src"]
CALL = re.compile(r"(?<![A-Za-z0-9_])(strncpy|strncat)\s*\(")
MECHANICAL = "sizeof-1-same"  # the one bucket that needs no human review


def split_args(body):
    args, depth, cur = [], 0, []
    for ch in body:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            args.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if cur:
        args.append("".join(cur).strip())
    return args


def classify(third, dst):
    t = re.sub(r"\s+", "", third)
    d = re.sub(r"\s+", "", dst)
    m = re.fullmatch(r"sizeof\((.+)\)-1", t)
    if m:
        return ("sizeof-1-same" if m.group(1) == d else "sizeof-1-diff", t)
    if re.fullmatch(r"sizeof\((.+)\)", t):
        return ("sizeof", t)
    if re.fullmatch(r"sizeof\((.+)\)-\d+", t):
        return ("sizeof-N", t)
    if re.fullmatch(r"[A-Z][A-Z0-9_]*-1", t):
        return ("MACRO-1", t)
    if re.fullmatch(r"[A-Z][A-Z0-9_]*", t):
        return ("MACRO", t)
    if re.fullmatch(r"\d+", t):
        return ("literal", t)
    if re.fullmatch(r"[a-z_][a-z0-9_]*-1", t):
        return ("var-1", t)
    if "strlen" in t or re.search(r"\blen\b|_len\b|length", t) or "+1" in t:
        return ("len/var", t)
    return ("other", t)


def scan(roots):
    rows = []
    for root in roots:
        for dp, _, fns in os.walk(root):
            for fn in fns:
                if not fn.endswith((".c", ".h", ".cpp")):
                    continue
                path = os.path.join(dp, fn)
                try:
                    text = open(path, errors="replace").read()
                except OSError:
                    continue
                for m in CALL.finditer(text):
                    start, depth, i = m.end(), 1, m.end()
                    while i < len(text) and depth > 0:
                        if text[i] == "(":
                            depth += 1
                        elif text[i] == ")":
                            depth -= 1
                        i += 1
                    body = text[start : i - 1]
                    line = text.count("\n", 0, m.start()) + 1
                    args = split_args(body)
                    if len(args) != 3:
                        rows.append((path, line, "ARGC!=3", args[0] if args else "", body[:60]))
                        continue
                    cat, detail = classify(args[2], args[0])
                    rows.append((path, line, cat, args[0][:40], detail))
    return rows


def main():
    rows = scan(ROOTS)
    print(f"TOTAL raw strncpy/strncat sites: {len(rows)}\n")
    print("By category:")
    for cat, n in collections.Counter(r[2] for r in rows).most_common():
        tag = "  (mechanical)" if cat == MECHANICAL else ""
        print(f"  {n:5d}  {cat}{tag}")
    print("\nBy directory:")
    for d, n in sorted(collections.Counter(os.path.dirname(r[0]) for r in rows).items()):
        print(f"  {n:5d}  {d}")
    print("\nReview list (every site NOT provably-mechanical):")
    for p, ln, cat, dst, detail in rows:
        if cat != MECHANICAL:
            print(f"  {p}:{ln}  [{cat}] dst={dst!r} bound={detail!r}")


if __name__ == "__main__":
    main()
