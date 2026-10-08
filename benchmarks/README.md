# Retrieval Benchmarks

Measures DAWN's memory retrieval quality against three published benchmarks.

## ⚠ Read this before comparing numbers to published systems

This harness produces several metrics. **They measure different things, and
only one is directly comparable to ByteRover / MemMachine / Hindsight / Mem0
published headlines.** Confusing them produces apples-to-oranges results.

| Metric | What it measures | Comparable to leaders? |
|---|---|---|
| `recall_reach` | Top-K provenance overlap with gold dialog IDs | **NO** — internal diagnostic only |
| `recall_entailment` | Strict YES/NO: do retrieved facts contain enough info to derive gold? | **NO** — internal diagnostic |
| `recall_generation` | Generate-and-judge: LLM produces answer from retrieved memory, judge scores vs gold | **YES, if Mem0 protocol is matched** |

**Published systems (ByteRover, MemMachine, Hindsight, Mem0, Memobase) all
report `recall_generation`-style LLM-judge accuracy.** They do NOT report
retrieval-reach or entailment. If you quote one of our `recall_reach` numbers
alongside a published 90%+ headline, you're misrepresenting both.

### How to know the run is leader-comparable

The bench prints a `LEADER-COMPARABLE: YES|NO` banner at the end, and tags
`leader_comparable: true|false` in the results JSON. Honor it. The classifier
lives at `classify_leader_comparable()` in `run_benchmark.py`.

**Mem0 protocol** (all required):

| Flag | Value | Why |
|---|---|---|
| `--memory-pipeline` | required | Production retrieval (memory_facts + entity graph), not raw chunks |
| `--generator-provider` | `openai` | Mem0/MemMachine publish using OpenAI |
| `--generator-model` | `gpt-4o-mini` (or `gpt-4o`) | Mem0 default + MemMachine convention |
| `--judge-provider` | `openai` | Same family — avoids cross-family bias |
| `--judge-model` | `gpt-4o-mini` (or `gpt-4o`) | Same as generator (Mem0 protocol) |
| `--prompt-style` | `mem0` | "Be generous, topic match counts" rubric — strict rubric undercuts ~10-15pp |
| `--with-source` | required | Production memory_callback context; bare-facts mode underweights generator ~6pp |
| `--exclude-categories` | `5` | Mem0 convention excludes cat-5 adversarial |

### Canonical leader-comparable run

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark locomo \
    --dataset ~/datasets/locomo/data/locomo10.json \
    --memory-pipeline \
    --config ./dawn.toml \
    --cache-dir ./benchmarks/snapshots_phase2_base \
    --generator-provider openai --generator-model gpt-4o-mini \
    --judge-provider openai --judge-model gpt-4o-mini \
    --prompt-style mem0 \
    --exclude-categories 5 \
    --with-source \
    --output /tmp/bench_leader.json
```

Expect ~$2 spend, ~50 min wall clock. Cache makes re-runs free.

### Comparable to whom?

| Comparison target | Use |
|---|---|
| ByteRover 2.0 (92.2%), MemMachine v0.2 (91.2%) | `recall_generation`, Mem0 protocol above |
| Hindsight / Backboard (89-90%) | Same Mem0 protocol; their judge is GPT-OSS-120B but published comparison runs use Mem0-equivalent prompts |
| MemReranker MAP (0.74) | `recall_reach` is the closest analogue but not identical (reach is binary hit-in-top-K, MAP is rank-weighted). Run with `--memory-pipeline` and report alongside MAP, not as MAP. |
| Mem0 paper "J score" | `recall_generation` under Mem0 protocol = same metric, same dataset, same prompts |

### Internal diagnostic metric (`recall_reach`)

Useful for **regression testing** across DAWN-internal changes — does a code
change move retrieval quality? But the number IS NOT a leader benchmark.
When publishing internal progress (atlas STATE.md, X posts, etc.), label it
"recall_reach (NOT leader-comparable)" explicitly. Historical DAWN benchmark
numbers in `atlas/dawn/memory/STATE.md` are `recall_reach` unless otherwise
labeled.

---

## Architecture

A C binary (`bench_retrieval`) exercises DAWN's real embedding engine and
document search scoring. A Python script (`run_benchmark.py`) loads benchmark
datasets, drives the C binary via JSON-lines on stdin/stdout, and computes
metrics. The binary uses an in-memory SQLite database — it never touches
DAWN's production data.

## Build

```bash
cmake --preset debug && make -C build-debug bench_retrieval
```

## Datasets

### LongMemEval (500 questions)

Source: [xiaowu0162/longmemeval-cleaned](https://huggingface.co/datasets/xiaowu0162/longmemeval-cleaned) on HuggingFace.

```bash
mkdir -p ~/datasets/longmemeval
curl -fsSL -o ~/datasets/longmemeval/longmemeval_s_cleaned.json \
  https://huggingface.co/datasets/xiaowu0162/longmemeval-cleaned/resolve/main/longmemeval_s_cleaned.json
