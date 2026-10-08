#!/usr/bin/env python3
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# By contributing to this project, you agree to license your contributions
# under the GPLv3 (or any later version) or any future licenses chosen by
# the project author(s).
#
# Speaker-attribution guardrail for the extraction prompt.
#
# A cheap pre-bench probe (~$0.10 per multi-model run, ~5 minutes) that
# catches the fact-widening + attribution-shift modes that the full LoCoMo
# bench catches at $10 + 2 hours.  Becomes a permanent guardrail: every
# extraction-prompt change must pass this probe before any full bench.
#
# By default the probe runs against THREE providers (Anthropic Haiku +
# OpenAI gpt-5.4-mini + local Qwen3.6-35B) with a 2-of-3 quorum verdict.
# Cross-model robustness was added in Phase 0 of attempt #2 (May 2026)
# after the single-Haiku probe shipped at the end of attempt #1 — see the
# bedrock validation history below.  The quorum tolerates one
# model-specific quirk while requiring real cross-model evidence of
# widening.  Use --provider X to single-out a specific provider for
# debugging.
#
# Both components run from the SAME 25 hand-curated dialog snippets — no
# upstream bench / snapshot machinery, no separate calibration step.  The
# probe parses EXTRACTION_PROMPT_TEMPLATE live from
# src/memory/memory_extraction.c and sends each snippet through each
# provider at temperature 0.0, then aggregates two views over the
# responses:
#
#   (1) FACT-COUNT WIDENING — total facts produced across all 25 cases.
#       Catches the top-K-crowding mechanism that sank attempt #1.  At
#       temperature 0.0 Anthropic and local are highly deterministic;
#       OpenAI shows ~14% run-to-run drift (documented in the bedrock
#       table below — set baseline conservatively).  Per-provider
#       baseline; threshold +10%.  Conceptual basis: when the prompt
#       encourages finer-grained fact splitting (one event → three
#       sub-facts), the 20-fact retrieval top-K dilutes canonical-subject
#       attribution and the generator's source-budget chars get spent on
#       fragments instead of cohesive facts.  Bedrock evidence: attempt
#       #1 widened by +28% on Anthropic and +14% on local — clear
#       cross-model signal — but only +2% on OpenAI.  The 2-of-3 quorum
#       turns this asymmetric signal into a robust reject.
#
#   (2) ATTRIBUTION CORRECTNESS — per-case subject check.
#       Each case has expected and forbidden subject keywords.  Pass
#       requires (a) at least one extracted fact mentions an expected
#       subject, AND (b) no extracted fact mentions a forbidden subject
#       without also mentioning an expected one.  Aggregate metrics:
#       known-good pass count (must stay within KNOWN_GOOD_REGRESS_TOLERANCE
#       of per-provider baseline) + attribution-failure pass count (must
#       stay within ATTRIBUTION_REGRESS_TOLERANCE of per-provider
#       baseline).  Component 2's attribution gate alone wasn't sensitive
#       enough to flag attempt #1 — the test cases are simple enough that
#       both Haiku and attempt #1 got 25/25 — but the gate fires
#       immediately on any future prompt change that breaks the simple
#       case, which is the correctness floor we want to keep.
#
# Bedrock validation 2026-05-07 (attempt #2 Phase 0 cross-model
# extension):
#
#   ┌────────────┬─────────────────┬───────────────────┬─────────┐
#   │ Test       │ Anthropic       │ OpenAI            │ Local   │
#   ├────────────┼─────────────────┼───────────────────┼─────────┤
#   │ HEAD       │ 61 (+1.7%)  ✓   │ 43 (-14%)   ✓     │ 43 ✓    │
#   │ Attempt #1 │ 77 (+28%)  ✗    │ 51 (+2%)    ✓     │ 48 ✗    │
#   │ Revert     │ 58 (-3.3%)  ✓   │ 49 (-2%)    ✓     │ 42 ✓    │
#   └────────────┴─────────────────┴───────────────────┴─────────┘
#   Aggregate verdicts: HEAD=PASS (3/3), Attempt #1=FAIL (1/3 c1),
#   Revert=PASS (3/3).  The probe correctly rejects attempt #1 across
#   models even though one of the three (OpenAI) passes individually.
#
# Cross-model finding: attempt #1's widening signal is asymmetric.  Each
# model reacts differently to extraction-prompt changes; running the
# probe on a single model would have flagged attempt #1 if Haiku were
# chosen, but missed it on OpenAI.  Multi-model + quorum makes the probe
# robust to "single-model favorability" — a future candidate that
# accidentally games one model's prompt-handling style will fail on the
# other two.
#
# Local Qwen3.6-35B-A3B drops good_audrey_dogs at HEAD (extracts only
# Andrew's response, skips Audrey's "I've had two dogs since I was a kid"
# statement entirely).  Calibrated baseline reflects this — it's not a
# regression target, it's a known Qwen quirk.
#
# Workflow for future prompt changes:
#
#   1. Edit src/memory/memory_extraction.c.
#   2. python3 benchmarks/bench_speaker_attribution.py
#      Probe exits non-zero on aggregate failure with per-provider details.
#   3. If green, only then run the full LoCoMo bench (~$10).
#   4. --provider X to debug a single provider; --skip-local when local
#      LLM is offline.
#
# Per-provider HEAD calibration baselines (2026-05-07):
#
#   Anthropic claude-haiku-4-5: 60 facts, attr 20/20, kg 5/5, c3 fix 6/12.
#   OpenAI gpt-5.4-mini:        50 facts, attr 20/20, kg 5/5, c3 fix 1/12.
#   Local Qwen3.6-35B-A3B-Q4:   42 facts, attr 20/20, kg 4/5, c3 fix 5/12.
#
# Component 1 threshold: +10% of per-provider baseline.
# Component 2 thresholds: known_good drop > 1 OR attribution drop > 2 → FAIL.
# Component 3 threshold: candidate fix_count > per-provider baseline (strict).
# Aggregate: 2-of-3 providers must pass each component.
#
# Attempt #2 Phase 1.5 finding (component 3 added 2026-05-07):
#   - Attempt #1 (reverted in attempt #1) IMPROVES c3 fix-rate dramatically:
#     Anthropic 6→7, OpenAI 1→6, local 5→10 — aggregate 12/36 → 23/36
#     (+30.6pp).  But it ALSO widens c1 (+28% on Haiku, +14% on local) which
#     is what loses cat-1/cat-2 generation in full LoCoMo.  The trade-off
#     was invisible to the original c1+c2 probe.
#   - c4 (drop "USER" from EXISTING USER PROFILE header): improves Anthropic
#     by 1 case (6→7), no movement on OpenAI/local.  c1 clean.  Quorum FAIL
#     on c3.  Conclusion: too subtle to deliver real attribution lift.
#   - c5 (replace ALWAYS-name-the-subject bullet with speaker-centric
#     wording): improves Anthropic by 1 case, regresses local by 1 case
#     (5→4).  Aggregate FAIL on c3.  Replacement-only wording without new
#     instructions doesn't move cross-model attribution.
#
# Open: no Phase 1 prompt-only candidate delivers cross-model c3 improvement
# without c1 widening.  The mechanism that produces c3 improvement (explicit
# per-speaker attribution rule à la attempt #1) is the same mechanism that
# produces c1 widening.  Phase 1.5 candidate space: rule-replacement (not
# addition) with attribution emphasis, output-cap directives ("emit at most
# N facts per turn"), or structural changes (build_existing_profile()
# symmetry fix at C-code level).
#
# Recalibrate via the calibration ritual when an extraction model or the
# embedding provider changes:
#
#   1. Update PROVIDERS[X]['default_model'] (or pass --model X).
#   2. Run probe on HEAD with the fresh model.
#   3. Update PROVIDERS[X]['baseline_*'] constants from the printed JSON.
#   4. Re-run the bedrock validation against attempt #1 (apply attempt #1
#      patch — see CAT2_TEMPORAL.md Case 10 for the diff — run probe,
#      confirm aggregate FAIL, revert, confirm aggregate PASS).

