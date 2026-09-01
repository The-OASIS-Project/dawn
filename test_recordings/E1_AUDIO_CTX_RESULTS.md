# E1 — Whisper `audio_ctx` scaling: benchmark results

Backing data for the `[asr] audio_ctx_floor` default (768). Whisper's encoder
processes a full 30 s window (`audio_ctx = 1500`) regardless of clip length, so a
short command pays for 30 s of encoder every time. E1 scales `audio_ctx` to the
utterance length (with a floor) to cut transcription latency. This documents the
sweep that chose the floor.

Corpus: the 50 `test_*.wav` clips in this directory (references in
`recording_guide.txt`); model `ggml-small.en.bin` unless noted. Jetson Orin, GPU
Whisper. WER = word error rate vs. the reference (edit distance / ref words);
`trans_ms` = transcription time (model load excluded); `vs base` is relative to
`audio_ctx = 0` (Whisper's default 1500-token window).

## Reproduce

```bash
make -C ../build-debug asr_benchmark
# fixed-vs-auto sweep (small.en):
./e1_audio_ctx_sweep.sh ../models/whisper.cpp/ggml-small.en.bin e1_sweep.csv 0 512 384 256 192 128 -1
python3 e1_wer_analysis.py e1_sweep.csv
# floor tuning (small.en) — negative value = auto-scale with floor = -value:
./e1_audio_ctx_sweep.sh ../models/whisper.cpp/ggml-small.en.bin e1_floor.csv 0 -256 -384 -512 -768
python3 e1_wer_analysis.py e1_floor.csv
# floor tuning (base.en):
./e1_audio_ctx_sweep.sh ../models/whisper.cpp/ggml-base.en.bin e1_floor_base.csv 0 -256 -512 -768
python3 e1_wer_analysis.py e1_floor_base.csv
```

(Raw CSVs from the runs below are committed alongside: `e1_sweep.csv`,
`e1_floor.csv`, `e1_floor_base.csv`.)

## 1. Fixed `audio_ctx` is a trap — it must scale with length (`e1_sweep.csv`, small.en)

| audio_ctx | trans_ms | vs base | WER |
|---|---|---|---|
| auto (−1) | 809 | +25% | 6.2% |
| 0 (baseline) | 1079 | — | 2.2% |
| 512 | 1898 | −76% | 109% |
| 384 | 1596 | −48% | 112% |
| 256 | 1966 | −82% | 244% |
| 192 | 3152 | −192% | 592% |
| 128 | 3182 | −195% | 848% |

A **fixed** `audio_ctx` below what a clip needs makes Whisper hallucinate/loop —
WER in the hundreds of percent *and* 2–3× slower. Only **auto-scale** (never
below the audio's own token need) is viable. Fixed mode is diagnostic-only.

## 2. Floor tuning, small.en (`e1_floor.csv`)

| floor | trans_ms | vs base | WER | WER Δ | clips regressed |
|---|---|---|---|---|---|
| **auto/fl768** | **866** | **+20%** | **3.2%** | **+1.0pp** | 1 (test_020) |
| auto/fl512 | 839 | +23% | 7.2% | +5.0pp | 3 |
| auto/fl384 | 838 | +23% | 6.2% | +4.0pp | 3 |
| auto/fl256 | 818 | +25% | 6.2% | +4.0pp | 3 |
| baseline | 1089 | — | 2.2% | — | — |

fl768 recovers the clips that fail at lower floors; the lone regression is a
benign one-word insertion (`"Friday, stop."` → `"Friday to stop."`).

## 3. Floor tuning, base.en (`e1_floor_base.csv`)

| floor | trans_ms | vs base | WER Δ |
|---|---|---|---|
| auto/fl768 | 662 | +5% | +0.0pp |
| auto/fl512 | 594 | +15% | +0.0pp |
| auto/fl256 | 683 | +2% | **+40.8pp** |
| baseline | 699 | — | — |

base.en is already fast (baseline 699 ms vs small.en 1089), so the latency win is
marginal — but it **craters at fl256** (43% WER), confirming smaller models
hallucinate more readily when context-starved. This is why the floor is chosen
for cross-model safety, not for maximum speed.

## Decision

**Default `audio_ctx_floor = 768`.** It is the model-independent safe floor:
≤ +1 pp WER on both small.en (+20% latency) and base.en (+0 pp, +5%), and it
avoids the base.en cratering seen below 512. The win is real on small.en (the
current model) and harmless on base.en; `0` disables (full 1500 window).
