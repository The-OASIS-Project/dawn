# DAWN Local LLM Model Test Analysis - FINAL RESULTS

> **⚠️ HISTORICAL RECORD — SUPERSEDED.** This document analyses the original
> 31-configuration campaign run on a Jetson Orin **16GB** against a **105-point**
> FRIDAY suite. Its numbers are preserved as written and are *not* comparable
> with current figures, which use a **116-point** suite on an AGX Orin **64GB**:
> a percentage difference between the two eras can be the suite changing rather
> than the model.
>
> **For current numbers see
> [`services/llama-server/README.md`](../../services/llama-server/README.md).**
> As of the 2026-09-03 full-fleet sweep the production model is Qwen3.6 35B-A3B
> (MoE, Preset J) at 94.0% and ~37.1 tok/s on llama.cpp b10626 — not the
> Qwen3-4B below. Highest local score is Gemma 4 31B at 97.4%.
>
> What *does* still hold from this campaign: **batch size 768 is the critical
> quality parameter**, and sampling parameters (temperature, top-k, top-p,
> repeat penalty) have negligible effect. Both remain the basis of every preset.

## Executive Summary

After extensive testing (31+ configurations across 5 models), we achieved **81.9% quality (B grade)** with local LLM inference on Jetson Orin. The breakthrough came from discovering that **batch size is THE critical parameter** for quality.

**Winner:** Qwen3-4B-Instruct-2507-Q4_K_M @ batch 768, context 1024
- **Quality:** 81.9% (86/105 points, B grade)
- **TTFT:** 116-138ms (excellent for streaming)
- **Speed:** 13.5 tok/s
- **Streaming Latency:** ~1.3s perceived (ASR + TTFT + TTS start)

**Key Discovery:** Temperature, top-k, top-p, repeat_penalty have **ZERO effect** on quality (extensively tested). Only batch size and context matter.

---

## Final Model Rankings (Batch 768, Context 1024)

### Production-Ready Models (C Grade or Better)

| Rank | Model | Quality | TTFT | Speed | Streaming Latency | Recommendation |
|------|-------|---------|------|-------|-------------------|----------------|
| 🥇 1 | **Qwen3-4B Q4** | **81.9%** (B) | **116-138ms** | 13.5 tok/s | **~1.3s** | ✅ **PRODUCTION** |
| 🥈 2 | **Qwen2.5-7B Q4** | **85.7%** (B+) | 181-218ms | 9.8 tok/s | ~1.4s | ✅ Best quality |
| 🥉 3 | **Llama-3.2-3B** | **71.4%** (C) | **92-108ms** | 18.0 tok/s | **~1.2s** | ✅ Fastest |

### Non-Viable Models

| Rank | Model | Quality | Issue |
|------|-------|---------|-------|
| 4 | Phi-3-mini | 41.9% (F) | Poor instruction following |
| 5 | Qwen3-4B Q6 | 36.2% (F) | `<think>` loops (template issue) |

### Cloud Baseline (Reference)

| Model | Quality | Latency | Cost |
|-------|---------|---------|------|
| GPT-4o | 100% (A+) | 3.1s total | ~$0.01/query |
| Claude 3.5 Sonnet | 92.4% (A) | ~3.5s total | ~$0.01/query |

---

## Why Qwen3-4B Q4 is the Winner

### Decision Criteria

**Speed vs Quality Trade-off:**
- Qwen2.5-7B: 4% better quality, but slower TTFT (181ms vs 138ms)
- Llama-3.2-3B: 10% faster TTFT, but 10% lower quality
- **Qwen3-4B Q4: Best balance** - good quality + fast TTFT

### TTFT Analysis (Critical for Streaming)

With DAWN's streaming architecture (LLM → TTS pipeline), **Time To First Token (TTFT) determines perceived latency**, not total generation time.

**Perceived Latency Breakdown:**
```
User speaks → ASR (0.5s) → VAD (0.5s) → TTFT (0.1-0.2s) → TTS starts → User hears response
Total perceived: ~1.1-1.4s
```

**All three top models have excellent TTFT (<220ms):**
- Llama-3.2-3B: 92-108ms (best)
- Qwen3-4B Q4: 116-138ms (excellent)
- Qwen2.5-7B: 181-218ms (good)

**The tok/s speed matters less** - it only affects how long TTS continues generating in background, not when user first hears response.

