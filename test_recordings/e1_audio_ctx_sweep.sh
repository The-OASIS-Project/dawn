#!/usr/bin/env bash
#
# E1 audio_ctx sweep: run every test_*.wav clip through the ASR benchmark at a
# range of Whisper encoder audio_ctx values and collect one combined CSV. The
# companion analyzer (e1_wer_analysis.py) then reports latency vs WER per
# audio_ctx so the length->ctx floor can be chosen from data.
#
# audio_ctx semantics (asr_benchmark --audio-ctx):
#   0   = model default (1500 = full 30s window)   <- baseline
#   >0  = fixed value
#   -1  = auto-scale to the clip length (production formula)
#
# Usage:
#   ./e1_audio_ctx_sweep.sh [model.bin] [out.csv] [ctx values...]
# Defaults: small.en model, e1_sweep.csv, ctx set "0 512 384 256 192 128 -1".

# NOTE: intentionally NOT `set -e`/`pipefail` — a 350-run loop must not abort just
# because one benchmark run or one grep produces no match; each run is tolerated.
set -u

cd "$(dirname "$0")"

MODEL="${1:-../models/whisper.cpp/ggml-small.en.bin}"
OUT="${2:-e1_sweep.csv}"
shift || true
shift || true
CTX_VALUES=("$@")
if [ "${#CTX_VALUES[@]}" -eq 0 ]; then
   CTX_VALUES=(0 512 384 256 192 128 -1)
fi

BENCH="../build-debug/tests/asr_benchmark"
if [ ! -x "$BENCH" ]; then
   echo "Error: $BENCH not found. Build it: make -C ../build-debug asr_benchmark" >&2
   exit 1
fi
if [ ! -f "$MODEL" ]; then
   echo "Error: model $MODEL not found." >&2
   exit 1
fi

export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}:/usr/local/lib"

WAVS=(test_*.wav)
echo "Sweeping ${#WAVS[@]} clips x ${#CTX_VALUES[@]} audio_ctx values (${CTX_VALUES[*]})" >&2
echo "Model: $MODEL  ->  $OUT" >&2

header_written=0
: >"$OUT"
tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

for ctx in "${CTX_VALUES[@]}"; do
   n=0
   fail=0
   for wav in "${WAVS[@]}"; do
      # asr_benchmark interleaves INFO logs with the CSV on stdout; a single run's
      # failure must not stop the sweep, so tolerate it and just warn.
      "$BENCH" "$wav" --engines whisper --whisper-model "$MODEL" \
         --audio-ctx "$ctx" --csv >"$tmp" 2>/dev/null || true
      if [ "$header_written" -eq 0 ] && grep -m1 '^wav_file,' "$tmp" >>"$OUT"; then
         header_written=1
      fi
      if grep '^test_' "$tmp" >>"$OUT"; then
         n=$((n + 1))
      else
         fail=$((fail + 1))
         echo "    WARN: no CSV row for $wav @ audio_ctx=$ctx" >&2
      fi
   done
   echo "  audio_ctx=$ctx: $n ok, $fail failed" >&2
done

echo "Done. Rows: $(($(wc -l <"$OUT") - 1)). Analyze: python3 e1_wer_analysis.py $OUT" >&2
