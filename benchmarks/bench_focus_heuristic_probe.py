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
# Reranker Phase B-i — Dominant-token heuristic prototype.
#
# Standalone Python probe for the "low-IDF dominant-token cosine
# over-inclusion" pathology surfaced by Phase A.  Three new fixture
# cases (16-18) demonstrate that cases where:
#   1. query has a low-IDF dominant token (e.g. "favorite restaurant",
#      "doctor blood test", "database migration project"), AND
#   2. > threshold of candidates share that token, AND
#   3. the right answer doesn't contain it (uses specific vocabulary)
# unanimously fail under any reasonable focus_injection weight setting
# because the semantic gap exceeds what the recency lever can close.
#
# This script tests whether a non-ML heuristic (downweight candidates
# whose query-overlap is ONLY via dominant tokens) is sufficient to
# rescue these cases without regressing the existing 15.  If it works,
# Phase B-ii implements the heuristic in C; if it doesn't, fall back to
# cross-encoder or LLM-as-judge.
#
# Architecture:
#   - Loads cases from focus_probe_cases.json (same schema as
#     bench_focus_fix_rate.py).
#   - For each case, computes a per-candidate dominant-token penalty.
#   - Bakes the penalty into `semantic_override` (subtracts; bench's
#     composite formula propagates this faithfully because w_sem=1.0).
#   - Calls existing bench_focus_pipeline binary with adjusted seeds.
#   - Parses rendered_block + score_breakdown from stdout.
#   - --dry mode: checks if expected substrings appear in rendered_block.
#   - --judge mode: runs multi-model 2-of-3 LLM judge quorum (paid).
#
# Flags:
#   --threshold X       dominance threshold (fraction of pool, default 0.6)
#   --base-penalty X    max penalty applied (composite-score units, default 0.4)
#   --sweep             parameter sweep across pre-set values (dry-only)
#   --dry               no LLM calls; just substring + rank check
#   --judge             paid LLM judge with chosen params
#   --cases X,Y         filter to specific cases during dev
#
# Phase A's design doc (RERANKER_PHASE_A_DESIGN.md) frames this as the
# first candidate in Phase B's decision matrix.  If it passes all 18
# cases at clean threshold/penalty, Phase B-ii is greenlit.

import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from multi_model_probe import (  # noqa: E402
    AGGREGATE_QUORUM,
    PROVIDER_DEFAULTS,
    load_secrets,
    resolve_provider_config,
)

DAWN_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BENCH_BIN = DAWN_ROOT / "build-debug/tests/bench_focus_pipeline"
DEFAULT_CASES_PATH = DAWN_ROOT / "benchmarks/focus_probe_cases.json"
DEFAULT_SECRETS_PATH = DAWN_ROOT / "secrets.toml"
DEFAULT_DAWN_TOML = DAWN_ROOT / "dawn.toml"

# Mirrors bench_focus_fix_rate.py's DEFAULT_FOCUS_CONFIG (corrected for
# Step-3 + Phase 1j re-bench).  Cross-check with that script if
# defaults drift.
DEFAULT_FOCUS_CONFIG = {
    "top_k": 8,
    "min_score": 0.40,
    "focus_budget_bytes": 10240,
    "weight_semantic": 1.0,
    "weight_recency": 0.30,
    "weight_importance": 1.00,
    "weight_source": 1.0,
    "source_weights": {
        "memory_fact": 1.0,
        "memory_entity": 0.9,
        "memory_relation": 0.85,
        "memory_summary": 1.0,
        "document_chunk": 0.7,
        "calendar_event": 0.6,
        "recent_email": 0.5,
        "dawn_background": 0.8,
    },
    "dedup": {
        "recent_window_turns": 8,
        "score_uplift_factor": 1.5,
    },
}

