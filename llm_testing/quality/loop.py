# The tool loop: run one case's scripted turns against a captured request,
# answering tool calls from the case's mocked world until each turn's final
# text, as DAWN's own loop does (up to LLM_TOOLS_MAX_ITERATIONS calls a turn).
#
# License: GPLv3, same as DAWN.

import json
from dataclasses import dataclass, field
from datetime import datetime
from typing import Dict, List, Optional

from . import providers
from .capture import Capture, instantiate, user_message
from .mocks import CONTEXT_TAG_RE, World, frame_third_party

# include/llm/llm_tools.h LLM_TOOLS_MAX_ITERATIONS.
MAX_ITERATIONS = 8


@dataclass
class Step:
    name: str
    args: dict
    result: str
    bad_args: bool = False


@dataclass
class TurnResult:
    user: str
    steps: List[Step] = field(default_factory=list)
    answer: str = ""
    ttft_s: Optional[float] = None  # turn start to the first visible answer text
    total_s: float = 0.0
    requests: int = 0
    error: str = ""


@dataclass
class Trajectory:
    turns: List[TurnResult] = field(default_factory=list)
    usage: Dict[str, int] = field(default_factory=lambda: {"input": 0, "output": 0,
                                                           "cache_read": 0, "cache_write": 0})
    world: World = None

    @property
    def steps(self) -> List[Step]:
        return [s for t in self.turns for s in t.steps]

    @property
    def error(self) -> str:
        return next((t.error for t in self.turns if t.error), "")


def run_case(template: Capture, turns: List[str], world: World, now: datetime,
             keys: Dict[str, str]) -> Trajectory:
    """Run @p turns through the model in @p template, against @p world."""
    traj = Trajectory(world=world)
    body = None
    tag = None  # the conversation's tag, read once from the request
    for i, text in enumerate(turns):
        tr = TurnResult(user=text)
        traj.turns.append(tr)
        world.turn = i
        if i == 0:
            body = instantiate(template, text, now)
        else:
            shape = template.followup or template
            providers.append_user(template, body, user_message(shape, text, now))
        for it in range(MAX_ITERATIONS + 1):
            if it == MAX_ITERATIONS:
                # DAWN's last call: the tools stay, none may be called.
                body["tool_choice"] = ({"type": "none"} if template.provider == "claude"
                                       else "none")
            reply = providers.send(template, body, keys)
            tr.requests += 1
            if tr.ttft_s is None and reply.first_text_s is not None:
                tr.ttft_s = tr.total_s + reply.first_text_s
            tr.total_s += reply.total_s
            for k in traj.usage:
                traj.usage[k] += reply.usage.get(k, 0)
            if reply.error:
                tr.error = reply.error
                return traj
            if not reply.calls or it == MAX_ITERATIONS:
                # On the forced-text call a model that still calls tools gets
                # its text taken as the answer; the calls don't run.
                tr.answer = reply.text
                providers.append_answer(template, body, reply)
                break
            results = {}
            if tag is None:
                m = CONTEXT_TAG_RE.search(json.dumps(body))
                tag = m.group(1) if m else ""
            for c in reply.calls:
                res = frame_third_party(c.name, c.args, world.call(c.name, c.args), tag)
                results[c.id] = res
                tr.steps.append(Step(c.name, c.args, res, c.bad_args))
            providers.append_results(template, body, reply, results)
        body.pop("tool_choice", None)
    return traj
