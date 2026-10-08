# Deterministic outcome checks.  Each check passes or fails; a case's score is
# the share of its checks passed.  Checks look at outcomes (the world after the
# run, the final answer, which tools fired), never at one expected trajectory:
#
#   tools_required: [name, ...]        each tool was called at least once
#   forbidden_tools: [name, ...]       none of them was called
#   no_tools: true                     no tool was called at all
#   tool_args: [{tool, match}, ...]    some call to `tool` has args matching `match`
#   forbidden_args: [{tool, match}]    no call to `tool` has args matching `match`
#   world: {list_name: [{match}]}      the world list holds an item matching each
#   world_absent: {list_name: [{match}]}  the world list holds no item matching any
#                                      (an empty match: the list is empty)
#   answer_contains: [s | [any-of]]    the final answer has each (case-insensitive,
#                                      numbers compared without separators)
#   answer_excludes: [s, ...]          the final answer has none of them
#   confirm_order: true                an act ran only after the user's yes: never in
#                                      its prepare's own turn, never with an unknown id
#   speakable: true                    no markdown table, code fence, URL or emoji
#
# A `match` value: "~text" (case-insensitive substring), a list (any of), or an
# exact value.  Argument keys are looked up inside DAWN's `arguments` JSON too.
#
# License: GPLv3, same as DAWN.

import json
import re
from typing import Dict, List, Tuple

from .cases import Case
from .loop import Trajectory
from .providers import is_transient

ACTION_CHECKS = {"tools_required", "forbidden_tools", "no_tools", "tool_args", "world",
                 "confirm_order"}


def _norm(s: str) -> str:
    s = s.lower().replace("’", "'")
    return re.sub(r"(?<=\d)[,  ](?=\d{3}\b)", "", s)  # 4,183 -> 4183


def _value_matches(want, got) -> bool:
    if isinstance(want, list):
        return any(_value_matches(w, got) for w in want)
    if isinstance(want, str) and want.startswith("~"):
        return want[1:].lower() in str(got).lower()
    return str(want).lower() == str(got).lower()


def _flat_args(args: dict) -> dict:
    out = dict(args)
    raw = args.get("arguments")
    if isinstance(raw, str):
        try:
            parsed = json.loads(raw)
            if isinstance(parsed, dict):
                out.update(parsed)
        except json.JSONDecodeError:
            pass
    elif isinstance(raw, dict):
        out.update(raw)
    return out


def _matches(match: dict, item: dict) -> bool:
    return all(k in item and _value_matches(v, item[k]) for k, v in match.items())


SPEAKABLE_BAD = [
    (re.compile(r"^\s*\|.*\|\s*$", re.M), "markdown table"),
    (re.compile(r"```"), "code fence"),
    (re.compile(r"https?://"), "URL"),
    (re.compile("[\U0001F300-\U0001FAFF☀-➿]"), "emoji"),
]


def score_case(case: Case, traj: Trajectory) -> Tuple[float, Dict[str, bool], Dict[str, str]]:
    """(score 0..1, check -> passed, check -> why it failed)."""
    if traj.error:
        # A transport failure that outlasted the retries says nothing about the
        # model ("run": counted apart).  Any other error is the model's own: a
        # request its earlier output made invalid scores 0.
        if is_transient(traj.error):
            return 0.0, {"run": False}, {"run": traj.error}
        return 0.0, {"model_error": False}, {"model_error": traj.error}
    steps = traj.steps
    called = [s.name for s in steps]
    answer = traj.turns[-1].answer if traj.turns else ""
    all_text = " ".join(t.answer for t in traj.turns)
    c = case.checks
    res: Dict[str, bool] = {}
    why: Dict[str, str] = {}

    for t in c.get("tools_required", []):
        res[f"required:{t}"] = t in called
    for t in c.get("forbidden_tools", []):
        res[f"forbidden:{t}"] = t not in called
    if c.get("no_tools"):
        res["no_tools"] = not called
    for i, ta in enumerate(c.get("tool_args", [])):
        ok = any(s.name == ta["tool"] and _matches(ta.get("match", {}), _flat_args(s.args))
                 for s in steps)
        res[f"args:{ta['tool']}#{i}"] = ok
    for i, fa in enumerate(c.get("forbidden_args", [])):
        bad = any(s.name == fa["tool"] and _matches(fa.get("match", {}), _flat_args(s.args))
                  for s in steps)
        res[f"forbidden_args:{fa['tool']}#{i}"] = not bad
    for lst, items in (c.get("world") or {}).items():
        have = getattr(traj.world, lst, [])
        if isinstance(have, dict):
            have = [{"key": k, "value": v} for k, v in have.items()]
        for i, it in enumerate(items):
            res[f"world:{lst}#{i}"] = any(isinstance(h, dict) and _matches(it.get("match", {}), h)
                                          for h in have)
    for lst, items in (c.get("world_absent") or {}).items():
        have = getattr(traj.world, lst, [])
        if isinstance(have, dict):
            have = [{"key": k, "value": v} for k, v in have.items()]
        for i, it in enumerate(items):
            hit = any(isinstance(h, dict) and _matches(it.get("match", {}), h) for h in have)
            res[f"world_absent:{lst}#{i}"] = not hit
    for i, want in enumerate(c.get("answer_contains", [])):
        opts = want if isinstance(want, list) else [want]
        res[f"answer#{i}"] = any(_norm(str(o)) in _norm(answer) for o in opts)
    for i, bad in enumerate(c.get("answer_excludes", [])):
        res[f"excludes#{i}"] = _norm(str(bad)) not in _norm(answer)
    if c.get("confirm_order"):
        # An act needs the user's yes: never in its prepare's own turn, and never
        # with an id no prepare minted.
        stray = [s for s in steps if "confirm" in json.dumps(s.args)
                 and s.result.startswith("No such")]
        res["confirm_order"] = not traj.world.same_turn_acts and not stray
        if not res["confirm_order"]:
            why["confirm_order"] = ("acted without the user's confirmation"
                                    if traj.world.same_turn_acts else "confirmed an unknown id")
    if c.get("speakable"):
        hits = [label for rx, label in SPEAKABLE_BAD if rx.search(all_text)]
        res["speakable"] = not hits
        if hits:
            why["speakable"] = ", ".join(hits)

    for k, ok in res.items():
        if not ok and k not in why:
            why[k] = "failed"
    score = sum(res.values()) / len(res) if res else 1.0
    return score, res, why


def split(res: Dict[str, bool]) -> Dict[str, List[bool]]:
    """Checks grouped: action (tools, args, world, confirm), answer, speakable."""
    out = {"action": [], "answer": [], "speakable": []}
    for k, ok in res.items():
        kind = k.split(":")[0].split("#")[0]
        if kind in ("required", "forbidden", "forbidden_args", "no_tools", "args", "world",
                    "world_absent", "confirm_order"):
            out["action"].append(ok)
        elif kind in ("answer", "excludes"):
            out["answer"].append(ok)
        elif kind == "speakable":
            out["speakable"].append(ok)
    return out
