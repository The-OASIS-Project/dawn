# CLAUDE.md

Guidance for Claude Code when working in this repository.

## Project Overview

D.A.W.N. (part of The OASIS Project) is a voice-controlled AI assistant daemon written in C/C++. Integrates Whisper ASR (CUDA), Piper TTS (ONNX), cloud + local LLMs (OpenAI/Claude/Ollama/llama.cpp), MQTT command/control, DAP2 satellites (RPi + ESP32), vision, extended thinking, a scheduler, and a plan-executor DSL for multi-step tool orchestration. Primary target: Jetson with CUDA. Also supports x86_64 server mode.

See @ARCHITECTURE.md for subsystem breakdowns, data flow, and module dependencies.

## Critical Rules — Always Follow

- **NEVER delete files.** Tell the developer which files to delete. Files may hold secrets or unrecoverable data.
- **NEVER run `git add`, `git commit`, or `git push`.** Suggest the command and message; let the developer run it.
- **Feedback before implementation.** When the developer asks a question, provide analysis, trade-offs, and a recommendation *first*. Wait for explicit confirmation ("go ahead", "do it", "yes") before coding.
- **Format before committing.** Every change must pass `./format_code.sh --check`. The pre-commit hook enforces this.
- **GPL header on every new `.c`/`.cpp`/`.h`.** Template at the bottom of this file.
- **Adding a `dawn.toml` setting? Read @docs/CONFIGURATION_GUIDE.md first.** It touches up to nine files. A setting that `config_write_toml()` doesn't emit is **silently deleted from the user's `dawn.toml`** on their next WebUI settings save — clean build, green tests, no warning. Already shipped three times. `config_to_json()` (GET) + `config_write_toml()` (persist) + `webui_config.c` POST handler always move together, and new sections go in `tests/test_config_roundtrip.c`'s `required[]`.
- **Never commit `docs/TODO.md` or `docs/DONE.md`** — both developer-maintained. TODO.md is the active list; DONE.md is the shipped/completed archive (moved 2026-05-28). When an item ships, move its `~~strikethrough~~` row from TODO.md into DONE.md under the matching section.
- **Design doc commit policy**: commit design docs only when they describe shipped or in-flight code (implementation matches the doc substantially). Docs for planned-but-unstarted work and working/scratch docs stay untracked — the developer uses them as a local unimplemented-work reminder. When unsure, ask.

## Build & Test

```bash
# Build (creates build-debug/)
cmake --preset debug
make -C build-debug -j8

# Run
LD_LIBRARY_PATH=/usr/local/lib ./build-debug/dawn

# Format
./format_code.sh --changed       # fast: only uncommitted/staged files (use during dev)
./format_code.sh                 # fix all files
./format_code.sh --check         # CI mode (final pre-commit verification)

# Unit test (standalone binaries in tests/)
make -C build-debug test_<name>
./build-debug/tests/test_<name>
```

