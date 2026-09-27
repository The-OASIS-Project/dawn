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
"""Grade a run_ab.py run offline, and compare two graded runs.

Per turn:
  - tool check   every expect_tools name was called; no forbid_tools name was
  - judge        a fixed grader model decides pass/fail against the turn rubric
  - errors       a turn that errored, timed out, or persisted no final answer
  - citations    on expect_citation turns: was memory injected, was it cited,
                 how many cited ordinals were dropped as invalid
  - cache/usage  Anthropic calls parsed from the daemon-log lines captured
                 during the turn (best-effort; see run_ab.py)

Judge verdicts are cached in <run>/judge_cache.json keyed by (judge model,
prompt), so re-grading is free and a run is graded identically every time.
The judge model is fixed per comparison: use the same --judge-model for the
baseline and the candidate.

  ./grade_ab.py results/baseline                    # grade, write summary.json
  ./grade_ab.py results/baseline --no-judge         # deterministic checks only
  ./grade_ab.py --compare results/baseline results/after
"""

import argparse
import glob
import hashlib
import json
import os
import re
import sys
import time

import requests

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DEFAULT_JUDGE = "claude-sonnet-5"

JUDGE_SYSTEM = (
    "You grade one turn of a conversation with a voice assistant named FRIDAY. "
    "Decide whether the assistant's answer to the LATEST user message satisfies the rubric. "
    "Judge substance only: tone, persona, brevity and formatting do not matter. Numbers must "
    "be correct; equivalent phrasings and reasonable rounding are fine. "
    'Reply with JSON only: {"pass": true|false, "reason": "<one sentence>"}')

RE_CACHE_WRITE = re.compile(r"Claude cache created: (\d+)")
RE_CACHE_READ = re.compile(r"Claude cache hit: (\d+)")
RE_CLAUDE_USAGE = re.compile(r"Claude usage: (\d+) input, (\d+) output")


# ----------------------------------------------------------------------------- helpers

def load_secret(name):
    for p in (os.path.join(REPO, "secrets.toml"), os.path.expanduser("~/.config/dawn/secrets.toml")):
        if os.path.exists(p):
            with open(p) as f:
                m = re.search(rf'^\s*{name}\s*=\s*"([^"]+)"', f.read(), re.MULTILINE)
            if m:
                return m.group(1)
    return None


def load_run(run_dir):
    with open(os.path.join(run_dir, "manifest.json")) as f:
        manifest = json.load(f)
    arts = []
    for path in sorted(glob.glob(os.path.join(run_dir, "*", "*.json"))):
        with open(path) as f:
            arts.append(json.load(f))
    return manifest, arts


def parse_claude_calls(lines):
    """Group daemon-log lines into per-call records (cache lines precede the usage line)."""
    calls, read, write = [], 0, 0
    for ln in lines or []:
        m = RE_CACHE_WRITE.search(ln)
        if m:
            write += int(m.group(1))
            continue
        m = RE_CACHE_READ.search(ln)
        if m:
            read += int(m.group(1))
            continue
        m = RE_CLAUDE_USAGE.search(ln)
        if m:
            uncached = int(m.group(1))
            calls.append({"read": read, "write": write, "uncached": uncached,
                          "prompt": read + write + uncached, "output": int(m.group(2))})
            read, write = 0, 0
    return calls


def transcript_for_judge(turns, upto):
    parts = []
    for t in turns[:upto]:
        p = t.get("persisted") or {}
        parts.append(f"USER: {t['script']['text']}\nFRIDAY: {p.get('final_answer') or '(no answer)'}")
    return "\n\n".join(parts)


# ----------------------------------------------------------------------------- judge

