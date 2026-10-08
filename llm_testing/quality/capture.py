# Captured requests: load a capture set (the files `dawn-admin llm capture`
# wrote plus scripts/quality_capture.py's manifest) and turn one into a request
# for a new question at a fixed time.  Only the question, the turn's time line
# and `stream` change; everything else goes out as DAWN sent it.
#
# License: GPLv3, same as DAWN.

import copy
import glob
import json
import os
import re
from dataclasses import dataclass, field
from datetime import datetime
from typing import Dict, List, Optional
from zoneinfo import ZoneInfo

# DAWN's per-turn time line (prompt_turn_head() in src/core/prompt_sections.c).
TIME_LINE_RE = re.compile(r"\[system_time\] Current time: [^\n]*")
TIME_LINE_FMT = ("[system_time] Current time: {human} (ISO: {iso}{offset}).  This timestamp is "
                 "fresh as of this turn; use it for relative-time computations and tool args "
                 "like `fire_at`.  The `time` tool is only needed for sub-second precision.")


def time_line(now: datetime) -> str:
    """The time line DAWN writes for @p now (an aware datetime)."""
    off = now.strftime("%z")
    off = f"{off[:3]}:{off[3:]}" if len(off) == 5 else "Z"
    return TIME_LINE_FMT.format(human=now.strftime("%A, %Y-%m-%d %H:%M %Z"),
                                iso=now.strftime("%Y-%m-%dT%H:%M:%S"), offset=off)


def clock(iso: str, tz: str) -> datetime:
    """A fixed run time: @p iso local time in zone @p tz."""
    return datetime.fromisoformat(iso).replace(tzinfo=ZoneInfo(tz))


@dataclass
class Capture:
    path: str
    provider: str  # "claude", "openai-chat", "openai-responses"
    url: str
    headers: List[str]
    body: dict
    surface: str
    model: str
    question: str  # the user text this request was captured with
    turns: List[str] = field(default_factory=list)  # the conversation's scripted turns
    # A later turn's request of the same model and surface: the shape of turns
    # after the first (DAWN sends later turns without the first's memory block).
    followup: Optional["Capture"] = None


def _strings(obj):
    """Every string inside @p obj (dicts and lists walked)."""
    if isinstance(obj, str):
        yield obj
    elif isinstance(obj, dict):
        for v in obj.values():
            yield from _strings(v)
    elif isinstance(obj, list):
        for v in obj:
            yield from _strings(v)


def messages_key(provider: str) -> str:
    """The body field holding the conversation."""
    return "input" if provider == "openai-responses" else "messages"


def _last_user(body: dict, provider: str) -> Optional[dict]:
    for msg in reversed(body.get(messages_key(provider), [])):
        if isinstance(msg, dict) and msg.get("role") == "user":
            return msg
    return None


def load_capture_set(directory: str) -> List[Capture]:
    """The first request of every captured turn, in capture order.

    Requests are matched to the manifest's turns by the turn text appearing in
    the request's last user message; a tool-loop follow-up request (same turn,
    no new user text) is skipped."""
    files = sorted(glob.glob(os.path.join(directory, "[0-9][0-9][0-9]-*.json")))
    manifest = [json.loads(line) for line in open(os.path.join(directory, "manifest.jsonl"))
                if line.strip()]
    caps, fi = [], 0
    for entry in manifest:
        for turn in entry["turns"]:
            while fi < len(files):
                raw = json.load(open(files[fi]))
                fi += 1
                last = _last_user(raw["body"], raw["provider"])
                if last and any(turn in s for s in _strings(last)):
                    caps.append(Capture(files[fi - 1], raw["provider"], raw["url"], raw["headers"],
                                        raw["body"], entry["surface"], entry["model"], turn,
                                        list(entry["turns"])))
                    break
            else:
                raise ValueError(f"no captured request for turn {turn!r} ({entry['model']})")
    return caps


def first_turns(caps: List[Capture]) -> List[Capture]:
    """The templates: each captured conversation's first turn, each with a
    later-turn request of its model and surface attached when one was captured."""
    later = {}
    for c in caps:
        if c.question != c.turns[0]:
            later.setdefault((c.model, c.surface), c)
    out = []
    for c in caps:
        if c.question == c.turns[0]:
            c.followup = later.get((c.model, c.surface))
            out.append(c)
    return out


def _replace_strings(obj, fn):
    if isinstance(obj, str):
        return fn(obj)
    if isinstance(obj, dict):
        return {k: _replace_strings(v, fn) for k, v in obj.items()}
    if isinstance(obj, list):
        return [_replace_strings(v, fn) for v in obj]
    return obj


def user_message(template: Capture, question: str, now: datetime) -> dict:
    """A user turn shaped like the capture's: its question and time line replaced.

    Each must be replaced exactly once: if DAWN's message format drifts, a run
    must fail rather than ask the captured question at the capture's time."""
    msg = copy.deepcopy(_last_user(template.body, template.provider))
    line = time_line(now)
    counts = {"time": 0, "question": 0}

    def swap(s):
        s, n = TIME_LINE_RE.subn(lambda _m: line, s)
        counts["time"] += n
        counts["question"] += s.count(template.question)
        return s.replace(template.question, question)

    out = _replace_strings(msg, swap)
    if counts != {"time": 1, "question": 1}:
        raise ValueError(f"{template.path}: expected one time line and one question in the "
                         f"user turn, found {counts}")
    return out