### Quality Breakdown (Qwen3-4B Q4)

**Category Scores:**
- Boolean commands: **100%** ✅ (20/20)
- Getter commands: **90%** ✅ (18/20)
- Analog commands: **120%** ✅ (12/10, bonus points)
- Music playback: **90%** (9/10)
- Vision AI: **90%** (9/10)
- Multiple commands: **67%** (10/15)
- Clarification: **70%** (7/10)
- Conversational: **10%** (1/10)

**Strengths:**
- Perfect command execution (boolean/getter/analog)
- Excellent JSON formatting
- Maintains FRIDAY persona
- Respects word limits

**Weaknesses:**
- Struggles with conversational questions (10%)
- Multiple command formatting needs improvement (67%)

---

## The Discovery Journey - How We Got Here

### Phase 1: Initial Survey (Batch 256) - All Models Failed

**Nov 21, 18:00 - First tests with batch 256:**

| Model | Quality | Speed | Issue |
|-------|---------|-------|-------|
| Qwen3-4B Q4 | 37.1% | 15.0 tok/s | Poor JSON, missing commands |
| Llama-3.2-3B | 25.7% | 19.7 tok/s | **Gibberish** (3333...) |
| Phi-3-mini | 25.7% | 18.5 tok/s | Blank responses |
| Qwen2.5-7B Q4 | 22.9% | 10.6 tok/s | No JSON commands |
| Qwen3-4B Q6 | 15.2% | 11.5 tok/s | `<think>` loops |

**Conclusion:** Something fundamentally wrong - all models failing badly.

### Phase 2: Batch Size Discovery - BREAKTHROUGH! 🚀

**Qwen3-4B Q4 tested with varying batch sizes:**

| Batch | Quality | Change | Observation |
|-------|---------|--------|-------------|
| 128 | ~18% | baseline | Very poor |
| 256 | 37.1% | +19.1% | Still poor |
| 512 | 56% | +18.9% | Major improvement! |
| **768** | **81.9%** | **+25.9%** | 🎯 **BREAKTHROUGH** |
| 1024 | 81.9% | +0% | Same (memory limit) |

**Critical Finding:** Batch size is THE parameter that matters for quality!
- **18% → 81.9% improvement** just from batch size!
- Context must be ≥1024 for good quality

### Phase 3: Fine-Tuning (19 Parameters) - NO EFFECT!

**Systematically tested at batch 768, context 1024:**

**Sampling Parameters (ALL = 81.9% quality):**
- Temperature: 0.5, 0.7, 0.9, 1.0 → **All 81.9%**
- Top-K: 20, 40, 60, 100 → **All 81.9%**
- Top-P: 0.8, 0.85, 0.9, 0.95 → **All 81.9%**
- Repeat Penalty: 1.0, 1.1, 1.2, 1.3 → **All 81.9%**
- Min-P: 0, 0.05, 0.1 → **All 81.9%**

**Other Parameters (NO EFFECT on quality):**
- Threads: 2, 4, 6, 8 → Only affects speed
- Cache type: f16, q8_0 → No quality change
- GPU layers: 99 (all on GPU)

**Conclusion:** After batch/context are optimal, **nothing else matters for quality!**

### Phase 4: Retest All Models with Batch 768 - Game Changer!

**Nov 21, 23:00 - Fair comparison with batch 768:**

| Model | Batch 256 | Batch 768 | Improvement | Status |
|-------|-----------|-----------|-------------|--------|
| **Qwen3-4B Q4** | 37.1% | **81.9%** | **+44.8%** | ✅ Winner |
| **Qwen2.5-7B** | 22.9% | **85.7%** | **+62.8%** | ✅ Quality king |
| **Llama-3.2-3B** | 25.7% (gibberish) | **71.4%** | **+45.7%** | ✅ Now viable! |
| Phi-3-mini | 25.7% | 41.9% | +16.2% | ❌ Still poor |
| Qwen3-4B Q6 | 15.2% | 36.2% | +21.0% | ❌ Still broken |

**Impact:** Batch size scaling works for Qwen and Llama models, not for Phi-3.

---

## Critical Insights

### 1. Only Two Parameters Matter for Quality

**For Quality:**
- ✅ **Batch size: MUST be ≥768** (18% → 81.9% improvement!)
- ✅ **Context size: MUST be ≥1024** (56% at 512 → 81.9% at 1024)

