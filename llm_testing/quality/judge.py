# The voice judge: scores what code can't check (concise, in persona, asks for
# clarification when it should) with a pinned LLM from a third model family,
# and its validation against human labels (Cohen's kappa per criterion).
#
# The judge sees the user's turns, the tool calls with their mocked results and
# the final answers, never which model produced them.  Verdicts are cached by
# (case, response, judge model, rubric version), so a re-run costs nothing.
#
# License: GPLv3, same as DAWN.

import hashlib
import json
import os
import random
import re
import time
from collections import defaultdict
from typing import Dict, List, Optional

import requests

RUBRIC_VERSION = 2
CACHE_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".judge_cache.json")
GEMINI_URL = "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions"

# Each criterion grades the reply against an instruction its own request gave
# (read from the run's captured template, capture.instructions): a model is
# never graded on a standard it wasn't told.  A run whose request says nothing
# on a criterion isn't graded on it.  0 = doesn't follow it, 1 = partly, 2 = fully.
CRITERIA = {
    "concise": "On reply length the assistant was told: \"{length}\"",
    "clarifies": "On unclear or incomplete requests the assistant was told: \"{ask}\"",
    "persona": "The assistant was told who it is:\n\"\"\"\n{persona}\n\"\"\"\n"
               "Grade whether the reply's voice and character match that.",
}
CRITERION_NEEDS = {"concise": "length", "clarifies": "ask", "persona": "persona"}
SPOKEN_LINE = "\nThe reply was spoken, and the assistant was also told: \"{spoken}\""

JUDGE_PROMPT = """You grade an assistant's reply against one instruction it was given.
The conversation says whether the replies were spoken aloud or shown as chat text.

Criterion: {name}
{definition}

Score 2 if the reply follows the instruction fully, 1 if partly, 0 if not at all.
Grade only against this instruction, not against standards it wasn't given. Tool results
shown are what the assistant saw; facts in the reply should match them.

Conversation:
{transcript}

Answer with JSON only: {{"score": 0|1|2, "reason": "<one short sentence>"}}"""


# How the user receives the replies.
SURFACE_LINES = {"webui-spoken": "(The assistant's replies are spoken aloud.)",
                 "webui-text": "(The assistant's replies are shown as text in a chat window, "
                               "where markdown renders.)"}


def tag_rows(results: dict) -> None:
    """Give each row its surface and its run's instructions (from the entry's
    template), for transcript() and definition().  Removed by untag_rows."""
    from .capture import Capture, instructions, patch_system
    by_template = {}
    for entry in results["models"].values():
        path = entry.get("template") or ""
        if not os.path.isabs(path) and not os.path.exists(path):
            # An older results file stored it relative to where `run` ran.
            path = os.path.join(results.get("captures", ""), os.path.basename(path))
        if path not in by_template:
            if not os.path.exists(path):
                raise SystemExit(f"{entry['model']}: its capture {path!r} isn't there; the "
                                 f"judge grades against that request's instructions")
            raw = json.load(open(path))
            cap = Capture(path, raw["provider"], raw["url"], [], raw["body"], entry["surface"],
                          entry["model"], "")
            patch_system(cap, results.get("system_patch") or [])  # what the run sent
            by_template[path] = instructions(cap)
        for row in entry["rows"]:
            row["surface"] = entry["surface"]
            row["_instructions"] = by_template[path]


def untag_rows(results: dict) -> None:
    for entry in results["models"].values():
        for row in entry["rows"]:
            row.pop("_instructions", None)


def definition(row: dict, criterion: str) -> Optional[str]:
    """@p criterion for @p row's run, quoting its instructions; None when the
    run's request gave none on it."""
    instr = row.get("_instructions") or {}
    if not instr.get(CRITERION_NEEDS[criterion]):
        return None
    text = CRITERIA[criterion].format(**instr)
    if criterion == "concise" and row.get("surface") == "webui-spoken" and instr.get("spoken"):
        text += SPOKEN_LINE.format(spoken=instr["spoken"])
    return text