import argparse
import json
import os
import re
import sys
from pathlib import Path

# 1i.A factor: dispatch / endpoint / secrets-parsing / quorum constant moved
# to benchmarks/multi_model_probe.py so the focus-injection probe (1i.C)
# can share the same harness.  Probe-specific baselines and case lists
# stay in this file.
from multi_model_probe import (
    AGGREGATE_QUORUM,
    PROVIDER_DEFAULTS,
    PROVIDER_CALLERS,
    _anthropic_call,
    _openai_call,
    _local_call,
    load_secrets,
    read_local_endpoint_from_toml,
    read_local_model_from_endpoint,
    resolve_provider_config,
)

DAWN_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_LOCOMO_PATH = Path.home() / "datasets/locomo/data/locomo10.json"
DEFAULT_SECRETS_PATH = DAWN_ROOT / "secrets.toml"
DEFAULT_DAWN_TOML = DAWN_ROOT / "dawn.toml"
EXTRACTION_C_PATH = DAWN_ROOT / "src/memory/memory_extraction.c"

# Per-provider HEAD baselines.  Calibrated 2026-05-07 against current main
# (post-attempt-#1 revert).  Keys map to (default model, default endpoint,
# baseline fact count, baseline attribution pass rate).  endpoint=None
# means "read from dawn.toml" (used for local).
#
# 1i.A: model + endpoint defaults moved to PROVIDER_DEFAULTS in
# multi_model_probe.py — kept here only as a back-pointer so the
# pre-factor calibration constants stay co-located with the speaker-
# specific baselines.  resolve_provider_config reads from PROVIDER_DEFAULTS,
# so the speaker probe and any future probe see the same defaults.
#
# Recalibrate by running probe with --recalibrate against HEAD; constants
# update from the printed JSON (--json-output).
PROVIDERS = {
    "anthropic": {
        "default_model": PROVIDER_DEFAULTS["anthropic"]["default_model"],
        "endpoint": PROVIDER_DEFAULTS["anthropic"]["endpoint"],
        "baseline_fact_count": 60,
        "baseline_attribution_passed": 20,    # of 20 attribution-failure cases
        "baseline_known_good_passed": 5,       # of 5 known-good cases
    },
    "openai": {
        "default_model": PROVIDER_DEFAULTS["openai"]["default_model"],
        "endpoint": PROVIDER_DEFAULTS["openai"]["endpoint"],
        "baseline_fact_count": 50,
        "baseline_attribution_passed": 20,
        "baseline_known_good_passed": 5,
    },
    "local": {
        "default_model": PROVIDER_DEFAULTS["local"]["default_model"],
        "endpoint": PROVIDER_DEFAULTS["local"]["endpoint"],
        "baseline_fact_count": 42,
        "baseline_attribution_passed": 20,
        # Qwen3.6-35B-A3B drops good_audrey_dogs at HEAD — it extracts Andrew's
        # comment but skips Audrey's "I've had two dogs since I was a kid"
        # statement entirely.  Documented quirk, not a regression target.
        "baseline_known_good_passed": 4,
    },
}

FACT_COUNT_THRESHOLD = 1.10        # fail C1 if candidate > BASELINE * threshold
ATTRIBUTION_REGRESS_TOLERANCE = 2  # fail C2 if attribution_passed < baseline - this many
KNOWN_GOOD_REGRESS_TOLERANCE = 1   # fail C2 if known_good_passed < baseline - this many


# =============================================================================
# Prompt extraction from C source
# =============================================================================

def extract_prompt_template_from_c(c_path):
    """Parse EXTRACTION_PROMPT_TEMPLATE from memory_extraction.c.

    The template is a static const string built by C string-literal
    concatenation across many lines, containing C strings whose literal
    text includes semicolons (e.g. "preferences[];").  A naive regex
    that stops at the first ';' eats half the prompt.  Instead we locate
    the assignment, then walk character by character respecting C
    string-literal escape rules until we find a ';' outside any string.
    Fails loudly if the declaration shape changes.
    """
    src = Path(c_path).read_text()

    decl = re.search(
        r"(?:static\s+)?const\s+char\s*\*\s*(?:MEMORY_)?EXTRACTION_PROMPT_TEMPLATE\s*=\s*",
        src)
    if not decl:
        sys.exit(f"error: EXTRACTION_PROMPT_TEMPLATE declaration not found in {c_path}")

    i = decl.end()
    fragments = []
    in_string = False
    in_block_comment = False
    in_line_comment = False
    cur = []
    n = len(src)
    while i < n:
        ch = src[i]
        if in_block_comment:
            if ch == "*" and i + 1 < n and src[i + 1] == "/":
                in_block_comment = False
                i += 2
                continue
            i += 1
            continue
        if in_line_comment:
            if ch == "\n":
                in_line_comment = False
            i += 1
            continue
        if in_string:
            if ch == "\\" and i + 1 < n:
                cur.append(src[i:i + 2])
                i += 2
                continue
            if ch == '"':
                fragments.append("".join(cur))
                cur = []
                in_string = False
                i += 1
                continue
            cur.append(ch)
            i += 1
            continue
        # Outside string and comments — looking for opening literal or ';' terminator.
        if ch == ";":
            break
        if ch == '"':
            in_string = True
            i += 1
            continue
        if ch == "/" and i + 1 < n and src[i + 1] == "*":
            in_block_comment = True
            i += 2
            continue
        if ch == "/" and i + 1 < n and src[i + 1] == "/":
            in_line_comment = True
            i += 2
            continue
        i += 1

    if i >= n:
        sys.exit(f"error: walked past end-of-file looking for ';' terminator")
    if not fragments:
        sys.exit(f"error: no string literals found inside EXTRACTION_PROMPT_TEMPLATE")

    # Decode the small set of C escapes we actually use.
    def decode(s):
        return (s.replace(r"\n", "\n")
                 .replace(r"\t", "\t")
                 .replace(r'\"', '"')
                 .replace(r"\\", "\\"))

    return "".join(decode(f) for f in fragments)


# =============================================================================
# Provider dispatch + endpoint resolution + secrets parsing all live in
# benchmarks/multi_model_probe.py — imported at the top of this file
# alongside AGGREGATE_QUORUM and PROVIDER_DEFAULTS.  The pre-factor
# implementations were 200+ lines that this file no longer carries.
# =============================================================================


# =============================================================================
# Component 1 — fact-count widening
#
# Computed from the same case_results component 2 produces.  No separate
# extraction or snapshot needed.  The metric is the total number of facts
# extracted across all 25 cases; threshold is per-provider baseline * 1.10.
# =============================================================================


def component1_fact_count(case_results, baseline_fact_count):
    """Aggregate total fact count across all test cases.  Compares against
    the per-provider baseline.  Returns (pass, info)."""
    total = sum(len(r.get("fact_texts") or []) for r in case_results)
    threshold = int(baseline_fact_count * FACT_COUNT_THRESHOLD)
    if baseline_fact_count > 0:
        delta_pct = 100.0 * (total - baseline_fact_count) / baseline_fact_count
    else:
        delta_pct = 0.0  # calibration mode: baseline not yet set
    info = {
        "baseline_fact_count": baseline_fact_count,
        "candidate_fact_count": total,
        "threshold_fact_count": threshold,
        "delta_pct": round(delta_pct, 1),
        "n_cases": len(case_results),
    }
    # In calibration mode (baseline=0) component 1 always passes — the run
    # is just measuring, not gating.
    if baseline_fact_count == 0:
        return True, info
    return total <= threshold, info


# =============================================================================
# Targeted attribution test cases (component 2)
# =============================================================================
#
# Each case has: a 1-3 turn dialog snippet (with explicit "Speaker said,"
# prefixes mirroring the LoCoMo bench harness), an anchor date for the
# extraction prompt, expected/forbidden subject keywords (case-insensitive
# substring match against fact_text).  Pass requires:
#   - at least one extracted fact contains an expected subject, AND
#   - no extracted fact contains a forbidden subject without also
#     containing an expected subject.
#
# 20 attribution-failure cases (assistant-side personal statements) +
# 5 known-good cases (user-side personal statements as positive control).
# All sourced from LoCoMo10 cat-1/cat-2 evidence dialogs.

