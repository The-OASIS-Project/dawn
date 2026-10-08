# Contributing to DAWN

Thanks for your interest. DAWN is part of [The OASIS Project](https://github.com/The-OASIS-Project) and is
licensed under GPLv3 or later. This page covers how to build and test a change, the standards it has to
meet, and how a change gets into `main`.

Security problems go through [SECURITY.md](SECURITY.md), not a public issue.

## Build

Install the dependencies first: [GETTING_STARTED.md](GETTING_STARTED.md) for a Jetson or other embedded
board, [docs/GETTING_STARTED_SERVER.md](docs/GETTING_STARTED_SERVER.md) for an x86-64 server, and
[DEPENDENCIES.md](DEPENDENCIES.md) for the full list.

```bash
cmake --preset debug
make -C build-debug -j"$(nproc)"
```

The presets are in `CMakePresets.json`. `ci` builds without the WebUI, ONNX Runtime, CUDA or the Whisper
submodule, and is the quickest way to get the unit tests running on a stock Ubuntu machine.

## Before you commit

Install the git hooks once:

```bash
./install-git-hooks.sh
```

The pre-commit hook checks the formatting of what you stage and, when source or build files are staged,
builds the tests and runs the `ci` suite. The pre-push hook runs the suite again.

The order that keeps what you test identical to what you commit:

```bash
./format_code.sh --changed               # format first: it rewrites code
make -C build-debug -j"$(nproc)"         # the full build also runs the invariant checks
make -C build-debug tests-ci             # plain make doesn't rebuild the test programs
ctest --test-dir build-debug -L ci
./format_code.sh --check --changed
```

Build warnings are errors in CI, and the Docker build compiles at `-O2` with GCC 12, which reports some
warnings a debug build doesn't. A change should leave no new `warning:` lines in a local build.

[docs/TESTING.md](docs/TESTING.md) describes every test, sanitizer, analyzer and fuzzer, and how to run
each one locally.

## Standards

[CODING_STYLE_GUIDE.md](CODING_STYLE_GUIDE.md) is the full guide. The rules that differ from common C
practice:

- Functions return `SUCCESS` (0) or `FAILURE` (1), or a specific code above 1. No negative return codes.
- Every new `.c`, `.cpp` and `.h` file starts with the GPL header in the style guide.
- 3-space indent, 100-character lines, K&R braces; `clang-format` (version 14) enforces it.
- Logging uses the `OLOG_*` macros from `common/include/logging.h`.
- Files past 1,500 lines (C) or 1,000 (JavaScript) should be split before they grow; see
  [ARCHITECTURE.md](ARCHITECTURE.md#file-organization-standards).

Some changes have a guide of their own:

- **A new `dawn.toml` setting**: [docs/CONFIGURATION_GUIDE.md](docs/CONFIGURATION_GUIDE.md). A setting
  the TOML writer doesn't emit is silently deleted from the user's file on their next settings save.
- **A new LLM tool**: [docs/TOOL_DEVELOPMENT_GUIDE.md](docs/TOOL_DEVELOPMENT_GUIDE.md).
- **A change to the WebSocket protocol**: [docs/WEBSOCKET_PROTOCOL.md](docs/WEBSOCKET_PROTOCOL.md).
- **Anything that crosses subsystems**: the layering and lock-order rules in
  [ARCHITECTURE.md](ARCHITECTURE.md). Some of them are enforced at build time, and the build fails when
  one breaks.

Add or update tests with the change. Code that parses bytes from outside (mail, web pages, a server's
responses) should get a fuzz harness in `tests/fuzz/`.

## Commits and pull requests

- `main` is protected: changes arrive by pull request, CI must pass, and pull requests land as merge
  commits.
- One logical change per commit. The message says what changed and why, in plain language a reader
  without your context can follow.
- A change that affects someone updating an existing install (a changed default, a manual migration
  step, a user-visible behavior change) gets a dated entry at the top of [UPGRADING.md](UPGRADING.md).
- By contributing you agree to license your contribution under GPLv3 or later (the header in every
  source file says so too).

## How DAWN is developed

DAWN is developed with an AI coding assistant (Claude Code). [CLAUDE.md](CLAUDE.md) holds the rules it
works under: fix problems properly rather than patch around them, verify claims against the code, ask
before committing, never push. The maintainer reviews and approves every commit.

Before a change is committed it goes through a review step: `.claude/skills/review/SKILL.md` runs
specialist reviewers (architecture, efficiency, security, correctness, and for larger changes UI and
coding standards) over the diff, and each finding is checked against the code before it is acted on.
The reviewers' definitions are published in
[atlas/claude-agents](https://github.com/The-OASIS-Project/atlas/tree/main/claude-agents).
Everything else on this page applies to every change, however it was written: the hooks, the build-time
invariant checks and CI.
