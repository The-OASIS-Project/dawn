# CLI: python3 -m llm_testing.memory_eval run|judge|report|label|validate
#
#   run       the memory eval cases on captured models (the quality suite's
#             captures, tool loop and mocks); writes a results JSON
#   judge     grade the ask and quiet runs with the quality suite's judge
#   report    pass counts per decision and per tag, and the verdicts that
#             changed against a previous results file
#   label     score a sample of judged replies yourself (for validate)
#   validate  Cohen's kappa of the judge against your labels, per criterion
#
# The results file has the quality suite's shape, so its judge, labeller and
# validator read it unchanged.
#
# License: GPLv3, same as DAWN.

import argparse
import hashlib
import json
import os
import sys
import time
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Dict, List, Optional

from ..quality import capture, loop, providers
from ..quality.__main__ import DEFAULT_SECRETS
from ..quality.cases import case_hash as quality_case_hash
from ..quality.mocks import World
from ..memory_audit.storage import private_open
from . import cases as cases_mod, rubric, score

RESULTS_VERSION = 1


def case_hash(mc: cases_mod.MemoryCase) -> str:
    """A fingerprint of what a case asks, shows and checks (focus injection is a
    run setting, recorded beside it)."""
    blob = json.dumps({"case": quality_case_hash(mc.case), "decision": mc.decision,
                       "because": mc.because, "forgotten": mc.forgotten, "tags": mc.tags},
                      sort_keys=True)
    return hashlib.sha256(blob.encode()).hexdigest()[:16]


def run_case(template, mc: cases_mod.MemoryCase, run: int, now, keys) -> dict:
    """One run of @p mc on @p template, as a results row."""
    world = World(now, mc.case.state, mc.case.mocks)
    traj = loop.run_case(template, mc.case.turns, world, now, keys, items=mc.items)
    s, res, why = score.score_case(mc, traj)
    return {
        "case": mc.id, "category": mc.decision, "run": run, "score": s, "checks": res,
        "why": why, "decision": mc.decision, "tags": mc.tags, "case_surface": mc.case_surface,
        "memory_context": mc.context(),
        "turns": [{"user": t.user, "answer": t.answer, "ttft_s": t.ttft_s, "total_s": t.total_s,
                   "requests": t.requests, "error": t.error,
                   "steps": [{"tool": st.name, "args": st.args, "result": st.result}
                             for st in t.steps]} for t in traj.turns],
        "usage": traj.usage, "unmocked": world.unmocked, "judge_criteria": mc.case.judge,
    }


def _run_one(template, mc, run, now, keys) -> dict:
    try:
        return run_case(template, mc, run, now, keys)
    except Exception as e:  # a harness bug in one case must not lose the run
        return {"case": mc.id, "category": mc.decision, "run": run, "score": 0.0,
                "checks": {"run": False}, "why": {"run": f"harness: {type(e).__name__}: {e}"},
                "decision": mc.decision, "tags": mc.tags, "case_surface": mc.case_surface,
                "memory_context": mc.context(), "turns": [], "usage": {}, "unmocked": [],
                "judge_criteria": mc.case.judge}


