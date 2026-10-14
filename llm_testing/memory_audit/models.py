# Memory audit: send one batch prompt to a classifier model.
#
# Model specs: "claude:<model>" (Anthropic Messages API, key from secrets.toml)
# or "local:<name>" (an OpenAI-compatible server such as llama.cpp; <name> is
# only a label, the server serves what it loaded).  No reasoning: classifying a
# fact against a rubric doesn't need it, and the run covers thousands of facts.
#
# License: GPLv3, same as DAWN.

import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                                "benchmarks"))
from anthropic_request import anthropic_body, text_of  # noqa: E402

ANTHROPIC_URL = "https://api.anthropic.com/v1/messages"
MAX_TOKENS = 4096
TIMEOUT_S = 300
RETRIES = 4


def load_keys(secrets_path):
    """API keys from DAWN's secrets.toml (flat `name = "value"` lines)."""
    keys = {}
    if os.path.exists(secrets_path):
        for line in open(secrets_path):
            m = re.match(r'\s*([a-z_]+_api_key)\s*=\s*"([^"]*)"', line)
            if m:
                keys[m.group(1)] = m.group(2)
    return keys


def parse_spec(spec):
    provider, _, model = spec.partition(":")
    if provider not in ("claude", "local") or not model:
        raise SystemExit(f"model spec '{spec}': use claude:<model> or local:<name>")
    return provider, model


def check_ready(spec, keys, endpoint):
    provider, _ = parse_spec(spec)
    if provider == "claude" and not keys.get("claude_api_key"):
        raise SystemExit("no claude_api_key in secrets.toml (pass --secrets)")
    if provider == "local" and not endpoint:
        raise SystemExit("local model: pass --endpoint http://host:port")


def _post(url, headers, body):
    req = urllib.request.Request(url, data=json.dumps(body).encode(), headers=headers)
    with urllib.request.urlopen(req, timeout=TIMEOUT_S) as resp:
        return json.load(resp)


def _call(spec, system, user, keys, endpoint):
    provider, model = parse_spec(spec)
    if provider == "claude":
        headers = {"content-type": "application/json", "anthropic-version": "2023-06-01",
                   "x-api-key": keys["claude_api_key"]}
        data = _post(ANTHROPIC_URL, headers, anthropic_body(model, system, user, 0.0, MAX_TOKENS))
        u = data.get("usage", {})
        return text_of(data), {"in": u.get("input_tokens", 0), "out": u.get("output_tokens", 0)}
    body = {
        "model": model,
        "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}],
        "temperature": 0,
        "max_tokens": MAX_TOKENS,
        "chat_template_kwargs": {"enable_thinking": False},
    }
    data = _post(endpoint.rstrip("/") + "/v1/chat/completions",
                 {"content-type": "application/json"}, body)
    u = data.get("usage", {})
    text = data["choices"][0]["message"].get("content") or ""
    return text, {"in": u.get("prompt_tokens", 0), "out": u.get("completion_tokens", 0)}


def classify_batch(spec, system, user, keys, endpoint):
    """(text, usage) for one batch, retrying network errors, 429 and 5xx."""
    delay = 2.0
    for attempt in range(RETRIES + 1):
        try:
            return _call(spec, system, user, keys, endpoint)
        except urllib.error.HTTPError as e:
            if e.code not in (429, 500, 502, 503, 504, 529) or attempt == RETRIES:
                raise RuntimeError(f"HTTP {e.code}: {e.read()[:300]!r}") from None
        except (urllib.error.URLError, TimeoutError, ConnectionError, OSError):
            if attempt == RETRIES:
                raise
        time.sleep(delay)
        delay *= 2
    raise RuntimeError("unreachable")


def parse_reply(text, ids, kind_names, tier_names=None):
    """{id: (kind, reason, tier)} from a reply; ids the reply skipped or garbled
    are left out so the caller retries them.  tier is None unless tier_names is
    given, and then a valid tier is required too.  Tolerates a ```json fence and
    text around the array."""
    m = re.search(r"\[.*\]", text, re.S)
    if not m:
        return {}
    try:
        items = json.loads(m.group(0))
    except json.JSONDecodeError:
        return {}
    wanted = set(ids)
    out = {}
    for it in items if isinstance(items, list) else []:
        if not isinstance(it, dict):
            continue
        fid, kind = it.get("id"), it.get("kind")
        if isinstance(fid, str) and fid.isdigit():
            fid = int(fid)
        tier = it.get("tier") if tier_names else None
        if fid in wanted and kind in kind_names and (not tier_names or tier in tier_names):
            out[fid] = (kind, str(it.get("reason", ""))[:200], tier)
    return out
