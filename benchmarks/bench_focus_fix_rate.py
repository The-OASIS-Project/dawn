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
#
# Phase 1i.C — multi-model focus-injection fix-rate probe (Component 6).
#
# Architecture:
#   - Loads synthetic cases from focus_probe_cases.json (no real PII).
#   - For each case: shells out to ./build-debug/tests/bench_focus_pipeline
#     with the case's memory_seed + query + config as JSON stdin, parses
#     the rendered focus block from stdout JSON.
#   - For each provider in {anthropic, openai, local}: asks the model
#     "does this surfaced context contain ALL of the expected facts?"
#     (binary YES/NO).
#   - Applies AGGREGATE_QUORUM (2-of-3) over per-provider verdicts.
#   - Prints a per-case verdict matrix and aggregate fix-rate.
#
# Flags:
#   --dry          run everything except LLM calls; verifies the bench
#                  binary integration + JSON parse path.
#   --recalibrate  prints a Python literal block for paste into
#                  HEAD_BASELINE_FIX_COUNT (per-provider HEAD baselines).
#   --cases X,Y    filter to specific case_ids during dev.
#   --bench-bin    override the bench binary path (default
#                  ./build-debug/tests/bench_focus_pipeline).
#
# Coordinator pin: this turn does NOT run the paid Component 6 dry-run.
# `--recalibrate` against HEAD is the manual step the coordinator
# authorizes once review passes.  Without baselines, we report
# (fix_count, total) per provider as raw measurement — no PASS/FAIL.

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

# Reach into the factored harness from 1i.A.  These are the same imports
# bench_speaker_attribution.py uses; identical surface.
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

# Per-provider HEAD baselines for component 6 fix-rate.
# Recalibrated 2026-05-13 (Phase B-ii reranker workstream) after
# landing the dominant-token over-inclusion heuristic in
# memory/focus_dominant_token.c.  18/18 across all three providers
# on the augmented 18-case fixture set — cases 16-18 lift from
# unanimous FAIL × 3 (Phase A baseline) to unanimous PASS × 3 with
# zero regression on the existing 15.
#
# Prior bumps preserved in atlas tuning log:
#   - Phase 1j.B (2026-05-08): w_imp 0.2 → 1.0, w_rec 0.3 → 0.15,
#     11/11 fix-rate against the original 11-case fixture set
#   - Phase 1j re-bench (2026-05-13): w_rec 0.15 → 0.30, 15/15
#     fix-rate against the augmented 15-case set
#   - Phase B-ii (2026-05-13): dominant-token heuristic, 18/18
#     against the 18-case set
#
# Regression detection: (candidate_fix_count < baseline_fix_count)
# is a FAIL on that provider.  See atlas/dawn/memory/PHASE_1J_TUNING_LOG.md
# and docs/RERANKER_PHASE_A_DESIGN.md for the full bench history.
HEAD_BASELINE_FIX_COUNT = {
    "anthropic": 18,
    "openai": 18,
    "local": 18,
}

# Production-shape config block — passed verbatim to bench_focus_pipeline
# unless a case overrides via per-case "config" field.  Mirrors the
# shipped dawn.toml.example focus_injection defaults with one
# deliberate exception:
#
#   top_k=8 here vs production's top_k=12 (config_defaults.c:342).
#
# The fixture-design contract (v2 schema_version) pins rank 9-12 for
# FAIL cases and rank 5-8 for PASS cases under top_k=8 as a stress-test
# parameter.  Bumping the harness to 12 would let every existing FAIL
# case pass by default and erase the probe's pathology coverage.
# Production runs with the extra 4 slots of slack precisely because
# top_k=8 was too tight after Step-3 added summaries to the per-turn
# budget; the bench keeps 8 to surface pathologies the production
# default has room to absorb.
#
# memory_summary source weight raised 0.7 → 1.0 in May 2026 (Step-3
# default change at config_defaults.c:373); harness updated to match
# 2026-05-13.
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
    # Phase B-ii: dominant-token over-inclusion heuristic.  Mirrors the
    # shipped production defaults in config_defaults.c.  Cases 16-18
    # (severe_dominant_token_*) require enabled=true to pass; cases 1-15
    # are invariant to the heuristic (none trigger dominant-token shape).
    "dominant_token_heuristic": {
        "enabled": True,
        "threshold": 0.60,
        "base_penalty": 0.40,
    },
}