# Conservative stopword set — these are too common to be meaningful
# discriminators.  Pronouns, articles, common verbs.  Match Phase A
# fixture design which avoids stopwords in the dominant-token shape.
STOPWORDS = {
    "the", "a", "an", "is", "are", "was", "were", "be", "been", "being",
    "have", "has", "had", "do", "does", "did", "will", "would", "could",
    "should", "may", "might", "in", "on", "at", "to", "of", "for", "and",
    "or", "but", "with", "by", "from", "as", "this", "that", "these",
    "those", "my", "your", "his", "her", "their", "our", "i", "me", "you",
    "he", "she", "it", "we", "they", "what", "whats", "who", "whom",
    "when", "where", "why", "how", "hows", "if", "than", "so", "out",
    "up", "down", "off", "over", "under", "into", "onto", "about",
    "yes", "no", "not", "any", "some", "all", "each", "every", "most",
    "more", "much", "many", "few", "lot", "lots", "very", "just", "only",
    "also", "even", "still", "ever", "never", "always", "sometimes",
    "user", "users", "users's", "use", "used", "using",
}

TOKEN_RE = re.compile(r"[a-z0-9]+(?:'[a-z]+)?")


def tokens(text: str) -> set[str]:
    """Tokenize: lowercase, alphanumeric runs, drop stopwords + short."""
    return {
        t for t in TOKEN_RE.findall(text.lower())
        if len(t) > 3 and t not in STOPWORDS
    }


def compute_penalties(query: str, candidates: list[dict],
                      threshold: float, base_penalty: float) -> list[float]:
    """
    Returns per-candidate penalty values (composite-score units).

    Algorithm:
      1. Tokenize query, drop stopwords + short words.
      2. For each query token, count how many candidates contain it.
      3. Tokens shared by > threshold of candidates are "dominant".
      4. For each candidate, compute:
         - shared_query_toks = intersection of query and candidate tokens
         - shared_dominant   = shared_query_toks ∩ dominant_tokens
         - non_dominant_match = shared_query_toks - dominant_tokens
         - If no shared_dominant → no penalty
         - If non_dominant_match present → penalty diluted by ratio
         - If only dominant shared → full base_penalty
    """
    q_toks = tokens(query)
    if not q_toks:
        return [0.0] * len(candidates)

    n = len(candidates)
    if n == 0:
        return []

    cand_toks = [tokens(c["text"]) for c in candidates]
    token_freq = Counter()
    for ct in cand_toks:
        for t in q_toks & ct:
            token_freq[t] += 1
    dominant = {t for t, f in token_freq.items() if f / n > threshold}

    penalties = []
    for ct in cand_toks:
        shared = q_toks & ct
        sd = shared & dominant
        nd = shared - dominant
        if not sd:
            penalties.append(0.0)
        elif not nd:
            penalties.append(base_penalty)
        else:
            # Diluted: penalty scales with the fraction of matches that
            # are dominant-only.  Candidate matching 1 dominant + 2
            # non-dominant gets penalty * 1/3.
            penalties.append(base_penalty * len(sd) / len(shared))

    return penalties


def adjust_seeds(case: dict, threshold: float, base_penalty: float) -> tuple[dict, list[float]]:
    """
    Apply penalty by reducing each seed's semantic_override.  Because
    composite = w_sem * sem + ... and w_sem = 1.0 by default, reducing
    semantic_override by `penalty` reduces composite by `penalty`.
    Returns adjusted case + per-candidate penalty list.
    """
    penalties = compute_penalties(
        case["user_query"], case["memory_seed"], threshold, base_penalty)
    adjusted = dict(case)
    adjusted["memory_seed"] = []
    for seed, pen in zip(case["memory_seed"], penalties):
        new_seed = dict(seed)
        new_seed["semantic_override"] = max(
            0.0, seed["semantic_override"] - pen)
        adjusted["memory_seed"].append(new_seed)
    return adjusted, penalties


def run_bench_pipeline(case: dict, bench_bin: Path,
                       config: dict) -> dict:
    """Run bench_focus_pipeline against a single case; return parsed JSON."""
    payload = {
        "protocol_version": 1,
        "query": case["user_query"],
        "memory_seed": case["memory_seed"],
        "config": config,
        "session_state": None,
    }
    proc = subprocess.run(
        [str(bench_bin)],
        input=json.dumps(payload),
        capture_output=True, text=True, timeout=30)
    if proc.returncode != 0:
        raise RuntimeError(
            f"bench_focus_pipeline failed: {proc.stderr.strip()}")
    return json.loads(proc.stdout)


