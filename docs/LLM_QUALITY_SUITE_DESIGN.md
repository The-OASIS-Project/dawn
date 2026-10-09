# LLM Quality Suite v2: Design

**Status:** rev 5 (2026-10-09). Built and used for its first decisions: capture (P0, WebUI surfaces with the
Claude, OpenAI Responses and chat-completions carriers), the runner and checks (P1), 125 audited cases (P2), the judge with
rubric v2 and labeling (P3, concise validated, clarifies and persona not yet), statistics (P4) and the usage
README (P5, in part), and per-capture reasoning (`@effort`). The remaining work is in §4. Usage: `llm_testing/quality/README.md`.
**Replaces:** `llm_testing/scripts/test_llm_quality_native.py` as the basis for choosing a chat model. The old
suite stays as a quick smoke test.
**Why now:** choosing a chat default between OpenAI `gpt-5.6-luna`, Claude Haiku 4.5 and Haiku 5.5 showed that
the current suite can't rank strong models (§1). Extraction has a solid benchmark (LoCoMo, leader-comparable);
chat doesn't.

---

## 1. What's wrong with the current suite

Measured 2026-10-08, three runs per model, thinking off, with DAWN's `[system_time]` line added:

| Model | Score | Where the points went |
|---|---|---|
| Haiku 4.5 | 99.0% | One conversational point |
| Luna | 96.5% (100 / 91.3 / 98.1) | Multi-tool 5/7 in two runs; mental math once |
| Haiku 5.5 off / low / medium | 90.7 / 91.7 / 92.9% | Answers 47×89 in its head (correct), scored 2/9 |

The gaps are about one test wide, and the scoring decides them:

1. **Too few cases.** 15 tests and 116 points: one test is 6–11% of the score, and there are no confidence
   intervals.
2. **It scores form, not outcome.**
   - A correct answer without the expected tool call loses 7/9.
   - Persona is a regex for "sir"/"boss".
   - Brevity is a length check.
   - Arguments are substring-matched.

   A 2026 audit of published tool-use benchmarks names substring checks and lock-in to one trajectory as the
   top grader defects (18.5% grader–human misalignment, arXiv 2607.02577).
3. **Single-shot, no tool results.** A sensible tool sequence (`date` → `scheduler`) fails at the first call.
   The final answer, which is what the user hears, is never scored.
4. **Not production's inputs.**
   - It uses a ~1K synthetic prompt and a hand-dumped 10-tool subset from Aug 2026. Production's remote tool set
     is 59 tools (~101 KB) and its frozen prefix ~10.9 KB.
   - Production puts the time line inside a framed turn-context block, not as a bare line.
   - The Time/Date tests expect a tool call that production's prompt tells the model to skip.
   - It uses max_tokens 200/512; production uses 16384.
5. **The wrong latency.** It measures whole-response time on a small prompt, with no streaming time to first
   token and no prompt caching.

What it's still good for: a smoke test that a model calls DAWN's tools at all.

## 2. Goals and non-goals

**Goals**
- Rank chat models on what users experience:
  - right action (tool and arguments)
  - right final answer
  - DAWN's voice (concise, in persona, speakable)
  - reliability across repeats
  - time to first token
  - cost per turn
- Production-faithful inputs, **captured from real daemon traffic**, never rebuilt or hand-maintained.
- Honest statistics: report what the data can resolve, and decide by a rule fixed in advance.
- Cheap to run: a few dollars per full comparison, re-runs nearly free through caches.
- One runner for cloud (Claude, OpenAI incl. Responses, OpenRouter) and local (llama.cpp/Ollama).

**Non-goals**
- A general leaderboard.
- End-to-end voice (ASR/TTS).
- Memory retrieval quality (LoCoMo owns it).
- Simulated users: follow-up turns are scripted.
- Satellite (DAP2) capture until a Python satellite client exists.
- Rescoring old runs under new rubrics: results store `rubric_version`.

## 3. Design