**Have ZERO Effect on Quality:**
- ❌ Temperature (tested: 0.5, 0.7, 0.9, 1.0)
- ❌ Top-K (tested: 20, 40, 60, 100)
- ❌ Top-P (tested: 0.8, 0.85, 0.9, 0.95)
- ❌ Repeat Penalty (tested: 1.0-1.3)
- ❌ Min-P (tested: 0, 0.05, 0.1)
- ❌ Threads (affects speed only)
- ❌ Cache type (affects memory only)

### 2. TTFT vs Tokens/Sec for Streaming

**Traditional Metric (Misleading for Streaming):**
- Tokens/sec: How fast model generates complete response
- Relevant for: Batch processing, full-response generation
- **Not relevant for:** Streaming architectures like DAWN

**Correct Metric (TTFT):**
- Time To First Token: How long until first token appears
- Relevant for: Streaming TTS, perceived latency
- **Critical for:** User experience in voice assistants

**Why This Matters:**
```
Non-streaming: User waits for ALL tokens before hearing response
Latency = ASR + (all tokens / tok/s) + TTS

Streaming: User hears response as soon as first token arrives
Latency = ASR + TTFT + TTS_start (~0.1s)
```

**Example with Qwen2.5-7B:**
- Tokens/sec: 9.8 tok/s (seems slow)
- TTFT: 181-218ms (actually excellent!)
- **Perceived latency: ~1.4s** (very fast!)

### 3. Q4 vs Q6 Quantization

- **Q4:** 81.9% quality @ 13.5 tok/s, TTFT 138ms
- **Q6:** 36.2% quality @ ? tok/s (still broken)
- **Conclusion:** Q4 is superior for Qwen3-4B (Q6 has template issues)

### 4. Model Size vs Performance

- **3B models:** Fast but lower quality (Llama: 71.4%, 18 tok/s)
- **4B models:** Best balance (Qwen3-4B: 81.9%, 13.5 tok/s) ⭐
- **7B models:** Best quality but slower (Qwen2.5-7B: 85.7%, 9.8 tok/s)

### 5. Batch Size is Model-Specific

Not all models benefit equally from batch 768:
- **Qwen models:** Massive improvement (+45-63%)
- **Llama 3.2:** Massive improvement (+45.7%, fixes gibberish!)
- **Phi-3:** Moderate improvement (+16.2%, still broken)

---

## Performance Comparison

### Local vs Cloud

| Metric | Qwen3-4B Q4 (Local) | GPT-4o (Cloud) | Delta |
|--------|---------------------|----------------|-------|
| Quality | 81.9% (B) | 100% (A+) | -18.1% |
| TTFT | 116-138ms | ~200-300ms | Better! |
| Streaming Latency | ~1.3s | ~3.1s | **Better!** |
| Total Latency | ~4.9s | ~3.1s | +1.8s |
| Cost | $0 | ~$0.01/query | - |
| Privacy | Full | None | - |
| Offline | ✅ Yes | ❌ No | - |
| Reliability | ✅ High | ⚠️ Network-dependent | - |

### Local Model Comparison

| Model | Quality | TTFT | Speed | Use Case |
|-------|---------|------|-------|----------|
| **Qwen3-4B Q4** | 81.9% | 138ms | 13.5 tok/s | **Production** (balanced) |
| Qwen2.5-7B Q4 | 85.7% | 218ms | 9.8 tok/s | Quality-focused |
| Llama-3.2-3B | 71.4% | 108ms | 18.0 tok/s | Speed-focused |

---

## Test Methodology

### Quality Rubric (105 points total)

**Command Categories:**
- Boolean commands (ON/OFF): 20 points
- Getter commands (get status): 20 points
- Analog commands (set level): 10 points
- Music playback: 10 points
- Vision AI: 10 points
- Multiple commands: 15 points

**Response Quality:**
- Clarification requests: 10 points
- Conversational responses: 10 points

**Grading Scale:**
- A+ (95-100%): Excellent
- A (90-94%): Very Good
- B (80-89%): Good
- C (70-79%): Acceptable
- D (60-69%): Poor
- F (<60%): Fail

### Speed Measurement

- **TTFT:** Time to first token (critical for streaming)
- **Tokens/sec:** Generation throughput (background metric)
- **Streaming Latency:** ASR + TTFT + TTS_start
- **Total Latency:** ASR + all tokens + TTS

