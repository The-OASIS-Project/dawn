# Deep-research smoke + baseline capture

The P0 eval **skeleton** for the `deep_research` feature (DEEP_RESEARCH_DESIGN.md
§15 Step 11). P0 is where the round **digest** and the **synthesis** are written,
and *compression losing a load-bearing fact* is the named mediocrity risk (§14) —
so this exists to let a human eyeball report quality now and diff it as the prompts
evolve. It is **not** an automated pass/fail: a research run needs the live daemon +
an LLM + web search, and "is this report good?" is a judgment call, not a metric.

## What's here

- **`smoke_queries.json`** — 8 briefs chosen to be BOTH genuine research (current /
  aggregated / niche material a capable model can't one-shot from training, so the
  multi-round loop actually engages) AND useful to the developer: each maps to a
  real DAWN subsystem decision (which local LLM / embedding / TTS / ASR to run on
  Jetson, AR-glasses HUD hardware, cellular modem, the self-hosted-assistant
  landscape). Evergreen textbook topics were removed — a strong model answers them
  in one search and never spawns a run. Each brief has a `should_cover` checklist
  (what a *good* report contains, for eyeballing — not a machine-graded gold answer).
- **`capture_baseline.py`** — a READ-ONLY snapshot of runs that already executed,
  pulled straight from `auth.db`: budgets (rounds / tool calls / input tokens /
  wall-clock), coverage counts, claim + distinct-source counts, and the rendered
  report. Never mutates the DB.
- **`run_benchmark.py`** — the **unattended driver** (§16). Drives runs headlessly
  through the `dawn-admin research` verb: start → poll `research status` to a
  terminal state (timeout → cancel) → extract the report + per-claim evidence from
  `auth.db` (read-only) → write `<out>/<task_id>.json`. Resumable (skips existing
  artifacts). `--extract-only <run_id>` re-extracts an existing run with no spawn.
- **`score_exact_match.py`** — deterministic short-answer scorer (BrowseComp/GAIA
  floor): checks whether a task's `gold` string surfaced in the report/claims. No
  judge/API key.
- **`score_deepresearch_bench.py`** — RACE/FACT adapter: deterministic citation +
  coverage stats (the structural health FACT builds on — DAWN's claims already
  carry `source_url` + a verbatim `quote`), and writes per-artifact judge-input
  files for the external LLM-judge step. Also does the two format conversions the
  **official** DeepResearch-Bench harness needs: `--query <query.jsonl>` turns their
  task file into our task set, and `--dr-bench <model>` turns our artifacts into
  their `raw_data/<model>.jsonl`.
- **`tasks/`** — task sets for the driver (same `{queries:[{id,brief,…}]}` shape as
  `smoke_queries.json`; add `gold` for exact-match, `reference` for RACE).

## Running the smoke

1. **Enable the feature.** It ships OFF (`[research] enabled = false`). Set
   `[research] enabled = true` in `dawn.toml` (or the WebUI **Deep Research**
   settings panel) and make sure web `search` + `url_fetch` are configured.
2. **Submit each brief conversationally.** There is no automated driver in P0 — the
   tool is confirmation-gated and LLM-mediated by design. For each entry in
   `smoke_queries.json`, tell Friday (WebUI text, voice, or a chat channel)
   something like:

   > Do a deep-research run on: *«brief»*. Show me the plan and cost, then go ahead.

   She'll propose (plan + cost envelope), you confirm, and the run starts in the
   background. Repeat for all 8. They run concurrently under the `[jobs]` caps; let
   them finish (watch the sidebar / `deep_research status`).
3. **Capture the baseline:**

   ```bash
   ./capture_baseline.py --md baseline.md --out baseline.json
   ```

   `baseline.md` has the full rendered reports to read; `baseline.json` has the
   metrics to diff later. (Both are run outputs — keep them out of git, like the
   other `benchmarks/bench_*.json`.)

## Automated benchmark run (the driver)

Instead of submitting each brief by hand, drive them headlessly. The driver spawns
runs over the SO_PEERCRED-gated admin socket, bypassing the conversational
confirm — the operator is the authorization (§16.4). Runs are **web-only**.

1. **Enable the feature** (as above) and make sure the daemon is running the build
   with the `research` admin verb.
2. **Use a dedicated eval account**, not the primary user — a benchmark brief is
   third-party text, so its conversation/report shouldn't land in your own space.
   `--user` is required and validated against a real account.