# Maximum response tokens for the YES/NO judge prompt — minimal so the
# spend per call is small.  At ~$0.005-0.01 per call × 10 cases × 3
# providers = ~$0.15-0.30, under the M4 cap.
JUDGE_MAX_TOKENS = 64

JUDGE_SYSTEM = (
    "You evaluate whether a context block surfaced for an LLM contains "
    "specific facts.  Reply with exactly one word: YES or NO.  Do not "
    "explain.  Substring match is acceptable (case-insensitive)."
)


def build_judge_user_prompt(rendered_block, expected_facts):
    """Return the user-prompt string sent to each provider for verdict."""
    if not expected_facts:
        # Negative-empty case — we want the model to confirm the surfaced
        # context does NOT introduce noise relevant to the query.  We
        # invert: "is the context appropriately empty / minimal?"
        return (
            "Surfaced context (may be empty):\n"
            "----\n"
            f"{rendered_block.strip() or '(empty)'}\n"
            "----\n"
            "There are NO expected facts for this case (the query was "
            "intentionally unrelated to the memory pool).\n"
            "Does the surfaced context appropriately contain no relevant "
            "items, OR clearly indicate a weak/marginal match?  "
            "Reply YES if the context is empty / minimal / irrelevant, "
            "NO if it surfaces strongly-relevant items it should not.")
    facts_lines = "\n".join(f"- {f}" for f in expected_facts)
    return (
        "Surfaced context:\n"
        "----\n"
        f"{rendered_block.strip() or '(empty)'}\n"
        "----\n"
        "Expected facts to surface (the context must contain ALL of these):\n"
        f"{facts_lines}\n"
        "Does the surfaced context contain ALL of the expected facts "
        "(case-insensitive substring match acceptable)?  Reply YES or NO.")


def parse_yesno(text):
    """Return True for YES, False for NO, None for ambiguous.  Tolerant
    of trailing punctuation and surrounding whitespace."""
    if not isinstance(text, str):
        return None
    s = text.strip().upper()
    s = re.sub(r'[^A-Z]', '', s)
    if s.startswith("YES"):
        return True
    if s.startswith("NO"):
        return False
    return None


# =============================================================================
# bench_focus_pipeline subprocess driver
# =============================================================================


def run_bench_pipeline(bench_bin, case, base_config):
    """Build stdin JSON for the case, shell out to bench_focus_pipeline,
    return the parsed stdout JSON.  Raises CalledProcessError on
    framework failure."""
    config = dict(base_config)
    if "config" in case:
        # Merge per-case overrides on top.
        config = {**config, **case["config"]}

    stdin_obj = {
        "protocol_version": 1,
        "config": config,
        "memory_seed": case["memory_seed"],
        "query": case["user_query"],
    }
    if "now" in case:
        stdin_obj["now"] = case["now"]
    stdin_payload = json.dumps(stdin_obj)

    proc = subprocess.run(
        [str(bench_bin)],
        input=stdin_payload,
        capture_output=True,
        text=True,
        timeout=30)
    if proc.returncode != 0:
        sys.stderr.write(
            f"  bench_focus_pipeline FAILED (rc={proc.returncode}) for "
            f"{case['case_id']}: {proc.stderr.strip()}\n")
        return None
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as e:
        sys.stderr.write(
            f"  bench_focus_pipeline produced unparseable JSON for "
            f"{case['case_id']}: {e}\n  stdout (first 400 chars): "
            f"{proc.stdout[:400]!r}\n")
        return None


# =============================================================================
# Per-provider judge call
# =============================================================================


def judge_with_provider(provider_name, caller_fn, model, endpoint, api_key,
                        rendered_block, expected_facts):
    """Send the judge prompt to one provider; return True (YES), False (NO),
    or None (ambiguous / error)."""
    user_prompt = build_judge_user_prompt(rendered_block, expected_facts)
    try:
        text = caller_fn(model, JUDGE_SYSTEM, user_prompt, api_key, endpoint,
                         temperature=0.0, max_tokens=JUDGE_MAX_TOKENS,
                         timeout=60.0)
    except Exception as exc:
        sys.stderr.write(
            f"    {provider_name}: judge call failed: {exc}\n")
        return None
    return parse_yesno(text)


# =============================================================================
# Main case-running loop
# =============================================================================


