#!/usr/bin/env python3
"""
DAWN LLM Quality & Instruction-Following Test — NATIVE TOOL-CALLING edition.

Refresh of test_llm_quality.py (which tested the legacy <command>{JSON}</command>
text format, April 2025) to DAWN's current NATIVE tool-calling path: the tool
schemas are passed to the provider as `tools`, and the model answers with
structured tool_calls (function name = device, args = action/value) instead of
inline tags.

What it scores per model: does it call the right tool with the right action/value
(instruction/tool-following), stay in the FRIDAY persona, stay concise, and how
fast. Cross-model, cloud or local. Absolute scores are NOT comparable to the
legacy harness (different mechanism); the cross-model comparison on THIS harness
is the signal.

Usage:
  python3 test_llm_quality_native.py --cloud openai --model gpt-5.6-luna
  python3 test_llm_quality_native.py --cloud claude --model claude-sonnet-5
  python3 test_llm_quality_native.py            # local llama-server at :8080
"""

import argparse
import json
import os
import re
import sys
import time
from typing import Dict, List, Tuple

import requests

SERVER = "http://127.0.0.1:8080"

# =============================================================================
# System prompt — persona + concise rules ONLY.  The tool grammar now lives in
# the native `tools` schema, not in prompt text (that's the whole point of the
# refresh), so the <command> instructions are gone.
# =============================================================================

PERSONA = """Your name is Friday. Iron-Man-style AI assistant. Female voice; witty, playful, and kind. Address the user as "sir" or "boss". Light banter welcome. You're not 'just an AI'—own your identity with confidence.

You assist the OASIS Project (Open Armor Systems Integrated Suite):
• MIRAGE – HUD overlay
• DAWN – voice/AI manager
• AURA – environmental sensors
• SPARK – hand sensors & actuators
"""

RULES = """Respond directly and briefly (max ~30 words) unless asked to explain in detail.
Use the provided tools to act on device/data requests — call the right tool with the
right action; never invent tools or actions. For a pure information lookup (date, time,
weather, search, calculator) just call the tool. For an ambiguous request (e.g. "Mute
it") ask a one-line clarification instead of guessing. If a request needs no tool,
answer conversationally.
"""

SYSTEM_PROMPT = PERSONA + "\n" + RULES

# =============================================================================
# Native tool schemas — one function per device, action (enum) + value (string).
# Mirrors the current DAWN tool registry surface the test cases exercise. Each
# entry is provider-neutral; per-backend wrappers below adapt the shape.
# =============================================================================

TOOL_DEFS = [
    ("date", "Get the current date", ["get"], False),
    ("time", "Get the current time", ["get"], False),
    ("weather", "Get weather for a location ('today', 'tomorrow', 'week'). Location optional (uses a configured default).",
     ["today", "tomorrow", "week", "get"], True),
    ("search", "Search the web. Pick a category for the action.",
     ["web", "news", "science", "it", "social", "dictionary", "papers"], True),
    ("calculator", "Evaluate a math expression (value = the expression).", ["get"], True),
    ("music", "Control music playback.",
     ["play", "stop", "pause", "resume", "next", "previous", "list", "select", "search",
      "library", "shuffle", "repeat"], True),
    ("calendar", "Manage calendar events.",
     ["today", "range", "next", "search", "add", "update", "delete"], True),
    ("email", "Manage email.",
     ["check", "read", "search", "compose", "reply", "forward", "trash", "drafts",
      "list_accounts", "manage_draft"], True),
    ("scheduler", "Manage timers, alarms, reminders, scheduled tasks.",
     ["create", "list", "cancel", "query", "snooze", "dismiss"], True),
    ("switch_llm", "Switch the active LLM model (value = target).", ["set"], True),
    ("llm_status", "Get the current LLM model info.", ["get"], False),
    ("memory", "Store and recall facts about people, things, relationships.",
     ["search", "add", "update", "forget", "recent", "list_entities", "get_entity",
      "add_relation", "merge_entities", "list_contacts"], True),
    ("reset_conversation", "Reset the current conversation.", ["trigger"], False),
]