```

### LoCoMo (10 conversations, ~2000 QA pairs)

Source: [snap-research/locomo](https://github.com/snap-research/locomo) on GitHub.

```bash
git clone --depth 1 https://github.com/snap-research/locomo.git ~/datasets/locomo
```

### ConvoMem (75K+ QA pairs)

Source: [Salesforce/ConvoMem](https://huggingface.co/datasets/Salesforce/ConvoMem) on HuggingFace.

```bash
mkdir -p ~/datasets/convomem
curl -fsSL -o ~/datasets/convomem/user_evidence_sample.json \
  "https://huggingface.co/datasets/Salesforce/ConvoMem/resolve/main/core_benchmark/evidence_questions/user_evidence/1_evidence/0050e213-5032-42a0-8041-b5eef2f8ab91_Telemarketer.json"
```

For the full dataset (all 6 categories), use `huggingface-cli`:

```bash
huggingface-cli download Salesforce/ConvoMem --repo-type dataset --local-dir ~/datasets/convomem
```

## Current Internal Baselines (May 2026) — `recall_reach`, NOT leader-comparable

These are DAWN's internal retrieval-reach numbers — top-K provenance overlap
with gold dialog IDs. Use them for regression testing across DAWN-internal
changes ("did this commit move retrieval quality?"). **Do NOT quote these
alongside ByteRover/MemMachine/Hindsight/Mem0 published headlines** — see
the warning section above. For leader-comparable numbers, see
`atlas/dawn/memory/STATE.md`.

Pipeline: `bge-small-en-v1.5-int8` bi-encoder + hybrid scoring (cosine + keyword
+ temporal-query proximity + proper-noun boost).

| Benchmark | `recall_reach` | Config |
|---|---|---|
| **LongMemEval R@5** | **97.0%** | turn-level, official scoring, top-K=5 |
| **LongMemEval NDCG@5** | **92.5%** | turn-level, official scoring |
| **LoCoMo overall** | **91.7%** | session granularity, top-K=10 (after Phase 0+1A+2.1, May 14 2026) |
| LoCoMo cat-1 (single-hop) | 82.8% | — |
| LoCoMo cat-2 (temporal) | 92.4% | — |
| LoCoMo cat-3 (inference) | 79.5% | — |
| LoCoMo cat-4 (multi-hop) | 95.8% | — |
| **ConvoMem avg recall** | **99.0%** | 100 items |

**Granularity note for LoCoMo.** The argparse default is `--granularity session`
(one doc per session, all dialog turns concatenated). The standard run command
below produces the published 81.6% / 64.4% numbers at session granularity. At
true dialog granularity (`--granularity dialog`, one doc per dialog turn — closer
to production retrieval), the same pipeline scores **64.9% overall / 44.6% cat-3**.
The dialog-level number is what session-neighbor boost (below) targets and lifts.

**Top-K depth ceiling** (recall if all top-K positions are returned — informs
whether a wider candidate pool would help):

| top-K | LoCoMo overall | Cat-1 | Cat-3 |
|---|---|---|---|
| 10 | 81.6% | 69.3% | 64.4% |
| 15 | 89.9% | 82.8% | 77.8% |

## Running

> **Important — required flags.**  Always pass `--temporal-weight 0.20` and
> `--proper-noun-boost 1.0` for standard runs.  Omitting them gives the
> pre-S7 (April 2026) baseline and looks like a regression.  These signals
> are part of DAWN's hybrid scoring; the orchestrator defaults them to 0
> (disabled) so ablation runs are also possible without code changes.

### Standard LoCoMo run (~2 min)

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark locomo \
    --dataset ~/datasets/locomo/data/locomo10.json \
    --temporal-weight 0.20 \
    --proper-noun-boost 1.0
```

