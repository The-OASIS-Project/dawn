# Llama.cpp Server as a Systemd Service

This package provides a clean, production-ready setup for running llama-server as a systemd service with **optimal DAWN settings**.

## Model Options

All speeds measured on **Jetson AGX Orin 64GB at MAXN (60W)** power mode.
Run `sudo nvpmodel -m 0 && sudo jetson_clocks` before benchmarking.
At 30W mode, speeds are ~3x slower.

### Recommended Configurations

| Use Case | Model | Preset | Speed | Quality | Vision |
|----------|-------|--------|-------|---------|--------|
| **Home (64GB Orin)** | Qwen3.6 35B-A3B MoE | J | 37.01-37.22 tok/s | 94.0% (A) | Yes |
| **Helmet (16GB Orin)** | Gemma 3 4B IT | A3 | 13.9 tok/s (28W) | 89.7% (B) | Yes |
| **Voice only (any)** | Qwen3 4B Instruct | A | 36.71 tok/s | 94.8% (A) | No |

### Full Benchmark Results (AGX Orin 64GB MAXN)

**All figures below were measured in a single sweep on 2026-09-03, llama.cpp
b10626, AGX Orin 64GB @ MAXN, 116-point FRIDAY suite** — one build, one machine,
one suite, so the rows are directly comparable to each other for the first time.