### Hardware Platform

**NVIDIA Jetson Orin (embedded):**
- GPU: 1024-core Ampere
- RAM: 16GB unified memory
- CUDA: 11.8

**Jetson-Specific Issues:**
- Memory fragmentation prevents large batch allocations
- Need to reboot for clean memory state
- CUDA allocator less robust than desktop GPUs
- Batch 1024 fails even with 11GB free (hence batch 768)

---

## Configuration Summary

### Tested Configurations: 31+

**Initial Survey:** 5 models × various configs = 19 tests
**Batch Size Search:** 7 different batch sizes
**Fine-Tuning:** 19 parameters tested (temp, top-k, top-p, etc.)

**Total:** 31+ distinct test runs with full quality validation

### Winner Configuration

**Model:** Qwen3-4B-Instruct-2507-Q4_K_M.gguf
```bash
GPU_LAYERS: 99
CONTEXT: 1024
BATCH: 768
UBATCH: 768
THREADS: 4
TEMP: 0.7
TOP_P: 0.9
TOP_K: 40
REPEAT_PENALTY: 1.1
EXTRA_FLAGS: --flash-attn
```

**Template:** ChatML format (standard for Qwen models)
**Location:** `/var/lib/llama-cpp/templates/qwen3_chatml.jinja`

---

## Production Recommendations

### Primary Recommendation: Qwen3-4B Q4

**Use When:**
- Balanced speed/quality needed
- TTFT ~138ms acceptable
- 81.9% quality sufficient
- Running on Jetson Orin or similar

**Deployment:**
```bash
cd services/llama-server
sudo ./install.sh
# Uses optimal settings by default
```

### Alternative 1: Qwen2.5-7B Q4 (Quality-Focused)

**Use When:**
- Best quality needed (85.7%)
- Can tolerate +80ms TTFT vs Qwen3-4B
- Have memory for 7B model

**Trade-off:** 4% better quality, 60% slower generation (but TTFT only +80ms)

### Alternative 2: Llama-3.2-3B (Speed-Focused)

**Use When:**
- Fastest TTFT needed (92-108ms)
- 71.4% quality acceptable
- Want snappiest responses

**Trade-off:** 10% lower quality, but 25% faster TTFT

### Cloud Option: GPT-4o (Premium)

**Use When:**
- 100% quality required
- Cost acceptable (~$0.01/query)
- Network reliability guaranteed
- Privacy not a concern

---

## Detailed Results Locations

### Final Benchmark Results
- `llm_testing/results/final_benchmark/` - Nov 21 batch 768 tests
  - All 5 models tested with fair settings
  - Quality + speed results
  - Individual test outputs

### Historical Results (Archive)
- `llm_testing/results/archive/preliminary/` - Batch 256 tests
- `llm_testing/results/archive/batch_search/` - Batch size discovery
- `llm_testing/results/archive/fine_tuning/` - 19-parameter tests

### Cloud Baseline
- `llm_testing/results/cloud_baseline/` - GPT-4o & Claude results

### Scripts
- `llm_testing/scripts/test_single_model.sh` - Single model testing
- `llm_testing/scripts/benchmark_all_models.sh` - Batch testing
- `llm_testing/scripts/test_llm_quality.py` - Quality validator
- `llm_testing/scripts/model_configs.conf` - Model-specific configs

---

## Key Takeaways

1. ⭐ **Batch size is everything** - 18% → 81.9% improvement
2. ⭐ **Context must be ≥1024** - Quality drops at 512
3. ⭐ **Sampling parameters don't matter** - Temp/top-k/top-p have ZERO effect
4. ⭐ **TTFT > tokens/sec** - For streaming, first token time matters most
5. ⭐ **Local LLM is viable** - 81.9% is good enough for production
6. ⭐ **Three viable options** - Qwen3-4B (balanced), Qwen2.5-7B (quality), Llama-3.2 (speed)
7. ⭐ **Batch scaling is model-specific** - Works great for Qwen/Llama, not Phi-3
8. ⭐ **Q4 > Q6** - For Qwen3-4B, Q4 performs better (Q6 has template issues)

---

## Future Work

### Potential Improvements

