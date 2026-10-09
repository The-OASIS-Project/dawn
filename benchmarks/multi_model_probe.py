# Shared multi-model harness for DAWN bench probes.
#
# Used by benchmarks that want to run the same logic across multiple LLM
# providers (Anthropic / OpenAI / local llama.cpp / Ollama) and aggregate
# verdicts via N-of-M quorum.  Probe-specific baselines, case lists, and
# metrics live in the calling probe; this module owns dispatch only.
#
# Public surface:
#     PROVIDER_DEFAULTS       — per-provider {default_model, endpoint, secrets_key}
#     AGGREGATE_QUORUM        — default 2-of-3 quorum constant
#     load_secrets(path)      — parse a small subset of secrets.toml inline
#                               (no tomllib dep — Python 3.10 ships on Jetson)
#     resolve_provider_config(provider, secrets, dawn_toml_path, override_model,
#                             override_endpoint)
#                             → (caller_fn, model, endpoint, api_key)
#     PROVIDER_CALLERS        — {name → caller_fn}; each accepts
#                               (model, system, user_prompt, api_key, endpoint,
#                                temperature, max_tokens, timeout) and returns
#                               the assistant text.  Raises on transport failure.
#     read_local_endpoint_from_toml(toml_path) → str|None
#     read_local_model_from_endpoint(endpoint) → str
#
# Factoring history: extracted from benchmarks/bench_speaker_attribution.py
# during Phase 1i.A of the Dynamic Context Injection workstream so a second
# probe (focus-injection fix-rate, Phase 1i.C) can share the same provider
# dispatch surface without forking it.  Every symbol re-exported here keeps
# byte-identical behavior to the speaker probe's pre-factor copy.
#
# License: GPLv3 — same as the calling probes.

import json
import re
import sys
from pathlib import Path

from anthropic_request import anthropic_body, text_of


# Aggregate verdict: PASS iff at least this many providers pass each component.
# With 3 providers, a 2-of-3 quorum tolerates one model-specific quirk while
# requiring real cross-model evidence of widening / regression.
AGGREGATE_QUORUM = 2


# Per-provider dispatch defaults.  Calling probes layer their OWN
# probe-specific baselines (fact counts, fix counts, etc.) on top — those
# don't belong here because they're calibrated against probe-specific
# regression targets.  This dict carries only the dispatch shape that any
# multi-model probe needs.
PROVIDER_DEFAULTS = {
    "anthropic": {
        "default_model": "claude-haiku-4-5",
        "endpoint": "https://api.anthropic.com/v1/messages",
        "secrets_key": "claude_api_key",
    },
    "openai": {
        "default_model": "gpt-5.4-mini",
        "endpoint": "https://api.openai.com/v1/chat/completions",
        "secrets_key": "openai_api_key",
    },
    "local": {
        "default_model": "",                   # use whatever's loaded at the endpoint
        "endpoint": None,                      # read from dawn.toml [llm.local].endpoint
        "secrets_key": None,                   # local endpoints don't auth
    },
}


# =============================================================================
# Provider call helpers — one urllib-based call per backend.  No SDK
# dependencies, mirroring run_benchmark.py:_anthropic_call shape so probes
# stay self-contained.
# =============================================================================


