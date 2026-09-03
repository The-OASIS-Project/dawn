# LLM Testing Results

This directory contains results from extensive local LLM testing on NVIDIA Jetson Orin.

## Directory Structure

### `final_benchmark/`
**Nov 21, 2025 - results of the original campaign (batch 768, context 1024)**
*Historical: Orin 16GB, 105-point suite. Superseded — see Key Results Summary below.*

All 5 models tested with optimal fair settings:
- Qwen3-4B-Instruct-2507-Q4_K_M: 81.9% quality (B grade) — **winner of that campaign**
- Qwen2.5-7B-Instruct-Q4_K_M: 85.7% quality (B+ grade, but slower)
- Llama-3.2-3B-Instruct: 71.4% quality (C grade, fastest TTFT)
- Phi-3-mini-4k-instruct: 41.9% quality (F grade)
- Qwen_Qwen3-4B-Q6_K_L: 36.2% quality (F grade, template issues)

Contains:
- `BENCHMARK_REPORT.md` - Executive summary
- Individual quality/speed results for each model
- Server logs

### `cloud_baseline/`
**Cloud LLM baseline for comparison**

- GPT-4o: 100% quality (A+ grade)
- Claude 3.5 Sonnet: 92.4% quality (A grade)

Demonstrates local achieves 82% of cloud quality at zero cost.

### `archive/`
**Historical test data (not needed for reproduction)**

Archived intermediate results from the discovery process. Not required for understanding final results.

## Key Results Summary

**Current** — one sweep of all presets, 2026-09-03, AGX Orin 64GB MAXN,
llama.cpp b10626, 116-point FRIDAY suite:

| Model | Preset | Quality | Speed | Vision | Recommendation |
|-------|--------|---------|-------|--------|----------------|
| Gemma 4 31B | D | **97.4%** | 6.75 tok/s | Yes | Highest quality, WebUI only |
| Qwen3 4B Instruct | A | 94.8% | 36.71 tok/s | No | Voice-only |
| **Qwen3.6 35B-A3B (MoE)** | J | **94.0%** | **37.11 tok/s** | Yes | ✅ **Production** |
| Qwen3.8 27B | K | 94.0% | 8.28 tok/s | Yes | Ties J, 4.5x slower |
| Qwen3-Coder-Next | H | 94.0% | 34.37 tok/s | No | Coding |
| Gemma 4 26B-A4B | G | 93.1% | 31.48 tok/s | Yes | Vision/tools unverified |
| Qwen3.5 27B | E | 93.1% | 7.55 tok/s | Yes | WebUI tier |
| Qwen3.6 27B | I | 92.2% | 7.47 tok/s | Yes | WebUI tier |
| Qwen3.5 35B-A3B | F | 91.4% | 34.97 tok/s | Yes | Stable alt |
| Gemma 3 4B IT | A3 | 89.7% | 37.93 tok/s | Yes | 16GB Orin |
| Qwen3.5 4B | A2 | 89.7% | 31.19 tok/s | Yes | Small hardware |
| Gemma 3 12B IT | C | 89.7% | 16.51 tok/s | Yes | WebUI tier |

The live table (TTFT, context scaling, all presets) lives in
`../../services/llama-server/README.md`.

**Historical** — Orin 16GB, 105-point suite. Different hardware *and* a
different suite, so not comparable with the table above:

| Model | Quality | TTFT | Speed | Recommendation |
|-------|---------|------|-------|----------------|
| **Qwen3-4B Q4** | **81.9%** | 116-138ms | 13.5 tok/s | Production *(at the time)* |
| Qwen2.5-7B Q4 | 85.7% | 181-218ms | 9.8 tok/s | Quality-focused |
| Llama-3.2-3B | 71.4% | 92-108ms | 18.0 tok/s | Speed-focused |

## How to Reproduce

1. Install llama.cpp and download models
2. Use configs from `../scripts/model_configs.conf`
3. Run: `../scripts/benchmark_all_models.sh`
4. Quality test: `../scripts/test_llm_quality.py`

See `../docs/MODEL_TEST_ANALYSIS.md` for complete analysis.

## Critical Findings

1. **Batch size is THE parameter** - 18% → 81.9% improvement from batch 256 → 768
   (still true; batch 768 remains the basis of every preset)
2. **Context must be ≥1024** - Quality drops significantly at 512
3. **Sampling parameters have ZERO effect** - Temperature, top-k, top-p don't matter
4. **TTFT matters most** - For streaming, first token time determines perceived latency
5. **Local LLM is viable** - was 81.9% on the 2025 campaign; now 94.0% with
   Qwen3.6 35B-A3B and 97.4% with Gemma 4 31B, the latter above Claude
   Haiku/Sonnet (96.5%) on this suite

## Test Date

November 21-22, 2025

## Hardware

NVIDIA Jetson Orin
- 1024-core Ampere GPU
- 16GB unified memory
- CUDA 11.8
