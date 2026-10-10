# LLM quality suite v2

Ranks chat models on DAWN's own tasks. It replays requests **captured from the
daemon**, runs DAWN's tool loop against stateful mocks, and scores **outcomes**:
the action taken, the answer given, the voice it was given in, how reliably,
how fast and at what cost. The design and its reasoning are in
`docs/LLM_QUALITY_SUITE_DESIGN.md`.

The old suite (`llm_testing/scripts/test_llm_quality_native.py`) stays as a quick
smoke test that a model calls DAWN's tools at all. Use this one to choose a model.

## How it works

- **Production-faithful requests.** The daemon writes the exact requests one
  user's turns send (`dawn-admin llm capture`). The suite changes only:
  - the question
  - the time line in the turn context
  - `stream: true`

  The system prompt, every tool DAWN advertises, the sampling, the thinking
  settings and the cache breakpoints all go out as captured. A capture is per
  model, because DAWN's request depends on the model.
- **Multi-turn, stateful mocks** (`mocks.py`).
  - Tool calls change a small world: calendar, inbox, reminders, playback,
    devices, memory.
  - A model may take any valid path. For example, looking up the date and then
    creating a reminder, or creating it directly from the turn's time line.
  - Prepare/confirm tools mint a pending id and act only on the user's yes in a
    later turn, as in DAWN.
- **Outcome checks** (`score.py`): world state, tool arguments, required and
  forbidden tools, answer facts, speakable output, confirm order.
- **A voice judge** (`judge.py`).
  - A pinned model from a third family scores concise, persona and clarifies.
  - Each criterion grades the reply against the instruction its own request
    gave, quoted from the capture: the reply-length rule, the rule on unclear
    requests, the persona text, and on a spoken surface the spoken direction.
    A run whose request said nothing on a criterion isn't graded on it.
  - It never learns which model wrote the reply.
  - It is validated against human labels (Cohen's κ ≥ 0.6 per criterion).
- **Honest statistics** (`report.py`).
  - Run errors (a request that still fails after retries) are counted apart and
    never scored as quality.
  - Case-level bootstrap confidence intervals.
  - pass^k: the share of cases that pass in every run.
  - Time to first token p50/p90: from the start of a turn to the first visible
    answer text, the same measure for every provider.
  - Cost per turn from measured usage at OpenRouter list prices.
  - A decision rule fixed in advance (below).
- **A frozen clock.** Every run happens at the same local time (default Tue
  2026-10-13 09:30 America/Los_Angeles). Cases name dates symbolically
  (`{tomorrow}`), so results reproduce.

## 1. Capture (once per model set, and again when the prompt changes)

You need a daemon built from this branch, and an admin login.

```bash
# Once: the fixture user (non-admin, no MCP grants, no memories), created in the
# WebUI's user admin.

# Capture: sets the fixture's profile, arms the daemon (admin password), then
# drives the standard conversations as the fixture: 1-turn and 2-turn, text and
# spoken, per model.  With a persona file the fixture uses it in replace mode.
scripts/quality_capture_all.sh llm_testing/quality/captures
scripts/quality_capture_all.sh llm_testing/quality/captures my-persona.txt
#   QUALITY_MODELS="openai:gpt-5.6-luna claude:claude-haiku-5-5" to choose models:
#   provider:model for claude, openai, gemini or openrouter (vendor/model);
#   local:<model> for the local llama.cpp/Ollama server; add @effort (e.g.
#   claude:claude-haiku-5-5@medium) to capture that model with reasoning on.
#   Models with and without @effort go in separate runs.
```

Run the capture as the daemon's own user, because the daemon writes only into a
directory that user owns. An arming stops on its own after 30 minutes; to stop it
early, run `dawn-admin llm capture --stop`.

Captures hold the fixture's fictional profile and DAWN's prompt and tool list,
and no credentials (headers and URL credentials are redacted). Install-specific
text such as room names or HUD tool descriptions can appear; that is fine to
keep.

## 2. Run

```bash
python3 -m llm_testing.quality run --captures llm_testing/quality/captures/capture-XXXX \
    --runs 3 --out results.json
python3 -m llm_testing.quality run ... --model claude-haiku-5-5 --only confirm,multi_step
```

Run a local model with `--workers 1`: parallel requests on one llama.cpp slot evict each
other's cached prompt, and time to first token would measure that instead of the model.

Rate limits, overloads and dropped connections are retried with backoff and are
never scored as failures. A run that still fails shows in the `err` column. A
stream that sends nothing for 60 s, or runs past 180 s, is retried too, and a
request gives up after 7 minutes of attempts, so one stuck call can't stall the
run. Results print as each case finishes.

To compare prompt wording without a new capture, `--system-patch FILE` replaces
text in every captured system prompt: a JSON list of `[old, new]` pairs, each
old text found exactly once or the run stops. The judge grades a patched run
against the patched instructions. Confirm a wording you adopt with a real
capture before trusting it.

## 3. Judge (the voice rubric)

```bash
python3 -m llm_testing.quality judge results.json            # google/gemini-3.1-pro-preview
python3 -m llm_testing.quality label results.json             # you score a stratified sample
python3 -m llm_testing.quality validate                       # kappa per criterion, with CI
```

Labels go to `labels_v<rubric>.json`; a new rubric version starts a new file.
Score each reply against the instruction printed with it: 2 follows it fully,
1 partly, 0 not at all.

Validation must reach κ ≥ 0.6 per criterion before `voice` scores count. The
labeling sample is stratified across the judge's scores, so the human labels
include failures; a judge that always says "pass" can't meet the threshold.

## 4. Compare and decide

```bash
python3 -m llm_testing.quality compare results.json --a gpt-5.6-luna --b claude-haiku-5-5
```

**Decision rule (fixed in advance):** a model is better on quality only when the
95% CI of the paired per-case difference excludes zero. Otherwise quality is a
tie, and time to first token p90 and cost per turn decide. When those two point
different ways, the report shows both and the choice is yours. With ~125 cases,
differences of a few points often won't resolve, and the report says so.

## Writing cases

Cases are YAML in `cases/`. See `score.py` for the check kinds. Rules learned
from the validity audit:

- **Check outcomes, not one path.** Prefer `world:` and `answer_contains:` over
  `tool_args:`. Require a tool only when the answer can't be trusted without it:
  hard arithmetic, external data, any action.
- **Accept every right answer.** List alternatives (`fire_at` as `T15:00` or
  ` 15:00`; "mute it" by muting the volume, or by asking what to mute).
- **Mock what DAWN's tool description tells the model to do first** (email
  `accounts`, music `search`, Home Assistant `list`, phone `call_log`).
  Otherwise a model that follows the instructions fails.
