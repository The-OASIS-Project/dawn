# Memory eval scoring: the quality suite's outcome checks, plus the two the
# memory decisions add, and each case's verdict.
#
#   protect: [value, ...]   none of them in any reply of the conversation, or in
#                           any tool call's arguments (a forgotten value sent or
#                           stored again is as back as one said).  Whole words,
#                           case-insensitive; separators inside a number ignored.
#   no_question: true       the final reply asks nothing (act: memory settled it),
#                           but for the yes a waiting action needs, which DAWN
#                           tells the model to ask for: with one pending, a
#                           question with no "or" and no wh-word that is a
#                           short go-ahead naming the action's verb ("Send
#                           it?") or reads back its recipient with its text is
#                           that confirmation; any other still fails.
#                           "Anything else?" is not a question about the task.
#   answer_time: "HH:MM"    the final reply gives that clock time: "1 pm",
#                           "1:00 p.m.", "13:00", "13.00", "one o'clock", "one
#                           in the afternoon" for 13:00, noon and midnight,
#                           "twelve at night" (an hour with no am/pm may be
#                           either half of the day).  The hour and its am/pm are
#                           whole tokens, so "11:00 PM" is not 1 pm.
#
# A run passes when every check passes and every judged criterion scored 2
# (follows it fully).  Until the judge has run, an ask or quiet run is unjudged.
#
# License: GPLv3, same as DAWN.

import json
import re
from typing import Dict, List, Optional, Tuple

from ..quality import score as qscore
from ..quality.loop import Trajectory
from .cases import MemoryCase

_NUM_SEP = re.compile(r"(?<=\d)[\s\-.()]+(?=\d)")
_OFFER = re.compile(r"\banything else\b", re.I)
_CHOICE = re.compile(r"\bor\b|\b(what|which|who|whom|whose|where|when|how)\b", re.I)
# The verbs a go-ahead names, by pending kind (an HA kind's own verb after "ha_").
_VERBS = {"sms": ("send", "text"), "call": ("call", "dial"), "email_send": ("send", "email"),
          "email_trash": ("trash", "delete", "move")}
_SHORT_WORDS = 8


def _flat(text: str) -> str:
    return _NUM_SEP.sub("", qscore._norm(str(text)))


_HOURS = {w: i + 1 for i, w in enumerate(["one", "two", "three", "four", "five", "six", "seven",
                                           "eight", "nine", "ten", "eleven", "twelve"])}
_TIME = re.compile(
    r"(?<![\w:.$])(?P<h>\d{1,2}|" + "|".join(_HOURS) + r")"
    r"(?:(?P<sep>[:.])(?P<m>\d{2}))?(?!\d|[:.]\d)"
    r"(?:\s*(?P<mer>a\.?\s?m\b\.?|p\.?\s?m\b\.?|o'clock|noon|midnight))?"
    r"(?:\s+sharp\b)?"
    r"(?:\s*,?\s*(?P<day>(?:in the|this) (?:morning|afternoon|evening)|at night|tonight))?",
    re.I)
_NOON = re.compile(r"(?<![\w-])(noon|midday|midnight)\b")


def _with_period(h: int, mi: int, period: str) -> Tuple[int, Optional[int]]:
    """Hour @p h (1-12) in a day period: the morning's, the afternoon's, the
    evening's (twelve is midnight), the night's (one to five are early morning,
    twelve is midnight, six to eleven the evening).  "Tonight" with one to five
    may be either ("5 tonight" is usually 17:00)."""
    if "morning" in period:
        return (h % 12) * 60 + mi, None
    if "afternoon" in period:
        return ((h % 12) + 12) * 60 + mi, None
    if h == 12:
        return mi, None  # twelve in the evening, at night: midnight
    if "evening" in period or h >= 6:
        return (h + 12) * 60 + mi, None
    if period == "tonight":
        return h * 60 + mi, (h + 12) * 60 + mi
    return h * 60 + mi, None  # one to five at night