# fmt: off
TEST_CASES = [
    # ---- Conv 7 (Deborah/Jolene) — Case 10 family --------------------------
    {
        "id": "case10_jolene_pendant",
        "kind": "attribution_failure",
        "anchor": "2023-01-15",
        "turns": [
            ("user", 'Deborah said, "Hey Jolene, nice to meet you! Have you found a way to remember her?"'),
            ("assistant", 'Jolene said, "This pendant reminds me of my mother, she gave it to me in 2010 in Paris."'),
        ],
        "expected_subjects": ["jolene"],
        "forbidden_subjects": ["deborah"],
    },
    # ---- Conv 0 (Caroline=user / Melanie=assistant) ------------------------
    {
        "id": "melanie_charity_race",
        "kind": "attribution_failure",
        "anchor": "2023-05-25",
        "turns": [
            ("user", 'Caroline said, "Hey Mel! What\'s been going on?"'),
            ("assistant", 'Melanie said, "Hey Caroline, since we last chatted, I\'ve had a lot of things happening to me. I ran a charity race for mental health last Saturday – it was tough but rewarding."'),
        ],
        "expected_subjects": ["melanie"],
        "forbidden_subjects": ["caroline"],
    },
    {
        "id": "melanie_pottery_class",
        "kind": "attribution_failure",
        "anchor": "2023-07-03",
        "turns": [
            ("user", 'Caroline said, "Doing anything creative lately?"'),
            ("assistant", 'Melanie said, "Wow, Caroline! That\'s great! I just signed up for a pottery class yesterday. It\'s like therapy for me."'),
        ],
        "expected_subjects": ["melanie"],
        "forbidden_subjects": ["caroline"],
    },
    {
        "id": "melanie_clarinet",
        "kind": "attribution_failure",
        "anchor": "2023-08-30",
        "turns": [
            ("user", 'Caroline said, "Do you play any instruments?"'),
            ("assistant", 'Melanie said, "Yeah, I play clarinet! Started when I was young and it\'s been great. Expression of myself and a way to relax."'),
        ],
        "expected_subjects": ["melanie"],
        "forbidden_subjects": ["caroline"],
    },
    {
        "id": "melanie_book_nothing_impossible",
        "kind": "attribution_failure",
        "anchor": "2023-07-12",
        "turns": [
            ("user", 'Caroline said, "Read anything good recently?"'),
            ("assistant", 'Melanie said, "I read \\"Nothing is Impossible\\" back in 2022 — it really stuck with me."'),
        ],
        "expected_subjects": ["melanie"],
        "forbidden_subjects": ["caroline"],
    },
    # ---- Conv 1 (Gina=user / Jon=assistant) --------------------------------
    {
        "id": "jon_paris_trip",
        "kind": "attribution_failure",
        "anchor": "2023-01-30",
        "turns": [
            ("user", 'Gina said, "What have you been up to lately, Jon?"'),
            ("assistant", 'Jon said, "I traveled to Paris on the 28th — it was an incredible trip."'),
        ],
        "expected_subjects": ["jon"],
        "forbidden_subjects": ["gina"],
    },
    # ---- Conv 2 (Maria=user / John=assistant) ------------------------------
    {
        "id": "john_personal_statement",
        "kind": "attribution_failure",
        "anchor": "2023-06-15",
        "turns": [
            ("user", 'Maria said, "How have things been with you, John?"'),
            ("assistant", 'John said, "I started a new job at a tech startup in May 2023 — it\'s been a huge change."'),
        ],
        "expected_subjects": ["john"],
        "forbidden_subjects": ["maria"],
    },
    # ---- Conv 3 (Nate=user / Joanna=assistant) -----------------------------
    {
        "id": "joanna_screenplay",
        "kind": "attribution_failure",
        "anchor": "2022-05-20",
        "turns": [
            ("user", 'Nate said, "How\'s the writing going, Joanna?"'),
            ("assistant", 'Joanna said, "I\'m working on my third screenplay right now — trying to finish a draft by the end of the month."'),
        ],
        "expected_subjects": ["joanna"],
        "forbidden_subjects": ["nate"],
    },
    {
        "id": "joanna_woodhaven_visit",
        "kind": "attribution_failure",
        "anchor": "2022-07-15",
        "turns": [
            ("user", 'Nate said, "Been anywhere fun?"'),
            ("assistant", 'Joanna said, "I visited Woodhaven in July 2022 — beautiful little town."'),
        ],
        "expected_subjects": ["joanna"],
        "forbidden_subjects": ["nate"],
    },
    # ---- Conv 4 (John=user / Tim=assistant) --------------------------------
    {
        "id": "tim_personal_health",
        "kind": "attribution_failure",
        "anchor": "2023-09-10",
        "turns": [
            ("user", 'John said, "How have you been holding up?"'),
            ("assistant", 'Tim said, "Honestly, I started running every morning at the beginning of August. It\'s been good for my head."'),
        ],
        "expected_subjects": ["tim"],
        "forbidden_subjects": ["john"],
    },
    # ---- Conv 5 (Audrey=user / Andrew=assistant) ---------------------------
    {
        "id": "andrew_dog_adoption",
        "kind": "attribution_failure",
        "anchor": "2023-04-12",
        "turns": [
            ("user", 'Audrey said, "Any new pets in your life?"'),
            ("assistant", 'Andrew said, "I adopted a rescue dog named Max last weekend — he\'s already settled in."'),
        ],
        "expected_subjects": ["andrew"],
        "forbidden_subjects": ["audrey"],
    },
    # ---- Conv 6 (John=user / James=assistant) — Cyberpunk case ------------
    {
        "id": "james_cyberpunk",
        "kind": "attribution_failure",
        "anchor": "2022-10-21",
        "turns": [
            ("user", 'John said, "Played anything new lately?"'),
            ("assistant", 'James said, "Those games introduced me to gaming and I\'ve been hooked ever since. By the way, yesterday I tried Cyberpunk 2077. Great game, so immersive."'),
        ],
        "expected_subjects": ["james"],
        "forbidden_subjects": ["john"],
    },
    # ---- Conv 7 (Deborah=user / Jolene=assistant) — more cases ------------
    {
        "id": "jolene_paris_pendant_country",
        "kind": "attribution_failure",
        "anchor": "2023-01-15",
        "turns": [
            ("user", 'Deborah said, "Tell me more about that pendant."'),
            ("assistant", 'Jolene said, "My mother bought it for me in France in 2010 — we were visiting Paris together that summer."'),
        ],
        "expected_subjects": ["jolene"],
        "forbidden_subjects": ["deborah"],
    },
    # ---- Conv 8 (Sam=user / Evan=assistant) — Kustom guitar ---------------
    {
        "id": "evan_vintage_guitar",
        "kind": "attribution_failure",
        "anchor": "2023-11-05",
        "turns": [
            ("user", 'Sam said, "What\'s the story with that guitar?"'),
            ("assistant", 'Evan said, "It\'s a 1968 Kustom K-200A vintage guitar and I got it as a gift from a close friend. I lost my job at the end of October 2023, so it\'s been a tough time."'),
        ],
        "expected_subjects": ["evan"],
        "forbidden_subjects": ["sam"],
    },
    # ---- Conv 9 (Calvin=user / Dave=assistant) -----------------------------
    {
        "id": "dave_garden_project",
        "kind": "attribution_failure",
        "anchor": "2023-05-18",
        "turns": [
            ("user", 'Calvin said, "What have you been working on?"'),
            ("assistant", 'Dave said, "I built a vegetable garden in my backyard last April — first time growing tomatoes."'),
        ],
        "expected_subjects": ["dave"],
        "forbidden_subjects": ["calvin"],
    },
    # ---- More Conv 0 cases for diversity ----------------------------------
    {
        "id": "melanie_camping_beach",
        "kind": "attribution_failure",
        "anchor": "2023-06-15",
        "turns": [
            ("user", 'Caroline said, "Where do you like to camp?"'),
            ("assistant", 'Melanie said, "Here\'s a pic of my family camping at the beach. We love it, it brings us together."'),
        ],
        "expected_subjects": ["melanie"],
        "forbidden_subjects": ["caroline"],
    },
    {
        "id": "melanie_horse_painting",
        "kind": "attribution_failure",
        "anchor": "2023-08-15",
        "turns": [
            ("user", 'Caroline said, "Show me your latest art!"'),
            ("assistant", 'Melanie said, "Here\'s a photo of my horse painting I did recently. I\'ve also painted a sunset and a sunrise."'),
        ],
        "expected_subjects": ["melanie"],
        "forbidden_subjects": ["caroline"],
    },
    {
        "id": "melanie_pets",
        "kind": "attribution_failure",
        "anchor": "2023-08-01",
        "turns": [
            ("user", 'Caroline said, "How are the kids and pets?"'),
            ("assistant", 'Melanie said, "Yeah, they\'re good — we got another cat named Bailey to join Oliver and Luna."'),
        ],
        "expected_subjects": ["melanie"],
        "forbidden_subjects": ["caroline"],
    },
    {
        "id": "melanie_pottery_plate",
        "kind": "attribution_failure",
        "anchor": "2023-08-25",
        "turns": [
            ("user", 'Caroline said, "What\'s new in your art?"'),
            ("assistant", 'Melanie said, "I made a plate in pottery class yesterday — I love how relaxing pottery is."'),
        ],
        "expected_subjects": ["melanie"],
        "forbidden_subjects": ["caroline"],
    },
    {
        "id": "melanie_running_destress",
        "kind": "attribution_failure",
        "anchor": "2023-07-15",
        "turns": [
            ("user", 'Caroline said, "How do you handle stress?"'),
            ("assistant", 'Melanie said, "I\'ve been running farther to de-stress, which has been great for my headspace."'),
        ],
        "expected_subjects": ["melanie"],
        "forbidden_subjects": ["caroline"],
    },

    # =====================================================================
    # KNOWN-GOOD positive controls — role=user side personal statements.
    # Should always pass with any reasonable prompt.  If a candidate prompt
    # fails any of these, the simple single-user case is broken.
    # =====================================================================
    {
        "id": "good_caroline_lgbtq",
        "kind": "known_good",
        "anchor": "2023-04-22",
        "turns": [
            ("user", 'Caroline said, "I came out to my parents three years ago — it was a turning point for me."'),
            ("assistant", 'Melanie said, "Wow, Caroline, that takes courage. How did it go?"'),
        ],
        "expected_subjects": ["caroline"],
        "forbidden_subjects": [],  # no specific forbidden — just expect Caroline-attribution
    },
    {
        "id": "good_deborah_meet_jolene",
        "kind": "known_good",
        "anchor": "2023-01-15",
        "turns": [
            ("user", 'Deborah said, "I work at a publishing house in New York and I just got promoted to senior editor."'),
            ("assistant", 'Jolene said, "Congrats, Deborah! That\'s wonderful."'),
        ],
        "expected_subjects": ["deborah"],
        "forbidden_subjects": [],
    },
    {
        "id": "good_audrey_dogs",
        "kind": "known_good",
        "anchor": "2023-04-12",
        "turns": [
            ("user", 'Audrey said, "I\'ve had two dogs since I was a kid — Bella and Max are family to me."'),
            ("assistant", 'Andrew said, "That\'s sweet. Mine\'s an only-child rescue."'),
        ],
        "expected_subjects": ["audrey"],
        "forbidden_subjects": [],
    },
    {
        "id": "good_calvin_japan",
        "kind": "known_good",
        "anchor": "2023-09-10",
        "turns": [
            ("user", 'Calvin said, "I went to Japan for a month last spring — best trip of my life."'),
            ("assistant", 'Dave said, "Whoa, that sounds amazing."'),
        ],
        "expected_subjects": ["calvin"],
        "forbidden_subjects": [],
    },
    {
        "id": "good_sam_homebrew",
        "kind": "known_good",
        "anchor": "2023-11-05",
        "turns": [
            ("user", 'Sam said, "I\'ve been brewing my own beer at home for about two years now."'),
            ("assistant", 'Evan said, "That\'s a cool hobby."'),
        ],
        "expected_subjects": ["sam"],
        "forbidden_subjects": [],
    },
]
# fmt: on