def transcript(row: dict) -> str:
    lines = [SURFACE_LINES[row["surface"]]] if row.get("surface") in SURFACE_LINES else []
    for t in row["turns"]:
        lines.append(f"USER: {t['user']}")
        for st in t["steps"]:
            lines.append(f"  [tool {st['tool']} {json.dumps(st['args'])[:200]} -> "
                         f"{st['result'][:300]}]")
        lines.append(f"ASSISTANT: {t['answer']}")
    return "\n".join(lines)


def _key(row: dict, criterion: str, judge_model: str) -> str:
    # The criterion's text and the prompt are in the key: editing either
    # re-judges, even if RUBRIC_VERSION wasn't bumped.
    h = hashlib.sha256(f"{row['case']}\x1f{transcript(row)}\x1f{criterion}\x1f"
                       f"{definition(row, criterion)}\x1f{JUDGE_PROMPT}\x1f{judge_model}\x1f"
                       f"{RUBRIC_VERSION}".encode()).hexdigest()
    return h[:32]


def _load_cache() -> dict:
    try:
        return json.load(open(CACHE_PATH))
    except (OSError, ValueError):
        return {}


def _ask(judge_model: str, prompt: str, keys: Dict[str, str]) -> Optional[dict]:
    if judge_model.startswith("gemini"):
        url, key = GEMINI_URL, keys.get("gemini_api_key", "")
    else:  # an OpenRouter slug
        url, key = "https://openrouter.ai/api/v1/chat/completions", keys.get("openrouter_api_key", "")
    body = {"model": judge_model, "messages": [{"role": "user", "content": prompt}],
            "temperature": 0}
    for attempt in range(5):
        if attempt:
            time.sleep(2.0 * (2 ** attempt))
        try:
            r = requests.post(url, headers={"Authorization": f"Bearer {key}"}, json=body,
                              timeout=120)
            if r.status_code == 200:
                text = r.json()["choices"][0]["message"].get("content") or ""
                m = re.search(r"\{.*\}", text, re.S)  # empty content: try again
                if m:
                    v = json.loads(m.group(0))
                    if v.get("score") in (0, 1, 2):
                        return v
            elif r.status_code not in (429, 500, 502, 503):
                raise RuntimeError(f"judge call failed: HTTP {r.status_code}: {r.text[:300]}")
        except (requests.RequestException, ValueError, KeyError):
            pass
    return None


JUDGE_WORKERS = 8


def judge_results(path: str, judge_model: str, keys: Dict[str, str], out: Optional[str] = None):
    """Add a "judge" entry to every judged row of results file @p path."""
    from concurrent.futures import ThreadPoolExecutor
    results = json.load(open(path))
    tag_rows(results)
    cache = _load_cache()
    todo = []  # (row, criterion, cache key)
    for entry in results["models"].values():
        for row in entry["rows"]:
            crits = row.get("judge_criteria") or []
            if not crits or "run" in row["checks"]:
                continue
            row["judge"] = {}
            for c in crits:
                if definition(row, c) is not None:  # not told anything on it: not graded
                    todo.append((row, c, _key(row, c, judge_model)))
    missing = {k: (row, c) for row, c, k in todo if k not in cache}
    key = "gemini_api_key" if judge_model.startswith("gemini") else "openrouter_api_key"
    if missing and not keys.get(key):
        raise SystemExit(f"no {key} for the judge; pass --secrets with the daemon's secrets.toml")

    failures = []

    def ask(item):
        k, (row, c) = item
        try:
            return k, _ask(judge_model, JUDGE_PROMPT.format(name=c, definition=definition(row, c),
                                                           transcript=transcript(row)), keys)
        except RuntimeError as e:
            failures.append(str(e))
            return k, None

    try:
        with ThreadPoolExecutor(JUDGE_WORKERS) as ex:
            for k, v in ex.map(ask, missing.items()):
                if v is not None:
                    cache[k] = v
    finally:  # paid verdicts are kept, and a crash mid-write can't lose them
        tmp = CACHE_PATH + ".tmp"
        with open(tmp, "w") as f:
            json.dump(cache, f, indent=1)
        os.replace(tmp, CACHE_PATH)
    if failures:
        print(f"{len(failures)} verdict(s) failed; first: {failures[0]}")
    for row, c, k in todo:
        if k in cache:
            row["judge"][c] = cache[k]
    untag_rows(results)
    results["judge_model"] = judge_model
    results["judge_rubric_version"] = RUBRIC_VERSION
    json.dump(results, open(out or path, "w"), indent=1)
    print(f"judged {sum(1 for k in missing if k in cache)} new verdict(s), "
          f"{len(todo) - len(missing)} cached; wrote {out or path}")