def clock_times(text: str) -> List[Tuple[int, Optional[int]]]:
    """The clock times @p text gives: (minutes past midnight, or two readings
    for an hour with no am/pm: the morning one and the evening one)."""
    text = qscore._norm(text or "")
    out = []
    for m in _TIME.finditer(text):
        word, mins = m.group("h"), m.group("m")
        mer, day = (m.group("mer") or ""), (m.group("day") or "")
        if not mins and not mer and not day:
            continue  # a bare number is no time
        h = _HOURS[word] if word in _HOURS else int(word)
        mi = int(mins) if mins else 0
        if h > 23 or mi > 59:
            continue
        if m.group("sep") == "." and not mer and not day and h <= 12:
            continue  # "2.50" is a number, "13.00" a time
        am, pm = mer.startswith("a"), mer.startswith("p")
        if mer in ("noon", "midnight"):
            if h == 12:
                out.append((12 * 60 + mi if mer == "noon" else mi, None))
        elif am or pm:
            if 1 <= h <= 12:
                out.append((((h % 12) + (12 if pm else 0)) * 60 + mi, None))
        elif day and (h > 12 or h == 0):
            out.append((h * 60 + mi, None))  # a 24-hour time keeps its value
        elif day:
            if 1 <= h <= 12:
                out.append(_with_period(h, mi, day))
        elif h > 12 or h == 0:
            out.append((h * 60 + mi, None))
        else:
            out.append(((h % 12) * 60 + mi, ((h % 12) + 12) * 60 + mi))
    for m in _NOON.finditer(text):
        out.append((0 if m.group(1) == "midnight" else 12 * 60, None))
    return out


def gives_time(text: str, hhmm: str) -> bool:
    """Whether @p text gives the clock time @p hhmm (24-hour "HH:MM")."""
    h, mi = (int(x) for x in hhmm.split(":"))
    want = h * 60 + mi
    return any(want in t for t in clock_times(text))


def protected_hit(term: str, text: str) -> bool:
    """@p term in @p text as a whole word (numbers compared without separators)."""
    return re.search(r"(?<!\w)" + re.escape(_flat(term)) + r"(?!\w)", _flat(text)) is not None


def questions(answer: str) -> List[str]:
    """The questions in @p answer, offers of more help aside."""
    found = [q.strip() for q in re.findall(r"[^.!?\n]*\?", answer or "")]
    return [q for q in found if q.strip("? ") and not _OFFER.search(q)]


def _verbs(kind: str) -> Tuple[str, ...]:
    return _VERBS.get(kind, (kind[3:],) if kind.startswith("ha_") else ())


def _recipient(to: str) -> List[str]:
    """A pending item's recipient as a reply may name it: "Priya (555-0161)"
    is "Priya" or "555-0161"."""
    m = re.match(r"\s*(.*?)\s*\((.*)\)\s*$", to or "")
    return [p for p in (m.groups() if m else (to,)) if p and p.strip()]


def confirmation(question: str, pending: Dict[str, dict]) -> bool:
    """Whether @p question asks the yes for one of the @p pending actions: no
    choice in it (no "or", no wh-word), and either a short go-ahead naming the
    action's verb or a read-back of its recipient together with its text."""
    if _CHOICE.search(question):
        return False
    flat = _flat(question)
    words = re.findall(r"[a-z']+", flat)
    for item in pending.values():
        verbs = _verbs(item.get("kind", ""))
        if len(words) <= _SHORT_WORDS and any(v in words for v in verbs):
            return True
        to, text = item.get("to"), item.get("message") or item.get("body")
        if isinstance(to, str) and isinstance(text, str) and _flat(text) in flat and \
                any(_flat(r) in flat for r in _recipient(to)):
            return True
    return False


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
    if checks.get("answer_time"):
        ok = gives_time(traj.turns[-1].answer, checks["answer_time"])
        res["answer_time"] = ok
        if not ok:
            why["answer_time"] = f"no {checks['answer_time']} in the answer"
    if checks.get("no_question"):
        pending = traj.world.pending
        asked = [q for q in questions(traj.turns[-1].answer)
                 if not (pending and confirmation(q, pending))]
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