# =============================================================================
# Component 3 — fix-rate mini-bench on conv 7 attribution-shape questions
#
# Components 1+2 are regression gates: they catch fact-widening and known-good
# breakage but cannot tell us whether a candidate actually FIXES anything,
# because the 25 component-2 snippets all pass on HEAD already (Phase 1.1
# subject-naming rule does most of the work for short snippets).  Component 3
# fills that gap with 12 LoCoMo conv-7 cat-2/3 questions where the gold
# answer is supported by Jolene-side (role=assistant) dialog adjacent to a
# Deborah-side (role=user) turn on the same topic — the production
# attribution-ambiguity scenario.
#
# Selection criteria, applied to LoCoMo10 conv 7:
#   - Category in {2 (temporal), 3 (multi-hop)}
#   - Question subject is "Jolene" (assistant-role speaker; first speaker
#     Deborah is role=user in the bench harness)
#   - Evidence dia_id has speaker == "Jolene" (the gold answer is grounded
#     in something Jolene said about herself)
#   - Surrounding ±1 turn is included as the dialog snippet so the candidate
#     prompt has to disambiguate against an adjacent Deborah turn — many of
#     these (D1:8 pendant, D1:6 mother passing) feature Deborah making a
#     parallel claim about herself immediately after Jolene's.
# Mix: 8 cat-2 (temporal arithmetic + absolute dates) + 4 cat-3 (place /
# multi-hop).  Sessions span 1, 2, 4, 5, 7, 13, 19 — covers the full conv-7
# arc, not just session_1 where Case 10 lives.
#
# Per-case pipeline: extract dialog → generate answer from extracted facts →
# judge against gold.  Three LLM calls per case per provider.  fix_rate =
# correct_count / 12.  Pass criteria (per-provider): fix_rate > HEAD baseline
# fix_rate (must improve, not just match).  Aggregate: PASS if ≥ 2 of 3
# providers improve.
#
# Honest limits to flag here so future readers don't over-index on this
# signal:
#   - Single-conv signal isn't truth.  Full LoCoMo at budget=12288 across
#     all 10 convs remains the source of truth for ship/no-ship.  Component
#     3 catches the COARSE attribution-fix shape; it doesn't predict
#     cat-2/cat-3 generation lift to within 1pp.
#   - Question selection involves judgment.  The 12 cases below are picked
#     from 32 LoCoMo conv-7 Jolene-side cat-2/3 candidates; rotation on
#     recalibration is encouraged.  Document the rationale when changing.
#   - HEAD baseline determines lift sensitivity.  If HEAD scores 12/12, no
#     candidate can improve and component 3 has no signal — that is itself
#     a meaningful finding worth reporting at the gate.