def run_case(case, providers, bench_bin, base_config, dry):
    """Execute one case end-to-end.  Returns a dict with the verdict
    matrix + bench output excerpt."""
    bench_out = run_bench_pipeline(bench_bin, case, base_config)
    if bench_out is None:
        return {
            "case_id": case["case_id"],
            "bench_failed": True,
            "candidate_count": 0,
            "rendered_block": "",
            "providers": {p["name"]: None for p in providers},
            "quorum_pass": None,
            "head_known_fail": case.get("head_known_fail", False),
        }

    rendered_block = bench_out.get("rendered_block", "")
    candidate_count = bench_out.get("candidate_count", 0)
    expected = case.get("expected_facts_to_surface", [])

    per_provider = {}
    for p in providers:
        if dry:
            per_provider[p["name"]] = None
            continue
        verdict = judge_with_provider(
            p["name"], p["caller"], p["model"], p["endpoint"], p["api_key"],
            rendered_block, expected)
        per_provider[p["name"]] = verdict

    yes_count = sum(1 for v in per_provider.values() if v is True)
    quorum_pass = (yes_count >= AGGREGATE_QUORUM) if not dry else None

    return {
        "case_id": case["case_id"],
        "bench_failed": False,
        "candidate_count": candidate_count,
        "rendered_block": rendered_block,
        "providers": per_provider,
        "quorum_pass": quorum_pass,
        "head_known_fail": case.get("head_known_fail", False),
    }


def print_verdict_matrix(results, providers, dry):
    print()
    print(f"=== Per-case verdict matrix ===")
    name_width = max((len(r["case_id"]) for r in results), default=20) + 2
    header = f"{'case':<{name_width}}{'cands':>7}"
    for p in providers:
        header += f"{p['name']:>11}"
    header += f"{'quorum':>9}{'HEAD':>7}"
    print(header)
    print("-" * len(header))
    for r in results:
        row = f"{r['case_id']:<{name_width}}{r['candidate_count']:>7}"
        for p in providers:
            v = r["providers"].get(p["name"])
            if v is True:
                mark = "YES"
            elif v is False:
                mark = "NO"
            elif v is None and dry:
                mark = "(dry)"
            elif r["bench_failed"]:
                mark = "BENCH-FAIL"
            else:
                mark = "?"
            row += f"{mark:>11}"
        if dry:
            row += f"{'(dry)':>9}"
        elif r["bench_failed"]:
            row += f"{'BENCH-FAIL':>9}"
        else:
            row += f"{'PASS' if r['quorum_pass'] else 'FAIL':>9}"
        row += f"{'fail' if r['head_known_fail'] else 'pass':>7}"
        print(row)
    print()


def print_aggregate(results, providers, dry):
    """Per-provider fix-count + aggregate quorum fix-rate."""
    if dry:
        print("=== Aggregate fix-rate (--dry: no LLM verdicts) ===")
        bench_ok = sum(1 for r in results if not r["bench_failed"])
        print(f"  Bench-binary integration: {bench_ok}/{len(results)} cases "
              f"produced parseable stdout.")
        print()
        return

    print(f"=== Aggregate fix-rate (quorum {AGGREGATE_QUORUM}-of-{len(providers)}) ===")
    total = len(results)
    for p in providers:
        passes = sum(1 for r in results
                     if r["providers"].get(p["name"]) is True)
        baseline = HEAD_BASELINE_FIX_COUNT.get(p["name"])
        baseline_str = (str(baseline) if baseline is not None
                        else "None (not yet calibrated)")
        print(f"  {p['name']:<10}: {passes}/{total} fix  baseline={baseline_str}")
    quorum_passes = sum(1 for r in results if r["quorum_pass"] is True)
    print(f"  {'aggregate':<10}: {quorum_passes}/{total} quorum-pass")
    print()


def print_recalibration_block(results, providers):
    """--recalibrate: print a Python literal block for paste into
    HEAD_BASELINE_FIX_COUNT.  Only prints fix counts (not aggregate)
    so future runs can detect per-provider regressions even when
    overall quorum stays steady."""
    print()
    print("=== Recalibration block — paste into HEAD_BASELINE_FIX_COUNT ===")
    print("HEAD_BASELINE_FIX_COUNT = {")
    for p in providers:
        passes = sum(1 for r in results
                     if r["providers"].get(p["name"]) is True)
        print(f"    {p['name']!r}: {passes},")
    print("}")
    print()


# =============================================================================
# Provider setup + main
# =============================================================================


