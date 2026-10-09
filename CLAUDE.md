# CLAUDE.md

Guidance for Claude Code when working in this repository.

## Project Overview

D.A.W.N. (part of The OASIS Project) is a voice-controlled AI assistant daemon written in C/C++. Integrates Whisper ASR (CUDA), Piper TTS (ONNX), cloud + local LLMs (OpenAI/Claude/Ollama/llama.cpp), MQTT command/control, DAP2 satellites (RPi + ESP32), vision, extended thinking, a scheduler, and a plan-executor DSL for multi-step tool orchestration. Primary target: Jetson with CUDA. Also supports x86_64 server mode.

See @ARCHITECTURE.md for subsystem breakdowns, data flow, and module dependencies.

## ⚠️ THE PRIME DIRECTIVE: WE ALWAYS DO IT RIGHT. WE DON'T DO STOP-GAPS. ⚠️

**This rule outranks convenience, speed, and scope. It governs every other rule in this file.**

- **Do it right. Don't be lazy.** Every fix is the real fix, designed properly and phased properly.
  There is no "quick fix now, real fix later." Never propose a stop-gap, a band-aid, a workaround
  dressed up as a fix, or a degrade-instead-of-fix path as the plan. If the proper fix is large,
  **phase the proper fix**: every phase must be a piece of the final design, not something to rip
  out later.
- **DAWN is a public project.** Judge correctness against **every user's** accounts, configs,
  platforms, and hardware — never only the developer's. "It works on my setup" or "my account isn't
  affected" is not an argument. Other people run this.
- **Verify, don't assume.** Check claims against the code and against authoritative vendor
  documentation or live behavior, and reproduce a bug before calling it fixed. If you haven't
  verified something, say so plainly.
- **Right is not maximal.** The right fix is the *least code* that fully closes a real problem.
  Simple beats clever; a smaller design that works beats a complete one that's harder to read, test
  and change. Least code is still the real fix, never a band-aid at the symptom.
- **Real, reachable, practical.** Fix what a real user, or a real attacker against a real install,
  can hit today. Drop what needs a stack of unlikely conditions — don't fix it, and don't log it in
  TODO either: a theoretical TODO row is debt that never closes and buries the real items. Reachable
  includes anything an outsider can cause through content DAWN reads (email, web, SMS, documents),
  and any silent, irreversible outcome (data loss, a wrong action) even when rare. Justify each fix
  or drop in one sentence: who triggers it, and how, on a default install.
- **A burden is a bug (WWFD).** DAWN should feel like FRIDAY: usable, versatile, faster than doing it
  yourself. A safeguard that makes a common request ask or fail every time has failed. Weigh
  usability and security together; neither wins by default.
- **When a real tradeoff exists,** lay the options out honestly, say what each one actually fixes,
  and recommend the proper one even when it's more work. Never quietly pick the easy option, and
  never pick the heavy one just to look thorough.

## Critical Rules — Always Follow