TTFT is the range across the harness's three probes, which use **~66-token
prompts** — far smaller than anything DAWN actually sends, so this column is
useful for comparing models *to each other* and useless as a latency estimate.
See [Latency: what actually governs perceived speed](#latency-what-actually-governs-perceived-speed)
for real numbers. Speed is the mean of the same three probes.

Quality carries real run-to-run variance at temp 0.7 — where a model was run
more than once the spread is given. Treat a 1-3 point gap between models as
noise, not a ranking.

**4B class (voice-viable on all hardware):**

| Model | Type | Size | Quality | Speed | TTFT (66-tok) | Vision | Notes |
|-------|------|------|---------|-------|---------------|--------|-------|
| **Gemma 3 4B IT** | Dense | 2.5 GB | 89.7% (B) | 37.93 tok/s | 84-124 ms | Yes | Fastest 4B + vision |
| **Qwen3 4B Instruct** | Dense | 2.5 GB | **94.8% (A)** | 36.71 tok/s | **45-59 ms** | No | Best TTFT of any preset |
| Qwen3.5 4B | SSM hybrid | 2.9 GB | 89.7% (B) | 31.19 tok/s | 163-224 ms | Yes | Vision, SSM overhead |

*(Gemma 3 4B on a 16GB Orin @ 28W measured 13.9 tok/s — different hardware, not
re-run in this sweep.)*

**12B+ class (mixed voice/WebUI):**

| Model | Type | Size | Quality | Speed | TTFT (66-tok) | Vision | Notes |
|-------|------|------|---------|-------|---------------|--------|-------|
| Gemma 3 12B IT | Dense | 7.3 GB | 89.7% (B) | 16.51 tok/s | 201-297 ms | Yes | WebUI quality tier |

**MoE class (voice-viable on 64GB):**

| Model | Active/Total | Size | Quality | Speed | TTFT (66-tok) | Vision | Notes |
|-------|-------------|------|---------|-------|---------------|--------|-------|
| **Qwen3.6 35B-A3B** | ~3B/35B | 20.1 GB | **94.0% (A)** *(91.4-94.0, 4 runs)* | **37.01-37.22 tok/s** | 264-332 ms | Yes | **Home recommended** (Preset J), 128K ctx |
| Qwen3.5 35B-A3B | 3B/35B | 19.9 GB | 91.4% (A) | 34.97 tok/s | 255-391 ms | Yes | Stable alt (Preset F), 128K ctx |
| Gemma 4 26B-A4B | 4B/25B | 15.9 GB | 93.1% (A) | 31.48 tok/s | 235-277 ms | Yes | Text now works with `--reasoning off`; vision/tools unverified |

**Dense large (WebUI only on 64GB):**

| Model | Params | Size | Quality | Speed | TTFT (66-tok) | Vision | Notes |
|-------|--------|------|---------|-------|---------------|--------|-------|
| **Gemma 4 31B** | 30.7B | 18.2 GB | **97.4% (A)** | 6.75 tok/s | 659-730 ms | Yes | **Highest local quality measured** |
| Qwen3.8 27B | 27B | 16.5 GB | 94.0% (A) *(93.1-95.7, 3 runs)* | 8.28 tok/s | 625-772 ms | Yes | Preset K (unsloth UD quant) |
| Qwen3-Coder-Next | 80B/3B MoE | 42.2 GB | 94.0% (A) | 34.37 tok/s | 342-390 ms | No | Coding specialist, 256K ctx |
| Qwen3.5 27B | 26.9B | 15.9 GB | 93.1% (A) | 7.55 tok/s | 656-948 ms | Yes | Too slow for voice |
| Qwen3.6 27B | 27B | 17.5 GB | 92.2% (A) *(89.7-92.2, 2 runs)* | 7.47 tok/s | 658-803 ms | Yes | Qwen's flagship 27B coder |

### Cloud Baseline Comparison (Claude API)

For reference — how local Jetson models compare to cloud flagships on the same
13-test FRIDAY instruction-following suite:

| Model | Quality | Type |
|-------|---------|------|
| Claude Opus 4.7 | 99.1% | Cloud flagship |
| Claude Haiku 4.5 | 96.5% | Cloud |
| Claude Sonnet 4.6 | 96.5% | Cloud |
| **Gemma 4 31B (local)** | **97.4%** | Jetson — highest local, but 6.75 tok/s |
| Qwen3.8 27B (local) | 94.0% | Jetson (Preset K) — 8.28 tok/s |
| **Qwen3.6 35B-A3B (local)** | **94.0%** | **Jetson AGX Orin 64GB (Preset J)** |
| Qwen3 4B (local) | 94.8% | Jetson — any hardware |
| Qwen3-Coder-Next (local) | 94.0% | Jetson — coding, no vision |
| Gemma 4 26B-A4B (local) | 93.1% | Jetson (Preset G) |
| Qwen3.5 35B-A3B (local) | 91.4% | Jetson — stable alt (Preset F) |

Gemma 4 31B at 97.4% now sits **above** Claude Haiku/Sonnet (96.5%) on this
suite and within 1.7 points of Opus — at zero cost, fully offline. It is also
6.75 tok/s, so it buys that quality at roughly 5x the latency of Preset J.
The production pick trades ~3.4 points of quality for 5.5x the speed.

Caveat: this is a 13-test, 116-point instruction-following suite with ±3 points
of run-to-run variance. It measures DAWN's command/tool-formatting behaviour,
not general capability — do not read it as a general model ranking.

### Latency: what actually governs perceived speed

**tok/s is not the number that matters.** Generation speed decides how fast a
reply *continues*; **prompt processing** decides how long the user waits before
hearing anything. On long prompts the second dominates, and the two rank models
differently — Qwen 3.6 27B and Qwen 3.5 35B-A3B are one tok/s tier apart but
**2.5x** apart on prompt processing.

#### Three different prompt sizes — don't mix them up

Earlier revisions of this document quoted "DAWN's ~1000-token system prompt".
That figure is the **quality suite's** prompt, not production's. Measured:

| Context | Prompt size | What it is |
|---|---|---|
| `test_llama_performance.sh` | **~66 tokens** | the TTFT column in the tables above |
| `test_llm_quality.py` | **1,145 tokens** (1,139–1,607) | the FRIDAY suite |
| **Real DAWN traffic** | **median 2,360**, p90 ~6k, max 52k | what users actually pay for |

Production numbers come from 2,318 prompt-eval events in
`/var/log/llama-cpp/error.log`, attributed to Preset J by tracking which model
was loaded at the time.

#### Prompt-processing rate (the durable figure)

Processing is near-linear in prompt size, so the useful published constant is a
**rate**, not a single TTFT — it predicts any prompt size and does not go stale
as the prompt grows:

| Model | Prompt processing | Median TTFT in production | p90 |
|---|---|---|---|
| Qwen3.6 35B-A3B (Preset J) | **~482 tok/s** | 3,834 ms | 12,699 ms |
| Qwen3.5 35B-A3B (Preset F) | ~562 tok/s | 1,270 ms\* | 9,915 ms |
| Qwen3.6 27B (Preset I) | **~219 tok/s** | 7,689 ms | 35,796 ms |

\* *Preset F's median reflects smaller prompts in its sample (p50 708 tok), not
a faster model — compare the rate column, not the median.*

    TTFT ≈ prompt_tokens ÷ prompt_rate

#### Preset J measured curve (n=2,318)

| Prompt tokens | n | Median TTFT | p90 |
|---|---|---|---|
| 0–500 | 424 | 476 ms | 680 ms |
| 500–1k | 116 | 1,349 ms | 1,378 ms |
| 1k–2k | 517 | 1,993 ms | 3,120 ms |
| 2k–4k | 396 | 6,663 ms | 9,701 ms |
| 4k–8k | 845 | 10,355 ms | 15,047 ms |
| 8k–16k | 10 | 17,423 ms | 23,641 ms |

**Overall median 3.8 s, p90 12.7 s, p99 26.4 s.** Earlier revisions claimed
"~1.3 s"; that was never measured against a production prompt.

Prompt caching matters as much as the model: the same log holds **1,749
"re-processing due to lack of cache data"** events, each paying full price. Any
single TTFT number is meaningless without saying whether the prefix was cached.

**Known gap:** these logs cannot separate voice turns from RAG/document
workloads — the 52k-token prompts are certainly not voice. Closing that needs
DAWN's own `ttft_ms` (already computed in `session_manager_llm.c`, currently
sent to the WebUI and discarded) logged with the session type.