def resolve_providers(secrets, dawn_toml, skip_local):
    out = []
    targets = ["anthropic", "openai"]
    if not skip_local:
        targets.append("local")
    for name in targets:
        try:
            caller, model, endpoint, api_key = resolve_provider_config(
                name, secrets, dawn_toml)
        except SystemExit:
            # resolve_provider_config sys.exit()'s on missing creds; we
            # want to skip rather than abort the whole run.
            sys.stderr.write(
                f"  {name}: skipping — missing credentials or endpoint.\n")
            continue
        out.append({
            "name": name,
            "caller": caller,
            "model": model,
            "endpoint": endpoint,
            "api_key": api_key,
        })
    return out


def main():
    p = argparse.ArgumentParser(
        description="Multi-model focus-injection fix-rate probe (Component 6).",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    p.add_argument("--cases-path", type=Path, default=DEFAULT_CASES_PATH,
                   help="Path to focus_probe_cases.json")
    p.add_argument("--bench-bin", type=Path, default=DEFAULT_BENCH_BIN,
                   help="Path to compiled bench_focus_pipeline binary")
    p.add_argument("--secrets-path", type=Path, default=DEFAULT_SECRETS_PATH)
    p.add_argument("--dawn-toml", type=Path, default=DEFAULT_DAWN_TOML)
    p.add_argument("--cases", type=str, default="",
                   help="Comma-separated case_ids to filter (default: all)")
    p.add_argument("--skip-local", action="store_true",
                   help="Skip the local provider (use when local LLM is offline)")
    p.add_argument("--dry", action="store_true",
                   help="Run bench-binary integration only; skip LLM calls")
    p.add_argument("--recalibrate", action="store_true",
                   help="Print a Python literal block of fix counts for "
                        "pasting into HEAD_BASELINE_FIX_COUNT.")
    args = p.parse_args()

    if not args.bench_bin.is_file() or not os.access(args.bench_bin, os.X_OK):
        sys.exit(f"error: bench binary not found or not executable: "
                 f"{args.bench_bin}\n  Build first: "
                 f"make -C build-debug bench_focus_pipeline")

    if not args.cases_path.is_file():
        sys.exit(f"error: cases file not found: {args.cases_path}")

    with args.cases_path.open() as f:
        cases_doc = json.load(f)
    cases = cases_doc.get("cases", [])
    if args.cases:
        wanted = {x.strip() for x in args.cases.split(",") if x.strip()}
        cases = [c for c in cases if c.get("case_id") in wanted]
        if not cases:
            sys.exit(f"error: no cases match filter {args.cases!r}")

    print(f"=== Focus-injection fix-rate probe ===")
    print(f"  Cases:      {len(cases)} from {args.cases_path}")
    print(f"  Bench bin:  {args.bench_bin}")
    if args.dry:
        print(f"  Mode:       --dry (no LLM calls)")

    secrets = load_secrets(args.secrets_path) if not args.dry else {}
    providers = ([] if args.dry
                 else resolve_providers(secrets, args.dawn_toml,
                                        args.skip_local))
    if args.dry:
        # Build a minimal pseudo-provider list so the matrix prints
        # consistent column widths without making any LLM calls.
        providers = [{"name": name,
                      "caller": None, "model": None,
                      "endpoint": None, "api_key": None}
                     for name in ("anthropic", "openai", "local")
                     if name in PROVIDER_DEFAULTS]
    print(f"  Providers:  {[p['name'] for p in providers]}")
    print()

    base_config = DEFAULT_FOCUS_CONFIG
    results = []
    for case in cases:
        cid = case.get("case_id", "(unnamed)")
        print(f"  Running {cid}...")
        result = run_case(case, providers, args.bench_bin, base_config,
                          dry=args.dry)
        results.append(result)

    print_verdict_matrix(results, providers, args.dry)
    print_aggregate(results, providers, args.dry)

    if args.recalibrate and not args.dry:
        print_recalibration_block(results, providers)

    # Exit code:
    # - --dry: 0 iff every case's bench-binary integration produced
    #          parseable stdout (lets CI use --dry as a smoke test).
    # - live: 0 iff no provider regressed below its baseline (when
    #         baselines are populated); 0 always when baselines are
    #         None (calibration mode).
    if args.dry:
        any_failed = any(r["bench_failed"] for r in results)
        return 1 if any_failed else 0
    for prov in providers:
        baseline = HEAD_BASELINE_FIX_COUNT.get(prov["name"])
        if baseline is None:
            continue
        passes = sum(1 for r in results
                     if r["providers"].get(prov["name"]) is True)
        if passes < baseline:
            sys.stderr.write(
                f"  {prov['name']}: regression — {passes} < baseline "
                f"{baseline}\n")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