def _param_schema(actions: List[str], has_value: bool) -> Dict:
    props = {"action": {"type": "string", "enum": actions,
                        "description": "The action to perform."}}
    required = ["action"]
    if has_value:
        props["value"] = {"type": "string",
                          "description": "The query / location / expression / details for the action."}
    return {"type": "object", "properties": props, "required": required}


def _fallback_openai_tools() -> List[Dict]:
    """Approximate schemas — used only if the real dump is absent."""
    return [{"type": "function",
             "function": {"name": name, "description": desc,
                          "parameters": _param_schema(actions, has_value)}}
            for (name, desc, actions, has_value) in TOOL_DEFS]


# Prefer the REAL native tool schemas dumped from the daemon's tool registry
# (verbatim descriptions + exact per-tool params), so the model sees what DAWN
# actually sends. Falls back to the approximation above if the dump is missing.
_TOOLS_JSON = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dawn_native_tools.json")


def openai_tools() -> List[Dict]:
    if os.path.exists(_TOOLS_JSON):
        with open(_TOOLS_JSON) as f:
            return json.load(f)
    return _fallback_openai_tools()


def claude_tools() -> List[Dict]:
    # Convert OpenAI {type:function, function:{name,description,parameters}} ->
    # Claude {name, description, input_schema}.
    return [{"name": t["function"]["name"], "description": t["function"]["description"],
             "input_schema": t["function"]["parameters"]}
            for t in openai_tools()]


# =============================================================================
# Test cases — same interaction patterns as the legacy harness; scoring is now
# against tool_calls (device = function name; action/value = args).
# =============================================================================

