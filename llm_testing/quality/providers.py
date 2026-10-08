# Send a captured request (streamed) and read the reply in each provider's
# dialect: Anthropic Messages, OpenAI Responses, OpenAI Chat Completions.  Also
# appends a turn's tool results and the next user turn in the same dialect, so
# the tool loop continues the conversation the way DAWN does.  No request
# builders: the body is the captured one.
#
# A stream that ends without its terminal event, an error event inside a
# stream, and rate limits or overloads are errors.  The transient ones are
# retried (they say nothing about the model); a reply that still fails is a run
# error, which the report counts apart from quality.
#
# License: GPLv3, same as DAWN.

import json
import os
import re
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional

import requests

from .capture import Capture, messages_key

REQUEST_TIMEOUT_S = 180
RETRY_ATTEMPTS = 6
TRUNCATED = "stream ended early"
# Errors that say nothing about the model.  Not "server_error": OpenAI also
# uses it for content the model produced, which retrying would hide.
_TRANSIENT = ("rate_limit", "overloaded", "api_error", TRUNCATED, "HTTP 408", "HTTP 429",
              "HTTP 500", "HTTP 502", "HTTP 503", "HTTP 504", "HTTP 529", "timed out",
              "Read timed out", "Connection aborted", "Connection broken", "IncompleteRead",
              "ConnectTimeout")
# Connection failures, matched only as the exception that ended the request (the
# start of the error), never inside a response body that quotes them.
_TRANSIENT_EXCEPTIONS = ("ConnectionError:", "SSLError:", "ChunkedEncodingError:")


@dataclass
class ToolCall:
    id: str
    name: str
    args: dict
    bad_args: bool = False


@dataclass
class Reply:
    text: str = ""
    calls: List[ToolCall] = field(default_factory=list)
    assistant_items: list = field(default_factory=list)  # what to append, provider-shaped
    # input (uncached), output, cache_read, cache_write: one meaning across providers
    usage: Dict[str, int] = field(default_factory=dict)
    first_text_s: Optional[float] = None  # time to the first visible answer text
    total_s: float = 0.0
    stop: str = ""
    error: str = ""


def load_keys(secrets_path: str) -> Dict[str, str]:
    """API keys from DAWN's secrets.toml (flat `name = "value"` lines)."""
    keys = {}
    if os.path.exists(secrets_path):
        for line in open(secrets_path):
            m = re.match(r'\s*([a-z_]+_api_key)\s*=\s*"([^"]*)"', line)
            if m:
                keys[m.group(1)] = m.group(2)
    return keys


def _headers(cap: Capture, keys: Dict[str, str]) -> Dict[str, str]:
    """The captured headers with the redacted credential filled back in."""
    if "openrouter.ai" in cap.url:
        key = keys.get("openrouter_api_key", "")
    elif cap.provider == "claude":
        key = keys.get("claude_api_key", "")
    else:
        key = keys.get("openai_api_key", "")
    out = {}
    for h in cap.headers:
        name, _, value = h.partition(":")
        value = value.strip()
        if value == "[REDACTED]":
            value = f"Bearer {key}" if name.lower() == "authorization" else key
        out[name.strip()] = value
    return out


def _sse(resp):
    """(event, data) pairs from a server-sent-events response; "[DONE]" as ("done", {})."""
    event = None
    for raw in resp.iter_lines(decode_unicode=True):
        if raw is None:
            continue
        if raw.startswith("event:"):
            event = raw[6:].strip()
        elif raw.startswith("data:"):
            data = raw[5:].strip()
            if data == "[DONE]":
                yield "done", {}
                return
            try:
                yield event, json.loads(data)
            except json.JSONDecodeError:
                continue
        elif raw == "":
            event = None


def _parse_args(text: str):
    """Tool-call arguments as a dict; anything else is a malformed call."""
    try:
        args = json.loads(text) if text else {}
    except json.JSONDecodeError:
        return {}, True
    return (args, False) if isinstance(args, dict) else ({}, True)