### 3.1 Capture real requests, don't rebuild them
Rebuilding a request out of band can't be faithful:
- The prompt builder needs a live session of the right surface type. It picks voice directives, the ASR hint,
  `Room=`, the satellite area and inline-tool support from the session (`prompt_builder.c` ~586-632).
- The tool list reads the thread-local command context (`llm_tools_filter.c` ~290).
- The frozen prefix carries a per-conversation tag, and the turn context is a framed block
  (`session_prefix.c`, `prompt_sections.c`).
- The admin socket's `uint16_t` framing can't carry a 100 KB payload.

So:

- **Hook:**
  - `llm_request_capture(provider, url, headers, body)` at every chat send site:
    - `llm_claude.c` (both senders)
    - `llm_openai_chat_completions.c` (both)
    - the Responses sender in `llm_openai_responses.c`

    `llm_local_provider.c`'s sender only queries Ollama's context size; local chat goes through
    chat-completions.
  - When armed for a user, it writes the next N requests to `<dir>/<n>-<provider>.json`:
    - the URL and headers, with `x-api-key`/`Authorization` redacted
    - the full body
  - The admin names the directory, the same as `db backup <path>`. It is opened once (`O_DIRECTORY|O_NOFOLLOW`)
    and must be empty, owned by the daemon's user, and writable by no one else. Files are written with
    `openat(..., O_EXCL|O_NOFOLLOW, 0600)`. URL userinfo and credential query parameters are redacted too.
  - The hook disarms after N requests, or after 30 minutes.
- **Arm:** `dawn-admin llm capture --user quality-fixture --out <dir> --requests N` (or `--stop`). The admin
  socket carries only arm/disarm, never the payload. `scripts/quality_capture_all.sh` arms with twice the
  requests the standard conversations need, so a retry can't leave a turn uncaptured.