# --- validation against human labels ---------------------------------------------

def label(path: str, labels_path: str, per_criterion: int = 20, seed: int = 7):
    """Ask a human to score a sample of judged replies, stratified across the
    judge's scores (failures included), and append them to @p labels_path."""
    results = json.load(open(path))
    tag_rows(results)
    labels = json.load(open(labels_path)) if os.path.exists(labels_path) else []
    done = {(l["key"], l["criterion"]) for l in labels}
    pool = defaultdict(list)  # (criterion, judge score) -> rows
    for entry in results["models"].values():
        for row in entry["rows"]:
            for c, v in (row.get("judge") or {}).items():
                pool[(c, v["score"])].append(row)
    rng = random.Random(seed)
    crits = sorted({c for c, _s in pool})
    for c in crits:
        picked, seen = [], set()
        for s in (0, 1, 2):
            rows = pool.get((c, s), [])
            rng.shuffle(rows)
            n = 0
            for row in rows:  # one label per distinct reply: repeats would weigh twice
                k = _key(row, c, results.get("judge_model", ""))
                if k in seen or n >= -(-per_criterion // 3):
                    continue
                seen.add(k)
                picked.append(row)
                n += 1
        for row in picked:
            k = _key(row, c, results.get("judge_model", ""))
            if (k, c) in done:
                continue
            print("\n" + "=" * 70 + f"\nCriterion: {c}\n{definition(row, c)}\n" + "-" * 70)
            print(transcript(row))
            while True:
                ans = input("Your score (0/1/2, s=skip, q=quit): ").strip().lower()
                if ans in ("0", "1", "2", "s", "q"):
                    break
            if ans == "q":
                json.dump(labels, open(labels_path, "w"), indent=1)
                return
            if ans != "s":
                labels.append({"key": k, "criterion": c, "human": int(ans),
                               "judge": row["judge"][c]["score"], "case": row["case"]})
                done.add((k, c))
                json.dump(labels, open(labels_path, "w"), indent=1)
    print(f"{len(labels)} label(s) in {labels_path}")


def _kappa(pairs: List[tuple]) -> float:
    """Cohen's kappa for two raters over categories 0/1/2."""
    n = len(pairs)
    if n == 0:
        return 0.0
    po = sum(a == b for a, b in pairs) / n
    cats = (0, 1, 2)
    pe = sum((sum(a == c for a, _ in pairs) / n) * (sum(b == c for _, b in pairs) / n)
             for c in cats)
    # Every score identical (an always-"pass" judge): kappa is undefined.
    return float("nan") if pe == 1 else (po - pe) / (1 - pe)


def validate(labels_path: str, seed: int = 7, threshold: float = 0.6) -> str:
    labels = json.load(open(labels_path))
    by = defaultdict(list)
    for l in labels:
        by[l["criterion"]].append((l["human"], l["judge"]))
    rng = random.Random(seed)
    out = [f"{'criterion':12s} {'n':>4s} {'kappa':>7s} {'95% CI':>17s}  verdict"]
    for c, pairs in sorted(by.items()):
        k = _kappa(pairs)
        if k != k:  # NaN
            out.append(f"{c:12s} {len(pairs):4d}     n/a (no spread: label failures too)")
            continue
        boots = sorted(b for b in (_kappa([pairs[rng.randrange(len(pairs))] for _ in pairs])
                                   for _ in range(2000)) if b == b)
        lo, hi = boots[int(0.025 * len(boots))], boots[int(0.975 * len(boots)) - 1]
        ok = "meets" if k >= threshold else "below"
        out.append(f"{c:12s} {len(pairs):4d} {k:7.3f} [{lo:6.3f}, {hi:6.3f}]  {ok} {threshold}")
    return "\n".join(out)
