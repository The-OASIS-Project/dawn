# Reports: per-model summary and a paired comparison of two models.
#
# Statistics follow the design (docs/LLM_QUALITY_SUITE_DESIGN.md §3.7):
#   - the unit of resampling is the case: a case's score is its mean over runs,
#     and confidence intervals come from a bootstrap over cases;
#   - pass^k: the share of cases that passed in every one of k runs;
#   - the decision rule is fixed in advance: a model wins on quality only when
#     the 95% CI of the paired per-case difference excludes zero; otherwise
#     time-to-first-token p90 and cost per turn decide.
# Cost uses the measured token usage priced with OpenRouter's list prices
# (fetched once and stored in the report), never a hand-kept table.
#
# License: GPLv3, same as DAWN.

import json
import random
import re
import statistics
from collections import defaultdict
from typing import Dict, List, Optional, Tuple

import requests

from .score import split

BOOTSTRAP_SAMPLES = 4000
OPENROUTER_MODELS_URL = "https://openrouter.ai/api/v1/models"
_PRICES: Optional[dict] = None

# DAWN model id -> OpenRouter slug, where they differ in more than the vendor.
_SLUG_VENDORS = {"claude-": "anthropic/", "gpt-": "openai/", "gemini-": "google/"}


def _slug(model: str) -> str:
    """DAWN's model id as OpenRouter's slug (claude-haiku-5-5 -> anthropic/claude-haiku-5.5)."""
    model = re.sub(r"-\d{8}$", "", model)  # a dated snapshot id prices as its model
    for prefix, vendor in _SLUG_VENDORS.items():
        if model.startswith(prefix):
            name = model
            if vendor == "anthropic/":
                head, _, tail = name.rpartition("-")
                if head and tail.isdigit() and head[-1].isdigit():
                    name = f"{head}.{tail}"
            return vendor + name
    return model


def prices() -> dict:
    """slug -> {input, output, cache_read, cache_write} USD per token (live)."""
    global _PRICES
    if _PRICES is None:
        _PRICES = {}
        try:
            for m in requests.get(OPENROUTER_MODELS_URL, timeout=20).json().get("data", []):
                p = m.get("pricing", {})
                _PRICES[m["id"]] = {
                    "input": float(p.get("prompt") or 0),
                    "output": float(p.get("completion") or 0),
                    "cache_read": float(p.get("input_cache_read") or p.get("prompt") or 0),
                    "cache_write": float(p.get("input_cache_write") or p.get("prompt") or 0)}
        except (requests.RequestException, ValueError):
            pass
    return _PRICES


def cost_usd(price: Optional[dict], usage: Dict[str, int]) -> Optional[float]:
    if not price:
        return None
    return sum(usage.get(k, 0) * price[k] for k in ("input", "output", "cache_read", "cache_write"))


def _ok(rows: List[dict]) -> List[dict]:
    """Rows that ran: a run that still failed after retries is a run error,
    counted apart, never scored as the model's quality."""
    return [r for r in rows if "run" not in r["checks"]]


def _case_means(rows: List[dict]) -> Dict[str, float]:
    by = defaultdict(list)
    for r in _ok(rows):
        by[r["case"]].append(r["score"])
    return {c: statistics.mean(v) for c, v in by.items()}


def _bootstrap_ci(values: List[float], seed: int = 7) -> Tuple[float, float]:
    if not values:
        return (0.0, 0.0)
    rng = random.Random(seed)
    n = len(values)
    means = sorted(sum(values[rng.randrange(n)] for _ in range(n)) / n
                   for _ in range(BOOTSTRAP_SAMPLES))
    return means[int(0.025 * len(means))], means[int(0.975 * len(means)) - 1]


def _pct(xs: List[float], q: float) -> float:
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * len(xs)))] if xs else 0.0


