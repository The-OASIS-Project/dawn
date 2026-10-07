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
# By contributing to this project, you agree to license your contributions
# under the GPLv3 (or any later version) or any future licenses chosen by
# the project author(s).
#
# Run clang-tidy over DAWN's own sources (src/ and common/src/) in a build's
# compile database, in parallel.  The checks are in .clang-tidy, which makes
# every finding an error, so any finding fails the run.
#
#   pip install --require-hashes -r .github/clang-tidy-requirements.txt
#   cmake --preset debug && make -C build-debug models_toml_builtin
#   scripts/run_clang_tidy.py build-debug
#
# The version is pinned so local runs and CI report the same findings.

import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import tempfile

PINNED = "20.1.0"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OWN = (os.path.join(ROOT, "src") + os.sep, os.path.join(ROOT, "common", "src") + os.sep)
FINDING = re.compile(r": (error|warning): .*\[[\w.,-]+\]$", re.M)


def own_database(build, outdir):
    """Write a compile database with one entry per DAWN source into outdir.

    A test build compiles many sources into several test programs, and
    clang-tidy analyzes a file once per entry; the daemon's own entry is kept.
    """
    path = os.path.join(build, "compile_commands.json")
    if not os.path.isfile(path):
        sys.exit(f"{path} not found: configure the build first")
    with open(path) as f:
        entries = json.load(f)
    chosen = {}
    for e in entries:
        p = os.path.normpath(os.path.join(e.get("directory", ""), e["file"]))
        if not p.startswith(OWN):
            continue
        cmd = e.get("command") or " ".join(e.get("arguments", []))
        if p not in chosen or "CMakeFiles/dawn.dir/" in cmd:
            chosen[p] = e
    with open(os.path.join(outdir, "compile_commands.json"), "w") as f:
        json.dump(list(chosen.values()), f)
    return sorted(chosen)


def run_one(tidy, build, path):
    r = subprocess.run([tidy, "-p", build, "--quiet", path], capture_output=True, text=True)
    out = r.stdout + r.stderr
    return path, len(FINDING.findall(out)), r.returncode, out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("build", help="build directory holding compile_commands.json")
    ap.add_argument("--clang-tidy", default="clang-tidy", help="clang-tidy binary")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 1, help="parallel jobs")
    args = ap.parse_args()

    ver = subprocess.run([args.clang_tidy, "--version"], capture_output=True, text=True).stdout
    if PINNED not in ver:
        print(f"warning: not clang-tidy {PINNED}; findings may differ from CI", file=sys.stderr)

    findings = failed = 0
    with tempfile.TemporaryDirectory() as db:
        files = own_database(os.path.abspath(args.build), db)
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.j) as pool:
            for path, n, rc, out in pool.map(lambda p: run_one(args.clang_tidy, db, p), files):
                if n or rc:
                    print(out.rstrip(), flush=True)
                    findings += n
                    failed += 1
    print(f"clang-tidy: {len(files)} files, {findings} findings in {failed} files")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