def cmd_run(args):
    now = capture.clock(args.clock, args.tz)
    templates = capture.first_turns(capture.load_capture_set(args.captures))
    want = set(args.model or [])
    keys = providers.load_keys(args.secrets)
    missing = sorted({providers.key_name(t) for t in templates
                      if (not want or t.model in want)
                      and providers.key_name(t) and not keys.get(providers.key_name(t))})
    if missing:  # every request would fail with 401 and be scored as the model's fault
        raise SystemExit(f"{args.secrets}: no {', '.join(missing)}; pass --secrets with the "
                         f"daemon's secrets.toml")
    focus = args.focus == "on"
    mcases = cases_mod.load(now, only=args.only, focus=focus)
    results = {"version": RESULTS_VERSION, "suite": "memory_eval", "label": args.label,
               "focus": focus, "rubric_version": rubric.RUBRIC_VERSION,
               "clock": now.isoformat(), "captures": os.path.abspath(args.captures),
               "runs": args.runs, "started_at": int(time.time()), "models": {},
               "case_hashes": {mc.id: case_hash(mc) for mc in mcases}}
    out = args.out or os.path.join(rubric.DATA_DIR, f"{args.label}.json")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)

    def save():
        # 0600: a results file holds the captured requests (the user's
        # system prompt and persona).
        tmp = out + ".tmp"
        with private_open(tmp) as f:
            json.dump(results, f, indent=1)
        os.replace(tmp, out)

    by_key = {}
    for tpl in templates:
        if not want or tpl.model in want:
            by_key.setdefault(f"{tpl.model}|{tpl.surface}", tpl)
    if not by_key:
        raise SystemExit("no captured model matches --model")
    for model in sorted({t.model for t in by_key.values()}):
        lacking = sorted({mc.case.surface for mc in mcases} -
                         {t.surface for t in by_key.values() if t.model == model})
        if lacking:
            print(f"warning: {model} has no {', '.join(lacking)} capture; its cases on that "
                  f"surface don't run", file=sys.stderr)
    for key, tpl in by_key.items():
        mine = [mc for mc in mcases if mc.case.surface == tpl.surface]
        if not mine:
            continue
        entry = results["models"][key] = {"model": tpl.model, "surface": tpl.surface,
                                          "provider": tpl.provider,
                                          "template": os.path.abspath(tpl.path), "rows": []}
        jobs = [(mc, r) for mc in mine for r in range(args.runs)]
        print(f"== {tpl.model} ({tpl.surface}): {len(mine)} cases x {args.runs} runs", flush=True)
        rows = [None] * len(jobs)
        with ThreadPoolExecutor(args.workers) as ex:
            futs = {ex.submit(_run_one, tpl, mc, r, now, keys): i
                    for i, (mc, r) in enumerate(jobs)}
            for done, f in enumerate(as_completed(futs), 1):
                row = rows[futs[f]] = f.result()
                fails = ",".join(k for k, v in row["checks"].items() if not v)
                print(f"  [{score.run_verdict(row):8s}] {row['case']} #{row['run']} "
                      + (f"{fails} " if fails else "") + f"({done}/{len(jobs)})", flush=True)
        entry["rows"].extend(rows)
        save()  # a later failure keeps the models already run
    save()
    print(f"Saved {out}")
    print(report(results, _load(args.previous) if args.previous else None,
                 args.previous or ""))


def _load(path: str) -> dict:
    with open(path) as f:
        return json.load(f)


def verdicts(results: dict) -> Dict[str, Dict[str, dict]]:
    """model -> case -> {verdict, decision, tags, why}: each case over its runs."""
    rows = defaultdict(lambda: defaultdict(list))
    for entry in results["models"].values():
        for r in entry["rows"]:
            rows[entry["model"]][r["case"]].append(r)
    out = {}
    for model, by_case in rows.items():
        out[model] = {}
        for cid, rs in by_case.items():
            why = next((f"{k}: {v}" for r in rs if score.run_verdict(r) == "fail"
                        for k, v in r["why"].items()), "")
            if not why and any(score.run_verdict(r) == "fail" for r in rs):
                low = next(r for r in rs if score.run_verdict(r) == "fail")
                why = "; ".join(f"{c} {v['score']}: {v.get('reason', '')}"
                                for c, v in (low.get("judge") or {}).items() if v["score"] < 2)
            out[model][cid] = {"verdict": score.case_verdict(rs), "decision": rs[0]["decision"],
                               "tags": rs[0]["tags"], "why": why}
    return out


def _bucket(verdict: str) -> str:
    return "mixed" if verdict.startswith("mixed") else verdict


def _table(title: str, groups: Dict[str, List[str]]) -> List[str]:
    cols = ("pass", "fail", "mixed", "unjudged", "error")
    lines = [f"  {title:24s}" + "".join(f"{c:>9s}" for c in cols)]
    for name, vs in groups.items():
        counts = defaultdict(int)
        for v in vs:
            counts[_bucket(v)] += 1
        lines.append(f"  {name + f' ({len(vs)})':24s}" + "".join(f"{counts[c]:9d}" for c in cols))
    return lines