- Dependencies and setup: see @DEPENDENCIES.md and @GETTING_STARTED.md.
- x86_64 server mode: see `docs/GETTING_STARTED_SERVER.md`.
- Pre-commit hook: `./install-git-hooks.sh` (one-time).

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
LOG_INFO("System initialized");
LOG_WARNING("Battery voltage low: %.2fV", voltage);
LOG_ERROR("I2C communication failed: %d", error);
```

## Configuration Files

- `dawn.toml` — runtime config (LLM provider, ASR/TTS, audio, network, WebUI, scheduler, MQTT). See file for all sections.
- `dawn.h` — compile-time defaults (`AI_NAME`, `AI_DESCRIPTION`, device names, MQTT broker defaults).
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
3. **Review** — run relevant review agents in parallel on the diff. Consolidate findings, triage (fix / skip / ask), apply fixes, re-verify.
4. **Test** — developer tests manually and reports. Fix issues found; adjacent bugs may warrant their own mini cycle.
5. **Document** — update or create the atlas design doc (`~/code/The-OASIS-Project/atlas/dawn/`) for significant features. Memory-subsystem docs land under `atlas/dawn/memory/`; everything else flat under `atlas/dawn/archive/`. Have architecture-reviewer verify the doc against code.
6. **Update planning docs** — cut the item's row from `docs/TODO.md` and paste it into `docs/DONE.md` under the matching section, with the `~~strikethrough~~` SHIPPED tag including the commit hash; remove any `§N` detail section from TODO.md.
7. **Commit** — run `./format_code.sh --check` once more. Provide a single `git add` command and a commit message. **Developer runs `git add`/`commit`/`push`.** Wait for confirmation.

## Code Review Workflow

Trigger phrases: "code review", "review my changes", "run the agents", "run the big three", "run all four", "run all five", "full review", "what do the agents think?".

1. Capture diff via `git status` + `git diff`.
2. Launch review agents in **parallel**:
   - **Big three** (code review / run the big three): `architecture-reviewer`, `embedded-efficiency-reviewer`, `security-auditor`.
   - **All four** (run all four): above + `ui-design-architect` (when UI changes present).
   - **All five** (full review / run all five): above + `coding-standards-auditor` — mandatory for large refactors, new modules, or pre-release audits.
3. Synthesize into a consolidated table with severity and action (fix / skip / ask). **Fix pre-existing issues when found** — triage on merit (severity + fix effort), not on when introduced.
4. Apply approved fixes; re-verify format and tests.
5. **Re-review substantial post-review changes.** Code written *after* the agent pass — fixes, or new work added during a redirect — was seen by no reviewer. Re-run the relevant lens on it before commit (this is separate from tweaking already-reviewed code). A bug introduced while applying feedback is invisible to the original pass; that is exactly how the reconnect-state bug on the music branch reached the PR and was caught only by the bots.

## Benchmark Methodology — `recall_reach` ≠ leader entailment

LoCoMo/LongMemEval/ConvoMem measure different things depending on flags. The bench prints a `LEADER-COMPARABLE: YES|NO` banner and tags `leader_comparable` in the results JSON. Honor it.

- **`recall_reach`** — top-K provenance overlap. Internal diagnostic. **NEVER** quote alongside ByteRover/MemMachine/Hindsight/Mem0 — they publish LLM-judge generation, not retrieval reach.
- **`recall_generation`** — generate-and-judge. **Leader-comparable IF the Mem0 protocol is matched.** Required flags: `--memory-pipeline --generator-provider openai --generator-model gpt-4o-mini --judge-provider openai --judge-model gpt-4o-mini --prompt-style mem0 --with-source --exclude-categories 5`.

When writing a number for an external audience (atlas, X post, paper, README), confirm `leader_comparable: true` in the run's results JSON or DON'T compare to leaders. See `benchmarks/README.md` for full methodology. Historical benchmark numbers live in [`atlas/dawn/memory/STATE.md`](https://github.com/The-OASIS-Project/atlas/tree/main/dawn/memory) — reference that file rather than re-listing numbers in TODO.md.

## Design Docs

- **Active planning**: @docs/TODO.md (master, untracked), plus per-feature docs in `docs/` (e.g., `PHONE_SMS_DESIGN.md`, `SPEAKER_IDENTIFICATION_PLAN.md`).
- **Shipped/completed archive**: `docs/DONE.md` (untracked, sibling to TODO.md). Migrated 2026-05-28 when TODO.md outgrew its own length. Section structure mirrors TODO.md so cross-reference back stays trivial.
- **Archived designs**: [atlas/dawn](https://github.com/The-OASIS-Project/atlas/tree/main/dawn) — shipped-feature design docs kept for historical reference. Memory subsystem under [`memory/`](https://github.com/The-OASIS-Project/atlas/tree/main/dawn/memory) (system design, injection filter, cat-2 temporal, reranker investigation, LoCoMo cat-3 profiling). Everything else flat under [`archive/`](https://github.com/The-OASIS-Project/atlas/tree/main/dawn/archive) — RAG, user auth, plan executor, scheduler, image search, CalDAV, email, etc.

## License

GPLv3 or later. Every new source file includes the GPL header block (see Code Standards above).

### Third-party attribution

Portions of the memory subsystem are adapted from [mem0ai/mem0](https://github.com/mem0ai/mem0) (Apache 2.0, © 2023 Taranjeet Singh) — see `NOTICE` and `DEPENDENCIES.md` (Architectural Influences). When you port or adapt code from Mem0, add a `/* Adapted from mem0ai/mem0 (Apache-2.0). See NOTICE. */` comment block at the point of use describing what was changed. Apache 2.0 §4 requires preserving the copyright notice and stating significant modifications — the per-file comment plus the central NOTICE/DEPENDENCIES entries satisfy both.
