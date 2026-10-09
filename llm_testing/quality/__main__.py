# CLI: python3 -m llm_testing.quality run|compare ...
#
#   run      every case R times for each captured model; writes a results JSON
#   compare  paired comparison of two models from results files
#   judge    (P3) score the voice rubric on a results file
#
# License: GPLv3, same as DAWN.

import argparse
import json
import os
import sys
import time
from concurrent.futures import ThreadPoolExecutor

from . import capture, cases as cases_mod, loop, providers, score

from .judge import RUBRIC_VERSION

RESULTS_VERSION = 1
DEFAULT_SECRETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                               "secrets.toml")


def _run_one(template, case, run, now, keys):
    try:
        return _run_case(template, case, run, now, keys)
    except Exception as e:  # a harness bug in one case must not lose the run
        return {"case": case.id, "category": case.category, "run": run, "score": 0.0,
                "checks": {"run": False}, "why": {"run": f"harness: {type(e).__name__}: {e}"},
                "turns": [], "usage": {}, "unmocked": [], "judge_criteria": case.judge}


def _run_case(template, case, run, now, keys):
    from .mocks import World
    world = World(now, case.state, case.mocks)
    traj = loop.run_case(template, case.turns, world, now, keys)
    s, res, why = score.score_case(case, traj)
    return {
        "case": case.id, "category": case.category, "run": run, "score": s,
        "checks": res, "why": why,
        "turns": [{"user": t.user, "answer": t.answer, "ttft_s": t.ttft_s, "total_s": t.total_s,
                   "requests": t.requests, "error": t.error,
                   "steps": [{"tool": st.name, "args": st.args, "result": st.result}
                             for st in t.steps]} for t in traj.turns],
        "usage": traj.usage, "unmocked": world.unmocked, "judge_criteria": case.judge,
    }


def cmd_run(args):
    now = capture.clock(args.clock, args.tz)
    templates = capture.first_turns(capture.load_capture_set(args.captures))
    patch = json.load(open(args.system_patch)) if args.system_patch else []
    patched = set()
    for tpl in templates:
        capture.patch_system(tpl, patch, patched)
    want = set(args.model or [])
    keys = providers.load_keys(args.secrets)
    missing = sorted({providers.key_name(t) for t in templates
                      if providers.key_name(t) and not keys.get(providers.key_name(t))})
    if missing:  # every request would fail with 401 and be scored as the model's fault
        raise SystemExit(f"{args.secrets}: no {', '.join(missing)}; pass --secrets with the "
                         f"daemon's secrets.toml")
    case_list = cases_mod.load_cases(now, only=args.only)
    from .report import _slug, prices
    results = {"version": RESULTS_VERSION, "rubric_version": RUBRIC_VERSION,
               "clock": now.isoformat(), "captures": os.path.abspath(args.captures),
               "runs": args.runs, "started_at": int(time.time()), "models": {},
               "system_patch": patch,
               # A case is the same case across files only if its definition is.
               "case_hashes": {c.id: cases_mod.case_hash(c) for c in case_list}}
    out = args.out or f"quality_{int(time.time())}.json"

    def save():
        tmp = out + ".tmp"
        with open(tmp, "w") as f:
            json.dump(results, f, indent=1)
        os.replace(tmp, out)
    # One template per model and surface (its first captured conversation); a
    # case runs on its own surface's.
    by_key = {}
    for tpl in templates:
        if not want or tpl.model in want:
            by_key.setdefault(f"{tpl.model}|{tpl.surface}", tpl)
    for key, tpl in by_key.items():
        if tpl.followup is None:
            print(f"warning: no 2-turn capture for {key}; later turns reuse the first turn's "
                  f"shape (re-capture with scripts/quality_capture_all.sh)", file=sys.stderr)
        results["models"][key] = {"model": tpl.model, "surface": tpl.surface,
                                  "provider": tpl.provider,
                                  "template": os.path.abspath(tpl.path), "rows": []}
    results["prices"] = {e["model"]: prices().get(_slug(e["model"]))
                         for e in results["models"].values()}  # the prices this run used
    for key, entry in results["models"].items():
        tpl = by_key[key]
        mine = [c for c in case_list if c.surface == tpl.surface]
        jobs = [(c, r) for c in mine for r in range(args.runs)]
        print(f"== {tpl.model} ({tpl.surface}): {len(mine)} cases x {args.runs} runs", flush=True)
        with ThreadPoolExecutor(args.workers) as ex:
            futs = [ex.submit(_run_one, tpl, c, r, now, keys) for c, r in jobs]
            for f in futs:
                row = f.result()
                entry["rows"].append(row)
                mark = "ok " if row["score"] == 1 else f"{row['score']:.2f}"
                fails = ",".join(k for k, v in row["checks"].items() if not v)
                print(f"  [{mark}] {row['case']} #{row['run']} {fails}", flush=True)
        save()  # a later failure keeps the models already run
    save()
    print(f"Saved {out}")
    from .report import summarize
    print(summarize(results))


