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

- No automated driver and no gold-answer grading — deliberate for the skeleton.
  A WS/text driver that submits the briefs, and an LLM-judge grader against
  `should_cover`, are natural P1 additions.
- `capture_baseline.py` reports the latest report **revision** (the durable audit
  copy). The user-facing report also lands as a note (`report_doc_id`); the
  revision markdown is identical at P0.
