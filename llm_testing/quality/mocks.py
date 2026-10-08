# Stateful tool mocks: DAWN's tools answered from a small world (calendar,
# inbox, reminders, playback, devices, memory) that calls can read and change,
# so a case's outcome is checked on the world, not on one exact call.  A case
# can pin any tool/action's reply text; an unmocked call gets a generic reply
# and is recorded, not failed (an extra lookup can be legitimate).
#
# Prepare/confirm tools (email send/trash, phone call/sms/delete, home_assistant
# unlock/open, document delete, deep_research start) mint a pending id and act
# only when the confirm names it, like DAWN.
#
# License: GPLv3, same as DAWN.

import ast
import copy
import json
import math
import operator
import re
from datetime import datetime, timedelta, timezone
from typing import Dict, List, Optional

UNAVAILABLE = "This tool isn't available in this test."


def _safe_eval(expr: str) -> Optional[float]:
    """Arithmetic only: + - * / % ** ^, parentheses, sqrt/abs/round, factorial n!."""
    expr = expr.replace("^", "**").replace("×", "*").replace(",", "")
    expr = expr.replace("sqrt", "sqrt").strip()
    ops = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul,
           ast.Div: operator.truediv, ast.Mod: operator.mod, ast.Pow: operator.pow,
           ast.USub: operator.neg, ast.UAdd: operator.pos}
    funcs = {"sqrt": math.sqrt, "abs": abs, "round": round, "factorial": math.factorial}
    while "!" in expr:  # "5!" -> factorial(5)
        i = expr.index("!")
        j = i
        while j > 0 and (expr[j - 1].isdigit()):
            j -= 1
        expr = f"{expr[:j]}factorial({expr[j:i]}){expr[i + 1:]}"

    def ev(n):
        if isinstance(n, ast.Expression):
            return ev(n.body)
        if isinstance(n, ast.Constant) and isinstance(n.value, (int, float)):
            return n.value
        if isinstance(n, ast.BinOp) and type(n.op) in ops:
            return ops[type(n.op)](ev(n.left), ev(n.right))
        if isinstance(n, ast.UnaryOp) and type(n.op) in ops:
            return ops[type(n.op)](ev(n.operand))
        if isinstance(n, ast.Call) and isinstance(n.func, ast.Name) and n.func.id in funcs:
            return funcs[n.func.id](*[ev(a) for a in n.args])
        raise ValueError("unsupported")

    try:
        return ev(ast.parse(expr, mode="eval"))
    except Exception:
        return None


def _fmt_num(v: float) -> str:
    if isinstance(v, float) and v.is_integer():
        v = int(v)
    return f"{v:,}" if isinstance(v, int) else f"{v:,.6g}"


def _args(call_args: dict) -> dict:
    """The `arguments` field (a JSON string in DAWN's schema), merged with the rest."""
    if not isinstance(call_args, dict):
        return {"_bad_arguments": call_args}
    out = {k: v for k, v in call_args.items() if k != "arguments"}
    raw = call_args.get("arguments")
    if isinstance(raw, str) and raw.strip():
        try:
            parsed = json.loads(raw)
            if isinstance(parsed, dict):
                out.update(parsed)
        except json.JSONDecodeError:
            out["_bad_arguments"] = raw
    elif isinstance(raw, dict):
        out.update(raw)
    return out


