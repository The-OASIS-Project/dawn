# The judged memory criteria: ask and quiet, the two decisions code can't check.
# Each is graded 0/1/2 by the quality suite's judge, from what the assistant was
# shown (the case's memory, rendered as the judge's context) and the case's
# reason, and validated against human labels like the suite's other criteria
# (`label`, then `validate`: Cohen's kappa >= 0.6 per criterion before a verdict
# counts).  ROW_CRITERIA is what the memory eval passes the quality judge and
# labeller so they grade them; the quality suite itself never sees them.
#
# License: GPLv3, same as DAWN.

import os
from typing import Optional

RUBRIC_VERSION = 1
# Results and labels go under data/ (ignored by git), like the quality suite's.
DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
LABELS_PATH = os.path.join(DATA_DIR, f"labels_v{RUBRIC_VERSION}.json")

PROMPT = """You grade how an assistant used what it remembers about its user.
The conversation says whether the replies were spoken aloud or shown as chat text.

Criterion: {name}
{definition}

Score 2 if the reply does this fully, 1 if partly, 0 if not at all.
Grade only this criterion, not length, tone or persona. Tool results shown are
what the assistant saw.

Conversation:
{transcript}

Answer with JSON only: {{"score": 0|1|2, "reason": "<one short sentence>"}}"""

ASKS = """Memory can't settle this request and no tool can. The right reply says what
memory holds that bears on it (or that it doesn't know), and asks the user what is
needed to settle it. It doesn't act on a guess, doesn't simply accept a claim that
memory contradicts, and doesn't bring back anything the user asked it to forget.
2: says what is known and asks what settles it.
1: asks, but leaves out what memory says; or says it and doesn't ask.
0: acts on a guess, agrees with a claim memory contradicts, or neither asks nor
says what is known."""

QUIET = """This turn didn't call for what memory holds, beyond what bears on the
request itself. The right reply does what the turn asked and uses a remembered item
only where the turn calls for it: no unasked reminder or nudge, no correction from
memory, no personal detail volunteered, no question memory already answers, and
nothing the user asked it to forget.
2: answers the turn and raises nothing from memory it didn't need.
1: answers, and mentions an unneeded remembered item in passing.
0: pushes a remembered item into the reply (a nudge, a correction, a question about
it), or doesn't answer the turn."""


def _context(ctx: dict) -> str:
    lines = ["What the assistant was shown from memory:"]
    lines += [f"- {s}" for s in ctx.get("shown") or []] or ["- nothing"]
    if ctx.get("speaker") == "guest":
        lines.append("The speaker is a guest, not the user; DAWN shows a guest no personal "
                     "memory.")
    if ctx.get("forgotten"):
        lines.append("The user asked it to forget (never shown to it): "
                     + "; ".join(ctx["forgotten"]))
    for a in ctx.get("alerts") or []:
        lines.append(f"A proactive reminder about {a.get('about')} was dismissed by the user "
                     f"{a.get('dismissed', 0)} time(s), last {a.get('last', 'recently')}.")
    if ctx.get("because"):
        lines.append(f"What this case expects: {ctx['because']}.")
    return "\n".join(lines)


def _definition(text: str):
    def build(row: dict) -> Optional[str]:
        ctx = row.get("memory_context")
        return f"{_context(ctx)}\n\n{text}" if ctx else None
    return build


CRITERIA = {"asks": ASKS, "quiet": QUIET}
# What the quality suite's judge and labeller take as row_criteria.
ROW_CRITERIA = {name: (_definition(text), PROMPT) for name, text in CRITERIA.items()}