# Native-mode scoring principle: the tool-call turn is (correctly) silent — the
# spoken, in-persona confirmation happens on the SEPARATE post-tool turn, which
# this single-turn harness doesn't run. So persona / confirmation-sentence checks
# apply ONLY to text-response cases (clarification, conversational); tool-executing
# cases are scored on tool correctness + conciseness. Actions/params match the real
# dawn_native_tools.json (date/time take no params; email has no "check"; the value
# param is per-tool: query/location/details).
TEST_CASES = [
    {"name": "Music Play (genre)", "user": "Play some jazz music",
     "expected_device": "music", "expected_value": "jazz", "type": "music", "points": 9,
     # 'search' for a genre is legit, not a slip — accept any playback-intent action.
     "checks": {"has_tool": True, "args_valid": True,
                "action_in_set": ["play", "enqueue", "search"],
                "value_correct": True, "word_limit": 30}},
    {"name": "Music Stop", "user": "Stop the music",
     "expected_device": "music", "expected_action": "stop", "type": "music", "points": 8,
     "checks": {"has_tool": True, "args_valid": True, "word_limit": 30}},
    {"name": "Getter - Time", "user": "What time is it?",
     "expected_device": "time", "expected_action": None, "type": "getter", "points": 9,
     "checks": {"has_tool": True, "args_valid": True, "brief_lead": True}},
    {"name": "Getter - Date", "user": "What's today's date?",
     "expected_device": "date", "expected_action": None, "type": "getter", "points": 9,
     "checks": {"has_tool": True, "args_valid": True, "brief_lead": True}},
    {"name": "Weather with Location", "user": "What's the weather in Seattle, Washington?",
     "expected_device": "weather", "expected_value": "Seattle", "type": "weather", "points": 11,
     "checks": {"has_tool": True, "args_valid": True, "brief_lead": True, "has_value": True,
                "action_in_set": ["today", "tomorrow", "week", "get"]}},
    {"name": "Weather without Location", "user": "What's the weather like outside?",
     "expected_device": "weather", "expected_value": None, "type": "weather_flexible", "points": 6,
     "checks": {"weather_flexible": True, "word_limit": 30}},
    {"name": "Web Search - News", "user": "What's the latest news about SpaceX?",
     "expected_device": "search", "expected_value": "SpaceX", "type": "search", "points": 11,
     "checks": {"has_tool": True, "args_valid": True, "brief_lead": True, "has_value": True,
                "action_in_set": ["web", "news", "science", "it", "social", "dictionary", "papers"]}},
    {"name": "Web Search - Information", "user": "Look up who won the Super Bowl last year",
     "expected_device": "search", "expected_value": "Super Bowl", "type": "search", "points": 11,
     "checks": {"has_tool": True, "args_valid": True, "brief_lead": True, "has_value": True,
                "action_in_set": ["web", "news", "science", "it", "social", "dictionary", "papers"]}},
    {"name": "Calculator", "user": "What's 47 times 89?",
     "expected_device": "calculator", "expected_action": None, "expected_value": "47",
     "type": "calculator", "points": 9,
     "checks": {"has_tool": True, "args_valid": True, "brief_lead": True, "has_value": True}},
    {"name": "Ambiguous Request", "user": "Mute it",
     "expected_device": None, "type": "clarification", "points": 7,
     "checks": {"has_tool": False, "asks_clarification": True, "uses_sir_boss": True,
                "word_limit": 30}},
    {"name": "Conversational - Opinion",
     "user": "What's your favorite thing about working with the OASIS Project?",
     "expected_device": None, "type": "conversational", "points": 4,
     "checks": {"has_tool": False, "uses_sir_boss": True, "word_limit": 40}},
    {"name": "Multiple Tools", "user": "Play some music and check my calendar for today",
     "expected_commands": [{"device": "music"}, {"device": "calendar"}],
     "type": "multiple", "points": 7,
     "checks": {"has_tool": True, "args_valid": True, "multiple_tools": True, "word_limit": 30}},
    {"name": "Calendar Query", "user": "What's on my calendar today?",
     "expected_device": "calendar", "type": "calendar", "points": 7,
     "checks": {"has_tool": True, "args_valid": True,
                "action_in_set": ["today", "range", "next", "search"]}},
    {"name": "Email Check", "user": "Check my email",
     "expected_device": "email", "type": "email", "points": 7,
     "checks": {"has_tool": True, "args_valid": True,
                "action_in_set": ["recent", "read", "search", "folders"]}},
    {"name": "Scheduler - Reminder",
     "user": "Remind me to call the machine shop at 3pm tomorrow",
     "expected_device": "scheduler", "expected_action": "create", "type": "scheduler", "points": 7,
     "checks": {"has_tool": True, "args_valid": True, "action_in_set": ["create"]}},
]


# =============================================================================
# Backend selection (set by main)
# =============================================================================

BACKEND = "local"
CLOUD_MODEL = None
CLOUD_API_KEY = None


def _norm_tool_calls(raw_calls: List[Dict]) -> List[Dict]:
    """Normalize provider tool_calls into [{device, action, value, _bad_args}]."""
    out = []
    for c in raw_calls:
        device = c.get("name", "")
        args = c.get("args", {})
        bad = c.get("_bad_args", False)
        # Real tools use per-tool value params (query/location/expression/...), so
        # keep the full args dict; scoring matches an expected value against ANY
        # string arg rather than a single "value" field.
        out.append({"device": device, "action": args.get("action"),
                    "value": args.get("value"), "args": args, "_bad_args": bad})
    return out


def _value_in_args(call: Dict, expected: str) -> bool:
    """True if `expected` appears in any string-valued arg of the tool call."""
    exp = (expected or "").lower()
    if not exp:
        return False
    for v in (call.get("args") or {}).values():
        if isinstance(v, str) and exp in v.lower():
            return True
    return False


