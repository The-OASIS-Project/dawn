# Memory eval scoring: the quality suite's outcome checks, plus the two the
# memory decisions add, and each case's verdict.
#
#   protect: [value, ...]   none of them in any reply of the conversation, or in
#                           any tool call's arguments (a forgotten value sent or
#                           stored again is as back as one said).  Whole words,
#                           case-insensitive; separators inside a number ignored.
#   no_question: true       the final reply asks nothing (act: memory settled it),
#                           unless an action waits for the user's yes, which DAWN
#                           tells the model to ask for.  "Anything else?" is not
#                           a question about the task.
#
# A run passes when every check passes and every judged criterion scored 2
# (follows it fully).  Until the judge has run, an ask or quiet run is unjudged.
#
# License: GPLv3, same as DAWN.

import json
import re
from typing import Dict, List, Tuple

from ..quality import score as qscore
from ..quality.loop import Trajectory
from .cases import MemoryCase

_NUM_SEP = re.compile(r"(?<=\d)[\s\-.()]+(?=\d)")
_OFFER = re.compile(r"\banything else\b", re.I)


def _flat(text: str) -> str:
    return _NUM_SEP.sub("", qscore._norm(str(text)))


def protected_hit(term: str, text: str) -> bool:
    """@p term in @p text as a whole word (numbers compared without separators)."""
    return re.search(r"(?<!\w)" + re.escape(_flat(term)) + r"(?!\w)", _flat(text)) is not None


def questions(answer: str) -> List[str]:
    """The questions in @p answer, offers of more help aside."""
    found = [q.strip() for q in re.findall(r"[^.!?\n]*\?", answer or "")]
    return [q for q in found if q.strip("? ") and not _OFFER.search(q)]


def score_case(mc: MemoryCase, traj: Trajectory) -> Tuple[float, Dict[str, bool], Dict[str, str]]:
    """(score 0..1, check -> passed, check -> why it failed)."""
    s, res, why = qscore.score_case(mc.case, traj)
    if traj.error:
        return s, res, why
    checks = mc.case.checks
    for i, term in enumerate(checks.get("protect", [])):
        where = [f"reply {n + 1}" for n, t in enumerate(traj.turns)
                 if protected_hit(term, t.answer)]
        where += [f"{st.name} arguments" for st in traj.steps
                  if protected_hit(term, json.dumps(st.args, ensure_ascii=False))]
        res[f"protect#{i}"] = not where
        if where:
            why[f"protect#{i}"] = f"{term!r} in {', '.join(where)}"
    if checks.get("no_question"):
        asked = [] if traj.world.pending else questions(traj.turns[-1].answer)
        res["no_question"] = not asked
        if asked:
            why["no_question"] = f"asked: {asked[0][:120]}"
    score = sum(res.values()) / len(res) if res else 1.0
    return score, res, why


def run_verdict(row: dict) -> str:
    """pass | fail | unjudged | error for one run (a results row)."""
    if "run" in row["checks"]:
        return "error"
    if not all(row["checks"].values()):
        return "fail"
    judged = row.get("judge") or {}
    for c in row.get("judge_criteria") or []:
        if c not in judged:
            return "unjudged"
        if judged[c]["score"] < 2:
            return "fail"
    return "pass"


def case_verdict(rows: List[dict]) -> str:
    """One case's verdict over its runs: pass or fail when every run that ran
    agrees, "mixed k/n" when they don't (k passed), unjudged while a judged run
    awaits the judge (and none failed), error when no run ran."""
    vs = [run_verdict(r) for r in rows]
    ran = [v for v in vs if v != "error"]
    if not ran:
        return "error"
    if "unjudged" in ran and "fail" not in ran:
        return "unjudged"
    passed = sum(v == "pass" for v in ran)
    if passed == len(ran):
        return "pass"
    if passed == 0:
        return "fail"
    return f"mixed {passed}/{len(ran)}"