### Local vs cloud: measured, same suite

Identical 13-test FRIDAY suite, `max_tokens=150`, ~1,145-token prompt. This is
**end-to-end response time**, not TTFT. Cloud figures include network RTT from
this location.

| Model | Quality | Median | Mean | Max |
|---|---|---|---|---|
| Claude Opus 4.7 | 99.1% | 1.55 s | 1.79 s | 2.88 s |
| Claude Sonnet 4.6 | 96.6% | 1.64 s | 1.84 s | 4.38 s |
| Claude Haiku 4.5 | 96.6% | 1.06 s | 1.27 s | 2.19 s |
| **Qwen3.6 35B-A3B (Preset J)** | 94.0% | **0.75 s** | 0.99 s | 2.85 s |
| Qwen3-Coder-Next (H) | 94.0% | 0.65 s | 1.11 s | 3.69 s |
| Qwen3 4B Instruct (A) | 94.8% | 1.06 s | 1.13 s | 1.65 s |
| Gemma 4 26B-A4B (G) | 93.1% | 1.41 s | 1.50 s | 2.64 s |
| Gemma 3 12B (C) | 89.7% | 2.36 s | 2.57 s | 4.00 s |
| Qwen3.8 27B (K) | 95.7% | 3.99 s | 4.43 s | 10.05 s |
| Gemma 4 31B (D) | 97.4% | 5.68 s | 5.85 s | 11.27 s |

**Preset J is faster end-to-end than every Claude model tested** — 0.75 s vs
Haiku's 1.06 s — for 2.6 quality points, at zero cost and fully offline.

⚠ **These are warm-cache numbers.** The suite reuses one system prompt across
all 13 tests, so after the first call llama.cpp has the 1,145-token prefix
cached and prompt processing costs ~nothing — Preset J's 0.75 s median is
essentially generation time alone (~32 completion tokens at 37 tok/s ≈ 0.86 s).
It is *not* what a cold turn costs.

That is why a benchmark at 1,145 tokens finishes in under a second while
production, at a 2,360-token median, shows 3.8 s: production hits the cache far
less often (1,749 recorded misses). Read the table as "local is competitive with
cloud **when the prefix is cached**" — which is what DAWN's two-segment prompt
design is built to achieve, and worth verifying rather than assuming. Cloud
providers are much less sensitive to prompt growth and to cache state, so the
local advantage narrows, and can invert, as prompts grow or caching misses.

### Context Scaling: Qwen3.5/3.6 35B-A3B on AGX Orin 64GB MAXN

The hybrid SSM+Transformer architecture (30 SSM + 10 attention layers) makes
context scaling nearly free. Only the 10 attention layers grow KV cache with
context size, and with only 2 KV heads per layer the cost is minimal.

Numbers below are measured on **Qwen 3.5 35B-A3B (Preset F)**. **Qwen 3.6
35B-A3B (Preset J)** inherits the same architecture and shows the same flat
scaling — 128K context confirmed working in production.

| Context | KV Cache | Gen Speed | TTFT | Free Memory |
|---------|----------|-----------|------|-------------|
| 32K | 340 MB | 30.0 tok/s | 188 ms | ~30 GB |
| 64K | 680 MB | 30.1 tok/s | 171 ms | ~29.7 GB |
| **128K** | **1360 MB** | **30.2 tok/s** | **168 ms** | **~29 GB** |

128K is the recommended context for Presets F and J on AGX Orin 64GB. Zero
performance penalty vs 32K, with 4x the usable context for heavy tool workflows.

### Qwen 3.8 Status (August 2026) — Preset K, benchmarked

The Qwen3.8 open-weight release has exactly two models: **Qwen3.8-27B** (dense,
vision, Apache 2.0, 262144 native context) and **Qwen3.8-2.4T-A95B** (2.4T total
/ 95B active — does not fit on this hardware, and not an open licence).

**There is no Qwen 3.8 model in the 35B-A3B MoE class.** Qwen 3.8 therefore does
*not* offer a successor to Preset J, which remains the production recommendation.
Preset K competes with Preset I (Qwen 3.6 27B) only, and since it is the same
weight class with the same hybrid linear/full-attention layout, expect roughly
the same ~7 tok/s — a WebUI-tier quality comparison, not a speed upgrade.

**Benchmarked 2026-08-27** on llama.cpp b10626, using the **unsloth Dynamic
(UD) Q4_K_M** quant — `unsloth/Qwen3.8-27B-GGUF`, 16.5 GB:

| | Qwen 3.6 27B (Preset I) | Qwen 3.8 27B (Preset K) | Qwen 3.6 35B-A3B (Preset J) |
|---|---|---|---|
| Quality | 92.2% (b8667) | **93.1%** (108/116, 1 run) | 91.4–94.0% (2 runs) |
| Speed | 7.47 tok/s | **8.28 tok/s** | 37.11 tok/s |
| Tier | WebUI | WebUI | **voice + WebUI** |