def _read_claude(resp, reply: Reply, text_seen):
    blocks: Dict[int, dict] = {}
    finished = False
    for _ev, d in _sse(resp):
        t = d.get("type")
        if t == "message_start":
            u = d["message"].get("usage", {})
            reply.usage.update(input=u.get("input_tokens", 0),
                               cache_read=u.get("cache_read_input_tokens", 0) or 0,
                               cache_write=u.get("cache_creation_input_tokens", 0) or 0)
        elif t == "content_block_start":
            blocks[d["index"]] = dict(d["content_block"])
            if d["content_block"].get("type") == "tool_use":
                blocks[d["index"]]["_json"] = ""
        elif t == "content_block_delta":
            b, delta = blocks[d["index"]], d["delta"]
            kind = delta.get("type")
            if kind == "text_delta":
                if delta["text"].strip():
                    text_seen()
                b["text"] = b.get("text", "") + delta["text"]
            elif kind == "input_json_delta":
                b["_json"] += delta.get("partial_json", "")
            elif kind == "thinking_delta":
                b["thinking"] = b.get("thinking", "") + delta.get("thinking", "")
            elif kind == "signature_delta":
                b["signature"] = b.get("signature", "") + delta.get("signature", "")
        elif t == "message_delta":
            reply.usage["output"] = d.get("usage", {}).get("output_tokens", 0)
            reply.stop = d.get("delta", {}).get("stop_reason") or reply.stop
        elif t == "message_stop":
            finished = True
        elif t == "error":
            reply.error = json.dumps(d.get("error"))
    if not reply.error and not finished:
        reply.error = TRUNCATED
    content = []
    for i in sorted(blocks):
        b = blocks[i]
        if b.get("type") == "tool_use":
            args, bad = _parse_args(b.pop("_json"))
            b["input"] = args
            reply.calls.append(ToolCall(b["id"], b["name"], args, bad))
        elif b.get("type") == "text":
            if not b.get("text"):
                continue  # Anthropic refuses an empty text block on replay; DAWN skips them
            reply.text += b["text"]
        content.append(b)
    reply.assistant_items = [{"role": "assistant", "content": content}]


def _read_responses(resp, reply: Reply, text_seen):
    items: List[dict] = []
    finished = False
    for _ev, d in _sse(resp):
        t = d.get("type", "")
        if t == "response.output_text.delta":
            if d.get("delta", "").strip():
                text_seen()
            reply.text += d.get("delta", "")
        elif t == "response.output_item.done":
            items.append(d["item"])
        elif t == "response.completed":
            finished = True
            r = d["response"]
            u = r.get("usage", {}) or {}
            cached = (u.get("input_tokens_details") or {}).get("cached_tokens", 0) or 0
            # OpenAI counts cached tokens inside input_tokens; "input" here is uncached.
            reply.usage.update(input=u.get("input_tokens", 0) - cached,
                               output=u.get("output_tokens", 0), cache_read=cached, cache_write=0)
            reply.stop = r.get("status", "")
            if not items:
                items = r.get("output", [])
        elif t == "response.incomplete":
            # The output cap or a content filter: the model's own stop, scored
            # as the answer it gave (as Claude's max_tokens is).
            finished = True
            reply.stop = "incomplete"
        elif t in ("response.failed", "error"):
            reply.error = json.dumps(d)
    if not reply.error and not finished:
        reply.error = TRUNCATED
    for it in items:
        if it.get("type") == "function_call":
            args, bad = _parse_args(it.get("arguments", ""))
            reply.calls.append(ToolCall(it["call_id"], it["name"], args, bad))
    reply.assistant_items = items


def merge_reasoning_details(pieces: List[dict]) -> List[dict]:
    """Streamed reasoning_details merged as DAWN merges them before replay
    (llm_reasoning_details_add): a piece of the same type and index continues
    the open entry, its text and summary appended, other fields set again."""
    out: List[dict] = []
    for p in pieces:
        if not isinstance(p, dict):
            continue
        cur = out[-1] if out else None
        same = (cur is not None and p.get("type") is not None and p.get("type") == cur.get("type")
                and ("index" in p) == ("index" in cur) and p.get("index") == cur.get("index"))
        if not same:
            cur = {}
            out.append(cur)
        for k, v in p.items():
            if k in ("text", "summary") and isinstance(v, str):
                cur[k] = cur.get(k, "") + v
            elif v is not None:
                cur[k] = v
    return out


def _read_chat(resp, reply: Reply, text_seen):
    calls: Dict[int, dict] = {}
    reasoning, details = "", []
    finished = False
    for ev, d in _sse(resp):
        if ev == "done":
            finished = True
            break
        if d.get("error"):
            err = d["error"]
            code = err.get("code") if isinstance(err, dict) else None
            # OpenRouter reports an upstream 429/5xx inside a 200 stream.
            prefix = f"HTTP {code}: " if isinstance(code, int) else ""
            reply.error = prefix + json.dumps(err)
            continue
        if d.get("usage"):
            u = d["usage"]
            cached = (u.get("prompt_tokens_details") or {}).get("cached_tokens", 0) or 0
            reply.usage.update(input=u.get("prompt_tokens", 0) - cached,
                               output=u.get("completion_tokens", 0), cache_read=cached,
                               cache_write=0)
        for ch in d.get("choices", []):
            delta = ch.get("delta", {})
            if delta.get("content"):
                if delta["content"].strip():
                    text_seen()
                reply.text += delta["content"]
            reasoning += delta.get("reasoning") or ""
            details.extend(delta.get("reasoning_details") or [])
            for tc in delta.get("tool_calls") or []:
                c = calls.setdefault(tc.get("index", 0), {"id": "", "name": "", "args": ""})
                c["id"] = tc.get("id") or c["id"]
                fn = tc.get("function", {})
                c["name"] += fn.get("name") or ""
                c["args"] += fn.get("arguments") or ""
            if ch.get("finish_reason"):
                reply.stop = ch["finish_reason"]
                if ch["finish_reason"] == "error":
                    reply.error = reply.error or "finish_reason error"
    if not reply.error and not finished:
        reply.error = TRUNCATED
    msg = {"role": "assistant", "content": reply.text or None}
    # Reasoning goes back as DAWN sends it: some models (Gemini via OpenRouter)
    # refuse a tool follow-up without their thought signatures.
    if reasoning:
        msg["reasoning"] = reasoning
    if details:
        msg["reasoning_details"] = merge_reasoning_details(details)
    if calls:
        msg["tool_calls"] = []
        for i in sorted(calls):
            c = calls[i]
            args, bad = _parse_args(c["args"])
            reply.calls.append(ToolCall(c["id"], c["name"], args, bad))
            msg["tool_calls"].append({"id": c["id"], "type": "function",
                                      "function": {"name": c["name"], "arguments": c["args"]}})
    reply.assistant_items = [msg]