def cmd_compare(args):
    from .report import compare
    print(compare(args.results, args.a, args.b, seed=args.seed))


def cmd_label(args):
    from .judge import label
    label(args.results, args.labels, per_criterion=args.per_criterion)


def cmd_validate(args):
    from .judge import validate
    print(validate(args.labels))


def cmd_judge(args):
    from .judge import judge_results
    judge_results(args.results, args.judge_model, keys=providers.load_keys(args.secrets),
                  out=args.out)


def main():
    p = argparse.ArgumentParser(prog="python3 -m llm_testing.quality",
                                description="DAWN LLM quality suite v2")
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="run the cases against captured models")
    r.add_argument("--captures", required=True, help="capture directory (with manifest.jsonl)")
    r.add_argument("--model", action="append", help="only these models (repeatable)")
    r.add_argument("--runs", type=int, default=3)
    r.add_argument("--only", default="", help="case ids/categories (comma list)")
    r.add_argument("--clock", default="2026-10-13T09:30:00", help="frozen local time")
    r.add_argument("--tz", default="America/Los_Angeles", help="the fixture user's timezone")
    r.add_argument("--workers", type=int, default=6)
    r.add_argument("--secrets", default=DEFAULT_SECRETS)
    r.add_argument("--out")
    r.add_argument("--system-patch",
                   help="JSON [[old, new], ...]: replace text in each captured system prompt "
                        "(an A/B of prompt wording on the same captures)")
    r.set_defaults(fn=cmd_run)
    c = sub.add_parser("compare", help="paired comparison of two models")
    c.add_argument("results", nargs="+", help="results JSON file(s)")
    c.add_argument("--a", required=True,
                   help="model; N:model picks it from the Nth results file (0-based)")
    c.add_argument("--b", required=True)
    c.add_argument("--seed", type=int, default=7)
    c.set_defaults(fn=cmd_compare)
    j = sub.add_parser("judge", help="score the voice rubric with an LLM judge")
    j.add_argument("results")
    j.add_argument("--judge-model", default="google/gemini-3.1-pro-preview",
                   help="a gemini-* id (Google's API) or an OpenRouter slug")
    j.add_argument("--secrets", default=DEFAULT_SECRETS)
    j.add_argument("--out")
    j.set_defaults(fn=cmd_judge)
    lb = sub.add_parser("label", help="score a stratified sample of judged replies yourself")
    lb.add_argument("results")
    lb.add_argument("--labels", default=f"llm_testing/quality/labels_v{RUBRIC_VERSION}.json")
    lb.add_argument("--per-criterion", type=int, default=20)
    lb.set_defaults(fn=cmd_label)
    v = sub.add_parser("validate", help="Cohen's kappa of the judge against your labels")
    v.add_argument("--labels", default=f"llm_testing/quality/labels_v{RUBRIC_VERSION}.json")
    v.set_defaults(fn=cmd_validate)
    args = p.parse_args()
    args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