def instantiate(template: Capture, question: str, now: datetime) -> dict:
    """The captured request, asking @p question at @p now, streamed."""
    body = copy.deepcopy(template.body)
    msgs = body[messages_key(template.provider)]
    for i in range(len(msgs) - 1, -1, -1):
        if isinstance(msgs[i], dict) and msgs[i].get("role") == "user":
            msgs[i] = user_message(template, question, now)
            break
    body["stream"] = True
    if template.provider == "openai-chat":
        body.setdefault("stream_options", {})["include_usage"] = True
    return body


# ── The instructions a captured request gave (what the judge grades against) ──

IDENTITY_HEAD = "## Your Identity\n"
IDENTITY_END = "\n\nIMPORTANT: Use the identity above."
NOTE_LABEL_RE = re.compile(r"^\[Operator note[^\]]*\] ")
RULES_RE = re.compile(r"(?m)^RULES\n")  # the RULES heading on a line of its own
SPOKEN_MARKERS = ("read aloud by text-to-speech", "read to the user by text-to-speech")


def system_text(body: dict, provider: str) -> str:
    """The request's system prompt as one string."""
    if provider == "openai-responses":
        return body.get("instructions") or ""
    if provider == "claude":
        sys_ = body.get("system") or ""
        return sys_ if isinstance(sys_, str) else "\n".join(b.get("text", "") for b in sys_)
    msgs = body.get("messages") or []
    if msgs and msgs[0].get("role") == "system" and isinstance(msgs[0].get("content"), str):
        return msgs[0]["content"]
    return ""


def _rules(sys_text: str) -> List[str]:
    """The RULES block's rules, each as one line of text without its number."""
    parts = RULES_RE.split(sys_text, 1)
    rules: List[str] = []
    for line in (parts[1] if len(parts) > 1 else "").split("\n"):
        m = re.match(r"^\d+\. (.*)", line)
        if m:
            rules.append(m.group(1))
        elif line.startswith("   ") and rules:
            rules[-1] += " " + line.strip()
        else:
            break  # the block ends at its first line that isn't a rule
    return rules


def instructions(cap: Capture) -> Dict[str, Optional[str]]:
    """What @p cap's request told the model about who it is, reply length,
    unclear requests and (on a spoken surface) how to speak.  A key is None
    when the request said nothing about it: the judge doesn't grade that."""
    sys_text = system_text(cap.body, cap.provider)
    if IDENTITY_HEAD in sys_text:  # a replace-mode persona: up to its old notice or the rules
        persona = sys_text.split(IDENTITY_HEAD, 1)[1].split(IDENTITY_END, 1)[0]
        persona = RULES_RE.split(persona, 1)[0]
    else:
        persona = RULES_RE.split(sys_text, 1)[0]
        persona = "\n".join(l for l in persona.split("\n")
                            if not l.startswith("When writing a mathematical factorial"))
    rules = _rules(sys_text)
    length = next((r for r in rules if "concise" in r or r.startswith("Match the length")), None)
    ask = next((r for r in rules if "ambiguous" in r or "missing something you need" in r), None)
    spoken = None
    for s in _strings(cap.body):
        for para in s.split("\n\n"):
            if any(m in para for m in SPOKEN_MARKERS):
                spoken = NOTE_LABEL_RE.sub("", para.strip())
    return {"persona": persona.strip() or None, "length": length, "ask": ask, "spoken": spoken}


def patch_system(cap: Capture, pairs: List[List[str]], done: Optional[set] = None) -> None:
    """Replace each [old, new] in @p cap's system prompt (and its follow-up's),
    in place: an A/B of prompt wording on the same captures.  Each old text
    must occur exactly once, or the run would test a prompt nobody wrote.
    @p done holds the captures already patched (a follow-up is shared)."""
    done = set() if done is None else done
    for c in (cap, cap.followup):
        if c is None or id(c) in done or not pairs:
            continue
        done.add(id(c))
        def swap(text: str) -> str:
            for old, new in pairs:
                if text.count(old) != 1:
                    raise ValueError(f"{c.path}: {old[:60]!r} occurs {text.count(old)} times "
                                     f"in the system prompt, not once")
                text = text.replace(old, new)
            return text
        if c.provider == "openai-responses":
            c.body["instructions"] = swap(c.body.get("instructions") or "")
        elif c.provider == "claude":
            sys_ = c.body.get("system")
            if isinstance(sys_, str):
                c.body["system"] = swap(sys_)
            elif isinstance(sys_, list) and sys_:
                # Each old text must sit whole in one block, once in the prompt.
                for old, new in pairs:
                    counts = [b.get("text", "").count(old) for b in sys_]
                    if sum(counts) != 1:
                        raise ValueError(f"{c.path}: {old[:60]!r} occurs {sum(counts)} times "
                                         f"within one system block, not once")
                    b = sys_[counts.index(1)]
                    b["text"] = b["text"].replace(old, new)
            else:
                raise ValueError(f"{c.path}: no system prompt to patch")
        else:
            msgs = c.body.get("messages") or []
            if not msgs or msgs[0].get("role") != "system":
                raise ValueError(f"{c.path}: no system message to patch")
            msgs[0]["content"] = swap(msgs[0]["content"])