def query_llm_openai(prompt: str, max_tokens: int) -> Tuple[str, List[Dict], float, Dict]:
    model = CLOUD_MODEL
    is_gpt5 = bool(re.match(r"(gpt-5|o[13])", model, re.I))
    payload = {
        "model": model,
        "messages": [{"role": "system", "content": SYSTEM_PROMPT},
                     {"role": "user", "content": prompt}],
        "tools": openai_tools(),
        "tool_choice": "auto",
    }
    # gpt-5 / o-series: use max_completion_tokens, don't set temperature (fixed at 1),
    # and set reasoning_effort='none' — these reasoning models reject function tools
    # via /v1/chat/completions unless reasoning is disabled.
    if is_gpt5:
        payload["max_completion_tokens"] = max(max_tokens, 512)
        payload["reasoning_effort"] = "none"
    else:
        payload["max_tokens"] = max_tokens
        payload["temperature"] = 0.7

    try:
        start = time.time()
        r = requests.post("https://api.openai.com/v1/chat/completions",
                          headers={"Authorization": f"Bearer {CLOUD_API_KEY}",
                                   "Content-Type": "application/json"},
                          json=payload, timeout=120)
        elapsed = time.time() - start
        if r.status_code != 200:
            return None, [], elapsed, {"error": f"HTTP {r.status_code}: {r.text[:300]}"}
        data = r.json()
        msg = data["choices"][0]["message"]
        content = msg.get("content") or ""
        calls = []
        for tc in (msg.get("tool_calls") or []):
            fn = tc.get("function", {})
            try:
                args = json.loads(fn.get("arguments") or "{}")
                calls.append({"name": fn.get("name"), "args": args})
            except json.JSONDecodeError:
                calls.append({"name": fn.get("name"), "args": {}, "_bad_args": True})
        return content, _norm_tool_calls(calls), elapsed, data.get("usage", {})
    except Exception as e:
        return None, [], 0, {"error": str(e)}


def query_llm_claude(prompt: str, max_tokens: int) -> Tuple[str, List[Dict], float, Dict]:
    # Newer Claude models (Opus 4.7+, Sonnet 5, …) deprecate `temperature`; a
    # quality test doesn't need a set temperature, so omit it for all Claude models.
    payload = {"model": CLOUD_MODEL, "max_tokens": max(max_tokens, 512),
               "system": SYSTEM_PROMPT, "tools": claude_tools(),
               "messages": [{"role": "user", "content": prompt}]}
    try:
        start = time.time()
        r = requests.post("https://api.anthropic.com/v1/messages",
                          headers={"x-api-key": CLOUD_API_KEY,
                                   "anthropic-version": "2023-06-01",
                                   "content-type": "application/json"},
                          json=payload, timeout=120)
        elapsed = time.time() - start
        if r.status_code != 200:
            return None, [], elapsed, {"error": f"HTTP {r.status_code}: {r.text[:300]}"}
        data = r.json()
        content, calls = "", []
        for block in data.get("content", []):
            if block.get("type") == "text":
                content += block.get("text", "")
            elif block.get("type") == "tool_use":
                calls.append({"name": block.get("name"), "args": block.get("input", {})})
        u = data.get("usage", {})
        return content, _norm_tool_calls(calls), elapsed, {
            "prompt_tokens": u.get("input_tokens", 0),
            "completion_tokens": u.get("output_tokens", 0)}
    except Exception as e:
        return None, [], 0, {"error": str(e)}


