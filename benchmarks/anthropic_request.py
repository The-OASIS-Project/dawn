# Anthropic Messages request bodies for the benchmark harnesses.
#
# Newer Claude models reject parameters the older ones took, and think by
# default when `thinking` is left out, so one fixed body no longer works across
# models:
#   - Claude Opus 4.7 and later, Sonnet 5 and later and Haiku 5.5 return a 400
#     for `temperature` (Haiku 5.5 accepts only its default of 1).
#   - Opus 5, Sonnet 5, Haiku 5.5 and later run adaptive thinking when
#     `thinking` is omitted; a judge call with max_tokens=8 can then end
#     before any text.  Those that can turn thinking off get "disabled";
#     those that can't (Opus 5.5, Sonnet 5.5, Fable, Mythos) get adaptive
#     thinking at effort "low" and room for it in max_tokens.
# Models before Opus 4.7 keep the body they always had (temperature, no
# thinking parameter), so their results stay comparable with earlier runs.
#
# Which models can turn thinking off is read from the repo's models.toml
# [thinking.anthropic] (the table DAWN itself uses), by longest matching
# prefix.  Which take `temperature` is harness-only knowledge, listed below.
# An unlisted model is treated as the newest kind: no sampling parameters,
# adaptive at "low".
#
# DAWN_BENCH_THINKING_EFFORT=<effort> runs the newer models with adaptive
# thinking at that effort instead, to compare a model with thinking on.
#
# License: GPLv3 — same as the calling benchmarks.

import os
import re
from pathlib import Path

# Models that take `temperature` and think only when asked: everything before
# Claude Opus 4.7.  Longest prefix wins, so the 4.7+ rows override "claude-opus-4".
_SAMPLING_PREFIXES = {
    "claude-3": True,
    "claude-opus-4": True,       # Opus 4, 4.1, 4.5, 4.6
    "claude-sonnet-4": True,     # Sonnet 4, 4.5, 4.6
    "claude-haiku-4": True,
    "claude-opus-4-7": False,
    "claude-opus-4-8": False,
}

# An adaptive call still thinks a little at "low"; leave it room to answer.
ADAPTIVE_MIN_MAX_TOKENS = 2048

_MODELS_TOML = Path(__file__).resolve().parent.parent / "models.toml"
_ROW_RE = re.compile(r'^"([^"]+)"\s*=\s*\{\s*modes\s*=\s*\[([^\]]*)\]')

_can_disable = None  # prefix -> bool, loaded on first use


def _load_thinking_rows():
    """[thinking.anthropic] prefix -> whether "disabled" is among its modes.
    Its rows are one-line inline tables, so no TOML library is needed
    (Python 3.10 on the Jetson has no tomllib)."""
    rows = {}
    in_table = False
    for line in _MODELS_TOML.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line.startswith("["):
            in_table = line == "[thinking.anthropic]"
            continue
        m = _ROW_RE.match(line) if in_table else None
        if m:
            rows[m.group(1)] = '"disabled"' in m.group(2)
    if not rows:
        raise RuntimeError(f"no [thinking.anthropic] rows in {_MODELS_TOML}")
    return rows


def _anthropic_id(model):
    """Anthropic's spelling of an OpenRouter slug: anthropic/claude-haiku-4.5
    -> claude-haiku-4-5 (as DAWN's llm_model_route does)."""
    if model.startswith("anthropic/"):
        model = model[len("anthropic/"):]
    return re.sub(r"(?<=\d)\.(?=\d)", "-", model)


def _longest(table, model):
    model = _anthropic_id(model)
    best = max((p for p in table if model.startswith(p)), key=len, default=None)
    return table[best] if best is not None else None


def takes_temperature(model):
    """True when @p model accepts `temperature` (models before Opus 4.7)."""
    return bool(_longest(_SAMPLING_PREFIXES, model))


def anthropic_body(model, system, user_prompt, temperature, max_tokens):
    """A Messages API body for one system + user turn that @p model accepts.
    @p temperature applies only to models that take it."""
    global _can_disable
    body = {
        "model": model,
        "max_tokens": max_tokens,
        "system": system,
        "messages": [{"role": "user", "content": user_prompt}],
    }
    if takes_temperature(model):
        body["temperature"] = temperature
        return body
    if _can_disable is None:
        _can_disable = _load_thinking_rows()
    effort = os.environ.get("DAWN_BENCH_THINKING_EFFORT", "")
    if effort:
        body["thinking"] = {"type": "adaptive"}
        body["output_config"] = {"effort": effort}
        body["max_tokens"] = max(max_tokens, ADAPTIVE_MIN_MAX_TOKENS)
    elif _longest(_can_disable, model):
        body["thinking"] = {"type": "disabled"}
    else:
        body["thinking"] = {"type": "adaptive"}
        body["output_config"] = {"effort": "low"}
        body["max_tokens"] = max(max_tokens, ADAPTIVE_MIN_MAX_TOKENS)
    return body


def text_of(data):
    """The answer text of a Messages API response.  Raises when the reply was
    cut off before any text (all of max_tokens spent thinking), so callers
    retry or fail rather than score an empty answer."""
    for b in data.get("content", []):
        if b.get("type") == "text":
            return b.get("text", "")
    if data.get("stop_reason") == "max_tokens":
        raise RuntimeError(f"{data.get('model')}: max_tokens reached before any text")
    return ""