_READERS = {"claude": _read_claude, "openai-responses": _read_responses,
            "openai-chat": _read_chat}


def is_transient(error: str) -> bool:
    return any(t in error for t in _TRANSIENT) or error.startswith(_TRANSIENT_EXCEPTIONS)


def send(cap: Capture, body: dict, keys: Dict[str, str]) -> Reply:
    """POST @p body and read the streamed reply, retrying transient failures
    with backoff.  Nothing has run yet when a request is retried, so the
    mocked world is untouched."""
    for attempt in range(RETRY_ATTEMPTS):
        reply = _send_once(cap, body, keys)
        if not reply.error or not is_transient(reply.error):
            return reply
        time.sleep(min(30.0, 2.0 * (2 ** attempt)))
    return reply


def _send_once(cap: Capture, body: dict, keys: Dict[str, str]) -> Reply:
    reply = Reply()
    start = time.time()

    def text_seen():
        if reply.first_text_s is None:
            reply.first_text_s = time.time() - start

    try:
        with requests.post(cap.url, headers=_headers(cap, keys), json=body, stream=True,
                           timeout=REQUEST_TIMEOUT_S) as resp:
            resp.encoding = "utf-8"  # an event stream without a charset is not Latin-1
            if resp.status_code != 200:
                reply.error = f"HTTP {resp.status_code}: {resp.text[:300]}"
            else:
                _READERS[cap.provider](resp, reply, text_seen)
    except requests.RequestException as e:
        reply.error = f"{type(e).__name__}: {e}"
    reply.total_s = time.time() - start
    return reply


def _mark_breakpoint(msgs: list):
    """Claude: the conversation's one cache breakpoint on the last non-empty
    block of the last user message, as DAWN places it
    (mark_conversation_breakpoint in llm_claude_format.c), so each tool round
    and turn reads everything before it from the cache."""
    for m in msgs:
        if isinstance(m, dict) and m.get("role") == "user":
            _strip_cache_control(m)
    for m in reversed(msgs):
        if not (isinstance(m, dict) and m.get("role") == "user"):
            continue
        blocks = m.get("content")
        if isinstance(blocks, str):
            m["content"] = blocks = [{"type": "text", "text": blocks}]
        n = len(blocks or [])
        while n > 0 and "text" in blocks[n - 1] and not blocks[n - 1]["text"]:
            n -= 1
        if n:
            blocks[n - 1]["cache_control"] = {"type": "ephemeral"}
        return


def append_results(cap: Capture, body: dict, reply: Reply, results: Dict[str, str]):
    """Append the assistant's turn and its tool results (by call id) to @p body."""
    msgs = body[messages_key(cap.provider)]
    msgs.extend(reply.assistant_items)
    if cap.provider == "claude":
        msgs.append({"role": "user", "content": [
            {"type": "tool_result", "tool_use_id": c.id, "content": results[c.id]}
            for c in reply.calls]})
        _mark_breakpoint(msgs)
    elif cap.provider == "openai-responses":
        msgs.extend({"type": "function_call_output", "call_id": c.id, "output": results[c.id]}
                    for c in reply.calls)
    else:
        msgs.extend({"role": "tool", "tool_call_id": c.id, "content": results[c.id]}
                    for c in reply.calls)


def append_answer(cap: Capture, body: dict, reply: Reply):
    """Append a final (no tool calls) assistant turn to @p body."""
    body[messages_key(cap.provider)].extend(reply.assistant_items)


def _strip_cache_control(obj):
    if isinstance(obj, dict):
        obj.pop("cache_control", None)
        for v in obj.values():
            _strip_cache_control(v)
    elif isinstance(obj, list):
        for v in obj:
            _strip_cache_control(v)


def append_user(cap: Capture, body: dict, message: dict):
    """Append the next user turn (Claude: the breakpoint moves onto it)."""
    msgs = body[messages_key(cap.provider)]
    msgs.append(message)
    if cap.provider == "claude":
        _mark_breakpoint(msgs)