def model_stats(entry: dict, price: Optional[dict], cases: Optional[set] = None) -> dict:
    """Stats over @p entry's rows (only @p cases when given)."""
    rows = [r for r in entry["rows"] if cases is None or r["case"] in cases]
    ok = _ok(rows)
    means = _case_means(rows)
    # pass^k over cases whose every run ran (a run error says nothing either way)
    runs_by_case, errored = defaultdict(list), {r["case"] for r in rows if "run" in r["checks"]}
    for r in ok:
        if r["case"] not in errored:
            runs_by_case[r["case"]].append(r["score"] == 1.0)
    groups = defaultdict(list)
    voice = []
    for r in ok:
        for kind, oks in split(r["checks"]).items():
            groups[kind].extend(oks)
        voice.extend(v["score"] / 2 for v in (r.get("judge") or {}).values())
    # A turn that never showed text counts its whole time: failing silently is
    # not fast.
    ttft = [t["ttft_s"] if t.get("ttft_s") is not None else t["total_s"]
            for r in ok for t in r["turns"] if t.get("requests")]
    turns = sum(len(r["turns"]) for r in ok) or 1
    costs = [cost_usd(price, r["usage"]) for r in ok]
    lo, hi = _bootstrap_ci(list(means.values()))
    unmocked = defaultdict(int)
    for r in rows:
        for u in r.get("unmocked", []):
            unmocked[u] += 1
    return {
        "cases": len(means), "score": statistics.mean(means.values()) if means else 0.0,
        "ci": (lo, hi),
        "pass_k": sum(all(v) for v in runs_by_case.values()) / (len(runs_by_case) or 1),
        "action": statistics.mean(groups["action"]) if groups["action"] else None,
        "answer": statistics.mean(groups["answer"]) if groups["answer"] else None,
        "voice": statistics.mean(voice) if voice else None,
        "ttft_p50": _pct(ttft, 0.5), "ttft_p90": _pct(ttft, 0.9),
        "cost_per_turn": (sum(costs) / turns) if costs and None not in costs else None,
        "errors": len(rows) - len(ok), "unmocked": dict(unmocked),
    }


def _price(results: dict, model: str) -> Optional[dict]:
    """The price stored with the results; live prices only for older files."""
    stored = results.get("prices", {}).get(model)
    return stored if stored else prices().get(_slug(model))


def summarize(results: dict) -> str:
    lines = [f"Clock {results['clock']}, {results['runs']} run(s) per case, "
             f"rubric v{results['rubric_version']}",
             "score/CI/pass^k/action/answer: deterministic checks over the runs that ran; "
             "voice: judge, valid only once `validate` shows kappa >= 0.6; err: run errors "
             "(excluded from quality)", "",
             f"{'model':24s} {'surface':13s} {'score':>6s} {'95% CI':>15s} {'pass^k':>7s} "
             f"{'action':>7s} {'answer':>7s} {'voice':>6s} {'ttft50':>7s} {'ttft90':>7s} "
             f"{'$/turn':>9s} err"]
    gaps = defaultdict(int)
    for entry in results["models"].values():
        s = model_stats(entry, _price(results, entry["model"]))
        fmt = lambda v: f"{v:.3f}" if v is not None else "  -  "
        cost = f"{s['cost_per_turn']:.5f}" if s["cost_per_turn"] is not None else "    ?"
        lines.append(f"{entry['model']:24s} {entry['surface']:13s} {s['score']:6.3f} "
                     f"[{s['ci'][0]:.3f},{s['ci'][1]:.3f}] {s['pass_k']:7.3f} "
                     f"{fmt(s['action']):>7s} {fmt(s['answer']):>7s} {fmt(s['voice']):>6s} "
                     f"{s['ttft_p50']:6.2f}s {s['ttft_p90']:6.2f}s {cost:>9s} {s['errors']}")
        for k, v in s["unmocked"].items():
            gaps[k] += v
    by_cat = defaultdict(dict)
    keys = list(results["models"])
    for key, entry in results["models"].items():
        cats = defaultdict(list)
        for r in _ok(entry["rows"]):
            cats[r["category"]].append(r["score"])
        for cat, v in cats.items():
            by_cat[cat][key] = statistics.mean(v)
    lines += ["", "per category: " + "  ".join(k.replace("|", "/") for k in keys)]
    for cat in sorted(by_cat):
        lines.append(f"  {cat:15s} " + "  ".join(
            f"{by_cat[cat][k]:.2f}" if k in by_cat[cat] else "  - " for k in keys))
    if gaps:
        lines += ["", "unmocked tool actions (mock gaps to review): " +
                  ", ".join(f"{k} x{v}" for k, v in sorted(gaps.items(), key=lambda x: -x[1]))]
    return "\n".join(lines)


