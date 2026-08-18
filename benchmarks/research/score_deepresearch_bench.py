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
"""RACE/FACT adapter for DeepResearch-Bench-style scoring (DEEP_RESEARCH_DESIGN.md §16.5).

DeepResearch Bench grades a *report* with two LLM-judge frameworks:
  RACE — Reference-based Adaptive Criteria (comprehensiveness / depth / instruction-
         following / readability, scored against a reference report), and
  FACT — citation accuracy (does each statement's cited URL actually support it).

DAWN's ledger already yields FACT's raw material directly: every claim carries its
source_url AND a verbatim supporting quote, so citation verification is a
claim<->quote<->url check, not a re-extraction pass.

This tool does the parts that need NO judge/API key, and prepares the input for
the part that does:

  1. Deterministic citation + coverage stats per artifact (the structural health
     FACT builds on): claims, cited ratio, distinct sources, quote coverage,
     answered/total, report size.
  2. Writes a per-artifact judge-input file (report + reference + claim/url/quote
     triples) ready for an external RACE/FACT LLM judge.

It does NOT fabricate a quality score without a judge. The LLM-judge step needs a
judge model + key and the DeepResearch Bench prompts
(https://github.com/Ayanami0730/deep_research_bench); point it at the judge-input
files written here — see the printed next-steps.

For the OFFICIAL DeepResearch-Bench harness, --dr-bench does the two format
conversions its runner needs, so you never hand-write the glue:
  * --query <query.jsonl>  : their data/prompt_data/query.jsonl -> our task set
                             ({queries:[{id,brief}]}) to feed run_benchmark.py.
  * --dr-bench <model>     : our artifacts -> their data/test_data/raw_data/<model>.jsonl
                             ({id, prompt, article}); drop into the bench repo and
                             run run_benchmark.sh with TARGET_MODELS=(<model>).

  # 1. convert their tasks, run them through DAWN
  ./score_deepresearch_bench.py --query deep_research_bench/.../query.jsonl --tasks-out dr_tasks.json
  ./run_benchmark.py --tasks dr_tasks.json --user 2 --out results/dr
  # 2. emit the bench's raw_data line file for RACE/FACT
  ./score_deepresearch_bench.py results/dr --dr-bench dawn-sonnet-5 --raw-out raw_data

  # local stats / judge-input prep (no bench repo needed)
  ./score_deepresearch_bench.py results/ --judge-out results/judge_inputs
  ./score_deepresearch_bench.py results/one.json
"""

import argparse
import glob
import json
import os
import sys


def _stats(art):
    claims = art.get("claims", [])
    n = len(claims)
    cited = sum(1 for c in claims if c.get("source_url"))
    quoted = sum(1 for c in claims if c.get("quote"))
    distinct = len({c["source_url"] for c in claims if c.get("source_url")})
    cov = art.get("coverage", {})
    total_q = sum(cov.get(k, 0) for k in ("open", "answered", "unanswerable"))
    return {
        "task_id": art.get("task_id"),
        "status": art.get("status"),
        "stop_reason": art.get("stop_reason"),
        "n_claims": n,
        "cited_ratio": (cited / n) if n else 0.0,
        "quote_ratio": (quoted / n) if n else 0.0,
        "distinct_sources": distinct,
        "answered": cov.get("answered", 0),
        "total_questions": total_q,
        "report_chars": len(art.get("report_md", "")),
        "has_reference": bool(art.get("reference")),
    }


def _judge_input(art):
    """Everything a RACE/FACT judge needs — report + reference + FACT citation triples."""
    return {
        "task_id": art.get("task_id"),
        "brief": art.get("brief"),
        "report_md": art.get("report_md", ""),
        "reference": art.get("reference"),          # RACE: None → report-only grading
        "should_cover": art.get("should_cover"),    # rubric hints, if the task set has them
        "citations": [                              # FACT: statement <-> supporting quote <-> url
            {"claim": c.get("claim"), "source_url": c.get("source_url"), "quote": c.get("quote")}
            for c in art.get("claims", [])
            if c.get("source_url")
        ],
    }


def _artifacts(path):
    if os.path.isdir(path):
        return sorted(glob.glob(os.path.join(path, "*.json")))
    return [path]


def _query_to_tasks(query_jsonl):
    """DeepResearch-Bench data/prompt_data/query.jsonl -> our task list.  Each line
    is {"id":..., "prompt":...}; we map prompt->brief and PRESERVE the id verbatim
    (RACE aligns raw_data ids to reference ids, so the type must match theirs)."""
    tasks = []
    with open(query_jsonl, encoding="utf-8") as f:
        for ln, raw in enumerate(f, 1):
            raw = raw.strip()
            if not raw:
                continue
            try:
                o = json.loads(raw)
            except json.JSONDecodeError as e:
                print(f"skip {query_jsonl}:{ln}: {e}", file=sys.stderr)
                continue
            tid = o.get("id")
            brief = o.get("prompt") or o.get("brief") or o.get("query")
            if tid is None or not brief:
                print(f"skip {query_jsonl}:{ln}: needs id + prompt", file=sys.stderr)
                continue
            t = {"id": tid, "brief": brief}
            if "language" in o:
                t["language"] = o["language"]
            tasks.append(t)
    return tasks


