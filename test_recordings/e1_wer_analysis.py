#!/usr/bin/env python3
"""
E1 audio_ctx sweep analyzer.

Reads the combined CSV from e1_audio_ctx_sweep.sh and the reference transcripts
in recording_guide.txt, then reports — per audio_ctx value — mean transcription
latency, mean RTF, and mean word error rate (WER). The point is to see how far
audio_ctx can be scaled down before WER regresses, i.e. to choose the length->ctx
floor for the production auto-scale.

Usage:  python3 e1_wer_analysis.py [e1_sweep.csv] [recording_guide.txt]
"""

import csv
import os
import re
import sys
from collections import defaultdict

CSV_PATH = sys.argv[1] if len(sys.argv) > 1 else "e1_sweep.csv"
GUIDE_PATH = sys.argv[2] if len(sys.argv) > 2 else "recording_guide.txt"


def normalize(text):
    """Lowercase, drop punctuation/ellipses, collapse whitespace -> word list."""
    text = text.lower()
    text = text.replace("...", " ")
    text = re.sub(r"[^a-z0-9\s]", " ", text)
    return text.split()


def wer(ref_words, hyp_words):
    """Word-level Levenshtein distance / reference length."""
    if not ref_words:
        return 0.0 if not hyp_words else 1.0
    # DP edit distance over word lists.
    prev = list(range(len(hyp_words) + 1))
    for i, rw in enumerate(ref_words, 1):
        cur = [i]
        for j, hw in enumerate(hyp_words, 1):
            cost = 0 if rw == hw else 1
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost))
        prev = cur
    return prev[-1] / len(ref_words)


def load_references(path):
    """Parse 'test_XXX.wav (Ns):  [dir] "reference text"' lines -> {basename: text}."""
    refs = {}
    line_re = re.compile(r'(test_\d+\.wav).*?"([^"]*)"')
    with open(path) as fh:
        for line in fh:
            m = line_re.search(line)
            if m:
                refs[m.group(1)] = m.group(2)
    return refs


def main():
    if not os.path.exists(CSV_PATH):
        sys.exit(f"CSV not found: {CSV_PATH} (run e1_audio_ctx_sweep.sh first)")
    refs = load_references(GUIDE_PATH)
    if not refs:
        sys.exit(f"No references parsed from {GUIDE_PATH}")

    # Per audio_ctx accumulators.
    lat = defaultdict(list)
    rtf = defaultdict(list)
    wers = defaultdict(list)
    missing_ref = set()
    # Track per-clip WER at each ctx to flag regressions vs the baseline (ctx=0).
    per_clip = defaultdict(dict)  # wav -> {ctx: wer}

    with open(CSV_PATH, newline="") as fh:
        for row in csv.DictReader(fh):
            if row.get("success") != "1":
                continue
            wav = os.path.basename(row["wav_file"])
            ctx = int(row["audio_ctx"])
            ref = refs.get(wav)
            if ref is None:
                missing_ref.add(wav)
                continue
            w = wer(normalize(ref), normalize(row.get("transcription", "")))
            lat[ctx].append(float(row["transcription_time_ms"]))
            rtf[ctx].append(float(row["rtf"]))
            wers[ctx].append(w)
            per_clip[wav][ctx] = w

    def mean(xs):
        return sum(xs) / len(xs) if xs else float("nan")

    # negatives (auto, labeled by floor) first, then non-negatives ascending.
    order = sorted(lat.keys(), key=lambda c: (c >= 0, c))
    # Baseline is ctx=0 (Whisper default) when present; otherwise the largest ctx
    # (closest to the full 1500 window), NOT the most-negative auto run.
    baseline = 0 if 0 in lat else max(lat.keys())
    base_ms = mean(lat[baseline])

    print(f"\nE1 audio_ctx sweep — {CSV_PATH}")
    print(f"baseline audio_ctx={baseline} (mean {base_ms:.0f} ms)\n")
    print(f"{'audio_ctx':>10} {'n':>4} {'trans_ms':>10} {'vs base':>9} "
          f"{'rtf':>6} {'WER':>7} {'WER delta':>10}")
    print("-" * 62)
    base_wer = mean(wers[baseline])
    for ctx in order:
        m_ms = mean(lat[ctx])
        speed = f"{(1 - m_ms / base_ms) * 100:+.0f}%" if base_ms else "-"
        m_wer = mean(wers[ctx])
        dwer = f"{(m_wer - base_wer) * 100:+.1f}pp"
        label = f"auto/fl{-ctx}" if ctx < 0 else str(ctx)
        print(f"{label:>10} {len(lat[ctx]):>4} {m_ms:>10.0f} {speed:>9} "
              f"{mean(rtf[ctx]):>6.3f} {m_wer * 100:>6.1f}% {dwer:>10}")

    # Flag clips whose WER got materially worse than baseline at any ctx.
    print("\nPer-clip WER regressions vs baseline (>10pp worse):")
    flagged = False
    for wav in sorted(per_clip):
        b = per_clip[wav].get(baseline)
        if b is None:
            continue
        for ctx in order:
            if ctx == baseline:
                continue
            w = per_clip[wav].get(ctx)
            if w is not None and w - b > 0.10:
                ref = refs.get(wav, "")
                print(f"  {wav} ctx={(f'auto/fl{-ctx}' if ctx < 0 else ctx)}: "
                      f"{b * 100:.0f}% -> {w * 100:.0f}%  (ref: \"{ref}\")")
                flagged = True
    if not flagged:
        print("  none — no clip regressed >10pp at any tested audio_ctx.")

    if missing_ref:
        print(f"\nNote: {len(missing_ref)} clip(s) had no reference in the guide "
              f"(excluded): {', '.join(sorted(missing_ref))}")
    print()


if __name__ == "__main__":
    main()