class Judge:
    def __init__(self, model, cache_path, enabled):
        self.model = model
        self.cache_path = cache_path
        self.enabled = enabled
        self.key = load_secret("claude_api_key") or os.environ.get("ANTHROPIC_API_KEY")
        self.cache = {}
        if os.path.exists(cache_path):
            with open(cache_path) as f:
                self.cache = json.load(f)
        self.calls = 0

    def save(self):
        with open(self.cache_path, "w") as f:
            json.dump(self.cache, f, indent=1)

    def verdict(self, prompt):
        k = hashlib.sha256(f"{self.model}\n{JUDGE_SYSTEM}\n{prompt}".encode()).hexdigest()
        if k in self.cache and self.cache[k].get("parsed"):
            return self.cache[k]
        if not self.enabled:
            return None
        if not self.key:
            sys.exit("judge needs claude_api_key in secrets.toml (or ANTHROPIC_API_KEY); "
                     "or pass --no-judge")
        # Current models think by default when `thinking` is omitted; fix the effort so the
        # judge is cheap and deterministic in shape, and leave room for thinking + verdict.
        body = {"model": self.model, "max_tokens": 4096, "system": JUDGE_SYSTEM,
                "output_config": {"effort": "low"},
                "messages": [{"role": "user", "content": prompt}]}
        for attempt in range(4):
            r = requests.post("https://api.anthropic.com/v1/messages", json=body, timeout=120,
                              headers={"x-api-key": self.key, "anthropic-version": "2023-06-01",
                                       "content-type": "application/json"})
            if r.status_code in (429, 500, 502, 503, 529):
                time.sleep(2 ** attempt * 3)
                continue
            r.raise_for_status()
            break
        else:
            r.raise_for_status()
        self.calls += 1
        text = "".join(b.get("text", "") for b in r.json().get("content", [])
                       if b.get("type") == "text")
        m = re.search(r"\{.*\}", text, re.DOTALL)
        try:
            v = json.loads(m.group(0)) if m else {}
        except ValueError:
            v = {}
        if not v:  # truncated/odd JSON: recover the verdict field if it is unambiguous
            pm = re.search(r'"pass"\s*:\s*(true|false)', text)
            if pm:
                v = {"pass": pm.group(1) == "true", "reason": text[:300]}
        out = {"pass": bool(v.get("pass")), "reason": v.get("reason") or text[:300],
               "parsed": bool(v)}
        if not v:
            return out  # don't cache an unusable verdict; a re-grade retries it
        self.cache[k] = out
        self.save()
        return out


# ----------------------------------------------------------------------------- grading

def grade_turn(art, i, judge):
    t = art["turns"][i]
    script, live, pers = t["script"], t.get("live") or {}, t.get("persisted") or {}
    called = [c.get("name") for c in pers.get("tool_calls", [])]
    expect, forbid = script.get("expect_tools", []), script.get("forbid_tools", [])
    tool_ok = all(n in called for n in expect) and not any(n in called for n in forbid)
    answer = pers.get("final_answer")
    errs = live.get("errors") or []
    notices = [e for e in errs if str(e.get("code") or "").startswith("INFO_")]
    real_errors = [e for e in errs if e not in notices]
    errored = bool(real_errors) or not answer

    verdict = None
    if answer:
        prompt = []
        if script.get("needs_date"):
            prompt.append(f"Wall-clock time when the user sent the latest message: "
                          f"{live.get('sent_weekday')}, {live.get('sent_at')}.")
        prior = transcript_for_judge(art["turns"], i)
        if prior:
            prompt.append(f"Earlier conversation:\n{prior}")
        prompt.append(f"LATEST USER MESSAGE: {script['text']}")
        prompt.append(f"ASSISTANT ANSWER: {answer}")
        prompt.append(f"RUBRIC: {script['rubric']}")
        verdict = judge.verdict("\n\n".join(prompt))

    audits = pers.get("citation_audit", [])
    injected = any(a.get("injected_ids") for a in audits)
    cited = any(a.get("cited_ids") for a in audits)
    # Memory (fact/summary) vs document-chunk injections: a recall turn is only
    # testing memory retrieval if memory items were actually injected.
    inj_ids = [i for a in audits for i in (a.get("injected_ids") or "").split(",") if i]
    cit_ids = [i for a in audits for i in (a.get("cited_ids") or "").split(",") if i]
    mem_kinds = ("fact:", "summary:")
    injected_memory = any(i.startswith(mem_kinds) for i in inj_ids)
    cited_memory = any(i.startswith(mem_kinds) for i in cit_ids)
    dropped = sum((a.get("dropped_count") or 0) for a in audits)
    calls = parse_claude_calls(live.get("daemon_log"))
    return {"scenario": art["scenario"], "rep": art.get("rep", 1), "turn": i + 1,
            "tool_ok": tool_ok, "called": called, "expect_tools": expect,
            "errored": errored, "errors": real_errors, "notices": notices,
            "elapsed_s": live.get("elapsed_s"),
            "judge": verdict, "expect_citation": bool(script.get("expect_citation")),
            "injected": injected, "cited": cited, "dropped": dropped,
            "injected_memory": injected_memory, "cited_memory": cited_memory,
            "memory_tool_calls": sum(1 for n in called if n in ("memory", "recall")),
            "claude_calls": calls}


