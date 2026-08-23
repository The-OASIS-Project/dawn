# DAWN LLM Integration Guide

## Overview

DAWN supports **four cloud providers** (OpenAI, Claude, Gemini, and OpenRouter — one key, hundreds of models from many vendors) and **local LLM** (llama.cpp or Ollama) for voice command processing. All configuration is done at runtime via `dawn.toml` — no recompilation needed to switch providers or models.

After testing 31+ configurations across 5 local models, we achieved **81.9% quality (B grade)** with local inference — production-ready for voice assistants.

**Local LLM Winner:** Qwen3-4B-Instruct-2507-Q4_K_M @ batch 768
- **Quality:** 81.9% (86/105 points, B grade)
- **TTFT:** 116-138ms (excellent for streaming)
- **Streaming Latency:** ~1.3s perceived (ASR + TTFT + TTS start)

---

## Quick Start

### Option 1: Cloud LLM — Recommended for Quality

**Best for:** Highest accuracy, fastest response, extended thinking support

1. Add your API key to `secrets.toml`:
```toml
openai_api_key = "sk-proj-..."
# Or for other providers:
# claude_api_key = "sk-ant-..."
# gemini_api_key = "AIza..."
```

2. Set provider and model in `dawn.toml`:
```toml
[llm]
type = "cloud"

[llm.cloud]
provider = "openai"    # "openai", "claude", or "gemini"

# Model lists for quick switching via WebUI or voice command
openai_models = ["gpt-5-mini", "gpt-5.2", "gpt-5-nano", "gpt-5"]
claude_models = ["claude-sonnet-4-5", "claude-opus-4-5", "claude-haiku-4-5"]
gemini_models = ["gemini-2.5-flash", "gemini-2.5-pro", "gemini-3-flash-preview"]
```

3. Run DAWN — it uses the first model in the active provider's list.

**Switch providers at runtime** via WebUI settings or voice command ("Switch to Claude").

---

### Option 2: Local LLM — Privacy / Offline

**Best for:** Offline capability, privacy, no API costs, responsive streaming

1. Start llama-server:
```bash
sudo systemctl start llama-server
```

2. Configure in `dawn.toml`:
```toml
[llm]
type = "local"

[llm.local]
endpoint = "http://127.0.0.1:8080"
provider = "auto"    # Auto-detects llama.cpp vs Ollama
# model = ""         # Optional — server may auto-select
```

3. Run DAWN — it connects to the local server.

**Performance:**
- Quality: 81.9% (B grade)
- TTFT: 116-138ms
- Streaming Latency: ~1.3s perceived
- Cost: Free

---

## Cloud Providers

### OpenAI

| Setting | Value |
|---------|-------|
| API Key | `secrets.toml`: `openai_api_key = "sk-proj-..."` |
| Endpoint | Default: `https://api.openai.com/v1` (configurable) |
| Models | gpt-5-mini, gpt-5.2, gpt-5-nano, gpt-5, o3-mini |
| Vision | Supported (enabled by default for cloud) |
| Tools | Native function calling |
| Thinking | Supported via `reasoning_effort` parameter (o-series and GPT-5) |

### Claude (Anthropic)

| Setting | Value |
|---------|-------|
| API Key | `secrets.toml`: `claude_api_key = "sk-ant-..."` |
| Endpoint | Default: `https://api.anthropic.com/v1` |
| Models | claude-sonnet-4-5, claude-opus-4-5, claude-haiku-4-5 |
| Vision | Supported |
| Tools | Native function calling |
| Thinking | Supported via `budget_tokens` parameter |

### Gemini (Google)

| Setting | Value |
|---------|-------|
| API Key | `secrets.toml`: `gemini_api_key = "AIza..."` |
| Endpoint | OpenAI-compatible endpoint (auto-configured) |
| Models | gemini-2.5-flash, gemini-2.5-pro, gemini-3-flash-preview, gemini-3-pro-preview |
| Vision | Supported |
| Tools | Native function calling |
| Thinking | Supported via `reasoning_effort` (Gemini 2.5+; cannot fully disable) |

### OpenRouter (Provider)