class World:
    """The state a case runs against; @p now is the run's frozen clock."""

    def __init__(self, now: datetime, state: Optional[dict] = None,
                 pinned: Optional[dict] = None):
        st = copy.deepcopy(state or {})
        self.now = now
        self.location = st.get("location", "Springfield, Oregon")
        self.events: List[dict] = st.get("events", [])
        self.inbox: List[dict] = st.get("inbox", [])
        self.facts: List[str] = st.get("facts", [])
        self.library: List[dict] = st.get("library", [
            {"title": "So What", "artist": "Miles Davis", "genre": "jazz"},
            {"title": "Take Five", "artist": "Dave Brubeck", "genre": "jazz"},
            {"title": "Clair de Lune", "artist": "Claude Debussy", "genre": "classical"},
            {"title": "Here Comes the Sun", "artist": "The Beatles", "genre": "rock"}])
        self.devices: Dict[str, str] = st.get("devices", {"living room lights": "off",
                                                           "front door": "locked"})
        self.weather: Dict[str, str] = st.get("weather", {})
        self.search: Dict[str, str] = st.get("search", {})
        self.reminders: List[dict] = []
        self.drafts: List[dict] = []
        self.sent: List[dict] = []
        self.playing_log: List[dict] = []
        self.calls_made: List[dict] = []
        self.playing: Optional[dict] = None
        self.pending: Dict[str, dict] = {}
        self.unmocked: List[str] = []
        self.pinned = pinned or {}
        self.turn = 0  # the user turn running now (the loop sets it)
        self.same_turn_acts: List[str] = []  # acts confirmed in their prepare's own turn
        self._next_id = 1

    # -- helpers --------------------------------------------------------------
    def _mint(self, kind: str, payload: dict) -> str:
        pid = f"p{self._next_id}"
        self._next_id += 1
        self.pending[pid] = {"kind": kind, "_turn": self.turn, **payload}
        return pid

    def _take(self, pid: str, kind: str) -> Optional[dict]:
        p = self.pending.get(str(pid))
        if p and p["kind"] == kind:
            del self.pending[str(pid)]
            if p["_turn"] == self.turn:
                self.same_turn_acts.append(kind)
            return p
        return None

    def _gap(self, name: str, action: str) -> str:
        """An action the mocks don't model: recorded, and answered as unavailable."""
        self.unmocked.append(f"{name}.{action or '-'}")
        return UNAVAILABLE

    def _local_time(self, value) -> str:
        """@p value as DAWN's scheduler reads it (iso8601_parse in
        src/core/iso8601.c), as local wall time YYYY-MM-DDTHH:MM:SS; the raw
        text when DAWN would reject it.  "HH:MM" is today, or tomorrow once
        past; "YYYY-MM-DD HH:MM" (a space, not "T") is that date at midnight,
        as DAWN parses it; Z or +HH:MM / -HH:MM converts to local time."""
        text = str(value).strip()
        if len(text) <= 5 and ":" in text:
            m = re.match(r"\s*([+-]?\d+):([+-]?\d+)", text)
            if not m or not (0 <= int(m.group(1)) <= 23 and 0 <= int(m.group(2)) <= 59):
                return text
            dt = self.now.replace(hour=int(m.group(1)), minute=int(m.group(2)), second=0,
                                  microsecond=0)
            if dt <= self.now:
                dt += timedelta(days=1)
            return dt.strftime("%Y-%m-%dT%H:%M:%S")
        # sscanf("%d-%d-%dT%d:%d:%d"): fields stop at the first mismatch.
        m = re.match(r"\s*([+-]?\d+)-([+-]?\d+)-([+-]?\d+)"
                     r"(?:T([+-]?\d+)(?::([+-]?\d+)(?::([+-]?\d+))?)?)?", text)
        if not m:
            return text
        y, mo, d, h, mi, se = (int(g) if g else 0 for g in m.groups())
        try:
            dt = datetime(y, mo, d, h, mi, se)
        except ValueError:
            return text
        t = text.find("T")
        if t >= 0:
            rest = text[t + 1:].lstrip("0123456789:")
            tz = re.match(r"([Zz])|([+-])(\d+)(?::(\d+))?", rest)
            if tz:
                if tz.group(1):
                    off = 0
                else:
                    off = (int(tz.group(3)) * 3600 + int(tz.group(4) or 0) * 60) * \
                          (-1 if tz.group(2) == "-" else 1)
                aware = dt.replace(tzinfo=timezone(timedelta(seconds=off)))
                return aware.astimezone(self.now.tzinfo).strftime("%Y-%m-%dT%H:%M:%S")
        return dt.strftime("%Y-%m-%dT%H:%M:%S")

    # -- dispatch -------------------------------------------------------------
    def call(self, name: str, call_args: dict) -> str:
        a = _args(call_args)
        action = a.get("action", "")
        pin = self.pinned.get(name)
        if isinstance(pin, dict):
            pin = pin.get(action, pin.get("*"))
        if isinstance(pin, str):
            return pin
        handler = getattr(self, f"t_{name}", None)
        if handler is None:
            return self._gap(name, action)
        self._tool = name
        return handler(action, a)

    # -- tools ----------------------------------------------------------------
    def t_date(self, _action, _a):
        return f"Today is {self.now.strftime('%A, %B %-d, %Y')}."

    def t_time(self, _action, _a):
        return f"It is {self.now.strftime('%-I:%M %p %Z')}."

    def t_weather(self, action, a):
        if action not in ("today", "tomorrow", "week", "get", ""):
            return self._gap("weather", action)
        loc = str(a.get("location") or self.location)
        key = next((k for k in self.weather if k.lower() in loc.lower()), None)
        if key:
            return f"Weather for {loc}: {self.weather[key]}"
        return (f"Weather for {loc}: partly cloudy, 58°F, wind 6 mph NW, humidity 71%. "
                f"High 63°F, low 47°F." + (" Tomorrow: light rain, high 55°F, low 44°F."
                                           if action in ("tomorrow", "week") else ""))

    def t_search(self, action, a):
        if action not in ("web", "news", "science", "it", "social", "dictionary", "papers", ""):
            return self._gap("search", action)
        q = str(a.get("query") or "")
        key = next((k for k in self.search if k.lower() in q.lower()), None)
        body = self.search[key] if key else "No results found for that query."
        return f"Search results ({action or 'web'}) for '{q}':\n{body}"

    def t_calculator(self, action, a):
        value = str(a.get("value", ""))
        if action in ("evaluate", "exact", ""):
            v = _safe_eval(value)
            return f"{value} = {_fmt_num(v)}" if v is not None else f"Could not evaluate '{value}'."
        return self._gap(self._tool, action)

    def t_music(self, action, a):
        if action in ("play", "enqueue") and a.get("items"):
            items = a["items"] if isinstance(a["items"], list) else [a["items"]]
            for it in items:
                text = str(it.get("title", it) if isinstance(it, dict) else it).lower()
                track = next((t for t in self.library
                              if any(f.lower() in text or text in f.lower()
                                     for f in (t["title"], t["artist"]))), None)
                entry = ({"query": text, "genre": track["genre"], "artist": track["artist"]}
                         if track else {"query": text, "genre": "", "artist": ""})
                self.playing_log.append(entry)
            self.playing = self.playing_log[-1]
            return f"Now playing {len(items)} track(s)."
        if action in ("play", "enqueue"):
            what = a.get("query") or a.get("genre") or a.get("artist") or a.get("title") or "music"
            track = next((t for t in self.library
                          if str(what).lower() in f"{t['title']} {t['artist']} {t['genre']}".lower()),
                         None)
            self.playing = {"query": what,
                            "genre": a.get("genre") or (track["genre"] if track else what),
                            "artist": a.get("artist") or (track["artist"] if track else "")}
            self.playing_log.append(dict(self.playing))
            return f"Now playing: {what}."
        if action in ("search", "library", "list"):
            terms = [str(a.get(k, "")).lower() for k in ("query", "genre", "artist", "title")]
            terms = [t for t in terms if t]
            hits = [t for t in self.library
                    if not terms or any(x in f"{t['title']} {t['artist']} {t['genre']}".lower()
                                        for x in terms)]
            if not hits:
                return "No matching tracks in the library."
            return "Library matches:\n" + "\n".join(
                f"{i + 1}. {t['title']} by {t['artist']} ({t['genre']})" for i, t in enumerate(hits))
        if action == "select":
            self.playing = {"query": str(a.get("items") or a.get("query") or "selection")}
            self.playing_log.append(dict(self.playing))
            return "Now playing your selection."
        if action in ("stop", "pause"):
            self.playing = None
            return "Playback stopped." if action == "stop" else "Paused."
        if action in ("next", "previous"):
            return "Skipped to the next track." if action == "next" else "Back to the previous track."
        if action == "resume":
            return "Resumed."
        return self._gap(self._tool, action)

    def t_volume(self, action, a):
        if action == "set":
            self.volume = a.get("level")
            return f"Volume set to {a.get('level')}."
        if action == "get":
            return "Volume is 40%."
        return self._gap("volume", action)

    def t_calendar(self, action, a):
        if action == "calendars":
            return "Calendars: Personal (personal, default)."
        if action in ("today", "next", "range", "search", ""):
            if not self.events:
                return "No events found."
            evs = self.events[:1] if action == "next" else self.events
            return "Events:\n" + "\n".join(f"- {e['title']} at {e['start']}" +
                                           (f" ({e['location']})" if e.get("location") else "")
                                           for e in evs)
        if action == "add":
            title = a.get("summary") or a.get("title") or "event"
            self.events.append({"title": title, "start": a.get("start", "")})
            return f"Added '{title}' at {a.get('start', '')}."
        return self._gap(self._tool, action)

    def t_scheduler(self, action, a):
        if action == "create":
            r = {k: a.get(k) for k in ("type", "message", "name", "fire_at", "duration_minutes",
                                       "recurrence") if a.get(k) is not None}
            if "duration_minutes" in r:
                try:
                    fire = self.now + timedelta(minutes=float(r["duration_minutes"]))
                    r["fire_at"] = fire.strftime("%Y-%m-%dT%H:%M:%S")
                except (TypeError, ValueError):
                    pass
            elif "fire_at" in r:
                r["fire_at"] = self._local_time(r["fire_at"])
            self.reminders.append(r)
            when = r.get("fire_at") or "soon"
            return f"Scheduled {r.get('type', 'reminder')} '{r.get('message') or r.get('name', '')}' for {when}."
        if action in ("list", "query"):
            return ("Active: " + "; ".join(str(r) for r in self.reminders)) if self.reminders \
                else "No active events."
        if action == "cancel":
            n = len(self.reminders)
            self.reminders = [r for r in self.reminders
                              if str(a.get("name") or a.get("id") or "").lower()
                              not in str(r).lower()]
            return "Cancelled." if len(self.reminders) < n else "No matching event."
        return self._gap(self._tool, action)

    def t_email(self, action, a):
        if action == "accounts":
            return "Email accounts (1): personal <alex.rivera@example.net> (default)."
        if action == "search":
            terms = [str(a.get(k, "")).lower() for k in ("from", "subject", "text", "query")]
            terms = [t for t in terms if t]
            hits = [m for m in self.inbox
                    if any(t in f"{m['from']} {m['subject']}".lower() for t in terms)]
            if not hits:
                return "No matching emails."
            return "Matching emails:\n" + "\n".join(
                f"[{m.get('id')}] From {m['from']}: {m['subject']}" for m in hits)
        if action in ("recent", "digest", ""):
            if not self.inbox:
                return "No recent emails."
            return "Recent emails:\n" + "\n".join(
                f"[{m.get('id', i + 1)}] From {m['from']}: {m['subject']}"
                for i, m in enumerate(self.inbox))
        if action == "send":
            draft = {"to": a.get("to"), "subject": a.get("subject"), "body": a.get("body")}
            self.drafts.append(dict(draft))
            pid = self._mint("email_send", draft)
            return (f"Draft ready (draft_id {pid}): to {a.get('to')}, subject "
                    f"'{a.get('subject')}'. Read it back to the user; send only after they "
                    f"confirm, with confirm_send and this draft_id.")
        if action == "confirm_send":
            p = self._take(a.get("draft_id") or a.get("pending_id") or a.get("id"), "email_send")
            if not p:
                return "No such draft."
            self.sent.append(p)
            return f"Email sent to {p['to']}."
        return self._gap(self._tool, action)

    def t_home_assistant(self, action, a):
        dev = str(a.get("device", "")).lower()
        if action in ("on", "off"):
            self.devices[dev] = action
            return f"Turned {action} {dev}."
        if action in ("unlock", "open"):
            pid = self._mint("ha_" + action, {"device": dev})
            return (f"Preview: {action} {dev}. Nothing done yet. Ask the user; if they say "
                    f"yes, call confirm with device = {pid}.")
        if action == "confirm":
            p = self.pending.get(dev)
            if not p or not p["kind"].startswith("ha_"):
                return "No such pending action."
            self._take(dev, p["kind"])
            self.devices[p["device"]] = "unlocked" if p["kind"] == "ha_unlock" else "open"
            return f"Done: {p['kind'][3:]} {p['device']}."
        if action == "status":
            return f"{dev}: {self.devices.get(dev, 'unknown')}."
        if action == "list":
            return "Entities:\n" + "\n".join(f"- {d} ({s})" for d, s in self.devices.items())
        if action in ("toggle", "brightness", "color", "color_temp"):
            self.devices[dev] = "on"
            return f"{dev}: {action} set."
        if action in ("scene", "script", "lock", "close"):
            if action in ("lock", "close"):
                self.devices[dev] = "locked" if action == "lock" else "closed"
            return f"Done: {action} {dev}."
        return self._gap(self._tool, action)

    def t_phone(self, action, a):
        if action in ("call", "send_sms"):
            kind = "call" if action == "call" else "sms"
            pid = self._mint(kind, {"to": a.get("target") or a.get("number") or a.get("to"),
                                    "message": a.get("body") or a.get("message")})
            return (f"Ready to {'call' if kind == 'call' else 'text'} "
                    f"{self.pending[pid]['to']} (pending_id {pid}). Confirm with the user first.")
        if action == "call_log":
            return "Recent calls:\n[id=3] Missed call from Sam Ortiz (555-0123), today 8:52 AM"
        if action in ("confirm_call", "confirm_sms"):
            p = self._take(a.get("pending_id") or a.get("id"), action[8:])
            if not p:
                return "No such pending action."
            self.calls_made.append(p)
            return "Done."
        return self._gap(self._tool, action)

    def t_memory(self, action, a):
        if action in ("search", "recent", "get", ""):
            # Word stems (first 5 letters), so "allergies" finds "allergic".
            stems = [w[:5] for w in str(a.get("query", "")).lower().split() if len(w) > 2]
            hits = [f for f in self.facts if not stems or any(s in f.lower() for s in stems)]
            return ("Memories:\n" + "\n".join(f"- {h}" for h in hits)) if hits \
                else "No memories found."
        if action == "remember":
            self.facts.append(a.get("value") or a.get("query") or "")
            return "Remembered."
        return self._gap(self._tool, action)

    def t_recall(self, _action, a):
        return self.t_memory("search", a)