# fmt: off
COMPONENT3_CASES = [
    {
        "id": "c3_d1_2_engineering_project",
        "category": "2",
        "anchor": "2023-01-23",
        "turns": [
            ("user", 'Deborah said, "Hey Jolene, nice to meet you! How\'s your week going? Anything fun happened?"'),
            ("assistant", 'Jolene said, "Hi Deb! Good to meet you! Yeah, my week\'s been busy. I finished an electrical engineering project last week — took a lot of work."'),
            ("user", 'Deborah said, "Congrats! Last week I visited a place that holds a lot of memories for me. It was my mother\'s old house."'),
        ],
        "question": "What kind of project was Jolene working on in the beginning of January 2023?",
        "gold": "electrical engineering project",
    },
    {
        "id": "c3_d1_6_mother_passing",
        "category": "2",
        "anchor": "2023-01-23",
        "turns": [
            ("user", 'Deborah said, "It was full of memories, she passed away a few years ago. This is our last photo together."'),
            ("assistant", 'Jolene said, "Sorry about your loss, Deb. My mother also passed away last year. This is my room in her house, I also have many memories there."'),
            ("user", 'Deborah said, "My mom\'s house had a special bench near the window. She loved to sit there every morning."'),
        ],
        "question": "When did Jolene's mother pass away?",
        "gold": "in 2022",
    },
    {
        "id": "c3_d1_8_pendant_when",
        "category": "2",
        "anchor": "2023-01-23",
        "turns": [
            ("user", 'Deborah said, "My mom\'s house had a special bench near the window. She loved to sit there every morning."'),
            ("assistant", 'Jolene said, "Staying connected is super important. Do you have something to remember her by? This pendant reminds me of my mother, she gave it to me in 2010 in Paris."'),
            ("user", 'Deborah said, "Yes, I also have a pendant that reminds me of my mother. And what is special for you about your jewelry?"'),
        ],
        "question": "When did Jolene's mom gift her a pendant?",
        "gold": "in 2010",
    },
    {
        "id": "c3_d1_8_pendant_country",
        "category": "3",
        "anchor": "2023-01-23",
        "turns": [
            ("user", 'Deborah said, "My mom\'s house had a special bench near the window. She loved to sit there every morning."'),
            ("assistant", 'Jolene said, "Staying connected is super important. Do you have something to remember her by? This pendant reminds me of my mother, she gave it to me in 2010 in Paris."'),
            ("user", 'Deborah said, "Yes, I also have a pendant that reminds me of my mother. And what is special for you about your jewelry?"'),
        ],
        "question": "In what country did Jolene's mother buy her the pendant?",
        "gold": "France",
    },
    {
        "id": "c3_d2_24_seraphim_when",
        "category": "2",
        "anchor": "2023-01-27",
        "turns": [
            ("user", 'Deborah said, "Awww, that\'s so nice!"'),
            ("assistant", 'Jolene said, "I bought it a year ago in Paris."'),
            ("user", 'Deborah said, "Cool, Jolene! Pets bring so much happiness!"'),
        ],
        "question": "When did Jolene buy her pet Seraphim?",
        "gold": "in 2022",
    },
    {
        "id": "c3_d2_24_seraphim_country",
        "category": "3",
        "anchor": "2023-01-27",
        "turns": [
            ("user", 'Deborah said, "Awww, that\'s so nice!"'),
            ("assistant", 'Jolene said, "I bought it a year ago in Paris."'),
            ("user", 'Deborah said, "Cool, Jolene! Pets bring so much happiness!"'),
        ],
        "question": "In what country did Jolene buy snake Seraphim?",
        "gold": "France",
    },
    {
        "id": "c3_d4_23_avalanche_book",
        "category": "2",
        "anchor": "2023-02-04",
        "turns": [
            ("user", 'Deborah said, "Great, this is interesting! Have you come across any recent ones that really struck you?"'),
            ("assistant", 'Jolene said, "Two weeks ago I read \\"Avalanche\\" by Neal Stephenson in one sitting!"'),
            ("user", 'Deborah said, "That sounds cool, Jolene. Stories can be so powerful."'),
        ],
        "question": "Which book did Jolene read in January 2023?",
        "gold": "Avalanche by Neal Stephenson",
    },
    {
        "id": "c3_d4_33_bogota_country",
        "category": "3",
        "anchor": "2023-02-04",
        "turns": [
            ("user", 'Deborah said, "Oh, there\'s so many great places! My favorite is a park with a forest trail."'),
            ("assistant", 'Jolene said, "Here\'s a picture I took on vacation last summer in Bogota. It was so beautiful and calming watching the sunset over the water."'),
            ("user", 'Deborah said, "That sounds great, Jolene. Nature\'s calming for sure."'),
        ],
        "question": "In what country was Jolene during summer 2022?",
        "gold": "Colombia",
    },
    {
        "id": "c3_d5_1_mini_retreat",
        "category": "2",
        "anchor": "2023-02-09",
        "turns": [
            ("assistant", 'Jolene said, "Hey Deborah! Been a few days since we last talked so I wanted to fill you in on something cool. Last Wednesday I did a mini retreat to reflect on my career."'),
            ("user", 'Deborah said, "Hey Jolene! Sounds great. Taking time to reflect can be really awesome. Did you gain any new insights from it?"'),
        ],
        "question": "When did Jolene have a mini-retreat to reflect on her career?",
        "gold": "Wednesday before 9 February, 2023",
    },
    {
        "id": "c3_d7_7_dating_year",
        "category": "2",
        "anchor": "2023-02-25",
        "turns": [
            ("user", 'Deborah said, "Aw, that\'s wonderful! How long have you been married?"'),
            ("assistant", 'Jolene said, "We\'re not married yet but we\'ve been together for three years. We\'re taking it slow and loving the ride."'),
            ("user", 'Deborah said, "Sounds nice, Jolene. So, how did you two meet?"'),
        ],
        "question": "Which year did Jolene and her partner start dating?",
        "gold": "2020",
    },
    {
        "id": "c3_d13_15_talkeetna_state",
        "category": "3",
        "anchor": "2023-06-06",
        "turns": [
            ("user", 'Deborah said, "Have you considered taking some breaks and finding activities like yoga to help you relax and unwind?"'),
            ("assistant", 'Jolene said, "Yeah, I\'m trying to do it. Here\'s an example of how I spent yesterday morning, yoga on top of mount Talkeetna."'),
            ("user", 'Deborah said, "Nice job, Jolene! How long have you been doing yoga and meditation?"'),
        ],
        "question": "Which US state did Jolene visit during her internship?",
        "gold": "Alaska",
    },
    {
        "id": "c3_d19_2_console_gift",
        "category": "2",
        "anchor": "2023-08-19",
        "turns": [
            ("user", 'Deborah said, "Hey Jolene! Hope you\'re having a good one. Last Friday I told Anna the story of my life."'),
            ("assistant", 'Jolene said, "Life\'s been hella busy since we last talked. I bought a console for my partner as a gift on the 17th and it\'s so much fun!"'),
            ("user", 'Deborah said, "Well done! As for me, I\'ve been focusing on teaching yoga."'),
        ],
        "question": "When did Jolene gift her partner a new console?",
        "gold": "17 August, 2023",
    },
]
# fmt: on

# Per-provider HEAD baseline fix counts (correct_answers/12).  Calibrated
# 2026-05-07.  HEAD misses overlap heavily across providers (D1:6 mother,
# D1:8 pendant when+country, D2:24 seraphim when+country, D4:33 bogota
# country, D13:15 talkeetna state, D19:2 console — these are the genuine
# HEAD-failure cases that candidates must fix to register c3 improvement).
# When set to 0, that provider runs in calibration mode and the verdict is
# informational only.  Recalibrate with --include-component-3 on a fresh
# HEAD run when extraction model or prompt baseline changes.
COMPONENT3_BASELINES = {
    "anthropic": 6,   # 6/12 — Haiku gets the simpler cases right
    "openai": 1,      # 1/12 — gpt-5.4-mini struggles on HEAD here
    "local": 5,       # 5/12 — Qwen3.6-35B middle-ground
}


GENERATOR_PROMPT = (
    "You are answering a question from a memory system.\n\n"
    "Question: {question}\n\n"
    "Memory facts available:\n{facts}\n\n"
    "Answer concisely (a phrase or short sentence).  If the facts don't "
    "contain enough information, say \"I don't know.\""
)

JUDGE_PROMPT = (
    "Question: {question}\n"
    "Gold answer: {gold}\n"
    "Generated answer: {generated}\n\n"
    "Is the generated answer correct?  Be lenient on form (\"Tuesday\" "
    "matches \"last Tuesday\", \"5\" matches \"five\", \"in 2010\" matches "
    "\"2010\", \"electrical engineering project\" matches \"electricity "
    "engineering project\", etc.) and strict on substance.  Output exactly "
    "YES or NO with no other text."
)