def check_substrings(rendered_block: str, expected: list[str]) -> bool:
    """Dry-mode rank check: are all expected substrings in the block?"""
    if not expected:
        # Negative case — block should be empty or weak (skip).
        return True
    rb = rendered_block.lower()
    return all(e.lower() in rb for e in expected)


JUDGE_SYSTEM = (
    "You are evaluating whether a memory-injection focus block contains "
    "the expected information.  Respond with a single word: YES or NO.  "
    "Say YES if the block contains ALL of the expected facts (or "
    "appropriate substrings for them).  Say NO if any expected fact is "
    "missing.  No explanations."
)

JUDGE_TEMPLATE = (
    "Focus block presented to the LLM:\n\n{block}\n\n"
    "User query: {query}\n\n"
    "Expected facts that should appear in the focus block: {expected}\n\n"
    "Does the focus block contain ALL of these expected facts (or "
    "appropriate substrings of them)?  Respond YES or NO only."
)

JUDGE_MAX_TOKENS = 64


def call_judge(provider_cfg: dict, system: str, prompt: str,
               max_tokens: int = JUDGE_MAX_TOKENS) -> str:
    """Single LLM call; returns 'YES' / 'NO' / 'ERR'."""
    try:
        text = provider_cfg["caller"](
            provider_cfg["model"], system, prompt,
            provider_cfg["api_key"], provider_cfg["endpoint"],
            max_tokens=max_tokens)
    except Exception as e:
        print(f"  [{provider_cfg['name']}] judge error: {e}",
              file=sys.stderr)
        return "ERR"
    up = text.strip().upper()
    if "YES" in up[:8]:
        return "YES"
    if "NO" in up[:8]:
        return "NO"
    return "ERR"


def judge_case(case: dict, rendered_block: str,
               providers: list[dict]) -> dict:
    """Run all providers + quorum on a single case."""
    expected = ", ".join(case["expected_facts_to_surface"]) or "(none)"
    prompt = JUDGE_TEMPLATE.format(
        block=rendered_block,
        query=case["user_query"],
        expected=expected)
    verdicts = {}
    for p in providers:
        verdicts[p["name"]] = call_judge(p, JUDGE_SYSTEM, prompt)
    yes_count = sum(1 for v in verdicts.values() if v == "YES")
    quorum = "PASS" if yes_count >= AGGREGATE_QUORUM else "FAIL"
    return {"per_provider": verdicts, "quorum": quorum,
            "yes_count": yes_count}