- **NEVER delete files.** Tell the developer which files to delete. Files may hold secrets or unrecoverable data.
- **Commits need an explicit OK; never push.** Propose the `git add` and the message, then run `git add`/`git commit` only after the developer approves that specific commit (approval doesn't carry to the next one). **Never `git push`** — the developer pushes.
- **Merge strategy: merge commits (not rebase), since 2026-08-18.** PRs land as merge commits, which **preserve each branch commit's original SHA on `main`** — so a commit hash is a durable reference: cite hashes freely in TODO.md/DONE.md/design docs. `PR #N` is still richer (diff + all commits + review threads + CI) when the review context matters, so prefer it there, but it's a preference now, not a correctness rule. (This reverses the old rebase-era "cite PRs, not branch hashes" convention — branch SHAs no longer die at merge.)
- **Feedback before implementation.** When the developer asks a question, provide analysis, trade-offs, and a recommendation *first*. Wait for explicit confirmation ("go ahead", "do it", "yes") before coding.
- **Format before committing.** Every change must pass the format check. The pre-commit hook enforces it on the staged content (`./format_code.sh --staged --check`); CI runs the full-tree `--check`.
- **GPL header on every new `.c`/`.cpp`/`.h`.** Template at the bottom of this file.
- **Adding a `dawn.toml` setting? Read @docs/CONFIGURATION_GUIDE.md first.** It touches up to nine files. A setting that `config_write_toml()` doesn't emit is **silently deleted from the user's `dawn.toml`** on their next WebUI settings save — clean build, green tests, no warning. Already shipped four times. `config_to_json()` (GET) + `config_write_toml()` (persist) + `webui_config.c` POST handler always move together, and new sections go in `tests/test_config_roundtrip.c`'s `required[]`.
- **Never commit `docs/TODO.md` or `docs/DONE.md`** — both developer-maintained. TODO.md is the active list; DONE.md is the shipped/completed archive (moved 2026-05-28). When an item ships, move its `~~strikethrough~~` row from TODO.md into DONE.md under the matching section.
- **Keep internal planning language out of committed text.** Code comments, build-file comments, and commit messages must stand on their own for a future reader who has none of our working context. So: no references to `docs/TODO.md`/`docs/DONE.md` or any other untracked doc (they're not in the repo — the pointer dangles), and no internal roadmap shorthand like "Phase A/B/C" of some rollout (meaningless outside the session that coined it). State the substance directly instead — *what* the code does or why, not *where it sits* in our plan. A dated commit hash or a tracked-doc path is fine; a TODO row number or phase label is not.
- **Design doc commit policy**: commit design docs only when they describe shipped or in-flight code (implementation matches the doc substantially). Docs for planned-but-unstarted work and working/scratch docs stay untracked — the developer uses them as a local unimplemented-work reminder. When unsure, ask.
- **User-facing upgrade notes → `UPGRADING.md`.** When a change affects someone *upgrading* an existing install — a changed default, a schema/data migration needing a manual step, a user-visible behavior change, or a new opt-in — add a dated entry at the **top** of `UPGRADING.md` (newest-first), in plain language: what changed and what (if anything) the user must do. Add an entry **only when there is real upgrade impact**; drop-in changes need none. This IS a tracked, committed doc (unlike TODO.md/DONE.md), and it's user-facing — write for a human running the update, not for a developer reading the diff.

## Untrusted content (prompt injection)

Prompt injection is not solvable inside the model: no frame, filter or prompt holds against a
determined attacker (adaptive attacks beat >90% of published defenses), and small local models
follow injected text far more readily. Safety comes from what a steered turn can reach.

- **Check vendor guidance first** (Anthropic, OpenAI, Qwen) before designing anything that puts
  outside text in front of a model.
- **Outside text goes in tool results only**, never the system prompt or the user's message
  (Anthropic: Claude is trained to distrust instructions there). Label its source; frame and
  neutralize it.
- **Gate in code, not prompts:** hard-block only where outside text in the turn meets an effect
  that is silent or irreversible and no human sees a DAWN-rendered summary first. Elsewhere prefer
  visible + undoable over refusal.
- Details, sources and the decision rule: `docs/PROMPT_INJECTION.md`.

## Build & Test

```bash
# Build (creates build-debug/)
cmake --preset debug
make -C build-debug -j8

# Run
LD_LIBRARY_PATH=/usr/local/lib ./build-debug/dawn

# Format
./format_code.sh --changed       # fast: only uncommitted/staged files (use during dev)
./format_code.sh --check --changed  # verify only changed files (local pre-commit; run AFTER build)
./format_code.sh                 # fix all files
./format_code.sh --check         # full-tree scan (what CI runs; walks the whole repo, slow)
./format_code.sh --staged --check  # the staged (index) content — what the pre-commit hook checks

# CI test suite (plain `make` does NOT rebuild test binaries — build tests-ci first)
make -C build-debug tests-ci
ctest --test-dir build-debug -L ci

# Single unit test (standalone binaries in tests/)
make -C build-debug test_<name>
./build-debug/tests/test_<name>
```

- Dependencies and setup: see @DEPENDENCIES.md and @GETTING_STARTED.md.
- x86_64 server mode: see `docs/GETTING_STARTED_SERVER.md`.
- Git hooks: `./install-git-hooks.sh` (one-time) installs both:
  - **pre-commit** — `format_code.sh --staged --check`; when source/build files are staged (`src/ include/ common/ tests/ benchmarks/ cmake/`, `CMakeLists.txt`, `*.cmake`) it also builds `tests-ci benches` and runs `ctest -L ci`.
  - **pre-push** — builds `tests-ci` and runs the `ci` suite (prefers `build-ci/`, the WebUI-off build).
  - Optional `pre-commit.local.hook` / `pre-push.local.hook` (gitignored) run as local extensions.

## Code Standards

Full standards in @CODING_STYLE_GUIDE.md. Critical gotchas that differ from typical C:

- **Return codes**: `SUCCESS` (0) and `FAILURE` (1). **Never use negative returns** (no `-1`, no negative errno). Use error codes > 1 for specific errors.
- **Naming**: `snake_case` functions/vars, `UPPER_CASE` constants, typedef with `_t` suffix.
- **Memory**: prefer static allocation; always null-check after malloc; `free(ptr); ptr = NULL;`.
- **Functions**: soft target < 50 lines, inputs first / outputs last, clarity over line counts.
- **Comments**: Doxygen for public APIs; explain "why" not "what".
- **Formatting**: 3-space indent, 100-char lines, K&R braces, right-aligned pointers (`int *ptr`). Enforced by `.clang-format`.

### GPL File Header (required on every new source/header file)

```c
/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s).
 *
 * [Brief description of file purpose]
 */
```

## Thread Safety (non-obvious)

- **ASR models** (Whisper/Vosk): read-only; create separate recognizers per thread.
- **TTS engine**: mutex-protected (`tts_mutex`).
- **LLM endpoint**: handles concurrent HTTP requests.
- **Conversation history**: needs mutex for multi-client writes.
- **Local provider detection**: cached with mutex, 5-minute TTL.

## File Size Discipline

- **1,500+ lines (C) / 1,000+ (JS)**: flag as getting large.
- **2,500+ lines**: recommend splitting before adding features.
- **New feature in a large file**: propose creating a separate module instead.
- **Refactoring large files**: never attempt a full rewrite. Incremental extraction only — one feature at a time, keep original working, test after each step.

## Patterns

### Tool registration (modular — preferred for new tools)

Tools are self-contained modules in `src/tools/` with metadata, config, and callbacks. Register via `src/tools/tools_init.c` in `tools_register_all()`. The registry provides O(1) name/device/alias lookup via FNV-1a hash tables. See `src/tools/memory_tool.c` or `src/tools/calendar_tool.c` for a clean template.

### Command callbacks (legacy — for core system devices in `mosquitto_comms.c`)

```c
char *myCallback(const char *actionName, char *value, int *should_respond) {
   // should_respond: 1 = return data to AI, 0 = handle directly
   // Return: allocated string for AI (when should_respond=1), or NULL
}
```

### Logging

```c
OLOG_INFO("System initialized");
OLOG_WARNING("Battery voltage low: %.2fV", voltage);
OLOG_ERROR("I2C communication failed: %d", error);
```

The macros are `OLOG_*` (`common/include/logging.h`), not `LOG_*`, which collide with syslog's level constants.

## Configuration Files

- `dawn.toml` — runtime config (LLM provider, ASR/TTS, audio, network, WebUI, scheduler, MQTT). See file for all sections.
- `dawn.h` — compile-time defaults: `APPLICATION_NAME`, `AI_NAME`, the default persona (`AI_PERSONA*`), the default voice-output directives and the ASR disambiguation hint. Other defaults (MQTT broker, etc.) live in `src/config/config_defaults.c`.
- `secrets.toml` — API keys / OAuth credentials. **Gitignored.** Never commit.

**Adding or changing a setting** — follow @docs/CONFIGURATION_GUIDE.md. Two mechanisms exist: *tool-owned*
config (`config_section` + `config_writer` in `tool_metadata_t`, written back automatically by the tool
registry — prefer this for anything tool-specific, it has no round-trip hazard) and *global* config
(`dawn_config_t` + `config_parser.c`, where you must wire the writer yourself). Verify with
`tests/test_config_roundtrip.c`, then by saving an unrelated settings panel and confirming your section is
still in `dawn.toml`.

## Satellite Development

- **Tier 1 (Raspberry Pi)** — full satellite binary in `dawn_satellite/`. Build: `cd dawn_satellite && mkdir build && cd build && cmake .. && make -j8`. See `docs/DAP2_SATELLITE.md`.
- **Tier 2 (ESP32)** — Arduino-based, streams raw PCM over WebSocket. See `docs/WEBSOCKET_PROTOCOL.md` for the binary wire protocol.

## Development Lifecycle

1. **Plan** (non-trivial only) — plan mode for features touching multiple subsystems. Explore agent to understand, Plan agent to design, architecture-reviewer / ui-design-architect on the plan before exiting.
2. **Implement** — task tracking for multi-step work. Build + format + unit tests after each logical chunk.
3. **Review** — run relevant review agents in parallel on the diff. Consolidate findings, triage (fix / skip / ask), apply fixes, re-verify. Reviewers report everything; we act only on what's reachable.
4. **Test** — developer tests manually and reports. Fix issues found; adjacent bugs may warrant their own mini cycle.
5. **Document** — update or create the atlas design doc (`~/code/The-OASIS-Project/atlas/dawn/`) for significant features. Memory-subsystem docs land under `atlas/dawn/memory/`; everything else flat under `atlas/dawn/archive/`. Have architecture-reviewer verify the doc against code.
6. **Update planning docs** — cut the item's row from `docs/TODO.md` and paste it into `docs/DONE.md` under the matching section, with the `~~strikethrough~~` SHIPPED tag including the commit hash; remove any `§N` detail section from TODO.md.
7. **Commit** — always **format before building**, so the bytes you compiled and tested are the bytes you commit (formatting rewrites code; compiling first verifies a draft you then discard). The verify order is `./format_code.sh --changed` → build → test → `./format_code.sh --check --changed` (changed-scope verify — never bare `--check`, which walks the whole tree). Then provide a single `git add` command and a commit message, and wait for the developer's OK before running them. **The developer pushes.**

## Code Review Workflow

Trigger phrases: "code review", "review my changes", "run the agents", "run the big three", "run all four", "run all five", "run all six", "full review", "what do the agents think?". The `/review` skill (`.claude/skills/review/SKILL.md`) implements this workflow.

1. Capture diff via `git status` + `git diff`.
2. Launch review agents in **parallel** (these are user-level agents in `~/.claude/agents/`, not in the repo — a fresh clone won't have them):
   - **Big three** (code review / run the big three): `architecture-reviewer`, `embedded-efficiency-reviewer`, `security-auditor` — **plus `correctness-reviewer` in every set** (the general-logic lens; it also checks non-literal format strings such as prompt templates, which `-Wformat` can't).
   - **All four** (run all four): above + `ui-design-architect` (when UI changes present).
   - **All five / all six** (full review / run all five / run all six): above + `coding-standards-auditor` — mandatory for large refactors, new modules, or pre-release audits.
3. Synthesize into one table with severity and action. Triage on **reachability**, not the label (see the prime directive): **fix** what's reachable in practice, any severity, with the least code; **drop** what isn't — no TODO row — and say so in one line. Pre-existing issues the same way: fixed because they're real and reachable, not because they were found.
4. Apply the fixes; re-verify format and tests.
5. **One full review round per commit.** Post-review fixes get a self-check, or **one targeted reviewer** when they're substantial (a rewrite, new logic) — code written *after* the agent pass was seen by no reviewer, which is how the reconnect-state bug on the music branch reached the PR and was caught only by the bots. Never re-run the full set for polish.

## Benchmark Methodology — `recall_reach` ≠ leader entailment

LoCoMo/LongMemEval/ConvoMem measure different things depending on flags. The bench prints a `LEADER-COMPARABLE: YES|NO` banner and tags `leader_comparable` in the results JSON. Honor it.

- **`recall_reach`** — top-K provenance overlap. Internal diagnostic. **NEVER** quote alongside ByteRover/MemMachine/Hindsight/Mem0 — they publish LLM-judge generation, not retrieval reach.
- **`recall_generation`** — generate-and-judge. **Leader-comparable IF the Mem0 protocol is matched.** Required flags: `--memory-pipeline --generator-provider openai --generator-model gpt-4o-mini --judge-provider openai --judge-model gpt-4o-mini --prompt-style mem0 --with-source --exclude-categories 5`.

When writing a number for an external audience (atlas, X post, paper, README), confirm `leader_comparable: true` in the run's results JSON or DON'T compare to leaders. See `benchmarks/README.md` for full methodology. Historical benchmark numbers live in [`atlas/dawn/memory/STATE.md`](https://github.com/The-OASIS-Project/atlas/tree/main/dawn/memory) — reference that file rather than re-listing numbers in TODO.md.

## Design Docs

- **Active planning**: @docs/TODO.md (master, untracked), plus per-feature docs in `docs/` (e.g., `PHONE_SMS_DESIGN.md`, `DEEP_RESEARCH_DESIGN.md`).
- **Shipped/completed archive**: `docs/DONE.md` (untracked, sibling to TODO.md). Migrated 2026-05-28 when TODO.md outgrew its own length. Section structure mirrors TODO.md so cross-reference back stays trivial.
- **Archived designs**: [atlas/dawn](https://github.com/The-OASIS-Project/atlas/tree/main/dawn) — shipped-feature design docs kept for historical reference. Memory subsystem under [`memory/`](https://github.com/The-OASIS-Project/atlas/tree/main/dawn/memory) (system design, injection filter, cat-2 temporal, reranker investigation, LoCoMo cat-3 profiling). Everything else flat under [`archive/`](https://github.com/The-OASIS-Project/atlas/tree/main/dawn/archive) — RAG, user auth, plan executor, scheduler, image search, CalDAV, email, etc.

## License

GPLv3 or later. Every new source file includes the GPL header block (see Code Standards above).

### Third-party attribution

Portions of the memory subsystem are adapted from [mem0ai/mem0](https://github.com/mem0ai/mem0) (Apache 2.0, © 2023 Taranjeet Singh) — see `NOTICE` and `DEPENDENCIES.md` (Architectural Influences). When you port or adapt code from Mem0, add a `/* Adapted from mem0ai/mem0 (Apache-2.0). See NOTICE. */` comment block at the point of use describing what was changed. Apache 2.0 §4 requires preserving the copyright notice and stating significant modifications — the per-file comment plus the central NOTICE/DEPENDENCIES entries satisfy both.