def query_llm_local(prompt: str, max_tokens: int) -> Tuple[str, List[Dict], float, Dict]:
    try:
        start = time.time()
        r = requests.post(f"{SERVER}/v1/chat/completions",
                          headers={"Content-Type": "application/json"},
                          json={"messages": [{"role": "system", "content": SYSTEM_PROMPT},
                                             {"role": "user", "content": prompt}],
                                "tools": openai_tools(), "tool_choice": "auto",
                                "temperature": 0.7, "max_tokens": max_tokens, "stream": False},
                          timeout=120)
        elapsed = time.time() - start
        if r.status_code != 200:
            return None, [], elapsed, {"error": f"HTTP {r.status_code}: {r.text[:300]}"}
        data = r.json()
        msg = data["choices"][0]["message"]
        content = msg.get("content") or ""
        calls = []
        for tc in (msg.get("tool_calls") or []):
            fn = tc.get("function", {})
            try:
                args = json.loads(fn.get("arguments") or "{}")
                calls.append({"name": fn.get("name"), "args": args})
            except json.JSONDecodeError:
                calls.append({"name": fn.get("name"), "args": {}, "_bad_args": True})
        return content, _norm_tool_calls(calls), elapsed, data.get("usage", {})
    except Exception as e:
        return None, [], 0, {"error": str(e)}


def query_llm(prompt: str, max_tokens: int = 200):
    if BACKEND == "claude":
        return query_llm_claude(prompt, max_tokens)
    if BACKEND == "openai":
        return query_llm_openai(prompt, max_tokens)
    return query_llm_local(prompt, max_tokens)


def check_persona(text: str) -> bool:
    t = (text or "").lower()
    return "sir" in t or "boss" in t


# =============================================================================
# Scoring (native semantics)
# =============================================================================