### Standard LongMemEval run (~95 min on Jetson Orin, turn granularity)

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark longmemeval \
    --dataset ~/datasets/longmemeval/longmemeval_s_cleaned.json \
    --granularity turn --turn-scoring official \
    --temporal-weight 0.20 \
    --proper-noun-boost 1.0
```

### Standard ConvoMem run (~2 min)

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark convomem \
    --dataset ~/datasets/convomem/user_evidence_sample.json \
    --limit 100 \
    --temporal-weight 0.20 \
    --proper-noun-boost 1.0
```

### Quick smoke test (subset of LongMemEval, ~4 min)

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark longmemeval \
    --dataset ~/datasets/longmemeval/longmemeval_s_cleaned.json \
    --granularity turn --turn-scoring official \
    --temporal-weight 0.20 --proper-noun-boost 1.0 \
    --limit 50
```

## Experiments

### Test a different ONNX model without recompiling

`bench_retrieval` honors `DAWN_ONNX_MODEL` for the embedding provider's model
path. Useful for A/B-testing model swaps before changing `MODEL_PATH` in
`src/memory/memory_embed_onnx.c`.

```bash
DAWN_ONNX_MODEL=models/embeddings/bge-base-en-v1.5.onnx \
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark locomo \
    --dataset ~/datasets/locomo/data/locomo10.json \
    --temporal-weight 0.20 --proper-noun-boost 1.0
```

### Session-neighbor boost (LoCoMo dialog only)

Mitigates session-fragmentation in LoCoMo dialog-granularity runs: top-N items
by cosine become session anchors (split `dia_id` on `':'`), and chunks sharing
an anchor's session prefix get an additive boost before keyword re-ranking.
Off by default. No-op for any dataset whose doc IDs don't contain `':'`
(LoCoMo session, ConvoMem, LongMemEval all reproduce baseline to four
decimal places when these flags are set).

Validated settings (May 2026 sweep): `window=3, boost=0.03` →
LoCoMo dialog overall **+3.0pp** (cat-3 +2.5, cat-4 +3.6, cat-5 +5.5,
cat-1 -0.6 within noise on 282 questions).

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark locomo \
    --dataset ~/datasets/locomo/data/locomo10.json \
    --granularity dialog \
    --temporal-weight 0.20 --proper-noun-boost 1.0 \
    --session-neighbor-window 3 --session-neighbor-boost 0.03
```

### Top-K depth experiment

Diagnoses whether the right evidence is being retrieved at all (just buried
deeper in the candidate list). Confirms whether a wider candidate pool or
reranker has headroom to help on this dataset.

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark locomo \
    --dataset ~/datasets/locomo/data/locomo10.json \
    --temporal-weight 0.20 --proper-noun-boost 1.0 \
    --top-k 15
```

### Temporal-weight sweep

Re-tune the temporal boost when the embedding model changes. The current 0.20
default was calibrated on bge-small-en-v1.5-int8.

```bash
for tw in 0.15 0.20 0.25 0.30; do
    echo "=== temporal-weight $tw ===" && \
    python3 benchmarks/run_benchmark.py \
        --binary ./build-debug/tests/bench_retrieval \
        --benchmark locomo \
        --dataset ~/datasets/locomo/data/locomo10.json \
        --temporal-weight $tw --proper-noun-boost 1.0
done
```

### Raw cosine baseline (no hybrid scoring)

For apples-to-apples comparison with published bi-encoder-only systems, use
`--raw` to disable keyword boosting (and omit the temporal/proper-noun flags).

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark longmemeval \
    --dataset ~/datasets/longmemeval/longmemeval_s_cleaned.json \
    --raw
```