def render_facts_for_generator(facts):
    """Render extracted facts as a bulleted string for the generator."""
    if not facts:
        return "(no facts extracted)"
    out = []
    for f in facts:
        text = f.get("text") or ""
        if text:
            out.append(f"- {text}")
    return "\n".join(out) if out else "(no facts extracted)"


def component3_mini_bench(prompt_template, caller_fn, model, endpoint, api_key,
                          baseline_fix_count, quiet=False):
    """For each conv-7 attribution-shape case, extract → generate → judge.
    Returns (pass, info).  Pass = fix_count > baseline_fix_count.  When
    baseline_fix_count == 0 (calibration mode) the run always reports PASS
    with the measured fix_count for the operator to record."""
    case_results = []
    for i, case in enumerate(COMPONENT3_CASES, 1):
        conv_json = build_conversation_json(case["turns"])
        ext_prompt = build_extraction_prompt(
            prompt_template, case["anchor"], conv_json, existing_profile="(none)")
        # 1. Extract
        try:
            ext_raw = caller_fn(model, "", ext_prompt, api_key, endpoint,
                                temperature=0.0, max_tokens=1024)
        except Exception as exc:
            case_results.append({**case, "stage": "extract", "error": str(exc),
                                 "correct": False})
            if not quiet:
                sys.stdout.write(f"\r  [{i}/{len(COMPONENT3_CASES)}] "
                                 f"{case['id']:<35} extract_ERR  ")
                sys.stdout.flush()
            continue
        parsed = parse_extraction_response(ext_raw)
        facts = parsed.get("facts") if isinstance(parsed, dict) else None
        facts_str = render_facts_for_generator(facts)
        # 2. Generate
        gen_user = GENERATOR_PROMPT.format(question=case["question"],
                                           facts=facts_str)
        try:
            gen_raw = caller_fn(model, "", gen_user, api_key, endpoint,
                                temperature=0.0, max_tokens=128)
        except Exception as exc:
            case_results.append({**case, "stage": "generate",
                                 "error": str(exc), "fact_count": len(facts or []),
                                 "correct": False})
            continue
        # 3. Judge
        judge_user = JUDGE_PROMPT.format(question=case["question"],
                                         gold=case["gold"],
                                         generated=gen_raw.strip())
        try:
            verdict_raw = caller_fn(model, "", judge_user, api_key, endpoint,
                                    temperature=0.0, max_tokens=8)
        except Exception as exc:
            case_results.append({**case, "stage": "judge",
                                 "error": str(exc),
                                 "fact_count": len(facts or []),
                                 "generated": gen_raw, "correct": False})
            continue
        verdict_clean = (verdict_raw or "").strip().upper()
        # Strip non-alpha first char clutter, accept YES anywhere at start.
        verdict_clean = re.sub(r"[^A-Z]", "", verdict_clean)
        correct = verdict_clean.startswith("YES")
        case_results.append({
            **case,
            "fact_count": len(facts or []),
            "generated": gen_raw.strip(),
            "verdict": verdict_clean,
            "correct": correct,
        })
        if not quiet:
            marker = "FIX" if correct else "MISS"
            sys.stdout.write(f"\r  [{i}/{len(COMPONENT3_CASES)}] "
                             f"{case['id']:<35} {marker:<5}")
            sys.stdout.flush()
    if not quiet:
        print()

    fix_count = sum(1 for r in case_results if r.get("correct"))
    fix_rate = fix_count / max(1, len(case_results))
    info = {
        "total_cases": len(case_results),
        "fix_count": fix_count,
        "fix_rate": round(fix_rate, 3),
        "baseline_fix_count": baseline_fix_count,
        "case_results": case_results,
    }
    if baseline_fix_count == 0:
        # Calibration mode: report measured fix_count, treat as PASS.
        return True, info
    return fix_count > baseline_fix_count, info


def build_conversation_json(turns):
    """Format turns as the JSON-array-of-{role,content} that production
    extraction sees in conversation_json (memory_extraction.c:1252)."""
    return json.dumps([{"role": r, "content": c} for r, c in turns])


def build_extraction_prompt(template, anchor_iso, conversation_json,
                            existing_profile="(none)"):
    """Apply the same %s formatting production uses (memory_extraction.c,
    the extraction prompt's snprintf): anchor_line, conversation_json,
    existing_profile, expiry_block.  anchor_line is empty when no anchor is
    known; here we always provide one because each test case has a date.
    expiry_block is empty, as in production while [memory] expire_enabled
    is off (the default)."""
    anchor_line = f"Conversation anchor: {anchor_iso}\n\n"
    return template % (anchor_line, conversation_json, existing_profile, "")


def parse_extraction_response(raw):
    """Parse the LLM's JSON response into the {facts, ...} dict.  Mirrors
    memory_extraction_parse_json() in C: tries direct parse, then a
    ```json fence, then bracket-fallback."""
    if not raw:
        return None
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        pass
    m = re.search(r"```(?:json)?\s*(.*?)```", raw, flags=re.DOTALL)
    if m:
        try:
            return json.loads(m.group(1))
        except json.JSONDecodeError:
            pass
    start = None
    for ch in ("{", "["):
        i = raw.find(ch)
        if i >= 0 and (start is None or i < start):
            start = i
    if start is not None:
        try:
            return json.loads(raw[start:])
        except json.JSONDecodeError:
            pass
    return None


def evaluate_attribution(facts, expected_subjects, forbidden_subjects):
    """Score a single test case's facts.

    Pass:
      - at least one fact contains some expected_subject keyword, AND
      - no fact contains a forbidden_subject keyword WITHOUT also
        containing an expected_subject keyword (mixed-mention facts are
        neutral, sole-forbidden facts fail).
    Returns (pass, expected_hits, forbidden_only_hits, neutral_hits)."""
    if not facts:
        return False, 0, 0, 0
    expected_hits = 0
    forbidden_only_hits = 0
    neutral_hits = 0
    exp_lc = [e.lower() for e in expected_subjects]
    forb_lc = [f.lower() for f in forbidden_subjects]
    for fact in facts:
        text = (fact.get("text") or "").lower()
        has_exp = any(e in text for e in exp_lc)
        has_forb = any(f in text for f in forb_lc) if forb_lc else False
        if has_exp:
            expected_hits += 1
        if has_forb and not has_exp:
            forbidden_only_hits += 1
        elif has_forb and has_exp:
            neutral_hits += 1
    passed = expected_hits >= 1 and forbidden_only_hits == 0
    return passed, expected_hits, forbidden_only_hits, neutral_hits