3. **Run the driver:**

   ```bash
   # from the repo root:
   benchmarks/research/run_benchmark.py \
       --tasks benchmarks/research/smoke_queries.json --user <eval_id> \
       --admin ./build-debug/dawn-admin/dawn-admin \
       --out benchmarks/research/results --timeout 3600
   ```

   Each task: `dawn-admin research start` → poll → extract → `results/<id>.json`
   (report + claims + coverage). Local-model runs are slow (tens of minutes each);
   set `--timeout` above the expected wall-clock so a healthy run isn't cancelled.
   Run **local-first** (free-but-slow) for a baseline; a cloud pass on a subset is
   optional for a headline.

   Every artifact is stamped with a `run_config` block — the code revision
   (`git`, `-dirty` if the tree has uncommitted prompt edits), the daemon's active
   model, and the `[research]` budgets — read once per batch from `dawn.toml`
   (override with `--config`, and add a human label with `--prompt-tag`). This is
   what keeps a score honest: it says *which* prompt/model/budgets produced it. So a
   B2 prompt-tuning score is never a naked number.
4. **Score offline** (never re-runs research):

   ```bash
   ./score_deepresearch_bench.py results/ --judge-out results/judge_inputs   # stats + judge prep
   ./score_exact_match.py results/                                           # if tasks carry `gold`
   ```

   The LLM-judge (RACE quality + FACT citation-support) needs a judge model + key
   and the DeepResearch Bench prompts
   ([repo](https://github.com/Ayanami0730/deep_research_bench)); point it at the
   `judge_inputs/` files. **Report the stop-reason distribution alongside the
   score** — a good score reached mostly on `token_budget` fuses is a worse result
   than the same score on `coverage`/`concluded` (controller health is half of what
   the eval measures), and pin the `[research]` budgets/model the runs used.

**Manual `dawn-admin` use** (spot checks): `research start --user N --brief "…"` or
`--brief-file <path>`; `research status --user N <run_id>`; `research cancel --user
N <run_id>`.

## Running against the official DeepResearch-Bench

DAWN is the *harness*; a run is (model × harness), so the score is system-level —
hold a strong model constant (e.g. Sonnet 5 in `dawn.toml`) to judge the harness.
DeepResearch-Bench scores exactly the reports you submit, so a **subset run is
valid for iteration** (it is NOT leaderboard-comparable — a different task mix — but
it is the right signal for tuning). Clone the bench
([repo](https://github.com/Ayanami0730/deep_research_bench)); its tasks live in
`data/prompt_data/query.jsonl` (100 `{id, prompt}` lines).

```bash
cd benchmarks/research

# 1. their query.jsonl -> our task set (ids preserved, so RACE aligns to references).
#    Slice to a subset first for a cheap iteration pass.
./score_deepresearch_bench.py --query <bench>/data/prompt_data/query.jsonl \
    --tasks-out dr_tasks.json

# 2. run them through DAWN (dedicated eval user; long timeout for cloud too).
./run_benchmark.py --tasks dr_tasks.json --user <eval_id> \
    --admin ../../build-debug/dawn-admin/dawn-admin \
    --out results/dr --timeout 3600 --prompt-tag sonnet5-baseline

# 3. our artifacts -> their raw_data line file, + the deterministic stats table.
./score_deepresearch_bench.py results/dr --dr-bench dawn-sonnet-5 --raw-out raw_data

# 4. drop raw_data/dawn-sonnet-5.jsonl into <bench>/data/test_data/raw_data/ and run
#    THEIR judge (needs OPENROUTER_API_KEY + JINA_API_KEY):
#      TARGET_MODELS=("dawn-sonnet-5") bash run_benchmark.sh
```

Report the **stop-reason distribution and the pinned `run_config`** next to the
score — a number reached mostly on `token_budget` fuses is a worse result than the
same number on `coverage`/`concluded` (controller health is half of what the eval
measures).

## What to look at (the quality dimensions)

The metrics are a proxy; the **report text is the real signal**. Read each report
against its `should_cover` list and watch for:

- **Compression loss (§14, the big one):** does a claim that got recorded (it's in
  `research_claims`, so `claims.count` > the report's bullet count is the tell)
  fail to appear in the synthesized report? The report is a view over the claims,
  so nothing recorded should silently vanish.
- **Synthesis coherence:** is the report organized by sub-question and readable, or
  a flat dump? Do citations point at real sources?
- **Coverage honesty:** `questions.answered / total`. A run that stopped on
  `coverage` should have most questions answered; one that stopped on `budget`
  with few answered is under-resourced (bump `[research]` budgets) or the
  controller/plan is weak.
- **Multi-perspective briefs:** does it preserve disagreement, or flatten it into
  one confident answer?
- **Cost sanity:** `input_tokens` / `tool_calls` per run vs the budgets — is it
  spending its budget productively or spinning?

## Notes / limits (P0)

- The **automated driver** (`run_benchmark.py`) + scorers ship the "natural P1
  addition" the skeleton anticipated. The conversational path (`capture_baseline.py`
  + hand-submitted briefs) still works and is fine for a quick one-off; the driver
  is for running a *set* unattended. The RACE/FACT LLM-judge itself still needs an
  external judge model + key (the adapter preps its inputs).
- `capture_baseline.py` reports the latest report **revision** (the durable audit
  copy). The user-facing report also lands as a note (`report_doc_id`); the
  revision markdown is identical at P0.