def report(results: dict, previous: Optional[dict] = None, previous_name: str = "") -> str:
    """Counts per decision and per tag for each model, the cases that didn't
    pass, and (with @p previous) the verdicts that changed."""
    head = (f"Memory eval '{results.get('label', '')}': clock {results['clock']}, focus "
            f"{'on' if results.get('focus') else 'off'}, {results['runs']} run(s) per case. "
            f"A case passes when all its runs pass; ask and quiet need the judge "
            f"(valid once `validate` shows kappa >= 0.6).")
    lines = [head]
    now = verdicts(results)
    before = verdicts(previous) if previous else {}
    for model, by_case in sorted(now.items()):
        lines += ["", f"== {model}: {len(by_case)} cases"]
        lines += _table("decision", {d: [v["verdict"] for v in by_case.values()
                                         if v["decision"] == d]
                                     for d in cases_mod.DECISIONS})
        lines += _table("tag", {t: [v["verdict"] for v in by_case.values() if t in v["tags"]]
                                for t in cases_mod.TAGS})
        bad = [(cid, v) for cid, v in sorted(by_case.items()) if v["verdict"] != "pass"]
        if bad:
            lines.append("  not passing:")
            lines += [f"    {cid:14s} {v['verdict']:10s} {v['why'][:110]}" for cid, v in bad]
        if previous is None:
            continue
        prev = before.get(model)
        if prev is None and len(before) == 1 and len(now) == 1:
            prev = next(iter(before.values()))  # one model each: compare them
        name = f"{previous_name} ('{previous.get('label', '')}')"
        if prev is None:
            lines.append(f"  {model} is not in {name}")
            continue
        hashes, old_hashes = results.get("case_hashes", {}), previous.get("case_hashes", {})
        changed = [f"    {cid:14s} {prev[cid]['verdict']} -> {v['verdict']}"
                   + ("  (the case changed between the runs)"
                      if hashes.get(cid) != old_hashes.get(cid) else "")
                   for cid, v in sorted(by_case.items())
                   if cid in prev and prev[cid]["verdict"] != v["verdict"]]
        only = sorted(set(by_case) ^ set(prev))
        lines.append(f"  changed verdicts against {name}: {len(changed)}"
                     + (f"; run in only one file: {', '.join(only)}" if only else ""))
        lines += changed
        if previous.get("focus") != results.get("focus"):
            lines.append(f"  focus injection differs: {'on' if previous.get('focus') else 'off'}"
                         f" -> {'on' if results.get('focus') else 'off'}")
    return "\n".join(lines)


def cmd_report(args):
    print(report(_load(args.results), _load(args.previous) if args.previous else None,
                 args.previous or ""))


def cmd_judge(args):
    from ..quality.judge import judge_results
    judge_results(args.results, args.judge_model, keys=providers.load_keys(args.secrets),
                  out=args.out, row_criteria=rubric.ROW_CRITERIA)


def cmd_label(args):
    from ..quality.judge import label
    os.makedirs(os.path.dirname(os.path.abspath(args.labels)), exist_ok=True)
    label(args.results, args.labels, per_criterion=args.per_criterion,
          row_criteria=rubric.ROW_CRITERIA)


def cmd_validate(args):
    from ..quality.judge import validate
    print(validate(args.labels))


def main(argv=None):
    p = argparse.ArgumentParser(prog="python3 -m llm_testing.memory_eval",
                                description="DAWN memory eval: the decisions made with memory")
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="run the cases against captured models")
    r.add_argument("--captures", required=True, help="capture directory (with manifest.jsonl)")
    r.add_argument("--model", action="append", help="only these models (repeatable)")
    r.add_argument("--label", required=True,
                   help="a name for this run (a build, a memory configuration); names the "
                        "default output file")
    r.add_argument("--out", help="results file (default llm_testing/memory_eval/data/<label>.json)")
    r.add_argument("--previous", help="a results file to report changed verdicts against")
    r.add_argument("--focus", choices=("on", "off"), default="on",
                   help="send the facts as the first turn's retrieved items too, as DAWN does "
                        "with [memory.focus_injection] on (its default)")
    r.add_argument("--runs", type=int, default=3)
    r.add_argument("--only", default="", help="case ids, decisions or tags (comma list)")
    r.add_argument("--clock", default="2026-10-13T09:30:00", help="frozen local time")
    r.add_argument("--tz", default="America/Los_Angeles", help="the fixture user's timezone")
    r.add_argument("--workers", type=int, default=6)
    r.add_argument("--secrets", default=DEFAULT_SECRETS)
    r.set_defaults(fn=cmd_run)
    rp = sub.add_parser("report", help="counts per decision and tag, changed verdicts")
    rp.add_argument("results")
    rp.add_argument("--previous", help="a results file to report changed verdicts against")
    rp.set_defaults(fn=cmd_report)
    j = sub.add_parser("judge", help="grade the ask and quiet runs with the LLM judge")
    j.add_argument("results")
    j.add_argument("--judge-model", default="google/gemini-3.1-pro-preview",
                   help="a gemini-* id (Google's API) or an OpenRouter slug")
    j.add_argument("--secrets", default=DEFAULT_SECRETS)
    j.add_argument("--out")
    j.set_defaults(fn=cmd_judge)
    lb = sub.add_parser("label", help="score a stratified sample of judged replies yourself")
    lb.add_argument("results")
    lb.add_argument("--labels", default=rubric.LABELS_PATH)
    lb.add_argument("--per-criterion", type=int, default=20)
    lb.set_defaults(fn=cmd_label)
    v = sub.add_parser("validate", help="Cohen's kappa of the judge against your labels")
    v.add_argument("--labels", default=rubric.LABELS_PATH)
    v.set_defaults(fn=cmd_validate)
    args = p.parse_args(argv)
    args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