**Verdict: no production impact.** Qwen 3.8 27B edges out its predecessor
Qwen 3.6 27B on both axes (+0.9 points, +19% speed), but the quality delta is
inside this suite's ±3-point run-to-run variance, so only the speed gain is
solid. Against Preset J it is 4.5x slower; on quality the two overlap inside the
suite's ±3-point variance (K 93.1% from one run vs J 91.4-94.0% from two), so
no quality claim either way is supportable without more runs. With no
Qwen 3.8 model in the 35B-A3B MoE class, the 3.8 generation offers DAWN
nothing for the voice path — **Preset J remains the recommendation**.

Caveat on the I-vs-K comparison: Preset I's 92.2% was measured on b8667 and is
not reproducible on b10626 without `REASONING_MODE=off`, so that column is
cross-build. Re-run Preset I on b10626 for a clean generational comparison.

The harness prints "❌ NOT RECOMMENDED" for Preset K — that is purely its
25 tok/s voice-viability gate, not a failure. Preset I trips the same gate.

`general.architecture` is **`qwen35`** — Qwen 3.8 reuses the Qwen 3.5
implementation, which is why a llama.cpp tree containing no "qwen38" string
loads it fine. Vision verified working with `mmproj-F16.gguf`. The GGUF also
carries MTP/NextN tensors that llama.cpp ignores on this arch (`unused tensor
blk.64.nextn.*` warnings, ~260 MB of dead weight — harmless).

Configuration notes captured from the official model card and template source:

- **Non-thinking (what DAWN uses):** temp 0.7, top_p 0.80, top_k 20, min_p 0.0,
  repetition_penalty 1.0, **presence_penalty 1.5**.
- **Thinking:** temp 1.0, top_p 0.95, top_k 20, presence_penalty 0.0.
- `presence_penalty` is a **new knob for this repo** — Preset K is the first
  preset to set it. See `PRESENCE_PENALTY` in the config reference below.
- **Thinking is ON by default**, the same trap that scored Qwen 3.6 35B-A3B at
  18%. The template reads `enable_thinking is undefined or enable_thinking is
  true`; the way to turn it off is `REASONING_MODE=off` →
  `--reasoning off`. Qwen 3.8 additionally supports `reasoning_effort`
  (`xhigh` default / `medium` / `low`) if a graded dial is wanted.
- **KV cache is cheap**: only 16 of 64 layers carry one — 64 KB/token at f16,
  ~34 KB/token at the `q8_0` cache type this service uses. At Q4_K_M the
  resident set is ~19.8 GB at 32K, ~23 GB at 128K, ~27.5 GB at 262K.
- **Filename change**: bartowski dropped the `Qwen_` prefix this generation.

Preset K ships at `CONTEXT_SIZE=32768`, which is what was benchmarked. Native
context is 262144; raise it only after a context-scaling pass.

Preset K sets `MMPROJ` (`mmproj-Qwen3.8-27B-f16.gguf`) different from
`MMPROJ_FILE` (`mmproj-F16.gguf`): unsloth ships a generic projector name that
would collide with other models in the shared models directory, so the
installer renames it after download.

### llama.cpp b10626 upgrade (2026-08-25)

Rebuilt from b8667 → **b10626**. Preset J regression gate re-run on 2026-08-27:

| | b8667 | b10626 |
|---|---|---|
| Speed | 32.07 tok/s | **37.0-37.2 tok/s** (+16%) |
| Quality | 94.8% (110/116) | 94.0% (109/116), 91.4% (106/116) |
| TTFT *(66-token prompt)* | 201 / 241 / 301 ms | 252 / 287 / 308 ms |

Speed is the only clear win. Quality is flat within noise — the suite shows ±3
points run-to-run at temp 0.7 (94.8 / 94.0 / 91.4 across three runs), so do not
read a 1–2 point move as a regression in either direction.

**TTFT did not improve.** `test_llama_performance.sh` sends ~66-token prompts;
on that like-for-like basis b10626 is flat to marginally *worse* than b8667
(201/241/301 → 252/287/308 ms). Real-world TTFT is a different and much larger
number — see the next section.

**Three behaviour changes this upgrade introduced:**

1. `--chat-template-kwargs {"enable_thinking":false}` is **deprecated** in
   favour of `--reasoning on|off|auto`. It still works but warns.
2. Its env var was **renamed** `LLAMA_CHAT_TEMPLATE_KWARGS` →
   `LLAMA_ARG_CHAT_TEMPLATE_KWARGS`. The generated config used the old name, so
   the line was read by nobody and thinking came back ON silently. This is why
   presets now emit `REASONING_MODE` instead.
