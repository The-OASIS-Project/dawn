# dawn_ab — daemon-driven multi-turn A/B

Measures how changes to DAWN's **request construction** (prompt layout, per-turn context placement,
thinking replay, caching) affect answer quality, tool use, memory citation and cache coverage.

`llm_testing/` calls provider APIs directly with its own prompt, so it cannot see changes to DAWN's
request path. This harness sends every turn **through the running daemon** (WebUI WebSocket), so
the request is built by the real code under test.

## Files

| File | Role |
|---|---|
| `scenarios.json` | Scripted multi-turn conversations. Each turn has `expect_tools`, a judge `rubric`, and optional `needs_date` / `expect_citation` / `forbid_tools` / `expect_answer` (regexes that must all match the answer: a ground-truth check graded without the judge) / `switch_to_alternate` (switch the conversation's model before this turn, to the first `--alternates` entry that isn't the model in use). A scenario marked `opt_in` runs only when named in `--only`. |
| `memory_fixture.json` | Fixed, fictional memory set loaded into the eval account before a run. |
| `run_ab.py` | Drives the conversations; writes one JSON artifact per (model, scenario). |
| `grade_ab.py` | Offline grading (tool check, LLM judge, citation audit, cache lines) and run-vs-run comparison. |

## Setup (once)

A **dedicated, non-admin eval account** on the daemon (e.g. `benchmark`). `--reseed-memory` wipes that
account's memory and loads the fixture; it refuses an admin account, and an account holding memory
that didn't come from an import. Never point it at a real user.

## Running

```bash
cd benchmarks/dawn_ab
# credentials: env DAWN_AB_USER / DAWN_AB_PASS, or ~/.config/dawn/dawn_ab.env
# (same KEY=VALUE lines, chmod 600) -- never passed on argv

# Baseline on current code
./run_ab.py --reseed-memory --label "baseline $(git rev-parse --short HEAD)" \
    --models claude:claude-opus-5-5,claude:claude-sonnet-5 --out results/baseline
./grade_ab.py results/baseline

# After the change (same scenarios, same fixture, same judge)
./run_ab.py --reseed-memory --label "after" --out results/after
./grade_ab.py results/after
./grade_ab.py --compare results/baseline results/after
```

Useful flags: `--only memory_recall,tool_followup`, `--repeat 3` (LLM variance), `--thinking-mode`,
`--effort`, `--alternates` (models a `switch_to_alternate` turn switches to), `--no-judge`
(deterministic checks only, no API spend).

`large_page_view` (opt-in) checks large tool results: a pinned Wikipedia revision fetched with
`url_fetch`, with the answer in the middle a view leaves out, so the model has to read more of the
stored result (`result_read`) or say it couldn't find it. It needs web access, and a view only fires on
a model with a window of about 200K tokens or less (the page is cut at 24,000 characters), so run it as
`--only large_page_view --models claude:claude-sonnet-4-5`. Its `expect_answer` grades the numbers
deterministically; `tool result views` and `result_read calls` show whether the view path ran.

`long_single_topic` is one topic over 12 turns, with the same remembered facts relevant early, in the
middle and late, and a model switch at turn 10. It measures what remembered context costs per turn
over a long conversation, and whether answers still use a fact that was shown many turns back.

## What each metric means

- **tool check %** — every `expect_tools` tool was called in the turn, and no `forbid_tools` tool.
- **answer check %** — on turns with `expect_answer`, every pattern matched the answer (no API spend).
- **tool result views / result_read calls** — views the daemon made of large tool results, and reads
  of stored results, from the daemon-log lines written during each turn.
- **first batch tokens (med)** — on turns that called tools, how many tokens the first tool batch
  added to the request (the first tool iteration's prompt minus the call's before it), median over
  those turns. The cost a view saves; any provider.
- **judge pass %** — a fixed grader model (`--judge-model`, default `claude-sonnet-5`) passes the
  answer against the turn rubric. Verdicts are cached in `<run>/judge_cache.json`. Compare runs only
  when both used the same judge model.
- **recall: injected / cited %** — on `expect_citation` turns, from `memory_citation_audit`: was
  memory injected, and did the answer cite it. **cite-rate %** is cited ÷ injected over all turns.
- **memory available %** — on `expect_citation` turns, memory was either sent with the turn or
  named as still relevant from earlier in the conversation.
- **cite-rate: new / referenced items %** — cited ÷ offered, separately for items sent with the turn
  and items the turn named as already in context (`referenced_ids`; daemons that re-send every item
  each turn have none, so everything counts as new there).
- **context chars / turn** — the size of the per-turn context rows the daemon saved (turn context
  plus memory), averaged over all turns and over turns 3 and later. On a long single-topic
  conversation it shows whether items already in context are sent again.
- **dropped citations** — cited ordinals the daemon rejected as invalid. It should stay 0.
- **cache read / write / uncached % prompt** — Anthropic calls parsed from daemon-log lines written
  during each turn. **Best-effort:** other traffic on the same daemon interleaves into those lines.
  Run on a quiet daemon when these numbers matter. **This is not a measure of cache coverage in
  production.** The scenarios are a few turns long, so the prompt is almost entirely the tools and
  system prefix, which is cached anyway. The uncached share that matters is on long sessions and long
  tool loops, and this harness doesn't produce those. Read this line as a regression check only.

## Caveats

- Conversations are **private**, so no memory extraction happens and runs don't change the account's
  memory. The fixture is reloaded on every `--reseed-memory`.
- Imported memories are embedded in the background right after `--reseed-memory`; the run waits
  (up to two minutes) until they all are, and `memory reseeded: N facts (M embedded)` says if it
  gave up early. Runs from before 2026-09-26 had no embedded imports (fixture recall was
  keyword-only), and runs before 2026-10-01 could start before embedding finished, so compare
  them only with runs from that time.
- Weather turns depend on live data. The rubric checks grounding, not specific values.
- LLM output varies. Use `--repeat` before reading much into a one-turn difference.