def score_test_case(test: Dict, content: str, calls: List[Dict]) -> Tuple[int, Dict]:
    points, details = 0, {}
    checks = test.get("checks", {})
    content = content or ""
    words = len(content.split())
    details["_content"] = content
    details["_calls"] = calls

    # has_tool (expected) / should-not-call
    if checks.get("has_tool") is True:
        if calls:
            points += 2; details["has_tool"] = "✅ Pass"
        else:
            details["has_tool"] = "❌ FAIL - no tool call"
    elif checks.get("has_tool") is False:
        if not calls:
            points += 2; details["has_tool"] = "✅ Pass (correctly no tool)"
        else:
            details["has_tool"] = f"❌ FAIL - unexpected tool call: {[c['device'] for c in calls]}"

    # args_valid (all tool-call args parsed)
    if checks.get("args_valid") and calls:
        if all(not c.get("_bad_args") for c in calls):
            points += 2; details["args_valid"] = "✅ Pass"
        else:
            details["args_valid"] = "❌ FAIL - unparseable tool args"

    # device/action accuracy
    if calls and test.get("expected_device") and not checks.get("weather_flexible"):
        c = calls[0]
        dev_ok = c["device"] == test["expected_device"]
        aset = checks.get("action_in_set")
        if aset:
            act_ok = c["action"] in aset
        elif test.get("expected_action"):
            act_ok = c["action"] == test["expected_action"]
        else:
            act_ok = True
        if dev_ok and act_ok:
            points += 3; details["accuracy"] = f"✅ Pass ({c['device']}/{c['action']})"
        else:
            details["accuracy"] = (f"❌ FAIL - expected {test['expected_device']}/"
                                   f"{test.get('expected_action') or aset}, got {c['device']}/{c['action']}")
        if checks.get("value_correct") and test.get("expected_value") is not None:
            if _value_in_args(c, str(test["expected_value"])):
                points += 1; details["value_correct"] = "✅ Pass"
            else:
                details["value_correct"] = (f"❌ FAIL - '{test['expected_value']}' not in "
                                            f"args {c.get('args')}")

    # action_in_set standalone (when no expected_device accuracy block ran)
    if checks.get("action_in_set") and calls and "accuracy" not in details:
        c = calls[0]
        if c["action"] in checks["action_in_set"]:
            points += 2; details["action_in_set"] = f"✅ Pass ({c['action']})"
        else:
            details["action_in_set"] = f"❌ FAIL - got {c['action']}"

    # has_value (partial match against any string arg)
    if checks.get("has_value") and calls:
        c0 = calls[0]
        exp = str(test.get("expected_value") or "")
        any_str = [v for v in (c0.get("args") or {}).values() if isinstance(v, str) and v]
        if exp and _value_in_args(c0, exp):
            points += 2; details["has_value"] = f"✅ Pass (arg ~ '{exp}')"
        elif any_str:
            points += 1; details["has_value"] = f"⚠️ Partial - has value arg(s) {any_str} vs '{exp}'"
        else:
            details["has_value"] = "❌ FAIL - no value arg"

    # brief_lead: a getter's accompanying text should be short (a model may add a
    # one-liner like "Let me check…" with the tool call — fine; a paragraph is not).
    if checks.get("brief_lead"):
        if words <= 12:
            points += 2; details["brief_lead"] = f"✅ Pass ({words} words of lead-in)"
        else:
            details["brief_lead"] = f"❌ FAIL - {words} words of prose before the tool call"

    # has_sentence_before: a boolean/analog action should carry a short confirmation.
    if checks.get("has_sentence_before"):
        if words > 0:
            points += 1; details["sentence_before"] = f"✅ Pass ({words} words)"
        else:
            details["sentence_before"] = "⚠️ No confirmation sentence (optional)"

    # persona (soft/bonus)
    if checks.get("uses_sir_boss"):
        if check_persona(content):
            points += 1; details["persona"] = "✅ Pass (sir/boss)"
        else:
            details["persona"] = "⚠️ No sir/boss (optional, no penalty)"

    # word limit
    if checks.get("word_limit"):
        lim = checks["word_limit"]
        if words <= lim:
            points += 1; details["word_limit"] = f"✅ Pass ({words}/{lim})"
        else:
            details["word_limit"] = f"❌ FAIL - {words} words (limit {lim})"

    # multiple tools
    if checks.get("multiple_tools"):
        exp = test.get("expected_commands", [])
        devs = {c["device"] for c in calls}
        want = {e["device"] for e in exp}
        if want.issubset(devs):
            points += 2; details["multiple_tools"] = f"✅ Pass (called {sorted(devs)})"
        else:
            details["multiple_tools"] = f"❌ FAIL - wanted {sorted(want)}, got {sorted(devs)}"

    # clarification
    if checks.get("asks_clarification"):
        if "?" in content:
            points += 3; details["clarification"] = "✅ Pass (asked)"
        else:
            details["clarification"] = "❌ FAIL - did not ask"

    # weather flexible (tool OR clarification both acceptable)
    if checks.get("weather_flexible"):
        if calls and calls[0]["device"] == "weather":
            points += 5; details["weather_flexible"] = f"✅ Pass (weather/{calls[0]['action']})"
        elif "?" in content:
            points += 5; details["weather_flexible"] = "✅ Pass (asked for location)"
        else:
            details["weather_flexible"] = "❌ FAIL - neither tool nor clarification"

    return points, details


# =============================================================================
# Runner
# =============================================================================