3. `--reasoning-format` now defaults to `auto` (extracts thoughts) and `--jinja`
   defaults to enabled. **Preset I is affected**: it needed no thinking-disable
   on b8667 and does on b10626, otherwise `content` returns empty. Its recorded
   92.2% is not reproducible on b10626 without `REASONING_MODE=off`.

**Benchmark pitfall found the same day.** `test_single_model.sh` calls
`killall llama-server`, which cannot touch the systemd service (that runs as
user `llama`). The bench's own server then failed to bind port 8080, exited,
and every probe was answered by the *service* — silently benchmarking Qwen 3.6
**27B** with thinking on and reporting it as the 35B-A3B MoE (7.48 tok/s,
19.8%). The harness now refuses to run if the port is held, if its own server
dies during startup, or if `/v1/models` reports a model other than the one
requested. **Always `sudo systemctl stop llama-server` before benchmarking.**

### Gemma 4 Status (April 2026)

**Thinking leak: mostly fixed** on CUDA in llama.cpp b8738+ (PRs #21326, #21327,
#21343, #21390, #21566 merged early April). Use `--reasoning off` or
`--chat-template-kwargs '{"enable_thinking":false}'` with a quantized GGUF
(Q4/Q5/Q8 — F16 may still loop).

**Text quality is now measurable and good** (2026-09-03 sweep, b10626, with
`REASONING_MODE=off`): Gemma 4 31B scored **97.4%** — the highest local result
on this suite — and Gemma 4 26B-A4B scored **93.1% at 31.48 tok/s**, which is
voice-viable. The thinking problem is solved by `--reasoning off`; what remains
is everything else.

**Still blocking for DAWN** (not re-verified on b10626 — status below is from
April and needs re-checking before either preset is promoted):
- **Vision (mmproj) on CUDA crashes** — issue #21402 still open. DAWN uses
  vision on the home Orin, so this alone blocks production use.
- **Tool calling loops** — issue #21375 (peg-gemma4 parser) still open.
  DAWN relies heavily on tool calling.

Both were re-run on b10626 in the 2026-09-03 sweep: Gemma 4 26B-A4B is
**93.1% at 31.48 tok/s** vs Qwen 3.6 35B-A3B's **94.0% at 37.1 tok/s** — so on
the current build Qwen 3.6 is ahead on both axes, and the "ties on quality,
runs faster" claim from April no longer holds. Gemma 4 26B-A4B is a credible
fallback rather than an upgrade, and only if #21402 and #21375 are confirmed
closed.

**Current recommendation:** Use **Qwen 3.6 35B-A3B** (Preset J) for production
on AGX Orin 64GB — fastest of anything scoring ≥94%. Qwen 3.5 35B-A3B (Preset F)
is the stable alt, though the sweep put it at 91.4%, below its previously
recorded 94.8%. Gemma **3** models do not have any of the Gemma 4 issues.

### Hardware: Jetson AGX Orin 64GB Developer Kit

| Spec | Value |
|------|-------|
| GPU | Ampere (SM 8.7), 2048 CUDA cores, 1.3 GHz (MAXN) |
| Memory | 64 GB unified LPDDR5, ~204 GB/s bandwidth |
| CPU | 12-core Arm Cortex-A78AE, 2.2 GHz (MAXN) |
| Power modes | MAXN (60W), 30W, 15W |
| CUDA compute | 8.7 |

---

## Quick Start

The installer auto-detects your hardware and recommends a preset. Just run:

```bash
sudo ./install.sh
```

Or install a specific preset non-interactively:

```bash
# AGX Orin 64GB: Qwen 3.6 35B-A3B MoE (recommended — 94.0%, ~37 tok/s, vision)
sudo ./install.sh -P J

# Small hardware: Qwen3 4B Instruct (94.8% quality, 36.71 tok/s, no vision)
sudo ./install.sh -P A
```

The installer will download model files automatically if they're missing (requires `hf` CLI).

### Manual Download (if needed)

```bash
# Preset J: Qwen 3.6 35B-A3B MoE (recommended)
hf download bartowski/Qwen_Qwen3.6-35B-A3B-GGUF \
  Qwen_Qwen3.6-35B-A3B-Q4_K_M.gguf \
  --local-dir /var/lib/llama-cpp/models/

hf download bartowski/Qwen_Qwen3.6-35B-A3B-GGUF \
  mmproj-Qwen_Qwen3.6-35B-A3B-f16.gguf \
  --local-dir /var/lib/llama-cpp/models/

# Preset F: Qwen 3.5 35B-A3B MoE (stable alt)
hf download bartowski/Qwen_Qwen3.5-35B-A3B-GGUF \
  Qwen_Qwen3.5-35B-A3B-Q4_K_M.gguf \
  --local-dir /var/lib/llama-cpp/models/

hf download bartowski/Qwen_Qwen3.5-35B-A3B-GGUF \
  mmproj-Qwen_Qwen3.5-35B-A3B-f16.gguf \
  --local-dir /var/lib/llama-cpp/models/

# Preset A: Qwen3 4B Instruct
hf download unsloth/Qwen3-4B-Instruct-2507-GGUF \
  Qwen3-4B-Instruct-2507-Q4_K_M.gguf \
  --local-dir /var/lib/llama-cpp/models/

# Preset K: Qwen 3.8 27B (requires llama.cpp b10419+ to load this GGUF)
# Note: no "Qwen_" filename prefix this generation.
hf download bartowski/Qwen3.8-27B-GGUF \
  Qwen3.8-27B-Q4_K_M.gguf \
  --local-dir /var/lib/llama-cpp/models/

hf download bartowski/Qwen3.8-27B-GGUF \
  mmproj-Qwen3.8-27B-f16.gguf \
  --local-dir /var/lib/llama-cpp/models/
```

### Qwen 3.5 27B Vision (AGX Orin 64GB)

```bash
# Download model and vision projector
hf download bartowski/Qwen_Qwen3.5-27B-GGUF \
  Qwen_Qwen3.5-27B-Q4_K_M.gguf \
  --local-dir /var/lib/llama-cpp/models/

hf download bartowski/Qwen_Qwen3.5-27B-GGUF \
  mmproj-Qwen_Qwen3.5-27B-f16.gguf \
  --local-dir /var/lib/llama-cpp/models/

# Install service
sudo ./install.sh -m /var/lib/llama-cpp/models/Qwen_Qwen3.5-27B-Q4_K_M.gguf \
  --mmproj /var/lib/llama-cpp/models/mmproj-Qwen_Qwen3.5-27B-f16.gguf
```

### Extended Thinking Mode

```bash
# Download thinking model
huggingface-cli download unsloth/Qwen3-4B-Thinking-2507-GGUF \
  Qwen3-4B-Thinking-2507-Q4_K_M.gguf \
  --local-dir /var/lib/llama-cpp/models/

# Copy thinking template
sudo cp qwen3_thinking.jinja /var/lib/llama-cpp/templates/

# Edit config before install (or after, see Switching Modes)
nano llama-server.conf
# Uncomment Thinking MODEL line, comment Instruct MODEL line
# Set REASONING_FORMAT=deepseek

# Install service
sudo ./install.sh
```

---

## Switching Between Instruct and Thinking Modes

Edit `/usr/local/etc/llama-cpp/llama-server.conf`:

### For Instruct Mode (default):
```bash
MODEL="/var/lib/llama-cpp/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"
TEMPLATE="/var/lib/llama-cpp/templates/qwen3_chatml.jinja"
REASONING_FORMAT=none
```

### For Thinking Mode:
```bash
MODEL="/var/lib/llama-cpp/models/Qwen3-4B-Thinking-2507-Q4_K_M.gguf"
TEMPLATE="/var/lib/llama-cpp/templates/qwen3_thinking.jinja"
REASONING_FORMAT=deepseek
MMPROJ=
```

### For Qwen 3.6 35B-A3B Vision MoE (AGX Orin 64GB, ⭐ recommended):
```bash
MODEL="/var/lib/llama-cpp/models/Qwen_Qwen3.6-35B-A3B-Q4_K_M.gguf"
TEMPLATE=
REASONING_FORMAT=deepseek
REASONING_MODE=off
MMPROJ="/var/lib/llama-cpp/models/mmproj-Qwen_Qwen3.6-35B-A3B-f16.gguf"
CONTEXT_SIZE=131072
```

Qwen 3.6 ships with thinking ON by default; `REASONING_MODE=off` (which becomes
`--reasoning off`) is required to disable it. Without it, generation is siphoned
into `reasoning_content`, `content` comes back **empty**, and the FRIDAY suite
scores ~20% at completely normal speed.

### For Qwen 3.5 35B-A3B Vision MoE (AGX Orin 64GB, stable alt):
```bash
MODEL="/var/lib/llama-cpp/models/Qwen_Qwen3.5-35B-A3B-Q4_K_M.gguf"
TEMPLATE=
REASONING_FORMAT=deepseek
REASONING_MODE=off
MMPROJ="/var/lib/llama-cpp/models/mmproj-Qwen_Qwen3.5-35B-A3B-f16.gguf"
CONTEXT_SIZE=131072
```

### For Gemma 4 26B-A4B Vision MoE (AGX Orin 64GB):
```bash
MODEL="/var/lib/llama-cpp/models/google_gemma-4-26B-A4B-it-Q4_K_M.gguf"
TEMPLATE=
REASONING_FORMAT=deepseek
MMPROJ="/var/lib/llama-cpp/models/mmproj-google_gemma-4-26B-A4B-it-f16.gguf"
CONTEXT_SIZE=32768
TEMPERATURE=1.0
TOP_P=0.95
TOP_K=64
REPEAT_PENALTY=1.0
```

### For Qwen 3.8 27B Vision dense (AGX Orin 64GB; this GGUF needs llama.cpp b10419+):
```bash
MODEL="/var/lib/llama-cpp/models/Qwen3.8-27B-Q4_K_M.gguf"
TEMPLATE=
REASONING_FORMAT=deepseek
REASONING_MODE=off
MMPROJ="/var/lib/llama-cpp/models/mmproj-Qwen3.8-27B-f16.gguf"
CONTEXT_SIZE=32768
REPEAT_PENALTY=1.0
PRESENCE_PENALTY=1.5
```

`PRESENCE_PENALTY=1.5` is Qwen's documented non-thinking recommendation; leave
the key out entirely for any model that does not ask for it.

### For Qwen 3.5 27B Vision dense (AGX Orin 64GB):
```bash
MODEL="/var/lib/llama-cpp/models/Qwen_Qwen3.5-27B-Q4_K_M.gguf"
TEMPLATE=
REASONING_FORMAT=deepseek
MMPROJ="/var/lib/llama-cpp/models/mmproj-Qwen_Qwen3.5-27B-f16.gguf"
CONTEXT_SIZE=32768
```

### For Gemma 4 31B Vision (AGX Orin 64GB):
```bash
MODEL="/var/lib/llama-cpp/models/google_gemma-4-31B-it-Q4_K_M.gguf"
TEMPLATE=
REASONING_FORMAT=deepseek
MMPROJ="/var/lib/llama-cpp/models/mmproj-google_gemma-4-31B-it-f16.gguf"
CONTEXT_SIZE=32768
TEMPERATURE=1.0
TOP_P=0.95
TOP_K=64
REPEAT_PENALTY=1.0
```

Then restart:
```bash
sudo systemctl restart llama-server
```

### DAWN Configuration

When using Thinking mode, also update `dawn.toml`:
```toml
[llm.thinking]
mode = "enabled"
budget_tokens = 5000
```

---

## Configuration Reference

### llama-server.conf Options

| Variable | Default | Description |
|----------|---------|-------------|
| `MODEL` | Qwen3-4B-Instruct | Path to GGUF model file |
| `TEMPLATE` | qwen3_chatml.jinja | Chat template file |
| `REASONING_FORMAT` | none | `none` for Instruct, `deepseek` for Thinking models |
| `MMPROJ` | *(empty)* | Path to multimodal projector GGUF (vision models only) |
| `CONTEXT_SIZE` | 8192 | Context window size (32768 for vision models) |
| `BATCH_SIZE` | 768 | **Critical for quality** - do not reduce |
| `UNBATCH_SIZE` | 768 | Should match BATCH_SIZE |
| `GPU_LAYERS` | 99 | Layers to offload to GPU |
| `PORT` | 8080 | Server listen port |
| `HOST` | 127.0.0.1 | Server bind address |
| `REASONING_MODE` | *(unset)* | Optional. `on`/`off`/`auto` → `--reasoning`. Set `off` for any model whose template defaults thinking ON, or `content` comes back empty. Needs llama.cpp **b8287+** (where `--reasoning` was added) |
| `PRESENCE_PENALTY` | *(unset)* | Optional. Passed only when set, so omitting it keeps llama.cpp's 0.0 default. Qwen 3.8 wants `1.5` in non-thinking mode |

### Critical Parameters (affect quality)

After testing **31+ configurations**, these settings achieve best performance:

- **Batch Size:** 768 (84.8% quality vs 18% at 256)
- **Context:** 8192 (headroom for conversations)
- **GPU Layers:** 99 (full offload)

**Do NOT reduce batch size** - quality drops dramatically.

---

## Prerequisites

- llama.cpp compiled and installed (`/usr/local/bin/llama-server`)
- Linux system with systemd
- CUDA drivers (for GPU acceleration)
- Root privileges for installation

---

## Installation

### Automatic Installation

```bash
chmod +x install.sh
sudo ./install.sh
```

The script will:
1. Create `llama` user and group
2. Set up directory structure
3. Install config, service, and logrotate files
4. Enable and start the service

### Manual Installation

1. Create the service user:
   ```bash
   sudo useradd --system --no-create-home --shell /usr/sbin/nologin llama
   sudo usermod -a -G video,render llama
   ```

2. Create directory structure:
   ```bash
   sudo mkdir -p /var/lib/llama-cpp/{models,templates,run}
   sudo mkdir -p /usr/local/etc/llama-cpp
   sudo mkdir -p /var/log/llama-cpp
   ```

3. Copy model and template files:
   ```bash
   sudo cp /path/to/model.gguf /var/lib/llama-cpp/models/
   sudo cp qwen3_chatml.jinja /var/lib/llama-cpp/templates/
   sudo cp qwen3_thinking.jinja /var/lib/llama-cpp/templates/  # Optional
   ```

4. Install config and service:
   ```bash
   sudo cp llama-server.conf /usr/local/etc/llama-cpp/
   sudo cp llama-server.service /etc/systemd/system/
   sudo cp llama-server /etc/logrotate.d/
   ```

5. Set permissions:
   ```bash
   sudo chown -R llama:llama /var/lib/llama-cpp /var/log/llama-cpp
   sudo chmod -R 755 /var/lib/llama-cpp /var/log/llama-cpp
   ```

6. Configure library path:
   ```bash
   sudo sh -c 'echo "/usr/local/lib" > /etc/ld.so.conf.d/llama-cpp.conf'
   sudo ldconfig
   ```

7. Enable and start:
   ```bash
   sudo systemctl daemon-reload
   sudo systemctl enable llama-server
   sudo systemctl start llama-server
   ```

---

## Service Management

```bash
# Status
sudo systemctl status llama-server

# Logs
sudo tail -f /var/log/llama-cpp/server.log
sudo tail -f /var/log/llama-cpp/error.log
sudo journalctl -u llama-server -f

# Restart (after config changes)
sudo systemctl restart llama-server

# Stop
sudo systemctl stop llama-server
```

---

## Testing

### Health Check
```bash
curl http://127.0.0.1:8080/health
```

### Quality Test
```bash
cd ../../llm_testing/scripts
python3 test_llm_quality.py
```

**Expected (Instruct):** 84-85% quality, ~15 tok/s

---

## Troubleshooting

### Out of Memory

```
cudaMalloc failed: out of memory
```

**Solution:** Reboot to clear GPU memory fragmentation (Jetson-specific):
```bash
sudo reboot
```

### Low Quality (<70%)

Verify critical settings:
```bash
grep -E "BATCH_SIZE|CONTEXT_SIZE|MODEL" /usr/local/etc/llama-cpp/llama-server.conf
```

Should show:
- `BATCH_SIZE=768`
- `CONTEXT_SIZE=8192`
- Model ending in `Q4_K_M.gguf`

### Thinking Not Working

1. Check reasoning format:
   ```bash
   grep REASONING_FORMAT /usr/local/etc/llama-cpp/llama-server.conf
   # Should be: REASONING_FORMAT=deepseek
   ```

2. Check model is Thinking variant:
   ```bash
   grep MODEL /usr/local/etc/llama-cpp/llama-server.conf
   # Should contain "Thinking"
   ```

3. Check DAWN config (`dawn.toml`):
   ```toml
   [llm.thinking]
   mode = "enabled"
   ```

### GPU Access Issues

```bash
# Add llama user to GPU groups
sudo usermod -a -G video,render llama

# Verify GPU access
sudo -u llama ls -la /dev/nvidia*

# Check for CUDA errors
sudo journalctl -u llama-server | grep -i cuda
```

### Server Won't Start

```bash
# Port in use?
netstat -tulpn | grep 8080

# Model exists?
ls -lh /var/lib/llama-cpp/models/*.gguf

# Detailed logs
sudo journalctl -u llama-server -n 100
```

---

## Files

| File | Installed Location | Purpose |
|------|-------------------|---------|
| `llama-server.conf` | `/usr/local/etc/llama-cpp/` | Server configuration |
| `llama-server.service` | `/etc/systemd/system/` | Systemd service unit |
| `llama-server` | `/etc/logrotate.d/` | Log rotation config |
| `qwen3_chatml.jinja` | `/var/lib/llama-cpp/templates/` | Instruct chat template |
| `qwen3_thinking.jinja` | `/var/lib/llama-cpp/templates/` | Thinking chat template |

---

## Performance Comparison

### AGX Orin 64GB at MAXN (60W) vs Claude Cloud

| Metric | Claude Opus 4.7 | Claude Sonnet 4.6 | Qwen3.6 35B-A3B (local) | Qwen3 4B (local) | Gemma 3 4B (local) |
|--------|-----------------|-------------------|-------------------------|------------------|---------------------|
| Quality | 99.1% | 96.5% | 94.0% (A) | 94.8% (A) | 89.7% (B) |
| Speed | cloud | cloud | ~37.1 tok/s | 36.71 tok/s | 37.93 tok/s |
| Vision | Yes | Yes | Yes | No | Yes |
| Offline | No | No | Yes | Yes | Yes |
| Privacy | Data sent | Data sent | Fully local | Fully local | Fully local |
| Cost | ~$0.05/query | ~$0.01/query | Free | Free | Free |
| Best for | Quality-first | Quality-first | Home/64GB | Voice-only | Helmet/16GB |

### Power Mode Impact (Qwen3-4B baseline)

| Power Mode | GPU Clock | EMC Clock | tok/s | Relative |
|------------|-----------|-----------|-------|----------|
| MAXN (60W) | 1.3 GHz | 3.2 GHz | 35.5 | 1.0x |
| 30W | 612 MHz | 2.1 GHz | 10.4 | 0.29x |

**Always use MAXN for inference workloads:**
```bash
sudo nvpmodel -m 0 && sudo jetson_clocks
```

See `../../docs/LLM_INTEGRATION_GUIDE.md` for complete integration details.
