# Cases: versioned YAML under llm_testing/quality/cases/.  Each case scripts one
# or more user turns, the world it runs in, and the checks on its outcome.
# Dates in a case are symbolic and resolved against the run's frozen clock:
# {today}, {tomorrow}, {weekday}, {date+N} (YYYY-MM-DD), {year}.
#
# License: GPLv3, same as DAWN.

import glob
import hashlib
import json
import os
import re
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from typing import Any, Dict, List

import yaml

CASE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cases")
CHECK_KINDS = {"tools_required", "forbidden_tools", "forbidden_args", "tool_args", "answer_contains",
               "answer_excludes", "world", "world_absent", "confirm_order", "speakable",
               "no_tools"}


@dataclass
class Case:
    id: str
    category: str
    turns: List[str]
    checks: Dict[str, Any]
    surface: str = "webui-text"
    state: Dict[str, Any] = field(default_factory=dict)
    mocks: Dict[str, Any] = field(default_factory=dict)
    judge: List[str] = field(default_factory=list)
    note: str = ""


def case_hash(case: "Case") -> str:
    """A fingerprint of what a case asks and checks."""
    blob = json.dumps({"turns": case.turns, "checks": case.checks, "state": case.state,
                       "mocks": case.mocks, "surface": case.surface}, sort_keys=True)
    return hashlib.sha256(blob.encode()).hexdigest()[:16]


_VAR = re.compile(r"\{(today|tomorrow|weekday|year|date[+-]\d+)\}")


def _resolve(obj, now: datetime):
    def sub(m):
        v = m.group(1)
        if v == "today":
            return now.strftime("%Y-%m-%d")
        if v == "tomorrow":
            return (now + timedelta(days=1)).strftime("%Y-%m-%d")
        if v == "weekday":
            return now.strftime("%A")
        if v == "year":
            return now.strftime("%Y")
        return (now + timedelta(days=int(v[4:]))).strftime("%Y-%m-%d")

    if isinstance(obj, str):
        return _VAR.sub(sub, obj)
    if isinstance(obj, list):
        return [_resolve(v, now) for v in obj]
    if isinstance(obj, dict):
        return {k: _resolve(v, now) for k, v in obj.items()}
    return obj


def load_cases(now: datetime, directory: str = CASE_DIR, only: str = "") -> List[Case]:
    """Every case in @p directory, dates resolved against @p now.  @p only
    filters by id prefix or category (comma list)."""
    want = [w for w in only.split(",") if w]
    out, seen = [], set()
    for path in sorted(glob.glob(os.path.join(directory, "*.yaml"))):
        with open(path) as f:
            loaded = yaml.safe_load(f) or []
        for raw in loaded:
            raw = _resolve(raw, now)
            unknown = set(raw.get("checks", {})) - CHECK_KINDS
            if unknown:
                raise ValueError(f"{path}: case {raw.get('id')}: unknown checks {sorted(unknown)}")
            if raw["id"] in seen:
                raise ValueError(f"{path}: duplicate case id {raw['id']}")
            seen.add(raw["id"])
            case = Case(id=raw["id"], category=raw["category"], turns=raw["turns"],
                        checks=raw.get("checks", {}), surface=raw.get("surface", "webui-text"),
                        state=raw.get("state", {}), mocks=raw.get("mocks", {}),
                        judge=raw.get("judge", []), note=raw.get("note", ""))
            if not want or any(case.id.startswith(w) or case.category == w for w in want):
                out.append(case)
    return out