def pct(n, d):
    return round(100.0 * n / d, 1) if d else None


def summarize(rows):
    n = len(rows)
    judged = [r for r in rows if r["judge"] is not None]
    cite_rows = [r for r in rows if r["expect_citation"]]
    calls = [c for r in rows for c in r["claude_calls"]]
    prompt = sum(c["prompt"] for c in calls)
    lat = sorted(r["elapsed_s"] for r in rows if r["elapsed_s"] is not None)
    return {
        "turns": n,
        "errored": sum(r["errored"] for r in rows),
        "turns_with_notices": sum(bool(r.get("notices")) for r in rows),
        "tool_ok_pct": pct(sum(r["tool_ok"] for r in rows), n),
        "judge_pass_pct": pct(sum(r["judge"]["pass"] for r in judged), len(judged)),
        "judged": len(judged),
        "recall_turns": len(cite_rows),
        "recall_injected_pct": pct(sum(r["injected"] for r in cite_rows), len(cite_rows)),
        "recall_cited_pct": pct(sum(r["cited"] for r in cite_rows), len(cite_rows)),
        "recall_memory_injected_pct": pct(sum(r["injected_memory"] for r in cite_rows),
                                          len(cite_rows)),
        "recall_memory_cited_pct": pct(sum(r["cited_memory"] for r in cite_rows), len(cite_rows)),
        "recall_memory_tool_calls": sum(r["memory_tool_calls"] for r in cite_rows),
        "cite_rate_all_injected_pct": pct(sum(r["cited"] for r in rows if r["injected"]),
                                          sum(r["injected"] for r in rows)),
        "dropped_citations": sum(r["dropped"] for r in rows),
        "latency_median_s": lat[len(lat) // 2] if lat else None,
        "claude_calls_logged": len(calls),
        "cache_read_pct_of_prompt": pct(sum(c["read"] for c in calls), prompt),
        "cache_write_pct_of_prompt": pct(sum(c["write"] for c in calls), prompt),
        "uncached_pct_of_prompt": pct(sum(c["uncached"] for c in calls), prompt),
        "prompt_tokens_logged": prompt,
    }


def grade(run_dir, judge_model, use_judge):
    manifest, arts = load_run(run_dir)
    judge = Judge(judge_model, os.path.join(run_dir, "judge_cache.json"), use_judge)
    by_model, harness_errors = {}, []
    for art in arts:
        if art.get("harness_error"):
            harness_errors.append({"model": art["model"], "scenario": art["scenario"],
                                   "error": art["harness_error"]})
            continue
        rows = by_model.setdefault(art["model"], [])
        for i in range(len(art["turns"])):
            rows.append(grade_turn(art, i, judge))
    summary = {"run": os.path.abspath(run_dir), "label": manifest.get("label"),
               "git": manifest.get("git"), "judge_model": judge_model if use_judge else None,
               "harness_errors": harness_errors,
               "models": {m: summarize(r) for m, r in by_model.items()},
               "turns": by_model}
    with open(os.path.join(run_dir, "summary.json"), "w") as f:
        json.dump(summary, f, indent=2)
    print_summary(summary)
    if judge.calls:
        print(f"(judge calls this pass: {judge.calls})")
    return summary


METRICS = [("turns", "turns"), ("errored", "errored turns"),
           ("turns_with_notices", "turns w/ INFO notice"), ("tool_ok_pct", "tool check %"),
           ("judge_pass_pct", "judge pass %"), ("recall_injected_pct", "recall: injected %"),
           ("recall_cited_pct", "recall: cited %"),
           ("recall_memory_injected_pct", "recall: memory injected %"),
           ("recall_memory_cited_pct", "recall: memory cited %"),
           ("recall_memory_tool_calls", "recall: memory tool calls"), ("cite_rate_all_injected_pct", "cite-rate %"),
           ("dropped_citations", "dropped citations"), ("latency_median_s", "median turn s"),
           ("claude_calls_logged", "claude calls (log)"),
           ("cache_read_pct_of_prompt", "cache read % prompt"),
           ("cache_write_pct_of_prompt", "cache write % prompt"),
           ("uncached_pct_of_prompt", "uncached % prompt")]


def print_summary(s):
    print(f"\n{s['run']}  label={s.get('label')!r}  git={(s.get('git') or {}).get('rev', '')[:10]}")
    for e in s["harness_errors"]:
        print(f"  HARNESS ERROR {e['model']} {e['scenario']}: {e['error']}")
    for model, m in s["models"].items():
        print(f"\n  {model}")
        for key, label in METRICS:
            print(f"    {label:24s} {m.get(key)}")
    for model, rows in s["turns"].items():
        bad = [r for r in rows if r["errored"] or not r["tool_ok"]
               or (r["judge"] is not None and not r["judge"]["pass"])]
        for r in bad:
            why = r["judge"]["reason"] if r["judge"] else ""
            print(f"  FAIL {model} {r['scenario']}#{r['turn']}: tool_ok={r['tool_ok']} "
                  f"called={r['called']} errored={r['errored']} {why}")


def compare(a_dir, b_dir):
    with open(os.path.join(a_dir, "summary.json")) as f:
        a = json.load(f)
    with open(os.path.join(b_dir, "summary.json")) as f:
        b = json.load(f)
    if a.get("judge_model") != b.get("judge_model"):
        print(f"WARNING: judge models differ ({a.get('judge_model')} vs {b.get('judge_model')}) "
              "-- judge pass rates are not comparable")
    print(f"A = {a['run']} ({a.get('label')!r})\nB = {b['run']} ({b.get('label')!r})")
    for model in sorted(set(a["models"]) | set(b["models"])):
        ma, mb = a["models"].get(model, {}), b["models"].get(model, {})
        print(f"\n  {model}")
        print(f"    {'metric':24s} {'A':>10s} {'B':>10s}")
        for key, label in METRICS:
            print(f"    {label:24s} {str(ma.get(key)):>10s} {str(mb.get(key)):>10s}")


def main():
    ap = argparse.ArgumentParser(description="Grade / compare dawn_ab runs")
    ap.add_argument("run_dir", nargs="?")
    ap.add_argument("--judge-model", default=DEFAULT_JUDGE)
    ap.add_argument("--no-judge", action="store_true", help="deterministic checks only")
    ap.add_argument("--compare", nargs=2, metavar=("RUN_A", "RUN_B"))
    args = ap.parse_args()
    if args.compare:
        compare(*args.compare)
    elif args.run_dir:
        grade(args.run_dir, args.judge_model, not args.no_judge)
    else:
        ap.error("give a run directory or --compare A B")


if __name__ == "__main__":
    main()
