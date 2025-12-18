# DAWN LLM Integration Guide

## Overview

DAWN supports both **local LLM** (via llama.cpp) and **cloud LLM** (OpenAI GPT-4o, Claude) for voice command processing. After testing 31+ configurations across 5 models, we achieved **81.9% quality (B grade)** with local inference - production-ready for voice assistants.

**Local LLM Winner:** Qwen3-4B-Instruct-2507-Q4_K_M @ batch 768
- **Quality:** 81.9% (86/105 points, B grade)
- **TTFT:** 116-138ms (excellent for streaming)
- **Streaming Latency:** ~1.3s perceived (ASR + TTFT + TTS start)

This guide shows you how to configure and use each option.

---

## Quick Start

### Option 1: Cloud LLM (GPT-4o) - Recommended

**Best for:** Highest accuracy (100%), fastest response (3.1s total latency)

1. Configure API key in `secrets.toml`:
```toml
openai_api_key = "sk-proj-..."
```

2. Set model in `dawn.toml` (optional, defaults to gpt-4o):
```toml
[llm.cloud]
openai_model = "gpt-4o"
```

3. Build and run DAWN - it will automatically use GPT-4o

**Performance:**
- Quality: 100% (A+ grade)
- Latency: ~3.1s total (1.2s LLM + 0.5s ASR + 0.2s TTS + 1.2s silence)
- Cost: ~$0.01-0.02 per interaction

---

### Option 2: Local LLM (Qwen3-4B Q4) - Privacy/Offline ✅ Recommended

**Best for:** Offline capability, privacy, no API costs, responsive streaming

1. Start llama-server with optimized settings:
```bash
sudo systemctl start llama-server
```

2. Configure DAWN to use local endpoint in `dawn.h`:
```c
#define OPENAI_API_ENDPOINT "http://127.0.0.1:8080/v1"
#define OPENAI_MODEL "qwen3-4b"  // Or leave as gpt-4o, llama.cpp ignores it
```

3. Build and run DAWN - it will connect to local llama-server

**Performance:**
- Quality: 81.9% (B grade)
- TTFT: 116-138ms (Time To First Token)
- Streaming Latency: ~1.3s perceived (ASR + TTFT + TTS start)
- Total Latency: ~4.9s (full response generation)
- Speed: 13.5 tok/s
- Cost: Free (runs locally)

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
- `/usr/local/bin/llama-server` - Binary
- `/usr/local/etc/llama-cpp/llama-server.conf` - Configuration (optimized settings)
- `/etc/systemd/system/llama-server.service` - Systemd unit
- `/etc/logrotate.d/llama-server` - Log rotation

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

---

## Optimal Settings (Tested)

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

We tested 5 models with optimal settings (batch 768, context 1024):

| Model | Quality | TTFT | Speed | Recommendation |
|-------|---------|------|-------|----------------|
| **Qwen3-4B Q4** | **81.9%** ✅ | 116-138ms | 13.5 tok/s | **Production** (balanced) |
| Qwen2.5-7B Q4 | **85.7%** ✅ | 181-218ms | 9.8 tok/s | Quality-focused option |
| Llama-3.2-3B | **71.4%** ✅ | 92-108ms | 18.0 tok/s | Speed-focused option |
| Phi-3-mini | 41.9% ❌ | ? | ? | Poor instruction following |
| Qwen3-4B Q6 | 36.2% ❌ | ? | ? | Template issues (`<think>` loops) |

**Three Viable Options:**
1. **Qwen3-4B Q4 (Recommended):** Best balance of quality + TTFT
2. **Qwen2.5-7B Q4:** Best quality (85.7%), only +80ms TTFT vs Qwen3-4B
3. **Llama-3.2-3B:** Fastest TTFT (92ms), acceptable quality (71.4%)

---

## Configuration Files

### `llm_testing/scripts/model_configs.conf`

Central configuration for all tested models. Use with test scripts:

```bash
cd llm_testing/scripts
./test_single_model.sh Qwen3-4B-Instruct-2507-Q4_K_M.gguf
```

Automatically loads optimal settings from `model_configs.conf`.

### `services/llama-server/llama-server.conf`

Production configuration for systemd service. Updated with optimal Qwen3-4B Q4 settings.

---

## Testing & Validation

### Quality Test

Run the quality test suite to verify command format compliance:

```bash
cd llm_testing/scripts
python3 test_llm_quality.py
```

**Expected Results:**
- GPT-4o: 100% (105/105 points)
- Claude 3.5 Sonnet: 92.4% (97/105 points)
- Qwen3-4B Q4 (local): 81.9% (86/105 points)

### Speed Test

Built into `test_single_model.sh`:

```bash
cd llm_testing/scripts
./test_single_model.sh Qwen3-4B-Instruct-2507-Q4_K_M.gguf
```

**Expected Results:**
- TTFT: 116-138ms (Time To First Token)
- Tokens/sec: ~13.5 tok/s
- 50-token response: ~3.7s total

---

## Cloud Baseline Results

Cloud LLMs establish peak performance:

### GPT-4o (OpenAI)
- **Quality:** 105/105 (100%) - Perfect command formatting
- **Speed:** 0.72-2.48s per test
- **All categories:** 100%
- **Cost:** ~$0.01-0.02 per interaction

### Claude Sonnet 3.7 (Anthropic)
- **Quality:** 97/105 (92.4%)
- **Speed:** 2.60-7.29s per test
- **One miss:** Conversational test (tried non-existent command)
- **Cost:** Similar to GPT-4o

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
- **Fix:** Check `openai_api_key` in `secrets.toml`

**Problem:** Rate limiting (429 Too Many Requests)
- **Fix:** Add retry logic or reduce request frequency

---

## Performance Comparison

| Metric | Cloud (GPT-4o) | Local (Qwen3-4B) |
|--------|----------------|------------------|
| **Quality** | 100% ✅ | 82.9% ✅ |
| **Speed** | 1.2s LLM | 3.4s LLM |
| **Total Latency** | 3.1s | 5.2s |
| **Offline?** | ❌ No | ✅ Yes |
| **Privacy** | ❌ Data sent to API | ✅ Fully local |
| **Cost** | ~$0.01-0.02 each | Free |
| **Reliability** | 99.9% uptime | Local control |

---

## Hybrid Mode (Future)

Future enhancement: Automatically choose based on context:

```c
if (is_simple_command(user_input)) {
    // Use local LLM (fast, offline)
    use_local_llm();
} else if (is_complex_query(user_input)) {
    // Use cloud LLM (highest accuracy)
    use_cloud_llm();
}
```

---

## Test Results Archive

Complete testing methodology and results:
- **Location:** `llm_testing/results/`
- **Final optimal:** 82.9% quality @ 14.9 tok/s
- **Config search:** 7 batch sizes tested, found batch 1024→768 breakthrough
- **Fine-tuning:** 19 parameters tested, only batch/context matter
- **Cloud baseline:** GPT-4o 100%, Claude 92%

---

## References

- **llama.cpp:** https://github.com/ggerganov/llama.cpp
- **Qwen3-4B Model:** https://huggingface.co/Qwen/Qwen3-4B-Instruct-GGUF
- **OpenAI API:** https://platform.openai.com/docs/api-reference
- **Test Scripts:** `llm_testing/scripts/`
- **Service Files:** `services/llama-server/`