def component2_attribution(prompt_template, caller_fn, model, endpoint, api_key,
                           baseline_attribution_passed, baseline_known_good_passed,
                           quiet=False):
    """Run each test case through the live extraction prompt + provider call.
    Returns (pass, info).  `caller_fn` is one of the PROVIDER_CALLERS values."""
    case_results = []
    failures = []
    for i, case in enumerate(TEST_CASES, 1):
        conv_json = build_conversation_json(case["turns"])
        prompt = build_extraction_prompt(
            prompt_template, case["anchor"], conv_json, existing_profile="(none)")
        try:
            raw = caller_fn(model, "", prompt, api_key, endpoint,
                            temperature=0.0, max_tokens=1024)
        except Exception as exc:
            if not quiet:
                print(f"  [{i}] {case['id']}: API error: {exc}", file=sys.stderr)
            case_results.append({**case, "raw": "", "fact_texts": [],
                                 "passed": False, "reason": f"api_error: {exc}"})
            failures.append(case["id"])
            continue
        parsed = parse_extraction_response(raw)
        facts = parsed.get("facts") if isinstance(parsed, dict) else None
        if facts is None:
            case_results.append({**case, "raw": raw, "fact_texts": [],
                                 "passed": False, "reason": "parse_failed"})
            failures.append(case["id"])
            if not quiet:
                sys.stdout.write(
                    f"\r  [{i}/{len(TEST_CASES)}] {case['id']:<35} parse_FAIL  ")
                sys.stdout.flush()
            continue
        passed, eh, fh, nh = evaluate_attribution(
            facts, case["expected_subjects"], case["forbidden_subjects"])
        case_results.append({
            **case, "raw": raw,
            "fact_texts": [f.get("text", "") for f in facts],
            "expected_hits": eh, "forbidden_only_hits": fh, "neutral_hits": nh,
            "passed": passed,
            "reason": "ok" if passed else f"exp={eh} forb_only={fh}",
        })
        if not passed:
            failures.append(case["id"])
        if not quiet:
            marker = "PASS" if passed else "FAIL"
            sys.stdout.write(
                f"\r  [{i}/{len(TEST_CASES)}] {case['id']:<35} {marker:<6}")
            sys.stdout.flush()
    if not quiet:
        print()

    n_total = len(case_results)
    n_passed = sum(1 for r in case_results if r["passed"])
    n_known_good = sum(1 for r in case_results if r["kind"] == "known_good")
    n_known_good_passed = sum(
        1 for r in case_results if r["kind"] == "known_good" and r["passed"])
    n_attr = sum(1 for r in case_results if r["kind"] == "attribution_failure")
    n_attr_passed = sum(
        1 for r in case_results if r["kind"] == "attribution_failure" and r["passed"])

    info = {
        "total_cases": n_total,
        "total_passed": n_passed,
        "pass_rate": round(n_passed / n_total, 3) if n_total else 0.0,
        "known_good_passed": n_known_good_passed,
        "known_good_total": n_known_good,
        "attribution_passed": n_attr_passed,
        "attribution_total": n_attr,
        "failures": failures,
        "case_results": case_results,
    }

    # Threshold rules (per-provider baselines + small regression tolerance):
    #  - known-good drop > KNOWN_GOOD_REGRESS_TOLERANCE → FAIL
    #    (positive-control regression; even one beyond baseline matters)
    #  - attribution drop > ATTRIBUTION_REGRESS_TOLERANCE → FAIL
    #    (real regression on the attribution-failure suite)
    #
    # Calibration mode: if a baseline is 0, treat the run as PASS — it's
    # measuring the new baseline, not gating against the old one.
    if baseline_known_good_passed == 0 and baseline_attribution_passed == 0:
        return True, info
    if (baseline_known_good_passed > 0
            and n_known_good_passed
                < baseline_known_good_passed - KNOWN_GOOD_REGRESS_TOLERANCE):
        return False, info
    if (baseline_attribution_passed > 0
            and n_attr_passed
                < baseline_attribution_passed - ATTRIBUTION_REGRESS_TOLERANCE):
        return False, info
    return True, info


# =============================================================================
# Helpers
# =============================================================================

# load_secrets / resolve_provider_config / read_local_* all live in
# multi_model_probe.py; imported above.  This section used to carry a
# duplicate `load_secrets`; removed during 1i.A.


# =============================================================================
# Main
# =============================================================================

def run_single_provider(provider, model_override, endpoint_override, secrets,
                        dawn_toml_path, prompt_template, include_c3=False,
                        quiet=False):
    """Run all 25 cases through one provider and compute both components.
    When include_c3 is true, also run the conv-7 attribution-shape mini-bench
    (component 3).  Returns dict with provider config used + per-component
    results."""
    caller_fn, model, endpoint, api_key = resolve_provider_config(
        provider, secrets, dawn_toml_path,
        override_model=model_override, override_endpoint=endpoint_override)
    cfg = PROVIDERS[provider]

    if not quiet:
        print(f"  Provider: {provider} model={model or '(default)'} "
              f"endpoint={endpoint}")
        print(f"  Baseline: {cfg['baseline_fact_count']} facts, "
              f"attr {cfg['baseline_attribution_passed']}/20, "
              f"kg {cfg['baseline_known_good_passed']}/5"
              + (f", c3 fix {COMPONENT3_BASELINES.get(provider, 0)}/12"
                 if include_c3 else ""))

    c2_pass, c2_info = component2_attribution(
        prompt_template, caller_fn, model, endpoint, api_key,
        cfg["baseline_attribution_passed"],
        cfg["baseline_known_good_passed"], quiet=quiet)
    c1_pass, c1_info = component1_fact_count(
        c2_info["case_results"], cfg["baseline_fact_count"])

    result = {
        "provider": provider,
        "model": model,
        "endpoint": endpoint,
        "component_1": {"passed": c1_pass, **c1_info},
        "component_2": {
            "passed": c2_pass,
            "total_passed": c2_info["total_passed"],
            "total_cases": c2_info["total_cases"],
            "pass_rate": c2_info["pass_rate"],
            "known_good_passed": c2_info["known_good_passed"],
            "known_good_total": c2_info["known_good_total"],
            "attribution_passed": c2_info["attribution_passed"],
            "attribution_total": c2_info["attribution_total"],
            "failures": c2_info["failures"],
            "case_results": c2_info["case_results"],
        },
    }

    if include_c3:
        if not quiet:
            print(f"  --- component 3 mini-bench (12 cases × 3 calls) ---")
        c3_pass, c3_info = component3_mini_bench(
            prompt_template, caller_fn, model, endpoint, api_key,
            COMPONENT3_BASELINES.get(provider, 0), quiet=quiet)
        result["component_3"] = {"passed": c3_pass, **c3_info}

    return result


def print_provider_block(result):
    p = result["provider"]
    c1 = result["component_1"]
    c2 = result["component_2"]
    print(f"--- {p}: component 1 fact-count — "
          f"{'PASS' if c1['passed'] else 'FAIL'} ---")
    print(f"  Baseline:  {c1['baseline_fact_count']} facts "
          f"({c1['n_cases']} cases)")
    print(f"  Candidate: {c1['candidate_fact_count']} facts  "
          f"(threshold <= {c1['threshold_fact_count']})  "
          f"delta {c1['delta_pct']:+.1f}%")
    print(f"--- {p}: component 2 attribution — "
          f"{'PASS' if c2['passed'] else 'FAIL'} ---")
    print(f"  Total:       {c2['total_passed']}/{c2['total_cases']}  "
          f"({100 * c2['pass_rate']:.1f}%)")
    print(f"  Known-good:  {c2['known_good_passed']}/{c2['known_good_total']}")
    print(f"  Attribution: {c2['attribution_passed']}/{c2['attribution_total']}")
    if c2["failures"]:
        print(f"  Failures: {', '.join(c2['failures'][:10])}"
              + (" ..." if len(c2['failures']) > 10 else ""))
    if "component_3" in result:
        c3 = result["component_3"]
        print(f"--- {p}: component 3 fix-rate — "
              f"{'PASS' if c3['passed'] else 'FAIL'} ---")
        print(f"  Baseline:  {c3.get('baseline_fix_count', 0)}/12 "
              f"(0 = calibration mode)")
        print(f"  Candidate: {c3['fix_count']}/{c3['total_cases']}  "
              f"({100 * c3['fix_rate']:.1f}%)")
        misses = [r["id"] for r in c3.get("case_results", [])
                  if not r.get("correct")]
        if misses:
            print(f"  Misses: {', '.join(misses[:8])}"
                  + (" ..." if len(misses) > 8 else ""))
    print()