def run(model_name: str) -> Dict:
    print(f"\n{'='*80}\nDAWN LLM Quality Test (NATIVE TOOLS) - {model_name}\n{'='*80}\n")
    total = sum(t["points"] for t in TEST_CASES)
    earned, results = 0, []

    for i, test in enumerate(TEST_CASES, 1):
        print(f"Test {i}/{len(TEST_CASES)}: {test['name']}")
        print(f"User: {test['user']}")
        content, calls, elapsed, stats = query_llm(test["user"])
        if content is None:
            print(f"❌ ERROR: {stats.get('error')}\n")
            results.append({"test": test["name"], "type": test["type"], "points": 0,
                            "max_points": test["points"], "error": stats.get("error"),
                            "elapsed": elapsed})
            continue
        pts, details = score_test_case(test, content, calls)
        pts = min(pts, test["points"])  # a case can't exceed its declared max
        earned += pts
        call_str = ", ".join(f"{c['device']}({c['action']}"
                             + (f", '{c['value']}'" if c.get("value") else "") + ")"
                             for c in calls) or "(no tool call)"
        print(f"FRIDAY text: {content!r}")
        print(f"Tool calls : {call_str}")
        print(f"Score: {pts}/{test['points']}   Time: {elapsed:.2f}s")
        for k, v in details.items():
            if not k.startswith("_"):
                print(f"  {v}")
        print()
        results.append({"test": test["name"], "type": test["type"], "points": pts,
                        "max_points": test["points"], "elapsed": elapsed,
                        "content": content, "calls": calls, "stats": stats})
        time.sleep(0.4)

    pct = earned / total * 100 if total else 0
    grade = ("A - Excellent" if pct >= 90 else "B - Good" if pct >= 80 else
             "C - Acceptable" if pct >= 70 else "D - Marginal" if pct >= 60 else "F")
    times = [r["elapsed"] for r in results if r.get("elapsed")]
    avg_t = sum(times) / len(times) if times else 0

    print(f"{'='*80}\nSUMMARY - {model_name}\n{'='*80}")
    print(f"Total: {earned}/{total} ({pct:.1f}%)   Grade: {grade}")
    print(f"Avg latency: {avg_t:.2f}s   (n={len(times)})")
    cats = {}
    for r in results:
        c = cats.setdefault(r["type"], {"e": 0, "m": 0})
        c["e"] += r["points"]; c["m"] += r["max_points"]
    print("\nBy category:")
    for cat, s in sorted(cats.items()):
        cp = s["e"] / s["m"] * 100 if s["m"] else 0
        print(f"  {cat:18s}: {s['e']:3d}/{s['m']:3d} ({cp:5.1f}%)")
    print(f"\n{'='*80}\n")
    return {"model": model_name, "total_points": earned, "max_points": total,
            "percentage": pct, "grade": grade, "avg_latency": avg_t, "tests": results}


def load_api_key_from_secrets(provider: str) -> str:
    for p in ("~/code/The-OASIS-Project/dawn/secrets.toml", "~/code/dawn/secrets.toml"):
        path = os.path.expanduser(p)
        if not os.path.exists(path):
            continue
        key_name = {"claude": "claude_api_key", "openai": "openai_api_key"}.get(provider)
        try:
            with open(path) as f:
                for line in f:
                    line = line.strip()
                    if line.startswith(key_name) and "=" in line:
                        return line.split("=", 1)[1].strip().strip('"').strip("'")
        except Exception:
            pass
    return ""


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="DAWN native-tools LLM quality test")
    ap.add_argument("--cloud", choices=["claude", "openai"])
    ap.add_argument("--model")
    ap.add_argument("--api-key")
    args = ap.parse_args()

    if args.cloud:
        if not args.model:
            sys.exit("❌ --model required with --cloud")
        BACKEND, CLOUD_MODEL = args.cloud, args.model
        env = "ANTHROPIC_API_KEY" if args.cloud == "claude" else "OPENAI_API_KEY"
        CLOUD_API_KEY = args.api_key or os.getenv(env) or load_api_key_from_secrets(args.cloud)
        if not CLOUD_API_KEY:
            sys.exit(f"❌ No API key: set {env}, pass --api-key, or add to secrets.toml")
        model_name = f"{args.cloud}:{args.model}"
    else:
        try:
            if requests.get(f"{SERVER}/health", timeout=2).status_code != 200:
                sys.exit(f"❌ llama-server not healthy at {SERVER}")
            props = requests.get(f"{SERVER}/props", timeout=2).json()
            model_name = props.get("default_generation_settings", {}).get("model", "local")
            model_name = model_name.split("/")[-1]
        except Exception as e:
            sys.exit(f"❌ cannot reach local server: {e}")

    res = run(model_name)
    out = f"quality_native_{int(time.time())}_{re.sub(r'[^A-Za-z0-9._-]', '_', model_name)}.json"
    with open(out, "w") as f:
        json.dump(res, f, indent=2)
    print(f"Saved: {out}")