### Alternate provider (Ollama)

The default provider is ONNX (`bge-small-en-v1.5-int8`, 384 dims). For
experimenting with other models on a separate Ollama instance:

```bash
ollama pull bge-small
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark longmemeval \
    --dataset ~/datasets/longmemeval/longmemeval_s_cleaned.json \
    --provider ollama --model bge-small --endpoint http://localhost:11434
```

### Memory-pipeline mode (LoCoMo only) + extraction caching

`--memory-pipeline` swaps raw-dialog cosine retrieval for the production path:
each LoCoMo session is ingested as DAWN messages, the configured extraction
LLM (`extraction_provider` / `extraction_model` from `dawn.toml`) is fired at
each session boundary, and the resulting `memory_facts` (with v40 provenance
back-links) become the retrieval corpus.  The orchestrator scores
`recall_reach` via provenance overlap — see
[atlas/dawn/memory/LOCOMO_CAT3_PROFILING.md](https://github.com/The-OASIS-Project/atlas/blob/main/dawn/memory/LOCOMO_CAT3_PROFILING.md)
for what that does and doesn't measure.

Extraction is the slow part (hundreds of cloud LLM calls per LoCoMo run).  The
runner caches per-conversation extraction state under `--cache-dir` (default
`./benchmarks/snapshots/`) and reuses it on subsequent runs.  Cache keys
include the snapshot format version, extraction provider/model, **a SHA-256
of the live `MEMORY_EXTRACTION_PROMPT_TEMPLATE` body**, embedding
provider/dims, conv index, and a SHA-256 hash of the conversation contents,
so any change to those — including any edit to the extraction prompt — auto-
invalidates old entries.  First-run timing on a single LoCoMo conv with
`claude-haiku-4-5` is ~130 s; cache-hit re-run is ~2.6 s (≈50× speedup), and
recall numbers are bit-identical between runs.

**Why the prompt hash is part of the key.**  Before May 2026 the cache key did
NOT include the prompt body, so editing `MEMORY_EXTRACTION_PROMPT_TEMPLATE`
left old snapshots looking valid — re-running against a stale `--cache-dir`
silently served fact corpora extracted under an older prompt, producing
apples-to-oranges numbers.  The bench engine now emits
`extraction_prompt_sha256` in its `ready` JSON, the harness mixes it into
the cache key, and the C side writes it into the snapshot map JSON.  On load
the C side verifies the stored hash against the live code's hash and **fails
loudly** if they disagree (rather than silently falling back to re-extraction,
which costs real $ in LLM calls).  Pre-May-2026 snapshots (bare JSON arrays
without the fingerprint) are also rejected loudly — delete them or point at a
fresh `--cache-dir`.

```bash
# First run: extracts + saves snapshots under ./benchmarks/snapshots/
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark locomo \
    --dataset ~/datasets/locomo/data/locomo10.json \
    --memory-pipeline \
    --temporal-weight 0.20 --proper-noun-boost 1.0

# Subsequent runs reuse the cache; per-conv "cache HIT key=..." log line confirms
# the load.  Result JSON also reports cache_hits / cache_misses counters.
```

Use `--no-cache` to force re-extraction (e.g., when validating non-determinism
or after manually editing extracted facts).  Override the location with
`--cache-dir <path>` if you want to keep multiple cache pools (one per model
sweep, etc.).  Wipe a single key by deleting both the matching `.db` and
`.json` under the cache directory.

### Entailment scoring (LLM judge)

`recall_reach` measures geometric overlap between retrieved facts' provenance
and the gold dia_ids — useful as a diagnostic, but it does not measure whether
a careful reader could actually answer the question from the retrieved facts.
`recall_entailment` closes that gap: an LLM judge sees `(question, gold_answer,
retrieved fact texts)` and answers a strict YES/NO.  Aggregate fraction of YES
verdicts is the metric.

Opt in by passing `--judge-model`:

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark locomo \
    --dataset ~/datasets/locomo/data/locomo10.json \
    --memory-pipeline \
    --temporal-weight 0.20 --proper-noun-boost 1.0 \
    --judge-model claude-haiku-4-5
```

The judge's API key is resolved in this order: `--judge-api-key`, then
`ANTHROPIC_API_KEY` env var, then `claude_api_key` from `./secrets.toml`.

Verdicts persist to `<cache-dir>/judgements.json`, keyed on `(prompt version,
judge model, question, gold answer, sorted retrieved fact texts)`.  Re-running
with the same retrieval + judge is free; first run on a single LoCoMo conv with
Haiku-4.5 was ~123 s (~600 ms/call × 197 QA pairs), re-run was 2.7 s with
identical numbers.  Pass `--no-judge-cache` to force fresh API calls (useful
when iterating on the prompt — bump `ENTAILMENT_PROMPT_VERSION` in the script
if you want old cached entries auto-invalidated instead).

**Judge model selection.** Haiku-4.5 is cheap and good enough for diagnostic
gating during prompt/extraction iteration.  Switch to Sonnet-4.6 or Opus-4.7
for higher-confidence final numbers — the cache key encodes the judge model,
so swapping doesn't lose work, it just adds a parallel set of cached verdicts.
Per-LoCoMo-conv cost on Haiku is roughly $0.02 (197 calls × ~500 in / 5 out
tokens); a full 10-conv run is roughly $0.20.

**What the metric is and isn't.** It is not yet generate-and-judge — there is
no answer being generated by DAWN, only retrieval.  The judge is asking
whether retrieval *could* support a correct answer if a strong synthesizer
were attached.  Generate-and-judge (the leader-comparable metric) is the next
section.

### Generate-and-judge (leader-comparable)

Two LLM calls per QA pair:

1. **Generator** sees the question + retrieved facts and synthesizes a
   candidate answer.  Prompted to be concise and to say "I don't know" when
   the facts don't contain a clear answer.
2. **Correctness judge** sees the question, gold answer, and generated answer
   and returns a strict YES/NO.  Prompted to be lenient on form ("Tuesday" ≡
   "last Tuesday", "5" ≡ "five") and strict on substance, with an explicit
   abstain rule for cat-5: if gold says "Not mentioned" / "no information",
   a generated "I don't know" counts as YES.

This is the metric that published memory systems report — it measures what a
real user would experience if DAWN's retrieval were wired to a real
synthesizer.  `recall_entailment` is a useful upper bound on
`recall_generation` (the generator can't do worse than what retrieval gave
it), so a wide gap between the two pinpoints synthesis-side problems
distinct from retrieval-side ones.

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark locomo \
    --dataset ~/datasets/locomo/data/locomo10.json \
    --memory-pipeline \
    --temporal-weight 0.20 --proper-noun-boost 1.0 \
    --judge-model claude-haiku-4-5 \
    --generator-model claude-haiku-4-5
```

`--generator-model` requires `--judge-model` (the same judge model is reused
for the correctness verdict; cache files are separate, so swapping
generators doesn't invalidate correctness verdicts and vice versa).

Cache layout under `<cache-dir>/`:

| File | Contains | Cache key |
|---|---|---|
| `judgements.json` | Entailment verdicts (Phase 2.2) | judge model + entailment prompt version + question + gold + sorted retrieved facts |
| `generations.json` | Generated answers | generator model + generator prompt version + question + sorted retrieved facts |
| `correctness.json` | Correctness verdicts | judge model + correctness prompt version + question + gold + generated answer |

Each cache file is independent — bumping `GENERATOR_PROMPT_VERSION` in the
script invalidates only generations (and transitively only the correctness
verdicts that hashed the now-stale generations); entailment verdicts are
untouched.  A generator API error caches an empty raw answer, which
short-circuits the correctness judge through its empty-generation fast-path
on rerun (no wasted API call on transient errors).

**Cost estimate (Haiku-4.5).** Generator ~500 in / 50 out tokens; correctness
~150 in / 5 out.  Per LoCoMo conv (~197 QA): ~$0.12 cold, free on rerun.
Full LoCoMo (10 convs, ~2000 QA): ~$1.20 cold.  Use `claude-sonnet-4-6` for
the judge (and optionally generator) for higher-confidence final numbers —
the cache key encodes the model so a Haiku→Sonnet upgrade just adds a
parallel pool of verdicts.

**Smoke run (1 LoCoMo conv, Haiku-4.5 for both):** 590 s cold (197 generator
+ 196 correctness calls), 3.2 s warm — `recall_reach=0.602`,
`recall_entailment=0.193`, `recall_generation=0.173`.  The `reach > ent > gen`
ordering held within and across categories; cat-3 inference showed
`reach=0.36 → ent=0.64 → gen=0.18`, the canonical "retrieval covers it but
synthesis can't close the inference" signal.

### Four-model extraction sweep

`--sweep-extraction-models` runs the full benchmark once per extraction
model and emits a comparison table.  Pass a comma-separated list; entries
are `model` (default provider `claude`) or `provider:model`:

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark locomo \
    --dataset ~/datasets/locomo/data/locomo10.json \
    --memory-pipeline \
    --temporal-weight 0.20 --proper-noun-boost 1.0 \
    --judge-model claude-haiku-4-5 \
    --generator-model claude-haiku-4-5 \
    --sweep-extraction-models \
        claude-haiku-4-5,claude-sonnet-4-6,claude-opus-4-7,openai:gpt-4o-mini
```

Each model gets its own snapshot in `<cache-dir>/` (cache key includes the
extraction model, so models never collide).  Judge/generator/correctness
caches are also segmented per (model, prompt version), so swapping
extraction models does not invalidate downstream verdicts that share their
inputs.

The sweep prints a per-model summary table and a per-category breakdown
across models for the strictest available metric (`recall_generation` if
generator is enabled, else `recall_entailment`, else `recall_reach`).
Per-model JSONs are saved to `<cache-dir>/sweep_<provider>_<model>.json`;
the aggregate (with all models) goes to `--output` if provided.

**What stays constant across the sweep.** Embedding model, judge model,
generator model, retrieval scoring weights, dataset.  Only extraction
varies — the experiment is "given identical retrieval and identical
synthesis, does extraction quality differ between Haiku/Sonnet/Opus?".  If
the judge has a strong opinion about a generator's wording, hold that
opinion constant by keeping the same judge model across all entries.

**Cost.** Each model in the sweep costs one cold extraction pass over the
dataset (no reuse across models).  Re-running a sweep is free (snapshot
cache hits per model).  Generator + correctness costs are per-(model, QA
pair) but cached too — adding a fifth model only re-extracts and re-judges
that fifth model.

#### Four-model sweep results (May 2026)

Full LoCoMo (10 convs, 1982 QA pairs).  Embedding `bge-small-en-v1.5-int8`,
judge / generator / correctness all `claude-haiku-4-5`, top-K=10,
temporal-weight 0.20, proper-noun-boost 1.0.

| Extraction model | reach | ent | gen |
|---|---|---|---|
| `claude-haiku-4-5` | 0.7392 | 0.2568 | 0.2084 |
| `claude-sonnet-4-6` | **0.7506** | **0.2583** | **0.2250** |
| `claude-opus-4-6` | 0.6739 | 0.1821 | 0.1599 |
| `local:Qwen3.6-35B-A3B-Q4_K_M` | 0.6186 | 0.1700 | 0.1564 |

Per-category `recall_generation`:

| cat | haiku | sonnet | opus | local | what it tests |
|---|---|---|---|---|---|
| 1 | 0.209 | **0.248** | 0.181 | 0.152 | profile facts |
| 2 | 0.022 | **0.028** | 0.028 | 0.016 | temporal |
| 3 | **0.228** | 0.185 | 0.174 | 0.185 | inference |
| 4 | 0.316 | **0.348** | 0.249 | 0.237 | single-hop |
| 5 | **0.135** | 0.128 | 0.072 | 0.103 | adversarial |

**Headline takeaways:**

- Sonnet wins overall but only barely — `gen` +1.7 pp over Haiku, `ent`
  effectively tied. Not a meaningful price/performance gain at ~5× the
  cost.
- Opus is meaningfully **worse** than Haiku (`gen` -4.9 pp) — produces
  ~2-3× the entity count per conv (~43 vs ~15), suggesting it
  over-fragments the entity graph and the resulting facts retrieve
  less reliably.
- Local Qwen3.6-35B-A3B-Q4_K_M trails the cloud field but is real for
  fully-local deployments (free, ~3 hours wall-clock).
- **Cat-2 temporal is broken across all four models** (0.02-0.03 `gen`).
  Bottleneck is extraction-side date loss, not model strength.
  Highest-leverage future improvement target.

Going to a larger Claude tier is not a free win: Opus actively hurts.
Raw data: `benchmarks/snapshots/sweep_results.json`.

#### Claude Haiku 5.5 extraction (October 2026)

Full LoCoMo under the **leader-comparable** Mem0 protocol (the canonical
run above: generator and judge `gpt-4o-mini`, `--prompt-style mem0`,
`--with-source`, `--exclude-categories 5`; n=1536), so these numbers are
not on the May table's scale.  `--extraction-effort` sets
`[memory] extraction_effort`.

| Extraction | gen | ent | cat-1 | cat-2 | cat-3 | cat-4 | cost vs 4.5 | time/session |
|---|---|---|---|---|---|---|---|---|
| `claude-haiku-4-5` | 0.7279 | 0.731 | 0.723 | 0.676 | 0.500 | 0.774 | 100% | 16 s |
| `claude-haiku-5-5`, effort off | 0.7214 | 0.704 | 0.759 | 0.704 | 0.522 | 0.737 | 11% | 6 s |
| `claude-haiku-5-5`, effort low | 0.7601 | 0.759 | 0.738 | 0.785 | 0.522 | 0.791 | 15% | 12 s |
| `claude-haiku-5-5`, effort medium | **0.7689** | 0.759 | 0.752 | 0.779 | 0.533 | 0.803 | 19% | 18 s |

Low and medium are means of two runs (low 0.7637 / 0.7565, medium
0.7721 / 0.7656); a re-run of one setting moves `gen` by about 0.7 pp.
Medium beats low by +0.85 pp in both pairs, but pooled that is not
significant (McNemar p = 0.22).  Cost is per extraction call at list
prices, measured on LoCoMo conv 0's 19 sessions: Haiku 5.5 reads ~42%
more input tokens for the same text (new tokenizer), and medium writes
~2.4x the output tokens of effort off, at a tenth of Haiku 4.5's price.
`bench_temporal_arithmetic.py` agrees: 79.5% (4.5), 71.8% (5.5 off),
89.7% (low), 92.3% (medium).

**Practical recommendation:** `extraction_model = claude-haiku-5-5` with
`extraction_effort = "medium"`, and `compact_model = claude-haiku-5-5` by
extension (compaction isn't benchmarked).

### Save results to JSON

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark longmemeval \
    --dataset ~/datasets/longmemeval/longmemeval_s_cleaned.json \
    --granularity turn --turn-scoring official \
    --temporal-weight 0.20 --proper-noun-boost 1.0 \
    --output results.json
```

## After Running: What to Update

When results change meaningfully (>0.5 pp on any benchmark):

1. **Atlas design doc** — update the results tables in
   `~/code/The-OASIS-Project/atlas/dawn/memory/SYSTEM_DESIGN.md` §14.3:
   - Session-level results table
   - Turn-level results table
   - Comparison-to-published-baselines table (update the DAWN row and SOTA margin)
   - LoCoMo category breakdown table
   - Analysis bullet points
2. **`docs/TODO.md`** — update the shipped-section entry that tracks the
   latest retrieval baseline numbers.
3. **This file** — update the "Current Baselines" table above.

Atlas is the primary record; the table here is a fast-reference copy that
should match.

## Granularity (LongMemEval-only)

Session-level (~48 docs per question, top-K=10) is faster but easier.
Turn-level (~273 docs per question, top-K=5) is what academic papers like RMM
(Tan et al., ACL 2025) use. The standard run uses turn-level.

Two scoring modes for turn-level:

- `official` (default): any user turn from the answer session counts as
  correct.  Matches the official LongMemEval evaluation code.
- `strict`: only turns annotated with `has_answer=true` count.  Harder
  criterion (~1.7 targets per question vs ~11 for official).

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark longmemeval \
    --dataset ~/datasets/longmemeval/longmemeval_s_cleaned.json \
    --granularity turn --turn-scoring strict \
    --temporal-weight 0.20 --proper-noun-boost 1.0
```

## Flags Quick Reference

| Flag | Values | Notes |
|---|---|---|
| `--temporal-weight` | 0.0–1.0 | **Always 0.20 for standard runs**; 0.0 omits temporal boost (ablation) |
| `--proper-noun-boost` | 0.0–2.0 | **Always 1.0 for standard runs**; 0.0 for ablation |
| `--granularity` | `session`, `turn`, `dialog` | Argparse default `session`. Standard LoCoMo runs use the default (session); LME standard runs pass `turn`. Pass `dialog` for production-aligned LoCoMo retrieval. |
| `--turn-scoring` | `official`, `strict` | LME only; `official` for published comparisons |
| `--top-k` | int | Default 10; use 15–20 for depth experiments |
| `--sentence-chunks` | flag | LoCoMo only; tested May 2026 — zero effect, don't use |
| `--session-neighbor-window` | int | Top-N items by cosine become session anchors (split doc id on `':'`). 0 = off (default). LoCoMo dialog only. |
| `--session-neighbor-boost` | 0.0–0.20 | Additive score for chunks matching an anchor's session prefix. Validated 0.03 with `window=3`. |
| `--raw` | flag | Disables keyword boost; pure cosine baseline only |
| `--limit` | int | Cap dataset size for quick smoke tests |
| `--provider` | `onnx`, `ollama`, `openai` | Default `onnx` |
| `--model` | str | Override provider's model name |
| `--endpoint` | url | Required for `ollama`/`openai` providers |
| `--memory-pipeline` | flag | LoCoMo only: extraction + memory_facts retrieval (vs raw dialog cosine) |
| `--config` | path | Path to `dawn.toml` for `--memory-pipeline` (default `./dawn.toml`) |
| `--cache-dir` | path | Snapshot cache directory for `--memory-pipeline` (default `./benchmarks/snapshots`) |
| `--no-cache` | flag | Force fresh extraction; skip both load and save of snapshots |
| `--judge-model` | str | Anthropic model for entailment judge (e.g., `claude-haiku-4-5`). Empty disables. Memory-pipeline LoCoMo only. |
| `--judge-provider` | str | Judge provider; only `anthropic` supported in v1 |
| `--judge-api-key` | str | API key fallback chain: flag → `ANTHROPIC_API_KEY` env → `secrets.toml` claude_api_key |
| `--judge-temperature` | float | Judge sampling temperature (default 0.0) |
| `--judge-max-facts` | int | Cap on retrieved facts shown to judge (default 20) |
| `--no-judge-cache` | flag | Disable disk cache of judge verdicts (entailment + generations + correctness) |
| `--generator-model` | str | Anthropic model for the answer generator (e.g., `claude-haiku-4-5`). Empty disables generate-and-judge. Requires `--judge-model`. |
| `--generator-temperature` | float | Generator sampling temperature (default 0.0) |
| `--generator-max-tokens` | int | Max generator output tokens (default 200) |
| `--sweep-extraction-models` | str | Comma-separated list (`model` or `provider:model`). Runs the benchmark once per model and emits a comparison table. Memory-pipeline LoCoMo only. |
| `--output` | path | Save full results JSON |

Environment variables:

| Var | Notes |
|---|---|
| `DAWN_ONNX_MODEL` | Override the ONNX model file path without recompiling |

## Metrics

| Benchmark | Metric | Description |
|---|---|---|
| LongMemEval | Recall@K | Did the correct session/turn appear in the top K results? |
| LongMemEval | NDCG@K | Position-weighted relevance (higher = correct answer ranked earlier) |
| LoCoMo | Avg Recall | Fraction of evidence dialog IDs found in top 10, averaged across QA pairs |
| ConvoMem | Avg Recall | Fraction of evidence messages found via substring match in top 10 |

LoCoMo is also broken down by question category (1–5: profile facts,
temporal, inference, single-hop, adversarial) — see the per-category numbers
in the baseline table above.