1. **Fix Qwen3-4B Q6 template issue** - Could be better than Q4 if template works
2. **Test larger context (2048)** - May improve multi-turn conversations
3. **Fine-tune for DAWN** - Custom training on DAWN command dataset
4. **Hybrid architecture** - Local for common commands, cloud for complex queries
5. **Investigate Phi-3 issues** - Why doesn't batch scaling help?

### Open Questions

1. Why do only batch/context affect quality? (LLM inference theory question)
2. Can we get Q6 working properly? (Template investigation needed)
3. Would context 2048 improve conversational scores? (Currently 10%)
4. Is there a sweet spot between batch 512-768? (Fine-grained search)

---

## Conclusion

After 31+ configurations tested across 5 models, we achieved **81.9% quality (B grade)** with local inference on Jetson Orin. The breakthrough was discovering that **batch size ≥768 and context ≥1024 are the only parameters that matter** for quality.

**Winner:** Qwen3-4B-Instruct-2507-Q4_K_M @ batch 768, context 1024
- **Quality:** 81.9% (B grade, production-ready)
- **TTFT:** 116-138ms (excellent for streaming)
- **Streaming Latency:** ~1.3s (fast, responsive)
- **Status:** Ready for production deployment

**Alternative options available** for quality-focused (Qwen2.5-7B @ 85.7%) or speed-focused (Llama-3.2-3B @ 71.4%) use cases.

**Cloud comparison:** GPT-4o achieves 100% quality but costs $0.01/query and requires network. Local option provides 82% of cloud quality at zero cost with full privacy and offline capability.

**Recommendation:** Deploy Qwen3-4B Q4 for production. Consider Qwen2.5-7B for quality-critical applications or Llama-3.2-3B for speed-critical applications.

---

## Update: December 2024 - Qwen3 Model Retesting & Speculative Decoding Analysis

### Executive Summary (Dec 2024)

After extensive retesting with optimized configurations and Qwen's official recommended parameters, we achieved **84.8% quality (B grade)** - a 3% improvement over previous tests. More importantly, we conducted a thorough investigation into **speculative decoding** and found it is **NOT beneficial for DAWN's use case**.

**Key Findings:**
- Qwen3-4B-Instruct-2507-Q4_K_M achieves **84.8% quality** with Qwen's official params
- Speculative decoding is **40% slower** for DAWN's prompt structure
- Unsloth Dynamic (UD) quantizations offer no quality improvement
- Prompt caching provides no benefit for varied user commands

---

### Updated Model Rankings (December 2024)

| Model | Quality | Speed | Grade | Status |
|-------|---------|-------|-------|--------|
| **Qwen3-4B-Instruct-2507-Q4_K_M** | **84.8%** | 15.2 tok/s | **B** | ✅ **PRODUCTION** |
| Qwen3-4B-Instruct-2507-UD-Q4_K_XL | 84.1% | 14.8 tok/s | B | ✅ Alternative |
| Qwen3-4B-Instruct-2507-UD-Q5_K_XL | 83.4% | 13.9 tok/s | B | ✅ Alternative |
| Qwen3-4B-Instruct-2507-UD-Q3_K_XL | 81.2% | 15.5 tok/s | B | ⚠️ Lower quality |
| Qwen3-4B-Instruct-2507-Q8_0 | OOM | - | - | ❌ Too large |