def _anthropic_call(model, system, user_prompt, api_key, endpoint,
                    temperature=0.0, max_tokens=1024, timeout=60.0):
    import urllib.request
    payload = json.dumps(anthropic_body(model, system, user_prompt, temperature,
                                        max_tokens)).encode("utf-8")
    req = urllib.request.Request(
        endpoint,
        data=payload,
        headers={
            "x-api-key": api_key,
            "anthropic-version": "2023-06-01",
            "content-type": "application/json",
        },
        method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        body = resp.read().decode("utf-8")
    return text_of(json.loads(body))


def _openai_call(model, system, user_prompt, api_key, endpoint,
                 temperature=0.0, max_tokens=1024, timeout=60.0):
    """OpenAI chat-completions style.  Used for both api.openai.com and any
    OpenAI-compatible local endpoint that exposes /v1/chat/completions.

    Newer OpenAI models (gpt-5.4*, o-series) reject `max_tokens` and require
    `max_completion_tokens` instead.  We try `max_completion_tokens` first
    when the endpoint is api.openai.com (the canonical signal), and retry
    with `max_tokens` on a 400 response from older / local endpoints that
    only accept the legacy field."""
    import urllib.request
    import urllib.error
    messages = []
    if system:
        messages.append({"role": "system", "content": system})
    messages.append({"role": "user", "content": user_prompt})

    def post(token_field):
        body = {
            "model": model,
            "messages": messages,
            "temperature": temperature,
            token_field: max_tokens,
        }
        payload = json.dumps(body).encode("utf-8")
        headers = {"content-type": "application/json"}
        if api_key:
            headers["authorization"] = f"Bearer {api_key}"
        req = urllib.request.Request(
            endpoint, data=payload, headers=headers, method="POST")
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.read().decode("utf-8")

    is_canonical_openai = "api.openai.com" in (endpoint or "")
    primary, fallback = (("max_completion_tokens", "max_tokens")
                         if is_canonical_openai
                         else ("max_tokens", "max_completion_tokens"))
    try:
        body = post(primary)
    except urllib.error.HTTPError as e:
        if e.code == 400:
            body = post(fallback)
        else:
            raise
    data = json.loads(body)
    choices = data.get("choices") or []
    if not choices:
        return ""
    msg = choices[0].get("message") or {}
    return msg.get("content") or ""


def _local_call(model, system, user_prompt, api_key, endpoint,
                temperature=0.0, max_tokens=1024, timeout=120.0):
    """Local llama.cpp / Ollama OpenAI-compatible endpoint.  Same wire shape
    as _openai_call; longer default timeout because Qwen3-35B-A3B inference
    on a single Jetson is slower than cloud round-trip."""
    return _openai_call(model, system, user_prompt, api_key, endpoint,
                        temperature=temperature, max_tokens=max_tokens,
                        timeout=timeout)


PROVIDER_CALLERS = {
    "anthropic": _anthropic_call,
    "openai": _openai_call,
    "local": _local_call,
}


# =============================================================================
# Resolution + endpoint discovery
# =============================================================================


def resolve_provider_config(provider, secrets, dawn_toml_path, override_model=None,
                            override_endpoint=None):
    """Return a (caller_fn, model, endpoint, api_key) tuple for the named
    provider.  Reads model/endpoint from PROVIDER_DEFAULTS, falling back to
    dawn.toml for the local endpoint, and accepts CLI overrides."""
    if provider not in PROVIDER_DEFAULTS:
        sys.exit(f"error: unknown provider {provider!r}; expected one of "
                 f"{list(PROVIDER_DEFAULTS.keys())}")
    cfg = PROVIDER_DEFAULTS[provider]
    model = override_model or cfg["default_model"]
    endpoint = override_endpoint or cfg["endpoint"]
    api_key = None
    if provider == "anthropic":
        api_key = secrets.get("claude_api_key", "")
        if not api_key:
            sys.exit("error: anthropic provider needs claude_api_key in secrets.toml")
    elif provider == "openai":
        api_key = secrets.get("openai_api_key", "")
        if not api_key:
            sys.exit("error: openai provider needs openai_api_key in secrets.toml")
    elif provider == "local":
        if endpoint is None:
            endpoint = read_local_endpoint_from_toml(dawn_toml_path)
        # Local endpoints we use don't require auth.
        api_key = None
        if not model:
            model = read_local_model_from_endpoint(endpoint)
        if endpoint and not endpoint.endswith("/chat/completions"):
            endpoint = endpoint.rstrip("/") + "/v1/chat/completions"
    return PROVIDER_CALLERS[provider], model, endpoint, api_key


def read_local_endpoint_from_toml(toml_path):
    """Cheap inline TOML lookup for [llm.local] endpoint — avoids a tomllib
    dep on Python 3.10 systems (Jetson Linux 5.15 ships 3.10).  Falls back
    to None if the section isn't found."""
    try:
        body = Path(toml_path).read_text()
    except OSError:
        return None
    in_section = False
    for line in body.splitlines():
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            in_section = (s == "[llm.local]")
            continue
        if in_section and s.startswith("endpoint"):
            m = re.search(r'endpoint\s*=\s*"([^"]+)"', s)
            if m:
                return m.group(1)
    return None


def read_local_model_from_endpoint(endpoint):
    """Query the local /v1/models endpoint (or /models) and return the first
    model id.  Used when no --model is supplied for the local provider."""
    if not endpoint:
        return ""
    base = endpoint.split("/v1/")[0] if "/v1/" in endpoint else endpoint
    base = base.rstrip("/")
    import urllib.request
    for path in ("/v1/models", "/models"):
        try:
            with urllib.request.urlopen(base + path, timeout=5.0) as resp:
                data = json.loads(resp.read().decode("utf-8"))
            models = data.get("models") or data.get("data") or []
            if models:
                m = models[0]
                # Different runtimes name the field differently.
                return m.get("id") or m.get("name") or m.get("model") or ""
        except Exception:
            continue
    return ""


# =============================================================================
# Secrets parsing
# =============================================================================


def load_secrets(secrets_path):
    """Return a dict with the provider API keys we use.  Inline parser
    (no tomllib dep) — tolerates missing keys; provider-specific lookups
    in resolve_provider_config error out only when the chosen provider
    needs a key it can't find."""
    out = {}
    try:
        body = Path(secrets_path).read_text()
    except OSError:
        return out
    for key in ("claude_api_key", "openai_api_key", "gemini_api_key"):
        m = re.search(rf'^\s*{key}\s*=\s*"([^"]+)"', body, re.MULTILINE)
        if m:
            out[key] = m.group(1)
    return out