def _write_raw_data(arts, model, raw_out):
    """Our artifacts -> DeepResearch-Bench data/test_data/raw_data/<model>.jsonl,
    one line per report: {"id", "prompt": brief, "article": report_md}.  Skips an
    artifact with an empty report (a failed run contributes no gradable article)."""
    os.makedirs(raw_out, exist_ok=True)
    outpath = os.path.join(raw_out, f"{model}.jsonl")
    n, skipped = 0, 0
    with open(outpath, "w", encoding="utf-8") as out:
        for art in arts:
            report = art.get("report_md", "")
            if not report:
                skipped += 1
                continue
            rec = {"id": art.get("task_id"), "prompt": art.get("brief"), "article": report}
            out.write(json.dumps(rec, ensure_ascii=False) + "\n")
            n += 1
    return outpath, n, skipped


def main():
    ap = argparse.ArgumentParser(description="DeepResearch-Bench RACE/FACT adapter (prep + stats).")
    ap.add_argument("path", nargs="?", help="artifact dir or a single artifact JSON")
    ap.add_argument("--judge-out", help="dir to write per-artifact judge-input JSON files")
    ap.add_argument("--query", help="DeepResearch-Bench query.jsonl to convert to our task set")
    ap.add_argument("--tasks-out", help="where --query writes the task set (default: stdout)")
    ap.add_argument("--dr-bench", metavar="MODEL",
                    help="emit artifacts as the bench's raw_data/<MODEL>.jsonl")
    ap.add_argument("--raw-out", default="raw_data", help="output dir for --dr-bench (default raw_data)")
    args = ap.parse_args()

    # --query: convert their tasks and exit (the "before running" prep step).
    if args.query:
        tasks = _query_to_tasks(args.query)
        blob = {"queries": tasks}
        if args.tasks_out:
            with open(args.tasks_out, "w", encoding="utf-8") as f:
                json.dump(blob, f, indent=2)
            print(f"{len(tasks)} task(s) -> {args.tasks_out}")
        else:
            print(json.dumps(blob, indent=2))
        return

    if not args.path:
        ap.error("path (artifact dir/JSON) is required unless using --query")

    if args.judge_out:
        os.makedirs(args.judge_out, exist_ok=True)

    rows = []
    arts = []
    for f in _artifacts(args.path):
        try:
            art = json.load(open(f))
        except (OSError, json.JSONDecodeError) as e:
            print(f"skip {f}: {e}", file=sys.stderr)
            continue
        if "report_md" not in art:
            continue
        arts.append(art)
        rows.append(_stats(art))
        if args.judge_out:
            base = os.path.splitext(os.path.basename(f))[0]
            with open(os.path.join(args.judge_out, f"{base}.judge.json"), "w") as jf:
                json.dump(_judge_input(art), jf, indent=2)

    if not rows:
        print("No artifacts with a report found.")
        return

    if args.dr_bench:
        outpath, n_written, skipped = _write_raw_data(arts, args.dr_bench, args.raw_out)
        note = f" ({skipped} empty-report artifact(s) skipped)" if skipped else ""
        print(f"raw_data: {n_written} report(s) -> {outpath}{note}\n"
              f"  drop into deep_research_bench/data/test_data/raw_data/ and run their\n"
              f"  run_benchmark.sh with TARGET_MODELS=(\"{args.dr_bench}\").\n")

    print(f"{'task':<26} {'status':<16} {'claims':>6} {'cited%':>7} {'quote%':>7} "
          f"{'srcs':>5} {'ans/tot':>8} {'chars':>7}")
    print("-" * 92)
    for r in rows:
        print(f"{str(r['task_id']):<26} {r['status'] + '/' + str(r['stop_reason']):<16} "
              f"{r['n_claims']:>6} {100 * r['cited_ratio']:>6.0f}% {100 * r['quote_ratio']:>6.0f}% "
              f"{r['distinct_sources']:>5} {str(r['answered']) + '/' + str(r['total_questions']):>8} "
              f"{r['report_chars']:>7}")

    n = len(rows)
    print(f"\n{n} report(s). Deterministic citation/coverage stats above (no judge needed).")
    if args.judge_out:
        print(f"Judge inputs written to {args.judge_out}/ — {sum(r['has_reference'] for r in rows)}"
              f"/{n} carry a reference (RACE grades report-only without one).")
    print(
        "\nNext (LLM-judge, needs a key): run the DeepResearch Bench RACE + FACT judges over the\n"
        "judge-input files — https://github.com/Ayanami0730/deep_research_bench. FACT can verify\n"
        "each citation from the claim<->quote<->url triple this tool already extracted, rather\n"
        "than re-parsing the prose."
    )


if __name__ == "__main__":
    main()