**Best Configuration (Qwen's Official Params):**
```bash
TEMP: 0.7
TOP_P: 0.8
TOP_K: 20
MIN_P: 0
BATCH: 768
CONTEXT: 1024
EXTRA_FLAGS: --flash-attn on --min-p 0
```

---

### Speculative Decoding Deep Dive

#### What is Speculative Decoding?

Speculative decoding uses a small "draft" model to predict multiple tokens, which are then verified in parallel by the main model. In theory, this can provide 2-4x speedup when:
- Draft model predicts well (high acceptance rate)
- Response length is long relative to prompt
- Both models share the same tokenizer

#### Our Test Setup

- **Main Model:** Qwen3-4B-Instruct-2507-Q4_K_M (2.3 GB)
- **Draft Model:** Qwen3-0.6B-Q8_0 (604 MB)
- **Draft Acceptance Rate:** 98.4% (excellent - same tokenizer family)
- **Configuration:**
  ```bash
  -md /var/lib/llama-cpp/models/Qwen3-0.6B-Q8_0.gguf
  -ngld 99        # All draft layers on GPU
  -cd 1024        # Draft context size (CRITICAL!)
  --draft-max 8   # Max draft tokens per iteration
  --draft-min 0   # Min draft tokens
  ```

#### Critical Discovery: Draft Context Size (`-cd`)

**OOM Issue:** Initial attempts failed with OOM even with 11GB free. The cause:
- Draft model defaults to **4096 context** if `-cd` not specified
- This allocates ~448 MiB for draft KV cache alone
- Combined with main model, exceeded available memory

**Fix:** Explicitly set `-cd 1024` to match main model context.

#### Performance Results

**Simple Prompts (no system prompt):**

| Configuration | Prompt Tokens | Completion | Speed | Speedup |
|--------------|---------------|------------|-------|---------|
| NO spec decoding | 21 | 81 | 15.2 tok/s | baseline |
| WITH spec decoding (cold) | 21 | 81 | 27.2 tok/s | **1.8x** |
| WITH spec decoding (cached) | 21 | 81 | 85.0 tok/s | **5.6x** |

**DAWN System Prompt (90 tokens):**

| Configuration | Command | Completion | Speed |
|--------------|---------|------------|-------|
| **NO spec** | Turn on lights | 33 | **14.6 tok/s** |
| **NO spec** | Set volume 50 | 25 | **15.4 tok/s** |
| **NO spec** | What time is it | 15 | **15.0 tok/s** |
| **NO spec** | Play jazz | 39 | **15.6 tok/s** |
| **NO spec** | Turn off detection | 39 | **15.5 tok/s** |
| **NO spec Average** | | | **15.2 tok/s** |
| | | | |
| **WITH spec** | Turn on lights | 33 | 10.7 tok/s |
| **WITH spec** | Set volume 50 | 28 | 8.0 tok/s |
| **WITH spec** | What time is it | 37 | 9.3 tok/s |
| **WITH spec** | Play jazz | 39 | 9.7 tok/s |
| **WITH spec** | Turn off detection | 32 | 9.5 tok/s |
| **WITH spec Average** | | | **9.4 tok/s** |

#### Why Speculative Decoding Hurts DAWN Performance

**The Math:**

1. **DAWN's Use Case:**
   - Long system prompt: ~90 tokens
   - Short user command: ~5-10 tokens
   - Short response: ~30-50 tokens

2. **Speculative Decoding Overhead:**
   - Both models must process the full prompt
   - Draft tokens generated, then verified by main model
   - Verification overhead per batch of draft tokens

3. **Break-Even Analysis:**
   - Spec decoding wins when: `(response_tokens * speedup) > prompt_processing_overhead`
   - For DAWN: `(35 tokens * 1.8x) < (90 token prompt * 2 models)`
   - **Result:** Overhead exceeds benefit

**Prompt Caching Does NOT Help:**
- Caching requires **exact prefix match**
- Different user commands = different prompts
- Only system prompt prefix is cacheable
- User message suffix changes, breaking cache

#### Conclusion: Skip Speculative Decoding for DAWN

| Factor | Finding |
|--------|---------|
| Simple prompts | ✅ 1.8-5.6x speedup |
| DAWN prompts | ❌ **40% slower** |
| Memory overhead | +600 MB for draft model |
| Quality impact | None (84.8% maintained) |
| **Recommendation** | **Don't use for DAWN** |

**When Speculative Decoding WOULD Help:**
- Long response generation (100+ tokens)
- Short prompts (<30 tokens)
- Multi-turn conversations with caching
- Batch processing of similar queries

---

### Unsloth Dynamic (UD) Quantization Analysis

We tested Unsloth's "Dynamic 2.0" quantizations which claim better quality preservation at smaller sizes.

#### Results

| Model | Size | Quality | Speed | Notes |
|-------|------|---------|-------|-------|
| Standard Q4_K_M | 2.32 GB | **84.8%** | 15.2 tok/s | ✅ Best |
| UD-Q4_K_XL | 2.41 GB | 84.1% | 14.8 tok/s | Slightly larger |
| UD-Q5_K_XL | 2.87 GB | 83.4% | 13.9 tok/s | No improvement |
| UD-Q3_K_XL | 1.89 GB | 81.2% | 15.5 tok/s | Fastest, lower quality |

#### Key Findings

1. **UD quantizations offer no quality improvement** over standard Q4_K_M
2. **Standard Q4_K_M is optimal** - best quality/size/speed balance
3. **UD-Q3_K_XL is viable** if memory-constrained (only -3.6% quality)
4. **262K context** on UD models requires smaller batch (512 vs 768) to avoid OOM

---

### Updated Quality Test Results (Qwen3-4B-Instruct-2507-Q4_K_M)

**Total Score: 123/145 (84.8%) - Grade: B**

| Category | Score | Percentage | Notes |
|----------|-------|------------|-------|
| boolean | 20/20 | 100.0% | ✅ Perfect |
| analog | 12/10 | 120.0% | ✅ Bonus points |
| getter | 18/20 | 90.0% | ✅ Excellent |
| vision | 9/10 | 90.0% | ✅ Excellent |
| music | 11/10 | 110.0% | ✅ Bonus points |
| search | 22/20 | 110.0% | ✅ Bonus points |
| weather | 11/10 | 110.0% | ✅ Bonus points |
| multiple | 10/15 | 66.7% | ⚠️ Needs work |
| clarification | 7/10 | 70.0% | ⚠️ Needs work |
| weather_clarify | 1/10 | 10.0% | ❌ Poor |
| conversational | 2/10 | 20.0% | ❌ Poor |

**Strengths:**
- Perfect boolean command execution
- Excellent JSON formatting with correct device/action
- Bonus points on multiple categories (conciseness rewarded)
- Web search queries well-formed

**Weaknesses:**
- Weather without location should ask for clarification (doesn't)
- Conversational questions trigger unnecessary commands
- Multiple commands formatting inconsistent

---

### Test Script Enhancements

The `test_single_model.sh` script was enhanced with speculative decoding support:

```bash
# Usage examples:
./test_single_model.sh Qwen3-4B-Instruct-2507-Q4_K_M.gguf           # Standard test
./test_single_model.sh --spec Qwen3-4B-Instruct-2507-Q4_K_M.gguf    # With spec decoding
./test_single_model.sh --draft Qwen3-0.6B-Q8_0.gguf Qwen3-4B-Instruct-2507-Q4_K_M.gguf  # Custom draft

# Options:
#   --spec, -s              Enable speculative decoding with default draft model
#   --draft <model.gguf>    Enable speculative decoding with specific draft model
#   -h, --help              Show help message
```

**Default Draft Model:** Qwen3-0.6B-Q8_0.gguf (same tokenizer family as Qwen3-4B)

---

### Updated Recommendations (December 2024)

#### Production Configuration

**Model:** Qwen3-4B-Instruct-2507-Q4_K_M.gguf

```bash
GPU_LAYERS: 99
CONTEXT: 1024
BATCH: 768
UBATCH: 768
THREADS: 4
TEMP: 0.7
TOP_P: 0.8
TOP_K: 20
REPEAT_PENALTY: 1.1
EXTRA_FLAGS: --flash-attn on --min-p 0
```

**DO NOT USE:**
- Speculative decoding (40% slower for DAWN)
- UD quantizations (no benefit over standard Q4)
- Q8_0 quantization (OOM on Jetson)

#### Memory Budget on Jetson Orin (16GB unified)

| Component | Memory | Notes |
|-----------|--------|-------|
| Qwen3-4B Q4 model | ~2.4 GB | GPU layers |
| KV Cache (ctx 1024) | ~144 MB | Scales with context |
| Compute buffers | ~302 MB | Flash attention |
| System/OS | ~2 GB | Linux + CUDA runtime |
| **Total Used** | **~5 GB** | |
| **Available for other tasks** | **~11 GB** | |

---

### Key Takeaways (Updated)

1. ⭐ **Qwen3-4B Q4_K_M remains the best choice** - 84.8% quality, 15.2 tok/s
2. ⭐ **Qwen's official params (temp=0.7, top_p=0.8, top_k=20)** provide best results
3. ⭐ **Speculative decoding is counterproductive** for DAWN's prompt structure
4. ⭐ **Unsloth Dynamic quantizations offer no advantage** over standard Q4
5. ⭐ **Draft context size (-cd) is critical** to avoid OOM with speculative decoding
6. ⭐ **Prompt caching only helps with exact matches** - not useful for varied commands
7. ⭐ **Short responses + long prompts = worst case** for speculative decoding
8. ⭐ **Quality improved 3%** (81.9% → 84.8%) with Qwen's official parameters