- **Drive real turns** as the fixture user (`scripts/quality_capture.py --standard`, over **one**
  WebSocket connection, since every connection holds one of the daemon's 8 session slots until its idle
  timeout), one capture per surface:
  - `webui-text` and `webui-spoken`, through the WebUI login path `scripts/ws_observer.py` already uses
    (built)
  - `local-mic`, typed through the local path (not yet)

  Then one capture per provider family: Claude and OpenAI Responses (built); OpenAI chat-completions,
  OpenRouter and local (not yet: the chat-completions reader exists but hasn't run on a real capture).
  Each surface gets a 1-turn and a 2-turn capture; later turns of a case use the 2-turn capture's shape.
- **Cross-check (not yet):** the DB already stores each conversation's exact prefix and tool bytes with sha256
  (`prompt_blobs`). Assert that the captures' prefix hash matches across providers.
- **The suite changes only:**
  - the user text
  - the two timestamps inside the framed turn-context line (virtual clock, §3.2)
  - `stream: true`

  Everything else goes out as captured: sampling, thinking, max_tokens, cache_control breakpoints,
  system-message shape, tool set.

### 3.2 Fixture user, virtual clock
- **A `quality-fixture` user**, non-admin, created in the WebUI's user admin. Not a user with harness history,
  memories or an MCP grant: its tool set would differ from a normal user's.
  - `scripts/quality_capture.py --setup` sets its fictional profile (identity, location, timezone, units);
    `scripts/quality_capture_all.sh` runs it before every capture.
  - **Persona:** the default, or a persona file the fixture uses in replace mode
    (`quality_capture_all.sh <dir> <persona-file>`). The capture records which in `persona.txt`.
  - WebUI conversations are created private, so nothing is extracted from them.
  - No facts are seeded: the memory and recall tools are mocked (§3.3).
- **Never hand-edit a capture.** Re-capture instead.
- **Captures stay local.** They hold an install's prompt, tool list and room and HUD details, and a capture made
  with a persona file holds that persona; each install captures its own. Credentials are redacted at capture.
- **Virtual clock:** freeze `now` per run (for example Tue 2026-10-13 09:30 in the fixture's timezone), write it
  into the captured time line, and store it in the results. Mocks derive "today" from it. Runs are then
  reproducible and caches can hit.

### 3.3 Multi-turn loop over stateful mocks
- **Scripted turns:** each case has `turns: [user text, ...]`, deterministic and not simulated.
- **The loop:** the harness runs DAWN's tool loop, up to `LLM_TOOLS_MAX_ITERATIONS`, until a final text answer.
  Tool results are appended in each provider's shape. For Claude that's one `tool_result` user message, as in
  `llm_tools_results.c`.
- **Mocks** are templated generators over a small world state (calendar events, reminders, playback, inbox):
  - `weather(location)` returns "Forecast for {location}: …"
  - `calculator(expr)` evaluates
  - `scheduler(create)` adds to state
  - search returns fixed snippets

  Grounding is then deterministic: the mocked value must appear in the final answer.
  - An unmocked call gets a generic "unavailable" result and is **recorded, not auto-failed**, because some
    extra lookups are legitimate.
- **Confirm flows:**
  - A prepare's result mints an id.
  - The scripted turn 2 confirms it. Turn 2 is the very next turn of the same session, the same constraint as
    `turn_origin.h`.
  - Covered pairs:
    - email `send`/`trash`
    - phone `call`/`send_sms`/delete
    - Home Assistant `open`
    - document `delete`
    - research `start`
- **Excluded tools:** `switch_llm` and `reset_conversation`. They skip the follow-up turn.

### 3.4 Outcome-based scoring
Each case declares checks:

| Kind | Example | How |
|---|---|---|
| **Action** | A reminder exists with `fire_at` = tomorrow 15:00 local | Final world state / tool args vs expected values; any valid path passes |
| **Answer** | Says 4,183; states the mocked forecast | Normalized value match in the final text, with or without a tool |
| **Tool required** | Hard math, external data, any action | The tool appears in the trajectory |
| **Forbidden** (must-not-call) | "What day is it?" must not call `time`; no act without a prepare | Listed tools absent / ordering rule |
| **Speakable** (deterministic, voice surfaces) | No markdown tables, code fences, URLs or emoji; no long lists | Regex/structure pre-check against `DEFAULT_VOICE_OUTPUT_DIRECTIVE[_WEBUI]` (`dawn.h`) |
| **Voice** (judged) | Concise, in persona, asks for clarification when ambiguous | LLM judge, rubric 0–2 per criterion (§3.5) |

- **Equivalents:** a case can list alternate valid trajectories (`equivalents`).
- **Mental math:** easy arithmetic answered correctly scores full marks. Only "hard" math requires the tool.
- **Headline metrics:** `action_accuracy`, `answer_accuracy`, `voice_score`, and the composite, all per category.

### 3.5 Judge
- **Rubric v2: grade against the instruction the request gave.** Each criterion quotes it from the run's
  capture: concise the reply-length rule (plus the spoken direction on a spoken surface), clarifies the rule on
  incomplete requests, persona the persona text (replace mode's own text, or the default persona). A request
  that says nothing on a criterion isn't graded on it. Rubric v1 graded against a fixed standard the prompt
  never stated, and a human disagreed with it consistently (κ 0.12–0.40, the judge always harsher).
- **Judge model:** pinned, and outside both finalists' families when they differ (default
  `google/gemini-3.1-pro-preview` through OpenRouter, ~$0.006 per verdict). Otherwise use two judges on a 20%
  subset and report their disagreement.
- **What it sees:** model identity is stripped from the transcript, and the mocked tool results are included so
  grounding can be judged.
- **Validation:**
  - 40–60 human-labeled responses, stratified across the score range (failures included), stored per rubric
    version (`labels_v<N>.json`); the labeler is shown the instruction it grades against
  - **Cohen's κ ≥ 0.6 per criterion**, reported with its CI
  - not raw agreement, which a judge that always says "pass" can meet
  - a criterion below the threshold is reported but never decides anything
- **Caching:** verdicts are cached by (case, response hash, judge model, rubric version).
- **Cost:** ~900 verdicts per 3-model × 3-run comparison, about $3 with a Sonnet/Gemini-class judge and about $10
  with an Opus-class one. Re-runs are free.

### 3.6 Cases
- **Count:** 120–150 cases in versioned YAML (`llm_testing/cases/*.yaml`). Action and answer checks are cheap
  once mocks exist. A stratified subset gets the voice judge.
- **Categories:**
  - single tool (music, weather, search, calendar, email, scheduler, Home Assistant, memory, calculator
    easy/hard)
  - multi-step ("remind me 30 minutes before my next meeting")
  - ambiguous / needs clarification
  - conversational
  - context use (time and date from the turn context, relative dates)
  - confirm flows (2-turn)
  - irrelevance (no tool should fire)
  - voice-style phrasing ("uh play some jazz")
- **Case fields:** id, category, surface, turns, mocks/state, checks, equivalents, forbidden tools, weight.
  Expected values reference the virtual clock symbolically (`tomorrow 15:00`).

### 3.7 Runs, statistics, decision rule
- **Repeats:** R runs per model (default 3). The unit of resampling is **the case**: take the per-case mean over
  runs, then a paired bootstrap over cases for every metric and each model pair. A sign test is reported as a
  robustness line only.
- **Reliability:** report **pass^k** (k = R) per category beside the mean. A voice assistant that works two
  times in three is failing.
- **Decision rule, fixed in advance:** a model is preferred on quality only when the paired 95% CI of the
  per-case difference excludes zero. Otherwise time-to-first-token p90 and cost per turn decide.
  "No significant difference" is stated explicitly.

  Resolution, to be honest about it: at ~150 cases, differences of a few points will often not resolve (Miller,
  arXiv 2411.00640). The rule covers that case.
- **Latency:** every request is streamed, recording time to first token and total time. Report p50/p90 per
  surface. The first request per model is flagged as a cold-cache read. All cases share one frozen prefix, so
  the rest read warm within the cache TTL, and no separate warm pass is needed.
- **Cost:**
  - measured from `usage`, including cache read/write tokens
  - priced with per-model USD rates from OpenRouter's `/api/v1/models` (DAWN already fetches it), snapshotted
    into the results, times models.toml's cache multipliers
  - no hand-kept price table
- **Output:** a schema-versioned results JSON plus a markdown report, with response and judge caches.

### 3.8 Runner shape
- **Package:** `llm_testing/quality/`, kept under ~1,500 lines:
  - `cases.py`
  - `capture.py`: loads captures and substitutes the user text and clock
  - `providers.py`: streams and parses three SSE dialects (Anthropic, chat completions, Responses), and appends
    tool results per provider. It contains no request builders.
  - `mocks.py`
  - `loop.py`
  - `score.py`
  - `judge.py`
  - `report.py`
- **CLI:** `python3 -m llm_testing.quality run --captures <dir> --runs 3 [--model M] [--only cats]
  [--system-patch F]`, then `judge`, `label`, `validate` and `compare <results...> --a M --b N` (`N:M` picks a
  model from the Nth results file).
- **Prompt A/B on the same captures:** `--system-patch` replaces `[old, new]` text pairs in every captured system
  prompt, each old text exactly once or the run stops. The results store the patch and the judge grades against
  the patched instructions. A wording adopted this way is confirmed by a real capture.
- **Not an off-the-shelf eval framework** (e.g. Inspect AI): DAWN must send its exact captured body
  (cache_control, system-message shape, Responses items), and those frameworks own the request body. A thin
  runner is justified.

## 4. Phasing
Each phase is a piece of the final design.

**Built**
- **P0 Capture:** the hook, `dawn-admin llm capture`, the capture scripts, persona captures; WebUI text and
  spoken with the Claude and OpenAI Responses carriers. Consecutive turns differ only as §3.1 allows.
- **P1 Runner:** frozen clock, stateful mocks, scripted multi-turn, the outcome checks.
- **P2 Cases:** 125 cases, validity-audited on development and real captures (README audit logs).
- **P3 Judge:** rubric v2, labeling, validation; concise validated (κ 0.83, 11 labels).
- **P4 Stats:** paired bootstrap, pass^k, time to first token, cost, the decision rule; transport errors
  retried and excluded.
- **P5 Docs, in part:** `llm_testing/quality/README.md`.

**Remaining**
1. **Clarifies and persona to κ ≥ 0.6.** Clarifies disagreed on acting where the player or calendar answers
   the question (rule 2 says check first) and on one question with two parts; tighten the rubric there, then
   fresh labels. Persona needs more labels. Until then neither decides anything.
2. **Local capture: done (2026-10-09).** `local:<model>` captures the chat-completions carrier from a
   llama.cpp server; a smoke run passed end to end (reader, tool calls, results). OpenRouter's `anthropic/`
   models use the Claude Messages path, already covered. Run local models with `--workers 1` (one
   llama.cpp slot; parallel requests evict each other's cached prompt).
3. **local-mic capture**, for the local surface's own directions.
4. **The prefix-hash cross-check** (§3.1).
5. **Trailing offers.** Haiku 5.5 ends ~19% of text replies with a question (Luna ~2%); a clause in the length
   rule didn't change it. A dedicated, more prominent rule, A/B'd the same way, is the next attempt.
6. **P5 rest:** the LLM_INTEGRATION_GUIDE points to the suite (done). Retiring
   `llm_testing/scripts/test_llm_quality.py` waits on moving the llama-server speed scripts
   (`benchmark_all_models.sh`, `test_single_model.sh`) and the latency figures they produced
   (`ARCHITECTURE.md`, `services/llama-server/README.md`) to the suite on a local capture.

## 5. Decisions
- **Capture file location:** an admin-named path, opened `O_EXCL|0600` (as `db backup`), rather than a fixed
  path under `/var/lib/dawn/`.
- **Captures are not committed** (§3.2), so no secrets/PII scan is needed.
- **Request dump vs capture:** capture of real traffic (§3.1) replaces the rev-1 synthesized `request-dump`.
- **Local models:** captured in P0 with chat-completions SSE only; no adapter.
- **Satellite (DAP2):** deferred. local-mic covers the voice-directive prompt.

## 6. First results (2026-10-08)

Three runs per model, real captures, the new prompts and a replace-mode persona; fixed checks, text / spoken:

| Model | Score | TTFT p90 | $/turn |
|---|---|---|---|
| Luna (`gpt-5.6-luna`) | 0.977 / 1.000 | 4.7 s | 0.0011 |
| Haiku 4.5 | 0.968 / 0.964 | 1.8 s | 0.0076 |
| Haiku 5.5 | 0.994 / 1.000 | 1.7 s | 0.0010 |

Haiku 5.5 is better than Haiku 4.5 on text (paired CI excludes zero; spoken ties) and ties Luna, so time to
first token and cost decide for it. The judge's validated concise score: Luna 1.00, Haiku 4.5 0.89, Haiku 5.5
0.75 (text; all 1.00 spoken).

The suite also changed DAWN's prompts: new length and asking rules, measured not to cost tool use or answers on
any of the three models, and a fix for notes that came after the user's words on models without
mid-conversation system messages.

### Chat reasoning (2026-10-09)

Haiku 5.5, the replace-mode persona, today's main, 3 runs; reasoning off (the config) against adaptive at
effort medium (`quality_capture_all.sh` with `claude:claude-haiku-5-5@medium`):

| | Score text / spoken | TTFT p50 / p90 (text) | $/turn (text) |
|---|---|---|---|
| Off | 0.985 / 0.984 | 0.69 s / 1.74 s | 0.00096 |
| Medium | 0.989 / 1.000 | 2.38 s / 4.38 s | 0.00100 |

No significant quality difference on either surface, so time to first token and cost decide: reasoning
stays off for chat. Reasoning helps where nobody waits for it, memory extraction (`[memory]
extraction_effort = "medium"`).