def _entries(results_list: List[dict], name: str) -> Dict[str, Tuple[dict, dict]]:
    """surface -> (entry, results) for @p name: "model", or "N:model" to take it
    from the Nth results file (to compare one model before and after a change)."""
    files = list(enumerate(results_list))
    m = re.match(r"(\d+):(.+)", name)
    if m:
        idx, name = int(m.group(1)), m.group(2)
        files = [(i, r) for i, r in files if i == idx]
    out = {}
    for _i, res in files:
        for key, entry in res["models"].items():
            if name in (key, entry["model"]):
                if entry["surface"] in out:
                    raise SystemExit(f"{name!r} is in more than one results file; "
                                     f"pick one with N:{name}")
                out[entry["surface"]] = (entry, res)
    if not out:
        raise SystemExit(f"model {name!r} not in the results")
    return out


def compare(paths: List[str], a: str, b: str, seed: int = 7) -> str:
    """Paired per-case comparison of models @p a and @p b on each surface both
    ran, with the decision rule.  Time and cost are compared on the same cases."""
    res = [json.load(open(p)) for p in paths]
    ea_all, eb_all = _entries(res, a), _entries(res, b)
    out = []
    for surface in sorted(set(ea_all) & set(eb_all)):
        (ea, ra), (eb, rb) = ea_all[surface], eb_all[surface]
        ma, mb = _case_means(ea["rows"]), _case_means(eb["rows"])
        ha, hb = ra.get("case_hashes", {}), rb.get("case_hashes", {})
        changed = sorted(c for c in set(ma) & set(mb) if ha.get(c) != hb.get(c))
        if changed:
            out.append(f"  Not comparable: {len(changed)} case(s) changed between the runs "
                       f"({', '.join(changed[:5])}); rerun both on the same cases.")
            continue
        common = sorted(set(ma) & set(mb))
        out.append(f"== {surface}: {len(common)} shared cases ({a}: {len(ma)}, {b}: {len(mb)})")
        if not common or len(common) < 0.8 * max(len(ma), len(mb)):
            out.append("  Not comparable: the two runs share too few cases "
                       "(different --only, --model or run errors).")
            continue
        diffs = [mb[c] - ma[c] for c in common]
        mean = statistics.mean(diffs)
        lo, hi = _bootstrap_ci(diffs, seed)
        wins, losses = sum(d > 0 for d in diffs), sum(d < 0 for d in diffs)
        out.append(f"  {b} minus {a}: {mean:+.3f} (95% CI [{lo:+.3f}, {hi:+.3f}]); "
                   f"{b} better on {wins}, worse on {losses}")
        if lo > 0 or hi < 0:
            out.append(f"  Decision: {b if mean > 0 else a} is better on quality "
                       f"(the CI excludes zero).")
        else:
            sa = model_stats(ea, _price(ra, ea["model"]), set(common))
            sb = model_stats(eb, _price(rb, eb["model"]), set(common))
            out.append("  Decision: no significant quality difference; time and cost decide "
                       "(when they disagree, the choice is yours).")
            out.append(f"    time to first token p90: {a} {sa['ttft_p90']:.2f}s, "
                       f"{b} {sb['ttft_p90']:.2f}s")
            if sa["cost_per_turn"] is not None and sb["cost_per_turn"] is not None:
                out.append(f"    cost per turn: {a} ${sa['cost_per_turn']:.5f}, "
                           f"{b} ${sb['cost_per_turn']:.5f}")
        worst = [(d, c) for d, c in sorted(zip(diffs, common)) if d < 0][:5]
        if worst:
            out.append(f"  Largest losses for {b}: " + ", ".join(f"{c} {d:+.2f}" for d, c in worst))
    if not out:
        out.append("No surface was run for both models.")
    return "\n".join(out)