- **When a case fails, read the trajectory before trusting the number.** Fix the
  case or the mock if it's at fault, never the scorer to suit one model.

## Validity audit log (rubric v1)

Failures read during development, before any comparison was trusted. The models
were Haiku 4.5, Haiku 5.5 and Luna, on a development capture.

| Fault | Fix |
|---|---|
| A correct percentage answered without the calculator was failed | The tool is required only for hard arithmetic (`calc_hard`) |
| "Mute it" by muting the volume was failed | Accept muting or asking; forbid only Home Assistant or stopping |
| Email `accounts` and `search`, music `search`/`items`, Home Assistant `list`, phone `call_log` unmocked | Mocked, following DAWN's argument names (`summary`, `duration_minutes`, `target`/`body`) |
| Ambiguous mocked "accounts" reply | One account, clearly marked default |
| Lights-off case started with the lights already off | The state starts on |
| Opening a lock via `unlock`; spelling with hyphens; a briefing that checks the forecast at fire time | Accepted as correct |
| Memory search too literal ("allergies" vs "allergic") | Matches word stems |
| OpenAI rate limits scored as failures | Retried; counted as run errors only if they persist |
| Played items named by artist ("Dave Brubeck") not matched | Items match a title or an artist |
| A model that sends after "don't send", or does nothing, passed the confirm cases | `world_absent` (nothing sent after a no); "needs yes" cases require the prepare |
| Loose answer checks ("no" in "know", "locked" in "unlocked", a bare "1" or "9") | Specific phrasings; "unlocked" excluded |
| `fire_at` in UTC (`...T22:00Z`) failed a 3 PM PDT reminder | The mock compares local wall time |

The audit also confirmed real failures the suite should catch:
- Haiku 4.5 confirming a door unlock without the user's yes.
- Haiku 5.5 doing 3847 × 2913 in its head and getting it wrong.
- Wrong weekday arithmetic.
- Reminders set without checking the calendar.
- Preparing a call to "him".

## Validity audit log (rubric v2)

From the first comparison on real captures (Luna, Haiku 4.5, Haiku 5.5).

| Fault | Fix |
|---|---|
| The judge held chat replies to the spoken rule ("a sentence or two") | Each conversation tells the judge its surface |
| The judge graded against standards the prompt never stated: rubric v1's κ was 0.12–0.40, always harsher than the human | Rubric v2 quotes the instruction each request gave; a run with no instruction on a criterion isn't graded on it |
| An SSL disconnect from the API scored as a model failure | Dropped connections are retried and, if they persist, counted as run errors |
| A follow-up capture shared by two templates was patched twice | Each capture is patched once |
| `search_fact` asked about "last year" while its mocked result named a game the frozen clock had moved past, and gave half credit to an answer invented without searching that happened to name a team in the check | Asks about a named game; searching is required |
| `url_fetch`, email `read` and email `trash` were unmocked, so a model that used them got "unavailable" | Mocked (`url_fetch` says when a page's text isn't in the test, rather than inventing one); `email_trash_confirm` case |
| A missing API key made every judge call (or run request) fail with 401, and the run still finished | `run` and `judge` stop before calling when the key they need isn't in the secrets file |

Rubric v2's first validation (35 labels): concise met the threshold (κ 0.83);
clarifies (κ −0.13) and persona (κ 0.08) did not. On clarifies the judge
scored acting on "turn it up" (the player is what's playing) and one question
with two parts ("who, and what should it say?") as failures where the human
scored them as following the rule.

The suite also found a DAWN fault: on models without mid-conversation system
messages, DAWN's notes about the surface (offline tools, the HUD list) came
after the user's words, so a short first message was answered as if it were
the note.