def main():
    p = argparse.ArgumentParser(
        description=("Phase B-i dominant-token heuristic probe.  "
                     "Standalone prototype; ships nothing."),
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    p.add_argument("--cases-path", type=Path, default=DEFAULT_CASES_PATH)
    p.add_argument("--bench-bin", type=Path, default=DEFAULT_BENCH_BIN)
    p.add_argument("--secrets-path", type=Path, default=DEFAULT_SECRETS_PATH)
    p.add_argument("--dawn-toml", type=Path, default=DEFAULT_DAWN_TOML)
    p.add_argument("--cases", type=str, default="")
    p.add_argument("--threshold", type=float, default=0.6)
    p.add_argument("--base-penalty", type=float, default=0.4)
    p.add_argument("--sweep", action="store_true",
                   help="dry-mode parameter sweep, no LLM calls")
    p.add_argument("--dry", action="store_true",
                   help="no LLM calls; substring + rank check only")
    p.add_argument("--judge", action="store_true",
                   help="paid LLM judge with chosen --threshold/--base-penalty")
    p.add_argument("--skip-local", action="store_true")
    args = p.parse_args()

    if not args.bench_bin.is_file() or not os.access(args.bench_bin, os.X_OK):
        sys.exit(f"error: bench binary missing or not executable: {args.bench_bin}")
    if not args.cases_path.is_file():
        sys.exit(f"error: cases file missing: {args.cases_path}")

    with args.cases_path.open() as f:
        all_cases = json.load(f)["cases"]
    if args.cases:
        wanted = {x.strip() for x in args.cases.split(",") if x.strip()}
        all_cases = [c for c in all_cases if c["case_id"] in wanted]
    if not all_cases:
        sys.exit("error: no cases after filter")

    if args.sweep:
        print(f"=== Sweep: dry-mode (no LLM), {len(all_cases)} cases ===")
        param_grid = [(t, p_)
                      for t in (0.5, 0.6, 0.7)
                      for p_ in (0.30, 0.40, 0.50)]
        print(f"{'threshold':>10} {'penalty':>8} | "
              f"{'pass':>5}/{'total':>5} | failed cases")
        print("-" * 90)
        for thr, pen in param_grid:
            results = []
            for case in all_cases:
                adj, _ = adjust_seeds(case, thr, pen)
                out = run_bench_pipeline(adj, args.bench_bin, DEFAULT_FOCUS_CONFIG)
                ok = check_substrings(
                    out["rendered_block"],
                    case["expected_facts_to_surface"])
                results.append((case["case_id"], ok))
            n_pass = sum(1 for _, ok in results if ok)
            fails = [cid for cid, ok in results if not ok]
            print(f"{thr:>10.2f} {pen:>8.2f} | {n_pass:>5}/{len(results):>5} | "
                  f"{', '.join(fails) if fails else '(all pass)'}")
        return

    if args.dry or not args.judge:
        # Default to dry single-config run.
        print(f"=== Single-config dry-run: threshold={args.threshold} "
              f"base_penalty={args.base_penalty} ===")
        for case in all_cases:
            adj, penalties = adjust_seeds(case, args.threshold, args.base_penalty)
            out = run_bench_pipeline(adj, args.bench_bin, DEFAULT_FOCUS_CONFIG)
            ok = check_substrings(
                out["rendered_block"],
                case["expected_facts_to_surface"])
            verdict = "PASS" if ok else "FAIL"
            # Show penalty distribution for sanity.
            n_penalized = sum(1 for pp in penalties if pp > 0)
            print(f"  {case['case_id']:55s} {verdict}  "
                  f"({n_penalized}/{len(penalties)} candidates penalized)")
        return

    # Paid path: LLM judge.
    print(f"=== Paid judge run: threshold={args.threshold} "
          f"base_penalty={args.base_penalty} ===")
    secrets = load_secrets(args.secrets_path)
    providers = []
    for name in ("anthropic", "openai", "local"):
        if name == "local" and args.skip_local:
            continue
        try:
            caller, model, endpoint, api_key = resolve_provider_config(
                name, secrets, args.dawn_toml)
            providers.append({"name": name, "caller": caller, "model": model,
                              "endpoint": endpoint, "api_key": api_key})
        except SystemExit:
            print(f"  [{name}] skipping — missing credentials or endpoint",
                  file=sys.stderr)
    if not providers:
        sys.exit("error: no providers available")
    print(f"  Providers: {[p['name'] for p in providers]}")

    results = []
    for case in all_cases:
        adj, penalties = adjust_seeds(case, args.threshold, args.base_penalty)
        out = run_bench_pipeline(adj, args.bench_bin, DEFAULT_FOCUS_CONFIG)
        print(f"  Running {case['case_id']}...")
        verdict = judge_case(case, out["rendered_block"], providers)
        results.append((case, verdict))

    print("\n=== Per-case verdict matrix ===")
    header = f"{'case':50s} {'HEAD':>6}"
    for p in providers:
        header += f"  {p['name']:>9}"
    header += f"  {'quorum':>6}"
    print(header)
    print("-" * len(header))
    for case, verdict in results:
        head_marker = "fail" if case["head_known_fail"] else "pass"
        line = f"{case['case_id']:50s} {head_marker:>6}"
        for p in providers:
            line += f"  {verdict['per_provider'][p['name']]:>9}"
        line += f"  {verdict['quorum']:>6}"
        print(line)

    print("\n=== Aggregate fix-rate (quorum 2-of-3) ===")
    pass_count = sum(1 for _, v in results if v["quorum"] == "PASS")
    print(f"  aggregate : {pass_count}/{len(results)} quorum-pass")
    per_prov = {p["name"]: 0 for p in providers}
    for _, v in results:
        for prov, ans in v["per_provider"].items():
            if ans == "YES":
                per_prov[prov] += 1
    for name, count in per_prov.items():
        print(f"  {name:10s}: {count}/{len(results)} fix")


if __name__ == "__main__":
    main()