def print_aggregate_matrix(results):
    """Render a per-provider component matrix + aggregate verdict.  Includes
    component 3 column iff any provider has a c3 result."""
    has_c3 = any("component_3" in r for r in results)
    print(f"=== Aggregate matrix (quorum {AGGREGATE_QUORUM}-of-{len(results)}) ===")
    if has_c3:
        print(f"{'Provider':<12} {'C1 fact-count':>16} "
              f"{'C2 attribution':>18} {'C3 fix-rate':>14} {'Overall':>10}")
    else:
        print(f"{'Provider':<12} {'C1 fact-count':>16} "
              f"{'C2 attribution':>18} {'Overall':>10}")
    print("-" * (74 if has_c3 else 60))
    c1_passed = 0
    c2_passed = 0
    c3_passed = 0
    c3_total = 0
    for r in results:
        c1 = r["component_1"]
        c2 = r["component_2"]
        c1_mark = "PASS" if c1["passed"] else "FAIL"
        c2_mark = "PASS" if c2["passed"] else "FAIL"
        c1_detail = (f"{c1['candidate_fact_count']:>3} "
                     f"({c1['delta_pct']:+5.1f}%)")
        c2_detail = (f"{c2['total_passed']:>2}/{c2['total_cases']:>2} "
                     f"kg={c2['known_good_passed']}/{c2['known_good_total']}")
        if c1["passed"]:
            c1_passed += 1
        if c2["passed"]:
            c2_passed += 1
        if "component_3" in r:
            c3 = r["component_3"]
            c3_mark = "PASS" if c3["passed"] else "FAIL"
            c3_detail = (f"{c3['fix_count']:>2}/{c3['total_cases']:>2} "
                         f"vs {c3.get('baseline_fix_count', 0)}")
            c3_total += 1
            if c3["passed"]:
                c3_passed += 1
        else:
            c3_mark = "—"
            c3_detail = "—"
        components_pass = c1["passed"] and c2["passed"] and (
            "component_3" not in r or r["component_3"]["passed"])
        overall = "PASS" if components_pass else "FAIL"
        if has_c3:
            print(f"{r['provider']:<12} {c1_mark:>5} {c1_detail:>10} "
                  f"{c2_mark:>5} {c2_detail:>12} {c3_mark:>5} {c3_detail:>8} "
                  f"{overall:>10}")
        else:
            print(f"{r['provider']:<12} {c1_mark:>5} {c1_detail:>10} "
                  f"{c2_mark:>5} {c2_detail:>12} {overall:>10}")
    print("-" * (74 if has_c3 else 60))
    aggregate_c1 = c1_passed >= AGGREGATE_QUORUM
    aggregate_c2 = c2_passed >= AGGREGATE_QUORUM
    aggregate_c3 = (c3_passed >= AGGREGATE_QUORUM) if c3_total else True
    aggregate_overall = aggregate_c1 and aggregate_c2 and aggregate_c3
    if has_c3:
        print(f"{'aggregate':<12} {'PASS' if aggregate_c1 else 'FAIL':>5}  "
              f"{c1_passed}/{len(results)}        "
              f"{'PASS' if aggregate_c2 else 'FAIL':>5}  {c2_passed}/{len(results)}     "
              f"{'PASS' if aggregate_c3 else 'FAIL':>5}  {c3_passed}/{c3_total}    "
              f"{'PASS' if aggregate_overall else 'FAIL':>10}")
    else:
        print(f"{'aggregate':<12} {'PASS' if aggregate_c1 else 'FAIL':>5}  "
              f"{c1_passed}/{len(results)}        "
              f"{'PASS' if aggregate_c2 else 'FAIL':>5}  {c2_passed}/{len(results)}     "
              f"{'PASS' if aggregate_overall else 'FAIL':>10}")
    print()
    return aggregate_overall, aggregate_c1, aggregate_c2, aggregate_c3


def main():
    parser = argparse.ArgumentParser(
        description="Cheap speaker-attribution probe (multi-model).",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument("--provider",
                        help="Single provider: anthropic | openai | local. "
                             "Default: run all 3 and aggregate.")
    parser.add_argument("--model",
                        help="Override the provider default model")
    parser.add_argument("--endpoint",
                        help="Override the provider endpoint URL")
    parser.add_argument("--secrets-path", type=Path, default=DEFAULT_SECRETS_PATH,
                        help="Path to secrets.toml")
    parser.add_argument("--dawn-toml", type=Path, default=DEFAULT_DAWN_TOML,
                        help="Path to dawn.toml (used for [llm.local] endpoint)")
    parser.add_argument("--extraction-c", type=Path, default=EXTRACTION_C_PATH,
                        help="Path to memory_extraction.c (extracts live prompt)")
    parser.add_argument("--json-output", type=Path,
                        help="Optional path to dump per-provider per-case results")
    parser.add_argument("--skip-local", action="store_true",
                        help="Skip the local provider (use when local LLM is offline)")
    parser.add_argument("--include-component-3", action="store_true",
                        help="Also run the conv-7 attribution-shape mini-bench "
                             "(component 3).  Adds 12 extract+generate+judge "
                             "cycles per provider; ~$0.10 extra per multi-model run.")
    args = parser.parse_args()

    print(f"=== Speaker-attribution probe ===")
    print(f"  EXTRACTION_PROMPT_TEMPLATE source: {args.extraction_c}")

    prompt_template = extract_prompt_template_from_c(args.extraction_c)
    if prompt_template.count("%s") < 3:
        sys.exit("error: extracted prompt has fewer than 3 %s placeholders — "
                 "regex parse may have lost fragments")

    n_attr = sum(1 for c in TEST_CASES if c["kind"] == "attribution_failure")
    n_good = sum(1 for c in TEST_CASES if c["kind"] == "known_good")
    print(f"  Prompt template: {len(prompt_template)} chars, "
          f"{prompt_template.count('%s')} %s placeholders")
    print(f"  Test cases: {len(TEST_CASES)} ({n_attr} attribution-failure + "
          f"{n_good} known-good)")
    print()

    secrets = load_secrets(args.secrets_path)

    if args.provider:
        providers = [args.provider]
    else:
        providers = ["anthropic", "openai"]
        if not args.skip_local:
            providers.append("local")

    results = []
    for p in providers:
        try:
            r = run_single_provider(
                p, args.model, args.endpoint, secrets, args.dawn_toml,
                prompt_template, include_c3=args.include_component_3,
                quiet=False)
        except SystemExit:
            raise
        except Exception as exc:
            print(f"  {p}: setup/run error: {exc}", file=sys.stderr)
            results.append({
                "provider": p, "model": "(error)", "endpoint": "(error)",
                "component_1": {"passed": False, "baseline_fact_count": 0,
                                "candidate_fact_count": 0,
                                "threshold_fact_count": 0, "delta_pct": 0.0,
                                "n_cases": 0, "error": str(exc)},
                "component_2": {"passed": False, "total_passed": 0, "total_cases": 0,
                                "pass_rate": 0.0,
                                "known_good_passed": 0, "known_good_total": 0,
                                "attribution_passed": 0, "attribution_total": 0,
                                "failures": [], "case_results": [],
                                "error": str(exc)},
            })
            continue
        results.append(r)
        print_provider_block(r)

    aggregate_overall, agg_c1, agg_c2, agg_c3 = print_aggregate_matrix(results)

    if args.json_output:
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
        try:
            fd = os.open(args.json_output, flags, 0o600)
        except FileExistsError:
            sys.exit(f"error: --json-output path {args.json_output} already exists")
        with os.fdopen(fd, "w") as f:
            json.dump({
                "providers": results,
                "aggregate": {
                    "overall_passed": aggregate_overall,
                    "component_1_quorum": agg_c1,
                    "component_2_quorum": agg_c2,
                    "component_3_quorum": agg_c3,
                    "quorum_required": AGGREGATE_QUORUM,
                    "providers_run": len(results),
                },
            }, f, indent=2, default=str)
        print(f"  Saved results to {args.json_output}")

    if len(results) == 1:
        # Single-provider mode: report that provider's verdict only.
        only = results[0]
        single_pass = (only["component_1"]["passed"]
                       and only["component_2"]["passed"]
                       and (only.get("component_3", {}).get("passed", True)))
        print(f"=== Single-provider verdict ({only['provider']}): "
              f"{'PASS' if single_pass else 'FAIL'} ===")
        return 0 if single_pass else 1

    print(f"=== Aggregate verdict: "
          f"{'PASS' if aggregate_overall else 'FAIL'} ===")
    return 0 if aggregate_overall else 1


if __name__ == "__main__":
    sys.exit(main())