[OpenRouter](https://openrouter.ai) is a single API that fronts models from
many vendors. It's a **first-class `provider` value** (not a mode): set
`provider = "openrouter"` and the main chat routes through OpenRouter with one
key. Its model list uses `vendor/model` IDs.

| Setting | Value |
|---------|-------|
| API Key | `secrets.toml`: `openrouter_api_key = "sk-or-..."` (get one at [openrouter.ai/keys](https://openrouter.ai/keys)) |
| Enable | `dawn.toml`: `[llm.cloud] provider = "openrouter"` |
| Endpoint | `https://openrouter.ai/api/v1` (fixed) |
| Models | `vendor/model` IDs, e.g. `anthropic/claude-sonnet-4`, `openai/gpt-5.4`, `google/gemini-2.5-flash` |
| Vision / Tools / Thinking | Depend on the selected upstream model |

```toml
[llm.cloud]
provider = "openrouter"
# Curated shortlist shown in the WebUI/voice model switcher (vendor/model IDs):
openrouter_models = ["anthropic/claude-sonnet-4", "openai/gpt-5.4", "google/gemini-2.5-flash", "meta-llama/llama-3.3-70b-instruct"]
openrouter_default_model_idx = 0
```

> The background tasks (memory-extraction, compaction, silent-observe, scheduler)
> each follow their OWN provider setting — they do **not** inherit the main
> provider. To run one through OpenRouter, set that task's provider to
> `"openrouter"` and put a `vendor/model` slug in its normal model field
> (`compact_model`, `[llm.silent_observe] model`, `memory.extraction_model`);
> empty = the main OpenRouter default.
>
> *(A legacy `use_openrouter = true` config is auto-migrated to
> `provider = "openrouter"` at load and rewritten on the next settings save.)*

### Model Lists and Runtime Switching

Each provider has a configurable model list. The first model is the default; users can switch via the WebUI settings panel or by voice ("Use GPT-5" / "Switch to Claude Opus").

```toml
[llm.cloud]
provider = "openai"

# Each list's first entry is the default when switching to that provider
openai_models = ["gpt-5-mini", "gpt-5.2", "gpt-5-nano", "gpt-5"]
openai_default_model_idx = 0

claude_models = ["claude-sonnet-4-5", "claude-opus-4-5", "claude-haiku-4-5"]
claude_default_model_idx = 0

gemini_models = ["gemini-2.5-flash", "gemini-2.5-pro"]
gemini_default_model_idx = 0

# Custom endpoint (empty = use provider's default URL)
# endpoint = ""
```

---

## Local LLM Setup (llama.cpp)

### Prerequisites

```bash
# Install llama.cpp (if not already installed)
git clone https://github.com/ggerganov/llama.cpp
cd llama.cpp
make GGML_CUDA=1

# Download optimal model
mkdir -p /var/lib/llama-cpp/models
cd /var/lib/llama-cpp/models
wget https://huggingface.co/Qwen/Qwen3-4B-Instruct-GGUF/resolve/main/Qwen3-4B-Instruct-2507-Q4_K_M.gguf
```

### Service Installation

The `services/llama-server/` directory contains systemd service files for automatic startup:

```bash
cd services/llama-server
sudo ./install.sh
```

This installs:
- `/usr/local/bin/llama-server` — Binary
- `/usr/local/etc/llama-cpp/llama-server.conf` — Configuration (optimized settings)
- `/etc/systemd/system/llama-server.service` — Systemd unit
- `/etc/logrotate.d/llama-server` — Log rotation

### Manual Start (for testing)

```bash
/usr/local/bin/llama-server \
    -m /var/lib/llama-cpp/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf \
    --gpu-layers 99 \
    -c 1024 \
    -b 768 \
    -ub 768 \
    -t 4 \
    --temp 0.7 \
    --top-p 0.9 \
    --top-k 40 \
    --repeat-penalty 1.1 \
    --flash-attn \
    --host 127.0.0.1 \
    --port 8080
```

### Local Provider Auto-Detection

DAWN auto-detects whether the local endpoint is llama.cpp or Ollama by probing the server:

```toml
[llm.local]
endpoint = "http://127.0.0.1:8080"    # llama.cpp default
# endpoint = "http://127.0.0.1:11434"  # Ollama default
provider = "auto"    # "auto", "ollama", "llama_cpp", or "generic"
model = ""           # Optional — server may auto-select
vision_enabled = false  # Enable for vision models (LLaVA, Qwen-VL)
```

Detection is cached with a 5-minute TTL (mutex-protected) to avoid repeated probes.

---

## Extended Thinking / Reasoning Mode

DAWN supports extended thinking across all providers — the LLM "thinks through" complex queries before responding. Thinking content is captured and shown in the WebUI debug panel.

### Configuration

```toml
[llm.thinking]
mode = "disabled"           # "disabled" or "enabled"
reasoning_effort = "medium" # "low", "medium", "high"

# Token budgets for each effort level
budget_low = 1024
budget_medium = 8192
budget_high = 16384
# NOTE: Budgets are clamped to 50% of model context size
```

### Provider-Specific Behavior

| Provider | Mechanism | Notes |
|----------|-----------|-------|
| OpenAI (o-series, GPT-5) | `reasoning_effort` parameter | Maps directly to low/medium/high |
| Claude | `budget_tokens` parameter | Uses the token budget for the selected effort level |
| Gemini (2.5+) | `reasoning_effort` parameter | Cannot fully disable thinking on 2.5+ models |
| Local (llama.cpp) | `budget_tokens` parameter | Works with models that support thinking (Qwen3, DeepSeek-R1) |

### Streaming with Thinking

When thinking is enabled, streaming responses include both thinking and text chunks. The WebUI shows thinking content in a collapsible debug section. Thinking blocks include a cryptographic signature for Claude (required for multi-turn conversations).

---

## Rate Limiting

DAWN includes a process-wide sliding window rate limiter to prevent 429 errors from cloud APIs.

```toml
[llm]
rate_limit_enabled = true   # Enable/disable rate limiting
rate_limit_rpm = 40         # Max cloud API requests per minute
```

- Gates all cloud call paths: chat completion, per-session config, and tool loop
- Uses interrupt-aware blocking — cancelled requests release their slot
- Local LLM calls bypass the rate limiter entirely
- Configurable via WebUI settings panel

---

## Tool Calling

DAWN uses native LLM tool calling (function calling) for all providers.

```toml
[llm.tools]
enabled = true              # native tool/function calling on (true) or off (false)
local_enabled = []          # Tools for local voice (empty = all)
remote_enabled = []         # Tools for WebUI/remote (empty = all)
```

Tools execute in parallel when the LLM requests multiple independent actions. Each tool has a safety classification that determines whether it can run without confirmation.

See **[TOOL_DEVELOPMENT_GUIDE.md](TOOL_DEVELOPMENT_GUIDE.md)** for writing new tools.

---

## Optimal Local LLM Settings (Tested)

### Qwen3-4B Q4 Parameters

After testing **31+ configurations**, these settings achieve optimal quality:

| Parameter | Value | Why |
|-----------|-------|-----|
| **Model** | Qwen3-4B-Instruct-2507-Q4_K_M.gguf | Best speed/quality balance |
| **Batch Size** | 768 | **CRITICAL:** 81.9% quality (vs 18% at 256) |
| **Context** | 1024 | **CRITICAL:** Required for quality (56% at 512) |
| **GPU Layers** | 99 | Offload everything to GPU |
| Temperature | 0.7 | No effect on quality (tested 0.5-1.0) |
| Top-K | 40 | No effect on quality (tested 20-100) |
| Top-P | 0.9 | No effect on quality (tested 0.8-0.95) |
| Repeat Penalty | 1.1 | No effect on quality (tested 1.0-1.3) |

**Key Finding:** After testing 19 parameters, **only batch size and context size matter**. All sampling parameters (temperature, top-k, top-p, repeat penalty) have **zero effect** on quality.

### Model Comparison (All @ Batch 768)

| Model | Quality | TTFT | Speed | Recommendation |
|-------|---------|------|-------|----------------|
| **Qwen3-4B Q4** | **81.9%** | 116-138ms | 13.5 tok/s | **Production** (balanced) |
| Qwen2.5-7B Q4 | **85.7%** | 181-218ms | 9.8 tok/s | Quality-focused option |
| Llama-3.2-3B | **71.4%** | 92-108ms | 18.0 tok/s | Speed-focused option |
| Phi-3-mini | 41.9% | ? | ? | Poor instruction following |
| Qwen3-4B Q6 | 36.2% | ? | ? | Template issues (`<think>` loops) |

**Three Viable Options:**
1. **Qwen3-4B Q4 (Recommended):** Best balance of quality + TTFT
2. **Qwen2.5-7B Q4:** Best quality (85.7%), only +80ms TTFT vs Qwen3-4B
3. **Llama-3.2-3B:** Fastest TTFT (92ms), acceptable quality (71.4%)

---

## Performance Comparison

| Metric | Cloud | Local (Qwen3-4B) |
|--------|-------|-------------------|
| **Quality** | 92-100% | 81.9% |
| **Speed** | ~1.2s LLM | ~3.4s LLM |
| **Total Latency** | ~3.1s | ~5.2s |
| **Offline?** | No | Yes |
| **Privacy** | Data sent to API | Fully local |
| **Cost** | ~$0.01-0.02 each | Free |
| **Thinking** | All providers | llama.cpp (Qwen3, DeepSeek-R1) |

---

## Complete Configuration Reference

All LLM settings in `dawn.toml`:

```toml
[llm]
type = "cloud"                  # "cloud" or "local"
max_tokens = 4096               # Maximum response tokens
compact_soft_threshold = 0.60   # Async background compaction trigger
compact_hard_threshold = 0.85   # Blocking compaction trigger (safety net)
compact_use_session = true      # Use session's LLM for compaction
compact_provider = ""           # Dedicated compaction provider (claude/openai/gemini/local)
compact_model = ""              # Dedicated compaction model (e.g., claude-haiku-4-5)
conversation_logging = false    # Save chat history to log files
rate_limit_enabled = true       # Throttle cloud API calls
rate_limit_rpm = 40             # Max requests per minute

[llm.cloud]
provider = "openai"             # "openai", "claude", or "gemini"
endpoint = ""                   # Custom endpoint (empty = provider default)
vision_enabled = true           # Cloud models support vision by default

openai_models = ["gpt-5-mini", "gpt-5.2", "gpt-5-nano", "gpt-5"]
openai_default_model_idx = 0
claude_models = ["claude-sonnet-4-5", "claude-opus-4-5", "claude-haiku-4-5"]
claude_default_model_idx = 0
gemini_models = ["gemini-2.5-flash", "gemini-2.5-pro"]
gemini_default_model_idx = 0

[llm.local]
endpoint = "http://127.0.0.1:8080"
model = ""
provider = "auto"               # "auto", "ollama", "llama_cpp", "generic"
vision_enabled = false

[llm.tools]
enabled = true              # native tool/function calling on (true) or off (false)

[llm.thinking]
mode = "disabled"               # "disabled" or "enabled"
reasoning_effort = "medium"     # "low", "medium", "high"
budget_low = 1024
budget_medium = 8192
budget_high = 16384
```

API keys go in `secrets.toml` (separate file, mode 0600):
```toml
openai_api_key = "sk-proj-..."
claude_api_key = "sk-ant-..."
gemini_api_key = "AIza..."
```

Environment variable overrides are available for all settings. Pattern: `DAWN_<SECTION>_<KEY>` (e.g., `DAWN_LLM_TYPE`, `DAWN_LLM_CLOUD_PROVIDER`). API keys use: `OPENAI_API_KEY`, `CLAUDE_API_KEY`, `GEMINI_API_KEY`.

---

## Troubleshooting

### Local LLM Issues

**Problem:** Low quality (<70%)
- **Check:** Batch size must be 768+
- **Check:** Context size must be 1024+
- **Fix:** Update `llama-server.conf` with optimal settings

**Problem:** Out of memory errors
```
cudaMalloc failed: out of memory
```
- **Cause:** Memory fragmentation (Jetson specific)
- **Fix 1:** Reboot to clear fragmentation
- **Fix 2:** Reduce batch size to 512 (quality drops to ~28%)

**Problem:** Server won't start
- **Check:** Port 8080 not already in use: `netstat -tulpn | grep 8080`
- **Check:** Model file exists: `ls -lh /var/lib/llama-cpp/models/*.gguf`
- **Check:** Logs: `journalctl -u llama-server -f`

### Cloud LLM Issues

**Problem:** API errors (401 Unauthorized)
- **Fix:** Check API key in `secrets.toml` matches the active provider

**Problem:** Rate limiting (429 Too Many Requests)
- **Fix:** DAWN has a built-in rate limiter (`rate_limit_rpm`, default 40). Lower the value if still hitting limits, or check if multiple DAWN instances share the same API key.

**Problem:** Unknown cloud provider error
- **Fix:** `provider` must be `"openai"`, `"claude"`, or `"gemini"` (case-sensitive)

---

## Testing & Validation

### Quality Test

```bash
cd llm_testing/scripts
python3 test_llm_quality.py
```

### Speed Test

```bash
cd llm_testing/scripts
./test_single_model.sh Qwen3-4B-Instruct-2507-Q4_K_M.gguf
```

### Test Results Archive

Complete testing methodology and results:
- **Location:** `llm_testing/results/`
- **Config search:** 7 batch sizes tested, found batch 1024→768 breakthrough
- **Fine-tuning:** 19 parameters tested, only batch/context matter

---

## References

- **llama.cpp:** https://github.com/ggerganov/llama.cpp
- **Qwen3-4B Model:** https://huggingface.co/Qwen/Qwen3-4B-Instruct-GGUF
- **OpenAI API:** https://platform.openai.com/docs/api-reference
- **Claude API:** https://docs.anthropic.com/en/docs
- **Gemini API:** https://ai.google.dev/docs
- **Tool Development:** [TOOL_DEVELOPMENT_GUIDE.md](TOOL_DEVELOPMENT_GUIDE.md)
- **Test Scripts:** `llm_testing/scripts/`
- **Service Files:** `services/llama-server/`
